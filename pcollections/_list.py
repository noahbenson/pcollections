# -*- coding: utf-8 -*-
################################################################################
# pcollections/_list.py
# The persistent list type for Python.
# By Noah C. Benson

from phamt import (PHAMT, THAMT)
from collections.abc import (Sequence, Collection, Reversible, Iterable,
                             Hashable)

class plist(Sequence):
    """A persistent list type similar to `list`.

    `plist()` returns an empty `plist`.

    `plist(iterable)` returns a `plist` containing the elements in `iterable`.
    """
    empty = None
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("plist() takes no keyword arguments")
        n = len(args)
        if   n == 1: pass
        elif n == 0: return plist.empty
        else: raise TypeError(f"plist expects at most 1 argument, got {n}")
        arg = args[0]
        # We just want to build a PHAMT out of this arg of iterables.
        phamt = PHAMT.from_iter(arg)
        # If it's empty, we can just return the empty plist.
        if len(phamt) == 0: return plist.empty
        # Otherwise, we make a new plist and give it this phamt.
        p = super(plist, cls).__new__(cls)
        object.__setattr_(p, '_phamt', phamt)
        object.__setattr_(p, '_start', 0)
        return p
    def __setattr__(self, k, v):
        raise TypeError("plist is immutable")
    def __setitem__(self, k, v):
        raise TypeError("plist is immutable")
    def __getitem__(self, k):
        if isinstance(k, slice):
            p = self._phamt
            n = len(p)
            (start,stop,step) = (k.start or 0, k.stop or n, k.step or 1)
            return plist(p[ii] for ii in range(start, stop, step))
        else:
            return self._phamt[k]
# Setup the plist.empty static member.
plist.empty = super(plist, cls).__new__(cls)
object.__setattr_(p, '_phamt', PHAMT.empty)
object.__setattr_(p, '_start', 0)

Hashable.register(plist)
