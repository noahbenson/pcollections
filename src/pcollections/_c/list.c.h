///////////////////////////////////////////////////////////////////////////////
// _c/list.c.h
// The persistent (plist) and transient (tlist) list types, implemented as
// thin CPython wrappers around a FAT node tree (see fat.h/trie.h).
//
// A list's elements are the leaves of a FAT whose keys are the consecutive
// integers start, start+1, ..., start+length-1. Prepending decrements
// `start`; a new list starts at LIST_START_MID (see below). FAT storage
// depends on keys being clustered, not on their size, so the position of
// the run in the key range does not matter.
//
// Every FAT node here has leafsize == sizeof(PyObject*); the leaf callbacks
// (pyobj_incref/pyobj_decref) receive a PyObject**.
//
// plist and tlist implement the core sequence methods natively (set,
// delete, append, prepend, insert, clear, transient/persistent, indexing,
// slicing, iteration, len), plus repr, hashing, and comparison. The rest of
// the sequence API (count, index, extend, sort, +, *, pop, ...) is inherited
// from their pcollections.abc bases (see pcoll_exec_list()). Both types can
// be subclassed (llist and tllist in lazy.c.h are subclasses), and
// operations that return a new list build an instance of type(self), or of
// the type being constructed, so subclasses are preserved.


#define PYLEAFSIZE ((uint8_t)sizeof(PyObject*))

// The `start` of a new list: the midpoint of the key range. Iteration visits
// keys in numeric order, so `start` must not wrap past zero; starting in the
// middle leaves room for about 2**63 prepends or appends.
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
   Trie_t root;         // owned; always persistent.
   trieint_t start;      // key of element 0.
   Py_ssize_t length;    // element count (FAT nodes do not track counts).
   Py_hash_t hashcode;   // cached hash, or -1 if not yet computed.
   PyObject* weaklist;
} PListObject;

// The types, the empty plist, and seqstr live in the module state (core.h).

// Wraps `root` (whose reference is consumed) in a new instance of `type`,
// which must be PListType or a subtype of it. An empty result is type's
// empty instance instead (see plist_type_empty()).
static PyObject* plist_type_empty(PyTypeObject* type);
static PyObject* plist_wrap_astype(PyTypeObject* type, Trie_t root,
                                    trieint_t start, Py_ssize_t length) {
   PListObject* self;
   if (length == 0) {
      fatnode_decref(root, pyobj_decref);
      return plist_type_empty(type);
   }
   // See pdict_wrap_astype() (dict.c.h) on tp_alloc.
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
// A new empty instance of `type`.
static PyObject* plist_make_empty(PyTypeObject* type) {
   PListObject* self = (PListObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   self->root = fat_empty(PYLEAFSIZE);
   self->start = LIST_START_MID;
   self->hashcode = -1;
   return (PyObject*)self;
}
// The empty instance of `type` (see pcoll_type_empty in core.h).
static PyObject* plist_type_empty(PyTypeObject* type) {
   if (type == ST(PListType)) {
      Py_INCREF(ST(g_plist_empty));
      return (PyObject*)ST(g_plist_empty);
   }
   return pcoll_type_empty(type, plist_make_empty);
}

// Converts a subscript to an index, raising TypeError as list does.
static int seq_index_arg(PyObject* self, PyObject* key, Py_ssize_t* out) {
   if (!PyIndex_Check(key)) {
      PyErr_Format(PyExc_TypeError,
                   "%.200s indices must be integers or slices, not %.200s",
                   pcoll_short_name(Py_TYPE(self)),
                   pcoll_short_name(Py_TYPE(key)));
      return -1;
   }
   *out = PyNumber_AsSsize_t(key, PyExc_IndexError);
   if (*out == -1 && PyErr_Occurred()) return -1;
   return 0;
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
   // See pdict_dealloc() (dict.c.h).
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   plist_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}

static Py_ssize_t plist_length(PListObject* self) {
   return self->length;
}

// Builds an instance of `type` holding the elements of `iterable`. Returns
// NULL with an exception set on failure.
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
   // plist_new_dispatch() is defined after tlist.
   return plist_new_dispatch(type, arg);
}

// repr/str helper for both types: pcollections.util.seqstr(self), wrapped
// in the type's delimiters, truncated at 60 characters if `truncate`.
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
   {
      Py_hash_t cached = PCOLL_HASH_LOAD(self->hashcode);
      if (cached != -1) return cached;
   }
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
   PCOLL_HASH_STORE(self->hashcode, h);
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
      // Unreachable: every index in [0, length) has a key.
      PyErr_SetString(PyExc_RuntimeError, "plist: internal lookup failure");
      return NULL;
   }
   val = *(PyObject**)valptr;
   Py_INCREF(val);
   return val;
}

