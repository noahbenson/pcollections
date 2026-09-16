///////////////////////////////////////////////////////////////////////////////
// _c/uintbits.h
// Unsigned integer bit Operations.
// Here we define functions for performing popcount, clz, and ctz on the
// various unsigned integer types.
// Including this file results in the following values being defined:
//  - _PCOLLECTIONS__C_UINTBITS_H
//  - The contents of <stdint.h>, <limits.h>, and <stddef.h>
//  - The contents of <stdbit.h> if it is available on the system.
//  - UINTPTR_C(x) and SIZE_C(x) macros, if not already defined.
//  - popcount8, popcount16, popcount32, popcount64, popcount128 (count ones);
//  - clz8, clz16, clz32, clz64, clz128 (count leading zeros);
//  - ctz8, ctz16, ctz32, ctz64, ctz128 (count trailing zeros);
//  - pow8, pow16, pow32, pow64, pow128 (integer exponentiation);
//  - ltmask8/lemask8/gtmask8/gemask8 and the 16/32/64/128-bit equivalents
//    (bit masks for "less/greater than (or equal to) bit k");
//  - nextbinpow8, nextbinpow16, nextbinpow32, nextbinpow64, nextbinpow128
//    (lowest power of 2 strictly greater than the argument);
//  - popcount_uintptr, popcount_size;
//  - clz_uintptr, clz_size;
//  - ctz_uintptr, ctz_size;
//  - pow_uintptr, pow_size;
//  - ltmask_uintptr/lemask_uintptr/gtmask_uintptr/gemask_uintptr and the
//    _size equivalents;
//  - nextbinpow_uintptr, nextbinpow_size.
// The 64-bit and 128-bit versions are only defined if uint64_t and uint128_t
// types are available on the system.
// The uint128_t type is defined if it is available as a type on the system but
// is not already defined by stdint.h (along with UINT128_MAX and the UINT128_C
// macro).

#ifndef _PCOLLECTIONS__C_UINTBITS_H
#define _PCOLLECTIONS__C_UINTBITS_H

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


// Dependencies ===============================================================

// We need stdint for uint64_t and similar types.
#include <stdint.h>
// We also need limits for mapping the normal type names to the stdint types.
#include <limits.h>
// We also need stddef for size_t.
#include <stddef.h>

// For starters, it's possible that the uint128_t isn't defined explicitly but
// could be... if this is the case, we can go ahead and define it.
// (The test is ULLONG_MAX > UINT64_MAX rather than a shift by 64, which is
// undefined when unsigned long long is 64 bits wide.)
#if (!defined(uint128_t)                  \
     && defined(ULLONG_MAX)               \
     && defined(UINT64_MAX)               \
     && (ULLONG_MAX > UINT64_MAX))
   EXTC typedef unsigned long long uint128_t;
   EXTC typedef unsigned long long uint_fast128_t;
   EXTC typedef unsigned long long uint_least128_t;
#  ifndef UINT128_MAX
#    define UINT128_MAX 0xffffffffffffffffffffffffffffffff
#  endif
#  ifndef UINT128_C
#    define UINT128_C(x) (x ## ULL)
#  endif
#endif

