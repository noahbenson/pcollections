///////////////////////////////////////////////////////////////////////////////
// _C/amt.h
// AMT-specific node construction, mutation, and per-item update operations.
//
// All AMT nodes are represented using the shared Trie_t/struct TrieData type
// defined in trie.h; there is no separate AMT-specific struct or pointer type
// here. The low-level, transience-agnostic structural helpers (shift/mask
// arithmetic, bit/cellindex helpers, refcounting, free, lookup, and path
// iteration) all live in trie.h as trienode_*/amtnode_*/amt_* functions. This
// file only contains what's genuinely AMT-specific: node construction, the
// copy-and-modify and mutate-in-place update primitives, and the per-item
// (whole-tree) update operations built on top of them.
//
// Naming conventions used below (per the project's overall convention):
//  - amt_*      : operates on an AMT node/tree without caring whether it (or
//                 the nodes it touches) is persistent or transient. This
//                 includes the non-mutating "and"/"but" family: they always
//                 either return their input completely unchanged, or a newly
//                 allocated node whose transient bit matches the input's.
//                 Since the caller decides whether to flip the returned
//                 node's transient bit (see amtnode_set/amtnode_del below),
//                 these primitives themselves never need to know or care.
//  - tamt_*/pamt_* : operate specifically on transient/persistent nodes.
//                 tamt_setitem()/tamt_delitem() *mutate* an already-transient
//                 trie in place (claiming any not-yet-owned persistent
//                 subtrees they descend through by copying and flipping the
//                 transient bit on the copy). There is no separate pamt_*
//                 item-level entry point: amt_anditem()/amt_butitem() already
//                 produce a wholly new, untouched-input persistent result
//                 when called on a fully persistent trie, so they serve as
//                 the persistent API directly.


//=============================================================================
// Initialization.

#ifndef _PCOLLECTIONS__C_AMT_H
#define _PCOLLECTIONS__C_AMT_H


#include <Python.h>
#include <string.h>
#include "uintbits.h"
// trie.h provides pcoll_atomic_u64_t/PCOLL_ATOMIC_U64_FETCH_*1 (a portable
// stand-in for <stdatomic.h>, which MSVC doesn't have at all without
// /std:c11 -- see trie.h's shim comment); this file doesn't call any
// atomic_* function directly, so it doesn't need its own <stdatomic.h>
// include (and, on Windows, must not have one).
#include "trie.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


//=============================================================================
// Node Constructors.

// The amtnode_alloc() function just allocates space for the AMT node; as such,
// it only needs to know the leafsize and the number of cells. All data is
// returned uninitialized.
static inline Trie_t amtnode_alloc(uint8_t cellsize, uint8_t ncells) {
   return (Trie_t)malloc(sizeof(struct TrieData) + (size_t)ncells*cellsize);
}

// amtnode_init() initializes the header of an AMT node that has been
// allocated by amtnode_alloc(), or potentially memory that has been written
// into another struct.
// This function sets in the AMT node's header:
//  - refcount (set to 1)
//  - prefix (stored exactly as given -- no flag bits are packed into it;
//    see "The trieint_t is_transient flag" comment in trie.h)
//  - depth
//  - flags (just the is_transient flag; the node's real bit-shift amount is
//    no longer stored at all -- see amtnode_shift() in trie.h, which
//    recomputes it from `depth` on the rare occasions it's needed)
//  - ncells (a real, ordinary cell count -- there is no is_fat flag
//    anywhere; see that same trie.h comment)
//  - leafsize
// It does not set the bits or the cells, which remain uninitialized.
static inline void amtnode_init(Trie_t a,
                                trieint_t prefix,
                                uint8_t leafsize,
                                uint8_t depth,
                                uint8_t ncells,
                                bool is_transient) {
   uint8_t maxcells = amtdepth_maxcells(depth);
   ncells = (ncells > maxcells? maxcells : ncells);
   // The refcount should always start at 1. (We can set directly because no
   // other thread can legally access this memory yet.)
   a->header.refcount = 1;
   // `prefix` is stored exactly as given now -- is_transient lives
   // elsewhere (see trie.h); there is nothing to mask or OR in here.
   a->header.prefix = prefix;
   a->header.leafsize = leafsize;
   a->header.ncells = ncells;
   a->header.depth = depth;
   a->header.flags = (is_transient? TRIE_FLAG_ISTRANSIENT : 0);
}

