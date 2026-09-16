///////////////////////////////////////////////////////////////////////////////
// _c/lazy.c.h
// The `lazy` type, the lazy collections (ldict, tldict, llist, tllist), and
// the helpers unlazy/reprlazy/strlazy/holdlazy. LazyError and
// lazy_error_unwrap are shared with the pure-Python backend and live in
// pcollections/_lazybase.py, which also documents the lazy-value states.
//
// Lazy value states and threads
// -----------------------------
// `state` is atomic. Once a lazy value is ready (or failed), its value (or
// error) never changes, and the writer stores it before publishing the new
// state, so a reader that sees READY or FAILED can use the fields without a
// lock. This is the common case: requesting a computed value is one atomic
// load.
//
// A pending value is computed under `mutex`. Other threads that request it
// block on the mutex (with the GIL released, since the computing thread
// needs the GIL to finish) and find the result when they get the mutex. The
// computing thread records itself in `owner`, so that a computation that
// requests its own value raises LazyError instead of deadlocking.
//
// Lazy collections
// ----------------
// Reads from a lazy collection compute the lazy values they return (see
// _lazy.py). ldict/tldict/llist/tllist add no fields to pdict/tdict/plist/
// tlist, so their instances use those types' structs.

#if PY_VERSION_HEX < 0x03090000
#  include <frameobject.h>
#endif

#define LAZY_PENDING 0
#define LAZY_RUNNING 1
#define LAZY_READY 2
#define LAZY_FAILED 3


//=============================================================================
// The `lazy` type.

typedef struct {
   PyObject_HEAD
   PyObject* fn;           // pending or failed: the function;
   PyObject* args;         //    its positional arguments (a tuple);
   PyObject* kwargs;       //    and keyword arguments (a dict or NULL).
   PyObject* value;        // ready: the value.
   PyObject* error;        // failed: the exception the computation raised,
   PyObject* error_tb;     //    and its traceback at that time.
   PyObject* origin_code;  // the code object that created the lazy value,
   int origin_line;        //    and the line; NULL/0 if unknown or ready.
   PyObject* origin_stack; // the formatted creation stack, or NULL.
   PyObject* weaklist;
   pcoll_atomic_flag_t state;
   pcoll_atomic_u64_t owner; // the thread computing the value, or 0.
   pcoll_mutex_t mutex;
} LazyObject;

static int lazy_traverse(LazyObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->fn);
   Py_VISIT(self->args);
   Py_VISIT(self->kwargs);
   Py_VISIT(self->value);
   Py_VISIT(self->error);
   Py_VISIT(self->error_tb);
   Py_VISIT(self->origin_code);
   Py_VISIT(self->origin_stack);
   return 0;
}
static int lazy_clear(LazyObject* self) {
   Py_CLEAR(self->fn);
   Py_CLEAR(self->args);
   Py_CLEAR(self->kwargs);
   Py_CLEAR(self->value);
   Py_CLEAR(self->error);
   Py_CLEAR(self->error_tb);
   Py_CLEAR(self->origin_code);
   Py_CLEAR(self->origin_stack);
   return 0;
}
static void lazy_dealloc(LazyObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   PCOLL_CLEAR_WEAKREFS(self);
   PCOLL_MUTEX_DESTROY(&self->mutex);
   lazy_clear(self);
   tp->tp_free((PyObject*)self);
   Py_DECREF(tp);
}

// Allocates an instance of `type` in the given state, or returns NULL.
static LazyObject* lazy_alloc(PyTypeObject* type, int state) {
   LazyObject* self = (LazyObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   PCOLL_ATOMIC_INIT(&self->state, state);
   PCOLL_ATOMIC_U64_STORE(&self->owner, 0);
   if (PCOLL_MUTEX_INIT(&self->mutex) != 0) {
      PyErr_SetString(PyExc_RuntimeError,
                      "failed to initialize a lazy value's mutex");
      Py_DECREF(self);
      return NULL;
   }
   return self;
}

// Returns whether new lazy values should record their creation stack.
static int lazy_trace_enabled(void) {
   PyObject* flag = PyObject_GetAttr((PyObject*)ST(LazyType), ST(g_str_trace));
   int r;
   if (!flag) return -1;
   r = PyObject_IsTrue(flag);
   Py_DECREF(flag);
   return r;
}

static PyObject* lazy_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t nargs = PyTuple_GET_SIZE(args);
   PyObject* fn;
   LazyObject* self;
   PyFrameObject* frame;
   int trace;
   if (nargs < 1) {
      PyErr_SetString(PyExc_TypeError,
                      "lazy() missing required positional argument: 'fn'");
      return NULL;
   }
   fn = PyTuple_GET_ITEM(args, 0);
   if (!PyCallable_Check(fn)) {
      PyErr_Format(PyExc_TypeError,
                   "lazy(%R) must be given a callable function", fn);
      return NULL;
   }
   trace = lazy_trace_enabled();
   if (trace < 0) return NULL;
   self = lazy_alloc(type, LAZY_PENDING);
   if (!self) return NULL;
   Py_INCREF(fn);
   self->fn = fn;
   self->args = PyTuple_GetSlice(args, 1, nargs);
   if (!self->args) { Py_DECREF(self); return NULL; }
   if (kwds && PyDict_GET_SIZE(kwds) > 0) {
      self->kwargs = PyDict_Copy(kwds);
      if (!self->kwargs) { Py_DECREF(self); return NULL; }
   }
   // Record where the value was created (the innermost Python frame).
   frame = PyEval_GetFrame();
   if (frame) {
#if PY_VERSION_HEX >= 0x03090000
      self->origin_code = (PyObject*)PyFrame_GetCode(frame);
#else
      self->origin_code = (PyObject*)frame->f_code;
      Py_INCREF(self->origin_code);
#endif
      self->origin_line = PyFrame_GetLineNumber(frame);
      if (trace) {
         self->origin_stack = PyObject_CallFunctionObjArgs(
            ST(g_capture_stack), (PyObject*)frame, NULL);
         if (!self->origin_stack) { Py_DECREF(self); return NULL; }
      }
   }
   return (PyObject*)self;
}

