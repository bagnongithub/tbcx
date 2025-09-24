/*
 * tbcxload.c
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef enum { TBCX_SRC_TOP, TBCX_SRC_PROC } TbcxBCSource;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static Tcl_Namespace *EnsureNamespace(Tcl_Interp *interp, const char *nsName, Tcl_Namespace *fallback);
static int            EvalInNamespace(Tcl_Interp *interp, Tcl_Namespace *nsPtr, Tcl_Obj *scriptObj);
static void           FreeLoadedByteCode(ByteCode *bc);
static int            InstallPrecompiled(Tcl_Interp *interp, Tcl_Namespace *targetNs, const char *fqName, const char *argSpec, ByteCode *bc, uint32_t numLocals, Tcl_Command *outTok);
static void           LambdaLiterals(Tcl_Interp *interp, Tcl_Obj **lits, Tcl_Size n);
static int            LoadFromChannel(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Namespace *nsPtr);
static int            ReadAll(Tcl_Channel ch, unsigned char *dst, Tcl_Size need);
static int            ReadByteCode(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Namespace *nsPtr, TbcxBCSource src, const TbcxHeader *topHdr, uint32_t formatIfProc, ByteCode **out, uint32_t *outNumLocals);
static int            ReadOneLiteral(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj);
static void           TbcxFinalizeByteCode(ByteCode *bc, Tcl_Interp *interp, size_t codeLen);
int                   Tbcx_LoadChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
int                   Tbcx_LoadFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);

/* C11 sanity: we assume opcodes are byte-sized */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(((unsigned char)0)) == 1, "opcode byte size guard");
#endif

/* ==========================================================================
 * Namespace & ByteCode lifecycle
 * ========================================================================== */

static Tcl_HashTable  tbcxMethReg;
static int            tbcxMethRegInit = 0;

/*
 * EnsureNamespace
 * Resolve or create a namespace for a given name; fallback to provided ns/global.
 */

static Tcl_Namespace *EnsureNamespace(Tcl_Interp *interp, const char *nsName, Tcl_Namespace *fallback) {
    if (!nsName || !*nsName) {
        return (fallback ? fallback : Tcl_GetGlobalNamespace(interp));
    }

    Namespace  *containerNs = NULL, *dummyActual = NULL, *dummyAlt = NULL;
    const char *simple = NULL;

    TclGetNamespaceForQualName(interp, nsName, (Namespace *)Tcl_GetGlobalNamespace(interp), 0, &containerNs, &dummyAlt, &dummyActual, &simple);
    (void)simple;

    if (!containerNs) {
        Tcl_Namespace *created = Tcl_CreateNamespace(interp, nsName, NULL, NULL);
        if (created) {
            containerNs = (Namespace *)created;
        }
    }
    if (!containerNs) {
        containerNs = (Namespace *)(fallback ? fallback : Tcl_GetGlobalNamespace(interp));
    }
    return (Tcl_Namespace *)containerNs;
}

/*
 * FreeLoadedByteCode
 * Free a ByteCode loaded from a TBCX stream (including AuxData/literals).
 */

static void FreeLoadedByteCode(ByteCode *bc) {
    if (!bc)
        return;
    if (bc->auxDataArrayPtr) {
        for (Tcl_Size i = 0; i < bc->numAuxDataItems; ++i) {
            if (bc->auxDataArrayPtr[i].type && bc->auxDataArrayPtr[i].type->freeProc) {
                bc->auxDataArrayPtr[i].type->freeProc(bc->auxDataArrayPtr[i].clientData);
            }
        }
        Tcl_Free((char *)bc->auxDataArrayPtr);
    }
    if (bc->objArrayPtr) {
        for (Tcl_Size i = 0; i < bc->numLitObjects; ++i) {
            if (bc->objArrayPtr[i])
                Tcl_DecrRefCount(bc->objArrayPtr[i]);
        }
        Tcl_Free((char *)bc->objArrayPtr);
    }
    if (bc->exceptArrayPtr) {
        Tcl_Free((char *)bc->exceptArrayPtr);
    }
    if (bc->nsPtr) {
        bc->nsPtr->refCount--;
    }
    if (bc->interpHandle) {
        TclHandleRelease(bc->interpHandle);
    }
    Tcl_Free((char *)bc);
}

/*
 * TbcxFinalizeByteCode
 * Finalize a ByteCode struct as precompiled for this interpreter.
 */

static void TbcxFinalizeByteCode(ByteCode *bc, Tcl_Interp *interp, size_t codeLen) {
    Interp        *iPtr = (Interp *)interp;
    unsigned char *end  = bc->codeStart + codeLen;

    bc->flags |= TCL_BYTECODE_PRECOMPILED;
    bc->interpHandle    = TclHandlePreserve(iPtr->handle);
    bc->structureSize   = sizeof(ByteCode);
    bc->numCodeBytes    = (Tcl_Size)codeLen;
    bc->numSrcBytes     = 0;
    bc->numCmdLocBytes  = 0;
    bc->codeDeltaStart  = end;
    bc->codeLengthStart = end;
    bc->srcDeltaStart   = end;
    bc->srcLengthStart  = end;
    bc->source          = "tbcx:precompiled";
    bc->localCachePtr   = NULL;
}

/* ==========================================================================
 * Deserialization helpers (I/O + literals)
 * ========================================================================== */

/*
 * ReadAll
 * Read exactly N bytes from a channel; return 0 on EOF/error.
 */

static int ReadAll(Tcl_Channel ch, unsigned char *dst, Tcl_Size need) {
    while (need) {
        const Tcl_Size maxChunk = (Tcl_Size)(1 << 20);
        Tcl_Size       chunk    = (need > maxChunk) ? maxChunk : need;
        Tcl_Size       n        = Tcl_ReadRaw(ch, (char *)dst, chunk);
        if (n <= 0)
            return 0;
        dst += n;
        need -= n;
    }
    return 1;
}

/*
 * ReadOneLiteral
 * Deserialize a single literal value from a TBCX stream into a Tcl_Obj.
 */

