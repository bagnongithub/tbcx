/*
 *tbcxsave.c
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct ProcEntry {
    Tcl_Obj *name;
    Tcl_Obj *ns;
    Tcl_Obj *args;
    Tcl_Obj *body;
} ProcEntry;

typedef struct DynCap {
    ProcEntry *list;
    int        count;
    int        cap;
} DynCap;

typedef struct ClassEntry {
    Tcl_Obj  *clsFqn;
    Tcl_Obj **supers;
    int       nSupers;
} ClassEntry;

typedef struct MethEntry {
    Tcl_Obj      *clsFqn;
    Tcl_Obj      *methName;
    Tcl_Obj      *args;
    Tcl_Obj      *body;
    unsigned char kind;
} MethEntry;

typedef struct DynClassCap {
    ClassEntry *list;
    int         count;
    int         cap;
} DynClassCap;

typedef struct DynMethCap {
    MethEntry *list;
    int        count;
    int        cap;
} DynMethCap;

typedef struct OoCap {
    DynClassCap classes;
    DynMethCap  methods;
} OoCap;

typedef struct NsBodyRec {
    uint32_t litIndex;
    Tcl_Obj *ns;
    Tcl_Obj *body;
} NsBodyRec;

typedef struct DynNsBodyCap {
    NsBodyRec *list;
    int        count;
    int        cap;
} DynNsBodyCap;

typedef struct DefBodyBind {
    unsigned char kind;      /* 0=PROC, 1=CLASS_METHOD, 2=INSTANCE_METHOD */
    Tcl_Obj      *A;         /* PROC: proc FQN; METHODs: class FQN */
    Tcl_Obj      *B;         /* PROC: "";       METHODs: method name */
    uint32_t      bodyLitIx; /* index into top-level bc->objArrayPtr */
} DefBodyBind;

typedef struct DefBodyBindVec {
    DefBodyBind *list;
    int          count;
    int          cap;
} DefBodyBindVec;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static int        AssertAuxCoverage(Tcl_Interp *interp, const ByteCode *bc, uint32_t *outCount);
static Tcl_Obj   *BuildBodyFilteredInNs(Tcl_Interp *, Tcl_Obj *, Tcl_Obj *, ProcEntry *, int);
static Tcl_Obj   *BuildTopLevelFiltered(Tcl_Interp *, Tcl_Obj *, ProcEntry *, int);
static int        CanWriteAux(const AuxData *ad);
static int        CollectOOByEval(Tcl_Interp *interp, Tcl_Obj *script, DynClassCap *outClasses, DynMethCap *outMethods);
static int        CollectOOFromScript(Tcl_Interp *interp, Tcl_Obj *script, Tcl_Obj *nsDefault, DynClassCap *outClasses, DynMethCap *outMethods);
static int        CollectProcsByEval(Tcl_Interp *interp, Tcl_Obj *script, ProcEntry **outEntries, int *outCount);
static int        CollectProcsFromScript(Tcl_Interp *, Tcl_Obj *, Tcl_Obj *, ProcEntry **, int *);
static int        CompileMethodBodyForSave(Tcl_Interp *interp, Tcl_Obj *clsFqn, Tcl_Obj *methName, unsigned char kind, Tcl_Obj *argsObj, Tcl_Obj *bodyObj, Tcl_Obj **outBodyObj, int *outNumLocals);
static int        CompileNsBodyForSave(Tcl_Interp *interp, Tcl_Obj *nsFqn, Tcl_Obj *bodyObj, Tcl_Obj **outBodyObj, int *outNumLocals);
static uint32_t   CountWritableAux(const ByteCode *bc);
static int        EmitTopLevelAndProcs(Tcl_Interp *, Tcl_Obj *, Tcl_Channel);
static Tcl_Obj   *FilterScriptInNs(Tcl_Interp *, Tcl_Obj *, Tcl_Obj *, ProcEntry *, int, int dropAllProcForms, int inClassBody);
static int        FindEmptyStringLiteralIndex(const ByteCode *bc);
static void       FreeClassEntryArray(ClassEntry *arr, int n);
static void       FreeMethEntryArray(MethEntry *arr, int n);
static void       FreeProcEntryArray(ProcEntry *arr, int n);
static int        GetByteCodeFromScript(Tcl_Interp *, Tcl_Obj *, ByteCode **);
static int        IsClassBodyMethodCmd(const char *s, int len);
static int        IsNamespaceCmdName(const char *s, int len);
static int        IsOoClassCmdName(const char *s, int len);
static int        IsOoDefineCmdName(const char *s, int len);
static int        IsProcCmdName(const char *s, int len);
static uint32_t   LocalsFromAux(const ByteCode *bc);
static int        MergeClassLists(DynClassCap *base, DynClassCap *extra);
static int        MergeMethLists(DynMethCap *base, DynMethCap *extra);
static int        MergeProcLists(Tcl_Interp *interp, ProcEntry **base, int *nBase, ProcEntry *extra, int nExtra);
static int        NameEndsWith(const char *s, int len, const char *tail);
static int        NeutralizeProcCreates(ByteCode *bc, Tcl_Obj *nsPrefix, ProcEntry *procs, int nProcs, unsigned char **outPatched, size_t *outLen, int *outRewrites);
static Tcl_Obj   *QualifyInNs(Tcl_Obj *nameObj, Tcl_Obj *nsPrefix);
static int        ReadWholeFile(Tcl_Interp *, const char *, Tcl_Obj **);
static int        ScanNamespaceEvalBodies(const ByteCode *bc, DynNsBodyCap *out);
static int        TokenEquals(const char *s, int len, const char *lit);
static int        UnknownGuardCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]);
static Tcl_Token *WordToken(const Tcl_Parse *p, int wordIndex);
static void       WriteAux(Tcl_Channel ch, const AuxData *ad);
static void       WriteAux_DictUpd(Tcl_Channel ch, const DictUpdateInfo *info);
static void       WriteAux_Foreach(Tcl_Channel ch, const ForeachInfo *info);
static void       WriteAux_JTNum(Tcl_Channel ch, const JumptableInfo *info);
static void       WriteAux_JTStr(Tcl_Channel ch, const JumptableInfo *info);
static void       WriteHeaderEx(Tcl_Channel ch, const ByteCode *bc, uint32_t numAuxToWrite);
static void       WriteLiteral(Tcl_Channel ch, Tcl_Obj *o);

/* ==========================================================================
 * Containers (OO capture)
 * ========================================================================== */

static void DynNsInit(DynNsBodyCap *c) {
    c->list  = NULL;
    c->count = 0;
    c->cap   = 0;
}

static void DynNsPush(DynNsBodyCap *c, const NsBodyRec *e) {
    if (c->count == c->cap) {
        c->cap  = c->cap ? c->cap * 2 : 8;
        c->list = (NsBodyRec *)Tcl_Realloc((char *)c->list, sizeof(NsBodyRec) * (size_t)c->cap);
    }
    c->list[c->count++] = *e;
}

static void FreeNsBodyArray(NsBodyRec *arr, int n) {
    if (!arr)
        return;
    for (int i = 0; i < n; ++i) {
        Tcl_DecrRefCount(arr[i].ns);
        Tcl_DecrRefCount(arr[i].body);
    }
    Tcl_Free((char *)arr);
}

static int NsBodiesContainsLit(const DynNsBodyCap *c, uint32_t idx) {
    if (!c || !c->list)
        return 0;
    for (int i = 0; i < c->count; ++i)
        if (c->list[i].litIndex == idx)
            return 1;
    return 0;
}

/*
 * DynClassInit
 * Initialize a dynamic array for captured classes.
 */

static void DynClassInit(DynClassCap *c) {
    c->list  = NULL;
    c->count = 0;
    c->cap   = 0;
}

/*
 * DynMethInit
 * Initialize a dynamic array for captured methods.
 */

static void DynMethInit(DynMethCap *c) {
    c->list  = NULL;
    c->count = 0;
    c->cap   = 0;
}

/*
 * DynClassPush
 * Append a ClassEntry to the dynamic array, growing as needed.
 */

static void DynClassPush(DynClassCap *c, const ClassEntry *e) {
    if (c->count == c->cap) {
        c->cap  = c->cap ? c->cap * 2 : 8;
        c->list = (ClassEntry *)Tcl_Realloc((char *)c->list, sizeof(ClassEntry) * (size_t)c->cap);
    }
    c->list[c->count++] = *e;
}

/*
 * DynMethPush
 * Append a MethEntry to the dynamic array, growing as needed.
 */

static void DynMethPush(DynMethCap *c, const MethEntry *e) {
    if (c->count == c->cap) {
        c->cap  = c->cap ? c->cap * 2 : 8;
        c->list = (MethEntry *)Tcl_Realloc((char *)c->list, sizeof(MethEntry) * (size_t)c->cap);
    }
    c->list[c->count++] = *e;
}

/* ==========================================================================
 * Low-level I/O helpers
 * ========================================================================== */

/*
 * wr
 * Raw write to a Tcl channel; panics on failure.
 */

static inline void wr(Tcl_Channel ch, const void *buf, Tcl_Size n) {
    const char *p = (const char *)buf;
    while (n) {
        Tcl_Size w = Tcl_WriteRaw(ch, p, (Tcl_Size)n);
        if (w <= 0)
            Tcl_Panic("write failure");
        p += w;
        n -= (Tcl_Size)w;
    }
}

/*
 * wr1
 * Write 1 byte to the channel.
 */

static inline void wr1(Tcl_Channel ch, unsigned char v) {
    wr(ch, &v, 1);
}

/*
 * wr4
 * Write a 4-byte little-endian value.
 */

static inline void wr4(Tcl_Channel ch, uint32_t v) {
    unsigned char b[4];
    putle32(b, v);
    wr(ch, b, 4);
}

/*
 * wr8
 * Write an 8-byte little-endian value.
 */

static inline void wr8(Tcl_Channel ch, uint64_t v) {
    unsigned char b[8];
    putle64(b, v);
    wr(ch, b, 8);
}

/* ==========================================================================
 * Serialization: header/literals/aux
 * ========================================================================== */

/*
 * WriteHeaderEx
 * Serialize the TBCX file header from a ByteCode block.
 */

static void WriteHeaderEx(Tcl_Channel ch, const ByteCode *bc, uint32_t numAuxToWrite) {
    unsigned char b[sizeof(TbcxHeader)];
    memset(b, 0, sizeof b);

    putle32(b + 0, TBCX_MAGIC);
    putle32(b + 4, TBCX_FORMAT);
    putle32(b + 8, TBCX_FLAGS_V1);
    putle64(b + 12, (uint64_t)bc->numCodeBytes);
    putle32(b + 20, (uint32_t)bc->numCommands);
    putle32(b + 24, (uint32_t)bc->numExceptRanges);
    putle32(b + 28, (uint32_t)bc->numLitObjects);
    putle32(b + 32, (uint32_t)numAuxToWrite);
    uint32_t topNumLocals = (uint32_t)(bc->procPtr ? bc->procPtr->numCompiledLocals : LocalsFromAux(bc));
    putle32(b + 36, topNumLocals);
    putle32(b + 40, (uint32_t)bc->maxStackDepth);
    wr(ch, b, sizeof b);
}

/*
 * WriteLiteral
 * Serialize a Tcl_Obj literal to TBCX.
 */

static void WriteLiteral(Tcl_Channel ch, Tcl_Obj *o) {
    const Tcl_ObjType *tp = o->typePtr;

    Tcl_WideInt        w;
    if (Tcl_GetWideIntFromObj(NULL, o, &w) == TCL_OK) {
        wr1(ch, (unsigned char)TBCX_LIT_WIDEINT);
        wr8(ch, (uint64_t)w);
        return;
    }

    if (tp == tbcxTyBoolean) {
        int b = 0;
        if (Tcl_GetBooleanFromObj(NULL, o, &b) == TCL_OK) {
            wr1(ch, (unsigned char)TBCX_LIT_BOOLEAN);
            wr1(ch, (unsigned char)(b ? 1 : 0));
            return;
        }
    }

    if (tp == tbcxTyDouble) {
        double d;
        Tcl_GetDoubleFromObj(NULL, o, &d);
        wr1(ch, (unsigned char)TBCX_LIT_DOUBLE);
        uint64_t bits = 0;
        memcpy(&bits, &d, 8);
        wr8(ch, bits);
        return;
    }

    if ((tp == tbcxTyBignum) || (tp && strcmp(tp->name, "bignum") == 0)) {
        mp_int big;
        if (Tcl_GetBignumFromObj(NULL, o, &big) == TCL_OK) {
            int bits = (int)mp_count_bits(&big);
            if (bits <= 63) {
                /* Fits in signed 64-bit (including negatives): emit WIDEINT */
                Tcl_WideInt w;
                if (Tcl_GetWideIntFromObj(NULL, o, &w) == TCL_OK) {
                    wr1(ch, (unsigned char)TBCX_LIT_WIDEINT);
                    wr8(ch, (uint64_t)w);
                    mp_clear(&big);
                    return;
                }
            }
            if (!mp_isneg(&big) && bits <= 64) {
                unsigned char be[8]   = {0};
                size_t        n       = mp_ubin_size(&big);
                size_t        written = 0;
                if (n) {
                    mp_err rc2 = mp_to_ubin(&big, be + (8 - n), n, &written);
                    if (rc2 != MP_OKAY || written != n) {
                        mp_clear(&big);
                        Tcl_Panic("tommath: mp_to_ubin(short) failed");
                    }
                }
                uint64_t v = 0;
                for (int i = 0; i < 8; i++)
                    v = (v << 8) | be[i];
                wr1(ch, (unsigned char)TBCX_LIT_WIDEUINT);
                wr8(ch, v);
                mp_clear(&big);
                return;
            }
            size_t         need    = mp_ubin_size(&big);
            unsigned char *buf     = (unsigned char *)Tcl_Alloc(need ? need : 1);
            size_t         written = 0;
            if (need) {
                mp_err rc = mp_to_ubin(&big, buf, need, &written);
                if (rc != MP_OKAY) {
                    Tcl_Free((char *)buf);
                    mp_clear(&big);
                    Tcl_Panic("tommath: mp_to_ubin failed");
                }
            }
            wr1(ch, (unsigned char)TBCX_LIT_BIGNUM);
            wr1(ch, (unsigned char)(mp_isneg(&big) ? 1 : 0));
            wr4(ch, (uint32_t)written);
            if (written)
                wr(ch, buf, written);
            Tcl_Free((char *)buf);
            mp_clear(&big);
            return;
        }
    }

    if (tp == tbcxTyByteArray) {
        Tcl_Size       blen = 0;
        unsigned char *bp   = Tcl_GetByteArrayFromObj(o, &blen);
        wr1(ch, (unsigned char)TBCX_LIT_BYTEARR);
        wr4(ch, (uint32_t)blen);
        if (blen)
            wr(ch, bp, (size_t)blen);
        return;
    }

    if (tp == tbcxTyList) {
        Tcl_Size  n     = 0;
        Tcl_Obj **elems = NULL;
        if (Tcl_ListObjGetElements(NULL, o, &n, &elems) == TCL_OK) {
            wr1(ch, (unsigned char)TBCX_LIT_LIST);
            wr4(ch, (uint32_t)n);
            for (Tcl_Size i = 0; i < n; ++i)
                WriteLiteral(ch, elems[i]);
            return;
        }
    }

    if (tp == tbcxTyDict) {
        Tcl_DictSearch s;
        Tcl_Obj       *k, *v;
        int            done = 0;
        Tcl_Size       n    = 0;
        Tcl_DictObjSize(NULL, o, &n);
        wr1(ch, (unsigned char)TBCX_LIT_DICT);
        wr4(ch, (uint32_t)n);
        if (Tcl_DictObjFirst(NULL, o, &s, &k, &v, &done) == TCL_OK) {
            while (!done) {
                WriteLiteral(ch, k);
                WriteLiteral(ch, v);
                Tcl_DictObjNext(&s, &k, &v, &done);
            }
        }
        return;
    }

    Tcl_Size    slen = 0;
    const char *s    = Tcl_GetStringFromObj(o, &slen);
    wr1(ch, (unsigned char)TBCX_LIT_STRING);
    wr4(ch, (uint32_t)slen);
    if (slen)
        wr(ch, s, (size_t)slen);
}

