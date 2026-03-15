/* ==========================================================================
 * tbcxload.c — TBCX load+eval for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * File-local globals
 * ========================================================================== */

/* tbcxHiddenId: monotonically increasing counter used to generate unique
 * rename targets for OO shim commands.  Protected by tbcxHiddenIdMutex. */
static unsigned int tbcxHiddenId = 0;

/* tbcxHiddenIdMutex: protects tbcxHiddenId.  Lock-order position: leaf —
 * no other TBCX mutex may be held while this is held. */
TCL_DECLARE_MUTEX(tbcxHiddenIdMutex);

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct
{ /* minimal prefix used by TclInitByteCodeObj in our path */
    Tcl_Interp* interp;
    Namespace* nsPtr;
    unsigned char *codeStart, *codeNext;
    Tcl_Obj** objArrayPtr;
    Tcl_Size numLitObjects;
    AuxData* auxDataArrayPtr;
    Tcl_Size numAuxDataItems;
    ExceptionRange* exceptArrayPtr;
    Tcl_Size numExceptRanges;
    Tcl_Size maxStackDepth;
    Proc* procPtr;
} TBCX_CompileEnvMin;

typedef struct
{
    Tcl_HashTable procsByFqn;       /* key: FQN Tcl_Obj*, val: procbody Tcl_Obj* */
    Tcl_Obj** procsByIdx;           /* indexed array [0..numProcsIdx-1] for marker lookup */
    uint32_t numProcsIdx;           /* size of procsByIdx array */
    Command* procCmdPtr;            /* the "proc" Command struct (NULL if invalidated) */
    Tcl_Interp* interp;             /* owning interpreter (for trace removal) */
    int traceInstalled;             /* 1 if command trace is active on "proc" */
    Tcl_ObjCmdProc2* savedObjProc2; /* saved objProc2 handler */
    Tcl_ObjCmdProc2* savedNreProc2; /* saved nreProc2 handler (may be NULL) */
    void* savedClientData2;         /* saved objClientData2 */
    /* handler pointers captured from first successful proc creation,
       used for direct registration of subsequent procs. */
    Tcl_ObjCmdProc2* procDispatchObj;  /* objProc2 on created proc Command */
    Tcl_ObjCmdProc2* procDispatchNre;  /* nreProc2 on created proc Command */
    Tcl_CmdDeleteProc* procDeleteProc; /* deleteProc on created proc Command */
    int haveDispatch;                  /* 1 once pointers captured */
} ProcShim;

typedef struct
{
    Tcl_HashTable methodsByKey;               /* key: STRING "class\x1Fkind\x1Fname", val: Tcl_Obj* PAIR {args, procbody} */
    char origName[TBCX_OSHIM_NAME_MAX];       /* unique rename target for oo::define (reentrancy-safe) */
    char origNameObjDef[TBCX_OSHIM_NAME_MAX]; /* unique rename target for oo::objdefine */
    int hasObjDefine;                         /* 1 if oo::objdefine was successfully shimmed */
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
#define TBCX_APPLY_SHIM_KEY "tbcx::applyShim"

typedef struct
{
    Proc* procPtr;  /* precompiled Proc (we hold a refcount) */
    Tcl_Obj* nsObj; /* namespace for lambdaExpr internal rep */
} ApplyLambdaEntry;

typedef struct
{
    Command* applyCmdPtr; /* cached "apply" Command (NULL if invalidated) */
    Tcl_Interp* interp;   /* owning interpreter (for trace removal) */
    int traceInstalled;   /* 1 if command trace is active on "apply" */
    Tcl_ObjCmdProc2* savedApplyProc;
    Tcl_ObjCmdProc2* savedApplyNre; /* saved nreProc2 handler (may be NULL) */
    void* savedApplyCD;
    Tcl_HashTable lambdaRegistry; /* key: ONE_WORD (Tcl_Obj *), val: ApplyLambdaEntry* */
} ApplyShim;

typedef struct
{
    Var* oldLocals;
    Tcl_Size oldNum;
    LocalCache* oldCache;
    Var* allocated;
} TbcxTopFrameSave;

/* Runaway detection limits */
#define TBCX_MAX_LITERAL_DEPTH 64
#define TBCX_MAX_CONTAINER_ELEMS (16u * 1024u * 1024u)

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static int AddOOShim(Tcl_Interp* ip, OOShim* os);
static int AddProcShim(Tcl_Interp* ip, ProcShim* ps);
static void ApplyCmdDeleteTrace(void* cd, Tcl_Interp* interp, const char* oldName, const char* newName, int flags);
static void ApplyShimCleanup(void* cd, Tcl_Interp* ip);
static void ApplyShimPurgeStale(ApplyShim* as);
static Tcl_Obj* ByteCodeObj(Tcl_Interp* ip,
                            Namespace* nsPtr,
                            const unsigned char* code,
                            uint32_t codeLen,
                            Tcl_Obj** lits,
                            uint32_t numLits,
                            AuxData* auxArr,
                            uint32_t numAux,
                            ExceptionRange* exArr,
                            uint32_t numEx,
                            int maxStackDepth,
                            int setPrecompiled);
static int CmdApplyShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[]);
static int CmdApplyShimNre(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[]);
static int CmdOOShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[]);
static int CmdOOShimObjDef(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[]);
static int CmdProcShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[]);
static void CompiledLocals(Proc* procPtr, int neededCount);
static int DefOO(Tcl_Interp* ip, OOShim* os, Tcl_Obj* clsFqn, uint8_t kind, Tcl_Obj* nameOpt, Tcl_Obj* argsOpt, Tcl_Obj* bodyOpt);
static void DelOOShim(Tcl_Interp* ip, OOShim* os);
static void DelProcShim(Tcl_Interp* ip, ProcShim* ps);
static ApplyShim* EnsureApplyShim(Tcl_Interp* ip);
static void FixCompiledLocalNames(Proc* procPtr, LocalCache* lc);
static int LoadTbcxStream(Tcl_Interp* ip, Tcl_Channel ch);
static void MethodKeyBuf(Tcl_DString* ds, Tcl_Obj* clsFqn, uint8_t kind, Tcl_Obj* name);
static void OOShim_IdentifyMethod(const char* subc,
                                  Tcl_Size objc,
                                  Tcl_Obj* const objv[],
                                  Tcl_Obj* clsFqn,
                                  Tcl_DString* keyDs,
                                  uint8_t* kindOut,
                                  int* bodyIdxOut,
                                  Tcl_Obj** runtimeArgsOut,
                                  Tcl_Obj** nameOOut,
                                  Tcl_Obj** tmpEmptyArgsOut,
                                  int* hasKeyOut);
static void OOShim_LookupPair(Tcl_Interp* ip,
                              OOShim* os,
                              Tcl_DString* keyDs,
                              int hasKey,
                              int bodyIdx,
                              Tcl_Obj* runtimeArgs,
                              Tcl_Obj** savedArgsOut,
                              Tcl_Obj** preBodyOut);
static int PrecompClass(Tcl_Interp* ip, OOShim* os, Tcl_Obj* clsFqn);
static void ProcCmdDeleteTrace(void* cd, Tcl_Interp* interp, const char* oldName, const char* newName, int flags);
static int
ProcShim_DirectInstall(ProcShim* ps, Tcl_Interp* ip, Tcl_Obj* fqn, Tcl_Obj* nameObj, Proc* preProc, Tcl_Obj* savedArgs);
static inline void R_Error(TbcxIn* r, const char* msg);
static int ReadAuxArray(TbcxIn* r, AuxData** auxOut, uint32_t* numAuxOut);
static int ReadExceptions(TbcxIn* r, ExceptionRange** exOut, uint32_t* numOut);
static Tcl_Obj* ReadLit_Bignum(TbcxIn* r);
static Tcl_Obj* ReadLit_LambdaBC(TbcxIn* r, Tcl_Interp* ip, int depth, int dumpOnly);
static Tcl_Obj* ReadLiteral(TbcxIn* r, Tcl_Interp* ip, int depth, int dumpOnly);
static int ReadMethod(TbcxIn* r, Tcl_Interp* ip, OOShim* os);
static int ReadProc(TbcxIn* r, Tcl_Interp* ip, ProcShim* shim, uint32_t procIdx);
static inline void RefreshBC(ByteCode* bcPtr, Tcl_Interp* ip, Namespace* nsPtr);
static void RegisterPrecompiledLambda(Tcl_Interp* ip, Tcl_Obj* lambda, Proc* procPtr, Tcl_Obj* nsObj);
Tcl_Namespace* Tbcx_EnsureNamespace(Tcl_Interp* ip, const char* fqn);
int Tbcx_LoadObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);
inline int Tbcx_R_Bytes(TbcxIn* r, void* p, Tcl_Size n);
void Tbcx_R_Init(TbcxIn* r, Tcl_Interp* ip, Tcl_Channel ch);
inline int Tbcx_R_LPString(TbcxIn* r, char** sp, uint32_t* lenp);
inline int Tbcx_R_U32(TbcxIn* r, uint32_t* vp);
inline int Tbcx_R_U64(TbcxIn* r, uint64_t* vp);
inline int Tbcx_R_U8(TbcxIn* r, uint8_t* v);
Tcl_Obj*
Tbcx_ReadBlock(TbcxIn* r, Tcl_Interp* ip, Namespace* nsForDefault, uint32_t* numLocalsOut, int setPrecompiled, int dumpOnly);
int Tbcx_ReadHeader(TbcxIn* r, TbcxHeader* H);
void TbcxApplyShimPurgeAll(Tcl_Interp* ip);
static ByteCode* TbcxByteCode(Tcl_Obj* objPtr, const Tcl_ObjType* typePtr, const TBCX_CompileEnvMin* env, int setPrecompiled);
static void TbcxFixLocalCacheExtras(ByteCode* bcPtr, Proc* procPtr);
static void TopLocals_Begin(Tcl_Interp* ip, ByteCode* bcPtr, TbcxTopFrameSave* sv);
static void TopLocals_End(Tcl_Interp* ip, TbcxTopFrameSave* sv);

/* ==========================================================================
 * Buffered Read I/O & Utilities
 *
 * LIMITATION (TODO TBCX-10): All I/O in the load, save, and dump paths is
 * synchronous and blocking on the interpreter thread.  This violates
 * low-latency requirements for event-loop-driven deployments.  Fixing
 * this requires an async/coroutine-based I/O architecture.
 * Safe temporary invariant: all current callers (tbcx::save, tbcx::load,
 * tbcx::dump) are Tcl_ObjCmdProc2 implementations invoked synchronously
 * by the event loop, so blocking does not starve other file handlers —
 * it simply extends the command's wall-clock time.
 * ========================================================================== */

static inline void R_Error(TbcxIn* r, const char* msg)
{
    if (r->err == TCL_OK)
    {
        Tcl_SetObjResult(r->interp, Tcl_NewStringObj(msg, -1));
        r->err = TCL_ERROR;
    }
}

void Tbcx_R_Init(TbcxIn* r, Tcl_Interp* ip, Tcl_Channel ch)
{
    r->interp = ip;
    r->chan = ch;
    r->err = TCL_OK;
    r->bufPos = 0;
    r->bufFill = 0;
}

inline int Tbcx_R_Bytes(TbcxIn* r, void* p, Tcl_Size n)
{
    if (r->err)
        return 0;
    if (n == 0)
        return 1;
    unsigned char* dst = (unsigned char*)p;
    Tcl_Size rem = n;
    while (rem > 0)
    {
        /* Serve from buffer first */
        Tcl_Size avail = r->bufFill - r->bufPos;
        if (avail > 0)
        {
            Tcl_Size chunk = (rem < avail) ? rem : avail;
            memcpy(dst, r->buf + r->bufPos, (size_t)chunk);
            r->bufPos += chunk;
            dst += chunk;
            rem -= chunk;
            continue;
        }
        /* Buffer empty — refill */
        Tcl_Size got = Tcl_ReadRaw(r->chan, (char*)r->buf, (Tcl_Size)TBCX_BUFSIZE);
        if (got < 0)
        {
            R_Error(r, "tbcx: I/O error during read");
            return 0;
        }
        if (got == 0)
        {
            R_Error(r, "tbcx: unexpected EOF (short read)");
            return 0;
        }
        r->bufPos = 0;
        r->bufFill = got;
    }
    return 1;
}

inline int Tbcx_R_U8(TbcxIn* r, uint8_t* v)
{
    return Tbcx_R_Bytes(r, v, 1);
}

inline int Tbcx_R_U32(TbcxIn* r, uint32_t* vp)
{
    uint32_t v = 0;
    if (!Tbcx_R_Bytes(r, &v, 4))
        return 0;
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed))
    {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    *vp = v;
    return 1;
}

inline int Tbcx_R_U64(TbcxIn* r, uint64_t* vp)
{
    uint64_t v = 0;
    if (!Tbcx_R_Bytes(r, &v, 8))
        return 0;
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed))
    {
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) |
            ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
            ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }
    *vp = v;
    return 1;
}

inline int Tbcx_R_LPString(TbcxIn* r, char** sp, uint32_t* lenp)
{
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_STR)
    {
        R_Error(r, "tbcx: LPString too large");
        return 0;
    }
    char* buf = (char*)Tcl_Alloc(n + 1u);
    if (n && !Tbcx_R_Bytes(r, buf, n))
    {
        Tcl_Free(buf);
        return 0;
    }
    buf[n] = '\0';
    *sp = buf;
    *lenp = n;
    return 1;
}

