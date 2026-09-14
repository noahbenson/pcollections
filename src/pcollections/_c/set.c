///////////////////////////////////////////////////////////////////////////////
// _c/set.c
// The persistent (pset) and transient (tset) set types, implemented as thin
// CPython wrappers around a pair of tries: an AMT used as a hash table
// (hash(elem) -> index) and a FAT used as an insertion-ordered element table
// (index -> (elem, next_index)).
//
// This is deliberately almost the same file as dict.c, with the value half
// of every (key, value, next) triple removed -- see dict.c's own file header
// for the full rationale behind the two-trie design, the tombstone-based
// deletion scheme, and the compaction policy; only the differences from
// dict.c are called out in comments here.
//
//  - `els` (a FAT) is the insertion-ordered element table, dense indices
//    0, 1, ..., `top`-1. Each leaf is a `SetEntry` (see below): the element
//    plus the FAT index of the *next* entry sharing the same hash (or
//    SET_NO_NEXT), forming a singly-linked collision chain. Iterating `els`
//    in ascending index order (fat_firstpath/fat_nextpath) therefore visits
//    elements in insertion order, exactly like pdict/tdict's own `els`.
//  - `idx` (an AMT) maps hash(elem) -> the FAT index of the *first* entry in
//    that hash's collision chain, exactly as in dict.c.
//
// Compaction: identical policy and mechanism to dict.c's -- discard()
// overwrites the doomed slot with a tombstone (see setentry_is_tombstone()
// below) rather than actually removing it from `els` (removing a *middle*
// FAT index would shift every later index down by one, silently
// invalidating `idx` and every collision-chain `.next` pointer at or past
// that index -- this is not a hypothetical concern: it is exactly the bug
// that dict.c's own tombstone scheme was written to fix, confirmed via a
// dedicated debug walk during that work). `ndeleted > 1024**2 or
// ndeleted > 0.3*count` (checked after every mutating op) triggers a
// from-scratch rebuild of `els`/`idx` with the live elements renumbered
// 0..count-1 in their original insertion order.
//
// Every FAT node here has leafsize == sizeof(SetEntry); every AMT node has
// leafsize == sizeof(trieint_t).
//
// Scope note: pset/tset implement the "must implement" primitives of
// PersistentSet/TransientSet (see pcollections/abc/_set.py) natively in C --
// add/discard/clear/transient/__len__/__iter__/__contains__ for pset;
// add/discard/clear/persistent/__len__/__iter__/__contains__ for tset --
// plus __hash__ (pset only; MutableSet's default is already fully generic
// -- hash(frozenset(self)) + 1 -- so tset need only have it disabled, not
// replaced). Everything else -- pop/remove/addall/discardall/removeall/
// union/intersection/difference/symmetric_difference/isdisjoint/issubset/
// issuperset/__eq__/__ne__/__lt__/__le__/__gt__/__ge__/__or__/__and__/
// __sub__/__xor__/__ior__/__iand__/__isub__/__ixor__/__reduce__/__str__/
// __repr__ -- is inherited for free from PersistentSet/TransientSet (which
// in turn get their Set-algebra defaults from collections.abc.Set/
// MutableSet), via the same PyType_FromSpecWithBases heap-type technique
// dict.c/list.c use. Like pdict/tdict, pset/tset are not subclassable
// (Py_TPFLAGS_BASETYPE left unset) -- note for later: the planned ldict/
// llist-style lazy subclasses will need BASETYPE added to whichever of
// these C types they end up inheriting from.
//
// pset/tset do NOT get native __repr__/__str__ overrides the way pdict/
// tdict do: PersistentSet.__str__/__repr__/TransientSet.__str__/__repr__
// are fully generic (built from seqstr(self, ...), which only needs
// __iter__), so they're left to inherit rather than being reimplemented
// natively -- see the note on the "same slot-inheritance uncertainty as
// tp_hash" below for why this was decided empirically rather than assumed.


//=============================================================================
// Initialization.

#include <Python.h>
#include <string.h>
#include "uintbits.h"
// trie.h provides pcoll_atomic_u64_t/PCOLL_ATOMIC_U64_FETCH_*1 (a portable
// stand-in for <stdatomic.h>, which MSVC doesn't have at all without
// /std:c11 -- see trie.h's shim comment); this file doesn't call any
// atomic_* function directly, so it doesn't need its own <stdatomic.h>
// include (and, on Windows, must not have one).
#include "trie.h"
#include "amt.h"
#include "fat.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


//=============================================================================
// SetEntry: the FAT leaf type for `els`. Mirrors dict.c's DictEntry minus
// the value.

typedef struct {
   PyObject* key;
   trieint_t next;   // FAT index of the next entry sharing this element's
                      // hash, or SET_NO_NEXT if this is the last link.
} SetEntry;
#define SETELSLEAFSIZE ((uint8_t)sizeof(SetEntry))
// idx (the AMT hash table) stores raw trieint_t FAT-index leaves.
#define SETIDXLEAFSIZE ((uint8_t)sizeof(trieint_t))
#define SET_NO_NEXT (~(trieint_t)0)

// Compaction thresholds -- identical to dict.c's.
#define SET_COMPACT_ABS_THRESHOLD ((Py_ssize_t)1024 * 1024)
#define SET_COMPACT_FRAC_NUM 3
#define SET_COMPACT_FRAC_DEN 10


//=============================================================================
// fat_empty()/amt_empty(): see dict.c/list.c for the identical pattern and
// rationale -- each translation unit that uses these tries defines its own
// canonical-empty-node singletons, indexed by leafsize.

static Trie_t g_fat_empty_singletons[256];
static Trie_t g_amt_empty_singletons[256];

Trie_t fat_empty(uint8_t leafsize) {
   if (!g_fat_empty_singletons[leafsize]) {
      Trie_t t = fatnode_new(0, leafsize, FAT_MAX_DEPTH, false);
      t->header.bits = 0;
      trienode_incref(t);
      g_fat_empty_singletons[leafsize] = t;
   }
   trienode_incref(g_fat_empty_singletons[leafsize]);
   return g_fat_empty_singletons[leafsize];
}
Trie_t amt_empty(uint8_t leafsize) {
   if (!g_amt_empty_singletons[leafsize]) {
      // Must go through amtnode_new() (which calls amtnode_init()), not a
      // bare amtnode_alloc() -- see dict.c's identical comment; an early
      // version of dict.c's own amt_empty() got this wrong and it was
      // caught immediately by ASAN as a stack-buffer-overflow the first
      // time a mutating op was ever exercised.
      Trie_t t = amtnode_new(0, leafsize, AMT_MAX_DEPTH, 0, false);
      t->header.bits = 0;
      trienode_incref(t);
      g_amt_empty_singletons[leafsize] = t;
   }
   trienode_incref(g_amt_empty_singletons[leafsize]);
   return g_amt_empty_singletons[leafsize];
}


