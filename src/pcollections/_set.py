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
from .abc._core import _type_empty, _partner_type
from ._guard import (TransientIter, new_busy_flag, begin_update, end_update,
                     changed_during_lookup)
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
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_hashcode",
                 "__weakref__")
    @classmethod
    def _empty(cls):
        return _type_empty(cls, lambda: cls._new(FAT.empty, AMT.empty, 0, 0, 0))
    def __new__(cls, *args, **kw):
        if len(kw) > 0:
            raise TypeError("pset() takes no keyword arguments")
        n = len(args)
        if n == 1:
            arg = args[0]
        elif n == 0:
            return cls._empty()
        else:
            raise TypeError(f"pset expects at most 1 argument, got {n}")
        # An object of exactly this type is returned as-is.
        if type(arg) is cls:
            return arg
        # A tset or pset shares its storage.
        if isinstance(arg, tset):
            (els, idx, top, count, ndeleted, _) = arg._snapshot()
        elif isinstance(arg, pset):
            (els, idx, top, count, ndeleted) = (
                arg._els, arg._idx, arg._top, arg._count, arg._ndeleted)
        else:
            # For anything else, we route this through tset.
            return tset(arg)._persistent_as(cls)
        if count == 0:
            return cls._empty()
        return cls._new(els, idx, top, count, ndeleted)
    def __len__(self):
        return self._count
    def __contains__(self, el):
        h = hash(el)
        ii = self._idx.get(h, None)
        while ii is not None:
            (x,ii) = self._els[ii]
            if x is el or el == x:
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
            # PersistentSet.__hash__ goes through __iter__, which skips
            # tombstones; this override caches the result.
            h = PersistentSet.__hash__(self)
            object.__setattr__(self, '_hashcode', h)
        return self._hashcode
    def transient(self):
        """Returns a transient copy of the set in constant time."""
        cls = _partner_type(self, '__transient_type__', tset)
        return cls._new(TFAT(self._els), TAMT(self._idx), self._top,
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
                if x is obj or obj == x:
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
        if count == 0:
            return cls._empty()
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
            if x is obj or obj == x:
                # We remove this object: unlink it from its collision chain
                # and overwrite its els slot with a tombstone rather than
                # removing the slot (see pcollections/_compact.py).
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
        return type(self)._empty()
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
        object.__setattr__(new_tset, '_version', 0)
        object.__setattr__(new_tset, '_kversion', 0)
        object.__setattr__(new_tset, '_busy', new_busy_flag())
        object.__setattr__(new_tset, '_orig', orig)
        return new_tset
    @classmethod
    def empty(cls):
        """Returns an empty tset."""
        return cls._new(TFAT(FAT.empty), TAMT(AMT.empty), 0, 0, 0)
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_version",
                 "_kversion", "_busy", "_orig", "__weakref__")
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
            orig = arg if (cls is tset and type(arg) is pset) else None
            return cls._new(TFAT(arg._els), TAMT(arg._idx), arg._top,
                            arg._count, arg._ndeleted, orig)
        # For anything else, however, we just build up.
        t = cls.empty()
        t.addall(arg)
        return t
    def __len__(self):
        return self._count
    def _changed(self):
        object.__setattr__(self, '_version', self._version + 1)
        object.__setattr__(self, '_kversion', self._kversion + 1)
        object.__setattr__(self, '_orig', None)
    def _chain(self, h, obj):
        """Walks the collision chain for `obj`, whose hash is `h`; see
        `tdict._chain`. Returns `(ii, ii_next, ii_prev, x_prev)`."""
        v = self._version
        if v & 1:
            changed_during_lookup(self)
        try:
            ii = self._idx.get(h, None)
        except (LookupError, TypeError):
            # Another thread changed the collection as we read it.
            if self._version == v:
                raise
            changed_during_lookup(self)
        ii_prev = x_prev = None
        while ii is not None:
            try:
                (x, ii_next) = self._els[ii]
            except LookupError:
                # Another thread changed the tset as we read it.
                if self._version == v:
                    raise
                changed_during_lookup(self)
            if self._version != v:
                changed_during_lookup(self)
            eq = x is obj or bool(obj == x)
            if self._version != v:
                changed_during_lookup(self)
            if eq:
                return (ii, ii_next, ii_prev, x_prev)
            (ii_prev, x_prev, ii) = (ii, x, ii_next)
        return (None, None, ii_prev, x_prev)
    def __contains__(self, el):
        return self._chain(hash(el), el)[0] is not None
    def __iter__(self):
        return TransientIter(self, _identity)
    def add(self, obj):
        """Adds the given object to the tset."""
        h = hash(obj)
        begin_update(self)
        try:
            self._add(h, obj)
        finally:
            end_update(self)
    def _add(self, h, obj):
        (ii, _, ii_prev, x_prev) = self._chain(h, obj)
        if ii is not None:
            return None
        self._changed()
        top = self._top
        self._els[top] = (obj, None)
        if ii_prev is None:
            # The hash is new: start a chain.
            self._idx[h] = top
        else:
            # Add the element to the end of the chain.
            self._els[ii_prev] = (x_prev, top)
        object.__setattr__(self, '_top', top + 1)
        object.__setattr__(self, '_count', self._count + 1)
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
        h = hash(obj)
        begin_update(self)
        try:
            self._discard(h, obj)
        finally:
            end_update(self)
    def _discard(self, h, obj):
        (ii, ii_next, ii_prev, x_prev) = self._chain(h, obj)
        if ii is None:
            return None
        # We remove this object! (See pset.discard()'s comment on
        # tombstoning.)
        self._changed()
        if ii_prev is None:
            # We're removing from the front of the chain.
            if ii_next is None:
                del self._idx[h]
            else:
                self._idx[h] = ii_next
        else:
            # We're removing from the end or the middle.
            self._els[ii_prev] = (x_prev, ii_next)
        self._els[ii] = (TOMBSTONE, None)
        object.__setattr__(self, '_count', self._count - 1)
        object.__setattr__(self, '_ndeleted', self._ndeleted + 1)
        self._maybe_compact()
        return None
    def clear(self):
        """Clears the tset."""
        begin_update(self)
        try:
            self._changed()
            object.__setattr__(self, '_els', TFAT(FAT.empty))
            object.__setattr__(self, '_idx', TAMT(AMT.empty))
            object.__setattr__(self, '_top', 0)
            object.__setattr__(self, '_count', 0)
            object.__setattr__(self, '_ndeleted', 0)
        finally:
            end_update(self)
    def _snapshot(self):
        """Returns `(els, idx, top, count, ndeleted, orig)`, with the tries
        frozen, for sharing with another collection."""
        begin_update(self, False)
        try:
            return (self._els.persistent(), self._idx.persistent(),
                    self._top, self._count, self._ndeleted, self._orig)
        finally:
            end_update(self)
    def _persistent_as(self, cls):
        (els, idx, top, count, ndeleted, orig) = self._snapshot()
        if type(orig) is cls:
            return orig
        elif count == 0:
            return cls._empty()
        else:
            return cls._new(els, idx, top, count, ndeleted)
    def persistent(self):
        """Efficiently returns a persistent set that is a copy of the tset."""
        return self._persistent_as(
            _partner_type(self, '__persistent_type__', pset))


#===============================================================================
# Compaction.
# Mirrors _dict.py's _compact_els() -- see that function's comment -- just
# replaying live elements through tset.add() instead of tdict.__setitem__.

pset.__transient_type__ = tset
tset.__persistent_type__ = pset


def _identity(x):
    return x

def _compact_els(els):
    t = tset.empty()
    for (_ii, (x, _next)) in els:
        if is_tombstone(x):
            continue
        t.add(x)
    return (t._els.persistent(), t._idx.persistent(), t._top)
