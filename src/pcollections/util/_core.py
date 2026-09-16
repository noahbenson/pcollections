# -*- coding: utf-8 -*-
################################################################################
# pcollections/util/_core.py
# Implementation of the core pcollections utilities.
# By Noah C. Benson

import sys as _sys
from collections.abc import (Sequence, Set, Mapping)


#===============================================================================
# Utility Functions

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
    n1 = len(set1)
    n2 = len(set2)
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
        return -1 # All elements of set1 are in set2, set1 < set2.
    else:
        return 1 # All elements of set2 are in set1, set1 > set2.

def seqeq(seq1, seq2):
    """Returns `True` if two sequences have the same length and pairwise
    identical-or-equal elements, like list equality."""
    if len(seq1) != len(seq2):
        return False
    for (a, b) in zip(seq1, seq2):
        if not (a is b or a == b):
            return False
    return True

def seqorder(seq1, seq2, op):
    """Compares two sequences with the ordering operator `op` (one of
    `operator.lt`, `le`, `gt`, `ge`), as `list` does: the first pair of
    elements that differ decides, and otherwise the lengths do."""
    for (a, b) in zip(seq1, seq2):
        if not (a is b or a == b):
            return op(a, b)
    return op(len(seq1), len(seq2))

def seqcmp(seq1, seq2):
    """Compares one sequence to another.

    `seqcmp(a, b)` returns -1 if `a` sorts before `b`, 0 if `a` is equal to
    `b`, and 1 if `a` sorts after `b`. Elements are compared as `list` compares
    them: only the first pair that is not identical or equal is ordered.
    """
    for seq in (seq1, seq2):
        if not isinstance(seq, Sequence):
            raise TypeError(
                f"unsupported operand type for compare: {type(seq)}")
    for (a, b) in zip(seq1, seq2):
        if a is b or a == b:
            continue
        return -1 if a < b else 1
    n1 = len(seq1)
    n2 = len(seq2)
    return -1 if n1 < n2 else 1 if n1 > n2 else 0

_HASH_MASK = (1 << _sys.hash_info.width) - 1

def _shuffle_bits(h):
    return (((h ^ 89869747) ^ (h << 16)) * 3644798167) & _HASH_MASK

def _frozenset_hash_of(hashes):
    # CPython's frozenset hash (Objects/setobject.c), given the elements'
    # hashes.
    h = 0
    n = 0
    for eh in hashes:
        h ^= _shuffle_bits(eh & _HASH_MASK)
        n += 1
    h ^= ((n + 1) * 1927868237) & _HASH_MASK
    h ^= (h >> 11) ^ (h >> 25)
    h = (h * 69069 + 907133923) & _HASH_MASK
    if h == _HASH_MASK:
        h = 590923713
    if h > _HASH_MASK >> 1:
        h -= _HASH_MASK + 1
    return h

def _check_frozenset_hash():
    samples = [(), (0,), (1, 2, 3), ('a', 'b'), (-1, -2, 2**70, 0.5),
               tuple(range(100)), (frozenset((1, 2)), (1, 2), None)]
    return all(_frozenset_hash_of(map(hash, s)) == hash(frozenset(s))
               for s in samples)

#: Whether `frozenset_hash` reproduces this interpreter's frozenset hash.
FROZENSET_HASH_MATCHES = _check_frozenset_hash()

def frozenset_hash(iterable):
    """Returns `hash(frozenset(iterable))` for an iterable of distinct
    hashable elements, without building the frozenset."""
    if FROZENSET_HASH_MATCHES:
        return _frozenset_hash_of(map(hash, iterable))
    return hash(frozenset(iterable))

def seqstr(seq, maxlen=None, sep=", ", tostr=repr):
    """Returns a string representation of the given sequence or iterable.

    Mappings are converted to `"key: value"` strings.

    The option `maxlen` may be set to a positive integer to fill in the string
    with an ellipsis in the case that it goes to long; for exaple,
    `seqstr(range(10), 12)` would return `"0, 1, 2 ..."`.
    """
    if isinstance(seq, Mapping):
        orig_tostr = tostr
        tostr = lambda kv: f"{orig_tostr(kv[0])}: {orig_tostr(kv[1])}"
        seq = seq.items()
    if maxlen is None:
        return sep.join(map(tostr, seq))
    elif not isinstance(maxlen, int) or maxlen < 3:
        raise TypeError(f"maxlen must be an integer >= 3 or None, not {maxlen}")
    seplen = len(sep)
    elllen = 3 + seplen
    parts = []
    N = 0
    for el in seq:
        s = tostr(el)
        parts.append(s)
        n = len(s)
        N = n if N == 0 else N + seplen + n
        if N > maxlen:
            # We've overshot; remove the final element if need-be.
            while len(parts) > 0 and N + elllen > maxlen:
                s = parts.pop()
                N -= len(s) + seplen
            parts.append("...")
            break
    # We successfully added all the elements; we should be able to join them
    # at this point.
    return sep.join(parts)
