///////////////////////////////////////////////////////////////////////////////
// _c/lazy.c
// pcollections._c.lazy: the `lazy` value-cell type, its supporting
// `LazyError` exception and `lazy_error_unwrap` helper, and the small
// dereferencing utilities (unlazy/reprlazy/strlazy/holdlazy) that the lazy
// collection types (ldict/tldict/llist/tllist -- built on top of this module)
// use throughout.
//
// This file follows pcollections/_lazy.py (the reference Python
// implementation) as its interface spec: same names, same semantics.
//
// ---------------------------------------------------------------------------
// Thread-safety design (this is the one part of the whole pcollections._c
// rewrite the user explicitly called out as needing to work *without* the
// GIL held during the interesting part -- anticipating a future
// free-threaded/no-GIL Python build):
//
// A `lazy` cell has exactly two states, "pending" (not yet computed) and
// "ready" (computed and cached). We track this with an explicit
// `atomic_int ready` flag rather than inferring it from, say, `value != NULL`
// the way the Python reference infers readiness from `self.partial is None`
// -- an explicit flag lets us pick the memory-ordering guarantees we need
// rather than relying on the GIL to make a plain pointer compare safe.
//
//   * Once `ready` is observed true (via an *acquire* load), reading
//     `self->value` needs no lock at all: the writer stores the fully
//     constructed value into `self->value` *before* it stores `ready = 1`
//     with *release* ordering (see lazy_call's success path), so an acquire
//     load that observes `ready == 1` is guaranteed (by the C11 memory
//     model's release/acquire synchronizes-with rule) to also see that
//     `value` write -- a classic "seqlock-free publish" of an
//     immutable-once-published value. This is the hot/fast path: every call
//     to an already-computed lazy value is a single atomic load plus an
//     INCREF, no mutex acquisition, on any thread, ever.
//   * While pending, a real mutex (`pthread_mutex_t`, created
//     PTHREAD_MUTEX_RECURSIVE to match the reference's use of
//     `threading.RLock` -- see lazy_call's rationale below) serializes the
//     one interesting transition: actually invoking the wrapped callable and
//     publishing its result. Concurrent callers race to acquire the mutex;
//     whoever gets it first re-checks `ready` (double-checked locking, since
//     another thread may have finished the computation while this one was
//     waiting on the lock) and, only if still pending, performs the call.
//     Every other caller either takes the fast path above or blocks briefly
//     on the mutex and then observes the now-ready value.
//   * A mutex, not a spinlock, is used deliberately: the wrapped callable is
//     arbitrary user code that may itself block or take a while, and there's
//     no bound on how long the lock might be held.
//   * Reentrancy: the reference's use of `RLock` (not a plain `Lock`) means
//     a lazy value whose own computation somehow recursively calls itself
//     (on the *same* thread) does not deadlock -- the recursive acquire
//     succeeds. It does still re-run the computation in that case (since
//     `self.partial`/`ready` haven't been updated yet by the outer,
//     still-in-progress call), exactly mirroring the Python reference; that
//     is arguably surprising behavior for a self-referential lazy value, but
//     it is *not* a bug we're introducing -- it's the reference's own
//     documented-by-implication behavior, faithfully reproduced.
//   * On failure (the callable raises), the cell is left pending -- neither
//     `fn`/`args`/`kwargs` nor `ready` are touched -- so a later call can
//     legitimately retry the computation, exactly as the Python reference
//     permits (it only clears `self.partial` / sets `self.value` in the
//     success path).

//=============================================================================
// Initialization.

#include <Python.h>
#include <stdatomic.h>
#include <pthread.h>
#include "uintbits.h"
#include "trie.h"
#include "fat.h"

#ifdef __cplusplus
#  define EXTC extern "C"
#else
#  define EXTC
#endif


//=============================================================================
// Module-level imports, cached once at PyInit_lazy time.

static PyObject* g_functools_partial_type = NULL;

// pdict/tdict (pcollections._c.dict) and plist/tlist (pcollections._c.list)
// -- the base types that ldict/tldict/llist/tllist (further down) subclass.
// Imported once at PyInit_lazy time.
static PyTypeObject* PDictType = NULL;
static PyTypeObject* TDictType = NULL;
static PyTypeObject* PListType = NULL;
static PyTypeObject* TListType = NULL;
// collections.abc.ItemsView/ValuesView: ldict.items()/values() (and
// tldict's) return plain instances of these generic ABC mixins rather than
// new bespoke C view types -- see the note on LDictType's `items`/`values`
// methods below for why that's sufficient and correct.
static PyObject* g_ItemsView = NULL;
static PyObject* g_ValuesView = NULL;
// pcollections.util.seqstr -- the real reference implementation used by
// ldict/tldict/llist's __str__/__repr__ (see ldict_repr/ldict_str/etc.
// below). A bound PyCFunction wrapping reprlazy_c (below) is cached as
// g_reprlazy_func and passed as seqstr's `tostr` argument wherever the
// reference passes `tostr=reprlazy`.
static PyObject* g_seqstr = NULL;
static PyObject* g_reprlazy_func = NULL;
// Cached *unbound* base-class methods, fetched once via
// PyObject_GetAttrString((PyObject*)BaseType, "methodname") at init time --
// each is a plain function/method-descriptor object that must be called as
// `func(self, ...)`, exactly mirroring how the reference Python code calls
// e.g. `pdict.get(self, key, default)` to reach the *base* implementation
// even when `self` is really an ldict (bypassing ldict's own override,
// which is the whole point). See call_unbound() below.
static PyObject* g_pdict_get = NULL;
static PyObject* g_tdict_get = NULL;
static PyObject* g_tdict_pop = NULL;

// Calls the real pcollections.util.seqstr(seq, maxlen=maxlen, tostr=tostr).
// maxlen is passed only when has_maxlen is true (matching the reference's
// maxlen=None default when omitted); tostr is passed only when non-NULL
// (matching the reference's tostr=repr default when omitted).
static PyObject* call_seqstr(PyObject* seq, long maxlen, int has_maxlen,
                              PyObject* tostr) {
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
   if (tostr && PyDict_SetItemString(kwargs, "tostr", tostr) < 0) {
      Py_DECREF(args); Py_DECREF(kwargs); return NULL;
   }
   result = PyObject_Call(g_seqstr, args, kwargs);
   Py_DECREF(args); Py_DECREF(kwargs);
   return result;
}


//=============================================================================
// LazyError: a RuntimeError subclass carrying the (fn, args, kwargs) that a
// lazy computation was attempting when it failed. See _lazy.py's LazyError
// for the reference: __init__ stores `partial` as an ordinary attribute
// (BaseException.__new__, inherited and left untouched here, already handles
// `.args`), and __str__/__repr__ build a descriptive message from it.
//
// `partial` may be either a real `functools.partial` instance (if a caller
// constructs LazyError directly with one -- not something lazy.c itself ever
// does, but part of the documented interface) or a plain
// (fn, args, kwargs-dict) tuple (what lazy_new always uses internally).

static PyTypeObject* LazyErrorType = NULL;

static int lazyerror_init(PyObject* self, PyObject* args, PyObject* kwds) {
   PyObject* partial_arg;
   static char* kwlist[] = {"partial", NULL};
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &partial_arg))
      return -1;
   if (PyObject_SetAttrString(self, "partial", partial_arg) < 0) return -1;
   return 0;
}

