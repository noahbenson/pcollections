# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_api.py
# Tests of the public API details shared by both backends: hashing, equality,
# slicing, subclassing, operators, error messages, and weak references.
# By Noah C. Benson

"""Tests of API details, run against every backend.

Most of these compare a pcollections type with the builtin it resembles
(`list`, `set`, `frozenset`, or `dict`), since the types are meant to behave
like those builtins wherever that makes sense. `_ParityHarness` runs the same
random operations on both backends and checks that they agree.
"""

import gc
import math
import operator
import os
import pickle
import random
import subprocess
import sys
import textwrap
import unittest
import weakref

from ._backends import BACKENDS, make_tests


_PKG_PARENT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def _run_python(code, env_updates=(), timeout=300):
    env = dict(os.environ)
    for k in ('PCOLLECTIONS_NO_C_EXTENSIONS', 'PCOLLECTIONS_REQUIRE_C'):
        env.pop(k, None)
    env.update(env_updates)
    env['PYTHONPATH'] = os.pathsep.join(
        [_PKG_PARENT] + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
    return subprocess.run(
        [sys.executable, '-c', textwrap.dedent(code)],
        env=env, capture_output=True, text=True, errors='replace',
        timeout=timeout)


class _Unorderable:
    """Equal to itself only, and not orderable."""
    __slots__ = ('v',)
    def __init__(self, v):
        self.v = v
    def __eq__(self, other):
        return isinstance(other, _Unorderable) and self.v == other.v
    def __hash__(self):
        return hash(self.v)
    def __repr__(self):
        return f"_Unorderable({self.v!r})"


def _exc(fn, *args, **kwargs):
    """Returns `(type, message)` for the exception `fn` raises, or `None`."""
    try:
        fn(*args, **kwargs)
    except Exception as e:
        return (type(e), str(e))
    return None


def _outcome(fn, *args, **kwargs):
    """Returns `('ok', result)` or `('err', type, message)`."""
    try:
        return ('ok', fn(*args, **kwargs))
    except Exception as e:
        return ('err', type(e), str(e))


class _ApiTests:
    """Instantiated once per backend by ``make_tests``."""

    # Hashing ---------------------------------------------------------------
    def test_pset_hash_matches_frozenset(self):
        r = random.Random(1)
        samples = [(), (0,), (1, 2, 3), ('a', None, 2.5), tuple(range(200)),
                   (frozenset((1, 2)), (1, 2), -1, -2, 2**70)]
        for s in samples:
            self.assertEqual(hash(self.pset(s)), hash(frozenset(s)), s)
        # After building up and removing elements.
        p = self.pset(range(500))
        for k in r.sample(range(500), 300):
            p = p.discard(k)
        self.assertEqual(hash(p), hash(frozenset(p)))
        t = self.tset(range(100))
        for k in range(0, 100, 3):
            t.discard(k)
        self.assertEqual(hash(t.persistent()), hash(frozenset(t)))
        # Nested persistent sets hash like nested frozensets.
        inner = self.pset((1, 2))
        self.assertEqual(hash(self.pset((inner, 3))),
                         hash(frozenset((frozenset((1, 2)), 3))))
        # Equal objects hash equally, so they are interchangeable as keys.
        self.assertEqual({frozenset((1, 2)): 'x'}[self.pset((1, 2))], 'x')
        self.assertEqual({self.pset((1, 2)): 'x'}[frozenset((1, 2))], 'x')

    def test_transients_are_unhashable(self):
        for t in (self.tdict(a=1), self.tset((1,)), self.tlist((1,)),
                  self.tldict(a=1), self.tllist((1,))):
            with self.assertRaises(TypeError, msg=type(t)):
                hash(t)
            self.assertIsNone(type(t).__hash__)

    def test_persistent_hashes(self):
        self.assertEqual(hash(self.plist((1, 2))), hash(self.plist([1, 2])))
        self.assertEqual(hash(self.pdict(a=1)), hash(self.pdict([('a', 1)])))
        self.assertNotEqual(hash(self.pdict(a=1)), hash(frozenset({'a': 1}.items())))
        with self.assertRaises(TypeError):
            hash(self.plist(([],)))

    # NaN and identity ------------------------------------------------------
    def test_identity_or_equality(self):
        nan = float('nan')
        # dict/set/list find an object by identity even when it isn't equal
        # to itself; the persistent types do the same.
        for (p, t) in ((self.pset, self.tset),):
            for s in (p((nan, 1)), t((nan, 1))):
                self.assertIn(nan, s)
                self.assertNotIn(float('nan'), s)
                self.assertEqual(s, set((nan, 1)))
        for d in (self.pdict({nan: 1}), self.tdict({nan: 1})):
            self.assertIn(nan, d)
            self.assertEqual(d[nan], 1)
            self.assertNotIn(float('nan'), d)
            self.assertEqual(d, {nan: 1})
            self.assertEqual(dict(d), {nan: 1})
        d = self.pdict(a=nan)
        self.assertEqual(d, {'a': nan})
        self.assertEqual(d, self.pdict(a=nan))
        self.assertNotEqual(d, self.pdict(a=float('nan')))
        for L in (self.plist((1, nan, 2)), self.tlist((1, nan, 2)),
                  self.llist((1, nan, 2))):
            ref = [1, nan, 2]
            self.assertIn(nan, L)
            self.assertEqual(L.index(nan), ref.index(nan))
            self.assertEqual(L.count(nan), ref.count(nan))
            self.assertEqual(L.count(float('nan')), 0)
            self.assertEqual(L, ref)
            self.assertEqual(L, type(L)(ref))
            self.assertFalse(L != ref)
            self.assertEqual(list(L) == [1, float('nan'), 2],
                             ref == [1, float('nan'), 2])
            self.assertTrue(L <= ref)
            self.assertFalse(L < ref)
        p = self.plist((1, nan, 2)).remove(nan)
        self.assertEqual(list(p), [1, 2])
        t = self.tlist((1, nan, 2))
        t.remove(nan)
        self.assertEqual(list(t), [1, 2])

    def test_unorderable_elements(self):
        a, b = _Unorderable(1), _Unorderable(2)
        for L in (self.plist, self.tlist):
            self.assertEqual(L((a, b)), [a, b])
            self.assertEqual(L((a, b)), L((_Unorderable(1), _Unorderable(2))))
            self.assertNotEqual(L((a,)), L((b,)))
            self.assertTrue(L((a,)) < L((a, b)))
            with self.assertRaises(TypeError):
                L((a,)) < L((b,))
        self.assertEqual(self.pdict(x=a), {'x': a})
        self.assertEqual(self.pset((a, b)), {a, b})

    def test_ordering_against_list(self):
        r = random.Random(2)
        ops = (operator.lt, operator.le, operator.gt, operator.ge,
               operator.eq, operator.ne)
        for _ in range(300):
            x = [r.randrange(3) for _ in range(r.randrange(4))]
            y = [r.randrange(3) for _ in range(r.randrange(4))]
            for op in ops:
                expect = op(x, y)
                for L in (self.plist, self.tlist, self.llist):
                    self.assertEqual(op(L(x), L(y)), expect, (op, x, y))
                    self.assertEqual(op(L(x), y), expect, (op, x, y))
                    self.assertEqual(op(x, L(y)), expect, (op, x, y))
        for L in (self.plist, self.tlist):
            # Like list, a plist is never equal to a tuple.
            self.assertNotEqual(L((1, 2)), (1, 2))
            self.assertNotEqual(L((1, 2)), 'ab')
            for op in ops[:4]:
                with self.assertRaises(TypeError):
                    op(L((1,)), 5)
                with self.assertRaises(TypeError):
                    op(L((1,)), {1})

    # drop / delete / dropall ------------------------------------------------
    def test_drop(self):
        d = self.pdict(a=1, b=2)
        self.assertEqual(d.drop('a'), {'b': 2})
        self.assertIs(d.drop('z'), d)
        with self.assertRaises(KeyError):
            d.drop('z', error=True)
        with self.assertRaises(KeyError):
            d.drop('z', True)
        with self.assertRaises(KeyError):
            d.delete('z')
        self.assertEqual(d.delete('b'), {'a': 1})
        self.assertEqual(d.dropall(('a', 'z')), {'b': 2})
        with self.assertRaises(KeyError):
            d.deleteall(('a', 'z'))
        self.assertEqual(d.deleteall(('a', 'b')), {})
        s = self.pset((1, 2))
        self.assertEqual(s.drop(1), {2})
        self.assertIs(s.drop(5), s)
        with self.assertRaises(KeyError):
            s.drop(5, error=True)
        p = self.plist((1, 2, 3))
        self.assertEqual(p.drop(), [1, 2])
        self.assertEqual(p.drop(0), [2, 3])
        self.assertEqual(p.drop(-2), [1, 3])
        self.assertIs(p.drop(10), p)
        with self.assertRaises(IndexError):
            p.drop(10, error=True)
        with self.assertRaises(IndexError):
            p.delete(10)
        self.assertIs(self.plist().drop(), self.plist())
        with self.assertRaises(TypeError):
            p.drop('x')

    # Slicing ---------------------------------------------------------------
    def test_slicing_matches_list(self):
        vals = [None, -12, -7, -3, -1, 0, 1, 2, 5, 7, 12]
        steps = [None, -3, -2, -1, 1, 2, 3, 5]
        for n in (0, 1, 7, 40):
            ref = list(range(n))
            for L in (self.plist, self.tlist, self.llist, self.tllist):
                obj = L(ref)
                for start in vals:
                    for stop in vals:
                        for step in steps:
                            s = slice(start, stop, step)
                            got = obj[s]
                            self.assertIs(type(got), L)
                            self.assertEqual(list(got), ref[s], (L, n, s))
                with self.assertRaises(ValueError):
                    obj[::0]
                for bad in ('a', 1.0, None):
                    e = _exc(operator.getitem, obj, bad)
                    self.assertEqual(e[0], TypeError, (L, bad))
                    self.assertIn('indices must be integers or slices', e[1])
        # Objects with __index__ work as indices.
        class I:
            def __index__(self):
                return 1
        self.assertEqual(self.plist((5, 6))[I()], 6)
        self.assertEqual(self.tlist((5, 6))[I()], 6)
        self.assertEqual(list(self.plist((5, 6, 7))[I():]), [6, 7])

    def test_tlist_slice_assignment(self):
        r = random.Random(3)
        for trial in range(400):
            n = r.randrange(12)
            ref = list(range(n))
            t = self.tlist(ref)
            start = r.choice([None, r.randrange(-15, 15)])
            stop = r.choice([None, r.randrange(-15, 15)])
            step = r.choice([None, 1, -1, 2, -2, 3])
            s = slice(start, stop, step)
            kind = r.random()
            if kind < 0.4:
                new = [100 + i for i in range(r.randrange(5))]
                if kind < 0.2:
                    new = len(ref[s]) * [7] if step not in (None, 1) else new
                src = r.choice([list, tuple, iter, self.plist, self.tlist])
                ra = _outcome(ref.__setitem__, s, new)
                ta = _outcome(t.__setitem__, s, src(new))
            else:
                ra = _outcome(ref.__delitem__, s)
                ta = _outcome(t.__delitem__, s)
            self.assertEqual(ta[:2], ra[:2], (n, s))
            self.assertEqual(list(t), ref, (n, s))
            self.assertEqual(len(t), len(ref))
        # Self-assignment and assignment of a non-iterable.
        t = self.tlist((1, 2, 3))
        t[1:] = t
        self.assertEqual(list(t), [1, 1, 2, 3])
        with self.assertRaises(TypeError):
            t[1:2] = 5
        with self.assertRaises(ValueError):
            t[::2] = [1]
        with self.assertRaises(IndexError):
            t[10] = 1
        with self.assertRaises(IndexError):
            del t[10]
        with self.assertRaises(TypeError):
            t['a'] = 1
        t[-1] = 'z'
        self.assertEqual(t[3], 'z')
        # The persistent copy made before a slice assignment is unaffected.
        t = self.tlist(range(5))
        p = t.persistent()
        t[1:4] = ['a']
        del t[::2]
        self.assertEqual(list(p), [0, 1, 2, 3, 4])
        self.assertEqual(list(t), ['a'])

    # Reversal ---------------------------------------------------------------
    def test_reversed_mappings(self):
        src = {'a': 1, 'b': 2, 'c': 3}
        for d in (self.pdict(src), self.tdict(src), self.ldict(src),
                  self.tldict(src)):
            self.assertEqual(list(reversed(d)), list(reversed(list(d))))
            self.assertEqual(list(reversed(d.keys())),
                             list(reversed(list(d.keys()))))
            self.assertEqual(list(reversed(d.items())),
                             list(reversed(list(d.items()))))
            self.assertEqual(list(reversed(d.values())),
                             list(reversed(list(d.values()))))
            self.assertEqual(sorted(reversed(d)), sorted(src))
        for L in (self.plist, self.tlist):
            self.assertEqual(list(reversed(L((1, 2, 3)))), [3, 2, 1])

    # Generic aliases -------------------------------------------------------
    @unittest.skipIf(sys.version_info < (3, 9), "requires Python 3.9")
    def test_class_getitem(self):
        import types
        for name in ('pdict', 'tdict', 'plist', 'tlist', 'pset', 'tset',
                     'ldict', 'tldict', 'llist', 'tllist'):
            cls = getattr(self, name)
            alias = cls[int] if 'list' in name or 'set' in name else cls[str, int]
            self.assertIsInstance(alias, types.GenericAlias)
            self.assertIs(alias.__origin__, cls)
        self.assertEqual(self.pdict[str, int]().__class__, self.pdict)

    # Operators and construction -------------------------------------------
    def test_mapping_union(self):
        d = self.pdict(a=1, b=2)
        self.assertEqual(d | {'b': 3, 'c': 4}, {'a': 1, 'b': 3, 'c': 4})
        self.assertIs(type(d | {'c': 4}), self.pdict)
        self.assertEqual({'b': 3, 'c': 4} | d, {'a': 1, 'b': 2, 'c': 4})
        self.assertIs(type({'c': 4} | d), self.pdict)
        self.assertEqual(self.pdict(x=1) | d, {'x': 1, 'a': 1, 'b': 2})
        with self.assertRaises(TypeError):
            d | [('c', 1)]
        with self.assertRaises(TypeError):
            d |= 5
        t = self.tdict(a=1)
        t2 = t
        t |= {'b': 2}
        self.assertIs(t, t2)
        self.assertEqual(t, {'a': 1, 'b': 2})
        t |= [('c', 3)]
        self.assertEqual(t, {'a': 1, 'b': 2, 'c': 3})
        self.assertIs(type(t | {'z': 0}), self.tdict)
        self.assertEqual(t, {'a': 1, 'b': 2, 'c': 3})
        with self.assertRaises(TypeError):
            t | [('c', 1)]
        p = d
        p |= {'q': 1}
        self.assertEqual(d, {'a': 1, 'b': 2})
        self.assertEqual(p, {'a': 1, 'b': 2, 'q': 1})

    def test_fromkeys(self):
        for cls in (self.pdict, self.tdict, self.ldict, self.tldict):
            d = cls.fromkeys('abc')
            self.assertIs(type(d), cls)
            self.assertEqual(dict(d), dict.fromkeys('abc'))
            self.assertEqual(dict(cls.fromkeys([1, 2], 0)), {1: 0, 2: 0})
            self.assertEqual(dict(cls.fromkeys(())), {})

    def test_sequence_operators(self):
        p = self.plist((1, 2))
        self.assertEqual(p + [3], [1, 2, 3])
        self.assertIs(type(p + [3]), self.plist)
        self.assertEqual([0] + p, [0, 1, 2])
        # Like list, a tuple can't be concatenated with a plist.
        with self.assertRaises(TypeError):
            (0,) + p
        with self.assertRaises(TypeError):
            p + (0,)
        self.assertIs(p + [], p)
        self.assertIs(self.plist() + p, p)
        self.assertEqual(p * 2, [1, 2, 1, 2])
        self.assertEqual(2 * p, [1, 2, 1, 2])
        self.assertEqual(p * 0, [])
        self.assertEqual(p * -1, [])
        for bad in (2.0, '2', None):
            with self.assertRaises(TypeError):
                p * bad
            with self.assertRaises(TypeError):
                bad * p
        with self.assertRaises(TypeError):
            p + 5
        with self.assertRaises(TypeError):
            p + {3}
        t = self.tlist((1,))
        t2 = t
        t += (2, 3)
        self.assertIs(t, t2)
        t *= 2
        self.assertIs(t, t2)
        self.assertEqual(t, [1, 2, 3, 1, 2, 3])
        with self.assertRaises(TypeError):
            t *= 'x'

    def test_set_operators(self):
        s = self.pset((1, 2, 3))
        self.assertEqual(s | {4}, {1, 2, 3, 4})
        self.assertEqual(s & frozenset((2, 9)), {2})
        self.assertEqual(s - {1}, {2, 3})
        self.assertEqual(s ^ {1, 9}, {2, 3, 9})
        self.assertEqual({1, 9} ^ s, {2, 3, 9})
        self.assertEqual({0, 1} | s, {0, 1, 2, 3})
        self.assertTrue(s > {1})
        self.assertTrue({1} < s)
        self.assertTrue(s <= s)
        for bad in ([1], (1,), 'a', 5, None):
            for op in (operator.or_, operator.and_, operator.sub,
                       operator.xor, operator.lt, operator.le,
                       operator.gt, operator.ge):
                with self.assertRaises(TypeError, msg=(op, bad)):
                    op(s, bad)
            self.assertNotEqual(s, bad)
        self.assertEqual(s.union([4]), {1, 2, 3, 4})
        self.assertEqual(s.difference('a'), s)

    def test_not_implemented(self):
        # Returning NotImplemented lets the other operand take over.
        class Other:
            def __radd__(self, other):
                return 'radd'
            def __rmul__(self, other):
                return 'rmul'
            def __ror__(self, other):
                return 'ror'
            def __eq__(self, other):
                return 'eq'
            def __lt__(self, other):
                return 'lt'
            def __gt__(self, other):
                return 'gt'
        o = Other()
        for L in (self.plist((1,)), self.tlist((1,))):
            self.assertEqual(L + o, 'radd')
            self.assertEqual(L * o, 'rmul')
            self.assertEqual(L == o, 'eq')
            self.assertEqual(L < o, 'gt')
        for S in (self.pset((1,)), self.tset((1,))):
            self.assertEqual(S | o, 'ror')
            self.assertEqual(S == o, 'eq')
            self.assertEqual(S > o, 'lt')
        for D in (self.pdict(a=1), self.tdict(a=1)):
            self.assertEqual(D | o, 'ror')
            self.assertEqual(D == o, 'eq')

    def test_error_messages(self):
        for L in (self.plist, self.tlist):
            name = L.__name__
            obj = L((1, 2))
            self.assertEqual(_exc(obj.index, 5),
                             (ValueError, f"5 is not in {name}"))
            e = _exc(obj.remove, 5)
            self.assertEqual(e[0], ValueError)
            self.assertIn('not in', e[1])
            e = _exc(obj.pop, 5)
            self.assertEqual(e[0], IndexError)
            self.assertEqual(_exc(L().pop)[0], IndexError)
            self.assertEqual(_exc(obj.index, 1, 1)[0], ValueError)
            self.assertEqual(obj.index(2, -1), 1)
            self.assertEqual(_exc(operator.getitem, obj, 2)[0], IndexError)
        self.assertEqual(_exc(self.plist().remove, 1)[0], ValueError)
        self.assertEqual(_exc(operator.getitem, self.pdict(), 'x'),
                         (KeyError, "'x'"))
        self.assertEqual(_exc(self.pset().remove, 'x')[0], KeyError)
        self.assertEqual(_exc(self.pset().pop)[0], KeyError)
        self.assertEqual(_exc(self.pdict().popitem)[0], KeyError)
        self.assertEqual(_exc(self.tdict().popitem)[0], KeyError)
        self.assertEqual(_exc(self.tset().pop)[0], KeyError)
        self.assertEqual(_exc(hash, self.pdict(x=[]))[0], TypeError)
        self.assertEqual(_exc(self.pdict().set, [], 1)[0], TypeError)
        self.assertEqual(_exc(self.pset().add, [])[0], TypeError)

    # Weak references --------------------------------------------------------
    def test_weakrefs(self):
        objs = [self.pdict(a=1), self.tdict(a=1), self.plist((1,)),
                self.tlist((1,)), self.pset((1,)), self.tset((1,)),
                self.ldict(a=1), self.tldict(a=1), self.llist((1,)),
                self.tllist((1,)), self.lazy(int, 1)]
        while objs:
            obj = objs.pop(0)
            fired = []
            r = weakref.ref(obj, lambda _: fired.append(1))
            self.assertIs(r(), obj)
            ws = weakref.WeakKeyDictionary()
            if type(obj).__hash__ is not None:
                ws[obj] = 1
                self.assertEqual(len(ws), 1)
            tname = type(obj).__name__
            del obj
            gc.collect()
            self.assertIsNone(r(), tname)
            self.assertEqual(fired, [1], tname)
            self.assertEqual(len(ws), 0, tname)
        # The empty singletons can be weakly referenced too.
        for cls in (self.pdict, self.plist, self.pset):
            self.assertIs(weakref.ref(cls.empty)(), cls.empty)
        d = weakref.WeakValueDictionary()
        key = self.pset((1,))
        val = self.pdict(v=1)
        d[key] = val
        self.assertIs(d[frozenset((1,))], val)

    # Subclasses ------------------------------------------------------------
    def _subclass_pair(self, pname, tname):
        P, T = getattr(self, pname), getattr(self, tname)
        class SubP(P):
            __slots__ = ()
        class SubT(T):
            __slots__ = ()
        SubP.__transient_type__ = SubT
        SubT.__persistent_type__ = SubP
        return (SubP, SubT)

    def test_partner_hooks(self):
        pairs = [('pdict', 'tdict'), ('plist', 'tlist'), ('pset', 'tset'),
                 ('ldict', 'tldict'), ('llist', 'tllist')]
        for (pn, tn) in pairs:
            P, T = getattr(self, pn), getattr(self, tn)
            self.assertIs(P.__transient_type__, T)
            self.assertIs(T.__persistent_type__, P)
            self.assertIs(type(P().transient()), T)
            self.assertIs(type(T().persistent()), P)

    def test_subclass_types_are_kept(self):
        seed = {'pdict': {'a': 1, 'b': 2}, 'plist': (1, 2, 3),
                'pset': (1, 2, 3), 'ldict': {'a': 1, 'b': 2},
                'llist': (1, 2, 3)}
        for (pn, tn) in (('pdict', 'tdict'), ('plist', 'tlist'),
                         ('pset', 'tset'), ('ldict', 'tldict'),
                         ('llist', 'tllist')):
            (SubP, SubT) = self._subclass_pair(pn, tn)
            p = SubP(seed[pn])
            self.assertIs(type(p), SubP)
            self.assertEqual(p, getattr(self, pn)(seed[pn]))
            empty = SubP()
            self.assertIs(type(empty), SubP)
            self.assertIs(SubP.empty, empty)
            self.assertIsNot(SubP.empty, getattr(self, pn).empty)
            t = p.transient()
            self.assertIs(type(t), SubT)
            self.assertIs(type(t.persistent()), SubP)
            self.assertIs(type(SubT(seed[pn])), SubT)
            self.assertIs(type(SubT().persistent()), SubP)
            self.assertIs(type(p.clear()), SubP)
            self.assertIs(type(p.copy()), SubP)
            self.assertIs(type(pickle.loads(pickle.dumps(p))), SubP) \
                if SubP.__qualname__.count('<locals>') == 0 else None
            if 'dict' in pn:
                results = [p.set('c', 3), p.drop('a'), p.drop('z'),
                           p.delete('a'), p.setall('x', (1,)),
                           p.dropall('ab'), p.update(z=0),
                           p | {'y': 1}, SubP.fromkeys('ab'),
                           p.dropall(('a', 'b'))]
                tres = [t | {'y': 1}, SubT.fromkeys('ab')]
            elif 'list' in pn:
                results = [p.set(0, 5), p.append(4), p.prepend(0),
                           p.insert(1, 9), p.extend((7,)), p.drop(),
                           p.delete(0), p.remove(2), p.reverse(),
                           p.sort(reverse=True), p[1:], p[::-1], p + [1],
                           p * 2, p[5:], p.drop(10)]
                tres = [t[1:], t[::-1], t.copy()]
            else:
                results = [p.add(4), p.add(1), p.discard(1), p.drop(1),
                           p.remove(1), p.addall((5, 6)),
                           p.discardall((1, 2)), p.union({7}),
                           p.intersection({1}), p.difference({1}),
                           p.symmetric_difference({1, 8}), p | {9},
                           p & {1}, p - {1}, p ^ {1}]
                tres = [t | {9}, t.copy(), t.union({1})]
            for (i, x) in enumerate(results):
                self.assertIs(type(x), SubP, (pn, i))
            for (i, x) in enumerate(tres):
                self.assertIs(type(x), SubT, (tn, i))

    def test_subclass_without_hooks(self):
        # A subclass that doesn't set the hooks gets the base partner types.
        for (pn, tn) in (('pdict', 'tdict'), ('plist', 'tlist'),
                         ('pset', 'tset')):
            P, T = getattr(self, pn), getattr(self, tn)
            class SubP(P):
                pass
            class SubT(T):
                pass
            p = SubP()
            self.assertIs(type(p.transient()), T)
            self.assertIs(type(SubT().persistent()), P)
            self.assertIs(type(T(p).persistent()), P)
            # Persistent objects are immutable, subclasses included.
            with self.assertRaises(TypeError):
                p.attr = 1

    def test_subclass_constructor_copies(self):
        base = self.pdict(a=1)
        sub = type('Sub', (self.pdict,), {})(base)
        self.assertIsNot(sub, base)
        self.assertIs(self.pdict(base), base)
        self.assertEqual(sub, base)
        sub2 = type(sub)(sub)
        self.assertIs(sub2, sub)
        plain = self.pset(type('SubS', (self.pset,), {})((1, 2)))
        self.assertIs(type(plain), self.pset)
        plain = self.plist(type('SubL', (self.plist,), {})((1, 2)))
        self.assertIs(type(plain), self.plist)
        self.assertEqual(plain, [1, 2])

    def test_bad_hooks(self):
        class Bad(self.pdict):
            __transient_type__ = int
        with self.assertRaises(TypeError):
            Bad(a=1).transient()

    # Leaks -----------------------------------------------------------------
    def test_no_leaks(self):
        import tracemalloc
        def work():
            d = self.pdict((i, str(i)) for i in range(200))
            t = d.transient()
            for i in range(0, 200, 2):
                del t[i]
            d = t.persistent() | {'x': 1}
            l = self.plist(range(300))
            tl = l.transient()
            tl[10:200:3] = range(len(range(10, 200, 3)))
            del tl[::2]
            s = self.pset(range(300)).discardall(range(100))
            hash(s), hash(l), hash(d)
            weakref.ref(s)
            ld = self.ldict(a=self.lazy(lambda: [1] * 10))
            ld['a']
            return (d, tl.persistent(), s, ld)
        for _ in range(3):
            work()
        gc.collect()
        tracemalloc.start()
        try:
            before = tracemalloc.get_traced_memory()[0]
            for _ in range(300):
                work()
            gc.collect()
            after = tracemalloc.get_traced_memory()[0]
        finally:
            tracemalloc.stop()
        self.assertLess(after - before, 200_000, after - before)

    # Pickling across backends -----------------------------------------------
    def test_cross_backend_pickle(self):
        if self.backend_name != 'c':
            self.skipTest("only needed once")
        code = """
            import pickle, sys, pcollections as pc
            if sys.argv[-1] == 'dump':
                pass
            objs = [pc.pdict(a=1, b=pc.plist((1, 2))), pc.pset((1, 2)),
                    pc.plist(range(40)), pc.tdict(x=1), pc.tset((3,)),
                    pc.tlist((1,)), pc.ldict(a=pc.lazy(int, '5')),
                    pc.llist((pc.lazy(int, '6'),))]
            data = {data!r}
            if data is None:
                sys.stdout.write(pickle.dumps(objs, 0).decode('latin-1'))
            else:
                back = pickle.loads(data.encode('latin-1'))
                assert [type(x).__name__ for x in back] == \\
                       [type(x).__name__ for x in objs], back
                assert back == objs, (back, objs)
                assert pc.using_c_extension == {expect_c!r}
                print('ok')
        """
        env_py = {'PCOLLECTIONS_NO_C_EXTENSIONS': '1'}
        for (src_env, dst_env, expect_c) in ((env_py, {}, True),
                                            ({}, env_py, False)):
            dump = _run_python(textwrap.dedent(code).format(
                data=None, expect_c=None), src_env)
            self.assertEqual(dump.returncode, 0, dump.stderr)
            load = _run_python(textwrap.dedent(code).format(
                data=dump.stdout, expect_c=expect_c), dst_env)
            self.assertEqual(load.returncode, 0, load.stderr)
            self.assertEqual(load.stdout.strip(), 'ok', load.stderr)


class TestBackendSelection(unittest.TestCase):
    """Tests of the environment variables that choose the backend."""

    def _probe(self, env, extra=''):
        code = """
            import warnings
            warnings.simplefilter('always')
            with warnings.catch_warnings(record=True) as w:
                import pcollections as pc
            print(pc.using_c_extension, type(pc.backend_error).__name__,
                  [str(x.category.__name__) for x in w])
        """ + extra
        return _run_python(code, env)

    def test_python_backend_is_silent(self):
        proc = self._probe({'PCOLLECTIONS_NO_C_EXTENSIONS': '1'})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.split(), ['False', 'NoneType', '[]'])

    @unittest.skipUnless('c' in BACKENDS, "C backend not built")
    def test_c_backend_default(self):
        proc = self._probe({})
        self.assertEqual(proc.stdout.split(), ['True', 'NoneType', '[]'],
                         proc.stderr)
        proc = self._probe({'PCOLLECTIONS_REQUIRE_C': '1'})
        self.assertEqual(proc.stdout.split(), ['True', 'NoneType', '[]'],
                         proc.stderr)

    def test_require_c_with_no_c(self):
        proc = self._probe({'PCOLLECTIONS_NO_C_EXTENSIONS': '1',
                            'PCOLLECTIONS_REQUIRE_C': '1'})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('ImportError', proc.stderr)

    def test_missing_c_backend_warns(self):
        # Block the C module with an import hook.
        code = """
            import sys, importlib.abc, warnings
            class Block(importlib.abc.MetaPathFinder):
                def find_spec(self, name, path=None, target=None):
                    if name == 'pcollections._c._core':
                        raise ImportError('blocked')
            sys.meta_path.insert(0, Block())
            warnings.simplefilter('always')
            with warnings.catch_warnings(record=True) as w:
                import pcollections as pc
            print(pc.using_c_extension, type(pc.backend_error).__name__,
                  [x.category.__name__ for x in w])
            import os
            if os.environ.get('PCOLLECTIONS_REQUIRE_C'):
                print('unreachable')
        """
        proc = _run_python(code)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.split(),
                         ['False', 'ImportError', "['RuntimeWarning']"])
        proc = _run_python(code, {'PCOLLECTIONS_REQUIRE_C': '1'})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('PCOLLECTIONS_REQUIRE_C', proc.stderr)
        self.assertNotIn('unreachable', proc.stdout)


