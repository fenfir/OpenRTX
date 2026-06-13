/***************************************************************************
 *   CK803S (C-SKY V2) endianness impl for Miosix.                         *
 *   Port of endianness_impl_cortexMx.h (Terraneo Federico). CK803S is     *
 *   built little-endian (-EL). swapBytes16 uses plain C (CK803S lacks     *
 *   ARM's rev16); 32/64-bit use GCC builtins.                             *
 *   GPL v2+ with the Miosix linking exception (see the cortexMx header).  *
 ***************************************************************************/

#ifndef ENDIANNESS_IMPL_H
#define ENDIANNESS_IMPL_H

#ifndef MIOSIX_BIG_ENDIAN
//This target (ck803s -EL) is little endian
#define MIOSIX_LITTLE_ENDIAN
#endif //MIOSIX_BIG_ENDIAN

#ifdef __cplusplus
#define __MIOSIX_INLINE inline
#else //__cplusplus
#define __MIOSIX_INLINE static inline
#endif //__cplusplus

__MIOSIX_INLINE unsigned short swapBytes16(unsigned short x)
{
    return (unsigned short)((x>>8) | (x<<8));
}

__MIOSIX_INLINE unsigned int swapBytes32(unsigned int x)
{
    #ifdef __GNUC__
    return __builtin_bswap32(x);
    #else
    return ( x>>24)               |
           ((x<< 8) & 0x00ff0000) |
           ((x>> 8) & 0x0000ff00) |
           ( x<<24);
    #endif
}

__MIOSIX_INLINE unsigned long long swapBytes64(unsigned long long x)
{
    #ifdef __GNUC__
    return __builtin_bswap64(x);
    #else
    return ( x>>56)                          |
           ((x<<40) & 0x00ff000000000000ull) |
           ((x<<24) & 0x0000ff0000000000ull) |
           ((x<< 8) & 0x000000ff00000000ull) |
           ((x>> 8) & 0x00000000ff000000ull) |
           ((x>>24) & 0x0000000000ff0000ull) |
           ((x>>40) & 0x000000000000ff00ull) |
           ( x<<56);
    #endif
}

#undef __MIOSIX_INLINE

#endif //ENDIANNESS_IMPL_H