// Builds the `lazy raised error during call to fn(args...)` message
// described by _lazy.py's LazyError.__str__. Returns a new reference (a str)
// or NULL with an exception set.
static PyObject* lazyerror_buildmsg(PyObject* self) {
   PyObject* partial_val;
   PyObject *fn, *fargs, *fkwargs;
   int is_real_partial;
   PyObject *fnname, *args_tuple, *args_str, *msg;

   partial_val = PyObject_GetAttrString(self, "partial");
   if (!partial_val) return NULL;

   is_real_partial = PyObject_IsInstance(partial_val, g_functools_partial_type);
   if (is_real_partial < 0) { Py_DECREF(partial_val); return NULL; }
   if (is_real_partial) {
      fn = PyObject_GetAttrString(partial_val, "func");
      fargs = PyObject_GetAttrString(partial_val, "args");
      fkwargs = PyObject_GetAttrString(partial_val, "keywords");
      Py_DECREF(partial_val);
      if (!fn || !fargs || !fkwargs) {
         Py_XDECREF(fn); Py_XDECREF(fargs); Py_XDECREF(fkwargs);
         return NULL;
      }
   } else {
      if (!PyTuple_Check(partial_val) || PyTuple_GET_SIZE(partial_val) != 3) {
         Py_DECREF(partial_val);
         PyErr_SetString(PyExc_TypeError,
            "LazyError.partial must be a functools.partial instance or a "
            "(fn, args, kwargs) tuple");
         return NULL;
      }
      fn = PyTuple_GET_ITEM(partial_val, 0); Py_INCREF(fn);
      fargs = PyTuple_GET_ITEM(partial_val, 1); Py_INCREF(fargs);
      fkwargs = PyTuple_GET_ITEM(partial_val, 2); Py_INCREF(fkwargs);
      Py_DECREF(partial_val);
   }

   fnname = PyObject_GetAttrString(fn, "__name__");
   if (!fnname) {
      PyErr_Clear();
      fnname = PyUnicode_FromString("<anonymous>");
      if (!fnname) {
         Py_DECREF(fn); Py_DECREF(fargs); Py_DECREF(fkwargs);
         return NULL;
      }
   }
   args_tuple = PySequence_Tuple(fargs);
   Py_DECREF(fargs);
   if (!args_tuple) {
      Py_DECREF(fn); Py_DECREF(fkwargs); Py_DECREF(fnname);
      return NULL;
   }
   args_str = PyObject_Str(args_tuple);
   Py_DECREF(args_tuple);
   if (!args_str) {
      Py_DECREF(fn); Py_DECREF(fkwargs); Py_DECREF(fnname);
      return NULL;
   }
   msg = PyUnicode_FromFormat("lazy raised error during call to %U%U",
                              fnname, args_str);
   Py_DECREF(fnname);
   Py_DECREF(args_str);
   Py_DECREF(fn);
   if (!msg) { Py_DECREF(fkwargs); return NULL; }

   if (PyDict_Check(fkwargs) && PyDict_Size(fkwargs) > 0) {
      PyObject* parts = PyList_New(0);
      PyObject *key, *value;
      Py_ssize_t pos = 0;
      PyObject *sep, *opts, *msg_trunc, *newmsg;
      Py_ssize_t mlen;
      if (!parts) { Py_DECREF(fkwargs); Py_DECREF(msg); return NULL; }
      while (PyDict_Next(fkwargs, &pos, &key, &value)) {
         PyObject* vs = PyObject_Str(value);
         PyObject* piece;
         if (!vs) { Py_DECREF(parts); Py_DECREF(fkwargs); Py_DECREF(msg); return NULL; }
         piece = PyUnicode_FromFormat("%S=%U", key, vs);
         Py_DECREF(vs);
         if (!piece) { Py_DECREF(parts); Py_DECREF(fkwargs); Py_DECREF(msg); return NULL; }
         if (PyList_Append(parts, piece) < 0) {
            Py_DECREF(piece); Py_DECREF(parts); Py_DECREF(fkwargs); Py_DECREF(msg);
            return NULL;
         }
         Py_DECREF(piece);
      }
      Py_DECREF(fkwargs);
      sep = PyUnicode_FromString(", ");
      if (!sep) { Py_DECREF(parts); Py_DECREF(msg); return NULL; }
      opts = PyUnicode_Join(sep, parts);
      Py_DECREF(sep);
      Py_DECREF(parts);
      if (!opts) { Py_DECREF(msg); return NULL; }
      // errmsg = f'{errmsg[:-1]}, {opts})'  -- drop the trailing ')', then
      // append ', <opts>)'.
      mlen = PyUnicode_GET_LENGTH(msg);
      msg_trunc = PyUnicode_Substring(msg, 0, mlen - 1);
      Py_DECREF(msg);
      if (!msg_trunc) { Py_DECREF(opts); return NULL; }
      newmsg = PyUnicode_FromFormat("%U, %U)", msg_trunc, opts);
      Py_DECREF(msg_trunc);
      Py_DECREF(opts);
      if (!newmsg) return NULL;
      msg = newmsg;
   } else {
      Py_DECREF(fkwargs);
   }
   return msg;
}

static PyObject* lazyerror_str(PyObject* self) {
   return lazyerror_buildmsg(self);
}
static PyObject* lazyerror_repr(PyObject* self) {
   return lazyerror_buildmsg(self);
}


//=============================================================================
// LazyErrorUnwrapper / lazy_error_unwrap: a singleton that is both a callable
// (`lazy_error_unwrap(err)` unwraps a LazyError to its __context__) and a
// context manager (`with lazy_error_unwrap: ...` does the same to whatever
// LazyError propagates out of the block). See _lazy.py's
// LazyErrorUnwrapper/lazy_error_unwrap for the reference.

typedef struct {
   PyObject_HEAD
} LazyErrorUnwrapperObject;

static PyObject* lazyerrorunwrap_call(PyObject* self, PyObject* args, PyObject* kwds) {
   PyObject* err;
   static char* kwlist[] = {"err", NULL};
   PyObject* ctx;
   (void)self;
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &err)) return NULL;
   if ((PyObject*)Py_TYPE(err) == (PyObject*)LazyErrorType) {
      ctx = PyException_GetContext(err); // new reference, may be NULL (no context => None)
      if (!ctx) Py_RETURN_NONE;
      return ctx;
   }
   Py_INCREF(err);
   return err;
}
static PyObject* lazyerrorunwrap_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   (void)self;
   Py_RETURN_NONE;
}
static PyObject* lazyerrorunwrap_exit(PyObject* self, PyObject* args) {
   PyObject *ex_type, *ex_val, *tb;
   (void)self;
   if (!PyArg_ParseTuple(args, "OOO", &ex_type, &ex_val, &tb)) return NULL;
   if (ex_type == (PyObject*)LazyErrorType) {
      PyObject* ctx = PyException_GetContext(ex_val);
      if (!ctx) {
         PyErr_SetString(PyExc_RuntimeError,
            "LazyError has no __context__ to unwrap");
         return NULL;
      }
      PyErr_SetObject((PyObject*)Py_TYPE(ctx), ctx);
      Py_DECREF(ctx);
      return NULL;
   }
   Py_RETURN_FALSE;
}
static PyMethodDef lazyerrorunwrap_methods[] = {
   {"__enter__", (PyCFunction)lazyerrorunwrap_enter, METH_NOARGS, NULL},
   {"__exit__", (PyCFunction)lazyerrorunwrap_exit, METH_VARARGS, NULL},
   {NULL, NULL, 0, NULL}
};
static PyTypeObject LazyErrorUnwrapperType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.lazy.LazyErrorUnwrapper",
   .tp_basicsize = sizeof(LazyErrorUnwrapperObject),
   .tp_itemsize = 0,
   .tp_flags = Py_TPFLAGS_DEFAULT,
   .tp_call = lazyerrorunwrap_call,
   .tp_methods = lazyerrorunwrap_methods,
};
static PyObject* g_lazy_error_unwrap = NULL; // the singleton instance


//=============================================================================
// The `lazy` type itself.

typedef struct {
   PyObject_HEAD
   PyObject* fn;         // callable; NULL once ready
   PyObject* args;       // tuple; NULL once ready
   PyObject* kwargs;     // dict or NULL; NULL once ready
   PyObject* value;      // valid only once ready
   PyObject* init_error; // a LazyError, pre-built at construction time
   pthread_mutex_t mutex;
   atomic_int ready;     // 0 = pending, 1 = ready -- see the file header.
} LazyObject;

static PyTypeObject* LazyType = NULL;

static int lazy_traverse(LazyObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->fn);
   Py_VISIT(self->args);
   Py_VISIT(self->kwargs);
   Py_VISIT(self->value);
   Py_VISIT(self->init_error);
   return 0;
}
static int lazy_clear(LazyObject* self) {
   Py_CLEAR(self->fn);
   Py_CLEAR(self->args);
   Py_CLEAR(self->kwargs);
   Py_CLEAR(self->value);
   Py_CLEAR(self->init_error);
   return 0;
}
static void lazy_dealloc(LazyObject* self) {
   PyObject_GC_UnTrack(self);
   pthread_mutex_destroy(&self->mutex);
   Py_CLEAR(self->fn);
   Py_CLEAR(self->args);
   Py_CLEAR(self->kwargs);
   Py_CLEAR(self->value);
   Py_CLEAR(self->init_error);
   Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* lazy_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   Py_ssize_t nargs;
   PyObject* fn;
   PyObject* fn_args;
   PyObject* fn_kwargs;
   PyObject* partial_tuple;
   PyObject* init_error;
   LazyObject* self;
   pthread_mutexattr_t attr;
   int mutex_rc;

   nargs = PyTuple_GET_SIZE(args);
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
   fn_args = PyTuple_GetSlice(args, 1, nargs); // new reference
   if (!fn_args) return NULL;
   if (kwds && PyDict_Size(kwds) > 0) {
      fn_kwargs = PyDict_Copy(kwds); // new reference, our own stable copy
      if (!fn_kwargs) { Py_DECREF(fn_args); return NULL; }
   } else {
      fn_kwargs = PyDict_New();
      if (!fn_kwargs) { Py_DECREF(fn_args); return NULL; }
   }

   // Pre-build the LazyError we'll raise if the computation ever fails --
   // mirroring the reference's raise/except dance, whose only real purpose
   // is capturing a traceback rooted at *construction* time rather than at
   // whatever later point __call__ happens to fail.
   partial_tuple = PyTuple_Pack(3, fn, fn_args, fn_kwargs);
   if (!partial_tuple) { Py_DECREF(fn_args); Py_DECREF(fn_kwargs); return NULL; }
   init_error = PyObject_CallFunctionObjArgs((PyObject*)LazyErrorType,
                                              partial_tuple, NULL);
   Py_DECREF(partial_tuple);
   if (!init_error) { Py_DECREF(fn_args); Py_DECREF(fn_kwargs); return NULL; }

   self = (LazyObject*)type->tp_alloc(type, 0);
   if (!self) {
      Py_DECREF(fn_args); Py_DECREF(fn_kwargs); Py_DECREF(init_error);
      return NULL;
   }
   Py_INCREF(fn);
   self->fn = fn;
   self->args = fn_args;
   self->kwargs = fn_kwargs;
   self->value = NULL;
   self->init_error = init_error;
   atomic_init(&self->ready, 0);

   mutex_rc = pthread_mutexattr_init(&attr);
   if (mutex_rc == 0) mutex_rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
   if (mutex_rc == 0) mutex_rc = pthread_mutex_init(&self->mutex, &attr);
   pthread_mutexattr_destroy(&attr);
   if (mutex_rc != 0) {
      Py_DECREF(self);
      PyErr_SetString(PyExc_RuntimeError, "failed to initialize lazy's internal mutex");
      return NULL;
   }

   // Note: unlike PDictObject/PListObject/etc. elsewhere in this project
   // (which use PyObject_GC_New and so must explicitly PyObject_GC_Track
   // themselves), `self` here was allocated via type->tp_alloc() -- the
   // default PyType_GenericAlloc for a GC-enabled heap type already tracks
   // new instances itself, so calling PyObject_GC_Track again here would
   // trip the "object already tracked" assertion.
   return (PyObject*)self;
}