static int ReadOneLiteral(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj) {
    unsigned char tag;
    if (!ReadAll(ch, &tag, 1)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: literal tag", -1));
        return TCL_ERROR;
    }

    switch (tag) {
    case TBCX_LIT_STRING: {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: string len", -1));
            return TCL_ERROR;
        }
        uint32_t L = le32(lb);

        Tcl_Obj *o;
        if (L == 0) {
            o = Tcl_NewObj();
        } else {
            char *buf = (char *)Tcl_Alloc(L);
            if (L && !ReadAll(ch, (unsigned char *)buf, L)) {
                Tcl_Free(buf);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: string bytes", -1));
                return TCL_ERROR;
            }
            o = Tcl_NewStringObj(buf, (Tcl_Size)L);
            Tcl_Free(buf);
        }
        *outObj = o;
        return TCL_OK;
    }
    case TBCX_LIT_WIDEINT: {
        unsigned char ib[8];
        if (!ReadAll(ch, ib, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: wideint", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewWideIntObj((Tcl_WideInt)(int64_t)le64(ib));
        return TCL_OK;
    }
    case TBCX_LIT_DOUBLE: {
        unsigned char db[8];
        if (!ReadAll(ch, db, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: double", -1));
            return TCL_ERROR;
        }
        uint64_t bits = le64(db);
        double   d;
        memcpy(&d, &bits, 8);
        *outObj = Tcl_NewDoubleObj(d);
        return TCL_OK;
    }
    case TBCX_LIT_BIGNUM: {
        unsigned char sign, lb[4];
        if (!ReadAll(ch, &sign, 1) || !ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bignum header", -1));
            return TCL_ERROR;
        }
        uint32_t       L   = le32(lb);
        unsigned char *mag = (unsigned char *)Tcl_Alloc(L ? L : 1);
        if (L && !ReadAll(ch, mag, L)) {
            Tcl_Free((char *)mag);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bignum mag", -1));
            return TCL_ERROR;
        }
        mp_int big;
        mp_err rc = mp_init(&big);
        if (rc != MP_OKAY) {
            Tcl_Free((char *)mag);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_init failed", -1));
            return TCL_ERROR;
        }
        if (L) {
            rc = mp_unpack(&big, (size_t)L, MP_MSB_FIRST, 1, MP_BIG_ENDIAN, 0, mag);
            if (rc != MP_OKAY) {
                mp_clear(&big);
                Tcl_Free((char *)mag);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_unpack failed", -1));
                return TCL_ERROR;
            }
        }
        if (sign) {
            rc = mp_neg(&big, &big);
            if (rc != MP_OKAY) {
                mp_clear(&big);
                Tcl_Free((char *)mag);
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_neg failed", -1));
                return TCL_ERROR;
            }
        }
        Tcl_Free((char *)mag);
        *outObj = Tcl_NewBignumObj(&big);
        return TCL_OK;
    }
    case TBCX_LIT_WIDEUINT: {
        unsigned char ub[8];
        if (!ReadAll(ch, ub, 8)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: wideuint", -1));
            return TCL_ERROR;
        }
        uint64_t u = le64(ub);
        if (u <= (uint64_t)INT64_MAX) {
            *outObj = Tcl_NewWideIntObj((Tcl_WideInt)(int64_t)u);
        } else {
            mp_int big;
            mp_err rc = mp_init(&big);
            if (rc != MP_OKAY) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj("tommath: mp_init failed", -1));
                return TCL_ERROR;
            }
            mp_set_u64(&big, u);
            *outObj = Tcl_NewBignumObj(&big);
        }
        return TCL_OK;
    }
    case TBCX_LIT_BOOLEAN: {
        unsigned char bb;
        if (!ReadAll(ch, &bb, 1)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: boolean", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewBooleanObj(bb ? 1 : 0);
        return TCL_OK;
    }
    case TBCX_LIT_BYTEARR: {
        unsigned char lb[4];
        if (!ReadAll(ch, lb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bytearray len", -1));
            return TCL_ERROR;
        }
        uint32_t       L   = le32(lb);
        unsigned char *buf = (unsigned char *)Tcl_Alloc(L ? L : 1);
        if (L && !ReadAll(ch, buf, L)) {
            Tcl_Free(buf);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: bytearray bytes", -1));
            return TCL_ERROR;
        }
        *outObj = Tcl_NewByteArrayObj(buf, (Tcl_Size)L);
        Tcl_Free(buf);
        return TCL_OK;
    }
    case TBCX_LIT_LIST: {
        unsigned char nb[4];
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: list count", -1));
            return TCL_ERROR;
        }
        uint32_t n = le32(nb);
        if (n > TBCX_MAX_LITERALS) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("unreasonable list length", -1));
            return TCL_ERROR;
        }
        Tcl_Obj **elems = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * n);
        for (uint32_t i = 0; i < n; ++i) {
            Tcl_Obj *e = NULL;
            if (ReadOneLiteral(interp, ch, &e) != TCL_OK) {
                while (i--)
                    Tcl_DecrRefCount(elems[i]);
                Tcl_Free((char *)elems);
                return TCL_ERROR;
            }
            Tcl_IncrRefCount(e);
            elems[i] = e;
        }
        Tcl_Obj *lst = Tcl_NewListObj((Tcl_Size)n, elems);
        for (uint32_t i = 0; i < n; ++i)
            Tcl_DecrRefCount(elems[i]);
        Tcl_Free((char *)elems);
        *outObj = lst;
        return TCL_OK;
    }
    case TBCX_LIT_DICT: {
        unsigned char nb[4];
        if (!ReadAll(ch, nb, 4)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: dict pairs", -1));
            return TCL_ERROR;
        }
        uint32_t pairs = le32(nb);
        if (pairs > TBCX_MAX_LITERALS) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("unreasonable dict size", -1));
            return TCL_ERROR;
        }
        Tcl_Obj *d = Tcl_NewDictObj();
        for (uint32_t i = 0; i < pairs; ++i) {
            Tcl_Obj *k = NULL, *v = NULL;
            if (ReadOneLiteral(interp, ch, &k) != TCL_OK) {
                Tcl_DecrRefCount(d);
                return TCL_ERROR;
            }
            if (ReadOneLiteral(interp, ch, &v) != TCL_OK) {
                Tcl_DecrRefCount(k);
                Tcl_DecrRefCount(d);
                return TCL_ERROR;
            }
            Tcl_DictObjPut(NULL, d, k, v);
        }
        *outObj = d;
        return TCL_OK;
    }
    default:
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unknown literal tag", -1));
        return TCL_ERROR;
    }
}

/* ==========================================================================
 * Deserialization: ByteCode blocks
 * ========================================================================== */

#define R(n, buf)                                                                                                                                                                                      \
    do {                                                                                                                                                                                               \
        if (!ReadAll(ch, (unsigned char *)(buf), (Tcl_Size)(n)))                                                                                                                                       \
            goto io_fail;                                                                                                                                                                              \
    } while (0)
#define R1(b) R(1, &(b))

/*
 * LambdaLiterals
 *
 * Pre-scan a list of candidate lambda literals, validate their list form, convert them to the
 * custom lambda internal rep, and JIT/byte-compile their bodies upfront to prime the interpreter
 * and avoid runtime conversion costs.
 */

static void LambdaLiterals(Tcl_Interp *interp, Tcl_Obj **lits, Tcl_Size n) {
    if (!lits || n <= 0 || !tbcxTyLambda)
        return;

    for (Tcl_Size i = 0; i < n; ++i) {
        Tcl_Obj *o = lits[i];
        if (!o)
            continue;

        Tcl_Size  ll = 0;
        Tcl_Obj **ve = NULL;
        if (Tcl_ListObjGetElements(NULL, o, &ll, &ve) != TCL_OK) {
            continue;
        }
        if (ll != 2 && ll != 3)
            continue;

        Tcl_Size argsN = 0;
        (void)Tcl_ListObjLength(NULL, ve[0], &argsN);

        if (Tcl_ConvertToType(interp, o, tbcxTyLambda) != TCL_OK) {
            Tcl_ResetResult(interp);
            continue;
        }

        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(o, tbcxTyLambda);
        if (!ir)
            continue;
        Proc *procPtr = (Proc *)ir->twoPtrValue.ptr1;
        if (procPtr && procPtr->bodyPtr) {
            (void)Tcl_ConvertToType(interp, procPtr->bodyPtr, tbcxTyBytecode);
            Tcl_ResetResult(interp);
        }
    }
}

