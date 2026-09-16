# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_set.py
# Declaration of tests for the pset and tset types.
# By Noah C. Benson

from random import randint
from unittest import TestCase

from ._backends import make_tests

class _PSetTestMixin:
    """Tests for the `pset` and `tset` classes.

    This both runs a number of simple tests of the `pset` API and a series of
    randomized tests in which `pset` must match the behavior of Python's
    native `set` type.
    """
    def test_api(self):
        """Tests/demonstrates the basic pset API."""
        pset = self.pset
        tset = self.tset
        # An empty pset can be created with `pset()`.
        e = pset()
        self.assertEqual(len(e), 0)
        self.assertIsInstance(e, pset)
        self.assertIs(pset([]), pset.empty)
        self.assertIs(e, pset.empty)
        # A pset can also be created by passing an iterable to the type.
        p1 = pset(range(10))
        self.assertEqual(len(p1), 10)
        for k in range(10):
            self.assertIn(k, p1)
        self.assertNotIn(-1, p1)
        self.assertNotIn(11, p1)
        # psets can be equal to each other and to other sets.
        l1 = set(range(10))
        self.assertEqual(l1, p1)
        self.assertEqual(p1, l1)
        self.assertEqual(p1, pset(l1))
        self.assertNotEqual(pset.empty, [])
        self.assertNotEqual(pset.empty, '')
        # Clearing a pset always yields the empty pset.
        self.assertIs(p1.clear(), pset.empty)
        # Copying a pset always just returns the pset (it is immutable).
        self.assertIs(p1.copy(), p1)
        # psets are hashable.
        self.assertIsInstance(hash(p1), int)
        # They can be iterated.
        self.assertEqual(l1, set(iter(p1)))
        # They have lengths.
        self.assertEqual(len(p1), 10)
        # Adding/discarding elements returns a new pset, leaving the
        # original unchanged.
        p2 = p1.add(100)
        self.assertIn(100, p2)
        self.assertNotIn(100, p1)
        self.assertNotEqual(p1, p2)
        p3 = p2.discard(100)
        self.assertEqual(p3, p1)
        # discard() on a missing element is a no-op (returns the same pset).
        self.assertIs(p1.discard(-500), p1)
        # Set algebra behaves like the builtin set type.
        a = pset([1,2,3,4])
        b = pset([3,4,5,6])
        self.assertEqual(set(a | b), {1,2,3,4,5,6})
        self.assertEqual(set(a & b), {3,4})
        self.assertEqual(set(a - b), {1,2})
        self.assertEqual(set(a ^ b), {1,2,5,6})
        self.assertTrue(pset([1,2]) <= a)
        self.assertTrue(a >= pset([1,2]))
    def test_immutable(self):
        """Ensures that `pset` throws the right errors when one mutates it."""
        pset = self.pset
        l = pset(range(10))
        # `add`/`discard` return copies rather than mutating in place.
        l2 = l.add(10)
        self.assertIn(10, l2)
        self.assertNotIn(10, l)
        # Cannot set-attr.
        with self.assertRaises(TypeError):
            l._top = -10
    def test_random(self):
        "Performs a randomized test on the pset type."
        pset = self.pset
        tset = self.tset
        nops = 100
        valmax = 40
        p = pset()
        t = tset()
        l = set()
        for opnum in range(nops):
            op = randint(0, 9)
            el = randint(0, valmax)
            if op < 5:
                p = p.add(el)
                t.add(el)
                l.add(el)
            else:
                p = p.discard(el)
                t.discard(el)
                l.discard(el)
            self.assertEqual(p, t)
            self.assertEqual(p, l)
            self.assertEqual(l, t)
            if randint(0, 20) == 0:
                tmp = t.persistent()
                self.assertEqual(tmp, p)
                self.assertEqual(tmp, t)
    def test_tset(self):
        """Ensures that tset objects can be used with psets."""
        pset = self.pset
        tset = self.tset
        p = pset([1,2,3])
        t = p.transient()
        self.assertEqual(p, t)
        self.assertIs(type(t), tset)
        self.assertIs(p, t.persistent())
        t.add(4)
        self.assertNotEqual(p, t)
        self.assertIs(type(t.persistent()), pset)
        self.assertIn(4, t)
        self.assertEqual(t.persistent(), t)

make_tests('TestPSet', _PSetTestMixin, globals())
