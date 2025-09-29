/* tbcxdump.c — human-readable TBCX dumper for Tcl 9.1
 *
 */

#include <stdarg.h>
#include <stdio.h>

#include "tbcx.h"

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct {
    Tcl_Interp *interp;
    Tcl_Channel chan;
    int         err;      /* TCL_OK or TCL_ERROR */
    Tcl_WideInt consumed; /* bytes consumed from channel */
} TbcxIn;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static void        Appendf(Tcl_Obj *o, const char *fmt, ...);
static void        AppendPrintfVA(Tcl_Obj *objPtr, const char *fmt, va_list ap);
static void        AppendQuoted(Tcl_Obj *o, const char *s, uint32_t n, uint32_t limit);
static void        DumpCompiledBlock(TbcxIn *r, Tcl_Obj *o, int depth, uint32_t *out_codeLen, uint32_t *out_numLits, uint32_t *out_numAux, uint32_t *out_numExcept, uint32_t *out_numLocals,
                                     uint32_t *out_maxStack);
static void        Indent(Tcl_Obj *o, int depth);
static int         R_Bytes(TbcxIn *r, void *dst, size_t n);
static inline void R_Error(TbcxIn *r, const char *msg);
static char       *R_LPString(TbcxIn *r, uint32_t *outLen);
static uint32_t    R_U32(TbcxIn *r);
static uint64_t    R_U64(TbcxIn *r);
static uint8_t     R_U8(TbcxIn *r);
static int         SkipBytes(TbcxIn *r, Tcl_WideInt n);

/* ==========================================================================
 * Stuff
 * ========================================================================== */

static void        AppendPrintfVA(Tcl_Obj *objPtr, const char *fmt, va_list ap) {
    /* First try a small stack buffer; if it's not enough, allocate once. */
    char    stack[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return; /* formatting failed; nothing to append */
    }
    if ((size_t)n < sizeof stack) {
        /* Fits in stack buffer (n excludes terminating NUL) */
        Tcl_AppendToObj(objPtr, stack, (Tcl_Size)n);
    } else {
        /* Need a dynamic buffer of size n+1 (include NUL) */
        size_t need = (size_t)n + 1;
        char  *buf  = (char *)Tcl_Alloc(need);
        (void)vsnprintf(buf, need, fmt, ap2);
        Tcl_AppendToObj(objPtr, buf, (Tcl_Size)n);
        Tcl_Free(buf);
    }
    va_end(ap2);
}

static inline void R_Error(TbcxIn *r, const char *msg) {
    if (r->err == TCL_OK) {
        Tcl_SetObjResult(r->interp, Tcl_NewStringObj(msg, -1));
        r->err = TCL_ERROR;
    }
}

static int R_Bytes(TbcxIn *r, void *dst, size_t n) {
    if (r->err != TCL_OK)
        return 0;
    int got = Tcl_ReadRaw(r->chan, (char *)dst, (int)n);
    if (got != (int)n) {
        R_Error(r, "tbcx::dump: short read");
        return 0;
    }
    r->consumed += (Tcl_WideInt)n;
    return 1;
}

static int SkipBytes(TbcxIn *r, Tcl_WideInt n) {
    if (r->err != TCL_OK)
        return 0;
    char buf[4096];
    while (n > 0) {
        int chunk = (int)((n > (Tcl_WideInt)sizeof(buf)) ? sizeof(buf) : n);
        int got   = Tcl_ReadRaw(r->chan, buf, chunk);
        if (got != chunk) {
            R_Error(r, "tbcx::dump: short read while skipping");
            return 0;
        }
        r->consumed += got;
        n -= got;
    }
    return 1;
}

static uint8_t R_U8(TbcxIn *r) {
    uint8_t v = 0;
    R_Bytes(r, &v, 1);
    return v;
}

static uint32_t R_U32(TbcxIn *r) {
    uint32_t v = 0;
    R_Bytes(r, &v, 4);
    if (!tbcxHostIsLE) {
#if defined(__GNUC__) || defined(__clang__)
        v = __builtin_bswap32(v);
#else
        v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
#endif
    }
    return v;
}

static uint64_t R_U64(TbcxIn *r) {
    uint64_t v = 0;
    R_Bytes(r, &v, 8);
    if (!tbcxHostIsLE) {
#if defined(__GNUC__) || defined(__clang__)
        v = __builtin_bswap64(v);
#else
        v = ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) | ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) | ((v & 0x000000FF00000000ull) >> 8) |
            ((v & 0x0000FF0000000000ull) >> 24) | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
