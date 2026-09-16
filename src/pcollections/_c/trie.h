///////////////////////////////////////////////////////////////////////////////
// _c/trie.h
// The node layout and shared low-level operations for the two trie kinds used
// by the persistent collections:
//
//  - AMT (array mapped trie): up to 32 cells per node, compacted so that a
//    node allocates only as many cells as it has occupied bits (a cell's
//    index is the popcount of the occupied bits below it). Used for hash
//    tables: `idx` in pdict/pset, whose leaves are plain integers.
//  - FAT (fixed arity trie): exactly FAT_CELLS cells per node, and a cell's
//    index equals its bit index. Used for dense, sequential keys: `els` in
//    pdict/pset and the element store of plist. Its leaves hold Python
//    object references.
//
// Both kinds share one memory layout (struct TrieData), so the generic
// trienode_* helpers, lookup, and path iteration work on either. The kinds
// differ in how nodes are allocated and reference-counted:
//
//  - FAT nodes are Python objects (instances of the per-interpreter node
//    types created in core.h). Their reference counts are ordinary Python
//    reference counts, and they take part in cyclic garbage collection: a
//    node reports its own children or leaves exactly once, however many
//    collections share it. Nodes that cannot be part of a cycle (all of
//    whose contents are atomic, like ints and strings) are not tracked by
//    the collector at all (see fat.h).
//  - AMT nodes are plain C memory with an atomic reference count. They hold
//    no Python objects, so the collector never needs to see them, and they
//    are safely shared between interpreters.
//
// Every node starts with the same header region: for a FAT node it is the
// PyObject header; for an AMT node the first word is the C reference count
// and the rest is zero, so the "type" slot is NULL. trienode_is_fat() tests
// that slot.

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

// Portable 64-bit atomic reference count for AMT nodes. MSVC's C mode has no
// <stdatomic.h> (without /std:c11), so Windows uses the Interlocked
// intrinsics; like atomic_fetch_add/sub, they return the previous value.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
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
// Key and occupancy integer types.

// trieint_t: the unsigned key type. Python hashes are signed (Py_hash_t), but
// the tries work with the same-width unsigned type.
EXTC typedef size_t trieint_t;
#define TRIEINT_C(x)       SIZE_C(x)
#define TRIEINT_WIDTH      SIZE_WIDTH
#define TRIEINT_MAX        SIZE_MAX
#define TRIEINT_1          SIZE_C(1)
#define TRIEINT_0          SIZE_C(0)
#define popcount_trieint   popcount_size
#define clz_trieint        clz_size
#define ctz_trieint        ctz_size
#define ltmask_trieint     ltmask_size
#define lemask_trieint     lemask_size
#define gtmask_trieint     gtmask_size
#define gemask_trieint     gemask_size
#define nextbinpow_trieint nextbinpow_size

// triebits_t: the occupancy-bits type. It needs at least (1 << AMT_DIVBITS)
// bits (32 for the usual AMT_DIVBITS of 5).
#define TRIEBITS_WIDTH 32
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
#else
#  error Could not deduce type of triebits_t.
#endif
#define TRIEBITS_0 TRIEBITS_C(0)
#define TRIEBITS_1 TRIEBITS_C(1)


//=============================================================================
// AMT geometry.

// Bits per AMT level: 5, giving 32 cells per node (as in Clojure and Scala;
// 5 also measured faster than 4 or 6 here).
#define AMT_DIVBITS 5

#define AMT_REMBITS   (TRIEINT_WIDTH % AMT_DIVBITS)
#define AMT_LAYERS    ((TRIEINT_WIDTH + AMT_DIVBITS - TRIEINT_1) / AMT_DIVBITS)
#define AMT_MAX_DEPTH (AMT_LAYERS - TRIEINT_1)
#define AMT_MAX_CELLS (TRIEINT_1 << AMT_DIVBITS)

#define AMT_DIVMASK (~(TRIEINT_MAX << AMT_DIVBITS))
#define AMT_REMMASK (~(TRIEINT_MAX << AMT_REMBITS))

#define AMT_ROOT_BITS  AMT_DIVBITS
#define AMT_NODE_BITS  AMT_DIVBITS
#define AMT_TWIG_BITS  AMT_REMBITS
#define AMT_ROOT_SHIFT (TRIEINT_WIDTH - AMT_ROOT_BITS)
#define AMT_TWIG_SHIFT 0
#define AMT_NODE_MASK  AMT_DIVMASK
#define AMT_TWIG_MASK  AMT_REMMASK

