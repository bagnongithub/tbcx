/* ==========================================================================
 * tbcxdump.c — Human-readable dump of .tbcx files (Tcl 9.1)
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static void AppendEscaped(Tcl_Obj *dst, const char *s, Tcl_Size n);
static int  DisassembleBC(Tcl_Interp *ip, Tcl_Obj *bc, Tcl_Obj *dst, const char *title);
static void DumpAuxArray(Tcl_Interp *ip, AuxData *auxArr, uint32_t numAux, Tcl_Obj *dst);
static void DumpBCDetails(Tcl_Interp *ip, Tcl_Obj *bcObj, Tcl_Obj *dst);
static void DumpExceptions(ExceptionRange *exArr, uint32_t numEx, Tcl_Obj *dst);
int         Tbcx_DumpFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);

/* ==========================================================================
 * Stuff
 * ========================================================================== */

static void AppendEscaped(Tcl_Obj *dst, const char *s, Tcl_Size n) {
    Tcl_DString ds;
    Tcl_DStringInit(&ds);
    for (Tcl_Size i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '\\' || c == '"') {
            char buf[5];
            sprintf(buf, "\\x%02X", c);
            Tcl_DStringAppend(&ds, buf, -1);
        } else {
            Tcl_DStringAppend(&ds, (const char *)&s[i], 1);
        }
    }
    Tcl_AppendPrintfToObj(dst, "%s", Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
}

static int DumpLiteralValue(Tcl_Interp *ip, Tcl_Obj *lit, Tcl_Obj *dst) {
    const Tcl_ObjType *ty = lit->typePtr;
    if (ty == tbcxTyBignum) {
        Tcl_Size    n = 0;
        const char *s = Tcl_GetStringFromObj(lit, &n);
        Tcl_AppendToObj(dst, "(bignum) ", -1);
        AppendEscaped(dst, s, n);
        return TCL_OK;
    } else if (ty == tbcxTyBoolean) {
        int b = 0;
        Tcl_GetBooleanFromObj(NULL, lit, &b);
        Tcl_AppendPrintfToObj(dst, "(boolean) %s", b ? "true" : "false");
        return TCL_OK;
    } else if (ty == tbcxTyByteArray) {
        Tcl_Size       n = 0;
        unsigned char *p = Tcl_GetByteArrayFromObj(lit, &n);
        Tcl_AppendPrintfToObj(dst, "(bytearray) %ld bytes", (long)n);
        if (n)
            Tcl_AppendToObj(dst, " (hex:", -1);
        for (Tcl_Size i = 0; i < n && i < 32; i++)
            Tcl_AppendPrintfToObj(dst, " %02X", p[i]);
        if (n > 32)
            Tcl_AppendToObj(dst, " …", -1);
        if (n)
            Tcl_AppendToObj(dst, ")", -1);
        return TCL_OK;
    } else if (ty == tbcxTyDict) {
        Tcl_Size    n = 0;
        const char *s = Tcl_GetStringFromObj(lit, &n);
        Tcl_AppendToObj(dst, "(dict) ", -1);
        AppendEscaped(dst, s, n);
        return TCL_OK;
    } else if (ty == tbcxTyDouble) {
        double d = 0;
        Tcl_GetDoubleFromObj(NULL, lit, &d);
        Tcl_AppendPrintfToObj(dst, "(double) %.17g", d);
        return TCL_OK;
    } else if (ty == tbcxTyList) {
        Tcl_AppendToObj(dst, "(list) ", -1);
        Tcl_AppendObjToObj(dst, lit);
        return TCL_OK;
    } else if (ty == tbcxTyBytecode) {
        Tcl_AppendToObj(dst, "(bytecode)", -1);
        return TCL_OK;
    } else if (ty == tbcxTyInt) {
        Tcl_WideInt wv = 0;
        Tcl_GetWideIntFromObj(NULL, lit, &wv);
        Tcl_AppendPrintfToObj(dst, "(int) %" TCL_LL_MODIFIER "d", (long long)wv);
        return TCL_OK;
    } else {
        Tcl_Size    n = 0;
        const char *s = Tcl_GetStringFromObj(lit, &n);
        Tcl_AppendToObj(dst, "(string) \"", -1);
        AppendEscaped(dst, s, n);
        Tcl_AppendToObj(dst, "\"", 1);
        return TCL_OK;
    }
}

