# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_list.py
# Declaration of tests for the plist type.
# By Noah C. Benson

from unittest import TestCase
from .._list import plist

class TestPList(TestCase):
    """Tests for the `plist` class.

    This both runs a number of simple tests of the `plist` API and a series of
    randomized tests in which `plist` must match the behavior of Python's
    native `list` type.
    """
    def test_immutable(self):
        """Ensures that `plist` throws the right errors when one mutates it."""
        l = plist(range(10))
        # Cannot set-item.
        with self.assertRaises(TypeError):
            l[0] = 10
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