// Slicing for plist and tlist: returns a new persistent tree holding the
// elements root[start + start_i + k*step_i] for k in [0, slicelen), at keys
// LIST_START_MID + k, and sets *out_n to slicelen.
static Trie_t fat_getslice(Trie_t root, trieint_t start, Py_ssize_t length,
                           Py_ssize_t start_i, Py_ssize_t stop_i,
                           Py_ssize_t step_i, Py_ssize_t* out_n) {
   // The slice was unpacked (which may run user code) by the caller.
   Py_ssize_t slicelen = PySlice_AdjustIndices(length, &start_i, &stop_i,
                                               step_i);
   Py_ssize_t k;
   Trie_t work;
   work = fat_empty(PYLEAFSIZE);
   for (k = 0; k < slicelen; ++k) {
      Py_ssize_t srcidx = start_i + k * step_i;
      void* valptr = NULL;
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
      Py_ssize_t n, a, b, c;
      Trie_t work;
      if (PySlice_Unpack(key, &a, &b, &c) < 0) return NULL;
      work = fat_getslice(self->root, self->start, self->length, a, b, c, &n);
      if (!work) return NULL;
      return plist_wrap_astype(Py_TYPE(self), work, LIST_START_MID, n);
   } else {
      Py_ssize_t i;
      if (seq_index_arg((PyObject*)self, key, &i) < 0) return NULL;
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
   // fat_anditem() does not consume its input; new_root is a new reference.
   new_root = fat_anditem(self->root, self->start + (trieint_t)idx, &obj,
                          pyobj_incref);
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
   // Removing the first or last element is a single trie removal; any other
   // index shifts the elements on the shorter side by one.
   if (idx == 0) {
      if (n == 1) return plist_type_empty(Py_TYPE(self));
      // fat_butitem() does not consume its input.
      work = fat_butitem(self->root, st, pyobj_incref);
      return plist_wrap_astype(Py_TYPE(self), work, st + 1, n - 1);
   } else if (idx == n - 1) {
      work = fat_butitem(self->root, st + (trieint_t)idx, pyobj_incref);
      return plist_wrap_astype(Py_TYPE(self), work, st, n - 1);
   }
   // The transient mutators consume their input, so `work` starts with its
   // own reference to the root.
   trienode_incref(self->root);
   work = self->root;
   if (n - idx <= idx) {
      Py_ssize_t ii;
      for (ii = idx; ii < n - 1; ++ii) {
         void* valptr = NULL; PyObject* val;
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
         void* valptr = NULL; PyObject* val;
         trieint_t key = st + (trieint_t)ii;
         fat_lookup(self->root, key, &valptr);
         val = *(PyObject**)valptr;
         work = tfat_setitem(work, key + 1, &val, pyobj_incref, pyobj_decref);
      }
      work = tfat_delitem(work, st, pyobj_incref, pyobj_decref);
      st += 1;
   }
   fat_freeze(work);
   return plist_wrap_astype(Py_TYPE(self), work, st, n - 1);
}

static PyObject* plist_append(PListObject* self, PyObject* obj) {
   Trie_t new_root;
   new_root = fat_anditem(self->root, self->start + (trieint_t)self->length,
                          &obj, pyobj_incref);
   return plist_wrap_astype(Py_TYPE(self), new_root, self->start, self->length + 1);
}

static PyObject* plist_prepend(PListObject* self, PyObject* obj) {
   trieint_t new_start = self->start - 1;
   Trie_t new_root;
   new_root = fat_anditem(self->root, new_start, &obj, pyobj_incref);
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
         void* valptr = NULL; PyObject* val;
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
         void* valptr = NULL; PyObject* val;
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
   return plist_wrap_astype(Py_TYPE(self), work, st, n + 1);
}

static PyObject* plist_clear_method(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   return plist_type_empty(Py_TYPE(self));
}

static PyObject* plist_transient(PListObject* self, PyObject* Py_UNUSED(ignored));

static PyObject* plist_richcompare(PListObject* self, PyObject* other, int op);

static PyMethodDef plist_methods[] = {
   {"set", (PyCFunction)plist_set, METH_VARARGS,
    "set($self, index, obj, /)\n--\n\nReturns a copy of the list with the given index set to the given object."},
   {"delete", (PyCFunction)plist_delete, METH_VARARGS,
    "delete($self, index=-1, /)\n--\n\nReturns a copy of the plist with the item at index removed (default: last)."},
   {"append", (PyCFunction)plist_append, METH_O,
    "append($self, obj, /)\n--\n\nReturns a new list with object appended."},
   {"prepend", (PyCFunction)plist_prepend, METH_O,
    "prepend($self, obj, /)\n--\n\nReturns a new list with object prepended."},
   {"insert", (PyCFunction)plist_insert, METH_VARARGS,
    "insert($self, index, obj, /)\n--\n\nReturns a new plist with object inserted before index."},
   {"clear", (PyCFunction)plist_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nReturns the empty plist."},
   {"transient", (PyCFunction)plist_transient, METH_NOARGS,
    "transient($self, /)\n--\n\nEfficiently copies the plist into a tlist and returns the tlist."},
   PCOLL_CLASS_GETITEM_METHODDEF
   {NULL, NULL, 0, NULL}
};

// PListType is a heap type created in pcoll_exec_list() with a
// pcollections.abc base.
static PyType_Slot plist_slots[] = {
   {Py_tp_dealloc, (void*)plist_dealloc},
   {Py_tp_repr, (void*)plist_repr},
   {Py_tp_str, (void*)plist_str},
   {Py_sq_length, (void*)plist_length},
   {Py_sq_item, (void*)plist_item},
   {Py_mp_length, (void*)plist_length},
   {Py_mp_subscript, (void*)plist_subscript},
   {Py_tp_hash, (void*)plist_hash},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A persistent (immutable) list.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    plist() -> an empty plist\n"
      "    plist(iterable) -> a plist of the elements of iterable\n"
      "\n"
      "Methods that would change a list instead return a new plist (set, append,\n"
      "prepend, extend, insert, delete, drop, remove, pop, sort, reverse, clear).\n"
      "A plist is hashable if its elements are. transient() returns a tlist copy in\n"
      "constant time.")},
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
   // Subclassable; llist (lazy.c.h) is a subclass.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = plist_slots,
};


