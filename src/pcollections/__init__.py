# -*- coding: utf-8 -*-
################################################################################
# pcollections/__init__.py
# Initialization file for the pcollections library.
# By Noah C. Benson

"""Persistent and Transient Collections for Python

Whenever the compiled `pcollections._c` extension modules (`_c.dict`,
`_c.list`, `_c.set`, `_c.lazy`) are available, this package uses them as the
source of every public type and function (`pdict`, `tdict`, `plist`,
`tlist`, `pset`, `tset`, `lazy`, `ldict`, `tldict`, `llist`, `tllist`,
`unlazy`, `holdlazy`, `LazyError`, `lazy_error_unwrap`); when they aren't
available (not built, or built for a different Python version/platform),
it falls back to the pure-Python reference implementation in
`pcollections._dict`/`_list`/`_set`/`_lazy`, which provides an identical
public interface (see `pcollections.test`, which runs the same test suite
against both backends and checks their interfaces match).

This choice is made exactly once at import time, as a single all-or-nothing
unit -- never a mix of C types and Python types. That's a deliberate
restriction, not an arbitrary one: `_c/lazy.c`'s C `ldict`/`llist` are
built as real subclasses of the C `pdict`/`plist` (imported directly from
`_c.dict`/`_c.list` at the C extension's own init time), while
`_lazy.py`'s pure-Python `ldict`/`llist` are, likewise, subclasses of the
pure-Python `pdict`/`plist`. If this module imported, say, C `pdict` but
pure-Python `ldict`, the result would be two unrelated, incompatible
notions of "pdict" in the same process (`isinstance(some_ldict, pdict)`
would be `False` for the C `pdict`), which no amount of interface-matching
could paper over. Falling back as one unit keeps every type internally
consistent, at the cost of losing the C speedups for everything if even
one of the four extension modules can't be built/loaded.
"""

def _load_c_backend():
    """Imports and returns the C-extension implementations of every public
    pcollections type/function, as a dict keyed by public name. Raises
    ImportError or TypeError (uncaught, by design -- see the module
    docstring and the try/except around this function's call site) if any
    of the four extension modules isn't importable or isn't usable on this
    interpreter.

    TypeError is included alongside the obvious ImportError because a
    *present* extension module can still fail to finish initializing on an
    interpreter whose C API has moved out from under it: e.g. CPython 3.14
    tightened PyType_FromSpecWithBases/PyType_FromMetaclass to reject a
    heap type whose base's metaclass overrides tp_new (which
    collections.abc.ABCMeta -- the metaclass behind pcollections.abc's
    PersistentMapping/TransientMapping/etc., which _c/dict.c and friends
    subclass when building pdict/tdict/etc. -- does), and that shows up as
    `PyInit_dict()` (etc.) failing with exactly that TypeError instead of
    an ImportError. Since this is exactly the kind of "unusual...Python
    implementation" case the module docstring already promises a graceful
    pure-Python fallback for, and not a sign of a corrupted install, it's
    caught here rather than left to crash the whole `import pcollections`."""
    from ._c import dict as _cdict
    from ._c import list as _clist
    from ._c import set  as _cset
    from ._c import lazy as _clazy
    return {
        'pdict': _cdict.pdict, 'tdict': _cdict.tdict,
        'plist': _clist.plist, 'tlist': _clist.tlist,
        'pset':  _cset.pset,   'tset':  _cset.tset,
        'lazy': _clazy.lazy, 'unlazy': _clazy.unlazy,
        'holdlazy': _clazy.holdlazy,
        'llist': _clazy.llist, 'ldict': _clazy.ldict,
        'tllist': _clazy.tllist, 'tldict': _clazy.tldict,
        'LazyError': _clazy.LazyError,
        'lazy_error_unwrap': _clazy.lazy_error_unwrap,
    }


def _load_python_backend():
    """Imports and returns the pure-Python implementations of every public
    pcollections type/function, as a dict keyed by public name."""
    from ._list import plist, tlist
    from ._set  import pset,  tset
    from ._dict import pdict, tdict
    from ._lazy import (
        lazy, unlazy, holdlazy,
        llist, ldict,
        tllist, tldict,
        LazyError,
        lazy_error_unwrap)
    return {
        'pdict': pdict, 'tdict': tdict,
        'plist': plist, 'tlist': tlist,
        'pset':  pset,  'tset':  tset,
        'lazy': lazy, 'unlazy': unlazy, 'holdlazy': holdlazy,
        'llist': llist, 'ldict': ldict,
        'tllist': tllist, 'tldict': tldict,
        'LazyError': LazyError,
        'lazy_error_unwrap': lazy_error_unwrap,
    }


try:
    _backend = _load_c_backend()
    #: True if the compiled C extension modules are backing the types in
    #: this package; False if the pure-Python fallback is in use. Mostly
    #: useful for diagnostics/tests (see `pcollections.test`), not
    #: something ordinary user code should need to branch on, since both
    #: backends are meant to be interface- and behavior-identical.
    using_c_extension = True
except (ImportError, TypeError):
    _backend = _load_python_backend()
    using_c_extension = False

globals().update(_backend)
del _backend, _load_c_backend, _load_python_backend

# We don't include the abc types in the __all__; they are probably not as
# frequently used and don't really need to be here. One can always `import
# pcollections.abc` if they are needed.
#
# from .abc import (
#     Persistent,         Transient,
#     PersistentSequence, TransientSequence,
#     PersistentSet,      TransientSet,
#     PersistentMapping,  TransientMapping)

__all__ = (
    "plist", "tlist",
    "pset",  "tset",
    "pdict", "tdict",
    "llist", "tllist",
    "ldict", "tldict",
    "lazy", "unlazy", "holdlazy")

__version__ = "0.4.0"
