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
/* TBCX_FORMAT 93u is the on-wire format for Tcl 9.1. The loader accepts
 * format 93 artifacts and requires an exact Tcl producer-version match.
 *
 * The top-block policy word selects source-backed, strict compiled-only, or
 * include-source behavior. Strict artifacts contain empty proc, method,
 * BYTESRC, and lambda source LPStrings. Include-source artifacts retain those
 * bytes for introspection. Both nonzero policies execute PRECOMPILED material.
 * Stripped proc, method, and lambda objects receive diagnostic string
 * representations.
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
#define TBCX_FORMAT 93u

/* Format-93 top-block source policy. Nested block policy fields are zero. */
#define TBCX_BLOCK_POLICY_LEGACY 0u
#define TBCX_BLOCK_POLICY_STRICT_COMPILED_ONLY (1u << 0)
#define TBCX_BLOCK_POLICY_INCLUDE_SOURCE (1u << 1)
#define TBCX_BLOCK_POLICY_MASK (TBCX_BLOCK_POLICY_STRICT_COMPILED_ONLY | TBCX_BLOCK_POLICY_INCLUDE_SOURCE)

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
#define TBCX_SAVE_FL_INCLUDE_SOURCE                                                                                              \
    0x1u /* retain exact proc, method,                                                                                           \
            BYTESRC, and lambda source                                                                                           \
            LPStrings for introspection;                                                                                         \
            execution remains compiled */

/* Two-line string representation for proc and method bodies in strict
 * artifacts. Introspection returns this value, and evaluating a copied body
 * raises the diagnostic error on its second command. */
#define TBCX_STRIPPED_SOURCE_SENTINEL                                                                                            \
    "# tbcx: body source stripped at save time; info body unavailable\n"                                                         \
    "error \"tbcx: introspection-based cloning is not supported for this artifact\""

/* Diagnostic body used only for the in-memory string representation of a
 * strict lambda.  It is never serialized as executable source.  A new string
 * object reconstructed from the diagnostic lambda has no TBCX provenance and
 * therefore fails with a stable error instead of executing an empty body. */
#define TBCX_STRIPPED_LAMBDA_SENTINEL                                                                                            \
    "return -code error -errorcode {TBCX EVAL LAMBDA STRIPPED} "                                                                 \
    "{tbcx: stripped lambda identity was lost}"

/* Thread-ownership assertion.  Verifies that the current thread is the
 * one that created the interpreter, which is the only thread permitted
 * to use it under Tcl's threading model.  Active only in debug builds
 * (NDEBUG not defined) unless TBCX_THREAD_CHECKS is also defined.
 * Fires assert() on violation — use for internal helpers and void-
 * returning callbacks where TCL_ERROR cannot be propagated. */
#if defined(TBCX_THREAD_CHECKS) || !defined(NDEBUG)
#define TBCX_ASSERT_INTERP_THREAD(ip)                                                                                            \
    do                                                                                                                           \
    {                                                                                                                            \
        Interp* _tbcx_iPtr = (Interp*)(ip);                                                                                      \
        assert(_tbcx_iPtr->threadId == Tcl_GetCurrentThread() && "TBCX: must be called from the interp-owning thread");          \
    } while (0)
#else
#define TBCX_ASSERT_INTERP_THREAD(ip) ((void)0)
#endif

/* Thread-ownership check for Tcl command entry points. Always active in all
 * builds. Returns TCL_ERROR through the interpreter result channel. */