class TestParityHarness(unittest.TestCase):
    """Runs random operations on both backends and compares the results."""

    def setUp(self):
        if 'c' not in BACKENDS:
            self.skipTest("C backend not built")
        self.py, self.c = BACKENDS['python'], BACKENDS['c']

    @staticmethod
    def _norm_outcome(out):
        if out[0] == 'ok':
            return ('ok', TestParityHarness._norm(out[1]))
        (_, etype, msg) = out
        # Argument-count messages come from the interpreter and differ
        # between Python functions and C functions; C type names are
        # module-qualified.
        if etype is TypeError and 'argument' in msg:
            msg = None
        else:
            msg = msg.replace('pcollections.', '')
        return ('err', etype, msg)

    @staticmethod
    def _norm(x):
        # Compare results by type name and contents.
        name = type(x).__name__
        if name in ('pdict', 'tdict', 'ldict', 'tldict'):
            return (name, sorted(x.items(), key=repr))
        if name in ('pset', 'tset'):
            return (name, sorted(x, key=repr))
        if name in ('plist', 'tlist', 'llist', 'tllist'):
            return (name, list(x))
        if isinstance(x, (list, tuple)):
            return (name, [TestParityHarness._norm(y) for y in x])
        if name.endswith('iterator') or name.startswith('dict_'):
            return (name.split('_')[0], None)
        return (name, x)

    def _compare(self, ops, make, seed_count):
        r = random.Random(seed_count)
        for trial in range(seed_count):
            objs = {b: make(B) for (b, B) in (('python', self.py),
                                             ('c', self.c))}
            for step in range(60):
                (name, argfn) = r.choice(ops)
                args = argfn(r)
                res = {}
                for (b, obj) in objs.items():
                    fn = getattr(obj, name)
                    out = _outcome(fn, *args)
                    res[b] = self._norm_outcome(out)
                self.assertEqual(res['python'], res['c'],
                                 (trial, step, name, args))
                self.assertEqual(self._norm(objs['python']),
                                 self._norm(objs['c']))

    def test_tlist_parity(self):
        idx = lambda r: (r.randrange(-8, 8),)
        sl = lambda r: (slice(r.choice([None, r.randrange(-8, 8)]),
                              r.choice([None, r.randrange(-8, 8)]),
                              r.choice([None, 1, -1, 2, 0])),)
        ops = [('append', lambda r: (r.randrange(5),)),
               ('insert', lambda r: (r.randrange(-8, 8), 'i')),
               ('pop', lambda r: r.choice([(), idx(r)])),
               ('remove', lambda r: (r.randrange(5),)),
               ('index', lambda r: (r.randrange(5),)),
               ('count', lambda r: (r.randrange(5),)),
               ('__getitem__', lambda r: r.choice([idx(r), sl(r),
                                                    ('x',)])),
               ('__setitem__', lambda r: r.choice([
                   idx(r) + ('s',), sl(r) + ([1, 2],), sl(r) + (5,)])),
               ('__delitem__', lambda r: r.choice([idx(r), sl(r)])),
               ('extend', lambda r: ([r.randrange(5)] * r.randrange(3),)),
               ('reverse', lambda r: ()),
               ('__contains__', lambda r: (r.randrange(5),)),
               ('__eq__', lambda r: ([0, 1],)),
               ('__lt__', lambda r: ([r.randrange(5)],)),
               ('__mul__', lambda r: (r.choice([2, 'x', 1.0]),)),
               ('__add__', lambda r: (r.choice([[1], (2,), 3]),)),
               ('persistent', lambda r: ())]
        self._compare(ops, lambda B: B['tlist']((0, 1, 2)), 60)

    def test_tdict_parity(self):
        key = lambda r: (r.choice([r.randrange(6), 'k', [1]]),)
        ops = [('__setitem__', lambda r: key(r) + (r.randrange(3),)),
               ('__getitem__', key), ('__delitem__', key),
               ('get', key), ('pop', key),
               ('pop', lambda r: key(r) + (None,)),
               ('popitem', lambda r: ()),
               ('setdefault', lambda r: key(r) + (7,)),
               ('update', lambda r: (r.choice([{1: 2}, [(3, 4)], 5,
                                               [(1,)]]),)),
               ('__or__', lambda r: (r.choice([{1: 2}, [(3, 4)]]),)),
               ('__contains__', key),
               ('__eq__', lambda r: ({0: 1},)),
               ('persistent', lambda r: ())]
        self._compare(ops, lambda B: B['tdict']({0: 0, 1: 1}), 60)

    def test_tset_parity(self):
        el = lambda r: (r.choice([r.randrange(6), 'e', [1]]),)
        other = lambda r: (r.choice([{1, 2}, frozenset((3,)), [1], 5]),)
        ops = [('add', el), ('discard', el), ('remove', el),
               ('pop', lambda r: ()), ('__contains__', el),
               ('update', other), ('difference_update', other),
               ('intersection_update', other),
               ('symmetric_difference_update', other),
               ('__or__', other), ('__and__', other), ('__sub__', other),
               ('__xor__', other), ('__le__', other), ('__lt__', other),
               ('isdisjoint', other), ('issubset', other),
               ('persistent', lambda r: ())]
        self._compare(ops, lambda B: B['tset']((0, 1, 2)), 60)

    def test_persistent_parity(self):
        for (name, ops, init) in (
            ('plist', [('drop', lambda r: r.choice([(), (5,), (1, True),
                                                    ('x',)])),
                       ('delete', lambda r: (r.randrange(-4, 4),)),
                       ('set', lambda r: (r.randrange(-4, 4), 'v')),
                       ('insert', lambda r: (r.randrange(-4, 4), 'v')),
                       ('remove', lambda r: (r.randrange(3),)),
                       ('sort', lambda r: ()), ('clear', lambda r: ())],
             (2, 0, 1)),
            ('pdict', [('drop', lambda r: r.choice([('a',), ('z',),
                                                    ('z', True)])),
                       ('delete', lambda r: (r.choice('az'),)),
                       ('dropall', lambda r: ('az',)),
                       ('deleteall', lambda r: ('az',)),
                       ('set', lambda r: ([],)),
                       ('setall', lambda r: ('q', (1,))),
                       ('__or__', lambda r: ([1],))],
             {'a': 1}),
            ('pset', [('drop', lambda r: r.choice([(1,), (9,), (9, True)])),
                      ('remove', lambda r: (r.choice([1, 9]),)),
                      ('add', lambda r: ([],)),
                      ('discardall', lambda r: ((1, 2),)),
                      ('union', lambda r: (5,))],
             (1, 2))):
            r = random.Random(4)
            for _ in range(100):
                (m, argfn) = r.choice(ops)
                args = argfn(r)
                res = [_outcome(getattr(B[name](init), m), *args)
                       for B in (self.py, self.c)]
                res = [self._norm_outcome(x) for x in res]
                self.assertEqual(res[0], res[1], (name, m, args))


