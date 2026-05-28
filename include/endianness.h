#ifndef _ENDIANNESS_H_
#define _ENDIANNESS_H_

/*
 * Portable endian conversion. Uses compiler-provided __BYTE_ORDER__ which
 * is defined by gcc and clang on Linux, macOS, and *BSD. The upstream
 * version only checked glibc's __BYTE_ORDER, which isn't defined on macOS.
 */

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) \
    && defined(__ORDER_BIG_ENDIAN__)

    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define TO_LITTLE_ENDIAN(x) (x)
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define TO_LITTLE_ENDIAN(x) __builtin_bswap32(x)
    #else
        #error "Unsupported byte order"
    #endif

#elif defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)

    #if __BYTE_ORDER == __LITTLE_ENDIAN
        #define TO_LITTLE_ENDIAN(x) (x)
    #elif __BYTE_ORDER == __BIG_ENDIAN
        #include <byteswap.h>
        #define TO_LITTLE_ENDIAN(x) bswap_32(x)
    #endif

#else
    #error "Cannot determine byte order"
#endif

#endif
