# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_backends.py
# Collects the pure-Python and (if available) C implementations of the
# pcollections types into a single BACKENDS dict, keyed by backend name, so
# the rest of the test suite can run the exact same test logic against both
# and can compare the two for interface/behavior parity.
# By Noah C. Benson

"""Backend discovery for the pcollections test suite.

``BACKENDS`` maps a backend name ('python', and 'c' when the compiled
extension modules are importable) to a dict of the public classes/functions
each backend provides, under a common set of keys:
    pdict, tdict, plist, tlist, pset, tset,
    lazy, ldict, tldict, llist, tllist,
    LazyError, lazy_error_unwrap, unlazy, holdlazy

Every test in this package that exercises class *behavior* should be written
once and parametrized over ``BACKENDS`` (see ``_dict.py``/``_list.py``/
``_set.py`` for the pattern: a plain mixin class holding the test methods,
instantiated once per backend via a small factory function) rather than
importing a single hardcoded implementation -- this is what makes the suite
exercise both the pure-Python fallback and the C extension (when built), and
is also what ``_parity.py`` relies on to compare the two for interface
identity.
"""


def _load_python():
    from .. import _dict as dm, _list as lm, _set as sm, _lazy as zm
    return {
        'pdict': dm.pdict, 'tdict': dm.tdict,
        'plist': lm.plist, 'tlist': lm.tlist,
        'pset': sm.pset, 'tset': sm.tset,
        'lazy': zm.lazy, 'ldict': zm.ldict, 'tldict': zm.tldict,
        'llist': zm.llist, 'tllist': zm.tllist,
        'LazyError': zm.LazyError, 'lazy_error_unwrap': zm.lazy_error_unwrap,
        'unlazy': zm.unlazy, 'holdlazy': zm.holdlazy,
    }


def _load_c():
    from .._c import dict as dm, list as lm, set as sm, lazy as zm
    return {
        'pdict': dm.pdict, 'tdict': dm.tdict,
        'plist': lm.plist, 'tlist': lm.tlist,
        'pset': sm.pset, 'tset': sm.tset,
        'lazy': zm.lazy, 'ldict': zm.ldict, 'tldict': zm.tldict,
        'llist': zm.llist, 'tllist': zm.tllist,
        'LazyError': zm.LazyError, 'lazy_error_unwrap': zm.lazy_error_unwrap,
        'unlazy': zm.unlazy, 'holdlazy': zm.holdlazy,
    }


def _discover():
    backends = {'python': _load_python()}
    try:
        backends['c'] = _load_c()
    except (ImportError, TypeError):
        # The compiled extension modules aren't available in this
        # environment (not built, or built for a different Python/platform),
        # or are built but unusable on this interpreter -- e.g. CPython
        # 3.14 raises TypeError (not ImportError) from PyInit_dict() and
        # friends, since it now rejects the heap types _c/dict.c (etc.)
        # build on top of pcollections.abc's ABCMeta-based mixins; see
        # pcollections/__init__.py's _load_c_backend() docstring for the
        # full explanation -- either way, the suite simply runs python-only
        # in that case, matching pcollections/__init__.py's own fallback.
        pass
    return backends


BACKENDS = _discover()


def known_failure(*backend_names):
    """Marks a test method as a known failure for the named backends.

    ``make_tests`` wraps the method in ``unittest.expectedFailure`` for each
    listed backend, so the suite stays green while a bug is open. When the
    bug is fixed the test reports an "unexpected success", which fails the
    run and signals that the marker should be removed.
    """
    def _mark(fn):
        fn._known_failures = frozenset(backend_names)
        return fn
    return _mark


def _copy_function(fn):
    import functools
    @functools.wraps(fn)
    def _wrapper(*args, **kwargs):
        return fn(*args, **kwargs)
    return _wrapper


def make_tests(base_name, mixin_cls, module_globals):
    """Builds one ``unittest.TestCase`` subclass of ``mixin_cls`` per
    available backend, named ``f"{base_name}_{backend}"``, with the
    backend's classes/functions bound as class attributes (so test methods
    can refer to e.g. ``self.pdict``), and installs them into
    ``module_globals`` (pass ``globals()`` from the calling test module) so
    that ``unittest`` discovery picks them up.
    """
    import inspect
    from unittest import TestCase, expectedFailure
    made = {}
    for name, backend in BACKENDS.items():
        cls_name = f"{base_name}_{name}"
        # Plain functions (unlike classes/instances) are descriptors: stored
        # as a class attribute and accessed via `self.holdlazy`, a bare
        # `def holdlazy(obj): ...` would be bound as an instance method
        # (silently swallowing `self` as its first argument) instead of
        # being called as the free function the test bodies expect --
        # staticmethod() suppresses that binding.
        attrs = {
            k: (staticmethod(v) if inspect.isfunction(v) else v)
            for (k, v) in backend.items()
        }
        attrs['backend_name'] = name
        for attr in dir(mixin_cls):
            fn = getattr(mixin_cls, attr)
            if name in getattr(fn, '_known_failures', ()):
                # expectedFailure() marks the function object it is given,
                # so wrap first: the mixin's function is shared by every
                # backend's class.
                attrs[attr] = expectedFailure(_copy_function(fn))
        cls = type(cls_name, (mixin_cls, TestCase), attrs)
        cls.__module__ = module_globals.get('__name__', mixin_cls.__module__)
        module_globals[cls_name] = cls
        made[name] = cls
    return made
