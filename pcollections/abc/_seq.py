# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_seq.py
# The definitions of the abstract base classes for the pcollections sequence
# types.
# By Noah C. Benson

from numbers         import (Integral)
from collections.abc import (Sequence, MutableSequence)

from ._core import (Persistent, Transient)
from ..util import (seqstr, seqcmp)


#===============================================================================
# PersistentSequence

class PersistentSequence(Persistent, Sequence):
    """All the operations on a persistent sequence.

    Persistent sequences are sequences (i.e., objects that inherit from
    `collections.abc.Sequence`), but they differ from other sequences in that
    they support efficient updating by means of efficiently producing copies of
    themselves that incorporate requested changes.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `PersistentSequence`, that class is noted in
    parentheses.
     * `set(index, object)`
     * `delete(index=-1)`
     * `append(object)`
     * `prepend(object)`
     * `insert(index, object)`
     * `clear()`
     * `transient()` (`Persistent`)
     * `__iter__` (`Iterable`)
     * `__len__` (`Sized`)
     * `__getitem__` (`Sequence`)
     * `__reduce__` (for pickling)

    Additionally, `PersistentSequence` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__hash__` (`Hashable`)
     * `__contains__` (`Container`)
     * `__reversed__` (`Reversible`)
     * `count` (`Sequence`)
     * `extend(iterable)` (`Sequence`)
     * `index(value)` (`Sequence`)
     * `copy()` (`Persistent`)
     * `__add__`
     * `__radd__`
     * `__mul__`
     * __rmul__`
     * `drop(index=-1)`
     * `pop(index=-1)`
     * `remove(value)`
     * `sort()`
     * `reverse()`
     * `__json__` (for `json_fix` module)
    """
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
    def drop(self, index=-1):
        """Returns a copy of the persistent sequence with the value at the given
        index dropped.

        If the index is too large or too small for the size of the sequence,
        then the sequence is returned as-is. Non-integer indices will still
        result in raised exceptions."""
        n = len(self)
        if index < -n or index >= n:
            return self
        else:
            return self.delete(index)
    def pop(self, index=-1):
        """Returns tuple of the value at the given index and a copy of the
        persistent sequence with the item at that index removed (default index:
        last)."""
        el = self[index]
        return (el, self.delete(index))
    def remove(self, value):
        """Returns a new list with the first occurence of value removed.

        Raises ValueError if the value is not present.
        """
        ii = self.index(value)
        return self.delete(ii)
    def sort(self, key=None, reverse=False):
        """Returns a sorted copy of the given persistent sequence."""
        t = self.clear().transient()
        for k in sorted(self, key=key, reverse=reverse):
            t.append(k)
        return type(self)(t)
    def reverse(self):
        """Returns a plist that is a reversed copy."""
        t = self.clear().transient()
        for el in self.__reversed__():
            t.append(el)
        return type(self)(t)
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"[|{seqstr(self, maxlen=60)}|]"
    def __repr__(self):
        #s = repr(list(self))
        #return f"[|{s[1:-1]}|]"
        return f"[|{seqstr(self)}|]"
    _eq_types = ()
    def __eq__(self, other):
        if other is self:
            return True
        elif not isinstance(other, PersistentSequence._eq_types):
            return False
        else:
            return seqcmp(self, other) == 0
    def __lt__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'<' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) < 0
    def __le__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'<=' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) <= 0
    def __gt__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'>' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) > 0
    def __ge__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'>=' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) >= 0
    def __hash__(self):
        return hash(tuple(self)) + 1
    def __contains__(self, value):
        for el in self:
            if el == value:
                return True
        return False
    def __reversed__(self):
        n = len(self)
        return map(self.__getitem__, range(n - 1, -1, -1))
    def count(self, value):
        """Returns the number of occurences of value."""
        n = 0
        for (k,obj) in self._phamt:
            if obj == value: n += 1
        return n
    def extend(self, iterable):
        """Return a new persistent sequence with the iterables appended."""
        t = self.transient()
        for el in iterable:
            t.append(el)
        return type(self)(t)
    def index(self, value, start=0, stop=None):
        """Returns first index of value.

        Raises ValueError if value is not present.
        """
        if stop is None:
            stop = len(self)
        if start == 0:
            for (ii,val) in enumerate(self):
                if ii >= stop:
                    break
                elif val == value:
                    return ii
        else:
            for (ii,val) in enumerate(self):
                if ii >= stop:
                    break
                elif ii < start:
                    pass
                elif val == value:
                    return ii
        raise ValueError(f"{value} is not in {type(self)}")
    # We include a hash function; though this should be overwritten to cache the
    # hash-code due to it's slowness.
    def __add__(self, obj):
        if not isinstance(obj, (list, PersistentSequence)):
            msg = (f"unsuppoorted operand type for +:"
                   f" '{type(obj)}' and '{type(self)}'")
            raise TypeError(msg)
        elif len(obj) == 0:
            return self
        elif len(self) == 0 and isinstance(obj, PersistentSequence):
            return obj
        else:
            return self.extend(obj)
    def __radd__(self, obj):
        if isinstance(obj, PersistentSequence):
            return NotImplemented
        elif not isinstance(obj, list):
            msg = (f"unsuppoorted operand type for +:"
                   f" '{type(self)}' and '{type(obj)}'")
            raise TypeError(msg)
        elif len(obj) == 0:
            return self
        elif len(self) == 0 and isinstance(obj, PersistentSequence):
            return obj
        else:
            t = self.transient()
            for el in reversed(obj):
                t.prepend(el)
            return type(self)(t)
    def __mul__(self, value):
        if not isinstance(value, Integral):
            msg = f"can't multiply sequence by non-int of type '{type(value)}'"
            raise ValueError(msg)
        reps = int(value)
        if reps < 0:
            reps = 0
        if reps == 0:
            return self.clear()
        elif reps == 1:
            return self
        t = self.transient()
        for r in range(reps - 1):
            for el in self:
                t.append(el)
        return type(self)(t)
    def __rmul__(self, value):
        return self.__mul__(value)
    # For pickling:
    def __reduce__(self):
        return (self.__new__, (type(self), list(self),))
    def __json__(self):
        from json import dumps
        return dumps(list(self))


