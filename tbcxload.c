/* ==========================================================================
 * tbcxload.c — TBCX load+eval for Tcl 9.1
 *
 * Thread safety model:
 *
 *   All public entry points (Tbcx_LoadObjCmd, TbcxApplyShimPurgeAll)
 *   MUST be called from the thread that owns the Tcl_Interp.  This is
 *   the standard Tcl threading model — interpreters are not thread-safe.
 *   Debug builds enforce this via TBCX_ASSERT_INTERP_THREAD.
 *
 *   Per-interpreter state (ApplyShim, load depth, OO shim hidden-ID
 *   counter) is consolidated in TbcxInterpState, attached via
 *   Tcl_SetAssocData.  No mutex is needed for this state because
 *   each interpreter is used by exactly one thread.
 *
 *   Proc structs created by TBCX (in ReadProc, ReadMethod, ReadLit_LambdaBC,
 *   ProcShim_DirectInstall, CmdProcShim slow path, and top-level topProc)
 *   are per-interp and NEVER shared across interpreters or threads.  Their
 *   refCount fields are therefore safe to manipulate without a mutex.
 *
 *   FixLiteralPoolProcPtr propagates procPtr as a non-owning backpointer
 *   to nested bytecode literals in the same literal pool.  No refCount
 *   manipulation is needed because ByteCode.procPtr is a non-owning
 *   backpointer — TclReleaseByteCode does not decrement it.
 *
 *   Global state (tbcxTyBytecode, opcode cache, etc.) is initialized
 *   under tbcxTypeMutex with acquire/release fencing on tbcxTypesLoaded.
 *   After initialization, these are read-only and safe without locking.
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * File-local globals
 * ========================================================================== */

/* Stack-allocate argv for commands with this many words or fewer,
 * avoiding a per-call heap allocation in the common case. */
#define TBCX_ARGV_STACK 8

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct { /* minimal prefix used by TclInitByteCodeObj in our path */
    Tcl_Interp     *interp;
    Namespace      *nsPtr;
    unsigned char  *codeStart, *codeNext;
    Tcl_Obj       **objArrayPtr;
    Tcl_Size        numLitObjects;
    AuxData        *auxDataArrayPtr;
    Tcl_Size        numAuxDataItems;
    ExceptionRange *exceptArrayPtr;
    Tcl_Size        numExceptRanges;
    Tcl_Size        maxStackDepth;
    Proc           *procPtr;
} TBCX_CompileEnvMin;

typedef struct {
    Tcl_HashTable      procsByFqn;       /* key: FQN const char* (TCL_STRING_KEYS), val: Tcl_Obj* pair {args, procbody} */
    Tcl_Obj          **procsByIdx;       /* indexed array [0..numProcsIdx-1] for marker lookup */
    uint32_t           numProcsIdx;      /* size of procsByIdx array */
    Command           *procCmdPtr;       /* the "proc" Command struct (NULL if invalidated) */
    Tcl_Interp        *interp;           /* owning interpreter (for trace removal) */
    int                traceInstalled;   /* 1 if command trace is active on "proc" */
    Tcl_ObjCmdProc2   *savedObjProc2;    /* saved objProc2 handler */
    Tcl_ObjCmdProc2   *savedNreProc2;    /* saved nreProc2 handler (may be NULL) */
    void              *savedClientData2; /* saved objClientData2 */
    /* handler pointers captured from first successful proc creation,
       used for direct registration of subsequent procs. */
    Tcl_ObjCmdProc2   *procDispatchObj; /* objProc2 on created proc Command */
    Tcl_ObjCmdProc2   *procDispatchNre; /* nreProc2 on created proc Command */
    Tcl_CmdDeleteProc *procDeleteProc;  /* deleteProc on created proc Command */
    int                haveDispatch;    /* 1 once pointers captured */
} ProcShim;

typedef struct {
    Tcl_HashTable    methodsByKey;         /* key: STRING "class\x1Fkind\x1Fname", val: Tcl_Obj* PAIR {args, procbody} */
    Command         *defineCmdPtr;         /* cached oo::define Command (NULL if invalidated by rename/delete) */
    Command         *objdefCmdPtr;         /* cached oo::objdefine Command (NULL if invalidated) */
    int              defineTraceInstalled; /* 1 if command trace is active on oo::define */
    int              objdefTraceInstalled; /* 1 if command trace is active on oo::objdefine */
    Tcl_ObjCmdProc2 *savedDefineProc;
    Tcl_ObjCmdProc2 *savedDefineNre;
    void            *savedDefineCD;
    Tcl_ObjCmdProc2 *savedObjdefProc;
    Tcl_ObjCmdProc2 *savedObjdefNre;
    void            *savedObjdefCD;
    int              hasObjDefine; /* 1 if oo::objdefine was successfully shimmed */
} OOShim;

/* ApplyShim — persistent interceptor on the [apply] command.
 * Survives beyond LoadTbcxStream (lambdas may be called later from procs).
 * Attached to the interpreter via Tcl_SetAssocData so it is cleaned up
 * when the interpreter is deleted.
 *
 * Purpose: when a precompiled lambda's lambdaExpr internal rep gets
 * evicted by shimmer, the shim detects the missing rep and re-installs
 * the precompiled Proc* from its registry before forwarding to Tcl's
 * real [apply].  This eliminates the need to store body source text
 * in .tbcx files. */
#define TBCX_INTERP_STATE_KEY "tbcx::interpState"

typedef struct {
    Proc    *procPtr; /* precompiled Proc (we hold a refcount) */
    Tcl_Obj *nsObj;   /* namespace for lambdaExpr internal rep */
} ApplyLambdaEntry;

typedef struct {
    Command         *applyCmdPtr;    /* cached "apply" Command (NULL if invalidated) */
    Tcl_Interp      *interp;         /* owning interpreter (for trace removal) */
    int              traceInstalled; /* 1 if command trace is active on "apply" */
    Tcl_ObjCmdProc2 *savedApplyProc;
    Tcl_ObjCmdProc2 *savedApplyNre; /* saved nreProc2 handler (may be NULL) */
    void            *savedApplyCD;
    Tcl_HashTable    lambdaRegistry; /* key: ONE_WORD (Tcl_Obj *), val: ApplyLambdaEntry* */
    Tcl_Size         numRegistered;  /* Count of registered lambdas.
                                        When 0, CmdApplyShim bypasses hash lookup
                                        and forwards directly to savedApplyProc. */
} ApplyShim;

/* TbcxInterpState — consolidated per-interpreter state.
 * Attached via Tcl_SetAssocData under TBCX_INTERP_STATE_KEY.
 * Created lazily on first tbcx::load; destroyed when the interpreter
 * is deleted. */
typedef struct {
    Tcl_Interp *interp;
    ApplyShim   apply;        /* embedded — no separate heap allocation */
    int         applyActive;  /* 1 once the [apply] shim has been installed */
    Tcl_Size    loadDepth;    /* reentrancy depth for tbcx::load */
    uint64_t    nextHiddenId; /* per-interp OO shim rename counter */
} TbcxInterpState;

typedef struct {
    Var        *oldLocals;
    Tcl_Size    oldNum;
    LocalCache *oldCache;
    Var        *allocated;
    /* Frame pointer captured at TopLocals_Begin so TopLocals_End can
     * restore the exact same frame.  Begin chooses varFramePtr (the
     * active frame at eval time), but between Begin and End the interp
     * may push/pop frames.  Recording the pointer here avoids using
     * whichever frame is active at End time. */
    CallFrame  *frame;
} TbcxTopFrameSave;

/* Runaway detection limits */
#define TBCX_MAX_LITERAL_DEPTH 64
#define TBCX_MAX_CONTAINER_ELEMS (1u * 1024u * 1024u)

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static int         AddOOShim(Tcl_Interp *ip, OOShim *os);
static int         AddProcShim(Tcl_Interp *ip, ProcShim *ps);
static void        ApplyCmdDeleteTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags);
static void        ApplyShimTeardown(ApplyShim *as, Tcl_Interp *ip);
static void        ApplyShimPurgeStale(ApplyShim *as);
static Tcl_Obj    *ByteCodeObj(Tcl_Interp *ip, Namespace *nsPtr, const unsigned char *code, uint32_t codeLen, Tcl_Obj **lits, uint32_t numLits, AuxData *auxArr, uint32_t numAux, ExceptionRange *exArr,
                               uint32_t numEx, int maxStackDepth, int setPrecompiled);
static int         CmdApplyShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static int         CmdApplyShimNre(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static int         CmdOOShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static int         CmdOOShimObjDef(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static int         CmdProcShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static void        CompiledLocals(Proc *procPtr, Tcl_Size neededCount);
static int         DefOO(Tcl_Interp *ip, OOShim *os, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *nameOpt, Tcl_Obj *argsOpt, Tcl_Obj *bodyOpt);
static void        DelOOShim(Tcl_Interp *ip, OOShim *os);
static void        DelProcShim(Tcl_Interp *ip, ProcShim *ps);
static ApplyShim  *EnsureApplyShim(Tcl_Interp *ip);
static void        FixCompiledLocalNames(Proc *procPtr, LocalCache *lc);
static int         LoadTbcxStream(Tcl_Interp *ip, Tcl_Channel ch, Tcl_Obj *scriptFilePath);
static int         MethodKeyBuf(Tcl_DString *ds, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *name);
static void        OOShimDefineCmdTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags);
static void        OOShimObjdefCmdTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags);
static void        OOShim_IdentifyMethod(const char *subc, Tcl_Size objc, Tcl_Obj *const objv[], Tcl_Obj *clsFqn, Tcl_DString *keyDs, uint8_t *kindOut, Tcl_Size *bodyIdxOut, Tcl_Obj **runtimeArgsOut,
                                         Tcl_Obj **nameOOut, Tcl_Obj **tmpEmptyArgsOut, int *hasKeyOut);
static void        OOShim_LookupPair(Tcl_Interp *ip, OOShim *os, Tcl_DString *keyDs, int hasKey, Tcl_Size bodyIdx, Tcl_Obj *runtimeArgs, Tcl_Obj **savedArgsOut, Tcl_Obj **preBodyOut);
static int         PrecompClass(Tcl_Interp *ip, OOShim *os, Tcl_Obj *clsFqn);
static void        ProcCmdDeleteTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags);
static int         ProcShim_DirectInstall(ProcShim *ps, Tcl_Interp *ip, Tcl_Obj *fqn, Tcl_Obj *nameObj, Proc *preProc, Tcl_Obj *savedArgs);
static inline void R_Error(TbcxIn *r, const char *msg);
static int         ReadAuxArray(TbcxIn *r, AuxData **auxOut, uint32_t *numAuxOut);
static int         ReadExceptions(TbcxIn *r, ExceptionRange **exOut, uint32_t *numOut);
static Tcl_Obj    *ReadLit_Bignum(TbcxIn *r);
static Tcl_Obj    *ReadLit_LambdaBC(TbcxIn *r, Tcl_Interp *ip, int depth, int dumpOnly);
static Tcl_Obj    *ReadLiteral(TbcxIn *r, Tcl_Interp *ip, int depth, int dumpOnly);
static int         ReadMethod(TbcxIn *r, Tcl_Interp *ip, OOShim *os);
static int         ReadProc(TbcxIn *r, Tcl_Interp *ip, ProcShim *shim, uint32_t procIdx);
static inline void RefreshBC(ByteCode *bcPtr, Tcl_Interp *ip, Namespace *nsPtr);
static void        FixLiteralPoolProcPtr(ByteCode *bcPtr);
static void        NullLiteralPoolProcPtr(ByteCode *bcPtr, Proc *target);
static void        RegisterPrecompiledLambda(Tcl_Interp *ip, Tcl_Obj *lambda, Proc *procPtr, Tcl_Obj *nsObj);
Tcl_Namespace     *Tbcx_EnsureNamespace(Tcl_Interp *ip, const char *fqn);
int                Tbcx_LoadObjCmd(TCL_UNUSED(void *), Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
inline int         Tbcx_R_Bytes(TbcxIn *r, void *p, Tcl_Size n);
void               Tbcx_R_Init(TbcxIn *r, Tcl_Interp *ip, Tcl_Channel ch);
inline int         Tbcx_R_LPString(TbcxIn *r, char **sp, uint32_t *lenp);
inline int         Tbcx_R_U32(TbcxIn *r, uint32_t *vp);
inline int         Tbcx_R_U64(TbcxIn *r, uint64_t *vp);
inline int         Tbcx_R_U8(TbcxIn *r, uint8_t *v);
Tcl_Obj           *Tbcx_ReadBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut, int setPrecompiled, int dumpOnly);
int                Tbcx_ReadHeader(TbcxIn *r, TbcxHeader *H);
void               TbcxApplyShimPurgeAll(Tcl_Interp *ip);
static ByteCode   *TbcxByteCode(Tcl_Obj *objPtr, const Tcl_ObjType *typePtr, const TBCX_CompileEnvMin *env, int setPrecompiled);
static void        TbcxFixLocalCacheExtras(ByteCode *bcPtr, Proc *procPtr);
static TbcxInterpState *TbcxGetInterpState(Tcl_Interp *ip);
static void             TbcxInterpStateCleanup(void *cd, Tcl_Interp *ip);
static void             TopLocals_Begin(Tcl_Interp *ip, ByteCode *bcPtr, TbcxTopFrameSave *sv);
static void             TopLocals_End(Tcl_Interp *ip, TbcxTopFrameSave *sv);

/* ==========================================================================
 * Buffered Read I/O & Utilities
 * ========================================================================== */

static inline void      R_Error(TbcxIn *r, const char *msg) {
    if (r->err == TCL_OK) {
        Tcl_SetObjResult(r->interp, Tcl_NewStringObj(msg, -1));
        r->err = TCL_ERROR;
    }
}

void Tbcx_R_Init(TbcxIn *r, Tcl_Interp *ip, Tcl_Channel ch) {
    r->interp  = ip;
    r->chan    = ch;
    r->err     = TCL_OK;
    r->bufPos  = 0;
    r->bufFill = 0;
}

inline int Tbcx_R_Bytes(TbcxIn *r, void *p, Tcl_Size n) {
    if (r->err)
        return 0;
    if (n == 0)
        return 1;
    unsigned char *dst = (unsigned char *)p;
    Tcl_Size       rem = n;
    while (rem > 0) {
        /* Serve from buffer first */
        Tcl_Size avail = r->bufFill - r->bufPos;
        if (avail > 0) {
            Tcl_Size chunk = (rem < avail) ? rem : avail;
            memcpy(dst, r->buf + r->bufPos, (size_t)chunk);
            r->bufPos += chunk;
            dst += chunk;
            rem -= chunk;
            continue;
        }
        /* Buffer empty — refill */
        Tcl_Size got = Tcl_ReadRaw(r->chan, (char *)r->buf, (Tcl_Size)TBCX_BUFSIZE);
        if (got < 0) {
            R_Error(r, "tbcx: I/O error during read");
            return 0;
        }
        if (got == 0) {
            R_Error(r, "tbcx: unexpected EOF (short read)");
            return 0;
        }
        r->bufPos  = 0;
        r->bufFill = got;
    }
    return 1;
}

inline int Tbcx_R_U8(TbcxIn *r, uint8_t *v) {
    return Tbcx_R_Bytes(r, v, 1);
}

inline int Tbcx_R_U32(TbcxIn *r, uint32_t *vp) {
    uint32_t v = 0;
    if (!Tbcx_R_Bytes(r, &v, 4))
        return 0;
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed)) {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    *vp = v;
    return 1;
}

inline int Tbcx_R_U64(TbcxIn *r, uint64_t *vp) {
    uint64_t v = 0;
    if (!Tbcx_R_Bytes(r, &v, 8))
        return 0;
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed)) {
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) |
            ((v & 0x0000FF0000000000ull) >> 24) | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }
    *vp = v;
    return 1;
}

inline int Tbcx_R_LPString(TbcxIn *r, char **sp, uint32_t *lenp) {
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_STR) {
        R_Error(r, "tbcx: LPString too large");
        return 0;
    }
    char *buf = (char *)Tcl_AttemptAlloc(n + 1u);
    if (!buf) {
        R_Error(r, "tbcx: allocation failed (LPString)");
        return 0;
    }
    if (n && !Tbcx_R_Bytes(r, buf, n)) {
        Tcl_Free(buf);
        return 0;
    }
    buf[n] = '\0';
    *sp    = buf;
    *lenp  = n;
    return 1;
}

static inline void RefreshBC(ByteCode *bcPtr, Tcl_Interp *ip, Namespace *nsPtr) {
    if (!bcPtr)
        return;
    Interp *iPtr = (Interp *)ip;
    /* Release the previous interpHandle (if any) before preserving the new
       one.  Without this, stale handles can accumulate and -- in nested
       child interpreters -- reference freed interp structures. */
    if (bcPtr->interpHandle)
        TclHandleRelease(bcPtr->interpHandle);
    bcPtr->interpHandle = TclHandlePreserve(iPtr->handle);
    bcPtr->compileEpoch = iPtr->compileEpoch;
    bcPtr->nsPtr        = nsPtr;
    bcPtr->nsEpoch      = nsPtr ? nsPtr->resolverEpoch : 0;
}

/* FixLiteralPoolProcPtr — propagate a ByteCode's procPtr to all bytecode
 * literals in its literal pool that currently have procPtr=NULL.
 *
 * The saver's P1/P2 optimizations precompile body literals (namespace eval,
 * while, for, foreach, try, if bodies) as separate TBCX_LIT_BYTESRC objects
 * in the literal pool.  These inner bytecodes are created with procPtr=NULL
 * because their enclosing proc context is not known at literal-read time.
 *
 * When the bytecode engine (TEBCresume) evaluates one of these inner bodies
 * via an NRE yield/resume path (e.g. coroutine yield inside a while loop),
 * it dereferences codePtr->procPtr->firstLocalPtr without a NULL guard.
 * If procPtr is NULL, this crashes at address 0x30 (offset of firstLocalPtr
 * in the Proc struct on 64-bit).
 *
 * This function propagates the parent ByteCode's procPtr to all
 * NULL-procPtr bytecode literals, matching the semantic that these
 * body literals execute in the enclosing proc's context.
 *
 * ByteCode.procPtr is a non-owning backpointer — TclReleaseByteCode
 * does not decrement it.  Proc lifetime is managed by the procbody/
 * lambda internal rep via TclProcCleanupProc.  The inner bytecode
 * literals live in the parent's literal pool and are freed when the
 * parent ByteCode is freed, so the backpointers cannot outlive the
 * Proc they point to.  No refCount increment is needed. */
static void FixLiteralPoolProcPtr(ByteCode *bcPtr) {
    if (!bcPtr || !bcPtr->procPtr || bcPtr->numLitObjects <= 0 || !bcPtr->objArrayPtr)
        return;
    assert(bcPtr->procPtr->iPtr != NULL && "FixLiteralPoolProcPtr: procPtr->iPtr must be non-NULL");
    Tcl_Obj **litObjv = bcPtr->objArrayPtr;
    for (Tcl_Size i = 0; i < bcPtr->numLitObjects; i++) {
        if (!litObjv[i])
            continue;
        ByteCode *litBC = TbcxGetByteCode(litObjv[i]);
        if (litBC && !litBC->procPtr) {
            litBC->procPtr = bcPtr->procPtr; /* non-owning backpointer */
            FixLiteralPoolProcPtr(litBC);
        }
    }
}

/* NullLiteralPoolProcPtr — recursively null out non-owning procPtr
 * backpointers that match `target` in a bytecode's literal pool.
 *
 * This is the cleanup counterpart to FixLiteralPoolProcPtr.  Before
 * freeing a Proc that was propagated into nested bytecode literals,
 * call this to eliminate all dangling backpointers — not just the
 * immediate children but the full recursive tree. */
static void NullLiteralPoolProcPtr(ByteCode *bcPtr, Proc *target) {
    if (!bcPtr || !target || bcPtr->numLitObjects <= 0 || !bcPtr->objArrayPtr)
        return;
    Tcl_Obj **litObjv = bcPtr->objArrayPtr;
    for (Tcl_Size i = 0; i < bcPtr->numLitObjects; i++) {
        if (!litObjv[i])
            continue;
        ByteCode *litBC = TbcxGetByteCode(litObjv[i]);
        if (litBC && litBC->procPtr == target) {
            litBC->procPtr = NULL;
            NullLiteralPoolProcPtr(litBC, target);
        }
    }
}

/* TbcxFixLocalCacheExtras — Reallocate a TBCX-built LocalCache to include
 * the "extras" area that Tcl 9.1's InitArgsAndLocals expects.
 *
 * Tcl 9.1's InitLocalCache (tclProc.c) allocates:
 * offsetof(LocalCache, varName0) + numVars*sizeof(Obj*) + numArgs*sizeof(Var)
 * The trailing numArgs Var structures hold per-argument flags (VAR_IS_ARGS)
 * and default values (defValuePtr).  TBCX's Tbcx_ReadBlock only allocates
 * header+names — no extras area.  When Tcl finds a non-NULL localCachePtr
 * it uses it directly, reading the extras past our short allocation.
 *
 * This function preserves the variable names from the TBCX cache (which
 * include names for locals beyond the declared arguments, such as those
 * introduced by [my variable]) while adding the extras area populated
 * from the Proc's CompiledLocal chain.
 *
 * For procs loaded via CmdProcShim, the cache is NULLed out instead
 * (because TclCreateProc builds a complete CompiledLocal chain with all
 * names, so InitLocalCache can rebuild correctly).  But for methods and
 * lambdas, TBCX builds the CompiledLocal chain with only argument entries
 * + unnamed padding, so NULLing loses the non-argument local names. */
static void TbcxFixLocalCacheExtras(ByteCode *bcPtr, Proc *procPtr) {
    if (!bcPtr || !procPtr || !bcPtr->localCachePtr)
        return;

    LocalCache *old     = bcPtr->localCachePtr;
    Tcl_Size    numVars = old->numVars;
    Tcl_Size    numArgs = procPtr->numArgs;

    size_t      nameBytes, extraBytes, bytes;
    if (!tbcx_checked_mul((size_t)numVars, sizeof(Tcl_Obj *), &nameBytes) || !tbcx_checked_mul((size_t)numArgs, sizeof(Var), &extraBytes))
        return; /* overflow — skip cache fix; Tcl rebuilds on first call */
    bytes = offsetof(LocalCache, varName0);
    if (nameBytes > SIZE_MAX - bytes)
        return;
    bytes += nameBytes;
    if (extraBytes > SIZE_MAX - bytes)
        return;
    bytes += extraBytes;
    LocalCache *lc = (LocalCache *)Tcl_Alloc(bytes);
    memset(lc, 0, bytes);
    lc->refCount       = 1;
    lc->numVars        = numVars;

    /* Copy variable names from the old (TBCX) cache */
    Tcl_Obj **oldNames = (Tcl_Obj **)&old->varName0;
    Tcl_Obj **newNames = (Tcl_Obj **)&lc->varName0;
    for (Tcl_Size i = 0; i < numVars; i++) {
        newNames[i] = oldNames[i];
        if (newNames[i])
            Tcl_IncrRefCount(newNames[i]);
    }

    /* Populate the extras area from the Proc's CompiledLocal chain.
       Each argument gets: flags (VAR_IS_ARGS bit) and defValuePtr. */
    if (numArgs > 0) {
        Var           *varPtr = (Var *)(newNames + numVars);
        CompiledLocal *cl     = procPtr->firstLocalPtr;
        for (Tcl_Size a = 0; a < numArgs && cl; a++, cl = cl->nextPtr) {
            varPtr[a].flags        = (cl->flags & VAR_IS_ARGS);
            varPtr[a].value.objPtr = cl->defValuePtr;
        }
    }

    /* Release old cache and install new one */
    if (--old->refCount <= 0) {
        for (Tcl_Size i = 0; i < old->numVars; i++) {
            if (oldNames[i])
                Tcl_DecrRefCount(oldNames[i]);
        }
        Tcl_Free(old);
    }
    bcPtr->localCachePtr = lc;
}

/* MethodKeyBuf — Build a hash key string "classFqn\x1Fkind\x1Fname" into a
 * caller-provided Tcl_DString.  This avoids allocating a Tcl_Obj + refcount
 * management for what is purely a transient lookup key.  Caller must call
 * Tcl_DStringFree(ds) when done.
 * Returns 1 on success, 0 if a required string object is invalid. */
static int MethodKeyBuf(Tcl_DString *ds, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *name) {
    Tcl_DStringInit(ds);
    Tcl_Size    fqnLen = 0;
    const char *fqn    = Tbcx_GetStringFromObjStrict(NULL, clsFqn, &fqnLen);
    if (!fqn)
        return 0;
    Tcl_DStringAppend(ds, fqn, fqnLen);
    Tcl_DStringAppend(ds, "\x1F", 1);
    {
        /* kind is always 0–4 (TBCX_METH_*); format as a single ASCII digit
         * without a Tcl_Obj round-trip.  Falls back to two digits for values
         * up to 99, which covers any future extension. */
        char kindBuf[4];
        int  kindLen;
        if (kind < 10) {
            kindBuf[0] = '0' + (char)kind;
            kindLen    = 1;
        } else {
            kindBuf[0] = '0' + (char)(kind / 10);
            kindBuf[1] = '0' + (char)(kind % 10);
            kindLen    = 2;
        }
        Tcl_DStringAppend(ds, kindBuf, kindLen);
    }
    Tcl_DStringAppend(ds, "\x1F", 1);
    if (name) {
        Tcl_Size    nLen = 0;
        const char *n    = Tbcx_GetStringFromObjStrict(NULL, name, &nLen);
        if (!n) {
            Tcl_DStringFree(ds);
            return 0;
        }
        Tcl_DStringAppend(ds, n, nLen);
    }
    return 1;
}