// amtnode_new() creates a new AMT object and returns it partially initialized.
// If given a leafsize of 0, this is interpreted as being for a Python object
// in the twigs (with refcounting). Nodes whose depths aren't AMT_MAX_DEPTH
// (i.e. twigs) are always refcounted during updates. All sizes but 0 treat
// the leaves as untyped bytes. The max leaf size is 255 bytes (max of uint8).
// The returned AMT node has the following data set:
//  - refcount (set to 1)
//  - prefix
//  - depth
//  - shift
//  - ncells
//  - leafsize
// It does not set the bits or the cells, which are uninitialized.
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

// amt_1leaf: Make a new twig node that contains a single pair (key / value).
// Increments the refcount for the value as well (if leaf_incref is given).
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

// amt_cells_incref() increments the references to all the cells in the given
// amt node: child subtrees for a branch node, or leaf values (if leaf_incref
// is given) for a twig node.
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
// Non-mutating (copy-and-modify) node primitives.
// amt_and()/amt_but() never touch their input node: they either return it
// completely unchanged (when the requested change is a no-op) or allocate a
// brand new node that is a copy of the input plus the one changed cell/bit.
// A newly allocated node's transient bit is always inherited unchanged from
// the input node; the caller is free to flip it (with trienode_set_transient)
// on a freshly-allocated result, since such a result is guaranteed to be
// exclusively owned by the caller at that point. Because of this, neither
// function needs to know or care whether it's being used in a persistent or
// a transient context, hence the plain "amt_" prefix.

// amt_and() creates a new AMT node that is identical to the given AMT node
// except for having the cell at bitindex `bi` set to `val` (allocating the
// bit if it wasn't already set). The leaf_incref function is for
// incrementing the reference count of a leaf value (ignored for branch
// nodes, where the child subtree's own refcount is incremented instead). If
// it's null, no leaf reference counting is tracked. The returned node's
// child/leaf references (including the new one) are all correctly
// reference-counted; the input node `a` is left untouched.
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
   // Incref every sibling cell copied from `a` (everything except the cell
   // we just wrote at `ci`) since those siblings are now referenced by both
   // `a` (untouched) and new_a. The just-written cell at `ci` is handled
   // differently by kind: a branch child (a subtree pointer) always already
   // carries, by this file's convention, exactly the one unit of ownership
   // meant for this new cell -- freshly built via amt_1leaf()/amt_subjoin(),
   // or itself the result of a deeper amt_and()/amtnode_set() call during
   // path propagation -- so incref'ing it again here would leak it, never
   // letting its refcount reach 0. A twig's leaf value follows a different,
   // pre-existing convention: leaf_incref (when given) is called for every
   // occupied cell including `ci`, since callers pass a real leaf_incref
   // when they want the newly-written value's ownership transferred in from
   // a borrowed reference, and NULL when they've already accounted for it
   // (e.g. amt_anditem() passing NULL when the value came from amt_1leaf(),
   // which already increfed it once via its own leaf_incref argument).
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

// amt_but() creates a new AMT node that is identical to the given AMT node
// except for having the cell at bitindex `bi` unset. If the bit wasn't set to
// begin with, this is a no-op and the input node `a` is returned completely
// unchanged (no allocation, no incref). Otherwise, behaves like amt_and():
// the input is left untouched, and the returned node is a correctly
// reference-counted new node whose transient bit matches the input's.
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
   if (!(bits & (TRIEBITS_1 << bi))) {
      // The bit isn't set, so deleting it is a no-op: return the input
      // completely unchanged.
      return a;
   }
   // We're deleting an existing bit, so we need one fewer cell than before.
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

