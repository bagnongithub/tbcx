/* ==========================================================================
 * tbcxdump.c — Disassemble and print bytecode saved in .tbcx files
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct {
    Tcl_Channel ch;
    int64_t     limit; /* remaining bytes available; -1 means unlimited */
} Reader;

typedef enum {
    LIT_BIGNUM    = TBCX_LIT_BIGNUM,
    LIT_BOOLEAN   = TBCX_LIT_BOOLEAN,
    LIT_BYTEARR   = TBCX_LIT_BYTEARR,
    LIT_DICT      = TBCX_LIT_DICT,
    LIT_DOUBLE    = TBCX_LIT_DOUBLE,
    LIT_LIST      = TBCX_LIT_LIST,
    LIT_STRING    = TBCX_LIT_STRING,
    LIT_WIDEINT   = TBCX_LIT_WIDEINT,
    LIT_WIDEUINT  = TBCX_LIT_WIDEUINT,
    LIT_LAMBDA_BC = TBCX_LIT_LAMBDA_BC,
    LIT_BYTECODE  = TBCX_LIT_BYTECODE
} TbcxLitTag;

typedef struct {
    TbcxLitTag tag;
    Tcl_Obj   *preview; /* refcounted Tcl_Obj* with a short printable form */
} LitPreview;

typedef enum { OP_NONE, OP_INT1, OP_INT4, OP_UINT1, OP_UINT4, OP_IDX4, OP_LVT1, OP_LVT4, OP_AUX4, OP_OFF1, OP_OFF4, OP_LIT1, OP_LIT4, OP_SCLS1, OP_UNSF1, OP_CLK1, OP_LRPL1 } OpKind;

