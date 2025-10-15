/* ==========================================================================
 * tbcxsave.c — TBCX compile+save for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct {
    Tcl_Interp *interp;
    Tcl_Channel chan;
    int         err; /* TCL_OK or TCL_ERROR */
} TbcxOut;

typedef struct TbcxCtx {
    Tcl_Interp   *interp;
    Tcl_HashTable stripBodies; /* set of body strings to strip (by exact bytes) */
    int           stripInit;
    int           stripActive; /* only active when writing top-level block */
} TbcxCtx;

typedef struct {
    const char *key;
    int         targetOffset;
} JTEntry;

typedef struct {
    Tcl_WideInt key;
    int         targetOffset;
} JTNumEntry;

typedef struct {
    Tcl_Obj *name; /* FQN or as-is */
    Tcl_Obj *ns;   /* NS for emission (string) */
    Tcl_Obj *args;
    Tcl_Obj *body;
    int      kind;  /* 0=proc; 1=inst-meth; 2=class-meth; 3=ctor; 4=dtor (methods stored in “methods” section) */
    Tcl_Obj *cls;   /* class name if method */
    int      flags; /* bitmask: 1 = captured from oo::class builder */
} DefRec;

typedef struct {
    DefRec *v;
    size_t  n, cap;
} DefVec;

/* Class set (unique FQNs). We store stable UTF-8 keys; value is unused. */
typedef struct {
    Tcl_HashTable ht; /* TCL_STRING_KEYS; key is stable char* we own */
    int           init;
} ClsSet;

#define DEF_F_FROM_BUILDER 0x01
#define DEF_KIND_PROC 0
#define DEF_KIND_INST 1
#define DEF_KIND_CLASS 2
#define DEF_KIND_CTOR 3
#define DEF_KIND_DTOR 4

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

int                            BuildLocals(Tcl_Interp *ip, Tcl_Obj *argsList, CompiledLocal **firstOut, CompiledLocal **lastOut, int *numArgsOut);
static Tcl_Obj                *CanonTrivia(Tcl_Obj *in);
static void                    CaptureClassBody(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, Tcl_Obj *clsFqn, DefVec *defs, ClsSet *classes, int flags);
static void                    CaptureStaticsRec(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, DefVec *defs, ClsSet *classes, int inBuilder);
int                            CheckBinaryChan(Tcl_Interp *ip, Tcl_Channel ch);
static inline const char      *CmdCore(const char *s);
static int                     CmpJTEntryUtf8_qsort(const void *pa, const void *pb);
static int                     CmpTclObjUtf8_qsort(const void *pa, const void *pb);
static int                     CompileProcLike(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *nsFQN, Tcl_Obj *argsList, Tcl_Obj *bodyObj, const char *whereTag);
static uint32_t                ComputeNumLocals(ByteCode *bc);
static void                    CS_Add(ClsSet *cs, Tcl_Obj *clsFqn);
static void                    CS_Free(ClsSet *cs);
static void                    CS_Init(ClsSet *cs);
static void                    CtxAddStripBody(TbcxCtx *ctx, Tcl_Obj *body);
static void                    CtxFreeStripBodies(TbcxCtx *ctx);
static void                    CtxInitStripBodies(TbcxCtx *ctx);
static void                    DV_Free(DefVec *dv);
static void                    DV_Init(DefVec *dv);
static void                    DV_Push(DefVec *dv, DefRec r);
static int                     EmitTbcxStream(Tcl_Interp *ip, Tcl_Obj *scriptObj, TbcxOut *w);
static Tcl_Obj                *FqnUnder(Tcl_Obj *curNs, Tcl_Obj *name);
void                           FreeLocals(CompiledLocal *first);
static int                     IsPureOodefineBuilderBody(Tcl_Interp *ip, const char *script, Tcl_Size len);
static void                    Lit_Bignum(TbcxOut *w, Tcl_Obj *o);
static void                    Lit_Bytecode(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *bcObj);
static void                    Lit_Dict(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o);
static void                    Lit_LambdaBC(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *lambda);
static void                    Lit_List(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o);
static inline const Tcl_Token *NextWord(const Tcl_Token *wordTok);
static Tcl_Obj                *NsFqn(Tcl_Interp *ip, Tcl_Namespace *nsPtr);
static Tcl_Obj                *ResolveToBytecodeObj(TbcxCtx *ctx, Tcl_Obj *cand);
static Tcl_Obj                *RewriteScript(Tcl_Interp *ip, const char *script, Tcl_Size len, DefVec *defs, Tcl_Obj *curNs);
static int                     ShouldStripBody(TbcxCtx *ctx, Tcl_Obj *obj);
static Tcl_Obj                *StubbedBuilderBody(Tcl_Interp *ip, Tcl_Obj *bodyObj);
static void                    StubLinesForClass(Tcl_DString *out, DefVec *defs, Tcl_Obj *clsFqn);
int                            Tbcx_SaveChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
int                            Tbcx_SaveFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
int                            Tbcx_SaveObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
static inline void             W_Bytes(TbcxOut *w, const void *p, size_t n);
static inline void             W_Error(TbcxOut *w, const char *msg);
static inline void             W_LPString(TbcxOut *w, const char *s, Tcl_Size n);
static inline void             W_U32(TbcxOut *w, uint32_t v);
static inline void             W_U64(TbcxOut *w, uint64_t v);
static inline void             W_U8(TbcxOut *w, uint8_t v);
static Tcl_Obj                *WordLiteralObj(Tcl_Interp *ip, const Tcl_Token *wordTok);
static void                    WriteAux_DictUpdate(TbcxOut *w, AuxData *ad);
static void                    WriteAux_Foreach(TbcxOut *w, AuxData *ad);
static void                    WriteAux_JTNum(TbcxOut *w, AuxData *ad);
static void                    WriteAux_JTStr(TbcxOut *w, AuxData *ad);
static void                    WriteCompiledBlock(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *bcObj);
static void                    WriteHeaderTop(TbcxOut *w, ByteCode *top);
static void                    WriteLiteral(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *obj);

/* ==========================================================================
 * Stuff
 * ========================================================================== */

static int                     CmpTclObjUtf8_qsort(const void *pa, const void *pb) {
    const Tcl_Obj *const *oa = (const Tcl_Obj *const *)pa;
    const Tcl_Obj *const *ob = (const Tcl_Obj *const *)pb;
    Tcl_Size              la = 0, lb = 0;
    const char           *a   = Tcl_GetStringFromObj((Tcl_Obj *)*oa, &la);
    const char           *b   = Tcl_GetStringFromObj((Tcl_Obj *)*ob, &lb);
    int                   cmp = Tcl_UtfNcmp(a, b, (la < lb ? la : lb));
    if (cmp != 0)
        return cmp;
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

static int CmpJTEntryUtf8_qsort(const void *pa, const void *pb) {
    const JTEntry *a  = (const JTEntry *)pa;
    const JTEntry *b  = (const JTEntry *)pb;
    const char    *sa = a->key ? a->key : "";
    const char    *sb = b->key ? b->key : "";
    return strcmp(sa, sb);
}

static inline void W_Error(TbcxOut *w, const char *msg) {
    if (w->err == TCL_OK) {
        Tcl_SetObjResult(w->interp, Tcl_NewStringObj(msg, -1));
        w->err = TCL_ERROR;
    }
}

static inline void W_Bytes(TbcxOut *w, const void *p, size_t n) {
    if (w->err)
        return;
    if (Tcl_WriteRaw(w->chan, (const char *)p, n) != (int)n) {
        W_Error(w, "tbcx: short write");
        return;
    }
}

static inline void W_U8(TbcxOut *w, uint8_t v) {
    W_Bytes(w, &v, 1);
}

static inline void W_U32(TbcxOut *w, uint32_t v) {
    if (!tbcxHostIsLE) {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    W_Bytes(w, &v, 4);
}
static inline void W_U64(TbcxOut *w, uint64_t v) {
    if (!tbcxHostIsLE) {
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) |
            ((v & 0x0000FF0000000000ull) >> 24) | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }
    W_Bytes(w, &v, 8);
}

static inline void W_LPString(TbcxOut *w, const char *s, Tcl_Size n) {
    if (n < 0)
        n = (Tcl_Size)strlen(s);
    if ((uint64_t)n > TBCX_MAX_STR) {
        W_Error(w, "tbcx: string too large");
        return;
    }
    W_U32(w, (uint32_t)n);
    W_Bytes(w, s, (size_t)n);
}

static void CtxInitStripBodies(TbcxCtx *ctx) {
    if (!ctx)
        return;
    Tcl_InitHashTable(&ctx->stripBodies, TCL_STRING_KEYS);
    ctx->stripInit   = 1;
    ctx->stripActive = 0;
}

static void CtxAddStripBody(TbcxCtx *ctx, Tcl_Obj *body) {
    if (!ctx || !ctx->stripInit || !body)
        return;
    Tcl_Size       len   = 0;
    const char    *s     = Tcl_GetStringFromObj(body, &len);
    int            isNew = 0;
    /* With TCL_STRING_KEYS tables, Tcl copies the key; we must not free it ourselves. */
    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&ctx->stripBodies, (const char *)s, &isNew);
    if (isNew) {
        Tcl_SetHashValue(he, (ClientData)(uintptr_t)len);
    }
}

static int ShouldStripBody(TbcxCtx *ctx, Tcl_Obj *obj) {
    if (!ctx || !ctx->stripInit || !ctx->stripActive || !obj)
        return 0;
    Tcl_Size       len = 0;
    const char    *s   = Tcl_GetStringFromObj(obj, &len);
    Tcl_HashEntry *he  = Tcl_FindHashEntry(&ctx->stripBodies, (const char *)s);
    if (!he)
        return 0;
    uintptr_t storedLen = (uintptr_t)Tcl_GetHashValue(he);
    return (storedLen == (uintptr_t)len) ? 1 : 0;
}

static void CtxFreeStripBodies(TbcxCtx *ctx) {
    if (!ctx || !ctx->stripInit)
        return;
    Tcl_DeleteHashTable(&ctx->stripBodies);
    ctx->stripInit   = 0;
    ctx->stripActive = 0;
}

/* Obtain FQN for a namespace object; fallback "::" */
static Tcl_Obj *NsFqn(Tcl_Interp *ip, Tcl_Namespace *nsPtr) {
    if (!nsPtr)
        return Tcl_NewStringObj("::", -1);
    Tcl_Obj *o = Tcl_NewStringObj(nsPtr->fullName, -1);
    return o;
}

/* --- Literal emission (R1 encodings; §8) ---------------------------------- */

