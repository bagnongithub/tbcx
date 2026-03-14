/* ==========================================================================
 * tbcx.h - TBCX shared declarations for Tcl 9.1
 * ========================================================================== */

#ifndef TBCX_H
#define TBCX_H

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
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

#define TBCX_AUX_JT_STR 0u
#define TBCX_AUX_JT_NUM 1u
#define TBCX_AUX_DICTUPD 2u
#define TBCX_AUX_NEWFORE 3u

#define TBCX_METH_INST 0u
#define TBCX_METH_CLASS 1u
#define TBCX_METH_CTOR 2u
#define TBCX_METH_DTOR 3u

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

uint32_t PackTclVersion(void);

#define TBCX_MAX_CODE (1024u * 1024u * 1024u)
#define TBCX_MAX_LITERALS (64u * 1024u * 1024u)
#define TBCX_MAX_AUX (64u * 1024u * 1024u)
#define TBCX_MAX_EXCEPT (64u * 1024u * 1024u)
#define TBCX_MAX_STR (16u * 1024u * 1024u)
#define TBCX_MAX_LOCALS 65536u
#define TBCX_MAX_STACK 65536u
#define TBCX_MAX_PROCS (1u * 1024u * 1024u)
#define TBCX_MAX_CLASSES (1u * 1024u * 1024u)
#define TBCX_MAX_METHODS (1u * 1024u * 1024u)

#define TBCX_BUFSIZE (64u * 1024u)

extern const Tcl_ObjType* tbcxTyBignum;
extern const Tcl_ObjType* tbcxTyBoolean;
extern const Tcl_ObjType* tbcxTyByteArray;
extern const Tcl_ObjType* tbcxTyBytecode;
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
    size_t bufPos;  /* next byte to consume */
    size_t bufFill; /* valid bytes in buf */
} TbcxIn;

typedef struct
{
    Tcl_Interp* interp;
    Tcl_Channel chan;
    int err;
    unsigned char buf[TBCX_BUFSIZE];
    size_t bufPos;       /* next free position in buf */
    uint64_t totalBytes; /* total bytes written (buf flushes + current bufPos) */
} TbcxOut;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

int Tbcx_BuildLocals(Tcl_Interp* ip, Tcl_Obj* argsList, CompiledLocal** firstOut, CompiledLocal** lastOut, int* numArgsOut);
int Tbcx_CheckBinaryChan(Tcl_Interp* ip, Tcl_Channel ch);
Tcl_Namespace* Tbcx_EnsureNamespace(Tcl_Interp* ip, const char* fqn);
void Tbcx_FreeLocals(CompiledLocal* first);
int Tbcx_ProbeOpenChannel(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Channel* chPtr);
int Tbcx_ProbeReadableFile(Tcl_Interp* interp, Tcl_Obj* pathObj);

void Tbcx_R_Init(TbcxIn* r, Tcl_Interp* ip, Tcl_Channel ch);
int Tbcx_R_Bytes(TbcxIn* r, void* p, size_t n);
int Tbcx_R_LPString(TbcxIn* r, char** sp, uint32_t* lenp);
int Tbcx_R_U32(TbcxIn* r, uint32_t* vp);
int Tbcx_R_U64(TbcxIn* r, uint64_t* vp);
int Tbcx_R_U8(TbcxIn* r, uint8_t* v);

void Tbcx_W_Init(TbcxOut* w, Tcl_Interp* ip, Tcl_Channel ch);
int Tbcx_W_Flush(TbcxOut* w);

Tcl_Obj*
Tbcx_ReadBlock(TbcxIn* r, Tcl_Interp* ip, Namespace* nsForDefault, uint32_t* numLocalsOut, int setPrecompiled, int dumpOnly);
int Tbcx_ReadHeader(TbcxIn* r, TbcxHeader* H);

void TbcxApplyShimPurgeAll(Tcl_Interp* ip);

#endif
