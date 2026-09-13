///////////////////////////////////////////////////////////////////////////////
// _c/fat.h
// Definition of the Fixed Arity Trie (FAT) types.


//=============================================================================
// Initialization.

#ifndef _PCOLLECTIONS__C_FAT_H
#define _PCOLLECTIONS__C_FAT_H


#include <Python.h>
#include <stdatomic.h>
#include <string.h>
#include "uintbits.h"
#include "trie.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


#define SEQ_ROOT_BITS  TRIEINT_REMBITS
#define SEQ_NODE_BITS  TRIEINT_REMBITS
#define SEQ_TWIG_BITS  TRIEINT_REMBITS
#define SEQ_ROOT_SHIFT (AMTHASH_WIDTH - SEQ_ROOT_BITS)
#define SEQ_TWIG_SHIFT 0
#define SEQ_ROOT_MASK  TRIEINT_REMMASK
#define SEQ_NODE_MASK  TRIEINT_DIVMASK
#define SEQ_TWIG_MASK  TRIEINT_DIVMASK

#define SEQ_ROOT_CELLS (1 << SEQ_ROOT_BITS)
#define SEQ_NODE_CELLS (1 << SEQ_NODE_BITS)
#define SEQ_TWIG_CELLS (1 << SEQ_TWIG_BITS)

static inline trieint_t seq_node_shift(amtbits_t depth) {
   return SEQ_ROOT_SHIFT - depth*SEQ_NODE_BITS;
}
static inline trieint_t seq_shift(amtbits_t depth) {
   return (depth > 0? seq_node_shift(depth) : SEQ_TWIG_SHIFT);
}
static inline amtbits_t seq_mask(amtbits_t depth) {
   return (depth > 0? HASH_STDSHIFT : HASH_OFFSHIFT);
}
static inline trieint_t seq_cellindex(trieint_t hash,
                                      triebits_t depth) {
   trieint_t shift = seq_shift(depth);
   trieint_t mask = seq_mask(depth);
   return ((hash >> shift) & mask);
}
static inline triebits_t seq_bitset(const AMT_t node,
                                    trieint_t hash) {
   return amt_getbit(seq_cellindex(hash, node->depth));
}


