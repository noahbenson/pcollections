///////////////////////////////////////////////////////////////////////////////
// _c/fat.h
// FAT node construction, update primitives, and per-item operations.
//
// See trie.h for the node layout. Every FAT node has FAT_CELLS cells
// and a cell's index equals its bit index, so setting or clearing a cell
// never moves any other cell and a node never needs to grow.
//
// FAT nodes are Python objects (see trie.h). The node types, one per cell
// size, are created per interpreter in core.h; fatnode_new() allocates from
// them. A twig's leaves must use one of the layouts described at
// fatleaf_nrefs() below, which is how a node's type knows which Python
// references its leaves hold.
//
// Naming:
//  - fat_*        works on persistent or transient nodes without modifying
//                 its input. fat_and()/fat_but() return either the input
//                 unchanged or a new node that inherits the input's
//                 transient flag; fat_anditem()/fat_butitem() are the
//                 persistent per-item API.
//  - tfat_*       mutates a transient tree in place, first copying
//                 ("claiming") any persistent node it needs to change.
//
// Invariants:
//  - Dense tree: every occupied cell of a branch at depth d holds a child at
//    depth d+1 (a branch may have a single child). Only a tree's
//    root may start below depth 0.
//  - A node has no occupied cells only if it is the canonical empty node
//    returned by fat_empty().
//  - Reference ownership: a function that returns a node returns a
//    reference its caller owns. A child pointer passed in as `val` to a
//    branch cell transfers its reference into the cell. Leaves are copied
//    in, and `leaf_incref` (if given) is called for each leaf of a newly
//    built twig, including the new one.
//  - Garbage collection: a node is tracked by the collector whenever it can
//    reach a Python object that the collector might free, i.e. when a leaf
//    refers to a GC-capable object or a child is tracked (so an untracked
//    node's cells never need tracking). A node may stay tracked after it no
//    longer needs to be. Newly built nodes are untracked until their cells
//    are in place (see fatnode_finish()). Anything that may run Python code
//    (releasing a reference, which may run a finalizer) happens only after
//    the node being changed is consistent again.

#ifndef _PCOLLECTIONS__C_FAT_H
#define _PCOLLECTIONS__C_FAT_H

#include <Python.h>
#include <string.h>
#include "uintbits.h"
#include "trie.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif

// Provided by core.h: the FAT node type (for the current interpreter) whose
// cells are `cellsize` bytes, and the canonical empty FAT for a leaf size.
static PyTypeObject* pcoll_fat_nodetype(size_t cellsize);
Trie_t fat_empty(uint8_t leafsize);

#if PY_VERSION_HEX >= 0x03090000
#  define PCOLL_GC_IS_TRACKED(o) PyObject_GC_IsTracked((PyObject*)(o))
#else
#  define PCOLL_GC_IS_TRACKED(o) _PyObject_GC_IS_TRACKED((PyObject*)(o))
#endif


//=============================================================================
// Leaves.

// The number of PyObject* references at the start of a leaf of the given
// size. The supported layouts are:
//  - sizeof(PyObject*):      a single object (plist/tlist elements);
//  - 2 * sizeof(void*):      {PyObject* key; trieint_t next;} (pset/tset);
//  - 3 * sizeof(void*):      {PyObject* key, *val; trieint_t next;}
//                            (pdict/tdict).
// A reference may be NULL (a deleted dict/set entry).
static inline int fatleaf_nrefs(uint8_t leafsize) {
   return (leafsize == sizeof(PyObject*)
           ? 1
           : (int)(leafsize / sizeof(void*)) - 1);
}

// Whether a leaf refers to anything the garbage collector tracks, or might
// track later.
static inline bool fatleaf_has_gc_refs(uint8_t leafsize, void* leaf) {
   PyObject** refs = (PyObject**)leaf;
   int n = fatleaf_nrefs(leafsize), i;
   for (i = 0; i < n; ++i)
      if (refs[i] && PyObject_IS_GC(refs[i]))
         return true;
   return false;
}


//=============================================================================
// Construction.