static int DefOO(Tcl_Interp *ip, OOShim *os, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *nameOpt, Tcl_Obj *argsOpt, Tcl_Obj *bodyOpt) {
    int      rc    = TCL_OK;
    /* argv0 is only used for error messages in the original handler */
    Tcl_Obj *argv0 = Tcl_NewStringObj("oo::define", -1);
    Tcl_IncrRefCount(argv0);

    /* Fast path for methods (instance/class): always direct */
    if (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS) {
        Tcl_Obj *sub = Tcl_NewStringObj((kind == TBCX_METH_CLASS) ? "classmethod" : "method", -1);
        Tcl_IncrRefCount(sub);
        Tcl_IncrRefCount(nameOpt);
        Tcl_IncrRefCount(argsOpt);
        Tcl_IncrRefCount(bodyOpt);
        Tcl_Obj *av[6] = {argv0, clsFqn, sub, nameOpt, argsOpt, bodyOpt};
        rc             = os->savedDefineProc(os->savedDefineCD, ip, 6, av);
        Tcl_DecrRefCount(bodyOpt);
        Tcl_DecrRefCount(argsOpt);
        Tcl_DecrRefCount(nameOpt);
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Self methods: create-then-swap via oo::define CLS self method.
       Direct procbody substitution fails because TclOO's internal handling
       of self methods (same path as oo::objdefine) doesn't survive method
       chain rebuilds triggered by subclass "superclass" declarations.
       Instead: (1) create with ";" placeholder body, (2) swap bodyPtr. */
    if (kind == TBCX_METH_SELF) {
        const Tcl_ObjInternalRep *pbIR           = Tcl_FetchInternalRep(bodyOpt, tbcxTyProcBody);
        Proc                     *preProc        = pbIR ? (Proc *)pbIR->twoPtrValue.ptr1 : NULL;

        /* Save preProc fields BEFORE step 1 — the procbody's Proc may be
           corrupted during oo::define handler execution (observed:
           numCompiledLocals becomes garbage after step 1). */
        Tcl_Obj                  *savedBody      = preProc ? preProc->bodyPtr : NULL;
        Tcl_Size                  savedNumLocals = preProc ? preProc->numCompiledLocals : 0;
        if (savedBody)
            Tcl_IncrRefCount(savedBody);

        /* Step 1: Create self method with ";" placeholder */
        Tcl_Obj *selfSub = Tcl_NewStringObj("self", -1);
        Tcl_IncrRefCount(selfSub);
        Tcl_Obj *methSub = Tcl_NewStringObj("method", -1);
        Tcl_IncrRefCount(methSub);
        Tcl_IncrRefCount(nameOpt);
        Tcl_IncrRefCount(argsOpt);
        Tcl_Obj *placeholder = Tcl_NewStringObj(";", 1);
        Tcl_IncrRefCount(placeholder);
        Tcl_Obj *av[7] = {argv0, clsFqn, selfSub, methSub, nameOpt, argsOpt, placeholder};
        rc             = os->savedDefineProc(os->savedDefineCD, ip, 7, av);
        Tcl_DecrRefCount(placeholder);
        Tcl_DecrRefCount(argsOpt);
        Tcl_DecrRefCount(nameOpt);
        Tcl_DecrRefCount(methSub);
        Tcl_DecrRefCount(selfSub);

        /* Step 2: Swap Proc bodyPtr with precompiled bytecode.
           Use savedBody/savedNumLocals instead of reading from preProc
           (which may have been corrupted during step 1). */
        if (rc == TCL_OK && savedBody) {
            Tcl_Object tclObj = Tcl_GetObjectFromObj(ip, clsFqn);
            if (tclObj) {
                Object *oPtr = (Object *)tclObj;
                if (oPtr->methodsPtr) {
                    Tcl_Obj *mNameObj = Tcl_NewStringObj(Tbcx_GetStringSafe(nameOpt), -1);
                    Tcl_IncrRefCount(mNameObj);
                    Tcl_HashEntry *he = Tcl_FindHashEntry(oPtr->methodsPtr, (const char *)mNameObj);
                    Tcl_DecrRefCount(mNameObj);
                    if (he) {
                        Method *meth = (Method *)Tcl_GetHashValue(he);
                        if (meth && meth->clientData) {
                            ProcedureMethod *pmPtr = (ProcedureMethod *)meth->clientData;
                            if (pmPtr && pmPtr->procPtr) {
                                Proc *selfProc = pmPtr->procPtr;
                                Tcl_IncrRefCount(savedBody);
                                if (selfProc->bodyPtr)
                                    Tcl_DecrRefCount(selfProc->bodyPtr);
                                selfProc->bodyPtr = savedBody;
                                CompiledLocals(selfProc, savedNumLocals);
                                {
                                    ByteCode *bc = TbcxGetByteCode(savedBody);
                                    if (bc) {
                                        Namespace *nsPtr = (Namespace *)oPtr->namespacePtr;
                                        TbcxFixupByteCode(bc, selfProc, ip, nsPtr, TBCX_FIXUP_CACHE_KEEP);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (savedBody)
            Tcl_DecrRefCount(savedBody);

        Tcl_DecrRefCount(argv0);
        return rc;
    }

    int hasPrecompiled = (bodyOpt && bodyOpt->typePtr == tbcxTyProcBody) ? 1 : 0;
    if ((kind == TBCX_METH_CTOR || kind == TBCX_METH_DTOR) && hasPrecompiled) {
        /* Extract precompiled Proc from the procbody object */
        const Tcl_ObjInternalRep *pbIR            = Tcl_FetchInternalRep(bodyOpt, tbcxTyProcBody);
        Proc                     *preProc         = pbIR ? (Proc *)pbIR->twoPtrValue.ptr1 : NULL;

        /* Step 1: Create ctor/dtor with empty body via the saved original
           oo::define.  TclOO builds the full Method/Proc/dispatch structure,
           so 'next' etc. work in the correct context. */
        /* CRITICAL: The placeholder body MUST be non-empty.  TclOO's
           TclOODefineConstructorObjCmd (and destructor equivalent) checks
           the body length: if it's 0, it DELETES the ctor/dtor entirely
           (sets constructorPtr/destructorPtr to NULL) instead of creating
           a Method with a Proc.  A semicolon is the minimal no-op body
           that produces a real Method we can swap the bytecode into. */
        Tcl_Obj                  *placeholderBody = Tcl_NewStringObj(";", 1);
        Tcl_IncrRefCount(placeholderBody);

        if (kind == TBCX_METH_CTOR) {
            Tcl_Obj *sub = Tcl_NewStringObj("constructor", -1);
            Tcl_IncrRefCount(sub);
            Tcl_IncrRefCount(argsOpt);
            Tcl_Obj *cav[5] = {argv0, clsFqn, sub, argsOpt, placeholderBody};
            rc              = os->savedDefineProc(os->savedDefineCD, ip, 5, cav);
            Tcl_DecrRefCount(argsOpt);
            Tcl_DecrRefCount(sub);
        } else /* TBCX_METH_DTOR */
        {
            Tcl_Obj *sub = Tcl_NewStringObj("destructor", -1);
            Tcl_IncrRefCount(sub);
            Tcl_Obj *dav[4] = {argv0, clsFqn, sub, placeholderBody};
            rc              = os->savedDefineProc(os->savedDefineCD, ip, 4, dav);
            Tcl_DecrRefCount(sub);
        }
        Tcl_DecrRefCount(placeholderBody);

        if (rc != TCL_OK) {
            Tcl_DecrRefCount(argv0);
            return rc;
        }

        /* Step 2: Locate the Proc that TclOO just created and swap its
           bodyPtr with the precompiled bytecode.  This preserves the full
           TclOO constructor/destructor dispatch chain. */
        if (preProc && preProc->bodyPtr) {
            Tcl_Object tclObj = Tcl_GetObjectFromObj(ip, clsFqn);
            Class     *clsPtr = tclObj ? (Class *)Tcl_GetObjectAsClass(tclObj) : NULL;
            if (clsPtr) {
                Method *meth = (kind == TBCX_METH_CTOR) ? clsPtr->constructorPtr : clsPtr->destructorPtr;
                if (meth && meth->clientData) {
                    ProcedureMethod *pmPtr = (ProcedureMethod *)meth->clientData;
                    if (pmPtr && pmPtr->procPtr) {
                        Proc    *ctorProc = pmPtr->procPtr;

                        /* Swap bodyPtr: drop the empty-body bytecode,
                           install the precompiled one. */
                        Tcl_Obj *preBody  = preProc->bodyPtr;
                        Tcl_IncrRefCount(preBody);
                        if (ctorProc->bodyPtr)
                            Tcl_DecrRefCount(ctorProc->bodyPtr);
                        ctorProc->bodyPtr = preBody;

                        /* Extend compiled locals for body-internal variables
                           beyond the declared arguments. */
                        CompiledLocals(ctorProc, preProc->numCompiledLocals);

                        /* Fix bytecode linkage: point bc→procPtr at the
                           real TclOO Proc and refresh epochs. */
                        ByteCode *bc = TbcxGetByteCode(preBody);
                        if (bc) {
                            Object    *oPtr  = (Object *)tclObj;
                            Namespace *nsPtr = (Namespace *)oPtr->namespacePtr;
                            TbcxFixupByteCode(bc, ctorProc, ip, nsPtr, TBCX_FIXUP_CACHE_KEEP);
                        }
                    }
                }
            }
        }
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Fallback ctor/dtor (non-precompiled body): forward directly */
    if (kind == TBCX_METH_CTOR) {
        Tcl_Obj *sub = Tcl_NewStringObj("constructor", -1);
        Tcl_IncrRefCount(sub);
        Tcl_IncrRefCount(argsOpt);
        Tcl_IncrRefCount(bodyOpt);
        Tcl_Obj *av[5] = {argv0, clsFqn, sub, argsOpt, bodyOpt};
        rc             = os->savedDefineProc(os->savedDefineCD, ip, 5, av);
        Tcl_DecrRefCount(bodyOpt);
        Tcl_DecrRefCount(argsOpt);
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Destructors: use short form (no args) or long form based on argsOpt. */
    { /* destructor */
        Tcl_Obj *sub = Tcl_NewStringObj("destructor", -1);
        Tcl_IncrRefCount(sub);
        /* Choose 4-arg or 5-arg form based on argsOpt emptiness */
        int useShort = 1;
        if (argsOpt) {
            Tcl_Size L      = -1;
            int      listRc = Tcl_ListObjLength(ip, argsOpt, &L);
            if (listRc != TCL_OK) {
                /* Treat failed list canonicalization as "unknown" and keep
                   the short destructor form, but do not leak a stale interp
                   result into the surrounding evaluation. */
                Tcl_ResetResult(ip);
            } else if (L > 0) {
                useShort = 0;
            }
        }
        if (useShort) {
            Tcl_IncrRefCount(bodyOpt);
            Tcl_Obj *dv[4] = {argv0, clsFqn, sub, bodyOpt};
            rc             = os->savedDefineProc(os->savedDefineCD, ip, 4, dv);
            Tcl_DecrRefCount(bodyOpt);
        } else {
            Tcl_IncrRefCount(argsOpt);
            Tcl_IncrRefCount(bodyOpt);
            Tcl_Obj *dv[5] = {argv0, clsFqn, sub, argsOpt, bodyOpt};
            rc             = os->savedDefineProc(os->savedDefineCD, ip, 5, dv);
            Tcl_DecrRefCount(bodyOpt);
            Tcl_DecrRefCount(argsOpt);
        }
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }
}

static int PrecompClass(Tcl_Interp *ip, OOShim *os, Tcl_Obj *clsFqn) {
    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    int            rc             = TCL_OK;
    Tcl_Size       fqnLen         = 0;
    const char    *fqn            = Tbcx_GetStringFromObjSafe(clsFqn, &fqnLen);

    /* Save the list of unexported (non-public) methods BEFORE swapping
       bodies.  DefOO re-creates methods via oo::define which resets flags
       to PUBLIC, losing any prior "unexport" from the builder body. */
    Tcl_Obj       *unexportedList = NULL;
    {
        /* Get all methods (including private) */
        Tcl_Obj *allCmd[5];
        allCmd[0] = Tcl_NewStringObj("info", -1);
        allCmd[1] = Tcl_NewStringObj("class", -1);
        allCmd[2] = Tcl_NewStringObj("methods", -1);
        allCmd[3] = clsFqn;
        allCmd[4] = Tcl_NewStringObj("-private", -1);
        for (int ci = 0; ci < 5; ci++)
            Tcl_IncrRefCount(allCmd[ci]);
        int      allRc      = Tcl_EvalObjv(ip, 5, allCmd, 0);
        Tcl_Obj *allMethods = (allRc == TCL_OK) ? Tcl_DuplicateObj(Tcl_GetObjResult(ip)) : NULL;
        if (allMethods)
            Tcl_IncrRefCount(allMethods);
        Tcl_ResetResult(ip);

        /* Get public methods only */
        Tcl_Obj *pubCmd[4];
        pubCmd[0]           = allCmd[0];
        pubCmd[1]           = allCmd[1];
        pubCmd[2]           = allCmd[2];
        pubCmd[3]           = clsFqn;
        int      pubRc      = Tcl_EvalObjv(ip, 4, pubCmd, 0);
        Tcl_Obj *pubMethods = (pubRc == TCL_OK) ? Tcl_DuplicateObj(Tcl_GetObjResult(ip)) : NULL;
        if (pubMethods)
            Tcl_IncrRefCount(pubMethods);
        Tcl_ResetResult(ip);

        for (int ci = 0; ci < 5; ci++)
            Tcl_DecrRefCount(allCmd[ci]);

        /* Compute unexported = all - public */
        if (allMethods && pubMethods) {
            Tcl_Size  allLen = 0, pubLen = 0;
            Tcl_Obj **allElems = NULL;
            Tcl_Obj **pubElems = NULL;
            if (Tcl_ListObjGetElements(ip, allMethods, &allLen, &allElems) == TCL_OK && Tcl_ListObjGetElements(ip, pubMethods, &pubLen, &pubElems) == TCL_OK) {
                unexportedList = Tcl_NewListObj(0, NULL);
                Tcl_IncrRefCount(unexportedList);
                for (Tcl_Size ai = 0; ai < allLen; ai++) {
                    const char *aName = Tcl_GetString(allElems[ai]);
                    int         found = 0;
                    for (Tcl_Size pi = 0; pi < pubLen; pi++) {
                        if (strcmp(aName, Tcl_GetString(pubElems[pi])) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found)
                        Tcl_ListObjAppendElement(ip, unexportedList, allElems[ai]);
                }
            }
        }
        if (allMethods)
            Tcl_DecrRefCount(allMethods);
        if (pubMethods)
            Tcl_DecrRefCount(pubMethods);
    }

    for (e = Tcl_FirstHashEntry(&os->methodsByKey, &s); e; e = Tcl_NextHashEntry(&s)) {
        const char *k = (const char *)Tcl_GetHashKey(&os->methodsByKey, e);
        if (!k)
            continue;
        Tcl_Size kLen = (Tcl_Size)strlen(k);
        /* match "fqn\x1F" prefix */
        if (!(kLen > fqnLen + 1 && memcmp(k, fqn, (size_t)fqnLen) == 0 && k[fqnLen] == '\x1F')) {
            continue;
        }
        /* parse kind */
        const char *p = k + fqnLen + 1;    /* after first \x1F */
        const char *q = strchr(p, '\x1F'); /* second sep */
        if (!q)
            continue;
        unsigned kind = 0;
        for (const char *snum = p; snum < q; snum++) {
            if (*snum < '0' || *snum > '9') {
                kind = 0;
                break;
            }
            kind = (unsigned)(kind * 10 + (unsigned)(*snum - '0'));
        }
        const char *nameStr = q + 1; /* may be empty */
        /* Skip SELF methods — they are installed by the oo::objdefine stub
           in the top-level bytecode, which forwards to the saved original
           oo::objdefine with the precompiled body substituted.  Installing
           SELF methods here via DefOO's create-then-swap corrupts TclOO's
           dispatch chain (the Method exists in the hash but is invisible
           to the cached method chain). */
        if (kind == TBCX_METH_SELF)
            continue;
        Tcl_Obj *pair = (Tcl_Obj *)Tcl_GetHashValue(e); /* {args, procbody} */
        if (!pair)
            continue;
        Tcl_Obj *savedArgs = NULL, *procBody = NULL;
        {
            Tcl_Size  pairLen   = 0;
            Tcl_Obj **pairElems = NULL;
            if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) != TCL_OK || pairLen < 2)
                continue;
            savedArgs = pairElems[0];
            procBody  = pairElems[1];
        }

        Tcl_Obj *nameO = NULL;
        if (nameStr && *nameStr) {
            nameO = Tcl_NewStringObj(nameStr, -1);
            Tcl_IncrRefCount(nameO);
        }
        rc = DefOO(ip, os, clsFqn, (uint8_t)kind, nameO, (kind == TBCX_METH_DTOR ? savedArgs /* may be "" */ : savedArgs), procBody);
        if (nameO)
            Tcl_DecrRefCount(nameO);
        if (rc != TCL_OK) {
            /* leave the interp error as-is */
            if (unexportedList)
                Tcl_DecrRefCount(unexportedList);
            return rc;
        }
    }
    /* Restore unexported method flags that were lost during DefOO body
       swaps.  DefOO re-creates methods via oo::define which resets flags
       to PUBLIC, losing any prior "unexport" from the builder body.

       The unexport subcommand requires Tcl's full command dispatch context
       (TclOO dispatches it through an internal ensemble mechanism).  Direct
       savedDefineProc calls bypass that context and silently fail.  We
       temporarily restore the original handler on the Command struct so
       that Tcl_EvalObjv("oo::define") routes to TclOO directly — no
       recursion through CmdOOShim because the shim is temporarily removed.
       This is safe: single-threaded, no user code executes in this loop. */
    if (rc == TCL_OK && unexportedList) {
        Tcl_Size  uLen   = 0;
        Tcl_Obj **uElems = NULL;
        if (Tcl_ListObjGetElements(ip, unexportedList, &uLen, &uElems) == TCL_OK && uLen > 0 && os->defineCmdPtr) {
            /* Temporarily restore original handlers */
            os->defineCmdPtr->objProc2       = os->savedDefineProc;
            os->defineCmdPtr->objClientData2 = os->savedDefineCD;
            if (os->savedDefineNre)
                os->defineCmdPtr->nreProc2 = os->savedDefineNre;

            Tcl_Obj *defCmd = Tcl_NewStringObj("oo::define", -1);
            Tcl_IncrRefCount(defCmd);
            Tcl_Obj *unexpSub = Tcl_NewStringObj("unexport", -1);
            Tcl_IncrRefCount(unexpSub);
            for (Tcl_Size ui = 0; ui < uLen; ui++) {
                Tcl_Obj *av[4] = {defCmd, clsFqn, unexpSub, uElems[ui]};
                rc             = Tcl_EvalObjv(ip, 4, av, 0);
                if (rc != TCL_OK) {
                    break;
                }
            }
            Tcl_DecrRefCount(unexpSub);
            Tcl_DecrRefCount(defCmd);

            /* Re-install the shim */
            os->defineCmdPtr->objProc2       = CmdOOShim;
            os->defineCmdPtr->objClientData2 = os;
            if (os->savedDefineNre)
                os->defineCmdPtr->nreProc2 = CmdOOShim;

            if (rc == TCL_OK) {
                Tcl_ResetResult(ip);
            }
        }
    }
    if (unexportedList)
        Tcl_DecrRefCount(unexportedList);
    return rc;
}

/* OOShim_IdentifyMethod — parse the oo::define subcommand to determine
 * method kind, body index, args reference, and hash key for precompiled
 * body lookup.  Sets *hasKeyOut=1 and initializes keyDs on success. */
static void OOShim_IdentifyMethod(const char *subc, Tcl_Size objc, Tcl_Obj *const objv[], Tcl_Obj *clsFqn, Tcl_DString *keyDs, uint8_t *kindOut, Tcl_Size *bodyIdxOut, Tcl_Obj **runtimeArgsOut,
                                  Tcl_Obj **nameOOut, Tcl_Obj **tmpEmptyArgsOut, int *hasKeyOut) {
    *kindOut         = TBCX_METH_NONE;
    *bodyIdxOut      = -1;
    *runtimeArgsOut  = NULL;
    *nameOOut        = NULL;
    *tmpEmptyArgsOut = NULL;
    *hasKeyOut       = 0;

    if (strcmp(subc, "method") == 0 || strcmp(subc, "classmethod") == 0) {
        if (objc >= 6) {
            *kindOut         = (strcmp(subc, "classmethod") == 0) ? TBCX_METH_CLASS : TBCX_METH_INST;
            Tcl_Size nameIdx = objc - 3, argsIdx = objc - 2;
            *bodyIdxOut     = objc - 1;
            *runtimeArgsOut = objv[argsIdx];
            if (MethodKeyBuf(keyDs, clsFqn, *kindOut, objv[nameIdx]))
                *hasKeyOut = 1;
            *nameOOut = objv[nameIdx];
        }
    } else if (strcmp(subc, "constructor") == 0) {
        if (objc >= 5) {
            *kindOut         = TBCX_METH_CTOR;
            Tcl_Size argsIdx = objc - 2;
            *bodyIdxOut      = objc - 1;
            *runtimeArgsOut  = objv[argsIdx];
            if (MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL))
                *hasKeyOut = 1;
        }
    } else if (strcmp(subc, "destructor") == 0) {
        *kindOut = TBCX_METH_DTOR;
        if (objc >= 5) {
            Tcl_Size argsIdx = objc - 2;
            *bodyIdxOut      = objc - 1;
            *runtimeArgsOut  = objv[argsIdx];
            if (MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL))
                *hasKeyOut = 1;
        } else if (objc == 4) {
            *bodyIdxOut     = 3;
            *runtimeArgsOut = Tcl_NewStringObj("", 0);
            Tcl_IncrRefCount(*runtimeArgsOut);
            *tmpEmptyArgsOut = *runtimeArgsOut;
            if (MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL))
                *hasKeyOut = 1;
        }
    }
    /* oo::define CLS self method NAME ARGS BODY  (objc >= 7) */
    else if (strcmp(subc, "self") == 0 && objc >= 7) {
        const char *selfSub = Tbcx_GetStringStrict(NULL, objv[3]);
        if (!selfSub)
            return;
        if (strcmp(selfSub, "method") == 0) {
            *kindOut        = TBCX_METH_SELF;
            /* objv: [0]=oo::define [1]=CLS [2]=self [3]=method [4]=NAME [5]=ARGS [6]=BODY */
            *bodyIdxOut     = objc - 1;
            *runtimeArgsOut = objv[objc - 2];
            *nameOOut       = objv[objc - 3];
            if (MethodKeyBuf(keyDs, clsFqn, TBCX_METH_SELF, *nameOOut))
                *hasKeyOut = 1;
        }
    }
}

/* OOShim_LookupPair — look up the precompiled {args, procbody} pair for
 * the given method key.  Returns the pair elements via out-params. */
static void OOShim_LookupPair(Tcl_Interp *ip, OOShim *os, Tcl_DString *keyDs, int hasKey, Tcl_Size bodyIdx, Tcl_Obj *runtimeArgs, Tcl_Obj **savedArgsOut, Tcl_Obj **preBodyOut) {
    *savedArgsOut = NULL;
    *preBodyOut   = NULL;
    if (hasKey && bodyIdx >= 0 && runtimeArgs != NULL) {
        Tcl_HashEntry *he = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(keyDs));
        if (he) {
            Tcl_Obj *pair = (Tcl_Obj *)Tcl_GetHashValue(he);
            if (pair) {
                Tcl_Size  pairLen   = 0;
                Tcl_Obj **pairElems = NULL;
                if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) == TCL_OK && pairLen >= 2) {
                    *savedArgsOut = pairElems[0];
                    *preBodyOut   = pairElems[1];
                }
            }
        }
    }
}

static int CmdOOShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    OOShim   *os  = (OOShim *)cd;
    int       rc  = TCL_OK;
    Tcl_Obj  *cls = NULL, *sub = NULL;
    Tcl_Obj  *nameO = NULL; /* method/classmethod name when applicable */

    /* Prepare argv for calling the saved original handler.
       argv2[0] = objv[0] (the command name the caller used). */
    Tcl_Obj  *argvStack[TBCX_ARGV_STACK];
    Tcl_Obj **argv2;
    if (objc <= TBCX_ARGV_STACK) {
        argv2 = argvStack;
    } else {
        size_t allocSz;
        if (objc < 0 || !tbcx_checked_mul(sizeof(Tcl_Obj *), (size_t)objc, &allocSz)) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: argument count overflow in oo::define shim", -1));
            return TCL_ERROR;
        }
        argv2 = (Tcl_Obj **)Tcl_Alloc(allocSz);
    }
    argv2[0] = objv[0];
    for (Tcl_Size i = 1; i < objc; i++) {
        argv2[i] = objv[i];
    }

    /* Fast path: if we don't even have "class subcmd ..." just forward. */
    if (objc < 3) {
        rc = os->savedDefineProc(os->savedDefineCD, ip, objc, argv2);
        if (argv2 != argvStack)
            Tcl_Free(argv2);
        return rc;
    }

    cls               = objv[1];
    sub               = objv[2];

    /* Compute if this is the builder form: "oo::define <cls> <script>" */
    int isBuilderForm = 0;
    if (objc == 3) {
        isBuilderForm = 1;
    }

    /* Compute class FQN under current namespace if relative. */
    Tcl_Obj    *clsFqn = NULL;
    const char *nm     = Tbcx_GetStringSafe(cls);
    if (nm[0] == ':' && nm[1] == ':') {
        clsFqn = cls; /* borrow ref */
    } else {
        Tcl_Namespace *cur     = Tcl_GetCurrentNamespace(ip);
        const char    *curName = cur ? cur->fullName : "::";
        clsFqn                 = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0')) {
            Tcl_AppendToObj(clsFqn, "::", 2);
        }
        Tcl_AppendObjToObj(clsFqn, cls);
        Tcl_IncrRefCount(clsFqn);
    }

    /* Try to locate a precompiled body matching this definition form. */
    const char *subc = Tbcx_GetStringSafe(sub);
    Tcl_DString keyDs;                         /* hash key "class\x1Fkind\x1Fname" */
    int         hasKey       = 0;              /* set to 1 once keyDs is initialized */
    Tcl_Obj    *savedArgs    = NULL;           /* canonical args saved with the precompiled body */
    Tcl_Obj    *preBody      = NULL;           /* precompiled procbody to substitute */
    Tcl_Obj    *runtimeArgs  = NULL;           /* args provided by the user at runtime */
    Tcl_Obj    *tmpEmptyArgs = NULL;           /* temp "" for destructor short form */
    Tcl_Size    bodyIdx      = -1;             /* index of body word in argv */
    uint8_t     kind         = TBCX_METH_NONE; /* TBCX_METH_* */

    /* Identify the OO subcommand and look up a precompiled pair. */
    OOShim_IdentifyMethod(subc, objc, objv, clsFqn, &keyDs, &kind, &bodyIdx, &runtimeArgs, &nameO, &tmpEmptyArgs, &hasKey);
    OOShim_LookupPair(ip, os, &keyDs, hasKey, bodyIdx, runtimeArgs, &savedArgs, &preBody);

    /* If we have a precompiled body and the signature matches byte-wise,
       substitute the body argument and call the original. */
    if (preBody && savedArgs && !isBuilderForm) {
        if (kind == TBCX_METH_CTOR || kind == TBCX_METH_DTOR) {
            rc = DefOO(ip, os, clsFqn, kind, NULL, (kind == TBCX_METH_CTOR ? savedArgs : NULL), preBody);
            goto cleanup;
        }
        Tcl_Size _dummyLen = 0;
        /* Force list-shimmering so the string rep is canonical before
           byte-wise comparison.  If conversion fails the comparison still
           works on the existing string rep; clear the interp result to
           avoid contaminating later evaluation. */
        if (Tcl_ListObjLength(ip, runtimeArgs, &_dummyLen) != TCL_OK)
            Tcl_ResetResult(ip);
        if (Tcl_ListObjLength(ip, savedArgs, &_dummyLen) != TCL_OK)
            Tcl_ResetResult(ip);
        Tcl_Size    aLen = 0, bLen = 0;
        const char *a = Tbcx_GetStringFromObjSafe(runtimeArgs, &aLen);
        const char *b = Tbcx_GetStringFromObjSafe(savedArgs, &bLen);

        if (aLen == bLen && memcmp(a, b, (size_t)aLen) == 0) {
            rc = DefOO(ip, os, clsFqn, kind, (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS || kind == TBCX_METH_SELF) ? nameO : NULL, (kind == TBCX_METH_DTOR ? savedArgs : savedArgs), preBody);
            goto cleanup;
        }
        /* Fast path for stubbed empty bodies: install precompiled body directly. */
        if (preBody && bodyIdx >= 0) {
            Tcl_Size bodyLen = 0;
            (void)Tbcx_GetStringFromObjSafe(objv[bodyIdx], &bodyLen);
            if (bodyLen == 0) {
                if (kind == TBCX_METH_CTOR) {
                    Tcl_Obj *argsForCtor = savedArgs ? savedArgs : Tcl_NewObj();
                    if (!savedArgs)
                        Tcl_IncrRefCount(argsForCtor);
                    rc = DefOO(ip, os, clsFqn, TBCX_METH_CTOR, NULL, argsForCtor, preBody);
                    if (!savedArgs)
                        Tcl_DecrRefCount(argsForCtor);
                } else if (kind == TBCX_METH_DTOR) {
                    rc = DefOO(ip, os, clsFqn, TBCX_METH_DTOR, NULL, NULL, preBody);
                } else {
                    rc = DefOO(ip, os, clsFqn, kind, nameO, savedArgs, preBody);
                }
                /* cleanup and return */
                goto cleanup;
            }
        }
    }

    if (!isBuilderForm) {
        /* Fallback: forward to the original without substitution.
         * Special tolerance for destructor with accidental empty-args list:
         * If objc>=5 and args=={}, collapse to canonical 4-arg form.
         */
        if (kind == TBCX_METH_DTOR && objc >= 5 && runtimeArgs) {
            Tcl_Size nEl    = -1;
            int      listRc = Tcl_ListObjLength(ip, runtimeArgs, &nEl);
            if (listRc != TCL_OK) {
                /* Failed canonicalization should simply disable the special
                   empty-args shortcut.  Clear the transient list-conversion
                   error before forwarding to the original command. */
                Tcl_ResetResult(ip);
            }
            if (listRc == TCL_OK && nEl == 0) {
                Tcl_Obj *body = objv[objc - 1];
                rc            = DefOO(ip, os, clsFqn, TBCX_METH_DTOR, NULL, NULL, body);
            } else {
                /* Forward to original; normalize the class arg to FQN */
                argv2[1] = clsFqn;
                Tcl_IncrRefCount(clsFqn);
                rc = os->savedDefineProc(os->savedDefineCD, ip, objc, argv2);
                Tcl_DecrRefCount(clsFqn);
            }
        } else {
            /* For builder form and other subcommands, ensure class arg is FQN */
            if (objc >= 2) {
                argv2[1] = clsFqn;
                Tcl_IncrRefCount(clsFqn);
            }
            rc = os->savedDefineProc(os->savedDefineCD, ip, objc, argv2);
            if (objc >= 2)
                Tcl_DecrRefCount(clsFqn);
        }
    } else {
        /* Builder form: run the builder block first, then apply precompiled bodies
         * for this class (namespace-relative class names resolved to FQN above). */
        /* Ensure class arg is FQN and stay in current namespace (not global) */
        if (objc >= 2) {
            argv2[1] = clsFqn;
            Tcl_IncrRefCount(clsFqn);
        }
        rc = os->savedDefineProc(os->savedDefineCD, ip, objc, argv2);
        if (objc >= 2)
            Tcl_DecrRefCount(clsFqn);
        if (rc == TCL_OK) {
            int patchRc = PrecompClass(ip, os, clsFqn);
            if (patchRc != TCL_OK) {
                rc = patchRc; /* leave error in interp */
            }
        }
    }
    /*
     * HARDENING: Even if the inline substitution above didn’t trigger,
     * ensure the precompiled body is installed. If we have a saved
     * {args,procbody} pair for this exact define form, re-apply it now
     * using the saved original handler. This
     * guarantees the constructor/method ends up with the compiled body
     * instead of the empty stub emitted by the saver.
     */
    if (rc == TCL_OK && hasKey) {
        Tcl_HashEntry *he2 = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs));
        if (he2) {
            Tcl_Obj *pair2      = (Tcl_Obj *)Tcl_GetHashValue(he2);
            Tcl_Obj *savedArgs2 = NULL, *preBody2 = NULL;
            if (pair2) {
                Tcl_Size  p2Len   = 0;
                Tcl_Obj **p2Elems = NULL;
                if (Tcl_ListObjGetElements(ip, pair2, &p2Len, &p2Elems) == TCL_OK && p2Len >= 2) {
                    savedArgs2 = p2Elems[0];
                    preBody2   = p2Elems[1];
                }
            }
            if (savedArgs2 && preBody2) {
                /* Normalize class arg to FQN for deterministic resolution */
                Tcl_Obj *clsForCall = clsFqn;
                if (clsForCall != cls)
                    Tcl_IncrRefCount(clsForCall);
                int rc2 = TCL_OK;
                if (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS || kind == TBCX_METH_SELF) {
                    if (nameO) {
                        rc2 = DefOO(ip, os, clsForCall, kind, nameO, savedArgs2, preBody2);
                    }
                } else if (kind == TBCX_METH_CTOR) {
                    rc2 = DefOO(ip, os, clsForCall, TBCX_METH_CTOR, NULL, savedArgs2, preBody2);
                } else if (kind == TBCX_METH_DTOR) {
                    rc2 = DefOO(ip, os, clsForCall, TBCX_METH_DTOR, NULL, NULL, preBody2);
                }
                if (rc2 != TCL_OK)
                    rc = rc2;
                if (clsForCall != cls)
                    Tcl_DecrRefCount(clsForCall);
            }
        }
    }

