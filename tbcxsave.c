/* ==========================================================================
 * tbcxsave.c — TBCX compile+save for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

/* Checked multiplication for allocation sizes.  Returns 1 on success
 * (result stored in *out), 0 if the multiplication would overflow size_t. */
static inline int tbcx_checked_mul(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/* Monotonic counter for unique temp-file naming (Finding #8).
 * Two concurrent tbcx::save calls to the same target no longer
 * clobber each other's staging file.
 * uint64_t to prevent wrap-around in long-running processes. */
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
    Tcl_HashTable stripBodies;
    int stripInit;
    int stripActive;
    /* compiled copies of namespace eval body literals, used only
       during serialization.  The actual literal pool is NEVER modified.
       Key: Tcl_Obj* (original literal), Value: Tcl_Obj* (compiled copy). */
    Tcl_HashTable compiledLits; /* ONE_WORD_KEYS */
    int compiledInit;
    /* namespace eval body text -> namespace FQN mapping.
       Value is NULL (sentinel) when the same body maps to multiple namespaces. */
    Tcl_HashTable nsEvalBodies; /* STRING_KEYS: body text -> Tcl_Obj* nsFqn or NULL */
    int nsEvalInit;
    int precompileDepth; /* recursion guard for on-the-fly precompilation in WriteLiteral */
    /* Track body texts already emitted as TBCX_LIT_BYTECODE (either
       from PrecompileLiteralPool or from on-the-fly precompilation).
       Second encounters are emitted as plain strings to prevent output
       explosion when many procs share the same namespace-eval body. */
    Tcl_HashTable emittedBodies; /* STRING_KEYS: body text -> (int)1 */
    int emittedInit;
    /* Pointer-based dedup for bytecode Tcl_Obj* — catches shared
       literals that have empty/invalidated string reps. */
    Tcl_HashTable emittedPtrs; /* ONE_WORD_KEYS: Tcl_Obj* -> 1 */
    int emittedPtrsInit;
    /* Improvement #1: instruction-level body literal detection.
       Maps Tcl_Obj* (literal pointer) -> Namespace* (compilation target).
       Populated per-block by InstrScanBodyLiterals Phase 1 (invokeStk
       analysis); checked by WriteLit_Untyped to precompile body arguments
       to eval-like commands (try, catch, eval, uplevel, time, timerate,
       dict for/map, self method).
       Phase 2 (foreach/lmap bodies) uses a separate per-block index array
       (phase2marks[]) to avoid Tcl_Obj* pointer sharing contamination. */
    Tcl_HashTable instrBodyLits; /* ONE_WORD_KEYS: Tcl_Obj* -> Namespace* */
    int instrBodyInit;
    /* Instrumentation for runaway detection */
    uint64_t totalLiterals; /* total WriteLiteral calls */
    uint64_t totalBlocks;   /* total WriteCompiledBlock calls */
    int blockDepth;         /* current WriteCompiledBlock recursion depth */
    int maxBlockDepth;      /* high-water mark */
    int runaway;            /* set to 1 when a limit is hit */
} TbcxCtx;

typedef struct
{
    const char* key;
    int targetOffset;
} JTEntry;

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
    int flags; /* bitmask: 1 = captured from oo::class builder */
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
#define DEF_F_SELF_METHOD  0x02  /* self method (class-level, emitted differently in stubs) */
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
                             int depth);
static Tcl_Obj* CaptureAndRewriteScript(
    Tcl_Interp* ip, const char* script, Tcl_Size len, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
static inline const char* CmdCore(const char* s);
static int CmpJTEntryUtf8_qsort(const void* pa, const void* pb);
static int CmpJTNumEntry_qsort(const void* pa, const void* pb);
static int CmpStrPtr_qsort(const void* pa, const void* pb);
static int CompileProcLike(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* nsFQN, Tcl_Obj* argsList, Tcl_Obj* bodyObj, const char* whereTag);
static uint32_t ComputeNumLocals(ByteCode* bc);
static void CS_Add(ClsSet* cs, Tcl_Obj* clsFqn);
static void CS_Free(ClsSet* cs);
static void CS_Init(ClsSet* cs);
static void CtxAddStripBody(TbcxCtx* ctx, Tcl_Obj* body);
static void CtxFreeStripBodies(TbcxCtx* ctx);
static void CtxInitStripBodies(TbcxCtx* ctx);
static void CtxInitCompiled(TbcxCtx* ctx);
static void CtxAddCompiled(TbcxCtx* ctx, Tcl_Obj* orig, Tcl_Obj* compiled);
static Tcl_Obj* CtxGetCompiled(TbcxCtx* ctx, Tcl_Obj* orig);
static void CtxFreeCompiled(TbcxCtx* ctx);
static void CtxInitNsEval(TbcxCtx* ctx);
static void CtxAddNsEval(TbcxCtx* ctx, const char* bodyText, Tcl_Size bodyLen, Tcl_Obj* nsFqn);
static void CtxFreeNsEval(TbcxCtx* ctx);
static void PrecompileLiteralPool(TbcxCtx* ctx, ByteCode* top);
static void ScanForNsEvalBodies(TbcxCtx* ctx, const char* script, Tcl_Size len);
static void ScanScriptBodiesRec(TbcxCtx* ctx, const char* script, Tcl_Size len, Tcl_Obj* curNs, int depth);
static void RegisterBodyAndRecurse(TbcxCtx* ctx, const Tcl_Token* tok, Tcl_Obj* curNs, int depth);
static Tcl_Obj*
RecurseScriptBody(Tcl_Interp* ip, const Tcl_Token* bodyTok, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
static void DV_Free(DefVec* dv);
static void DV_Init(DefVec* dv);
static void DV_Push(DefVec* dv, DefRec r);
static int EmitTbcxStream(Tcl_Obj* scriptObj, TbcxOut* w);
static Tcl_Obj* FqnUnder(Tcl_Interp* ip, Tcl_Obj* curNs, Tcl_Obj* name);
static int IsPureOodefineBuilderBody(Tcl_Interp* ip, const char* script, Tcl_Size len);
static void Lit_Bignum(TbcxOut* w, Tcl_Obj* o);
static void Lit_Bytecode(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj);
static void Lit_Dict(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o);
static void Lit_LambdaBC(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* lambda);
static void Lit_List(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* o);
static inline const Tcl_Token* NextWord(const Tcl_Token* wordTok);
static Tcl_Obj* NsFqn(Tcl_Namespace* nsPtr);
static int ReadAllFromChannel(Tcl_Interp* interp, Tcl_Channel ch, Tcl_Obj** outObjPtr);
static Tcl_Obj* ResolveToBytecodeObj(Tcl_Obj* cand);
static int ShouldStripBody(TbcxCtx* ctx, Tcl_Obj* obj);
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
static void WriteCompiledBlock(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj);
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

static void CtxInitStripBodies(TbcxCtx* ctx)
{
    if (!ctx)
        return;
    Tcl_InitHashTable(&ctx->stripBodies, TCL_STRING_KEYS);
    ctx->stripInit = 1;
    ctx->stripActive = 0;
}

static void CtxAddStripBody(TbcxCtx* ctx, Tcl_Obj* body)
{
    if (!ctx || !ctx->stripInit || !body)
        return;
    Tcl_Size len = 0;
    const char* s = Tbcx_GetStringFromObjSafe(body, &len);
    if (!s)
        return;
    /* TCL_STRING_KEYS truncates at embedded NUL — skip optimization for
       binary body text to avoid false collisions. */
    if (len > 0 && memchr(s, '\0', (size_t)len) != NULL)
        return;
    int isNew;
    /* For TCL_STRING_KEYS, Tcl_CreateHashEntry() *copies* the key internally. */
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&ctx->stripBodies, (const char*)s, &isNew);
    if (isNew)
    {
        Tcl_SetHashValue(he, (ClientData)(uintptr_t)len);
    }
}

static int ShouldStripBody(TbcxCtx* ctx, Tcl_Obj* obj)
{
    if (!ctx || !ctx->stripInit || !ctx->stripActive || !obj)
        return 0;
    Tcl_Size len = 0;
    const char* s = Tbcx_GetStringFromObjSafe(obj, &len);
    if (!s)
        return 0;
    /* TCL_STRING_KEYS truncates at embedded NUL — never match binary bodies. */
    if (len > 0 && memchr(s, '\0', (size_t)len) != NULL)
        return 0;
    Tcl_HashEntry* he = Tcl_FindHashEntry(&ctx->stripBodies, (const char*)s);
    if (!he)
        return 0;
    uintptr_t storedLen = (uintptr_t)Tcl_GetHashValue(he);
    return (storedLen == (uintptr_t)len) ? 1 : 0;
}

static void CtxFreeStripBodies(TbcxCtx* ctx)
{
    if (!ctx || !ctx->stripInit)
        return;
    Tcl_DeleteHashTable(&ctx->stripBodies);
    ctx->stripInit = 0;
    ctx->stripActive = 0;
}

/* ==========================================================================
 * Compiled-lits side-table — compiled copies used only at serialization.
 * The literal pool is NEVER modified.  Key: Tcl_Obj* (original literal
 * pointer from objArrayPtr), Value: Tcl_Obj* (compiled copy).
 * ========================================================================== */

static void CtxInitCompiled(TbcxCtx* ctx)
{
    if (!ctx)
        return;
    Tcl_InitHashTable(&ctx->compiledLits, TCL_ONE_WORD_KEYS);
    ctx->compiledInit = 1;
}

static void CtxAddCompiled(TbcxCtx* ctx, Tcl_Obj* orig, Tcl_Obj* compiled)
{
    if (!ctx || !ctx->compiledInit || !orig || !compiled)
        return;
    int isNew;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&ctx->compiledLits, (const char*)orig, &isNew);
    if (isNew)
    {
        Tcl_IncrRefCount(compiled);
        Tcl_SetHashValue(he, (ClientData)compiled);
    }
}

static Tcl_Obj* CtxGetCompiled(TbcxCtx* ctx, Tcl_Obj* orig)
{
    if (!ctx || !ctx->compiledInit || !orig)
        return NULL;
    Tcl_HashEntry* he = Tcl_FindHashEntry(&ctx->compiledLits, (const char*)orig);
    if (!he)
        return NULL;
    return (Tcl_Obj*)Tcl_GetHashValue(he);
}

static void CtxFreeCompiled(TbcxCtx* ctx)
{
    if (!ctx || !ctx->compiledInit)
        return;
    Tcl_HashSearch s;
    for (Tcl_HashEntry* he = Tcl_FirstHashEntry(&ctx->compiledLits, &s); he; he = Tcl_NextHashEntry(&s))
    {
        Tcl_Obj* v = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (v)
            Tcl_DecrRefCount(v);
    }
    Tcl_DeleteHashTable(&ctx->compiledLits);
    ctx->compiledInit = 0;
}

/* ==========================================================================
 * Namespace eval body -> namespace FQN mapping.
 * Value is NULL (sentinel) when the same body text maps to multiple
 * namespaces - such bodies are NOT pre-compiled
 * ========================================================================== */

static void CtxInitNsEval(TbcxCtx* ctx)
{
    if (!ctx)
        return;
    Tcl_InitHashTable(&ctx->nsEvalBodies, TCL_STRING_KEYS);
    ctx->nsEvalInit = 1;
}

static void CtxAddNsEval(TbcxCtx* ctx, const char* bodyText, Tcl_Size bodyLen, Tcl_Obj* nsFqn)
{
    if (!ctx || !ctx->nsEvalInit || !bodyText || !nsFqn)
        return;
    /* TCL_STRING_KEYS truncates at embedded NUL — skip precompile side-table
       for binary body text to avoid false collisions. */
    if (bodyLen > 0 && memchr(bodyText, '\0', (size_t)bodyLen) != NULL)
        return;
    int isNew;
    Tcl_HashEntry* he = Tcl_CreateHashEntry(&ctx->nsEvalBodies, bodyText, &isNew);
    if (isNew)
    {
        Tcl_IncrRefCount(nsFqn);
        Tcl_SetHashValue(he, (ClientData)nsFqn);
    }
    else
    {
        /* Same body text seen for a second namespace.  Check if it's the
           same namespace (OK) or different (conflict -> mark as NULL). */
        Tcl_Obj* existing = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (existing)
        {
            Tcl_Size eLen, nLen;
            const char* e = Tbcx_GetStringFromObjSafe(existing, &eLen);
            const char* n = Tbcx_GetStringFromObjSafe(nsFqn, &nLen);
            if (eLen != nLen || memcmp(e, n, (size_t)eLen) != 0)
            {
                /* Conflict: different namespaces share same body text.
                   Mark with NULL so PrecompileLiteralPool skips it. */
                Tcl_DecrRefCount(existing);
                Tcl_SetHashValue(he, NULL);
            }
        }
        /* If already NULL (previous conflict), leave it. */
    }
}

