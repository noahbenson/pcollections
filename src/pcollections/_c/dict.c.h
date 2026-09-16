///////////////////////////////////////////////////////////////////////////////
// _c/dict.c.h
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
//    like plist/tlist's own FAT-tree list encoding in list.c.h -- except a
//    dict's `top` only ever grows via new insertions, so unlike a list's
//    `start` there is no prepend-style operation and therefore no need for
//    list.c.h's LIST_START_MID wraparound trick: index 0 is always safe).
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
// FAT, which (like list.c.h's plist/tlist encoding) maintains a strictly
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
// heap-type technique list.c.h uses for plist/tlist. Unlike plist/tlist,
// pdict/tdict *are* subclassable in C (Py_TPFLAGS_BASETYPE is set on both --
// see pdict_spec's comment): pcollections._c._core's ldict/tldict subclass
// them directly, exactly mirroring the reference _lazy.py's
// `class ldict(pdict)`/`class tldict(tdict)`. Every construction site in
// this file that the reference spells as `self._new(...)`/`cls._new(...)`
// (rather than naming `pdict`/`tdict` explicitly) is written here in terms
// of `Py_TYPE(self)`/a threaded-through `type`/`cls` parameter instead of a
// hardcoded PDictType/TDictType, so that e.g. `some_ldict.set(...)` returns
// another ldict rather than silently downgrading to a plain pdict on first
// mutation -- see pdict_wrap_astype()/tdict_wrap_astype()/
// pdict_new_dispatch()/tdict_new() for where this actually happens.


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
// Leaf refcounting callbacks.

// els leaves (DictEntry) own a reference to both their key and value.
// A tombstone's key and value are both NULL.
static void dictentry_incref(void* v) {
   DictEntry* e = (DictEntry*)v;
   Py_XINCREF(e->key);
   Py_XINCREF(e->val);
}
static void dictentry_decref(void* v) {
   DictEntry* e = (DictEntry*)v;
   Py_XDECREF(e->key);
   Py_XDECREF(e->val);
}
// idx leaves are raw FAT indices; they use noop_incref/noop_decref (core.h).

//=============================================================================
// Tombstones.
//
// IMPORTANT correction to the design-comment at the top of this file: `els`
// (a FAT) can NOT hold "holes" the way that comment originally assumed.
// FAT maintains a genuinely *dense* invariant -- exactly like a Python list
// (which is exactly what it's used for in list.c.h) -- so removing a key from
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
// A tombstone is a DictEntry whose key and value are both NULL. Every direct
// `els`-walking loop (dict_rebuild_compacted(), pdict_repr()/tdict_repr(),
// pdict_hash(), dictiter_next(), the GC traversal) checks for it and skips it.
// A dict_chain_find() walk never lands on a tombstone: an entry is always
// unlinked from its collision chain before it is tombstoned, so __getitem__,
// __contains__, get(), set() and friends need no check.
static int dictentry_is_tombstone(const DictEntry* e) {
   return e->key == NULL;
}
static void dict_make_tombstone(DictEntry* out) {
   out->key = NULL;
   out->val = NULL;
   out->next = DICT_NO_NEXT;
}