//=============================================================================
// fat_freeze()/amt_freeze(): recursively mark `a` and every transient node
// reachable from it as persistent -- see dict.c/list.c for the rationale.

static void fat_freeze(Trie_t a) {
   if (!trie_is_transient(a))
      return;
   trienode_set_transient(a, false);
   if (!fatnode_is_twig(a)) {
      triebits_t bi;
      for (bi = trienode_first_bitindex(a); bi < FAT_CELLS;
           bi = trienode_next_bitindex(a, bi))
         fat_freeze(trienode_subt(a, bi));
   }
}
static void amt_freeze(Trie_t a) {
   if (!trie_is_transient(a))
      return;
   trienode_set_transient(a, false);
   if (!amtnode_is_twig(a)) {
      triebits_t bi;
      for (bi = trienode_first_bitindex(a); bi < TRIEBITS_WIDTH;
           bi = trienode_next_bitindex(a, bi))
         amt_freeze(trienode_subt(a, amtnode_bit2cellindex(a, bi)));
   }
}


//=============================================================================
// Leaf refcounting callbacks.

// els leaves (SetEntry) own a reference to their key. idx leaves are raw
// FAT indices -- nothing to refcount.
static void setentry_incref(void* v) {
   SetEntry* e = (SetEntry*)v;
   Py_INCREF(e->key);
}
static void setentry_decref(void* v) {
   SetEntry* e = (SetEntry*)v;
   Py_DECREF(e->key);
}
static void noop_incref(void* v) { (void)v; }
static void noop_decref(void* v) { (void)v; }

//=============================================================================
// Tombstones -- see dict.c's much longer comment (near dictentry_is_
// tombstone()) for the full rationale for why deletion must overwrite
// rather than remove a FAT slot. Identical scheme here, just for SetEntry.
static PyObject* g_set_dummy = NULL;

// pcollections.util.seqstr -- the real reference string-formatting helper
// (imported once at PyInit_set time), used by pset_repr/pset_str/
// tset_repr/tset_str below so they match abc/_set.py's PersistentSet.
// __str__/__repr__ and TransientSet.__str__/__repr__ exactly.
static PyObject* g_seqstr = NULL;

// Calls the real pcollections.util.seqstr(seq, maxlen=maxlen) (or plain
// seqstr(seq) when has_maxlen is false, matching the reference's
// maxlen=None default); tostr is always left at its default (repr).
static PyObject* call_seqstr(PyObject* seq, long maxlen, int has_maxlen) {
   PyObject* args; PyObject* kwargs; PyObject* result;
   args = PyTuple_Pack(1, seq);
   if (!args) return NULL;
   kwargs = PyDict_New();
   if (!kwargs) { Py_DECREF(args); return NULL; }
   if (has_maxlen) {
      PyObject* ml = PyLong_FromLong(maxlen);
      if (!ml || PyDict_SetItemString(kwargs, "maxlen", ml) < 0) {
         Py_XDECREF(ml); Py_DECREF(args); Py_DECREF(kwargs); return NULL;
      }
      Py_DECREF(ml);
   }
   result = PyObject_Call(g_seqstr, args, kwargs);
   Py_DECREF(args); Py_DECREF(kwargs);
   return result;
}

static int setentry_is_tombstone(const SetEntry* e) {
   return e->key == g_set_dummy;
}
static void set_make_tombstone(SetEntry* out) {
   out->key = g_set_dummy;
   out->next = SET_NO_NEXT;
}


//=============================================================================
// A small recursive GC traversal helper: visits every PyObject* (the key of
// every entry) reachable from an `els` FAT tree.
static int set_gc_traverse(Trie_t node, visitproc visit, void* arg) {
   triebits_t bi;
   if (fatnode_is_twig(node)) {
      for (bi = trienode_first_bitindex(node); bi < FAT_CELLS;
           bi = trienode_next_bitindex(node, bi)) {
         SetEntry* e = (SetEntry*)trienode_leaf(node, fatnode_bit2cellindex(node, bi));
         Py_VISIT(e->key);
      }
   } else {
      for (bi = trienode_first_bitindex(node); bi < FAT_CELLS;
           bi = trienode_next_bitindex(node, bi)) {
         int r = set_gc_traverse(trienode_subt(node, fatnode_bit2cellindex(node, bi)),
                                  visit, arg);
         if (r) return r;
      }
   }
   return 0;
}


//=============================================================================
// Hash-key conversion -- identical to dict.c's dict_hash_key().
static int set_hash_key(PyObject* key, trieint_t* out) {
   Py_hash_t h = PyObject_Hash(key);
   if (h == -1 && PyErr_Occurred()) return -1;
   *out = (trieint_t)(size_t)h;
   return 0;
}


//=============================================================================
// Core chain-walking primitive shared by pset and tset. Mirrors dict.c's
// dict_chain_find() exactly (including *out_prev being set on BOTH the
// found and not-found paths -- dict.c's very first version of this function
// only set it on the not-found path, which was the root cause of a nasty,
// near-100%-reproducible corruption bug in pdict_drop()/tdict's __delitem__
// reading uninitialized stack garbage whenever the target was actually
// found; getting this right from the start here avoids repeating that).
static int set_chain_find(Trie_t els, Trie_t idx, trieint_t hkey,
                           PyObject* key, trieint_t* out_index,
                           trieint_t* out_prev) {
   void* found;
   trieint_t ii;
   trieint_t prev = SET_NO_NEXT;
   if (!amt_lookup(idx, hkey, &found)) {
      if (out_prev) *out_prev = SET_NO_NEXT;
      return 0;
   }
   ii = *(trieint_t*)found;
   while (1) {
      void* ep;
      SetEntry* e;
      int eq;
      fat_lookup(els, ii, &ep);
      e = (SetEntry*)ep;
      eq = PyObject_RichCompareBool(key, e->key, Py_EQ);
      if (eq < 0) return -1;
      if (eq) {
         if (out_index) *out_index = ii;
         if (out_prev) *out_prev = prev;
         return 1;
      }
      prev = ii;
      if (e->next == SET_NO_NEXT) break;
      ii = e->next;
   }
   if (out_prev) *out_prev = prev;
   return 0;
}


//=============================================================================
// pset

