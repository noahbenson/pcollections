///////////////////////////////////////////////////////////////////////////////
// _c/dict.c
// The persistent (pdict) and transient (tdict) dict types, implemented as
// thin CPython wrappers around a pair of tries: an AMT used as a hash table
// (hash(key) -> index) and a FAT used as an insertion-ordered value table
// (index -> (key, value, next_index)).
//
// Design (mirrors pcollections/_dict.py's PHAMT/THAMT-based reference, but
// swaps in our own AMT/FAT tries and, unlike the reference, actively
// compacts away holes left by deletions -- see "Compaction" below):
//
//  - `els` (a FAT) is the insertion-ordered value table. Its keys are dense
//    integers 0, 1, 2, ..., `top`-1 (assigned in insertion order, exactly
//    like plist/tlist's own FAT-tree list encoding in list.c -- except a
//    dict's `top` only ever grows via new insertions, so unlike a list's
//    `start` there is no prepend-style operation and therefore no need for
//    list.c's LIST_START_MID wraparound trick: index 0 is always safe).
//    Each leaf is a `DictEntry` (see below): the (key, value) pair plus the
//    FAT index of the *next* entry sharing the same hash (or DICT_NO_NEXT),
//    forming a singly-linked collision chain. Iterating `els` in ascending
//    index order (fat_firstpath/fat_nextpath) therefore visits entries in
//    insertion order, exactly like a modern Python dict.
//  - `idx` (an AMT) maps hash(key) -> the FAT index of the *first* entry in
//    that hash's collision chain. A lookup hashes the key, looks the hash up
//    in `idx` to find the chain head, then walks `els`' linked list from
//    there comparing keys with `==` until it finds an exact match (or runs
//    off the end of the chain). AMT (rather than FAT) is used for `idx`
//    because hash values are effectively random/scattered, which is exactly
//    AMT's strength, whereas `els`' dense sequential indices are exactly
//    FAT's strength. `idx`'s leaves are raw `trieint_t` FAT indices, not
//    PyObject*s -- there is nothing to incref/decref there (see
//    noop_incref/noop_decref below).
//  - A key is looked up by its FAT index only through the collision chain
//    (`idx` never stores anything but the chain *head*), matching the
//    reference implementation's PHAMT-based scheme exactly.
//
// Compaction: deleting an entry patches up the collision chain around it
// (repointing `idx` or the previous link's `.next`, exactly as a real
// removal would) but does NOT actually remove it from `els` -- `els` is a
// FAT, which (like list.c's plist/tlist encoding) maintains a strictly
// *dense* invariant: removing a key from the middle would shift every
// later index down by one to close the gap, silently invalidating every
// `idx` entry and collision-chain `.next` pointer at or past that index.
// So a deleted entry's slot is instead *overwritten* with a tombstone (see
// dictentry_is_tombstone() below) -- physically present in `els` but
// unreachable from `idx`, burning a slot that direct `els`-walking code
// (dict_rebuild_compacted(), pdict_repr()/tdict_repr(), pdict_hash(),
// dictiter_next()) knows to skip. `top` (the next fresh index to hand out)
// is never decremented, so indices are *not* reused between compactions --
// exactly mirroring the reference's `_top`. Left unchecked, a dict that
// churns through many set/drop cycles would see `top` grow without bound
// even though `count` (the live element count) stays small, wasting FAT
// tree depth and, since FAT's own depth is a function of the *spread*
// between its lowest and highest live key, real memory. So: every mutating
// operation checks, after it completes, whether `ndeleted` (the number of
// indices burned by deletions since the last compaction) exceeds 1024**2 or
// 30% of `count` (the live element count) -- and if so, rebuilds `els`/`idx`
// from scratch with the live entries renumbered 0..count-1 in their
// original (insertion) order, resetting `top = count` and `ndeleted = 0`.
// For a persistent dict this produces an entirely fresh pair of tries (the
// old ones are left untouched, as they must be -- other pdicts may still be
// sharing them); for a transient dict it happens in place.
//
// Every FAT node here has leafsize == sizeof(DictEntry); every AMT node has
// leafsize == sizeof(trieint_t).
//
// Scope note: pdict/tdict implement the "must implement" primitives of
// PersistentMapping/TransientMapping (see pcollections/abc/_map.py) natively
// in C -- set/drop/clear/transient/__len__/__iter__/__getitem__ for pdict;
// __setitem__/__delitem__/clear/persistent/__len__/__iter__/__getitem__ for
// tdict -- plus __hash__ (pdict only; the inherited default needs an `_els`
// attribute we don't have) and keys()/items()/values() (returning our own
// view types, exactly as the reference's concrete pdict/tdict classes do,
// rather than collections.abc.Mapping's generic KeysView/ItemsView/
// ValuesView). Everything else -- get/setdefault/popitem/pop/copy/update/
// setall/dropall/deleteall/discardall/removeall/__reduce__/__json__/
// __eq__/__ne__/__contains__/__str__ -- is inherited for free from
// PersistentMapping/TransientMapping (which in turn get __eq__/__contains__
// from collections.abc.Mapping), via the same PyType_FromSpecWithBases
// heap-type technique list.c uses for plist/tlist. Unlike plist/tlist,
// pdict/tdict *are* subclassable in C (Py_TPFLAGS_BASETYPE is set on both --
// see pdict_spec's comment): pcollections._c.lazy's ldict/tldict subclass
// them directly, exactly mirroring the reference _lazy.py's
// `class ldict(pdict)`/`class tldict(tdict)`. Every construction site in
// this file that the reference spells as `self._new(...)`/`cls._new(...)`
// (rather than naming `pdict`/`tdict` explicitly) is written here in terms
// of `Py_TYPE(self)`/a threaded-through `type`/`cls` parameter instead of a
// hardcoded PDictType/TDictType, so that e.g. `some_ldict.set(...)` returns
// another ldict rather than silently downgrading to a plain pdict on first
// mutation -- see pdict_wrap_astype()/tdict_wrap_astype()/
// pdict_new_dispatch()/tdict_new() for where this actually happens.


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

// els (the FAT value table) stores DictEntry leaves.
typedef struct {
   PyObject* key;
   PyObject* val;
   trieint_t next;   // FAT index of the next entry sharing this key's hash,
                      // or DICT_NO_NEXT if this is the last link.
} DictEntry;
#define ELSLEAFSIZE ((uint8_t)sizeof(DictEntry))
// idx (the AMT hash table) stores raw trieint_t FAT-index leaves.
#define IDXLEAFSIZE ((uint8_t)sizeof(trieint_t))
// Sentinel meaning "no next entry in this hash's collision chain". Every
// realistic dict is nowhere near SIZE_MAX live insertions, so this can never
// collide with a real index.
#define DICT_NO_NEXT (~(trieint_t)0)

// Compaction thresholds -- see the file header comment above.
#define DICT_COMPACT_ABS_THRESHOLD ((Py_ssize_t)1024 * 1024)
#define DICT_COMPACT_FRAC_NUM 3
#define DICT_COMPACT_FRAC_DEN 10


//=============================================================================
// fat_empty()/amt_empty(): the real (non-test) definitions of the
// canonical-empty-node singletons that trie.h/amt.h/fat.h declare but
// deliberately don't define (each translation unit that uses these tries
// gets to decide how/whether to cache them) -- see list.c for the identical
// pattern and rationale. Indexed by leafsize for robustness; in practice
// this file only ever asks for ELSLEAFSIZE/IDXLEAFSIZE respectively.

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
      // bare amtnode_alloc() -- amtnode_alloc() only reserves the memory and
      // leaves the whole header (including leafsize!) uninitialized. An
      // early version of this function called amtnode_alloc() directly and
      // left header.leafsize as garbage, which amt_anditem()/amt_1leaf()
      // later read to size a memcpy -- caught immediately by ASAN as a
      // stack-buffer-overflow the first time pdict.set() was ever exercised.
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
// reachable from it as persistent. See list.c's fat_freeze() for the full
// rationale (needed so tdict.persistent() can be called mid-mutation-session
// and leave the tdict itself still usable afterward); amt_freeze() is the
// same operation for AMT nodes, needed because `idx` can accumulate
// transient-claimed nodes exactly like `els` can.

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

// els leaves (DictEntry) own a reference to both their key and value.
static void dictentry_incref(void* v) {
   DictEntry* e = (DictEntry*)v;
   Py_INCREF(e->key);
   Py_INCREF(e->val);
}
static void dictentry_decref(void* v) {
   DictEntry* e = (DictEntry*)v;
   Py_DECREF(e->key);
   Py_DECREF(e->val);
}
// idx leaves are raw FAT indices -- nothing to refcount.
static void noop_incref(void* v) { (void)v; }
static void noop_decref(void* v) { (void)v; }

//=============================================================================
// Tombstones.
//
// IMPORTANT correction to the design-comment at the top of this file: `els`
// (a FAT) can NOT hold "holes" the way that comment originally assumed.
// FAT maintains a genuinely *dense* invariant -- exactly like a Python list
// (which is exactly what it's used for in list.c) -- so removing a key from
// the *middle* of it (via fat_butitem()/tfat_delitem()) does not just clear
// an occupancy bit: it closes the gap by shifting every subsequent index
// down by one, precisely the way plist.delete()/list.pop() must. That's
// perfect for a list, but catastrophic for `els`, since `idx` stores raw FAT
// indices as its leaves: the instant any index shifts, every `idx` entry
// (and every DictEntry.next collision-chain pointer) pointing at or past
// that index is silently pointing at the wrong slot (or past the end
// entirely) -- confirmed via a dedicated (temporary, since-removed) debug
// walk that reproduced the exact symptom: `idx` recording hash(1) -> FAT
// index 1 immediately after a drop() that had shifted key 1's real entry
// down to index 0.
//
// The fix: never call fat_butitem()/tfat_delitem() on `els` for a plain
// deletion. Instead, first unlink the doomed entry from its collision chain
// exactly as before (patching `idx` or the previous link's `.next`), then
// *overwrite* (not remove) its slot with a tombstone via fat_anditem()/
// tfat_setitem() -- an in-place value replacement, which (unlike deletion)
// never touches occupancy and therefore never shifts anything. The slot
// stays physically present in `els` (burning space, exactly what `ndeleted`
// already tracks) until compaction rebuilds `els`/`idx` from scratch and
// skips tombstones on the way -- see dict_rebuild_compacted() below.
//
// A tombstone is a DictEntry whose key/val both point at the single shared
// `g_dict_dummy` sentinel (an ordinary, otherwise-unreachable object created
// once at module-init time -- mirroring CPython's own dict/set "dummy"
// placeholder), which every direct `els`-walking loop (dict_rebuild_
// compacted(), pdict_repr()/tdict_repr(), pdict_hash(), dictiter_next())
// checks for via pointer identity and skips. A dict_chain_find() walk can
// never land on a tombstoned entry in the first place -- it's always fully
// unlinked from every collision chain before being tombstoned -- so that
// function (and anything built on it: __getitem__, __contains__, get(),
// set()'s/__setitem__'s "already present" check, etc.) needs no changes at
// all.
static PyObject* g_dict_dummy = NULL;

static int dictentry_is_tombstone(const DictEntry* e) {
   return e->key == g_dict_dummy;
}
static void dict_make_tombstone(DictEntry* out) {
   out->key = g_dict_dummy;
   out->val = g_dict_dummy;
   out->next = DICT_NO_NEXT;
}