static void Lit_Bignum(TbcxOut *w, Tcl_Obj *o) {
    mp_int z;
    if (Tcl_GetBignumFromObj(NULL, o, &z) != TCL_OK) {
        W_Error(w, "tbcx: bad bignum");
        return;
    }
    /* Export absolute value magnitude as base-256 little-endian bytes */
    mp_int mag;
    mp_err mrc;
    mrc = TclBN_mp_init(&mag);
    if (mrc != MP_OKAY) {
        W_Error(w, "tbcx: bignum init");
        TclBN_mp_clear(&z);
        return;
    }
    mrc = mp_copy(&z, &mag);
    if (mrc != MP_OKAY) {
        W_Error(w, "tbcx: bignum copy");
        mp_clear(&mag);
        mp_clear(&z);
        return;
    }
    if (mp_isneg(&mag)) {
        mrc = mp_neg(&mag, &mag);
        if (mrc != MP_OKAY) {
            W_Error(w, "tbcx: bignum abs/neg");
            mp_clear(&mag);
            mp_clear(&z);
            return;
        }
    }
    /* Use TomMath's unsigned-binary (big-endian) export, then reverse to LE. */
    size_t         be_bytes = TclBN_mp_ubin_size(&mag);
    unsigned char *be       = NULL;
    if (be_bytes > 0) {
        be = (unsigned char *)Tcl_Alloc(be_bytes);
        if (!be) {
            W_Error(w, "tbcx: oom");
            TclBN_mp_clear(&mag);
            TclBN_mp_clear(&z);
            return;
        }
        mrc = TclBN_mp_to_ubin(&mag, be, be_bytes, NULL);
        if (mrc != MP_OKAY) {
            W_Error(w, "tbcx: bignum export");
            Tcl_Free((char *)be);
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
    if (magLen == 0) { /* zero */
        W_U8(w, 0);
        W_U32(w, 0);
    } else {
        int sign = mp_isneg(&z) ? 2 : 1;
        W_U8(w, (uint8_t)sign);
        W_U32(w, (uint32_t)magLen);
        /* write little-endian bytes */
        for (size_t i = 0; i < magLen; i++) {
            W_U8(w, be[be_bytes - 1 - i]);
        }
    }
    if (be)
        Tcl_Free((char *)be);
    TclBN_mp_clear(&mag);
    TclBN_mp_clear(&z);
}

static void Lit_List(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o) {
    Tcl_Size  n = 0;
    Tcl_Obj **v = NULL;
    if (Tcl_ListObjGetElements(NULL, o, &n, &v) != TCL_OK) {
        W_Error(w, "tbcx: list decode");
        return;
    }
    W_U32(w, (uint32_t)n);
    for (Tcl_Size i = 0; i < n; i++)
        WriteLiteral(w, ctx, v[i]);
}

static void Lit_Dict(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o) {
    Tcl_DictSearch s;
    int            done = 0;
    Tcl_Obj       *k, *v;
    Tcl_Size       sz = 0;
    if (Tcl_DictObjSize(NULL, o, &sz) != TCL_OK) {
        W_Error(w, "tbcx: dict decode");
        return;
    }
    /* collect and sort keys by UTF-8 bytes for deterministic output */
    Tcl_Obj **keys = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)sz);
    Tcl_Size  idx  = 0;
    if (Tcl_DictObjFirst(NULL, o, &s, &k, &v, &done) != TCL_OK) {
        Tcl_Free((char *)keys);
        W_Error(w, "tbcx: dict iter");
        return;
    }
    while (!done) {
        keys[idx++] = k;
        Tcl_DictObjNext(&s, &k, &v, &done);
    }
    /* deterministic UTF-8 sort */
    if (idx > 1) {
        qsort(keys, (size_t)idx, sizeof(Tcl_Obj *), CmpTclObjUtf8_qsort);
    }
    W_U32(w, (uint32_t)idx);
    for (Tcl_Size i = 0; i < idx; i++) {
        Tcl_Obj *val = NULL;
        if (Tcl_DictObjGet(NULL, o, keys[i], &val) != TCL_OK) {
            W_Error(w, "tbcx: dict get");
            break;
        }
        WriteLiteral(w, ctx, keys[i]);
        WriteLiteral(w, ctx, val ? val : Tcl_NewObj());
    }
    Tcl_Free((char *)keys);
}

static void Lit_LambdaBC(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *lambda) {
    /* Public parse of lambda list: {args body ?ns?} */
    Tcl_Size  L = 0;
    Tcl_Obj **E = NULL;
    if (Tcl_ListObjGetElements(ctx->interp, lambda, &L, &E) != TCL_OK || (L != 2 && L != 3)) {
        W_Error(w, "tbcx: lambda must be a list of 2 or 3 elements");
        return;
    }
    Tcl_Obj       *argsList = E[0];
    Tcl_Obj       *bodyObj  = E[1];
    Tcl_Obj       *nsObjIn  = (L == 3 ? E[2] : NULL);

    /* Resolve namespace (public): use provided FQN if present, else current */
    Tcl_Namespace *nsPtr    = NULL;
    Tcl_Obj       *nsFQN    = NULL;
    if (nsObjIn) {
        const char *nsName = Tcl_GetString(nsObjIn);
        nsPtr              = Tcl_FindNamespace(ctx->interp, nsName, NULL, 0);
        if (!nsPtr) {
            nsPtr = Tcl_CreateNamespace(ctx->interp, nsName, NULL, NULL);
        }
        nsFQN = Tcl_NewStringObj(nsName, -1);
    } else {
        nsPtr = Tcl_GetCurrentNamespace(ctx->interp);
        nsFQN = NsFqn(ctx->interp, nsPtr);
    }
    Tcl_IncrRefCount(nsFQN);
    int         compiled_ok = 0;
    Tcl_Size    nsLen       = 0;
    const char *nsStr       = Tcl_GetStringFromObj(nsFQN, &nsLen);
    W_LPString(w, nsStr, nsLen);

    /* Marshal args & defaults from the public args list */
    Tcl_Size  argc = 0;
    Tcl_Obj **argv = NULL;
    if (Tcl_ListObjGetElements(ctx->interp, argsList, &argc, &argv) != TCL_OK) {
        Tcl_DecrRefCount(nsFQN);
        W_Error(w, "tbcx: bad lambda args list");
        return;
    }
    W_U32(w, (uint32_t)argc);
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nf = 0;
        Tcl_Obj **fv = NULL;
        if (Tcl_ListObjGetElements(ctx->interp, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2) {
            Tcl_DecrRefCount(nsFQN);
            W_Error(w, "tbcx: bad lambda arg spec");
            return;
        }
        Tcl_Size    nmLen = 0;
        const char *nm    = Tcl_GetStringFromObj(fv[0], &nmLen);
        W_LPString(w, nm, nmLen);
        if (nf == 2) {
            W_U8(w, 1);
            WriteLiteral(w, ctx, fv[1]);
        } else {
            W_U8(w, 0);
        }
    }

    /* Compile body with proc semantics (internal via stubs), using a temp Proc
       whose locals match the args list — same shape your loader builds. */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr              = (Interp *)ctx->interp;
    procPtr->refCount          = 1;
    procPtr->numArgs           = (int)argc;
    procPtr->numCompiledLocals = (int)argc;
    /* Build compiled locals from the public args list */
    {
        CompiledLocal *first = NULL, *last = NULL;
        int            numA = 0;
        if (BuildLocals(ctx->interp, argsList, &first, &last, &numA) != TCL_OK) {
            W_Error(w, "tbcx: lambda args decode");
            goto lambda_cleanup;
        }
        procPtr->numArgs           = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr     = first;
        procPtr->lastLocalPtr      = last;
    }

    Tcl_IncrRefCount(bodyObj);
    if (TclProcCompileProc(ctx->interp, procPtr, bodyObj, (Namespace *)nsPtr, "body of lambda term", "lambdaExpr") != TCL_OK) {
        Tcl_DecrRefCount(bodyObj);
        W_Error(w, "tbcx: lambda compile");
        goto lambda_cleanup;
    }
    compiled_ok = 1; /* compiler succeeded; it may have converted bodyObj to 'procbody' */
    /* Resolve to the actual bytecode object (unwrap 'procbody' as needed) */
    {
        Tcl_Obj *cand  = procPtr->bodyPtr ? procPtr->bodyPtr : bodyObj;
        Tcl_Obj *bcObj = ResolveToBytecodeObj(ctx, cand);
        if (!bcObj) {
            W_Error(w, "tbcx: proc-like compile produced no bytecode");
        } else {
            WriteCompiledBlock(w, ctx, bcObj);
        }
    }
    Tcl_DecrRefCount(bodyObj);

lambda_cleanup:
    /* Drop temp Proc + locals only if the compiler did not take ownership. */
    if (!compiled_ok) {
        FreeLocals(procPtr->firstLocalPtr);
        Tcl_Free((char *)procPtr);
    }

    Tcl_DecrRefCount(nsFQN);
}

static void Lit_Bytecode(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *bcObj) {
    /* nsFqn from ByteCode */
    ByteCode *codePtr = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, codePtr);
    Tcl_Obj *nsFQN = NsFqn(ctx->interp, (Tcl_Namespace *)(codePtr ? codePtr->nsPtr : NULL));
    Tcl_IncrRefCount(nsFQN);
    Tcl_Size    nsLen;
    const char *nsStr = Tcl_GetStringFromObj(nsFQN, &nsLen);
    W_LPString(w, nsStr, nsLen);
    WriteCompiledBlock(w, ctx, bcObj);
    Tcl_DecrRefCount(nsFQN);
}

int BuildLocals(Tcl_Interp *ip, Tcl_Obj *argsList, CompiledLocal **firstOut, CompiledLocal **lastOut, int *numArgsOut) {
    if (!firstOut || !lastOut || !numArgsOut)
        return TCL_ERROR;
    *firstOut = *lastOut = NULL;
    *numArgsOut          = 0;

    Tcl_Size  argc       = 0;
    Tcl_Obj **argv       = NULL;
    if (Tcl_ListObjGetElements(ip, argsList, &argc, &argv) != TCL_OK) {
        return TCL_ERROR;
    }

    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nf = 0, nmLen = 0;
        Tcl_Obj **fv = NULL;
        if (Tcl_ListObjGetElements(ip, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2) {
            /* cleanup partial chain */
            FreeLocals(first);
            return TCL_ERROR;
        }
        const char    *nm = Tcl_GetStringFromObj(fv[0], &nmLen);
        CompiledLocal *cl = (CompiledLocal *)Tcl_Alloc(offsetof(CompiledLocal, name) + 1u + (size_t)nmLen);
        memset(cl, 0, sizeof(CompiledLocal));
        cl->nameLength = (int)nmLen;
        memcpy(cl->name, nm, (size_t)nmLen + 1);
        cl->frameIndex = (int)i;
        cl->flags      = VAR_ARGUMENT;
        if (nf == 2) {
            cl->defValuePtr = fv[1];
            Tcl_IncrRefCount(cl->defValuePtr);
        }
        if (i == argc - 1 && nmLen == 4 && memcmp(nm, "args", 4) == 0)
            cl->flags |= VAR_IS_ARGS;
        if (!first)
            first = last = cl;
        else {
            last->nextPtr = cl;
            last          = cl;
        }
    }

    *firstOut   = first;
    *lastOut    = last;
    *numArgsOut = (int)argc;
    return TCL_OK;
}