// lazy._from_value(value): a computed lazy value (used for unpickling).
static PyObject* lazy_from_value(PyObject* cls, PyObject* value) {
   LazyObject* self;
   if (!PyType_Check(cls) ||
       !PyType_IsSubtype((PyTypeObject*)cls, ST(LazyType))) {
      PyErr_SetString(PyExc_TypeError, "expected a lazy type");
      return NULL;
   }
   self = lazy_alloc((PyTypeObject*)cls, LAZY_READY);
   if (!self) return NULL;
   Py_INCREF(value);
   self->value = value;
   return (PyObject*)self;
}

// Returns a new LazyError for this lazy value (see _lazybase.make_error),
// or NULL with an exception set. `kind` is "failed" or "recursive".
static PyObject* lazy_make_error(LazyObject* self, const char* kind) {
   PyObject* kwargs = self->kwargs ? self->kwargs : PyDict_New();
   PyObject* line = PyLong_FromLong(self->origin_line);
   PyObject* result = NULL;
   if (kwargs && line) {
      result = PyObject_CallFunction(
         ST(g_make_lazy_error), "sOOOOOOOO", kind,
         self->fn ? self->fn : Py_None,
         self->args ? self->args : Py_None,
         kwargs,
         self->origin_code ? self->origin_code : Py_None,
         line,
         self->origin_stack ? self->origin_stack : Py_None,
         self->error ? self->error : Py_None,
         self->error_tb ? self->error_tb : Py_None);
   }
   if (kwargs != self->kwargs) Py_XDECREF(kwargs);
   Py_XDECREF(line);
   return result;
}
static PyObject* lazy_raise(LazyObject* self, const char* kind) {
   PyObject* err = lazy_make_error(self, kind);
   if (err) {
      PyErr_SetObject((PyObject*)Py_TYPE(err), err);
      Py_DECREF(err);
   }
   return NULL;
}

// Takes the current exception (which must be set), normalized and with its
// traceback attached.
static PyObject* pcoll_take_exception(void) {
#if PY_VERSION_HEX >= 0x030C0000
   return PyErr_GetRaisedException();
#else
   PyObject* type, *value, *tb;
   PyErr_Fetch(&type, &value, &tb);
   PyErr_NormalizeException(&type, &value, &tb);
   if (tb) {
      PyException_SetTraceback(value, tb);
      Py_DECREF(tb);
   }
   Py_XDECREF(type);
   return value;
#endif
}

static PyObject* lazy_value(LazyObject* self) {
   PyObject* value = self->value;
   if (!value) {
      PyErr_SetString(PyExc_RuntimeError, "lazy value has been cleared");
      return NULL;
   }
   Py_INCREF(value);
   return value;
}

// Computes (if necessary) and returns the value; the body of lazy.__call__.
static PyObject* lazy_compute(LazyObject* self) {
   int state = PCOLL_ATOMIC_LOAD_ACQUIRE(&self->state);
   uint64_t me;
   PyObject* value;
   if (state == LAZY_READY) return lazy_value(self);
   if (state == LAZY_FAILED) return lazy_raise(self, "failed");
   me = (uint64_t)PyThread_get_thread_ident();
   if (state == LAZY_RUNNING && PCOLL_ATOMIC_U64_LOAD(&self->owner) == me)
      return lazy_raise(self, "recursive");
   // The thread holding the mutex needs the GIL to finish, so wait for the
   // mutex without it.
   Py_BEGIN_ALLOW_THREADS
   PCOLL_MUTEX_LOCK(&self->mutex);
   Py_END_ALLOW_THREADS
   state = PCOLL_ATOMIC_LOAD_ACQUIRE(&self->state);
   if (state != LAZY_PENDING) {
      PCOLL_MUTEX_UNLOCK(&self->mutex);
      return state == LAZY_READY ? lazy_value(self)
                                 : lazy_raise(self, "failed");
   }
   PCOLL_ATOMIC_U64_STORE(&self->owner, me);
   PCOLL_ATOMIC_STORE_RELEASE(&self->state, LAZY_RUNNING);
   value = PyObject_Call(self->fn, self->args, self->kwargs);
   if (value) {
      // Publish the value before the state (see the file comment), then
      // release what the computation needed.
      self->value = value;
      PCOLL_ATOMIC_U64_STORE(&self->owner, 0);
      PCOLL_ATOMIC_STORE_RELEASE(&self->state, LAZY_READY);
      PCOLL_MUTEX_UNLOCK(&self->mutex);
      Py_CLEAR(self->fn);
      Py_CLEAR(self->args);
      Py_CLEAR(self->kwargs);
      Py_CLEAR(self->origin_code);
      Py_CLEAR(self->origin_stack);
      Py_INCREF(value);
      return value;
   }
   PCOLL_ATOMIC_U64_STORE(&self->owner, 0);
   if (!PyErr_ExceptionMatches(PyExc_Exception)) {
      // KeyboardInterrupt and the like: leave the value pending.
      PCOLL_ATOMIC_STORE_RELEASE(&self->state, LAZY_PENDING);
      PCOLL_MUTEX_UNLOCK(&self->mutex);
      return NULL;
   }
   self->error = pcoll_take_exception();
   self->error_tb = PyException_GetTraceback(self->error);
   PCOLL_ATOMIC_STORE_RELEASE(&self->state, LAZY_FAILED);
   PCOLL_MUTEX_UNLOCK(&self->mutex);
   return lazy_raise(self, "failed");
}

static PyObject* lazy_call(LazyObject* self, PyObject* args, PyObject* kwds) {
   if ((args && PyTuple_GET_SIZE(args) > 0) ||
       (kwds && PyDict_GET_SIZE(kwds) > 0)) {
      PyErr_SetString(PyExc_TypeError,
                      "lazy objects take no arguments when called");
      return NULL;
   }
   return lazy_compute(self);
}

static PyObject* lazy_is_ready(LazyObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyBool_FromLong(
      PCOLL_ATOMIC_LOAD_ACQUIRE(&self->state) == LAZY_READY);
}

