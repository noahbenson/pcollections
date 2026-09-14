# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_parity.py
# Checks that the C and pure-Python backends expose identical public
# interfaces, regardless of which one `pcollections` ends up using.
# By Noah C. Benson

"""Interface-parity checks between the pure-Python and C backends.

These tests only run anything when both backends are available (i.e. the C
extension modules are built and importable) -- when only the pure-Python
backend is present, `test_public_api_matches` for every pair is skipped
rather than failing, since there is nothing to compare against.

Two backends are considered interface-identical here if, for every
corresponding pair of classes (`pdict`/`pdict`, `llist`/`llist`, etc.),
their *public* attributes (excluding a documented list of legitimately
implementation-specific names, and excluding leading-underscore internals
like `_els`/`_idx`/`_start`, which are storage details that are expected --
even required, per abc/_core.py's comment on why -- to differ between a
trie-backed C struct and a PHAMT/THAMT-backed Python object) are the same
set of names, and that each is the same *kind* of thing (method, property,
etc.) on both sides.
"""

from unittest import TestCase, skipUnless

from ._backends import BACKENDS

_HAVE_C = 'c' in BACKENDS

# Names that are allowed to appear on only one side. `empty` is a plain
# stored class attribute on pdict/plist (both backends) but a classmethod
# on tdict/tlist (both backends) -- included here defensively in case a
# future backend implements it a third way; `__weakref__`/`__dict__` are
# per-instance slots CPython adds automatically and aren't meaningful to
# compare. `__orig_bases__`/`__slots__`/`__abstractmethods__` etc. are
# implementation bookkeeping from ABCMeta/typing, not part of the public
# collection API. `__firstlineno__`/`__static_attributes__` (3.13+) and
# `__annotate_func__`/`__annotations_cache__` (3.14+, PEP 649/749's deferred-
# evaluation machinery) are the same kind of thing, one CPython version
# later: bookkeeping the compiler attaches to every ordinary `class`
# statement regardless of whether the class actually has annotations,
# which a C heap type built via PyType_FromSpecWithBases never goes
# through (there's no class body for the compiler to have compiled in the
# first place) -- confirmed as a real 3.14 CI failure (present on the
# actual 3.14 release but not on the 3.14.0rc2 interpreter used to develop
# this suite, so it never showed up locally).
_IGNORE = {
    '__dict__', '__weakref__', '__slots__', '__module__', '__doc__',
    '__abstractmethods__', '__orig_bases__', '__parameters__',
    '__class_getitem__', '__init_subclass__', '__subclasshook__',
    '__firstlineno__', '__static_attributes__',
    '__annotate_func__', '__annotations_cache__',
    # `lazy.__slots__ = ('partial', 'value')` in _lazy.py: these are raw
    # instance storage (what a lazy computation is waiting on / has cached),
    # analogous to pdict's `_els`/`_idx` but, unlike those, not given a
    # leading underscore in the reference -- an inconsistency in _lazy.py's
    # own naming, not part of the documented public API. The C `lazy` type
    # deliberately does not expose these as raw attributes (its internal
    # state is a completely different mutex/atomic-flag representation, not
    # a stored tuple), so they're excluded here rather than replicated.
    'partial', 'value',
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