#endif
    }
    return v;
}

static char *R_LPString(TbcxIn *r, uint32_t *outLen) {
    uint32_t n = R_U32(r);
    if (n > TBCX_MAX_STR) {
        R_Error(r, "tbcx::dump: LPString too large");
        return NULL;
    }
    char *s = (char *)Tcl_Alloc((size_t)n + 1u);
    if (!R_Bytes(r, s, n)) {
        Tcl_Free(s);
        return NULL;
    }
    s[n] = '\0';
    if (outLen)
        *outLen = n;
    return s;
}

static void Indent(Tcl_Obj *o, int depth) {
    int  spaces = depth * 2;
    char buf[128];
    while (spaces > 0) {
        int chunk = spaces > (int)sizeof(buf) ? (int)sizeof(buf) : spaces;
        memset(buf, ' ', (size_t)chunk);
        Tcl_AppendToObj(o, buf, chunk);
        spaces -= chunk;
    }
}

static void Appendf(Tcl_Obj *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    AppendPrintfVA(o, fmt, ap);
    va_end(ap);
}

static void AppendQuoted(Tcl_Obj *o, const char *s, uint32_t n, uint32_t limit) {
    Tcl_AppendToObj(o, "\"", 1);
    uint32_t shown = (limit && n > limit) ? limit : n;
    for (uint32_t i = 0; i < shown; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\\':
            Tcl_AppendToObj(o, "\\\\", 2);
            break;
        case '\"':
            Tcl_AppendToObj(o, "\\\"", 2);
            break;
        case '\n':
            Tcl_AppendToObj(o, "\\n", 2);
            break;
        case '\r':
            Tcl_AppendToObj(o, "\\r", 2);
            break;
        case '\t':
            Tcl_AppendToObj(o, "\\t", 2);
            break;
        default:
            if (c < 0x20 || c >= 0x7F) {
                char hx[5];
                snprintf(hx, sizeof(hx), "\\x%02X", c);
                Tcl_AppendToObj(o, hx, -1);
            } else {
                char ch = (char)c;
                Tcl_AppendToObj(o, &ch, 1);
            }
        }
    }
    if (shown < n) {
        Appendf(o, "\" …(+%u bytes)", (unsigned)(n - shown));
        return;
    }
    Tcl_AppendToObj(o, "\"", 1);
}

static void AppendHexPreview(Tcl_Obj *o, const unsigned char *p, uint32_t n, uint32_t limit) {
    uint32_t shown = (limit && n > limit) ? limit : n;
    Tcl_AppendToObj(o, "0x", 2);
    for (uint32_t i = 0; i < shown; i++) {
        char hx[3];
        snprintf(hx, sizeof(hx), "%02X", p[i]);
        Tcl_AppendToObj(o, hx, 2);
        if (i + 1 < shown)
            Tcl_AppendToObj(o, " ", 1);
    }
    if (shown < n) {
        Appendf(o, " …(+%u bytes)", (unsigned)(n - shown));
    }
}

static const char *LitTagName(uint32_t tag) {
    switch (tag) {
    case TBCX_LIT_BIGNUM:
        return "BIGNUM";
    case TBCX_LIT_BOOLEAN:
        return "BOOLEAN";
    case TBCX_LIT_BYTEARR:
        return "BYTEARRAY";
    case TBCX_LIT_DICT:
        return "DICT";
    case TBCX_LIT_DOUBLE:
        return "DOUBLE";
    case TBCX_LIT_LIST:
        return "LIST";
    case TBCX_LIT_STRING:
        return "STRING";
    case TBCX_LIT_WIDEINT:
        return "WIDEINT";
    case TBCX_LIT_WIDEUINT:
        return "WIDEUINT";
    case TBCX_LIT_LAMBDA_BC:
        return "LAMBDA_BC";
    case TBCX_LIT_BYTECODE:
        return "BYTECODE";
    default:
        return "UNKNOWN";
    }
}

static const char *AuxTagName(uint32_t tag) {
    switch (tag) {
    case TBCX_AUX_JT_STR:
        return "Jumptable[str]";
    case TBCX_AUX_JT_NUM:
        return "Jumptable[num]";
    case TBCX_AUX_DICTUPD:
        return "DictUpdate";
    case TBCX_AUX_NEWFORE:
        return "NewForeach";
    case TBCX_AUX_FOREACH:
        return "Foreach";
    default:
        return "UNKNOWN";
    }
}

