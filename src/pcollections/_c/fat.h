///////////////////////////////////////////////////////////////////////////////
// _c/fat.h
// FAT-specific node construction, mutation, and per-item update operations.
//
// All FAT nodes are represented using the shared Trie_t/struct TrieData type
// defined in trie.h; there is no separate FAT-specific struct or pointer type
// here. The low-level, transience-agnostic structural helpers (digit/divisor
// arithmetic, bit/cellindex helpers, refcounting, free, lookup, and path
// iteration) all live in trie.h as trienode_*/fatnode_*/fat_* functions. This
// file only contains what's genuinely FAT-specific: node construction, the
// copy-and-modify and mutate-in-place update primitives, and the per-item
// (whole-tree) update operations built on top of them -- mirroring amt.h
// function-for-function, adapted for FAT's fixed-arity layout.
//
// The one structural fact that makes this file simpler than amt.h throughout:
// a FAT node *always* has exactly FAT_CELLS (29) cells allocated, at every
// depth and regardless of occupancy or transience, and a cell's index is
// always identical to its bitindex (fatnode_bit2cellindex() is the identity
// function -- see trie.h). An AMT node, by contrast, is compacted down to
// exactly as many cells as it has occupied bits, with cellindex computed via
// popcount, which is why amt.h's primitives need mincells/nextbinpow growth
// bookkeeping and memmove-based insert/delete shifting. None of that applies
// here: every FAT node already has room for any bit that could ever be set in
// it, so "growing" a node is never necessary, and un/setting a cell is a
// direct, position-preserving write -- no shifting of any other cell is ever
// required.
//
// Naming conventions used below (mirroring amt.h's overall convention):
//  - fat_*      : operates on a FAT node/tree without caring whether it (or
//                 the nodes it touches) is persistent or transient. This
//                 includes the non-mutating "and"/"but" family: they always
//                 either return their input completely unchanged, or a newly
//                 allocated node whose transient bit matches the input's.
//  - tfat_*/pfat_* : operate specifically on transient/persistent nodes.
//                 tfat_setitem()/tfat_delitem() *mutate* an already-transient
//                 trie in place (claiming any not-yet-owned persistent
//                 subtrees they descend through by copying and flipping the
//                 transient bit on the copy). There is no separate pfat_*
//                 item-level entry point: fat_anditem()/fat_butitem() already
//                 produce a wholly new, untouched-input persistent result
//                 when called on a fully persistent trie, so they serve as
//                 the persistent API directly.
//
// A note on FAT's structural invariant, since it's the opposite of AMT's in
// one important respect. AMT is built for arbitrary, sparse 64-bit keys, so
// every amt_subjoin()/amtnode collapse is aimed at keeping the tree as
// *minimal* as possible: a branch node always has >= 2 children, and any
// branch that would be left with exactly 1 child after a deletion is
// collapsed away in favor of that child directly, so no depth is ever
// "wasted" materializing a node that doesn't actually disambiguate anything.
// FAT, by contrast, indexes keys that start at 0 and are (almost always)
// densely sequential -- so instead of a minimal-tree invariant, FAT
// maintains a *dense* one: if a FAT node exists at depth d, then every depth
// from d down to the twig depth must be explicitly represented somewhere
// beneath it -- i.e. every occupied cell of a branch node at depth d points
// to a child at EXACTLY depth d+1, never deeper, even when that child would
// otherwise have only one occupied cell of its own (unlike AMT, a FAT
// branch node is allowed to have as few as 1 child; it just may never be
// collapsed away). The one place a FAT tree is still allowed to "start
// deep" is at its own root: fat_subjoin() picks the shallowest depth at
// which two subtrees' prefixes first diverge as the depth for their new
// common parent, exactly as before -- that's not a skip, since there is
// nothing above the root that needs representing. What changed is that
// fat_subjoin() (and the "insert into an empty cell" paths in
// fat_anditem()/tfat_setitem()) now explicitly wrap each side down to
// parent-depth+1 via fat_wrap_chain()/fat_1leaf_at() instead of linking
// a deeper node directly into a shallower parent's cell.


//=============================================================================
// Initialization.

#ifndef _PCOLLECTIONS__C_FAT_H
#define _PCOLLECTIONS__C_FAT_H


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

