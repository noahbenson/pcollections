///////////////////////////////////////////////////////////////////////////////
// _C/amt.h
// Definition of the Array Mapped Trie (AMT) types.


//=============================================================================
// Initialization.

#ifndef _PCOLLECTIONS__C_AMT_H
#define _PCOLLECTIONS__C_AMT_H


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
#define AMT_LAYERS    ((TRIEINT_WIDTH + AMT_DIVBITS - TRIEINT_1) / \
                        AMT_DIVBITS)
#define AMT_MAX_DEPTH (AMT_LAYERS - TRIEINT_1)
#define AMT_MAX_CELLS (TRIEINT_1 << AMT_DIVBITS)

// The masks for layers with either the diviser or the mod number of bits.
#define AMT_DIVMASK (~(TRIEINT_MAX << AMT_DIVBITS))
#define AMT_REMMASK (~(TRIEINT_MAX << AMT_REMBITS))


//=============================================================================
// Types

// Definitions for AMT nodes --------------------------------------------------

#define AMT_ROOT_BITS  AMT_DIVBITS
#define AMT_NODE_BITS  AMT_DIVBITS
#define AMT_TWIG_BITS  AMT_REMBITS
#define AMT_ROOT_SHIFT (TRIEINT_WIDTH - AMT_ROOT_BITS)
#define AMT_TWIG_SHIFT 0
#define AMT_ROOT_MASK  AMT_DIVMASK
#define AMT_NODE_MASK  AMT_DIVMASK
#define AMT_TWIG_MASK  AMT_REMMASK

#define AMT_ROOT_CELLS (1 << AMT_ROOT_BITS)
#define AMT_NODE_CELLS (1 << AMT_NODE_BITS)
#define AMT_TWIG_CELLS (1 << AMT_TWIG_BITS)

// And some handy map/seq operations.
static inline triebits_t amtdepth_shift(triebits_t depth) {
   return (depth == AMT_MAX_DEPTH? 0 : AMT_ROOT_SHIFT - depth*AMT_NODE_BITS);
}
static inline triebits_t amtdepth_mask(triebits_t depth) {
   return (depth < AMT_MAX_DEPTH? AMT_DIVBITS : AMT_REMBITS);
}
static inline triebits_t amtdepth_bitindex(triebits_t depth, trieint_t h) {
   return (
      depth < AMT_MAX_DEPTH
      ? (h >> amtdepth_shift(depth)) & AMT_DIVMASK
      : h & AMT_REMMASK);
}
static inline triebits_t amtdepth_maxcells(triebits_t depth) {
   return (
      depth < AMT_MAX_DEPTH
      ? AMT_NODE_CELLS
      : AMT_TWIG_CELLS);
}

//-----------------------------------------------------------------------------
// AMT data structure definition.
// As of this writing, most of the time this will be used for 64-bit hashes.
// The comments document the size of a 64-bit hash AMT with 32 cells per node.

EXTC struct AMTData {
   // -------------------------------------------------------------------------
   // First we have critical header data. This contains 20 bytes.
   struct TrieHeader header;
   // Other metadata related to the node.
   // Size: 4 bytes (32 bites)
   uint8_t depth;  // The node's trie depth.
   uint8_t shift;  // The node's bit-shift.
   uint8_t ncells;  // How many cells allocated in this node?
   uint8_t leafsize;  // The size of leaves in the twig nodes.
   // This leaves us at the 24-byte mark, which is pretty good when there are
   // 1 to 5 children (thus the node fits inside the 64-byte line, and not bad
   // when there are up to 13 children (128 bytes), or even up to 29 children
   // (256 bytes). If there are 32 children (FAT nodes), we get 280 bytes.
   // -------------------------------------------------------------------------
   // The cells (which are allocated based on the size of the container type
   // but there are always a max of 32 of them except potentially for the root
   // and the twig nodes, which may have 16, 8, 4, or 2).
   char cells[];
};
EXTC typedef struct AMTData* AMT_t;

// We use a dummy AMT object in the lookup function as a sort of hack for
// enabling a branchless algorithm. We want this to exist in the data segment
// but to have a full set of cells; therefore, we need to put it in another
// structure to make it viable.
EXTC struct AMTDummyData {
   struct AMTData amt;
   void* cells[AMTBITS_WIDTH];
};