typedef struct {
    const char *name;
    unsigned    nops;
    OpKind      op[2];
    unsigned    deprecated; /* 1 if deprecated — disassembly forbidden */
} OpInfo;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static void            AppendQuoted(Tcl_Obj *out, const unsigned char *s, Tcl_Size n);
static int             DisassembleCode(Tcl_Obj *out, const unsigned char *code, uint32_t codeLen, const LitPreview *lits, uint32_t nLits, Tcl_Obj *err);
static int             DumpCompiledBlock(Reader *r, Tcl_Obj *out, Tcl_Obj *err);
static int             DumpFile(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj);
static int             DumpHeader(Reader *r, Tcl_Obj *out, Tcl_Obj *err);
static int             DumpOneAux(Reader *r, Tcl_Obj *out, Tcl_Obj *err, uint32_t idx);
static int             DumpOneClass(Reader *r, Tcl_Obj *out, Tcl_Obj *err);
static int             DumpOneMethod(Reader *r, Tcl_Obj *out, Tcl_Obj *err);
static void            FreeLitPreviewArray(LitPreview *arr, uint32_t n);
static inline int32_t  I32LE(const unsigned char *p);
static Tcl_Obj        *MakeHexBytes(const unsigned char *p, Tcl_Size n, Tcl_Size maxShow);
static inline unsigned OpKindSize(OpKind k);
static Tcl_Obj        *PreviewFromStringBytes(const unsigned char *s, uint32_t n);
static void            PrintOperand(Tcl_Obj *out, OpKind k, const unsigned char *p, Tcl_Size pc, const LitPreview *lits, uint32_t nLits);
static int             R_Bytes(Reader *r, unsigned char *buf, Tcl_Size n, Tcl_Obj *err);
static int             R_LPString(Reader *r, unsigned char **bufOut, uint32_t *lenOut, Tcl_Obj *err);
static int             R_U32(Reader *r, uint32_t *v, Tcl_Obj *err);
static int             R_U64(Reader *r, uint64_t *v, Tcl_Obj *err);
static int             R_U8(Reader *r, unsigned char *v, Tcl_Obj *err);
static int             ReadOneLiteral(Reader *r, LitPreview *dst, Tcl_Obj *out, uint32_t idx, Tcl_Obj *err);
int                    Tbcx_DumpFileObjCmd(ClientData cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
static inline uint32_t U32LE(const unsigned char *p);

/* ==========================================================================
 * Bytecode
 * ========================================================================== */

/* NOTE: The order MUST match Tcl 9.1's tclInstructionTable[] exactly.
 * We mark the legacy 1‑byte forms as deprecated.
 */
#define BTOP0(nm) {nm, 0, {OP_NONE, OP_NONE}, 0}
#define BTOP1(nm, t1) {nm, 1, {t1, OP_NONE}, 0}
#define BTOP2(nm, t1, t2) {nm, 2, {t1, t2}, 0}
#define DTOP0(nm) {nm, 0, {OP_NONE, OP_NONE}, 1}
#define DTOP1(nm, t1) {nm, 1, {t1, OP_NONE}, 1}
#define DTOP2(nm, t1, t2) {nm, 2, {t1, t2}, 1}

static const OpInfo opTable[] = {
    /* 0..9 */
    BTOP0("done"),
    DTOP1("push1", OP_LIT1),
    BTOP1("push", OP_LIT4),
    BTOP0("pop"),
    BTOP0("dup"),
    BTOP1("strcat", OP_UINT1),
    DTOP1("invokeStk1", OP_UINT1),
    BTOP1("invokeStk", OP_UINT4),
    BTOP0("evalStk"),
    BTOP0("exprStk"),
    /* 10..23 */
    DTOP1("loadScalar1", OP_LVT1),
    BTOP1("loadScalar", OP_LVT4),
    BTOP0("loadScalarStk"),
    DTOP1("loadArray1", OP_LVT1),
    BTOP1("loadArray", OP_LVT4),
    BTOP0("loadArrayStk"),
    BTOP0("loadStk"),
    DTOP1("storeScalar1", OP_LVT1),
    BTOP1("storeScalar", OP_LVT4),
    BTOP0("storeScalarStk"),
    DTOP1("storeArray1", OP_LVT1),
    BTOP1("storeArray", OP_LVT4),
    BTOP0("storeArrayStk"),
    BTOP0("storeStk"),
    /* 24..34 */
    DTOP1("incrScalar1", OP_LVT1),
    BTOP0("incrScalarStk"),
    DTOP1("incrArray1", OP_LVT1),
    BTOP0("incrArrayStk"),
    BTOP0("incrStk"),
    DTOP2("incrScalar1Imm", OP_LVT1, OP_INT1),
    BTOP1("incrScalarStkImm", OP_INT1),
    DTOP2("incrArray1Imm", OP_LVT1, OP_INT1),
    BTOP1("incrArrayStkImm", OP_INT1),
    BTOP1("incrStkImm", OP_INT1),
    /* 35..40 */
    DTOP1("jump1", OP_OFF1),
    BTOP1("jump", OP_OFF4),
    DTOP1("jumpTrue1", OP_OFF1),
    BTOP1("jumpTrue", OP_OFF4),
    DTOP1("jumpFalse1", OP_OFF1),
    BTOP1("jumpFalse", OP_OFF4),
    /* 41..67 (binary ops, control, catch, results) */
    BTOP0("bitor"),
    BTOP0("bitxor"),
    BTOP0("bitand"),
    BTOP0("eq"),
    BTOP0("neq"),
    BTOP0("lt"),
    BTOP0("gt"),
    BTOP0("le"),
    BTOP0("ge"),
    BTOP0("lshift"),
    BTOP0("rshift"),
    BTOP0("add"),
    BTOP0("sub"),
    BTOP0("mult"),
    BTOP0("div"),
    BTOP0("mod"),
    BTOP0("uplus"),
    BTOP0("uminus"),
    BTOP0("bitnot"),
    BTOP0("not"),
    BTOP0("tryCvtToNumeric"),
    BTOP0("break"),
    BTOP0("continue"),
    BTOP1("beginCatch", OP_UINT4),
    BTOP0("endCatch"),
    BTOP0("pushResult"),
    BTOP0("pushReturnCode"),
    /* 68..76 string/list basics */
    BTOP0("streq"),
    BTOP0("strneq"),
    BTOP0("strcmp"),
    BTOP0("strlen"),
    BTOP0("strindex"),
    BTOP1("strmatch", OP_INT1),
    BTOP1("list", OP_UINT4),
    BTOP0("listIndex"),
    BTOP0("listLength"),
    /* 77..88 append/lappend family */
    DTOP1("appendScalar1", OP_LVT1),
    BTOP1("appendScalar", OP_LVT4),
    DTOP1("appendArray1", OP_LVT1),
    BTOP1("appendArray", OP_LVT4),
    BTOP0("appendArrayStk"),
    BTOP0("appendStk"),
    DTOP1("lappendScalar1", OP_LVT1),
    BTOP1("lappendScalar", OP_LVT4),
    DTOP1("lappendArray1", OP_LVT1),
    BTOP1("lappendArray", OP_LVT4),
    BTOP0("lappendArrayStk"),
    BTOP0("lappendStk"),
    /* 89..94 misc list & return & exponent */
    BTOP1("lindexMulti", OP_UINT4),
    BTOP1("over", OP_UINT4),
    BTOP0("lsetList"),
    BTOP1("lsetFlat", OP_UINT4),
    BTOP2("returnImm", OP_INT4, OP_UINT4),
    BTOP0("expon"),
    /* 95..101 compiled-command framing */
    BTOP1("listIndexImm", OP_IDX4),
    BTOP2("listRangeImm", OP_IDX4, OP_IDX4),
    BTOP2("startCommand", OP_OFF4, OP_UINT4),
    BTOP0("listIn"),
    BTOP0("listNotIn"),
    BTOP0("pushReturnOpts"),
    BTOP0("returnStk"),
    /* 102..111 dict path ops */
    BTOP1("dictGet", OP_UINT4),
    BTOP2("dictSet", OP_UINT4, OP_LVT4),
    BTOP2("dictUnset", OP_UINT4, OP_LVT4),
    BTOP2("dictIncrImm", OP_INT4, OP_LVT4),
    BTOP1("dictAppend", OP_LVT4),
    BTOP1("dictLappend", OP_LVT4),
    BTOP1("dictFirst", OP_LVT4),
    BTOP1("dictNext", OP_LVT4),
    BTOP2("dictUpdateStart", OP_LVT4, OP_AUX4),
    BTOP2("dictUpdateEnd", OP_LVT4, OP_AUX4),
    /* 112..123 switch/upvar/exists/nop */
    BTOP1("jumpTable", OP_AUX4),
    BTOP1("upvar", OP_LVT4),
    BTOP1("nsupvar", OP_LVT4),
    BTOP1("variable", OP_LVT4),
    BTOP2("syntax", OP_INT4, OP_UINT4),
    BTOP1("reverse", OP_UINT4),
    BTOP1("regexp", OP_INT1),
    BTOP1("existScalar", OP_LVT4),
    BTOP1("existArray", OP_LVT4),
    BTOP0("existArrayStk"),
    BTOP0("existStk"),
    BTOP0("nop"),
    /* 124..132 unset group and dict-with helpers */
    DTOP0("returnCodeBranch1"),
    BTOP2("unsetScalar", OP_UNSF1, OP_LVT4),
    BTOP2("unsetArray", OP_UNSF1, OP_LVT4),
    BTOP1("unsetArrayStk", OP_UNSF1),
    BTOP1("unsetStk", OP_UNSF1),
    BTOP0("dictExpand"),
    BTOP0("dictRecombineStk"),
    BTOP1("dictRecombineImm", OP_LVT4),
    /* 133..143 info/ns/oo/array, then invokeReplace */
    BTOP0("currentNamespace"),
    BTOP0("infoLevelNumber"),
    BTOP0("infoLevelArgs"),
    BTOP0("resolveCmd"),
    BTOP0("tclooSelf"),
    BTOP0("tclooClass"),
    BTOP0("tclooNamespace"),
    BTOP0("tclooIsObject"),
    BTOP0("arrayExistsStk"),
    BTOP1("arrayExistsImm", OP_LVT4),
    BTOP0("arrayMakeStk"),
    BTOP1("arrayMakeImm", OP_LVT4),
    BTOP2("invokeReplace", OP_UINT4, OP_UINT1),
    /* 145..154 list ops, foreach, string trim */
    BTOP0("listConcat"),
    BTOP0("expandDrop"),
    BTOP1("foreach_start", OP_AUX4),
    BTOP0("foreach_step"),
    BTOP0("foreach_end"),
    BTOP0("lmap_collect"),
    BTOP0("strtrim"),
    BTOP0("strtrimLeft"),
    BTOP0("strtrimRight"),
    BTOP1("concatStk", OP_UINT4),
    /* 155..161 cases and origin */
    BTOP0("strcaseUpper"),
    BTOP0("strcaseLower"),
    BTOP0("strcaseTitle"),
    BTOP0("strreplace"),
    BTOP0("originCmd"),
    DTOP1("tclooNext", OP_UINT1),
    DTOP1("tclooNextClass", OP_UINT1),
    /* 162..171 coroutine/numeric/string class + lappendList & clock/dictGetDef */
    BTOP0("yieldToInvoke"),
    BTOP0("numericType"),
    BTOP0("tryCvtToBoolean"),
    BTOP1("strclass", OP_SCLS1),
    BTOP1("lappendList", OP_LVT4),
    BTOP1("lappendListArray", OP_LVT4),
    BTOP0("lappendListArrayStk"),
    BTOP0("lappendListStk"),
    BTOP1("clockRead", OP_CLK1),
    BTOP1("dictGetDef", OP_UINT4),
    /* 172..178 TIP 461 strings, lreplace, const */
    BTOP0("strlt"),
    BTOP0("strgt"),
    BTOP0("strle"),
    BTOP0("strge"),
    BTOP2("lreplace", OP_UINT4, OP_LRPL1),
    BTOP1("constImm", OP_LVT4),
    BTOP0("constStk"),
    /* 179..185 Updated 9.1 incr/tail/oo-next */
    BTOP1("incrScalar", OP_LVT4),
    BTOP1("incrArray", OP_LVT4),
    BTOP2("incrScalarImm", OP_LVT4, OP_INT1),
    BTOP2("incrArrayImm", OP_LVT4, OP_INT1),
    BTOP1("tailcall", OP_UINT4),
    BTOP1("tclooNext", OP_UINT4),
    BTOP1("tclooNextClass", OP_UINT4),
    /* 186..197 Really new 9.1 ops */
    BTOP0("swap"),
    BTOP0("errorPrefixEq"),
    BTOP0("tclooId"),
    BTOP0("dictPut"),
    BTOP0("dictRemove"),
    BTOP0("isEmpty"),
    BTOP1("jumpTableNum", OP_AUX4),
    BTOP0("tailcallList"),
    BTOP0("tclooNextList"),
    BTOP0("tclooNextClassList"),
    BTOP1("arithSeries", OP_UINT1),
    BTOP0("uplevel"),
};

/* ==========================================================================
 * Stuff
 * ========================================================================== */

static int R_Bytes(Reader *r, unsigned char *buf, Tcl_Size n, Tcl_Obj *err) {
    if (r->limit >= 0 && (int64_t)n > r->limit) {
        Tcl_AppendPrintfToObj(err, "truncated file (want %" TCL_SIZE_MODIFIER "d more bytes)", n);
        return TCL_ERROR;
    }
    Tcl_Size got = 0;
    while (got < n) {
        Tcl_Size here = Tcl_ReadRaw(r->ch, (char *)buf + got, n - got);
        if (here <= 0) {
            Tcl_AppendToObj(err, "unexpected EOF", -1);
            return TCL_ERROR;
        }
        got += here;
    }
    if (r->limit >= 0)
        r->limit -= n;
    return TCL_OK;
}

static int R_U8(Reader *r, unsigned char *v, Tcl_Obj *err) {
    return R_Bytes(r, v, 1, err);
}

static int R_U32(Reader *r, uint32_t *v, Tcl_Obj *err) {
    (void)r;
    uint32_t tmp = 0;
    if (R_Bytes(r, (unsigned char *)&tmp, 4, err) != TCL_OK)
        return TCL_ERROR;
    if (!tbcxHostIsLE) {
        tmp = ((tmp & 0xFF) << 24) | ((tmp & 0xFF00) << 8) | ((tmp >> 8) & 0xFF00) | (tmp >> 24);
    }
    *v = tmp;
    return TCL_OK;
}

static int R_U64(Reader *r, uint64_t *v, Tcl_Obj *err) {
    (void)r;
    uint64_t tmp = 0;
    if (R_Bytes(r, (unsigned char *)&tmp, 8, err) != TCL_OK)
        return TCL_ERROR;
    if (!tbcxHostIsLE) {
        tmp = ((tmp & 0x00000000000000FFull) << 56) | ((tmp & 0x000000000000FF00ull) << 40) | ((tmp & 0x0000000000FF0000ull) << 24) | ((tmp & 0x00000000FF000000ull) << 8) |
              ((tmp & 0x000000FF00000000ull) >> 8) | ((tmp & 0x0000FF0000000000ull) >> 24) | ((tmp & 0x00FF000000000000ull) >> 40) | ((tmp & 0xFF00000000000000ull) >> 56);
    }
    *v = tmp;
    return TCL_OK;
}

static int R_LPString(Reader *r, unsigned char **bufOut, uint32_t *lenOut, Tcl_Obj *err) {
    uint32_t n = 0;
    if (R_U32(r, &n, err) != TCL_OK)
        return TCL_ERROR;
    if (n > TBCX_MAX_STR) {
        Tcl_AppendToObj(err, "LPString too large", -1);
        return TCL_ERROR;
    }
    unsigned char *buf = (unsigned char *)Tcl_Alloc(n ? n : 1);
    if (n && R_Bytes(r, buf, n, err) != TCL_OK) {
        Tcl_Free(buf);
        return TCL_ERROR;
    }
    *bufOut = buf;
    *lenOut = n;
    return TCL_OK;
}

static void AppendQuoted(Tcl_Obj *out, const unsigned char *s, Tcl_Size n) {
    Tcl_AppendToObj(out, "\"", 1);
    for (Tcl_Size i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '\\' || c == '\"') {
            Tcl_AppendPrintfToObj(out, "\\%c", c);
        } else if (c >= 0x20 && c < 0x7f) {
            Tcl_AppendPrintfToObj(out, "%c", c);
        } else {
            Tcl_AppendPrintfToObj(out, "\\x%02x", c);
        }
    }
    Tcl_AppendToObj(out, "\"", 1);
}