//=============================================================================
// A small recursive GC traversal helper: visits every PyObject* (both the
// key and the value of every entry) reachable from an `els` FAT tree. `idx`
// never needs a traverse -- its leaves are plain integers, not references.
static int dict_gc_traverse(Trie_t node, visitproc visit, void* arg) {
   triebits_t bi;
   if (fatnode_is_twig(node)) {
      for (bi = trienode_first_bitindex(node); bi < FAT_CELLS;
           bi = trienode_next_bitindex(node, bi)) {
         DictEntry* e = (DictEntry*)trienode_leaf(node, fatnode_bit2cellindex(node, bi));
         Py_VISIT(e->key);
         Py_VISIT(e->val);
      }
   } else {
      for (bi = trienode_first_bitindex(node); bi < FAT_CELLS;
           bi = trienode_next_bitindex(node, bi)) {
         int r = dict_gc_traverse(trienode_subt(node, fatnode_bit2cellindex(node, bi)),
                                   visit, arg);
         if (r) return r;
      }
   }
   return 0;
}


//=============================================================================
// Hash-key conversion: Python's hash() returns a signed Py_hash_t; AMT keys
// are unsigned trieint_t. Since idx is a genuine hash table (not an ordered
// list like els), all we need is a bit-preserving reinterpretation -- there
// is no ordering or wraparound-headroom concern here at all (contrast
// list.c's LIST_START_MID, which exists only because FAT list keys *do*
// need to sort in a particular way).
static int dict_hash_key(PyObject* key, trieint_t* out) {
   Py_hash_t h = PyObject_Hash(key);
   if (h == -1 && PyErr_Occurred()) return -1;
   *out = (trieint_t)(size_t)h;
   return 0;
}


//=============================================================================
// Core chain-walking primitives shared by pdict and tdict. These operate
// purely on (els, idx) pairs and never decide for themselves whether that
// pair is the persistent or transient one -- callers pick the right
// mutation primitives (fat_anditem/amt_anditem for persistent,
// tfat_setitem/tamt_setitem for transient) around them.

// Looks up `key` (whose hash is already known) by walking its collision
// chain in `els`, starting from the chain head recorded in `idx`. In BOTH
// outcomes, *out_prev (if non-NULL) is set to the FAT index of the entry
// immediately *before* the relevant point in the chain -- DICT_NO_NEXT if
// there is no such entry (the hash isn't in `idx` at all, or the relevant
// entry is the chain's own head). On success (`key` found) returns 1, sets
// *out_index to the FAT index of the matching entry (*out_val, if non-NULL,
// to that entry's value, borrowed), and *out_prev to the FAT index of the
// entry immediately preceding *the match* in its chain -- exactly what
// pdict_drop()/tdict_ass_subscript()'s __delitem__ path need to splice the
// match back out. On "not found", returns 0 and *out_prev is set to the FAT
// index of the chain's *last* entry instead -- exactly what's needed to
// splice a brand new entry onto the end of the chain (or start a fresh one,
// if *out_prev comes back DICT_NO_NEXT). Returns -1 (no exception set --
// this never actually fails) only if defensive-programming paranoia demands
// a tri-state return; in practice this function cannot fail, so callers
// only ever see 0 or 1.
//
// NOTE: an earlier version of this function left *out_prev untouched on the
// found (return 1) path, on the theory that only pdict_set()'s "not found"
// insertion path ever needed it. That's wrong -- pdict_drop() and
// tdict_ass_subscript()'s __delitem__ path call this with `found` always
// true (dropping a key that isn't present short-circuits before ever
// looking at `prev`) and unconditionally read *out_prev to decide how to
// splice the match out of its chain, so leaving it unset meant reading
// uninitialized stack garbage on every single deletion. Confirmed via a
// dedicated debug walk (see the tombstone comment above) reproducing the
// exact symptom: dropping a key from a same-hash chain of length > 1 took
// the "non-head removal" branch (patch the previous link's `.next`) even
// for chains of length 1, based on whatever garbage `prev` happened to
// contain -- corrupting `idx` (left pointing at a slot about to become a
// tombstone) far more often than not.
static int dict_chain_find(Trie_t els, Trie_t idx, trieint_t hkey,
                            PyObject* key, trieint_t* out_index,
                            PyObject** out_val, trieint_t* out_prev) {
   void* found;
   trieint_t ii;
   trieint_t prev = DICT_NO_NEXT;
   if (!amt_lookup(idx, hkey, &found)) {
      if (out_prev) *out_prev = DICT_NO_NEXT;
      return 0;
   }
   ii = *(trieint_t*)found;
   while (1) {
      void* ep;
      DictEntry* e;
      int eq;
      fat_lookup(els, ii, &ep);
      e = (DictEntry*)ep;
      eq = PyObject_RichCompareBool(key, e->key, Py_EQ);
      if (eq < 0) return -1;
      if (eq) {
         if (out_index) *out_index = ii;
         if (out_val) *out_val = e->val;
         if (out_prev) *out_prev = prev;
         return 1;
      }
      prev = ii;
      if (e->next == DICT_NO_NEXT) break;
      ii = e->next;
   }
   if (out_prev) *out_prev = prev;
   return 0;
}


//=============================================================================
// pdict

typedef struct {
   PyObject_HEAD
   Trie_t idx;           // AMT: hash -> first FAT index. Persistent.
   Trie_t els;           // FAT: index -> DictEntry. Persistent.
   Py_ssize_t top;        // next fresh index to hand out.
   Py_ssize_t count;      // number of live entries (== len()).
   Py_ssize_t ndeleted;   // holes burned by deletions since last compaction.
   Py_hash_t hashcode;    // -1 == not yet computed.
} PDictObject;

// PDictType/TDictType are heap types (see list.c's PListType for the full
// rationale): pdict/tdict need to inherit from the Python-level
// PersistentMapping/TransientMapping ABC mixins, and CPython refuses to let
// a statically allocated type derive from a dynamically allocated one.
static PyTypeObject* PDictType = NULL;
static PyTypeObject* TDictType = NULL;
// The six view types are heap types too (see build_view_subtype() below),
// inheriting from collections.abc.KeysView/ItemsView/ValuesView respectively
// -- per the same PyType_FromSpecWithBases heap-type trick used for
// PDictType/TDictType/PListType/TListType, so that isinstance(d.keys(),
// collections.abc.KeysView) (etc.) holds, and so KeysView/ItemsView's own
// Set-derived default __and__/__or__/__sub__/__xor__/comparisons all work
// for free. They add NO native fields of their own (see build_view_subtype);
// all state lives in the inherited `_mapping` slot from collections.abc's
// own MappingView.
static PyTypeObject* PDictKeysType = NULL;
static PyTypeObject* PDictItemsType = NULL;
static PyTypeObject* PDictValuesType = NULL;
static PyTypeObject* TDictKeysType = NULL;
static PyTypeObject* TDictItemsType = NULL;
static PyTypeObject* TDictValuesType = NULL;
// The plain key iterator types (for pdict/tdict's own __iter__, and reused
// for the items()/values() views' __iter__ overrides -- see DictIterObject
// below), by contrast, are ordinary static types: they don't need to
// isinstance() as anything in particular, so there's no reason to pay for
// the heap-type machinery.
static PyTypeObject PDictIterType;
static PyTypeObject TDictIterType;
static PDictObject* g_pdict_empty = NULL;
// Imported once at module-init time (see PyInit_dict), used to replicate the
// reference _dict.py's `isinstance(arg, Mapping)`/`isinstance(arg, Sized)`
// checks in pdict/tdict's general-iterable constructor dispatch.
static PyObject* g_abc_Mapping = NULL;
static PyObject* g_abc_Sized = NULL;
// pcollections.util.seqstr -- the real reference string-formatting helper,
// imported once at PyInit_dict time and used by pdict_repr/pdict_str/
// tdict_repr/tdict_str below so those match abc/_map.py's
// PersistentMapping.__str__/__repr__ and TransientMapping.__str__/__repr__
// exactly (including TransientMapping.__repr__'s "{|...|}" delimiter, which
// does NOT match its own __str__'s "{<...>}" -- this looks like a
// copy-paste bug in the reference, but is faithfully replicated here; see
// tdict_repr below).
static PyObject* g_seqstr = NULL;

// Calls the real pcollections.util.seqstr(seq, maxlen=maxlen) (or plain
// seqstr(seq) when has_maxlen is false, matching the reference's
// maxlen=None default). tostr is always left at its default (repr) here --
// dict.c has no lazy-aware formatting need (that's lazy.c's ldict/tldict).
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