static const char* lazy_state_name(LazyObject* self) {
   switch (PCOLL_ATOMIC_LOAD_ACQUIRE(&self->state)) {
      case LAZY_READY: return "ready";
      case LAZY_FAILED: return "failed";
      default: return "waiting";
   }
}
static PyObject* lazy_repr(LazyObject* self) {
   return PyUnicode_FromFormat("lazy(<%zu>: %s)", (size_t)(uintptr_t)self,
                               lazy_state_name(self));
}
static PyObject* lazy_str(LazyObject* self) {
   return PyUnicode_FromFormat("lazy(<%zu>)", (size_t)(uintptr_t)self);
}

// Pickling computes the value; see the lazy docstring.
static PyObject* lazy_reduce(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   PyObject* value, *dict, *result;
   value = PyObject_CallFunctionObjArgs(self, NULL);
   if (!value) return NULL;
   dict = PyObject_GetAttrString(self, "__dict__");
   if (!dict) {
      if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
         Py_DECREF(value);
         return NULL;
      }
      PyErr_Clear();
   } else if (PyDict_Check(dict) && PyDict_GET_SIZE(dict) == 0) {
      Py_CLEAR(dict);
   }
   result = Py_BuildValue("O(OO)O", ST(g_ready_lazy), (PyObject*)Py_TYPE(self),
                          value, dict ? dict : Py_None);
   Py_DECREF(value);
   Py_XDECREF(dict);
   return result;
}

static PyMethodDef lazy_methods[] = {
   {"is_ready", (PyCFunction)lazy_is_ready, METH_NOARGS,
    "Returns True if the value has been computed, otherwise False."},
   {"_from_value", (PyCFunction)lazy_from_value, METH_O | METH_CLASS, NULL},
   {"__reduce__", (PyCFunction)lazy_reduce, METH_NOARGS, NULL},
   {NULL, NULL, 0, NULL}
};

static PyType_Slot lazy_slots[] = {
   {Py_tp_new, (void*)lazy_new},
   {Py_tp_dealloc, (void*)lazy_dealloc},
   {Py_tp_traverse, (void*)lazy_traverse},
   {Py_tp_clear, (void*)lazy_clear},
   {Py_tp_call, (void*)lazy_call},
   {Py_tp_repr, (void*)lazy_repr},
   {Py_tp_str, (void*)lazy_str},
   {Py_tp_methods, (void*)lazy_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A value computed on first request, like a partial with no free\n"
      "arguments.\n\n"
      "lazy(fn, *args, **kwargs)() computes fn(*args, **kwargs) once, caches\n"
      "the result, and releases fn and its arguments. See the pure-Python\n"
      "pcollections._lazy.lazy for the full description.")},
   {0, NULL}
};
static PyType_Spec lazy_spec = {
   .name = "pcollections.lazy",
   .basicsize = sizeof(LazyObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = lazy_slots,
};


//=============================================================================
// Shared helpers for the lazy collections.

// Calls an unbound base-class method: `unbound_func(self, *args)`.
static PyObject* call_unbound(PyObject* unbound_func, PyObject* self, PyObject* args) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* full = PyTuple_New(n + 1);
   Py_ssize_t i;
   PyObject* result;
   if (!full) return NULL;
   Py_INCREF(self);
   PyTuple_SET_ITEM(full, 0, self);
   for (i = 0; i < n; ++i) {
      PyObject* item = PyTuple_GET_ITEM(args, i);
      Py_INCREF(item);
      PyTuple_SET_ITEM(full, i + 1, item);
   }
   result = PyObject_Call(unbound_func, full, NULL);
   Py_DECREF(full);
   return result;
}

static int is_lazy_obj(PyObject* v) {
   return PyObject_TypeCheck(v, ST(LazyType));
}

// unlazy(v), consuming the reference to `v` (which may be NULL, in which
// case NULL is returned). A plain `lazy` is computed directly; a subclass is
// called, so that an overridden __call__ runs.
static PyObject* unlazy_owned(PyObject* v) {
   PyObject* result;
   if (!v) return NULL;
   if (Py_TYPE(v) == ST(LazyType))
      result = lazy_compute((LazyObject*)v);
   else if (is_lazy_obj(v))
      result = PyObject_CallFunctionObjArgs(v, NULL);
   else
      return v;
   Py_DECREF(v);
   return result;
}

// Computes `v` if it is a lazy value; returns 0, or -1 on error.
static int lazy_ready_one(PyObject* v) {
   PyObject* r;
   if (!is_lazy_obj(v)) return 0;
   Py_INCREF(v);
   r = unlazy_owned(v);
   if (!r) return -1;
   Py_DECREF(r);
   return 0;
}

// holdlazy(obj): obj.__holdlazy__() if obj's type defines it, else obj.
// Returns a new reference; *found (if not NULL) is set to whether the
// method was found.
static PyObject* holdlazy_find(PyObject* obj, int* found) {
   PyObject* method = PyObject_GetAttrString((PyObject*)Py_TYPE(obj),
                                             "__holdlazy__");
   PyObject* result;
   if (!method) {
      if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;
      PyErr_Clear();
      if (found) *found = 0;
      Py_INCREF(obj);
      return obj;
   }
   if (found) *found = 1;
   result = PyObject_CallFunctionObjArgs(method, obj, NULL);
   Py_DECREF(method);
   return result;
}
static PyObject* holdlazy_obj(PyObject* obj) {
   return holdlazy_find(obj, NULL);
}
// Maps holdlazy_obj over the tuple `args`.
static PyObject* holdlazy_map_args(PyObject* args) {
   Py_ssize_t n = PyTuple_GET_SIZE(args);
   PyObject* result = PyTuple_New(n);
   Py_ssize_t i;
   if (!result) return NULL;
   for (i = 0; i < n; ++i) {
      PyObject* held = holdlazy_obj(PyTuple_GET_ITEM(args, i));
      if (!held) { Py_DECREF(result); return NULL; }
      PyTuple_SET_ITEM(result, i, held);
   }
   return result;
}

