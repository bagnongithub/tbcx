/*
 * tbcx.c - TBCX for Tcl 9.1+
 *
 */

#include "tbcx.h"

#define PKG_TBCX "tbcx"
#define PKG_TBCX_VER "1.0"

const Tcl_ObjType *tbcxTyBignum      = NULL;
const Tcl_ObjType *tbcxTyBoolean     = NULL;
const Tcl_ObjType *tbcxTyByteArray   = NULL;
const Tcl_ObjType *tbcxTyBytecode    = NULL;
const Tcl_ObjType *tbcxTyDict        = NULL;
const Tcl_ObjType *tbcxTyDouble      = NULL;
const Tcl_ObjType *tbcxTyInt         = NULL;
const Tcl_ObjType *tbcxTyList        = NULL;
const Tcl_ObjType *tbcxTyLambda      = NULL;

const AuxDataType *tbcxAuxJTStr      = NULL;
const AuxDataType *tbcxAuxJTNum      = NULL;
const AuxDataType *tbcxAuxDictUpdate = NULL;
const AuxDataType *tbcxAuxForeach    = NULL;

static int         tbcxTypesLoaded   = 0;
TCL_DECLARE_MUTEX(tbcxTypeMutex);

#if defined(WORDS_BIGENDIAN) || (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || (defined(BYTE_ORDER) && (BYTE_ORDER == BIG_ENDIAN))
int tbcxHostIsLE = 0;
#else
int tbcxHostIsLE = 1;
#endif

void TbcxInitEndian(Tcl_Interp *interp) {
    int isLE = -1;

    if (interp) {
        Tcl_Obj *val = Tcl_GetVar2Ex(interp, "tcl_platform", "byteOrder", TCL_GLOBAL_ONLY);
        if (val) {
            const char *s = Tcl_GetString(val);
            if (s && s[0]) {
                isLE = (s[0] == 'l' || s[0] == 'L') ? 1 : (s[0] == 'b' || s[0] == 'B') ? 0 : -1;
            }
        }
    }

    if (isLE < 0) {
        union {
            uint32_t      u;
            unsigned char c[4];
        } u  = {0x01020304u};
        isLE = (u.c[0] == 0x04);
    }

    tbcxHostIsLE = isLE ? 1 : 0;
}

static const Tcl_ObjType *NeedObjType(const char *name) {
    const Tcl_ObjType *t = Tcl_GetObjType(name);
    if (!t) {
        Tcl_Obj *probe = NULL;
        if (strcmp(name, "bignum") == 0) {
            mp_int z;
            if (mp_init(&z) != MP_OKAY)
                Tcl_Panic("tommath: mp_init failed");
            probe = Tcl_NewBignumObj(&z);
        } else if (strcmp(name, "boolean") == 0) {
            probe = Tcl_NewBooleanObj(0);
        } else if (strcmp(name, "int") == 0) {
            probe = Tcl_NewIntObj(0);
        } else if (strcmp(name, "double") == 0) {
            probe = Tcl_NewDoubleObj(0.0);
        } else if (strcmp(name, "bytearray") == 0) {
            probe = Tcl_NewByteArrayObj(NULL, 0);
        }
        if (probe) {
            t = probe->typePtr;
            Tcl_DecrRefCount(probe);
        }
    }
    if (!t)
        Tcl_Panic("missing Tcl_ObjType '%s'", name);
    return t;
}

void Tbcx_InitTypes(Tcl_Interp *interp) {
    if (tbcxTypesLoaded)
        return;
    Tcl_MutexLock(&tbcxTypeMutex);
    if (tbcxTypesLoaded) {
        Tcl_MutexUnlock(&tbcxTypeMutex);
        return;
    }

    TbcxInitEndian(interp);

    tbcxTyBoolean     = NeedObjType("boolean");
    tbcxTyByteArray   = NeedObjType("bytearray");
    tbcxTyBytecode    = NeedObjType("bytecode");
    tbcxTyDict        = NeedObjType("dict");
    tbcxTyDouble      = NeedObjType("double");
    tbcxTyInt         = NeedObjType("int");
    tbcxTyList        = NeedObjType("list");
    tbcxTyBignum      = NeedObjType("bignum");
    tbcxTyLambda      = Tcl_GetObjType("lambdaExpr");

    tbcxAuxJTStr      = TclGetAuxDataType("JumptableInfo");
    tbcxAuxJTNum      = TclGetAuxDataType("JumptableNumInfo");
    tbcxAuxDictUpdate = TclGetAuxDataType("DictUpdateInfo");
    tbcxAuxForeach    = TclGetAuxDataType("NewForeachInfo");

    tbcxTypesLoaded   = 1;
    Tcl_MutexUnlock(&tbcxTypeMutex);
}

EXTERN int Tbcx_SaveObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
EXTERN int Tbcx_SaveChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
EXTERN int Tbcx_SaveFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
EXTERN int Tbcx_LoadChanObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
EXTERN int Tbcx_LoadFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);
EXTERN int Tbcx_DumpFileObjCmd(void *cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]);

int        tbcx_Init(Tcl_Interp *interp) {
    if (Tcl_InitStubs(interp, TCL_VERSION, 1) == NULL) {
        return TCL_ERROR;
    }

    if (Tcl_TomMath_InitStubs(interp, TCL_VERSION) == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: libtommath stubs not available", -1));
        return TCL_ERROR;
    }

    Tbcx_InitTypes(interp);

    Tcl_CreateObjCommand2(interp, "tbcx::save", Tbcx_SaveObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::savechan", Tbcx_SaveChanObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::savefile", Tbcx_SaveFileObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::loadchan", Tbcx_LoadChanObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::loadfile", Tbcx_LoadFileObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::dumpfile", Tbcx_DumpFileObjCmd, NULL, NULL);

    Tcl_PkgProvide(interp, PKG_TBCX, PKG_TBCX_VER);

    return TCL_OK;
}
