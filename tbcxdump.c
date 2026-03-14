/* ==========================================================================
 * tbcxdump.c — Human-readable dump of .tbcx files (Tcl 9.1)
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static void AppendEscaped(Tcl_Obj* dst, const char* s, Tcl_Size n);
static void AppendLitPreview(Tcl_Obj* dst, Tcl_Obj* lit, int maxChars);
static void TbcxDisassembleCode(ByteCode* bc, Tcl_Obj* dst, const char* title);
static void DumpAuxArray(AuxData* auxArr, uint32_t numAux, Tcl_Obj* dst);
static void DumpBCDetails(Tcl_Obj* bcObj, Tcl_Obj* dst);
static void DumpExceptions(ExceptionRange* exArr, uint32_t numEx, Tcl_Obj* dst);
static int DumpLiteralValue(Tcl_Obj* lit, Tcl_Obj* dst);
static void DumpLocals(ByteCode* bc, Tcl_Obj* dst);
static int DumpProcsSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out);
static int DumpClassesSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out);
static int DumpMethodsSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out);
int Tbcx_DumpObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);

/* ==========================================================================
 * Dump Helpers
 * ========================================================================== */

static void AppendEscaped(Tcl_Obj* dst, const char* s, Tcl_Size n)
{
    Tcl_DString ds;
    Tcl_DStringInit(&ds);
    for (Tcl_Size i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '\\' || c == '"')
        {
            /* Use Tcl_DString formatting instead of raw sprintf */
            char esc[5];
            esc[0] = '\\';
            esc[1] = 'x';
            esc[2] = "0123456789ABCDEF"[(c >> 4) & 0xF];
            esc[3] = "0123456789ABCDEF"[c & 0xF];
            esc[4] = '\0';
            Tcl_DStringAppend(&ds, esc, 4);
        }
        else
        {
            Tcl_DStringAppend(&ds, (const char*)&s[i], 1);
        }
    }
    Tcl_AppendPrintfToObj(dst, "%s", Tcl_DStringValue(&ds));
    Tcl_DStringFree(&ds);
}

/* Append a short preview of a literal value for inline annotation.
   Output is capped at maxChars to keep disassembly readable. */
static void AppendLitPreview(Tcl_Obj* dst, Tcl_Obj* lit, int maxChars)
{
    if (!lit)
    {
        Tcl_AppendToObj(dst, "(null)", -1);
        return;
    }
    const Tcl_ObjType* ty = lit->typePtr;
    if (ty == tbcxTyBytecode)
    {
        Tcl_AppendToObj(dst, "(bytecode)", -1);
        return;
    }
    if (ty == tbcxTyByteArray)
    {
        Tcl_Size n = 0;
        (void)Tcl_GetByteArrayFromObj(lit, &n);
        Tcl_AppendPrintfToObj(dst, "(bytes:%ld)", (long)n);
        return;
    }
    Tcl_Size sLen = 0;
    const char* s = Tcl_GetStringFromObj(lit, &sLen);
    if (sLen <= maxChars)
    {
        Tcl_AppendToObj(dst, "\"", 1);
        AppendEscaped(dst, s, sLen);
        Tcl_AppendToObj(dst, "\"", 1);
    }
    else
    {
        Tcl_AppendToObj(dst, "\"", 1);
        AppendEscaped(dst, s, (Tcl_Size)maxChars);
        Tcl_AppendToObj(dst, "...\"", -1);
    }
}

/* Helper: look up local variable name from ByteCode, returns "" for unnamed. */
static const char* LocalName(ByteCode* bc, int idx, Tcl_Size* outLen)
{
    static const char empty[] = "";
    if (outLen)
        *outLen = 0;
    if (!bc)
        return empty;
    /* Try LocalCache first */
    if (bc->localCachePtr && idx >= 0 && idx < bc->localCachePtr->numVars)
    {
        Tcl_Obj** names = (Tcl_Obj**)&bc->localCachePtr->varName0;
        if (names[idx])
            return Tcl_GetStringFromObj(names[idx], outLen);
    }
    /* Fallback: walk Proc compiled locals */
    if (bc->procPtr)
    {
        for (CompiledLocal* cl = bc->procPtr->firstLocalPtr; cl; cl = cl->nextPtr)
        {
            if (cl->frameIndex == idx && cl->nameLength > 0)
            {
                if (outLen)
                    *outLen = (Tcl_Size)cl->nameLength;
                return cl->name;
            }
        }
    }
    return empty;
}

