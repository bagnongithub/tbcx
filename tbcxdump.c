/*
 * tbcxdump.c — Disassemble and print bytecode saved in .tbcx files
 *
 * This version expands the original dumper to decode Tcl 9.1 bytecode,
 * resolve literal indices to strings, and print a readable listing.
 *
 * IMPORTANT POLICY:
 *  - Supports ONLY Tcl 9.1 "allowed" bytecodes.
 *  - If a deprecated opcode (the legacy 1‑byte operand forms and a few
 *    others) is encountered, we stop with an error; we do NOT decode them.
 *
 * The opcode names and operand kinds are copied from the Tcl 9.1 sources'
 * instruction description table (tclCompile.c / tclCompile.h).
 *
 * Build: drop-in replacement for the original 0f5c4.tbcxdump.c
 */

#include "tcl.h"
#include "tbcx.h"
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* ===== Helpers copied from the previous dumper (lightly augmented) ===== */

typedef struct {
    Tcl_Channel ch;
    int64_t     limit;   /* remaining bytes available; -1 means unlimited */
    int         le;      /* non-zero if file payload is little-endian */
} Reader;

/* Read exactly n bytes (or fail) into buf. Decrements r->limit when >=0. */
static int
R_Bytes(Reader *r, unsigned char *buf, Tcl_Size n, Tcl_Obj *err)
{
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
    if (r->limit >= 0) r->limit -= n;
    return TCL_OK;
}

static int
R_U8(Reader *r, unsigned char *v, Tcl_Obj *err)
{
    return R_Bytes(r, v, 1, err);
}

static int
R_U32(Reader *r, uint32_t *v, Tcl_Obj *err)
{
    unsigned char b[4];
    if (R_Bytes(r, b, 4, err) != TCL_OK) return TCL_ERROR;
    if (r->le) {
        *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    } else {
        *v = (uint32_t)b[3] | ((uint32_t)b[2] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[0] << 24);
    }
    return TCL_OK;
}

static int
R_I32(Reader *r, int32_t *v, Tcl_Obj *err)
{
    uint32_t u;
    if (R_U32(r, &u, err) != TCL_OK) return TCL_ERROR;
    *v = (int32_t)u;
    return TCL_OK;
}