// The fatnode_alloc() function just allocates space for the FAT node; as
// such, it only needs to know the cellsize -- every FAT node, whatever its
// depth or occupancy, always has exactly FAT_CELLS cells. All data is
// returned uninitialized.
static inline Trie_t fatnode_alloc(uint8_t cellsize) {
   return (Trie_t)malloc(
      sizeof(struct TrieData) + (size_t)FAT_CELLS*(size_t)cellsize);
}

// fatnode_init() initializes the header of a FAT node that has been
// allocated by fatnode_alloc(), or potentially memory that has been written
// into another struct.
// This function sets in the FAT node's header:
//  - refcount (set to 1)
//  - prefix (stored exactly as given -- see below)
//  - depth
//  - leafsize
//  - flags (just the is_transient flag -- see below)
//  - ncells (left at 0; FAT has no use for this field at all -- see below)
// It does not set the bits or the cells, which remain uninitialized.
static inline void fatnode_init(Trie_t a,
                                trieint_t prefix,
                                uint8_t leafsize,
                                uint8_t depth,
                                bool is_transient) {
   // The refcount should always start at 1. (We can set directly because no
   // other thread can legally access this memory yet.)
   a->header.refcount = 1;
   // `prefix` is stored here EXACTLY as given -- no masking, no OR'd-in flag
   // bits. See the long comment above "The trieint_t is_transient flag" in
   // trie.h for why: FAT's digit arithmetic is base-29 division/modulo, not
   // power-of-2 bit-shifting, so stealing even one low bit of `prefix` for
   // a flag corrupts every depth's digit extraction derived from it
   // (confirmed the hard way -- see that comment for the concrete failure).
   // is_transient never lived in `prefix` either; it lives in its own
   // dedicated `flags` byte instead (see that same trie.h comment), which
   // FAT shares with AMT unchanged. There is no is_fat flag anywhere
   // (removed entirely -- see that comment): a FAT node's cells only ever
   // point to other FAT nodes, so nothing ever needs to ask an arbitrary
   // node what kind it is; the caller already knows, from context, that
   // it's holding a FAT node.
   a->header.prefix = prefix;
   a->header.leafsize = leafsize;
   a->header.depth = depth;
   a->header.flags = (is_transient? TRIE_FLAG_ISTRANSIENT : 0);
   // `ncells` is entirely unused by FAT (every FAT node always has exactly
   // FAT_CELLS cells) -- set to 0 rather than left uninitialized, purely so
   // a debugger inspecting a FAT node's header doesn't show misleading
   // garbage.
   a->header.ncells = 0;
}

// fatnode_new() creates a new FAT object and returns it partially
// initialized. If given a leafsize of 0, this is interpreted as being for a
// Python object in the twigs (with refcounting). Nodes whose depths aren't
// FAT_MAX_DEPTH (i.e. twigs) are always refcounted during updates. All sizes
// but 0 treat the leaves as untyped bytes. The max leaf size is 255 bytes
// (max of uint8).
// The returned FAT node has the following data set:
//  - refcount (set to 1)
//  - prefix
//  - depth
//  - leafsize
// It does not set the bits or the cells, which are uninitialized. Unlike
// amtnode_new(), there's no ncells parameter -- every FAT node always gets
// exactly FAT_CELLS cells, so there's nothing to decide.
static inline Trie_t fatnode_new(trieint_t prefix,
                                 uint8_t leafsize,
                                 uint8_t depth,
                                 bool is_transient) {
   size_t cellsize = (depth == FAT_MAX_DEPTH? leafsize : sizeof(void*));
   Trie_t a = fatnode_alloc((uint8_t)cellsize);
   fatnode_init(a, prefix, leafsize, depth, is_transient);
   return a;
}

// fat_1leaf: Make a new twig node that contains a single pair (key / value).
// Increments the refcount for the value as well (if leaf_incref is given).
static inline Trie_t fat_1leaf(trieint_t key, void* val,
                               uint8_t leafsize,
                               bool is_transient,
                               void (*leaf_incref)(void*)) {
   triebits_t bi = (triebits_t)fatdepth_bitindex(FAT_MAX_DEPTH, key);
   // Canonicalize the stored prefix by zeroing this twig's own (last) digit,
   // mirroring amt_1leaf()'s `key & ~AMT_TWIG_MASK` -- a node's own digit is
   // recovered from which cell you're looking at, never from its prefix.
   trieint_t prefix = key - (trieint_t)bi;
   Trie_t h = fatnode_new(prefix, leafsize, FAT_MAX_DEPTH, is_transient);
   h->header.bits = (TRIEBITS_1 << bi);
   trienode_set_leaf(h, bi, val);
   if (leaf_incref)
      (*leaf_incref)(val);
   return h;
}