// Chains `raw_exc` (the exception just raised by calling the wrapped
// callable, already the current exception) onto `self->init_error` as its
// __context__, then raises init_error -- mirroring the reference's
// `except Exception as partial_eror: raise init_error` (where Python's
// implicit exception chaining sets init_error.__context__ = partial_eror
// automatically; here we do that step explicitly).
static void lazy_reraise_as_init_error(LazyObject* self) {
   PyObject *etype, *evalue, *etb;
   PyErr_Fetch(&etype, &evalue, &etb);
   PyErr_NormalizeException(&etype, &evalue, &etb);
   if (etb) {
      PyException_SetTraceback(evalue, etb);
      Py_DECREF(etb);
   }
   Py_XDECREF(etype);
   // PyException_SetContext steals `evalue`.
   PyException_SetContext(self->init_error, evalue);
   PyErr_SetObject((PyObject*)Py_TYPE(self->init_error), self->init_error);
}

static PyObject* lazy_call(LazyObject* self, PyObject* args, PyObject* kwds) {
   PyObject* val;

   if ((args && PyTuple_GET_SIZE(args) > 0) || (kwds && PyDict_Size(kwds) > 0)) {
      PyErr_SetString(PyExc_TypeError, "lazy objects take no arguments when called");
      return NULL;
   }

   // Fast, lock-free path: already computed. See the file header for why
   // this acquire-load is sufficient without the mutex.
   if (atomic_load_explicit(&self->ready, memory_order_acquire)) {
      val = self->value;
      Py_INCREF(val);
      return val;
   }

   // Release the GIL before blocking on the mutex. This is not optional: the
   // thread that is currently holding `self->mutex` is, by construction,
   // somewhere inside PyObject_Call(self->fn, ...) below -- arbitrary Python
   // code that will very often need to reacquire the GIL itself (e.g. after
   // a time.sleep(), an I/O wait, or simply running more bytecode) before it
   // can finish and reach pthread_mutex_unlock(). If this thread held the
   // GIL while blocked in pthread_mutex_lock(), that other thread could
   // never get the GIL back, and the two threads would deadlock forever.
   Py_BEGIN_ALLOW_THREADS
   pthread_mutex_lock(&self->mutex);
   Py_END_ALLOW_THREADS
   // Double-checked: another thread may have finished computing this value
   // while we were waiting for the lock.
   if (atomic_load_explicit(&self->ready, memory_order_acquire)) {
      val = self->value;
      Py_INCREF(val);
      pthread_mutex_unlock(&self->mutex);
      return val;
   }

   val = PyObject_Call(self->fn, self->args, self->kwargs);
   if (!val) {
      lazy_reraise_as_init_error(self);
      // Left pending on purpose (fn/args/kwargs/ready untouched): a later
      // call may retry the computation, exactly as the Python reference
      // permits.
      pthread_mutex_unlock(&self->mutex);
      return NULL;
   }

   // Publish: store the value, then release-store `ready`. This ordering
   // (value first, flag second, with release semantics on the flag) is what
   // makes the lock-free fast path above safe on any thread.
   //
   // Also drop fn/args/kwargs *and* init_error here -- mirroring the
   // reference, which discards its whole `partial` tuple (part, rlock,
   // init_error) by setting `self.partial = None` on success. init_error
   // itself holds a (fn, args, kwargs) tuple, so failing to clear it here
   // would silently keep the original callable and its arguments alive
   // forever even after the value is cached -- exactly the "resources
   // previously needed to make the computation" this type is documented to
   // throw away once ready.
   self->value = val; // takes ownership of the reference from PyObject_Call
   Py_CLEAR(self->fn);
   Py_CLEAR(self->args);
   Py_CLEAR(self->kwargs);
   Py_CLEAR(self->init_error);
   atomic_store_explicit(&self->ready, 1, memory_order_release);
   pthread_mutex_unlock(&self->mutex);

   Py_INCREF(val); // an additional reference for the caller
   return val;
}

static PyObject* lazy_is_ready(LazyObject* self, PyObject* Py_UNUSED(ignored)) {
   int ready = atomic_load_explicit(&self->ready, memory_order_acquire);
   return PyBool_FromLong(ready);
}

static PyObject* lazy_repr(LazyObject* self) {
   int ready = atomic_load_explicit(&self->ready, memory_order_acquire);
   return PyUnicode_FromFormat("lazy(<%zu>: %s)",
                               (size_t)(uintptr_t)self,
                               ready ? "ready" : "waiting");
}
static PyObject* lazy_str(LazyObject* self) {
   return PyUnicode_FromFormat("lazy(<%zu>)", (size_t)(uintptr_t)self);
}

static PyMethodDef lazy_methods[] = {
   {"is_ready", (PyCFunction)lazy_is_ready, METH_NOARGS,
    "Returns True if the lazy value is cached and False otherwise."},
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
      "A callable like `partial` for lazily-computed, thread-safe values.\n\n"
      "`l = lazy(fn, *args, **kwargs)` stores the given callable `fn` with\n"
      "the given `args` and `kwargs` as arguments. When the lazy value of\n"
      "`l` is later requested (via `l()`), the value is computed exactly\n"
      "once (safely under concurrent access from multiple threads), cached,\n"
      "and the partial data is forgotten.")},
   {0, NULL}
};
static PyType_Spec lazy_spec = {
   .name = "pcollections._c.lazy.lazy",
   .basicsize = sizeof(LazyObject),
   .itemsize = 0,
   // Not subclassable (Py_TPFLAGS_BASETYPE unset): a `lazy` cell's identity
   // and thread-safety guarantees are tied to its own fixed struct layout;
   // nothing in this project's reference implementation subclasses `lazy`.
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = lazy_slots,
};


//=============================================================================
// Shadow struct layouts for PDictObject/TDictObject/PListObject/TListObject.
//
// ldict/tldict/llist/tllist (below) subclass pdict/tdict/plist/tlist -- C
// types defined in the *separate* translation units dict.c/list.c -- and
// add no new slots of their own (matching the reference _lazy.py's
// `__slots__ = ()` on every one of these four classes), so an ldict
// instance is byte-for-byte identical in memory to a plist... er, pdict
// instance. Most of what follows never needs to know this: __new__
// delegates to the base type's own (already fully subclass-aware) tp_new,
// get()/pop()/etc. delegate to cached *unbound* base-class methods, and
// __getitem__/__iter__ delegate to the base type's own mp_subscript/
// sq_item/tp_iter slots -- all through perfectly ordinary, public,
// cross-TU-safe PyTypeObject/PyObject APIs, no struct-layout knowledge
// required.
//
// A small handful of operations, however, have NO public-API equivalent:
// as_pdict()/as_tdict()/as_plist() and the transient()<->persistent()
// cross-type retargeting (ldict.transient() must return a *tldict*, not a
// plain tdict; tldict.persistent() must return an *ldict*, etc.) all need
// to reach past any dereferencing and directly *share* the other object's
// raw backing trie(s) -- exactly what dict.c's/list.c's own *_wrap_astype
// helpers do internally, except those are `static` (file-local) and so
// cannot be called from here. For just these few call sites, we duplicate
// the minimal struct layout (verified field-for-field against dict.c/
// list.c at the time of writing) so we can read a source object's raw
// (els, idx, top, count, ndeleted)/(root, start, length) directly, bump
// the shared trie nodes' refcounts (trienode_incref -- a generic,
// content-agnostic operation declared `static inline` in the shared
// trie.h, so it's safe to call from any translation unit that includes
// that header), and hand them to a freshly tp_alloc'd instance ourselves.
//
// KEEP THIS IN SYNC with the authoritative struct definitions in dict.c/
// list.c: if those field lists (order, types, or added fields) ever
// change, these mirrors must change with them, or the casts below become
// undefined behavior. (This is the same "separate TUs, small duplicated
// pieces" convention this project already uses for e.g. each collection
// type's own copy of pyobj_incref/pyobj_decref/fat_freeze.)
typedef struct {
   PyObject_HEAD
   Trie_t idx;
   Trie_t els;
   Py_ssize_t top;
   Py_ssize_t count;
   Py_ssize_t ndeleted;
   Py_hash_t hashcode;
} PDictObject;
typedef struct {
   PyObject_HEAD
   Trie_t idx;
   Trie_t els;
   Py_ssize_t top;
   Py_ssize_t count;
   Py_ssize_t ndeleted;
   PyObject* orig;
} TDictObject;
typedef struct {
   PyObject_HEAD
   Trie_t root;
   trieint_t start;
   Py_ssize_t length;
   Py_hash_t hashcode;
} PListObject;
typedef struct {
   PyObject_HEAD
   Trie_t root;
   trieint_t start;
   Py_ssize_t length;
   PyObject* orig;
} TListObject;