static const char *MethKindName(uint8_t k) {
    switch (k) {
    case TBCX_METH_INST:
        return "method";
    case TBCX_METH_CLASS:
        return "classmethod";
    case TBCX_METH_CTOR:
        return "constructor";
    case TBCX_METH_DTOR:
        return "destructor";
    default:
        return "unknown";
    }
}

static void DumpLiteral(TbcxIn *r, Tcl_Obj *o, int depth) {
    uint32_t tag = R_U32(r);
    Indent(o, depth);
    Appendf(o, "- literal tag=%u (%s)\n", (unsigned)tag, LitTagName(tag));

    switch (tag) {
    case TBCX_LIT_BIGNUM: {
        uint8_t        sign   = R_U8(r);
        uint32_t       magLen = R_U32(r);
        unsigned char *mag    = NULL;
        if (magLen > 0) {
            mag = (unsigned char *)Tcl_Alloc(magLen);
            if (!R_Bytes(r, mag, magLen)) {
                if (mag)
                    Tcl_Free((char *)mag);
                return;
            }
        }
        Indent(o, depth + 1);
        Appendf(o, "sign=%u, magLE_len=%u, magLE=", (unsigned)sign, (unsigned)magLen);
        if (magLen)
            AppendHexPreview(o, mag, magLen, 64);
        Tcl_AppendToObj(o, "\n", 1);
        if (mag)
            Tcl_Free((char *)mag);
        break;
    }
    case TBCX_LIT_BOOLEAN: {
        uint8_t b = R_U8(r);
        Indent(o, depth + 1);
        Appendf(o, "value=%s\n", b ? "true" : "false");
        break;
    }
    case TBCX_LIT_BYTEARR: {
        uint32_t n = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "length=%u, bytes=", (unsigned)n);
        if (n) {
            unsigned char *buf = (unsigned char *)Tcl_Alloc(n);
            if (!R_Bytes(r, buf, n)) {
                Tcl_Free((char *)buf);
                return;
            }
            AppendHexPreview(o, buf, n, 64);
            Tcl_Free((char *)buf);
        }
        Tcl_AppendToObj(o, "\n", 1);
        break;
    }
    case TBCX_LIT_DICT: {
        uint32_t pairs = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "entries=%u\n", (unsigned)pairs);
        for (uint32_t i = 0; i < pairs; i++) {
            Indent(o, depth + 1);
            Appendf(o, "key[%u]:\n", i);
            DumpLiteral(r, o, depth + 2);
            Indent(o, depth + 1);
            Appendf(o, "val[%u]:\n", i);
            DumpLiteral(r, o, depth + 2);
        }
        break;
    }
    case TBCX_LIT_DOUBLE: {
        uint64_t bits = R_U64(r);
        union {
            uint64_t u;
            double   d;
        } u;
        u.u = bits;
        Indent(o, depth + 1);
        Appendf(o, "value=%.17g (bits=0x%016llX)\n", u.d, (unsigned long long)bits);
        break;
    }
    case TBCX_LIT_LIST: {
        uint32_t n = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "elements=%u\n", (unsigned)n);
        for (uint32_t i = 0; i < n; i++) {
            Indent(o, depth + 1);
            Appendf(o, "elem[%u]:\n", i);
            DumpLiteral(r, o, depth + 2);
        }
        break;
    }
    case TBCX_LIT_STRING: {
        uint32_t ln = 0;
        char    *s  = R_LPString(r, &ln);
        if (!s)
            return;
        Indent(o, depth + 1);
        Tcl_AppendToObj(o, "value=", -1);
        AppendQuoted(o, s, ln, 256);
        Tcl_AppendToObj(o, "\n", 1);
        Tcl_Free(s);
        break;
    }
    case TBCX_LIT_WIDEINT: {
        int64_t v = (int64_t)R_U64(r);
        Indent(o, depth + 1);
        Appendf(o, "value=%lld\n", (long long)v);
        break;
    }
    case TBCX_LIT_WIDEUINT: {
        uint64_t v = R_U64(r);
        Indent(o, depth + 1);
        Appendf(o, "value=%llu\n", (unsigned long long)v);
        break;
    }
    case TBCX_LIT_LAMBDA_BC: {
        uint32_t nsLen = 0;
        char    *ns    = R_LPString(r, &nsLen);
        if (!ns)
            return;
        Indent(o, depth + 1);
        Tcl_AppendToObj(o, "ns=", -1);
        AppendQuoted(o, ns, nsLen, 256);
        Tcl_AppendToObj(o, "\n", 1);
        Tcl_Free(ns);

        uint32_t numArgs = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "args=%u\n", (unsigned)numArgs);
        for (uint32_t i = 0; i < numArgs; i++) {
            uint32_t ln   = 0;
            char    *name = R_LPString(r, &ln);
            if (!name)
                return;
            uint8_t hasDef = R_U8(r);
            Indent(o, depth + 2);
            Tcl_AppendToObj(o, "arg: ", -1);
            AppendQuoted(o, name, ln, 256);
            Tcl_AppendToObj(o, "\n", 1);
            Tcl_Free(name);
            if (hasDef) {
                Indent(o, depth + 2);
                Tcl_AppendToObj(o, "default:\n", -1);
                DumpLiteral(r, o, depth + 3);
            }
        }
        Indent(o, depth + 1);
        Tcl_AppendToObj(o, "body[compiled]:\n", -1);
        DumpCompiledBlock(r, o, depth + 2, NULL, NULL, NULL, NULL, NULL, NULL);
        break;
    }
    case TBCX_LIT_BYTECODE: {
        uint32_t nsLen = 0;
        char    *ns    = R_LPString(r, &nsLen);
        if (!ns)
            return;
        Indent(o, depth + 1);
        Tcl_AppendToObj(o, "ns=", -1);
        AppendQuoted(o, ns, nsLen, 256);
        Tcl_AppendToObj(o, "\n", 1);
        Tcl_Free(ns);

        Indent(o, depth + 1);
        Tcl_AppendToObj(o, "body[compiled]:\n", -1);
        DumpCompiledBlock(r, o, depth + 2, NULL, NULL, NULL, NULL, NULL, NULL);
        break;
    }
    default: {
        Indent(o, depth + 1);
        Appendf(o, "unrecognized literal tag %u\n", (unsigned)tag);
        r->err = TCL_ERROR;
        break;
    }
    }
}