/*
 * WriteAux_JTStr
 * Serialize string jump-table AuxData.
 */

static void WriteAux_JTStr(Tcl_Channel ch, const JumptableInfo *info) {
    Tcl_HashSearch hs;
    Tcl_HashTable *ht = (Tcl_HashTable *)&info->hashTable;
    Tcl_HashEntry *h;
    wr1(ch, (unsigned char)TBCX_AUX_JT_STR);
    wr4(ch, (uint32_t)ht->numEntries);
    for (h = Tcl_FirstHashEntry(ht, &hs); h; h = Tcl_NextHashEntry(&hs)) {
        const char *key = (const char *)Tcl_GetHashKey(ht, h);
        uint32_t    L   = (uint32_t)strlen(key);
        wr4(ch, L);
        if (L)
            wr(ch, key, L);
        wr4(ch, (uint32_t)(intptr_t)Tcl_GetHashValue(h));
    }
}

/*
 * WriteAux_JTNum
 * Serialize numeric jump-table AuxData.
 */

static void WriteAux_JTNum(Tcl_Channel ch, const JumptableInfo *info) {
    Tcl_HashSearch hs;
    Tcl_HashTable *ht = (Tcl_HashTable *)&info->hashTable;
    Tcl_HashEntry *h;
    wr1(ch, (unsigned char)TBCX_AUX_JT_NUM);
    wr4(ch, (uint32_t)ht->numEntries);
    for (h = Tcl_FirstHashEntry(ht, &hs); h; h = Tcl_NextHashEntry(&hs)) {
        intptr_t key = (intptr_t)Tcl_GetHashKey(ht, h);
        wr8(ch, (uint64_t)key);
        wr4(ch, (uint32_t)(intptr_t)Tcl_GetHashValue(h));
    }
}

/*
 * WriteAux_DictUpd
 * Serialize DictUpdate AuxData.
 */

static void WriteAux_DictUpd(Tcl_Channel ch, const DictUpdateInfo *info) {
    wr1(ch, (unsigned char)TBCX_AUX_DICTUPD);
    wr4(ch, (uint32_t)info->length);
    for (Tcl_Size i = 0; i < info->length; ++i)
        wr4(ch, (uint32_t)info->varIndices[i]);
}

/*
 * WriteAux_Foreach
 * Serialize Foreach AuxData (new/legacy).
 */

static void WriteAux_Foreach(Tcl_Channel ch, const ForeachInfo *info) {
    wr1(ch, (unsigned char)TBCX_AUX_NEWFORE);
    wr4(ch, (uint32_t)info->numLists);
    wr4(ch, (uint32_t)info->firstValueTemp);
    wr4(ch, (uint32_t)info->loopCtTemp);
    for (Tcl_Size i = 0; i < info->numLists; ++i) {
        const ForeachVarList *vl = info->varLists[i];
        wr4(ch, (uint32_t)vl->numVars);
        for (Tcl_Size j = 0; j < vl->numVars; ++j)
            wr4(ch, (uint32_t)vl->varIndexes[j]);
    }
}

/*
 * WriteAux
 * Dispatch serializer for supported AuxData types.
 */

static void WriteAux(Tcl_Channel ch, const AuxData *ad) {
    if (ad->type == tbcxAuxJTStr) {
        WriteAux_JTStr(ch, (const JumptableInfo *)ad->clientData);
    } else if (ad->type == tbcxAuxJTNum) {
        WriteAux_JTNum(ch, (const JumptableInfo *)ad->clientData);
    } else if (ad->type == tbcxAuxDictUpdate) {
        WriteAux_DictUpd(ch, (const DictUpdateInfo *)ad->clientData);
    } else if (ad->type == tbcxAuxForeach) {
        WriteAux_Foreach(ch, (const ForeachInfo *)ad->clientData);
    } else if (ad->type && ad->type->name && strcmp(ad->type->name, "ForeachInfo") == 0) {
        WriteAux_Foreach(ch, (const ForeachInfo *)ad->clientData);
    }
}

/* ==========================================================================
 * Bytecode analysis/coverage
 * ========================================================================== */

/*
 * CanWriteAux
 * Return non-zero if an AuxData item is supported by serializer.
 */

static int CanWriteAux(const AuxData *ad) {
    return (ad->type == tbcxAuxJTStr) || (ad->type == tbcxAuxJTNum) || (ad->type == tbcxAuxDictUpdate) || (ad->type == tbcxAuxForeach) ||
           (ad->type && ad->type->name && strcmp(ad->type->name, "ForeachInfo") == 0);
}

/*
 * CountWritableAux
 * Count the AuxData items in a ByteCode that we can serialize.
 */

static uint32_t CountWritableAux(const ByteCode *bc) {
    uint32_t c = 0;
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; ++i)
        if (CanWriteAux(&bc->auxDataArrayPtr[i]))
            ++c;
    return c;
}

/*
 * AssertAuxCoverage
 * Validate all AuxData in ByteCode are supported; build error message otherwise.
 */

static int AssertAuxCoverage(Tcl_Interp *interp, const ByteCode *bc, uint32_t *outCount) {
    uint32_t c = CountWritableAux(bc);
    if (outCount)
        *outCount = c;
    if ((Tcl_Size)c == bc->numAuxDataItems)
        return TCL_OK;
    Tcl_Obj *msg   = Tcl_NewStringObj("tbcx: unsupported AuxData types in bytecode: ", -1);
    int      first = 1;
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; ++i) {
        if (!CanWriteAux(&bc->auxDataArrayPtr[i])) {
            const char *nm = bc->auxDataArrayPtr[i].type ? bc->auxDataArrayPtr[i].type->name : "(null)";
            if (!first)
                Tcl_AppendToObj(msg, ", ", 2);
            else
                first = 0;
            Tcl_AppendToObj(msg, nm, -1);
        }
    }
    Tcl_SetObjResult(interp, msg);
    return TCL_ERROR;
}

/*
 * LocalsFromAux
 * Infer required number of compiled locals from AuxData when no procPtr.
 */

static uint32_t LocalsFromAux(const ByteCode *bc) {
    uint32_t maxIx = 0u;
    int      seen  = 0;

    if (!bc || !bc->auxDataArrayPtr || bc->numAuxDataItems <= 0)
        return 0u;

    for (Tcl_Size i = 0; i < bc->numAuxDataItems; ++i) {
        const AuxData *ad = &bc->auxDataArrayPtr[i];
        if (!ad->type)
            continue;
        if (ad->type == tbcxAuxForeach || (ad->type->name && strcmp(ad->type->name, "ForeachInfo") == 0)) {
            const ForeachInfo *fi = (const ForeachInfo *)ad->clientData;
            if (fi) {
                int v = 0;
                v     = (int)fi->firstValueTemp;
                if (v >= 0 && (uint32_t)v > maxIx)
                    maxIx = (uint32_t)v;
                v = (int)fi->loopCtTemp;
                if (v >= 0 && (uint32_t)v > maxIx)
                    maxIx = (uint32_t)v;
                for (Tcl_Size j = 0; j < fi->numLists; ++j) {
                    const ForeachVarList *vl = fi->varLists[j];
                    for (Tcl_Size k = 0; vl && k < vl->numVars; ++k) {
                        v = (int)vl->varIndexes[k];
                        if (v >= 0 && (uint32_t)v > maxIx)
                            maxIx = (uint32_t)v;
                    }
                }
                seen = 1;
            }
        } else if (ad->type == tbcxAuxDictUpdate || (ad->type->name && strcmp(ad->type->name, "DictUpdateInfo") == 0)) {
            const DictUpdateInfo *di = (const DictUpdateInfo *)ad->clientData;
            if (di) {
                for (Tcl_Size j = 0; j < di->length; ++j) {
                    int v = (int)di->varIndices[j];
                    if (v >= 0 && (uint32_t)v > maxIx)
                        maxIx = (uint32_t)v;
                }
                seen = 1;
            }
        }
    }
    return seen ? (maxIx + 1u) : 0u;
}

/* ==========================================================================
 * Compilation helpers
 * ========================================================================== */