// Local copies of list.c's/dict.c's fat_freeze()/amt_freeze() (recursively
// marks a transient trie and everything reachable from it as persistent).
// Needed only by tllist_persistent()/tldict_persistent() below, which (like
// the reference's tlist.persistent()/tdict.persistent()) must freeze the
// tlist/tdict-shaped raw trie(s) they share into the llist/ldict they
// return. Pure, generic, content-agnostic trie-structure operations (see
// list.c's fat_freeze for the fuller rationale) -- safe to duplicate here
// exactly as list.c/dict.c each already duplicate it independently.
static void lazyfile_fat_freeze(Trie_t a) {
   if (!trie_is_transient(a)) return;
   trienode_set_transient(a, false);
   if (!fatnode_is_twig(a)) {
      triebits_t bi;
      for (bi = trienode_first_bitindex(a); bi < FAT_CELLS;
           bi = trienode_next_bitindex(a, bi))
         lazyfile_fat_freeze(trienode_subt(a, bi));
   }
}
static void lazyfile_amt_freeze(Trie_t a) {
   if (!trie_is_transient(a)) return;
   trienode_set_transient(a, false);
   if (!amtnode_is_twig(a)) {
      triebits_t bi;
      for (bi = trienode_first_bitindex(a); bi < TRIEBITS_WIDTH;
           bi = trienode_next_bitindex(a, bi))
         lazyfile_amt_freeze(trienode_subt(a, amtnode_bit2cellindex(a, bi)));
   }
}

// Prepends `self` to the (possibly empty) tuple `args` and calls
// `unbound_func(self, *args)` -- i.e. calls an *unbound* base-class method
// with an explicit `self`, exactly mirroring what the reference Python code
// means by e.g. `pdict.get(self, key, default)`. Returns a new reference,
// or NULL with an exception set.
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

// If `v` is a `lazy` instance, calls it (forcing/caching its value) and
// returns the result; otherwise returns `v` unchanged. In both cases
// steals/returns ownership of exactly one reference (i.e. this is the
// C-level equivalent of the reference's free function `unlazy`, specialized
// to consume its argument rather than borrow it -- convenient at the many
// call sites here that already hold a fresh reference they'd otherwise have
// to separately DECREF).
static PyObject* unlazy_owned(PyObject* v) {
   if (!v) return NULL;
   if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
      PyObject* result = PyObject_CallFunctionObjArgs(v, NULL);
      Py_DECREF(v);
      return result;
   }
   return v;
}

// holdlazy(obj), with require_lazy=False hardcoded (the reference's
// `map(holdlazy, args)` in llist/ldict/tldict's __new__ always uses the
// default) -- see mod_holdlazy further below for the full (user-facing,
// require_lazy-configurable) version this mirrors.
static PyObject* holdlazy_obj(PyObject* obj) {
   PyObject* result;
   if (!PyObject_HasAttrString(obj, "__holdlazy__")) {
      Py_INCREF(obj);
      return obj;
   }
   result = PyObject_CallMethod(obj, "__holdlazy__", NULL);
   return result;
}
// Maps holdlazy_obj over every element of `args` (a tuple), leaving kwds
// untouched -- mirrors llist/ldict/tldict's `__new__(cls, *args, **kw):
// return <base>.__new__(cls, *map(holdlazy, args), **kw)`.
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

static PyTypeObject* build_c_subtype(PyType_Spec* spec, PyTypeObject* base) {
   PyObject* bases = PyTuple_Pack(1, (PyObject*)base);
   PyObject* result;
   PyTypeObject* result_t;
   if (!bases) return NULL;
   result = PyType_FromSpecWithBases(spec, bases);
   Py_DECREF(bases);
   if (!result) return NULL;
   result_t = (PyTypeObject*)result;
   // Mirror dict.c's build_abc_subtype(): explicitly propagate tp_richcompare
   // from `base` if this spec didn't set its own (none of ldict/tldict/
   // llist/tllist's specs set Py_tp_richcompare -- they all rely on
   // inheriting pdict/tdict/plist/tlist's own comparison behavior
   // unchanged). This was found to be necessary empirically: ordinary
   // PyType_FromSpecWithBases slot inheritance did not reliably carry
   // tp_richcompare from a base that is *itself* a heap type built via
   // PyType_FromSpecWithBases (pdict/tdict rely on this for their own
   // inherited Mapping.__eq__ via a raw field poke rather than a spec
   // slot -- see build_abc_subtype's own extensive comment in dict.c for
   // why it's done as a raw field assignment rather than a spec slot in
   // the first place). Doing the same raw-field copy one level further
   // down (base -> this new subtype) restores correct `==`/`!=` behavior;
   // confirmed via a direct regression (`ldict(x=5) == {"x": 5}` returned
   // False before this fix).
   if (!result_t->tp_richcompare) result_t->tp_richcompare = base->tp_richcompare;
   return result_t;
}


//=============================================================================
// ldict (persistent lazy dict) and tldict (transient lazy dict).
// See _lazy.py's ldict/tldict for the reference.

static PyTypeObject* LDictType = NULL;
static PyTypeObject* TLDictType = NULL;
static PDictObject* g_ldict_empty = NULL;

// Raw-shares src's backing tries into a fresh instance of `type` (which must
// be PDictObject-layout: PDictType or LDictType), with hashcode reset to
// -1 (uncached) -- no leaf-level work at all, just two refcount bumps. Used
// by ldict.as_pdict()/__holdlazy__() and by the ldict.empty bootstrap in
// PyInit_lazy.
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
   result = PDictType->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}

static PyObject* ldict_subscript(PDictObject* self, PyObject* key) {
   PyObject* v = PDictType->tp_as_mapping->mp_subscript((PyObject*)self, key);
   return unlazy_owned(v);
}

static PyObject* ldict_get(PyObject* self, PyObject* args) {
   return unlazy_owned(call_unbound(g_pdict_get, self, args));
}

static PyObject* ldict_getlazy(PyObject* self, PyObject* args) {
   // Deliberately RAW (no unlazy_owned) -- matches the reference's
   // `getlazy`, whose entire purpose is to hand back the `lazy` object
   // itself rather than its (possibly not-yet-computed) value.
   return call_unbound(g_pdict_get, self, args);
}

static PyObject* ldict_items(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   // The reference's `ldict_items`/`ldict_values` are Python classes with an
   // overridable `_from_kv` hook (subclassing the pure-Python `pdict_items`/
   // `pdict_values`). Our C dict.c instead builds PDictItemsType/
   // PDictValuesType directly atop collections.abc.ItemsView/ValuesView,
   // with no `_from_kv`-style hook to subclass. But ItemsView/ValuesView's
   // own __iter__/__contains__ are already specified (by collections.abc)
   // to work by calling `self._mapping[key]` -- i.e. `mapping.__getitem__`,
   // which for an ldict is *ldict's own* (dereferencing) override, since
   // `self._mapping` really is the ldict instance itself. So plain
   // `ItemsView(self)`/`ValuesView(self)` already dereference correctly,
   // with no new C view type needed at all.
   return PyObject_CallFunctionObjArgs(g_ItemsView, self, NULL);
}
static PyObject* ldict_values(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs(g_ValuesView, self, NULL);
}

static PyObject* ldict_is_lazy(PyObject* self, PyObject* key) {
   // Raw (undereferenced) lookup -- matches reference's `pdict.__getitem__`.
   PyObject* v = PDictType->tp_as_mapping->mp_subscript(self, key);
   int is_lazy;
   if (!v) return NULL;
   is_lazy = ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType);
   Py_DECREF(v);
   return PyBool_FromLong(is_lazy);
}
static PyObject* ldict_is_ready(PyObject* self, PyObject* key) {
   PyObject* v = PDictType->tp_as_mapping->mp_subscript(self, key);
   PyObject* result;
   if (!v) return NULL;
   if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
      result = PyObject_CallMethod(v, "is_ready", NULL);
   } else {
      result = Py_True;
      Py_INCREF(result);
   }
   Py_DECREF(v);
   return result;
}
static PyObject* ldict_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   PyObject* it = PyObject_GetIter(self); // ldict doesn't override __iter__:
                                          // dict iteration yields keys only,
                                          // which are never lazy.
   PyObject* key;
   if (!it) return NULL;
   while ((key = PyIter_Next(it))) {
      PyObject* v = PDictType->tp_as_mapping->mp_subscript(self, key);
      Py_DECREF(key);
      if (!v) { Py_DECREF(it); return NULL; }
      if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
         PyObject* forced = PyObject_CallFunctionObjArgs(v, NULL);
         Py_DECREF(v);
         if (!forced) { Py_DECREF(it); return NULL; }
         Py_DECREF(forced);
      } else {
         Py_DECREF(v);
      }
   }
   Py_DECREF(it);
   if (PyErr_Occurred()) return NULL;
   Py_INCREF(self);
   return self;
}
static PyObject* ldict_as_pdict(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   return pdictlike_share(PDictType, self);
}
static PyObject* ldict_clear(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   (void)self;
   Py_INCREF(g_ldict_empty);
   return (PyObject*)g_ldict_empty;
}
static PyObject* ldict_transient(PDictObject* self, PyObject* Py_UNUSED(ignored)) {
   TDictObject* t = (TDictObject*)TLDictType->tp_alloc(TLDictType, 0);
   if (!t) return NULL;
   trienode_incref(self->els);
   trienode_incref(self->idx);
   Py_INCREF(self);
   t->els = self->els;
   t->idx = self->idx;
   t->top = self->top;
   t->count = self->count;
   t->ndeleted = self->ndeleted;
   t->orig = (PyObject*)self;
   return (PyObject*)t;
}

