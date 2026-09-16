///////////////////////////////////////////////////////////////////////////////
// _c/amt.h
// AMT node construction, update primitives, and per-item operations.
//
// See trie.h for the node layout. AMT nodes are plain C memory with an
// atomic reference count; their leaves are raw bytes that hold no Python
// references (the leaf_incref/leaf_decref callbacks are kept for generality,
// and are no-ops in this project). An AMT node allocates only as many cells
// as it needs (at least its occupancy), and its occupied cells are packed in
// bit order, so inserting or removing a cell shifts the cells after it.
//
// Naming:
//  - amt_*        works on persistent or transient nodes without modifying
//                 its input. amt_and()/amt_but() return either the input
//                 unchanged or a new node that inherits the input's
//                 transient flag; amt_anditem()/amt_butitem() are the
//                 persistent per-item API.
//  - tamt_*       mutates a transient tree in place, first copying
//                 ("claiming") any persistent node it needs to change.
//
// Invariants:
//  - Minimal tree: a branch always has at least two children (a branch left
//    with one child is replaced by that child), and a node has no occupied
//    cells only if it is the canonical empty node returned by amt_empty().
//  - Reference ownership: a function that returns a node returns a
//    reference its caller owns. A child pointer passed in as `val` to a
//    branch cell transfers its reference into the cell.

#ifndef _PCOLLECTIONS__C_AMT_H
#define _PCOLLECTIONS__C_AMT_H


#include <Python.h>
#include <string.h>
#include "uintbits.h"
#include "trie.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


//=============================================================================
// Node Constructors.

// Allocates zeroed memory for an AMT node with `ncells` cells.
static inline Trie_t amtnode_alloc(uint8_t cellsize, uint8_t ncells) {
   Trie_t a = (Trie_t)calloc(1, sizeof(struct TrieData) + (size_t)ncells*cellsize);
   if (!a)
      // The trie operations have no error path; running out of memory
      // while building a node is fatal.
      Py_FatalError("pcollections: out of memory allocating a trie node");
   return a;
}

// Initializes the header of a node from amtnode_alloc(), with a reference
// count of 1. The bits and cells are left as allocated (zero).
static inline void amtnode_init(Trie_t a,
                                trieint_t prefix,
                                uint8_t leafsize,
                                uint8_t depth,
                                uint8_t ncells,
                                bool is_transient) {
   uint8_t maxcells = amtdepth_maxcells(depth);
   ncells = (ncells > maxcells? maxcells : ncells);
   a->base.c.refcount = 1;
   a->header.prefix = prefix;
   a->header.leafsize = leafsize;
   a->header.ncells = ncells;
   a->header.depth = depth;
   a->header.flags = (is_transient? TRIE_FLAG_ISTRANSIENT : 0);
}

// Allocates and initializes a node with no occupied cells.
static inline Trie_t amtnode_new(trieint_t prefix,
                                 uint8_t leafsize,
                                 uint8_t depth,
                                 uint8_t ncells,
                                 bool is_transient) {
   size_t cellsize = (depth == AMT_MAX_DEPTH? leafsize : sizeof(void*));
   Trie_t a = amtnode_alloc((uint8_t)cellsize, ncells);
   amtnode_init(a, prefix, leafsize, depth, ncells, is_transient);
   return a;
}

// Makes a twig holding the single leaf `val` at `key`.
static inline Trie_t amt_1leaf(trieint_t key, void* val,
                               uint8_t leafsize,
                               bool is_transient,
                               void (*leaf_incref)(void*)) {
   uint8_t ncells = (is_transient? 4 : 1);
   Trie_t h = amtnode_new(
      key & ~AMT_TWIG_MASK,
      leafsize,
      AMT_MAX_DEPTH,
      ncells,
      is_transient);
   h->header.bits = (TRIEBITS_1 << (key & AMT_TWIG_MASK));
   trienode_set_leaf(h, 0, val);
   if (leaf_incref)
      (*leaf_incref)(val);
   return h;
}