static int ScanNamespaceEvalBodies(const ByteCode *bc, DynNsBodyCap *out) {
    DynNsInit(out);
    if (!bc || !bc->codeStart || bc->numCodeBytes == 0 || bc->numLitObjects <= 0) {
        return TCL_OK;
    }

    const unsigned char *code    = bc->codeStart;
    size_t               codeLen = (size_t)bc->numCodeBytes;

    for (size_t i = 0; i + 5 <= codeLen; ++i) {
        if (code[i] != (unsigned char)INST_INVOKE_STK)
            continue;
        unsigned nargs = TclGetUInt4AtPtr(code + i + 1);
        if (nargs != 4u)
            continue;

        size_t p = i;
        struct {
            size_t   pos;
            unsigned lit;
        } win[4];
        int ok = 1;
        for (int k = 3; k >= 0; --k) {
            if (p >= 5 && code[p - 5] == (unsigned char)INST_PUSH) {
                win[k].pos = p - 5;
                win[k].lit = (unsigned)TclGetUInt4AtPtr(code + p - 4);
                p -= 5;
            } else {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        if (win[0].lit >= (unsigned)bc->numLitObjects)
            continue;
        Tcl_Size    ln0 = 0;
        const char *s0  = Tcl_GetStringFromObj(bc->objArrayPtr[win[0].lit], &ln0);
        if (!NameEndsWith(s0, (int)ln0, "namespace"))
            continue;

        if (win[1].lit >= (unsigned)bc->numLitObjects)
            continue;
        Tcl_Size    ln1 = 0;
        const char *s1  = Tcl_GetStringFromObj(bc->objArrayPtr[win[1].lit], &ln1);
        if (!(ln1 == 4 && memcmp(s1, "eval", 4) == 0))
            continue;

        unsigned litNs   = win[2].lit;
        unsigned litBody = win[3].lit;
        if (litNs >= (unsigned)bc->numLitObjects || litBody >= (unsigned)bc->numLitObjects)
            continue;

        NsBodyRec rec;
        rec.litIndex = (uint32_t)litBody;
        rec.ns       = Tcl_DuplicateObj(bc->objArrayPtr[litNs]);
        Tcl_IncrRefCount(rec.ns);
        rec.body = Tcl_DuplicateObj(bc->objArrayPtr[litBody]);
        Tcl_IncrRefCount(rec.body);
        DynNsPush(out, &rec);
    }
    return TCL_OK;
}

static int CompileNsBodyForSave(Tcl_Interp *interp, Tcl_Obj *nsFqn, Tcl_Obj *bodyObj, Tcl_Obj **outBodyObj, int *outNumLocals) {
    if (!nsFqn || !bodyObj)
        return TCL_ERROR;

    Tcl_Namespace *nsPtr = Tcl_FindNamespace(interp, Tcl_GetString(nsFqn), NULL, 0);
    if (!nsPtr)
        nsPtr = Tcl_CreateNamespace(interp, Tcl_GetString(nsFqn), NULL, NULL);
    if (!nsPtr)
        return TCL_ERROR;

    Tcl_CallFrame frame;
    if (Tcl_PushCallFrame(interp, &frame, nsPtr, /*isProcCallFrame*/ 0) != TCL_OK) {
        return TCL_ERROR;
    }

    Tcl_Obj *dup = Tcl_DuplicateObj(bodyObj);
    Tcl_IncrRefCount(dup);

    ByteCode *bc = NULL;
    int       rc = GetByteCodeFromScript(interp, dup, &bc);
    if (rc == TCL_OK) {
        *outBodyObj   = dup;
        *outNumLocals = (int)LocalsFromAux(bc);
    } else {
        Tcl_DecrRefCount(dup);
    }
    Tcl_PopCallFrame(interp);
    return rc;
}

/*
 * CompileProcBodyForSave
 * Compile a proc definition into bytecode using the core compiler (via TclCreateProc).
 */

static int CompileProcBodyForSave(Tcl_Interp *interp, Tcl_Obj *nameObj, Tcl_Obj *argsObj, Tcl_Obj *bodyObj, Tcl_Obj **outBodyObj, int *outNumLocals) {
    Namespace *nsPtr   = (Namespace *)Tcl_GetCurrentNamespace(interp);
    Proc      *procPtr = NULL;
    int        rc      = TCL_ERROR;

    Tcl_IncrRefCount(nameObj);
    Tcl_IncrRefCount(argsObj);
    Tcl_IncrRefCount(bodyObj);

    if (TclCreateProc(interp, nsPtr, Tcl_GetString(nameObj), argsObj, bodyObj, &procPtr) != TCL_OK) {
        goto done;
    }
    if (Tcl_ConvertToType(interp, procPtr->bodyPtr, tbcxTyBytecode) != TCL_OK) {
        goto done;
    }
    {
        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(procPtr->bodyPtr, tbcxTyBytecode);
        if (!ir)
            goto done;
        Tcl_IncrRefCount(procPtr->bodyPtr);
        *outBodyObj   = procPtr->bodyPtr;
        *outNumLocals = procPtr->numCompiledLocals;
    }
    rc = TCL_OK;

done:
    Tcl_DeleteCommand(interp, Tcl_GetString(nameObj));
    Tcl_DecrRefCount(nameObj);
    Tcl_DecrRefCount(argsObj);
    Tcl_DecrRefCount(bodyObj);
    return rc;
}

/*
 * CompileMethodBodyForSave
 * Compile an oo method/ctor/dtor body by proxying through temporary proc.
 */

static int CompileMethodBodyForSave(Tcl_Interp *interp, Tcl_Obj *clsFqn, Tcl_Obj *methName, unsigned char kind, Tcl_Obj *argsObj, Tcl_Obj *bodyObj, Tcl_Obj **outBodyObj, int *outNumLocals) {
    static unsigned long tbcxMethSeq = 0;
    Tcl_Obj             *name        = Tcl_DuplicateObj(clsFqn);
    Tcl_IncrRefCount(name);
    Tcl_AppendToObj(name, "::", 2);
    Tcl_AppendToObj(name, "__tbcx_m_", -1);
    switch (kind) {
    case TBCX_METH_CTOR:
        Tcl_AppendToObj(name, "ctor", -1);
        break;
    case TBCX_METH_DTOR:
        Tcl_AppendToObj(name, "dtor", -1);
        break;
    default:
        Tcl_AppendToObj(name, "m_", -1);
        if (methName)
            Tcl_AppendObjToObj(name, methName);
        break;
    }
    Tcl_AppendPrintfToObj(name, "_%lu", ++tbcxMethSeq);

    int rc = CompileProcBodyForSave(interp, name, argsObj, bodyObj, outBodyObj, outNumLocals);
    Tcl_DecrRefCount(name);
    return rc;
}

/*
 * GetByteCodeFromScript
 * Fetch the ByteCode internal rep from a Tcl_Obj script.
 */

static int GetByteCodeFromScript(Tcl_Interp *interp, Tcl_Obj *scriptObj, ByteCode **out) {
    if (Tcl_ConvertToType(interp, scriptObj, tbcxTyBytecode) != TCL_OK)
        return TCL_ERROR;
    {
        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(scriptObj, tbcxTyBytecode);
        if (!ir)
            return TCL_ERROR;
        *out = (ByteCode *)ir->twoPtrValue.ptr1;
        return (*out ? TCL_OK : TCL_ERROR);
    }
}

/* ==========================================================================
 * OO parse/capture (static + dynamic) and merges
 * ========================================================================== */

/*
 * CollectOOClassBody
 * Parse an oo::class body to collect methods/ctors/dtors/superclasses.
 */

static int CollectOOClassBody(Tcl_Interp *interp, Tcl_Obj *body, Tcl_Obj *clsFqn, DynClassCap *classes, DynMethCap *methods) {
    Tcl_Size    totalBytes = 0;
    const char *s          = Tcl_GetStringFromObj(body, &totalBytes);
    const char *p = s, *end = s + totalBytes;
    Tcl_Parse   parse;
    while (p < end) {
        if (Tcl_ParseCommand(interp, p, (int)(end - p), 0, &parse) != TCL_OK)
            return TCL_ERROR;
        if (parse.numWords > 0) {
            Tcl_Token *w0 = WordToken(&parse, 0);
            if (w0 && IsClassBodyMethodCmd(w0->start, w0->size)) {
                if (TokenEquals(w0->start, w0->size, "superclass") && parse.numWords >= 2) {
                    Tcl_Token *tList = WordToken(&parse, parse.numWords - 1);
                    if (tList) {
                        ClassEntry ce;
                        memset(&ce, 0, sizeof ce);
                        ce.clsFqn = Tcl_DuplicateObj(clsFqn);
                        Tcl_IncrRefCount(ce.clsFqn);

                        Tcl_Obj *lst = Tcl_NewStringObj(tList->start, (Tcl_Size)tList->size);
                        Tcl_IncrRefCount(lst);
                        Tcl_Size  n     = 0;
                        Tcl_Obj **elems = NULL;
                        if (Tcl_ListObjGetElements(NULL, lst, &n, &elems) == TCL_OK && n > 0) {
                            ce.supers  = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)n);
                            ce.nSupers = (int)n;
                            for (int i = 0; i < ce.nSupers; ++i) {
                                ce.supers[i] = Tcl_DuplicateObj(elems[i]);
                                Tcl_IncrRefCount(ce.supers[i]);
                            }
                        }
                        Tcl_DecrRefCount(lst);
                        DynClassPush(classes, &ce);
                    }
                } else if (TokenEquals(w0->start, w0->size, "constructor") && parse.numWords >= 3) {
                    Tcl_Token *tArgs = WordToken(&parse, 1);
                    Tcl_Token *tBody = WordToken(&parse, 2);
                    if (tArgs && tBody) {
                        MethEntry me;
                        memset(&me, 0, sizeof me);
                        me.kind   = TBCX_METH_CTOR;
                        me.clsFqn = Tcl_DuplicateObj(clsFqn);
                        Tcl_IncrRefCount(me.clsFqn);
                        me.methName = NULL;
                        me.args     = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
                        Tcl_IncrRefCount(me.args);
                        me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                        Tcl_IncrRefCount(me.body);
                        DynMethPush(methods, &me);
                    }
                } else if (TokenEquals(w0->start, w0->size, "destructor") && parse.numWords >= 2) {
                    Tcl_Token *tBody = WordToken(&parse, 1);
                    if (tBody) {
                        MethEntry me;
                        memset(&me, 0, sizeof me);
                        me.kind   = TBCX_METH_DTOR;
                        me.clsFqn = Tcl_DuplicateObj(clsFqn);
                        Tcl_IncrRefCount(me.clsFqn);
                        me.methName = NULL;
                        me.args     = Tcl_NewStringObj("", -1);
                        Tcl_IncrRefCount(me.args);
                        me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                        Tcl_IncrRefCount(me.body);
                        DynMethPush(methods, &me);
                    }
                } else if (TokenEquals(w0->start, w0->size, "method") && parse.numWords >= 4) {
                    Tcl_Token *tName = WordToken(&parse, 1);
                    Tcl_Token *tArgs = WordToken(&parse, 2);
                    Tcl_Token *tBody = WordToken(&parse, 3);
                    if (tName && tArgs && tBody) {
                        MethEntry me;
                        memset(&me, 0, sizeof me);
                        me.kind   = TBCX_METH_INST;
                        me.clsFqn = Tcl_DuplicateObj(clsFqn);
                        Tcl_IncrRefCount(me.clsFqn);
                        me.methName = Tcl_NewStringObj(tName->start, (Tcl_Size)tName->size);
                        Tcl_IncrRefCount(me.methName);
                        me.args = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
                        Tcl_IncrRefCount(me.args);
                        me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                        Tcl_IncrRefCount(me.body);
                        DynMethPush(methods, &me);
                    }
                }
            }
        }
        p = parse.commandStart + parse.commandSize;
        Tcl_FreeParse(&parse);
    }
    return TCL_OK;
}

/*
 * CollectOOFromScript
 * Parse a script for oo::class/oo::define forms and collect OO constructs.
 */

static int CollectOOFromScript(Tcl_Interp *interp, Tcl_Obj *script, Tcl_Obj *nsDefault, DynClassCap *outClasses, DynMethCap *outMethods) {
    Tcl_Size    totalBytes = 0;
    const char *s          = Tcl_GetStringFromObj(script, &totalBytes);
    const char *p = s, *end = s + totalBytes;
    Tcl_Parse   parse;
    int         code = TCL_OK;

    while (p < end) {
        if (Tcl_ParseCommand(interp, p, (int)(end - p), 0, &parse) != TCL_OK) {
            code = TCL_ERROR;
            break;
        }
        if (parse.numWords > 0) {
            Tcl_Token *w0 = WordToken(&parse, 0);
            if (w0) {
                const char *w0s = w0->start;
                int         w0n = w0->size;
                if (IsOoClassCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *w1 = WordToken(&parse, 1);
                    if (w1 && TokenEquals(w1->start, w1->size, "create")) {
                        Tcl_Token *tName = WordToken(&parse, 2);
                        Tcl_Token *tBody = (parse.numWords >= 4 ? WordToken(&parse, parse.numWords - 1) : NULL);
                        if (tName) {
                            Tcl_Obj *className = Tcl_NewStringObj(tName->start, (Tcl_Size)tName->size);
                            Tcl_Obj *fq        = QualifyInNs(className, nsDefault);
                            if (tBody) {
                                Tcl_Obj *body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                                int      rc   = CollectOOClassBody(interp, body, fq, outClasses, outMethods);
                                Tcl_DecrRefCount(body);
                                if (rc != TCL_OK) {
                                    Tcl_DecrRefCount(fq);
                                    Tcl_DecrRefCount(className);
                                    code = TCL_ERROR;
                                    goto next;
                                }
                            }
                            ClassEntry ce;
                            memset(&ce, 0, sizeof ce);
                            ce.clsFqn = fq;
                            Tcl_IncrRefCount(ce.clsFqn);
                            DynClassPush(outClasses, &ce);
                            Tcl_DecrRefCount(fq);
                            Tcl_DecrRefCount(className);
                        }
                    }
                } else if (IsOoDefineCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *tCls = WordToken(&parse, 1);
                    Tcl_Token *tSub = WordToken(&parse, 2);
                    if (tCls && tSub) {
                        Tcl_Obj *clsName = Tcl_NewStringObj(tCls->start, (Tcl_Size)tCls->size);
                        Tcl_Obj *fq      = QualifyInNs(clsName, nsDefault);
                        if (TokenEquals(tSub->start, tSub->size, "method") && parse.numWords >= 6) {
                            Tcl_Token *tName = WordToken(&parse, 3);
                            Tcl_Token *tArgs = WordToken(&parse, 4);
                            Tcl_Token *tBody = WordToken(&parse, 5);
                            if (tName && tArgs && tBody) {
                                MethEntry me;
                                memset(&me, 0, sizeof me);
                                me.kind   = TBCX_METH_INST;
                                me.clsFqn = Tcl_DuplicateObj(fq);
                                Tcl_IncrRefCount(me.clsFqn);
                                me.methName = Tcl_NewStringObj(tName->start, (Tcl_Size)tName->size);
                                Tcl_IncrRefCount(me.methName);
                                me.args = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
                                Tcl_IncrRefCount(me.args);
                                me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                                Tcl_IncrRefCount(me.body);
                                DynMethPush(outMethods, &me);
                            }
                        } else if (TokenEquals(tSub->start, tSub->size, "constructor") && parse.numWords >= 5) {
                            Tcl_Token *tArgs = WordToken(&parse, 3);
                            Tcl_Token *tBody = WordToken(&parse, 4);
                            if (tArgs && tBody) {
                                MethEntry me;
                                memset(&me, 0, sizeof me);
                                me.kind   = TBCX_METH_CTOR;
                                me.clsFqn = Tcl_DuplicateObj(fq);
                                Tcl_IncrRefCount(me.clsFqn);
                                me.methName = NULL;
                                me.args     = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
                                Tcl_IncrRefCount(me.args);
                                me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                                Tcl_IncrRefCount(me.body);
                                DynMethPush(outMethods, &me);
                            }
                        } else if (TokenEquals(tSub->start, tSub->size, "destructor") && parse.numWords >= 4) {
                            Tcl_Token *tBody = WordToken(&parse, 3);
                            if (tBody) {
                                MethEntry me;
                                memset(&me, 0, sizeof me);
                                me.kind   = TBCX_METH_DTOR;
                                me.clsFqn = Tcl_DuplicateObj(fq);
                                Tcl_IncrRefCount(me.clsFqn);
                                me.methName = NULL;
                                me.args     = Tcl_NewStringObj("", -1);
                                Tcl_IncrRefCount(me.args);
                                me.body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                                Tcl_IncrRefCount(me.body);
                                DynMethPush(outMethods, &me);
                            }
                        } else if (TokenEquals(tSub->start, tSub->size, "superclass") && parse.numWords >= 4) {
                            Tcl_Token *tList = WordToken(&parse, 3);
                            if (tList) {
                                ClassEntry ce;
                                memset(&ce, 0, sizeof ce);
                                ce.clsFqn = Tcl_DuplicateObj(fq);
                                Tcl_IncrRefCount(ce.clsFqn);
                                Tcl_Obj *lst = Tcl_NewStringObj(tList->start, (Tcl_Size)tList->size);
                                Tcl_IncrRefCount(lst);
                                Tcl_Size  n     = 0;
                                Tcl_Obj **elems = NULL;
                                if (Tcl_ListObjGetElements(NULL, lst, &n, &elems) == TCL_OK && n > 0) {
                                    ce.supers  = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)n);
                                    ce.nSupers = (int)n;
                                    for (int i = 0; i < ce.nSupers; ++i) {
                                        ce.supers[i] = Tcl_DuplicateObj(elems[i]);
                                        Tcl_IncrRefCount(ce.supers[i]);
                                    }
                                }
                                Tcl_DecrRefCount(lst);
                                DynClassPush(outClasses, &ce);
                            }
                        } else if (TokenEquals(tSub->start, tSub->size, "eval") && parse.numWords >= 4) {
                            Tcl_Token *tBody = WordToken(&parse, parse.numWords - 1);
                            if (tBody) {
                                Tcl_Obj *body = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                                int      rc   = CollectOOClassBody(interp, body, fq, outClasses, outMethods);
                                Tcl_DecrRefCount(body);
                                if (rc != TCL_OK) {
                                    Tcl_DecrRefCount(fq);
                                    Tcl_DecrRefCount(clsName);
                                    code = TCL_ERROR;
                                    goto next;
                                }
                            }
                        }
                        {
                            ClassEntry ce;
                            memset(&ce, 0, sizeof ce);
                            ce.clsFqn = Tcl_DuplicateObj(fq);
                            Tcl_IncrRefCount(ce.clsFqn);
                            DynClassPush(outClasses, &ce);
                        }
                        Tcl_DecrRefCount(fq);
                        Tcl_DecrRefCount(clsName);
                    }
                }
            }
        }
    next:
        p = parse.commandStart + parse.commandSize;
        Tcl_FreeParse(&parse);
        if (code != TCL_OK)
            break;
    }
    return code;
}

