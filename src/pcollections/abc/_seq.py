# -*- coding: utf-8 -*-
###############################################################################
# pcollections/abc/_seq.py
# The definitions of the abstract base classes for the pcollections sequence
# types.
# By Noah C. Benson

import operator
from collections.abc import (Sequence, MutableSequence)

from ._core import (_PersistentBase, Persistent, Transient)
from ..util import (seqstr, seqeq, seqorder)


def _seq_types():
    return (list, PersistentSequence, TransientSequence)

def _held(seq):
    """The sequence with any lazy elements uncomputed (see `holdlazy`)."""
    method = getattr(type(seq), '__holdlazy__', None)
    return seq if method is None else method(seq)

def _index(index):
    try:
        return operator.index(index)
    except TypeError:
        raise TypeError(f"sequence indices must be integers or slices, "
                        f"not {type(index).__name__}") from None

def _seq_eq(seq, other):
    if other is seq:
        return True
    if not isinstance(other, _seq_types()):
        return NotImplemented
    return seqeq(seq, other)

def _seq_order(seq, other, op):
    if not isinstance(other, _seq_types()):
        return NotImplemented
    return seqorder(seq, other, op)

def _seq_count(seq, value):
    n = 0
    for el in seq:
        if el is value or el == value:
            n += 1
    return n

def _seq_index(seq, value, start, stop):
    n = len(seq)
    (start, stop, _) = slice(start, stop).indices(n)
    for (ii, el) in enumerate(seq):
        if ii >= stop:
            break
        if ii >= start and (el is value or el == value):
            return ii
    raise ValueError(f"{value!r} is not in {type(seq).__name__}")


#==============================================================================
# _PersistentSequenceBase