static void DumpAuxData(TbcxIn *r, Tcl_Obj *o, int depth) {
    uint32_t tag = R_U32(r);
    Indent(o, depth);
    Appendf(o, "- aux tag=%u (%s)\n", (unsigned)tag, AuxTagName(tag));

    switch (tag) {
    case TBCX_AUX_JT_STR: {
        uint32_t cnt = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "entries=%u\n", (unsigned)cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t ln  = 0;
            char    *key = R_LPString(r, &ln);
            if (!key)
                return;
            uint32_t off = R_U32(r);
            Indent(o, depth + 2);
            Tcl_AppendToObj(o, "key=", -1);
            AppendQuoted(o, key, ln, 256);
            Appendf(o, " -> pc=%u\n", (unsigned)off);
            Tcl_Free(key);
        }
        break;
    }
    case TBCX_AUX_JT_NUM: {
        uint32_t cnt = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "entries=%u\n", (unsigned)cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            uint64_t key = R_U64(r);
            uint32_t off = R_U32(r);
            Indent(o, depth + 2);
            Appendf(o, "key=%lld -> pc=%u\n", (long long)(int64_t)key, (unsigned)off);
        }
        break;
    }
    case TBCX_AUX_DICTUPD: {
        uint32_t len = R_U32(r);
        Indent(o, depth + 1);
        Appendf(o, "length=%u, varIndices=", (unsigned)len);
        for (uint32_t i = 0; i < len; i++) {
            uint32_t idx = R_U32(r);
            Appendf(o, "%s%u", (i ? "," : "["), (unsigned)idx);
        }
        Tcl_AppendToObj(o, "]\n", 2);
        break;
    }
    case TBCX_AUX_NEWFORE:
    case TBCX_AUX_FOREACH: {
        uint32_t numLists       = R_U32(r);
        uint32_t loopCtTemp     = R_U32(r);
        uint32_t firstValueTemp = R_U32(r);
        uint32_t numListsRpt    = R_U32(r); /* saver emits this once more */
        Indent(o, depth + 1);
        Appendf(o, "numLists=%u (%u rpt), loopCtTemp=%u, firstValueTemp=%u\n", (unsigned)numLists, (unsigned)numListsRpt, (unsigned)loopCtTemp, (unsigned)firstValueTemp);
        for (uint32_t L = 0; L < numLists; L++) {
            uint32_t nvars = R_U32(r);
            Indent(o, depth + 2);
            Appendf(o, "list[%u] vars=%u: ", (unsigned)L, (unsigned)nvars);
            for (uint32_t v = 0; v < nvars; v++) {
                uint32_t idx = R_U32(r);
                Appendf(o, "%s%u", (v ? "," : "["), (unsigned)idx);
            }
            Tcl_AppendToObj(o, "]\n", 2);
        }
        break;
    }
    default:
        Indent(o, depth + 1);
        Appendf(o, "unrecognized aux tag %u\n", (unsigned)tag);
        r->err = TCL_ERROR;
        break;
    }
}