//=============================================================================
// tlist

typedef struct {
   PyObject_HEAD
   Trie_t root;          // owned; may be transient or persistent.
   trieint_t start;
   Py_ssize_t length;
   PyObject* orig;        // the plist this was made from by transient()
                          // (owned), or NULL; dropped on any change.
   pcoll_tguard guard;    // see core.h.
   PyObject* weaklist;
} TListObject;

// Wraps `root` in a new instance of `type` (TListType or a subtype),
// consuming the references to `root` and `orig`. tlist_wrap() always makes
// a plain tlist.
static PyObject* tlist_wrap_astype(PyTypeObject* type, Trie_t root,
                                   trieint_t start, Py_ssize_t length,
                                   PyObject* orig /* owned; or NULL */) {
   // See pdict_wrap_astype() (dict.c.h) on tp_alloc.
   TListObject* self = (TListObject*)type->tp_alloc(type, 0);
   if (!self) {
      fatnode_decref(root, pyobj_decref);
      Py_XDECREF(orig);
      return NULL;
   }
   self->root = root;
   self->start = start;
   self->length = length;
   self->orig = orig;
   return (PyObject*)self;
}
static PyObject* tlist_wrap(Trie_t root, trieint_t start, Py_ssize_t length,
                            PyObject* orig /* owned; or NULL */) {
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
   // See pdict_dealloc() (dict.c.h).
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   tlist_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}

static Py_ssize_t tlist_length_impl(TListObject* self) {
   return self->length;
}
PCOLL_LOCKED0(Py_ssize_t, tlist_length, tlist_length_impl, TListObject*)

// Drops the cached original; called on every change.
static void tlist_invalidate_orig(TListObject* self) {
   PyObject* orig = self->orig;
   self->orig = NULL;
   Py_XDECREF(orig);
}

// A new empty instance of `type` (tlist.empty() is a classmethod).
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
   // A plist (or subclass) that is not lazy shares its root; the new tlist
   // copies nodes as it changes them. Unlike plist.transient(), this does
   // not cache the plist as `orig`. Anything else, including a tlist, is
   // copied by iteration.
   if (PyObject_TypeCheck(arg, ST(PListType)) && !pcoll_holds_lazy(arg)) {
      PListObject* p = (PListObject*)arg;
      trienode_incref(p->root);
      return tlist_wrap_astype(type, p->root, p->start, p->length, NULL);
   }
   return tlist_from_iterable_astype(type, arg);
}

// Matches abc/_seq.py's TransientSequence.__repr__/__str__: both use the
// "[<...>]" delimiter, __str__ truncated at 60 chars, __repr__ not.
static PyObject* tlist_repr(TListObject* self) {
   return seq_str((PyObject*)self, "[<", ">]", 0);
}
static PyObject* tlist_str(TListObject* self) {
   return seq_str((PyObject*)self, "[<", ">]", 1);
}