// GC support: PyType_FromSpecWithBases does NOT automatically inherit
// Py_tp_traverse/Py_tp_clear from the base type the way ordinary Python-
// level subclassing does -- a spec that itself declares Py_TPFLAGS_HAVE_GC
// (as all four of ldict/tldict/llist/tllist's specs do, needed since they
// hold PyObject* fields) must supply its own traverse/clear functions or
// type creation fails outright ("has the Py_TPFLAGS_HAVE_GC flag but has no
// traverse function"). Since ldict is layout-identical to PDictObject, we
// simply forward to whatever PDictType's own tp_traverse/tp_clear happen to
// be -- these are plain runtime field reads (PDictType is fully built by
// the time any ldict instance exists to be traversed/cleared), not
// something that needs to be known at this file's compile time.
static int ldict_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return PDictType->tp_traverse(self, visit, arg);
}
static int ldict_gc_clear(PyObject* self) {
   return PDictType->tp_clear(self);
}

// __str__/__repr__: now that pcollections/util.py (really a util/
// subpackage: __init__.py + _core.py) is available, these call the real
// util.seqstr directly on self.as_pdict() (a raw pdict sharing the same
// els/idx tries, so un-forced values are still `lazy` instances), exactly
// matching _lazy.py's `ldict.__str__`/`__repr__`:
//   __str__: f"{{|{seqstr(self.as_pdict(), maxlen=60, tostr=reprlazy)}|}}"
//   __repr__: f"{{|{seqstr(self.as_pdict())}|}}"  (default tostr=repr, NOT
//             reprlazy -- so an un-forced value's repr shows as
//             "lazy(<id>: waiting)" via lazy's own __repr__, not "<lazy>";
//             only __str__ hides it behind "<lazy>").
static PyObject* ldict_repr(PDictObject* self) {
   PyObject* raw = ldict_as_pdict(self, NULL);
   PyObject* s; PyObject* result;
   if (!raw) return NULL;
   s = call_seqstr(raw, 0, 0, NULL);
   Py_DECREF(raw);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* ldict_str(PDictObject* self) {
   PyObject* raw = ldict_as_pdict(self, NULL);
   PyObject* s; PyObject* result;
   if (!raw) return NULL;
   s = call_seqstr(raw, 60, 1, g_reprlazy_func);
   Py_DECREF(raw);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}

// Forward-declared: ldict_slots (right below) needs to reference
// ldict_hash, whose definition follows ldict_spec purely for readability
// (grouping __str__/__repr__/__hash__ together, right after the main
// methods table).
static Py_hash_t ldict_hash(PDictObject* self);

static PyMethodDef ldict_methods[] = {
   {"get", (PyCFunction)ldict_get, METH_VARARGS,
    "Returns the (dereferenced) value for key, or default if not present."},
   {"items", (PyCFunction)ldict_items, METH_NOARGS,
    "Returns a (dereferencing) view of the ldict's items."},
   {"values", (PyCFunction)ldict_values, METH_NOARGS,
    "Returns a (dereferencing) view of the ldict's values."},
   {"is_lazy", (PyCFunction)ldict_is_lazy, METH_O,
    "Returns True if the given key is mapped to a lazy value."},
   {"is_ready", (PyCFunction)ldict_is_ready, METH_O,
    "Returns True if the given key's value can be returned without further computation."},
   {"ready_all", (PyCFunction)ldict_ready_all, METH_NOARGS,
    "Caches all lazy items then returns the ldict."},
   {"as_pdict", (PyCFunction)ldict_as_pdict, METH_NOARGS,
    "Returns a pdict with the ldict's lazy values left uncached."},
   {"__holdlazy__", (PyCFunction)ldict_as_pdict, METH_NOARGS, NULL},
   {"getlazy", (PyCFunction)ldict_getlazy, METH_VARARGS,
    "Like get(), but returns lazy objects instead of their results."},
   {"clear", (PyCFunction)ldict_clear, METH_NOARGS,
    "Returns the empty ldict."},
   {"transient", (PyCFunction)ldict_transient, METH_NOARGS,
    "Efficiently copies the ldict into a tldict and returns the tldict."},
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
      "A persistent lazy dict type: identical to pdict, except that any "
      "lazy value contained in it is dereferenced prior to being returned "
      "by item extraction or iteration.")},
   {0, NULL}
};
static PyType_Spec ldict_spec = {
   .name = "pcollections._c.lazy.ldict",
   .basicsize = sizeof(PDictObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = ldict_slots,
};

// ldict_hash: forces every lazy value via ldict's own (dereferencing)
// items() before hashing -- matching the reference's documented "hashing an
// ldict object results in all lazy values being calculated". This needs to
// be a real override (not simply inherited from pdict) because dict.c's
// pdict_hash walks the raw backing trie directly (bypassing any subclass
// __getitem__/items() override entirely) rather than going through `self`
// polymorphically -- see this file's header-comment-adjacent note near
// LDictType's build site in PyInit_lazy for the fuller rationale.
static Py_hash_t ldict_hash(PDictObject* self) {
   PyObject *items, *fs, *h;
   Py_hash_t result;
   if (self->hashcode != -1) return self->hashcode;
   items = PyObject_CallFunctionObjArgs(g_ItemsView, (PyObject*)self, NULL);
   if (!items) return -1;
   fs = PySet_New(items);
   Py_DECREF(items);
   if (!fs) return -1;
   h = PyFrozenSet_New(fs);
   Py_DECREF(fs);
   if (!h) return -1;
   result = PyObject_Hash(h);
   Py_DECREF(h);
   if (result == -1) return -1;
   result += 2; // matches pdict_hash's own frozenset-hash-plus-2 convention.
   if (result == -1) result = -2;
   self->hashcode = result;
   return result;
}

static PyObject* tldict_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = TDictType->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
static PyObject* tldict_subscript(TDictObject* self, PyObject* key) {
   PyObject* v = TDictType->tp_as_mapping->mp_subscript((PyObject*)self, key);
   return unlazy_owned(v);
}
static PyObject* tldict_get(PyObject* self, PyObject* args) {
   return unlazy_owned(call_unbound(g_tdict_get, self, args));
}
static PyObject* tldict_getlazy(PyObject* self, PyObject* args) {
   return call_unbound(g_tdict_get, self, args); // raw, no unlazy_owned.
}
static PyObject* tldict_pop(PyObject* self, PyObject* args) {
   // The reference's _lazy.py originally had `def pop(self, *args): return
   // unlazy(self.pop(*args))` -- calling `self.pop` from *inside*
   // `tldict.pop` recursed into itself forever (infinite recursion / a
   // RecursionError on the very first call). Confirmed with Noah and fixed
   // in _lazy.py to `unlazy(tdict.pop(self, *args))` (an explicit
   // base-class call, exactly like every other method in this file) --
   // this C implementation already matched that fix.
   return unlazy_owned(call_unbound(g_tdict_pop, self, args));
}
static PyObject* tldict_items(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   return PyObject_CallFunctionObjArgs(g_ItemsView, self, NULL);
}
static PyObject* tldict_values(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   // See ldict_items' comment on why a plain generic ValuesView already
   // dereferences correctly. This does mean we don't reproduce the
   // reference's tldict_values.__contains__ optimization (checking non-lazy
   // entries before forcing any lazy ones) -- ValuesView's generic
   // __contains__ just checks each key's (dereferencing) value in whatever
   // order iteration yields them, which is equally *correct*, just not
   // necessarily as laziness-preserving in the case where the sought value
   // is found among the non-lazy entries. A deliberate simplification.
   return PyObject_CallFunctionObjArgs(g_ValuesView, self, NULL);
}
static PyObject* tldict_is_lazy(PyObject* self, PyObject* key) {
   PyObject* v = TDictType->tp_as_mapping->mp_subscript(self, key);
   int is_lazy;
   if (!v) return NULL;
   is_lazy = ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType);
   Py_DECREF(v);
   return PyBool_FromLong(is_lazy);
}
static PyObject* tldict_is_ready(PyObject* self, PyObject* key) {
   PyObject* v = TDictType->tp_as_mapping->mp_subscript(self, key);
   PyObject* result;
   if (!v) return NULL;
   if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
      result = PyObject_CallMethod(v, "is_ready", NULL);
   } else {
      result = Py_True;
      Py_INCREF(result);
   }
   Py_DECREF(v);
   return result;
}
static PyObject* tldict_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   PyObject* it = PyObject_GetIter(self);
   PyObject* key;
   if (!it) return NULL;
   while ((key = PyIter_Next(it))) {
      PyObject* v = TDictType->tp_as_mapping->mp_subscript(self, key);
      Py_DECREF(key);
      if (!v) { Py_DECREF(it); return NULL; }
      if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
         PyObject* forced = PyObject_CallFunctionObjArgs(v, NULL);
         Py_DECREF(v);
         if (!forced) { Py_DECREF(it); return NULL; }
         Py_DECREF(forced);
      } else {
         Py_DECREF(v);
      }
   }
   Py_DECREF(it);
   if (PyErr_Occurred()) return NULL;
   Py_INCREF(self);
   return self;
}
static PyObject* tldict_as_tdict(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   TDictObject* t = (TDictObject*)TDictType->tp_alloc(TDictType, 0);
   if (!t) return NULL;
   trienode_incref(self->els);
   trienode_incref(self->idx);
   t->els = self->els;
   t->idx = self->idx;
   t->top = self->top;
   t->count = self->count;
   t->ndeleted = self->ndeleted;
   t->orig = NULL; // matches reference's `tdict._new(self._els, self._idx,
                    // self._top)` -- no cached orig for the plain-tdict
                    // result of as_tdict().
   return (PyObject*)t;
}
static PyObject* tldict_persistent(TDictObject* self, PyObject* Py_UNUSED(ignored)) {
   PDictObject* p;
   if (self->count == 0) {
      Py_INCREF(g_ldict_empty);
      return (PyObject*)g_ldict_empty;
   }
   if (self->orig) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   lazyfile_fat_freeze(self->els);
   lazyfile_amt_freeze(self->idx);
   p = (PDictObject*)LDictType->tp_alloc(LDictType, 0);
   if (!p) return NULL;
   trienode_incref(self->els);
   trienode_incref(self->idx);
   p->els = self->els;
   p->idx = self->idx;
   p->top = self->top;
   p->count = self->count;
   p->ndeleted = self->ndeleted;
   p->hashcode = -1;
   return (PyObject*)p;
}
// Matches _lazy.py's tldict.__str__/__repr__ exactly:
//   __str__: f"{{|{seqstr(self.as_tdict(), maxlen=60, tostr=reprlazy)}|}}"
//   __repr__: f"{{|{seqstr(self.as_tdict())}|}}"
// NB: both use the "{|...|}" delimiter -- NOT tdict's own transient
// "{<...>}" shape (see tdict_str/tdict_repr in dict.c) -- this is what the
// reference actually does, not a mistake to "fix" here.
static PyObject* tldict_repr(TDictObject* self) {
   PyObject* as_td = tldict_as_tdict(self, NULL);
   PyObject* s; PyObject* result;
   if (!as_td) return NULL;
   s = call_seqstr(as_td, 0, 0, NULL);
   Py_DECREF(as_td);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}