// fat_1leaf_at(): like fat_1leaf(), but for attaching a brand-new single
// key/value pair *underneath an existing branch node* rather than as a
// whole standalone tree. Builds the twig via fat_1leaf() as before, but then
// -- unlike fat_1leaf() alone -- also builds a chain of single-occupant
// branch nodes filling in every depth from `start_depth` down to (but not
// including) FAT_MAX_DEPTH, so the result satisfies FAT's dense-tree
// invariant (see the file header comment): a node may never be linked into
// a cell at a depth shallower than its own minus one. `start_depth` must be
// <= FAT_MAX_DEPTH; passing FAT_MAX_DEPTH is equivalent to a bare
// fat_1leaf() call (the loop below simply doesn't execute).
static inline Trie_t fat_1leaf_at(uint8_t start_depth, trieint_t key, void* val,
                                  uint8_t leafsize,
                                  bool is_transient,
                                  void (*leaf_incref)(void*)) {
   Trie_t node = fat_1leaf(key, val, leafsize, is_transient, leaf_incref);
   int d;
   for (d = (int)FAT_MAX_DEPTH - 1; d >= (int)start_depth; --d) {
      triebits_t bi = (triebits_t)fatdepth_bitindex((uint8_t)d, key);
      trieint_t div = fatdepth_div((uint8_t)d) * TRIEINT_C(FAT_CELLS);
      trieint_t prefix = key - (key % div);
      Trie_t parent = fatnode_new(prefix, leafsize, (uint8_t)d, is_transient);
      parent->header.bits = (TRIEBITS_1 << bi);
      trienode_set_subt(parent, bi, node);
      node = parent;
   }
   return node;
}

// fat_cells_incref() increments the references to all the occupied cells in
// the given FAT node: child subtrees for a branch node, or leaf values (if
// leaf_incref is given) for a twig node. Unlike amt_cells_incref() (which
// can loop blindly over cellindex 0..occupancy-1, since AMT cells are
// compacted contiguously in that range), a FAT node's occupied cells are
// scattered across bitindex 0..FAT_CELLS-1 with unoccupied gaps in between
// -- cellindex == bitindex always -- so this must scan set bits explicitly.
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
// Non-mutating (copy-and-modify) node primitives.
// fat_and()/fat_but() never touch their input node: they either return it
// completely unchanged (when the requested change is a no-op) or allocate a
// brand new node that is a copy of the input plus the one changed cell/bit.
// A newly allocated node's transient bit is always inherited unchanged from
// the input node, exactly as amt_and()/amt_but() do.

// fat_and() creates a new FAT node that is identical to the given FAT node
// except for having the cell at bitindex `bi` set to `val` (allocating the
// bit if it wasn't already set). The leaf_incref function is for
// incrementing the reference count of a leaf value (ignored for branch
// nodes, where the child subtree's own refcount is incremented instead). If
// it's null, no leaf reference counting is tracked. The returned node's
// child/leaf references (including the new one) are all correctly
// reference-counted; the input node `a` is left untouched.
// Unlike amt_and(), there's no separate "overwriting an existing cell" vs.
// "adding a new cell" case to distinguish, and no mincells parameter: since
// every FAT node already has all FAT_CELLS slots physically present, setting
// bit `bi` (whether or not it was already set) is exactly the same
// operation -- copy the whole fixed-size cells array, OR the bit into
// `bits`, and write `val` into slot `bi`.
static inline Trie_t fat_and(Trie_t a,
                             triebits_t bi,
                             void* val,
                             void (*leaf_incref)(void*)) {
   Trie_t new_a;
   bool is_twig = fatnode_is_twig(a);
   size_t cellsize = fatnode_cellsize(a);
   bool is_tr = trie_is_transient(a);
   triebits_t sbi;
   new_a = fatnode_new(a->header.prefix, a->header.leafsize, a->header.depth, is_tr);
   memcpy(new_a->cells, a->cells, (size_t)FAT_CELLS*cellsize);
   new_a->header.bits = a->header.bits | (TRIEBITS_1 << bi);
   if (is_twig) {
      trienode_set_leaf(new_a, bi, val);
      // As in amt_and(): a twig's leaf value follows the caller-controlled
      // leaf_incref convention, called for every occupied cell of the
      // result -- including the just-written one -- since callers pass a
      // real leaf_incref when they want the newly-written value's
      // ownership transferred in from a borrowed reference.
      if (leaf_incref)
         for (sbi = trienode_first_bitindex(new_a); sbi < FAT_CELLS;
              sbi = trienode_next_bitindex(new_a, sbi))
            (*leaf_incref)(trienode_leaf(new_a, sbi));
   } else {
      trienode_set_subt(new_a, bi, (Trie_t)val);
      // A branch child pointer, by contrast, is never incref'd for the cell
      // we just wrote: by this file's convention (matching amt_and()) it
      // already carries exactly the one unit of ownership meant for this
      // cell. Every *other* occupied cell was copied from `a` and is now
      // shared between `a` (untouched) and new_a, so each of those gets an
      // incref.
      for (sbi = trienode_first_bitindex(new_a); sbi < FAT_CELLS;
           sbi = trienode_next_bitindex(new_a, sbi))
         if (sbi != bi)
            trienode_incref(trienode_subt(new_a, sbi));
   }
   return new_a;
}