static PyObject* tlist_item_impl(TListObject* self, Py_ssize_t i) {
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
static PyObject* tlist_read_item_impl(TListObject* self, Py_ssize_t i) {
   if (tguard_read(&self->guard, (PyObject*)self, "a lookup") < 0)
      return NULL;
   return tlist_item_impl(self, i);
}
PCOLL_LOCKED1(PyObject*, tlist_item, tlist_read_item_impl,
              TListObject*, Py_ssize_t)

static PyObject* tlist_getslice_impl(TListObject* self, PyObject* bounds) {
   // `bounds` is a tuple of the unpacked (start, stop, step).
   Py_ssize_t n;
   Trie_t work;
   if (tguard_read(&self->guard, (PyObject*)self, "a lookup") < 0)
      return NULL;
   work = fat_getslice(
      self->root, self->start, self->length,
      PyLong_AsSsize_t(PyTuple_GET_ITEM(bounds, 0)),
      PyLong_AsSsize_t(PyTuple_GET_ITEM(bounds, 1)),
      PyLong_AsSsize_t(PyTuple_GET_ITEM(bounds, 2)), &n);
   if (!work) return NULL;
   return tlist_wrap_astype(Py_TYPE(self), work, LIST_START_MID, n, NULL);
}
PCOLL_LOCKED1(PyObject*, tlist_getslice, tlist_getslice_impl,
              TListObject*, PyObject*)

static PyObject* tlist_subscript(TListObject* self, PyObject* key) {
   if (PySlice_Check(key)) {
      Py_ssize_t a, b, c;
      PyObject* bounds, *result;
      if (PySlice_Unpack(key, &a, &b, &c) < 0) return NULL;
      bounds = Py_BuildValue("(nnn)", a, b, c);
      if (!bounds) return NULL;
      result = tlist_getslice(self, bounds);
      Py_DECREF(bounds);
      return result;
   } else {
      Py_ssize_t i;
      if (seq_index_arg((PyObject*)self, key, &i) < 0) return NULL;
      return tlist_item(self, i);
   }
}

// The bodies of the tlist modifications, run with the guard held and the
// index already normalized.
static void tlist_delete_guarded(TListObject* self, Py_ssize_t idx) {
   Py_ssize_t ii;
   tguard_keys_changed(&self->guard);
   // Shift whichever side of idx is shorter over the deleted element.
   if (self->length - idx <= idx) {
      for (ii = idx; ii < self->length - 1; ++ii) {
         void* valptr = NULL; PyObject* val;
         trieint_t k = self->start + (trieint_t)ii;
         fat_lookup(self->root, k + 1, &valptr);
         val = *(PyObject**)valptr;
         tfat_setitem_at(&self->root, k, &val, pyobj_incref, pyobj_decref);
      }
      self->length -= 1;
      tfat_delitem_at(&self->root, self->start + (trieint_t)self->length,
                      pyobj_incref, pyobj_decref);
   } else {
      for (ii = idx; ii > 0; --ii) {
         void* valptr = NULL; PyObject* val;
         trieint_t k = self->start + (trieint_t)ii;
         fat_lookup(self->root, k - 1, &valptr);
         val = *(PyObject**)valptr;
         tfat_setitem_at(&self->root, k, &val, pyobj_incref, pyobj_decref);
      }
      self->start += 1;
      self->length -= 1;
      tfat_delitem_at(&self->root, self->start - 1, pyobj_incref, pyobj_decref);
   }
   tlist_invalidate_orig(self);
}
static void tlist_insert_guarded(TListObject* self, Py_ssize_t index,
                                 PyObject* obj) {
   Py_ssize_t ii, n = self->length;
   tguard_keys_changed(&self->guard);
   if (n - index <= index) {
      for (ii = n; ii > index; --ii) {
         void* valptr = NULL; PyObject* val;
         trieint_t k = self->start + (trieint_t)(ii - 1);
         fat_lookup(self->root, k, &valptr);
         val = *(PyObject**)valptr;
         tfat_setitem_at(&self->root, k + 1, &val, pyobj_incref, pyobj_decref);
      }
   } else {
      for (ii = 0; ii < index; ++ii) {
         void* valptr = NULL; PyObject* val;
         trieint_t k = self->start + (trieint_t)ii;
         fat_lookup(self->root, k, &valptr);
         val = *(PyObject**)valptr;
         tfat_setitem_at(&self->root, k - 1, &val, pyobj_incref, pyobj_decref);
      }
      self->start -= 1;
   }
   // The slot at `index` now holds a duplicate of a neighbor (or, at either
   // end, nothing); either way the element count is as below once it's set.
   self->length += 1;
   tfat_setitem_at(&self->root, self->start + (trieint_t)index, &obj,
                   pyobj_incref, pyobj_decref);
   tlist_invalidate_orig(self);
}

static int tlist_ass_item_impl(TListObject* self, Py_ssize_t i, PyObject* v) {
   Py_ssize_t idx;
   if (normalize_index(i, self->length, "tlist", &idx) < 0) return -1;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return -1;
   if (v == NULL) {
      // `del tl[i]` via the sequence protocol.
      tlist_delete_guarded(self, idx);
   } else {
      tguard_changed(&self->guard);
      tfat_setitem_at(&self->root, self->start + (trieint_t)idx, &v,
                      pyobj_incref, pyobj_decref);
      tlist_invalidate_orig(self);
   }
   tguard_exit(&self->guard);
   return 0;
}
PCOLL_LOCKED2(int, tlist_ass_item, tlist_ass_item_impl,
              TListObject*, Py_ssize_t, PyObject*)

// Replaces the contents of the tlist with the elements of the list
// `items`. The caller holds the guard.
static void tlist_replace_guarded(TListObject* self, PyObject* items) {
   Py_ssize_t i, n = PyList_GET_SIZE(items);
   Trie_t root = fat_empty(PYLEAFSIZE);
   Trie_t old = self->root;
   for (i = 0; i < n; ++i) {
      PyObject* val = PyList_GET_ITEM(items, i);
      root = tfat_setitem(root, LIST_START_MID + (trieint_t)i, &val,
                          pyobj_incref, pyobj_decref);
   }
   tguard_keys_changed(&self->guard);
   self->root = root;
   self->start = LIST_START_MID;
   self->length = n;
   tlist_invalidate_orig(self);
   fatnode_decref(old, pyobj_decref);
}
// The raw elements of the tlist, as a new list.
static PyObject* tlist_raw_list(TListObject* self) {
   PyObject* items = PyList_New(self->length);
   TriePath path;
   Py_ssize_t i = 0;
   int ok;
   if (!items) return NULL;
   for (ok = fat_firstpath(self->root, &path); ok && i < self->length;
        ok = fat_nextpath(&path), ++i) {
      PyObject* val = *(PyObject**)triepath_val(&path);
      Py_INCREF(val);
      PyList_SET_ITEM(items, i, val);
   }
   return items;
}
// Slice assignment and deletion, with list's semantics: the elements are
// copied into a list, the list is changed, and the tlist is rebuilt from it
// (so this takes time proportional to the length).
static int tlist_ass_slice_impl(TListObject* self, PyObject* key, PyObject* v) {
   PyObject* items;
   int rc;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return -1;
   items = tlist_raw_list(self);
   rc = !items ? -1
      : v ? PyObject_SetItem(items, key, v)
      : PyObject_DelItem(items, key);
   if (rc == 0) tlist_replace_guarded(self, items);
   tguard_exit(&self->guard);
   Py_XDECREF(items);
   return rc;
}
PCOLL_LOCKED2(int, tlist_ass_slice, tlist_ass_slice_impl,
              TListObject*, PyObject*, PyObject*)
static int tlist_ass_subscript(TListObject* self, PyObject* key, PyObject* v) {
   if (PySlice_Check(key)) {
      int rc;
      if (!v) return tlist_ass_slice(self, key, NULL);
      // Take the new elements first (reading `v` may run user code, or read
      // this tlist).
      v = PySequence_List(v);
      if (!v) {
         if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            PyErr_SetString(PyExc_TypeError,
                            "can only assign an iterable");
         }
         return -1;
      }
      rc = tlist_ass_slice(self, key, v);
      Py_DECREF(v);
      return rc;
   }
   {
      Py_ssize_t i;
      if (seq_index_arg((PyObject*)self, key, &i) < 0) return -1;
      return tlist_ass_item(self, i, v);
   }
}