/*
 * Tbcx_OoDefineIntercept
 * Child-interp interceptor for ::oo::define that records definitions only.
 */

static int Tbcx_OoDefineIntercept(void *cd, Tcl_Interp *child, Tcl_Size objc, Tcl_Obj *const objv[]) {
    OoCap *cap = (OoCap *)cd;
    if (objc < 3)
        return TCL_OK;
    Namespace  *curNs  = (Namespace *)Tcl_GetCurrentNamespace(child);
    const char *nsFull = (curNs && curNs->fullName) ? curNs->fullName : "::";
    Tcl_Obj    *nsObj  = Tcl_NewStringObj(nsFull, -1);
    Tcl_IncrRefCount(nsObj);

    Tcl_Obj *clsFq = QualifyInNs(objv[1], nsObj);

    if (objc >= 6 && strcmp(Tcl_GetString(objv[2]), "method") == 0) {
        MethEntry me;
        memset(&me, 0, sizeof me);
        me.kind   = TBCX_METH_INST;
        me.clsFqn = clsFq;
        Tcl_IncrRefCount(me.clsFqn);
        me.methName = Tcl_DuplicateObj(objv[3]);
        Tcl_IncrRefCount(me.methName);
        me.args = Tcl_DuplicateObj(objv[4]);
        Tcl_IncrRefCount(me.args);
        me.body = Tcl_DuplicateObj(objv[5]);
        Tcl_IncrRefCount(me.body);
        DynMethPush(&cap->methods, &me);
    } else if (objc >= 5 && strcmp(Tcl_GetString(objv[2]), "constructor") == 0) {
        MethEntry me;
        memset(&me, 0, sizeof me);
        me.kind   = TBCX_METH_CTOR;
        me.clsFqn = clsFq;
        Tcl_IncrRefCount(me.clsFqn);
        me.methName = NULL;
        me.args     = Tcl_DuplicateObj(objv[3]);
        Tcl_IncrRefCount(me.args);
        me.body = Tcl_DuplicateObj(objv[4]);
        Tcl_IncrRefCount(me.body);
        DynMethPush(&cap->methods, &me);
    } else if (objc >= 4 && strcmp(Tcl_GetString(objv[2]), "destructor") == 0) {
        MethEntry me;
        memset(&me, 0, sizeof me);
        me.kind   = TBCX_METH_DTOR;
        me.clsFqn = clsFq;
        Tcl_IncrRefCount(me.clsFqn);
        me.methName = NULL;
        me.args     = Tcl_NewStringObj("", -1);
        Tcl_IncrRefCount(me.args);
        me.body = Tcl_DuplicateObj(objv[3]);
        Tcl_IncrRefCount(me.body);
        DynMethPush(&cap->methods, &me);
    } else if (objc >= 4 && strcmp(Tcl_GetString(objv[2]), "superclass") == 0) {
        ClassEntry ce;
        memset(&ce, 0, sizeof ce);
        ce.clsFqn = clsFq;
        Tcl_IncrRefCount(ce.clsFqn);
        Tcl_Size  n     = 0;
        Tcl_Obj **elems = NULL;
        if (Tcl_ListObjGetElements(NULL, objv[3], &n, &elems) == TCL_OK && n > 0) {
            ce.supers  = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)n);
            ce.nSupers = (int)n;
            for (int i = 0; i < ce.nSupers; ++i) {
                ce.supers[i] = Tcl_DuplicateObj(elems[i]);
                Tcl_IncrRefCount(ce.supers[i]);
            }
        }
        DynClassPush(&cap->classes, &ce);
    } else if (objc >= 4 && strcmp(Tcl_GetString(objv[2]), "eval") == 0) {
        Tcl_Obj *script = objv[3];
        (void)CollectOOClassBody(child, script, clsFq, &cap->classes, &cap->methods);
    } else {
        ClassEntry ce;
        memset(&ce, 0, sizeof ce);
        ce.clsFqn = clsFq;
        Tcl_IncrRefCount(ce.clsFqn);
        DynClassPush(&cap->classes, &ce);
    }
    Tcl_DecrRefCount(nsObj);
    return TCL_OK;
}

/*
 * Tbcx_OoClassIntercept
 * Child-interp interceptor for ::oo::class create that records classes.
 */

static int Tbcx_OoClassIntercept(void *cd, Tcl_Interp *child, Tcl_Size objc, Tcl_Obj *const objv[]) {
    OoCap *cap = (OoCap *)cd;
    if (objc < 3)
        return TCL_OK;
    if (strcmp(Tcl_GetString(objv[1]), "create") != 0)
        return TCL_OK;
    Namespace  *curNs  = (Namespace *)Tcl_GetCurrentNamespace(child);
    const char *nsFull = (curNs && curNs->fullName) ? curNs->fullName : "::";
    Tcl_Obj    *nsObj  = Tcl_NewStringObj(nsFull, -1);
    Tcl_IncrRefCount(nsObj);
    Tcl_Obj   *clsFq = QualifyInNs(objv[2], nsObj);
    ClassEntry ce;
    memset(&ce, 0, sizeof ce);
    ce.clsFqn = clsFq;
    Tcl_IncrRefCount(ce.clsFqn);
    DynClassPush(&cap->classes, &ce);
    if (objc >= 4) {
        (void)CollectOOClassBody(child, objv[3], clsFq, &cap->classes, &cap->methods);
    }
    Tcl_DecrRefCount(nsObj);
    return TCL_OK;
}

/*
 * FreeClassEntryArray
 * Release references and free a ClassEntry array.
 */

static void FreeClassEntryArray(ClassEntry *arr, int n) {
    if (!arr)
        return;
    for (int i = 0; i < n; ++i) {
        Tcl_DecrRefCount(arr[i].clsFqn);
        for (int k = 0; k < arr[i].nSupers; ++k)
            Tcl_DecrRefCount(arr[i].supers[k]);
        if (arr[i].supers)
            Tcl_Free((char *)arr[i].supers);
    }
    Tcl_Free((char *)arr);
}

/*
 * FreeMethEntryArray
 * Release references and free a MethEntry array.
 */

static void FreeMethEntryArray(MethEntry *arr, int n) {
    if (!arr)
        return;
    for (int i = 0; i < n; ++i) {
        Tcl_DecrRefCount(arr[i].clsFqn);
        if (arr[i].methName)
            Tcl_DecrRefCount(arr[i].methName);
        Tcl_DecrRefCount(arr[i].args);
        Tcl_DecrRefCount(arr[i].body);
    }
    Tcl_Free((char *)arr);
}

/*
 * MergeClassLists
 * Merge two class lists; last wins for superclass list per class FQN.
 */

static int MergeClassLists(DynClassCap *base, DynClassCap *extra) {
    if (!extra || extra->count == 0)
        return TCL_OK;
    Tcl_HashTable ht;
    Tcl_InitHashTable(&ht, TCL_STRING_KEYS);
    for (int i = 0; i < base->count; ++i) {
        const char    *k = Tcl_GetString(base->list[i].clsFqn);
        int            isNew;
        Tcl_HashEntry *h = Tcl_CreateHashEntry(&ht, k, &isNew);
        Tcl_SetHashValue(h, INT2PTR(i));
    }
    for (int i = 0; i < extra->count; ++i) {
        const char    *k = Tcl_GetString(extra->list[i].clsFqn);
        Tcl_HashEntry *h = Tcl_FindHashEntry(&ht, k);
        if (h) {
            int idx = PTR2INT(Tcl_GetHashValue(h));
            for (int s = 0; s < base->list[idx].nSupers; ++s)
                Tcl_DecrRefCount(base->list[idx].supers[s]);
            if (base->list[idx].supers)
                Tcl_Free((char *)base->list[idx].supers);
            base->list[idx].nSupers = extra->list[i].nSupers;
            if (extra->list[i].nSupers) {
                base->list[idx].supers = (Tcl_Obj **)Tcl_Alloc(sizeof(Tcl_Obj *) * (size_t)extra->list[i].nSupers);
                for (int s = 0; s < extra->list[i].nSupers; ++s) {
                    base->list[idx].supers[s] = extra->list[i].supers[s];
                    Tcl_IncrRefCount(base->list[idx].supers[s]);
                }
            } else
                base->list[idx].supers = NULL;
        } else {
            DynClassPush(base, &extra->list[i]);
            int            isNew;
            Tcl_HashEntry *h2 = Tcl_CreateHashEntry(&ht, k, &isNew);
            Tcl_SetHashValue(h2, INT2PTR(base->count - 1));
        }
    }
    Tcl_DeleteHashTable(&ht);
    return TCL_OK;
}

/*
 * MergeMethLists
 * Merge two method lists; dedup by (class,kind,name), last wins for body/args.
 */

static int MergeMethLists(DynMethCap *base, DynMethCap *extra) {
    if (!extra || extra->count == 0)
        return TCL_OK;
    Tcl_HashTable ht;
    Tcl_InitHashTable(&ht, TCL_STRING_KEYS);
    Tcl_DString key;
    Tcl_DStringInit(&key);
    for (int i = 0; i < base->count; ++i) {
        Tcl_DStringSetLength(&key, 0);
        Tcl_DStringAppend(&key, Tcl_GetString(base->list[i].clsFqn), -1);
        Tcl_DStringAppend(&key, "|", 1);
        char kb[4];
        snprintf(kb, sizeof(kb), "%u", (unsigned)base->list[i].kind);
        Tcl_DStringAppend(&key, kb, -1);
        Tcl_DStringAppend(&key, "|", 1);
        Tcl_DStringAppend(&key, base->list[i].methName ? Tcl_GetString(base->list[i].methName) : "", -1);
        int            isNew;
        Tcl_HashEntry *h = Tcl_CreateHashEntry(&ht, Tcl_DStringValue(&key), &isNew);
        Tcl_SetHashValue(h, INT2PTR(i));
    }
    for (int i = 0; i < extra->count; ++i) {
        Tcl_DStringSetLength(&key, 0);
        Tcl_DStringAppend(&key, Tcl_GetString(extra->list[i].clsFqn), -1);
        Tcl_DStringAppend(&key, "|", 1);
        char kb[4];
        snprintf(kb, sizeof(kb), "%u", (unsigned)extra->list[i].kind);
        Tcl_DStringAppend(&key, kb, -1);
        Tcl_DStringAppend(&key, "|", 1);
        Tcl_DStringAppend(&key, extra->list[i].methName ? Tcl_GetString(extra->list[i].methName) : "", -1);
        Tcl_HashEntry *h = Tcl_FindHashEntry(&ht, Tcl_DStringValue(&key));
        if (h) {
            int idx = PTR2INT(Tcl_GetHashValue(h));
            Tcl_DecrRefCount(base->list[idx].args);
            Tcl_DecrRefCount(base->list[idx].body);
            if (base->list[idx].methName)
                Tcl_DecrRefCount(base->list[idx].methName);
            base->list[idx].args = extra->list[i].args;
            Tcl_IncrRefCount(base->list[idx].args);
            base->list[idx].body = extra->list[i].body;
            Tcl_IncrRefCount(base->list[idx].body);
            base->list[idx].methName = extra->list[i].methName;
            if (base->list[idx].methName)
                Tcl_IncrRefCount(base->list[idx].methName);
        } else {
            DynMethPush(base, &extra->list[i]);
            int            isNew;
            Tcl_HashEntry *h2 = Tcl_CreateHashEntry(&ht, Tcl_DStringValue(&key), &isNew);
            Tcl_SetHashValue(h2, INT2PTR(base->count - 1));
        }
    }
    Tcl_DStringFree(&key);
    Tcl_DeleteHashTable(&ht);
    return TCL_OK;
}

/*
 * CollectOOByEval
 * Run script in child interp; intercept oo::define/class to capture OO constructs.
 */

static int CollectOOByEval(Tcl_Interp *interp, Tcl_Obj *script, DynClassCap *outClasses, DynMethCap *outMethods) {
    OoCap cap;
    DynClassInit(&cap.classes);
    DynMethInit(&cap.methods);

    {
        Tcl_Size    nb = 0;
        const char *ss = Tcl_GetStringFromObj(script, &nb);
        if (ss && !strstr(ss, "oo::")) {
            outClasses->list  = NULL;
            outClasses->count = 0;
            outClasses->cap   = 0;
            outMethods->list  = NULL;
            outMethods->count = 0;
            outMethods->cap   = 0;
            return TCL_OK;
        }
    }

    char nameBuf[64];
    snprintf(nameBuf, sizeof(nameBuf), "tbcx_dyncap_oo_%p", (void *)interp);
    Tcl_Interp *child = Tcl_CreateChild(interp, nameBuf, 1);
    if (!child)
        return TCL_ERROR;
    (void)Tcl_InitStubs(child, TCL_VERSION, 1);

    Tcl_CreateObjCommand2(child, "oo::define", Tbcx_OoDefineIntercept, &cap, NULL);
    Tcl_CreateObjCommand2(child, "oo::class", Tbcx_OoClassIntercept, &cap, NULL);
    Tcl_CreateObjCommand2(child, "unknown", UnknownGuardCmd, NULL, NULL);
    (void)Tcl_EvalObjEx(child, script, TCL_EVAL_DIRECT);

    *outClasses = cap.classes;
    *outMethods = cap.methods;
    Tcl_DeleteInterp(child);
    return TCL_OK;
}