static void CtxFreeNsEval(TbcxCtx* ctx)
{
    if (!ctx || !ctx->nsEvalInit)
        return;
    Tcl_HashSearch s;
    for (Tcl_HashEntry* he = Tcl_FirstHashEntry(&ctx->nsEvalBodies, &s); he; he = Tcl_NextHashEntry(&s))
    {
        Tcl_Obj* v = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (v)
            Tcl_DecrRefCount(v);
    }
    Tcl_DeleteHashTable(&ctx->nsEvalBodies);
    ctx->nsEvalInit = 0;
}

/* ==========================================================================
 * Scan rewritten script for ::tcl::namespace::eval commands to build
 * the body text -> namespace FQN mapping used by PrecompileLiteralPool.
 * ========================================================================== */

/* Helper: register a word token's literal text as a script body for
   pre-compilation, then recurse into it to find nested bodies.

   SAFETY: only register strings that look like multi-command scripts or
   contain substitutions / multiple words.  Bare command names like
   "custom_error" must NOT be registered, because the string may be
   shared in Tcl's literal pool with non-script uses (proc names,
   variable names, etc.).  Pre-compiling a shared literal corrupts
   every use of it. */
static void RegisterBodyAndRecurse(TbcxCtx* ctx, const Tcl_Token* tok, Tcl_Obj* curNs, int depth)
{
    Tcl_Obj* bodyObj = WordLiteralObj(tok);
    if (!bodyObj)
        return;
    Tcl_Size bl = 0;
    const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
    int hasScriptIndicator = 0;
    int hasDollar = 0;
    for (Tcl_Size i = 0; i < bl; i++)
    {
        char ch = bs[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ';' || ch == '$' || ch == '[')
        {
            hasScriptIndicator = 1;
            if (ch == '$')
                hasDollar = 1;
        }
    }
    if (bl >= 2 && hasScriptIndicator)
    {
        /* Bodies containing $ (variable references) are NOT registered
           for pre-compilation.  These bodies execute in the CALLER's
           frame (try body, foreach body, catch body, etc.).  When the
           caller is a proc, pre-compiling the body as a standalone script
           compiles $var as a namespace variable lookup instead of a
           proc-local load, breaking variable resolution at runtime.
           We still recurse into the body to find nested patterns
           (namespace eval, etc.) that CAN be safely pre-compiled.
           Note: namespace eval bodies are registered by the explicit
           handler in ScanScriptBodiesRec, not via this function. */
        if (!hasDollar)
            CtxAddNsEval(ctx, bs, bl, curNs);
        ScanScriptBodiesRec(ctx, bs, bl, curNs, depth + 1);
    }
    Tcl_DecrRefCount(bodyObj);
}

/* Recursive inner scanner: walks the rewritten script looking for
   script body literals that should be pre-compiled to bytecode.
   Handles namespace eval, try/on/trap/finally, foreach, while,
   for, catch, and if/elseif/else. */
static void ScanScriptBodiesRec(TbcxCtx* ctx, const char* script, Tcl_Size len, Tcl_Obj* curNs, int depth)
{
    if (!ctx || !ctx->nsEvalInit || !script || len <= 0)
        return;
    /* Guard against unbounded recursion from pathologically nested scripts */
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return;
    Tcl_Interp* ip = ctx->interp;
    Tcl_Parse p;
    const char* cur = script;
    Tcl_Size remain = len;

    while (remain > 0)
    {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK)
        {
            Tcl_FreeParse(&p);
            /* Skip to next newline and continue scanning rather than
               aborting.  Parse errors here are non-fatal — any bodies
               we miss will just remain as string literals. */
            while (remain > 0 && *cur != '\n')
            {
                cur++;
                remain--;
            }
            if (remain > 0)
            {
                cur++;
                remain--;
            }
            continue;
        }
        if (p.numWords >= 1)
        {
            const Tcl_Token* w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj* cmd = WordLiteralObj(w0);
            if (cmd)
            {
                const char* c0 = Tbcx_GetStringSafe(cmd);

                /* ---- ::tcl::namespace::eval ns body ---- */
                if (p.numWords == 3 && strcmp(c0, "::tcl::namespace::eval") == 0)
                {
                    const Tcl_Token* wNs = NextWord(w0);
                    const Tcl_Token* wBody = NextWord(wNs);
                    Tcl_Obj* nsObj = WordLiteralObj(wNs);
                    Tcl_Obj* bodyObj = WordLiteralObj(wBody);
                    if (nsObj && bodyObj)
                    {
                        Tcl_Size bl;
                        const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
                        const char* nsn = Tbcx_GetStringSafe(nsObj);
                        Tcl_Obj* nsFqn;
                        if (nsn[0] == ':' && nsn[1] == ':')
                        {
                            nsFqn = nsObj;
                            Tcl_IncrRefCount(nsFqn);
                        }
                        else
                        {
                            nsFqn = Tcl_NewStringObj("::", -1);
                            Tcl_AppendObjToObj(nsFqn, nsObj);
                            Tcl_IncrRefCount(nsFqn);
                        }
                        CtxAddNsEval(ctx, bs, bl, nsFqn);
                        ScanScriptBodiesRec(ctx, bs, bl, nsFqn, depth + 1);
                        Tcl_DecrRefCount(nsFqn);
                    }
                    if (nsObj)
                        Tcl_DecrRefCount(nsObj);
                    if (bodyObj)
                        Tcl_DecrRefCount(bodyObj);
                }

                /* ---- namespace eval ns body (4-word, unrewritten) ---- */
                else if (p.numWords == 4 && (strcmp(c0, "namespace") == 0 || strcmp(c0, "::namespace") == 0))
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
                            Tcl_Size bl;
                            const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
                            Tcl_Obj* nsFqn = FqnUnder(ctx->interp, curNs, nsObj);
                            if (bs && nsFqn)
                            {
                                CtxAddNsEval(ctx, bs, bl, nsFqn);
                                ScanScriptBodiesRec(ctx, bs, bl, nsFqn, depth + 1);
                            }
                            if (nsFqn)
                                Tcl_DecrRefCount(nsFqn);
                        }
                        if (nsObj)
                            Tcl_DecrRefCount(nsObj);
                        if (bodyObj)
                            Tcl_DecrRefCount(bodyObj);
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                }

                /* ---- try body ?on/trap code varList body? ?finally body? ---- */
                else if (strcmp(c0, "try") == 0 && p.numWords >= 2)
                {
                    const Tcl_Token* wBody = NextWord(w0);
                    RegisterBodyAndRecurse(ctx, wBody, curNs, depth);
                    /* Walk remaining words for handler/finally bodies */
                    const Tcl_Token* wt = wBody;
                    int w = 2;
                    while (w < p.numWords)
                    {
                        wt = NextWord(wt);
                        Tcl_Obj* kw = WordLiteralObj(wt);
                        if (!kw)
                            break;
                        const char* ks = Tbcx_GetStringSafe(kw);
                        if ((strcmp(ks, "on") == 0 || strcmp(ks, "trap") == 0) && w + 3 < p.numWords)
                        {
                            /* on/trap  code/pattern  varList  body */
                            Tcl_DecrRefCount(kw);
                            wt = NextWord(wt);
                            w++; /* code/pattern */
                            wt = NextWord(wt);
                            w++; /* varList */
                            wt = NextWord(wt);
                            w++; /* handler body */
                            RegisterBodyAndRecurse(ctx, wt, curNs, depth);
                            w++;
                        }
                        else if (strcmp(ks, "finally") == 0 && w + 1 < p.numWords)
                        {
                            Tcl_DecrRefCount(kw);
                            wt = NextWord(wt);
                            w++; /* finally body */
                            RegisterBodyAndRecurse(ctx, wt, curNs, depth);
                            break;
                        }
                        else
                        {
                            Tcl_DecrRefCount(kw);
                            break;
                        }
                    }
                }

                /* ---- foreach ... body  (last word is the body) ---- */
                else if (strcmp(c0, "foreach") == 0 && p.numWords >= 4)
                {
                    const Tcl_Token* tok = w0;
                    for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                        tok = NextWord(tok);
                    RegisterBodyAndRecurse(ctx, tok, curNs, depth);
                }

                /* ---- while cond body ---- */
                else if (strcmp(c0, "while") == 0 && p.numWords == 3)
                {
                    const Tcl_Token* wCond = NextWord(w0);
                    const Tcl_Token* wBody = NextWord(wCond);
                    RegisterBodyAndRecurse(ctx, wBody, curNs, depth);
                }

                /* ---- for init cond next body ---- */
                else if (strcmp(c0, "for") == 0 && p.numWords == 5)
                {
                    const Tcl_Token* w1 = NextWord(w0);
                    const Tcl_Token* w2 = NextWord(w1);
                    const Tcl_Token* w3 = NextWord(w2);
                    const Tcl_Token* wBody = NextWord(w3);
                    RegisterBodyAndRecurse(ctx, wBody, curNs, depth);
                }

                /* ---- catch body ... ---- */
                else if (strcmp(c0, "catch") == 0 && p.numWords >= 2)
                {
                    const Tcl_Token* wBody = NextWord(w0);
                    RegisterBodyAndRecurse(ctx, wBody, curNs, depth);
                }

                /* ---- uplevel ?level? body ----
                   The uplevel body itself must remain a string (it executes
                   in the caller's scope, not the compiled namespace).  But
                   we DO recurse into it to find nested patterns like
                   namespace eval or try that CAN be precompiled. */
                else if (strcmp(c0, "uplevel") == 0 && p.numWords >= 2 && p.numWords <= 3)
                {
                    const Tcl_Token* tok = w0;
                    for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                        tok = NextWord(tok);
                    Tcl_Obj* bodyObj = WordLiteralObj(tok);
                    if (bodyObj)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
                        if (bl >= 2)
                            ScanScriptBodiesRec(ctx, bs, bl, curNs, depth + 1);
                        Tcl_DecrRefCount(bodyObj);
                    }
                }

                /* ---- eval body ----
                   Same as uplevel: body must stay as string for runtime
                   scope, but recurse for nested precompilable patterns. */
                else if (strcmp(c0, "eval") == 0 && p.numWords == 2)
                {
                    const Tcl_Token* wBody = NextWord(w0);
                    Tcl_Obj* bodyObj = WordLiteralObj(wBody);
                    if (bodyObj)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
                        if (bl >= 2)
                            ScanScriptBodiesRec(ctx, bs, bl, curNs, depth + 1);
                        Tcl_DecrRefCount(bodyObj);
                    }
                }

                /* ---- if cond body ?elseif cond body? ?else body? ---- */
                else if (strcmp(c0, "if") == 0 && p.numWords >= 3)
                {
                    const Tcl_Token* wt = NextWord(w0); /* cond */
                    wt = NextWord(wt);                  /* body (or "then") */
                    int w = 3;
                    /* Skip optional "then" keyword */
                    Tcl_Obj* probe = WordLiteralObj(wt);
                    if (probe)
                    {
                        if (strcmp(Tbcx_GetStringSafe(probe), "then") == 0 && w < p.numWords)
                        {
                            wt = NextWord(wt);
                            w++;
                        }
                        Tcl_DecrRefCount(probe);
                    }
                    RegisterBodyAndRecurse(ctx, wt, curNs, depth);
                    /* Walk elseif/else clauses */
                    while (w < p.numWords)
                    {
                        wt = NextWord(wt);
                        w++;
                        Tcl_Obj* kw = WordLiteralObj(wt);
                        if (!kw)
                            break;
                        const char* ks = Tbcx_GetStringSafe(kw);
                        if (strcmp(ks, "elseif") == 0 && w + 1 < p.numWords)
                        {
                            Tcl_DecrRefCount(kw);
                            wt = NextWord(wt);
                            w++; /* cond */
                            wt = NextWord(wt);
                            w++; /* body or "then" */
                            probe = WordLiteralObj(wt);
                            if (probe)
                            {
                                if (strcmp(Tbcx_GetStringSafe(probe), "then") == 0 && w < p.numWords)
                                {
                                    wt = NextWord(wt);
                                    w++;
                                }
                                Tcl_DecrRefCount(probe);
                            }
                            RegisterBodyAndRecurse(ctx, wt, curNs, depth);
                        }
                        else if (strcmp(ks, "else") == 0 && w < p.numWords)
                        {
                            Tcl_DecrRefCount(kw);
                            wt = NextWord(wt);
                            w++;
                            RegisterBodyAndRecurse(ctx, wt, curNs, depth);
                            break;
                        }
                        else
                        {
                            Tcl_DecrRefCount(kw);
                            break;
                        }
                    }
                }

                /* ---- oo::define cls { builder body } ----
                   Register the builder body for pre-compilation so that
                   OO class structure strings are converted to bytecode
                   for obscurity. */
                else if (strcmp(c0, "oo::define") == 0 && p.numWords == 3)
                {
                    const Tcl_Token* wCls = NextWord(w0);
                    const Tcl_Token* wBod = NextWord(wCls);
                    RegisterBodyAndRecurse(ctx, wBod, curNs, depth);
                }

                /* ---- oo::class/oo::abstract/oo::singleton create name {builder} ---- */
                else if ((strcmp(c0, "oo::class") == 0 || strcmp(c0, "oo::abstract") == 0 || strcmp(c0, "oo::singleton") == 0) &&
                         p.numWords >= 4)
                {
                    const Tcl_Token* w1 = NextWord(w0);
                    Tcl_Obj* sub = WordLiteralObj(w1);
                    if (sub)
                    {
                        if (strcmp(Tbcx_GetStringSafe(sub), "create") == 0 && p.numWords >= 4)
                        {
                            const Tcl_Token* w2 = NextWord(w1);
                            const Tcl_Token* w3 = NextWord(w2);
                            RegisterBodyAndRecurse(ctx, w3, curNs, depth);
                        }
                        Tcl_DecrRefCount(sub);
                    }
                }

                /* ---- oo::objdefine obj { builder body } ----
                   When the target is a variable ($obj), CaptureAndRewriteScript
                   leaves the command verbatim; register the builder body here
                   so that PrecompileLiteralPool can convert it to bytecode. */
                else if (strcmp(c0, "oo::objdefine") == 0 && p.numWords == 3)
                {
                    const Tcl_Token* wObj = NextWord(w0);
                    const Tcl_Token* wBod = NextWord(wObj);
                    RegisterBodyAndRecurse(ctx, wBod, curNs, depth);
                }

                /* ---- lmap varList list body ---- (like foreach, last word is body) */
                else if (strcmp(c0, "lmap") == 0 && p.numWords >= 4)
                {
                    const Tcl_Token* tok = w0;
                    for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                        tok = NextWord(tok);
                    RegisterBodyAndRecurse(ctx, tok, curNs, depth);
                }

                /* ---- dict for {key val} dict body ---- */
                else if (strcmp(c0, "dict") == 0 && p.numWords >= 2)
                {
                    const Tcl_Token* w1 = NextWord(w0);
                    Tcl_Obj* sub = WordLiteralObj(w1);
                    if (sub)
                    {
                        const char* sc = Tbcx_GetStringSafe(sub);
                        /* dict for {k v} dict body  — 5 words total */
                        if ((strcmp(sc, "for") == 0 || strcmp(sc, "map") == 0) && p.numWords == 5)
                        {
                            const Tcl_Token* tok = w0;
                            for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                                tok = NextWord(tok);
                            RegisterBodyAndRecurse(ctx, tok, curNs, depth);
                        }
                        /* dict with dictVar ?key ...? body — last word is body, >=3 words */
                        else if (strcmp(sc, "with") == 0 && p.numWords >= 4)
                        {
                            const Tcl_Token* tok = w0;
                            for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                                tok = NextWord(tok);
                            RegisterBodyAndRecurse(ctx, tok, curNs, depth);
                        }
                        Tcl_DecrRefCount(sub);
                    }
                }

                /* ---- switch ?opts? string {pat body ...} ----
                   Bodies are in alternating pattern/body pairs in the
                   last word (dict-style).  Scan each body (even positions). */
                else if (strcmp(c0, "switch") == 0 && p.numWords >= 3)
                {
                    /* Last word is the pattern/body dictionary */
                    const Tcl_Token* tok = w0;
                    for (Tcl_Size fw = 0; fw + 1 < p.numWords; fw++)
                        tok = NextWord(tok);
                    Tcl_Obj* dictBody = WordLiteralObj(tok);
                    if (dictBody)
                    {
                        Tcl_Size nElems = 0;
                        Tcl_Obj** elems = NULL;
                        if (Tcl_ListObjGetElements(NULL, dictBody, &nElems, &elems) == TCL_OK && nElems >= 2 && (nElems % 2) == 0)
                        {
                            for (Tcl_Size si = 1; si < nElems; si += 2)
                            {
                                Tcl_Size bl = 0;
                                const char* bs = Tbcx_GetStringFromObjSafe(elems[si], &bl);
                                if (bl >= 2)
                                {
                                    int hasScriptInd = 0, hasDollar = 0;
                                    for (Tcl_Size bi = 0; bi < bl; bi++)
                                    {
                                        char ch = bs[bi];
                                        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ';' || ch == '$' ||
                                            ch == '[')
                                            hasScriptInd = 1;
                                        if (ch == '$')
                                            hasDollar = 1;
                                    }
                                    if (hasScriptInd && !hasDollar)
                                        CtxAddNsEval(ctx, bs, bl, curNs);
                                    if (hasScriptInd)
                                        ScanScriptBodiesRec(ctx, bs, bl, curNs, depth + 1);
                                }
                            }
                        }
                        Tcl_DecrRefCount(dictBody);
                    }
                }

                /* ---- lsort -command cmd list ----
                   When -command is present, the command prefix may embed
                   a lambda or script body. Scan the -command value. */
                else if (strcmp(c0, "lsort") == 0 && p.numWords >= 3)
                {
                    const Tcl_Token* wt = w0;
                    int w = 1;
                    while (w < p.numWords)
                    {
                        wt = NextWord(wt);
                        Tcl_Obj* opt = WordLiteralObj(wt);
                        if (opt)
                        {
                            const char* os = Tbcx_GetStringSafe(opt);
                            if (strcmp(os, "-command") == 0 && w + 1 < p.numWords)
                            {
                                Tcl_DecrRefCount(opt);
                                wt = NextWord(wt);
                                w++;
                                /* The -command value — scan for nested patterns */
                                Tcl_Obj* cmdVal = WordLiteralObj(wt);
                                if (cmdVal)
                                {
                                    Tcl_Size cl = 0;
                                    const char* cs = Tbcx_GetStringFromObjSafe(cmdVal, &cl);
                                    if (cl >= 2)
                                        ScanScriptBodiesRec(ctx, cs, cl, curNs, depth + 1);
                                    Tcl_DecrRefCount(cmdVal);
                                }
                                break;
                            }
                            Tcl_DecrRefCount(opt);
                        }
                        w++;
                    }
                }

                Tcl_DecrRefCount(cmd);
            }

            /* ---- Recurse into command substitutions ----
               Commands like [try {body} on ok {v} {handler}] inside
               set result [try ...] are invisible to top-level parsing.
               Walk every sub-token looking for TCL_TOKEN_COMMAND and
               recursively scan its content. */
            {
                const Tcl_Token* wtk = p.tokenPtr;
                if (wtk->type == TCL_TOKEN_COMMAND)
                    wtk++; /* skip Tcl 9.x command envelope */
                for (Tcl_Size wIdx = 0; wIdx < p.numWords; wIdx++)
                {
                    for (int sub = 0; sub < wtk->numComponents; sub++)
                    {
                        const Tcl_Token* st = wtk + 1 + sub;
                        if (st->type == TCL_TOKEN_COMMAND && st->size >= 2)
                        {
                            /* Token spans [cmd]; inner text is start+1 .. start+size-2 */
                            ScanScriptBodiesRec(ctx, st->start + 1, st->size - 2, curNs, depth + 1);
                        }
                    }
                    wtk = NextWord(wtk);
                }
            }
        }
        cur = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
    Tcl_ResetResult(ip);
}