#define AMT_NODE_CELLS (1 << AMT_NODE_BITS)
#define AMT_TWIG_CELLS (1 << AMT_TWIG_BITS)

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
   return (depth < AMT_MAX_DEPTH? AMT_NODE_CELLS : AMT_TWIG_CELLS);
}
static inline bool amtdepth_prefix_match(triebits_t depth,
                                         trieint_t prefix,
                                         trieint_t k) {
   triebits_t shmask = amtdepth_shiftmask(depth);
   // At depth 0 there are no bits above the node to compare. (Shifting by
   // the full width would be undefined behavior.)
   if (shmask >= TRIEINT_WIDTH)
      return true;
   return (prefix >> shmask) == (k >> shmask);
}


//=============================================================================
// FAT geometry.

// FAT nodes have FAT_CELLS cells, chosen so that a node whose cells are
// pointers occupies 256 bytes including its 48-byte header (see struct
// TrieData), i.e. four cache lines: 48 + 26*8 = 256.
#define FAT_CELLS 26

// A key is split into base-FAT_CELLS digits, one per depth; depth 0 holds the
// most significant digit. FAT_MAX_DEPTH is the largest k with
// FAT_CELLS^k <= 2^TRIEINT_WIDTH - 1, so there are FAT_MAX_DEPTH + 1 digits.
// _fat_divs[d] is FAT_CELLS^(FAT_MAX_DEPTH - d), the divisor that extracts
// the digit at depth d. (Changing FAT_CELLS requires recomputing both.)
#if (FAT_CELLS == 26)
#  if (TRIEINT_WIDTH == 64)
#    define FAT_MAX_DEPTH 13
static const trieint_t _fat_divs[FAT_MAX_DEPTH + 1] = {
   TRIEINT_C(2481152873203736576), TRIEINT_C(95428956661682176),
   TRIEINT_C(3670344486987776),    TRIEINT_C(141167095653376),
   TRIEINT_C(5429503678976),       TRIEINT_C(208827064576),
   TRIEINT_C(8031810176),          TRIEINT_C(308915776),
   TRIEINT_C(11881376),            TRIEINT_C(456976),
   TRIEINT_C(17576),               TRIEINT_C(676),
   TRIEINT_C(26),                  TRIEINT_C(1)
};
#  elif (TRIEINT_WIDTH == 32)
#    define FAT_MAX_DEPTH 6
static const trieint_t _fat_divs[FAT_MAX_DEPTH + 1] = {
   TRIEINT_C(308915776), TRIEINT_C(11881376), TRIEINT_C(456976),
   TRIEINT_C(17576),     TRIEINT_C(676),      TRIEINT_C(26),
   TRIEINT_C(1)
};
#  else
#    error Unsupported TRIEINT_WIDTH for FAT_CELLS == 26.
#  endif
#else
#  error _fat_divs and FAT_MAX_DEPTH must be recomputed for this FAT_CELLS.
#endif
#define FAT_LAYERS (FAT_MAX_DEPTH + 1)

static inline trieint_t fatdepth_div(uint8_t depth) {
   return _fat_divs[depth];
}
static inline trieint_t fatdepth_bitindex(uint8_t depth, trieint_t k) {
   return (k / fatdepth_div(depth)) % TRIEINT_C(FAT_CELLS);
}
// The prefix of a node at `depth` that contains `key`: `key` with the digits
// at `depth` and below set to zero.
static inline trieint_t fatdepth_prefix(uint8_t depth, trieint_t key) {
   trieint_t div;
   if (depth == 0) return 0;
   div = fatdepth_div(depth) * TRIEINT_C(FAT_CELLS);
   return key - (key % div);
}
static inline bool fatdepth_prefix_match(uint8_t depth,
                                         trieint_t prefix, trieint_t k) {
   // At depth 0 there are no digits above the node to compare (and the
   // divisor below would overflow).
   trieint_t div;
   if (depth == 0) return true;
   div = fatdepth_div(depth) * TRIEINT_C(FAT_CELLS);
   return (prefix / div) == (k / div);
}


