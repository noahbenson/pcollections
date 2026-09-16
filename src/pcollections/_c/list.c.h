///////////////////////////////////////////////////////////////////////////////
// _c/list.c.h
// The persistent (plist) and transient (tlist) list types, implemented as
// thin CPython wrappers around a FAT node tree (see fat.h/trie.h).
//
// A list's elements are just the values of a FAT tree whose keys are a run
// of consecutive trieint_t (unsigned) integers: `start`, `start+1`, ...,
// `start+length-1`. `start` is allowed to "wrap" below 0 the way an unsigned
// integer wraps (prepending decrements it, exactly mirroring the reference
// Python implementation's use of an arbitrary-precision, possibly-negative
// `_start`) -- since FAT's efficiency comes from keys being densely
// clustered (sharing common high bits), not from being near zero, a run of
// keys sitting near the top of the unsigned range is exactly as cheap to
// store as one sitting near 0. Wrapping all the way around trieint_t's
// range would take on the order of 2**64 net prepends, which is not a
// real-world concern.
//
// Every FAT node used here has leafsize == sizeof(PyObject*): a leaf cell
// holds the bytes of a PyObject* pointer, never a raw value. `leaf_incref`/
// `leaf_decref` callbacks (`pyobj_incref`/`pyobj_decref` below) always see a
// pointer to a stored PyObject* (a PyObject**), per fat.h's own convention:
// dereference once to get the actual object pointer.
//
// The reference Python implementation of plist/tlist (pcollections/_list.py)
// is the interface spec this file follows: same method names, same
// semantics, translated onto our own FAT tree instead of the external
// `phamt` package's PHAMT/THAMT types.
//
// Scope note: plist/tlist implement the "must implement" core of
// PersistentSequence/TransientSequence (see pcollections/abc/_seq.py's own
// docstrings for that list) natively in C -- set/delete/append/prepend/
// insert/clear/transient/persistent/__iter__/__len__/__getitem__/
// __setitem__/__delitem__/__reduce__ -- plus __repr__/__hash__/__eq__/
// ordering and cyclic-GC support. PListType/TListType are built as HEAP
// types at import time (see pcoll_exec_list, and the comment on PListType's
// declaration below) specifically so they can set PersistentSequence/
// TransientSequence (imported from pcollections.abc at runtime) as their
// tp_base: this means everything else -- count/index/extend/sort/reverse/
// __add__/__radd__/__iadd__/__mul__/__rmul__/__imul__/pop/remove/copy/
// __json__/isinstance(..., collections.abc.Sequence)/etc. -- comes for free
// via ordinary Python-level method inheritance, exactly as if `plist`/
// `tlist` had been declared in Python as subclasses of those ABC mixins,
// rather than being reimplemented here. plist/tlist ARE subclassable in C
// (Py_TPFLAGS_BASETYPE is set on both -- see the note on their PyType_Spec
// flags below): pcollections._c._core's `llist`/`tllist` subclass them
// directly, so every construction site that the reference's plist/tlist
// route through `self._new(...)`/`cls._new(...)`/`cls.empty` now threads
// `Py_TYPE(self)`/`type` through instead of hardcoding PListType/TListType,
// mirroring dict.c.h's identical pdict/tdict subclassing support (see that
// file's header comment for the fuller rationale, including the
// tp_alloc-vs-PyObject_GC_New memory-safety subtlety that a subclass with
// auto-added __dict__/__weakref__ slots requires).


#define PYLEAFSIZE ((uint8_t)sizeof(PyObject*))

// The `start` value used for every freshly-constructed (not derived from an
// existing list) plist/tlist: the midpoint of the unsigned trieint_t range,
// rather than 0. A list's elements live at keys start, start+1, ...,
// start+length-1, and `start` is allowed to wrap (see the file header
// comment above) -- but fat_firstpath/fat_nextpath iterate in strictly
// ascending *numeric* key order, not a wraparound-aware "logical" order. If
// a fresh list started at start==0, a single prepend would underflow start
// to SIZE_MAX, and that element would then sort *last* instead of *first*
// during iteration (__iter__/__repr__/list()/__hash__/etc.), even though
// direct indexing (which computes start+idx explicitly) would still be
// correct. Starting at the midpoint instead gives symmetric headroom in
// both directions: reaching real wraparound would take on the order of
// 2**63 net prepends or appends, which is not a real-world concern.
#define LIST_START_MID ((trieint_t)1 << (TRIEINT_WIDTH - 1))




//=============================================================================
// Index/slice helpers.

// Normalizes a Python-style possibly-negative index against length `n`,
// raising IndexError (with `what` as the leading word of the message) and
// returning -1 if it's out of range; returns 0 and writes the normalized
// (always in [0, n)) index into *out otherwise.
static int normalize_index(Py_ssize_t idx, Py_ssize_t n, const char* what,
                           Py_ssize_t* out) {
   if (idx < 0) idx += n;
   if (idx < 0 || idx >= n) {
      PyErr_Format(PyExc_IndexError, "%s index out of range", what);
      return -1;
   }
   *out = idx;
   return 0;
}


//=============================================================================
// plist

typedef struct PListObject {
   PyObject_HEAD
   Trie_t root;         // owned ref; always fully persistent.
   trieint_t start;      // logical key of element 0 (wraps mod 2**64).
   Py_ssize_t length;    // cached element count (kept O(1) -- FAT nodes
                         // don't track a subtree element count themselves).
   Py_hash_t hashcode;   // -1 == not yet computed (matches CPython's usual
                         // cached-hash convention; a genuine hash of -1 is
                         // remapped to -2, same as tuple/str do).
} PListObject;

// The types, the empty plist, and seqstr live in the module state (core.h).

