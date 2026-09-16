///////////////////////////////////////////////////////////////////////////////
// _c/dict.c.h
// The persistent (pdict) and transient (tdict) dict types, implemented as
// thin CPython wrappers around a pair of tries: an AMT used as a hash table
// (hash(key) -> index) and a FAT used as an insertion-ordered value table
// (index -> (key, value, next_index)).
//
//  - `els` (a FAT) is the insertion-ordered entry table. Its keys are the
//    indices 0, 1, ..., top-1, assigned in insertion order, so iterating it
//    in key order visits the entries in insertion order. Each leaf is a
//    DictEntry: the key, the value, and the index of the next entry whose
//    key has the same hash (or DICT_NO_NEXT), forming a collision chain.
//  - `idx` (an AMT) maps hash(key) to the index of the first entry in that
//    hash's chain. A lookup finds the chain head in `idx`, then walks the
//    chain in `els` comparing keys with ==. Its leaves are plain integers.
//
// Deletion and compaction: a deleted entry is unlinked from its chain and
// its slot in `els` is overwritten with a tombstone (see
// dictentry_is_tombstone()). `top` never decreases, so indices are not
// reused; `ndeleted` counts the tombstones. When a modification leaves
// `ndeleted` above 1024**2 or above 30% of `count`, `els` and `idx` are
// rebuilt with the live entries renumbered 0..count-1 (see
// dict_rebuild_compacted()): as new tries for a pdict, in place for a
// tdict. The Python backend uses the same scheme (pcollections/_compact.py).
//
// pdict and tdict implement the core mapping methods natively, plus
// __hash__ (pdict) and keys()/items()/values(); the rest of the mapping API
// is inherited from their pcollections.abc bases (see pcoll_exec_dict()).
// Both types can be subclassed (ldict and tldict in lazy.c.h are
// subclasses). Operations that return a new collection build an instance of
// type(self), or of the type being constructed, so subclasses are preserved.


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
// Marks the end of a collision chain; no entry has this index.
#define DICT_NO_NEXT (~(trieint_t)0)

// Compaction thresholds (see the file comment).
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
// A tombstone is a DictEntry whose key and value are NULL. Deleting an entry
// first unlinks it from its collision chain (patching `idx` or the previous
// entry's `next`), then overwrites its slot with a tombstone, which stays in
// `els` until the next compaction. Code that walks `els` directly
// (dict_rebuild_compacted(), pdict_hash(), the iterators) skips tombstones;
// dict_chain_find() never reaches one, since an entry is unlinked before it
// is tombstoned.
static int dictentry_is_tombstone(const DictEntry* e) {
   return e->key == NULL;
}
static void dict_make_tombstone(DictEntry* out) {
   out->key = NULL;
   out->val = NULL;
   out->next = DICT_NO_NEXT;
}




//=============================================================================
// Hash-key conversion: Python's hash() returns a signed Py_hash_t and AMT
// keys are unsigned. `idx` is a hash table, so key order does not matter and
// a bit-preserving conversion suffices.
static int dict_hash_key(PyObject* key, trieint_t* out) {
   Py_hash_t h = PyObject_Hash(key);
   if (h == -1 && PyErr_Occurred()) return -1;
   *out = (trieint_t)(size_t)h;
   return 0;
}