static inline void RefreshBC(ByteCode* bcPtr, Tcl_Interp* ip, Namespace* nsPtr)
{
    if (!bcPtr)
        return;
    bcPtr->compileEpoch = ((Interp*)ip)->compileEpoch;
    bcPtr->nsPtr = nsPtr;
    bcPtr->nsEpoch = nsPtr ? nsPtr->resolverEpoch : 0;
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
static void TbcxFixLocalCacheExtras(ByteCode* bcPtr, Proc* procPtr)
{
    if (!bcPtr || !procPtr || !bcPtr->localCachePtr)
        return;

    LocalCache* old = bcPtr->localCachePtr;
    Tcl_Size numVars = old->numVars;
    int numArgs = procPtr->numArgs;

    /* Allocate new cache with the extras area */
    size_t bytes = offsetof(LocalCache, varName0) + (size_t)numVars * sizeof(Tcl_Obj*) + (size_t)numArgs * sizeof(Var);
    LocalCache* lc = (LocalCache*)Tcl_Alloc(bytes);
    memset(lc, 0, bytes);
    lc->refCount = 1;
    lc->numVars = numVars;

    /* Copy variable names from the old (TBCX) cache */
    Tcl_Obj** oldNames = (Tcl_Obj**)&old->varName0;
    Tcl_Obj** newNames = (Tcl_Obj**)&lc->varName0;
    for (Tcl_Size i = 0; i < numVars; i++)
    {
        newNames[i] = oldNames[i];
        if (newNames[i])
            Tcl_IncrRefCount(newNames[i]);
    }

    /* Populate the extras area from the Proc's CompiledLocal chain.
       Each argument gets: flags (VAR_IS_ARGS bit) and defValuePtr. */
    if (numArgs > 0)
    {
        Var* varPtr = (Var*)(newNames + numVars);
        CompiledLocal* cl = procPtr->firstLocalPtr;
        for (int a = 0; a < numArgs && cl; a++, cl = cl->nextPtr)
        {
            varPtr[a].flags = (cl->flags & VAR_IS_ARGS);
            varPtr[a].value.objPtr = cl->defValuePtr;
        }
    }

    /* Release old cache and install new one */
    if (--old->refCount <= 0)
    {
        for (Tcl_Size i = 0; i < old->numVars; i++)
        {
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
 * Tcl_DStringFree(ds) when done. */
static void MethodKeyBuf(Tcl_DString* ds, Tcl_Obj* clsFqn, uint8_t kind, Tcl_Obj* name)
{
    Tcl_DStringInit(ds);
    Tcl_Size fqnLen = 0;
    const char* fqn = Tcl_GetStringFromObj(clsFqn, &fqnLen);
    Tcl_DStringAppend(ds, fqn, fqnLen);
    Tcl_DStringAppend(ds, "\x1F", 1);
    {
        Tcl_Obj* kindObj = Tcl_ObjPrintf("%u", (unsigned)kind);
        Tcl_IncrRefCount(kindObj);
        Tcl_DStringAppend(ds, Tcl_GetString(kindObj), -1);
        Tcl_DecrRefCount(kindObj);
    }
    Tcl_DStringAppend(ds, "\x1F", 1);
    if (name)
    {
        Tcl_Size nLen = 0;
        const char* n = Tcl_GetStringFromObj(name, &nLen);
        Tcl_DStringAppend(ds, n, nLen);
    }
}

static int DefOO(Tcl_Interp* ip, OOShim* os, Tcl_Obj* clsFqn, uint8_t kind, Tcl_Obj* nameOpt, Tcl_Obj* argsOpt, Tcl_Obj* bodyOpt)
{
    int rc = TCL_OK;
    Tcl_Obj* argv0 = Tcl_NewStringObj(os->origName, -1);
    Tcl_IncrRefCount(argv0);

    /* Fast path for methods (instance/class): always direct */
    if (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS)
    {
        Tcl_Obj* sub = Tcl_NewStringObj((kind == TBCX_METH_CLASS) ? "classmethod" : "method", -1);
        Tcl_IncrRefCount(sub);
        Tcl_IncrRefCount(nameOpt);
        Tcl_IncrRefCount(argsOpt);
        Tcl_IncrRefCount(bodyOpt);
        Tcl_Obj* av[6] = {argv0, clsFqn, sub, nameOpt, argsOpt, bodyOpt};
        rc = Tcl_EvalObjv(ip, 6, av, 0);
        Tcl_DecrRefCount(bodyOpt);
        Tcl_DecrRefCount(argsOpt);
        Tcl_DecrRefCount(nameOpt);
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Constructors/Destructors: TclOO's constructor and destructor creation
       paths do NOT honor the procbody internal rep — they always recompile
       from the body's string rep.

       Fix: create the ctor/dtor normally (with an empty body) so TclOO
       establishes proper dispatch context — in particular, 'next' inside
       a constructor routes through the constructor chain (not the method
       chain).  Then locate the Proc that TclOO created and swap its bodyPtr
       with the precompiled bytecode.

       Previous approach (hidden method + wrapper) broke 'next' because the
       precompiled body ran as a method, where 'next' looks for the next
       *method* — not the next constructor. */
    int hasPrecompiled = (bodyOpt && bodyOpt->typePtr == tbcxTyProcBody) ? 1 : 0;
    if ((kind == TBCX_METH_CTOR || kind == TBCX_METH_DTOR) && hasPrecompiled)
    {
        /* Extract precompiled Proc from the procbody object */
        const Tcl_ObjInternalRep* pbIR = Tcl_FetchInternalRep(bodyOpt, tbcxTyProcBody);
        Proc* preProc = pbIR ? (Proc*)pbIR->twoPtrValue.ptr1 : NULL;

        /* Step 1: Create ctor/dtor with empty body via the renamed original
           oo::define.  TclOO builds the full Method/Proc/dispatch structure,
           so 'next' etc. work in the correct context. */
        /* CRITICAL: The placeholder body MUST be non-empty.  TclOO's
           TclOODefineConstructorObjCmd (and destructor equivalent) checks
           the body length: if it's 0, it DELETES the ctor/dtor entirely
           (sets constructorPtr/destructorPtr to NULL) instead of creating
           a Method with a Proc.  A semicolon is the minimal no-op body
           that produces a real Method we can swap the bytecode into. */
        Tcl_Obj* placeholderBody = Tcl_NewStringObj(";", 1);
        Tcl_IncrRefCount(placeholderBody);

        if (kind == TBCX_METH_CTOR)
        {
            Tcl_Obj* sub = Tcl_NewStringObj("constructor", -1);
            Tcl_IncrRefCount(sub);
            Tcl_IncrRefCount(argsOpt);
            Tcl_Obj* cav[5] = {argv0, clsFqn, sub, argsOpt, placeholderBody};
            rc = Tcl_EvalObjv(ip, 5, cav, 0);
            Tcl_DecrRefCount(argsOpt);
            Tcl_DecrRefCount(sub);
        }
        else /* TBCX_METH_DTOR */
        {
            Tcl_Obj* sub = Tcl_NewStringObj("destructor", -1);
            Tcl_IncrRefCount(sub);
            Tcl_Obj* dav[4] = {argv0, clsFqn, sub, placeholderBody};
            rc = Tcl_EvalObjv(ip, 4, dav, 0);
            Tcl_DecrRefCount(sub);
        }
        Tcl_DecrRefCount(placeholderBody);

        if (rc != TCL_OK)
        {
            Tcl_DecrRefCount(argv0);
            return rc;
        }

        /* Step 2: Locate the Proc that TclOO just created and swap its
           bodyPtr with the precompiled bytecode.  This preserves the full
           TclOO constructor/destructor dispatch chain.

           Uses the public TclOO API (Tcl_GetObjectFromObj, Tcl_GetObjectAsClass)
           which goes through tclOOStubsPtr — initialized in tbcx_Init via
           TclOOInitializeStubs.  Internal struct access (Class*, Method*,
           ProcedureMethod*) uses tclOOInt.h as throughout tbcx. */
        if (preProc && preProc->bodyPtr)
        {
            Tcl_Object tclObj = Tcl_GetObjectFromObj(ip, clsFqn);
            Class* clsPtr = tclObj ? (Class*)Tcl_GetObjectAsClass(tclObj) : NULL;
            if (clsPtr)
            {
                Method* meth = (kind == TBCX_METH_CTOR) ? clsPtr->constructorPtr : clsPtr->destructorPtr;
                if (meth && meth->clientData)
                {
                    ProcedureMethod* pmPtr = (ProcedureMethod*)meth->clientData;
                    if (pmPtr && pmPtr->procPtr)
                    {
                        Proc* ctorProc = pmPtr->procPtr;

                        /* Swap bodyPtr: drop the empty-body bytecode,
                           install the precompiled one. */
                        Tcl_Obj* preBody = preProc->bodyPtr;
                        Tcl_IncrRefCount(preBody);
                        if (ctorProc->bodyPtr)
                            Tcl_DecrRefCount(ctorProc->bodyPtr);
                        ctorProc->bodyPtr = preBody;

                        /* Extend compiled locals for body-internal variables
                           beyond the declared arguments. */
                        CompiledLocals(ctorProc, preProc->numCompiledLocals);

                        /* Fix bytecode linkage: point bc→procPtr at the
                           real TclOO Proc and refresh epochs. */
                        ByteCode* bc = NULL;
                        ByteCodeGetInternalRep(preBody, tbcxTyBytecode, bc);
                        if (bc)
                        {
                            bc->procPtr = ctorProc;
                            Object* oPtr = (Object*)tclObj;
                            Namespace* nsPtr = (Namespace*)oPtr->namespacePtr;
                            RefreshBC(bc, ip, nsPtr);
                            TbcxFixLocalCacheExtras(bc, ctorProc);
                            if (bc->localCachePtr)
                                FixCompiledLocalNames(ctorProc, bc->localCachePtr);
                        }
                    }
                }
            }
        }
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Fallback ctor/dtor (non-precompiled body): forward directly */
    if (kind == TBCX_METH_CTOR)
    {
        Tcl_Obj* sub = Tcl_NewStringObj("constructor", -1);
        Tcl_IncrRefCount(sub);
        Tcl_IncrRefCount(argsOpt);
        Tcl_IncrRefCount(bodyOpt);
        Tcl_Obj* av[5] = {argv0, clsFqn, sub, argsOpt, bodyOpt};
        rc = Tcl_EvalObjv(ip, 5, av, 0);
        Tcl_DecrRefCount(bodyOpt);
        Tcl_DecrRefCount(argsOpt);
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }

    /* Destructors: use short form (no args) or long form based on argsOpt. */
    { /* destructor */
        Tcl_Obj* sub = Tcl_NewStringObj("destructor", -1);
        Tcl_IncrRefCount(sub);
        /* Choose 4-arg or 5-arg form based on argsOpt emptiness */
        int useShort = 1;
        if (argsOpt)
        {
            Tcl_Size L = -1;
            if (Tcl_ListObjLength(ip, argsOpt, &L) == TCL_OK && L > 0)
                useShort = 0;
        }
        if (useShort)
        {
            Tcl_IncrRefCount(bodyOpt);
            Tcl_Obj* dv[4] = {argv0, clsFqn, sub, bodyOpt};
            rc = Tcl_EvalObjv(ip, 4, dv, 0);
            Tcl_DecrRefCount(bodyOpt);
        }
        else
        {
            Tcl_IncrRefCount(argsOpt);
            Tcl_IncrRefCount(bodyOpt);
            Tcl_Obj* dv[5] = {argv0, clsFqn, sub, argsOpt, bodyOpt};
            rc = Tcl_EvalObjv(ip, 5, dv, 0);
            Tcl_DecrRefCount(bodyOpt);
            Tcl_DecrRefCount(argsOpt);
        }
        Tcl_DecrRefCount(sub);
        Tcl_DecrRefCount(argv0);
        return rc;
    }
}

static int PrecompClass(Tcl_Interp* ip, OOShim* os, Tcl_Obj* clsFqn)
{
    Tcl_HashSearch s;
    Tcl_HashEntry* e;
    int rc = TCL_OK;
    Tcl_Size fqnLen = 0;
    const char* fqn = Tcl_GetStringFromObj(clsFqn, &fqnLen);

    for (e = Tcl_FirstHashEntry(&os->methodsByKey, &s); e; e = Tcl_NextHashEntry(&s))
    {
        const char* k = (const char*)Tcl_GetHashKey(&os->methodsByKey, e);
        if (!k)
            continue;
        Tcl_Size kLen = (Tcl_Size)strlen(k);
        /* match "fqn\x1F" prefix */
        if (!(kLen > fqnLen + 1 && memcmp(k, fqn, (size_t)fqnLen) == 0 && k[fqnLen] == '\x1F'))
        {
            continue;
        }
        /* parse kind */
        const char* p = k + fqnLen + 1;    /* after first \x1F */
        const char* q = strchr(p, '\x1F'); /* second sep */
        if (!q)
            continue;
        unsigned kind = 0;
        for (const char* snum = p; snum < q; snum++)
        {
            if (*snum < '0' || *snum > '9')
            {
                kind = 0;
                break;
            }
            kind = (unsigned)(kind * 10 + (unsigned)(*snum - '0'));
        }
        const char* nameStr = q + 1;                   /* may be empty */
        Tcl_Obj* pair = (Tcl_Obj*)Tcl_GetHashValue(e); /* {args, procbody} */
        if (!pair)
            continue;
        Tcl_Obj *savedArgs = NULL, *procBody = NULL;
        {
            Tcl_Size pairLen = 0;
            Tcl_Obj** pairElems = NULL;
            if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) != TCL_OK || pairLen < 2)
                continue;
            savedArgs = pairElems[0];
            procBody = pairElems[1];
        }

        Tcl_Obj* nameO = NULL;
        if (nameStr && *nameStr)
        {
            nameO = Tcl_NewStringObj(nameStr, -1);
            Tcl_IncrRefCount(nameO);
        }
        rc = DefOO(
            ip, os, clsFqn, (uint8_t)kind, nameO, (kind == TBCX_METH_DTOR ? savedArgs /* may be "" */ : savedArgs), procBody);
        if (nameO)
            Tcl_DecrRefCount(nameO);
        if (rc != TCL_OK)
        {
            /* leave the interp error as-is */
            return rc;
        }
    }
    return rc;
}

/* OOShim_IdentifyMethod — parse the oo::define subcommand to determine
 * method kind, body index, args reference, and hash key for precompiled
 * body lookup.  Sets *hasKeyOut=1 and initializes keyDs on success. */
static void OOShim_IdentifyMethod(const char* subc,
                                  Tcl_Size objc,
                                  Tcl_Obj* const objv[],
                                  Tcl_Obj* clsFqn,
                                  Tcl_DString* keyDs,
                                  uint8_t* kindOut,
                                  int* bodyIdxOut,
                                  Tcl_Obj** runtimeArgsOut,
                                  Tcl_Obj** nameOOut,
                                  Tcl_Obj** tmpEmptyArgsOut,
                                  int* hasKeyOut)
{
    *kindOut = 0xFF;
    *bodyIdxOut = -1;
    *runtimeArgsOut = NULL;
    *nameOOut = NULL;
    *tmpEmptyArgsOut = NULL;
    *hasKeyOut = 0;

    if (strcmp(subc, "method") == 0 || strcmp(subc, "classmethod") == 0)
    {
        if (objc >= 6)
        {
            *kindOut = (strcmp(subc, "classmethod") == 0) ? TBCX_METH_CLASS : TBCX_METH_INST;
            Tcl_Size nameIdx = objc - 3, argsIdx = objc - 2;
            *bodyIdxOut = (int)(objc - 1);
            *runtimeArgsOut = objv[argsIdx];
            MethodKeyBuf(keyDs, clsFqn, *kindOut, objv[nameIdx]);
            *hasKeyOut = 1;
            *nameOOut = objv[nameIdx];
        }
    }
    else if (strcmp(subc, "constructor") == 0)
    {
        if (objc >= 5)
        {
            *kindOut = TBCX_METH_CTOR;
            Tcl_Size argsIdx = objc - 2;
            *bodyIdxOut = (int)(objc - 1);
            *runtimeArgsOut = objv[argsIdx];
            MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL);
            *hasKeyOut = 1;
        }
    }
    else if (strcmp(subc, "destructor") == 0)
    {
        *kindOut = TBCX_METH_DTOR;
        if (objc >= 5)
        {
            Tcl_Size argsIdx = objc - 2;
            *bodyIdxOut = (int)(objc - 1);
            *runtimeArgsOut = objv[argsIdx];
            MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL);
            *hasKeyOut = 1;
        }
        else if (objc == 4)
        {
            *bodyIdxOut = 3;
            *runtimeArgsOut = Tcl_NewStringObj("", 0);
            Tcl_IncrRefCount(*runtimeArgsOut);
            *tmpEmptyArgsOut = *runtimeArgsOut;
            MethodKeyBuf(keyDs, clsFqn, *kindOut, NULL);
            *hasKeyOut = 1;
        }
    }
}

/* OOShim_LookupPair — look up the precompiled {args, procbody} pair for
 * the given method key.  Returns the pair elements via out-params. */
static void OOShim_LookupPair(Tcl_Interp* ip,
                              OOShim* os,
                              Tcl_DString* keyDs,
                              int hasKey,
                              int bodyIdx,
                              Tcl_Obj* runtimeArgs,
                              Tcl_Obj** savedArgsOut,
                              Tcl_Obj** preBodyOut)
{
    *savedArgsOut = NULL;
    *preBodyOut = NULL;
    if (hasKey && bodyIdx >= 0 && runtimeArgs != NULL)
    {
        Tcl_HashEntry* he = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(keyDs));
        if (he)
        {
            Tcl_Obj* pair = (Tcl_Obj*)Tcl_GetHashValue(he);
            if (pair)
            {
                Tcl_Size pairLen = 0;
                Tcl_Obj** pairElems = NULL;
                if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) == TCL_OK && pairLen >= 2)
                {
                    *savedArgsOut = pairElems[0];
                    *preBodyOut = pairElems[1];
                }
            }
        }
    }
}

