# -*- coding: utf-8 -*-
################################################################################
# pcollections/_dict.py
# The persistent dict type for Python.
# By Noah C. Benson

from collections.abc import (
    Mapping,
    MutableMapping,
    Set,
    MappingView,
    KeysView,
    ItemsView,
    ValuesView,
    Sized
)

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

from .abc import (
    PersistentMapping,
    TransientMapping
)


#===============================================================================
# pdict
# The persistent dict type.

class PDictView(MappingView):
    __slots__ = ('_mapping')
    def __new__(cls, d):
        if not isinstance(d, pdict):
            raise ValueError(f"can only make {cls} object from pdict")
        obj = super(PDictView, cls).__new__(cls)
        object.__setattr__(obj, '_mapping', d)
        return obj
    def __iter__(self):
        return (
            self._from_kv(kv)
            for (_ii, (kv, _next)) in self._mapping._els
            if not is_tombstone(kv)
        )
    def __reversed__(self):
        return reversed(list(self.__iter__()))
    def __len__(self):
        return len(self._mapping)
    # Abstract methods that must be overloaded by the concrete view classes
    # below.
    def _from_kv(self, kv):
        raise NotImplementedError()
    def __contains__(self, k):
        raise NotImplementedError()
class pdict_keys(KeysView, PDictView):
    __slots__ = ()
    def _from_kv(self, kv):
        return kv[0]
    def __contains__(self, k):
        return (k in self._mapping)
    def __reversed__(self):
        return reversed(list(self.__iter__()))
    def __repr__(self):
        s = ", ".join(map(repr, iter(self)))
        return f"pdict_keys({{{s}}})"
    def __str__(self):
        s = ", ".join(map(repr, iter(self)))
        return f"pdict_keys({{{s}}})"
class pdict_items(ItemsView, PDictView):
    __slots__ = ()
    def _from_kv(self, kv):
        return kv
    def __contains__(self, kv):
        if not isinstance(kv, tuple) or len(kv) != 2:
            return False
        (k0,v0) = self._from_kv(kv)
        d = Ellipsis if v0 is None else None
        v = self._mapping.get(k0, d)
        (k,v) = self._from_kv((k0,v))
        return v0 == v
class pdict_values(ValuesView, PDictView):
    __slots__ = ()
    def _from_kv(self, kv):
        return kv[1]
    def __contains__(self, v):
        for (_ii, (kv, _next)) in self._mapping._els:
            if is_tombstone(kv):
                continue
            v0 = self._from_kv(kv)
            if v0 == v:
                return True
        return False