typedef struct {
   PyObject_HEAD
   Trie_t idx;           // AMT: hash -> first FAT index. Persistent.
   Trie_t els;           // FAT: index -> SetEntry. Persistent.
   Py_ssize_t top;        // next fresh index to hand out.
   Py_ssize_t count;      // number of live entries (== len()).
   Py_ssize_t ndeleted;   // holes burned by deletions since last compaction.
   Py_hash_t hashcode;    // -1 == not yet computed.
} PSetObject;

// PSetType/TSetType are heap types (see list.c's PListType for the full
// rationale): pset/tset need to inherit from the Python-level
// PersistentSet/TransientSet ABC mixins, and CPython refuses to let a
// statically allocated type derive from a dynamically allocated one.
static PyTypeObject* PSetType = NULL;
static PyTypeObject* TSetType = NULL;
// The iterator type (for pset/tset's own __iter__) is, by contrast, an
// ordinary static type: it doesn't need to isinstance() as anything in
// particular, so there's no reason to pay for the heap-type machinery.
static PyTypeObject PSetIterType;
static PyTypeObject TSetIterType;
static PSetObject* g_pset_empty = NULL;

static int pset_traverse(PSetObject* self, visitproc visit, void* arg) {
   return set_gc_traverse(self->els, visit, arg);
}
static int pset_clear(PSetObject* self) {
   Trie_t els = self->els, idx = self->idx;
   self->els = NULL; self->idx = NULL;
   if (els) fatnode_decref(els, setentry_decref);
   if (idx) amtnode_decref(idx, noop_decref);
   return 0;
}
static void pset_dealloc(PSetObject* self) {
   PyObject_GC_UnTrack(self);
   pset_clear(self);
   PyObject_GC_Del(self);
}
static Py_ssize_t pset_length(PSetObject* self) {
   return self->count;
}

// Takes ownership of the one reference to `els`/`idx` that callers already
// hold, wrapping them in a new pset -- EXCEPT that a 0-element result
// always collapses to the canonical empty pset singleton instead (mirrors
// pdict_wrap()'s identical convention in dict.c).
static PyObject* pset_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                            Py_ssize_t count, Py_ssize_t ndeleted) {
   PSetObject* self;
   if (count == 0) {
      fatnode_decref(els, setentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_INCREF(g_pset_empty);
      return (PyObject*)g_pset_empty;
   }
   self = PyObject_GC_New(PSetObject, PSetType);
   if (!self) {
      fatnode_decref(els, setentry_decref);
      amtnode_decref(idx, noop_decref);
      return NULL;
   }
   self->els = els;
   self->idx = idx;
   self->top = top;
   self->count = count;
   self->ndeleted = ndeleted;
   self->hashcode = -1;
   PyObject_GC_Track(self);
   return (PyObject*)self;
}

// Rebuilds a fresh, fully transient (els, idx) pair containing exactly the
// live entries currently in (els, idx), renumbered 0..count-1 in their
// original insertion-order sequence. Consumes neither input. Mirrors
// dict.c's dict_rebuild_compacted() exactly, minus the value half.
static int set_rebuild_compacted(Trie_t els, Trie_t idx,
                                  Trie_t* out_els, Trie_t* out_idx,
                                  Py_ssize_t* out_top) {
   Trie_t new_els = fat_empty(SETELSLEAFSIZE);
   Trie_t new_idx = amt_empty(SETIDXLEAFSIZE);
   trieint_t newtop = 0;
   TriePath iter;
   int ok;
   (void)idx;
   for (ok = fat_firstpath(els, &iter); ok; ok = fat_nextpath(&iter)) {
      SetEntry* e = (SetEntry*)triepath_val(&iter);
      SetEntry newentry;
      trieint_t hkey, prev;
      if (setentry_is_tombstone(e)) continue;
      if (set_hash_key(e->key, &hkey) < 0) {
         fatnode_decref(new_els, setentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return -1;
      }
      // Every key here is, by construction, not yet present in the fresh
      // tables, so this always appends a brand new chain link.
      set_chain_find(new_els, new_idx, hkey, e->key, NULL, &prev);
      if (prev == SET_NO_NEXT) {
         void* found;
         if (!amt_lookup(new_idx, hkey, &found)) {
            trieint_t idxval = newtop;
            new_idx = tamt_setitem(new_idx, hkey, &idxval, noop_incref, noop_decref);
         } else {
            void* ep; SetEntry patched;
            trieint_t tailidx = *(trieint_t*)found;
            fat_lookup(new_els, tailidx, &ep);
            memcpy(&patched, ep, sizeof(SetEntry));
            patched.next = newtop;
            new_els = tfat_setitem(new_els, tailidx, &patched,
                                    setentry_incref, setentry_decref);
         }
      } else {
         void* ep; SetEntry patched;
         fat_lookup(new_els, prev, &ep);
         memcpy(&patched, ep, sizeof(SetEntry));
         patched.next = newtop;
         new_els = tfat_setitem(new_els, prev, &patched,
                                 setentry_incref, setentry_decref);
      }
      newentry.key = e->key;
      newentry.next = SET_NO_NEXT;
      new_els = tfat_setitem(new_els, newtop, &newentry,
                              setentry_incref, setentry_decref);
      ++newtop;
   }
   *out_els = new_els;
   *out_idx = new_idx;
   *out_top = (Py_ssize_t)newtop;
   return 0;
}

static int set_should_compact(Py_ssize_t count, Py_ssize_t ndeleted) {
   if (ndeleted <= 0) return 0;
   if (ndeleted > SET_COMPACT_ABS_THRESHOLD) return 1;
   return ndeleted * SET_COMPACT_FRAC_DEN > count * SET_COMPACT_FRAC_NUM;
}

static PyObject* pset_new_dispatch(PyObject* arg);

static PyObject* pset_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   (void)type;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "pset() takes no keyword arguments");
      return NULL;
   }
   if (n == 0) {
      Py_INCREF(g_pset_empty);
      return (PyObject*)g_pset_empty;
   } else if (n == 1) {
      return pset_new_dispatch(PyTuple_GET_ITEM(args, 0));
   } else {
      PyErr_Format(PyExc_TypeError,
                    "pset expects at most 1 argument, got %zd", n);
      return NULL;
   }
}

