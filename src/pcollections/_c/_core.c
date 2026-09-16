///////////////////////////////////////////////////////////////////////////////
// _c/_core.c
// The pcollections._c._core extension module: the C implementations of
// every public pcollections type and function.
//
// This is the only file the build compiles. It includes core.h (shared
// definitions, including the per-interpreter module state) and then each
// part, in dependency order: dict and list first, since the lazy
// collections subclass their types.

#include "core.h"
#include "dict.c.h"
#include "list.c.h"
#include "set.c.h"
#include "lazy.c.h"


//=============================================================================
// Module state lifecycle.

#if PY_VERSION_HEX >= 0x03090000
#  define PCOLL_MODULE_STATE_SIZE ((Py_ssize_t)sizeof(pcoll_state))
static pcoll_state* module_state(PyObject* m) {
   return (pcoll_state*)PyModule_GetState(m);
}
#else
// On 3.8 the state is a process-global that is never freed: without
// PyType_FromModuleAndSpec, objects can outlive the module object (see
// core.h), and only one interpreter may load the module anyway.
#  define PCOLL_MODULE_STATE_SIZE 0
static pcoll_state g_state_38;
static PyObject* g_state_38_owner = NULL;  // the module that executed.
static pcoll_state* module_state(PyObject* m) {
   // A module object that failed to execute (in a second interpreter, say)
   // has no state; in particular its traversal must not report the owner's
   // objects, which belong to another interpreter.
   if (g_state_38_owner && m != g_state_38_owner) return NULL;
   return &g_state_38;
}
#endif

#define PCOLL_VISIT_FIELD(field) Py_VISIT((PyObject*)(field))
static int core_traverse(PyObject* m, visitproc visit, void* arg) {
   pcoll_state* st = module_state(m);
   if (!st) return 0;
   PCOLL_STATE_FOREACH(PCOLL_VISIT_FIELD, st);
   return 0;
}
#undef PCOLL_VISIT_FIELD

static int core_clear(PyObject* m) {
#if PY_VERSION_HEX >= 0x03090000
   pcoll_state* st = module_state(m);
   if (!st) return 0;
   PCOLL_STATE_FOREACH(Py_CLEAR, st);
#else
   (void)m;
#endif
   return 0;
}

static void core_free(void* mv) {
#if PY_VERSION_HEX >= 0x03090000
   PyObject* m = (PyObject*)mv;
   pcoll_state* st = module_state(m);
   if (!st) return;
   pcoll_registry_remove(st);
   core_clear(m);
#else
   (void)mv;  // the 3.8 state is never freed (see module_state).
#endif
}

static int core_exec(PyObject* m) {
   pcoll_state* st = module_state(m);
   PyObject* util;
   if (pcoll_registry_init() < 0) return -1;
   if (!st) {
      PyErr_SetString(PyExc_ImportError,
         "pcollections._c._core supports only one interpreter per process "
         "on Python 3.8");
      return -1;
   }
   // Register first: the parts' code reaches the state through ST().
   if (pcoll_registry_add(st) < 0) return -1;
#if PY_VERSION_HEX < 0x03090000
   g_state_38_owner = m;
#endif

   // Create the shared empty tries for every leaf size the parts use, so
   // later lookups never need the lock. (Each call returns a reference,
   // which is released immediately; the singleton keeps its own.)
   fatnode_decref(fat_empty(ELSLEAFSIZE), NULL);
   amtnode_decref(amt_empty(IDXLEAFSIZE), NULL);
   fatnode_decref(fat_empty(PYLEAFSIZE), NULL);
   fatnode_decref(fat_empty(SETELSLEAFSIZE), NULL);
   amtnode_decref(amt_empty(SETIDXLEAFSIZE), NULL);

   util = PyImport_ImportModule("pcollections.util");
   if (!util) return -1;
   st->g_seqstr = PyObject_GetAttrString(util, "seqstr");
   Py_DECREF(util);
   if (!st->g_seqstr) return -1;

   if (pcoll_exec_dict(m, st) < 0) return -1;
   if (pcoll_exec_list(m, st) < 0) return -1;
   if (pcoll_exec_set(m, st) < 0) return -1;
   if (pcoll_exec_lazy(m, st) < 0) return -1;
   return 0;
}


//=============================================================================
// Module definition.

static PyModuleDef_Slot core_slots[] = {
   {Py_mod_exec, (void*)core_exec},
#if PY_VERSION_HEX >= 0x030C0000
   {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
   {0, NULL}
};

static PyModuleDef core_module = {
   PyModuleDef_HEAD_INIT,
   .m_name = "pcollections._c._core",
   .m_doc = "C implementations of the pcollections types.",
   .m_size = PCOLL_MODULE_STATE_SIZE,
   .m_methods = lazy_module_methods,
   .m_slots = core_slots,
   .m_traverse = core_traverse,
   .m_clear = core_clear,
   .m_free = core_free,
};

PyMODINIT_FUNC PyInit__core(void) {
   return PyModuleDef_Init(&core_module);
}
