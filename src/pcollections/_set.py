# -*- coding: utf-8 -*-
################################################################################
# pcollections/_set.py
# The persistent set type for Python.
# By Noah C. Benson

from phamt import (PHAMT,THAMT)

from .abc  import (PersistentSet, TransientSet)
from .util import (setcmp)


#===============================================================================
# pset
# The persistent set type.

class pset(PersistentSet):
    """A persistent set type similar to `set` or `frozenset`.

    `pset()` returns the empty `pset`.

    `pset(iterable)` returns a `pset` containing the elements in `iterable`.

    Unlike `set`, `pset` is immutable; unlike `frozenset`, `pset` supports the
    efficient creation of new `pset` objects that are identical but with small
    changes such as the inclusion of an additional element.
    """
    empty = None
    @classmethod
    def _new(cls, els, idx, top):
        new_pset = super(pset, cls).__new__(cls)
        object.__setattr__(new_pset, '_els', els)
        object.__setattr__(new_pset, '_idx', idx)
        object.__setattr__(new_pset, '_top', top)
        object.__setattr__(new_pset, '_hashcode', None)
        return new_pset
    __slots__ = ("_els", "_idx", "_top", "_hashcode")
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("pset() takes no keyword arguments")
        n = len(args)
        if n == 1:
            arg = args[0]
        elif n == 0:
            return pset.empty
        else:
            raise TypeError(f"pset expects at most 1 argument, got {n}")
        # If arg is a tset, this is a special case.
        if isinstance(arg, tset):
            if len(arg) == 0:
                return cls.empty
            else:
                return cls._new(arg._els.persistent(),
                                arg._idx.persistent(),
                                arg._top)
        # If it's a pset, we can just return it as-is.
        if isinstance(arg, pset):
            return arg
        # For anything else, however, we just route this through tset.
        return tset(arg).persistent()
    def __len__(self):
        return len(self._els)
    def __contains__(self, el):
        h = hash(el)
        ii = self._idx.get(h, None)
        while ii is not None:
            (x,ii) = self._els[ii]
            if el == x:
                return True
        return False
    def __iter__(self):
        return map(lambda u: u[1][0], self._els)
    def __hash__(self):
        if self._hashcode is None:
            h = PersistentSet.__hash__(self)
            object.__setattr__(self, '_hashcode', h)
        return self._hashcode
    def transient(self):
        """Returns a transient copy of the set in constant time."""
        return tset._new(THAMT(self._els), THAMT(self._idx), self._top, self)
    def add(self, obj):
        """Returns a copy of the pset that includes the given object."""
        # Get the hash and initial index (if there is one).
        h = hash(obj)
        ii = self._idx.get(h, None)
        if ii is None:
            # The object's hash is not here yet, so we can append to els and
            # insert it into idx.
            new_els = self._els.assoc(self._top, (obj, None))
            new_idx = self._idx.assoc(h, self._top)
        else:
            # First make sure it's not already in the set.
            ii_prev = None
            while ii is not None:
                (x,ii_next) = self._els[ii]
                if obj == x:
                    return self
                ii_prev = ii
                x_prev = x
                ii = ii_next
            # If we reach this point, we can add the object to the end of the
            # list.
            new_els = self._els.assoc(self._top, (obj, None))
            new_els = new_els.assoc(ii_prev, (x_prev, self._top))
        return self._new(new_els, new_idx, self._top + 1)
    def discard(self, obj):
        """Returns a copy of the pset that does not include the given object.

        If the element is not a member, `discard` returns the original pset.
        """
        # Get the hash and initial index (if there is one).
        h = hash(obj)
        ii = self._idx.get(h, None)
        # First make sure it's not already in the set.
        ii_prev = None
        x_prev = None
        while ii is not None:
            (x,ii_next) = self._els[ii]
            if obj == x:
                # We remove this object!
                if ii_prev is None:
                    # We're removing from the front of the list.
                    if ii_next is None:
                        new_idx = self._idx.dissoc(h)
                    else:
                        new_idx = self._idx.assoc(h, ii_next)
                    new_els = self._els
                else:
                    # We're removing from the end or the middle.
                    new_idx = self._idx
                    new_els = self._els.assoc(ii_prev, (x_prev, ii_next))
                new_els = new_els.dissoc(ii)
                return self._new(new_els, new_idx, self._top)
            ii_prev = ii
            x_prev = x
            ii = ii_next
        # If we reach this point, then obj isn't in the set, so we just return
        # self unchanged.
        return self
    def clear(self):
        """Returns the empty pset."""
        return pset.empty