cleanup:
    if (hasKey)
        Tcl_DStringFree(&keyDs);
    if (clsFqn != cls)
        Tcl_DecrRefCount(clsFqn);
    if (tmpEmptyArgs)
        Tcl_DecrRefCount(tmpEmptyArgs);
    if (argv2 != argvStack)
        Tcl_Free(argv2);
    return rc;
}

/* oo::objdefine shim — mirrors CmdOOShim logic but forwards to
   the saved original oo::objdefine handler.  Uses the same
   methodsByKey registry since saver stores objdefine methods
   there keyed by the object/class FQN. */
static int CmdOOShimObjDef(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    OOShim   *os = (OOShim *)cd;
    int       rc = TCL_OK;

    /* Prepare argv for calling the saved original handler. */
    Tcl_Obj  *argvStack[TBCX_ARGV_STACK];
    Tcl_Obj **argv2;
    if (objc <= TBCX_ARGV_STACK) {
        argv2 = argvStack;
    } else {
        size_t allocSz;
        if (objc < 0 || !tbcx_checked_mul(sizeof(Tcl_Obj *), (size_t)objc, &allocSz)) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: argument count overflow in oo::objdefine shim", -1));
            return TCL_ERROR;
        }
        argv2 = (Tcl_Obj **)Tcl_Alloc(allocSz);
    }
    argv2[0] = objv[0];
    for (Tcl_Size i = 1; i < objc; i++)
        argv2[i] = objv[i];

    if (objc < 3) {
        rc = os->savedObjdefProc(os->savedObjdefCD, ip, objc, argv2);
        if (argv2 != argvStack)
            Tcl_Free(argv2);
        return rc;
    }

    Tcl_Obj    *obj           = objv[1];
    Tcl_Obj    *sub           = objv[2];
    int         isBuilderForm = (objc == 3);

    /* Compute object FQN */
    Tcl_Obj    *objFqn        = NULL;
    const char *nm            = Tbcx_GetStringSafe(obj);
    if (nm[0] == ':' && nm[1] == ':') {
        objFqn = obj;
    } else {
        Tcl_Namespace *cur     = Tcl_GetCurrentNamespace(ip);
        const char    *curName = cur ? cur->fullName : "::";
        objFqn                 = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0'))
            Tcl_AppendToObj(objFqn, "::", 2);
        Tcl_AppendObjToObj(objFqn, obj);
        Tcl_IncrRefCount(objFqn);
    }

    /* For multi-word form: try to find and substitute precompiled body */
    if (!isBuilderForm) {
        const char *subc = Tbcx_GetStringSafe(sub);
        Tcl_DString keyDs;
        int         hasKey  = 0;
        Tcl_Obj    *nameO   = NULL;
        Tcl_Size    bodyIdx = -1;
        uint8_t     kind    = TBCX_METH_NONE;

        if ((strcmp(subc, "method") == 0 || strcmp(subc, "classmethod") == 0) && objc >= 6) {
            kind    = (strcmp(subc, "classmethod") == 0) ? TBCX_METH_CLASS : TBCX_METH_INST;
            bodyIdx = objc - 1;
            nameO   = objv[objc - 3];
            if (MethodKeyBuf(&keyDs, objFqn, kind, nameO))
                hasKey = 1;
            /* Fallback: for "method" in oo::objdefine context, the saver
               may have stored it as TBCX_METH_SELF (kind=4).  Try that key
               if the INST key has no match. */
            if (strcmp(subc, "method") == 0 && hasKey && !Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs))) {
                Tcl_DString selfDs;
                if (MethodKeyBuf(&selfDs, objFqn, TBCX_METH_SELF, nameO)) {
                    if (Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&selfDs))) {
                        /* Rebuild keyDs with the SELF key */
                        Tcl_DStringFree(&keyDs);
                        MethodKeyBuf(&keyDs, objFqn, TBCX_METH_SELF, nameO);
                        kind = TBCX_METH_SELF;
                    }
                    Tcl_DStringFree(&selfDs);
                }
            }
        } else if (strcmp(subc, "constructor") == 0 && objc >= 5) {
            kind    = TBCX_METH_CTOR;
            bodyIdx = objc - 1;
            if (MethodKeyBuf(&keyDs, objFqn, kind, NULL))
                hasKey = 1;
        } else if (strcmp(subc, "destructor") == 0 && objc >= 4) {
            kind    = TBCX_METH_DTOR;
            bodyIdx = objc - 1;
            if (MethodKeyBuf(&keyDs, objFqn, kind, NULL))
                hasKey = 1;
        }

        if (hasKey && bodyIdx >= 0) {
            Tcl_HashEntry *he = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs));
            if (he) {
                Tcl_Obj *pair      = (Tcl_Obj *)Tcl_GetHashValue(he);
                Tcl_Obj *savedArgs = NULL, *preBody = NULL;
                if (pair) {
                    Tcl_Size  pLen   = 0;
                    Tcl_Obj **pElems = NULL;
                    if (Tcl_ListObjGetElements(ip, pair, &pLen, &pElems) == TCL_OK && pLen >= 2) {
                        savedArgs = pElems[0];
                        preBody   = pElems[1];
                    }
                }
                (void)savedArgs; /* only preBody used in objdefine path */
                if (preBody) {
                    /* CRITICAL: Do NOT substitute procbody for SELF methods.
                       When oo::define CLS self method ... is processed,
                       TclOO's "self" subcommand internally delegates to
                       oo::objdefine, which hits this shim.  Substituting
                       the procbody here creates a method that doesn't
                       survive TclOO's method chain rebuilds (triggered by
                       subclass "superclass" declarations).  Instead, just
                       forward with the original ";" body so TclOO creates
                       a valid Method+Proc.  The DefOO SELF create-then-swap
                       in the define shim will handle the bytecode installation
                       by swapping the Proc's bodyPtr after oo::define returns. */
                    if (kind == TBCX_METH_SELF) {
                        Tcl_DStringFree(&keyDs);
                        goto objdefine_forward; /* skip second DStringFree */
                    } else {
                        /* Substitute the body argument and forward */
                        argv2[bodyIdx] = preBody;
                        rc             = os->savedObjdefProc(os->savedObjdefCD, ip, objc, argv2);
                        Tcl_DStringFree(&keyDs);
                        if (objFqn != obj)
                            Tcl_DecrRefCount(objFqn);
                        if (argv2 != argvStack)
                            Tcl_Free(argv2);
                        return rc;
                    }
                }
            }
            Tcl_DStringFree(&keyDs);
        }
    }

objdefine_forward:
    /* Builder form or unmatched: forward to real oo::objdefine */
    rc = os->savedObjdefProc(os->savedObjdefCD, ip, objc, argv2);

    /* After builder form execution, apply precompiled bodies.
       Use the saved oo::define handler for body installation via PrecompClass,
       since per-object methods can also be set through oo::define on the object. */
    if (isBuilderForm && rc == TCL_OK) {
        int patchRc = PrecompClass(ip, os, objFqn);
        if (patchRc != TCL_OK) {
            rc = patchRc; /* preserve PrecompClass() error/result */
        }
    }

    if (objFqn != obj)
        Tcl_DecrRefCount(objFqn);
    if (argv2 != argvStack)
        Tcl_Free(argv2);
    return rc;
}

/* OOShimDefineCmdTrace — called when "oo::define" is renamed or deleted
 * while the shim is active.  Invalidates the cached Command* to prevent
 * stale-pointer writes in DelOOShim. */
static void OOShimDefineCmdTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags) {
    OOShim *os = (OOShim *)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & TCL_TRACE_RENAME) {
        /* Same UAF-avoidance as ProcCmdDeleteTrace: the Command struct
         * survives the rename, so we must restore its handlers *now*
         * while it's still accessible — otherwise the renamed command
         * retains our CmdOOShim with a dangling os pointer once
         * LoadTbcxStream returns. */
        if (os->defineCmdPtr) {
            os->defineCmdPtr->objProc2       = os->savedDefineProc;
            os->defineCmdPtr->objClientData2 = os->savedDefineCD;
            if (os->savedDefineNre) {
                os->defineCmdPtr->nreProc2 = os->savedDefineNre;
            }
        }
    }
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE)) {
        os->defineCmdPtr         = NULL;
        os->defineTraceInstalled = 0;
    }
}

/* OOShimObjdefCmdTrace — same for "oo::objdefine". */
static void OOShimObjdefCmdTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags) {
    OOShim *os = (OOShim *)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & TCL_TRACE_RENAME) {
        if (os->objdefCmdPtr) {
            os->objdefCmdPtr->objProc2       = os->savedObjdefProc;
            os->objdefCmdPtr->objClientData2 = os->savedObjdefCD;
            if (os->savedObjdefNre) {
                os->objdefCmdPtr->nreProc2 = os->savedObjdefNre;
            }
        }
    }
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE)) {
        os->objdefCmdPtr         = NULL;
        os->objdefTraceInstalled = 0;
    }
}

/* AddOOShim — intercept [oo::define] and [oo::objdefine] to substitute
 * precompiled method/constructor/destructor bodies.
 *
 * Patches the existing Command handlers in place (like ProcShim and
 * ApplyShim) instead of renaming commands.  Installs rename/delete
 * traces to detect if the shimmed commands are moved or destroyed
 * during top-level script execution.
 *
 * Thread safety / lock ordering:
 *   No TBCX mutex is held during this call.
 *   Must be called from the interp-owning thread only. */
static int AddOOShim(Tcl_Interp *ip, OOShim *os) {
    memset(os, 0, sizeof(*os));
    Tcl_InitHashTable(&os->methodsByKey, TCL_STRING_KEYS);

    /* Find and patch oo::define */
    Tcl_Command defToken = Tcl_FindCommand(ip, "oo::define", NULL, 0);
    if (!defToken) {
        Tcl_DeleteHashTable(&os->methodsByKey);
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: oo::define not found", -1));
        return TCL_ERROR;
    }
    os->defineCmdPtr    = (Command *)defToken;
    os->savedDefineProc = os->defineCmdPtr->objProc2;
    os->savedDefineNre  = os->defineCmdPtr->nreProc2;
    os->savedDefineCD   = os->defineCmdPtr->objClientData2;

    if (Tcl_TraceCommand(ip, "oo::define", TCL_TRACE_RENAME | TCL_TRACE_DELETE, OOShimDefineCmdTrace, os) != TCL_OK) {
        Tcl_DeleteHashTable(&os->methodsByKey);
        return TCL_ERROR;
    }
    os->defineTraceInstalled         = 1;

    os->defineCmdPtr->objProc2       = CmdOOShim;
    os->defineCmdPtr->objClientData2 = os;
    if (os->defineCmdPtr->nreProc2) {
        os->defineCmdPtr->nreProc2 = CmdOOShim;
    }

    /* Also shim oo::objdefine if it exists */
    os->hasObjDefine        = 0;
    Tcl_Command objdefToken = Tcl_FindCommand(ip, "oo::objdefine", NULL, 0);
    if (objdefToken) {
        os->objdefCmdPtr    = (Command *)objdefToken;
        os->savedObjdefProc = os->objdefCmdPtr->objProc2;
        os->savedObjdefNre  = os->objdefCmdPtr->nreProc2;
        os->savedObjdefCD   = os->objdefCmdPtr->objClientData2;

        if (Tcl_TraceCommand(ip, "oo::objdefine", TCL_TRACE_RENAME | TCL_TRACE_DELETE, OOShimObjdefCmdTrace, os) == TCL_OK) {
            os->objdefTraceInstalled         = 1;
            os->objdefCmdPtr->objProc2       = CmdOOShimObjDef;
            os->objdefCmdPtr->objClientData2 = os;
            if (os->objdefCmdPtr->nreProc2) {
                os->objdefCmdPtr->nreProc2 = CmdOOShimObjDef;
            }
            os->hasObjDefine = 1;
        }
    }
    return TCL_OK;
}

static void DelOOShim(Tcl_Interp *ip, OOShim *os) {
    /* Remove traces before restoring handlers */
    if (os->defineTraceInstalled) {
        Tcl_UntraceCommand(ip, "oo::define", TCL_TRACE_RENAME | TCL_TRACE_DELETE, OOShimDefineCmdTrace, os);
        os->defineTraceInstalled = 0;
    }
    /* Restore oo::define handlers only if the Command is still alive */
    if (os->defineCmdPtr) {
        os->defineCmdPtr->objProc2       = os->savedDefineProc;
        os->defineCmdPtr->objClientData2 = os->savedDefineCD;
        if (os->savedDefineNre) {
            os->defineCmdPtr->nreProc2 = os->savedDefineNre;
        }
    }

    if (os->hasObjDefine) {
        if (os->objdefTraceInstalled) {
            Tcl_UntraceCommand(ip, "oo::objdefine", TCL_TRACE_RENAME | TCL_TRACE_DELETE, OOShimObjdefCmdTrace, os);
            os->objdefTraceInstalled = 0;
        }
        if (os->objdefCmdPtr) {
            os->objdefCmdPtr->objProc2       = os->savedObjdefProc;
            os->objdefCmdPtr->objClientData2 = os->savedObjdefCD;
            if (os->savedObjdefNre) {
                os->objdefCmdPtr->nreProc2 = os->savedObjdefNre;
            }
        }
    }

    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&os->methodsByKey, &s); e; e = Tcl_NextHashEntry(&s)) {
        Tcl_Obj *val = (Tcl_Obj *)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&os->methodsByKey);
}

Tcl_Namespace *Tbcx_EnsureNamespace(Tcl_Interp *ip, const char *fqn) {
    /* Validate the namespace string at the API boundary rather than
       relying on each call site to validate beforehand.
       Note: embedded NULs cannot be detected from a plain const char*;
       that check belongs at the wire-reading sites (Tbcx_ValidateKeyString). */
    if (!fqn || fqn[0] == '\0') {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: empty namespace path", -1));
        return NULL;
    }
    if (!(fqn[0] == ':' && fqn[1] == ':')) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: namespace must be absolute: \"%s\"", fqn));
        return NULL;
    }
    Tcl_Namespace *nsPtr = Tcl_FindNamespace(ip, fqn, NULL, 0);
    if (!nsPtr) {
        nsPtr = Tcl_CreateNamespace(ip, fqn, NULL, NULL);
    }
    return nsPtr;
}

/* Mirrors TclInitByteCode()’s packed layout + TclInitByteCodeObj()’s attach,
 * but never calls internal TclPreserveByteCode(). Instead we set refCount = 1.
 */

static ByteCode *TbcxByteCode(Tcl_Obj *objPtr, const Tcl_ObjType *typePtr, const TBCX_CompileEnvMin *env, int setPrecompiled) {
    Interp      *iPtr              = (Interp *)env->interp;
    Namespace   *nsPtr             = env->nsPtr ? env->nsPtr : (iPtr->varFramePtr ? iPtr->varFramePtr->nsPtr : iPtr->globalNsPtr);

    /* Sizes for packed allocation (match TclInitByteCode) */
    const size_t codeBytes         = (size_t)(env->codeNext - env->codeStart);
    const size_t objArrayBytes     = (size_t)env->numLitObjects * sizeof(Tcl_Obj *);
    const size_t exceptArrayBytes  = (size_t)env->numExceptRanges * sizeof(ExceptionRange);
    const size_t auxDataArrayBytes = (size_t)env->numAuxDataItems * sizeof(AuxData);
    const size_t cmdLocBytes       = 0; /* no cmd-location map stored */

    /* Overflow guard: ensure the combined structure size doesn't wrap around.
       Use SIZE_MAX/2 as a generous upper bound to detect pathological inputs
       from malformed .tbcx files. */
    {
        size_t safeCap = SIZE_MAX / 2;
        if (codeBytes > safeCap || objArrayBytes > safeCap || exceptArrayBytes > safeCap || auxDataArrayBytes > safeCap) {
            Tcl_SetObjResult(env->interp, Tcl_NewStringObj("tbcx: ByteCode section sizes overflow", -1));
            return NULL;
        }
    }

    /* Validate exception-range offsets against the code block bounds. */
    if (env->numExceptRanges > 0 && env->exceptArrayPtr) {
        for (Tcl_Size vi = 0; vi < env->numExceptRanges; vi++) {
            ExceptionRange *er = &env->exceptArrayPtr[vi];
            /* Overflow-safe bounds check: validate offset first, then
               remaining space, to avoid uint32_t wrap-around. */
            if ((size_t)er->codeOffset > codeBytes || (size_t)er->numCodeBytes > codeBytes - (size_t)er->codeOffset) {
                Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: exception range %td offset+len (%u+%u) exceeds code size (%lu)", vi, (unsigned)er->codeOffset, (unsigned)er->numCodeBytes,
                                                            (unsigned long)codeBytes));
                return NULL;
            }
            if (er->catchOffset >= 0 && (uint32_t)er->catchOffset >= codeBytes) {
                Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: exception range %td catchOffset (%u) exceeds code size (%lu)", vi, (unsigned)er->catchOffset, (unsigned long)codeBytes));
                return NULL;
            }
            /* Validate continue/break handler offsets (same pattern as catchOffset). */
            if (er->continueOffset >= 0 && (size_t)er->continueOffset >= codeBytes) {
                Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: exception range %td continueOffset (%u) exceeds code size (%lu)", vi, (unsigned)er->continueOffset, (unsigned long)codeBytes));
                return NULL;
            }
            if (er->breakOffset >= 0 && (size_t)er->breakOffset >= codeBytes) {
                Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: exception range %td breakOffset (%u) exceeds code size (%lu)", vi, (unsigned)er->breakOffset, (unsigned long)codeBytes));
                return NULL;
            }
        }
    }

    size_t structureSize = 0;
    {
        /* Checked addition: detect wrap-around from the sum of aligned
           segments, which the per-segment SIZE_MAX/2 guard above does
           not fully prevent on 32-bit targets. */
#define TBCX_CHECKED_ADD(sz, val)                                                                                                                                                                      \
    do {                                                                                                                                                                                               \
        size_t _aligned = TCL_ALIGN(val);                                                                                                                                                              \
        if ((sz) + _aligned < (sz)) {                                                                                                                                                                  \
            Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: ByteCode total size overflow"));                                                                                                        \
            return NULL;                                                                                                                                                                               \
        }                                                                                                                                                                                              \
        (sz) += _aligned;                                                                                                                                                                              \
    } while (0)

        TBCX_CHECKED_ADD(structureSize, sizeof(ByteCode));
        TBCX_CHECKED_ADD(structureSize, codeBytes);
        TBCX_CHECKED_ADD(structureSize, objArrayBytes);
        TBCX_CHECKED_ADD(structureSize, exceptArrayBytes);
        TBCX_CHECKED_ADD(structureSize, auxDataArrayBytes);
        TBCX_CHECKED_ADD(structureSize, cmdLocBytes);
        TBCX_CHECKED_ADD(structureSize, (size_t)0);

#undef TBCX_CHECKED_ADD
    }

    unsigned char *base = (unsigned char *)Tcl_AttemptAlloc(structureSize);
    if (!base) {
        Tcl_SetObjResult(env->interp, Tcl_NewStringObj("tbcx: allocation failed (ByteCode structure)", -1));
        return NULL;
    }
    ByteCode *codePtr = (ByteCode *)base;

    /* ----- Header & environment capture (same fields as core) ----- */
    memset(codePtr, 0, sizeof(ByteCode));
    codePtr->interpHandle    = TclHandlePreserve(iPtr->handle);
    codePtr->compileEpoch    = iPtr->compileEpoch;
    codePtr->nsPtr           = nsPtr;
    codePtr->nsEpoch         = nsPtr->resolverEpoch;

    /* *** Inline TclPreserveByteCode(codePtr) *** */
    codePtr->refCount        = 1; /* brand-new ByteCode held by this one Tcl_Obj */
    codePtr->flags           = ((nsPtr->compiledVarResProc || iPtr->resolverPtr) ? TCL_BYTECODE_RESOLVE_VARS : 0);
    codePtr->source          = NULL;         /* no retained source for precompiled */
    codePtr->procPtr         = env->procPtr; /* may be NULL for top-level blocks   */

    codePtr->maxExceptDepth  = TCL_INDEX_NONE;
    codePtr->maxStackDepth   = (Tcl_Size)env->maxStackDepth;
    codePtr->numAuxDataItems = (Tcl_Size)env->numAuxDataItems;
    codePtr->numCmdLocBytes  = 0;
    codePtr->numCodeBytes    = (Tcl_Size)codeBytes;
    codePtr->numCommands     = 0;
    codePtr->numExceptRanges = (Tcl_Size)env->numExceptRanges;
    codePtr->numLitObjects   = (Tcl_Size)env->numLitObjects;
    codePtr->numSrcBytes     = 0;
    codePtr->structureSize   = (Tcl_Size)structureSize;

    /* ----- Pack variable-length tails with Tcl's exact alignment sequence ---- */
    unsigned char *p         = base + TCL_ALIGN(sizeof(ByteCode));

    /* 1) Code bytes */
    codePtr->codeStart       = p;
    if (codeBytes)
        memcpy(p, env->codeStart, codeBytes);
    p += TCL_ALIGN(codeBytes);

    /* 2) Literal object array */
    p                    = (unsigned char *)TCL_ALIGN((uintptr_t)p);
    codePtr->objArrayPtr = (Tcl_Obj **)p;
    if (env->numLitObjects) {
        for (Tcl_Size i = 0; i < env->numLitObjects; i++) {
            Tcl_Obj *lit            = env->objArrayPtr[i];
            codePtr->objArrayPtr[i] = lit;
            if (lit)
                Tcl_IncrRefCount(lit);
        }
    }
    p += TCL_ALIGN(objArrayBytes);

    /* 3) Exception ranges */
    p                       = (unsigned char *)TCL_ALIGN((uintptr_t)p);
    codePtr->exceptArrayPtr = (ExceptionRange *)p;
    if (env->numExceptRanges) {
        memcpy(p, env->exceptArrayPtr, exceptArrayBytes);
    }
    p += TCL_ALIGN(exceptArrayBytes);

    /* 4) AuxData array */

    if (env->numExceptRanges > 0 && env->exceptArrayPtr) {
        Tcl_Size maxDepth = 0;
        for (Tcl_Size i = 0; i < env->numExceptRanges; i++) {
            ExceptionRange *er = &env->exceptArrayPtr[i];
            Tcl_Size        d  = (Tcl_Size)er->nestingLevel + 1; /* levels start at 0 */
            if (d > maxDepth) {
                maxDepth = d;
            }
        }
        codePtr->maxExceptDepth = maxDepth;
    } else {
        codePtr->maxExceptDepth = 0;
    }
    p                        = (unsigned char *)TCL_ALIGN((uintptr_t)p);
    codePtr->auxDataArrayPtr = (AuxData *)p;
    if (env->numAuxDataItems) {
        memcpy(p, env->auxDataArrayPtr, auxDataArrayBytes);
    }
    p += TCL_ALIGN(auxDataArrayBytes);

    /* 5) (Empty) command-location map segment + required alignment step */
    p                        = (unsigned char *)TCL_ALIGN((uintptr_t)(p + cmdLocBytes));

    /* 6) Cursors: in a precompiled image we just coalesce them to the tail */
    codePtr->codeDeltaStart  = p;
    codePtr->codeLengthStart = p;
    codePtr->srcDeltaStart   = p;
    codePtr->srcLengthStart  = p;

    /* Locals cache is created lazily by the engine if needed */
    codePtr->localCachePtr   = NULL;

    /* ----- Attach as the internal rep of objPtr ----- */
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = codePtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(objPtr, typePtr, &ir);
    if (setPrecompiled) {
        codePtr->flags |= TCL_BYTECODE_PRECOMPILED;
    }
    return codePtr;
}

static Tcl_Obj *ByteCodeObj(Tcl_Interp *ip, Namespace *nsPtr, const unsigned char *code, uint32_t codeLen, Tcl_Obj **lits, uint32_t numLits, AuxData *auxArr, uint32_t numAux, ExceptionRange *exArr,
                            uint32_t numEx, int maxStackDepth, int setPrecompiled) {
    Tcl_Obj           *bcObj = NULL;
    TBCX_CompileEnvMin env;
    memset(&env, 0, sizeof(env));
    env.interp        = ip;
    env.nsPtr         = (Namespace *)nsPtr;
    env.maxStackDepth = maxStackDepth;
    env.procPtr       = NULL;

    env.codeStart     = (unsigned char *)Tcl_AttemptAlloc(codeLen ? codeLen : 1u);
    if (!env.codeStart) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (code buffer)", -1));
        goto read_fail;
    }
    if (codeLen)
        memcpy(env.codeStart, code, codeLen);
    env.codeNext      = env.codeStart + codeLen;
    env.objArrayPtr   = NULL;
    env.numLitObjects = (Tcl_Size)numLits;
    if (numLits) {
        env.objArrayPtr = (Tcl_Obj **)Tcl_AttemptAlloc(sizeof(Tcl_Obj *) * numLits);
        if (!env.objArrayPtr) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (literal array)", -1));
            goto read_fail;
        }
        for (uint32_t i = 0; i < numLits; i++) {
            env.objArrayPtr[i] = lits[i];
            if (env.objArrayPtr[i])
                Tcl_IncrRefCount(env.objArrayPtr[i]);
        }
    }

    env.auxDataArrayPtr = NULL;
    env.numAuxDataItems = (Tcl_Size)numAux;
    if (numAux) {
        env.auxDataArrayPtr = (AuxData *)Tcl_AttemptAlloc(sizeof(AuxData) * numAux);
        if (!env.auxDataArrayPtr) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (aux data array)", -1));
            goto read_fail;
        }
        for (uint32_t i = 0; i < numAux; i++)
            env.auxDataArrayPtr[i] = auxArr[i];
    }

    env.exceptArrayPtr  = NULL;
    env.numExceptRanges = (Tcl_Size)numEx;
    if (numEx) {
        env.exceptArrayPtr = (ExceptionRange *)Tcl_AttemptAlloc(sizeof(ExceptionRange) * numEx);
        if (!env.exceptArrayPtr) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (exception range array)", -1));
            goto read_fail;
        }
        for (uint32_t i = 0; i < numEx; i++)
            env.exceptArrayPtr[i] = exArr[i];
    }

    bcObj = Tcl_NewObj();
    if (!TbcxByteCode(bcObj, tbcxTyBytecode, &env, setPrecompiled)) {
        /* TbcxByteCode failed (overflow, exception-range validation, etc.).
         * Clean up the bare object and all temporary state, then return NULL
         * so the caller can propagate the error. */
        Tcl_IncrRefCount(bcObj);
        Tcl_DecrRefCount(bcObj);
        bcObj = NULL;
    }

