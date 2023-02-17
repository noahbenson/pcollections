# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_dict.py
# Declaration of tests for the pdict and tdict types.
# By Noah C. Benson

from random import randint
from unittest import TestCase

from .._dict import (pdict, tdict)
from .._lazy import (lazy, ldict)

class TestPDict(TestCase):
    """Tests for the `pdict` and `tdict` classes.

    This both runs a number of simple tests of the `pdict` API and a series of
    randomized tests in which `pdict` must match the behavior of Python's
    native `dict` type.
    """
    def test_api(self):
        """Tests/demonstrates the basic pdict API."""
        # An empty pdict can be created with `pdict()`.
        e = pdict()
        self.assertEqual(len(e), 0)
        self.assertIsInstance(e, pdict)
        # This is identical to the pdict.empty object.
        self.assertIs(e, pdict.empty)
        # A pdict can also be created by passing an iterable to the type.
        p1 = pdict(zip(range(0,10), range(10,20)))
        self.assertEqual(len(p1), 10)
        # Element access is like with the dict type.
        self.assertEqual(p1[0], 10)
        self.assertEqual(p1[5], 15)
        self.assertEqual(p1[9], 19)
        # pdicts can be equal to each other and to other Mappings like dicts.
        l1 = dict(zip(range(0,10), range(10,20)))
        self.assertEqual(l1, p1)
        self.assertEqual(p1, l1)
        self.assertEqual(p1, pdict(l1.items()))
        self.assertNotEqual(p1, p1.keys())
        self.assertNotEqual(pdict.empty, [])
        self.assertNotEqual(pdict.empty, '')
        # Clearing a pdict always yields the empty pdict.
        self.assertIs(p1.clear(), pdict.empty)
        # Copying a pdict always just returns the pdict (it is immutable).
        self.assertIs(p1.copy(), p1)
        # pdicts are hashable.
        self.assertIsInstance(hash(p1), int)
        # pdicts contain their keyss.
        for k in range(0,10):
            self.assertIn(k, p1)
        self.assertNotIn(-1, p1)
        self.assertNotIn(11, p1)
        # They can be iterated.
        self.assertEqual(l1.keys(), set(iter(p1)))
        # They have lengths.
        self.assertEqual(len(p1), 10)
        # Mutating elements is done by creating a copy with the desired mutation
        # using the set method. This leaves the original object unchanged.
        p2 = p1.set(0, 100)
        self.assertEqual(p2[0], 100)
        self.assertEqual(p1[0], 10)
        self.assertNotEqual(p1, p2)
        p2 = p1.set(10, 100)
        self.assertEqual(p2[10], 100)
        self.assertNotIn(10, p1)
        self.assertNotEqual(p1, p2)
        # Deleting elements can be done using the delete or drop methods.
        self.assertEqual(p2.delete(10), p1)
        self.assertEqual(p2.drop(10), p1)
        # Drop and delete only differ in their behavior when the index is
        # invalid.
        with self.assertRaises(KeyError):
            p2.delete(20)
        self.assertEqual(p2, p2.drop(20))
        # The update method returns a merged dictionary of the arguments.
        l2 = dict(p2)
        ext = {4:40,5:50,6:60}
        l2.update(ext)
        self.assertEqual(p2.update({4:40,5:50,6:60}), l2)
        l2 = dict(p2)
        l2.update(a=10, b=20)
        self.assertEqual(p2.update(a=10, b=20), l2)
        # The pop method may be used to extract an item.
        self.assertEqual(p2.pop(1), (11, p2.drop(1)))
        # There are also batch methods for most of the operations.
        self.assertEqual(pdict.empty.setall(range(3), range(0,30,10)),
                         {0:0, 1:10, 2:20})
    def test_immutable(self):
        """Ensures that `pdict` throws the right errors when one mutates it."""
        l = pdict(zip(range(10), range(0,100,10)))
        # Cannot set-item.
        with self.assertRaises(TypeError):
            l[0] = 10
        # Cannot del-item.
        with self.assertRaises(TypeError):
            del l[0]
        # Cannot set-attr.
        with self.assertRaises(TypeError):
            l._top = -10
    def test_random(self):
        "Performs a randomized test on the pdict type."
        nops = 100
        valmax = 1000
        keymax = 200
        p = pdict()
        t = tdict()
        l = dict()
        for opnum in range(nops):
            op = randint(0, 9)
            key = randint(0, keymax)
            el = randint(0, valmax)
            if op < 4:
                ii = randint(0, len(l))
                #print(f"set ({ii}, {el}) at {len(l)}")
                p = p.set(ii, el)
                t[ii] = el
                l[ii] = el
            elif op == 4:
                if len(p) > 0:
                    k = list(l.keys())[randint(0, len(l) - 1)]
                    lel = l.pop(k)
                    #print(f"pop {k} {lel} at {len(l)}")
                    (pel, p) = p.pop(k)
                    tel = t.pop(k)
                    self.assertEqual(pel, tel)
                    self.assertEqual(pel, lel)
            elif op == 5:
                if len(p) > 0:
                    k = list(l.keys())[randint(0, len(l) - 1)]
                    #print(f"drop {k} at {len(l)}")
                    p = p.drop(k)
                    del t[k]
                    del l[k]
            #print('   -', repr(p), ' [', p._start, ']')
            #print('   -', repr(t), ' [', t._start, ']')
            #print('   -', repr(l))
            self.assertEqual(p, t)
            self.assertEqual(p, l)
            self.assertEqual(l, t)
            if randint(0, 20) == 0:
                tmp = t.persistent()
                self.assertEqual(tmp, p)
                self.assertEqual(tmp, t)