// amt_subjoin(): join two sub-AMTs (which must not already share a common
// parent) under a new parent AMT node and return it. Takes ownership of `a`
// without incref'ing it -- matching this file's convention (see amt_and()'s
// comment) that `a` is expected to be a freshly built, exclusively-owned
// node (in both current call sites, the result of amt_1leaf()) whose one
// unit of ownership transfers directly into the new parent's cell. `b`, by
// contrast, is expected to still be referenced elsewhere (e.g. by whatever
// `a` diverged from along an existing path), so it gets an explicit incref
// for its new reference from the parent. The new parent's transient bit is
// set from is_transient directly (there's no existing node to inherit it
// from, since this constructs a brand new branch point above two
// previously-unrelated subtrees).
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
// Mutating (mutate-in-place-or-claim) node primitives.
// amtnode_set()/amtnode_del() are used specifically by the tamt_* item-level
// operations below while walking down an already-transient trie. Called on a
// node that's already transient (and therefore, per the persistent/transient
// invariant, uniquely owned by this transient trie), they mutate that node's
// cells directly in place and return it unchanged. Called on a node that's
// still persistent (an untouched, possibly-shared subtree the transient
// trie hasn't claimed yet), they instead go through amt_and()/amt_but() to
// get a private copy and then explicitly flip that copy's transient bit
// before returning it -- this is the "claiming" step. Either way, the
// returned node is safe for the transient caller to mutate further in place
// without going through this claiming step again.

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
      // a is a not-yet-claimed persistent subtree: get a private, transient
      // copy first (amt_and() always allocates a fresh copy here, since it's
      // impossible for the "no-op" shortcut to apply -- we're always about
      // to write val into the cell one way or another).
      triebits_t ncells = nextbinpow_triebits(nocc + 1);
      Trie_t new_a = amt_and(a, bi, val, leaf_incref, ncells);
      trienode_set_transient(new_a, true);
      return new_a;
   }
   // a is already transient and therefore uniquely owned: mutate in place.
   if (bits & (TRIEBITS_1 << bi)) {
      // We're overwriting an existing cell.
      if (!is_twig) {
         amtnode_decref(trienode_subt(a, ci), leaf_decref);
         trienode_set_subt(a, ci, (Trie_t)val);
      } else {
      // Overwrite a twig leaf. The new leaf's references are taken *before*
      // the old leaf's are released, and the old leaf is released only after
      // the cell already holds the new one: the two leaves may share
      // referents (tdict/tset rewrite an entry by copying it and changing one
      // field, so the key object is in both), and releasing first can free an
      // object the new leaf still needs. Releasing last also means any
      // finalizer the release triggers sees a consistent node.
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
      // We're adding a new cell.
      if (a->header.ncells > nocc) {
         // There's already room allocated for it.
         memmove(
            a->cells + cellsize*(ci + 1),
            a->cells + cellsize*ci,
            cellsize*(nocc - ci));
      } else {
         // No room: reallocate (as a claimed transient copy) with the next
         // power-of-2 capacity above what we have now.
         triebits_t ncells = nextbinpow_triebits(nocc + 1);
         Trie_t new_a = amt_and(a, bi, val, leaf_incref, ncells);
         trienode_set_transient(new_a, true);
         // amt_and() already did all the remaining work (setting the cell,
         // increffing siblings), so we can hand the caller the new node
         // directly. Deliberately NOT decreffing the old, now-replaced `a`
         // here: `a`'s single existing reference is owned by whatever cell
         // (or top-level variable) currently points to it, and that owner
         // hasn't been updated to point to new_a yet -- it still holds `a`.
         // Retiring `a` here, before that update happens, would drop its
         // refcount out from under its still-valid reference (this was a
         // real bug: tamt_setitem()'s propagate-up loop independently
         // decrefs whatever the parent cell *currently* holds when it
         // overwrites that cell with new_a, so decreffing `a` here too was a
         // double decref -- a use-after-free the moment `a`'s refcount was
         // exactly 1, caught via AddressSanitizer). The caller (either the
         // next level up during propagate-up, or tamt_setitem() itself when
         // `a` is the tree root with no parent cell) is responsible for
         // retiring `a`'s one reference exactly once, at the point it
         // actually stops pointing to `a` and starts pointing to new_a.
         return new_a;
      }
      a->header.bits = bits | (TRIEBITS_1 << bi);
   }
   // Set the new value in place. As in amt_and(), a branch child pointer is
   // never incref'ed here: by this file's convention it already carries
   // exactly the one unit of ownership meant for this cell (freshly built,
   // or itself the result of a deeper amtnode_set()/amtnode_del() call
   // during path propagation), so incref'ing it again would leak it. A
   // twig's leaf value follows the separate, controllable leaf_incref
   // convention instead.
   if (is_twig) {
      trienode_set_leaf(a, ci, val);
      if (leaf_incref)
         (*leaf_incref)(val);
   } else {
      trienode_set_subt(a, ci, (Trie_t)val);
   }
   return a;
}

