///////////////////////////////////////////////////////////////////////////////
// _c/trie.h
// Definitions and configuration for the trie data types used in the
// persistent collections library.
//
// The Trie type is a memory layout used by both array-mapped trie (AMT) and
// fixed arity trie (FAT) types. The AMT and FAT types are identical except
// that the AMT stores up to 32 cells in each node, each of which contains a
// child whose exact bitinidex is found using the occupancy bits, while the FAT
// type always stores exactly 29 cells, some of which contain children whose
// cellinidex matches their bitindex.
//
// Because AMT and FAT tries share most of their layout and code, as much
// functionality as possible is placed in the trienode_* functions.


//=============================================================================
// Initialization.

#ifndef _PCOLLECTIONS__C_TRIE_H
#define _PCOLLECTIONS__C_TRIE_H


#include <Python.h>
#include <stdbool.h>
#include <string.h>
#include "uintbits.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif

// Portable 64-bit atomic refcount -------------------------------------------
// Trie nodes are refcounted as plain C objects (not Python objects -- see
// the TrieHeader.refcount comment below), and that refcounting has to be
// atomic since a persistent trie node can be shared, and concurrently
// incref'd/decref'd, across threads. C11's <stdatomic.h> is the natural way
// to write that -- but MSVC has NO implementation of <stdatomic.h> at all
// unless the translation unit is built with /std:c11 or /std:c17, and
// setup.py deliberately does *not* pass that flag on Windows (see its own
// comment for why), so `#include <stdatomic.h>` fails outright there. This
// is exactly the same problem core.h's mutex/atomic-flag shim solves for
// its own <pthread.h>/<stdatomic.h> use (see that file's shim comment for
// the full rationale) -- this is the same fix, applied here instead of
// duplicating core.h's shim, since every trie node in dict.c.h/list.c.h/set.c.h
// (via amt.h/fat.h) shares this one refcount implementation through this
// header. `InterlockedExchangeAdd64` is a full-fence (a strictly stronger,
// and thus safe, superset of the acq_rel ordering the POSIX code below
// explicitly requests) 64-bit fetch-and-add intrinsic, stable since Windows
// Vista on x64 (the only Windows arch this project's CI targets -- see
// tests.yml), and -- like C11's atomic_fetch_add/atomic_fetch_sub_explicit
// -- it returns the value from *before* the operation, so passing -1 to it
// below reproduces `atomic_fetch_sub_explicit(..., 1, memory_order_acq_rel)`
// exactly (fetch_sub's return value is defined as what fetch_add with the
// negated operand would return).
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef volatile LONG64 pcoll_atomic_u64_t;
#  define PCOLL_ATOMIC_U64_FETCH_ADD1(ptr) \
      ((uint64_t)InterlockedExchangeAdd64((ptr), 1))
#  define PCOLL_ATOMIC_U64_FETCH_SUB1(ptr) \
      ((uint64_t)InterlockedExchangeAdd64((ptr), -1))
#else
#  include <stdatomic.h>
   typedef _Atomic uint64_t pcoll_atomic_u64_t;
#  define PCOLL_ATOMIC_U64_FETCH_ADD1(ptr) atomic_fetch_add((ptr), 1)
#  define PCOLL_ATOMIC_U64_FETCH_SUB1(ptr) \
      atomic_fetch_sub_explicit((ptr), 1, memory_order_acq_rel)
#endif


//=============================================================================
// Trie Integers Setup

//-----------------------------------------------------------------------------
// trieint_t: The unsigned integer key type.
// Python itself uses signed integers for hashes (Py_hash_t), but we want to
// make sure our code uses *unsigned* integers internally.  In this case, the
// size_t type is the same size as Py_hash_t (which is just defined from
// Py_ssize_t, which in turn is defined from ssize_t, which is signed but the
// same size as size_t).
EXTC typedef size_t trieint_t;
// The max value of the hash is the same as the max value of a size_t integer.
#define TRIEINT_C(x)       SIZE_C(x)
#define TRIEINT_WIDTH      SIZE_WIDTH
#define TRIEINT_MAX        SIZE_MAX
#define TRIEINT_1          SIZE_C(1)
#define TRIEINT_0          SIZE_C(0)
// We also need to define some functions for hashes:
#define popcount_trieint   popcount_size
#define clz_trieint        clz_size
#define ctz_trieint        ctz_size
#define ltmask_trieint     ltmask_size
#define lemask_trieint     lemask_size
#define gtmask_trieint     gtmask_size
#define gemask_trieint     gemask_size
#define nextbinpow_trieint nextbinpow_size


//==============================================================================
// The tribits_t Integer Type

//-----------------------------------------------------------------------------
// triebits_t: The bits type.
// The bits type needs to be an unsigned integer type that has at least (1 <<
// AMT_DIVBITS) bits. The value of AMT_DIVBITS is usually 5 (requiring 32
// bits), so we'll use that number here as an example:
// There's a legitimate question here about whether triebits_t should be
// uint32_t or uint_fast32_t. The former is guaranteed to be exactly 32 bits,
// which will potentially save space; the latter is supposed to be faster for
// certain operations on some systems. If the fast version is actually faster
// in our code, it's probably because uint_fast32_t is a uint64_t on a 64-bit
// system where a uint32_t would be promoted prior to some hardware operation.
// This will likely save no more than a few nanoseconds per lookup operation at
// best. On the othr hand, it's likely to require only a few extra bytes of
// storage per node, so pick your optimization.
#define TRIEBITS_WIDTH 32
#if (defined(UINT8_WIDTH) && (TRIEBITS_WIDTH == UINT8_WIDTH))
   EXTC typedef uint8_t triebits_t;
#  define TRIEBITS_C(x)       UINT8_C(x)
#  define TRIEBITS_MAX        UINT8_MAX
#  define popcount_triebits   popcount8
#  define clz_triebits        clz8
#  define ctz_triebits        ctz8
#  define ltmask_triebits     ltmask8
#  define lemask_triebits     lemask8
#  define gtmask_triebits     gtmask8
#  define gemask_triebits     gemask8
#  define nextbinpow_triebits nextbinpow8
#elif (defined(UINT16_WIDTH) && (TRIEBITS_WIDTH == UINT16_WIDTH))
   EXTC typedef uint16_t triebits_t;
#  define TRIEBITS_C(x)       UINT16_C(x)
#  define TRIEBITS_MAX        UINT16_MAX 
#  define popcount_triebits   popcount16
#  define clz_triebits        clz16
#  define ctz_triebits        ctz16
#  define ltmask_triebits     ltmask16
#  define lemask_triebits     lemask16
#  define gtmask_triebits     gtmask16
#  define gemask_triebits     gemask16
#  define nextbinpow_triebits nextbinpow16
#elif (defined(UINT32_WIDTH) && (TRIEBITS_WIDTH == UINT32_WIDTH))
   EXTC typedef uint32_t triebits_t;
#  define TRIEBITS_C(x)       UINT32_C(x)
#  define TRIEBITS_MAX        UINT32_MAX 
#  define popcount_triebits   popcount32
#  define clz_triebits        clz32
#  define ctz_triebits        ctz32
#  define ltmask_triebits     ltmask32
#  define lemask_triebits     lemask32
#  define gtmask_triebits     gtmask32
#  define gemask_triebits     gemask32
#  define nextbinpow_triebits nextbinpow32
#elif (defined(UINT64_WIDTH) && (TRIEBITS_WIDTH == UINT64_WIDTH))
   EXTC typedef uint64_t triebits_t;
#  define TRIEBITS_C(x)       UINT64_C(x)
#  define TRIEBITS_MAX        UINT64_MAX 
#  define popcount_triebits   popcount64
#  define clz_triebits        clz64
#  define ctz_triebits        ctz64
#  define ltmask_triebits     ltmask64
#  define lemask_triebits     lemask64
#  define gtmask_triebits     gtmask64
#  define gemask_triebits     gemask64
#  define nextbinpow_triebits nextbinpow64
#elif (defined(UINT128_WIDTH) && (TRIEBITS_WIDTH == UINT128_WIDTH))
   EXTC typedef uint128_t triebits_t;