// fat_but() creates a new FAT node that is identical to the given FAT node
// except for having the cell at bitindex `bi` unset. If the bit wasn't set to
// begin with, this is a no-op and the input node `a` is returned completely
// unchanged (no allocation, no incref). Otherwise, behaves like fat_and():
// the input is left untouched, and the returned node is a correctly
// reference-counted new node whose transient bit matches the input's. Unlike
// amt_but(), there's no memmove to close a gap and no mincells parameter --
// clearing a bit never needs to touch any other cell's position.
static inline Trie_t fat_but(Trie_t a,
                             triebits_t bi,
                             void (*leaf_incref)(void*)) {
   Trie_t new_a;
   size_t cellsize = fatnode_cellsize(a);
   bool is_tr = trie_is_transient(a);
   if (!(a->header.bits & (TRIEBITS_1 << bi))) {
      // The bit isn't set, so deleting it is a no-op: return the input
      // completely unchanged.
      return a;
   }
   new_a = fatnode_new(a->header.prefix, a->header.leafsize, a->header.depth, is_tr);
   memcpy(new_a->cells, a->cells, (size_t)FAT_CELLS*cellsize);
   new_a->header.bits = a->header.bits & ~(TRIEBITS_1 << bi);
   // Every remaining occupied cell (there is no "just written" cell to
   // exclude here, unlike fat_and()) is now shared between `a` and new_a.
   fat_cells_incref(new_a, leaf_incref);
   return new_a;
}

// fat_divergedepth(): find the shallowest FAT depth at which two prefixes'
// digits diverge. Used by fat_subjoin() to decide where two previously-
// unrelated subtrees should be joined -- mirrors amt_subjoin()'s use of
// clz_trieint() on the XOR of the two prefixes to find the highest differing
// bit, but FAT has no equivalent of "leading zero count" for a base-29
// digit representation, so this scans depths from the root down instead
// (at most FAT_MAX_DEPTH iterations, since fat_subjoin()'s callers only ever
// invoke it in a context where the two prefixes are already known to
// genuinely diverge somewhere at or above the deeper of the two nodes'
// depths -- see triepath_fatfind()'s is_beneath contract).
static inline uint8_t fat_divergedepth(trieint_t pa, trieint_t pb) {
   uint8_t d;
   for (d = 0; d < FAT_MAX_DEPTH; ++d)
      if (fatdepth_bitindex(d, pa) != fatdepth_bitindex(d, pb))
         return d;
   return FAT_MAX_DEPTH;
}

