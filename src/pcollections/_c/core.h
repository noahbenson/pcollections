///////////////////////////////////////////////////////////////////////////////
// _c/core.h
// Shared definitions for the pcollections._c._core extension module.
//
// The C backend is a single extension module built from one translation unit
// (_core.c), which includes this header followed by the per-type parts
// (dict.c.h, list.c.h, set.c.h, lazy.c.h). This header holds everything the
// parts share:
//
//  - portable threading shims (a recursive mutex and an atomic int);
//  - the per-interpreter module state (`pcoll_state`) and its lookup;
//  - the canonical empty trie nodes;
//  - small helpers used by more than one part (freezing tries, building the
//    heap types on top of pcollections.abc, calling pcollections.util.seqstr).
//
// Module state and subinterpreters
// --------------------------------
// Every Python object the module creates at import time (types, empty
// singletons, cached imports) lives in a `pcoll_state` owned by the module
// object, so each interpreter gets its own. Code reaches the state for the
// current interpreter with `pcoll_get_state()`, or the shorthand `ST(name)`.
//
// The lookup is keyed by the current interpreter's ID (IDs, unlike
// PyInterpreterState addresses, are never reused within a process): a small
// registry maps each ID to its state, and a thread-local cache makes the
// common case one comparison. A module instance registers its state when it
// executes and unregisters it when freed; each unregistration bumps a global
// generation counter, which invalidates every thread's cache. A module may be
// executed at most once per interpreter (a second attempt raises
// ImportError), which is what makes "the state for this interpreter"
// well-defined. (Some Python versions never free an extension module that a
// destroyed subinterpreter loaded; its entry then stays in the registry,
// harmlessly, since no later interpreter has its ID.)
//
// Heap types are created with PyType_FromModuleAndSpec (3.9+), so each type
// keeps its module, and therefore the state, alive for as long as any
// instance exists. On 3.8, which lacks that API, only one interpreter per
// process may load the module, and its state is never freed.
//
// Trie nodes contain no Python objects of their own, and their reference
// counts are atomic, so the empty-node singletons below are shared by all
// interpreters.

#ifndef PCOLLECTIONS__C_CORE_H
#define PCOLLECTIONS__C_CORE_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdbool.h>
#include <string.h>
#include "uintbits.h"
#include "trie.h"
#include "amt.h"
#include "fat.h"

#ifndef EXTC
#  ifdef __cplusplus
#    define EXTC extern "C"
#  else
#    define EXTC
#  endif
#endif


//=============================================================================
// Portable threading shims.
// A recursive mutex (used by `lazy` and by the state registry) and an atomic
// int. Windows uses CRITICAL_SECTION and the Interlocked intrinsics (MSVC's C
// mode has neither <pthread.h> nor, without /std:c11, <stdatomic.h>);
// everything else uses pthreads and C11 atomics.

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION pcoll_mutex_t;
   typedef volatile LONG pcoll_atomic_flag_t;
   static inline int pcoll_mutex_init_recursive(pcoll_mutex_t* m) {
      // CRITICAL_SECTION is always recursive for its owning thread.
      return InitializeCriticalSectionAndSpinCount(m, 0) ? 0 : -1;
   }
#  define PCOLL_MUTEX_INIT(m) pcoll_mutex_init_recursive(m)
#  define PCOLL_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#  define PCOLL_MUTEX_LOCK(m) EnterCriticalSection(m)
#  define PCOLL_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#  define PCOLL_ATOMIC_INIT(flag, val) (*(flag) = (val))
   // A same-value compare-exchange is a fully fenced read.
#  define PCOLL_ATOMIC_LOAD_ACQUIRE(flag) InterlockedCompareExchange((flag), 0, 0)
#  define PCOLL_ATOMIC_STORE_RELEASE(flag, val) ((void)InterlockedExchange((flag), (val)))
#  define PCOLL_ATOMIC_INCREMENT(flag) ((void)InterlockedIncrement(flag))
#  define PCOLL_THREAD_LOCAL __declspec(thread)
#else
#  include <stdatomic.h>
#  include <pthread.h>
   typedef pthread_mutex_t pcoll_mutex_t;
   typedef atomic_int pcoll_atomic_flag_t;
   static inline int pcoll_mutex_init_recursive(pcoll_mutex_t* m) {
      pthread_mutexattr_t attr;
      int rc = pthread_mutexattr_init(&attr);
      if (rc == 0) rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      if (rc == 0) rc = pthread_mutex_init(m, &attr);
      pthread_mutexattr_destroy(&attr);
      return rc;
   }