// Whether `obj` is a lazy collection, whose storage must not be shared with
// a non-lazy collection (see dict.c.h and list.c.h).
static int pcoll_holds_lazy(PyObject* obj) {
   pcoll_state* st = pcoll_get_state();
   return ((st->LDictType && PyObject_TypeCheck(obj, st->LDictType)) ||
           (st->TLDictType && PyObject_TypeCheck(obj, st->TLDictType)) ||
           (st->LListType && PyObject_TypeCheck(obj, st->LListType)) ||
           (st->TLListType && PyObject_TypeCheck(obj, st->TLListType)));
}

// Builds a lazy collection type on top of `base`. PyType_FromSpecWithBases
// does not carry tp_richcompare over from a heap base whose comparison came
// from a Python mixin, so it is copied explicitly.
static PyTypeObject* build_c_subtype(PyObject* m, PyType_Spec* spec, PyTypeObject* base) {
   PyObject* bases = PyTuple_Pack(1, (PyObject*)base);
   PyTypeObject* result;
   if (!bases) return NULL;
   result = pcoll_new_type(m, spec, bases);
   Py_DECREF(bases);
   if (!result) return NULL;
   if (!result->tp_richcompare) result->tp_richcompare = base->tp_richcompare;
   return result;
}

// is_lazy/is_ready for a raw (possibly lazy) element `v`, which is
// consumed; NULL is passed through.
static PyObject* lazy_is_lazy_result(PyObject* v) {
   int r;
   if (!v) return NULL;
   r = is_lazy_obj(v);
   Py_DECREF(v);
   return PyBool_FromLong(r);
}
static PyObject* lazy_is_ready_result(PyObject* v) {
   PyObject* result;
   if (!v) return NULL;
   if (Py_TYPE(v) == ST(LazyType))
      result = lazy_is_ready((LazyObject*)v, NULL);
   else if (is_lazy_obj(v))
      result = PyObject_CallMethod(v, "is_ready", NULL);
   else
      result = PyBool_FromLong(1);
   Py_DECREF(v);
   return result;
}
// Computes every lazy value produced by iterating over `raw` (consumed; the
// raw values of a collection), then returns a new reference to `self`.
static PyObject* lazy_ready_all_from(PyObject* self, PyObject* raw) {
   PyObject* items;
   Py_ssize_t i, n;
   if (!raw) return NULL;
   // Take a snapshot first: computing a value may change a transient.
   items = PySequence_List(raw);
   Py_DECREF(raw);
   if (!items) return NULL;
   n = PyList_GET_SIZE(items);
   for (i = 0; i < n; ++i) {
      if (lazy_ready_one(PyList_GET_ITEM(items, i)) < 0) {
         Py_DECREF(items);
         return NULL;
      }
   }
   Py_DECREF(items);
   Py_INCREF(self);
   return self;
}
// Formats `raw` (a plain collection holding the raw values, consumed) with
// seqstr and reprlazy, between `open` and `close`.
static PyObject* lazy_seq_str(PyObject* raw, const char* open,
                              const char* close, int truncate) {
   PyObject* s, *result;
   if (!raw) return NULL;
   s = call_seqstr(raw, truncate ? 60 : 0, truncate, ST(g_reprlazy_func));
   Py_DECREF(raw);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("%s%U%s", open, s, close);
   Py_DECREF(s);
   return result;
}


//=============================================================================
// ldict and tldict.

// A pdict-layout instance of `type` sharing src's (immutable) tries.
static PyObject* pdictlike_share(PyTypeObject* type, PDictObject* src) {
   PDictObject* self = (PDictObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   trienode_incref(src->els);
   trienode_incref(src->idx);
   self->els = src->els;
   self->idx = src->idx;
   self->top = src->top;
   self->count = src->count;
   self->ndeleted = src->ndeleted;
   self->hashcode = -1;
   return (PyObject*)self;
}

static PyObject* ldict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = ST(PDictType)->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
static PyObject* ldict_raw_item(PyObject* self, PyObject* key) {
   return ST(PDictType)->tp_as_mapping->mp_subscript(self, key);
}
static PyObject* ldict_subscript(PyObject* self, PyObject* key) {
   return unlazy_owned(ldict_raw_item(self, key));
}
static PyObject* ldict_get(PyObject* self, PyObject* args) {
   return unlazy_owned(pdict_get((PDictObject*)self, args));
}
static PyObject* ldict_getlazy(PyObject* self, PyObject* args) {
   return pdict_get((PDictObject*)self, args);
}
// items()/values(): views (pcollections.abc._view) that read through
// self[key], which computes the values.
static PyObject* ldict_items(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs(ST(g_ItemsView), self, NULL);
}
static PyObject* ldict_values(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs(ST(g_ValuesView), self, NULL);
}
static PyObject* ldict_is_lazy(PyObject* self, PyObject* key) {
   return lazy_is_lazy_result(ldict_raw_item(self, key));
}
static PyObject* ldict_is_ready(PyObject* self, PyObject* key) {
   return lazy_is_ready_result(ldict_raw_item(self, key));
}
static PyObject* ldict_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return lazy_ready_all_from(
      self, PyObject_CallFunctionObjArgs((PyObject*)ST(PDictValuesType),
                                         self, NULL));
}
static PyObject* ldict_held_pdict(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return pdictlike_share(ST(PDictType), (PDictObject*)self);
}
// The lazy collections' specs need their own GC slots; ldict's are pdict's.
static int ldict_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return pdict_traverse((PDictObject*)self, visit, arg);
}
static int ldict_gc_clear(PyObject* self) {
   return pdict_clear((PDictObject*)self);
}
static PyObject* ldict_repr(PyObject* self) {
   return lazy_seq_str(ldict_held_pdict(self, NULL), "{|", "|}", 0);
}
static PyObject* ldict_str(PyObject* self) {
   return lazy_seq_str(ldict_held_pdict(self, NULL), "{|", "|}", 1);
}
// Hashing reads (and so computes) the values.
static Py_hash_t ldict_hash(PDictObject* self) {
   PyObject *items, *fs;
   Py_hash_t result = PCOLL_HASH_LOAD(self->hashcode);
   if (result != -1) return result;
   items = ldict_items((PyObject*)self, NULL);
   if (!items) return -1;
   fs = PyFrozenSet_New(items);
   Py_DECREF(items);
   if (!fs) return -1;
   result = PyObject_Hash(fs);
   Py_DECREF(fs);
   if (result == -1) return -1;
   result += 2; // pdict_hash's offset.
   if (result == -1) result = -2;
   PCOLL_HASH_STORE(self->hashcode, result);
   return result;
}