//=============================================================================
// Hash-key conversion: Python's hash() returns a signed Py_hash_t; AMT keys
// are unsigned trieint_t. Since idx is a genuine hash table (not an ordered
// list like els), all we need is a bit-preserving reinterpretation -- there
// is no ordering or wraparound-headroom concern here at all (contrast
// list.c.h's LIST_START_MID, which exists only because FAT list keys *do*
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
// chain in `els`, starting from the chain head recorded in `idx`.
//
// Returns 1 if `key` is found, setting *out_index to the matching entry's FAT
// index, *out_val (if non-NULL) to its value (borrowed), and *out_prev (if
// non-NULL) to the index of the entry before it in the chain, or
// DICT_NO_NEXT if it is the chain's head; these are what a deletion needs to
// unlink it. Returns 0 if `key` is absent, setting *out_prev to the index of
// the chain's last entry, or DICT_NO_NEXT if there is no chain for the hash;
// this is where an insertion links the new entry. Returns -1 with an
// exception set if a comparison fails.
//
// Comparing keys calls user code. For a transient, `g` is its guard and
// `owner` the transient: if the transient changes during a comparison, the
// lookup raises RuntimeError rather than keep walking entries that may no
// longer exist. For a persistent dict both are NULL.
static int dict_chain_find(Trie_t els, Trie_t idx, trieint_t hkey,
                           PyObject* key, trieint_t* out_index,
                           PyObject** out_val, trieint_t* out_prev,
                           const pcoll_tguard* g, PyObject* owner) {
   void* found;
   trieint_t ii;
   trieint_t prev = DICT_NO_NEXT;
   uint64_t version = g ? g->version : 0;
   if (!amt_lookup(idx, hkey, &found)) {
      if (out_prev) *out_prev = DICT_NO_NEXT;
      return 0;
   }
   ii = *(trieint_t*)found;
   while (1) {
      void* ep = NULL;
      DictEntry* e;
      PyObject* ekey;
      int eq;
      fat_lookup(els, ii, &ep);
      e = (DictEntry*)ep;
      ekey = e->key;
      if (ekey == key) {
         eq = 1;
      } else {
         Py_INCREF(ekey);
         eq = PyObject_RichCompareBool(key, ekey, Py_EQ);
         Py_DECREF(ekey);
         if (g && g->version != version) {
            if (eq >= 0)
               tguard_lookup_error(Py_TYPE(owner)->tp_name);
            return -1;
         }
         if (eq < 0) return -1;
      }
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

// Returns the index of the last entry in the chain for `hkey`, or
// DICT_NO_NEXT if there is no such chain. Calls no user code.
static trieint_t dict_chain_tail(Trie_t els, Trie_t idx, trieint_t hkey) {
   void* found;
   trieint_t ii;
   if (!amt_lookup(idx, hkey, &found)) return DICT_NO_NEXT;
   ii = *(trieint_t*)found;
   while (1) {
      void* ep = NULL;
      fat_lookup(els, ii, &ep);
      if (((DictEntry*)ep)->next == DICT_NO_NEXT) return ii;
      ii = ((DictEntry*)ep)->next;
   }
}


//=============================================================================
// pdict

typedef struct PDictObject {
   PyObject_HEAD
   Trie_t idx;           // AMT: hash -> first FAT index. Persistent.
   Trie_t els;           // FAT: index -> DictEntry. Persistent.
   Py_ssize_t top;        // next fresh index to hand out.
   Py_ssize_t count;      // number of live entries (== len()).
   Py_ssize_t ndeleted;   // holes burned by deletions since last compaction.
   Py_hash_t hashcode;    // -1 == not yet computed.
} PDictObject;

// The types, the empty pdict, and the cached imports this part uses live in
// the module state (core.h): ST(PDictType), ST(g_pdict_empty), and so on.

static PyObject* pdict_keys(PDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_items(PDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_values(PDictObject* self, PyObject* Py_UNUSED(ignored));

static int pdict_traverse(PDictObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   // The trie reports its own contents (see fat.h); the idx AMT holds no
   // Python references.
   Py_VISIT((PyObject*)self->els);
   return 0;
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
// plist_wrap's identical convention in list.c.h), but *only* when `type` is
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
   if (count == 0 && type == ST(PDictType)) {
      fatnode_decref(els, dictentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_INCREF(ST(g_pdict_empty));
      return (PyObject*)ST(g_pdict_empty);
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
   return pdict_wrap_astype(ST(PDictType), els, idx, top, count, ndeleted);
}

// Returns the canonical empty instance of `type` (a new reference), mirroring
// the reference's `cls.empty` attribute lookup in pdict.__new__. For plain
// PDictType this is just the g_pdict_empty singleton built once at
// module execution time; for any other (necessarily subclass) type, the class is
// expected to have its own `.empty` class attribute set up the same way
// PDictType/PListType set theirs -- lazy.c.h does this for ldict at its own
// init time.
static PyObject* pdict_type_empty(PyTypeObject* type) {
   if (type == ST(PDictType)) {
      Py_INCREF(ST(g_pdict_empty));
      return (PyObject*)ST(g_pdict_empty);
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
      // The keys are distinct, so each one starts a new chain or extends
      // an existing one; no comparisons (and so no user code) are needed.
      prev = dict_chain_tail(new_els, new_idx, hkey);
      if (prev == DICT_NO_NEXT) {
         trieint_t idxval = newtop;
         new_idx = tamt_setitem(new_idx, hkey, &idxval, noop_incref, noop_decref);
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
// exactly mirroring plist_new_dispatch()'s relationship to tlist in list.c.h.
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
   PyObject* s = call_seqstr((PyObject*)self, 0, 0, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
// Matches abc/_map.py's PersistentMapping.__str__:
// f"{{|{seqstr(self, maxlen=60)}|}}" (truncated at 60 chars).
static PyObject* pdict_str(PDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1, NULL);
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
   result = PCOLL_HASH_LOAD(self->hashcode);
   if (result != -1) return result;
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
   PCOLL_HASH_STORE(self->hashcode, result);
   return result;
}

static PyObject* pdict_subscript(PDictObject* self, PyObject* key) {
   trieint_t hkey;
   trieint_t found_index;
   PyObject* val;
   int found;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                           &val, NULL, NULL, NULL);
   if (found <= 0) {
      if (found == 0) PyErr_SetObject(PyExc_KeyError, key);
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
   int found;
   if (!PyArg_ParseTuple(args, "O|O", &key, &deflt)) return NULL;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   found = dict_chain_find(self->els, self->idx, hkey, key, NULL, &val,
                           NULL, NULL, NULL);
   if (found < 0) return NULL;
   if (!found) {
      Py_INCREF(deflt);
      return deflt;
   }
   Py_INCREF(val);
   return val;
}
static int pdict_contains(PDictObject* self, PyObject* key) {
   trieint_t hkey;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   return dict_chain_find(self->els, self->idx, hkey, key, NULL, NULL, NULL,
                          NULL, NULL);
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
                           &old_val, &prev, NULL, NULL);
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
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                           NULL, &prev, NULL, NULL);
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
   Py_INCREF(ST(g_pdict_empty));
   return (PyObject*)ST(g_pdict_empty);
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
   .name = "pcollections.pdict",
   .basicsize = sizeof(PDictObject),
   .itemsize = 0,
   // Py_TPFLAGS_BASETYPE: pdict *is* meant to be subclassed in C -- see
   // pcollections._c._core's ldict, which adds no fields of its own (matching
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
                            // matching field on TListObject in list.c.h for
                            // the exact caching/invalidation convention.
   pcoll_tguard guard;      // see core.h.
} TDictObject;

static int tdict_traverse(TDictObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->orig);
   // The trie reports its own contents (see fat.h); the idx AMT holds no
   // Python references.
   Py_VISIT((PyObject*)self->els);
   return 0;
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
static Py_ssize_t tdict_length_impl(TDictObject* self) {
   return self->count;
}
PCOLL_LOCKED0(Py_ssize_t, tdict_length, tdict_length_impl, TDictObject*)
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
   return tdict_wrap_astype(ST(TDictType), els, idx, top, count, ndeleted, orig);
}
static void tdict_invalidate_orig(TDictObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

// Freezes the transient's tries and returns new references to them, for
// sharing with another collection. Returns -1 (with RuntimeError set) if a
// modification is in progress. The caller holds the transient's lock.
static int tdict_share(TDictObject* self, Trie_t* els, Trie_t* idx) {
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return -1;
   fat_freeze(self->els);
   amt_freeze(self->idx);
   trienode_incref(self->els);
   trienode_incref(self->idx);
   *els = self->els;
   *idx = self->idx;
   return 0;
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
   is_mapping = PyObject_IsInstance(arg, ST(g_abc_Mapping));
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
   // correctly *dereferences* rather than raw-sharing -- see lazy.c.h's
   // ldict/tldict for why that distinction matters.
   if (Py_TYPE(arg) == ST(TDictType)) {
      // tdict(some_tdict): share a frozen snapshot of that tdict's current
      // (els, idx) -- not the tdict's own live, still-mutable trees -- as
      // the starting point for a brand-new, independent transient session.
      // Also propagates `_orig`: if the source tdict still has a valid
      // cached original pdict, this new tdict is (right now) an exact copy
      // of that same pdict too, so it's valid to share that same cache.
      TDictObject* t = (TDictObject*)arg;
      Trie_t els, idx;
      PyObject* orig = NULL;
      Py_ssize_t top = 0, count = 0, ndeleted = 0;
      int rc;
      PCOLL_BEGIN_LOCK(arg);
      rc = tdict_share(t, &els, &idx);
      if (rc == 0) {
         // The cached original is a pdict, so it is kept only when making a
         // plain tdict.
         orig = (type == ST(TDictType)) ? t->orig : NULL;
         Py_XINCREF(orig);
         top = t->top; count = t->count; ndeleted = t->ndeleted;
      }
      PCOLL_END_LOCK();
      if (rc < 0) return NULL;
      obj = tdict_wrap_astype(type, els, idx, top, count, ndeleted, orig);
   } else if (Py_TYPE(arg) == ST(PDictType)) {
      // tdict(some_pdict): the same O(1) sharing logic as pdict_transient()
      // (below), just parameterized by `type` instead of hardcoded to
      // TDictType -- `cls._new(...)` in the reference's tdict.__new__, not
      // `tdict._new(...)`, so this must build a `type` instance, which may
      // be a subclass (e.g. tdict(some_pdict) called by way of
      // tldict.__new__ delegating to tdict.__new__).
      PDictObject* p = (PDictObject*)arg;
      PyObject* orig = (type == ST(TDictType)) ? arg : NULL;
      trienode_incref(p->els);
      trienode_incref(p->idx);
      Py_XINCREF(orig);
      obj = tdict_wrap_astype(type, p->els, p->idx, p->top, p->count,
                                p->ndeleted, orig);
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
   PyObject* s = call_seqstr((PyObject*)self, 0, 0, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}
// Matches abc/_map.py's TransientMapping.__str__:
// f"{{<{seqstr(self, maxlen=60)}>}}" (truncated, "{<...>}" delimiter).
static PyObject* tdict_str(TDictObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}

// Looks `key` up in the transient; returns as dict_chain_find() does, with
// *out_val set to a new reference on success. The hash is computed first,
// since __hash__ may itself change the transient.
static int tdict_lookup(TDictObject* self, PyObject* key, PyObject** out_val) {
   trieint_t hkey;
   PyObject* val;
   int found;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   found = dict_chain_find(self->els, self->idx, hkey, key, NULL, &val, NULL,
                           &self->guard, (PyObject*)self);
   if (found > 0 && out_val) {
      Py_INCREF(val);
      *out_val = val;
   }
   return found;
}
static PyObject* tdict_subscript_impl(TDictObject* self, PyObject* key) {
   PyObject* val = NULL;
   int found = tdict_lookup(self, key, &val);
   if (found == 0) PyErr_SetObject(PyExc_KeyError, key);
   return val;
}
PCOLL_LOCKED1(PyObject*, tdict_subscript, tdict_subscript_impl,
              TDictObject*, PyObject*)
static PyObject* tdict_get_impl(TDictObject* self, PyObject* args) {
   PyObject* key; PyObject* deflt = Py_None;
   PyObject* val = NULL;
   int found;
   if (!PyArg_ParseTuple(args, "O|O", &key, &deflt)) return NULL;
   found = tdict_lookup(self, key, &val);
   if (found < 0) return NULL;
   if (!found) {
      Py_INCREF(deflt);
      return deflt;
   }
   return val;
}
PCOLL_LOCKED1(PyObject*, tdict_get, tdict_get_impl, TDictObject*, PyObject*)
static int tdict_contains_impl(TDictObject* self, PyObject* key) {
   return tdict_lookup(self, key, NULL);
}
PCOLL_LOCKED1(int, tdict_contains, tdict_contains_impl, TDictObject*, PyObject*)

// Maybe-compact a tdict in place after a mutation. Always succeeds unless
// re-hashing a key fails (see dict_rebuild_compacted).
static int tdict_maybe_compact(TDictObject* self) {
   Trie_t c_els, c_idx; Py_ssize_t c_top;
   if (!dict_should_compact(self->count, self->ndeleted)) return 0;
   Trie_t old_els = self->els, old_idx = self->idx;
   if (dict_rebuild_compacted(old_els, old_idx, &c_els, &c_idx, &c_top) < 0)
      return -1;
   self->els = c_els; self->idx = c_idx; self->top = c_top; self->ndeleted = 0;
   fatnode_decref(old_els, dictentry_decref);
   amtnode_decref(old_idx, noop_decref);
   return 0;
}

// The body of tdict_ass_subscript(), run with the guard held.
static int tdict_ass_subscript_guarded(TDictObject* self, trieint_t hkey,
                                       PyObject* key, PyObject* val) {
   trieint_t found_index, prev;
   PyObject* old_val;
   int found;
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                           &old_val, &prev, &self->guard, (PyObject*)self);
   if (found < 0) return -1;
   if (val == NULL) {
      // __delitem__.
      void* ep; DictEntry entry; DictEntry tombstone;
      if (!found) {
         PyErr_SetObject(PyExc_KeyError, key);
         return -1;
      }
      tguard_keys_changed(&self->guard);
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
         tfat_setitem_at(&self->els, prev, &pentry,
                         dictentry_incref, dictentry_decref);
      }
      self->count -= 1;
      self->ndeleted += 1;
      // OVERWRITE found_index's slot with a tombstone -- never delete it,
      // which would shift every later index down by one and invalidate the
      // idx/collision-chain pointers; see the tombstone comment near
      // dictentry_is_tombstone().
      tfat_setitem_at(&self->els, found_index, &tombstone,
                      dictentry_incref, dictentry_decref);
      tdict_invalidate_orig(self);
      return tdict_maybe_compact(self);
   }
   // __setitem__.
   if (found) {
      if (val != old_val) {
         void* ep; DictEntry entry;
         tguard_changed(&self->guard);
         fat_lookup(self->els, found_index, &ep);
         memcpy(&entry, ep, sizeof(DictEntry));
         entry.val = val;
         tfat_setitem_at(&self->els, found_index, &entry,
                         dictentry_incref, dictentry_decref);
         tdict_invalidate_orig(self);
      }
      return 0;
   }
   {
      DictEntry entry;
      trieint_t newindex = (trieint_t)self->top;
      tguard_keys_changed(&self->guard);
      entry.key = key; entry.val = val; entry.next = DICT_NO_NEXT;
      tfat_setitem_at(&self->els, newindex, &entry,
                      dictentry_incref, dictentry_decref);
      if (prev == DICT_NO_NEXT) {
         self->idx = tamt_setitem(self->idx, hkey, &newindex, noop_incref, noop_decref);
      } else {
         void* ep; DictEntry patched;
         fat_lookup(self->els, prev, &ep);
         memcpy(&patched, ep, sizeof(DictEntry));
         patched.next = newindex;
         tfat_setitem_at(&self->els, prev, &patched,
                         dictentry_incref, dictentry_decref);
      }
      self->top += 1;
      self->count += 1;
   }
   tdict_invalidate_orig(self);
   return tdict_maybe_compact(self);
}

static int tdict_ass_subscript_impl(TDictObject* self, PyObject* key, PyObject* val) {
   trieint_t hkey;
   int rc;
   if (dict_hash_key(key, &hkey) < 0) return -1;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return -1;
   rc = tdict_ass_subscript_guarded(self, hkey, key, val);
   tguard_exit(&self->guard);
   return rc;
}
PCOLL_LOCKED2(int, tdict_ass_subscript, tdict_ass_subscript_impl,
              TDictObject*, PyObject*, PyObject*)

static PyObject* tdict_clear_impl(TDictObject* self) {
   Trie_t old_els = self->els, old_idx = self->idx;
   PyObject* old_orig = self->orig;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return NULL;
   tguard_keys_changed(&self->guard);
   self->els = fat_empty(ELSLEAFSIZE);
   self->idx = amt_empty(IDXLEAFSIZE);
   self->top = 0; self->count = 0; self->ndeleted = 0;
   self->orig = NULL;
   tguard_exit(&self->guard);
   fatnode_decref(old_els, dictentry_decref);
   amtnode_decref(old_idx, noop_decref);
   Py_XDECREF(old_orig);
   Py_RETURN_NONE;
}
PCOLL_LOCKED0(PyObject*, tdict_clear_locked, tdict_clear_impl, TDictObject*)
static PyObject* tdict_clear_method(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return tdict_clear_locked(self);
}

static PyObject* pdict_wrap(Trie_t els, Trie_t idx, Py_ssize_t top,
                             Py_ssize_t count, Py_ssize_t ndeleted);

static PyObject* tdict_persistent_impl(TDictObject* self) {
   Trie_t els, idx;
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return NULL;
   if (self->count == 0) {
      Py_INCREF(ST(g_pdict_empty));
      return (PyObject*)ST(g_pdict_empty);
   }
   if (self->orig) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   if (tdict_share(self, &els, &idx) < 0) return NULL;
   return pdict_wrap(els, idx, self->top, self->count, self->ndeleted);
}
PCOLL_LOCKED0(PyObject*, tdict_persistent_locked, tdict_persistent_impl,
              TDictObject*)
static PyObject* tdict_persistent(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return tdict_persistent_locked(self);
}

static PyObject* tdict_keys(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* tdict_items(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* tdict_values(TDictObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   trienode_incref(self->els);
   trienode_incref(self->idx);
   // tdict_wrap() takes ownership of (does not itself incref) the `orig`
   // reference it's handed -- self must be incref'd here, exactly as
   // plist_transient() does in list.c.h, or the returned tdict ends up holding
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
   .name = "pcollections.tdict",
   .basicsize = sizeof(TDictObject),
   .itemsize = 0,
   // See pdict_spec's comment on Py_TPFLAGS_BASETYPE -- tldict (lazy.c.h) is
   // the subclass this enables here.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tdict_slots,
};


//=============================================================================
// pdict_new()'s general-argument routing (both the "n==1, general iterable"
// case and the "n==0, kwargs-only" case) is defined down here, now that
// tdict_new()/tdict_persistent() both exist -- mirrors plist_new_dispatch()'s
// relationship to tlist in list.c.h exactly.

static PyObject* pdict_new_dispatch(PyTypeObject* type, PyObject* arg, PyObject* kw) {
   PyObject* targs;
   PyObject* t;
   PyObject* result;
   Py_ssize_t kwn = (kw ? PyDict_Size(kw) : 0);
   if (kwn == 0 && arg != NULL) {
      int sized = PyObject_IsInstance(arg, ST(g_abc_Sized));
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
      // lazy.c.h's tldict for why that's the reference's actual, if slightly
      // surprising, behavior for a *transient* lazy-dict argument, as
      // opposed to a *persistent* one).
      // Storage is shared only with a collection that is not lazy: reading
      // from a lazy collection computes its values (see _lazy.py).
      if (PyObject_TypeCheck(arg, ST(TDictType)) && !pcoll_holds_lazy(arg)) {
         TDictObject* t2 = (TDictObject*)arg;
         Trie_t els, idx;
         Py_ssize_t top = 0, count = 0, ndeleted = 0;
         int rc;
         PCOLL_BEGIN_LOCK(arg);
         rc = tdict_share(t2, &els, &idx);
         if (rc == 0) {
            top = t2->top; count = t2->count; ndeleted = t2->ndeleted;
         }
         PCOLL_END_LOCK();
         if (rc < 0) return NULL;
         if (count == 0 && type != ST(PDictType)) {
            fatnode_decref(els, dictentry_decref);
            amtnode_decref(idx, noop_decref);
            return pdict_type_empty(type);
         }
         return pdict_wrap_astype(type, els, idx, top, count, ndeleted);
      }
      if (Py_TYPE(arg) == type) {
         // type(arg) is cls in the reference: arg is already exactly the
         // type being constructed -- return it unchanged.
         Py_INCREF(arg);
         return arg;
      }
      if (Py_TYPE(arg) == ST(PDictType)) {
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
   t = tdict_new(ST(TDictType), targs, kw);
   Py_DECREF(targs);
   if (!t) return NULL;
   {
      TDictObject* tobj = (TDictObject*)t;
      if (tobj->count == 0) {
         Py_DECREF(t);
         result = pdict_type_empty(type);
      } else {
         Trie_t els, idx;
         (void)tdict_share(tobj, &els, &idx);  // tobj is private: cannot fail.
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
// exactly mirroring list.c.h's plist_iterator/tlist_iterator split) is reused
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
   int state;          // 0 = not yet started, 1 = active, 2 = exhausted,
                       // 3 = failed (the transient changed).
   DictIterMode mode;
   bool transient;     // whether owner is a tdict.
   // For a transient owner: its size and guard counters when the iterator
   // was created or last moved (see dictiter_next()).
   Py_ssize_t count;
   uint64_t version;
   uint64_t keyversion;
   trieint_t nextkey;  // the key after the last one yielded.
} DictIterObject;

static void dictiter_dealloc(DictIterObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   Py_XDECREF(self->owner);
   PyObject_GC_Del(self);
   Py_DECREF(tp);
}
static int dictiter_traverse(DictIterObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->owner);
   return 0;
}
// Finds the first live entry at or after the path's current position
// (`ok` says whether there is one) and returns it in the iterator's mode.
static PyObject* dictiter_yield(DictIterObject* self, int ok) {
   DictEntry* e;
   // Skip tombstoned (deleted-but-not-yet-compacted) slots -- see the
   // tombstone comment near dictentry_is_tombstone() above.
   while (ok && dictentry_is_tombstone((DictEntry*)triepath_val(&self->path)))
      ok = fat_nextpath(&self->path);
   if (!ok) { self->state = 2; return NULL; }
   e = (DictEntry*)triepath_val(&self->path);
   self->nextkey = triepath_key(&self->path) + 1;
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
// A persistent owner never changes, so its iterator just walks the trie. A
// transient owner may change between steps, and its iterator's path may then
// point at nodes that no longer exist; so, like the builtin dict's iterators,
// the iterator raises RuntimeError if the size or the set of keys changed,
// and otherwise (only values were replaced) finds its place again by key.
static PyObject* dictiter_next_impl(DictIterObject* self) {
   Trie_t els;
   int ok;
   if (self->state == 2) return NULL;
   if (!self->transient) {
      PDictObject* p = (PDictObject*)self->owner;
      if (self->state == 0) {
         self->state = 1;
         return dictiter_yield(self, fat_firstpath(p->els, &self->path));
      }
      return dictiter_yield(self, fat_nextpath(&self->path));
   } else {
      TDictObject* t = (TDictObject*)self->owner;
      const char* name = Py_TYPE(t)->tp_name;
      PyObject* result;
      els = t->els;
      if (els == NULL) { self->state = 2; return NULL; }
      if (self->state != 3 && t->count != self->count) {
         self->state = 3;
         PyErr_Format(PyExc_RuntimeError,
                      "%.200s changed size during iteration", name);
         return NULL;
      }
      if (self->state == 3 || t->guard.keyversion != self->keyversion) {
         self->state = 3;
         PyErr_Format(PyExc_RuntimeError,
                      "%.200s keys changed during iteration", name);
         return NULL;
      }
      if (self->state == 0) {
         self->state = 1;
         ok = fat_firstpath(els, &self->path);
      } else if (t->guard.version != self->version) {
         ok = fat_seekpath(els, self->nextkey, &self->path);
      } else {
         ok = fat_nextpath(&self->path);
      }
      self->version = t->guard.version;
      result = dictiter_yield(self, ok);
      return result;
   }
}
static PyObject* dictiter_next(DictIterObject* self) {
   PyObject* r;
   PCOLL_BEGIN_LOCK2((PyObject*)self, self->owner);
   r = dictiter_next_impl(self);
   PCOLL_END_LOCK2();
   return r;
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
   it->transient = !PyObject_TypeCheck(owner_dict, ST(PDictType));
   it->count = 0;
   it->version = 0;
   it->keyversion = 0;
   if (it->transient) {
      TDictObject* t = (TDictObject*)owner_dict;
      PCOLL_BEGIN_LOCK(owner_dict);
      it->count = t->count;
      it->version = t->guard.version;
      it->keyversion = t->guard.keyversion;
      PCOLL_END_LOCK();
   }
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

static PyType_Slot dictiter_slots[] = {
   {Py_tp_dealloc, (void*)dictiter_dealloc},
   {Py_tp_traverse, (void*)dictiter_traverse},
   {Py_tp_iter, (void*)dictiter_self},
   {Py_tp_iternext, (void*)dictiter_next},
   {0, NULL}
};
static PyType_Spec pdictiter_spec = {
   .name = "pcollections._c._core.pdict_iterator",
   .basicsize = sizeof(DictIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = dictiter_slots,
};
static PyType_Spec tdictiter_spec = {
   .name = "pcollections._c._core.tdict_iterator",
   .basicsize = sizeof(DictIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = dictiter_slots,
};

static PyObject* pdict_iter(PDictObject* self) {
   return make_dictiter(ST(PDictIterType), (PyObject*)self, DICTITER_KEYS);
}
static PyObject* tdict_iter(TDictObject* self) {
   return make_dictiter(ST(TDictIterType), (PyObject*)self, DICTITER_KEYS);
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
   if (type == ST(PDictKeysType) || type == ST(PDictItemsType) || type == ST(PDictValuesType))
      required = ST(PDictType);
   else
      required = ST(TDictType);
   // PyObject_TypeCheck (isinstance-equivalent), not an exact Py_TYPE()
   // match: `d` only needs to be layout-compatible as a PDictObject/
   // TDictObject, which holds for any subclass that adds no fields of its
   // own -- e.g. ldict/tldict (pcollections._c._core) -- so `some_ldict.keys()`
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
   it = make_dictiter(ST(PDictIterType), mapping, DICTITER_ITEMS);
   Py_DECREF(mapping);
   return it;
}
static PyObject* pdictvalues_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(ST(PDictIterType), mapping, DICTITER_VALUES);
   Py_DECREF(mapping);
   return it;
}
static PyObject* tdictitems_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(ST(TDictIterType), mapping, DICTITER_ITEMS);
   Py_DECREF(mapping);
   return it;
}
static PyObject* tdictvalues_iter(PyObject* self) {
   PyObject* mapping = dictview_get_mapping(self);
   PyObject* it;
   if (!mapping) return NULL;
   it = make_dictiter(ST(TDictIterType), mapping, DICTITER_VALUES);
   Py_DECREF(mapping);
   return it;
}

// Each array below reserves two placeholder entries for Py_tp_traverse/
// Py_tp_clear, patched in by build_view_type() at module execution time (see its
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
// .basicsize is filled in at module execution time (see build_view_type) -- it
// depends on the dynamically-imported ABC base type's own tp_basicsize.
static const PyType_Spec pdict_keys_view_spec = {
   .name = "pcollections._c._core.pdict_keys", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = dictview_keys_slots,
};
static const PyType_Spec pdict_items_view_spec = {
   .name = "pcollections._c._core.pdict_items", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = pdict_items_view_slots,
};
static const PyType_Spec pdict_values_view_spec = {
   .name = "pcollections._c._core.pdict_values", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = pdict_values_view_slots,
};
static const PyType_Spec tdict_keys_view_spec = {
   .name = "pcollections._c._core.tdict_keys", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = dictview_keys_slots,
};
static const PyType_Spec tdict_items_view_spec = {
   .name = "pcollections._c._core.tdict_items", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = tdict_items_view_slots,
};
static const PyType_Spec tdict_values_view_spec = {
   .name = "pcollections._c._core.tdict_values", .basicsize = 0, .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, .slots = tdict_values_view_slots,
};

static PyObject* pdict_keys(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(PDictKeysType), (PyObject*)self, NULL);
}
static PyObject* pdict_items(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(PDictItemsType), (PyObject*)self, NULL);
}
static PyObject* pdict_values(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(PDictValuesType), (PyObject*)self, NULL);
}
static PyObject* tdict_keys(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(TDictKeysType), (PyObject*)self, NULL);
}
static PyObject* tdict_items(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(TDictItemsType), (PyObject*)self, NULL);
}
static PyObject* tdict_values(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs((PyObject*)ST(TDictValuesType), (PyObject*)self, NULL);
}


//=============================================================================
// Module execution.

// Builds one of the six view types on top of `base` (a plain mixin from
// pcollections.abc._view). The views add no fields of their own, so their
// basicsize is exactly the base's. PyType_FromSpecWithBases does not inherit
// tp_traverse/tp_clear from a Python-defined base, so the base's are copied
// in explicitly. The static spec is copied first, so concurrent execution in
// several interpreters never writes to shared memory.
static PyTypeObject* build_view_type(PyObject* m, const PyType_Spec* spec,
                                     PyObject* base) {
   PyType_Spec local_spec;
   PyType_Slot local_slots[8];
   PyTypeObject* base_t;
   PyObject* bases;
   PyTypeObject* result;
   int n;
   if (!PyType_Check(base)) {
      PyErr_SetString(PyExc_TypeError, "expected a type object");
      return NULL;
   }
   base_t = (PyTypeObject*)base;
   local_spec = *spec;
   local_spec.basicsize = (int)base_t->tp_basicsize;
   for (n = 0; spec->slots[n].slot != 0; ++n) {
      local_slots[n] = spec->slots[n];
      if (local_slots[n].slot == Py_tp_traverse)
         local_slots[n].pfunc = (void*)base_t->tp_traverse;
      else if (local_slots[n].slot == Py_tp_clear)
         local_slots[n].pfunc = (void*)base_t->tp_clear;
   }
   local_slots[n] = spec->slots[n];
   local_spec.slots = local_slots;
   bases = PyTuple_Pack(1, base);
   if (!bases) return NULL;
   result = pcoll_new_type(m, &local_spec, bases);
   Py_DECREF(bases);
   return result;
}

// Creates pdict, tdict, their views and iterators, and the empty pdict, and
// adds the public types to module `m`. `st` is this interpreter's state.
static int pcoll_exec_dict(PyObject* m, pcoll_state* st) {
   PyObject* pcoll_abc = NULL;
   PyObject* coll_abc = NULL;
   PyObject* KeysView_t = NULL;
   PyObject* ItemsView_t = NULL;
   PyObject* ValuesView_t = NULL;
   PyObject* KeysBase = NULL;
   PyObject* ItemsBase = NULL;
   PyObject* ValuesBase = NULL;
   PDictObject* empty;
   int rc = -1;

   pcoll_abc = PyImport_ImportModule("pcollections.abc");
   if (!pcoll_abc) goto done;
   st->PDictType = build_abc_subtype(m, &pdict_spec, pcoll_abc,
                                     "_PersistentMappingBase", 1);
   if (!st->PDictType) goto done;
   st->TDictType = build_abc_subtype(m, &tdict_spec, pcoll_abc,
                                     "_TransientMappingBase", 1);
   if (!st->TDictType) goto done;
   if (register_virtual_subclass(pcoll_abc, "PersistentMapping", st->PDictType) < 0 ||
       register_virtual_subclass(pcoll_abc, "TransientMapping", st->TDictType) < 0)
      goto done;
   // tdict is unhashable, like its base (MutableMapping sets __hash__ = None).
   st->TDictType->tp_hash = st->TDictType->tp_base->tp_hash;

   coll_abc = PyImport_ImportModule("collections.abc");
   if (!coll_abc) goto done;
   st->g_abc_Mapping = PyObject_GetAttrString(coll_abc, "Mapping");
   if (!st->g_abc_Mapping) goto done;
   st->g_abc_Sized = PyObject_GetAttrString(coll_abc, "Sized");
   if (!st->g_abc_Sized) goto done;
   // The views are registered with the real stdlib view ABCs.
   KeysView_t = PyObject_GetAttrString(coll_abc, "KeysView");
   ItemsView_t = PyObject_GetAttrString(coll_abc, "ItemsView");
   ValuesView_t = PyObject_GetAttrString(coll_abc, "ValuesView");
   if (!KeysView_t || !ItemsView_t || !ValuesView_t) goto done;
   KeysBase = PyObject_GetAttrString(pcoll_abc, "_KeysViewBase");
   ItemsBase = PyObject_GetAttrString(pcoll_abc, "_ItemsViewBase");
   ValuesBase = PyObject_GetAttrString(pcoll_abc, "_ValuesViewBase");
   if (!KeysBase || !ItemsBase || !ValuesBase) goto done;
   if (!(st->PDictKeysType = build_view_type(m, &pdict_keys_view_spec, KeysBase)) ||
       !(st->PDictItemsType = build_view_type(m, &pdict_items_view_spec, ItemsBase)) ||
       !(st->PDictValuesType = build_view_type(m, &pdict_values_view_spec, ValuesBase)) ||
       !(st->TDictKeysType = build_view_type(m, &tdict_keys_view_spec, KeysBase)) ||
       !(st->TDictItemsType = build_view_type(m, &tdict_items_view_spec, ItemsBase)) ||
       !(st->TDictValuesType = build_view_type(m, &tdict_values_view_spec, ValuesBase)))
      goto done;
   if (register_as_virtual_subclass(KeysView_t, st->PDictKeysType) < 0 ||
       register_as_virtual_subclass(KeysView_t, st->TDictKeysType) < 0 ||
       register_as_virtual_subclass(ItemsView_t, st->PDictItemsType) < 0 ||
       register_as_virtual_subclass(ItemsView_t, st->TDictItemsType) < 0 ||
       register_as_virtual_subclass(ValuesView_t, st->PDictValuesType) < 0 ||
       register_as_virtual_subclass(ValuesView_t, st->TDictValuesType) < 0)
      goto done;

   if (!(st->PDictIterType = pcoll_new_internal_type(m, &pdictiter_spec)) ||
       !(st->TDictIterType = pcoll_new_internal_type(m, &tdictiter_spec)))
      goto done;

   // The canonical empty pdict. (pdict_wrap() returns this singleton for an
   // empty result, so it is built by hand.)
   empty = (PDictObject*)st->PDictType->tp_alloc(st->PDictType, 0);
   if (!empty) goto done;
   empty->els = fat_empty(ELSLEAFSIZE);
   empty->idx = amt_empty(IDXLEAFSIZE);
   empty->top = 0;
   empty->count = 0;
   empty->ndeleted = 0;
   empty->hashcode = -1;
   st->g_pdict_empty = empty;
   if (pcoll_type_setattr(st->PDictType, "empty", (PyObject*)empty) < 0)
      goto done;

   if (pcoll_module_add(m, "pdict", (PyObject*)st->PDictType) < 0 ||
       pcoll_module_add(m, "tdict", (PyObject*)st->TDictType) < 0 ||
       pcoll_module_add(m, "pdict_keys", (PyObject*)st->PDictKeysType) < 0 ||
       pcoll_module_add(m, "pdict_items", (PyObject*)st->PDictItemsType) < 0 ||
       pcoll_module_add(m, "pdict_values", (PyObject*)st->PDictValuesType) < 0 ||
       pcoll_module_add(m, "tdict_keys", (PyObject*)st->TDictKeysType) < 0 ||
       pcoll_module_add(m, "tdict_items", (PyObject*)st->TDictItemsType) < 0 ||
       pcoll_module_add(m, "tdict_values", (PyObject*)st->TDictValuesType) < 0)
      goto done;
   rc = 0;
done:
   Py_XDECREF(pcoll_abc);
   Py_XDECREF(coll_abc);
   Py_XDECREF(KeysView_t);
   Py_XDECREF(ItemsView_t);
   Py_XDECREF(ValuesView_t);
   Py_XDECREF(KeysBase);
   Py_XDECREF(ItemsBase);
   Py_XDECREF(ValuesBase);
   return rc;
}
