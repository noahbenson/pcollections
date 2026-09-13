# -*- coding: utf-8 -*-
################################################################################
# pcollections/_list.py
# The persistent list type for Python.
# By Noah C. Benson

from itertools import (chain, islice)

from phamt import (PHAMT,THAMT)

from .abc import (PersistentSequence, TransientSequence)


#===============================================================================
# plist

class plist(PersistentSequence):
    """A persistent list type similar to `list`.

    `plist()` returns an empty `plist`.

    `plist(iterable)` returns a `plist` containing the elements in `iterable`.
    """
    empty = None
    __slots__ = ("_phamt", "_start", "_hashcode")
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError(f"{cls.__name__}() takes no keyword arguments")
        n = len(args)
        if n == 0:
            return cls.empty
        elif n != 1:
            msg = f"{cls.__name__} expects at most 1 argument, got {n}"
            raise TypeError(msg)
        arg = args[0]
        # If arg is a tlist, this is a special case.
        if isinstance(arg, tlist):
            if len(arg) == 0:
                return cls.empty
            else:
                return cls._new(arg._thamt.persistent(), arg._start)
        elif isinstance(arg, cls):
            return arg
        elif isinstance(arg, plist):
            if len(arg) == 0:
                return cls.empty
            else:
                return cls._new(arg._phamt, arg._start)        
        # We just want to build a PHAMT out of this arg of iterables.
        thamt = THAMT(PHAMT.empty)
        for (ii,val) in enumerate(iter(arg)):
            thamt[ii] = val
        phamt = thamt.persistent()
        # If it's empty, we can just return the empty plist.
        if len(phamt) == 0: return cls.empty
        # Otherwise, we make a new plist and give it this phamt.
        return cls._new(phamt, 0)
    @classmethod
    def _new(cls, phamt, start):
        new_plist = super(plist, cls).__new__(cls)
        object.__setattr__(new_plist, '_phamt', phamt)
        object.__setattr__(new_plist, '_start', start)
        object.__setattr__(new_plist, '_hashcode', None)
        return new_plist
    def set(self, index, obj):
        """Returns a copy of the list with the given index set to the given
        object."""
        start = self._start
        phamt = self._phamt
        n = len(phamt)
        if index < -n or index >= n:
            raise IndexError("plist index out of range")
        if index < 0:
            index += n
        index += start
        if phamt[index] is obj:
            return self
        new_phamt = phamt.assoc(index, obj)
        return self._new(new_phamt, start)
    def delete(self, index=-1):
        """"Returns a copy of the plist with the item at index removed (default
        index: last)."""
        phamt = self._phamt
        n = len(phamt)
        st = self._start
        if n == 0:
            raise IndexError("delete from empty plist")
        if index >= n or index < -n:
            raise IndexError("plist.delete index out of range")
        elif index < 0:
            index += n
        # Some cases don't require a THAMT:
        if index == 0:
            if n == 1:
                return plist.empty
            else:
                return plist._new(phamt.dissoc(st), st + 1)
        elif index == n - 1:
            return plist._new(phamt.dissoc(st + index), st)
        # Other cases require moving elements around a fair bit, so we use a
        # THAMT object.
        thamt = THAMT(phamt)
        if n - index <= index:
            for ii in range(index + st, n + st - 1):
                thamt[ii] = phamt[ii + 1]
            del thamt[n + st - 1]
        else:
            for ii in range(st, index + st):
                thamt[ii + 1] = phamt[ii]
            del thamt[st]
            st += 1
        return self._new(thamt.persistent(), st)
    def append(self, obj):
        """Returns a new list with object appended."""
        phamt = self._phamt
        n = len(phamt)
        new_phamt = phamt.assoc(self._start + n, obj)
        return self._new(new_phamt, self._start)
    def prepend(self, obj):
        """Returns a new list with object prepended."""
        phamt = self._phamt
        n = len(phamt)
        new_start = self._start - 1
        new_phamt = phamt.assoc(new_start, obj)
        return self._new(new_phamt, new_start)
    def insert(self, index, obj):
        """Returns a new plist with object inserted before index."""
        start = self._start
        phamt = self._phamt
        n = len(phamt)
        if   index == 0: return self.prepend(obj)
        elif index == n: return self.append(obj)
        elif index < -n: index = -n
        elif index > n:  index = n
        if   index < 0:  index += n
        thamt = THAMT(phamt)
        if n - index <= index:
            for ii in range(index + start, n + start):
                thamt[ii + 1] = phamt[ii]
            thamt[index + start] = obj
        else:
            for ii in range(start, index + start):
                thamt[ii - 1] = phamt[ii]
            start -= 1
            thamt[index + start] = obj
        phamt = thamt.persistent()
        return self._new(phamt, start)
    def clear(self):
        """Returns the empty plist."""
        return plist.empty
    def __iter__(self):
        phamt = self._phamt
        n = len(phamt)
        st = self._start
        if st >= 0 or n + st < 0:
            return map(lambda u:u[1], self._phamt)
        else:
            from itertools import chain, islice
            return chain((phamt[k] for k in range(st, 0)),
                         islice(map(lambda u:u[1], phamt), 0, n + st))
    def __len__(self):
        """Returns the length of the plist."""
        return len(self._phamt)
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
            return self._new(thamt.persistent(), 0)
        elif k >= n or k < -n:
            raise IndexError("plist index out of range")
        elif k < 0:
            k += n
        return phamt[k + st]
    def transient(self):
        """Efficiently copes the plist into a tlist and returns the tlist."""
        return tlist._new(THAMT(self._phamt), self._start, self)
    # We redefine the hash function in order to use the _hashcode member.
    def __hash__(self):
        if self._hashcode is None:
            h = PersistentSequence.__hash__(self)
            object.__setattr__(self, '_hashcode', h)
        return self._hashcode