static void
AppendQuoted(Tcl_Obj *out, const unsigned char *s, Tcl_Size n)
{
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

/* ===== Literal decoding (for annotating disassembly) ===== */

typedef enum {
    LIT_STRING = 1, LIT_WIDEINT = 2, LIT_WIDEUINT = 3, LIT_DOUBLE = 4,
    LIT_BYTEARR = 5, LIT_BIGNUM = 6, LIT_DICT = 7, LIT_LIST = 8,
    LIT_LAMBDA_BC = 9, LIT_BYTECODE = 10
} TbcxLitTag;

/* Small preview string we can embed into disassembly comments */
typedef struct {
    TbcxLitTag tag;
    Tcl_Obj   *preview; /* refcounted Tcl_Obj* with a short printable form */
} LitPreview;

static void
FreeLitPreviewArray(LitPreview *arr, uint32_t n)
{
    if (!arr) return;
    for (uint32_t i = 0; i < n; i++) {
        if (arr[i].preview) Tcl_DecrRefCount(arr[i].preview);
    }
    ckfree(arr);
}

static Tcl_Obj*
MakeHexBytes(const unsigned char *p, Tcl_Size n, Tcl_Size maxShow)
{
    Tcl_Obj *o = Tcl_NewObj(); Tcl_IncrRefCount(o);
    Tcl_AppendToObj(o, "0x", 2);
    Tcl_Size shown = n < maxShow ? n : maxShow;
    for (Tcl_Size i = 0; i < shown; i++) {
        Tcl_AppendPrintfToObj(o, "%02x", p[i]);
    }
    if (shown < n) Tcl_AppendToObj(o, "...", 3);
    return o;
}

static Tcl_Obj*
PreviewFromStringBytes(const unsigned char *s, uint32_t n)
{
    Tcl_Obj *o = Tcl_NewObj(); Tcl_IncrRefCount(o);
    AppendQuoted(o, s, n);
    return o;
}

static int
ReadOneLiteral(Reader *r, LitPreview *dst, Tcl_Obj *out, uint32_t idx, Tcl_Obj *err)
{
    unsigned char tagByte;
    if (R_U8(r, &tagByte, err) != TCL_OK) return TCL_ERROR;
    dst->tag = (TbcxLitTag)tagByte;
    dst->preview = NULL;

    switch (dst->tag) {
    case LIT_STRING: {
        uint32_t n;
        if (R_U32(r, &n, err) != TCL_OK) return TCL_ERROR;
        unsigned char *buf = ckalloc(n);
        if (R_Bytes(r, buf, n, err) != TCL_OK) { ckfree(buf); return TCL_ERROR; }
        /* Save a pretty printable form for disassembly; also print the literal block */
        Tcl_Obj *pv = PreviewFromStringBytes(buf, n);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] STRING len=%u value=", idx, n);
        AppendQuoted(out, buf, n);
        Tcl_AppendToObj(out, "\n", 1);
        ckfree(buf);
        break;
    }
    case LIT_WIDEINT: {
        int32_t lo32, hi32;
        if (R_I32(r, &lo32, err) != TCL_OK) return TCL_ERROR;
        if (R_I32(r, &hi32, err) != TCL_OK) return TCL_ERROR;
        int64_t v = ((int64_t)(uint32_t)lo32) | (((int64_t)hi32) << 32);
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%" PRId64, v);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] WIDEINT %" PRId64 "\n", idx, v);
        break;
    }
    case LIT_WIDEUINT: {
        uint32_t lo, hi;
        if (R_U32(r, &lo, err) != TCL_OK) return TCL_ERROR;
        if (R_U32(r, &hi, err) != TCL_OK) return TCL_ERROR;
        uint64_t v = ((uint64_t)lo) | (((uint64_t)hi) << 32);
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%" PRIu64, v);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] WIDEUINT %" PRIu64 "\n", idx, v);
        break;
    }
    case LIT_DOUBLE: {
        /* Stored as 8 bytes; we only store a textual snapshot */
        unsigned char b[8];
        if (R_Bytes(r, b, 8, err) != TCL_OK) return TCL_ERROR;
        /* Avoid aliasing UB: memcpy into a double */
        double d;
        memcpy(&d, b, 8);
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%.17g", d);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] DOUBLE %s\n",
                              idx, Tcl_GetString(pv));
        break;
    }
    case LIT_BYTEARR: {
        uint32_t n;
        if (R_U32(r, &n, err) != TCL_OK) return TCL_ERROR;
        unsigned char *buf = ckalloc(n);
        if (R_Bytes(r, buf, n, err) != TCL_OK) { ckfree(buf); return TCL_ERROR; }
        Tcl_Obj *pv = MakeHexBytes(buf, n, 16);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BYTEARRAY len=%u data=%s\n",
                              idx, n, Tcl_GetString(pv));
        ckfree(buf);
        break;
    }
    case LIT_BIGNUM: {
        /* Stored as sign + byte count + big-endian magnitude */
        unsigned char sign;
        uint32_t n;
        if (R_U8(r, &sign, err) != TCL_OK) return TCL_ERROR;
        if (R_U32(r, &n, err) != TCL_OK) return TCL_ERROR;
        unsigned char *buf = ckalloc(n);
        if (R_Bytes(r, buf, n, err) != TCL_OK) { ckfree(buf); return TCL_ERROR; }
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "%s", sign ? "-" : "+");
        Tcl_AppendObjToObj(pv, MakeHexBytes(buf, n, 16));
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BIGNUM %s (len=%u)\n",
                              idx, Tcl_GetString(pv), n);
        ckfree(buf);
        break;
    }
    case LIT_DICT: {
        uint32_t nPairs;
        if (R_U32(r, &nPairs, err) != TCL_OK) return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "<dict %u pairs>", nPairs);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] DICT %u pairs\n", idx, nPairs);
        /* Each key/value is stored as a nested literal blob */
        for (uint32_t i = 0; i < nPairs; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
        }
        break;
    }
    case LIT_LIST: {
        uint32_t nEls;
        if (R_U32(r, &nEls, err) != TCL_OK) return TCL_ERROR;
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendPrintfToObj(pv, "<list %u>", nEls);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] LIST %u elements\n", idx, nEls);
        for (uint32_t i = 0; i < nEls; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
        }
        break;
    }
    case LIT_LAMBDA_BC: {
        /* Lambda: argument/namespace strings + a nested bytecode chunk */
        uint32_t nArgs, nNs;
        if (R_U32(r, &nArgs, err) != TCL_OK) return TCL_ERROR;
        unsigned char *args = ckalloc(nArgs);
        if (R_Bytes(r, args, nArgs, err) != TCL_OK) { ckfree(args); return TCL_ERROR; }
        if (R_U32(r, &nNs, err) != TCL_OK) { ckfree(args); return TCL_ERROR; }
        unsigned char *ns = ckalloc(nNs);
        if (R_Bytes(r, ns, nNs, err) != TCL_OK) { ckfree(args); ckfree(ns); return TCL_ERROR; }
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendToObj(pv, "<lambda ", -1);
        AppendQuoted(pv, args, nArgs);
        Tcl_AppendToObj(pv, " in ", -1);
        AppendQuoted(pv, ns, nNs);
        Tcl_AppendToObj(pv, ">", -1);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] LAMBDA args=", idx);
        AppendQuoted(out, args, nArgs);
        Tcl_AppendToObj(out, " ns=", -1);
        AppendQuoted(out, ns, nNs);
        Tcl_AppendToObj(out, "\n", 1);
        ckfree(args); ckfree(ns);
        /* Nested bytecode block follows: reuse the compiled block dumper
         * in a compact mode: just skip/consume it (we don't recurse fully here). */
        uint32_t codeLen;
        if (R_U32(r, &codeLen, err) != TCL_OK) return TCL_ERROR;
        unsigned char *code = ckalloc(codeLen);
        if (R_Bytes(r, code, codeLen, err) != TCL_OK) { ckfree(code); return TCL_ERROR; }
        ckfree(code);
        /* And then the literal/aux/except/epilogue of the nested block */
        uint32_t nLit; if (R_U32(r, &nLit, err) != TCL_OK) return TCL_ERROR;
        for (uint32_t i = 0; i < nLit; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
        }
        uint32_t nAux; if (R_U32(r, &nAux, err) != TCL_OK) return TCL_ERROR;
        for (uint32_t i = 0; i < nAux; i++) {
            uint32_t auxTag, auxLen;
            if (R_U32(r, &auxTag, err) != TCL_OK) return TCL_ERROR;
            if (R_U32(r, &auxLen, err) != TCL_OK) return TCL_ERROR;
            unsigned char *skip = ckalloc(auxLen);
            if (R_Bytes(r, skip, auxLen, err) != TCL_OK) { ckfree(skip); return TCL_ERROR; }
            ckfree(skip);
        }
        uint32_t nExc; if (R_U32(r, &nExc, err) != TCL_OK) return TCL_ERROR;
        if (nExc > 0) {
            unsigned char *skip = ckalloc(nExc * 20);
            if (R_Bytes(r, skip, nExc * 20, err) != TCL_OK) { ckfree(skip); return TCL_ERROR; }
            ckfree(skip);
        }
        uint32_t epLen; if (R_U32(r, &epLen, err) != TCL_OK) return TCL_ERROR;
        unsigned char *ep = ckalloc(epLen);
        if (R_Bytes(r, ep, epLen, err) != TCL_OK) { ckfree(ep); return TCL_ERROR; }
        ckfree(ep);
        break;
    }
    case LIT_BYTECODE: {
        /* Saved subprogram: we report a compact summary, and consume. */
        uint32_t nameLen;
        if (R_U32(r, &nameLen, err) != TCL_OK) return TCL_ERROR;
        unsigned char *nm = ckalloc(nameLen);
        if (R_Bytes(r, nm, nameLen, err) != TCL_OK) { ckfree(nm); return TCL_ERROR; }
        Tcl_Obj *pv = Tcl_NewObj(); Tcl_IncrRefCount(pv);
        Tcl_AppendToObj(pv, "<bytecode ", -1);
        AppendQuoted(pv, nm, nameLen);
        Tcl_AppendToObj(pv, ">", -1);
        dst->preview = pv;
        Tcl_AppendPrintfToObj(out, "  [%u] BYTECODE name=", idx);
        AppendQuoted(out, nm, nameLen);
        Tcl_AppendToObj(out, "\n", 1);
        ckfree(nm);

        /* Read nested compiled block (code/lits/aux/exc/epi) as above */
        uint32_t codeLen;
        if (R_U32(r, &codeLen, err) != TCL_OK) return TCL_ERROR;
        unsigned char *code = ckalloc(codeLen);
        if (R_Bytes(r, code, codeLen, err) != TCL_OK) { ckfree(code); return TCL_ERROR; }
        ckfree(code);
        uint32_t nLit; if (R_U32(r, &nLit, err) != TCL_OK) return TCL_ERROR;
        for (uint32_t i = 0; i < nLit; i++) {
            LitPreview tmp = {0, NULL};
            if (ReadOneLiteral(r, &tmp, out, UINT32_MAX, err) != TCL_OK) {
                if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
                return TCL_ERROR;
            }
            if (tmp.preview) Tcl_DecrRefCount(tmp.preview);
        }
        uint32_t nAux; if (R_U32(r, &nAux, err) != TCL_OK) return TCL_ERROR;
        for (uint32_t i = 0; i < nAux; i++) {
            uint32_t auxTag, auxLen;
            if (R_U32(r, &auxTag, err) != TCL_OK) return TCL_ERROR;
            if (R_U32(r, &auxLen, err) != TCL_OK) return TCL_ERROR;
            unsigned char *skip = ckalloc(auxLen);
            if (R_Bytes(r, skip, auxLen, err) != TCL_OK) { ckfree(skip); return TCL_ERROR; }
            ckfree(skip);
        }
        uint32_t nExc; if (R_U32(r, &nExc, err) != TCL_OK) return TCL_ERROR;
        if (nExc > 0) {
            unsigned char *skip = ckalloc(nExc * 20);
            if (R_Bytes(r, skip, nExc * 20, err) != TCL_OK) { ckfree(skip); return TCL_ERROR; }
            ckfree(skip);
        }
        uint32_t epLen; if (R_U32(r, &epLen, err) != TCL_OK) return TCL_ERROR;
        unsigned char *ep = ckalloc(epLen);
        if (R_Bytes(r, ep, epLen, err) != TCL_OK) { ckfree(ep); return TCL_ERROR; }
        ckfree(ep);
        break;
    }
    default:
        Tcl_AppendPrintfToObj(err, "unknown literal tag %u", (unsigned)dst->tag);
        return TCL_ERROR;
    }
    return TCL_OK;
}

