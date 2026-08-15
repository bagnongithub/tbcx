/* ==========================================================================
 * tbcxsave.c — TBCX compile+save for Tcl 9.1
 * ========================================================================== */

#include <errno.h>
#include "tbcx.h"

/* Process-local component of exclusive staging-file names.  Cross-process
 * safety comes from CREAT|EXCL below; this counter only reduces retries for
 * concurrent saves within one process. */
static uint64_t tbcxSaveTmpId = 0;
/* tbcxSaveTmpMutex: protects tbcxSaveTmpId.  Lock-order position: leaf —
 * no other TBCX mutex may be held while this is held. */
TCL_DECLARE_MUTEX(tbcxSaveTmpMutex);

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct TbcxCtx
{
    Tcl_Interp* interp;
    /* Instrumentation for runaway detection */
    uint64_t totalLiterals; /* total WriteLiteral calls */
    uint64_t totalBlocks;   /* total WriteCompiledBlock calls */
    int blockDepth;         /* current WriteCompiledBlock recursion depth */
    int maxBlockDepth;      /* high-water mark */
    int runaway;            /* set to 1 when a limit is hit */
    /* Save-time flags (TBCX_SAVE_FL_*).  Copied from Tbcx_SaveObjCmd so
     * that lower-level writers can consult them without threading the
     * flag set through every call site. */
    unsigned saveFlags;
    uint32_t artifactPolicy;
    Tcl_Size executableSourceFieldCount;
    Tcl_Size nonemptyExecutableSourceFieldCount;
    uint64_t executableSourceBytes;
    int unsupportedCrossInterp;
    int sourceRequired;
    /* Commands defined by captured [proc] records in this artifact.  A call
     * to one of these names is runtime generic dispatch, not evidence that a
     * same-named Tcl core command will interpret one of its arguments as a
     * script.  Keeping this table per save prevents false executable-role
     * classifications such as a user-defined [lmap] receiving ordinary list
     * data. */
    Tcl_HashTable authoredCommands; /* STRING_KEYS, canonical FQNs */
    int authoredCommandsInit;
    /* DEPTH OF `oo::define` / `oo::objdefine` BODIES CURRENTLY BEING WRITTEN.
     *
     * A command NAME does not determine which command will run: TclOO gives
     * `self` two unrelated meanings, and which one applies depends on the
     * namespace the body will be evaluated in.  In an `oo::define` body,
     * `::oo::define::self` takes a SCRIPT (`self { method x {} {…} }`).
     * Anywhere else — a method body, a proc, the top level —
     * `::oo::Helpers::self` takes a DATA WORD (`[self caller]`).  Classifying
     * the operand of the second as a script BLANKS it (a literal all of whose
     * uses are executable is written out as the empty string), so the release
     * artifact evaluates `self ""`.
     *
     * This used to be inferred from `bc->procPtr == NULL`, read as "not inside
     * a method".  That proxy is false for a NESTED script: a method body
     * carries a Proc, but the body of a `catch` / `if` / `while` / `for` /
     * `foreach` / `try` inside it is its own ByteCode with a NULL `procPtr`,
     * so every `[self caller]` written inside one was destroyed.  The context
     * is now PROPAGATED from the place that actually knows it — the writer of
     * the define body — instead of being guessed at the use site.
     *
     * A counter rather than a flag: `oo::define` bodies nest (a `self { … }`
     * block, or an `oo::define` inside one). */
    int defineBodyDepth;
    /* Original source file path when tbcx::save was given a filesystem
     * path (not an inline script or channel).  Written into the header
     * as an LPString so the loader can set iPtr->scriptFile to match
     * what `source` would have done.  NULL when no path is available. */
    Tcl_Obj* sourcePath;
} TbcxCtx;

typedef struct
{
    const char* key;
    int targetOffset;
} JTEntry;

typedef enum TbcxLiteralRole
{
    TBCX_LITERAL_DATA,
    TBCX_LITERAL_SCRIPT_SAME_INTERP,
    TBCX_LITERAL_SCRIPT_CROSS_INTERP,
    TBCX_LITERAL_LAMBDA,
    TBCX_LITERAL_SWITCH_LIST
} TbcxLiteralRole;

typedef struct TbcxLiteralOccurrence
{
    Tcl_Size literalIndex;
    Tcl_Size pushPc;
    TbcxLiteralRole role;
    Namespace* namespacePtr; /* borrowed for this save call */
    /* This occurrence is the BODY of an `oo::define` / `oo::objdefine`, i.e.
     * the one context in which the TclOO definition DSL is in scope.  The
     * writer raises `ctx->defineBodyDepth` around it so the scan of the nested
     * block can tell the DSL's `self { SCRIPT }` and `private { SCRIPT }` from
     * the same-named commands that mean something else everywhere else.  See
     * the comment on `defineBodyDepth`. */
    unsigned char defineBody;
} TbcxLiteralOccurrence;

typedef struct TbcxLiteralPlan
{
    TbcxLiteralOccurrence* occurrences;
    Tcl_Size count;
    Tcl_Size capacity;
    Tcl_Size executableCount;
    unsigned char* retainOriginal;
    unsigned char* hasExecutable;
    Tcl_Size numOriginal;
} TbcxLiteralPlan;

typedef struct
{
    Tcl_WideInt key;
    int targetOffset;
} JTNumEntry;

typedef struct
{
    Tcl_Obj* name; /* FQN or as-is */
    Tcl_Obj* ns;   /* NS for emission (string) */
    Tcl_Obj* args;
    Tcl_Obj* body;
    int kind; /* 0=proc; 1=inst-meth; 2=class-meth; 3=ctor; 4=dtor (methods stored in “methods” section) */
    Tcl_Obj* cls;
    int flags;   /* bitmask: 1 = captured from oo::class builder */
    int scope;   /* TBCX_MSCOPE_* — TclOO visibility the method must have
                    after load (declaration option + lexical private +
                    same-body export/unexport).  Unused for procs. */
    int emitted; /* set once EmitFlatSelfStubs has written this self
                    method's flat stub, so a class touched by several
                    commands does not double-emit (and re-bump the
                    epoch after a later self method's body swap). */
} DefRec;

typedef struct
{
    DefRec* v;
    Tcl_Size n, cap;
} DefVec;

typedef struct
{
    Tcl_HashTable ht; /* TCL_STRING_KEYS; key storage is owned by Tcl */
    int init;
} ClsSet;

#define DEF_F_FROM_BUILDER 0x01
#define DEF_F_SELF_METHOD 0x02 /* self method (class-level, emitted differently in stubs) */
#define DEF_F_OBJDEFINE                                                                                                          \
    0x04 /* captured from oo::objdefine (object-definition                                                                       \
          * origin) rather than oo::define/oo::class.  Drives                                                                    \
          * the per-record origin byte written to the methods                                                                    \
          * table, so the loader keys class-instance and                                                                         \
          * per-object methods distinctly (and they coexist).                                                                    \
          * Also gates the self-method-in-objdefine reject. */

/* Lexical-context bits threaded through CaptureClassBody recursion so a
 * method captured inside a `private { ... }` block becomes true-private and
 * one inside a `self { ... }` block becomes a self method. */
#define CAP_CTX_PRIVATE 0x01
#define CAP_CTX_SELF 0x02
#define DEF_KIND_PROC 0
#define DEF_KIND_INST 1
#define DEF_KIND_CLASS 2
#define DEF_KIND_CTOR 3
#define DEF_KIND_DTOR 4

/* Runaway detection limits.  If any is exceeded, serialization aborts
   with an error message identifying the trigger point. */
#define TBCX_MAX_LITERAL_CALLS (2u * 1024u * 1024u)  /* 2M WriteLiteral calls */
#define TBCX_MAX_BLOCK_CALLS (256u * 1024u)          /* 256K WriteCompiledBlock calls */
#define TBCX_MAX_BLOCK_DEPTH 64                      /* recursion depth */
#define TBCX_MAX_OUTPUT_BYTES (256u * 1024u * 1024u) /* 256 MB output */

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static Tcl_Obj* CanonTrivia(Tcl_Obj* in);
static void CaptureClassBody(Tcl_Interp* ip,
                             const char* script,
                             Tcl_Size len,
                             Tcl_Obj* curNs,
                             Tcl_Obj* clsFqn,
                             DefVec* defs,
                             ClsSet* classes,
                             int flags,
                             int depth,
                             int ctxFlags,
                             Tcl_Size foldStartIdx);
static Tcl_Obj* CaptureAndRewriteScript(
    Tcl_Interp* ip, const char* script, Tcl_Size len, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
static inline const char* CmdCore(const char* s);
static int CmpJTEntryUtf8_qsort(const void* pa, const void* pb);
static int CmpJTNumEntry_qsort(const void* pa, const void* pb);
static int CmpStrPtr_qsort(const void* pa, const void* pb);
static int CompileProcLike(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* nsFQN, Tcl_Obj* argsList, Tcl_Obj* bodyObj, const char* whereTag);
static int CompileProcForArtifact(Tcl_Interp* ip,
                                  Proc* procPtr,
                                  Tcl_Obj* bodyObj,
                                  Namespace* nsPtr,
                                  const char* description,
                                  const char* procName,
                                  uint32_t policy);
static int CompileScriptForArtifact(Tcl_Interp* ip, Tcl_Obj* objPtr, uint32_t policy);
static int CtxHasAuthoredCommand(TbcxCtx* ctx, ByteCode* bc, const char* commandName);
static uint32_t ComputeNumLocals(ByteCode* bc);
static void CS_Add(ClsSet* cs, Tcl_Obj* clsFqn);
static void CS_Free(ClsSet* cs);
static void CS_Init(ClsSet* cs);
static Tcl_Obj*
RecurseScriptBody(Tcl_Interp* ip, const Tcl_Token* bodyTok, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
static void DV_Free(DefVec* dv);
static void DV_Init(DefVec* dv);
static void DV_Push(DefVec* dv, DefRec r);
static int EmitTbcxStream(Tcl_Obj* scriptObj, TbcxOut* w, unsigned saveFlags, Tcl_Obj* sourcePath);
static Tcl_Obj* FqnUnder(Tcl_Interp* ip, Tcl_Obj* curNs, Tcl_Obj* name);
static int InstrIs(const char* name, const char* prefix);
static int IsPureOodefineBuilderBody(Tcl_Interp* ip, const char* script, Tcl_Size len);
static int IsPureObjdefineBuilderBody(Tcl_Interp* ip, const char* script, Tcl_Size len);
static int ObjdefineBuilderHasNonLiteralMethod(Tcl_Interp* ip, const char* script, Tcl_Size len, int depth);
static void Lit_Bignum(TbcxOut* w, Tcl_Obj* o);
static void Lit_Bytecode(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj);
static void Lit_Dict(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o);
static void Lit_LambdaBC(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* lambda);
static void Lit_List(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o);
static inline const Tcl_Token* NextWord(const Tcl_Token* wordTok);
static Tcl_Obj* NsFqn(Tcl_Namespace* nsPtr);
static int ReadAllFromChannel(Tcl_Interp* interp, Tcl_Channel ch, Tcl_Obj** outObjPtr);
static Tcl_Obj* ResolveToBytecodeObj(Tcl_Obj* cand);
static Tcl_Obj* StubbedBuilderBody(Tcl_Interp* ip, Tcl_Obj* bodyObj);
static void
StubLinesForClass(Tcl_Interp* ip, Tcl_DString* out, DefVec* defs, Tcl_Obj* clsFqn, const char* body, Tcl_Size bodyLen);
int Tbcx_SaveObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);
static inline void W_Bytes(TbcxOut* w, const void* p, size_t n);
static inline void W_Error(TbcxOut* w, const char* msg);
static inline void W_LPString(TbcxOut* w, const char* s, Tcl_Size n);
static inline void W_U32(TbcxOut* w, uint32_t v);
static inline void W_U64(TbcxOut* w, uint64_t v);
static inline void W_U8(TbcxOut* w, uint8_t v);
static Tcl_Obj* WordLiteralObj(const Tcl_Token* wordTok);
static void WriteAux_DictUpdate(TbcxOut* w, AuxData* ad);
static void WriteAux_Foreach(TbcxOut* w, AuxData* ad);
static void WriteAux_JTNum(TbcxOut* w, AuxData* ad);
static void WriteAux_JTStr(TbcxOut* w, AuxData* ad);
static void WriteCompiledBlock(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj, uint32_t blockPolicy);
static void WriteCompiledScriptLiteral(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* compiledObj, Tcl_Obj* authoredSource);
static void WriteExecutableSource(TbcxOut* w, TbcxCtx* ctx, TbcxExecSourceKind kind, const char* bytes, Tcl_Size length);
static void WriteHeaderTop(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* topObj);
static void WriteLiteral(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* obj);
static void WriteLocalNames(TbcxOut* w, ByteCode* bc, uint32_t numLocals);

/* ==========================================================================
 * Buffered Write I/O & Comparators
 * ========================================================================== */

static int CmpJTEntryUtf8_qsort(const void* pa, const void* pb)
{
    const JTEntry* a = (const JTEntry*)pa;
    const JTEntry* b = (const JTEntry*)pb;
    const char* sa = a->key ? a->key : "";
    const char* sb = b->key ? b->key : "";
    return strcmp(sa, sb);
}

static int CmpJTNumEntry_qsort(const void* pa, const void* pb)
{
    const JTNumEntry* a = (const JTNumEntry*)pa;
    const JTNumEntry* b = (const JTNumEntry*)pb;
    if (a->key < b->key)
        return -1;
    if (a->key > b->key)
        return 1;
    return 0;
}

static int CmpStrPtr_qsort(const void* pa, const void* pb)
{
    const char* a = *(const char* const*)pa;
    const char* b = *(const char* const*)pb;
    return strcmp(a ? a : "", b ? b : "");
}

static inline void W_Error(TbcxOut* w, const char* msg)
{
    if (w->err == TCL_OK)
    {
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj(msg, -1));
        w->err = TCL_ERROR;
    }
}

/* Buffered write I/O — collapses thousands of syscalls into a handful. */
void Tbcx_W_Init(TbcxOut* w, Tcl_Interp* ip, Tcl_Channel ch)
{
    w->interp = ip;
    w->chan = ch;
    w->err = TCL_OK;
    w->bufPos = 0;
    w->totalBytes = 0;
}

int Tbcx_W_Flush(TbcxOut* w)
{
    if (w->err || w->bufPos == 0)
        return w->err;
    Tcl_Size off = 0;
    while (off < w->bufPos)
    {
        Tcl_Size toWrite = w->bufPos - off;
        Tcl_Size got = Tcl_WriteRaw(w->chan, (const char*)w->buf + off, toWrite);
        if (got <= 0)
        {
            W_Error(w, "tbcx: short write");
            return w->err;
        }
        off += got;
    }
    w->totalBytes += w->bufPos;
    w->bufPos = 0;
    return w->err;
}

static inline void W_Bytes(TbcxOut* w, const void* p, size_t n)
{
    if (w->err)
        return;
    /* Hard output limit at entry */
    if (w->totalBytes + w->bufPos + n > TBCX_MAX_OUTPUT_BYTES)
    {
        W_Error(w, "tbcx: output too large");
        return;
    }
    const unsigned char* src = (const unsigned char*)p;
    size_t rem = n;
    while (rem > 0)
    {
        size_t avail = TBCX_BUFSIZE - w->bufPos;
        if (avail == 0)
        {
            Tbcx_W_Flush(w);
            if (w->err)
                return;
            /* Re-check after flush (totalBytes updated) */
            if (w->totalBytes > TBCX_MAX_OUTPUT_BYTES)
            {
                W_Error(w, "tbcx: output too large");
                return;
            }
            avail = TBCX_BUFSIZE;
        }
        size_t chunk = (rem < avail) ? rem : avail;
        memcpy(w->buf + w->bufPos, src, chunk);
        w->bufPos += chunk;
        src += chunk;
        rem -= chunk;
    }
}

static inline void W_U8(TbcxOut* w, uint8_t v)
{
    W_Bytes(w, &v, 1);
}

static inline void W_U32(TbcxOut* w, uint32_t v)
{
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed))
    {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    W_Bytes(w, &v, 4);
}
static inline void W_U64(TbcxOut* w, uint64_t v)
{
    if (!atomic_load_explicit(&tbcxHostIsLE, memory_order_relaxed))
    {
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) |
            ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
            ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }
    W_Bytes(w, &v, 8);
}

static inline void W_LPString(TbcxOut* w, const char* s, Tcl_Size n)
{
    if (n < 0)
        n = (Tcl_Size)strlen(s);
    if ((uint64_t)n > TBCX_MAX_STR)
    {
        W_Error(w, "tbcx: string too large");
        return;
    }
    W_U32(w, (uint32_t)n);
    W_Bytes(w, s, (size_t)n);
}

static int PolicyIncludesSource(const TbcxCtx* ctx)
{
    return ctx && (ctx->artifactPolicy & TBCX_BLOCK_POLICY_INCLUDE_SOURCE) != 0u;
}

/* The only serializer for designated executable-source fields.  Keeping all
 * four kinds on this path supplies the staged save's final accounting check
 * before commit. */
static void WriteExecutableSource(TbcxOut* w, TbcxCtx* ctx, TbcxExecSourceKind kind, const char* bytes, Tcl_Size length)
{
    (void)kind;
    if (!ctx || w->err)
        return;
    if (ctx->executableSourceFieldCount == TCL_SIZE_MAX)
    {
        W_Error(w, "tbcx: executable-source field count overflow");
        return;
    }
    ctx->executableSourceFieldCount++;

    if (!PolicyIncludesSource(ctx))
    {
        W_LPString(w, "", 0);
        return;
    }
    if (length < 0 || (uint64_t)length > UINT32_MAX || (uint64_t)length > TBCX_MAX_STR)
    {
        W_Error(w, "tbcx: executable source exceeds limit");
        return;
    }
    if (length > 0)
    {
        uint64_t add = (uint64_t)length;
        if (UINT64_MAX - ctx->executableSourceBytes < add)
        {
            W_Error(w, "tbcx: executable-source byte count overflow");
            return;
        }
        ctx->nonemptyExecutableSourceFieldCount++;
        ctx->executableSourceBytes += add;
    }
    W_LPString(w, bytes ? bytes : "", length);
}

/* NsFqn — Returns a Tcl_Obj* at refcount 1 (caller owns the reference and
 * must Tcl_DecrRefCount when done). */
static Tcl_Obj* NsFqn(Tcl_Namespace* nsPtr)
{
    Tcl_Obj* o = Tcl_NewStringObj(nsPtr ? nsPtr->fullName : "::", -1);
    Tcl_IncrRefCount(o);
    return o; /* refcount 1 */
}

static void Lit_Bignum(TbcxOut* w, Tcl_Obj* o)
{
    mp_int z;
    if (Tcl_GetBignumFromObj(NULL, o, &z) != TCL_OK)
    {
        W_Error(w, "tbcx: bad bignum");
        return;
    }
    mp_int mag;
    mp_err mrc;
    mrc = TclBN_mp_init(&mag);
    if (mrc != MP_OKAY)
    {
        W_Error(w, "tbcx: bignum init");
        TclBN_mp_clear(&z);
        return;
    }
    mrc = TclBN_mp_copy(&z, &mag);
    if (mrc != MP_OKAY)
    {
        W_Error(w, "tbcx: bignum copy");
        TclBN_mp_clear(&mag);
        TclBN_mp_clear(&z);
        return;
    }
    if (mp_isneg(&mag))
    {
        mrc = TclBN_mp_neg(&mag, &mag);
        if (mrc != MP_OKAY)
        {
            W_Error(w, "tbcx: bignum abs/neg");
            TclBN_mp_clear(&mag);
            TclBN_mp_clear(&z);
            return;
        }
    }
    /* Use TomMath's unsigned-binary (big-endian) export, then reverse to LE. */
    size_t be_bytes = TclBN_mp_ubin_size(&mag);
    unsigned char* be = NULL;
    if (be_bytes > 0)
    {
        /* Tcl 9.1's Tcl_Alloc panics on OOM; no NULL check needed. */
        be = (unsigned char*)Tcl_Alloc(be_bytes);
        mrc = TclBN_mp_to_ubin(&mag, be, be_bytes, NULL);
        if (mrc != MP_OKAY)
        {
            W_Error(w, "tbcx: bignum export");
            Tcl_Free((char*)be);
            TclBN_mp_clear(&mag);
            TclBN_mp_clear(&z);
            return;
        }
    }
    /* Strip leading zeros (TclBN_mp_ubin_size may be 1 for zero) */
    size_t firstNZ = 0;
    if (be_bytes == 0 || !be)
    {
        /* Zero magnitude — emit compact zero encoding */
        W_U8(w, 0);
        W_U32(w, 0);
        if (be)
            Tcl_Free((char*)be);
        TclBN_mp_clear(&mag);
        TclBN_mp_clear(&z);
        return;
    }
    while (firstNZ < be_bytes && be[firstNZ] == 0)
        firstNZ++;
    size_t magLen = be_bytes - firstNZ;
    if (magLen == 0)
    { /* zero */
        W_U8(w, 0);
        W_U32(w, 0);
    }
    else
    {
        if (magLen > (size_t)UINT32_MAX)
        {
            W_Error(w, "tbcx: bignum magnitude exceeds uint32_t");
            Tcl_Free((char*)be);
            TclBN_mp_clear(&mag);
            TclBN_mp_clear(&z);
            return;
        }
        int sign = mp_isneg(&z) ? 2 : 1;
        W_U8(w, (uint8_t)sign);
        W_U32(w, (uint32_t)magLen);
        /* write little-endian bytes */
        for (size_t i = 0; i < magLen; i++)
        {
            W_U8(w, be[be_bytes - 1 - i]);
        }
    }
    if (be)
        Tcl_Free((char*)be);
    TclBN_mp_clear(&mag);
    TclBN_mp_clear(&z);
}

static void Lit_List(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o)
{
    Tcl_Size n = 0;
    Tcl_Obj** v = NULL;
    if (Tcl_ListObjGetElements(NULL, o, &n, &v) != TCL_OK)
    {
        W_Error(w, "tbcx: list decode");
        return;
    }
    W_U32(w, (uint32_t)n);
    for (Tcl_Size i = 0; i < n; i++)
        WriteLiteral(w, ctx, v[i]);
}

typedef struct
{
    Tcl_Obj* key;
    Tcl_Obj* val;
} KVPair;

static void Lit_Dict(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o)
{
    Tcl_DictSearch s;
    int done = 0;
    Tcl_Obj *k, *v;
    Tcl_Size sz = 0;
    if (Tcl_DictObjSize(NULL, o, &sz) != TCL_OK)
    {
        W_Error(w, "tbcx: dict decode");
        return;
    }
    size_t nPairs = (size_t)(sz ? sz : 1);
    if (sz < 0 || nPairs > SIZE_MAX / sizeof(KVPair))
    {
        W_Error(w, "tbcx: dict too large");
        return;
    }
    KVPair* pairs = (KVPair*)Tcl_AttemptAlloc(sizeof(KVPair) * nPairs);
    if (!pairs)
    {
        W_Error(w, "tbcx: allocation failed (dict)");
        return;
    }
    Tcl_Size idx = 0;
    if (Tcl_DictObjFirst(NULL, o, &s, &k, &v, &done) != TCL_OK)
    {
        Tcl_Free((char*)pairs);
        W_Error(w, "tbcx: dict iter");
        return;
    }
    while (!done)
    {
        pairs[idx].key = k;
        pairs[idx].val = v;
        idx++;
        Tcl_DictObjNext(&s, &k, &v, &done);
    }
    /* Preserve insertion order — do NOT sort.  Sorting would alter
       dict iteration order, which is observable via [dict for] etc. */
    W_U32(w, (uint32_t)idx);
    for (Tcl_Size i = 0; i < idx; i++)
    {
        WriteLiteral(w, ctx, pairs[i].key);
        if (pairs[i].val)
        {
            WriteLiteral(w, ctx, pairs[i].val);
        }
        else
        {
            /* Defensive: should not happen, but handle safely */
            Tcl_Obj* empty = Tcl_NewObj();
            Tcl_IncrRefCount(empty);
            WriteLiteral(w, ctx, empty);
            Tcl_DecrRefCount(empty);
        }
    }
    Tcl_Free((char*)pairs);
}

static void Lit_LambdaBC(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* lambda)
{
    Tcl_Size L = 0;
    Tcl_Obj** E = NULL;
    if (Tcl_ListObjGetElements(ctx->interp, lambda, &L, &E) != TCL_OK || (L != 2 && L != 3))
    {
        W_Error(w, "tbcx: lambda must be a list of 2 or 3 elements");
        return;
    }
    Tcl_Obj* argsList = E[0];
    Tcl_Obj* bodyObj = E[1];
    Tcl_Obj* nsObjIn = (L == 3 ? E[2] : NULL);

    /* Resolve namespace (public): use provided FQN if present, else current */
    Tcl_Namespace* nsPtr = NULL;
    Tcl_Obj* nsFQN = NULL;
    if (nsObjIn)
    {
        Tcl_Obj* current = NsFqn(Tcl_GetCurrentNamespace(ctx->interp));
        nsFQN = FqnUnder(ctx->interp, current, nsObjIn);
        Tcl_DecrRefCount(current);
        if (nsFQN)
            nsPtr = Tbcx_EnsureNamespace(ctx->interp, Tbcx_GetStringSafe(nsFQN));
        if (!nsFQN || !nsPtr)
        {
            if (nsFQN)
                Tcl_DecrRefCount(nsFQN);
            W_Error(w, "tbcx: lambda namespace could not be resolved");
            return;
        }
    }
    else
    {
        nsPtr = Tcl_GetCurrentNamespace(ctx->interp);
        nsFQN = NsFqn(nsPtr);
    }
    int compiled_ok = 0;
    Tcl_Size nsLen = 0;
    const char* nsStr = Tbcx_GetStringFromObjSafe(nsFQN, &nsLen);
    W_LPString(w, nsStr, nsLen);

    /* Marshal args & defaults from the public args list */
    Tcl_Size argc = 0;
    Tcl_Obj** argv = NULL;
    if (Tcl_ListObjGetElements(ctx->interp, argsList, &argc, &argv) != TCL_OK)
    {
        Tcl_DecrRefCount(nsFQN);
        W_Error(w, "tbcx: bad lambda args list");
        return;
    }
    W_U32(w, (uint32_t)argc);
    for (Tcl_Size i = 0; i < argc; i++)
    {
        Tcl_Size nf = 0;
        Tcl_Obj** fv = NULL;
        if (Tcl_ListObjGetElements(ctx->interp, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2)
        {
            Tcl_DecrRefCount(nsFQN);
            W_Error(w, "tbcx: bad lambda arg spec");
            return;
        }
        Tcl_Size nmLen = 0;
        const char* nm = Tbcx_GetStringFromObjSafe(fv[0], &nmLen);
        W_LPString(w, nm, nmLen);
        if (nf == 2)
        {
            W_U8(w, 1);
            WriteLiteral(w, ctx, fv[1]);
        }
        else
        {
            W_U8(w, 0);
        }
    }

    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ctx->interp;
    procPtr->refCount = 1;
    procPtr->numArgs = argc;
    procPtr->numCompiledLocals = argc;
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size numA = 0;
        if (Tbcx_BuildLocals(ctx->interp, argsList, &first, &last, &numA) != TCL_OK)
        {
            W_Error(w, "tbcx: lambda args decode");
            goto lambda_cleanup;
        }
        procPtr->numArgs = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr = first;
        procPtr->lastLocalPtr = last;
    }

    Tcl_IncrRefCount(bodyObj);
    if (CompileProcForArtifact(
            ctx->interp, procPtr, bodyObj, (Namespace*)nsPtr, "body of lambda term", "lambdaExpr", ctx->artifactPolicy) != TCL_OK)
    {
        Tcl_DecrRefCount(bodyObj);
        /* Preserve the detailed compiler error from TclProcCompileProc
         * instead of overwriting with a generic message. */
        if (w->err == TCL_OK)
        {
            Tcl_Obj* detail = Tcl_GetObjResult(ctx->interp);
            Tcl_Size detLen = 0;
            const char* detStr = Tbcx_GetStringFromObjSafe(detail, &detLen);
            Tcl_SetObjResult(w->interp, Tcl_ObjPrintf("tbcx: lambda compile: %.*s", (int)(detLen > 200 ? 200 : detLen), detStr));
            w->err = TCL_ERROR;
        }
        goto lambda_cleanup;
    }
    compiled_ok = 1;
    {
        Tcl_Obj* cand = procPtr->bodyPtr ? procPtr->bodyPtr : bodyObj;
        Tcl_Obj* bcObj = ResolveToBytecodeObj(cand);
        if (!bcObj)
        {
            W_Error(w, "tbcx: proc-like compile produced no bytecode");
        }
        else
        {
            WriteCompiledBlock(w, ctx, bcObj, 0u);
        }
    }

    /* Lambda source follows the artifact-wide policy.  Strict mode writes
     * only the zero length; the loader creates a diagnostic representation
     * in memory while execution continues to use this compiled Proc. */
    {
        Tcl_Size bLen = 0;
        const char* bStr = "";
        if (PolicyIncludesSource(ctx))
        {
            bStr = Tbcx_GetStringFromObjStrict(ctx->interp, bodyObj, &bLen);
            if (!bStr)
            {
                w->err = TCL_ERROR;
                goto lambda_body_done;
            }
        }
        WriteExecutableSource(w, ctx, TBCX_EXEC_SOURCE_LAMBDA, bStr, bLen);
    }

lambda_body_done:

    Tcl_DecrRefCount(bodyObj);

lambda_cleanup:
    if (!compiled_ok)
    {
        /* Compilation failed: procPtr was never attached to a ByteCode.
         * We own procPtr and its locals — free them here. */
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char*)procPtr);
    }
    /* OWNERSHIP (compiled_ok == true):
     *   bodyObj holds the procbody/bytecode internal rep, which owns
     *   the Proc reference (via Proc.refCount).
     *   bc->procPtr is a non-owning backpointer used at execution time;
     *   ByteCode cleanup does NOT free the Proc.
     *   Proc lifetime ends through TclProcCleanupProc() when the owning
     *   object or command drops the last reference.
     *   bodyObj is on the Tcl stack, refcount held by caller (Tcl_IncrRefCount
     *   at line above), will be freed when that refcount drops.
     *
     *   If w->err was set after compiled_ok (e.g. by WriteCompiledBlock or
     *   W_LPString), the ByteCode Tcl_Obj is still alive — the write error
     *   does not affect object lifetimes.  The caller (WriteLiteral) will
     *   propagate the error and eventually the Tcl_Obj will be freed normally. */

    Tcl_DecrRefCount(nsFQN);
}

static void Lit_Bytecode(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj)
{
    ByteCode* codePtr = NULL;
    codePtr = TbcxGetByteCode(bcObj);
    Tcl_Obj* nsFQN = NsFqn((Tcl_Namespace*)(codePtr ? codePtr->nsPtr : NULL));
    Tcl_Size nsLen;
    const char* nsStr = Tbcx_GetStringFromObjSafe(nsFQN, &nsLen);
    W_LPString(w, nsStr, nsLen);
    WriteCompiledBlock(w, ctx, bcObj, 0u);
    Tcl_DecrRefCount(nsFQN);
}

static void WriteCompiledScriptLiteral(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* compiledObj, Tcl_Obj* authoredSource)
{
    Tcl_Size sourceLength = 0;
    const char* sourceBytes = "";

    W_U32(w, TBCX_LIT_BYTESRC);
    if (PolicyIncludesSource(ctx))
    {
        sourceBytes = Tbcx_GetStringFromObjStrict(ctx->interp, authoredSource, &sourceLength);
        if (!sourceBytes)
        {
            w->err = TCL_ERROR;
            return;
        }
    }
    WriteExecutableSource(w, ctx, TBCX_EXEC_SOURCE_BYTESRC, sourceBytes, sourceLength);
    Lit_Bytecode(w, ctx, compiledObj);
}

int Tbcx_BuildLocals(Tcl_Interp* ip, Tcl_Obj* argsList, CompiledLocal** firstOut, CompiledLocal** lastOut, Tcl_Size* numArgsOut)
{
    if (!firstOut || !lastOut || !numArgsOut)
        return TCL_ERROR;
    *firstOut = *lastOut = NULL;
    *numArgsOut = 0;

    Tcl_Size argc = 0;
    Tcl_Obj** argv = NULL;
    if (Tcl_ListObjGetElements(ip, argsList, &argc, &argv) != TCL_OK)
    {
        return TCL_ERROR;
    }

    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++)
    {
        Tcl_Size nf = 0, nmLen = 0;
        Tcl_Obj** fv = NULL;
        if (Tcl_ListObjGetElements(ip, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2)
        {
            Tbcx_FreeLocals(first);
            return TCL_ERROR;
        }
        const char* nm = Tbcx_GetStringFromObjSafe(fv[0], &nmLen);
        /* Checked allocation size: offsetof(CompiledLocal, name) + 1 + nmLen */
        size_t baseSize = offsetof(CompiledLocal, name) + 1u;
        size_t allocSize = baseSize + (size_t)nmLen;
        if (allocSize < baseSize) /* overflow wrap */
        {
            Tbcx_FreeLocals(first);
            if (ip)
                Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: argument name alloc overflow", -1));
            return TCL_ERROR;
        }
        if (allocSize < sizeof(CompiledLocal))
            allocSize = sizeof(CompiledLocal);
        CompiledLocal* cl = (CompiledLocal*)Tcl_Alloc(allocSize);
        memset(cl, 0, allocSize);
        cl->nameLength = nmLen;
        memcpy(cl->name, nm, (size_t)nmLen + 1);
        cl->frameIndex = i;
        cl->flags = VAR_ARGUMENT;
        if (nf == 2)
        {
            cl->defValuePtr = fv[1];
            Tcl_IncrRefCount(cl->defValuePtr);
        }
        if (i == argc - 1 && nmLen == 4 && memcmp(nm, "args", 4) == 0)
            cl->flags |= VAR_IS_ARGS;
        if (!first)
            first = last = cl;
        else
        {
            last->nextPtr = cl;
            last = cl;
        }
    }

    *firstOut = first;
    *lastOut = last;
    *numArgsOut = argc;
    return TCL_OK;
}