static PyObject* tlist_insert_impl(TListObject* self, Py_ssize_t index,
                                   PyObject* obj) {
   Py_ssize_t n = self->length;
   if (index < -n) index = -n;
   else if (index > n) index = n;
   if (index < 0) index += n;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return NULL;
   tlist_insert_guarded(self, index, obj);
   tguard_exit(&self->guard);
   Py_RETURN_NONE;
}
PCOLL_LOCKED2(PyObject*, tlist_insert_locked, tlist_insert_impl,
              TListObject*, Py_ssize_t, PyObject*)

static PyObject* tlist_append(TListObject* self, PyObject* obj) {
   return tlist_insert_locked(self, PY_SSIZE_T_MAX, obj);
}

static PyObject* tlist_prepend(TListObject* self, PyObject* obj) {
   return tlist_insert_locked(self, 0, obj);
}

static PyObject* tlist_insert(TListObject* self, PyObject* args) {
   Py_ssize_t index;
   PyObject* obj;
   if (!PyArg_ParseTuple(args, "nO", &index, &obj)) return NULL;
   return tlist_insert_locked(self, index, obj);
}

static PyObject* tlist_clear_impl(TListObject* self) {
   Trie_t old_root = self->root;
   PyObject* old_orig = self->orig;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return NULL;
   tguard_keys_changed(&self->guard);
   self->root = fat_empty(PYLEAFSIZE);
   self->start = LIST_START_MID;
   self->length = 0;
   self->orig = NULL;
   tguard_exit(&self->guard);
   fatnode_decref(old_root, pyobj_decref);
   Py_XDECREF(old_orig);
   Py_RETURN_NONE;
}
PCOLL_LOCKED0(PyObject*, tlist_clear_locked, tlist_clear_impl, TListObject*)
static PyObject* tlist_clear_method(TListObject* self, PyObject* Py_UNUSED(ignored)) {
   return tlist_clear_locked(self);
}