static PyObject* pdict_keys(PDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_items(PDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_values(PDictObject* self, PyObject* Py_UNUSED(ignored));

static int pdict_traverse(PDictObject* self, visitproc visit, void* arg) {
   return dict_gc_traverse(self->els, visit, arg);
}
static int pdict_clear(PDictObject* self) {
   Trie_t els = self->els, idx = self->idx;
   self->els = NULL; self->idx = NULL;
   if (els) fatnode_decref(els, dictentry_decref);
   if (idx) amtnode_decref(idx, noop_decref);
   return 0;
}
static void pdict_dealloc(PDictObject* self) {
   // Standard CPython heap-type dealloc idiom: tp_alloc (used in
   // pdict_wrap_astype) does Py_INCREF(type) for a heap type, so the
   // terminal dealloc must balance it with Py_DECREF(tp) after freeing the
   // instance via tp->tp_free (NOT the type-specific PyObject_GC_Del, which
   // would also be wrong for a subclass whose tp_free may differ). This is
   // required for correct refcounting once pdict/tdict are subclassable
   // (Py_TPFLAGS_BASETYPE); it was invisibly harmless before only because
   // PDictType/TDictType themselves are eternal singletons.
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   pdict_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t pdict_length(PDictObject* self) {
   return self->count;
}

// Takes ownership of the one reference to `els`/`idx` that callers already
// hold, wrapping them in a new instance of `type` -- EXCEPT that a 0-element
// result collapses to the canonical empty pdict singleton instead (mirrors
// plist_wrap's identical convention in list.c), but *only* when `type` is
// exactly PDictType: that collapse is a pure optimization (any 0-element
// pdict is behaviorally identical to the singleton), not something the
// reference _dict.py itself does (pdict.set()/drop() just call
// `self._new(...)` unconditionally, never special-casing emptiness), so for
// any other (necessarily subclass, e.g. ldict) target type we skip it and
// always build a fresh, correctly-typed instance -- collapsing e.g. an empty
// ldict result down to the unrelated, non-lazy g_pdict_empty singleton would
// silently lose the subclass (and, worse, hand back a *wrong* answer: `type
// is ldict` would become false).
//
// `type` must itself be PDictType or a subtype of it (i.e. layout-compatible
// as a PDictObject -- true for any subclass that adds no new slots, which is
// the only kind pdict's own Py_TPFLAGS_BASETYPE-enabled subclassing
// supports; see the note on pdict_spec's flags below).
static PyObject* pdict_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                    Py_ssize_t top, Py_ssize_t count,
                                    Py_ssize_t ndeleted) {
   PDictObject* self;
   if (count == 0 && type == PDictType) {
      fatnode_decref(els, dictentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_INCREF(g_pdict_empty);
      return (PyObject*)g_pdict_empty;
   }
   // Use type->tp_alloc (not PyObject_GC_New) so that a subclass which adds
   // trailing fields (e.g. a plain `class ldict(pdict): pass` picks up
   // __dict__/__weakref__ from Python, growing tp_basicsize past
   // sizeof(PDictObject)) gets those extra bytes zero-initialized. tp_alloc's
   // default (PyType_GenericAlloc) also does the heap-type Py_INCREF(type)
   // and GC-tracks the object itself, so we must NOT call PyObject_GC_Track
   // again below (that would assert-fail as a double-track) and pdict_dealloc
   // must balance the INCREF with a Py_DECREF(Py_TYPE(self)).
   self = (PDictObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(els, dictentry_decref);
      amtnode_decref(idx, noop_decref);
      return NULL;
   }
   self->els = els;
   self->idx = idx;
   self->top = top;
   self->count = count;
   self->ndeleted = ndeleted;
   self->hashcode = -1;
   return (PyObject*)self;
}
static PyObject* pdict_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                             Py_ssize_t count, Py_ssize_t ndeleted) {
   return pdict_wrap_astype(PDictType, els, idx, top, count, ndeleted);
}

// Returns the canonical empty instance of `type` (a new reference), mirroring
// the reference's `cls.empty` attribute lookup in pdict.__new__. For plain
// PDictType this is just the g_pdict_empty singleton built once at
// PyInit_dict time; for any other (necessarily subclass) type, the class is
// expected to have its own `.empty` class attribute set up the same way
// PDictType/PListType set theirs -- lazy.c does this for ldict at its own
// init time.
static PyObject* pdict_type_empty(PyTypeObject* type) {
   if (type == PDictType) {
      Py_INCREF(g_pdict_empty);
      return (PyObject*)g_pdict_empty;
   }
   return PyObject_GetAttrString((PyObject*)type, "empty");
}

// Rebuilds a fresh, fully transient (els, idx) pair containing exactly the
// live entries currently in (els, idx), renumbered 0..count-1 in their
// original insertion-order sequence. Consumes neither input (the caller
// still owns els/idx afterward and must decref them itself once it's done
// reading from them). Returns 0 on success (with *out_els/*out_idx set to
// new, exclusively-owned transient trees and *out_top set to the new
// count), or -1 (with an exception set) on failure -- which can only happen
// if re-hashing a key raises, astronomically unlikely for any key that
// successfully hashed once already, but not impossible (a poorly-behaved
// __hash__ could do anything).
static int dict_rebuild_compacted(Trie_t els, Trie_t idx,
                                   Trie_t* out_els, Trie_t* out_idx,
                                   Py_ssize_t* out_top) {
   Trie_t new_els = fat_empty(ELSLEAFSIZE);
   Trie_t new_idx = amt_empty(IDXLEAFSIZE);
   trieint_t newtop = 0;
   TriePath iter;
   int ok;
   for (ok = fat_firstpath(els, &iter); ok; ok = fat_nextpath(&iter)) {
      DictEntry* e = (DictEntry*)triepath_val(&iter);
      DictEntry newentry;
      trieint_t hkey, prev;
      if (dictentry_is_tombstone(e)) continue; // dead slot -- see the
                                                // tombstone comment above.
      if (dict_hash_key(e->key, &hkey) < 0) {
         fatnode_decref(new_els, dictentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return -1;
      }
      // Every key here is, by construction, not yet present in the fresh
      // tables, so this always appends a brand new chain link -- exactly
      // the "not found" half of dict_chain_find's contract.
      dict_chain_find(new_els, new_idx, hkey, e->key, NULL, NULL, &prev);
      if (prev == DICT_NO_NEXT) {
         // Either a genuinely empty chain (first key with this hash) --
         // check idx directly to tell the two apart -- or the chain's tail.
         void* found;
         if (!amt_lookup(new_idx, hkey, &found)) {
            trieint_t idxval = newtop;
            new_idx = tamt_setitem(new_idx, hkey, &idxval, noop_incref, noop_decref);
         } else {
            void* ep; DictEntry patched;
            trieint_t tailidx = *(trieint_t*)found;
            fat_lookup(new_els, tailidx, &ep);
            memcpy(&patched, ep, sizeof(DictEntry));
            patched.next = newtop;
            new_els = tfat_setitem(new_els, tailidx, &patched,
                                    dictentry_incref, dictentry_decref);
         }
      } else {
         void* ep; DictEntry patched;
         fat_lookup(new_els, prev, &ep);
         memcpy(&patched, ep, sizeof(DictEntry));
         patched.next = newtop;
         new_els = tfat_setitem(new_els, prev, &patched,
                                 dictentry_incref, dictentry_decref);
      }
      newentry.key = e->key;
      newentry.val = e->val;
      newentry.next = DICT_NO_NEXT;
      new_els = tfat_setitem(new_els, newtop, &newentry,
                              dictentry_incref, dictentry_decref);
      ++newtop;
   }
   *out_els = new_els;
   *out_idx = new_idx;
   *out_top = (Py_ssize_t)newtop;
   return 0;
}

static int dict_should_compact(Py_ssize_t count, Py_ssize_t ndeleted) {
   if (ndeleted <= 0) return 0;
   if (ndeleted > DICT_COMPACT_ABS_THRESHOLD) return 1;
   return ndeleted * DICT_COMPACT_FRAC_DEN > count * DICT_COMPACT_FRAC_NUM;
}

// Defined further down (after tdict exists -- see the comment there), and
// implemented by routing through tdict's own general-argument constructor,
// exactly mirroring plist_new_dispatch()'s relationship to tlist in list.c.
static PyObject* pdict_new_dispatch(PyTypeObject* type, PyObject* arg, PyObject* kw);

static PyObject* pdict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   if (n == 0) {
      if (!kwds || PyDict_Size(kwds) == 0) {
         return pdict_type_empty(type);
      }
      return pdict_new_dispatch(type, NULL, kwds);
   } else if (n == 1) {
      return pdict_new_dispatch(type, PyTuple_GET_ITEM(args, 0), kwds);
   } else {
      PyErr_Format(PyExc_TypeError,
                    "pdict expects at most 1 argument, got %zd", n);
      return NULL;
   }
}

// Matches abc/_map.py's PersistentMapping.__repr__: f"{{|{seqstr(self)}|}}"
// (untruncated, default tostr=repr).
static PyObject* pdict_repr(PDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
// Matches abc/_map.py's PersistentMapping.__str__:
// f"{{|{seqstr(self, maxlen=60)}|}}" (truncated at 60 chars).
static PyObject* pdict_str(PDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}

static Py_hash_t pdict_hash(PDictObject* self) {
   PyObject* fs;
   PyObject* h;
   Py_hash_t result;
   TriePath iter;
   int ok;
   if (self->hashcode != -1) return self->hashcode;
   fs = PySet_New(NULL);
   if (!fs) return -1;
   for (ok = fat_firstpath(self->els, &iter); ok; ok = fat_nextpath(&iter)) {
      DictEntry* e = (DictEntry*)triepath_val(&iter);
      PyObject* pair;
      int r;
      if (dictentry_is_tombstone(e)) continue;
      pair = PyTuple_Pack(2, e->key, e->val);
      if (!pair) { Py_DECREF(fs); return -1; }
      r = PySet_Add(fs, pair);
      Py_DECREF(pair);
      if (r < 0) { Py_DECREF(fs); return -1; }
   }
   h = PyFrozenSet_New(fs);
   Py_DECREF(fs);
   if (!h) return -1;
   result = PyObject_Hash(h);
   Py_DECREF(h);
   if (result == -1) return -1;
   result += 2;
   if (result == -1) result = -2;
   self->hashcode = result;
   return result;
}

static PyObject* pdict_subscript(PDictObject* self, PyObject* key) {
   trieint_t hkey;
   trieint_t found_index;
   PyObject* val;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   if (!dict_chain_find(self->els, self->idx, hkey, key, &found_index, &val, NULL)) {
      PyErr_SetObject(PyExc_KeyError, key);
      return NULL;
   }
   Py_INCREF(val);
   return val;
}
static PyObject* pdict_get(PDictObject* self, PyObject* args) {
   PyObject* key;
   PyObject* deflt = Py_None;
   trieint_t hkey;
   PyObject* val;
   if (!PyArg_ParseTuple(args, "O|O", &key, &deflt)) return NULL;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   if (!dict_chain_find(self->els, self->idx, hkey, key, NULL, &val, NULL)) {
      Py_INCREF(deflt);
      return deflt;
   }
   Py_INCREF(val);
   return val;
}
static int pdict_contains(PDictObject* self, PyObject* key) {
   trieint_t hkey;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   return dict_chain_find(self->els, self->idx, hkey, key, NULL, NULL, NULL);
}

static PyObject* pdict_set(PDictObject* self, PyObject* args) {
   PyObject* key; PyObject* val;
   trieint_t hkey;
   trieint_t found_index, prev;
   PyObject* old_val;
   Trie_t new_els, new_idx;
   Py_ssize_t new_top = self->top, new_count = self->count;
   Py_ssize_t new_ndeleted = self->ndeleted;
   int found;
   if (!PyArg_ParseTuple(args, "OO", &key, &val)) return NULL;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                            &old_val, &prev);
   if (found < 0) return NULL;
   if (found) {
      DictEntry entry;
      void* ep;
      if (val == old_val) {
         Py_INCREF(self);
         return (PyObject*)self;
      }
      fat_lookup(self->els, found_index, &ep);
      memcpy(&entry, ep, sizeof(DictEntry));
      entry.val = val;
      new_els = fat_anditem(self->els, found_index, &entry, dictentry_incref);
      trienode_incref(self->idx);
      new_idx = self->idx;
   } else {
      DictEntry entry;
      entry.key = key; entry.val = val; entry.next = DICT_NO_NEXT;
      new_els = fat_anditem(self->els, (trieint_t)new_top, &entry, dictentry_incref);
      if (prev == DICT_NO_NEXT) {
         trieint_t idxval = (trieint_t)new_top;
         new_idx = amt_anditem(self->idx, hkey, &idxval, noop_incref);
      } else {
         void* ep; DictEntry patched;
         Trie_t prev_els;
         fat_lookup(self->els, prev, &ep);
         memcpy(&patched, ep, sizeof(DictEntry));
         patched.next = (trieint_t)new_top;
         // fat_anditem() is non-consuming (leaves its input tree untouched
         // and independently valid), so the intermediate `prev_els` result
         // from the append above must be explicitly decref'd once we're done
         // reading from it -- otherwise it's a leaked, permanently-orphaned
         // reference every time a `.set()` call appends a new key that
         // collides with an existing hash.
         prev_els = new_els;
         new_els = fat_anditem(prev_els, prev, &patched, dictentry_incref);
         fatnode_decref(prev_els, dictentry_decref);
         trienode_incref(self->idx);
         new_idx = self->idx;
      }
      new_top += 1;
      new_count += 1;
   }
   if (dict_should_compact(new_count, new_ndeleted)) {
      Trie_t c_els, c_idx; Py_ssize_t c_top;
      if (dict_rebuild_compacted(new_els, new_idx, &c_els, &c_idx, &c_top) < 0) {
         fatnode_decref(new_els, dictentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return NULL;
      }
      fat_freeze(c_els);
      amt_freeze(c_idx);
      fatnode_decref(new_els, dictentry_decref);
      amtnode_decref(new_idx, noop_decref);
      new_els = c_els; new_idx = c_idx; new_top = c_top; new_ndeleted = 0;
   }
   // self._new(...) in the reference -- preserve self's actual (sub)class,
   // e.g. ldict.set(...) must return another ldict, not a plain pdict.
   return pdict_wrap_astype(Py_TYPE(self), new_els, new_idx, new_top,
                              new_count, new_ndeleted);
}

static PyObject* pdict_drop(PDictObject* self, PyObject* key) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_count, new_ndeleted;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index, NULL, &prev);
   if (found < 0) return NULL;
   if (!found) {
      Py_INCREF(self);
      return (PyObject*)self;
   }
   {
      void* ep; DictEntry entry; DictEntry tombstone;
      fat_lookup(self->els, found_index, &ep);
      memcpy(&entry, ep, sizeof(DictEntry));
      dict_make_tombstone(&tombstone);
      if (prev == DICT_NO_NEXT) {
         // Removing the head of the chain.
         if (entry.next == DICT_NO_NEXT) {
            new_idx = amt_butitem(self->idx, hkey, noop_incref);
         } else {
            trieint_t idxval = entry.next;
            new_idx = amt_anditem(self->idx, hkey, &idxval, noop_incref);
         }
      } else {
         void* pp; DictEntry pentry;
         Trie_t tmp_els;
         fat_lookup(self->els, prev, &pp);
         memcpy(&pentry, pp, sizeof(DictEntry));
         pentry.next = entry.next;
         trienode_incref(self->idx);
         new_idx = self->idx;
         // Same non-consuming-intermediate-result concern as pdict_set()
         // above: free the intermediate tree once we're done reading from it.
         // NOTE: this OVERWRITES found_index's slot with a tombstone via
         // fat_anditem() -- it must never call fat_butitem()/tfat_delitem()
         // here, which would shift every later index down by one and
         // silently invalidate idx/collision-chain pointers; see the
         // tombstone comment above dictentry_is_tombstone().
         tmp_els = fat_anditem(self->els, prev, &pentry, dictentry_incref);
         new_els = fat_anditem(tmp_els, found_index, &tombstone, dictentry_incref);
         fatnode_decref(tmp_els, dictentry_decref);
         goto have_new_els;
      }
      new_els = fat_anditem(self->els, found_index, &tombstone, dictentry_incref);
   }
have_new_els:
   new_count = self->count - 1;
   new_ndeleted = self->ndeleted + 1;
   if (dict_should_compact(new_count, new_ndeleted)) {
      Trie_t c_els, c_idx; Py_ssize_t c_top;
      if (dict_rebuild_compacted(new_els, new_idx, &c_els, &c_idx, &c_top) < 0) {
         fatnode_decref(new_els, dictentry_decref);
         amtnode_decref(new_idx, noop_decref);
         return NULL;
      }
      fat_freeze(c_els);
      amt_freeze(c_idx);
      fatnode_decref(new_els, dictentry_decref);
      amtnode_decref(new_idx, noop_decref);
      return pdict_wrap_astype(Py_TYPE(self), c_els, c_idx, c_top, new_count, 0);
   }
   return pdict_wrap_astype(Py_TYPE(self), new_els, new_idx, self->top,
                              new_count, new_ndeleted);
}

static PyObject* pdict_clear_method(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   (void)self;
   Py_INCREF(g_pdict_empty);
   return (PyObject*)g_pdict_empty;
}

static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored));

