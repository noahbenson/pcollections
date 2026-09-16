# -*- coding: utf-8 -*-
################################################################################
# pcollections/_list.py
# The persistent list type for Python.
# By Noah C. Benson

import operator
from itertools import (chain, islice)


def _index_arg(seq, k):
    """Converts a subscript to an index, raising TypeError as list does."""
    try:
        return operator.index(k)
    except TypeError:
        raise TypeError(f"{type(seq).__name__} indices must be integers or "
                        f"slices, not {type(k).__name__}") from None

# plist/tlist encode their contents as a single dense, (possibly negative-
# indexed, via prepend) integer-keyed trie -- exactly the "insertion-order
# value table" role _c/list.c.h's own FAT plays (see that file and
# pcollections/_trie.py's module docstring for why FAT is, in this pure-
# Python port, an AMT specialized only by name/role rather than a
# structurally distinct trie kind). Imported under the names this file
# already uses throughout (PHAMT/THAMT) so the external `phamt` package
# dependency is removed with no other change needed below.
from ._trie import (
    FAT as PHAMT,
    TFAT as THAMT
)

from .abc import (PersistentSequence, TransientSequence)
from .abc._core import _type_empty, _partner_type
from ._guard import (TListIter, updating, new_busy_flag, begin_update,
                     end_update)


#===============================================================================
# plist