class TestLDict(TestCase):
    """Tests for the `ldict` and `lazy` classes.

    Because the `ldict` code is based largely on the `pdict` code, only API
    tests (and not randomized tests) are run for the lazy dictionary type.
    """
    def test_api(self):
        """Tests/demonstrates the basic ldict API."""
        # An empty ldict can be created with `ldict()`.
        e = ldict()
        self.assertEqual(len(e), 0)
        self.assertIsInstance(e, ldict)
        # An ldict is also a pdict.
        self.assertIsInstance(e, pdict)
        # This is identical to the ldict.empty object.
        self.assertIs(e, ldict.empty)
        # ... but not to pdict.empty.
        self.assertIsNot(e, pdict.empty)
        # An ldict can also be created by passing an iterable to the type.
        p1 = ldict(zip(range(0,10), range(10,20)))
        self.assertEqual(len(p1), 10)
        # Element access is like with the dict type.
        self.assertEqual(p1[0], 10)
        self.assertEqual(p1[5], 15)
        self.assertEqual(p1[9], 19)
        # ldicts can be equal to each other and to other Mappings like dicts.
        l1 = dict(zip(range(0,10), range(10,20)))
        self.assertEqual(l1, p1)
        self.assertEqual(p1, l1)
        self.assertEqual(p1, pdict(l1.items()))
        self.assertEqual(p1, ldict(l1.items()))
        self.assertNotEqual(p1, p1.keys())
        self.assertNotEqual(ldict.empty, [])
        self.assertNotEqual(ldict.empty, '')
        # Clearing an ldict always yields the empty ldict.
        self.assertIs(p1.clear(), ldict.empty)
        # Copying an ldict always just returns the ldict (it is immutable).
        self.assertIs(p1.copy(), p1)
        # ldicts are hashable (assuming all values are hashable).
        self.assertIsInstance(hash(p1), int)
        # ldicts contain their keyss.
        for k in range(0,10):
            self.assertIn(k, p1)
        self.assertNotIn(-1, p1)
        self.assertNotIn(11, p1)
        # They can be iterated.
        self.assertEqual(l1.keys(), set(iter(p1)))
        # They have lengths.
        self.assertEqual(len(p1), 10)
        # Mutating elements is done by creating a copy with the desired mutation
        # using the set method. This leaves the original object unchanged.
        p2 = p1.set(0, 100)
        self.assertEqual(p2[0], 100)
        self.assertEqual(p1[0], 10)
        self.assertNotEqual(p1, p2)
        p2 = p1.set(10, 100)
        self.assertEqual(p2[10], 100)
        self.assertNotIn(10, p1)
        self.assertNotEqual(p1, p2)
        # Deleting elements can be done using the delete or drop methods.
        self.assertEqual(p2.delete(10), p1)
        self.assertEqual(p2.drop(10), p1)
        # Drop and delete only differ in their behavior when the index is
        # invalid.
        with self.assertRaises(KeyError):
            p2.delete(20)
        self.assertEqual(p2, p2.drop(20))
        # The update method returns a merged dictionary of the arguments.
        l2 = dict(p2)
        ext = {4:40,5:50,6:60}
        l2.update(ext)
        self.assertEqual(p2.update({4:40,5:50,6:60}), l2)
        l2 = dict(p2)
        l2.update(a=10, b=20)
        self.assertEqual(p2.update(a=10, b=20), l2)
        # The pop method may be used to extract an item.
        self.assertEqual(p2.pop(1), (11, p2.drop(1)))
        # There are also batch methods for most of the operations.
        self.assertEqual(ldict.empty.setall(range(3), range(0,30,10)),
                         {0:0, 1:10, 2:20})
    def test_lazy(self):
        "Tests the lazy aspects of the ldict class."
        # Lazy dictionaries don't evaluate lazy arguments until they are
        # requested, and they only evaluate them once.
        def counter(n):
            counter.count += n
            return counter.count
        counter.count = 0
        p1 = ldict(a=lazy(counter, 1), b=lazy(counter, 10))
        self.assertEqual(counter.count, 0)
        self.assertEqual(p1['a'], 1)
        self.assertEqual(counter.count, 1)
        # Reaccessing the key a will not call counter() again.
        self.assertEqual(p1['a'], 1)
        self.assertEqual(counter.count, 1)
        # However, the key b has not yet been evaluated, so accessing it will
        # call counter().
        self.assertEqual(p1['b'], 11)
        self.assertEqual(counter.count, 11)
        self.assertEqual(p1['b'], 11)
        self.assertEqual(counter.count, 11)
        # Iteration also reifies the values.
        counter.count = 0
        p1 = ldict(a=lazy(counter, 1), b=lazy(counter, 10))
        self.assertEqual(list(p1.values()), [1, 11])
        self.assertEqual(p1['a'], 1)
        self.assertEqual(p1['b'], 11)
        self.assertEqual(counter.count, 11)
        # Converstion into a tdict preserves the lazy items.
        counter.count = 0
        p1 = ldict(a=lazy(counter, 1), b=lazy(counter, 10))
        t1 = p1.transient()
        self.assertIsInstance(t1['a'], lazy)
        self.assertIsInstance(t1['b'], lazy)
        # Conversion back into a lazy dict will hide these.
        p2 = ldict(t1)
        self.assertEqual(p2['a'], 1)
        self.assertEqual(p2['b'], 11)
        # Note that the new dictionary in this case shares the lazy values with
        # the old dictionary.
        self.assertEqual(p1['a'], 1)
        self.assertEqual(p1['b'], 11)
        # Equality comparisons depend on the reified, not lazy, values.
        counter.count = 0
        p1 = ldict(a=lazy(counter, 1), b=lazy(counter, 10))
        self.assertEqual(p1, {'a': 1, 'b': 11})
    def test_immutable(self):
        """Ensures that `pdict` throws the right errors when one mutates it."""
        l = ldict(zip(range(10), range(0,100,10)))
        # Cannot set-item.
        with self.assertRaises(TypeError):
            l[0] = 10
        # Cannot del-item.
        with self.assertRaises(TypeError):
            del l[0]
        # Cannot set-attr.
        with self.assertRaises(TypeError):
            l._top = -10