static void DumpAuxArray(Tcl_Interp *ip, AuxData *auxArr, uint32_t numAux, Tcl_Obj *dst) {
    if (!numAux)
        return;
    Tcl_AppendToObj(dst, "  AuxData:\n", -1);

    for (uint32_t i = 0; i < numAux; i++) {
        const AuxDataType *ty        = auxArr[i].type;
        const char        *tn        = (ty && ty->name) ? ty->name : "(unknown)";
        ClientData         cd        = auxArr[i].clientData;

        /* Be defensive: never dereference clientData unless we are really sure
           of the aux type.  Recognize by pointer *or* by type name, so we stay
           robust across builds where we only have the name. */
        int                isJTStr   = (ty == tbcxAuxJTStr) || (tn && strcmp(tn, "JumptableInfo") == 0);
        int                isJTNum   = (ty == tbcxAuxJTNum) || (tn && strcmp(tn, "JumptableNumInfo") == 0);
        int                isDictUpd = (ty == tbcxAuxDictUpdate) || (tn && strcmp(tn, "DictUpdateInfo") == 0);
        int                isForeach = (ty == tbcxAuxForeach) || (tn && strcmp(tn, "ForeachInfo") == 0);
        int                isNewFor  = (ty == tbcxAuxNewForeach) || (tn && strcmp(tn, "NewForeachInfo") == 0);

        Tcl_AppendPrintfToObj(dst, "    #%u: %s", i, tn);

        if (!cd) {
            Tcl_AppendToObj(dst, " (no data)\n", -1);
            continue;
        }

        if (isJTStr) {
            JumptableInfo *info = (JumptableInfo *)cd;
            Tcl_AppendToObj(dst, " (jumptable:str)\n", -1);
            /* Iterate the string-keyed hash in a safe way */
            Tcl_HashSearch s;
            Tcl_HashEntry *e;
            for (e = Tcl_FirstHashEntry(&info->hashTable, &s); e; e = Tcl_NextHashEntry(&s)) {
                const char *k      = (const char *)Tcl_GetHashKey(&info->hashTable, e);
                int         target = PTR2INT(Tcl_GetHashValue(e));
                Tcl_AppendPrintfToObj(dst, "      \"%s\" -> pc %d\n", k ? k : "", target);
            }
            continue;
        }

        if (isJTNum) {
            JumptableNumInfo *info = (JumptableNumInfo *)cd;
            Tcl_AppendToObj(dst, " (jumptable:num)\n", -1);
            /* Key representation depends on how the table was created.
               The loader uses TCL_ONE_WORD_KEYS on LP64 and TCL_STRING_KEYS on ILP32. */
            Tcl_HashSearch s;
            Tcl_HashEntry *e;
            for (e = Tcl_FirstHashEntry(&info->hashTable, &s); e; e = Tcl_NextHashEntry(&s)) {
                int target = PTR2INT(Tcl_GetHashValue(e));
                if (info->hashTable.keyType == TCL_ONE_WORD_KEYS) {
                    Tcl_WideInt key = (Tcl_WideInt)(intptr_t)Tcl_GetHashKey(&info->hashTable, e);
                    Tcl_AppendPrintfToObj(dst, "      %" TCL_LL_MODIFIER "d -> pc %d\n", (long long)key, target);
                } else {
                    /* string-keys case: store as decimal strings in the table */
                    const char *ks = (const char *)Tcl_GetHashKey(&info->hashTable, e);
                    if (ks && *ks) {
                        /* show the key as-is (it was created from a 64-bit value) */
                        Tcl_AppendPrintfToObj(dst, "      %s -> pc %d\n", ks, target);
                    } else {
                        Tcl_AppendPrintfToObj(dst, "      <key?> -> pc %d\n", target);
                    }
                }
            }
            continue;
        }

        if (isDictUpd) {
            DictUpdateInfo *info = (DictUpdateInfo *)cd;
            Tcl_AppendToObj(dst, " (dictupdate)\n", -1);
            Tcl_AppendPrintfToObj(dst, "      %ld variable indices:", (long)info->length);
            for (Tcl_Size k = 0; k < info->length; k++) {
                Tcl_AppendPrintfToObj(dst, " %ld", (long)info->varIndices[k]);
            }
            Tcl_AppendToObj(dst, "\n", 1);
            continue;
        }

        if (isForeach || isNewFor) {
            ForeachInfo *info = (ForeachInfo *)cd;
            Tcl_AppendPrintfToObj(dst, " (%s)\n", isNewFor ? "newForeach" : "foreach");
            Tcl_AppendPrintfToObj(dst, "      lists=%ld, firstTmp=%d, loopCtTmp=%d\n", (long)info->numLists, (int)info->firstValueTemp, (int)info->loopCtTemp);
            for (Tcl_Size L = 0; L < info->numLists; L++) {
                ForeachVarList *vl = info->varLists[L];
                Tcl_AppendPrintfToObj(dst, "      list#%ld vars:", (long)L);
                if (vl) {
                    for (Tcl_Size j = 0; j < vl->numVars; j++) {
                        Tcl_AppendPrintfToObj(dst, " %d", (int)vl->varIndexes[j]);
                    }
                }
                Tcl_AppendToObj(dst, "\n", 1);
            }
            continue;
        }

        /* Unknown aux type: be gentle and do not touch clientData. */
        Tcl_AppendToObj(dst, " (unknown)\n", -1);
    }
}