static Py_hash_t pset_hash(PSetObject* self) {
   PyObject* fs;
   PyObject* h;
   Py_hash_t result;
   TriePath iter;
   int ok;
   if (self->hashcode != -1) return self->hashcode;
   fs = PySet_New(NULL);
   if (!fs) return -1;
   for (ok = fat_firstpath(self->els, &iter); ok; ok = fat_nextpath(&iter)) {
      SetEntry* e = (SetEntry*)triepath_val(&iter);
      int r;
      if (setentry_is_tombstone(e)) continue;
      r = PySet_Add(fs, e->key);
      if (r < 0) { Py_DECREF(fs); return -1; }
   }
   h = PyFrozenSet_New(fs);
   Py_DECREF(fs);
   if (!h) return -1;
   result = PyObject_Hash(h);
   Py_DECREF(h);
   if (result == -1) return -1;
   // Matches PersistentSet.__hash__'s `hash(frozenset(self)) + 1` exactly
   // (dict.c's analogous pdict_hash uses +2, matching PersistentMapping's
   // own convention -- these offsets exist upstream so that, e.g., an empty
   // pset and an empty pdict don't collide with frozenset() itself).
   result += 1;
   if (result == -1) result = -2;
   self->hashcode = result;
   return result;
}

static int pset_contains(PSetObject* self, PyObject* el) {
   trieint_t hkey;
   if (set_hash_key(el, &hkey) < 0) return -1;
   return set_chain_find(self->els, self->idx, hkey, el, NULL, NULL);
}

static PyObject* pset_add(PSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_top = self->top, new_count = self->count;
   Py_ssize_t new_ndeleted = self->ndeleted;
   if (set_hash_key(obj, &hkey) < 0) return NULL;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev);
   if (found < 0) return NULL;
   if (found) {
      Py_INCREF(self);
      return (PyObject*)self;
   }
   {
      SetEntry entry;
      entry.key = obj; entry.next = SET_NO_NEXT;
      new_els = fat_anditem(self->els, (trieint_t)new_top, &entry, setentry_incref);
      if (prev == SET_NO_NEXT) {
         trieint_t idxval = (trieint_t)new_top;
         new_idx = amt_anditem(self->idx, hkey, &idxval, noop_incref);
      } else {
         void* ep; SetEntry patched;
         Trie_t prev_els;
         fat_lookup(self->els, prev, &ep);
         memcpy(&patched, ep, sizeof(SetEntry));
         patched.next = (trieint_t)new_top;
         // fat_anditem() is non-consuming (leaves its input tree untouched
         // and independently valid), so the intermediate `prev_els` result
         // from the append above must be explicitly decref'd once we're
         // done reading from it -- see dict.c's pdict_set() for the
         // identical concern.
         prev_els = new_els;
         new_els = fat_anditem(prev_els, prev, &patched, setentry_incref);
         fatnode_decref(prev_els, setentry_decref);
         trienode_incref(self->idx);
         new_idx = self->idx;
      }
      new_top += 1;
      new_count += 1;
   }
   if (set_should_compact(new_count, new_ndeleted)) {
      Trie_t c_els, c_idx; Py_ssize_t c_top;
      if (set_rebuild_compacted(new_els, new_idx, &c_els, &c_idx, &c_top) < 0) {
         fatnode_decref(new_els, setentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return NULL;
      }
      fat_freeze(c_els);
      amt_freeze(c_idx);
      fatnode_decref(new_els, setentry_decref);
      amtnode_decref(new_idx, noop_decref);
      new_els = c_els; new_idx = c_idx; new_top = c_top; new_ndeleted = 0;
   }
   return pset_wrap(new_els, new_idx, new_top, new_count, new_ndeleted);
}

static PyObject* pset_discard(PSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_count, new_ndeleted;
   if (set_hash_key(obj, &hkey) < 0) return NULL;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev);
   if (found < 0) return NULL;
   if (!found) {
      Py_INCREF(self);
      return (PyObject*)self;
   }
   {
      void* ep; SetEntry entry; SetEntry tombstone;
      fat_lookup(self->els, found_index, &ep);
      memcpy(&entry, ep, sizeof(SetEntry));
      set_make_tombstone(&tombstone);
      if (prev == SET_NO_NEXT) {
         // Removing the head of the chain.
         if (entry.next == SET_NO_NEXT) {
            new_idx = amt_butitem(self->idx, hkey, noop_incref);
         } else {
            trieint_t idxval = entry.next;
            new_idx = amt_anditem(self->idx, hkey, &idxval, noop_incref);
         }
      } else {
         void* pp; SetEntry pentry;
         Trie_t tmp_els;
         fat_lookup(self->els, prev, &pp);
         memcpy(&pentry, pp, sizeof(SetEntry));
         pentry.next = entry.next;
         trienode_incref(self->idx);
         new_idx = self->idx;
         // OVERWRITE found_index's slot with a tombstone via fat_anditem()
         // -- must never be fat_butitem(), which would shift every later
         // index down by one; see the tombstone comment near
         // setentry_is_tombstone() at the top of this file.
         tmp_els = fat_anditem(self->els, prev, &pentry, setentry_incref);
         new_els = fat_anditem(tmp_els, found_index, &tombstone, setentry_incref);
         fatnode_decref(tmp_els, setentry_decref);
         goto have_new_els;
      }
      new_els = fat_anditem(self->els, found_index, &tombstone, setentry_incref);
   }
have_new_els:
   new_count = self->count - 1;
   new_ndeleted = self->ndeleted + 1;
   if (set_should_compact(new_count, new_ndeleted)) {
      Trie_t c_els, c_idx; Py_ssize_t c_top;
      if (set_rebuild_compacted(new_els, new_idx, &c_els, &c_idx, &c_top) < 0) {
         fatnode_decref(new_els, setentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return NULL;
      }
      fat_freeze(c_els);
      amt_freeze(c_idx);
      fatnode_decref(new_els, setentry_decref);
      amtnode_decref(new_idx, noop_decref);
      return pset_wrap(c_els, c_idx, c_top, new_count, 0);
   }
   return pset_wrap(new_els, new_idx, self->top, new_count, new_ndeleted);
}

static PyObject* pset_clear_method(PSetObject* self, PyObject* Py_UNUSED(ignored)) {
   (void)self;
   Py_INCREF(g_pset_empty);
   return (PyObject*)g_pset_empty;
}

