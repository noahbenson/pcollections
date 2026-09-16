# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_core.py
# The definitions of the abstract base classes for the persistent data types.
# By Noah C. Benson

from collections.abc import Hashable


class _PersistentBase:
    """Plain (non-``ABCMeta``) mixin holding ``Persistent``'s concrete method
    bodies, with no abstract base of its own.

    This exists so that C extension types (``pcollections._c._core.pdict``,
    ``_c.set.pset``, ``_c.list.plist``, and friends) can inherit these
    methods -- ``__setattr__``/``__delattr__``/``__setitem__``/``__delitem__``
    raising ``TypeError``, and a ``copy()`` that returns ``self`` -- from a
    base class whose metaclass is plain ``type``, not ``ABCMeta``.

    CPython 3.14 tightened ``PyType_FromSpecWithBases``/``PyType_FromMetaclass``
    (the C API the extension types use to build themselves as heap types at
    import time) to unconditionally reject a base whose metaclass overrides
    ``tp_new`` -- which ``ABCMeta`` does (``Persistent`` inherits from
    ``collections.abc.Hashable``, an ``ABCMeta``-based class) -- so a C type
    that inherited directly from ``Persistent`` can no longer be constructed
    at all on 3.14. Confirmed empirically (against a real 3.14 interpreter)
    that there is no C-API-level workaround: not passing the correct
    metaclass explicitly to ``PyType_FromMetaclass``, not reassigning
    ``__bases__`` after construction (the latter also fails outright, with a
    ``deallocator differs from 'object'`` ``TypeError``, since these C types
    have a custom ``tp_dealloc`` for their native struct fields that could
    never match a plain-Python class's deallocator anyway).

    ``_PersistentBase`` -- and its siblings ``_PersistentMappingBase``
    (``abc/_map.py``), ``_PersistentSetBase`` (``abc/_set.py``), and
    ``_PersistentSequenceBase`` (``abc/_seq.py``) -- give the C extensions a
    plain class to inherit from instead. The extensions then call
    ``.register()`` on the real ``Persistent``/``PersistentMapping``/etc. ABCs
    (see each ``_c/*.c``'s ``PyInit_*``) so that ``isinstance``/``issubclass``
    checks against ``pcollections.abc.Persistent`` (and, transitively, against
    ``collections.abc.Hashable``/``Mapping``/``Set``/``Sequence``, since a
    ``.register()`` with a *real* subclass of those stdlib ABCs is honored by
    them too -- confirmed empirically) keep working exactly as before, without
    ever asking the C API to build a heap type on top of an ``ABCMeta`` base.

    The public ``Persistent`` class below still mixes this class in alongside
    the real ``Hashable`` ABC, so pure-Python subclasses (and
    ``pcollections.abc``'s own public interface) are completely unaffected:
    ``isinstance(x, Persistent)`` and ``isinstance(x, Hashable)`` are both
    still literally true (not just true via ``.register()``) for any ordinary
    Python subclass, exactly as before this fix.
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


class Persistent(_PersistentBase, Hashable):
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

    (The concrete methods above are actually implemented on ``_PersistentBase``,
    a plain mixin with no ``ABCMeta`` in its inheritance chain -- see that
    class's docstring for why. ``Persistent`` itself is unaffected: it's
    still a real, ``ABCMeta``-based subclass of ``collections.abc.Hashable``,
    exactly as before.)
    """
    __slots__ = ()
    # Abstract methods.
    def transient(self):
        """Efficiently returns a transient copy of the persistent object."""
        raise NotImplementedError()


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

    Unlike ``Persistent``, ``Transient`` was never ``ABCMeta``-based to begin
    with (it doesn't inherit from any ``collections.abc`` class), so it needs
    no ``_TransientBase``-style split for the C extensions: ``_c/dict.c.h`` and
    friends can -- and do -- keep inheriting from ``Transient`` directly.
    """
    # See the matching comment on _PersistentBase.__slots__ above: this keeps
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