read_fail:
    /* Drop the temporary holds on literals and free the temporary array.       */
    /* TbcxByteCode() has already incref'd the same objects into the ByteCode. */
    if (env.objArrayPtr) {
        if (env.numLitObjects > 0) {
            for (Tcl_Size i = 0; i < env.numLitObjects; i++) {
                if (env.objArrayPtr[i]) {
                    Tcl_DecrRefCount(env.objArrayPtr[i]);
                }
            }
        }
        Tcl_Free(env.objArrayPtr);
    }

    /* env.auxDataArrayPtr: this is a temporary shallow copy of the
     * caller's auxArr.  The clientData payloads are owned by the caller
     * (or, on TbcxByteCode success, by the packed ByteCode via the
     * memcpy in TbcxByteCode) — never by env.auxDataArrayPtr itself.
     * A shallow free here is correct. */
    if (env.auxDataArrayPtr)
        Tcl_Free(env.auxDataArrayPtr);
    if (env.exceptArrayPtr)
        Tcl_Free(env.exceptArrayPtr);
    if (env.codeStart)
        Tcl_Free(env.codeStart);

    return bcObj;
}

static void CompiledLocals(Proc *procPtr, Tcl_Size neededCount) {
    if (!procPtr)
        return;
    if (neededCount <= procPtr->numCompiledLocals)
        return;

    CompiledLocal *first = procPtr->firstLocalPtr;
    CompiledLocal *last  = procPtr->lastLocalPtr;

    for (Tcl_Size i = procPtr->numCompiledLocals; i < neededCount; i++) {
        /* Minimal allocation: struct header + 1 byte for the NUL terminator.
           Unnamed padding locals don't need the full sizeof(CompiledLocal)
           which includes the trailing name[1] flexible array. */
        const size_t   allocSize = offsetof(CompiledLocal, name) + 1u;
        CompiledLocal *cl        = (CompiledLocal *)Tcl_Alloc(allocSize);
        memset(cl, 0, allocSize);
        cl->nameLength = 0;
        cl->name[0]    = '\0';
        cl->frameIndex = i;
        if (!first)
            first = last = cl;
        else {
            last->nextPtr = cl;
            last          = cl;
        }
    }
    procPtr->firstLocalPtr     = first;
    procPtr->lastLocalPtr      = last;
    procPtr->numCompiledLocals = neededCount;
}

/* FixCompiledLocalNames — Copy variable names from a LocalCache into the
 * Proc's CompiledLocal chain entries that are unnamed (nameLength==0).
 *
 * CompiledLocals() creates unnamed padding for body-internal locals.
 * TclCreateProc only names the argument locals.  When Tcl rebuilds the
 * LocalCache from the CompiledLocal chain, unnamed entries produce NULL
 * name slots.  Commands that resolve variables by NAME (dict lappend,
 * dict with, upvar, etc.) then fail with "can't read '': no such variable".
 *
 * This function copies the correct names from the TBCX-deserialized
 * LocalCache (which has all names) into the unnamed CompiledLocal entries,
 * so that Tcl's LocalCache rebuild produces correct name mappings.
 */
static void FixCompiledLocalNames(Proc *procPtr, LocalCache *lc) {
    if (!procPtr || !lc)
        return;
    Tcl_Obj      **names   = (Tcl_Obj **)&lc->varName0;
    Tcl_Size       numVars = lc->numVars;

    CompiledLocal *prev    = NULL;
    CompiledLocal *cl      = procPtr->firstLocalPtr;
    while (cl) {
        if (cl->nameLength > 0) {
            /* already named (argument) — skip */
            prev = cl;
            cl   = cl->nextPtr;
            continue;
        }
        Tcl_Size idx = cl->frameIndex;
        if (idx < 0 || idx >= numVars || !names[idx]) {
            prev = cl;
            cl   = cl->nextPtr;
            continue;
        }
        Tcl_Size    nameLen = 0;
        const char *nm      = Tbcx_GetStringFromObjSafe(names[idx], &nameLen);
        if (nameLen == 0) {
            prev = cl;
            cl   = cl->nextPtr;
            continue;
        }

        /* Reallocate the CompiledLocal to hold the name.
           The flexible array member name[1] needs nameLen+1 bytes. */
        size_t         newSize = offsetof(CompiledLocal, name) + (size_t)nameLen + 1u;
        CompiledLocal *repl    = (CompiledLocal *)Tcl_Alloc(newSize);
        memcpy(repl, cl, offsetof(CompiledLocal, name));
        memcpy(repl->name, nm, (size_t)nameLen);
        repl->name[nameLen] = '\0';
        repl->nameLength    = nameLen;
        /* Splice replacement into the linked list using the tracked
           predecessor (O(1) splice). */
        repl->nextPtr       = cl->nextPtr;
        if (prev)
            prev->nextPtr = repl;
        else
            procPtr->firstLocalPtr = repl;
        if (cl == procPtr->lastLocalPtr)
            procPtr->lastLocalPtr = repl;
        Tcl_Free(cl);
        prev = repl;
        cl   = repl->nextPtr; /* continue iteration from next node */
    }
}

/* ==========================================================================
 * Centralized ByteCode fixup
 *
 * Every code path that links a ByteCode to a Proc must perform the same
 * sequence: set procPtr, refresh epochs, propagate to literal pool,
 * optionally fix the LocalCache.  A single function ensures consistency.
 * ========================================================================== */
void TbcxFixupByteCode(ByteCode *bc, Proc *proc, Tcl_Interp *ip, Namespace *ns, int cacheMode) {
    if (!bc)
        return;
    assert(ip != NULL && "TbcxFixupByteCode requires non-NULL interp");
    TBCX_ASSERT_INTERP_THREAD(ip);
    bc->procPtr = proc;
    RefreshBC(bc, ip, ns);
    FixLiteralPoolProcPtr(bc);

    switch (cacheMode) {
    case TBCX_FIXUP_CACHE_KEEP:
        /* Methods/lambdas: preserve cache with extras area + names fix */
        if (proc) {
            TbcxFixLocalCacheExtras(bc, proc);
            if (bc->localCachePtr)
                FixCompiledLocalNames(proc, bc->localCachePtr);
        }
        break;
    case TBCX_FIXUP_CACHE_DROP:
        /* Procs via CmdProcShim: NULL out cache, free old.
           Tcl rebuilds from CompiledLocal chain on first call. */
        if (bc->localCachePtr) {
            if (proc)
                FixCompiledLocalNames(proc, bc->localCachePtr);
            LocalCache *old   = bc->localCachePtr;
            bc->localCachePtr = NULL;
            if (--old->refCount <= 0) {
                Tcl_Obj **names = (Tcl_Obj **)&old->varName0;
                for (Tcl_Size j = 0; j < old->numVars; j++)
                    if (names[j])
                        Tcl_DecrRefCount(names[j]);
                Tcl_Free(old);
            }
        }
        break;
    case TBCX_FIXUP_CACHE_NONE:
    default:
        /* No cache handling (ReadProc: cache handled later by shim) */
        break;
    }
}

/* ==========================================================================
 * Walks a ByteCode and its literal pool, checking invariants.  Returns
 * TCL_OK if all checks pass.  On failure, sets the interp result with
 * a diagnostic and returns TCL_ERROR.
 * ========================================================================== */
int TbcxVerifyLoadedBC(ByteCode *bc, Tcl_Interp *ip, const char *label) {
    if (!bc) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx verify: %s — NULL ByteCode", label ? label : "?"));
        return TCL_ERROR;
    }
    if (!(bc->flags & TCL_BYTECODE_PRECOMPILED)) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx verify: %s — PRECOMPILED flag not set (flags=0x%x)", label ? label : "?", (unsigned)bc->flags));
        return TCL_ERROR;
    }
    /* Check interpHandle validity.  Preserve a temporary handle for the
     * comparison and release *the temporary*; bc->interpHandle itself is
     * owned by the ByteCode and must not be touched here. */
    Interp   *iPtr = (Interp *)ip;
    TclHandle tmp  = TclHandlePreserve(iPtr->handle);
    if (bc->interpHandle != tmp) {
        /* Different interp binding — diagnostic-only path.  No error. */
    }
    TclHandleRelease(tmp);

    /* Recursively check literal pool */
    if (bc->numLitObjects > 0 && bc->objArrayPtr) {
        for (Tcl_Size i = 0; i < bc->numLitObjects; i++) {
            if (!bc->objArrayPtr[i])
                continue;
            ByteCode *litBC = TbcxGetByteCode(bc->objArrayPtr[i]);
            if (litBC && !litBC->procPtr && bc->procPtr) {
                if (ip)
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx verify: %s — literal %d has NULL procPtr"
                                                       " but parent has non-NULL",
                                                       label ? label : "?", (int)i));
                return TCL_ERROR;
            }
        }
    }
    return TCL_OK;
}

static int ReadMethod(TbcxIn *r, Tcl_Interp *ip, OOShim *os) {
    /* classFqn */
    char    *clsf = NULL;
    uint32_t clsL = 0;
    if (!Tbcx_R_LPString(r, &clsf, &clsL))
        return TCL_ERROR;
    Tcl_Obj *clsFqn = Tcl_NewStringObj(clsf, (Tcl_Size)clsL);
    Tcl_IncrRefCount(clsFqn);
    Tcl_Free(clsf);
    /* kind */
    uint8_t kind = 0;
    if (!Tbcx_R_U8(r, &kind)) {
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    /* name (empty for ctor/dtor) */
    char    *mname = NULL;
    uint32_t mnL   = 0;
    if (!Tbcx_R_LPString(r, &mname, &mnL)) {
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    Tcl_Obj *nameObj = Tcl_NewStringObj(mname, (Tcl_Size)mnL);
    Tcl_IncrRefCount(nameObj);
    Tcl_Free(mname);
    /* args text */
    char    *args = NULL;
    uint32_t aL   = 0;
    if (!Tbcx_R_LPString(r, &args, &aL)) {
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    Tcl_Obj *argsObj = Tcl_NewStringObj(args, (Tcl_Size)aL);
    Tcl_IncrRefCount(argsObj);
    Tcl_Free(args);

    /* Validate class FQN: reject embedded NUL and require absolute form */
    {
        Tcl_Size    clsFqnLen = 0;
        const char *clsFqnStr = Tbcx_GetStringFromObjStrict(ip, clsFqn, &clsFqnLen);
        if (!clsFqnStr || Tbcx_ValidateKeyString(ip, clsFqnStr, clsFqnLen, "method class FQN", 1) != TCL_OK) {
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            return TCL_ERROR;
        }
    }
    /* Validate method name: reject embedded NUL (may be empty for ctor/dtor) */
    if (mnL > 0) {
        Tcl_Size    nameLen = 0;
        const char *nameStr = Tbcx_GetStringFromObjStrict(ip, nameObj, &nameLen);
        if (!nameStr || Tbcx_ValidateKeyString(ip, nameStr, nameLen, "method name", 0) != TCL_OK) {
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            return TCL_ERROR;
        }
    }

    /* Body source text.  Read before the compiled block so we can
     * attach it to the fresh body Tcl_Obj with Tcl_InitStringRep — that
     * restores `info class method -body` / `info object method -body`
     * round-trip fidelity.  Length==0 means the artifact was written
     * without -include-source (the default for tbcx, matching the
     * tclcompiler/tbcload tradition for Tcl AOT output); substitute
     * the diagnostic sentinel so introspection is loud rather than
     * silently empty. */
    char    *bodySrc = NULL;
    uint32_t bodySrcLen = 0;
    if (!Tbcx_R_LPString(r, &bodySrc, &bodySrcLen)) {
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }

    /* compiled block (namespace default: class namespace) + receive numLocals */
    Namespace *clsNs  = (Namespace *)Tbcx_EnsureNamespace(ip, Tcl_GetString(clsFqn));
    uint32_t   nLoc   = 0;
    Tcl_Obj   *bodyBC = Tbcx_ReadBlock(r, ip, clsNs, &nLoc, 1, 0);
    if (!bodyBC) {
        Tcl_Free(bodySrc);
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    Tcl_IncrRefCount(bodyBC);

    /* Attach source text as string rep.  bodyBC->bytes is
     * &tclEmptyString (TclNewObj default), not NULL — so we must
     * Tcl_InvalidateStringRep() first to force Tcl_InitStringRep down
     * its allocate-and-copy branch (bytes==NULL).  The other two
     * branches (empty-string singleton and allocated-nonempty) are
     * "allocate only" and do not memcpy from src.  Neither call
     * touches the internal rep.  See ReadProc Stage 4.5 for the
     * detailed branch analysis. */
    Tcl_InvalidateStringRep(bodyBC);
    if (bodySrcLen > 0) {
        Tcl_InitStringRep(bodyBC, bodySrc, (size_t)bodySrcLen);
    } else {
        Tcl_InitStringRep(bodyBC,
            TBCX_STRIPPED_SOURCE_SENTINEL,
            sizeof(TBCX_STRIPPED_SOURCE_SENTINEL) - 1u);
    }
    Tcl_Free(bodySrc);
    bodySrc = NULL;
    /* Build Proc + compiled locals from argsObj */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr  = bodyBC;
    Tcl_IncrRefCount(bodyBC); /* Proc's own reference */
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size       numA = 0;
        if (Tbcx_BuildLocals(ip, argsObj, &first, &last, &numA) != TCL_OK) {
            Tcl_DecrRefCount(bodyBC); /* Proc's reference */
            Tcl_DecrRefCount(bodyBC); /* local reference  */
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            Tcl_Free((char *)procPtr);
            return TCL_ERROR;
        }
        procPtr->numArgs           = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr     = first;
        procPtr->lastLocalPtr      = last;
    }
    CompiledLocals(procPtr, (Tcl_Size)nLoc);

    /* Link ByteCode back to this Proc and refresh epochs */
    {
        ByteCode *bcPtr = TbcxGetByteCode(bodyBC);
        if (bcPtr) {
            TbcxFixupByteCode(bcPtr, procPtr, ip, clsNs, TBCX_FIXUP_CACHE_KEEP);
        }
    }

    /* Build procbody and register.
     * CRITICAL: The string rep MUST be non-empty.  TclOO's method creation
     * checks Tcl_GetCharLength(bodyObj) and DELETES the method if it's 0.
     * Using ";" (minimal no-op) ensures TclOO creates a real Method, then
     * recognises the procbody internal rep and uses the precompiled bytecode. */
    Tcl_Obj *procBodyObj = Tcl_NewStringObj(";", 1);
    Tcl_IncrRefCount(procBodyObj);
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = procPtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
    procPtr->refCount++;

    Tcl_DString keyDs;
    if (!MethodKeyBuf(&keyDs, clsFqn, kind, (mnL ? nameObj : NULL))) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: invalid string in method key construction", -1));
        /* Release procBodyObj first — its freeIntRep drops
         * procPtr->refCount from 2 to 1.  Then call TclProcCleanupProc
         * (which decrements to 0 and frees the Proc + its locals +
         * releases its own ref on bodyBC).  Finally drop our local
         * bodyBC reference and the rest.  Without this sequence the
         * Proc leaked on every malformed-key input. */
        Tcl_DecrRefCount(procBodyObj); /* procPtr: 2 -> 1 */
        TclProcCleanupProc(procPtr);   /* procPtr: 1 -> 0, frees Proc */
        Tcl_DecrRefCount(bodyBC);      /* local reference */
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        Tcl_DecrRefCount(argsObj);
        return TCL_ERROR;
    }
    int            isNew = 0;
    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs), &isNew);
    Tcl_DStringFree(&keyDs);
    /* Build and validate pair BEFORE touching the old value so the hash
       entry remains valid if list construction fails. */
    Tcl_Obj *pair = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(pair);
    if (Tcl_ListObjAppendElement(ip, pair, argsObj) != TCL_OK || Tcl_ListObjAppendElement(ip, pair, procBodyObj) != TCL_OK) {
        Tcl_DecrRefCount(pair);
        /* Same procPtr ownership discipline as the MethodKeyBuf failure
         * path above: drop procBodyObj (2->1), then cleanup procPtr
         * (1->0), then drop local bodyBC reference. */
        Tcl_DecrRefCount(procBodyObj); /* procPtr: 2 -> 1 */
        TclProcCleanupProc(procPtr);   /* procPtr: 1 -> 0, frees Proc */
        Tcl_DecrRefCount(bodyBC);      /* local reference */
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        Tcl_DecrRefCount(argsObj);
        return TCL_ERROR;
    }
    /* Atomic swap: set new value, then release old */
    Tcl_Obj *oldPair = isNew ? NULL : (Tcl_Obj *)Tcl_GetHashValue(he);
    Tcl_SetHashValue(he, pair);
    if (oldPair)
        Tcl_DecrRefCount(oldPair);
    Tcl_DecrRefCount(procBodyObj);
    Tcl_DecrRefCount(bodyBC); /* local reference */
    Tcl_DecrRefCount(nameObj);
    Tcl_DecrRefCount(clsFqn);
    Tcl_DecrRefCount(argsObj);
    return TCL_OK;
}

/* ---- Extracted literal readers ----
 * Each handles one tag from the .tbcx literal stream.  Returns a refcount-0
 * Tcl_Obj* on success or NULL on error. */

static Tcl_Obj *ReadLit_Bignum(TbcxIn *r) {
    uint8_t  sign   = 0;
    uint32_t magLen = 0;
    if (!Tbcx_R_U8(r, &sign))
        return NULL;
    if (!Tbcx_R_U32(r, &magLen))
        return NULL;
    if (magLen > (64u * 1024u * 1024u)) {
        R_Error(r, "tbcx: bignum too large");
        return NULL;
    }
    Tcl_Obj *o = NULL;
    if (magLen == 0 || sign == 0) {
        o = Tcl_NewWideIntObj(0);
    } else {
        unsigned char *le = (unsigned char *)Tcl_AttemptAlloc(magLen);
        if (!le) {
            R_Error(r, "tbcx: allocation failed (bignum)");
            return NULL;
        }
        if (!Tbcx_R_Bytes(r, le, magLen)) {
            Tcl_Free((char *)le);
            return NULL;
        }
        mp_int z;
        mp_err mrc = TclBN_mp_init(&z);
        if (mrc != MP_OKAY) {
            Tcl_Free((char *)le);
            R_Error(r, "tbcx: bignum init");
            return NULL;
        }
        /* Import LE magnitude bytes into mp_int via repeated shift+add.
         * Tcl 9.1's stubs table does not expose mp_from_ubin, so we use
         * the portable TclBN_mp_mul_2d + TclBN_mp_add_d path. */
        for (int i = (int)magLen - 1; i >= 0; i--) {
            if ((mrc = TclBN_mp_mul_2d(&z, 8, &z)) != MP_OKAY)
                break;
            if ((mrc = TclBN_mp_add_d(&z, le[i], &z)) != MP_OKAY)
                break;
        }
        if (mrc != MP_OKAY) {
            Tcl_Free((char *)le);
            TclBN_mp_clear(&z);
            R_Error(r, "tbcx: bignum import");
            return NULL;
        }
        if (sign == 2) {
            mrc = TclBN_mp_neg(&z, &z);
            if (mrc != MP_OKAY) {
                Tcl_Free((char *)le);
                TclBN_mp_clear(&z);
                R_Error(r, "tbcx: bignum neg");
                return NULL;
            }
        }
        o = Tcl_NewBignumObj(&z);
        Tcl_Free((char *)le);
    }
    return o;
}