/* ==========================================================================
 * Native bytecode disassembler — walks raw code bytes using the
 * instruction descriptor table to decode names and operands.
 *
 * The table is obtained via TclGetInstructionTable() (internal stubs)
 * rather than referencing the unexported tclInstructionTable symbol.
 *
 * Advantages over ::tcl::unsupported::disassemble:
 *   - Works on TCL_BYTECODE_PRECOMPILED objects without flag manipulation
 *   - No dependency on an unsupported Tcl command
 *   - Richer annotations: literal value previews, variable names, jump targets
 * ========================================================================== */

static void TbcxDisassembleCode(ByteCode* bc, Tcl_Obj* dst, const char* title)
{
    if (!bc || !bc->codeStart || bc->numCodeBytes <= 0)
    {
        Tcl_AppendPrintfToObj(dst, "  Disassembly (%s): (empty)\n", title ? title : "");
        return;
    }

    const InstructionDesc* instTable = (const InstructionDesc*)TclGetInstructionTable();
    const unsigned char* code = bc->codeStart;
    Tcl_Size len = bc->numCodeBytes;

    Tcl_AppendPrintfToObj(dst, "  Disassembly (%s):\n", title ? title : "");

    Tcl_Size pc = 0;
    while (pc < len)
    {
        unsigned int opcode = code[pc];

        /* Unknown opcode — hex dump and advance 1 byte */
        if (opcode > LAST_INST_OPCODE)
        {
            Tcl_AppendPrintfToObj(dst, "    (%4" TCL_Z_MODIFIER "d) ??? 0x%02x\n", pc, opcode);
            pc++;
            continue;
        }

        const InstructionDesc* desc = &instTable[opcode];
        Tcl_AppendPrintfToObj(dst, "    (%4" TCL_Z_MODIFIER "d) %-20s", pc, desc->name);

        /* Safety: if numBytes extends past the code block, print what we can
           and stop.  This handles truncated/corrupt bytecode gracefully. */
        if (pc + desc->numBytes > len)
        {
            Tcl_AppendToObj(dst, " <truncated>\n", -1);
            break;
        }

        /* Decode operands — offset past the 1-byte opcode */
        const unsigned char* op = code + pc + 1;
        int opOff = 0;         /* byte offset into operand area */
        int lvtIdx = -1;       /* LVT index for variable annotation */
        int jumpTarget = -1;   /* computed jump target for offset annotation */
        int firstUintVal = -1; /* first UINT operand value (for push detection) */

        for (int i = 0; i < desc->numOperands; i++)
        {
            switch (desc->opTypes[i])
            {
                case OPERAND_INT1:
                {
                    int val = (int)(int8_t)op[opOff];
                    Tcl_AppendPrintfToObj(dst, " %d", val);
                    opOff += 1;
                    break;
                }
                case OPERAND_INT4:
                {
                    int32_t val = (int32_t)(((int32_t)(int8_t)op[opOff] << 24) | (op[opOff + 1] << 16) | (op[opOff + 2] << 8) |
                                            op[opOff + 3]);
                    Tcl_AppendPrintfToObj(dst, " %d", (int)val);
                    opOff += 4;
                    break;
                }
                case OPERAND_UINT1:
                {
                    unsigned val = op[opOff];
                    Tcl_AppendPrintfToObj(dst, " %u", val);
                    if (i == 0 && firstUintVal < 0)
                        firstUintVal = (int)val;
                    opOff += 1;
                    break;
                }
                case OPERAND_UINT4:
                {
                    uint32_t val = ((uint32_t)op[opOff] << 24) | ((uint32_t)op[opOff + 1] << 16) |
                                   ((uint32_t)op[opOff + 2] << 8) | (uint32_t)op[opOff + 3];
                    Tcl_AppendPrintfToObj(dst, " %u", val);
                    if (i == 0 && firstUintVal < 0)
                        firstUintVal = (int)val;
                    opOff += 4;
                    break;
                }
                case OPERAND_IDX4:
                {
                    uint32_t val = ((uint32_t)op[opOff] << 24) | ((uint32_t)op[opOff + 1] << 16) |
                                   ((uint32_t)op[opOff + 2] << 8) | (uint32_t)op[opOff + 3];
                    Tcl_AppendPrintfToObj(dst, " %u", val);
                    opOff += 4;
                    break;
                }
                case OPERAND_LVT1:
                {
                    unsigned val = op[opOff];
                    Tcl_AppendPrintfToObj(dst, " %%v%u", val);
                    lvtIdx = (int)val;
                    opOff += 1;
                    break;
                }
                case OPERAND_LVT4:
                {
                    uint32_t val = ((uint32_t)op[opOff] << 24) | ((uint32_t)op[opOff + 1] << 16) |
                                   ((uint32_t)op[opOff + 2] << 8) | (uint32_t)op[opOff + 3];
                    Tcl_AppendPrintfToObj(dst, " %%v%u", val);
                    lvtIdx = (int)val;
                    opOff += 4;
                    break;
                }
                case OPERAND_AUX4:
                {
                    uint32_t val = ((uint32_t)op[opOff] << 24) | ((uint32_t)op[opOff + 1] << 16) |
                                   ((uint32_t)op[opOff + 2] << 8) | (uint32_t)op[opOff + 3];
                    Tcl_AppendPrintfToObj(dst, " aux#%u", val);
                    opOff += 4;
                    break;
                }
                case OPERAND_OFFSET1:
                {
                    int val = (int)(int8_t)op[opOff];
                    jumpTarget = (int)pc + val;
                    Tcl_AppendPrintfToObj(dst, " %+d", val);
                    opOff += 1;
                    break;
                }
                case OPERAND_OFFSET4:
                {
                    int32_t val = (int32_t)(((int32_t)(int8_t)op[opOff] << 24) | (op[opOff + 1] << 16) | (op[opOff + 2] << 8) |
                                            op[opOff + 3]);
                    jumpTarget = (int)pc + val;
                    Tcl_AppendPrintfToObj(dst, " %+d", (int)val);
                    opOff += 4;
                    break;
                }
                case OPERAND_SCLS1:
                {
                    unsigned val = op[opOff];
                    Tcl_AppendPrintfToObj(dst, " scls=%u", val);
                    opOff += 1;
                    break;
                }
                default:
                    Tcl_AppendToObj(dst, " ???", -1);
                    break;
            }
        }

        /* ---- Inline annotations ---- */

        /* Literal value preview for push-family instructions.
           Detect by instruction name (covers "push1", "push4", "push")
           rather than opcode constants which are deprecated in Tcl 9.1. */
        if (firstUintVal >= 0 && desc->name[0] == 'p' && desc->name[1] == 'u' && desc->name[2] == 's' && desc->name[3] == 'h' &&
            firstUintVal < bc->numLitObjects && bc->objArrayPtr[firstUintVal])
        {
            Tcl_AppendToObj(dst, " \t# ", -1);
            AppendLitPreview(dst, bc->objArrayPtr[firstUintVal], 48);
        }

        /* Variable name annotation for LVT operands */
        if (lvtIdx >= 0)
        {
            Tcl_Size nLen = 0;
            const char* nm = LocalName(bc, lvtIdx, &nLen);
            if (nLen > 0)
            {
                Tcl_AppendToObj(dst, " \t# \"", -1);
                AppendEscaped(dst, nm, nLen);
                Tcl_AppendToObj(dst, "\"", 1);
            }
        }

        /* Jump target annotation */
        if (jumpTarget >= 0)
        {
            Tcl_AppendPrintfToObj(dst, " \t# -> pc %d", jumpTarget);
        }

        Tcl_AppendToObj(dst, "\n", 1);
        pc += desc->numBytes;
    }
}