// AMT-related utility functions:
#define AMT_FLAG_TRANSIENT_BIT 0
static inline bool amt_is_transient(const AMT_t th) {
   // The lowest bit is the density bit!
   return th->header.prefix & TRIEINT_1;
}
static inline bool amtnode_is_twig(const AMT_t th) {
   return th->depth == AMT_MAX_DEPTH;
}
static inline trieint_t amtnode_prefix(const AMT_t th) {
   // Remove the transient bit (if it's set).
   return (th->header.prefix & ~TRIENT_1);
}
static inline bool amtnode_prefix_match(const AMT_t th, trieint_t k) {
   uint8_t shift = th->shift;
   return (th->header.prefix >> shift) == (k >> shift);
}
static inline trieint_t amtnode_minleaf(const AMT_t th) {
   return (th->header.prefix & (TRIEINT_MAX << th->shift));
}
static inline trieint_t amtnode_maxleaf(const AMT_t th) {
   return (th->header.prefix | ~(TRIEINT_MAX << th->shift));
}
static inline triebits_t amtnode_hasbit(const AMT_t amt, triebits_t bitindex) {
   return (amt->header.bits & (AMTBITS_1 << bitindex)) > 0;
}
static inline triebits_t amtnode_firstbit(const AMT_t amt) {
   return ctz_triebits(amt->header.bits);
}
static inline triebits_t amtnode_nextbit(const AMT_t amt, triebits_t gebit) {
   return ctz_triebits(amt->header.bits >> gebit) + gebit;
}
static inline triebits_t amtnode_bit2cellindex(const AMT_t amt,
                                          triebits_t bitindex) {
   return popcount_triebits(amt->header.bits & ltmask_triebits(bitindex));
}
static inline triebits_t amtnode_occupancy(const AMT_t amt) {
   return popcount_triebits(amt->header.bits);
}
static inline void* amttwig_cell(const AMT_t node, trieint_t cellindex) {
   return (void*)(node->cells + cellindex*node->leafsize);
}
static inline AMT_t amtbranch_cell(const AMT_t node, trieint_t cellindex) {
   return ((AMT_t*)node->cells)[cellindex];
}
static inline size_t amtnode_cellsize(const AMT_t node) {
   return amtnode_is_twig(node)? node->leafsize : sizeof(void*);
}
static inline void* amtnode_cell(const AMT_t node, trieint_t cellindex) {
   return amtnode_is_twig(node)
      ? amttwig_cell(node, cellindex)
      : amtbranch_cell(node, cellindex);
}
static inline void amttwig_setcell(AMT_t a, triebits_t cellindex, void* v) {
   memcpy((void*)(a->cells + cellindex*a->leafsize), v, a->leafsize);
}
static inline void amtbranch_setcell(AMT_t a, triebits_t cellindex, AMT_t cell) {
   ((AMT_t*)a->cells)[cellindex] = cell;
}
static inline void amtnode_setcell(AMT_t a, triebits_t cellindex, void* v) {
   size_t cs = amtnode_cellsize(a);
   v = (void*)(amtnode_is_twig(a)? v, &v);
   memcpy((void*)(a->cells + cellindex*cs), v, cs);
}
static inline void amtnode_incref(AMT_t a) {
   atomic_fetch_add(&a->refcount, 1);
}
// Note that after calling amtnode_decref(a) it is quite possible that a is no
// longer a valid pointer!
static inline void amtnode_decref(AMT_t a, void (*leaf_deref)(void*)) {
   uint64_t r;
   r = atomic_fetch_sub_explicit(&a->refcount, 1, memory_order_acq_rel);
   if (r == 1) {
      // We need to deallocate this node, so we need to decrement the reference
      // count on any child node or leaf. There may be arbitrarily many nodes
      // to visit.
      // However, once any node is being deallocated, we can overwrite the
      // first two words in its memory (the refcount and the prefix) and use
      // them as indicators of where we are in our dereferencing (and
      // deallocation) search.
      triebits_t ci = 0, nocc = amtnode_occupancy(a);
      AMT_t tmp;
      bool is_twig = amtnode_is_twig(a);
      ((AMT_t*)a)[0] = 0;
      ((uintptr_t*)a)[1] = 0;
      while (1) {
         if (ci < nocc) {
            // We need to decref child ci.
            if (!is_twig) {
               tmp = amtbranch_cell(a, ci);
               r = atomic_fetch_sub_explicit(
                  &tmp->refcount,
                  1,
                  memory_order_acq_rel);
               if (r == 1) {
                  // This node also needs to be deallocated. Incrememnt this
                  // stack-layer's cellindex then move on to the child cell.
                  ((uintptr_t*)a)[1] = ci + 1;
                  ((AMT_t*)tmp)[0] = a;
                  a = tmp;
                  is_twig = amtnode_is_twig(a);
                  nocc = amtnode_occupancy(a);
                  continue;
               }
            } else if (leaf_deref) {
               (*leaf_deref)(amttwig_cell(a, ci));
            }
            ++ci;
         } else {
            // At this point, we've decrefed all children, so it's safe to free
            // this node.
            tmp = a;
            a = ((AMT_t*)a)[0];
            free(tmp);
            if (!a) break;
            ci = ((uintptr_t*)a)[1];
            // Nodes above any other node can't be twigs.
            is_twig = 0;
            nocc = amtnode_occupancy(a);
         }
      }
      // We should have freed the original a in the final else clause above.
   }
}