static PyMethodDef pdict_methods[] = {
   {"set", (PyCFunction)pdict_set, METH_VARARGS,
    "Returns a copy of the pdict that maps the given key to the given value."},
   {"drop", (PyCFunction)pdict_drop, METH_O,
    "Returns a copy of the pdict that does not include the given key."},
   {"clear", (PyCFunction)pdict_clear_method, METH_NOARGS,
    "Returns the empty pdict."},
   {"transient", (PyCFunction)pdict_transient, METH_NOARGS,
    "Efficiently copies the pdict into a tdict and returns the tdict."},
   {"get", (PyCFunction)pdict_get, METH_VARARGS,
    "Returns the value for key if key is in the pdict, else default."},
   {"keys", (PyCFunction)pdict_keys, METH_NOARGS, "Returns a view of the keys."},
   {"items", (PyCFunction)pdict_items, METH_NOARGS, "Returns a view of the items."},
   {"values", (PyCFunction)pdict_values, METH_NOARGS, "Returns a view of the values."},
   {NULL, NULL, 0, NULL}
};
static PyObject* pdict_iter(PDictObject* self);

static PyType_Slot pdict_slots[] = {
   {Py_tp_dealloc, (void*)pdict_dealloc},
   {Py_tp_repr, (void*)pdict_repr},
   {Py_tp_str, (void*)pdict_str},
   {Py_mp_length, (void*)pdict_length},
   {Py_mp_subscript, (void*)pdict_subscript},
   {Py_sq_contains, (void*)pdict_contains},
   {Py_tp_hash, (void*)pdict_hash},
   {Py_tp_doc,
    (void*)"A persistent dict type similar to `dict`, preserving insertion"
           " order, backed by an AMT hash table and a FAT value table."},
   {Py_tp_traverse, (void*)pdict_traverse},
   {Py_tp_clear, (void*)pdict_clear},
   {Py_tp_iter, (void*)pdict_iter},
   {Py_tp_methods, (void*)pdict_methods},
   {Py_tp_new, (void*)pdict_new},
   {0, NULL}
};
static PyType_Spec pdict_spec = {
   .name = "pcollections._c.dict.pdict",
   .basicsize = sizeof(PDictObject),
   .itemsize = 0,
   // Py_TPFLAGS_BASETYPE: pdict *is* meant to be subclassed in C -- see
   // pcollections._c.lazy's ldict, which adds no fields of its own (matching
   // the reference _lazy.py's `class ldict(pdict): __slots__ = ()`) and
   // relies on pdict_wrap_astype()/pdict_new_dispatch() etc. throughout this
   // file having been made to build instances of `Py_TYPE(self)`/`cls`
   // rather than hardcoding PDictType, exactly mirroring the reference's own
   // `self._new(...)`/`cls._new(...)` idiom.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = pdict_slots,
};


//=============================================================================
// tdict

typedef struct {
   PyObject_HEAD
   Trie_t idx;            // AMT: hash -> first FAT index. May be transient.
   Trie_t els;             // FAT: index -> DictEntry. May be transient.
   Py_ssize_t top;
   Py_ssize_t count;
   Py_ssize_t ndeleted;
   PyObject* orig;          // cached pdict (owned ref), or NULL -- see the
                            // matching field on TListObject in list.c for
                            // the exact caching/invalidation convention.
} TDictObject;

static int tdict_traverse(TDictObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->orig);
   return dict_gc_traverse(self->els, visit, arg);
}
static int tdict_clear(TDictObject* self) {
   Trie_t els = self->els, idx = self->idx;
   PyObject* orig = self->orig;
   self->els = NULL; self->idx = NULL; self->orig = NULL;
   if (els) fatnode_decref(els, dictentry_decref);
   if (idx) amtnode_decref(idx, noop_decref);
   Py_XDECREF(orig);
   return 0;
}
static void tdict_dealloc(TDictObject* self) {
   // See pdict_dealloc's comment: balances the Py_INCREF(type) that
   // tp_alloc performs for a heap type, via tp->tp_free + Py_DECREF(tp).
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   tdict_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t tdict_length(TDictObject* self) {
   return self->count;
}
// `type` must be TDictType or a subtype of it (see the analogous note on
// pdict_wrap_astype() above); `tdict_wrap()` below is the TDictType-only
// convenience wrapper used at call sites that (per the reference _dict.py)
// are never meant to be subclass-aware in the first place -- see each call
// site's own comment for which case it is.
static PyObject* tdict_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                    Py_ssize_t top, Py_ssize_t count,
                                    Py_ssize_t ndeleted, PyObject* orig) {
   // See pdict_wrap_astype's comment: tp_alloc (not PyObject_GC_New) is
   // required so a subclass's extra trailing fields (__dict__/__weakref__)
   // are zero-initialized, and it already GC-tracks + INCREFs the type, so
   // no manual PyObject_GC_Track here (tdict_dealloc balances the INCREF).
   TDictObject* self = (TDictObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(els, dictentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_XDECREF(orig);
      return NULL;
   }
   self->els = els; self->idx = idx;
   self->top = top; self->count = count; self->ndeleted = ndeleted;
   self->orig = orig;
   return (PyObject*)self;
}
static PyObject* tdict_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                             Py_ssize_t count, Py_ssize_t ndeleted,
                             PyObject* orig) {
   return tdict_wrap_astype(TDictType, els, idx, top, count, ndeleted, orig);
}
static void tdict_invalidate_orig(TDictObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

static PyObject* tdict_empty_astype(PyTypeObject* type) {
   return tdict_wrap_astype(type, fat_empty(ELSLEAFSIZE), amt_empty(IDXLEAFSIZE),
                              0, 0, 0, NULL);
}

// Forward-declared: the real (combined setitem/delitem) definition is further
// down, but tdict_build_from_arg()/tdict_new() (both immediately below) need
// to call it already.
static int tdict_ass_subscript(TDictObject* self, PyObject* key, PyObject* val);

// Builds a fresh, freshly-populated tdict out of a general Python iterable
// (of key-value pairs, each coerced via PySequence_Tuple exactly as real
// `dict(iterable)` does -- so 2-element lists/tuples/anything-iterable all
// work, not just literal tuples) or, if `arg` is a Mapping, out of
// `arg.items()`. Mirrors tdict.__new__'s "else" branch in _dict.py. Returns
// a new reference, or NULL (with an exception set) on failure.
static PyObject* tdict_build_from_arg_astype(PyTypeObject* type, PyObject* arg) {
   PyObject* obj = tdict_empty_astype(type);
   PyObject* items_src;
   PyObject* iterator;
   PyObject* item;
   int is_mapping;
   if (!obj) return NULL;
   is_mapping = PyObject_IsInstance(arg, g_abc_Mapping);
   if (is_mapping < 0) { Py_DECREF(obj); return NULL; }
   if (is_mapping) {
      items_src = PyObject_CallMethod(arg, "items", NULL);
      if (!items_src) { Py_DECREF(obj); return NULL; }
   } else {
      items_src = arg;
      Py_INCREF(items_src);
   }
   iterator = PyObject_GetIter(items_src);
   Py_DECREF(items_src);
   if (!iterator) { Py_DECREF(obj); return NULL; }
   while ((item = PyIter_Next(iterator))) {
      PyObject* pair = PySequence_Tuple(item);
      Py_DECREF(item);
      if (!pair) { Py_DECREF(iterator); Py_DECREF(obj); return NULL; }
      if (PyTuple_GET_SIZE(pair) != 2) {
         PyErr_SetString(PyExc_ValueError,
                         "dictionary update sequence element has length != 2");
         Py_DECREF(pair); Py_DECREF(iterator); Py_DECREF(obj);
         return NULL;
      }
      if (tdict_ass_subscript((TDictObject*)obj, PyTuple_GET_ITEM(pair, 0),
                              PyTuple_GET_ITEM(pair, 1)) < 0) {
         Py_DECREF(pair); Py_DECREF(iterator); Py_DECREF(obj);
         return NULL;
      }
      Py_DECREF(pair);
   }
   Py_DECREF(iterator);
   if (PyErr_Occurred()) { Py_DECREF(obj); return NULL; }
   return obj;
}

// Forward-declared: implemented after PDictType/pdict_transient exist (the
// `type(arg) is pdict` case below routes through pdict_transient()).
static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored));