class _PersistentSequenceBase(_PersistentBase):
    """Plain (non-``ABCMeta``) mixin holding ``PersistentSequence``'s concrete
    method bodies, so that ``pcollections._c._core.plist`` can inherit them
    without inheriting ``ABCMeta`` anywhere in its base chain -- see
    ``_PersistentBase``'s docstring (``abc/_core.py``) for the full CPython
    3.14 rationale.

    Like ``PersistentSet``, ``PersistentSequence`` never actually relied on
    any concrete method from ``collections.abc.Sequence`` -- every method
    below was already implemented directly on ``PersistentSequence`` itself,
    so this is a pure relocation of that existing code (list.c.h's plist/tlist
    additionally implement their own native ``__eq__``/``__lt__``/etc. via a
    real ``Py_tp_richcompare`` slot, so the comparison methods here are never
    actually reached for the C backend -- but are kept, faithfully, for the
    pure-Python backend and any other subclass that doesn't override them).
    """
    __slots__ = ()
    # Methods which must be implemented in the children.
    def set(self, index, obj):
        """Returns a copy of the persistent sequence with the given index set to
        the given object.
        """
        raise NotImplementedError()
    def delete(self, index=-1):
        """"Returns a copy of the persistent sequence with the item at index
        removed (default index: last)."""
        raise NotImplementedError()
    def append(self, obj):
        """Returns a copy of the persistent sequence with object appended."""
        raise NotImplementedError()
    def prepend(self, obj):
        """Returns a copy of the persistent sequence with object prepended."""
        raise NotImplementedError()
    def insert(self, index, obj):
        "Returns a new persistent sequence with object inserted before index."
        raise NotImplementedError()
    def clear(self):
        """Returns an empty persistent sequence of the same type."""
        raise NotImplementedError()
    # Methods with default implementations that may not or may not have very
    # good performance in specific instance classes.
    def drop(self, index=-1, error=False):
        """Returns a copy of the persistent sequence with the element at the
        given index removed.

        If the index is out of range, `drop` returns the sequence itself, or,
        if `error` is true, raises `IndexError`."""
        index = _index(index)
        n = len(self)
        if index < -n or index >= n:
            if error:
                raise IndexError(f"{type(self).__name__} index out of range")
            return self
        return self.delete(index)
    def pop(self, index=-1):
        """Returns a tuple of the value at the given index and a copy of the
        persistent sequence with the item at that index removed (default index:
        last)."""
        el = self[index]
        return (el, self.delete(index))
    def remove(self, value):
        """Returns a copy of the sequence with the first occurence of value
        removed.

        Raises ValueError if the value is not present.
        """
        for (ii, el) in enumerate(self):
            if el is value or el == value:
                return self.delete(ii)
        name = type(self).__name__
        raise ValueError(f"{name}.remove(x): x not in {name}")
    def sort(self, key=None, reverse=False):
        """Returns a sorted copy of the given persistent sequence."""
        t = self.clear().transient()
        for k in sorted(self, key=key, reverse=reverse):
            t.append(k)
        return type(self)(t)
    def reverse(self):
        """Returns a reversed copy of the persistent sequence."""
        t = self.clear().transient()
        for el in self.__reversed__():
            t.append(el)
        return type(self)(t)
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"[|{seqstr(self, maxlen=60)}|]"
    def __repr__(self):
        return f"[|{seqstr(self)}|]"
    def __eq__(self, other):
        return _seq_eq(self, other)
    def __ne__(self, other):
        r = _seq_eq(self, other)
        return r if r is NotImplemented else not r
    def __lt__(self, other):
        return _seq_order(self, other, operator.lt)
    def __le__(self, other):
        return _seq_order(self, other, operator.le)
    def __gt__(self, other):
        return _seq_order(self, other, operator.gt)
    def __ge__(self, other):
        return _seq_order(self, other, operator.ge)
    def __hash__(self):
        return hash(tuple(self)) + 1
    def __contains__(self, value):
        for el in self:
            if el is value or el == value:
                return True
        return False
    def __reversed__(self):
        n = len(self)
        return map(self.__getitem__, range(n - 1, -1, -1))
    def count(self, value):
        """Returns the number of occurences of value."""
        return _seq_count(self, value)
    def extend(self, iterable):
        """Returns a copy of the persistent sequence with the iterable's
        elements appended."""
        t = self.transient()
        for el in iterable:
            t.append(el)
        return type(self)(t)
    def index(self, value, start=0, stop=None):
        """Returns the first index of value.

        Raises ValueError if value is not present.
        """
        return _seq_index(self, value, start, stop)
    def __add__(self, obj):
        if not isinstance(obj, _seq_types()):
            return NotImplemented
        if len(obj) == 0:
            return self
        if len(self) == 0 and type(obj) is type(self):
            return obj
        return self.extend(obj)
    def __radd__(self, obj):
        if not isinstance(obj, _seq_types()):
            return NotImplemented
        if len(obj) == 0:
            return self
        t = self.transient()
        for el in reversed(obj):
            t.prepend(el)
        return type(self)(t)
    def __mul__(self, value):
        try:
            reps = operator.index(value)
        except TypeError:
            return NotImplemented
        if reps <= 0:
            return self.clear()
        elif reps == 1:
            return self
        t = self.transient()
        items = list(_held(self))
        for r in range(reps - 1):
            for el in items:
                t.append(el)
        return type(self)(t)
    def __rmul__(self, value):
        return self.__mul__(value)
    def __reduce__(self):
        # Pickle by class: the class pickles by its qualified name, so a
        # pickle made with one backend loads with the other.
        return (type(self), (list(self),))
    def __json__(self):
        from json import dumps
        return dumps(list(self))


#==============================================================================
# PersistentSequence