// Bit widths of the fixed-size integer types and of uintptr_t/size_t.
// C23's <stdint.h> defines these (UINT8_WIDTH, ..., UINTPTR_WIDTH,
// SIZE_WIDTH), and this header and trie.h rely on them. Before C23 only some
// libcs provide them (glibc does, via _GNU_SOURCE; Apple's libc and MSVC's
// UCRT do not), so each is used if already defined and otherwise derived
// from the type's MAX macro.
#ifndef UINT8_WIDTH
#  define UINT8_WIDTH 8
#endif
#ifndef UINT16_WIDTH
#  define UINT16_WIDTH 16
#endif
#ifndef UINT32_WIDTH
#  define UINT32_WIDTH 32
#endif
#if defined(UINT64_MAX) && !defined(UINT64_WIDTH)
#  define UINT64_WIDTH 64
#endif
#if defined(UINT128_MAX) && !defined(UINT128_WIDTH)
#  define UINT128_WIDTH 128
#endif
#ifndef UINTPTR_WIDTH
#  if (UINTPTR_MAX == UINT8_MAX)
#     define UINTPTR_WIDTH 8
#  elif (UINTPTR_MAX == UINT16_MAX)
#     define UINTPTR_WIDTH 16
#  elif (UINTPTR_MAX == UINT32_MAX)
#     define UINTPTR_WIDTH 32
#  elif defined(UINT64_MAX) && (UINTPTR_MAX == UINT64_MAX)
#     define UINTPTR_WIDTH 64
#  elif defined(UINT128_MAX) && (UINTPTR_MAX == UINT128_MAX)
#     define UINTPTR_WIDTH 128
#  else
#     error Cannot deduce width of uintptr_t.
#  endif
#endif
#ifndef SIZE_WIDTH
#  if (SIZE_MAX == UINT8_MAX)
#     define SIZE_WIDTH 8
#  elif (SIZE_MAX == UINT16_MAX)
#     define SIZE_WIDTH 16
#  elif (SIZE_MAX == UINT32_MAX)
#     define SIZE_WIDTH 32
#  elif defined(UINT64_MAX) && (SIZE_MAX == UINT64_MAX)
#     define SIZE_WIDTH 64
#  elif defined(UINT128_MAX) && (SIZE_MAX == UINT128_MAX)
#     define SIZE_WIDTH 128
#  else
#     error Cannot deduce width of size_t.
#  endif
#endif

// If the stdbit header is available, we want to use those functions.
#if defined(__STDC_VERSION_STDBIT_H__) && (__STDC_VERSION_STDBIT_H__ >= 202311L)
#  include <stdbit.h>
#  define _PCOLLECTIONS__STDBIT_H
#endif


// Mapping stdbit suffixes to nbit suffixes ===================================
// The stdbit functions use suffixes like _uc or _ul for the unsigned char and
// unsigned long versions of the function. We want to use suffixes like 8 and
// 64 for those same functions, so we use some preprocessor magic to translate
// between them.
#if (UINT8_MAX == UCHAR_MAX)
#  define _STDBIT8(x) ((x) ## _uc)
#elif (UINT8_MAX == USHRT_MAX)
#  define _STDBIT8(x) ((x) ## _us)
#elif (UINT8_MAX == UINT_MAX)
#  define _STDBIT8(x) ((x) ## _ui)
#elif (UINT8_MAX == ULONG_MAX)
#  define _STDBIT8(x) ((x) ## _ul)
#elif (UINT8_MAX == ULLONG_MAX)
#  define _STDBIT8(x) ((x) ## _ull)
#endif
#if (UINT16_MAX == UCHAR_MAX)
#  define _STDBIT16(x) ((x) ## _uc)
#elif (UINT16_MAX == USHRT_MAX)
#  define _STDBIT16(x) ((x) ## _us)
#elif (UINT16_MAX == UINT_MAX)
#  define _STDBIT16(x) ((x) ## _ui)
#elif (UINT16_MAX == ULONG_MAX)
#  define _STDBIT16(x) ((x) ## _ul)
#elif (UINT16_MAX == ULLONG_MAX)
#  define _STDBIT16(x) ((x) ## _ull)
#endif
#if (UINT32_MAX == UCHAR_MAX)
#  define _STDBIT32(x) ((x) ## _uc)
#elif (UINT32_MAX == USHRT_MAX)
#  define _STDBIT32(x) ((x) ## _us)
#elif (UINT32_MAX == UINT_MAX)
#  define _STDBIT32(x) ((x) ## _ui)
#elif (UINT32_MAX == ULONG_MAX)
#  define _STDBIT32(x) ((x) ## _ul)
#elif (UINT32_MAX == ULLONG_MAX)
#  define _STDBIT32(x) ((x) ## _ull)
#endif
#ifdef UINT64_MAX
#  if (UINT64_MAX == UCHAR_MAX)
#    define _STDBIT64(x) ((x) ## _uc)
#  elif (UINT64_MAX == USHRT_MAX)
#    define _STDBIT64(x) ((x) ## _us)
#  elif (UINT64_MAX == UINT_MAX)
#    define _STDBIT64(x) ((x) ## _ui)
#  elif (UINT64_MAX == ULONG_MAX)
#    define _STDBIT64(x) ((x) ## _ul)
#  elif (UINT64_MAX == ULLONG_MAX)
#    define _STDBIT64(x) ((x) ## _ull)
#  endif
#endif
#ifdef UINT128_MAX
#  if (UINT128_MAX == UCHAR_MAX)
#    define _STDBIT128(x) ((x) ## _uc)
#  elif (UINT128_MAX == USHRT_MAX)
#    define _STDBIT128(x) ((x) ## _us)
#  elif (UINT128_MAX == UINT_MAX)
#    define _STDBIT128(x) ((x) ## _ui)
#  elif (UINT128_MAX == ULONG_MAX)
#    define _STDBIT128(x) ((x) ## _ul)
#  elif (UINT128_MAX == ULLONG_MAX)
#    define _STDBIT128(x) ((x) ## _ull)
#  endif
#endif


