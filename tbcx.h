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
#define TBCX_FORMAT 91u

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
#define TBCX_LIT_BYTECODE 10u
#define TBCX_LIT_BYTESRC 11u /* bytecode + source text (enables cross-interp recompilation) */

#define TBCX_AUX_JT_STR 0u
#define TBCX_AUX_JT_NUM 1u
#define TBCX_AUX_DICTUPD 2u
#define TBCX_AUX_NEWFORE 3u

#define TBCX_METH_INST 0u
#define TBCX_METH_CLASS 1u
#define TBCX_METH_CTOR 2u
#define TBCX_METH_DTOR 3u
#define TBCX_METH_SELF 4u /* self method (class-level, installed via oo::objdefine) */

/* Indexed proc marker prefix.  The save side emits stub bodies of the
   form "\x01TBCX<decimal-index>" so that the load-side ProcShim can
   match by position rather than by FQN — correctly handling conflicting
   proc definitions across if/else branches. */
#define TBCX_PROC_MARKER_PFX "\x01TBCX"
#define TBCX_PROC_MARKER_PFX_LEN 5

/* TbcxHeader: wire-format documentation & local staging area.
 * Serialized field-by-field via W_U32/W_U64; never memcpy'd as a block. */
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
} TbcxHeader;

extern _Atomic int tbcxHostIsLE;

uint32_t Tbcx_PackTclVersion(void);

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
 * The project review spec treats Tcl string/bytearray accessors as fallible.
 * These helpers guarantee a non-NULL pointer and zero length on failure so
 * callers can safely treat the value as an empty string/byte-array in
 * non-fatal scanning/rewrite paths.
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
    static unsigned char emptyByte = 0;
    unsigned char* p;

    if (lenPtr)
        *lenPtr = 0;
    if (!objPtr)
        return &emptyByte;
    p = Tcl_GetByteArrayFromObj(objPtr, lenPtr);
    if (!p)
    {
        if (lenPtr)
            *lenPtr = 0;
        return &emptyByte;
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

/* Maximum length of generated shim command names (oo::define rename targets).
 * Must accommodate "::tbcx::__oo_objdef_orig_NNNN__" with generous room. */
#define TBCX_OSHIM_NAME_MAX 80

extern const Tcl_ObjType* tbcxTyBignum;
extern const Tcl_ObjType* tbcxTyBoolean;
extern const Tcl_ObjType* tbcxTyByteArray;
extern const Tcl_ObjType* tbcxTyBytecode;

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
static inline ByteCode* TbcxGetByteCode(Tcl_Obj* objPtr)
{
    const Tcl_ObjInternalRep* irPtr = Tcl_FetchInternalRep(objPtr, tbcxTyBytecode);
    return irPtr ? (ByteCode*)irPtr->twoPtrValue.ptr1 : NULL;
}

/* Flags for TbcxFixupByteCode cache handling strategy. */
#define TBCX_FIXUP_CACHE_KEEP 0 /* Preserve + fix LocalCache (methods, lambdas) */
#define TBCX_FIXUP_CACHE_DROP 1 /* NULL out LocalCache (procs — Tcl rebuilds) */
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
    unsigned char buf[TBCX_BUFSIZE];
    Tcl_Size bufPos;  /* next byte to consume */
    Tcl_Size bufFill; /* valid bytes in buf */
} TbcxIn;

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
int Tbcx_R_Bytes(TbcxIn* r, void* p, Tcl_Size n);
int Tbcx_R_LPString(TbcxIn* r, char** sp, uint32_t* lenp);
int Tbcx_R_U32(TbcxIn* r, uint32_t* vp);
int Tbcx_R_U64(TbcxIn* r, uint64_t* vp);
int Tbcx_R_U8(TbcxIn* r, uint8_t* v);
void Tbcx_W_Init(TbcxOut* w, Tcl_Interp* ip, Tcl_Channel ch);
int Tbcx_W_Flush(TbcxOut* w);
Tcl_Obj* Tbcx_ReadBlock(TbcxIn* r, Tcl_Interp* ip, Namespace* nsForDefault, uint32_t* numLocalsOut, int setPrecompiled, int dumpOnly);
int Tbcx_ReadHeader(TbcxIn* r, TbcxHeader* H);
void TbcxApplyShimPurgeAll(Tcl_Interp* ip);
void TbcxFixupByteCode(ByteCode* bc, Proc* proc, Tcl_Interp* ip, Namespace* ns, int cacheMode);
int TbcxVerifyLoadedBC(ByteCode* bc, Tcl_Interp* ip, const char* label);

#endif