static PyMethodDef ldict_methods[] = {
   {"get", (PyCFunction)ldict_get, METH_VARARGS,
    "Returns the (computed) value for key, or default if key is absent."},
   {"items", (PyCFunction)ldict_items, METH_NOARGS,
    "Returns a view of the items, with lazy values computed."},
   {"values", (PyCFunction)ldict_values, METH_NOARGS,
    "Returns a view of the values, with lazy values computed."},
   {"is_lazy", (PyCFunction)ldict_is_lazy, METH_O,
    "Returns True if key is mapped to a lazy object."},
   {"is_ready", (PyCFunction)ldict_is_ready, METH_O,
    "Returns True if key is mapped to a non-lazy value or a computed lazy value."},
   {"ready_all", (PyCFunction)ldict_ready_all, METH_NOARGS,
    "Computes all lazy values, then returns the dict."},
   {"held_pdict", (PyCFunction)ldict_held_pdict, METH_NOARGS,
    "Returns a pdict of the dict's items with lazy values left uncomputed."},
   {"__holdlazy__", (PyCFunction)ldict_held_pdict, METH_NOARGS, NULL},
   {"getlazy", (PyCFunction)ldict_getlazy, METH_VARARGS,
    "Like get(), but returns a lazy value itself rather than its value."},
   {NULL, NULL, 0, NULL}
};
static PyType_Slot ldict_slots[] = {
   {Py_tp_new, (void*)ldict_new},
   {Py_tp_traverse, (void*)ldict_gc_traverse},
   {Py_tp_clear, (void*)ldict_gc_clear},
   {Py_mp_subscript, (void*)ldict_subscript},
   {Py_tp_hash, (void*)ldict_hash},
   {Py_tp_repr, (void*)ldict_repr},
   {Py_tp_str, (void*)ldict_str},
   {Py_tp_methods, (void*)ldict_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A persistent dict whose lazy values are computed when read.")},
   {0, NULL}
};
static PyType_Spec ldict_spec = {
   .name = "pcollections.ldict",
   .basicsize = sizeof(PDictObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = ldict_slots,
};

static PyObject* tldict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = ST(TDictType)->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
static PyObject* tldict_raw_item(PyObject* self, PyObject* key) {
   return ST(TDictType)->tp_as_mapping->mp_subscript(self, key);
}
static PyObject* tldict_subscript(PyObject* self, PyObject* key) {
   return unlazy_owned(tldict_raw_item(self, key));
}
static PyObject* tldict_get(PyObject* self, PyObject* args) {
   return unlazy_owned(tdict_get((TDictObject*)self, args));
}
static PyObject* tldict_getlazy(PyObject* self, PyObject* args) {
   return tdict_get((TDictObject*)self, args);
}
static PyObject* tldict_pop(PyObject* self, PyObject* args) {
   return unlazy_owned(call_unbound(ST(g_tdict_pop), self, args));
}
static PyObject* tldict_is_lazy(PyObject* self, PyObject* key) {
   return lazy_is_lazy_result(tldict_raw_item(self, key));
}
static PyObject* tldict_is_ready(PyObject* self, PyObject* key) {
   return lazy_is_ready_result(tldict_raw_item(self, key));
}
static PyObject* tldict_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return lazy_ready_all_from(
      self, PyObject_CallFunctionObjArgs((PyObject*)ST(TDictValuesType),
                                         self, NULL));
}
// A plain tdict holding a frozen snapshot of the tldict's tries.
static PyObject* tldict_held_tdict_impl(TDictObject* self) {
   Trie_t els, idx;
   if (tdict_share(self, &els, &idx) < 0) return NULL;
   return tdict_wrap(els, idx, self->top, self->count, self->ndeleted, NULL);
}
PCOLL_LOCKED0(PyObject*, tldict_held_tdict_locked, tldict_held_tdict_impl,
              TDictObject*)
static PyObject* tldict_held_tdict(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return tldict_held_tdict_locked((TDictObject*)self);
}
static PyObject* tldict_repr(PyObject* self) {
   return lazy_seq_str(tldict_held_tdict(self, NULL), "{<", ">}", 0);
}
static PyObject* tldict_str(PyObject* self) {
   return lazy_seq_str(tldict_held_tdict(self, NULL), "{<", ">}", 1);
}
static int tldict_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return tdict_traverse((TDictObject*)self, visit, arg);
}
static int tldict_gc_clear(PyObject* self) {
   return tdict_clear((TDictObject*)self);
}