// Freezes the transient's trie and returns a new reference to it, for
// sharing with another collection. Returns NULL (with RuntimeError set) if a
// modification is in progress. The caller holds the transient's lock.
static Trie_t tlist_share(TListObject* self) {
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return NULL;
   fat_freeze(self->root);
   trienode_incref(self->root);
   return self->root;
}

// Returns a new instance of `ptype` (a persistent partner type) holding the
// transient's contents; the cached original is used when it has that type.
static PyObject* tlist_persistent_as(TListObject* self, PyTypeObject* ptype) {
   Trie_t root;
   if (tguard_check(&self->guard, (PyObject*)self) < 0) return NULL;
   if (self->orig && Py_TYPE(self->orig) == ptype) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   if (self->length == 0) return plist_type_empty(ptype);
   root = tlist_share(self);
   if (!root) return NULL;
   return plist_wrap_astype(ptype, root, self->start, self->length);
}
static PyObject* tlist_persistent_impl(TListObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyTypeObject* ptype = NULL;
   PyObject* result;
   if (tp == ST(TListType)) ptype = ST(PListType);
   else if (ST(TLListType) && tp == ST(TLListType)) ptype = ST(LListType);
   if (ptype) {
      Py_INCREF(ptype);
   } else {
      ptype = pcoll_partner_type((PyObject*)self, "__persistent_type__",
                                 ST(PListType));
      if (!ptype) return NULL;
   }
   result = tlist_persistent_as(self, ptype);
   Py_DECREF(ptype);
   return result;
}
PCOLL_LOCKED0(PyObject*, tlist_persistent_locked, tlist_persistent_impl,
              TListObject*)
static PyObject* tlist_persistent(TListObject* self, PyObject* Py_UNUSED(ignored)) {
   return tlist_persistent_locked(self);
}

static PyObject* tlist_pop_impl(TListObject* self, Py_ssize_t index) {
   Py_ssize_t idx;
   PyObject* val;
   if (self->length == 0) {
      PyErr_Format(PyExc_IndexError, "pop from empty %s",
                   pcoll_short_name(Py_TYPE(self)));
      return NULL;
   }
   if (normalize_index(index, self->length, "pop", &idx) < 0) return NULL;
   if (tguard_enter(&self->guard, (PyObject*)self) < 0) return NULL;
   val = tlist_item_impl(self, idx);  // new reference
   if (val) tlist_delete_guarded(self, idx);
   tguard_exit(&self->guard);
   return val;
}
PCOLL_LOCKED1(PyObject*, tlist_pop_locked, tlist_pop_impl,
              TListObject*, Py_ssize_t)
static PyObject* tlist_pop(TListObject* self, PyObject* args) {
   Py_ssize_t index = -1;
   if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
   return tlist_pop_locked(self, index);
}

static PyObject* plist_transient(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   PyTypeObject* tp = Py_TYPE(self);
   PyTypeObject* ttype = NULL;
   PyObject* result;
   if (tp == ST(PListType)) ttype = ST(TListType);
   else if (ST(LListType) && tp == ST(LListType)) ttype = ST(TLListType);
   if (ttype) {
      Py_INCREF(ttype);
   } else {
      ttype = pcoll_partner_type((PyObject*)self, "__transient_type__",
                                 ST(TListType));
      if (!ttype) return NULL;
   }
   trienode_incref(self->root);
   Py_INCREF(self);
   result = tlist_wrap_astype(ttype, self->root, self->start, self->length,
                              (PyObject*)self);
   Py_DECREF(ttype);
   return result;
}

