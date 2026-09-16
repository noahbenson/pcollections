///////////////////////////////////////////////////////////////////////////////
// _c/set.c.h
// The persistent (pset) and transient (tset) set types, implemented as thin
// CPython wrappers around a pair of tries: an AMT used as a hash table
// (hash(elem) -> index) and a FAT used as an insertion-ordered element table
// (index -> (elem, next_index)).
//
// The design follows dict.c.h (see its file comment) without the values:
//
//  - `els` (a FAT) is the insertion-ordered element table, with keys
//    0, 1, ..., top-1. Each leaf is a SetEntry: the element and the index of
//    the next entry whose element has the same hash (or SET_NO_NEXT).
//  - `idx` (an AMT) maps hash(elem) to the index of the first entry in that
//    hash's collision chain.
//
// Deletion tombstones the entry's slot, and compaction uses the same policy
// as dict.c.h.
//
// Every FAT node here has leafsize == sizeof(SetEntry); every AMT node has
// leafsize == sizeof(trieint_t).
//
// pset and tset implement add, discard, clear, transient/persistent, len,
// iteration, membership, repr and str natively, plus __hash__ (pset); the
// rest of the set API (set algebra, comparisons, pop, remove, ...) is
// inherited from their pcollections.abc bases (see pcoll_exec_set()). Both
// types can be subclassed, and operations that return a new set build an
// instance of type(self), or of the type being constructed.


//=============================================================================
// SetEntry: the FAT leaf type for `els` (dict.c.h's DictEntry without the
// value).

typedef struct {
   PyObject* key;
   trieint_t next;   // FAT index of the next entry sharing this element's
                      // hash, or SET_NO_NEXT if this is the last link.
} SetEntry;
#define SETELSLEAFSIZE ((uint8_t)sizeof(SetEntry))
// idx (the AMT hash table) stores raw trieint_t FAT-index leaves.
#define SETIDXLEAFSIZE ((uint8_t)sizeof(trieint_t))
#define SET_NO_NEXT (~(trieint_t)0)

// Compaction thresholds (the same as dict.c.h's).
#define SET_COMPACT_ABS_THRESHOLD ((Py_ssize_t)1024 * 1024)
#define SET_COMPACT_FRAC_NUM 3
#define SET_COMPACT_FRAC_DEN 10


//=============================================================================
// Leaf refcounting callbacks.

// els leaves (SetEntry) own a reference to their key; idx leaves are plain
// integers.
// A tombstone's key is NULL.
static void setentry_incref(void* v) {
   SetEntry* e = (SetEntry*)v;
   Py_XINCREF(e->key);
}
static void setentry_decref(void* v) {
   SetEntry* e = (SetEntry*)v;
   Py_XDECREF(e->key);
}

//=============================================================================
// Tombstones: an entry whose key is NULL (see dictentry_is_tombstone() in
// dict.c.h).
static int setentry_is_tombstone(const SetEntry* e) {
   return e->key == NULL;
}
static void set_make_tombstone(SetEntry* out) {
   out->key = NULL;
   out->next = SET_NO_NEXT;
}




//=============================================================================
// Hash-key conversion (as dict_hash_key() in dict.c.h).
static int set_hash_key(PyObject* key, trieint_t* out) {
   Py_hash_t h = PyObject_Hash(key);
   if (h == -1 && PyErr_Occurred()) return -1;
   *out = (trieint_t)(size_t)h;
   return 0;
}