//=============================================================================
// Nodes.

// The per-node metadata (16 bytes).
EXTC typedef struct TrieHeader {
   // The key prefix shared by everything beneath this node, stored exactly
   // (no bits are borrowed for flags: FAT's digit arithmetic uses division,
   // which any borrowed bit would corrupt). A node's own digit, and all
   // digits below it, are zero.
   trieint_t prefix;
   // Which cells are occupied.
   triebits_t bits;
   // The node's depth; a node at the maximum depth for its kind is a twig
   // (its cells are leaves), any other node is a branch (its cells are
   // child nodes).
   uint8_t depth;
   // The size in bytes of a twig's leaves.
   uint8_t leafsize;
   // TRIE_FLAG_ISTRANSIENT, or 0.
   uint8_t flags;
   // The number of allocated cells (AMT only; FAT nodes always have
   // FAT_CELLS).
   uint8_t ncells;
} *TrieHeader_t;

// A node. `base` is the PyObject header of a FAT node, or, for an AMT node,
// a C reference count followed by zeros (see the file comment).
EXTC typedef struct TrieData {
   union {
      PyObject ob;
      struct { pcoll_atomic_u64_t refcount; } c;
   } base;
   struct TrieHeader header;
   char cells[];
} *Trie_t;

// A statically allocated empty twig, used by the lookups to keep their loops
// branch-free. It has the same leading layout as struct TrieData but a
// fixed-size cell array (a struct ending in a flexible array member can't be
// embedded in another struct).
EXTC struct TrieDummyData {
   union {
      PyObject ob;
      struct { pcoll_atomic_u64_t refcount; } c;
   } base;
   struct TrieHeader header;
   void* cells[TRIEBITS_WIDTH];
};

#define TRIE_FLAG_ISTRANSIENT UINT8_C(0x01)

// Naming: trienode_* functions work on nodes of either kind; amtnode_*/amt_*
// and fatnode_*/fat_* work only on their own kind, which callers always know
// from context (AMT nodes only ever point to AMT nodes, and FAT nodes to FAT
// nodes).

static inline bool trienode_is_fat(const Trie_t t) {
   return t->base.ob.ob_type != NULL;
}
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
// An AMT node's bit shift, computed from its depth.
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
   (void)fat;
   return bitindex;
}
static inline triebits_t trienode_occupancy(const Trie_t th) {
   return popcount_triebits(th->header.bits);
}
// Cell access. _subt reads a branch's child pointer; _leaf returns a pointer
// to a twig's leaf (whose size is the node's leafsize). Neither checks the
// node's kind.
static inline Trie_t trienode_subt(const Trie_t th, trieint_t cellindex) {
   return ((Trie_t*)th->cells)[cellindex];
}
static inline void* trienode_leaf(const Trie_t th, trieint_t cellindex) {
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

// Bit scanning. Both return TRIEBITS_WIDTH (which is >= FAT_CELLS) when no
// set bit remains, independent of what ctz returns for 0.
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
   (void)th;
   return TRIEBITS_0;
}
static inline triebits_t amtnode_next_cellindex(Trie_t th, triebits_t prev) {
   (void)th;
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
   (void)th;
   return TRIEBITS_C(FAT_CELLS);
}


//=============================================================================
// Reference counting.
// A function or cell that holds a node owns one reference to it. A newly
// created node has one reference, owned by its creator.

// FAT nodes are Python objects; releasing the last reference runs the node
// type's dealloc (core.h), which releases the node's children or leaves. The
// leaf_decref argument is accepted for symmetry with amtnode_decref() and
// ignored: a FAT node's type knows how to release its own leaves.
static inline void fatnode_decref(Trie_t t, void (*leaf_decref)(void*)) {
   (void)leaf_decref;
   Py_DECREF(&t->base.ob);
}

static inline void trienode_incref(Trie_t th) {
   if (trienode_is_fat(th))
      Py_INCREF(&th->base.ob);
   else
      PCOLL_ATOMIC_U64_FETCH_ADD1(&th->base.c.refcount);
}