static void FreeLitPreviewArray(LitPreview *arr, uint32_t n) {
    if (!arr)
        return;
    for (uint32_t i = 0; i < n; i++) {
        if (arr[i].preview)
            Tcl_DecrRefCount(arr[i].preview);
    }
    Tcl_Free(arr);
}

static Tcl_Obj *MakeHexBytes(const unsigned char *p, Tcl_Size n, Tcl_Size maxShow) {
    Tcl_Obj *o = Tcl_NewObj();
    Tcl_IncrRefCount(o);
    Tcl_AppendToObj(o, "0x", 2);
    Tcl_Size shown = n < maxShow ? n : maxShow;
    for (Tcl_Size i = 0; i < shown; i++) {
        Tcl_AppendPrintfToObj(o, "%02x", p[i]);
    }
    if (shown < n)
        Tcl_AppendToObj(o, "...", 3);
    return o;
}

static Tcl_Obj *PreviewFromStringBytes(const unsigned char *s, uint32_t n) {
    Tcl_Obj *o = Tcl_NewObj();
    Tcl_IncrRefCount(o);
    AppendQuoted(o, s, n);
    return o;
}

static int ReadOneLiteral(Reader *r, LitPreview *dst, Tcl_Obj *out, uint32_t idx, Tcl_Obj *err) {
    uint32_t tag32 = 0;
    if (R_U32(r, &tag32, err) != TCL_OK)
        return TCL_ERROR;
    dst->tag     = (TbcxLitTag)tag32;
    dst->preview = NULL;

    switch (dst->tag) {
    case LIT_BOOLEAN: {
        unsigned char b;
        if (R_U8(r, &b, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewStringObj(b ? "true" : "false", -1);
        Tcl_IncrRefCount(pv);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BOOLEAN %s\n", idx, b ? "true" : "false");
        break;
    }
    case LIT_STRING: {
        uint32_t       n;
        unsigned char *buf = NULL;
        if (R_LPString(r, &buf, &n, err) != TCL_OK)
            return TCL_ERROR;
        /* Save a pretty printable form for disassembly; also print the literal block */
        Tcl_Obj *pv  = PreviewFromStringBytes(buf, n);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] STRING len=%u value=", idx, n);
        AppendQuoted(out, buf, n);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_Free(buf);
        break;
    }
    case LIT_WIDEINT: {
        uint64_t u;
        if (R_U64(r, &u, err) != TCL_OK)
            return TCL_ERROR;
        int64_t  v  = (int64_t)u;
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%" PRId64, v);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] WIDEINT %" PRId64 "\n", idx, v);
        break;
    }
    case LIT_WIDEUINT: {
        uint64_t v;
        if (R_U64(r, &v, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%" PRIu64, v);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] WIDEUINT %" PRIu64 "\n", idx, v);
        break;
    }
    case LIT_DOUBLE: {
        /* Stored as 8 bytes; we only store a textual snapshot */
        uint64_t bits;
        if (R_U64(r, &bits, err) != TCL_OK)
            return TCL_ERROR;
        /* Avoid aliasing UB: memcpy into a double */
        double d;
        memcpy(&d, &bits, 8);
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%.17g", d);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] DOUBLE %s\n", idx, Tcl_GetString(pv));
        break;
    }
    case LIT_BYTEARR: {
        uint32_t       n;
        unsigned char *buf = NULL;
        if (R_LPString(r, &buf, &n, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv  = MakeHexBytes(buf, n, 16);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BYTEARRAY len=%u data=%s\n", idx, n, Tcl_GetString(pv));
        Tcl_Free(buf);
        break;
    }
    case LIT_BIGNUM: {
        /* Stored as sign + byte count + little-endian magnitude */
        unsigned char sign;
        uint32_t      n;
        if (R_U8(r, &sign, err) != TCL_OK)
            return TCL_ERROR;
        if (R_U32(r, &n, err) != TCL_OK)
            return TCL_ERROR;
        unsigned char *buf = Tcl_Alloc(n);
        if (R_Bytes(r, buf, n, err) != TCL_OK) {
            Tcl_Free(buf);
            return TCL_ERROR;
        }
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%s", sign == 2 ? "-" : "+");
        Tcl_AppendObjToObj(pv, MakeHexBytes(buf, n, 16));
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BIGNUM %s (len=%u)\n", idx, Tcl_GetString(pv), n);
        Tcl_Free(buf);
        break;
    }
    case LIT_DICT: {
        uint32_t nPairs;
        if (R_U32(r, &nPairs, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "<dict %u pairs>", nPairs);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] DICT %u pairs\n", idx, nPairs);
        /* Each key/value is stored as a nested literal blob */
        for (uint32_t i = 0; i < nPairs; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview)
                    Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview)
                Tcl_DecrRefCount(tmp.preview);
        }
        break;
    }
    case LIT_LIST: {
        uint32_t nEls;
        if (R_U32(r, &nEls, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "<list %u>", nEls);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] LIST %u elements\n", idx, nEls);
        for (uint32_t i = 0; i < nEls; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview)
                    Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview)
                Tcl_DecrRefCount(tmp.preview);
        }
        break;
    }
    case LIT_LAMBDA_BC: {
        /* nsFQN (LPString) */
        unsigned char *ns    = NULL;
        uint32_t       nsLen = 0;
        if (R_LPString(r, &ns, &nsLen, err) != TCL_OK)
            return TCL_ERROR;
        /* numArgs + args (each: LPString name, u8 hasDefault, then maybe default literal) */
        uint32_t nArgs = 0;
        if (R_U32(r, &nArgs, err) != TCL_OK) {
            Tcl_Free(ns);
            return TCL_ERROR;
        }
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendToObj(pv, "<lambda … in ", -1);
        AppendQuoted(pv, ns, nsLen);
        Tcl_AppendToObj(pv, ">", -1);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] LAMBDA ns=", idx);
        AppendQuoted(out, ns, nsLen);
        Tcl_AppendPrintfToObj(out, " args=%u\n", nArgs);
        Tcl_Free(ns);
        for (uint32_t i = 0; i < nArgs; i++) {
            unsigned char *name    = NULL;
            uint32_t       nameLen = 0;
            unsigned char  hasDef  = 0;
            if (R_LPString(r, &name, &nameLen, err) != TCL_OK)
                return TCL_ERROR;
            if (R_U8(r, &hasDef, err) != TCL_OK) {
                Tcl_Free(name);
                return TCL_ERROR;
            }
            Tcl_AppendToObj(out, "    arg ", -1);
            AppendQuoted(out, name, nameLen);
            Tcl_Free(name);
            if (hasDef) {
                Tcl_AppendToObj(out, " default=", -1);
                LitPreview tmp = {0, NULL};
                if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                    if (tmp.preview)
                        Tcl_DecrRefCount(tmp.preview);
                    return TCL_ERROR;
                }
                if (tmp.preview)
                    Tcl_DecrRefCount(tmp.preview);
            }
            Tcl_AppendToObj(out, "\n", 1);
        }
        /* Nested compiled block */
        if (DumpCompiledBlock(r, out, err) != TCL_OK)
            return TCL_ERROR;
        break;
    }
    case LIT_BYTECODE: {
        /* nsFQN (LPString), then nested compiled block */
        unsigned char *nm      = NULL;
        uint32_t       nameLen = 0;
        if (R_LPString(r, &nm, &nameLen, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj();
        Tcl_IncrRefCount(pv);
        Tcl_AppendToObj(pv, "<bytecode ns=", -1);
        AppendQuoted(pv, nm, nameLen);
        Tcl_AppendToObj(pv, ">", -1);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BYTECODE ns=", idx);
        AppendQuoted(out, nm, nameLen);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_Free(nm);
        if (DumpCompiledBlock(r, out, err) != TCL_OK)
            return TCL_ERROR;
        break;
    }
    default:
        Tcl_AppendPrintfToObj(err, "unknown literal tag %u", (unsigned)dst->tag);
        return TCL_ERROR;
    }
    return TCL_OK;
}