// Takes ownership of the one reference to `root` that callers already hold
// (per this file's fat_anditem/fat_butitem/tfat_* usage convention) and
// wraps it in a new plist, EXCEPT that a 0-length result always collapses to
// the canonical empty plist singleton instead (mirroring FAT's own "0
// occupants iff canonical empty node" invariant one layer up, and the
// reference implementation's habit of returning `plist.empty` rather than a
// fresh 0-length instance).
// `type` must be PListType or a subtype of it (layout-compatible as a
// PListObject -- true for any subclass adding no new slots, the only kind
// this Py_TPFLAGS_BASETYPE-enabled subclassing supports; see pdict_wrap_
// astype's identical note in dict.c.h). The 0-length collapse to the
// canonical empty singleton is, per that same file's convention, a pure
// optimization applied *only* when `type` is exactly PListType -- for any
// other (necessarily subclass, e.g. llist) target type we always build a
// fresh, correctly-typed instance instead, since collapsing to the
// unrelated g_plist_empty would silently lose the subclass.
static PyObject* plist_wrap_astype(PyTypeObject* type, Trie_t root,
                                    trieint_t start, Py_ssize_t length) {
   PListObject* self;
   if (length == 0 && type == ST(PListType)) {
      fatnode_decref(root, pyobj_decref);
      Py_INCREF(ST(g_plist_empty));
      return (PyObject*)ST(g_plist_empty);
   }
   // tp_alloc (not PyObject_GC_New) so a subclass's auto-added trailing
   // fields (__dict__/__weakref__) are zero-initialized -- see dict.c.h's
   // pdict_wrap_astype comment for the full rationale. tp_alloc already
   // GC-tracks + INCREFs the type, so no manual PyObject_GC_Track here;
   // plist_dealloc balances the INCREF with Py_DECREF(Py_TYPE(self)).
   self = (PListObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(root, pyobj_decref);
      return NULL;
   }
   self->root = root;
   self->start = start;
   self->length = length;
   self->hashcode = -1;
   return (PyObject*)self;
}
static PyObject* plist_wrap(Trie_t root, trieint_t start, Py_ssize_t length) {
   return plist_wrap_astype(ST(PListType), root, start, length);
}

// Returns the canonical empty instance of `type` (a new reference), mirroring
// the reference's `cls.empty` class-attribute lookup in plist.__new__ (plist,
// unlike tlist, stores `empty` as a plain attribute rather than a
// classmethod). For plain PListType this is just the g_plist_empty
// singleton; for any other (necessarily subclass) type, the subclass is
// expected to have set its own `empty` class attribute (falling back to
// pdict.empty/plist.empty via ordinary MRO attribute lookup if it hasn't).
static PyObject* plist_type_empty(PyTypeObject* type) {
   if (type == ST(PListType)) {
      Py_INCREF(ST(g_plist_empty));
      return (PyObject*)ST(g_plist_empty);
   }
   return PyObject_GetAttrString((PyObject*)type, "empty");
}

static int plist_traverse(PListObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT((PyObject*)self->root);
   return 0;
}
static int plist_clear(PListObject* self) {
   Trie_t root = self->root;
   self->root = NULL;
   if (root) fatnode_decref(root, pyobj_decref);
   return 0;
}
static void plist_dealloc(PListObject* self) {
   // Standard CPython heap-type dealloc idiom, balancing tp_alloc's
   // Py_INCREF(type) -- see pdict_dealloc's comment in dict.c.h.
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   plist_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}

static Py_ssize_t plist_length(PListObject* self) {
   return self->length;
}

// Builds a plist out of a general Python iterable, key 0, 1, 2, ..., as an
// instance of `type` -- mirrors plist.__new__'s final fallback branch
// (build a THAMT, then `cls._new(phamt, 0)` unless it's empty, in which case
// `cls.empty`). Returns NULL (with an exception set) on failure.
static PyObject* plist_from_iterable_astype(PyTypeObject* type, PyObject* iterable) {
   PyObject* iterator = PyObject_GetIter(iterable);
   Trie_t root;
   PyObject* item;
   Py_ssize_t n = 0;
   if (!iterator) return NULL;
   root = fat_empty(PYLEAFSIZE);
   while ((item = PyIter_Next(iterator))) {
      root = tfat_setitem(root, LIST_START_MID + (trieint_t)n, &item, pyobj_incref, pyobj_decref);
      Py_DECREF(item);
      ++n;
   }
   Py_DECREF(iterator);
   if (PyErr_Occurred()) {
      fatnode_decref(root, pyobj_decref);
      return NULL;
   }
   fat_freeze(root);
   if (n == 0) {
      // Explicit collapse-to-cls.empty, matching the reference's own
      // explicit `if len(phamt) == 0: return cls.empty` check here (plist_
      // wrap_astype's *implicit* collapse only ever applies when `type` is
      // exactly PListType, so a subclass target needs this spelled out).
      fatnode_decref(root, pyobj_decref);
      return plist_type_empty(type);
   }
   return plist_wrap_astype(type, root, LIST_START_MID, n);
}

static PyObject* plist_new_dispatch(PyTypeObject* type, PyObject* arg);

static PyObject* plist_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n;
   PyObject* arg;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "plist() takes no keyword arguments");
      return NULL;
   }
   n = PyTuple_GET_SIZE(args);
   if (n == 0) {
      return plist_type_empty(type);
   } else if (n != 1) {
      PyErr_Format(PyExc_TypeError,
                   "plist expects at most 1 argument, got %zd", n);
      return NULL;
   }
   arg = PyTuple_GET_ITEM(args, 0);
   // (tlist support is wired in further down, once TListType exists --
   // see plist_new_dispatch() below.)
   return plist_new_dispatch(type, arg);
}

// Shared repr/str helper for both types: calls the real
// pcollections.util.seqstr on `self` (so ordering/formatting/truncation
// come from the actual reference implementation, not a hand-rolled copy)
// and wraps the result in the type's delimiter pair.
static PyObject* seq_str(PyObject* self, const char* open, const char* close,
                          int truncate) {
   PyObject* s = call_seqstr(self, 60, truncate, NULL);
   PyObject* result;
   if (!s) return NULL;
   result = PyUnicode_FromFormat("%s%U%s", open, s, close);
   Py_DECREF(s);
   return result;
}

// Matches abc/_seq.py's PersistentSequence.__repr__/__str__: both use the
// "[|...|]" delimiter, __str__ truncated at 60 chars, __repr__ not.
static PyObject* plist_repr(PListObject* self) {
   return seq_str((PyObject*)self, "[|", "|]", 0);
}
static PyObject* plist_str(PListObject* self) {
   return seq_str((PyObject*)self, "[|", "|]", 1);
}

static Py_hash_t plist_hash(PListObject* self) {
   Py_ssize_t n, i;
   PyObject* tup;
   TriePath path;
   int ok;
   Py_hash_t h;
   if (self->hashcode != -1) return self->hashcode;
   n = self->length;
   tup = PyTuple_New(n);
   if (!tup) return -1;
   for (ok = fat_firstpath(self->root, &path), i = 0; ok;
        ok = fat_nextpath(&path), ++i) {
      PyObject* item = *(PyObject**)triepath_val(&path);
      Py_INCREF(item);
      PyTuple_SET_ITEM(tup, i, item);
   }
   h = PyObject_Hash(tup);
   Py_DECREF(tup);
   if (h == -1) return -1;
   h += 1;
   if (h == -1) h = -2;
   self->hashcode = h;
   return h;
}