// Bit Functions ==============================================================
// When possible here, we use the stdbit functions; otherwise, we write little
// functions that will hopefully be inlined into pretty optimal machine code.

// popcount(x):
// Returns the number of set bits in the given unsigned integer x.
#ifdef _PCOLLECTIONS__STDBIT_H
#  ifdef _STDBIT8
#    define popcount8 _STDBIT8(stdc_count_ones)
#  endif
#  ifdef _STDBIT16
#    define popcount16 _STDBIT16(stdc_count_ones)
#  endif
#  ifdef _STDBIT32
#    define popcount32 _STDBIT32(stdc_count_ones)
#  endif
#  ifdef _STDBIT64
#    define popcount64 _STDBIT64(stdc_count_ones)
#  endif
#  ifdef _STDBIT128
#    define popcount128 _STDBIT128(stdc_count_ones)
#  endif
#else
   // A branch-free 32-bit population count; the other widths are built on
   // top of it.
   EXTC static inline uint32_t popcount32(uint32_t w) {
      w = w - ((w >> 1) & 0x55555555);
      w = (w & 0x33333333) + ((w >> 2) & 0x33333333);
      w = (w + (w >> 4)) & 0x0F0F0F0F;
      return (w * 0x01010101) >> 24;
   }
   EXTC static inline uint8_t popcount8(uint8_t w) {return popcount32(w);}
   EXTC static inline uint16_t popcount16(uint16_t w) {return popcount32(w);}
#  ifdef UINT64_MAX
      EXTC static inline uint64_t popcount64(uint64_t w) {
         return ( (uint64_t)popcount32((uint32_t)w)
                + (uint64_t)popcount32((uint32_t)(w >> 32)) );
      }
#  endif
#  ifdef UINT128_MAX
      EXTC static inline uint128_t popcount128(uint128_t w) {
         return ( (uint128_t)popcount32((uint32_t)w)
                + (uint128_t)popcount32((uint32_t)(w >> 32))
                + (uint128_t)popcount32((uint32_t)(w >> 64))
                + (uint128_t)popcount32((uint32_t)(w >> 96)) );
      }
#  endif
#endif