#  define PCOLL_MUTEX_INIT(m) pcoll_mutex_init_recursive(m)
#  define PCOLL_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define PCOLL_MUTEX_LOCK(m) pthread_mutex_lock(m)
#  define PCOLL_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#  define PCOLL_ATOMIC_INIT(flag, val) atomic_init((flag), (val))
#  define PCOLL_ATOMIC_LOAD_ACQUIRE(flag) atomic_load_explicit((flag), memory_order_acquire)
#  define PCOLL_ATOMIC_STORE_RELEASE(flag, val) atomic_store_explicit((flag), (val), memory_order_release)
#  define PCOLL_ATOMIC_INCREMENT(flag) ((void)atomic_fetch_add((flag), 1))
#  define PCOLL_THREAD_LOCAL _Thread_local
#endif


//=============================================================================
// Module state.

// The object structs of the types whose singletons the state holds. Their
// full definitions are in the part files.
struct PDictObject;
struct PListObject;
struct PSetObject;

typedef struct pcoll_state {
   // dict.c.h
   PyTypeObject* PDictType;
   PyTypeObject* TDictType;
   PyTypeObject* PDictKeysType;
   PyTypeObject* PDictItemsType;
   PyTypeObject* PDictValuesType;
   PyTypeObject* TDictKeysType;
   PyTypeObject* TDictItemsType;
   PyTypeObject* TDictValuesType;
   PyTypeObject* PDictIterType;
   PyTypeObject* TDictIterType;
   struct PDictObject* g_pdict_empty;
   PyObject* g_dict_dummy;
   PyObject* g_abc_Mapping;
   PyObject* g_abc_Sized;
   // list.c.h
   PyTypeObject* PListType;
   PyTypeObject* TListType;
   PyTypeObject* PListIterType;
   PyTypeObject* TListIterType;
   struct PListObject* g_plist_empty;
   // set.c.h
   PyTypeObject* PSetType;
   PyTypeObject* TSetType;
   PyTypeObject* PSetIterType;
   PyTypeObject* TSetIterType;
   struct PSetObject* g_pset_empty;
   PyObject* g_set_dummy;
   // lazy.c.h
   PyTypeObject* LazyType;
   PyTypeObject* LazyErrorType;
   PyTypeObject* LazyErrorUnwrapperType;
   PyTypeObject* UnlazyIterType;
   PyTypeObject* LDictType;
   PyTypeObject* TLDictType;
   PyTypeObject* LListType;
   PyTypeObject* TLListType;
   struct PDictObject* g_ldict_empty;
   struct PListObject* g_llist_empty;
   PyObject* g_lazy_error_unwrap;
   PyObject* g_functools_partial_type;
   PyObject* g_ItemsView;
   PyObject* g_ValuesView;
   PyObject* g_reprlazy_func;
   PyObject* g_pdict_get;
   PyObject* g_tdict_get;
   PyObject* g_tdict_pop;
   // shared
   PyObject* g_seqstr;
} pcoll_state;

// Applies X to every object field of the state (module m_traverse/m_clear).
#define PCOLL_STATE_FOREACH(X, st) \
   X((st)->PDictType);\
   X((st)->TDictType);\
   X((st)->PDictKeysType);\
   X((st)->PDictItemsType);\
   X((st)->PDictValuesType);\
   X((st)->TDictKeysType);\
   X((st)->TDictItemsType);\
   X((st)->TDictValuesType);\
   X((st)->PDictIterType);\
   X((st)->TDictIterType);\
   X((st)->g_pdict_empty);\
   X((st)->g_dict_dummy);\
   X((st)->g_abc_Mapping);\
   X((st)->g_abc_Sized);\
   X((st)->PListType);\
   X((st)->TListType);\
   X((st)->PListIterType);\
   X((st)->TListIterType);\
   X((st)->g_plist_empty);\
   X((st)->PSetType);\
   X((st)->TSetType);\
   X((st)->PSetIterType);\
   X((st)->TSetIterType);\
   X((st)->g_pset_empty);\
   X((st)->g_set_dummy);\
   X((st)->LazyType);\
   X((st)->LazyErrorType);\
   X((st)->LazyErrorUnwrapperType);\
   X((st)->UnlazyIterType);\
   X((st)->LDictType);\
   X((st)->TLDictType);\
   X((st)->LListType);\
   X((st)->TLListType);\
   X((st)->g_ldict_empty);\
   X((st)->g_llist_empty);\
   X((st)->g_lazy_error_unwrap);\
   X((st)->g_functools_partial_type);\
   X((st)->g_ItemsView);\
   X((st)->g_ValuesView);\
   X((st)->g_reprlazy_func);\
   X((st)->g_pdict_get);\
   X((st)->g_tdict_get);\
   X((st)->g_tdict_pop);\
   X((st)->g_seqstr);