// tlist.empty() is a classmethod that returns a new empty instance of cls.
static PyObject* tlist_empty_classmethod(PyObject* cls, PyObject* Py_UNUSED(ignored)) {
   return tlist_empty_astype((PyTypeObject*)cls);
}

static PyMethodDef tlist_methods[] = {
   {"empty", (PyCFunction)tlist_empty_classmethod, METH_NOARGS | METH_CLASS,
    "empty($type, /)\n--\n\nReturns a new, empty tlist."},
   {"append", (PyCFunction)tlist_append, METH_O,
    "append($self, obj, /)\n--\n\nAppends object to the end of the list."},
   {"prepend", (PyCFunction)tlist_prepend, METH_O,
    "prepend($self, obj, /)\n--\n\nPrepends object to the beginning of the tlist."},
   {"insert", (PyCFunction)tlist_insert, METH_VARARGS,
    "insert($self, index, obj, /)\n--\n\nInserts the given object before the given index."},
   {"clear", (PyCFunction)tlist_clear_method, METH_NOARGS,
    "clear($self, /)\n--\n\nClears all elements from the tlist."},
   {"persistent", (PyCFunction)tlist_persistent, METH_NOARGS,
    "persistent($self, /)\n--\n\nEfficiently copies the tlist into a plist and returns the plist."},
   {"pop", (PyCFunction)tlist_pop, METH_VARARGS,
    "pop($self, index=-1, /)\n--\n\nRemoves and returns the item at index (default last)."},
   PCOLL_CLASS_GETITEM_METHODDEF
   {NULL, NULL, 0, NULL}
};

static PyObject* tlist_richcompare(TListObject* self, PyObject* other, int op);
static PyObject* tlist_iter(TListObject* self);

// TListType, like PListType, is a heap type created in pcoll_exec_list().
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
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient (mutable) list.\n"
      "\n"
      "Usage::\n"
      "\n"
      "    tlist() -> an empty tlist\n"
      "    tlist(iterable) -> a tlist of the elements of iterable\n"
      "\n"
      "A tlist has the interface of list, plus prepend. persistent() returns a plist\n"
      "copy in constant time. A tlist is meant to be used by one thread at a time;\n"
      "a modification that overlaps another modification raises RuntimeError.")},
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
   // Subclassable; tllist (lazy.c.h) is a subclass.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tlist_slots,
};


//=============================================================================
// Comparison, shared by plist and tlist. Sequences compare with list, plist,
// and tlist (and their subclasses) as lists do: elements are compared with
// "identical or equal", and only the first pair that differs is ordered.

static int is_seq_comparable(PyObject* o) {
   return (PyObject_TypeCheck(o, ST(PListType)) ||
           PyObject_TypeCheck(o, ST(TListType)) || PyList_Check(o));
}

static PyObject* seq_richcompare(PyObject* self, PyObject* other, int op) {
   PyObject* a = NULL, *b = NULL, *result = NULL;
   Py_ssize_t na, nb, i;
   if (!is_seq_comparable(other)) Py_RETURN_NOTIMPLEMENTED;
   if (other == self && (op == Py_EQ || op == Py_NE))
      Py_RETURN_RICHCOMPARE(0, 0, op);
   // Compare snapshots of the elements, as the builtins do for lists.
   a = PySequence_List(self);
   if (!a) return NULL;
   b = PySequence_List(other);
   if (!b) goto done;
   na = PyList_GET_SIZE(a);
   nb = PyList_GET_SIZE(b);
   if ((op == Py_EQ || op == Py_NE) && na != nb) {
      result = PyBool_FromLong(op == Py_NE);
      goto done;
   }
   for (i = 0; i < na && i < nb; ++i) {
      int eq = PyObject_RichCompareBool(PyList_GET_ITEM(a, i),
                                        PyList_GET_ITEM(b, i), Py_EQ);
      if (eq < 0) goto done;
      if (!eq) break;
   }
   if (i >= na || i >= nb) {
      {
         int c = (na < nb) ? -1 : (na > nb) ? 1 : 0;
         switch (op) {
            case Py_LT: result = PyBool_FromLong(c < 0); break;
            case Py_LE: result = PyBool_FromLong(c <= 0); break;
            case Py_EQ: result = PyBool_FromLong(c == 0); break;
            case Py_NE: result = PyBool_FromLong(c != 0); break;
            case Py_GT: result = PyBool_FromLong(c > 0); break;
            default: result = PyBool_FromLong(c >= 0); break;
         }
      }
      goto done;
   }
   if (op == Py_EQ) {
      result = Py_False;
      Py_INCREF(result);
   } else if (op == Py_NE) {
      result = Py_True;
      Py_INCREF(result);
   } else {
      result = PyObject_RichCompare(PyList_GET_ITEM(a, i),
                                    PyList_GET_ITEM(b, i), op);
   }
done:
   Py_XDECREF(a);
   Py_XDECREF(b);
   return result;
}