// Increments the references held by every occupied cell of `a`: its
// children, or (if leaf_incref is given) its leaves.
static inline void amt_cells_incref(Trie_t a, void (*leaf_incref)(void*)) {
   triebits_t ii, nocc = trienode_occupancy(a);
   if (!amtnode_is_twig(a)) {
      for (ii = 0; ii < nocc; ++ii)
         trienode_incref(trienode_subt(a, ii));
   } else if (leaf_incref) {
      for (ii = 0; ii < nocc; ++ii)
         (*leaf_incref)(trienode_leaf(a, ii));
   }
}


//=============================================================================
// Copying primitives (the input is never modified).

// Returns a copy of `a` with cell `bi` set to `val`, with room for at least
// `mincells` cells.
static inline Trie_t amt_and(Trie_t a,
                             triebits_t bi,
                             void* val,
                             void (*leaf_incref)(void*),
                             triebits_t mincells) {
   Trie_t new_a;
   bool is_twig = amtnode_is_twig(a);
   size_t cellsize = amtnode_cellsize(a);
   triebits_t
      nocc = trienode_occupancy(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(a, bi);
   bool is_tr = trie_is_transient(a);
   triebits_t ncells, ii, newocc;
   if (bits & (TRIEBITS_1 << bi)) {
      // We're overwriting an existing cell, so the occupancy doesn't change.
      ncells = (nocc < mincells? mincells : nocc);
      new_a = amtnode_new(
         a->header.prefix, a->header.leafsize, a->header.depth, ncells, is_tr);
      memcpy(new_a->cells, a->cells, nocc*cellsize);
      new_a->header.bits = bits;
      newocc = nocc;
   } else {
      // We're setting a new bit, so we need one more cell than before.
      newocc = nocc + 1;
      ncells = (newocc < mincells? mincells : newocc);
      new_a = amtnode_new(
         a->header.prefix, a->header.leafsize, a->header.depth, ncells, is_tr);
      memcpy(new_a->cells, a->cells, ci*cellsize);
      memcpy(
         new_a->cells + (ci + 1)*cellsize,
         a->cells + ci*cellsize,
         (nocc - ci)*cellsize);
      new_a->header.bits = bits | (TRIEBITS_1 << bi);
   }
   if (is_twig)
      trienode_set_leaf(new_a, ci, val);
   else
      trienode_set_subt(new_a, ci, (Trie_t)val);
   // Copied children are now shared with `a`; the new child's reference is
   // transferred in. For a twig, leaf_incref (if given) is called for every
   // leaf, including the new one.
   if (is_twig) {
      if (leaf_incref)
         for (ii = 0; ii < newocc; ++ii)
            (*leaf_incref)(trienode_leaf(new_a, ii));
   } else {
      for (ii = 0; ii < newocc; ++ii)
         if (ii != ci)
            trienode_incref(trienode_subt(new_a, ii));
   }
   return new_a;
}

// Returns a copy of `a` with cell `bi` cleared, or `a` itself (with no new
// reference) if that cell is already empty.
static inline Trie_t amt_but(Trie_t a,
                             triebits_t bi,
                             void (*leaf_incref)(void*),
                             triebits_t mincells) {
   Trie_t new_a;
   size_t cellsize = amtnode_cellsize(a);
   triebits_t
      nocc = trienode_occupancy(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(a, bi);
   bool is_tr = trie_is_transient(a);
   triebits_t ncells;
   if (!(bits & (TRIEBITS_1 << bi)))
      return a;
   ncells = nocc - 1;
   ncells = (ncells < mincells? mincells : ncells);
   new_a = amtnode_new(
      a->header.prefix, a->header.leafsize, a->header.depth, ncells, is_tr);
   memcpy(new_a->cells, a->cells, ci*cellsize);
   memcpy(
      new_a->cells + ci*cellsize,
      a->cells + (ci + 1)*cellsize,
      (nocc - ci - 1)*cellsize);
   new_a->header.bits = bits & ~(TRIEBITS_1 << bi);
   amt_cells_incref(new_a, leaf_incref);
   return new_a;
}

// Joins two subtrees whose prefixes diverge under a new parent at the depth
// of their highest differing digit. `a`'s reference is consumed; `b` gains a
// reference.
static inline Trie_t amt_subjoin(Trie_t a, Trie_t b, bool is_transient) {
   // Join two subtrees; first find the highest bit they differ on.
   trieint_t highdiff = clz_trieint(a->header.prefix ^ b->header.prefix);
   uint8_t depth = (uint8_t)(highdiff / AMT_DIVBITS);
   trieint_t shift = amtdepth_shift(depth);
   triebits_t aii = amtdepth_bitindex(depth, a->header.prefix);
   triebits_t bii = amtdepth_bitindex(depth, b->header.prefix);
   Trie_t parent = amtnode_new(
      a->header.prefix & gemask_trieint(shift),
      a->header.leafsize,
      depth,
      2,
      is_transient);
   parent->header.bits = (TRIEBITS_1 << aii) | (TRIEBITS_1 << bii);
   trienode_set_subt(parent, 0, (aii < bii? a : b));
   trienode_set_subt(parent, 1, (aii < bii? b : a));
   trienode_incref(b);
   return parent;
}


//=============================================================================
// In-place primitives for transient trees.
// amtnode_set()/amtnode_del() change a transient node in place and return
// it (or, if it lacks room, a larger transient copy). Given a persistent
// node, they instead return a changed, transient copy; the caller then
// replaces the node with the copy.

static inline Trie_t amtnode_set(Trie_t a,
                                 triebits_t bi,
                                 void* val,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   bool is_twig = amtnode_is_twig(a);
   size_t cellsize = amtnode_cellsize(a);
   triebits_t
      nocc = trienode_occupancy(a),
      bits = a->header.bits,
      ci = amtnode_bit2cellindex(a, bi);
   if (!trie_is_transient(a)) {
      triebits_t ncells = nextbinpow_triebits(nocc + 1);
      Trie_t new_a = amt_and(a, bi, val, leaf_incref, ncells);
      trienode_set_transient(new_a, true);
      return new_a;
   }
   if (bits & (TRIEBITS_1 << bi)) {
      if (!is_twig) {
         Trie_t old = trienode_subt(a, ci);
         trienode_set_subt(a, ci, (Trie_t)val);
         amtnode_decref(old, leaf_decref);
      } else {
         // Take the new leaf's references before releasing the old leaf's,
         // and release only once the node is consistent.
         char old[256];  // leafsize is a uint8_t, so this always fits.
         memcpy(old, trienode_leaf(a, ci), a->header.leafsize);
         if (leaf_incref)
            (*leaf_incref)(val);
         trienode_set_leaf(a, ci, val);
         if (leaf_decref)
            (*leaf_decref)(old);
      }
      return a;
   } else {
      if (a->header.ncells > nocc) {
         memmove(
            a->cells + cellsize*(ci + 1),
            a->cells + cellsize*ci,
            cellsize*(nocc - ci));
      } else {
         // No room: return a larger transient copy. `a` is not released
         // here; whoever holds it (the parent's cell, or the caller for the
         // root) releases it when replacing it with the copy.
         triebits_t ncells = nextbinpow_triebits(nocc + 1);
         Trie_t new_a = amt_and(a, bi, val, leaf_incref, ncells);
         trienode_set_transient(new_a, true);
         return new_a;
      }
      a->header.bits = bits | (TRIEBITS_1 << bi);
   }
   // A branch child's reference is transferred into the cell.
   if (is_twig) {
      trienode_set_leaf(a, ci, val);
      if (leaf_incref)
         (*leaf_incref)(val);
   } else {
      trienode_set_subt(a, ci, (Trie_t)val);
   }
   return a;
}

static inline Trie_t amtnode_del(Trie_t a,
                                 triebits_t bi,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   triebits_t
      bit = (TRIEBITS_1 << bi),
      nocc = trienode_occupancy(a),
      ci = amtnode_bit2cellindex(a, bi);
   if (!trie_is_transient(a)) {
      Trie_t new_a = amt_but(a, bi, leaf_incref, nextbinpow_triebits(nocc));
      if (new_a != a)
         trienode_set_transient(new_a, true);
      return new_a;
   }
   if (!(a->header.bits & bit)) {
      return a;
   }
   // The bit is set: unset it, shift the following cells down to close the
   // gap (cell indices are popcounts, so the cells must stay packed in bit
   // order; see amtnode_bit2cellindex()), then release the removed cell.
   {
      size_t cellsize = amtnode_cellsize(a);
      char old[256];  // the removed cell, released after the update.
      bool is_twig = amtnode_is_twig(a);
      memcpy(old, a->cells + ci*cellsize, cellsize);
      memmove(
         a->cells + ci*cellsize,
         a->cells + (ci + 1)*cellsize,
         (nocc - ci - 1)*cellsize);
      a->header.bits &= ~bit;
      if (!is_twig)
         amtnode_decref(*(Trie_t*)old, leaf_decref);
      else if (leaf_decref)
         (*leaf_decref)(old);
   }
   return a;
}


//=============================================================================
// Per-item operations.

// Returns a new reference to the canonical empty AMT for `leafsize` (see
// core.h).
Trie_t amt_empty(uint8_t leafsize);

// If `node` is a branch with a single child, returns (a new reference to)
// that child in its place; otherwise returns `node`. `orig` is the node that
// `node` replaces at this level. If `node` is a new copy (node != orig),
// nothing else refers to it and it is released here; if it is `orig` itself
// (changed in place), its parent's cell still holds it, and it is released
// when that cell is overwritten with the child.
static inline Trie_t amt_collapse(Trie_t node, Trie_t orig,
                                  void (*leaf_decref)(void*)) {
   if (!amtnode_is_twig(node) && trienode_occupancy(node) == 1) {
      Trie_t child = trienode_subt(node, 0);
      trienode_incref(child);
      if (node != orig)
         amtnode_decref(node, leaf_decref);
      return child;
   }
   return node;
}

// Returns a tree equal to `a` except that `key` maps to `val`. `a` is not
// modified.
static inline Trie_t amt_anditem(Trie_t a, trieint_t key, void* val,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   if (a->header.bits == 0)
      return amt_1leaf(key, val, a->header.leafsize, trie_is_transient(a),
                       leaf_incref);
   found = triepath_amtfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         // The key lies outside this subtree: join a new twig to it.
         node = amt_subjoin(
            amt_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                     leaf_incref),
            node,
            trie_is_transient(node));
      } else if (amtnode_is_twig(node)) {
         node = amt_and(node, path.index[step], val, leaf_incref, 1);
      } else {
         node = amt_and(
            node,
            path.index[step],
            amt_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                     leaf_incref),
            NULL,
            2);
      }
   } else {
      node = amt_and(node, path.index[step], val, leaf_incref, 1);
   }
   // Copy each ancestor, pointing it at the new child.
   while (step > 0) {
      --step;
      node = amt_and(path.node[step], path.index[step], node, NULL, 1);
   }
   return node;
}