// fat_wrap_chain(): given an already-fully-formed, exclusively-owned `node`
// (branch or twig) at some depth strictly greater than `target_depth`,
// build a chain of single-occupant branch nodes filling in every
// intervening depth from `target_depth` up to (but not including) `node`'s
// own depth, and return the node at `target_depth` -- the top of that
// chain. This is what lets fat_subjoin() honor FAT's dense-tree invariant
// (see the file header comment) when joining two subtrees whose depths
// don't happen to already sit at exactly (new parent depth)+1: rather than
// linking `node` directly into a shallower cell (which would silently skip
// every depth in between, exactly the AMT-style compression FAT must NOT
// do), each intervening depth gets its own real branch node with exactly
// one occupied cell.
// `node`'s own single reference is consumed as the base of the chain (no
// incref happens here); the caller is responsible for arranging that
// exactly one reference to `node` is available to hand off, precisely as
// with any other and/but/subjoin primitive in this file. If
// `node->header.depth == target_depth` already, this is a no-op: `node` is
// returned completely unchanged (no allocation).
// The digit at each intervening depth is read off of `node`'s own
// (canonicalized) prefix, captured once before the loop -- a node's prefix
// always retains every digit shallower than its own depth untouched (see
// fat_1leaf()'s and fat_subjoin()'s canonicalization comments), so this is
// exactly as valid for depths well above `node`'s immediate parent as it is
// for the one directly above it.
static inline Trie_t fat_wrap_chain(Trie_t node, uint8_t target_depth,
                                    bool is_transient) {
   trieint_t prefix = trienode_prefix(node);
   int d;
   for (d = (int)node->header.depth - 1; d >= (int)target_depth; --d) {
      triebits_t bi = (triebits_t)fatdepth_bitindex((uint8_t)d, prefix);
      trieint_t div = fatdepth_div((uint8_t)d) * TRIEINT_C(FAT_CELLS);
      trieint_t parent_prefix = prefix - (prefix % div);
      Trie_t parent = fatnode_new(parent_prefix, node->header.leafsize,
                                  (uint8_t)d, is_transient);
      parent->header.bits = (TRIEBITS_1 << bi);
      trienode_set_subt(parent, bi, node);
      node = parent;
   }
   return node;
}

// fat_subjoin(): join two sub-FATs (which must not already share a common
// parent) under a new parent FAT node and return it. Takes ownership of `a`
// without incref'ing it -- matching amt_subjoin()'s convention (see there):
// `a` is expected to be a freshly built, exclusively-owned node (e.g. the
// result of fat_1leaf()) whose one unit of ownership transfers directly
// into the new parent's cell (by way of fat_wrap_chain(), if `a` doesn't
// already sit at exactly depth+1). `b`, by contrast, is expected to still
// be referenced elsewhere, so it gets an explicit incref for its new
// reference from the parent, before it's (potentially) wrapped -- the wrap
// chain, like the parent's cell itself, consumes exactly that one new
// reference. The new parent's transient bit is set from is_transient
// directly, since there's no existing node to inherit it from.
// Per FAT's dense-tree invariant (see the file header comment), the new
// parent's own depth is still chosen as the shallowest depth at which `a`
// and `b`'s prefixes diverge -- that's the one place a FAT (sub)tree is
// allowed to "start deep", since there's nothing above this new parent that
// needs representing -- but everything *below* the new parent, down to
// wherever `a` and `b` actually live, is now filled in one depth at a time.
static inline Trie_t fat_subjoin(Trie_t a, Trie_t b, bool is_transient) {
   trieint_t pa = trienode_prefix(a);
   trieint_t pb = trienode_prefix(b);
   uint8_t depth = fat_divergedepth(pa, pb);
   triebits_t aii = (triebits_t)fatdepth_bitindex(depth, pa);
   triebits_t bii = (triebits_t)fatdepth_bitindex(depth, pb);
   // Canonicalize the parent's prefix the same way fat_1leaf() does: zero
   // out this depth's own digit (and everything below it) -- everything
   // shallower than `depth` is preserved, since `a` and `b` agree on all of
   // that by construction (this is exactly the depth where they first stop
   // agreeing).
   trieint_t div = fatdepth_div(depth) * TRIEINT_C(FAT_CELLS);
   trieint_t prefix = pa - (pa % div);
   Trie_t parent = fatnode_new(prefix, a->header.leafsize, depth, is_transient);
   Trie_t achain, bchain;
   trienode_incref(b);
   achain = fat_wrap_chain(a, (uint8_t)(depth + 1), is_transient);
   bchain = fat_wrap_chain(b, (uint8_t)(depth + 1), is_transient);
   parent->header.bits = (TRIEBITS_1 << aii) | (TRIEBITS_1 << bii);
   trienode_set_subt(parent, aii, achain);
   trienode_set_subt(parent, bii, bchain);
   return parent;
}


//=============================================================================
// Mutating (mutate-in-place-or-claim) node primitives.
// fatnode_set()/fatnode_del() are used specifically by the tfat_* item-level
// operations below while walking down an already-transient trie. See
// amtnode_set()/amtnode_del()'s comments in amt.h for the general
// claim-vs-mutate-in-place shape shared by both. The one thing that's
// *not* shared: because every FAT node -- transient or persistent -- is
// always fully allocated at FAT_CELLS cells, the already-transient,
// mutate-in-place path here never has a "no room, reallocate" branch to
// worry about. That entire category of subtlety in amtnode_set() (and the
// real bug it took an AddressSanitizer catch to find and fix) simply
// doesn't exist for FAT.