class PersistentSequence(_PersistentSequenceBase, Persistent, Sequence):
    """All the operations on a persistent sequence.

    ``PersistentSequence`` objects are sequences (i.e., objects that inherit
    from ``collections.abc.Sequence``), but they differ from other sequences in
    that they support efficient updating by means of efficiently producing
    copies of themselves that incorporate requested changes.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `PersistentSequence`, that class is noted in
    parentheses.
     * ``set(index, object)``
     * ``delete(index=-1)``
     * ``append(object)``
     * ``prepend(object)``
     * ``insert(index, object)``
     * ``clear()``
     * ``transient()`` (``Persistent``)
     * ``__iter__`` (``Iterable``)
     * ``__len__`` (``Sized``)
     * ``__getitem__`` (``Sequence``)
     * ``__reduce__`` (for pickling)

    Additionally, ``PersistentSequence`` includes default implementations of
    the following methods, which may or may not be optimal for any particular
    base-class.
     * ``__str__`` (``object`)
     * ``__repr__`` (``object`)
     * ``__eq__`` (``object`)
     * ``__hash__`` (``Hashable`)
     * ``__contains__`` (``Container`)
     * ``__reversed__`` (``Reversible`)
     * ``count`` (``Sequence`)
     * ``extend(iterable)`` (``Sequence`)
     * ``index(value)`` (``Sequence`)
     * ``copy()`` (``Persistent`)
     * ``__add__``
     * ``__radd__``
     * ``__mul__``
     * ___rmul__``
     * ``drop(index=-1)``
     * ``pop(index=-1)``
     * ``remove(value)``
     * ``sort()``
     * ``reverse()``
     * ``__json__`` (for the ``json_fix`` module)

    """
    # See Persistent.__slots__'s comment (abc/_core.py): keeps this mixin,
    # and anything that mixes it in, from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()


#===============================================================================
# _TransientSequenceBase