//=============================================================================
// Collision-chain lookup shared by pset and tset; see dict_chain_find() in
// dict.c.h. *out_prev is set whether or not the element is found.
static int set_chain_find(Trie_t els, Trie_t idx, trieint_t hkey,
                          PyObject* key, trieint_t* out_index,
                          trieint_t* out_prev,
                          pcoll_tguard* g, PyObject* owner, int reading) {
   void* found;
   trieint_t ii;
   trieint_t prev = SET_NO_NEXT;
   uint64_t version = g ? g->version : 0;
   if (reading && tguard_read(g, owner, "a lookup") < 0) return -1;
   if (!amt_lookup(idx, hkey, &found)) {
      if (out_prev) *out_prev = SET_NO_NEXT;
      return 0;
   }
   ii = *(trieint_t*)found;
   while (1) {
      void* ep = NULL;
      SetEntry* e;
      PyObject* ekey;
      int eq;
      fat_lookup(els, ii, &ep);
      e = (SetEntry*)ep;
      ekey = e->key;
      if (ekey == key) {
         eq = 1;
      } else {
         Py_INCREF(ekey);
         eq = PyObject_RichCompareBool(key, ekey, Py_EQ);
         Py_DECREF(ekey);
         if (g && (g->version != version || (reading && tguard_busy(g)))) {
            if (eq >= 0)
               tguard_lookup_error(Py_TYPE(owner)->tp_name);
            return -1;
         }
         if (eq < 0) return -1;
      }
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

// Returns the index of the last entry in the chain for `hkey`, or
// SET_NO_NEXT if there is no such chain. Calls no user code.
static trieint_t set_chain_tail(Trie_t els, Trie_t idx, trieint_t hkey) {
   void* found;
   trieint_t ii;
   if (!amt_lookup(idx, hkey, &found)) return SET_NO_NEXT;
   ii = *(trieint_t*)found;
   while (1) {
      void* ep = NULL;
      fat_lookup(els, ii, &ep);
      if (((SetEntry*)ep)->next == SET_NO_NEXT) return ii;
      ii = ((SetEntry*)ep)->next;
   }
}


//=============================================================================
// pset

typedef struct PSetObject {
   PyObject_HEAD
   Trie_t idx;           // AMT: hash -> first FAT index. Persistent.
   Trie_t els;           // FAT: index -> SetEntry. Persistent.
   Py_ssize_t top;        // next fresh index to hand out.
   Py_ssize_t count;      // number of live entries (== len()).
   Py_ssize_t ndeleted;   // tombstones since the last compaction.
   Py_hash_t hashcode;    // cached hash, or -1 if not yet computed.
   PyObject* weaklist;
} PSetObject;

// The types and the empty pset live in the module state (core.h).

static int pset_traverse(PSetObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT((PyObject*)self->els);
   return 0;
}
static int pset_clear(PSetObject* self) {
   Trie_t els = self->els, idx = self->idx;
   self->els = NULL; self->idx = NULL;
   if (els) fatnode_decref(els, setentry_decref);
   if (idx) amtnode_decref(idx, noop_decref);
   return 0;
}
static void pset_dealloc(PSetObject* self) {
   // Heap-type instances own a reference to their type.
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   pset_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t pset_length(PSetObject* self) {
   return self->count;
}

// A new empty instance of `type`.
static PyObject* pset_make_empty(PyTypeObject* type) {
   PSetObject* self = (PSetObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   self->els = fat_empty(SETELSLEAFSIZE);
   self->idx = amt_empty(SETIDXLEAFSIZE);
   self->hashcode = -1;
   return (PyObject*)self;
}
// The empty instance of `type` (see pcoll_type_empty in core.h).
static PyObject* pset_type_empty(PyTypeObject* type) {
   if (type == ST(PSetType)) {
      Py_INCREF(ST(g_pset_empty));
      return (PyObject*)ST(g_pset_empty);
   }
   return pcoll_type_empty(type, pset_make_empty);
}
// Wraps `els`/`idx` (whose references are consumed) in a new instance of
// `type`, or returns the empty instance for an empty set.
static PyObject* pset_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                  Py_ssize_t top, Py_ssize_t count,
                                  Py_ssize_t ndeleted) {
   PSetObject* self;
   if (count == 0) {
      fatnode_decref(els, setentry_decref);
      amtnode_decref(idx, noop_decref);
      return pset_type_empty(type);
   }
   self = (PSetObject*)type->tp_alloc(type, 0);
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
   return (PyObject*)self;
}

// Builds new transient tries holding the live entries of `els`, renumbered
// 0..count-1 in insertion order; see dict_rebuild_compacted() in dict.c.h.
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
      // The elements are distinct, so each one starts a new chain or
      // extends an existing one; no comparisons are needed.
      prev = set_chain_tail(new_els, new_idx, hkey);
      if (prev == SET_NO_NEXT) {
         trieint_t idxval = newtop;
         new_idx = tamt_setitem(new_idx, hkey, &idxval, noop_incref, noop_decref);
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

static PyObject* pset_new_dispatch(PyTypeObject* type, PyObject* arg);

static PyObject* pset_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   (void)type;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "pset() takes no keyword arguments");
      return NULL;
   }
   if (n == 0) {
      return pset_type_empty(type);
   } else if (n == 1) {
      return pset_new_dispatch(type, PyTuple_GET_ITEM(args, 0));
   } else {
      PyErr_Format(PyExc_TypeError,
                    "pset expects at most 1 argument, got %zd", n);
      return NULL;
   }
}

// The frozenset hash (Objects/setobject.c), so that a pset hashes like an
// equal frozenset. If this interpreter's frozenset hash differs (checked at
// import by pcollections.util), a frozenset is built instead.
static Py_uhash_t pset_shuffle_bits(Py_uhash_t h) {
   return ((h ^ (Py_uhash_t)89869747UL) ^ (h << 16)) * (Py_uhash_t)3644798167UL;
}
static Py_hash_t pset_hash(PSetObject* self) {
   Py_hash_t result;
   TriePath iter;
   int ok;
   result = PCOLL_HASH_LOAD(self->hashcode);
   if (result != -1) return result;
   if (ST(frozenset_hash_ok)) {
      Py_uhash_t h = 0;
      for (ok = fat_firstpath(self->els, &iter); ok; ok = fat_nextpath(&iter)) {
         SetEntry* e = (SetEntry*)triepath_val(&iter);
         Py_hash_t eh;
         if (setentry_is_tombstone(e)) continue;
         eh = PyObject_Hash(e->key);
         if (eh == -1 && PyErr_Occurred()) return -1;
         h ^= pset_shuffle_bits((Py_uhash_t)eh);
      }
      h ^= ((Py_uhash_t)self->count + 1) * (Py_uhash_t)1927868237UL;
      h ^= (h >> 11) ^ (h >> 25);
      h = h * 69069U + (Py_uhash_t)907133923UL;
      if (h == (Py_uhash_t)-1) h = (Py_uhash_t)590923713UL;
      result = (Py_hash_t)h;
   } else {
      PyObject* fs = PyFrozenSet_New((PyObject*)self);
      if (!fs) return -1;
      result = PyObject_Hash(fs);
      Py_DECREF(fs);
      if (result == -1) return -1;
   }
   PCOLL_HASH_STORE(self->hashcode, result);
   return result;
}

static int pset_contains(PSetObject* self, PyObject* el) {
   trieint_t hkey;
   if (set_hash_key(el, &hkey) < 0) return -1;
   return set_chain_find(self->els, self->idx, hkey, el, NULL, NULL, NULL,
                         NULL, 0);
}

static PyObject* pset_add(PSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_top = self->top, new_count = self->count;
   Py_ssize_t new_ndeleted = self->ndeleted;
   if (set_hash_key(obj, &hkey) < 0) return NULL;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev,
                          NULL, NULL, 0);
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
         // fat_anditem() does not consume its input, so the intermediate
         // tree is released here.
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
   return pset_wrap_astype(Py_TYPE(self), new_els, new_idx, new_top,
                           new_count, new_ndeleted);
}

