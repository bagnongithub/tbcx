/* ==========================================================================
 * tbcx.c - TBCX for Tcl 9.1
 * ========================================================================== */

#include "tbcx.h"

/* ==========================================================================
 * File-local globals
 * ========================================================================== */

#define PKG_TBCX "tbcx"
#define PKG_TBCX_VER "1.0"

const Tcl_ObjType* tbcxTyBignum = NULL;
const Tcl_ObjType* tbcxTyBoolean = NULL;
const Tcl_ObjType* tbcxTyByteArray = NULL;
const Tcl_ObjType* tbcxTyBytecode = NULL;
const Tcl_ObjType* tbcxTyDict = NULL;
const Tcl_ObjType* tbcxTyDouble = NULL;
const Tcl_ObjType* tbcxTyInt = NULL;
const Tcl_ObjType* tbcxTyLambda = NULL;
const Tcl_ObjType* tbcxTyList = NULL;
const Tcl_ObjType* tbcxTyProcBody = NULL;

const AuxDataType* tbcxAuxJTStr = NULL;
const AuxDataType* tbcxAuxJTNum = NULL;
const AuxDataType* tbcxAuxDictUpdate = NULL;
const AuxDataType* tbcxAuxNewForeach = NULL;

static int tbcxTypesLoaded = 0;
int tbcxHostIsLE = 1;

TCL_DECLARE_MUTEX(tbcxTypeMutex);

/* ==========================================================================
 * Extern Declarations
 * ========================================================================== */

EXTERN int Tbcx_SaveObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);
EXTERN int Tbcx_LoadObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);
EXTERN int Tbcx_DumpObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);
EXTERN int Tbcx_GcObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[]);

/* ==========================================================================
 * Forward Declarations
 * ========================================================================== */

static const Tcl_ObjType* NeedObjType(const char* name);
uint32_t PackTclVersion(void);
static void TbcxInitEndian(Tcl_Interp* interp);
static int TbcxInitTypes(Tcl_Interp* interp);

DLLEXPORT int tbcx_SafeInit(Tcl_Interp* ip);
DLLEXPORT int tbcx_Init(Tcl_Interp* interp);

/* ==========================================================================
 * Explicit ApplyShim purge
 * ========================================================================== */