// amtnode_del(): the mutate-in-place-or-claim counterpart to amtnode_set(),
// used by tamt_delitem(). See amtnode_set()'s comment for the general shape.
static inline Trie_t amtnode_del(Trie_t a,
                                 triebits_t bi,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   triebits_t
      bit = (TRIEBITS_1 << bi),
      nocc = trienode_occupancy(a),
      ci = amtnode_bit2cellindex(a, bi);
   if (!trie_is_transient(a)) {
      // a is not-yet-claimed and persistent. amt_but() may itself decide
      // this is a no-op (bit not set) and hand back `a` unchanged -- in that
      // case there is nothing to claim, since nothing was allocated.
      Trie_t new_a = amt_but(a, bi, leaf_incref, nextbinpow_triebits(nocc));
      if (new_a != a)
         trienode_set_transient(new_a, true);
      return new_a;
   }
   // a is already transient and therefore uniquely owned: mutate in place.
   if (!(a->header.bits & bit)) {
      // The bit isn't set, so there's nothing to do.
      return a;
   }
   // The bit is set: deref the previous value, unset the bit, and shift the
   // remaining cells down to close the gap. Every cell index is derived
   // on the fly from popcount(bits & ltmask(bitindex)) (see
   // amtnode_bit2cellindex()), which assumes the cells array is always
   // densely packed in bit order with no holes -- so unlike amtnode_set()'s
   // "add a new cell" branch (which shifts cells *up* to open a gap before
   // writing), removal must shift the following cells *down* to close one,
   // or every subsequent cell's real position stops matching what
   // popcount-based lookup expects it to be.
   {
      size_t cellsize = amtnode_cellsize(a);
      if (!amtnode_is_twig(a))
         amtnode_decref(trienode_subt(a, ci), leaf_decref);
      else if (leaf_decref)
         (*leaf_decref)(trienode_leaf(a, ci));
      memmove(
         a->cells + ci*cellsize,
         a->cells + (ci + 1)*cellsize,
         (nocc - ci - 1)*cellsize);
      a->header.bits &= ~bit;
   }
   return a;
}


//=============================================================================
// AMT API Functions (whole-tree, per-item operations).

// The amt_empty() function can be used to get the canonical empty PAMT for a
// particular leafsize. There is no separate canonical empty transient AMT;
// you always start from the canonical empty persistent one and call
// tamt_setitem() on it (or an equivalent "make transient" step) to begin
// building a transient one.
// Structural invariant maintained by every function below: a node has 0
// children if and only if it is this canonical empty node -- never any
// other node, persistent or transient -- and a node has exactly 1 child
// only if it is a twig (a single key/value pair); a branch (non-twig) node
// always has either 0 children (impossible -- see above) or at least 2. In
// other words, every AMT is always its minimal possible tree: no dangling
// empty nodes, and no branch node that exists only to point at a single
// child. amt_butitem()/tamt_delitem() are responsible for maintaining this
// on every removal (collapsing a branch down to its lone surviving child,
// and substituting this shared node whenever a tree drains to nothing) --
// see amt_collapse()'s comment.
// Ownership contract: like every other function here that returns a
// Trie_t, amt_empty() hands back a reference the caller owns and must
// eventually decref exactly once (whether or not this call is the first
// one ever to return this particular leafsize's singleton) -- callers never
// need to special-case "this came from amt_empty()" when it comes to
// refcounting. The singleton itself is expected to never actually reach a
// refcount of 0 in practice, since amt_empty()'s own permanent hold on it
// keeps it alive independent of any transient reference some caller hands
// back later.
Trie_t amt_empty(uint8_t leafsize);

