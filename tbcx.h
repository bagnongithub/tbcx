/* ==========================================================================
 * tbcx.h - TBCX shared declarations for Tcl 9.1
 * ========================================================================== */

#ifndef TBCX_H
#define TBCX_H

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef USE_TCL_STUBS
#define USE_TCL_STUBS
#endif

#include "tcl.h"
#include "tclCompile.h"
#include "tclInt.h"
#include "tclIntDecls.h"
#include "tclOO.h"
#include "tclOOInt.h"
#include "tclTomMath.h"

#define TBCX_MAGIC 0x58434254u
/* TBCX_FORMAT 92u — current on-wire format for Tcl 9.1.
 *
 * Each proc record and each method record carries an LPString body-source
 * field immediately before its compiled block.  The loader attaches that
 * text as the body Tcl_Obj's string rep (via Tcl_InitStringRep) without
 * touching the ByteCode internal rep.  By default the body source is
 * STRIPPED from the artifact: bodies are stored as "" on the wire and the
 * loader substitutes the diagnostic sentinel TBCX_STRIPPED_SOURCE_SENTINEL,
 * so `info body` returns a loud string instead of silently returning empty.
 * The save-time flag `-include-source` instead stores the original body
 * text as an LPString, preserving byte-for-byte `info body` round-trip for
 * artifacts where introspection idioms (cloneRule, info class definition,
 * TIP #280 source attribution, etc.) must work.
 *
 * Each method record carries a `scope` u8 immediately AFTER `kind` and
 * before `name` (see TBCX_MSCOPE_*); it records the TclOO visibility the
 * method must have after load — public (exported), unexported, or TIP 500
 * true-private.  Ctor/dtor records always carry DEFAULT.  An `origin` u8
 * follows `scope` (see TBCX_MORIGIN_*), distinguishing a class-instance
 * method from a per-object method that shares the same FQN/kind/name.
 *
 * Proc and method bodies are matched POSITIONALLY at load: records for a
 * given key are kept in a definition-order FIFO and each definition site
 * consumes the front record.  Rewritten stub bodies use the recognizable
 * TBCX_METH_STUB_BODY / TBCX_PROC_MARKER_PFX sentinels so a verbatim
 * (non-literal) body is never patched from a stale same-key record. */
#define TBCX_FORMAT 92u

/* Method visibility scope (method record `scope` u8).  Mirrors the
 * TclOO SCOPE_FLAGS (PUBLIC_METHOD / unexported / TRUE_PRIVATE_METHOD),
 * decoupled from the internal enum values so the wire format is stable
 * regardless of tclOOInt.h.  Ctor/dtor records always carry DEFAULT. */
#define TBCX_MSCOPE_DEFAULT 0u      /* visibility from method-name case      */
#define TBCX_MSCOPE_PUBLIC 1u       /* exported (forced public)              */
#define TBCX_MSCOPE_UNEXPORTED 2u   /* unexported (forced private-by-export) */
#define TBCX_MSCOPE_TRUE_PRIVATE 3u /* TIP 500 true-private                  */

/* Save-time flags (Tbcx_SaveObjCmd → EmitTbcxStream).  Passed through
 * TbcxCtx.saveFlags and consulted where per-site behavior changes.
 * Wire format is unaffected by the flags themselves — they only alter
 * what the save side chooses to emit. */
#define TBCX_SAVE_FL_INCLUDE_SOURCE 0x1u /* write original body srcText as
                                            LPString for every proc/method;
                                            the opposite of tclcompiler/
                                            tbcload's source-strip-always
                                            default.  Without this flag,
                                            bodies are emitted as "" on the
                                            wire and the loader installs
                                            the diagnostic sentinel. */