static void DumpExceptions(ExceptionRange *exArr, uint32_t numEx, Tcl_Obj *dst) {
    if (!numEx)
        return;
    Tcl_AppendToObj(dst, "  Exception Ranges:\n", -1);
    for (uint32_t i = 0; i < numEx; i++) {
        ExceptionRange *er = &exArr[i];
        const char     *t  = (er->type == LOOP_EXCEPTION_RANGE) ? "loop" : (er->type == CATCH_EXCEPTION_RANGE) ? "catch" : "other";
        Tcl_AppendPrintfToObj(dst, "    #%u: %s [pc %ld .. %ld]", i, t, er->codeOffset, er->codeOffset + er->numCodeBytes);
        if (er->type == LOOP_EXCEPTION_RANGE) {
            Tcl_AppendPrintfToObj(dst, ", continue->pc %ld, break->pc %ld\n", er->continueOffset, er->breakOffset);
        } else if (er->type == CATCH_EXCEPTION_RANGE) {
            Tcl_AppendPrintfToObj(dst, ", catch->pc %ld\n", er->catchOffset);
        } else {
            Tcl_AppendToObj(dst, "\n", 1);
        }
    }
}

static void DumpBCDetails(Tcl_Interp *ip, Tcl_Obj *bcObj, Tcl_Obj *dst) {
    if (!bcObj)
        return;
    ByteCode *bc = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, bc);
    if (!bc)
        return;

    Tcl_AppendPrintfToObj(dst, "  ByteCode meta: codeBytes=%ld, maxStack=%ld, numLits=%ld, numAux=%ld, numExcept=%ld\n", (long)bc->numCodeBytes, (long)bc->maxStackDepth, (long)bc->numLitObjects,
                          (long)bc->numAuxDataItems, (long)bc->numExceptRanges);

    if (bc->numLitObjects > 0) {
        Tcl_AppendToObj(dst, "  Literals:\n", -1);
        for (Tcl_Size i = 0; i < bc->numLitObjects; i++) {
            Tcl_AppendPrintfToObj(dst, "    [%ld] ", (long)i);
            DumpLiteralValue(ip, bc->objArrayPtr[i], dst);
            Tcl_AppendToObj(dst, "\n", 1);
        }
    }
    DumpAuxArray(ip, bc->auxDataArrayPtr, (uint32_t)bc->numAuxDataItems, dst);
    DumpExceptions(bc->exceptArrayPtr, (uint32_t)bc->numExceptRanges, dst);
}