/*
 * ReadByteCode
 * Deserialize a ByteCode block (top-level or nested) from TBCX.
 */

static int ReadByteCode(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Namespace *nsPtr, TbcxBCSource src, const TbcxHeader *topHdr, uint32_t formatIfProc, ByteCode **out, uint32_t *outNumLocals) {
    unsigned char   b4[4];
    ByteCode       *codePtr = NULL;
    Tcl_Obj       **lits    = NULL;
    AuxData        *aux     = NULL;
    ExceptionRange *xr      = NULL;
    unsigned char  *code    = NULL;
    size_t          codeLen = 0;
    uint32_t        numLits = 0, numAux = 0, numEx = 0, maxStack = 0, numLocals = 0;

    if (src == TBCX_SRC_PROC) {
        if (formatIfProc != TBCX_FORMAT) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("unsupported nested bytecode format", -1));
            return TCL_ERROR;
        }
        R(4, b4);
        codeLen = (size_t)le32(b4);
        if (codeLen > TBCX_MAX_CODE || codeLen > (size_t)SIZE_MAX)
            goto bad;
    } else {
        codeLen   = (size_t)topHdr->codeLen;
        numLits   = topHdr->numLiterals;
        numAux    = topHdr->numAux;
        numEx     = topHdr->numExcept;
        maxStack  = topHdr->maxStackDepth;
        numLocals = topHdr->numLocals;
        if (codeLen > TBCX_MAX_CODE || numLits > TBCX_MAX_LITERALS || numAux > TBCX_MAX_AUX || numEx > TBCX_MAX_EXCEPT)
            goto bad;
    }

    codePtr = (ByteCode *)Tcl_Alloc(sizeof(ByteCode) + codeLen);
    memset(codePtr, 0, sizeof(ByteCode));
    code = ((unsigned char *)codePtr) + sizeof(ByteCode);
    if (codeLen && !ReadAll(ch, code, (Tcl_Size)codeLen))
        goto io_fail;

    if (src == TBCX_SRC_PROC) {
        R(4, b4);
        numLits = le32(b4);
        if (numLits > TBCX_MAX_LITERALS)
            goto bad;
    }
    if (numLits) {
        lits = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * numLits);
        memset(lits, 0, sizeof(Tcl_Obj *) * numLits);
    }
    for (uint32_t i = 0; i < numLits; ++i) {
        Tcl_Obj *o = NULL;
        if (ReadOneLiteral(interp, ch, &o) != TCL_OK)
            goto bad;
        Tcl_IncrRefCount(o);
        lits[i] = o;
    }

    if (src == TBCX_SRC_PROC) {
        R(4, b4);
        numAux = le32(b4);
        if (numAux > TBCX_MAX_AUX)
            goto bad;
    }
    if (numAux) {
        aux = (AuxData *)Tcl_Alloc(sizeof(AuxData) * numAux);
        memset(aux, 0, sizeof(AuxData) * numAux);
    }
    for (uint32_t i = 0; i < numAux; ++i) {
        unsigned char kind;
        R1(kind);
        const struct AuxDataType *tp = NULL;
        switch (kind) {
        case TBCX_AUX_JT_STR:
            tp = tbcxAuxJTStr;
            break;
        case TBCX_AUX_JT_NUM:
            tp = tbcxAuxJTNum;
            break;
        case TBCX_AUX_DICTUPD:
            tp = tbcxAuxDictUpdate;
            break;
        case TBCX_AUX_NEWFORE:
        case TBCX_AUX_FOREACH:
            tp = tbcxAuxForeach;
            break;
        default:
            goto bad;
        }

        if (kind == TBCX_AUX_JT_STR) {
            unsigned char cb[4];
            R(4, cb);
            uint32_t       count = le32(cb);
            JumptableInfo *info  = (JumptableInfo *)Tcl_Alloc(sizeof(JumptableInfo));
            Tcl_InitHashTable(&info->hashTable, TCL_STRING_KEYS);
            aux[i].type       = tp;
            aux[i].clientData = info;
            for (uint32_t k = 0; k < count; ++k) {
                unsigned char lb2[4], ob2[4];
                R(4, lb2);
                uint32_t L   = le32(lb2);
                char    *key = (char *)Tcl_Alloc(L + 1);
                if (L)
                    R(L, key);
                key[L] = 0;
                R(4, ob2);
                int            isNew;
                Tcl_HashEntry *h = Tcl_CreateHashEntry(&info->hashTable, key, &isNew);
                Tcl_SetHashValue(h, INT2PTR((int)le32(ob2)));
                Tcl_Free(key);
            }
        } else if (kind == TBCX_AUX_JT_NUM) {
            unsigned char cb[4];
            R(4, cb);
            uint32_t       count = le32(cb);
            JumptableInfo *info  = (JumptableInfo *)Tcl_Alloc(sizeof(JumptableInfo));
            Tcl_InitHashTable(&info->hashTable, TCL_ONE_WORD_KEYS);
            aux[i].type       = tp;
            aux[i].clientData = info;
            for (uint32_t k = 0; k < count; ++k) {
                unsigned char kb[8], ob2[4];
                R(8, kb);
                R(4, ob2);
                intptr_t key = (intptr_t)le64(kb);
#if INTPTR_MAX == INT32_MAX
                if (key < INT32_MIN || key > INT32_MAX) {
                    goto bad;
                }
#endif
                int            isNew;
                Tcl_HashEntry *h = Tcl_CreateHashEntry(&info->hashTable, (char *)key, &isNew);
                Tcl_SetHashValue(h, INT2PTR((int)le32(ob2)));
            }
        } else if (kind == TBCX_AUX_DICTUPD) {
            unsigned char nb2[4];
            R(4, nb2);
            uint32_t        n    = le32(nb2);
            DictUpdateInfo *info = (DictUpdateInfo *)Tcl_Alloc(sizeof(DictUpdateInfo) + (size_t)n * sizeof(Tcl_Size));
            info->length         = (Tcl_Size)n;
            aux[i].type          = tp;
            aux[i].clientData    = info;
            for (uint32_t j = 0; j < n; ++j) {
                unsigned char vb[4];
                R(4, vb);
                info->varIndices[j] = (Tcl_Size)le32(vb);
            }
        } else if (kind == TBCX_AUX_NEWFORE || kind == TBCX_AUX_FOREACH) {
            unsigned char lb3[4];
            R(4, lb3);
            uint32_t     lists      = le32(lb3);
            ForeachInfo *info       = (ForeachInfo *)Tcl_Alloc(sizeof(ForeachInfo) + lists * sizeof(ForeachVarList *));
            info->numLists          = (Tcl_Size)lists;
            aux[i].type             = tp;
            aux[i].clientData       = info;
            Tcl_LVTIndex  firstTemp = 0, loopTemp = 0;
            unsigned char tb1[4], tb2[4];
            R(4, tb1);
            R(4, tb2);
            firstTemp            = (Tcl_LVTIndex)le32(tb1);
            loopTemp             = (Tcl_LVTIndex)le32(tb2);
            info->firstValueTemp = firstTemp;
            info->loopCtTemp     = loopTemp;
            for (uint32_t j = 0; j < lists; ++j) {
                unsigned char cb2[4];
                R(4, cb2);
                uint32_t nv = le32(cb2);
                if (nv == 0 || nv > TBCX_MAX_LITERALS)
                    goto bad;
                ForeachVarList *vl = (ForeachVarList *)Tcl_Alloc(sizeof(ForeachVarList) + (nv - 1) * sizeof(Tcl_LVTIndex));
                vl->numVars        = (Tcl_Size)nv;
                for (uint32_t k = 0; k < nv; ++k) {
                    unsigned char vb[4];
                    R(4, vb);
                    vl->varIndexes[k] = (Tcl_LVTIndex)le32(vb);
                }
                info->varLists[j] = vl;
            }
        } else {
            goto bad;
        }
    }

    if (src == TBCX_SRC_PROC) {
        R(4, b4);
        numEx = le32(b4);
        if (numEx > TBCX_MAX_EXCEPT)
            goto bad;
    }

    if (numEx) {
        xr = (ExceptionRange *)Tcl_Alloc(sizeof(ExceptionRange) * numEx);
        memset(xr, 0, sizeof(ExceptionRange) * numEx);
    }
    for (uint32_t i = 0; i < numEx; ++i) {
        /* Read fields once, assign once */
        unsigned char tb, sb[4], eb[4], cb[4], bb[4], hb[4], nb[4];
        R1(tb);
        R(4, nb);
        xr[i].nestingLevel = (int)le32(nb);
        R(4, sb);
        R(4, eb);
        R(4, cb);
        R(4, bb);
        R(4, hb);

        xr[i].type      = (tb ? CATCH_EXCEPTION_RANGE : LOOP_EXCEPTION_RANGE);

        const int start = (int)le32(sb);
        const int end   = (int)le32(eb);
        if (end < start || (size_t)end > codeLen)
            goto bad;
        xr[i].codeOffset   = start;
        xr[i].numCodeBytes = end - start; /* [start, end) */

        const int cont     = (int)le32(cb);
        const int brk      = (int)le32(bb);
        const int cat      = (int)le32(hb);
        if ((cont != -1 && (cont < 0 || (size_t)cont >= codeLen)) || (brk != -1 && (brk < 0 || (size_t)brk >= codeLen)) || (cat != -1 && (cat < 0 || (size_t)cat >= codeLen))) {
            goto bad;
        }
        xr[i].continueOffset = cont;
        xr[i].breakOffset    = brk;
        xr[i].catchOffset    = cat;
    }

    if (src == TBCX_SRC_PROC) {
        unsigned char bA[4], bB[4], bC[4];
        R(4, bA);
        R(4, bB);
        R(4, bC);
        maxStack  = le32(bA);
        numLocals = le32(bC);
        if (outNumLocals)
            *outNumLocals = numLocals;
    }

    codePtr->codeStart       = code;
    codePtr->numCommands     = (src == TBCX_SRC_TOP ? (int)topHdr->numCmds : 0);
    codePtr->objArrayPtr     = lits;
    codePtr->numLitObjects   = (int)numLits;
    codePtr->auxDataArrayPtr = aux;
    codePtr->numAuxDataItems = (int)numAux;
    codePtr->exceptArrayPtr  = xr;
    codePtr->numExceptRanges = (int)numEx;
    codePtr->maxStackDepth   = (int)maxStack;
    if (numEx == 0) {
        codePtr->maxExceptDepth = TCL_INDEX_NONE;
    } else {
        Tcl_Size md = 0;
        for (uint32_t i = 0; i < numEx; ++i) {
            Tcl_Size d = (Tcl_Size)xr[i].nestingLevel;
            if (d > md)
                md = d;
        }
        codePtr->maxExceptDepth = md;
    }
    codePtr->nsPtr = (Namespace *)nsPtr;
    if (codePtr->nsPtr)
        codePtr->nsPtr->refCount++;
    codePtr->compileEpoch = ((Interp *)interp)->compileEpoch;
    codePtr->nsEpoch      = codePtr->nsPtr ? codePtr->nsPtr->resolverEpoch : 0;
    TbcxFinalizeByteCode(codePtr, interp, codeLen);

    LambdaLiterals(interp, codePtr->objArrayPtr, codePtr->numLitObjects);

    *out = codePtr;
    return TCL_OK;