// AMT nodes are freed here. After amtnode_decref(t, ...), `t` may no longer
// be valid. `leaf_decref` (if not NULL) is called on each leaf of each twig
// freed.
static inline void amttwig_free(Trie_t t, void (*leaf_decref)(void*)) {
   triebits_t ci, nocc;
   if (leaf_decref) {
      nocc = trienode_occupancy(t);
      for (ci = 0; ci < nocc; ++ci)
         (*leaf_decref)(trienode_leaf(t, ci));
   }
   free(t);
}
static inline void amtnode_free(Trie_t t, void (*leaf_decref)(void*)) {
   uint64_t r;
   Trie_t u, node;
   Trie_t nodes[AMT_LAYERS];
   uint8_t cis[AMT_LAYERS];
   uint8_t noccs[AMT_LAYERS];
   uint8_t ci, nocc, top;
   if (amtnode_is_twig(t)) {
      amttwig_free(t, leaf_decref);
      return;
   }
   // Walk the subtree iteratively, releasing each child and descending into
   // any child whose count reaches zero.
   node = t;
   ci = 0;
   nocc = trienode_occupancy(t);
   top = 0;
   while (1) {
      if (ci < nocc) {
         u = trienode_subt(node, ci);
         r = PCOLL_ATOMIC_U64_FETCH_SUB1(&u->base.c.refcount);
         if (r <= 1) {
            if (amtnode_is_twig(u)) {
               amttwig_free(u, leaf_decref);
            } else {
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
         free(node);
         if (top == 0) return;
         --top;
         node = nodes[top];
         ci = cis[top];
         nocc = noccs[top];
      }
   }
}
static inline void amtnode_decref(Trie_t t, void (*leaf_decref)(void*)) {
   uint64_t r = PCOLL_ATOMIC_U64_FETCH_SUB1(&t->base.c.refcount);
   if (r <= 1)
      amtnode_free(t, leaf_decref);
}


//=============================================================================
// Lookup.
// Both return 1 and set *result to a pointer to the leaf if `key` is present,
// and return 0 otherwise.

static inline int amt_lookup(Trie_t node,
                             trieint_t key,
                             void** result) {
   // The dummy is an empty twig: whenever a descent misses, the loop moves
   // to it instead of branching, and the final check then fails.
   static struct TrieDummyData amtnode_dummytwig_data = {
      .header = {
         .depth = AMT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   Trie_t amtnode_dummytwig = (Trie_t)&amtnode_dummytwig_data;
   triebits_t depth, bitindex, cellindex, bit;
   trieint_t shift, keyshift;
   bool ok;
   void* cell;
   // AMT descents may skip depths, so re-read each node's depth.
   depth = node->header.depth;
   while (depth < AMT_MAX_DEPTH) {
      shift = amtdepth_shift(depth);
      keyshift = (key >> shift);
      // Prefixes are checked once, at the twig.
      bitindex = keyshift & AMT_DIVMASK;
      bit = node->header.bits & (TRIEBITS_1 << bitindex);
      ok = (bit > 0);
      // A persistent AMT node has exactly as many cells as occupied bits, so
      // when the bit is absent the computed cell index can be one past the
      // end. It is masked to 0 (always a valid index for a non-empty node)
      // in that case; the value read is then discarded. (`0 - ok` rather
      // than `-ok` avoids an MSVC warning about negating an unsigned value.)
      cellindex = amtnode_bit2cellindex(node, bitindex);
      cellindex &= (triebits_t)((triebits_t)0 - (triebits_t)ok);
      cell = trienode_subt(node, cellindex);
      node = (ok? (Trie_t)cell : amtnode_dummytwig);
      depth = node->header.depth;
   }
   ok = amtnode_prefix_match(node, key);
   bitindex = (key & AMT_TWIG_MASK);
   bit = node->header.bits & (TRIEBITS_1 << bitindex);
   ok &= (bit > 0);
   cell = trienode_leaf(node, amtnode_bit2cellindex(node, bitindex));
   if (result && ok)
      *result = cell;
   return (ok > 0);
}

static inline int fat_lookup(Trie_t node, trieint_t key, void** result) {
   static struct TrieDummyData fatnode_dummytwig_data = {
      .header = {
         .depth = FAT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   Trie_t fatnode_dummytwig = (Trie_t)&fatnode_dummytwig_data;
   triebits_t depth, cellindex, bit;
   bool ok;
   void* cell;
   void* discard;
   result = (result? result : &discard);
   // Every FAT node has all FAT_CELLS cells allocated, so reading any cell
   // index below FAT_CELLS is in bounds whether or not it is occupied.
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
// Paths and iteration.

#if (AMT_LAYERS > FAT_LAYERS)
#  define TRIE_MAX_LAYERS AMT_LAYERS
#else
#  define TRIE_MAX_LAYERS FAT_LAYERS
#endif

// A path from a root down to a node: node[0] is the root and node[steps-1]
// the last node reached. index[i] is the *bit* index (the key's digit at
// that node's depth), not a compacted AMT cell index, so that the key can be
// reconstructed from the path.
struct TriePathData {
   uint8_t steps;
   // For a failed find: whether the key falls beneath node[steps-1] (so a
   // new leaf belongs inside it) rather than diverging above it.
   bool is_beneath;
   // For a successful find or iteration step: the leaf pointer.
   void* value;
   uint8_t index[TRIE_MAX_LAYERS];
   Trie_t node[TRIE_MAX_LAYERS];
};
EXTC typedef struct TriePathData TriePath, *TriePath_t;

// The key and leaf of a path that ends at a leaf.
static inline trieint_t triepath_key(const TriePath_t p) {
   uint8_t ii = p->steps - 1;
   return trienode_prefix(p->node[ii]) + p->index[ii];
}
static inline void* triepath_val(const TriePath_t p) {
   return p->value;
}

// Finds `key` in AMT `a`, filling in `path`. Returns 1 if found.
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
      a = trienode_subt(a, cellindex);
      bitindex = amtdepth_bitindex(a->header.depth, key);
      path->node[path->steps] = a;
      path->index[path->steps] = bitindex;
      path->steps++;
      cellindex = amtnode_bit2cellindex(a, bitindex);
      ok = amtnode_prefix_match(a, key);
      if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
         path->is_beneath = ok;
         return 0;
      }
   }
   path->is_beneath = 1;
   path->value = trienode_leaf(a, cellindex);
   return 1;
}

// Iteration over an AMT in key order:
//    TriePath it;
//    for (int ok = amt_firstpath(amt, &it); ok; ok = amt_nextpath(&it))
//       use(triepath_key(&it), triepath_val(&it));
static inline int amt_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t top = 0;
   triebits_t bi;
   // A tree with 0 or 1 items is a lone twig (possibly the empty node).
   if (amtnode_is_twig(a)) {
      bi = trienode_first_bitindex(a);
      if (bi >= TRIEBITS_WIDTH)
         return 0;
      path->index[0] = bi;
      path->node[0] = a;
      path->steps = 1;
      path->value = trienode_leaf(a, amtnode_bit2cellindex(a, bi));
      return 1;
   }
   do {
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, amtnode_bit2cellindex(node, bi));
      ++top;
   } while (!amtnode_is_twig(node));
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
   if (bi < TRIEBITS_WIDTH) {
      path->index[top] = bi;
      path->value = trienode_leaf(node, amtnode_bit2cellindex(node, bi));
      return 1;
   } else if (top == 0) {
      return 0;
   }
   // Climb to the nearest ancestor with a later child...
   --top;
   while (1) {
      node = path->node[top];
      bi = trienode_next_bitindex(node, path->index[top]);
      if (bi < TRIEBITS_WIDTH) {
         path->index[top] = bi;
         break;
      } else if (top == 0) {
         return 0;
      } else {
         --top;
      }
   }
   // ...then descend to its first leaf.
   node = trienode_subt(node, amtnode_bit2cellindex(node, path->index[top]));
   while (!amtnode_is_twig(node)) {
      ++top;
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, amtnode_bit2cellindex(node, bi));
   }
   ++top;
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, amtnode_bit2cellindex(node, bi));
   return 1;
}

