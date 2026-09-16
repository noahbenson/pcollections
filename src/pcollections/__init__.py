# -*- coding: utf-8 -*-
################################################################################
# pcollections/__init__.py
# Initialization file for the pcollections library.
# By Noah C. Benson

"""Persistent and Transient Collections for Python

The public types and functions come from one of two backends, chosen once at
import time:

- the C backend, the compiled extension module ``pcollections._c._core``,
  used whenever it can be imported; or
- the pure-Python backend (``pcollections._dict``, ``_list``, ``_set``, and
  ``_lazy``), which provides the same interface and is used otherwise.

The choice is all-or-nothing, never a mix: the lazy collections subclass the
persistent collections of their own backend (``ldict`` subclasses ``pdict``,
and so on), so mixing backends would give two unrelated ``pdict`` types in one
process.

``using_c_extension`` records which backend is in use.
"""

_PUBLIC_NAMES = (
    'pdict', 'tdict', 'plist', 'tlist', 'pset', 'tset',
    'lazy', 'unlazy', 'holdlazy', 'llist', 'ldict', 'tllist', 'tldict',
    'LazyError', 'lazy_error_unwrap')


def _load_c_backend():
    """Returns the C backend's public objects, keyed by public name.

    Raises ImportError if the extension is missing, and may raise TypeError
    if it is present but cannot initialize on this interpreter; the caller
    falls back to the pure-Python backend in either case.
    """
    from ._c import _core
    return {name: getattr(_core, name) for name in _PUBLIC_NAMES}


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
    #: True if the C backend is in use; False for the pure-Python backend.
    using_c_extension = True
except (ImportError, TypeError):
    _backend = _load_python_backend()
    using_c_extension = False

globals().update(_backend)
if not using_c_extension:
    # The C types are named pcollections.<name>; give the pure-Python classes
    # the same module when they are the ones in use, so that pickles always
    # refer to pcollections.<name> and load under either backend.
    for _obj in _backend.values():
        if isinstance(_obj, type):
            _obj.__module__ = __name__
    del _obj
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

__version__ = "0.5.0"