static PyObject* plist_item(PListObject* self, Py_ssize_t i) {
   void* valptr;
   Py_ssize_t idx;
   int found;
   PyObject* val;
   if (normalize_index(i, self->length, "plist", &idx) < 0) return NULL;
   found = fat_lookup(self->root, self->start + (trieint_t)idx, &valptr);
   if (!found) {
      // Should be unreachable given the invariant that every logical index
      // in [0, length) has a real stored key -- but never trust that from
      // deep inside a C extension without a real check.
      PyErr_SetString(PyExc_RuntimeError, "plist: internal lookup failure");
      return NULL;
   }
   val = *(PyObject**)valptr;
   Py_INCREF(val);
   return val;
}

// Shared slice-fetch implementation for plist/tlist: builds a fresh
// transient tree (keys 0..slicelen-1) out of the elements
// self->root[self->start + start_i + k*step_i] for k in [0, slicelen), then
// freezes it. Used by both plist.__getitem__ and tlist.__getitem__.
static Trie_t fat_getslice(Trie_t root, trieint_t start, PyObject* slice,
                           Py_ssize_t length, Py_ssize_t* out_n) {
   Py_ssize_t start_i, stop_i, step_i, slicelen, k;
   Trie_t work;
   if (PySlice_GetIndicesEx(slice, length, &start_i, &stop_i, &step_i,
                            &slicelen) < 0)
      return NULL;
   work = fat_empty(PYLEAFSIZE);
   for (k = 0; k < slicelen; ++k) {
      Py_ssize_t srcidx = start_i + k * step_i;
      void* valptr;
      PyObject* val;
      fat_lookup(root, start + (trieint_t)srcidx, &valptr);
      val = *(PyObject**)valptr;
      work = tfat_setitem(work, LIST_START_MID + (trieint_t)k, &val, pyobj_incref, pyobj_decref);
   }
   fat_freeze(work);
   *out_n = slicelen;
   return work;
}

static PyObject* plist_subscript(PListObject* self, PyObject* key) {
   if (PySlice_Check(key)) {
      Py_ssize_t n;
      Trie_t work = fat_getslice(self->root, self->start, key, self->length,
                                 &n);
      if (!work) return NULL;
      // Matches reference __getitem__'s slice branch: `self._new(...)` --
      // subclass-preserving (unlike e.g. plist.clear(), which hardcodes the
      // base plist.empty; see each method's own comment for which case it is).
      return plist_wrap_astype(Py_TYPE(self), work, LIST_START_MID, n);
   } else {
      Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
      if (i == -1 && PyErr_Occurred()) return NULL;
      return plist_item(self, i);
   }
}

static PyObject* plist_iter(PListObject* self);

static PyObject* plist_set(PListObject* self, PyObject* args) {
   Py_ssize_t idx;
   PyObject* index_obj;
   PyObject* obj;
   void* valptr;
   Trie_t new_root;
   if (!PyArg_ParseTuple(args, "OO", &index_obj, &obj)) return NULL;
   {
      Py_ssize_t i = PyNumber_AsSsize_t(index_obj, PyExc_IndexError);
      if (i == -1 && PyErr_Occurred()) return NULL;
      if (normalize_index(i, self->length, "plist", &idx) < 0) return NULL;
   }
   fat_lookup(self->root, self->start + (trieint_t)idx, &valptr);
   if (*(PyObject**)valptr == obj) {
      Py_INCREF(self);
      return (PyObject*)self;
   }
   // fat_anditem() is the non-mutating persistent API: it never touches or
   // consumes a reference to its input tree (unlike tfat_setitem()), so no
   // incref of self->root is needed here -- self keeps its own reference,
   // completely unaffected, and new_root is an independently-owned result.
   new_root = fat_anditem(self->root, self->start + (trieint_t)idx, &obj,
                          pyobj_incref);
   // Matches reference set()'s `self._new(...)` -- subclass-preserving.
   return plist_wrap_astype(Py_TYPE(self), new_root, self->start, self->length);
}

static PyObject* plist_delete(PListObject* self, PyObject* args) {
   Py_ssize_t index = -1, idx, n;
   trieint_t st;
   Trie_t work;
   if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
   n = self->length;
   if (n == 0) {
      PyErr_SetString(PyExc_IndexError, "delete from empty plist");
      return NULL;
   }
   if (normalize_index(index, n, "plist.delete", &idx) < 0) return NULL;
   st = self->start;
   // NOTE on subclass preservation in delete(): the reference _list.py's
   // delete() has a genuine, deliberate asymmetry -- these first two "easy"
   // branches (removing the first or last element directly) call the
   // hardcoded base `plist._new`/`plist.empty`, NOT `self._new`/`cls.empty`,
   // so they do NOT preserve a subclass; only the general "moving elements
   // around" branch below (which the reference spells `self._new(...)`)
   // does. plist_wrap (hardcoded to PListType) and g_plist_empty here match
   // that exactly -- this is intentional fidelity to the reference, not an
   // oversight.
   if (idx == 0) {
      if (n == 1) {
         Py_INCREF(ST(g_plist_empty));
         return (PyObject*)ST(g_plist_empty);
      }
      // fat_butitem(), like fat_anditem(), never touches or consumes a
      // reference to its input -- no incref of self->root needed here.
      work = fat_butitem(self->root, st, pyobj_incref);
      return plist_wrap(work, st + 1, n - 1);
   } else if (idx == n - 1) {
      work = fat_butitem(self->root, st + (trieint_t)idx, pyobj_incref);
      return plist_wrap(work, st, n - 1);
   }
   // From here on, `work` is fed through tfat_setitem()/tfat_delitem() (the
   // *transient*, reference-consuming mutators), so it does need its own,
   // separately-owned starting reference -- hence the incref self keeps its
   // own self->root reference throughout, fully unaffected.
   trienode_incref(self->root);
   work = self->root;
   if (n - idx <= idx) {
      Py_ssize_t ii;
      for (ii = idx; ii < n - 1; ++ii) {
         void* valptr; PyObject* val;
         trieint_t key = st + (trieint_t)ii;
         fat_lookup(self->root, key + 1, &valptr);
         val = *(PyObject**)valptr;
         work = tfat_setitem(work, key, &val, pyobj_incref, pyobj_decref);
      }
      work = tfat_delitem(work, st + (trieint_t)(n - 1), pyobj_incref,
                          pyobj_decref);
   } else {
      Py_ssize_t ii;
      for (ii = 0; ii < idx; ++ii) {
         void* valptr; PyObject* val;
         trieint_t key = st + (trieint_t)ii;
         fat_lookup(self->root, key, &valptr);
         val = *(PyObject**)valptr;
         work = tfat_setitem(work, key + 1, &val, pyobj_incref, pyobj_decref);
      }
      work = tfat_delitem(work, st, pyobj_incref, pyobj_decref);
      st += 1;
   }
   fat_freeze(work);
   // Unlike the two branches above, this general "moving elements around"
   // path DOES preserve subclass in the reference (`self._new(...)`).
   return plist_wrap_astype(Py_TYPE(self), work, st, n - 1);
}

