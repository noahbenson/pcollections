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
// FAT trie nodes are Python objects (see trie.h), so their types and the
// canonical empty FAT nodes are part of the state. AMT nodes are plain C
// memory holding no Python objects, so the canonical empty AMT nodes are
// shared by all interpreters.

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
   // Sets the flag from 0 to 1; true if it was 0.
#  define PCOLL_ATOMIC_ACQUIRE(flag) (InterlockedCompareExchange((flag), 1, 0) == 0)
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
   static inline bool pcoll_atomic_acquire(pcoll_atomic_flag_t* flag) {
      int expected = 0;
      return atomic_compare_exchange_strong(flag, &expected, 1);
   }
#  define PCOLL_ATOMIC_ACQUIRE(flag) pcoll_atomic_acquire(flag)
#  define PCOLL_THREAD_LOCAL _Thread_local
#endif


//=============================================================================
// Free-threaded builds.
// Without the GIL, every method of a transient type runs inside a critical
// section on the object (as the builtin list and dict do), and iterators lock
// themselves and their collection. PCOLL_LOCKED0/1/2 define a function
// `name` that calls `inner` inside such a section; with the GIL they reduce
// to a plain call. Persistent objects are immutable apart from their cached
// hash, which is read and written atomically.

#ifdef Py_GIL_DISABLED
#  define PCOLL_BEGIN_LOCK(o) Py_BEGIN_CRITICAL_SECTION(o)
#  define PCOLL_END_LOCK() Py_END_CRITICAL_SECTION()
#  define PCOLL_BEGIN_LOCK2(a, b) Py_BEGIN_CRITICAL_SECTION2(a, b)
#  define PCOLL_END_LOCK2() Py_END_CRITICAL_SECTION2()
#  define PCOLL_HASH_LOAD(field) _Py_atomic_load_ssize_relaxed(&(field))
#  define PCOLL_HASH_STORE(field, v) _Py_atomic_store_ssize_relaxed(&(field), (v))
#else
#  define PCOLL_BEGIN_LOCK(o) {
#  define PCOLL_END_LOCK() }
#  define PCOLL_BEGIN_LOCK2(a, b) {
#  define PCOLL_END_LOCK2() }
#  define PCOLL_HASH_LOAD(field) (field)
#  define PCOLL_HASH_STORE(field, v) ((field) = (v))
#endif

#define PCOLL_LOCKED0(R, name, inner, T0)                               \
   static R name(T0 a0) {                                                \
      R r;                                                               \
      PCOLL_BEGIN_LOCK((PyObject*)a0);                                   \
      r = inner(a0);                                                     \
      PCOLL_END_LOCK();                                                  \
      return r;                                                          \
   }
#define PCOLL_LOCKED1(R, name, inner, T0, T1)                           \
   static R name(T0 a0, T1 a1) {                                         \
      R r;                                                               \
      PCOLL_BEGIN_LOCK((PyObject*)a0);                                   \
      r = inner(a0, a1);                                                 \
      PCOLL_END_LOCK();                                                  \
      return r;                                                          \
   }
#define PCOLL_LOCKED2(R, name, inner, T0, T1, T2)                       \
   static R name(T0 a0, T1 a1, T2 a2) {                                  \
      R r;                                                               \
      PCOLL_BEGIN_LOCK((PyObject*)a0);                                   \
      r = inner(a0, a1, a2);                                             \
      PCOLL_END_LOCK();                                                  \
      return r;                                                          \
   }


//=============================================================================
// Transient guards.
// Transients are for use by one thread at a time. They must not be corrupted
// when that rule is broken, or when user code that runs during an operation
// (a key's __hash__ or __eq__, an object's __del__) touches the transient
// being operated on. Every transient therefore carries a guard:
//
//  - `busy` is set for the duration of each modification. A modification
//    that finds it already set (reentrantly, or from another thread) raises
//    RuntimeError instead of proceeding.
//  - `version` changes whenever the contents change, and `keyversion`
//    whenever the set of keys (or, for lists, the positions) changes. A
//    lookup that calls user code checks `version` afterward and raises
//    RuntimeError if the transient changed underneath it; iterators use both
//    counters to detect changes (see the part files).
//
// The busy flag is atomic so that it also catches modifications from
// another thread, including in free-threaded builds, where a critical section
// is released whenever its thread blocks.