//=============================================================================
// Collision-chain primitives shared by pdict and tdict. They only read
// (els, idx); callers apply the persistent (fat_anditem/amt_anditem) or
// transient (tfat_setitem/tamt_setitem) updates.

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
                           pcoll_tguard* g, PyObject* owner, int reading) {
   void* found;
   trieint_t ii;
   trieint_t prev = DICT_NO_NEXT;
   uint64_t version = g ? g->version : 0;
   if (reading && tguard_read(g, owner, "a lookup") < 0) return -1;
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
         if (g && (g->version != version || (reading && tguard_busy(g)))) {
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
   Py_ssize_t ndeleted;   // tombstones since the last compaction.
   Py_hash_t hashcode;    // cached hash, or -1 if not yet computed.
   PyObject* weaklist;
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
   // tp_alloc took a reference to the (heap) type; free the instance with
   // tp_free, which a subclass may override, then release the type.
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   pdict_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t pdict_length(PDictObject* self) {
   return self->count;
}

// Wraps `els` and `idx` (whose references are consumed) in a new instance of
// `type`, which must be PDictType or a subtype of it. An empty result is
// type's empty instance instead (see pdict_type_empty()).
static PyObject* pdict_type_empty(PyTypeObject* type);
static PyObject* pdict_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                    Py_ssize_t top, Py_ssize_t count,
                                    Py_ssize_t ndeleted) {
   PDictObject* self;
   if (count == 0) {
      fatnode_decref(els, dictentry_decref);
      amtnode_decref(idx, noop_decref);
      return pdict_type_empty(type);
   }
   // tp_alloc zeroes any fields a Python subclass adds (__dict__,
   // __weakref__), takes a reference to the type (released in
   // pdict_dealloc()), and starts GC tracking.
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

// A new empty instance of `type`.
static PyObject* pdict_make_empty(PyTypeObject* type) {
   PDictObject* self = (PDictObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   self->els = fat_empty(ELSLEAFSIZE);
   self->idx = amt_empty(IDXLEAFSIZE);
   self->hashcode = -1;
   return (PyObject*)self;
}
// The empty instance of `type` (see pcoll_type_empty in core.h).
static PyObject* pdict_type_empty(PyTypeObject* type) {
   if (type == ST(PDictType)) {
      Py_INCREF(ST(g_pdict_empty));
      return (PyObject*)ST(g_pdict_empty);
   }
   return pcoll_type_empty(type, pdict_make_empty);
}

// Builds new transient tries holding the live entries of (els, idx),
// renumbered 0..count-1 in insertion order. The inputs are not consumed.
// Returns 0 and sets *out_els, *out_idx, and *out_top (the new count), or
// returns -1 with an exception set if hashing a key fails.
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
      if (dictentry_is_tombstone(e)) continue;
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

// Defined after tdict, which it uses for the general case.
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
                           &val, NULL, NULL, NULL, 0);
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
                           NULL, NULL, NULL, 0);
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
                          NULL, NULL, 0);
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
                           &old_val, &prev, NULL, NULL, 0);
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
         // fat_anditem() does not consume its input, so the intermediate
         // tree is released here.
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
   // The result has type(self), so subclasses are preserved.
   return pdict_wrap_astype(Py_TYPE(self), new_els, new_idx, new_top,
                              new_count, new_ndeleted);
}

static PyObject* pdict_drop_key(PDictObject* self, PyObject* key, int error) {
   trieint_t hkey;
   trieint_t found_index, prev;
   int found;
   Trie_t new_els, new_idx;
   Py_ssize_t new_count, new_ndeleted;
   if (dict_hash_key(key, &hkey) < 0) return NULL;
   found = dict_chain_find(self->els, self->idx, hkey, key, &found_index,
                           NULL, &prev, NULL, NULL, 0);
   if (found < 0) return NULL;
   if (!found) {
      if (error) {
         PyErr_SetObject(PyExc_KeyError, key);
         return NULL;
      }
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
         // Relink the previous entry, then tombstone the deleted slot; the
         // intermediate tree is released as in pdict_set().
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

static PyObject* pdict_drop(PDictObject* self, PyObject* args, PyObject* kwds) {
   static char* kwlist[] = {"key", "error", NULL};
   PyObject* key;
   int error = 0;
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p:drop", kwlist,
                                    &key, &error))
      return NULL;
   return pdict_drop_key(self, key, error);
}

static PyObject* pdict_clear_method(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return pdict_type_empty(Py_TYPE(self));
}

static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored));