// Allocates a FAT node of type `tp` (NULL to look up the type for the cell
// size) with the given header values and no occupied cells. The node is not
// yet tracked by the garbage collector; see fatnode_finish().
static inline Trie_t fatnode_new_as(PyTypeObject* tp,
                                    trieint_t prefix,
                                    uint8_t leafsize,
                                    uint8_t depth,
                                    bool is_transient) {
   size_t cellsize = (depth == FAT_MAX_DEPTH? leafsize : sizeof(void*));
   Trie_t a;
   if (!tp) tp = pcoll_fat_nodetype(cellsize);
   a = (Trie_t)PyObject_GC_New(PyObject, tp);
   if (!a)
      // The trie operations have no error path; running out of memory
      // while building a node is fatal.
      Py_FatalError("pcollections: out of memory allocating a trie node");
   memset(&a->header, 0,
          sizeof(struct TrieHeader) + (size_t)FAT_CELLS * cellsize);
   a->header.prefix = prefix;
   a->header.leafsize = leafsize;
   a->header.depth = depth;
   a->header.flags = (is_transient? TRIE_FLAG_ISTRANSIENT : 0);
   return a;
}
static inline Trie_t fatnode_new(trieint_t prefix,
                                 uint8_t leafsize,
                                 uint8_t depth,
                                 bool is_transient) {
   return fatnode_new_as(NULL, prefix, leafsize, depth, is_transient);
}

// Whether node `t` needs to be tracked by the garbage collector.
static inline bool fatnode_wants_tracking(Trie_t t) {
   triebits_t bi;
   if (fatnode_is_twig(t)) {
      uint8_t ls = t->header.leafsize;
      for (bi = trienode_first_bitindex(t); bi < FAT_CELLS;
           bi = trienode_next_bitindex(t, bi))
         if (fatleaf_has_gc_refs(ls, trienode_leaf(t, bi)))
            return true;
   } else {
      for (bi = trienode_first_bitindex(t); bi < FAT_CELLS;
           bi = trienode_next_bitindex(t, bi))
         if (PCOLL_GC_IS_TRACKED(trienode_subt(t, bi)))
            return true;
   }
   return false;
}

// Called once a newly built node's cells and references are all in place:
// starts tracking it if it needs to be tracked.
static inline Trie_t fatnode_finish(Trie_t t) {
   if (!PCOLL_GC_IS_TRACKED(t) && fatnode_wants_tracking(t))
      PyObject_GC_Track((PyObject*)t);
   return t;
}

// Whether cell contents `val` (a leaf pointer for a twig, a child node for a
// branch) of node `t` require `t` to be tracked.
static inline bool fatnode_cell_wants_tracking(Trie_t t, void* val) {
   if (fatnode_is_twig(t))
      return fatleaf_has_gc_refs(t->header.leafsize, val);
   else
      return PCOLL_GC_IS_TRACKED((Trie_t)val);
}

// Called after a cell of an existing (transient) node is set to `val`:
// starts tracking the node if the new contents require it.
static inline void fatnode_track_for(Trie_t t, void* val) {
   if (!PCOLL_GC_IS_TRACKED(t) && fatnode_cell_wants_tracking(t, val))
      PyObject_GC_Track((PyObject*)t);
}

// Makes a twig holding the single leaf `val` at `key`.
static inline Trie_t fat_1leaf(trieint_t key, void* val,
                               uint8_t leafsize,
                               bool is_transient,
                               void (*leaf_incref)(void*)) {
   triebits_t bi = (triebits_t)fatdepth_bitindex(FAT_MAX_DEPTH, key);
   Trie_t h = fatnode_new(fatdepth_prefix(FAT_MAX_DEPTH, key), leafsize,
                          FAT_MAX_DEPTH, is_transient);
   trienode_set_leaf(h, bi, val);
   h->header.bits = (TRIEBITS_1 << bi);
   if (leaf_incref)
      (*leaf_incref)(val);
   return fatnode_finish(h);
}