static PyObject* pset_transient(PSetObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pset_iter(PSetObject* self);

// Matches abc/_set.py's PersistentSet.__repr__/__str__: both use the
// "{|...|}" delimiter (same shape as PersistentMapping's -- distinguished
// only by content, since seqstr on a non-Mapping just joins repr()s with no
// ": "), __str__ truncated at 60 chars, __repr__ not.
static PyObject* pset_repr(PSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* pset_str(PSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}

static PyMethodDef pset_methods[] = {
   {"add", (PyCFunction)pset_add, METH_O,
    "Returns a copy of the pset that includes the given object."},
   {"discard", (PyCFunction)pset_discard, METH_O,
    "Returns a copy of the pset that does not include the given object."},
   {"clear", (PyCFunction)pset_clear_method, METH_NOARGS,
    "Returns the empty pset."},
   {"transient", (PyCFunction)pset_transient, METH_NOARGS,
    "Efficiently copies the pset into a tset and returns the tset."},
   {NULL, NULL, 0, NULL}
};

static PyType_Slot pset_slots[] = {
   {Py_tp_dealloc, (void*)pset_dealloc},
   {Py_tp_repr, (void*)pset_repr},
   {Py_tp_str, (void*)pset_str},
   {Py_sq_length, (void*)pset_length},
   {Py_sq_contains, (void*)pset_contains},
   {Py_tp_hash, (void*)pset_hash},
   {Py_tp_doc,
    (void*)"A persistent set type similar to `set`, preserving insertion"
           " order, backed by an AMT hash table and a FAT element table."},
   {Py_tp_traverse, (void*)pset_traverse},
   {Py_tp_clear, (void*)pset_clear},
   {Py_tp_iter, (void*)pset_iter},
   {Py_tp_methods, (void*)pset_methods},
   {Py_tp_new, (void*)pset_new},
   {0, NULL}
};
static PyType_Spec pset_spec = {
   .name = "pcollections._c.set.pset",
   .basicsize = sizeof(PSetObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = pset_slots,
};


//=============================================================================
// tset

typedef struct {
   PyObject_HEAD
   Trie_t idx;            // AMT: hash -> first FAT index. May be transient.
   Trie_t els;             // FAT: index -> SetEntry. May be transient.
   Py_ssize_t top;
   Py_ssize_t count;
   Py_ssize_t ndeleted;
   PyObject* orig;          // cached pset (owned ref), or NULL -- see
                            // TDictObject's matching field in dict.c for the
                            // exact caching/invalidation convention.
} TSetObject;

static int tset_traverse(TSetObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->orig);
   return set_gc_traverse(self->els, visit, arg);
}
static int tset_clear(TSetObject* self) {
   Trie_t els = self->els, idx = self->idx;
   PyObject* orig = self->orig;
   self->els = NULL; self->idx = NULL; self->orig = NULL;
   if (els) fatnode_decref(els, setentry_decref);
   if (idx) amtnode_decref(idx, noop_decref);
   Py_XDECREF(orig);
   return 0;
}
static void tset_dealloc(TSetObject* self) {
   PyObject_GC_UnTrack(self);
   tset_clear(self);
   PyObject_GC_Del(self);
}
static Py_ssize_t tset_length(TSetObject* self) {
   return self->count;
}
static PyObject* tset_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                            Py_ssize_t count, Py_ssize_t ndeleted,
                            PyObject* orig) {
   TSetObject* self = PyObject_GC_New(TSetObject, TSetType);
   if (!self) {
      fatnode_decref(els, setentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_XDECREF(orig);
      return NULL;
   }
   self->els = els; self->idx = idx;
   self->top = top; self->count = count; self->ndeleted = ndeleted;
   self->orig = orig;
   PyObject_GC_Track(self);
   return (PyObject*)self;
}
static void tset_invalidate_orig(TSetObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

static PyObject* tset_empty(void) {
   return tset_wrap(fat_empty(SETELSLEAFSIZE), amt_empty(SETIDXLEAFSIZE), 0, 0, 0, NULL);
}

// Maybe-compact a tset in place after a mutation -- mirrors dict.c's
// tdict_maybe_compact() exactly.
static int tset_maybe_compact(TSetObject* self) {
   Trie_t c_els, c_idx; Py_ssize_t c_top;
   if (!set_should_compact(self->count, self->ndeleted)) return 0;
   if (set_rebuild_compacted(self->els, self->idx, &c_els, &c_idx, &c_top) < 0)
      return -1;
   fatnode_decref(self->els, setentry_decref);
   amtnode_decref(self->idx, noop_decref);
   self->els = c_els; self->idx = c_idx; self->top = c_top; self->ndeleted = 0;
   return 0;
}

// The shared insertion primitive behind both tset.add() and the general
// iterable-builder below. Returns 0 on success (including the "already
// present" no-op case) or -1 (with an exception set) on failure.
static int tset_add_element(TSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   if (set_hash_key(obj, &hkey) < 0) return -1;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev);
   if (found < 0) return -1;
   if (found) return 0;
   {
      SetEntry entry;
      entry.key = obj; entry.next = SET_NO_NEXT;
      self->els = tfat_setitem(self->els, (trieint_t)self->top, &entry,
                                setentry_incref, setentry_decref);
      if (prev == SET_NO_NEXT) {
         trieint_t idxval = (trieint_t)self->top;
         self->idx = tamt_setitem(self->idx, hkey, &idxval, noop_incref, noop_decref);
      } else {
         void* ep; SetEntry patched;
         fat_lookup(self->els, prev, &ep);
         memcpy(&patched, ep, sizeof(SetEntry));
         patched.next = (trieint_t)self->top;
         self->els = tfat_setitem(self->els, prev, &patched,
                                   setentry_incref, setentry_decref);
      }
   }
   self->top += 1;
   self->count += 1;
   tset_invalidate_orig(self);
   if (tset_maybe_compact(self) < 0) return -1;
   return 0;
}

static PyObject* tset_add(TSetObject* self, PyObject* obj) {
   if (tset_add_element(self, obj) < 0) return NULL;
   Py_RETURN_NONE;
}

static PyObject* tset_discard(TSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   if (set_hash_key(obj, &hkey) < 0) return NULL;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev);
   if (found < 0) return NULL;
   if (!found) Py_RETURN_NONE;
   {
      void* ep; SetEntry entry; SetEntry tombstone;
      fat_lookup(self->els, found_index, &ep);
      memcpy(&entry, ep, sizeof(SetEntry));
      set_make_tombstone(&tombstone);
      if (prev == SET_NO_NEXT) {
         if (entry.next == SET_NO_NEXT)
            self->idx = tamt_delitem(self->idx, hkey, noop_incref, noop_decref);
         else {
            trieint_t idxval = entry.next;
            self->idx = tamt_setitem(self->idx, hkey, &idxval, noop_incref, noop_decref);
         }
      } else {
         void* pp; SetEntry pentry;
         fat_lookup(self->els, prev, &pp);
         memcpy(&pentry, pp, sizeof(SetEntry));
         pentry.next = entry.next;
         self->els = tfat_setitem(self->els, prev, &pentry,
                                   setentry_incref, setentry_decref);
      }
      // OVERWRITE found_index's slot with a tombstone via tfat_setitem() --
      // must never be tfat_delitem(), which would shift every later index
      // down by one; see the tombstone comment near setentry_is_tombstone()
      // at the top of this file.
      self->els = tfat_setitem(self->els, found_index, &tombstone,
                                setentry_incref, setentry_decref);
   }
   self->count -= 1;
   self->ndeleted += 1;
   tset_invalidate_orig(self);
   if (tset_maybe_compact(self) < 0) return NULL;
   Py_RETURN_NONE;
}

static int tset_contains(TSetObject* self, PyObject* el) {
   trieint_t hkey;
   if (set_hash_key(el, &hkey) < 0) return -1;
   return set_chain_find(self->els, self->idx, hkey, el, NULL, NULL);
}

// Builds a fresh, freshly-populated tset out of a general Python iterable of
// elements. Mirrors tset.__new__'s general-argument fallback in _set.py
// (`t = cls.empty(); t.addall(arg); return t`).
static PyObject* tset_build_from_arg(PyObject* arg) {
   PyObject* obj = tset_empty();
   PyObject* iterator;
   PyObject* item;
   if (!obj) return NULL;
   iterator = PyObject_GetIter(arg);
   if (!iterator) { Py_DECREF(obj); return NULL; }
   while ((item = PyIter_Next(iterator))) {
      int r = tset_add_element((TSetObject*)obj, item);
      Py_DECREF(item);
      if (r < 0) { Py_DECREF(iterator); Py_DECREF(obj); return NULL; }
   }
   Py_DECREF(iterator);
   if (PyErr_Occurred()) { Py_DECREF(obj); return NULL; }
   return obj;
}

// Forward-declared: implemented after PSetType/pset_transient exist (the
// `type(arg) is pset` case below routes through pset_transient()).
static PyObject* pset_transient(PSetObject* self, PyObject* Py_UNUSED(ignored));

static PyObject* tset_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* arg;
   (void)type;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "tset() takes no keyword arguments");
      return NULL;
   }
   if (n == 0) return tset_empty();
   if (n != 1) {
      PyErr_Format(PyExc_TypeError,
                    "tset expects at most 1 argument, got %zd", n);
      return NULL;
   }
   arg = PyTuple_GET_ITEM(args, 0);
   if (Py_TYPE(arg) == TSetType) {
      // tset(some_tset): share a frozen snapshot of that tset's current
      // (els, idx) -- not the tset's own live, still-mutable trees -- as
      // the starting point for a brand-new, independent transient session.
      // Also propagates `orig`, exactly mirroring dict.c's tdict_new().
      // (Note: the reference _set.py's tset.__new__ doesn't special-case a
      // tset argument at all -- it just falls through to addall(), an O(n)
      // rebuild -- but dict.c's tdict.__new__ DOES have this fast path,
      // matching _dict.py's own tdict.__new__ exactly. Adding it here too
      // keeps pset/tset "mostly identical to the dict types" as asked.)
      TSetObject* t = (TSetObject*)arg;
      Trie_t els = t->els, idx = t->idx;
      PyObject* orig = t->orig;
      fat_freeze(els);
      amt_freeze(idx);
      trienode_incref(els);
      trienode_incref(idx);
      Py_XINCREF(orig);
      return tset_wrap(els, idx, t->top, t->count, t->ndeleted, orig);
   }
   if (Py_TYPE(arg) == PSetType) {
      // tset(some_pset): exactly pset.transient()'s own O(1) sharing logic.
      return pset_transient((PSetObject*)arg, NULL);
   }
   return tset_build_from_arg(arg);
}

