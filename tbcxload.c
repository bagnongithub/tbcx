/* ==========================================================================
 * tbcxload.c — TBCX load+eval for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct {
    Tcl_Interp *interp;
    Tcl_Channel chan;
    int         err; /* TCL_OK / TCL_ERROR */
} TbcxIn;

typedef struct { /* minimal prefix used by TclInitByteCodeObj in our path */
    Tcl_Interp     *interp;
    Namespace      *nsPtr;
    unsigned char  *codeStart, *codeNext, *codeEnd;
    Tcl_Obj       **objArrayPtr;
    int             numLitObjects;
    AuxData        *auxDataArrayPtr;
    int             numAuxDataItems;
    ExceptionRange *exceptArrayPtr;
    int             numExceptRanges;
    int             maxStackDepth;
    Proc           *procPtr;
} TBCX_CompileEnvMin;

typedef struct {
    Tcl_HashTable procsByFqn; /* key: FQN Tcl_Obj*, val: procbody Tcl_Obj* */
} ProcShim;

typedef struct {
    Tcl_HashTable methodsByKey; /* key: Tcl_Obj* "class\x1Fkind\x1Fname", val: Tcl_Obj* PAIR {args, procbody} */

} OOShim;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static Tcl_Obj       *BuildMethodKey(Tcl_Interp *ip, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *name);
static int            InstallOOShim(Tcl_Interp *ip, OOShim *os);
static int            InstallProcShim(Tcl_Interp *ip, ProcShim *ps);
static int            LoadTbcxStream(Tcl_Interp *ip, Tcl_Channel ch);
static inline int     R_Bytes(TbcxIn *r, void *p, size_t n);
static inline void    R_Error(TbcxIn *r, const char *msg);
static inline int     R_LPString(TbcxIn *r, char **sp, uint32_t *lenp);
static inline int     R_U32(TbcxIn *r, uint32_t *vp);
static inline int     R_U64(TbcxIn *r, uint64_t *vp);
static inline int     R_U8(TbcxIn *r, uint8_t *v);
static int            ReadAuxArray(TbcxIn *r, Tcl_Interp *ip, AuxData **auxOut, uint32_t *numAuxOut);
static Tcl_Obj       *ReadCompiledBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut);
static int            ReadExceptions(TbcxIn *r, ExceptionRange **exOut, uint32_t *numOut);
static int            ReadHeader(TbcxIn *r, TbcxHeader *H);
static Tcl_Obj       *ReadLiteral(TbcxIn *r, Tcl_Interp *ip);
static int            ReadOneMethodAndRegister(TbcxIn *r, Tcl_Interp *ip, OOShim *os);
static int            ReadOneProcAndRegister(TbcxIn *r, Tcl_Interp *ip, ProcShim *shim);
static Tcl_Namespace *TBCX_EnsureNamespace(Tcl_Interp *ip, const char *fqn);
static Tcl_Obj       *TBCX_NewByteCodeObjFromParts(Tcl_Interp *ip, Namespace *nsPtr, const unsigned char *code, uint32_t codeLen, Tcl_Obj **lits, uint32_t numLits, AuxData *auxArr, uint32_t numAux,
                                                   ExceptionRange *exArr, uint32_t numEx, int maxStackDepth);
static ByteCode      *TbcxInitByteCodeObj(Tcl_Obj *objPtr, const Tcl_ObjType *typePtr, const TBCX_CompileEnvMin *env);
int                   Tbcx_LoadChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
int                   Tbcx_LoadFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
static int            Tbcx_OODefineShimObjCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static int            Tbcx_ProcShimObjCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static void           UninstallOOShim(Tcl_Interp *ip, OOShim *os);
static void           UninstallProcShim(Tcl_Interp *ip, ProcShim *ps);

/* ==========================================================================
 * Stuff
 * ========================================================================== */

static inline void    R_Error(TbcxIn *r, const char *msg) {
    if (r->err == TCL_OK) {
        Tcl_SetObjResult(r->interp, Tcl_NewStringObj(msg, -1));
        r->err = TCL_ERROR;
    }
}

static inline int R_Bytes(TbcxIn *r, void *p, size_t n) {
    if (r->err)
        return 0;
    int got = Tcl_ReadRaw(r->chan, (char *)p, (int)n);
    if (got != (int)n) {
        R_Error(r, "tbcx: short read");
        return 0;
    }
    return 1;
}

static inline int R_U8(TbcxIn *r, uint8_t *v) {
    return R_Bytes(r, v, 1);
}

static inline int R_U32(TbcxIn *r, uint32_t *vp) {
    uint32_t v = 0;
    if (!R_Bytes(r, &v, 4))
        return 0;
    if (!tbcxHostIsLE) {
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    }
    *vp = v;
    return 1;
}

static inline int R_U64(TbcxIn *r, uint64_t *vp) {
    uint64_t v = 0;
    if (!R_Bytes(r, &v, 8))
        return 0;
    if (!tbcxHostIsLE) {
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) |
            ((v & 0x0000FF0000000000ull) >> 24) | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }
    *vp = v;
    return 1;
}

/* LPString = u32 length + payload bytes (no NUL) */
static inline int R_LPString(TbcxIn *r, char **sp, uint32_t *lenp) {
    uint32_t n = 0;
    if (!R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_STR) {
        R_Error(r, "tbcx: LPString too large");
        return 0;
    }
    char *buf = (char *)Tcl_Alloc(n + 1u);
    if (!R_Bytes(r, buf, n)) {
        Tcl_Free(buf);
        return 0;
    }
    buf[n] = '\0';
    *sp    = buf;
    *lenp  = n;
    return 1;
}

static Tcl_Obj *BuildMethodKey(Tcl_Interp *ip, Tcl_Obj *clsFqn, uint8_t kind, Tcl_Obj *name) {
    Tcl_Obj *k = Tcl_NewStringObj("", 0);
    Tcl_AppendObjToObj(k, clsFqn);
    Tcl_AppendToObj(k, "\x1F", 1);
    char kb[8];
    int  n = snprintf(kb, sizeof kb, "%u", (unsigned)kind);
    Tcl_AppendToObj(k, kb, n);
    Tcl_AppendToObj(k, "\x1F", 1);
    if (name)
        Tcl_AppendObjToObj(k, name);
    Tcl_IncrRefCount(k);
    return k;
}