void FreeLocals(CompiledLocal *first) {
    for (CompiledLocal *cl = first; cl;) {
        CompiledLocal *next = cl->nextPtr;
        if (cl->defValuePtr)
            Tcl_DecrRefCount(cl->defValuePtr);
        Tcl_Free((char *)cl);
        cl = next;
    }
}

static int CompileProcLike(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *nsFQN, Tcl_Obj *argsList, Tcl_Obj *bodyObj, const char *whereTag) {
    Tcl_Interp    *ip   = ctx->interp;
    const char    *nsNm = Tcl_GetString(nsFQN);
    Tcl_Namespace *ns   = Tcl_FindNamespace(ip, nsNm, NULL, 0);
    if (!ns)
        ns = Tcl_CreateNamespace(ip, nsNm, NULL, NULL);
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    /* Build compiled locals from the public args list */
    {
        CompiledLocal *first = NULL, *last = NULL;
        int            numA = 0;
        if (BuildLocals(ip, argsList, &first, &last, &numA) != TCL_OK) {
            W_Error(w, "tbcx: bad arg spec");
            goto cple_fail;
        }
        procPtr->numArgs           = numA;
        procPtr->numCompiledLocals = numA;
        procPtr->firstLocalPtr     = first;
        procPtr->lastLocalPtr      = last;
    }
    Tcl_IncrRefCount(bodyObj);
    if (TclProcCompileProc(ip, procPtr, bodyObj, (Namespace *)ns, whereTag, "proc") != TCL_OK) {
        Tcl_DecrRefCount(bodyObj);
        W_Error(w, "tbcx: proc-like compile failed");
        goto cple_fail;
    }
    /* Compiler may leave bytecode in procPtr->bodyPtr and/or convert bodyObj to 'procbody'. */
    {
        Tcl_Obj *cand  = procPtr->bodyPtr ? procPtr->bodyPtr : bodyObj;
        Tcl_Obj *bcObj = ResolveToBytecodeObj(ctx, cand);
        if (!bcObj) {
            Tcl_DecrRefCount(bodyObj);
            W_Error(w, "tbcx: proc-like compile produced no bytecode");
            goto cple_fail;
        }
        WriteCompiledBlock(w, ctx, bcObj);
    }
    Tcl_DecrRefCount(bodyObj);
    /* Success: the Tcl compiler takes ownership of procPtr/locals in 9.1. */
    return (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

    /* cleanup locals/proc (failure path only) */
    FreeLocals(procPtr->firstLocalPtr);

    Tcl_Free((char *)procPtr);
    return (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

cple_fail:
    FreeLocals(procPtr->firstLocalPtr);

    Tcl_Free((char *)procPtr);
    return TCL_ERROR;
}

static void WriteLiteral(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *obj) {
    const Tcl_ObjType *ty = obj->typePtr;
    if (ty == tbcxTyBignum) {
        W_U32(w, TBCX_LIT_BIGNUM);
        Lit_Bignum(w, obj);
    } else if (ty == tbcxTyBoolean) {
        int b = 0;
        Tcl_GetBooleanFromObj(NULL, obj, &b);
        W_U32(w, TBCX_LIT_BOOLEAN);
        W_U8(w, (uint8_t)(b != 0));
    } else if (ty == tbcxTyByteArray) {
        Tcl_Size       n;
        unsigned char *p = Tcl_GetByteArrayFromObj(obj, &n);
        W_U32(w, TBCX_LIT_BYTEARR);
        W_U32(w, (uint32_t)n);
        W_Bytes(w, p, (size_t)n);
    } else if (ty == tbcxTyDict) {
        W_U32(w, TBCX_LIT_DICT);
        Lit_Dict(w, ctx, obj);
    } else if (ty == tbcxTyDouble) {
        double d = 0;
        Tcl_GetDoubleFromObj(NULL, obj, &d);
        union {
            double   d;
            uint64_t u;
        } u;
        u.d = d;
        W_U32(w, TBCX_LIT_DOUBLE);
        W_U64(w, u.u);
    } else if (ty == tbcxTyList) {
        W_U32(w, TBCX_LIT_LIST);
        Lit_List(w, ctx, obj);
    } else if (ty == tbcxTyBytecode) {
        if (ctx && ctx->stripActive) {
            Tcl_Size    n = 0;
            const char *s = Tcl_GetStringFromObj(obj, &n);
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, s, n);
        } else {
            W_U32(w, TBCX_LIT_BYTECODE);
            Lit_Bytecode(w, ctx, obj);
        }
    } else if (obj->typePtr == tbcxTyProcBody) {
        /* Guard: for top-level, don't leak proc bodies as raw bytecode; bodies
           are serialized in Procs/Methods sections and source is stripped. */
        if (ctx && ctx->stripActive) {
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        } else {
            Proc *p = (Proc *)obj->internalRep.twoPtrValue.ptr1;
            if (p && p->bodyPtr) {
                ByteCode *bc = NULL;
                ByteCodeGetInternalRep(p->bodyPtr, tbcxTyBytecode, bc);
                if (bc) {
                    W_U32(w, TBCX_LIT_BYTECODE);
                    Lit_Bytecode(w, ctx, p->bodyPtr);
                    return;
                }
            }
            W_U32(w, TBCX_LIT_STRING);
            W_LPString(w, "", 0);
        }
    } else if (tbcxTyLambda != NULL && ty == tbcxTyLambda) {
        W_U32(w, TBCX_LIT_LAMBDA_BC);
        Lit_LambdaBC(w, ctx, obj);
    } else if (ty == tbcxTyInt) {
        Tcl_WideInt wv = 0;
        Tcl_GetWideIntFromObj(NULL, obj, &wv);
        if (wv >= 0) {
            W_U32(w, TBCX_LIT_WIDEUINT);
            W_U64(w, (uint64_t)wv);
        } else {
            W_U32(w, TBCX_LIT_WIDEINT);
            W_U64(w, (uint64_t)wv);
        } /* two's complement */
    } else {
        /* Default: emit as STRING using current UTF-8 rep. Optionally strip known proc/method bodies. */
        Tcl_Size    n;
        const char *s = Tcl_GetStringFromObj(obj, &n);
        W_U32(w, TBCX_LIT_STRING);
        if (ShouldStripBody(ctx, obj)) {
            W_LPString(w, "", 0);
        } else {
            W_LPString(w, s, n);
        }
    }
}

static void WriteAux_JTStr(TbcxOut *w, AuxData *ad) {
    JumptableInfo *info = (JumptableInfo *)ad->clientData;
    if (!info) {
        W_U32(w, 0);
        return;
    }
    Tcl_HashSearch srch;
    Tcl_HashEntry *h;
    /* Count entries */
    uint32_t       cnt = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
        cnt++;
    /* Collect into array for sorting */
    JTEntry *arr = (JTEntry *)Tcl_Alloc(sizeof(JTEntry) * cnt);
    uint32_t i   = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch)) {
        arr[i].key          = (const char *)Tcl_GetHashKey(&info->hashTable, h);
        arr[i].targetOffset = PTR2INT(Tcl_GetHashValue(h));
        i++;
    }
    if (cnt > 1) {
        qsort(arr, (size_t)cnt, sizeof(JTEntry), CmpJTEntryUtf8_qsort);
    }
    W_U32(w, cnt);
    for (uint32_t k = 0; k < cnt; k++) {
        const char *s = arr[k].key ? arr[k].key : "";
        W_LPString(w, s, (Tcl_Size)strlen(s));
        W_U32(w, (uint32_t)arr[k].targetOffset);
    }
    Tcl_Free((char *)arr);
}

static void WriteAux_JTNum(TbcxOut *w, AuxData *ad) {
    JumptableNumInfo *info = (JumptableNumInfo *)ad->clientData;
    if (!info) {
        W_U32(w, 0);
        return;
    }
    Tcl_HashSearch srch;
    Tcl_HashEntry *h;
    uint32_t       cnt = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch))
        cnt++;
    JTNumEntry *arr = (JTNumEntry *)Tcl_Alloc(sizeof(JTNumEntry) * cnt);
    uint32_t    i   = 0;
    for (h = Tcl_FirstHashEntry(&info->hashTable, &srch); h; h = Tcl_NextHashEntry(&srch)) {
        arr[i].key          = (Tcl_WideInt)(intptr_t)Tcl_GetHashKey(&info->hashTable, h);
        arr[i].targetOffset = PTR2INT(Tcl_GetHashValue(h));
        i++;
    }
    /* sort numeric ascending */
    for (uint32_t a = 1; a < cnt; a++) {
        JTNumEntry t = arr[a];
        uint32_t   b = a;
        while (b > 0 && arr[b - 1].key > t.key) {
            arr[b] = arr[b - 1];
            b--;
        }
        arr[b] = t;
    }
    W_U32(w, cnt);
    for (uint32_t k = 0; k < cnt; k++) {
        W_U64(w, (uint64_t)arr[k].key);
        W_U32(w, (uint32_t)arr[k].targetOffset);
    }
    Tcl_Free((char *)arr);
}

static void WriteAux_DictUpdate(TbcxOut *w, AuxData *ad) {
    DictUpdateInfo *info = (DictUpdateInfo *)ad->clientData;
    if (!info) {
        W_U32(w, 0);
        return;
    }
    W_U32(w, (uint32_t)info->length);
    for (Tcl_Size i = 0; i < info->length; i++)
        W_U32(w, (uint32_t)info->varIndices[i]);
}

static void WriteAux_Foreach(TbcxOut *w, AuxData *ad) {
    ForeachInfo *info     = (ForeachInfo *)ad->clientData;
    Tcl_Size     numLists = info ? info->numLists : 0;
    W_U32(w, (uint32_t)numLists);
    W_U32(w, (uint32_t)(info ? info->loopCtTemp : 0));
    W_U32(w, (uint32_t)(info ? info->firstValueTemp : 0));
    W_U32(w, (uint32_t)numLists);
    for (Tcl_Size i = 0; i < numLists; i++) {
        ForeachVarList *vl = info->varLists[i];
        Tcl_Size        nv = vl ? vl->numVars : 0;
        W_U32(w, (uint32_t)nv);
        for (Tcl_Size j = 0; j < nv; j++) {
            W_U32(w, (uint32_t)vl->varIndexes[j]);
        }
    }
}