// Wraps `node` (whose reference is consumed) in single-child branches for
// every depth from its own depth - 1 up to `target_depth`, returning the
// node at `target_depth` (or `node` itself if it is already there). This is
// how new subtrees are attached without skipping depths.
static inline Trie_t fat_wrap_chain(Trie_t node, uint8_t target_depth,
                                    bool is_transient) {
   trieint_t prefix = trienode_prefix(node);
   uint8_t leafsize = node->header.leafsize;
   PyTypeObject* branch_tp = NULL;
   int d;
   for (d = (int)node->header.depth - 1; d >= (int)target_depth; --d) {
      triebits_t bi = (triebits_t)fatdepth_bitindex((uint8_t)d, prefix);
      Trie_t parent = fatnode_new_as(branch_tp,
                                     fatdepth_prefix((uint8_t)d, prefix),
                                     leafsize, (uint8_t)d, is_transient);
      branch_tp = Py_TYPE((PyObject*)parent);
      trienode_set_subt(parent, bi, node);
      parent->header.bits = (TRIEBITS_1 << bi);
      node = fatnode_finish(parent);
   }
   return node;
}

// Makes a subtree rooted at `start_depth` (<= FAT_MAX_DEPTH) holding the
// single leaf `val` at `key`, for insertion under an existing branch.
static inline Trie_t fat_1leaf_at(uint8_t start_depth, trieint_t key,
                                  void* val, uint8_t leafsize,
                                  bool is_transient,
                                  void (*leaf_incref)(void*)) {
   Trie_t twig = fat_1leaf(key, val, leafsize, is_transient, leaf_incref);
   return fat_wrap_chain(twig, start_depth, is_transient);
}

// Increments the references held by every occupied cell of `a`: its
// children, or (if leaf_incref is given) its leaves.
static inline void fat_cells_incref(Trie_t a, void (*leaf_incref)(void*)) {
   triebits_t bi;
   if (!fatnode_is_twig(a)) {
      for (bi = trienode_first_bitindex(a); bi < FAT_CELLS;
           bi = trienode_next_bitindex(a, bi))
         trienode_incref(trienode_subt(a, bi));
   } else if (leaf_incref) {
      for (bi = trienode_first_bitindex(a); bi < FAT_CELLS;
           bi = trienode_next_bitindex(a, bi))
         (*leaf_incref)(trienode_leaf(a, bi));
   }
}


//=============================================================================
// Copying primitives (the input is never modified).

// Returns a copy of `a` with cell `bi` set to `val`.
static inline Trie_t fat_and(Trie_t a,
                             triebits_t bi,
                             void* val,
                             void (*leaf_incref)(void*)) {
   Trie_t new_a;
   bool is_twig = fatnode_is_twig(a);
   size_t cellsize = fatnode_cellsize(a);
   triebits_t sbi;
   // A copy needs tracking if `a` did (keeping it tracked is harmless even
   // if the replaced cell was the only reason) or if the new cell does; the
   // other cells are `a`'s, and an untracked `a` has none that need it.
   bool track = PCOLL_GC_IS_TRACKED(a);
   new_a = fatnode_new_as(Py_TYPE((PyObject*)a), a->header.prefix,
                          a->header.leafsize, a->header.depth,
                          trie_is_transient(a));
   memcpy(new_a->cells, a->cells, (size_t)FAT_CELLS * cellsize);
   new_a->header.bits = a->header.bits | (TRIEBITS_1 << bi);
   if (is_twig) {
      trienode_set_leaf(new_a, bi, val);
      if (leaf_incref)
         for (sbi = trienode_first_bitindex(new_a); sbi < FAT_CELLS;
              sbi = trienode_next_bitindex(new_a, sbi))
            (*leaf_incref)(trienode_leaf(new_a, sbi));
   } else {
      // The new child's reference is transferred in; the other children
      // are now shared with `a`.
      trienode_set_subt(new_a, bi, (Trie_t)val);
      for (sbi = trienode_first_bitindex(new_a); sbi < FAT_CELLS;
           sbi = trienode_next_bitindex(new_a, sbi))
         if (sbi != bi)
            trienode_incref(trienode_subt(new_a, sbi));
   }
   if (track || fatnode_cell_wants_tracking(new_a, val))
      PyObject_GC_Track((PyObject*)new_a);
   return new_a;
}