#  define TRIEBITS_C(x)       UINT128_C(x)
#  define TRIEBITS_MAX        UINT128_MAX
#  define popcount_triebits   popcount128
#  define clz_triebits        clz128
#  define ctz_triebits        ctz128
#  define ltmask_triebits     ltmask128
#  define lemask_triebits     lemask128
#  define gtmask_triebits     gtmask128
#  define gemask_triebits     gemask128
#  define nextbinpow_triebits nextbinpow128
#else
#  error Could not deduce type of triebits_t.
#endif
// Some utility macros for the TRIEBITS type.
#define TRIEBITS_0 TRIEBITS_C(0)
#define TRIEBITS_1 TRIEBITS_C(1)


//=============================================================================
// Array Mapped Trie Setup

// The bits per layer of a Trie.
// Across multiple implementations (e.g., Clojure and Scala, which have a
// shared history) this value is always 5, resulting in 32 buckets per node. In
// testing this on my own desktop, 5 does give better performance than 6 or 4.
#define AMT_DIVBITS 5

// We can also define a few pieces of meta-data about the hash type now, using
// the above.
#define AMT_REMBITS   (TRIEINT_WIDTH % AMT_DIVBITS)
#define AMT_LAYERS    ((TRIEINT_WIDTH + AMT_DIVBITS - TRIEINT_1) / AMT_DIVBITS)
#define AMT_MAX_DEPTH (AMT_LAYERS - TRIEINT_1)
#define AMT_MAX_CELLS (TRIEINT_1 << AMT_DIVBITS)

// The masks for layers with either the diviser or the mod number of bits.
#define AMT_DIVMASK (~(TRIEINT_MAX << AMT_DIVBITS))
#define AMT_REMMASK (~(TRIEINT_MAX << AMT_REMBITS))

// Definitions for AMT nodes.
#define AMT_ROOT_BITS  AMT_DIVBITS
#define AMT_NODE_BITS  AMT_DIVBITS
#define AMT_TWIG_BITS  AMT_REMBITS
#define AMT_ROOT_SHIFT (TRIEINT_WIDTH - AMT_ROOT_BITS)
#define AMT_TWIG_SHIFT 0
#define AMT_NODE_MASK  AMT_DIVMASK
#define AMT_TWIG_MASK  AMT_REMMASK

#define AMT_NODE_CELLS (1 << AMT_NODE_BITS)
#define AMT_TWIG_CELLS (1 << AMT_TWIG_BITS)

// And some handy map/seq operations.
static inline uint8_t amtdepth_shift(uint8_t depth) {
   return (depth < AMT_MAX_DEPTH? AMT_ROOT_SHIFT - depth*AMT_NODE_BITS : 0);
}
static inline triebits_t amtdepth_mask(uint8_t depth) {
   return (depth < AMT_MAX_DEPTH? AMT_DIVBITS : AMT_REMBITS);
}
static inline triebits_t amtdepth_shiftmask(uint8_t depth) {
   return TRIEINT_WIDTH - depth*AMT_NODE_BITS;
}
static inline triebits_t amtdepth_bitindex(uint8_t depth, trieint_t h) {
   return (
      depth < AMT_MAX_DEPTH
      ? (h >> amtdepth_shift(depth)) & AMT_DIVMASK
      : h & AMT_REMMASK);
}
static inline uint8_t amtdepth_maxcells(uint8_t depth) {
   return (
      depth < AMT_MAX_DEPTH
      ? AMT_NODE_CELLS
      : AMT_TWIG_CELLS);
}
static inline bool amtdepth_prefix_match(triebits_t depth,
                                         trieint_t prefix,
                                         trieint_t k) {
   triebits_t shmask = amtdepth_shiftmask(depth);
   // At depth 0 (the root's own window is the topmost AMT_DIVBITS bits),
   // shmask is exactly TRIEINT_WIDTH: there are no bits above the root left
   // to compare, so the match is vacuously true. A shift by the full width
   // is undefined behavior in C, and on x86 a 64-bit shift silently wraps
   // to a no-op (shift-by-0) rather than "shift everything out", which
   // would otherwise turn this into "match iff the entire key is
   // bit-for-bit identical to the stored prefix" -- wrong for every root
   // (or other depth-0) node.
   if (shmask >= TRIEINT_WIDTH)
      return true;
   return (prefix >> shmask) == (k >> shmask);
}

//=============================================================================
// Fixed Arity Trie Setup

// For FATs, we want to use a branching factor at each layer in the trie that
// optimizes cache performance. We pick 29 nodes because, given our header size
// below, it allows us to hold 1 node in 256 bytes, which fits inside of 4
// cache lines.
#define FAT_CELLS 29
// This gives us a number of layers and a divisor for each layer, depending on
// our trieint width.
// Note that if you want to change FAT_CELLS, you'll need to recalculate these
// numbers by solving for the minimum integer k in 2^T <= N^k where T is
// TRIEINT_WIDTH and N is FAT_CELLS.
#if (FAT_CELLS == 29)
#  if (TRIEINT_WIDTH == 8)
#    define FAT_MAX_DEPTH 1
#  elif (TRIEINT_WIDTH == 16)
#    define FAT_MAX_DEPTH 3
#  elif (TRIEINT_WIDTH == 32)
#    define FAT_MAX_DEPTH 6
#  elif (TRIEINT_WIDTH == 64)
#    define FAT_MAX_DEPTH 13
#  elif (TRIEINT_WIDTH == 128)
#    define FAT_MAX_DEPTH 26
#  endif
#else
   // If you get this error, it's because you've changed FAT_CELLS and need to
   // manually redefine the FAT_MAX_DEPTH values above.
#  error FAT_MAX_DEPTH was not defined, probably because FAT_CELLS was changed.
#endif
// From this we can deduce a few other things; the number of layers for one.
// Another is the set of divisors used to extract the bit indices from the key;
// whereas AMTs use a simpler method of shiftings bits around (because they use
// a branching factor of 32, a power of 2), FATs use a branching factor of 29
// so that they can fit in 256 bytes of memory, complicating the layer
// arithmetic. Here we insert precalculated values for FAT_CELLS = 29.
#define FAT_LAYERS (FAT_MAX_DEPTH + 1)

// We also need to manually define the divisors for each layer of the FAT.
// Each entry below is a power of 29 (up to 29^13 for a 64-bit trieint_t, or
// 29^26 for 128-bit) -- large enough that computing it as a plain `int`
// product (the literal `29` defaults to `int`) silently overflows well
// before the higher powers are reached: signed integer overflow is
// undefined behavior in C, and in practice the compiler warns about it and
// folds the expression to a wrapped, wrong value at compile time (confirmed
// via -Wall: e.g. 29^13 folded to a nonsense 8-digit result instead of the
// correct ~20-digit value). Every one of the shallower FAT depths silently
// got a garbage divisor as a result, corrupting fatdepth_bitindex() for any
// node above roughly depth 7 -- fatal, since fat_lookup(), fat_subjoin(),
// and every path-descent function depend on it (caught the hard way: a
// 2-key FAT tree lost its first key immediately, root-caused by tracing
// _fat_divs's printed values against hand-computed powers of 29). The fix
// is to force the entire product chain into trieint_t (or uint128_t, on a
// 128-bit build) arithmetic instead of `int` arithmetic: casting just the
// *first* literal in each left-associative product chain is enough, since
// C's usual arithmetic conversions then promote every subsequent operand
// once the running product is already the wider type.
#if (FAT_CELLS == 29)
   const trieint_t _fat_divs[] = {
#    if (TRIEINT_WIDTH == 128)
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
#    endif
#    if (TRIEINT_WIDTH >= 64)
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29 * 29,

        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29*29,
        TRIEINT_C(29)*29*29*29*29 * 29*29,
#    endif 
#    if (TRIEINT_WIDTH >= 32)
        TRIEINT_C(29)*29*29*29*29 * 29,

        TRIEINT_C(29)*29*29*29*29,
        TRIEINT_C(29)*29*29*29,
#    endif 
#    if (TRIEINT_WIDTH >= 16)
        TRIEINT_C(29)*29*29,
        TRIEINT_C(29)*29,
#    endif
        TRIEINT_C(29),
        1
   };