static int DumpLiteralValue(Tcl_Obj* lit, Tcl_Obj* dst)
{
    const Tcl_ObjType* ty = lit->typePtr;
    /* In Tcl 9.1 the integer types were unified: tbcxTyBignum == tbcxTyInt.
       Probe with WideInt first to label small integers correctly. */
    if (ty == tbcxTyBignum || ty == tbcxTyInt)
    {
        Tcl_WideInt wv = 0;
        if (Tcl_GetWideIntFromObj(NULL, lit, &wv) == TCL_OK)
        {
            Tcl_AppendPrintfToObj(dst, "(int) %" TCL_LL_MODIFIER "d", (long long)wv);
        }
        else
        {
            /* True bignum — doesn't fit in 64 bits */
            Tcl_Size n = 0;
            const char* s = Tcl_GetStringFromObj(lit, &n);
            Tcl_AppendToObj(dst, "(bignum) ", -1);
            AppendEscaped(dst, s, n);
        }
        return TCL_OK;
    }
    else if (tbcxTyBoolean && tbcxTyBoolean != tbcxTyInt && ty == tbcxTyBoolean)
    {
        int b = 0;
        Tcl_GetBooleanFromObj(NULL, lit, &b);
        Tcl_AppendPrintfToObj(dst, "(boolean) %s", b ? "true" : "false");
        return TCL_OK;
    }
    else if (ty == tbcxTyByteArray)
    {
        Tcl_Size n = 0;
        unsigned char* p = Tcl_GetByteArrayFromObj(lit, &n);
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
    }
    else if (ty == tbcxTyDict)
    {
        Tcl_Size n = 0;
        const char* s = Tcl_GetStringFromObj(lit, &n);
        Tcl_AppendToObj(dst, "(dict) ", -1);
        AppendEscaped(dst, s, n);
        return TCL_OK;
    }
    else if (ty == tbcxTyDouble)
    {
        double d = 0;
        Tcl_GetDoubleFromObj(NULL, lit, &d);
        Tcl_AppendPrintfToObj(dst, "(double) %.17g", d);
        return TCL_OK;
    }
    else if (ty == tbcxTyList)
    {
        Tcl_AppendToObj(dst, "(list) ", -1);
        Tcl_AppendObjToObj(dst, lit);
        return TCL_OK;
    }
    else if (ty == tbcxTyBytecode)
    {
        Tcl_AppendToObj(dst, "(bytecode)", -1);
        return TCL_OK;
    }
    else
    {
        Tcl_Size n = 0;
        const char* s = Tcl_GetStringFromObj(lit, &n);
        Tcl_AppendToObj(dst, "(string) \"", -1);
        AppendEscaped(dst, s, n);
        Tcl_AppendToObj(dst, "\"", 1);
        return TCL_OK;
    }
}

