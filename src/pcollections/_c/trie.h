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
#include "uintbits.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
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
#if (defined(UINT16_WIDTH) && (TRIEBITS_WIDTH == UINT16_WIDTH))
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
#if (defined(UINT32_WIDTH) && (TRIEBITS_WIDTH == UINT32_WIDTH))
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
#if (defined(UINT64_WIDTH) && (TRIEBITS_WIDTH == UINT64_WIDTH))
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
#if (defined(UINT128_WIDTH) && (TRIEBITS_WIDTH == UINT128_WIDTH))
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
#if (FAT_CELLS == 29)
   const trieint_t[] _fat_divs = {
#    if (TRIEINT_WIDTH == 128)
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29 * 29,

        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29*29,
#    endif
#    if (TRIEINT_WIDTH >= 64)
        29*29*29*29*29 * 29*29*29*29*29 * 29*29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29*29,
        29*29*29*29*29 * 29*29*29*29*29 * 29,

        29*29*29*29*29 * 29*29*29*29*29,
        29*29*29*29*29 * 29*29*29*29,
        29*29*29*29*29 * 29*29*29,
        29*29*29*29*29 * 29*29,
#    endif 
#    if (TRIEINT_WIDTH >= 32)
        29*29*29*29*29 * 29,

        29*29*29*29*29,
        29*29*29*29,
#    endif 
#    if (TRIEINT_WIDTH >= 16)
        29*29*29,
        29*29,
#    endif
        29,
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
                                         trient_t prefix, trieint_t k) {
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
   _Atomic uint64_t refcount;
   // The hash prefix of the node. This prefix is always stored in unshifted
   // bits, meaning that for a hash key k, you can tell if k falls beneath the
   // node by comparing (k >> shift) == (prefix >> shift).
   // Because we are using 64-bit hashes, there are 4 bits at the low end of
   // the prefix that are never used in a trie node (not counting leaves, which
   // aren't stored in trie nodes). We use these to store two flags:
   //  - is_transient bit: is this node transient (1) or persistent (0)?
   //  - is_fat bit: is this node a FAT (1) or an AMT (0)?
   // As long as we are using 32-bit, 64-bit, or 128-bit hashes, there are
   // enough bits for up to at least 2 bits to be stored; if the hash size is
   // 256 bits or 16 bits, then there's still 1 bit for is_transient.
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
   // The following two items are used by AMTs but not FATs.
   uint8_t shift;  // The node's bit-shift for an AMT node.
   uint8_t ncells;  // How many cells allocated in this node?
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
EXTC struct TrieDummyData {
   struct TrieData trie;
   void* cells[TRIEBITS_WIDTH];
};


//-----------------------------------------------------------------------------
// Trie Methods
// Methods are prefixed with various strings:
// - Any method trie_* such as trie_is_amt() is intended as a public interface
//   method for any trie object such as the user can obtain using these public
//   functions; these trie objects differ from those that might be found
//   internally to a trie by, for example, crawling the trie's subnodes.
// - Any method trienode_* such as trienode_is_transient() is intended as a
//   private method for use within the module, and is intended to work on any
//   trie node.
// - For AMT and FAT nodes, a similar paradigm applies with the prefixes
//   amt_ / amtnode_ and fat_ / fatnode_.
// - When there are duplicate implementations of a function (e.g., some
//   function trienode_f() has been written but so have amtnode_f() and
//   fatnode_f()), then typically the trienode_f() function will do nothing but
//   check whether the node is an amtnode or fatnode and run it through the
//   appropriate submethod.

// The trieint_t prefix flags.
#define TRIE_BIT_ISTRANSIENT  0
#define TRIE_BIT_ISFAT        1
#define TRIE_FLAG(bit)        (TRIEINT_1 << (bit))
#define TRIE_PREFIX_MASK      (~TRIEINT_C(3))

static inline trieint_t trienode_prefix(const Trie_t t) {
   return TRIE_PREFIX_MASK & t->prefix;
}
static inline bool trie_is_persistent(const Trie_t t) {
   return TRIE_FLAG(TRIE_BIT_ISTRANSIENT) & ~t->prefix;
}
static inline bool trie_is_transient(const Trie_t t) {
   return TRIE_FLAG(TRIE_BIT_ISTRANSIENT) & t->prefix;
}
static inline void trienode_set_transient(Trie_t th, bool is_tr) {
   th->header.prefix &= ~TRIE_FLAG(TRIE_BIT_ISTRANSIENT);
   th->header.prefix |= (trieint_t)is_tr << TRIE_BIT_ISTRANSIENT;
}
static inline bool trie_is_amt(const Trie_t t) {
   return TRIE_FLAG_ISFAT & ~t->prefix;
}
static inline bool trie_is_fat(const Trie_t t) {
   return TRIE_FLAG_ISFAT & t->prefix;
}
static inline void trienode_set_fat(Trie_t th, bool is_fat) {
   th->header.prefix &= ~TRIE_FLAG(TRIE_BIT_ISFAT);
   th->header.prefix |= (trieint_t)is_tr << TRIE_BIT_ISFAT;
}
static inline bool amtnode_is_twig(const Trie_t th) {
   return th->header.depth == AMT_MAX_DEPTH;
}
static inline bool fatnode_is_twig(const Trie_t th) {
   return th->header.depth == FAT_MAX_DEPTH;
}
static inline bool trienode_is_twig(const Trie_t th) {
   return th->header.depth == (trie_is_fat(th)? FAT_MAX_DEPTH : AMT_MAX_DEPTH);
}
static inline bool amtnode_prefix_match(const Trie_t th, trieint_t k) {
   return amtdepth_prefix_match(th->header.depth, th->header.prefix, k);
}
static inline bool fatnode_prefix_match(const Trie_t th, trieint_t k) {
   return fatdepth_prefix_match(th->header.depth, th->header.prefix, k);
}
static inline trieint_t amtnode_minleaf(const Trie_t th) {
   return (th->header.prefix & (TRIEINT_MAX << th->header.shift));
}
static inline trieint_t amtnode_maxleaf(const Trie_t th) {
   return (th->header.prefix | ~(TRIEINT_MAX << th->header.shift));
}
static inline trieint_t fatnode_minleaf(const Trie_t th) {
   return (th->header.prefix / triedepth_div(th->header.depth));
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
   return (void*)(th->cells + cellindex*th->leafsize);
}
static inline void trienode_set_subt(Trie_t th,
                                     trieint_t cellindex,
                                     Trie_t subt) {
   ((Trie_t*)th->cells)[cellindex] = subt;
}
static inline void trienode_set_leaf(Trie_t th,
                                     trieint_t cellindex,
                                     void* data) {
   memcpy(th->cells + cellindex*th->leafsize, data, th->leafsize);
}
static inline size_t amtnode_cellsize(const Trie_t node) {
   return amtnode_is_twig(node)? node->leafsize : sizeof(void*);
}
static inline size_t fatnode_cellsize(const Trie_t node) {
   return fatnode_is_twig(node)? node->leafsize : sizeof(void*);
}
// Bitindex and cellindex scanning functions for iterating over tries.
static inline triebits_t trienode_first_bitindex(Trie_t th) {
   return ctz_triebits(th->header.bits);
}
static inline triebits_t trienode_next_bitindex(Trie_t th, triebits_t prev) {
   return ctz_triebits(th->header.bits >> prev) + prev;
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
   atomic_fetch_add(&th->header.refcount, 1);
}
// Note that after calling amtnode_decref(t) it is quite possible that t is no
// longer a valid pointer!
static inline void amttwig_free(Trie_t t, void (*leaf_decref)(void*)) {
   triebits_t ci, nocc;
   // We need to deallocate this node, which means we need to first dereference
   // each of the leaves.
   nocc = trienode_occupancy(t);
   for (ci = 0; ci < nocc; ++ci)
      (*leaf_deref)(trienode_leaf(t, ci));
   // Once that's done, we free the node.
   free(t);
}
static inline void amttwig_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
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
      (*leaf_deref)(trienode_leaf(t, fatnode_bit2cellindex(bi)));
   // Once that's done, we free the node.
   free(t);
}
static inline void fattwig_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
   // If we aren't the last one holding the reference, we don't do anything.
   if (r <= 1)
      fattwig_free(t, leaf_deref);
}
static inline void trietwig_decref_noprop(Trie_t t) {
   uint64_t r;
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
   // If we aren't the last one holding the reference, we don't do anything.
   if (r > 1) return;
   // We need to deallocate this node, but since there's no leaf cleanup,
   // we just free the memory.
   free(t);
}
static inline void amtnode_free(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   Trie_t u, node;
   void* leaf;
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
   if (trienode_is_twig(t)) {
      if (leaf_deref)
         amttwig_free(t, leaf_deref);
      else
         free(t);
      return;
   }
   node = a;
   ci = 0;
   nocc = trienode_occupancy(t);
   top = 0;
   // To start with, we write slightly different code depending on whether we
   // have a leaf dereference function or not.
   while (1) {
      if (ci < nocc) {
         // We need to process subtree ci of node.
         u = amtnode_subt(node, ci);
         r = atomic_fetch_sub_explicit(
            &u->header.refcount, 1, memory_order_acq_rel);
         if (r <= 1) {
            if (amtnode_is_twig(u)) {
               if (leaf_decref)
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
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
   if (r <= 1)
      amtnode_free(t, leaf_deref);
}
static inline void fatnode_free(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   Trie_t u, node;
   void* leaf;
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
   if (trienode_is_twig(t)) {
      if (leaf_deref)
         fattwig_free(t, leaf_deref);
      else
         free(t);
      return;
   }
   node = a;
   bi = trienode_first_bitindex(a);
   top = 0;
   // To start with, we write slightly different code depending on whether we
   // have a leaf dereference function or not.
   while (1) {
      if (bi < FAT_CELLS) {
         // We need to process subtree ci of node.
         u = fatnode_subt(node, bi);
         r = atomic_fetch_sub_explicit(
            &u->header.refcount, 1, memory_order_acq_rel);
         if (r <= 1) {
            // We need to deallocate this node too!
            if (fatnode_is_twig(u)) {
               if (leaf_decref)
                  fattwig_free(u, leaf_deref);
               else
                  free(u);
            } else {
               nodes[top] = node;
               bis[top] = trienode_next_bitindex(node, bi);
               ++top;
               node = u;
               bi = trienode_first_bitinidex(node);
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
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
   if (r <= 1)
      fatnode_free(t, leaf_deref);
}
static inline void trienode_free(Trie_t t, void (*leaf_deref)(void*)) {
   if (trienode_is_fat(t))
      fatnode_free(t, leaf_deref);
   else
      amtnode_free(t, leaf_deref);
}
static inline void trienode_decref(Trie_t t, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = atomic_fetch_sub_explicit(&t->header.refcount, 1, memory_order_acq_rel);
   if (r <= 1)
      trienode_free(t, leaf_deref);
}

// Lookup ---------------------------------------------------------------------

static inline int amt_lookup(Trie_t node,
                             trieint_t key,
                             void** result) {
   static struct TrieDummyData amtnode_dummytwig_data = {
      .trie = {
         .depth = AMT_MAX_DEPTH,
         .leafsize = node->leafsize
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
      bit = node->header.bits & (AMTBITS_1 << bitindex);
      // Things are only okay if bit is truthy.
      ok = (bit > 0);
      // Now get the pointer to the cell itself.
      cellindex = amtnode_bit2cellindex(node, bitindex);
      cell = amtnode_subt(node, cellindex);
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
   bit = node->header.bits & (AMTBITS_1 << bitindex);
   // Things are only okay if bit is truthy.
   ok &= (bit > 0);
   // get the cell itself.
   cell = amtnode_leaf(node, amtnode_bit2cellindex(node, bitindex));
   // Now, IF everything is okay, we set *result to *cell and return 1;
   // otherwise we don't set it and we return 0.
   if (result && ok)
      *result = cell;
   return (ok > 0);
}

static inline int fat_lookup(Trie_t node, trieint_t key, void** result) {
   static struct TrieDummyData fatnode_dummytwig_data = {
      .trie = {
         .depth = FAT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   triebits_t depth;
   trieint_t shift, cellindex, addrmask;
   uintptr_t ok, tmp;
   void* cell;
   void* dummy = (void*)&fatnode_dummytwig_data;
   result = (result? &dummy : result);
   // In an FAT, we branch once to the appropriate depth then fall down to
   // the max depth.
#  define FATNODE_LOOKUP_STEP(depth)                                          \
      do {                                                                    \
         cellidx = fatdepth_bitindex((depth), k);                             \
         ok = (node->header.bits & (TRIEBITS_1 << cellidx)? UINTPTR_MAX : 0); \
         cell = (node->depth == depth                                         \
            ? ((void**)node->cells)[cellidx]                                  \
            : node);                                                          \
          node = (Trie_t)(                                                    \
             + ((+ok) & ((uintptr_t)cell))                                    \
             | ((~ok) & ((uintptr_t)dummy)));                                 \
       } while (0)
   // Note that in the above, we don't check the address; this is because we
   // only need to check that the twig address matches; if we end up in a weird
   // twig node because we didn't check the addresses on the way down, that's
   // fine because that twig node's address won't match, and we check the
   // address in the twig step code below.
#  define FATTWIG_LOOKUP_STEP()                                               \
      do {                                                                    \
         cellidx = fatdepth_bitindex(FAT_MAX_DEPTH, key);                     \
         ok = (node->header.bits & (TRIEBITS_1 << cellidx)? UINTPTR_MAX : 0); \
         cell = (void*)(node->cells + node->leafsize*cellidx);                \
         cell = (void*)(ok & ((uintptr_t)cell));                              \
      } while (0)
   // Notice that we don't check prefixes above, because we can just check the
   // prefix once now; then if matches are found below prefixes must match.
   ok = fatdepth_prefix_match(node->depth, node->header.prefix, key);
   node = (ok? node : (Trie_t)dummy);
   depth = node->depth;
   // In this switch statement, we deliberately do not use breaks because we   
   // want to jump to the initial depth then trickle down the lower depths.
   switch (depth) {
   case 0: FATNODE_LOOKUP_STEP(0);
#  if (TRIEINT_MAX_DEPTH == 1)
      case 1: FATTWIG_LOOKUP_STEP(1);
#  else
      case 1: FATNODE_LOOKUP_STEP(1);
      case 2: FATNODE_LOOKUP_STEP(2);
#     if (TRIEINT_MAX_DEPTH == 3)
         case 3: FATTWIG_LOOKUP_STEP(3);
#     else
         case 3: FATNODE_LOOKUP_STEP(3);
         case 4: FATNODE_LOOKUP_STEP(4);
         case 5: FATNODE_LOOKUP_STEP(5);
#        if (TRIEINT_MAX_DEPTH == 6)
            case 6: FATTWIG_LOOKUP_STEP(6);
#        else
         case 6:  FATNODE_LOOKUP_STEP(6);
         case 7:  FATNODE_LOOKUP_STEP(7);
         case 8:  FATNODE_LOOKUP_STEP(8);
         case 9:  FATNODE_LOOKUP_STEP(9);
         case 10: FATNODE_LOOKUP_STEP(10);
         case 11: FATNODE_LOOKUP_STEP(11);
         case 12: FATNODE_LOOKUP_STEP(12);
#        if (TRIEINT_MAX_DEPTH == 13)
            case 13: FATTWIG_LOOKUP_STEP(13);
#        else
            case 13: FATNODE_LOOKUP_STEP(13);
            case 14: FATNODE_LOOKUP_STEP(14);
            case 15: FATNODE_LOOKUP_STEP(15);
            case 16: FATNODE_LOOKUP_STEP(16);
            case 17: FATNODE_LOOKUP_STEP(17);
            case 18: FATNODE_LOOKUP_STEP(18);
            case 19: FATNODE_LOOKUP_STEP(19);
            case 20: FATNODE_LOOKUP_STEP(20);
            case 21: FATNODE_LOOKUP_STEP(21);
            case 22: FATNODE_LOOKUP_STEP(22);
            case 23: FATNODE_LOOKUP_STEP(23);
            case 24: FATNODE_LOOKUP_STEP(24);
            case 25: FATNODE_LOOKUP_STEP(25);
            case 26: FATTWIG_LOOKUP_STEP(26);
#        endif
#     endif
#  endif
   }
#  undef FATTWIG_LOOKUP_STEP
#  undef FATNODE_LOOKUP_STEP
   // At this point, we've stepped through each depth; if ok is nonzero then
   // node is the ready to be returned. Otherwise, we return a failure code.
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
   // The cellindex or bitindex of the current match in the associated node.
   // (14 bytes; 16 bytes total)
   uint8_t index[TRIE_MAX_LAYERS];
   // The stack of nodes followed along the path; node[0] is always the start
   // node, and node[steps-1] is always the final step.
   // (8 * 14 = 112 bytes; 128 bytes total)
   Trie_t node[TRIE_MAX_LAYERS];
   // A nice feature of this data structure is that most of the time, there
   // only be a few depths searched, so the final 5 or 6 node[] elements will
   // never be touched. The first 64 bytes of this struct contains everything
   // up to the first 6 node depths, so for most iterations, the entire
   // iteration memory needed can fit in 1 64-byte cache line.
};
EXTC typedef struct TriePathData TriePath, *TriePath_t;
// Get the key found by a TriePath.
// This only returns a valid value if the TriePath encodes a found element.
static inline trieint_t triepath_key(const TriePath_t p) {
   uint8_t ii = p->steps - 1;
   return trienode_prefix(p->node[ii]) + p->index[ii];
}
static inline void* triepath_val(const TriePath_t p) {
   uint8_t ii = p->steps - 1;
   return trienode_leaf(p->node[ii], p->index[ii]);
}
// Fill in a path to a node (or fail to find it).
static inline int triepath_amtfind(TriePath_t path, Trie_t a, trieint_t key) {
   triebits_t cellindex;
   triebits_t bitindex = (key >> a->shift) & amtdepth_mask(a->depth);
   bool ok = amtnode_prefix_match(a, key);
   path->node[0] = a;
   path->index[0] = bitindex;
   path->steps = 1;
   if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
      path->is_beneath = ok;
      return 0;
   }
   cellindex = amtnode_bit2cellindex(a, bitindex);
   while (a->depth < AMT_MAX_DEPTH) {
      // descend a node...
      a = amtnode_subt(a, cellindex);
      bitindex = (key >> a->shift) & AMT_NODE_MASK;
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
   return 1;
}
static inline int amt_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t ci = 0;
   triebits_t nocc = trienode_occupancy(t);
   triebits_t top = 0;
   do {
      // We descend into the first subtree.
      path->index[top] = 0;
      path->node[top] = node;
      node = amtnode_subt(node, 0);
      ++top;
   } while (!amtnode_is_twig(node));
   // We've reached a twig node.
   path->index[top] = 0;
   path->node[top] = node;
   path->steps = top + 1;
   return 1;
}
static inline int amt_nextpath(TriePath_t path) {
   triebits_t top = path->steps - 1;
   Trie_t node = path->node[top];
   triebits_t nocc = trienode_occupancy(node);
   triebits_t ci;
   // The path always leaves off at a twig.
   if (++path->index[top] < nocc) {
      // We can just return this next leaf!
      return 1;
   } else if (top == 0) {
      // Otherwise, if we're at the top, we're already done.
      return 0;
   }
   // If we're not at the top, we're going to ascend back up.
   --top;
   while (1) {
      node = path->node[top];
      nocc = trienode_occupancy(node);
      if (++path->index[top] < nocc) {
         // This is where we descend.
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
   node = amtnode_subt(node, path->index[top]);
   while (!amtnode_is_twig(node)) {
      // We descend into the first subtree.
      ++top;
      path->index[top] = 0;
      path->node[top] = node;
      node = amtnode_subt(node, 0);
   }
   // We've reached a twig node.
   ++top;
   path->index[top] = 0;
   path->node[top] = node;
   path->steps = top + 1;
   return 1;
}
// To iterate over AMT nodes, the correct method is as so:
//    struct TriePath iter;
//    for (int ok = amt_firstpath(amt, &iter); ok; ok = amt_nextpath(&iter)) {
//        process_key_value(triepath_key(&iter), triepath_val(&iter));
//    }
static inline int triepath_fatfind(TriePath_t path, Trie_t a, trieint_t key) {
   bool ok = amtnode_prefix_match(a, key);
   triebits_t bitindex = fatdepth_bitindex(a->depth, key);
   path->node[0] = a;
   path->index[0] = bitindex;
   path->steps = 1;
   if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
      path->is_beneath = ok;
      return 0;
   }
   // In an FAT, we branch once to the appropriate depth then fall down to
   // the max depth.
#  define FATFIND_STEP(depth)                                                 \
      do {                                                                    \
         cellidx = fatdepth_bitindex((depth), key);                           \
         ok = (node->header.bits & (TRIEBITS_1 << cellidx)? UINTPTR_MAX : 0); \
         cell = (node->depth == (depth)                                       \
            ? ((void**)node->cells)[cellidx]                                  \
            : node);                                                          \
          node = (Trie_t)(                                                    \
             + ((+ok) & ((uintptr_t)cell))                                    \
             | ((~ok) & ((uintptr_t)dummy)));                                 \
       } while (0)
   // Note that in the above, we don't check the address; this is because we
   // only need to check that the twig address matches; if we end up in a weird
   // twig node because we didn't check the addresses on the way down, that's
   // fine because that twig node's address won't match, and we check the
   // address in the twig step code below.
#  define FATTWIG_LOOKUP_STEP()                                               \
      do {                                                                    \
         cellidx = fatdepth_bitindex(FAT_MAX_DEPTH, key);                     \
         ok = (node->header.bits & (TRIEBITS_1 << cellidx)? UINTPTR_MAX : 0); \
         cell = (void*)(node->cells + node->leafsize*cellidx);                \
         cell = (void*)(ok & ((uintptr_t)cell));                              \
      } while (0)
   // Notice that we don't check prefixes above, because we can just check the
   // prefix once now; then if matches are found below prefixes must match.
   ok = fatdepth_prefix_match(node->depth, node->header.prefix, key);
   node = (ok? node : (Trie_t)dummy);
   depth = node->depth;
   // In this switch statement, we deliberately do not use breaks because we   
   // want to jump to the initial depth then trickle down the lower depths.
   switch (depth) {
   case 0: FATNODE_LOOKUP_STEP(0);
#  if (TRIEINT_MAX_DEPTH == 1)
      case 1: FATTWIG_LOOKUP_STEP(1);
#  else
      case 1: FATNODE_LOOKUP_STEP(1);
      case 2: FATNODE_LOOKUP_STEP(2);
#     if (TRIEINT_MAX_DEPTH == 3)
         case 3: FATTWIG_LOOKUP_STEP(3);
#     else
         case 3: FATNODE_LOOKUP_STEP(3);
         case 4: FATNODE_LOOKUP_STEP(4);
         case 5: FATNODE_LOOKUP_STEP(5);
#        if (TRIEINT_MAX_DEPTH == 6)
            case 6: FATTWIG_LOOKUP_STEP(6);
#        else
         case 6:  FATNODE_LOOKUP_STEP(6);
         case 7:  FATNODE_LOOKUP_STEP(7);
         case 8:  FATNODE_LOOKUP_STEP(8);
         case 9:  FATNODE_LOOKUP_STEP(9);
         case 10: FATNODE_LOOKUP_STEP(10);
         case 11: FATNODE_LOOKUP_STEP(11);
         case 12: FATNODE_LOOKUP_STEP(12);
#        if (TRIEINT_MAX_DEPTH == 13)
            case 13: FATTWIG_LOOKUP_STEP(13);
#        else
            case 13: FATNODE_LOOKUP_STEP(13);
            case 14: FATNODE_LOOKUP_STEP(14);
            case 15: FATNODE_LOOKUP_STEP(15);
            case 16: FATNODE_LOOKUP_STEP(16);
            case 17: FATNODE_LOOKUP_STEP(17);
            case 18: FATNODE_LOOKUP_STEP(18);
            case 19: FATNODE_LOOKUP_STEP(19);
            case 20: FATNODE_LOOKUP_STEP(20);
            case 21: FATNODE_LOOKUP_STEP(21);
            case 22: FATNODE_LOOKUP_STEP(22);
            case 23: FATNODE_LOOKUP_STEP(23);
            case 24: FATNODE_LOOKUP_STEP(24);
            case 25: FATNODE_LOOKUP_STEP(25);
            case 26: FATTWIG_LOOKUP_STEP(26);
#        endif
#     endif
#  endif
   }
#  undef FATTWIG_LOOKUP_STEP
#  undef FATNODE_LOOKUP_STEP
         
   switch (a->depth) {
   }
   while (a->depth < FAT_MAX_DEPTH) {
      // descend a node...
      a = amtnode_subt(a, cellindex);
      bitindex = (key >> a->shift) & AMT_NODE_MASK;
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
   return 1;
}
static inline int amt_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t ci = 0;
   triebits_t nocc = trienode_occupancy(t);
   triebits_t top = 0;
   do {
      // We descend into the first subtree.
      path->index[top] = 0;
      path->node[top] = node;
      node = amtnode_subt(node, 0);
      ++top;
   } while (!amtnode_is_twig(node));
   // We've reached a twig node.
   path->index[top] = 0;
   path->node[top] = node;
   path->steps = top + 1;
   return 1;
}
static inline int amt_nextpath(TriePath_t path) {
   triebits_t top = path->steps - 1;
   Trie_t node = path->node[top];
   triebits_t nocc = trienode_occupancy(node);
   triebits_t ci;
   // The path always leaves off at a twig.
   if (++path->index[top] < nocc) {
      // We can just return this next leaf!
      return 1;
   } else if (top == 0) {
      // Otherwise, if we're at the top, we're already done.
      return 0;
   }
   // If we're not at the top, we're going to ascend back up.
   --top;
   while (1) {
      node = path->node[top];
      nocc = trienode_occupancy(node);
      if (++path->index[top] < nocc) {
         // This is where we descend.
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
   node = amtnode_subt(node, path->index[top]);
   while (!amtnode_is_twig(node)) {
      // We descend into the first subtree.
      ++top;
      path->index[top] = 0;
      path->node[top] = node;
      node = amtnode_subt(node, 0);
   }
   // We've reached a twig node.
   ++top;
   path->index[top] = 0;
   path->node[top] = node;
   path->steps = top + 1;
   return 1;
}