class pdict(PersistentMapping):
    """A persistent dict type similar to `dict`.

    `pdict()` returns the empty `pdict`.

    `pdict(iterable)` returns a `pdict` containing the elements in `iterable`,
    which must be `(key, value)` tuples.

    `pdict(mapping)` returns a `pdict` containing the key-value pairs in the
    given mapping.
    """
    empty = None
    @classmethod
    def _new(cls, els, idx, top, count, ndeleted):
        new_pdict = super(pdict, cls).__new__(cls)
        object.__setattr__(new_pdict, '_els', els)
        object.__setattr__(new_pdict, '_idx', idx)
        object.__setattr__(new_pdict, '_top', top)
        object.__setattr__(new_pdict, '_count', count)
        object.__setattr__(new_pdict, '_ndeleted', ndeleted)
        object.__setattr__(new_pdict, '_hashcode', None)
        return new_pdict
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_hashcode")
    def __new__(cls, *args, **kw):
        n = len(args)
        if n == 1:
            arg = args[0]
        elif n == 0:
            if len(kw) == 0:
                return cls.empty
            else:
                arg = kw
                kw = {}
        else:
            msg = f"{cls.__name__} expects at most 1 argument, got {n}"
            raise TypeError(msg)
        # If any keyword options were given, we route through tdict.
        if not kw:
            # If arg is a tdict and no keyword arguments have been given, this
            # is a special case.
            if isinstance(arg, Sized) and len(arg) == 0:
                return cls.empty
            elif isinstance(arg, tdict):
                return cls._new(
                    arg._els.persistent(),
                    arg._idx.persistent(),
                    arg._top,
                    arg._count,
                    arg._ndeleted)
            elif type(arg) is cls:
                # Also, if it's already the right type, we can just return it
                # as-is.
                return arg
            elif type(arg) is pdict:
                return cls._new(arg._els, arg._idx, arg._top, arg._count,
                                arg._ndeleted)
        # For anything else, however, we just route this through tdict.
        t = tdict(arg, **kw)
        return cls._new(
            t._els.persistent(),
            t._idx.persistent(),
            t._top,
            t._count,
            t._ndeleted)
    def __hash__(self):
        if self._hashcode is None:
            # NB: we deliberately do NOT delegate to
            # PersistentMapping.__hash__ here: that generic default reads
            # self._els directly (`hash(frozenset(map(lambda u: u[1][0],
            # self._els))) + 2`), which would include tombstoned slots (see
            # pcollections/_compact.py) as bogus, bare TOMBSTONE "pairs".
            # Going through self.items() instead (which already skips
            # tombstones -- see pdict_items/PDictView above) keeps the
            # exact same formula/offset while staying tombstone-safe. This
            # mirrors why the C implementation (pcollections/_c/dict.c.h)
            # implements __hash__ natively rather than relying on this same
            # generic default -- see that file's own comment.
            h = hash(frozenset(self.items())) + 2
            object.__setattr__(self, '_hashcode', h)
        return self._hashcode
    def __len__(self):
        return self._count
    def __contains__(self, k):
        h = hash(k)
        ii = self._idx.get(h, None)
        while ii is not None:
            ((kk,vv),ii) = self._els[ii]
            if k == kk:
                return True
        return False
    def __iter__(self):
        return (
            kv[0]
            for (_ii, (kv, _next)) in self._els
            if not is_tombstone(kv)
        )
    def __reversed__(self):
        return reversed(self.keys())
    def __getitem__(self, key):
        h = hash(key)
        ii = self._idx.get(h, None)
        while ii is not None:
            (kv,ii) = self._els[ii]
            if key == kv[0]:
                return kv[1]
        raise KeyError(key)
    def get(self, key, default=None):
        h = hash(key)
        ii = self._idx.get(h, None)
        while ii is not None:
            (kv,ii) = self._els[ii]
            if key == kv[0]:
                return kv[1]
        return default
    def transient(self):
        """Returns a transient copy of the dict in constant time."""
        return tdict._new(TFAT(self._els), TAMT(self._idx), self._top,
                          self._count, self._ndeleted, self)
    def set(self, key, val):
        """Returns a copy of the pdict that maps the given key to the given
        value."""
        # Get the hash and initial index (if there is one).
        h = hash(key)
        ii = self._idx.get(h, None)
        if ii is None:
            # The object's hash is not here yet, so we can append to els and
            # insert it into idx.
            new_els = self._els.assoc(self._top, ((key, val), None))
            new_idx = self._idx.assoc(h, self._top)
        else:
            # First make sure it's not already in the dict, tracking the
            # previous link (index and value) as we walk the chain so that,
            # if we reach the end without finding `key`, we know exactly
            # which slot's `next` pointer to patch in order to append.
            # (An earlier version of this loop never advanced `ii` here --
            # a copy-paste omission that both infinite-looped on any hash
            # collision between unequal keys, and, even fixed to terminate,
            # would have patched the wrong slot's `next` pointer, since it
            # never tracked the previous link either. Confirmed against
            # tdict.__setitem__/pset.add() below, both of which already get
            # this right.)
            ii_prev = None
            kv_prev = None
            while ii is not None:
                (kv,ii_next) = self._els[ii]
                (k,v) = kv
                if key == k:
                    # It is in the dict; either it's exactly in the dict or we
                    # replace it.
                    if val is v:
                        return self
                    else:
                        new_els = self._els.assoc(ii, ((key,val), ii_next))
                        return self._new(new_els, self._idx, self._top,
                                         self._count, self._ndeleted)
                ii_prev = ii
                kv_prev = kv
                ii = ii_next
            # If we reach this point, we can add the object to the end of the
            # list.
            new_els = self._els.assoc(self._top, ((key,val), None))
            new_els = new_els.assoc(ii_prev, (kv_prev, self._top))
            new_idx = self._idx
        return self._new(new_els, new_idx, self._top + 1, self._count + 1,
                         self._ndeleted)
    def drop(self, key):
        """Returns a copy of the pdict that does not include the given key.

        If the key is not in the dict, returns the dict unchanged.
        """
        # Get the hash and initial index (if there is one).
        h = hash(key)
        ii = self._idx.get(h, None)
        # First make sure it's not already in the set.
        ii_prev = None
        kv_prev = None
        while ii is not None:
            (kv,ii_next) = self._els[ii]
            (k,v) = kv
            if key == k:
                # We remove this entry! Unlink it from its collision chain
                # (exactly as a real removal would) but overwrite its els
                # slot with a tombstone rather than actually removing it --
                # see pcollections/_compact.py for why.
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
                    new_els = self._els.assoc(ii_prev, (kv_prev, ii_next))
                new_els = new_els.assoc(ii, (TOMBSTONE, None))
                return self._maybe_compacted(
                    new_els, new_idx, self._top,
                    self._count - 1, self._ndeleted + 1)
            ii_prev = ii
            kv_prev = kv
            ii = ii_next
        # If we reach this point, then obj isn't in the set, so we just return
        # self unchanged.
        return self
    def clear(self):
        """Returns the empty pdict."""
        return type(self).empty
    @classmethod
    def _maybe_compacted(cls, els, idx, top, count, ndeleted):
        """Builds a new pdict from the given (already-tombstoned) els/idx,
        compacting first if `ndeleted` has crossed should_compact()'s
        threshold. Shared by drop()/pop()."""
        if should_compact(count, ndeleted):
            els, idx, top = _compact_els(els)
            ndeleted = 0
        return cls._new(els, idx, top, count, ndeleted)
    # We include reimplements for some of these because we can improve them in
    # some non-trivial way.
    def pop(self, key, *args):
        """Returns a tuple of the value mapped to the given key and a copy of
        the pdict with that key removed.

        If the key is not found, the second argument is returned, if given,
        otherwise, a `KeyError` is raised.
        """
        nargs = len(args)
        if nargs > 1:
            raise TypeError(f"pop expected at most 2 arguments, got {nargs}")
        h = hash(key)
        ii = self._idx.get(h, None)
        ii_prev = None
        kv_prev = None
        while ii is not None:
            (kv,ii_next) = self._els[ii]
            (k,v) = kv
            if key == k:
                # We remove this entry! (See drop()'s comment on tombstoning.)
                if ii_prev is None:
                    if ii_next is None:
                        new_idx = self._idx.dissoc(h)
                    else:
                        new_idx = self._idx.assoc(h, ii_next)
                    new_els = self._els
                else:
                    new_idx = self._idx
                    new_els = self._els.assoc(ii_prev, (kv_prev, ii_next))
                new_els = new_els.assoc(ii, (TOMBSTONE, None))
                new_pdict = self._maybe_compacted(
                    new_els, new_idx, self._top,
                    self._count - 1, self._ndeleted + 1)
                return (v, new_pdict)
            ii_prev = ii
            kv_prev = kv
            ii = ii_next
        # It's not here!
        if nargs == 0:
            raise KeyError(key)
        else:
            return (args[0], self)
    def keys(self):
        return pdict_keys(self)
    def items(self):
        return pdict_items(self)
    def values(self):
        return pdict_values(self)