static int Tbcx_OODefineShimObjCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    OOShim *os = (OOShim *)cd;
    if (objc < 3) {
        Tcl_Obj *argv0 = Tcl_NewStringObj("::tbcx::__oo_define_orig__", -1);
        Tcl_IncrRefCount(argv0);
        Tcl_Obj **argv2 = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)objc);
        argv2[0]        = argv0;
        for (Tcl_Size i = 1; i < objc; i++)
            argv2[i] = objv[i];
        int rc = Tcl_EvalObjv(ip, objc, argv2, TCL_EVAL_GLOBAL);
        Tcl_Free(argv2);
        Tcl_DecrRefCount(argv0);
        return rc;
    }
    Tcl_Obj    *cls = objv[1], *sub = objv[2];
    const char *subc         = Tcl_GetString(sub);
    /* Compute class FQN under current ns if relative */
    Tcl_Obj    *clsFqn       = NULL;
    const char *nm           = Tcl_GetString(cls);
    Tcl_Obj    *tmpEmptyArgs = NULL; /* for destructor(empty-args) lifetime */
    if (nm[0] == ':' && nm[1] == ':') {
        clsFqn = cls;
    } else {
        Tcl_Namespace *cur     = Tcl_GetCurrentNamespace(ip);
        const char    *curName = cur ? cur->fullName : "::";
        clsFqn                 = Tcl_NewStringObj(curName, -1);
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0'))
            Tcl_AppendToObj(clsFqn, "::", 2);
        Tcl_AppendObjToObj(clsFqn, cls);
    }

    Tcl_Obj *key         = NULL;
    Tcl_Obj *preBody     = NULL;
    Tcl_Obj *savedArgs   = NULL;
    Tcl_Obj *runtimeArgs = NULL;
    uint8_t  kind        = 0xFF;

    /* Normalize optioned forms: for method/classmethod pick the last 3 words: name args body.
       For constructor pick last 2 words: args body. For destructor: last word is body, or last 2 if args present. */
    int      wantSubst   = 0;
    int      nameIdx = -1, argsIdx = -1, bodyIdx = -1;
    if (strcmp(subc, "method") == 0 || strcmp(subc, "classmethod") == 0) {
        if (objc >= 6) {
            kind        = (strcmp(subc, "classmethod") == 0) ? TBCX_METH_CLASS : TBCX_METH_INST;
            nameIdx     = (int)objc - 3;
            argsIdx     = (int)objc - 2;
            bodyIdx     = (int)objc - 1;
            key         = BuildMethodKey(ip, clsFqn, kind, objv[nameIdx]);
            runtimeArgs = objv[argsIdx];
            wantSubst   = 1;
        }
    } else if (strcmp(subc, "constructor") == 0) {
        if (objc >= 5) {
            kind        = TBCX_METH_CTOR;
            argsIdx     = (int)objc - 2;
            bodyIdx     = (int)objc - 1;
            key         = BuildMethodKey(ip, clsFqn, kind, NULL);
            runtimeArgs = objv[argsIdx];
            wantSubst   = 1;
        }
    } else if (strcmp(subc, "destructor") == 0) {
        kind = TBCX_METH_DTOR;
        if (objc >= 5) {
            argsIdx     = (int)objc - 2;
            bodyIdx     = (int)objc - 1;
            runtimeArgs = objv[argsIdx];
        } else if (objc == 4) {
            bodyIdx     = 3;
            /* Treat as empty-args form */
            runtimeArgs = Tcl_NewStringObj("", 0);
            Tcl_IncrRefCount(runtimeArgs);
            tmpEmptyArgs = runtimeArgs;
        }
        if (bodyIdx >= 0) {
            key       = BuildMethodKey(ip, clsFqn, kind, NULL);
            wantSubst = 1;
        }
    }
    if (wantSubst && key) {
        Tcl_HashEntry *he = Tcl_FindHashEntry(&os->methodsByKey, (const char *)key);
        if (he) {
            Tcl_Obj *pair = (Tcl_Obj *)Tcl_GetHashValue(he);
            if (pair) {
                (void)Tcl_ListObjIndex(ip, pair, 0, &savedArgs);
                (void)Tcl_ListObjIndex(ip, pair, 1, &preBody);
            }
        }
    }

    /* Always call original, substituting body if we have a precompiled one. AND args match. */
    Tcl_Obj *argv0 = Tcl_NewStringObj("::tbcx::__oo_define_orig__", -1);
    Tcl_IncrRefCount(argv0);
    int rc = TCL_OK;
    if (preBody && runtimeArgs && savedArgs) {
        Tcl_Size    aLen = 0, bLen = 0;
        const char *a     = Tcl_GetStringFromObj(runtimeArgs, &aLen);
        const char *b     = Tcl_GetStringFromObj(savedArgs, &bLen);
        int         match = (aLen == bLen) && (memcmp(a, b, (size_t)aLen) == 0);
        if (match) {
            /* Build argv with the same shape but with body replaced at bodyIdx */
            Tcl_Obj **argv2 = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)objc);
            argv2[0]        = argv0;
            for (Tcl_Size i = 1; i < objc; i++) {
                argv2[i] = (i == (Tcl_Size)bodyIdx) ? preBody : objv[i];
            }
            rc = Tcl_EvalObjv(ip, objc, argv2, TCL_EVAL_GLOBAL);
            Tcl_Free(argv2);
            Tcl_DecrRefCount(argv0);
            if (clsFqn != cls)
                Tcl_DecrRefCount(clsFqn);
            if (tmpEmptyArgs)
                Tcl_DecrRefCount(tmpEmptyArgs);
            return rc;
        }
    }
    rc = Tcl_EvalObjv(ip, objc, (Tcl_Obj *const *)objv, TCL_EVAL_GLOBAL);

    Tcl_DecrRefCount(argv0);
    if (key && key != clsFqn)
        Tcl_DecrRefCount(key);
    if (clsFqn != cls)
        Tcl_DecrRefCount(clsFqn);
    if (tmpEmptyArgs)
        Tcl_DecrRefCount(tmpEmptyArgs);
    return rc;
}

static int InstallOOShim(Tcl_Interp *ip, OOShim *os) {
    memset(os, 0, sizeof(*os));
    Tcl_InitObjHashTable(&os->methodsByKey);
    if (TclRenameCommand(ip, "oo::define", "::tbcx::__oo_define_orig__") != TCL_OK)
        return TCL_ERROR;
    Tcl_CreateObjCommand2(ip, "oo::define", Tbcx_OODefineShimObjCmd, os, NULL);
    return TCL_OK;
}

static void UninstallOOShim(Tcl_Interp *ip, OOShim *os) {
    Tcl_DeleteCommand(ip, "oo::define");
    (void)TclRenameCommand(ip, "::tbcx::__oo_define_orig__", "oo::define");
    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&os->methodsByKey, &s); e; e = Tcl_NextHashEntry(&s)) {
        Tcl_Obj *val = (Tcl_Obj *)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&os->methodsByKey);
}

static Tcl_Namespace *TBCX_EnsureNamespace(Tcl_Interp *ip, const char *fqn) {
    Tcl_Namespace *nsPtr = NULL;
    nsPtr                = Tcl_FindNamespace(ip, fqn, NULL, TCL_LEAVE_ERR_MSG);
    if (!nsPtr) {
        nsPtr = Tcl_CreateNamespace(ip, fqn, NULL, NULL);
    }
    return nsPtr;
}

/* Mirrors TclInitByteCode()’s packed layout + TclInitByteCodeObj()’s attach,
 * but never calls internal TclPreserveByteCode(). Instead we set refCount = 1.
 */