// The registry: one entry per interpreter that has executed the module.
typedef struct {
   int64_t interp_id;
   pcoll_state* state;
} pcoll_registry_entry;

static pcoll_mutex_t g_registry_lock;
static int g_registry_lock_ready = 0;
static pcoll_registry_entry* g_registry = NULL;
static Py_ssize_t g_registry_len = 0;
static Py_ssize_t g_registry_cap = 0;
static pcoll_atomic_flag_t g_registry_generation;

static PCOLL_THREAD_LOCAL int64_t tl_interp_id = -1;
static PCOLL_THREAD_LOCAL pcoll_state* tl_state = NULL;
static PCOLL_THREAD_LOCAL int tl_generation = -1;

static inline int64_t pcoll_current_interp_id(void) {
#if PY_VERSION_HEX >= 0x03090000
   return PyInterpreterState_GetID(PyInterpreterState_Get());
#else
   return PyInterpreterState_GetID(PyThreadState_Get()->interp);
#endif
}

// Must be called (at least once) before any other registry function. The
// first call happens in PyInit__core, which the import system serializes per
// interpreter; the flag is re-checked under no lock only after it is set.
static int pcoll_registry_init(void) {
   if (g_registry_lock_ready) return 0;
   if (PCOLL_MUTEX_INIT(&g_registry_lock) != 0) {
      PyErr_SetString(PyExc_RuntimeError,
                      "pcollections: could not create the registry lock");
      return -1;
   }
   PCOLL_ATOMIC_INIT(&g_registry_generation, 0);
   g_registry_lock_ready = 1;
   return 0;
}

// Looks up the state for interpreter `id`; the registry lock must be held.
static pcoll_state* pcoll_registry_find_locked(int64_t id) {
   Py_ssize_t i;
   for (i = 0; i < g_registry_len; ++i)
      if (g_registry[i].interp_id == id)
         return g_registry[i].state;
   return NULL;
}

// Registers `state` for the current interpreter. Fails with ImportError if
// the interpreter already has one.
static int pcoll_registry_add(pcoll_state* state) {
   int64_t id = pcoll_current_interp_id();
   int rc = 0;
   if (id < 0) return -1;
   PCOLL_MUTEX_LOCK(&g_registry_lock);
   if (pcoll_registry_find_locked(id)) {
      PyErr_SetString(PyExc_ImportError,
         "pcollections._c._core can only be loaded once per interpreter");
      rc = -1;
   }
#if PY_VERSION_HEX < 0x03090000
   else if (g_registry_len > 0) {
      PyErr_SetString(PyExc_ImportError,
         "pcollections._c._core supports only one interpreter per process "
         "on Python 3.8");
      rc = -1;
   }
#endif
   else {
      if (g_registry_len == g_registry_cap) {
         Py_ssize_t newcap = g_registry_cap ? 2 * g_registry_cap : 8;
         pcoll_registry_entry* grown = (pcoll_registry_entry*)realloc(
            g_registry, (size_t)newcap * sizeof(pcoll_registry_entry));
         if (!grown) {
            PyErr_NoMemory();
            rc = -1;
         } else {
            g_registry = grown;
            g_registry_cap = newcap;
         }
      }
      if (rc == 0) {
         g_registry[g_registry_len].interp_id = id;
         g_registry[g_registry_len].state = state;
         ++g_registry_len;
      }
   }
   PCOLL_MUTEX_UNLOCK(&g_registry_lock);
   return rc;
}

// Removes `state` from the registry (if present) and invalidates every
// thread's cached lookup.
static void pcoll_registry_remove(pcoll_state* state) {
   Py_ssize_t i;
   if (!g_registry_lock_ready) return;
   PCOLL_MUTEX_LOCK(&g_registry_lock);
   for (i = 0; i < g_registry_len; ++i) {
      if (g_registry[i].state == state) {
         g_registry[i] = g_registry[g_registry_len - 1];
         --g_registry_len;
         PCOLL_ATOMIC_INCREMENT(&g_registry_generation);
         break;
      }
   }
   PCOLL_MUTEX_UNLOCK(&g_registry_lock);
}

