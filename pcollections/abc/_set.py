# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_set.py
# The definitions of the abstract base classes for the pcollections set types.
# By Noah C. Benson

from collections.abc import (Set, MutableSet)

from ._core import (Persistent, Transient)
from ..util import (setcmp, seqstr)


#===============================================================================
# PersistentSet

class PersistentSet(Set, Persistent):
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
     * `addall(values)`
     * `discardall(values)`
     * `removeall(values)`
     * `__reduce__` (for pickling)
    """
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
        if isinstance(other, Set):
            return setcmp(self, other) == 0
        else:
            return False
    def __ne__(self, other):
        if isinstance(other, Set):
            return setcmp(self, other) != 0
        else:
            return True
    def __hash__(self):
        return hash(frozenset(self)) + 1
    def __lt__(self, other):
        return setcmp(self, other) == -1
    def __le__(self, other):
        c = setcmp(self, other)
        return c == -1 or c == 0
    def __gt__(self, other):
        return setcmp(self, other) == 1
    def __ge__(self, other):
        c = setcmp(self, other)
        return c == 1 or c == 0
    def __and__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for &:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.transient()
        t &= other
        return type(self)(t)
    def __or__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for |:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.transient()
        t |= other
        return type(self)(t)
    def __sub__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for -:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.transient()
        t -= other
        return type(self)(t)
    def __xor__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for ^:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.transient()
        t ^= other
        return type(self)(t)
    def __rand__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for &:"
                            f" '{type(other)}' and '{type(set)}'")
        t = self.transient()
        t &= other
        return type(self)(t)
    def __ror__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for |:"
                            f" '{type(other)}' and '{type(set)}'")
        t = self.transient()
        t |= other
        return type(self)(t)
    def __rsub__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for -:"
                            f" '{type(other)}' and '{type(set)}'")
        t = self.clear().transient()
        t.adall(other)
        t -= self
        return type(self)(t)
    def __rxor__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for ^:"
                            f" '{type(other)}' and '{type(set)}'")
        t = self.transient()
        t ^= other
        return type(self)(t)
    def isdisjoint(self, other):
        """Returns `True` if two sets have a null intersection."""
        return setcmp(self, other) is None
    def pop(self):
        """Returns a tuple of an arbitrary element from the persistent set and a
        copy of the set with that element removed.

        Raises `KeyError` if the set is empty.
        """
        if len(self) == 0:
            raise KeyError(f"pop from empty {type(self)}")
        el = next(iter(self))
        newset = self.discard(el)
        return (el, newset)
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
        return (self.__new__, (type(self), list(self),))


#===============================================================================
# TransientSet

class TransientSet(MutableSet, Transient):
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
     * `__hash__` (`Hashable`)
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
        return setcmp(self, other) == 0
    def __ne__(self, other):
        return setcmp(self, other) != 0
    def __hash__(self):
        return hash(frozenset(self)) + 1
    def __lt__(self, other):
        return setcmp(self, other) == -1
    def __le__(self, other):
        c = setcmp(self, other)
        return c == -1 or c == 0
    def __gt__(self, other):
        return setcmp(self, other) == 1
    def __ge__(self, other):
        c = setcmp(self, other)
        return c == 1 or c == 0
    def isdisjoint(self, other):
        """Returns `True` if two sets have a null intersection."""
        return setcmp(self, other) is None
    def __and__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for &:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.copy()
        t &= other
        return t
    def __or__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for |:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.copy()
        t |= other
        return t
    def __sub__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for -:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.copy()
        t -= other
        return t
    def __xor__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for ^:"
                            f" '{type(self)}' and '{type(other)}'")
        t = self.copy()
        t ^= other
        return t
    def __rand__(self, other):
        return self.__and__(other)
    def __ror__(self, other):
        return self.__or__(other)
    def __rsub__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for -:"
                            f" '{type(other)}' and '{type(set)}'")
        t = self.copy()
        t.clear()
        t.adall(other)
        t -= self
        return t
    def __rxor__(self, other):
        return self.__xor__(other)
    def __iand__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for &=:"
                            f" '{type(self)}' and '{type(other)}'")
        rm = [el for el in self if el not in other]
        for el in rm:
            self.remove(el)
        return self
    def __ior__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for |=:"
                            f" '{type(self)}' and '{type(other)}'")
        self.addall(other)
        return self
    def __isub__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for -=:"
                            f" '{type(self)}' and '{type(other)}'")
        self.discardall(other)
        return self
    def __ixor__(self, other):
        if not isinstance(other, Set):
            raise TypeError(f"unsupported operand type for ^=:"
                            f" '{type(self)}' and '{type(other)}'")
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
        """Removes an arbitrary element from the transient set and returns it.

        Raises `KeyError` if the set is empty.
        """
        if len(self) == 0:
            raise KeyError(f"pop from empty {type(self)}")
        el = next(iter(self))
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
        return (self.__new__, (type(self), list(self),))