static PyObject* tldict_str(TDictObject* self) {
   PyObject* as_td = tldict_as_tdict(self, NULL);
   PyObject* s; PyObject* result;
   if (!as_td) return NULL;
   s = call_seqstr(as_td, 60, 1, g_reprlazy_func);
   Py_DECREF(as_td);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("{|%U|}", s);
   Py_DECREF(s);
   return result;
}

// See ldict_gc_traverse/ldict_gc_clear's comment.
static int tldict_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return TDictType->tp_traverse(self, visit, arg);
}
static int tldict_gc_clear(PyObject* self) {
   return TDictType->tp_clear(self);
}

static PyMethodDef tldict_methods[] = {
   {"get", (PyCFunction)tldict_get, METH_VARARGS,
    "Returns the (dereferenced) value for key, or default if not present."},
   {"getlazy", (PyCFunction)tldict_getlazy, METH_VARARGS,
    "Like get(), but returns lazy objects instead of their results."},
   {"pop", (PyCFunction)tldict_pop, METH_VARARGS,
    "Removes and returns the (dereferenced) value for the given key."},
   {"items", (PyCFunction)tldict_items, METH_NOARGS,
    "Returns a (dereferencing) view of the tldict's items."},
   {"values", (PyCFunction)tldict_values, METH_NOARGS,
    "Returns a (dereferencing) view of the tldict's values."},
   {"is_lazy", (PyCFunction)tldict_is_lazy, METH_O,
    "Returns True if the given key is mapped to a lazy value."},
   {"is_ready", (PyCFunction)tldict_is_ready, METH_O,
    "Returns True if the given key's value can be returned without further computation."},
   {"ready_all", (PyCFunction)tldict_ready_all, METH_NOARGS,
    "Caches all lazy items then returns the tldict."},
   {"as_tdict", (PyCFunction)tldict_as_tdict, METH_NOARGS,
    "Returns a tdict with the tldict's lazy values left uncached."},
   {"__holdlazy__", (PyCFunction)tldict_as_tdict, METH_NOARGS, NULL},
   {"persistent", (PyCFunction)tldict_persistent, METH_NOARGS,
    "Efficiently copies the tldict into an ldict and returns the ldict."},
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
      "A transient lazy dict type: identical to tdict, except that item "
      "access automatically returns the reified values of lazy values "
      "instead of the lazy objects themselves.")},
   {0, NULL}
};
static PyType_Spec tldict_spec = {
   .name = "pcollections._c.lazy.tldict",
   .basicsize = sizeof(TDictObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = tldict_slots,
};


//=============================================================================
// llist (persistent lazy list) and tllist (transient lazy list).
// See _lazy.py's llist/tllist for the reference.

static PyTypeObject* LListType = NULL;
static PyTypeObject* TLListType = NULL;
static PListObject* g_llist_empty = NULL;

typedef struct {
   PyObject_HEAD
   PyObject* inner; // the raw (un-dereferencing) iterator being wrapped.
} UnlazyIterObject;
static void unlazyiter_dealloc(UnlazyIterObject* self) {
   PyObject_GC_UnTrack(self);
   Py_CLEAR(self->inner);
   PyObject_GC_Del(self);
}
static int unlazyiter_traverse(UnlazyIterObject* self, visitproc visit, void* arg) {
   Py_VISIT(self->inner);
   return 0;
}
static int unlazyiter_clear(UnlazyIterObject* self) {
   Py_CLEAR(self->inner);
   return 0;
}
static PyObject* unlazyiter_next(UnlazyIterObject* self) {
   return unlazy_owned(PyIter_Next(self->inner));
}
static PyObject* unlazyiter_self(PyObject* self) { Py_INCREF(self); return self; }
static PyTypeObject UnlazyIterType = {
   PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "pcollections._c.lazy._unlazy_iterator",
   .tp_basicsize = sizeof(UnlazyIterObject),
   .tp_dealloc = (destructor)unlazyiter_dealloc,
   .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .tp_traverse = (traverseproc)unlazyiter_traverse,
   .tp_clear = (inquiry)unlazyiter_clear,
   .tp_iter = unlazyiter_self,
   .tp_iternext = (iternextfunc)unlazyiter_next,
};
// Steals `inner` (a new reference to the raw iterator to wrap, or NULL if
// obtaining it already failed, in which case this just propagates NULL).
static PyObject* make_unlazy_iter(PyObject* inner) {
   UnlazyIterObject* it;
   if (!inner) return NULL;
   it = PyObject_GC_New(UnlazyIterObject, &UnlazyIterType);
   if (!it) { Py_DECREF(inner); return NULL; }
   it->inner = inner;
   PyObject_GC_Track(it);
   return (PyObject*)it;
}

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

static PyObject* llist_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
   PyObject* held_args = holdlazy_map_args(args);
   PyObject* result;
   if (!held_args) return NULL;
   result = PListType->tp_new(type, held_args, kwds);
   Py_DECREF(held_args);
   return result;
}
static PyObject* llist_subscript(PListObject* self, PyObject* key) {
   // Handles both int and slice keys uniformly, exactly like the reference
   // (`el = plist.__getitem__(self, k); return el() if isinstance(el, lazy)
   // else el`). For a slice key, the reference's plist.__getitem__ builds
   // its result via `self._new(...)` -- a classmethod bound through the
   // *actual* instance, so when self is an llist, that already constructs
   // another llist (not a plain plist), still holding any lazy elements
   // uncached internally. Our C plist_subscript's slice branch is
   // (correctly, per list.c's own subclass-preservation work) parameterized
   // by Py_TYPE(self) the same way, so delegating to PListType's own
   // mp_subscript here already produces an llist for a slice key, exactly
   // matching the reference -- `el` is therefore never itself a `lazy`
   // instance in the slice case, so it passes through unforced, while a
   // scalar index's raw (possibly-lazy) element does get dereferenced here.
   PyObject* v = PListType->tp_as_mapping->mp_subscript((PyObject*)self, key);
   return unlazy_owned(v);
}
static PyObject* llist_item(PListObject* self, Py_ssize_t i) {
   PyObject* v = PListType->tp_as_sequence->sq_item((PyObject*)self, i);
   return unlazy_owned(v);
}
static PyObject* llist_iter(PListObject* self) {
   return make_unlazy_iter(PListType->tp_iter((PyObject*)self));
}
static PyObject* llist_is_lazy(PyObject* self, PyObject* index_obj) {
   Py_ssize_t i = PyNumber_AsSsize_t(index_obj, PyExc_IndexError);
   PyObject* v;
   int is_lazy;
   if (i == -1 && PyErr_Occurred()) return NULL;
   v = PListType->tp_as_sequence->sq_item(self, i);
   if (!v) return NULL;
   is_lazy = ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType);
   Py_DECREF(v);
   return PyBool_FromLong(is_lazy);
}
static PyObject* llist_is_ready(PyObject* self, PyObject* index_obj) {
   Py_ssize_t i = PyNumber_AsSsize_t(index_obj, PyExc_IndexError);
   PyObject* v;
   PyObject* result;
   if (i == -1 && PyErr_Occurred()) return NULL;
   v = PListType->tp_as_sequence->sq_item(self, i);
   if (!v) return NULL;
   if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
      result = PyObject_CallMethod(v, "is_ready", NULL);
   } else {
      result = Py_True;
      Py_INCREF(result);
   }
   Py_DECREF(v);
   return result;
}
static PyObject* llist_ready_all(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   Py_ssize_t n = PyObject_Length(self);
   Py_ssize_t i;
   if (n < 0) return NULL;
   for (i = 0; i < n; ++i) {
      PyObject* v = PListType->tp_as_sequence->sq_item(self, i);
      if (!v) return NULL;
      if ((PyObject*)Py_TYPE(v) == (PyObject*)LazyType) {
         PyObject* forced = PyObject_CallFunctionObjArgs(v, NULL);
         Py_DECREF(v);
         if (!forced) return NULL;
         Py_DECREF(forced);
      } else {
         Py_DECREF(v);
      }
   }
   Py_INCREF(self);
   return self;
}
static PyObject* llist_as_plist(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   return plistlike_share(PListType, self);
}
static PyObject* llist_getlazy(PyObject* self, PyObject* index_obj) {
   return PListType->tp_as_mapping->mp_subscript(self, index_obj); // raw.
}
static PyObject* llist_clear(PyObject* self, PyObject* Py_UNUSED(ignored)) {
   (void)self;
   Py_INCREF(g_llist_empty);
   return (PyObject*)g_llist_empty;
}
static PyObject* llist_transient(PListObject* self, PyObject* Py_UNUSED(ignored)) {
   TListObject* t = (TListObject*)TLListType->tp_alloc(TLListType, 0);
   if (!t) return NULL;
   trienode_incref(self->root);
   Py_INCREF(self);
   t->root = self->root;
   t->start = self->start;
   t->length = self->length;
   t->orig = (PyObject*)self;
   return (PyObject*)t;
}
// See ldict_gc_traverse/ldict_gc_clear's comment.
static int llist_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return PListType->tp_traverse(self, visit, arg);
}
static int llist_gc_clear(PyObject* self) {
   return PListType->tp_clear(self);
}
// Matches _lazy.py's llist.__str__/__repr__ exactly, now that the real
// util.seqstr is available:
//   __str__: f"[|{seqstr(self.as_plist(), maxlen=60, tostr=reprlazy)}|]"
//   __repr__: f"[|{seqstr(self.as_plist())}|]"  (default tostr=repr)
static PyObject* llist_repr(PListObject* self) {
   PyObject* raw = llist_as_plist(self, NULL);
   PyObject* s; PyObject* result;
   if (!raw) return NULL;
   s = call_seqstr(raw, 0, 0, NULL);
   Py_DECREF(raw);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("[|%U|]", s);
   Py_DECREF(s);
   return result;
}
static PyObject* llist_str(PListObject* self) {
   PyObject* raw = llist_as_plist(self, NULL);
   PyObject* s; PyObject* result;
   if (!raw) return NULL;
   s = call_seqstr(raw, 60, 1, g_reprlazy_func);
   Py_DECREF(raw);
   if (!s) return NULL;
   result = PyUnicode_FromFormat("[|%U|]", s);
   Py_DECREF(s);
   return result;
}