/* ======== 9.1 bytecode disassembler ======== */

/* Operand kinds (copied from tclCompile.h) */
typedef enum {
    OP_NONE,
    OP_INT1, OP_INT4,
    OP_UINT1, OP_UINT4,
    OP_IDX4,
    OP_LVT1, OP_LVT4,
    OP_AUX4,
    OP_OFF1, OP_OFF4,
    OP_LIT1, OP_LIT4,
    OP_SCLS1,
    OP_UNSF1,
    OP_CLK1,
    OP_LRPL1
} OpKind;

/* Opcode descriptor (subset of Tcl's InstructionDesc) */
typedef struct {
    const char *name;
    unsigned    nops;
    OpKind      op[2];
    unsigned    deprecated; /* 1 if deprecated — disassembly forbidden */
} OpInfo;

/* Utility to get operand byte width for advancing pc */
static inline unsigned
OpKindSize(OpKind k) {
    switch (k) {
        case OP_INT1: case OP_UINT1: case OP_OFF1: case OP_LVT1:
        case OP_LIT1: case OP_SCLS1: case OP_UNSF1: case OP_CLK1:
        case OP_LRPL1:
            return 1u;
        case OP_INT4: case OP_UINT4: case OP_IDX4: case OP_LVT4:
        case OP_AUX4: case OP_OFF4: case OP_LIT4:
            return 4u;
        case OP_NONE: default:
            return 0u;
    }
}