static PyObject* plist_append(PListObject* self, PyObject* obj) {
   Trie_t new_root;
   // fat_anditem() doesn't touch/consume self->root -- no incref needed.
   new_root = fat_anditem(self->root, self->start + (trieint_t)self->length,
                          &obj, pyobj_incref);
   // Matches reference append()'s `self._new(...)` -- subclass-preserving.
   return plist_wrap_astype(Py_TYPE(self), new_root, self->start, self->length + 1);
}

static PyObject* plist_prepend(PListObject* self, PyObject* obj) {
   trieint_t new_start = self->start - 1;
   Trie_t new_root;
   new_root = fat_anditem(self->root, new_start, &obj, pyobj_incref);
   // Matches reference prepend()'s `self._new(...)` -- subclass-preserving.
   return plist_wrap_astype(Py_TYPE(self), new_root, new_start, self->length + 1);
}

static PyObject* plist_insert(PListObject* self, PyObject* args) {
   Py_ssize_t index, n;
   PyObject* obj;
   trieint_t st;
   Trie_t work;
   if (!PyArg_ParseTuple(args, "nO", &index, &obj)) return NULL;
   n = self->length;
   if (index == 0) return plist_prepend(self, obj);
   if (index == n) return plist_append(self, obj);
   if (index < -n) index = -n;
   else if (index > n) index = n;
   if (index < 0) index += n;
   st = self->start;
   trienode_incref(self->root);
   work = self->root;
   if (n - index <= index) {
      Py_ssize_t ii;
      for (ii = n - 1; ii >= index; --ii) {
         void* valptr; PyObject* val;
         trieint_t key = st + (trieint_t)ii;
         fat_lookup(self->root, key, &valptr);
         val = *(PyObject**)valptr;
         work = tfat_setitem(work, key + 1, &val, pyobj_incref, pyobj_decref);
      }
      work = tfat_setitem(work, st + (trieint_t)index, &obj, pyobj_incref,
                          pyobj_decref);
   } else {
      Py_ssize_t ii;
      for (ii = 0; ii < index; ++ii) {
         void* valptr; PyObject* val;
         trieint_t key = st + (trieint_t)ii;
         fat_lookup(self->root, key, &valptr);
         val = *(PyObject**)valptr;
         work = tfat_setitem(work, key - 1, &val, pyobj_incref, pyobj_decref);
      }
      st -= 1;
      work = tfat_setitem(work, st + (trieint_t)index, &obj, pyobj_incref,
                          pyobj_decref);
   }
   fat_freeze(work);
   // Matches reference insert()'s final `self._new(...)` -- subclass-
   // preserving (its index==0/index==n edge cases already delegate to
   // plist_prepend/plist_append above, which are themselves preserving).
   return plist_wrap_astype(Py_TYPE(self), work, st, n + 1);
}

static PyObject* plist_clear_method(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   // Matches reference clear()'s `return plist.empty` -- hardcoded to the
   // base plist singleton, deliberately NOT subclass-preserving (unlike
   // most other mutators here).
   (void)self;
   Py_INCREF(ST(g_plist_empty));
   return (PyObject*)ST(g_plist_empty);
}

static PyObject* plist_transient(PListObject* self, PyObject* Py_UNUSED(ignored));

static PyObject* plist_richcompare(PListObject* self, PyObject* other, int op);

static PyMethodDef plist_methods[] = {
   {"set", (PyCFunction)plist_set, METH_VARARGS,
    "Returns a copy of the list with the given index set to the given object."},
   {"delete", (PyCFunction)plist_delete, METH_VARARGS,
    "Returns a copy of the plist with the item at index removed (default: last)."},
   {"append", (PyCFunction)plist_append, METH_O,
    "Returns a new list with object appended."},
   {"prepend", (PyCFunction)plist_prepend, METH_O,
    "Returns a new list with object prepended."},
   {"insert", (PyCFunction)plist_insert, METH_VARARGS,
    "Returns a new plist with object inserted before index."},
   {"clear", (PyCFunction)plist_clear_method, METH_NOARGS,
    "Returns the empty plist."},
   {"transient", (PyCFunction)plist_transient, METH_NOARGS,
    "Efficiently copies the plist into a tlist and returns the tlist."},
   {NULL, NULL, 0, NULL}
};

// PListType is built as a HEAP type (see the note on its declaration above),
// via PyType_FromSpecWithBases() in pcoll_exec_list -- so what used to be a
// single static PyTypeObject initializer is a PyType_Spec/PyType_Slot pair
// instead; the actual PyTypeObject* is constructed at import time, once
// PersistentSequence (fetched at runtime from pcollections.abc) is on hand
// to pass as the base.
static PyType_Slot plist_slots[] = {
   {Py_tp_dealloc, (void*)plist_dealloc},
   {Py_tp_repr, (void*)plist_repr},
   {Py_tp_str, (void*)plist_str},
   {Py_sq_length, (void*)plist_length},
   {Py_sq_item, (void*)plist_item},
   {Py_mp_length, (void*)plist_length},
   {Py_mp_subscript, (void*)plist_subscript},
   {Py_tp_hash, (void*)plist_hash},
   {Py_tp_doc,
    (void*)"A persistent list type similar to `list`, backed by a FAT tree."},
   {Py_tp_traverse, (void*)plist_traverse},
   {Py_tp_clear, (void*)plist_clear},
   {Py_tp_richcompare, (void*)plist_richcompare},
   {Py_tp_iter, (void*)plist_iter},
   {Py_tp_methods, (void*)plist_methods},
   {Py_tp_new, (void*)plist_new},
   {0, NULL}
};
static PyType_Spec plist_spec = {
   .name = "pcollections.plist",
   .basicsize = sizeof(PListObject),
   .itemsize = 0,
   // Py_TPFLAGS_BASETYPE: plist is subclassable (pcollections._c._core's
   // `llist` subclasses it directly) -- see the file header comment and
   // dict.c.h's identical note on pdict_spec/tdict_spec's flags.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = plist_slots,
};


//=============================================================================
// tlist

typedef struct {
   PyObject_HEAD
   Trie_t root;          // owned ref; may be transient or persistent.
   trieint_t start;
   Py_ssize_t length;
   PyObject* orig;        // cached plist (owned ref), or NULL. Set only by
                          // plist.transient(); invalidated (decref'd and
                          // reset to NULL) by every mutation, exactly as the
                          // reference implementation does. Never set by the
                          // tlist(plist_instance) constructor path -- see
                          // that constructor's comment.
} TListObject;