# Make the empty pdict.
pdict.empty = pdict._new(FAT.empty, AMT.empty, 0, 0, 0)


#===============================================================================
# tdict
# The transient set type.

class tdict_view(Set):
    def _iter(self, arg):
        if self._tdict._version > self._version:
            raise RuntimeError(f"{type(self)} changed during iteration")
        else:
            return self._from_kv(arg[1])
    def __new__(cls, d):
        if not isinstance(d, tdict):
            raise ValueError("can only make tdict_keys object from tdict")
        sup = super(tdict_view,cls)
        obj = sup.__new__(cls)
        sup.__setattr__(obj, '_tdict', d)
        sup.__setattr__(obj, '_version', d._version)
        return obj
    def __iter__(self):
        v0 = self._version
        def _gen():
            for (_ii, (kv, _next)) in self._tdict._els:
                if self._tdict._version > v0:
                    raise RuntimeError(f"{type(self)} changed during iteration")
                if is_tombstone(kv):
                    continue
                yield self._from_kv(kv)
        return _gen()
    def __reversed__(self):
        return reversed(list(self.__iter__()))
    def __len__(self):
        return len(self._tdict)
    # These are the abstract methods that need to be implemented in the actual
    # view classes below.
    def _from_kv(kv):
        raise NotImplementedError()
    def __contains__(self, arg):
        raise NotImplementedError()
