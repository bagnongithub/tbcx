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

/* tbcxTypesLoaded: publication gate for the one-time type initialization.
 * Written to 1 with memory_order_release under tbcxTypeMutex, read with
 * memory_order_acquire on the fast path.  This guarantees that any thread
 * observing tbcxTypesLoaded == 1 also sees all preceding stores to the
 * tbcxTy and tbcxAux pointer globals, even on weakly-ordered architectures. */
static _Atomic int tbcxTypesLoaded = 0;

/* tbcxHostIsLE: host byte-order flag (1 = little-endian, 0 = big-endian).
 * Written once during TbcxInitTypes, thereafter read from hot paths
 * (W_U32, W_U64, R_U32, R_U64) without locking.
 * Declared _Atomic (C11) to guarantee visibility across threads on
 * weakly-ordered architectures (ARM, POWER). */
_Atomic int tbcxHostIsLE = 1;

/* tbcxTypeMutex: protects the one-time initialization of all tbcxTy* /
 * tbcxAux* globals and the tbcxTypesLoaded flag.  Lock-order position:
 * leaf — no other TBCX mutex may be held while this is held.
 * After tbcxTypesLoaded is set to 1, the protected data is immutable. */
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
uint32_t Tbcx_PackTclVersion(void);
static int TbcxComputeEndian(Tcl_Interp* interp);
static const Tcl_ObjType* TbcxProbeLambdaType(Tcl_Interp* interp);
static int TbcxInitTypes(Tcl_Interp* interp);

DLLEXPORT int tbcx_SafeInit(Tcl_Interp* ip);
DLLEXPORT int tbcx_Init(Tcl_Interp* interp);

/* ==========================================================================
 * Explicit ApplyShim purge
 *
 * Synopsis:   tbcx::gc
 * Arguments:  none
 * Returns:    TCL_OK always.
 * Thread:     must be called on the interp-owning thread.
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

uint32_t Tbcx_PackTclVersion(void)
{
    int maj, min, pat, typ;
    Tcl_GetVersion(&maj, &min, &pat, &typ);
    return ((uint32_t)maj << 24) | ((uint32_t)min << 16) | ((uint32_t)pat << 8) | (uint32_t)typ;
}

/* Determine host byte order.  Returns 1 for LE, 0 for BE.
 * Tries Tcl's tcl_platform(byteOrder) first, falls back to union probe.
 * Called OUTSIDE the mutex so Tcl API calls are safe. */
static int TbcxComputeEndian(Tcl_Interp* interp)
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

    return isLE ? 1 : 0;
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

/* Probe for the lambdaExpr type pointer by evaluating a trivial [apply].
 * Called OUTSIDE the mutex so Tcl_EvalObjv is safe. */
static const Tcl_ObjType* TbcxProbeLambdaType(Tcl_Interp* interp)
{
    const Tcl_ObjType* ty = Tcl_GetObjType("lambdaExpr");
    if (ty || !interp)
        return ty;

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
        ty = probe->typePtr;
    }
    Tcl_ResetResult(interp);
    Tcl_DecrRefCount(probe);
    Tcl_DecrRefCount(applyWord);
    return ty;
}

static int TbcxInitTypes(Tcl_Interp* interp)
{
    /* ---- Phase 1: Do ALL Tcl API work OUTSIDE the mutex ----
     * NeedObjType calls Tcl_GetObjType, object constructors, etc.
     * TbcxComputeEndian calls Tcl_GetVar2Ex.
     * TbcxProbeLambdaType calls Tcl_EvalObjv.
     *
     * None of these may be called under tbcxTypeMutex because:
     *  (a) Tcl_EvalObjv can re-enter the Tcl event loop / call
     *      other package inits, risking deadlock.
     *  (b) The other Tcl APIs may acquire internal Tcl mutexes,
     *      creating lock-order hazards.
     *  (c) Holding a global mutex during arbitrary interpreter work
     *      serializes unrelated interpreter startup.
     *
     * Since the values are host-constant and Tcl-version-constant,
     * computing them redundantly from two racing threads is harmless;
     * only the final assignment is serialized by the mutex. */

    int hostLE = TbcxComputeEndian(interp);

    const Tcl_ObjType* tyBignum = NeedObjType("bignum");
    const Tcl_ObjType* tyBoolean = NeedObjType("boolean");
    const Tcl_ObjType* tyByteArray = NeedObjType("bytearray");
    const Tcl_ObjType* tyBytecode = NeedObjType("bytecode");
    const Tcl_ObjType* tyDict = NeedObjType("dict");
    const Tcl_ObjType* tyDouble = NeedObjType("double");
    const Tcl_ObjType* tyInt = NeedObjType("int");
    const Tcl_ObjType* tyLambda = TbcxProbeLambdaType(interp);
    const Tcl_ObjType* tyList = NeedObjType("list");
    const Tcl_ObjType* tyProcBody = NeedObjType("procbody");

    const AuxDataType* auxJTStr = TclGetAuxDataType("JumptableInfo");
    const AuxDataType* auxJTNum = TclGetAuxDataType("JumptableNumInfo");
    const AuxDataType* auxDictUpdate = TclGetAuxDataType("DictUpdateInfo");
    const AuxDataType* auxNewForeach = TclGetAuxDataType("NewForeachInfo");

    /* Verify critical types before acquiring the mutex */
    if (!tyBytecode || !tyInt || !tyList || !tyProcBody || !tyDouble || !tyByteArray)
    {
        if (interp)
        {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: required Tcl_ObjType not available"));
        }
        return TCL_ERROR;
    }

    /* ---- Phase 2: Assign globals under the mutex ---- */
    Tcl_MutexLock(&tbcxTypeMutex);

    if (atomic_load_explicit(&tbcxTypesLoaded, memory_order_acquire))
    {
        /* Another thread beat us — our local probes are redundant but harmless.
         * The acquire fence ensures we see all type pointer stores. */
        Tcl_MutexUnlock(&tbcxTypeMutex);
        return TCL_OK;
    }

    atomic_store(&tbcxHostIsLE, hostLE);

    tbcxTyBignum = tyBignum;
    tbcxTyBoolean = tyBoolean;
    tbcxTyByteArray = tyByteArray;
    tbcxTyBytecode = tyBytecode;
    tbcxTyDict = tyDict;
    tbcxTyDouble = tyDouble;
    tbcxTyInt = tyInt;
    tbcxTyLambda = tyLambda;
    tbcxTyList = tyList;
    tbcxTyProcBody = tyProcBody;

    tbcxAuxJTStr = auxJTStr;
    tbcxAuxJTNum = auxJTNum;
    tbcxAuxDictUpdate = auxDictUpdate;
    tbcxAuxNewForeach = auxNewForeach;

    /* Publish: all type pointer stores above must be visible to any
     * thread that subsequently observes tbcxTypesLoaded == 1. */
    atomic_store_explicit(&tbcxTypesLoaded, 1, memory_order_release);

    Tcl_MutexUnlock(&tbcxTypeMutex);
    return TCL_OK;
}