static PyMethodDef pdict_methods[] = {
   {"set", (PyCFunction)pdict_set, METH_VARARGS,
    "set($self, key, val, /)\n--\n\nReturns a copy of the pdict that maps the given key to the given value."},
   {"drop", (PyCFunction)(void(*)(void))pdict_drop, METH_VARARGS | METH_KEYWORDS,
    "drop($self, key, error=False)\n--\n\nReturns a copy of the pdict without the given key; if the key is absent,\n"
    "returns the pdict itself, or raises KeyError if error is true."},
   {"clear", (PyCFunction)pdict_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nReturns the empty pdict."},
   {"transient", (PyCFunction)pdict_transient, METH_NOARGS,
    "transient($self, /)\n--\n\nEfficiently copies the pdict into a tdict and returns the tdict."},
   {"get", (PyCFunction)pdict_get, METH_VARARGS,
    "get($self, key, default=None, /)\n--\n\nReturns the value for key if key is in the pdict, else default."},
   {"keys", (PyCFunction)pdict_keys, METH_NOARGS, "keys($self, /)\n--\n\nReturns a view of the keys."},
   {"items", (PyCFunction)pdict_items, METH_NOARGS, "items($self, /)\n--\n\nReturns a view of the items."},
   {"values", (PyCFunction)pdict_values, METH_NOARGS, "values($self, /)\n--\n\nReturns a view of the values."},
   PCOLL_CLASS_GETITEM_METHODDEF
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
   {Py_tp_doc, (void*)PyDoc_STR(
      "A persistent (immutable) dictionary.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    pdict() -> an empty pdict\n"
      "    pdict(mapping) -> a pdict with the items of mapping\n"
      "    pdict(iterable) -> a pdict with the (key, value) pairs of iterable\n"
      "    pdict(**kwargs) -> a pdict with the given keyword items\n"
      "\n"
      "The arguments are those of dict. Methods that would change a dict instead\n"
      "return a new pdict (set, setall, update, drop, dropall, delete, deleteall,\n"
      "setdefault, pop, popitem, clear). A pdict remembers the order in which its\n"
      "keys were added, and is hashable if its values are. transient() returns a\n"
      "tdict copy in constant time.")},
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
   // Subclassable; ldict (lazy.c.h) is a subclass.
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
   PyObject* orig;          // a persistent dict with the same contents
                            // (owned), or NULL; dropped on any change.
   pcoll_tguard guard;      // see core.h.
   PyObject* weaklist;
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
   // See pdict_dealloc().
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   tdict_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}
static Py_ssize_t tdict_length_impl(TDictObject* self) {
   return self->count;
}
PCOLL_LOCKED0(Py_ssize_t, tdict_length, tdict_length_impl, TDictObject*)
// Wraps the tries in a new instance of `type` (TDictType or a subtype),
// consuming the references to `els`, `idx`, and `orig`. tdict_wrap() always
// makes a plain tdict.
static PyObject* tdict_wrap_astype(PyTypeObject* type, Trie_t els, Trie_t idx,
                                    Py_ssize_t top, Py_ssize_t count,
                                    Py_ssize_t ndeleted, PyObject* orig) {
   // See pdict_wrap_astype() on tp_alloc.
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

// Defined below (setitem and delitem).
static int tdict_ass_subscript(TDictObject* self, PyObject* key, PyObject* val);

// Builds a new `type` instance from arg.items() if `arg` is a Mapping, and
// otherwise from `arg` as an iterable of key-value pairs (each converted
// with PySequence_Tuple, so any 2-item iterable works, as with dict()).
// Returns a new reference, or NULL with an exception set.
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

// Defined below.
static PyObject* pdict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored));