void Tbcx_FreeLocals(CompiledLocal* first)
{
    for (CompiledLocal* cl = first; cl;)
    {
        CompiledLocal* next = cl->nextPtr;
        if (cl->defValuePtr)
            Tcl_DecrRefCount(cl->defValuePtr);
        Tcl_Free((char*)cl);
        cl = next;
    }
}

static int CompileProcLike(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* nsFQN, Tcl_Obj* argsList, Tcl_Obj* bodyObj, const char* whereTag)
{
    Tcl_Interp* ip = ctx->interp;
    const char* nsNm = Tbcx_GetStringSafe(nsFQN);
    Tcl_Namespace* ns = Tcl_FindNamespace(ip, nsNm, NULL, 0);
    if (!ns)
        ns = Tcl_CreateNamespace(ip, nsNm, NULL, NULL);
    if (!ns)
    {
        W_Error(w, "tbcx: failed to create namespace for proc-like compilation");
        return TCL_ERROR;
    }
    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ip;
    procPtr->refCount = 1;
    {
        CompiledLocal *first = NULL, *last = NULL;
        Tcl_Size numA = 0;
        if (Tbcx_BuildLocals(ip, argsList, &first, &last, &numA) != TCL_OK)
        {
            W_Error(w, "tbcx: bad arg spec");
            goto cple_fail;
        }
        procPtr->numArgs = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr = first;
        procPtr->lastLocalPtr = last;
    }
    Tcl_IncrRefCount(bodyObj);
    if (CompileProcForArtifact(ip, procPtr, bodyObj, (Namespace*)ns, whereTag, "proc", ctx->artifactPolicy) != TCL_OK)
    {
        /* Preserve the detailed compiler diagnostic left in interp result
           by TclProcCompileProc, wrapping it with our context prefix. */
        Tcl_Obj* detail = Tcl_GetObjResult(ip);
        Tcl_IncrRefCount(detail);
        Tcl_DecrRefCount(bodyObj);
        if (w->err == TCL_OK)
        {
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: proc-like compile failed: %s", Tbcx_GetStringSafe(detail)));
            w->err = TCL_ERROR;
        }
        Tcl_DecrRefCount(detail);
        goto cple_fail;
    }
    /* Compiler may leave bytecode in procPtr->bodyPtr and/or convert bodyObj to 'procbody'. */
    {
        Tcl_Obj* cand = procPtr->bodyPtr ? procPtr->bodyPtr : bodyObj;
        Tcl_Obj* bcObj = ResolveToBytecodeObj(cand);
        if (!bcObj)
        {
            Tcl_DecrRefCount(bodyObj);
            W_Error(w, "tbcx: proc-like compile produced no bytecode");
            goto cple_fail;
        }
        WriteCompiledBlock(w, ctx, bcObj, 0u);
    }
    Tcl_DecrRefCount(bodyObj);
    /* The Tcl compiler sets bc->procPtr = procPtr as a non-owning
       backpointer.  The Proc's lifetime is managed by refCount — the
       procbody/lambda internal rep holds the owning reference, and
       TclProcCleanupProc is the cleanup path.  Do NOT free procPtr
       or its locals here. */
    return (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

cple_fail:
    Tbcx_FreeLocals(procPtr->firstLocalPtr);
    Tcl_Free((char*)procPtr);
    return TCL_ERROR;
}

static void WriteLocalNames(TbcxOut* w, ByteCode* bc, uint32_t numLocals)
{
    const Tcl_Size n = (Tcl_Size)numLocals;
    Tcl_Size i = 0;
    if (w->err)
        return;
    if (n == 0)
        return;
    if (bc && bc->procPtr && bc->procPtr->numCompiledLocals > 0)
    {
        /* overflow-safe multiply — n from numCompiledLocals */
        size_t tmpBytes;
        if (!tbcx_checked_mul((size_t)n, sizeof(Tcl_Obj*), &tmpBytes))
        {
            W_Error(w, "tbcx: local name array size overflow");
            return;
        }
        Tcl_Obj** tmp = (Tcl_Obj**)Tcl_Alloc(tmpBytes);
        memset(tmp, 0, tmpBytes);
        for (CompiledLocal* cl = bc->procPtr->firstLocalPtr; cl; cl = cl->nextPtr)
        {
            if (cl->frameIndex >= 0 && cl->frameIndex < n)
            {
                tmp[cl->frameIndex] = Tcl_NewStringObj(cl->name, (Tcl_Size)cl->nameLength);
                Tcl_IncrRefCount(tmp[cl->frameIndex]);
            }
        }
        for (i = 0; i < n; i++)
        {
            if (tmp[i])
            {
                Tcl_Size ln = 0;
                const char* s = Tbcx_GetStringFromObjSafe(tmp[i], &ln);
                W_LPString(w, s, ln);
                Tcl_DecrRefCount(tmp[i]);
            }
            else
            {
                W_LPString(w, "", 0);
            }
        }
        Tcl_Free((char*)tmp);
        return;
    }
    /* For top-level bytecode, the LocalCache is the authoritative source
       of variable names.  It is populated during TclSetByteCodeFromAny and
       contains the correct names even when bc->procPtr is NULL.
       Ensures variable names survive correctly for commands like
       scan, regexp, dict, and uplevel that reference locals by name. */
    if (bc && bc->localCachePtr && bc->localCachePtr->numVars > 0)
    {
        Tcl_Obj* const* names = (Tcl_Obj* const*)&bc->localCachePtr->varName0;
        Tcl_Size nVars = bc->localCachePtr->numVars;
        for (i = 0; i < n; i++)
        {
            if (i < nVars && names[i])
            {
                Tcl_Size ln = 0;
                const char* s = Tbcx_GetStringFromObjSafe(names[i], &ln);
                W_LPString(w, s, ln);
            }
            else
            {
                W_LPString(w, "", 0);
            }
        }
        return;
    }
    /* No procPtr and no LocalCache: emit empty names (keeps format consistent). */
    for (i = 0; i < n; i++)
    {
        W_LPString(w, "", 0);
    }
}

/* WriteLit_Untyped — value-only serialization for literals with no recognized
 * internal representation.  Executable classification happens exclusively
 * in the instruction occurrence scanner. */
static void WriteLit_Untyped(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* obj)
{
    /* Get the string rep once and reuse throughout — the pointer remains
     * valid as long as we don't modify the object (which we never do). */
    Tcl_Size n = 0;
    const char* s = Tbcx_GetStringFromObjSafe(obj, &n);

    /* Executable classification is occurrence-based; untyped values are data. */

    /* Integer fidelity: Tcl's compiler sometimes stores integer
       constants as untyped string literals (typePtr == NULL).
       Detect them and emit as WIDEINT/WIDEUINT to avoid
       string-to-int shimmer on first use after load.
       Probe a DUPLICATE to avoid mutating the live literal pool. */
    if (n >= 1 && n <= 20)
    {
        Tcl_Obj* probe = Tcl_NewStringObj(s, n);
        Tcl_IncrRefCount(probe);
        Tcl_WideInt wv = 0;
        if (Tcl_GetWideIntFromObj(NULL, probe, &wv) == TCL_OK)
        {
            Tcl_DecrRefCount(probe);
            /* Verify the string rep IS the canonical integer form.
               Reject non-canonical forms like "042", "+5", " 3 " to
               preserve exact string-rep semantics. */
            Tcl_Obj* canonObj = Tcl_ObjPrintf("%" TCL_LL_MODIFIER "d", (long long)wv);
            Tcl_IncrRefCount(canonObj);
            Tcl_Size cl = 0;
            const char* canon = Tbcx_GetStringFromObjSafe(canonObj, &cl);
            if (cl == n && memcmp(s, canon, (size_t)n) == 0)
            {
                Tcl_DecrRefCount(canonObj);
                if (wv >= 0)
                {
                    W_U32(w, TBCX_LIT_WIDEUINT);
                    W_U64(w, (uint64_t)wv);
                }
                else
                {
                    W_U32(w, TBCX_LIT_WIDEINT);
                    W_U64(w, (uint64_t)wv);
                }
                return;
            }
            Tcl_DecrRefCount(canonObj);
        }
        else
        {
            Tcl_DecrRefCount(probe);
        }
    }

    {
        int hasHighByte = 0;
        for (Tcl_Size i = 0; i < n && i < 4096; i++)
        {
            if ((unsigned char)s[i] >= 0x80)
            {
                hasHighByte = 1;
                break;
            }
        }
        if (hasHighByte)
        {
            /* Probe a copy as bytearray — succeeds iff all code points <= 255 */
            Tcl_Obj* probe = Tcl_DuplicateObj(obj);
            Tcl_IncrRefCount(probe);
            Tcl_Size bLen = 0;
            unsigned char* bytes = Tbcx_GetByteArrayFromObjSafe(probe, &bLen);
            if (bytes && bLen > 0)
            {
                W_U32(w, TBCX_LIT_BYTEARR);
                W_U32(w, (uint32_t)bLen);
                W_Bytes(w, bytes, (size_t)bLen);
                Tcl_DecrRefCount(probe);
                return;
            }
            Tcl_DecrRefCount(probe);
        }
    }

    W_U32(w, TBCX_LIT_STRING);
    W_LPString(w, s, n);
}
static void WriteLiteral(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* obj)
{
    /* Runaway detection */
    if (ctx)
    {
        ctx->totalLiterals++;
        if (ctx->runaway || w->err)
            return;
        uint64_t litStartBytes = w->totalBytes + w->bufPos;
        if (ctx->totalLiterals > TBCX_MAX_LITERAL_CALLS || litStartBytes > TBCX_MAX_OUTPUT_BYTES)
        {
            ctx->runaway = 1;
            W_Error(w, "tbcx: runaway serialization detected (too many literals or output too large)");
            return;
        }
    }

    const Tcl_ObjType* ty = obj->typePtr;
    /* Tcl 9.1 bignum, int, and boolean objects can share the "int" type.
     * Unified integers are classified by value and use wide-integer tags.
     * The BOOLEAN tag is used only when boolean has a distinct type pointer. */
    if (ty == tbcxTyBignum || ty == tbcxTyInt)
    {
        Tcl_WideInt wv = 0;
        if (Tcl_GetWideIntFromObj(NULL, obj, &wv) == TCL_OK)
        {
            if (wv >= 0)
            {
                W_U32(w, TBCX_LIT_WIDEUINT);
                W_U64(w, (uint64_t)wv);
            }
            else
            {
                W_U32(w, TBCX_LIT_WIDEINT);
                W_U64(w, (uint64_t)wv);
            }
        }
        else
        {
            /* True bignum — doesn't fit in 64 bits */
            W_U32(w, TBCX_LIT_BIGNUM);
            Lit_Bignum(w, obj);
        }
    }
    else if (tbcxTyBoolean && tbcxTyBoolean != tbcxTyInt && ty == tbcxTyBoolean)
    {
        /* Boolean branch only when boolean has a distinct typePtr
           from int (pre-9.1 or future Tcl builds).  When they
           share a typePtr, integers are handled above via wideint. */
        int b = 0;
        if (Tcl_GetBooleanFromObj(NULL, obj, &b) != TCL_OK)
            b = 0; /* defensive: should not happen given type check above */
        W_U32(w, TBCX_LIT_BOOLEAN);
        W_U8(w, (uint8_t)(b != 0));
    }
    else if (ty == tbcxTyByteArray)
    {
        Tcl_Size n = 0;
        unsigned char* p = Tbcx_GetByteArrayFromObjSafe(obj, &n);
        if (n > 0 && !p)
        {
            W_Error(w, "tbcx: bytearray conversion returned NULL");
            return;
        }
        W_U32(w, TBCX_LIT_BYTEARR);
        W_U32(w, (uint32_t)n);
        if (n > 0)
            W_Bytes(w, p, (size_t)n);
    }
    else if (ty == tbcxTyDict)
    {
        /* Empty dicts (from Tcl_NewObj() shimmer) emit as empty string
           to avoid unnecessary dict alloc on load. */
        Tcl_Size dsz = 0;
        if (Tcl_DictObjSize(NULL, obj, &dsz) == TCL_OK && dsz == 0)
        {
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
        else
        {
            W_U32(w, TBCX_LIT_DICT);
            Lit_Dict(w, ctx, obj);
        }
    }
    else if (ty == tbcxTyDouble)
    {
        double d = 0;
        if (Tcl_GetDoubleFromObj(NULL, obj, &d) != TCL_OK)
        {
            W_Error(w, "tbcx: double literal conversion failed");
            return;
        }
        union
        {
            double d;
            uint64_t u;
        } u;
        u.d = d;
        W_U32(w, TBCX_LIT_DOUBLE);
        W_U64(w, u.u);
    }
    else if (ty == tbcxTyList)
    {
        /* Guard against a scalar value carrying a parasitic list intrep.
           Every Tcl string is a valid list, so a plain literal such as
           "#aa" (or " aa", "{x}") can have typePtr == tbcxTyList while its
           true value is the scalar string.  Serializing it element-wise
           re-quotes it as a 1-element list ("#aa" -> "{#aa}") and the
           corruption compounds on every round-trip ("#aa" -> "{{#aa}}").
           Element-wise serialization is sound ONLY when the element
           decomposition reproduces the object's exact string value; when
           it does not, serialize by value (string) instead — that path is
           byte-faithful and the loaded scalar is still a usable list. */
        {
            Tcl_Size nChk = 0;
            Tcl_Obj** vChk = NULL;
            int faithful = 0;
            if (Tcl_ListObjGetElements(NULL, obj, &nChk, &vChk) == TCL_OK)
            {
                Tcl_Obj* rebuilt = Tcl_NewListObj(nChk, vChk);
                Tcl_IncrRefCount(rebuilt);
                Tcl_Size oL = 0, rL = 0;
                const char* oS = Tbcx_GetStringFromObjSafe(obj, &oL);
                const char* rS = Tbcx_GetStringFromObjSafe(rebuilt, &rL);
                faithful = (oL == rL && (oL == 0 || memcmp(oS, rS, (size_t)oL) == 0));
                Tcl_DecrRefCount(rebuilt);
            }
            if (!faithful)
            {
                WriteLit_Untyped(w, ctx, obj);
                return;
            }
        }
        W_U32(w, TBCX_LIT_LIST);
        Lit_List(w, ctx, obj);
    }
    else if (ty == tbcxTyBytecode || ty == tbcxTyProcBody || (tbcxTyLambda != NULL && ty == tbcxTyLambda))
    {
        /* Internal reps do not make a value executable.  Only an exact
         * evaluator occurrence selected by InstrScanBodyLiterals is emitted
         * as tag 9/10; all other uses preserve their data string. */
        WriteLit_Untyped(w, ctx, obj);
    }
    else
    {
        WriteLit_Untyped(w, ctx, obj);
    }
}

static void WriteAux_JTStr(TbcxOut* w, AuxData* ad)
{
    JumptableInfo* info = (JumptableInfo*)ad->clientData;
    if (!info)
    {
        W_U32(w, 0);
        return;
    }
    Tcl_HashSearch srch;
    Tcl_HashEntry* h;
    uint32_t cnt = 0;

    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
        cnt++;
    if (cnt == 0)
    {
        W_U32(w, 0);
        return;
    }
    size_t arrBytes = 0;
    if (!tbcx_checked_mul(sizeof(JTEntry), (size_t)cnt, &arrBytes))
    {
        W_Error(w, "tbcx: jumptable string aux too large");
        return;
    }
    JTEntry* arr = (JTEntry*)Tcl_Alloc(arrBytes);
    uint32_t i = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
    {
        arr[i].key = (const char*)Tcl_GetHashKey(&info->hashTable, h);
        arr[i].targetOffset = PTR2INT(Tcl_GetHashValue(h));
        i++;
    }
    if (cnt > 1)
    {
        qsort(arr, (size_t)cnt, sizeof(JTEntry), CmpJTEntryUtf8_qsort);
    }
    W_U32(w, cnt);
    for (uint32_t k = 0; k < cnt; k++)
    {
        const char* s = arr[k].key ? arr[k].key : "";
        W_LPString(w, s, (Tcl_Size)strlen(s));
        W_U32(w, (uint32_t)arr[k].targetOffset);
    }
    Tcl_Free((char*)arr);
}

static void WriteAux_JTNum(TbcxOut* w, AuxData* ad)
{
    JumptableNumInfo* info = (JumptableNumInfo*)ad->clientData;
    if (!info)
    {
        W_U32(w, 0);
        return;
    }
    Tcl_HashSearch srch;
    Tcl_HashEntry* h;
    uint32_t cnt = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
        cnt++;
    if (cnt == 0)
    {
        W_U32(w, 0);
        return;
    }
    size_t numBytes = 0;
    if (!tbcx_checked_mul(sizeof(JTNumEntry), (size_t)cnt, &numBytes))
    {
        W_Error(w, "tbcx: jumptable numeric aux too large");
        return;
    }
    JTNumEntry* arr = (JTNumEntry*)Tcl_Alloc(numBytes);
    uint32_t i = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
    {
        arr[i].key = (Tcl_WideInt)(intptr_t)Tcl_GetHashKey(&info->hashTable, h);
        arr[i].targetOffset = PTR2INT(Tcl_GetHashValue(h));
        i++;
    }
    if (cnt > 1)
    {
        qsort(arr, (size_t)cnt, sizeof(JTNumEntry), CmpJTNumEntry_qsort);
    }
    W_U32(w, cnt);
    for (uint32_t k = 0; k < cnt; k++)
    {
        W_U64(w, (uint64_t)arr[k].key);
        W_U32(w, (uint32_t)arr[k].targetOffset);
    }
    Tcl_Free((char*)arr);
}

static void WriteAux_DictUpdate(TbcxOut* w, AuxData* ad)
{
    DictUpdateInfo* info = (DictUpdateInfo*)ad->clientData;
    if (!info)
    {
        W_U32(w, 0);
        return;
    }
    W_U32(w, (uint32_t)info->length);
    for (Tcl_Size i = 0; i < info->length; i++)
        W_U32(w, (uint32_t)info->varIndices[i]);
}

static void WriteAux_Foreach(TbcxOut* w, AuxData* ad)
{
    ForeachInfo* info = (ForeachInfo*)ad->clientData;
    Tcl_Size numLists = info ? info->numLists : 0;
    W_U32(w, (uint32_t)numLists);
    /* Tcl 9.1's newForeachInfoType repurposes loopCtTemp as a signed
       bytecode jump and does not initialize/use firstValueTemp.  Do not
       serialize that indeterminate field; canonicalize it to zero. */
    W_U32(w, (uint32_t)(info ? info->loopCtTemp : 0));
    W_U32(w, 0);
    W_U32(w, (uint32_t)numLists); /* intentional duplicate — loader validates match */
    for (Tcl_Size i = 0; i < numLists; i++)
    {
        ForeachVarList* vl = info->varLists[i];
        Tcl_Size nv = vl ? vl->numVars : 0;
        W_U32(w, (uint32_t)nv);
        for (Tcl_Size j = 0; j < nv; j++)
        {
            W_U32(w, (uint32_t)vl->varIndexes[j]);
        }
    }
}

static uint32_t ComputeNumLocals(ByteCode* bc)
{
    if (!bc)
        return 0;
    /* For top-level scripts, procPtr is NULL but localCachePtr may
       have the correct count from TclSetByteCodeFromAny.  Check it first.
       The LocalCache is authoritative — it is populated by the Tcl compiler
       for all locals including those introduced by scan, regexp, lassign, etc. */
    if (bc->localCachePtr && bc->localCachePtr->numVars > 0)
    {
        return (uint32_t)bc->localCachePtr->numVars;
    }
    /* No LocalCache (or empty) — fall through to AuxData scan regardless
       of whether procPtr is present. */
    /* Defensive: tolerate missing aux array even when the count is non-zero. */
    if (bc->numAuxDataItems <= 0 || bc->auxDataArrayPtr == NULL)
    {
        return 0;
    }
    Tcl_Size maxIdx = -1;
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; i++)
    {
        AuxData* ad = &bc->auxDataArrayPtr[i];
        if (!ad || !ad->type)
            continue; /* extra safety */
        if (ad->type == tbcxAuxDictUpdate)
        {
            DictUpdateInfo* info = (DictUpdateInfo*)ad->clientData;
            if (!info)
                continue;
            for (Tcl_Size k = 0; k < info->length; k++)
            {
                Tcl_Size v = info->varIndices[k];
                if (v > maxIdx)
                    maxIdx = v;
            }
        }
        else if (ad->type == tbcxAuxNewForeach)
        {
            ForeachInfo* info = (ForeachInfo*)ad->clientData;
            if (!info)
                continue;
            /* In Tcl 9.1 newForeachInfoType, firstValueTemp is unused and
               loopCtTemp is a signed jump distance, not a local index. */
            for (Tcl_Size l = 0; l < info->numLists; l++)
            {
                ForeachVarList* vl = info->varLists[l];
                if (!vl)
                    continue;
                for (Tcl_Size j = 0; j < vl->numVars; j++)
                {
                    Tcl_Size v = (Tcl_Size)vl->varIndexes[j];
                    if (v > maxIdx)
                        maxIdx = v;
                }
            }
        }
        else
        {
            /* other AuxData kinds don't contribute to locals */
        }
    }
    if (maxIdx < 0)
        return 0;
    return (uint32_t)(maxIdx + 1u);
}

static Tcl_Obj* ResolveToBytecodeObj(Tcl_Obj* cand)
{
    if (!cand)
        return NULL;
    ByteCode* bc = NULL;
    bc = TbcxGetByteCode(cand);
    if (bc)
        return cand;
    if (cand->typePtr == tbcxTyProcBody)
    {
        const Tcl_ObjInternalRep* irPtr = Tcl_FetchInternalRep(cand, tbcxTyProcBody);
        Proc* p = irPtr ? (Proc*)irPtr->twoPtrValue.ptr1 : NULL;
        if (p && p->bodyPtr)
        {
            ByteCode* bc2 = NULL;
            bc2 = TbcxGetByteCode(p->bodyPtr);
            if (bc2)
                return p->bodyPtr;
        }
    }
    return NULL;
}

/* A loaded block is marked TCL_BYTECODE_PRECOMPILED and has no retained
 * source/command map from which Tcl could recompile it.  Tcl's normal epoch
 * guard consequently accepts that immutable block after an earlier generic
 * command changes a compileProc command.  Preserve Tcl's normal optimized
 * bytecode when it is self-contained; when a block mixes an epoch-capable
 * operation with compiler-specific command instructions, rebuild that block
 * using generic command dispatch.  Generic dispatch performs command lookup
 * at execution time and carries its own command/limit accounting.
 *
 * The hazard list mirrors IsCompactibleCompileEnv() in Tcl 9.1.  Presence of
 * START_CMD tells us that compiler-specific instructions survived Tcl's own
 * safe-compaction pass. */
static int ByteCodeNeedsGenericCommands(ByteCode* bc)
{
    if (!bc || !bc->codeStart || bc->numCodeBytes <= 0)
        return 0;

    const InstructionDesc* table = (const InstructionDesc*)TclGetInstructionTable();
    int hasStart = 0, hasHazard = 0;

    for (Tcl_Size pc = 0; pc < bc->numCodeBytes;)
    {
        unsigned opcode = bc->codeStart[pc];
        if (opcode > LAST_INST_OPCODE || table[opcode].numBytes <= 0)
            return 1; /* compiler output should never reach this path */
        const char* name = table[opcode].name;
        if (name)
        {
            if (strcmp(name, "startCommand") == 0)
            {
                hasStart = 1;
            }
            else if (InstrIs(name, "invokeStk") || strcmp(name, "invokeExpanded") == 0 || strcmp(name, "invokeReplace") == 0 ||
                     strcmp(name, "evalStk") == 0 || strcmp(name, "exprStk") == 0 || strcmp(name, "yield") == 0 ||
                     strcmp(name, "yieldToInvoke") == 0 || strcmp(name, "upvar") == 0 || strcmp(name, "nsupvar") == 0 ||
                     strcmp(name, "variable") == 0)
            {
                hasHazard = 1;
            }
        }
        if (hasStart && hasHazard)
            return 1;
        pc += table[opcode].numBytes;
    }
    return 0;
}

typedef struct ArtifactCompileLimitState
{
    int active;
    int exceeded;
    int granularityTicker;
    Tcl_Size cmdCount;
    int forcedCommandLimit;
} ArtifactCompileLimitState;

static void EnableArtifactCompileChecks(Interp* iPtr, ArtifactCompileLimitState* state)
{
    state->active = iPtr->limit.active;
    state->exceeded = iPtr->limit.exceeded;
    state->granularityTicker = iPtr->limit.granularityTicker;
    state->cmdCount = iPtr->limit.cmdCount;
    state->forcedCommandLimit = (iPtr->limit.active & (TCL_LIMIT_COMMANDS | TCL_LIMIT_TIME)) == 0;

    if (state->forcedCommandLimit)
    {
        /* Merely setting the active bit leaves a fresh interpreter's command
           ceiling at zero.  Constant folding can execute Tcl commands while
           compiling and would then compile a synthetic LIMIT error.  Give
           the temporary limit an unreachable ceiling instead. */
        iPtr->limit.active |= TCL_LIMIT_COMMANDS;
        iPtr->limit.cmdCount = TCL_SIZE_MAX;
        iPtr->limit.exceeded &= ~TCL_LIMIT_COMMANDS;
    }
}

static void RestoreArtifactCompileChecks(Interp* iPtr, const ArtifactCompileLimitState* state)
{
    iPtr->limit.active = state->active;
    if (state->forcedCommandLimit)
    {
        iPtr->limit.exceeded = state->exceeded;
        iPtr->limit.granularityTicker = state->granularityTicker;
        iPtr->limit.cmdCount = state->cmdCount;
    }
}

static int CompileScriptForArtifact(Tcl_Interp* ip, Tcl_Obj* objPtr, uint32_t policy)
{
    Interp* iPtr = (Interp*)ip;
    ArtifactCompileLimitState limitState;
    int hadNoInline;
    int rc = TCL_ERROR;
    CallFrame* compileFrame = iPtr->varFramePtr;
    LocalCache* savedLocalCache = compileFrame ? compileFrame->localCachePtr : NULL;
    Proc* savedCompiledProc = iPtr->compiledProcPtr;

    /* Compile as if command limits are enabled so Tcl retains START_CMD even
       when the saver itself has no active limit.  The artifact may later run
       in an interpreter with command or time limits, and immutable bytecode
       cannot be recompiled to add those readiness checks at load time. */
    EnableArtifactCompileChecks(iPtr, &limitState);
    hadNoInline = (iPtr->flags & DONT_COMPILE_CMDS_INLINE) != 0;
    iPtr->compiledProcPtr = NULL;
    if (compileFrame)
        compileFrame->localCachePtr = NULL;
    if ((policy & TBCX_BLOCK_POLICY_MASK) != 0u)
        iPtr->flags |= DONT_COMPILE_CMDS_INLINE;

    rc = TclSetByteCodeFromAny(ip, objPtr, NULL, NULL);
    if (rc != TCL_OK)
        goto compile_script_done;

    /* Every new-policy compilation started with generic dispatch enabled.
     * Include-source and strict artifacts deliberately share execution
     * semantics; retained source is introspection data only. */
    if ((policy & TBCX_BLOCK_POLICY_MASK) != 0u)
    {
        rc = TCL_OK;
        goto compile_script_done;
    }

    ByteCode* bc = TbcxGetByteCode(objPtr);
    if (!ByteCodeNeedsGenericCommands(bc))
    {
        rc = TCL_OK;
        goto compile_script_done;
    }

    Tcl_StoreInternalRep(objPtr, tbcxTyBytecode, NULL);
    iPtr->flags |= DONT_COMPILE_CMDS_INLINE;
    rc = TclSetByteCodeFromAny(ip, objPtr, NULL, NULL);

compile_script_done:
    if (!hadNoInline)
        iPtr->flags &= ~DONT_COMPILE_CMDS_INLINE;
    if (compileFrame)
        compileFrame->localCachePtr = savedLocalCache;
    iPtr->compiledProcPtr = savedCompiledProc;
    RestoreArtifactCompileChecks(iPtr, &limitState);
    return rc;
}

static int CompileProcForArtifact(Tcl_Interp* ip,
                                  Proc* procPtr,
                                  Tcl_Obj* bodyObj,
                                  Namespace* nsPtr,
                                  const char* description,
                                  const char* procName,
                                  uint32_t policy)
{
    Interp* iPtr = (Interp*)ip;
    ArtifactCompileLimitState limitState;
    int hadNoInline;
    int rc;

    EnableArtifactCompileChecks(iPtr, &limitState);
    hadNoInline = (iPtr->flags & DONT_COMPILE_CMDS_INLINE) != 0;
    if ((policy & TBCX_BLOCK_POLICY_MASK) != 0u)
        iPtr->flags |= DONT_COMPILE_CMDS_INLINE;

    rc = TclProcCompileProc(ip, procPtr, bodyObj, nsPtr, description, procName);
    if (!hadNoInline)
        iPtr->flags &= ~DONT_COMPILE_CMDS_INLINE;
    if (rc != TCL_OK)
    {
        RestoreArtifactCompileChecks(iPtr, &limitState);
        return rc;
    }

    if ((policy & TBCX_BLOCK_POLICY_MASK) != 0u)
    {
        RestoreArtifactCompileChecks(iPtr, &limitState);
        return TCL_OK;
    }

    Tcl_Obj* bcObj = ResolveToBytecodeObj(procPtr->bodyPtr ? procPtr->bodyPtr : bodyObj);
    ByteCode* bc = bcObj ? TbcxGetByteCode(bcObj) : NULL;
    if (!ByteCodeNeedsGenericCommands(bc))
    {
        RestoreArtifactCompileChecks(iPtr, &limitState);
        return TCL_OK;
    }

    Tcl_StoreInternalRep(bodyObj, tbcxTyBytecode, NULL);
    int alreadyGeneric = (iPtr->flags & DONT_COMPILE_CMDS_INLINE) != 0;
    iPtr->flags |= DONT_COMPILE_CMDS_INLINE;
    rc = TclProcCompileProc(ip, procPtr, bodyObj, nsPtr, description, procName);
    if (!alreadyGeneric)
        iPtr->flags &= ~DONT_COMPILE_CMDS_INLINE;
    RestoreArtifactCompileChecks(iPtr, &limitState);
    return rc;
}

/* ==========================================================================
 * Walks the bytecode instruction stream of a compiled block, looking for
 * the pattern:  push cmd; push arg0; ... push argN; invokeStk (N+1)
 * If `cmd` is a body-evaluating command (foreach, while, for, try, etc.),
 * the exact body-argument PUSH is assigned an executable role in the block's
 * occurrence plan. Serialization then emits a fresh compiled literal record
 * and rewrites only that instruction operand.
 *
 * Uses a forward-pass stack model: each stack slot records either a
 * occurrence index (>=0) or -1 (non-literal / runtime value). Unknown opcode
 * effects reset the stack conservatively.
 * ========================================================================== */

/* Body-argument position rules for eval-like commands.
   bodyPos: 0 = last argument, N>0 = Nth arg (1-based from first non-cmd arg).
   For "dict": subcmd-aware, handled specially. */
/* defineOnly: this name takes a script ONLY inside an `oo::define` /
   `oo::objdefine` body.  Such a rule is applied only when the writer has
   raised `ctx->defineBodyDepth`, because the same word in any other context
   belongs to a different command whose operand is DATA — and a wrong
   classification blanks that operand rather than merely mislabelling it. */
typedef struct
{
    const char* cmd;
    int bodyPos;
    int defineOnly;
} BodyCmdRule;
static const BodyCmdRule sBodyCmds[] = {{"foreach", 0, 0},  /* last arg */
                                        {"lmap", 0, 0},     /* last arg */
                                        {"for", 0, 0},      /* last arg (4th of 4) */
                                        {"while", 0, 0},    /* last arg (2nd of 2) */
                                        {"try", 1, 0},      /* first arg */
                                        {"catch", 1, 0},    /* first arg */
                                        {"eval", 1, 0},     /* first arg */
                                        {"uplevel", 0, 0},  /* last arg */
                                        {"time", 1, 0},     /* first arg */
                                        {"timerate", 1, 0}, /* first arg */
                                        {"private", 1, 1},  /* TclOO builder DSL */
                                        {NULL, 0, 0}};

/* "dict for/map/update/with" have body as last arg, but the command name
   is "dict" with subcommand as the second pushed value. */
static int IsDictBodySubcmd(const char* sub)
{
    return (strcmp(sub, "for") == 0 || strcmp(sub, "map") == 0 || strcmp(sub, "update") == 0 || strcmp(sub, "with") == 0);
}

/* Check if instruction name starts with given prefix. */
static int InstrIs(const char* name, const char* prefix)
{
    while (*prefix)
    {
        if (*name != *prefix)
            return 0;
        name++;
        prefix++;
    }
    return 1;
}

/* Opcode handler categories for InstrScanBodyLiterals stack model.
   Declared at file scope so the static opMap cache can reference them. */
enum
{
    OP_UNKNOWN = 0,
    OP_PUSH,         /* push1/push4 */
    OP_INVOKE_STK,   /* invokeStk1/invokeStk4 */
    OP_NOP,          /* startCommand, nop, beginCatch, endCatch, verifyDict, foreach_*, dict update, expandStart, unsetScalar */
    OP_POP,          /* pop, lmap_collect */
    OP_PUSH1_NOPOP,  /* loadScalar, dup, over, nsupvar, upvar, variable, currentNamespace, tclooClass, infoLevelNumber,
                        pushResult, pushReturnCode, pushReturnOpts, expandStkTop */
    OP_LOAD_STK,     /* loadStk/loadArray: pop 1, push 1 */
    OP_LOAD_ARR_STK, /* loadArrayStk: pop 2, push 1 */
    OP_STORE_LVT,    /* storeScalar: clobber TOS */
    OP_STORE_STK,    /* storeStk/storeArray: pop 2, push 1 */
    OP_STORE_ARR_STK,   /* storeArrayStk: pop 3, push 1 */
    OP_SWAP,            /* swap: exchange top 2 */
    OP_INCR_STK_IMM,    /* incrStkImm, existStk: pop 1, push 1 */
    OP_INCR_STK,        /* incrStk, lappendStk, appendStk: pop 2, push 1 */
    OP_LAPPEND_LST_STK, /* lappendListStk: pop 2, push 1 */
    OP_UNSET_STK,       /* unsetStk: pop 1 */
    OP_RETURN_STK,      /* returnStk: reset */
    OP_STRCAT_N,        /* strcat, list, concat, lappendList: pop N, push 1 */
    OP_RESET, /* return, done, jump, invokeExpanded, invokeReplace, tailcall, yield, reverse, dictFirst, dictNext, uplevel */
    OP_ARITH, /* arithmetic, comparison, string ops, list ops, dict ops, scalar/array variable ops: clobber TOS */
};

/* Cached opcode→handler map.  Built once during TbcxInitTypes() under
 * the tbcxTypeMutex, before tbcxTypesLoaded is published.  Immutable
 * after init — safe to read from any thread without synchronization.
 * TclGetInstructionTable() is constant for a given Tcl build. */
static unsigned char tbcxOpMap[256];
static int tbcxOpMapReady = 0;

/* TbcxSaveInitOpcodesLocked — one-time opcode map initialization.
 * Called from TbcxInitTypes() inside the type-init mutex, before the
 * tbcxTypesLoaded flag is published.  Must NOT be called concurrently.
 * Not declared in tbcx.h — only accessible via file-local extern in tbcx.c. */
void TbcxSaveInitOpcodesLocked(void)
{
    if (tbcxOpMapReady)
        return;

    const InstructionDesc* instTable = (const InstructionDesc*)TclGetInstructionTable();

    memset(tbcxOpMap, OP_UNKNOWN, sizeof(tbcxOpMap));
    for (unsigned i = 0; i <= LAST_INST_OPCODE; i++)
    {
        const char* nm = instTable[i].name;
        if (!nm)
            continue;

        switch (nm[0])
        {
            case 'p':
                if (InstrIs(nm, "push"))
                {
                    tbcxOpMap[i] = (nm[4] == 'R'                                    ? OP_PUSH1_NOPOP
                                    : nm[4] == '\0' || nm[4] == '1' || nm[4] == '4' ? OP_PUSH
                                                                                    : OP_PUSH1_NOPOP);
                }
                else if (InstrIs(nm, "pop"))
                    tbcxOpMap[i] = OP_POP;
                break;
            case 'i':
                if (InstrIs(nm, "invokeStk"))
                    tbcxOpMap[i] = OP_INVOKE_STK;
                else if (InstrIs(nm, "invokeExpanded") || InstrIs(nm, "invokeReplace"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "incrStkImm"))
                    tbcxOpMap[i] = OP_INCR_STK_IMM;
                else if (InstrIs(nm, "incrStk"))
                    tbcxOpMap[i] = OP_INCR_STK;
                else if (InstrIs(nm, "incrScalar") || InstrIs(nm, "incrArray"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "isEmpty") || InstrIs(nm, "infoLevelNumber"))
                    tbcxOpMap[i] = (nm[1] == 'n' ? OP_PUSH1_NOPOP : OP_ARITH);
                break;
            case 'l':
                if (InstrIs(nm, "loadStk"))
                    tbcxOpMap[i] = OP_LOAD_STK;
                else if (InstrIs(nm, "loadArrayStk"))
                    tbcxOpMap[i] = OP_LOAD_ARR_STK;
                else if (InstrIs(nm, "loadArray"))
                    tbcxOpMap[i] = OP_LOAD_STK;
                else if (InstrIs(nm, "loadScalar"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "lappendListStk"))
                    tbcxOpMap[i] = OP_LAPPEND_LST_STK;
                else if (InstrIs(nm, "lappendStk"))
                    tbcxOpMap[i] = OP_INCR_STK;
                else if (InstrIs(nm, "lappendList"))
                    tbcxOpMap[i] = OP_STRCAT_N;
                else if (InstrIs(nm, "lappendScalar") || InstrIs(nm, "lappendArray"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "list"))
                    tbcxOpMap[i] = OP_STRCAT_N;
                else if (InstrIs(nm, "lindexMulti") || InstrIs(nm, "listLength") || InstrIs(nm, "listIndexImm") ||
                         InstrIs(nm, "listRangeImm") || InstrIs(nm, "lreplace") || InstrIs(nm, "lsetFlat") ||
                         InstrIs(nm, "land") || InstrIs(nm, "lor") || InstrIs(nm, "lnot") || InstrIs(nm, "lt") ||
                         InstrIs(nm, "le") || InstrIs(nm, "lshift"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "lmap_collect"))
                    tbcxOpMap[i] = OP_POP;
                break;
            case 's':
                if (InstrIs(nm, "storeStk"))
                    tbcxOpMap[i] = OP_STORE_STK;
                else if (InstrIs(nm, "storeArrayStk"))
                    tbcxOpMap[i] = OP_STORE_ARR_STK;
                else if (InstrIs(nm, "storeArray"))
                    tbcxOpMap[i] = OP_STORE_STK;
                else if (InstrIs(nm, "storeScalar"))
                    tbcxOpMap[i] = OP_STORE_LVT;
                else if (InstrIs(nm, "startCommand"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "strcat"))
                    tbcxOpMap[i] = OP_STRCAT_N;
                else if (InstrIs(nm, "swap"))
                    tbcxOpMap[i] = OP_SWAP;
                else if (InstrIs(nm, "sub") || InstrIs(nm, "streq") || InstrIs(nm, "strlen") || InstrIs(nm, "strmatch") ||
                         InstrIs(nm, "strindex") || InstrIs(nm, "strclass") || InstrIs(nm, "strfind") ||
                         InstrIs(nm, "strrfind") || InstrIs(nm, "strrangeImm") || InstrIs(nm, "strreplace") ||
                         InstrIs(nm, "strtrim") || InstrIs(nm, "strcaseLower") || InstrIs(nm, "strcaseUpper") ||
                         InstrIs(nm, "strlt") || InstrIs(nm, "strgt") || InstrIs(nm, "strle") || InstrIs(nm, "strge"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'a':
                if (InstrIs(nm, "appendStk"))
                    tbcxOpMap[i] = OP_INCR_STK;
                else if (InstrIs(nm, "arithSeries"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "add") || InstrIs(nm, "appendScalar") || InstrIs(nm, "appendArray"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'b':
                if (InstrIs(nm, "beginCatch"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "bitand") || InstrIs(nm, "bitor") || InstrIs(nm, "bitxor") || InstrIs(nm, "bitnot"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'c':
                if (InstrIs(nm, "concat"))
                    tbcxOpMap[i] = OP_STRCAT_N;
                else if (InstrIs(nm, "clockRead"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "constImm"))
                    tbcxOpMap[i] = OP_POP;
                else if (InstrIs(nm, "constStk"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "currentNamespace"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                break;
            case 'd':
                if (InstrIs(nm, "done"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "dup"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "div") || InstrIs(nm, "dictGet") || InstrIs(nm, "dictPut") || InstrIs(nm, "dictSet") ||
                         InstrIs(nm, "dictExists") || InstrIs(nm, "dictRemove") || InstrIs(nm, "dictGetDef"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "dictFirst") || InstrIs(nm, "dictNext"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "dictUpdateStart") || InstrIs(nm, "dictUpdateEnd"))
                    tbcxOpMap[i] = OP_NOP;
                break;
            case 'e':
                if (InstrIs(nm, "endCatch"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "expandStkTop"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "expandStart"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "existStk"))
                    tbcxOpMap[i] = OP_INCR_STK_IMM;
                else if (InstrIs(nm, "existScalar") || InstrIs(nm, "existArray") || InstrIs(nm, "eq") || InstrIs(nm, "expon") ||
                         InstrIs(nm, "exprStk") || InstrIs(nm, "errorPrefixEq"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'f':
                if (InstrIs(nm, "foreach_start") || InstrIs(nm, "foreach_step") || InstrIs(nm, "foreach_end"))
                    tbcxOpMap[i] = OP_NOP;
                break;
            case 'g':
                if (InstrIs(nm, "gt") || InstrIs(nm, "ge"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'j':
                if (InstrIs(nm, "jump"))
                    tbcxOpMap[i] = OP_RESET;
                break;
            case 'm':
                if (InstrIs(nm, "mult") || InstrIs(nm, "mod"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'n':
                if (InstrIs(nm, "nop"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "not") || InstrIs(nm, "neq") || InstrIs(nm, "numericType"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "nsupvar"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                break;
            case 'o':
                if (InstrIs(nm, "over"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                break;
            case 'r':
                if (InstrIs(nm, "returnStk"))
                    tbcxOpMap[i] = OP_RETURN_STK;
                else if (InstrIs(nm, "return"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "reverse"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "rshift") || InstrIs(nm, "regexp"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 't':
                if (InstrIs(nm, "tailcall"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "tclooClass"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "tclooId"))
                    tbcxOpMap[i] = OP_ARITH;
                else if (InstrIs(nm, "tclooNext"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "tclooSelf"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "tryCvtToNumeric"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'u':
                if (InstrIs(nm, "unsetStk"))
                    tbcxOpMap[i] = OP_UNSET_STK;
                else if (InstrIs(nm, "unsetScalar"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "uplevel"))
                    tbcxOpMap[i] = OP_RESET;
                else if (InstrIs(nm, "upvar"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                else if (InstrIs(nm, "uminus") || InstrIs(nm, "uplus"))
                    tbcxOpMap[i] = OP_ARITH;
                break;
            case 'v':
                if (InstrIs(nm, "verifyDict"))
                    tbcxOpMap[i] = OP_NOP;
                else if (InstrIs(nm, "variable"))
                    tbcxOpMap[i] = OP_PUSH1_NOPOP;
                break;
            case 'y':
                if (InstrIs(nm, "yield"))
                    tbcxOpMap[i] = OP_RESET;
                break;
        }
    }
    tbcxOpMapReady = 1;
}

static void LiteralPlanFree(TbcxLiteralPlan* plan)
{
    if (!plan)
        return;
    if (plan->occurrences)
        Tcl_Free(plan->occurrences);
    if (plan->retainOriginal)
        Tcl_Free(plan->retainOriginal);
    if (plan->hasExecutable)
        Tcl_Free(plan->hasExecutable);
    memset(plan, 0, sizeof(*plan));
}

static int LiteralPlanInit(TbcxLiteralPlan* plan, Tcl_Interp* interp, Tcl_Size numOriginal)
{
    memset(plan, 0, sizeof(*plan));
    plan->numOriginal = numOriginal;
    if (numOriginal <= 0)
        return TCL_OK;
    if ((uint64_t)numOriginal > TBCX_MAX_LITERALS)
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: too many literals for occurrence plan", -1));
        return TCL_ERROR;
    }
    plan->retainOriginal = (unsigned char*)Tcl_AttemptAlloc((size_t)numOriginal);
    plan->hasExecutable = (unsigned char*)Tcl_AttemptAlloc((size_t)numOriginal);
    if (!plan->retainOriginal || !plan->hasExecutable)
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: allocation failed (literal occurrence plan)", -1));
        LiteralPlanFree(plan);
        return TCL_ERROR;
    }
    memset(plan->retainOriginal, 0, (size_t)numOriginal);
    memset(plan->hasExecutable, 0, (size_t)numOriginal);
    return TCL_OK;
}

static int LiteralPlanAddPush(TbcxLiteralPlan* plan, Tcl_Interp* interp, Tcl_Size literalIndex, Tcl_Size pushPc)
{
    if (plan->count == plan->capacity)
    {
        Tcl_Size newCapacity = plan->capacity ? plan->capacity * 2 : 64;
        if (newCapacity < plan->capacity || (uint64_t)newCapacity > TBCX_MAX_LITERALS)
        {
            newCapacity = TBCX_MAX_LITERALS;
        }
        if (newCapacity <= plan->capacity)
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: too many literal push occurrences", -1));
            return -1;
        }
        size_t bytes = 0;
        if (!tbcx_checked_mul((size_t)newCapacity, sizeof(*plan->occurrences), &bytes))
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: literal occurrence plan size overflow", -1));
            return -1;
        }
        void* newStorage = plan->occurrences ? Tcl_AttemptRealloc(plan->occurrences, bytes) : Tcl_AttemptAlloc(bytes);
        if (!newStorage)
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: allocation failed (literal push occurrences)", -1));
            return -1;
        }
        plan->occurrences = (TbcxLiteralOccurrence*)newStorage;
        plan->capacity = newCapacity;
    }

    Tcl_Size index = plan->count++;
    TbcxLiteralOccurrence* occ = &plan->occurrences[index];
    occ->literalIndex = literalIndex;
    occ->pushPc = pushPc;
    occ->role = TBCX_LITERAL_DATA;
    occ->namespacePtr = NULL;
    occ->defineBody = 0;
    return (int)index;
}

static void
LiteralPlanMark(TbcxLiteralPlan* plan, TbcxCtx* ctx, int occurrenceIndex, TbcxLiteralRole role, Namespace* namespacePtr)
{
    if (occurrenceIndex < 0 || (Tcl_Size)occurrenceIndex >= plan->count)
        return;
    TbcxLiteralOccurrence* occ = &plan->occurrences[occurrenceIndex];
    if (occ->literalIndex < 0 || occ->literalIndex >= plan->numOriginal)
        return;

    if (occ->role != TBCX_LITERAL_DATA && occ->role != role)
    {
        ctx->sourceRequired = 1;
        return;
    }
    if (occ->role == role && occ->namespacePtr && namespacePtr && occ->namespacePtr != namespacePtr)
    {
        ctx->sourceRequired = 1;
        return;
    }
    occ->role = role;
    occ->namespacePtr = namespacePtr;
}

static int OperandWidth(InstOperandType type)
{
    switch (type)
    {
        case OPERAND_NONE:
            return 0;
        case OPERAND_INT1:
        case OPERAND_UINT1:
        case OPERAND_LVT1:
        case OPERAND_OFFSET1:
        case OPERAND_LIT1:
        case OPERAND_SCLS1:
        case OPERAND_UNSF1:
        case OPERAND_CLK1:
        case OPERAND_LRPL1:
            return 1;
        case OPERAND_INT4:
        case OPERAND_UINT4:
        case OPERAND_IDX4:
        case OPERAND_LVT4:
        case OPERAND_AUX4:
        case OPERAND_OFFSET4:
        case OPERAND_LIT4:
            return 4;
        default:
            return -1;
    }
}

/* Build an occurrence plan from the generic instruction stream.  Stack
 * entries identify the exact PUSH instruction, not merely its literal slot;
 * equal strings can therefore be data and scripts in the same block. */
static int InstrScanBodyLiterals(ByteCode* bc, TbcxCtx* ctx, TbcxLiteralPlan* plan)
{
    if (!bc || !ctx || !plan || !bc->codeStart || bc->numCodeBytes <= 0 || !bc->objArrayPtr || bc->numLitObjects <= 0)
        return TCL_OK;

    const InstructionDesc* instTable = (const InstructionDesc*)TclGetInstructionTable();
    const unsigned char* code = bc->codeStart;
    Tcl_Size codeLen = bc->numCodeBytes;
    Tcl_Size numLits = bc->numLitObjects;
    Tcl_Size stackCapacity = bc->maxStackDepth > 0 ? bc->maxStackDepth + 16 : 128;
    if (stackCapacity < 128)
        stackCapacity = 128;
    if ((uint64_t)stackCapacity > TBCX_MAX_CODE)
    {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: bytecode stack is too deep to classify", -1));
        return TCL_ERROR;
    }
    size_t stackBytes = 0;
    if (!tbcx_checked_mul((size_t)stackCapacity, sizeof(int), &stackBytes))
    {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: literal classifier stack overflow", -1));
        return TCL_ERROR;
    }
    int* stack = (int*)Tcl_AttemptAlloc(stackBytes);
    if (!stack)
    {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: allocation failed (literal classifier stack)", -1));
        return TCL_ERROR;
    }
    Tcl_Size sp = 0;

#define ISCAN_PUSH(v_)                                                                                                           \
    do                                                                                                                           \
    {                                                                                                                            \
        if (sp < stackCapacity)                                                                                                  \
        {                                                                                                                        \
            stack[sp++] = (v_);                                                                                                  \
        }                                                                                                                        \
        else                                                                                                                     \
        {                                                                                                                        \
            Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: literal classifier stack overflow", -1));                      \
            goto scan_error;                                                                                                     \
        }                                                                                                                        \
    } while (0)
#define ISCAN_POP() (sp > 0 ? stack[--sp] : -1)
#define OCC_LIT(occ_) ((occ_) >= 0 && (Tcl_Size)(occ_) < plan->count ? (int)plan->occurrences[(occ_)].literalIndex : -1)
#define STACK_LIT(pos_) OCC_LIT(stack[(pos_)])
#define MARK_OCC_AS(occ_, role_, ns_) LiteralPlanMark(plan, ctx, (occ_), (role_), (ns_))
#define MARK_SCRIPT(occ_) MARK_OCC_AS((occ_), TBCX_LITERAL_SCRIPT_SAME_INTERP, bc->nsPtr)

    Tcl_Size pc = 0;
    while (pc < codeLen)
    {
        unsigned int opcode = code[pc];
        if (opcode > LAST_INST_OPCODE)
        {
            Tcl_SetObjResult(ctx->interp, Tcl_ObjPrintf("tbcx: invalid compiler opcode at pc %" TCL_SIZE_MODIFIER "d", pc));
            goto scan_error;
        }
        const InstructionDesc* desc = &instTable[opcode];
        if (desc->numBytes <= 0 || pc + desc->numBytes > codeLen)
        {
            Tcl_SetObjResult(ctx->interp,
                             Tcl_ObjPrintf("tbcx: truncated compiler instruction at pc %" TCL_SIZE_MODIFIER "d", pc));
            goto scan_error;
        }
        const unsigned char* operand = code + pc + 1;

        switch (tbcxOpMap[opcode])
        {
            case OP_PUSH:
            {
                int literalIndex = desc->numBytes == 2   ? (int)TclGetUInt1AtPtr(operand)
                                   : desc->numBytes == 5 ? (int)TclGetUInt4AtPtr(operand)
                                                         : -1;
                int occurrenceIndex = LiteralPlanAddPush(plan, ctx->interp, literalIndex, pc);
                if (occurrenceIndex < 0)
                    goto scan_error;
                ISCAN_PUSH(occurrenceIndex);
                break;
            }

            case OP_INVOKE_STK:
            {
                int argc = desc->numBytes == 2   ? (int)TclGetUInt1AtPtr(operand)
                           : desc->numBytes == 5 ? (int)TclGetUInt4AtPtr(operand)
                                                 : 0;
                if (argc >= 1 && sp >= argc)
                {
                    Tcl_Size base = sp - argc;
                    int commandLiteral = STACK_LIT(base);
                    if (commandLiteral >= 0 && commandLiteral < numLits && bc->objArrayPtr[commandLiteral])
                    {
                        const char* rawCommand = Tbcx_GetStringSafe(bc->objArrayPtr[commandLiteral]);
                        const char* command = CmdCore(rawCommand);
                        int bodyOccurrence = -1;
                        /* Set when `bodyOccurrence` is an `oo::define` /
                         * `oo::objdefine` body, so the writer can raise the
                         * DSL context while that block is scanned. */
                        int bodyIsDefineBody = 0;

                        if (!CtxHasAuthoredCommand(ctx, bc, rawCommand))
                        {
                            if (strcmp(command, "interp") == 0 && argc >= 4)
                            {
                                int subLiteral = STACK_LIT(base + 1);
                                if (subLiteral >= 0 && subLiteral < numLits && bc->objArrayPtr[subLiteral])
                                {
                                    const char* subcommand = Tbcx_GetStringSafe(bc->objArrayPtr[subLiteral]);
                                    size_t subLength = strlen(subcommand);
                                    int isEval = subLength > 0 && subLength <= 4 && strncmp("eval", subcommand, subLength) == 0;
                                    if (isEval)
                                    {
                                        ctx->unsupportedCrossInterp = 1;
                                        for (int i = 3; i < argc; i++)
                                            MARK_OCC_AS(stack[base + i], TBCX_LITERAL_SCRIPT_CROSS_INTERP, NULL);
                                    }
                                }
                            }
                            else if (strcmp(command, "namespace") == 0 && argc >= 4)
                            {
                                int subLiteral = STACK_LIT(base + 1);
                                int namespaceLiteral = STACK_LIT(base + 2);
                                if (subLiteral >= 0 && subLiteral < numLits && namespaceLiteral >= 0 &&
                                    namespaceLiteral < numLits && bc->objArrayPtr[subLiteral] &&
                                    bc->objArrayPtr[namespaceLiteral] &&
                                    strcmp(Tbcx_GetStringSafe(bc->objArrayPtr[subLiteral]), "eval") == 0)
                                {
                                    if (argc != 4)
                                    {
                                        ctx->sourceRequired = 1;
                                    }
                                    else
                                    {
                                        Tcl_Obj* current = NsFqn((Tcl_Namespace*)bc->nsPtr);
                                        Tcl_Obj* targetName = FqnUnder(ctx->interp, current, bc->objArrayPtr[namespaceLiteral]);
                                        Namespace* target =
                                            targetName
                                                ? (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tbcx_GetStringSafe(targetName))
                                                : NULL;
                                        if (targetName)
                                            Tcl_DecrRefCount(targetName);
                                        Tcl_DecrRefCount(current);
                                        if (target)
                                            MARK_OCC_AS(stack[base + 3], TBCX_LITERAL_SCRIPT_SAME_INTERP, target);
                                        else
                                            ctx->sourceRequired = 1;
                                    }
                                }
                            }
                            else if (strcmp(command, "tcl::namespace::eval") == 0 && argc == 3)
                            {
                                int namespaceLiteral = STACK_LIT(base + 1);
                                if (namespaceLiteral >= 0 && namespaceLiteral < numLits && bc->objArrayPtr[namespaceLiteral])
                                {
                                    Tcl_Obj* current = NsFqn((Tcl_Namespace*)bc->nsPtr);
                                    Tcl_Obj* targetName = FqnUnder(ctx->interp, current, bc->objArrayPtr[namespaceLiteral]);
                                    Namespace* target =
                                        targetName ? (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tbcx_GetStringSafe(targetName))
                                                   : NULL;
                                    if (targetName)
                                        Tcl_DecrRefCount(targetName);
                                    Tcl_DecrRefCount(current);
                                    if (target)
                                        MARK_OCC_AS(stack[base + 2], TBCX_LITERAL_SCRIPT_SAME_INTERP, target);
                                    else
                                        ctx->sourceRequired = 1;
                                }
                            }
                            else if (strcmp(command, "dict") == 0 && argc >= 3)
                            {
                                int subLiteral = STACK_LIT(base + 1);
                                if (subLiteral >= 0 && subLiteral < numLits && bc->objArrayPtr[subLiteral] &&
                                    IsDictBodySubcmd(Tbcx_GetStringSafe(bc->objArrayPtr[subLiteral])))
                                    bodyOccurrence = stack[base + argc - 1];
                            }
                            else if (strncmp(command, "tcl::dict::", 11) == 0)
                            {
                                const char* subcommand = command + 11;
                                if (IsDictBodySubcmd(subcommand))
                                    bodyOccurrence = stack[base + argc - 1];
                            }
                            else if (strcmp(command, "self") == 0 && argc >= 5 && ctx->defineBodyDepth > 0)
                            {
                                /* `self method NAME ?opts? ARGS BODY` — the
                                 * DEFINITION DSL only.  Gated on the define-body
                                 * context for the same reason as the two-word
                                 * form below. */
                                int subLiteral = STACK_LIT(base + 1);
                                if (subLiteral >= 0 && subLiteral < numLits && bc->objArrayPtr[subLiteral] &&
                                    strcmp(Tbcx_GetStringSafe(bc->objArrayPtr[subLiteral]), "method") == 0)
                                    bodyOccurrence = stack[base + argc - 1];
                            }
                            else if (strcmp(command, "self") == 0 && argc == 2 && ctx->defineBodyDepth > 0)
                            {
                                /* `self { SCRIPT }` — the DEFINITION DSL.  ONLY
                                 * inside an `oo::define` / `oo::objdefine` body
                                 * does `self` name a command that takes a
                                 * script; everywhere else `self` is TclOO's
                                 * introspection helper and this operand is the
                                 * DATA WORD naming a subcommand (`caller`,
                                 * `class`, `object`, …).
                                 *
                                 * The context is PROPAGATED, not guessed.  It
                                 * was previously inferred from
                                 * `bc->procPtr == NULL` ("not inside a method"),
                                 * which is false for every nested script — a
                                 * `catch`/`if`/`while`/`for`/`foreach`/`try`
                                 * body inside a method has a NULL `procPtr` —
                                 * so `[self caller]` written inside one had its
                                 * subcommand word classified as a script and
                                 * therefore BLANKED, and the artifact evaluated
                                 * `self ""`. */
                                bodyOccurrence = stack[base + 1];
                            }
                            else if ((strcmp(command, "oo::define") == 0 || strcmp(command, "oo::objdefine") == 0) && argc == 3)
                            {
                                bodyOccurrence = stack[base + 2];
                                bodyIsDefineBody = 1;
                            }
                            else if (strcmp(command, "for") == 0 && argc == 5)
                            {
                                MARK_SCRIPT(stack[base + 1]);
                                MARK_SCRIPT(stack[base + 3]);
                                bodyOccurrence = stack[base + 4];
                            }
                            else
                            {
                                for (const BodyCmdRule* rule = sBodyCmds; rule->cmd; rule++)
                                {
                                    if (strcmp(command, rule->cmd) != 0)
                                        continue;
                                    /* A DSL-only rule needs the DSL context.
                                     * `private` names a script-taking command
                                     * ONLY inside an `oo::define` body; the same
                                     * word elsewhere is somebody else's command
                                     * and its last operand is data.  Same class
                                     * of hazard as `self` above — a wrong
                                     * classification does not merely mislabel
                                     * the operand, it BLANKS it. */
                                    if (rule->defineOnly && ctx->defineBodyDepth <= 0)
                                        break;
                                    bodyOccurrence = rule->bodyPos == 0
                                                         ? stack[base + argc - 1]
                                                         : (base + rule->bodyPos < sp ? stack[base + rule->bodyPos] : -1);
                                    break;
                                }
                            }

                            if (strcmp(command, "eval") == 0 && argc != 2)
                                ctx->sourceRequired = 1;
                            if (strcmp(command, "uplevel") == 0 && argc > 3)
                                ctx->sourceRequired = 1;
                            MARK_SCRIPT(bodyOccurrence);
                            if (bodyIsDefineBody && bodyOccurrence >= 0 && bodyOccurrence < plan->count)
                                plan->occurrences[bodyOccurrence].defineBody = 1;

                            if (strcmp(command, "apply") == 0 && argc >= 2)
                                MARK_OCC_AS(stack[base + 1], TBCX_LITERAL_LAMBDA, bc->nsPtr);

                            if (strcmp(command, "if") == 0 && argc >= 3)
                            {
                                int pos = 2;
                                while (pos < argc)
                                {
                                    int literal = STACK_LIT(base + pos);
                                    const char* word = literal >= 0 && literal < numLits && bc->objArrayPtr[literal]
                                                           ? Tbcx_GetStringSafe(bc->objArrayPtr[literal])
                                                           : "";
                                    if (strcmp(word, "then") == 0)
                                        pos++;
                                    if (pos >= argc)
                                        break;
                                    MARK_SCRIPT(stack[base + pos]);
                                    pos++;
                                    if (pos >= argc)
                                        break;
                                    literal = STACK_LIT(base + pos);
                                    word = literal >= 0 && literal < numLits && bc->objArrayPtr[literal]
                                               ? Tbcx_GetStringSafe(bc->objArrayPtr[literal])
                                               : "";
                                    if (strcmp(word, "elseif") == 0)
                                        pos += 2;
                                    else if (strcmp(word, "else") == 0)
                                        pos++;
                                    else
                                        break;
                                }
                            }

                            if (strcmp(command, "try") == 0 && argc >= 4)
                            {
                                int pos = 2;
                                while (pos < argc)
                                {
                                    int literal = STACK_LIT(base + pos);
                                    if (literal < 0 || literal >= numLits || !bc->objArrayPtr[literal])
                                        break;
                                    const char* keyword = Tbcx_GetStringSafe(bc->objArrayPtr[literal]);
                                    if (strcmp(keyword, "on") == 0 || strcmp(keyword, "trap") == 0)
                                    {
                                        if (pos + 3 < argc)
                                            MARK_SCRIPT(stack[base + pos + 3]);
                                        pos += 4;
                                    }
                                    else if (strcmp(keyword, "finally") == 0)
                                    {
                                        if (pos + 1 < argc)
                                            MARK_SCRIPT(stack[base + pos + 1]);
                                        break;
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                            }

                            if (strcmp(command, "switch") == 0 && argc >= 3)
                            {
                                int pos = 1;
                                while (pos < argc)
                                {
                                    int literal = STACK_LIT(base + pos);
                                    const char* word = literal >= 0 && literal < numLits && bc->objArrayPtr[literal]
                                                           ? Tbcx_GetStringSafe(bc->objArrayPtr[literal])
                                                           : "";
                                    if (strcmp(word, "--") == 0)
                                    {
                                        pos++;
                                        break;
                                    }
                                    if (strcmp(word, "-exact") == 0 || strcmp(word, "-glob") == 0 ||
                                        strcmp(word, "-regexp") == 0 || strcmp(word, "-integer") == 0 ||
                                        strcmp(word, "-nocase") == 0)
                                    {
                                        pos++;
                                        continue;
                                    }
                                    if (strcmp(word, "-matchvar") == 0 || strcmp(word, "-indexvar") == 0)
                                    {
                                        pos += 2;
                                        continue;
                                    }
                                    break;
                                }
                                pos++; /* value being matched */
                                int remaining = argc - pos;
                                if (remaining == 1)
                                {
                                    MARK_OCC_AS(stack[base + pos], TBCX_LITERAL_SWITCH_LIST, bc->nsPtr);
                                }
                                else if (remaining >= 2 && (remaining % 2) == 0)
                                {
                                    for (int i = pos + 1; i < argc; i += 2)
                                    {
                                        int bodyLiteral = STACK_LIT(base + i);
                                        if (bodyLiteral >= 0 && bodyLiteral < numLits && bc->objArrayPtr[bodyLiteral] &&
                                            strcmp(Tbcx_GetStringSafe(bc->objArrayPtr[bodyLiteral]), "-") != 0)
                                            MARK_SCRIPT(stack[base + i]);
                                    }
                                }
                                else
                                {
                                    ctx->sourceRequired = 1;
                                }
                            }
                        }
                    }
                }
                sp = sp >= argc ? sp - argc : 0;
                ISCAN_PUSH(-1);
                break;
            }

            case OP_NOP:
                break;
            case OP_POP:
                (void)ISCAN_POP();
                break;
            case OP_PUSH1_NOPOP:
                ISCAN_PUSH(-1);
                break;
            case OP_LOAD_STK:
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_LOAD_ARR_STK:
                (void)ISCAN_POP();
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_STORE_LVT:
                if (sp > 0)
                    stack[sp - 1] = -1;
                break;
            case OP_STORE_STK:
                (void)ISCAN_POP();
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_STORE_ARR_STK:
                (void)ISCAN_POP();
                (void)ISCAN_POP();
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_SWAP:
                if (sp >= 2)
                {
                    int tmp = stack[sp - 1];
                    stack[sp - 1] = stack[sp - 2];
                    stack[sp - 2] = tmp;
                }
                break;
            case OP_INCR_STK_IMM:
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_INCR_STK:
            case OP_LAPPEND_LST_STK:
                (void)ISCAN_POP();
                (void)ISCAN_POP();
                ISCAN_PUSH(-1);
                break;
            case OP_UNSET_STK:
                (void)ISCAN_POP();
                break;
            case OP_RETURN_STK:
            case OP_RESET:
                sp = 0;
                break;
            case OP_STRCAT_N:
            {
                int count = desc->numBytes == 2   ? (int)TclGetUInt1AtPtr(operand)
                            : desc->numBytes == 5 ? (int)TclGetUInt4AtPtr(operand)
                                                  : 0;
                sp = sp >= count ? sp - count : 0;
                ISCAN_PUSH(-1);
                break;
            }
            case OP_ARITH:
                if (sp > 0)
                    stack[sp - 1] = -1;
                break;
            default:
                sp = 0;
                break;
        }
        pc += desc->numBytes;
    }

    /* A source-bearing original slot is neutralized only when every use is an
     * executable occurrence. */
    for (Tcl_Size i = 0; i < plan->count; i++)
    {
        TbcxLiteralOccurrence* occ = &plan->occurrences[i];
        if (occ->literalIndex < 0 || occ->literalIndex >= numLits)
            continue;
        if (occ->role == TBCX_LITERAL_DATA)
        {
            plan->retainOriginal[occ->literalIndex] = 1;
        }
        else
        {
            plan->hasExecutable[occ->literalIndex] = 1;
            plan->executableCount++;
        }
    }

    /* Conservatively retain any literal referenced by an instruction other
     * than PUSH (for example a syntax-error record). */
    pc = 0;
    while (pc < codeLen)
    {
        unsigned int opcode = code[pc];
        const InstructionDesc* desc = &instTable[opcode];
        if (tbcxOpMap[opcode] != OP_PUSH)
        {
            int offset = 1;
            for (int i = 0; i < desc->numOperands; i++)
            {
                InstOperandType type = desc->opTypes[i];
                int width = OperandWidth(type);
                if (width < 0 || offset + width > desc->numBytes)
                {
                    Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: unknown compiler operand layout", -1));
                    goto scan_error;
                }
                if (type == OPERAND_LIT1 || type == OPERAND_LIT4)
                {
                    unsigned int literalIndex =
                        type == OPERAND_LIT1 ? TclGetUInt1AtPtr(code + pc + offset) : TclGetUInt4AtPtr(code + pc + offset);
                    if (literalIndex < (unsigned)numLits)
                        plan->retainOriginal[literalIndex] = 1;
                }
                offset += width;
            }
        }
        pc += desc->numBytes;
    }

    Tcl_Free(stack);
#undef ISCAN_PUSH
#undef ISCAN_POP
#undef OCC_LIT
#undef STACK_LIT
#undef MARK_OCC_AS
#undef MARK_SCRIPT
    return TCL_OK;

scan_error:
    Tcl_Free(stack);
#undef ISCAN_PUSH
#undef ISCAN_POP
#undef OCC_LIT
#undef STACK_LIT
#undef MARK_OCC_AS
#undef MARK_SCRIPT
    return TCL_ERROR;
}

static int WriteScriptOccurrence(TbcxOut* writer, TbcxCtx* ctx, Tcl_Obj* authoredSource, Namespace* targetNamespace)
{
    Tcl_Obj* copy = Tcl_DuplicateObj(authoredSource);
    Tcl_IncrRefCount(copy);
    int result = CompileScriptForArtifact(ctx->interp, copy, ctx->artifactPolicy);
    if (result == TCL_OK)
    {
        ByteCode* inner = TbcxGetByteCode(copy);
        if (!inner)
        {
            result = TCL_ERROR;
        }
        else
        {
            if (!targetNamespace)
                targetNamespace = (Namespace*)Tcl_GetGlobalNamespace(ctx->interp);
            inner->nsPtr = targetNamespace;
            inner->nsEpoch = targetNamespace->resolverEpoch;
            WriteCompiledScriptLiteral(writer, ctx, copy, authoredSource);
            if (writer->err != TCL_OK)
                result = TCL_ERROR;
        }
    }
    Tcl_DecrRefCount(copy);

    if (result != TCL_OK && writer->err == TCL_OK)
    {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: compiled-only save cannot represent script occurrence", -1));
        Tcl_SetErrorCode(ctx->interp, "TBCX", "SAVE", "SOURCE_REQUIRED", "script", NULL);
        writer->err = TCL_ERROR;
    }
    return result;
}

static int WriteSwitchListOccurrence(TbcxOut* writer, TbcxCtx* ctx, Tcl_Obj* pairList, Namespace* targetNamespace)
{
    Tcl_Size count = 0;
    Tcl_Obj** elements = NULL;
    if (Tcl_ListObjGetElements(ctx->interp, pairList, &count, &elements) != TCL_OK || (count % 2) != 0)
    {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("tbcx: switch pattern/body list is not statically representable", -1));
        Tcl_SetErrorCode(ctx->interp, "TBCX", "SAVE", "SOURCE_REQUIRED", "switch", NULL);
        writer->err = TCL_ERROR;
        return TCL_ERROR;
    }
    if ((uint64_t)count > TBCX_MAX_LITERALS)
    {
        W_Error(writer, "tbcx: switch pattern/body list is too large");
        return TCL_ERROR;
    }

    W_U32(writer, TBCX_LIT_LIST);
    W_U32(writer, (uint32_t)count);
    for (Tcl_Size i = 0; i < count && writer->err == TCL_OK; i++)
    {
        if ((i & 1) == 0 || strcmp(Tbcx_GetStringSafe(elements[i]), "-") == 0)
        {
            WriteLiteral(writer, ctx, elements[i]);
        }
        else
        {
            if (WriteScriptOccurrence(writer, ctx, elements[i], targetNamespace) != TCL_OK)
                return TCL_ERROR;
        }
    }
    return writer->err == TCL_OK ? TCL_OK : TCL_ERROR;
}

static void WriteCompiledBlock(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj, uint32_t blockPolicy)
{
    ByteCode* bc = NULL;
    bc = TbcxGetByteCode(bcObj);

    if (!bc)
    {
        W_Error(w, "tbcx: object is not bytecode");
        return;
    }

    /* Runaway detection */
    if (ctx)
    {
        ctx->totalBlocks++;
        ctx->blockDepth++;
        if (ctx->blockDepth > ctx->maxBlockDepth)
            ctx->maxBlockDepth = ctx->blockDepth;
        if (ctx->runaway || w->err)
        {
            ctx->blockDepth--;
            return;
        }
        if (ctx->totalBlocks > TBCX_MAX_BLOCK_CALLS || ctx->blockDepth > TBCX_MAX_BLOCK_DEPTH)
        {
            ctx->runaway = 1;
            W_Error(w, "tbcx: runaway serialization detected (block depth or count exceeded)");
            ctx->blockDepth--;
            return;
        }
    }

    if ((uint64_t)bc->numLitObjects > TBCX_MAX_LITERALS)
    {
        W_Error(w, "tbcx: too many literals");
        if (ctx)
            ctx->blockDepth--;
        return;
    }
    if ((uint64_t)bc->numAuxDataItems > TBCX_MAX_AUX)
    {
        W_Error(w, "tbcx: too many AuxData");
        if (ctx)
            ctx->blockDepth--;
        return;
    }
    if ((uint64_t)bc->numExceptRanges > TBCX_MAX_EXCEPT)
    {
        W_Error(w, "tbcx: too many exceptions");
        if (ctx)
            ctx->blockDepth--;
        return;
    }

    /* Build the per-PUSH plan before writing anything from this block.  Each
     * executable push receives a fresh literal record and an adjusted operand;
     * Tcl's literal-pool deduplication therefore cannot merge occurrences. */
    TbcxLiteralPlan plan;
    if (LiteralPlanInit(&plan, w->interp, bc->numLitObjects) != TCL_OK)
    {
        w->err = TCL_ERROR;
        if (ctx)
            ctx->blockDepth--;
        return;
    }
    if (ctx && InstrScanBodyLiterals(bc, ctx, &plan) != TCL_OK)
        w->err = TCL_ERROR;

    if (ctx && ctx->unsupportedCrossInterp)
    {
#ifdef TBCX_BENCH_COUNTERS
        Tbcx_BenchUnsupportedCrossInterpReject();
#endif
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj("tbcx: cross-interpreter compiled script is unsupported", -1));
        Tcl_SetErrorCode(w->interp, "TBCX", "EVAL", "CROSSINTERP", "UNSUPPORTED", NULL);
        w->err = TCL_ERROR;
    }
    else if (ctx && ctx->sourceRequired)
    {
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj("tbcx: compiled-only save cannot represent script occurrence", -1));
        Tcl_SetErrorCode(w->interp, "TBCX", "SAVE", "SOURCE_REQUIRED", "script", NULL);
        w->err = TCL_ERROR;
    }

    uint64_t serializedLiteralCount = (uint64_t)bc->numLitObjects + (uint64_t)plan.executableCount;
    if (serializedLiteralCount > TBCX_MAX_LITERALS || serializedLiteralCount > UINT32_MAX)
    {
        W_Error(w, "tbcx: too many occurrence literals");
    }
    if ((uint64_t)bc->numCodeBytes > TBCX_MAX_CODE)
    {
        W_Error(w, "tbcx: code too large");
    }
    if (w->err)
    {
        LiteralPlanFree(&plan);
        if (ctx)
            ctx->blockDepth--;
        return;
    }

    unsigned char* serializedCode = NULL;
    const unsigned char* codeBytes = bc->codeStart;
    if (plan.executableCount > 0)
    {
        serializedCode = (unsigned char*)Tcl_AttemptAlloc((size_t)bc->numCodeBytes);
        if (!serializedCode)
        {
            W_Error(w, "tbcx: allocation failed (occurrence bytecode)");
        }
        else
        {
            memcpy(serializedCode, bc->codeStart, (size_t)bc->numCodeBytes);
            const InstructionDesc* table = (const InstructionDesc*)TclGetInstructionTable();
            uint32_t nextLiteral = (uint32_t)bc->numLitObjects;
            for (Tcl_Size i = 0; i < plan.count && !w->err; i++)
            {
                TbcxLiteralOccurrence* occ = &plan.occurrences[i];
                if (occ->role == TBCX_LITERAL_DATA)
                    continue;
                if (occ->role == TBCX_LITERAL_SCRIPT_CROSS_INTERP)
                {
                    W_Error(w, "tbcx: unsupported cross-interpreter occurrence");
                    break;
                }
                if (occ->pushPc < 0 || occ->pushPc >= bc->numCodeBytes)
                {
                    W_Error(w, "tbcx: invalid literal occurrence pc");
                    break;
                }
                unsigned int opcode = serializedCode[occ->pushPc];
                const InstructionDesc* desc = &table[opcode];
                if (tbcxOpMap[opcode] != OP_PUSH)
                {
                    W_Error(w, "tbcx: literal occurrence is not a push");
                    break;
                }
                if (desc->numBytes == 2)
                {
                    if (nextLiteral > UINT8_MAX)
                    {
                        W_Error(w, "tbcx: push1 occurrence index overflow");
                        break;
                    }
                    serializedCode[occ->pushPc + 1] = (unsigned char)nextLiteral;
                }
                else if (desc->numBytes == 5)
                {
                    TclStoreInt4AtPtr(nextLiteral, serializedCode + occ->pushPc + 1);
                }
                else
                {
                    W_Error(w, "tbcx: unsupported push instruction width");
                    break;
                }
                nextLiteral++;
            }
            codeBytes = serializedCode;
        }
    }
    if (w->err)
    {
        if (serializedCode)
            Tcl_Free(serializedCode);
        LiteralPlanFree(&plan);
        if (ctx)
            ctx->blockDepth--;
        return;
    }

    /* Preserve the compiler's instruction stream apart from literal-index
     * operands rewritten above.  Generic dispatch remains intact. */
    W_U32(w, (uint32_t)bc->numCodeBytes);
    W_Bytes(w, codeBytes, (size_t)bc->numCodeBytes);
    if (serializedCode)
        Tcl_Free(serializedCode);

    W_U32(w, (uint32_t)serializedLiteralCount);
    for (Tcl_Size i = 0; i < bc->numLitObjects && !w->err; i++)
    {
        if (plan.hasExecutable && plan.hasExecutable[i] && !plan.retainOriginal[i])
        {
            /* The original slot is now unreachable.  Do not leave a second
             * authored copy behind as a STRING record. */
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
        else
        {
            WriteLiteral(w, ctx, bc->objArrayPtr[i]);
        }
    }

    for (Tcl_Size i = 0; i < plan.count && !w->err; i++)
    {
        TbcxLiteralOccurrence* occ = &plan.occurrences[i];
        if (occ->role == TBCX_LITERAL_DATA)
            continue;
        Tcl_Obj* literal = bc->objArrayPtr[occ->literalIndex];
        Namespace* target =
            occ->namespacePtr ? occ->namespacePtr : (bc->nsPtr ? bc->nsPtr : (Namespace*)Tcl_GetGlobalNamespace(ctx->interp));
        switch (occ->role)
        {
            case TBCX_LITERAL_SCRIPT_SAME_INTERP:
                /* RAISE THE DSL CONTEXT ACROSS AN `oo::define` BODY.  The scan
                 * of that nested block — and of every block nested inside it —
                 * runs underneath this call, which is precisely how the
                 * definition-DSL rules learn where they are.  A counter, so a
                 * `self { … }` block or a nested `oo::define` inside the body
                 * keeps the context raised. */
                if (occ->defineBody)
                    ctx->defineBodyDepth++;
                (void)WriteScriptOccurrence(w, ctx, literal, target);
                if (occ->defineBody)
                    ctx->defineBodyDepth--;
                break;
            case TBCX_LITERAL_LAMBDA:
                W_U32(w, TBCX_LIT_LAMBDA_BC);
                Lit_LambdaBC(w, ctx, literal);
                break;
            case TBCX_LITERAL_SWITCH_LIST:
                (void)WriteSwitchListOccurrence(w, ctx, literal, target);
                break;
            case TBCX_LITERAL_SCRIPT_CROSS_INTERP:
                W_Error(w, "tbcx: unsupported cross-interpreter occurrence");
                break;
            default:
                W_Error(w, "tbcx: invalid executable occurrence role");
                break;
        }
    }
    LiteralPlanFree(&plan);
    if (w->err)
    {
        if (ctx)
            ctx->blockDepth--;
        return;
    }
    /* 3) AuxData array */
    W_U32(w, (uint32_t)bc->numAuxDataItems);
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; i++)
    {
        AuxData* ad = &bc->auxDataArrayPtr[i];
        uint32_t tag = 0xFFFFFFFFu;
        if (ad->type == tbcxAuxJTStr)
        {
            tag = TBCX_AUX_JT_STR;
        }
        else if (ad->type == tbcxAuxJTNum)
        {
            tag = TBCX_AUX_JT_NUM;
        }
        else if (ad->type == tbcxAuxDictUpdate)
        {
            tag = TBCX_AUX_DICTUPD;
        }
        else if (ad->type == tbcxAuxNewForeach)
        {
            tag = TBCX_AUX_NEWFORE;
        }
        else
        {
            /* Unknown AuxData type — error out. */
            if (w->err == TCL_OK)
            {
                Tcl_SetObjResult(w->interp,
                                 Tcl_ObjPrintf("tbcx: unsupported AuxData kind '%s'",
                                               (ad->type && ad->type->name) ? ad->type->name : "(null)"));
                w->err = TCL_ERROR;
            }
            if (ctx)
                ctx->blockDepth--;
            return;
        }
        W_U32(w, tag);
        switch (tag)
        {
            case TBCX_AUX_JT_STR:
                WriteAux_JTStr(w, ad);
                break;
            case TBCX_AUX_JT_NUM:
                WriteAux_JTNum(w, ad);
                break;
            case TBCX_AUX_DICTUPD:
                WriteAux_DictUpdate(w, ad);
                break;
            case TBCX_AUX_NEWFORE:
                WriteAux_Foreach(w, ad);
                break;
        }
    }

    /* 4) exception ranges */
    W_U32(w, (uint32_t)bc->numExceptRanges);
    for (Tcl_Size i = 0; i < bc->numExceptRanges; i++)
    {
        ExceptionRange* er = &bc->exceptArrayPtr[i];
        W_U8(w, (uint8_t)er->type);
        W_U32(w, (uint32_t)er->nestingLevel);
        W_U32(w, (uint32_t)er->codeOffset);
        W_U32(w, (uint32_t)er->numCodeBytes);
        W_U32(w, (uint32_t)er->continueOffset);
        W_U32(w, (uint32_t)er->breakOffset);
        W_U32(w, (uint32_t)er->catchOffset);
    }

    /* 5) epilogue */
    W_U32(w, (uint32_t)bc->maxStackDepth);
    W_U32(w, blockPolicy);
    uint32_t numLocals = 0u;

    if (bc->procPtr)
    {
        /* procPtr->numCompiledLocals is authoritative for proc bytecodes.
           It includes ALL locals: parameters, named variables, AND compiler
           temporaries (foreach iteration vars, etc.).
           Do NOT use ComputeNumLocals(bc) here — the AuxData scan can
           read garbage from misaligned ForeachInfo structs in Tcl 9.1,
           producing billion-scale numLocals and a 12GB output explosion. */
        numLocals = (uint32_t)bc->procPtr->numCompiledLocals;
    }
    else if (bc->localCachePtr && bc->localCachePtr->numVars > 0)
    {
        /* For top-level bytecode, LocalCache is authoritative. */
        numLocals = (uint32_t)bc->localCachePtr->numVars;
    }
    else
    {
        /* No Proc and no LocalCache — fall back to AuxData scan.
           This only applies to bare top-level scripts with no LocalCache,
           where foreach/dictupdate temps need to be inferred from AuxData. */
        numLocals = ComputeNumLocals(bc);
    }

    /* Sanity cap — defence in depth.  Silently cap instead of writing to
     * stderr — extension code must not use stderr directly. */
    if (numLocals > TBCX_MAX_LOCALS)
    {
        numLocals = 0;
    }

    W_U32(w, numLocals);
    if (numLocals > 0)
    {
        WriteLocalNames(w, bc, numLocals);
    }
    if (ctx)
        ctx->blockDepth--;
}

static void WriteHeaderTop(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* topObj)
{
    ByteCode* top = NULL;
    top = TbcxGetByteCode(topObj);
    if (!top)
    {
        W_Error(w, "tbcx: failed to get top bytecode");
        return;
    }
    TbcxHeader H;
    H.magic = TBCX_MAGIC;
    H.format = TBCX_FORMAT;
    H.tcl_version = Tbcx_PackTclVersion();
    H.codeLenTop = (uint64_t)top->numCodeBytes;
    H.numExceptTop = (uint32_t)top->numExceptRanges;
    {
        TbcxLiteralPlan plan;
        if (LiteralPlanInit(&plan, w->interp, top->numLitObjects) != TCL_OK || InstrScanBodyLiterals(top, ctx, &plan) != TCL_OK)
        {
            LiteralPlanFree(&plan);
            w->err = TCL_ERROR;
            return;
        }
        uint64_t count = (uint64_t)top->numLitObjects + (uint64_t)plan.executableCount;
        LiteralPlanFree(&plan);
        if (count > TBCX_MAX_LITERALS || count > UINT32_MAX)
        {
            W_Error(w, "tbcx: too many top-level occurrence literals");
            return;
        }
        H.numLitsTop = (uint32_t)count;
    }
    H.numAuxTop = (uint32_t)top->numAuxDataItems;
    H.maxStackTop = (uint32_t)top->maxStackDepth;
    /* Compute top-level locals: LocalCache is authoritative;
       temp span comes from AuxData; take the max. */
    {
        uint32_t nl = 0, auxSpan = ComputeNumLocals(top);
        /* prefer LocalCache (authoritative for top-level compiled code) */
        if (top->localCachePtr && top->localCachePtr->numVars > 0)
        {
            nl = (uint32_t)top->localCachePtr->numVars;
        }
        if (nl < auxSpan)
            nl = auxSpan;
        /* Sanity cap — defence in depth */
        if (nl > 65536)
            nl = 0;
        H.numLocalsTop = nl;
    }
    W_U32(w, H.magic);
    W_U32(w, H.format);
    W_U32(w, H.tcl_version);
    W_U64(w, H.codeLenTop);
    W_U32(w, H.numExceptTop);
    W_U32(w, H.numLitsTop);
    W_U32(w, H.numAuxTop);
    W_U32(w, H.numLocalsTop);
    W_U32(w, H.maxStackTop);
    if (ctx->sourcePath)
    {
        Tcl_Size pLen = 0;
        const char* pStr = Tbcx_GetStringFromObjSafe(ctx->sourcePath, &pLen);
        W_LPString(w, pStr, pLen);
    }
    else
    {
        W_LPString(w, "", 0);
    }
}

static void DV_Init(DefVec* dv)
{
    dv->v = NULL;
    dv->n = 0;
    dv->cap = 0;
}
static void DV_Push(DefVec* dv, DefRec r)
{
    if (dv->n == dv->cap)
    {
        Tcl_Size newCap = dv->cap ? dv->cap * 2 : 16;
        /* Guard against pathological overflow.  Instead of Tcl_Panic
         * (which kills the process), silently refuse — the caller's
         * definition will be lost but the process survives.  In practice
         * this limit is unreachable for legitimate scripts. */
        if (newCap < dv->cap || (size_t)newCap > (SIZE_MAX / sizeof(DefRec)))
        {
            /* Release the caller's incref'd fields to avoid leaks */
            if (r.name)
                Tcl_DecrRefCount(r.name);
            if (r.ns)
                Tcl_DecrRefCount(r.ns);
            if (r.args)
                Tcl_DecrRefCount(r.args);
            if (r.body)
                Tcl_DecrRefCount(r.body);
            if (r.cls)
                Tcl_DecrRefCount(r.cls);
            return;
        }
        dv->cap = newCap;
        dv->v = (DefRec*)Tcl_Realloc(dv->v, (size_t)dv->cap * sizeof(DefRec));
    }
    dv->v[dv->n++] = r;
}
static void DV_Free(DefVec* dv)
{
    for (Tcl_Size i = 0; i < dv->n; i++)
    {
        if (dv->v[i].name)
            Tcl_DecrRefCount(dv->v[i].name);
        if (dv->v[i].ns)
            Tcl_DecrRefCount(dv->v[i].ns);
        if (dv->v[i].args)
            Tcl_DecrRefCount(dv->v[i].args);
        if (dv->v[i].body)
            Tcl_DecrRefCount(dv->v[i].body);
        if (dv->v[i].cls)
            Tcl_DecrRefCount(dv->v[i].cls);
    }
    if (dv->v)
        Tcl_Free((char*)dv->v);
}

static void CS_Init(ClsSet* cs)
{
    cs->init = 1;
    Tcl_InitHashTable(&cs->ht, TCL_STRING_KEYS);
}

static void CS_Add(ClsSet* cs, Tcl_Obj* clsFqn)
{
    if (!cs || !cs->init || !clsFqn)
        return;
    Tcl_Size len = 0;
    const char* s = Tbcx_GetStringFromObjSafe(clsFqn, &len);
    /* TCL_STRING_KEYS truncates at embedded NUL — skip class registration
       for binary FQNs to avoid false collisions. */
    if (len > 0 && memchr(s, '\0', (size_t)len) != NULL)
        return;
    int isNew = 0;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&cs->ht, s, &isNew);
    if (isNew)
        Tcl_SetHashValue(he, NULL);
}

static void CS_Free(ClsSet* cs)
{
    if (!cs || !cs->init)
        return;
    Tcl_DeleteHashTable(&cs->ht);
    cs->init = 0;
}

static Tcl_Obj* WordLiteralObj(const Tcl_Token* wordTok)
{
    /* Prefer: WORD with exactly one TEXT component — already strips braces */
    if (wordTok->type == TCL_TOKEN_WORD && wordTok->numComponents == 1)
    {
        const Tcl_Token* t = wordTok + 1;
        if (t->type == TCL_TOKEN_TEXT)
        {
            Tcl_Obj* o = Tcl_NewStringObj(t->start, t->size);
            Tcl_IncrRefCount(o);
            return o;
        }
    }
    /* Multi-component WORD: braced bodies containing backslash-newline
       continuations parse as WORD (type 1) with TEXT + BS sub-tokens.
       Resolve the BS sequences (primarily backslash-newline -> space) to
       produce the same string value that Tcl's compiler stores in its
       literal pool.  Only handle words where ALL sub-tokens are TEXT or
       BS — if any sub-token is a variable or command substitution, the
       word is not a static literal and we must return NULL. */
    if (wordTok->type == TCL_TOKEN_WORD && wordTok->numComponents > 1)
    {
        int allLiteral = 1;
        for (Tcl_Size i = 0; i < wordTok->numComponents; i++)
        {
            const Tcl_Token* sub = wordTok + 1 + i;
            if (sub->type != TCL_TOKEN_TEXT && sub->type != TCL_TOKEN_BS)
            {
                allLiteral = 0;
                break;
            }
        }
        if (allLiteral)
        {
            Tcl_DString ds;
            Tcl_DStringInit(&ds);
            for (Tcl_Size i = 0; i < wordTok->numComponents; i++)
            {
                const Tcl_Token* sub = wordTok + 1 + i;
                if (sub->type == TCL_TOKEN_TEXT)
                {
                    Tcl_DStringAppend(&ds, sub->start, sub->size);
                }
                else /* TCL_TOKEN_BS — resolve backslash sequence */
                {
                    char buf[8];
                    int count = 0;
                    int resultLen = Tcl_UtfBackslash(sub->start, &count, buf);
                    Tcl_DStringAppend(&ds, buf, resultLen);
                }
            }
            Tcl_Obj* o = Tcl_NewStringObj(Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
            Tcl_DStringFree(&ds);
            Tcl_IncrRefCount(o);
            return o;
        }
    }
    /* Fallback: SIMPLE_WORD — but peel balanced braces if present */
    if (wordTok->type == TCL_TOKEN_SIMPLE_WORD)
    {
        const char* s = wordTok->start;
        Tcl_Size n = wordTok->size;
        if (n >= 2 && s[0] == '{' && s[n - 1] == '}')
        {
            Tcl_Obj* o = Tcl_NewStringObj(s + 1, n - 2);
            Tcl_IncrRefCount(o);
            return o;
        }
        Tcl_Obj* o = Tcl_NewStringObj(s, n);
        Tcl_IncrRefCount(o);
        return o;
    }
    /* Final fallback: braced content is always literal in Tcl — no
       substitutions inside braces.  If the raw word starts with '{' and
       ends with '}', extract the inner text regardless of token type.
       This handles Tcl 9.1 edge cases where large braced blocks parse
       with an unexpected token type (e.g. TCL_TOKEN_WORD with non-TEXT
       sub-tokens). */
    {
        const char* s = wordTok->start;
        Tcl_Size n = wordTok->size;
        if (n >= 2 && s[0] == '{' && s[n - 1] == '}')
        {
            Tcl_Obj* o = Tcl_NewStringObj(s + 1, n - 2);
            Tcl_IncrRefCount(o);
            return o;
        }
    }
    return NULL;
}

static inline const Tcl_Token* NextWord(const Tcl_Token* wordTok)
{
    return wordTok + 1 + wordTok->numComponents;
}

static inline const char* CmdCore(const char* s)
{
    if (!s)
        return "";
    while (s[0] == ':' && s[1] == ':')
        s += 2;
    return s;
}

static int CtxHasAuthoredCommand(TbcxCtx* ctx, ByteCode* bc, const char* commandName)
{
    if (!ctx || !ctx->authoredCommandsInit || !commandName || commandName[0] == '\0')
        return 0;

    if (commandName[0] == ':' && commandName[1] == ':')
    {
        return Tcl_FindHashEntry(&ctx->authoredCommands, commandName) != NULL;
    }

    const char* namespaceName = (bc && bc->nsPtr && bc->nsPtr->fullName) ? bc->nsPtr->fullName : "::";
    Tcl_DString key;
    Tcl_DStringInit(&key);
    Tcl_DStringAppend(&key, namespaceName, -1);
    if (strcmp(namespaceName, "::") != 0)
        Tcl_DStringAppend(&key, "::", 2);
    Tcl_DStringAppend(&key, commandName, -1);
    int found = Tcl_FindHashEntry(&ctx->authoredCommands, Tcl_DStringValue(&key)) != NULL;

    /* Bare commands fall back from the current namespace to the global
     * namespace.  A relative qualified command does not use that fallback. */
    if (!found && strchr(commandName, ':') == NULL && strcmp(namespaceName, "::") != 0)
    {
        Tcl_DStringSetLength(&key, 0);
        Tcl_DStringAppend(&key, "::", 2);
        Tcl_DStringAppend(&key, commandName, -1);
        found = Tcl_FindHashEntry(&ctx->authoredCommands, Tcl_DStringValue(&key)) != NULL;
    }
    Tcl_DStringFree(&key);
    return found;
}

static Tcl_Obj* FqnUnder(Tcl_Interp* ip, Tcl_Obj* curNs, Tcl_Obj* name)
{
    Tcl_Size nameLen = 0;
    const char* nm = Tbcx_GetStringFromObjStrict(ip, name, &nameLen);
    if (!nm)
        return NULL;
    if (nameLen > 0 && memchr(nm, '\0', (size_t)nameLen) != NULL)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: name contains embedded NUL"));
        return NULL;
    }
    if (nameLen >= 2 && nm[0] == ':' && nm[1] == ':')
    {
        Tcl_IncrRefCount(name); /* returned refcount = 1 */
        return name;
    }

    Tcl_Size nsLen = 0;
    const char* ns = Tbcx_GetStringFromObjStrict(ip, curNs, &nsLen);
    if (!ns)
        return NULL;
    if (nsLen > 0 && memchr(ns, '\0', (size_t)nsLen) != NULL)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: namespace contains embedded NUL"));
        return NULL;
    }

    Tcl_Obj* fqn = Tcl_NewStringObj(ns, nsLen); /* refcount 0 */
    Tcl_IncrRefCount(fqn);                      /* refcount 1 — own before mutation */
    if (!(nsLen == 2 && ns[0] == ':' && ns[1] == ':'))
        Tcl_AppendToObj(fqn, "::", 2);
    Tcl_AppendObjToObj(fqn, name);
    return fqn; /* returned refcount = 1 */
}

/* Scan method-definition option words (-export/-private/-unexport) and
 * return the resulting TBCX_MSCOPE_*.  `firstOpt` points at the first option
 * word; `nOpts` is how many words precede the trailing args/body.  Last
 * recognized option wins (matching TclOO's left-to-right application).
 * Returns TBCX_MSCOPE_DEFAULT when no recognized scope option is present. */
/* Return whether a `self` token is followed by the literal word `method`. */
static int SelfSubIsMethod(const Tcl_Token* wSelf)
{
    const Tcl_Token* wSub = NextWord(wSelf);
    if (!wSub)
        return 0;
    Tcl_Obj* o = WordLiteralObj(wSub);
    if (!o)
        return 0;
    int r = (strcmp(Tbcx_GetStringSafe(o), "method") == 0);
    Tcl_DecrRefCount(o);
    return r;
}

static int MethodScopeFromOptionWords(const Tcl_Token* firstOpt, Tcl_Size nOpts)
{
    int scope = TBCX_MSCOPE_DEFAULT;
    const Tcl_Token* t = firstOpt;
    for (Tcl_Size i = 0; i < nOpts; i++)
    {
        Tcl_Obj* w = WordLiteralObj(t);
        if (w)
        {
            const char* s = Tbcx_GetStringSafe(w);
            if (strcmp(s, "-export") == 0)
                scope = TBCX_MSCOPE_PUBLIC;
            else if (strcmp(s, "-unexport") == 0)
                scope = TBCX_MSCOPE_UNEXPORTED;
            else if (strcmp(s, "-private") == 0)
                scope = TBCX_MSCOPE_TRUE_PRIVATE;
            Tcl_DecrRefCount(w);
        }
        t = NextWord(t);
    }
    return scope;
}

/* Fold a same-body export/unexport (or self-export/self-unexport) command
 * into the scope of the most-recently captured matching method.  `wantSelf`
 * selects self vs ordinary methods.  `names`/`nNames` are the method names
 * the command targets.  Updates the LAST matching DefRec (the method's own
 * definition), so a definition option is overridden by a later export. */
static void FoldExportScope(
    DefVec* defs, Tcl_Obj* clsFqn, const Tcl_Token* firstName, Tcl_Size nNames, int wantSelf, int newScope, Tcl_Size foldStartIdx)
{
    Tcl_Size fqnLen = 0;
    const char* fqn = Tbcx_GetStringFromObjSafe(clsFqn, &fqnLen);
    const Tcl_Token* t = firstName;
    for (Tcl_Size i = 0; i < nNames; i++)
    {
        Tcl_Obj* nm = WordLiteralObj(t);
        if (nm)
        {
            Tcl_Size nLen = 0;
            const char* ns = Tbcx_GetStringFromObjSafe(nm, &nLen);
            for (Tcl_Size d = defs->n - 1; d >= 0; d--)
            {
                DefRec* r = &defs->v[d];
                if (r->kind != DEF_KIND_INST && r->kind != DEF_KIND_CLASS)
                    continue;
                int isSelf = (r->flags & DEF_F_SELF_METHOD) ? 1 : 0;
                if (isSelf != (wantSelf ? 1 : 0))
                    continue;
                /* A non-self (class-instance/class) `export`/`unexport` STATEMENT
                   may only fold into a method defined by the SAME builder
                   (records at index >= foldStartIdx).  An export in a SEPARATE
                   `oo::define` command is order-dependent: applying it to an
                   earlier builder's method record would export the method at its
                   definition time, before the export statement actually runs, so
                   an observation taken between the two commands would see the
                   wrong visibility.  Such cross-builder exports instead apply at
                   the export command's own position — the statement is forwarded
                   verbatim in the stubbed builder and the post-swap visibility
                   restore (PrecompClass) preserves it.  Self methods have no such
                   restore path, so they keep the whole-history fold. */
                if (!wantSelf && d < foldStartIdx)
                    continue;
                Tcl_Size cLen = 0;
                const char* cs = Tbcx_GetStringFromObjSafe(r->cls, &cLen);
                if (!(cLen == fqnLen && memcmp(cs, fqn, (size_t)fqnLen) == 0))
                    continue;
                Tcl_Size mLen = 0;
                const char* ms = Tbcx_GetStringFromObjSafe(r->name, &mLen);
                if (mLen == nLen && memcmp(ms, ns, (size_t)nLen) == 0)
                {
                    r->scope = newScope;
                    break;
                }
            }
            Tcl_DecrRefCount(nm);
        }
        t = NextWord(t);
    }
}

static void CaptureClassBody(Tcl_Interp* ip,
                             const char* script,
                             Tcl_Size len,
                             Tcl_Obj* curNs,
                             Tcl_Obj* clsFqn,
                             DefVec* defs,
                             ClsSet* classes,
                             int flags,
                             int depth,
                             int ctxFlags,
                             Tcl_Size foldStartIdx)
{
    /* foldStartIdx is the defs index where THIS builder's records begin (passed
       by the top-level caller as defs->n, then threaded unchanged through nested
       private/self recursion), so a class export/unexport statement folds only
       into methods this same builder defined. */
    /* Guard against unbounded recursion via nested private blocks */
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return;
    int lexPrivate = (ctxFlags & CAP_CTX_PRIVATE) ? 1 : 0;
    int lexSelf = (ctxFlags & CAP_CTX_SELF) ? 1 : 0;
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = (len >= 0 ? len : (Tcl_Size)strlen(script));
    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    /* Name is always the word after "method"/"classmethod".
                       Args and body are always the last two words.
                       Option flags (-export/-private/-unexport) sit between
                       name and args; resolve them into the method scope. */
                    const Tcl_Token* wN = NextWord(w0);
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                        t = NextWord(t);
                    const Tcl_Token* wA = t;
                    const Tcl_Token* wB = NextWord(wA);
                    Tcl_Obj* mname = WordLiteralObj(wN);
                    Tcl_Obj* args = WordLiteralObj(wA);
                    Tcl_Obj* body = WordLiteralObj(wB);
                    /* options are the words between name and args:
                       numWords - (keyword + name + args + body) = numWords-4 */
                    int optScope = MethodScopeFromOptionWords(NextWord(wN), p.numWords - 4);
                    if (mname && args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        /* A method/classmethod inside `self { ... }` is a self
                           method.  classmethod is never a self method. */
                        int asSelf = (lexSelf && strcmp(kw, "classmethod") != 0) ? 1 : 0;
                        r.kind = (strcmp(kw, "classmethod") == 0) ? DEF_KIND_CLASS : DEF_KIND_INST;
                        if (asSelf)
                            r.kind = DEF_KIND_CLASS; /* wire-kind set to SELF via flag below */
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.name = mname; /* transferred from WordLiteralObj (refcount 1) */
                        r.args = args;  /* transferred from WordLiteralObj (refcount 1) */
                        r.body = body;  /* transferred from WordLiteralObj (refcount 1) */
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        r.flags = flags | (asSelf ? DEF_F_SELF_METHOD : 0);
                        /* Explicit option wins; else lexical `private` makes
                           it true-private; else name-default. */
                        r.scope = (optScope != TBCX_MSCOPE_DEFAULT)
                                      ? optScope
                                      : (lexPrivate ? TBCX_MSCOPE_TRUE_PRIVATE : TBCX_MSCOPE_DEFAULT);
                        DV_Push(defs, r);
                        CS_Add(classes, clsFqn);
                    }
                    else
                    {
                        if (mname)
                            Tcl_DecrRefCount(mname);
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                }
                else if (strcmp(kw, "constructor") == 0 && p.numWords >= 2)
                {
                    /* Last two words are args/body; allow the {body}-only form too */
                    const Tcl_Token *wArgs = NULL, *wBody = NULL;
                    const Tcl_Token* tokWalk = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                        tokWalk = NextWord(tokWalk);
                    wBody = tokWalk;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj* args = wArgs ? WordLiteralObj(wArgs) : NULL;
                    if (!args)
                    {
                        /* Fallback: empty arg spec.  IncrRefCount so all
                           paths hold a refcount-1 owned ref, matching
                           WordLiteralObj's return convention. */
                        args = Tcl_NewStringObj("", 0);
                        Tcl_IncrRefCount(args);
                    }
                    Tcl_Obj* body = WordLiteralObj(wBody);
                    /* Coerce constructor arg-spec to a list value (matches proc/method handling). */
                    if (args)
                    {
                        Tcl_Size _tbcx_dummy;
                        if (Tcl_ListObjLength(ip, args, &_tbcx_dummy) != TCL_OK)
                        {
                            Tcl_ResetResult(ip);
                            Tcl_DecrRefCount(args);
                            args = NULL; /* skip capture */
                        }
                    }
                    if (args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_CTOR;
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.args = args; /* transferred (refcount 1) */
                        r.body = body; /* transferred from WordLiteralObj (refcount 1) */
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        r.flags = flags;
                        DV_Push(defs, r);
                        CS_Add(classes, clsFqn);
                    }
                    else
                    {
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                }
                else if (strcmp(kw, "destructor") == 0 && p.numWords >= 1)
                {
                    /* Destructor may be "… destructor {body}" or "… destructor {args} {body}" */
                    const Tcl_Token* tokWalk = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                        tokWalk = NextWord(tokWalk);
                    const Tcl_Token* wBody = tokWalk;
                    Tcl_Obj* args = NULL;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                            pre = NextWord(pre);
                        args = WordLiteralObj(pre);
                    }
                    else
                    {
                        args = Tcl_NewStringObj("", 0);
                        Tcl_IncrRefCount(args);
                    }
                    Tcl_Obj* body = WordLiteralObj(wBody);
                    /* Keep destructor arg list canonical too for consistency. */
                    if (args)
                    {
                        Tcl_Size _tbcx_dummy;
                        if (Tcl_ListObjLength(ip, args, &_tbcx_dummy) != TCL_OK)
                        {
                            Tcl_ResetResult(ip);
                            Tcl_DecrRefCount(args);
                            args = NULL;
                        }
                    }
                    if (args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_DTOR;
                        r.name = NULL; /* destructors have no name */
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.args = args; /* transferred (refcount 1) */
                        r.body = body; /* transferred from WordLiteralObj (refcount 1) */
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        r.flags = flags;
                        DV_Push(defs, r);
                        CS_Add(classes, clsFqn);
                    }
                    else
                    {
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                }
                /* "private { script }" -- recurse into the block to capture
                   methods/ctors/dtors defined in private (true-private)
                   context.  "private method/classmethod ..." (inline form,
                   numWords>=3) is captured directly here so its body is
                   stubbed rather than leaked verbatim. */
                else if (strcmp(kw, "private") == 0 && p.numWords == 2)
                {
                    const Tcl_Token* wBlock = NextWord(w0);
                    Tcl_Obj* block = WordLiteralObj(wBlock);
                    if (block)
                    {
                        Tcl_Size bkLen = 0;
                        const char* bkStr = Tbcx_GetStringFromObjSafe(block, &bkLen);
                        CaptureClassBody(ip,
                                         bkStr,
                                         bkLen,
                                         curNs,
                                         clsFqn,
                                         defs,
                                         classes,
                                         flags,
                                         depth + 1,
                                         ctxFlags | CAP_CTX_PRIVATE,
                                         foldStartIdx);
                        Tcl_DecrRefCount(block);
                    }
                }
                else if (strcmp(kw, "private") == 0 && p.numWords >= 3)
                {
                    /* Inline "private method NAME ?opts? ARGS BODY" (or
                       classmethod/self method) — recurse over the tail so the
                       same option/lexical/self handling applies, with the
                       true-private lexical context set. */
                    const char* raw = p.commandStart;
                    Tcl_Size rawLen = p.commandSize;
                    /* Skip the leading "private" word + following whitespace to
                       get the inner command text. */
                    const char* inner = raw;
                    Tcl_Size skip = 0;
                    while (skip < rawLen && inner[skip] != ' ' && inner[skip] != '\t')
                        skip++;
                    while (skip < rawLen && (inner[skip] == ' ' || inner[skip] == '\t'))
                        skip++;
                    if (skip < rawLen)
                        CaptureClassBody(ip,
                                         inner + skip,
                                         rawLen - skip,
                                         curNs,
                                         clsFqn,
                                         defs,
                                         classes,
                                         flags,
                                         depth + 1,
                                         ctxFlags | CAP_CTX_PRIVATE,
                                         foldStartIdx);
                }
                /* "self { script }" block -- recurse with self context so the
                   inner method/export commands are captured as self methods,
                   never leaked verbatim. */
                else if (strcmp(kw, "self") == 0 && p.numWords == 2)
                {
                    const Tcl_Token* wBlock = NextWord(w0);
                    Tcl_Obj* block = WordLiteralObj(wBlock);
                    if (block)
                    {
                        Tcl_Size bkLen = 0;
                        const char* bkStr = Tbcx_GetStringFromObjSafe(block, &bkLen);
                        CaptureClassBody(ip,
                                         bkStr,
                                         bkLen,
                                         curNs,
                                         clsFqn,
                                         defs,
                                         classes,
                                         flags,
                                         depth + 1,
                                         ctxFlags | CAP_CTX_SELF,
                                         foldStartIdx);
                        Tcl_DecrRefCount(block);
                    }
                }
                /* "self method NAME ?opts? ARGS BODY" -- capture the body so it
                   can be stubbed, preventing source leakage, with scope. */
                else if (strcmp(kw, "self") == 0 && p.numWords >= 5)
                {
                    const Tcl_Token* wSub = NextWord(w0);
                    Tcl_Obj* subCmd = WordLiteralObj(wSub);
                    if (subCmd)
                    {
                        const char* subStr = Tbcx_GetStringSafe(subCmd);
                        if (strcmp(subStr, "method") == 0)
                        {
                            const Tcl_Token* wN = NextWord(wSub);
                            /* Args and body are the last two words */
                            const Tcl_Token* t = w0;
                            for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                                t = NextWord(t);
                            const Tcl_Token* wA = t;
                            const Tcl_Token* wB = NextWord(wA);
                            Tcl_Obj* mname = WordLiteralObj(wN);
                            Tcl_Obj* args = WordLiteralObj(wA);
                            Tcl_Obj* body = WordLiteralObj(wB);
                            /* options between name and args:
                               numWords - (self + method + name + args + body) */
                            int optScope = MethodScopeFromOptionWords(NextWord(wN), p.numWords - 5);
                            if (mname && args && body)
                            {
                                DefRec r;
                                memset(&r, 0, sizeof(r));
                                r.kind = DEF_KIND_CLASS;
                                r.cls = clsFqn;
                                Tcl_IncrRefCount(r.cls);
                                r.name = mname;
                                r.args = args;
                                r.body = body;
                                r.ns = curNs;
                                Tcl_IncrRefCount(r.ns);
                                r.flags = flags | DEF_F_SELF_METHOD;
                                r.scope = (optScope != TBCX_MSCOPE_DEFAULT)
                                              ? optScope
                                              : (lexPrivate ? TBCX_MSCOPE_TRUE_PRIVATE : TBCX_MSCOPE_DEFAULT);
                                DV_Push(defs, r);
                                CS_Add(classes, clsFqn);
                            }
                            else
                            {
                                if (mname)
                                    Tcl_DecrRefCount(mname);
                                if (args)
                                    Tcl_DecrRefCount(args);
                                if (body)
                                    Tcl_DecrRefCount(body);
                            }
                        }
                        else if ((strcmp(subStr, "export") == 0 || strcmp(subStr, "unexport") == 0) && p.numWords >= 3)
                        {
                            /* self export/unexport NAME... — fold into the
                               captured self method's scope (the self path has
                               no verbatim-execution fallback). */
                            int sc = (strcmp(subStr, "export") == 0) ? TBCX_MSCOPE_PUBLIC : TBCX_MSCOPE_UNEXPORTED;
                            FoldExportScope(defs, clsFqn, NextWord(wSub), p.numWords - 2, 1, sc, foldStartIdx);
                        }
                        Tcl_DecrRefCount(subCmd);
                    }
                }
                /* "self export/unexport NAME" with exactly 3 words is handled
                   by the >=5 branch's else-if only when numWords>=5; cover the
                   3/4-word case here. */
                else if (strcmp(kw, "self") == 0 && (p.numWords == 3 || p.numWords == 4))
                {
                    const Tcl_Token* wSub = NextWord(w0);
                    Tcl_Obj* subCmd = WordLiteralObj(wSub);
                    if (subCmd)
                    {
                        const char* subStr = Tbcx_GetStringSafe(subCmd);
                        if (strcmp(subStr, "export") == 0 || strcmp(subStr, "unexport") == 0)
                        {
                            int sc = (strcmp(subStr, "export") == 0) ? TBCX_MSCOPE_PUBLIC : TBCX_MSCOPE_UNEXPORTED;
                            FoldExportScope(defs, clsFqn, NextWord(wSub), p.numWords - 2, 1, sc, foldStartIdx);
                        }
                        Tcl_DecrRefCount(subCmd);
                    }
                }
                /* In a self-context block, bare "export/unexport NAME" targets
                   self methods. */
                else if (lexSelf && (strcmp(kw, "export") == 0 || strcmp(kw, "unexport") == 0) && p.numWords >= 2)
                {
                    int sc = (strcmp(kw, "export") == 0) ? TBCX_MSCOPE_PUBLIC : TBCX_MSCOPE_UNEXPORTED;
                    FoldExportScope(defs, clsFqn, NextWord(w0), p.numWords - 1, 1, sc, foldStartIdx);
                }
                /* Ordinary (non-self) "export/unexport NAME..." — fold into the
                   captured instance/class method scope. */
                else if ((strcmp(kw, "export") == 0 || strcmp(kw, "unexport") == 0) && p.numWords >= 2)
                {
                    int sc = (strcmp(kw, "export") == 0) ? TBCX_MSCOPE_PUBLIC : TBCX_MSCOPE_UNEXPORTED;
                    FoldExportScope(defs, clsFqn, NextWord(w0), p.numWords - 1, 0, sc, foldStartIdx);
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
}

/* EmitFlatSelfStubs — emit `oo::define CLS self method NAME ARGS ;` for every
 * captured self method of clsFqn.  Self methods MUST be installed via these
 * flat top-level stubs (the load side's create-then-swap requires the self
 * method to appear as a standalone oo::define call, not nested in a builder
 * block), so the same emission is used by both the pure (StubLinesForClass)
 * and non-pure (StubbedBuilderBody) class-rewrite paths.  The visibility scope
 * travels in the method-record scope byte and is applied at load time. */
static void EmitFlatSelfStubs(Tcl_Interp* ip, Tcl_DString* out, DefVec* defs, Tcl_Obj* clsFqn)
{
    (void)ip;
    Tcl_Size fqnLen = 0;
    const char* cls = Tbcx_GetStringFromObjSafe(clsFqn, &fqnLen);
    for (Tcl_Size i = 0; i < defs->n; i++)
    {
        DefRec* r = &defs->v[i];
        if (!(r->flags & DEF_F_SELF_METHOD))
            continue;
        if ((r->flags & DEF_F_FROM_BUILDER) == 0)
            continue;
        if (!(r->kind == DEF_KIND_INST || r->kind == DEF_KIND_CLASS))
            continue;
        if (r->emitted)
            continue; /* already written by an earlier class-rewrite call */
        Tcl_Size thisLen = 0;
        const char* thisCls = Tbcx_GetStringFromObjSafe(r->cls, &thisLen);
        if (!(thisLen == fqnLen && memcmp(thisCls, cls, (size_t)fqnLen) == 0))
            continue;
        r->emitted = 1;
        Tcl_Size tmp;
        Tcl_DString ln;
        Tcl_DStringInit(&ln);
        Tcl_DStringAppendElement(&ln, "oo::define");
        Tcl_DStringAppendElement(&ln, cls);
        Tcl_DStringAppendElement(&ln, "self");
        Tcl_DStringAppendElement(&ln, "method");
        Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->name, &tmp));
        Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->args, &tmp));
        /* Stub body: the sentinel, uniform with every other method form, so the
           loader's "patch iff forwarded body == sentinel" gate recognises it and
           never mistakes a verbatim self method for a stub. */
        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
        Tcl_DStringAppend(out, "\n", 1);
        Tcl_DStringFree(&ln);
    }
}

/* EmitSelfDeclaratives — for each `self <subcmd> ...` (or `self { block }`)
 * command in the class body, emit the DECLARATIVE self commands as
 * `oo::define CLS self <cmd>` lines.  Self method/export/unexport are NOT
 * emitted (methods become flat stubs via EmitFlatSelfStubs; export/unexport
 * are folded into the captured method's scope byte), so the original body's
 * self-method bodies never reach the rewritten script — closing the source
 * leak while preserving self mixin/variable/filter/forward/class side effects. */
static void EmitSelfDeclaratives(Tcl_Interp* ip, Tcl_DString* out, Tcl_Obj* clsFqn, const char* body, Tcl_Size bodyLen)
{
    if (!body || bodyLen <= 0)
        return;
    const char* cls = Tbcx_GetStringSafe(clsFqn);
    Tcl_Parse p;
    const char* cur = body;
    Tcl_Size remain = bodyLen;
    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 2)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd && strcmp(CmdCore(Tbcx_GetStringSafe(cmd)), "self") == 0)
            {
                const Tcl_Token* wSub = NextWord(w0);
                Tcl_Obj* subObj = wSub ? WordLiteralObj(wSub) : NULL;
                const char* sub = subObj ? Tbcx_GetStringSafe(subObj) : "";
                if (p.numWords == 2)
                {
                    /* self { block } — re-emit non-method/export declaratives. */
                    Tcl_Obj* block = WordLiteralObj(wSub);
                    if (block)
                    {
                        Tcl_Size blkLen = 0;
                        const char* blk = Tbcx_GetStringFromObjSafe(block, &blkLen);
                        Tcl_Parse bp;
                        const char* bc = blk;
                        Tcl_Size br = blkLen;
                        while (br > 0)
                        {
                            if (Tcl_ParseCommand(ip, bc, br, 0, &bp) != TCL_OK)
                            {
                                Tcl_FreeParse(&bp);
                                break;
                            }
                            if (bp.numWords >= 1)
                            {
                                const Tcl_Token* bw0 = bp.tokenPtr;
                                if (bw0->type == TCL_TOKEN_COMMAND)
                                    bw0++;
                                Tcl_Obj* bcmd = WordLiteralObj(bw0);
                                const char* bk = bcmd ? CmdCore(Tbcx_GetStringSafe(bcmd)) : "";
                                if (bcmd && strcmp(bk, "method") != 0 && strcmp(bk, "export") != 0 && strcmp(bk, "unexport") != 0)
                                {
                                    const char* rs = bp.commandStart;
                                    Tcl_Size rl = bp.commandSize;
                                    while (rl > 0 && (rs[rl - 1] == '\n' || rs[rl - 1] == '\r' || rs[rl - 1] == ' ' ||
                                                      rs[rl - 1] == '\t' || rs[rl - 1] == ';'))
                                        rl--;
                                    if (rl > 0)
                                    {
                                        Tcl_DString ln;
                                        Tcl_DStringInit(&ln);
                                        Tcl_DStringAppendElement(&ln, "oo::define");
                                        Tcl_DStringAppendElement(&ln, cls);
                                        Tcl_DStringAppendElement(&ln, "self");
                                        Tcl_DStringAppend(&ln, " ", 1);
                                        Tcl_DStringAppend(&ln, rs, rl);
                                        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                                        Tcl_DStringAppend(out, "\n", 1);
                                        Tcl_DStringFree(&ln);
                                    }
                                }
                                if (bcmd)
                                    Tcl_DecrRefCount(bcmd);
                            }
                            bc = bp.commandStart + bp.commandSize;
                            br = (blk + blkLen) - bc;
                            Tcl_FreeParse(&bp);
                        }
                        Tcl_DecrRefCount(block);
                    }
                }
                else if (strcmp(sub, "method") != 0 && strcmp(sub, "export") != 0 && strcmp(sub, "unexport") != 0)
                {
                    /* self <declarative> ... — emit `oo::define CLS self ...`. */
                    const char* rawStart = p.commandStart;
                    Tcl_Size rawLen = p.commandSize;
                    while (rawLen > 0 &&
                           (rawStart[rawLen - 1] == '\n' || rawStart[rawLen - 1] == '\r' || rawStart[rawLen - 1] == ' ' ||
                            rawStart[rawLen - 1] == '\t' || rawStart[rawLen - 1] == ';'))
                        rawLen--;
                    if (rawLen > 0)
                    {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "oo::define");
                        Tcl_DStringAppendElement(&ln, cls);
                        Tcl_DStringAppend(&ln, " ", 1);
                        Tcl_DStringAppend(&ln, rawStart, rawLen);
                        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                }
                if (subObj)
                    Tcl_DecrRefCount(subObj);
            }
            if (cmd)
                Tcl_DecrRefCount(cmd);
        }
        cur = p.commandStart + p.commandSize;
        remain = (body + bodyLen) - cur;
        Tcl_FreeParse(&p);
    }
    Tcl_ResetResult(ip);
}

static void StubLinesForClass(Tcl_Interp* ip, Tcl_DString* out, DefVec* defs, Tcl_Obj* clsFqn, const char* body, Tcl_Size bodyLen)
{
    Tcl_Size fqnLen = 0;
    const char* cls = Tbcx_GetStringFromObjSafe(clsFqn, &fqnLen);

    /* Pass 1: Scan the builder body for non-method OO commands
       (variable, superclass, mixin, filter, forward, export, unexport)
       and emit them verbatim as oo::define lines.  Uses Tcl_ParseCommand
       but only touches the command keyword — never iterates method
       args/body tokens.  Methods/ctor/dtor are emitted from DefVec in pass 2. */
    if (body && bodyLen > 0)
    {
        Tcl_Parse p;
        const char* cur = body;
        Tcl_Size remain = bodyLen;

        while (remain > 0)
        {
            if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
            {
                Tcl_FreeParse(&p);
                break;
            }
            if (p.numWords >= 2)
            {
                const Tcl_Token* w0 = p.tokenPtr;
                if (w0->type == TCL_TOKEN_COMMAND)
                    w0++;
                Tcl_Obj* cmd = WordLiteralObj(w0);
                if (cmd)
                {
                    const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                    /* Emit declarative, order-independent keywords here.
                     * EmitSelfDeclaratives and EmitFlatSelfStubs handle every
                     * `self` command. */
                    if (strcmp(kw, "variable") == 0 || strcmp(kw, "superclass") == 0 || strcmp(kw, "mixin") == 0 ||
                        strcmp(kw, "filter") == 0 || strcmp(kw, "forward") == 0)
                    {
                        /* Extract the raw command text from the source and
                           wrap it in oo::define cls <raw command>. */
                        Tcl_Size cmdLen = p.commandSize;
                        /* Trim trailing whitespace/newline from raw command text */
                        const char* rawStart = p.commandStart;
                        Tcl_Size rawLen = cmdLen;
                        while (rawLen > 0 &&
                               (rawStart[rawLen - 1] == '\n' || rawStart[rawLen - 1] == '\r' || rawStart[rawLen - 1] == ' ' ||
                                rawStart[rawLen - 1] == '\t' || rawStart[rawLen - 1] == ';'))
                            rawLen--;
                        if (rawLen > 0)
                        {
                            Tcl_Obj* rawCmd = Tcl_NewStringObj(rawStart, rawLen);
                            Tcl_IncrRefCount(rawCmd);
                            Tcl_DString ln;
                            Tcl_DStringInit(&ln);
                            Tcl_DStringAppendElement(&ln, "oo::define");
                            Tcl_DStringAppendElement(&ln, cls);
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rawCmd));
                            Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                            Tcl_DStringAppend(out, "\n", 1);
                            Tcl_DStringFree(&ln);
                            Tcl_DecrRefCount(rawCmd);
                        }
                    }
                    Tcl_DecrRefCount(cmd);
                }
            }
            cur = p.commandStart + p.commandSize;
            remain = (body + bodyLen) - cur;
            Tcl_FreeParse(&p);
        }
        Tcl_ResetResult(ip);
    }

    /* Pass 2: emit a flat stub line for each non-self method/ctor/dtor defined
       IN THIS builder body.  Parsing the body (rather than iterating the shared
       DefVec) keeps emission builder-scoped: a class touched by several
       commands does not re-stub an earlier command's methods, which would make
       the load side over-consume their precompiled FIFO records.  Self forms
       are skipped — EmitFlatSelfStubs emits them; `private` never appears (a
       pure builder rejects it).  Every stub body is the sentinel, uniform with
       all method forms, so the loader gates installation on "forwarded body ==
       sentinel"; the sentinel is non-empty, so a stubbed ctor/dtor is not
       treated as a deletion.  All names/args here are literals (a non-literal
       method already forced the whole builder verbatim before this runs).  Each
       statement's args are advisory — the loader installs from the precompiled
       record's args, not the stub's. */
    if (body && bodyLen > 0)
    {
        Tcl_Parse p;
        const char* cur = body;
        Tcl_Size remain = bodyLen;
        while (remain > 0)
        {
            if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
            {
                Tcl_FreeParse(&p);
                break;
            }
            if (p.numWords >= 1)
            {
                const Tcl_Token* w0 = p.tokenPtr;
                if (w0->type == TCL_TOKEN_COMMAND)
                    w0++;
                Tcl_Obj* cmd = WordLiteralObj(w0);
                if (cmd)
                {
                    const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                    int emit = 0;
                    Tcl_DString ln;
                    Tcl_DStringInit(&ln);
                    Tcl_DStringAppendElement(&ln, "oo::define");
                    Tcl_DStringAppendElement(&ln, cls);
                    if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                    {
                        const Tcl_Token* wN = NextWord(w0);
                        const Tcl_Token* t = w0;
                        for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                            t = NextWord(t);
                        Tcl_Obj* mname = WordLiteralObj(wN);
                        Tcl_Obj* args = WordLiteralObj(t);
                        if (mname && args)
                        {
                            Tcl_DStringAppendElement(&ln, kw);
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(mname));
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(args));
                            Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                            emit = 1;
                        }
                        if (mname)
                            Tcl_DecrRefCount(mname);
                        if (args)
                            Tcl_DecrRefCount(args);
                    }
                    else if (strcmp(kw, "constructor") == 0 && p.numWords >= 2)
                    {
                        const Tcl_Token* wArgs = NULL;
                        if (p.numWords >= 3)
                        {
                            const Tcl_Token* pre = w0;
                            for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                                pre = NextWord(pre);
                            wArgs = pre;
                        }
                        Tcl_Obj* args = wArgs ? WordLiteralObj(wArgs) : NULL;
                        Tcl_DStringAppendElement(&ln, "constructor");
                        Tcl_DStringAppendElement(&ln, args ? Tbcx_GetStringSafe(args) : "");
                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                        emit = 1;
                        if (args)
                            Tcl_DecrRefCount(args);
                    }
                    else if (strcmp(kw, "destructor") == 0 && p.numWords >= 2)
                    {
                        Tcl_DStringAppendElement(&ln, "destructor");
                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                        emit = 1;
                    }
                    if (emit)
                    {
                        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(out, "\n", 1);
                    }
                    Tcl_DStringFree(&ln);
                    Tcl_DecrRefCount(cmd);
                }
            }
            cur = p.commandStart + p.commandSize;
            remain = (body + bodyLen) - cur;
            Tcl_FreeParse(&p);
        }
        Tcl_ResetResult(ip);
    }

    /* Self declaratives (mixin/variable/filter/forward/class inside
       `self ...` or `self { ... }`) then flat self-method stubs.  Self method
       stubs are emitted LAST: the load-side Proc body swap for self methods is
       invalidated by subsequent oo::define epoch bumps, so nothing must follow
       them before the self methods run in the top-level bytecode. */
    EmitSelfDeclaratives(ip, out, clsFqn, body, bodyLen);
    EmitFlatSelfStubs(ip, out, defs, clsFqn);
}

static int IsPureOodefineBuilderBody(Tcl_Interp* ip, const char* script, Tcl_Size len)
{
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = (len >= 0 ? len : (Tcl_Size)strlen(script));

    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            /* Be conservative: on parse error, do not treat as pure. */
            Tcl_FreeParse(&p);
            return 0;
        }
        /* Empty command (possible with trivia); keep scanning. */
        if (p.numWords == 0)
        {
            cur = p.commandStart + p.commandSize;
            remain = (script + len) - cur;
            Tcl_FreeParse(&p);
            continue;
        }

        /* Step over TCL_TOKEN_COMMAND when present in 9.x so w0 is first word. */
        const Tcl_Token* w0 = p.tokenPtr;
        if (w0->type == TCL_TOKEN_COMMAND)
            w0++;

        /* If first word isn't a fixed literal, bail out. */
        Tcl_Obj* cmd = WordLiteralObj(w0);
        if (!cmd)
        {
            Tcl_FreeParse(&p);
            return 0;
        }
        const char* core = CmdCore(Tbcx_GetStringSafe(cmd));

        int ok = 0;
        if ((strcmp(core, "method") == 0 || strcmp(core, "classmethod") == 0))
        {
            /* Expect: method mname args body  (>=4 words) */
            ok = (p.numWords >= 4);
        }
        else if ((strcmp(core, "constructor") == 0 || strcmp(core, "destructor") == 0))
        {
            ok = (p.numWords >= 2);
        }
        else if (strcmp(core, "variable") == 0 || strcmp(core, "superclass") == 0 || strcmp(core, "mixin") == 0 ||
                 strcmp(core, "filter") == 0 || strcmp(core, "forward") == 0 || strcmp(core, "self") == 0)
        {
            /* These declarative OO builder commands are order-independent and
             * contain no method bodies. Method-name operations use
             * StubbedBuilderBody and retain source order. */
            ok = (p.numWords >= 2);
        }
        else if (strcmp(core, "private") == 0)
        {
            /* Builders with private blocks must use StubbedBuilderBody
               to preserve the private wrapper.  StubLinesForClass emits
               flat oo::define lines which lose the private context. */
            ok = 0;
        }
        else
        {
            ok = 0; /* any other subcommand breaks purity */
        }
        Tcl_DecrRefCount(cmd);
        if (!ok)
        {
            Tcl_FreeParse(&p);
            return 0;
        }

        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
    return 1;
}

/* IsPureObjdefineBuilderBody — purity predicate for an `oo::objdefine OBJ { … }`
 * braced builder body.  It is deliberately STRICTER than the class predicate
 * (IsPureOodefineBuilderBody): the only body-bearing form a pure object builder
 * may contain is a plain per-object method `method NAME ARGS BODY` with no
 * scope option.  When the predicate returns 1 the saver expands the block into
 * flat `oo::objdefine OBJ method NAME ARGS ""` statements (each precompiled,
 * stubbed, and name-case-scoped at load by the flat objdefine shim path).  When
 * it returns 0 the saver emits a single stubbed builder body that the loader
 * replays through PrecompObject, which patches the object's own method table
 * while honouring options, private scope, and export/unexport.
 *
 * Non-pure (route through StubbedBuilderBody + PrecompObject):
 *   - `method NAME -export|-unexport|-private ARGS BODY` (any scope option),
 *   - `private { … }` and inline `private method …`,
 *   - `export …` / `unexport …` (order- and existence-dependent),
 *   - `deletemethod …` / `renamemethod …`.
 * Treated as non-pure so TclOO raises the source-equivalent error during the
 * forwarded replay (these are not valid per-object body forms, never installed
 * as object methods):
 *   - `classmethod …`, `constructor …`, `destructor …`, `self method …`.
 * Declarative object commands that carry no method body and are order-
 * independent (`variable`, `mixin`, `filter`, `forward`, `class`) stay pure and
 * are copied verbatim by the expansion path. */
static int IsPureObjdefineBuilderBody(Tcl_Interp* ip, const char* script, Tcl_Size len)
{
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = (len >= 0 ? len : (Tcl_Size)strlen(script));

    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            /* Be conservative: on parse error, do not treat as pure. */
            Tcl_FreeParse(&p);
            return 0;
        }
        if (p.numWords == 0)
        {
            cur = p.commandStart + p.commandSize;
            remain = (script + len) - cur;
            Tcl_FreeParse(&p);
            continue;
        }

        const Tcl_Token* w0 = p.tokenPtr;
        if (w0->type == TCL_TOKEN_COMMAND)
            w0++;

        Tcl_Obj* cmd = WordLiteralObj(w0);
        if (!cmd)
        {
            Tcl_FreeParse(&p);
            return 0;
        }
        const char* core = CmdCore(Tbcx_GetStringSafe(cmd));

        int ok = 0;
        if (strcmp(core, "method") == 0)
        {
            /* Plain per-object method only: exactly `method NAME ARGS BODY`.
               A scope option sits between NAME and ARGS, lifting the word count
               above 4, which forces the non-pure (PrecompObject) path. */
            ok = (p.numWords == 4);
        }
        else if (strcmp(core, "variable") == 0 || strcmp(core, "mixin") == 0 || strcmp(core, "filter") == 0 ||
                 strcmp(core, "forward") == 0 || strcmp(core, "class") == 0)
        {
            /* Declarative, order-independent, no method body — copy verbatim. */
            ok = (p.numWords >= 2);
        }
        else
        {
            /* method-with-option, private, export, unexport, deletemethod,
               renamemethod, classmethod, constructor, destructor, self, and any
               unrecognised subcommand all break purity. */
            ok = 0;
        }
        Tcl_DecrRefCount(cmd);
        if (!ok)
        {
            Tcl_FreeParse(&p);
            return 0;
        }

        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
    return 1;
}

/* CollectMutationScan — walk a class/object builder body, recording every
 * method/classmethod NAME it defines (into methodNames) and every `forward`
 * NAME (into forwardNames), and raising *sawDelRename on any `deletemethod`/
 * `renamemethod`.  Recurses `private { … }`, inline `private …`, and `self { … }`
 * blocks and the `self method`/`self forward`/`self deletemethod` forms. */
static void CollectMutationScan(Tcl_Interp* ip,
                                const char* script,
                                Tcl_Size len,
                                Tcl_HashTable* methodNames,
                                Tcl_HashTable* forwardNames,
                                int* sawDelRename,
                                int depth)
{
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return;
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = (len >= 0 ? len : (Tcl_Size)strlen(script));
    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    Tcl_Obj* nm = WordLiteralObj(NextWord(w0));
                    if (nm)
                    {
                        int isNew;
                        Tcl_CreateHashEntry(methodNames, Tbcx_GetStringSafe(nm), &isNew);
                        Tcl_DecrRefCount(nm);
                    }
                }
                else if (strcmp(kw, "forward") == 0 && p.numWords >= 3)
                {
                    Tcl_Obj* nm = WordLiteralObj(NextWord(w0));
                    if (nm)
                    {
                        int isNew;
                        Tcl_CreateHashEntry(forwardNames, Tbcx_GetStringSafe(nm), &isNew);
                        Tcl_DecrRefCount(nm);
                    }
                }
                else if (strcmp(kw, "deletemethod") == 0 || strcmp(kw, "renamemethod") == 0)
                {
                    if (sawDelRename)
                        *sawDelRename = 1;
                }
                else if (strcmp(kw, "private") == 0 && p.numWords == 2)
                {
                    Tcl_Obj* blk = WordLiteralObj(NextWord(w0));
                    if (blk)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tbcx_GetStringFromObjSafe(blk, &bl);
                        CollectMutationScan(ip, bs, bl, methodNames, forwardNames, sawDelRename, depth + 1);
                        Tcl_DecrRefCount(blk);
                    }
                }
                else if (strcmp(kw, "private") == 0 && p.numWords >= 3)
                {
                    const char* raw = p.commandStart;
                    Tcl_Size rl = p.commandSize;
                    Tcl_Size skip = 0;
                    while (skip < rl && raw[skip] != ' ' && raw[skip] != '\t')
                        skip++;
                    while (skip < rl && (raw[skip] == ' ' || raw[skip] == '\t'))
                        skip++;
                    if (skip < rl)
                        CollectMutationScan(ip, raw + skip, rl - skip, methodNames, forwardNames, sawDelRename, depth + 1);
                }
                else if (strcmp(kw, "self") == 0 && p.numWords == 2)
                {
                    Tcl_Obj* blk = WordLiteralObj(NextWord(w0));
                    if (blk)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tbcx_GetStringFromObjSafe(blk, &bl);
                        CollectMutationScan(ip, bs, bl, methodNames, forwardNames, sawDelRename, depth + 1);
                        Tcl_DecrRefCount(blk);
                    }
                }
                else if (strcmp(kw, "self") == 0 && p.numWords >= 3)
                {
                    const Tcl_Token* wSub = NextWord(w0);
                    Tcl_Obj* sub = WordLiteralObj(wSub);
                    if (sub)
                    {
                        const char* ss = CmdCore(Tbcx_GetStringSafe(sub));
                        if ((strcmp(ss, "method") == 0 || strcmp(ss, "classmethod") == 0) && p.numWords >= 5)
                        {
                            Tcl_Obj* nm = WordLiteralObj(NextWord(wSub));
                            if (nm)
                            {
                                int isNew;
                                Tcl_CreateHashEntry(methodNames, Tbcx_GetStringSafe(nm), &isNew);
                                Tcl_DecrRefCount(nm);
                            }
                        }
                        else if (strcmp(ss, "forward") == 0 && p.numWords >= 4)
                        {
                            Tcl_Obj* nm = WordLiteralObj(NextWord(wSub));
                            if (nm)
                            {
                                int isNew;
                                Tcl_CreateHashEntry(forwardNames, Tbcx_GetStringSafe(nm), &isNew);
                                Tcl_DecrRefCount(nm);
                            }
                        }
                        else if (strcmp(ss, "deletemethod") == 0 || strcmp(ss, "renamemethod") == 0)
                        {
                            if (sawDelRename)
                                *sawDelRename = 1;
                        }
                        Tcl_DecrRefCount(sub);
                    }
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
}

/* BuilderMutatesOwnMethod — 1 if a class/object builder body DEFINES a method
 * and then deletes, renames, or forwards-over it within the SAME builder.  tbcx
 * replays method bodies positionally and (for pure builders) emits declaratives
 * around method stubs, then re-installs each precompiled method, so a
 * same-builder mutation of a just-defined method would be UNDONE at load (the
 * deleted/renamed/overridden method resurrected with its precompiled body).
 * When this returns 1 the caller leaves the whole builder verbatim so the
 * mutation runs in source order, matching `source`.  `deletemethod`/
 * `renamemethod` are method-mutating and rare, so any occurrence forces
 * verbatim; a `forward` is common (delegation to a different name), so it forces
 * verbatim only when its name collides with a method this same builder defines.
 * Separate-command mutations (a `deletemethod` in its own `oo::define`) are
 * unaffected by the reinstall and continue to round-trip whether or not this
 * fires. */
static int BuilderMutatesOwnMethod(Tcl_Interp* ip, const char* script, Tcl_Size len)
{
    Tcl_HashTable methodNames, forwardNames;
    Tcl_InitHashTable(&methodNames, TCL_STRING_KEYS);
    Tcl_InitHashTable(&forwardNames, TCL_STRING_KEYS);
    int sawDelRename = 0;
    CollectMutationScan(ip, script, len, &methodNames, &forwardNames, &sawDelRename, 0);
    int res = sawDelRename;
    if (!res)
    {
        Tcl_HashSearch s;
        Tcl_HashEntry* e;
        for (e = Tcl_FirstHashEntry(&forwardNames, &s); e && !res; e = Tcl_NextHashEntry(&s))
        {
            const char* fn = (const char*)Tcl_GetHashKey(&forwardNames, e);
            if (fn && Tcl_FindHashEntry(&methodNames, fn))
                res = 1;
        }
    }
    Tcl_DeleteHashTable(&methodNames);
    Tcl_DeleteHashTable(&forwardNames);
    return res;
}

/* ObjdefineBuilderHasNonLiteralMethod — return 1 if a braced class/object
 * builder must be left VERBATIM.  Two reasons: (a) any per-object/class method
 * has a NAME, arg-spec, or body that is NOT a static literal (e.g.
 * `method m {} [computeBody]` or `method m $argv {…}`) — it cannot be
 * precompiled, and the stub/expand paths would turn it into a no-op at load; or
 * (b) the builder defines a method and then mutates it in place (deletemethod /
 * renamemethod / forward-over-its-own-method), which the positional re-install
 * would otherwise undo (see BuilderMutatesOwnMethod).  When this returns 1 the
 * caller leaves the whole builder verbatim (bodies run as ordinary code at
 * load), matching how the flat `oo::objdefine OBJ method …` statement and the
 * runtime-variable-target builder are already handled.  Recurses through
 * `private { … }` and inline `private method …`. */
static int ObjdefineBuilderHasNonLiteralMethod(Tcl_Interp* ip, const char* script, Tcl_Size len, int depth)
{
    if (depth == 0 && BuilderMutatesOwnMethod(ip, script, len))
        return 1; /* same-builder method mutation — leave verbatim */
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return 1; /* too deep to introspect safely — treat as non-literal (verbatim) */
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = (len >= 0 ? len : (Tcl_Size)strlen(script));
    int found = 0;
    while (remain > 0 && !found)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    /* name = word after keyword; args = 2nd-last; body = last.
                       Capture requires all three to be literals (see
                       CaptureClassBody); a non-literal name (`method $m …`),
                       arg-spec, or body means the method cannot be captured, so
                       the whole builder must be left verbatim. */
                    Tcl_Obj* name = WordLiteralObj(NextWord(w0));
                    if (!name)
                        found = 1;
                    else
                        Tcl_DecrRefCount(name);
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                        t = NextWord(t);
                    Tcl_Obj* args = WordLiteralObj(t);
                    Tcl_Obj* body = WordLiteralObj(NextWord(t));
                    if (!args || !body)
                        found = 1;
                    if (args)
                        Tcl_DecrRefCount(args);
                    if (body)
                        Tcl_DecrRefCount(body);
                }
                else if (strcmp(kw, "private") == 0 && p.numWords == 2)
                {
                    Tcl_Obj* block = WordLiteralObj(NextWord(w0));
                    if (block)
                    {
                        Tcl_Size bl2 = 0;
                        const char* bs2 = Tbcx_GetStringFromObjSafe(block, &bl2);
                        if (ObjdefineBuilderHasNonLiteralMethod(ip, bs2, bl2, depth + 1))
                            found = 1;
                        Tcl_DecrRefCount(block);
                    }
                    else
                    {
                        found = 1; /* non-literal private block — cannot introspect */
                    }
                }
                else if (strcmp(kw, "private") == 0 && p.numWords >= 3)
                {
                    /* Inline `private method …` — recurse over the tail. */
                    const char* raw = p.commandStart;
                    Tcl_Size rl = p.commandSize;
                    Tcl_Size skip = 0;
                    while (skip < rl && raw[skip] != ' ' && raw[skip] != '\t')
                        skip++;
                    while (skip < rl && (raw[skip] == ' ' || raw[skip] == '\t'))
                        skip++;
                    if (skip < rl && ObjdefineBuilderHasNonLiteralMethod(ip, raw + skip, rl - skip, depth + 1))
                        found = 1;
                }
                else if (strcmp(kw, "constructor") == 0 && p.numWords >= 2)
                {
                    /* constructor ?ARGS? BODY — body is last; args 2nd-last if present. */
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                        t = NextWord(t);
                    Tcl_Obj* body = WordLiteralObj(t);
                    if (!body)
                        found = 1;
                    else
                        Tcl_DecrRefCount(body);
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                            pre = NextWord(pre);
                        Tcl_Obj* args = WordLiteralObj(pre);
                        if (!args)
                            found = 1;
                        else
                            Tcl_DecrRefCount(args);
                    }
                }
                else if (strcmp(kw, "destructor") == 0 && p.numWords >= 2)
                {
                    /* destructor BODY — body is the last word. */
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                        t = NextWord(t);
                    Tcl_Obj* body = WordLiteralObj(t);
                    if (!body)
                        found = 1;
                    else
                        Tcl_DecrRefCount(body);
                }
                else if (strcmp(kw, "self") == 0 && p.numWords == 2)
                {
                    /* self { block } — recurse into the self-context block. */
                    Tcl_Obj* block = WordLiteralObj(NextWord(w0));
                    if (block)
                    {
                        Tcl_Size bl2 = 0;
                        const char* bs2 = Tbcx_GetStringFromObjSafe(block, &bl2);
                        if (ObjdefineBuilderHasNonLiteralMethod(ip, bs2, bl2, depth + 1))
                            found = 1;
                        Tcl_DecrRefCount(block);
                    }
                    else
                    {
                        found = 1;
                    }
                }
                else if (strcmp(kw, "self") == 0 && p.numWords >= 5)
                {
                    /* self method NAME ?opts? ARGS BODY. */
                    const Tcl_Token* wSub = NextWord(w0);
                    Tcl_Obj* sub = WordLiteralObj(wSub);
                    if (sub)
                    {
                        if (strcmp(Tbcx_GetStringSafe(sub), "method") == 0)
                        {
                            Tcl_Obj* name = WordLiteralObj(NextWord(wSub));
                            if (!name)
                                found = 1;
                            else
                                Tcl_DecrRefCount(name);
                            const Tcl_Token* t = w0;
                            for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                                t = NextWord(t);
                            Tcl_Obj* args = WordLiteralObj(t);
                            Tcl_Obj* body = WordLiteralObj(NextWord(t));
                            if (!args || !body)
                                found = 1;
                            if (args)
                                Tcl_DecrRefCount(args);
                            if (body)
                                Tcl_DecrRefCount(body);
                        }
                        Tcl_DecrRefCount(sub);
                    }
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
    return found;
}

/* Build a builder body where every method/classmethod/constructor/destructor
 * subcommand has its body replaced by the empty string, while copying all
 * other subcommands verbatim. This ensures that load-time evaluation of the
 * builder will not compile real method bodies, but will still execute
 * superclass/mixin/export/variable/etc. side-effects. */
static Tcl_Obj* StubbedBuilderBody(Tcl_Interp* ip, Tcl_Obj* bodyObj)
{
    Tcl_Size blen = 0;
    const char* bstr = Tbcx_GetStringFromObjSafe(bodyObj, &blen);
    Tcl_DString out;
    Tcl_DStringInit(&out);
    Tcl_Parse p;
    const char* cur = bstr;
    Tcl_Size remain = blen;

    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* kw = CmdCore(Tbcx_GetStringSafe(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    /* Emit all words except the last (body), then append
                       an empty stub body.  This preserves the method name
                       and any option flags (-export/-private/-unexport)
                       between name and args. */
                    Tcl_DString ln;
                    Tcl_DStringInit(&ln);
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                    {
                        Tcl_Obj* word = WordLiteralObj(t);
                        if (word)
                        {
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(word));
                            Tcl_DecrRefCount(word);
                        }
                        t = NextWord(t);
                    }
                    Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY); /* stub sentinel */
                    Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                    Tcl_DStringAppend(&out, "\n", 1);
                    Tcl_DStringFree(&ln);
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                else if ((strcmp(kw, "constructor") == 0) && p.numWords >= 2)
                {
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 1 < p.numWords; w++)
                        t = NextWord(t);
                    const Tcl_Token* wArgs = NULL;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj* args = wArgs ? WordLiteralObj(wArgs) : NULL;
                    if (!args)
                    {
                        args = Tcl_NewStringObj("", 0);
                        Tcl_IncrRefCount(args);
                    }
                    if (args)
                    {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "constructor");
                        Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(args));
                        /* Sentinel stub body (uniform with every method form). */
                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    if (args)
                        Tcl_DecrRefCount(args);
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                else if (strcmp(kw, "destructor") == 0 && p.numWords >= 2)
                {
                    /* Destructor takes exactly one argument (body).
                       Emit "destructor {<sentinel>}" — uniform stub body. */
                    {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "destructor");
                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                else if (strcmp(kw, "private") == 0 && p.numWords >= 2)
                {
                    Tcl_Obj* stubbed = NULL;
                    if (p.numWords == 2)
                    {
                        /* Block form: private { script } — recurse into the
                           block and emit private { <stubbed> }. */
                        const Tcl_Token* wBlock = NextWord(w0);
                        Tcl_Obj* block = WordLiteralObj(wBlock);
                        if (block)
                        {
                            stubbed = StubbedBuilderBody(ip, block);
                            Tcl_DecrRefCount(block);
                        }
                    }
                    else
                    {
                        /* Inline form: private method/classmethod ... — stub
                           the body by recursing over the tail (everything after
                           the leading "private" word) so the method body never
                           leaks verbatim.  `private self method ...` is stripped
                           to a flat self stub by the recursion. */
                        const char* raw = p.commandStart;
                        Tcl_Size rl = p.commandSize;
                        Tcl_Size skip = 0;
                        while (skip < rl && raw[skip] != ' ' && raw[skip] != '\t')
                            skip++;
                        while (skip < rl && (raw[skip] == ' ' || raw[skip] == '\t'))
                            skip++;
                        if (skip < rl)
                        {
                            Tcl_Obj* tail = Tcl_NewStringObj(raw + skip, rl - skip);
                            Tcl_IncrRefCount(tail);
                            stubbed = StubbedBuilderBody(ip, tail);
                            Tcl_DecrRefCount(tail);
                        }
                    }
                    if (stubbed)
                    {
                        Tcl_Size sl = 0;
                        const char* ss = Tbcx_GetStringFromObjSafe(stubbed, &sl);
                        /* Emit the private wrapper only when it has real
                           content; an all-self block stubs to empty. */
                        Tcl_Size nb = sl;
                        while (nb > 0 && (ss[nb - 1] == ' ' || ss[nb - 1] == '\t' || ss[nb - 1] == '\n' || ss[nb - 1] == '\r' ||
                                          ss[nb - 1] == ';'))
                            nb--;
                        if (nb > 0)
                        {
                            Tcl_DString ln;
                            Tcl_DStringInit(&ln);
                            Tcl_DStringAppendElement(&ln, "private");
                            Tcl_DStringAppendElement(&ln, ss);
                            Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                            Tcl_DStringAppend(&out, "\n", 1);
                            Tcl_DStringFree(&ln);
                        }
                        Tcl_DecrRefCount(stubbed);
                    }
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                else if (strcmp(kw, "self") == 0)
                {
                    /* All self forms (self method / self { block } / self
                       export/unexport / self declaratives) are handled by
                       EmitFlatSelfStubs + EmitSelfDeclaratives at the caller,
                       so strip them here to avoid leaking self method bodies
                       and double-defining. */
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                Tcl_DecrRefCount(cmd);
            }
            /* Non-method subcommand: copy verbatim */
            Tcl_DStringAppend(&out, p.commandStart, p.commandSize);
        }
    next_cmd:
        cur = p.commandStart + p.commandSize;
        remain = (bstr + blen) - cur;
        Tcl_FreeParse(&p);
    }
    Tcl_Obj* res = Tcl_NewStringObj(Tcl_DStringValue(&out), Tcl_DStringLength(&out));
    Tcl_IncrRefCount(res); /* caller owns one reference */
    Tcl_DStringFree(&out);
    return res;
}

static Tcl_Obj* CanonTrivia(Tcl_Obj* in)
{
    if (!in)
    {
        Tcl_Obj* r = Tcl_NewStringObj("", 0);
        Tcl_IncrRefCount(r);
        return r;
    }
    Tcl_Size n = 0;
    const char* s = Tbcx_GetStringFromObjSafe(in, &n);
    if (n == 0)
    {
        Tcl_Obj* r = Tcl_NewStringObj("", 0);
        Tcl_IncrRefCount(r);
        return r;
    }

    Tcl_Size i = 0;
    Tcl_Size j = n;
    while (i < j)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            i++;
        }
        else
        {
            break;
        }
    }
    while (j > i)
    {
        unsigned char c = (unsigned char)s[j - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            j--;
        }
        else
        {
            break;
        }
    }
    if (j <= i)
    {
        Tcl_Obj* r = Tcl_NewStringObj("", 0);
        Tcl_IncrRefCount(r);
        return r;
    }
    /* Input and result have independent reference-count ownership, including
     * when no trimming occurs. */
    Tcl_Obj* r = Tcl_NewStringObj(s + i, j - i);
    Tcl_IncrRefCount(r);
    return r;
}

/* ==========================================================================
 * Recurse into control-flow script bodies
 *
 * Given a word token, extract its literal text, recurse through
 * CaptureAndRewriteScript to stub nested proc/method definitions,
 * and return the rewritten text.  Returns NULL if the token is not a
 * literal OR if the body text is unchanged (no definitions found).
 * ========================================================================== */
static Tcl_Obj*
RecurseScriptBody(Tcl_Interp* ip, const Tcl_Token* bodyTok, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth)
{
    if (!ip || !bodyTok || !curNs || !defs || !classes || depth > TBCX_MAX_BLOCK_DEPTH)
        return NULL;
    Tcl_Obj* bodyObj = WordLiteralObj(bodyTok);
    if (!bodyObj)
        return NULL;
    Tcl_Size bl = 0;
    const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
    if (bl < 4)
    { /* too short for "proc" */
        Tcl_DecrRefCount(bodyObj);
        return NULL;
    }
    Tcl_Obj* rewritten = CaptureAndRewriteScript(ip, bs, bl, curNs, defs, classes, depth);
    if (!rewritten)
    {
        Tcl_DecrRefCount(bodyObj);
        return NULL;
    }
    /* Only return non-NULL when the body was actually modified (i.e.
       proc/method/class definitions were found and stubbed).  Returning
       non-NULL for unchanged bodies triggers a command rebuild that
       can corrupt non-literal words like variable references. */
    Tcl_Size rl = 0;
    const char* rs = Tbcx_GetStringFromObjSafe(rewritten, &rl);
    if (rl == bl && memcmp(rs, bs, (size_t)bl) == 0)
    {
        /* Nothing changed — signal "no rewrite needed" */
        Tcl_DecrRefCount(rewritten);
        Tcl_DecrRefCount(bodyObj);
        return NULL;
    }
    Tcl_DecrRefCount(bodyObj);
    return rewritten;
}

/* ==========================================================================
 * RewriteCtx — bundles shared state for CaptureAndRewriteScript handlers.
 * Avoids passing 6+ parameters to each extracted handler function.
 * ========================================================================== */

typedef struct
{
    Tcl_Interp* ip;
    Tcl_DString* out;
    Tcl_Obj* curNs;
    DefVec* defs;
    ClsSet* classes;
    int depth;
} RewriteCtx;

/* EmitStandaloneDefineTail — handle a standalone (non-builder-block)
 * `oo::define CLS <tail>` whose <tail> defines or modifies methods (the
 * private/self forms, in inline `private method …`, block `private { … }`,
 * `self method …`, or block `self { … }` shape).  The tail is processed as a
 * one-command builder body — equivalent to `oo::define CLS { <tail> }` — so
 * method bodies are stubbed (no source leak) and the scope byte is set, while
 * self methods are emitted as flat stubs and self declaratives re-emitted.
 *
 * Returns 1 if the command was handled (captured + stubbed), or 0 to leave it
 * VERBATIM: a tail with a non-literal method/ctor/dtor/self name/args/body
 * cannot be precompiled, and stubbing it would silently drop the body — so the
 * caller emits the original command unchanged (every body runs at load, exactly
 * as `source` would), mirroring the braced-builder verbatim policy.  The check
 * runs BEFORE CaptureClassBody so no orphan records are captured for a builder
 * that ends up verbatim. */
static int EmitStandaloneDefineTail(RewriteCtx* ctx, Tcl_Obj* clsFqn, Tcl_Obj* cls, const char* tail, Tcl_Size tailLen)
{
    if (!ctx || !ctx->ip || !ctx->out || !ctx->curNs || !ctx->defs || !ctx->classes || !clsFqn || !cls || !tail || tailLen < 0)
        return 0;
    if (ObjdefineBuilderHasNonLiteralMethod(ctx->ip, tail, tailLen, 0))
        return 0; /* leave the whole standalone command verbatim */
    CaptureClassBody(ctx->ip, tail, tailLen, ctx->curNs, clsFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0, 0, ctx->defs->n);
    CS_Add(ctx->classes, clsFqn);
    if (IsPureOodefineBuilderBody(ctx->ip, tail, tailLen))
    {
        StubLinesForClass(ctx->ip, ctx->out, ctx->defs, clsFqn, tail, tailLen);
    }
    else
    {
        Tcl_Obj* tailObj = Tcl_NewStringObj(tail, tailLen);
        Tcl_IncrRefCount(tailObj);
        Tcl_Obj* stubbed = StubbedBuilderBody(ctx->ip, tailObj);
        Tcl_DString cmdLn;
        Tcl_DStringInit(&cmdLn);
        Tcl_DStringAppendElement(&cmdLn, "oo::define");
        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(cls));
        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(stubbed));
        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
        Tcl_DStringAppend(ctx->out, "\n", 1);
        Tcl_DStringFree(&cmdLn);
        Tcl_DecrRefCount(stubbed);
        EmitSelfDeclaratives(ctx->ip, ctx->out, clsFqn, tail, tailLen);
        EmitFlatSelfStubs(ctx->ip, ctx->out, ctx->defs, clsFqn);
        Tcl_DecrRefCount(tailObj);
    }
    return 1;
}

