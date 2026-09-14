# -*- coding: utf-8 -*-
################################################################################
# pcollections/_set.py
# The persistent set type for Python.
# By Noah C. Benson

from ._trie import (
    AMT,
    TAMT,
    FAT,
    TFAT
)
from ._compact import (
    TOMBSTONE,
    is_tombstone,
    should_compact
)

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
    def _new(cls, els, idx, top, count, ndeleted):
        new_pset = super(pset, cls).__new__(cls)
        object.__setattr__(new_pset, '_els', els)
        object.__setattr__(new_pset, '_idx', idx)
        object.__setattr__(new_pset, '_top', top)
        object.__setattr__(new_pset, '_count', count)
        object.__setattr__(new_pset, '_ndeleted', ndeleted)
        object.__setattr__(new_pset, '_hashcode', None)
        return new_pset
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_hashcode")
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
                                arg._top,
                                arg._count,
                                arg._ndeleted)
        # If it's a pset, we can just return it as-is.
        if isinstance(arg, pset):
            return arg
        # For anything else, however, we just route this through tset.
        return tset(arg).persistent()
    def __len__(self):
        return self._count
    def __contains__(self, el):
        h = hash(el)
        ii = self._idx.get(h, None)
        while ii is not None:
            (x,ii) = self._els[ii]
            if el == x:
                return True
        return False
    def __iter__(self):
        return (
            x
            for (_ii, (x, _next)) in self._els
            if not is_tombstone(x)
        )
    def __hash__(self):
        if self._hashcode is None:
            # PersistentSet.__hash__ (`hash(frozenset(self)) + 1`) already
            # goes through __iter__ (fixed above to skip tombstones), unlike
            # PersistentMapping.__hash__ in _dict.py -- so, unlike pdict,
            # there's nothing tombstone-unsafe to work around here; this
            # override exists purely to cache the result, exactly as before.
            h = PersistentSet.__hash__(self)
            object.__setattr__(self, '_hashcode', h)
        return self._hashcode
    def transient(self):
        """Returns a transient copy of the set in constant time."""
        return tset._new(TFAT(self._els), TAMT(self._idx), self._top,
                         self._count, self._ndeleted, self)
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
            x_prev = None
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
            new_idx = self._idx
        return self._new(new_els, new_idx, self._top + 1, self._count + 1,
                         self._ndeleted)
    @classmethod
    def _maybe_compacted(cls, els, idx, top, count, ndeleted):
        if should_compact(count, ndeleted):
            els, idx, top = _compact_els(els)
            ndeleted = 0
        return cls._new(els, idx, top, count, ndeleted)
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
                # We remove this object! Unlink it from its collision chain
                # but overwrite its els slot with a tombstone rather than
                # actually removing it -- see pcollections/_compact.py, and
                # pdict.drop()'s matching comment in _dict.py, for why.
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
                new_els = new_els.assoc(ii, (TOMBSTONE, None))
                return self._maybe_compacted(
                    new_els, new_idx, self._top,
                    self._count - 1, self._ndeleted + 1)
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
pset.empty = pset._new(FAT.empty, AMT.empty, 0, 0, 0)


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
    def _new(cls, els, idx, top, count, ndeleted, orig=None):
        new_tset = super(tset, cls).__new__(cls)
        object.__setattr__(new_tset, '_els', els)
        object.__setattr__(new_tset, '_idx', idx)
        object.__setattr__(new_tset, '_top', top)
        object.__setattr__(new_tset, '_count', count)
        object.__setattr__(new_tset, '_ndeleted', ndeleted)
        object.__setattr__(new_tset, '_orig', orig)
        return new_tset
    @classmethod
    def empty(cls):
        """Returns an empty tset."""
        return cls._new(TFAT(FAT.empty), TAMT(AMT.empty), 0, 0, 0)
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_orig")
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
            return cls._new(TFAT(arg._els), TAMT(arg._idx), arg._top,
                            arg._count, arg._ndeleted)
        # For anything else, however, we just build up.
        t = cls.empty()
        t.addall(arg)
        return t
    def __len__(self):
        return self._count
    def __contains__(self, el):
        h = hash(el)
        ii = self._idx.get(h, None)
        while ii is not None:
            (x,ii) = self._els[ii]
            if el == x:
                return True
        return False
    def __iter__(self):
        return (
            x
            for (_ii, (x, _next)) in self._els
            if not is_tombstone(x)
        )
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
        object.__setattr__(self, '_count', self._count + 1)
        object.__setattr__(self, '_orig', None)
    def _maybe_compact(self):
        if should_compact(self._count, self._ndeleted):
            new_els, new_idx, new_top = _compact_els(self._els)
            object.__setattr__(self, '_els', TFAT(new_els))
            object.__setattr__(self, '_idx', TAMT(new_idx))
            object.__setattr__(self, '_top', new_top)
            object.__setattr__(self, '_ndeleted', 0)
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
                # We remove this object! (See pset.discard()'s comment on
                # tombstoning.)
                if ii_prev is None:
                    # We're removing from the front of the list.
                    if ii_next is None:
                        del self._idx[h]
                    else:
                        self._idx[h] = ii_next
                    self._els[ii] = (TOMBSTONE, None)
                else:
                    # We're removing from the end or the middle.
                    self._els[ii_prev] = (x_prev, ii_next)
                    self._els[ii] = (TOMBSTONE, None)
                object.__setattr__(self, '_count', self._count - 1)
                object.__setattr__(self, '_ndeleted', self._ndeleted + 1)
                object.__setattr__(self, '_orig', None)
                self._maybe_compact()
                return None
            ii_prev = ii
            x_prev = x
            ii = ii_next
        # If we reach this point, then obj isn't in the set, so we just return.
        return None
    def clear(self):
        """Clears the tset."""
        object.__setattr__(self, '_els', TFAT(FAT.empty))
        object.__setattr__(self, '_idx', TAMT(AMT.empty))
        object.__setattr__(self, '_top', 0)
        object.__setattr__(self, '_count', 0)
        object.__setattr__(self, '_ndeleted', 0)
        object.__setattr__(self, '_orig', None)
    def persistent(self):
        """Efficiently returns a persistent set that is a copy of the tset."""
        if len(self) == 0:
            return pset.empty
        elif self._orig is None:
            return pset._new(self._els.persistent(),
                             self._idx.persistent(),
                             self._top,
                             self._count,
                             self._ndeleted)
        else:
            return self._orig


#===============================================================================
# Compaction.
# Mirrors _dict.py's _compact_els() -- see that function's comment -- just
# replaying live elements through tset.add() instead of tdict.__setitem__.

def _compact_els(els):
    t = tset.empty()
    for (_ii, (x, _next)) in els:
        if is_tombstone(x):
            continue
        t.add(x)
    return (t._els.persistent(), t._idx.persistent(), t._top)