static PyObject* plist_richcompare(PListObject* self, PyObject* other, int op) {
   return seq_richcompare((PyObject*)self, other, op);
}
static PyObject* tlist_richcompare(TListObject* self, PyObject* other, int op) {
   return seq_richcompare((PyObject*)self, other, op);
}


//=============================================================================
// Iterators.
// One iterator type for plist and one for tlist; they share an
// implementation (see seqiter_next_impl()).

typedef struct {
   PyObject_HEAD
   PyObject* owner;   // the plist/tlist being iterated; owned.
   TriePath path;
   int state;         // 0 = not yet started, 1 = active, 2 = exhausted.
   bool transient;    // whether owner is a tlist.
   Py_ssize_t index;  // the index of the next element (tlist only).
   uint64_t version;  // the owner's guard version at the last step.
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

// A plist iterator walks the trie. A tlist iterator behaves like a list
// iterator: it yields the element at its current index, whatever the list
// holds there now, and stops for good once the index reaches the end. It
// walks the trie while the list is unchanged, and finds its place again by
// index after any change. It raises RuntimeError if a modification is in
// progress.
static PyObject* seqiter_next_impl(SeqIterObject* self) {
   int ok;
   PyObject* val;
   if (self->state == 2) return NULL;
   if (!self->transient) {
      if (self->state == 0) {
         ok = fat_firstpath(((PListObject*)self->owner)->root, &self->path);
         self->state = 1;
      } else {
         ok = fat_nextpath(&self->path);
      }
   } else {
      TListObject* t = (TListObject*)self->owner;
      if (t->root == NULL || self->index >= t->length) {
         self->state = 2;
         return NULL;
      }
      if (tguard_read(&t->guard, (PyObject*)t, "iteration") < 0) return NULL;
      if (self->state == 0 || t->guard.version != self->version) {
         ok = fat_seekpath(t->root, t->start + (trieint_t)self->index,
                           &self->path);
         self->state = 1;
         self->version = t->guard.version;
      } else {
         ok = fat_nextpath(&self->path);
      }
      self->index += 1;
   }
   if (!ok) {
      self->state = 2;
      return NULL;
   }
   val = *(PyObject**)triepath_val(&self->path);
   Py_INCREF(val);
   return val;
}
static PyObject* seqiter_next(SeqIterObject* self) {
   PyObject* r;
   PCOLL_BEGIN_LOCK2((PyObject*)self, self->owner);
   r = seqiter_next_impl(self);
   PCOLL_END_LOCK2();
   return r;
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
   it->transient = !PyObject_TypeCheck(owner, ST(PListType));
   it->index = 0;
   it->version = 0;
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
// plist construction from an argument (defined here because it uses tlist):
//   type(arg) is the requested type -> arg
//   lazy collection                 -> copied by iteration
//   tlist (or subclass)             -> shares its frozen root
//   plist (or subclass)             -> shares its root
//   anything else                   -> copied by iteration
static PyObject* plist_new_dispatch(PyTypeObject* type, PyObject* arg) {
   // Storage is not shared with a lazy collection: reading from it computes
   // its values.
   if (Py_TYPE(arg) == type) {
      Py_INCREF(arg);
      return arg;
   }
   if (pcoll_holds_lazy(arg))
      return plist_from_iterable_astype(type, arg);
   if (PyObject_TypeCheck(arg, ST(TListType))) {
      TListObject* t = (TListObject*)arg;
      Trie_t root;
      trieint_t start = 0;
      Py_ssize_t length = 0;
      PCOLL_BEGIN_LOCK(arg);
      root = tlist_share(t);
      start = t->start;
      length = t->length;
      PCOLL_END_LOCK();
      if (!root) return NULL;
      if (length == 0) {
         fatnode_decref(root, pyobj_decref);
         return plist_type_empty(type);
      }
      return plist_wrap_astype(type, root, start, length);
   }
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
   pcoll_set_weaklistoffset(st->PListType, offsetof(PListObject, weaklist));
   pcoll_set_weaklistoffset(st->TListType, offsetof(TListObject, weaklist));
   if (pcoll_type_setattr(st->PListType, "__transient_type__",
                          (PyObject*)st->TListType) < 0 ||
       pcoll_type_setattr(st->TListType, "__persistent_type__",
                          (PyObject*)st->PListType) < 0)
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