static inline unsigned OpKindSize(OpKind k) {
    switch (k) {
    case OP_INT1:
    case OP_UINT1:
    case OP_OFF1:
    case OP_LVT1:
    case OP_LIT1:
    case OP_SCLS1:
    case OP_UNSF1:
    case OP_CLK1:
    case OP_LRPL1:
        return 1u;
    case OP_INT4:
    case OP_UINT4:
    case OP_IDX4:
    case OP_LVT4:
    case OP_AUX4:
    case OP_OFF4:
    case OP_LIT4:
        return 4u;
    case OP_NONE:
    default:
        return 0u;
    }
}

/* Reads a 4-byte little-endian (Tcl's internal byte order is defined) */
static inline uint32_t U32LE(const unsigned char *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t I32LE(const unsigned char *p) {
    return (int32_t)U32LE(p);
}

static void PrintOperand(Tcl_Obj *out, OpKind k, const unsigned char *p, Tcl_Size pc, const LitPreview *lits, uint32_t nLits) {
    switch (k) {
    case OP_INT1:
        Tcl_AppendPrintfToObj(out, " %d", (int8_t)p[0]);
        break;
    case OP_INT4:
        Tcl_AppendPrintfToObj(out, " %d", I32LE(p));
        break;
    case OP_UINT1:
        Tcl_AppendPrintfToObj(out, " %u", (unsigned)p[0]);
        break;
    case OP_UINT4:
        Tcl_AppendPrintfToObj(out, " %u", (unsigned)U32LE(p));
        break;
    case OP_IDX4:
        Tcl_AppendPrintfToObj(out, " %d", I32LE(p));
        break;
    case OP_LVT1:
        Tcl_AppendPrintfToObj(out, " lvt[%u]", (unsigned)p[0]);
        break;
    case OP_LVT4:
        Tcl_AppendPrintfToObj(out, " lvt[%u]", (unsigned)U32LE(p));
        break;
    case OP_AUX4:
        Tcl_AppendPrintfToObj(out, " aux[%u]", (unsigned)U32LE(p));
        break;
    case OP_OFF1:
        Tcl_AppendPrintfToObj(out, " ->%+" PRId64, (int64_t)(int8_t)p[0] + (int64_t)pc);
        break;
    case OP_OFF4:
        Tcl_AppendPrintfToObj(out, " ->%+" PRId64, (int64_t)I32LE(p) + (int64_t)pc);
        break;
    case OP_LIT1: {
        unsigned idx = p[0];
        if (idx < nLits && lits[idx].preview) {
            Tcl_AppendPrintfToObj(out, " lit[%u]=", idx);
            Tcl_AppendToObj(out, Tcl_GetString(lits[idx].preview), -1);
        } else {
            Tcl_AppendPrintfToObj(out, " lit[%u]", idx);
        }
        break;
    }
    case OP_LIT4: {
        unsigned idx = U32LE(p);
        if (idx < nLits && lits[idx].preview) {
            Tcl_AppendPrintfToObj(out, " lit[%u]=", idx);
            Tcl_AppendToObj(out, Tcl_GetString(lits[idx].preview), -1);
        } else {
            Tcl_AppendPrintfToObj(out, " lit[%u]", idx);
        }
        break;
    }
    case OP_SCLS1:
        Tcl_AppendPrintfToObj(out, " strClass#%u", (unsigned)p[0]);
        break;
    case OP_UNSF1:
        Tcl_AppendPrintfToObj(out, " unsetFlags=0x%02x", (unsigned)p[0]);
        break;
    case OP_CLK1:
        Tcl_AppendPrintfToObj(out, " clock#%u", (unsigned)p[0]);
        break;
    case OP_LRPL1:
        Tcl_AppendPrintfToObj(out, " lreplaceFlags=0x%02x", (unsigned)p[0]);
        break;
    case OP_NONE:
    default:
        break;
    }
}

static int DisassembleCode(Tcl_Obj *out, const unsigned char *code, uint32_t codeLen, const LitPreview *lits, uint32_t nLits, Tcl_Obj *err) {
    Tcl_AppendToObj(out, "  code-disassembly:\n", -1);
    Tcl_Size pc = 0;
    while ((uint32_t)pc < codeLen) {
        unsigned op = code[pc];
        if (op >= sizeof(opTable) / sizeof(opTable[0])) {
            Tcl_AppendPrintfToObj(err, "unknown opcode %u at pc=%" TCL_SIZE_MODIFIER "d", op, pc);
            return TCL_ERROR;
        }
        const OpInfo *d = &opTable[op];
        if (d->deprecated) {
            Tcl_AppendPrintfToObj(err, "deprecated opcode \"%s\" (op=%u) at pc=%" TCL_SIZE_MODIFIER "d — 9.1-only disassembler refuses to decode", d->name, op, pc);
            return TCL_ERROR;
        }

        Tcl_AppendPrintfToObj(out, "    %6" TCL_SIZE_MODIFIER "d: %-18s", pc, d->name);

        /* Gather and print operands */
        const unsigned char *p     = code + pc + 1;
        Tcl_Size             bytes = 1;
        for (unsigned i = 0; i < d->nops; i++) {
            PrintOperand(out, d->op[i], p, pc, lits, nLits);
            unsigned w = OpKindSize(d->op[i]);
            p += w;
            bytes += w;
        }
        Tcl_AppendToObj(out, "\n", 1);
        pc += bytes;
    }
    return TCL_OK;
}

static int DumpOneAux(Reader *r, Tcl_Obj *out, Tcl_Obj *err, uint32_t idx) {
    uint32_t tag;
    if (R_U32(r, &tag, err) != TCL_OK)
        return TCL_ERROR;
    switch (tag) {
    case TBCX_AUX_JT_STR: {
        uint32_t cnt = 0;
        if (R_U32(r, &cnt, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_AppendPrintfToObj(out, "  [aux %u] jumptable(str) entries=%u\n", idx, cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            unsigned char *key = NULL;
            uint32_t       kL  = 0;
            uint32_t       off = 0;
            if (R_LPString(r, &key, &kL, err) != TCL_OK)
                return TCL_ERROR;
            if (R_U32(r, &off, err) != TCL_OK) {
                Tcl_Free(key);
                return TCL_ERROR;
            }
            Tcl_AppendToObj(out, "    key=", -1);
            AppendQuoted(out, key, kL);
            Tcl_AppendPrintfToObj(out, " -> %u\n", off);
            Tcl_Free(key);
        }
        break;
    }
    case TBCX_AUX_JT_NUM: {
        uint32_t cnt = 0;
        if (R_U32(r, &cnt, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_AppendPrintfToObj(out, "  [aux %u] jumptable(num) entries=%u\n", idx, cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            uint64_t key = 0;
            uint32_t off = 0;
            if (R_U64(r, &key, err) != TCL_OK)
                return TCL_ERROR;
            if (R_U32(r, &off, err) != TCL_OK)
                return TCL_ERROR;
            Tcl_AppendPrintfToObj(out, "    key=%" PRIu64 " -> %u\n", key, off);
        }
        break;
    }
    case TBCX_AUX_DICTUPD: {
        uint32_t L = 0;
        if (R_U32(r, &L, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_AppendPrintfToObj(out, "  [aux %u] dictUpdate len=%u indices:", idx, L);
        for (uint32_t i = 0; i < L; i++) {
            uint32_t v = 0;
            if (R_U32(r, &v, err) != TCL_OK)
                return TCL_ERROR;
            Tcl_AppendPrintfToObj(out, " %u", v);
        }
        Tcl_AppendToObj(out, "\n", 1);
        break;
    }
    case TBCX_AUX_FOREACH:
    case TBCX_AUX_NEWFORE: {
        uint32_t numLists = 0, loopCt = 0, firstVal = 0, dupNum = 0;
        if (R_U32(r, &numLists, err) != TCL_OK)
            return TCL_ERROR;
        if (R_U32(r, &loopCt, err) != TCL_OK)
            return TCL_ERROR;
        if (R_U32(r, &firstVal, err) != TCL_OK)
            return TCL_ERROR;
        if (R_U32(r, &dupNum, err) != TCL_OK)
            return TCL_ERROR;
        Tcl_AppendPrintfToObj(out, "  [aux %u] %s numLists=%u (dup=%u) firstValueTemp=%u loopCtTemp=%u\n", idx, (tag == TBCX_AUX_FOREACH ? "foreach" : "newForeach"), numLists, dupNum, firstVal,
                              loopCt);
        for (uint32_t iL = 0; iL < numLists; iL++) {
            uint32_t nv = 0;
            if (R_U32(r, &nv, err) != TCL_OK)
                return TCL_ERROR;
            Tcl_AppendPrintfToObj(out, "    list[%u] vars=%u:", iL, nv);
            for (uint32_t j = 0; j < nv; j++) {
                uint32_t idxv = 0;
                if (R_U32(r, &idxv, err) != TCL_OK)
                    return TCL_ERROR;
                Tcl_AppendPrintfToObj(out, " %u", idxv);
            }
            Tcl_AppendToObj(out, "\n", 1);
        }
        break;
    }
    default:
        Tcl_AppendPrintfToObj(err, "unsupported AuxData tag %u", tag);
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int DumpCompiledBlock(Reader *r, Tcl_Obj *out, Tcl_Obj *err) {
    uint32_t codeLen;
    if (R_U32(r, &codeLen, err) != TCL_OK)
        return TCL_ERROR;
    /* Be robust when codeLen==0 (mirror loader's alloc-at-least-1 policy) */
    unsigned char *code = Tcl_Alloc(codeLen ? codeLen : 1u);
    if (codeLen && R_Bytes(r, code, codeLen, err) != TCL_OK) {
        Tcl_Free(code);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "  codeLen=%u\n", codeLen);

    /* Literals */
    uint32_t nLits;
    if (R_U32(r, &nLits, err) != TCL_OK) {
        Tcl_Free(code);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "  literals=%u\n", nLits);
    LitPreview *lits = (nLits ? (LitPreview *)Tcl_Alloc(sizeof(LitPreview) * nLits) : NULL);
    for (uint32_t i = 0; i < nLits; i++) {
        lits[i].tag     = 0;
        lits[i].preview = NULL;
        if (ReadOneLiteral(r, &lits[i], out, i, err) != TCL_OK) {
            FreeLitPreviewArray(lits, nLits);
            Tcl_Free(code);
            return TCL_ERROR;
        }
    }

    /* Aux data */
    uint32_t nAux;
    if (R_U32(r, &nAux, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "  auxData=%u\n", nAux);
    for (uint32_t i = 0; i < nAux; i++) {
        if (DumpOneAux(r, out, err, i) != TCL_OK) {
            FreeLitPreviewArray(lits, nLits);
            Tcl_Free(code);
            return TCL_ERROR;
        }
    }

    /* Disassemble the buffered code (AFTER collecting literal previews). */
    if (DisassembleCode(out, code, codeLen, lits, nLits, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }

    /* Exception ranges */
    uint32_t nExc;
    if (R_U32(r, &nExc, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "  exceptions=%u\n", nExc);
    for (uint32_t i = 0; i < nExc; i++) {
        uint32_t type, nest, codeOfs, codeBytes, cont, brk, catc;
        if (R_U32(r, &type, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &nest, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &codeOfs, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &codeBytes, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &cont, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &brk, err) != TCL_OK)
            goto exc_fail;
        if (R_U32(r, &catc, err) != TCL_OK)
            goto exc_fail;
        Tcl_AppendPrintfToObj(out, "    [%u] type=%u nest=%u code=[%u..%u) break=%u continue=%u catch=%u\n", i, type, nest, codeOfs, codeOfs + codeBytes, brk, cont, catc);
        continue;
    exc_fail:
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }

    /* Epilogue: 3×u32 (maxStack, reserved=0, numLocals) */
    uint32_t maxStack = 0, reserved = 0, numLocals = 0;
    if (R_U32(r, &maxStack, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }
    if (R_U32(r, &reserved, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }
    if (R_U32(r, &numLocals, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        Tcl_Free(code);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "  epilogue: maxStack=%u reserved=%u numLocals=%u\n", maxStack, reserved, numLocals);

    FreeLitPreviewArray(lits, nLits);
    Tcl_Free(code);
    return TCL_OK;
}

static int DumpHeader(Reader *r, Tcl_Obj *out, Tcl_Obj *err) {
    TbcxHeader H;
    if (R_U32(r, &H.magic, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.format, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.tcl_version, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U64(r, &H.codeLenTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.numCmdsTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.numExceptTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.numLitsTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.numAuxTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.numLocalsTop, err) != TCL_OK)
        return TCL_ERROR;
    if (R_U32(r, &H.maxStackTop, err) != TCL_OK)
        return TCL_ERROR;
    if (H.magic != TBCX_MAGIC || H.format != TBCX_FORMAT) {
        Tcl_AppendToObj(err, "bad tbcx header", -1);
        return TCL_ERROR;
    }
    Tcl_AppendPrintfToObj(out, "tbcx: format=%u tcl=0x%08x codeLenTop=%" PRIu64 " lits=%u aux=%u locals=%u maxStack=%u\n", H.format, H.tcl_version, (unsigned long long)H.codeLenTop, H.numLitsTop,
                          H.numAuxTop, H.numLocalsTop, H.maxStackTop);
    return TCL_OK;
}

static int DumpOneMethod(Reader *r, Tcl_Obj *out, Tcl_Obj *err) {
    /* classFqn */
    unsigned char *cls  = NULL;
    uint32_t       clsL = 0;
    if (R_LPString(r, &cls, &clsL, err) != TCL_OK)
        return TCL_ERROR;
    /* kind */
    unsigned char kind = 0;
    if (R_U8(r, &kind, err) != TCL_OK) {
        Tcl_Free(cls);
        return TCL_ERROR;
    }
    /* name (LPString; may be empty for ctor/dtor) */
    unsigned char *name  = NULL;
    uint32_t       nameL = 0;
    if (R_LPString(r, &name, &nameL, err) != TCL_OK) {
        Tcl_Free(cls);
        return TCL_ERROR;
    }
    /* args (LPString) */
    unsigned char *args  = NULL;
    uint32_t       argsL = 0;
    if (R_LPString(r, &args, &argsL, err) != TCL_OK) {
        Tcl_Free(cls);
        Tcl_Free(name);
        return TCL_ERROR;
    }
    /* bodyTextLen (ignored) */
    uint32_t bodyTextLen = 0;
    if (R_U32(r, &bodyTextLen, err) != TCL_OK) {
        Tcl_Free(cls);
        Tcl_Free(name);
        Tcl_Free(args);
        return TCL_ERROR;
    }
    Tcl_AppendToObj(out, "method class=", -1);
    AppendQuoted(out, cls, clsL);
    Tcl_AppendPrintfToObj(out, " kind=%u name=", kind);
    AppendQuoted(out, name, nameL);
    Tcl_AppendToObj(out, " args=", -1);
    AppendQuoted(out, args, argsL);
    Tcl_AppendToObj(out, "\n", 1);
    Tcl_Free(cls);
    Tcl_Free(name);
    Tcl_Free(args);
    return DumpCompiledBlock(r, out, err);
}

static int DumpOneClass(Reader *r, Tcl_Obj *out, Tcl_Obj *err) {
    /* Saver currently writes only classFqn + nSupers (0) */
    unsigned char *name    = NULL;
    uint32_t       nameLen = 0;
    if (R_LPString(r, &name, &nameLen, err) != TCL_OK)
        return TCL_ERROR;
    uint32_t nSupers = 0;
    if (R_U32(r, &nSupers, err) != TCL_OK) {
        Tcl_Free(name);
        return TCL_ERROR;
    }
    Tcl_AppendToObj(out, "class ", -1);
    AppendQuoted(out, name, nameLen);
    Tcl_AppendPrintfToObj(out, " supers=%u", nSupers);
    for (uint32_t i = 0; i < nSupers; i++) {
        unsigned char *sup = NULL;
        uint32_t       sL  = 0;
        if (R_LPString(r, &sup, &sL, err) != TCL_OK) {
            Tcl_Free(name);
            return TCL_ERROR;
        }
        Tcl_AppendToObj(out, " ", -1);
        AppendQuoted(out, sup, sL);
        Tcl_Free(sup);
    }
    Tcl_AppendToObj(out, "\n", 1);
    Tcl_Free(name);
    return TCL_OK;
}

static int DumpFile(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj) {
    Reader R     = {0};
    R.ch         = ch;
    R.limit      = -1;
    Tcl_Obj *out = Tcl_NewObj();
    Tcl_Obj *err = Tcl_NewObj();
    Tcl_IncrRefCount(out);
    Tcl_IncrRefCount(err);

    if (CheckBinaryChan(interp, ch) != TCL_OK)
        goto fail;
    if (DumpHeader(&R, out, err) != TCL_OK)
        goto fail;

    Tcl_AppendToObj(out, "top:\n", -1);
    if (DumpCompiledBlock(&R, out, err) != TCL_OK)
        goto fail;

    /* Procs section */
    uint32_t nProcs = 0;
    if (R_U32(&R, &nProcs, err) != TCL_OK)
        goto fail;
    Tcl_AppendPrintfToObj(out, "procs=%u\n", nProcs);
    for (uint32_t i = 0; i < nProcs; i++) {
        unsigned char *name = NULL, *ns = NULL, *args = NULL;
        uint32_t       nL = 0, sL = 0, aL = 0;
        if (R_LPString(&R, &name, &nL, err) != TCL_OK)
            goto fail;
        if (R_LPString(&R, &ns, &sL, err) != TCL_OK) {
            Tcl_Free(name);
            goto fail;
        }
        if (R_LPString(&R, &args, &aL, err) != TCL_OK) {
            Tcl_Free(name);
            Tcl_Free(ns);
            goto fail;
        }
        Tcl_AppendToObj(out, " proc ", -1);
        AppendQuoted(out, name, nL);
        Tcl_AppendToObj(out, " ns=", -1);
        AppendQuoted(out, ns, sL);
        Tcl_AppendToObj(out, " args=", -1);
        AppendQuoted(out, args, aL);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_Free(name);
        Tcl_Free(ns);
        Tcl_Free(args);
        if (DumpCompiledBlock(&R, out, err) != TCL_OK)
            goto fail;
    }

    /* Classes */
    uint32_t nClasses = 0;
    if (R_U32(&R, &nClasses, err) != TCL_OK)
        goto fail;
    Tcl_AppendPrintfToObj(out, "classes=%u\n", nClasses);

    for (uint32_t i = 0; i < nClasses; i++) {
        if (DumpOneClass(&R, out, err) != TCL_OK)
            goto fail;
    }
    /* Methods */
    uint32_t nMethods = 0;
    if (R_U32(&R, &nMethods, err) != TCL_OK)
        goto fail;
    Tcl_AppendPrintfToObj(out, "methods=%u\n", nMethods);
    for (uint32_t m = 0; m < nMethods; m++) {
        if (DumpOneMethod(&R, out, err) != TCL_OK)
            goto fail;
    }

    *outObj = out;
    Tcl_DecrRefCount(err);
    return TCL_OK;

fail:
    Tcl_DecrRefCount(out);
    Tcl_SetObjResult(interp, err);
    return TCL_ERROR;
}

/* ==========================================================================
 * Tcl command
 * ========================================================================== */

int Tbcx_DumpFileObjCmd(ClientData cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        return TCL_ERROR;
    }

    Tcl_Channel ch = Tcl_FSOpenFileChannel(interp, objv[1], "r", 0);
    if (ch == NULL)
        return TCL_ERROR;

    if (CheckBinaryChan(interp, ch) != TCL_OK) {
        Tcl_Close(NULL, ch);
        return TCL_ERROR;
    }

    Tcl_Obj *out = NULL;
    int      rc  = DumpFile(interp, ch, &out);

    if (rc == TCL_OK) {
        if (Tcl_Close(interp, ch) != TCL_OK) {
            if (out)
                Tcl_DecrRefCount(out);
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, out);
        return TCL_OK;
    } else {
        Tcl_Close(NULL, ch);
        return TCL_ERROR;
    }
}