#else
   // If you get this error, it's because you've changed FAT_CELLS and need to
   // manually redefine the _fat_divs value above.
#  error _fat_divs was not defined, probably because FAT_CELLS was changed.
#endif

// Critically, the above data lets us define some helper methods.
static inline trieint_t fatdepth_div(uint8_t depth) {
   return _fat_divs[depth];
}
static inline trieint_t fatdepth_bitindex(uint8_t depth, trieint_t k) {
   return (k / fatdepth_div(depth)) % TRIEINT_C(FAT_CELLS);
}
static inline bool fatdepth_prefix_match(uint8_t depth,
                                         trieint_t prefix, trieint_t k) {
   trieint_t div = fatdepth_div(depth) * TRIEINT_C(FAT_CELLS);
   return (depth == 0) | ((prefix / div) == (k / div));
}


//==============================================================================
// The TrieHeader and Trie Data Structures

// The header data for any trie.
EXTC typedef struct TrieHeader {
   // -------------------------------------------------------------------------
   // First we have critical header data.
   // The refcount. We refcount trie nodes as C objects and not as Python
   // objects. This makes them more space-efficient and faster, but it means
   // we have to do garbage collection ourselves.
   // Size: 8 bytes (64 bits)
   pcoll_atomic_u64_t refcount;
   // The hash prefix of the node. This prefix is always stored in unshifted
   // bits, meaning that for a hash key k and an AMT node's real shift amount
   // s (see amtnode_shift() below), you can tell if k falls beneath the node
   // by comparing (k >> s) == (prefix >> s). `prefix` is always stored
   // EXACTLY as given -- no flag bits are ever packed into it, for either
   // AMT or FAT nodes. See "The trieint_t is_transient flag" comment below
   // for where is_transient actually lives and why (short version: FAT's
   // base-29 div/mod digit arithmetic can't tolerate ANY bit of `prefix`
   // being stolen for a flag, so it lives in a dedicated `flags` byte
   // instead, uniformly for AMT and FAT alike). There is no is_fat flag
   // anywhere in this header at all -- see that same comment for why one
   // isn't needed.
   // Size: 8 bytes (64 bits)
   trieint_t prefix;
   // -------------------------------------------------------------------------
   // [16-byte mark] Alignment with 16-byte blocks isn't critical, but it's
   //    fairly likely that 64-byte marks matter in terms of cache performance.
   // -------------------------------------------------------------------------
   // The occupancy bits (which cells are filled).
   // Size: 4 bytes (32 bits)
   triebits_t bits;
   // Other metadata stored by a trie node.
   uint8_t depth;  // The node's trie depth.
   uint8_t leafsize;  // The size of leaves in the twig nodes.
   // `flags` holds only is_transient (bit 0 -- see "The trieint_t
   // is_transient flag" comment below), for both AMT and FAT nodes alike,
   // via the exact same bitmask, checked without first needing to know
   // which kind of node this is.
   // An earlier design stored an AMT node's real bit-shift amount in this
   // byte (there was no need for the shift itself to persist: it's just
   // AMT_ROOT_SHIFT - depth*AMT_NODE_BITS, a couple of cheap ALU ops from
   // `depth`, which is already loaded from this same cache line by
   // essentially every operation anyway) and stole its spare top bit for
   // is_transient. That worked -- an AMT shift is always < TRIEINT_WIDTH,
   // currently capped at 128, so it never came close to needing bit 7 --
   // but it meant one more "don't read this field raw, you have to mask it
   // first" trap (the same shape of bug that once corrupted FAT's prefix
   // field), for a value nothing in this codebase currently even calls
   // (amtnode_minleaf()/amtnode_maxleaf() are the only readers, and were
   // unused). Recomputing on the rare occasions the real shift is actually
   // needed (see amtnode_shift() below) costs a handful of register-only
   // instructions -- confirmed by compiling both versions and comparing
   // the generated code -- which is strictly cheaper than computing AND
   // storing it on every node construction, given nothing was reading the
   // stored value. So this byte is now nothing but flags, plainly.
   uint8_t flags;
   // `ncells` is how many cells are allocated in this node -- a real,
   // ordinary count, for AMT nodes only (meaningless for FAT, which always
   // has exactly FAT_CELLS cells and never reads or writes this field).
   // Unlike an earlier design, no bit of `ncells` is reserved for a flag:
   // there is no is_fat bit anywhere in a trie node's header (see "The
   // trieint_t is_transient flag" comment below for why one was removed).
   uint8_t ncells;
   // Total: 24 bytes.
} *TrieHeader_t;

// The Trie type is a sort of "abstract" c structure in that it can be used to
// point to either an AMT_t or a FAT_t, both of which use identical memory
// structures.
EXTC typedef struct TrieData {
   // First the header: 24 bytes.
   struct TrieHeader header;
   // Then the cells.
   char cells[];
} *Trie_t;

// We use a dummy Trie object in the lookup function as a sort of hack for
// enabling a branchless algorithm. We want this to exist in the data segment
// but to have a full set of cells; therefore, we need to put it in another
// structure to make it viable.
//
// This struct deliberately does NOT embed `struct TrieData` (which is what
// its name might suggest, and is what an earlier version of this file did):
// TrieData ends in `char cells[]`, a C99 flexible array member (FAM), and
// embedding a FAM-terminated struct as a *non-final* member of another
// struct -- which is exactly what a `struct TrieData trie;` field followed
// by anything else amounts to -- is undefined behavior per the C standard
// (a struct containing a FAM "shall not appear ... as a member of any
// structure"). GCC/Clang tolerate this as a well-established extension (this
// compiled and ran correctly, and passed every test, on Linux/macOS for as
// long as this project has existed), but MSVC does not: it fails with
// "error C2229: struct 'TrieDummyData' has an illegal zero-sized array" on
// the `cells[TRIEBITS_WIDTH]` member below, precisely because it correctly
// refuses to let anything follow a member whose type ends in a flexible
// array. So: skip the wrapper and declare the *same* leading layout
// directly -- a `struct TrieHeader header` (identical to TrieData's own
// first member, and at the same offset 0, since TrieHeader is itself
// FAM-free) immediately followed by a real, fixed-size `cells` array. Every
// use of this struct (see amt_lookup()/fat_lookup() below) only ever takes
// the ADDRESS of the whole struct and reinterprets it as `Trie_t` (a
// `struct TrieData*`) -- never actually dereferencing a `.trie` field -- so
// this is purely a struct-shape change: the header start at offset 0
// followed immediately by the cells (TrieHeader's own size is a multiple of
// pointer alignment -- see its "Total: 24 bytes" comment above -- so
// `cells` lands at the identical offset either way, with no layout change
// for any code that follows a Trie_t pointer's header/cells regions).
EXTC struct TrieDummyData {
   struct TrieHeader header;
   void* cells[TRIEBITS_WIDTH];
};