/* Slice the raw tail text of a command starting at the token `wFrom` through
 * the end of the parsed command, trimming trailing whitespace/`;`. */
static const char* CmdTailFromToken(Tcl_Parse* p, const Tcl_Token* wFrom, Tcl_Size* lenOut)
{
    const char* cmdEnd = p->commandStart + p->commandSize;
    const char* tail = wFrom->start;
    Tcl_Size tailLen = (Tcl_Size)(cmdEnd - tail);
    while (tailLen > 0 && (tail[tailLen - 1] == '\n' || tail[tailLen - 1] == '\r' || tail[tailLen - 1] == ' ' ||
                           tail[tailLen - 1] == '\t' || tail[tailLen - 1] == ';'))
        tailLen--;
    *lenOut = tailLen;
    return tail;
}

/* Handle all oo::define, oo::class, oo::objdefine, and metaclass commands.
 * Returns 1 if the command was handled, 0 otherwise.
 * The DString ctx->out is appended with the rewritten command text. */
static int RewriteOoCmd(RewriteCtx* ctx, Tcl_Parse* p, const Tcl_Token* w0, const char* c0, int* handled)
{
    if (!ctx || !ctx->ip || !ctx->out || !ctx->curNs || !ctx->defs || !ctx->classes || !p || !w0 || !c0 || !handled)
        return 0;
    /* Recover the command-word literal (needed by the metaclass handler).
       The caller guarantees w0 is a valid literal token. */
    Tcl_Obj* cmd = WordLiteralObj(w0);
    if (!cmd)
        return *handled;

    /* ---- oo::define cls subcommand ... (multi-word method/ctor/dtor) ---- */
    if (p->numWords >= 5 && strcmp(c0, "oo::define") == 0)
    {
        const Tcl_Token* wCls = NextWord(w0);
        Tcl_Obj* cls = WordLiteralObj(wCls);
        const Tcl_Token* wK = NextWord(wCls);
        Tcl_Obj* kwd = WordLiteralObj(wK);
        if (cls && kwd)
        {
            Tcl_Obj* clsFqn = FqnUnder(ctx->ip, ctx->curNs, cls);
            const char* kw = clsFqn ? Tbcx_GetStringSafe(kwd) : "";

            if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p->numWords >= 6)
            {
                /* Words: oo::define CLS method NAME ?-option? ARGS BODY.
                   The name is ALWAYS the word right after the keyword; args/body
                   are the last two; any option (-export/-private/-unexport) sits
                   between name and args and is resolved into the scope byte. */
                const Tcl_Token* wN = NextWord(wK);
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 2 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wA = tokP, *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
                /* options between name and args: numWords - (oo::define+cls+kw+name+args+body) */
                int optScope = MethodScopeFromOptionWords(NextWord(wN), p->numWords - 6);
                if (args)
                {
                    Tcl_Size _d;
                    if (Tcl_ListObjLength(ctx->ip, args, &_d) != TCL_OK)
                    {
                        Tcl_ResetResult(ctx->ip);
                        Tcl_DecrRefCount(args);
                        args = NULL;
                    }
                }
                if (mname && args && body)
                {
                    /* Capture */
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "classmethod") == 0 ? DEF_KIND_CLASS : DEF_KIND_INST);
                    r.cls = clsFqn;
                    Tcl_IncrRefCount(r.cls); /* borrowed: local DecrRefCount at scope end */
                    r.name = mname;          /* transferred from WordLiteralObj (refcount 1) */
                    r.args = args;           /* transferred from WordLiteralObj (refcount 1) */
                    r.body = body;           /* transferred from WordLiteralObj (refcount 1) */
                    r.ns = ctx->curNs;
                    r.scope = optScope;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: stub the body (option dropped — scope travels in
                       the scope byte and is re-applied at load by DefOO). */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::define");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(cls));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(kwd));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(mname));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    /* Sentinel stub body: the loader installs the precompiled
                       body only when the forwarded body equals it, so a verbatim
                       (non-literal-body) flat method is never patched from a
                       stale same-key record. */
                    Tcl_DStringAppendElement(&line, TBCX_METH_STUB_BODY);
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&line);
                    *handled = 1;
                }
                else
                {
                    if (mname)
                        Tcl_DecrRefCount(mname);
                    if (args)
                        Tcl_DecrRefCount(args);
                    if (body)
                        Tcl_DecrRefCount(body);
                }
            }
            else if ((strcmp(kw, "constructor") == 0 || strcmp(kw, "destructor") == 0) && p->numWords >= 4)
            {
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 1 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wBody = tokP, *wArgsTok = NULL;
                if (p->numWords >= 5)
                {
                    const Tcl_Token* pre = w0;
                    for (Tcl_Size w = 0; w + 2 < p->numWords; w++)
                        pre = NextWord(pre);
                    wArgsTok = pre;
                }
                Tcl_Obj* args = wArgsTok ? WordLiteralObj(wArgsTok) : NULL;
                if (!args)
                {
                    args = Tcl_NewStringObj("", 0);
                    Tcl_IncrRefCount(args);
                }
                Tcl_Obj* body = WordLiteralObj(wBody);
                if (args)
                {
                    Tcl_Size _d;
                    if (Tcl_ListObjLength(ctx->ip, args, &_d) != TCL_OK)
                    {
                        Tcl_ResetResult(ctx->ip);
                        Tcl_DecrRefCount(args);
                        args = NULL;
                    }
                }
                if (args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "constructor") == 0 ? DEF_KIND_CTOR : DEF_KIND_DTOR);
                    r.cls = clsFqn;
                    Tcl_IncrRefCount(r.cls); /* borrowed: local DecrRefCount at scope end */
                    r.args = args;           /* transferred (refcount 1) */
                    r.body = body;           /* transferred from WordLiteralObj (refcount 1) */
                    r.ns = ctx->curNs;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: stub */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::define");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(cls));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    /* Sentinel stub body (uniform with every method form). */
                    Tcl_DStringAppendElement(&line, TBCX_METH_STUB_BODY);
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&line);
                    *handled = 1;
                }
                else
                {
                    if (args)
                        Tcl_DecrRefCount(args);
                    if (body)
                        Tcl_DecrRefCount(body);
                }
            }
            /* Standalone body-bearing forms:
                 oo::define CLS self method NAME ?opt? ARGS BODY
                 oo::define CLS private method|classmethod|self method ... BODY
                 oo::define CLS private { ... }
               are equivalent to the one-command builder body
               `oo::define CLS { <tail> }`; route the tail through the same
               capture+stub machinery so the body is stubbed (no source leak)
               and the scope byte is set.  Bodiless self/private forms
               (self export/unexport, self mixin, …) are left to the verbatim
               fallback below — they carry no body to leak.  */
            else if (clsFqn && ((strcmp(kw, "self") == 0 && SelfSubIsMethod(wK)) || strcmp(kw, "private") == 0))
            {
                /* Standalone inline `oo::define CLS self method …` /
                   `oo::define CLS private method|classmethod … BODY` (numWords>=5
                   reach here; the bare-block numWords==4 forms are routed by the
                   dedicated branch below). */
                Tcl_Size tailLen = 0;
                const char* tail = CmdTailFromToken(p, wK, &tailLen);
                /* 0 == leave verbatim (non-literal method); caller emits the
                   original command unchanged. */
                *handled = EmitStandaloneDefineTail(ctx, clsFqn, cls, tail, tailLen);
            }
            if (clsFqn)
                Tcl_DecrRefCount(clsFqn);
            Tcl_DecrRefCount(cls);
            Tcl_DecrRefCount(kwd);
        }
        else
        {
            if (cls)
                Tcl_DecrRefCount(cls);
            if (kwd)
                Tcl_DecrRefCount(kwd);
        }
    }

    /* ---- oo::define cls private|self { block } / constructor|destructor body
            (bare-block / body-only standalone forms, numWords == 4) ---- */
    else if (!*handled && p->numWords == 4 && strcmp(c0, "oo::define") == 0)
    {
        const Tcl_Token* wCls = NextWord(w0);
        Tcl_Obj* cls = WordLiteralObj(wCls);
        const Tcl_Token* wK = wCls ? NextWord(wCls) : NULL;
        Tcl_Obj* kwd = wK ? WordLiteralObj(wK) : NULL;
        if (cls && kwd)
        {
            const char* kw2 = Tbcx_GetStringSafe(kwd);
            if (strcmp(kw2, "private") == 0 || strcmp(kw2, "self") == 0 || strcmp(kw2, "constructor") == 0 ||
                strcmp(kw2, "destructor") == 0)
            {
                Tcl_Obj* clsFqn = FqnUnder(ctx->ip, ctx->curNs, cls);
                if (clsFqn)
                {
                    Tcl_Size tailLen = 0;
                    const char* tail = CmdTailFromToken(p, wK, &tailLen);
                    /* 0 == leave verbatim (non-literal method/ctor/dtor body). */
                    *handled = EmitStandaloneDefineTail(ctx, clsFqn, cls, tail, tailLen);
                    Tcl_DecrRefCount(clsFqn);
                }
            }
        }
        if (cls)
            Tcl_DecrRefCount(cls);
        if (kwd)
            Tcl_DecrRefCount(kwd);
    }

    /* ---- oo::define cls { builder body } ---- */
    else if (!*handled && p->numWords == 3 && strcmp(c0, "oo::define") == 0)
    {
        const Tcl_Token* wCls = NextWord(w0);
        const Tcl_Token* wBod = wCls ? NextWord(wCls) : NULL;
        Tcl_Obj* cls = wCls ? WordLiteralObj(wCls) : NULL;
        Tcl_Obj* bod = wBod ? WordLiteralObj(wBod) : NULL;
        if (cls && bod)
        {
            Tcl_Obj* clsFqn = FqnUnder(ctx->ip, ctx->curNs, cls);
            if (clsFqn)
            {
                Tcl_Size bl = 0;
                const char* bs = Tbcx_GetStringFromObjSafe(bod, &bl);
                if (ObjdefineBuilderHasNonLiteralMethod(ctx->ip, bs, bl, 0))
                {
                    /* A method/ctor/dtor/self with a non-literal name/args/body
                       cannot be precompiled; stubbing it would silently drop the
                       body.  Leave the whole class builder VERBATIM (caller emits
                       the original command) so every body runs at load, exactly
                       as `source` would — at the cost of not precompiling this
                       one builder.  Mirrors the oo::objdefine handling. */
                    Tcl_DecrRefCount(clsFqn);
                }
                else
                {
                    /* Capture from builder body */
                    CaptureClassBody(
                        ctx->ip, bs, bl, ctx->curNs, clsFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0, 0, ctx->defs->n);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: check purity then stub */
                    if (IsPureOodefineBuilderBody(ctx->ip, bs, bl))
                    {
                        StubLinesForClass(ctx->ip, ctx->out, ctx->defs, clsFqn, bs, bl);
                    }
                    else
                    {
                        Tcl_Obj* stubbed = StubbedBuilderBody(ctx->ip, bod);
                        Tcl_DString cmdLn;
                        Tcl_DStringInit(&cmdLn);
                        Tcl_DStringAppendElement(&cmdLn, "oo::define");
                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(cls));
                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(stubbed));
                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                        Tcl_DStringAppend(ctx->out, "\n", 1);
                        Tcl_DStringFree(&cmdLn);
                        Tcl_DecrRefCount(stubbed);
                        /* Self forms were stripped from the stubbed body; emit them
                           as flat declarative lines + flat self-method stubs. */
                        EmitSelfDeclaratives(ctx->ip, ctx->out, clsFqn, bs, bl);
                        EmitFlatSelfStubs(ctx->ip, ctx->out, ctx->defs, clsFqn);
                    }
                    Tcl_DecrRefCount(clsFqn);
                    *handled = 1;
                } /* else: capturable builder */
            }
        }
        if (cls)
            Tcl_DecrRefCount(cls);
        if (bod)
            Tcl_DecrRefCount(bod);
    }

    /* ---- oo::class/oo::abstract/oo::singleton create name ?{builder}? ---- */
    else if (!*handled && p->numWords >= 3 &&
             (strcmp(c0, "oo::class") == 0 || strcmp(c0, "oo::abstract") == 0 || strcmp(c0, "oo::singleton") == 0))
    {
        const Tcl_Token* w1 = NextWord(w0);
        Tcl_Obj* sub = WordLiteralObj(w1);
        if (sub && strcmp(Tbcx_GetStringSafe(sub), "create") == 0)
        {
            const Tcl_Token* w2 = NextWord(w1);
            Tcl_Obj* nm = WordLiteralObj(w2);
            if (nm)
            {
                Tcl_Obj* clsFqn = FqnUnder(ctx->ip, ctx->curNs, nm);
                if (clsFqn)
                {
                    int leaveVerbatim = 0;
                    Tcl_DString cmdLn;
                    Tcl_DStringInit(&cmdLn);
                    Tcl_DStringAppendElement(&cmdLn, c0);
                    Tcl_DStringAppendElement(&cmdLn, "create");
                    Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(nm));
                    if (p->numWords >= 4)
                    {
                        const Tcl_Token* w3 = NextWord(w2);
                        Tcl_Obj* bod = WordLiteralObj(w3);
                        if (bod)
                        {
                            Tcl_Size bl = 0;
                            const char* bs = Tbcx_GetStringFromObjSafe(bod, &bl);
                            if (ObjdefineBuilderHasNonLiteralMethod(ctx->ip, bs, bl, 0))
                            {
                                /* Non-literal method/ctor/dtor/self body — leave the
                                   whole creation command verbatim (caller emits the
                                   original); nothing captured or stubbed. */
                                leaveVerbatim = 1;
                            }
                            else
                            {
                                CS_Add(ctx->classes, clsFqn);
                                CaptureClassBody(ctx->ip,
                                                 bs,
                                                 bl,
                                                 ctx->curNs,
                                                 clsFqn,
                                                 ctx->defs,
                                                 ctx->classes,
                                                 DEF_F_FROM_BUILDER,
                                                 0,
                                                 0,
                                                 ctx->defs->n);
                                if (IsPureOodefineBuilderBody(ctx->ip, bs, bl))
                                {
                                    Tcl_DStringAppendElement(&cmdLn, "");
                                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                    Tcl_DStringAppend(ctx->out, "\n", 1);
                                    StubLinesForClass(ctx->ip, ctx->out, ctx->defs, clsFqn, bs, bl);
                                }
                                else
                                {
                                    Tcl_Obj* stubbed = StubbedBuilderBody(ctx->ip, bod);
                                    Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(stubbed));
                                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                    Tcl_DStringAppend(ctx->out, "\n", 1);
                                    Tcl_DecrRefCount(stubbed);
                                    EmitSelfDeclaratives(ctx->ip, ctx->out, clsFqn, bs, bl);
                                    EmitFlatSelfStubs(ctx->ip, ctx->out, ctx->defs, clsFqn);
                                }
                            }
                            Tcl_DecrRefCount(bod);
                        }
                        else
                        {
                            CS_Add(ctx->classes, clsFqn);
                            Tcl_DStringAppendElement(&cmdLn, "");
                            Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                            Tcl_DStringAppend(ctx->out, "\n", 1);
                        }
                    }
                    else
                    {
                        CS_Add(ctx->classes, clsFqn);
                        Tcl_DStringAppendElement(&cmdLn, "");
                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                        Tcl_DStringAppend(ctx->out, "\n", 1);
                    }
                    Tcl_DStringFree(&cmdLn);
                    Tcl_DecrRefCount(clsFqn);
                    if (!leaveVerbatim)
                        *handled = 1;
                } /* if (clsFqn) */
                Tcl_DecrRefCount(nm);
            }
        }
        if (sub)
            Tcl_DecrRefCount(sub);
    }

    /* ---- $metaclass create name {builder} ----
     * Handles custom metaclass "create" commands where the
     * command name was already registered as a class earlier
     * in this script (e.g. CountingMeta create MyClass {body}).
     *
     * Only triggers when numWords is exactly 4 AND the 4th
     * word looks like an OO builder body (contains method/
     * constructor/destructor/variable/superclass keywords).
     * 5+ words indicates regular object creation
     * (ClassName create objName arg1 arg2 ...) and must NOT
     * be intercepted.  3-word forms are ambiguous and are
     * left to the existing oo::class create handler.
     */
    if (!*handled && p->numWords == 4 && ctx->classes->init)
    {
        const Tcl_Token* wSub = NextWord(w0);
        Tcl_Obj* sub = WordLiteralObj(wSub);
        if (sub && strcmp(Tbcx_GetStringSafe(sub), "create") == 0)
        {
            Tcl_Obj* cmdFqn = FqnUnder(ctx->ip, ctx->curNs, cmd);
            int isMetaCreate = 0;
            if (cmdFqn)
            {
                const char* cmdFqnStr = Tcl_GetString(cmdFqn);
                Tcl_HashEntry* clsHe = Tcl_FindHashEntry(&ctx->classes->ht, cmdFqnStr);
                if (clsHe)
                {
                    /* Verify the 4th word is a builder body, not a ctor arg.
                       Check if it contains any OO builder keyword when parsed. */
                    const Tcl_Token* w3 = NextWord(NextWord(wSub));
                    Tcl_Obj* probe = WordLiteralObj(w3);
                    if (probe)
                    {
                        Tcl_Size pl = 0;
                        const char* ps = Tbcx_GetStringFromObjSafe(probe, &pl);
                        Tcl_Parse pp;
                        const char* pcur = ps;
                        Tcl_Size prem = pl;
                        while (prem > 0 && !isMetaCreate)
                        {
                            if (Tcl_ParseCommand(ctx->ip, pcur, prem, 0, &pp) != TCL_OK)
                            {
                                Tcl_FreeParse(&pp);
                                break;
                            }
                            if (pp.numWords >= 1)
                            {
                                const Tcl_Token* pw = pp.tokenPtr;
                                if (pw->type == TCL_TOKEN_COMMAND)
                                    pw++;
                                Tcl_Obj* pcmd = WordLiteralObj(pw);
                                if (pcmd)
                                {
                                    const char* pk = CmdCore(Tbcx_GetStringSafe(pcmd));
                                    if (strcmp(pk, "method") == 0 || strcmp(pk, "classmethod") == 0 ||
                                        strcmp(pk, "constructor") == 0 || strcmp(pk, "destructor") == 0 ||
                                        strcmp(pk, "variable") == 0 || strcmp(pk, "superclass") == 0 ||
                                        strcmp(pk, "mixin") == 0 || strcmp(pk, "filter") == 0 || strcmp(pk, "forward") == 0 ||
                                        strcmp(pk, "private") == 0)
                                    {
                                        isMetaCreate = 1;
                                    }
                                    Tcl_DecrRefCount(pcmd);
                                }
                            }
                            pcur = pp.commandStart + pp.commandSize;
                            prem = (ps + pl) - pcur;
                            Tcl_FreeParse(&pp);
                        }
                        Tcl_ResetResult(ctx->ip);
                        Tcl_DecrRefCount(probe);
                    }
                } /* if (clsHe) */
            } /* if (cmdFqn) — skip meta-create optimization on FQN failure */
            Tcl_ResetResult(ctx->ip);
            if (isMetaCreate)
            {
                const Tcl_Token* w2 = NextWord(wSub);
                Tcl_Obj* nm = WordLiteralObj(w2);
                if (nm)
                {
                    Tcl_Obj* clsFqn = FqnUnder(ctx->ip, ctx->curNs, nm);
                    if (clsFqn)
                    {
                        int leaveVerbatim = 0;
                        Tcl_DString cmdLn;
                        Tcl_DStringInit(&cmdLn);
                        Tcl_DStringAppendElement(&cmdLn, c0);
                        Tcl_DStringAppendElement(&cmdLn, "create");
                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(nm));
                        {
                            const Tcl_Token* w3 = NextWord(w2);
                            Tcl_Obj* bod = WordLiteralObj(w3);
                            if (bod)
                            {
                                Tcl_Size bl = 0;
                                const char* bs = Tbcx_GetStringFromObjSafe(bod, &bl);
                                if (ObjdefineBuilderHasNonLiteralMethod(ctx->ip, bs, bl, 0))
                                {
                                    leaveVerbatim = 1; /* non-literal body — leave verbatim */
                                }
                                else
                                {
                                    CS_Add(ctx->classes, clsFqn);
                                    CaptureClassBody(ctx->ip,
                                                     bs,
                                                     bl,
                                                     ctx->curNs,
                                                     clsFqn,
                                                     ctx->defs,
                                                     ctx->classes,
                                                     DEF_F_FROM_BUILDER,
                                                     0,
                                                     0,
                                                     ctx->defs->n);
                                    if (IsPureOodefineBuilderBody(ctx->ip, bs, bl))
                                    {
                                        Tcl_DStringAppendElement(&cmdLn, "");
                                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                        Tcl_DStringAppend(ctx->out, "\n", 1);
                                        StubLinesForClass(ctx->ip, ctx->out, ctx->defs, clsFqn, bs, bl);
                                    }
                                    else
                                    {
                                        Tcl_Obj* stubbed = StubbedBuilderBody(ctx->ip, bod);
                                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(stubbed));
                                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                        Tcl_DStringAppend(ctx->out, "\n", 1);
                                        Tcl_DecrRefCount(stubbed);
                                        EmitSelfDeclaratives(ctx->ip, ctx->out, clsFqn, bs, bl);
                                        EmitFlatSelfStubs(ctx->ip, ctx->out, ctx->defs, clsFqn);
                                    }
                                }
                                Tcl_DecrRefCount(bod);
                            }
                            else
                            {
                                CS_Add(ctx->classes, clsFqn);
                                Tcl_DStringAppendElement(&cmdLn, "");
                                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                Tcl_DStringAppend(ctx->out, "\n", 1);
                            }
                        }
                        Tcl_DStringFree(&cmdLn);
                        Tcl_DecrRefCount(clsFqn);
                        if (!leaveVerbatim)
                            *handled = 1;
                    } /* if (clsFqn) */
                    Tcl_DecrRefCount(nm);
                }
            }
            if (cmdFqn)
                Tcl_DecrRefCount(cmdFqn);
        }
        if (sub)
            Tcl_DecrRefCount(sub);
    }

    /* ---- oo::objdefine obj { builder body } ----
     * Handles per-object method definitions.
     * Literal target: full capture + stub (like oo::define).
     * Variable target: leave verbatim (can't resolve obj FQN).
     */
    if (!*handled && p->numWords == 3 && strcmp(c0, "oo::objdefine") == 0)
    {
        const Tcl_Token* wObj = NextWord(w0);
        const Tcl_Token* wBod = wObj ? NextWord(wObj) : NULL;
        Tcl_Obj* objNm = wObj ? WordLiteralObj(wObj) : NULL;
        Tcl_Obj* bod = wBod ? WordLiteralObj(wBod) : NULL;
        if (objNm && bod)
        {
            Tcl_Obj* objFqn = FqnUnder(ctx->ip, ctx->curNs, objNm);
            if (objFqn)
            {
                Tcl_Size bl = 0;
                const char* bs = Tbcx_GetStringFromObjSafe(bod, &bl);
                if (ObjdefineBuilderHasNonLiteralMethod(ctx->ip, bs, bl, 0))
                {
                    /* A per-object method whose body or arg-spec is not a static
                       literal (e.g. `method m {} [computeBody]`) cannot be
                       precompiled, and stubbing it would silently drop the body.
                       Leave the whole builder VERBATIM — matching the flat
                       `oo::objdefine OBJ method …` statement and the runtime-
                       variable-target forms — so every body runs as ordinary
                       code at load (source-equivalent), at the cost of not
                       precompiling/stripping this one builder.  *handled stays 0
                       so the caller emits the original command unchanged. */
                    Tcl_DecrRefCount(objFqn);
                }
                else
                {
                    /* Capture from builder body.  The object-definition origin flag
                       travels into every captured record so each is keyed with
                       object origin at load (coexisting with any same-name class
                       method), and so a `self method` here is rejected at save. */
                    CaptureClassBody(ctx->ip,
                                     bs,
                                     bl,
                                     ctx->curNs,
                                     objFqn,
                                     ctx->defs,
                                     ctx->classes,
                                     DEF_F_FROM_BUILDER | DEF_F_OBJDEFINE,
                                     0,
                                     0,
                                     ctx->defs->n);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: check purity then stub.  The object predicate is
                       stricter than the class one — only plain `method NAME ARGS
                       BODY` stays pure; option/private/export/unexport blocks route
                       through StubbedBuilderBody so the loader replays them via
                       PrecompObject against the object's own method table. */
                    if (IsPureObjdefineBuilderBody(ctx->ip, bs, bl))
                    {
                        /* Expand into multi-word oo::objdefine stubs,
                           walking the body to preserve command order and
                           emit non-method commands (variable etc.) verbatim. */
                        const char* objNmStr = Tbcx_GetStringSafe(objNm);
                        Tcl_Parse pp;
                        const char* pcur = bs;
                        Tcl_Size prem = bl;
                        while (prem > 0)
                        {
                            if (Tcl_ParseCommand(ctx->ip, pcur, prem, 0, &pp) != TCL_OK)
                            {
                                Tcl_FreeParse(&pp);
                                break;
                            }
                            if (pp.numWords >= 1)
                            {
                                const Tcl_Token* pw0 = pp.tokenPtr;
                                if (pw0->type == TCL_TOKEN_COMMAND)
                                    pw0++;
                                Tcl_Obj* pkw = WordLiteralObj(pw0);
                                if (pkw)
                                {
                                    const char* pk = CmdCore(Tbcx_GetStringSafe(pkw));
                                    Tcl_DString ln;
                                    Tcl_DStringInit(&ln);
                                    Tcl_DStringAppendElement(&ln, "oo::objdefine");
                                    Tcl_DStringAppendElement(&ln, objNmStr);
                                    if ((strcmp(pk, "method") == 0 || strcmp(pk, "classmethod") == 0) && pp.numWords >= 4)
                                    {
                                        /* Emit all words except last (body), then the stub sentinel */
                                        const Tcl_Token* wt = pw0;
                                        for (Tcl_Size wi = 0; wi + 1 < pp.numWords; wi++)
                                        {
                                            Tcl_Obj* w = WordLiteralObj(wt);
                                            if (w)
                                            {
                                                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(w));
                                                Tcl_DecrRefCount(w);
                                            }
                                            wt = NextWord(wt);
                                        }
                                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                                    }
                                    else if (strcmp(pk, "constructor") == 0 && pp.numWords >= 3)
                                    {
                                        const Tcl_Token* wt = pw0;
                                        for (Tcl_Size wi = 0; wi + 1 < pp.numWords; wi++)
                                        {
                                            Tcl_Obj* w = WordLiteralObj(wt);
                                            if (w)
                                            {
                                                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(w));
                                                Tcl_DecrRefCount(w);
                                            }
                                            wt = NextWord(wt);
                                        }
                                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                                    }
                                    else if (strcmp(pk, "destructor") == 0)
                                    {
                                        Tcl_DStringAppendElement(&ln, "destructor");
                                        Tcl_DStringAppendElement(&ln, TBCX_METH_STUB_BODY);
                                    }
                                    else
                                    {
                                        /* Non-body command (variable etc.) — emit verbatim */
                                        const Tcl_Token* wt = pw0;
                                        for (Tcl_Size wi = 0; wi < pp.numWords; wi++)
                                        {
                                            Tcl_Obj* w = WordLiteralObj(wt);
                                            if (w)
                                            {
                                                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(w));
                                                Tcl_DecrRefCount(w);
                                            }
                                            wt = NextWord(wt);
                                        }
                                    }
                                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                                    Tcl_DStringAppend(ctx->out, "\n", 1);
                                    Tcl_DStringFree(&ln);
                                    Tcl_DecrRefCount(pkw);
                                }
                            }
                            pcur = pp.commandStart + pp.commandSize;
                            prem = (bs + bl) - pcur;
                            Tcl_FreeParse(&pp);
                        }
                        Tcl_ResetResult(ctx->ip);
                    }
                    else
                    {
                        Tcl_Obj* stubbed = StubbedBuilderBody(ctx->ip, bod);
                        Tcl_DString cmdLn;
                        Tcl_DStringInit(&cmdLn);
                        Tcl_DStringAppendElement(&cmdLn, "oo::objdefine");
                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(objNm));
                        Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(stubbed));
                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                        Tcl_DStringAppend(ctx->out, "\n", 1);
                        Tcl_DStringFree(&cmdLn);
                        Tcl_DecrRefCount(stubbed);
                    }
                    Tcl_DecrRefCount(objFqn);
                    *handled = 1;
                } /* else: capturable builder */
            } /* if (objFqn) */
        }
        if (objNm)
            Tcl_DecrRefCount(objNm);
        if (bod)
            Tcl_DecrRefCount(bod);
    }

    /* ---- oo::objdefine obj subcommand ... (multi-word) ---- */
    if (!*handled && p->numWords >= 5 && strcmp(c0, "oo::objdefine") == 0)
    {
        const Tcl_Token* wObj = NextWord(w0);
        Tcl_Obj* objNm = WordLiteralObj(wObj);
        const Tcl_Token* wK = NextWord(wObj);
        Tcl_Obj* kwd = WordLiteralObj(wK);
        if (objNm && kwd)
        {
            Tcl_Obj* objFqn = FqnUnder(ctx->ip, ctx->curNs, objNm);
            const char* kw = objFqn ? Tbcx_GetStringSafe(kwd) : "";

            if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p->numWords >= 6)
            {
                /* Words: oo::objdefine OBJ method NAME ?-option? ARGS BODY.
                   Name is the word right after the keyword; args/body are the
                   last two; options resolve into the scope byte (applied at
                   load by the objdefine shim). */
                const Tcl_Token* wN = NextWord(wK);
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 2 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wA = tokP, *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
                int optScope = MethodScopeFromOptionWords(NextWord(wN), p->numWords - 6);
                if (args)
                {
                    Tcl_Size _d;
                    if (Tcl_ListObjLength(ctx->ip, args, &_d) != TCL_OK)
                    {
                        Tcl_ResetResult(ctx->ip);
                        Tcl_DecrRefCount(args);
                        args = NULL;
                    }
                }
                if (mname && args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "classmethod") == 0 ? DEF_KIND_CLASS : DEF_KIND_INST);
                    r.cls = objFqn;
                    Tcl_IncrRefCount(r.cls); /* borrowed: local DecrRefCount at scope end */
                    r.name = mname;          /* transferred from WordLiteralObj (refcount 1) */
                    r.args = args;           /* transferred from WordLiteralObj (refcount 1) */
                    r.body = body;           /* transferred from WordLiteralObj (refcount 1) */
                    r.ns = ctx->curNs;
                    r.scope = optScope;
                    r.flags = DEF_F_OBJDEFINE; /* object-definition origin (drives the wire origin byte) */
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: stub the body (option dropped — scope byte
                       carries it; the objdefine shim re-applies it). */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::objdefine");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(objNm));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(mname));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    Tcl_DStringAppendElement(&line, TBCX_METH_STUB_BODY);
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&line);
                    *handled = 1;
                }
                else
                {
                    if (mname)
                        Tcl_DecrRefCount(mname);
                    if (args)
                        Tcl_DecrRefCount(args);
                    if (body)
                        Tcl_DecrRefCount(body);
                }
            }
            else if ((strcmp(kw, "constructor") == 0 || strcmp(kw, "destructor") == 0) && p->numWords >= 4)
            {
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 1 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wBody = tokP, *wArgsTok = NULL;
                if (p->numWords >= 5)
                {
                    const Tcl_Token* pre = w0;
                    for (Tcl_Size w = 0; w + 2 < p->numWords; w++)
                        pre = NextWord(pre);
                    wArgsTok = pre;
                }
                Tcl_Obj* args = wArgsTok ? WordLiteralObj(wArgsTok) : NULL;
                if (!args)
                {
                    args = Tcl_NewStringObj("", 0);
                    Tcl_IncrRefCount(args);
                }
                Tcl_Obj* body = WordLiteralObj(wBody);
                if (args)
                {
                    Tcl_Size _d;
                    if (Tcl_ListObjLength(ctx->ip, args, &_d) != TCL_OK)
                    {
                        Tcl_ResetResult(ctx->ip);
                        Tcl_DecrRefCount(args);
                        args = NULL;
                    }
                }
                if (args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "constructor") == 0 ? DEF_KIND_CTOR : DEF_KIND_DTOR);
                    r.cls = objFqn;
                    Tcl_IncrRefCount(r.cls); /* borrowed: local DecrRefCount at scope end */
                    r.args = args;           /* transferred (refcount 1) */
                    r.body = body;           /* transferred from WordLiteralObj (refcount 1) */
                    r.ns = ctx->curNs;
                    r.flags = DEF_F_OBJDEFINE; /* object-definition origin (drives the wire origin byte) */
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: stub */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::objdefine");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(objNm));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    /* Sentinel stub body (uniform with every method form). */
                    Tcl_DStringAppendElement(&line, TBCX_METH_STUB_BODY);
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&line);
                    *handled = 1;
                }
                else
                {
                    if (args)
                        Tcl_DecrRefCount(args);
                    if (body)
                        Tcl_DecrRefCount(body);
                }
            }
            if (objFqn)
                Tcl_DecrRefCount(objFqn);
            Tcl_DecrRefCount(objNm);
            Tcl_DecrRefCount(kwd);
        }
        else
        {
            if (objNm)
                Tcl_DecrRefCount(objNm);
            if (kwd)
                Tcl_DecrRefCount(kwd);
        }
    }

    /* ============================================================
     * Control-flow body recursion
     * Recurse into literal body arguments of common control-flow
     * commands to capture nested proc/method definitions.
     *
     * Safety: RecurseScriptBody returns NULL when the body is
     * unchanged, so these handlers only fire when a definition
     * was actually found and stubbed.  Additionally, each handler
     * verifies that all non-body words are extractable as literals;
     * if any word uses variable/command substitution, the command
     * is left verbatim (the non-literal word can't be reconstructed
     * from DStringAppendElement).
     * ============================================================ */
    if (cmd)
        Tcl_DecrRefCount(cmd);
    return *handled;
}