// amt_collapse(): if `node` is a branch (non-twig) with exactly one
// remaining child, that violates the minimal-tree invariant above -- a
// branch must always have at least two children -- so collapse it away and
// return that lone child directly in its place; a twig with one leaf, or a
// branch with two or more children, is returned completely unchanged (both
// are already valid as-is).
// `orig` is whatever node `node` is meant to replace at this level (i.e.
// path.node[step] at the point of the call) -- it decides how the
// now-collapsed-away wrapper gets retired. amt_but()'s persistent-copy path
// always hands back a brand new, nobody-else-references-it-yet node, so
// `node != orig` there and it's safe (and necessary) to decref the wrapper
// immediately. But amtnode_del()'s already-transient, mutate-in-place path
// hands back the SAME pointer as `orig` -- meaning `orig`'s own parent cell
// still physically holds that exact pointer, not yet overwritten -- so
// decreffing it here, before that cell is ever updated, would free a node
// that's still live in the tree (a real bug: this is exactly what happened
// before this comment was added, caught via AddressSanitizer as a
// use-after-free a couple of tamt_delitem() calls later). In that case the
// wrapper's retirement is left to whichever *existing* mechanism already
// retires "the old value that used to occupy this slot" when `node` (now
// the child) gets attached in its place -- amtnode_set()'s overwrite-decref
// for a non-root level, or the explicit root decref at the bottom of
// tamt_setitem()/tamt_delitem() -- exactly as it would for any other
// replaced value, collapsed or not.
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

// amt_anditem(): return a new tree identical to `a` except that `key` maps to
// `val`. Does not modify `a` (or anything reachable from it) in any way.
// Works regardless of whether `a` (or any node along the affected path) is
// persistent or transient: every newly allocated node along the path
// inherits its transient bit from the node it replaces (see amt_and()'s
// comment), so calling this on a fully persistent tree produces a fully
// persistent result, sharing every untouched sibling subtree with `a`.
static inline Trie_t amt_anditem(Trie_t a, trieint_t key, void* val,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   // If a is empty, just return a new twig.
   if (a->header.bits == 0)
      return amt_1leaf(key, val, a->header.leafsize, trie_is_transient(a),
                       leaf_incref);
   // If we find the key, we're ready to start allocating at the leaf
   // (steps-1) node; otherwise, we need to worry about is_beneath.
   found = triepath_amtfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         // The path diverges here -- we need a new leaf-pair and to connect
         // it to the highest subtree we did share a prefix with.
         node = amt_subjoin(
            amt_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                     leaf_incref),
            node,
            trie_is_transient(node));
      } else if (amtnode_is_twig(node)) {
         // The current node is a twig, and we need to add a leaf.
         node = amt_and(node, path.index[step], val, leaf_incref, 1);
      } else {
         // The current node needs a new child, which is a 1-leaf twig.
         node = amt_and(
            node,
            path.index[step],
            amt_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                     leaf_incref),
            NULL,
            2);
      }
   } else {
      // The item was found: overwrite that cell.
      node = amt_and(node, path.index[step], val, leaf_incref, 1);
   }
   // We now need to build a new path by backtracking up the steps, wrapping
   // each parent in a fresh copy that points at the updated child.
   while (step > 0) {
      --step;
      node = amt_and(path.node[step], path.index[step], node, NULL, 1);
   }
   return node;
}