bad:
    if (aux) {
        for (uint32_t j = 0; j < numAux; ++j)
            if (aux[j].type && aux[j].type->freeProc)
                aux[j].type->freeProc(aux[j].clientData);
        Tcl_Free((char *)aux);
    }
    if (lits) {
        for (uint32_t j = 0; j < numLits; ++j)
            if (lits[j])
                Tcl_DecrRefCount(lits[j]);
        Tcl_Free((char *)lits);
    }
    if (xr)
        Tcl_Free((char *)xr);
    if (codePtr)
        Tcl_Free((char *)codePtr);
    Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid/corrupt bytecode block", -1));
    return TCL_ERROR;
io_fail:
    Tcl_SetObjResult(interp, Tcl_NewStringObj("unexpected EOF in bytecode block", -1));
    goto bad;
}

/* ==========================================================================
 * Proc support
 * ========================================================================== */

/*
 * InstallPrecompiled
 * Create a proc and install a precompiled ByteCode body for it.
 */

static int InstallPrecompiled(Tcl_Interp *interp, Tcl_Namespace *targetNs, const char *fqName, const char *argSpec, ByteCode *bc, uint32_t numLocals, Tcl_Command *outTok) {
    Namespace  *containerNs = NULL, *dummyActual = NULL, *dummyAlt = NULL;
    const char *simple = NULL;
    TclGetNamespaceForQualName(interp, fqName, (Namespace *)targetNs, 0, &containerNs, &dummyAlt, &dummyActual, &simple);
    if (!containerNs) {
        containerNs = (Namespace *)targetNs;
    }
    if (!simple) {
        simple = fqName;
    }

    const char *nsFull  = containerNs->fullName ? containerNs->fullName : "::";
    Tcl_Obj    *absName = Tcl_NewStringObj(nsFull, -1);
    if (!(nsFull[0] == ':' && nsFull[1] == ':' && nsFull[2] == '\0')) {
        Tcl_AppendToObj(absName, "::", 2);
    }
    Tcl_AppendToObj(absName, simple, -1);
    Tcl_IncrRefCount(absName);

    Tcl_Obj *cmdv[4];
    cmdv[0] = Tcl_NewStringObj("::proc", -1);
    cmdv[1] = absName;
    Tcl_IncrRefCount(cmdv[1]);
    cmdv[2] = Tcl_NewStringObj(argSpec ? argSpec : "", -1);
    cmdv[3] = Tcl_NewStringObj("", -1);
    Tcl_IncrRefCount(cmdv[2]);
    Tcl_IncrRefCount(cmdv[3]);
    if (Tcl_EvalObjv(interp, 4, cmdv, TCL_EVAL_DIRECT) != TCL_OK) {
        Tcl_DecrRefCount(cmdv[3]);
        Tcl_DecrRefCount(cmdv[2]);
        Tcl_DecrRefCount(cmdv[1]);
        Tcl_DecrRefCount(absName);
        return TCL_ERROR;
    }
    Tcl_DecrRefCount(cmdv[3]);
    Tcl_DecrRefCount(cmdv[2]);
    Tcl_DecrRefCount(cmdv[1]);

    Tcl_Command tok = Tcl_FindCommand(interp, Tcl_GetString(absName), NULL, 0);
    if (!tok) {
        Tcl_DecrRefCount(absName);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: failed to resolve created proc", -1));
        return TCL_ERROR;
    }

    {
        Command *cmdPtr            = (Command *)tok;
        Proc    *procPtr           = (Proc *)cmdPtr->objClientData;

        procPtr->numCompiledLocals = (int)numLocals;
        bc->procPtr                = procPtr;

        Tcl_Obj *bodyObj           = Tcl_NewObj();
        Tcl_IncrRefCount(bodyObj);
        bc->refCount = 1;
        ByteCodeSetInternalRep(bodyObj, tbcxTyBytecode, bc);
        if (procPtr->bodyPtr) {
            Tcl_DecrRefCount(procPtr->bodyPtr);
        }
        procPtr->bodyPtr = bodyObj;

        bc->compileEpoch = ((Interp *)interp)->compileEpoch;
        bc->nsEpoch      = bc->nsPtr ? bc->nsPtr->resolverEpoch : 0;
    }

    if (outTok) {
        *outTok = tok;
    }
    Tcl_DecrRefCount(absName);
    return TCL_OK;
}