/* Diagnostic sentinel installed as body string-rep when the artifact was
 * written without -include-source (the default).  Two-line shape matches
 * ActiveState TclPro tclcompiler's canonical pattern (Compiler8.html,
 * "Example 1: Cloning Procedures"), validated by 25 years of production
 * use in TclPro/ActiveTcl/TclDevKit:
 *
 *     1. A `#` comment line that shows up as the top of any error
 *        traceback, making the cause visible at a glance without
 *        requiring readers to parse the error message.
 *     2. An `error` call so that if downstream code round-trips this
 *        body — e.g.
 *            proc new {} [info body original]
 *            new
 *        — the resulting proc fails loudly at invocation rather than
 *        silently running as a no-op.
 *
 * Newline separator between the two statements; bodies become two-
 * statement scripts. `info body`, `info frame`, and error traces all
 * display this string directly, so the diagnostic is visible in
 * every consumption path. */
#define TBCX_STRIPPED_SOURCE_SENTINEL                                                \
    "# tbcx: body source stripped at save time; info body unavailable\n"             \
    "error \"tbcx: introspection-based cloning is not supported for this artifact\""

/* Thread-ownership assertion.  Verifies that the current thread is the
 * one that created the interpreter, which is the only thread permitted
 * to use it under Tcl's threading model.  Active only in debug builds
 * (NDEBUG not defined) unless TBCX_THREAD_CHECKS is also defined.
 * Fires assert() on violation — use for internal helpers and void-
 * returning callbacks where TCL_ERROR cannot be propagated. */
#if defined(TBCX_THREAD_CHECKS) || !defined(NDEBUG)
#define TBCX_ASSERT_INTERP_THREAD(ip)                                                                                                                                                                  \
    do {                                                                                                                                                                                               \
        Interp *_tbcx_iPtr = (Interp *)(ip);                                                                                                                                                           \
        assert(_tbcx_iPtr->threadId == Tcl_GetCurrentThread() && "TBCX: must be called from the interp-owning thread");                                                                                \
    } while (0)
#else
#define TBCX_ASSERT_INTERP_THREAD(ip) ((void)0)
#endif

/* Thread-ownership check for Tcl command entry points.  Always active
 * in all builds (debug and release).  Returns TCL_ERROR with a
 * diagnostic message instead of crashing — use at the four public
 * Tcl commands (save, load, dump, gc) where the caller can handle
 * the error through the normal Tcl result channel.
 *
 * Cost: one pointer comparison + one Tcl_GetCurrentThread() call per
 * command invocation — negligible relative to I/O and compilation. */
#define TBCX_CHECK_INTERP_THREAD(ip)                                                                                                                                                                   \
    do {                                                                                                                                                                                               \
        if (((Interp *)(ip))->threadId != Tcl_GetCurrentThread()) {                                                                                                                                    \
            Tcl_SetObjResult((ip), Tcl_NewStringObj("tbcx: called from non-owning thread", -1));                                                                                                       \
            return TCL_ERROR;                                                                                                                                                                          \
        }                                                                                                                                                                                              \
    } while (0)

#define TBCX_LIT_BIGNUM 0u
#define TBCX_LIT_BOOLEAN 1u
#define TBCX_LIT_BYTEARR 2u
#define TBCX_LIT_DICT 3u
#define TBCX_LIT_DOUBLE 4u
#define TBCX_LIT_LIST 5u
#define TBCX_LIT_STRING 6u
#define TBCX_LIT_WIDEINT 7u
#define TBCX_LIT_WIDEUINT 8u
#define TBCX_LIT_LAMBDA_BC 9u
#define TBCX_LIT_BYTESRC 10u

#define TBCX_AUX_JT_STR 0u
#define TBCX_AUX_JT_NUM 1u
#define TBCX_AUX_DICTUPD 2u
#define TBCX_AUX_NEWFORE 3u

#define TBCX_METH_INST 0u
#define TBCX_METH_CLASS 1u
#define TBCX_METH_CTOR 2u
#define TBCX_METH_DTOR 3u
#define TBCX_METH_SELF 4u    /* self method (class-level, installed via oo::define builder body) */
#define TBCX_METH_NONE 0xFFu /* sentinel: no method kind identified */

/* Method-record origin (wire u8, immediately after the scope byte).  Records
 * are keyed at load by (classFqn, kind, name, origin); the origin lets a
 * class-instance method and a per-object method that share an FQN/kind/name
 * (e.g. on a class object) coexist instead of colliding. */
