# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_set.py
# The definitions of the abstract base classes for the pcollections set types.
# By Noah C. Benson

from collections.abc import (Set, MutableSet)

from ._core import (_PersistentBase, Persistent, Transient)
from ..util import (setcmp, seqstr, frozenset_hash)


def _as_set(obj):
    """`obj` if it is a `Set`, and otherwise a `set` of its elements (the
    named set methods accept any iterable, as `set`'s do)."""
    return obj if isinstance(obj, Set) else set(obj)

def _isdisjoint(s, other):
    other = _as_set(other)
    (small, large) = (s, other) if len(s) <= len(other) else (other, s)
    return not any(el in large for el in small)

def _last_element(s):
    """The set's last element: `s._last()` for the pcollections types, and
    otherwise the last element that iterating over `s` yields."""
    last = getattr(s, '_last', None)
    if last is not None:
        return last()
    for el in s:
        pass
    return el


#===============================================================================
# _PersistentSetBase


class _PersistentSetBase(_PersistentBase):
    """Plain (non-``ABCMeta``) mixin holding ``PersistentSet``'s concrete
    method bodies, so that ``pcollections._c._core.pset`` can inherit them
    without ``ABCMeta`` in its bases; see ``_PersistentBase``'s docstring
    (``abc/_core.py``).

    Unlike the mapping mixins, this defines all of its methods itself
    (comparisons, set algebra, ``isdisjoint``, etc.) rather than copying any
    from ``collections.abc.Set``.
    """
    __slots__ = ()
    # Methods which must be implemented in the children.
    def add(self, obj):
        """Returns a copy of the persistent set that includes the given
        object.
        """
        raise NotImplementedError()
    def discard(self, obj):
        """Returns a copy of the persistent set that does not include the given
        object.

        If the element is not a member, `discard` returns the original pset.
        """
        raise NotImplementedError()
    def clear(self):
        """Returns the empty pset."""
        raise NotImplementedError()
    # Methods which are probably fine for all child classes.
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"{{|{seqstr(self, maxlen=60)}|}}"
    def __repr__(self):
        return f"{{|{seqstr(self)}|}}"
    def __eq__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == 0
    def __ne__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) != 0
    def __hash__(self):
        # Equal to the hash of an equal frozenset.
        return frozenset_hash(self)
    def __lt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == -1
    def __le__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) in (-1, 0)
    def __gt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == 1
    def __ge__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) in (1, 0)
    def drop(self, obj, error=False):
        """Returns a copy of the persistent set without the given object.

        If the object is not a member, `drop` returns the set itself, or, if
        `error` is true, raises `KeyError`.
        """
        return self.remove(obj) if error else self.discard(obj)
    def __and__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t &= other
        return type(self)(t)
    def __or__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t |= other
        return type(self)(t)
    def __sub__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t -= other
        return type(self)(t)
    def __xor__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t ^= other
        return type(self)(t)
    def __rand__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t &= other
        return type(self)(t)
    def __ror__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t |= other
        return type(self)(t)
    def __rsub__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.clear().transient()
        t.addall(other)
        t -= self
        return type(self)(t)
    def __rxor__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.transient()
        t ^= other
        return type(self)(t)
    def isdisjoint(self, other):
        """Returns `True` if two sets have a null intersection."""
        return _isdisjoint(self, other)
    def issubset(self, other):
        """Report whether another set contains this set."""
        if not isinstance(other, (set, frozenset)):
            other = set(other)
        return other.issuperset(self)
    def issuperset(self, other):
        """Report whether this set contains another set."""
        if isinstance(other, (set, frozenset)):
            return other.issubset(self)
        else:
            return all(map(self.__contains__, iter(other)))
    def difference(self, *args):
        """Returns the difference of two or more sets as a new persistent set.

        (I.e., all elements that are in this set but not the others.)
        """
        t = self.transient()
        for arg in args:
            t.discardall(arg)
        if len(self) == len(t):
            return self
        return type(self)(t)
    def intersection(self, *args):
        """Return the intersection of two sets as a new persistent set.

        (I.e., all elements that are in both sets.)
        """
        t = self.transient()
        for arg in args:
            t &= _as_set(arg)
        if len(t) == len(self):
            return self
        return type(self)(t)
    def symmetric_difference(self, *args):
        """Return the symmetric difference of two sets as a new set.

        (I.e., all elements that are in exactly one of the sets.)
        """
        if not args:
            return self
        t = self.transient()
        for arg in args:
            t ^= _as_set(arg)
        return type(self)(t)
    def union(self, *args):
        """Returns the union of the persistent set and all arguments."""
        t = self.transient()
        for arg in args:
            t.addall(arg)
        if len(t) == len(self):
            return self
        else:
            return type(self)(t)
    def pop(self):
        """Returns a tuple of the last element of the persistent set and a copy
        of the set with that element removed.

        The last element is the one most recently added (the last one that
        iteration yields). Raises `KeyError` if the set is empty.
        """
        if len(self) == 0:
            raise KeyError("pop from an empty set")
        el = _last_element(self)
        return (el, self.discard(el))
    def remove(self, element):
        """Removes an element from the persistent set; it must be a member.

        Raises a `KeyError` if the element is not a member.
        """
        s = self.discard(element)
        if len(s) == len(self):
            raise KeyError(element)
        return s
    def addall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        included.
        """
        t = self.transient()
        t.addall(iterable)
        if len(t) == len(self):
            return self
        else:
            return type(self)(t)
    def discardall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        discarded.

        If any element is not found, then it is ignored.
        """
        t = self.transient()
        t.discardall(iterable)
        if len(t) == len(self):
            return self
        else:
            return type(self)(t)
    def removeall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        discarded.

        If any element is not found, then it is ignored.
        """
        t = self.transient()
        t.removeall(iterable)
        if len(t) == len(self):
            return self
        else:
            return type(self)(t)
    def __reduce__(self):
        # Pickle by class: the class pickles by its qualified name, so a
        # pickle made with one backend loads with the other.
        return (type(self), (list(self),))


#===============================================================================
# PersistentSet

class PersistentSet(_PersistentSetBase, Set, Persistent):
    """All the operations on a persistent set.

    Persistent sets are sets (i.e., objects that inherit from
    `collections.abc.Set`), but they differ from other sets in that they support
    efficient updating by means of efficiently producing copies of themselves
    that incorporate requested changes.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `PersistentSet`, that class is noted in
    parentheses.
     * `__iter__` (`Iterable`)
     * `__len__` (`Sized`)
     * `__contains__` (`Container`)
     * `transient()` (`Persistent`)
     * `add(index, object)`
     * `discard()`
     * `clear()`

    Additionally, `PersistentSequence` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `__setattr__` (`object`; raises a `TypeError`)
     * `__setitem__` (`object`; raises a `TypeError`)
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__ne__` (`object`)
     * `__hash__` (`Hashable`)
     * `__lt__` (`Set`)
     * `__le__` (`Set`)
     * `__gt__` (`Set`)
     * `__or__` (`Set`)
     * `__and__` (`Set`)
     * `__xor__` (`Set`)
     * `__sub__` (`Set`)
     * `__ror__` (`Set`)
     * `__rand__` (`Set`)
     * `__rxor__` (`Set`)
     * `__rsub__` (`Set`)
     * `isdisjoint` (`Set`)
     * `copy()` (`Persistent`)
     * `pop()`
     * `remove(value)`
     * `drop(value, error=False)`
     * `addall(values)`
     * `discardall(values)`
     * `removeall(values)`
     * `__reduce__` (for pickling)
    """
    # See _PersistentBase.__slots__'s comment (abc/_core.py): keeps this mixin,
    # and anything that mixes it in, from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()


#===============================================================================
# _TransientSetBase

class _TransientSetBase(Transient):
    """Plain (non-``ABCMeta``) mixin holding ``TransientSet``'s concrete
    method bodies, so that ``pcollections._c._core.tset`` can inherit them
    without ``ABCMeta`` in its bases; see ``_PersistentBase``'s docstring
    (``abc/_core.py``). ``Transient`` is not ``ABCMeta``-based, so this
    subclasses it directly.
    """
    __slots__ = ()
    # Methods which must be implemented in the children.
    def add(self, obj):
        """Adds the given object to the transient set."""
        raise NotImplementedError()
    def discard(self, obj):
        """Discards the given object from the transient set.

        If the element is not a member, `discard` does nothing.
        """
        raise NotImplementedError()
    def clear(self):
        """Clears the transient set."""
        raise NotImplementedError()
    # Methods which are probably fine for all child classes.
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"{{<{seqstr(self, maxlen=60)}>}}"
    def __repr__(self):
        #s = repr(dict(self))
        #return f"{{<{s[1:-1]}>}}"
        return f"{{<{seqstr(self)}>}}"
    def __eq__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == 0
    def __ne__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) != 0
    # Transient sets are mutable, so they are not hashable.
    __hash__ = None
    def __lt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == -1
    def __le__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) in (-1, 0)
    def __gt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) == 1
    def __ge__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return setcmp(self, other) in (1, 0)
    def isdisjoint(self, other):
        """Returns `True` if two sets have a null intersection."""
        return _isdisjoint(self, other)
    def issubset(self, other):
        """Report whether another set contains this set."""
        if not isinstance(other, (set, frozenset)):
            other = set(other)
        return other.issuperset(self)
    def issuperset(self, other):
        """Report whether this set contains another set."""
        if isinstance(other, (set, frozenset)):
            return other.issubset(self)
        else:
            return all(map(self.__contains__, iter(other)))
    def difference_update(self, *args):
        """Remove all elements of another set from this set."""
        for arg in args:
            self.discardall(arg)
    def difference(self, *args):
        """Returns the difference of two or more sets as a new transient set.

        (I.e., all elements that are in this set but not the others.)
        """
        t = self.copy()
        t.difference_update(*args)
        return t
    def intersection_update(self, *args):
        """Update a set with the intersection of itself and another."""
        for arg in args:
            self &= _as_set(arg)
    def intersection(self, *args):
        """Return the intersection of two sets as a new transient set.

        (I.e., all elements that are in both sets.)
        """
        t = self.copy()
        t.intersection_update(*args)
        return t
    def symmetric_difference_update(self, other):
        """Update a set with the symmetric difference of itself and another."""
        self ^= _as_set(other)
    def symmetric_difference(self, other):
        """Return the symmetric difference of two sets as a new set.

        (I.e., all elements that are in exactly one of the sets.)
        """
        t = self.copy()
        t.symmetric_difference_update(other)
        return t
    def update(self, *args):
        for arg in args:
            self.addall(arg)
    def union(self, *args):
        """Returns the union of the transient set and all arguments."""
        t = self.copy()
        t.update(*args)
        return t
    def __and__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.copy()
        t &= other
        return t
    def __or__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.copy()
        t |= other
        return t
    def __sub__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.copy()
        t -= other
        return t
    def __xor__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.copy()
        t ^= other
        return t
    def __rand__(self, other):
        return self.__and__(other)
    def __ror__(self, other):
        return self.__or__(other)
    def __rsub__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        t = self.copy()
        t.clear()
        t.addall(other)
        t -= self
        return t
    def __rxor__(self, other):
        return self.__xor__(other)
    def __iand__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        rm = [el for el in self if el not in other]
        for el in rm:
            self.remove(el)
        return self
    def __ior__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        self.addall(other)
        return self
    def __isub__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        self.discardall(other)
        return self
    def __ixor__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        rm = []
        for el in self:
            if el in other:
                rm.append(el)
        for el in other:
            if el in self:
                self.remove(el)
            else:
                self.add(el)
        for el in rm:
            self.discard(el)
        return self
    def pop(self):
        """Removes the last element from the transient set and returns it.

        The last element is the one most recently added (the last one that
        iteration yields). Raises `KeyError` if the set is empty.
        """
        if len(self) == 0:
            raise KeyError("pop from an empty set")
        el = _last_element(self)
        self.discard(el)
        return el
    def remove(self, element):
        """Removes an element from the persistent set; it must be a member.

        Raises a `KeyError` if the element is not a member.
        """
        n = len(self)
        self.discard(element)
        if n == len(self):
            raise KeyError(element)
    def addall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        included.
        """
        for el in iterable:
            self.add(el)
    def discardall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        discarded.

        If any element is not found, then it is ignored.
        """
        for el in iterable:
            self.discard(el)
    def removeall(self, iterable):
        """Returns a copy of the persistent set with the given elements
        discarded.

        If any element is not found, then a `KeyError` is raised.
        """
        for el in iterable:
            self.remove(el)
    def copy(self):
        """Returns a copy of the transient set."""
        return self.persistent().transient()
    def __reduce__(self):
        # Pickle by class: the class pickles by its qualified name, so a
        # pickle made with one backend loads with the other.
        return (type(self), (list(self),))


#===============================================================================
# TransientSet

class TransientSet(_TransientSetBase, MutableSet, Transient):
    """All the operations on a transient set.

    Transient sets are mutable sets (i.e., objects that inherit from
    `collections.abc.MutableSet`), but they differ from other mutable sets in
    that they support efficient updating by means of efficiently producing
    copies of themselves that incorporate requested changes.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `TransientSet`, that class is noted in
    parentheses.
     * `__len__` (`Sized`)
     * `__contains__` (`Container`)
     * `__iter__` (`Iterable`)
     * `add(object)` (`MutableSet`)
     * `discard()`  (`MutableSet`)
     * `persistent()` (`Transient`)
     * `clear()`
     * `__getstate__` (for pickling)
     * `__setstate__` (for pickling)

    Additionally, `PersistentSequence` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `__setattr__` (`object`; raises a `TypeError`)
     * `__setitem__` (`object`; raises a `TypeError`)
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__ne__` (`object`)
     * `__lt__` (`Set`)
     * `__le__` (`Set`)
     * `__gt__` (`Set`)
     * `__ge__` (`Set`)
     * `isdisjoint` (`Set`)
     * `__or__` (`Set`)
     * `__and__` (`Set`)
     * `__xor__` (`Set`)
     * `__sub__` (`Set`)
     * `__ror__` (`Set`)
     * `__rand__` (`Set`)
     * `__rxor__` (`Set`)
     * `__rsub__` (`Set`)
     * `__ior__` (`MutableSet`)
     * `__iand__` (`MutableSet`)
     * `__ixor__` (`MutableSet`)
     * `__isub__` (`MutableSet`)
     * `copy()` (`Transient`)
     * `pop()`
     * `remove(value)`
     * `addall(values)`
     * `discardall(values)`
     * `removeall(values)`
    """
    # See _PersistentBase.__slots__'s comment (abc/_core.py): keeps this mixin,
    # and anything that mixes it in, from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()