static PyObject* tset_persistent(TSetObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t els, idx;
   if (self->count == 0) {
      Py_INCREF(g_pset_empty);
      return (PyObject*)g_pset_empty;
   }
   if (self->orig) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   els = self->els; idx = self->idx;
   fat_freeze(els);
   amt_freeze(idx);
   trienode_incref(els);
   trienode_incref(idx);
   return pset_wrap(els, idx, self->top, self->count, self->ndeleted);
}

static PyObject* pset_transient(PSetObject* self, PyObject* Py_UNUSED(ignored)) {
   trienode_incref(self->els);
   trienode_incref(self->idx);
   // tset_wrap()'s `orig` parameter takes ownership of (does not itself
   // incref) the reference it's handed -- self must be incref'd here,
   // exactly as pdict_transient() does in dict.c, or the returned tset ends
   // up holding an unowned pointer to self (a use-after-free/double-free on
   // `self` once the tset is later deallocated).
   Py_INCREF(self);
   return tset_wrap(self->els, self->idx, self->top, self->count,
                     self->ndeleted, (PyObject*)self);
}

static PyObject* tset_clear_method(TSetObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t els = self->els, idx = self->idx;
   PyObject* orig = self->orig;
   self->els = fat_empty(SETELSLEAFSIZE);
   self->idx = amt_empty(SETIDXLEAFSIZE);
   self->top = 0; self->count = 0; self->ndeleted = 0;
   self->orig = NULL;
   fatnode_decref(els, setentry_decref);
   amtnode_decref(idx, noop_decref);
   Py_XDECREF(orig);
   Py_RETURN_NONE;
}

static PyObject* tset_iter(TSetObject* self);

// Matches abc/_set.py's TransientSet.__repr__/__str__: both use the
// "{<...>}" delimiter (no repr/str asymmetry here, unlike TransientMapping
// in dict.c).
static PyObject* tset_repr(TSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* tset_str(TSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}

// Matches abc/_set.py's tset.empty being a *classmethod* (same pattern as
// tdict_empty_classmethod in dict.c). pset/tset don't support subclassing
// (no Py_TPFLAGS_BASETYPE on their specs), so `cls` here is always exactly
// TSetType and this is equivalent to just calling tset_empty() -- but it's
// written to take `cls` anyway for parity with tdict/tlist's classmethod
// signature. Previously unexposed to Python at all.
static PyObject* tset_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   (void)cls;
   return tset_empty();
}

static PyMethodDef tset_methods[] = {
   {"empty", (PyCFunction)tset_empty_classmethod, METH_NOARGS | METH_CLASS,
    "Returns a new, empty tset."},
   {"add", (PyCFunction)tset_add, METH_O,
    "Adds the given object to the tset."},
   {"discard", (PyCFunction)tset_discard, METH_O,
    "Discards the given object from the tset."},
   {"clear", (PyCFunction)tset_clear_method, METH_NOARGS,
    "Clears all elements from the tset."},
   {"persistent", (PyCFunction)tset_persistent, METH_NOARGS,
    "Efficiently copies the tset into a pset and returns the pset."},
   {NULL, NULL, 0, NULL}
};

