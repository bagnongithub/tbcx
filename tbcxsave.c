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
    Tcl_Interp *interp;
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
    int      kind; /* 0=proc; 1=inst-meth; 2=class-meth; 3=ctor; 4=dtor (methods stored in “methods” section) */
    Tcl_Obj *cls;  /* class name if method */
} DefRec;

typedef struct {
    DefRec *v;
    size_t  n, cap;
} DefVec;

typedef struct {
    Tcl_Obj *name; /* class FQN */
} ClsRec;

typedef struct {
    ClsRec *v;
    size_t  n, cap;
} ClsVec;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static void                    CaptureStaticsRec(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, DefVec *defs, ClsVec *classes);
int                            CheckBinaryChan(Tcl_Interp *ip, Tcl_Channel ch);
static int                     CompileProcLikeAndEmit(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *nsFQN, Tcl_Obj *argsList, Tcl_Obj *bodyObj, const char *whereTag);
static uint32_t                ComputeNumLocalsFromAux(ByteCode *bc);
static void                    CV_Free(ClsVec *cv);
static void                    CV_Init(ClsVec *cv);
static void                    CV_PushUnique(ClsVec *cv, Tcl_Obj *clsFqn);
static void                    DV_Free(DefVec *dv);
static void                    DV_Init(DefVec *dv);
static void                    DV_Push(DefVec *dv, DefRec r);
static int                     EmitTbcxStream(Tcl_Interp *ip, Tcl_Obj *scriptObj, TbcxOut *w);
static Tcl_Obj                *FqnUnder(Tcl_Obj *curNs, Tcl_Obj *name);
static void                    Lit_Bignum(TbcxOut *w, Tcl_Obj *o);
static void                    Lit_Bytecode(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *bcObj);
static void                    Lit_Dict(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o);
static void                    Lit_LambdaBC(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *lambda);
static void                    Lit_List(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *o);
static inline const Tcl_Token *NextWord(const Tcl_Token *wordTok);
static Tcl_Obj                *NsFqn(Tcl_Interp *ip, Tcl_Namespace *nsPtr);
static Tcl_Obj                *ResolveToBytecodeObj(TbcxCtx *ctx, Tcl_Obj *cand);
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