//-----------------------------------------------------------------------------
// Trie Methods
// Methods are prefixed with various strings:
// - Any method trie_* such as trie_is_transient() is intended as a public
//   interface method for any trie object such as the user can obtain using
//   these public functions; these trie objects differ from those that might
//   be found internally to a trie by, for example, crawling the trie's
//   subnodes.
// - Any method trienode_* such as trienode_prefix() is intended as a private
//   method for use within the module. Unlike an earlier design, these are
//   never a kind-dispatching wrapper around separate amtnode_*/fatnode_*
//   submethods -- there is no is_fat flag to dispatch on (see "The trieint_t
//   is_transient flag" comment above), and no genuine need for one, since
//   every trienode_* function here is instead written to work identically
//   on either kind's node directly (pure header/cell field access, e.g.
//   trienode_occupancy() or trienode_subt()), with no kind-dependent branch
//   inside it at all.
// - For AMT and FAT nodes, a similar paradigm applies with the prefixes
//   amt_ / amtnode_ and fat_ / fatnode_ -- these DO differ in behavior by
//   kind (that's the whole point of having two of them), but each one only
//   ever touches nodes of its own kind, so which one to call is always
//   already decided by the caller's own context, never by inspecting the
//   node passed in.

// The trieint_t is_transient flag.
// ---------------------------------------------------------------------------
// A note on where is_transient actually lives, and why there is no is_fat
// flag at all:
//
// An earlier design packed both an is_transient flag AND an is_fat flag
// into the low bits of `prefix`, alongside the real numeric prefix value.
// That works fine for AMT: every AMT prefix comparison is a power-of-2
// bit-shift/mask (`(k >> shift) == (prefix >> shift)`), and every non-twig
// depth's shift is large enough (a multiple of AMT_DIVBITS, i.e. >= 5) that
// the low bits are already "don't care" for the comparison -- clearing or
// overwriting them changes nothing.
//
// FAT prefix comparisons, by contrast, are base-29 DIVISION/MODULO
// operations (fatdepth_prefix_match() computes prefix/div and key/div for
// div a power of 29), not bit-shifts. Div-by-N and mask-low-bits are not
// interchangeable the way div-by-power-of-2 and mask-low-bits are: an exact
// multiple of div, once its low bits are cleared (or overwritten by an
// OR'd-in flag), is in general no longer FLOOR-DIVISION-equal to that same
// multiple -- concretely, floor((m*div - r)/div) == m - 1, not m, for any
// r > 0 (an exact multiple sits at the very bottom edge of its own
// division bucket, so *any* nonzero adjustment drops it into the previous
// bucket, not just a large one). This was caught the hard way: a freshly
// built 2-key FAT tree lost its first key immediately after the second
// insert, root-caused by tracing the actual stored `prefix` field of the
// resulting node against a hand-computed value and finding it off by
// exactly a flag bit's contribution.
//
// So `prefix` cannot have ANY bits stolen from it -- not even one, and not
// just for FAT -- without corrupting FAT's digit arithmetic (AMT tolerates
// it, but there's no reason to keep two different rules for the two kinds
// when one rule that works for both is available). is_transient therefore
// lives in its own dedicated `flags` byte instead (bit 0), uniformly for
// AMT and FAT alike, one mechanism rather than two.
//
// An earlier design put is_transient in the spare top bit of `shift`
// instead of giving it a byte of its own, on the reasoning that an AMT
// node's real shift value never gets anywhere near bit 7. That was true
// (and would still be true up to the widest hash width this trie
// currently supports), but it was solving a problem that didn't need
// solving: nothing in this codebase actually reads an AMT node's real
// shift value on any hot path -- amtnode_minleaf()/amtnode_maxleaf() are
// the only two places that ever want it, and it's a couple of cheap ALU
// ops to recompute from `depth` (which every caller has already loaded
// anyway) rather than a real per-node cost to store -- see amtnode_shift()
// below. So `shift` was retired as a stored field entirely: no numeric
// shift value lives in the header at all anymore, which means there's
// nothing left in `flags` to protect a stolen bit from, and no "remember
// to mask this before using it as a number" trap for a future maintainer
// to fall into (the same shape of bug that once corrupted FAT's prefix
// field).
//
// is_fat, similarly, doesn't exist anywhere in a trie node's header at
// all -- there was previously a bit stashed in header.ncells's top bit,
// readable via one uniform computation on ANY node without first knowing
// which kind it was. That flag turned out to be pure overhead too: every
// place in this codebase that ever asked "is this node AMT or FAT?"
// already knew the answer from *context*, not from the node itself. An AMT
// node's cells only ever point to other AMT nodes, and a FAT node's cells
// only ever point to other FAT nodes -- the two trees never interleave --
// so every function here is written as either an amt_*/amtnode_* function
// (which only ever touches AMT nodes) or a fat_*/fatnode_* function (which
// only ever touches FAT nodes); nothing in this file ever receives a Trie_t
// whose kind is genuinely unknown to its caller. (Below the C layer, the
// Python-level PAMT/TAMT/PFAT/TFAT wrapper types each know their own kind
// by construction too, for the same reason.) Wherever this file used to
// call the generic, kind-dispatching trienode_is_twig()/trienode_decref()/
// trienode_free() from within a function that already statically knew its
// own kind (amtnode_free() calling trienode_is_twig() on a node it already
// knows is an AMT node, say), it now just calls the kind-specific
// amtnode_is_twig()/amtnode_decref()/etc. or fatnode_* equivalent directly.
#define TRIE_FLAG_ISTRANSIENT UINT8_C(0x01)

