# -*- coding: utf-8 -*-
################################################################################
# pcollections/abc/_view.py
# Plain (non-ABCMeta) mixins backing pcollections._c._core's six view types
# (pdict_keys/pdict_items/pdict_values/tdict_keys/tdict_items/tdict_values).
# By Noah C. Benson

"""Plain mixins for the ``dict``-view family (``keys()``/``items()``/
``values()``).

``pcollections._c._core``'s six view heap types used to inherit directly from
``collections.abc.KeysView``/``ItemsView``/``ValuesView`` (which is exactly
what the pure-Python ``pcollections._dict``'s own ``pdict_keys``/``pdict_items``/
``pdict_values`` still do -- see that module -- since ordinary Python class
statements are unaffected by the issue described next).

CPython 3.14 tightened ``PyType_FromSpecWithBases``/``PyType_FromMetaclass``
(the C API ``_c/dict.c.h``'s ``build_view_type()`` uses to build these six types
as heap types at import time) to unconditionally reject a base whose metaclass
overrides ``tp_new`` -- which ``ABCMeta`` does, and ``KeysView``/``ItemsView``/
``ValuesView`` (via ``MappingView``/``Set``/``Collection``/...) are all
``ABCMeta``-based -- so building these view types directly on top of them is no
longer possible from C on 3.14. See ``pcollections.abc._core``'s
``_PersistentBase`` docstring for the fuller CPython 3.14 story; this module
is the dict-view-family analogue of that fix.

``_MappingViewBase``/``_KeysViewBase``/``_ItemsViewBase``/``_ValuesViewBase``
below are faithful, verbatim ports of ``collections.abc``'s own
``MappingView``/``KeysView``/``ItemsView``/``ValuesView`` concrete method
bodies (and, for the ``Set``-derived Keys/Items views, of ``collections.abc.Set``
itself), with no ``ABCMeta`` anywhere in their inheritance chain.
``_c/dict.c.h`` builds its six view heap types on top of these instead, then
calls ``.register()`` on the real ``collections.abc.KeysView``/``ItemsView``/
``ValuesView`` (see ``_c/dict.c.h``'s ``pcoll_exec_dict()``) so that
``isinstance``/``issubclass`` checks against those stdlib ABCs -- and,
transitively, against ``collections.abc.Set``/``Collection``/``Iterable``/
``Container``/``Sized`` for the Keys/Items views -- keep working exactly as
before, without the C API ever being asked to build a heap type on top of an
``ABCMeta`` base.

These mixins aren't used by pcollections.abc's own public classes (there
never were public ``pcollections.abc.KeysView``-style classes -- the pure
Python backend uses the real stdlib ABCs directly, and still does), so unlike
``_core.py``/``_map.py``/``_set.py``/``_seq.py``'s "base" mixins, there's no
public counterpart here to keep behaviorally identical; these exist solely
for ``_c/dict.c.h`` to build on.
"""

from collections.abc import (Set, Iterable)


#===============================================================================
# _MappingViewBase

class _MappingViewBase:
    """Plain port of ``collections.abc.MappingView``'s concrete methods.

    Declares the ``_mapping`` slot itself (matching
    ``MappingView.__slots__ = ('_mapping',)``) so that a heap type built
    directly on this class gets exactly the extra storage its C ``dictview_new``
    (``_c/dict.c.h``) needs -- see ``_c/dict.c.h``'s ``build_view_type()``, which
    reads this class's (dynamically, via ``tp_basicsize``) computed size.
    """
    __slots__ = ('_mapping',)
    def __len__(self):
        return len(self._mapping)
    def __repr__(self):
        return '{0.__class__.__name__}({0._mapping!r})'.format(self)


#===============================================================================
# _SetViewMixin

class _SetViewMixin:
    """Plain port of ``collections.abc.Set``'s concrete methods, for the two
    (``Keys``/``Items``) of the three dict-view mixins that are also
    set-like. Ported verbatim from ``collections.abc.Set``, with one
    intentional simplification: ``_from_iterable`` always returns a plain
    ``set`` (matching ``KeysView``/``ItemsView``'s own ``_from_iterable``
    override in the stdlib -- ``Set``'s own default, ``cls(it)``, isn't
    applicable here since these view types' constructors take a mapping, not
    an arbitrary iterable).
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
    """Plain port of ``collections.abc.KeysView``'s own ``__contains__``/
    ``__iter__`` (its comparisons/set-algebra come from ``_SetViewMixin``, its
    ``__len__``/``__repr__``/``_mapping`` from ``_MappingViewBase``)."""
    __slots__ = ()
    def __contains__(self, key):
        return key in self._mapping
    def __iter__(self):
        yield from self._mapping


class _ItemsViewBase(_MappingViewBase, _SetViewMixin):
    """Plain port of ``collections.abc.ItemsView``'s own ``__contains__``/
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


class _ValuesViewBase(_MappingViewBase):
    """Plain port of ``collections.abc.ValuesView``'s own ``__contains__``/
    ``__iter__`` (``ValuesView`` is ``Collection``-based, not ``Set``-based --
    no set algebra to port)."""
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