static PyObject* tdict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* arg;
   PyObject* kwds_to_merge;
   PyObject* obj;
   if (n == 0) {
      if (!kwds || PyDict_Size(kwds) == 0) return tdict_empty_astype(type);
      // Matches tdict.__new__'s `arg = kw; kw = None` special case: kwds
      // itself becomes the sole constructor argument (a dict IS a Mapping,
      // so tdict_build_from_arg's isinstance(Mapping) check routes it
      // through `.items()` correctly), and -- crucially -- it must NOT also
      // be merged in again afterward (kwds_to_merge stays NULL here).
      arg = kwds;
      kwds_to_merge = NULL;
   } else if (n == 1) {
      arg = PyTuple_GET_ITEM(args, 0);
      kwds_to_merge = (kwds && PyDict_Size(kwds) > 0) ? kwds : NULL;
   } else {
      PyErr_Format(PyExc_TypeError,
                    "tdict expects at most 1 argument, got %zd", n);
      return NULL;
   }
   // Both of the exact-type checks below intentionally use `Py_TYPE(arg) ==
   // ...` (never PyType_IsSubtype/isinstance): this mirrors the reference
   // _dict.py's own `type(arg) is tdict` / `type(arg) is pdict` checks
   // exactly -- a subclass instance (e.g. a tldict or ldict argument) is
   // deliberately excluded from these fast structural-sharing paths and
   // falls through to the general `tdict_build_from_arg_astype` path below,
   // which goes through `arg.items()` and so (for a lazy-dict argument)
   // correctly *dereferences* rather than raw-sharing -- see lazy.c's
   // ldict/tldict for why that distinction matters.
   if (Py_TYPE(arg) == TDictType) {
      // tdict(some_tdict): share a frozen snapshot of that tdict's current
      // (els, idx) -- not the tdict's own live, still-mutable trees -- as
      // the starting point for a brand-new, independent transient session.
      // Also propagates `_orig`: if the source tdict still has a valid
      // cached original pdict, this new tdict is (right now) an exact copy
      // of that same pdict too, so it's valid to share that same cache.
      TDictObject* t = (TDictObject*)arg;
      Trie_t els = t->els, idx = t->idx;
      PyObject* orig = t->orig;
      fat_freeze(els);
      amt_freeze(idx);
      trienode_incref(els);
      trienode_incref(idx);
      Py_XINCREF(orig);
      obj = tdict_wrap_astype(type, els, idx, t->top, t->count, t->ndeleted, orig);
   } else if (Py_TYPE(arg) == PDictType) {
      // tdict(some_pdict): the same O(1) sharing logic as pdict_transient()
      // (below), just parameterized by `type` instead of hardcoded to
      // TDictType -- `cls._new(...)` in the reference's tdict.__new__, not
      // `tdict._new(...)`, so this must build a `type` instance, which may
      // be a subclass (e.g. tdict(some_pdict) called by way of
      // tldict.__new__ delegating to tdict.__new__).
      PDictObject* p = (PDictObject*)arg;
      trienode_incref(p->els);
      trienode_incref(p->idx);
      Py_INCREF(arg);
      obj = tdict_wrap_astype(type, p->els, p->idx, p->top, p->count,
                                p->ndeleted, arg);
   } else {
      obj = tdict_build_from_arg_astype(type, arg);
   }
   if (!obj) return NULL;
   if (kwds_to_merge) {
      PyObject* key; PyObject* val;
      Py_ssize_t pos = 0;
      while (PyDict_Next(kwds_to_merge, &pos, &key, &val)) {
         if (tdict_ass_subscript((TDictObject*)obj, key, val) < 0) {
            Py_DECREF(obj);
            return NULL;
         }
      }
   }
   return obj;
}

// Matches abc/_map.py's TransientMapping.__repr__: f"{{<{seqstr(self)}>}}"
// (untruncated, "{<...>}" delimiter -- the same delimiter its __str__
// (below) uses). An earlier version of this function replicated a
// copy-paste bug in the reference that used PersistentMapping's "{|...|}"
// delimiter here instead; that bug has since been fixed in abc/_map.py
// (confirmed with Noah), so this now matches.
static PyObject* tdict_repr(TDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}
// Matches abc/_map.py's TransientMapping.__str__:
// f"{{<{seqstr(self, maxlen=60)}>}}" (truncated, "{<...>}" delimiter).
static PyObject* tdict_str(TDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}

static PyObject* tdict_subscript(TDictObject* self, PyObject* key) {
   trieint_t hkey; PyObject* val;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   if (!dict_chain_find(self->els, self->idx, hkey, key, NULL, &val, NULL)) {
      PyErr_SetObject(PyExc_KeyError, key);
      return NULL;
   }
   Py_INCREF(val);
   return val;
}
static PyObject* tdict_get(TDictObject* self, PyObject* args) {
   PyObject* key; PyObject* deflt = Py_None;
   trieint_t hkey; PyObject* val;
   if (!PyArg_ParseTuple(args, "O|O", &key, &deflt)) return NULL;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   if (!dict_chain_find(self->els, self->idx, hkey, key, NULL, &val, NULL)) {
      Py_INCREF(deflt);
      return deflt;
   }
   Py_INCREF(val);
   return val;
}
static int tdict_contains(TDictObject* self, PyObject* key) {
   trieint_t hkey;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   return dict_chain_find(self->els, self->idx, hkey, key, NULL, NULL, NULL);
}

// Maybe-compact a tdict in place after a mutation. Always succeeds unless
// re-hashing a key fails (see dict_rebuild_compacted).
static int tdict_maybe_compact(TDictObject* self) {
   Trie_t c_els, c_idx; Py_ssize_t c_top;
   if (!dict_should_compact(self->count, self->ndeleted)) return 0;
   if (dict_rebuild_compacted(self->els, self->idx, &c_els, &c_idx, &c_top) < 0)
      return -1;
   fatnode_decref(self->els, dictentry_decref);
   amtnode_decref(self->idx, noop_decref);
   self->els = c_els; self->idx = c_idx; self->top = c_top; self->ndeleted = 0;
   return 0;
}

static int tdict_ass_subscript(TDictObject* self, PyObject* key, PyObject* val) {
   trieint_t hkey;
   trieint_t found_index, prev;
   PyObject* old_val;
   int found;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   if (val == NULL) {
      // __delitem__.
      found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                               NULL, &prev);
      if (found < 0) return -1;
      if (!found) {
         PyErr_SetObject(PyExc_KeyError, key);
         return -1;
      }
      {
         void* ep; DictEntry entry; DictEntry tombstone;
         fat_lookup(self->els, found_index, &ep);
         memcpy(&entry, ep, sizeof(DictEntry));
         dict_make_tombstone(&tombstone);
         if (prev == DICT_NO_NEXT) {
            if (entry.next == DICT_NO_NEXT)
               self->idx = tamt_delitem(self->idx, hkey, noop_incref, noop_decref);
            else {
               trieint_t idxval = entry.next;
               self->idx = tamt_setitem(self->idx, hkey, &idxval, noop_incref, noop_decref);
            }
         } else {
            void* pp; DictEntry pentry;
            fat_lookup(self->els, prev, &pp);
            memcpy(&pentry, pp, sizeof(DictEntry));
            pentry.next = entry.next;
            self->els = tfat_setitem(self->els, prev, &pentry,
                                      dictentry_incref, dictentry_decref);
         }
         // OVERWRITE found_index's slot with a tombstone via tfat_setitem()
         // -- must never be tfat_delitem()/fat_butitem(), which would shift
         // every later index down by one and invalidate idx/collision-chain
         // pointers; see the tombstone comment near dictentry_is_tombstone().
         self->els = tfat_setitem(self->els, found_index, &tombstone,
                                   dictentry_incref, dictentry_decref);
      }
      self->count -= 1;
      self->ndeleted += 1;
      tdict_invalidate_orig(self);
      if (tdict_maybe_compact(self) < 0) return -1;
      return 0;
   }
   // __setitem__.
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                            &old_val, &prev);
   if (found < 0) return -1;
   if (found) {
      if (val != old_val) {
         void* ep; DictEntry entry;
         fat_lookup(self->els, found_index, &ep);
         memcpy(&entry, ep, sizeof(DictEntry));
         entry.val = val;
         self->els = tfat_setitem(self->els, found_index, &entry,
                                   dictentry_incref, dictentry_decref);
         tdict_invalidate_orig(self);
      }
      return 0;
   }
   {
      DictEntry entry;
      entry.key = key; entry.val = val; entry.next = DICT_NO_NEXT;
      self->els = tfat_setitem(self->els, (trieint_t)self->top, &entry,
                                dictentry_incref, dictentry_decref);
      if (prev == DICT_NO_NEXT) {
         trieint_t idxval = (trieint_t)self->top;
         self->idx = tamt_setitem(self->idx, hkey, &idxval, noop_incref, noop_decref);
      } else {
         void* ep; DictEntry patched;
         fat_lookup(self->els, prev, &ep);
         memcpy(&patched, ep, sizeof(DictEntry));
         patched.next = (trieint_t)self->top;
         self->els = tfat_setitem(self->els, prev, &patched,
                                   dictentry_incref, dictentry_decref);
      }
   }
   self->top += 1;
   self->count += 1;
   tdict_invalidate_orig(self);
   if (tdict_maybe_compact(self) < 0) return -1;
   return 0;
}

static PyObject* tdict_clear_method(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t old_els = self->els, old_idx = self->idx;
   PyObject* old_orig = self->orig;
   self->els = fat_empty(ELSLEAFSIZE);
   self->idx = amt_empty(IDXLEAFSIZE);
   self->top = 0; self->count = 0; self->ndeleted = 0;
   self->orig = NULL;
   fatnode_decref(old_els, dictentry_decref);
   amtnode_decref(old_idx, noop_decref);
   Py_XDECREF(old_orig);
   Py_RETURN_NONE;
}

static PyObject* pdict_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                             Py_ssize_t count, Py_ssize_t ndeleted);

