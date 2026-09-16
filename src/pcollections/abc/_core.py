# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_core.py
# The definitions of the abstract base classes for the persistent data types.
# By Noah C. Benson

from collections.abc import Hashable


def _partner_type(obj, attr, required):
    """Returns `type(obj).<attr>` (`__transient_type__` or
    `__persistent_type__`), which must be a subclass of `required`."""
    cls = getattr(type(obj), attr)
    if not (isinstance(cls, type) and issubclass(cls, required)):
        raise TypeError(f"{type(obj).__name__}.{attr} must be a subclass of "
                        f"{required.__name__}")
    return cls

def _type_empty(cls, make):
    """Returns the empty instance of persistent type `cls`: `cls.empty` if it
    is an instance of exactly `cls`, and otherwise `make()`, which is cached
    as `cls.empty` unless `cls` defines its own `empty`."""
    e = getattr(cls, 'empty', None)
    if type(e) is cls:
        return e
    e = make()
    if 'empty' not in cls.__dict__:
        try:
            type.__setattr__(cls, 'empty', e)
        except (TypeError, AttributeError):
            pass
    return e


class _PersistentBase:
    """Plain (non-``ABCMeta``) mixin holding ``Persistent``'s concrete method
    bodies, with no abstract base of its own.

    This lets the C extension types in ``pcollections._c._core`` (``pdict``,
    ``pset``, ``plist``, and the others) inherit these methods
    (``__setattr__``/``__delattr__``/``__setitem__``/``__delitem__`` raising
    ``TypeError``, and a ``copy()`` that returns ``self``) from a base whose
    metaclass is ``type``.

    On CPython 3.14, ``PyType_FromSpecWithBases``/``PyType_FromMetaclass``
    reject a base whose metaclass overrides ``tp_new``, as ``ABCMeta`` does
    (``Persistent`` derives from ``collections.abc.Hashable``), so a C heap
    type cannot inherit from ``Persistent`` directly. Passing the metaclass
    to ``PyType_FromMetaclass`` does not help, and reassigning ``__bases__``
    afterward fails because the C types have their own ``tp_dealloc``.

    ``_PersistentBase`` and its siblings ``_PersistentMappingBase``
    (``abc/_map.py``), ``_PersistentSetBase`` (``abc/_set.py``), and
    ``_PersistentSequenceBase`` (``abc/_seq.py``) are the plain bases the C
    types inherit from. The C module then registers each type as a virtual
    subclass of the corresponding ABC (``PersistentMapping`` etc.), so
    ``isinstance``/``issubclass`` checks against those ABCs, and the
    ``collections.abc`` ABCs they derive from, succeed.

    The public ``Persistent`` class mixes this class in alongside
    ``Hashable``, so pure-Python subclasses are real subclasses of both.
    """
    # Empty __slots__ (as in collections.abc's mixins) so this class adds no
    # __dict__/__weakref__; the concrete class decides that. Otherwise the
    # __slots__ of subclasses such as plist/pdict would be defeated, and a C
    # subclass would inherit a tp_dictoffset/tp_weaklistoffset computed for
    # this class's layout, which would corrupt its native fields.
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

    Subclasses: methods that return a changed copy return an instance of the
    same class. A persistent class names its transient partner in the class
    attribute ``__transient_type__`` (used by ``transient()``), and a
    transient class names its persistent partner in ``__persistent_type__``
    (used by ``persistent()``). A subclass that wants ``transient()`` and
    ``persistent()`` to round-trip to itself defines both, for example::

        class MyDict(pdict):
            pass
        class MyTDict(tdict):
            __persistent_type__ = MyDict
        MyDict.__transient_type__ = MyTDict

    (The concrete methods above are implemented on ``_PersistentBase``, a
    plain mixin without ``ABCMeta``; see that class's docstring.
    ``Persistent`` itself is an ``ABCMeta``-based subclass of
    ``collections.abc.Hashable``.)
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

    Unlike ``Persistent``, ``Transient`` is not ``ABCMeta``-based (it does
    not inherit from any ``collections.abc`` class), so it needs no
    ``_TransientBase`` counterpart: the C transient types inherit from it
    through ``_TransientMappingBase`` and its siblings.
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
