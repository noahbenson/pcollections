# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_core.py
# The definitions of the abstract base classes for the persistent data types.
# By Noah C. Benson

from collections.abc import Hashable

class Persistent(Hashable):
    """An abstract type for any persistent object.

    The ``Persistent`` type is an abstract base class for objects that are
    immutable. The class implements the following methods, each of which throw
    a ``TypeError``:
     * `__setattr__`
     * `__delattr__`
     * `__setitem__`
     * `__delitem__`

    It also implements a ``copy()`` method that simply returns ``self``.

    The class includes one abstract method, ``transient()``, which can be
    overloaded if the object has a transient companion type.
    """
    # Abstract methods.
    def transient(self):
        """Efficiently returns a transient copy of the persistent object."""
        raise NotImplementedError()
    # Methods that should throw errors in children.
    def __setattr__(self, k, v):
        raise TypeError(f"{type(self)} is immutable")
    def __delattr__(self, k):
        raise TypeError(f"{type(self)} is immutable")
    def __setitem__(self, k, v):
        raise TypeError(f"{type(self)} is immutable")
    def __delitem__(self, k):
        raise TypeError(f"{type(self)} is immutable")
    # Implementation methods that are probably fine in all children.
    def copy(self):
        """Returns the persistent object (persistent objects needn't be
        copied)."""
        return self

class Transient:
    """The abstract base type for transient objects.

    Transient objects are companions to ``Persistent`` objects. Transient
    objects are typically mutable objects with structures similar to their
    immutable companion types that can be quickly mutated then quickly frozen
    back into persistent types.

    The ``Transient`` type implements two methods: ``persistent()``, which
    should convert the object's data into a persistent object and return that
    new persistent object, and ``copy()``, which by default returns
    ``self.persistent().transient()``.
    """
    def persistent(self):
        """Efficiently returns a persistent copy of the transient object."""
        raise NotImplementedError()
    # Implementations that are probably fine for most children.
    def copy(self):
        """Returns a copy of the transient object."""
        return self.persistent().transient()
    