static Tcl_Obj *ReadLit_LambdaBC(TbcxIn *r, Tcl_Interp *ip, int depth, int dumpOnly) {
    char    *nsStr = NULL;
    uint32_t nsLen = 0;
    if (!Tbcx_R_LPString(r, &nsStr, &nsLen))
        return NULL;
    Tcl_Obj *nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
    Tcl_Free(nsStr);
    Tcl_IncrRefCount(nsObj);

    /* Validate namespace string: reject embedded NUL and require absolute form */
    {
        Tcl_Size    nsObjLen = 0;
        const char *nsObjStr = Tbcx_GetStringFromObjStrict(ip, nsObj, &nsObjLen);
        if (!nsObjStr || Tbcx_ValidateKeyString(ip, nsObjStr, nsObjLen, "lambda namespace", 1) != TCL_OK) {
            Tcl_DecrRefCount(nsObj);
            return NULL;
        }
    }

    Namespace *nsPtr;
    if (dumpOnly) {
        nsPtr = (Namespace *)Tcl_FindNamespace(ip, Tcl_GetString(nsObj), NULL, 0);
        if (!nsPtr)
            nsPtr = (Namespace *)Tcl_GetGlobalNamespace(ip);
    } else {
        nsPtr = (Namespace *)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
        if (!nsPtr) {
            Tcl_DecrRefCount(nsObj);
            return NULL;
        }
    }

    /* Read argument specifications */
    uint32_t numArgs = 0;
    if (!Tbcx_R_U32(r, &numArgs)) {
        Tcl_DecrRefCount(nsObj);
        return NULL;
    }
    if (numArgs > TBCX_MAX_LOCALS) {
        R_Error(r, "tbcx: too many lambda arguments");
        Tcl_DecrRefCount(nsObj);
        return NULL;
    }
    Tcl_Obj *argList = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(argList);
    for (uint32_t i = 0; i < numArgs; i++) {
        char    *argName    = NULL;
        uint32_t argNameLen = 0;
        if (!Tbcx_R_LPString(r, &argName, &argNameLen)) {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argList);
            return NULL;
        }
        Tcl_Obj *argNameObj = Tcl_NewStringObj(argName, (Tcl_Size)argNameLen);
        Tcl_IncrRefCount(argNameObj);
        Tcl_Free(argName);
        uint8_t hasDef = 0;
        if (!Tbcx_R_U8(r, &hasDef)) {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argNameObj);
            Tcl_DecrRefCount(argList);
            return NULL;
        }
        if (hasDef) {
            Tcl_Obj *defVal = ReadLiteral(r, ip, depth + 1, dumpOnly);
            if (!defVal) {
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argNameObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
            /* Own a reference so all error paths can release safely.
             * ReadLiteral returns refcount-0; incref → 1 (owned by this scope). */
            Tcl_IncrRefCount(defVal);
            Tcl_Obj *argPair = Tcl_NewListObj(0, NULL);
            Tcl_IncrRefCount(argPair);
            if (Tcl_ListObjAppendElement(ip, argPair, argNameObj) != TCL_OK || Tcl_ListObjAppendElement(ip, argPair, defVal) != TCL_OK || Tcl_ListObjAppendElement(ip, argList, argPair) != TCL_OK) {
                Tcl_DecrRefCount(argPair);
                Tcl_DecrRefCount(defVal);
                Tcl_DecrRefCount(argNameObj);
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
            Tcl_DecrRefCount(argPair);
            Tcl_DecrRefCount(defVal); /* list now owns its reference */
            Tcl_DecrRefCount(argNameObj);
        } else {
            if (Tcl_ListObjAppendElement(ip, argList, argNameObj) != TCL_OK) {
                Tcl_DecrRefCount(argNameObj);
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
            Tcl_DecrRefCount(argNameObj);
        }
    }

    /* Build Proc with precompiled bytecode */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size       numA = 0;
        if (Tbcx_BuildLocals(ip, argList, &first, &last, &numA) != TCL_OK) {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argList);
            Tcl_Free((char *)procPtr);
            return NULL;
        }
        procPtr->numArgs           = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr     = first;
        procPtr->lastLocalPtr      = last;
    }

    uint32_t nLocalsBody = 0;
    Tcl_Obj *bodyBC      = Tbcx_ReadBlock(r, ip, nsPtr, &nLocalsBody, 1, dumpOnly);
    if (!bodyBC) {
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(argList);
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char *)procPtr);
        return NULL;
    }
    Tcl_IncrRefCount(bodyBC); /* own immediately — don't leave at refcount 0 */
    procPtr->bodyPtr = bodyBC;
    Tcl_IncrRefCount(bodyBC); /* Proc's own reference */
    {
        ByteCode *bc = TbcxGetByteCode(bodyBC);
        if (bc)
            TbcxFixupByteCode(bc, procPtr, ip, nsPtr, TBCX_FIXUP_CACHE_KEEP);
    }
    CompiledLocals(procPtr, (Tcl_Size)nLocalsBody);

    /* Read original body source text */
    char    *bodySrc    = NULL;
    uint32_t bodySrcLen = 0;
    if (!Tbcx_R_LPString(r, &bodySrc, &bodySrcLen)) {
        ByteCode *bc2 = NULL;
        bc2           = TbcxGetByteCode(bodyBC);
        if (bc2)
            bc2->procPtr = NULL;
        Tcl_DecrRefCount(bodyBC); /* Proc's reference */
        Tcl_DecrRefCount(bodyBC); /* local reference  */
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char *)procPtr);
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(argList);
        return NULL;
    }

    /* Build lambda Tcl_Obj with original body source in string rep */
    Tcl_Obj *lambda;
    {
        const char *nsName   = Tbcx_GetStringSafe(nsObj);
        int         isGlobal = (nsName[0] == ':' && nsName[1] == ':' && nsName[2] == '\0');
        Tcl_Obj    *bodyText = Tcl_NewStringObj(bodySrc, (Tcl_Size)bodySrcLen);
        Tcl_Free(bodySrc);
        if (isGlobal) {
            Tcl_Obj *elems[2] = {argList, bodyText};
            lambda            = Tcl_NewListObj(2, elems);
        } else {
            Tcl_Obj *elems[3] = {argList, bodyText, nsObj};
            lambda            = Tcl_NewListObj(3, elems);
        }
        (void)Tbcx_GetStringSafe(lambda);
    }

    /* Register in ApplyShim for shimmer recovery (skip in dump mode) */
    if (!dumpOnly) {
        RegisterPrecompiledLambda(ip, lambda, procPtr, nsObj);
        Tcl_DecrRefCount(bodyBC); /* local reference */
    } else {
        ByteCode *bc2 = NULL;
        bc2           = TbcxGetByteCode(bodyBC);
        if (bc2)
            bc2->procPtr = NULL;
        Tcl_DecrRefCount(bodyBC); /* Proc's reference */
        Tcl_DecrRefCount(bodyBC); /* local reference  */
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char *)procPtr);
    }

    Tcl_DecrRefCount(argList);
    Tcl_DecrRefCount(nsObj);
    return lambda;
}

static Tcl_Obj *ReadLiteral(TbcxIn *r, Tcl_Interp *ip, int depth, int dumpOnly) {
    if (depth > TBCX_MAX_LITERAL_DEPTH) {
        R_Error(r, "tbcx: literal nesting too deep");
        return NULL;
    }

    uint32_t tag = 0;
    if (!Tbcx_R_U32(r, &tag))
        return NULL;

    switch (tag) {
    case TBCX_LIT_BIGNUM:
        return ReadLit_Bignum(r);
    case TBCX_LIT_BOOLEAN: {
        uint8_t b = 0;
        if (!Tbcx_R_U8(r, &b))
            return NULL;
        return Tcl_NewBooleanObj(b ? 1 : 0);
    }
    case TBCX_LIT_BYTEARR: {
        uint32_t n = 0;
        if (!Tbcx_R_U32(r, &n))
            return NULL;
        if (n > TBCX_MAX_STR) {
            R_Error(r, "tbcx: bytearray too large");
            return NULL;
        }
        unsigned char *buf = (unsigned char *)Tcl_AttemptAlloc(n ? n : 1u);
        if (!buf) {
            R_Error(r, "tbcx: allocation failed (bytearray literal)");
            return NULL;
        }
        if (n && !Tbcx_R_Bytes(r, buf, n)) {
            Tcl_Free((char *)buf);
            return NULL;
        }
        Tcl_Obj *o = Tcl_NewByteArrayObj(buf, n);
        Tcl_Free((char *)buf);
        return o;
    }
    case TBCX_LIT_DICT: {
        uint32_t cnt = 0;
        if (!Tbcx_R_U32(r, &cnt))
            return NULL;
        if (cnt > TBCX_MAX_CONTAINER_ELEMS) {
            R_Error(r, "tbcx: dict too many pairs");
            return NULL;
        }
        Tcl_Obj *d = Tcl_NewDictObj();
        for (uint32_t i = 0; i < cnt; i++) {
            Tcl_Obj *k = ReadLiteral(r, ip, depth + 1, dumpOnly);
            if (!k) {
                Tcl_IncrRefCount(d);
                Tcl_DecrRefCount(d);
                return NULL;
            }
            Tcl_Obj *v = ReadLiteral(r, ip, depth + 1, dumpOnly);
            if (!v) {
                Tcl_IncrRefCount(k);
                Tcl_DecrRefCount(k);
                Tcl_IncrRefCount(d);
                Tcl_DecrRefCount(d);
                return NULL;
            }
            if (Tcl_DictObjPut(ip, d, k, v) != TCL_OK) {
                Tcl_IncrRefCount(k);
                Tcl_DecrRefCount(k);
                Tcl_IncrRefCount(v);
                Tcl_DecrRefCount(v);
                Tcl_IncrRefCount(d);
                Tcl_DecrRefCount(d);
                return NULL;
            }
        }
        return d;
    }
    case TBCX_LIT_DOUBLE: {
        uint64_t bits = 0;
        if (!Tbcx_R_U64(r, &bits))
            return NULL;
        union {
            uint64_t u;
            double   d;
        } u;
        u.u = bits;
        return Tcl_NewDoubleObj(u.d);
    }
    case TBCX_LIT_LIST: {
        uint32_t n = 0;
        if (!Tbcx_R_U32(r, &n))
            return NULL;
        if (n > TBCX_MAX_CONTAINER_ELEMS) {
            R_Error(r, "tbcx: list too many elements");
            return NULL;
        }
        Tcl_Obj *lst = Tcl_NewListObj(0, NULL);
        for (uint32_t i = 0; i < n; i++) {
            Tcl_Obj *e = ReadLiteral(r, ip, depth + 1, dumpOnly);
            if (!e) {
                Tcl_IncrRefCount(lst);
                Tcl_DecrRefCount(lst);
                return NULL;
            }
            if (Tcl_ListObjAppendElement(ip, lst, e) != TCL_OK) {
                /* e has refCount 0 — free it before freeing lst. */
                Tcl_IncrRefCount(e);
                Tcl_DecrRefCount(e);
                Tcl_IncrRefCount(lst);
                Tcl_DecrRefCount(lst);
                return NULL;
            }
        }
        return lst;
    }
    case TBCX_LIT_WIDEINT: {
        uint64_t u = 0;
        if (!Tbcx_R_U64(r, &u))
            return NULL;
        /* stored as 2's complement */
        Tcl_WideInt wi = (Tcl_WideInt)u;
        return Tcl_NewWideIntObj(wi);
    }
    case TBCX_LIT_WIDEUINT: {
        uint64_t u = 0;
        if (!Tbcx_R_U64(r, &u))
            return NULL;
        if (u <= (uint64_t)TCL_INDEX_NONE) {
            return Tcl_NewWideIntObj((Tcl_WideInt)u);
        } else {
            /* promote to bignum */
            mp_int z;
            mp_err mrc = TclBN_mp_init(&z);
            if (mrc != MP_OKAY) {
                R_Error(r, "tbcx: wideuint init");
                return NULL;
            }
            /* Portable path: build from 8 bytes with shift/add so we can
               keep checking mp_err on each TomMath call. */
            for (int i = 7; i >= 0; i--) {
                mrc = TclBN_mp_mul_2d(&z, 8, &z);
                if (mrc != MP_OKAY)
                    break;
                mrc = TclBN_mp_add_d(&z, (unsigned int)((u >> (8 * i)) & 0xFFu), &z);
                if (mrc != MP_OKAY)
                    break;
            }
            if (mrc != MP_OKAY) {
                TclBN_mp_clear(&z);
                R_Error(r, "tbcx: wideuint import");
                return NULL;
            }
            Tcl_Obj *o = Tcl_NewBignumObj(&z);
            return o;
        }
    }
    case TBCX_LIT_BYTESRC: {
        /* source text + nsFQN + compiled block.
           The source text becomes the Tcl_Obj's string rep so Tcl can
           recompile from source when evaluated in a different interpreter
           (e.g. interp eval) or after a compile-epoch bump (ProcShim).
           PRECOMPILED is NOT set — this allows Tcl to gracefully
           recompile instead of erroring with "jumped interps". */
        char    *srcStr = NULL;
        uint32_t srcLen = 0;
        if (!Tbcx_R_LPString(r, &srcStr, &srcLen))
            return NULL;
        char    *nsStr = NULL;
        uint32_t nsLen = 0;
        if (!Tbcx_R_LPString(r, &nsStr, &nsLen)) {
            Tcl_Free(srcStr);
            return NULL;
        }
        Tcl_Obj *nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
        Tcl_IncrRefCount(nsObj);
        Tcl_Free(nsStr);
        {
            Tcl_Size    nsObjLen = 0;
            const char *nsObjStr = Tbcx_GetStringFromObjStrict(ip, nsObj, &nsObjLen);
            if (!nsObjStr || Tbcx_ValidateKeyString(ip, nsObjStr, nsObjLen, "bytesrc literal namespace", 1) != TCL_OK) {
                Tcl_DecrRefCount(nsObj);
                Tcl_Free(srcStr);
                return NULL;
            }
        }
        Namespace *nsPtr;
        if (dumpOnly)
            nsPtr = (Namespace *)Tcl_FindNamespace(ip, Tcl_GetString(nsObj), NULL, 0);
        else
            nsPtr = (Namespace *)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
        if (!nsPtr) {
            if (dumpOnly)
                nsPtr = (Namespace *)Tcl_GetGlobalNamespace(ip);
            else {
                R_Error(r, "tbcx: namespace creation failed for bytesrc literal");
                Tcl_DecrRefCount(nsObj);
                Tcl_Free(srcStr);
                return NULL;
            }
        }
        uint32_t dummyNL = 0;
        /* setPrecompiled=0: BYTESRC literals have source text, so Tcl
           CAN recompile from the string rep.  Without PRECOMPILED, Tcl
           will recompile on compile-epoch mismatch (after ProcShim bumps
           the epoch) or interpHandle mismatch (child interp / interp eval).
           This is correct — the bytecode is a cache, the source is the
           ground truth.  With PRECOMPILED=1, Tcl would error with
           "compiled script jumped interps" on interpHandle mismatch
           instead of gracefully recompiling. */
        Tcl_Obj *bc      = Tbcx_ReadBlock(r, ip, nsPtr, &dummyNL, 0, dumpOnly);
        Tcl_DecrRefCount(nsObj);
        if (bc && srcLen > 0) {
            /* Replace the empty string rep with the preserved source text.
               The bytecode internal rep is untouched.  When evaluated in a
               different interpreter (interp eval), Tcl detects the interp
               mismatch and recompiles from this source text, avoiding the
               "compiled script jumped interps" error. */
            Tcl_InvalidateStringRep(bc);
            Tcl_InitStringRep(bc, srcStr, (Tcl_Size)srcLen);
        }
        Tcl_Free(srcStr);
        return bc;
    }
    case TBCX_LIT_LAMBDA_BC:
        return ReadLit_LambdaBC(r, ip, depth, dumpOnly);
    case TBCX_LIT_STRING: {
        char    *s = NULL;
        uint32_t n = 0;
        if (!Tbcx_R_LPString(r, &s, &n))
            return NULL;
        Tcl_Obj *o = Tcl_NewStringObj(s, (Tcl_Size)n);
        Tcl_Free(s);
        return o;
    }
    default:
        R_Error(r, "tbcx: unknown literal tag");
        return NULL;
    }
}

/* FreeAuxArrayOwned — deep-free an AuxData array, invoking each entry's
 * type->freeProc on its clientData before releasing the array itself.
 *
 * Shallow `Tcl_Free(arr)` on a post-ReadAuxArray failure path leaks every
 * clientData payload (hash tables for JumptableInfo/JumptableNumInfo,
 * local-index arrays for DictUpdateInfo, ForeachInfo struct).  Use this
 * helper on every failure path BEFORE ownership transfers into the
 * packed ByteCode via ByteCodeObj(). */
static void FreeAuxArrayOwned(AuxData *arr, uint32_t n) {
    if (!arr)
        return;
    for (uint32_t i = 0; i < n; i++) {
        if (arr[i].type && arr[i].type->freeProc && arr[i].clientData) {
            arr[i].type->freeProc(arr[i].clientData);
            arr[i].clientData = NULL;
        }
    }
    Tcl_Free(arr);
}

static int ReadAuxArray(TbcxIn *r, AuxData **auxOut, uint32_t *numAuxOut) {
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_AUX) {
        R_Error(r, "tbcx: aux too many");
        return 0;
    }
    AuxData *arr = NULL;
    if (n) {
        size_t auxBytes = 0;
        if (!tbcx_checked_mul(sizeof(AuxData), n, &auxBytes)) {
            R_Error(r, "tbcx: aux array size overflow");
            return 0;
        }
        arr = (AuxData *)Tcl_AttemptAlloc(auxBytes);
        if (!arr) {
            R_Error(r, "tbcx: allocation failed (aux array)");
            return 0;
        }
    }

    uint32_t i = 0; /* declared here so fail_aux can reference it */
    for (i = 0; i < n; i++) {
        uint32_t tag = 0;
        if (!Tbcx_R_U32(r, &tag))
            goto fail_aux;

        if (tag == TBCX_AUX_JT_STR) {
            /* u32 cnt, then cnt × (LPString key, u32 pcOffset) */
            uint32_t cnt = 0;
            if (!Tbcx_R_U32(r, &cnt))
                goto fail_aux;
            if (cnt > TBCX_MAX_LITERALS) {
                R_Error(r, "tbcx: jump table too large");
                goto fail_aux;
            }
            JumptableInfo *info = (JumptableInfo *)Tcl_AttemptAlloc(sizeof(*info));
            if (!info) {
                R_Error(r, "tbcx: allocation failed (jumptable)");
                goto fail_aux;
            }
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
            for (uint32_t k = 0; k < cnt; k++) {
                char    *s  = NULL;
                uint32_t sl = 0;
                if (!Tbcx_R_LPString(r, &s, &sl)) {
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                uint32_t off = 0;
                if (!Tbcx_R_U32(r, &off)) {
                    Tcl_Free(s);
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                /* For TCL_STRING_KEYS, Tcl duplicates the key we pass in. */
                int            newEntry = 0;
                Tcl_HashEntry *he       = Tcl_CreateHashEntry(&info->hashTable, s, &newEntry);
                Tcl_SetHashValue(he, INT2PTR((int)off));
                Tcl_Free(s); /* safe: table holds its own duplicate */
            }
            arr[i].type       = tbcxAuxJTStr;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_JT_NUM) {
            uint32_t cnt = 0;
            if (!Tbcx_R_U32(r, &cnt))
                goto fail_aux;
            if (cnt > TBCX_MAX_LITERALS) {
                R_Error(r, "tbcx: numeric jump table too large");
                goto fail_aux;
            }
            JumptableNumInfo *info = (JumptableNumInfo *)Tcl_AttemptAlloc(sizeof(*info));
            if (!info) {
                R_Error(r, "tbcx: allocation failed (jumptable_num)");
                goto fail_aux;
            }
#if UINTPTR_MAX < 0xFFFFFFFFFFFFFFFFull
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
#else
            Tcl_InitHashTable(&info->hashTable, TCL_ONE_WORD_KEYS);
#endif
            for (uint32_t k = 0; k < cnt; k++) {
                uint64_t key = 0;
                uint32_t off = 0;

                if (!Tbcx_R_U64(r, &key) || !Tbcx_R_U32(r, &off)) {
                    /* cleanup on short read */
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                int            newE = 0;
                Tcl_HashEntry *he;
#if UINTPTR_MAX < 0xFFFFFFFFFFFFFFFFull
                {
                    Tcl_Obj *keyObj = Tcl_ObjPrintf("%" PRIu64, key);
                    Tcl_IncrRefCount(keyObj);
                    he = Tcl_CreateHashEntry(&info->hashTable, Tbcx_GetStringSafe(keyObj), &newE);
                    Tcl_DecrRefCount(keyObj);
                }
#else
                he = Tcl_CreateHashEntry(&info->hashTable, (const char *)(intptr_t)key, &newE);
#endif
                Tcl_SetHashValue(he, INT2PTR((int)off));
            }
            arr[i].type       = tbcxAuxJTNum;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_DICTUPD) {
            uint32_t L = 0;
            if (!Tbcx_R_U32(r, &L))
                goto fail_aux;
            if (L > TBCX_MAX_LITERALS) {
                R_Error(r, "tbcx: dict-update aux too large");
                goto fail_aux;
            }
            size_t elemBytes = (size_t)L * sizeof(Tcl_Size);
            /* Overflow check: verify the multiplication didn't wrap */
            if (L > 0 && elemBytes / L != sizeof(Tcl_Size)) {
                R_Error(r, "tbcx: dict-update aux overflow");
                goto fail_aux;
            }
            size_t          bytes = offsetof(DictUpdateInfo, varIndices) + elemBytes;
            DictUpdateInfo *info  = (DictUpdateInfo *)Tcl_AttemptAlloc(bytes);
            if (!info) {
                R_Error(r, "tbcx: allocation failed (dictupdate)");
                goto fail_aux;
            }
            memset(info, 0, bytes);
            info->length = (Tcl_Size)L;
            for (uint32_t k = 0; k < L; k++) {
                uint32_t x = 0;
                if (!Tbcx_R_U32(r, &x)) {
                    Tcl_Free(info);
                    goto fail_aux;
                }
                info->varIndices[k] = (Tcl_Size)x;
            }
            arr[i].type       = tbcxAuxDictUpdate;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_NEWFORE) {
            if (!tbcxAuxNewForeach) {
                R_Error(r, "tbcx: NewForeachInfo AuxData type not available in this Tcl build");
                goto fail_aux;
            }

            uint32_t numLists = 0, loopCtU = 0, firstValU = 0, dupNumLists = 0;
            if (!Tbcx_R_U32(r, &numLists) || !Tbcx_R_U32(r, &loopCtU) || !Tbcx_R_U32(r, &firstValU) || !Tbcx_R_U32(r, &dupNumLists))
                goto fail_aux;
            if (dupNumLists != numLists) {
                R_Error(r, "tbcx: foreach aux mismatch");
                goto fail_aux;
            }
            if (numLists > TBCX_MAX_LITERALS) {
                R_Error(r, "tbcx: foreach aux too many lists");
                goto fail_aux;
            }
            size_t listBytes = (size_t)numLists * sizeof(ForeachVarList *);
            if (numLists > 0 && listBytes / numLists != sizeof(ForeachVarList *)) {
                R_Error(r, "tbcx: foreach aux overflow");
                goto fail_aux;
            }
            size_t       bytes = offsetof(ForeachInfo, varLists) + listBytes;
            ForeachInfo *info  = (ForeachInfo *)Tcl_AttemptAlloc(bytes);
            if (!info) {
                R_Error(r, "tbcx: allocation failed (foreach)");
                goto fail_aux;
            }
            memset(info, 0, bytes);
            info->numLists       = (Tcl_Size)numLists;
            info->firstValueTemp = (Tcl_LVTIndex)(int32_t)firstValU;
            info->loopCtTemp     = (Tcl_LVTIndex)(int32_t)loopCtU;
            for (uint32_t iL = 0; iL < numLists; iL++) {
                uint32_t nv = 0;
                if (!Tbcx_R_U32(r, &nv)) {
                    arr[i].type       = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++; /* count this entry so fail_aux frees it */
                    goto fail_aux;
                }
                if (nv > TBCX_MAX_LITERALS) {
                    R_Error(r, "tbcx: foreach varlist too large");
                    arr[i].type       = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++;
                    goto fail_aux;
                }
                size_t varIdxBytes = (size_t)nv * sizeof(Tcl_LVTIndex);
                if (nv > 0 && varIdxBytes / nv != sizeof(Tcl_LVTIndex)) {
                    R_Error(r, "tbcx: foreach varlist overflow");
                    arr[i].type       = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++;
                    goto fail_aux;
                }
                size_t          vlBytes = offsetof(ForeachVarList, varIndexes) + varIdxBytes;
                ForeachVarList *vl      = (ForeachVarList *)Tcl_AttemptAlloc(vlBytes);
                if (!vl) {
                    R_Error(r, "tbcx: allocation failed (foreach varlist)");
                    arr[i].type       = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++; /* count this entry so fail_aux frees it */
                    goto fail_aux;
                }
                memset(vl, 0, vlBytes);
                vl->numVars = (Tcl_Size)nv;
                for (uint32_t j = 0; j < nv; j++) {
                    uint32_t idx = 0;
                    if (!Tbcx_R_U32(r, &idx)) {
                        Tcl_Free(vl);
                        arr[i].type       = tbcxAuxNewForeach;
                        arr[i].clientData = info;
                        i++;
                        goto fail_aux;
                    }
                    vl->varIndexes[j] = (Tcl_LVTIndex)idx;
                }
                info->varLists[iL] = vl;
            }
            arr[i].type       = tbcxAuxNewForeach;
            arr[i].clientData = info;
        } else {
            R_Error(r, "tbcx: unsupported AuxData tag");
            goto fail_aux;
        }
    }
    *auxOut    = arr;
    *numAuxOut = n;
    return 1;

fail_aux:
    /* Free already-completed AuxData entries [0..i) via their type's freeProc */
    for (uint32_t j = 0; j < i; j++) {
        if (arr[j].type && arr[j].type->freeProc && arr[j].clientData)
            arr[j].type->freeProc(arr[j].clientData);
    }
    if (arr)
        Tcl_Free(arr);
    return 0;
}

static int ReadExceptions(TbcxIn *r, ExceptionRange **exOut, uint32_t *numOut) {
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_EXCEPT) {
        R_Error(r, "tbcx: too many exceptions");
        return 0;
    }
    ExceptionRange *arr = NULL;
    if (n) {
        size_t exBytes = 0;
        if (!tbcx_checked_mul(sizeof(ExceptionRange), n, &exBytes)) {
            R_Error(r, "tbcx: exception array size overflow");
            return 0;
        }
        arr = (ExceptionRange *)Tcl_AttemptAlloc(exBytes);
        if (!arr) {
            R_Error(r, "tbcx: allocation failed (exception array)");
            return 0;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t  type8   = 0;
        uint32_t len     = 0;
        uint32_t nesting = 0, from = 0, cont = 0, brk = 0, cat = 0;
        if (!Tbcx_R_U8(r, &type8) || !Tbcx_R_U32(r, &nesting) || !Tbcx_R_U32(r, &from) || !Tbcx_R_U32(r, &len) || !Tbcx_R_U32(r, &cont) || !Tbcx_R_U32(r, &brk) || !Tbcx_R_U32(r, &cat)) {
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
        /* Reject out-of-enum exception-range types from the wire.
         * Tcl's evaluator treats everything that is not CATCH as
         * loop-like control metadata, so a crafted `.tbcx` with e.g.
         * type8 = 99 could alter break/continue unwinding behaviour
         * even when offsets are in range. */
        if (type8 != (uint8_t)LOOP_EXCEPTION_RANGE &&
            type8 != (uint8_t)CATCH_EXCEPTION_RANGE) {
            R_Error(r, "tbcx: invalid exception range type");
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
        arr[i].type           = (ExceptionRangeType)type8;
        /* Store in Tcl_Size domain (matches Tcl 9.1 headers).
         * Preserve -1 sentinel encoded as 0xFFFFFFFF on the wire. */
        arr[i].nestingLevel   = (Tcl_Size)nesting;
        arr[i].codeOffset     = (Tcl_Size)from;
        arr[i].numCodeBytes   = (Tcl_Size)len;
        arr[i].continueOffset = (cont == 0xFFFFFFFFu) ? (Tcl_Size)-1 : (Tcl_Size)cont;
        arr[i].breakOffset    = (brk == 0xFFFFFFFFu) ? (Tcl_Size)-1 : (Tcl_Size)brk;
        arr[i].catchOffset    = (cat == 0xFFFFFFFFu) ? (Tcl_Size)-1 : (Tcl_Size)cat;
        /* Tighten the metadata shape: LOOP ranges must not carry a catch
         * offset, CATCH ranges must not carry break/continue offsets. */
        if (arr[i].type == LOOP_EXCEPTION_RANGE) {
            if (arr[i].catchOffset != (Tcl_Size)-1) {
                R_Error(r, "tbcx: LOOP exception range with non-sentinel catchOffset");
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
        } else {
            if (arr[i].breakOffset != (Tcl_Size)-1 ||
                arr[i].continueOffset != (Tcl_Size)-1) {
                R_Error(r, "tbcx: CATCH exception range with non-sentinel break/continueOffset");
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
        }
    }
    *exOut  = arr;
    *numOut = n;
    return 1;
}

/* ValidateBlockOperands — semantic validation pass over the raw instruction
 * stream from the wire.  Runs after the literal/aux/exception sections have
 * been parsed and sized, BEFORE TbcxByteCode() hands the block to Tcl's
 * evaluator.
 *
 * Validates:
 *   1) opcode byte <= LAST_INST_OPCODE;
 *   2) each instruction is fully contained within [0, codeLen);
 *   3) every LIT1/LIT4 operand is < numLits;
 *   4) every LVT1/LVT4 operand is < numLocals;
 *   5) every AUX4 operand is < numAux;
 *   6) every OFFSET1/OFFSET4 operand produces a target in [0, codeLen)
 *      AND lands on a recorded instruction boundary;
 *   7) every JumptableInfo/JumptableNumInfo hash entry's stored offset
 *      satisfies the same in-range + on-boundary constraint.
 *
 * Without this pass, a crafted .tbcx with e.g. INST_PUSH <huge> can cause
 * an out-of-bounds read of codePtr->objArrayPtr at runtime inside Tcl's
 * evaluator.  The evaluator itself does not revalidate operands (by
 * design — the compiler is trusted), so revalidation MUST happen at
 * load time for untrusted artifacts.
 *
 * Returns TCL_OK on success; on failure, sets an interpreter result
 * describing the offending instruction and returns TCL_ERROR. */
static int ValidateBlockOperands(
    Tcl_Interp *ip,
    const unsigned char *code, uint32_t codeLen,
    uint32_t numLits, uint32_t numAux, uint32_t numLocals,
    AuxData *auxArr)
{
    const InstructionDesc *instTable = (const InstructionDesc *)TclGetInstructionTable();
    unsigned char *boundary = NULL;

    /* Empty blocks are trivially valid. */
    if (codeLen == 0) {
        return TCL_OK;
    }

    boundary = (unsigned char *)Tcl_AttemptAlloc((size_t)codeLen);
    if (!boundary) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (boundary map)", -1));
        return TCL_ERROR;
    }
    memset(boundary, 0, (size_t)codeLen);

    /* Pass 1: walk instruction stream, validate opcode and length,
     * populate boundary map. */
    uint32_t pc = 0;
    while (pc < codeLen) {
        unsigned int op = code[pc];
        if (op > LAST_INST_OPCODE) {
            Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                "tbcx: invalid opcode 0x%02x at pc %u", op, pc));
            Tcl_Free(boundary);
            return TCL_ERROR;
        }
        const InstructionDesc *desc = &instTable[op];
        if (desc->numBytes <= 0 ||
            (uint32_t)desc->numBytes > codeLen ||
            pc + (uint32_t)desc->numBytes > codeLen) {
            Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                "tbcx: truncated instruction %s (opcode 0x%02x) at pc %u",
                desc->name ? desc->name : "?", op, pc));
            Tcl_Free(boundary);
            return TCL_ERROR;
        }
        boundary[pc] = 1;
        pc += (uint32_t)desc->numBytes;
    }
    if (pc != codeLen) {
        /* Final instruction ran past end; already reported above, but
         * guard defensively. */
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: code stream does not end on instruction boundary", -1));
        Tcl_Free(boundary);
        return TCL_ERROR;
    }

    /* Pass 2: walk operands per instruction and validate each by
     * InstOperandType.  Use the core's InstructionDesc.opTypes array
     * rather than opcode-name matching so new opcodes are handled
     * automatically. */
    pc = 0;
    while (pc < codeLen) {
        unsigned int op = code[pc];
        const InstructionDesc *desc = &instTable[op];
        uint32_t off = 1; /* skip the opcode byte */
        for (int oi = 0; oi < desc->numOperands; oi++) {
            InstOperandType ot = desc->opTypes[oi];
            switch (ot) {
            case OPERAND_NONE:
                break;
            case OPERAND_INT1:
            case OPERAND_UINT1:
            case OPERAND_SCLS1:
            case OPERAND_UNSF1:
            case OPERAND_CLK1:
            case OPERAND_LRPL1:
                /* 1-byte immediate / flag / small-table-index operands.
                 * No range check needed — the payload is just a bit
                 * pattern interpreted by the opcode itself. */
                off += 1;
                break;
            case OPERAND_LVT1: {
                unsigned int idx = TclGetUInt1AtPtr(code + pc + off);
                if (idx >= numLocals) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: local index %u >= %u at pc %u",
                        desc->name, idx, numLocals, pc));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 1;
                break;
            }
            case OPERAND_LIT1: {
                unsigned int idx = TclGetUInt1AtPtr(code + pc + off);
                if (idx >= numLits) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: literal index %u >= %u at pc %u",
                        desc->name, idx, numLits, pc));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 1;
                break;
            }
            case OPERAND_OFFSET1: {
                int rel = TclGetInt1AtPtr(code + pc + off);
                int64_t tgt = (int64_t)pc + rel;
                if (tgt < 0 || tgt >= (int64_t)codeLen || !boundary[tgt]) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: jump offset %d from pc %u lands off-boundary (target %" PRId64 ")",
                        desc->name, rel, pc, tgt));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 1;
                break;
            }
            case OPERAND_INT4:
            case OPERAND_UINT4:
            case OPERAND_IDX4:
                off += 4;
                break;
            case OPERAND_LVT4: {
                unsigned int idx = TclGetUInt4AtPtr(code + pc + off);
                if (idx >= numLocals) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: local index %u >= %u at pc %u",
                        desc->name, idx, numLocals, pc));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 4;
                break;
            }
            case OPERAND_LIT4: {
                unsigned int idx = TclGetUInt4AtPtr(code + pc + off);
                if (idx >= numLits) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: literal index %u >= %u at pc %u",
                        desc->name, idx, numLits, pc));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 4;
                break;
            }
            case OPERAND_AUX4: {
                unsigned int idx = TclGetUInt4AtPtr(code + pc + off);
                if (idx >= numAux) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: aux index %u >= %u at pc %u",
                        desc->name, idx, numAux, pc));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 4;
                break;
            }
            case OPERAND_OFFSET4: {
                int rel = TclGetInt4AtPtr(code + pc + off);
                int64_t tgt = (int64_t)pc + rel;
                if (tgt < 0 || tgt >= (int64_t)codeLen || !boundary[tgt]) {
                    Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                        "tbcx: %s: jump offset %d from pc %u lands off-boundary (target %" PRId64 ")",
                        desc->name, rel, pc, tgt));
                    Tcl_Free(boundary);
                    return TCL_ERROR;
                }
                off += 4;
                break;
            }
            default:
                /* Unknown operand type — bail out safely rather than
                 * silently accept.  Tcl may add new operand kinds in
                 * future; this coerces us to revisit the validator. */
                Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                    "tbcx: %s: unknown operand type %d at pc %u (update validator)",
                    desc->name, (int)ot, pc));
                Tcl_Free(boundary);
                return TCL_ERROR;
            }
        }
        pc += (uint32_t)desc->numBytes;
    }

    /* Pass 3: walk jump-table aux payloads and validate stored targets.
     * The stored value is the relative offset added to the jumpTable
     * opcode's PC (see tclExecute.c processJumpTableEntry: "new pc =
     * PC_REL + jumpOffset").  We need to locate each jumpTable /
     * jumpTableNum opcode in the code stream and validate its aux's
     * hash entries against THAT opcode's PC, not against 0.  Any aux
     * index appearing more than once in the code stream (in the weird
     * case of shared aux) is validated against each occurrence. */
    for (uint32_t i = 0; i < numAux; i++) {
        if (auxArr[i].type != tbcxAuxJTStr && auxArr[i].type != tbcxAuxJTNum)
            continue;
        Tcl_HashTable *ht = (Tcl_HashTable *)auxArr[i].clientData;

        /* Find every jumpTable / jumpTableNum opcode pointing at this
         * aux entry, and validate each hash entry's target against
         * its PC. */
        int validated = 0;
        pc = 0;
        while (pc < codeLen) {
            unsigned int op = code[pc];
            const InstructionDesc *desc = &instTable[op];
            const char *nm = desc->name;
            if (nm && (strcmp(nm, "jumpTable") == 0 || strcmp(nm, "jumpTableNum") == 0)) {
                /* Single AUX4 operand at pc+1. */
                unsigned int auxIdx = TclGetUInt4AtPtr(code + pc + 1);
                if (auxIdx == i) {
                    Tcl_HashSearch hs;
                    Tcl_HashEntry *he;
                    for (he = Tcl_FirstHashEntry(ht, &hs); he;
                         he = Tcl_NextHashEntry(&hs)) {
                        intptr_t rel = (intptr_t)PTR2INT(Tcl_GetHashValue(he));
                        int64_t  tgt = (int64_t)pc + rel;
                        if (tgt < 0 || tgt >= (int64_t)codeLen || !boundary[tgt]) {
                            Tcl_SetObjResult(ip, Tcl_ObjPrintf(
                                "tbcx: jump-table aux %u: target pc+%" PRIdPTR
                                " = %" PRId64 " out of range or off-boundary"
                                " (jumpTable at pc %u, codeLen %u)",
                                i, (intptr_t)rel, tgt, pc, codeLen));
                            Tcl_Free(boundary);
                            return TCL_ERROR;
                        }
                    }
                    validated = 1;
                }
            }
            pc += (uint32_t)desc->numBytes;
        }
        (void)validated;
        /* An aux slot that is never referenced is harmless at load time
         * — it will be freed with the ByteCode and never dispatched to.
         * We do not require that every aux entry be reachable. */
    }

    Tcl_Free(boundary);
    return TCL_OK;
}