// `type` must be TListType or a subtype of it (see the analogous note on
// plist_wrap_astype above); `tlist_wrap()` below is the TListType-only
// convenience wrapper used at call sites that (per the reference _list.py)
// are never meant to be subclass-aware -- see each call site's own comment.
static PyObject* tlist_wrap_astype(PyTypeObject* type, Trie_t root,
                                   trieint_t start, Py_ssize_t length,
                                   PyObject* orig /* borrowed; NULL for none */) {
   // tp_alloc (not PyObject_GC_New) -- see plist_wrap_astype's comment;
   // tlist_dealloc balances the heap-type INCREF it performs.
   TListObject* self = (TListObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(root, pyobj_decref);
      Py_XDECREF(orig);
      return NULL;
   }
   self->root = root;
   self->start = start;
   self->length = length;
   self->orig = orig;  // ownership transferred in from caller
   return (PyObject*)self;
}
static PyObject* tlist_wrap(Trie_t root, trieint_t start, Py_ssize_t length,
                            PyObject* orig /* borrowed; NULL for none */) {
   return tlist_wrap_astype(ST(TListType), root, start, length, orig);
}

static int tlist_traverse(TListObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->orig);
   Py_VISIT((PyObject*)self->root);
   return 0;
}
static int tlist_clear(TListObject* self) {
   Trie_t root = self->root;
   PyObject* orig = self->orig;
   self->root = NULL;
   self->orig = NULL;
   if (root) fatnode_decref(root, pyobj_decref);
   Py_XDECREF(orig);
   return 0;
}
static void tlist_dealloc(TListObject* self) {
   // See plist_dealloc's comment: balances tp_alloc's Py_INCREF(type).
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   tlist_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}

static Py_ssize_t tlist_length(TListObject* self) {
   return self->length;
}

// Every real mutation invalidates any cached `_orig` plist -- see this
// file's header comment on the `orig` field.
static void tlist_invalidate_orig(TListObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

// `tlist.empty` is a *classmethod* in the reference (unlike plist.empty,
// which is a plain stored attribute) -- it always builds a fresh instance
// rather than looking up a cached one, so `tlist_empty_astype` mirrors that
// directly instead of going through a `pdict_type_empty`-style `.empty`
// attribute lookup for subclasses.
static PyObject* tlist_empty_astype(PyTypeObject* type) {
   return tlist_wrap_astype(type, fat_empty(PYLEAFSIZE), LIST_START_MID, 0, NULL);
}

static PyObject* tlist_from_iterable_astype(PyTypeObject* type, PyObject* iterable) {
   PyObject* iterator = PyObject_GetIter(iterable);
   Trie_t root;
   PyObject* item;
   Py_ssize_t n = 0;
   if (!iterator) return NULL;
   root = fat_empty(PYLEAFSIZE);
   while ((item = PyIter_Next(iterator))) {
      root = tfat_setitem(root, LIST_START_MID + (trieint_t)n, &item, pyobj_incref, pyobj_decref);
      Py_DECREF(item);
      ++n;
   }
   Py_DECREF(iterator);
   if (PyErr_Occurred()) {
      fatnode_decref(root, pyobj_decref);
      return NULL;
   }
   return tlist_wrap_astype(type, root, LIST_START_MID, n, NULL);
}

static PyObject* tlist_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t n;
   PyObject* arg;
   if (kwds && PyDict_Size(kwds) > 0) {
      PyErr_SetString(PyExc_TypeError, "tlist() takes no keyword arguments");
      return NULL;
   }
   n = PyTuple_GET_SIZE(args);
   if (n == 0) return tlist_empty_astype(type);
   if (n != 1) {
      PyErr_Format(PyExc_TypeError,
                   "tlist expects at most 1 argument, got %zd", n);
      return NULL;
   }
   arg = PyTuple_GET_ITEM(args, 0);
   // isinstance(arg, plist) -- a real isinstance check, matching the
   // reference exactly (tlist.__new__ has no "isinstance(arg, cls)" or
   // "isinstance(arg, tlist)" fast path of its own; passing an existing
   // tlist/subclass instance here falls through to the generic iterable
   // path below, making an independent copy -- matches the reference's own
   // asymmetry between `tlist(p)` sharing a plist's root and `tlist(t)`
   // rebuilding from iteration). Deliberately also matches any future plist
   // subclass (e.g. llist), sharing its root raw/undereferenced -- see
   // dict.c.h's analogous comment on tdict_new's pdict-argument branch.
   if (PyObject_TypeCheck(arg, ST(PListType))) {
      // Direct constructor call: shares the plist's (already fully
      // persistent) root immediately -- no copying, no freezing needed.
      // Lazily claimed/copied node-by-node the moment any mutation actually
      // touches it, exactly as fatnode_set()/fatnode_del() already handle.
      // Deliberately does NOT cache `_orig` -- only plist.transient() does
      // that (see tlist's `orig` field comment and plist_transient() below);
      // this mirrors the reference implementation's own asymmetry between
      // `tlist(p)` and `p.transient()`.
      PListObject* p = (PListObject*)arg;
      trienode_incref(p->root);
      return tlist_wrap_astype(type, p->root, p->start, p->length, NULL);
   }
   return tlist_from_iterable_astype(type, arg);
}

// Matches abc/_seq.py's TransientSequence.__repr__/__str__: both use the
// "[<...>]" delimiter (no repr/str asymmetry here, unlike TransientMapping
// in dict.c.h), __str__ truncated at 60 chars, __repr__ not.
static PyObject* tlist_repr(TListObject* self) {
   return seq_str((PyObject*)self, "[<", ">]", 0);
}
static PyObject* tlist_str(TListObject* self) {
   return seq_str((PyObject*)self, "[<", ">]", 1);
}

static PyObject* tlist_item(TListObject* self, Py_ssize_t i) {
   void* valptr;
   Py_ssize_t idx;
   int found;
   PyObject* val;
   if (normalize_index(i, self->length, "tlist", &idx) < 0) return NULL;
   found = fat_lookup(self->root, self->start + (trieint_t)idx, &valptr);
   if (!found) {
      PyErr_SetString(PyExc_RuntimeError, "tlist: internal lookup failure");
      return NULL;
   }
   val = *(PyObject**)valptr;
   Py_INCREF(val);
   return val;
}

