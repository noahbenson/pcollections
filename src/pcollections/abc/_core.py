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
    # No instance state of its own, and no __dict__/__weakref__ either: like
    # collections.abc's own mixins, this is meant to be combined with other
    # bases via multiple inheritance, and a concrete leaf class decides for
    # itself (via its own __slots__, or lack thereof) whether instances get a
    # __dict__. Without this, *every* class in the MRO that omits __slots__
    # gives instances a __dict__ regardless of what the leaf class declares,
    # silently defeating any __slots__ a concrete subclass (e.g. plist/pdict)
    # declares -- and, for a C-implemented subclass, inheriting a nonzero
    # tp_dictoffset/tp_weaklistoffset computed for *this* class's own (much
    # smaller) layout onto a C struct with extra native fields corrupts
    # memory the instant a weakref or instance attribute touches it.
    __slots__ = ()
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
    # See the matching comment on Persistent.__slots__ above: this keeps
    # Transient (and everything that mixes it in) from acquiring an instance
    # __dict__/__weakref__ of its own.
    __slots__ = ()
    def persistent(self):
        """Efficiently returns a persistent copy of the transient object."""
        raise NotImplementedError()
    # Implementations that are probably fine for most children.
    def copy(self):
        """Returns a copy of the transient object."""
        return self.persistent().transient()
    