/* Handle control-flow commands (while, for, foreach, catch, if, try, switch).
 * Returns 1 if the command was handled, 0 otherwise. */
static int RewriteControlFlow(RewriteCtx* ctx, Tcl_Parse* p, const Tcl_Token* w0, const char* c0, int* handled)
{
    if (!ctx || !ctx->ip || !ctx->out || !ctx->curNs || !ctx->defs || !ctx->classes || !p || !w0 || !c0 || !handled)
        return 0;
    if (!*handled && strcmp(c0, "while") == 0 && p->numWords == 3)
    {
        const Tcl_Token* wCond = NextWord(w0);
        const Tcl_Token* wBody = NextWord(wCond);
        Tcl_Obj* rew = RecurseScriptBody(ctx->ip, wBody, ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
        if (rew)
        {
            Tcl_Obj* condO = WordLiteralObj(wCond);
            if (condO)
            {
                Tcl_DString ln;
                Tcl_DStringInit(&ln);
                Tcl_DStringAppendElement(&ln, "while");
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(condO));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rew));
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                Tcl_DecrRefCount(condO);
                *handled = 1;
            }
            Tcl_DecrRefCount(rew);
        }
    }
    if (!*handled && strcmp(c0, "for") == 0 && p->numWords == 5)
    {
        const Tcl_Token* w1t = NextWord(w0);
        const Tcl_Token* w2t = NextWord(w1t);
        const Tcl_Token* w3t = NextWord(w2t);
        const Tcl_Token* wBody = NextWord(w3t);
        Tcl_Obj* rew = RecurseScriptBody(ctx->ip, wBody, ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
        if (rew)
        {
            Tcl_Obj* o1 = WordLiteralObj(w1t);
            Tcl_Obj* o2 = WordLiteralObj(w2t);
            Tcl_Obj* o3 = WordLiteralObj(w3t);
            if (o1 && o2 && o3)
            {
                Tcl_DString ln;
                Tcl_DStringInit(&ln);
                Tcl_DStringAppendElement(&ln, "for");
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(o1));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(o2));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(o3));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rew));
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                *handled = 1;
            }
            if (o1)
                Tcl_DecrRefCount(o1);
            if (o2)
                Tcl_DecrRefCount(o2);
            if (o3)
                Tcl_DecrRefCount(o3);
            Tcl_DecrRefCount(rew);
        }
    }
    if (!*handled && strcmp(c0, "foreach") == 0 && p->numWords >= 4)
    {
        /* foreach ... body — last word is the body */
        const Tcl_Token* tok = w0;
        for (Tcl_Size fw = 0; fw + 1 < p->numWords; fw++)
            tok = NextWord(tok);
        Tcl_Obj* rew = RecurseScriptBody(ctx->ip, tok, ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
        if (rew)
        {
            /* Pre-scan: all middle words must be literals */
            int allLit = 1;
            const Tcl_Token* wt = NextWord(w0);
            for (Tcl_Size fw = 1; fw + 1 < p->numWords; fw++)
            {
                Tcl_Obj* probe = WordLiteralObj(wt);
                if (!probe)
                {
                    allLit = 0;
                    break;
                }
                Tcl_DecrRefCount(probe);
                wt = NextWord(wt);
            }
            if (allLit)
            {
                Tcl_DString ln;
                Tcl_DStringInit(&ln);
                Tcl_DStringAppendElement(&ln, "foreach");
                wt = NextWord(w0);
                for (Tcl_Size fw = 1; fw + 1 < p->numWords; fw++)
                {
                    Tcl_Obj* wo = WordLiteralObj(wt);
                    Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(wo));
                    Tcl_DecrRefCount(wo);
                    wt = NextWord(wt);
                }
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rew));
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                *handled = 1;
            }
            Tcl_DecrRefCount(rew);
        }
    }
    if (!*handled && strcmp(c0, "catch") == 0 && p->numWords >= 2)
    {
        const Tcl_Token* wBody = NextWord(w0);
        Tcl_Obj* rew = RecurseScriptBody(ctx->ip, wBody, ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
        if (rew)
        {
            /* Pre-scan trailing words */
            int allLit = 1;
            const Tcl_Token* wt = NextWord(wBody);
            for (Tcl_Size fw = 2; fw < p->numWords; fw++)
            {
                Tcl_Obj* probe = WordLiteralObj(wt);
                if (!probe)
                {
                    allLit = 0;
                    break;
                }
                Tcl_DecrRefCount(probe);
                wt = NextWord(wt);
            }
            if (allLit)
            {
                Tcl_DString ln;
                Tcl_DStringInit(&ln);
                Tcl_DStringAppendElement(&ln, "catch");
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rew));
                wt = NextWord(wBody);
                for (Tcl_Size fw = 2; fw < p->numWords; fw++)
                {
                    Tcl_Obj* wo = WordLiteralObj(wt);
                    Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(wo));
                    Tcl_DecrRefCount(wo);
                    wt = NextWord(wt);
                }
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                *handled = 1;
            }
            Tcl_DecrRefCount(rew);
        }
    }
    /* ---- if cond ?then? body ?elseif cond ?then? body? ... ?else body? ----
     * Recurse into each branch body to capture proc/class definitions and stub
     * their source. Each captured proc has a unique Procs-section index and an
     * indexed TBCX_PROC_MARKER_PFX marker. */
    if (!*handled && strcmp(c0, "if") == 0 && p->numWords >= 3)
    {
        int allLit = 1;
        size_t twordsSz;
        if (p->numWords > 0 && tbcx_checked_mul(sizeof(Tcl_Token*), (size_t)p->numWords, &twordsSz))
        {
            const Tcl_Token** twords = (const Tcl_Token**)Tcl_Alloc(twordsSz);
            twords[0] = w0;
            for (Tcl_Size fi = 1; fi < p->numWords; fi++)
                twords[fi] = NextWord(twords[fi - 1]);
            for (Tcl_Size fi = 0; fi < p->numWords; fi++)
            {
                Tcl_Obj* probe = WordLiteralObj(twords[fi]);
                if (!probe)
                {
                    allLit = 0;
                    break;
                }
                Tcl_DecrRefCount(probe);
            }
            if (allLit)
            {
                int anyMod = 0;
                Tcl_Obj** rw = (Tcl_Obj**)Tcl_Alloc(twordsSz); /* same count, pointers same size */
                for (Tcl_Size fi = 0; fi < p->numWords; fi++)
                    rw[fi] = WordLiteralObj(twords[fi]);
                /* Walk if structure:
                   word 0 = "if", word 1 = condition,
                   word 2 = body or "then", ... */
                int fi = 2;
                while (fi < p->numWords)
                {
                    const char* ws = rw[fi] ? Tbcx_GetStringSafe(rw[fi]) : "";
                    if (strcmp(ws, "then") == 0)
                    {
                        fi++;
                        if (fi >= p->numWords)
                            break;
                    }
                    /* fi is a body position */
                    {
                        Tcl_Obj* ifRew =
                            RecurseScriptBody(ctx->ip, twords[fi], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                        if (ifRew)
                        {
                            Tcl_DecrRefCount(rw[fi]);
                            rw[fi] = ifRew; /* transferred (refcount 1) */
                            anyMod = 1;
                        }
                    }
                    fi++; /* past body */
                    if (fi >= p->numWords)
                        break;
                    ws = rw[fi] ? Tbcx_GetStringSafe(rw[fi]) : "";
                    if (strcmp(ws, "elseif") == 0)
                    {
                        fi++; /* skip "elseif" */
                        fi++; /* skip condition */
                    }
                    else if (strcmp(ws, "else") == 0)
                    {
                        fi++; /* skip "else" */
                        if (fi < p->numWords)
                        {
                            Tcl_Obj* elRew =
                                RecurseScriptBody(ctx->ip, twords[fi], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                            if (elRew)
                            {
                                Tcl_DecrRefCount(rw[fi]);
                                rw[fi] = elRew; /* transferred (refcount 1) */
                                anyMod = 1;
                            }
                        }
                        break;
                    }
                    else
                        break;
                }
                if (anyMod)
                {
                    Tcl_DString ln;
                    Tcl_DStringInit(&ln);
                    for (Tcl_Size fi2 = 0; fi2 < p->numWords; fi2++)
                    {
                        if (rw[fi2])
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rw[fi2]));
                    }
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&ln);
                    *handled = 1;
                }
                for (Tcl_Size fi2 = 0; fi2 < p->numWords; fi2++)
                {
                    if (rw[fi2])
                        Tcl_DecrRefCount(rw[fi2]);
                }
                Tcl_Free((char*)rw);
            }
            Tcl_Free((char*)twords);
        } /* checked_mul ok */
    }
    if (!*handled && strcmp(c0, "try") == 0 && p->numWords >= 2)
    {
        /* try body ?on/trap code varList body? ?finally body?
           Recurse into ALL body positions — not just the main
           try body — so proc/class definitions in handlers are
           captured and stubbed too. */
        int allLit = 1;
        /* Collect all word tokens */
        size_t trySz;
        if (p->numWords > 0 && tbcx_checked_mul(sizeof(Tcl_Token*), (size_t)p->numWords, &trySz))
        {
            const Tcl_Token** twords = (const Tcl_Token**)Tcl_Alloc(trySz);
            twords[0] = w0;
            for (Tcl_Size fi = 1; fi < p->numWords; fi++)
                twords[fi] = NextWord(twords[fi - 1]);
            /* Check that all words are literals */
            for (Tcl_Size fi = 0; fi < p->numWords; fi++)
            {
                Tcl_Obj* probe = WordLiteralObj(twords[fi]);
                if (!probe)
                {
                    allLit = 0;
                    break;
                }
                Tcl_DecrRefCount(probe);
            }
            if (allLit)
            {
                int anyMod = 0;
                Tcl_Obj** rw = (Tcl_Obj**)Tcl_Alloc(trySz); /* same count, pointers same size */
                for (Tcl_Size fi = 0; fi < p->numWords; fi++)
                    rw[fi] = WordLiteralObj(twords[fi]);
                /* Identify body positions and try to rewrite them.
                   Word 1 is the main try body.  Then walk handlers. */
                {
                    Tcl_Obj* tryRew = RecurseScriptBody(ctx->ip, twords[1], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                    if (tryRew)
                    {
                        Tcl_DecrRefCount(rw[1]);
                        rw[1] = tryRew; /* transferred (refcount 1) */
                        anyMod = 1;
                    }
                }
                int fi = 2;
                while (fi < p->numWords)
                {
                    const char* ks = rw[fi] ? Tbcx_GetStringSafe(rw[fi]) : "";
                    if ((strcmp(ks, "on") == 0 || strcmp(ks, "trap") == 0) && fi + 3 < p->numWords)
                    {
                        /* on/trap code varList body */
                        int bodyIdx = fi + 3;
                        Tcl_Obj* hRew =
                            RecurseScriptBody(ctx->ip, twords[bodyIdx], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                        if (hRew)
                        {
                            Tcl_DecrRefCount(rw[bodyIdx]);
                            rw[bodyIdx] = hRew; /* transferred (refcount 1) */
                            anyMod = 1;
                        }
                        fi = bodyIdx + 1;
                    }
                    else if (strcmp(ks, "finally") == 0 && fi + 1 < p->numWords)
                    {
                        int bodyIdx = fi + 1;
                        Tcl_Obj* fRew =
                            RecurseScriptBody(ctx->ip, twords[bodyIdx], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                        if (fRew)
                        {
                            Tcl_DecrRefCount(rw[bodyIdx]);
                            rw[bodyIdx] = fRew; /* transferred (refcount 1) */
                            anyMod = 1;
                        }
                        fi = bodyIdx + 1;
                    }
                    else
                    {
                        fi++;
                    }
                }
                if (anyMod)
                {
                    Tcl_DString ln;
                    Tcl_DStringInit(&ln);
                    for (Tcl_Size fi2 = 0; fi2 < p->numWords; fi2++)
                    {
                        if (rw[fi2])
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(rw[fi2]));
                    }
                    Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                    Tcl_DStringAppend(ctx->out, "\n", 1);
                    Tcl_DStringFree(&ln);
                    *handled = 1;
                }
                for (Tcl_Size fi2 = 0; fi2 < p->numWords; fi2++)
                {
                    if (rw[fi2])
                        Tcl_DecrRefCount(rw[fi2]);
                }
                Tcl_Free((char*)rw);
            }
            Tcl_Free((char*)twords);
        } /* checked_mul ok */
    }

    return *handled;
}

static Tcl_Obj* CaptureAndRewriteScript(
    Tcl_Interp* ip, const char* script, Tcl_Size len, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth)
{
    if (!ip || !script || !curNs || !defs || !classes)
        return NULL;
    /* Guard against unbounded recursion from nested namespace eval / control flow */
    if (depth > TBCX_MAX_BLOCK_DEPTH)
    {
        Tcl_Obj* r = Tcl_NewStringObj(script, len);
        Tcl_IncrRefCount(r);
        return r;
    }

    Tcl_DString out;
    Tcl_DStringInit(&out);
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = len >= 0 ? len : (Tcl_Size)strlen(script);

    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            /* Skip to next newline, copying verbatim, and continue.
               This avoids aborting the entire rewrite when a single
               command causes a parse error (e.g., from unusual quoting). */
            const char* nl = cur;
            while (nl < script + len && *nl != '\n')
                nl++;
            if (nl < script + len)
                nl++; /* include the newline */
            Tcl_DStringAppend(&out, cur, (Tcl_Size)(nl - cur));
            cur = nl;
            remain = (script + len) - cur;
            continue;
        }
        const char* cmdStart = p.commandStart;
        const char* cmdEnd = p.commandStart + p.commandSize;
        int handled = 0;

        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* c0 = CmdCore(Tbcx_GetStringSafe(cmd));

                /* ---- proc name args body ---- */
                if (p.numWords == 4 && strcmp(c0, "proc") == 0)
                {
                    const Tcl_Token* w1 = NextWord(w0);
                    const Tcl_Token* w2 = NextWord(w1);
                    const Tcl_Token* w3 = NextWord(w2);
                    Tcl_Obj* name = WordLiteralObj(w1);
                    Tcl_Obj* args = WordLiteralObj(w2);
                    Tcl_Obj* body = WordLiteralObj(w3);
                    if (args)
                    {
                        Tcl_Size _d;
                        if (Tcl_ListObjLength(ip, args, &_d) != TCL_OK)
                        {
                            Tcl_ResetResult(ip);
                            Tcl_DecrRefCount(args);
                            args = NULL;
                        }
                    }
                    if (name && args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_PROC;
                        r.name = name; /* transferred from WordLiteralObj (refcount 1) */
                        r.args = args; /* transferred from WordLiteralObj (refcount 1) */
                        r.body = body; /* transferred from WordLiteralObj (refcount 1) */
                        r.flags = 0;
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        DV_Push(defs, r);
                        /* Rewrite: emit proc with indexed marker body so that
                           the loader's ProcShim can match by position rather
                           than FQN — correctly handling conflicting proc names
                           across if/else branches.  The marker prefix \x01TBCX
                           cannot collide with any valid Tcl proc body. */
                        {
                            uint32_t procIdx = 0;
                            for (Tcl_Size k = 0; k < defs->n; k++)
                                if (defs->v[k].kind == DEF_KIND_PROC)
                                    procIdx++;
                            procIdx--; /* just-pushed entry is 0-based */
                            Tcl_Obj* markerObj = Tcl_ObjPrintf(TBCX_PROC_MARKER_PFX "%u", (unsigned)procIdx);
                            Tcl_IncrRefCount(markerObj);
                            Tcl_DString line;
                            Tcl_DStringInit(&line);
                            Tcl_DStringAppendElement(&line, "proc");
                            Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(name));
                            Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                            Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(markerObj));
                            Tcl_DecrRefCount(markerObj);
                            Tcl_DStringAppend(&out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                            Tcl_DStringAppend(&out, "\n", 1);
                            Tcl_DStringFree(&line);
                        }
                        handled = 1;
                    }
                    else
                    {
                        if (name)
                            Tcl_DecrRefCount(name);
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                }

                /* ---- namespace eval ns body ---- */
                else if (p.numWords == 4 && strcmp(c0, "namespace") == 0)
                {
                    const Tcl_Token* w1 = NextWord(w0);
                    Tcl_Obj* sub = WordLiteralObj(w1);
                    if (sub && strcmp(Tbcx_GetStringSafe(sub), "eval") == 0)
                    {
                        const Tcl_Token* w2 = NextWord(w1);
                        const Tcl_Token* w3 = NextWord(w2);
                        Tcl_Obj* nsObj = WordLiteralObj(w2);
                        Tcl_Obj* bodyObj = WordLiteralObj(w3);
                        if (nsObj && bodyObj)
                        {
                            Tcl_Obj* nsFqn = FqnUnder(ip, curNs, nsObj);
                            if (nsFqn)
                            {
                                /* Recurse: single-pass capture+rewrite of inner body */
                                Tcl_Size bodyLen = 0;
                                const char* bodyStr = Tbcx_GetStringFromObjSafe(bodyObj, &bodyLen);
                                Tcl_Obj* rewritten =
                                    CaptureAndRewriteScript(ip, bodyStr, bodyLen, nsFqn, defs, classes, depth + 1);
                                if (rewritten)
                                {
                                    Tcl_Obj* canonBody = CanonTrivia(rewritten);
                                    Tcl_DString cmdLn;
                                    Tcl_DStringInit(&cmdLn);
                                    Tcl_DStringAppendElement(&cmdLn, "::tcl::namespace::eval");
                                    Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(nsObj));
                                    Tcl_DStringAppendElement(&cmdLn, Tbcx_GetStringSafe(canonBody));
                                    Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                    Tcl_DStringAppend(&out, "\n", 1);
                                    Tcl_DStringFree(&cmdLn);
                                    Tcl_DecrRefCount(canonBody);
                                    Tcl_DecrRefCount(rewritten);
                                    handled = 1;
                                }
                                Tcl_DecrRefCount(nsFqn);
                            } /* if (nsFqn) */
                            Tcl_DecrRefCount(bodyObj);
                            Tcl_DecrRefCount(nsObj);
                        }
                        else
                        {
                            if (nsObj)
                                Tcl_DecrRefCount(nsObj);
                            if (bodyObj)
                                Tcl_DecrRefCount(bodyObj);
                        }
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                }

                /* ---- OO commands (oo::define, oo::class, oo::objdefine, metaclass) ---- */
                if (!handled)
                {
                    RewriteCtx rctx = {ip, &out, curNs, defs, classes, depth};
                    RewriteOoCmd(&rctx, &p, w0, c0, &handled);
                }

                /* ---- Control flow (while, for, foreach, catch, if, try, switch) ---- */
                if (!handled)
                {
                    RewriteCtx rctx = {ip, &out, curNs, defs, classes, depth};
                    RewriteControlFlow(&rctx, &p, w0, c0, &handled);
                }

                Tcl_DecrRefCount(cmd);
            }
        }
        if (!handled)
        {
            Tcl_DStringAppend(&out, cmdStart, (Tcl_Size)(cmdEnd - cmdStart));
        }
        cur = cmdEnd;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
    Tcl_Obj* rew = Tcl_NewStringObj(Tcl_DStringValue(&out), Tcl_DStringLength(&out));
    Tcl_IncrRefCount(rew); /* caller owns one reference */
    Tcl_DStringFree(&out);
    return rew;
}