static PyObject* pset_discard(PSetObject* self, PyObject* obj) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_count, new_ndeleted;
   if (set_hash_key(obj, &hkey) < 0) return NULL;
   found = set_chain_find(self->els, self->idx, hkey, obj, &found_index, &prev,
                          NULL, NULL, 0);
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
         // Relink the previous entry, then tombstone the deleted slot.
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
      return pset_wrap_astype(Py_TYPE(self), c_els, c_idx, c_top, new_count, 0);
   }
   return pset_wrap_astype(Py_TYPE(self), new_els, new_idx, self->top,
                           new_count, new_ndeleted);
}

static PyObject* pset_clear_method(PSetObject* self, PyObject* Py_UNUSED(ignored)) {
   return pset_type_empty(Py_TYPE(self));
}

static PyObject* pset_transient(PSetObject* self, PyObject* Py_UNUSED(ignored));
static PyObject* pset_iter(PSetObject* self);

// Matches abc/_set.py's PersistentSet.__repr__/__str__: both use the
// "{|...|}" delimiter, __str__ truncated at 60 chars, __repr__ not.
static PyObject* pset_repr(PSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* pset_str(PSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}

static PyMethodDef pset_methods[] = {
   {"add", (PyCFunction)pset_add, METH_O,
    "add($self, obj, /)\n--\n\nReturns a copy of the pset that includes the given object."},
   {"discard", (PyCFunction)pset_discard, METH_O,
    "discard($self, obj, /)\n--\n\nReturns a copy of the pset that does not include the given object."},
   {"clear", (PyCFunction)pset_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nReturns the empty pset."},
   {"transient", (PyCFunction)pset_transient, METH_NOARGS,
    "transient($self, /)\n--\n\nEfficiently copies the pset into a tset and returns the tset."},
   PCOLL_CLASS_GETITEM_METHODDEF
   {NULL, NULL, 0, NULL}
};

static PyType_Slot pset_slots[] = {
   {Py_tp_dealloc, (void*)pset_dealloc},
   {Py_tp_repr, (void*)pset_repr},
   {Py_tp_str, (void*)pset_str},
   {Py_sq_length, (void*)pset_length},
   {Py_sq_contains, (void*)pset_contains},
   {Py_tp_hash, (void*)pset_hash},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A persistent (immutable) set.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    pset() -> an empty pset\n"
      "    pset(iterable) -> a pset of the elements of iterable\n"
      "\n"
      "Methods that would change a set instead return a new pset (add, addall,\n"
      "discard, discardall, remove, removeall, drop, pop, clear). A pset remembers\n"
      "the order in which its elements were added, and has the same hash as an\n"
      "equal frozenset. transient() returns a tset copy in constant time.")},
   {Py_tp_traverse, (void*)pset_traverse},
   {Py_tp_clear, (void*)pset_clear},
   {Py_tp_iter, (void*)pset_iter},
   {Py_tp_methods, (void*)pset_methods},
   {Py_tp_new, (void*)pset_new},
   {0, NULL}
};
static PyType_Spec pset_spec = {
   .name = "pcollections.pset",
   .basicsize = sizeof(PSetObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
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
   PyObject* orig;          // a persistent set with the same contents
                            // (owned), or NULL; dropped on any change.
   pcoll_tguard guard;      // see core.h.
   PyObject* weaklist;
} TSetObject;

static int tset_traverse(TSetObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->orig);
   Py_VISIT((PyObject*)self->els);
   return 0;
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
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   tset_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t tset_length_impl(TSetObject* self) {
   return self->count;
}
PCOLL_LOCKED0(Py_ssize_t, tset_length, tset_length_impl, TSetObject*)
// Wraps `els`/`idx` and `orig` (whose references are consumed) in a new
// instance of `type`.
static PyObject* tset_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                  Py_ssize_t top, Py_ssize_t count,
                                  Py_ssize_t ndeleted, PyObject* orig) {
   TSetObject* self = (TSetObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(els, setentry_decref);
      amtnode_decref(idx, noop_decref);
      Py_XDECREF(orig);
      return NULL;
   }
   self->els = els; self->idx = idx;
   self->top = top; self->count = count; self->ndeleted = ndeleted;
   self->orig = orig;
   return (PyObject*)self;
}
static void tset_invalidate_orig(TSetObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

static PyObject* tset_empty_astype(PyTypeObject* type) {
   return tset_wrap_astype(type, fat_empty(SETELSLEAFSIZE),
                           amt_empty(SETIDXLEAFSIZE), 0, 0, 0, NULL);
}

// Compacts the tset in place if needed (see tdict_maybe_compact()).
static int tset_maybe_compact(TSetObject* self) {
   Trie_t c_els, c_idx; Py_ssize_t c_top;
   if (!set_should_compact(self->count, self->ndeleted)) return 0;
   Trie_t old_els = self->els, old_idx = self->idx;
   if (set_rebuild_compacted(old_els, old_idx, &c_els, &c_idx, &c_top) < 0)
      return -1;
   self->els = c_els; self->idx = c_idx; self->top = c_top; self->ndeleted = 0;
   fatnode_decref(old_els, setentry_decref);
   amtnode_decref(old_idx, noop_decref);
   return 0;
}

// The bodies of tset.add() and tset.discard(), run with the guard held.
static int tset_add_guarded(TSetObject* self, trieint_t hkey, PyObject* obj) {
   trieint_t found_index, prev, newindex;
   SetEntry entry;
   int found = set_chain_find(self->els, self->idx, hkey, obj, &found_index,
                              &prev, &self->guard, (PyObject*)self, 0);
   if (found) return found < 0 ? -1 : 0;
   tguard_keys_changed(&self->guard);
   newindex = (trieint_t)self->top;
   entry.key = obj; entry.next = SET_NO_NEXT;
   tfat_setitem_at(&self->els, newindex, &entry,
                   setentry_incref, setentry_decref);
   if (prev == SET_NO_NEXT) {
      self->idx = tamt_setitem(self->idx, hkey, &newindex, noop_incref, noop_decref);
   } else {
      void* ep; SetEntry patched;
      fat_lookup(self->els, prev, &ep);
      memcpy(&patched, ep, sizeof(SetEntry));
      patched.next = newindex;
      tfat_setitem_at(&self->els, prev, &patched,
                      setentry_incref, setentry_decref);
   }
   self->top += 1;
   self->count += 1;
   tset_invalidate_orig(self);
   return tset_maybe_compact(self);
}
static int tset_discard_guarded(TSetObject* self, trieint_t hkey, PyObject* obj) {
   trieint_t found_index, prev;
   void* ep; SetEntry entry; SetEntry tombstone;
   int found = set_chain_find(self->els, self->idx, hkey, obj, &found_index,
                              &prev, &self->guard, (PyObject*)self, 0);
   if (found <= 0) return found;
   tguard_keys_changed(&self->guard);
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
      tfat_setitem_at(&self->els, prev, &pentry,
                      setentry_incref, setentry_decref);
   }
   self->count -= 1;
   self->ndeleted += 1;
   // Tombstone the deleted slot.
   tfat_setitem_at(&self->els, found_index, &tombstone,
                   setentry_incref, setentry_decref);
   tset_invalidate_orig(self);
   if (tset_maybe_compact(self) < 0) return -1;
   return 1;
}
// Adds (add = 1) or discards (add = 0) `obj`. Returns -1 on error.
static int tset_update_one(TSetObject* self, PyObject* obj, int add) {
   trieint_t hkey;
   int rc;
   if (set_hash_key(obj, &hkey) < 0) return -1;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return -1;
   rc = add ? tset_add_guarded(self, hkey, obj)
            : tset_discard_guarded(self, hkey, obj);
   tguard_exit(&self->guard);
   return rc;
}
PCOLL_LOCKED2(int, tset_update_one_locked, tset_update_one,
              TSetObject*, PyObject*, int)
