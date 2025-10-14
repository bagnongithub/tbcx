/* ==========================================================================
 * tbcx.h - TBCX shared declarations for Tcl 9.1
 * ========================================================================== */

#ifndef TBCX_H
#define TBCX_H

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tcl.h"
#include "tclCompile.h"
#include "tclInt.h"
#include "tclIntDecls.h"
#include "tclOO.h"
#include "tclOOInt.h"
#include "tclTomMath.h"

#define TBCX_MAGIC 0x58434254u
#define TBCX_FORMAT 9u

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
#define TBCX_AUX_FOREACH 4u

#define TBCX_METH_INST 0u
#define TBCX_METH_CLASS 1u
#define TBCX_METH_CTOR 2u
#define TBCX_METH_DTOR 3u

#pragma pack(push, 1)
typedef struct TbcxHeader {
    uint32_t magic;       /* "TBCX" */
    uint32_t format;      /* "9u" */
    uint32_t tcl_version; /* mmjjppTT */
    uint64_t codeLenTop;
    uint32_t numCmdsTop, numExceptTop;
    uint32_t numLitsTop, numAuxTop;
    uint32_t numLocalsTop, maxStackTop;
} TbcxHeader;
#pragma pack(pop)

extern int tbcxHostIsLE;

uint32_t   PackTclVersion(void);

#define TBCX_MAX_CODE (1024u * 1024u * 1024u)
#define TBCX_MAX_LITERALS (64u * 1024u * 1024u)
#define TBCX_MAX_AUX (64u * 1024u * 1024u)
#define TBCX_MAX_EXCEPT (64u * 1024u * 1024u)
#define TBCX_MAX_STR (16u * 1024u * 1024u)

extern const Tcl_ObjType *tbcxTyBignum;
extern const Tcl_ObjType *tbcxTyBoolean;
extern const Tcl_ObjType *tbcxTyByteArray;
extern const Tcl_ObjType *tbcxTyBytecode;
extern const Tcl_ObjType *tbcxTyDict;
extern const Tcl_ObjType *tbcxTyDouble;
extern const Tcl_ObjType *tbcxTyInt;
extern const Tcl_ObjType *tbcxTyLambda;
extern const Tcl_ObjType *tbcxTyList;
extern const Tcl_ObjType *tbcxTyProcBody;

extern const AuxDataType *tbcxAuxJTStr;
extern const AuxDataType *tbcxAuxJTNum;
extern const AuxDataType *tbcxAuxDictUpdate;
extern const AuxDataType *tbcxAuxForeach;
extern const AuxDataType *tbcxAuxNewForeach;

/* ==========================================================================
 * Type definitions
 * ========================================================================== */

typedef struct TbcxIn {
    Tcl_Interp *interp;
    Tcl_Channel chan;
    int         err; /* TCL_OK / TCL_ERROR */
} TbcxIn;

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

int            BuildLocals(Tcl_Interp *ip, Tcl_Obj *argsList, CompiledLocal **firstOut, CompiledLocal **lastOut, int *numArgsOut);
int            CheckBinaryChan(Tcl_Interp *ip, Tcl_Channel ch);
Tcl_Namespace *EnsureNamespace(Tcl_Interp *ip, const char *fqn);
void           FreeLocals(CompiledLocal *first);
int            R_Bytes(TbcxIn *r, void *p, size_t n);
int            R_LPString(TbcxIn *r, char **sp, uint32_t *lenp);
int            R_U32(TbcxIn *r, uint32_t *vp);
int            R_U64(TbcxIn *r, uint64_t *vp);
int            R_U8(TbcxIn *r, uint8_t *v);
Tcl_Obj       *ReadCompiledBlock(TbcxIn *r, Tcl_Interp *ip, Namespace *nsForDefault, uint32_t *numLocalsOut);
int            ReadHeader(TbcxIn *r, TbcxHeader *H);

#endif