static inline Trie_t fatnode_set(Trie_t a,
                                 triebits_t bi,
                                 void* val,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   bool is_twig = fatnode_is_twig(a);
   triebits_t bit = (TRIEBITS_1 << bi);
   if (!trie_is_transient(a)) {
      // a is a not-yet-claimed persistent subtree: get a private, transient
      // copy. fat_and() always allocates a fresh copy (there's no "no-op"
      // shortcut for fat_and() the way there is for fat_but()), so this is
      // always both the claim and the mutation in one step.
      Trie_t new_a = fat_and(a, bi, val, leaf_incref);
      trienode_set_transient(new_a, true);
      return new_a;
   }
   // a is already transient and therefore uniquely owned: mutate in place.
   // There's always room for `bi` -- every FAT node has all FAT_CELLS cells
   // physically present from the moment it's allocated.
   if (a->header.bits & bit) {
      // We're overwriting an existing cell; deref whatever was there first.
      if (!is_twig)
         fatnode_decref(trienode_subt(a, bi), leaf_decref);
      else if (leaf_decref)
         (*leaf_decref)(trienode_leaf(a, bi));
   } else {
      a->header.bits |= bit;
   }
   // Set the new value in place. As in amtnode_set(), a branch child pointer
   // is never incref'ed here (it already carries its one unit of ownership);
   // a twig's leaf value follows the separate, controllable leaf_incref
   // convention instead.
   if (is_twig) {
      trienode_set_leaf(a, bi, val);
      if (leaf_incref)
         (*leaf_incref)(val);
   } else {
      trienode_set_subt(a, bi, (Trie_t)val);
   }
   return a;
}

// fatnode_del(): the mutate-in-place-or-claim counterpart to fatnode_set(),
// used by tfat_delitem(). See fatnode_set()'s comment for the general shape.
// Unlike amtnode_del(), there's no memmove to close a gap when removing a
// cell -- clearing the bit is the entire operation.
static inline Trie_t fatnode_del(Trie_t a,
                                 triebits_t bi,
                                 void (*leaf_incref)(void*),
                                 void (*leaf_decref)(void*)) {
   triebits_t bit = (TRIEBITS_1 << bi);
   if (!trie_is_transient(a)) {
      // a is not-yet-claimed and persistent. fat_but() may itself decide
      // this is a no-op (bit not set) and hand back `a` unchanged -- in
      // that case there is nothing to claim, since nothing was allocated.
      Trie_t new_a = fat_but(a, bi, leaf_incref);
      if (new_a != a)
         trienode_set_transient(new_a, true);
      return new_a;
   }
   // a is already transient and therefore uniquely owned: mutate in place.
   if (!(a->header.bits & bit)) {
      // The bit isn't set, so there's nothing to do.
      return a;
   }
   if (!fatnode_is_twig(a))
      fatnode_decref(trienode_subt(a, bi), leaf_decref);
   else if (leaf_decref)
      (*leaf_decref)(trienode_leaf(a, bi));
   a->header.bits &= ~bit;
   return a;
}


//=============================================================================
// FAT API Functions (whole-tree, per-item operations).

// The fat_empty() function can be used to get the canonical empty PFAT for a
// particular leafsize. There is no separate canonical empty transient FAT;
// you always start from the canonical empty persistent one and call
// tfat_setitem() on it (or an equivalent "make transient" step) to begin
// building a transient one.
// Unlike amt_empty() (see its doc comment in amt.h), FAT does not maintain a
// *minimal*-tree invariant -- it maintains the opposite, *dense*-tree
// invariant described in this file's header comment: a branch node may have
// as few as 1 occupied cell, and is never collapsed away in favor of that
// lone child (doing so would skip that depth, which FAT must never do). The
// only structural guarantee fat_empty() itself is part of: a node has 0
// occupied cells if and only if it is this canonical empty node -- any
// FAT node that would otherwise become fully empty (whether it's a lone
// twig or an interior branch whose last child was just removed) is always
// replaced by, or pruned in favor of, this shared node rather than kept
// around as a real "empty" node of its own. A fat_empty() of a given
// leafsize is a distinct node from an amt_empty() of the same leafsize --
// they're different kinds entirely, never interchangeable and never
// pointer-equal -- but nothing needs to check that dynamically: nothing
// that holds a FAT tree could ever end up holding an AMT one instead, so
// there's no is_fat-style flag to compare here, just two separate
// functions.
Trie_t fat_empty(uint8_t leafsize);