// clz(bits)
// Returns the number of leading zeros in the bits.
#ifdef _PCOLLECTIONS__STDBIT_H
#  ifdef _STDBIT8
#    define clz8 _STDBIT8(stdc_count_leading_zeros)
#  endif
#  ifdef _STDBIT16
#    define clz16 _STDBIT16(stdc_count_leading_zeros)
#  endif
#  ifdef _STDBIT32
#    define clz32 _STDBIT32(stdc_count_leading_zeros)
#  endif
#  ifdef _STDBIT64
#    define clz64 _STDBIT64(stdc_count_leading_zeros)
#  endif
#  ifdef _STDBIT128
#    define clz128 _STDBIT128(stdc_count_leading_zeros)
#  endif
#else
   EXTC static inline uint32_t clz32(uint32_t v) {
      v = v | (v >> 1);
      v = v | (v >> 2);
      v = v | (v >> 4);
      v = v | (v >> 8);
      v = v | (v >> 16);
      return popcount32(~v);
   }
   EXTC static inline uint8_t clz8(uint8_t w) {
      return (uint8_t)clz32((uint8_t)w) - 24;
   }
   EXTC static inline uint16_t clz16(uint16_t w) {
      return (uint16_t)clz32((uint32_t)w) - 16;
   }
#  ifdef UINT64_MAX
      EXTC static inline uint64_t clz64(uint64_t w) {
         uint64_t c1 = clz32((uint32_t)(w >> 32));
         uint64_t c2 = clz32((uint32_t)w);
         return (c1 == 32 ? 32 + c2 : c1);
      }
#  endif
#  ifdef UINT128_MAX
      EXTC static inline uint128_t clz128(uint128_t w) {
         uint32_t c1 = clz32((uint32_t)(w >> 96));
         uint32_t c2 = clz32((uint32_t)(w >> 64));
         uint32_t c3 = clz32((uint32_t)(w >> 32));
         uint32_t c4 = clz32((uint32_t)(w));
         c3 = (c3 == 32? 32 + c4 : c3);
         c2 = (c2 == 32? 32 + c3 : c2);
         c1 = (c1 == 32? 32 + c2 : c1);
         return (uint128_t)c1;
      }
#  endif
#endif

// ctz(bits)
// Returns the number of trailing zeros in the bits.
#ifdef _PCOLLECTIONS__STDBIT_H
#  ifdef _STDBIT8
#    define ctz8 _STDBIT8(stdc_count_trailing_zeros)
#  endif
#  ifdef _STDBIT16
#    define ctz16 _STDBIT16(stdc_count_trailing_zeros)
#  endif
#  ifdef _STDBIT32
#    define ctz32 _STDBIT32(stdc_count_trailing_zeros)
#  endif
#  ifdef _STDBIT64
#    define ctz64 _STDBIT64(stdc_count_trailing_zeros)
#  endif
#  ifdef _STDBIT128
#    define ctz128 _STDBIT128(stdc_count_trailing_zeros)
#  endif
#else
   EXTC static inline uint32_t ctz32(uint32_t v) {
      static const int deBruijn_values[32] = {
         0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
         31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
      };
      // `v & -v` (isolate the lowest set bit), spelled `(uint32_t)0 - v` to
      // avoid MSVC's C4146 warning about negating an unsigned value.
      return deBruijn_values[((uint32_t)((v & ((uint32_t)0 - v)) * 0x077CB531U)) >> 27];
   }
   EXTC static inline uint16_t ctz16(uint16_t w) {
      return ctz32((uint32_t)w);
   }
#  ifdef UINT64_MAX
      EXTC static inline uint64_t ctz64(uint64_t w) {
         uint32_t c = ctz32((uint32_t)w);
         return (c == 32 ? 32 + ctz32((uint32_t)(w >> 32)) : c);
      }
#  endif
#  ifdef UINT128_MAX
      EXTC static inline uint128_t ctz128(uint128_t w) {
         uint32_t c = ctz32((uint32_t)w);
         if (c < 32) return c;
         c = ctz32((uint32_t)(w >> 32));
         if (c < 32) return c + 32;
         c = ctz32((uint32_t)(w >> 64));
         if (c < 32) return c + 64;
         return ctz32((uint32_t)(w >> 96)) + 96;
      }