/* Public entry point: scans the rewritten script from the root namespace. */
static void ScanForNsEvalBodies(TbcxCtx* ctx, const char* script, Tcl_Size len)
{
    Tcl_Obj* rootNs = Tcl_NewStringObj("::", -1);
    Tcl_IncrRefCount(rootNs);
    ScanScriptBodiesRec(ctx, script, len, rootNs, 0);
    Tcl_DecrRefCount(rootNs);
}

/* ==========================================================================
 * Pre-compile namespace eval body literals into a SIDE TABLE.
 *
 * The literal pool (top->objArrayPtr) is NEVER modified.  Instead,
 * compiled copies are stored in ctx->compiledLits.  WriteLiteral
 * checks this table and emits TBCX_LIT_BYTECODE for matched literals.
 *
 * This avoids two classes of bugs:
 *  - Corrupting the interpreter's literal table (string.5-type failures)
 *  - Changing runtime behavior of commands that dispatch bodies via
 *    invokeStk (try, oo::define, interp eval, etc.)
 *
 * ========================================================================== */

static void PrecompileLiteralPool(TbcxCtx* ctx, ByteCode* top)
{
    if (!ctx || !top || !ctx->nsEvalInit || !ctx->compiledInit)
        return;

    for (Tcl_Size i = 0; i < top->numLitObjects; i++)
    {
        Tcl_Obj* lit = top->objArrayPtr[i];
        if (!lit)
            continue;
        const Tcl_ObjType* ty = lit->typePtr;

        /* Skip non-string literals */
        if (ty == tbcxTyBytecode || ty == tbcxTyProcBody || (tbcxTyLambda != NULL && ty == tbcxTyLambda))
            continue;
        if (Tcl_FetchInternalRep(lit, tbcxTyBytecode) != NULL)
            continue;
        if (ty == tbcxTyInt || ty == tbcxTyDouble || ty == tbcxTyBoolean || ty == tbcxTyBignum || ty == tbcxTyByteArray ||
            (tbcxTyDict != NULL && ty == tbcxTyDict) || ty == tbcxTyList)
            continue;

        Tcl_Size sLen = 0;
        const char* s = Tbcx_GetStringFromObjSafe(lit, &sLen);
        if (sLen < 2)
            continue;

        /* P1 only: check tracked namespace eval body map */
        Tcl_HashEntry* he = Tcl_FindHashEntry(&ctx->nsEvalBodies, s);
        if (!he && sLen > 4)
        {
            /* Fallback: try with leading/trailing whitespace trimmed. */
            const char* ts = s;
            Tcl_Size tLen = sLen;
            while (tLen > 0 && ((unsigned char)*ts <= ' '))
            {
                ts++;
                tLen--;
            }
            while (tLen > 0 && ((unsigned char)ts[tLen - 1] <= ' '))
                tLen--;
            if (tLen != sLen && tLen > 0)
            {
                Tcl_Obj* trimmed = Tcl_NewStringObj(ts, tLen);
                Tcl_IncrRefCount(trimmed);
                he = Tcl_FindHashEntry(&ctx->nsEvalBodies, Tbcx_GetStringSafe(trimmed));
                Tcl_DecrRefCount(trimmed);
            }
        }
        if (!he)
            continue;
        Tcl_Obj* nsObj = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (!nsObj)
            continue; /* NULL = multi-namespace conflict, skip */

        Namespace* targetNs = (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tbcx_GetStringSafe(nsObj));
        if (!targetNs)
            continue;

        /* Compile a SEPARATE copy — never touch the pool. */
        Tcl_Obj* copy = Tcl_DuplicateObj(lit);
        Tcl_IncrRefCount(copy);
        if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
        {
            ByteCode* bc = NULL;
            bc = TbcxGetByteCode(copy);
            if (!bc)
            {
                /* Tcl 9.1 slot-based lookup fallback */
                const Tcl_ObjInternalRep* ir = Tcl_FetchInternalRep(copy, tbcxTyBytecode);
                if (ir)
                    bc = (ByteCode*)ir->twoPtrValue.ptr1;
            }
            if (bc)
            {
                bc->nsPtr = targetNs;
                bc->nsEpoch = targetNs->resolverEpoch;
                /* Store in side-table: original pointer -> compiled copy.
                   WriteLiteral will find this and emit TBCX_LIT_BYTECODE. */
                CtxAddCompiled(ctx, lit, copy);
            }
        }
        else
        {
            Tcl_ResetResult(ctx->interp);
        }
        Tcl_DecrRefCount(copy); /* CtxAddCompiled held its own ref if successful */
    }
}