// amt_butitem(): return a new tree identical to `a` except that `key` (and
// its associated value) is absent. If `key` isn't present in `a` to begin
// with, this is a no-op and `a` is returned completely unchanged. Like
// amt_anditem(), this does not modify `a`, and works regardless of whether
// `a` is persistent or transient. Maintains the minimal-tree invariant
// documented above amt_empty(): a branch node left with exactly one child
// is collapsed into that child (see amt_collapse()), and a tree that drains
// to nothing comes back as the canonical amt_empty() node rather than a
// one-off empty node of its own.
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
   // As long as the node we just produced is completely empty, it must be
   // removed from ITS parent's cells entirely rather than reattached, so we
   // keep walking upward deleting bits (instead of reattaching pointers)
   // for as long as each level empties out in turn. Each amt_but() call here
   // always allocates a fresh, empty, exclusively-owned (refcount 1) node --
   // amt_but() never returns its input unchanged when we get here, since the
   // bit we're removing is known to be set (found == true) -- and that node
   // is never attached anywhere or referenced again once we've checked its
   // occupancy, so it must be explicitly decreffed here or it leaks (the
   // same leak as tamt_delitem()'s analogous loop guards against for its
   // claimed-empty copies).
   while (step > 0 && trienode_occupancy(node) == 0) {
      Trie_t emptied = node;
      --step;
      node = amt_but(path.node[step], path.index[step], leaf_incref, 1);
      amtnode_decref(emptied, NULL);
   }
   // The empty-cascade above has just stopped (either because it reached
   // the root, or because some ancestor's occupancy came out nonzero after
   // its one bit was cleared) -- this is the only point in the whole walk
   // where a branch node's occupancy could have just become exactly 1
   // (every level processed above this one is a plain value-overwrite,
   // which never changes an ancestor's own occupancy), so this is the only
   // place a collapse can ever be needed.
   node = amt_collapse(node, path.node[step], NULL);
   if (step == 0 && trienode_occupancy(node) == 0) {
      // The whole tree drained to nothing: swap this one-off empty result
      // for the shared canonical one instead of returning a node that no
      // other empty AMT of this leafsize will ever be pointer-equal to.
      uint8_t leafsize = node->header.leafsize;
      amtnode_decref(node, NULL);
      return amt_empty(leafsize);
   }
   // From here up (if any steps remain), reattach the surviving node
   // pointer normally.
   while (step > 0) {
      --step;
      node = amt_and(path.node[step], path.index[step], node, NULL, 1);
   }
   return node;
}

// tamt_setitem(): mutate the transient tree `a` in place so that `key` maps
// to `val`, claiming (copying and marking transient) any not-yet-owned
// persistent subtrees encountered along the way. Returns the (possibly new)
// root of the tree -- callers must always use the returned value, since the
// root itself may need to be replaced if it wasn't already transient.
static inline Trie_t tamt_setitem(Trie_t a, trieint_t key, void* val,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node, newnode;
   TriePath path;
   int found;
   uint8_t step;
   // If a is empty, there's nothing to claim/mutate -- just hand back a new
   // (transient) twig. As with the general case at the end of this function,
   // tamt_setitem() always consumes exactly one reference to its top-level
   // `a` argument -- whether `a` was already transient (uniquely owned) or
   // persistent (e.g. the canonical shared empty node, in which case this
   // retires only *this caller's* reference to it, same as claiming any
   // other shared persistent node; a caller that still needs its own
   // separate reference to `a` afterward must incref() it before passing it
   // in here, exactly as one would before sharing any other persistent
   // node with a new owner) -- so the old `a` is always decreffed here once
   // replaced.
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
   // We now need to propagate the (possibly new) child pointer back up the
   // path, claiming/mutating each parent in turn. If a parent already holds
   // this exact child pointer (the common case once a subtree has already
   // been claimed and is being mutated in place across repeated operations),
   // there's nothing to do at this level -- and, since that can only happen
   // when the parent is already transient too (a transient node can never
   // be the parent of an untouched persistent child that we just mutated in
   // place), nothing above needs updating either, so we stop immediately.
   // (Calling amtnode_set() here anyway would be actively wrong, not just
   // redundant: it would decref the "old" value at that cell -- which is
   // this same, still-live object -- and could free it out from under us
   // before the immediately following incref/store.)
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, amtnode_bit2cellindex(parent, bi)) == newnode)
         return a;
      node = newnode;
      newnode = amtnode_set(parent, bi, node, NULL, NULL);
   }
   // `newnode` is now the (possibly new) root. Every intermediate level's
   // replacement was already retired by the propagate-up loop's own
   // overwrite-decref above (whether the old value there was a transient
   // node being reallocated, or a persistent node being claimed for the
   // first time -- see amtnode_set()'s comment; either way that decref
   // correctly retires exactly the one reference that level's parent cell
   // held). The one reference this loop can never retire on its own is the
   // root's: there's no parent cell above it to trigger that
   // overwrite-decref. So if the root itself ended up replaced (whether by
   // reallocation, or by claiming `a` itself from persistent), that one
   // reference -- the one held by whatever variable the caller passed in as
   // `a` -- needs retiring here, exactly once, mirroring what would have
   // happened one level further up if the root had a parent. (This is why
   // test code that wants to keep its own separate reference to the
   // original `a` around after calling tamt_setitem() must incref() it
   // first, the same as sharing any other persistent node with a new
   // owner.)
   if (newnode != a)
      amtnode_decref(a, leaf_decref);
   return newnode;
}