// Returns the module state for the current interpreter. Never fails while
// any pcollections object or type exists in this interpreter (they keep the
// module, and so its registration, alive).
static pcoll_state* pcoll_get_state_slow(int64_t id, int gen) {
   pcoll_state* st;
   PCOLL_MUTEX_LOCK(&g_registry_lock);
   st = pcoll_registry_find_locked(id);
   PCOLL_MUTEX_UNLOCK(&g_registry_lock);
   tl_interp_id = id;
   tl_state = st;
   tl_generation = gen;
   return st;
}
static inline pcoll_state* pcoll_get_state(void) {
   int64_t id = pcoll_current_interp_id();
   int gen = (int)PCOLL_ATOMIC_LOAD_ACQUIRE(&g_registry_generation);
   if (id == tl_interp_id && gen == tl_generation && tl_state)
      return tl_state;
   return pcoll_get_state_slow(id, gen);
}
#define ST(name) (pcoll_get_state()->name)


//=============================================================================
// Canonical empty trie nodes.
// fat_empty()/amt_empty() (declared in fat.h/amt.h) return a new reference to
// the shared empty node for a given leaf size. The nodes are created by
// pcoll_create_empty_tries() at module execution, under the registry lock,
// for every leaf size the module uses; a request for any other size creates
// its node on demand under the same lock.

static Trie_t g_fat_empty_singletons[256];
static Trie_t g_amt_empty_singletons[256];

static Trie_t pcoll_make_fat_empty(uint8_t leafsize) {
   Trie_t t = fatnode_new(0, leafsize, FAT_MAX_DEPTH, false);
   t->header.bits = 0;
   trienode_incref(t);  // permanent hold: the singleton is never freed.
   return t;
}
static Trie_t pcoll_make_amt_empty(uint8_t leafsize) {
   // amtnode_new() (not amtnode_alloc()) so the header is initialized.
   Trie_t t = amtnode_new(0, leafsize, AMT_MAX_DEPTH, 0, false);
   t->header.bits = 0;
   trienode_incref(t);  // permanent hold: the singleton is never freed.
   return t;
}

Trie_t fat_empty(uint8_t leafsize) {
   Trie_t t = g_fat_empty_singletons[leafsize];
   if (!t) {
      PCOLL_MUTEX_LOCK(&g_registry_lock);
      if (!g_fat_empty_singletons[leafsize])
         g_fat_empty_singletons[leafsize] = pcoll_make_fat_empty(leafsize);
      t = g_fat_empty_singletons[leafsize];
      PCOLL_MUTEX_UNLOCK(&g_registry_lock);
   }
   trienode_incref(t);
   return t;
}
Trie_t amt_empty(uint8_t leafsize) {
   Trie_t t = g_amt_empty_singletons[leafsize];
   if (!t) {
      PCOLL_MUTEX_LOCK(&g_registry_lock);
      if (!g_amt_empty_singletons[leafsize])
         g_amt_empty_singletons[leafsize] = pcoll_make_amt_empty(leafsize);
      t = g_amt_empty_singletons[leafsize];
      PCOLL_MUTEX_UNLOCK(&g_registry_lock);
   }
   trienode_incref(t);
   return t;
}


//=============================================================================
// Freezing.
// fat_freeze()/amt_freeze() mark `a` and every transient node below it as
// persistent. A transient calls this when it hands its tries to a new
// persistent object; the transient stays usable, and its next mutation of a
// frozen node copies it first.

