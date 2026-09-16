# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_parity.py
# Checks that the C and pure-Python backends expose identical public
# interfaces, regardless of which one `pcollections` ends up using.
# By Noah C. Benson

"""Interface-parity checks between the pure-Python and C backends.

These tests run only when both backends are available (i.e., the C
extension module is built and importable); otherwise
`test_public_api_matches` is skipped for every pair.

Two backends are considered interface-identical here if, for every
corresponding pair of classes (`pdict`/`pdict`, `llist`/`llist`, etc.),
their *public* attributes are the same set of names, and each is the same
*kind* of thing (method, property, etc.) on both sides. Excluded are a
listed set of implementation-specific names and leading-underscore
internals such as `_els`/`_idx`/`_start`, which are storage details that
differ between the C structs and the pure-Python trie-backed objects.
"""

from unittest import TestCase, skipUnless

from ._backends import BACKENDS

_HAVE_C = 'c' in BACKENDS

# Names that are allowed to appear on only one side. `__weakref__`/
# `__dict__` are per-instance slots CPython adds automatically.
# `__orig_bases__`/`__slots__`/`__abstractmethods__` etc. are bookkeeping
# from ABCMeta/typing, not public API. `__firstlineno__`/
# `__static_attributes__` (3.13+) and `__annotate_func__`/
# `__annotations_cache__` (3.14+, PEP 649/749) are attached by the compiler
# to every `class` statement, and so are absent from C heap types built with
# PyType_FromSpecWithBases.
_IGNORE = {
    '__dict__', '__weakref__', '__slots__', '__module__', '__doc__',
    '__abstractmethods__', '__orig_bases__', '__parameters__',
    '__class_getitem__', '__init_subclass__', '__subclasshook__',
    '__firstlineno__', '__static_attributes__',
    '__annotate_func__', '__annotations_cache__',
    # lazy.trace is a class attribute holding a setting.
    'trace',
}


def _public_names(cls):
    names = set()
    for klass in cls.__mro__:
        for name in vars(klass):
            if name.startswith('_') and not (name.startswith('__') and name.endswith('__')):
                continue  # skip single/double-leading non-dunder "private" names
            names.add(name)
    return names - _IGNORE


class _ParityMixin:
    """Set `python_cls`/`c_cls` as class attributes on a subclass to compare
    one pair of classes."""
    python_cls = None
    c_cls = None

    def test_public_api_matches(self):
        if self.c_cls is None:
            self.skipTest("C backend not available in this environment")
        py_names = _public_names(self.python_cls)
        c_names = _public_names(self.c_cls)
        only_py = py_names - c_names
        only_c = c_names - py_names
        self.assertEqual(
            only_py, set(),
            f"{self.python_cls.__name__}: names present in the pure-Python "
            f"backend but missing from the C backend: {sorted(only_py)}")
        self.assertEqual(
            only_c, set(),
            f"{self.c_cls.__name__}: names present in the C backend but "
            f"missing from the pure-Python backend (undocumented extras): "
            f"{sorted(only_c)}")


def _make_parity_tests():
    made = {}
    if not _HAVE_C:
        return made
    py = BACKENDS['python']
    c = BACKENDS['c']
    for key in ('pdict', 'tdict', 'plist', 'tlist', 'pset', 'tset',
                'lazy', 'ldict', 'tldict', 'llist', 'tllist'):
        cls_name = f"TestParity_{key}"
        cls = type(cls_name, (_ParityMixin, TestCase),
                    {'python_cls': py[key], 'c_cls': c[key]})
        globals()[cls_name] = cls
        made[key] = cls
    return made


_make_parity_tests()