static void tbcxRegisterMethodBody(const char *classFqn, Tcl_Obj *methodNameObj, int isClassMethod, Tcl_Obj *bodyObj) {
    if (!tbcxMethRegInit) {
        Tcl_InitHashTable(&tbcxMethReg, TCL_STRING_KEYS);
        tbcxMethRegInit = 1;
    }
    Tcl_DString k;
    Tcl_DStringInit(&k);
    Tcl_DStringAppend(&k, "K:", 2);
    Tcl_DStringAppend(&k, isClassMethod ? "1:" : "0:", 2);
    Tcl_DStringAppend(&k, classFqn, -1);
    Tcl_DStringAppend(&k, "\x1f", 1);
    Tcl_DStringAppend(&k, Tcl_GetString(methodNameObj), -1);
    int            isNew = 0;
    Tcl_HashEntry *hPtr  = Tcl_CreateHashEntry(&tbcxMethReg, Tcl_DStringValue(&k), &isNew);
    if (isNew) {
        Tcl_IncrRefCount(bodyObj);
        Tcl_SetHashValue(hPtr, (ClientData)bodyObj);
    }
    Tcl_DStringFree(&k);
}

static void tbcxClearMethodRegistry(void) {
    if (!tbcxMethRegInit)
        return;
    Tcl_HashSearch hs;
    Tcl_HashEntry *e;
    for (e = Tcl_FirstHashEntry(&tbcxMethReg, &hs); e; e = Tcl_NextHashEntry(&hs)) {
        Tcl_Obj *o = (Tcl_Obj *)Tcl_GetHashValue(e);
        if (o)
            Tcl_DecrRefCount(o);
    }
    Tcl_DeleteHashTable(&tbcxMethReg);
    tbcxMethRegInit = 0;
}

/* ==========================================================================
 * High-level loader
 * ========================================================================== */

static int EvalInNamespace(Tcl_Interp *interp, Tcl_Namespace *nsPtr, Tcl_Obj *scriptObj) {
    Tcl_CallFrame *cf = NULL;
    if (TclPushStackFrame(interp, &cf, nsPtr, 0) != TCL_OK) {
        return TCL_ERROR;
    }
    int rc = Tcl_EvalObjEx(interp, scriptObj, 0);
    if (rc == TCL_ERROR) {
        const char *full = ((Namespace *)nsPtr)->fullName;
        Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf("\n    (in namespace eval \"%s\" script line %d)", full ? full : "::", Tcl_GetErrorLine(interp)));
    }
    TclPopStackFrame(interp);
    return rc;
}

/*
 * LoadFromChannel
 * Load and execute a TBCX file from an open channel into a namespace.
 */