static PyObject* tdict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* arg;
   PyObject* kwds_to_merge;
   PyObject* obj;
   if (n == 0) {
      if (!kwds || PyDict_Size(kwds) == 0) return tdict_empty_astype(type);
      // Keyword arguments only: kwds is the sole source of items, and is
      // not merged in again afterward.
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
   // Storage is shared only when arg's type is exactly tdict or pdict.
   // Other types, including the lazy subclasses, are read through
   // arg.items(), which computes lazy values.
   if (Py_TYPE(arg) == ST(TDictType)) {
      // Share frozen tries with the source tdict, and its cached original,
      // which has the same contents.
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
      // Share the pdict's tries, as pdict_transient() does; `type` may be
      // a subclass of tdict.
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
// (untruncated).
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
                           &self->guard, (PyObject*)self, 1);
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

// Compacts the tdict in place if it has enough tombstones. Fails only if
// hashing a key fails (see dict_rebuild_compacted()).
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
                           &old_val, &prev, &self->guard, (PyObject*)self,
                           0);
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
      // Tombstone the deleted slot (see dictentry_is_tombstone()).
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


// Returns a new instance of `ptype` (a persistent partner type) holding the
// transient's contents; the cached original is used when it has that type.
static PyObject* tdict_persistent_as(TDictObject* self, PyTypeObject* ptype) {
   Trie_t els, idx;
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return NULL;
   if (self->orig && Py_TYPE(self->orig) == ptype) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   if (self->count == 0) return pdict_type_empty(ptype);
   if (tdict_share(self, &els, &idx) < 0) return NULL;
   return pdict_wrap_astype(ptype, els, idx, self->top, self->count,
                            self->ndeleted);
}
// The persistent partner type of the tdict `self` (a new reference).
static PyTypeObject* tdict_partner(PyObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyTypeObject* result = NULL;
   if (tp == ST(TDictType)) result = ST(PDictType);
   else if (ST(TLDictType) && tp == ST(TLDictType)) result = ST(LDictType);
   if (result) {
      Py_INCREF(result);
      return result;
   }
   return pcoll_partner_type(self, "__persistent_type__", ST(PDictType));
}
static PyObject* tdict_persistent_impl(TDictObject* self) {
   PyTypeObject* ptype = tdict_partner((PyObject*)self);
   PyObject* result;
   if (!ptype) return NULL;
   result = tdict_persistent_as(self, ptype);
   Py_DECREF(ptype);
   return result;
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
   PyTypeObject* tp = Py_TYPE(self);
   PyTypeObject* ttype = NULL;
   PyObject* result;
   if (tp == ST(PDictType)) ttype = ST(TDictType);
   else if (ST(LDictType) && tp == ST(LDictType)) ttype = ST(TLDictType);
   if (ttype) {
      Py_INCREF(ttype);
   } else {
      ttype = pcoll_partner_type((PyObject*)self, "__transient_type__",
                                 ST(TDictType));
      if (!ttype) return NULL;
   }
   trienode_incref(self->els);
   trienode_incref(self->idx);
   // The transient keeps a reference to self as its cached original.
   Py_INCREF(self);
   result = tdict_wrap_astype(ttype, self->els, self->idx, self->top,
                              self->count, self->ndeleted, (PyObject*)self);
   Py_DECREF(ttype);
   return result;
}

// tdict.empty() is a classmethod that returns a new empty instance of cls
// (whereas pdict.empty is a shared class attribute).
static PyObject* tdict_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   return tdict_empty_astype((PyTypeObject*)cls);
}