static void DumpExceptions(TbcxIn *r, Tcl_Obj *o, int depth, uint32_t num) {
    for (uint32_t i = 0; i < num; i++) {
        uint32_t type32  = R_U32(r);
        uint32_t nesting = R_U32(r);
        uint32_t from    = R_U32(r);
        uint32_t len     = R_U32(r);
        uint32_t cont    = R_U32(r);
        uint32_t brk     = R_U32(r);
        uint32_t cat     = R_U32(r);
        uint32_t toExcl  = from + len;
        Indent(o, depth);
        Appendf(o, "- except[%u]: type=%u nesting=%u code=[%u..%u) (len=%u) cont=%u break=%u catch=%u\n", (unsigned)i, (unsigned)(uint8_t)type32, (unsigned)nesting, (unsigned)from, (unsigned)toExcl,
                (unsigned)len, (unsigned)cont, (unsigned)brk, (unsigned)cat);
    }
}

static void DumpCodePreview(TbcxIn *r, Tcl_Obj *o, int depth, uint32_t codeLen) {
    uint32_t preview = codeLen < 64u ? codeLen : 64u;
    if (preview == 0) {
        Indent(o, depth);
        Tcl_AppendToObj(o, "code: <empty>\n", -1);
        return;
    }
    unsigned char buf[64];
    if (!R_Bytes(r, buf, preview))
        return;
    Indent(o, depth);
    Tcl_AppendToObj(o, "code: ", -1);
    AppendHexPreview(o, buf, preview, preview);
    if (codeLen > preview)
        Appendf(o, " …(+%u bytes)", (unsigned)(codeLen - preview));
    Tcl_AppendToObj(o, "\n", 1);
    if (codeLen > preview)
        SkipBytes(r, (Tcl_WideInt)(codeLen - preview));
}

static void DumpCompiledBlock(TbcxIn *r, Tcl_Obj *o, int depth, uint32_t *out_codeLen, uint32_t *out_numLits, uint32_t *out_numAux, uint32_t *out_numExcept, uint32_t *out_numLocals,
                              uint32_t *out_maxStack) {
    uint32_t codeLen = R_U32(r);
    Indent(o, depth);
    Appendf(o, "codeLen=%u\n", (unsigned)codeLen);
    DumpCodePreview(r, o, depth + 1, codeLen);

    uint32_t numLits = R_U32(r);
    Indent(o, depth);
    Appendf(o, "literals=%u\n", (unsigned)numLits);
    for (uint32_t i = 0; i < numLits; i++) {
        Indent(o, depth + 1);
        Appendf(o, "lit[%u]:\n", (unsigned)i);
        DumpLiteral(r, o, depth + 2);
        if (r->err)
            return;
    }

    uint32_t numAux = R_U32(r);
    Indent(o, depth);
    Appendf(o, "auxData=%u\n", (unsigned)numAux);
    for (uint32_t i = 0; i < numAux; i++) {
        DumpAuxData(r, o, depth + 1);
        if (r->err)
            return;
    }

    uint32_t numExcept = R_U32(r);
    Indent(o, depth);
    Appendf(o, "exceptions=%u\n", (unsigned)numExcept);
    DumpExceptions(r, o, depth + 1, numExcept);
    if (r->err)
        return;

    uint32_t maxStack  = R_U32(r);
    uint32_t reserved  = R_U32(r);
    uint32_t numLocals = R_U32(r);
    Indent(o, depth);
    Appendf(o, "epilogue: maxStack=%u reserved=%u numLocals=%u\n", (unsigned)maxStack, (unsigned)reserved, (unsigned)numLocals);

    if (out_codeLen)
        *out_codeLen = codeLen;
    if (out_numLits)
        *out_numLits = numLits;
    if (out_numAux)
        *out_numAux = numAux;
    if (out_numExcept)
        *out_numExcept = numExcept;
    if (out_numLocals)
        *out_numLocals = numLocals;
    if (out_maxStack)
        *out_maxStack = maxStack;
}