static int tset_add_element(TSetObject* self, PyObject* obj) {
   return tset_update_one_locked(self, obj, 1);
}

static PyObject* tset_add(TSetObject* self, PyObject* obj) {
   if (tset_update_one_locked(self, obj, 1) < 0) return NULL;
   Py_RETURN_NONE;
}

static PyObject* tset_discard(TSetObject* self, PyObject* obj) {
   if (tset_update_one_locked(self, obj, 0) < 0) return NULL;
   Py_RETURN_NONE;
}

static int tset_contains_impl(TSetObject* self, PyObject* el) {
   trieint_t hkey;
   if (set_hash_key(el, &hkey) < 0) return -1;
   return set_chain_find(self->els, self->idx, hkey, el, NULL, NULL,
                         &self->guard, (PyObject*)self, 1);
}
PCOLL_LOCKED1(int, tset_contains, tset_contains_impl, TSetObject*, PyObject*)

// Builds a new `type` instance holding the elements of iterable `arg`.
static PyObject* tset_build_from_arg(PyTypeObject* type, PyObject* arg) {
   PyObject* obj = tset_empty_astype(type);
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

// Freezes the transient's tries and returns new references to them, for
// sharing with another collection. Returns -1 (with RuntimeError set) if a
// modification is in progress. The caller holds the transient's lock.
static int tset_share(TSetObject* self, Trie_t* els, Trie_t* idx) {
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return -1;
   fat_freeze(self->els);
   amt_freeze(self->idx);
   trienode_incref(self->els);
   trienode_incref(self->idx);
   *els = self->els;
   *idx = self->idx;
   return 0;
}

static PyObject* tset_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* arg;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "tset() takes no keyword arguments");
      return NULL;
   }
   if (n == 0) return tset_empty_astype(type);
   if (n != 1) {
      PyErr_Format(PyExc_TypeError,
                    "tset expects at most 1 argument, got %zd", n);
      return NULL;
   }
   arg = PyTuple_GET_ITEM(args, 0);
   if (PyObject_TypeCheck(arg, ST(TSetType))) {
      // Share a frozen snapshot of the other tset. Its cached original is a
      // pset, so it is kept only when making a plain tset.
      TSetObject* t = (TSetObject*)arg;
      Trie_t els, idx;
      PyObject* orig = NULL;
      Py_ssize_t top = 0, count = 0, ndeleted = 0;
      int rc;
      PCOLL_BEGIN_LOCK(arg);
      rc = tset_share(t, &els, &idx);
      if (rc == 0) {
         orig = (type == ST(TSetType)) ? t->orig : NULL;
         Py_XINCREF(orig);
         top = t->top; count = t->count; ndeleted = t->ndeleted;
      }
      PCOLL_END_LOCK();
      if (rc < 0) return NULL;
      return tset_wrap_astype(type, els, idx, top, count, ndeleted, orig);
   }
   if (PyObject_TypeCheck(arg, ST(PSetType))) {
      PSetObject* p = (PSetObject*)arg;
      PyObject* orig = (type == ST(TSetType) && Py_TYPE(arg) == ST(PSetType))
         ? arg : NULL;
      trienode_incref(p->els);
      trienode_incref(p->idx);
      Py_XINCREF(orig);
      return tset_wrap_astype(type, p->els, p->idx, p->top, p->count,
                              p->ndeleted, orig);
   }
   return tset_build_from_arg(type, arg);
}