// Returns a tree equal to `a` except that `key` is absent, or `a` itself
// (with no new reference) if `key` is not present. Empty subtrees are
// removed, single-child branches collapsed, and a tree that becomes empty is
// replaced by amt_empty().
static inline Trie_t amt_butitem(Trie_t a, trieint_t key,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   found = triepath_amtfind(&path, a, key);
   if (!found) return a;
   step = path.steps - 1;
   node = amt_but(path.node[step], path.index[step], leaf_incref, 1);
   // While the new node is empty, remove it from its parent instead. (Each
   // such node is a fresh copy nothing else refers to.)
   while (step > 0 && trienode_occupancy(node) == 0) {
      Trie_t emptied = node;
      --step;
      node = amt_but(path.node[step], path.index[step], leaf_incref, 1);
      amtnode_decref(emptied, NULL);
   }
   // Only the node where the removal stopped can have dropped to one child.
   node = amt_collapse(node, path.node[step], NULL);
   if (step == 0 && trienode_occupancy(node) == 0) {
      uint8_t leafsize = node->header.leafsize;
      amtnode_decref(node, NULL);
      return amt_empty(leafsize);
   }
   while (step > 0) {
      --step;
      node = amt_and(path.node[step], path.index[step], node, NULL, 1);
   }
   return node;
}