// fat_anditem(): return a new tree identical to `a` except that `key` maps to
// `val`. Does not modify `a` (or anything reachable from it) in any way.
// Works regardless of whether `a` (or any node along the affected path) is
// persistent or transient -- mirrors amt_anditem() structurally, using
// triepath_fatfind()/fat_1leaf()/fat_1leaf_at()/fat_and()/fat_subjoin() in
// place of the AMT equivalents. The one place this genuinely differs from
// amt_anditem() (beyond helper names): when the current node needs a
// brand-new child, that child is built with fat_1leaf_at(), which fills in
// every depth from the current node's depth+1 down to the twig depth
// explicitly -- never a bare twig linked straight into a shallower cell --
// per FAT's dense-tree invariant (see the file header comment).
static inline Trie_t fat_anditem(Trie_t a, trieint_t key, void* val,
                                 void (*leaf_incref)(void*)) {
   Trie_t node;
   TriePath path;
   int found;
   uint8_t step;
   // If a is empty, just return a new twig. Nothing needs to be
   // explicitly represented above a lone key: the dense-tree invariant only
   // constrains what's *beneath* an existing node, and there's nothing
   // beneath a brand-new standalone twig.
   if (a->header.bits == 0)
      return fat_1leaf(key, val, a->header.leafsize, trie_is_transient(a),
                       leaf_incref);
   found = triepath_fatfind(&path, a, key);
   step = path.steps - 1;
   node = path.node[step];
   if (!found) {
      if (!path.is_beneath) {
         // The path diverges here -- we need a new leaf-pair and to connect
         // it to the highest subtree we did share a prefix with.
         // fat_subjoin() itself fills in any depths between its new parent
         // and each side's actual depth.
         node = fat_subjoin(
            fat_1leaf(key, val, a->header.leafsize, trie_is_transient(node),
                     leaf_incref),
            node,
            trie_is_transient(node));
      } else if (fatnode_is_twig(node)) {
         // The current node is a twig, and we need to add a leaf.
         node = fat_and(node, path.index[step], val, leaf_incref);
      } else {
         // The current node needs a new child: build the full chain from
         // this node's own depth+1 down to the twig, holding the one new
         // key/value pair.
         node = fat_and(
            node,
            path.index[step],
            fat_1leaf_at((uint8_t)(node->header.depth + 1), key, val,
                        a->header.leafsize, trie_is_transient(node),
                        leaf_incref),
            NULL);
      }
   } else {
      // The item was found: overwrite that cell.
      node = fat_and(node, path.index[step], val, leaf_incref);
   }
   // We now need to build a new path by backtracking up the steps, wrapping
   // each parent in a fresh copy that points at the updated child.
   while (step > 0) {
      --step;
      node = fat_and(path.node[step], path.index[step], node, NULL);
   }
   return node;
}

// fat_butitem(): return a new tree identical to `a` except that `key` (and
// its associated value) is absent. If `key` isn't present in `a` to begin
// with, this is a no-op and `a` is returned completely unchanged. Mirrors
// amt_butitem() structurally (pruning any subtree that drains to fully
// empty, and converging a fully drained tree onto the canonical fat_empty()
// node), but does NOT collapse a branch left with exactly one child -- per
// FAT's dense-tree invariant (see the file header comment), a branch node
// with a single occupied cell is a perfectly ordinary, valid FAT node, not
// a case to special-case away.
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
   // As long as the node we just produced is completely empty, it must be
   // removed from ITS parent's cells entirely rather than reattached -- see
   // amt_butitem()'s comment for why the freshly-produced "emptied" node
   // must be explicitly decreffed here or it leaks.
   while (step > 0 && trienode_occupancy(node) == 0) {
      Trie_t emptied = node;
      --step;
      node = fat_but(path.node[step], path.index[step], leaf_incref);
      fatnode_decref(emptied, NULL);
   }
   if (step == 0 && trienode_occupancy(node) == 0) {
      // The whole tree drained to nothing: swap this one-off empty result
      // for the shared canonical one.
      uint8_t leafsize = node->header.leafsize;
      fatnode_decref(node, NULL);
      return fat_empty(leafsize);
   }
   // From here up (if any steps remain), reattach the surviving node
   // pointer normally.
   while (step > 0) {
      --step;
      node = fat_and(path.node[step], path.index[step], node, NULL);
   }
   return node;
}