# Make the empty pset.
pset.empty = pset._new(PHAMT.empty, PHAMT.empty, 0)


#===============================================================================
# tset
# The transient set type.

class tset(TransientSet):
    """A transient set type similar to `set` for mutating persistent sets.

    `tset()` returns the empty `tset`.

    `tset(iterable)` returns a `tset` containing the elements in `iterable`.

    `tset(p)` efficiently returns a transient copy of the persistent set `p`.

    Unlike `set`, `tset` is immutable; unlike `frozenset`, `tset` supports the
    efficient creation of new `tset` objects that are identical but with small
    changes such as the inclusion of an additional element.
    """
    @classmethod
    def _new(cls, els, idx, top, orig=None):
        new_tset = super(tset, cls).__new__(cls)
        object.__setattr__(new_tset, '_els', els)
        object.__setattr__(new_tset, '_idx', idx)
        object.__setattr__(new_tset, '_top', top)
        object.__setattr__(new_tset, '_orig', orig)
        return new_tset
    @classmethod
    def empty(cls):
        """Returns an empty tset."""
        return cls._new(THAMT(PHAMT.empty), THAMT(PHAMT.empty), 0)
    __slots__ = ("_els", "_idx", "_top", "_orig")
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("tset() takes no keyword arguments")
        n = len(args)
        if n == 1:
            pass
        elif n == 0:
            return cls.empty()
        else:
            raise TypeError(f"tset expects at most 1 argument, got {n}")
        arg = args[0]
        # If arg is a pset, this is a special case.
        if isinstance(arg, pset):
            return cls._new(THAMT(arg._els), THAMT(arg._idx), arg._top)
        # For anything else, however, we just build up.
        t = cls.empty()
        t.addall(arg)
        return t
    def __len__(self):
        return len(self._els)
    def __contains__(self, el):
        h = hash(el)
        ii = self._idx.get(h, None)
        while ii is not None:
            (x,ii) = self._els[ii]
            if el == x:
                return True
        return False
    def __iter__(self):
        return map(lambda u: u[1][0], self._els)
    def add(self, obj):
        """Returns a copy of the tset that includes the given object."""
        # Get the hash and initial index (if there is one).
        h = hash(obj)
        ii_first = self._idx.get(h, None)
        if ii_first is None:
            # The object's hash is not here yet, so we can append to els and
            # insert it into idx.
            self._els[self._top] = (obj, None)
            self._idx[h] = self._top
        else:
            # First make sure it's not already in the set.
            ii = ii_first
            ii_prev = None
            while ii is not None:
                (x,ii_next) = self._els[ii]
                if obj == x:
                    return None
                ii_prev = ii
                x_prev = x
                ii = ii_next
            # If we reach this point, we can add the object to the end of the
            # list.
            self._els[self._top] = (obj, None)
            self._els[ii_prev] = (x_prev, self._top)
        object.__setattr__(self, '_top', self._top + 1)
        object.__setattr__(self, '_orig', None)
    def discard(self, obj):
        """Discards the given object from the set.

        If the element is not a member, `discard` simply returns.
        """
        # Get the hash and initial index (if there is one).
        h = hash(obj)
        ii = self._idx.get(h, None)
        # First make sure it's not already in the set.
        ii_prev = None
        x_prev = None
        while ii is not None:
            (x,ii_next) = self._els[ii]
            if obj == x:
                # We remove this object!
                if ii_prev is None:
                    # We're removing from the front of the list.
                    if ii_next is None:
                        del self._idx[h]
                    else:
                        self._idx[h] = ii_next
                    del self._els[ii]
                else:
                    # We're removing from the end or the middle.
                    del self._els[ii]
                    self._els[ii_prev] = (x_prev, ii_next)
                object.__setattr__(self, '_orig', None)
                return None
            ii_prev = ii
            x_prev = x
            ii = ii_next
        # If we reach this point, then obj isn't in the set, so we just return.
        return None
    def clear(self):
        """Clears the tset."""
        object.__setattr__(self, '_els', THAMT(PHAMT.empty))
        object.__setattr__(self, '_idx', THAMT(PHAMT.empty))
        object.__setattr__(self, '_top', 0)
        object.__setattr__(self, '_orig', None)
    def persistent(self):
        """Efficiently returns a persistent set that is a copy of the tset."""
        if len(self) == 0:
            return pset.empty
        elif self._orig is None:
            return pset._new(self._els.persistent(),
                             self._idx.persistent(),
                             self._top)
        else:
            return self._orig