// Returns a copy of `a` with cell `bi` cleared, or `a` itself (with no new
// reference) if that cell is already empty.
static inline Trie_t fat_but(Trie_t a,
                             triebits_t bi,
                             void (*leaf_incref)(void*)) {
   Trie_t new_a;
   size_t cellsize = fatnode_cellsize(a);
   if (!(a->header.bits & (TRIEBITS_1 << bi)))
      return a;
   new_a = fatnode_new_as(Py_TYPE((PyObject*)a), a->header.prefix,
                          a->header.leafsize, a->header.depth,
                          trie_is_transient(a));
   memcpy(new_a->cells, a->cells, (size_t)FAT_CELLS * cellsize);
   memset(new_a->cells + (size_t)bi * cellsize, 0, cellsize);
   new_a->header.bits = a->header.bits & ~(TRIEBITS_1 << bi);
   fat_cells_incref(new_a, leaf_incref);
   // As in fat_and(): the copy's cells are a subset of `a`'s.
   if (PCOLL_GC_IS_TRACKED(a))
      PyObject_GC_Track((PyObject*)new_a);
   return new_a;
}

// The shallowest depth at which the keys (or prefixes) `pa` and `pb` differ.
static inline uint8_t fat_divergedepth(trieint_t pa, trieint_t pb) {
   uint8_t d;
   for (d = 0; d < FAT_MAX_DEPTH; ++d)
      if (fatdepth_bitindex(d, pa) != fatdepth_bitindex(d, pb))
         return d;
   return FAT_MAX_DEPTH;
}

// Joins two subtrees whose prefixes diverge under a new parent at the depth
// where they diverge. `a`'s reference is consumed; `b` gains a reference.
static inline Trie_t fat_subjoin(Trie_t a, Trie_t b, bool is_transient) {
   trieint_t pa = trienode_prefix(a);
   trieint_t pb = trienode_prefix(b);
   uint8_t depth = fat_divergedepth(pa, pb);
   triebits_t aii = (triebits_t)fatdepth_bitindex(depth, pa);
   triebits_t bii = (triebits_t)fatdepth_bitindex(depth, pb);
   uint8_t leafsize = a->header.leafsize;
   Trie_t parent, achain, bchain;
   trienode_incref(b);
   achain = fat_wrap_chain(a, (uint8_t)(depth + 1), is_transient);
   bchain = fat_wrap_chain(b, (uint8_t)(depth + 1), is_transient);
   parent = fatnode_new(fatdepth_prefix(depth, pa), leafsize, depth,
                        is_transient);
   trienode_set_subt(parent, aii, achain);
   trienode_set_subt(parent, bii, bchain);
   parent->header.bits = (TRIEBITS_1 << aii) | (TRIEBITS_1 << bii);
   return fatnode_finish(parent);
}


//=============================================================================
// In-place primitives for transient trees.
// fatnode_set()/fatnode_del() change a transient node in place and return
// it. Given a persistent node, they instead return a changed, transient copy
// (fat_and()/fat_but()); the caller then replaces the node with the copy.

static inline Trie_t fatnode_set(Trie_t a,
                                 triebits_t bi,
                                 void* val,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   triebits_t bit = (TRIEBITS_1 << bi);
   if (!trie_is_transient(a)) {
      Trie_t new_a = fat_and(a, bi, val, leaf_incref);
      trienode_set_transient(new_a, true);
      return new_a;
   }
   if (!fatnode_is_twig(a)) {
      // The child's reference is transferred into the cell; any previous
      // child is released once the node is consistent again.
      Trie_t old = (a->header.bits & bit)? trienode_subt(a, bi) : NULL;
      trienode_set_subt(a, bi, (Trie_t)val);
      a->header.bits |= bit;
      fatnode_track_for(a, val);
      if (old)
         fatnode_decref(old, NULL);
   } else if (a->header.bits & bit) {
      // Take the new leaf's references before releasing the old leaf's: the
      // two may share referents (tdict/tset rewrite an entry by copying it
      // and changing one field, so the same key object is in both).
      char old[256];  // leafsize is a uint8_t, so this always fits.
      memcpy(old, trienode_leaf(a, bi), a->header.leafsize);
      if (leaf_incref)
         (*leaf_incref)(val);
      trienode_set_leaf(a, bi, val);
      fatnode_track_for(a, val);
      if (leaf_decref)
         (*leaf_decref)(old);
   } else {
      trienode_set_leaf(a, bi, val);
      a->header.bits |= bit;
      if (leaf_incref)
         (*leaf_incref)(val);
      fatnode_track_for(a, val);
   }
   return a;
}