// trienode_prefix() returns the numeric prefix, suitable for arithmetic
// (AMT's shift/mask arithmetic, or FAT's div/mod arithmetic). Neither kind
// ever steals a bit from `prefix` for flag storage, so this is just
// `t->header.prefix`, unconditionally, for either kind -- kept as its own
// function (rather than inlined at each call site) so callers don't need
// to care whether that remains true in the future.
static inline trieint_t trienode_prefix(const Trie_t t) {
   return t->header.prefix;
}
static inline bool trie_is_persistent(const Trie_t t) {
   return (t->header.flags & TRIE_FLAG_ISTRANSIENT) == 0;
}
static inline bool trie_is_transient(const Trie_t t) {
   return (t->header.flags & TRIE_FLAG_ISTRANSIENT) != 0;
}
static inline void trienode_set_transient(Trie_t th, bool is_tr) {
   if (is_tr)
      th->header.flags |= TRIE_FLAG_ISTRANSIENT;
   else
      th->header.flags &= (uint8_t)~TRIE_FLAG_ISTRANSIENT;
}
static inline bool amtnode_is_twig(const Trie_t th) {
   return th->header.depth == AMT_MAX_DEPTH;
}
static inline bool fatnode_is_twig(const Trie_t th) {
   return th->header.depth == FAT_MAX_DEPTH;
}
static inline bool amtnode_prefix_match(const Trie_t th, trieint_t k) {
   return amtdepth_prefix_match(th->header.depth, th->header.prefix, k);
}
static inline bool fatnode_prefix_match(const Trie_t th, trieint_t k) {
   return fatdepth_prefix_match(th->header.depth, th->header.prefix, k);
}
// amtnode_shift() returns an AMT node's real numeric shift amount. This is
// no longer a stored field (see "The trieint_t is_transient flag" comment
// above): it's recomputed from `depth`, which the caller has virtually
// always already loaded from this same header for some other reason.
// Meaningless for a FAT node (which has no notion of a bit-shift at all);
// only call this on a node already known to be AMT.
static inline uint8_t amtnode_shift(const Trie_t th) {
   return amtdepth_shift(th->header.depth);
}
static inline trieint_t amtnode_minleaf(const Trie_t th) {
   return (th->header.prefix & (TRIEINT_MAX << amtnode_shift(th)));
}
static inline trieint_t amtnode_maxleaf(const Trie_t th) {
   return (th->header.prefix | ~(TRIEINT_MAX << amtnode_shift(th)));
}
static inline trieint_t fatnode_minleaf(const Trie_t th) {
   return (th->header.prefix / fatdepth_div(th->header.depth));
}
static inline trieint_t fatnode_maxleaf(const Trie_t th) {
   return fatnode_minleaf(th) * TRIEINT_C(FAT_CELLS) - TRIEINT_1;
}
static inline bool trienode_hasbit(const Trie_t th, triebits_t bitindex) {
   return (th->header.bits & (TRIEBITS_1 << bitindex)) > 0;
}
static inline triebits_t amtnode_bit2cellindex(const Trie_t amt,
                                               triebits_t bitindex) {
   return popcount_triebits(amt->header.bits & ltmask_triebits(bitindex));
}
static inline triebits_t fatnode_bit2cellindex(const Trie_t fat,
                                               triebits_t bitindex) {
   return bitindex;
}
static inline triebits_t trienode_occupancy(const Trie_t th) {
   return popcount_triebits(th->header.bits);
}
// The _subt and _leaf versions of theses functions are for extracting from
// either a core node (_subt for subtrie) or a twig node (_leaf). The
// _leaf version uses the leafsize; the subt reads from the cell and returns
// the read pointer, but neither ensures that the node is of the right type.
// If you're not sure what kind of node you have use the _cells version.
static inline Trie_t trienode_subt(const Trie_t th, trieint_t cellindex) {
   return ((Trie_t*)th->cells)[cellindex];
}
static inline Trie_t trienode_leaf(const Trie_t th, trieint_t cellindex) {
   return (void*)(th->cells + cellindex*th->header.leafsize);
}
static inline void trienode_set_subt(Trie_t th,
                                     trieint_t cellindex,
                                     Trie_t subt) {
   ((Trie_t*)th->cells)[cellindex] = subt;
}
static inline void trienode_set_leaf(Trie_t th,
                                     trieint_t cellindex,
                                     void* data) {
   memcpy(th->cells + cellindex*th->header.leafsize, data, th->header.leafsize);
}
static inline size_t amtnode_cellsize(const Trie_t node) {
   return amtnode_is_twig(node)? node->header.leafsize : sizeof(void*);
}
static inline size_t fatnode_cellsize(const Trie_t node) {
   return fatnode_is_twig(node)? node->header.leafsize : sizeof(void*);
}
// Bitindex and cellindex scanning functions for iterating over tries.
// Both of the following are written to tolerate two things a naive
// ctz-based implementation gets wrong:
//  1. "No more set bits" must come back as some index >= TRIEBITS_WIDTH
//     (i.e. past the end of any real cell range, FAT_CELLS included) so
//     that a caller's "while (bi < N)" scan terminates. Whether ctz(0)
//     itself returns TRIEBITS_WIDTH depends on which ctz_triebits
//     implementation this build picked: the C23 <stdbit.h> path guarantees
//     it, but the manual De Bruijn fallback (used whenever <stdbit.h> isn't
//     available -- confirmed via a standalone check to be what this build
//     actually uses) returns 0 for a zero input instead. Both functions
//     below special-case the "no bits left" condition explicitly rather
//     than relying on ctz(0)'s value, so they behave identically on either
//     implementation.
//  2. trienode_next_bitindex(th, prev) must find the next set bit *after*
//     prev, not prev itself. Shifting by `prev` (rather than `prev + 1`)
//     leaves bit 0 of the shifted value equal to bit `prev` of the
//     original -- which is always set, since `prev` is by construction a
//     previously-found set bit -- so ctz of that shifted value is always
//     0, making the naive `ctz(bits >> prev) + prev` formula return `prev`
//     right back, unconditionally. Every caller that scans with this
//     pattern (fattwig_free, fatnode_free, and the FAT set-cell iteration
//     used elsewhere) would loop forever the moment it tried to advance
//     past its first hit -- confirmed empirically, and never previously
//     exercised since FAT wasn't wired up to anything that actually built
//     or freed real bit-populated nodes until now. Shifting by `prev + 1`
//     instead fixes this: bit 0 of the shifted value is now the bit right
//     after `prev`, so ctz of it correctly measures the gap to the next
//     set bit (or reports "none left" per point 1 above).
static inline triebits_t trienode_first_bitindex(Trie_t th) {
   triebits_t bits = th->header.bits;
   return (bits == 0)? TRIEBITS_WIDTH : ctz_triebits(bits);
}
static inline triebits_t trienode_next_bitindex(Trie_t th, triebits_t prev) {
   triebits_t nextpos = prev + 1;
   triebits_t rest;
   if (nextpos >= TRIEBITS_WIDTH)
      return TRIEBITS_WIDTH;
   rest = th->header.bits >> nextpos;
   return (rest == 0)? TRIEBITS_WIDTH : (ctz_triebits(rest) + nextpos);
}
static inline triebits_t amtnode_first_cellindex(Trie_t th) {
   return TRIEBITS_0;
}
static inline triebits_t amtnode_next_cellindex(Trie_t th, triebits_t prev) {
   return prev + 1;
}
static inline triebits_t amtnode_sup_cellindex(Trie_t th) {
   return trienode_occupancy(th);
}
static inline triebits_t fatnode_first_cellindex(Trie_t th) {
   return trienode_first_bitindex(th);
}
static inline triebits_t fatnode_next_cellindex(Trie_t th, triebits_t prev) {
   return trienode_next_bitindex(th, prev);
}
static inline triebits_t fatnode_sup_cellindex(Trie_t th) {
   return TRIEBITS_C(FAT_CELLS);
}

// Memory and Reference Management --------------------------------------------