static int DisassembleBC(Tcl_Interp *ip, Tcl_Obj *bc, Tcl_Obj *dst, const char *title) {
    Tcl_Obj  *cmd[3];

    /* Allow disassembly of prebuilt images by temporarily clearing the
       TCL_BYTECODE_PRECOMPILED flag. The unsupported disassembler refuses
       to run when that bit is set. We restore it afterwards. */
    ByteCode *bcPtr = NULL;
    ByteCodeGetInternalRep(bc, tbcxTyBytecode, bcPtr);
    int hadPre = (bcPtr && (bcPtr->flags & TCL_BYTECODE_PRECOMPILED)) ? 1 : 0;
    if (hadPre) {
        bcPtr->flags &= ~TCL_BYTECODE_PRECOMPILED;
    }

    cmd[0] = Tcl_NewStringObj("::tcl::unsupported::disassemble", -1);
    cmd[1] = Tcl_NewStringObj("script", -1);
    cmd[2] = bc;
    Tcl_IncrRefCount(cmd[0]);
    Tcl_IncrRefCount(cmd[1]);
    Tcl_IncrRefCount(cmd[2]);

    int rc = Tcl_EvalObjv(ip, 3, cmd, TCL_EVAL_GLOBAL);

    if (hadPre) {
        bcPtr->flags |= TCL_BYTECODE_PRECOMPILED;
    }

    if (rc == TCL_OK) {
        Tcl_Obj *res = Tcl_GetObjResult(ip);
        Tcl_AppendPrintfToObj(dst, "  Disassembly (%s):\n", title ? title : "");
        Tcl_AppendObjToObj(dst, res);
        Tcl_AppendToObj(dst, "\n", 1);
    } else {
        Tcl_Obj *err = Tcl_GetObjResult(ip);
        Tcl_AppendPrintfToObj(dst, "  [disassemble error: %s]\n", err ? Tcl_GetString(err) : "?");
    }
    Tcl_DecrRefCount(cmd[0]);
    Tcl_DecrRefCount(cmd[1]);
    Tcl_DecrRefCount(cmd[2]);
    return rc;
}

/* ==========================================================================
 * Tcl command
 * ========================================================================== */

