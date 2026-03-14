/* ==========================================================================
 * tbcxsave.c — TBCX compile+save for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

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
    size_t n, cap;
} DefVec;

typedef struct
{
    Tcl_HashTable ht; /* TCL_STRING_KEYS; key storage is owned by Tcl */
    int init;
} ClsSet;

#define DEF_F_FROM_BUILDER 0x01
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
static Tcl_Obj*
CaptureAndRewriteScript(Tcl_Interp* ip, const char* script, Tcl_Size len, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
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
static void CtxAddNsEval(TbcxCtx* ctx, const char* bodyText, Tcl_Obj* nsFqn);
static void CtxFreeNsEval(TbcxCtx* ctx);
static void PrecompileLiteralPool(TbcxCtx* ctx, ByteCode* top);
static void ScanForNsEvalBodies(TbcxCtx* ctx, const char* script, Tcl_Size len);
static void ScanScriptBodiesRec(TbcxCtx* ctx, const char* script, Tcl_Size len, Tcl_Obj* curNs, int depth);
static void RegisterBodyAndRecurse(TbcxCtx* ctx, const Tcl_Token* tok, Tcl_Obj* curNs, int depth);
static Tcl_Obj* RecurseScriptBody(Tcl_Interp* ip, const Tcl_Token* bodyTok, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth);
static void DV_Free(DefVec* dv);
static void DV_Init(DefVec* dv);
static void DV_Push(DefVec* dv, DefRec r);
static int EmitTbcxStream(Tcl_Obj* scriptObj, TbcxOut* w);
static Tcl_Obj* FqnUnder(Tcl_Obj* curNs, Tcl_Obj* name);
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

/* Buffered write I/O — collapses thousands of syscalls into a handful */
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
    size_t off = 0;
    while (off < w->bufPos)
    {
        Tcl_Size toWrite = (Tcl_Size)(w->bufPos - off);
        Tcl_Size got = Tcl_WriteRaw(w->chan, (const char*)w->buf + off, toWrite);
        if (got <= 0)
        {
            W_Error(w, "tbcx: short write");
            return w->err;
        }
        off += (size_t)got;
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
    if (!tbcxHostIsLE)
    {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    W_Bytes(w, &v, 4);
}
static inline void W_U64(TbcxOut* w, uint64_t v)
{
    if (!tbcxHostIsLE)
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
    const char* s = Tcl_GetStringFromObj(body, &len);
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
    const char* s = Tcl_GetStringFromObj(obj, &len);
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

static void CtxAddNsEval(TbcxCtx* ctx, const char* bodyText, Tcl_Obj* nsFqn)
{
    if (!ctx || !ctx->nsEvalInit || !bodyText || !nsFqn)
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
            const char* e = Tcl_GetStringFromObj(existing, &eLen);
            const char* n = Tcl_GetStringFromObj(nsFqn, &nLen);
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
    const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
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
            CtxAddNsEval(ctx, bs, curNs);
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
                const char* c0 = Tcl_GetString(cmd);

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
                        const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
                        const char* nsn = Tcl_GetString(nsObj);
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
                        CtxAddNsEval(ctx, bs, nsFqn);
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
                    if (sub && strcmp(Tcl_GetString(sub), "eval") == 0)
                    {
                        const Tcl_Token* w2 = NextWord(w1);
                        const Tcl_Token* w3 = NextWord(w2);
                        Tcl_Obj* nsObj = WordLiteralObj(w2);
                        Tcl_Obj* bodyObj = WordLiteralObj(w3);
                        if (nsObj && bodyObj)
                        {
                            Tcl_Size bl;
                            const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
                            Tcl_Obj* nsFqn = FqnUnder(curNs, nsObj);
                            CtxAddNsEval(ctx, bs, nsFqn);
                            ScanScriptBodiesRec(ctx, bs, bl, nsFqn, depth + 1);
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
                        const char* ks = Tcl_GetString(kw);
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
                    for (int fw = 0; fw < p.numWords - 1; fw++)
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
                    for (int fw = 0; fw < p.numWords - 1; fw++)
                        tok = NextWord(tok);
                    Tcl_Obj* bodyObj = WordLiteralObj(tok);
                    if (bodyObj)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
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
                        const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
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
                        if (strcmp(Tcl_GetString(probe), "then") == 0 && w < p.numWords)
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
                        const char* ks = Tcl_GetString(kw);
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
                                if (strcmp(Tcl_GetString(probe), "then") == 0 && w < p.numWords)
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
                        if (strcmp(Tcl_GetString(sub), "create") == 0 && p.numWords >= 4)
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
                    for (int fw = 0; fw < p.numWords - 1; fw++)
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
                        const char* sc = Tcl_GetString(sub);
                        /* dict for {k v} dict body  — 5 words total */
                        if ((strcmp(sc, "for") == 0 || strcmp(sc, "map") == 0) && p.numWords == 5)
                        {
                            const Tcl_Token* tok = w0;
                            for (int fw = 0; fw < p.numWords - 1; fw++)
                                tok = NextWord(tok);
                            RegisterBodyAndRecurse(ctx, tok, curNs, depth);
                        }
                        /* dict with dictVar ?key ...? body — last word is body, >=3 words */
                        else if (strcmp(sc, "with") == 0 && p.numWords >= 4)
                        {
                            const Tcl_Token* tok = w0;
                            for (int fw = 0; fw < p.numWords - 1; fw++)
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
                    for (int fw = 0; fw < p.numWords - 1; fw++)
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
                                const char* bs = Tcl_GetStringFromObj(elems[si], &bl);
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
                                        CtxAddNsEval(ctx, bs, curNs);
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
                            const char* os = Tcl_GetString(opt);
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
                                    const char* cs = Tcl_GetStringFromObj(cmdVal, &cl);
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
                for (int wIdx = 0; wIdx < p.numWords; wIdx++)
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
        const char* s = Tcl_GetStringFromObj(lit, &sLen);
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
                he = Tcl_FindHashEntry(&ctx->nsEvalBodies, Tcl_GetString(trimmed));
                Tcl_DecrRefCount(trimmed);
            }
        }
        if (!he)
            continue;
        Tcl_Obj* nsObj = (Tcl_Obj*)Tcl_GetHashValue(he);
        if (!nsObj)
            continue; /* NULL = multi-namespace conflict, skip */

        Namespace* targetNs = (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tcl_GetString(nsObj));
        if (!targetNs)
            continue;

        /* Compile a SEPARATE copy — never touch the pool. */
        Tcl_Obj* copy = Tcl_DuplicateObj(lit);
        Tcl_IncrRefCount(copy);
        if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
        {
            ByteCode* bc = NULL;
            ByteCodeGetInternalRep(copy, tbcxTyBytecode, bc);
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

static Tcl_Obj* NsFqn(Tcl_Namespace* nsPtr)
{
    if (!nsPtr)
        return Tcl_NewStringObj("::", -1);
    Tcl_Obj* o = Tcl_NewStringObj(nsPtr->fullName, -1);
    return o;
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
        be = (unsigned char*)Tcl_Alloc(be_bytes);
        if (!be)
        {
            W_Error(w, "tbcx: oom");
            TclBN_mp_clear(&mag);
            TclBN_mp_clear(&z);
            return;
        }
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
    KVPair* pairs = (KVPair*)Tcl_Alloc(sizeof(KVPair) * (size_t)(sz ? sz : 1));
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
        const char* nsName = Tcl_GetString(nsObjIn);
        nsPtr = Tcl_FindNamespace(ctx->interp, nsName, NULL, 0);
        if (!nsPtr)
        {
            nsPtr = Tcl_CreateNamespace(ctx->interp, nsName, NULL, NULL);
        }
        nsFQN = Tcl_NewStringObj(nsName, -1);
    }
    else
    {
        nsPtr = Tcl_GetCurrentNamespace(ctx->interp);
        nsFQN = NsFqn(nsPtr);
    }
    Tcl_IncrRefCount(nsFQN);
    int compiled_ok = 0;
    Tcl_Size nsLen = 0;
    const char* nsStr = Tcl_GetStringFromObj(nsFQN, &nsLen);
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
        const char* nm = Tcl_GetStringFromObj(fv[0], &nmLen);
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
        int numA = 0;
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
        W_Error(w, "tbcx: lambda compile");
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
        const char* bStr = Tcl_GetStringFromObj(bodyObj, &bLen);
        W_LPString(w, bStr, bLen);
    }

    Tcl_DecrRefCount(bodyObj);

lambda_cleanup:
    if (!compiled_ok)
    {
        Tbcx_FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char*)procPtr);
    }
    /* When compiled_ok is true, the Tcl compiler has attached procPtr to
       the ByteCode (bc->procPtr = procPtr).  TclCleanupByteCode owns the
       Proc and will free it when the ByteCode is destroyed.  Even if a
       subsequent write error occurred (w->err), the ByteCode's Tcl_Obj
       is still alive on the call stack and will be cleaned up normally. */

    Tcl_DecrRefCount(nsFQN);
}

static void Lit_Bytecode(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj)
{
    ByteCode* codePtr = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, codePtr);
    Tcl_Obj* nsFQN = NsFqn((Tcl_Namespace*)(codePtr ? codePtr->nsPtr : NULL));
    Tcl_IncrRefCount(nsFQN);
    Tcl_Size nsLen;
    const char* nsStr = Tcl_GetStringFromObj(nsFQN, &nsLen);
    W_LPString(w, nsStr, nsLen);
    WriteCompiledBlock(w, ctx, bcObj);
    Tcl_DecrRefCount(nsFQN);
}

int Tbcx_BuildLocals(Tcl_Interp* ip, Tcl_Obj* argsList, CompiledLocal** firstOut, CompiledLocal** lastOut, int* numArgsOut)
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
    /* Guard against Tcl_Size -> int overflow (CompiledLocal fields are int) */
    if (argc > INT_MAX)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: argument list too large", -1));
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
        const char* nm = Tcl_GetStringFromObj(fv[0], &nmLen);
        size_t allocSize = offsetof(CompiledLocal, name) + 1u + (size_t)nmLen;
        if (allocSize < sizeof(CompiledLocal))
            allocSize = sizeof(CompiledLocal);
        CompiledLocal* cl = (CompiledLocal*)Tcl_Alloc(allocSize);
        memset(cl, 0, allocSize);
        cl->nameLength = (int)nmLen;
        memcpy(cl->name, nm, (size_t)nmLen + 1);
        cl->frameIndex = (int)i;
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
    *numArgsOut = (int)argc;
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
    const char* nsNm = Tcl_GetString(nsFQN);
    Tcl_Namespace* ns = Tcl_FindNamespace(ip, nsNm, NULL, 0);
    if (!ns)
        ns = Tcl_CreateNamespace(ip, nsNm, NULL, NULL);
    Proc* procPtr = (Proc*)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr = (Interp*)ip;
    procPtr->refCount = 1;
    {
        CompiledLocal *first = NULL, *last = NULL;
        int numA = 0;
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
        Tcl_DecrRefCount(bodyObj);
        W_Error(w, "tbcx: proc-like compile failed");
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
        Tcl_Obj** tmp = (Tcl_Obj**)Tcl_Alloc((size_t)n * sizeof(Tcl_Obj*));
        memset(tmp, 0, (size_t)n * sizeof(Tcl_Obj*));
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
                const char* s = Tcl_GetStringFromObj(tmp[i], &ln);
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
                const char* s = Tcl_GetStringFromObj(names[i], &ln);
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
        Tcl_Size sn = 0;
        (void)Tcl_GetStringFromObj(obj, &sn);
        if (sn >= 4)
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
                        const char* bodyStr = Tcl_GetStringFromObj(elems[1], &bodyLen);
                        if (bodyLen >= 4)
                        {
                            int startsWithLetter =
                                ((bodyStr[0] >= 'a' && bodyStr[0] <= 'z') || (bodyStr[0] >= 'A' && bodyStr[0] <= 'Z'));
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
            Tcl_Size sLen = 0;
            const char* ss = Tcl_GetStringFromObj(obj, &sLen);
            Tcl_CreateHashEntry(&ctx->emittedBodies, ss, &isNew);
        }
        W_U32(w, TBCX_LIT_BYTECODE);
        Lit_Bytecode(w, ctx, compiled);
        return;
    }

    Tcl_Size n;
    const char* s = Tcl_GetStringFromObj(obj, &n);

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
                    he = Tcl_FindHashEntry(&ctx->nsEvalBodies, Tcl_GetString(trimmed));
                    Tcl_DecrRefCount(trimmed);
                }
            }
            Tcl_Obj* nsObj = he ? (Tcl_Obj*)Tcl_GetHashValue(he) : NULL;
            if (nsObj)
            {
                Namespace* targetNs = (Namespace*)Tbcx_EnsureNamespace(ctx->interp, Tcl_GetString(nsObj));
                if (targetNs)
                {
                    Tcl_Obj* copy = Tcl_DuplicateObj(obj);
                    Tcl_IncrRefCount(copy);
                    if (TclSetByteCodeFromAny(ctx->interp, copy, NULL, NULL) == TCL_OK)
                    {
                        ByteCode* bc = NULL;
                        ByteCodeGetInternalRep(copy, tbcxTyBytecode, bc);
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
                            W_U32(w, TBCX_LIT_BYTECODE);
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
            const char* canon = Tcl_GetStringFromObj(canonObj, &cl);
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

    /* Fallback: emit as plain string */
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
        Tcl_GetBooleanFromObj(NULL, obj, &b);
        W_U32(w, TBCX_LIT_BOOLEAN);
        W_U8(w, (uint8_t)(b != 0));
    }
    else if (ty == tbcxTyByteArray)
    {
        Tcl_Size n;
        unsigned char* p = Tcl_GetByteArrayFromObj(obj, &n);
        W_U32(w, TBCX_LIT_BYTEARR);
        W_U32(w, (uint32_t)n);
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
        Tcl_GetDoubleFromObj(NULL, obj, &d);
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
                    const char* bodyStr = Tcl_GetStringFromObj(elems[1], &bodyLen);
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
                    const char* bsStr = Tcl_GetStringFromObj(obj, &bsLen);
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
                const char* fs = Tcl_GetStringFromObj(obj, &fl);
                W_U32(w, TBCX_LIT_STRING);
                W_LPString(w, fs, fl);
            }
            else
            {
                W_U32(w, TBCX_LIT_BYTECODE);
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
                ByteCodeGetInternalRep(p->bodyPtr, tbcxTyBytecode, bc);
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
                                const char* fs = Tcl_GetStringFromObj(p->bodyPtr, &fl);
                                W_U32(w, TBCX_LIT_STRING);
                                W_LPString(w, fs, fl);
                                return;
                            }
                        }
                        if (ctx->emittedInit)
                        {
                            Tcl_Size pbLen = 0;
                            const char* pbStr = Tcl_GetStringFromObj(p->bodyPtr, &pbLen);
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
                    W_U32(w, TBCX_LIT_BYTECODE);
                    Lit_Bytecode(w, ctx, p->bodyPtr);
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
    JTEntry* arr = (JTEntry*)Tcl_Alloc(sizeof(JTEntry) * cnt);
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
    JTNumEntry* arr = (JTNumEntry*)Tcl_Alloc(sizeof(JTNumEntry) * cnt);
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
    ByteCodeGetInternalRep(cand, tbcxTyBytecode, bc);
    if (bc)
        return cand;
    if (cand->typePtr == tbcxTyProcBody)
    {
        Proc* p = (Proc*)cand->internalRep.twoPtrValue.ptr1;
        if (p && p->bodyPtr)
        {
            ByteCode* bc2 = NULL;
            ByteCodeGetInternalRep(p->bodyPtr, tbcxTyBytecode, bc2);
            if (bc2)
                return p->bodyPtr;
        }
    }
    return NULL;
}

static void WriteCompiledBlock(TbcxOut* w, TbcxCtx* ctx, Tcl_Obj* bcObj)
{
    ByteCode* bc = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, bc);

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
    W_U32(w, (uint32_t)bc->numLitObjects);
    for (Tcl_Size i = 0; i < bc->numLitObjects; i++)
    {
        Tcl_Obj* lit = bc->objArrayPtr[i];
        WriteLiteral(w, ctx, lit);
        if (w->err)
        {
            if (ctx)
                ctx->blockDepth--;
            return;
        }
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
    ByteCodeGetInternalRep(topObj, tbcxTyBytecode, top);
    if (!top)
    {
        W_Error(w, "tbcx: failed to get top bytecode");
        return;
    }
    TbcxHeader H;
    H.magic = TBCX_MAGIC;
    H.format = TBCX_FORMAT;
    H.tcl_version = PackTclVersion();
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
        size_t newCap = dv->cap ? dv->cap * 2 : 16;
        /* Guard against pathological overflow */
        if (newCap < dv->cap || newCap > (SIZE_MAX / sizeof(DefRec)))
            Tcl_Panic("tbcx: DefVec overflow");
        dv->cap = newCap;
        dv->v = (DefRec*)Tcl_Realloc(dv->v, dv->cap * sizeof(DefRec));
    }
    dv->v[dv->n++] = r;
}
static void DV_Free(DefVec* dv)
{
    for (size_t i = 0; i < dv->n; i++)
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
    const char* s = Tcl_GetString(clsFqn);
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

static Tcl_Obj* FqnUnder(Tcl_Obj* curNs, Tcl_Obj* name)
{
    const char* nm = Tcl_GetString(name);
    if (nm[0] == ':' && nm[1] == ':')
    {
        Tcl_IncrRefCount(name);
        return name;
    }
    Tcl_Size ln = 0;
    const char* ns = Tcl_GetStringFromObj(curNs, &ln);
    Tcl_Obj* fqn = Tcl_NewStringObj(ns, ln);
    if (!(ln == 2 && ns[0] == ':' && ns[1] == ':'))
        Tcl_AppendToObj(fqn, "::", 2);
    Tcl_AppendObjToObj(fqn, name);
    Tcl_IncrRefCount(fqn);
    return fqn;
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
                const char* kw = CmdCore(Tcl_GetString(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    /* Name is always the word after "method"/"classmethod".
                       Args and body are always the last two words.
                       Any option flags (-export/-private/-unexport) sit
                       between name and args and are ignored here. */
                    const Tcl_Token* wN = NextWord(w0);
                    const Tcl_Token* t = w0;
                    for (int w = 0; w < p.numWords - 2; w++)
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
                        r.name = mname;
                        Tcl_IncrRefCount(r.name);
                        r.args = args;
                        Tcl_IncrRefCount(r.args);
                        r.body = body;
                        Tcl_IncrRefCount(r.body);
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
                    for (int w = 0; w < p.numWords - 1; w++)
                        tokWalk = NextWord(tokWalk);
                    wBody = tokWalk;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj* args = (wArgs ? WordLiteralObj(wArgs) : Tcl_NewStringObj("", 0));
                    Tcl_Obj* body = WordLiteralObj(wBody);
                    /* Coerce constructor arg-spec to a list value (matches proc/method handling). */
                    if (args)
                    {
                        Tcl_Size _tbcx_dummy;
                        (void)Tcl_ListObjLength(ip, args, &_tbcx_dummy);
                    }
                    if (args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_CTOR;
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.args = args;
                        Tcl_IncrRefCount(r.args);
                        r.body = body;
                        Tcl_IncrRefCount(r.body);
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
                    for (int w = 0; w < p.numWords - 1; w++)
                        tokWalk = NextWord(tokWalk);
                    const Tcl_Token* wBody = tokWalk;
                    Tcl_Obj* args = NULL;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        args = WordLiteralObj(pre);
                    }
                    else
                    {
                        args = Tcl_NewStringObj("", 0);
                    }
                    Tcl_Obj* body = WordLiteralObj(wBody);
                    /* Keep destructor arg list canonical too for consistency. */
                    if (args)
                    {
                        Tcl_Size _tbcx_dummy;
                        (void)Tcl_ListObjLength(ip, args, &_tbcx_dummy);
                    }
                    if (args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_DTOR;
                        r.name = NULL; /* destructors have no name */
                        r.cls = clsFqn;
                        Tcl_IncrRefCount(r.cls);
                        r.args = args;
                        Tcl_IncrRefCount(r.args);
                        r.body = body;
                        Tcl_IncrRefCount(r.body);
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
                /* "private { script }" — recurse into the block to capture
                   methods/ctors/dtors defined in private context. */
                else if (strcmp(kw, "private") == 0 && p.numWords >= 2)
                {
                    const Tcl_Token* wBlock = NextWord(w0);
                    Tcl_Obj* block = WordLiteralObj(wBlock);
                    if (block)
                    {
                        Tcl_Size bkLen = 0;
                        const char* bkStr = Tcl_GetStringFromObj(block, &bkLen);
                        CaptureClassBody(ip, bkStr, bkLen, curNs, clsFqn, defs, classes, flags, depth + 1);
                        Tcl_DecrRefCount(block);
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
    const char* cls = Tcl_GetStringFromObj(clsFqn, &fqnLen);

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
                    const char* kw = CmdCore(Tcl_GetString(cmd));
                    /* Only emit declarative, order-independent keywords. */
                    if (strcmp(kw, "variable") == 0 || strcmp(kw, "superclass") == 0 || strcmp(kw, "mixin") == 0 ||
                        strcmp(kw, "filter") == 0 || strcmp(kw, "forward") == 0 || strcmp(kw, "self") == 0)
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
                            Tcl_DStringAppendElement(&ln, Tcl_GetString(rawCmd));
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
    for (size_t i = 0; i < defs->n; i++)
    {
        DefRec* r = &defs->v[i];
        if (r->kind == DEF_KIND_PROC)
            continue;
        if ((r->flags & DEF_F_FROM_BUILDER) == 0)
            continue;
        Tcl_Size thisLen = 0;
        const char* thisCls = Tcl_GetStringFromObj(r->cls, &thisLen);
        if (!(thisLen == fqnLen && memcmp(thisCls, cls, (size_t)fqnLen) == 0))
            continue;

        Tcl_DString ln;
        Tcl_DStringInit(&ln);
        Tcl_Size tmp;
        Tcl_DStringAppendElement(&ln, "oo::define");
        Tcl_DStringAppendElement(&ln, cls);

        if (r->kind == DEF_KIND_INST || r->kind == DEF_KIND_CLASS)
        {
            Tcl_DStringAppendElement(&ln, (r->kind == DEF_KIND_CLASS) ? "classmethod" : "method");
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->name, &tmp));
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->args, &tmp));
            Tcl_DStringAppendElement(&ln, "");
        }
        else if (r->kind == DEF_KIND_CTOR)
        {
            Tcl_DStringAppendElement(&ln, "constructor");
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->args, &tmp));
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
        const char* core = CmdCore(Tcl_GetString(cmd));

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
    const char* bstr = Tcl_GetStringFromObj(bodyObj, &blen);
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
                const char* kw = CmdCore(Tcl_GetString(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4)
                {
                    /* Emit all words except the last (body), then append
                       an empty stub body.  This preserves the method name
                       and any option flags (-export/-private/-unexport)
                       between name and args. */
                    Tcl_DString ln;
                    Tcl_DStringInit(&ln);
                    const Tcl_Token* t = w0;
                    for (int w = 0; w < p.numWords - 1; w++)
                    {
                        Tcl_Obj* word = WordLiteralObj(t);
                        if (word)
                        {
                            Tcl_DStringAppendElement(&ln, Tcl_GetString(word));
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
                    for (int w = 0; w < p.numWords - 1; w++)
                        t = NextWord(t);
                    const Tcl_Token* wArgs = NULL;
                    if (p.numWords >= 3)
                    {
                        const Tcl_Token* pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj* args = wArgs ? WordLiteralObj(wArgs) : Tcl_NewStringObj("", 0);
                    if (args)
                    {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "constructor");
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(args));
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
                            Tcl_DStringAppendElement(&ln, Tcl_GetString(stubbed));
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
    Tcl_DStringFree(&out);
    return res;
}

static Tcl_Obj* CanonTrivia(Tcl_Obj* in)
{
    if (!in)
        return Tcl_NewStringObj("", 0);
    Tcl_Size n = 0;
    const char* s = Tcl_GetStringFromObj(in, &n);
    if (n == 0)
        return Tcl_NewStringObj("", 0);

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
        return Tcl_NewStringObj("", 0);
    /* Note: even when i==0 && j==n (no trimming), we return a new Tcl_Obj
       because callers may DecrRefCount both the input and the result
       independently, and aliasing would cause a double-free. */
    return Tcl_NewStringObj(s + i, j - i);
}

/* ==========================================================================
 * Recurse into control-flow script bodies
 *
 * Given a word token, extract its literal text, recurse through
 * CaptureAndRewriteScript to stub nested proc/method definitions,
 * and return the rewritten text.  Returns NULL if the token is not a
 * literal OR if the body text is unchanged (no definitions found).
 * ========================================================================== */
static Tcl_Obj* RecurseScriptBody(Tcl_Interp* ip, const Tcl_Token* bodyTok, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth)
{
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return NULL;
    Tcl_Obj* bodyObj = WordLiteralObj(bodyTok);
    if (!bodyObj)
        return NULL;
    Tcl_Size bl = 0;
    const char* bs = Tcl_GetStringFromObj(bodyObj, &bl);
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
    const char* rs = Tcl_GetStringFromObj(rewritten, &rl);
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

typedef struct {
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
static int
RewriteOoCmd(RewriteCtx* ctx, Tcl_Parse* p, const Tcl_Token* w0, const char* c0, int* handled)
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
            Tcl_Obj* clsFqn = FqnUnder(ctx->curNs, cls);
            const char* kw = Tcl_GetString(kwd);

            if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p->numWords >= 6)
            {
                const Tcl_Token* tokP = w0;
                for (int w = 0; w < p->numWords - 3; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wN = tokP, *wA = NextWord(wN), *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
                if (args)
                {
                    Tcl_Size _d;
                    (void)Tcl_ListObjLength(ctx->ip, args, &_d);
                }
                if (mname && args && body)
                {
                    /* Capture */
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "classmethod") == 0 ? DEF_KIND_CLASS : DEF_KIND_INST);
                    r.cls = clsFqn;
                    Tcl_IncrRefCount(r.cls);
                    r.name = mname;
                    Tcl_IncrRefCount(r.name);
                    r.args = args;
                    Tcl_IncrRefCount(r.args);
                    r.body = body;
                    Tcl_IncrRefCount(r.body);
                    r.ns = ctx->curNs;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: stub the body */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::define");
                    Tcl_DStringAppendElement(&line, Tcl_GetString(cls));
                    Tcl_DStringAppendElement(&line, Tcl_GetString(kwd));
                    Tcl_DStringAppendElement(&line, Tcl_GetString(mname));
                    Tcl_DStringAppendElement(&line, Tcl_GetString(args));
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
                for (int w = 0; w < p->numWords - 1; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wBody = tokP, *wArgsTok = NULL;
                if (p->numWords >= 5)
                {
                    const Tcl_Token* pre = w0;
                    for (int w = 0; w < p->numWords - 2; w++)
                        pre = NextWord(pre);
                    wArgsTok = pre;
                }
                Tcl_Obj* args = wArgsTok ? WordLiteralObj(wArgsTok) : Tcl_NewStringObj("", 0);
                Tcl_Obj* body = WordLiteralObj(wBody);
                if (args)
                {
                    Tcl_Size _d;
                    (void)Tcl_ListObjLength(ctx->ip, args, &_d);
                }
                if (args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "constructor") == 0 ? DEF_KIND_CTOR : DEF_KIND_DTOR);
                    r.cls = clsFqn;
                    Tcl_IncrRefCount(r.cls);
                    r.args = args;
                    Tcl_IncrRefCount(r.args);
                    r.body = body;
                    Tcl_IncrRefCount(r.body);
                    r.ns = ctx->curNs;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, clsFqn);
                    /* Rewrite: stub */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::define");
                    Tcl_DStringAppendElement(&line, Tcl_GetString(cls));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tcl_GetString(args));
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
            Tcl_Obj* clsFqn = FqnUnder(ctx->curNs, cls);
            Tcl_Size bl = 0;
            const char* bs = Tcl_GetStringFromObj(bod, &bl);
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
                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(cls));
                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&cmdLn);
                Tcl_DecrRefCount(stubbed);
            }
            Tcl_DecrRefCount(clsFqn);
            *handled = 1;
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
        if (sub && strcmp(Tcl_GetString(sub), "create") == 0)
        {
            const Tcl_Token* w2 = NextWord(w1);
            Tcl_Obj* nm = WordLiteralObj(w2);
            if (nm)
            {
                Tcl_Obj* clsFqn = FqnUnder(ctx->curNs, nm);
                CS_Add(ctx->classes, clsFqn);
                Tcl_DString cmdLn;
                Tcl_DStringInit(&cmdLn);
                Tcl_DStringAppendElement(&cmdLn, c0);
                Tcl_DStringAppendElement(&cmdLn, "create");
                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(nm));
                if (p->numWords >= 4)
                {
                    const Tcl_Token* w3 = NextWord(w2);
                    Tcl_Obj* bod = WordLiteralObj(w3);
                    if (bod)
                    {
                        Tcl_Size bl = 0;
                        const char* bs = Tcl_GetStringFromObj(bod, &bl);
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
                            Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
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
                Tcl_DecrRefCount(nm);
                *handled = 1;
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
        if (sub && strcmp(Tcl_GetString(sub), "create") == 0)
        {
            Tcl_Obj* cmdFqn = FqnUnder(ctx->curNs, cmd);
            const char* cmdFqnStr = Tcl_GetString(cmdFqn);
            Tcl_HashEntry* clsHe = Tcl_FindHashEntry(&ctx->classes->ht, cmdFqnStr);
            int isMetaCreate = 0;
            if (clsHe)
            {
                /* Verify the 4th word is a builder body, not a ctor arg.
                   Check if it contains any OO builder keyword when parsed. */
                const Tcl_Token* w3 = NextWord(NextWord(wSub));
                Tcl_Obj* probe = WordLiteralObj(w3);
                if (probe)
                {
                    Tcl_Size pl = 0;
                    const char* ps = Tcl_GetStringFromObj(probe, &pl);
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
                                const char* pk = CmdCore(Tcl_GetString(pcmd));
                                if (strcmp(pk, "method") == 0 || strcmp(pk, "classmethod") == 0 ||
                                    strcmp(pk, "constructor") == 0 || strcmp(pk, "destructor") == 0 ||
                                    strcmp(pk, "variable") == 0 || strcmp(pk, "superclass") == 0 ||
                                    strcmp(pk, "mixin") == 0 || strcmp(pk, "filter") == 0 ||
                                    strcmp(pk, "forward") == 0 || strcmp(pk, "private") == 0)
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
            }
            if (isMetaCreate)
            {
                const Tcl_Token* w2 = NextWord(wSub);
                Tcl_Obj* nm = WordLiteralObj(w2);
                if (nm)
                {
                    Tcl_Obj* clsFqn = FqnUnder(ctx->curNs, nm);
                    CS_Add(ctx->classes, clsFqn);
                    Tcl_DString cmdLn;
                    Tcl_DStringInit(&cmdLn);
                    Tcl_DStringAppendElement(&cmdLn, c0);
                    Tcl_DStringAppendElement(&cmdLn, "create");
                    Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(nm));
                    {
                        const Tcl_Token* w3 = NextWord(w2);
                        Tcl_Obj* bod = WordLiteralObj(w3);
                        if (bod)
                        {
                            Tcl_Size bl = 0;
                            const char* bs = Tcl_GetStringFromObj(bod, &bl);
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
                                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
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
                    Tcl_DecrRefCount(nm);
                    *handled = 1;
                }
            }
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
            Tcl_Obj* objFqn = FqnUnder(ctx->curNs, objNm);
            Tcl_Size bl = 0;
            const char* bs = Tcl_GetStringFromObj(bod, &bl);
            /* Capture from builder body */
            CaptureClassBody(ctx->ip, bs, bl, ctx->curNs, objFqn, ctx->defs, ctx->classes, DEF_F_FROM_BUILDER, 0);
            CS_Add(ctx->classes, objFqn);
            /* Rewrite: check purity then stub */
            if (IsPureOodefineBuilderBody(ctx->ip, bs, bl))
            {
                /* Expand into multi-word oo::objdefine stubs,
                   walking the body to preserve command order and
                   emit non-method commands (variable etc.) verbatim. */
                const char* objNmStr = Tcl_GetString(objNm);
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
                            const char* pk = CmdCore(Tcl_GetString(pkw));
                            Tcl_DString ln;
                            Tcl_DStringInit(&ln);
                            Tcl_DStringAppendElement(&ln, "oo::objdefine");
                            Tcl_DStringAppendElement(&ln, objNmStr);
                            if ((strcmp(pk, "method") == 0 || strcmp(pk, "classmethod") == 0) && pp.numWords >= 4)
                            {
                                /* Emit all words except last (body), then empty stub */
                                const Tcl_Token* wt = pw0;
                                for (int wi = 0; wi < pp.numWords - 1; wi++)
                                {
                                    Tcl_Obj* w = WordLiteralObj(wt);
                                    if (w)
                                    {
                                        Tcl_DStringAppendElement(&ln, Tcl_GetString(w));
                                        Tcl_DecrRefCount(w);
                                    }
                                    wt = NextWord(wt);
                                }
                                Tcl_DStringAppendElement(&ln, "");
                            }
                            else if (strcmp(pk, "constructor") == 0 && pp.numWords >= 3)
                            {
                                const Tcl_Token* wt = pw0;
                                for (int wi = 0; wi < pp.numWords - 1; wi++)
                                {
                                    Tcl_Obj* w = WordLiteralObj(wt);
                                    if (w)
                                    {
                                        Tcl_DStringAppendElement(&ln, Tcl_GetString(w));
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
                                for (int wi = 0; wi < pp.numWords; wi++)
                                {
                                    Tcl_Obj* w = WordLiteralObj(wt);
                                    if (w)
                                    {
                                        Tcl_DStringAppendElement(&ln, Tcl_GetString(w));
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
                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(objNm));
                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&cmdLn);
                Tcl_DecrRefCount(stubbed);
            }
            Tcl_DecrRefCount(objFqn);
            *handled = 1;
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
            Tcl_Obj* objFqn = FqnUnder(ctx->curNs, objNm);
            const char* kw = Tcl_GetString(kwd);

            if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p->numWords >= 6)
            {
                const Tcl_Token* tokP = w0;
                for (int w = 0; w < p->numWords - 3; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wN = tokP, *wA = NextWord(wN), *wB = NextWord(wA);
                Tcl_Obj *mname = WordLiteralObj(wN), *args = WordLiteralObj(wA), *body = WordLiteralObj(wB);
                if (args)
                {
                    Tcl_Size _d;
                    (void)Tcl_ListObjLength(ctx->ip, args, &_d);
                }
                if (mname && args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "classmethod") == 0 ? DEF_KIND_CLASS : DEF_KIND_INST);
                    r.cls = objFqn;
                    Tcl_IncrRefCount(r.cls);
                    r.name = mname;
                    Tcl_IncrRefCount(r.name);
                    r.args = args;
                    Tcl_IncrRefCount(r.args);
                    r.body = body;
                    Tcl_IncrRefCount(r.body);
                    r.ns = ctx->curNs;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: stub the body */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::objdefine");
                    Tcl_DStringAppendElement(&line, Tcl_GetString(objNm));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tcl_GetString(mname));
                    Tcl_DStringAppendElement(&line, Tcl_GetString(args));
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
                for (int w = 0; w < p->numWords - 1; w++)
                    tokP = NextWord(tokP);
                const Tcl_Token *wBody = tokP, *wArgsTok = NULL;
                if (p->numWords >= 5)
                {
                    const Tcl_Token* pre = w0;
                    for (int w = 0; w < p->numWords - 2; w++)
                        pre = NextWord(pre);
                    wArgsTok = pre;
                }
                Tcl_Obj* args = wArgsTok ? WordLiteralObj(wArgsTok) : Tcl_NewStringObj("", 0);
                Tcl_Obj* body = WordLiteralObj(wBody);
                if (args)
                {
                    Tcl_Size _d;
                    (void)Tcl_ListObjLength(ctx->ip, args, &_d);
                }
                if (args && body)
                {
                    DefRec r;
                    memset(&r, 0, sizeof(r));
                    r.kind = (strcmp(kw, "constructor") == 0 ? DEF_KIND_CTOR : DEF_KIND_DTOR);
                    r.cls = objFqn;
                    Tcl_IncrRefCount(r.cls);
                    r.args = args;
                    Tcl_IncrRefCount(r.args);
                    r.body = body;
                    Tcl_IncrRefCount(r.body);
                    r.ns = ctx->curNs;
                    Tcl_IncrRefCount(r.ns);
                    DV_Push(ctx->defs, r);
                    CS_Add(ctx->classes, objFqn);
                    /* Rewrite: stub */
                    Tcl_DString line;
                    Tcl_DStringInit(&line);
                    Tcl_DStringAppendElement(&line, "oo::objdefine");
                    Tcl_DStringAppendElement(&line, Tcl_GetString(objNm));
                    Tcl_DStringAppendElement(&line, kw);
                    Tcl_DStringAppendElement(&line, Tcl_GetString(args));
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
static int
RewriteControlFlow(RewriteCtx* ctx, Tcl_Parse* p, const Tcl_Token* w0, const char* c0, int* handled)
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
                Tcl_DStringAppendElement(&ln, Tcl_GetString(condO));
                Tcl_DStringAppendElement(&ln, Tcl_GetString(rew));
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
                Tcl_DStringAppendElement(&ln, Tcl_GetString(o1));
                Tcl_DStringAppendElement(&ln, Tcl_GetString(o2));
                Tcl_DStringAppendElement(&ln, Tcl_GetString(o3));
                Tcl_DStringAppendElement(&ln, Tcl_GetString(rew));
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
        for (int fw = 0; fw < p->numWords - 1; fw++)
            tok = NextWord(tok);
        Tcl_Obj* rew = RecurseScriptBody(ctx->ip, tok, ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
        if (rew)
        {
            /* Pre-scan: all middle words must be literals */
            int allLit = 1;
            const Tcl_Token* wt = NextWord(w0);
            for (int fw = 1; fw < p->numWords - 1; fw++)
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
                for (int fw = 1; fw < p->numWords - 1; fw++)
                {
                    Tcl_Obj* wo = WordLiteralObj(wt);
                    Tcl_DStringAppendElement(&ln, Tcl_GetString(wo));
                    Tcl_DecrRefCount(wo);
                    wt = NextWord(wt);
                }
                Tcl_DStringAppendElement(&ln, Tcl_GetString(rew));
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
            for (int fw = 2; fw < p->numWords; fw++)
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
                Tcl_DStringAppendElement(&ln, Tcl_GetString(rew));
                wt = NextWord(wBody);
                for (int fw = 2; fw < p->numWords; fw++)
                {
                    Tcl_Obj* wo = WordLiteralObj(wt);
                    Tcl_DStringAppendElement(&ln, Tcl_GetString(wo));
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
        const Tcl_Token** twords = (const Tcl_Token**)Tcl_Alloc(sizeof(Tcl_Token*) * (size_t)p->numWords);
        twords[0] = w0;
        for (int fi = 1; fi < p->numWords; fi++)
            twords[fi] = NextWord(twords[fi - 1]);
        for (int fi = 0; fi < p->numWords; fi++)
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
            Tcl_Obj** rw = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)p->numWords);
            for (int fi = 0; fi < p->numWords; fi++)
                rw[fi] = WordLiteralObj(twords[fi]);
            /* Walk if structure:
               word 0 = "if", word 1 = condition,
               word 2 = body or "then", ... */
            int fi = 2;
            while (fi < p->numWords)
            {
                const char* ws = rw[fi] ? Tcl_GetString(rw[fi]) : "";
                if (strcmp(ws, "then") == 0)
                {
                    fi++;
                    if (fi >= p->numWords)
                        break;
                }
                /* fi is a body position */
                {
                    Tcl_Obj* ifRew = RecurseScriptBody(ctx->ip, twords[fi], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                    if (ifRew)
                    {
                        Tcl_DecrRefCount(rw[fi]);
                        rw[fi] = ifRew;
                        Tcl_IncrRefCount(ifRew);
                        anyMod = 1;
                    }
                }
                fi++; /* past body */
                if (fi >= p->numWords)
                    break;
                ws = rw[fi] ? Tcl_GetString(rw[fi]) : "";
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
                        Tcl_Obj* elRew = RecurseScriptBody(ctx->ip, twords[fi], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                        if (elRew)
                        {
                            Tcl_DecrRefCount(rw[fi]);
                            rw[fi] = elRew;
                            Tcl_IncrRefCount(elRew);
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
                for (int fi2 = 0; fi2 < p->numWords; fi2++)
                {
                    if (rw[fi2])
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(rw[fi2]));
                }
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                *handled = 1;
            }
            for (int fi2 = 0; fi2 < p->numWords; fi2++)
            {
                if (rw[fi2])
                    Tcl_DecrRefCount(rw[fi2]);
            }
            Tcl_Free((char*)rw);
        }
        Tcl_Free((char*)twords);
    }
    if (!*handled && strcmp(c0, "try") == 0 && p->numWords >= 2)
    {
        /* try body ?on/trap code varList body? ?finally body?
           Recurse into ALL body positions — not just the main
           try body — so proc/class definitions in handlers are
           captured and stubbed too. */
        int allLit = 1;
        /* Collect all word tokens */
        const Tcl_Token** twords = (const Tcl_Token**)Tcl_Alloc(sizeof(Tcl_Token*) * (size_t)p->numWords);
        twords[0] = w0;
        for (int fi = 1; fi < p->numWords; fi++)
            twords[fi] = NextWord(twords[fi - 1]);
        /* Check that all words are literals */
        for (int fi = 0; fi < p->numWords; fi++)
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
            Tcl_Obj** rw = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*) * (size_t)p->numWords);
            for (int fi = 0; fi < p->numWords; fi++)
                rw[fi] = WordLiteralObj(twords[fi]);
            /* Identify body positions and try to rewrite them.
               Word 1 is the main try body.  Then walk handlers. */
            {
                Tcl_Obj* tryRew = RecurseScriptBody(ctx->ip, twords[1], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                if (tryRew)
                {
                    Tcl_DecrRefCount(rw[1]);
                    rw[1] = tryRew;
                    Tcl_IncrRefCount(tryRew);
                    anyMod = 1;
                }
            }
            int fi = 2;
            while (fi < p->numWords)
            {
                const char* ks = rw[fi] ? Tcl_GetString(rw[fi]) : "";
                if ((strcmp(ks, "on") == 0 || strcmp(ks, "trap") == 0) && fi + 3 < p->numWords)
                {
                    /* on/trap code varList body */
                    int bodyIdx = fi + 3;
                    Tcl_Obj* hRew = RecurseScriptBody(ctx->ip, twords[bodyIdx], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                    if (hRew)
                    {
                        Tcl_DecrRefCount(rw[bodyIdx]);
                        rw[bodyIdx] = hRew;
                        Tcl_IncrRefCount(hRew);
                        anyMod = 1;
                    }
                    fi = bodyIdx + 1;
                }
                else if (strcmp(ks, "finally") == 0 && fi + 1 < p->numWords)
                {
                    int bodyIdx = fi + 1;
                    Tcl_Obj* fRew = RecurseScriptBody(ctx->ip, twords[bodyIdx], ctx->curNs, ctx->defs, ctx->classes, ctx->depth + 1);
                    if (fRew)
                    {
                        Tcl_DecrRefCount(rw[bodyIdx]);
                        rw[bodyIdx] = fRew;
                        Tcl_IncrRefCount(fRew);
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
                for (int fi2 = 0; fi2 < p->numWords; fi2++)
                {
                    if (rw[fi2])
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(rw[fi2]));
                }
                Tcl_DStringAppend(ctx->out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                Tcl_DStringAppend(ctx->out, "\n", 1);
                Tcl_DStringFree(&ln);
                *handled = 1;
            }
            for (int fi2 = 0; fi2 < p->numWords; fi2++)
            {
                if (rw[fi2])
                    Tcl_DecrRefCount(rw[fi2]);
            }
            Tcl_Free((char*)rw);
        }
        Tcl_Free((char*)twords);
    }

    return *handled;
}

static Tcl_Obj*
CaptureAndRewriteScript(Tcl_Interp* ip, const char* script, Tcl_Size len, Tcl_Obj* curNs, DefVec* defs, ClsSet* classes, int depth)
{
    /* Guard against unbounded recursion from nested namespace eval / control flow */
    if (depth > TBCX_MAX_BLOCK_DEPTH)
        return Tcl_NewStringObj(script, len);

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
                const char* c0 = CmdCore(Tcl_GetString(cmd));

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
                        (void)Tcl_ListObjLength(ip, args, &_d);
                    }
                    if (name && args && body)
                    {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_PROC;
                        r.name = name;
                        r.args = args;
                        r.body = body;
                        r.flags = 0;
                        r.ns = curNs;
                        Tcl_IncrRefCount(name);
                        Tcl_IncrRefCount(args);
                        Tcl_IncrRefCount(body);
                        Tcl_IncrRefCount(r.ns);
                        DV_Push(defs, r);
                        /* Rewrite: emit proc with indexed marker body so that
                           the loader's ProcShim can match by position rather
                           than FQN — correctly handling conflicting proc names
                           across if/else branches.  The marker prefix \x01TBCX
                           cannot collide with any valid Tcl proc body. */
                        {
                            uint32_t procIdx = 0;
                            for (size_t k = 0; k < defs->n; k++)
                                if (defs->v[k].kind == DEF_KIND_PROC)
                                    procIdx++;
                            procIdx--; /* just-pushed entry is 0-based */
                            Tcl_Obj* markerObj = Tcl_ObjPrintf(TBCX_PROC_MARKER_PFX "%u", (unsigned)procIdx);
                            Tcl_IncrRefCount(markerObj);
                            Tcl_DString line;
                            Tcl_DStringInit(&line);
                            Tcl_DStringAppendElement(&line, "proc");
                            Tcl_DStringAppendElement(&line, Tcl_GetString(name));
                            Tcl_DStringAppendElement(&line, Tcl_GetString(args));
                            Tcl_DStringAppendElement(&line, Tcl_GetString(markerObj));
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
                    if (sub && strcmp(Tcl_GetString(sub), "eval") == 0)
                    {
                        const Tcl_Token* w2 = NextWord(w1);
                        const Tcl_Token* w3 = NextWord(w2);
                        Tcl_Obj* nsObj = WordLiteralObj(w2);
                        Tcl_Obj* bodyObj = WordLiteralObj(w3);
                        if (nsObj && bodyObj)
                        {
                            Tcl_Obj* nsFqn = FqnUnder(curNs, nsObj);
                            /* Recurse: single-pass capture+rewrite of inner body */
                            Tcl_Size bodyLen = 0;
                            const char* bodyStr = Tcl_GetStringFromObj(bodyObj, &bodyLen);
                            Tcl_Obj* rewritten = CaptureAndRewriteScript(ip, bodyStr, bodyLen, nsFqn, defs, classes, depth + 1);
                            Tcl_Obj* canonBody = CanonTrivia(rewritten);
                            Tcl_DString cmdLn;
                            Tcl_DStringInit(&cmdLn);
                            Tcl_DStringAppendElement(&cmdLn, "::tcl::namespace::eval");
                            Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(nsObj));
                            Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(canonBody));
                            Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                            Tcl_DStringAppend(&out, "\n", 1);
                            Tcl_DStringFree(&cmdLn);
                            Tcl_DecrRefCount(canonBody);
                            Tcl_DecrRefCount(rewritten);
                            Tcl_DecrRefCount(nsFqn);
                            Tcl_DecrRefCount(bodyObj);
                            Tcl_DecrRefCount(nsObj);
                            handled = 1;
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
                    RewriteCtx rctx = { ip, &out, curNs, defs, classes, depth };
                    RewriteOoCmd(&rctx, &p, w0, c0, &handled);
                }

                /* ---- Control flow (while, for, foreach, catch, if, try, switch) ---- */
                if (!handled)
                {
                    RewriteCtx rctx = { ip, &out, curNs, defs, classes, depth };
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

    DefVec defs;
    DV_Init(&defs);
    ClsSet classes;
    CS_Init(&classes);
    Tcl_Obj* rootNs = Tcl_NewStringObj("::", -1);
    Tcl_IncrRefCount(rootNs);
    Tcl_Size srcLen = 0;
    const char* srcStr = Tcl_GetStringFromObj(scriptObj, &srcLen);
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
            srcCopy = rew;
            Tcl_IncrRefCount(srcCopy);
        }
    }

    /* Build strip-bodies set from captured definitions — used by WriteLiteral
       to avoid serializing proc bodies as nested bytecode in the top-level block. */
    for (size_t i = 0; i < defs.n; i++)
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
    ByteCodeGetInternalRep(srcCopy, tbcxTyBytecode, top);
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
        const char* rewStr = Tcl_GetStringFromObj(srcCopy, &rewLen);
        ScanForNsEvalBodies(&ctx, rewStr, rewLen);
    }
    /* Also scan captured proc/method/class bodies for script-body
       patterns (uplevel, eval, try, etc.) that produce string literals
       in separately compiled bytecodes.  The rewritten script has these
       bodies stubbed, so ScanForNsEvalBodies above cannot see them.
       WriteLiteral will check nsEvalBodies when serializing nested
       blocks and precompile matching strings on the fly. */
    for (size_t i = 0; i < defs.n; i++)
    {
        if (defs.v[i].body)
        {
            Tcl_Size bLen = 0;
            const char* bStr = Tcl_GetStringFromObj(defs.v[i].body, &bLen);
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
    for (size_t i = 0; i < defs.n; i++)
    {
        if (defs.v[i].kind == DEF_KIND_PROC)
            numProcs++;
    }
    W_U32(w, numProcs);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind == DEF_KIND_PROC)
        {
            /* Serialize FQN/name, ns, args */
            Tcl_Size ln;
            const char* s;
            s = Tcl_GetStringFromObj(defs.v[i].name, &ln);
            W_LPString(w, s, ln);
            s = Tcl_GetStringFromObj(defs.v[i].ns, &ln);
            W_LPString(w, s, ln);
            s = Tcl_GetStringFromObj(defs.v[i].args, &ln);
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
            const char** keys = (const char**)Tcl_Alloc(sizeof(const char*) * numClasses);
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
    }

    /* 7. Methods section: emit captured OO methods/ctors/dtors */
    uint32_t numMethods = 0;
    for (size_t i = 0; i < defs.n; i++)
    {
        if (defs.v[i].kind != DEF_KIND_PROC)
            numMethods++;
    }
    W_U32(w, numMethods);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind != DEF_KIND_PROC)
        {
            Tcl_Size ln;
            const char* s;
            /* classFqn */
            s = Tcl_GetStringFromObj(defs.v[i].cls, &ln);
            W_LPString(w, s, ln);
            /* wire kind 0..3 expected by loader (inst=0, class=1, ctor=2, dtor=3) */
            W_U8(w, (uint8_t)(defs.v[i].kind - DEF_KIND_INST));
            /* name (empty for ctor/dtor) */
            if (defs.v[i].kind == DEF_KIND_CTOR || defs.v[i].kind == DEF_KIND_DTOR)
            {
                W_LPString(w, "", 0);
            }
            else
            {
                s = Tcl_GetStringFromObj(defs.v[i].name, &ln);
                W_LPString(w, s, ln);
            }
            /* args */
            s = Tcl_GetStringFromObj(defs.v[i].args, &ln);
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
            Tcl_Seek(ch, curPos, SEEK_SET); /* restore position */
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
    const char* name = Tcl_GetString(obj);
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
 * Tcl commands
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
        Tcl_AppendToObj(tmpPath, ".tbcx.tmp", -1);
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
            if (outNorm && Tcl_FSRenameFile(tmpPath, outNorm) == TCL_OK)
            {
                Tcl_SetObjResult(interp, outNorm);
            }
            else
            {
                /* Rename failed — clean up temp and report */
                Tcl_FSDeleteFile(tmpPath);
                {
                    Tcl_Size errLen = 0;
                    (void)Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &errLen);
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