class tdict_keys(KeysView, tdict_view):
    __slots__ = ('_tdict', '_version')
    def _from_kv(self, arg):
        return arg[0]
    def __contains__(self, k):
        return (k in self._tdict)
class tdict_items(ItemsView, tdict_view):
    __slots__ = ('_tdict', '_version')
    def _from_kv(self, arg):
        return arg
    def __contains__(self, kv):
        if not isinstance(kv, tuple) or len(kv) != 2:
            return False
        d = Ellipsis if kv[1] is None else None
        return self._tdict.get(kv[0], d) == kv[1]
class tdict_values(ValuesView, tdict_view):
    __slots__ = ('_tdict', '_version')
    def _from_kv(self, arg):
        return arg[1]
    def __contains__(self, v):
        for (_ii, (kv, _next)) in self._tdict._els:
            if is_tombstone(kv):
                continue
            if kv[1] == v:
                return True
        return False
class tdict(TransientMapping):
    """A transient dict type similar to `dict` for mutating persistent dicts.

    `tdict()` returns the empty `tdict`.

    `tdict(iterable)` returns a `tdict` containing the key-value pairs in
    `iterable`.

    `tdict(p)` efficiently returns a transient copy of the persistent dict `p`.

    The interfaces for `dict` and `tdict` are nearly identical, but unlike
    `dict`, a `tdict` can be created from a `pdict` in `O(1)` time, and a
    `pdict` can be created from a `tdict` in `O(log n)` time (and note that in
    practice this `O(log n)` has a very low constant). The `tdict` type is
    explicitly intended to make batch mutations to `pdict` objects more
    efficient by reducing the number of allocations required.
    """
    @classmethod
    def _new(cls, els, idx, top, count, ndeleted, orig=None):
        new_tdict = super(tdict, cls).__new__(cls)
        object.__setattr__(new_tdict, '_els', els)
        object.__setattr__(new_tdict, '_idx', idx)
        object.__setattr__(new_tdict, '_top', top)
        object.__setattr__(new_tdict, '_count', count)
        object.__setattr__(new_tdict, '_ndeleted', ndeleted)
        object.__setattr__(new_tdict, '_version', 0)
        object.__setattr__(new_tdict, '_orig', orig)
        return new_tdict
    @classmethod
    def empty(cls):
        """Returns an empty tdict."""
        return cls._new(TFAT(FAT.empty), TAMT(AMT.empty), 0, 0, 0)
    __slots__ = ("_els", "_idx", "_top", "_count", "_ndeleted", "_version",
                "_orig")
    def __new__(cls, *args, **kw):
        n = len(args)
        if n == 1:
            arg = args[0]
        elif n == 0:
            if len(kw) == 0:
                return cls.empty()
            else:
                arg = kw
                kw = None
        else:
            raise TypeError(f"pdict expects at most 1 argument, got {n}")
        # If arg is a tdict or pdict, this is a special case.
        if type(arg) is tdict:
            obj = cls._new(TFAT(arg._els.persistent()),
                           TAMT(arg._idx.persistent()),
                           arg._top,
                           arg._count,
                           arg._ndeleted,
                           arg._orig)
        elif type(arg) is pdict:
            obj = cls._new(TFAT(arg._els), TAMT(arg._idx), arg._top,
                           arg._count, arg._ndeleted, arg)
        else:
            obj = cls._new(TFAT(FAT.empty), TAMT(AMT.empty), 0, 0, 0)
            if isinstance(arg, Mapping):
                arg = arg.items()
            for (k,v) in arg:
                obj[k] = v
        # If keyword args were provided, merge them in.
        if kw:
            for (k,v) in kw.items():
                obj[k] = v
        return obj
    def __setattr__(self, k, v):
        raise TypeError("tdict attributes are immutable")
    def __setitem__(self, k, v):
        # Get the hash and initial index (if there is one).
        h = hash(k)
        ii = self._idx.get(h, None)
        # First make sure it's not already in the dict.
        ii_prev = None
        while ii is not None:
            (kv, ii_next) = self._els[ii]
            if kv[0] == k:
                if kv[1] is not v:
                    self._els[ii] = ((k,v), ii_next)
                    # The object has changed, so make sure we aren't tracking
                    # the original object still.
                    object.__setattr__(self, '_orig', None)
                # Note that this does not mandate a version update because it is
                # not changing the keys.
                return None
            ii_prev = ii
            kv_prev = kv
            ii = ii_next
        if ii_prev is None:
            # The object's hash is not here yet, so we can append to els and
            # insert it into idx.
            self._els[self._top] = ((k,v), None)
            self._idx[h] = self._top
        else:
            # If we reach this point, we can add the object to the end of the
            # list.
            self._els[self._top] = ((k,v), None)
            self._els[ii_prev] = (kv_prev, self._top)
        object.__setattr__(self, '_top', self._top + 1)
        object.__setattr__(self, '_count', self._count + 1)
        object.__setattr__(self, '_version', self._version + 1)
        object.__setattr__(self, '_orig', None)
    def __len__(self):
        return self._count
    def __contains__(self, k):
        h = hash(k)
        ii = self._idx.get(h, None)
        while ii is not None:
            ((kk,vv),ii) = self._els[ii]
            if k == kk:
                return True
        return False
    def __reversed__(self):
        return reversed(self.keys())
    def __getitem__(self, key):
        h = hash(key)
        ii = self._idx.get(h, None)
        while ii is not None:
            (kv,ii) = self._els[ii]
            if key == kv[0]:
                return kv[1]
        raise KeyError(key)
    def get(self, key, default=None):
        h = hash(key)
        ii = self._idx.get(h, None)
        while ii is not None:
            (kv,ii) = self._els[ii]
            if key == kv[0]:
                return kv[1]
        return default
    def __iter__(self):
        v0 = self._version
        def _gen():
            for (_ii, (kv, _next)) in self._els:
                if v0 < self._version:
                    raise RuntimeError(f"{type(self)} changed during iteration")
                if is_tombstone(kv):
                    continue
                yield kv[0]
        return _gen()
    def __delitem__(self, key):
        # Get the hash and initial index (if there is one).
        h = hash(key)
        ii = self._idx.get(h, None)
        # First make sure it's not already in the set.
        ii_prev = None
        kv_prev = None
        while ii is not None:
            (kv,ii_next) = self._els[ii]
            (k,v) = kv
            if key == k:
                # We remove this entry! (See pdict.drop()'s comment on
                # tombstoning.)
                if ii_prev is None:
                    # We're removing from the front of the list.
                    if ii_next is None:
                        del self._idx[h]
                    else:
                        self._idx[h] = ii_next
                else:
                    # We're removing from the end or the middle.
                    self._els[ii_prev] = (kv_prev, ii_next)
                self._els[ii] = (TOMBSTONE, None)
                object.__setattr__(self, '_count', self._count - 1)
                object.__setattr__(self, '_ndeleted', self._ndeleted + 1)
                object.__setattr__(self, '_version', self._version + 1)
                object.__setattr__(self, '_orig', None)
                self._maybe_compact()
                return None
            ii_prev = ii
            kv_prev = kv
            ii = ii_next
        # If we reach this point, then obj isn't in the set, so we just return
        # self unchanged.
        raise KeyError(key)
    def clear(self):
        """Returns the empty pdict."""
        object.__setattr__(self, '_els', TFAT(FAT.empty))
        object.__setattr__(self, '_idx', TAMT(AMT.empty))
        object.__setattr__(self, '_top', 0)
        object.__setattr__(self, '_count', 0)
        object.__setattr__(self, '_ndeleted', 0)
        object.__setattr__(self, '_version', self._version + 1)
        object.__setattr__(self, '_orig', None)
        return None
    def _maybe_compact(self):
        if should_compact(self._count, self._ndeleted):
            new_els, new_idx, new_top = _compact_els(self._els)
            object.__setattr__(self, '_els', TFAT(new_els))
            object.__setattr__(self, '_idx', TAMT(new_idx))
            object.__setattr__(self, '_top', new_top)
            object.__setattr__(self, '_ndeleted', 0)
    def pop(self, key, *args):
        """Removes the given key from the tdict and returns the previously
        associated value.

        If the key is not found, the second argument is returned, if given,
        otherwise, a `KeyError` is raised.
        """
        nargs = len(args)
        if nargs > 1:
            raise TypeError(f"pop expected at most 2 arguments, got {nargs}")
        h = hash(key)
        ii = self._idx.get(h, None)
        ii_prev = None
        kv_prev = None
        while ii is not None:
            (kv,ii_next) = self._els[ii]
            (k,v) = kv
            if key == k:
                # We remove this entry! (See pdict.drop()'s comment on
                # tombstoning.)
                if ii_prev is None:
                    if ii_next is None:
                        del self._idx[h]
                    else:
                        self._idx[h] = ii_next
                else:
                    self._els[ii_prev] = (kv_prev, ii_next)
                self._els[ii] = (TOMBSTONE, None)
                object.__setattr__(self, '_count', self._count - 1)
                object.__setattr__(self, '_ndeleted', self._ndeleted + 1)
                object.__setattr__(self, '_version', self._version + 1)
                object.__setattr__(self, '_orig', None)
                self._maybe_compact()
                return v
            ii_prev = ii
            kv_prev = kv
            ii = ii_next
        # It's not here!
        if nargs == 0:
            raise KeyError(key)
        else:
            return args[0]
    def persistent(self):
        """Efficiently returns a persistent (pdict) copy of the tdict."""
        if len(self) == 0:
            return pdict.empty
        elif self._orig is not None:
            return self._orig
        else:
            return pdict._new(self._els.persistent(),
                              self._idx.persistent(),
                              self._top,
                              self._count,
                              self._ndeleted)
    def keys(self):
        return tdict_keys(self)
    def items(self):
        return tdict_items(self)
    def values(self):
        return tdict_values(self)


#===============================================================================
# Compaction.
# Rebuilds els/idx from scratch, live entries only, renumbered 0..count-1 in
# original (insertion) order -- mirrors dict_rebuild_compacted()/_c/dict.c.h.
# Implemented by simply replaying every live (key, val) pair, in original
# order, through tdict.__setitem__'s already-correct chain-building logic,
# rather than duplicating that logic a second time here.

def _compact_els(els):
    t = tdict.empty()
    for (_ii, (kv, _next)) in els:
        if is_tombstone(kv):
            continue
        (k, v) = kv
        t[k] = v
    return (t._els.persistent(), t._idx.persistent(), t._top)