static int CmdOOShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[])
{
    OOShim* os = (OOShim*)cd;
    int rc = TCL_OK;
    Tcl_Obj *cls = NULL, *sub = NULL;
    Tcl_Obj* nameO = NULL; /* method/classmethod name when applicable */

    /* Prepare argv for calling the renamed original command. */
    Tcl_Obj* argv0 = Tcl_NewStringObj(os->origName, -1);

    Tcl_IncrRefCount(argv0);
    /* stack-allocate for typical command widths to avoid per-call heap alloc */
    Tcl_Obj* argvStack[8];
    Tcl_Obj** argv2 = (objc <= 8) ? argvStack : (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)objc);
    argv2[0] = argv0;
    for (Tcl_Size i = 1; i < objc; i++)
    {
        argv2[i] = objv[i];
    }

    /* Fast path: if we don't even have "class subcmd ..." just forward. */
    if (objc < 3)
    {
        rc = Tcl_EvalObjv(ip, objc, argv2, 0);
        Tcl_DecrRefCount(argv0);
        if (argv2 != argvStack)
            Tcl_Free(argv2);
        return rc;
    }

    cls = objv[1];
    sub = objv[2];

    /* Compute if this is the builder form: "oo::define <cls> <script>" */
    int isBuilderForm = 0;
    if (objc == 3)
    {
        isBuilderForm = 1;
    }

    /* Compute class FQN under current namespace if relative. */
    Tcl_Obj* clsFqn = NULL;
    const char* nm = Tcl_GetString(cls);
    if (nm[0] == ':' && nm[1] == ':')
    {
        clsFqn = cls; /* borrow ref */
    }
    else
    {
        Tcl_Namespace* cur = Tcl_GetCurrentNamespace(ip);
        const char* curName = cur ? cur->fullName : "::";
        clsFqn = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0'))
        {
            Tcl_AppendToObj(clsFqn, "::", 2);
        }
        Tcl_AppendObjToObj(clsFqn, cls);
        Tcl_IncrRefCount(clsFqn);
    }

    /* Try to locate a precompiled body matching this definition form. */
    const char* subc = Tcl_GetString(sub);
    Tcl_DString keyDs;            /* hash key "class\x1Fkind\x1Fname" */
    int hasKey = 0;               /* set to 1 once keyDs is initialized */
    Tcl_Obj* savedArgs = NULL;    /* canonical args saved with the precompiled body */
    Tcl_Obj* preBody = NULL;      /* precompiled procbody to substitute */
    Tcl_Obj* runtimeArgs = NULL;  /* args provided by the user at runtime */
    Tcl_Obj* tmpEmptyArgs = NULL; /* temp "" for destructor short form */
    int bodyIdx = -1;             /* index of body word in argv */
    uint8_t kind = 0xFF;          /* TBCX_METH_* */

    /* Identify the OO subcommand and look up a precompiled pair. */
    OOShim_IdentifyMethod(subc, objc, objv, clsFqn, &keyDs, &kind, &bodyIdx, &runtimeArgs, &nameO, &tmpEmptyArgs, &hasKey);
    OOShim_LookupPair(ip, os, &keyDs, hasKey, bodyIdx, runtimeArgs, &savedArgs, &preBody);

    /* If we have a precompiled body and the signature matches byte-wise,
       substitute the body argument and call the original. */
    if (preBody && savedArgs && !isBuilderForm)
    {
        if (kind == TBCX_METH_CTOR || kind == TBCX_METH_DTOR)
        {
            rc = DefOO(ip, os, clsFqn, kind, NULL, (kind == TBCX_METH_CTOR ? savedArgs : NULL), preBody);
            goto cleanup;
        }
        Tcl_Size _dummyLen = 0;
        (void)Tcl_ListObjLength(ip, runtimeArgs, &_dummyLen);
        (void)Tcl_ListObjLength(ip, savedArgs, &_dummyLen);
        Tcl_Size aLen = 0, bLen = 0;
        const char* a = Tcl_GetStringFromObj(runtimeArgs, &aLen);
        const char* b = Tcl_GetStringFromObj(savedArgs, &bLen);

        if (aLen == bLen && memcmp(a, b, (size_t)aLen) == 0)
        {
            rc = DefOO(ip,
                       os,
                       clsFqn,
                       kind,
                       (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS) ? nameO : NULL,
                       (kind == TBCX_METH_DTOR ? savedArgs : savedArgs),
                       preBody);
            goto cleanup;
        }
        /* Fast path for stubbed empty bodies: install precompiled body directly. */
        if (preBody && bodyIdx >= 0)
        {
            Tcl_Size bodyLen = 0;
            (void)Tcl_GetStringFromObj(objv[bodyIdx], &bodyLen);
            if (bodyLen == 0)
            {
                if (kind == TBCX_METH_CTOR)
                {
                    Tcl_Obj* argsForCtor = savedArgs ? savedArgs : Tcl_NewObj();
                    if (!savedArgs)
                        Tcl_IncrRefCount(argsForCtor);
                    rc = DefOO(ip, os, clsFqn, TBCX_METH_CTOR, NULL, argsForCtor, preBody);
                    if (!savedArgs)
                        Tcl_DecrRefCount(argsForCtor);
                }
                else if (kind == TBCX_METH_DTOR)
                {
                    rc = DefOO(ip, os, clsFqn, TBCX_METH_DTOR, NULL, NULL, preBody);
                }
                else
                {
                    rc = DefOO(ip, os, clsFqn, kind, nameO, savedArgs, preBody);
                }
                /* cleanup and return */
                goto cleanup;
            }
        }
    }

    if (!isBuilderForm)
    {
        /* Fallback: forward to the original without substitution.
         * Special tolerance for destructor with accidental empty-args list:
         * If objc>=5 and args=={}, collapse to canonical 4-arg form.
         */
        if (kind == TBCX_METH_DTOR && objc >= 5 && runtimeArgs)
        {
            Tcl_Size nEl = -1;
            if (Tcl_ListObjLength(ip, runtimeArgs, &nEl) == TCL_OK && nEl == 0)
            {
                Tcl_Obj* body = objv[objc - 1];
                rc = DefOO(ip, os, clsFqn, TBCX_METH_DTOR, NULL, NULL, body);
            }
            else
            {
                /* Forward to original; normalize the class arg to FQN */
                argv2[1] = clsFqn;
                Tcl_IncrRefCount(clsFqn);
                rc = Tcl_EvalObjv(ip, objc, argv2, 0);
                Tcl_DecrRefCount(clsFqn);
            }
        }
        else
        {
            /* For builder form and other subcommands, ensure class arg is FQN */
            if (objc >= 2)
            {
                argv2[1] = clsFqn;
                Tcl_IncrRefCount(clsFqn);
            }
            rc = Tcl_EvalObjv(ip, objc, argv2, 0);
            if (objc >= 2)
                Tcl_DecrRefCount(clsFqn);
        }
    }
    else
    {
        /* Builder form: run the builder block first, then apply precompiled bodies
         * for this class (namespace-relative class names resolved to FQN above). */
        /* Ensure class arg is FQN and stay in current namespace (not global) */
        if (objc >= 2)
        {
            argv2[1] = clsFqn;
            Tcl_IncrRefCount(clsFqn);
        }
        rc = Tcl_EvalObjv(ip, objc, argv2, 0);
        if (objc >= 2)
            Tcl_DecrRefCount(clsFqn);
        if (rc == TCL_OK)
        {
            int patchRc = PrecompClass(ip, os, clsFqn);
            if (patchRc != TCL_OK)
            {
                rc = patchRc; /* leave error in interp */
            }
        }
    }
    /*
     * HARDENING: Even if the inline substitution above didn’t trigger,
     * ensure the precompiled body is installed. If we have a saved
     * {args,procbody} pair for this exact define form, re-apply it now
     * using the renamed original (os->origName). This
     * guarantees the constructor/method ends up with the compiled body
     * instead of the empty stub emitted by the saver.
     */
    if (rc == TCL_OK && hasKey)
    {
        Tcl_HashEntry* he2 = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs));
        if (he2)
        {
            Tcl_Obj* pair2 = (Tcl_Obj*)Tcl_GetHashValue(he2);
            Tcl_Obj *savedArgs2 = NULL, *preBody2 = NULL;
            if (pair2)
            {
                Tcl_Size p2Len = 0;
                Tcl_Obj** p2Elems = NULL;
                if (Tcl_ListObjGetElements(ip, pair2, &p2Len, &p2Elems) == TCL_OK && p2Len >= 2)
                {
                    savedArgs2 = p2Elems[0];
                    preBody2 = p2Elems[1];
                }
            }
            if (savedArgs2 && preBody2)
            {
                /* Normalize class arg to FQN for deterministic resolution */
                Tcl_Obj* clsForCall = clsFqn;
                if (clsForCall != cls)
                    Tcl_IncrRefCount(clsForCall);
                int rc2 = TCL_OK;
                if (kind == TBCX_METH_INST || kind == TBCX_METH_CLASS)
                {
                    if (nameO)
                    {
                        rc2 = DefOO(ip, os, clsForCall, kind, nameO, savedArgs2, preBody2);
                    }
                }
                else if (kind == TBCX_METH_CTOR)
                {
                    rc2 = DefOO(ip, os, clsForCall, TBCX_METH_CTOR, NULL, savedArgs2, preBody2);
                }
                else if (kind == TBCX_METH_DTOR)
                {
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
    Tcl_DecrRefCount(argv0);
    if (argv2 != argvStack)
        Tcl_Free(argv2);
    return rc;
}

/* oo::objdefine shim — mirrors CmdOOShim logic but forwards to
   the renamed oo::objdefine (origNameObjDef).  Uses the same
   methodsByKey registry since saver stores objdefine methods
   there keyed by the object/class FQN. */
static int CmdOOShimObjDef(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[])
{
    OOShim* os = (OOShim*)cd;
    int rc = TCL_OK;

    Tcl_Obj* argv0 = Tcl_NewStringObj(os->origNameObjDef, -1);
    Tcl_IncrRefCount(argv0);

    Tcl_Obj* argvStack[8];
    Tcl_Obj** argv2 = (objc <= 8) ? argvStack : (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)objc);
    argv2[0] = argv0;
    for (Tcl_Size i = 1; i < objc; i++)
        argv2[i] = objv[i];

    if (objc < 3)
    {
        rc = Tcl_EvalObjv(ip, objc, argv2, 0);
        Tcl_DecrRefCount(argv0);
        if (argv2 != argvStack)
            Tcl_Free(argv2);
        return rc;
    }

    Tcl_Obj* obj = objv[1];
    Tcl_Obj* sub = objv[2];
    int isBuilderForm = (objc == 3);

    /* Compute object FQN */
    Tcl_Obj* objFqn = NULL;
    const char* nm = Tcl_GetString(obj);
    if (nm[0] == ':' && nm[1] == ':')
    {
        objFqn = obj;
    }
    else
    {
        Tcl_Namespace* cur = Tcl_GetCurrentNamespace(ip);
        const char* curName = cur ? cur->fullName : "::";
        objFqn = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0'))
            Tcl_AppendToObj(objFqn, "::", 2);
        Tcl_AppendObjToObj(objFqn, obj);
        Tcl_IncrRefCount(objFqn);
    }

    /* For multi-word form: try to find and substitute precompiled body */
    if (!isBuilderForm)
    {
        const char* subc = Tcl_GetString(sub);
        Tcl_DString keyDs;
        int hasKey = 0;
        Tcl_Obj* nameO = NULL;
        int bodyIdx = -1;
        uint8_t kind = 0xFF;

        if ((strcmp(subc, "method") == 0 || strcmp(subc, "classmethod") == 0) && objc >= 6)
        {
            kind = (strcmp(subc, "classmethod") == 0) ? TBCX_METH_CLASS : TBCX_METH_INST;
            bodyIdx = (int)(objc - 1);
            nameO = objv[objc - 3];
            MethodKeyBuf(&keyDs, objFqn, kind, nameO);
            hasKey = 1;
        }
        else if (strcmp(subc, "constructor") == 0 && objc >= 5)
        {
            kind = TBCX_METH_CTOR;
            bodyIdx = (int)(objc - 1);
            MethodKeyBuf(&keyDs, objFqn, kind, NULL);
            hasKey = 1;
        }
        else if (strcmp(subc, "destructor") == 0 && objc >= 4)
        {
            kind = TBCX_METH_DTOR;
            bodyIdx = (int)(objc - 1);
            MethodKeyBuf(&keyDs, objFqn, kind, NULL);
            hasKey = 1;
        }

        if (hasKey && bodyIdx >= 0)
        {
            Tcl_HashEntry* he = Tcl_FindHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs));
            if (he)
            {
                Tcl_Obj* pair = (Tcl_Obj*)Tcl_GetHashValue(he);
                Tcl_Obj *savedArgs = NULL, *preBody = NULL;
                if (pair)
                {
                    Tcl_Size pLen = 0;
                    Tcl_Obj** pElems = NULL;
                    if (Tcl_ListObjGetElements(ip, pair, &pLen, &pElems) == TCL_OK && pLen >= 2)
                    {
                        savedArgs = pElems[0];
                        preBody = pElems[1];
                    }
                }
                (void)savedArgs; /* only preBody used in objdefine path */
                if (preBody)
                {
                    /* Substitute the body argument and forward */
                    argv2[bodyIdx] = preBody;
                    rc = Tcl_EvalObjv(ip, objc, argv2, 0);
                    Tcl_DStringFree(&keyDs);
                    if (objFqn != obj)
                        Tcl_DecrRefCount(objFqn);
                    Tcl_DecrRefCount(argv0);
                    if (argv2 != argvStack)
                        Tcl_Free(argv2);
                    return rc;
                }
            }
            Tcl_DStringFree(&keyDs);
        }
    }

    /* Builder form or unmatched: forward to real oo::objdefine */
    rc = Tcl_EvalObjv(ip, objc, argv2, 0);

    /* After builder form execution, apply precompiled bodies.
       Use oo::define (the renamed original) for body installation via PrecompClass,
       since per-object methods can also be set through oo::define on the object. */
    if (isBuilderForm && rc == TCL_OK)
    {
        (void)PrecompClass(ip, os, objFqn);
    }

    if (objFqn != obj)
        Tcl_DecrRefCount(objFqn);
    Tcl_DecrRefCount(argv0);
    if (argv2 != argvStack)
        Tcl_Free(argv2);
    return rc;
}

/* AddOOShim — intercept [oo::define] and [oo::objdefine] to substitute
 * precompiled method/constructor/destructor bodies.
 *
 * Thread safety / lock ordering:
 *   Calls TclRenameCommand and Tcl_CreateObjCommand2, both of which may
 *   acquire Tcl-internal mutexes.  No TBCX mutex is held during this call.
 *   Must be called from the interp-owning thread only. */
static int AddOOShim(Tcl_Interp* ip, OOShim* os)
{
    memset(os, 0, sizeof(*os));
    Tcl_InitHashTable(&os->methodsByKey, TCL_STRING_KEYS);

    /* Generate a unique rename target so nested tbcx::load calls don't
       collide.  Each nesting level gets its own orig command name. */
    Tcl_MutexLock(&tbcxHiddenIdMutex);
    if (tbcxHiddenId == UINT_MAX)
    {
        Tcl_MutexUnlock(&tbcxHiddenIdMutex);
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: OO shim ID counter exhausted"));
        return TCL_ERROR;
    }
    unsigned int myId = tbcxHiddenId++;
    Tcl_MutexUnlock(&tbcxHiddenIdMutex);

    /* Build unique rename targets using Tcl_ObjPrintf instead of raw snprintf */
    {
        Tcl_Obj* tmp = Tcl_ObjPrintf("::tbcx::__oo_define_orig_%u__", myId);
        Tcl_IncrRefCount(tmp);
        Tcl_Size len = 0;
        const char* s = Tcl_GetStringFromObj(tmp, &len);
        if (len >= (Tcl_Size)sizeof(os->origName))
            len = (Tcl_Size)(sizeof(os->origName) - 1);
        memcpy(os->origName, s, (size_t)len);
        os->origName[len] = '\0';
        Tcl_DecrRefCount(tmp);

        tmp = Tcl_ObjPrintf("::tbcx::__oo_objdef_orig_%u__", myId);
        Tcl_IncrRefCount(tmp);
        s = Tcl_GetStringFromObj(tmp, &len);
        if (len >= (Tcl_Size)sizeof(os->origNameObjDef))
            len = (Tcl_Size)(sizeof(os->origNameObjDef) - 1);
        memcpy(os->origNameObjDef, s, (size_t)len);
        os->origNameObjDef[len] = '\0';
        Tcl_DecrRefCount(tmp);
    }

    if (TclRenameCommand(ip, "oo::define", os->origName) != TCL_OK)
    {
        Tcl_DeleteHashTable(&os->methodsByKey);
        return TCL_ERROR;
    }
    if (!Tcl_CreateObjCommand2(ip, "oo::define", CmdOOShim, os, NULL))
    {
        /* Shim creation failed after rename — restore original command name */
        (void)TclRenameCommand(ip, os->origName, "oo::define");
        Tcl_DeleteHashTable(&os->methodsByKey);
        return TCL_ERROR;
    }

    /* Also shim oo::objdefine if it exists */
    os->hasObjDefine = 0;
    if (TclRenameCommand(ip, "oo::objdefine", os->origNameObjDef) == TCL_OK)
    {
        if (!Tcl_CreateObjCommand2(ip, "oo::objdefine", CmdOOShimObjDef, os, NULL))
        {
            /* Restore objdefine and continue — define shim is still active */
            (void)TclRenameCommand(ip, os->origNameObjDef, "oo::objdefine");
        }
        else
        {
            os->hasObjDefine = 1;
        }
    }
    return TCL_OK;
}

static void DelOOShim(Tcl_Interp* ip, OOShim* os)
{
    Tcl_DeleteCommand(ip, "oo::define");
    (void)TclRenameCommand(ip, os->origName, "oo::define");
    if (os->hasObjDefine)
    {
        Tcl_DeleteCommand(ip, "oo::objdefine");
        (void)TclRenameCommand(ip, os->origNameObjDef, "oo::objdefine");
    }
    Tcl_HashSearch s;
    Tcl_HashEntry* e;
    for (e = Tcl_FirstHashEntry(&os->methodsByKey, &s); e; e = Tcl_NextHashEntry(&s))
    {
        Tcl_Obj* val = (Tcl_Obj*)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&os->methodsByKey);
}

Tcl_Namespace* Tbcx_EnsureNamespace(Tcl_Interp* ip, const char* fqn)
{
    Tcl_Namespace* nsPtr = NULL;
    /* use flag 0 to avoid leaving error messages that linger
     * when the namespace doesn't exist but will be created */
    nsPtr = Tcl_FindNamespace(ip, fqn, NULL, 0);
    if (!nsPtr)
    {
        nsPtr = Tcl_CreateNamespace(ip, fqn, NULL, NULL);
    }
    return nsPtr;
}

/* Mirrors TclInitByteCode()’s packed layout + TclInitByteCodeObj()’s attach,
 * but never calls internal TclPreserveByteCode(). Instead we set refCount = 1.
 */