// tfat_setitem(): mutate the transient tree `a` in place so that `key` maps
// to `val`, claiming (copying and marking transient) any not-yet-owned
// persistent subtrees encountered along the way. Returns the (possibly new)
// root of the tree -- callers must always use the returned value. Mirrors
// tamt_setitem() exactly, including its "consumes exactly one reference to
// the top-level `a` argument" contract (see tamt_setitem()'s comment in
// amt.h for the full rationale -- a caller wanting to keep its own separate
// reference to the original `a` must incref() it first, same as sharing any
// other persistent node with a new owner).
static inline Trie_t tfat_setitem(Trie_t a, trieint_t key, void* val,
                                  void (*leaf_incref)(void*),
                                  void (*leaf_decref)(void*)) {
   Trie_t node, newnode;
   TriePath path;
   int found;
   uint8_t step;
   if (a->header.bits == 0) {
      Trie_t result = fat_1leaf(key, val, a->header.leafsize, true, leaf_incref);
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
         // The current node needs a new child: as in fat_anditem(), build
         // the full chain from this node's own depth+1 down to the twig
         // (see fat_1leaf_at()'s comment) rather than a bare twig.
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
   // Propagate the (possibly new) child pointer back up the path, claiming/
   // mutating each parent in turn -- see tamt_setitem()'s comment for why
   // this stops immediately (without touching anything further up) the
   // moment a parent already holds the exact child pointer we just produced.
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, bi) == newnode)
         return a;
      node = newnode;
      newnode = fatnode_set(parent, bi, node, NULL, NULL);
   }
   // `newnode` is now the (possibly new) root; retire `a`'s one reference
   // here if it ended up replaced, exactly as tamt_setitem() does.
   if (newnode != a)
      fatnode_decref(a, leaf_decref);
   return newnode;
}

// tfat_delitem(): mutate the transient tree `a` in place so that `key` (and
// its associated value) is absent, claiming any not-yet-owned persistent
// subtrees encountered along the way. Returns the (possibly new) root --
// callers must always use the returned value. Mirrors tamt_delitem()
// structurally, but -- exactly as fat_butitem() differs from amt_butitem()
// -- never collapses a branch left with exactly one child; see fat_butitem()
// 's comment and the file header comment for why.
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
   node = fatnode_del(path.node[step], path.index[step], leaf_incref, leaf_decref);
   // As long as the node we just produced is completely empty, remove it
   // from its parent's cells entirely (instead of reattaching it) and keep
   // walking upward -- see tamt_delitem()'s comment for why the now-orphaned
   // empty copy must be explicitly decreffed here whenever a claim happened.
   while (step > 0 && trienode_occupancy(node) == 0) {
      orig_child = path.node[step];
      --step;
      Trie_t parent = path.node[step];
      Trie_t emptied = node;
      node = fatnode_del(parent, path.index[step], leaf_incref, leaf_decref);
      if (emptied != orig_child)
         fatnode_decref(emptied, leaf_decref);
   }
   if (step == 0 && trienode_occupancy(node) == 0) {
      // The whole tree drained to nothing -- see tamt_delitem()'s comment:
      // `node` and `a` may or may not be the same object here, but either
      // way every reference either of them still holds needs retiring
      // exactly once, in favor of the shared canonical empty node.
      uint8_t leafsize = node->header.leafsize;
      fatnode_decref(node, leaf_decref);
      if (node != a)
         fatnode_decref(a, leaf_decref);
      return fat_empty(leafsize);
   }
   // From here up (if any steps remain), reattach the surviving node
   // pointer normally.
   while (step > 0) {
      --step;
      Trie_t parent = path.node[step];
      triebits_t bi = path.index[step];
      if (trienode_subt(parent, bi) == node)
         return a;
      node = fatnode_set(parent, bi, node, NULL, NULL);
   }
   // As in tamt_delitem(), the root's own reference has no parent cell to
   // retire it via the loops' own overwrite-decrefs, so it must be retired
   // explicitly here whenever the root ended up replaced.
   if (node != a)
      fatnode_decref(a, leaf_decref);
   return node;
}

#undef EXTC

#endif  // ifndef _PCOLLECTIONS__C_FAT_H