static uint32_t ComputeNumLocals(ByteCode *bc) {
    Tcl_Size maxIdx = 0;
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; i++) {
        AuxData *ad = &bc->auxDataArrayPtr[i];
        if (ad->type == tbcxAuxDictUpdate) {
            DictUpdateInfo *info = (DictUpdateInfo *)ad->clientData;
            if (info) {
                for (Tcl_Size k = 0; k < info->length; k++) {
                    if (info->varIndices[k] > maxIdx)
                        maxIdx = info->varIndices[k];
                }
            }
        } else if (ad->type == tbcxAuxForeach || ad->type == tbcxAuxNewForeach) {
            ForeachInfo *info = (ForeachInfo *)ad->clientData;
            if (info) {
                if (info->firstValueTemp > maxIdx)
                    maxIdx = info->firstValueTemp;
                if (info->loopCtTemp > maxIdx)
                    maxIdx = info->loopCtTemp;
                for (Tcl_Size L = 0; L < info->numLists; L++) {
                    ForeachVarList *vl = info->varLists[L];
                    if (!vl)
                        continue;
                    for (Tcl_Size v = 0; v < vl->numVars; v++) {
                        if (vl->varIndexes[v] > maxIdx)
                            maxIdx = vl->varIndexes[v];
                    }
                }
            }
        }
    }
    return (uint32_t)(maxIdx + 1);
}

static Tcl_Obj *ResolveToBytecodeObj(TbcxCtx *ctx, Tcl_Obj *cand) {
    if (!cand)
        return NULL;
    /* Fast path: direct bytecode rep */
    ByteCode *bc = NULL;
    ByteCodeGetInternalRep(cand, tbcxTyBytecode, bc);
    if (bc)
        return cand;
    if (cand->typePtr == tbcxTyProcBody) {
        Proc *p = (Proc *)cand->internalRep.twoPtrValue.ptr1;
        if (p && p->bodyPtr) {
            ByteCode *bc2 = NULL;
            ByteCodeGetInternalRep(p->bodyPtr, tbcxTyBytecode, bc2);
            if (bc2)
                return p->bodyPtr;
        }
    }
    (void)ctx; /* reserved for future use */
    return NULL;
}

static void WriteCompiledBlock(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *bcObj) {
    ByteCode *bc = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, bc);

    if (!bc) {
        W_Error(w, "tbcx: object is not bytecode");
        return;
    }
    if ((uint64_t)bc->numLitObjects > TBCX_MAX_LITERALS) {
        W_Error(w, "tbcx: too many literals");
        return;
    }
    if ((uint64_t)bc->numAuxDataItems > TBCX_MAX_AUX) {
        W_Error(w, "tbcx: too many AuxData");
        return;
    }
    if ((uint64_t)bc->numExceptRanges > TBCX_MAX_EXCEPT) {
        W_Error(w, "tbcx: too many exceptions");
        return;
    }

    /* 1) code */
    if ((uint64_t)bc->numCodeBytes > TBCX_MAX_CODE) {
        W_Error(w, "tbcx: code too large");
        return;
    }
    W_U32(w, (uint32_t)bc->numCodeBytes);
    W_Bytes(w, bc->codeStart, (size_t)bc->numCodeBytes);

    /* 2) literal pool */
    W_U32(w, (uint32_t)bc->numLitObjects);
    for (Tcl_Size i = 0; i < bc->numLitObjects; i++) {
        Tcl_Obj *lit = bc->objArrayPtr[i];
        WriteLiteral(w, ctx, lit);
    }

    /* 3) AuxData array */
    W_U32(w, (uint32_t)bc->numAuxDataItems);
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; i++) {
        AuxData *ad  = &bc->auxDataArrayPtr[i];
        uint32_t tag = 0xFFFFFFFFu;
        if (ad->type == tbcxAuxJTStr) {
            tag = TBCX_AUX_JT_STR;
        } else if (ad->type == tbcxAuxJTNum) {
            tag = TBCX_AUX_JT_NUM;
        } else if (ad->type == tbcxAuxDictUpdate) {
            tag = TBCX_AUX_DICTUPD;
        } else if (ad->type == tbcxAuxForeach || ad->type == tbcxAuxNewForeach) {
            /* Preserve the AuxData name family in the stream tag: */
            if (ad->type == tbcxAuxForeach) {
                tag = TBCX_AUX_FOREACH; /* "ForeachInfo" */
            } else {
                tag = TBCX_AUX_NEWFORE; /* "NewForeachInfo" */
            }
        } else {
            W_Error(w, "tbcx: unsupported AuxData kind");
            return;
        }
        W_U32(w, tag);
        switch (tag) {
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
        case TBCX_AUX_FOREACH:
            WriteAux_Foreach(w, ad);
            break;
        }
    }

    /* 4) exception ranges (emit as empty for simplicity; extension: fill if needed) */
    W_U32(w, (uint32_t)bc->numExceptRanges);
    for (Tcl_Size i = 0; i < bc->numExceptRanges; i++) {
        ExceptionRange *er = &bc->exceptArrayPtr[i];
        W_U32(w, (uint8_t)er->type);
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
    uint32_t numLocals = 0;
    if (bc->procPtr) {
        numLocals = (uint32_t)bc->procPtr->numCompiledLocals;
    } else if (bc->localCachePtr) {
        numLocals = (uint32_t)bc->localCachePtr->numVars;
    } else {
        numLocals = ComputeNumLocals(bc);
    }
    W_U32(w, numLocals);
}