static PyMethodDef tdict_methods[] = {
   {"empty", (PyCFunction)tdict_empty_classmethod, METH_NOARGS | METH_CLASS,
    "empty($type, /)\n--\n\nReturns a new, empty tdict."},
   {"clear", (PyCFunction)tdict_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nClears all elements from the tdict."},
   {"persistent", (PyCFunction)tdict_persistent, METH_NOARGS,
    "persistent($self, /)\n--\n\nEfficiently copies the tdict into a pdict and returns the pdict."},
   {"get", (PyCFunction)tdict_get, METH_VARARGS,
    "get($self, key, default=None, /)\n--\n\nReturns the value for key if key is in the tdict, else default."},
   {"keys", (PyCFunction)tdict_keys, METH_NOARGS, "keys($self, /)\n--\n\nReturns a view of the keys."},
   {"items", (PyCFunction)tdict_items, METH_NOARGS, "items($self, /)\n--\n\nReturns a view of the items."},
   {"values", (PyCFunction)tdict_values, METH_NOARGS, "values($self, /)\n--\n\nReturns a view of the values."},
   PCOLL_CLASS_GETITEM_METHODDEF
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
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient (mutable) dictionary.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    tdict() -> an empty tdict\n"
      "    tdict(mapping) -> a tdict with the items of mapping\n"
      "    tdict(iterable) -> a tdict with the (key, value) pairs of iterable\n"
      "    tdict(**kwargs) -> a tdict with the given keyword items\n"
      "\n"
      "A tdict has the interface of dict. persistent() returns a pdict copy in\n"
      "constant time. A tdict is meant to be used by one thread at a time; a\n"
      "modification that overlaps another modification raises RuntimeError.")},
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
   // Subclassable; tldict (lazy.c.h) is a subclass.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tdict_slots,
};


//=============================================================================
// pdict construction from an argument and/or keywords (defined here because
// the general case builds a tdict).

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
      // Storage is shared with a tdict (or subclass) that is not lazy: reading
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
         // arg already has the requested type: return it.
         Py_INCREF(arg);
         return arg;
      }
      if (Py_TYPE(arg) == ST(PDictType)) {
         // A plain pdict: share its tries in a new `type` instance.
         PDictObject* p = (PDictObject*)arg;
         trienode_incref(p->els);
         trienode_incref(p->idx);
         return pdict_wrap_astype(type, p->els, p->idx, p->top, p->count,
                                    p->ndeleted);
      }
   }
   // General case (including keyword-only construction): build a plain
   // tdict from the arguments, then wrap its frozen tries in a `type`
   // instance. Other collections, including ldict, are read through
   // items(), which computes lazy values.
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
// One iterator type for pdict and one for tdict. Each walks `els` directly
// and yields keys (for __iter__), values, or items (for the __iter__ of the
// values() and items() views, which so avoid a lookup per key).

typedef enum {
   DICTITER_KEYS = 0,
   DICTITER_VALUES = 1,
   DICTITER_ITEMS = 2
} DictIterMode;

typedef struct {
   PyObject_HEAD
   PyObject* owner;    // the pdict/tdict being walked (not a view); owned.
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
   // Skip tombstones.
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
// or if a modification is in progress, and otherwise (only values were
// replaced) finds its place again by key.
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
      if (tguard_read(&t->guard, (PyObject*)t, "iteration") < 0) return NULL;
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
// There is a keys, items, and values view type for pdict and for tdict. Each
// subclasses a plain mixin from pcollections.abc (_KeysViewBase,
// _ItemsViewBase, _ValuesViewBase) and is registered as a virtual subclass
// of the matching collections.abc view. The views add no C fields (the
// mapping is kept in the base's `_mapping` slot) and inherit their methods,
// except that the items and values views iterate natively (see the
// iterators above).

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
   // Subclasses (such as ldict and tldict) are accepted.
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

// The Py_tp_traverse and Py_tp_clear entries are placeholders that
// build_view_type() fills in from the base type (see there).
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
// .basicsize is set from the base type by build_view_type().
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

// Builds a view type on top of `base` (a plain mixin from
// pcollections.abc._view). The views add no fields of their own, so their
// basicsize is the base's. PyType_FromSpecWithBases does not inherit
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
   pcoll_set_weaklistoffset(st->PDictType, offsetof(PDictObject, weaklist));
   pcoll_set_weaklistoffset(st->TDictType, offsetof(TDictObject, weaklist));
   if (pcoll_type_setattr(st->PDictType, "__transient_type__",
                          (PyObject*)st->TDictType) < 0 ||
       pcoll_type_setattr(st->TDictType, "__persistent_type__",
                          (PyObject*)st->PDictType) < 0)
      goto done;
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
   // The views are registered with the collections.abc view ABCs.
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

   // The canonical empty pdict, also stored as pdict.empty.
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