/* ==========================================================================
 * Token/name utilities
 * ========================================================================== */

/*
 * WordToken
 * Return the token for the Nth word in a Tcl_Parse; NULL on error/out-of-range.
 */

static Tcl_Token *WordToken(const Tcl_Parse *p, int wordIndex) {
    if (wordIndex < 0 || wordIndex >= p->numWords)
        return NULL;
    Tcl_Token *tok = p->tokenPtr;
    for (int w = 0; w < p->numWords; ++w) {
        if (tok->type != TCL_TOKEN_WORD)
            return NULL;
        if (w == wordIndex)
            return tok;
        tok += tok->numComponents + 1;
    }
    return NULL;
}

/*
 * TokenEquals
 * Token/literal equality helper.
 */

static int TokenEquals(const char *s, int len, const char *lit) {
    int n = (int)strlen(lit);
    return (len == n) && (memcmp(s, lit, (size_t)n) == 0);
}

/*
 * NameEndsWith
 * Check if fully-qualified command name ends with a given tail (e.g., 'proc').
 */

static int NameEndsWith(const char *s, int len, const char *tail) {
    int tn = (int)strlen(tail);
    if (len < tn)
        return 0;
    if (memcmp(s + (len - tn), tail, (size_t)tn) != 0)
        return 0;
    if (len == tn)
        return 1;
    return (len >= tn + 2) && s[len - tn - 1] == ':' && s[len - tn - 2] == ':';
}

/*
 * IsProcCmdName
 * Return non-zero if token resolves to a 'proc' command (any namespace).
 */

static int IsProcCmdName(const char *s, int len) {
    return NameEndsWith(s, len, "proc");
}

/*
 * IsOoDefineCmdName
 * Return non-zero if token resolves to 'oo::define' (any namespace).
 */

static int IsOoDefineCmdName(const char *s, int len) {
    return NameEndsWith(s, len, "oo::define");
}

/*
 * IsOoClassCmdName
 * Return non-zero if token resolves to 'oo::class' (any namespace).
 */

static int IsOoClassCmdName(const char *s, int len) {
    return NameEndsWith(s, len, "oo::class");
}

/*
 * IsClassBodyMethodCmd
 * Return non-zero for method/constructor/destructor/superclass keywords.
 */

static int IsClassBodyMethodCmd(const char *s, int len) {
    return TokenEquals(s, len, "method") || TokenEquals(s, len, "constructor") || TokenEquals(s, len, "destructor") || TokenEquals(s, len, "superclass");
}

/*
 * IsNamespaceCmdName
 * Return non-zero if token resolves to 'namespace' (any namespace).
 */

static int IsNamespaceCmdName(const char *s, int len) {
    return NameEndsWith(s, len, "namespace");
}

/*
 * QualifyInNs
 * Qualify a simple name under the given namespace prefix, if any.
 */

static Tcl_Obj *QualifyInNs(Tcl_Obj *nameObj, Tcl_Obj *nsPrefix) {
    const char *n = Tcl_GetString(nameObj);
    if (n[0] == ':' && n[1] == ':') {
        Tcl_IncrRefCount(nameObj);
        return nameObj;
    }
    Tcl_Size    plen = 0;
    const char *p    = Tcl_GetStringFromObj(nsPrefix, &plen);
    if (plen == 2 && p[0] == ':' && p[1] == ':') {
        Tcl_Obj *fq = Tcl_NewStringObj("::", 2);
        Tcl_AppendObjToObj(fq, nameObj);
        Tcl_IncrRefCount(fq);
        return fq;
    }
    if (!nsPrefix || Tcl_GetCharLength(nsPrefix) == 0) {
        Tcl_IncrRefCount(nameObj);
        return nameObj;
    }
    Tcl_Obj *fq = Tcl_DuplicateObj(nsPrefix);
    Tcl_AppendToObj(fq, "::", 2);
    Tcl_AppendObjToObj(fq, nameObj);
    Tcl_IncrRefCount(fq);
    return fq;
}

/* ==========================================================================
 * Proc capture & script filtering
 * ========================================================================== */

/*
 * ProcTripleMatches
 * Compare captured proc triple (name,args,body) with provided values.
 */

static int ProcTripleMatches(const ProcEntry *e, Tcl_Obj *fqName, Tcl_Obj *args, Tcl_Obj *body) {
    /* --- name: must match byte-for-byte --- */
    Tcl_Size    ln1, ln2;
    const char *n1 = Tcl_GetStringFromObj(e->name, &ln1);
    const char *n2 = Tcl_GetStringFromObj(fqName, &ln2);
    if (ln1 != ln2 || memcmp(n1, n2, (size_t)ln1) != 0) {
        return 0;
    }

    /* --- args: compare as Tcl lists so {} == "" and whitespace canonicalizes --- */
    Tcl_Size    la1, la2;
    const char *a1s       = Tcl_GetStringFromObj(e->args, &la1);
    const char *a2s       = Tcl_GetStringFromObj(args, &la2);
    int         argsEqual = 0;
    if (la1 == la2 && memcmp(a1s, a2s, (size_t)la1) == 0) {
        argsEqual = 1; /* fast path: identical strings */
    } else {
        Tcl_Size nA = 0, nB = 0;
        if (Tcl_ListObjLength(NULL, e->args, &nA) == TCL_OK && Tcl_ListObjLength(NULL, args, &nB) == TCL_OK && nA == nB) {
            Tcl_Obj **EA = NULL, **EB = NULL;
            if (Tcl_ListObjGetElements(NULL, e->args, &nA, &EA) == TCL_OK && Tcl_ListObjGetElements(NULL, args, &nB, &EB) == TCL_OK) {
                argsEqual = 1;
                for (int i = 0; i < nA; ++i) {
                    Tcl_Size    l1, l2;
                    const char *s1 = Tcl_GetStringFromObj(EA[i], &l1);
                    const char *s2 = Tcl_GetStringFromObj(EB[i], &l2);
                    if (l1 != l2 || memcmp(s1, s2, (size_t)l1) != 0) {
                        argsEqual = 0;
                        break;
                    }
                }
            }
        }
    }
    if (!argsEqual) {
        return 0;
    }

    /* --- body: must match byte-for-byte --- */
    Tcl_Size    lb1, lb2;
    const char *b1 = Tcl_GetStringFromObj(e->body, &lb1);
    const char *b2 = Tcl_GetStringFromObj(body, &lb2);
    return (lb1 == lb2) && (memcmp(b1, b2, (size_t)lb1) == 0);
}

/*
 * DynCapInit
 * Initialize dynamic capture for classic procs.
 */

static void DynCapInit(DynCap *c) {
    c->list  = NULL;
    c->count = 0;
    c->cap   = 0;
}

/*
 * DynCapPush
 * Append a ProcEntry to dynamic capture list, growing as needed.
 */

static void DynCapPush(DynCap *c, const ProcEntry *e) {
    if (c->count == c->cap) {
        c->cap  = c->cap ? (c->cap * 2) : 8;
        c->list = (ProcEntry *)Tcl_Realloc((char *)c->list, sizeof(ProcEntry) * (size_t)c->cap);
    }
    c->list[c->count++] = *e;
}

/*
 * Tbcx_InterceptProcCmd
 * Child-interp interceptor for ::proc; records proc definitions only.
 */

static int Tbcx_InterceptProcCmd(void *cd, Tcl_Interp *child, Tcl_Size objc, Tcl_Obj *const objv[]) {
    DynCap *cap = (DynCap *)cd;
    if (objc < 4) {
        Tcl_WrongNumArgs(child, 1, objv, "name args body");
        return TCL_ERROR;
    }

    Namespace  *curNs  = (Namespace *)Tcl_GetCurrentNamespace(child);
    const char *nsFull = (curNs && curNs->fullName) ? curNs->fullName : "::";
    Tcl_Obj    *nsObj  = Tcl_NewStringObj(nsFull, -1);
    Tcl_IncrRefCount(nsObj);
    Tcl_Obj  *fq = QualifyInNs(objv[1], nsObj);

    ProcEntry e;
    e.name = fq;
    e.ns   = nsObj;
    e.args = Tcl_DuplicateObj(objv[2]);
    Tcl_IncrRefCount(e.args);
    e.body = Tcl_DuplicateObj(objv[3]);
    Tcl_IncrRefCount(e.body);
    DynCapPush(cap, &e);

    return TCL_OK;
}

/*
 * FreeProcEntryArray
 * Release references and free a ProcEntry array.
 */

static void FreeProcEntryArray(ProcEntry *arr, int n) {
    if (!arr)
        return;
    for (int i = 0; i < n; ++i) {
        Tcl_DecrRefCount(arr[i].name);
        Tcl_DecrRefCount(arr[i].ns);
        Tcl_DecrRefCount(arr[i].args);
        Tcl_DecrRefCount(arr[i].body);
    }
    Tcl_Free((char *)arr);
}

/*
 * UnknownGuardCmd
 * Register a C command
 */

static int UnknownGuardCmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    (void)objc;
    (void)objv;
    Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx dynamic scan forbids calling unknown command", -1));
    return TCL_ERROR;
}

/*
 * CollectProcsByEval
 * Run script in a safe child to capture dynamically created procs.
 */

static int CollectProcsByEval(Tcl_Interp *interp, Tcl_Obj *script, ProcEntry **outEntries, int *outCount) {
    *outEntries = NULL;
    *outCount   = 0;

    {
        Tcl_Size    nb = 0;
        const char *ss = Tcl_GetStringFromObj(script, &nb);
        if (ss && !strstr(ss, "proc") && !strstr(ss, "namespace")) {
            return TCL_OK;
        }
    }

    char nameBuf[64];
    snprintf(nameBuf, sizeof(nameBuf), "tbcx_dyncap_%p", (void *)interp);
    Tcl_Interp *child = Tcl_CreateChild(interp, nameBuf, 1);
    if (!child)
        return TCL_ERROR;
    (void)Tcl_InitStubs(child, TCL_VERSION, 1);

    DynCap cap;
    DynCapInit(&cap);
    Tcl_CreateObjCommand2(child, "proc", Tbcx_InterceptProcCmd, &cap, NULL);
    {
        Tcl_Namespace *tclNs = Tcl_FindNamespace(child, "::tcl", NULL, 0);
        if (!tclNs)
            tclNs = Tcl_CreateNamespace(child, "::tcl", NULL, NULL);
        if (tclNs) {
            Tcl_CallFrame frame;
            if (Tcl_PushCallFrame(child, &frame, tclNs, 0) == TCL_OK) {
                Tcl_CreateObjCommand2(child, "proc", Tbcx_InterceptProcCmd, &cap, NULL);
                Tcl_PopCallFrame(child);
            }
        }
    }

    Tcl_CreateObjCommand2(child, "unknown", UnknownGuardCmd, NULL, NULL);
    (void)Tcl_EvalObjEx(child, script, TCL_EVAL_DIRECT);

    *outEntries = cap.list;
    *outCount   = cap.count;
    Tcl_DeleteInterp(child);
    return TCL_OK;
}

/*
 * MergeProcLists
 * Merge static/dynamic proc lists; dedup by fully-qualified name.
 */

static int MergeProcLists(Tcl_Interp *interp, ProcEntry **base, int *nBase, ProcEntry *extra, int nExtra) {
    if (!extra || nExtra == 0)
        return TCL_OK;
    Tcl_HashTable ht;
    Tcl_InitHashTable(&ht, TCL_STRING_KEYS);
    for (int i = 0; i < *nBase; ++i) {
        const char    *k = Tcl_GetString((*base)[i].name);
        int            isNew;
        Tcl_HashEntry *h = Tcl_CreateHashEntry(&ht, k, &isNew);
        Tcl_SetHashValue(h, INT2PTR(i));
    }
    for (int i = 0; i < nExtra; ++i) {
        const char    *k = Tcl_GetString(extra[i].name);
        Tcl_HashEntry *h = Tcl_FindHashEntry(&ht, k);
        if (h) {
            int idx = PTR2INT(Tcl_GetHashValue(h));
            Tcl_DecrRefCount((*base)[idx].args);
            Tcl_DecrRefCount((*base)[idx].body);
            Tcl_DecrRefCount((*base)[idx].ns);
            (*base)[idx].args = extra[i].args;
            Tcl_IncrRefCount((*base)[idx].args);
            (*base)[idx].body = extra[i].body;
            Tcl_IncrRefCount((*base)[idx].body);
            (*base)[idx].ns = extra[i].ns;
            Tcl_IncrRefCount((*base)[idx].ns);
        } else {
            *base                = (ProcEntry *)Tcl_Realloc((char *)(*base), sizeof(ProcEntry) * (size_t)(*nBase + 1));
            (*base)[*nBase].name = extra[i].name;
            Tcl_IncrRefCount((*base)[*nBase].name);
            (*base)[*nBase].ns = extra[i].ns;
            Tcl_IncrRefCount((*base)[*nBase].ns);
            (*base)[*nBase].args = extra[i].args;
            Tcl_IncrRefCount((*base)[*nBase].args);
            (*base)[*nBase].body = extra[i].body;
            Tcl_IncrRefCount((*base)[*nBase].body);
            int            isNew;
            Tcl_HashEntry *h2 = Tcl_CreateHashEntry(&ht, k, &isNew);
            Tcl_SetHashValue(h2, INT2PTR(*nBase));
            (*nBase)++;
        }
    }
    Tcl_DeleteHashTable(&ht);
    return TCL_OK;
}

/*
 * CollectProcsFromScript
 * Parse script text to collect literal proc definitions (incl. namespace eval).
 */