static void fat_freeze(Trie_t a) {
   if (!trie_is_transient(a))
      return;  // persistent nodes only ever have persistent children.
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
// Leaf reference-counting callbacks shared by several parts.

// Leaves that are plain integers (hash-table indices).
static void noop_incref(void* v) { (void)v; }
static void noop_decref(void* v) { (void)v; }
// Leaves that are a single PyObject*.
static void pyobj_incref(void* v) { Py_INCREF(*(PyObject**)v); }
static void pyobj_decref(void* v) { Py_DECREF(*(PyObject**)v); }


//=============================================================================
// GC helpers.

// Instances of heap types own a reference to their type, and (from 3.9 on)
// must report it from tp_traverse so the collector can free types, and with
// them the module state, when an interpreter shuts down. On 3.8 the generic
// subclass traversal already reports it, so reporting it again would
// over-count.
#if PY_VERSION_HEX >= 0x03090000
#  define PCOLL_VISIT_TYPE(self) Py_VISIT((PyObject*)Py_TYPE(self))
#else
#  define PCOLL_VISIT_TYPE(self) ((void)0)
#endif


//=============================================================================
// Type-construction helpers.

// Creates a heap type from `spec` with the given bases (NULL for object),
// associated with module `m` where the Python version supports it.
static PyTypeObject* pcoll_new_type(PyObject* m, PyType_Spec* spec,
                                    PyObject* bases) {
#if PY_VERSION_HEX >= 0x03090000
   return (PyTypeObject*)PyType_FromModuleAndSpec(m, spec, bases);
#else
   (void)m;
   return (PyTypeObject*)(bases ? PyType_FromSpecWithBases(spec, bases)
                                : PyType_FromSpec(spec));
#endif
}

// Creates a helper type (an iterator, say) that Python code must not
// instantiate directly. Its spec's flags should include
// PCOLL_TPFLAGS_INTERNAL.
#if PY_VERSION_HEX >= 0x030A0000
#  define PCOLL_TPFLAGS_INTERNAL Py_TPFLAGS_DISALLOW_INSTANTIATION
#else
#  define PCOLL_TPFLAGS_INTERNAL 0
#endif
static PyTypeObject* pcoll_new_internal_type(PyObject* m, PyType_Spec* spec) {
   PyTypeObject* t = pcoll_new_type(m, spec, NULL);
#if PY_VERSION_HEX < 0x030A0000
   if (t) t->tp_new = NULL;
#endif
   return t;
}

// Creates a heap type from `spec` whose single base is the class named
// `base_name` in pcollections.abc (one of the plain, non-ABCMeta mixins; see
// pcollections/abc/_core.py for why the C types can't use the ABCs
// directly). If `inherit_richcompare` is true, the new type takes the base's
// tp_richcompare: listing the slot in the spec instead would make CPython add
// __eq__/__ne__ wrappers to the new type that dispatch back to the same
// function, which recurses forever.
static PyTypeObject* build_abc_subtype(PyObject* m, PyType_Spec* spec,
                                       PyObject* abc_module,
                                       const char* base_name,
                                       int inherit_richcompare) {
   PyObject* base;
   PyObject* bases;
   PyTypeObject* result;
   base = PyObject_GetAttrString(abc_module, base_name);
   if (!base) return NULL;
   if (!PyType_Check(base)) {
      Py_DECREF(base);
      PyErr_Format(PyExc_TypeError, "pcollections.abc.%s is not a type",
                   base_name);
      return NULL;
   }
   bases = PyTuple_Pack(1, base);
   if (!bases) { Py_DECREF(base); return NULL; }
   result = pcoll_new_type(m, spec, bases);
   Py_DECREF(bases);
   if (result && inherit_richcompare)
      result->tp_richcompare = ((PyTypeObject*)base)->tp_richcompare;
   Py_DECREF(base);
   return result;
}

// Registers `concrete` as a virtual subclass of the ABC `abc_cls`, so that
// isinstance()/issubclass() checks against it (and, transitively, against
// the stdlib ABCs it derives from) succeed.
static int register_as_virtual_subclass(PyObject* abc_cls,
                                        PyTypeObject* concrete) {
   PyObject* result = PyObject_CallMethod(abc_cls, "register", "O",
                                          (PyObject*)concrete);
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

// Adds `obj` to module `m` under `name` (borrowing `obj`).
static int pcoll_module_add(PyObject* m, const char* name, PyObject* obj) {
   Py_INCREF(obj);
   if (PyModule_AddObject(m, name, obj) < 0) {
      Py_DECREF(obj);
      return -1;
   }
   return 0;
}

// Sets `name` on a type's dict (used for class-level attributes such as
// pdict.empty).
static int pcoll_type_setattr(PyTypeObject* type, const char* name,
                              PyObject* value) {
   if (PyDict_SetItemString(type->tp_dict, name, value) < 0) return -1;
   PyType_Modified(type);
   return 0;
}


//=============================================================================
// Calls pcollections.util.seqstr(seq, maxlen=maxlen, tostr=tostr). `maxlen`
// is passed only when `has_maxlen` is true and `tostr` only when non-NULL,
// matching seqstr's own defaults otherwise.

static PyObject* call_seqstr(PyObject* seq, long maxlen, int has_maxlen,
                             PyObject* tostr) {
   PyObject* args;
   PyObject* kwargs;
   PyObject* result;
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
   result = PyObject_Call(ST(g_seqstr), args, kwargs);
   Py_DECREF(args);
   Py_DECREF(kwargs);
   return result;
}

#endif // PCOLLECTIONS__C_CORE_H