/* ==========================================================================
 * Scrub remaining string literals containing definition commands
 *
 * Defence-in-depth: after pre-compilation, any string literal still
 * containing proc/method/constructor/destructor definitions has its text
 * registered for stripping via the stripBodies mechanism.
 * ========================================================================== */

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
        const char* nsName = Tbcx_GetStringSafe(nsObjIn);
        nsPtr = Tcl_FindNamespace(ctx->interp, nsName, NULL, 0);
        if (!nsPtr)
        {
            nsPtr = Tcl_CreateNamespace(ctx->interp, nsName, NULL, NULL);
        }
        nsFQN = Tcl_NewStringObj(nsName, -1);
        Tcl_IncrRefCount(nsFQN);
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
    procPtr->numArgs = (int)argc;
    procPtr->numCompiledLocals = (int)argc;
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
    if (TclProcCompileProc(ctx->interp, procPtr, bodyObj, (Namespace*)nsPtr, "body of lambda term", "lambdaExpr") != TCL_OK)
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
            WriteCompiledBlock(w, ctx, bcObj);
        }
    }

    /* Emit original body source text so the loader can reconstruct
       a faithful string rep.  Without this, the lambda's string rep
       would be "{args {}} ?ns?" — breaking code that interpolates
       lambda variables into dynamic strings (compose patterns). */
    {
        Tcl_Size bLen = 0;
        const char* bStr = Tbcx_GetStringFromObjSafe(bodyObj, &bLen);
        W_LPString(w, bStr, bLen);
    }

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
     *   procPtr → owned by ByteCode via bc->procPtr = procPtr
     *   ByteCode → owned by bodyObj (or procPtr->bodyPtr) via internal rep
     *   bodyObj  → on the Tcl stack, refcount held by caller (Tcl_IncrRefCount
     *              at line above), will be freed when that refcount drops.
     *   TclCleanupByteCode frees both the ByteCode and the attached Proc.
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
    WriteCompiledBlock(w, ctx, bcObj);
    Tcl_DecrRefCount(nsFQN);
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
    if (TclProcCompileProc(ip, procPtr, bodyObj, (Namespace*)ns, whereTag, "proc") != TCL_OK)
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
        WriteCompiledBlock(w, ctx, bcObj);
    }
    Tcl_DecrRefCount(bodyObj);
    /* The Tcl compiler attaches procPtr to the ByteCode (bc->procPtr = procPtr).
       The ByteCode owns the Proc and will free it when cleaned up via
       TclCleanupByteCode.  Do NOT free procPtr or its locals here. */
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
        /* F3 fix: overflow-safe multiply — n from numCompiledLocals */
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
       This fixes "can't read '': no such variable" for scan/regexp/dict/uplevel. */
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

/* WriteLit_Untyped — handles literals with no internal rep or with an
 * unrecognized type pointer.  Performs lambda detection, side-table
 * lookup, on-the-fly precompilation, integer fidelity probing, and
 * falls back to plain string emission. */
