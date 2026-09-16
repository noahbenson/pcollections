# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_view.py
# Plain (non-ABCMeta) mixins backing pcollections._c._core's six view types
# (pdict_keys/pdict_items/pdict_values/tdict_keys/tdict_items/tdict_values),
# and the view classes for the C backend's lazy mappings.
# By Noah C. Benson

"""Plain mixins for the ``dict``-view family (``keys()``/``items()``/
``values()``).

On CPython 3.14, ``PyType_FromSpecWithBases``/``PyType_FromMetaclass``
reject a base whose metaclass overrides ``tp_new``, as ``ABCMeta`` does, so
the C backend cannot build its view heap types directly on
``collections.abc.KeysView``/``ItemsView``/``ValuesView``. (See
``pcollections.abc._core._PersistentBase`` for the same issue with the
collection types.)

``_MappingViewBase``/``_KeysViewBase``/``_ItemsViewBase``/``_ValuesViewBase``
copy the concrete methods of ``collections.abc``'s ``MappingView``/
``KeysView``/``ItemsView``/``ValuesView`` (and, for the set-like Keys/Items
views, of ``collections.abc.Set``) without ``ABCMeta`` in their MRO.
``build_view_type()`` in ``_c/dict.c.h`` builds the six view types on these,
and ``pcoll_exec_dict()`` registers them with the stdlib view ABCs so that
``isinstance``/``issubclass`` checks against those ABCs (and the ABCs they
derive from) succeed.

These mixins are used only by the C backend; the pure-Python backend's views
(in ``pcollections._dict``) subclass the stdlib ABCs directly.

``ldict_items``/``ldict_values`` at the end of this module are the views
returned by the C backend's ``ldict``/``tldict`` ``items()``/``values()``.
"""

from collections.abc import (Set, Iterable)


#===============================================================================
# _MappingViewBase

class _MappingViewBase:
    """Plain copy of ``collections.abc.MappingView``'s concrete methods.

    Declares the ``_mapping`` slot (as ``MappingView`` does), which provides
    the storage ``dictview_new`` in ``_c/dict.c.h`` uses;
    ``build_view_type()`` reads this class's ``tp_basicsize``.
    """
    __slots__ = ('_mapping',)
    def __len__(self):
        return len(self._mapping)
    def __repr__(self):
        return '{0.__class__.__name__}({0._mapping!r})'.format(self)


#===============================================================================
# _SetViewMixin

class _SetViewMixin:
    """Plain copy of ``collections.abc.Set``'s concrete methods, for the
    set-like Keys/Items view mixins. ``_from_iterable`` returns a plain
    ``set``, as ``KeysView``/``ItemsView`` do in the stdlib, since the view
    constructors take a mapping rather than an iterable.
    """
    __slots__ = ()
    def __le__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        if len(self) > len(other):
            return False
        for elem in self:
            if elem not in other:
                return False
        return True
    def __lt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return len(self) < len(other) and self.__le__(other)
    def __gt__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return len(self) > len(other) and self.__ge__(other)
    def __ge__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        if len(self) < len(other):
            return False
        for elem in other:
            if elem not in self:
                return False
        return True
    def __eq__(self, other):
        if not isinstance(other, Set):
            return NotImplemented
        return len(self) == len(other) and self.__le__(other)
    @classmethod
    def _from_iterable(cls, it):
        return set(it)
    def __and__(self, other):
        if not isinstance(other, Iterable):
            return NotImplemented
        return self._from_iterable(value for value in other if value in self)
    __rand__ = __and__
    def isdisjoint(self, other):
        'Return True if two sets have a null intersection.'
        for value in other:
            if value in self:
                return False
        return True
    def __or__(self, other):
        if not isinstance(other, Iterable):
            return NotImplemented
        chain = (e for s in (self, other) for e in s)
        return self._from_iterable(chain)
    __ror__ = __or__
    def __sub__(self, other):
        if not isinstance(other, Set):
            if not isinstance(other, Iterable):
                return NotImplemented
            other = self._from_iterable(other)
        return self._from_iterable(value for value in self
                                   if value not in other)
    def __rsub__(self, other):
        if not isinstance(other, Set):
            if not isinstance(other, Iterable):
                return NotImplemented
            other = self._from_iterable(other)
        return self._from_iterable(value for value in other
                                   if value not in self)
    def __xor__(self, other):
        if not isinstance(other, Set):
            if not isinstance(other, Iterable):
                return NotImplemented
            other = self._from_iterable(other)
        return (self - other) | (other - self)
    __rxor__ = __xor__


#===============================================================================
# _KeysViewBase / _ItemsViewBase / _ValuesViewBase

class _KeysViewBase(_MappingViewBase, _SetViewMixin):
    """Plain copy of ``collections.abc.KeysView``'s ``__contains__``/
    ``__iter__`` (its comparisons/set-algebra come from ``_SetViewMixin``, its
    ``__len__``/``__repr__``/``_mapping`` from ``_MappingViewBase``)."""
    __slots__ = ()
    def __contains__(self, key):
        return key in self._mapping
    def __iter__(self):
        yield from self._mapping
    def __reversed__(self):
        return reversed(list(self._mapping))


class _ItemsViewBase(_MappingViewBase, _SetViewMixin):
    """Plain copy of ``collections.abc.ItemsView``'s ``__contains__``/
    ``__iter__``."""
    __slots__ = ()
    def __contains__(self, item):
        key, value = item
        try:
            v = self._mapping[key]
        except KeyError:
            return False
        else:
            return v is value or v == value
    def __iter__(self):
        for key in self._mapping:
            yield (key, self._mapping[key])
    def __reversed__(self):
        return reversed(list(self))


class _ValuesViewBase(_MappingViewBase):
    """Plain copy of ``collections.abc.ValuesView``'s ``__contains__``/
    ``__iter__`` (``ValuesView`` is not set-like, so there is no set
    algebra)."""
    __slots__ = ()
    def __contains__(self, value):
        for key in self._mapping:
            v = self._mapping[key]
            if v is value or v == value:
                return True
        return False
    def __iter__(self):
        for key in self._mapping:
            yield self._mapping[key]
    def __reversed__(self):
        return reversed(list(self))


#===============================================================================
# Views of lazy mappings (used by the C backend's ldict and tldict)

import collections.abc as _cabc

class ldict_items(_cabc.ItemsView):
    """The items of a lazy mapping; reading a value computes it."""
    __slots__ = ()
    def __reversed__(self):
        return reversed(list(self))

class ldict_values(_cabc.ValuesView):
    """The values of a lazy mapping; reading a value computes it."""
    __slots__ = ()
    def __reversed__(self):
        return reversed(list(self))