static ByteCode *TbcxInitByteCodeObj(Tcl_Obj *objPtr, const Tcl_ObjType *typePtr, const TBCX_CompileEnvMin *env) {
    Interp      *iPtr              = (Interp *)env->interp;
    Namespace   *nsPtr             = env->nsPtr ? env->nsPtr : (iPtr->varFramePtr ? iPtr->varFramePtr->nsPtr : iPtr->globalNsPtr);

    /* Sizes for packed allocation (match TclInitByteCode) */
    const size_t codeBytes         = (size_t)(env->codeNext - env->codeStart);
    const size_t objArrayBytes     = (size_t)env->numLitObjects * sizeof(Tcl_Obj *);
    const size_t exceptArrayBytes  = (size_t)env->numExceptRanges * sizeof(ExceptionRange);
    const size_t auxDataArrayBytes = (size_t)env->numAuxDataItems * sizeof(AuxData);
    const size_t cmdLocBytes       = 0; /* no cmd-location map in this path */

    size_t       structureSize     = 0;
    structureSize += TCL_ALIGN(sizeof(ByteCode));
    structureSize += TCL_ALIGN(codeBytes);
    structureSize += TCL_ALIGN(objArrayBytes);
    structureSize += TCL_ALIGN(exceptArrayBytes);
    structureSize += TCL_ALIGN(auxDataArrayBytes);
    structureSize += cmdLocBytes;

    unsigned char *base    = (unsigned char *)Tcl_Alloc(structureSize);
    ByteCode      *codePtr = (ByteCode *)base;

    /* ----- Header & environment capture (same fields as core) ----- */
    memset(codePtr, 0, sizeof(ByteCode));
    codePtr->interpHandle    = TclHandlePreserve(iPtr->handle);
    codePtr->compileEpoch    = iPtr->compileEpoch;
    codePtr->nsPtr           = nsPtr;
    codePtr->nsEpoch         = nsPtr->resolverEpoch;

    /* *** Inline TclPreserveByteCode(codePtr) *** */
    codePtr->refCount        = 1; /* brand-new ByteCode held by this one Tcl_Obj */

    /* Flags: precompiled image; also request variable resolution at first run */
    codePtr->flags           = TCL_BYTECODE_PRECOMPILED | ((nsPtr->compiledVarResProc || iPtr->resolverPtr) ? TCL_BYTECODE_RESOLVE_VARS : 0);

    codePtr->source          = NULL;         /* no retained source for precompiled */
    codePtr->procPtr         = env->procPtr; /* may be NULL for top-level blocks   */

    /* Counts */
    codePtr->numCommands     = 0; /* no cmd-location map    */
    codePtr->numSrcBytes     = 0; /* not tracked in stream  */
    codePtr->numCodeBytes    = (Tcl_Size)codeBytes;
    codePtr->numLitObjects   = (Tcl_Size)env->numLitObjects;
    codePtr->numExceptRanges = (Tcl_Size)env->numExceptRanges;
    codePtr->numAuxDataItems = (Tcl_Size)env->numAuxDataItems;
    codePtr->numCmdLocBytes  = 0;
    codePtr->maxExceptDepth  = TCL_INDEX_NONE;
    codePtr->maxStackDepth   = (Tcl_Size)env->maxStackDepth;

    /* ----- Pack the variable-length tails with the same alignment steps ---- */
    unsigned char *p         = base + TCL_ALIGN(sizeof(ByteCode));

    /* 1) Code bytes */
    codePtr->codeStart       = p;
    if (codeBytes)
        memcpy(p, env->codeStart, codeBytes);
    p += TCL_ALIGN(codeBytes);

    /* 2) Literal object array */
    codePtr->objArrayPtr = (Tcl_Obj **)p;
    if (env->numLitObjects) {
        for (int i = 0; i < env->numLitObjects; i++) {
            Tcl_Obj *lit            = env->objArrayPtr[i];
            codePtr->objArrayPtr[i] = lit;
            if (lit)
                Tcl_IncrRefCount(lit);
        }
    }
    p += TCL_ALIGN(objArrayBytes);

    /* 3) Exception ranges */
    codePtr->exceptArrayPtr = (ExceptionRange *)p;
    if (env->numExceptRanges) {
        memcpy(p, env->exceptArrayPtr, exceptArrayBytes);
    }
    p += TCL_ALIGN(exceptArrayBytes);

    /* 4) AuxData array */
    codePtr->auxDataArrayPtr = (AuxData *)p;
    if (env->numAuxDataItems) {
        memcpy(p, env->auxDataArrayPtr, auxDataArrayBytes);
    }
    p += TCL_ALIGN(auxDataArrayBytes);

    /* 5) No command-location map: point all cursors to the tail */
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

    return codePtr;
}

