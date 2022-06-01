# -*- coding: utf-8 -*-
################################################################################
# pcollections/_list.py
# The persistent list type for Python.
# By Noah C. Benson

from phamt           import (PHAMT,THAMT)
from collections.abc import (Sequence,Collection,Reversible,Iterable,Hashable)

def first(obj):
    """Returns the first element of an object."""
    try: return obj[0]
    except TypeError: pass
    return next(iter(obj))
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
        thamt = THAMT(PHAMT.empty)
        for (ii,val) in enumerate(iter(arg)):
            thamt[ii] = val
        phamt = thamt.persistent()
        # If it's empty, we can just return the empty plist.
        if len(phamt) == 0: return plist.empty
        # Otherwise, we make a new plist and give it this phamt.
        new_plist = Sequence.__new__(plist)
        object.__setattr__(new_plist, '_phamt', phamt)
        object.__setattr__(new_plist, '_start', 0)
        return new_plist
    def _new_plist(self, phamt, start):
        cls = type(self)
        new_plist = super(plist, cls).__new__(cls)
        object.__setattr__(new_plist, '_phamt', phamt)
        object.__setattr__(new_plist, '_start', start)
        return new_plist
    def __setattr__(self, k, v):
        raise TypeError("plist is immutable")
    def __setitem__(self, k, v):
        raise TypeError("plist is immutable")
    def __getitem__(self, k):
        st = self._start
        phamt = self._phamt
        n = len(phamt)
        if isinstance(k, slice):
            (start,stop,step) = (k.start or 0, k.stop or n, k.step or 1)
            if   start <= -n: start = 0
            elif start < 0:   start += n
            elif start > n:   start = n
            if   stop <= -n: stop = 0
            elif stop < 0:   stop += n
            elif stop > n:   stop = n
            if st != 0:
                start += st
                stop += st
            thamt = THAMT(PHAMT.empty)
            for (ii,jj) in enumerate(range(start, stop, step)):
                thamt[ii] = phamt[jj]
            return self._new_plist(thamt.persistent(), 0)
        elif k >= n or k < -n:
            raise IndexError("plist index out of range")
        elif k < 0:
            k += n
        return phamt[k + st]
    def __len__(self):
        """Returns the length of the plist."""
        return len(self._phamt)
    def append(self, obj):
        """Returns a new list with object appended."""
        phamt = self._phamt
        n = len(phamt)
        new_phamt = phamt.assoc(n, obj)
        return self._new_plist(new_phamt, self._start)
    def prepend(self, obj):
        """Returns a new list with object prepended."""
        phamt = self._phamt
        n = len(phamt)
        new_start = self._start - 1
        new_phamt = phamt.assoc(new_start, obj)
        return self._new_plist(new_phamt, new_start)
    def clear(self):
        """Returns the empty plist."""
        return plist.empty
    def copy(self):
        """Returns the plist (persistent lists needn't be copied)."""
        return self
    def count(self, value):
        """Returns the number of occurences of value."""
        n = 0
        for (k,obj) in self._phamt:
            if obj == value: n += 1
        return n
    def extend(self, iterable):
        """Return a new list with the iterables appended."""
        cls = type(self)
        phamt = self._phamt
        thamt = THAMT(phamt)
        n = len(phamt)
        for (ii,val) in enumerate(iter(iterable), start=n):
            thamt[ii] = val
        phamt = thamt.persistent()
        return self._new_plist(phamt, self._start)
    def intend(self, iterable):
        """Return a new list with the iterables prepended."""
        cls = type(self)
        phamt = self._phamt
        start = self._start
        thamt = THAMT(phamt)
        n = len(phamt)
        if not isinstance(iterable, (list,plist,tuple)):
            iterable = list(iter(iterable))
        m = len(iterale)
        for (ii,val) in enumerate(iter(iterable), start=(start - m)):
            thamt[ii] = val
        phamt = thamt.persistent()
        return self._new_plist(phamt, start - m)
    def index(self, value, start=0, stop=None):
        """Returns firrst index of value.

        Raises ValueError if value is not present.
        """
        phamt = self._phamt
        st = self._start
        n = len(phamt)
        if stop is None: stop = n+1
        if   start <= -n: start = 0
        elif start < 0:   start += n
        elif start > n:   start = n
        if   stop <= -n: stop = 0
        elif stop < 0:   stop += n
        elif stop > n:   stop = n
        start += st
        stop += st
        for (ii,val) in phamt:
            if   ii < start:   continue
            elif ii >= stop:   break
            elif val == value: return ii - st
        raise ValueError(f'{value} is not in plist')
    def insert(self, index, obj):
        """Returns a new plist with object inserted before index."""
        start = self._start
        phamt = self._phamt
        n = len(phamt)
        if   index < -n: index = -n
        elif index > n:  index = n
        if   index < 0:  index += n
        thamt = THAMT(phamt)
        thamt[index + start] = obj
        #here
        if n - index < index:
            print(' - ', index, start, n)
            for ii in range(index + start, n + start):
                thamt[ii + 1] = phamt[ii]
        else:
            print(' * ', index, start, n)
            for ii in range(start, index + start + 1):
                thamt[ii - 1] = phamt[ii]
            start -= 1
        phamt = thamt.persistent()
        return self._new_plist(phamt, start)
    def pop(self, index=-1):
        "Returns a new plist with the item at index removed (default: last)."
        phamt = self._phamt
        n = len(phamt)
        st = self._start
        if index >= n or index < -n:
            raise IndexError("plist.pop index out of range")
        elif index < 0:
            index += n
        thamt = THAMT(phamt)
        if n - index < index:
            for ii in range(index + st, n + st - 1):
                thamt[ii] = phamt[ii + 1]
            del thamt[ii + st]
        else:
            for ii in range(st, index + st):
                thamt[ii + 1] = phamt[ii]
            del thamt[st]
            st += 1
        return self._new_plist(thamt.persistent(), st)
    def remove(self, value):
        """Returns a new list with the first occurene of value removed.

        Raises ValueError if the value is not present.
        """
        phamt = self._phamt
        n = len(phamt)
        st = self._start
        for (k,val) in phamt:
            if val == value:
                return self.pop(k - st)
        raise ValueError(f"plist.remove(x): x not in plist")
    def reverse(self):
        """Returns a plist that is a reversed copy."""
        phamt = self._phamt
        thamt = THAMT(PHAMT.empty)
        n = len(phamt)
        for (k,val) in phamt:
            thamt[n - k - 1] = val
        return self._new_plist(thamt.persistent(), 0)
    def sort(self, key=None, reverse=False):
        """Returns a sorted copy of the given plist."""
        return plist(sorted(self, key=key, reverse=reverse))
    def __str__(self):
        return f"<{list(self).__str__()}>"
    def __repr__(self):
        return f"<{list(self).__repr__()}>"
    def __add__(self, obj):
        if isinstance(obj, list):
            if len(obj) == 0: return self
            else: return self.extend(obj)
        elif isinstance(obj, plist):
            if   len(obj) == 0:        return self
            elif len(self) == 0:       return obj
            elif len(obj) > len(self): return obj.intend(self)
            else:                      return self.extend(obj)
        else:
            msg = f"unsuppoorted operand type for +: '{type(obj)}' and 'plist'"
            raise TypeError(msg)
    def __radd__(self, obj):
        if isinstance(obj, list):
            return obj + list(self)
        elif isinstance(obj, plist):
            if   len(obj) == 0:        return self
            elif len(self) == 0:       return obj
            if   len(obj) > len(self): return obj.extend(self)
            else:                      return self.intend(obj)
        else:
            msg = f"unsuppoorted operand type for +: '{type(obj)}' and 'plist'"
            raise TypeError(msg)
    def __mul__(self, value):
        from numbers import Integral
        if not isinstance(value, Integral):
            msg = f"can't multiply sequence by non-int of type '{type(value)}'"
            raise ValueError(msg)
        reps = int(value)
        if reps < 0: reps = 0
        if reps == 0: return plist.empty
        elif reps == 1: return self
        phamt = self._phamt
        n = len(phamt)
        start = self._start
        thamt = THAMT(phamt)
        for ii0 in range(n, n*reps, n):
            for (ii,v) in enumerate(self.__iter__()):
                thamt[ii + ii0] = v
        return self._new_plist(thamt.persistent(), start)
    def __rmul__(self, value):
        return self.__mul__(value)
    def __iter__(self):
        st = self._start
        if st >= 0:
            return map(first, iter(self._phamt))
        else:
            from itertools import chain, islice
            phamt = self._phamt
            n = len(phamt)
            return chain((phamt[k] for k in range(st, 0)),
                         islice(map(first, iter(phamt)), 0, n + st))
    def __eq__(self, other):
        if other is self: return True
        if len(self) != len(other): return False
        if type(self) != type(other): return False
        for (a,b) in zip(self.__iter__(), other.__iter__()):
            if a != b: return False
        return True
    def __hash__(self):
        return hash(tuple(self))
# Setup the plist.empty static member.
plist.empty = Sequence.__new__(plist)
object.__setattr__(plist.empty, '_phamt', PHAMT.empty)
object.__setattr__(plist.empty, '_start', 0)
# Register the plist as members of various types.
Collection.register(plist)
Reversible.register(plist)
Iterable.register(plist)
Hashable.register(plist)