static PyObject* tdict_persistent(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t els, idx;
   if (self->count == 0) {
      Py_INCREF(g_pdict_empty);
      return (PyObject*)g_pdict_empty;
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
   return pdict_wrap(els, idx, self->top, self->count, self->ndeleted);
}

static PyObject* tdict_keys(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* tdict_items(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* tdict_values(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   trienode_incref(self->els);
   trienode_incref(self->idx);
   // tdict_wrap() takes ownership of (does not itself incref) the `orig`
   // reference it's handed -- self must be incref'd here, exactly as
   // plist_transient() does in list.c, or the returned tdict ends up holding
   // an unowned pointer to self, and later decref's it once too many times
   // when the tdict is deallocated (a use-after-free/double-free on `self`).
   Py_INCREF(self);
   return tdict_wrap(self->els, self->idx, self->top, self->count,
                      self->ndeleted, (PyObject*)self);
}

// Matches _dict.py's tdict.empty being a *classmethod* (unlike pdict.empty,
// a plain stored attribute -- see pdict_type_empty's comment): tdict is
// transient/mutable, so there is no single shared empty singleton to hand
// back, only a fresh one built to order each call. Previously this was
// only reachable internally (via tdict_empty_astype, used by tdict_new()
// etc.) and never exposed to Python at all -- `tdict.empty()` raised
// AttributeError, diverging from the reference.
static PyObject* tdict_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   return tdict_empty_astype((PyTypeObject*)cls);
}

static PyMethodDef tdict_methods[] = {
   {"empty", (PyCFunction)tdict_empty_classmethod, METH_NOARGS | METH_CLASS,
    "Returns a new, empty tdict."},
   {"clear", (PyCFunction)tdict_clear_method, METH_NOARGS,
    "Clears all elements from the tdict."},
   {"persistent", (PyCFunction)tdict_persistent, METH_NOARGS,
    "Efficiently copies the tdict into a pdict and returns the pdict."},
   {"get", (PyCFunction)tdict_get, METH_VARARGS,
    "Returns the value for key if key is in the tdict, else default."},
   {"keys", (PyCFunction)tdict_keys, METH_NOARGS, "Returns a view of the keys."},
   {"items", (PyCFunction)tdict_items, METH_NOARGS, "Returns a view of the items."},
   {"values", (PyCFunction)tdict_values, METH_NOARGS, "Returns a view of the values."},
   {NULL, NULL, 0, NULL}
};

static PyObject* tdict_iter(TDictObject* self);

static PyType_Slot tdict_slots[] = {
   {Py_tp_dealloc, (void*)tdict_dealloc},
   {Py_tp_repr, (void*)tdict_repr},
   {Py_tp_str, (void*)tdict_str},
   {Py_mp_length, (void*)tdict_length},
   {Py_mp_subscript, (void*)tdict_subscript},
   {Py_mp_ass_subscript, (void*)tdict_ass_subscript},
   {Py_sq_contains, (void*)tdict_contains},
   {Py_tp_doc,
    (void*)"A transient (mutable) dict type similar to `dict`, preserving"
           " insertion order, backed by an AMT hash table and a FAT value"
           " table."},
   {Py_tp_traverse, (void*)tdict_traverse},
   {Py_tp_clear, (void*)tdict_clear},
   {Py_tp_iter, (void*)tdict_iter},
   {Py_tp_methods, (void*)tdict_methods},
   {Py_tp_new, (void*)tdict_new},
   {0, NULL}
};
static PyType_Spec tdict_spec = {
   .name = "pcollections._c.dict.tdict",
   .basicsize = sizeof(TDictObject),
   .itemsize = 0,
   // See pdict_spec's comment on Py_TPFLAGS_BASETYPE -- tldict (lazy.c) is
   // the subclass this enables here.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tdict_slots,
};


//=============================================================================
// pdict_new()'s general-argument routing (both the "n==1, general iterable"
// case and the "n==0, kwargs-only" case) is defined down here, now that
// tdict_new()/tdict_persistent() both exist -- mirrors plist_new_dispatch()'s
// relationship to tlist in list.c exactly.

static PyObject* pdict_new_dispatch(PyTypeObject* type, PyObject* arg, PyObject* kw) {
   PyObject* targs;
   PyObject* t;
   PyObject* result;
   Py_ssize_t kwn = (kw ? PyDict_Size(kw) : 0);
   if (kwn == 0 && arg != NULL) {
      int sized = PyObject_IsInstance(arg, g_abc_Sized);
      if (sized < 0) return NULL;
      if (sized) {
         Py_ssize_t n = PyObject_Length(arg);
         if (n < 0) return NULL;
         if (n == 0) {
            return pdict_type_empty(type);
         }
      }
      // isinstance(arg, tdict) in the reference (_dict.py's pdict.__new__)
      // -- a genuine isinstance check, not `type(arg) is tdict`, so this
      // deliberately also matches a tldict argument (raw-sharing its
      // internal state without dereferencing any lazy values -- see
      // lazy.c's tldict for why that's the reference's actual, if slightly
      // surprising, behavior for a *transient* lazy-dict argument, as
      // opposed to a *persistent* one).
      if (PyObject_TypeCheck(arg, TDictType)) {
         TDictObject* t2 = (TDictObject*)arg;
         Trie_t els = t2->els, idx = t2->idx;
         fat_freeze(els);
         amt_freeze(idx);
         trienode_incref(els);
         trienode_incref(idx);
         return pdict_wrap_astype(type, els, idx, t2->top, t2->count, t2->ndeleted);
      }
      if (Py_TYPE(arg) == type) {
         // type(arg) is cls in the reference: arg is already exactly the
         // type being constructed -- return it unchanged.
         Py_INCREF(arg);
         return arg;
      }
      if (Py_TYPE(arg) == PDictType) {
         // pdict(some_pdict) or, e.g., ldict(some_plain_pdict): share arg's
         // raw (els, idx) directly into a fresh `type`-typed instance --
         // `cls._new(arg._els, arg._idx, arg._top)` in the reference.
         PDictObject* p = (PDictObject*)arg;
         trienode_incref(p->els);
         trienode_incref(p->idx);
         return pdict_wrap_astype(type, p->els, p->idx, p->top, p->count,
                                    p->ndeleted);
      }
   }
   // General path (also covers the n==0-with-kwargs-only case, where `arg`
   // is NULL and `kw` alone drives construction): build a *plain* tdict --
   // `t = tdict(arg, **kw)` in the reference names the global `tdict` class
   // explicitly, not `cls`-derived -- then take an O(log n) persistent
   // snapshot of it, this time built as `type` (`cls._new(...)` in the
   // reference). If `arg` was itself, say, an ldict (excluded from all the
   // fast paths above since it's neither a tdict, nor exactly `type`, nor
   // exactly plain pdict), building that intermediate tdict goes through
   // tdict_build_from_arg's general iteration, which calls `arg.items()` --
   // correctly *dereferencing* any lazy values along the way.
   targs = arg ? PyTuple_Pack(1, arg) : PyTuple_New(0);
   if (!targs) return NULL;
   t = tdict_new(TDictType, targs, kw);
   Py_DECREF(targs);
   if (!t) return NULL;
   {
      TDictObject* tobj = (TDictObject*)t;
      if (tobj->count == 0) {
         Py_DECREF(t);
         result = pdict_type_empty(type);
      } else {
         Trie_t els = tobj->els, idx = tobj->idx;
         fat_freeze(els);
         amt_freeze(idx);
         trienode_incref(els);
         trienode_incref(idx);
         result = pdict_wrap_astype(type, els, idx, tobj->top, tobj->count,
                                      tobj->ndeleted);
         Py_DECREF(t);
      }
   }
   return result;
}


//=============================================================================
// Iterators.
// A single pair of iterator types (one for pdict, one for tdict -- so that
// the owner-type check below stays a simple, explicit Py_TYPE() comparison,
// exactly mirroring list.c's plist_iterator/tlist_iterator split) is reused
// for THREE purposes: pdict/tdict's own plain __iter__ (mode KEYS), and the
// items()/values() views' __iter__ overrides (modes ITEMS/VALUES -- see
// below). All three walk `els` directly via fat_firstpath/fat_nextpath in a
// single pass, which is why items()/values() get their own native __iter__
// overrides at all, rather than relying on collections.abc.ItemsView/
// ValuesView's own default __iter__ (which would otherwise re-look-up each
// key's value one at a time via __getitem__ -- correct, but a wasted
// redundant hash-chain walk when we already have the value in hand while
// scanning els).

typedef enum {
   DICTITER_KEYS = 0,
   DICTITER_VALUES = 1,
   DICTITER_ITEMS = 2
} DictIterMode;

typedef struct {
   PyObject_HEAD
   PyObject* owner;    // strong ref to the pdict/tdict being walked (never a
                       // view object) -- keeps `els` alive for the duration.
   TriePath path;
   int state;          // 0 = not yet started, 1 = active, 2 = exhausted.
   DictIterMode mode;
} DictIterObject;

static void dictiter_dealloc(DictIterObject* self) {
   PyObject_GC_UnTrack(self);
   Py_XDECREF(self->owner);
   PyObject_GC_Del(self);
}
static int dictiter_traverse(DictIterObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->owner);
   return 0;
}
static Trie_t dictiter_owner_els(DictIterObject* self) {
   // PyObject_TypeCheck (isinstance-equivalent), not an exact Py_TYPE()
   // match: self->owner may now be an ldict/tldict (pcollections._c.lazy)
   // rather than a plain pdict/tdict, and must still be routed to the
   // right struct layout by *family* (persistent vs transient), not by
   // exact class.
   if (PyObject_TypeCheck(self->owner, PDictType))
      return ((PDictObject*)self->owner)->els;
   else
      return ((TDictObject*)self->owner)->els;
}
static PyObject* dictiter_next(DictIterObject* self) {
   int ok;
   DictEntry* e;
   if (self->state == 2) return NULL;
   if (self->state == 0) {
      ok = fat_firstpath(dictiter_owner_els(self), &self->path);
      self->state = 1;
   } else {
      ok = fat_nextpath(&self->path);
   }
   // Skip tombstoned (deleted-but-not-yet-compacted) slots -- see the
   // tombstone comment near dictentry_is_tombstone() above.
   while (ok && dictentry_is_tombstone((DictEntry*)triepath_val(&self->path)))
      ok = fat_nextpath(&self->path);
   if (!ok) { self->state = 2; return NULL; }
   e = (DictEntry*)triepath_val(&self->path);
   switch (self->mode) {
      case DICTITER_KEYS:
         Py_INCREF(e->key);
         return e->key;
      case DICTITER_VALUES:
         Py_INCREF(e->val);
         return e->val;
      default: // DICTITER_ITEMS
         return PyTuple_Pack(2, e->key, e->val);
   }
}
static PyObject* dictiter_self(PyObject* self) { Py_INCREF(self); return self; }

static PyObject* make_dictiter(PyTypeObject* itertype, PyObject* owner_dict,
                                DictIterMode mode) {
   DictIterObject* it = PyObject_GC_New(DictIterObject, itertype);
   if (!it) return NULL;
   Py_INCREF(owner_dict);
   it->owner = owner_dict;
   it->state = 0;
   it->mode = mode;
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

static PyTypeObject PDictIterType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.dict.pdict_iterator",
   .tp_basicsize = sizeof(DictIterObject),
   .tp_dealloc = (destructor)dictiter_dealloc,
   .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .tp_traverse = (traverseproc)dictiter_traverse,
   .tp_iter = dictiter_self,
   .tp_iternext = (iternextfunc)dictiter_next,
};
static PyTypeObject TDictIterType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.dict.tdict_iterator",
   .tp_basicsize = sizeof(DictIterObject),
   .tp_dealloc = (destructor)dictiter_dealloc,
   .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .tp_traverse = (traverseproc)dictiter_traverse,
   .tp_iter = dictiter_self,
   .tp_iternext = (iternextfunc)dictiter_next,
};

static PyObject* pdict_iter(PDictObject* self) {
   return make_dictiter(&PDictIterType, (PyObject*)self, DICTITER_KEYS);
}
static PyObject* tdict_iter(TDictObject* self) {
   return make_dictiter(&TDictIterType, (PyObject*)self, DICTITER_KEYS);
}