static ByteCode* TbcxByteCode(Tcl_Obj* objPtr, const Tcl_ObjType* typePtr, const TBCX_CompileEnvMin* env, int setPrecompiled)
{
    Interp* iPtr = (Interp*)env->interp;
    Namespace* nsPtr = env->nsPtr ? env->nsPtr : (iPtr->varFramePtr ? iPtr->varFramePtr->nsPtr : iPtr->globalNsPtr);

    /* Sizes for packed allocation (match TclInitByteCode) */
    const size_t codeBytes = (size_t)(env->codeNext - env->codeStart);
    const size_t objArrayBytes = (size_t)env->numLitObjects * sizeof(Tcl_Obj*);
    const size_t exceptArrayBytes = (size_t)env->numExceptRanges * sizeof(ExceptionRange);
    const size_t auxDataArrayBytes = (size_t)env->numAuxDataItems * sizeof(AuxData);
    const size_t cmdLocBytes = 0; /* no cmd-location map stored */

    /* Overflow guard: ensure the combined structure size doesn't wrap around.
       Use SIZE_MAX/2 as a generous upper bound to detect pathological inputs
       from malformed .tbcx files. */
    {
        size_t safeCap = SIZE_MAX / 2;
        if (codeBytes > safeCap || objArrayBytes > safeCap || exceptArrayBytes > safeCap || auxDataArrayBytes > safeCap)
        {
            Tcl_SetObjResult(env->interp, Tcl_NewStringObj("tbcx: ByteCode section sizes overflow", -1));
            return NULL;
        }
    }

    /* Validate exception-range offsets against the code block bounds. */
    if (env->numExceptRanges > 0 && env->exceptArrayPtr)
    {
        for (int vi = 0; vi < env->numExceptRanges; vi++)
        {
            ExceptionRange* er = &env->exceptArrayPtr[vi];
            uint32_t erEnd = (uint32_t)er->codeOffset + (uint32_t)er->numCodeBytes;
            if (erEnd > codeBytes)
            {
                Tcl_SetObjResult(env->interp,
                                 Tcl_ObjPrintf("tbcx: exception range %d offset+len (%u+%u) exceeds code size (%lu)",
                                               vi,
                                               (unsigned)er->codeOffset,
                                               (unsigned)er->numCodeBytes,
                                               (unsigned long)codeBytes));
                return NULL;
            }
            if (er->catchOffset >= 0 && (uint32_t)er->catchOffset >= codeBytes)
            {
                Tcl_SetObjResult(env->interp,
                                 Tcl_ObjPrintf("tbcx: exception range %d catchOffset (%u) exceeds code size (%lu)",
                                               vi,
                                               (unsigned)er->catchOffset,
                                               (unsigned long)codeBytes));
                return NULL;
            }
        }
    }

    size_t structureSize = 0;
    {
        /* Checked addition: detect wrap-around from the sum of aligned
           segments, which the per-segment SIZE_MAX/2 guard above does
           not fully prevent on 32-bit targets. */
#define TBCX_CHECKED_ADD(sz, val)                                                                                                \
    do                                                                                                                           \
    {                                                                                                                            \
        size_t _aligned = TCL_ALIGN(val);                                                                                        \
        if ((sz) + _aligned < (sz))                                                                                              \
        {                                                                                                                        \
            Tcl_SetObjResult(env->interp, Tcl_ObjPrintf("tbcx: ByteCode total size overflow"));                                  \
            return NULL;                                                                                                         \
        }                                                                                                                        \
        (sz) += _aligned;                                                                                                        \
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

    unsigned char* base = (unsigned char*)Tcl_Alloc(structureSize);
    ByteCode* codePtr = (ByteCode*)base;

    /* ----- Header & environment capture (same fields as core) ----- */
    memset(codePtr, 0, sizeof(ByteCode));
    codePtr->interpHandle = TclHandlePreserve(iPtr->handle);
    codePtr->compileEpoch = iPtr->compileEpoch;
    codePtr->nsPtr = nsPtr;
    codePtr->nsEpoch = nsPtr->resolverEpoch;

    /* *** Inline TclPreserveByteCode(codePtr) *** */
    codePtr->refCount = 1; /* brand-new ByteCode held by this one Tcl_Obj */
    codePtr->flags = ((nsPtr->compiledVarResProc || iPtr->resolverPtr) ? TCL_BYTECODE_RESOLVE_VARS : 0);
    codePtr->source = NULL;          /* no retained source for precompiled */
    codePtr->procPtr = env->procPtr; /* may be NULL for top-level blocks   */

    codePtr->maxExceptDepth = TCL_INDEX_NONE;
    codePtr->maxStackDepth = (Tcl_Size)env->maxStackDepth;
    codePtr->numAuxDataItems = (Tcl_Size)env->numAuxDataItems;
    codePtr->numCmdLocBytes = 0;
    codePtr->numCodeBytes = (Tcl_Size)codeBytes;
    codePtr->numCommands = 0;
    codePtr->numExceptRanges = (Tcl_Size)env->numExceptRanges;
    codePtr->numLitObjects = (Tcl_Size)env->numLitObjects;
    codePtr->numSrcBytes = 0;
    codePtr->structureSize = (Tcl_Size)structureSize;

    /* ----- Pack variable-length tails with Tcl's exact alignment sequence ---- */
    unsigned char* p = base + TCL_ALIGN(sizeof(ByteCode));

    /* 1) Code bytes */
    codePtr->codeStart = p;
    if (codeBytes)
        memcpy(p, env->codeStart, codeBytes);
    p += TCL_ALIGN(codeBytes);

    /* 2) Literal object array */
    p = (unsigned char*)TCL_ALIGN((uintptr_t)p);
    codePtr->objArrayPtr = (Tcl_Obj**)p;
    if (env->numLitObjects)
    {
        for (int i = 0; i < env->numLitObjects; i++)
        {
            Tcl_Obj* lit = env->objArrayPtr[i];
            codePtr->objArrayPtr[i] = lit;
            if (lit)
                Tcl_IncrRefCount(lit);
        }
    }
    p += TCL_ALIGN(objArrayBytes);

    /* 3) Exception ranges */
    p = (unsigned char*)TCL_ALIGN((uintptr_t)p);
    codePtr->exceptArrayPtr = (ExceptionRange*)p;
    if (env->numExceptRanges)
    {
        memcpy(p, env->exceptArrayPtr, exceptArrayBytes);
    }
    p += TCL_ALIGN(exceptArrayBytes);

    /* 4) AuxData array */

    if (env->numExceptRanges > 0 && env->exceptArrayPtr)
    {
        Tcl_Size maxDepth = 0;
        for (int i = 0; i < env->numExceptRanges; i++)
        {
            ExceptionRange* er = &env->exceptArrayPtr[i];
            Tcl_Size d = (Tcl_Size)er->nestingLevel + 1; /* levels start at 0 */
            if (d > maxDepth)
            {
                maxDepth = d;
            }
        }
        codePtr->maxExceptDepth = maxDepth;
    }
    else
    {
        codePtr->maxExceptDepth = 0;
    }
    p = (unsigned char*)TCL_ALIGN((uintptr_t)p);
    codePtr->auxDataArrayPtr = (AuxData*)p;
    if (env->numAuxDataItems)
    {
        memcpy(p, env->auxDataArrayPtr, auxDataArrayBytes);
    }
    p += TCL_ALIGN(auxDataArrayBytes);

    /* 5) (Empty) command-location map segment + required alignment step */
    p = (unsigned char*)TCL_ALIGN((uintptr_t)(p + cmdLocBytes));

    /* 6) Cursors: in a precompiled image we just coalesce them to the tail */
    codePtr->codeDeltaStart = p;
    codePtr->codeLengthStart = p;
    codePtr->srcDeltaStart = p;
    codePtr->srcLengthStart = p;

    /* Locals cache is created lazily by the engine if needed */
    codePtr->localCachePtr = NULL;

    /* ----- Attach as the internal rep of objPtr ----- */
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = codePtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(objPtr, typePtr, &ir);
    if (setPrecompiled)
    {
        ByteCode* bcPtr = NULL;
        ByteCodeGetInternalRep(objPtr, tbcxTyBytecode, bcPtr);
        if (bcPtr)
            bcPtr->flags |= TCL_BYTECODE_PRECOMPILED;
    }
    return codePtr;
}

static Tcl_Obj* ByteCodeObj(Tcl_Interp* ip,
                            Namespace* nsPtr,
                            const unsigned char* code,
                            uint32_t codeLen,
                            Tcl_Obj** lits,
                            uint32_t numLits,
                            AuxData* auxArr,
                            uint32_t numAux,
                            ExceptionRange* exArr,
                            uint32_t numEx,
                            int maxStackDepth,
                            int setPrecompiled)
{
    Tcl_Obj* bcObj = NULL;
    TBCX_CompileEnvMin env;
    memset(&env, 0, sizeof(env));
    env.interp = ip;
    env.nsPtr = (Namespace*)nsPtr;
    env.maxStackDepth = maxStackDepth;
    env.procPtr = NULL;

    env.codeStart = (unsigned char*)Tcl_Alloc(codeLen ? codeLen : 1u);
    if (codeLen)
        memcpy(env.codeStart, code, codeLen);
    env.codeNext = env.codeStart + codeLen;
    env.objArrayPtr = NULL;
    env.numLitObjects = (Tcl_Size)numLits;
    if (numLits)
    {
        env.objArrayPtr = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * numLits);
        for (uint32_t i = 0; i < numLits; i++)
        {
            env.objArrayPtr[i] = lits[i];
            if (env.objArrayPtr[i])
                Tcl_IncrRefCount(env.objArrayPtr[i]);
        }
    }

    env.auxDataArrayPtr = NULL;
    env.numAuxDataItems = (Tcl_Size)numAux;
    if (numAux)
    {
        env.auxDataArrayPtr = (AuxData*)Tcl_Alloc(sizeof(AuxData) * numAux);
        for (uint32_t i = 0; i < numAux; i++)
            env.auxDataArrayPtr[i] = auxArr[i];
    }

    env.exceptArrayPtr = NULL;
    env.numExceptRanges = (Tcl_Size)numEx;
    if (numEx)
    {
        env.exceptArrayPtr = (ExceptionRange*)Tcl_Alloc(sizeof(ExceptionRange) * numEx);
        for (uint32_t i = 0; i < numEx; i++)
            env.exceptArrayPtr[i] = exArr[i];
    }

    bcObj = Tcl_NewObj();
    if (!TbcxByteCode(bcObj, tbcxTyBytecode, &env, setPrecompiled))
    {
        /* TbcxByteCode failed (overflow, exception-range validation, etc.).
         * Clean up the bare object and all temporary state, then return NULL
         * so the caller can propagate the error. */
        Tcl_IncrRefCount(bcObj);
        Tcl_DecrRefCount(bcObj);
        bcObj = NULL;
    }

    /* Drop the temporary holds on literals and free the temporary array.       */
    /* TbcxByteCode() has already incref'd the same objects into the ByteCode. */
    if (env.objArrayPtr)
    {
        if (env.numLitObjects > 0)
        {
            for (int i = 0; i < env.numLitObjects; i++)
            {
                if (env.objArrayPtr[i])
                {
                    Tcl_DecrRefCount(env.objArrayPtr[i]);
                }
            }
        }
        Tcl_Free(env.objArrayPtr);
    }

    if (env.auxDataArrayPtr)
        Tcl_Free(env.auxDataArrayPtr);
    if (env.exceptArrayPtr)
        Tcl_Free(env.exceptArrayPtr);
    if (env.codeStart)
        Tcl_Free(env.codeStart);

    return bcObj;
}

static void CompiledLocals(Proc* procPtr, int neededCount)
{
    if (!procPtr)
        return;
    if (neededCount <= procPtr->numCompiledLocals)
        return;

    CompiledLocal* first = procPtr->firstLocalPtr;
    CompiledLocal* last = procPtr->lastLocalPtr;

    for (int i = procPtr->numCompiledLocals; i < neededCount; i++)
    {
        /* Minimal allocation: struct header + 1 byte for the NUL terminator.
           Unnamed padding locals don't need the full sizeof(CompiledLocal)
           which includes the trailing name[1] flexible array. */
        const size_t allocSize = offsetof(CompiledLocal, name) + 1u;
        CompiledLocal* cl = (CompiledLocal*)Tcl_Alloc(allocSize);
        memset(cl, 0, allocSize);
        cl->nameLength = 0;
        cl->name[0] = '\0';
        cl->frameIndex = i;
        if (!first)
            first = last = cl;
        else
        {
            last->nextPtr = cl;
            last = cl;
        }
    }
    procPtr->firstLocalPtr = first;
    procPtr->lastLocalPtr = last;
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
static void FixCompiledLocalNames(Proc* procPtr, LocalCache* lc)
{
    if (!procPtr || !lc)
        return;
    Tcl_Obj** names = (Tcl_Obj**)&lc->varName0;
    Tcl_Size numVars = lc->numVars;

    CompiledLocal* prev = NULL;
    CompiledLocal* cl = procPtr->firstLocalPtr;
    while (cl)
    {
        if (cl->nameLength > 0)
        {
            /* already named (argument) — skip */
            prev = cl;
            cl = cl->nextPtr;
            continue;
        }
        Tcl_Size idx = cl->frameIndex;
        if (idx < 0 || idx >= numVars || !names[idx])
        {
            prev = cl;
            cl = cl->nextPtr;
            continue;
        }
        Tcl_Size nameLen = 0;
        const char* nm = Tcl_GetStringFromObj(names[idx], &nameLen);
        if (nameLen == 0)
        {
            prev = cl;
            cl = cl->nextPtr;
            continue;
        }

        /* Reallocate the CompiledLocal to hold the name.
           The flexible array member name[1] needs nameLen+1 bytes. */
        size_t newSize = offsetof(CompiledLocal, name) + (size_t)nameLen + 1u;
        CompiledLocal* repl = (CompiledLocal*)Tcl_Alloc(newSize);
        memcpy(repl, cl, offsetof(CompiledLocal, name));
        memcpy(repl->name, nm, (size_t)nameLen);
        repl->name[nameLen] = '\0';
        repl->nameLength = (unsigned)nameLen;

        /* Splice replacement into the linked list using tracked predecessor
           (O(1) instead of the previous O(n) predecessor search). */
        repl->nextPtr = cl->nextPtr;
        if (prev)
            prev->nextPtr = repl;
        else
            procPtr->firstLocalPtr = repl;
        if (cl == procPtr->lastLocalPtr)
            procPtr->lastLocalPtr = repl;
        Tcl_Free(cl);
        prev = repl;
        cl = repl->nextPtr; /* continue iteration from next node */
    }
}

static int ReadMethod(TbcxIn* r, Tcl_Interp* ip, OOShim* os)
{
    /* classFqn */
    char* clsf = NULL;
    uint32_t clsL = 0;
    if (!Tbcx_R_LPString(r, &clsf, &clsL))
        return TCL_ERROR;
    Tcl_Obj* clsFqn = Tcl_NewStringObj(clsf, (Tcl_Size)clsL);
    Tcl_IncrRefCount(clsFqn);
    Tcl_Free(clsf);
    /* kind */
    uint8_t kind = 0;
    if (!Tbcx_R_U8(r, &kind))
    {
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    /* name (empty for ctor/dtor) */
    char* mname = NULL;
    uint32_t mnL = 0;
    if (!Tbcx_R_LPString(r, &mname, &mnL))
    {
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    Tcl_Obj* nameObj = Tcl_NewStringObj(mname, (Tcl_Size)mnL);
    Tcl_IncrRefCount(nameObj);
    Tcl_Free(mname);
    /* args text */
    char* args = NULL;
    uint32_t aL = 0;
    if (!Tbcx_R_LPString(r, &args, &aL))
    {
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    Tcl_Obj* argsObj = Tcl_NewStringObj(args, (Tcl_Size)aL);
    Tcl_IncrRefCount(argsObj);
    Tcl_Free(args);

    /* compiled block (namespace default: class namespace) + receive numLocals */
    Namespace* clsNs = (Namespace*)Tbcx_EnsureNamespace(ip, Tcl_GetString(clsFqn));
    uint32_t nLoc = 0;
    Tcl_Obj* bodyBC = Tbcx_ReadBlock(r, ip, clsNs, &nLoc, 1, 0);
    if (!bodyBC)
    {
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        return TCL_ERROR;
    }
    /* Build Proc + compiled locals from argsObj */
    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr = bodyBC;
    Tcl_IncrRefCount(bodyBC);
    {
        CompiledLocal *first = NULL, *last = NULL;
        int numA = 0;
        if (Tbcx_BuildLocals(ip, argsObj, &first, &last, &numA) != TCL_OK)
        {
            Tcl_DecrRefCount(bodyBC);
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            Tcl_Free((char*)procPtr);
            return TCL_ERROR;
        }
        procPtr->numArgs = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr = first;
        procPtr->lastLocalPtr = last;
    }
    CompiledLocals(procPtr, (int)nLoc);

    /* Link ByteCode back to this Proc and refresh epochs */
    {
        ByteCode* bcPtr = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bcPtr);
        if (bcPtr)
        {
            bcPtr->procPtr = procPtr;
            RefreshBC(bcPtr, ip, clsNs);

            /* Reallocate the LocalCache with the extras area that Tcl 9.1's
               InitArgsAndLocals expects (numArgs * sizeof(Var) for argument
               flags and default values), while preserving the variable names
               that TBCX read from the .tbcx stream. */
            TbcxFixLocalCacheExtras(bcPtr, procPtr);
            if (bcPtr->localCachePtr)
                FixCompiledLocalNames(procPtr, bcPtr->localCachePtr);
        }
    }

    /* Build procbody and register */
    Tcl_Obj* procBodyObj = Tcl_NewObj();
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = procPtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
    procPtr->refCount++;

    Tcl_DString keyDs;
    MethodKeyBuf(&keyDs, clsFqn, kind, (mnL ? nameObj : NULL));
    int isNew = 0;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&os->methodsByKey, Tcl_DStringValue(&keyDs), &isNew);
    Tcl_DStringFree(&keyDs);
    if (!isNew)
    {
        Tcl_Obj* oldPair = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (oldPair)
            Tcl_DecrRefCount(oldPair);
    }
    /* Store PAIR {argsObj, procBodyObj} so we can verify signature at shim time */
    Tcl_Obj* pair = Tcl_NewListObj(0, NULL);
    if (Tcl_ListObjAppendElement(ip, pair, argsObj) != TCL_OK || Tcl_ListObjAppendElement(ip, pair, procBodyObj) != TCL_OK)
    {
        Tcl_DecrRefCount(procBodyObj);
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
        Tcl_DecrRefCount(argsObj);
        return TCL_ERROR;
    }
    Tcl_IncrRefCount(pair);
    Tcl_SetHashValue(he, pair);
    Tcl_DecrRefCount(nameObj);
    Tcl_DecrRefCount(clsFqn);
    Tcl_DecrRefCount(argsObj);
    return TCL_OK;
}

/* ---- Extracted literal readers ----
 * Each handles one tag from the .tbcx literal stream.  Returns a refcount-0
 * Tcl_Obj* on success or NULL on error. */

static Tcl_Obj* ReadLit_Bignum(TbcxIn* r)
{
    uint8_t sign = 0;
    uint32_t magLen = 0;
    if (!Tbcx_R_U8(r, &sign))
        return NULL;
    if (!Tbcx_R_U32(r, &magLen))
        return NULL;
    if (magLen > (64u * 1024u * 1024u))
    {
        R_Error(r, "tbcx: bignum too large");
        return NULL;
    }
    Tcl_Obj* o = NULL;
    if (magLen == 0 || sign == 0)
    {
        o = Tcl_NewWideIntObj(0);
    }
    else
    {
        unsigned char* le = (unsigned char*)Tcl_Alloc(magLen);
        if (!Tbcx_R_Bytes(r, le, magLen))
        {
            Tcl_Free((char*)le);
            return NULL;
        }
        mp_int z;
        mp_err mrc = TclBN_mp_init(&z);
        if (mrc != MP_OKAY)
        {
            Tcl_Free((char*)le);
            R_Error(r, "tbcx: bignum init");
            return NULL;
        }
#if defined(MP_HAS_FROM_UBIN)
        unsigned char* be = (unsigned char*)Tcl_Alloc(magLen);
        for (uint32_t i = 0; i < magLen; i++)
            be[i] = le[magLen - 1 - i];
        mrc = TclBN_mp_from_ubin(&z, be, magLen);
        Tcl_Free((char*)be);
#elif defined(MP_HAS_READ_UNSIGNED_BIN)
        unsigned char* be = (unsigned char*)Tcl_Alloc(magLen);
        for (uint32_t i = 0; i < magLen; i++)
            be[i] = le[magLen - 1 - i];
        mrc = TclBN_mp_read_unsigned_bin(&z, be, magLen);
        Tcl_Free((char*)be);
#else
        for (int i = (int)magLen - 1; i >= 0; i--)
        {
            if ((mrc = TclBN_mp_mul_2d(&z, 8, &z)) != MP_OKAY)
                break;
            if ((mrc = TclBN_mp_add_d(&z, le[i], &z)) != MP_OKAY)
                break;
        }
#endif
        if (mrc != MP_OKAY)
        {
            Tcl_Free((char*)le);
            TclBN_mp_clear(&z);
            R_Error(r, "tbcx: bignum import");
            return NULL;
        }
        if (sign == 2)
        {
            mrc = TclBN_mp_neg(&z, &z);
            if (mrc != MP_OKAY)
            {
                Tcl_Free((char*)le);
                TclBN_mp_clear(&z);
                R_Error(r, "tbcx: bignum neg");
                return NULL;
            }
        }
        o = Tcl_NewBignumObj(&z);
        Tcl_Free((char*)le);
    }
    return o;
}

static Tcl_Obj* ReadLit_LambdaBC(TbcxIn* r, Tcl_Interp* ip, int depth, int dumpOnly)
{
    char* nsStr = NULL;
    uint32_t nsLen = 0;
    if (!Tbcx_R_LPString(r, &nsStr, &nsLen))
        return NULL;
    Tcl_Obj* nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
    Tcl_Free(nsStr);
    Tcl_IncrRefCount(nsObj);
    Namespace* nsPtr;
    if (dumpOnly)
        nsPtr = (Namespace*)Tcl_FindNamespace(ip, Tcl_GetString(nsObj), NULL, 0);
    else
        nsPtr = (Namespace*)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
    if (!nsPtr)
        nsPtr = (Namespace*)Tcl_GetGlobalNamespace(ip);

    /* Read argument specifications */
    uint32_t numArgs = 0;
    if (!Tbcx_R_U32(r, &numArgs))
    {
        Tcl_DecrRefCount(nsObj);
        return NULL;
    }
    Tcl_Obj* argList = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(argList);
    for (uint32_t i = 0; i < numArgs; i++)
    {
        char* argName = NULL;
        uint32_t argNameLen = 0;
        if (!Tbcx_R_LPString(r, &argName, &argNameLen))
        {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argList);
            return NULL;
        }
        Tcl_Obj* argNameObj = Tcl_NewStringObj(argName, (Tcl_Size)argNameLen);
        Tcl_Free(argName);
        uint8_t hasDef = 0;
        if (!Tbcx_R_U8(r, &hasDef))
        {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argNameObj);
            Tcl_DecrRefCount(argList);
            return NULL;
        }
        if (hasDef)
        {
            Tcl_Obj* defVal = ReadLiteral(r, ip, depth + 1, dumpOnly);
            if (!defVal)
            {
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argNameObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
            Tcl_Obj* argPair = Tcl_NewListObj(0, NULL);
            if (Tcl_ListObjAppendElement(ip, argPair, argNameObj) != TCL_OK ||
                Tcl_ListObjAppendElement(ip, argPair, defVal) != TCL_OK ||
                Tcl_ListObjAppendElement(ip, argList, argPair) != TCL_OK)
            {
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
        }
        else
        {
            if (Tcl_ListObjAppendElement(ip, argList, argNameObj) != TCL_OK)
            {
                Tcl_DecrRefCount(nsObj);
                Tcl_DecrRefCount(argList);
                return NULL;
            }
        }
    }

    /* Build Proc with precompiled bytecode */
    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ip;
    procPtr->refCount = 1;
    {
        CompiledLocal *first = NULL, *last = NULL;
        int numA = 0;
        if (Tbcx_BuildLocals(ip, argList, &first, &last, &numA) != TCL_OK)
        {
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argList);
            Tcl_Free((char*)procPtr);
            return NULL;
        }
        procPtr->numArgs = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr = first;
        procPtr->lastLocalPtr = last;
    }

    uint32_t nLocalsBody = 0;
    Tcl_Obj* bodyBC = Tbcx_ReadBlock(r, ip, nsPtr, &nLocalsBody, 1, dumpOnly);
    if (!bodyBC)
    {
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(argList);
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char*)procPtr);
        return NULL;
    }
    procPtr->bodyPtr = bodyBC;
    Tcl_IncrRefCount(bodyBC);
    {
        ByteCode* bc = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bc);
        if (bc)
        {
            bc->procPtr = procPtr;
            RefreshBC(bc, ip, nsPtr);
            TbcxFixLocalCacheExtras(bc, procPtr);
        }
    }
    CompiledLocals(procPtr, (int)nLocalsBody);

    /* Read original body source text */
    char* bodySrc = NULL;
    uint32_t bodySrcLen = 0;
    if (!Tbcx_R_LPString(r, &bodySrc, &bodySrcLen))
    {
        ByteCode* bc2 = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bc2);
        if (bc2)
            bc2->procPtr = NULL;
        Tcl_DecrRefCount(bodyBC);
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char*)procPtr);
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(argList);
        return NULL;
    }

    /* Build lambda Tcl_Obj with original body source in string rep */
    Tcl_Obj* lambda;
    {
        const char* nsName = Tcl_GetString(nsObj);
        int isGlobal = (nsName[0] == ':' && nsName[1] == ':' && nsName[2] == '\0');
        Tcl_Obj* bodyText = Tcl_NewStringObj(bodySrc, (Tcl_Size)bodySrcLen);
        Tcl_Free(bodySrc);
        if (isGlobal)
        {
            Tcl_Obj* elems[2] = {argList, bodyText};
            lambda = Tcl_NewListObj(2, elems);
        }
        else
        {
            Tcl_Obj* elems[3] = {argList, bodyText, nsObj};
            lambda = Tcl_NewListObj(3, elems);
        }
        (void)Tcl_GetString(lambda);
    }

    /* Register in ApplyShim for shimmer recovery (skip in dump mode) */
    if (!dumpOnly)
    {
        RegisterPrecompiledLambda(ip, lambda, procPtr, nsObj);
    }
    else
    {
        ByteCode* bc2 = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bc2);
        if (bc2)
            bc2->procPtr = NULL;
        Tcl_DecrRefCount(bodyBC);
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char*)procPtr);
    }

    Tcl_DecrRefCount(argList);
    Tcl_DecrRefCount(nsObj);
    return lambda;
}