#define TBCX_CHECK_INTERP_THREAD(ip)                                                                                             \
    do                                                                                                                           \
    {                                                                                                                            \
        if (((Interp*)(ip))->threadId != Tcl_GetCurrentThread())                                                                 \
        {                                                                                                                        \
            Tcl_SetObjResult((ip), Tcl_NewStringObj("tbcx: called from non-owning thread", -1));                                 \
            return TCL_ERROR;                                                                                                    \
        }                                                                                                                        \
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

/* Method-body sentinel used in rewritten builders. The loader substitutes a
 * precompiled body only when the forwarded body equals this value. The
 * leading "#\x01" makes the sentinel a no-op Tcl comment. */
#define TBCX_METH_STUB_BODY "#\x01tbcx-method-stub"
#define TBCX_METH_STUB_BODY_LEN (sizeof(TBCX_METH_STUB_BODY) - 1)

/* Indexed proc marker prefix. Stub bodies have the form
 * "\x01TBCX<decimal-index>" and are matched positionally by ProcShim. */
#define TBCX_PROC_MARKER_PFX "\x01TBCX"
#define TBCX_PROC_MARKER_PFX_LEN 5

typedef struct TbcxHeader
{
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
    Tcl_Obj* sourcePath; /* LPString; NULL or empty = no path */
} TbcxHeader;

extern _Atomic int tbcxHostIsLE;

uint32_t Tbcx_PackTclVersion(void);

/* Maximum values accepted from a .tbcx stream. Wire-derived allocations use
 * Tcl_AttemptAlloc after checked size arithmetic. */
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
#define TBCX_MAX_IMAGE_BYTES (256u * 1024u * 1024u)

#define TBCX_BUFSIZE (64u * 1024u)

/* Hardened Tcl object accessors.
 *
 * Tcl string/bytearray accessors can return NULL on corrupted or
 * pure-bytearray objects.  These helpers guarantee a non-NULL pointer
 * and zero length on failure so callers can safely treat the value as
 * an empty string/byte-array in non-fatal scanning/rewrite paths.
 */
static inline const char* Tbcx_GetStringFromObjSafe(Tcl_Obj* objPtr, Tcl_Size* lenPtr)
{
    const char* s;

    if (lenPtr)
        *lenPtr = 0;
    if (!objPtr)
        return "";
    s = Tcl_GetStringFromObj(objPtr, lenPtr);
    if (!s)
    {
        if (lenPtr)
            *lenPtr = 0;
        return "";
    }
    return s;
}

static inline const char* Tbcx_GetStringSafe(Tcl_Obj* objPtr)
{
    const char* s;

    if (!objPtr)
        return "";
    s = Tcl_GetString(objPtr);
    return s ? s : "";
}

static inline unsigned char* Tbcx_GetByteArrayFromObjSafe(Tcl_Obj* objPtr, Tcl_Size* lenPtr)
{
    /* const: sentinel byte must never be written through the returned pointer.
     * Cast to non-const on return to match Tcl_GetByteArrayFromObj signature;
     * callers that only need the bytes for reading are safe. */
    static const unsigned char emptyByte = 0;
    unsigned char* p;

    if (lenPtr)
        *lenPtr = 0;
    if (!objPtr)
        return (unsigned char*)&emptyByte;
    p = Tcl_GetByteArrayFromObj(objPtr, lenPtr);
    if (!p)
    {
        if (lenPtr)
            *lenPtr = 0;
        return (unsigned char*)&emptyByte;
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
static inline const char* Tbcx_GetStringFromObjStrict(Tcl_Interp* ip, Tcl_Obj* o, Tcl_Size* len)
{
    const char* s;
    if (!o)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    s = Tcl_GetStringFromObj(o, len);
    if (!s)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object", -1));
        return NULL;
    }
    return s;
}

static inline const char* Tbcx_GetStringStrict(Tcl_Interp* ip, Tcl_Obj* o)
{
    const char* s;
    if (!o)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    s = Tcl_GetString(o);
    if (!s)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid string object", -1));
        return NULL;
    }
    return s;
}

static inline unsigned char* Tbcx_GetByteArrayFromObjStrict(Tcl_Interp* ip, Tcl_Obj* o, Tcl_Size* len)
{
    unsigned char* p;
    if (!o)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid bytearray object (NULL Tcl_Obj)", -1));
        return NULL;
    }
    p = Tcl_GetByteArrayFromObj(o, len);
    if (!p)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: expected valid bytearray object", -1));
        return NULL;
    }
    return p;
}