#  endif
#endif


// Other Utilities ============================================================

#ifdef UINT128_MAX
EXTC static inline uint128_t pow128(uint128_t base, uint128_t expt) {
   uint128_t result = UINT128_C(1);
   while (expt > 0) {
      result *= (expt & UINT128_C(1)? base : UINT128_C(1));
      base *= base;
      expt >>= 1;
   }
   return result;
}
#endif
EXTC static inline uint64_t pow64(uint64_t base, uint64_t expt) {
   uint64_t result = UINT64_C(1);
   while (expt > 0) {
      result *= (expt & UINT64_C(1)? base : UINT64_C(1));
      base *= base;
      expt >>= 1;
   }
   return result;
}
EXTC static inline uint32_t pow32(uint32_t base, uint32_t expt) {
   uint32_t result = UINT32_C(1);
   while (expt > 0) {
      result *= (expt & UINT32_C(1)? base : UINT32_C(1));
      base *= base;
      expt >>= 1;
   }
   return result;
}
EXTC static inline uint16_t pow16(uint16_t base, uint16_t expt) {
   uint16_t result = UINT16_C(1);
   while (expt > 0) {
      result *= (expt & UINT16_C(1)? base : UINT16_C(1));
      base *= base;
      expt >>= 1;
   }
   return result;
}
EXTC static inline uint8_t pow8(uint8_t base, uint8_t expt) {
   uint8_t result = UINT8_C(1);
   while (expt > 0) {
      result *= (expt & UINT8_C(1)? base : UINT8_C(1));
      base *= base;
      expt >>= 1;
   }
   return result;
}
// Various masking operations
EXTC static inline uint8_t ltmask8(uint8_t k) {
   return ~(UINT8_MAX << k);
}
EXTC static inline uint8_t lemask8(uint8_t k) {
   return ~(UINT8_MAX << (k+1));
}
EXTC static inline uint8_t gtmask8(uint8_t k) {
   return UINT8_MAX << (k+1);
}
EXTC static inline uint8_t gemask8(uint8_t k) {
   return UINT8_MAX << k;
}
EXTC static inline uint16_t ltmask16(uint16_t k) {
   return ~(UINT16_MAX << k);
}
EXTC static inline uint16_t lemask16(uint16_t k) {
   return ~(UINT16_MAX << (k+1));
}
EXTC static inline uint16_t gtmask16(uint16_t k) {
   return UINT16_MAX << (k+1);
}
EXTC static inline uint16_t gemask16(uint16_t k) {
   return UINT16_MAX << k;
}
EXTC static inline uint32_t ltmask32(uint32_t k) {
   return ~(UINT32_MAX << k);
}
EXTC static inline uint32_t lemask32(uint32_t k) {
   return ~(UINT32_MAX << (k+1));
}
EXTC static inline uint32_t gtmask32(uint32_t k) {
   return UINT32_MAX << (k+1);
}
EXTC static inline uint32_t gemask32(uint32_t k) {
   return UINT32_MAX << k;
}
EXTC static inline uint64_t ltmask64(uint64_t k) {
   return ~(UINT64_MAX << k);
}
EXTC static inline uint64_t lemask64(uint64_t k) {
   return ~(UINT64_MAX << (k+1));
}
EXTC static inline uint64_t gtmask64(uint64_t k) {
   return UINT64_MAX << (k+1);
}
EXTC static inline uint64_t gemask64(uint64_t k) {
   return UINT64_MAX << k;
}
#ifdef UINT128_MAX
EXTC static inline uint128_t ltmask128(uint128_t k) {
   return ~(UINT128_MAX << k);
}
EXTC static inline uint128_t lemask128(uint128_t k) {
   return ~(UINT128_MAX << (k+1));
}
EXTC static inline uint128_t gtmask128(uint128_t k) {
   return UINT128_MAX << (k+1);
}
EXTC static inline uint128_t gemask128(uint128_t k) {
   return UINT128_MAX << k;
}
#endif
// Finding the next binary power of an integer: For an integer x, the
// next binary power is the lowest y for which y > x and y is a power
// of 2.
// Note: the result overflows (wraps, per normal unsigned-integer semantics)
// when x is large enough that the true next power of 2 doesn't fit in the
// return type (e.g. nextbinpow8(200), since 256 isn't a uint8_t); callers
// that care about this range need to check for it themselves.
EXTC static inline uint8_t nextbinpow8(uint8_t k) {
   return (uint8_t)(UINT8_C(1) << (8 - clz8(k)));
}
EXTC static inline uint16_t nextbinpow16(uint16_t k) {
   return (uint16_t)(UINT16_C(1) << (16 - clz16(k)));
}
EXTC static inline uint32_t nextbinpow32(uint32_t k) {
   return (uint32_t)(UINT32_C(1) << (32 - clz32(k)));
}
#ifdef UINT64_MAX
EXTC static inline uint64_t nextbinpow64(uint64_t k) {
   return (uint64_t)(UINT64_C(1) << (64 - clz64(k)));
}
#endif
#ifdef UINT128_MAX
EXTC static inline uint128_t nextbinpow128(uint128_t k) {
   return (uint128_t)(UINT128_C(1) << (128 - clz128(k)));
}
#endif