static Tcl_Obj* ReadLiteral(TbcxIn* r, Tcl_Interp* ip, int depth, int dumpOnly)
{
    if (depth > TBCX_MAX_LITERAL_DEPTH)
    {
        R_Error(r, "tbcx: literal nesting too deep");
        return NULL;
    }

    uint32_t tag = 0;
    if (!Tbcx_R_U32(r, &tag))
        return NULL;

    switch (tag)
    {
        case TBCX_LIT_BIGNUM:
            return ReadLit_Bignum(r);
        case TBCX_LIT_BOOLEAN:
        {
            uint8_t b = 0;
            if (!Tbcx_R_U8(r, &b))
                return NULL;
            return Tcl_NewBooleanObj(b ? 1 : 0);
        }
        case TBCX_LIT_BYTEARR:
        {
            uint32_t n = 0;
            if (!Tbcx_R_U32(r, &n))
                return NULL;
            if (n > TBCX_MAX_STR)
            {
                R_Error(r, "tbcx: bytearray too large");
                return NULL;
            }
            unsigned char* buf = (unsigned char*)Tcl_Alloc(n ? n : 1u);
            if (n && !Tbcx_R_Bytes(r, buf, n))
            {
                Tcl_Free((char*)buf);
                return NULL;
            }
            Tcl_Obj* o = Tcl_NewByteArrayObj(buf, n);
            Tcl_Free((char*)buf);
            return o;
        }
        case TBCX_LIT_DICT:
        {
            uint32_t cnt = 0;
            if (!Tbcx_R_U32(r, &cnt))
                return NULL;
            if (cnt > TBCX_MAX_CONTAINER_ELEMS)
            {
                R_Error(r, "tbcx: dict too many pairs");
                return NULL;
            }
            Tcl_Obj* d = Tcl_NewDictObj();
            for (uint32_t i = 0; i < cnt; i++)
            {
                Tcl_Obj* k = ReadLiteral(r, ip, depth + 1, dumpOnly);
                if (!k)
                {
                    Tcl_IncrRefCount(d);
                    Tcl_DecrRefCount(d);
                    return NULL;
                }
                Tcl_Obj* v = ReadLiteral(r, ip, depth + 1, dumpOnly);
                if (!v)
                {
                    Tcl_IncrRefCount(k);
                    Tcl_DecrRefCount(k);
                    Tcl_IncrRefCount(d);
                    Tcl_DecrRefCount(d);
                    return NULL;
                }
                if (Tcl_DictObjPut(ip, d, k, v) != TCL_OK)
                {
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
        case TBCX_LIT_DOUBLE:
        {
            uint64_t bits = 0;
            if (!Tbcx_R_U64(r, &bits))
                return NULL;
            union
            {
                uint64_t u;
                double d;
            } u;
            u.u = bits;
            return Tcl_NewDoubleObj(u.d);
        }
        case TBCX_LIT_LIST:
        {
            uint32_t n = 0;
            if (!Tbcx_R_U32(r, &n))
                return NULL;
            if (n > TBCX_MAX_CONTAINER_ELEMS)
            {
                R_Error(r, "tbcx: list too many elements");
                return NULL;
            }
            Tcl_Obj* lst = Tcl_NewListObj(0, NULL);
            for (uint32_t i = 0; i < n; i++)
            {
                Tcl_Obj* e = ReadLiteral(r, ip, depth + 1, dumpOnly);
                if (!e)
                {
                    Tcl_IncrRefCount(lst);
                    Tcl_DecrRefCount(lst);
                    return NULL;
                }
                if (Tcl_ListObjAppendElement(ip, lst, e) != TCL_OK)
                {
                    Tcl_IncrRefCount(lst);
                    Tcl_DecrRefCount(lst);
                    return NULL;
                }
            }
            return lst;
        }
        case TBCX_LIT_WIDEINT:
        {
            uint64_t u = 0;
            if (!Tbcx_R_U64(r, &u))
                return NULL;
            /* stored as 2's complement */
            Tcl_WideInt wi = (Tcl_WideInt)u;
            return Tcl_NewWideIntObj(wi);
        }
        case TBCX_LIT_WIDEUINT:
        {
            uint64_t u = 0;
            if (!Tbcx_R_U64(r, &u))
                return NULL;
            if (u <= (uint64_t)TCL_INDEX_NONE)
            {
                return Tcl_NewWideIntObj((Tcl_WideInt)u);
            }
            else
            {
                /* promote to bignum */
                mp_int z;
                mp_err mrc = TclBN_mp_init(&z);
                if (mrc != MP_OKAY)
                {
                    R_Error(r, "tbcx: wideuint init");
                    return NULL;
                }
#if defined(MP_HAS_READ_UNSIGNED_BIN)
                unsigned char be[8];
                for (int i = 0; i < 8; i++)
                    be[i] = (unsigned char)((u >> (8 * (7 - i))) & 0xFF);
                mrc = TclBN_mp_read_unsigned_bin(&z, be, 8);
#else
                /* Portable fallback: build from 8 bytes with shift/add so we can
                   keep checking mp_err on each TomMath call. */
                for (int i = 7; i >= 0; i--)
                {
                    mrc = TclBN_mp_mul_2d(&z, 8, &z);
                    if (mrc != MP_OKAY)
                        break;
                    mrc = TclBN_mp_add_d(&z, (unsigned int)((u >> (8 * i)) & 0xFFu), &z);
                    if (mrc != MP_OKAY)
                        break;
                }
#endif
                if (mrc != MP_OKAY)
                {
                    TclBN_mp_clear(&z);
                    R_Error(r, "tbcx: wideuint import");
                    return NULL;
                }
                Tcl_Obj* o = Tcl_NewBignumObj(&z);
                return o;
            }
        }
        case TBCX_LIT_BYTECODE:
        {
            /* nsFQN string then compiled block */
            char* nsStr = NULL;
            uint32_t nsLen = 0;
            if (!Tbcx_R_LPString(r, &nsStr, &nsLen))
                return NULL;
            Tcl_Obj* nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
            Tcl_Free(nsStr);
            Namespace* nsPtr;
            if (dumpOnly)
                nsPtr = (Namespace*)Tcl_FindNamespace(ip, Tcl_GetString(nsObj), NULL, 0);
            else
                nsPtr = (Namespace*)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
            if (!nsPtr)
                nsPtr = (Namespace*)Tcl_GetGlobalNamespace(ip);
            Tcl_IncrRefCount(nsObj);
            uint32_t dummyNL = 0;
            /* setPrecompiled=1: TBCX literal bytecodes have no source text
               (string rep is "").  They MUST be marked precompiled so Tcl
               skips compile-epoch validation — otherwise ProcShim/OOShim
               installation bumps the epoch and Tcl would recompile from
               the empty string, losing all compiled content. */
            Tcl_Obj* bc = Tbcx_ReadBlock(r, ip, nsPtr, &dummyNL, 1, dumpOnly);
            Tcl_DecrRefCount(nsObj);
            return bc;
        }
        case TBCX_LIT_LAMBDA_BC:
            return ReadLit_LambdaBC(r, ip, depth, dumpOnly);
        case TBCX_LIT_STRING:
        {
            char* s = NULL;
            uint32_t n = 0;
            if (!Tbcx_R_LPString(r, &s, &n))
                return NULL;
            Tcl_Obj* o = Tcl_NewStringObj(s, (Tcl_Size)n);
            Tcl_Free(s);
            return o;
        }
        default:
            R_Error(r, "tbcx: unknown literal tag");
            return NULL;
    }
}

static int ReadAuxArray(TbcxIn* r, AuxData** auxOut, uint32_t* numAuxOut)
{
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_AUX)
    {
        R_Error(r, "tbcx: aux too many");
        return 0;
    }
    AuxData* arr = NULL;
    if (n)
        arr = (AuxData*)Tcl_Alloc(sizeof(AuxData) * n);

    uint32_t i = 0; /* declared here so fail_aux can reference it */
    for (i = 0; i < n; i++)
    {
        uint32_t tag = 0;
        if (!Tbcx_R_U32(r, &tag))
            goto fail_aux;

        if (tag == TBCX_AUX_JT_STR)
        {
            /* u32 cnt, then cnt × (LPString key, u32 pcOffset) */
            uint32_t cnt = 0;
            if (!Tbcx_R_U32(r, &cnt))
                goto fail_aux;
            if (cnt > TBCX_MAX_LITERALS)
            {
                R_Error(r, "tbcx: jump table too large");
                goto fail_aux;
            }
            JumptableInfo* info = (JumptableInfo*)Tcl_Alloc(sizeof(*info));
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
            for (uint32_t k = 0; k < cnt; k++)
            {
                char* s = NULL;
                uint32_t sl = 0;
                if (!Tbcx_R_LPString(r, &s, &sl))
                {
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                uint32_t off = 0;
                if (!Tbcx_R_U32(r, &off))
                {
                    Tcl_Free(s);
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                /* For TCL_STRING_KEYS, Tcl duplicates the key we pass in. */
                int newEntry = 0;
                Tcl_HashEntry* he = Tcl_CreateHashEntry(&info->hashTable, s, &newEntry);
                Tcl_SetHashValue(he, INT2PTR((int)off));
                Tcl_Free(s); /* safe: table holds its own duplicate */
            }
            arr[i].type = tbcxAuxJTStr;
            arr[i].clientData = info;
        }
        else if (tag == TBCX_AUX_JT_NUM)
        {
            uint32_t cnt = 0;
            if (!Tbcx_R_U32(r, &cnt))
                goto fail_aux;
            if (cnt > TBCX_MAX_LITERALS)
            {
                R_Error(r, "tbcx: numeric jump table too large");
                goto fail_aux;
            }
            JumptableNumInfo* info = (JumptableNumInfo*)Tcl_Alloc(sizeof(*info));
#if UINTPTR_MAX < 0xFFFFFFFFFFFFFFFFull
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
#else
            Tcl_InitHashTable(&info->hashTable, TCL_ONE_WORD_KEYS);
#endif
            for (uint32_t k = 0; k < cnt; k++)
            {
                uint64_t key = 0;
                uint32_t off = 0;

                if (!Tbcx_R_U64(r, &key) || !Tbcx_R_U32(r, &off))
                {
                    /* cleanup on short read */
                    Tcl_DeleteHashTable(&info->hashTable);
                    Tcl_Free(info);
                    goto fail_aux;
                }
                int newE = 0;
                Tcl_HashEntry* he;
#if UINTPTR_MAX < 0xFFFFFFFFFFFFFFFFull
                {
                    Tcl_Obj* keyObj = Tcl_ObjPrintf("%" PRIu64, key);
                    Tcl_IncrRefCount(keyObj);
                    he = Tcl_CreateHashEntry(&info->hashTable, Tcl_GetString(keyObj), &newE);
                    Tcl_DecrRefCount(keyObj);
                }
#else
                he = Tcl_CreateHashEntry(&info->hashTable, (const char*)(intptr_t)key, &newE);
#endif
                Tcl_SetHashValue(he, INT2PTR((int)off));
            }
            arr[i].type = tbcxAuxJTNum;
            arr[i].clientData = info;
        }
        else if (tag == TBCX_AUX_DICTUPD)
        {
            uint32_t L = 0;
            if (!Tbcx_R_U32(r, &L))
                goto fail_aux;
            if (L > TBCX_MAX_LITERALS)
            {
                R_Error(r, "tbcx: dict-update aux too large");
                goto fail_aux;
            }
            size_t elemBytes = (size_t)L * sizeof(Tcl_Size);
            /* Overflow check: verify the multiplication didn't wrap */
            if (L > 0 && elemBytes / L != sizeof(Tcl_Size))
            {
                R_Error(r, "tbcx: dict-update aux overflow");
                goto fail_aux;
            }
            size_t bytes = offsetof(DictUpdateInfo, varIndices) + elemBytes;
            DictUpdateInfo* info = (DictUpdateInfo*)Tcl_Alloc(bytes);
            memset(info, 0, bytes);
            info->length = (Tcl_Size)L;
            for (uint32_t k = 0; k < L; k++)
            {
                uint32_t x = 0;
                if (!Tbcx_R_U32(r, &x))
                {
                    Tcl_Free(info);
                    goto fail_aux;
                }
                info->varIndices[k] = (Tcl_Size)x;
            }
            arr[i].type = tbcxAuxDictUpdate;
            arr[i].clientData = info;
        }
        else if (tag == TBCX_AUX_NEWFORE)
        {
            if (!tbcxAuxNewForeach)
            {
                R_Error(r, "tbcx: NewForeachInfo AuxData type not available in this Tcl build");
                goto fail_aux;
            }

            uint32_t numLists = 0, loopCtU = 0, firstValU = 0, dupNumLists = 0;
            if (!Tbcx_R_U32(r, &numLists) || !Tbcx_R_U32(r, &loopCtU) || !Tbcx_R_U32(r, &firstValU) ||
                !Tbcx_R_U32(r, &dupNumLists))
                goto fail_aux;
            if (dupNumLists != numLists)
            {
                R_Error(r, "tbcx: foreach aux mismatch");
                goto fail_aux;
            }
            if (numLists > TBCX_MAX_LITERALS)
            {
                R_Error(r, "tbcx: foreach aux too many lists");
                goto fail_aux;
            }
            size_t listBytes = (size_t)numLists * sizeof(ForeachVarList*);
            if (numLists > 0 && listBytes / numLists != sizeof(ForeachVarList*))
            {
                R_Error(r, "tbcx: foreach aux overflow");
                goto fail_aux;
            }
            size_t bytes = offsetof(ForeachInfo, varLists) + listBytes;
            ForeachInfo* info = (ForeachInfo*)Tcl_Alloc(bytes);
            memset(info, 0, bytes);
            info->numLists = (Tcl_Size)numLists;
            info->firstValueTemp = (Tcl_LVTIndex)(int32_t)firstValU;
            info->loopCtTemp = (Tcl_LVTIndex)(int32_t)loopCtU;
            for (uint32_t iL = 0; iL < numLists; iL++)
            {
                uint32_t nv = 0;
                if (!Tbcx_R_U32(r, &nv))
                {
                    arr[i].type = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++; /* count this entry so fail_aux frees it */
                    goto fail_aux;
                }
                if (nv > TBCX_MAX_LITERALS)
                {
                    R_Error(r, "tbcx: foreach varlist too large");
                    arr[i].type = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++;
                    goto fail_aux;
                }
                size_t varIdxBytes = (size_t)nv * sizeof(Tcl_LVTIndex);
                if (nv > 0 && varIdxBytes / nv != sizeof(Tcl_LVTIndex))
                {
                    R_Error(r, "tbcx: foreach varlist overflow");
                    arr[i].type = tbcxAuxNewForeach;
                    arr[i].clientData = info;
                    i++;
                    goto fail_aux;
                }
                size_t vlBytes = offsetof(ForeachVarList, varIndexes) + varIdxBytes;
                ForeachVarList* vl = (ForeachVarList*)Tcl_Alloc(vlBytes);
                memset(vl, 0, vlBytes);
                vl->numVars = (Tcl_Size)nv;
                for (uint32_t j = 0; j < nv; j++)
                {
                    uint32_t idx = 0;
                    if (!Tbcx_R_U32(r, &idx))
                    {
                        Tcl_Free(vl);
                        arr[i].type = tbcxAuxNewForeach;
                        arr[i].clientData = info;
                        i++;
                        goto fail_aux;
                    }
                    vl->varIndexes[j] = (Tcl_LVTIndex)idx;
                }
                info->varLists[iL] = vl;
            }
            arr[i].type = tbcxAuxNewForeach;
            arr[i].clientData = info;
        }
        else
        {
            R_Error(r, "tbcx: unsupported AuxData tag");
            goto fail_aux;
        }
    }
    *auxOut = arr;
    *numAuxOut = n;
    return 1;

fail_aux:
    /* Free already-completed AuxData entries [0..i) via their type's freeProc */
    for (uint32_t j = 0; j < i; j++)
    {
        if (arr[j].type && arr[j].type->freeProc && arr[j].clientData)
            arr[j].type->freeProc(arr[j].clientData);
    }
    if (arr)
        Tcl_Free(arr);
    return 0;
}

static int ReadExceptions(TbcxIn* r, ExceptionRange** exOut, uint32_t* numOut)
{
    uint32_t n = 0;
    if (!Tbcx_R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_EXCEPT)
    {
        R_Error(r, "tbcx: too many exceptions");
        return 0;
    }
    ExceptionRange* arr = NULL;
    if (n)
        arr = (ExceptionRange*)Tcl_Alloc(sizeof(ExceptionRange) * n);
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t type8 = 0;
        uint32_t len = 0;
        uint32_t nesting = 0, from = 0, cont = 0, brk = 0, cat = 0;
        if (!Tbcx_R_U8(r, &type8) || !Tbcx_R_U32(r, &nesting) || !Tbcx_R_U32(r, &from) || !Tbcx_R_U32(r, &len) ||
            !Tbcx_R_U32(r, &cont) || !Tbcx_R_U32(r, &brk) || !Tbcx_R_U32(r, &cat))
        {
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
        arr[i].type = (ExceptionRangeType)type8;
        arr[i].nestingLevel = (int)nesting;
        arr[i].codeOffset = (int)from;
        arr[i].numCodeBytes = (int)len;
        arr[i].continueOffset = (int)cont;
        arr[i].breakOffset = (int)brk;
        arr[i].catchOffset = (int)cat;
    }
    *exOut = arr;
    *numOut = n;
    return 1;
}

Tcl_Obj*
Tbcx_ReadBlock(TbcxIn* r, Tcl_Interp* ip, Namespace* nsForDefault, uint32_t* numLocalsOut, int setPrecompiled, int dumpOnly)
{
    /* 1) code */
    uint32_t codeLen = 0;
    if (!Tbcx_R_U32(r, &codeLen))
        return NULL;
    if (codeLen > TBCX_MAX_CODE)
    {
        R_Error(r, "tbcx: code too large");
        return NULL;
    }
    unsigned char* code = (unsigned char*)Tcl_Alloc(codeLen ? codeLen : 1u);
    if (codeLen && !Tbcx_R_Bytes(r, code, codeLen))
    {
        Tcl_Free((char*)code);
        return NULL;
    }

    /* 2) literals */
    uint32_t numLits = 0;
    if (!Tbcx_R_U32(r, &numLits))
    {
        Tcl_Free((char*)code);
        return NULL;
    }
    if (numLits > TBCX_MAX_LITERALS)
    {
        R_Error(r, "tbcx: too many literals");
        Tcl_Free((char*)code);
        return NULL;
    }
    /* stack-allocate for small literal counts */
    Tcl_Obj* litStack[64];
    Tcl_Obj** lits = NULL;
    int litsOnHeap = 0;
    if (numLits)
    {
        if (numLits <= 64)
        {
            lits = litStack;
        }
        else
        {
            lits = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * numLits);
            litsOnHeap = 1;
        }
    }
    for (uint32_t i = 0; i < numLits; i++)
    {
        Tcl_Obj* lit = ReadLiteral(r, ip, 0, dumpOnly);
        if (!lit)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                Tcl_DecrRefCount(lits[j]);
            }
            if (litsOnHeap)
                Tcl_Free(lits);
            Tcl_Free((char*)code);
            return NULL;
        }
        Tcl_IncrRefCount(lit); /* Protect immediately — refcount 0→1 */
        lits[i] = lit;
    }

    /* 3) AuxData */
    AuxData* auxArr = NULL;
    uint32_t numAux = 0;
    if (!ReadAuxArray(r, &auxArr, &numAux))
    {
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char*)code);
        return NULL;
    }

    /* 4) Exceptions */
    ExceptionRange* exArr = NULL;
    uint32_t numEx = 0;
    if (!ReadExceptions(r, &exArr, &numEx))
    {
        if (auxArr)
            Tcl_Free(auxArr);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char*)code);
        return NULL;
    }

    /* 5) Epilogue: maxStack, reserved, numLocals */
    uint32_t maxStack = 0, reserved = 0, numLocals = 0;
    if (!Tbcx_R_U32(r, &maxStack) || !Tbcx_R_U32(r, &reserved) || !Tbcx_R_U32(r, &numLocals))
    {
        if (exArr)
            Tcl_Free(exArr);
        if (auxArr)
            Tcl_Free(auxArr);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]); /* refcount 1→0, freed */
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char*)code);
        return NULL;
    }

    (void)reserved; /* wire-format placeholder for future use */

    /* Hard caps on untrusted values from the .tbcx stream to prevent
     * huge allocations, integer wrap, or allocator panic.  These limits
     * are generous for legitimate bytecode but reject pathological inputs. */
    if (maxStack > TBCX_MAX_STACK)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: maxStack %u exceeds limit %u", maxStack, TBCX_MAX_STACK));
        if (exArr)
            Tcl_Free(exArr);
        if (auxArr)
            Tcl_Free(auxArr);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]);
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char*)code);
        return NULL;
    }
    if (numLocals > TBCX_MAX_LOCALS)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numLocals %u exceeds limit %u", numLocals, TBCX_MAX_LOCALS));
        if (exArr)
            Tcl_Free(exArr);
        if (auxArr)
            Tcl_Free(auxArr);
        for (uint32_t j = 0; j < numLits; j++)
            Tcl_DecrRefCount(lits[j]);
        if (litsOnHeap)
            Tcl_Free(lits);
        Tcl_Free((char*)code);
        return NULL;
    }

    if (numLocalsOut)
        *numLocalsOut = numLocals;

    Tcl_Obj* nameObjsStack[64];
    Tcl_Obj** nameObjs = NULL;
    int nameOnHeap = 0;
    if (numLocals > 0)
    {
        if (numLocals <= 64)
        {
            nameObjs = nameObjsStack;
        }
        else
        {
            nameObjs = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)numLocals);
            nameOnHeap = 1;
        }
        for (uint32_t i = 0; i < numLocals; i++)
        {
            char* s = NULL;
            uint32_t len = 0;
            if (!Tbcx_R_LPString(r, &s, &len))
            {
                if (nameObjs)
                {
                    for (uint32_t k = 0; k < i; k++)
                        Tcl_DecrRefCount(nameObjs[k]);
                    if (nameOnHeap)
                        Tcl_Free(nameObjs);
                }
                if (exArr)
                    Tcl_Free(exArr);
                if (auxArr)
                    Tcl_Free(auxArr);
                /* Release the protective refcounts taken on literal objects
                 * before freeing the lits array — otherwise they leak. */
                for (uint32_t j = 0; j < numLits; j++)
                    Tcl_DecrRefCount(lits[j]);
                if (litsOnHeap)
                    Tcl_Free(lits);
                Tcl_Free((char*)code);
                return NULL;
            }
            Tcl_Obj* o = Tcl_NewStringObj(s, (Tcl_Size)len);
            Tcl_Free(s);
            nameObjs[i] = o; /* refcount 0 for now */
        }
    }

    /* Build bytecode object */
    Tcl_Obj* bc =
        ByteCodeObj(ip, nsForDefault, code, codeLen, lits, numLits, auxArr, numAux, exArr, numEx, (int)maxStack, setPrecompiled);

    if (exArr)
        Tcl_Free(exArr);
    if (auxArr)
        Tcl_Free(auxArr);
    /* Drop our protective refcount on literals — ByteCodeObj has its own */
    for (uint32_t j = 0; j < numLits; j++)
        Tcl_DecrRefCount(lits[j]);
    if (litsOnHeap)
        Tcl_Free(lits);
    if (code)
        Tcl_Free(code);

    if (bc && numLocals > 0 && nameObjs)
    {
        ByteCode* bcPtr = NULL;
        ByteCodeGetInternalRep(bc, tbcxTyBytecode, bcPtr);
        if (bcPtr)
        {
            size_t bytes = offsetof(LocalCache, varName0) + (size_t)numLocals * sizeof(Tcl_Obj*);
            LocalCache* lc = (LocalCache*)Tcl_Alloc(bytes);
            lc->refCount = 1;
            lc->numVars = (Tcl_Size)numLocals;
            Tcl_Obj** dst = (Tcl_Obj**)&lc->varName0;
            for (uint32_t i = 0; i < numLocals; i++)
            {
                dst[i] = nameObjs[i];
                Tcl_IncrRefCount(dst[i]);
            }
            bcPtr->localCachePtr = lc;
        }
        else
        {
            /* bcPtr is NULL — nameObjs entries are at refcount 0 and
               will not be transferred to a LocalCache.  Release them. */
            for (uint32_t i = 0; i < numLocals; i++)
            {
                if (nameObjs[i])
                {
                    Tcl_IncrRefCount(nameObjs[i]);
                    Tcl_DecrRefCount(nameObjs[i]);
                }
            }
        }
        if (nameOnHeap)
            Tcl_Free(nameObjs);
    }
    else if (numLocals > 0 && nameObjs)
    {
        /* bc is NULL — ByteCodeObj failed.  Release the refcount-0
           nameObjs entries and free the heap array to avoid leaks. */
        for (uint32_t i = 0; i < numLocals; i++)
        {
            if (nameObjs[i])
            {
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
static int ProcShim_DirectInstall(ProcShim* ps, Tcl_Interp* ip, Tcl_Obj* fqn, Tcl_Obj* nameObj, Proc* preProc, Tcl_Obj* savedArgs)
{
    /* Build a new Proc directly from our precompiled data. */
    Proc* newProc = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(newProc, 0, sizeof(Proc));
    newProc->iPtr = (Interp*)ip;
    newProc->refCount = 1;
    newProc->bodyPtr = preProc->bodyPtr;
    Tcl_IncrRefCount(newProc->bodyPtr);
    {
        CompiledLocal *first = NULL, *last = NULL;
        int numA = 0;
        if (Tbcx_BuildLocals(ip, savedArgs, &first, &last, &numA) != TCL_OK)
        {
            Tcl_DecrRefCount(newProc->bodyPtr);
            Tcl_Free((char*)newProc);
            return TCL_ERROR;
        }
        newProc->numArgs = numA;
        newProc->numCompiledLocals = numA;
        newProc->firstLocalPtr = first;
        newProc->lastLocalPtr = last;
    }
    CompiledLocals(newProc, preProc->numCompiledLocals);

    /* Resolve namespace */
    const char* fqnStr = Tcl_GetString(fqn);
    Namespace* nsPtr = (Namespace*)Tcl_GetCurrentNamespace(ip);
    if (fqnStr[0] == ':' && fqnStr[1] == ':')
    {
        const char* lastSep = fqnStr;
        for (const char* p2 = fqnStr; *p2; p2++)
        {
            if (p2[0] == ':' && p2[1] == ':')
                lastSep = p2;
        }
        if (lastSep > fqnStr)
        {
            Tcl_Obj* nsObj = Tcl_NewStringObj(fqnStr, (Tcl_Size)(lastSep - fqnStr));
            Tcl_IncrRefCount(nsObj);
            nsPtr = (Namespace*)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
            Tcl_DecrRefCount(nsObj);
        }
        else
        {
            nsPtr = (Namespace*)Tcl_GetGlobalNamespace(ip);
        }
    }

    /* Register command using captured handler pointers. */
    Tcl_Command token = Tcl_CreateObjCommand2(ip, fqnStr, ps->procDispatchObj, newProc, ps->procDeleteProc);
    if (!token)
    {
        TclProcDeleteProc(newProc);
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: failed to create command \"%s\"", fqnStr));
        return TCL_ERROR;
    }
    Command* cmdPtr = (Command*)token;
    if (ps->procDispatchNre)
        cmdPtr->nreProc2 = ps->procDispatchNre;
    newProc->cmdPtr = cmdPtr;

    /* Fix bytecode linkage */
    ByteCode* bc = NULL;
    ByteCodeGetInternalRep(newProc->bodyPtr, tbcxTyBytecode, bc);
    if (bc)
    {
        bc->procPtr = newProc;
        RefreshBC(bc, ip, nsPtr);
        if (bc->localCachePtr)
        {
            FixCompiledLocalNames(newProc, bc->localCachePtr);
            LocalCache* old = bc->localCachePtr;
            bc->localCachePtr = NULL;
            if (--old->refCount <= 0)
            {
                Tcl_Obj** names = (Tcl_Obj**)&old->varName0;
                for (Tcl_Size j = 0; j < old->numVars; j++)
                    if (names[j])
                        Tcl_DecrRefCount(names[j]);
                Tcl_Free(old);
            }
        }
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

static int CmdProcShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[])
{
    ProcShim* ps = (ProcShim*)cd;

    if (objc != 4)
    {
        /* Forward to the original "proc" handler (synchronous entry). */
        return ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
    }

    Tcl_Obj *nameObj = objv[1], *argsObj = objv[2];
    const char* nm = Tcl_GetString(nameObj);
    Tcl_Obj* fqn = NULL;
    if (nm[0] == ':' && nm[1] == ':')
    {
        fqn = nameObj;
    }
    else
    {
        Tcl_Namespace* cur = Tcl_GetCurrentNamespace(ip);
        const char* curName = cur ? cur->fullName : "::";
        fqn = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0'))
        {
            Tcl_AppendToObj(fqn, "::", 2);
        }
        Tcl_AppendObjToObj(fqn, nameObj);
    }

    Tcl_HashEntry* he = Tcl_FindHashEntry(&ps->procsByFqn, Tcl_GetString(fqn));

    /* Indexed marker override: if the body argument starts with
       TBCX_PROC_MARKER_PFX, use the encoded index to select the
       pair directly from procsByIdx.  This handles conflicting
       proc definitions across if/else branches, where the same
       FQN appears multiple times with different compiled bodies. */
    Tcl_Obj* markerPair = NULL;
    {
        Tcl_Size bLen = 0;
        const char* bStr = Tcl_GetStringFromObj(objv[3], &bLen);
        if (bLen >= TBCX_PROC_MARKER_PFX_LEN && memcmp(bStr, TBCX_PROC_MARKER_PFX, TBCX_PROC_MARKER_PFX_LEN) == 0 &&
            ps->procsByIdx)
        {
            /* Parse decimal index after the prefix */
            unsigned idx = 0;
            const char* dp = bStr + TBCX_PROC_MARKER_PFX_LEN;
            int hasDigit = 0;
            while (*dp >= '0' && *dp <= '9')
            {
                idx = idx * 10u + (unsigned)(*dp - '0');
                dp++;
                hasDigit = 1;
            }
            if (hasDigit && *dp == '\0' && idx < ps->numProcsIdx && ps->procsByIdx[idx])
            {
                markerPair = ps->procsByIdx[idx];
            }
        }
    }

    if (he || markerPair)
    {
        Tcl_Obj* pair = markerPair ? markerPair : (Tcl_Obj*)Tcl_GetHashValue(he);
        Tcl_Obj *savedArgs = NULL, *procBody = NULL;
        {
            Tcl_Size pairLen = 0;
            Tcl_Obj** pairElems = NULL;
            if (Tcl_ListObjGetElements(ip, pair, &pairLen, &pairElems) == TCL_OK && pairLen >= 2)
            {
                savedArgs = pairElems[0];
                procBody = pairElems[1];
            }
        }
        if (savedArgs && procBody)
        {
            Tcl_Size _d = 0;
            (void)Tcl_ListObjLength(ip, argsObj, &_d);
            (void)Tcl_ListObjLength(ip, savedArgs, &_d);
            Tcl_Size aLen = 0, bLen = 0;
            const char* a = Tcl_GetStringFromObj(argsObj, &aLen);
            const char* b = Tcl_GetStringFromObj(savedArgs, &bLen);

            if (aLen == bLen && memcmp(a, b, (size_t)aLen) == 0)
            {
                /* Get our precompiled Proc from the registry. */
                const Tcl_ObjInternalRep* pbIR = Tcl_FetchInternalRep(procBody, tbcxTyProcBody);
                Proc* preProc = pbIR ? (Proc*)pbIR->twoPtrValue.ptr1 : NULL;
                if (!preProc || !preProc->bodyPtr)
                {
                    if (fqn != nameObj)
                        Tcl_DecrRefCount(fqn);
                    return ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
                }

                /* ---- direct install path ----
                 * Once we have captured the handler pointers from the first
                 * proc created via the slow path, we can skip TclCreateProc
                 * entirely for subsequent procs.  This avoids N compilations
                 * of the empty body string "". */
                if (ps->haveDispatch)
                {
                    int drc = ProcShim_DirectInstall(ps, ip, fqn, nameObj, preProc, savedArgs);
                    if (fqn != nameObj)
                        Tcl_DecrRefCount(fqn);
                    return drc;
                }

                /* ---- Slow path (first proc): create via TclCreateProc,
                   capture handler pointers, then swap body. ---- */
                {
                    int rc = ps->savedObjProc2(ps->savedClientData2, ip, objc, objv);
                    if (rc != TCL_OK)
                    {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }
                    Tcl_Command cmd = Tcl_FindCommand(ip, Tcl_GetString(fqn), NULL, TCL_GLOBAL_ONLY);
                    if (!cmd)
                    {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }
                    Command* cmdPtr = (Command*)cmd;
                    Proc* newProc = (Proc*)cmdPtr->objClientData2;
                    if (!newProc)
                    {
                        if (fqn != nameObj)
                            Tcl_DecrRefCount(fqn);
                        return rc;
                    }

                    /* P3: Capture handler pointers for future direct installs */
                    if (!ps->haveDispatch)
                    {
                        ps->procDispatchObj = cmdPtr->objProc2;
                        ps->procDispatchNre = cmdPtr->nreProc2;
                        ps->procDeleteProc = cmdPtr->deleteProc;
                        ps->haveDispatch = 1;
                    }

                    /* Swap bodyPtr */
                    Tcl_Obj* preBody = preProc->bodyPtr;
                    Tcl_IncrRefCount(preBody);
                    Tcl_DecrRefCount(newProc->bodyPtr);
                    newProc->bodyPtr = preBody;
                    CompiledLocals(newProc, preProc->numCompiledLocals);
                    {
                        ByteCode* bc = NULL;
                        ByteCodeGetInternalRep(preBody, tbcxTyBytecode, bc);
                        if (bc)
                        {
                            bc->procPtr = newProc;
                            RefreshBC(bc, ip, cmdPtr->nsPtr);
                            if (bc->localCachePtr)
                            {
                                FixCompiledLocalNames(newProc, bc->localCachePtr);
                                LocalCache* old = bc->localCachePtr;
                                bc->localCachePtr = NULL;
                                if (--old->refCount <= 0)
                                {
                                    Tcl_Obj** names = (Tcl_Obj**)&old->varName0;
                                    for (Tcl_Size j = 0; j < old->numVars; j++)
                                        if (names[j])
                                            Tcl_DecrRefCount(names[j]);
                                    Tcl_Free(old);
                                }
                            }
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
 * Invalidates the cached Command* to prevent stale-pointer writes in
 * DelProcShim.  Without this trace, a loaded script that renames or deletes
 * "proc" during execution would cause DelProcShim to write through a
 * freed Command struct (use-after-free / process crash). */
static void ProcCmdDeleteTrace(void* cd, Tcl_Interp* interp, const char* oldName, const char* newName, int flags)
{
    ProcShim* ps = (ProcShim*)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE))
    {
        ps->procCmdPtr = NULL;
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
static int AddProcShim(Tcl_Interp* ip, ProcShim* ps)
{
    memset(ps, 0, sizeof(*ps));
    Tcl_InitHashTable(&ps->procsByFqn, TCL_STRING_KEYS);

    Tcl_Command token = Tcl_FindCommand(ip, "proc", NULL, 0);
    if (!token)
    {
        Tcl_DeleteHashTable(&ps->procsByFqn);
        return TCL_ERROR;
    }

    ps->interp = ip;
    ps->procCmdPtr = (Command*)token;
    ps->savedObjProc2 = ps->procCmdPtr->objProc2;
    ps->savedNreProc2 = ps->procCmdPtr->nreProc2;
    ps->savedClientData2 = ps->procCmdPtr->objClientData2;

    /* Install a command trace so we know if "proc" is renamed or deleted
       while the loaded script is executing. */
    Tcl_TraceCommand(ip, "proc", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ProcCmdDeleteTrace, ps);
    ps->traceInstalled = 1;

    /* Intercept BOTH objProc2 AND nreProc2.
       In Tcl 9.1, "proc" is registered via Tcl_NRCreateCommand, which
       sets nreProc2 to a non-NULL value.  The bytecode engine (TEBC)
       dispatches through nreProc2 when non-NULL, completely bypassing
       objProc2.  The old code only swapped objProc2, so the shim was
       never called from bytecode — the original handler ran unintercepted,
       clobbered bodyPtr, and the proc crashed at invocation time.

       proc's handler (TclNRProcObjCmd) creates the proc synchronously
       and does NOT enqueue NRE callbacks, so it is safe for our shim
       to call it directly and do post-processing (bodyPtr restoration)
       after it returns. */
    ps->procCmdPtr->objProc2 = CmdProcShim;
    ps->procCmdPtr->objClientData2 = ps;
    if (ps->procCmdPtr->nreProc2)
    {
        ps->procCmdPtr->nreProc2 = CmdProcShim;
    }

    return TCL_OK;
}

static void DelProcShim(Tcl_Interp* ip, ProcShim* ps)
{
    /* Remove command trace before attempting any restore.
       If the command was already deleted, the trace fired and set
       procCmdPtr = NULL and traceInstalled = 0. */
    if (ps->traceInstalled)
    {
        Tcl_UntraceCommand(ip, "proc", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ProcCmdDeleteTrace, ps);
        ps->traceInstalled = 0;
    }

    /* Only restore handlers if the Command struct is still alive. */
    if (ps->procCmdPtr)
    {
        ps->procCmdPtr->objProc2 = ps->savedObjProc2;
        ps->procCmdPtr->objClientData2 = ps->savedClientData2;
        if (ps->savedNreProc2)
        {
            ps->procCmdPtr->nreProc2 = ps->savedNreProc2;
        }
    }
    Tcl_HashSearch s;
    Tcl_HashEntry* e;
    for (e = Tcl_FirstHashEntry(&ps->procsByFqn, &s); e; e = Tcl_NextHashEntry(&s))
    {
        Tcl_Obj* val = (Tcl_Obj*)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&ps->procsByFqn);
    /* Free indexed array */
    if (ps->procsByIdx)
    {
        for (uint32_t i = 0; i < ps->numProcsIdx; i++)
        {
            if (ps->procsByIdx[i])
                Tcl_DecrRefCount(ps->procsByIdx[i]);
        }
        Tcl_Free((char*)ps->procsByIdx);
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

static int CmdApplyShim(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[])
{
    ApplyShim* as = (ApplyShim*)cd;

    if (objc >= 2 && tbcxTyLambda)
    {
        Tcl_Obj* lambda = objv[1];

        /* Fast path: lambdaExpr internal rep is still present — Tcl handles it */
        if (!Tcl_FetchInternalRep(lambda, tbcxTyLambda))
        {
            /* lambdaExpr rep missing (shimmer happened) — try to recover */
            Tcl_HashEntry* he = Tcl_FindHashEntry(&as->lambdaRegistry, (const char*)lambda);
            if (he)
            {
                ApplyLambdaEntry* le = (ApplyLambdaEntry*)Tcl_GetHashValue(he);
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
static int CmdApplyShimNre(void* cd, Tcl_Interp* ip, Tcl_Size objc, Tcl_Obj* const objv[])
{
    ApplyShim* as = (ApplyShim*)cd;

    if (objc >= 2 && tbcxTyLambda)
    {
        Tcl_Obj* lambda = objv[1];

        if (!Tcl_FetchInternalRep(lambda, tbcxTyLambda))
        {
            Tcl_HashEntry* he = Tcl_FindHashEntry(&as->lambdaRegistry, (const char*)lambda);
            if (he)
            {
                ApplyLambdaEntry* le = (ApplyLambdaEntry*)Tcl_GetHashValue(he);
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
 * ApplyShimCleanup.  Without this trace, if "apply" is deleted or renamed
 * before interpreter destruction, ApplyShimCleanup would write through a
 * freed Command struct (use-after-free / process crash). */
static void ApplyCmdDeleteTrace(void* cd, Tcl_Interp* interp, const char* oldName, const char* newName, int flags)
{
    ApplyShim* as = (ApplyShim*)cd;
    (void)interp;
    (void)oldName;
    (void)newName;
    if (flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE))
    {
        as->applyCmdPtr = NULL;
        as->traceInstalled = 0; /* trace auto-removed on delete */
    }
}

static void ApplyShimCleanup(void* cd, Tcl_Interp* ip)
{
    ApplyShim* as = (ApplyShim*)cd;

    /* Remove command trace before attempting any restore.
       If the command was already deleted, the trace fired and set
       applyCmdPtr = NULL and traceInstalled = 0. */
    if (as->traceInstalled)
    {
        Tcl_UntraceCommand(ip, "apply", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ApplyCmdDeleteTrace, as);
        as->traceInstalled = 0;
    }

    /* Only restore original [apply] handlers if the Command struct is still alive */
    if (as->applyCmdPtr)
    {
        as->applyCmdPtr->objProc2 = as->savedApplyProc;
        as->applyCmdPtr->objClientData2 = as->savedApplyCD;
        if (as->savedApplyNre)
        {
            as->applyCmdPtr->nreProc2 = as->savedApplyNre;
        }
    }

    /* Free registry entries */
    Tcl_HashSearch s;
    Tcl_HashEntry* e;
    for (e = Tcl_FirstHashEntry(&as->lambdaRegistry, &s); e; e = Tcl_NextHashEntry(&s))
    {
        /* Release the lambda reference held by RegisterPrecompiledLambda */
        Tcl_Obj* lambdaKey = (Tcl_Obj*)Tcl_GetHashKey(&as->lambdaRegistry, e);
        if (lambdaKey)
            Tcl_DecrRefCount(lambdaKey);

        ApplyLambdaEntry* le = (ApplyLambdaEntry*)Tcl_GetHashValue(e);
        if (le)
        {
            if (le->procPtr && --le->procPtr->refCount <= 0)
            {
                /* Proc refCount hit zero — the registry was the last owner.
                   Clean up the Proc properly: release bodyPtr and free
                   compiled locals, then the Proc struct itself. */
                TclProcCleanupProc(le->procPtr);
            }
            if (le->nsObj)
                Tcl_DecrRefCount(le->nsObj);
            Tcl_Free(le);
        }
    }
    Tcl_DeleteHashTable(&as->lambdaRegistry);
    Tcl_Free(as);
}

/* ApplyShimPurgeStale — remove registry entries for lambda objects that
 * are only kept alive by the registry itself (refCount == 1).  These are
 * lambdas whose ByteCode literal pool has been freed, meaning they can
 * never be invoked again.  Releasing them prevents unbounded accumulation
 * in long-running interpreters that load many .tbcx files.
 *
 * Called lazily from EnsureApplyShim on subsequent tbcx::load calls. */
static void ApplyShimPurgeStale(ApplyShim* as)
{
    if (!as)
        return;

    /* Collect keys to remove (can't modify hash during iteration). */
    Tcl_Obj* staleStack[32];
    Tcl_Obj** stale = staleStack;
    int nStale = 0, staleCap = 32;
    int onHeap = 0;

    Tcl_HashSearch s;
    Tcl_HashEntry* e;
    for (e = Tcl_FirstHashEntry(&as->lambdaRegistry, &s); e; e = Tcl_NextHashEntry(&s))
    {
        Tcl_Obj* lambda = (Tcl_Obj*)Tcl_GetHashKey(&as->lambdaRegistry, e);
        if (lambda && lambda->refCount <= 1)
        {
            /* Only our registry holds a reference — this lambda is stale */
            if (nStale >= staleCap)
            {
                staleCap *= 2;
                if (!onHeap)
                {
                    Tcl_Obj** tmp = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)staleCap);
                    memcpy(tmp, stale, sizeof(Tcl_Obj*) * (size_t)nStale);
                    stale = tmp;
                    onHeap = 1;
                }
                else
                {
                    stale = (Tcl_Obj**)Tcl_Realloc(stale, sizeof(Tcl_Obj*) * (size_t)staleCap);
                }
            }
            stale[nStale++] = lambda;
        }
    }

    /* Remove stale entries */
    for (int i = 0; i < nStale; i++)
    {
        Tcl_Obj* lambda = stale[i];
        e = Tcl_FindHashEntry(&as->lambdaRegistry, (const char*)lambda);
        if (e)
        {
            ApplyLambdaEntry* le = (ApplyLambdaEntry*)Tcl_GetHashValue(e);
            if (le)
            {
                if (le->procPtr && --le->procPtr->refCount <= 0)
                {
                    TclProcCleanupProc(le->procPtr);
                }
                if (le->nsObj)
                    Tcl_DecrRefCount(le->nsObj);
                Tcl_Free(le);
            }
            Tcl_DeleteHashEntry(e);
            /* Release the registry's reference (may free the lambda) */
            Tcl_DecrRefCount(lambda);
        }
    }

    if (onHeap)
        Tcl_Free(stale);
}

/* ==========================================================================
 * Purges stale lambda entries from the per-interp ApplyShim registry.
 * Safe to call even when no ApplyShim has been installed (no-op).
 * ========================================================================== */

void TbcxApplyShimPurgeAll(Tcl_Interp* ip)
{
    ApplyShim* as = (ApplyShim*)Tcl_GetAssocData(ip, TBCX_APPLY_SHIM_KEY, NULL);
    if (as)
        ApplyShimPurgeStale(as);
}

/* EnsureApplyShim — get or create the per-interp ApplyShim.
 * First call installs the shim on [apply] and registers it as AssocData.
 * Subsequent calls (from additional tbcx::load invocations) return the
 * existing shim — new lambda entries are simply added to the registry. */
static ApplyShim* EnsureApplyShim(Tcl_Interp* ip)
{
    ApplyShim* as = (ApplyShim*)Tcl_GetAssocData(ip, TBCX_APPLY_SHIM_KEY, NULL);
    if (as)
    {
        /* Subsequent load: purge stale entries before adding new ones */
        ApplyShimPurgeStale(as);
        return as;
    }

    Tcl_Command token = Tcl_FindCommand(ip, "apply", NULL, 0);
    if (!token)
        return NULL;

    as = (ApplyShim*)Tcl_Alloc(sizeof(ApplyShim));
    memset(as, 0, sizeof(*as));
    Tcl_InitHashTable(&as->lambdaRegistry, TCL_ONE_WORD_KEYS);

    as->interp = ip;
    as->applyCmdPtr = (Command*)token;
    as->savedApplyProc = as->applyCmdPtr->objProc2;
    as->savedApplyNre = as->applyCmdPtr->nreProc2;
    as->savedApplyCD = as->applyCmdPtr->objClientData2;

    /* Install a command trace so we know if "apply" is renamed or deleted
       before cleanup runs.  Without this, ApplyShimCleanup would write
       through a stale Command* — a crash-class UAF bug. */
    Tcl_TraceCommand(ip, "apply", TCL_TRACE_RENAME | TCL_TRACE_DELETE, ApplyCmdDeleteTrace, as);
    as->traceInstalled = 1;

    /* Intercept BOTH objProc2 AND nreProc2, matching the ProcShim pattern.
       In Tcl 9.1, [apply] is NRE-enabled (nreProc2 != NULL), so the bytecode
       engine dispatches through nreProc2 — bypassing objProc2 entirely.
       Without intercepting nreProc2, shimmer recovery for precompiled lambdas
       would silently fail when called from compiled code.
       The NRE path uses a separate shim function that forwards to the
       original savedApplyNre, preserving the NRE trampoline for deeply
       recursive lambda calls. */
    as->applyCmdPtr->objProc2 = CmdApplyShim;
    as->applyCmdPtr->objClientData2 = as;
    if (as->applyCmdPtr->nreProc2)
    {
        as->applyCmdPtr->nreProc2 = CmdApplyShimNre;
    }

    Tcl_SetAssocData(ip, TBCX_APPLY_SHIM_KEY, ApplyShimCleanup, as);
    return as;
}

/* RegisterPrecompiledLambda — record a lambda in the ApplyShim registry
 * so that shimmer recovery can re-install the lambdaExpr internal rep. */
static void RegisterPrecompiledLambda(Tcl_Interp* ip, Tcl_Obj* lambda, Proc* procPtr, Tcl_Obj* nsObj)
{
    ApplyShim* as = EnsureApplyShim(ip);
    if (!as)
        return;

    /* Hold a reference on the lambda object so that if the ByteCode's
       literal pool releases it, the registry entry remains valid.
       Without this, the ONE_WORD_KEYS hash (keyed by Tcl_Obj pointer)
       could match a stale entry if the address is reused. */
    Tcl_IncrRefCount(lambda);

    int isNew = 0;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&as->lambdaRegistry, (const char*)lambda, &isNew);
    if (!isNew)
    {
        /* Lambda pointer reused (shouldn't happen, but be safe) */
        ApplyLambdaEntry* old = (ApplyLambdaEntry*)Tcl_GetHashValue(he);
        if (old)
        {
            if (old->procPtr && --old->procPtr->refCount <= 0)
            {
                TclProcCleanupProc(old->procPtr);
            }
            if (old->nsObj)
                Tcl_DecrRefCount(old->nsObj);
            Tcl_Free(old);
        }
        Tcl_DecrRefCount(lambda); /* drop extra ref from duplicate registration */
    }

    ApplyLambdaEntry* le = (ApplyLambdaEntry*)Tcl_Alloc(sizeof(ApplyLambdaEntry));
    le->procPtr = procPtr;
    le->procPtr->refCount++; /* registry holds its own reference */
    le->nsObj = nsObj;
    Tcl_IncrRefCount(le->nsObj);
    Tcl_SetHashValue(he, le);
}

int Tbcx_ReadHeader(TbcxIn* r, TbcxHeader* H)
{
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

    if (H->magic != TBCX_MAGIC || H->format != TBCX_FORMAT)
    {
        R_Error(r, "tbcx: bad header (unknown magic or format)");
        return 0;
    }
    {
        uint32_t rt = Tbcx_PackTclVersion();
        int hMaj = (int)((H->tcl_version >> 24) & 0xFFu);
        int hMin = (int)((H->tcl_version >> 16) & 0xFFu);
        int rMaj = (int)((rt >> 24) & 0xFFu);
        int rMin = (int)((rt >> 16) & 0xFFu);
        /* Require exact major.minor match.  Bytecode instruction semantics
           can change between minor versions (new opcodes, changed operand
           layouts, etc.) and the raw bytecode bytes are replayed verbatim.
           Allowing cross-minor loading could cause crashes or silent
           misbehavior.  We target 9.1 only. */
        if (hMaj != rMaj || hMin != rMin)
        {
            R_Error(r, "tbcx: incompatible Tcl version");
            return 0;
        }
    }
    return 1;
}

static int ReadProc(TbcxIn* r, Tcl_Interp* ip, ProcShim* shim, uint32_t procIdx)
{
    /* FQN, ns, args text */
    char* nameC = NULL;
    uint32_t nameL = 0;
    char* nsC = NULL;
    uint32_t nsL = 0;
    char* argsC = NULL;
    uint32_t argsL = 0;

    if (!Tbcx_R_LPString(r, &nameC, &nameL))
        return TCL_ERROR;
    if (!Tbcx_R_LPString(r, &nsC, &nsL))
    {
        Tcl_Free(nameC);
        return TCL_ERROR;
    }
    if (!Tbcx_R_LPString(r, &argsC, &argsL))
    {
        Tcl_Free(nameC);
        Tcl_Free(nsC);
        return TCL_ERROR;
    }

    Tcl_Obj* nameFqn = Tcl_NewStringObj(nameC, (Tcl_Size)nameL);
    Tcl_IncrRefCount(nameFqn);
    Tcl_Obj* nsObj = Tcl_NewStringObj(nsC, (Tcl_Size)nsL);
    Tcl_IncrRefCount(nsObj);
    Tcl_Obj* argsObj = Tcl_NewStringObj(argsC, (Tcl_Size)argsL);
    Tcl_IncrRefCount(argsObj);
    Tcl_Free(nameC);
    Tcl_Free(nsC);
    Tcl_Free(argsC);

    Namespace* nsPtr = (Namespace*)Tbcx_EnsureNamespace(ip, Tcl_GetString(nsObj));
    /* body block (+numLocals) */
    uint32_t nLoc = 0;
    Tcl_Obj* bodyBC = Tbcx_ReadBlock(r, ip, nsPtr, &nLoc, 1, 0);
    if (!bodyBC)
    {
        Tcl_DecrRefCount(nameFqn);
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(argsObj);
        return TCL_ERROR;
    }

    /* Build canonical FQN key from ns + name (unless name is already absolute) */
    Tcl_Obj* fqnKey = NULL;
    const char* nm = Tcl_GetString(nameFqn);
    if (nm[0] == ':' && nm[1] == ':')
    {
        fqnKey = nameFqn; /* alias — IncrRef below takes a second ref */
    }
    else
    {
        Tcl_Size nsLen = 0;
        const char* nsStr = Tcl_GetStringFromObj(nsObj, &nsLen);
        fqnKey = Tcl_NewStringObj(nsStr, nsLen);
        if (!(nsLen == 2 && nsStr[0] == ':' && nsStr[1] == ':'))
        {
            Tcl_AppendToObj(fqnKey, "::", 2);
        }
        Tcl_AppendObjToObj(fqnKey, nameFqn);
    }
    Tcl_IncrRefCount(fqnKey);
    /* nameFqn is consumed: fqnKey now owns its own reference (which may
       alias nameFqn in the absolute case).  Release the creation ref. */
    Tcl_DecrRefCount(nameFqn);
    nameFqn = NULL;

    /* Build Proc and procbody Tcl_Obj that refers to it */
    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr = bodyBC;
    Tcl_IncrRefCount(bodyBC);
    /* Build compiled locals/args consistent with argsObj */
    {
        CompiledLocal *first = NULL, *last = NULL;
        int numA = 0;
        if (Tbcx_BuildLocals(ip, argsObj, &first, &last, &numA) != TCL_OK)
        {
            Tcl_DecrRefCount(bodyBC);
            Tbcx_FreeLocals(procPtr->firstLocalPtr);
            Tcl_Free((char*)procPtr);
            Tcl_DecrRefCount(fqnKey);
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(argsObj);
            return TCL_ERROR;
        }
        procPtr->numArgs = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr = first;
        procPtr->lastLocalPtr = last;
    }
    CompiledLocals(procPtr, (int)nLoc);

    /* Link ByteCode back to this Proc and refresh epochs */
    {
        ByteCode* bcPtr = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bcPtr);
        if (bcPtr)
        {
            bcPtr->procPtr = procPtr;
            RefreshBC(bcPtr, ip, nsPtr);
        }
    }

    /* Create procbody Tcl_Obj and register under nameFqn */
    Tcl_Obj* procBodyObj = Tcl_NewObj();
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = procPtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
    procPtr->refCount++;

    /* Store PAIR {argsObj, procBodyObj} in shim registry (key & value refcounted) */
    Tcl_Obj* pair = Tcl_NewListObj(0, NULL);
    if (Tcl_ListObjAppendElement(ip, pair, argsObj) != TCL_OK || Tcl_ListObjAppendElement(ip, pair, procBodyObj) != TCL_OK)
    {
        Tcl_DecrRefCount(procBodyObj);
        return TCL_ERROR;
    }
    Tcl_IncrRefCount(pair);
    int isNew = 0;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&shim->procsByFqn, Tcl_GetString(fqnKey), &isNew);
    if (!isNew)
    {
        Tcl_Obj* oldPair = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (oldPair)
            Tcl_DecrRefCount(oldPair);
    }
    Tcl_SetHashValue(he, pair);
    /* Also store by index for marker-based lookup (Strategy A) */
    if (procIdx < shim->numProcsIdx && shim->procsByIdx)
    {
        shim->procsByIdx[procIdx] = pair;
        Tcl_IncrRefCount(pair); /* second ref: one for hash, one for array */
    }
    /* Clean temporaries — hash table copies the key string, does not hold the Tcl_Obj* */
    Tcl_DecrRefCount(fqnKey);
    Tcl_DecrRefCount(nsObj);
    Tcl_DecrRefCount(argsObj);
    return TCL_OK;
}

static void TopLocals_Begin(Tcl_Interp* ip, ByteCode* bcPtr, TbcxTopFrameSave* sv)
{
    Interp* iPtr = (Interp*)ip;

    /* The top-level bytecode is evaluated with TCL_EVAL_GLOBAL, which
       executes on iPtr->rootFramePtr.  We must set up compiled locals on
       that same frame — NOT iPtr->varFramePtr, which may differ when
       tbcx::load is called from non-global context (tcltest, uplevel, etc). */
    CallFrame* f = iPtr->rootFramePtr;

    memset(sv, 0, sizeof(*sv));
    if (!f)
        return; /* shouldn't happen, but be defensive */

    /* Save old state */
    sv->oldLocals = f->compiledLocals;
    sv->oldNum = f->numCompiledLocals;
    sv->oldCache = f->localCachePtr;

    /* Borrow the ByteCode's LocalCache and allocate a Var[] of matching size */
    Tcl_Size n = (bcPtr && bcPtr->localCachePtr) ? bcPtr->localCachePtr->numVars : 0;
    f->localCachePtr = bcPtr ? bcPtr->localCachePtr : NULL;
    if (f->localCachePtr)
    {
        f->localCachePtr->refCount++;
    }
    if (n <= 0)
    {
        f->compiledLocals = NULL;
        f->numCompiledLocals = 0;
        sv->allocated = NULL;
        return;
    }
    /* Allocate compiled locals and directly link each slot to the target Var
     * in the global namespace (no extra compilation, no upvar command call). */
    f->compiledLocals = (Var*)Tcl_Alloc(sizeof(Var) * (size_t)n);
    memset(f->compiledLocals, 0, sizeof(Var) * (size_t)n);
    f->numCompiledLocals = n;
    sv->allocated = f->compiledLocals;

    if (f->localCachePtr)
    {
        Tcl_Obj* const* names = (Tcl_Obj* const*)&f->localCachePtr->varName0; /* Tcl 9.x trailing array */
        Tcl_Namespace* gNs = Tcl_GetGlobalNamespace(ip);
        for (Tcl_Size i = 0; i < n; i++)
        {
            Tcl_Obj* nm = names ? names[i] : NULL;
            if (!nm)
                continue;
            Tcl_Size len = 0;
            const char* s = Tcl_GetStringFromObj(nm, &len);
            if (len == 0)
                continue; /* temp / unnamed */
            if (memchr(s, ':', (size_t)len))
                continue; /* skip qualified */
            /* Link to existing global variable if it exists.
               Do NOT create the variable — that would differ from source semantics
               where variables are only created on first assignment. */
            Tcl_Var vHandle = Tcl_FindNamespaceVar(ip, s, gNs, 0);
            if (vHandle)
            {
                Var* target = (Var*)vHandle; /* internal Var*, OK here */
                Var* dst = &f->compiledLocals[i];
                memset(dst, 0, sizeof(Var));
                dst->flags = VAR_LINK;
                dst->value.linkPtr = target;
            }
        }
    }
}

static void TopLocals_End(Tcl_Interp* ip, TbcxTopFrameSave* sv)
{
    Interp* iPtr = (Interp*)ip;
    /* Restore the same frame that TopLocals_Begin modified —
       iPtr->rootFramePtr, not iPtr->varFramePtr. */
    CallFrame* f = iPtr->rootFramePtr;
    if (!f)
        return; /* defensive */

    /* Drop what we lent to the frame */
    if (f->localCachePtr)
    {
        f->localCachePtr->refCount--;
        /* Do NOT free the cache here even if refCount reaches 0.
           The ByteCode's cleanup (TclCleanupByteCode) is responsible for
           freeing the LocalCache when the ByteCode is destroyed. Freeing
           it here would cause a use-after-free. */
    }
    if (sv->allocated)
        Tcl_Free((char*)sv->allocated);

    /* Restore previous state */
    f->compiledLocals = sv->oldLocals;
    f->numCompiledLocals = sv->oldNum;
    f->localCachePtr = sv->oldCache;
}

static int LoadTbcxStream(Tcl_Interp* ip, Tcl_Channel ch)
{
    TbcxIn r;
    Tbcx_R_Init(&r, ip, ch);
    TbcxHeader H;

    if (Tbcx_CheckBinaryChan(ip, ch) != TCL_OK)
        return TCL_ERROR;

    if (!Tbcx_ReadHeader(&r, &H) || r.err)
        return TCL_ERROR;

    Namespace* curNs = (Namespace*)Tbcx_EnsureNamespace(ip, "::");
    uint32_t dummyNL = 0;

    Tcl_Obj* topBC = Tbcx_ReadBlock(&r, ip, curNs, &dummyNL, 1, 0);
    if (!topBC)
        return TCL_ERROR;
    Tcl_IncrRefCount(topBC); /* protect against all early-return paths */

    int rc = TCL_ERROR;
    int shimInited = 0;   /* 1 once AddProcShim succeeded */
    int ooshimInited = 0; /* 1 once AddOOShim succeeded */

    ProcShim shim;
    memset(&shim, 0, sizeof(shim)); /* zero up front so DelProcShim is safe */
    OOShim ooshim;
    memset(&ooshim, 0, sizeof(ooshim));

    /* Procs */
    uint32_t numProcs = 0;
    if (!Tbcx_R_U32(&r, &numProcs))
        goto cleanup;
    if (numProcs > TBCX_MAX_PROCS)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numProcs %u exceeds limit %u", numProcs, TBCX_MAX_PROCS));
        goto cleanup;
    }

    /* Build proc shim registry and fill from section */
    if (numProcs)
    {
        if (AddProcShim(ip, &shim) != TCL_OK)
            goto cleanup;
        shimInited = 1;
        shim.procsByIdx = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * numProcs);
        memset(shim.procsByIdx, 0, sizeof(Tcl_Obj*) * numProcs);
        shim.numProcsIdx = numProcs;
    }

    for (uint32_t i = 0; i < numProcs; i++)
    {
        if (ReadProc(&r, ip, &shim, i) != TCL_OK)
            goto cleanup;
    }

    /* Classes section (saver currently emits 0) */
    uint32_t numClasses = 0;
    if (!Tbcx_R_U32(&r, &numClasses))
        goto cleanup;
    if (numClasses > TBCX_MAX_CLASSES)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numClasses %u exceeds limit %u", numClasses, TBCX_MAX_CLASSES));
        goto cleanup;
    }
    for (uint32_t c = 0; c < numClasses; c++)
    {
        /* classFqn + nSupers + supers… — saver writes 0; ignore here */
        char* cls = NULL;
        uint32_t cl = 0;
        if (!Tbcx_R_LPString(&r, &cls, &cl))
            goto cleanup;
        Tcl_Free(cls);
        uint32_t nSup = 0;
        if (!Tbcx_R_U32(&r, &nSup))
            goto cleanup;
        for (uint32_t s = 0; s < nSup; s++)
        {
            char* su = NULL;
            uint32_t sl = 0;
            if (!Tbcx_R_LPString(&r, &su, &sl))
                goto cleanup;
            Tcl_Free(su);
        }
    }
    uint32_t numMethods = 0;
    if (!Tbcx_R_U32(&r, &numMethods))
        goto cleanup;
    if (numMethods > TBCX_MAX_METHODS)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: numMethods %u exceeds limit %u", numMethods, TBCX_MAX_METHODS));
        goto cleanup;
    }
    if (numMethods)
    {
        if (AddOOShim(ip, &ooshim) != TCL_OK)
            goto cleanup;
        ooshimInited = 1;
    }
    for (uint32_t m = 0; m < numMethods; m++)
    {
        if (ReadMethod(&r, ip, &ooshim) != TCL_OK)
            goto cleanup;
    }

    /* Execute */
    {
        ByteCode* top = NULL;
        TbcxTopFrameSave _sv;

        ByteCodeGetInternalRep(topBC, tbcxTyBytecode, top);

        /* topBC is marked TCL_BYTECODE_PRECOMPILED (setPrecompiled=1 in
           Tbcx_ReadBlock above), which tells the Tcl bytecode engine to skip
           all compile/namespace epoch validity checks and execute the
           bytecode directly.  This is essential because AddProcShim and
           AddOOShim rename/create commands which bump the namespace
           resolverEpoch — without the precompiled flag, Tcl would detect
           stale epochs and try to recompile from the string rep (which is
           "" from Tcl_NewObj), silently losing all compiled code and
           crashing when procs are called. */

        TopLocals_Begin(ip, top, &_sv);

        rc = Tcl_EvalObjEx(ip, topBC, TCL_EVAL_GLOBAL);

        TopLocals_End(ip, &_sv);

        /* Handle TCL_RETURN the same way Tcl's 'source' command
           (Tcl_FSEvalFileEx) does. */
        if (rc == TCL_RETURN)
        {
            rc = TclUpdateReturnInfo((Interp*)ip);
        }
    }

cleanup:
    Tcl_DecrRefCount(topBC);
    if (ooshimInited)
        DelOOShim(ip, &ooshim);
    if (shimInited)
        DelProcShim(ip, &shim);
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

int Tbcx_LoadObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[])
{
    if (objc != 2)
    {
        Tcl_WrongNumArgs(interp, 1, objv, "in");
        return TCL_ERROR;
    }

    Tcl_Obj* inObj = objv[1];
    Tcl_Channel inCh = NULL;

    if (Tbcx_ProbeOpenChannel(interp, inObj, &inCh))
    {
        return LoadTbcxStream(interp, inCh);
    }

    if (Tbcx_ProbeReadableFile(interp, inObj))
    {
        Tcl_Channel ch = Tcl_FSOpenFileChannel(interp, inObj, "r", 0);
        if (!ch)
        {
            return TCL_ERROR;
        }
        int rc = LoadTbcxStream(interp, ch);
        if (Tcl_Close(interp, ch) != TCL_OK)
        {
            rc = TCL_ERROR;
        }
        return rc;
    }

    Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx::load: input is neither an open channel nor a readable file", -1));
    Tcl_SetErrorCode(interp, "TBCX", "LOAD", "BADINPUT", NULL);
    return TCL_ERROR;
}