static PyMethodDef tldict_methods[] = {
   {"get", (PyCFunction)tldict_get, METH_VARARGS,
    "Returns the (computed) value for key, or default if key is absent."},
   {"getlazy", (PyCFunction)tldict_getlazy, METH_VARARGS,
    "Like get(), but returns a lazy value itself rather than its value."},
   {"pop", (PyCFunction)tldict_pop, METH_VARARGS,
    "Removes key and returns its (computed) value."},
   {"items", (PyCFunction)ldict_items, METH_NOARGS,
    "Returns a view of the items, with lazy values computed."},
   {"values", (PyCFunction)ldict_values, METH_NOARGS,
    "Returns a view of the values, with lazy values computed."},
   {"is_lazy", (PyCFunction)tldict_is_lazy, METH_O,
    "Returns True if key is mapped to a lazy object."},
   {"is_ready", (PyCFunction)tldict_is_ready, METH_O,
    "Returns True if key is mapped to a non-lazy value or a computed lazy value."},
   {"ready_all", (PyCFunction)tldict_ready_all, METH_NOARGS,
    "Computes all lazy values, then returns the dict."},
   {"held_tdict", (PyCFunction)tldict_held_tdict, METH_NOARGS,
    "Returns a tdict of the dict's items with lazy values left uncomputed."},
   {"__holdlazy__", (PyCFunction)tldict_held_tdict, METH_NOARGS, NULL},
   {NULL, NULL, 0, NULL}
};
static PyType_Slot tldict_slots[] = {
   {Py_tp_new, (void*)tldict_new},
   {Py_tp_traverse, (void*)tldict_gc_traverse},
   {Py_tp_clear, (void*)tldict_gc_clear},
   {Py_mp_subscript, (void*)tldict_subscript},
   {Py_tp_repr, (void*)tldict_repr},
   {Py_tp_str, (void*)tldict_str},
   {Py_tp_methods, (void*)tldict_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient dict whose lazy values are computed when read.")},
   {0, NULL}
};
static PyType_Spec tldict_spec = {
   .name = "pcollections.tldict",
   .basicsize = sizeof(TDictObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tldict_slots,
};


//=============================================================================
// llist and tllist.

// An iterator that computes the lazy values of another iterator.
typedef struct {
   PyObject_HEAD
   PyObject* inner;
} UnlazyIterObject;
static void unlazyiter_dealloc(UnlazyIterObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   PyObject_GC_UnTrack(self);
   Py_CLEAR(self->inner);
   PyObject_GC_Del(self);
   Py_DECREF(tp);
}
static int unlazyiter_traverse(UnlazyIterObject* self, visitproc visit, void* arg) {
   PCOLL_VISIT_TYPE(self);
   Py_VISIT(self->inner);
   return 0;
}
static int unlazyiter_clear(UnlazyIterObject* self) {
   Py_CLEAR(self->inner);
   return 0;
}
static PyObject* unlazyiter_next(UnlazyIterObject* self) {
   if (!self->inner) return NULL;
   return unlazy_owned(PyIter_Next(self->inner));
}
static PyObject* unlazyiter_self(PyObject* self) { Py_INCREF(self); return self; }
static PyType_Slot unlazyiter_slots[] = {
   {Py_tp_dealloc, (void*)unlazyiter_dealloc},
   {Py_tp_traverse, (void*)unlazyiter_traverse},
   {Py_tp_clear, (void*)unlazyiter_clear},
   {Py_tp_iter, (void*)unlazyiter_self},
   {Py_tp_iternext, (void*)unlazyiter_next},
   {0, NULL}
};
static PyType_Spec unlazyiter_spec = {
   .name = "pcollections._c._core._unlazy_iterator",
   .basicsize = sizeof(UnlazyIterObject),
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | PCOLL_TPFLAGS_INTERNAL,
   .slots = unlazyiter_slots,
};
// Wraps `inner` (consumed; NULL is passed through).
static PyObject* make_unlazy_iter(PyObject* inner) {
   UnlazyIterObject* it;
   if (!inner) return NULL;
   it = PyObject_GC_New(UnlazyIterObject, ST(UnlazyIterType));
   if (!it) { Py_DECREF(inner); return NULL; }
   it->inner = inner;
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

// A plist-layout instance of `type` sharing src's (immutable) trie.
static PyObject* plistlike_share(PyTypeObject* type, PListObject* src) {
   PListObject* self = (PListObject*)type->tp_alloc(type, 0);
   if (!self) return NULL;
   trienode_incref(src->root);
   self->root = src->root;
   self->start = src->start;
   self->length = src->length;
   self->hashcode = -1;
   return (PyObject*)self;
}

// Returns the raw element at the index `index_obj`.
static PyObject* lazy_list_raw_at(PyTypeObject* base, PyObject* self,
                                  PyObject* index_obj) {
   Py_ssize_t i = PyNumber_AsSsize_t(index_obj, PyExc_IndexError);
   if (i == -1 && PyErr_Occurred()) return NULL;
   if (i < 0) {
      Py_ssize_t n = PyObject_Length(self);
      if (n < 0) return NULL;
      i += n;
   }
   return base->tp_as_sequence->sq_item(self, i);
}

static PyObject* llist_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = ST(PListType)->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
// A slice of an llist is an llist (raw), so only an element is computed.
static PyObject* llist_subscript(PyObject* self, PyObject* key) {
   return unlazy_owned(ST(PListType)->tp_as_mapping->mp_subscript(self, key));
}
static PyObject* llist_item(PyObject* self, Py_ssize_t i) {
   return unlazy_owned(ST(PListType)->tp_as_sequence->sq_item(self, i));
}
static PyObject* llist_iter(PyObject* self) {
   return make_unlazy_iter(ST(PListType)->tp_iter(self));
}
static PyObject* llist_is_lazy(PyObject* self, PyObject* index) {
   return lazy_is_lazy_result(lazy_list_raw_at(ST(PListType), self, index));
}
static PyObject* llist_is_ready(PyObject* self, PyObject* index) {
   return lazy_is_ready_result(lazy_list_raw_at(ST(PListType), self, index));
}
static PyObject* llist_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return lazy_ready_all_from(self, ST(PListType)->tp_iter(self));
}
static PyObject* llist_held_plist(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return plistlike_share(ST(PListType), (PListObject*)self);
}
static PyObject* llist_getlazy(PyObject* self, PyObject* index) {
   return ST(PListType)->tp_as_mapping->mp_subscript(self, index);
}
static int llist_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return plist_traverse((PListObject*)self, visit, arg);
}
static int llist_gc_clear(PyObject* self) {
   return plist_clear((PListObject*)self);
}
static PyObject* llist_repr(PyObject* self) {
   return lazy_seq_str(llist_held_plist(self, NULL), "[|", "|]", 0);
}
static PyObject* llist_str(PyObject* self) {
   return lazy_seq_str(llist_held_plist(self, NULL), "[|", "|]", 1);
}
// Hashing reads (and so computes) the elements.
static Py_hash_t llist_hash(PListObject* self) {
   PyObject* tup;
   Py_hash_t result = PCOLL_HASH_LOAD(self->hashcode);
   if (result != -1) return result;
   tup = PySequence_Tuple((PyObject*)self);
   if (!tup) return -1;
   result = PyObject_Hash(tup);
   Py_DECREF(tup);
   if (result == -1) return -1;
   result += 1; // plist_hash's offset.
   if (result == -1) result = -2;
   PCOLL_HASH_STORE(self->hashcode, result);
   return result;
}

static PyMethodDef llist_methods[] = {
   {"is_lazy", (PyCFunction)llist_is_lazy, METH_O,
    "Returns True if the element at index is a lazy object."},
   {"is_ready", (PyCFunction)llist_is_ready, METH_O,
    "Returns True if the element at index is not lazy or has been computed."},
   {"ready_all", (PyCFunction)llist_ready_all, METH_NOARGS,
    "Computes all lazy elements, then returns the list."},
   {"held_plist", (PyCFunction)llist_held_plist, METH_NOARGS,
    "Returns a plist of the list's elements with lazy elements left uncomputed."},
   {"__holdlazy__", (PyCFunction)llist_held_plist, METH_NOARGS, NULL},
   {"getlazy", (PyCFunction)llist_getlazy, METH_O,
    "Like self[index], but returns a lazy element itself rather than its value."},
   {NULL, NULL, 0, NULL}
};
static PyType_Slot llist_slots[] = {
   {Py_tp_new, (void*)llist_new},
   {Py_tp_traverse, (void*)llist_gc_traverse},
   {Py_tp_clear, (void*)llist_gc_clear},
   {Py_mp_subscript, (void*)llist_subscript},
   {Py_sq_item, (void*)llist_item},
   {Py_tp_iter, (void*)llist_iter},
   {Py_tp_hash, (void*)llist_hash},
   {Py_tp_repr, (void*)llist_repr},
   {Py_tp_str, (void*)llist_str},
   {Py_tp_methods, (void*)llist_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A persistent list whose lazy elements are computed when read.")},
   {0, NULL}
};
static PyType_Spec llist_spec = {
   .name = "pcollections.llist",
   .basicsize = sizeof(PListObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = llist_slots,
};

static PyObject* tllist_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = ST(TListType)->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
static PyObject* tllist_subscript(PyObject* self, PyObject* key) {
   return unlazy_owned(ST(TListType)->tp_as_mapping->mp_subscript(self, key));
}
static PyObject* tllist_item(PyObject* self, Py_ssize_t i) {
   return unlazy_owned(ST(TListType)->tp_as_sequence->sq_item(self, i));
}
static PyObject* tllist_iter(PyObject* self) {
   return make_unlazy_iter(ST(TListType)->tp_iter(self));
}
static PyObject* tllist_pop(PyObject* self, PyObject* args) {
   return unlazy_owned(tlist_pop((TListObject*)self, args));
}
static PyObject* tllist_getlazy(PyObject* self, PyObject* index) {
   return ST(TListType)->tp_as_mapping->mp_subscript(self, index);
}
static PyObject* tllist_is_lazy(PyObject* self, PyObject* index) {
   return lazy_is_lazy_result(lazy_list_raw_at(ST(TListType), self, index));
}
static PyObject* tllist_is_ready(PyObject* self, PyObject* index) {
   return lazy_is_ready_result(lazy_list_raw_at(ST(TListType), self, index));
}
static PyObject* tllist_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return lazy_ready_all_from(self, ST(TListType)->tp_iter(self));
}
// A plain tlist holding a frozen snapshot of the tllist's trie.
static PyObject* tllist_held_tlist_impl(TListObject* self) {
   Trie_t root = tlist_share(self);
   if (!root) return NULL;
   return tlist_wrap(root, self->start, self->length, NULL);
}
PCOLL_LOCKED0(PyObject*, tllist_held_tlist_locked, tllist_held_tlist_impl,
              TListObject*)
static PyObject* tllist_held_tlist(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return tllist_held_tlist_locked((TListObject*)self);
}
static PyObject* tllist_repr(PyObject* self) {
   return lazy_seq_str(tllist_held_tlist(self, NULL), "[<", ">]", 0);
}
static PyObject* tllist_str(PyObject* self) {
   return lazy_seq_str(tllist_held_tlist(self, NULL), "[<", ">]", 1);
}
static int tllist_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return tlist_traverse((TListObject*)self, visit, arg);
}
static int tllist_gc_clear(PyObject* self) {
   return tlist_clear((TListObject*)self);
}
static PyMethodDef tllist_methods[] = {
   {"getlazy", (PyCFunction)tllist_getlazy, METH_O,
    "Like self[index], but returns a lazy element itself rather than its value."},
   {"pop", (PyCFunction)tllist_pop, METH_VARARGS,
    "Removes and returns the (computed) element at index (default last)."},
   {"is_lazy", (PyCFunction)tllist_is_lazy, METH_O,
    "Returns True if the element at index is a lazy object."},
   {"is_ready", (PyCFunction)tllist_is_ready, METH_O,
    "Returns True if the element at index is not lazy or has been computed."},
   {"ready_all", (PyCFunction)tllist_ready_all, METH_NOARGS,
    "Computes all lazy elements, then returns the list."},
   {"held_tlist", (PyCFunction)tllist_held_tlist, METH_NOARGS,
    "Returns a tlist of the list's elements with lazy elements left uncomputed."},
   {"__holdlazy__", (PyCFunction)tllist_held_tlist, METH_NOARGS, NULL},
   {NULL, NULL, 0, NULL}
};
static PyType_Slot tllist_slots[] = {
   {Py_tp_new, (void*)tllist_new},
   {Py_tp_traverse, (void*)tllist_gc_traverse},
   {Py_tp_clear, (void*)tllist_gc_clear},
   {Py_mp_subscript, (void*)tllist_subscript},
   {Py_sq_item, (void*)tllist_item},
   {Py_tp_iter, (void*)tllist_iter},
   {Py_tp_repr, (void*)tllist_repr},
   {Py_tp_str, (void*)tllist_str},
   {Py_tp_methods, (void*)tllist_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient list whose lazy elements are computed when read.")},
   {0, NULL}
};
static PyType_Spec tllist_spec = {
   .name = "pcollections.tllist",
   .basicsize = sizeof(TListObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
   .slots = tllist_slots,
};


//=============================================================================
// unlazy / reprlazy / strlazy / holdlazy.

static PyObject* mod_unlazy(PyObject* self, PyObject* obj) {
   (void)self;
   Py_INCREF(obj);
   return unlazy_owned(obj);
}
static PyObject* mod_reprlazy(PyObject* self, PyObject* obj) {
   (void)self;
   if (is_lazy_obj(obj)) return PyUnicode_FromString("<lazy>");
   return PyObject_Repr(obj);
}
// A standalone callable, passed to seqstr as `tostr`.
static PyMethodDef reprlazy_methoddef = {
   "reprlazy", (PyCFunction)mod_reprlazy, METH_O,
   "Returns '<lazy>' if obj is a lazy object, otherwise repr(obj)."};
static PyObject* mod_strlazy(PyObject* self, PyObject* obj) {
   (void)self;
   if (is_lazy_obj(obj)) return PyUnicode_FromString("<lazy>");
   return PyObject_Str(obj);
}
static PyObject* mod_holdlazy(PyObject* self, PyObject* args, PyObject* kwds) {
   PyObject* obj;
   PyObject* result;
   int require_lazy = 0;
   int found = 0;
   static char* kwlist[] = {"obj", "require_lazy", NULL};
   (void)self;
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p", kwlist,
                                    &obj, &require_lazy))
      return NULL;
   result = holdlazy_find(obj, &found);
   if (result && !found && require_lazy) {
      Py_DECREF(result);
      PyErr_Format(PyExc_TypeError,
                   "__holdlazy__ method not found for type %R",
                   (PyObject*)Py_TYPE(obj));
      return NULL;
   }
   return result;
}


//=============================================================================
// Module execution.

// Creates lazy and the lazy collections, and adds them (with LazyError and
// lazy_error_unwrap) to module `m`. Must run after pcoll_exec_dict/
// pcoll_exec_list.
static int pcoll_exec_lazy(PyObject* m, pcoll_state* st) {
   PyObject* base = NULL;
   PyObject* coll_abc = NULL;
   PyObject* trace = NULL;
   PyObject* empty = NULL;
   int rc = -1;

   base = PyImport_ImportModule("pcollections._lazybase");
   if (!base) goto done;
   if (!(st->LazyErrorType = (PyTypeObject*)PyObject_GetAttrString(base, "LazyError")) ||
       !(st->g_lazy_error_unwrap = PyObject_GetAttrString(base, "lazy_error_unwrap")) ||
       !(st->g_make_lazy_error = PyObject_GetAttrString(base, "make_error")) ||
       !(st->g_capture_stack = PyObject_GetAttrString(base, "capture_stack")) ||
       !(st->g_ready_lazy = PyObject_GetAttrString(base, "ready_lazy")) ||
       !(trace = PyObject_GetAttrString(base, "TRACE_DEFAULT")) ||
       !(st->g_str_trace = PyUnicode_InternFromString("trace")))
      goto done;

   st->LazyType = pcoll_new_type(m, &lazy_spec, NULL);
   if (!st->LazyType) goto done;
   pcoll_set_weaklistoffset(st->LazyType, offsetof(LazyObject, weaklist));
   if (PyObject_SetAttr((PyObject*)st->LazyType, st->g_str_trace, trace) < 0)
      goto done;
   st->UnlazyIterType = pcoll_new_internal_type(m, &unlazyiter_spec);
   if (!st->UnlazyIterType) goto done;

   coll_abc = PyImport_ImportModule("pcollections.abc._view");
   if (!coll_abc) goto done;
   st->g_ItemsView = PyObject_GetAttrString(coll_abc, "ldict_items");
   if (!st->g_ItemsView) goto done;
   st->g_ValuesView = PyObject_GetAttrString(coll_abc, "ldict_values");
   if (!st->g_ValuesView) goto done;
   st->g_tdict_pop = PyObject_GetAttrString((PyObject*)st->TDictType, "pop");
   if (!st->g_tdict_pop) goto done;

   st->g_reprlazy_func = PyCFunction_NewEx(&reprlazy_methoddef, NULL, NULL);
   if (!st->g_reprlazy_func) goto done;

   if (!(st->LDictType = build_c_subtype(m, &ldict_spec, st->PDictType)) ||
       !(st->TLDictType = build_c_subtype(m, &tldict_spec, st->TDictType)) ||
       !(st->LListType = build_c_subtype(m, &llist_spec, st->PListType)) ||
       !(st->TLListType = build_c_subtype(m, &tllist_spec, st->TListType)))
      goto done;

   if (pcoll_type_setattr(st->LDictType, "__transient_type__",
                          (PyObject*)st->TLDictType) < 0 ||
       pcoll_type_setattr(st->TLDictType, "__persistent_type__",
                          (PyObject*)st->LDictType) < 0 ||
       pcoll_type_setattr(st->LListType, "__transient_type__",
                          (PyObject*)st->TLListType) < 0 ||
       pcoll_type_setattr(st->TLListType, "__persistent_type__",
                          (PyObject*)st->LListType) < 0)
      goto done;
   empty = pdictlike_share(st->LDictType, st->g_pdict_empty);
   if (!empty) goto done;
   st->g_ldict_empty = (PDictObject*)empty;
   if (pcoll_type_setattr(st->LDictType, "empty", empty) < 0) goto done;
   empty = plistlike_share(st->LListType, st->g_plist_empty);
   if (!empty) goto done;
   st->g_llist_empty = (PListObject*)empty;
   if (pcoll_type_setattr(st->LListType, "empty", empty) < 0) goto done;

   if (pcoll_module_add(m, "lazy", (PyObject*)st->LazyType) < 0 ||
       pcoll_module_add(m, "LazyError", (PyObject*)st->LazyErrorType) < 0 ||
       pcoll_module_add(m, "lazy_error_unwrap", st->g_lazy_error_unwrap) < 0 ||
       pcoll_module_add(m, "ldict", (PyObject*)st->LDictType) < 0 ||
       pcoll_module_add(m, "tldict", (PyObject*)st->TLDictType) < 0 ||
       pcoll_module_add(m, "llist", (PyObject*)st->LListType) < 0 ||
       pcoll_module_add(m, "tllist", (PyObject*)st->TLListType) < 0)
      goto done;
   rc = 0;
done:
   Py_XDECREF(base);
   Py_XDECREF(coll_abc);
   Py_XDECREF(trace);
   return rc;
}