// uintptr_t and size_t =======================================================
// There are a few macros not defined by POSIX that are useful for this library
// regarding the uintptr and size types. Specifically, the UINTPTR_C and SIZE_C
// macros, which let you declare constants of a particular width.

#ifndef UINTPTR_C
#  if (UINTPTR_WIDTH == 8)
#     define UINTPTR_C(x) UINT8_C(x)
#  elif (UINTPTR_WIDTH == 16)
#     define UINTPTR_C(x) UINT16_C(x)
#  elif (UINTPTR_WIDTH == 32)
#     define UINTPTR_C(x) UINT32_C(x)
#  elif (UINTPTR_WIDTH == 64)
#     define UINTPTR_C(x) UINT64_C(x)
#  elif (UINTPTR_WIDTH == 128)
#     define UINTPTR_C(x) UINT128_C(x)
#  else
#     error Cannot deduce size of uintptr_t.
#  endif
#endif
#ifndef SIZE_C
#  if (SIZE_WIDTH == 8)
#     define SIZE_C(x) UINT8_C(x)
#  elif (SIZE_WIDTH == 16)
#     define SIZE_C(x) UINT16_C(x)
#  elif (SIZE_WIDTH == 32)
#     define SIZE_C(x) UINT32_C(x)
#  elif (SIZE_WIDTH == 64)
#     define SIZE_C(x) UINT64_C(x)
#  elif (SIZE_WIDTH == 128)
#     define SIZE_C(x) UINT128_C(x)
#  else
#     error Cannot deduce size of size_t.
#  endif
#endif