static int EmitTbcxStream(Tcl_Obj* scriptObj, TbcxOut* w, unsigned saveFlags, Tcl_Obj* sourcePath)
{
    int rc = TCL_ERROR; /* set to TCL_OK only on success */
    TbcxCtx ctx = {0};
    ctx.interp = w->interp;
    ctx.saveFlags = saveFlags;
    ctx.artifactPolicy =
        (saveFlags & TBCX_SAVE_FL_INCLUDE_SOURCE) ? TBCX_BLOCK_POLICY_INCLUDE_SOURCE : TBCX_BLOCK_POLICY_STRICT_COMPILED_ONLY;
    ctx.sourcePath = sourcePath;
    Tcl_InitHashTable(&ctx.authoredCommands, TCL_STRING_KEYS);
    ctx.authoredCommandsInit = 1;

    DefVec defs;
    DV_Init(&defs);
    ClsSet classes;
    CS_Init(&classes);
    Tcl_Obj* rootNs = Tcl_NewStringObj("::", -1);
    Tcl_IncrRefCount(rootNs);
    Tcl_Size srcLen = 0;
    const char* srcStr = Tbcx_GetStringFromObjSafe(scriptObj, &srcLen);
    Tcl_Obj* srcCopy = Tcl_NewStringObj(srcStr, srcLen);
    Tcl_IncrRefCount(srcCopy);

    /* Single-pass capture + rewrite
       CaptureAndRewriteScript walks the script once, capturing proc/method/class
       definitions into defs/classes while simultaneously producing a rewritten
       script with method bodies stubbed out. */
    if (srcLen > 0)
    {
        Tcl_Obj* rew = CaptureAndRewriteScript(w->interp, srcStr, srcLen, rootNs, &defs, &classes, 0);
        if (rew)
        {
            Tcl_DecrRefCount(srcCopy);
            srcCopy = rew; /* transferred (refcount 1) */
        }
    }

    /* Record captured Tcl commands before classifying generic invocations.
     * Runtime dispatch may select these definitions instead of a same-named
     * core command, in which case its arguments remain ordinary data. */
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if (defs.v[i].kind != DEF_KIND_PROC)
            continue;
        Tcl_Obj* canonicalName = FqnUnder(w->interp, defs.v[i].ns, defs.v[i].name);
        if (!canonicalName)
            goto cleanup;
        Tcl_Size nameLength = 0;
        const char* name = Tbcx_GetStringFromObjStrict(w->interp, canonicalName, &nameLength);
        if (!name || (nameLength > 0 && memchr(name, '\0', (size_t)nameLength) != NULL))
        {
            if (name)
                Tcl_SetObjResult(w->interp, Tcl_NewStringObj("tbcx: proc command name contains embedded NUL", -1));
            Tcl_DecrRefCount(canonicalName);
            goto cleanup;
        }
        int isNew = 0;
        (void)Tcl_CreateHashEntry(&ctx.authoredCommands, name, &isNew);
        Tcl_DecrRefCount(canonicalName);
    }

    /* 1. Compile the rewritten top-level without the save caller's procedure
     * slot table. Top-level artifacts have no compiled locals, and their
     * generic variable instructions resolve in the load caller's active frame
     * and namespace. compiledProcPtr and the active LocalCache are restored
     * after compilation. */
    {
        Interp* iPtr = (Interp*)w->interp;
        CallFrame* saveFrame = iPtr->varFramePtr;
        LocalCache* saveLC = saveFrame ? saveFrame->localCachePtr : NULL;
        Proc* saveProc = iPtr->compiledProcPtr;
        iPtr->compiledProcPtr = NULL;
        if (saveFrame)
            saveFrame->localCachePtr = NULL;
        int tlRc = CompileScriptForArtifact(w->interp, srcCopy, ctx.artifactPolicy);
        if (saveFrame)
            saveFrame->localCachePtr = saveLC;
        iPtr->compiledProcPtr = saveProc;
        if (tlRc != TCL_OK)
            goto cleanup;
    }
    ByteCode* top = NULL;
    top = TbcxGetByteCode(srcCopy);
    if (!top)
    {
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj("tbcx: failed to get top bytecode", -1));
        goto cleanup;
    }

    /* 2. Early AuxData validation — fail fast if the compiled bytecode
       contains AuxData types TBCX doesn't know how to serialize.
       (Tcl 9.1's try/catch/finally compiles to exception ranges, not AuxData,
       so the five known types cover all standard Tcl 9.1 bytecode.) */
    for (Tcl_Size ai = 0; ai < top->numAuxDataItems && top->auxDataArrayPtr; ai++)
    {
        AuxData* ad = &top->auxDataArrayPtr[ai];
        if (ad->type != tbcxAuxJTStr && ad->type != tbcxAuxJTNum && ad->type != tbcxAuxDictUpdate &&
            ad->type != tbcxAuxNewForeach)
        {
            Tcl_SetObjResult(
                w->interp,
                Tcl_ObjPrintf("tbcx: top-level bytecode contains unsupported AuxData type '%s' at index %" TCL_Z_MODIFIER "d. "
                              "If this is a new Tcl 9.1+ type, TBCX must be extended to handle it.",
                              (ad->type && ad->type->name) ? ad->type->name : "(null)",
                              ai));
            goto cleanup;
        }
    }

    /* 3. Header */
    WriteHeaderTop(w, &ctx, srcCopy);
    if (w->err)
        goto cleanup;

    /* 4. Top-level compiled block */
    WriteCompiledBlock(w, &ctx, srcCopy, ctx.artifactPolicy);
    if (w->err)
        goto cleanup;

    /* 5. Procs section: nameFqn, namespace, args, srcText, block srcText is the
     *    original proc body as authored — attached at load time via
     *    Tcl_InitStringRep so `info body`, TIP #280 attribution, stack-trace
     *    line numbers, and introspection-based clones all round-trip correctly.
     *    Only emitted when -include-source was specified; otherwise the field
     *    is written empty and the loader substitutes a diagnostic sentinel. */
    uint32_t numProcs = 0;
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if (defs.v[i].kind == DEF_KIND_PROC)
            numProcs++;
    }
    W_U32(w, numProcs);
    for (Tcl_Size i = 0; i < defs.n; i++)
        if (defs.v[i].kind == DEF_KIND_PROC)
        {
            /* Serialize FQN/name, ns, args */
            Tcl_Size ln;
            const char* s;
            s = Tbcx_GetStringFromObjSafe(defs.v[i].name, &ln);
            W_LPString(w, s, ln);
            s = Tbcx_GetStringFromObjSafe(defs.v[i].ns, &ln);
            W_LPString(w, s, ln);
            s = Tbcx_GetStringFromObjSafe(defs.v[i].args, &ln);
            W_LPString(w, s, ln);

            /* Body source text.  Emitted before the compiled block
             * so the loader can read+allocate the string up front and
             * attach it without an extra pass.  Length==0 means
             * "stripped" and is the loader's cue to install a sentinel. */
            s = "";
            ln = 0;
            if (PolicyIncludesSource(&ctx))
            {
                s = Tbcx_GetStringFromObjStrict(w->interp, defs.v[i].body, &ln);
                if (!s)
                {
                    w->err = TCL_ERROR;
                    goto cleanup;
                }
            }
            WriteExecutableSource(w, &ctx, TBCX_EXEC_SOURCE_PROC, s, ln);

            /* Compile body offline (proc semantics) and emit */
            {
                if (CompileProcLike(w, &ctx, defs.v[i].ns, defs.v[i].args, defs.v[i].body, "body of proc") != TCL_OK)
                    goto cleanup;
            }
        }

    /* 6. Classes section (FQN + nSupers=0 for now) — use unique set.
       Sorted alphabetically for deterministic, reproducible output. */
    {
        Tcl_HashEntry* h;
        Tcl_HashSearch srch;
        /* Count */
        uint32_t numClasses = 0;
        for (h = Tcl_FirstHashEntry(&classes.ht, &srch); h; h = Tcl_NextHashEntry(&srch))
        {
            numClasses++;
        }
        W_U32(w, numClasses);
        if (numClasses > 0)
        {
            /* Collect keys, sort, then emit for reproducibility */
            size_t keyBytes = 0;
            if (!tbcx_checked_mul(sizeof(const char*), (size_t)numClasses, &keyBytes))
            {
                W_Error(w, "tbcx: class table too large");
                goto done_classes;
            }
            const char** keys = (const char**)Tcl_Alloc(keyBytes);
            uint32_t ki = 0;
            for (h = Tcl_FirstHashEntry(&classes.ht, &srch); h; h = Tcl_NextHashEntry(&srch))
            {
                keys[ki++] = (const char*)Tcl_GetHashKey(&classes.ht, h);
            }
            qsort(keys, (size_t)numClasses, sizeof(const char*), CmpStrPtr_qsort);
            for (ki = 0; ki < numClasses; ki++)
            {
                Tcl_Size ln = (Tcl_Size)strlen(keys[ki]);
                W_LPString(w, keys[ki], ln);
                W_U32(w, 0); /* nSupers */
            }
            Tcl_Free((char*)keys);
        }
    done_classes:;
    }

    /* 7-pre0. Per-object self/class methods are not representable.
     * oo::objdefine has no `self method` form, and object-definition replay
     * cannot install records carrying both object origin and self-method kind. */
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if ((defs.v[i].flags & DEF_F_OBJDEFINE) && (defs.v[i].flags & DEF_F_SELF_METHOD))
        {
            Tcl_Size cl = 0;
            const char* cs = Tbcx_GetStringFromObjSafe(defs.v[i].cls, &cl);
            Tcl_Size nl = 0;
            const char* ns = defs.v[i].name ? Tbcx_GetStringFromObjSafe(defs.v[i].name, &nl) : "";
            Tcl_SetObjResult(w->interp,
                             Tcl_ObjPrintf("tbcx: cannot serialize 'self method %s' inside an oo::objdefine builder for %s "
                                           "(per-object self/class methods are not representable)",
                                           ns,
                                           cs));
            w->err = TCL_ERROR;
            goto cleanup; /* rc is TCL_ERROR */
        }
    }

    /* No method-key collision guard is needed here: each method record carries
       an origin byte (class-definition vs object-definition), so a class-instance
       method and a per-object method that share an FQN/kind/name are keyed
       distinctly at load and coexist. */

    /* 7. Methods section: emit captured OO methods/ctors/dtors */
    uint32_t numMethods = 0;
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if (defs.v[i].kind != DEF_KIND_PROC)
            numMethods++;
    }
    W_U32(w, numMethods);
    for (Tcl_Size i = 0; i < defs.n; i++)
        if (defs.v[i].kind != DEF_KIND_PROC)
        {
            Tcl_Size ln;
            const char* s;
            /* classFqn */
            s = Tbcx_GetStringFromObjSafe(defs.v[i].cls, &ln);
            W_LPString(w, s, ln);
            /* wire kind 0..4 expected by loader (inst=0, class=1, ctor=2, dtor=3, self=4) */
            {
                uint8_t wireKind = (uint8_t)(defs.v[i].kind - DEF_KIND_INST);
                /* self methods get wire kind 4 (TBCX_METH_SELF)
                   so the load side installs them via oo::define { self method },
                   preserving metaclass inheritance for subclasses. */
                if (defs.v[i].flags & DEF_F_SELF_METHOD)
                    wireKind = 4; /* TBCX_METH_SELF */
                W_U8(w, wireKind);
            }
            /* scope byte: TclOO visibility the loaded method must have —
               see TBCX_MSCOPE_*.  Ctor/dtor carry DEFAULT. */
            {
                uint8_t wireScope = (uint8_t)defs.v[i].scope;
                if (defs.v[i].kind == DEF_KIND_CTOR || defs.v[i].kind == DEF_KIND_DTOR)
                    wireScope = TBCX_MSCOPE_DEFAULT;
                W_U8(w, wireScope);
            }
            /* origin byte: 0 = class-definition (oo::define/oo::class),
               1 = object-definition (oo::objdefine).  The loader keys records
               by (FQN, kind, name, origin) so a class-instance method and a
               per-object method that share an FQN/kind/name coexist. */
            W_U8(w, (uint8_t)((defs.v[i].flags & DEF_F_OBJDEFINE) ? TBCX_MORIGIN_OBJECT : TBCX_MORIGIN_CLASS));
            /* name (empty for ctor/dtor) */
            if (defs.v[i].kind == DEF_KIND_CTOR || defs.v[i].kind == DEF_KIND_DTOR)
            {
                W_LPString(w, "", 0);
            }
            else
            {
                s = Tbcx_GetStringFromObjSafe(defs.v[i].name, &ln);
                W_LPString(w, s, ln);
            }
            /* args */
            s = Tbcx_GetStringFromObjSafe(defs.v[i].args, &ln);
            W_LPString(w, s, ln);

            /* Body source text — see Procs-section comment above.
             * Applies identically to inst, class, ctor, dtor, and self
             * methods; TclOO's ProcedureMethod wraps a Proc* whose
             * bodyPtr's string rep is read by TclOOGetMethodBody,
             * info class method, info object method, etc.  Source
             * inclusion is all-or-nothing per artifact. */
            s = "";
            ln = 0;
            if (PolicyIncludesSource(&ctx))
            {
                s = Tbcx_GetStringFromObjStrict(w->interp, defs.v[i].body, &ln);
                if (!s)
                {
                    w->err = TCL_ERROR;
                    goto cleanup;
                }
            }
            WriteExecutableSource(w, &ctx, TBCX_EXEC_SOURCE_METHOD, s, ln);

            /* Compile & emit block (proc semantics) */
            if (CompileProcLike(w, &ctx, defs.v[i].cls, defs.v[i].args, defs.v[i].body, "body of method") != TCL_OK)
                goto cleanup;
        }

    if ((ctx.artifactPolicy & TBCX_BLOCK_POLICY_STRICT_COMPILED_ONLY) != 0u && ctx.nonemptyExecutableSourceFieldCount != 0)
    {
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj("tbcx: strict artifact retained executable source", -1));
        Tcl_SetErrorCode(w->interp, "TBCX", "SAVE", "SOURCE", NULL);
        w->err = TCL_ERROR;
        goto cleanup;
    }

    Tbcx_W_Flush(w); /* flush buffered writes before returning */
    rc = (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

cleanup:
    if (ctx.authoredCommandsInit)
    {
        Tcl_DeleteHashTable(&ctx.authoredCommands);
        ctx.authoredCommandsInit = 0;
    }
    DV_Free(&defs);
    CS_Free(&classes);
    Tcl_DecrRefCount(srcCopy);
    Tcl_DecrRefCount(rootNs);
    return rc;
}