static int CollectProcsFromScript(Tcl_Interp *interp, Tcl_Obj *script, Tcl_Obj *nsDefault, ProcEntry **outEntries, int *outCount) {
    Tcl_Size    totalBytes = 0;
    const char *s          = Tcl_GetStringFromObj(script, &totalBytes);
    const char *p = s, *end = s + totalBytes;
    Tcl_Parse   parse;
    int         code = TCL_OK, count = 0, cap = 8;
    ProcEntry  *list = (ProcEntry *)Tcl_Alloc(sizeof(ProcEntry) * cap);

    while (p < end) {
        if (Tcl_ParseCommand(interp, p, (int)(end - p), 0, &parse) != TCL_OK) {
            code = TCL_ERROR;
            break;
        }
        if (parse.numWords > 0) {
            Tcl_Token *w0 = WordToken(&parse, 0);
            if (w0) {
                const char *w0s = w0->start;
                int         w0n = w0->size;
                if (IsProcCmdName(w0s, w0n) && parse.numWords >= 4) {
                    Tcl_Token *tName = WordToken(&parse, 1);
                    Tcl_Token *tArgs = WordToken(&parse, 2);
                    Tcl_Token *tBody = WordToken(&parse, 3);
                    if (tName && tArgs && tBody) {
                        Tcl_Obj *name  = Tcl_NewStringObj(tName->start, (Tcl_Size)tName->size);
                        Tcl_Obj *args  = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
                        Tcl_Obj *body  = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                        Tcl_Obj *fq    = QualifyInNs(name, nsDefault);
                        Tcl_Obj *nsObj = (nsDefault ? nsDefault : Tcl_NewStringObj("", -1));
                        Tcl_IncrRefCount(args);
                        Tcl_IncrRefCount(body);
                        Tcl_IncrRefCount(fq);
                        Tcl_IncrRefCount(nsObj);
                        if (count == cap) {
                            cap *= 2;
                            list = (ProcEntry *)Tcl_Realloc((char *)list, sizeof(ProcEntry) * cap);
                        }
                        list[count].name = fq;
                        list[count].ns   = nsObj;
                        list[count].args = args;
                        list[count].body = body;
                        count++;
                        Tcl_DecrRefCount(name);
                        {
                            ProcEntry *subList  = NULL;
                            int        subCount = 0;
                            if (CollectProcsFromScript(interp, body, nsDefault, &subList, &subCount) == TCL_OK && subCount > 0) {
                                if (count + subCount > cap) {
                                    while (count + subCount > cap)
                                        cap *= 2;
                                    list = (ProcEntry *)Tcl_Realloc((char *)list, sizeof(ProcEntry) * cap);
                                }
                                memcpy(list + count, subList, sizeof(ProcEntry) * (size_t)subCount);
                                count += subCount;
                                Tcl_Free((char *)subList);
                            }
                        }
                    }
                } else if (IsNamespaceCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *w1 = WordToken(&parse, 1);
                    if (w1 && TokenEquals(w1->start, w1->size, "eval")) {
                        Tcl_Token *tNs   = WordToken(&parse, 2);
                        Tcl_Token *tBody = WordToken(&parse, parse.numWords - 1);
                        if (tNs && tBody) {
                            Tcl_Obj    *nsName = Tcl_NewStringObj(tNs->start, (Tcl_Size)tNs->size);
                            Tcl_Obj    *body   = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
                            Tcl_Obj    *nsNext = NULL;
                            const char *nn     = Tcl_GetString(nsName);
                            if (nn[0] == ':' && nn[1] == ':') {
                                nsNext = nsName;
                                Tcl_IncrRefCount(nsNext);
                            } else if (nsDefault && Tcl_GetCharLength(nsDefault) > 0) {
                                nsNext = Tcl_DuplicateObj(nsDefault);
                                Tcl_AppendToObj(nsNext, "::", 2);
                                Tcl_AppendObjToObj(nsNext, nsName);
                                Tcl_IncrRefCount(nsNext);
                            } else {
                                nsNext = nsName;
                                Tcl_IncrRefCount(nsNext);
                            }
                            ProcEntry *subList  = NULL;
                            int        subCount = 0;
                            if (CollectProcsFromScript(interp, body, nsNext, &subList, &subCount) != TCL_OK) {
                                Tcl_DecrRefCount(nsNext);
                                Tcl_DecrRefCount(nsName);
                                Tcl_DecrRefCount(body);
                                code = TCL_ERROR;
                            } else {
                                if (subCount) {
                                    if (count + subCount > cap) {
                                        while (count + subCount > cap)
                                            cap *= 2;
                                        list = (ProcEntry *)Tcl_Realloc((char *)list, sizeof(ProcEntry) * cap);
                                    }
                                    memcpy(list + count, subList, sizeof(ProcEntry) * (size_t)subCount);
                                    count += subCount;
                                    Tcl_Free((char *)subList);
                                }
                                Tcl_DecrRefCount(nsNext);
                                Tcl_DecrRefCount(nsName);
                                Tcl_DecrRefCount(body);
                            }
                        }
                    }
                }
            }
        }
        p = parse.commandStart + parse.commandSize;
        Tcl_FreeParse(&parse);
    }
    if (code != TCL_OK) {
        for (int i = 0; i < count; ++i) {
            Tcl_DecrRefCount(list[i].name);
            Tcl_DecrRefCount(list[i].ns);
            Tcl_DecrRefCount(list[i].args);
            Tcl_DecrRefCount(list[i].body);
        }
        Tcl_Free((char *)list);
        return TCL_ERROR;
    }
    *outEntries = list;
    *outCount   = count;
    return TCL_OK;
}

/*
 * ShouldDropAsCapturedProc
 * Heuristic to drop top-level proc forms that were captured dynamically.
 */

static int ShouldDropAsCapturedProc(const Tcl_Parse *parse, Tcl_Interp *interp, Tcl_Obj *nsPrefix, ProcEntry *procs, int nProcs) {
    if (!procs || nProcs <= 0)
        return 0;
    if (parse->numWords < 4)
        return 0;
    Tcl_Token *tName = WordToken(parse, 1);
    Tcl_Token *tArgs = WordToken(parse, 2);
    Tcl_Token *tBody = WordToken(parse, 3);
    if (!tName || !tArgs || !tBody)
        return 0;
    Tcl_Obj *nameObj = Tcl_NewStringObj(tName->start, (Tcl_Size)tName->size);
    Tcl_Obj *fq      = QualifyInNs(nameObj, nsPrefix);
    Tcl_Obj *argsObj = Tcl_NewStringObj(tArgs->start, (Tcl_Size)tArgs->size);
    Tcl_Obj *bodyObj = Tcl_NewStringObj(tBody->start, (Tcl_Size)tBody->size);
    int      drop    = 0;
    for (int i = 0; i < nProcs && !drop; ++i)
        drop = ProcTripleMatches(&procs[i], fq, argsObj, bodyObj);
    Tcl_DecrRefCount(fq);
    Tcl_DecrRefCount(argsObj);
    Tcl_DecrRefCount(bodyObj);
    Tcl_DecrRefCount(nameObj);
    return drop;
}

static Tcl_Obj *FilterScriptInNs(Tcl_Interp *interp, Tcl_Obj *script, Tcl_Obj *nsPrefix, ProcEntry *procs, int nProcs, int dropAllProcForms, int inClassBody) {
    Tcl_Size    totalBytes = 0;
    const char *s          = Tcl_GetStringFromObj(script, &totalBytes);
    const char *p = s, *end = s + totalBytes;
    Tcl_Parse   parse;
    Tcl_Obj    *out = Tcl_NewObj();
    Tcl_IncrRefCount(out);

    while (p < end) {
        if (Tcl_ParseCommand(interp, p, (int)(end - p), 0, &parse) != TCL_OK) {
            Tcl_DecrRefCount(out);
            return NULL;
        }
        int drop = 0; /* 0=keep, 1=drop whole command, 2=namespace-rewrite */
        if (parse.numWords > 0) {
            Tcl_Token *w0 = WordToken(&parse, 0);
            if (w0) {
                const char *w0s = w0->start;
                int         w0n = w0->size;
                if (IsProcCmdName(w0s, w0n) && parse.numWords >= 4) {
                    drop = dropAllProcForms ? 1 : ShouldDropAsCapturedProc(&parse, interp, nsPrefix, procs, nProcs);
                } else if (inClassBody && IsClassBodyMethodCmd(w0s, w0n)) {
                    drop = 1;
                } else if (!dropAllProcForms && ShouldDropAsCapturedProc(&parse, interp, nsPrefix, procs, nProcs)) {
                    drop = 1;
                } else if (dropAllProcForms && ShouldDropAsCapturedProc(&parse, interp, nsPrefix, procs, nProcs)) {
                    drop = 1;
                } else if (IsOoDefineCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *sub = WordToken(&parse, 2);
                    if (sub) {
                        if (TokenEquals(sub->start, sub->size, "method") || TokenEquals(sub->start, sub->size, "constructor") || TokenEquals(sub->start, sub->size, "destructor") ||
                            TokenEquals(sub->start, sub->size, "superclass") || TokenEquals(sub->start, sub->size, "eval")) {
                            drop = 1;
                        }
                    }
                } else if (IsOoClassCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *w1 = WordToken(&parse, 1);
                    if (w1 && TokenEquals(w1->start, w1->size, "create")) {
                        Tcl_Token *tName = WordToken(&parse, 2);
                        Tcl_Token *tBody = (parse.numWords >= 4 ? WordToken(&parse, parse.numWords - 1) : NULL);
                        if (tName && tBody) {
                            Tcl_Obj *nsName = Tcl_NewStringObj(tName->start, tName->size);
                            Tcl_Obj *nsNext = QualifyInNs(nsName, nsPrefix);
                            Tcl_DecrRefCount(nsName);
                            Tcl_Obj *body     = Tcl_NewStringObj(tBody->start, tBody->size);
                            Tcl_Obj *filtered = FilterScriptInNs(interp, body, nsNext, procs, nProcs, dropAllProcForms, 1);
                            Tcl_DecrRefCount(body);
                            Tcl_DecrRefCount(nsNext);
                            if (!filtered) {
                                Tcl_DecrRefCount(out);
                                Tcl_FreeParse(&parse);
                                return NULL;
                            }
                            Tcl_AppendToObj(out, parse.commandStart, (Tcl_Size)(tBody->start - parse.commandStart));
                            Tcl_AppendToObj(out, " {", 2);
                            Tcl_AppendObjToObj(out, filtered);
                            Tcl_AppendToObj(out, "}", 1);
                            Tcl_DecrRefCount(filtered);
                            drop = 2;
                        }
                    }
                } else if (IsNamespaceCmdName(w0s, w0n) && parse.numWords >= 3) {
                    Tcl_Token *w1 = WordToken(&parse, 1);
                    if (w1 && TokenEquals(w1->start, w1->size, "eval")) {
                        Tcl_Token *tNs   = WordToken(&parse, 2);
                        Tcl_Token *tBody = WordToken(&parse, parse.numWords - 1);
                        if (tNs && tBody) {
                            Tcl_Obj    *nsName = Tcl_NewStringObj(tNs->start, tNs->size);
                            Tcl_Obj    *body   = Tcl_NewStringObj(tBody->start, tBody->size);
                            Tcl_Obj    *nsNext = NULL;
                            const char *nn     = Tcl_GetString(nsName);
                            if (nn[0] == ':' && nn[1] == ':') {
                                nsNext = nsName;
                                Tcl_IncrRefCount(nsNext);
                            } else if (nsPrefix && Tcl_GetCharLength(nsPrefix) > 0) {
                                nsNext = Tcl_DuplicateObj(nsPrefix);
                                Tcl_AppendToObj(nsNext, "::", 2);
                                Tcl_AppendObjToObj(nsNext, nsName);
                                Tcl_IncrRefCount(nsNext);
                            } else {
                                nsNext = nsName;
                                Tcl_IncrRefCount(nsNext);
                            }
                            Tcl_Obj *filtered = FilterScriptInNs(interp, body, nsNext, procs, nProcs, dropAllProcForms, 0);
                            if (!filtered) {
                                Tcl_DecrRefCount(nsNext);
                                Tcl_DecrRefCount(nsName);
                                Tcl_DecrRefCount(body);
                                Tcl_DecrRefCount(out);
                                Tcl_FreeParse(&parse);
                                return NULL;
                            }
                            Tcl_AppendToObj(out, parse.commandStart, (Tcl_Size)(tBody->start - parse.commandStart));
                            Tcl_AppendToObj(out, " {", 2);
                            Tcl_AppendObjToObj(out, filtered);
                            Tcl_AppendToObj(out, "}", 1);
                            Tcl_DecrRefCount(filtered);
                            Tcl_DecrRefCount(nsNext);
                            Tcl_DecrRefCount(nsName);
                            Tcl_DecrRefCount(body);
                            drop = 2;
                        }
                    }
                }
            }
        }
        if (!drop) {
            Tcl_AppendToObj(out, parse.commandStart, (Tcl_Size)parse.commandSize);
        }
        p = parse.commandStart + parse.commandSize;
        Tcl_FreeParse(&parse);
    }
    return out;
}

/*
 * BuildTopLevelFiltered
 * Wrapper: filter top-level script with captured procs removed.
 */

static Tcl_Obj *BuildTopLevelFiltered(Tcl_Interp *interp, Tcl_Obj *script, ProcEntry *procs, int nProcs) {
    Tcl_Obj *empty = Tcl_NewStringObj("", -1);
    Tcl_IncrRefCount(empty);
    Tcl_Obj *r = FilterScriptInNs(interp, script, empty, procs, nProcs, 1, 0);
    Tcl_DecrRefCount(empty);
    return r;
}

static Tcl_Obj *BuildBodyFilteredInNs(Tcl_Interp *interp, Tcl_Obj *body, Tcl_Obj *nsPrefix, ProcEntry *procs, int nProcs) {
    return FilterScriptInNs(interp, body, nsPrefix, procs, nProcs, 0, 0);
}

/* ==========================================================================
 * File helpers
 * ========================================================================== */

/*
 * ReadWholeFile
 * Read a file into a binary Tcl_Obj.
 */