#===============================================================================
# TransientSequence

class TransientSequence(Transient, MutableSequence):
    """All the operations on a transient sequence.

    Transient sequences are mutable sequences (i.e., objects that inherit from
    `collections.abc.MutableSequence`), but they differ from other sequences in
    that they support efficient conversion to and from a paired persistent
    datatype.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `TransientSequence`, that class is noted in
    parentheses.
     * `clear()`
     * `persistent()` (`Transient`)
     * `__iter__` (`Iterable`)
     * `__len__` (`Sized`)
     * `__getitem__` (`Sequence`)
     * `__setitem__(index, object)` (`MutableSequence`)
     * `__delitem__(index)` (`MutableSequence`)
     * `append(object)` (`MutableSequence`)
     * `prepend(object)` (`MutableSequence`)
     * `insert(index, object)` (`MutableSequence`)

    Additionally, `TransientSequence` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `pop(index=-1)`
     * `remove(value)`
     * `sort()`
     * `reverse()`
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__contains__` (`Container`)
     * `__reversed__` (`Reversible`)
     * `count` (`Sequence`)
     * `extend(iterable)` (`Sequence`)
     * `index(value)` (`Sequence`)
     * `copy()` (`Transient`)
     * `__add__`
     * `__radd__`
     * `__iadd__`
     * `__mul__`
     * `__rmul__`
     * `__imul__`
     * `__reduce__` (for pickling)
     * `__json__` (for `json_fix` module)
    """
    def pop(self, index=-1):
        """Remove and return item at index (default last).

        Raises IndexError if list is empty or index is out of range."""
        el = self[index]
        del self[index]
        return el
    def remove(self, value):
        """Removes the first occurence of value.

        Raises ValueError if the value is not present.
        """
        ii = self.index(value)
        del self[ii]
    def sort(self, key=None, reverse=False):
        """Sort the tlist in ascending order and return None.

        The sort is in-place (i.e. the tlist itself is modified) and stable
        (i.e. the order of two equal elements is maintained).

        If a key function is given, apply it once to each list item and sort
        them, ascending or descending, according to their function values.

        The reverse flag can be set to sort in descending order.
        """
        for (ii,el) in enumerate(sorted(self, key=key, reverse=reverse)):
            t[ii] = el
    def count(self, value):
        """Returns the number of occurences of value."""
        n = 0
        for obj in self:
            if obj == value: n += 1
        return n
    def extend(self, iterable):
        """Extends tlist by appending elements from the iterable."""
        for val in iterable:
            self.append(val)
    def index(self, value, start=0, stop=None):
        """Returns the first index of value.

        Raises ValueError if value is not present.
        """
        for (ii,el) in enumerate(self):
            if value == el:
                return ii
        raise ValueError(f'{value} is not in {type(self)}')
    def reverse(self):
        """Reverses *IN PLACE*."""
        n = len(self)
        for k in range(n//2):
            kk = n - k
            tmp = self[k]
            self[k] = self[kk]
            self[kk] = tmp
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"[<{seqstr(self, maxlen=60)}>]"
    def __repr__(self):
        return f"[<{seqstr(self)}>]"
    _eq_types = ()
    def __eq__(self, other):
        if other is self:
            return True
        elif not isinstance(other, TransientSequence._eq_types):
            return False
        elif len(self) != len(other):
            return False
        else:
            return seqcmp(self, other) == 0
    def __lt__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'<' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) < 0
    def __le__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'<=' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) <= 0
    def __gt__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'>' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) > 0
    def __ge__(self, other):
        if not isinstance(other, (list, PersistentSequence, TransientSequence)):
            raise TypeError(f"'>=' not supported between instances of"
                            f" '{type(self)}' and '{type(other)}'")
        return seqcmp(self, other) >= 0
    def __contains__(self, value):
        for el in self:
            if el == value:
                return True
        return False
    def __reversed__(self):
        n = len(self)
        return map(self.__getitem__, range(n - 1, -1, -1))
    def count(self, value):
        """Returns the number of occurences of value."""
        n = 0
        for (k,obj) in self._phamt:
            if obj == value: n += 1
        return n
    def extend(self, iterable):
        """Return a new persistent sequence with the iterables appended."""
        for el in iterable:
            self.append(el)
    def index(self, value, start=0, stop=None):
        """Returns first index of value.

        Raises ValueError if value is not present.
        """
        if start == 0:
            for (ii,val) in enumerate(self):
                if ii >= stop:
                    break
                elif val == value:
                    return ii
        else:
            for (ii,val) in enumerate(self):
                if ii >= stop:
                    break
                elif ii < start:
                    pass
                elif val == value:
                    return ii
        raise ValueError(f"{value} is not in {type(self)}")
    def __iadd__(self, obj):
        if not isinstance(obj, (list, PersistentSequence, TransientSequence)):
            msg = (f"unsuppoorted operand type for +=:"
                   f" '{type(obj)}' and '{type(self)}'")
            raise TypeError(msg)
        elif len(obj) == 0:
            pass
        elif obj is self:
            self.extend(self.persistent())
        else:
            self.extend(obj)
        return self
    def __add__(self, obj):
        if not isinstance(obj, (list, PersistentSequence, TransientSequence)):
            return NotImplemented
        t = self.copy()
        t += obj
        return t
    def __radd__(self, obj):
        if not isinstance(obj, (list, PersistentSequence, TransientSequence)):
            msg = (f"unsuppoorted operand type for +:"
                   f" '{type(self)}' and '{type(obj)}'")
            raise TypeError(msg)
        elif len(obj) == 0:
            return self.copy()
        else:
            t = self.copy()
            for el in reversed(obj):
                t.prepend(el)
            return r
    def __imul__(self, value):
        if not isinstance(value, Integral):
            msg = f"can't multiply sequence by non-int of type '{type(value)}'"
            raise ValueError(msg)
        reps = int(value)
        if reps < 0:
            reps = 0
        if reps == 0:
            self.clear()
        elif reps > 1:
            n = len(self)
            for r in range(reps - 1):
                for (ii,el) in enumerate(self):
                    if ii > n:
                        break
                    else:
                        self.append(el)
        return self
    def __mul__(self, value):
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
        return (self.__new__, (type(self), list(self),))
    def __json__(self):
        from json import dumps
        return dumps(list(self))    

# Setup the _eq_types, which decides what types can be considered equal.
PersistentSequence._eq_types = (list, PersistentSequence, TransientSequence)
TransientSequence._eq_types = (list, PersistentSequence, TransientSequence)