// Returns a new instance of `ptype` (a persistent partner type) holding the
// transient's contents; the cached original is used when it has that type.
static PyObject* tset_persistent_as(TSetObject* self, PyTypeObject* ptype) {
   Trie_t els, idx;
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return NULL;
   if (self->orig && Py_TYPE(self->orig) == ptype) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   if (self->count == 0) return pset_type_empty(ptype);
   if (tset_share(self, &els, &idx) < 0) return NULL;
   return pset_wrap_astype(ptype, els, idx, self->top, self->count,
                           self->ndeleted);
}
static PyObject* tset_persistent_impl(TSetObject* self) {
   PyTypeObject* ptype;
   PyObject* result;
   if (Py_TYPE(self) == ST(TSetType)) {
      ptype = ST(PSetType);
      Py_INCREF(ptype);
   } else {
      ptype = pcoll_partner_type((PyObject*)self, "__persistent_type__",
                                 ST(PSetType));
      if (!ptype) return NULL;
   }
   result = tset_persistent_as(self, ptype);
   Py_DECREF(ptype);
   return result;
}
PCOLL_LOCKED0(PyObject*, tset_persistent_locked, tset_persistent_impl,
              TSetObject*)
static PyObject* tset_persistent(TSetObject* self, PyObject* Py_UNUSED(ignored)) {
   return tset_persistent_locked(self);
}