// Changes transient tree `a` in place so that `key` maps to `val`, and
// returns the tree's root (which may be a new node). Consumes the caller's
// reference to `a` (the returned root carries it); a caller that wants to
// keep `a` itself must hold its own extra reference.
static inline Trie_t tamt_setitem(Trie_t a, trieint_t key, void* val,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node, newnode;
   TriePath path;
   int found;
   uint8_t step;
   if (a->header.bits == 0) {
      Trie_t result = amt_1leaf(key, val, a->header.leafsize, true, leaf_incref);
      amtnode_decref(a, leaf_decref);
      return result;
   }
   found = triepath_amtfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         newnode = amt_subjoin(
            amt_1leaf(key, val, a->header.leafsize, true, leaf_incref),
            node,
            true);
      } else if (amtnode_is_twig(node)) {
         newnode = amtnode_set(node, path.index[step], val,
                               leaf_incref, leaf_decref);
      } else {
         newnode = amtnode_set(
            node,
            path.index[step],
            amt_1leaf(key, val, a->header.leafsize, true, leaf_incref),
            NULL,
            NULL);
      }
   } else {
      newnode = amtnode_set(node, path.index[step], val,
                            leaf_incref, leaf_decref);
   }
   // Point each ancestor at the new child. An ancestor that already holds
   // it is transient, as are all nodes above it, so nothing above changes.
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, amtnode_bit2cellindex(parent, bi)) == newnode)
         return a;
      node = newnode;
      newnode = amtnode_set(parent, bi, node, NULL, NULL);
   }
   // The root was replaced: release the caller's reference to the old one.
   if (newnode != a)
      amtnode_decref(a, leaf_decref);
   return newnode;
}