class plist(PersistentSequence):
    """A persistent list type similar to `list`.

    `plist()` returns an empty `plist`.

    `plist(iterable)` returns a `plist` containing the elements in `iterable`.
    """
    empty = None
    __slots__ = ("_phamt", "_start", "_hashcode", "__weakref__")
    @classmethod
    def _empty(cls):
        return _type_empty(cls, lambda: cls._new(PHAMT.empty, 0))
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError(f"{cls.__name__}() takes no keyword arguments")
        n = len(args)
        if n == 0:
            return cls._empty()
        elif n != 1:
            msg = f"{cls.__name__} expects at most 1 argument, got {n}"
            raise TypeError(msg)
        arg = args[0]
        # If arg is a tlist, this is a special case.
        # Storage is shared only with a collection that is not lazy: reading
        # from a lazy collection computes its values (see _lazy.py).
        if type(arg) is cls:
            return arg
        elif getattr(type(arg), '_holds_lazy', False):
            pass
        elif isinstance(arg, tlist):
            (th, start, _) = arg._snapshot()
            if len(th) == 0:
                return cls._empty()
            return cls._new(th, start)
        elif isinstance(arg, plist):
            if len(arg) == 0:
                return cls._empty()
            else:
                return cls._new(arg._phamt, arg._start)
        # We just want to build a PHAMT out of this arg of iterables.
        thamt = THAMT(PHAMT.empty)
        for (ii,val) in enumerate(iter(arg)):
            thamt[ii] = val
        phamt = thamt.persistent()
        # If it's empty, we can just return the empty plist.
        if len(phamt) == 0: return cls._empty()
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
        index = operator.index(index)
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
        index = operator.index(index)
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
                return type(self)._empty()
            else:
                return self._new(phamt.dissoc(st), st + 1)
        elif index == n - 1:
            return self._new(phamt.dissoc(st + index), st)
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
        index = operator.index(index)
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
        return type(self)._empty()
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
            items = [phamt[st + ii] for ii in range(*k.indices(n))]
            if not items:
                return type(self)._empty()
            thamt = THAMT(PHAMT.empty)
            for (ii, x) in enumerate(items):
                thamt[ii] = x
            return self._new(thamt.persistent(), 0)
        k = _index_arg(self, k)
        if k >= n or k < -n:
            raise IndexError("plist index out of range")
        elif k < 0:
            k += n
        return phamt[k + st]
    def transient(self):
        """Efficiently copies the plist into a tlist and returns the tlist."""
        cls = _partner_type(self, '__transient_type__', tlist)
        return cls._new(THAMT(self._phamt), self._start, self)
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
    __slots__ = ("_thamt", "_start", "_orig", "_version", "_busy",
                 "__weakref__")
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("tlist() takes no keyword arguments")
        n = len(args)
        if   n == 1: pass
        elif n == 0: return cls.empty()
        else: raise TypeError(f"tlist expects at most 1 argument, got {n}")
        arg = args[0]
        # If this is a (non-lazy) plist, share its storage.
        if (isinstance(arg, plist)
                and not getattr(type(arg), '_holds_lazy', False)):
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
        object.__setattr__(new_tlist, '_version', 0)
        object.__setattr__(new_tlist, '_busy', new_busy_flag())
        return new_tlist
    def _changed(self):
        object.__setattr__(self, '_version', self._version + 1)
        object.__setattr__(self, '_orig', None)
    @updating
    def clear(self):
        """Clears all elements from the tlist."""
        self._changed()
        object.__setattr__(self, '_thamt', THAMT(PHAMT.empty))
        object.__setattr__(self, '_start', 0)
        object.__setattr__(self, '_orig', None)
    def _snapshot(self):
        """Returns `(trie, start, orig)`, with the trie frozen, for sharing
        with another collection."""
        begin_update(self, False)
        try:
            return (self._thamt.persistent(), self._start, self._orig)
        finally:
            end_update(self)
    def _persistent_as(self, cls):
        (th, start, orig) = self._snapshot()
        if type(orig) is cls:
            return orig
        elif len(th) == 0:
            return cls._empty()
        else:
            return cls._new(th, start)
    def persistent(self):
        """Efficiently copies the tlist into a plist and returns the plist."""
        return self._persistent_as(
            _partner_type(self, '__persistent_type__', plist))
    def __iter__(self):
        return TListIter(self)
    def _raw_item(self, k, v=None):
        # Returns element k (already normalized), checking that the list has
        # not changed since version v.
        if v is None:
            v = self._version
        try:
            if not (v & 1):
                x = self._thamt[k + self._start]
                if self._version == v:
                    return x
        except LookupError:
            # Another thread changed the list as we read it.
            if self._version == v:
                raise
        raise RuntimeError(f"{type(self).__name__} changed during a lookup")
    def _fast_iter(self):
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
        v = self._version
        n = len(self._thamt)
        if isinstance(k, slice):
            items = [self._raw_item(ii, v) for ii in range(*k.indices(n))]
            thamt = THAMT(PHAMT.empty)
            for (ii, x) in enumerate(items):
                thamt[ii] = x
            return self._new(thamt, 0)
        k = _index_arg(self, k)
        if k >= n or k < -n:
            raise IndexError("tlist index out of range")
        elif k < 0:
            k += n
        return self._raw_item(k, v)
    def _raw_list(self):
        th = self._thamt
        st = self._start
        return [th[st + ii] for ii in range(len(th))]
    def _replace(self, items):
        # Replaces the contents with the list `items` (under the guard).
        self._changed()
        thamt = THAMT(PHAMT.empty)
        for (ii, x) in enumerate(items):
            thamt[ii] = x
        object.__setattr__(self, '_thamt', thamt)
        object.__setattr__(self, '_start', 0)
    def __setitem__(self, k, v):
        if isinstance(k, slice):
            # Slices are assigned as for a list: the elements are copied
            # into a list, which is changed, and the tlist is rebuilt.
            try:
                v = list(v)
            except TypeError:
                raise TypeError("can only assign an iterable") from None
            self._set_slice(k, v)
            return
        self._set_index(_index_arg(self, k), v)
    @updating
    def _set_slice(self, k, v):
        items = self._raw_list()
        items[k] = v
        self._replace(items)
    @updating
    def _set_index(self, k, v):
        n = len(self._thamt)
        if k >= n or k < -n:
            raise IndexError("tlist index out of range")
        elif k < 0:
            k += n
        self._changed()
        self._thamt[k + self._start] = v
    def __delitem__(self, index=-1):
        """Remove the item at index (default last).

        Raises IndexError if list is empty or index is out of range."""
        if isinstance(index, slice):
            self._del_slice(index)
        else:
            self._del_index(_index_arg(self, index))
    @updating
    def _del_slice(self, k):
        items = self._raw_list()
        del items[k]
        self._replace(items)
    @updating
    def _del_index(self, index):
        st = self._start
        th = self._thamt
        n = len(th)
        if index >= n or index < -n:
            raise IndexError("tlist index out of range")
        elif index < 0:
            index += n
        self._changed()
        if n - index <= index:
            for ii in range(index + st, n + st - 1):
                th[ii] = th[ii + 1]
            del th[n + st - 1]
        else:
            for ii in range(index + st, st, -1):
                th[ii] = th[ii - 1]
            del th[st]
            self._start += 1
    @updating
    def append(self, obj):
        """Appends object to the end of the list."""
        thamt = self._thamt
        n = len(thamt)
        self._changed()
        thamt[n + self._start] = obj
    @updating
    def prepend(self, obj):
        """Prepends object to the beginning of the tlist."""
        thamt = self._thamt
        self._changed()
        self._start -= 1
        thamt[self._start] = obj
    @updating
    def insert(self, index, obj):
        """Inserts the given object before the given index."""
        index = operator.index(index)
        st = self._start
        th = self._thamt
        n = len(th)
        if   index < -n: index = -n
        elif index > n:  index = n
        if   index < 0:  index += n
        self._changed()
        if n - index <= index:
            for ii in range(n + st, index + st, -1):
                th[ii] = th[ii - 1]
            th[index + st] = obj
        else:
            for ii in range(st - 1, index + st - 1):
                th[ii] = th[ii + 1]
            self._start -= 1
            th[index + self._start] = obj


plist.__transient_type__ = tlist
tlist.__persistent_type__ = plist