typedef struct {
   pcoll_atomic_flag_t busy;
   uint64_t version;
   uint64_t keyversion;
} pcoll_tguard;

// Starts a modification of `self`; returns -1 (with RuntimeError set) if one
// is already in progress.
static inline int tguard_enter(pcoll_tguard* g, PyObject* self) {
   if (PCOLL_ATOMIC_ACQUIRE(&g->busy)) return 0;
   PyErr_Format(PyExc_RuntimeError,
                "%.200s was modified during another modification of it"
                " (transients are not safe for concurrent or reentrant"
                " modification)", Py_TYPE(self)->tp_name);
   return -1;
}
static inline void tguard_exit(pcoll_tguard* g) {
   PCOLL_ATOMIC_STORE_RELEASE(&g->busy, 0);
}
// Fails like tguard_enter() if a modification is in progress, without
// starting one (for operations such as persistent() that must not observe a
// half-finished modification).
static inline int tguard_check(pcoll_tguard* g, PyObject* self) {
   if (!PCOLL_ATOMIC_LOAD_ACQUIRE(&g->busy)) return 0;
   PyErr_Format(PyExc_RuntimeError,
                "%.200s was used during a modification of it",
                Py_TYPE(self)->tp_name);
   return -1;
}
static inline void tguard_changed(pcoll_tguard* g) {
   g->version++;
}
static inline void tguard_keys_changed(pcoll_tguard* g) {
   g->version++;
   g->keyversion++;
}
// Raises the error for a lookup whose collection changed during a call to
// user code.
static inline int tguard_lookup_error(const char* tp_name) {
   PyErr_Format(PyExc_RuntimeError, "%.200s changed during a lookup",
                tp_name);
   return -1;
}

// In-place FAT updates of a transient's root. tfat_setitem()/tfat_delitem()
// may release the old root; the collection keeps pointing at it until the
// call returns, and releasing it can run arbitrary code (__del__) that looks
// at the collection. These keep the old root alive until the collection
// points at the new one.
static inline void tfat_setitem_at(Trie_t* slot, trieint_t key, void* val,
                                   void (*leaf_incref)(void*),
                                   void (*leaf_decref)(void*)) {
   Trie_t old = *slot;
   trienode_incref(old);
   *slot = tfat_setitem(old, key, val, leaf_incref, leaf_decref);
   fatnode_decref(old, leaf_decref);
}
static inline void tfat_delitem_at(Trie_t* slot, trieint_t key,
                                   void (*leaf_incref)(void*),
                                   void (*leaf_decref)(void*)) {
   Trie_t old = *slot;
   trienode_incref(old);
   *slot = tfat_delitem(old, key, leaf_incref, leaf_decref);
   fatnode_decref(old, leaf_decref);
}


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
   // lazy.c.h
   PyTypeObject* LazyType;
   PyTypeObject* LazyErrorType;
   PyTypeObject* UnlazyIterType;
   PyTypeObject* LDictType;
   PyTypeObject* TLDictType;
   PyTypeObject* LListType;
   PyTypeObject* TLListType;
   struct PDictObject* g_ldict_empty;
   struct PListObject* g_llist_empty;
   PyObject* g_lazy_error_unwrap;
   PyObject* g_make_lazy_error;
   PyObject* g_capture_stack;
   PyObject* g_ready_lazy;
   PyObject* g_str_trace;
   PyObject* g_ItemsView;
   PyObject* g_ValuesView;
   PyObject* g_reprlazy_func;
   PyObject* g_tdict_pop;
   // trie nodes: one FAT node type per cell size (1, 2, or 3 pointers),
   // and the canonical empty FAT for each leaf size.
   PyTypeObject* FatNode1Type;
   PyTypeObject* FatNode2Type;
   PyTypeObject* FatNode3Type;
   struct TrieData* g_fat_empty1;
   struct TrieData* g_fat_empty2;
   struct TrieData* g_fat_empty3;
   // shared
   PyObject* g_seqstr;
} pcoll_state;