static PyType_Slot tset_slots[] = {
   {Py_tp_dealloc, (void*)tset_dealloc},
   {Py_tp_repr, (void*)tset_repr},
   {Py_tp_str, (void*)tset_str},
   {Py_sq_length, (void*)tset_length},
   {Py_sq_contains, (void*)tset_contains},
   {Py_tp_doc,
    (void*)"A transient (mutable) set type similar to `set`, preserving"
           " insertion order, backed by an AMT hash table and a FAT element"
           " table."},
   {Py_tp_traverse, (void*)tset_traverse},
   {Py_tp_clear, (void*)tset_clear},
   {Py_tp_iter, (void*)tset_iter},
   {Py_tp_methods, (void*)tset_methods},
   {Py_tp_new, (void*)tset_new},
   {0, NULL}
};
static PyType_Spec tset_spec = {
   .name = "pcollections._c.set.tset",
   .basicsize = sizeof(TSetObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = tset_slots,
};

// pset_new_dispatch()'s general-argument routing is defined down here, now
// that tset_new()/tset_persistent() both exist -- mirrors dict.c's
// pdict_new_dispatch() exactly.
static PyObject* pset_new_dispatch(PyObject* arg) {
   PyObject* t;
   PyObject* result;
   if (Py_TYPE(arg) == TSetType) {
      // pset(some_tset): efficiently take a persistent snapshot -- reuses
      // tset_persistent() directly (rather than duplicating its `orig`-
      // cache-check logic) so the cache benefit applies here too, which is
      // actually an improvement on _set.py's own pset.__new__ (which
      // bypasses `tset.persistent()`'s cache and always rebuilds).
      return tset_persistent((TSetObject*)arg, NULL);
   }
   if (Py_TYPE(arg) == PSetType) {
      // pset(some_pset): already the right (immutable) type -- psets are
      // never subclassed (Py_TPFLAGS_BASETYPE is unset), so this is always
      // exactly a no-op copy.
      Py_INCREF(arg);
      return arg;
   }
   // General path: route through tset, then take a persistent snapshot.
   t = tset_build_from_arg(arg);
   if (!t) return NULL;
   result = tset_persistent((TSetObject*)t, NULL);
   Py_DECREF(t);
   return result;
}


//=============================================================================
// Iterator type. Both pset and tset walk `els` directly via fat_firstpath/
// fat_nextpath, skipping tombstones -- mirrors dict.c's DictIterObject,
// with no "mode" needed since sets only ever iterate their elements.

typedef struct {
   PyObject_HEAD
   PyObject* owner;    // strong ref to the pset/tset being walked -- keeps
                       // `els` alive for the duration.
   TriePath path;
   int state;          // 0 = not yet started, 1 = active, 2 = exhausted.
} SetIterObject;

static void setiter_dealloc(SetIterObject* self) {
   PyObject_GC_UnTrack(self);
   Py_XDECREF(self->owner);
   PyObject_GC_Del(self);
}
static int setiter_traverse(SetIterObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->owner);
   return 0;
}
static Trie_t setiter_owner_els(SetIterObject* self) {
   if (Py_TYPE(self->owner) == PSetType)
      return ((PSetObject*)self->owner)->els;
   else
      return ((TSetObject*)self->owner)->els;
}
static PyObject* setiter_next(SetIterObject* self) {
   int ok;
   SetEntry* e;
   if (self->state == 2) return NULL;
   if (self->state == 0) {
      ok = fat_firstpath(setiter_owner_els(self), &self->path);
      self->state = 1;
   } else {
      ok = fat_nextpath(&self->path);
   }
   while (ok && setentry_is_tombstone((SetEntry*)triepath_val(&self->path)))
      ok = fat_nextpath(&self->path);
   if (!ok) { self->state = 2; return NULL; }
   e = (SetEntry*)triepath_val(&self->path);
   Py_INCREF(e->key);
   return e->key;
}
static PyObject* setiter_self(PyObject* self) { Py_INCREF(self); return self; }

static PyObject* make_setiter(PyTypeObject* itertype, PyObject* owner_set) {
   SetIterObject* it = PyObject_GC_New(SetIterObject, itertype);
   if (!it) return NULL;
   Py_INCREF(owner_set);
   it->owner = owner_set;
   it->state = 0;
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

static PyTypeObject PSetIterType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.set.pset_iterator",
   .tp_basicsize = sizeof(SetIterObject),
   .tp_dealloc = (destructor)setiter_dealloc,
   .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .tp_traverse = (traverseproc)setiter_traverse,
   .tp_iter = setiter_self,
   .tp_iternext = (iternextfunc)setiter_next,
};
static PyTypeObject TSetIterType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.set.tset_iterator",
   .tp_basicsize = sizeof(SetIterObject),
   .tp_dealloc = (destructor)setiter_dealloc,
   .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .tp_traverse = (traverseproc)setiter_traverse,
   .tp_iter = setiter_self,
   .tp_iternext = (iternextfunc)setiter_next,
};

static PyObject* pset_iter(PSetObject* self) {
   return make_setiter(&PSetIterType, (PyObject*)self);
}
static PyObject* tset_iter(TSetObject* self) {
   return make_setiter(&TSetIterType, (PyObject*)self);
}


//=============================================================================
// Module definition.

static PyModuleDef set_module = {
   PyModuleDef_HEAD_INIT,
   "pcollections._c.set",
   "C implementations of pset and tset, backed by an AMT hash table and a"
   " FAT element table.",
   -1,
   NULL, NULL, NULL, NULL, NULL
};

