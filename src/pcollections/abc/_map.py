# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_map.py
# The definitions of the abstract base classes for the pcollections dict types.
# By Noah C. Benson

from collections.abc import (Mapping, MutableMapping)

from ._core import (Persistent, Transient)
from ..util import (seqstr)


#===============================================================================
# PersistentMapping

class PersistentMapping(Mapping, Persistent):
    """All the operations on a persistent mapping.

    Persistent mappings are mappings (i.e., objects that inherit from
    `collections.abc.Mapping`), but they differ from other mappings in that they
    support efficient updating by means of efficiently producing copies of
    themselves that incorporate requested changes.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `PersistentMapping`, that class is noted in
    parentheses.
     * `__len__` (`Sized`)
     * `__iter__` (`Iterable`)
     * `__getitem__` (`Mapping`)
     * `transient()` (`Persistent`)
     * `set(key, value)`
     * `drop(key)`
     * `clear()`

    Additionally, `PersistentMapping` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `__setattr__` (`object`; raises a `TypeError`)
     * `__setitem__` (`object`; raises a `TypeError`)
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__ne__` (`object`)
     * `__hash__` (`Hashable`)
     * `__contains__` (`Container`)
     * `get(key, default=None)` (`Mapping`)
     * `keys()` (`Mapping`)
     * `items()` (`Mapping`)
     * `values()` (`Mapping`)
     * `setdefault(key, default=None)` (`MutableMapping`)
     * `popitem()` (`MutableMapping`)
     * `pop(key)` (`MutableMapping`)
     * `copy()` (`Persistent`)
     * `delete(key)`
     * `remove(value)`
     * `setall(values)`
     * `dropall(key)`
     * `deleteall(key)`
     * `discardall(values)`
     * `removeall(values)`
     * `update(map, **kw)`
     * `__reduce__` (for pickling)
     * `__json__` (for `json_fix` module)
    """
    # Methods which must be implemented in the children.
    def set(self, key, val):
        """Returns a copy of the pdict that maps the given key to the given
        value."""
        raise NotImplementedError()
    def drop(self, key, error=False):
        """Returns a copy of the persistent mapping that does not include the
        given key.

        If the key is not in the dict, `drop` returns the original persistent
        mapping.
        """
        raise NotImplementedError()
    def clear(self):
        """Returns the empty persistent mapping of the same type."""
        raise NotImplementedError()
    # Methods that are probably fine for any child-class of PersistentMapping.
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"{{|{seqstr(self, maxlen=60)}|}}"
    def __repr__(self):
        return f"{{|{seqstr(self)}|}}"
    def __hash__(self):
        return hash(frozenset(map(lambda u: u[1][0], self._els))) + 2
    def __contains__(self, k):
        try:
            self[k]
            return True
        except KeyError:
            return False
    def get(self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default
    def delete(self, key):
        """Returns a copy of the mersistent mapping the excludes the given key.

        If the key is not found in the mapping, a `KeyError` is raised.
        """
        if key in self:
            return self.drop(key)
        else:
            raise KeyError(key)
    def setall(self, keys, vals):
        """Returns a copy of the persistent mapping that includes all the object
        in the given iterables of keys and values.
        """
        t = self.transient()
        for (k,v) in zip(keys, vals):
            t[k] = v
        return type(self)(t)
    def dropall(self, keys):
        """Returns a copy of the persistent mapping that excludes all the keys
        in the given iterable.

        Keys that are not found in the mapping are ignored.
        """
        t = self.transient()
        for k in keys:
            t.drop(k)
        return type(self)(t)
    def deleteall(self, keys):
        """Returns a copy of the persistent mapping that excludes all the keys
        in the given iterable.

        If any key is not found in the mapping, then a KeyError error is raised.
        """
        t = self.transient()
        for k in keys:
            del t[k]
        return type(self)(t)
    def setdefault(self, key, default=None):
        """Returns a copy of the persistent mapping with the key inserted with a
        value of default, if key is not already in the mapping.
        """
        return self if key in self else self.set(key, default)
    def popitem(self):
        """Returns a 2-tuple whose first element is itself a 2-tuple, `(key,
        value)` from the persistent mapping, and whose second element is a copy
        of the mapping with the key-value pair removed.

        Raises `KeyError` if the persistent mapping is empty.
        """
        if len(self) == 0:
            raise KeyError("popitem(): persistent mapping is empty")
        kv = next(iter(self.items()))
        return (kv, self.drop(kv[0]))
    def pop(self, key, *args):
        """Returns a tuple of the value mapped to the given key and a copy of
        the persistent mapping with that key removed. 

        If the key is not found, the second argument is returned, if given,
        otherwise, a `KeyError` is raised.
        """
        nargs = len(args)
        if nargs > 1:
            raise TypeError(f"pop expected at most 2 arguments, got {nargs}")
        try:
            val = self[key]
            return (val, self.drop(key))
        except KeyError:
            # It's not here!
            if nargs == 0:
                raise
            else:
                return (args[0], self)
    def update(self, *args, **kw):
        """Returns a copy of the persistent mapping with the key-value pairs in
        the iterable/mapping `arg` and the keyword arguments included.

        Multiple arguments may be provided, but they must each be a mapping or
        an iterable of items.
        """
        t = self.transient()
        for arg in args:
            for (k,v) in (arg.items() if isinstance(arg, Mapping) else arg):
                t[k] = v
        for (k,v) in kw.items():
            t[k] = v
        return type(self)(t)
    def __reduce__(self):
        return (self.__new__, (type(self), list(self.items()),))
    def __json__(self):
        from json import dumps
        return dumps(dict(self))


#===============================================================================
# TransientMapping

class TransientMapping(MutableMapping, Transient):
    """All the operations on a transient mapping.

    Transient mappings are mutable mappings (i.e., objects that inherit from
    `collections.abc.MutableMapping`), but they differ from other mutable
    mappings in that they support efficient conversion to and from persistent
    mappings.

    The following abstract methods must be implemented; if these methods are
    inherited from a superclass of `TransientMapping`, that class is noted in
    parentheses.
     * `__len__` (`Sized`)
     * `__iter__` (`Iterable`)
     * `__getitem__` (`Mapping`)
     * `__setitem__` (`MutableMapping`)
     * `__delitem__` (`MutableMapping`)
     * `transient()` (`Persistent`)
     * `clear()`

    Additionally, `TransientMapping` includes default implementations of the
    following methods, which may or may not be optimal for any particular
    base-class.
     * `__setattr__` (`object`; raises a `TypeError`)
     * `__str__` (`object`)
     * `__repr__` (`object`)
     * `__eq__` (`object`)
     * `__ne__` (`object`)
     * `__contains__` (`Container`)
     * `get(key, default=None)` (`Mapping`)
     * `keys()` (`Mapping`)
     * `items()` (`Mapping`)
     * `values()` (`Mapping`)
     * `setdefault(key, default=None)` (`MutableMapping`)
     * `popitem()` (`MutableMapping`)
     * `pop()` (`MutableMapping`)
     * `update(map, **kw)` (`MutableMapping`)
     * `copy()` (`Transient`)
     * `__reduce__` (for pickling)
     * `__json__` (for the `json_fix` module)
    """
    # Methods which must be implemented in the children.
    def clear(self):
        """Returns the empty persistent mapping of the same type."""
        raise NotImplementedError()
    # Methods that are probably fine for any child-class of PersistentMapping.
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"{{<{seqstr(self, maxlen=60)}>}}"
    def __repr__(self):
        #s = repr(dict(self))
        #return f"{{<{s[1:-1]}>}}"
        return f"{{|{seqstr(self)}|}}"
    def __contains__(self, k):
        try:
            self[k]
            return True
        except KeyError:
            return False
    def get(self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default
    def setdefault(self, key, default=None):
        """Insert key with a value of default if key is not in the dictionary.

        Return the value for the key if key is in the dictionary, else default.
        """
        try:
            return self[key]
        except KeyError:
            pass
        self[key] = default
        return default
    def popitem(self):
        """Remove and return a (key, value) pair as a 2-tuple.

        Raises `KeyError` if the persistent mapping is empty.
        """
        if len(self) == 0:
            raise KeyError("popitem(): transient mapping is empty")
        kv = next(iter(self.items()))
        del self[kv[0]]
        return kv
    def pop(self, key, *args):
        """Removes the specified key and returns the corresponding value.

        If the key is not found, the second argument is returned, if given,
        otherwise, a `KeyError` is raised.
        """
        nargs = len(args)
        if nargs > 1:
            raise TypeError(f"pop expected at most 2 arguments, got {nargs}")
        try:
            val = self[key]
            del self[key]
            return val
        except KeyError:
            # It's not here!
            if nargs == 0:
                raise
            else:
                return args[0]
    def update(self, *args, **kw):
        """Updates the transient mapping with items from the arguments and
        options.

        Multiple arguments may be provided, but they must each be either a
        mapping or an iterable of items.
        """
        for arg in args:
            for (k,v) in (arg.items() if isinstance(arg, Mapping) else arg):
                self[k] = v
        for (k,v) in kw.items():
            self[k] = v
    def copy(self):
        """Returns a copy of the transient mapping."""
        return self.persistent().transient()
    # The below handle pickling cases.
    def __reduce__(self):
        return (self.__new__, (type(self), list(self.items()),))
    def __json__(self):
        from json import dumps
        return dumps(dict(self))
        