#define TBCX_MORIGIN_CLASS 0u  /* captured from oo::define / oo::class      */
#define TBCX_MORIGIN_OBJECT 1u /* captured from oo::objdefine               */

/* Per-object/class method STUB body.  The save side replaces a captured
 * method's body with this exact no-op comment in the rewritten builder/stub;
 * the loader patches a method's precompiled body ONLY when the forwarded body
 * equals this sentinel.  A method left VERBATIM by the saver (non-literal
 * name/args/body, or a runtime-variable target) keeps its real body, which is
 * never this sentinel, so it is never overwritten by a precompiled record —
 * even one belonging to another builder that defined the same method name on
 * the same object.  The leading "#\x01" makes it a comment (a no-op if it ever
 * runs before the swap) that no authored body would ever match. */
#define TBCX_METH_STUB_BODY "#\x01tbcx-method-stub"
#define TBCX_METH_STUB_BODY_LEN (sizeof(TBCX_METH_STUB_BODY) - 1)

/* Indexed proc marker prefix.  The save side emits stub bodies of the
   form "\x01TBCX<decimal-index>" so that the load-side ProcShim can
   match by position rather than by FQN — correctly handling conflicting
   proc definitions across if/else branches. */
#define TBCX_PROC_MARKER_PFX "\x01TBCX"
#define TBCX_PROC_MARKER_PFX_LEN 5

typedef struct TbcxHeader {
    uint32_t magic;       /* "TBCX" */
    uint32_t format;      /* format version */
    uint32_t tcl_version; /* mmjjppTT */
    uint64_t codeLenTop;
    uint32_t numExceptTop;
    uint32_t numLitsTop;
    uint32_t numAuxTop;
    uint32_t numLocalsTop;
    uint32_t maxStackTop;
    /* Staging fields — NOT serialized as fixed-size.  sourcePath is
     * written/read as an LPString immediately after maxStackTop. */
    Tcl_Obj *sourcePath;  /* LPString; NULL or empty = no path */
} TbcxHeader;

extern _Atomic int tbcxHostIsLE;

uint32_t           Tbcx_PackTclVersion(void);

/* Wire-format caps — production-safe defaults for untrusted input.
 * These limit the maximum values the deserializer will accept from a
 * .tbcx stream.  Combined with Tcl_AttemptAlloc for wire-derived sizes,
 * hostile files cannot crash the process via OOM/panic. */
#define TBCX_MAX_CODE (64u * 1024u * 1024u)
#define TBCX_MAX_LITERALS (1u * 1024u * 1024u)
#define TBCX_MAX_AUX (1u * 1024u * 1024u)
#define TBCX_MAX_EXCEPT (1u * 1024u * 1024u)
#define TBCX_MAX_STR (4u * 1024u * 1024u)
#define TBCX_MAX_LOCALS 65536u
#define TBCX_MAX_STACK 65536u
#define TBCX_MAX_PROCS (256u * 1024u)
#define TBCX_MAX_CLASSES (256u * 1024u)
#define TBCX_MAX_METHODS (256u * 1024u)

#define TBCX_BUFSIZE (64u * 1024u)

/* Hardened Tcl object accessors.
 *
 * Tcl string/bytearray accessors can return NULL on corrupted or
 * pure-bytearray objects.  These helpers guarantee a non-NULL pointer
 * and zero length on failure so callers can safely treat the value as
 * an empty string/byte-array in non-fatal scanning/rewrite paths.
 */
static inline const char *Tbcx_GetStringFromObjSafe(Tcl_Obj *objPtr, Tcl_Size *lenPtr) {
    const char *s;

    if (lenPtr)
        *lenPtr = 0;
    if (!objPtr)
        return "";
    s = Tcl_GetStringFromObj(objPtr, lenPtr);
    if (!s) {
        if (lenPtr)
            *lenPtr = 0;
        return "";
    }
    return s;
}

static inline const char *Tbcx_GetStringSafe(Tcl_Obj *objPtr) {
    const char *s;

    if (!objPtr)
        return "";
    s = Tcl_GetString(objPtr);
    return s ? s : "";
}