static int ReadWholeFile(Tcl_Interp *interp, const char *path, Tcl_Obj **out) {
    Tcl_Channel ch = Tcl_OpenFileChannel(interp, path, "r", 0);
    if (!ch)
        return TCL_ERROR;
    Tcl_SetChannelOption(interp, ch, "-translation", "binary");
    Tcl_Obj *buf = Tcl_NewObj();
    Tcl_IncrRefCount(buf);
    char     tmp[8192];
    Tcl_Size n;
    while ((n = Tcl_ReadRaw(ch, tmp, (Tcl_Size)sizeof tmp)) > 0)
        Tcl_AppendToObj(buf, tmp, n);
    Tcl_Close(NULL, ch);
    *out = buf;
    return TCL_OK;
}

static int FindEmptyStringLiteralIndex(const ByteCode *bc) {
    if (!bc || !bc->objArrayPtr)
        return -1;
    for (Tcl_Size i = 0; i < bc->numLitObjects; ++i) {
        Tcl_Size ln = 0;
        Tcl_GetStringFromObj(bc->objArrayPtr[i], &ln);
        if (ln == 0)
            return (int)i;
    }
    return -1;
}

static inline unsigned get_u4(const unsigned char *p) {
    return (unsigned)TclGetUInt4AtPtr(p);
}
static inline void put_u4(unsigned char *p, unsigned v) {
    FStoreUInt4AtPtr(p, (int)v);
}

static Tcl_Obj *QualifyNameString(Tcl_Obj *nameStr, Tcl_Obj *nsPrefix) {
    const char *n = Tcl_GetString(nameStr);
    if (n[0] == ':' && n[1] == ':') {
        Tcl_IncrRefCount(nameStr);
        return nameStr;
    }
    if (!nsPrefix || Tcl_GetCharLength(nsPrefix) == 0) {
        Tcl_IncrRefCount(nameStr);
        return nameStr;
    }
    Tcl_Size    plen = 0;
    const char *p    = Tcl_GetStringFromObj(nsPrefix, &plen);
    if (plen == 2 && p[0] == ':' && p[1] == ':') {
        Tcl_Obj *fq = Tcl_NewStringObj("::", 2);
        Tcl_AppendObjToObj(fq, nameStr);
        Tcl_IncrRefCount(fq);
        return fq;
    }
    Tcl_Obj *fq = Tcl_DuplicateObj(nsPrefix);
    Tcl_AppendToObj(fq, "::", 2);
    Tcl_AppendObjToObj(fq, nameStr);
    Tcl_IncrRefCount(fq);
    return fq;
}