static inline Trie_t fatnode_del(Trie_t a,
                                 triebits_t bi,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   triebits_t bit = (TRIEBITS_1 << bi);
   if (!trie_is_transient(a)) {
      Trie_t new_a = fat_but(a, bi, leaf_incref);
      if (new_a != a)
         trienode_set_transient(new_a, true);
      return new_a;
   }
   if (!(a->header.bits & bit))
      return a;
   if (!fatnode_is_twig(a)) {
      Trie_t old = trienode_subt(a, bi);
      a->header.bits &= ~bit;
      trienode_set_subt(a, bi, NULL);
      fatnode_decref(old, NULL);
   } else {
      char old[256];
      uint8_t ls = a->header.leafsize;
      memcpy(old, trienode_leaf(a, bi), ls);
      a->header.bits &= ~bit;
      memset(trienode_leaf(a, bi), 0, ls);
      if (leaf_decref)
         (*leaf_decref)(old);
   }
   return a;
}

// After an in-place change left the node at path level `step` tracked,
// starts tracking any untracked ancestors (a tracked node's parent must be
// tracked).
static inline void fat_track_path(TriePath_t path, int step) {
   for (; step >= 0; --step) {
      Trie_t node = path->node[step];
      Trie_t child = trienode_subt(node, path->index[step]);
      if (PCOLL_GC_IS_TRACKED(node) || !PCOLL_GC_IS_TRACKED(child))
         return;
      PyObject_GC_Track((PyObject*)node);
   }
}


//=============================================================================
// Per-item operations.

// Returns a tree equal to `a` except that `key` maps to `val`. `a` is not
// modified.
static inline Trie_t fat_anditem(Trie_t a, trieint_t key, void* val,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   if (a->header.bits == 0)
      return fat_1leaf(key, val, a->header.leafsize, trie_is_transient(a),
                       leaf_incref);
   found = triepath_fatfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         // The key lies outside this subtree: join a new twig to it.
         node = fat_subjoin(
            fat_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                      leaf_incref),
            node,
            trie_is_transient(node));
      } else if (fatnode_is_twig(node)) {
         node = fat_and(node, path.index[step], val, leaf_incref);
      } else {
         node = fat_and(
            node,
            path.index[step],
            fat_1leaf_at((uint8_t)(node->header.depth + 1), key, val,
                         a->header.leafsize, trie_is_transient(node),
                         leaf_incref),
            NULL);
      }
   } else {
      node = fat_and(node, path.index[step], val, leaf_incref);
   }
   // Copy each ancestor, pointing it at the new child.
   while (step > 0) {
      --step;
      node = fat_and(path.node[step], path.index[step], node, NULL);
   }
   return node;
}

// Returns a tree equal to `a` except that `key` is absent, or `a` itself
// (with no new reference) if `key` is not present. A subtree that becomes
// empty is removed; a tree that becomes empty is replaced by fat_empty().
static inline Trie_t fat_butitem(Trie_t a, trieint_t key,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   found = triepath_fatfind(&path, a, key);
   if (!found) return a;
   step = path.steps - 1;
   node = fat_but(path.node[step], path.index[step], leaf_incref);
   // While the new node is empty, remove it from its parent instead. (Each
   // such node is a fresh copy nothing else refers to.)
   while (step > 0 && trienode_occupancy(node) == 0) {
      Trie_t emptied = node;
      --step;
      node = fat_but(path.node[step], path.index[step], leaf_incref);
      fatnode_decref(emptied, NULL);
   }
   if (step == 0 && trienode_occupancy(node) == 0) {
      uint8_t leafsize = node->header.leafsize;
      fatnode_decref(node, NULL);
      return fat_empty(leafsize);
   }
   while (step > 0) {
      --step;
      node = fat_and(path.node[step], path.index[step], node, NULL);
   }
   return node;
}