// Whether `obj` is a lazy collection (lazy.c.h).
static int pcoll_holds_lazy(PyObject* obj);

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
   X((st)->LazyType);\
   X((st)->LazyErrorType);\
   X((st)->UnlazyIterType);\
   X((st)->LDictType);\
   X((st)->TLDictType);\
   X((st)->LListType);\
   X((st)->TLListType);\
   X((st)->g_ldict_empty);\
   X((st)->g_llist_empty);\
   X((st)->g_lazy_error_unwrap);\
   X((st)->g_make_lazy_error);\
   X((st)->g_capture_stack);\
   X((st)->g_ready_lazy);\
   X((st)->g_str_trace);\
   X((st)->g_ItemsView);\
   X((st)->g_ValuesView);\
   X((st)->g_reprlazy_func);\
   X((st)->g_tdict_pop);\
   X((st)->FatNode1Type);\
   X((st)->FatNode2Type);\
   X((st)->FatNode3Type);\
   X((st)->g_fat_empty1);\
   X((st)->g_fat_empty2);\
   X((st)->g_fat_empty3);\
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

//=============================================================================
// FAT node types.
// FAT nodes are instances of three internal heap types, one per cell size (a
// branch's cells, and a list twig's leaves, are one pointer; a set twig's
// leaves are two; a dict twig's leaves are three). The types share the
// functions below, which find a twig's references using fatleaf_nrefs().

static PyTypeObject* pcoll_fat_nodetype(size_t cellsize) {
   pcoll_state* st = pcoll_get_state();
   switch (cellsize / sizeof(void*)) {
      case 1: return st->FatNode1Type;
      case 2: return st->FatNode2Type;
      case 3: return st->FatNode3Type;
      default:
         Py_FatalError("pcollections: unsupported FAT cell size");
         return NULL;
   }
}

// Releases the references held by the `bits` cells of `cells`, which belong
// to a node of the given depth and leaf size.
static void fatnode_release(uint8_t depth, uint8_t leafsize, triebits_t bits,
                            const char* cells) {
   triebits_t bi;
   while (bits) {
      bi = ctz_triebits(bits);
      bits &= bits - 1;
      if (depth == FAT_MAX_DEPTH) {
         PyObject* const* refs = (PyObject* const*)(cells + bi * leafsize);
         int i, n = fatleaf_nrefs(leafsize);
         for (i = 0; i < n; ++i)
            Py_XDECREF(refs[i]);
      } else {
         Py_DECREF(((PyObject* const*)cells)[bi]);
      }
   }
}

static int fatnode_traverse(PyObject* self, visitproc visit, void* arg) {
   Trie_t t = (Trie_t)self;
   triebits_t bi;
   PCOLL_VISIT_TYPE(self);
   if (fatnode_is_twig(t)) {
      uint8_t ls = t->header.leafsize;
      int i, n = fatleaf_nrefs(ls);
      for (bi = trienode_first_bitindex(t); bi < FAT_CELLS;
           bi = trienode_next_bitindex(t, bi)) {
         PyObject** refs = (PyObject**)trienode_leaf(t, bi);
         for (i = 0; i < n; ++i)
            Py_VISIT(refs[i]);
      }
   } else {
      for (bi = trienode_first_bitindex(t); bi < FAT_CELLS;
           bi = trienode_next_bitindex(t, bi))
         Py_VISIT((PyObject*)trienode_subt(t, bi));
   }
   return 0;
}

// Empties the node (breaking reference cycles), then releases what it held.
static int fatnode_clear(PyObject* self) {
   Trie_t t = (Trie_t)self;
   size_t nbytes = (size_t)FAT_CELLS * fatnode_cellsize(t);
   char saved[FAT_CELLS * 3 * sizeof(void*)];
   triebits_t bits = t->header.bits;
   if (!bits) return 0;
   memcpy(saved, t->cells, nbytes);
   t->header.bits = 0;
   memset(t->cells, 0, nbytes);
   fatnode_release(t->header.depth, t->header.leafsize, bits, saved);
   return 0;
}

static void fatnode_dealloc(PyObject* self) {
   PyTypeObject* tp = Py_TYPE(self);
   Trie_t t = (Trie_t)self;
   PyObject_GC_UnTrack(self);
   // Releasing a node can release its children in turn; the trashcan keeps
   // deeply nested structures from exhausting the C stack.
   Py_TRASHCAN_BEGIN(self, fatnode_dealloc)
   fatnode_release(t->header.depth, t->header.leafsize, t->header.bits,
                   t->cells);
   PyObject_GC_Del(self);
   Py_DECREF(tp);
   Py_TRASHCAN_END
}