/* ==========================================================================
 * Extension initialization entry point.
 *
 * Synopsis:   package require tbcx
 * Arguments:  interp — the interpreter to initialize in.
 * Returns:    TCL_OK on success, TCL_ERROR on failure.
 * Side effects: Registers tbcx::save, tbcx::load, tbcx::dump, tbcx::gc
 *               commands and provides package tbcx 1.0.
 * Thread:     must be called on the interp-owning thread.  Performs
 *             one-time global type initialization under tbcxTypeMutex;
 *             may call Tcl_EvalObjv for lambda type probing.
 * ========================================================================== */

DLLEXPORT int tbcx_Init(Tcl_Interp* interp)
{
    if (Tcl_InitStubs(interp, "9.1", 0) == NULL)
    {
        return TCL_ERROR;
    }

    if (Tcl_TomMath_InitStubs(interp, "9.1") == NULL)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: libtommath stubs not available"));
        return TCL_ERROR;
    }

    if (TclOOInitializeStubs(interp, "1.0") == NULL)
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: TclOO stubs not available"));
        return TCL_ERROR;
    }

    if (TbcxInitTypes(interp) != TCL_OK)
    {
        return TCL_ERROR;
    }

    if (!Tcl_CreateObjCommand2(interp, "tbcx::save", Tbcx_SaveObjCmd, NULL, NULL) ||
        !Tcl_CreateObjCommand2(interp, "tbcx::load", Tbcx_LoadObjCmd, NULL, NULL) ||
        !Tcl_CreateObjCommand2(interp, "tbcx::dump", Tbcx_DumpObjCmd, NULL, NULL) ||
        !Tcl_CreateObjCommand2(interp, "tbcx::gc", Tbcx_GcObjCmd, NULL, NULL))
    {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("tbcx: failed to register commands"));
        return TCL_ERROR;
    }

    if (Tcl_PkgProvide(interp, PKG_TBCX, PKG_TBCX_VER) != TCL_OK)
    {
        return TCL_ERROR;
    }

    return TCL_OK;
}

/* ==========================================================================
 * Safe interpreter initialization entry point.
 *
 * Synopsis:   (internal — called by Tcl's package loader for safe interps)
 * Arguments:  ip — the safe interpreter to initialize in.
 * Returns:    TCL_OK on success, TCL_ERROR on failure.
 * Side effects: Initializes type infrastructure and provides package tbcx 1.0.
 *               Does NOT register any commands — safe interps get no
 *               filesystem or bytecode access.  Parent interpreters that need
 *               controlled access can use [interp alias] or [interp expose].
 *               Note: lambda type probing executes a trivial [apply {{} {}]
 *               in the safe interpreter, which is a benign side effect.
 * Thread:     must be called on the interp-owning thread.
 * ========================================================================== */

/* SafeInit: do NOT expose any commands that perform filesystem I/O.
   The original code exposed tbcx::dump here, but dump opens arbitrary
   filesystem paths via Tcl_FSOpenFileChannel, reintroducing file-read
   capability into safe interpreters.  Parent interpreters that need
   controlled dump access can use [interp alias] or [interp expose]. */
DLLEXPORT int tbcx_SafeInit(Tcl_Interp* ip)
{
    if (Tcl_InitStubs(ip, "9.1", 0) == NULL)
        return TCL_ERROR;
    if (Tcl_TomMath_InitStubs(ip, "9.1") == NULL)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: libtommath stubs not available"));
        return TCL_ERROR;
    }
    if (TclOOInitializeStubs(ip, "1.0") == NULL)
    {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("tbcx: TclOO stubs not available"));
        return TCL_ERROR;
    }
    if (TbcxInitTypes(ip) != TCL_OK)
        return TCL_ERROR;

    /* No commands registered — safe interps get the package type
       infrastructure only.  Use [interp expose] from the parent
       to selectively grant access to tbcx subcommands. */

    if (Tcl_PkgProvide(ip, PKG_TBCX, PKG_TBCX_VER) != TCL_OK)
    {
        return TCL_ERROR;
    }
    return TCL_OK;
}