/* Set binary translation and an empty eofchar for .tbcx I/O. The channel
 * retains these settings after the operation. */
int Tbcx_CheckBinaryChan(Tcl_Interp* ip, Tcl_Channel ch)
{
    if (Tcl_SetChannelOption(ip, ch, "-translation", "binary") != TCL_OK)
        return TCL_ERROR;
    if (Tcl_SetChannelOption(ip, ch, "-eofchar", "") != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}

static int ReadAllFromChannel(Tcl_Interp* interp, Tcl_Channel ch, Tcl_Obj** outObjPtr)
{
    Tcl_Obj* dst = Tcl_NewObj();
    Tcl_IncrRefCount(dst);

    /* For seekable channels (files), determine remaining size and read in
       one large chunk instead of many 8 KB iterations.  This avoids
       repeated Tcl_Obj reallocation for scripts of tens/hundreds of KB.
       Non-seekable channels (pipes, sockets) fall through to chunked I/O. */
    Tcl_Size chunkSize = 8192;
    Tcl_WideInt curPos = Tcl_Tell(ch);
    if (curPos >= 0)
    {
        Tcl_WideInt endPos = Tcl_Seek(ch, 0, SEEK_END);
        if (endPos >= 0)
        {
            Tcl_WideInt remaining = endPos - curPos;
            if (Tcl_Seek(ch, curPos, SEEK_SET) < 0)
            {
                /* Seek-back failed after a successful seek-to-end.
                 * The channel is now positioned at EOF; falling through
                 * to chunked I/O would read 0 bytes and silently compile
                 * an empty script.  Treat as a hard error. */
                Tcl_DecrRefCount(dst);
                if (interp)
                    Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: seek-back failed after seek-to-end", -1));
                return TCL_ERROR;
            }
            if (remaining > 0 && remaining < (Tcl_WideInt)(64 * 1024 * 1024))
            {
                /* Read entire file in one call.  Add slack for encoding
                   expansion (UTF-8 -> internal can change length slightly). */
                chunkSize = (Tcl_Size)(remaining + 256);
            }
            else if (remaining >= (Tcl_WideInt)(64 * 1024 * 1024))
            {
                chunkSize = 256 * 1024; /* larger chunks for very big files */
            }
        }
    }

    while (1)
    {
        Tcl_Size nread = Tcl_ReadChars(ch, dst, chunkSize, /*appendFlag*/ 1);
        if (nread < 0)
        {
            Tcl_DecrRefCount(dst);
            if (interp)
            {
                const char* posixMsg = Tcl_PosixError(interp);
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: read error: %s", posixMsg));
            }
            return TCL_ERROR;
        }
        if (nread == 0)
        {
            break; /* EOF */
        }
    }

    *outObjPtr = dst;
    return TCL_OK;
}

int Tbcx_ProbeOpenChannel(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Channel* chPtr)
{
    const char* name = Tbcx_GetStringSafe(obj);
    /* Tcl_GetChannel may set error state; clear it if we fail, since we’ll try other forms. */
    Tcl_Channel ch = Tcl_GetChannel(interp, name, NULL);
    if (ch)
    {
        *chPtr = ch;
        return 1;
    }
    Tcl_ResetResult(interp);
    return 0;
}

int Tbcx_ProbeReadableFile(Tcl_Interp* interp, Tcl_Obj* pathObj)
{
    /* Normalize if possible (no hard failure if normalization fails). */
    Tcl_Obj* norm = Tcl_FSGetNormalizedPath(interp, pathObj);
    Tcl_ResetResult(interp); /* avoid leaking normalization messages into the decision */
    if (!norm)
        norm = pathObj;

    /* R_OK check: if it passes, we’ll attempt to open; open still authoritative. */
    if (Tcl_FSAccess(norm, R_OK) == 0)
    {
        return 1;
    }
    return 0;
}

/* ==========================================================================
 * Tcl command: tbcx::save
 *
 * Synopsis:   tbcx::save in out ?-include-source?
 * Arguments:  in  — Tcl script source: an open channel name, a filesystem
 *                    path to a .tcl file, or a literal script string.
 *             out — output destination: an open binary channel name, or a
 *                    filesystem path (written atomically via temp+rename).
 * Returns:    On success, the output path or channel name.
 * Errors:     TCL_ERROR on read/write failure, compilation failure, or
 *             unsupported AuxData types.  Sets interp result with details.
 * Thread:     Must be called on the interp-owning thread.  Uses
 *             tbcxSaveTmpMutex for unique temp-file naming (leaf lock).
 * ========================================================================== */

int Tbcx_SaveObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[])
{
    TBCX_CHECK_INTERP_THREAD(interp);

    /* The default policy writes empty proc, method, BYTESRC, and lambda
     * executable-source fields. -include-source writes their authored bytes
     * for introspection; execution uses the compiled representation in both
     * modes. */
    if (objc < 3 || objc > 4)
    {
        Tcl_WrongNumArgs(interp, 1, objv, "in out ?-include-source?");
        return TCL_ERROR;
    }
    unsigned saveFlags = 0;
    for (Tcl_Size i = 3; i < objc; i++)
    {
        const char* flag = Tbcx_GetStringStrict(interp, objv[i]);
        if (!flag)
        {
            return TCL_ERROR;
        }
        if (strcmp(flag, "-include-source") == 0)
        {
            saveFlags |= TBCX_SAVE_FL_INCLUDE_SOURCE;
        }
        else
        {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("tbcx::save: unknown option \"%s\"; "
                                           "expected -include-source",
                                           flag));
            return TCL_ERROR;
        }
    }

    Tcl_Obj* inObj = objv[1];
    Tcl_Obj* outObj = objv[2];

    Tcl_Channel inCh = NULL;
    Tcl_Obj* script = NULL;
    int rc = TCL_ERROR;
    Tcl_Obj* sourcePath = NULL;

    if (Tbcx_ProbeOpenChannel(interp, inObj, &inCh))
    {
        /* Input is a caller-provided channel.  Do NOT change its encoding
           settings — the caller may have configured an encoding for this
           script source.  Binary mode is only appropriate for the output
           (.tbcx) channel.  Read the script as text. */
        if (ReadAllFromChannel(interp, inCh, &script) != TCL_OK)
        {
            return TCL_ERROR;
        }
    }
    else if (Tbcx_ProbeReadableFile(interp, inObj))
    {
        /* Open in text mode (default UTF-8 encoding for Tcl 9.1) so that
           the script is properly decoded.  Binary mode would mishandle
           multi-byte UTF-8 sequences in Tcl_ReadChars. */
        Tcl_Channel tmp = Tcl_FSOpenFileChannel(interp, inObj, "r", 0);
        if (!tmp)
            return TCL_ERROR;
        if (ReadAllFromChannel(interp, tmp, &script) != TCL_OK)
        {
            Tcl_Close(interp, tmp);
            return TCL_ERROR;
        }
        if (Tcl_Close(interp, tmp) != TCL_OK)
        {
            Tcl_DecrRefCount(script);
            return TCL_ERROR;
        }
        /* Record the normalized source path so `info script` returns the
         * authored .tcl path at load time — matching source semantics. */
        sourcePath = Tcl_FSGetNormalizedPath(interp, inObj);
        if (!sourcePath)
        {
            sourcePath = inObj; /* fallback to as-given */
        }
        Tcl_IncrRefCount(sourcePath);
    }
    else
    {
        script = inObj;
        Tcl_IncrRefCount(script);
    }

    Tcl_Channel outCh = NULL;
    int weOpenedOut = 0;
    Tcl_Obj* tmpPath = NULL; /* temp file path for atomic write (weOpenedOut only) */

    if (Tbcx_ProbeOpenChannel(interp, outObj, &outCh))
    {
        /* Use caller-provided channel; do NOT close it. */
        if (Tbcx_CheckBinaryChan(interp, outCh) != TCL_OK)
        {
            Tcl_DecrRefCount(script);
            if (sourcePath)
                Tcl_DecrRefCount(sourcePath);
            return TCL_ERROR;
        }
    }
    else
    {
        /* Treat as path; write to a temp file in the same directory and
           rename on success so a failed serialization never leaves a
           truncated .tbcx at the final path. */
        Tcl_Obj* outNorm = Tcl_FSGetNormalizedPath(interp, outObj);
        if (!outNorm)
        {
            Tcl_DecrRefCount(script);
            if (sourcePath)
                Tcl_DecrRefCount(sourcePath);
            return TCL_ERROR;
        }
        /* Create the staging file exclusively in the destination directory.
           A process-local counter alone is insufficient: every process starts
           at zero and could otherwise truncate the same staging path. */
        for (unsigned attempt = 0; attempt < 256; attempt++)
        {
            Tcl_Time now;
            Tcl_GetTime(&now);
            Tcl_MutexLock(&tbcxSaveTmpMutex);
            uint64_t myTmpId = tbcxSaveTmpId++;
            Tcl_MutexUnlock(&tbcxSaveTmpMutex);

            uint64_t nonce = ((uint64_t)now.sec * UINT64_C(0x9E3779B97F4A7C15)) ^ ((uint64_t)now.usec << 21) ^
                             (myTmpId * UINT64_C(0xBF58476D1CE4E5B9)) ^ (uint64_t)(uintptr_t)interp ^ (uint64_t)attempt;
            Tcl_Obj* candidate = Tcl_DuplicateObj(outNorm);
            Tcl_Obj* suffix = Tcl_ObjPrintf(".tbcx.%016" PRIx64 ".tmp", nonce);
            Tcl_IncrRefCount(suffix);
            Tcl_AppendObjToObj(candidate, suffix);
            Tcl_DecrRefCount(suffix);
            Tcl_IncrRefCount(candidate);

            outCh = Tcl_FSOpenFileChannel(interp, candidate, "WRONLY CREAT EXCL", 0666);
            if (outCh)
            {
                tmpPath = candidate;
                break;
            }

            int openErr = Tcl_GetErrno();
            Tcl_DecrRefCount(candidate);
            if (openErr != EEXIST)
            {
                Tcl_DecrRefCount(script);
                if (sourcePath)
                    Tcl_DecrRefCount(sourcePath);
                return TCL_ERROR;
            }
            Tcl_ResetResult(interp);
        }
        if (!outCh)
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: unable to create a unique staging file", -1));
            Tcl_DecrRefCount(script);
            if (sourcePath)
                Tcl_DecrRefCount(sourcePath);
            return TCL_ERROR;
        }
        weOpenedOut = 1;
        if (Tbcx_CheckBinaryChan(interp, outCh) != TCL_OK)
        {
            Tcl_Close(interp, outCh);
            Tcl_FSDeleteFile(tmpPath);
            Tcl_DecrRefCount(tmpPath);
            Tcl_DecrRefCount(script);
            if (sourcePath)
                Tcl_DecrRefCount(sourcePath);
            return TCL_ERROR;
        }
    }

    TbcxOut w;
    Tbcx_W_Init(&w, interp, outCh);
    rc = EmitTbcxStream(script, &w, saveFlags, sourcePath);
    Tcl_DecrRefCount(script);
    if (sourcePath)
    {
        Tcl_DecrRefCount(sourcePath);
        sourcePath = NULL;
    }

    if (weOpenedOut)
    {
        if (Tcl_Close(interp, outCh) != TCL_OK)
            rc = TCL_ERROR;
    }

    if (rc == TCL_OK)
    {
        if (weOpenedOut)
        {
            /* Atomic rename: temp -> final destination */
            Tcl_Obj* outNorm = Tcl_FSGetNormalizedPath(interp, outObj);
            if (!outNorm)
            {
                /* Normalization failed — e.g. parent directory was deleted
                 * between write and rename.  Clean up temp and report. */
                Tcl_FSDeleteFile(tmpPath);
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: cannot normalize output path \"%s\"", Tbcx_GetStringSafe(outObj)));
                rc = TCL_ERROR;
            }
            else if (Tcl_FSRenameFile(tmpPath, outNorm) == TCL_OK)
            {
                Tcl_SetObjResult(interp, outNorm);
            }
            else
            {
                /* Rename failed — clean up temp and report */
                Tcl_FSDeleteFile(tmpPath);
                {
                    Tcl_Size errLen = 0;
                    (void)Tbcx_GetStringFromObjSafe(Tcl_GetObjResult(interp), &errLen);
                    if (errLen == 0)
                        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: failed to rename temp file to output path", -1));
                }
                rc = TCL_ERROR;
            }
            Tcl_DecrRefCount(tmpPath);
        }
        else
        {
            Tcl_SetObjResult(interp, outObj);
        }
    }
    else if (weOpenedOut)
    {
        /* Serialization failed — remove the partial temp file */
        Tcl_FSDeleteFile(tmpPath);
        Tcl_DecrRefCount(tmpPath);
    }
    return rc;
}