static int LoadFromChannel(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Namespace *nsPtr) {
    if (Tcl_SetChannelOption(interp, ch, "-translation", "binary") != TCL_OK) {
        return TCL_ERROR;
    }
    if (Tcl_SetChannelOption(interp, ch, "-eofchar", "") != TCL_OK) {
        return TCL_ERROR;
    }

    unsigned char Hbuf[sizeof(TbcxHeader)];
    if (!ReadAll(ch, Hbuf, (Tcl_Size)sizeof Hbuf)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("short read: header", -1));
        return TCL_ERROR;
    }
    TbcxHeader H;
    H.magic         = le32(Hbuf + 0);
    H.format        = le32(Hbuf + 4);
    H.flags         = le32(Hbuf + 8);
    H.codeLen       = le64(Hbuf + 12);
    H.numCmds       = le32(Hbuf + 20);
    H.numExcept     = le32(Hbuf + 24);
    H.numLiterals   = le32(Hbuf + 28);
    H.numAux        = le32(Hbuf + 32);
    H.numLocals     = le32(Hbuf + 36);
    H.maxStackDepth = le32(Hbuf + 40);

    if (H.magic != TBCX_MAGIC || H.format != TBCX_FORMAT || H.flags != 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("not a TBCX file", -1));
        return TCL_ERROR;
    }
    if (H.codeLen > TBCX_MAX_CODE || H.numLiterals > TBCX_MAX_LITERALS || H.numAux > TBCX_MAX_AUX || H.numExcept > TBCX_MAX_EXCEPT) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unreasonable sizes in header", -1));
        return TCL_ERROR;
    }

    ByteCode *topBC = NULL;
    if (ReadByteCode(interp, ch, nsPtr, TBCX_SRC_TOP, &H, 0, &topBC, NULL) != TCL_OK) {
        return TCL_ERROR;
    }

    unsigned char nb[4];
    uint32_t      numProcs = 0;
    {
        Tcl_Size got = Tcl_ReadRaw(ch, (char *)nb, (Tcl_Size)4);
        if (got == 0) {
            numProcs = 0;
        } else if (got < 0) {
            goto fail_top;
        } else {
            if (got < (Tcl_Size)4) {
                if (!ReadAll(ch, nb + got, (Tcl_Size)(4 - got))) {
                    goto fail_top;
                }
            }
            numProcs = le32(nb);
        }
    }

    for (uint32_t i = 0; i < numProcs; ++i) {
        if (!ReadAll(ch, nb, 4)) {
            goto fail_top;
        }
        uint32_t nL = le32(nb);
        if (nL > TBCX_MAX_STR)
            goto fail_top;
        char *name = (char *)Tcl_Alloc(nL + 1);
        if (nL && !ReadAll(ch, (unsigned char *)name, nL)) {
            Tcl_Free(name);
            goto fail_top;
        }
        name[nL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(name);
            goto fail_top;
        }
        uint32_t nsL = le32(nb);
        char    *ns  = (char *)Tcl_Alloc(nsL + 1);
        if (nsL && !ReadAll(ch, (unsigned char *)ns, nsL)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            goto fail_top;
        }
        ns[nsL] = 0;
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            goto fail_top;
        }
        uint32_t aL   = le32(nb);
        char    *args = (char *)Tcl_Alloc(aL + 1);
        if (aL && !ReadAll(ch, (unsigned char *)args, aL)) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_Free(args);
            goto fail_top;
        }
        args[aL]                    = 0;

        ByteCode      *pbc          = NULL;
        uint32_t       pbcNumLocals = 0;
        Tcl_Namespace *srcNsPtr     = EnsureNamespace(interp, (nsL ? ns : NULL), nsPtr);

        if (ReadByteCode(interp, ch, srcNsPtr, TBCX_SRC_PROC, NULL, H.format, &pbc, &pbcNumLocals) != TCL_OK) {
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_Free(args);
            goto fail_top;
        }
        Tcl_Command createdTok = NULL;
        if (InstallPrecompiled(interp, nsPtr, name, args, pbc, pbcNumLocals, &createdTok) != TCL_OK) {
            FreeLoadedByteCode(pbc);
            Tcl_Free(name);
            Tcl_Free(ns);
            Tcl_Free(args);
            goto fail_top;
        }
        Tcl_Free(name);
        Tcl_Free(ns);
        Tcl_Free(args);
    }

    uint32_t numClasses = 0;
    {
        Tcl_Size got = Tcl_ReadRaw(ch, (char *)nb, (Tcl_Size)4);
        if (got == 0) {
            numClasses = 0;
        } else if (got < 0) {
            goto fail_top;
        } else {
            if (got < (Tcl_Size)4) {
                if (!ReadAll(ch, nb + got, (Tcl_Size)(4 - got))) {
                    goto fail_top;
                }
            }
            numClasses = le32(nb);
        }
    }

    for (uint32_t i = 0; i < numClasses; ++i) {
        if (!ReadAll(ch, nb, 4))
            goto fail_top;
        uint32_t cL  = le32(nb);
        char    *cls = (char *)Tcl_Alloc(cL + 1);
        if (cL && !ReadAll(ch, (unsigned char *)cls, cL)) {
            Tcl_Free(cls);
            goto fail_top;
        }
        cls[cL] = 0;

        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(cls);
            goto fail_top;
        }
        uint32_t  nSup   = le32(nb);
        Tcl_Obj **supers = NULL;
        if (nSup) {
            supers = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * nSup);
            for (uint32_t k = 0; k < nSup; ++k) {
                if (!ReadAll(ch, nb, 4)) {
                    while (k--)
                        Tcl_DecrRefCount(supers[k]);
                    Tcl_Free((char *)supers);
                    Tcl_Free(cls);
                    goto fail_top;
                }
                uint32_t sL = le32(nb);
                char    *sn = (char *)Tcl_Alloc(sL + 1);
                if (sL && !ReadAll(ch, (unsigned char *)sn, sL)) {
                    Tcl_Free(sn);
                    while (k--)
                        Tcl_DecrRefCount(supers[k]);
                    Tcl_Free((char *)supers);
                    Tcl_Free(cls);
                    goto fail_top;
                }
                sn[sL]    = 0;
                supers[k] = Tcl_NewStringObj(sn, -1);
                Tcl_IncrRefCount(supers[k]);
                Tcl_Free(sn);
            }
        }

        {
            Tcl_Obj *argv[3];
            argv[0] = Tcl_NewStringObj("::oo::class", -1);
            argv[1] = Tcl_NewStringObj("create", -1);
            argv[2] = Tcl_NewStringObj(cls, -1);
            for (int a = 0; a < 3; ++a)
                Tcl_IncrRefCount(argv[a]);
            int rc = Tcl_EvalObjv(interp, 3, argv, TCL_EVAL_DIRECT);
            for (int a = 0; a < 3; ++a)
                Tcl_DecrRefCount(argv[a]);
            if (rc != TCL_OK) {
                if (supers) {
                    for (uint32_t k = 0; k < nSup; ++k)
                        Tcl_DecrRefCount(supers[k]);
                    Tcl_Free((char *)supers);
                }
                Tcl_Free(cls);
                goto fail_top;
            }
        }

        if (nSup) {
            Tcl_Obj *lst = Tcl_NewListObj(0, NULL);
            Tcl_IncrRefCount(lst);
            for (uint32_t k = 0; k < nSup; ++k)
                Tcl_ListObjAppendElement(NULL, lst, supers[k]);
            Tcl_Obj *argv[4];
            argv[0] = Tcl_NewStringObj("::oo::define", -1);
            argv[1] = Tcl_NewStringObj(cls, -1);
            argv[2] = Tcl_NewStringObj("superclass", -1);
            argv[3] = lst;
            for (int a = 0; a < 4; ++a)
                Tcl_IncrRefCount(argv[a]);
            int rc2 = Tcl_EvalObjv(interp, 4, argv, TCL_EVAL_DIRECT);
            for (int a = 0; a < 4; ++a)
                Tcl_DecrRefCount(argv[a]);
            for (uint32_t k = 0; k < nSup; ++k)
                Tcl_DecrRefCount(supers[k]);
            Tcl_DecrRefCount(lst);
            Tcl_Free((char *)supers);
            if (rc2 != TCL_OK) {
                Tcl_Free(cls);
                goto fail_top;
            }
        }
        Tcl_Free(cls);
    }

    uint32_t numMethods = 0;
    {
        Tcl_Size got = Tcl_ReadRaw(ch, (char *)nb, (Tcl_Size)4);
        if (got == 0) {
            numMethods = 0;
        } else if (got < 0) {
            goto fail_top;
        } else {
            if (got < (Tcl_Size)4) {
                if (!ReadAll(ch, nb + got, (Tcl_Size)(4 - got))) {
                    goto fail_top;
                }
            }
            numMethods = le32(nb);
        }
    }

    for (uint32_t i = 0; i < numMethods; ++i) {
        if (!ReadAll(ch, nb, 4))
            goto fail_top;
        uint32_t cL  = le32(nb);
        char    *cls = (char *)Tcl_Alloc(cL + 1);
        if (cL && !ReadAll(ch, (unsigned char *)cls, cL)) {
            Tcl_Free(cls);
            goto fail_top;
        }
        cls[cL]            = 0;

        unsigned char kind = 0;
        R1(kind);

        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(cls);
            goto fail_top;
        }
        uint32_t nL = le32(nb);
        if (nL > TBCX_MAX_STR)
            goto fail_top;
        char *mname = NULL;
        if (nL) {
            mname = (char *)Tcl_Alloc(nL + 1);
            if (!ReadAll(ch, (unsigned char *)mname, nL)) {
                Tcl_Free(mname);
                Tcl_Free(cls);
                goto fail_top;
            }
            mname[nL] = 0;
        }

        if (!ReadAll(ch, nb, 4)) {
            if (mname)
                Tcl_Free(mname);
            Tcl_Free(cls);
            goto fail_top;
        }
        uint32_t aL   = le32(nb);
        char    *args = (char *)Tcl_Alloc(aL + 1);
        if (aL && !ReadAll(ch, (unsigned char *)args, aL)) {
            Tcl_Free(args);
            if (mname)
                Tcl_Free(mname);
            Tcl_Free(cls);
            goto fail_top;
        }
        args[aL] = 0;

        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(args);
            if (mname)
                Tcl_Free(mname);
            Tcl_Free(cls);
            goto fail_top;
        }
        uint32_t bL      = le32(nb);
        char    *bodyStr = (char *)Tcl_Alloc(bL + 1);
        if (bL && !ReadAll(ch, (unsigned char *)bodyStr, bL)) {
            Tcl_Free(bodyStr);
            Tcl_Free(args);
            if (mname)
                Tcl_Free(mname);
            Tcl_Free(cls);
            goto fail_top;
        }
        bodyStr[bL]                = 0;

        ByteCode      *pbc         = NULL;
        uint32_t       dummyLocals = 0;
        Tcl_Namespace *classNsPtr  = EnsureNamespace(interp, cls, nsPtr);
        if (ReadByteCode(interp, ch, classNsPtr, TBCX_SRC_PROC, NULL, H.format, &pbc, &dummyLocals) != TCL_OK) {
            Tcl_Free(bodyStr);
            Tcl_Free(args);
            if (mname)
                Tcl_Free(mname);
            Tcl_Free(cls);
            goto fail_top;
        }

        Tcl_Obj *bodyObj = Tcl_NewObj();
        Tcl_IncrRefCount(bodyObj);
        pbc->refCount = 1;
        ByteCodeSetInternalRep(bodyObj, tbcxTyBytecode, pbc);
        Tcl_InitStringRep(bodyObj, bodyStr, (Tcl_Size)bL);

        int rc = TCL_OK;
        if (kind == TBCX_METH_INST) {
            Tcl_Obj *argv[6];
            argv[0] = Tcl_NewStringObj("::oo::define", -1);
            argv[1] = Tcl_NewStringObj(cls, -1);
            argv[2] = Tcl_NewStringObj("method", -1);
            argv[3] = Tcl_NewStringObj(mname ? mname : "", -1);
            argv[4] = Tcl_NewStringObj(args, -1);
            argv[5] = bodyObj;
            for (int a = 0; a < 6; ++a)
                Tcl_IncrRefCount(argv[a]);
            rc = Tcl_EvalObjv(interp, 6, argv, TCL_EVAL_DIRECT);
            if (rc == TCL_OK) {
                tbcxRegisterMethodBody(cls, argv[3], /*isClass*/ 0, bodyObj);
            }
            for (int a = 0; a < 6; ++a)
                Tcl_DecrRefCount(argv[a]);
        } else if (kind == TBCX_METH_CLASS) {
            Tcl_Obj *argv[6];
            argv[0] = Tcl_NewStringObj("::oo::define", -1);
            argv[1] = Tcl_NewStringObj(cls, -1);
            argv[2] = Tcl_NewStringObj("classmethod", -1);
            argv[3] = Tcl_NewStringObj(mname ? mname : "", -1);
            argv[4] = Tcl_NewStringObj(args, -1);
            argv[5] = bodyObj;
            for (int a = 0; a < 6; ++a)
                Tcl_IncrRefCount(argv[a]);
            rc = Tcl_EvalObjv(interp, 6, argv, TCL_EVAL_DIRECT);
            if (rc == TCL_OK) {
                tbcxRegisterMethodBody(cls, argv[3], /*isClass*/ 1, bodyObj);
            }
            for (int a = 0; a < 6; ++a)
                Tcl_DecrRefCount(argv[a]);
        } else if (kind == TBCX_METH_CTOR) {
            Tcl_Obj *argv[5];
            argv[0] = Tcl_NewStringObj("::oo::define", -1);
            argv[1] = Tcl_NewStringObj(cls, -1);
            argv[2] = Tcl_NewStringObj("constructor", -1);
            argv[3] = Tcl_NewStringObj(args, -1);
            argv[4] = bodyObj;
            for (int a = 0; a < 5; ++a)
                Tcl_IncrRefCount(argv[a]);
            rc = Tcl_EvalObjv(interp, 5, argv, TCL_EVAL_DIRECT);
            for (int a = 0; a < 5; ++a)
                Tcl_DecrRefCount(argv[a]);
        } else {
            Tcl_Obj *argv[4];
            argv[0] = Tcl_NewStringObj("::oo::define", -1);
            argv[1] = Tcl_NewStringObj(cls, -1);
            argv[2] = Tcl_NewStringObj("destructor", -1);
            argv[3] = bodyObj;
            for (int a = 0; a < 4; ++a)
                Tcl_IncrRefCount(argv[a]);
            rc = Tcl_EvalObjv(interp, 4, argv, TCL_EVAL_DIRECT);
            for (int a = 0; a < 4; ++a)
                Tcl_DecrRefCount(argv[a]);
        }

        Tcl_DecrRefCount(bodyObj);
        Tcl_Free(bodyStr);
        Tcl_Free(args);
        if (mname)
            Tcl_Free(mname);
        Tcl_Free(cls);
        if (rc != TCL_OK)
            goto fail_top;
    }

    uint32_t numDefBinds = 0;
    if (!ReadAll(ch, nb, 4))
        goto fail_top;
    numDefBinds = le32(nb);

    for (uint32_t i = 0; i < numDefBinds; ++i) {
        unsigned char kind = 0; /* 0=PROC, 1=CLASS_METHOD, 2=INSTANCE_METHOD */
        if (!ReadAll(ch, &kind, 1))
            goto fail_top;
        /* A */
        if (!ReadAll(ch, nb, 4))
            goto fail_top;
        uint32_t LA = le32(nb);
        if (LA > TBCX_MAX_STR)
            goto fail_top;
        char *A = (char *)Tcl_Alloc(LA + 1);
        if (LA && !ReadAll(ch, (unsigned char *)A, LA)) {
            Tcl_Free(A);
            goto fail_top;
        }
        A[LA] = 0;
        /* B */
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(A);
            goto fail_top;
        }
        uint32_t LB = le32(nb);
        if (LB > TBCX_MAX_STR) {
            Tcl_Free(A);
            goto fail_top;
        }
        char *B = (char *)Tcl_Alloc(LB + 1);
        if (LB && !ReadAll(ch, (unsigned char *)B, LB)) {
            Tcl_Free(A);
            Tcl_Free(B);
            goto fail_top;
        }
        B[LB] = 0;
        /* body literal index */
        if (!ReadAll(ch, nb, 4)) {
            Tcl_Free(A);
            Tcl_Free(B);
            goto fail_top;
        }
        uint32_t litIx = le32(nb);

        if (!topBC || litIx >= (uint32_t)topBC->numLitObjects) {
            Tcl_Free(A);
            Tcl_Free(B);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: def-body literal index out of range", -1));
            goto fail_top;
        }
        Tcl_Obj *lit = topBC->objArrayPtr[litIx];
        if (!lit) {
            Tcl_Free(A);
            Tcl_Free(B);
            goto fail_top;
        }

        if (kind == 0) {
            /* PROC: A=proc FQN; B="" */
            Tcl_Command tok = Tcl_FindCommand(interp, A, NULL, 0);
            if (tok) {
                Command *cmdPtr  = (Command *)tok;
                Proc    *procPtr = (Proc *)cmdPtr->objClientData;
                if (procPtr && procPtr->bodyPtr && Tcl_ConvertToType(interp, procPtr->bodyPtr, tbcxTyBytecode) == TCL_OK) {
                    const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(procPtr->bodyPtr, tbcxTyBytecode);
                    if (ir && ir->twoPtrValue.ptr1) {
                        ByteCode *pbcIR = (ByteCode *)ir->twoPtrValue.ptr1;
                        pbcIR->refCount++;
                        ByteCodeSetInternalRep(lit, tbcxTyBytecode, pbcIR);
                    }
                } else {
                    Tcl_ResetResult(interp);
                }
            }
        } else {
            /* CLASS/INSTANCE METHOD: A=class FQN; B=method name */
            if (tbcxMethRegInit) {
                Tcl_DString k;
                Tcl_DStringInit(&k);
                Tcl_DStringAppend(&k, "K:", 2);
                Tcl_DStringAppend(&k, (kind == 1) ? "1:" : "0:", 2);
                Tcl_DStringAppend(&k, A, -1);
                Tcl_DStringAppend(&k, "\x1f", 1);
                Tcl_DStringAppend(&k, B, -1);
                Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&tbcxMethReg, Tcl_DStringValue(&k));
                if (hPtr) {
                    Tcl_Obj *bodyObj = (Tcl_Obj *)Tcl_GetHashValue(hPtr);
                    if (Tcl_ConvertToType(interp, bodyObj, tbcxTyBytecode) == TCL_OK) {
                        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(bodyObj, tbcxTyBytecode);
                        if (ir && ir->twoPtrValue.ptr1) {
                            ByteCode *pbcIR = (ByteCode *)ir->twoPtrValue.ptr1;
                            pbcIR->refCount++;
                            ByteCodeSetInternalRep(lit, tbcxTyBytecode, pbcIR);
                        }
                    } else {
                        Tcl_ResetResult(interp);
                    }
                }
                Tcl_DStringFree(&k);
            }
        }
        Tcl_Free(A);
        Tcl_Free(B);
    }

    uint32_t numNsBodies = 0;
    {
        Tcl_Size got = Tcl_ReadRaw(ch, (char *)nb, (Tcl_Size)4);
        if (got == 0) {
            numNsBodies = 0;
        } else if (got < 0) {
            goto fail_top;
        } else {
            if (got < (Tcl_Size)4) {
                if (!ReadAll(ch, nb + got, (Tcl_Size)(4 - got))) {
                    goto fail_top;
                }
            }
            numNsBodies = le32(nb);
        }
    }

    for (uint32_t i = 0; i < numNsBodies; ++i) {
        if (!ReadAll(ch, nb, 4))
            goto fail_top;
        uint32_t litIx = le32(nb);

        if (!ReadAll(ch, nb, 4))
            goto fail_top;
        uint32_t nsL    = le32(nb);
        char    *nsName = (char *)Tcl_Alloc(nsL + 1);
        if (nsL && !ReadAll(ch, (unsigned char *)nsName, nsL)) {
            Tcl_Free(nsName);
            goto fail_top;
        }
        nsName[nsL]              = 0;

        Tcl_Namespace *bodyNsPtr = EnsureNamespace(interp, (nsL ? nsName : NULL), nsPtr);

        ByteCode      *nspbc     = NULL;
        uint32_t       dummyLocs = 0;
        if (ReadByteCode(interp, ch, bodyNsPtr, TBCX_SRC_PROC, NULL, H.format, &nspbc, &dummyLocs) != TCL_OK) {
            Tcl_Free(nsName);
            goto fail_top;
        }
        Tcl_Free(nsName);

        if (!topBC || litIx >= (uint32_t)topBC->numLitObjects) {
            FreeLoadedByteCode(nspbc);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: NS body literal index out of range", -1));
            goto fail_top;
        }
        Tcl_Obj *lit = topBC->objArrayPtr[litIx];
        if (!lit) {
            FreeLoadedByteCode(nspbc);
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: NS body literal missing", -1));
            goto fail_top;
        }
        nspbc->refCount = 1;
        ByteCodeSetInternalRep(lit, tbcxTyBytecode, nspbc);
    }

    topBC->compileEpoch = ((Interp *)interp)->compileEpoch;
    topBC->nsEpoch      = topBC->nsPtr ? topBC->nsPtr->resolverEpoch : 0;

    Tcl_Obj *scriptObj  = Tcl_NewObj();
    if (scriptObj == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: OOM creating script object", -1));
        goto fail_top;
    }
    Tcl_IncrRefCount(scriptObj);
    topBC->refCount = 1;
    ByteCodeSetInternalRep(scriptObj, tbcxTyBytecode, topBC);
    topBC    = NULL;

    int code = EvalInNamespace(interp, nsPtr, scriptObj);

    Tcl_DecrRefCount(scriptObj);
    tbcxClearMethodRegistry();

    if (code != TCL_OK) {
        return code;
    }
    return TCL_OK;