/* ==========================================================================
 * Tcl command
 * ========================================================================== */

int Tbcx_DumpFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "in.tbcx");
        return TCL_ERROR;
    }

    Tcl_Channel in = Tcl_FSOpenFileChannel(interp, objv[1], "r", 0);
    if (!in)
        return TCL_ERROR;
    if (CheckBinaryChan(interp, in) != TCL_OK) {
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    TbcxIn   r   = {interp, in, TCL_OK, 0};
    Tcl_Obj *out = Tcl_NewObj();
    Tcl_IncrRefCount(out);

    /* Header */
    uint32_t magic   = R_U32(&r);
    uint32_t format  = R_U32(&r);
    uint32_t ver     = R_U32(&r);
    uint64_t codeTop = R_U64(&r);
    uint32_t nCmdsT  = R_U32(&r);
    uint32_t nExT    = R_U32(&r);
    uint32_t nLitT   = R_U32(&r);
    uint32_t nAuxT   = R_U32(&r);
    uint32_t nLocT   = R_U32(&r);
    uint32_t maxST   = R_U32(&r);

    Appendf(out, "TBCX header: magic=0x%08X format=%u\n", magic, format);
    Appendf(out, "tcl_version=0x%08X\n", ver);
    Appendf(out, "top: codeLen=%llu numCmds=%u numExcept=%u numLits=%u numAux=%u numLocals=%u maxStack=%u\n", (unsigned long long)codeTop, (unsigned)nCmdsT, (unsigned)nExT, (unsigned)nLitT,
            (unsigned)nAuxT, (unsigned)nLocT, (unsigned)maxST);

    if (magic != TBCX_MAGIC) {
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx::dump: bad magic", -1));
        return TCL_ERROR;
    }
    if (format != TBCX_FORMAT) {
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx::dump: unsupported format", -1));
        return TCL_ERROR;
    }

    /* Top-level compiled block */
    Tcl_AppendToObj(out, "\n== Top-level block ==\n", -1);
    uint32_t b_code = 0, b_lits = 0, b_aux = 0, b_exc = 0, b_loc = 0, b_stk = 0;
    DumpCompiledBlock(&r, out, 0, &b_code, &b_lits, &b_aux, &b_exc, &b_loc, &b_stk);
    if (r.err) {
        Tcl_DecrRefCount(out);
        Tcl_Close(interp, in);
        return TCL_ERROR;
    }

    /* Cross-check header vs block */
    if ((uint64_t)b_code != codeTop || b_exc != nExT || b_lits != nLitT || b_aux != nAuxT || b_loc != nLocT || b_stk != maxST) {
        Tcl_AppendToObj(out, "!! header/block count mismatch detected\n", -1);
    }

    /* Procs */
    Tcl_AppendToObj(out, "\n== Procs ==\n", -1);
    uint32_t numProcs = R_U32(&r);
    Appendf(out, "count=%u\n", (unsigned)numProcs);
    for (uint32_t i = 0; i < numProcs; i++) {
        uint32_t ln   = 0;
        char    *name = R_LPString(&r, &ln);
        if (!name)
            goto io_fail;
        uint32_t lns = 0;
        char    *ns  = R_LPString(&r, &lns);
        if (!ns) {
            Tcl_Free(name);
            goto io_fail;
        }
        uint32_t lna  = 0;
        char    *args = R_LPString(&r, &lna);
        if (!args) {
            Tcl_Free(name);
            Tcl_Free(ns);
            goto io_fail;
        }

        Appendf(out, "- proc[%u]\n", (unsigned)i);
        Indent(out, 1);
        Tcl_AppendToObj(out, "name=", -1);
        AppendQuoted(out, name, ln, 256);
        Tcl_AppendToObj(out, "\n", -1);
        Indent(out, 1);
        Tcl_AppendToObj(out, "ns=", -1);
        AppendQuoted(out, ns, lns, 256);
        Tcl_AppendToObj(out, "\n", -1);
        Indent(out, 1);
        Tcl_AppendToObj(out, "args=", -1);
        AppendQuoted(out, args, lna, 256);
        Tcl_AppendToObj(out, "\n", -1);
        Tcl_Free(name);
        Tcl_Free(ns);
        Tcl_Free(args);

        Indent(out, 1);
        Tcl_AppendToObj(out, "body:\n", -1);
        DumpCompiledBlock(&r, out, 2, NULL, NULL, NULL, NULL, NULL, NULL);
        if (r.err)
            goto io_fail;
    }

    /* Classes */
    Tcl_AppendToObj(out, "\n== Classes ==\n", -1);
    uint32_t numClasses = R_U32(&r);
    Appendf(out, "count=%u\n", (unsigned)numClasses);
    for (uint32_t c = 0; c < numClasses; c++) {
        uint32_t ln  = 0;
        char    *cls = R_LPString(&r, &ln);
        if (!cls)
            goto io_fail;
        Appendf(out, "- class[%u] ", (unsigned)c);
        AppendQuoted(out, cls, ln, 256);
        Tcl_AppendToObj(out, "\n", 1);
        Tcl_Free(cls);
        uint32_t nSup = R_U32(&r);
        Indent(out, 1);
        Appendf(out, "superclasses=%u\n", (unsigned)nSup);
        for (uint32_t s = 0; s < nSup; s++) {
            uint32_t ls  = 0;
            char    *sup = R_LPString(&r, &ls);
            if (!sup)
                goto io_fail;
            Indent(out, 2);
            Tcl_AppendToObj(out, "super=", -1);
            AppendQuoted(out, sup, ls, 256);
            Tcl_AppendToObj(out, "\n", 1);
            Tcl_Free(sup);
        }
    }

    /* Methods */
    Tcl_AppendToObj(out, "\n== Methods ==\n", -1);
    uint32_t numMethods = R_U32(&r);
    Appendf(out, "count=%u\n", (unsigned)numMethods);
    for (uint32_t m = 0; m < numMethods; m++) {
        uint32_t ln  = 0;
        char    *cls = R_LPString(&r, &ln);
        if (!cls)
            goto io_fail;
        uint8_t  kind = R_U8(&r);
        uint32_t lnm  = 0;
        char    *name = R_LPString(&r, &lnm);
        if (!name) {
            Tcl_Free(cls);
            goto io_fail;
        }
        uint32_t lna  = 0;
        char    *args = R_LPString(&r, &lna);
        if (!args) {
            Tcl_Free(cls);
            Tcl_Free(name);
            goto io_fail;
        }
        uint32_t bodyTextLen = R_U32(&r);

        Appendf(out, "- method[%u] class=", (unsigned)m);
        AppendQuoted(out, cls, ln, 256);
        Appendf(out, " kind=%s\n", MethKindName(kind));
        Tcl_Free(cls);

        Indent(out, 1);
        Tcl_AppendToObj(out, "name=", -1);
        AppendQuoted(out, name, lnm, 256);
        Tcl_AppendToObj(out, "\n", -1);
        Tcl_Free(name);
        Indent(out, 1);
        Tcl_AppendToObj(out, "args=", -1);
        AppendQuoted(out, args, lna, 256);
        Tcl_AppendToObj(out, "\n", -1);
        Tcl_Free(args);

        if (bodyTextLen > 0) {
            /* Writer currently emits 0; still handle if present */
            char *txt = (char *)Tcl_Alloc(bodyTextLen + 1u);
            if (!R_Bytes(&r, txt, bodyTextLen)) {
                Tcl_Free(txt);
                goto io_fail;
            }
            txt[bodyTextLen] = '\0';
            Indent(out, 1);
            Tcl_AppendToObj(out, "bodyText=", -1);
            AppendQuoted(out, txt, bodyTextLen, 256);
            Tcl_AppendToObj(out, "\n", -1);
            Tcl_Free(txt);
        }

        Indent(out, 1);
        Tcl_AppendToObj(out, "body:\n", -1);
        DumpCompiledBlock(&r, out, 2, NULL, NULL, NULL, NULL, NULL, NULL);
        if (r.err)
            goto io_fail;
    }

    if (Tcl_Close(interp, in) != TCL_OK) {
        Tcl_DecrRefCount(out);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, out);
    Tcl_DecrRefCount(out);
    return TCL_OK;

io_fail:
    Tcl_DecrRefCount(out);
    Tcl_Close(interp, in);
    return TCL_ERROR;
}