static inline void trienode_incref(Trie_t th) {
   PCOLL_ATOMIC_U64_FETCH_ADD1(&th->header.refcount);
}
// Note that after calling amtnode_decref(t) it is quite possible that t is no
// longer a valid pointer!
static inline void amttwig_free(Trie_t t, void (*leaf_decref)(void*)) {
   triebits_t ci, nocc;
   // We need to deallocate this node, which means we need to first dereference
   // each of the leaves.
   nocc = trienode_occupancy(t);
   for (ci = 0; ci < nocc; ++ci)
      (*leaf_decref)(trienode_leaf(t, ci));
   // Once that's done, we free the node.
   free(t);
}
static inline void amttwig_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->header.refcount);
   // If we aren't the last one holding the reference, we don't do anything.
   if (r <= 1)
      amttwig_free(t, leaf_deref);
}
static inline void fattwig_free(Trie_t t, void (*leaf_deref)(void*)) {
   triebits_t bi;
   // We need to deallocate this node, which means we need to first dereference
   // each of the leaves.
   for (bi = trienode_first_bitindex(t);
        bi < FAT_CELLS;
        bi = trienode_next_bitindex(t, bi))
      (*leaf_deref)(trienode_leaf(t, fatnode_bit2cellindex(t, bi)));
   // Once that's done, we free the node.
   free(t);
}
static inline void fattwig_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->header.refcount);
   // If we aren't the last one holding the reference, we don't do anything.
   if (r <= 1)
      fattwig_free(t, leaf_deref);
}
static inline void trietwig_decref_noprop(Trie_t t) {
   uint64_t r;
   r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->header.refcount);
   // If we aren't the last one holding the reference, we don't do anything.
   if (r > 1) return;
   // We need to deallocate this node, but since there's no leaf cleanup,
   // we just free the memory.
   free(t);
}
static inline void amtnode_free(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   Trie_t u, node;
   Trie_t nodes[AMT_LAYERS];
   uint8_t cis[AMT_LAYERS];
   uint8_t noccs[AMT_LAYERS];
   uint8_t ci, nocc, top;
   // We need to deallocate this node, so we need to decrement the reference
   // count on any child node or leaf. There may be arbitrarily many nodes
   // to visit.
   // However, once any node is being deallocated, we can overwrite the
   // first two words in its memory (the refcount and the prefix) and use
   // them as indicators of where we are in our dereferencing (and
   // deallocation) search.
   if (amtnode_is_twig(t)) {
      if (leaf_deref)
         amttwig_free(t, leaf_deref);
      else
         free(t);
      return;
   }
   node = t;
   ci = 0;
   nocc = trienode_occupancy(t);
   top = 0;
   // To start with, we write slightly different code depending on whether we
   // have a leaf dereference function or not.
   while (1) {
      if (ci < nocc) {
         // We need to process subtree ci of node.
         u = trienode_subt(node, ci);
         r = PCOLL_ATOMIC_U64_FETCH_SUB1(&u->header.refcount);
         if (r <= 1) {
            if (amtnode_is_twig(u)) {
               if (leaf_deref)
                  amttwig_free(u, leaf_deref);
               else
                  free(u);
            } else {
               // We need to deallocate this node too!
               noccs[top] = nocc;
               cis[top] = ci + 1;
               nodes[top] = node;
               ++top;
               nocc = trienode_occupancy(u);
               node = u;
               ci = 0;
               continue;
            }
         }
         ++ci;
      } else {
         // We're done with processing this node! We can deallocate it.
         free(node);
         // Then we pop the stack! If top is 0 we're already at the top
         // and can just return.
         if (top == 0) return;
         --top;
         node = nodes[top];
         ci = cis[top];
         nocc = noccs[top];
      }
   }
}
static inline void amtnode_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->header.refcount);
   if (r <= 1)
      amtnode_free(t, leaf_deref);
}
static inline void fatnode_free(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   Trie_t u, node;
   Trie_t nodes[AMT_LAYERS];
   uint8_t bis[AMT_LAYERS];
   uint8_t bi, top;
   // We need to deallocate this node, so we need to decrement the reference
   // count on any child node or leaf. There may be arbitrarily many nodes
   // to visit.
   // However, once any node is being deallocated, we can overwrite the
   // first two words in its memory (the refcount and the prefix) and use
   // them as indicators of where we are in our dereferencing (and
   // deallocation) search.
   // Twigs get handled differently:
   if (fatnode_is_twig(t)) {
      if (leaf_deref)
         fattwig_free(t, leaf_deref);
      else
         free(t);
      return;
   }
   node = t;
   bi = trienode_first_bitindex(t);
   top = 0;
   // To start with, we write slightly different code depending on whether we
   // have a leaf dereference function or not.
   while (1) {
      if (bi < FAT_CELLS) {
         // We need to process subtree ci of node.
         u = trienode_subt(node, bi);
         r = PCOLL_ATOMIC_U64_FETCH_SUB1(&u->header.refcount);
         if (r <= 1) {
            // We need to deallocate this node too!
            if (fatnode_is_twig(u)) {
               if (leaf_deref)
                  fattwig_free(u, leaf_deref);
               else
                  free(u);
            } else {
               nodes[top] = node;
               bis[top] = trienode_next_bitindex(node, bi);
               ++top;
               node = u;
               bi = trienode_first_bitindex(node);
               continue;
            }
         }
         bi = trienode_next_bitindex(node, bi);
      } else {
         // We're done with processing this node! We can deallocate it.
         free(node);
         // Then we pop the stack! If top is 0 we're already at the top
         // and can just return.
         if (top == 0) return;
         --top;
         node = nodes[top];
         bi = bis[top];
      }
   }
}
static inline void fatnode_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->header.refcount);
   if (r <= 1)
      fatnode_free(t, leaf_deref);
}
// There is deliberately no generic trienode_free()/trienode_decref() here
// (an earlier design had them, dispatching on the is_fat flag): every call
// site in amt.h decrefs a node it already knows is an AMT node, and every
// call site in fat.h decrefs a node it already knows is a FAT node, so they
// call amtnode_decref()/fatnode_decref() directly instead. See "The
// trieint_t is_transient flag" comment above for the full rationale.

// Lookup ---------------------------------------------------------------------