/* NOTE: The order MUST match Tcl 9.1's tclInstructionTable[] exactly.
 * We mark the legacy 1‑byte forms as deprecated (forbidden here).
 */
#define BTOP0(nm)              { nm, 0, {OP_NONE, OP_NONE}, 0 }
#define BTOP1(nm,t1)           { nm, 1, {t1, OP_NONE}, 0 }
#define BTOP2(nm,t1,t2)        { nm, 2, {t1, t2}, 0 }
#define DTOP0(nm)             { nm, 0, {OP_NONE, OP_NONE}, 1 }
#define DTOP1(nm,t1)          { nm, 1, {t1, OP_NONE}, 1 }
#define DTOP2(nm,t1,t2)       { nm, 2, {t1, t2}, 1 }

static const OpInfo opTable[] = {
/* 0..9 */
BTOP0("done"),
DTOP1("push1",           OP_LIT1),
BTOP1("push",             OP_LIT4),
BTOP0("pop"),
BTOP0("dup"),
BTOP1("strcat",           OP_UINT1),
DTOP1("invokeStk1",      OP_UINT1),
BTOP1("invokeStk",        OP_UINT4),
BTOP0("evalStk"),
BTOP0("exprStk"),
/* 10..23 */
DTOP1("loadScalar1",     OP_LVT1),
BTOP1("loadScalar",       OP_LVT4),
BTOP0("loadScalarStk"),
DTOP1("loadArray1",      OP_LVT1),
BTOP1("loadArray",        OP_LVT4),
BTOP0("loadArrayStk"),
BTOP0("loadStk"),
DTOP1("storeScalar1",    OP_LVT1),
BTOP1("storeScalar",      OP_LVT4),
BTOP0("storeScalarStk"),
DTOP1("storeArray1",     OP_LVT1),
BTOP1("storeArray",       OP_LVT4),
BTOP0("storeArrayStk"),
BTOP0("storeStk"),
/* 24..34 */
DTOP1("incrScalar1",     OP_LVT1),
BTOP0("incrScalarStk"),
DTOP1("incrArray1",      OP_LVT1),
BTOP0("incrArrayStk"),
BTOP0("incrStk"),
DTOP2("incrScalar1Imm",  OP_LVT1, OP_INT1),
BTOP1("incrScalarStkImm", OP_INT1),
DTOP2("incrArray1Imm",   OP_LVT1, OP_INT1),
BTOP1("incrArrayStkImm",  OP_INT1),
BTOP1("incrStkImm",       OP_INT1),
/* 35..40 */
DTOP1("jump1",           OP_OFF1),
BTOP1("jump",             OP_OFF4),
DTOP1("jumpTrue1",       OP_OFF1),
BTOP1("jumpTrue",         OP_OFF4),
DTOP1("jumpFalse1",      OP_OFF1),
BTOP1("jumpFalse",        OP_OFF4),
/* 41..67 (binary ops, control, catch, results) */
BTOP0("bitor"), BTOP0("bitxor"), BTOP0("bitand"),
BTOP0("eq"), BTOP0("neq"), BTOP0("lt"), BTOP0("gt"), BTOP0("le"), BTOP0("ge"),
BTOP0("lshift"), BTOP0("rshift"),
BTOP0("add"), BTOP0("sub"), BTOP0("mult"), BTOP0("div"), BTOP0("mod"),
BTOP0("uplus"), BTOP0("uminus"), BTOP0("bitnot"), BTOP0("not"),
BTOP0("tryCvtToNumeric"),
BTOP0("break"), BTOP0("continue"),
BTOP1("beginCatch",       OP_UINT4),
BTOP0("endCatch"),
BTOP0("pushResult"),
BTOP0("pushReturnCode"),
/* 68..76 string/list basics */
BTOP0("streq"), BTOP0("strneq"), BTOP0("strcmp"),
BTOP0("strlen"), BTOP0("strindex"),
BTOP1("strmatch",         OP_INT1),
BTOP1("list",             OP_UINT4),
BTOP0("listIndex"),
BTOP0("listLength"),
/* 77..88 append/lappend family */
DTOP1("appendScalar1",   OP_LVT1),
BTOP1("appendScalar",     OP_LVT4),
DTOP1("appendArray1",    OP_LVT1),
BTOP1("appendArray",      OP_LVT4),
BTOP0("appendArrayStk"),
BTOP0("appendStk"),
DTOP1("lappendScalar1",  OP_LVT1),
BTOP1("lappendScalar",    OP_LVT4),
DTOP1("lappendArray1",   OP_LVT1),
BTOP1("lappendArray",     OP_LVT4),
BTOP0("lappendArrayStk"),
BTOP0("lappendStk"),
/* 89..94 misc list & return & exponent */
BTOP1("lindexMulti",      OP_UINT4),
BTOP1("over",             OP_UINT4),
BTOP0("lsetList"),
BTOP1("lsetFlat",         OP_UINT4),
BTOP2("returnImm",        OP_INT4, OP_UINT4),
BTOP0("expon"),
/* 95..101 compiled-command framing */
BTOP1("listIndexImm",     OP_IDX4),
BTOP2("listRangeImm",     OP_IDX4, OP_IDX4),
BTOP2("startCommand",     OP_OFF4, OP_UINT4),
BTOP0("listIn"),
BTOP0("listNotIn"),
BTOP0("pushReturnOpts"),
BTOP0("returnStk"),
/* 102..111 dict path ops */
BTOP1("dictGet",          OP_UINT4),
BTOP2("dictSet",          OP_UINT4, OP_LVT4),
BTOP2("dictUnset",        OP_UINT4, OP_LVT4),
BTOP2("dictIncrImm",      OP_INT4,  OP_LVT4),
BTOP1("dictAppend",       OP_LVT4),
BTOP1("dictLappend",      OP_LVT4),
BTOP1("dictFirst",        OP_LVT4),
BTOP1("dictNext",         OP_LVT4),
BTOP2("dictUpdateStart",  OP_LVT4, OP_AUX4),
BTOP2("dictUpdateEnd",    OP_LVT4, OP_AUX4),
/* 112..123 switch/upvar/exists/nop */
BTOP1("jumpTable",        OP_AUX4),
BTOP1("upvar",            OP_LVT4),
BTOP1("nsupvar",          OP_LVT4),
BTOP1("variable",         OP_LVT4),
BTOP2("syntax",           OP_INT4, OP_UINT4),
BTOP1("reverse",          OP_UINT4),
BTOP1("regexp",           OP_INT1),
BTOP1("existScalar",      OP_LVT4),
BTOP1("existArray",       OP_LVT4),
BTOP0("existArrayStk"),
BTOP0("existStk"),
BTOP0("nop"),
/* 124..132 unset group and dict-with helpers */
DTOP0("returnCodeBranch1"),
BTOP2("unsetScalar",      OP_UNSF1, OP_LVT4),
BTOP2("unsetArray",       OP_UNSF1, OP_LVT4),
BTOP1("unsetArrayStk",    OP_UNSF1),
BTOP1("unsetStk",         OP_UNSF1),
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
BTOP1("arrayExistsImm",   OP_LVT4),
BTOP0("arrayMakeStk"),
BTOP1("arrayMakeImm",     OP_LVT4),
BTOP2("invokeReplace",    OP_UINT4, OP_UINT1),
/* 145..154 list ops, foreach, string trim */
BTOP0("listConcat"),
BTOP0("expandDrop"),
BTOP1("foreach_start",    OP_AUX4),
BTOP0("foreach_step"),
BTOP0("foreach_end"),
BTOP0("lmap_collect"),
BTOP0("strtrim"),
BTOP0("strtrimLeft"),
BTOP0("strtrimRight"),
BTOP1("concatStk",        OP_UINT4),
/* 155..161 cases and origin */
BTOP0("strcaseUpper"),
BTOP0("strcaseLower"),
BTOP0("strcaseTitle"),
BTOP0("strreplace"),
BTOP0("originCmd"),
DTOP1("tclooNext",       OP_UINT1),
DTOP1("tclooNextClass",  OP_UINT1),
/* 162..171 coroutine/numeric/string class + lappendList & clock/dictGetDef */
BTOP0("yieldToInvoke"),
BTOP0("numericType"),
BTOP0("tryCvtToBoolean"),
BTOP1("strclass",         OP_SCLS1),
BTOP1("lappendList",      OP_LVT4),
BTOP1("lappendListArray", OP_LVT4),
BTOP0("lappendListArrayStk"),
BTOP0("lappendListStk"),
BTOP1("clockRead",        OP_CLK1),
BTOP1("dictGetDef",       OP_UINT4),
/* 172..178 TIP 461 strings, lreplace, const */
BTOP0("strlt"), BTOP0("strgt"), BTOP0("strle"), BTOP0("strge"),
BTOP2("lreplace",         OP_UINT4, OP_LRPL1),
BTOP1("constImm",         OP_LVT4),
BTOP0("constStk"),
/* 179..185 Updated 9.1 incr/tail/oo-next */
BTOP1("incrScalar",       OP_LVT4),
BTOP1("incrArray",        OP_LVT4),
BTOP2("incrScalarImm",    OP_LVT4, OP_INT1),
BTOP2("incrArrayImm",     OP_LVT4, OP_INT1),
BTOP1("tailcall",         OP_UINT4),
BTOP1("tclooNext",        OP_UINT4),
BTOP1("tclooNextClass",   OP_UINT4),
/* 186..197 Really new 9.1 ops */
BTOP0("swap"),
BTOP0("errorPrefixEq"),
BTOP0("tclooId"),
BTOP0("dictPut"),
BTOP0("dictRemove"),
BTOP0("isEmpty"),
BTOP1("jumpTableNum",     OP_AUX4),
BTOP0("tailcallList"),
BTOP0("tclooNextList"),
BTOP0("tclooNextClassList"),
BTOP1("arithSeries",      OP_UINT1),
BTOP0("uplevel"),
};