# Setup the plist.empty static member.
plist.empty = plist._new(PHAMT.empty, 0)


#===============================================================================
# tlist
# The transient list type.

class tlist(TransientSequence):
    """A transient list type, similar to `list`, for mutating persistent lists.

    `tlist()` returns an empty `tlist`.

    `tlist(iterable)` returns a `tlist` containing the elements in `iterable`.

    `tlist(p)` returns `tlist` equivalent to the `plist` p in constnat time.
    The returned `tlist` object can be efficiently mutated in-place then
    efficiently converted into a persistent list by calling the `persistent()`
    method.
    """
    @classmethod
    def empty(cls):
        "Returns an empty tlist."
        return cls._new(THAMT(PHAMT.empty), 0)
    __slots__ = ("_thamt", "_start", "_orig")
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("tlist() takes no keyword arguments")
        n = len(args)
        if   n == 1: pass
        elif n == 0: return cls.empty()
        else: raise TypeError(f"tlist expects at most 1 argument, got {n}")
        arg = args[0]
        # If this is a plist, we know what to do with it.
        if isinstance(arg, plist):
            return cls._new(THAMT(arg._phamt), arg._start)
        # We just want to build a THAMT out of this arg of iterables.
        thamt = THAMT(PHAMT.empty)
        for (ii,val) in enumerate(iter(arg)):
            thamt[ii] = val
        # We make a new tlist and give it this phamt.
        return cls._new(thamt, 0)
    @classmethod
    def _new(cls, thamt, start, orig=None):
        new_tlist = super(tlist, cls).__new__(cls)
        object.__setattr__(new_tlist, '_thamt', thamt)
        object.__setattr__(new_tlist, '_start', start)
        object.__setattr__(new_tlist, '_orig', orig)
        return new_tlist
    def clear(self):
        """Clears all elements from the tlist."""
        object.__setattr__(self, '_thamt', THAMT(PHAMT.empty))
        object.__setattr__(self, '_start', 0)
        object.__setattr__(self, '_orig', None)
    def persistent(self):
        """Efficiently copies the tlist into a plist and returns the plist."""
        if len(self._thamt) == 0:
            return plist.empty
        elif self._orig is None:
            return plist._new(self._thamt.persistent(), self._start)
        else:
            return self._orig
    def __iter__(self):
        st = self._start
        th = self._thamt
        n = len(th)
        if st >= 0 or n + st < 0:
            return map(lambda u:u[1], iter(th))
        else:
            return chain((th[k] for k in range(st, 0)),
                         islice(map(lambda u:u[1], iter(th)), 0, n + st))
    def __len__(self):
        """Returns the length of the tlist."""
        return len(self._thamt)
    def __getitem__(self, k):
        st = self._start
        thamt = self._thamt
        n = len(thamt)
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
            new_thamt = THAMT(PHAMT.empty)
            for (ii,jj) in enumerate(range(start, stop, step)):
                new_thamt[ii] = thamt[jj]
            return self._new(new_thamt, 0)
        elif k >= n or k < -n:
            raise IndexError("tlist index out of range")
        elif k < 0:
            k += n
        return thamt[k + st]
    def __setitem__(self, k, v):
        n = len(self._thamt)
        if k >= n or k < -n:
            raise IndexError(k)
        elif k < 0:
            k += n
        self._thamt[k + self._start] = v
        object.__setattr__(self, '_orig', None)
    def __delitem__(self, index=-1):
        """Remove and return item at index (default last).

        Raises IndexError if list is empty or index is out of range."""
        st = self._start
        th = self._thamt
        n = len(th)
        if index >= n or index < -n:
            raise IndexError(f"{type(self)} assignment index out of range")
        elif index < 0:
            index += n
        if n - index <= index:
            for ii in range(index + st, n + st - 1):
                th[ii] = th[ii + 1]
            del th[n + st - 1]
        else:
            for ii in range(index + st, st, -1):
                th[ii] = th[ii - 1]
            del th[st]
            self._start += 1
        object.__setattr__(self, '_orig', None)
    def append(self, obj):
        """Appends object to the end of the list."""
        thamt = self._thamt
        n = len(thamt)
        thamt[n + self._start] = obj
        object.__setattr__(self, '_orig', None)
    def prepend(self, obj):
        """Prepends object to the beginning of the tlist."""
        thamt = self._thamt
        n = len(thamt)
        self._start -= 1
        thamt[self._start] = obj
        object.__setattr__(self, '_orig', None)
    def insert(self, index, obj):
        """Inserts the given object before the given index."""
        st = self._start
        th = self._thamt
        n = len(th)
        if   index < -n: index = -n
        elif index > n:  index = n
        if   index < 0:  index += n
        if n - index <= index:
            for ii in range(n + st, index + st, -1):
                th[ii] = th[ii - 1]
            th[index + st] = obj
        else:
            for ii in range(st - 1, index + st - 1):
                th[ii] = th[ii + 1]
            self._start -= 1
            th[index + self._start] = obj
        object.__setattr__(self, '_orig', None)
