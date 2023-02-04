# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_set.py
# Declaration of tests for the pset and tset types.
# By Noah C. Benson

from random import randint
from unittest import TestCase

from .._set import (pset, tset)

class TestPSet(TestCase):
    """Tests for the `pset` and `tset` classes.

    This both runs a number of simple tests of the `pset` API and a series of
    randomized tests in which `pset` must match the behavior of Python's
    native `set` type.
    """
    def test_api(self):
        """Tests/demonstrates the basic pset API."""
        # An empty pset can be created with `pset()`.
        e = pset()
        self.assertEqual(len(e), 0)
        self.assertIsInstance(e, pset)
        # This is identical to the pset.empty object.
        self.assertIs(e, pset.empty)
        # A pset can also be created by passing an iterable to the type.
        p1 = pset(range(10))
        self.assertEqual(len(p1), 10)
        # psets can be equal to each other and to other sets (but, like with
        # set, not to other sequences like tuples or strings).
        l1 = set(range(10))
        self.assertEqual(l1, p1)
        self.assertEqual(p1, l1)
        self.assertEqual(p1, pset(range(10)))
        self.assertNotEqual(p1, tuple(p1))
        self.assertNotEqual(pset.empty, '')
        # psets can perform subset comparisonm including the isdisjoint method.
        self.assertLessEqual(p1, p1)
        self.assertLess(p1, set(range(15)))
        self.assertGreater(p1, pset(range(3,7)))
        self.assertGreaterEqual(p1, p1)
        self.assertTrue(p1.isdisjoint(pset(range(20,30))))
        self.assertFalse(p1.isdisjoint(set(range(-5,5))))
        # Clearing a pset always yields the empty pset.
        self.assertIs(p1.clear(), pset.empty)
        # Copying a pset always just returns the pset (it is immutable).
        self.assertIs(p1.copy(), p1)
        # psets are hashable.
        self.assertIsInstance(hash(p1), int)
        # psets contain values.
        self.assertIn(8, p1)
        self.assertNotIn(11, p1)
        # They can be iterated.
        self.assertEqual(p1, set(iter(p1)))
        # They have lengths.
        self.assertEqual(len(p1), 10)
        # Mutating elements is done by creating a copy with or without an item
        # included using the add or discard methods. This leaves the original
        # object unchanged.
        p2 = p1.add(10)
        self.assertNotIn(10, p1)
        self.assertIn(10, p2)
        p2 = p1.discard(9)
        self.assertIn(9, p1)
        self.assertNotIn(9, p2)
        # The remove method can also be used; it throws an error when an item is
        # not already in the set while discard simply returns the set.
        p2 = p1.remove(9)
        self.assertIn(9, p1)
        self.assertNotIn(9, p2)
        with self.assertRaises(KeyError):
            p1.remove(10)
        p2 = p1.discard(20)
        self.assertIs(p1, p2)
        # The pop method can return an arbitrary element as well as a copy of
        # the pset with that element removed.
        (el, p2) = p1.pop()
        self.assertIn(el, p1)
        self.assertNotIn(el, p2)
        self.assertEqual(p2.add(el), p1)
        # There are also batch methods for adding and removing elements.
        p2 = p1.discardall([4,5,6,7,8,9])
        self.assertEqual(set([0,1,2,3]), p2)
        self.assertEqual(p2.addall(range(4,10)), p1)
        self.assertEqual(p2, p1.removeall(range(4,10)))
        # Basic operators exist for psets as well.
        p1 = pset((1,2,3))
        p2 = pset((2,3,4))
        p3 = pset((5,6,7))
        self.assertEqual(p1 | p2, pset(range(1,5)))
        self.assertEqual(p1 | p3, set((1,2,3,5,6,7)))
        self.assertEqual(p3 - p2, p3)
        self.assertEqual(p2 - p1, set([4]))
        self.assertEqual(p1 & p2, set([2,3]))
        self.assertIs(p1 & p3, pset.empty)
    def test_immutable(self):
        """Ensures that `pset` throws the right errors when one mutates it."""
        l = pset(range(10))
        # Cannot set-attr.
        with self.assertRaises(TypeError):
            l._top = -10
    def test_random(self):
        "Performs a randomized test on the pset type."
        nops = 400
        valmax = 20
        p = pset()
        t = tset()
        l = set()
        for opnum in range(nops):
            op = randint(0, 3)
            el = randint(0, valmax)
            if op < 2:
                #print(f"add {el} at {len(l)}")
                p = p.add(el)
                t.add(el)
                l.add(el)
            elif op < 3:
                p = p.discard(el)
                t.discard(el)
                l.discard(el)
            else:
                if el in l:
                    p = p.remove(el)
                    t.remove(el)
                    l.remove(el)
                else:
                    with self.assertRaises(KeyError):
                        p.remove(el)
                    with self.assertRaises(KeyError):
                        t.remove(el)
                    with self.assertRaises(KeyError):
                        l.remove(el)
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