static inline int amt_lookup(const AMT_t node,
                             trieint_t key,
                             void** result) {
   static struct AMTDummyData amtnode_dummytwig_data = {
      .amt = {
         .depth = AMT_MAX_DEPTH,
         .leafsize = sizeof(void*)
      }
   };
   static AMT_t amtnode_dummytwig = (AMT_t)&amtnode_dummytwig_data;
   triebits_t depth, bitindex, cellindex, bit;
   trieint_t shift, keyshift;
   bool ok;
   void* cell;
   // In an map, we are not likely to get sequential depths of hash; instead,
   // we are likely to skip depths, so we switch for the depth on every node.
   depth = node->depth;
   while (depth < AMT_MAX_DEPTH) {
      // All non-twigs use the same code. First check the addresses match.
      shift = amtnode_shift(depth);
      keyshift = (key >> shift);
      ok = (keyshift == (node->header.prefix >> shift));
      // Next, check if the appropriate bit is set.
      bitindex = keyshift & AMT_DIVMASK;
      bit = node->header.bits & (AMTBITS_1 << bitindex);
      // Things are only okay if bit is truthy.
      ok &= (bit > 0);
      // Now get the pointer to the cell itself.
      cellindex = amtnode_bit2cellindex(node, bitindex);
      cell = amtbranch_cell(node, cellindex);
      // Now, IF everything is okay, we set node to *cell; otherwise, we set
      // it to the dummy amt, which is always a twig and never contains any
      // cells.
      node = (ok? cell : amtnode_dummytwig);
      depth = node->depth;
   }
   // This is a twig, so we return from this node.
   ok = amtnode_prefix_match(node, key);
   // Next, check if the appropriate bit is set.
   bitindex = (key & AMT_TWIG_MASK);
   bit = node->header.bits & (AMTBITS_1 << bitindex);
   // Things are only okay if bit is truthy.
   ok &= (bit > 0);
   // get the cell itself.
   cell = amttwig_cell(node, cellindex);
   // Now, IF everything is okay, we set *result to *cell and return 1;
   // otherwise we don't set it and we return 0.
   if (result && ok)
      *result = cell;
   return (ok > 0);
}