// Finds `key` in FAT `a`, filling in `path`. Returns 1 if found.
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
      a = trienode_subt(a, bitindex);
      bitindex = (triebits_t)fatdepth_bitindex(a->header.depth, key);
      path->node[path->steps] = a;
      path->index[path->steps] = bitindex;
      path->steps++;
      ok = fatnode_prefix_match(a, key);
      if (!(ok & ((a->header.bits >> bitindex) & TRIEBITS_1))) {
         path->is_beneath = ok;
         return 0;
      }
   }
   path->is_beneath = 1;
   path->value = trienode_leaf(a, bitindex);
   return 1;
}

// Iteration over a FAT in key order (same usage as amt_firstpath()).
static inline int fat_firstpath(Trie_t a, TriePath_t path) {
   Trie_t node = a;
   triebits_t top = 0;
   triebits_t bi;
   if (fatnode_is_twig(a)) {
      bi = trienode_first_bitindex(a);
      if (bi >= FAT_CELLS)
         return 0;
      path->index[0] = bi;
      path->node[0] = a;
      path->steps = 1;
      path->value = trienode_leaf(a, bi);
      return 1;
   }
   do {
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, bi);
      ++top;
   } while (!fatnode_is_twig(node));
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
   if (bi < FAT_CELLS) {
      path->index[top] = bi;
      path->value = trienode_leaf(node, bi);
      return 1;
   } else if (top == 0) {
      return 0;
   }
   --top;
   while (1) {
      node = path->node[top];
      bi = trienode_next_bitindex(node, path->index[top]);
      if (bi < FAT_CELLS) {
         path->index[top] = bi;
         break;
      } else if (top == 0) {
         return 0;
      } else {
         --top;
      }
   }
   node = trienode_subt(node, path->index[top]);
   while (!fatnode_is_twig(node)) {
      ++top;
      bi = trienode_first_bitindex(node);
      path->index[top] = bi;
      path->node[top] = node;
      node = trienode_subt(node, bi);
   }
   ++top;
   bi = trienode_first_bitindex(node);
   path->index[top] = bi;
   path->node[top] = node;
   path->steps = top + 1;
   path->value = trienode_leaf(node, bi);
   return 1;
}