static PyObject* pset_transient(PSetObject* self, PyObject* Py_UNUSED(ignored)) {
   PyTypeObject* ttype;
   PyObject* result;
   if (Py_TYPE(self) == ST(PSetType)) {
      ttype = ST(TSetType);
      Py_INCREF(ttype);
   } else {
      ttype = pcoll_partner_type((PyObject*)self, "__transient_type__",
                                 ST(TSetType));
      if (!ttype) return NULL;
   }
   trienode_incref(self->els);
   trienode_incref(self->idx);
   // The transient keeps a reference to self as its cached original.
   Py_INCREF(self);
   result = tset_wrap_astype(ttype, self->els, self->idx, self->top,
                             self->count, self->ndeleted, (PyObject*)self);
   Py_DECREF(ttype);
   return result;
}

static PyObject* tset_clear_impl(TSetObject* self) {
   Trie_t els = self->els, idx = self->idx;
   PyObject* orig = self->orig;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return NULL;
   tguard_keys_changed(&self->guard);
   self->els = fat_empty(SETELSLEAFSIZE);
   self->idx = amt_empty(SETIDXLEAFSIZE);
   self->top = 0; self->count = 0; self->ndeleted = 0;
   self->orig = NULL;
   tguard_exit(&self->guard);
   fatnode_decref(els, setentry_decref);
   amtnode_decref(idx, noop_decref);
   Py_XDECREF(orig);
   Py_RETURN_NONE;
}
PCOLL_LOCKED0(PyObject*, tset_clear_locked, tset_clear_impl, TSetObject*)
static PyObject* tset_clear_method(TSetObject* self, PyObject* Py_UNUSED(ignored)) {
   return tset_clear_locked(self);
}

static PyObject* tset_iter(TSetObject* self);