io_fail:
    tbcxClearMethodRegistry();
    Tcl_SetObjResult(interp, Tcl_NewStringObj("unexpected EOF in bytecode block", -1));
    return TCL_ERROR;

fail_top:
    if (Tcl_GetCharLength(Tcl_GetObjResult(interp)) == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: load from channel failed", -1));
    } else {
        Tcl_AppendObjToErrorInfo(interp, Tcl_NewStringObj("\n    (while loading TBCX)", -1));
    }
    if (topBC) {
        FreeLoadedByteCode(topBC);
    }
    tbcxClearMethodRegistry();
    return TCL_ERROR;
}

/* ==========================================================================
 * Tcl commands
 * ========================================================================== */

/*
 * Tbcx_LoadFileObjCmd
 * tbcx::loadfile path — load a TBCX file from disk.
 */

int Tbcx_LoadFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "path");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_OpenFileChannel(interp, Tcl_GetString(objv[1]), "r", 0);
    if (!ch)
        return TCL_ERROR;
    int res = LoadFromChannel(interp, ch, Tcl_GetCurrentNamespace(interp));
    Tcl_Close(NULL, ch);
    return res;
}

/*
 * Tbcx_LoadChanObjCmd
 * tbcx::loadchan channel — load a TBCX stream from an existing channel.
 */

int Tbcx_LoadChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channel");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);
    if (!ch) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid channel", -1));
        return TCL_ERROR;
    }
    return LoadFromChannel(interp, ch, Tcl_GetCurrentNamespace(interp));
}