/* Validate a key string for use with namespace/hash APIs.
 * Rejects embedded NULs and optionally requires absolute namespace form.
 * Returns TCL_OK on success, TCL_ERROR with interp result set on failure. */
static inline int Tbcx_ValidateKeyString(Tcl_Interp* ip, const char* s, Tcl_Size len, const char* what, int requireAbsoluteNs)
{
    if (!s)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: missing %s", what));
        return TCL_ERROR;
    }
    if (len > 0 && memchr(s, '\0', (size_t)len) != NULL)
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: %s contains embedded NUL", what));
        return TCL_ERROR;
    }
    if (requireAbsoluteNs && !(len >= 2 && s[0] == ':' && s[1] == ':'))
    {
        if (ip)
            Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: %s must be absolute", what));
        return TCL_ERROR;
    }
    return TCL_OK;
}

extern const Tcl_ObjType* tbcxTyBignum;
extern const Tcl_ObjType* tbcxTyBoolean;
extern const Tcl_ObjType* tbcxTyByteArray;
extern const Tcl_ObjType* tbcxTyBytecode;

/* Extract ByteCode* using the runtime bytecode type registered by Tcl.
 * Returns NULL when objPtr has no matching internal representation. */
static inline ByteCode* TbcxGetByteCode(Tcl_Obj* objPtr)
{
    const Tcl_ObjInternalRep* irPtr = Tcl_FetchInternalRep(objPtr, tbcxTyBytecode);
    return irPtr ? (ByteCode*)irPtr->twoPtrValue.ptr1 : NULL;
}

/* Flags for TbcxFixupByteCode cache handling strategy. */
#define TBCX_FIXUP_CACHE_KEEP 0 /* Preserve + fix LocalCache (methods, lambdas) */
#define TBCX_FIXUP_CACHE_DROP 1 /* NULL cache for a rebuilt-Proc fallback */
#define TBCX_FIXUP_CACHE_NONE 2 /* Don't touch LocalCache */

extern const Tcl_ObjType* tbcxTyDict;
extern const Tcl_ObjType* tbcxTyDouble;
extern const Tcl_ObjType* tbcxTyInt;
extern const Tcl_ObjType* tbcxTyLambda;
extern const Tcl_ObjType* tbcxTyList;
extern const Tcl_ObjType* tbcxTyProcBody;

extern const AuxDataType* tbcxAuxJTStr;
extern const AuxDataType* tbcxAuxJTNum;
extern const AuxDataType* tbcxAuxDictUpdate;
extern const AuxDataType* tbcxAuxNewForeach;

/* ==========================================================================
 * Buffered I/O wrapper types
 * ========================================================================== */

typedef struct TbcxIn
{
    Tcl_Interp* interp;
    Tcl_Channel chan;
    int err;
    /* A validated artifact is materialized from this neutral byte image.
     * When imageBytes is non-NULL, chan is not read. */
    const unsigned char* imageBytes;
    Tcl_Size imageLength;
    Tcl_Size imagePos;
    uint64_t absoluteOffset;
    uint32_t sourcePolicy;
    Tcl_Size executableSourceFieldCount;
    Tcl_Size nonemptyExecutableSourceFieldCount;
    uint64_t executableSourceBytes;
    unsigned char buf[TBCX_BUFSIZE];
    Tcl_Size bufPos;  /* next byte to consume */
    Tcl_Size bufFill; /* valid bytes in buf */
} TbcxIn;

typedef enum TbcxExecSourceKind
{
    TBCX_EXEC_SOURCE_PROC,
    TBCX_EXEC_SOURCE_METHOD,
    TBCX_EXEC_SOURCE_BYTESRC,
    TBCX_EXEC_SOURCE_LAMBDA
} TbcxExecSourceKind;

/* Interpreter-neutral, same-thread artifact blueprint.  The preflight parser
 * validates directly over these owned bytes and records policy/accounting;
 * materialization then starts a fresh TbcxIn over the same immutable image. */