Tcl_Obj *Tbcx_ReadBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut, int setPrecompiled, int dumpOnly) {
    /* 1) code */
    uint32_t codeLen = 0;
    if (!Tbcx_R_U32(r, &codeLen))
        return NULL;
    if (codeLen > TBCX_MAX_CODE) {
        R_Error(r, "tbcx: code too large");
        return NULL;
    }
    unsigned char *code = (unsigned char *)Tcl_AttemptAlloc(codeLen ? codeLen : 1u);
    if (!code) {
        R_Error(r, "tbcx: allocation failed (code)");
        return NULL;
    }
    if (codeLen && !Tbcx_R_Bytes(r, code, codeLen)) {
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 2) literals */
    uint32_t numLits = 0;
    if (!Tbcx_R_U32(r, &numLits)) {
        Tcl_Free((char *)code);
        return NULL;
    }
    if (numLits > TBCX_MAX_LITERALS) {
        R_Error(r, "tbcx: too many literals");
        Tcl_Free((char *)code);
        return NULL;
    }
    /* stack-allocate for small literal counts */
    Tcl_Obj  *litStack[64];
    Tcl_Obj **lits       = NULL;
    int       litsOnHeap = 0;
    if (numLits) {
        if (numLits <= 64) {
            lits = litStack;
        } else {
            size_t litBytes = 0;
            if (!tbcx_checked_mul(sizeof(Tcl_Obj *), numLits, &litBytes)) {
                R_Error(r, "tbcx: literal array size overflow");
                Tcl_Free((char *)code);
                return NULL;
            }
            lits = (Tcl_Obj **)Tcl_AttemptAlloc(litBytes);
            if (!lits) {
                R_Error(r, "tbcx: allocation failed (literals)");
                Tcl_Free((char *)code);
                return NULL;
            }
            litsOnHeap = 1;
        }
    }
    for (uint32_t i = 0; i < numLits; i++) {
        Tcl_Obj *lit = ReadLiteral(r, ip, 0, dumpOnly);
        if (!lit) {
            for (uint32_t j = 0; j < i; j++) {
                Tcl_DecrRefCount(lits[j]);
            }
            if (litsOnHeap)
                Tcl_Free(lits);
            Tcl_Free((char *)code);
            return NULL;
        }
        Tcl_IncrRefCount(lit); /* Protect immediately — refcount 0→1 */
        lits[i] = lit;
    }

    /* 3) AuxData */
    AuxData *auxArr = NULL;
    uint32_t numAux = 0;
    if (!ReadAuxArray(r, &auxArr, &numAux)) {
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 4) Exceptions */
    ExceptionRange *exArr = NULL;
    uint32_t        numEx = 0;
    if (!ReadExceptions(r, &exArr, &numEx)) {
        FreeAuxArrayOwned(auxArr, numAux);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 5) Epilogue: maxStack, reserved, numLocals */
    uint32_t maxStack = 0, reserved = 0, numLocals = 0;
    if (!Tbcx_R_U32(r, &maxStack) || !Tbcx_R_U32(r, &reserved) || !Tbcx_R_U32(r, &numLocals)) {
        if (exArr)
            Tcl_Free(exArr);
        FreeAuxArrayOwned(auxArr, numAux);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    (void)reserved; /* wire-format placeholder for future use */

    /* Hard caps on untrusted values from the .tbcx stream to prevent
     * huge allocations, integer wrap, or allocator panic.  These limits
     * are generous for legitimate bytecode but reject pathological inputs. */
    if (maxStack > TBCX_MAX_STACK) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: maxStack %u exceeds limit %u", maxStack, TBCX_MAX_STACK));
        if (exArr)
            Tcl_Free(exArr);
        FreeAuxArrayOwned(auxArr, numAux);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]);
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }
    if (numLocals > TBCX_MAX_LOCALS) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numLocals %u exceeds limit %u", numLocals, TBCX_MAX_LOCALS));
        if (exArr)
            Tcl_Free(exArr);
        FreeAuxArrayOwned(auxArr, numAux);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]);
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    if (numLocalsOut)
        *numLocalsOut = numLocals;

    Tcl_Obj  *nameObjsStack[64];
    Tcl_Obj **nameObjs   = NULL;
    int       nameOnHeap = 0;
    if (numLocals > 0) {
        if (numLocals <= 64) {
            nameObjs = nameObjsStack;
        } else {
            nameObjs = (Tcl_Obj **)Tcl_AttemptAlloc(sizeof(Tcl_Obj *) * (size_t)numLocals);
            if (!nameObjs) {
                R_Error(r, "tbcx: allocation failed (local names)");
                if (exArr)
                    Tcl_Free(exArr);
                FreeAuxArrayOwned(auxArr, numAux);
                for (uint32_t j = 0; j < numLits; j++)
                    Tcl_DecrRefCount(lits[j]);
                if (litsOnHeap)
                    Tcl_Free(lits);
                Tcl_Free((char *)code);
                return NULL;
            }
            nameOnHeap = 1;
        }
        for (uint32_t i = 0; i < numLocals; i++) {
            char    *s   = NULL;
            uint32_t len = 0;
            if (!Tbcx_R_LPString(r, &s, &len)) {
                if (nameObjs) {
                    for (uint32_t k = 0; k < i; k++)
                        Tcl_DecrRefCount(nameObjs[k]);
                    if (nameOnHeap)
                        Tcl_Free(nameObjs);
                }
                if (exArr)
                    Tcl_Free(exArr);
                FreeAuxArrayOwned(auxArr, numAux);
                /* Release the protective refcounts taken on literal objects
                 * before freeing the lits array — otherwise they leak. */
                for (uint32_t j = 0; j < numLits; j++)
                    Tcl_DecrRefCount(lits[j]);
                if (litsOnHeap)
                    Tcl_Free(lits);
                Tcl_Free((char *)code);
                return NULL;
            }
            Tcl_Obj *o = Tcl_NewStringObj(s, (Tcl_Size)len);
            Tcl_Free(s);
            nameObjs[i] = o; /* refcount 0 for now */
        }
    }

    /* Validate untrusted bytecode operands BEFORE handing them to
     * Tcl's evaluator.  Tcl core trusts compiler output and does NOT
     * revalidate at runtime, so a crafted .tbcx can OOB-read literals
     * or jump off-boundary without this pass.  Rejection here is the
     * canonical place to stop a hostile artifact. */
    if (ValidateBlockOperands(ip, code, codeLen, numLits, numAux,
                              numLocals, auxArr) != TCL_OK) {
        /* Same cleanup as any other post-aux failure in this function. */
        if (nameObjs) {
            for (uint32_t k = 0; k < numLocals; k++)
                Tcl_DecrRefCount(nameObjs[k]);
            if (nameOnHeap)
                Tcl_Free(nameObjs);
        }
        if (exArr)
            Tcl_Free(exArr);
        FreeAuxArrayOwned(auxArr, numAux);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]);
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    /* Build bytecode object */
    Tcl_Obj *bc = ByteCodeObj(ip, nsForDefault, code, codeLen, lits, numLits, auxArr, numAux, exArr, numEx, (int)maxStack, setPrecompiled);

    if (exArr)
        Tcl_Free(exArr);
    /* On success, ownership of each auxArr[i].clientData payload has
     * transferred into the packed ByteCode (via the memcpy in
     * TbcxByteCode).  Free only the outer array then — the packed
     * ByteCode will call each aux type's freeProc at destruction time.
     * On failure no ownership transferred; deep-free the payloads here
     * to avoid leaking them. */
    if (bc) {
        if (auxArr)
            Tcl_Free(auxArr);
    } else {
        FreeAuxArrayOwned(auxArr, numAux);
    }
    /* Drop our protective refcount on literals — ByteCodeObj has its own */
    for (uint32_t j = 0; j < numLits; j++)
        Tcl_DecrRefCount(lits[j]);
    if (litsOnHeap)
        Tcl_Free(lits);
    if (code)
        Tcl_Free(code);

    if (bc && numLocals > 0 && nameObjs) {
        ByteCode *bcPtr = NULL;
        bcPtr           = TbcxGetByteCode(bc);
        if (bcPtr) {
            size_t varBytes = 0;
            if (!tbcx_checked_mul((size_t)numLocals, sizeof(Tcl_Obj *), &varBytes) || varBytes > SIZE_MAX - offsetof(LocalCache, varName0)) {
                R_Error(r, "tbcx: local cache size overflow");
                for (uint32_t k = 0; k < numLocals; k++) {
                    if (nameObjs[k]) {
                        Tcl_IncrRefCount(nameObjs[k]);
                        Tcl_DecrRefCount(nameObjs[k]);
                    }
                }
                if (nameOnHeap)
                    Tcl_Free(nameObjs);
                Tcl_IncrRefCount(bc);
                Tcl_DecrRefCount(bc);
                return NULL;
            }
            size_t      bytes = offsetof(LocalCache, varName0) + varBytes;
            LocalCache *lc    = (LocalCache *)Tcl_AttemptAlloc(bytes);
            if (!lc) {
                R_Error(r, "tbcx: allocation failed (local cache)");
                /* Bounce refcount-0 nameObjs */
                for (uint32_t k = 0; k < numLocals; k++) {
                    if (nameObjs[k]) {
                        Tcl_IncrRefCount(nameObjs[k]);
                        Tcl_DecrRefCount(nameObjs[k]);
                    }
                }
                if (nameOnHeap)
                    Tcl_Free(nameObjs);
                /* Bounce refcount-0 bc */
                Tcl_IncrRefCount(bc);
                Tcl_DecrRefCount(bc);
                return NULL;
            }
            lc->refCount  = 1;
            lc->numVars   = (Tcl_Size)numLocals;
            Tcl_Obj **dst = (Tcl_Obj **)&lc->varName0;
            for (uint32_t i = 0; i < numLocals; i++) {
                dst[i] = nameObjs[i];
                Tcl_IncrRefCount(dst[i]);
            }
            bcPtr->localCachePtr = lc;
        } else {
            /* bcPtr is NULL — nameObjs entries are at refcount 0 and
               will not be transferred to a LocalCache.  Release them. */
            for (uint32_t i = 0; i < numLocals; i++) {
                if (nameObjs[i]) {
                    Tcl_IncrRefCount(nameObjs[i]);
                    Tcl_DecrRefCount(nameObjs[i]);
                }
            }
        }
        if (nameOnHeap)
            Tcl_Free(nameObjs);
    } else if (numLocals > 0 && nameObjs) {
        /* bc is NULL — ByteCodeObj failed.  Release the refcount-0
           nameObjs entries and free the heap array to avoid leaks. */
        for (uint32_t i = 0; i < numLocals; i++) {
            if (nameObjs[i]) {
                Tcl_IncrRefCount(nameObjs[i]);
                Tcl_DecrRefCount(nameObjs[i]);
            }
        }
        if (nameOnHeap)
            Tcl_Free(nameObjs);
    }

    return bc;
}

/* ProcShim_DirectInstall — build and register a new Proc from precompiled
 * data without going through TclCreateProc.  Called on the fast path once
 * handler pointers have been captured from the first slow-path proc.
 * Returns TCL_OK on success, TCL_ERROR on command-creation failure. */
static int ProcShim_DirectInstall(ProcShim *ps, Tcl_Interp *ip, Tcl_Obj *fqn, Tcl_Obj *nameObj, Proc *preProc, Tcl_Obj *savedArgs) {
    /* Build a new Proc directly from our precompiled data. */
    Proc *newProc = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(newProc, 0, sizeof(Proc));
    newProc->iPtr     = (Interp *)ip;
    newProc->refCount = 1;
    newProc->bodyPtr  = preProc->bodyPtr;
    Tcl_IncrRefCount(newProc->bodyPtr);
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size       numA = 0;
        if (Tbcx_BuildLocals(ip, savedArgs, &first, &last, &numA) != TCL_OK) {
            Tcl_DecrRefCount(newProc->bodyPtr);
            Tcl_Free((char *)newProc);
            return TCL_ERROR;
        }
        newProc->numArgs           = numA;
        newProc->numCompiledLocals = numA;
        newProc->firstLocalPtr     = first;
        newProc->lastLocalPtr      = last;
    }
    CompiledLocals(newProc, preProc->numCompiledLocals);

    /* Resolve namespace */
    const char *fqnStr = Tbcx_GetStringSafe(fqn);
    Namespace  *nsPtr  = (Namespace *)Tcl_GetCurrentNamespace(ip);
    if (fqnStr[0] == ':' && fqnStr[1] == ':') {
        const char *lastSep = fqnStr;
        for (const char *p2 = fqnStr; *p2; p2++) {
            if (p2[0] == ':' && p2[1] == ':')
                lastSep = p2;
        }
        if (lastSep > fqnStr) {
            Tcl_Obj *nsObj = Tcl_NewStringObj(fqnStr, (Tcl_Size)(lastSep - fqnStr));
            Tcl_IncrRefCount(nsObj);
            nsPtr = (Namespace *)Tbcx_EnsureNamespace(ip, Tbcx_GetStringSafe(nsObj));
            Tcl_DecrRefCount(nsObj);
        } else {
            nsPtr = (Namespace *)Tcl_GetGlobalNamespace(ip);
        }
    }

    /* Register command using captured handler pointers. */
    Tcl_Command token = Tcl_CreateObjCommand2(ip, fqnStr, ps->procDispatchObj, newProc, ps->procDeleteProc);
    if (!token) {
        TclProcDeleteProc(newProc);
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: failed to create command \"%s\"", fqnStr));
        return TCL_ERROR;
    }
    Command *cmdPtr = (Command *)token;
    if (ps->procDispatchNre)
        cmdPtr->nreProc2 = ps->procDispatchNre;
    newProc->cmdPtr = cmdPtr;

    /* Fix bytecode linkage */
    ByteCode *bc    = TbcxGetByteCode(newProc->bodyPtr);
    if (bc) {
        TbcxFixupByteCode(bc, newProc, ip, nsPtr, TBCX_FIXUP_CACHE_DROP);
    }
    return TCL_OK;
}

/* CmdProcShim — intercepts "proc" via BOTH objProc2 and nreProc2.
 *
 * Tcl 9.1's bytecode engine dispatches NRE-enabled commands through
 * Command.nreProc2 when non-NULL, completely bypassing objProc2.
 * Since "proc" is registered via Tcl_NRCreateCommand, its nreProc2 IS
 * non-NULL.  AddProcShim installs CmdProcShim on both dispatch paths
 * so the shim is reached regardless of how the command is invoked.
 *
 * All forwarding goes through savedObjProc2 (the synchronous entry)
 * so that bodyPtr restoration works after the call returns. */

static int CmdProcShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    ProcShim *ps = (ProcShim *)cd;

    if (objc != 4) {
        /* Forward to the original "proc" handler (synchronous entry). */
        return ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
    }

    Tcl_Obj    *nameObj = objv[1], *argsObj = objv[2];
    const char *nm  = Tbcx_GetStringSafe(nameObj);
    Tcl_Obj    *fqn = NULL;
    if (nm[0] == ':' && nm[1] == ':') {
        fqn = nameObj;
    } else {
        Tcl_Namespace *cur     = Tcl_GetCurrentNamespace(ip);
        const char    *curName = cur ? cur->fullName : "::";
        fqn                    = Tcl_NewStringObj(curName, -1);
        Tcl_IncrRefCount(fqn); /* own one ref so DecrRefCount on exit is safe */
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0')) {
            Tcl_AppendToObj(fqn, "::", 2);
        }
        Tcl_AppendObjToObj(fqn, nameObj);
    }

    Tcl_HashEntry *he         = Tcl_FindHashEntry(&ps->procsByFqn, Tcl_GetString(fqn));

    /* Indexed marker override: if the body argument starts with
       TBCX_PROC_MARKER_PFX, use the encoded index to select the
       pair directly from procsByIdx.  This handles conflicting
       proc definitions across if/else branches, where the same
       FQN appears multiple times with different compiled bodies. */
    Tcl_Obj       *markerPair = NULL;
    {
        Tcl_Size    bLen = 0;
        const char *bStr = Tbcx_GetStringFromObjSafe(objv[3], &bLen);
        if (bLen >= TBCX_PROC_MARKER_PFX_LEN && memcmp(bStr, TBCX_PROC_MARKER_PFX, TBCX_PROC_MARKER_PFX_LEN) == 0 && ps->procsByIdx) {
            /* Parse decimal index after the prefix */
            unsigned    idx      = 0;
            const char *dp       = bStr + TBCX_PROC_MARKER_PFX_LEN;
            int         hasDigit = 0;
            while (*dp >= '0' && *dp <= '9') {
                idx = idx * 10u + (unsigned)(*dp - '0');
                dp++;
                hasDigit = 1;
            }
            if (hasDigit && *dp == '\0' && idx < ps->numProcsIdx && ps->procsByIdx[idx]) {
                markerPair = ps->procsByIdx[idx];
            }
        }
    }

    if (he || markerPair) {
        Tcl_Obj *pair      = markerPair ? markerPair : (Tcl_Obj *)Tcl_GetHashValue(he);
        Tcl_Obj *savedArgs = NULL, *procBody = NULL;
        {
            Tcl_Size  pairLen   = 0;
            Tcl_Obj **pairElems = NULL;
            if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) == TCL_OK && pairLen >= 2) {
                savedArgs = pairElems[0];
                procBody  = pairElems[1];
            }
        }
        if (savedArgs && procBody) {
            Tcl_Size _d = 0;
            /* Force list-shimmering for canonical string rep before
               byte-wise comparison; if conversion fails, clear the interp
               result and continue with the existing string rep. */
            if (Tcl_ListObjLength(ip, argsObj, &_d) != TCL_OK)
                Tcl_ResetResult(ip);
            if (Tcl_ListObjLength(ip, savedArgs, &_d) != TCL_OK)
                Tcl_ResetResult(ip);
            Tcl_Size    aLen = 0, bLen = 0;
            const char *a = Tbcx_GetStringFromObjSafe(argsObj, &aLen);
            const char *b = Tbcx_GetStringFromObjSafe(savedArgs, &bLen);

            if (aLen == bLen && memcmp(a, b, (size_t)aLen) == 0) {
                /* Get our precompiled Proc from the registry. */
                const Tcl_ObjInternalRep *pbIR    = Tcl_FetchInternalRep(procBody, tbcxTyProcBody);
                Proc                     *preProc = pbIR ? (Proc *)pbIR->twoPtrValue.ptr1 : NULL;
                if (!preProc || !preProc->bodyPtr) {
                    if (fqn != nameObj)
                        Tcl_DecrRefCount(fqn);
                    return ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
                }

                /* ---- direct install path ----
                 * Once we have captured the handler pointers from the first
                 * proc created via the slow path, we can skip TclCreateProc
                 * entirely for subsequent procs.  This avoids N compilations
                 * of the empty body string "". */
                if (ps->haveDispatch) {
                    int drc = ProcShim_DirectInstall(ps, ip, fqn, nameObj, preProc, savedArgs);
                    if (fqn != nameObj)
                        Tcl_DecrRefCount(fqn);
                    return drc;
                }

                /* ---- Slow path (first proc): create via TclCreateProc,
                   capture handler pointers, then swap body. ---- */
                {
                    int rc = ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
                    if (rc != TCL_OK) {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }
                    Tcl_Command cmd = Tcl_FindCommand(ip, Tbcx_GetStringSafe(fqn), NULL, TCL_GLOBAL_ONLY);
                    if (!cmd) {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }
                    Command *cmdPtr  = (Command *)cmd;
                    Proc    *newProc = (Proc *)cmdPtr->objClientData2;
                    if (!newProc) {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }

                    /* Capture handler pointers for future direct installs */
                    if (!ps->haveDispatch) {
                        ps->procDispatchObj = cmdPtr->objProc2;
                        ps->procDispatchNre = cmdPtr->nreProc2;
                        ps->procDeleteProc  = cmdPtr->deleteProc;
                        ps->haveDispatch    = 1;
                    }

                    /* Swap bodyPtr */
                    Tcl_Obj *preBody = preProc->bodyPtr;
                    Tcl_IncrRefCount(preBody);
                    Tcl_DecrRefCount(newProc->bodyPtr);
                    newProc->bodyPtr = preBody;
                    CompiledLocals(newProc, preProc->numCompiledLocals);
                    {
                        ByteCode *bc = TbcxGetByteCode(preBody);
                        if (bc) {
                            TbcxFixupByteCode(bc, newProc, ip, cmdPtr->nsPtr, TBCX_FIXUP_CACHE_DROP);
                        }
                    }

                    if (fqn != nameObj)
                        Tcl_DecrRefCount(fqn);
                    return rc;
                }
            }
        }
    }

    /* No match — forward to original handler */
    int rc = ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
    if (fqn != nameObj)
        Tcl_DecrRefCount(fqn);
    return rc;
}

