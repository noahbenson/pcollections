# -*- coding: utf-8 -*-
################################################################################
# pcollections/_guard.py
# Change detection for the pure-Python transient types.
# By Noah C. Benson

"""Change detection for the pure-Python transients.

Transients are for use by one thread at a time. When user code that runs
during an operation (a key's ``__hash__`` or ``__eq__``) or an interleaved
loop changes a transient, the operation raises ``RuntimeError`` rather than
return wrong results, matching the C backend (see ``_c/core.h``).

Each transient has a ``_busy`` flag, set while it is being modified; a
modification that finds it set raises ``RuntimeError``. Each transient also
has a ``_version`` counter, which a change makes odd before it alters the
collection and even again once it is done, so that a reader (in another
thread, or in a ``__del__`` method run by the change) can tell that the
collection changed, or is changing, under it. ``tdict`` and ``tset`` also
keep ``_kversion``, which changes whenever the set of keys changes.
"""

from threading import Lock


def new_busy_flag():
    """Returns a new busy flag for a transient."""
    return Lock()


def begin_update(obj, modifying=True):
    """Marks the start of a modification of transient `obj`; raises
    `RuntimeError` if one is already in progress (reentrantly, or from
    another thread). Operations that must not see a half-finished
    modification, such as `persistent()`, also use this, with `modifying`
    false."""
    # A non-blocking acquire is an atomic test-and-set.
    if not obj._busy.acquire(False):
        if modifying:
            raise RuntimeError(
                f"{type(obj).__name__} was modified during another"
                f" modification of it (transients are not safe for"
                f" concurrent or reentrant modification)")
        raise RuntimeError(
            f"{type(obj).__name__} was used during a modification of it")


def end_update(obj):
    """Marks the end of a modification started with `begin_update`."""
    if obj._version & 1:
        object.__setattr__(obj, '_version', obj._version + 1)
    obj._busy.release()


def check_readable(obj, what="a lookup"):
    """Raises `RuntimeError` if `obj` is part way through a change (see the
    module docstring)."""
    if obj._version & 1:
        raise RuntimeError(f"{type(obj).__name__} changed during {what}")


def updating(method):
    """Decorates a method that modifies its transient, bracketing it with
    `begin_update` and `end_update`."""
    def wrapper(self, *args, **kw):
        begin_update(self)
        try:
            return method(self, *args, **kw)
        finally:
            end_update(self)
    wrapper.__name__ = method.__name__
    wrapper.__qualname__ = method.__qualname__
    wrapper.__doc__ = method.__doc__
    return wrapper


def changed_during_lookup(obj):
    """Raises the error for a lookup whose transient changed under it."""
    raise RuntimeError(f"{type(obj).__name__} changed during a lookup")


class TransientIter:
    """An iterator over the live entries of a ``tdict`` or ``tset``.

    Like the builtin dict and set iterators, it raises ``RuntimeError`` if
    the transient's size or keys change during iteration. If only values
    change, it finds its place again and continues.

    ``extract(payload)`` converts the payload of an ``_els`` entry (the
    ``(key, value)`` pair or the set element) into the value to yield.
    """
    __slots__ = ('_owner', '_extract', '_count', '_kversion', '_version',
                 '_it', '_nextkey', '_state')

    def __init__(self, owner, extract):
        self._owner = owner
        self._extract = extract
        self._count = owner._count
        self._kversion = owner._kversion
        self._version = None
        self._it = None
        self._nextkey = 0
        self._state = 0  # 0: active; 1: exhausted; 2: failed.

    def __iter__(self):
        return self

    def __next__(self):
        from ._compact import is_tombstone
        o = self._owner
        name = type(o).__name__
        while True:
            if self._state == 1:
                raise StopIteration
            if self._state == 0 and o._count != self._count:
                self._state = 2
                raise RuntimeError(f"{name} changed size during iteration")
            if self._state == 2 or o._kversion != self._kversion:
                self._state = 2
                raise RuntimeError(f"{name} keys changed during iteration")
            check_readable(o, "iteration")
            try:
                if self._it is None or o._version != self._version:
                    self._version = o._version
                    self._it = o._els.iter_from(self._nextkey)
                for (ii, (payload, _next)) in self._it:
                    if o._version != self._version:
                        break
                    if is_tombstone(payload):
                        continue
                    self._nextkey = ii + 1
                    return self._extract(payload)
                else:
                    self._state = 1
                    self._it = None
                    raise StopIteration
            except (LookupError, TypeError, ValueError):
                # Another thread changed the trie as we read it.
                if o._version == self._version:
                    raise
            # The transient changed during this step; find our place again.
            self._it = None


class TListIter:
    """An iterator over a ``tlist`` that behaves like a list iterator: it
    yields the element at its current index, whatever the list holds there
    now, and stops for good once the index reaches the end."""
    __slots__ = ('_owner', '_index', '_version', '_it')

    def __init__(self, owner):
        self._owner = owner
        self._index = 0
        self._version = None
        self._it = None

    def __iter__(self):
        return self

    def __next__(self):
        o = self._owner
        if o is None:
            raise StopIteration
        i = self._index
        v = o._version
        if i >= len(o):
            self._owner = None
            self._it = None
            raise StopIteration
        check_readable(o, "iteration")
        if i == 0 and self._version is None:
            # Walk the trie while the list is unchanged...
            self._it = o._fast_iter()
            self._version = v
        x = self._it
        if x is not None and v == self._version:
            try:
                x = next(x)
            except (LookupError, TypeError, ValueError, StopIteration):
                # Another thread changed the list as we read it.
                if o._version == v:
                    raise
            if o._version != v:
                x = self._it = None
        else:
            x = self._it = None
        if self._it is None:
            # ...and look elements up by index once it has changed.
            x = o._raw_item(i, v)
        self._index = i + 1
        return x