// Changes transient tree `a` in place so that `key` maps to `val`, and
// returns the tree's root (which may be a new node). Consumes the caller's
// reference to `a` (the returned root carries it); a caller that wants to
// keep `a` itself must hold its own extra reference.
static inline Trie_t tfat_setitem(Trie_t a, trieint_t key, void* val,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node, newnode;
   TriePath path;
   int found;
   int step;
   if (a->header.bits == 0) {
      Trie_t result = fat_1leaf(key, val, a->header.leafsize, true,
                                leaf_incref);
      fatnode_decref(a, leaf_decref);
      return result;
   }
   found = triepath_fatfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         newnode = fat_subjoin(
            fat_1leaf(key, val, a->header.leafsize, true, leaf_incref),
            node,
            true);
      } else if (fatnode_is_twig(node)) {
         newnode = fatnode_set(node, path.index[step], val,
                               leaf_incref, leaf_decref);
      } else {
         newnode = fatnode_set(
            node,
            path.index[step],
            fat_1leaf_at((uint8_t)(node->header.depth + 1), key, val,
                         a->header.leafsize, true, leaf_incref),
            NULL,
            NULL);
      }
   } else {
      newnode = fatnode_set(node, path.index[step], val,
                            leaf_incref, leaf_decref);
   }
   // Point each ancestor at the new child. An ancestor that already holds
   // it is transient, as are all nodes above it, so nothing above needs to
   // change except, possibly, GC tracking.
   while (step > 0) {
      Trie_t parent;
      triebits_t bi;
      --step;
      parent = path.node[step];
      bi = path.index[step];
      if (trienode_subt(parent, bi) == newnode) {
         fat_track_path(&path, step);
         return a;
      }
      node = newnode;
      newnode = fatnode_set(parent, bi, node, NULL, NULL);
   }
   // The root was replaced: release the caller's reference to the old one.
   if (newnode != a)
      fatnode_decref(a, leaf_decref);
   return newnode;
}

// Changes transient tree `a` in place so that `key` is absent, and returns
// the tree's root. Reference handling is as for tfat_setitem().
static inline Trie_t tfat_delitem(Trie_t a, trieint_t key,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node;
   Trie_t orig_child;
   TriePath path;
   int found;
   uint8_t step;
   found = triepath_fatfind(&path, a, key);
   if (!found) return a;
   step = path.steps - 1;
   node = fatnode_del(path.node[step], path.index[step],
                      leaf_incref, leaf_decref);
   // While the node is empty, remove it from its parent instead. If it was
   // a claimed copy, nothing else refers to it, so release it; otherwise
   // clearing the parent's cell releases it.
   while (step > 0 && trienode_occupancy(node) == 0) {
      Trie_t parent, emptied;
      orig_child = path.node[step];
      --step;
      parent = path.node[step];
      emptied = node;
      node = fatnode_del(parent, path.index[step], leaf_incref, leaf_decref);
      if (emptied != orig_child)
         fatnode_decref(emptied, leaf_decref);
   }
   if (step == 0 && trienode_occupancy(node) == 0) {
      uint8_t leafsize = node->header.leafsize;
      fatnode_decref(node, leaf_decref);
      if (node != a)
         fatnode_decref(a, leaf_decref);
      return fat_empty(leafsize);
   }
   while (step > 0) {
      Trie_t parent;
      triebits_t bi;
      --step;
      parent = path.node[step];
      bi = path.index[step];
      if (trienode_subt(parent, bi) == node)
         return a;
      node = fatnode_set(parent, bi, node, NULL, NULL);
   }
   if (node != a)
      fatnode_decref(a, leaf_decref);
   return node;
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_FAT_H