class TestHypothesis(unittest.TestCase):
    """Property tests; skipped when Hypothesis is not installed."""

    def setUp(self):
        try:
            import hypothesis
        except ImportError:
            self.skipTest("hypothesis is not installed")

    def test_properties(self):
        from hypothesis import given, settings, strategies as st
        elems = st.one_of(st.integers(-5, 5), st.text(max_size=2),
                          st.floats(allow_nan=False), st.none())
        for B in BACKENDS.values():
            @settings(max_examples=150, deadline=None)
            @given(st.lists(elems), st.lists(elems))
            def check(xs, ys):
                p = B['pset'](xs)
                self.assertEqual(p, set(xs))
                self.assertEqual(hash(p), hash(frozenset(xs)))
                self.assertEqual(p | B['pset'](ys), set(xs) | set(ys))
                self.assertEqual(p - set(ys), set(xs) - set(ys))
                L = B['plist'](xs)
                self.assertEqual(L, xs)
                self.assertEqual(L + ys, xs + ys)
                d = B['pdict'](zip(xs, ys))
                self.assertEqual(d, dict(zip(xs, ys)))
                self.assertEqual(list(d), list(dict(zip(xs, ys))))
            check()

            @settings(max_examples=150, deadline=None)
            @given(st.lists(st.integers()), st.data())
            def check_slices(xs, data):
                t = B['tlist'](xs)
                ref = list(xs)
                ints = st.one_of(st.none(), st.integers(-20, 20))
                for _ in range(5):
                    s = slice(data.draw(ints), data.draw(ints),
                              data.draw(st.sampled_from([None, 1, -1, 2, -3])))
                    self.assertEqual(list(t[s]), ref[s])
                    if data.draw(st.booleans()):
                        del ref[s]
                        del t[s]
                    else:
                        new = data.draw(st.lists(st.integers(), max_size=4))
                        if s.step not in (None, 1):
                            new = [0] * len(ref[s])
                        ref[s] = new
                        t[s] = new
                    self.assertEqual(list(t), ref)
            check_slices()


make_tests('TestApi', _ApiTests, globals())