// Matches abc/_set.py's TransientSet.__repr__/__str__: both use the
// "{<...>}" delimiter, __str__ truncated at 60 chars, __repr__ not.
static PyObject* tset_repr(TSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 0, 0, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* tset_str(TSetObject* self) {
   PyObject* s = call_seqstr((PyObject*)self, 60, 1, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{<%U>}", s);
   Py_DECREF(s);
   return result;
}

// tset.empty() is a classmethod that returns a new empty instance of cls.
static PyObject* tset_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   return tset_empty_astype((PyTypeObject*)cls);
}

static PyMethodDef tset_methods[] = {
   {"empty", (PyCFunction)tset_empty_classmethod, METH_NOARGS | METH_CLASS,
    "empty($type, /)\n--\n\nReturns a new, empty tset."},
   {"add", (PyCFunction)tset_add, METH_O,
    "add($self, obj, /)\n--\n\nAdds the given object to the tset."},
   {"discard", (PyCFunction)tset_discard, METH_O,
    "discard($self, obj, /)\n--\n\nDiscards the given object from the tset."},
   {"clear", (PyCFunction)tset_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nClears all elements from the tset."},
   {"persistent", (PyCFunction)tset_persistent, METH_NOARGS,
    "persistent($self, /)\n--\n\nEfficiently copies the tset into a pset and returns the pset."},
   PCOLL_CLASS_GETITEM_METHODDEF
   {NULL, NULL, 0, NULL}
};

static PyType_Slot tset_slots[] = {
   {Py_tp_dealloc, (void*)tset_dealloc},
   {Py_tp_repr, (void*)tset_repr},
   {Py_tp_str, (void*)tset_str},
   {Py_sq_length, (void*)tset_length},
   {Py_sq_contains, (void*)tset_contains},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient (mutable) set.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    tset() -> an empty tset\n"
      "    tset(iterable) -> a tset of the elements of iterable\n"
      "\n"
      "A tset has the interface of set, plus addall, discardall, and removeall.\n"
      "persistent() returns a pset copy in constant time. A tset is meant to be\n"
      "used by one thread at a time; a modification that overlaps another\n"
      "modification raises RuntimeError.")},
   {Py_tp_traverse, (void*)tset_traverse},
   {Py_tp_clear, (void*)tset_clear},
   {Py_tp_iter, (void*)tset_iter},
   {Py_tp_methods, (void*)tset_methods},
   {Py_tp_new, (void*)tset_new},
   {0, NULL}
};
static PyType_Spec tset_spec = {
   .name = "pcollections.tset",
   .basicsize = sizeof(TSetObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tset_slots,
};

// pset(arg), for `type` pset or a subtype. An object whose type is `type`
// is returned as-is; a tset or pset shares its tries; anything else is
// iterated.
static PyObject* pset_new_dispatch(PyTypeObject* type, PyObject* arg) {
   PyObject* t;
   PyObject* result;
   if (Py_TYPE(arg) == type) {
      Py_INCREF(arg);
      return arg;
   }
   if (PyObject_TypeCheck(arg, ST(TSetType))) {
      TSetObject* ts = (TSetObject*)arg;
      Trie_t els, idx;
      Py_ssize_t top = 0, count = 0, ndeleted = 0;
      int rc;
      PCOLL_BEGIN_LOCK(arg);
      rc = tset_share(ts, &els, &idx);
      if (rc == 0) {
         top = ts->top; count = ts->count; ndeleted = ts->ndeleted;
      }
      PCOLL_END_LOCK();
      if (rc < 0) return NULL;
      return pset_wrap_astype(type, els, idx, top, count, ndeleted);
   }
   if (PyObject_TypeCheck(arg, ST(PSetType))) {
      PSetObject* p = (PSetObject*)arg;
      trienode_incref(p->els);
      trienode_incref(p->idx);
      return pset_wrap_astype(type, p->els, p->idx, p->top, p->count,
                              p->ndeleted);
   }
   t = tset_build_from_arg(ST(TSetType), arg);
   if (!t) return NULL;
   result = tset_persistent_as((TSetObject*)t, type);
   Py_DECREF(t);
   return result;
}


//=============================================================================
// Iterators. As in dict.c.h, they walk `els` directly, skipping tombstones.

typedef struct {
   PyObject_HEAD
   PyObject* owner;    // the pset/tset being walked; owned.
   TriePath path;
   int state;          // 0 = not yet started, 1 = active, 2 = exhausted,
                       // 3 = failed (the transient changed).
   bool transient;     // whether owner is a tset.
   // For a transient owner: see dict.c.h's DictIterObject.
   Py_ssize_t count;
   uint64_t version;
   uint64_t keyversion;
   trieint_t nextkey;
} SetIterObject;

static void setiter_dealloc(SetIterObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   Py_XDECREF(self->owner);
   PyObject_GC_Del(self);
   Py_DECREF(tp);
}
static int setiter_traverse(SetIterObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->owner);
   return 0;
}
static PyObject* setiter_yield(SetIterObject* self, int ok) {
   SetEntry* e;
   while (ok && setentry_is_tombstone((SetEntry*)triepath_val(&self->path)))
      ok = fat_nextpath(&self->path);
   if (!ok) { self->state = 2; return NULL; }
   e = (SetEntry*)triepath_val(&self->path);
   self->nextkey = triepath_key(&self->path) + 1;
   Py_INCREF(e->key);
   return e->key;
}
// See dictiter_next_impl() in dict.c.h.
static PyObject* setiter_next_impl(SetIterObject* self) {
   if (self->state == 2) return NULL;
   if (!self->transient) {
      PSetObject* p = (PSetObject*)self->owner;
      if (self->state == 0) {
         self->state = 1;
         return setiter_yield(self, fat_firstpath(p->els, &self->path));
      }
      return setiter_yield(self, fat_nextpath(&self->path));
   } else {
      TSetObject* t = (TSetObject*)self->owner;
      const char* name = Py_TYPE(t)->tp_name;
      int ok;
      if (t->els == NULL) { self->state = 2; return NULL; }
      if (self->state != 3 && t->count != self->count) {
         self->state = 3;
         PyErr_Format(PyExc_RuntimeError,
                      "%.200s changed size during iteration", name);
         return NULL;
      }
      if (self->state == 3 || t->guard.keyversion != self->keyversion) {
         self->state = 3;
         PyErr_Format(PyExc_RuntimeError,
                      "%.200s changed during iteration", name);
         return NULL;
      }
      if (tguard_read(&t->guard, (PyObject*)t, "iteration") < 0) return NULL;
      if (self->state == 0) {
         self->state = 1;
         ok = fat_firstpath(t->els, &self->path);
      } else if (t->guard.version != self->version) {
         ok = fat_seekpath(t->els, self->nextkey, &self->path);
      } else {
         ok = fat_nextpath(&self->path);
      }
      self->version = t->guard.version;
      return setiter_yield(self, ok);
   }
}
static PyObject* setiter_next(SetIterObject* self) {
   PyObject* r;
   PCOLL_BEGIN_LOCK2((PyObject*)self, self->owner);
   r = setiter_next_impl(self);
   PCOLL_END_LOCK2();
   return r;
}
static PyObject* setiter_self(PyObject* self) { Py_INCREF(self); return self; }