// Forward-declared for the same reason as ldict_hash above.
static Py_hash_t llist_hash(PListObject* self);

// llist_hash: see ldict_hash's comment -- same rationale (plist_hash walks
// the raw backing trie directly, bypassing any subclass override).
static Py_hash_t llist_hash(PListObject* self) {
   PyObject* it;
   PyObject* tup;
   PyObject* v;
   PyObject* parts;
   Py_hash_t result;
   Py_ssize_t n, i;
   if (self->hashcode != -1) return self->hashcode;
   it = make_unlazy_iter(PListType->tp_iter((PyObject*)self));
   if (!it) return -1;
   parts = PyList_New(0);
   if (!parts) { Py_DECREF(it); return -1; }
   while ((v = PyIter_Next(it))) {
      if (PyList_Append(parts, v) < 0) {
         Py_DECREF(v); Py_DECREF(it); Py_DECREF(parts); return -1;
      }
      Py_DECREF(v);
   }
   Py_DECREF(it);
   if (PyErr_Occurred()) { Py_DECREF(parts); return -1; }
   n = PyList_GET_SIZE(parts);
   tup = PyTuple_New(n);
   if (!tup) { Py_DECREF(parts); return -1; }
   for (i = 0; i < n; ++i) {
      PyObject* item = PyList_GET_ITEM(parts, i);
      Py_INCREF(item);
      PyTuple_SET_ITEM(tup, i, item);
   }
   Py_DECREF(parts);
   result = PyObject_Hash(tup);
   Py_DECREF(tup);
   if (result == -1) return -1;
   result += 1; // matches plist_hash's own tuple-hash-plus-1 convention.
   if (result == -1) result = -2;
   self->hashcode = result;
   return result;
}

static PyMethodDef llist_methods[] = {
   {"is_lazy", (PyCFunction)llist_is_lazy, METH_O,
    "Returns True if the given index is mapped to a lazy value."},
   {"is_ready", (PyCFunction)llist_is_ready, METH_O,
    "Returns True if the given index's value can be returned without further computation."},
   {"ready_all", (PyCFunction)llist_ready_all, METH_NOARGS,
    "Caches all lazy items then returns the llist."},
   {"as_plist", (PyCFunction)llist_as_plist, METH_NOARGS,
    "Returns a plist with the llist's lazy values left uncached."},
   {"__holdlazy__", (PyCFunction)llist_as_plist, METH_NOARGS, NULL},
   {"getlazy", (PyCFunction)llist_getlazy, METH_O,
    "Like indexing, but returns lazy objects instead of their results."},
   {"clear", (PyCFunction)llist_clear, METH_NOARGS,
    "Returns the empty llist."},
   {"transient", (PyCFunction)llist_transient, METH_NOARGS,
    "Efficiently copies the llist into a tllist and returns the tllist."},
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
      "A persistent lazy list type: identical to plist, except that any "
      "lazy value contained in it is dereferenced prior to being returned "
      "by item extraction or iteration.")},
   {0, NULL}
};
static PyType_Spec llist_spec = {
   .name = "pcollections._c.lazy.llist",
   .basicsize = sizeof(PListObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = llist_slots,
};

static PyObject* tllist_subscript(TListObject* self, PyObject* key) {
   PyObject* v = TListType->tp_as_mapping->mp_subscript((PyObject*)self, key);
   return unlazy_owned(v);
}
static PyObject* tllist_item(TListObject* self, Py_ssize_t i) {
   PyObject* v = TListType->tp_as_sequence->sq_item((PyObject*)self, i);
   return unlazy_owned(v);
}
static PyObject* tllist_iter(TListObject* self) {
   return make_unlazy_iter(TListType->tp_iter((PyObject*)self));
}
static PyObject* tllist_getlazy(PyObject* self, PyObject* index_obj) {
   return TListType->tp_as_mapping->mp_subscript(self, index_obj); // raw.
}
static PyObject* tllist_persistent(TListObject* self, PyObject* Py_UNUSED(ignored)) {
   PListObject* p;
   if (self->length == 0) {
      Py_INCREF(g_llist_empty);
      return (PyObject*)g_llist_empty;
   }
   if (self->orig) {
      Py_INCREF(self->orig);
      return self->orig;
   }
   lazyfile_fat_freeze(self->root);
   p = (PListObject*)LListType->tp_alloc(LListType, 0);
   if (!p) return NULL;
   trienode_incref(self->root);
   p->root = self->root;
   p->start = self->start;
   p->length = self->length;
   p->hashcode = -1;
   return (PyObject*)p;
}
// See ldict_gc_traverse/ldict_gc_clear's comment.
static int tllist_gc_traverse(PyObject* self, visitproc visit, void* arg) {
   return TListType->tp_traverse(self, visit, arg);
}
static int tllist_gc_clear(PyObject* self) {
   return TListType->tp_clear(self);
}
static PyMethodDef tllist_methods[] = {
   {"getlazy", (PyCFunction)tllist_getlazy, METH_O,
    "Like indexing, but returns lazy objects instead of their results."},
   {"persistent", (PyCFunction)tllist_persistent, METH_NOARGS,
    "Efficiently copies the tllist into an llist and returns the llist."},
   {NULL, NULL, 0, NULL}
};
static PyType_Slot tllist_slots[] = {
   // No Py_tp_new: tllist has no __new__ override in the reference (unlike
   // llist/ldict/tldict) -- it inherits tlist's tp_new unchanged, including
   // tlist's own isinstance(arg, plist)-based construction logic (which
   // will happily accept an llist argument too, raw-sharing its root
   // without dereferencing -- see list.c's tlist_new comment).
   {Py_tp_traverse, (void*)tllist_gc_traverse},
   {Py_tp_clear, (void*)tllist_gc_clear},
   {Py_mp_subscript, (void*)tllist_subscript},
   {Py_sq_item, (void*)tllist_item},
   {Py_tp_iter, (void*)tllist_iter},
   {Py_tp_methods, (void*)tllist_methods},
   {Py_tp_doc, (void*)PyDoc_STR(
      "A transient lazy list type: identical to tlist, except that item "
      "access automatically returns the reified values of lazy elements "
      "instead of the lazy objects themselves.")},
   {0, NULL}
};
static PyType_Spec tllist_spec = {
   .name = "pcollections._c.lazy.tllist",
   .basicsize = sizeof(TListObject),
   .itemsize = 0,
   .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
   .slots = tllist_slots,
};


//=============================================================================
// unlazy / reprlazy / strlazy / holdlazy -- see _lazy.py for the reference.

static PyObject* mod_unlazy(PyObject* self, PyObject* obj) {
   (void)self;
   if ((PyObject*)Py_TYPE(obj) == (PyObject*)LazyType) {
      return PyObject_CallFunctionObjArgs(obj, NULL);
   }
   Py_INCREF(obj);
   return obj;
}
static PyObject* mod_reprlazy(PyObject* self, PyObject* obj) {
   (void)self;
   if ((PyObject*)Py_TYPE(obj) == (PyObject*)LazyType) {
      return PyUnicode_FromString("<lazy>");
   }
   return PyObject_Repr(obj);
}
// Named separately (rather than only inline inside lazy_module_methods
// below) so PyInit_lazy can wrap it into a standalone callable object
// (g_reprlazy_func) to pass as util.seqstr's `tostr` argument -- see
// ldict_str/tldict_str/llist_str above, which need exactly the reference's
// `tostr=reprlazy` behavior.
static PyMethodDef reprlazy_methoddef = {
   "reprlazy", (PyCFunction)mod_reprlazy, METH_O,
   "Returns '<lazy>' if obj is a lazy object, otherwise repr(obj)."};
static PyObject* mod_strlazy(PyObject* self, PyObject* obj) {
   (void)self;
   if ((PyObject*)Py_TYPE(obj) == (PyObject*)LazyType) {
      return PyUnicode_FromString("<lazy>");
   }
   return PyObject_Str(obj);
}
static PyObject* mod_holdlazy(PyObject* self, PyObject* args, PyObject* kwds) {
   PyObject* obj;
   int require_lazy = 0;
   PyObject* result;
   static char* kwlist[] = {"obj", "require_lazy", NULL};
   (void)self;
   if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p", kwlist,
                                     &obj, &require_lazy))
      return NULL;
   if (PyObject_HasAttrString(obj, "__holdlazy__")) {
      result = PyObject_CallMethod(obj, "__holdlazy__", NULL);
      return result;
   }
   if (require_lazy) {
      PyErr_Format(PyExc_TypeError,
         "__holdlazy__ method not found for type %R", (PyObject*)Py_TYPE(obj));
      return NULL;
   }
   Py_INCREF(obj);
   return obj;
}