int Tbcx_GcObjCmd(TCL_UNUSED(void*), Tcl_Interp* interp, Tcl_Size objc, Tcl_Obj* const objv[])
{
    if (objc != 1)
    {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    TbcxApplyShimPurgeAll(interp);
    return TCL_OK;
}

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

uint32_t PackTclVersion(void)
{
    int maj, min, pat, typ;
    Tcl_GetVersion(&maj, &min, &pat, &typ);
    return ((uint32_t)maj << 24) | ((uint32_t)min << 16) | ((uint32_t)pat << 8) | (uint32_t)typ;
}

static void TbcxInitEndian(Tcl_Interp* interp)
{
    int isLE = -1;

    if (interp)
    {
        Tcl_Obj* val = Tcl_GetVar2Ex(interp, "tcl_platform", "byteOrder", TCL_GLOBAL_ONLY);
        if (val)
        {
            const char* s = Tcl_GetString(val);
            if (s && s[0])
            {
                isLE = (s[0] == 'l' || s[0] == 'L') ? 1 : (s[0] == 'b' || s[0] == 'B') ? 0 : -1;
            }
        }
    }

    if (isLE < 0)
    {
        union
        {
            uint32_t u;
            unsigned char c[4];
        } u = {0x01020304u};
        isLE = (u.c[0] == 0x04);
    }

    tbcxHostIsLE = isLE ? 1 : 0;
}

static const Tcl_ObjType* NeedObjType(const char* name)
{
    const Tcl_ObjType* t = Tcl_GetObjType(name);
    if (!t)
    {
        Tcl_Obj* probe = NULL;
        if (strcmp(name, "bignum") == 0)
        {
            mp_int z;
            if (TclBN_mp_init(&z) != MP_OKAY)
                return NULL;
            probe = Tcl_NewBignumObj(&z);
        }
        else if (strcmp(name, "boolean") == 0)
        {
            probe = Tcl_NewBooleanObj(0);
        }
        else if (strcmp(name, "int") == 0)
        {
            probe = Tcl_NewWideIntObj(0);
        }
        else if (strcmp(name, "double") == 0)
        {
            probe = Tcl_NewDoubleObj(0.0);
        }
        else if (strcmp(name, "bytearray") == 0)
        {
            probe = Tcl_NewByteArrayObj(NULL, 0);
        }
        if (probe)
        {
            Tcl_IncrRefCount(probe);
            t = probe->typePtr;
            Tcl_DecrRefCount(probe);
        }
    }
    return t; /* may be NULL — caller must check */
}

static int TbcxInitTypes(Tcl_Interp* interp)
{
    /* Always acquire the mutex before reading tbcxTypesLoaded.
       A plain C int lacks the memory ordering guarantees needed for
       safe double-checked locking on weakly-ordered architectures
       (ARM, POWER).  Since this runs once per interpreter lifetime,
       the cost of always locking is negligible. */
    Tcl_MutexLock(&tbcxTypeMutex);

    if (tbcxTypesLoaded)
    {
        Tcl_MutexUnlock(&tbcxTypeMutex);
        return TCL_OK;
    }

    TbcxInitEndian(interp);

    tbcxTyBignum = NeedObjType("bignum");
    tbcxTyBoolean = NeedObjType("boolean");
    tbcxTyByteArray = NeedObjType("bytearray");
    tbcxTyBytecode = NeedObjType("bytecode");
    tbcxTyDict = NeedObjType("dict");
    tbcxTyDouble = NeedObjType("double");
    tbcxTyInt = NeedObjType("int");
    tbcxTyLambda = Tcl_GetObjType("lambdaExpr");
    if (!tbcxTyLambda && interp)
    {
        /* Tcl 9.1 does not publicly register the lambdaExpr type.
           Force its creation by evaluating a trivial [apply] — this
           makes Tcl install a lambdaExpr internal rep on the probe
           object, from which we can extract the type pointer. */
        Tcl_Obj* applyWord = Tcl_NewStringObj("apply", -1);
        Tcl_Obj* probe = Tcl_NewStringObj("{} {}", -1);
        Tcl_IncrRefCount(applyWord);
        Tcl_IncrRefCount(probe);
        Tcl_Obj* evalv[2] = {applyWord, probe};
        if (Tcl_EvalObjv(interp, 2, evalv, 0) == TCL_OK)
        {
            tbcxTyLambda = probe->typePtr;
        }
        Tcl_ResetResult(interp);
        Tcl_DecrRefCount(probe);
        Tcl_DecrRefCount(applyWord);
    }
    tbcxTyList = NeedObjType("list");
    tbcxTyProcBody = NeedObjType("procbody");

    /* Verify critical types are available */
    if (!tbcxTyBytecode || !tbcxTyInt || !tbcxTyList || !tbcxTyProcBody || !tbcxTyDouble || !tbcxTyByteArray)
    {
        Tcl_MutexUnlock(&tbcxTypeMutex);
        if (interp)
        {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: required Tcl_ObjType not available", -1));
        }
        return TCL_ERROR;
    }

    tbcxAuxJTStr = TclGetAuxDataType("JumptableInfo");
    tbcxAuxJTNum = TclGetAuxDataType("JumptableNumInfo");
    tbcxAuxDictUpdate = TclGetAuxDataType("DictUpdateInfo");
    tbcxAuxNewForeach = TclGetAuxDataType("NewForeachInfo");

    tbcxTypesLoaded = 1;

    Tcl_MutexUnlock(&tbcxTypeMutex);
    return TCL_OK;
}

DLLEXPORT int tbcx_Init(Tcl_Interp* interp)
{
    if (Tcl_InitStubs(interp, TCL_VERSION, 1) == NULL)
    {
        return TCL_ERROR;
    }

    if (Tcl_TomMath_InitStubs(interp, TCL_VERSION) == NULL)
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: libtommath stubs not available", -1));
        return TCL_ERROR;
    }

    if (TclOOInitializeStubs(interp, "1.0") == NULL)
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("tbcx: TclOO stubs not available", -1));
        return TCL_ERROR;
    }

    if (TbcxInitTypes(interp) != TCL_OK)
    {
        return TCL_ERROR;
    }

    Tcl_CreateObjCommand2(interp, "tbcx::save", Tbcx_SaveObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::load", Tbcx_LoadObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::dump", Tbcx_DumpObjCmd, NULL, NULL);
    Tcl_CreateObjCommand2(interp, "tbcx::gc", Tbcx_GcObjCmd, NULL, NULL);

    Tcl_PkgProvide(interp, PKG_TBCX, PKG_TBCX_VER);

    return TCL_OK;
}

/* SafeInit should only expose dump (read-only), not save/load which do
   arbitrary file I/O.  Callers who need save/load in safe interps must
   explicitly expose them. */
DLLEXPORT int tbcx_SafeInit(Tcl_Interp* ip)
{
    if (Tcl_InitStubs(ip, TCL_VERSION, 1) == NULL)
        return TCL_ERROR;
    if (Tcl_TomMath_InitStubs(ip, TCL_VERSION) == NULL)
    {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: libtommath stubs not available", -1));
        return TCL_ERROR;
    }
    if (TclOOInitializeStubs(ip, "1.0") == NULL)
    {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("tbcx: TclOO stubs not available", -1));
        return TCL_ERROR;
    }
    if (TbcxInitTypes(ip) != TCL_OK)
        return TCL_ERROR;

    Tcl_CreateObjCommand2(ip, "tbcx::dump", Tbcx_DumpObjCmd, NULL, NULL);

    Tcl_PkgProvide(ip, PKG_TBCX, PKG_TBCX_VER);
    return TCL_OK;
}
