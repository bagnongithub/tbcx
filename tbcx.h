/* tbcx.h - TBCX shared declarations for Tcl 9.1+ */

#ifndef TBCX_H
#define TBCX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tcl.h"
#include "tclCompile.h"
#include "tclInt.h"
#include "tclIntDecls.h"
#include "tclTomMath.h"

#define TBCX_MAGIC 0x58434254u
#define TBCX_FORMAT 9u
#define TBCX_FLAGS_V1 0u

#define TBCX_LIT_BIGNUM 0u
#define TBCX_LIT_BOOLEAN 1u
#define TBCX_LIT_BYTEARR 2u
#define TBCX_LIT_DICT 3u
#define TBCX_LIT_DOUBLE 4u
#define TBCX_LIT_LIST 5u
#define TBCX_LIT_STRING 6u
#define TBCX_LIT_WIDEINT 7u
#define TBCX_LIT_WIDEUINT 8u

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
    uint32_t magic;
    uint32_t format;
    uint32_t flags;
    uint64_t codeLen;
    uint32_t numCmds;
    uint32_t numExcept;
    uint32_t numLiterals;
    uint32_t numAux;
    uint32_t numLocals;
    uint32_t maxStackDepth;
} TbcxHeader;
#pragma pack(pop)

extern int tbcxHostIsLE;
void       TbcxInitEndian(Tcl_Interp *interp);

#if defined(_MSC_VER)
#include <stdlib.h>
#define TBCX_BSWAP16 _byteswap_ushort
#define TBCX_BSWAP32 _byteswap_ulong
#define TBCX_BSWAP64 _byteswap_uint64
#elif defined(__has_builtin)
#if __has_builtin(__builtin_bswap16)
#define TBCX_BSWAP16 __builtin_bswap16
#else
static inline uint16_t TBCX_BSWAP16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
#endif
#if __has_builtin(__builtin_bswap32)
#define TBCX_BSWAP32 __builtin_bswap32
#else
static inline uint32_t TBCX_BSWAP32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) | ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
#endif
#if __has_builtin(__builtin_bswap64)
#define TBCX_BSWAP64 __builtin_bswap64
#else
static inline uint64_t TBCX_BSWAP64(uint64_t x) {
    x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x & 0xFF00FF00FF00FF00ull) >> 8);
    x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x & 0xFFFF0000FFFF0000ull) >> 16);
    return (x << 32) | (x >> 32);
}
#endif
#else
static inline uint16_t TBCX_BSWAP16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint32_t TBCX_BSWAP32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) | ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
static inline uint64_t TBCX_BSWAP64(uint64_t x) {
    x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x & 0xFF00FF00FF00FF00ull) >> 8);
    x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x & 0xFFFF0000FFFF0000ull) >> 16);
    return (x << 32) | (x >> 32);
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TBCX_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define TBCX_LIKELY(x) (x)
#endif

static inline uint16_t le16(const void *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return TBCX_LIKELY(tbcxHostIsLE) ? v : TBCX_BSWAP16(v);
}

static inline uint32_t le32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return TBCX_LIKELY(tbcxHostIsLE) ? v : TBCX_BSWAP32(v);
}

static inline uint64_t le64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return TBCX_LIKELY(tbcxHostIsLE) ? v : TBCX_BSWAP64(v);
}

static inline void putle32(unsigned char *p, uint32_t v) {
    if (TBCX_LIKELY(tbcxHostIsLE)) {
        memcpy(p, &v, sizeof(v));
    } else {
        uint32_t w = TBCX_BSWAP32(v);
        memcpy(p, &w, sizeof(w));
    }
}

static inline void putle64(unsigned char *p, uint64_t v) {
    if (TBCX_LIKELY(tbcxHostIsLE)) {
        memcpy(p, &v, sizeof(v));
    } else {
        uint64_t w = TBCX_BSWAP64(v);
        memcpy(p, &w, sizeof(w));
    }
}

static inline void putbe32(unsigned char *p, uint32_t v) {
    if (TBCX_LIKELY(!tbcxHostIsLE)) {
        memcpy(p, &v, sizeof(v));
    } else {
        uint32_t w = TBCX_BSWAP32(v);
        memcpy(p, &w, sizeof(w));
    }
}

#define FStoreUInt4AtPtr(ptr_, val_)                                                                                                                                                                   \
    do {                                                                                                                                                                                               \
        putbe32((unsigned char *)(ptr_), (uint32_t)(val_));                                                                                                                                            \
    } while (0)

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
extern const Tcl_ObjType *tbcxTyList;
extern const Tcl_ObjType *tbcxTyLambda;

extern const AuxDataType *tbcxAuxJTStr;
extern const AuxDataType *tbcxAuxJTNum;
extern const AuxDataType *tbcxAuxDictUpdate;
extern const AuxDataType *tbcxAuxForeach;

#endif /* TBCX_H */