static void WriteHeaderTop(TbcxOut *w, ByteCode *top) {
    TbcxHeader H;
    H.magic        = TBCX_MAGIC;
    H.format       = TBCX_FORMAT;
    H.tcl_version  = PackTclVersion();
    H.codeLenTop   = (uint64_t)top->numCodeBytes;
    H.numExceptTop = (uint32_t)top->numExceptRanges;
    H.numLitsTop   = (uint32_t)top->numLitObjects;
    H.numAuxTop    = (uint32_t)top->numAuxDataItems;
    H.maxStackTop  = (uint32_t)top->maxStackDepth;
    H.numLocalsTop = (uint32_t)(top->localCachePtr ? top->localCachePtr->numVars : ComputeNumLocals(top));
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

static void DV_Init(DefVec *dv) {
    dv->v   = NULL;
    dv->n   = 0;
    dv->cap = 0;
}
static void DV_Push(DefVec *dv, DefRec r) {
    if (dv->n == dv->cap) {
        dv->cap = dv->cap ? dv->cap * 2 : 16;
        dv->v   = (DefRec *)Tcl_Realloc(dv->v, dv->cap * sizeof(DefRec));
    }
    dv->v[dv->n++] = r;
}
static void DV_Free(DefVec *dv) {
    for (size_t i = 0; i < dv->n; i++) {
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
        Tcl_Free(dv->v);
}

static void CS_Init(ClsSet *cs) {
    cs->init = 1;
    Tcl_InitHashTable(&cs->ht, TCL_STRING_KEYS);
}

static void CS_Add(ClsSet *cs, Tcl_Obj *clsFqn) {
    if (!cs || !cs->init || !clsFqn)
        return;
    Tcl_Size       ln    = 0;
    const char    *s     = Tcl_GetStringFromObj(clsFqn, &ln);
    int            isNew = 0;
    /* For TCL_STRING_KEYS, Tcl duplicates the key internally. */
    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&cs->ht, s, &isNew);
    if (isNew) {
        Tcl_SetHashValue(he, NULL);
    }
}

static void CS_Free(ClsSet *cs) {
    if (!cs || !cs->init)
        return;
    /* Tcl frees string keys it duplicated for TCL_STRING_KEYS. */
    Tcl_DeleteHashTable(&cs->ht);
    cs->init = 0;
}

static Tcl_Obj *WordLiteralObj(Tcl_Interp *ip, const Tcl_Token *wordTok) {
    (void)ip;
    /* Prefer: WORD with exactly one TEXT component — already strips braces */
    if (wordTok->type == TCL_TOKEN_WORD && wordTok->numComponents == 1) {
        const Tcl_Token *t = wordTok + 1;
        if (t->type == TCL_TOKEN_TEXT) {
            Tcl_Obj *o = Tcl_NewStringObj(t->start, t->size);
            Tcl_IncrRefCount(o);
            return o;
        }
    }
    /* Fallback: SIMPLE_WORD — but peel balanced braces if present */
    if (wordTok->type == TCL_TOKEN_SIMPLE_WORD) {
        const char *s = wordTok->start;
        Tcl_Size    n = wordTok->size;
        if (n >= 2 && s[0] == '{' && s[n - 1] == '}') {
            Tcl_Obj *o = Tcl_NewStringObj(s + 1, n - 2);
            Tcl_IncrRefCount(o);
            return o;
        }
        Tcl_Obj *o = Tcl_NewStringObj(s, n);
        Tcl_IncrRefCount(o);
        return o;
    }
    return NULL;
}

static inline const Tcl_Token *NextWord(const Tcl_Token *wordTok) {
    return wordTok + 1 + wordTok->numComponents;
}

/* Strip any leading '::' so “::oo::class” and “oo::class” compare equal. */
static inline const char *CmdCore(const char *s) {
    if (!s)
        return "";
    while (s[0] == ':' && s[1] == ':')
        s += 2;
    return s;
}

/* Combine current NS and a (maybe relative) name into FQN string */
static Tcl_Obj *FqnUnder(Tcl_Obj *curNs, Tcl_Obj *name) {
    const char *nm = Tcl_GetString(name);
    if (nm[0] == ':' && nm[1] == ':') {
        Tcl_IncrRefCount(name);
        return name;
    }
    Tcl_Size    ln  = 0;
    const char *ns  = Tcl_GetStringFromObj(curNs, &ln);
    Tcl_Obj    *fqn = Tcl_NewStringObj(ns, ln);
    if (!(ln == 2 && ns[0] == ':' && ns[1] == ':'))
        Tcl_AppendToObj(fqn, "::", 2);
    Tcl_AppendObjToObj(fqn, name);
    Tcl_IncrRefCount(fqn);
    return fqn;
}

static void CaptureClassBody(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, Tcl_Obj *clsFqn, DefVec *defs, ClsSet *classes, int flags) {
    Tcl_Parse   p;
    const char *cur    = script;
    Tcl_Size    remain = (len >= 0 ? len : (Tcl_Size)strlen(script));
    while (remain > 0) {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK) {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1) {
            const Tcl_Token *w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj *cmd = WordLiteralObj(ip, w0);
            if (cmd) {
                const char *kw = CmdCore(Tcl_GetString(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4) {
                    /* Robust to options: take the last three words as name/args/body */
                    const Tcl_Token *cur = w0;
                    for (int w = 0; w < p.numWords - 3; w++)
                        cur = NextWord(cur);
                    const Tcl_Token *wN    = cur;
                    const Tcl_Token *wA    = NextWord(wN);
                    const Tcl_Token *wB    = NextWord(wA);
                    /* Now: wN=name, wA=args, wB=body */
                    Tcl_Obj         *mname = WordLiteralObj(ip, wN);
                    Tcl_Obj         *args  = WordLiteralObj(ip, wA);
                    Tcl_Obj         *body  = WordLiteralObj(ip, wB);
                    if (mname && args && body) {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = (strcmp(kw, "classmethod") == 0) ? DEF_KIND_CLASS : DEF_KIND_INST;
                        r.cls  = clsFqn;
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
                    } else {
                        if (mname)
                            Tcl_DecrRefCount(mname);
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                } else if (strcmp(kw, "constructor") == 0 && p.numWords >= 2) {
                    /* Last two words are args/body; allow the {body}-only form too */
                    const Tcl_Token *wArgs = NULL, *wBody = NULL;
                    const Tcl_Token *cur = w0;
                    for (int w = 0; w < p.numWords - 1; w++)
                        cur = NextWord(cur);
                    wBody = cur;
                    if (p.numWords >= 3) {
                        const Tcl_Token *pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj *args = (wArgs ? WordLiteralObj(ip, wArgs) : Tcl_NewStringObj("", 0));
                    Tcl_Obj *body = WordLiteralObj(ip, wBody);
                    if (args && body) {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_CTOR;
                        r.cls  = clsFqn;
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
                    } else {
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                } else if (strcmp(kw, "destructor") == 0 && p.numWords >= 1) {
                    /* Destructor may be "… destructor {body}" or "… destructor {args} {body}" */
                    const Tcl_Token *cur = w0;
                    for (int w = 0; w < p.numWords - 1; w++)
                        cur = NextWord(cur);
                    const Tcl_Token *wBody = cur;
                    Tcl_Obj         *args  = NULL;
                    if (p.numWords >= 3) {
                        const Tcl_Token *pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        args = WordLiteralObj(ip, pre);
                    } else {
                        args = Tcl_NewStringObj("", 0);
                    }
                    Tcl_Obj *body = WordLiteralObj(ip, wBody);
                    if (args && body) {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = DEF_KIND_DTOR;
                        r.cls  = clsFqn;
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
                    } else {
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        cur    = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
}

static void StubLinesForClass(Tcl_DString *out, DefVec *defs, Tcl_Obj *clsFqn) {
    Tcl_Size    fqnLen = 0;
    const char *cls    = Tcl_GetStringFromObj(clsFqn, &fqnLen);
    for (size_t i = 0; i < defs->n; i++) {
        DefRec *r = &defs->v[i];
        if (r->kind == DEF_KIND_PROC)
            continue; /* skip procs */
        if ((r->flags & DEF_F_FROM_BUILDER) == 0)
            continue; /* only builder-captured entries */
        /* Match this class */
        Tcl_Size    thisLen = 0;
        const char *thisCls = Tcl_GetStringFromObj(r->cls, &thisLen);
        if (!(thisLen == fqnLen && memcmp(thisCls, cls, (size_t)fqnLen) == 0))
            continue;

        Tcl_DString ln;
        Tcl_DStringInit(&ln);
        Tcl_Size tmp;
        Tcl_DStringAppendElement(&ln, "oo::define");
        Tcl_DStringAppendElement(&ln, thisCls);

        if (r->kind == DEF_KIND_INST || r->kind == DEF_KIND_CLASS) {
            Tcl_DStringAppendElement(&ln, (r->kind == DEF_KIND_CLASS) ? "classmethod" : "method");
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->name, &tmp));
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->args, &tmp));
            Tcl_DStringAppendElement(&ln, "");
        } else if (r->kind == DEF_KIND_CTOR) {
            Tcl_DStringAppendElement(&ln, "constructor");
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->args, &tmp));
            Tcl_DStringAppendElement(&ln, "");
        } else if (r->kind == DEF_KIND_DTOR) {
            Tcl_DStringAppendElement(&ln, "destructor");
            /* Always use 5-word form: destructor {} "" so shim can compare args */
            Tcl_DStringAppendElement(&ln, Tcl_GetStringFromObj(r->args, &tmp));
            Tcl_DStringAppendElement(&ln, "");
        }
        Tcl_DStringAppend(out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
        Tcl_DStringAppend(out, "\n", 1);
        Tcl_DStringFree(&ln);
    }
}

static int IsPureOodefineBuilderBody(Tcl_Interp *ip, const char *script, Tcl_Size len) {
    Tcl_Parse   p;
    const char *cur    = script;
    Tcl_Size    remain = (len >= 0 ? len : (Tcl_Size)strlen(script));

    while (remain > 0) {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK) {
            /* Be conservative: on parse error, do not treat as pure. */
            Tcl_FreeParse(&p);
            return 0;
        }
        /* Empty command (possible with trivia); keep scanning. */
        if (p.numWords == 0) {
            cur    = p.commandStart + p.commandSize;
            remain = (script + len) - cur;
            Tcl_FreeParse(&p);
            continue;
        }

        /* Step over TCL_TOKEN_COMMAND when present in 9.x so w0 is first word. */
        const Tcl_Token *w0 = p.tokenPtr;
        if (w0->type == TCL_TOKEN_COMMAND)
            w0++;

        /* If first word isn't a fixed literal, bail out. */
        Tcl_Obj *cmd = WordLiteralObj(ip, w0);
        if (!cmd) {
            Tcl_FreeParse(&p);
            return 0;
        }
        const char *core = CmdCore(Tcl_GetString(cmd));

        int         ok   = 0;
        if ((strcmp(core, "method") == 0 || strcmp(core, "classmethod") == 0)) {
            /* Expect: method mname args body  (>=4 words) */
            ok = (p.numWords >= 4);
        } else if ((strcmp(core, "constructor") == 0 || strcmp(core, "destructor") == 0)) {
            /*
             * constructor/destructor allow:
             *   - full form: <kw> args body  (>=3 words, typically >=4 incl. command token)
             *   - destructor body-only shorthand: <kw> body (>=2 words)
             * We only ensure it's not a bare keyword.
             */
            ok = (p.numWords >= 2);
        } else {
            ok = 0; /* any other subcommand breaks purity */
        }
        Tcl_DecrRefCount(cmd);
        if (!ok) {
            Tcl_FreeParse(&p);
            return 0;
        }

        cur    = p.commandStart + p.commandSize;
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
static Tcl_Obj *StubbedBuilderBody(Tcl_Interp *ip, Tcl_Obj *bodyObj) {
    Tcl_Size    blen = 0;
    const char *bstr = Tcl_GetStringFromObj(bodyObj, &blen);
    Tcl_DString out;
    Tcl_DStringInit(&out);
    Tcl_Parse   p;
    const char *cur    = bstr;
    Tcl_Size    remain = blen;

    while (remain > 0) {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK) {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1) {
            const Tcl_Token *w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj *cmd = WordLiteralObj(ip, w0);
            if (cmd) {
                const char *kw = CmdCore(Tcl_GetString(cmd));
                if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 4) {
                    /* Last three words are name/args/body */
                    const Tcl_Token *t = w0;
                    for (int w = 0; w < p.numWords - 3; w++)
                        t = NextWord(t);
                    const Tcl_Token *wN = t, *wA = NextWord(wN);
                    Tcl_Obj         *mname = WordLiteralObj(ip, wN);
                    Tcl_Obj         *args  = WordLiteralObj(ip, wA);
                    if (mname && args) {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, (strcmp(kw, "classmethod") == 0) ? "classmethod" : "method");
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(mname));
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(args));
                        Tcl_DStringAppendElement(&ln, ""); /* stub body */
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    if (mname)
                        Tcl_DecrRefCount(mname);
                    if (args)
                        Tcl_DecrRefCount(args);
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                } else if ((strcmp(kw, "constructor") == 0) && p.numWords >= 2) {
                    /* Constructor: last two words are args/body (args may be omitted in shorthand) */
                    const Tcl_Token *t = w0;
                    for (int w = 0; w < p.numWords - 1; w++)
                        t = NextWord(t);
                    const Tcl_Token *wArgs = NULL;
                    if (p.numWords >= 3) {
                        const Tcl_Token *pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj *args = wArgs ? WordLiteralObj(ip, wArgs) : Tcl_NewStringObj("", 0);
                    if (args) {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "constructor");
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(args));
                        Tcl_DStringAppendElement(&ln, ""); /* stub body */
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    if (args)
                        Tcl_DecrRefCount(args);
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                } else if (strcmp(kw, "destructor") == 0 && p.numWords >= 2) {
                    /* Destructor: allow body-only or args/body; always canonicalize to two lists */
                    const Tcl_Token *t = w0;
                    for (int w = 0; w < p.numWords - 1; w++)
                        t = NextWord(t);
                    const Tcl_Token *wArgs = NULL;
                    if (p.numWords >= 3) {
                        const Tcl_Token *pre = w0;
                        for (int w = 0; w < p.numWords - 2; w++)
                            pre = NextWord(pre);
                        wArgs = pre;
                    }
                    Tcl_Obj *args = wArgs ? WordLiteralObj(ip, wArgs) : Tcl_NewStringObj("", 0);
                    if (args) {
                        Tcl_DString ln;
                        Tcl_DStringInit(&ln);
                        Tcl_DStringAppendElement(&ln, "destructor");
                        Tcl_DStringAppendElement(&ln, Tcl_GetString(args));
                        Tcl_DStringAppendElement(&ln, ""); /* stub body */
                        Tcl_DStringAppend(&out, Tcl_DStringValue(&ln), Tcl_DStringLength(&ln));
                        Tcl_DStringAppend(&out, "\n", 1);
                        Tcl_DStringFree(&ln);
                    }
                    if (args)
                        Tcl_DecrRefCount(args);
                    Tcl_DecrRefCount(cmd);
                    goto next_cmd;
                }
                Tcl_DecrRefCount(cmd);
            }
            /* Non-method subcommand: copy verbatim */
            Tcl_DStringAppend(&out, p.commandStart, p.commandSize);
        }
    next_cmd:
        cur    = p.commandStart + p.commandSize;
        remain = (bstr + blen) - cur;
        Tcl_FreeParse(&p);
    }
    Tcl_Obj *res = Tcl_NewStringObj(Tcl_DStringValue(&out), Tcl_DStringLength(&out));
    Tcl_DStringFree(&out);
    return res;
}

static Tcl_Obj *RewriteScript(Tcl_Interp *ip, const char *script, Tcl_Size len, DefVec *defs, Tcl_Obj *curNs) {
    Tcl_DString out;
    Tcl_DStringInit(&out);
    Tcl_Parse   p;
    const char *cur    = script;
    Tcl_Size    remain = len >= 0 ? len : (Tcl_Size)strlen(script);

    while (remain > 0) {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK) {
            Tcl_FreeParse(&p);
            break;
        }
        const char *cmdStart = p.commandStart;
        const char *cmdEnd   = p.commandStart + p.commandSize;
        int         handled  = 0;
        if (p.numWords >= 3) {
            const Tcl_Token *w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND)
                w0++;
            Tcl_Obj *cmd = WordLiteralObj(ip, w0);
            if (cmd) {
                const char *c0 = CmdCore(Tcl_GetString(cmd));
                /* Direct oo::define forms: stub out bodies to avoid any compile on load */
                if (strcmp(c0, "oo::define") == 0 && p.numWords >= 5) {
                    const Tcl_Token *wCls = NextWord(w0);
                    const Tcl_Token *wK   = NextWord(wCls);
                    Tcl_Obj         *kwd  = WordLiteralObj(ip, wK);
                    Tcl_Obj         *clsO = WordLiteralObj(ip, wCls);
                    if (kwd && clsO) {
                        const char *kw = CmdCore(Tcl_GetString(kwd));
                        if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 6) {
                            const Tcl_Token *t = w0;
                            for (int w = 0; w < p.numWords - 3; w++)
                                t = NextWord(t);
                            const Tcl_Token *wN = t, *wA = NextWord(wN);
                            Tcl_Obj         *mname = WordLiteralObj(ip, wN), *args = WordLiteralObj(ip, wA);
                            if (mname && args) {
                                Tcl_DString line;
                                Tcl_DStringInit(&line);
                                Tcl_DStringAppendElement(&line, "oo::define");
                                Tcl_DStringAppendElement(&line, Tcl_GetString(clsO));
                                Tcl_DStringAppendElement(&line, Tcl_GetString(kwd));
                                Tcl_DStringAppendElement(&line, Tcl_GetString(mname));
                                Tcl_DStringAppendElement(&line, Tcl_GetString(args));
                                Tcl_DStringAppendElement(&line, "");
                                Tcl_DStringAppend(&out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                                Tcl_DStringAppend(&out, "\n", 1);
                                Tcl_DStringFree(&line);
                                handled = 1;
                            }
                            if (mname)
                                Tcl_DecrRefCount(mname);
                            if (args)
                                Tcl_DecrRefCount(args);
                        } else if (strcmp(kw, "constructor") == 0 && p.numWords >= 4) {
                            const Tcl_Token *t = w0;
                            for (int w = 0; w < p.numWords - 1; w++)
                                t = NextWord(t);
                            const Tcl_Token *wArgsTok = NULL;
                            if (p.numWords >= 5) {
                                const Tcl_Token *pre = w0;
                                for (int w = 0; w < p.numWords - 2; w++)
                                    pre = NextWord(pre);
                                wArgsTok = pre;
                            }
                            Tcl_Obj *args = wArgsTok ? WordLiteralObj(ip, wArgsTok) : Tcl_NewStringObj("", 0);
                            if (args) {
                                Tcl_DString line;
                                Tcl_DStringInit(&line);
                                Tcl_DStringAppendElement(&line, "oo::define");
                                Tcl_DStringAppendElement(&line, Tcl_GetString(clsO));
                                Tcl_DStringAppendElement(&line, "constructor");
                                Tcl_DStringAppendElement(&line, Tcl_GetString(args));
                                Tcl_DStringAppendElement(&line, "");
                                Tcl_DStringAppend(&out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                                Tcl_DStringAppend(&out, "\n", 1);
                                Tcl_DStringFree(&line);
                                handled = 1;
                            }
                            if (args)
                                Tcl_DecrRefCount(args);
                        } else if (strcmp(kw, "destructor") == 0 && p.numWords >= 3) {
                            const Tcl_Token *t = w0;
                            for (int w = 0; w < p.numWords - 1; w++)
                                t = NextWord(t);
                            const Tcl_Token *wArgsTok = NULL;
                            if (p.numWords >= 4) {
                                const Tcl_Token *pre = w0;
                                for (int w = 0; w < p.numWords - 2; w++)
                                    pre = NextWord(pre);
                                wArgsTok = pre;
                            }
                            Tcl_Obj *args = wArgsTok ? WordLiteralObj(ip, wArgsTok) : Tcl_NewStringObj("", 0);
                            if (args) {
                                Tcl_DString line;
                                Tcl_DStringInit(&line);
                                Tcl_DStringAppendElement(&line, "oo::define");
                                Tcl_DStringAppendElement(&line, Tcl_GetString(clsO));
                                Tcl_DStringAppendElement(&line, "destructor");
                                Tcl_DStringAppendElement(&line, Tcl_GetString(args));
                                Tcl_DStringAppendElement(&line, "");
                                Tcl_DStringAppend(&out, Tcl_DStringValue(&line), Tcl_DStringLength(&line));
                                Tcl_DStringAppend(&out, "\n", 1);
                                Tcl_DStringFree(&line);
                                handled = 1;
                            }
                            if (args)
                                Tcl_DecrRefCount(args);
                        }
                    }
                    if (kwd)
                        Tcl_DecrRefCount(kwd);
                    if (clsO)
                        Tcl_DecrRefCount(clsO);
                }
                /* Handle: namespace eval <ns> <body> — rewrite the body recursively */
                if (strcmp(c0, "namespace") == 0) {
                    const Tcl_Token *w1  = NextWord(w0);
                    Tcl_Obj         *sub = (w1 ? WordLiteralObj(ip, w1) : NULL);
                    if (sub && strcmp(CmdCore(Tcl_GetString(sub)), "eval") == 0) {
                        const Tcl_Token *w2 = NextWord(w1);
                        const Tcl_Token *w3 = w2 ? NextWord(w2) : NULL;
                        if (w2 && w3) {
                            Tcl_Obj    *nsObj     = WordLiteralObj(ip, w2);
                            Tcl_Obj    *nsFqn     = FqnUnder(curNs, nsObj);
                            Tcl_Obj    *bodyObj   = WordLiteralObj(ip, w3);
                            Tcl_Obj    *rewritten = RewriteScript(ip, Tcl_GetString(bodyObj), Tcl_GetCharLength(bodyObj), defs, nsFqn);
                            Tcl_Obj    *canonBody = CanonTrivia(rewritten);
                            /* Re-emit: namespace eval */
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
                            Tcl_DecrRefCount(bodyObj);
                            Tcl_DecrRefCount(nsFqn);
                            Tcl_DecrRefCount(nsObj);
                            handled = 1;
                        }
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                }
                /* Handle: oo::define <class> {builder}
                 * If the builder is pure method/ctor/dtor: drop body and emit stubs.
                 * Otherwise, reconstruct a builder that preserves non-method
                 * subcommands and replaces every method/ctor/dtor body with "".
                 */
                if (!handled && strcmp(c0, "oo::define") == 0 && p.numWords == 3) {
                    const Tcl_Token *w1 = NextWord(w0);             /* class */
                    const Tcl_Token *w2 = w1 ? NextWord(w1) : NULL; /* builder body */
                    if (w1 && w2) {
                        Tcl_Obj *clsObj  = WordLiteralObj(ip, w1);
                        Tcl_Obj *bodyObj = WordLiteralObj(ip, w2);
                        if (clsObj && bodyObj) {
                            Tcl_Obj    *clsFqn = FqnUnder(curNs, clsObj);
                            Tcl_Size    blen   = 0;
                            const char *bstr   = Tcl_GetStringFromObj(bodyObj, &blen);
                            if (IsPureOodefineBuilderBody(ip, bstr, blen)) {
                                /*
                                 * Pure builder: drop the literal body entirely and
                                 * re-emit canonical stubs. This removes the huge
                                 * top-level literal (e.g., DOCX case) and leaves
                                 * method bodies to the precompiled Methods section.
                                 */
                                StubLinesForClass(&out, defs, clsFqn);
                                handled = 1;
                            } else {
                                /* Non-pure: rebuild builder with stubbed method bodies */
                                Tcl_Obj    *stubbed = StubbedBuilderBody(ip, bodyObj);
                                Tcl_DString cmdLn;
                                Tcl_DStringInit(&cmdLn);
                                Tcl_DStringAppendElement(&cmdLn, "oo::define");
                                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(clsObj));
                                Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
                                Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                Tcl_DStringAppend(&out, "\n", 1);
                                Tcl_DStringFree(&cmdLn);
                                Tcl_DecrRefCount(stubbed);
                                handled = 1;
                            }
                            Tcl_DecrRefCount(clsFqn);
                        }
                        if (clsObj)
                            Tcl_DecrRefCount(clsObj);
                        if (bodyObj)
                            Tcl_DecrRefCount(bodyObj);
                    }
                }

                /* Handle: oo::class create <name> ?{builder}?
                 * If builder is present and pure, drop it and emit stubs.
                 * If builder is present and non-pure, rebuild a stubbed builder
                 * that preserves non-method subcommands. Otherwise, use {}. */
                if (!handled && strcmp(c0, "oo::class") == 0 && p.numWords >= 3) {
                    const Tcl_Token *w1  = NextWord(w0);
                    Tcl_Obj         *sub = WordLiteralObj(ip, w1);
                    if (sub && strcmp(Tcl_GetString(sub), "create") == 0) {
                        const Tcl_Token *w2 = NextWord(w1); /* class name */
                        Tcl_Obj         *nm = WordLiteralObj(ip, w2);
                        if (nm) {
                            Tcl_Obj    *clsFqn = FqnUnder(curNs, nm);
                            Tcl_DString cmdLn;
                            Tcl_DStringInit(&cmdLn);
                            Tcl_DStringAppendElement(&cmdLn, "oo::class");
                            Tcl_DStringAppendElement(&cmdLn, "create");
                            Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(nm));
                            if (p.numWords >= 4) {
                                const Tcl_Token *w3  = NextWord(w2); /* builder */
                                Tcl_Obj         *bod = WordLiteralObj(ip, w3);
                                if (bod) {
                                    Tcl_Size    bl = 0;
                                    const char *bs = Tcl_GetStringFromObj(bod, &bl);
                                    if (IsPureOodefineBuilderBody(ip, bs, bl)) {
                                        /* drop builder, then emit stubs */
                                        Tcl_DStringAppendElement(&cmdLn, ""); /* empty builder */
                                        Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                        Tcl_DStringAppend(&out, "\n", 1);
                                        StubLinesForClass(&out, defs, clsFqn);
                                    } else {
                                        Tcl_Obj *stubbed = StubbedBuilderBody(ip, bod);
                                        Tcl_DStringAppendElement(&cmdLn, Tcl_GetString(stubbed));
                                        Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                        Tcl_DStringAppend(&out, "\n", 1);
                                        Tcl_DecrRefCount(stubbed);
                                    }
                                    Tcl_DStringFree(&cmdLn);
                                    Tcl_DecrRefCount(bod);
                                } else {
                                    Tcl_DStringAppendElement(&cmdLn, "");
                                    Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                    Tcl_DStringAppend(&out, "\n", 1);
                                    Tcl_DStringFree(&cmdLn);
                                }
                            } else {
                                Tcl_DStringAppendElement(&cmdLn, "");
                                Tcl_DStringAppend(&out, Tcl_DStringValue(&cmdLn), Tcl_DStringLength(&cmdLn));
                                Tcl_DStringAppend(&out, "\n", 1);
                                Tcl_DStringFree(&cmdLn);
                            }
                            Tcl_DecrRefCount(clsFqn);
                            Tcl_DecrRefCount(nm);
                            handled = 1;
                        }
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        if (!handled) {
            /* Default path: copy original command verbatim */
            Tcl_DStringAppend(&out, cmdStart, (int)(cmdEnd - cmdStart));
        }
        cur    = cmdEnd;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }

    Tcl_Obj *rew = Tcl_NewStringObj(Tcl_DStringValue(&out), Tcl_DStringLength(&out));
    Tcl_DStringFree(&out);
    return rew;
}

static Tcl_Obj *CanonTrivia(Tcl_Obj *in) {
    if (!in)
        return Tcl_NewStringObj("", 0);
    Tcl_Size    n = 0;
    const char *s = Tcl_GetStringFromObj(in, &n);
    if (n == 0)
        return Tcl_NewStringObj("", 0);

    Tcl_Size i = 0;
    Tcl_Size j = n;
    /* Trim leading */
    while (i < j) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
        } else {
            break;
        }
    }
    /* Trim trailing */
    while (j > i) {
        unsigned char c = (unsigned char)s[j - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            j--;
        } else {
            break;
        }
    }
    if (j <= i)
        return Tcl_NewStringObj("", 0);
    return Tcl_NewStringObj(s + i, j - i);
}

static void CaptureStaticsRec(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, DefVec *defs, ClsSet *classes, int inBuilder) {
    Tcl_Parse   p;
    const char *cur    = script;
    Tcl_Size    remain = len >= 0 ? len : (Tcl_Size)strlen(script);

    while (remain > 0) {
        if (Tcl_ParseCommand(ip, cur, remain, 0, &p) != TCL_OK) {
            Tcl_FreeParse(&p);
            break;
        }
        if (p.numWords >= 1) {
            /* In 9.x the stream may or may not begin with TCL_TOKEN_COMMAND.
             * Step over it only when present so w0 is really the first word. */
            const Tcl_Token *w0 = p.tokenPtr;
            if (w0->type == TCL_TOKEN_COMMAND) {
                w0++; /* first word */
            }
            Tcl_Obj *cmd = WordLiteralObj(ip, w0);
            if (cmd) {
                const char *cmdStr  = Tcl_GetString(cmd);
                const char *cmdCore = CmdCore(cmdStr);
                if (p.numWords == 4 && strcmp(cmdCore, "proc") == 0) {
                    const Tcl_Token *w1   = NextWord(w0);
                    const Tcl_Token *w2   = NextWord(w1);
                    const Tcl_Token *w3   = NextWord(w2);
                    Tcl_Obj         *name = WordLiteralObj(ip, w1);
                    Tcl_Obj         *args = WordLiteralObj(ip, w2);
                    Tcl_Obj         *body = WordLiteralObj(ip, w3);
                    /* Canonicalize arg list */
                    if (args) {
                        Tcl_Size _tbcx_dummy;
                        (void)Tcl_ListObjLength(ip, args, &_tbcx_dummy);
                    }
                    if (name && args && body) {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind  = DEF_KIND_PROC;
                        r.name  = name;
                        r.args  = args;
                        r.body  = body;
                        r.flags = 0;
                        Tcl_IncrRefCount(name);
                        Tcl_IncrRefCount(args);
                        Tcl_IncrRefCount(body);
                        r.ns = curNs;
                        Tcl_IncrRefCount(r.ns);
                        DV_Push(defs, r);
                    } else {
                        if (name)
                            Tcl_DecrRefCount(name);
                        if (args)
                            Tcl_DecrRefCount(args);
                        if (body)
                            Tcl_DecrRefCount(body);
                    }
                } else if (p.numWords == 4 && strcmp(cmdCore, "namespace") == 0) {
                    const Tcl_Token *w1  = NextWord(w0);
                    Tcl_Obj         *sub = WordLiteralObj(ip, w1);
                    if (sub && strcmp(Tcl_GetString(sub), "eval") == 0) {
                        const Tcl_Token *w2  = NextWord(w1); /* ns */
                        const Tcl_Token *w3  = NextWord(w2); /* body */
                        Tcl_Obj         *ns  = WordLiteralObj(ip, w2);
                        Tcl_Obj         *bod = WordLiteralObj(ip, w3);
                        if (ns && bod) {
                            Tcl_Obj    *nsFqn  = FqnUnder(curNs, ns);
                            Tcl_Size    bodLen = 0;
                            const char *bodStr = Tcl_GetStringFromObj(bod, &bodLen);
                            CaptureStaticsRec(ip, bodStr, bodLen, nsFqn, defs, classes, 0);
                            Tcl_DecrRefCount(nsFqn);
                        }
                        if (ns)
                            Tcl_DecrRefCount(ns);
                        if (bod)
                            Tcl_DecrRefCount(bod);
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                } else if (p.numWords >= 5 && strcmp(cmdCore, "oo::define") == 0) {
                    const Tcl_Token *wCls = NextWord(w0);
                    Tcl_Obj         *cls  = WordLiteralObj(ip, wCls);
                    const Tcl_Token *wK   = NextWord(wCls);
                    Tcl_Obj         *kwd  = WordLiteralObj(ip, wK);
                    if (cls && kwd) {
                        Tcl_Obj    *clsFqn = FqnUnder(curNs, cls);
                        const char *kw     = Tcl_GetString(kwd);
                        if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords >= 6) {
                            /* Robust: last three words are name/args/body */
                            const Tcl_Token *cur = w0;
                            for (int w = 0; w < p.numWords - 3; w++)
                                cur = NextWord(cur);
                            const Tcl_Token *wN    = cur;
                            const Tcl_Token *wA    = NextWord(wN);
                            const Tcl_Token *wB    = NextWord(wA);
                            /* Now: wN=name, wA=args, wB=body */
                            Tcl_Obj         *mname = WordLiteralObj(ip, wN);
                            Tcl_Obj         *args  = WordLiteralObj(ip, wA);
                            Tcl_Obj         *body  = WordLiteralObj(ip, wB);
                            if (args) {
                                Tcl_Size _tbcx_dummy;
                                (void)Tcl_ListObjLength(ip, args, &_tbcx_dummy);
                            }
                            if (mname && args && body) {
                                DefRec r;
                                memset(&r, 0, sizeof(r));
                                r.kind = (strcmp(kw, "classmethod") == 0 ? DEF_KIND_CLASS : DEF_KIND_INST);
                                r.cls  = clsFqn;
                                Tcl_IncrRefCount(r.cls);
                                r.name = mname;
                                r.args = args;
                                r.body = body;
                                Tcl_IncrRefCount(r.name);
                                Tcl_IncrRefCount(r.args);
                                Tcl_IncrRefCount(r.body);
                                r.ns = curNs;
                                Tcl_IncrRefCount(r.ns);
                                r.flags = inBuilder ? DEF_F_FROM_BUILDER : 0;
                                DV_Push(defs, r);
                                CS_Add(classes, clsFqn);
                            } else {
                                if (mname)
                                    Tcl_DecrRefCount(mname);
                                if (args)
                                    Tcl_DecrRefCount(args);
                                if (body)
                                    Tcl_DecrRefCount(body);
                                Tcl_DecrRefCount(clsFqn);
                            }
                        } else if ((strcmp(kw, "constructor") == 0 || strcmp(kw, "destructor") == 0) && p.numWords >= 4) {
                            /* Last two words are args/body; allow destructor body-only form */
                            const Tcl_Token *cur = w0;
                            for (int w = 0; w < p.numWords - 1; w++)
                                cur = NextWord(cur);
                            const Tcl_Token *wBody    = cur;
                            const Tcl_Token *wArgsTok = NULL;
                            if (p.numWords >= 5) {
                                const Tcl_Token *pre = w0;
                                for (int w = 0; w < p.numWords - 2; w++)
                                    pre = NextWord(pre);
                                wArgsTok = pre;
                            }
                            Tcl_Obj *args = (wArgsTok ? WordLiteralObj(ip, wArgsTok) : Tcl_NewStringObj("", 0));
                            Tcl_Obj *body = WordLiteralObj(ip, wBody);
                            if (args) {
                                Tcl_Size _tbcx_dummy;
                                (void)Tcl_ListObjLength(ip, args, &_tbcx_dummy);
                            }
                            if (args && body) {
                                DefRec r;
                                memset(&r, 0, sizeof(r));
                                r.kind = (strcmp(kw, "constructor") == 0 ? DEF_KIND_CTOR : DEF_KIND_DTOR);
                                r.cls  = clsFqn;
                                Tcl_IncrRefCount(r.cls);
                                r.args = args;
                                r.body = body;
                                Tcl_IncrRefCount(r.args);
                                Tcl_IncrRefCount(r.body);
                                r.ns = curNs;
                                Tcl_IncrRefCount(r.ns);
                                r.flags = inBuilder ? DEF_F_FROM_BUILDER : 0;
                                DV_Push(defs, r);
                                CS_Add(classes, clsFqn);
                            } else {
                                if (args)
                                    Tcl_DecrRefCount(args);
                                if (body)
                                    Tcl_DecrRefCount(body);
                                Tcl_DecrRefCount(clsFqn);
                            }
                        } else {
                            Tcl_DecrRefCount(clsFqn);
                        }
                    } else {
                        if (cls)
                            Tcl_DecrRefCount(cls);
                        if (kwd)
                            Tcl_DecrRefCount(kwd);
                    }
                } else if (p.numWords == 3 && strcmp(cmdCore, "oo::define") == 0) {
                    /* Builder form: oo::define <class> { … }
                     * Recurse into the builder to capture method/classmethod/ctor/dtor.
                     */
                    const Tcl_Token *wCls = NextWord(w0);
                    const Tcl_Token *wBod = wCls ? NextWord(wCls) : NULL;
                    Tcl_Obj         *cls  = wCls ? WordLiteralObj(ip, wCls) : NULL;
                    Tcl_Obj         *bod  = wBod ? WordLiteralObj(ip, wBod) : NULL;
                    if (cls && bod) {
                        Tcl_Obj    *clsFqn = FqnUnder(curNs, cls);
                        Tcl_Size    bl     = 0;
                        const char *bs     = Tcl_GetStringFromObj(bod, &bl);
                        CaptureClassBody(ip, bs, bl, curNs, clsFqn, defs, classes, DEF_F_FROM_BUILDER);
                        CS_Add(classes, clsFqn);
                        Tcl_DecrRefCount(clsFqn);
                    }
                    if (cls)
                        Tcl_DecrRefCount(cls);
                    if (bod)
                        Tcl_DecrRefCount(bod);
                } else if (p.numWords >= 3 && strcmp(cmdCore, "oo::class") == 0) {
                    /* literal: oo::class create <name> ... */
                    const Tcl_Token *w1  = NextWord(w0);
                    Tcl_Obj         *sub = WordLiteralObj(ip, w1);
                    if (sub && strcmp(Tcl_GetString(sub), "create") == 0) {
                        const Tcl_Token *w2 = NextWord(w1); /* class name */
                        Tcl_Obj         *nm = WordLiteralObj(ip, w2);
                        if (nm) {
                            Tcl_Obj *clsFqn = FqnUnder(curNs, nm);
                            CS_Add(classes, clsFqn);
                            /* Recurse into builder (word #4) to capture methods */
                            if (p.numWords >= 4) {
                                const Tcl_Token *w3  = NextWord(w2);
                                Tcl_Obj         *bod = WordLiteralObj(ip, w3);
                                if (bod) {
                                    Tcl_Size    bl = 0;
                                    const char *bs = Tcl_GetStringFromObj(bod, &bl);
                                    CaptureClassBody(ip, bs, bl, curNs, clsFqn, defs, classes, DEF_F_FROM_BUILDER);
                                    Tcl_DecrRefCount(bod);
                                }
                            }
                            Tcl_DecrRefCount(clsFqn);
                            Tcl_DecrRefCount(nm);
                        }
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                }
                Tcl_DecrRefCount(cmd);
            }
        }
        cur    = p.commandStart + p.commandSize;
        remain = (script + len) - cur;
        Tcl_FreeParse(&p);
    }
}

static int EmitTbcxStream(Tcl_Interp *ip, Tcl_Obj *scriptObj, TbcxOut *w) {
    TbcxCtx ctx = {0};
    ctx.interp  = ip;
    CtxInitStripBodies(&ctx);

    DefVec defs;
    DV_Init(&defs);
    ClsSet classes;
    CS_Init(&classes);
    Tcl_Obj *rootNs = Tcl_NewStringObj("::", -1);
    Tcl_IncrRefCount(rootNs);
    Tcl_Size    srcLen  = 0;
    const char *srcStr  = Tcl_GetStringFromObj(scriptObj, &srcLen);
    Tcl_Obj    *srcCopy = Tcl_NewStringObj(srcStr, srcLen);
    Tcl_IncrRefCount(srcCopy);
    if (srcLen > 0) {
        CaptureStaticsRec(ip, srcStr, srcLen, rootNs, &defs, &classes, 0);
    }

    /* Precompute set of bodies to strip out of top-level literals (we serialize compiled blocks separately). */
    for (size_t i = 0; i < defs.n; i++) {
        if (defs.v[i].body) {
            CtxAddStripBody(&ctx, defs.v[i].body);
        }
    }

    /* Top-level rewrite: remove builder bodies and append oo::define stubs for captured builder methods. */
    {
        Tcl_Obj *rew = RewriteScript(ip, srcStr, srcLen, &defs, rootNs);
        if (rew) {
            Tcl_DecrRefCount(srcCopy);
            srcCopy = rew;
            Tcl_IncrRefCount(srcCopy);
        }
    }

    /* 1. Compile top-level (after capture + rewrite) */
    if (TclSetByteCodeFromAny(ip, srcCopy, NULL, NULL) != TCL_OK) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CS_Free(&classes);
        CtxFreeStripBodies(&ctx);
        return TCL_ERROR;
    }
    ByteCode *top = NULL;
    ByteCodeGetInternalRep(srcCopy, tbcxTyBytecode, top);
    if (!top) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CS_Free(&classes);
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: failed to get top bytecode", -1));
        CtxFreeStripBodies(&ctx);
        return TCL_ERROR;
    }

    /* 3. Header */
    WriteHeaderTop(w, top);
    if (w->err) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CS_Free(&classes);
        CtxFreeStripBodies(&ctx);
        return TCL_ERROR;
    }

    /* 4. Top-level compiled block */
    ctx.stripActive = 1;
    WriteCompiledBlock(w, &ctx, srcCopy);
    ctx.stripActive = 0;
    if (w->err) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CS_Free(&classes);
        CtxFreeStripBodies(&ctx);
        return TCL_ERROR;
    }

    /* 5. Procs section (nameFqn, namespace, args, block) */
    /* For simplicity we emit only captured “proc” definitions here. OO methods follow in methods section. */
    uint32_t numProcs = 0;
    for (size_t i = 0; i < defs.n; i++) {
        if (defs.v[i].kind == DEF_KIND_PROC)
            numProcs++;
    }
    W_U32(w, numProcs);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind == DEF_KIND_PROC) {
            /* Serialize FQN/name, ns, args */
            Tcl_Size    ln;
            const char *s;
            s = Tcl_GetStringFromObj(defs.v[i].name, &ln);
            W_LPString(w, s, ln);
            s = Tcl_GetStringFromObj(defs.v[i].ns, &ln);
            W_LPString(w, s, ln);
            s = Tcl_GetStringFromObj(defs.v[i].args, &ln);
            W_LPString(w, s, ln);

            /* Compile body offline (proc semantics) and emit */
            if (CompileProcLike(w, &ctx, defs.v[i].ns, defs.v[i].args, defs.v[i].body, "body of proc") != TCL_OK) {
                Tcl_DecrRefCount(srcCopy);
                Tcl_DecrRefCount(rootNs);
                DV_Free(&defs);
                CS_Free(&classes);
                CtxFreeStripBodies(&ctx);
                return TCL_ERROR;
            }
        }

    /* 6. Classes section (FQN + nSupers=0 for now) — use unique set */
    {
        Tcl_HashEntry *h;
        Tcl_HashSearch srch;
        /* Count */
        uint32_t       numClasses = 0;
        for (h = Tcl_FirstHashEntry(&classes.ht, &srch); h; h = Tcl_NextHashEntry(&srch)) {
            numClasses++;
        }
        W_U32(w, numClasses);
        /* Stream each unique FQN once */
        for (h = Tcl_FirstHashEntry(&classes.ht, &srch); h; h = Tcl_NextHashEntry(&srch)) {
            const char *key = (const char *)Tcl_GetHashKey(&classes.ht, h);
            Tcl_Size    ln  = (Tcl_Size)strlen(key);
            W_LPString(w, key, ln);
            W_U32(w, 0); /* nSupers */
        }
    }

    /* 7. Methods section: emit captured OO methods/ctors/dtors */
    uint32_t numMethods = 0;
    for (size_t i = 0; i < defs.n; i++) {
        if (defs.v[i].kind != DEF_KIND_PROC)
            numMethods++;
    }
    W_U32(w, numMethods);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind != DEF_KIND_PROC) {
            Tcl_Size    ln;
            const char *s;
            /* classFqn */
            s = Tcl_GetStringFromObj(defs.v[i].cls, &ln);
            W_LPString(w, s, ln);
            /* wire kind 0..3 expected by loader (inst=0, class=1, ctor=2, dtor=3) */
            W_U8(w, (uint8_t)(defs.v[i].kind - DEF_KIND_INST));
            /* name (empty for ctor/dtor) */
            if (defs.v[i].kind == DEF_KIND_CTOR || defs.v[i].kind == DEF_KIND_DTOR) {
                W_LPString(w, "", 0);
            } else {
                s = Tcl_GetStringFromObj(defs.v[i].name, &ln);
                W_LPString(w, s, ln);
            }
            /* args */
            s = Tcl_GetStringFromObj(defs.v[i].args, &ln);
            W_LPString(w, s, ln);

            /* Compile & emit block (proc semantics) */
            if (CompileProcLike(w, &ctx, defs.v[i].cls, defs.v[i].args, defs.v[i].body, "body of method") != TCL_OK) {
                Tcl_DecrRefCount(srcCopy);
                Tcl_DecrRefCount(rootNs);
                DV_Free(&defs);
                CS_Free(&classes);
                CtxFreeStripBodies(&ctx);
                return TCL_ERROR;
            }
        }
    DV_Free(&defs);
    CS_Free(&classes);
    Tcl_DecrRefCount(srcCopy);
    Tcl_DecrRefCount(rootNs);
    CtxFreeStripBodies(&ctx);
    return (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;
}

