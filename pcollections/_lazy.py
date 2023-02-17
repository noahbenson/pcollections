# -*- coding: utf-8 -*-
################################################################################
# pcollections/_lazy.py
# The lazy dictionary and list implementations.
# By Noah C. Benson

from functools import partial
from threading import RLock

from phamt import PHAMT

from ._list import (
    plist,
    tlist
)
from ._dict import (
    pdict_items,
    pdict_values,
    pdict,
    tdict
)


#===============================================================================
# Lazy Value Type

class LazyError(RuntimeError):
    """A runtime error that occurs while evaluating a lazy value.

    See also: `lazy`.
    """
    __slots__ = 'partial'
    def __init__(self, partial):
        self.partial = partial
    def __str__(self):
        (fn, args, kwargs) = self.partial
        fnname = getattr(fn, '__name__', '<anonymous>')
        errmsg = ('lazy raised error during call to '
                  f'{fnname}{tuple(args)}')
        if len(kwargs) > 0:
            opts = ', '.join([f'{k}={v}' for (k,v) in kwargs.items()])
            errmsg = f'{errmsg[:-1]}, {opts})'
        return errmsg
    def __repr__(self):
        return str(self)
class lazy:
    """A callable like `partial` for lazily-computed values.

    The `lazy` type is constructed identically to the `partial` type. Unlike
    `partial`, however, `lazy` values must have their entire argument lists
    instantiated at the time of construction--i.e., it is not possible to call
    a lazy partial with additional arguments.

    `l = lazy(fn, *args, **kwargs)` stores the given callable `fn` with the
    given `args` and `kwargs` as arguments. When the lazy value of `l` is later
    requested (via `l()`), the value is cached, and the partial data is
    forgotten.
    """
    __slots__ = ('partial', 'value')
    def __new__(cls, fn, *args, **kw):
        # Make sure the function is callable.
        if not callable(fn):
            raise TypeError(f"lazy({fn}) must be given a callable function")
        # Allocate an object.
        obj = object.__new__(cls)
        # Create the partial object.
        part = partial(fn, *args, **kw)
        # We want to prepare an error to raise if something happens during the
        # calculation of the lazy value in the __call__ method.
        try:
            raise LazyError(part)
        except LazyError as e:
            error = e
        # Set the appropriate members.
        object.__setattr__(obj, 'partial', (part, RLock(), error))
        # We set value to an RLock for use in the calculation.
        object.__setattr__(obj, 'value', None)
        # That's all that is needed.
        return obj
    def __call__(self):
        part = self.partial
        if part is None:
            return self.value
        else:
            (part, rlock, init_error) = part
            # Acquire the rlock then re-check that the lazy hasn't already been
            # calculated (at which point self.partial will be None).
            rlock.acquire()
            try:
                if self.partial is None:
                    val = self.value
                else:
                    val = part()
                    # We've successfully calculated the value; set the members
                    # appropriately.
                    object.__setattr__(self, 'value', val)
                    object.__setattr__(self, 'partial', None)
                return val
            except Exception as partial_eror:
                # If an exception occurs, we want to raise an error that traces
                # back to the initialization of this object.
                raise init_error
            finally:
                rlock.release()
    def __repr__(self):
        s = 'cached' if self.is_cached() else 'waiting'
        return f"lazy(<{id(self)}>: {s})"
    def __str__(self):
        return f"lazy(<{id(self)}>)"
    def is_cached(self):
        """Returns `True` if the lazy value is cached and `False` otherwise.

        Returns
        -------
        boolean
            `True` if the given lazy object has cached its value and `False`
            otherwise.
        """
        return self.partial is None


#===============================================================================
# The Lazy List Type

class llist(plist):
    """A persistent lazy list type.

    The `llist` type is identical to the `plist` type, with the exception that
    any `lazy` value contained in an `llist` is dereferenced prior to being
    returned by item extraction from the list or iteration. Essentially, an
    `llist` containing a lazy value behaves equivalently to a `plist` whose
    values were all precalculated. Note that hashing an `llist` object results
    in all lazy values being calculated.

    The `llist.transient` method returns a normal `tlist`, but one the lazy
    values are not dereferenced, meaning that laziness is respected for
    edits. However, if one requests a lazy value from the transient list, the
    `lazy` object itself will be returned. In order to recreate a lazy list
    (instead of a persistent list) from the transient list, one must use
    `llist(t)` instead of `t.persistent()`; the latter call will return a
    `plist` containing `lazy` objects.
    """
    empty = None
    __slots__ = ()
    def __iter__(self):
        it = plist.__iter__(self)
        return map(lambda u: u() if isinstance(u, lazy) else u, it)
    def __getitem__(self, k):
        el = plist.__getitem__(self, k)
        return el() if isinstance(el, lazy) else el
    def is_lazy(self, index):
        """Determines if the given index is an uncached lazy value."""
        v = self[index]
        if isinstance(v, lazy):
            return not v.is_cached()
        else:
            return False
    def clear(self):
        return llist.empty
# Setup the llist.empty static member.
llist.empty = llist._new(PHAMT.empty, 0)


#===============================================================================
# The Lazy Dictionary Type

class ldict_items(pdict_items):
    __slots__ = ()
    def _from_kv(self, kv):
        (k,v) = kv
        if isinstance(v, lazy):
            return (k, v())
        else:
            return kv
class ldict_values(pdict_values):
    __slots__ = ()
    def _from_kv(self, kv):
        v = kv[1]
        if isinstance(v, lazy):
            v = v()
        return v
class ldict(pdict):
    """A persistent lazy dict type.

    The `ldict` type is identical to the `pdict` type, with the exception that
    any `lazy` value contained in an `ldict` is dereferenced prior to being
    returned by item extraction from the dict or iteration. Essentially, an
    `ldict` containing a lazy value behaves equivalently to a `pdict` whose
    values were all precalculated. Note that hashing an `ldict` object results
    in all lazy values being calculated.

    The `ldict.transient` method returns a normal `tdict`, but one the lazy
    values are not dereferenced, meaning that laziness is respected for
    edits. However, if one requests a lazy value from the transient dict, the
    `lazy` object itself will be returned. In order to recreate a lazy dict
    (instead of a persistent dict) from the transient dict, one must use
    `ldict(t)` instead of `t.persistent()`; the latter call will return a
    `pdict` containing `lazy` objects.
    """
    empty = None
    __slots__ = ()
    def __getitem__(self, key):
        v = pdict.__getitem__(self, key)
        if isinstance(v, lazy):
            v = v()
        return v
    def get(self, key, default=None):
        v = pdict.get(self, key, default)
        if isinstance(v, lazy):
            v = v()
        return v
    def items(self):
        return ldict_items(self)
    def values(self):
        return ldict_values(self)
    def is_lazy(self, index):
        """Determines if the given index is an uncached lazy value."""
        v = self[index]
        if isinstance(v, lazy):
            return not v.is_cached()
        else:
            return False
    def clear(self):
        return ldict.empty
# Make the empty pdict.
ldict.empty = ldict._new(PHAMT.empty, PHAMT.empty, 0)