/* Reads a 4-byte little-endian (Tcl's internal byte order is defined) */
static inline uint32_t U32LE(const unsigned char *p) {
    return  ((uint32_t)p[0])
          | ((uint32_t)p[1] << 8)
          | ((uint32_t)p[2] << 16)
          | ((uint32_t)p[3] << 24);
}
static inline int32_t  I32LE(const unsigned char *p) { return (int32_t)U32LE(p); }

static void
PrintOperand(Tcl_Obj *out, OpKind k, const unsigned char *p,
             Tcl_Size pc, const LitPreview *lits, uint32_t nLits)
{
    switch (k) {
    case OP_INT1:    Tcl_AppendPrintfToObj(out, " %d", (int8_t)p[0]); break;
    case OP_INT4:    Tcl_AppendPrintfToObj(out, " %d", I32LE(p)); break;
    case OP_UINT1:   Tcl_AppendPrintfToObj(out, " %u", (unsigned)p[0]); break;
    case OP_UINT4:   Tcl_AppendPrintfToObj(out, " %u", (unsigned)U32LE(p)); break;
    case OP_IDX4:    Tcl_AppendPrintfToObj(out, " %d", I32LE(p)); break;
    case OP_LVT1:    Tcl_AppendPrintfToObj(out, " lvt[%u]", (unsigned)p[0]); break;
    case OP_LVT4:    Tcl_AppendPrintfToObj(out, " lvt[%u]", (unsigned)U32LE(p)); break;
    case OP_AUX4:    Tcl_AppendPrintfToObj(out, " aux[%u]", (unsigned)U32LE(p)); break;
    case OP_OFF1:    Tcl_AppendPrintfToObj(out, " ->%+" PRId64, (int64_t)(int8_t)p[0] + (int64_t)pc); break;
    case OP_OFF4:    Tcl_AppendPrintfToObj(out, " ->%+" PRId64, (int64_t)I32LE(p) + (int64_t)pc); break;
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
    case OP_SCLS1:   Tcl_AppendPrintfToObj(out, " strClass#%u", (unsigned)p[0]); break;
    case OP_UNSF1:   Tcl_AppendPrintfToObj(out, " unsetFlags=0x%02x", (unsigned)p[0]); break;
    case OP_CLK1:    Tcl_AppendPrintfToObj(out, " clock#%u", (unsigned)p[0]); break;
    case OP_LRPL1:   Tcl_AppendPrintfToObj(out, " lreplaceFlags=0x%02x", (unsigned)p[0]); break;
    case OP_NONE:    default: break;
    }
}