static inline unsigned char *Tbcx_GetByteArrayFromObjSafe(Tcl_Obj *objPtr, Tcl_Size *lenPtr) {
    /* const: sentinel byte must never be written through the returned pointer.
     * Cast to non-const on return to match Tcl_GetByteArrayFromObj signature;
     * callers that only need the bytes for reading are safe. */
    static const unsigned char emptyByte = 0;
    unsigned char             *p;

    if (lenPtr)
        *lenPtr = 0;
    if (!objPtr)
        return (unsigned char *)&emptyByte;
    p = Tcl_GetByteArrayFromObj(objPtr, lenPtr);
    if (!p) {
        if (lenPtr)
            *lenPtr = 0;
        return (unsigned char *)&emptyByte;
    }
    return p;
}

/* Strict Tcl object accessors for identifier-critical contexts.
 *
 * Unlike the *Safe helpers above (which return "" on failure for scanning
 * paths), these return NULL and set an interpreter error result.  Use them
 * wherever an empty-string substitution would silently corrupt identifiers
 * (namespace FQNs, proc/method/class names, shim-name construction, wire-
 * format keys).  Callers must check the return value and propagate error. */
static inline const char *Tbcx_GetStringFromObjStrict(Tcl_Interp *ip, Tcl_Obj *o, Tcl_Size *len) {
    const char *s;
    if (!o) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    s = Tcl_GetStringFromObj(o, len);
    if (!s) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object", -1));
        return NULL;
    }
    return s;
}

static inline const char *Tbcx_GetStringStrict(Tcl_Interp *ip, Tcl_Obj *o) {
    const char *s;
    if (!o) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    s = Tcl_GetString(o);
    if (!s) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object", -1));
        return NULL;
    }
    return s;
}

static inline unsigned char *Tbcx_GetByteArrayFromObjStrict(Tcl_Interp *ip, Tcl_Obj *o, Tcl_Size *len) {
    unsigned char *p;
    if (!o) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid bytearray object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    p = Tcl_GetByteArrayFromObj(o, len);
    if (!p) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid bytearray object", -1));
        return NULL;
    }
    return p;
}

/* Validate a key string for use with namespace/hash APIs.
 * Rejects embedded NULs and optionally requires absolute namespace form.
 * Returns TCL_OK on success, TCL_ERROR with interp result set on failure. */
static inline int Tbcx_ValidateKeyString(Tcl_Interp *ip, const char *s, Tcl_Size len, const char *what, int requireAbsoluteNs) {
    if (!s) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: missing %s", what));
        return TCL_ERROR;
    }
    if (len > 0 && memchr(s, '\0', (size_t)len) != NULL) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: %s contains embedded NUL", what));
        return TCL_ERROR;
    }
    if (requireAbsoluteNs && !(len >= 2 && s[0] == ':' && s[1] == ':')) {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: %s must be absolute", what));
        return TCL_ERROR;
    }
    return TCL_OK;
}

extern const Tcl_ObjType *tbcxTyBignum;
extern const Tcl_ObjType *tbcxTyBoolean;
extern const Tcl_ObjType *tbcxTyByteArray;
extern const Tcl_ObjType *tbcxTyBytecode;

/* TbcxGetByteCode — extract ByteCode* from a Tcl_Obj using the runtime
 * type pointer (tbcxTyBytecode), NOT the compile-time &tclByteCodeType.
 *
 * ByteCodeGetInternalRep (from tclCompile.h) uses &tclByteCodeType, which
 * is resolved at LINK time.  In stubs-linked extensions (especially on
 * macOS with two-level namespace), this symbol may resolve to a different
 * address than what Tcl_GetObjType("bytecode") returns at RUNTIME.  When
 * they differ, ByteCodeGetInternalRep silently returns NULL, causing all
 * procPtr/epoch fixups to be skipped — leading to crashes on coroutine
 * yield/resume (procPtr=NULL, SIGSEGV at offset 0x30). */
static inline ByteCode   *TbcxGetByteCode(Tcl_Obj *objPtr) {
    const Tcl_ObjInternalRep *irPtr = Tcl_FetchInternalRep(objPtr, tbcxTyBytecode);
    return irPtr ? (ByteCode *)irPtr->twoPtrValue.ptr1 : NULL;
}