//=============================================================================
// keys()/items()/values() view types.
//
// PDictKeysType/PDictItemsType/PDictValuesType/TDictKeysType/TDictItemsType/
// TDictValuesType are built as heap types inheriting directly from
// collections.abc.KeysView/ItemsView/ValuesView (see build_view_type()
// below) -- so isinstance(d.keys(), collections.abc.KeysView) holds, and
// KeysView/ItemsView's own Set-derived __and__/__or__/__sub__/__xor__/
// richcompare all work for free, exactly as for the reference _dict.py's own
// pdict_keys(KeysView, PDictView) etc. (Set's default _from_iterable(cls,it)
// is `cls(it)`, which our dictview_new() below rejects unless `it` happens
// to be the right concrete pdict/tdict type -- an inherent limitation the
// reference implementation shares too: pdict_keys.__and__(other) would
// equally fail there, since PDictView.__new__ makes the identical demand.)
//
// These types add NO native fields of their own: `_mapping` is the slot
// collections.abc's own MappingView already defines (and KeysView/ItemsView/
// ValuesView inherit), so build_view_type() gives each type a basicsize
// exactly equal to its ABC base's own tp_basicsize (read dynamically -- see
// its comment) rather than a fixed sizeof(struct). Their own __contains__/
// __len__/__reversed__ (and, for keys(), __iter__ too) are therefore simply
// inherited unmodified from KeysView/ItemsView/ValuesView, which call back
// into our pdict/tdict's own native __iter__/__getitem__/__len__ -- correct
// (if not maximally fast for __contains__, which re-walks a hash chain) with
// zero extra C code. Only items()/values()' __iter__ is overridden natively
// (see the iterator section above) to avoid a redundant per-key hash lookup.

static PyObject* dictview_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* d;
   PyObject* self;
   PyTypeObject* required;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "dict view takes no keyword arguments");
      return NULL;
   }
   if (!PyArg_ParseTuple(args, "O", &d)) return NULL;
   if (type == PDictKeysType || type == PDictItemsType || type == PDictValuesType)
      required = PDictType;
   else
      required = TDictType;
   // PyObject_TypeCheck (isinstance-equivalent), not an exact Py_TYPE()
   // match: `d` only needs to be layout-compatible as a PDictObject/
   // TDictObject, which holds for any subclass that adds no fields of its
   // own -- e.g. ldict/tldict (pcollections._c.lazy) -- so `some_ldict.keys()`
   // (inherited unmodified from pdict) must be able to construct a
   // PDictKeysType view over an `ldict` instance, not just a plain `pdict`.
   if (!PyObject_TypeCheck(d, required)) {
      PyErr_Format(PyExc_ValueError, "can only make %s object from %s",
                   type->tp_name, required->tp_name);
      return NULL;
   }
   self = type->tp_alloc(type, 0);
   if (!self) return NULL;
   if (PyObject_SetAttrString(self, "_mapping", d) < 0) {
      Py_DECREF(self);
      return NULL;
   }
   return self;
}

static PyObject* dictview_get_mapping(PyObject* self) {
   return PyObject_GetAttrString(self, "_mapping");  // new reference
}
static PyObject* pdictitems_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(&PDictIterType, mapping, DICTITER_ITEMS);
   Py_DECREF(mapping);
   return it;
}
static PyObject* pdictvalues_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(&PDictIterType, mapping, DICTITER_VALUES);
   Py_DECREF(mapping);
   return it;
}
static PyObject* tdictitems_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(&TDictIterType, mapping, DICTITER_ITEMS);
   Py_DECREF(mapping);
   return it;
}
static PyObject* tdictvalues_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(&TDictIterType, mapping, DICTITER_VALUES);
   Py_DECREF(mapping);
   return it;
}

// Each array below reserves two placeholder entries for Py_tp_traverse/
// Py_tp_clear, patched in by build_view_type() at PyInit_dict time (see its
// comment): PyType_FromSpecWithBases does NOT reliably inherit tp_traverse/
// tp_clear from a Python-defined (heap) base the way ordinary Python class
// statements do -- confirmed the hard way via
// "SystemError: type ... has the Py_TPFLAGS_HAVE_GC flag but has no
// traverse function" the first time this was tried without them.
static PyType_Slot dictview_keys_slots[] = {
   {Py_tp_new, (void*)dictview_new},
   {Py_tp_traverse, NULL},
   {Py_tp_clear, NULL},
   {0, NULL}
};
static PyType_Slot pdict_items_view_slots[] = {
   {Py_tp_new, (void*)dictview_new},
   {Py_tp_iter, (void*)pdictitems_iter},
   {Py_tp_traverse, NULL},
   {Py_tp_clear, NULL},
   {0, NULL}
};
static PyType_Slot pdict_values_view_slots[] = {
   {Py_tp_new, (void*)dictview_new},
   {Py_tp_iter, (void*)pdictvalues_iter},
   {Py_tp_traverse, NULL},
   {Py_tp_clear, NULL},
   {0, NULL}
};
static PyType_Slot tdict_items_view_slots[] = {
   {Py_tp_new, (void*)dictview_new},
   {Py_tp_iter, (void*)tdictitems_iter},
   {Py_tp_traverse, NULL},
   {Py_tp_clear, NULL},
   {0, NULL}
};
static PyType_Slot tdict_values_view_slots[] = {
   {Py_tp_new, (void*)dictview_new},
   {Py_tp_iter, (void*)tdictvalues_iter},
   {Py_tp_traverse, NULL},
   {Py_tp_clear, NULL},
   {0, NULL}
};
// .basicsize is filled in at PyInit_dict time (see build_view_type) -- it
// depends on the dynamically-imported ABC base type's own tp_basicsize.
static PyType_Spec pdict_keys_view_spec = {
   .name = "pcollections._c.dict.pdict_keys", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = dictview_keys_slots,
};
static PyType_Spec pdict_items_view_spec = {
   .name = "pcollections._c.dict.pdict_items", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = pdict_items_view_slots,
};
static PyType_Spec pdict_values_view_spec = {
   .name = "pcollections._c.dict.pdict_values", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = pdict_values_view_slots,
};
static PyType_Spec tdict_keys_view_spec = {
   .name = "pcollections._c.dict.tdict_keys", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = dictview_keys_slots,
};
static PyType_Spec tdict_items_view_spec = {
   .name = "pcollections._c.dict.tdict_items", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = tdict_items_view_slots,
};
static PyType_Spec tdict_values_view_spec = {
   .name = "pcollections._c.dict.tdict_values", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = tdict_values_view_slots,
};

static PyObject* pdict_keys(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)PDictKeysType, (PyObject*)self, NULL);
}
static PyObject* pdict_items(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)PDictItemsType, (PyObject*)self, NULL);
}
static PyObject* pdict_values(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)PDictValuesType, (PyObject*)self, NULL);
}
static PyObject* tdict_keys(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)TDictKeysType, (PyObject*)self, NULL);
}
static PyObject* tdict_items(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)TDictItemsType, (PyObject*)self, NULL);
}
static PyObject* tdict_values(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)TDictValuesType, (PyObject*)self, NULL);
}


//=============================================================================
// Module definition.

static PyModuleDef dict_module = {
   PyModuleDef_HEAD_INIT,
   "pcollections._c.dict",
   "C implementations of pdict and tdict, backed by an AMT hash table and a"
   " FAT value table.",
   -1,
   NULL, NULL, NULL, NULL, NULL
};

// Builds the heap type described by `spec`, inheriting from the Python-level
// PersistentMapping/TransientMapping ABC mixin class named `base_name`
// (looked up on `abc_module`) -- see build_abc_subtype()'s twin in list.c
// for the full rationale; duplicated here (rather than shared) since this is
// a separate translation unit/module. Returns a new reference, or NULL (with
// an exception set) on failure.
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
   // pdict/tdict both rely on PersistentMapping/TransientMapping's own
   // tp_richcompare (== collections.abc.Mapping's __eq__/__ne__) being
   // inherited, without providing it natively. Naively listing
   // Py_tp_richcompare in pdict_slots[]/tdict_slots[] (even when copying the
   // exact same base function pointer) does NOT work: CPython's
   // spec-processing treats an explicitly-listed comparison slot as "this
   // type implements it", and additionally synthesizes a matching
   // __eq__/__ne__/... *wrapper descriptor* directly in this new type's own
   // tp_dict. Since slot_tp_richcompare (the generic dispatcher Python-
   // defined classes use) looks up __eq__ via the *instance's actual type*
   // MRO -- which, with that wrapper now present, finds THIS type's own
   // newly-synthesized wrapper first, calling straight back into
   // tp_richcompare -- that infinite-loops with no Python-level frames ever
   // created (confirmed the hard way: RecursionError with an empty
   // traceback, isolated and reproduced in a ~40-line standalone probe
   // before this fix). Poking the C struct field directly here, *after*
   // construction, sidesteps spec-processing entirely: no wrapper is ever
   // added to tp_dict, so MRO lookup falls through correctly to
   // Mapping.__eq__ (etc.) on the base class, exactly as plain Python
   // single-inheritance would behave.
   //
   // tp_hash is NOT touched here: pdict provides its own native tp_hash
   // (pdict_hash, via a real Py_tp_hash slot in pdict_spec -- a concrete
   // function, not a generic MRO-dispatching one, so it's in no danger of
   // this same self-reference loop and must not be overwritten), while
   // tdict provides none at all and needs TransientMapping's unhashability
   // patched in separately -- see PyInit_dict()'s explicit fixup for that,
   // right after this function builds TDictType.
   result_t->tp_richcompare = base_t->tp_richcompare;
   return result_t;
}

// Registers `concrete` as a virtual subclass (via .register()) of the type
// named `attr_name` on `module`. Used so that isinstance()/issubclass()
// checks against e.g. pcollections.abc.PersistentMapping (and, transitively,
// against collections.abc.Mapping -- confirmed empirically that .register()
// with a *real* subclass of a stdlib ABC is honored by that stdlib ABC too)
// keep succeeding for pdict/tdict/the view types even though their real base
// is now a plain, non-ABCMeta mixin (see build_abc_subtype()'s own comment,
// and pcollections.abc._core's _PersistentBase docstring, for why that
// mixin swap was necessary at all on CPython 3.14). Returns 0 on success, -1
// (with an exception set) on failure.
static int register_as_virtual_subclass(PyObject* abc_cls, PyTypeObject* concrete) {
   PyObject* result = PyObject_CallMethod(abc_cls, "register", "O", (PyObject*)concrete);
   if (!result) return -1;
   Py_DECREF(result);
   return 0;
}
static int register_virtual_subclass(PyObject* module, const char* attr_name,
                                      PyTypeObject* concrete) {
   PyObject* abc_cls;
   int rc;
   abc_cls = PyObject_GetAttrString(module, attr_name);
   if (!abc_cls) return -1;
   rc = register_as_virtual_subclass(abc_cls, concrete);
   Py_DECREF(abc_cls);
   return rc;
}

// Builds one of the six view heap types, inheriting from `base` (a plain,
// non-ABCMeta mixin from pcollections.abc._view -- _KeysViewBase/
// _ItemsViewBase/_ValuesViewBase -- see that module's docstring for why
// these are used instead of collections.abc.KeysView/ItemsView/ValuesView
// directly as of this ABCMeta fix). Unlike
// build_abc_subtype() above, this fills in spec->basicsize dynamically from
// base's own tp_basicsize right before construction -- see the file-level
// comment on the view-type globals for why: these types add no native
// fields of their own, so they need exactly as much room as their ABC base
// already reserves (for its own `_mapping` slot), no more and no less.
static PyTypeObject* build_view_type(PyType_Spec* spec, PyObject* base) {
   PyObject* bases;
   PyObject* result;
   PyTypeObject* base_t;
   PyType_Slot* slot;
   if (!PyType_Check(base)) {
      PyErr_SetString(PyExc_TypeError, "expected a type object");
      return NULL;
   }
   base_t = (PyTypeObject*)base;
   spec->basicsize = base_t->tp_basicsize;
   // Patch in the base's own tp_traverse/tp_clear -- see the comment on
   // the *_view_slots[] arrays above for why this is necessary at all.
   for (slot = spec->slots; slot->slot != 0; ++slot) {
      if (slot->slot == Py_tp_traverse)
         slot->pfunc = (void*)base_t->tp_traverse;
      else if (slot->slot == Py_tp_clear)
         slot->pfunc = (void*)base_t->tp_clear;
   }
   bases = PyTuple_Pack(1, base);
   if (!bases) return NULL;
   result = PyType_FromSpecWithBases(spec, bases);
   Py_DECREF(bases);
   return (PyTypeObject*)result;
}