// Changes transient tree `a` in place so that `key` is absent, and returns
// the tree's root. Reference handling is as for tamt_setitem(); structure is
// maintained as for amt_butitem().
static inline Trie_t tamt_delitem(Trie_t a, trieint_t key,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node;
   Trie_t orig_child;
   TriePath path;
   int found;
   uint8_t step;
   found = triepath_amtfind(&path, a, key);
   if (!found) return a;
   step = path.steps - 1;
   node = amtnode_del(path.node[step], path.index[step], leaf_incref, leaf_decref);
   // While the node is empty, remove it from its parent instead. If it was
   // a claimed copy, nothing else refers to it, so release it; otherwise
   // clearing the parent's cell releases it.
   while (step > 0 && trienode_occupancy(node) == 0) {
      orig_child = path.node[step];
      --step;
      Trie_t parent = path.node[step];
      Trie_t emptied = node;
      node = amtnode_del(parent, path.index[step], leaf_incref, leaf_decref);
      if (emptied != orig_child)
         amtnode_decref(emptied, leaf_decref);
   }
   node = amt_collapse(node, path.node[step], leaf_decref);
   if (step == 0 && trienode_occupancy(node) == 0) {
      uint8_t leafsize = node->header.leafsize;
      amtnode_decref(node, leaf_decref);
      if (node != a)
         amtnode_decref(a, leaf_decref);
      return amt_empty(leafsize);
   }
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, amtnode_bit2cellindex(parent, bi)) == node)
         return a;
      node = amtnode_set(parent, bi, node, NULL, NULL);
   }
   if (node != a)
      amtnode_decref(a, leaf_decref);
   return node;
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_AMT_H