static PyType_Slot fatnode_slots[] = {
   {Py_tp_dealloc, (void*)fatnode_dealloc},
   {Py_tp_traverse, (void*)fatnode_traverse},
   {Py_tp_clear, (void*)fatnode_clear},
   {0, NULL}
};
#define PCOLL_FATNODE_SPEC(n)                                              \
   {                                                                       \
      .name = "pcollections._c._core.FATNode" #n,                          \
      .basicsize = (int)(sizeof(struct TrieData)                           \
                         + (size_t)FAT_CELLS * (n) * sizeof(void*)),       \
      .itemsize = 0,                                                       \
      .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC                     \
               | PCOLL_TPFLAGS_INTERNAL,                                   \
      .slots = fatnode_slots,                                              \
   }
static PyType_Spec fatnode1_spec = PCOLL_FATNODE_SPEC(1);
static PyType_Spec fatnode2_spec = PCOLL_FATNODE_SPEC(2);
static PyType_Spec fatnode3_spec = PCOLL_FATNODE_SPEC(3);


//=============================================================================
// Canonical empty trie nodes.
// fat_empty()/amt_empty() (declared in fat.h/amt.h) return a new reference to
// the canonical empty node for a leaf size. The empty FAT nodes belong to the
// interpreter's state (see pcoll_exec_trie_types()); they are tracked by the
// collector so that the references they hold to their types are visible.
// The empty AMT nodes are process-wide, created on first use under the
// registry lock, and never freed.

static Trie_t g_amt_empty_singletons[256];

Trie_t fat_empty(uint8_t leafsize) {
   pcoll_state* st = pcoll_get_state();
   Trie_t t;
   switch (leafsize / sizeof(void*)) {
      case 1: t = st->g_fat_empty1; break;
      case 2: t = st->g_fat_empty2; break;
      case 3: t = st->g_fat_empty3; break;
      default:
         Py_FatalError("pcollections: unsupported FAT leaf size");
         return NULL;
   }
   trienode_incref(t);
   return t;
}

Trie_t amt_empty(uint8_t leafsize) {
   Trie_t t = g_amt_empty_singletons[leafsize];
   if (!t) {
      PCOLL_MUTEX_LOCK(&g_registry_lock);
      if (!g_amt_empty_singletons[leafsize]) {
         Trie_t e = amtnode_new(0, leafsize, AMT_MAX_DEPTH, 0, false);
         trienode_incref(e);  // permanent hold: never freed.
         g_amt_empty_singletons[leafsize] = e;
      }
      t = g_amt_empty_singletons[leafsize];
      PCOLL_MUTEX_UNLOCK(&g_registry_lock);
   }
   trienode_incref(t);
   return t;
}

// Creates this interpreter's FAT node types and empty FAT nodes.
static int pcoll_exec_trie_types(PyObject* m, pcoll_state* st) {
   if (!(st->FatNode1Type = pcoll_new_internal_type(m, &fatnode1_spec)) ||
       !(st->FatNode2Type = pcoll_new_internal_type(m, &fatnode2_spec)) ||
       !(st->FatNode3Type = pcoll_new_internal_type(m, &fatnode3_spec)))
      return -1;
   st->g_fat_empty1 = fatnode_new(0, (uint8_t)sizeof(void*),
                                  FAT_MAX_DEPTH, false);
   st->g_fat_empty2 = fatnode_new(0, (uint8_t)(2 * sizeof(void*)),
                                  FAT_MAX_DEPTH, false);
   st->g_fat_empty3 = fatnode_new(0, (uint8_t)(3 * sizeof(void*)),
                                  FAT_MAX_DEPTH, false);
   PyObject_GC_Track((PyObject*)st->g_fat_empty1);
   PyObject_GC_Track((PyObject*)st->g_fat_empty2);
   PyObject_GC_Track((PyObject*)st->g_fat_empty3);
   return 0;
}

#endif // PCOLLECTIONS__C_CORE_H