// Builds the heap type described by `spec`, inheriting from the Python-level
// PersistentSet/TransientSet ABC mixin class named `base_name` (looked up on
// `abc_module`) -- see dict.c's build_abc_subtype() for the full rationale;
// duplicated here (rather than shared) since this is a separate translation
// unit/module. Returns a new reference, or NULL (with an exception set) on
// failure.
static PyTypeObject* build_abc_subtype(PyType_Spec* spec, PyObject* abc_module,
                                        const char* base_name) {
   PyObject* base;
   PyObject* bases;
   PyObject* result;
   PyTypeObject* base_t;
   PyTypeObject* result_t;
   base = PyObject_GetAttrString(abc_module, base_name);
   if (!base) return NULL;
   if (!PyType_Check(base)) {
      Py_DECREF(base);
      PyErr_Format(PyExc_TypeError, "pcollections.abc.%s is not a type",
                   base_name);
      return NULL;
   }
   base_t = (PyTypeObject*)base;
   bases = PyTuple_Pack(1, base);
   Py_DECREF(base);
   if (!bases) return NULL;
   result = PyType_FromSpecWithBases(spec, bases);
   Py_DECREF(bases);
   if (!result) return NULL;
   result_t = (PyTypeObject*)result;
   // pset/tset both rely on PersistentSet/TransientSet's own tp_richcompare
   // (== collections.abc.Set's __eq__/__lt__/etc.) being inherited, without
   // providing it natively. See dict.c's build_abc_subtype() for the full
   // explanation of why this MUST be a direct field assignment here, after
   // construction, rather than an entry in `spec`'s slots list: listing
   // Py_tp_richcompare there causes CPython to also synthesize a matching
   // __eq__/__ne__/... wrapper descriptor directly in this new type's own
   // tp_dict, and since the generic slot_tp_richcompare dispatcher looks up
   // __eq__ via the *instance's actual type* MRO, it finds that newly
   // synthesized wrapper first and calls straight back into tp_richcompare
   // -- infinite C-level recursion with no Python frames ever created.
   //
   // tp_hash is NOT touched here: pset provides its own native tp_hash
   // (pset_hash, via a real Py_tp_hash slot in pset_spec -- a concrete
   // function, not a generic MRO-dispatching one, so it's in no danger of
   // this same self-reference loop and must not be overwritten), while tset
   // provides none of its own and needs TransientSet's tp_hash patched in
   // separately -- see PyInit_set()'s explicit fixup for that, right after
   // this function builds TSetType. NOTE (confirmed by reading abc/_set.py
   // directly): unlike TransientMapping -- which relies on MutableMapping's
   // `__hash__ = None` to stay unhashable, exactly like a plain dict --
   // TransientSet.__hash__ is a real, concrete function (the same
   // `hash(frozenset(self)) + 1` formula as PersistentSet.__hash__, fully
   // generic over just __iter__). So this patch actually makes tset
   // *hashable*, with a hash equal to the corresponding pset's -- that
   // asymmetry with tdict is intentional in the given Python reference, not
   // a bug here, and set_unit.py has a regression test pinning it down.
   result_t->tp_richcompare = base_t->tp_richcompare;
   return result_t;
}

// Registers `concrete` as a virtual subclass (via .register()) of the type
// named `attr_name` on `module`. See dict.c's twin of this function for the
// full rationale (isinstance()/issubclass() compatibility after pset/tset
// switched from inheriting PersistentSet/TransientSet directly to inheriting
// their plain, non-ABCMeta _PersistentSetBase/_TransientSetBase mixins --
// necessary on CPython 3.14, see pcollections.abc._core's _PersistentBase
// docstring). Returns 0 on success, -1 (with an exception set) on failure.
static int register_virtual_subclass(PyObject* module, const char* attr_name,
                                      PyTypeObject* concrete) {
   PyObject* abc_cls;
   PyObject* result;
   abc_cls = PyObject_GetAttrString(module, attr_name);
   if (!abc_cls) return -1;
   result = PyObject_CallMethod(abc_cls, "register", "O", (PyObject*)concrete);
   Py_DECREF(abc_cls);
   if (!result) return -1;
   Py_DECREF(result);
   return 0;
}

PyMODINIT_FUNC PyInit_set(void) {
   PyObject* m;
   PyObject* pcoll_abc_module;
   PSetObject* empty;

   // The tombstone sentinel -- see the comment near setentry_is_tombstone()
   // above. A plain object() instance works fine: it's never exposed to
   // Python code and is compared only by identity.
   g_set_dummy = PyObject_CallObject((PyObject*)&PyBaseObject_Type, NULL);
   if (!g_set_dummy) return NULL;

   // pset/tset are built on top of pcollections.abc's plain (non-ABCMeta)
   // _PersistentSetBase/_TransientSetBase mixins, then .register()'ed as
   // virtual subclasses of the real PersistentSet/TransientSet -- see
   // dict.c's PyInit_dict() for the fuller version of this same comment;
   // the CPython 3.14 rationale is identical here.
   pcoll_abc_module = PyImport_ImportModule("pcollections.abc");
   if (!pcoll_abc_module) return NULL;
   PSetType = build_abc_subtype(&pset_spec, pcoll_abc_module, "_PersistentSetBase");
   TSetType = build_abc_subtype(&tset_spec, pcoll_abc_module, "_TransientSetBase");
   if (!PSetType || !TSetType) { Py_DECREF(pcoll_abc_module); return NULL; }
   if (register_virtual_subclass(pcoll_abc_module, "PersistentSet", PSetType) < 0 ||
       register_virtual_subclass(pcoll_abc_module, "TransientSet", TSetType) < 0) {
      Py_DECREF(pcoll_abc_module); return NULL;
   }
   Py_DECREF(pcoll_abc_module);
   // tset provides no tp_hash of its own at all (unlike pset) -- patch in
   // TransientSet's own tp_hash directly, the same safe way
   // build_abc_subtype() patches tp_richcompare (a concrete function, not a
   // generic MRO-dispatching one, so this isn't at risk of that
   // self-reference loop, but going through spec slots isn't needed either
   // way). Note this makes tset *hashable* (see the longer comment in
   // build_abc_subtype() above) -- TransientSet.__hash__ is a real function
   // in abc/_set.py, not the `None` that MutableMapping-style unhashability
   // would give it.
   TSetType->tp_hash = ((PyTypeObject*)TSetType)->tp_base->tp_hash;

   {
      PyObject* util_module = PyImport_ImportModule("pcollections.util");
      if (!util_module) return NULL;
      g_seqstr = PyObject_GetAttrString(util_module, "seqstr");
      Py_DECREF(util_module);
      if (!g_seqstr) return NULL;
   }

   if (PyType_Ready(&PSetIterType) < 0) return NULL;
   if (PyType_Ready(&TSetIterType) < 0) return NULL;

   // Build the canonical empty pset singleton by hand (pset_wrap() itself
   // depends on it already existing, so it can't be used here).
   empty = PyObject_GC_New(PSetObject, PSetType);
   if (!empty) return NULL;
   empty->els = fat_empty(SETELSLEAFSIZE);
   empty->idx = amt_empty(SETIDXLEAFSIZE);
   empty->top = 0;
   empty->count = 0;
   empty->ndeleted = 0;
   empty->hashcode = -1;
   PyObject_GC_Track(empty);
   g_pset_empty = empty;

   if (PyDict_SetItemString(PSetType->tp_dict, "empty",
                            (PyObject*)g_pset_empty) < 0)
      return NULL;
   PyType_Modified(PSetType);

   m = PyModule_Create(&set_module);
   if (!m) return NULL;

   Py_INCREF(PSetType);
   if (PyModule_AddObject(m, "pset", (PyObject*)PSetType) < 0) {
      Py_DECREF(PSetType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TSetType);
   if (PyModule_AddObject(m, "tset", (PyObject*)TSetType) < 0) {
      Py_DECREF(TSetType); Py_DECREF(m); return NULL;
   }
   return m;
}