static int
DisassembleCode(Tcl_Obj *out, const unsigned char *code, uint32_t codeLen,
                const LitPreview *lits, uint32_t nLits, Tcl_Obj *err)
{
    Tcl_AppendToObj(out, "  code-disassembly:\n", -1);
    Tcl_Size pc = 0;
    while ((uint32_t)pc < codeLen) {
        unsigned op = code[pc];
        if (op >= sizeof(opTable)/sizeof(opTable[0])) {
            Tcl_AppendPrintfToObj(err, "unknown opcode %u at pc=%" TCL_SIZE_MODIFIER "d", op, pc);
            return TCL_ERROR;
        }
        const OpInfo *d = &opTable[op];
        if (d->deprecated) {
            Tcl_AppendPrintfToObj(err,
                "deprecated opcode \"%s\" (op=%u) at pc=%" TCL_SIZE_MODIFIER "d — 9.1-only disassembler refuses to decode",
                d->name, op, pc);
            return TCL_ERROR;
        }

        Tcl_AppendPrintfToObj(out, "    %6" TCL_SIZE_MODIFIER "d: %-18s", pc, d->name);

        /* Gather and print operands */
        const unsigned char *p = code + pc + 1;
        Tcl_Size bytes = 1;
        for (unsigned i = 0; i < d->nops; i++) {
            PrintOperand(out, d->op[i], p, pc, lits, nLits);
            unsigned w = OpKindSize(d->op[i]);
            p += w; bytes += w;
        }
        Tcl_AppendToObj(out, "\n", 1);
        pc += bytes;
    }
    return TCL_OK;
}