static PyObject* tlist_subscript(TListObject* self, PyObject* key) {
   if (PySlice_Check(key)) {
      Py_ssize_t n;
      Trie_t work = fat_getslice(self->root, self->start, key, self->length,
                                 &n);
      if (!work) return NULL;
      return tlist_wrap(work, LIST_START_MID, n, NULL);
   } else {
      Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
      if (i == -1 && PyErr_Occurred()) return NULL;
      return tlist_item(self, i);
   }
}

static int tlist_ass_item(TListObject* self, Py_ssize_t i, PyObject* v) {
   Py_ssize_t idx;
   if (v == NULL) {
      // `del tl[i]` via the sequence protocol.
      if (normalize_index(i, self->length, "tlist", &idx) < 0) return -1;
      if (self->length - idx <= idx) {
         Py_ssize_t ii;
         for (ii = idx; ii < self->length - 1; ++ii) {
            void* valptr; PyObject* val;
            trieint_t k = self->start + (trieint_t)ii;
            fat_lookup(self->root, k + 1, &valptr);
            val = *(PyObject**)valptr;
            self->root = tfat_setitem(self->root, k, &val, pyobj_incref,
                                      pyobj_decref);
         }
         self->root = tfat_delitem(self->root,
                                   self->start + (trieint_t)(self->length - 1),
                                   pyobj_incref, pyobj_decref);
      } else {
         Py_ssize_t ii;
         for (ii = idx; ii > 0; --ii) {
            void* valptr; PyObject* val;
            trieint_t k = self->start + (trieint_t)ii;
            fat_lookup(self->root, k - 1, &valptr);
            val = *(PyObject**)valptr;
            self->root = tfat_setitem(self->root, k, &val, pyobj_incref,
                                      pyobj_decref);
         }
         self->root = tfat_delitem(self->root, self->start, pyobj_incref,
                                   pyobj_decref);
         self->start += 1;
      }
      self->length -= 1;
      tlist_invalidate_orig(self);
      return 0;
   }
   if (normalize_index(i, self->length, "tlist", &idx) < 0) return -1;
   self->root = tfat_setitem(self->root, self->start + (trieint_t)idx, &v,
                             pyobj_incref, pyobj_decref);
   tlist_invalidate_orig(self);
   return 0;
}

static int tlist_ass_subscript(TListObject* self, PyObject* key, PyObject* v) {
   if (PySlice_Check(key)) {
      PyErr_SetString(PyExc_NotImplementedError,
                      "tlist slice assignment/deletion is not yet supported");
      return -1;
   }
   {
      Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
      if (i == -1 && PyErr_Occurred()) return -1;
      return tlist_ass_item(self, i, v);
   }
}

static PyObject* tlist_append(TListObject* self, PyObject* obj) {
   self->root = tfat_setitem(self->root,
                             self->start + (trieint_t)self->length,
                             &obj, pyobj_incref, pyobj_decref);
   self->length += 1;
   tlist_invalidate_orig(self);
   Py_RETURN_NONE;
}

static PyObject* tlist_prepend(TListObject* self, PyObject* obj) {
   self->start -= 1;
   self->root = tfat_setitem(self->root, self->start, &obj, pyobj_incref,
                             pyobj_decref);
   self->length += 1;
   tlist_invalidate_orig(self);
   Py_RETURN_NONE;
}

static PyObject* tlist_insert(TListObject* self, PyObject* args) {
   Py_ssize_t index, n;
   PyObject* obj;
   if (!PyArg_ParseTuple(args, "nO", &index, &obj)) return NULL;
   n = self->length;
   if (index < -n) index = -n;
   else if (index > n) index = n;
   if (index < 0) index += n;
   if (n - index <= index) {
      Py_ssize_t ii;
      for (ii = n; ii > index; --ii) {
         void* valptr; PyObject* val;
         trieint_t k = self->start + (trieint_t)(ii - 1);
         fat_lookup(self->root, k, &valptr);
         val = *(PyObject**)valptr;
         self->root = tfat_setitem(self->root, k + 1, &val, pyobj_incref,
                                   pyobj_decref);
      }
      self->root = tfat_setitem(self->root, self->start + (trieint_t)index,
                                &obj, pyobj_incref, pyobj_decref);
   } else {
      Py_ssize_t ii;
      for (ii = 0; ii < index; ++ii) {
         void* valptr; PyObject* val;
         trieint_t k = self->start + (trieint_t)ii;
         fat_lookup(self->root, k, &valptr);
         val = *(PyObject**)valptr;
         self->root = tfat_setitem(self->root, k - 1, &val, pyobj_incref,
                                   pyobj_decref);
      }
      self->start -= 1;
      self->root = tfat_setitem(self->root, self->start + (trieint_t)index,
                                &obj, pyobj_incref, pyobj_decref);
   }
   self->length += 1;
   tlist_invalidate_orig(self);
   Py_RETURN_NONE;
}

static PyObject* tlist_clear_method(TListObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t old_root = self->root;
   PyObject* old_orig = self->orig;
   self->root = fat_empty(PYLEAFSIZE);
   self->start = LIST_START_MID;
   self->length = 0;
   self->orig = NULL;
   fatnode_decref(old_root, pyobj_decref);
   Py_XDECREF(old_orig);
   Py_RETURN_NONE;
}

static PyObject* tlist_persistent(TListObject* self, PyObject* Py_UNUSED(ignored)) {
   Trie_t root;
   if (self->length == 0) {
      Py_INCREF(ST(g_plist_empty));
      return (PyObject*)ST(g_plist_empty);
   }
   if (self->orig) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   root = self->root;
   fat_freeze(root);
   trienode_incref(root);
   return plist_wrap(root, self->start, self->length);
}

static PyObject* tlist_pop(TListObject* self, PyObject* args) {
   Py_ssize_t index = -1, idx;
   PyObject* val;
   if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
   if (normalize_index(index, self->length, "pop", &idx) < 0) return NULL;
   val = tlist_item(self, idx);  // new reference
   if (!val) return NULL;
   if (tlist_ass_item(self, idx, NULL) < 0) {
      Py_DECREF(val);
      return NULL;
   }
   return val;
}

static PyObject* plist_transient(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   trienode_incref(self->root);
   Py_INCREF(self);
   return tlist_wrap(self->root, self->start, self->length, (PyObject*)self);
}

// Matches _list.py's tlist.empty being a *classmethod* (see
// tdict_empty_classmethod's comment in dict.c.h for the fuller rationale --
// same pattern here). Previously unexposed to Python at all.
static PyObject* tlist_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   return tlist_empty_astype((PyTypeObject*)cls);
}