/* Flags for TbcxFixupByteCode cache handling strategy. */
#define TBCX_FIXUP_CACHE_KEEP 0 /* Preserve + fix LocalCache (methods, lambdas) */
#define TBCX_FIXUP_CACHE_DROP 1 /* NULL out LocalCache (procs — Tcl rebuilds) */
#define TBCX_FIXUP_CACHE_NONE 2 /* Don't touch LocalCache */

extern const Tcl_ObjType *tbcxTyDict;
extern const Tcl_ObjType *tbcxTyDouble;
extern const Tcl_ObjType *tbcxTyInt;
extern const Tcl_ObjType *tbcxTyLambda;
extern const Tcl_ObjType *tbcxTyList;
extern const Tcl_ObjType *tbcxTyProcBody;

extern const AuxDataType *tbcxAuxJTStr;
extern const AuxDataType *tbcxAuxJTNum;
extern const AuxDataType *tbcxAuxDictUpdate;
extern const AuxDataType *tbcxAuxNewForeach;

/* ==========================================================================
 * Buffered I/O wrapper types
 * ========================================================================== */

typedef struct TbcxIn {
    Tcl_Interp   *interp;
    Tcl_Channel   chan;
    int           err;
    unsigned char buf[TBCX_BUFSIZE];
    Tcl_Size      bufPos;  /* next byte to consume */
    Tcl_Size      bufFill; /* valid bytes in buf */
} TbcxIn;

typedef struct {
    Tcl_Interp   *interp;
    Tcl_Channel   chan;
    int           err;
    unsigned char buf[TBCX_BUFSIZE];
    Tcl_Size      bufPos;     /* next free position in buf */
    uint64_t      totalBytes; /* total bytes written (buf flushes + current bufPos) */
} TbcxOut;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

int               Tbcx_BuildLocals(Tcl_Interp *ip, Tcl_Obj *argsList, CompiledLocal **firstOut, CompiledLocal **lastOut, Tcl_Size *numArgsOut);
int               Tbcx_CheckBinaryChan(Tcl_Interp *ip, Tcl_Channel ch);
Tcl_Namespace    *Tbcx_EnsureNamespace(Tcl_Interp *ip, const char *fqn);
void              Tbcx_FreeLocals(CompiledLocal *first);
int               Tbcx_ProbeOpenChannel(Tcl_Interp *interp, Tcl_Obj *obj, Tcl_Channel *chPtr);
int               Tbcx_ProbeReadableFile(Tcl_Interp *interp, Tcl_Obj *pathObj);
void              Tbcx_R_Init(TbcxIn *r, Tcl_Interp *ip, Tcl_Channel ch);
int               Tbcx_R_Bytes(TbcxIn *r, void *p, Tcl_Size n);
int               Tbcx_R_LPString(TbcxIn *r, char **sp, uint32_t *lenp);
int               Tbcx_R_U32(TbcxIn *r, uint32_t *vp);
int               Tbcx_R_U64(TbcxIn *r, uint64_t *vp);
int               Tbcx_R_U8(TbcxIn *r, uint8_t *v);
void              Tbcx_W_Init(TbcxOut *w, Tcl_Interp *ip, Tcl_Channel ch);
int               Tbcx_W_Flush(TbcxOut *w);
Tcl_Obj          *Tbcx_ReadBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut, int setPrecompiled, int dumpOnly);
int               Tbcx_ReadHeader(TbcxIn *r, TbcxHeader *H);
void              TbcxApplyShimPurgeAll(Tcl_Interp *ip);
void              TbcxFixupByteCode(ByteCode *bc, Proc *proc, Tcl_Interp *ip, Namespace *ns, int cacheMode);
int               TbcxVerifyLoadedBC(ByteCode *bc, Tcl_Interp *ip, const char *label);

/* Checked multiplication for allocation sizes.  Returns 1 on success
 * (result stored in *out), 0 if the multiplication would overflow size_t. */
static inline int tbcx_checked_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

#endif