static PyMethodDef lazy_module_methods[] = {
   {"unlazy", (PyCFunction)mod_unlazy, METH_O,
    "Returns the cached value of a lazy object, or the object itself if it is not lazy."},
   {"reprlazy", (PyCFunction)mod_reprlazy, METH_O,
    "Returns '<lazy>' if obj is a lazy object, otherwise repr(obj)."},
   {"strlazy", (PyCFunction)mod_strlazy, METH_O,
    "Returns '<lazy>' if obj is a lazy object, otherwise str(obj)."},
   {"holdlazy", (PyCFunction)mod_holdlazy, METH_VARARGS | METH_KEYWORDS,
    "Returns a persistent version of a lazy collection whose lazy values\n"
    "remain unevaluated, by calling its __holdlazy__() method if present."},
   {NULL, NULL, 0, NULL}
};

static PyModuleDef lazy_module = {
   PyModuleDef_HEAD_INIT,
   "pcollections._c.lazy",
   "C implementation of the `lazy` thread-safe deferred-computation cell, "
   "plus its supporting LazyError/lazy_error_unwrap/unlazy/reprlazy/strlazy/"
   "holdlazy helpers.",
   -1,
   lazy_module_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_lazy(void) {
   PyObject* m;
   PyObject* functools_module;

   functools_module = PyImport_ImportModule("functools");
   if (!functools_module) return NULL;
   g_functools_partial_type = PyObject_GetAttrString(functools_module, "partial");
   Py_DECREF(functools_module);
   if (!g_functools_partial_type) return NULL;

   // LazyErrorType: a heap type derived from RuntimeError (via
   // PyErr_NewException, which gives us the usual BaseException machinery
   // -- .args, __traceback__, __context__, __cause__, pickling, etc. -- for
   // free), with __init__/__str__/__repr__ patched to the reference's
   // behavior.
   LazyErrorType = (PyTypeObject*)PyErr_NewException(
      "pcollections._c.lazy.LazyError", PyExc_RuntimeError, NULL);
   if (!LazyErrorType) return NULL;
   LazyErrorType->tp_init = lazyerror_init;
   LazyErrorType->tp_str = lazyerror_str;
   LazyErrorType->tp_repr = lazyerror_repr;

   if (PyType_Ready(&LazyErrorUnwrapperType) < 0) return NULL;
   g_lazy_error_unwrap = LazyErrorUnwrapperType.tp_alloc(&LazyErrorUnwrapperType, 0);
   if (!g_lazy_error_unwrap) return NULL;

   LazyType = (PyTypeObject*)PyType_FromSpec(&lazy_spec);
   if (!LazyType) return NULL;

   if (PyType_Ready(&UnlazyIterType) < 0) return NULL;

   //---------------------------------------------------------------------
   // Import pcollections._c.dict/.list (the base types ldict/tldict/llist/
   // tllist subclass) and collections.abc (for ItemsView/ValuesView), and
   // cache the handful of base-class unbound methods used throughout the
   // functions above.
   {
      PyObject* dict_module = PyImport_ImportModule("pcollections._c.dict");
      PyObject* list_module;
      PyObject* abc_module;
      PyObject* pdict_empty_obj;
      PyObject* plist_empty_obj;

      if (!dict_module) return NULL;
      PDictType = (PyTypeObject*)PyObject_GetAttrString(dict_module, "pdict");
      TDictType = (PyTypeObject*)PyObject_GetAttrString(dict_module, "tdict");
      Py_DECREF(dict_module);
      if (!PDictType || !TDictType) return NULL;

      list_module = PyImport_ImportModule("pcollections._c.list");
      if (!list_module) return NULL;
      PListType = (PyTypeObject*)PyObject_GetAttrString(list_module, "plist");
      TListType = (PyTypeObject*)PyObject_GetAttrString(list_module, "tlist");
      Py_DECREF(list_module);
      if (!PListType || !TListType) return NULL;

      abc_module = PyImport_ImportModule("collections.abc");
      if (!abc_module) return NULL;
      g_ItemsView = PyObject_GetAttrString(abc_module, "ItemsView");
      g_ValuesView = PyObject_GetAttrString(abc_module, "ValuesView");
      Py_DECREF(abc_module);
      if (!g_ItemsView || !g_ValuesView) return NULL;

      g_pdict_get = PyObject_GetAttrString((PyObject*)PDictType, "get");
      g_tdict_get = PyObject_GetAttrString((PyObject*)TDictType, "get");
      g_tdict_pop = PyObject_GetAttrString((PyObject*)TDictType, "pop");
      if (!g_pdict_get || !g_tdict_get || !g_tdict_pop) return NULL;

      {
         PyObject* util_module = PyImport_ImportModule("pcollections.util");
         if (!util_module) return NULL;
         g_seqstr = PyObject_GetAttrString(util_module, "seqstr");
         Py_DECREF(util_module);
         if (!g_seqstr) return NULL;
      }
      g_reprlazy_func = PyCFunction_NewEx(&reprlazy_methoddef, NULL, NULL);
      if (!g_reprlazy_func) return NULL;

      //------------------------------------------------------------------
      // Build LDictType/TLDictType/LListType/TLListType as heap types
      // subclassing PDictType/TDictType/PListType/TListType directly (now
      // that those are all Py_TPFLAGS_BASETYPE-enabled -- see dict.c's/
      // list.c's own header comments on that subclassing support).
      LDictType = build_c_subtype(&ldict_spec, PDictType);
      TLDictType = build_c_subtype(&tldict_spec, TDictType);
      LListType = build_c_subtype(&llist_spec, PListType);
      TLListType = build_c_subtype(&tllist_spec, TListType);
      if (!LDictType || !TLDictType || !LListType || !TLListType) return NULL;

      //------------------------------------------------------------------
      // Bootstrap ldict.empty/llist.empty (plain class attributes, mirroring
      // pdict.empty/plist.empty -- NOT classmethods, unlike tdict/tlist's
      // own "empty" -- see this file's header-comment-adjacent notes on
      // tldict/tllist deliberately having no stored `.empty` at all). Each
      // is built by raw-sharing PDictType.empty's/PListType.empty's already-
      // correctly-constructed (and already non-transient/persistent) empty
      // trie(s) -- this sidesteps ever needing to build a *new* empty trie
      // ourselves (which would require knowing dict.c's/list.c's private
      // leaf-size constants).
      pdict_empty_obj = PyObject_GetAttrString((PyObject*)PDictType, "empty");
      if (!pdict_empty_obj) return NULL;
      g_ldict_empty = (PDictObject*)pdictlike_share(LDictType, (PDictObject*)pdict_empty_obj);
      Py_DECREF(pdict_empty_obj);
      if (!g_ldict_empty) return NULL;
      if (PyDict_SetItemString(LDictType->tp_dict, "empty", (PyObject*)g_ldict_empty) < 0)
         return NULL;
      PyType_Modified(LDictType);

      plist_empty_obj = PyObject_GetAttrString((PyObject*)PListType, "empty");
      if (!plist_empty_obj) return NULL;
      g_llist_empty = (PListObject*)plistlike_share(LListType, (PListObject*)plist_empty_obj);
      Py_DECREF(plist_empty_obj);
      if (!g_llist_empty) return NULL;
      if (PyDict_SetItemString(LListType->tp_dict, "empty", (PyObject*)g_llist_empty) < 0)
         return NULL;
      PyType_Modified(LListType);
   }

   m = PyModule_Create(&lazy_module);
   if (!m) return NULL;

   Py_INCREF(LazyType);
   if (PyModule_AddObject(m, "lazy", (PyObject*)LazyType) < 0) {
      Py_DECREF(LazyType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(LazyErrorType);
   if (PyModule_AddObject(m, "LazyError", (PyObject*)LazyErrorType) < 0) {
      Py_DECREF(LazyErrorType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(g_lazy_error_unwrap);
   if (PyModule_AddObject(m, "lazy_error_unwrap", g_lazy_error_unwrap) < 0) {
      Py_DECREF(g_lazy_error_unwrap); Py_DECREF(m); return NULL;
   }
   Py_INCREF(LDictType);
   if (PyModule_AddObject(m, "ldict", (PyObject*)LDictType) < 0) {
      Py_DECREF(LDictType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TLDictType);
   if (PyModule_AddObject(m, "tldict", (PyObject*)TLDictType) < 0) {
      Py_DECREF(TLDictType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(LListType);
   if (PyModule_AddObject(m, "llist", (PyObject*)LListType) < 0) {
      Py_DECREF(LListType); Py_DECREF(m); return NULL;
   }
   Py_INCREF(TLListType);
   if (PyModule_AddObject(m, "tllist", (PyObject*)TLListType) < 0) {
      Py_DECREF(TLListType); Py_DECREF(m); return NULL;
   }

   return m;
}
