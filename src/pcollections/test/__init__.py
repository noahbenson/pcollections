# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/__init__.py
# Declaration of tests for the pcollections package.
# By Noah C. Benson

"""Tests for the pcollections package.

Every test module in this package is written once and parametrized over
whichever backends are available (see `_backends.py`): the pure-Python
implementation always, plus the compiled C extension when it's importable.
Running `python -m unittest pcollections.test` (or `pcollections.test.suite()`
below) therefore exercises both implementations with identical test logic,
and `_parity.py` additionally checks that the two backends expose the same
public interface.

Importing this package (or any of its submodules) has the side effect of
defining the per-backend `TestCase` subclasses (e.g. `TestPDict_python`,
and `TestPDict_c` when the C extension is available) as module attributes,
which is what lets `unittest`'s test discovery find them.

`_trie.py` is the exception: it tests pcollections._trie (the pure-Python
AMT/FAT tries used by the pure-Python backend) directly, since the C tries
are not exposed to Python. Its TestCase classes are not per-backend, but
suite()/discovery pick them up the same way.
"""

from . import _backends
from . import _dict
from . import _list
from . import _set
from . import _parity
from . import _trie
from . import _regress
from . import _interp
from . import _pickle
from . import _gc
from . import _threads
from . import _lazy
from . import _api
from . import _stress


def suite():
    """Returns a `unittest.TestSuite` containing every test in this package,
    across every available backend."""
    import unittest
    loader = unittest.TestLoader()
    return loader.loadTestsFromModule(_TestModuleNamespace())


class _TestModuleNamespace:
    """A tiny shim exposing every dynamically-generated TestCase from
    `_dict`, `_list`, `_set`, `_parity`, and `_trie` as an attribute, so
    `TestLoader.loadTestsFromModule` can find them all at once."""
    def __init__(self):
        for mod in (_dict, _list, _set, _parity, _trie, _regress, _interp, _pickle, _gc,
                    _threads, _lazy, _api, _stress):
            for (name, val) in vars(mod).items():
                if isinstance(val, type) and name.startswith('Test'):
                    setattr(self, name, val)