static int NeutralizeProcCreates(ByteCode *bc, Tcl_Obj *nsPrefix, ProcEntry *procs, int nProcs, unsigned char **outPatched, size_t *outLen, int *outRewrites) {
    *outPatched = NULL;
    *outLen     = 0;
    if (outRewrites)
        *outRewrites = 0;
    if (!bc || !bc->codeStart || bc->numCodeBytes == 0 || bc->numLitObjects <= 0)
        return TCL_OK;

    int emptyIdx = FindEmptyStringLiteralIndex(bc);
    if (emptyIdx < 0) {
        return TCL_OK;
    }

    const unsigned char *code     = bc->codeStart;
    size_t               codeLen  = (size_t)bc->numCodeBytes;
    unsigned char       *copy     = NULL;
    int                  rewrites = 0;

    for (size_t i = 0; i + 5 <= codeLen; ++i) {
        if (code[i] != (unsigned char)INST_INVOKE_STK)
            continue;
        if (i + 5 > codeLen)
            break;
        unsigned nargs = get_u4(code + i + 1);
        if (nargs != 4u)
            continue;

        size_t p  = i;
        int    ok = 1;
        struct {
            size_t   pos;
            unsigned lit;
        } win[4];
        for (int k = 3; k >= 0; --k) {
            if (p >= 5 && code[p - 5] == (unsigned char)INST_PUSH) {
                win[k].pos = p - 5;
                win[k].lit = get_u4(code + p - 4);
                p -= 5;
            } else {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;
        unsigned litCmd = win[0].lit;
        if (litCmd >= (unsigned)bc->numLitObjects)
            continue;
        Tcl_Size    lc = 0;
        const char *sc = Tcl_GetStringFromObj(bc->objArrayPtr[litCmd], &lc);
        if (!NameEndsWith(sc, (int)lc, "proc"))
            continue;

        unsigned litName = win[1].lit, litArgs = win[2].lit, litBody = win[3].lit;
        if (litName >= (unsigned)bc->numLitObjects || litArgs >= (unsigned)bc->numLitObjects || litBody >= (unsigned)bc->numLitObjects)
            continue;

        Tcl_Obj *nameObj = bc->objArrayPtr[litName];
        Tcl_Obj *fqName  = QualifyNameString(nameObj, nsPrefix);
        Tcl_Obj *argsObj = bc->objArrayPtr[litArgs];
        Tcl_Obj *bodyObj = bc->objArrayPtr[litBody];

        int      match   = 0;
        for (int m = 0; m < nProcs && !match; ++m) {
            match = ProcTripleMatches(&procs[m], fqName, argsObj, bodyObj);
        }
        Tcl_DecrRefCount(fqName);
        if (!match)
            continue;

        if (!copy) {
            copy = (unsigned char *)Tcl_Alloc(codeLen);
            memcpy(copy, code, codeLen);
        }

        for (int k = 0; k < 4; ++k) {
            size_t pos = win[k].pos;
            size_t len = 5; /* INST_PUSH length */
            for (size_t b = 0; b < len; ++b)
                copy[pos + b] = (unsigned char)INST_NOP;
        }
        copy[i] = (unsigned char)INST_PUSH;
        put_u4(copy + i + 1, (unsigned)emptyIdx);

        ++rewrites;
        i += 4;
    }

    if (rewrites > 0) {
        *outPatched = copy;
        *outLen     = codeLen;
        if (outRewrites)
            *outRewrites = rewrites;
    }
    return TCL_OK;
}

/* ==========================================================================
 * High-level emitter
 * ========================================================================== */

/*
 * EmitTopLevelAndProcs
 * Serialize top-level bytecode, then proc and OO sections to a channel.
 */

static int EmitTopLevelAndProcs(Tcl_Interp *interp, Tcl_Obj *script, Tcl_Channel ch) {
    ProcEntry *procs  = NULL;
    int        nProcs = 0;
    if (CollectProcsFromScript(interp, script, NULL, &procs, &nProcs) != TCL_OK)
        return TCL_ERROR;
    {
        ProcEntry *dyn  = NULL;
        int        nDyn = 0;
        if (CollectProcsByEval(interp, script, &dyn, &nDyn) == TCL_OK && nDyn > 0) {
            if (MergeProcLists(interp, &procs, &nProcs, dyn, nDyn) != TCL_OK) {
                FreeProcEntryArray(dyn, nDyn);
                FreeProcEntryArray(procs, nProcs);
                return TCL_ERROR;
            }
        }
        FreeProcEntryArray(dyn, nDyn);
    }

    Tcl_Obj *top = BuildTopLevelFiltered(interp, script, procs, nProcs);
    if (top == NULL) {
        FreeProcEntryArray(procs, nProcs);
        return TCL_ERROR;
    }

    ByteCode *bc = NULL;
    if (GetByteCodeFromScript(interp, top, &bc) != TCL_OK) {
        Tcl_DecrRefCount(top);
        FreeProcEntryArray(procs, nProcs);
        return TCL_ERROR;
    }

    Tcl_SetChannelOption(interp, ch, "-translation", "binary");
    uint32_t numAuxW = 0;
    if (AssertAuxCoverage(interp, bc, &numAuxW) != TCL_OK) {
        Tcl_DecrRefCount(top);
        FreeProcEntryArray(procs, nProcs);
        return TCL_ERROR;
    }

    DynNsBodyCap nsBodiesScrub;
    (void)ScanNamespaceEvalBodies(bc, &nsBodiesScrub);
    WriteHeaderEx(ch, bc, numAuxW);
    wr(ch, bc->codeStart, (size_t)bc->numCodeBytes);

    for (Tcl_Size i = 0; i < bc->numLitObjects; ++i) {
        if (NsBodiesContainsLit(&nsBodiesScrub, (uint32_t)i)) {
            wr1(ch, (unsigned char)TBCX_LIT_STRING);
            wr4(ch, (uint32_t)0);
        } else {
            WriteLiteral(ch, bc->objArrayPtr[i]);
        }
    }
    for (Tcl_Size i = 0; i < bc->numAuxDataItems; ++i)
        if (CanWriteAux(&bc->auxDataArrayPtr[i]))
            WriteAux(ch, &bc->auxDataArrayPtr[i]);
    for (Tcl_Size i = 0; i < bc->numExceptRanges; ++i) {
        const ExceptionRange *x = &bc->exceptArrayPtr[i];
        wr1(ch, (unsigned char)(x->type == CATCH_EXCEPTION_RANGE ? 1u : 0u));
        wr4(ch, (uint32_t)x->nestingLevel);
        wr4(ch, (uint32_t)x->codeOffset);
        wr4(ch, (uint32_t)(x->codeOffset + x->numCodeBytes));
        wr4(ch, (uint32_t)x->continueOffset);
        wr4(ch, (uint32_t)x->breakOffset);
        wr4(ch, (uint32_t)x->catchOffset);
    }

    FreeNsBodyArray(nsBodiesScrub.list, nsBodiesScrub.count);
    Tcl_DecrRefCount(top);

    wr4(ch, (uint32_t)nProcs);
    for (int i = 0; i < nProcs; ++i) {
        const char *fqName = Tcl_GetString(procs[i].name);
        const char *nsName = Tcl_GetString(procs[i].ns);
        const char *argStr = Tcl_GetString(procs[i].args);

        wr4(ch, (uint32_t)strlen(fqName));
        wr(ch, fqName, strlen(fqName));
        wr4(ch, (uint32_t)strlen(nsName));
        wr(ch, nsName, strlen(nsName));
        wr4(ch, (uint32_t)strlen(argStr));
        wr(ch, argStr, strlen(argStr));

        Tcl_Obj *filteredBody = BuildBodyFilteredInNs(interp, procs[i].body, procs[i].ns, procs, nProcs);
        if (!filteredBody) {
            for (int j = 0; j < nProcs; ++j) {
                Tcl_DecrRefCount(procs[j].name);
                Tcl_DecrRefCount(procs[j].ns);
                Tcl_DecrRefCount(procs[j].args);
                Tcl_DecrRefCount(procs[j].body);
            }
            Tcl_Free((char *)procs);
            return TCL_ERROR;
        }
        Tcl_Obj *bodyObj   = NULL;
        int      numLocals = 0;

        if (CompileProcBodyForSave(interp, procs[i].name, procs[i].args, filteredBody, &bodyObj, &numLocals) != TCL_OK) {
            Tcl_DecrRefCount(filteredBody);
            for (int j = 0; j < nProcs; ++j) {
                Tcl_DecrRefCount(procs[j].name);
                Tcl_DecrRefCount(procs[j].ns);
                Tcl_DecrRefCount(procs[j].args);
                Tcl_DecrRefCount(procs[j].body);
            }
            Tcl_Free((char *)procs);
            return TCL_ERROR;
        }
        Tcl_DecrRefCount(filteredBody);
        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(bodyObj, tbcxTyBytecode);
        if (!ir) {
            Tcl_DecrRefCount(bodyObj);
            for (int j = 0; j < nProcs; ++j) {
                Tcl_DecrRefCount(procs[j].name);
                Tcl_DecrRefCount(procs[j].ns);
                Tcl_DecrRefCount(procs[j].args);
                Tcl_DecrRefCount(procs[j].body);
            }
            Tcl_Free((char *)procs);
            return TCL_ERROR;
        }
        ByteCode      *pbc        = (ByteCode *)ir->twoPtrValue.ptr1;

        unsigned char *patched    = NULL;
        size_t         patchedLen = 0;
        int            rewrites   = 0;
        (void)NeutralizeProcCreates(pbc, procs[i].ns, procs, nProcs, &patched, &patchedLen, &rewrites);
        if (patched) {
            wr4(ch, (uint32_t)patchedLen);
            wr(ch, patched, (Tcl_Size)patchedLen);
            Tcl_Free((char *)patched);
        } else {
            wr4(ch, (uint32_t)pbc->numCodeBytes);
            wr(ch, pbc->codeStart, (size_t)pbc->numCodeBytes);
        }

        wr4(ch, (uint32_t)pbc->numLitObjects);
        for (Tcl_Size j = 0; j < pbc->numLitObjects; ++j)
            WriteLiteral(ch, pbc->objArrayPtr[j]);

        uint32_t pAuxW = 0;
        if (AssertAuxCoverage(interp, pbc, &pAuxW) != TCL_OK) {
            Tcl_DecrRefCount(bodyObj);
            for (int j = 0; j < nProcs; ++j) {
                Tcl_DecrRefCount(procs[j].name);
                Tcl_DecrRefCount(procs[j].ns);
                Tcl_DecrRefCount(procs[j].args);
                Tcl_DecrRefCount(procs[j].body);
            }
            Tcl_Free((char *)procs);
            return TCL_ERROR;
        }
        wr4(ch, pAuxW);
        for (Tcl_Size j = 0; j < pbc->numAuxDataItems; ++j)
            if (CanWriteAux(&pbc->auxDataArrayPtr[j]))
                WriteAux(ch, &pbc->auxDataArrayPtr[j]);
        wr4(ch, (uint32_t)pbc->numExceptRanges);
        for (Tcl_Size j = 0; j < pbc->numExceptRanges; ++j) {
            const ExceptionRange *x = &pbc->exceptArrayPtr[j];
            wr1(ch, (unsigned char)(x->type == CATCH_EXCEPTION_RANGE ? 1u : 0u));
            wr4(ch, (uint32_t)x->nestingLevel);
            wr4(ch, (uint32_t)x->codeOffset);
            wr4(ch, (uint32_t)(x->codeOffset + x->numCodeBytes));
            wr4(ch, (uint32_t)x->continueOffset);
            wr4(ch, (uint32_t)x->breakOffset);
            wr4(ch, (uint32_t)x->catchOffset);
        }
        wr4(ch, (uint32_t)pbc->maxStackDepth);
        wr4(ch, (uint32_t)0);
        wr4(ch, (uint32_t)numLocals);

        /* Per-block def-body registrations for this proc (none for now; format reserved) */
        wr4(ch, (uint32_t)0);

        Tcl_DecrRefCount(bodyObj);
    }

    DynClassCap classesS;
    DynMethCap  methodsS;
    DynClassInit(&classesS);
    DynMethInit(&methodsS);

    if (CollectOOFromScript(interp, script, NULL, &classesS, &methodsS) != TCL_OK)
        return TCL_ERROR;
    {
        DynClassCap ctmp;
        DynMethCap  mtmp;
        DynClassInit(&ctmp);
        DynMethInit(&mtmp);
        if (CollectOOByEval(interp, script, &ctmp, &mtmp) == TCL_OK) {
            (void)MergeClassLists(&classesS, &ctmp);
            (void)MergeMethLists(&methodsS, &mtmp);
        }
        FreeClassEntryArray(ctmp.list, ctmp.count);
        FreeMethEntryArray(mtmp.list, mtmp.count);
    }

    wr4(ch, (uint32_t)classesS.count);
    for (int i = 0; i < classesS.count; ++i) {
        const char *cn   = Tcl_GetString(classesS.list[i].clsFqn);
        uint32_t    clen = (uint32_t)strlen(cn);
        wr4(ch, clen);
        if (clen)
            wr(ch, cn, clen);
        wr4(ch, (uint32_t)classesS.list[i].nSupers);
        for (int k = 0; k < classesS.list[i].nSupers; ++k) {
            const char *sn = Tcl_GetString(classesS.list[i].supers[k]);
            uint32_t    sl = (uint32_t)strlen(sn);
            wr4(ch, sl);
            if (sl)
                wr(ch, sn, sl);
        }
    }

    wr4(ch, (uint32_t)methodsS.count);
    for (int i = 0; i < methodsS.count; ++i) {
        const char *cls  = Tcl_GetString(methodsS.list[i].clsFqn);
        uint32_t    clsL = (uint32_t)strlen(cls);
        wr4(ch, clsL);
        if (clsL)
            wr(ch, cls, clsL);
        wr1(ch, (unsigned char)methodsS.list[i].kind);

        const char *nm  = (methodsS.list[i].methName ? Tcl_GetString(methodsS.list[i].methName) : "");
        uint32_t    nmL = (uint32_t)(methodsS.list[i].methName ? strlen(nm) : 0u);
        wr4(ch, nmL);
        if (nmL)
            wr(ch, nm, nmL);

        const char *args = Tcl_GetString(methodsS.list[i].args);
        uint32_t    aL   = (uint32_t)strlen(args);
        wr4(ch, aL);
        if (aL)
            wr(ch, args, aL);

        wr4(ch, (uint32_t)0);

        Tcl_Obj *filteredMethBody = BuildBodyFilteredInNs(interp, methodsS.list[i].body, methodsS.list[i].clsFqn, procs, nProcs);
        if (!filteredMethBody) {
            FreeClassEntryArray(classesS.list, classesS.count);
            FreeMethEntryArray(methodsS.list, methodsS.count);
            return TCL_ERROR;
        }

        Tcl_Obj *bodyObj   = NULL;
        int      numLocals = 0;
        if (CompileMethodBodyForSave(interp, methodsS.list[i].clsFqn, methodsS.list[i].methName, methodsS.list[i].kind, methodsS.list[i].args, filteredMethBody, &bodyObj, &numLocals) != TCL_OK) {
            Tcl_DecrRefCount(filteredMethBody);
            FreeClassEntryArray(classesS.list, classesS.count);
            FreeMethEntryArray(methodsS.list, methodsS.count);
            return TCL_ERROR;
        }
        Tcl_DecrRefCount(filteredMethBody);
        const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(bodyObj, tbcxTyBytecode);
        if (!ir) {
            Tcl_DecrRefCount(bodyObj);
            FreeClassEntryArray(classesS.list, classesS.count);
            FreeMethEntryArray(methodsS.list, methodsS.count);
            return TCL_ERROR;
        }
        ByteCode      *pbc        = (ByteCode *)ir->twoPtrValue.ptr1;
        unsigned char *patched    = NULL;
        size_t         patchedLen = 0;
        int            rewrites   = 0;
        (void)NeutralizeProcCreates(pbc, methodsS.list[i].clsFqn, procs, nProcs, &patched, &patchedLen, &rewrites);
        if (patched) {
            wr4(ch, (uint32_t)patchedLen);
            wr(ch, patched, (Tcl_Size)patchedLen);
            Tcl_Free((char *)patched);
        } else {
            wr4(ch, (uint32_t)pbc->numCodeBytes);
            wr(ch, pbc->codeStart, (size_t)pbc->numCodeBytes);
        }

        wr4(ch, (uint32_t)pbc->numLitObjects);
        for (Tcl_Size j = 0; j < pbc->numLitObjects; ++j)
            WriteLiteral(ch, pbc->objArrayPtr[j]);

        uint32_t pAuxW = 0;
        if (AssertAuxCoverage(interp, pbc, &pAuxW) != TCL_OK) {
            Tcl_DecrRefCount(bodyObj);
            FreeClassEntryArray(classesS.list, classesS.count);
            FreeMethEntryArray(methodsS.list, methodsS.count);
            return TCL_ERROR;
        }
        wr4(ch, pAuxW);
        for (Tcl_Size j = 0; j < pbc->numAuxDataItems; ++j)
            if (CanWriteAux(&pbc->auxDataArrayPtr[j]))
                WriteAux(ch, &pbc->auxDataArrayPtr[j]);

        wr4(ch, (uint32_t)pbc->numExceptRanges);
        for (Tcl_Size j = 0; j < pbc->numExceptRanges; ++j) {
            const ExceptionRange *x = &pbc->exceptArrayPtr[j];
            wr1(ch, (unsigned char)(x->type == CATCH_EXCEPTION_RANGE ? 1u : 0u));
            wr4(ch, (uint32_t)x->nestingLevel);
            wr4(ch, (uint32_t)x->codeOffset);
            wr4(ch, (uint32_t)(x->codeOffset + x->numCodeBytes));
            wr4(ch, (uint32_t)x->continueOffset);
            wr4(ch, (uint32_t)x->breakOffset);
            wr4(ch, (uint32_t)x->catchOffset);
        }
        wr4(ch, (uint32_t)pbc->maxStackDepth);
        wr4(ch, (uint32_t)0);
        wr4(ch, (uint32_t)numLocals);

        /* Per-block def-body registrations for this proc (none for now; format reserved) */
        wr4(ch, (uint32_t)0);

        Tcl_DecrRefCount(bodyObj);
    }

    {
        DynNsBodyCap nsBodies;
        if (ScanNamespaceEvalBodies(bc, &nsBodies) != TCL_OK) {
            FreeClassEntryArray(classesS.list, classesS.count);
            FreeMethEntryArray(methodsS.list, methodsS.count);
            FreeProcEntryArray(procs, nProcs);
            return TCL_ERROR;
        }
        wr4(ch, (uint32_t)nsBodies.count);
        for (int i = 0; i < nsBodies.count; ++i) {
            wr4(ch, nsBodies.list[i].litIndex);
            const char *nsName = Tcl_GetString(nsBodies.list[i].ns);
            uint32_t    nsLen  = (uint32_t)strlen(nsName);
            wr4(ch, nsLen);
            if (nsLen)
                wr(ch, nsName, nsLen);

            Tcl_Obj *bodyCompiled = NULL;
            int      numLocals    = 0;
            if (CompileNsBodyForSave(interp, nsBodies.list[i].ns, nsBodies.list[i].body, &bodyCompiled, &numLocals) != TCL_OK) {
                FreeClassEntryArray(classesS.list, classesS.count);
                FreeMethEntryArray(methodsS.list, methodsS.count);
                FreeProcEntryArray(procs, nProcs);
                FreeNsBodyArray(nsBodies.list, nsBodies.count);
                return TCL_ERROR;
            }
            const Tcl_ObjInternalRep *ir = Tcl_FetchInternalRep(bodyCompiled, tbcxTyBytecode);
            if (!ir) {
                Tcl_DecrRefCount(bodyCompiled);
                FreeClassEntryArray(classesS.list, classesS.count);
                FreeMethEntryArray(methodsS.list, methodsS.count);
                FreeProcEntryArray(procs, nProcs);
                FreeNsBodyArray(nsBodies.list, nsBodies.count);
                return TCL_ERROR;
            }
            ByteCode      *pbc        = (ByteCode *)ir->twoPtrValue.ptr1;

            unsigned char *patched    = NULL;
            size_t         patchedLen = 0;
            int            rewrites   = 0;
            (void)NeutralizeProcCreates(pbc, nsBodies.list[i].ns, procs, nProcs, &patched, &patchedLen, &rewrites);
            if (patched) {
                wr4(ch, (uint32_t)patchedLen);
                wr(ch, patched, (Tcl_Size)patchedLen);
                Tcl_Free((char *)patched);
            } else {
                wr4(ch, (uint32_t)pbc->numCodeBytes);
                wr(ch, pbc->codeStart, (size_t)pbc->numCodeBytes);
            }

            wr4(ch, (uint32_t)pbc->numLitObjects);
            for (Tcl_Size j = 0; j < pbc->numLitObjects; ++j)
                WriteLiteral(ch, pbc->objArrayPtr[j]);

            uint32_t bAuxW = 0;
            if (AssertAuxCoverage(interp, pbc, &bAuxW) != TCL_OK) {
                Tcl_DecrRefCount(bodyCompiled);
                FreeClassEntryArray(classesS.list, classesS.count);
                FreeMethEntryArray(methodsS.list, methodsS.count);
                FreeProcEntryArray(procs, nProcs);
                FreeNsBodyArray(nsBodies.list, nsBodies.count);
                return TCL_ERROR;
            }
            wr4(ch, bAuxW);
            for (Tcl_Size j = 0; j < pbc->numAuxDataItems; ++j)
                if (CanWriteAux(&pbc->auxDataArrayPtr[j]))
                    WriteAux(ch, &pbc->auxDataArrayPtr[j]);

            wr4(ch, (uint32_t)pbc->numExceptRanges);
            for (Tcl_Size j = 0; j < pbc->numExceptRanges; ++j) {
                const ExceptionRange *x = &pbc->exceptArrayPtr[j];
                wr1(ch, (unsigned char)(x->type == CATCH_EXCEPTION_RANGE ? 1u : 0u));
                wr4(ch, (uint32_t)x->nestingLevel);
                wr4(ch, (uint32_t)x->codeOffset);
                wr4(ch, (uint32_t)(x->codeOffset + x->numCodeBytes));
                wr4(ch, (uint32_t)x->continueOffset);
                wr4(ch, (uint32_t)x->breakOffset);
                wr4(ch, (uint32_t)x->catchOffset);
            }

            wr4(ch, (uint32_t)pbc->maxStackDepth);
            wr4(ch, (uint32_t)0);
            wr4(ch, (uint32_t)numLocals);

            Tcl_DecrRefCount(bodyCompiled);
        }
        FreeNsBodyArray(nsBodies.list, nsBodies.count);
    }

    FreeClassEntryArray(classesS.list, classesS.count);
    FreeMethEntryArray(methodsS.list, methodsS.count);
    FreeProcEntryArray(procs, nProcs);
    return TCL_OK;
}

/* ==========================================================================
 * Tcl commands
 * ========================================================================== */

/*
 * Tbcx_SaveObjCmd
 * tbcx::save script path — save compiled script to a TBCX file.
 */

int Tbcx_SaveObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script out.tbcx");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_OpenFileChannel(interp, Tcl_GetString(objv[2]), "w", 0666);
    if (!ch)
        return TCL_ERROR;
    int rc = EmitTopLevelAndProcs(interp, objv[1], ch);
    Tcl_Close(NULL, ch);

    if (rc == TCL_OK) {
        Tcl_Obj *norm = Tcl_FSGetNormalizedPath(interp, objv[2]);
        if (norm) {
            Tcl_IncrRefCount(norm);
            Tcl_SetObjResult(interp, norm);
            Tcl_DecrRefCount(norm);
        } else {
            Tcl_SetObjResult(interp, objv[2]);
        }
    }
    return rc;
}

/*
 * Tbcx_SaveChanObjCmd
 * tbcx::savechan script channel — save compiled script to an open channel.
 */

int Tbcx_SaveChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script outChannel");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_GetChannel(interp, Tcl_GetString(objv[2]), NULL);
    if (ch == NULL)
        return TCL_ERROR;
    Tcl_SetChannelOption(interp, ch, "-translation", "binary");
    Tcl_SetChannelOption(interp, ch, "-eofchar", "");
    return EmitTopLevelAndProcs(interp, objv[1], ch);
}

/*
 * Tbcx_SaveFileObjCmd
 * tbcx::savefile infile.tcl outfile.tbcx — compile & save a script file.
 */

int Tbcx_SaveFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "in.tcl out.tbcx");
        return TCL_ERROR;
    }

    Tcl_Obj *src = NULL;
    if (ReadWholeFile(interp, Tcl_GetString(objv[1]), &src) != TCL_OK)
        return TCL_ERROR;

    ByteCode *dummy = NULL;
    if (GetByteCodeFromScript(interp, src, &dummy) != TCL_OK) {
        Tcl_DecrRefCount(src);
        return TCL_ERROR;
    }

    Tcl_Channel ch = Tcl_OpenFileChannel(interp, Tcl_GetString(objv[2]), "w", 0666);
    if (!ch) {
        Tcl_DecrRefCount(src);
        return TCL_ERROR;
    }

    int rc = EmitTopLevelAndProcs(interp, src, ch);
    Tcl_Close(NULL, ch);
    Tcl_DecrRefCount(src);

    if (rc == TCL_OK) {
        Tcl_Obj *norm = Tcl_FSGetNormalizedPath(interp, objv[2]);
        if (norm) {
            Tcl_IncrRefCount(norm);
            Tcl_SetObjResult(interp, norm);
            Tcl_DecrRefCount(norm);
        } else {
            Tcl_SetObjResult(interp, objv[2]);
        }
    }
    return rc;
}