static PyMethodDef tlist_methods[] = {
   {"empty", (PyCFunction)tlist_empty_classmethod, METH_NOARGS | METH_CLASS,
    "Returns a new, empty tlist."},
   {"append", (PyCFunction)tlist_append, METH_O,
    "Appends object to the end of the list."},
   {"prepend", (PyCFunction)tlist_prepend, METH_O,
    "Prepends object to the beginning of the tlist."},
   {"insert", (PyCFunction)tlist_insert, METH_VARARGS,
    "Inserts the given object before the given index."},
   {"clear", (PyCFunction)tlist_clear_method, METH_NOARGS,
    "Clears all elements from the tlist."},
   {"persistent", (PyCFunction)tlist_persistent, METH_NOARGS,
    "Efficiently copies the tlist into a plist and returns the plist."},
   {"pop", (PyCFunction)tlist_pop, METH_VARARGS,
    "Removes and returns the item at index (default last)."},
   {NULL, NULL, 0, NULL}
};

static PyObject* tlist_richcompare(TListObject* self, PyObject* other, int op);
static PyObject* tlist_iter(TListObject* self);

// TListType, like PListType above, is built as a heap type via
// PyType_FromSpecWithBases() in pcoll_exec_list, so it can inherit from
// TransientSequence.
static PyType_Slot tlist_slots[] = {
   {Py_tp_dealloc, (void*)tlist_dealloc},
   {Py_tp_repr, (void*)tlist_repr},
   {Py_tp_str, (void*)tlist_str},
   {Py_sq_length, (void*)tlist_length},
   {Py_sq_item, (void*)tlist_item},
   {Py_sq_ass_item, (void*)tlist_ass_item},
   {Py_mp_length, (void*)tlist_length},
   {Py_mp_subscript, (void*)tlist_subscript},
   {Py_mp_ass_subscript, (void*)tlist_ass_subscript},
   {Py_tp_doc,
    (void*)"A transient (mutable) list type similar to `list`, backed by a"
           " FAT tree."},
   {Py_tp_traverse, (void*)tlist_traverse},
   {Py_tp_clear, (void*)tlist_clear},
   {Py_tp_richcompare, (void*)tlist_richcompare},
   {Py_tp_iter, (void*)tlist_iter},
   {Py_tp_methods, (void*)tlist_methods},
   {Py_tp_new, (void*)tlist_new},
   {0, NULL}
};
static PyType_Spec tlist_spec = {
   .name = "pcollections.tlist",
   .basicsize = sizeof(TListObject),
   .itemsize = 0,
   // Py_TPFLAGS_BASETYPE: tlist is subclassable (pcollections._c._core's
   // `tllist` subclasses it directly) -- see the file header comment and
   // dict.c.h's identical note on pdict_spec/tdict_spec's flags.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tlist_slots,
};


//=============================================================================
// Generic elementwise comparison, shared by plist/tlist richcompare. Works
// against anything that supports len()/PySequence_GetItem (so plist, tlist,
// list, tuple, ... all work uniformly) without needing to know the other
// side's concrete type. Returns -2 on error (with an exception set), else a
// standard -1/0/1 lexicographic-order result.
static int generic_seq_compare(PyObject* a, PyObject* b) {
   Py_ssize_t na, nb, n, i;
   na = PySequence_Length(a);
   if (na < 0) return -2;
   nb = PySequence_Length(b);
   if (nb < 0) return -2;
   n = (na < nb) ? na : nb;
   for (i = 0; i < n; ++i) {
      PyObject* ea = PySequence_GetItem(a, i);
      PyObject* eb;
      int eq;
      if (!ea) return -2;
      eb = PySequence_GetItem(b, i);
      if (!eb) { Py_DECREF(ea); return -2; }
      eq = PyObject_RichCompareBool(ea, eb, Py_EQ);
      if (eq < 0) { Py_DECREF(ea); Py_DECREF(eb); return -2; }
      if (!eq) {
         int lt = PyObject_RichCompareBool(ea, eb, Py_LT);
         Py_DECREF(ea); Py_DECREF(eb);
         if (lt < 0) return -2;
         return lt ? -1 : 1;
      }
      Py_DECREF(ea); Py_DECREF(eb);
   }
   if (na < nb) return -1;
   if (na > nb) return 1;
   return 0;
}

// Matches PersistentSequence/TransientSequence._eq_types (and the type
// checks in their __lt__/__le__/__gt__/__ge__) exactly: plist/tlist compare
// against list, plist, and tlist, but deliberately NOT against tuple or
// other sequence types -- exactly like `list == tuple(...)` is always False
// in ordinary Python, regardless of matching contents.
static int is_seq_comparable(PyObject* o) {
   // isinstance-style checks (matching the reference's own
   // `isinstance(other, PersistentSequence._eq_types)` /
   // `TransientSequence._eq_types` -- see abc/_seq.py), not exact-type: any
   // plist/tlist subclass (e.g. the future llist/tllist) must compare
   // correctly too, not just the exact base types.
   return (PyObject_TypeCheck(o, ST(PListType)) || PyObject_TypeCheck(o, ST(TListType)) ||
           PyList_Check(o));
}

static PyObject* seq_richcompare(PyObject* self, PyObject* other, int op) {
   int cmp;
   if (op == Py_EQ || op == Py_NE) {
      if (other == self) Py_RETURN_RICHCOMPARE(0, 0, op);
      if (!is_seq_comparable(other)) Py_RETURN_NOTIMPLEMENTED;
      if (PySequence_Length(self) != PySequence_Length(other))
         return PyBool_FromLong(op == Py_NE);
      cmp = generic_seq_compare(self, other);
      if (cmp == -2) return NULL;
      return PyBool_FromLong((cmp == 0) == (op == Py_EQ));
   }
   if (!is_seq_comparable(other)) Py_RETURN_NOTIMPLEMENTED;
   cmp = generic_seq_compare(self, other);
   if (cmp == -2) return NULL;
   Py_RETURN_RICHCOMPARE(cmp, 0, op);
}

static PyObject* plist_richcompare(PListObject* self, PyObject* other, int op) {
   return seq_richcompare((PyObject*)self, other, op);
}
static PyObject* tlist_richcompare(TListObject* self, PyObject* other, int op) {
   return seq_richcompare((PyObject*)self, other, op);
}


//=============================================================================
// Iterators.
// One iterator type per container type (a plist_iterator can't outlive
// mutation concerns the way a tlist_iterator must eventually care about --
// see this file's header comment on the "no mutation during iteration of a
// transient" assumption, enforced later at the Python layer, not here).
// Both share the same walk (fat_firstpath()/fat_nextpath()); only the owner
// field's static type differs.

typedef struct {
   PyObject_HEAD
   PyObject* owner;   // strong ref to the plist/tlist being iterated, keyed
                      // off of *type* below to know which; keeps root alive.
   TriePath path;
   int state;         // 0 = not yet started, 1 = active, 2 = exhausted.
} SeqIterObject;