int Tbcx_DumpFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        return TCL_ERROR;
    }

    Tcl_Channel in = Tcl_FSOpenFileChannel(interp, objv[1], "r", 0);
    if (!in)
        return TCL_ERROR;
    if (CheckBinaryChan(interp, in) != TCL_OK) {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    TbcxIn     r = {interp, in, TCL_OK};
    TbcxHeader H;
    if (!ReadHeader(&r, &H) || r.err) {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    Tcl_Obj *out = Tcl_NewObj();

    Tcl_AppendToObj(out, "TBCX Header:\n", -1);
    Tcl_AppendPrintfToObj(out, "  magic = 0x%08X ('%c''%c''%c''%c')\n", H.magic, (H.magic) & 0xFF, (H.magic >> 8) & 0xFF, (H.magic >> 16) & 0xFF, (H.magic >> 24) & 0xFF);
    Tcl_AppendPrintfToObj(out, "  format = %u\n", H.format);
    Tcl_AppendPrintfToObj(out, "  tcl_version = %u.%u.%u (type %u)\n", (unsigned)((H.tcl_version >> 24) & 0xFFu), (unsigned)((H.tcl_version >> 16) & 0xFFu), (unsigned)((H.tcl_version >> 8) & 0xFFu),
                          (unsigned)(H.tcl_version & 0xFFu));
    Tcl_AppendPrintfToObj(out, "  top: codeLen=%" PRIu64 ", numExcept=%u, numLits=%u, numAux=%u, numLocals=%u, maxStack=%u\n", (unsigned long long)H.codeLenTop, H.numExceptTop, H.numLitsTop,
                          H.numAuxTop, H.numLocalsTop, H.maxStackTop);

    /* Top-level block */
    Namespace *curNs   = (Namespace *)Tcl_GetCurrentNamespace(interp);
    uint32_t   dummyNL = 0;
    Tcl_Obj   *topBC   = ReadCompiledBlock(&r, interp, curNs, &dummyNL);
    if (!topBC) {
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }
    Tcl_IncrRefCount(topBC);
    Tcl_AppendToObj(out, "\nTop-level block:\n", -1);
    (void)DisassembleBC(interp, topBC, out, "top-level");
    DumpBCDetails(interp, topBC, out);

    /* Procs */
    uint32_t numProcs = 0;
    if (!R_U32(&r, &numProcs)) {
        Tcl_DecrRefCount(topBC);
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "\nProcs: %u\n", numProcs);
    for (uint32_t i = 0; i < numProcs; i++) {
        char    *nameC = NULL;
        uint32_t nameL = 0;
        char    *nsC   = NULL;
        uint32_t nsL   = 0;
        char    *argsC = NULL;
        uint32_t argsL = 0;
        if (!R_LPString(&r, &nameC, &nameL)) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        if (!R_LPString(&r, &nsC, &nsL)) {
            Tcl_Free(nameC);
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        if (!R_LPString(&r, &argsC, &argsL)) {
            Tcl_Free(nameC);
            Tcl_Free(nsC);
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }

        Tcl_Obj *nameFqn = Tcl_NewStringObj(nameC, (Tcl_Size)nameL);
        Tcl_Obj *nsObj   = Tcl_NewStringObj(nsC, (Tcl_Size)nsL);
        Tcl_Obj *argsObj = Tcl_NewStringObj(argsC, (Tcl_Size)argsL);
        Tcl_Free(nameC);
        Tcl_Free(nsC);
        Tcl_Free(argsC);

        Tcl_AppendToObj(out, "  - proc ", -1);
        Tcl_AppendObjToObj(out, nameFqn);
        Tcl_AppendToObj(out, "  (ns=", -1);
        Tcl_AppendObjToObj(out, nsObj);
        Tcl_AppendToObj(out, ")\n", -1);
        Tcl_AppendToObj(out, "    args: ", -1);
        Tcl_AppendObjToObj(out, argsObj);
        Tcl_AppendToObj(out, "\n", 1);

        Namespace *nsPtr  = (Namespace *)EnsureNamespace(interp, Tcl_GetString(nsObj));
        uint32_t   nLoc   = 0;
        Tcl_Obj   *bodyBC = ReadCompiledBlock(&r, interp, nsPtr, &nLoc);
        if (!bodyBC) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        Tcl_IncrRefCount(bodyBC); /* hold during disasm/print */

        (void)DisassembleBC(interp, bodyBC, out, Tcl_GetString(nameFqn));
        DumpBCDetails(interp, bodyBC, out);

        Tcl_DecrRefCount(bodyBC);
    }

    /* Classes */
    uint32_t numClasses = 0;
    if (!R_U32(&r, &numClasses)) {
        Tcl_DecrRefCount(topBC);
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "\nClasses: %u\n", numClasses);
    for (uint32_t c = 0; c < numClasses; c++) {
        char    *cls = NULL;
        uint32_t cl  = 0;
        if (!R_LPString(&r, &cls, &cl)) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        Tcl_Obj *clsObj = Tcl_NewStringObj(cls, (Tcl_Size)cl);
        Tcl_Free(cls);
        Tcl_AppendToObj(out, "  - class ", -1);
        Tcl_AppendObjToObj(out, clsObj);
        Tcl_AppendToObj(out, "\n", 1);

        uint32_t nSup = 0;
        if (!R_U32(&r, &nSup)) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        for (uint32_t s = 0; s < nSup; s++) {
            char    *su = NULL;
            uint32_t sl = 0;
            if (!R_LPString(&r, &su, &sl)) {
                Tcl_DecrRefCount(topBC);
                Tcl_DecrRefCount(out);
                Tcl_Close(interp, in);
                return TCL_ERROR;
            }
            Tcl_Free(su); /* acknowledged; saver typically emits 0 */
        }
    }

    /* Methods */
    uint32_t numMethods = 0;
    if (!R_U32(&r, &numMethods)) {
        Tcl_DecrRefCount(topBC);
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "\nMethods: %u\n", numMethods);
    for (uint32_t m = 0; m < numMethods; m++) {
        char    *clsf = NULL;
        uint32_t clsL = 0;
        if (!R_LPString(&r, &clsf, &clsL)) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        Tcl_Obj *clsFqn = Tcl_NewStringObj(clsf, (Tcl_Size)clsL);
        Tcl_Free(clsf);

        uint8_t kind = 0;
        if (!R_U8(&r, &kind)) {
            Tcl_DecrRefCount(clsFqn);
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }

        char    *mname = NULL;
        uint32_t mnL   = 0;
        if (!R_LPString(&r, &mname, &mnL)) {
            Tcl_DecrRefCount(clsFqn);
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        Tcl_Obj *nameObj = Tcl_NewStringObj(mname, (Tcl_Size)mnL);
        Tcl_Free(mname);

        char    *args = NULL;
        uint32_t aL   = 0;
        if (!R_LPString(&r, &args, &aL)) {
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }
        Tcl_Obj *argsObj = Tcl_NewStringObj(args, (Tcl_Size)aL);
        Tcl_Free(args);

        Tcl_AppendToObj(out, "  - ", -1);
        Tcl_AppendObjToObj(out, clsFqn);
        const char *kname = (kind == TBCX_METH_INST)    ? "::method "
                            : (kind == TBCX_METH_CLASS) ? "::classmethod "
                            : (kind == TBCX_METH_CTOR)  ? "::constructor "
                            : (kind == TBCX_METH_DTOR)  ? "::destructor "
                                                        : "::(unknown) ";
        Tcl_AppendPrintfToObj(out, " %s", kname);
        Tcl_AppendObjToObj(out, nameObj);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_AppendToObj(out, "    args: ", -1);
        Tcl_AppendObjToObj(out, argsObj);
        Tcl_AppendToObj(out, "\n", 1);

        Namespace *clsNs  = (Namespace *)EnsureNamespace(interp, Tcl_GetString(clsFqn));
        uint32_t   nLoc   = 0;
        Tcl_Obj   *bodyBC = ReadCompiledBlock(&r, interp, clsNs, &nLoc);
        if (!bodyBC) {
            Tcl_DecrRefCount(topBC);
            Tcl_DecrRefCount(out);
            Tcl_Close(interp, in);
            return TCL_ERROR;
        }

        Tcl_IncrRefCount(bodyBC);
        (void)DisassembleBC(interp, bodyBC, out, Tcl_GetString(nameObj));
        DumpBCDetails(interp, bodyBC, out);

        Tcl_DecrRefCount(bodyBC);
    }

    Tcl_DecrRefCount(topBC);
    if (Tcl_Close(interp, in) != TCL_OK) {
        Tcl_DecrRefCount(out);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, out);
    return TCL_OK;
}