static void DumpAuxArray(AuxData* auxArr, uint32_t numAux, Tcl_Obj* dst)
{
    if (!numAux)
        return;
    Tcl_AppendToObj(dst, "  AuxData:\n", -1);

    for (uint32_t i = 0; i < numAux; i++)
    {
        const AuxDataType* ty = auxArr[i].type;
        const char* tn = (ty && ty->name) ? ty->name : "(unknown)";
        ClientData cd = auxArr[i].clientData;

        /* Be defensive: never dereference clientData unless we are really sure
           of the aux type.  Recognize by pointer *or* by type name, so we stay
           robust across builds where we only have the name. */
        int isJTStr = (ty == tbcxAuxJTStr) || (tn && strcmp(tn, "JumptableInfo") == 0);
        int isJTNum = (ty == tbcxAuxJTNum) || (tn && strcmp(tn, "JumptableNumInfo") == 0);
        int isDictUpd = (ty == tbcxAuxDictUpdate) || (tn && strcmp(tn, "DictUpdateInfo") == 0);
        int isNewFor = (ty == tbcxAuxNewForeach) || (tn && strcmp(tn, "NewForeachInfo") == 0);

        Tcl_AppendPrintfToObj(dst, "    #%u: %s", i, tn);

        if (!cd)
        {
            Tcl_AppendToObj(dst, " (no data)\n", -1);
            continue;
        }

        if (isJTStr)
        {
            JumptableInfo* info = (JumptableInfo*)cd;
            Tcl_AppendToObj(dst, " (jumptable:str)\n", -1);
            /* Iterate the string-keyed hash in a safe way */
            Tcl_HashSearch s;
            Tcl_HashEntry* e;
            for (e = Tcl_FirstHashEntry(&info->hashTable, &s); e; e = Tcl_NextHashEntry(&s))
            {
                const char* k = (const char*)Tcl_GetHashKey(&info->hashTable, e);
                int target = PTR2INT(Tcl_GetHashValue(e));
                Tcl_AppendPrintfToObj(dst, "      \"%s\" -> pc %d\n", k ? k : "", target);
            }
            continue;
        }

        if (isJTNum)
        {
            JumptableNumInfo* info = (JumptableNumInfo*)cd;
            Tcl_AppendToObj(dst, " (jumptable:num)\n", -1);
            /* Key representation depends on how the table was created.
               The loader uses TCL_ONE_WORD_KEYS on LP64 and TCL_STRING_KEYS on ILP32. */
            Tcl_HashSearch s;
            Tcl_HashEntry* e;
            for (e = Tcl_FirstHashEntry(&info->hashTable, &s); e; e = Tcl_NextHashEntry(&s))
            {
                int target = PTR2INT(Tcl_GetHashValue(e));
                if (info->hashTable.keyType == TCL_ONE_WORD_KEYS)
                {
                    Tcl_WideInt key = (Tcl_WideInt)(intptr_t)Tcl_GetHashKey(&info->hashTable, e);
                    Tcl_AppendPrintfToObj(dst, "      %" TCL_LL_MODIFIER "d -> pc %d\n", (long long)key, target);
                }
                else
                {
                    /* string-keys case: store as decimal strings in the table */
                    const char* ks = (const char*)Tcl_GetHashKey(&info->hashTable, e);
                    if (ks && *ks)
                    {
                        /* show the key as-is (it was created from a 64-bit value) */
                        Tcl_AppendPrintfToObj(dst, "      %s -> pc %d\n", ks, target);
                    }
                    else
                    {
                        Tcl_AppendPrintfToObj(dst, "      <key?> -> pc %d\n", target);
                    }
                }
            }
            continue;
        }

        if (isDictUpd)
        {
            DictUpdateInfo* info = (DictUpdateInfo*)cd;
            Tcl_AppendToObj(dst, " (dictupdate)\n", -1);
            Tcl_AppendPrintfToObj(dst, "      %ld variable indices:", (long)info->length);
            for (Tcl_Size k = 0; k < info->length; k++)
            {
                Tcl_AppendPrintfToObj(dst, " %ld", (long)info->varIndices[k]);
            }
            Tcl_AppendToObj(dst, "\n", 1);
            continue;
        }

        if (isNewFor)
        {
            ForeachInfo* info = (ForeachInfo*)cd;
            Tcl_AppendToObj(dst, " (newForeach)\n", -1);
            Tcl_AppendPrintfToObj(dst,
                                  "      lists=%ld, firstTmp=%d, loopCtTmp=%d\n",
                                  (long)info->numLists,
                                  (int)info->firstValueTemp,
                                  (int)info->loopCtTemp);
            for (Tcl_Size L = 0; L < info->numLists; L++)
            {
                ForeachVarList* vl = info->varLists[L];
                Tcl_AppendPrintfToObj(dst, "      list#%ld vars:", (long)L);
                if (vl)
                {
                    for (Tcl_Size j = 0; j < vl->numVars; j++)
                    {
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

static void DumpExceptions(ExceptionRange* exArr, uint32_t numEx, Tcl_Obj* dst)
{
    if (!numEx)
        return;
    Tcl_AppendToObj(dst, "  Exception Ranges:\n", -1);
    for (uint32_t i = 0; i < numEx; i++)
    {
        ExceptionRange* er = &exArr[i];
        const char* t = (er->type == LOOP_EXCEPTION_RANGE) ? "loop" : (er->type == CATCH_EXCEPTION_RANGE) ? "catch" : "other";
        Tcl_AppendPrintfToObj(dst,
                              "    #%u: %s [pc %" TCL_Z_MODIFIER "d .. %" TCL_Z_MODIFIER "d]",
                              i,
                              t,
                              er->codeOffset,
                              er->codeOffset + er->numCodeBytes);
        if (er->type == LOOP_EXCEPTION_RANGE)
        {
            Tcl_AppendPrintfToObj(dst,
                                  ", continue->pc %" TCL_Z_MODIFIER "d, break->pc %" TCL_Z_MODIFIER "d\n",
                                  er->continueOffset,
                                  er->breakOffset);
        }
        else if (er->type == CATCH_EXCEPTION_RANGE)
        {
            Tcl_AppendPrintfToObj(dst, ", catch->pc %" TCL_Z_MODIFIER "d\n", er->catchOffset);
        }
        else
        {
            Tcl_AppendToObj(dst, "\n", 1);
        }
    }
}

static void DumpLocals(ByteCode* bc, Tcl_Obj* dst)
{
    if (!bc)
    {
        Tcl_AppendToObj(dst, "  Locals: 0\n", -1);
        return;
    }
    /* 1) LocalCache present (top-level or proc/method with cache) */
    if (bc->localCachePtr && bc->localCachePtr->numVars > 0)
    {
        Tcl_Size n = (Tcl_Size)bc->localCachePtr->numVars;
        Tcl_Obj** namev = (Tcl_Obj**)&bc->localCachePtr->varName0; /* Tcl 9.1 */
        Tcl_AppendPrintfToObj(dst, "  Locals (%" TCL_Z_MODIFIER "d):\n", n);
        for (Tcl_Size i = 0; i < n; i++)
        {
            Tcl_Size ln = 0;
            const char* s = namev[i] ? Tcl_GetStringFromObj(namev[i], &ln) : "";
            Tcl_AppendPrintfToObj(dst, "    [%" TCL_Z_MODIFIER "d] \"", i);
            AppendEscaped(dst, s, ln);
            Tcl_AppendToObj(dst, "\"\n", 2);
        }
        return;
    }
    /* 2) Fallback: compiled locals from the Proc, if any */
    if (bc->procPtr && bc->procPtr->numCompiledLocals > 0)
    {
        Proc* p = bc->procPtr;
        Tcl_Size n = (Tcl_Size)p->numCompiledLocals;
        CompiledLocal* cl = p->firstLocalPtr;
        Tcl_AppendPrintfToObj(dst, "  Locals (%" TCL_Z_MODIFIER "d) [from Proc]:\n", n);
        for (; cl; cl = cl->nextPtr)
        {
            const char* nm = cl->name;              /* trailing array (always valid) */
            Tcl_Size ln = (Tcl_Size)cl->nameLength; /* exact length from compiler */
            Tcl_AppendPrintfToObj(dst, "    [%" TCL_Z_MODIFIER "d] \"", (Tcl_Size)cl->frameIndex);
            AppendEscaped(dst, nm, ln);
            Tcl_AppendToObj(dst, "\"", 1);
            if (cl->flags & VAR_ARGUMENT)
                Tcl_AppendToObj(dst, "  (arg)", -1);
            if (cl->flags & VAR_IS_ARGS)
                Tcl_AppendToObj(dst, "  (args…)", -1);
            Tcl_AppendToObj(dst, "\n", 1);
        }
        return;
    }
    /* 3) No locals */
    Tcl_AppendToObj(dst, "  Locals: 0\n", -1);
}

static void DumpBCDetails(Tcl_Obj* bcObj, Tcl_Obj* dst)
{
    if (!bcObj)
        return;
    ByteCode* bc = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, bc);
    if (!bc)
        return;

    Tcl_AppendPrintfToObj(dst,
                          "  code=%ld, stack=%ld, lits=%ld, aux=%ld, except=%ld\n",
                          (long)bc->numCodeBytes,
                          (long)bc->maxStackDepth,
                          (long)bc->numLitObjects,
                          (long)bc->numAuxDataItems,
                          (long)bc->numExceptRanges);
    DumpLocals(bc, dst);
    if (bc->numLitObjects > 0)
    {
        Tcl_AppendToObj(dst, "  Literals:\n", -1);
        for (Tcl_Size i = 0; i < bc->numLitObjects; i++)
        {
            Tcl_AppendPrintfToObj(dst, "    [%ld] ", (long)i);
            DumpLiteralValue(bc->objArrayPtr[i], dst);
            Tcl_AppendToObj(dst, "\n", 1);
        }
    }
    DumpAuxArray(bc->auxDataArrayPtr, (uint32_t)bc->numAuxDataItems, dst);
    DumpExceptions(bc->exceptArrayPtr, (uint32_t)bc->numExceptRanges, dst);
}

/* Convenience wrapper: extract ByteCode* from a Tcl_Obj and disassemble. */
static void DisassembleBCObj(Tcl_Obj* bcObj, Tcl_Obj* dst, const char* title)
{
    if (!bcObj)
        return;
    ByteCode* bcPtr = NULL;
    ByteCodeGetInternalRep(bcObj, tbcxTyBytecode, bcPtr);
    TbcxDisassembleCode(bcPtr, dst, title);
}

/* ==========================================================================
 * Section helpers — each reads one section of the .tbcx stream and
 * appends human-readable output to `out`.  Returns TCL_OK on success.
 * On error, sets the interp result and returns TCL_ERROR; the caller
 * is responsible for top-level cleanup (topBC, out, channel).
 * ========================================================================== */

static int DumpProcsSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out)
{
    uint32_t numProcs = 0;
    if (!Tbcx_R_U32(r, &numProcs))
        return TCL_ERROR;
    Tcl_AppendPrintfToObj(out, "\nProcs: %u\n", numProcs);

    for (uint32_t i = 0; i < numProcs; i++)
    {
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

        Tcl_AppendToObj(out, "  - proc ", -1);
        Tcl_AppendObjToObj(out, nameFqn);
        Tcl_AppendToObj(out, "  (ns=", -1);
        Tcl_AppendObjToObj(out, nsObj);
        Tcl_AppendToObj(out, ")\n", -1);
        Tcl_AppendToObj(out, "    args: ", -1);
        Tcl_AppendObjToObj(out, argsObj);
        Tcl_AppendToObj(out, "\n", 1);

        Namespace* nsPtr = (Namespace*)Tcl_FindNamespace(interp, Tcl_GetString(nsObj), NULL, 0);
        if (!nsPtr)
            nsPtr = (Namespace*)Tcl_GetGlobalNamespace(interp);
        uint32_t nLoc = 0;
        Tcl_Obj* bodyBC = Tbcx_ReadBlock(r, interp, nsPtr, &nLoc, 0, 1);
        if (!bodyBC)
        {
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nsObj);
            Tcl_DecrRefCount(nameFqn);
            return TCL_ERROR;
        }
        Tcl_IncrRefCount(bodyBC);

        DisassembleBCObj(bodyBC, out, Tcl_GetString(nameFqn));
        DumpBCDetails(bodyBC, out);

        Tcl_DecrRefCount(bodyBC);
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nsObj);
        Tcl_DecrRefCount(nameFqn);
    }
    return TCL_OK;
}

static int DumpClassesSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out)
{
    (void)interp; /* currently unused — classes section is metadata only */
    uint32_t numClasses = 0;
    if (!Tbcx_R_U32(r, &numClasses))
        return TCL_ERROR;
    Tcl_AppendPrintfToObj(out, "\nClasses: %u\n", numClasses);

    for (uint32_t c = 0; c < numClasses; c++)
    {
        char* cls = NULL;
        uint32_t cl = 0;
        if (!Tbcx_R_LPString(r, &cls, &cl))
            return TCL_ERROR;
        Tcl_Obj* clsObj = Tcl_NewStringObj(cls, (Tcl_Size)cl);
        Tcl_IncrRefCount(clsObj);
        Tcl_Free(cls);
        Tcl_AppendToObj(out, "  - class ", -1);
        Tcl_AppendObjToObj(out, clsObj);
        Tcl_AppendToObj(out, "\n", 1);

        uint32_t nSup = 0;
        if (!Tbcx_R_U32(r, &nSup))
        {
            Tcl_DecrRefCount(clsObj);
            return TCL_ERROR;
        }
        for (uint32_t s = 0; s < nSup; s++)
        {
            char* su = NULL;
            uint32_t sl = 0;
            if (!Tbcx_R_LPString(r, &su, &sl))
            {
                Tcl_DecrRefCount(clsObj);
                return TCL_ERROR;
            }
            Tcl_Free(su); /* acknowledged; saver typically emits 0 */
        }
        Tcl_DecrRefCount(clsObj);
    }
    return TCL_OK;
}

static int DumpMethodsSection(TbcxIn* r, Tcl_Interp* interp, Tcl_Obj* out)
{
    uint32_t numMethods = 0;
    if (!Tbcx_R_U32(r, &numMethods))
        return TCL_ERROR;
    Tcl_AppendPrintfToObj(out, "\nMethods: %u\n", numMethods);

    for (uint32_t m = 0; m < numMethods; m++)
    {
        char* clsf = NULL;
        uint32_t clsL = 0;
        if (!Tbcx_R_LPString(r, &clsf, &clsL))
            return TCL_ERROR;
        Tcl_Obj* clsFqn = Tcl_NewStringObj(clsf, (Tcl_Size)clsL);
        Tcl_IncrRefCount(clsFqn);
        Tcl_Free(clsf);

        uint8_t kind = 0;
        if (!Tbcx_R_U8(r, &kind))
        {
            Tcl_DecrRefCount(clsFqn);
            return TCL_ERROR;
        }

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

        Tcl_AppendToObj(out, "  - ", -1);
        Tcl_AppendObjToObj(out, clsFqn);
        const char* kname = (kind == TBCX_METH_INST)    ? "::method "
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

        Namespace* clsNs = (Namespace*)Tcl_FindNamespace(interp, Tcl_GetString(clsFqn), NULL, 0);
        if (!clsNs)
            clsNs = (Namespace*)Tcl_GetGlobalNamespace(interp);
        uint32_t nLoc = 0;
        Tcl_Obj* bodyBC = Tbcx_ReadBlock(r, interp, clsNs, &nLoc, 0, 1);
        if (!bodyBC)
        {
            Tcl_DecrRefCount(argsObj);
            Tcl_DecrRefCount(nameObj);
            Tcl_DecrRefCount(clsFqn);
            return TCL_ERROR;
        }

        Tcl_IncrRefCount(bodyBC);
        DisassembleBCObj(bodyBC, out, Tcl_GetString(nameObj));
        DumpBCDetails(bodyBC, out);

        Tcl_DecrRefCount(bodyBC);
        Tcl_DecrRefCount(argsObj);
        Tcl_DecrRefCount(nameObj);
        Tcl_DecrRefCount(clsFqn);
    }
    return TCL_OK;
}

/* ==========================================================================
 * Tcl command
 *
 * Synopsis:   tbcx::dump filename
 * Arguments:  filename — path to a .tbcx file
 * Returns:    human-readable disassembly of the .tbcx contents.
 * Errors:     TCL_ERROR on file open failure, read error, or malformed input.
 * Thread:     must be called on the interp-owning thread.
 * ========================================================================== */

int Tbcx_DumpObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[])
{
    if (objc != 2)
    {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        return TCL_ERROR;
    }

    Tcl_Channel in = Tcl_FSOpenFileChannel(interp, objv[1], "r", 0);
    if (!in)
        return TCL_ERROR;
    if (Tbcx_CheckBinaryChan(interp, in) != TCL_OK)
    {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    TbcxIn r;
    Tbcx_R_Init(&r, interp, in);
    TbcxHeader H;
    if (!Tbcx_ReadHeader(&r, &H) || r.err)
    {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    Tcl_Obj* out = Tcl_NewObj();
    Tcl_IncrRefCount(out);
    int rc = TCL_ERROR;

    /* Header */
    Tcl_AppendToObj(out, "TBCX Header:\n", -1);
    Tcl_AppendPrintfToObj(out,
                          "  magic = 0x%08X ('%c''%c''%c''%c')\n",
                          H.magic,
                          (H.magic) & 0xFF,
                          (H.magic >> 8) & 0xFF,
                          (H.magic >> 16) & 0xFF,
                          (H.magic >> 24) & 0xFF);
    Tcl_AppendPrintfToObj(out, "  format = %u\n", H.format);
    Tcl_AppendPrintfToObj(out,
                          "  tcl_version = %u.%u.%u (type %u)\n",
                          (unsigned)((H.tcl_version >> 24) & 0xFFu),
                          (unsigned)((H.tcl_version >> 16) & 0xFFu),
                          (unsigned)((H.tcl_version >> 8) & 0xFFu),
                          (unsigned)(H.tcl_version & 0xFFu));
    Tcl_AppendPrintfToObj(out,
                          "  top: code=%" PRIu64 ", except=%u, lits=%u, aux=%u, locals=%u, stack=%u\n",
                          (unsigned long long)H.codeLenTop,
                          H.numExceptTop,
                          H.numLitsTop,
                          H.numAuxTop,
                          H.numLocalsTop,
                          H.maxStackTop);

    /* Top-level block */
    Namespace* curNs = (Namespace*)Tcl_GetGlobalNamespace(interp);
    uint32_t dummyNL = 0;
    Tcl_Obj* topBC = Tbcx_ReadBlock(&r, interp, curNs, &dummyNL, 0, 1);
    if (!topBC)
        goto cleanup_no_topbc;
    Tcl_IncrRefCount(topBC);
    Tcl_AppendToObj(out, "\nTop-level block:\n", -1);
    DisassembleBCObj(topBC, out, "top-level");
    DumpBCDetails(topBC, out);

    /* Sections */
    if (DumpProcsSection(&r, interp, out) != TCL_OK)
        goto cleanup;
    if (DumpClassesSection(&r, interp, out) != TCL_OK)
        goto cleanup;
    if (DumpMethodsSection(&r, interp, out) != TCL_OK)
        goto cleanup;

    rc = TCL_OK;

cleanup:
    Tcl_DecrRefCount(topBC);
cleanup_no_topbc:
    if (Tcl_Close(interp, in) != TCL_OK)
        rc = TCL_ERROR;
    if (rc == TCL_OK)
    {
        Tcl_SetObjResult(interp, out);
    }
    Tcl_DecrRefCount(out);
    return rc;
}