// We can also define versions of the bit functions for these two types.
#if (UINTPTR_WIDTH == 8)
#  define popcount_uintptr   popcount8
#  define clz_uintptr        clz8
#  define ctz_uintptr        ctz8
#  define pow_uintptr        pow8
#  define ltmask_uintptr     ltmask8
#  define lemask_uintptr     lemask8
#  define gtmask_uintptr     gtmask8
#  define gemask_uintptr     gemask8
#  define nextbinpow_uintptr nextbinpow8
#elif (UINTPTR_WIDTH == 16)
#  define popcount_uintptr   popcount16
#  define clz_uintptr        clz16
#  define ctz_uintptr        ctz16
#  define pow_uintptr        pow16
#  define ltmask_uintptr     ltmask16
#  define lemask_uintptr     lemask16
#  define gtmask_uintptr     gtmask16
#  define gemask_uintptr     gemask16
#  define nextbinpow_uintptr nextbinpow16
#elif (UINTPTR_WIDTH == 32)
#  define popcount_uintptr   popcount32
#  define clz_uintptr        clz32
#  define ctz_uintptr        ctz32
#  define pow_uintptr        pow32
#  define ltmask_uintptr     ltmask32
#  define lemask_uintptr     lemask32
#  define gtmask_uintptr     gtmask32
#  define gemask_uintptr     gemask32
#  define nextbinpow_uintptr nextbinpow32
#elif (UINTPTR_WIDTH == 64)
#  define popcount_uintptr   popcount64
#  define clz_uintptr        clz64
#  define ctz_uintptr        ctz64
#  define pow_uintptr        pow64
#  define ltmask_uintptr     ltmask64
#  define lemask_uintptr     lemask64
#  define gtmask_uintptr     gtmask64
#  define gemask_uintptr     gemask64
#  define nextbinpow_uintptr nextbinpow64
#elif (UINTPTR_WIDTH == 128)
#  define popcount_uintptr   popcount128
#  define clz_uintptr        clz128
#  define ctz_uintptr        ctz128
#  define pow_uintptr        pow128
#  define ltmask_uintptr     ltmask128
#  define lemask_uintptr     lemask128
#  define gtmask_uintptr     gtmask128
#  define gemask_uintptr     gemask128
#  define nextbinpow_uintptr nextbinpow128
#endif
#if (SIZE_WIDTH == 8)
#  define popcount_size   popcount8
#  define clz_size        clz8
#  define ctz_size        ctz8
#  define pow_size        pow8
#  define ltmask_size     ltmask8
#  define lemask_size     lemask8
#  define gtmask_size     gtmask8
#  define gemask_size     gemask8
#  define nextbinpow_size nextbinpow8
#elif (SIZE_WIDTH == 16)
#  define popcount_size   popcount16
#  define clz_size        clz16
#  define ctz_size        ctz16
#  define pow_size        pow16
#  define ltmask_size     ltmask16
#  define lemask_size     lemask16
#  define gtmask_size     gtmask16
#  define gemask_size     gemask16
#  define nextbinpow_size nextbinpow16
#elif (SIZE_WIDTH == 32)
#  define popcount_size   popcount32
#  define clz_size        clz32
#  define ctz_size        ctz32
#  define pow_size        pow32
#  define ltmask_size     ltmask32
#  define lemask_size     lemask32
#  define gtmask_size     gtmask32
#  define gemask_size     gemask32
#  define nextbinpow_size nextbinpow32
#elif (SIZE_WIDTH == 64)
#  define popcount_size   popcount64
#  define clz_size        clz64
#  define ctz_size        ctz64
#  define pow_size        pow64
#  define ltmask_size     ltmask64
#  define lemask_size     lemask64
#  define gtmask_size     gtmask64
#  define gemask_size     gemask64
#  define nextbinpow_size nextbinpow64
#elif (SIZE_WIDTH == 128)
#  define popcount_size   popcount128
#  define clz_size        clz128
#  define ctz_size        ctz128
#  define pow_size        pow128
#  define ltmask_size     ltmask128
#  define lemask_size     lemask128
#  define gtmask_size     gtmask128
#  define gemask_size     gemask128
#  define nextbinpow_size nextbinpow128
#endif


// Cleanup ====================================================================

// At this point, we're done with the _STDBIT* macros, so we can undefine them.
#ifdef _STDBIT8
#  undef _STDBIT8
#endif
#ifdef _STDBIT16
#  undef _STDBIT16
#endif
#ifdef _STDBIT32
#  undef _STDBIT32
#endif
#ifdef _STDBIT64
#  undef _STDBIT64
#endif
#ifdef _STDBIT128
#  undef _STDBIT128
#endif

// We also want to make sure not to leak EXTC into other files.
#undef EXTC

#endif  //ifndef _PCOLLECTIONS__C_UINTBITS_H