static inline int amt_lookup(Trie_t node,
                             trieint_t key,
                             void** result) {
   // The dummy's bits are always zero (a twig with no occupied cells), so no
   // lookup can ever actually read its cells; the leafsize is irrelevant and
   // is set to sizeof(void*) since that's a valid compile-time constant.
   static struct TrieDummyData amtnode_dummytwig_data = {
      .header = {
         .depth = AMT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   static Trie_t amtnode_dummytwig = (Trie_t)&amtnode_dummytwig_data;
   triebits_t depth, bitindex, cellindex, bit;
   trieint_t shift, keyshift;
   bool ok;
   void* cell;
   // In an AMT, we are not likely to get sequential depths of hash; instead,
   // we are likely to skip depths, so we check the depth on every iteration.
   depth = node->header.depth;
   while (depth < AMT_MAX_DEPTH) {
      // All non-twigs use the same code. First check the addresses match.
      shift = amtdepth_shift(depth);
      keyshift = (key >> shift);
      // It seems to make sense to check the prefix here, but actually we can
      // just do that in the twig node and skip it here.
      //ok = ((keyshift & ~AMT_DIVMASK) == (node->header.prefix >> shift));
      // Next, check if the appropriate bit is set.
      bitindex = keyshift & AMT_DIVMASK;
      bit = node->header.bits & (TRIEBITS_1 << bitindex);
      // Things are only okay if bit is truthy.
      ok = (bit > 0);
      // Now get the pointer to the cell itself. Note that amtnode_bit2cellindex()
      // returns popcount(bits below bitindex): if the queried bit ISN'T set
      // (ok is false) and every occupied bit happens to sort below it, this is
      // exactly equal to the node's occupancy -- i.e. one past the last valid
      // cell. A persistent node built via amt_and()/amt_but() is packed with
      // exactly as many cells as it has occupied bits (no slack), so reading
      // that one-past-the-end index is a real out-of-bounds heap read, not
      // just harmless speculation -- confirmed via AddressSanitizer. Since the
      // read cell is discarded below whenever !ok anyway, branchlessly mask
      // cellindex down to 0 (always a valid index for a real, non-dummy node,
      // which per the AMT invariants is never left with 0 occupancy) whenever
      // ok is false, keeping the read in-bounds without adding a branch.
      cellindex = amtnode_bit2cellindex(node, bitindex);
      // Written as `0 - ok` rather than `-ok`: identical result for an
      // unsigned type (two's-complement negation and "subtract from zero"
      // are the same bit pattern), but MSVC's C4146 ("unary minus operator
      // applied to unsigned type, result still unsigned") fires on the
      // unary-minus spelling even though the operation is completely
      // well-defined and intentional here; the subtraction spelling says
      // the same thing without tripping that warning.
      cellindex &= (triebits_t)((triebits_t)0 - (triebits_t)ok);
      cell = trienode_subt(node, cellindex);
      // Now, IF everything is okay, we set node to *cell; otherwise, we set
      // it to the dummy amt, which is always a twig and never contains any
      // cells.
      node = (ok? (Trie_t)cell : amtnode_dummytwig);
      depth = node->header.depth;
   }
   // This is a twig, so we return from this node.
   ok = amtnode_prefix_match(node, key);
   // Next, check if the appropriate bit is set.
   bitindex = (key & AMT_TWIG_MASK);
   bit = node->header.bits & (TRIEBITS_1 << bitindex);
   // Things are only okay if bit is truthy.
   ok &= (bit > 0);
   // get the cell itself.
   cell = trienode_leaf(node, amtnode_bit2cellindex(node, bitindex));
   // Now, IF everything is okay, we set *result to *cell and return 1;
   // otherwise we don't set it and we return 0.
   if (result && ok)
      *result = cell;
   return (ok > 0);
}

static inline int fat_lookup(Trie_t node, trieint_t key, void** result) {
   // As with the AMT dummy, the bits are always zero, so the leafsize is
   // irrelevant and is set to sizeof(void*) since that's a valid
   // compile-time constant. (There's no is_fat flag to set here -- see "The
   // trieint_t is_transient flag" comment above -- this dummy is only ever
   // touched by fat_lookup()'s own FAT-specific logic below, never by any
   // code that would need to ask what kind it is.)
   static struct TrieDummyData fatnode_dummytwig_data = {
      .header = {
         .depth = FAT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   static Trie_t fatnode_dummytwig = (Trie_t)&fatnode_dummytwig_data;
   triebits_t depth, cellindex, bit;
   bool ok;
   void* cell;
   void* discard;
   result = (result? result : &discard);
   // This function used to be a switch()-with-intentional-fallthrough that
   // stepped through a fixed sequence of case labels, one per depth,
   // assuming every real transition advances depth by exactly 1. That
   // assumption happens to hold for FAT now (see fat.h's file header
   // comment: FAT indexes densely sequential keys and maintains a
   // dense-tree invariant, so a branch node's child is always at exactly
   // parent-depth+1, never deeper -- unlike AMT, which deliberately skips
   // ahead whenever two subtrees agree on several digits, to keep its own
   // tree minimal), but the switch construct was still wrong even before
   // that invariant was settled on, and is left as a real loop rather than
   // reverted, both because it's exactly what amt_lookup() already does and
   // because it costs nothing to also be correct if FAT's per-depth
   // materialization were ever relaxed again in the future. The bug: once
   // the switch landed on the real child (however deep), it kept falling
   // through the remaining case labels regardless, each one re-testing the
   // *already-arrived* node's occupancy bits against an essentially
   // arbitrary cellindex derived from an irrelevant, already-passed depth
   // -- silently overwriting the correct `ok` with a bogus one (confirmed
   // via a minimal 2-key repro: a node landed correctly, with the right bit
   // set and the right prefix match, yet fat_lookup() still reported
   // not-found, because a later spurious case label's bit test happened to
   // come up false and swapped `node` for the dummy on the way to the final
   // twig step). A real loop that re-reads each node's *actual* depth every
   // iteration handles this correctly regardless of how deep any given
   // transition turns out to be.
   // Unlike amt_lookup(), there's no need for a branchless "mask the
   // speculative cellindex to stay in-bounds" trick here (see amt_lookup()'s
   // Bug 8 comment): every FAT node, at every depth, always has all
   // FAT_CELLS cells physically allocated regardless of occupancy, so
   // reading cell `cellindex` for any cellindex in [0, FAT_CELLS) is always
   // an in-bounds read, whether or not that cell happens to be occupied.
   depth = node->header.depth;
   while (depth < FAT_MAX_DEPTH) {
      ok = fatnode_prefix_match(node, key);
      cellindex = (triebits_t)fatdepth_bitindex(depth, key);
      bit = node->header.bits & (TRIEBITS_1 << cellindex);
      ok = ok && (bit > 0);
      cell = trienode_subt(node, cellindex);
      node = (ok? (Trie_t)cell : fatnode_dummytwig);
      depth = node->header.depth;
   }
   // This is a twig, so we return from this node.
   ok = fatnode_prefix_match(node, key);
   cellindex = (triebits_t)fatdepth_bitindex(FAT_MAX_DEPTH, key);
   bit = node->header.bits & (TRIEBITS_1 << cellindex);
   ok = ok && (bit > 0);
   cell = trienode_leaf(node, cellindex);
   if (result && ok)
      *result = cell;
   return (ok > 0);
}


//=============================================================================
// Iteration.
// Iteration requires a special data structure to store the iteration data in:
#if (AMT_LAYERS > FAT_LAYERS)
#  define TRIE_MAX_LAYERS AMT_LAYERS
#else
#  define TRIE_MAX_LAYERS FAT_LAYERS
#endif
struct TriePathData {
   // The number of steps currently in the path.
   // (1 byte; 1 byte total)
   uint8_t steps;
   // Set to 1 if the searched for node is beneath the current node.
   // (1 byte; 2 bytes total)
   bool is_beneath;
   // A cached pointer to the leaf value at the final step, filled in whenever
   // a find/iteration function successfully lands on a twig cell, so callers
   // don't need to re-derive it with triepath_val(). Only valid when the path
   // encodes a found element (mirrors triepath_val()'s own precondition).
   // (8 bytes; 10 bytes total)
   void* value;
   // The cellindex or bitindex of the current match in the associated node.
   // (14 bytes; 24 bytes total)
   uint8_t index[TRIE_MAX_LAYERS];
   // The stack of nodes followed along the path; node[0] is always the start
   // node, and node[steps-1] is always the final step.
   // (8 * 14 = 112 bytes; 136 bytes total)
   Trie_t node[TRIE_MAX_LAYERS];
   // A nice feature of this data structure is that most of the time, there
   // only be a few depths searched, so the final 5 or 6 node[] elements will
   // never be touched. The first 64 bytes of this struct contains everything
   // up to the first 5 node depths, so for most iterations, the entire
   // iteration memory needed can fit in 1 64-byte cache line.
};
EXTC typedef struct TriePathData TriePath, *TriePath_t;
// Get the key found by a TriePath.
// This only returns a valid value if the TriePath encodes a found element.
// Requires that path->index[last] hold the twig's real per-depth digit (the
// raw bit position within that twig's own window, i.e. what
// trienode_first_bitindex()/trienode_next_bitindex() report) rather than a
// compacted cellindex: trienode_prefix() only encodes the bits *above* the
// twig's own depth, so the twig's own digit has to be added back in raw, not
// as whatever physical slot happens to hold it. This holds uniformly for
// every path-building function below: triepath_amtfind()/triepath_fatfind()
// always stored the raw digit here, and amt_firstpath()/amt_nextpath() (see
// their comments) were fixed to do the same instead of storing a cellindex.
static inline trieint_t triepath_key(const TriePath_t p) {
   uint8_t ii = p->steps - 1;
   return trienode_prefix(p->node[ii]) + p->index[ii];
}
// Get the value found by a TriePath. Simply returns the cached `value`
// field rather than re-deriving it from node[last]/index[last]: `value` is
// always kept correct by every path-building function below the moment it
// lands on a twig cell (see the field's own doc comment), and re-deriving it
// here via trienode_leaf(node[last], index[last]) would need index[last] to
// be a cellindex -- which, per triepath_key()'s comment above, it deliberately
// is not (it's the raw digit instead, needed for correct key reconstruction).
// A single `return p->value` works uniformly for both AMT and FAT paths
// without needing to know which kind built this path.
static inline void* triepath_val(const TriePath_t p) {
   return p->value;
}
// Fill in a path to a node (or fail to find it).
static inline int triepath_amtfind(TriePath_t path, Trie_t a, trieint_t key) {
   triebits_t cellindex;
   triebits_t bitindex = amtdepth_bitindex(a->header.depth, key);
   bool ok = amtnode_prefix_match(a, key);
   path->node[0] = a;
   path->index[0] = bitindex;
   path->steps = 1;
   if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
      path->is_beneath = ok;
      return 0;
   }
   cellindex = amtnode_bit2cellindex(a, bitindex);
   while (a->header.depth < AMT_MAX_DEPTH) {
      // descend a node...
      a = trienode_subt(a, cellindex);
      bitindex = amtdepth_bitindex(a->header.depth, key);
      path->node[path->steps] = a;
      path->index[path->steps] = bitindex;
      path->steps++;
      cellindex = amtnode_bit2cellindex(a, bitindex);
      ok = amtnode_prefix_match(a, key);
      if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
         // If we didn't match prefixes, it's a 0-step match; if we didn't match
         // on the on the bit, it's still a step-1 match.
         path->is_beneath = ok;
         return 0;
      } 
      // Otherwise, we're continuing to descend.
   }
   // We've reached a twig node with the appropriate bit set.
   path->is_beneath = 1;
   path->value = trienode_leaf(a, cellindex);
   return 1;
}
// amt_firstpath()/amt_nextpath() walk cells in *bit* order (via
// trienode_first_bitindex()/trienode_next_bitindex(), the same primitives
// fat_firstpath()/fat_nextpath() use below), converting a bitindex to the
// physical cellindex trienode_subt()/trienode_leaf() need via
// amtnode_bit2cellindex() only at the point of actually reading a cell.
// This is deliberately NOT the cheaper-looking alternative of walking the
// compacted cellindex range [0, occupancy) directly with a plain ++index:
// that would visit cells in the right order (the compacted array is stored
// in ascending-bitindex order) but path->index[] would then hold a
// cellindex rather than the real per-depth digit, and triepath_key() (see
// its own comment) needs the real digit to reconstruct the key -- a
// cellindex is only numerically equal to the true digit when every lower
// bit happens to also be occupied, which is not the general case. Storing
// the real digit here costs one extra amtnode_bit2cellindex() call (a
// popcount) per cell visited versus the plain-increment version; on any
// modern CPU that's on the order of one cycle, negligible next to the
// pointer chase/cache-line fetch each step already does, and it's exactly
// what fat_firstpath()/fat_nextpath() already pay (fatnode_bit2cellindex()
// is just the identity function there, but the shape of the code, and the
// cost model, match).
static inline int amt_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t top = 0;
   triebits_t bi;
   // `a` itself may already be a twig: per AMT's minimal-tree invariant, a
   // tree holding 0 or exactly 1 item is represented as a single twig node
   // with no branch nodes above it at all (see amt_anditem()'s "if a is
   // empty" case and amt_1leaf()). The do/while loop below assumes its
   // *current* node is a branch (so that trienode_subt() reads a real
   // child pointer out of it) and only checks amtnode_is_twig() on the
   // node it just descended *into* -- so a twig `a` must be handled here,
   // up front, rather than being fed into that loop: descending "into" a
   // twig's cell would reinterpret a leaf's raw value bytes (or, for the
   // canonical empty node, memory past the end of its zero-cell allocation)
   // as a child pointer.
   if (amtnode_is_twig(a)) {
      bi = trienode_first_bitindex(a);
      if (bi >= TRIEBITS_WIDTH)
         return 0;  // the canonical empty tree: no first element.
      path->index[0] = bi;
      path->node[0] = a;
      path->steps = 1;
      path->value = trienode_leaf(a, amtnode_bit2cellindex(a, bi));
      return 1;
   }
   do {
      // We descend into the first occupied cell.
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, amtnode_bit2cellindex(node, bi));
      ++top;
   } while (!amtnode_is_twig(node));
   // We've reached a twig node.
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, amtnode_bit2cellindex(node, bi));
   return 1;
}
static inline int amt_nextpath(TriePath_t path) {
   triebits_t top = path->steps - 1;
   Trie_t node = path->node[top];
   triebits_t bi = trienode_next_bitindex(node, path->index[top]);
   // The path always leaves off at a twig.
   if (bi < TRIEBITS_WIDTH) {
      // We can just return this next leaf!
      path->index[top] = bi;
      path->value = trienode_leaf(node, amtnode_bit2cellindex(node, bi));
      return 1;
   } else if (top == 0) {
      // Otherwise, if we're at the top, we're already done.
      return 0;
   }
   // If we're not at the top, we're going to ascend back up.
   --top;
   while (1) {
      node = path->node[top];
      bi = trienode_next_bitindex(node, path->index[top]);
      if (bi < TRIEBITS_WIDTH) {
         // This is where we descend.
         path->index[top] = bi;
         break;
      } else if (top == 0) {
         return 0;
      } else {
         // We pop the stack! If top is 0 we're already at the top
         // and can just return a false result.
         --top;
      }
   }
   // At this point we've found a node with something to descend into.
   node = trienode_subt(node, amtnode_bit2cellindex(node, path->index[top]));
   while (!amtnode_is_twig(node)) {
      // We descend into the first occupied cell.
      ++top;
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, amtnode_bit2cellindex(node, bi));
   }
   // We've reached a twig node.
   ++top;
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, amtnode_bit2cellindex(node, bi));
   return 1;
}
// To iterate over AMT nodes, the correct method is as so:
//    struct TriePath iter;
//    for (int ok = amt_firstpath(amt, &iter); ok; ok = amt_nextpath(&iter)) {
//        process_key_value(triepath_key(&iter), triepath_val(&iter));
//    }
// Fill in a path to a node in a FAT (or fail to find it). Mirrors
// triepath_amtfind() above; the one structural simplification is that a
// FAT's cellindex is always identical to its bitindex (fatnode_bit2cellindex
// is the identity function -- FAT nodes are never compacted the way AMT
// nodes are), so there's no separate cellindex variable to track through
// the descent the way triepath_amtfind() needs one.
static inline int triepath_fatfind(TriePath_t path, Trie_t a, trieint_t key) {
   bool ok = fatnode_prefix_match(a, key);
   triebits_t bitindex = (triebits_t)fatdepth_bitindex(a->header.depth, key);
   path->node[0] = a;
   path->index[0] = bitindex;
   path->steps = 1;
   if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
      path->is_beneath = ok;
      return 0;
   }
   while (a->header.depth < FAT_MAX_DEPTH) {
      // descend a node...
      a = trienode_subt(a, bitindex);
      bitindex = (triebits_t)fatdepth_bitindex(a->header.depth, key);
      path->node[path->steps] = a;
      path->index[path->steps] = bitindex;
      path->steps++;
      ok = fatnode_prefix_match(a, key);
      if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
         // If we didn't match prefixes, it's a 0-step match; if we didn't
         // match on the bit, it's still a step-1 match.
         path->is_beneath = ok;
         return 0;
      }
      // Otherwise, we're continuing to descend.
   }
   // We've reached a twig node with the appropriate bit set.
   path->is_beneath = 1;
   path->value = trienode_leaf(a, bitindex);
   return 1;
}
static inline int fat_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t top = 0;
   triebits_t bi;
   // `a` itself may already be a twig: a lone key/value pair needs nothing
   // explicitly represented above it (see fat_anditem()'s "if a is empty"
   // case and fat_1leaf()), and the canonical fat_empty() node is a twig
   // too. The do/while loop below assumes its *current* node is a branch
   // (so that trienode_subt(node, bi) reads a real child pointer out of
   // it) and only checks fatnode_is_twig() on the node it just descended
   // *into* -- so a twig `a` must be handled here, up front, rather than
   // being fed into that loop: descending "into" a twig's occupied cell
   // would reinterpret a leaf's raw value bytes as a child pointer.
   if (fatnode_is_twig(a)) {
      bi = trienode_first_bitindex(a);
      if (bi >= FAT_CELLS)
         return 0;  // the canonical empty tree: no first element.
      path->index[0] = bi;
      path->node[0] = a;
      path->steps = 1;
      path->value = trienode_leaf(a, bi);
      return 1;
   }
   do {
      // We descend into the first occupied cell.
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, bi);
      ++top;
   } while (!fatnode_is_twig(node));
   // We've reached a twig node.
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, bi);
   return 1;
}
static inline int fat_nextpath(TriePath_t path) {
   triebits_t top = path->steps - 1;
   Trie_t node = path->node[top];
   triebits_t bi = trienode_next_bitindex(node, path->index[top]);
   // The path always leaves off at a twig.
   if (bi < FAT_CELLS) {
      // We can just return this next leaf!
      path->index[top] = bi;
      path->value = trienode_leaf(node, bi);
      return 1;
   } else if (top == 0) {
      // Otherwise, if we're at the top, we're already done.
      return 0;
   }
   // If we're not at the top, we're going to ascend back up.
   --top;
   while (1) {
      node = path->node[top];
      bi = trienode_next_bitindex(node, path->index[top]);
      if (bi < FAT_CELLS) {
         // This is where we descend.
         path->index[top] = bi;
         break;
      } else if (top == 0) {
         return 0;
      } else {
         // We pop the stack! If top is 0 we're already at the top
         // and can just return a false result.
         --top;
      }
   }
   // At this point we've found a node with something to descend into.
   node = trienode_subt(node, path->index[top]);
   while (!fatnode_is_twig(node)) {
      // We descend into the first occupied cell.
      ++top;
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, bi);
   }
   // We've reached a twig node.
   ++top;
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, bi);
   return 1;
}
// To iterate over FAT nodes, the correct method is as so:
//    struct TriePath iter;
//    for (int ok = fat_firstpath(fat, &iter); ok; ok = fat_nextpath(&iter)) {
//        process_key_value(triepath_key(&iter), triepath_val(&iter));
//    }

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_TRIE_H