static Tcl_Obj *TBCX_NewByteCodeObjFromParts(Tcl_Interp *ip, Namespace *nsPtr, const unsigned char *code, uint32_t codeLen, Tcl_Obj **lits, uint32_t numLits, AuxData *auxArr, uint32_t numAux,
                                             ExceptionRange *exArr, uint32_t numEx, int maxStackDepth) {
    Tcl_Obj           *bcObj = NULL;
    /* allocate and copy buffers for the transient env */
    TBCX_CompileEnvMin env;
    memset(&env, 0, sizeof(env));
    env.interp        = ip;
    env.nsPtr         = (Namespace *)nsPtr;
    env.maxStackDepth = maxStackDepth;
    env.procPtr       = NULL;

    env.codeStart     = (unsigned char *)Tcl_Alloc(codeLen ? codeLen : 1u);
    if (codeLen)
        memcpy(env.codeStart, code, codeLen);
    env.codeNext      = env.codeStart + codeLen;
    env.codeEnd       = env.codeNext;

    env.objArrayPtr   = NULL;
    env.numLitObjects = (int)numLits;
    if (numLits) {
        env.objArrayPtr = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numLits);
        for (uint32_t i = 0; i < numLits; i++) {
            env.objArrayPtr[i] = lits[i];
            if (env.objArrayPtr[i])
                Tcl_IncrRefCount(env.objArrayPtr[i]);
        }
    }

    env.auxDataArrayPtr = NULL;
    env.numAuxDataItems = (int)numAux;
    if (numAux) {
        env.auxDataArrayPtr = (AuxData *)Tcl_Alloc(sizeof(AuxData) * numAux);
        for (uint32_t i = 0; i < numAux; i++)
            env.auxDataArrayPtr[i] = auxArr[i];
    }

    env.exceptArrayPtr  = NULL;
    env.numExceptRanges = (int)numEx;
    if (numEx) {
        env.exceptArrayPtr = (ExceptionRange *)Tcl_Alloc(sizeof(ExceptionRange) * numEx);
        for (uint32_t i = 0; i < numEx; i++)
            env.exceptArrayPtr[i] = exArr[i];
    }

    bcObj = Tcl_NewObj();
    /* Store a bytecode internal rep by asking the core to initialize from env */
    (void)TbcxInitByteCodeObj(bcObj, tbcxTyBytecode, &env);

    /* Drop our transient copies (ByteCode now owns its copies) */
    if (env.objArrayPtr) {
        for (int i = 0; i < env.numLitObjects; i++)
            if (env.objArrayPtr[i])
                Tcl_DecrRefCount(env.objArrayPtr[i]);
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

/* Ensure the CompiledLocal linked-list length matches 'neededCount' by
 * appending placeholder locals (empty names, no defaults) for non-argument
 * locals referenced by the compiled bytecode. This keeps the invariant that
 * the list contains exactly 'numCompiledLocals' nodes, which Tcl assumes
 * when freeing a Proc on redefinition.
 */
static void TbcxExtendCompiledLocals(Proc *procPtr, int neededCount) {
    if (!procPtr)
        return;
    if (neededCount <= procPtr->numCompiledLocals)
        return;

    CompiledLocal *first = procPtr->firstLocalPtr;
    CompiledLocal *last  = procPtr->lastLocalPtr;

    for (int i = procPtr->numCompiledLocals; i < neededCount; i++) {
        CompiledLocal *cl = (CompiledLocal *)Tcl_Alloc(offsetof(CompiledLocal, name) + 1u);
        memset(cl, 0, sizeof(CompiledLocal));
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

/* Install one precompiled method/ctor/dtor into OO registry */
static int ReadOneMethodAndRegister(TbcxIn *r, Tcl_Interp *ip, OOShim *os) {
    /* classFqn */
    char    *clsf = NULL;
    uint32_t clsL = 0;
    if (!R_LPString(r, &clsf, &clsL))
        return TCL_ERROR;
    Tcl_Obj *clsFqn = Tcl_NewStringObj(clsf, (Tcl_Size)clsL);
    Tcl_Free(clsf);
    /* kind */
    uint8_t kind = 0;
    if (!R_U8(r, &kind))
        return TCL_ERROR;
    /* name (empty for ctor/dtor) */
    char    *mname = NULL;
    uint32_t mnL   = 0;
    if (!R_LPString(r, &mname, &mnL))
        return TCL_ERROR;
    Tcl_Obj *nameObj = Tcl_NewStringObj(mname, (Tcl_Size)mnL);
    Tcl_Free(mname);
    /* args text */
    char    *args = NULL;
    uint32_t aL   = 0;
    if (!R_LPString(r, &args, &aL)) {
        Tcl_DecrRefCount(nameObj);
        return TCL_ERROR;
    }
    Tcl_Obj *argsObj = Tcl_NewStringObj(args, (Tcl_Size)aL);
    Tcl_Free(args);
    /* bodyTextLen (ignored) */
    uint32_t bodyTextLen = 0;
    if (!R_U32(r, &bodyTextLen)) {
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        return TCL_ERROR;
    }

    /* compiled block (namespace default: class namespace) + receive numLocals */
    Namespace *clsNs  = (Namespace *)TBCX_EnsureNamespace(ip, Tcl_GetString(clsFqn));
    uint32_t   nLoc   = 0;
    Tcl_Obj   *bodyBC = ReadCompiledBlock(r, ip, clsNs, &nLoc);
    if (!bodyBC) {
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        return TCL_ERROR;
    }

    /* Build Proc + compiled locals from argsObj */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr  = bodyBC;
    Tcl_IncrRefCount(bodyBC);
    Tcl_Size  argc = 0;
    Tcl_Obj **argv = NULL;
    if (Tcl_ListObjGetElements(ip, argsObj, &argc, &argv) != TCL_OK)
        return TCL_ERROR;
    procPtr->numArgs           = (int)argc;
    procPtr->numCompiledLocals = (int)argc;
    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nf = 0, nmLen = 0;
        Tcl_Obj **fv = NULL;
        if (Tcl_ListObjGetElements(ip, argv[i], &nf, &fv) != TCL_OK || nf < 1 || nf > 2)
            return TCL_ERROR;
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
    TbcxExtendCompiledLocals(procPtr, (int)nLoc);

    /* Build procbody and register */
    Tcl_Obj           *procBodyObj = Tcl_NewObj();
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = procPtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
    procPtr->refCount++;

    /* Link ByteCode back to this Proc (parity with core procs) */
    {
        ByteCode *bcPtr = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bcPtr);
        if (bcPtr)
            bcPtr->procPtr = procPtr;
    }

    Tcl_Obj       *key   = BuildMethodKey(ip, clsFqn, kind, (mnL ? nameObj : NULL));
    int            isNew = 0;
    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&os->methodsByKey, (const char *)key, &isNew);
    /* Store PAIR {argsObj, procBodyObj} so we can verify signature at shim time */
    Tcl_Obj       *pair  = Tcl_NewListObj(0, NULL);
    Tcl_ListObjAppendElement(ip, pair, argsObj);
    Tcl_ListObjAppendElement(ip, pair, procBodyObj);
    Tcl_IncrRefCount(pair);
    Tcl_SetHashValue(he, pair);
    Tcl_DecrRefCount(key);
    /* keep key & value alive in the table */
    Tcl_DecrRefCount(nameObj);
    Tcl_DecrRefCount(clsFqn);
    return TCL_OK;
}

static Tcl_Obj *ReadLiteral(TbcxIn *r, Tcl_Interp *ip) {
    uint32_t tag = 0;
    if (!R_U32(r, &tag))
        return NULL;

    switch (tag) {
    case TBCX_LIT_BIGNUM: {
        uint8_t  sign   = 0;
        uint32_t magLen = 0;
        if (!R_U8(r, &sign))
            return NULL;
        if (!R_U32(r, &magLen))
            return NULL;
        if (magLen > (64u * 1024u * 1024u)) {
            R_Error(r, "tbcx: bignum too large");
            return NULL;
        }
        Tcl_Obj *o = NULL;
        if (magLen == 0 || sign == 0) {
            o = Tcl_NewWideIntObj(0);
        } else {
            /* Build big integer from little-endian magnitude */
            unsigned char *le = (unsigned char *)Tcl_Alloc(magLen);
            if (!R_Bytes(r, le, magLen)) {
                Tcl_Free((char *)le);
                return NULL;
            }
            /* Convert LE -> MP (tommath) with error checks */
            mp_int z;
            mp_err mrc = TclBN_mp_init(&z);
            if (mrc != MP_OKAY) {
                Tcl_Free((char *)le);
                R_Error(r, "tbcx: bignum init");
                return NULL;
            }
#if defined(MP_HAS_FROM_UBIN)
            unsigned char *be = (unsigned char *)Tcl_Alloc(magLen);
            for (uint32_t i = 0; i < magLen; i++)
                be[i] = le[magLen - 1 - i];
            mrc = TclBN_mp_from_ubin(&z, be, magLen);
            Tcl_Free((char *)be);
#elif defined(MP_HAS_READ_UNSIGNED_BIN)
            /* TomMath accepts big-endian; reverse to BE, then import */
            unsigned char *be = (unsigned char *)Tcl_Alloc(magLen);
            for (uint32_t i = 0; i < magLen; i++)
                be[i] = le[magLen - 1 - i];
            mrc = TclBN_mp_read_unsigned_bin(&z, be, magLen);
            Tcl_Free((char *)be);
#else
            /* Fallback: shift/add per byte (most portable) */
            for (int i = (int)magLen - 1; i >= 0; i--) {
                if ((mrc = TclBN_mp_mul_2d(&z, 8, &z)) != MP_OKAY)
                    break;
                if ((mrc = TclBN_mp_add_d(&z, le[i], &z)) != MP_OKAY)
                    break;
            }
#endif
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
    case TBCX_LIT_BOOLEAN: {
        uint8_t b = 0;
        if (!R_U8(r, &b))
            return NULL;
        return Tcl_NewBooleanObj(b ? 1 : 0);
    }
    case TBCX_LIT_BYTEARR: {
        uint32_t n = 0;
        if (!R_U32(r, &n))
            return NULL;
        unsigned char *buf = (unsigned char *)Tcl_Alloc(n);
        if (n && !R_Bytes(r, buf, n)) {
            Tcl_Free((char *)buf);
            return NULL;
        }
        Tcl_Obj *o = Tcl_NewByteArrayObj(buf, n);
        Tcl_Free((char *)buf);
        return o;
    }
    case TBCX_LIT_DICT: {
        uint32_t cnt = 0;
        if (!R_U32(r, &cnt))
            return NULL;
        Tcl_Obj *d = Tcl_NewDictObj();
        for (uint32_t i = 0; i < cnt; i++) {
            Tcl_Obj *k = ReadLiteral(r, ip);
            Tcl_Obj *v = ReadLiteral(r, ip);
            if (!k || !v)
                return NULL;
            Tcl_DictObjPut(ip, d, k, v);
        }
        return d;
    }
    case TBCX_LIT_DOUBLE: {
        uint64_t bits = 0;
        if (!R_U64(r, &bits))
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
        if (!R_U32(r, &n))
            return NULL;
        Tcl_Obj *lst = Tcl_NewListObj(0, NULL);
        for (uint32_t i = 0; i < n; i++) {
            Tcl_Obj *e = ReadLiteral(r, ip);
            if (!e)
                return NULL;
            Tcl_ListObjAppendElement(ip, lst, e);
        }
        return lst;
    }
    case TBCX_LIT_WIDEINT: {
        uint64_t u = 0;
        if (!R_U64(r, &u))
            return NULL;
        /* stored as 2's complement */
        Tcl_WideInt wi = (Tcl_WideInt)u;
        return Tcl_NewWideIntObj(wi);
    }
    case TBCX_LIT_WIDEUINT: {
        uint64_t u = 0;
        if (!R_U64(r, &u))
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
#if defined(MP_HAS_READ_UNSIGNED_BIN)
            unsigned char be[8];
            for (int i = 0; i < 8; i++)
                be[i] = (unsigned char)((u >> (8 * (7 - i))) & 0xFF);
            mrc = TclBN_mp_read_unsigned_bin(&z, be, 8);
#else
            /* Portable fallback: build from 8 bytes with shift/add so we can
               keep checking mp_err on each TomMath call. */
            for (int i = 7; i >= 0; i--) {
                mrc = TclBN_mp_mul_2d(&z, 8, &z);
                if (mrc != MP_OKAY)
                    break;
                mrc = TclBN_mp_add_d(&z, (unsigned int)((u >> (8 * i)) & 0xFFu), &z);
                if (mrc != MP_OKAY)
                    break;
            }
#endif
            if (mrc != MP_OKAY) {
                TclBN_mp_clear(&z);
                R_Error(r, "tbcx: wideuint import");
                return NULL;
            }
            Tcl_Obj *o = Tcl_NewBignumObj(&z);
            return o;
        }
    }
    case TBCX_LIT_BYTECODE: {
        /* nsFQN string then compiled block */
        char    *nsStr = NULL;
        uint32_t nsLen = 0;
        if (!R_LPString(r, &nsStr, &nsLen))
            return NULL;
        Tcl_Obj *nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
        Tcl_Free(nsStr);
        Namespace *nsPtr = (Namespace *)TBCX_EnsureNamespace(ip, Tcl_GetString(nsObj));
        Tcl_IncrRefCount(nsObj);
        uint32_t dummyNL = 0;
        Tcl_Obj *bc      = ReadCompiledBlock(r, ip, nsPtr, &dummyNL);
        Tcl_DecrRefCount(nsObj);
        return bc;
    }
    case TBCX_LIT_LAMBDA_BC: {
        /* nsFQN, numArgs, args (name + optional default literal), then body block */
        char    *nsStr = NULL;
        uint32_t nsLen = 0;
        if (!R_LPString(r, &nsStr, &nsLen))
            return NULL;
        Tcl_Obj *nsObj = Tcl_NewStringObj(nsStr, (Tcl_Size)nsLen);
        Tcl_Free(nsStr);
        Namespace *nsPtr   = (Namespace *)TBCX_EnsureNamespace(ip, Tcl_GetString(nsObj));
        uint32_t   numArgs = 0;
        if (!R_U32(r, &numArgs)) {
            Tcl_DecrRefCount(nsObj);
            return NULL;
        }

        Tcl_Obj *argList = Tcl_NewListObj(0, NULL);
        /* Build a Proc for the lambda */
        Proc    *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
        memset(procPtr, 0, sizeof(Proc));
        procPtr->iPtr              = (Interp *)ip;
        procPtr->refCount          = 1;
        procPtr->numArgs           = (int)numArgs;
        procPtr->numCompiledLocals = (int)numArgs;

        CompiledLocal *first = NULL, *last = NULL;
        for (uint32_t i = 0; i < numArgs; i++) {
            char    *nameC   = NULL;
            uint32_t nameLen = 0;
            if (!R_LPString(r, &nameC, &nameLen)) {
                Tcl_DecrRefCount(nsObj);
                R_Error(r, "tbcx: arg decode");
                return NULL;
            }
            uint8_t hasDef = 0;
            if (!R_U8(r, &hasDef)) {
                Tcl_Free(nameC);
                Tcl_DecrRefCount(nsObj);
                R_Error(r, "tbcx: arg flag");
                return NULL;
            }

            Tcl_Obj *argSpec = Tcl_NewListObj(0, NULL);
            Tcl_ListObjAppendElement(ip, argSpec, Tcl_NewStringObj(nameC, (Tcl_Size)nameLen));
            Tcl_Free(nameC);
            Tcl_Obj *defVal = NULL;
            if (hasDef) {
                defVal = ReadLiteral(r, ip);
                if (!defVal) {
                    Tcl_DecrRefCount(argSpec);
                    Tcl_DecrRefCount(nsObj);
                    R_Error(r, "tbcx: arg default");
                    return NULL;
                }
                Tcl_ListObjAppendElement(ip, argSpec, defVal);
            }
            Tcl_ListObjAppendElement(ip, argList, argSpec);

            /* Create compiled-local node (correct list/index handling) */
            Tcl_Obj *nameObj = NULL;
            if (Tcl_ListObjIndex(ip, argSpec, 0, &nameObj) != TCL_OK) {
                if (defVal)
                    Tcl_DecrRefCount(defVal);
                Tcl_DecrRefCount(argSpec);
                Tcl_DecrRefCount(nsObj);
                R_Error(r, "tbcx: arg spec");
                return NULL;
            }
            Tcl_Size       nmLen = 0;
            const char    *nm    = Tcl_GetStringFromObj(nameObj, &nmLen);
            CompiledLocal *cl    = (CompiledLocal *)Tcl_Alloc(offsetof(CompiledLocal, name) + 1u + (size_t)nmLen);
            memset(cl, 0, sizeof(CompiledLocal));
            cl->nameLength = (int)nmLen;
            memcpy(cl->name, nm, (size_t)nmLen + 1);
            cl->frameIndex = (int)i;
            cl->flags      = VAR_ARGUMENT;
            if (defVal) {
                cl->defValuePtr = defVal;
                Tcl_IncrRefCount(defVal);
            }
            if (i + 1 == numArgs && nmLen == 4 && memcmp(nm, "args", 4) == 0)
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

        /* Body compiled block (+numLocals captured from stream) */
        uint32_t nLocalsBody   = 0;
        Tcl_Obj *bodyBC        = ReadCompiledBlock(r, ip, nsPtr, &nLocalsBody);
        procPtr->bodyPtr       = bodyBC;
        Tcl_IncrRefCount(bodyBC);
        TbcxExtendCompiledLocals(procPtr, (int)nLocalsBody);

        /* Build lambdaExpr object (internal rep: twoPtrValue { Proc*, nsObjPtr }) */
        Tcl_Obj           *lambda = Tcl_NewObj();
        Tcl_ObjInternalRep ir;
        ir.twoPtrValue.ptr1 = procPtr;
        ir.twoPtrValue.ptr2 = nsObj;
        Tcl_IncrRefCount(nsObj); /* keep nsObj alive while lambda exists */
        Tcl_StoreInternalRep(lambda, tbcxTyLambda, &ir);
        return lambda;
    }
    case TBCX_LIT_STRING: {
        char    *s = NULL;
        uint32_t n = 0;
        if (!R_LPString(r, &s, &n))
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

static int ReadAuxArray(TbcxIn *r, Tcl_Interp *ip, AuxData **auxOut, uint32_t *numAuxOut) {
    uint32_t n = 0;
    if (!R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_AUX) {
        R_Error(r, "tbcx: aux too many");
        return 0;
    }
    AuxData *arr = NULL;
    if (n)
        arr = (AuxData *)Tcl_Alloc(sizeof(AuxData) * n);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t tag = 0;
        if (!R_U32(r, &tag)) {
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
        if (tag == TBCX_AUX_JT_STR) {
            /* u32 cnt, then cnt × (LPString key, u32 pcOffset) */
            uint32_t cnt = 0;
            if (!R_U32(r, &cnt)) {
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
            JumptableInfo *info = (JumptableInfo *)Tcl_Alloc(sizeof(*info));
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
            for (uint32_t k = 0; k < cnt; k++) {
                char    *s  = NULL;
                uint32_t sl = 0;
                if (!R_LPString(r, &s, &sl))
                    return 0;
                uint32_t off = 0;
                if (!R_U32(r, &off)) {
                    Tcl_Free(s);
                    return 0;
                }
                /* IMPORTANT: Tcl does not copy string keys for TCL_STRING_KEYS tables.
                 * We must keep a stable, heap-allocated copy alive as long as the table lives. */
                char *stable = (char *)Tcl_Alloc(sl + 1u);
                memcpy(stable, s, sl);
                stable[sl] = '\0';
                Tcl_Free(s); /* free transient buffer from R_LPString */
                int            newEntry = 0;
                Tcl_HashEntry *he       = Tcl_CreateHashEntry(&info->hashTable, (const char *)stable, &newEntry);
                Tcl_SetHashValue(he, INT2PTR((int)off));
            }
            arr[i].type       = tbcxAuxJTStr;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_JT_NUM) {
            uint32_t cnt = 0;
            if (!R_U32(r, &cnt)) {
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
            JumptableNumInfo *info = (JumptableNumInfo *)Tcl_Alloc(sizeof(*info));
            Tcl_InitHashTable(&info->hashTable, TCL_ONE_WORD_KEYS);
            for (uint32_t k = 0; k < cnt; k++) {
                uint64_t key = 0;
                uint32_t off = 0;
                if (!R_U64(r, &key) || !R_U32(r, &off)) {
                    return 0;
                }
                int            newE = 0;
                Tcl_HashEntry *he   = Tcl_CreateHashEntry(&info->hashTable, (const char *)(intptr_t)key, &newE);
                Tcl_SetHashValue(he, INT2PTR((int)off));
            }
            arr[i].type       = tbcxAuxJTNum;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_DICTUPD) {
            uint32_t L = 0;
            if (!R_U32(r, &L)) {
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
            size_t          bytes = sizeof(DictUpdateInfo) + (L ? (L - 1u) * sizeof(Tcl_Size) : 0);
            DictUpdateInfo *info  = (DictUpdateInfo *)Tcl_Alloc(bytes);
            info->length          = (Tcl_Size)L;
            for (uint32_t k = 0; k < L; k++) {
                uint32_t x = 0;
                if (!R_U32(r, &x))
                    return 0;
                info->varIndices[k] = (Tcl_Size)x;
            }
            arr[i].type       = tbcxAuxDictUpdate;
            arr[i].clientData = info;
        } else if (tag == TBCX_AUX_NEWFORE || tag == TBCX_AUX_FOREACH) {
            uint32_t numLists = 0, loopCt = 0, firstVal = 0, dupNumLists = 0;
            if (!R_U32(r, &numLists) || !R_U32(r, &loopCt) || !R_U32(r, &firstVal) || !R_U32(r, &dupNumLists)) {
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
            if (dupNumLists != numLists) {
                R_Error(r, "tbcx: foreach aux mismatch");
                if (arr)
                    Tcl_Free(arr);
                return 0;
            }
            size_t       bytes   = sizeof(ForeachInfo) + (numLists ? (numLists - 1u) * sizeof(ForeachVarList *) : 0);
            ForeachInfo *info    = (ForeachInfo *)Tcl_Alloc(bytes);
            info->numLists       = (Tcl_Size)numLists;
            info->firstValueTemp = (Tcl_LVTIndex)firstVal;
            info->loopCtTemp     = (Tcl_LVTIndex)loopCt;
            for (uint32_t iL = 0; iL < numLists; iL++) {
                uint32_t nv = 0;
                if (!R_U32(r, &nv))
                    return 0;
                size_t          vlBytes = sizeof(ForeachVarList) + (nv ? (nv - 1u) * sizeof(Tcl_LVTIndex) : 0);
                ForeachVarList *vl      = (ForeachVarList *)Tcl_Alloc(vlBytes);
                vl->numVars             = (Tcl_Size)nv;
                for (uint32_t j = 0; j < nv; j++) {
                    uint32_t idx = 0;
                    if (!R_U32(r, &idx))
                        return 0;
                    vl->varIndexes[j] = (Tcl_LVTIndex)idx;
                }
                info->varLists[iL] = vl;
            }
            /* Choose matching AuxDataType pointer for this tag. Prefer the name that matches the tag if present. */
            const AuxDataType *ty = NULL;
            if (tag == TBCX_AUX_FOREACH) {
                ty = tbcxAuxForeach ? tbcxAuxForeach : tbcxAuxNewForeach;
            } else { /* TBCX_AUX_NEWFORE */
                ty = tbcxAuxNewForeach ? tbcxAuxNewForeach : tbcxAuxForeach;
            }
            arr[i].type       = ty;
            arr[i].clientData = info;
        } else {
            R_Error(r, "tbcx: unsupported AuxData tag");
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
    }
    *auxOut    = arr;
    *numAuxOut = n;
    return 1;
}

static int ReadExceptions(TbcxIn *r, ExceptionRange **exOut, uint32_t *numOut) {
    uint32_t n = 0;
    if (!R_U32(r, &n))
        return 0;
    if (n > TBCX_MAX_EXCEPT) {
        R_Error(r, "tbcx: too many exceptions");
        return 0;
    }
    ExceptionRange *arr = NULL;
    if (n)
        arr = (ExceptionRange *)Tcl_Alloc(sizeof(ExceptionRange) * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t type32 = 0, len = 0;
        uint32_t nesting = 0, from = 0, cont = 0, brk = 0, cat = 0;
        if (!R_U32(r, &type32) || !R_U32(r, &nesting) || !R_U32(r, &from) || !R_U32(r, &len) || !R_U32(r, &cont) || !R_U32(r, &brk) || !R_U32(r, &cat)) {
            if (arr)
                Tcl_Free(arr);
            return 0;
        }
        /* Saver writes 32-bit type; keep only low 8 bits for the enum. */
        arr[i].type           = (ExceptionRangeType)(uint8_t)type32;
        arr[i].nestingLevel   = (int)nesting;
        arr[i].codeOffset     = (int)from;
        arr[i].numCodeBytes   = (int)len;
        arr[i].continueOffset = (int)cont;
        arr[i].breakOffset    = (int)brk;
        arr[i].catchOffset    = (int)cat;
    }
    *exOut  = arr;
    *numOut = n;
    return 1;
}

static Tcl_Obj *ReadCompiledBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut) {
    /* 1) code */
    uint32_t codeLen = 0;
    if (!R_U32(r, &codeLen))
        return NULL;
    if (codeLen > TBCX_MAX_CODE) {
        R_Error(r, "tbcx: code too large");
        return NULL;
    }
    unsigned char *code = (unsigned char *)Tcl_Alloc(codeLen ? codeLen : 1u);
    if (codeLen && !R_Bytes(r, code, codeLen)) {
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 2) literals */
    uint32_t numLits = 0;
    if (!R_U32(r, &numLits)) {
        Tcl_Free((char *)code);
        return NULL;
    }
    if (numLits > TBCX_MAX_LITERALS) {
        R_Error(r, "tbcx: too many literals");
        Tcl_Free((char *)code);
        return NULL;
    }
    Tcl_Obj **lits = NULL;
    if (numLits)
        lits = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numLits);
    for (uint32_t i = 0; i < numLits; i++) {
        Tcl_Obj *lit = ReadLiteral(r, ip);
        if (!lit) {
            if (lits)
                Tcl_Free(lits);
            Tcl_Free((char *)code);
            return NULL;
        }
        lits[i] = lit;
    }

    /* 3) AuxData */
    AuxData *auxArr = NULL;
    uint32_t numAux = 0;
    if (!ReadAuxArray(r, ip, &auxArr, &numAux)) {
        if (lits)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 4) Exceptions */
    ExceptionRange *exArr = NULL;
    uint32_t        numEx = 0;
    if (!ReadExceptions(r, &exArr, &numEx)) {
        if (auxArr)
            Tcl_Free(auxArr);
        if (lits)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    /* 5) Epilogue: maxStack, reserved, numLocals */
    uint32_t maxStack = 0, reserved = 0, numLocals = 0;
    if (!R_U32(r, &maxStack) || !R_U32(r, &reserved) || !R_U32(r, &numLocals)) {
        if (exArr)
            Tcl_Free(exArr);
        if (auxArr)
            Tcl_Free(auxArr);
        if (lits)
            Tcl_Free(lits);
        Tcl_Free((char *)code);
        return NULL;
    }

    if (numLocalsOut)
        *numLocalsOut = numLocals;

    /* Build bytecode object */
    Tcl_Obj *bc = TBCX_NewByteCodeObjFromParts(ip, nsForDefault, code, codeLen, lits, numLits, auxArr, numAux, exArr, numEx, (int)maxStack);

    if (exArr)
        Tcl_Free(exArr);
    if (auxArr)
        Tcl_Free(auxArr);
    if (lits)
        Tcl_Free(lits);
    if (code)
        Tcl_Free(code);
    return bc;
}

static int Tbcx_ProcShimObjCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    ProcShim *ps = (ProcShim *)cd;
    if (objc != 4) {
        /* Forward to the original “proc” by name (renamed below) */
        Tcl_Obj *argv0 = Tcl_NewStringObj("::tbcx::__proc_orig__", -1);
        Tcl_IncrRefCount(argv0);
        /* Avoid VLA (not portable); allocate on heap */
        Tcl_Obj **argv2 = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)objc);
        argv2[0]        = argv0;
        for (Tcl_Size i = 1; i < objc; i++) {
            argv2[i] = objv[i];
        }
        int rc = Tcl_EvalObjv(ip, objc, argv2, TCL_EVAL_GLOBAL);
        Tcl_Free(argv2);
        Tcl_DecrRefCount(argv0);
        return rc;
    }
    Tcl_Obj    *nameObj = objv[1], *argsObj = objv[2];
    /* Compute FQN: if name starts with "::", use as-is; else current ns */
    const char *nm  = Tcl_GetString(nameObj);
    Tcl_Obj    *fqn = NULL;
    if (nm[0] == ':' && nm[1] == ':') {
        fqn = nameObj;
    } else {
        Tcl_Namespace *cur     = Tcl_GetCurrentNamespace(ip);
        const char    *curName = cur ? cur->fullName : "::";
        fqn                    = Tcl_NewStringObj(curName, -1);
        /* Avoid “::::name” when in global namespace */
        if (!(curName[0] == ':' && curName[1] == ':' && curName[2] == '\0')) {
            Tcl_AppendToObj(fqn, "::", 2);
        }
        Tcl_AppendObjToObj(fqn, nameObj);
    }
    Tcl_HashEntry *he = Tcl_FindHashEntry(&ps->procsByFqn, (const char *)fqn);
    if (he) {
        /* Value is PAIR {savedArgs, procbody} */
        Tcl_Obj *pair      = (Tcl_Obj *)Tcl_GetHashValue(he);
        Tcl_Obj *savedArgs = NULL, *procBody = NULL;
        if (Tcl_ListObjIndex(ip, pair, 0, &savedArgs) != TCL_OK || Tcl_ListObjIndex(ip, pair, 1, &procBody) != TCL_OK) {
            if (fqn != nameObj)
                Tcl_DecrRefCount(fqn);
            /* Fall through to original */
        } else {
            /* Compare args signature byte-wise (canonical list eq is fine for now) */
            Tcl_Size    aLen = 0, bLen = 0;
            const char *a     = Tcl_GetStringFromObj(argsObj, &aLen);
            const char *b     = Tcl_GetStringFromObj(savedArgs, &bLen);
            int         match = (aLen == bLen) && (memcmp(a, b, (size_t)aLen) == 0);
            if (match) {
                Tcl_IncrRefCount(procBody);
                Tcl_Obj *argv0 = Tcl_NewStringObj("::tbcx::__proc_orig__", -1);
                Tcl_IncrRefCount(argv0);
                Tcl_Obj *argv[4] = {argv0, nameObj, argsObj, procBody};
                int      rc      = Tcl_EvalObjv(ip, 4, argv, TCL_EVAL_GLOBAL);
                Tcl_DecrRefCount(argv0);
                Tcl_DecrRefCount(procBody);
                if (fqn != nameObj)
                    Tcl_DecrRefCount(fqn);
                return rc;
            }
        }
    }
    /* forward to the original “proc” as well (avoid recursion) */
    Tcl_Obj *argv0 = Tcl_NewStringObj("::tbcx::__proc_orig__", -1);
    Tcl_IncrRefCount(argv0);
    Tcl_Obj *argv[4] = {argv0, nameObj, argsObj, objv[3]};
    int      rc      = Tcl_EvalObjv(ip, 4, argv, TCL_EVAL_GLOBAL);
    Tcl_DecrRefCount(argv0);
    if (fqn != nameObj)
        Tcl_DecrRefCount(fqn);
    return rc;
}

static int InstallProcShim(Tcl_Interp *ip, ProcShim *ps) {
    memset(ps, 0, sizeof(*ps));
    Tcl_InitObjHashTable(&ps->procsByFqn);

    Tcl_Command orig = Tcl_FindCommand(ip, "proc", NULL, 0);
    if (!orig)
        return TCL_ERROR;

    if (TclRenameCommand(ip, "proc", "::tbcx::__proc_orig__") != TCL_OK) {
        return TCL_ERROR;
    }

    Tcl_CreateObjCommand2(ip, "proc", Tbcx_ProcShimObjCmd, ps, NULL);
    return TCL_OK;
}

static void UninstallProcShim(Tcl_Interp *ip, ProcShim *ps) {
    Tcl_DeleteCommand(ip, "proc");
    (void)TclRenameCommand(ip, "::tbcx::__proc_orig__", "proc");
    Tcl_HashSearch s;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&ps->procsByFqn, &s); e; e = Tcl_NextHashEntry(&s)) {
        Tcl_Obj *val = (Tcl_Obj *)Tcl_GetHashValue(e);
        if (val)
            Tcl_DecrRefCount(val);
    }
    Tcl_DeleteHashTable(&ps->procsByFqn);
}

static int ReadHeader(TbcxIn *r, TbcxHeader *H) {
    if (!R_U32(r, &H->magic))
        return 0;
    if (!R_U32(r, &H->format))
        return 0;
    if (!R_U32(r, &H->tcl_version))
        return 0;
    if (!R_U64(r, &H->codeLenTop))
        return 0;
    if (!R_U32(r, &H->numCmdsTop))
        return 0;
    if (!R_U32(r, &H->numExceptTop))
        return 0;
    if (!R_U32(r, &H->numLitsTop))
        return 0;
    if (!R_U32(r, &H->numAuxTop))
        return 0;
    if (!R_U32(r, &H->numLocalsTop))
        return 0;
    if (!R_U32(r, &H->maxStackTop))
        return 0;

    if (H->magic != TBCX_MAGIC || H->format != TBCX_FORMAT) {
        R_Error(r, "tbcx: bad header");
        return 0;
    }
    {
        uint32_t rt   = PackTclVersion();
        int      hMaj = (int)((H->tcl_version >> 24) & 0xFFu);
        int      hMin = (int)((H->tcl_version >> 16) & 0xFFu);
        int      rMaj = (int)((rt >> 24) & 0xFFu);
        int      rMin = (int)((rt >> 16) & 0xFFu);
        if (hMaj != rMaj || hMin > rMin) {
            R_Error(r, "tbcx: incompatible Tcl version");
            return 0;
        }
    }
    return 1;
}

static int ReadOneProcAndRegister(TbcxIn *r, Tcl_Interp *ip, ProcShim *shim) {
    /* FQN, ns, args text */
    char    *nameC = NULL;
    uint32_t nameL = 0;
    char    *nsC   = NULL;
    uint32_t nsL   = 0;
    char    *argsC = NULL;
    uint32_t argsL = 0;
    if (!R_LPString(r, &nameC, &nameL))
        return TCL_ERROR;
    if (!R_LPString(r, &nsC, &nsL)) {
        Tcl_Free(nameC);
        return TCL_ERROR;
    }
    if (!R_LPString(r, &argsC, &argsL)) {
        Tcl_Free(nameC);
        Tcl_Free(nsC);
        return TCL_ERROR;
    }

    Tcl_Obj *nameFqn = Tcl_NewStringObj(nameC, (Tcl_Size)nameL);
    Tcl_Obj *nsObj   = Tcl_NewStringObj(nsC, (Tcl_Size)nsL);
    Tcl_Obj *argsObj = Tcl_NewStringObj(argsC, (Tcl_Size)argsL);
    Tcl_Free(nameC);
    Tcl_Free(nsC);
    Tcl_Free(argsC);

    Namespace *nsPtr  = (Namespace *)TBCX_EnsureNamespace(ip, Tcl_GetString(nsObj));
    /* body block (+numLocals) */
    uint32_t   nLoc   = 0;
    Tcl_Obj   *bodyBC = ReadCompiledBlock(r, ip, nsPtr, &nLoc);
    if (!bodyBC)
        return TCL_ERROR;

    /* Build canonical FQN key from ns + name (unless name is already absolute) */
    Tcl_Obj    *fqnKey = NULL;
    const char *nm     = Tcl_GetString(nameFqn);
    if (nm[0] == ':' && nm[1] == ':') {
        fqnKey = nameFqn; /* hash table will hold a reference */
    } else {
        Tcl_Size    nsLen = 0;
        const char *nsStr = Tcl_GetStringFromObj(nsObj, &nsLen);
        fqnKey            = Tcl_NewStringObj(nsStr, nsLen);
        if (!(nsLen == 2 && nsStr[0] == ':' && nsStr[1] == ':')) {
            Tcl_AppendToObj(fqnKey, "::", 2);
        }
        Tcl_AppendObjToObj(fqnKey, nameFqn);
        /* nameFqn was a temporary in this case; free it */
        Tcl_DecrRefCount(nameFqn);
    }

    /* Build Proc and procbody Tcl_Obj that refers to it */
    Proc *procPtr = (Proc *)Tcl_Alloc(sizeof(Proc));
    memset(procPtr, 0, sizeof(Proc));
    procPtr->iPtr     = (Interp *)ip;
    procPtr->refCount = 1;
    procPtr->bodyPtr  = bodyBC;
    Tcl_IncrRefCount(bodyBC);

    /* Build compiled locals/args consistent with argsObj */
    Tcl_Size  argc = 0;
    Tcl_Obj **argv = NULL;
    if (Tcl_ListObjGetElements(ip, argsObj, &argc, &argv) != TCL_OK)
        return TCL_ERROR;
    procPtr->numArgs           = (int)argc;
    procPtr->numCompiledLocals = (int)argc;

    CompiledLocal *first = NULL, *last = NULL;
    for (Tcl_Size i = 0; i < argc; i++) {
        Tcl_Size  nFields = 0;
        Tcl_Obj **fields  = NULL;
        if (Tcl_ListObjGetElements(ip, argv[i], &nFields, &fields) != TCL_OK)
            return TCL_ERROR;
        if (nFields < 1 || nFields > 2) {
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: bad arg spec", -1));
            return TCL_ERROR;
        }
        Tcl_Size       nmLen = 0;
        const char    *nm    = Tcl_GetStringFromObj(fields[0], &nmLen);
        CompiledLocal *cl    = (CompiledLocal *)Tcl_Alloc(offsetof(CompiledLocal, name) + 1u + (size_t)nmLen);
        memset(cl, 0, sizeof(CompiledLocal));
        cl->nameLength = (int)nmLen;
        memcpy(cl->name, nm, (size_t)nmLen + 1);
        cl->frameIndex = (int)i;
        cl->flags      = VAR_ARGUMENT;
        if (nFields == 2) {
            cl->defValuePtr = fields[1];
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
    TbcxExtendCompiledLocals(procPtr, (int)nLoc);

    /* Create procbody Tcl_Obj and register under nameFqn */
    Tcl_Obj           *procBodyObj = Tcl_NewObj();
    Tcl_ObjInternalRep ir;
    ir.twoPtrValue.ptr1 = procPtr;
    ir.twoPtrValue.ptr2 = NULL;
    Tcl_StoreInternalRep(procBodyObj, tbcxTyProcBody, &ir);
    procPtr->refCount++;

    /* Link ByteCode back to this Proc (parity with core procs) */
    {
        ByteCode *bcPtr = NULL;
        ByteCodeGetInternalRep(bodyBC, tbcxTyBytecode, bcPtr);
        if (bcPtr)
            bcPtr->procPtr = procPtr;
    }

    /* Store PAIR {argsObj, procBodyObj} in shim registry (key & value refcounted) */
    Tcl_Obj *pair = Tcl_NewListObj(0, NULL);
    Tcl_ListObjAppendElement(ip, pair, argsObj);
    Tcl_ListObjAppendElement(ip, pair, procBodyObj);
    Tcl_IncrRefCount(pair);
    int            isNew = 0;

    Tcl_HashEntry *he    = Tcl_CreateHashEntry(&shim->procsByFqn, (const char *)fqnKey, &isNew);
    Tcl_SetHashValue(he, pair);
    /* Clean temporaries not stored in the registry */
    Tcl_DecrRefCount(nsObj);
    /* fqnKey is held by the hash table; do not drop it here */
    return TCL_OK;
}

static int LoadTbcxStream(Tcl_Interp *ip, Tcl_Channel ch) {
    TbcxIn     r = {ip, ch, TCL_OK};
    TbcxHeader H;

    if (CheckBinaryChan(ip, ch) != TCL_OK)
        return TCL_ERROR;

    if (!ReadHeader(&r, &H) || r.err)
        return TCL_ERROR;

    /* Top-level block (default ns: current) */
    Namespace *curNs   = (Namespace *)Tcl_GetCurrentNamespace(ip);
    uint32_t   dummyNL = 0;
    Tcl_Obj   *topBC   = ReadCompiledBlock(&r, ip, curNs, &dummyNL);
    if (!topBC)
        return TCL_ERROR;

    /* Procs */
    uint32_t numProcs = 0;
    if (!R_U32(&r, &numProcs))
        return TCL_ERROR;

    /* Build proc shim registry and fill from section */
    ProcShim shim;
    if (numProcs) {
        if (InstallProcShim(ip, &shim) != TCL_OK)
            return TCL_ERROR;
    }

    for (uint32_t i = 0; i < numProcs; i++) {
        if (ReadOneProcAndRegister(&r, ip, &shim) != TCL_OK) {
            UninstallProcShim(ip, &shim);
            return TCL_ERROR;
        }
    }

    /* Classes section (saver currently emits 0) */
    uint32_t numClasses = 0;
    if (!R_U32(&r, &numClasses)) {
        UninstallProcShim(ip, &shim);
        return TCL_ERROR;
    }
    for (uint32_t c = 0; c < numClasses; c++) {
        /* classFqn + nSupers + supers… — saver writes 0; ignore here */
        char    *cls = NULL;
        uint32_t cl  = 0;
        if (!R_LPString(&r, &cls, &cl)) {
            UninstallProcShim(ip, &shim);
            return TCL_ERROR;
        }
        Tcl_Free(cls);
        uint32_t nSup = 0;
        if (!R_U32(&r, &nSup)) {
            UninstallProcShim(ip, &shim);
            return TCL_ERROR;
        }
        for (uint32_t s = 0; s < nSup; s++) {
            char    *su = NULL;
            uint32_t sl = 0;
            if (!R_LPString(&r, &su, &sl)) {
                UninstallProcShim(ip, &shim);
                return TCL_ERROR;
            }
            Tcl_Free(su);
        }
    }
    uint32_t numMethods = 0;
    if (!R_U32(&r, &numMethods)) {
        UninstallProcShim(ip, &shim);
        return TCL_ERROR;
    }
    OOShim ooshim;
    if (numMethods) {
        if (InstallOOShim(ip, &ooshim) != TCL_OK) {
            if (numProcs)
                UninstallProcShim(ip, &shim);
            return TCL_ERROR;
        }
    }
    for (uint32_t m = 0; m < numMethods; m++) {
        if (ReadOneMethodAndRegister(&r, ip, &ooshim) != TCL_OK) {
            if (numMethods)
                UninstallOOShim(ip, &ooshim);
            if (numProcs)
                UninstallProcShim(ip, &shim);
            return TCL_ERROR;
        }
    }

    Tcl_IncrRefCount(topBC);
    int rc = Tcl_EvalObjEx(ip, topBC, TCL_EVAL_GLOBAL);
    Tcl_DecrRefCount(topBC);
    if (numMethods)
        UninstallOOShim(ip, &ooshim);
    if (numProcs)
        UninstallProcShim(ip, &shim);
    return rc;
}

/* ==========================================================================
 * Tcl commands
 * ========================================================================== */

int Tbcx_LoadChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channelName");
        return TCL_ERROR;
    }

    const char *chName = Tcl_GetString(objv[1]);
    Tcl_Channel ch     = Tcl_GetChannel(interp, chName, NULL);
    if (!ch)
        return TCL_ERROR;
    return LoadTbcxStream(interp, ch);
}

int Tbcx_LoadFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "in.tbcx");
        return TCL_ERROR;
    }

    Tcl_Channel in = Tcl_FSOpenFileChannel(interp, objv[1], "r", 0);
    if (!in)
        return TCL_ERROR;

    int rc = LoadTbcxStream(interp, in);
    if (Tcl_Close(interp, in) != TCL_OK)
        rc = TCL_ERROR;
    return rc;
}