class _TransientSequenceBase(Transient):
    """Plain (non-``ABCMeta``) mixin holding ``TransientSequence``'s concrete
    method bodies, so that ``pcollections._c._core.tlist`` can inherit them
    without inheriting ``ABCMeta`` anywhere in its base chain -- see
    ``_PersistentBase``'s docstring (``abc/_core.py``) for the full CPython
    3.14 rationale. (``Transient`` itself was never ``ABCMeta``-based, so this
    can subclass it directly.)

    As with ``_PersistentSequenceBase``, this is a pure relocation of
    ``TransientSequence``'s already-self-contained methods -- nothing here is
    a new port from stdlib ``collections.abc.Sequence``/``MutableSequence``.
    """
    # See Persistent.__slots__'s comment (abc/_core.py): keeps this mixin,
    # and anything that mixes it in, from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()
    def pop(self, index=-1):
        """Remove and return item at index (default last).

        Raises IndexError if list is empty or index is out of range."""
        index = _index(index)
        n = len(self)
        if n == 0:
            raise IndexError(f"pop from empty {type(self).__name__}")
        if index < -n or index >= n:
            raise IndexError("pop index out of range")
        el = self[index]
        del self[index]
        return el
    def remove(self, value):
        """Removes the first occurence of value.

        Raises ValueError if the value is not present.
        """
        for (ii, el) in enumerate(self):
            if el is value or el == value:
                del self[ii]
                return
        name = type(self).__name__
        raise ValueError(f"{name}.remove(x): x not in {name}")
    def sort(self, key=None, reverse=False):
        """Sort the tlist in ascending order and return None.

        The sort is in-place (i.e. the tlist itself is modified) and stable
        (i.e. the order of two equal elements is maintained).

        If a key function is given, apply it once to each list item and sort
        them, ascending or descending, according to their function values.

        The reverse flag can be set to sort in descending order.
        """
        for (ii,el) in enumerate(sorted(self, key=key, reverse=reverse)):
            self[ii] = el
    def count(self, value):
        """Returns the number of occurences of value."""
        return _seq_count(self, value)
    def extend(self, iterable):
        """Extends tlist by appending elements from the iterable."""
        if iterable is self:
            iterable = list(_held(self))
        for val in iterable:
            self.append(val)
    def index(self, value, start=0, stop=None):
        """Returns the first index of value.

        Raises ValueError if value is not present.
        """
        return _seq_index(self, value, start, stop)
    def reverse(self):
        """Reverses *IN PLACE*."""
        n = len(self)
        for k in range(n//2):
            kk = n - 1 - k
            tmp = self[k]
            self[k] = self[kk]
            self[kk] = tmp
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"[<{seqstr(self, maxlen=60)}>]"
    def __repr__(self):
        return f"[<{seqstr(self)}>]"
    # Transient sequences are mutable, so they are not hashable.
    __hash__ = None
    def __eq__(self, other):
        return _seq_eq(self, other)
    def __ne__(self, other):
        r = _seq_eq(self, other)
        return r if r is NotImplemented else not r
    def __lt__(self, other):
        return _seq_order(self, other, operator.lt)
    def __le__(self, other):
        return _seq_order(self, other, operator.le)
    def __gt__(self, other):
        return _seq_order(self, other, operator.gt)
    def __ge__(self, other):
        return _seq_order(self, other, operator.ge)
    def __contains__(self, value):
        for el in self:
            if el is value or el == value:
                return True
        return False
    def __reversed__(self):
        n = len(self)
        return map(self.__getitem__, range(n - 1, -1, -1))
    def __iadd__(self, obj):
        # Like list, += accepts any iterable.
        try:
            obj = list(obj)
        except TypeError:
            return NotImplemented
        self.extend(obj)
        return self
    def __add__(self, obj):
        if not isinstance(obj, _seq_types()):
            return NotImplemented
        t = self.copy()
        t.extend(obj)
        return t
    def __radd__(self, obj):
        if not isinstance(obj, _seq_types()):
            return NotImplemented
        t = self.copy()
        for el in reversed(obj):
            t.prepend(el)
        return t
    def __imul__(self, value):
        try:
            reps = operator.index(value)
        except TypeError:
            return NotImplemented
        if reps <= 0:
            self.clear()
        elif reps > 1:
            # Take the elements first: the loop changes the sequence.
            items = list(_held(self))
            for r in range(reps - 1):
                for el in items:
                    self.append(el)
        return self
    def __mul__(self, value):
        try:
            operator.index(value)
        except TypeError:
            return NotImplemented
        t = self.copy()
        t *= value
        return t
    def __rmul__(self, value):
        return self.__mul__(value)
    def copy(self):
        """Returns a copy of the given transient sequence."""
        return self.persistent().transient()
    # For pickling.
    def __reduce__(self):
        # Pickle by class: the class pickles by its qualified name, so a
        # pickle made with one backend loads with the other.
        return (type(self), (list(self),))
    def __json__(self):
        from json import dumps
        return dumps(list(self))


#===============================================================================
# TransientSequence

class TransientSequence(_TransientSequenceBase, Transient, MutableSequence):
    """All the operations on a transient sequence.

    `TransientSequence` objects are mutable sequences (i.e., objects that
    inherit from ``collections.abc.MutableSequence``), but they differ from
    other sequences in that they support efficient conversion to and from a
    paired persistent datatype.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of ``TransientSequence`, that class is noted in
    parentheses.
     * ``clear()`
     * ``persistent()`` (``Transient``)
     * ``__iter__`` (``Iterable``)
     * ``__len__`` (``Sized``)
     * ``__getitem__`` (``Sequence``)
     * ``__setitem__(index, object)`` (``MutableSequence``)
     * ``__delitem__(index)`` (``MutableSequence``)
     * ``append(object)`` (``MutableSequence``)
     * ``prepend(object)`` (``MutableSequence``)
     * ``insert(index, object)`` (``MutableSequence``)

    Additionally, ``TransientSequence`` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * ``pop(index=-1)``
     * ``remove(value)``
     * ``sort()``
     * ``reverse()``
     * ``__str__`` (``object``)
     * ``__repr__`` (``object``)
     * ``__eq__`` (``object``)
     * ``__contains__`` (``Container``)
     * ``__reversed__`` (``Reversible``)
     * ``count`` (``Sequence``)
     * ``extend(iterable)`` (``Sequence``)
     * ``index(value)`` (``Sequence``)
     * ``copy()`` (``Transient```)
     * ``__add__``
     * ``__radd__``
     * ``__iadd__``
     * ``__mul__``
     * ``__rmul__``
     * ``__imul__``
     * ``__reduce__`` (for pickling)
     * ``__json__`` (for the ``json_fix`` module)
    """
    # See Persistent.__slots__'s comment (abc/_core.py): keeps this mixin,
    # and anything that mixes it in, from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()