//-----------------------------------------------------------------------------
// Iteration.
// Iteration requires a special data structure to store the iteration data in:
struct AMTPathData {
   // If an entry is found in the AMT for a find operation or during iteration,
   // its value is placed here.
   // (8 bytes)
   void* value;
   // The stack of nodes followed along the path; node[0] is always the start
   // node, and node[steps-1] is always the final step.
   // (8 * 13 = 104 bytes; 112 total)
   AMT_t node[AMT_LAYERS];
   // The bitindex of the current match in the associated node.
   // (13 bytes; 125 total)
   uint8_t bitindex[AMT_LAYERS];
   uint8_t steps;  // The number of steps currently in the path.
   // 1 if the searched for node is beneath the current node.
   bool is_beneath;  
   // (127 total bytes)
};
EXTC typedef struct AMTPathData AMTPath, *AMTPath_t;
// Get the key found by an AMTPath.
static inline trieint_t amtpath_key(const AMTPath_t p) {
   uint8_t ii = p->steps - 1;
   return amtnode_prefix(p->node[ii]) | p->bitindex[ii];
}
// Fill in a path to a node (or fail to find it).
static inline int amtpath_find(AMTPath_t path, AMT_t a, trieint_t key) {
   triebits_t cellindex, bitindex = (key >> a->shift) & amtdepth_mask(a->depth);
   bool ok = amtnode_prefix_match(a, key);
   path->node[0] = a;
   path->bitindex[0] = bitindex;
   path->steps = 1;
   if (!(ok & ((a->header.bits >> bitindex) & AMTBITS_1))) {
      path->is_beneath = ok;
      path->value = NULL;
      return 0;
   }
   cellindex = amtnode_bit2cellindex(a, bitindex);
   while (a->depth < AMT_MAX_DEPTH) {
      // descend a node...
      a = amtbranch_cell(a, cellindex);
      bitindex = (key >> a->shift) & amtdepth_mask(a->depth);
      path->node[path->steps] = a;
      path->bitindex[path->steps] = bitindex;
      path->steps++;
      cellindex = amtnode_bit2cellindex(a, bitindex);
      ok = amtnode_prefix_match(a, key);
      if (!(ok & ((a->header.bits >> bitindex) & AMTBITS_1))) {
         // If we didn't match prefixes, it's a 0-step match; if we didn't match
         // on the on the bit, it's still a step-1 match.
         path->is_beneath = ok;
         path->value = NULL;
         return 0;
      } 
      // Otherwise, we're continuing to descend.
   }
   // We've reached a twig node, so we can set the value.
   path->is_beneath = 1;
   path->value = amttwig_cell(a, cellindex);
   return 1;
}
// Starts at the given step in path; finds the first bit that is set in the
// node and descends, repeating on each descended node. Ignores the current
// settings of bitindex at step and the current path->steps.
static inline int _amtpath_down(AMTPath_t path, uint8_t step) {
   AMT_t node = path->node[step];
   uint8_t bitindex = ctz_triebits(node->header.bits);
   path->bitindex[step] = bitindex;
   while (node->depth < AMT_MAX_DEPTH) {
      // We've found a cell in a non-twig node; we're going to descend into it.
      node = amtnode_cell(amt, amtnode_bit2cellindex(node, bitindex));
      bitindex = ctz_triebits(node->header.bits);
      step++;
      path->node[step] = node;
      path->bitindex[step] = bitindex;
   }
   // We've found a cell in a twig node; we're going to return it.
   path->steps = step + 1;
   path->value = amttwig_cell(node, amtnode_bit2cellindex(node, bitindex));
   path->is_beneath = 1;
   return 1;   
}
// _amtpath_nextdown(path, step) looks for the next bit ... takes an AMTPath
// and a step and, starting from that step, finds the next bit that is set and
// descends to the first leaf under that bit position in the hash. If there is
// no next bit in the path without popping a step, then this returns
// 0. Otherwise, fills in the path down to the next leaf, writes key and val,
// and returns 1.
static inline int _amtpath_nextdown(AMTPath_t path, uint8_t step) {
   triebits_t cellindex, mask, bits;
   // We assume that the current node's data has already been written into the
   // path aside from bitindex.
   AMT_t node = path->node[step];
   triebits_t bitindex = path->bitindex[step] + 1;
   bits = node->header.bits >> bitindex;
   // If there aren't more bits, we return 0.
   if (bits == 0)
      return 0;
   bitindex += ctz_triebits(bits);
   path->bitindex[step] = bitindex;
   if (node->depth == AMT_MAX_DEPTH) {
      path->is_beneath = 1;
      path->steps = step + 1;
      path->value = amttwig_cell(node, amtnode_bit2cellindex(node, bitindex));
      return 1;
   }
   // Otherwise, descend...
   node = amtbranch_cell(node, amtnode_bit2cellindex(node, bitindex));
   path->node[++step] = node
   return _amtpath_down(path, step);
}
// amtpath_next(AMTPath_t path) returns the next element (after the selected
// element in the path) that is in the AMT referenced by the path.
static inline int amtpath_next(AMTPath_t path) {
   triebits_t bitindex, cellindex;
   uint8_t step = path->steps - 1;
   bool ok;
   while (1) {
      // First, see if there are more bits here.
      ok = _amtpath_nextdown(path, step);
      if (ok | (step == 0))
         return ok;
      step--;
   }
   // Unreachable.
   return 0;
}
// amtpath_head(path, node) finds the first bit set in the node.
static inline int amtpath_head(AMTPath_t path, AMT_t a) {
   uint8_t bitindex;
   if (a->header.bits == 0)
      return 0;
   path->node[0] = a;
   return _amtpath_down(path, 0);
}
// To iterate over AMT nodes, the correct method is as so:
//    struct AMTPath iter;
//    for (int ok = amtpath_head(&iter, amt); ok; ok = amtpath_next(&iter)) {
//        process_key_value(amtpath_key(&iter), iter.value);
//    }


//=============================================================================
// Node Constructors and Destructors.

// The amtnode_alloc() function just allocates space for the AMT node; as such,
// it only needs to know the leafsize and the number of cells. All data is
// returned uninitialized.
// A leafsize of 0 indicates that no cells are allocated.
static inline AMT_t amtnode_alloc(uint8_t cellsize, uint8_t ncells) {
   return (AMT_t)malloc(sizeof(struct AMTData) + ncells*cellsize);
}

// amtnode_init() initializes memory for an AMT node that has been allocated by
// amtnode_alloc, or potentially memory that has been written into another
// struct.
// This function sets in the AMT node:
//  - refcount (set to 1)
//  - prefix
//  - depth
//  - shift
//  - ncells
//  - leafsize
// It does not set the bits or the cells, which remain uninitialized.
static inline void amtnode_init(AMT_t a,
                                trieint_t prefix,
                                uint8_t leafsize,
                                uint8_t depth,
                                uint8_t ncells,
                                bool is_transient) {
   uint8_t maxcells = amtdepth_maxcells(depth);
   ncells = (ncells > maxcells? maxcells : ncells);
   // The refcount should always start at 1. (We can set directly because no
   // other thread can legally access this memory yet.)
   a->refcount = 1;
   // The prefix must encode the transient bit.
   a->header.prefix = prefix | (trieint_t)is_transient;
   a->leafsize = leafsize;
   a->ncells = ncells;
   a->depth = depth;
   a->shift = amtdepth_shift(depth);
}