// Completes `path` from `node`, placed at level `top`, down to the first leaf
// beneath it. Returns 0 only for an empty node.
static inline int fat_path_descend(TriePath_t path, int top, Trie_t node) {
   triebits_t bi;
   while (!fatnode_is_twig(node)) {
      bi = trienode_first_bitindex(node);
      path->index[top] = (uint8_t)bi;
      path->node[top] = node;
      node = trienode_subt(node, bi);
      ++top;
   }
   bi = trienode_first_bitindex(node);
   if (bi >= FAT_CELLS)
      return 0;
   path->index[top] = (uint8_t)bi;
   path->node[top] = node;
   path->steps = (uint8_t)(top + 1);
   path->value = trienode_leaf(node, bi);
   return 1;
}
// Moves `path` to the first leaf after cell index[top] of node[top], climbing
// toward the root as needed; levels below `top` are ignored. Returns 0 if
// there is no such leaf (including when top < 0).
static inline int fat_path_advance(TriePath_t path, int top) {
   while (top >= 0) {
      Trie_t node = path->node[top];
      triebits_t bi = trienode_next_bitindex(node, path->index[top]);
      if (bi < FAT_CELLS) {
         path->index[top] = (uint8_t)bi;
         if (fatnode_is_twig(node)) {
            path->steps = (uint8_t)(top + 1);
            path->value = trienode_leaf(node, bi);
            return 1;
         }
         return fat_path_descend(path, top + 1, trienode_subt(node, bi));
      }
      --top;
   }
   return 0;
}
// Sets `path` to the first leaf of FAT `a` whose key is >= `key`. Returns 0
// if there is none. Iterators use this to find their place again after the
// tree they walk has changed.
static inline int fat_seekpath(Trie_t a, trieint_t key, TriePath_t path) {
   Trie_t node = a;
   int top = 0;
   triebits_t bi;
   while (1) {
      path->node[top] = node;
      if (!fatnode_prefix_match(node, key)) {
         // Everything beneath the node is either after the key or before it.
         if (key < node->header.prefix)
            return fat_path_descend(path, top, node);
         return fat_path_advance(path, top - 1);
      }
      bi = (triebits_t)fatdepth_bitindex(node->header.depth, key);
      path->index[top] = (uint8_t)bi;
      if (!((node->header.bits >> bi) & TRIEBITS_1))
         return fat_path_advance(path, top);
      if (fatnode_is_twig(node)) {
         path->steps = (uint8_t)(top + 1);
         path->value = trienode_leaf(node, bi);
         return 1;
      }
      node = trienode_subt(node, bi);
      ++top;
   }
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_TRIE_H