typedef struct TbcxArtifactImage
{
    Tcl_Size refCount;
    Tcl_ThreadId ownerThread;
    unsigned char* bytes;
    Tcl_Size length;
    uint32_t sourcePolicy;
    Tcl_Size executableSourceFieldCount;
    Tcl_Size nonemptyExecutableSourceFieldCount;
    uint64_t executableSourceBytes;
} TbcxArtifactImage;

typedef struct
{
    Tcl_Interp* interp;
    Tcl_Channel chan;
    int err;
    unsigned char buf[TBCX_BUFSIZE];
    Tcl_Size bufPos;     /* next free position in buf */
    uint64_t totalBytes; /* total bytes written (buf flushes + current bufPos) */
} TbcxOut;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

int Tbcx_BuildLocals(Tcl_Interp* ip, Tcl_Obj* argsList, CompiledLocal** firstOut, CompiledLocal** lastOut, Tcl_Size* numArgsOut);
int Tbcx_CheckBinaryChan(Tcl_Interp* ip, Tcl_Channel ch);
Tcl_Namespace* Tbcx_EnsureNamespace(Tcl_Interp* ip, const char* fqn);
void Tbcx_FreeLocals(CompiledLocal* first);
int Tbcx_ProbeOpenChannel(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Channel* chPtr);
int Tbcx_ProbeReadableFile(Tcl_Interp* interp, Tcl_Obj* pathObj);
void Tbcx_R_Init(TbcxIn* r, Tcl_Interp* ip, Tcl_Channel ch);
void Tbcx_R_InitImage(TbcxIn* r, Tcl_Interp* ip, const TbcxArtifactImage* image);
int Tbcx_R_Bytes(TbcxIn* r, void* p, Tcl_Size n);
int Tbcx_R_AtEOF(TbcxIn* r);
int Tbcx_R_LPString(TbcxIn* r, char** sp, uint32_t* lenp);
int Tbcx_ReadExecutableSource(TbcxIn* r, TbcxExecSourceKind kind, char** sp, uint32_t* lenp);
int Tbcx_R_U32(TbcxIn* r, uint32_t* vp);
int Tbcx_R_U64(TbcxIn* r, uint64_t* vp);
int Tbcx_R_U8(TbcxIn* r, uint8_t* v);
int Tbcx_ValidateMethodMetadata(Tcl_Interp* ip, uint8_t kind, uint8_t scope, uint8_t origin);
void Tbcx_W_Init(TbcxOut* w, Tcl_Interp* ip, Tcl_Channel ch);
int Tbcx_W_Flush(TbcxOut* w);
Tcl_Obj* Tbcx_ReadBlock(TbcxIn* r,
                        Tcl_Interp* ip,
                        Namespace* nsForDefault,
                        uint32_t* numLocalsOut,
                        int setPrecompiled,
                        int dumpOnly,
                        uint32_t* topPolicyOut);
int Tbcx_ReadHeader(TbcxIn* r, TbcxHeader* H);
int Tbcx_ReadArtifactImage(Tcl_Interp* ip, Tcl_Channel ch, TbcxArtifactImage** imageOut);
int Tbcx_PreflightArtifact(Tcl_Interp* ip, TbcxArtifactImage* image);
void Tbcx_ArtifactImageRelease(TbcxArtifactImage* image);
const char* Tbcx_ExecSourceKindName(TbcxExecSourceKind kind);
int Tbcx_ValidateBlockPolicy(Tcl_Interp* ip, uint32_t policy);
#ifdef TBCX_BENCH_COUNTERS
void Tbcx_BenchUnsupportedCrossInterpReject(void);
#endif
void TbcxApplyShimPurgeAll(Tcl_Interp* ip);
void TbcxFixupByteCode(ByteCode* bc, Proc* proc, Tcl_Interp* ip, Namespace* ns, int cacheMode);
int TbcxVerifyLoadedBC(ByteCode* bc, Tcl_Interp* ip, const char* label);

/* Checked multiplication for allocation sizes.  Returns 1 on success
 * (result stored in *out), 0 if the multiplication would overflow size_t. */
static inline int tbcx_checked_mul(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

#endif
