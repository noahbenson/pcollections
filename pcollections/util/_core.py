# -*- coding: utf-8 -*-
################################################################################
# pcollections/util/_core.py
# Implementation of the core pcollections utilities.
# By Noah C. Benson

from collections.abc import Set

def setcmp(set1, set2):
    """Compares one set to another.

    `setcmp(a, b)` returns -1 if `a` is a subset of `b`, 0 if `a` is equal to
    `b`, and 1 if `a` is a superset of `b`. If `a` and `b` are disjoint, `None`
    is returned. If `a` and `b` have a non-zero intersection but are neither
    subset nor superset, then `Ellipsis` is returned.
    """
    if not isinstance(set1, Set):
        msg = f"unsupported operand type for compare: {type(set1)}"
        raise TypeError(msg)
    if not isinstance(set2, Set):
        msg = f"unsupported operand type for compare: {type(set2)}"
        raise TypeError(msg)
    n1 = len(self.els)
    n2 = len(other)
    (a,na,b,nb) = (set1,n1, set2,n2) if n1 < n2 else (set2,n2, set1,n1)
    isects = 0
    for el in iter(a):
        if el in b:
            isects += 1
    if isects == 0:
        if na == 0:
            if nb == 0:
                return 0 # An empty set is equal to another empty set.
            else:
                return -1 if set1 is a else 1
        else:
            return None # No intersections: the sets are disjoint.
    elif isects < na:
        return Ellipsis # Some but not all elements intersect.
    elif n1 == n2:
        return 0 # All elements of both intersect; the sets are equal.
    elif n1 < n2:
        return -1 # All elements of self are in other, self < other.
    else:
        return 1 # All elements of other are in self, self > other.