static inline void             W_Error(TbcxOut *w, const char *msg) {
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
    /* simple insertion sort */
    for (Tcl_Size i = 1; i < idx; i++) {
        Tcl_Obj    *cur = keys[i];
        Tcl_Size    j   = i;
        Tcl_Size    la;
        const char *a = Tcl_GetStringFromObj(cur, &la);
        while (j > 0) {
            Tcl_Size    l;
            const char *b   = Tcl_GetStringFromObj(keys[j - 1], &l);
            int         cmp = Tcl_UtfNcmp(a, b, (la < l ? la : l));
            if (cmp == 0)
                cmp = (la < l ? -1 : (la > l ? 1 : 0));
            if (cmp >= 0)
                break;
            keys[j] = keys[j - 1];
            j--;
        }
        keys[j] = cur;
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

    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nf = 0, nmLen = 0;
        Tcl_Obj **fv = NULL;
        if (Tcl_ListObjGetElements(ctx->interp, argv[i], &nf, &fv) != TCL_OK) {
            W_Error(w, "tbcx: lambda args decode");
            goto lambda_cleanup;
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
    procPtr->firstLocalPtr = first;
    procPtr->lastLocalPtr  = last;

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
        for (CompiledLocal *cl = first; cl;) {
            CompiledLocal *next = cl->nextPtr;
            if (cl->defValuePtr)
                Tcl_DecrRefCount(cl->defValuePtr);
            Tcl_Free((char *)cl);
            cl = next;
        }
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

/* Helper: compile a {args, body} pair with proc semantics and emit BC.
 * Mirrors the lambda path (Lit_LambdaBC) but takes explicit args/body/ns. */
static int CompileProcLikeAndEmit(TbcxOut *w, TbcxCtx *ctx, Tcl_Obj *nsFQN, Tcl_Obj *argsList, Tcl_Obj *bodyObj, const char *whereTag) {
    Tcl_Interp    *ip   = ctx->interp;
    const char    *nsNm = Tcl_GetString(nsFQN);
    Tcl_Namespace *ns   = Tcl_FindNamespace(ip, nsNm, NULL, 0);
    if (!ns)
        ns = Tcl_CreateNamespace(ip, nsNm, NULL, NULL);

    Tcl_Size  argc = 0;
    Tcl_Obj **argv = NULL;
    if (Tcl_ListObjGetElements(ip, argsList, &argc, &argv) != TCL_OK) {
        W_Error(w, "tbcx: bad args list");
        return TCL_ERROR;
    }

    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr              = (Interp *)ip;
    procPtr->refCount          = 1;
    procPtr->numArgs           = (int)argc;
    procPtr->numCompiledLocals = (int)argc;

    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nf = 0, nmLen = 0;
        Tcl_Obj **fv = NULL;
        if (Tcl_ListObjGetElements(ip, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2) {
            W_Error(w, "tbcx: bad arg spec");
            goto cple_fail;
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
    procPtr->firstLocalPtr = first;
    procPtr->lastLocalPtr  = last;

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
    for (CompiledLocal *cl = first; cl;) {
        CompiledLocal *nx = cl->nextPtr;
        if (cl->defValuePtr)
            Tcl_DecrRefCount(cl->defValuePtr);
        Tcl_Free((char *)cl);
        cl = nx;
    }
    Tcl_Free((char *)procPtr);
    return (w->err == TCL_OK) ? TCL_OK : TCL_ERROR;

cple_fail:
    for (CompiledLocal *cl = first; cl;) {
        CompiledLocal *nx = cl->nextPtr;
        if (cl->defValuePtr)
            Tcl_DecrRefCount(cl->defValuePtr);
        Tcl_Free((char *)cl);
        cl = nx;
    }
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
        W_U32(w, TBCX_LIT_BYTECODE);
        Lit_Bytecode(w, ctx, obj);
    } else if (obj->typePtr == tbcxTyProcBody) {
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
        /* Default: emit as STRING using current UTF-8 rep. */
        Tcl_Size    n;
        const char *s = Tcl_GetStringFromObj(obj, &n);
        W_U32(w, TBCX_LIT_STRING);
        W_LPString(w, s, n);
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
    /* Sort by key UTF-8 */
    for (uint32_t a = 1; a < cnt; a++) {
        JTEntry     t  = arr[a];
        uint32_t    b  = a;
        const char *sa = t.key ? t.key : "";
        while (b > 0) {
            const char *sb  = arr[b - 1].key ? arr[b - 1].key : "";
            int         cmp = strcmp(sa, sb);
            if (cmp >= 0)
                break;
            arr[b] = arr[b - 1];
            b--;
        }
        arr[b] = t;
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

static uint32_t ComputeNumLocalsFromAux(ByteCode *bc) {
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
    for (int i = 0; i < bc->numLitObjects; i++) {
        Tcl_Obj *lit = bc->objArrayPtr[i];
        WriteLiteral(w, ctx, lit);
    }

    /* 3) AuxData array */
    W_U32(w, (uint32_t)bc->numAuxDataItems);
    for (int i = 0; i < bc->numAuxDataItems; i++) {
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
    for (int i = 0; i < bc->numExceptRanges; i++) {
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
    /* Prefer the compiler's Proc view. localCachePtr is often NULL at save
     * time, and AuxData does not cover ordinary compiled locals (e.g. loop
     * temps from 'for', etc.). */
    if (bc->procPtr) {
        numLocals = (uint32_t)bc->procPtr->numCompiledLocals;
    } else if (bc->localCachePtr) {
        numLocals = (uint32_t)bc->localCachePtr->numVars;
    } else {
        numLocals = ComputeNumLocalsFromAux(bc);
    }
    W_U32(w, numLocals);
}

static void WriteHeaderTop(TbcxOut *w, ByteCode *top) {
    TbcxHeader H;
    H.magic        = TBCX_MAGIC;
    H.format       = TBCX_FORMAT;
    H.tcl_version  = PackTclVersion();
    H.codeLenTop   = (uint64_t)top->numCodeBytes;
    H.numCmdsTop   = 0;
    H.numExceptTop = (uint32_t)top->numExceptRanges;
    H.numLitsTop   = (uint32_t)top->numLitObjects;
    H.numAuxTop    = (uint32_t)top->numAuxDataItems;
    H.maxStackTop  = (uint32_t)top->maxStackDepth;
    H.numLocalsTop = (uint32_t)(top->localCachePtr ? top->localCachePtr->numVars : ComputeNumLocalsFromAux(top));
    W_U32(w, H.magic);
    W_U32(w, H.format);
    W_U32(w, H.tcl_version);
    W_U64(w, H.codeLenTop);
    W_U32(w, H.numCmdsTop);
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

static void CV_Init(ClsVec *cv) {
    cv->v = NULL;
    cv->n = cv->cap = 0;
}
static void CV_PushUnique(ClsVec *cv, Tcl_Obj *clsFqn) {
    Tcl_Size    lnA = 0;
    const char *sa  = Tcl_GetStringFromObj(clsFqn, &lnA);
    for (size_t i = 0; i < cv->n; i++) {
        Tcl_Size    lnB = 0;
        const char *sb  = Tcl_GetStringFromObj(cv->v[i].name, &lnB);
        if (lnA == lnB && memcmp(sa, sb, (size_t)lnA) == 0)
            return;
    }
    if (cv->n == cv->cap) {
        cv->cap = cv->cap ? cv->cap * 2 : 8;
        cv->v   = (ClsRec *)Tcl_Realloc(cv->v, cv->cap * sizeof(ClsRec));
    }
    cv->v[cv->n].name = clsFqn;
    Tcl_IncrRefCount(clsFqn);
    cv->n++;
}
static void CV_Free(ClsVec *cv) {
    for (size_t i = 0; i < cv->n; i++)
        Tcl_DecrRefCount(cv->v[i].name);
    if (cv->v)
        Tcl_Free(cv->v);
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

static void CaptureStaticsRec(Tcl_Interp *ip, const char *script, Tcl_Size len, Tcl_Obj *curNs, DefVec *defs, ClsVec *classes) {
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
                const char *cmdStr = Tcl_GetString(cmd);
                if (p.numWords == 4 && strcmp(cmdStr, "proc") == 0) {
                    const Tcl_Token *w1   = NextWord(w0);
                    const Tcl_Token *w2   = NextWord(w1);
                    const Tcl_Token *w3   = NextWord(w2);
                    Tcl_Obj         *name = WordLiteralObj(ip, w1);
                    Tcl_Obj         *args = WordLiteralObj(ip, w2);
                    Tcl_Obj         *body = WordLiteralObj(ip, w3);
                    if (name && args && body) {
                        DefRec r;
                        memset(&r, 0, sizeof(r));
                        r.kind = 0;
                        r.name = name;
                        r.args = args;
                        r.body = body;
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
                } else if (p.numWords == 4 && strcmp(cmdStr, "namespace") == 0) {
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
                            CaptureStaticsRec(ip, bodStr, bodLen, nsFqn, defs, classes);
                            Tcl_DecrRefCount(nsFqn);
                        }
                        if (ns)
                            Tcl_DecrRefCount(ns);
                        if (bod)
                            Tcl_DecrRefCount(bod);
                    }
                    if (sub)
                        Tcl_DecrRefCount(sub);
                } else if (p.numWords >= 5 && strcmp(cmdStr, "oo::define") == 0) {
                    const Tcl_Token *wCls = NextWord(w0);
                    Tcl_Obj         *cls  = WordLiteralObj(ip, wCls);
                    const Tcl_Token *wK   = NextWord(wCls);
                    Tcl_Obj         *kwd  = WordLiteralObj(ip, wK);
                    if (cls && kwd) {
                        Tcl_Obj    *clsFqn = FqnUnder(curNs, cls);
                        const char *kw     = Tcl_GetString(kwd);
                        if ((strcmp(kw, "method") == 0 || strcmp(kw, "classmethod") == 0) && p.numWords == 7) {
                            const Tcl_Token *wN    = NextWord(wK);
                            const Tcl_Token *wA    = NextWord(wN);
                            const Tcl_Token *wB    = NextWord(wA);
                            Tcl_Obj         *mname = WordLiteralObj(ip, wN);
                            Tcl_Obj         *args  = WordLiteralObj(ip, wA);
                            Tcl_Obj         *body  = WordLiteralObj(ip, wB);

                            if (mname && args && body) {
                                DefRec r;
                                memset(&r, 0, sizeof(r));
                                r.kind = (strcmp(kw, "classmethod") == 0 ? TBCX_METH_CLASS : TBCX_METH_INST);
                                r.cls  = clsFqn;
                                Tcl_IncrRefCount(r.cls);
                                r.name = mname;
                                r.args = args;
                                r.body = body;
                                Tcl_IncrRefCount(mname);
                                Tcl_IncrRefCount(args);
                                Tcl_IncrRefCount(body);
                                r.ns = curNs;
                                Tcl_IncrRefCount(r.ns);
                                DV_Push(defs, r);
                                CV_PushUnique(classes, clsFqn);
                            } else {
                                if (mname)
                                    Tcl_DecrRefCount(mname);
                                if (args)
                                    Tcl_DecrRefCount(args);
                                if (body)
                                    Tcl_DecrRefCount(body);
                                Tcl_DecrRefCount(clsFqn);
                            }
                        } else if ((strcmp(kw, "constructor") == 0 || strcmp(kw, "destructor") == 0) && p.numWords == 6) {
                            const Tcl_Token *wA   = NextWord(wK);
                            const Tcl_Token *wB   = NextWord(wA);
                            Tcl_Obj         *args = WordLiteralObj(ip, wA);
                            Tcl_Obj         *body = WordLiteralObj(ip, wB);
                            if (args && body) {
                                DefRec r;
                                memset(&r, 0, sizeof(r));
                                r.kind = (strcmp(kw, "constructor") == 0 ? TBCX_METH_CTOR : TBCX_METH_DTOR);
                                r.cls  = clsFqn;
                                Tcl_IncrRefCount(r.cls);
                                r.args = args;
                                r.body = body;
                                Tcl_IncrRefCount(args);
                                Tcl_IncrRefCount(body);
                                r.ns = curNs;
                                Tcl_IncrRefCount(r.ns);
                                DV_Push(defs, r);
                                CV_PushUnique(classes, clsFqn);
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
                } else if (p.numWords >= 3 && strcmp(cmdStr, "oo::class") == 0) {
                    /* literal: oo::class create <name> ... */
                    const Tcl_Token *w1  = NextWord(w0);
                    Tcl_Obj         *sub = WordLiteralObj(ip, w1);
                    if (sub && strcmp(Tcl_GetString(sub), "create") == 0) {
                        const Tcl_Token *w2 = NextWord(w1);
                        Tcl_Obj         *nm = WordLiteralObj(ip, w2);
                        if (nm) {
                            Tcl_Obj *clsFqn = FqnUnder(curNs, nm);
                            CV_PushUnique(classes, clsFqn);
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
    TbcxCtx ctx = {ip};

    DefVec  defs;
    DV_Init(&defs);
    ClsVec classes;
    CV_Init(&classes);
    Tcl_Obj *rootNs = Tcl_NewStringObj("::", -1);
    Tcl_IncrRefCount(rootNs);
    Tcl_Size    srcLen  = 0;
    const char *srcStr  = Tcl_GetStringFromObj(scriptObj, &srcLen);
    Tcl_Obj    *srcCopy = Tcl_NewStringObj(srcStr, srcLen);
    Tcl_IncrRefCount(srcCopy);
    if (srcLen > 0) {
        CaptureStaticsRec(ip, srcStr, srcLen, rootNs, &defs, &classes);
    }

    /* 1. Compile top-level (after capture) */
    if (TclSetByteCodeFromAny(ip, scriptObj, NULL, NULL) != TCL_OK) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CV_Free(&classes);
        return TCL_ERROR;
    }
    ByteCode *top = NULL;
    ByteCodeGetInternalRep(scriptObj, tbcxTyBytecode, top);
    if (!top) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CV_Free(&classes);
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: failed to get top bytecode", -1));
        return TCL_ERROR;
    }

    /* 3. Header */
    WriteHeaderTop(w, top);
    if (w->err) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CV_Free(&classes);
        return TCL_ERROR;
    }

    /* 4. Top-level compiled block */
    WriteCompiledBlock(w, &ctx, scriptObj);
    if (w->err) {
        Tcl_DecrRefCount(srcCopy);
        Tcl_DecrRefCount(rootNs);
        DV_Free(&defs);
        CV_Free(&classes);
        return TCL_ERROR;
    }

    /* 5. Procs section (nameFqn, namespace, args, block) */
    /* For simplicity we emit only captured “proc” definitions here. OO methods follow in methods section. */
    uint32_t numProcs = 0;
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind == 0)
            numProcs++;
    W_U32(w, numProcs);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind == 0) {
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
            if (CompileProcLikeAndEmit(w, &ctx, defs.v[i].ns, defs.v[i].args, defs.v[i].body, "body of proc") != TCL_OK) {
                Tcl_DecrRefCount(srcCopy);
                Tcl_DecrRefCount(rootNs);
                DV_Free(&defs);
                CV_Free(&classes);
                return TCL_ERROR;
            }
        }

    /* 6. Classes section (FQN + nSupers=0 for now) */
    W_U32(w, (uint32_t)classes.n);
    for (size_t c = 0; c < classes.n; c++) {
        Tcl_Size    ln = 0;
        const char *s  = Tcl_GetStringFromObj(classes.v[c].name, &ln);
        W_LPString(w, s, ln);
        W_U32(w, 0); /* nSupers */
    }

    /* 7. Methods section: emit captured OO methods/ctors/dtors */
    uint32_t numMethods = 0;
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind != 0)
            numMethods++;
    W_U32(w, numMethods);
    for (size_t i = 0; i < defs.n; i++)
        if (defs.v[i].kind != 0) {
            Tcl_Size    ln;
            const char *s;
            /* classFqn */
            s = Tcl_GetStringFromObj(defs.v[i].cls, &ln);
            W_LPString(w, s, ln);
            /* kind */
            W_U8(w, (uint8_t)defs.v[i].kind);
            /* name (empty for ctor/dtor) */
            if (defs.v[i].kind == TBCX_METH_CTOR || defs.v[i].kind == TBCX_METH_DTOR) {
                W_LPString(w, "", 0);
            } else {
                s = Tcl_GetStringFromObj(defs.v[i].name, &ln);
                W_LPString(w, s, ln);
            }
            /* args */
            s = Tcl_GetStringFromObj(defs.v[i].args, &ln);
            W_LPString(w, s, ln);
            /* bodyTextLen (=0 as we serialize compiled block next) */
            W_U32(w, 0);

            /* Compile & emit block (proc semantics) */
            if (CompileProcLikeAndEmit(w, &ctx, defs.v[i].cls, defs.v[i].args, defs.v[i].body, "body of method") != TCL_OK) {
                Tcl_DecrRefCount(srcCopy);
                Tcl_DecrRefCount(rootNs);
                DV_Free(&defs);
                CV_Free(&classes);
                return TCL_ERROR;
            }
        }
    DV_Free(&defs);
    CV_Free(&classes);
    Tcl_DecrRefCount(srcCopy);
    Tcl_DecrRefCount(rootNs);
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