int CheckBinaryChan(Tcl_Interp *ip, Tcl_Channel ch) {
    if (Tcl_SetChannelOption(ip, ch, "-translation", "binary") != TCL_OK)
        return TCL_ERROR;
    if (Tcl_SetChannelOption(ip, ch, "-eofchar", "") != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}

/* ==========================================================================
 * Tcl commands
 * ========================================================================== */

int Tbcx_SaveChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script channelName");
        return TCL_ERROR;
    }

    Tcl_Obj    *script = objv[1];
    const char *chName = Tcl_GetString(objv[2]);
    Tcl_Channel ch     = Tcl_GetChannel(interp, chName, NULL);
    if (!ch)
        return TCL_ERROR;
    if (CheckBinaryChan(interp, ch) != TCL_OK)
        return TCL_ERROR;

    TbcxOut w  = {interp, ch, TCL_OK};
    int     rc = EmitTbcxStream(interp, script, &w);
    return rc;
}

int Tbcx_SaveObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script outPath");
        return TCL_ERROR;
    }

    Tcl_Obj *script = objv[1];
    Tcl_Obj *path   = objv[2];
    Tcl_Obj *norm   = Tcl_FSGetNormalizedPath(interp, path);
    if (!norm)
        return TCL_ERROR;

    Tcl_Channel ch = Tcl_FSOpenFileChannel(interp, norm, "w", 0666);
    if (!ch)
        return TCL_ERROR;
    if (CheckBinaryChan(interp, ch) != TCL_OK) {
        Tcl_Close(interp, ch);
        return TCL_ERROR;
    }

    TbcxOut w  = {interp, ch, TCL_OK};
    int     rc = EmitTbcxStream(interp, script, &w);
    if (Tcl_Close(interp, ch) != TCL_OK)
        rc = TCL_ERROR;
    if (rc == TCL_OK)
        Tcl_SetObjResult(interp, norm);
    return rc;
}