// tamt_delitem(): mutate the transient tree `a` in place so that `key` (and
// its associated value) is absent, claiming any not-yet-owned persistent
// subtrees encountered along the way. Returns the (possibly new) root --
// callers must always use the returned value. Maintains the same
// minimal-tree invariant as amt_butitem() -- a branch left with exactly one
// child collapses into it, and draining a tree to nothing always converges
// on the canonical amt_empty() node, whether that draining happened via
// this function or amt_butitem().
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
   // As long as the node we just produced is completely empty, remove it
   // from its parent's cells entirely (instead of reattaching it) and keep
   // walking upward. Note that amtnode_del(parent, ...) decrefs whatever
   // parent's cell *currently* holds -- which is the pre-claim child if a
   // claim/copy happened one level down, not the (different, newly
   // allocated) empty node we just produced -- so if a claim did happen, the
   // now-orphaned empty copy has nothing else referencing it and must be
   // decreffed here explicitly, or it would simply leak.
   while (step > 0 && trienode_occupancy(node) == 0) {
      orig_child = path.node[step];
      --step;
      Trie_t parent = path.node[step];
      Trie_t emptied = node;
      node = amtnode_del(parent, path.index[step], leaf_incref, leaf_decref);
      if (emptied != orig_child)
         amtnode_decref(emptied, leaf_decref);
   }
   // As in amt_butitem(), this is the only point where a branch node's
   // occupancy could have just become exactly 1 -- collapse it if so.
   node = amt_collapse(node, path.node[step], leaf_decref);
   if (step == 0 && trienode_occupancy(node) == 0) {
      // The whole tree drained to nothing. `node` and `a` may be the same
      // object here (if `a` was already transient and got emptied in
      // place) or two distinct ones (if `a` was persistent and got claimed
      // into a separate, now-empty copy along the way) -- either way, every
      // reference either of them still holds needs retiring exactly once,
      // in favor of hunting the shared canonical empty node down instead.
      uint8_t leafsize = node->header.leafsize;
      amtnode_decref(node, leaf_decref);
      if (node != a)
         amtnode_decref(a, leaf_decref);
      return amt_empty(leafsize);
   }
   // From here up (if any steps remain), reattach the surviving node
   // pointer normally. As in tamt_setitem(), if a parent already holds this
   // exact pointer there's nothing to do at this level or above (and
   // calling amtnode_set() anyway would risk decreffing this same live
   // object out from under us) -- stop immediately in that case.
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, amtnode_bit2cellindex(parent, bi)) == node)
         return a;
      node = amtnode_set(parent, bi, node, NULL, NULL);
   }
   // As in tamt_setitem(), the root's own reference (the one held by
   // whatever variable the caller passed in as `a`) has no parent cell to
   // retire it via the loops' own overwrite-decrefs, so it must be retired
   // explicitly here, exactly once, whenever the root ended up replaced --
   // whether by the walk-up-while-empty loop claiming it (path.steps == 1),
   // the collapse above replacing it with its lone child, or the reattach
   // loop claiming/reallocating it.
   if (node != a)
      amtnode_decref(a, leaf_decref);
   return node;
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_AMT_H