static PyObject* make_setiter(PyTypeObject* itertype, PyObject* owner_set) {
   SetIterObject* it = PyObject_GC_New(SetIterObject, itertype);
   if (!it) return NULL;
   Py_INCREF(owner_set);
   it->owner = owner_set;
   it->state = 0;
   it->transient = !PyObject_TypeCheck(owner_set, ST(PSetType));
   it->count = 0;
   it->version = 0;
   it->keyversion = 0;
   it->nextkey = 0;
   if (it->transient) {
      TSetObject* t = (TSetObject*)owner_set;
      PCOLL_BEGIN_LOCK(owner_set);
      it->count = t->count;
      it->version = t->guard.version;
      it->keyversion = t->guard.keyversion;
      PCOLL_END_LOCK();
   }
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

static PyType_Slot setiter_slots[] = {
   {Py_tp_dealloc, (void*)setiter_dealloc},
   {Py_tp_traverse, (void*)setiter_traverse},
   {Py_tp_iter, (void*)setiter_self},
   {Py_tp_iternext, (void*)setiter_next},
   {0, NULL}
};
static PyType_Spec psetiter_spec = {
   .name = "pcollections._c._core.pset_iterator",
   .basicsize = sizeof(SetIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = setiter_slots,
};
static PyType_Spec tsetiter_spec = {
   .name = "pcollections._c._core.tset_iterator",
   .basicsize = sizeof(SetIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = setiter_slots,
};

static PyObject* pset_iter(PSetObject* self) {
   return make_setiter(ST(PSetIterType), (PyObject*)self);
}
static PyObject* tset_iter(TSetObject* self) {
   return make_setiter(ST(TSetIterType), (PyObject*)self);
}


//=============================================================================
// Module execution.

// Creates pset, tset, their iterators, and the empty pset, and adds the
// public types to module `m`.
static int pcoll_exec_set(PyObject* m, pcoll_state* st) {
   PyObject* abc = NULL;
   PSetObject* empty;
   int rc = -1;

   abc = PyImport_ImportModule("pcollections.abc");
   if (!abc) goto done;
   st->PSetType = build_abc_subtype(m, &pset_spec, abc, "_PersistentSetBase", 1);
   if (!st->PSetType) goto done;
   st->TSetType = build_abc_subtype(m, &tset_spec, abc, "_TransientSetBase", 1);
   if (!st->TSetType) goto done;
   if (register_virtual_subclass(abc, "PersistentSet", st->PSetType) < 0 ||
       register_virtual_subclass(abc, "TransientSet", st->TSetType) < 0)
      goto done;
   // tset is unhashable, like its base.
   st->TSetType->tp_hash = st->TSetType->tp_base->tp_hash;
   pcoll_set_weaklistoffset(st->PSetType, offsetof(PSetObject, weaklist));
   pcoll_set_weaklistoffset(st->TSetType, offsetof(TSetObject, weaklist));
   if (pcoll_type_setattr(st->PSetType, "__transient_type__",
                          (PyObject*)st->TSetType) < 0 ||
       pcoll_type_setattr(st->TSetType, "__persistent_type__",
                          (PyObject*)st->PSetType) < 0)
      goto done;
   {
      PyObject* util = PyImport_ImportModule("pcollections.util._core");
      PyObject* ok;
      if (!util) goto done;
      ok = PyObject_GetAttrString(util, "FROZENSET_HASH_MATCHES");
      Py_DECREF(util);
      if (!ok) goto done;
      st->frozenset_hash_ok = PyObject_IsTrue(ok);
      Py_DECREF(ok);
      if (st->frozenset_hash_ok < 0) goto done;
   }
   if (!(st->PSetIterType = pcoll_new_internal_type(m, &psetiter_spec)) ||
       !(st->TSetIterType = pcoll_new_internal_type(m, &tsetiter_spec)))
      goto done;

   empty = (PSetObject*)st->PSetType->tp_alloc(st->PSetType, 0);
   if (!empty) goto done;
   empty->els = fat_empty(SETELSLEAFSIZE);
   empty->idx = amt_empty(SETIDXLEAFSIZE);
   empty->top = 0;
   empty->count = 0;
   empty->ndeleted = 0;
   empty->hashcode = -1;
   st->g_pset_empty = empty;
   if (pcoll_type_setattr(st->PSetType, "empty", (PyObject*)empty) < 0)
      goto done;

   if (pcoll_module_add(m, "pset", (PyObject*)st->PSetType) < 0 ||
       pcoll_module_add(m, "tset", (PyObject*)st->TSetType) < 0)
      goto done;
   rc = 0;
done:
   Py_XDECREF(abc);
   return rc;
}