// amtnode_new() creates a new AMT object and returns it partially initialized.
// If given a leafsize of 0, this is interpreted as being for a Python object
// in the twigs (with refcounting). Nodes whose depths aren't AMT_MAX_DEPTH
// (i.e. twigs) are always refcounted during updates.  All sizes but 0 treat
// the leaves as untyped bytes. The max leaf size is 255 bytes (max of uint8).
// The returned AMT node has the following data set:
//  - refcount (set to 1)
//  - prefix
//  - depth
//  - shift
//  - ncells
//  - leafsize
// It does not set the bits or the cells, which are uninitialized.
static inline AMT_t amtnode_new(trieint_t prefix,
                                uint8_t leafsize,
                                uint8_t depth,
                                uint8_t ncells,
                                bool is_transient) {
   AMT_t a = amtnode_alloc(leafsize * (depth == AMT_MAX_DEPTH), ncells);
   amtnode_init(a, prefix, leafsize, depth, ncells, is_transient);
   return a;
}

// amttwig_1leaf: Make a new leaf that contains a single pair (key / value).
// Incrememnts the refcount for the value as well.
static inline AMT_T amttwig_1leaf(trieint_t key, void* val,
                                  uint8_t leafsize,
                                  bool is_transient,
                                  void (*leaf_incref)(void*)) {
   uint8_t ncells = (is_transient? 4 : 1);
   AMT_t h = amtnode_new(
      key & ~AMT_REMMASK,
      leafsize,
      AMT_MAX_DEPTH,
      ncells,
      is_transient);
   h->header.bits = (AMTBITS_1 << (key & AMT_REMMASK));
   memcpy(amttwig_cell(h, 0), val, leafsize);
   if (leaf_incref)
      (*leaf_incref)(val);
   return h;
}

// amtnode_cells_incref() incrememnts the references to all the cells in the given
// amt node.
static inline void amtnode_cells_incref(AMT_t a, void (*leaf_incref)(void*)) {
   uint8_t ii, ncells = amtnode_occupancy(a);
   if (!amtnode_is_twig(a)) {
      for (ii = 0; ii < ncells; ++ii)
         amtnode_incref(((AMT_t*)a->cells)[ii]);
   } else if (leaf_incref) {
      for (ii = 0; ii < ncells; ++ii)
         (*leaf_incref)(a->cells + cellsize*ii);
   }
}