/* ===== Existing TBCX container dump (rewired to disassemble) ===== */

static int DumpCompiledBlock(Reader *r, Tcl_Obj *out, Tcl_Obj *err);

static int
DumpOneAux(Reader *r, Tcl_Obj *out, Tcl_Obj *err, uint32_t idx)
{
    uint32_t tag, len;
    if (R_U32(r, &tag, err) != TCL_OK) return TCL_ERROR;
    if (R_U32(r, &len, err) != TCL_OK) return TCL_ERROR;
    Tcl_AppendPrintfToObj(out, "  [aux %u] tag=%u len=%u", idx, tag, len);
    /* Pretty-print small tables (e.g., jump tables) */
    if (len > 0) {
        unsigned char *buf = ckalloc(len);
        if (R_Bytes(r, buf, len, err) != TCL_OK) { ckfree(buf); return TCL_ERROR; }
        Tcl_AppendToObj(out, " data=", -1);
        Tcl_Obj *hx = MakeHexBytes(buf, len, 32);
        Tcl_AppendObjToObj(out, hx);
        Tcl_DecrRefCount(hx);
        ckfree(buf);
    }
    Tcl_AppendToObj(out, "\n", 1);
    return TCL_OK;
}

static int
DumpCompiledBlock(Reader *r, Tcl_Obj *out, Tcl_Obj *err)
{
    /* Read code first (buffer it), then literals (so we can resolve), then aux,
       then print disassembly, then exceptions and epilogue. */
    uint32_t codeLen;
    if (R_U32(r, &codeLen, err) != TCL_OK) return TCL_ERROR;
    unsigned char *code = ckalloc(codeLen);
    if (R_Bytes(r, code, codeLen, err) != TCL_OK) { ckfree(code); return TCL_ERROR; }
    Tcl_AppendPrintfToObj(out, "  codeLen=%u\n", codeLen);

    /* Literals */
    uint32_t nLits;
    if (R_U32(r, &nLits, err) != TCL_OK) { ckfree(code); return TCL_ERROR; }
    Tcl_AppendPrintfToObj(out, "  literals=%u\n", nLits);
    LitPreview *lits = (nLits ? (LitPreview*)ckalloc(sizeof(LitPreview) * nLits) : NULL);
    for (uint32_t i = 0; i < nLits; i++) {
        lits[i].tag = 0; lits[i].preview = NULL;
        if (ReadOneLiteral(r, &lits[i], out, i, err) != TCL_OK) {
            FreeLitPreviewArray(lits, nLits);
            ckfree(code);
            return TCL_ERROR;
        }
    }

    /* Aux data */
    uint32_t nAux;
    if (R_U32(r, &nAux, err) != TCL_OK) { FreeLitPreviewArray(lits, nLits); ckfree(code); return TCL_ERROR; }
    Tcl_AppendPrintfToObj(out, "  auxData=%u\n", nAux);
    for (uint32_t i = 0; i < nAux; i++) {
        if (DumpOneAux(r, out, err, i) != TCL_OK) {
            FreeLitPreviewArray(lits, nLits);
            ckfree(code);
            return TCL_ERROR;
        }
    }

    /* Disassemble the buffered code (AFTER collecting literal previews). */
    if (DisassembleCode(out, code, codeLen, lits, nLits, err) != TCL_OK) {
        FreeLitPreviewArray(lits, nLits);
        ckfree(code);
        return TCL_ERROR;
    }

    /* Exception ranges */
    uint32_t nExc;
    if (R_U32(r, &nExc, err) != TCL_OK) { FreeLitPreviewArray(lits, nLits); ckfree(code); return TCL_ERROR; }
    Tcl_AppendPrintfToObj(out, "  exceptions=%u\n", nExc);
    for (uint32_t i = 0; i < nExc; i++) {
        uint32_t type, nest, codeOfs, codeBytes, brk, cont, catc;
        if (R_U32(r, &type, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &nest, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &codeOfs, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &codeBytes, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &brk, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &cont, err) != TCL_OK) goto exc_fail;
        if (R_U32(r, &catc, err) != TCL_OK) goto exc_fail;
        Tcl_AppendPrintfToObj(out, "    [%u] type=%u nest=%u code=[%u..%u) break=%u continue=%u catch=%u\n",
                              i, type, nest, codeOfs, codeOfs+codeBytes, brk, cont, catc);
        continue;
    exc_fail:
        FreeLitPreviewArray(lits, nLits); ckfree(code); return TCL_ERROR;
    }

    /* Epilogue blob */
    uint32_t epLen;
    if (R_U32(r, &epLen, err) != TCL_OK) { FreeLitPreviewArray(lits, nLits); ckfree(code); return TCL_ERROR; }
    Tcl_AppendPrintfToObj(out, "  epilogueLen=%u\n", epLen);
    if (epLen > 0) {
        unsigned char *ep = ckalloc(epLen);
        if (R_Bytes(r, ep, epLen, err) != TCL_OK) { ckfree(ep); FreeLitPreviewArray(lits, nLits); ckfree(code); return TCL_ERROR; }
        Tcl_Obj *hx = MakeHexBytes(ep, epLen, 32);
        Tcl_AppendToObj(out, "  epilogue=", -1);
        Tcl_AppendObjToObj(out, hx);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_DecrRefCount(hx);
        ckfree(ep);
    }

    FreeLitPreviewArray(lits, nLits);
    ckfree(code);
    return TCL_OK;
}