static void seqiter_dealloc(SeqIterObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   Py_XDECREF(self->owner);
   PyObject_GC_Del(self);
   Py_DECREF(tp);
}
static int seqiter_traverse(SeqIterObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->owner);
   return 0;
}

static Trie_t seqiter_owner_root(SeqIterObject* self) {
   // isinstance-style check (not exact-type): self->owner may legitimately
   // be a plist/tlist *subclass* instance (e.g. llist/tllist) now that
   // PListType/TListType are subclassable -- see dict.c.h's analogous fix to
   // dictiter_owner_els for the same reasoning.
   if (PyObject_TypeCheck(self->owner, ST(PListType)))
      return ((PListObject*)self->owner)->root;
   else
      return ((TListObject*)self->owner)->root;
}

static PyObject* seqiter_next(SeqIterObject* self) {
   int ok;
   PyObject* val;
   if (self->state == 2) return NULL;
   if (self->state == 0) {
      ok = fat_firstpath(seqiter_owner_root(self), &self->path);
      self->state = 1;
   } else {
      ok = fat_nextpath(&self->path);
   }
   if (!ok) {
      self->state = 2;
      return NULL;
   }
   val = *(PyObject**)triepath_val(&self->path);
   Py_INCREF(val);
   return val;
}

static PyObject* seqiter_self(PyObject* self) {
   Py_INCREF(self);
   return self;
}

static PyObject* make_seqiter(PyTypeObject* itertype, PyObject* owner) {
   SeqIterObject* it = PyObject_GC_New(SeqIterObject, itertype);
   if (!it) return NULL;
   Py_INCREF(owner);
   it->owner = owner;
   it->state = 0;
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

static PyType_Slot seqiter_slots[] = {
   {Py_tp_dealloc, (void*)seqiter_dealloc},
   {Py_tp_traverse, (void*)seqiter_traverse},
   {Py_tp_iter, (void*)seqiter_self},
   {Py_tp_iternext, (void*)seqiter_next},
   {0, NULL}
};
static PyType_Spec plistiter_spec = {
   .name = "pcollections._c._core.plist_iterator",
   .basicsize = sizeof(SeqIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = seqiter_slots,
};
static PyType_Spec tlistiter_spec = {
   .name = "pcollections._c._core.tlist_iterator",
   .basicsize = sizeof(SeqIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = seqiter_slots,
};

static PyObject* plist_iter(PListObject* self) {
   return make_seqiter(ST(PListIterType), (PyObject*)self);
}
static PyObject* tlist_iter(TListObject* self) {
   return make_seqiter(ST(TListIterType), (PyObject*)self);
}


//=============================================================================
// plist_new()'s tlist-argument case is defined down here, after TListType
// exists, and dispatched to from plist_new() above via this forward-declared
// helper. Mirrors plist.__new__'s dispatch chain exactly:
//   isinstance(arg, tlist)  -> cls.empty or cls._new(frozen root, arg._start)
//   isinstance(arg, cls)    -> arg, unchanged
//   isinstance(arg, plist)  -> cls.empty or cls._new(arg's root, arg._start)
//   else                    -> build fresh from a general iterable
static PyObject* plist_new_dispatch(PyTypeObject* type, PyObject* arg) {
   // isinstance(arg, tlist) -- a real isinstance check (matches any tlist
   // subclass, e.g. the future tllist), mirroring the reference's own
   // isinstance-based fast path; raw-shares the frozen root without
   // dereferencing any lazily-held values (see dict.c.h's analogous comment
   // on pdict_new_dispatch's tdict-argument branch for the rationale).
   if (PyObject_TypeCheck(arg, ST(TListType))) {
      TListObject* t = (TListObject*)arg;
      Trie_t root;
      if (t->length == 0) return plist_type_empty(type);
      root = t->root;
      fat_freeze(root);
      trienode_incref(root);
      return plist_wrap_astype(type, root, t->start, t->length);
   }
   // isinstance(arg, cls): arg is already the exact runtime type we're
   // being asked to build -- return it unchanged (matches reference's
   // `elif isinstance(arg, cls): return arg`).
   if (PyObject_TypeCheck(arg, type)) {
      Py_INCREF(arg);
      return arg;
   }
   // isinstance(arg, plist) but NOT already an instance of `type` -- only
   // reachable when `type` is a strict plist subclass and `arg` is some
   // other plist/plist-subclass instance not caught above.
   if (PyObject_TypeCheck(arg, ST(PListType))) {
      PListObject* p = (PListObject*)arg;
      if (p->length == 0) return plist_type_empty(type);
      trienode_incref(p->root);
      return plist_wrap_astype(type, p->root, p->start, p->length);
   }
   return plist_from_iterable_astype(type, arg);
}


//=============================================================================
// Module execution.

// Creates plist, tlist, their iterators, and the empty plist, and adds the
// public types to module `m`.
static int pcoll_exec_list(PyObject* m, pcoll_state* st) {
   PyObject* abc = NULL;
   PListObject* empty;
   int rc = -1;

   abc = PyImport_ImportModule("pcollections.abc");
   if (!abc) goto done;
   // plist/tlist implement their own tp_richcompare.
   st->PListType = build_abc_subtype(m, &plist_spec, abc,
                                     "_PersistentSequenceBase", 0);
   if (!st->PListType) goto done;
   st->TListType = build_abc_subtype(m, &tlist_spec, abc,
                                     "_TransientSequenceBase", 0);
   if (!st->TListType) goto done;
   if (register_virtual_subclass(abc, "PersistentSequence", st->PListType) < 0 ||
       register_virtual_subclass(abc, "TransientSequence", st->TListType) < 0)
      goto done;
   if (!(st->PListIterType = pcoll_new_internal_type(m, &plistiter_spec)) ||
       !(st->TListIterType = pcoll_new_internal_type(m, &tlistiter_spec)))
      goto done;

   empty = (PListObject*)st->PListType->tp_alloc(st->PListType, 0);
   if (!empty) goto done;
   empty->root = fat_empty(PYLEAFSIZE);
   empty->start = LIST_START_MID;
   empty->length = 0;
   empty->hashcode = -1;
   st->g_plist_empty = empty;
   if (pcoll_type_setattr(st->PListType, "empty", (PyObject*)empty) < 0)
      goto done;

   if (pcoll_module_add(m, "plist", (PyObject*)st->PListType) < 0 ||
       pcoll_module_add(m, "tlist", (PyObject*)st->TListType) < 0)
      goto done;
   rc = 0;
done:
   Py_XDECREF(abc);
   return rc;
}