/* ProcCmdDeleteTrace — called when the "proc" command is renamed or deleted.
 *
 * The DELETE case is simple: the Command struct is about to be freed; we
 * just null our cached pointer so DelProcShim skips the restore.
 *
 * The RENAME case is subtler and was the site of an internal-review UAF:
 * Tcl keeps the Command struct alive across a rename (it just re-keys the
 * hash-table entry).  The struct's objProc2 field still holds
 * CmdProcShim, and its objClientData2 still points at *our* ProcShim —
 * which is stack-allocated inside LoadTbcxStream and disappears when
 * LoadTbcxStream returns.
 *
 * If we merely null ps->procCmdPtr here (as the old code did), DelProcShim
 * sees the NULL and skips the in-place restore.  The renamed command
 * is then left with a dangling clientData pointer; invoking it after
 * tbcx::load returns produces a stack-use-after-return that typically
 * crashes with "called Tcl_FindHashEntry on deleted table / illegal
 * instruction" or worse.
 *
 * The correct RENAME handling is to do the in-place restore *now*,
 * while ps->procCmdPtr is still valid, and THEN null it so DelProcShim
 * knows the work is done. */
static void ProcCmdDeleteTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags) {
    ProcShim *ps = (ProcShim *)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & TCL_TRACE_RENAME) {
        /* Restore original handlers while the Command struct is still
         * accessible — the rename keeps the struct alive but moves it
         * to a new name, and our shim's client data (ps) is about to
         * become invalid once LoadTbcxStream returns. */
        if (ps->procCmdPtr) {
            ps->procCmdPtr->objProc2       = ps->savedObjProc2;
            ps->procCmdPtr->objClientData2 = ps->savedClientData2;
            if (ps->savedNreProc2) {
                ps->procCmdPtr->nreProc2 = ps->savedNreProc2;
            }
        }
    }
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE)) {
        ps->procCmdPtr     = NULL;
        ps->traceInstalled = 0; /* trace auto-removed on delete */
    }
}

/* AddProcShim — intercept the [proc] command to substitute precompiled bodies.
 *
 * Thread safety / lock ordering:
 *   This function accesses Tcl internal Command structs and calls
 *   Tcl_TraceCommand, both of which may acquire Tcl-internal mutexes.
 *   No TBCX mutex is held during this call.  Callers must ensure this
 *   function is only called from the interp-owning thread. */
static int AddProcShim(Tcl_Interp *ip, ProcShim *ps) {
    memset(ps, 0, sizeof(*ps));
    Tcl_InitHashTable(&ps->procsByFqn, TCL_STRING_KEYS);

    Tcl_Command token = Tcl_FindCommand(ip, "proc", NULL, 0);
    if (!token) {
        Tcl_DeleteHashTable(&ps->procsByFqn);
        return TCL_ERROR;
    }

    ps->interp           = ip;
    ps->procCmdPtr       = (Command *)token;
    ps->savedObjProc2    = ps->procCmdPtr->objProc2;
    ps->savedNreProc2    = ps->procCmdPtr->nreProc2;
    ps->savedClientData2 = ps->procCmdPtr->objClientData2;

    /* Install a command trace so we know if "proc" is renamed or deleted
       while the loaded script is executing.
       Defensive: FindCommand above guarantees "proc" exists, so this
       cannot fail in single-threaded use, but check anyway. */
    if (Tcl_TraceCommand(ip, "proc", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ProcCmdDeleteTrace, ps) != TCL_OK) {
        Tcl_DeleteHashTable(&ps->procsByFqn);
        return TCL_ERROR;
    }
    ps->traceInstalled             = 1;

    /* Intercept BOTH objProc2 AND nreProc2.
       Tcl 9.1's "proc" is registered via Tcl_NRCreateCommand, which
       sets nreProc2 to a non-NULL value.  The bytecode engine (TEBC)
       dispatches through nreProc2 when non-NULL, completely bypassing
       objProc2.  Both must be swapped so the shim intercepts all
       invocations — bytecode-compiled and direct alike.

       proc's handler (TclNRProcObjCmd) creates the proc synchronously
       and does NOT enqueue NRE callbacks, so it is safe for our shim
       to call it directly and do post-processing (bodyPtr restoration)
       after it returns. */
    ps->procCmdPtr->objProc2       = CmdProcShim;
    ps->procCmdPtr->objClientData2 = ps;
    if (ps->procCmdPtr->nreProc2) {
        ps->procCmdPtr->nreProc2 = CmdProcShim;
    }

    return TCL_OK;
}

static void DelProcShim(Tcl_Interp *ip, ProcShim *ps) {
    /* Remove command trace before attempting any restore.
       If the command was already deleted, the trace fired and set
       procCmdPtr = NULL and traceInstalled = 0. */
    if (ps->traceInstalled) {
        Tcl_UntraceCommand(ip, "proc", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ProcCmdDeleteTrace, ps);
        ps->traceInstalled = 0;
    }

    /* Only restore handlers if the Command struct is still alive. */
    if (ps->procCmdPtr) {
        ps->procCmdPtr->objProc2       = ps->savedObjProc2;
        ps->procCmdPtr->objClientData2 = ps->savedClientData2;
        if (ps->savedNreProc2) {
            ps->procCmdPtr->nreProc2 = ps->savedNreProc2;
        }
    }
    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&ps->procsByFqn, &s); e; e = Tcl_NextHashEntry(&s)) {
        Tcl_Obj *val = (Tcl_Obj *)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&ps->procsByFqn);
    /* Free indexed array */
    if (ps->procsByIdx) {
        for (uint32_t i = 0; i < ps->numProcsIdx; i++) {
            if (ps->procsByIdx[i])
                Tcl_DecrRefCount(ps->procsByIdx[i]);
        }
        Tcl_Free((char *)ps->procsByIdx);
        ps->procsByIdx = NULL;
    }
}

/* ==========================================================================
 * ApplyShim — persistent [apply] interceptor for shimmer-proof lambdas
 *
 * Unlike ProcShim/OOShim which are scoped to LoadTbcxStream, ApplyShim
 * persists for the interpreter's lifetime via Tcl_SetAssocData.  This is
 * necessary because precompiled lambdas may be called from procs long
 * after tbcx::load returns.
 *
 * Fast path (no shimmer): apply sees lambdaExpr internal rep → uses it.
 *   The shim just forwards to the original handler.  Cost: one
 *   Tcl_FetchInternalRep check per [apply] call.
 *
 * Recovery path (shimmer happened): lambdaExpr rep was evicted, shim
 *   looks up the lambda in its registry by Tcl_Obj pointer, re-installs
 *   the precompiled Proc*, then forwards.
 * ========================================================================== */

static int CmdApplyShim(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    ApplyShim *as = (ApplyShim *)cd;

    if (as->numRegistered > 0 && objc >= 2 && tbcxTyLambda) {
        Tcl_Obj *lambda = objv[1];

        /* Fast path: lambdaExpr internal rep is still present — Tcl handles it */
        if (!Tcl_FetchInternalRep(lambda, tbcxTyLambda)) {
            /* lambdaExpr rep missing (shimmer happened) — try to recover */
            Tcl_HashEntry *he = Tcl_FindHashEntry(&as->lambdaRegistry, (const char *)lambda);
            if (he) {
                ApplyLambdaEntry  *le = (ApplyLambdaEntry *)Tcl_GetHashValue(he);
                /* Re-install the precompiled lambdaExpr internal rep */
                Tcl_ObjInternalRep ir;
                ir.twoPtrValue.ptr1 = le->procPtr;
                ir.twoPtrValue.ptr2 = le->nsObj;
                Tcl_StoreInternalRep(lambda, tbcxTyLambda, &ir);
                /* StoreInternalRep takes ownership; bump refcounts so our
                   registry copy stays valid for future shimmer recovery. */
                le->procPtr->refCount++;
                Tcl_IncrRefCount(le->nsObj);
            }
        }
    }

    return as->savedApplyProc(as->savedApplyCD, ip, objc, objv);
}

/* CmdApplyShimNre — NRE-aware variant installed on nreProc2.
 * Identical shimmer-recovery logic, but dispatches through the
 * original savedApplyNre so that the NRE trampoline is preserved
 * for deeply recursive lambda calls. */
static int CmdApplyShimNre(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    ApplyShim *as = (ApplyShim *)cd;

    if (as->numRegistered > 0 && objc >= 2 && tbcxTyLambda) {
        Tcl_Obj *lambda = objv[1];

        if (!Tcl_FetchInternalRep(lambda, tbcxTyLambda)) {
            Tcl_HashEntry *he = Tcl_FindHashEntry(&as->lambdaRegistry, (const char *)lambda);
            if (he) {
                ApplyLambdaEntry  *le = (ApplyLambdaEntry *)Tcl_GetHashValue(he);
                Tcl_ObjInternalRep ir;
                ir.twoPtrValue.ptr1 = le->procPtr;
                ir.twoPtrValue.ptr2 = le->nsObj;
                Tcl_StoreInternalRep(lambda, tbcxTyLambda, &ir);
                le->procPtr->refCount++;
                Tcl_IncrRefCount(le->nsObj);
            }
        }
    }

    return as->savedApplyNre(as->savedApplyCD, ip, objc, objv);
}

/* ApplyCmdDeleteTrace — called when the "apply" command is renamed or deleted.
 * Invalidates the cached Command* to prevent stale-pointer writes in
 * ApplyShimTeardown.  Without this trace, if "apply" is deleted or renamed
 * before interpreter destruction, ApplyShimTeardown would write through a
 * freed Command struct (use-after-free / process crash). */
static void ApplyCmdDeleteTrace(void *cd, Tcl_Interp *interp, const char *oldName, const char *newName, int flags) {
    ApplyShim *as = (ApplyShim *)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & TCL_TRACE_RENAME) {
        /* Same in-place restore pattern as ProcCmdDeleteTrace: the
         * Command struct survives the rename and must be restored
         * while we still have a valid pointer to it.  ApplyShim is
         * heap-allocated in TbcxInterpState (unlike ProcShim/OOShim
         * which are stack-allocated in LoadTbcxStream), so the
         * UAF severity here is lower — the worst case is the
         * renamed command continuing to dispatch through CmdApplyShim
         * with *stale but live* state.  Restoring the handlers here
         * also stops the shim from intercepting calls to the renamed
         * command, which matches the user's intent when they rename
         * [apply]. */
        if (as->applyCmdPtr) {
            as->applyCmdPtr->objProc2       = as->savedApplyProc;
            as->applyCmdPtr->objClientData2 = as->savedApplyCD;
            if (as->savedApplyNre) {
                as->applyCmdPtr->nreProc2 = as->savedApplyNre;
            }
        }
    }
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE)) {
        as->applyCmdPtr    = NULL;
        as->traceInstalled = 0; /* trace auto-removed on delete */
    }
}

/* ApplyShimTeardown — clean up the apply shim state (untrace, restore
 * original command handlers, free registry entries and hash table).
 * Does NOT free the ApplyShim struct itself — it is embedded in
 * TbcxInterpState and freed by TbcxInterpStateCleanup. */
static void ApplyShimTeardown(ApplyShim *as, Tcl_Interp *ip) {
    /* Remove command trace before attempting any restore.
       If the command was already deleted, the trace fired and set
       applyCmdPtr = NULL and traceInstalled = 0. */
    if (as->traceInstalled) {
        Tcl_UntraceCommand(ip, "apply", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ApplyCmdDeleteTrace, as);
        as->traceInstalled = 0;
    }

    /* Only restore original [apply] handlers if the Command struct is still alive */
    if (as->applyCmdPtr) {
        as->applyCmdPtr->objProc2       = as->savedApplyProc;
        as->applyCmdPtr->objClientData2 = as->savedApplyCD;
        if (as->savedApplyNre) {
            as->applyCmdPtr->nreProc2 = as->savedApplyNre;
        }
    }

    /* Free registry entries */
    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&as->lambdaRegistry, &s); e; e = Tcl_NextHashEntry(&s)) {
        /* Release the lambda reference held by RegisterPrecompiledLambda */
        Tcl_Obj *lambdaKey = (Tcl_Obj *)Tcl_GetHashKey(&as->lambdaRegistry, e);
        if (lambdaKey)
            Tcl_DecrRefCount(lambdaKey);

        ApplyLambdaEntry *le = (ApplyLambdaEntry *)Tcl_GetHashValue(e);
        if (le) {
            if (le->procPtr && --le->procPtr->refCount <= 0) {
                TclProcCleanupProc(le->procPtr);
            }
            if (le->nsObj)
                Tcl_DecrRefCount(le->nsObj);
            Tcl_Free(le);
        }
    }
    Tcl_DeleteHashTable(&as->lambdaRegistry);
}

/* TbcxInterpStateCleanup — Tcl_SetAssocData delete callback.
 * Called when the interpreter is being destroyed.  Tears down the
 * apply shim (if active) and frees the consolidated state struct. */
static void TbcxInterpStateCleanup(void *cd, Tcl_Interp *ip) {
    TbcxInterpState *st = (TbcxInterpState *)cd;
    if (st->applyActive) {
        ApplyShimTeardown(&st->apply, ip);
        st->applyActive = 0;
    } else {
        /* Hash table was initialized in TbcxGetInterpState but the
           apply shim was never activated — just delete the empty table. */
        Tcl_DeleteHashTable(&st->apply.lambdaRegistry);
    }
    Tcl_Free(st);
}

/* TbcxGetInterpState — get or create the per-interpreter state.
 * On first call, allocates and registers the state under
 * TBCX_INTERP_STATE_KEY with TbcxInterpStateCleanup as the
 * delete callback.  Subsequent calls return the existing state. */
static TbcxInterpState *TbcxGetInterpState(Tcl_Interp *ip) {
    TbcxInterpState *st = (TbcxInterpState *)Tcl_GetAssocData(ip, TBCX_INTERP_STATE_KEY, NULL);
    if (st)
        return st;

    st = (TbcxInterpState *)Tcl_Alloc(sizeof(TbcxInterpState));
    memset(st, 0, sizeof(*st));
    st->interp = ip;
    Tcl_InitHashTable(&st->apply.lambdaRegistry, TCL_ONE_WORD_KEYS);
    st->apply.interp = ip;
    Tcl_SetAssocData(ip, TBCX_INTERP_STATE_KEY, TbcxInterpStateCleanup, st);
    return st;
}

/* ApplyShimPurgeStale — remove registry entries for lambda objects that
 * are only kept alive by the registry itself (refCount == 1).  These are
 * lambdas whose ByteCode literal pool has been freed, meaning they can
 * never be invoked again.  Releasing them prevents unbounded accumulation
 * in long-running interpreters that load many .tbcx files.
 *
 * Called lazily from EnsureApplyShim on subsequent tbcx::load calls. */
static void ApplyShimPurgeStale(ApplyShim *as) {
    if (!as)
        return;

    /* Collect keys to remove (can't modify hash during iteration). */
    Tcl_Obj       *staleStack[32];
    Tcl_Obj      **stale  = staleStack;
    Tcl_Size       nStale = 0, staleCap = 32;
    int            onHeap = 0;

    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&as->lambdaRegistry, &s); e; e = Tcl_NextHashEntry(&s)) {
        Tcl_Obj *lambda = (Tcl_Obj *)Tcl_GetHashKey(&as->lambdaRegistry, e);
        /* Direct refCount access: depends on Tcl internal struct layout.
         * Safe because the interpreter is single-threaded and we intentionally
         * use Tcl internal headers.  refCount <= 1 means only our registry
         * holds a reference — the lambda is no longer live in user code. */
        if (lambda && lambda->refCount <= 1) {
            /* Only our registry holds a reference — this lambda is stale */
            if (nStale >= staleCap) {
                Tcl_Size newCap = staleCap * 2;
                if (newCap < staleCap || newCap > (Tcl_Size)(SIZE_MAX / sizeof(Tcl_Obj *)))
                    break; /* cap overflow — stop collecting, purge what we have */
                staleCap = newCap;
                if (!onHeap) {
                    Tcl_Obj **tmp = (Tcl_Obj **)Tcl_AttemptAlloc(sizeof(Tcl_Obj *) * (size_t)staleCap);
                    if (!tmp) {
                        /* OOM: stop collecting; purge what we have */
                        break;
                    }
                    memcpy(tmp, stale, sizeof(Tcl_Obj *) * (size_t)nStale);
                    stale  = tmp;
                    onHeap = 1;
                } else {
                    Tcl_Obj **tmp = (Tcl_Obj **)Tcl_AttemptRealloc(stale, sizeof(Tcl_Obj *) * (size_t)staleCap);
                    if (!tmp) {
                        /* OOM: stop collecting; purge what we have */
                        break;
                    }
                    stale = tmp;
                }
            }
            stale[nStale++] = lambda;
        }
    }

    /* Remove stale entries */
    for (Tcl_Size i = 0; i < nStale; i++) {
        Tcl_Obj *lambda = stale[i];
        e               = Tcl_FindHashEntry(&as->lambdaRegistry, (const char *)lambda);
        if (e) {
            ApplyLambdaEntry *le = (ApplyLambdaEntry *)Tcl_GetHashValue(e);
            if (le) {
                if (le->procPtr && --le->procPtr->refCount <= 0) {
                    TclProcCleanupProc(le->procPtr);
                }
                if (le->nsObj)
                    Tcl_DecrRefCount(le->nsObj);
                Tcl_Free(le);
            }
            Tcl_DeleteHashEntry(e);
            /* Release the registry's reference (may free the lambda) */
            Tcl_DecrRefCount(lambda);
            if (as->numRegistered > 0)
                as->numRegistered--;
        }
    }

    if (onHeap)
        Tcl_Free(stale);
}

/* ==========================================================================
 * Purges stale lambda entries from the per-interp ApplyShim registry.
 * Safe to call even when no ApplyShim has been installed (no-op).
 * ========================================================================== */

void TbcxApplyShimPurgeAll(Tcl_Interp *ip) {
    TBCX_ASSERT_INTERP_THREAD(ip);
    TbcxInterpState *st = (TbcxInterpState *)Tcl_GetAssocData(ip, TBCX_INTERP_STATE_KEY, NULL);
    if (st && st->applyActive)
        ApplyShimPurgeStale(&st->apply);
}

/* EnsureApplyShim — get or activate the per-interp ApplyShim.
 * First call installs the shim on [apply] within the TbcxInterpState.
 * Subsequent calls (from additional tbcx::load invocations) return the
 * existing shim — new lambda entries are simply added to the registry. */
static ApplyShim *EnsureApplyShim(Tcl_Interp *ip) {
    TbcxInterpState *st = TbcxGetInterpState(ip);
    ApplyShim       *as = &st->apply;

    if (st->applyActive) {
        /* Subsequent load: purge stale entries before adding new ones */
        ApplyShimPurgeStale(as);
        return as;
    }

    Tcl_Command token = Tcl_FindCommand(ip, "apply", NULL, 0);
    if (!token)
        return NULL;

    as->applyCmdPtr    = (Command *)token;
    as->savedApplyProc = as->applyCmdPtr->objProc2;
    as->savedApplyNre  = as->applyCmdPtr->nreProc2;
    as->savedApplyCD   = as->applyCmdPtr->objClientData2;

    /* Install a command trace so we know if "apply" is renamed or deleted
       before cleanup runs.  Without this, TbcxInterpStateCleanup would write
       through a stale Command* — a crash-class UAF bug. */
    if (Tcl_TraceCommand(ip, "apply", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ApplyCmdDeleteTrace, as) != TCL_OK) {
        return NULL;
    }
    as->traceInstalled              = 1;

    /* Intercept BOTH objProc2 AND nreProc2, matching the ProcShim pattern.
       In Tcl 9.1, [apply] is NRE-enabled (nreProc2 != NULL), so the bytecode
       engine dispatches through nreProc2 — bypassing objProc2 entirely.
       Without intercepting nreProc2, shimmer recovery for precompiled lambdas
       would silently fail when called from compiled code. */
    as->applyCmdPtr->objProc2       = CmdApplyShim;
    as->applyCmdPtr->objClientData2 = as;
    if (as->applyCmdPtr->nreProc2) {
        as->applyCmdPtr->nreProc2 = CmdApplyShimNre;
    }

    st->applyActive = 1;
    return as;
}

/* RegisterPrecompiledLambda — record a lambda in the ApplyShim registry
 * so that shimmer recovery can re-install the lambdaExpr internal rep. */
static void RegisterPrecompiledLambda(Tcl_Interp *ip, Tcl_Obj *lambda, Proc *procPtr, Tcl_Obj *nsObj) {
    ApplyShim *as = EnsureApplyShim(ip);
    if (!as)
        return;

    /* Hold a reference on the lambda object so that if the ByteCode's
       literal pool releases it, the registry entry remains valid.
       Without this, the ONE_WORD_KEYS hash (keyed by Tcl_Obj pointer)
       could match a stale entry if the address is reused. */
    Tcl_IncrRefCount(lambda);

    int            isNew = 0;
    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&as->lambdaRegistry, (const char *)lambda, &isNew);
    if (!isNew) {
        /* Lambda pointer reused (shouldn't happen, but be safe) */
        ApplyLambdaEntry *old = (ApplyLambdaEntry *)Tcl_GetHashValue(he);
        if (old) {
            if (old->procPtr && --old->procPtr->refCount <= 0) {
                TclProcCleanupProc(old->procPtr);
            }
            if (old->nsObj)
                Tcl_DecrRefCount(old->nsObj);
            Tcl_Free(old);
        }
        Tcl_DecrRefCount(lambda); /* drop extra ref from duplicate registration */
    }

    ApplyLambdaEntry *le = (ApplyLambdaEntry *)Tcl_Alloc(sizeof(ApplyLambdaEntry));
    le->procPtr          = procPtr;
    le->procPtr->refCount++; /* registry holds its own reference */
    le->nsObj = nsObj;
    Tcl_IncrRefCount(le->nsObj);
    Tcl_SetHashValue(he, le);
    if (isNew)
        as->numRegistered++;
}

int Tbcx_ReadHeader(TbcxIn *r, TbcxHeader *H) {
    if (!Tbcx_R_U32(r, &H->magic))
        return 0;
    if (!Tbcx_R_U32(r, &H->format))
        return 0;
    if (!Tbcx_R_U32(r, &H->tcl_version))
        return 0;
    if (!Tbcx_R_U64(r, &H->codeLenTop))
        return 0;
    if (!Tbcx_R_U32(r, &H->numExceptTop))
        return 0;
    if (!Tbcx_R_U32(r, &H->numLitsTop))
        return 0;
    if (!Tbcx_R_U32(r, &H->numAuxTop))
        return 0;
    if (!Tbcx_R_U32(r, &H->numLocalsTop))
        return 0;
    if (!Tbcx_R_U32(r, &H->maxStackTop))
        return 0;

    /* source path LPString, immediately after fixed-size fields.
     * Empty string means the artifact was built from an inline script
     * or channel (no path to preserve).  Non-empty means set
     * iPtr->scriptFile to this during top-level eval so `info script`
     * returns the authored source path. */
    {
        char    *srcP = NULL;
        uint32_t srcL = 0;
        if (!Tbcx_R_LPString(r, &srcP, &srcL))
            return 0;
        if (srcL > 0) {
            H->sourcePath = Tcl_NewStringObj(srcP, (Tcl_Size)srcL);
            Tcl_IncrRefCount(H->sourcePath);
        } else {
            H->sourcePath = NULL;
        }
        Tcl_Free(srcP);
    }

    if (H->magic != TBCX_MAGIC || H->format != TBCX_FORMAT) {
        R_Error(r, "tbcx: bad header (unknown magic or format)");
        return 0;
    }
    {
        uint32_t rt   = Tbcx_PackTclVersion();
        int      hMaj = (int)((H->tcl_version >> 24) & 0xFFu);
        int      hMin = (int)((H->tcl_version >> 16) & 0xFFu);
        int      rMaj = (int)((rt >> 24) & 0xFFu);
        int      rMin = (int)((rt >> 16) & 0xFFu);
        /* Require exact major.minor match.  Bytecode instruction semantics
           can change between minor versions (new opcodes, changed operand
           layouts, etc.) and the raw bytecode bytes are replayed verbatim.
           Allowing cross-minor loading could cause crashes or silent
           misbehavior.  We target 9.1 only. */
        if (hMaj != rMaj || hMin != rMin) {
            R_Error(r, "tbcx: incompatible Tcl version");
            return 0;
        }
    }
    return 1;
}