// amtnode_and() creates a new AMT object that is identical to the given AMT
// object except for having one additional cell/bit set (bitindex bi).
// The leaf_incref function is for incrementing the reference count of a leaf.
// (If it's null, then no leaf reference counting is tracked.)
// Objects are correctly reference-counted by this function.
static inline AMT_t amtnode_and(AMT_t a,
                                triebits_t bi,
                                void* val,
                                void (*leaf_incref)(void*),
                                triebits_t mincells) {
   AMT_t new_amt;
   triebits_t
      ncells,
      nocc = amtnode_occupancy(a),
      cellsize = amtnode_cellsize(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(bi);
   bool
      is_tr = amtnode_is_transient(a);
   // We're setting a new cell or updating an existing cell.
   if (bits & (AMTBITS_1 << k)) {
      // If we're setting a bit that has already been set, so we just copy
      // all the memory then overwrite.
      ncells = (nocc < mincells? mincells : nocc);
      new_amt = amtnode_new(a->header.prefix, a->leafsize, a->depth, ncells, is_tr);
      memcpy(new_amt->cells, a->cells, nocc*cellsize);
      amtnode_setcell(new_amt, ci, v);
      new_amt->header.bits = bits;
   } else {
      // We're setting a new bit, so we need to allocate more cells.
      ++nocc;
      ncells = (nocc < mincells? mincells : nocc);
      new_amt = amtnode_new(a->header.prefix, a->leafsize, a->depth, ncells, is_tr);
      memcpy(new_amt->cells, a->cells, ci*cellsize);
      amtnode_setcell(new_amt, ci, v);
      memcpy(
         new_amt->cells + (ci + 1)*cellsize,
         a->cells + ci*cellsize,
         (nocc - ci - 1)*cellsize);
      new_amt->header.bits = bits | (AMTBITS_1 << bi);
   }
   // Finally, we step through all the objects pointed to in the cells and
   // incref them.
   amtnode_cells_incref(new_amt);
   // Now we can return the new amt.
   return new_amt;
}
// amtnode_but() creates a new AMT object that is identical to the given AMT
// object except for having one cell/bit unset (bitindex bi).
// The leaf_incref function is for incrementing the reference count of a leaf.
// (If it's null, then no leaf reference counting is tracked.)
// Objects are correctly reference-counted by this function.
static inline AMT_t amtnode_but(AMT_t a,
                                triebits_t bi,
                                void (*leaf_incref)(void*),
                                triebits_t mincells) {
   AMT_t new_amt;
   triebits_t
      ncells = amtnode_occupancy(a),
      cellsize = amtnode_cellsize(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(bi);
   bool is_tr = amtnode_is_transient(a);
   if (bits & (AMTBITS_1 << k)) {
      // We're deleting an existing bit, so we need fewer cells.
      --ncells;
      ncells = (ncells < mincells? mincells : ncells);
      new_amt = amtnode_new(a->header.prefix, a->leafsize, a->depth, ncells, is_tr);
      memcpy(new_amt->cells, a->cells, ci*cellsize);
      memcpy(
         new_amt->cells + (ci + 1)*cellsize,
         a->cells + ci*cellsize,
         (ncells - ci - 1)*cellsize);
      new_amt->header.bits &= ~(AMTBITS_1 << bi);
   } else {
      // we're deleting a non-existant bit, so we don't make any changes.
      ncells = (ncells < mincells? mincells : ncells);
      new_amt = amtnode_new(a->header.prefix, a->leafsize, a->depth, ncells, is_tr);
      memcpy(new_amt->cells, a->cells, ncells*cellsize);
      new_amt->header.bits = bits;
   }
   // Finally, we step through all the objects pointed to in the cells and
   // incref them.
   // Finally, we step through all the objects pointed to in the cells and
   // incref them.
   amtnode_cells_incref(new_amt);
   // Now we can return the new amt.
   return new_amt;
}
// amtnode_set() creates a new AMT object that is identical to the given AMT object
// except for having one additional cell/bit set (bitindex bi) and being
// transient when the argument a is itself a persistent object, otherwise
// mutates a in-place to have the new value and returns a.
// The leaf_incref and leaf_decref functions are for incrementing/decrementing
// the reference count of a leaf (when null, no leaf reference counting is
// tracked). Objects are correctly reference-counted by this function.
static inline AMT_t amtnode_set(AMT_t a,
                                triebits_t bi,
                                void* val,
                                void (*leaf_incref)(void*),
                                void (*leaf_decref)(void*)) {
   AMT_t tmp;
   triebits_t ncells, ii,
      nocc = amtnode_occupancy(a),
      cellsize = amtnode_cellsize(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(bi);
   bool is_twig = amtnode_is_twig(a);
   if (amtnode_is_transient(a)) {
      // We're either setting a new cell or updating an existing cell.
      if (bits & (AMTBITS_1 << k)) {
         // We're setting a bit that has already been set, so we don't have to
         // reallocate anything. We do have to derefcount our objects, though.
         if (!is_twig)
            amtnode_decref(((AMT_t*)a->cells)[ci]);
         else if (leaf_decref)
            (*leaf_decref)((void*)(a->cells + ci*cellsize));
      } else {
         // We're adding a new cell.
         if (a->ncells > nocc) {
            // There's already space for our new cell!
            memmove(
               a->cells + cellsize*(ci+1),
               a->cells + cellsize*ci,
               cellsize*(nocc - ci));
         } else {
            // There isn't space for it, so we need to allocate a new AMT node
            // with the next highest power of 2 ncells.
            ncells = nextpow2_triebits(nocc);
            a = amtnode_and(a, bi, val, leaf_incref, ncells);
            // The amtnode_and operation does all the work, so we can just
            // return a now.
            return a;
         }
      }
      a->header.bits = bits | (AMTBITS_1 << k);
      // Set the new value now:
      if (!is_twig) {
         ((AMT_t*)a->cells)[ci] = (AMT_t)val;
         amtnode_incref((AMT_t)val);
      } else {
         memcpy((void*)(a->cells + ci*cellsize), val, cellsize);
         if (leaf_incref)
            (*leaf_incref)(val);
      }
   } else {
      // We're starting from a persistent map, so we'll need to start by making
      // a transient copy. Even if the bit is set, we make the number of cells
      // equal to the next highest power of two (because it's a transient).
      ncells = nextpow2_triebits(nocc);
      a = amtnode_and(a, k, val, leaf_incref, ncells);
   }
   // Now we can return the new or updated amt.
   return a;
}
// amtnode_del() creates a new AMT object that is identical to the given AMT object
// a except for having one cell/bit unset (bitindex bi) if a is a persistent
// AMT. Otherwise, if a is a transient AMT, the item is deleted in-place and a
// is returned.
// The leaf_incref and leaf_decref functions are for incrementing and
// decrementing the reference count of a leaf.  (If it's null, then no leaf
// reference counting is tracked.) Objects are correctly reference-counted by
// this function.
static inline AMT_t amtnode_del(AMT_t a,
                                triebits_t bi,
                                void (*leaf_incref)(void*),
                                void (*leaf_decref)(void*)) {
   triebits_t
      bit = (AMTBITS_1 << bi),
      nocc = amtnode_occupancy(a),
      cellsize = amtnode_cellsize(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(bi);
   if (amtnode_is_transient(a)) {
      if (bits & bit) {
         // The bit is set, so we should refcount the previous
         // value and unset the bit.
         a->header.bits &= ~bit;
         if (!amtnode_is_twig(a))
            amtnode_decref(amtbranch_cell(a, ci));
         else if (leaf_decref)
            (*leaf_decref)(amttwig_cell(a, ci));
      } else {
         // The bit isn't set, so there's no change!
      }
   } else {
      // The argument a is a persistent node, so no matter what we make a
      // duplicate, but we can do that using amtnode_but() then setting the
      // transient bit.
      // For transients, we always allocate a number of cells equal to the next
      // power of 2 greater than the old occupancy.
      a = amtnode_but(a, bi, leaf_incref, nextpow2_triebits(nocc));
      amtnode_set_transient(a, 1);
   }
   return a;
}

// amtnode_subjoin: join two sub-AMTs by a parent AMT and return it.
// Runs incref on both a and b for the new AMT.
static inline AMT_t amtnode_subjoin(AMT_t a, AMT_t b, bool is_transient) {
   // Join two subtrees; first find the highest bit they differ on.
   // Note that this initially is the bit count from the high bit.
   trieint_t highdiff = clz_trieint(a->header.prefix ^ b->header.prefix);
   uint8_t mask, depth = highdiff / AMT_DIVMOD;
   trieint_t shift = aptdepth_shift(depth);
   triebits_t aii, bii;
   AMT_t parent = amtnode_new(
      a->header.prefix & gemask_trieint(shift),
      a->leafsize,
      depth,
      2,
      is_transient);
   mask = amtdepth_mask(depth);
   aii = (a->header.prefix >> shift) & mask;
   bii = (b->header.prefix >> shift) & mask;
   parent->header.bits = (AMTBITS_1 << aii) | (AMTBITS_1 << bii);
   parent->cells[0] = (aii < bii? a : b);
   parent->cells[1] = (aii < bii? b : a);
   amtnode_incref(a);
   amtnode_incref(b);
   return parent;
}


//=============================================================================
// AMT API Functions.

// The amt_empty() function can be used to get the canonical empty PAMT for a
// particular leafsize.
// There is no canonical empty tamt; you just use the canonical pamt for this.
AMT_t amt_empty(uint8_t leafsize);

// Join an item to an existing persistent amt.
inline AMT_t amt_anditem(AMT_t a, trieint_t key, void* val,
                         void (*leaf_incref)(void*)) {
   AMT_t node;
   struct AMTPath path;
   int found;
   uint8_t step;
   // If a is empty, just return a new twig.
   if (a->header.bits == 0)
      return amttwig_1leaf(key, val, a->leafsize, 0, leaf_incref);
   // We we find the key, we're ready to start allocating at the leaf (steps-1)
   // node; otherwise, we need to worry about the is_beneath.
   found = amtpath_find(path, a, key);
   step = path->steps - 1;
   node = path->node[step];
   if (!found) {
      if (!path->is_beneath) {
         // The path diverges here--we need a new leafpair and to connect it
         // to the highest subtree.
         node = amtnode_subjoin(
             amttwig_1leaf(key, val, a->leafsize, 0, leaf_incref),
             node,
             0);
      } else if (node->depth == AMT_MAX_DEPTH) {
         // The current node is a twig, and we need to add a leaf.
         node = amtnode_and(node, path->bitindex[step], val, leaf_incref, 1);
      } else {
         // the current node needs a new child which is a 1leaf twig.
         node = amtnode_and(
            node,
            path->bitindex[step],
            amttwig_1leaf(key, val, a->leafsize, 0, leaf_incref),
            leaf_incref,
            2);
      }
   } else {
      // If the item was found, we're just going to use amtnode_and initially.
      node = amtnode_and(node, path->bitindex[step], val, leaf_incref, 1);
   }
   // We now need to build a new path by backtracking up the steps.
   while (step > 0) {
      --step;
      node = amtnode(
         path->node[step],
         path->bitindex[step],
         node,
         leaf_incref,
         1);
   }
   // Now return the new node!
   return node;
}
static inline AMT_t pamt_butitem(AMT_t a, trieint_t key,
                                 void (*leaf_incref)(void*)) {
   AMT_t node, tmp;
   struct AMTPath path;
   int found;
   uint8_t step, nocc;
   // If we don't find the key, then there's no change here.
   found = amtpath_find(path, a, key);
   if (!found) return a;
   // Otherwise, we need to build up a new path by backtracking along the
   // steps. Start by
   node = NULL;
   step = path->steps - 1;
   do {
      --step;
      tmp = path->node[step];
      if (node) {
         node = amtnode_and(tmp, path->bitindex[step], node, leaf_incref, 1);
      } else {
         nocc = amtnode_occupancy(tmp);
         if (nocc > 2)
            node = amtnode_but(tmp, path->bitindex[step], leaf_incref, 1);
         else if (nocc == 2) {
            if (amtnode_is_twig(tmp))
               node = amtnode_but(tmp, path->bitindex[step], leaf_incref, 1);
         }
      }
   } while (step > 0);
   return (node? node : amt_empty(a->leafsize));
}
static inline void tamt_setitem(AMT_t a, trieint_t key, void* val,
                                void (*leaf_incref)(void*),
                                void (*leaf_decref)(void*)) {
   AMT_t node, newnode;
   struct AMTPath path;
   int found;
   uint8_t step;
   // If a is empty, just return a new twig.
   if (a->header.bits == 0)
      return amttwig_1leaf(key, val, a->leafsize, 1, leaf_incref);
   // If we find the key, we're ready to start setting/allocating at the leaf
   // (steps-1) node; otherwise, we need to worry about the is_beneath.
   found = amtpath_find(path, a, key);
   step = path->steps - 1;
   node = path->node[step];
   if (!found) {
      if (!path->is_beneath) {
         // The path diverges here--we need a new leafpair and to connect it
         // to the highest subtree.
         newnode = amtnode_subjoin(
             amttwig_1leaf(key, val, a->leafsize, 1, leaf_incref),
             node,
             1);
      } else if (node->depth == AMT_MAX_DEPTH) {
         // The current node is a twig, and we need to add a leaf.
         newnode = amtnode_set(
            node, path->bitindex[step], val, leaf_incref, leaf_decref);
      } else {
         // the current node needs a new child which is a 1leaf twig.
         newnode = amtnode_set(
            node,
            path->bitindex[step],
            amttwig_1leaf(key, val, a->leafsize, 1, leaf_incref),
            leaf_incref,
            2);
      }
   } else {
      // If the item was found, we're just going to use amtnode_set initially.
      newnode = amtnode_set(
         node, path->bitindex[step], val, leaf_incref, leaf_decref);
   }
   // If the newnode is identical to the old node, everything has been
   // updated already.
   if (newnode == node)
      return a;
   // We now need to build a new path by backtracking up the steps.
   while (step > 0) {
      --step;
      node = newnode;
      newnode = amtnode_set(
         path->node[step],
         path->bitindex[step],
         node,
         leaf_incref,
         leaf_decref);
      if (newnode == node)
         return a;
   }
   // Now return the new tree!
   return node;
}
static inline void tamt_delitem(AMT_t amt, trieint_t key,
                                void (*leaf_incref)(void*),
                                void (*leaf_decref)(void*)) {
   AMT_t node, newnode, tmp;
   struct AMTPath path;
   int found;
   uint8_t step, nocc;
   // If we don't find the key, then there's no change here.
   found = amtpath_find(path, a, key);
   if (!found) return a;
   // Otherwise, we may need to build up a new path by backtracking along the
   // steps, but if we set a node that doesn't change, we can just return a.
   node = NULL;
   step = path->steps - 1;
   do {
      --step;
      tmp = node;
      node = path->node[step];
      if (node) {
         newnode = amtnode_set(
            node, path->bitindex[step], tmp, leaf_incref, leaf_decref);
         if (newnode == node) return a;
         else node = newnode;
      } else {
         nocc = amtnode_occupancy(node);
         if (nocc > 2) {
            newnode = amtnode_del(
               node, path->bitindex[step], leaf_incref, leaf_decref);
            if (newnode == node) return a;
            else node = newnode;
         } else if (nocc == 2) {
            if (amtnode_is_twig(tmp)) {
               newnode = amtnode_del(
                  node, path->bitindex[step], leaf_incref, leaf_decref);
               if (newnode == node) return a;
               else node = newnode;
            } else
               node = NULL;
         } else
            node = NULL;
      }
   } while (step > 0);
   return (node? node : amt_empty(a->leafsize));
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_AMT_H
