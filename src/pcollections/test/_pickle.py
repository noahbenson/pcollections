# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_pickle.py
# Pickling tests, run against every backend.
# By Noah C. Benson

"""Pickling tests.

Collections pickle by class, and the classes of the backend in use are named
``pcollections.<name>``, so a pickle refers only to ``pcollections`` and
loads under either backend.
"""

import pickle
import pickletools

import pcollections

from ._backends import BACKENDS, make_tests

_TYPES = ('pdict', 'tdict', 'plist', 'tlist', 'pset', 'tset',
          'ldict', 'tldict', 'llist', 'tllist')


def _globals_in(data):
    """Returns the (module, name) pairs a pickle refers to."""
    found = []
    strings = []
    for op, arg, _pos in pickletools.genops(data):
        if op.name in ('GLOBAL', 'INST'):
            found.append(tuple(arg.split(' ', 1)))
        elif op.name in ('SHORT_BINUNICODE', 'BINUNICODE', 'UNICODE',
                         'BINUNICODE8'):
            strings.append(arg)
        elif op.name == 'STACK_GLOBAL':
            found.append((strings[-2], strings[-1]))
    return found


class _PickleTests:
    """Instantiated once per backend by ``make_tests``."""

    def sample(self, name):
        ctor = getattr(self, name)
        if name.endswith('dict'):
            return ctor({'a': 1, 'b': [2, 3], 3: 'c'})
        if name.endswith('set'):
            return ctor(['a', 1, (2, 3)])
        return ctor(['a', 1, (2, 3), 4.5])

    def test_roundtrip(self):
        for name in _TYPES:
            obj = self.sample(name)
            for proto in range(2, pickle.HIGHEST_PROTOCOL + 1):
                back = pickle.loads(pickle.dumps(obj, protocol=proto))
                self.assertIs(type(back), type(obj), (name, proto))
                if name.startswith('t'):
                    self.assertEqual(back.persistent(), obj.persistent())
                else:
                    self.assertEqual(back, obj)

    def test_empty_roundtrip(self):
        for name in _TYPES:
            obj = getattr(self, name)()
            back = pickle.loads(pickle.dumps(obj))
            self.assertIs(type(back), type(obj), name)
            self.assertEqual(len(back), 0, name)

    def test_lazy_collections_pickle_computed_values(self):
        ld = self.ldict(a=self.lazy(int, '7'))
        back = pickle.loads(pickle.dumps(ld))
        self.assertEqual(back['a'], 7)
        self.assertFalse(back.is_lazy('a'))
        ll = self.llist([self.lazy(int, '8')])
        self.assertEqual(pickle.loads(pickle.dumps(ll))[0], 8)

    def test_pickles_refer_to_pcollections(self):
        # Only the backend in use has its classes named pcollections.<name>.
        in_use = self.pdict is pcollections.pdict
        for name in _TYPES:
            refs = _globals_in(pickle.dumps(self.sample(name)))
            if in_use:
                self.assertIn(('pcollections', name), refs, name)
            self.assertFalse(
                any(mod.startswith('pcollections._c') for (mod, _) in refs),
                (name, refs))

    def test_loads_under_the_other_backend(self):
        others = [b for (n, b) in BACKENDS.items() if n != self.backend_name]
        for other in others:
            for name in _TYPES:
                obj = self.sample(name)
                target = other[name]

                class Remap(pickle.Unpickler):
                    def find_class(self, module, qualname):
                        if (module == 'pcollections'
                                or module.startswith('pcollections.')):
                            if qualname in _TYPES:
                                return other[qualname]
                        return super().find_class(module, qualname)

                import io
                back = Remap(io.BytesIO(pickle.dumps(obj))).load()
                self.assertIs(type(back), target, name)
                expect = obj.persistent() if name.startswith('t') else obj
                got = back.persistent() if name.startswith('t') else back
                self.assertEqual(list(got.items()) if name.endswith('dict')
                                 else sorted(map(repr, got)),
                                 list(expect.items()) if name.endswith('dict')
                                 else sorted(map(repr, expect)),
                                 name)

    def test_subclass_roundtrip(self):
        back = pickle.loads(pickle.dumps(
            _subclasses[self.backend_name](a=1)))
        self.assertIs(type(back), _subclasses[self.backend_name])
        self.assertEqual(back['a'], 1)

    def test_internal_types_not_instantiable(self):
        for obj in (iter(self.pdict(a=1)), iter(self.plist([1])),
                    iter(self.pset([1])), iter(self.tdict(a=1)),
                    iter(self.tlist([1])), iter(self.tset([1]))):
            with self.assertRaises(TypeError):
                type(obj)()


# Module-level subclasses (pickle finds classes by qualified name).
_subclasses = {}
for _name, _backend in BACKENDS.items():
    _cls = type(f'SubPDict_{_name}', (_backend['pdict'],), {'__slots__': ()})
    _cls.__module__ = __name__
    globals()[_cls.__name__] = _cls
    _subclasses[_name] = _cls
del _name, _backend, _cls

make_tests('TestPickle', _PickleTests, globals())