static int ReadProc(TbcxIn *r, Tcl_Interp *ip, ProcShim *shim, uint32_t procIdx) {
    int      result = TCL_ERROR;

    /* ---- Stage 1: read wire strings ---- */
    char    *nameC  = NULL;
    uint32_t nameL  = 0;
    char    *nsC    = NULL;
    uint32_t nsL    = 0;
    char    *argsC  = NULL;
    uint32_t argsL  = 0;

    if (!Tbcx_R_LPString(r, &nameC, &nameL))
        goto cleanup_strings;
    if (!Tbcx_R_LPString(r, &nsC, &nsL))
        goto cleanup_strings;
    if (!Tbcx_R_LPString(r, &argsC, &argsL))
        goto cleanup_strings;

    /* ---- Stage 2: build Tcl_Obj wrappers ---- */
    Tcl_Obj *nameFqn = Tcl_NewStringObj(nameC, (Tcl_Size)nameL);
    Tcl_IncrRefCount(nameFqn);
    Tcl_Obj *nsObj = Tcl_NewStringObj(nsC, (Tcl_Size)nsL);
    Tcl_IncrRefCount(nsObj);
    Tcl_Obj *argsObj = Tcl_NewStringObj(argsC, (Tcl_Size)argsL);
    Tcl_IncrRefCount(argsObj);
    /* Wire strings no longer needed */
    Tcl_Free(nameC);
    nameC = NULL;
    Tcl_Free(nsC);
    nsC = NULL;
    Tcl_Free(argsC);
    argsC = NULL;

    /* ---- Stage 3: validate ---- */
    {
        Tcl_Size    nsObjLen = 0;
        const char *nsObjStr = Tbcx_GetStringFromObjStrict(ip, nsObj, &nsObjLen);
        if (!nsObjStr || Tbcx_ValidateKeyString(ip, nsObjStr, nsObjLen, "proc namespace", 1) != TCL_OK)
            goto cleanup_objs;
    }
    {
        Tcl_Size    nameObjLen = 0;
        const char *nameObjStr = Tbcx_GetStringFromObjStrict(ip, nameFqn, &nameObjLen);
        if (!nameObjStr || Tbcx_ValidateKeyString(ip, nameObjStr, nameObjLen, "proc name", 0) != TCL_OK)
            goto cleanup_objs;
    }

    /* ---- Stage 3.5: body source text ----
     * Read before the compiled block so we can attach it as the body
     * Tcl_Obj's string rep without a second allocation.  Length==0
     * indicates the artifact was built without -include-source (the
     * default, matching tclcompiler/tbcload); the loader then
     * installs a diagnostic sentinel so `info body` is loud rather
     * than silently empty. */
    char    *bodySrc    = NULL;
    uint32_t bodySrcLen = 0;
    if (!Tbcx_R_LPString(r, &bodySrc, &bodySrcLen))
        goto cleanup_objs;

    /* ---- Stage 4: read body bytecode ---- */
    Namespace *nsPtr  = (Namespace *)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
    uint32_t   nLoc   = 0;
    Tcl_Obj   *bodyBC = Tbcx_ReadBlock(r, ip, nsPtr, &nLoc, 1, 0);
    if (!bodyBC) {
        Tcl_Free(bodySrc);
        goto cleanup_objs;
    }
    Tcl_IncrRefCount(bodyBC); 

    /* ---- Stage 4.5: attach source text ----
     * Tbcx_ReadBlock produces a tbcxTyBytecode-typed Tcl_Obj whose
     * string-rep bytes field is &tclEmptyString (TclNewObj's default —
     * see tclInt.h:4292, not NULL).  Tcl_InitStringRep's three branches
     * behave very differently by bytes-starting-value; the only branch
     * that *copies* from the src argument is the bytes==NULL branch.
     * The bytes==&tclEmptyString branch allocates a new buffer of the
     * requested length but does NOT memcpy from src — Tcl treats that
     * call shape as "allocate space, caller will fill it".
     *
     * We therefore invalidate first (zeroing bytes to NULL), then call
     * Tcl_InitStringRep which follows the allocate-and-copy branch.
     * Neither call touches the internal rep. */
    Tcl_InvalidateStringRep(bodyBC);
    if (bodySrcLen > 0) {
        Tcl_InitStringRep(bodyBC, bodySrc, (size_t)bodySrcLen);
    } else {
        Tcl_InitStringRep(bodyBC,
            TBCX_STRIPPED_SOURCE_SENTINEL,
            sizeof(TBCX_STRIPPED_SOURCE_SENTINEL) - 1u);
    }
    Tcl_Free(bodySrc);
    bodySrc = NULL;

    /* ---- Stage 5: build FQN key ---- */
    Tcl_Obj    *fqnKey = NULL;
    const char *nm     = Tcl_GetString(nameFqn);
    if (nm[0] == ':' && nm[1] == ':') {
        fqnKey = nameFqn;
    } else {
        Tcl_Size    nsLen = 0;
        const char *nsStr = Tcl_GetStringFromObj(nsObj, &nsLen);
        fqnKey            = Tcl_NewStringObj(nsStr, nsLen);
        if (!(nsLen == 2 && nsStr[0] == ':' && nsStr[1] == ':'))
            Tcl_AppendToObj(fqnKey, "::", 2);
        Tcl_AppendObjToObj(fqnKey, nameFqn);
    }
    Tcl_IncrRefCount(fqnKey);
    Tcl_DecrRefCount(nameFqn);
    nameFqn       = NULL; /* consumed — fqnKey owns the reference */

    /* ---- Stage 6: build Proc ---- */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr  = bodyBC;
    Tcl_IncrRefCount(bodyBC);
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size       numA = 0;
        if (Tbcx_BuildLocals(ip, argsObj, &first, &last, &numA) != TCL_OK) {
            Tcl_DecrRefCount(bodyBC);
            Tcl_Free((char *)procPtr);
            goto cleanup_fqn;
        }
        procPtr->numArgs           = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr     = first;
        procPtr->lastLocalPtr      = last;
    }
    CompiledLocals(procPtr, (Tcl_Size)nLoc);

    /* Link ByteCode back to this Proc and refresh epochs */
    {
        ByteCode *bcPtr = TbcxGetByteCode(bodyBC);
        if (bcPtr)
            TbcxFixupByteCode(bcPtr, procPtr, ip, nsPtr, TBCX_FIXUP_CACHE_NONE);
    }

    /* ---- Stage 7: build procbody and register ---- */
    {
        Tcl_Obj           *procBodyObj = Tcl_NewObj();
        Tcl_ObjInternalRep ir;
        ir.twoPtrValue.ptr1 = procPtr;
        ir.twoPtrValue.ptr2 = NULL;
        Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
        procPtr->refCount++;

        Tcl_Obj *pair = Tcl_NewListObj(0, NULL);
        if (Tcl_ListObjAppendElement(ip, pair, argsObj) != TCL_OK || Tcl_ListObjAppendElement(ip, pair, procBodyObj) != TCL_OK) {
            Tcl_IncrRefCount(pair);
            Tcl_DecrRefCount(pair);
            Tcl_IncrRefCount(procBodyObj);
            Tcl_DecrRefCount(procBodyObj);
            /* procBodyObj's freeProc already did procPtr->refCount-- (2→1).
               TclProcCleanupProc does another refCount-- (1→0) and then
               frees locals, bodyPtr, and struct. */
            TclProcCleanupProc(procPtr);
            goto cleanup_fqn;
        }
        Tcl_IncrRefCount(pair);

        int            isNew = 0;
        Tcl_HashEntry *he    = Tcl_CreateHashEntry(&shim->procsByFqn, Tcl_GetString(fqnKey), &isNew);
        if (!isNew) {
            Tcl_Obj *oldPair = (Tcl_Obj *)Tcl_GetHashValue(he);
            if (oldPair)
                Tcl_DecrRefCount(oldPair);
        }
        Tcl_SetHashValue(he, pair);

        if (procIdx < shim->numProcsIdx && shim->procsByIdx) {
            shim->procsByIdx[procIdx] = pair;
            Tcl_IncrRefCount(pair);
        }
    }
    result = TCL_OK;

    /* ---- Cleanup chain ---- */
cleanup_fqn:
    if (fqnKey)
        Tcl_DecrRefCount(fqnKey);
    if (bodyBC)
        Tcl_DecrRefCount(bodyBC); /* release our local reference (F4) */
cleanup_objs:
    if (nameFqn)
        Tcl_DecrRefCount(nameFqn);
    if (nsObj)
        Tcl_DecrRefCount(nsObj);
    if (argsObj)
        Tcl_DecrRefCount(argsObj);
cleanup_strings:
    if (nameC)
        Tcl_Free(nameC);
    if (nsC)
        Tcl_Free(nsC);
    if (argsC)
        Tcl_Free(argsC);
    return result;
}

static void TopLocals_Begin(Tcl_Interp *ip, ByteCode *bcPtr, TbcxTopFrameSave *sv) {
    Interp    *iPtr = (Interp *)ip;

    /* The top-level bytecode is evaluated without TCL_EVAL_GLOBAL (so it
     * matches plain-source `source` semantics: runs in the caller's
     * namespace), which means the engine uses iPtr->varFramePtr — the
     * active frame at eval time — not iPtr->rootFramePtr.  We must
     * install compiled locals on that same frame, otherwise the
     * bytecode's slot-based variable ops find an empty LocalCache and
     * the fall-through to namespace resolution never happens the way
     * the compiler expected.
     *
     * Prior code targeted rootFramePtr because TCL_EVAL_GLOBAL forced
     * the engine onto the root frame; that assumption no longer holds
     * after the namespace fix.  varFramePtr is correct regardless of
     * whether tbcx::load was called from global, namespace-eval, or
     * proc-body context. */
    CallFrame *f    = iPtr->varFramePtr;
    if (!f)
        f = iPtr->rootFramePtr; /* very-early-init defensive fallback */

    memset(sv, 0, sizeof(*sv));
    if (!f)
        return; /* shouldn't happen, but be defensive */
    sv->frame = f;

    /* Save old state */
    sv->oldLocals    = f->compiledLocals;
    sv->oldNum       = f->numCompiledLocals;
    sv->oldCache     = f->localCachePtr;

    /* Borrow the ByteCode's LocalCache and allocate a Var[] of matching size */
    Tcl_Size n       = (bcPtr && bcPtr->localCachePtr) ? bcPtr->localCachePtr->numVars : 0;
    f->localCachePtr = bcPtr ? bcPtr->localCachePtr : NULL;
    if (f->localCachePtr) {
        f->localCachePtr->refCount++;
    }
    if (n <= 0) {
        f->compiledLocals    = NULL;
        f->numCompiledLocals = 0;
        sv->allocated        = NULL;
        return;
    }
    /* Allocate compiled locals and directly link each slot to the target Var
     * in the caller's namespace — NOT the global namespace.  Before the
     * namespace fix this linked against :: because top-level bytecode was
     * forced to run at :: via TCL_EVAL_GLOBAL; now that the eval scope is
     * the caller's, the linkage targets must follow. */
    {
        size_t varBytes;
        if (!tbcx_checked_mul(sizeof(Var), (size_t)n, &varBytes)) {
            /* Overflow — fall back to no compiled locals (safe default) */
            f->compiledLocals    = NULL;
            f->numCompiledLocals = 0;
            sv->allocated        = NULL;
            return;
        }
        f->compiledLocals = (Var *)Tcl_Alloc(varBytes);
        memset(f->compiledLocals, 0, varBytes);
    }
    f->numCompiledLocals = n;
    sv->allocated        = f->compiledLocals;

    if (f->localCachePtr) {
        Tcl_Obj *const *names    = (Tcl_Obj *const *)&f->localCachePtr->varName0; /* Tcl 9.x trailing array */
        Tcl_Namespace  *targetNs = (Tcl_Namespace *)f->nsPtr;
        if (!targetNs)
            targetNs = Tcl_GetCurrentNamespace(ip);
        if (!targetNs)
            targetNs = Tcl_GetGlobalNamespace(ip);
        for (Tcl_Size i = 0; i < n; i++) {
            Tcl_Obj *nm = names ? names[i] : NULL;
            if (!nm)
                continue;
            Tcl_Size    len = 0;
            const char *s   = Tbcx_GetStringFromObjSafe(nm, &len);
            if (len == 0)
                continue; /* temp / unnamed */
            if (memchr(s, ':', (size_t)len))
                continue; /* skip qualified */
            /* Link to existing variable in the caller's namespace if it
               exists.  Do NOT create the variable — that would differ
               from source semantics where variables are only created on
               first assignment. */
            Tcl_Var vHandle = Tcl_FindNamespaceVar(ip, s, targetNs, 0);
            if (vHandle) {
                Var *target = (Var *)vHandle; /* internal Var*, OK here */
                Var *dst    = &f->compiledLocals[i];
                memset(dst, 0, sizeof(Var));
                dst->flags         = VAR_LINK;
                dst->value.linkPtr = target;
            }
        }
    }
}

static void TopLocals_End(Tcl_Interp *ip, TbcxTopFrameSave *sv) {
    (void)ip;
    /* Restore the exact same frame that TopLocals_Begin modified
     * (recorded in sv->frame at Begin time).  Using iPtr->varFramePtr
     * here would be wrong if the eval pushed/popped frames in between. */
    CallFrame *f    = sv->frame;
    if (!f)
        return; /* defensive */

    /* Drop what we lent to the frame */
    if (f->localCachePtr) {
        f->localCachePtr->refCount--;
        /* Do NOT free the cache here even if refCount reaches 0.
           The ByteCode's cleanup (TclCleanupByteCode) is responsible for
           freeing the LocalCache when the ByteCode is destroyed. Freeing
           it here would cause a use-after-free. */
    }
    if (sv->allocated)
        Tcl_Free((char *)sv->allocated);

    /* Restore previous state */
    f->compiledLocals    = sv->oldLocals;
    f->numCompiledLocals = sv->oldNum;
    f->localCachePtr     = sv->oldCache;
}

#define TBCX_MAX_LOAD_DEPTH 8

static int LoadTbcxStream(Tcl_Interp *ip, Tcl_Channel ch, Tcl_Obj *scriptFilePath) {
    TbcxInterpState *st = TbcxGetInterpState(ip);
    if (st->loadDepth >= TBCX_MAX_LOAD_DEPTH) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx::load: reentrancy depth %" TCL_SIZE_MODIFIER "d exceeds limit %d", st->loadDepth, TBCX_MAX_LOAD_DEPTH));
        return TCL_ERROR;
    }
    st->loadDepth++;

    TbcxIn r;
    Tbcx_R_Init(&r, ip, ch);
    TbcxHeader H;
    memset(&H, 0, sizeof(H));  /* zero H.sourcePath for the early-exit path */

    if (Tbcx_CheckBinaryChan(ip, ch) != TCL_OK) {
        st->loadDepth--;
        return TCL_ERROR;
    }

    if (!Tbcx_ReadHeader(&r, &H) || r.err) {
        if (H.sourcePath) {
            Tcl_DecrRefCount(H.sourcePath);
        }
        st->loadDepth--;
        return TCL_ERROR;
    }

    /* Resolve the namespace where the top-level block should run.
     *
     * Plain-source equivalence: the canonical `moduleLoad`-style wrapper
     * for tbcx looks like
     *     uplevel 1 [list source $path]    ;# plain-source branch
     *     tbcx::load $path                  ;# tbcx branch
     * and callers expect the two branches to be semantically identical.
     * Tcl's `source` evaluates the file's top-level commands in the
     * caller's current namespace — a bare `$dir` inside the sourced
     * file resolves against that namespace, not against ::.
     *
     * For the tbcx branch to match, LoadTbcxStream must:
     *   (a) associate the top-level ByteCode with the caller's current
     *       namespace (so codePtr->nsPtr reflects the scope the code
     *       will actually run in — important for the recompile-check
     *       in TclProcCompileProc et al., and for lookupNsPtr resolution
     *       of variables and commands), and
     *   (b) evaluate the top-level block without TCL_EVAL_GLOBAL, so
     *       the engine does not switch to global scope before running
     *       the first opcode.
     *
     * Before this change both points were wrong: `::` was hard-coded
     * here and TCL_EVAL_GLOBAL was passed below.  The combined effect
     * forced every top-level block to execute at `::` regardless of
     * the caller's namespace, which meant that any tbcx-loaded script
     * with bare `$var` or `variable x` at its top — including the
     * ooxml scripts/text module and the `namespace eval ::wg3 {...}`
     * blocks in scripts/xml and scripts/toc — resolved those against
     * the wrong namespace and failed with `can't read "<name>": no
     * such variable`. */
    Namespace *curNs   = (Namespace *)Tcl_GetCurrentNamespace(ip);
    uint32_t   dummyNL = 0;

    Tcl_Obj   *topBC   = Tbcx_ReadBlock(&r, ip, curNs, &dummyNL, 1, 0);
    if (!topBC) {
        /* Release H.sourcePath — it was IncrRefCount'd by Tbcx_ReadHeader
         * on the success path; without this guard it leaks on every
         * failed load after a successful header read.  The dumper's
         * `cleanup_no_topbc:` label shows the correct pattern. */
        if (H.sourcePath) {
            Tcl_DecrRefCount(H.sourcePath);
        }
        st->loadDepth--;
        return TCL_ERROR;
    }
    Tcl_IncrRefCount(topBC); /* protect against all early-return paths */

    int      rc           = TCL_ERROR;
    int      shimInited   = 0; /* 1 once AddProcShim succeeded */
    int      ooshimInited = 0; /* 1 once AddOOShim succeeded */

    ProcShim shim;
    memset(&shim, 0, sizeof(shim)); /* zero up front so DelProcShim is safe */
    OOShim ooshim;
    memset(&ooshim, 0, sizeof(ooshim));

    /* Procs */
    uint32_t numProcs = 0;
    if (!Tbcx_R_U32(&r, &numProcs))
        goto cleanup;
    if (numProcs > TBCX_MAX_PROCS) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numProcs %u exceeds limit %u", numProcs, TBCX_MAX_PROCS));
        goto cleanup;
    }

    /* Build proc shim registry and fill from section */
    if (numProcs) {
        if (AddProcShim(ip, &shim) != TCL_OK)
            goto cleanup;
        shimInited      = 1;
        shim.procsByIdx = (Tcl_Obj **)Tcl_AttemptAlloc(sizeof(Tcl_Obj *) * numProcs);
        if (!shim.procsByIdx) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: allocation failed (proc index)", -1));
            goto cleanup;
        }
        memset(shim.procsByIdx, 0, sizeof(Tcl_Obj *) * numProcs);
        shim.numProcsIdx = numProcs;
    }

    for (uint32_t i = 0; i < numProcs; i++) {
        if (ReadProc(&r, ip, &shim, i) != TCL_OK)
            goto cleanup;
    }

    /* Classes section (saver currently emits 0) */
    uint32_t numClasses = 0;
    if (!Tbcx_R_U32(&r, &numClasses))
        goto cleanup;
    if (numClasses > TBCX_MAX_CLASSES) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numClasses %u exceeds limit %u", numClasses, TBCX_MAX_CLASSES));
        goto cleanup;
    }
    for (uint32_t c = 0; c < numClasses; c++) {
        /* classFqn + nSupers + supers… — saver writes 0; ignore here */
        char    *cls = NULL;
        uint32_t cl  = 0;
        if (!Tbcx_R_LPString(&r, &cls, &cl))
            goto cleanup;
        Tcl_Free(cls);
        uint32_t nSup = 0;
        if (!Tbcx_R_U32(&r, &nSup))
            goto cleanup;
        if (nSup > 1024u) {
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: nSuperclasses %u exceeds limit 1024", nSup));
            goto cleanup;
        }
        for (uint32_t s = 0; s < nSup; s++) {
            char    *su = NULL;
            uint32_t sl = 0;
            if (!Tbcx_R_LPString(&r, &su, &sl))
                goto cleanup;
            Tcl_Free(su);
        }
    }
    uint32_t numMethods = 0;
    if (!Tbcx_R_U32(&r, &numMethods))
        goto cleanup;
    if (numMethods > TBCX_MAX_METHODS) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numMethods %u exceeds limit %u", numMethods, TBCX_MAX_METHODS));
        goto cleanup;
    }
    if (numMethods) {
        if (AddOOShim(ip, &ooshim) != TCL_OK)
            goto cleanup;
        ooshimInited = 1;
    }
    for (uint32_t m = 0; m < numMethods; m++) {
        if (ReadMethod(&r, ip, &ooshim) != TCL_OK)
            goto cleanup;
    }

    /* Execute */
    {
        TbcxTopFrameSave _sv;

        ByteCode        *top     = TbcxGetByteCode(topBC);

        /* Create a minimal Proc and attach to the top-level ByteCode so
           that codePtr->procPtr is never NULL during TEBCresume.
           Also propagate via FixLiteralPoolProcPtr to any precompiled
           literal-pool bytecodes (TBCX_LIT_BYTESRC). */
        Proc            *topProc = NULL;
        if (top && !top->procPtr) {
            topProc = (Proc *)Tcl_Alloc(sizeof(Proc));
            memset(topProc, 0, sizeof(Proc));
            topProc->iPtr     = (Interp *)ip;
            topProc->refCount = 1;
            topProc->bodyPtr  = topBC;
            Tcl_IncrRefCount(topProc->bodyPtr);
            topProc->numArgs           = 0;
            topProc->numCompiledLocals = (top->localCachePtr) ? top->localCachePtr->numVars : 0;
            top->procPtr               = topProc;
            TbcxFixupByteCode(top, topProc, ip, curNs, TBCX_FIXUP_CACHE_NONE);
        }

        if (top)
            TbcxVerifyLoadedBC(top, ip, "top-level");

        TopLocals_Begin(ip, top, &_sv);

        /* Save and set iPtr->scriptFile around the top-level eval so
         * that `info script` inside the loaded bytecode returns the
         * authored source path — matching Tcl's Tcl_FSEvalFileEx
         * behavior (tclIOUtil.c:1806-1807) for sourced scripts.
         *
         * Priority: use H.sourcePath (the path the artifact was built
         * from) when the save side recorded one, because that's what
         * `source` would have returned.  Only if the artifact has no
         * recorded source path (empty LPString — built from inline
         * script or channel) do we fall back to the tbcx artifact path
         * passed in as scriptFilePath; this still satisfies the
         * self-invocation guard idiom `[info script] eq $::argv0`
         * because the .tbcx path differs from the outer argv0.
         *
         * Both common idioms then work for tbcx-loaded modules:
         *     set dir [file dirname [info script]]      ;# asset-relative
         *     if {[info script] eq $::argv0} { ... }    ;# self-guard
         */
        Interp  *iPtrLocal    = (Interp *)ip;
        Tcl_Obj *oldScript    = iPtrLocal->scriptFile;
        Tcl_Obj *effectivePath = H.sourcePath ? H.sourcePath : scriptFilePath;
        if (effectivePath) {
            iPtrLocal->scriptFile = effectivePath;
            Tcl_IncrRefCount(iPtrLocal->scriptFile);
        }

        rc = Tcl_EvalObjEx(ip, topBC, 0);

        if (effectivePath) {
            if (iPtrLocal->scriptFile) {
                Tcl_DecrRefCount(iPtrLocal->scriptFile);
            }
            iPtrLocal->scriptFile = oldScript;
        }

        TopLocals_End(ip, &_sv);

        if (topProc) {
            /* Recursively null out non-owning procPtr backpointers in
               literal pool bytecodes before freeing topProc.  This
               covers the full descendant tree, not just immediate
               children.  No refCount adjustment needed — these are
               non-owning backpointers. */
            if (top)
                NullLiteralPoolProcPtr(top, topProc);
            if (top)
                top->procPtr = NULL;
            Tcl_DecrRefCount(topProc->bodyPtr);
            topProc->bodyPtr = NULL;
            assert(topProc->refCount > 0 && "topProc refCount underflow before free");
            topProc->refCount--;
            Tcl_Free((char *)topProc);
        }

        /* Handle TCL_RETURN the same way Tcl's 'source' command
           (Tcl_FSEvalFileEx) does. */
        if (rc == TCL_RETURN) {
            rc = TclUpdateReturnInfo((Interp *)ip);
        }
    }

cleanup:
    if (H.sourcePath) {
        Tcl_DecrRefCount(H.sourcePath);
        H.sourcePath = NULL;
    }
    Tcl_DecrRefCount(topBC);
    if (ooshimInited)
        DelOOShim(ip, &ooshim);
    if (shimInited)
        DelProcShim(ip, &shim);
    st->loadDepth--;
    return rc;
}

/* ==========================================================================
 * Tcl command: tbcx::load
 *
 * Synopsis:   tbcx::load in
 * Arguments:  in — input source: an open binary channel name, or a
 *                   filesystem path to a .tbcx file.
 * Returns:    The result of evaluating the deserialized top-level bytecode.
 * Errors:     TCL_ERROR on read/parse failure, malformed .tbcx stream,
 *             or runtime error during top-level evaluation.
 * Thread:     Must be called on the interp-owning thread.  Temporarily
 *             shims the [proc] and [oo::define] commands during load to
 *             intercept and substitute precompiled bodies; restores them
 *             on all exit paths (success or error).
 * ========================================================================== */

int Tbcx_LoadObjCmd(TCL_UNUSED(void *), Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    TBCX_CHECK_INTERP_THREAD(interp);
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "in");
        return TCL_ERROR;
    }

    Tcl_Obj    *inObj = objv[1];
    Tcl_Channel inCh  = NULL;

    if (Tbcx_ProbeOpenChannel(interp, inObj, &inCh)) {
        /* Channel branch: no reliable path to use for iPtr->scriptFile;
         * pass NULL so LoadTbcxStream leaves scriptFile alone.  Callers
         * that use an already-open channel typically don't care about
         * `info script` anyway. */
        return LoadTbcxStream(interp, inCh, NULL);
    }

    if (Tbcx_ProbeReadableFile(interp, inObj)) {
        Tcl_Channel ch = Tcl_FSOpenFileChannel(interp, inObj, "r", 0);
        if (!ch) {
            return TCL_ERROR;
        }
        /* File branch: pass the .tbcx path so `info script` inside
         * the loaded top-level block returns the artifact's path —
         * matching Tcl_FSEvalFileEx's scriptFile handling (tclIOUtil.c
         * lines 1806-1807).  This is what `source` does, and any
         * sourced script that checks `[info script] eq $::argv0` as
         * a self-invocation guard (standard Tcl idiom) depends on
         * that value being distinct from the outer script's path. */
        int rc = LoadTbcxStream(interp, ch, inObj);
        if (Tcl_Close(interp, ch) != TCL_OK) {
            rc = TCL_ERROR;
        }
        return rc;
    }

    Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx::load: input is neither an open channel nor a readable file", -1));
    Tcl_SetErrorCode(interp, "TBCX", "LOAD", "BADINPUT", NULL);
    return TCL_ERROR;
}