PyMODINIT_FUNC PyInit_dict(void) {
   PyObject* m;
   PyObject* pcoll_abc_module;
   PyObject* coll_abc_module;
   PyObject* KeysView_t; PyObject* ItemsView_t; PyObject* ValuesView_t;
   PDictObject* empty;

   // The tombstone sentinel -- see the comment near dictentry_is_tombstone()
   // above. A plain object() instance works fine: it's never exposed to
   // Python code, compared only by identity, and (being childless) costs
   // nothing extra during GC traversal of the rare `els` node that still
   // holds one between compactions.
   g_dict_dummy = PyObject_CallObject((PyObject*)&PyBaseObject_Type, NULL);
   if (!g_dict_dummy) return NULL;

   // pdict/tdict are built on top of pcollections.abc's plain (non-ABCMeta)
   // _PersistentMappingBase/_TransientMappingBase mixins -- not the real,
   // ABCMeta-based PersistentMapping/TransientMapping -- since CPython 3.14
   // rejects building a heap type (via PyType_FromSpecWithBases, which is
   // what build_abc_subtype() does) on top of a base whose metaclass
   // overrides tp_new, which ABCMeta does. See pcollections.abc._core's
   // _PersistentBase docstring for the full story. We keep pcoll_abc_module
   // open past this point (rather than decref'ing it immediately) because
   // it's needed twice more below: to .register() PDictType/TDictType as
   // virtual subclasses of the real PersistentMapping/TransientMapping, and
   // to look up the dict-view family's own plain mixins
   // (_KeysViewBase/_ItemsViewBase/_ValuesViewBase, see pcollections.abc._view)
   // for build_view_type().
   pcoll_abc_module = PyImport_ImportModule("pcollections.abc");
   if (!pcoll_abc_module) return NULL;
   PDictType = build_abc_subtype(&pdict_spec, pcoll_abc_module, "_PersistentMappingBase");
   TDictType = build_abc_subtype(&tdict_spec, pcoll_abc_module, "_TransientMappingBase");
   if (!PDictType || !TDictType) { Py_DECREF(pcoll_abc_module); return NULL; }
   // .register() PDictType/TDictType as virtual subclasses of the real
   // PersistentMapping/TransientMapping ABCs, so isinstance()/issubclass()
   // checks against pcollections.abc.PersistentMapping/TransientMapping --
   // and, transitively, against collections.abc.Mapping/MutableMapping,
   // confirmed empirically -- keep succeeding exactly as they did when
   // PDictType/TDictType were real (not virtual) subclasses of those ABCs.
   if (register_virtual_subclass(pcoll_abc_module, "PersistentMapping", PDictType) < 0) {
      Py_DECREF(pcoll_abc_module); return NULL;
   }
   if (register_virtual_subclass(pcoll_abc_module, "TransientMapping", TDictType) < 0) {
      Py_DECREF(pcoll_abc_module); return NULL;
   }
   // tdict provides no tp_hash of its own at all (unlike pdict) -- patch in
   // TransientMapping's (i.e. MutableMapping's) __hash__ = None
   // unhashability directly, the same safe way build_abc_subtype() patches
   // tp_richcompare (a concrete function -- PyObject_HashNotImplemented --
   // not a generic MRO-dispatching one, so this one's not even at risk of
   // that self-reference loop, but going through spec slots isn't needed
   // either way).
   TDictType->tp_hash = ((PyTypeObject*)TDictType)->tp_base->tp_hash;

   {
      PyObject* util_module = PyImport_ImportModule("pcollections.util");
      if (!util_module) { Py_DECREF(pcoll_abc_module); return NULL; }
      g_seqstr = PyObject_GetAttrString(util_module, "seqstr");
      Py_DECREF(util_module);
      if (!g_seqstr) { Py_DECREF(pcoll_abc_module); return NULL; }
   }

   coll_abc_module = PyImport_ImportModule("collections.abc");
   if (!coll_abc_module) { Py_DECREF(pcoll_abc_module); return NULL; }
   g_abc_Mapping = PyObject_GetAttrString(coll_abc_module, "Mapping");
   g_abc_Sized = PyObject_GetAttrString(coll_abc_module, "Sized");
   // The *real* stdlib KeysView/ItemsView/ValuesView -- kept only so the six
   // view heap types built below can be .register()'ed with them (for
   // isinstance()/issubclass() compatibility); they are NOT used as those
   // types' actual base anymore (see build_view_type()'s comment and
   // pcollections.abc._view's module docstring).
   KeysView_t = PyObject_GetAttrString(coll_abc_module, "KeysView");
   ItemsView_t = PyObject_GetAttrString(coll_abc_module, "ItemsView");
   ValuesView_t = PyObject_GetAttrString(coll_abc_module, "ValuesView");
   Py_DECREF(coll_abc_module);
   if (!g_abc_Mapping || !g_abc_Sized || !KeysView_t || !ItemsView_t || !ValuesView_t) {
      Py_XDECREF(KeysView_t); Py_XDECREF(ItemsView_t); Py_XDECREF(ValuesView_t);
      Py_DECREF(pcoll_abc_module);
      return NULL;
   }

   {
      // The plain (non-ABCMeta) mixins that build_view_type() actually
      // builds the six view heap types on top of -- see _view.py.
      PyObject* PKeysViewBase = PyObject_GetAttrString(pcoll_abc_module, "_KeysViewBase");
      PyObject* PItemsViewBase = PyObject_GetAttrString(pcoll_abc_module, "_ItemsViewBase");
      PyObject* PValuesViewBase = PyObject_GetAttrString(pcoll_abc_module, "_ValuesViewBase");
      if (!PKeysViewBase || !PItemsViewBase || !PValuesViewBase) {
         Py_XDECREF(PKeysViewBase); Py_XDECREF(PItemsViewBase); Py_XDECREF(PValuesViewBase);
         Py_DECREF(KeysView_t); Py_DECREF(ItemsView_t); Py_DECREF(ValuesView_t);
         Py_DECREF(pcoll_abc_module);
         return NULL;
      }
      PDictKeysType = build_view_type(&pdict_keys_view_spec, PKeysViewBase);
      PDictItemsType = build_view_type(&pdict_items_view_spec, PItemsViewBase);
      PDictValuesType = build_view_type(&pdict_values_view_spec, PValuesViewBase);
      TDictKeysType = build_view_type(&tdict_keys_view_spec, PKeysViewBase);
      TDictItemsType = build_view_type(&tdict_items_view_spec, PItemsViewBase);
      TDictValuesType = build_view_type(&tdict_values_view_spec, PValuesViewBase);
      Py_DECREF(PKeysViewBase); Py_DECREF(PItemsViewBase); Py_DECREF(PValuesViewBase);
   }
   Py_DECREF(pcoll_abc_module);
   if (!PDictKeysType || !PDictItemsType || !PDictValuesType ||
       !TDictKeysType || !TDictItemsType || !TDictValuesType) {
      Py_DECREF(KeysView_t); Py_DECREF(ItemsView_t); Py_DECREF(ValuesView_t);
      return NULL;
   }
   // .register() the six view types with the real stdlib KeysView/ItemsView/
   // ValuesView (see register_as_virtual_subclass()'s comment) --
   // transitively also satisfies isinstance/issubclass checks against
   // Set/Collection/Iterable/Container/Sized for the Keys/Items views,
   // confirmed empirically.
   if (register_as_virtual_subclass(KeysView_t, PDictKeysType) < 0 ||
       register_as_virtual_subclass(KeysView_t, TDictKeysType) < 0 ||
       register_as_virtual_subclass(ItemsView_t, PDictItemsType) < 0 ||
       register_as_virtual_subclass(ItemsView_t, TDictItemsType) < 0 ||
       register_as_virtual_subclass(ValuesView_t, PDictValuesType) < 0 ||
       register_as_virtual_subclass(ValuesView_t, TDictValuesType) < 0) {
      Py_DECREF(KeysView_t); Py_DECREF(ItemsView_t); Py_DECREF(ValuesView_t);
      return NULL;
   }
   Py_DECREF(KeysView_t); Py_DECREF(ItemsView_t); Py_DECREF(ValuesView_t);

   if (PyType_Ready(&PDictIterType) < 0) return NULL;
   if (PyType_Ready(&TDictIterType) < 0) return NULL;

   // Build the canonical empty pdict singleton by hand (pdict_wrap() itself
   // depends on it already existing, so it can't be used here).
   // Use tp_alloc here too (see pdict_wrap_astype's comment) for consistency
   // with every other PDictObject allocation site, and so this singleton's
   // dealloc (should it ever run) balances correctly against tp_alloc's
   // Py_INCREF(PDictType).
   empty = (PDictObject*)PDictType->tp_alloc(PDictType, 0);
   if (!empty) return NULL;
   empty->els = fat_empty(ELSLEAFSIZE);
   empty->idx = amt_empty(IDXLEAFSIZE);
   empty->top = 0;
   empty->count = 0;
   empty->ndeleted = 0;
   empty->hashcode = -1;
   g_pdict_empty = empty;

   if (PyDict_SetItemString(PDictType->tp_dict, "empty",
                            (PyObject*)g_pdict_empty) < 0)
      return NULL;
   PyType_Modified(PDictType);

   m = PyModule_Create(&dict_module);
   if (!m) return NULL;

   Py_INCREF(PDictType);
   if (PyModule_AddObject(m, "pdict", (PyObject*)PDictType) < 0) {
      Py_DECREF(PDictType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TDictType);
   if (PyModule_AddObject(m, "tdict", (PyObject*)TDictType) < 0) {
      Py_DECREF(TDictType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(PDictKeysType);
   if (PyModule_AddObject(m, "pdict_keys", (PyObject*)PDictKeysType) < 0) {
      Py_DECREF(PDictKeysType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(PDictItemsType);
   if (PyModule_AddObject(m, "pdict_items", (PyObject*)PDictItemsType) < 0) {
      Py_DECREF(PDictItemsType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(PDictValuesType);
   if (PyModule_AddObject(m, "pdict_values", (PyObject*)PDictValuesType) < 0) {
      Py_DECREF(PDictValuesType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TDictKeysType);
   if (PyModule_AddObject(m, "tdict_keys", (PyObject*)TDictKeysType) < 0) {
      Py_DECREF(TDictKeysType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TDictItemsType);
   if (PyModule_AddObject(m, "tdict_items", (PyObject*)TDictItemsType) < 0) {
      Py_DECREF(TDictItemsType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TDictValuesType);
   if (PyModule_AddObject(m, "tdict_values", (PyObject*)TDictValuesType) < 0) {
      Py_DECREF(TDictValuesType); Py_DECREF(m); return NULL;
   }
   return m;
}