static void WriteLit_Untyped(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* obj)
{
    /* Get the string rep once and reuse throughout — the pointer remains
     * valid as long as we don't modify the object (which we never do). */
    Tcl_Size n = 0;
    const char* s = Tbcx_GetStringFromObjSafe(obj, &n);

    /* Lambda detection for string-typed literals.
       Tcl's compiler stores [apply] operands as plain string literals,
       not list-typed.  Try to parse as a list; if it has lambda shape
       ({argSpec body ?ns?} with script-like body), compile as lambda-bc.
       Body must start with a letter (command name) and contain both
       whitespace AND a script indicator to avoid false positives like
       "throw $code message" or "expr {$x * 3}".
       See comment above: runs regardless of stripActive. */
    if (ctx)
    {
        if (n >= 4)
        {
            Tcl_Obj* lcopy = Tcl_DuplicateObj(obj);
            Tcl_IncrRefCount(lcopy);
            Tcl_Size nElems = 0;
            Tcl_Obj** elems = NULL;
            int isLambdaLike = 0;
            if (Tcl_ListObjGetElements(NULL, lcopy, &nElems, &elems) == TCL_OK && (nElems == 2 || nElems == 3))
            {
                Tcl_Size largc = 0;
                Tcl_Obj** largv = NULL;
                if (Tcl_ListObjGetElements(NULL, elems[0], &largc, &largv) == TCL_OK)
                {
                    int validArgs = 1;
                    for (Tcl_Size ai = 0; ai < largc; ai++)
                    {
                        Tcl_Size nf = 0;
                        Tcl_Obj** fv = NULL;
                        if (Tcl_ListObjGetElements(NULL, largv[ai], &nf, &fv) != TCL_OK || nf < 1 || nf > 2)
                        {
                            validArgs = 0;
                            break;
                        }
                    }
                    if (validArgs)
                    {
                        Tcl_Size bodyLen = 0;
                        const char* bodyStr = Tbcx_GetStringFromObjSafe(elems[1], &bodyLen);
                        if (bodyLen >= 4)
                        {
                            /* Skip leading whitespace — multi-line lambda bodies
                               (natural Tcl formatting) start with \n + indentation. */
                            const char* bp = bodyStr;
                            Tcl_Size bpLen = bodyLen;
                            while (bpLen > 0 && ((unsigned char)*bp <= ' '))
                            {
                                bp++;
                                bpLen--;
                            }
                            int startsWithLetter =
                                (bpLen > 0 && ((bp[0] >= 'a' && bp[0] <= 'z') || (bp[0] >= 'A' && bp[0] <= 'Z')));
                            if (startsWithLetter)
                            {
                                int hasSpace = 0, hasIndicator = 0;
                                for (Tcl_Size bi = 0; bi < bodyLen; bi++)
                                {
                                    char ch = bodyStr[bi];
                                    if (ch == ' ' || ch == '\t')
                                        hasSpace = 1;
                                    if (ch == '$' || ch == '[' || ch == '\n' || ch == ';')
                                        hasIndicator = 1;
                                    if (hasSpace && hasIndicator)
                                    {
                                        isLambdaLike = 1;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (isLambdaLike)
            {
                W_U32(w, TBCX_LIT_LAMBDA_BC);
                Lit_LambdaBC(w, ctx, lcopy);
                Tcl_DecrRefCount(lcopy);
                return;
            }
            Tcl_DecrRefCount(lcopy);
        }
    }

    /* Side-table check: if PrecompileLiteralPool created a compiled
       copy of this literal, emit the COPY as TBCX_LIT_BYTECODE.
       The original literal in the pool is UNCHANGED (still a string).
       Mark BEFORE emitting (mark-before-visit) to prevent recursive
       re-emission of the same body text from nested literals. */
    Tcl_Obj* compiled = CtxGetCompiled(ctx, obj);
    if (compiled)
    {
        if (ctx->emittedInit)
        {
            int isNew;
            Tcl_CreateHashEntry(&ctx->emittedBodies, s, &isNew);
        }
        W_U32(w, TBCX_LIT_BYTESRC);
        W_LPString(w, s, n);
        Lit_Bytecode(w, ctx, compiled);
        return;
    }

    if (ShouldStripBody(ctx, obj))
    {
        W_U32(w, TBCX_LIT_STRING);
        W_LPString(w, "", 0);
        return;
    }

    /* On-the-fly precompilation: if this string literal was tracked
       by ScanScriptBodiesRec (in nsEvalBodies) but not yet compiled
       by PrecompileLiteralPool — e.g. because it lives in a proc or
       method bytecode rather than the top-level pool — compile it now.
       DEDUP: skip if this exact body text was already emitted as
       bytecode (from PrecompileLiteralPool or a previous on-the-fly
       compilation).  This prevents output explosion when many procs
       share the same namespace-eval body as a literal. */
    if (ctx && ctx->nsEvalInit && n >= 2 && ctx->precompileDepth < 32)
    {
        /* Dedup check — skip if already emitted as bytecode */
        int alreadyEmitted = 0;
        if (ctx->emittedInit)
        {
            Tcl_HashEntry* dedup = Tcl_FindHashEntry(&ctx->emittedBodies, s);
            alreadyEmitted = (dedup != NULL);
        }
        if (!alreadyEmitted)
        {
            Tcl_HashEntry* he = Tcl_FindHashEntry(&ctx->nsEvalBodies, s);
            if (!he && n > 4)
            {
                /* Whitespace-trimmed fallback (same as PrecompileLiteralPool) */
                const char* ts = s;
                Tcl_Size tLen = n;
                while (tLen > 0 && ((unsigned char)*ts <= ' '))
                {
                    ts++;
                    tLen--;
                }
                while (tLen > 0 && ((unsigned char)ts[tLen - 1] <= ' '))
                    tLen--;
                if (tLen != n && tLen > 0)
                {
                    Tcl_Obj* trimmed = Tcl_NewStringObj(ts, tLen);
                    Tcl_IncrRefCount(trimmed);
                    he = Tcl_FindHashEntry(&ctx->nsEvalBodies, Tbcx_GetStringSafe(trimmed));
                    Tcl_DecrRefCount(trimmed);
                }
            }
            Tcl_Obj* nsObj = he ? (Tcl_Obj*)Tcl_GetHashValue(he) : NULL;
            if (nsObj)
            {
                Namespace* targetNs = (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tbcx_GetStringSafe(nsObj));
                if (targetNs)
                {
                    Tcl_Obj* copy = Tcl_DuplicateObj(obj);
                    Tcl_IncrRefCount(copy);
                    if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
                    {
                        ByteCode* bc = NULL;
                        bc = TbcxGetByteCode(copy);
                        if (!bc)
                        {
                            const Tcl_ObjInternalRep* ir = Tcl_FetchInternalRep(copy, tbcxTyBytecode);
                            if (ir)
                                bc = (ByteCode*)ir->twoPtrValue.ptr1;
                        }
                        if (bc)
                        {
                            bc->nsPtr = targetNs;
                            bc->nsEpoch = targetNs->resolverEpoch;
                            /* Mark BEFORE emitting (mark-before-visit) */
                            if (ctx->emittedInit)
                            {
                                int isNew;
                                Tcl_CreateHashEntry(&ctx->emittedBodies, s, &isNew);
                            }
                            ctx->precompileDepth++;
                            W_U32(w, TBCX_LIT_BYTESRC); W_LPString(w, s, n);
                            Lit_Bytecode(w, ctx, copy);
                            ctx->precompileDepth--;
                            Tcl_DecrRefCount(copy);
                            return;
                        }
                    }
                    else
                    {
                        Tcl_ResetResult(ctx->interp);
                    }
                    Tcl_DecrRefCount(copy);
                }
            }
        } /* !alreadyEmitted */
    }

    /* Improvement #1: instruction-level body literal precompilation.
       If InstrScanBodyLiterals marked this literal as a body argument
       to an eval-like command (try, catch, eval, uplevel, etc.), compile
       it now into bytecode.  This prevents the source from leaking as
       plaintext in the .tbcx file.
       Skip if already emitted (dedup) or if it doesn't look like a script. */
    if (ctx && ctx->instrBodyInit && n >= 2)
    {
        Tcl_HashEntry* ihe = Tcl_FindHashEntry(&ctx->instrBodyLits, (const char*)obj);
        if (ihe)
        {
            int alreadyEmitted = 0;
            if (ctx->emittedInit)
            {
                int isNew;
                Tcl_CreateHashEntry(&ctx->emittedBodies, s, &isNew);
                if (!isNew)
                    alreadyEmitted = 1;
            }
            if (!alreadyEmitted && ctx->precompileDepth < 3)
            {
                Namespace* targetNs = (Namespace*)Tcl_GetHashValue(ihe);
                if (!targetNs)
                    targetNs = (Namespace*)Tcl_GetGlobalNamespace(ctx->interp);
                Tcl_Obj* copy = Tcl_DuplicateObj(obj);
                Tcl_IncrRefCount(copy);
                if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
                {
                    ByteCode* ibc = TbcxGetByteCode(copy);
                    if (ibc)
                    {
                        ibc->nsPtr = targetNs;
                        ibc->nsEpoch = targetNs->resolverEpoch;
                        ctx->precompileDepth++;
                        W_U32(w, TBCX_LIT_BYTESRC); W_LPString(w, s, n);
                        Lit_Bytecode(w, ctx, copy);
                        ctx->precompileDepth--;
                        Tcl_DecrRefCount(copy);
                        return;
                    }
                }
                else
                {
                    Tcl_ResetResult(ctx->interp);
                }
                Tcl_DecrRefCount(copy);
            }
        }
    }

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

    /* Fallback: emit as plain string.
     *
     * NOTE (S4, updated): Improvement #1 uses two-phase detection:
     *   Phase 1: instruction-level invokeStk pattern analysis detects
     *     body arguments to try, catch, eval, uplevel, time, timerate,
     *     dict for/map, self method.  Marks stored in ctx->instrBodyLits
     *     (Tcl_Obj* keys) and checked by WriteLit_Untyped.
     *   Phase 2: per-block index-based detection for foreach/lmap bodies
     *     compiled with specialized opcodes.  Marks stored in a local
     *     phase2marks[] array (indexed by literal position, not Tcl_Obj*),
     *     applied directly in WriteCompiledBlock before WriteLiteral.
     *     This avoids cross-block Tcl_Obj* pointer sharing contamination.
     *
     * Remaining leaks: while/for loop bodies (no foreach_start marker). */

    /* Bug fix: strings with non-ASCII content (bytes >= 0x80 in UTF-8 rep)
       may suffer from modified-UTF-8 round-trip issues on load.  In particular,
       U+0000 is encoded as 0xC0 0x80 in Tcl's internal modified UTF-8, but
       Tcl_NewStringObj may misinterpret this overlong sequence.  Strings where
       all characters have code points <= 255 can be losslessly serialized as
       TBCX_LIT_BYTEARR instead, avoiding the UTF-8 round-trip entirely. */
    {
        int hasHighByte = 0;
        for (Tcl_Size i = 0; i < n && i < 4096; i++)
        {
            if ((unsigned char)s[i] >= 0x80) { hasHighByte = 1; break; }
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
    /* In Tcl 9.1 the integer types were unified: bignum, int, and boolean
       all share the same Tcl_ObjType* ("int").  That means
       tbcxTyBignum == tbcxTyInt (and often == tbcxTyBoolean).
       We must use value-based probing rather than pointer comparison
       to choose the most compact serialization tag.
       NOTE: we do NOT attempt boolean probing here because
       Tcl_GetBooleanFromObj accepts any integer (42 -> true -> 1),
       which would corrupt values.  The wideint path handles
       0/1 correctly; the BOOLEAN tag is reserved for builds
       where tbcxTyBoolean has a distinct typePtr. */
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
        /* Check if this list looks like a lambda: {argSpec body ?ns?}
           with 2 or 3 elements, a valid arg specification, and a body
           that contains script indicators ($, [, newline, or ;).
           If so, precompile and emit as TBCX_LIT_LAMBDA_BC.
           NOTE: lambda detection runs regardless of stripActive — lambda
           operands are NOT proc/method bodies.  Proc body stripping is
           handled separately by the bytecode/procBody/ShouldStripBody
           checks above. */
        int isLambdaLike = 0;
        Tcl_Size nElems = 0;
        Tcl_Obj** elems = NULL;
        if (ctx && Tcl_ListObjGetElements(NULL, obj, &nElems, &elems) == TCL_OK && (nElems == 2 || nElems == 3))
        {
            Tcl_Size argc = 0;
            Tcl_Obj** argv = NULL;
            if (Tcl_ListObjGetElements(NULL, elems[0], &argc, &argv) == TCL_OK)
            {
                int validArgs = 1;
                for (Tcl_Size ai = 0; ai < argc; ai++)
                {
                    Tcl_Size nf = 0;
                    Tcl_Obj** fv = NULL;
                    if (Tcl_ListObjGetElements(NULL, argv[ai], &nf, &fv) != TCL_OK || nf < 1 || nf > 2)
                    {
                        validArgs = 0;
                        break;
                    }
                }
                if (validArgs)
                {
                    Tcl_Size bodyLen = 0;
                    const char* bodyStr = Tbcx_GetStringFromObjSafe(elems[1], &bodyLen);
                    if (bodyLen >= 4)
                    {
                        for (Tcl_Size bi = 0; bi < bodyLen; bi++)
                        {
                            char ch = bodyStr[bi];
                            if (ch == '$' || ch == '[' || ch == '\n' || ch == ';')
                            {
                                isLambdaLike = 1;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (isLambdaLike)
        {
            W_U32(w, TBCX_LIT_LAMBDA_BC);
            Lit_LambdaBC(w, ctx, obj);
        }
        else
        {
            W_U32(w, TBCX_LIT_LIST);
            Lit_List(w, ctx, obj);
        }
    }
    else if (ty == tbcxTyBytecode)
    {
        if (ctx && ctx->stripActive)
        {
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
        else
        {
            /* Dual dedup: pointer + string, mark-before-visit */
            int deduped = 0;
            if (ctx)
            {
                if (ctx->emittedPtrsInit)
                {
                    int isNew;
                    Tcl_CreateHashEntry(&ctx->emittedPtrs, (const char*)obj, &isNew);
                    if (!isNew)
                        deduped = 1;
                }
                if (!deduped && ctx->emittedInit)
                {
                    Tcl_Size bsLen = 0;
                    const char* bsStr = Tbcx_GetStringFromObjSafe(obj, &bsLen);
                    if (bsLen > 0)
                    {
                        int isNew;
                        Tcl_CreateHashEntry(&ctx->emittedBodies, bsStr, &isNew);
                        if (!isNew)
                            deduped = 2;
                    }
                }
            }
            if (deduped)
            {
                Tcl_Size fl = 0;
                const char* fs = Tbcx_GetStringFromObjSafe(obj, &fl);
                W_U32(w, TBCX_LIT_STRING);
                W_LPString(w, fs, fl);
            }
            else
            {
                Tcl_Size _srcLen = 0;
                const char* _srcStr = Tbcx_GetStringFromObjSafe(obj, &_srcLen);
                W_U32(w, TBCX_LIT_BYTESRC);
                W_LPString(w, _srcStr, _srcLen);
                Lit_Bytecode(w, ctx, obj);
            }
        }
    }
    else if (obj->typePtr == tbcxTyProcBody)
    {
        /* Strip proc bodies during top-level write. */
        if (ctx && ctx->stripActive)
        {
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
        else
        {
            Proc* p = (Proc*)obj->internalRep.twoPtrValue.ptr1;
            if (p && p->bodyPtr)
            {
                ByteCode* bc = NULL;
                bc = TbcxGetByteCode(p->bodyPtr);
                if (bc)
                {
                    /* Dual dedup (same as bytecode branch) */
                    if (ctx)
                    {
                        if (ctx->emittedPtrsInit)
                        {
                            int isNew;
                            Tcl_CreateHashEntry(&ctx->emittedPtrs, (const char*)p->bodyPtr, &isNew);
                            if (!isNew)
                            {
                                Tcl_Size fl = 0;
                                const char* fs = Tbcx_GetStringFromObjSafe(p->bodyPtr, &fl);
                                W_U32(w, TBCX_LIT_STRING);
                                W_LPString(w, fs, fl);
                                return;
                            }
                        }
                        if (ctx->emittedInit)
                        {
                            Tcl_Size pbLen = 0;
                            const char* pbStr = Tbcx_GetStringFromObjSafe(p->bodyPtr, &pbLen);
                            if (pbLen > 0)
                            {
                                int isNew;
                                Tcl_CreateHashEntry(&ctx->emittedBodies, pbStr, &isNew);
                                if (!isNew)
                                {
                                    W_U32(w, TBCX_LIT_STRING);
                                    W_LPString(w, pbStr, pbLen);
                                    return;
                                }
                            }
                        }
                    }
                    {
                        Tcl_Size _srcLen2 = 0;
                        const char* _srcStr2 = Tbcx_GetStringFromObjSafe(p->bodyPtr, &_srcLen2);
                        W_U32(w, TBCX_LIT_BYTESRC);
                        W_LPString(w, _srcStr2, _srcLen2);
                        Lit_Bytecode(w, ctx, p->bodyPtr);
                    }
                    return;
                }
            }
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
    }
    else if (tbcxTyLambda != NULL && ty == tbcxTyLambda)
    {
        W_U32(w, TBCX_LIT_LAMBDA_BC);
        Lit_LambdaBC(w, ctx, obj);
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
    if (!tbcx_checked_mul(sizeof(JTEntry), (size_t)cnt, &arrBytes)) {
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
    if (!tbcx_checked_mul(sizeof(JTNumEntry), (size_t)cnt, &numBytes)) {
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
    W_U32(w, (uint32_t)(info ? info->loopCtTemp : 0));
    W_U32(w, (uint32_t)(info ? info->firstValueTemp : 0));
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
            /* foreach introduces two temps plus var lists */
            if ((Tcl_Size)info->firstValueTemp > maxIdx)
                maxIdx = (Tcl_Size)info->firstValueTemp;
            if ((Tcl_Size)info->loopCtTemp > maxIdx)
                maxIdx = (Tcl_Size)info->loopCtTemp;
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
        Proc* p = (Proc*)cand->internalRep.twoPtrValue.ptr1;
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

/* ==========================================================================
 * Improvement #1: Instruction-level body literal detection.
 *
 * Walks the bytecode instruction stream of a compiled block, looking for
 * the pattern:  push cmd; push arg0; ... push argN; invokeStk (N+1)
 * If `cmd` is a body-evaluating command (foreach, while, for, try, etc.),
 * the body-argument literal is recorded in ctx->instrBodyLits so that
 * WriteLit_Untyped can precompile it instead of emitting plaintext.
 *
 * Uses a forward-pass stack model: each stack slot records either a
 * literal index (>=0) or -1 (non-literal / runtime value).  Only the
 * ~20 most common instructions are modeled; unknown opcodes reset the
 * stack conservatively.
 * ========================================================================== */

/* Body-argument position rules for eval-like commands.
   bodyPos: 0 = last argument, N>0 = Nth arg (1-based from first non-cmd arg).
   For "dict": subcmd-aware, handled specially. */
typedef struct { const char* cmd; int bodyPos; } BodyCmdRule;
static const BodyCmdRule sBodyCmds[] = {
    {"foreach",  0},   /* last arg */
    {"lmap",     0},   /* last arg */
    {"for",      0},   /* last arg (4th of 4) */
    {"while",    0},   /* last arg (2nd of 2) */
    {"try",      1},   /* first arg */
    {"catch",    1},   /* first arg */
    {"eval",     1},   /* first arg */
    {"uplevel",  0},   /* last arg */
    {"time",     1},   /* first arg */
    {"timerate", 1},   /* first arg */
    {NULL, 0}
};

/* "dict for" / "dict map" have body as last arg, but the command name
   is "dict" with subcommand "for"/"map" as the second pushed value. */
static int IsDictBodySubcmd(const char* sub)
{
    return (strcmp(sub, "for") == 0 || strcmp(sub, "map") == 0);
}

/* Return 1 if the string looks like a compilable script body.
   Strict test: requires BOTH a command indicator ($varname or [cmd])
   AND a word separator (space/tab/newline).  This excludes data strings,
   format templates (which ARE pushed), strcat fragments, etc.
   Examples:
     "incr sum $x"      → $x + space → YES
     "set a [expr ...]"  → [expr + space → YES
     "a\tb\nc"           → no $var or [cmd] → NO
     "  hello  "         → no $var or [cmd] → NO
     "expr {$x + %d}"   → $x + space → YES (but is pushed, so Phase 2 skips)
     "$x"                → $x but no space → NO */
static int LooksLikeScriptBody(const char* s, Tcl_Size n)
{
    if (n < 4) return 0;
    int hasCmdIndicator = 0;
    int hasWordSep = 0;
    Tcl_Size limit = (n < 300) ? n : 300;
    for (Tcl_Size i = 0; i < limit; i++)
    {
        char c = s[i];
        /* $varname: $ followed by letter or underscore or :: */
        if (c == '$' && i + 1 < n)
        {
            char nx = s[i + 1];
            if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z') ||
                nx == '_' || nx == ':')
                hasCmdIndicator = 1;
        }
        /* [command] */
        if (c == '[')
            hasCmdIndicator = 1;
        /* Word separator */
        if (c == ' ' || c == '\t' || c == '\n')
            hasWordSep = 1;
        if (hasCmdIndicator && hasWordSep)
            return 1;
    }
    return 0;
}

/* Check if instruction name starts with given prefix. */
static int InstrIs(const char* name, const char* prefix)
{
    while (*prefix)
    {
        if (*name != *prefix) return 0;
        name++; prefix++;
    }
    return 1;
}

static void InstrScanBodyLiterals(ByteCode* bc, TbcxCtx* ctx, char* phase2marks)
{
    if (!bc || !ctx || !ctx->instrBodyInit)
        return;
    if (!bc->codeStart || bc->numCodeBytes <= 0)
        return;
    if (!bc->objArrayPtr || bc->numLitObjects <= 0)
        return;

    const InstructionDesc* instTable = (const InstructionDesc*)TclGetInstructionTable();
    const unsigned char* code = bc->codeStart;
    Tcl_Size codeLen = bc->numCodeBytes;
    Tcl_Size numLits = bc->numLitObjects;

    /* Track which literals are pushed by ANY instruction.  Literals that
       are never pushed are "dead references" -- source text kept for error
       reporting by Tcl's compiler for inline-compiled bodies (foreach,
       while, for, lmap, dict for).  These should also be precompiled. */
    char* pushed = NULL;
    if (numLits <= 4096)
    {
        pushed = (char*)Tcl_Alloc((size_t)numLits);
        memset(pushed, 0, (size_t)numLits);
    }

    /* Stack model: each slot is a literal index or -1 (non-literal). */
#define ISCAN_MAX 128
    int stk[ISCAN_MAX];
    int sp = 0;
#define ISCAN_PUSH(v) do { if (sp < ISCAN_MAX) stk[sp++] = (v); } while(0)
#define ISCAN_POP()   (sp > 0 ? stk[--sp] : -1)

    Tcl_Size pc = 0;
    while (pc < codeLen)
    {
        unsigned int opcode = code[pc];
        if (opcode > LAST_INST_OPCODE) { pc++; sp = 0; continue; }

        const InstructionDesc* desc = &instTable[opcode];
        if (pc + desc->numBytes > codeLen) break;

        const char* nm = desc->name;
        const unsigned char* op = code + pc + 1;

        /* ---- push1 / push4 ---- */
        if (InstrIs(nm, "push"))
        {
            int litIdx = -1;
            /* Tcl 9.1 changed literal operands from OPERAND_UINT to
               OPERAND_LIT.  Use numBytes to infer operand size — this
               is robust across all Tcl versions. */
            if (desc->numBytes == 2)       /* push1: 1-byte operand */
                litIdx = op[0];
            else if (desc->numBytes == 5)  /* push4: 4-byte operand */
                litIdx = (int)((op[0] << 24) | (op[1] << 16) | (op[2] << 8) | op[3]);
            if (pushed && litIdx >= 0 && litIdx < numLits)
                pushed[litIdx] = 1;
            ISCAN_PUSH(litIdx);
        }
        /* ---- invokeStk1 / invokeStk4 ---- */
        else if (InstrIs(nm, "invokeStk"))
        {
            int argc = 0;
            if (desc->numBytes == 2)       /* invokeStk1: 1-byte count */
                argc = op[0];
            else if (desc->numBytes == 5)  /* invokeStk4: 4-byte count */
                argc = (int)((op[0] << 24) | (op[1] << 16) | (op[2] << 8) | op[3]);

            if (argc >= 2 && sp >= argc)
            {
                int base = sp - argc;
                int cmdLitIdx = stk[base];

                /* Resolve command name from literal pool */
                if (cmdLitIdx >= 0 && cmdLitIdx < numLits && bc->objArrayPtr[cmdLitIdx])
                {
                    const char* cmdStr = CmdCore(Tbcx_GetStringSafe(bc->objArrayPtr[cmdLitIdx]));
                    int bodyLitIdx = -1;

                    /* "dict for"/"dict map" -- subcmd is second pushed value */
                    if (strcmp(cmdStr, "dict") == 0 && argc >= 3)
                    {
                        int subIdx = stk[base + 1];
                        if (subIdx >= 0 && subIdx < numLits && bc->objArrayPtr[subIdx])
                        {
                            const char* sub = Tbcx_GetStringSafe(bc->objArrayPtr[subIdx]);
                            if (IsDictBodySubcmd(sub))
                                bodyLitIdx = stk[base + argc - 1]; /* last arg */
                        }
                    }
                    /* "self method NAME ARGS BODY" -- body is last arg */
                    else if (strcmp(cmdStr, "self") == 0 && argc >= 5)
                    {
                        int subIdx = stk[base + 1];
                        if (subIdx >= 0 && subIdx < numLits && bc->objArrayPtr[subIdx])
                        {
                            const char* sub = Tbcx_GetStringSafe(bc->objArrayPtr[subIdx]);
                            if (strcmp(sub, "method") == 0)
                                bodyLitIdx = stk[base + argc - 1]; /* last arg */
                        }
                    }
                    else
                    {
                        /* Check standard body-evaluating commands */
                        for (const BodyCmdRule* r = sBodyCmds; r->cmd; r++)
                        {
                            if (strcmp(cmdStr, r->cmd) == 0)
                            {
                                if (r->bodyPos == 0)
                                    bodyLitIdx = stk[base + argc - 1]; /* last arg */
                                else if (base + r->bodyPos < sp)
                                    bodyLitIdx = stk[base + r->bodyPos]; /* 1-based position */
                                break;
                            }
                        }
                    }

                    /* Record the body literal for precompilation */
                    if (bodyLitIdx >= 0 && bodyLitIdx < numLits)
                    {
                        Tcl_Obj* bodyObj = bc->objArrayPtr[bodyLitIdx];
                        if (bodyObj && bodyObj->typePtr != tbcxTyProcBody)
                        {
                            Tcl_Size bl = 0;
                            const char* bs = Tbcx_GetStringFromObjSafe(bodyObj, &bl);
                            if (LooksLikeScriptBody(bs, bl))
                            {
                                /* Mark in phase2marks[] (per-block index) —
                                   bypasses WriteLiteral entirely in
                                   WriteCompiledBlock, immune to stripActive
                                   and type-based dispatch issues. */
                                if (phase2marks)
                                    phase2marks[bodyLitIdx] = 1;
                                /* Also mark in instrBodyLits for the
                                   WriteLit_Untyped path (non-stripped blocks
                                   where the literal is still string-typed). */
                                if (!TbcxGetByteCode(bodyObj))
                                {
                                    int isNew;
                                    Tcl_HashEntry* he = Tcl_CreateHashEntry(
                                        &ctx->instrBodyLits, (const char*)bodyObj, &isNew);
                                    if (isNew)
                                        Tcl_SetHashValue(he, (void*)bc->nsPtr);
                                }
                            }
                        }
                    }
                }
            }

            /* Pop argc, push result (-1) */
            sp -= argc;
            if (sp < 0) sp = 0;
            ISCAN_PUSH(-1);
        }
        /* ---- Stack-neutral: startCommand, nop ---- */
        else if (InstrIs(nm, "startCommand") || InstrIs(nm, "nop"))
        {
            /* no stack effect */
        }
        /* ---- Pop ---- */
        else if (InstrIs(nm, "pop"))
        {
            (void)ISCAN_POP();
        }
        /* ---- Stack-producing (1 value, no stack consumption) ---- */
        else if (InstrIs(nm, "loadScalar") || InstrIs(nm, "loadArray") ||
                 InstrIs(nm, "dup") ||
                 InstrIs(nm, "over") || InstrIs(nm, "nsupvar") ||
                 InstrIs(nm, "upvar") || InstrIs(nm, "variable"))
        {
            ISCAN_PUSH(-1);
        }
        /* ---- loadStk: pops varname, pushes value ---- */
        else if (InstrIs(nm, "loadStk"))
        {
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- loadArrayStk: pops arrayname+index, pushes value ---- */
        else if (InstrIs(nm, "loadArrayStk"))
        {
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- storeScalar/storeArray: uses LVT, pops val, pushes val back ---- */
        else if (InstrIs(nm, "storeScalar") || InstrIs(nm, "storeArray"))
        {
            if (sp > 0) stk[sp - 1] = -1;
        }
        /* ---- storeStk: pops name+val, pushes val ---- */
        else if (InstrIs(nm, "storeStk"))
        {
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- storeArrayStk: pops arrayname+index+val, pushes val ---- */
        else if (InstrIs(nm, "storeArrayStk"))
        {
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- incrStkImm: pops varname, pushes incremented value ---- */
        else if (InstrIs(nm, "incrStkImm") || InstrIs(nm, "existStk"))
        {
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- incrStk/lappendStk/appendStk: pops varname+value, pushes result ---- */
        else if (InstrIs(nm, "incrStk") || InstrIs(nm, "lappendStk") ||
                 InstrIs(nm, "appendStk"))
        {
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- lappendListStk: pops varname+list, pushes result ---- */
        else if (InstrIs(nm, "lappendListStk"))
        {
            (void)ISCAN_POP();
            (void)ISCAN_POP();
            ISCAN_PUSH(-1);
        }
        /* ---- unsetStk: pops varname, pushes nothing ---- */
        else if (InstrIs(nm, "unsetStk"))
        {
            (void)ISCAN_POP();
        }
        /* ---- returnStk: pops options+value ---- */
        else if (InstrIs(nm, "returnStk"))
        {
            sp = 0;
        }
        /* ---- strcat N, list N, concat N: pop N, push 1 ---- */
        else if (InstrIs(nm, "strcat") || InstrIs(nm, "list") ||
                 InstrIs(nm, "concat") || InstrIs(nm, "lappendList"))
        {
            int cnt = 0;
            /* Use numBytes to infer operand size (robust across Tcl versions) */
            if (desc->numBytes == 2)
                cnt = op[0];
            else if (desc->numBytes == 5)
                cnt = (int)((op[0] << 24) | (op[1] << 16) | (op[2] << 8) | op[3]);
            sp -= cnt;
            if (sp < 0) sp = 0;
            ISCAN_PUSH(-1);
        }
        /* ---- returnImm / done / jump: reset ---- */
        else if (InstrIs(nm, "return") || InstrIs(nm, "done") ||
                 InstrIs(nm, "jump"))
        {
            sp = 0;
        }
        /* ---- Arithmetic / comparison / misc: pop some, push 1 ---- */
        else if (InstrIs(nm, "add") || InstrIs(nm, "sub") ||
                 InstrIs(nm, "mult") || InstrIs(nm, "div") ||
                 InstrIs(nm, "mod") || InstrIs(nm, "eq") ||
                 InstrIs(nm, "neq") || InstrIs(nm, "lt") ||
                 InstrIs(nm, "gt") || InstrIs(nm, "le") ||
                 InstrIs(nm, "ge") || InstrIs(nm, "land") ||
                 InstrIs(nm, "lor") || InstrIs(nm, "lnot") ||
                 InstrIs(nm, "bitnot") || InstrIs(nm, "lshift") ||
                 InstrIs(nm, "rshift") || InstrIs(nm, "bitand") ||
                 InstrIs(nm, "bitor") || InstrIs(nm, "bitxor") ||
                 InstrIs(nm, "uminus") || InstrIs(nm, "uplus") ||
                 InstrIs(nm, "not") || InstrIs(nm, "tryCvtToNumeric") ||
                 InstrIs(nm, "exprStk") || InstrIs(nm, "existScalar") ||
                 InstrIs(nm, "existArray") ||
                 InstrIs(nm, "appendScalar") || InstrIs(nm, "appendArray") ||
                 InstrIs(nm, "lappendScalar") || InstrIs(nm, "lappendArray") ||
                 InstrIs(nm, "incrScalar") || InstrIs(nm, "incrArray") ||
                 InstrIs(nm, "incrScalarImm") ||
                 InstrIs(nm, "lindexMulti") || InstrIs(nm, "lsetFlat"))
        {
            /* Conservative: these consume 1-2 and produce 1. Just mark non-lit. */
            if (sp > 0) stk[sp - 1] = -1;
        }
        /* ---- Unknown: reset stack (conservative) ---- */
        else
        {
            sp = 0;
        }

        pc += desc->numBytes;
    }

    /* ---- Phase 2: Per-block unpushed body literal detection ----
     *
     * Tcl 9 compiles foreach/lmap with specialized opcodes (foreach_start,
     * foreach_step).  The body is compiled inline but the source text
     * remains in the literal pool for error reporting only.  These source
     * literals are never referenced by a push instruction.
     *
     * CRITICAL: Phase 2 marks are stored in the caller-provided
     * `phase2marks[]` array indexed by literal position — NOT in
     * ctx->instrBodyLits (which is keyed by Tcl_Obj*).  This prevents
     * cross-block contamination from Tcl_Obj pointer sharing.
     *
     * Guard: only activates when this block contains foreach_start
     * instructions (detected by instruction name scanning).
     *
     * Safety: pushed[] excludes all data literals (format templates,
     * strcat fragments, string args). LooksLikeScriptBody() rejects
     * non-script data. */
    if (pushed && phase2marks)
    {
        /* Detect foreach_start instructions */
        int hasForeachInstr = 0;
        {
            Tcl_Size pc2 = 0;
            while (pc2 < codeLen)
            {
                unsigned int op2 = code[pc2];
                if (op2 > LAST_INST_OPCODE) { pc2++; continue; }
                const InstructionDesc* d2 = &instTable[op2];
                if (pc2 + d2->numBytes > codeLen) break;
                if (InstrIs(d2->name, "foreach_start"))
                {
                    hasForeachInstr = 1;
                    break;
                }
                pc2 += d2->numBytes;
            }
        }

        if (hasForeachInstr)
        {
            for (Tcl_Size i = 0; i < numLits; i++)
            {
                if (pushed[i])
                    continue; /* pushed = data, not dead body text */
                Tcl_Obj* lit = bc->objArrayPtr[i];
                if (!lit)
                    continue;
                if (lit->typePtr == tbcxTyProcBody)
                    continue;
                /* Don't skip bytecode-typed literals — Tcl's foreach
                   compiler may have shimmered the body literal to bytecode
                   type during inline compilation.  Per-block index marks
                   prevent cross-block contamination. */
                Tcl_Size sl = 0;
                const char* ss = Tbcx_GetStringFromObjSafe(lit, &sl);
                if (LooksLikeScriptBody(ss, sl))
                {
                    phase2marks[i] = 1;
                }
            }
        }
    }
    if (pushed)
        Tcl_Free(pushed);

#undef ISCAN_MAX
#undef ISCAN_PUSH
#undef ISCAN_POP
}

static void WriteCompiledBlock(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj)
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

    /* 1) code */
    if ((uint64_t)bc->numCodeBytes > TBCX_MAX_CODE)
    {
        W_Error(w, "tbcx: code too large");
        if (ctx)
            ctx->blockDepth--;
        return;
    }
    W_U32(w, (uint32_t)bc->numCodeBytes);
    W_Bytes(w, bc->codeStart, (size_t)bc->numCodeBytes);

    /* 2) literal pool */
    /* Improvement #1: scan instructions to identify body-argument literals
       BEFORE emitting the pool.
       Phase 1: populates ctx->instrBodyLits (Tcl_Obj* keys) for invokeStk
       body arguments.  Cleared at blockDepth==1 to prevent cross-block leak.
       Phase 2: populates phase2marks[] (per-block index array) for unpushed
       foreach/lmap body literals.  Index-based — immune to Tcl_Obj* sharing. */
    char* phase2marks = NULL;
    if (ctx && ctx->instrBodyInit)
    {
        if (ctx->blockDepth == 1)
        {
            Tcl_DeleteHashTable(&ctx->instrBodyLits);
            Tcl_InitHashTable(&ctx->instrBodyLits, TCL_ONE_WORD_KEYS);
        }
        if (bc->numLitObjects > 0)
        {
            phase2marks = (char*)Tcl_Alloc((size_t)bc->numLitObjects);
            memset(phase2marks, 0, (size_t)bc->numLitObjects);
        }
        InstrScanBodyLiterals(bc, ctx, phase2marks);
    }
    W_U32(w, (uint32_t)bc->numLitObjects);
    for (Tcl_Size i = 0; i < bc->numLitObjects; i++)
    {
        Tcl_Obj* lit = bc->objArrayPtr[i];
        /* Phase 2: if this literal index was marked as an unpushed loop
           body by InstrScanBodyLiterals, precompile it directly.  This
           bypasses WriteLiteral entirely — no Tcl_Obj* pointer lookup
           means no cross-block contamination. */
        int emittedP2 = 0;
        if (phase2marks && phase2marks[i] && ctx && ctx->precompileDepth < 3)
        {
            /* Check if literal is already bytecode-typed (Tcl's foreach
               compiler may have shimmered it during inline compilation) */
            ByteCode* existingBC = TbcxGetByteCode(lit);
            if (existingBC)
            {
                /* Already compiled — emit with source text for cross-interp safety */
                Tcl_Size _p2sLen = 0;
                const char* _p2sStr = Tbcx_GetStringFromObjSafe(lit, &_p2sLen);
                ctx->precompileDepth++;
                W_U32(w, TBCX_LIT_BYTESRC);
                W_LPString(w, _p2sStr, _p2sLen);
                Lit_Bytecode(w, ctx, lit);
                ctx->precompileDepth--;
                emittedP2 = 1;
            }
            else
            {
                Tcl_Size sl2 = 0;
                const char* ss2 = Tbcx_GetStringFromObjSafe(lit, &sl2);
                /* Dedup check */
                int alreadyDone = 0;
                if (ctx->emittedInit)
                {
                    int isNew;
                    Tcl_CreateHashEntry(&ctx->emittedBodies, ss2, &isNew);
                    if (!isNew)
                        alreadyDone = 1;
                }
                if (!alreadyDone && sl2 >= 2)
                {
                    Namespace* targetNs = bc->nsPtr ? bc->nsPtr
                        : (Namespace*)Tcl_GetGlobalNamespace(ctx->interp);
                    Tcl_Obj* copy = Tcl_DuplicateObj(lit);
                    Tcl_IncrRefCount(copy);
                    if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
                    {
                        ByteCode* ibc = TbcxGetByteCode(copy);
                        if (ibc)
                        {
                            ibc->nsPtr = targetNs;
                            ibc->nsEpoch = targetNs->resolverEpoch;
                            ctx->precompileDepth++;
                            W_U32(w, TBCX_LIT_BYTESRC);
                            W_LPString(w, ss2, sl2);
                            Lit_Bytecode(w, ctx, copy);
                            ctx->precompileDepth--;
                            emittedP2 = 1;
                        }
                    }
                    else
                    {
                        Tcl_ResetResult(ctx->interp);
                    }
                    Tcl_DecrRefCount(copy);
                }
            }
        }
        if (!emittedP2)
            WriteLiteral(w, ctx, lit);
        if (w->err)
        {
            if (phase2marks)
                Tcl_Free(phase2marks);
            if (ctx)
                ctx->blockDepth--;
            return;
        }
    }
    if (phase2marks)
        Tcl_Free(phase2marks);

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
    W_U32(w, 0);
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
    H.numLitsTop = (uint32_t)top->numLitObjects;
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
        for (int i = 0; i < wordTok->numComponents; i++)
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
            for (int i = 0; i < wordTok->numComponents; i++)
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

static Tcl_Obj* FqnUnder(Tcl_Interp* ip, Tcl_Obj* curNs, Tcl_Obj* name)
{
    Tcl_Size nameLen = 0;
    const char* nm = Tbcx_GetStringFromObjStrict(ip, name, &nameLen);
    if (!nm)
        return NULL;
    if (nameLen > 0 && memchr(nm, '\0', (size_t)nameLen) != NULL)
    {
        if (ip) Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: name contains embedded NUL"));
        return NULL;
    }
    if (nameLen >= 2 && nm[0] == ':' && nm[1] == ':')
    {
        Tcl_IncrRefCount(name);      /* returned refcount = 1 */
        return name;
    }

    Tcl_Size nsLen = 0;
    const char* ns = Tbcx_GetStringFromObjStrict(ip, curNs, &nsLen);
    if (!ns)
        return NULL;
    if (nsLen > 0 && memchr(ns, '\0', (size_t)nsLen) != NULL)
    {
        if (ip) Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: namespace contains embedded NUL"));
        return NULL;
    }

    Tcl_Obj* fqn = Tcl_NewStringObj(ns, nsLen); /* refcount 0 */
    Tcl_IncrRefCount(fqn);                      /* refcount 1 — own before mutation */
    if (!(nsLen == 2 && ns[0] == ':' && ns[1] == ':'))
        Tcl_AppendToObj(fqn, "::", 2);
    Tcl_AppendObjToObj(fqn, name);
    return fqn;                                /* returned refcount = 1 */
}

static void CaptureClassBody(Tcl_Interp* ip,
                             const char* script,
                             Tcl_Size len,
                             Tcl_Obj* curNs,
                             Tcl_Obj* clsFqn,
                             DefVec* defs,
                             ClsSet* classes,
                             int flags,
                             int depth)
{
    /* Guard against unbounded recursion via nested private blocks */
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
                    /* Name is always the word after "method"/"classmethod".
                       Args and body are always the last two words.
                       Any option flags (-export/-private/-unexport) sit
                       between name and args and are ignored here. */
                    const Tcl_Token* wN = NextWord(w0);
                    const Tcl_Token* t = w0;
                    for (Tcl_Size w = 0; w + 2 < p.numWords; w++)
                        t = NextWord(t);
                    const Tcl_Token* wA = t;
                    const Tcl_Token* wB = NextWord(wA);
                    Tcl_Obj* mname = WordLiteralObj(wN);
                    Tcl_Obj* args = WordLiteralObj(wA);
                    Tcl_Obj* body = WordLiteralObj(wB);
                    if (mname && args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = (strcmp(kw, "classmethod") == 0) ? DEF_KIND_CLASS : DEF_KIND_INST;
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.name = mname; /* transferred from WordLiteralObj (refcount 1) */
                        r.args = args;  /* transferred from WordLiteralObj (refcount 1) */
                        r.body = body;  /* transferred from WordLiteralObj (refcount 1) */
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        r.flags = flags;
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
                   methods/ctors/dtors defined in private context. */
                else if (strcmp(kw, "private") == 0 && p.numWords >= 2)
                {
                    const Tcl_Token* wBlock = NextWord(w0);
                    Tcl_Obj* block = WordLiteralObj(wBlock);
                    if (block)
                    {
                        Tcl_Size bkLen = 0;
                        const char* bkStr = Tbcx_GetStringFromObjSafe(block, &bkLen);
                        CaptureClassBody(ip, bkStr, bkLen, curNs, clsFqn, defs, classes, flags, depth + 1);
                        Tcl_DecrRefCount(block);
                    }
                }
                /* Improvement #2: "self method NAME ARGS BODY" -- capture
                   the body so it can be stubbed, preventing source leakage. */
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
                                DV_Push(defs, r);
                                CS_Add(classes, clsFqn);
                            }
                            else
                            {
                                if (mname) Tcl_DecrRefCount(mname);
                                if (args)  Tcl_DecrRefCount(args);
                                if (body)  Tcl_DecrRefCount(body);
                            }
                        }
                        Tcl_DecrRefCount(subCmd);
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
                    /* Only emit declarative, order-independent keywords.
                       Improvement #2: skip "self method ..." -- these are
                       captured in DefVec and emitted with stubbed bodies
                       in pass 2.  Other self commands (self mixin, etc.)
                       are still emitted verbatim. */
                    int isCapturedSelf = 0;
                    if (strcmp(kw, "self") == 0 && p.numWords >= 5)
                    {
                        const Tcl_Token* wSub = NextWord(w0);
                        Tcl_Obj* subObj = WordLiteralObj(wSub);
                        if (subObj)
                        {
                            if (strcmp(Tbcx_GetStringSafe(subObj), "method") == 0)
                                isCapturedSelf = 1;
                            Tcl_DecrRefCount(subObj);
                        }
                    }
                    if (!isCapturedSelf &&
                        (strcmp(kw, "variable") == 0 || strcmp(kw, "superclass") == 0 || strcmp(kw, "mixin") == 0 ||
                        strcmp(kw, "filter") == 0 || strcmp(kw, "forward") == 0 || strcmp(kw, "self") == 0))
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

    /* Pass 2: Emit method/ctor/dtor stubs from DefVec (original, proven safe). */
    for (Tcl_Size i = 0; i < defs->n; i++)
    {
        DefRec* r = &defs->v[i];
        if (r->kind == DEF_KIND_PROC)
            continue;
        if ((r->flags & DEF_F_FROM_BUILDER) == 0)
            continue;
        Tcl_Size thisLen = 0;
        const char* thisCls = Tbcx_GetStringFromObjSafe(r->cls, &thisLen);
        if (!(thisLen == fqnLen && memcmp(thisCls, cls, (size_t)fqnLen) == 0))
            continue;

        Tcl_DString ln;
        Tcl_DStringInit(&ln);
        Tcl_Size tmp;
        Tcl_DStringAppendElement(&ln, "oo::define");
        Tcl_DStringAppendElement(&ln, cls);

        if (r->kind == DEF_KIND_INST || r->kind == DEF_KIND_CLASS)
        {
            if (r->flags & DEF_F_SELF_METHOD)
            {
                /* Improvement #2 / Option A: emit as flat oo::objdefine form
                   so the existing CmdOOShimObjDef can intercept and substitute
                   the precompiled body.  This avoids the builder-body string
                   approach that left the body empty (breaking metaclass create). */
                Tcl_DStringFree(&ln); /* discard the "oo::define CLS" prefix */
                Tcl_DStringInit(&ln);
                Tcl_DStringAppendElement(&ln, "oo::objdefine");
                Tcl_DStringAppendElement(&ln, cls);
                Tcl_DStringAppendElement(&ln, "method");
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->name, &tmp));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->args, &tmp));
                Tcl_DStringAppendElement(&ln, "");
            }
            else
            {
                Tcl_DStringAppendElement(&ln, (r->kind == DEF_KIND_CLASS) ? "classmethod" : "method");
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->name, &tmp));
                Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->args, &tmp));
                Tcl_DStringAppendElement(&ln, "");
            }
        }
        else if (r->kind == DEF_KIND_CTOR)
        {
            Tcl_DStringAppendElement(&ln, "constructor");
            Tcl_DStringAppendElement(&ln, Tbcx_GetStringFromObjSafe(r->args, &tmp));
            Tcl_DStringAppendElement(&ln, "");
        }
        else if (r->kind == DEF_KIND_DTOR)
        {
            Tcl_DStringAppendElement(&ln, "destructor");
            Tcl_DStringAppendElement(&ln, ";");
        }
        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
        Tcl_DStringAppend(out, "\n", 1);
        Tcl_DStringFree(&ln);
    }
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
            /* Declarative, order-independent OO builder commands — these
               don't have method bodies and don't depend on method existence.
               Accept as pure so the builder can be expanded.
               Note: export/unexport/deletemethod/renamemethod are NOT here
               because they reference methods by name and must execute AFTER
               the method definitions.  Builders containing those commands
               fall through to StubbedBuilderBody which preserves order. */
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
                    Tcl_DStringAppendElement(&ln, ""); /* stubbed body */
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
                        Tcl_DStringAppendElement(&ln, "");
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
                       Emit "destructor {;}" — semicolon as minimal stub. */
                    {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "destructor");
                        Tcl_DStringAppendElement(&ln, ";");
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                else if (strcmp(kw, "private") == 0 && p.numWords >= 2)
                {
                    if (p.numWords == 2)
                    {
                        /* Block form: private { script } — recurse into
                           the block and emit private { <stubbed> }. */
                        const Tcl_Token* wBlock = NextWord(w0);
                        Tcl_Obj* block = WordLiteralObj(wBlock);
                        if (block)
                        {
                            Tcl_Obj* stubbed = StubbedBuilderBody(ip, block);
                            Tcl_DString ln;
                            Tcl_DStringInit(&ln);
                            Tcl_DStringAppendElement(&ln, "private");
                            Tcl_DStringAppendElement(&ln, Tbcx_GetStringSafe(stubbed));
                            Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                            Tcl_DStringAppend(&out, "\n", 1);
                            Tcl_DStringFree(&ln);
                            Tcl_DecrRefCount(stubbed);
                            Tcl_DecrRefCount(block);
                        }
                    }
                    else
                    {
                        /* Inline form: private method/ctor/dtor ...
                           Copy verbatim (it will be captured by the
                           dynamic probe and handled like any other
                           method definition). */
                        Tcl_DStringAppend(&out, p.commandStart, p.commandSize);
                    }
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
    /* Note: even when i==0 && j==n (no trimming), we return a new Tcl_Obj
       because callers may DecrRefCount both the input and the result
       independently, and aliasing would cause a double-free. */
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
    if (depth > TBCX_MAX_BLOCK_DEPTH)
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

/* Handle all oo::define, oo::class, oo::objdefine, and metaclass commands.
 * Returns 1 if the command was handled, 0 otherwise.
 * The DString ctx->out is appended with the rewritten command text. */
static int RewriteOoCmd(RewriteCtx* ctx, Tcl_Parse* p, const Tcl_Token* w0, const char* c0, int* handled)
{
    /* Recover the command-word literal (needed by the metaclass handler).
       The caller guarantees w0 is a valid literal token. */
    Tcl_Obj* cmd = WordLiteralObj(w0);

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
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 3 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wN = tokP, *wA = NextWord(wN), *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
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
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: stub the body */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::define");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(cls));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(kwd));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(mname));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    Tcl_DStringAppendElement(&line, "");
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
                    Tcl_DStringAppendElement(&line, "");
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
                /* Capture from builder body */
                CaptureClassBody(ctx->ip, bs, bl, ctx->curNs, clsFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0);
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
                }
                Tcl_DecrRefCount(clsFqn);
                *handled = 1;
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
                    CS_Add(ctx->classes, clsFqn);
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
                            CaptureClassBody(ctx->ip, bs, bl, ctx->curNs, clsFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0);
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
                            }
                            Tcl_DecrRefCount(bod);
                        }
                        else
                        {
                            Tcl_DStringAppendElement(&cmdLn, "");
                            Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                            Tcl_DStringAppend(ctx->out, "\n", 1);
                        }
                    }
                    else
                    {
                        Tcl_DStringAppendElement(&cmdLn, "");
                        Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                        Tcl_DStringAppend(ctx->out, "\n", 1);
                    }
                    Tcl_DStringFree(&cmdLn);
                    Tcl_DecrRefCount(clsFqn);
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
                                    strcmp(pk, "variable") == 0 || strcmp(pk, "superclass") == 0 || strcmp(pk, "mixin") == 0 ||
                                    strcmp(pk, "filter") == 0 || strcmp(pk, "forward") == 0 || strcmp(pk, "private") == 0)
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
                        CS_Add(ctx->classes, clsFqn);
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
                                CaptureClassBody(
                                    ctx->ip, bs, bl, ctx->curNs, clsFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0);
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
                                }
                                Tcl_DecrRefCount(bod);
                            }
                            else
                            {
                                Tcl_DStringAppendElement(&cmdLn, "");
                                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                Tcl_DStringAppend(ctx->out, "\n", 1);
                            }
                        }
                        Tcl_DStringFree(&cmdLn);
                        Tcl_DecrRefCount(clsFqn);
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
                /* Capture from builder body */
                CaptureClassBody(ctx->ip, bs, bl, ctx->curNs, objFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0);
                CS_Add(ctx->classes, objFqn);
                /* Rewrite: check purity then stub */
                if (IsPureOodefineBuilderBody(ctx->ip, bs, bl))
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
                                    /* Emit all words except last (body), then empty stub */
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
                                    Tcl_DStringAppendElement(&ln, "");
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
                                    Tcl_DStringAppendElement(&ln, "");
                                }
                                else if (strcmp(pk, "destructor") == 0)
                                {
                                    Tcl_DStringAppendElement(&ln, "destructor");
                                    Tcl_DStringAppendElement(&ln, ";");
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
                const Tcl_Token* tokP = w0;
                for (Tcl_Size w = 0; w + 3 < p->numWords; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wN = tokP, *wA = NextWord(wN), *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
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
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: stub the body */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::objdefine");
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(objNm));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(mname));
                    Tcl_DStringAppendElement(&line, Tbcx_GetStringSafe(args));
                    Tcl_DStringAppendElement(&line, "");
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
                    Tcl_DStringAppendElement(&line, "");
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
     * Recurse into each branch body to capture proc/class
     * definitions and stub their source.  Safe even when
     * multiple branches define conflicting procs with the
     * same name, because each captured proc gets a unique
     * index in the Procs section and the loader's ProcShim
     * matches by indexed marker (TBCX_PROC_MARKER_PFX),
     * not by FQN alone. */
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
                                Tcl_DecrRefCount(nsFqn);
                                handled = 1;
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

static int EmitTbcxStream(Tcl_Obj* scriptObj, TbcxOut* w)
{
    int rc = TCL_ERROR; /* set to TCL_OK only on success */
    TbcxCtx ctx = {0};
    ctx.interp = w->interp;
    CtxInitStripBodies(&ctx);
    CtxInitCompiled(&ctx);
    CtxInitNsEval(&ctx);
    Tcl_InitHashTable(&ctx.emittedBodies, TCL_STRING_KEYS);
    ctx.emittedInit = 1;
    Tcl_InitHashTable(&ctx.emittedPtrs, TCL_ONE_WORD_KEYS);
    ctx.emittedPtrsInit = 1;
    Tcl_InitHashTable(&ctx.instrBodyLits, TCL_ONE_WORD_KEYS);
    ctx.instrBodyInit = 1;

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

    /* Build strip-bodies set from captured definitions — used by WriteLiteral
       to avoid serializing proc bodies as nested bytecode in the top-level block. */
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if (defs.v[i].body)
        {
            CtxAddStripBody(&ctx, defs.v[i].body);
        }
    }

    /* 1. Compile top-level (after capture + rewrite) */
    if (TclSetByteCodeFromAny(w->interp, srcCopy, NULL, NULL) != TCL_OK)
    {
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

    /* pre-compile namespace eval bodies and script-like
       string literals in the literal pool so they serialize as
       bytecode rather than source text. */
    {
        /* Scan the ORIGINAL script first.  CaptureAndRewriteScript may
           fail to rewrite some namespace eval commands (e.g. if an earlier
           command in the script triggers a parse error that causes the
           rewrite loop to skip-and-continue past it).  For those commands,
           the original 4-word form is passed through verbatim, and the
           compiled literal pool holds the ORIGINAL body text.
           Registering the original bodies ensures PrecompileLiteralPool
           can match them even when the rewrite didn't transform them. */
        ScanForNsEvalBodies(&ctx, srcStr, srcLen);
        /* Scan the REWRITTEN script.  For namespace eval commands that WERE
           successfully rewritten to 3-arg ::tcl::namespace::eval form with
           stubbed bodies, this registers the STUBBED body text — which is
           what the compiled literal pool actually contains. */
        Tcl_Size rewLen = 0;
        const char* rewStr = Tbcx_GetStringFromObjSafe(srcCopy, &rewLen);
        ScanForNsEvalBodies(&ctx, rewStr, rewLen);
    }
    /* Also scan captured proc/method/class bodies for script-body
       patterns (uplevel, eval, try, etc.) that produce string literals
       in separately compiled bytecodes.  The rewritten script has these
       bodies stubbed, so ScanForNsEvalBodies above cannot see them.
       WriteLiteral will check nsEvalBodies when serializing nested
       blocks and precompile matching strings on the fly. */
    for (Tcl_Size i = 0; i < defs.n; i++)
    {
        if (defs.v[i].body)
        {
            Tcl_Size bLen = 0;
            const char* bStr = Tbcx_GetStringFromObjSafe(defs.v[i].body, &bLen);
            if (bLen >= 2)
            {
                Tcl_Obj* defNs = defs.v[i].ns ? defs.v[i].ns : rootNs;
                ScanScriptBodiesRec(&ctx, bStr, bLen, defNs, 0);
            }
        }
    }
    PrecompileLiteralPool(&ctx, top);

    /* 3. Header */
    WriteHeaderTop(w, &ctx, srcCopy);
    if (w->err)
        goto cleanup;

    /* 4. Top-level compiled block */
    ctx.stripActive = 1;
    WriteCompiledBlock(w, &ctx, srcCopy);
    ctx.stripActive = 0;
    if (w->err)
        goto cleanup;

    /* 5. Procs section (nameFqn, namespace, args, block) */
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
            if (!tbcx_checked_mul(sizeof(const char*), (size_t)numClasses, &keyBytes)) {
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
done_classes: ;
    }

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
                /* Improvement #2: self methods get wire kind 4 (TBCX_METH_SELF)
                   so the load side installs them via oo::objdefine, not oo::define. */
                if (defs.v[i].flags & DEF_F_SELF_METHOD)
                    wireKind = 4; /* TBCX_METH_SELF */
                W_U8(w, wireKind);
            }
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

            /* Compile & emit block (proc semantics) */
            if (CompileProcLike(w, &ctx, defs.v[i].cls, defs.v[i].args, defs.v[i].body, "body of method") != TCL_OK)
                goto cleanup;
        }

    Tbcx_W_Flush(w); /* flush buffered writes before returning */
    rc = (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

cleanup:
    DV_Free(&defs);
    CS_Free(&classes);
    Tcl_DecrRefCount(srcCopy);
    Tcl_DecrRefCount(rootNs);
    CtxFreeStripBodies(&ctx);
    CtxFreeCompiled(&ctx);
    CtxFreeNsEval(&ctx);
    if (ctx.emittedInit)
        Tcl_DeleteHashTable(&ctx.emittedBodies);
    if (ctx.emittedPtrsInit)
        Tcl_DeleteHashTable(&ctx.emittedPtrs);
    if (ctx.instrBodyInit)
        Tcl_DeleteHashTable(&ctx.instrBodyLits);
    return rc;
}

/* Ensure a channel is in binary mode for .tbcx I/O.
 * NOTE: This permanently modifies the channel's translation and eofchar
 * settings.  For caller-provided channels, this is intentional — .tbcx
 * streams require raw binary I/O, and leaving the channel in text mode
 * would corrupt the data.  Callers who need the original settings should
 * save/restore them themselves, or use file-path arguments instead. */
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
 * Synopsis:   tbcx::save in out
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
    if (objc != 3)
    {
        Tcl_WrongNumArgs(interp, 1, objv, "in out");
        return TCL_ERROR;
    }

    Tcl_Obj* inObj = objv[1];
    Tcl_Obj* outObj = objv[2];

    Tcl_Channel inCh = NULL;
    Tcl_Obj* script = NULL;
    int rc = TCL_ERROR;

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
            return TCL_ERROR;
        }
        tmpPath = Tcl_DuplicateObj(outNorm);
        /* Generate a unique temp name to prevent races between concurrent
           tbcx::save calls targeting the same destination. */
        {
            Tcl_MutexLock(&tbcxSaveTmpMutex);
            uint64_t myTmpId = tbcxSaveTmpId++;
            Tcl_MutexUnlock(&tbcxSaveTmpMutex);
            Tcl_Obj* suffix = Tcl_ObjPrintf(".tbcx.%" PRIu64 ".tmp", myTmpId);
            Tcl_IncrRefCount(suffix);
            Tcl_AppendObjToObj(tmpPath, suffix);
            Tcl_DecrRefCount(suffix);
        }
        Tcl_IncrRefCount(tmpPath);
        outCh = Tcl_FSOpenFileChannel(interp, tmpPath, "w", 0666);
        if (!outCh)
        {
            Tcl_DecrRefCount(tmpPath);
            Tcl_DecrRefCount(script);
            return TCL_ERROR;
        }
        weOpenedOut = 1;
        if (Tbcx_CheckBinaryChan(interp, outCh) != TCL_OK)
        {
            Tcl_Close(interp, outCh);
            Tcl_FSDeleteFile(tmpPath);
            Tcl_DecrRefCount(tmpPath);
            Tcl_DecrRefCount(script);
            return TCL_ERROR;
        }
    }

    TbcxOut w;
    Tbcx_W_Init(&w, interp, outCh);
    rc = EmitTbcxStream(script, &w);
    Tcl_DecrRefCount(script);

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
