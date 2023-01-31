# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_list.py
# Declaration of tests for the plist type.
# By Noah C. Benson

from unittest import TestCase

from .._list import (plist, tlist)

class TestPList(TestCase):
    """Tests for the `plist` class.

    This both runs a number of simple tests of the `plist` API and a series of
    randomized tests in which `plist` must match the behavior of Python's
    native `list` type.
    """
    def test_api(self):
        """Tests/demonstrates the basic plist API."""
        # An empty plist can be created with `plist()`.
        e = plist()
        self.assertEqual(len(e), 0)
        self.assertIsInstance(e, plist)
        # This is identical to the plist.empty object.
        self.assertIs(e, plist.empty)
        # A plist can also be created by passing an iterable to the type.
        p1 = plist(range(10))
        self.assertEqual(len(p1), 10)
        # Element access is like with the list type.
        self.assertEqual(p1[0], 0)
        self.assertEqual(p1[9], 9)
        self.assertEqual(p1[-2], 8)
        # Slice access is supported (slices are plists)
        self.assertEqual(p1[0:10:2], [0,2,4,6,8])
        self.assertIsInstance(p1[:-1], plist)
        # plists can be equal to each other and to other lists (but, like with
        # list, not to other sequences like tuples or strings).
        l1 = list(range(10))
        self.assertEqual(l1, p1)
        self.assertEqual(p1, l1)
        self.assertEqual(p1, plist(range(10)))
        self.assertNotEqual(p1, tuple(p1))
        self.assertNotEqual(plist.empty, '')
        # plists can perform ordering comparisons like list as well.
        self.assertLessEqual(p1[:4], p1)
        self.assertGreater(p1[4:], p1)
        self.assertLess(p1[:4], p1[6:])
        # Clearing a plist always yields the empty plist.
        self.assertIs(p1.clear(), plist.empty)
        # Copying a plist always just returns the plist (it is immutable).
        self.assertIs(p1.copy(), p1)
        # plists are hashable.
        self.assertIsInstance(hash(p1), int)
        # plists contain values.
        self.assertIn(8, p1)
        self.assertNotIn(11, p1)
        # They can be iterated.
        self.assertEqual(p1, list(iter(p1)))
        # They have lengths.
        self.assertEqual(len(p1), 10)
        # They can count and index items.
        self.assertEqual(p1.count(4), 1)
        self.assertEqual(p1.count(-4), 0)
        self.assertEqual(plist([1,2,3,3,4,5,3,3,6]).count(3), 4)
        self.assertEqual(p1.index(5), 5)
        # The reverse method does not mutate it in-place; instead it returns
        # a reversed plist. This is equivalent to the __reversed__ method.
        self.assertEqual(p1.reverse(), list(reversed(p1)))
        self.assertIsNot(p1.reverse(), p1)
        self.assertIsInstance(p1.reverse(), plist)
        # The sort method also returns a sorted plist.
        self.assertEqual(p1.sort(), p1)
        self.assertEqual(p1.reverse().sort(), p1)
        # Mutating elements is done by creating a copy with the desired mutation
        # using the set method. This leaves the original object unchanged.
        p2 = p1.set(0, 10)
        self.assertEqual(p2[0], 10)
        self.assertEqual(p1[0], 0)
        self.assertGreater(p2, p1)
        self.assertEqual(p1[1:], p2[1:])
        # Deleting elements can be done using the delete or drop methods.
        self.assertEqual(p2[:4].delete(1), [10, 2, 3])
        self.assertEqual(p2[:4].drop(2), [10, 1, 3])
        # Drop and delete only differ in their behavior when the index is
        # invalid.
        with self.assertRaises(IndexError):
            p2.delete(20)
        self.assertEqual(p2, p2.drop(20))
        # Items can be appended and prepended to the lists.
        p3 = plist([1,2,3])
        self.assertEqual(p3.append(4), [1,2,3,4])
        self.assertEqual(p3.prepend(0), [0,1,2,3])
        # The extend method also appends sequences, which is basically the same
        # as list + sequence.
        self.assertEqual(p3.extend([4,5,6]), [1,2,3,4,5,6])
        self.assertEqual(p3.extend([4,5,6]), p3 + [4,5,6])
        self.assertEqual([-1,0] + p3, [-1,0,1,2,3])
        # plists can also be multiplied.
        self.assertEqual(plist([1]) * 5, [1,1,1,1,1])
        self.assertEqual(5 * plist([1]), [1,1,1,1,1])
        # The pop method may be used to extract an item.
        self.assertEqual(p3.pop(1), (2, [1,3]))
        # Finally, the insert method can be used to insert items.
        self.assertEqual(p3.insert(1, 10), [1, 10, 2, 3])
    def test_immutable(self):
        """Ensures that `plist` throws the right errors when one mutates it."""
        l = plist(range(10))
        # Cannot set-item.
        with self.assertRaises(TypeError):
            l[0] = 10
        # Cannot del-item.
        with self.assertRaises(TypeError):
            del l[0]
        # Cannot set-attr.
        with self.assertRaises(TypeError):
            l._start = -10
    def test_mul(self):
        l = plist(range(10))
        # Zero * plist is an empty plist.
        self.assertIs(l * 0, plist.empty)
        self.assertIs(0 * l, plist.empty)
        # List * 2 doubles the list.
        ll = l * 2
        self.assertEqual(l, ll[:10])
        self.assertEqual(l, ll[10:])
        self.assertEqual(ll[:10], l)
        self.assertEqual(ll[10:], l)
        # Cannot multiply by a non-integer
        for notint in ['x', [1], 5.5]:
            with self.assertRaises(ValueError):
                u = l * notint
            with self.assertRaises(ValueError):
                u = notint * l
    def test_add(self):
        l = plist(range(10))
        # A plist plus an empty list is the same plist.
        self.assertIs(l, l + [])
        self.assertIs(l, l + plist.empty)
        self.assertIs(l, plist.empty + l)
        # If the list comes first, the return value is a list.
        self.assertEqual(list(l), [] + l)
        # Doubling a list:
        for ll in (l + l, l + list(l)):
            self.assertEqual(l, ll[:10])
            self.assertEqual(l, ll[10:])
        # Cannot add non-lists to lists.
        for notlist in [5,'5',(5,)]:
            with self.assertRaises(TypeError):
                u = notlist + l
            with self.assertRaises(TypeError):
                u = l + notlist