inline int seq_lookup(const AMT_t node, trieint_t key, void** result) {
   static struct AMTDummyData AMT_dummy = {
      .amt = {
         .depth = TRIEINT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   triebits_t depth;
   trieint_t shift, cellindex, addrmask;
   uintptr_t ok, tmp;
   void* cell;
   void* dummy;
   result = (result? &dummy : result);
   // In an seq, we branch once to the appropriate depth then fall down to
   // the max depth.
   depth = node->depth;
   shift = seq_shift(depth);
   addrmask = trieint_gemask(shift);
#  define SEQ_STEP(bitcount)                                                  \
     do {                                                                     \
         cellidx = trieint_cellindex_bits(key, shift, (bitcount));            \
         ok = (node->bits & (TRIEBITS_C(1) << cellidx)? UINTPTR_MAX : 0);     \
         cell = (node->depth == depth                                         \
            ? ((void**)node->cells)[cellidx]                                  \
            : node);                                                          \
         node = (AMT_t)(                                                     \
            + ((+ok) & ((uintptr_t)cell))                                     \
            | ((~ok) & ((uintptr_t)&AMT_dummy)));                            \
         depth--;                                                             \
         shift -= SEQ_NODE_BITS;                                             \
         addrmask = ~(~addrmask >> (bitcount));                               \
      } while (0)
   // Note that in the above, we don't check the address; this is because we
   // only need to check that the twig address matches; if we end up in a weird
   // twig node because we didn't check the addresses on the way down, that's
   // fine because that twig node's address won't match, and we check the
   // address in the twig step code below.
#  define SEQ_ROOT_STEP() SEQ_STEP(SEQ_ROOT_BITCOUNT)
#  define SEQ_NODE_STEP() SEQ_STEP(SEQ_NODE_BITCOUNT)
#  define SEQ_TWIG_STEP() \
     do {                                                                     \
         cellidx = key & SEQ_TWIG_MASK;                                      \
         ok = (                                                               \
            + (node->address == (key & addrmask)? UINTPTR_MAX : 0)            \
            & (node->bits & (TRIEBITS_C(1) << cellidx)? UINTPTR_MAX : 0));    \
         cell = (void*)(node->cells + node->leafsize*cellidx);                \
         cell = (void*)(ok & ((uintptr_t)cell));                              \
      } while (0)
   // In this switch statement, we deliberately do not use breaks because we   
   // want to jump to the initial depth then trickle down the lower depths.
   switch (depth) {
   case 0: SEQ_ROOT_STEP();
#  if (TRIEINT_MAX_DEPTH == 1)
   case 1:  // 8-bit hash twig
      SEQ_TWIG_STEP();
#  else
   case 1: SEQ_NODE_STEP();
   case 2: SEQ_NODE_STEP();
#  if (TRIEINT_MAX_DEPTH == 3)
   case 3:  // 16-bit hash twig
      SEQ_TWIG_STEP();
#  else
   case 3: SEQ_NODE_STEP();
   case 4: SEQ_NODE_STEP();
   case 5: SEQ_NODE_STEP();
#  if (TRIEINT_MAX_DEPTH == 6)
   case 6:  // 32-bit hash twig
      SEQ_TWIG_STEP();
#  else
   case 6: SEQ_NODE_STEP();
   case 7: SEQ_NODE_STEP();
   case 8: SEQ_NODE_STEP();
   case 9: SEQ_NODE_STEP();
   case 10: SEQ_NODE_STEP();
   case 11: SEQ_NODE_STEP();
#  if (TRIEINT_MAX_DEPTH == 12)
   case 12:  // 64-bit hash twig
      SEQ_TWIG_STEP();
#  else
   case 12: SEQ_NODE_STEP();
   case 13: SEQ_NODE_STEP();
   case 14: SEQ_NODE_STEP();
   case 15: SEQ_NODE_STEP();
   case 16: SEQ_NODE_STEP();
   case 17: SEQ_NODE_STEP();
   case 18: SEQ_NODE_STEP();
   case 19: SEQ_NODE_STEP();
   case 20: SEQ_NODE_STEP();
   case 21: SEQ_NODE_STEP();
   case 22: SEQ_NODE_STEP();
   case 23: SEQ_NODE_STEP();
   case 24: SEQ_NODE_STEP();
   case 25: SEQ_NODE_STEP();
   case 26: SEQ_NODE_STEP();
   case 27: SEQ_NODE_STEP();
   case 28: SEQ_NODE_STEP();
   case 29: SEQ_NODE_STEP();
   case 30: SEQ_NODE_STEP();
   case 31:  // 128-bit hash twig
      SEQ_TWIG_STEP();
#  endif  // if (TRIEINT_MAX_DEPTH == 12)
#  endif  // if (TRIEINT_MAX_DEPTH == 6)
#  endif  // if (TRIEINT_MAX_DEPTH == 3)
#  endif  // if (TRIEINT_MAX_DEPTH == 1)
   }
#  undef SEQ_TWIG_STEP
#  undef SEQ_NODE_STEP
#  undef SEQ_ROOT_STEP
#  undef SEQ_STEP
   // At this point, we've stepped through each depth; if ok is nonzero then
   // node is the ready to be returned. Otherwise, we return a failure code.
   *result = cell;
   return (ok > 0);
}

static inline AMT_t seq_new(uint16_t leafsize, uint16_t depth) {
   AMT_t h;
   size_t objsize;
   size_t ncells;
   ncells = (depth == TRIE_MAX_DEPTH? SEQ_TWIG_CELLS : SEQ_NODE_CELLS);
   leafsize &= AMT_MAX_LEAFSIZE;
   objsize = (leafsize && depth == TRIE_MAX_DEPTH? leafsize : sizeof(void*));
   objsize = sizeof(struct AMTData) + ncells*objsize;
   h = malloc(objsize);
   h->depth = depth;
   h->leafsize = leafsize;
   h->bits = TRIEBITS_C(0);
   return h;
}