int Tbcx_SaveFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "in.tcl outPath");
        return TCL_ERROR;
    }

    Tcl_Obj    *inPath  = objv[1];
    Tcl_Obj    *outPath = objv[2];
    Tcl_Channel in      = Tcl_FSOpenFileChannel(interp, inPath, "r", 0);
    if (!in)
        return TCL_ERROR;
    if (CheckBinaryChan(interp, in) != TCL_OK) {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }
    Tcl_Obj *script = Tcl_NewObj();
    Tcl_IncrRefCount(script);
    char buf[8192];
    while (1) {
        int n = Tcl_ReadRaw(in, buf, (int)sizeof(buf));
        if (n < 0) {
            Tcl_DecrRefCount(script);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        if (n == 0)
            break;
        Tcl_AppendToObj(script, buf, n);
    }
    Tcl_Close(interp, in);

    Tcl_Channel out = Tcl_FSOpenFileChannel(interp, outPath, "w", 0666);
    if (!out) {
        Tcl_DecrRefCount(script);
        return TCL_ERROR;
    }
    if (CheckBinaryChan(interp, out) != TCL_OK) {
        Tcl_Close(interp, out);
        Tcl_DecrRefCount(script);
        return TCL_ERROR;
    }

    TbcxOut w  = {interp, out, TCL_OK};
    int     rc = EmitTbcxStream(interp, script, &w);
    Tcl_DecrRefCount(script);
    if (Tcl_Close(interp, out) != TCL_OK)
        rc = TCL_ERROR;
    if (rc == TCL_OK) {
        Tcl_Obj *norm = Tcl_FSGetNormalizedPath(interp, outPath);
        if (norm)
            Tcl_SetObjResult(interp, norm);
    }
    return rc;
}
