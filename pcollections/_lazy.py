# -*- coding: utf-8 -*-
################################################################################
# pcollections/_lazy.py
# The lazy dictionary and list implementations.
# By Noah C. Benson

from functools import partial
from threading import RLock

from phamt import (PHAMT, THAMT)

from .util import seqstr
from ._list import (
    plist,
    tlist
)
from ._dict import (
    pdict_items,
    pdict_values,
    pdict,
    tdict,
    tdict_items,
    tdict_values
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
        s = 'ready' if self.is_ready() else 'waiting'
        return f"lazy(<{id(self)}>: {s})"
    def __str__(self):
        return f"lazy(<{id(self)}>)"
    def is_ready(self):
        """Returns `True` if the lazy value is cached and `False` otherwise.

        Returns
        -------
        boolean
            `True` if the given lazy object has cached its value and `False`
            otherwise.
        """
        return self.partial is None
def unlazy(obj):
    """Returns the cached value of a lazy object or the object if not lazy."""
    if isinstance(obj, lazy):
       return obj()
    else:
       return obj
def reprlazy(obj):
    """Returns `'<lazy>'` if `obj` is a `lazy` object, otherwise `repr(obj)`."""
    if isinstance(obj, lazy):
        return "<lazy>"
    else:
        return repr(obj)
def strlazy(obj):
    """Returns `'<lazy>'` if `obj` is a `lazy` object, otherwise `str(obj)`."""
    if isinstance(obj, lazy):
        return "<lazy>"
    else:
        return str(obj)


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
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"[|{seqstr(self.to_plist(), maxlen=60, tostr=reprlazy)}|]"
    def __repr__(self):
        return f"[|{seqstr(self.to_plist())}|]"
    def is_lazy(self, index):
        """Determines if the given key is mapped to a `lazy` value.

        `is_lazy` determines whether the associated key is mapped to a `lazy`
        object, but it does not determine if the lazy value is cached. To query
        whether a key is mapped to a value that is uncached, see the `is_ready`
        method.
        """
        v = plist.__getitem__(self, index)
        return isinstance(v, lazy)
    def is_ready(self, index):
        """Determines if the given key's value can be immediately returned.

        `is_ready` determines whether the associated key is either mapped to a
        non-`lazy` value or is mapped to a `lazy` value that is cached.
        """
        v = plist.__getitem__(self, index)
        if isinstance(v, lazy):
            return v.is_ready()
        else:
            return True
    def ready_all(self):
        "Caches all lazy items then returns the list."
        for el in self._phamt:
            if isinstance(el, lazy):
                el()
        return self
    def to_plist(self):
        """Returns a `plist`  of the lazy list with `lazy` values uncached.

        Whereas `plist(l)` will return a `plist` whose values are all the cached
        values of the `llist` `l`, the method `l.to_plist()` returns a copy of
        `l` where indices of `l` that lazily compute their values are mapped to
        their associated `lazy` objects. This is essentially a way to expose the
        raw values of a lazy list.
        """
        return plist._new(self._phamt, self._start)
    def getlazy(self, index):
        """Like getitem, but returns lazy objects instead of their results.

        For an `llist` variable `l`, `l.getlazy(ii)` is equivalent to `l[ii]`
        except that if the index `ii` maps to a lazy value, then `l.getlazy`
        will return the lazy object rather than evaluating it and returning the
        reified value.
        """
        return plist.__getitem__(self, index)
    def clear(self):
        return llist.empty
    def transient(self):
        return tllist._new(THAMT(self._phamt), self._start, self)
# Setup the llist.empty static member.
llist.empty = llist._new(PHAMT.empty, 0)

# The Transient Lazy List Type -------------------------------------------------
class tllist(tlist):
    """A transient lazy list type.

    Transient lazy lists are like transient lists (`tlist`) with the only
    difference being that they automatically return the reified values of lazy
    elements instead of the `lazy` objects themselves.
    """
    __slots__ = ()
    def __iter__(self):
        return map(unlazy, tlist.__iter__(self))
    def persistent(self):
        """Efficiently copies the tllist into an llist and returns the llist."""
        if len(self._thamt) == 0:
            return llist.empty
        elif self._orig is None:
            return llist._new(self._thamt.persistent(), self._start)
        else:
            return self._orig
    def __getitem__(self, k):
        return unlazy(tlist.__getitem__(self, k))
    def getlazy(self, k):
        """Returns an element of the list without dereferecing lazy elements."""
        return tlist.__getitem__(self, k)
        


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
    def __str__(self):
        # We have a max length of 60 characters, not counting the delimiters.
        return f"{{|{seqstr(self.to_pdict(), maxlen=60, tostr=reprlazy)}|}}"
    def __repr__(self):
        return f"{{|{seqstr(self.to_pdict())}|}}"
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
        """Determines if the given key is mapped to a `lazy` value.

        `is_lazy` determines whether the associated key is mapped to a `lazy`
        object, but it does not determine if the lazy value is cached. To query
        whether a key is mapped to a value that is uncached, see the `is_ready`
        method.
        """
        v = pdict.__getitem__(self, index)
        return isinstance(v, lazy)
    def is_ready(self, index):
        """Determines if the given key's value can be immediately returned.

        `is_ready` determines whether the associated key is either mapped to a
        non-`lazy` value or is mapped to a `lazy` value that is cached.
        """
        v = pdict.__getitem__(self, index)
        if isinstance(v, lazy):
            return v.is_ready()
        else:
            return True
    def ready_all(self):
        "Caches all lazy items then returns the dictionary."
        for arg in self._els:
            (k,v) = arg[1][0]
            if isinstance(v, lazy):
                v()
        return self
    def to_pdict(self):
        """Returns a `pdict`  of the lazy dict with `lazy` values uncached.

        Whereas `pdict(ld)` will return a `pdict` whose values are all the
        cached values of the `ldict` `ld`, the method `ld.to_pdict()` returns a
        copy of `ld` where keys of `ld` that lazily compute their values are
        mapped to their associated `lazy` objects. This is essentially a way to
        expose the raw values of a lazy dictionary.
        """
        return pdict._new(self._els, self._idx, self._top)
    def getlazy(self, key, default=None):
        """Like get, but returns lazy objects instead of their results.

        For an `ldict` variable `d`, `d.getlazy(k, default)` is equivalent to
        `d.get(k, default)` except that if the key `k` maps to a lazy value,
        then `d.getlazy` will return the lazy object rather than evaluating it
        and returning the reified value.
        """
        return pdict.get(self, key, default)
    def clear(self):
        return ldict.empty
    def transient(self):
        return tldict._new(THAMT(self._els), THAMT(self._idx), self._top, self)
# Make the empty pdict.
ldict.empty = ldict._new(PHAMT.empty, PHAMT.empty, 0)

# The Transient Lazy Dictionary Type -------------------------------------------
class tldict_items(tdict_items):
    __slots__ = ()
    def _from_kv(self, kv):
        if isinstance(kv[1], lazy):
            return (kv[0], kv[1]())
        else:
            return kv
class tldict_values(tdict_values):
    __slots__ = ()
    def _from_kv(self, arg):
        return unlazy(arg[1])
    def __contains__(self, v):
        for (kv,_) in self._els:
            if not isinstance(kv[1], lazy):
                if kv[1] == v:
                    return True
        for (kv,_) in self._els:
            if isinstance(kv[1], lazy):
                if kv[1]() == v:
                    return True
        return False
class tldict(tdict):
    """A transient lazy dict type.

    Transient lazy dicts are like transient dicts (`tdict`) with the only
    difference being that they automatically return the reified values of lazy
    keys instead of the `lazy` objects themselves.
    """
    __slots__ = ()
    def persistent(self):
        """Efficiently copies the tllist into an llist and returns the llist."""
        if len(self._thamt) == 0:
            return ldict.empty
        elif self._orig is None:
            return ldict._new(self._thamt.persistent(), self._start)
        else:
            return self._orig
    def __getitem__(self, k):
        return unlazy(tdict.__getitem__(self, k))
    def get(self, k, default=None):
        return unlazy(tdict.get(self, k, default))
    def pop(self, *args):
        return unlazy(self.pop(*args))
    def items(self):
        return tldict_items(self)
    def values(self):
        return tldict_values(self)