/* ===== Top-level dumper: classes, methods, etc. ===== */

static int
DumpHeader(Reader *r, Tcl_Obj *out, Tcl_Obj *err, uint32_t *flagsOut)
{
    unsigned char magic[8];
    if (R_Bytes(r, magic, 8, err) != TCL_OK) return TCL_ERROR;
    if (memcmp(magic, "TBCX\0\0\0\1", 8) != 0) {
        Tcl_AppendToObj(err, "bad magic", -1);
        return TCL_ERROR;
    }
    unsigned char leByte;
    if (R_U8(r, &leByte, err) != TCL_OK) return TCL_ERROR;
    r->le = (leByte != 0);
    uint32_t flags;
    if (R_U32(r, &flags, err) != TCL_OK) return TCL_ERROR;
    *flagsOut = flags;
    Tcl_AppendPrintfToObj(out, "tbcx: le=%u flags=0x%08x\n", (unsigned)r->le, flags);
    return TCL_OK;
}

static int
DumpOneMethod(Reader *r, Tcl_Obj *out, Tcl_Obj *err)
{
    uint32_t nameLen;
    if (R_U32(r, &nameLen, err) != TCL_OK) return TCL_ERROR;
    unsigned char *name = ckalloc(nameLen);
    if (R_Bytes(r, name, nameLen, err) != TCL_OK) { ckfree(name); return TCL_ERROR; }
    Tcl_AppendToObj(out, " method ", -1);
    AppendQuoted(out, name, nameLen);
    Tcl_AppendToObj(out, ":\n", -1);
    ckfree(name);
    return DumpCompiledBlock(r, out, err);
}

static int
DumpOneClass(Reader *r, Tcl_Obj *out, Tcl_Obj *err)
{
    uint32_t nameLen, nMethods;
    if (R_U32(r, &nameLen, err) != TCL_OK) return TCL_ERROR;
    unsigned char *name = ckalloc(nameLen);
    if (R_Bytes(r, name, nameLen, err) != TCL_OK) { ckfree(name); return TCL_ERROR; }
    if (R_U32(r, &nMethods, err) != TCL_OK) { ckfree(name); return TCL_ERROR; }
    Tcl_AppendToObj(out, "class ", -1);
    AppendQuoted(out, name, nameLen);
    Tcl_AppendPrintfToObj(out, " methods=%u\n", nMethods);
    ckfree(name);
    for (uint32_t i = 0; i < nMethods; i++) {
        if (DumpOneMethod(r, out, err) != TCL_OK) return TCL_ERROR;
    }
    return TCL_OK;
}

static int
DumpFile(Tcl_Interp *interp, Tcl_Channel ch, Tcl_Obj **outObj)
{
    Reader R = {0};
    R.ch = ch;
    R.limit = -1;
    R.le = 1;
    Tcl_Obj *out = Tcl_NewObj();
    Tcl_Obj *err = Tcl_NewObj();
    Tcl_IncrRefCount(out); Tcl_IncrRefCount(err);

    if (CheckBinaryChan(interp, ch) != TCL_OK) goto fail;

    uint32_t flags;
    if (DumpHeader(&R, out, err, &flags) != TCL_OK) goto fail;

    uint32_t nClasses;
    if (R_U32(&R, &nClasses, err) != TCL_OK) goto fail;
    Tcl_AppendPrintfToObj(out, "classes=%u\n", nClasses);
    for (uint32_t i = 0; i < nClasses; i++) {
        if (DumpOneClass(&R, out, err) != TCL_OK) goto fail;
    }

    *outObj = out;
    Tcl_DecrRefCount(err);
    return TCL_OK;

fail:
    Tcl_DecrRefCount(out);
    Tcl_SetObjResult(interp, err);
    return TCL_ERROR;
}

/* ===== Tcl command: tbcx::dumpfile channel ?-limit N? ===== */

int
Tbcx_DumpFileObjCmd(ClientData cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "channelId");
        return TCL_ERROR;
    }
    Tcl_Channel ch = Tcl_GetChannel(interp, Tcl_GetString(objv[1]), NULL);
    if (!ch) return TCL_ERROR;

    Tcl_Obj *out = NULL;
    if (DumpFile(interp, ch, &out) != TCL_OK) return TCL_ERROR;
    Tcl_SetObjResult(interp, out);
    return TCL_OK;
}
