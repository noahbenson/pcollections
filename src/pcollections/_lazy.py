# -*- coding: utf-8 -*-
################################################################################
# pcollections/_lazy.py
# The lazy dictionary and list implementations.
# By Noah C. Benson

from sys import _getframe
from threading import Lock, get_ident

from . import _lazybase
from ._lazybase import LazyError, lazy_error_unwrap

# llist/tllist reuse plist/tlist's FAT-backed encoding (self._phamt), and
# tldict wraps tdict's FAT/AMT-typed self._els/self._idx. As in _list.py,
# FAT/TFAT are also used under the names PHAMT/THAMT.
from ._trie import (
    AMT,
    TAMT,
    FAT,
    TFAT,
    FAT as PHAMT,
    TFAT as THAMT
)

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

_PENDING, _RUNNING, _READY, _FAILED = range(4)

class lazy:
    """A value computed on first request, like a `partial` with no free
    arguments.

    `l = lazy(fn, *args, **kwargs)` stores the callable `fn` and its
    arguments. The first call `l()` computes `fn(*args, **kwargs)`, caches
    the result, and releases `fn` and its arguments; later calls return the
    cached result. The computation runs at most once, even when several
    threads request the value at the same time: the others wait for it.

    If the computation raises an `Exception`, the lazy value fails: this and
    every later call raise a new `LazyError` whose `__cause__` is the
    original exception, without running the computation again. A
    computation that requests its own value raises `LazyError`. Any other
    exception (such as `KeyboardInterrupt`) propagates unchanged and leaves
    the value uncomputed.

    Two threads that each compute a lazy value needed by the other's
    computation deadlock. This cannot happen when lazy values are built from
    immutable data, since a lazy value can then only depend on lazy values
    that existed before it.

    A `lazy` records where it was created, for its error messages. When the
    class attribute `lazy.trace` is true (initially, if the
    PCOLLECTIONS_LAZY_TRACE environment variable is set), it also records the
    full stack.

    Pickling a `lazy` computes it and pickles the result.

    `lazy` may be subclassed; a subclass may override `__call__`, calling
    `super().__call__()` to compute the value.
    """
    __slots__ = ('_func', '_args', '_kwargs', '_value', '_state', '_owner',
                 '_lock', '_error', '_error_tb', '_origin_code',
                 '_origin_line', '_origin_stack', '__weakref__')
    trace = _lazybase.TRACE_DEFAULT
    def __new__(cls, *args, **kwargs):
        if not args:
            raise TypeError("lazy() missing required positional argument: 'fn'")
        fn = args[0]
        if not callable(fn):
            raise TypeError(f"lazy({fn!r}) must be given a callable function")
        self = object.__new__(cls)
        self._func = fn
        self._args = args[1:]
        self._kwargs = kwargs
        self._value = None
        self._state = _PENDING
        self._owner = None
        self._lock = Lock()
        self._error = None
        self._error_tb = None
        frame = _getframe(1)
        self._origin_code = frame.f_code
        self._origin_line = frame.f_lineno
        self._origin_stack = (_lazybase.capture_stack(frame) if lazy.trace
                              else None)
        return self
    @classmethod
    def _from_value(cls, value):
        self = object.__new__(cls)
        self._func = self._args = self._kwargs = None
        self._value = value
        self._state = _READY
        self._owner = None
        self._lock = Lock()
        self._error = self._error_tb = None
        self._origin_code = self._origin_stack = None
        self._origin_line = 0
        return self
    def _make_error(self, kind):
        return _lazybase.make_error(
            kind, self._func, self._args, self._kwargs, self._origin_code,
            self._origin_line, self._origin_stack, self._error,
            self._error_tb)
    def __call__(self):
        state = self._state
        if state == _READY:
            return self._value
        if state == _FAILED:
            raise self._make_error('failed')
        me = get_ident()
        if state == _RUNNING and self._owner == me:
            raise self._make_error('recursive')
        with self._lock:
            state = self._state
            if state == _READY:
                return self._value
            if state == _PENDING:
                self._owner = me
                self._state = _RUNNING
                try:
                    value = self._func(*self._args, **self._kwargs)
                except Exception as e:
                    self._error = e
                    self._error_tb = e.__traceback__
                    self._state = _FAILED
                    self._owner = None
                except BaseException:
                    self._owner = None
                    self._state = _PENDING
                    raise
                else:
                    self._value = value
                    self._func = self._args = self._kwargs = None
                    self._origin_code = self._origin_stack = None
                    self._state = _READY
                    self._owner = None
                    return value
        # The value failed; raised outside the except clause so that the
        # error's only link to the failure is its __cause__.
        raise self._make_error('failed')
    def _state_name(self):
        state = self._state
        return ('ready' if state == _READY else
                'failed' if state == _FAILED else 'waiting')
    def __repr__(self):
        return f"lazy(<{id(self)}>: {self._state_name()})"
    def __str__(self):
        return f"lazy(<{id(self)}>)"
    def __reduce__(self):
        state = getattr(self, '__dict__', None) or None
        return (_lazybase.ready_lazy, (type(self), self()), state)
    def is_ready(self):
        """Returns `True` if the value has been computed, otherwise `False`."""
        return self._state == _READY
def unlazy(obj):
    """Returns the value of `obj` if it is a `lazy`, otherwise `obj`."""
    return obj() if isinstance(obj, lazy) else obj
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
def holdlazy(obj, require_lazy=False):
    """Returns a lazy collection as a plain collection that holds its `lazy`
    values uncomputed.

    Reading from a lazy collection (`ldict`, `llist`, `tldict`, or
    `tllist`), including converting it to another collection, computes its
    lazy values. `holdlazy(coll)` instead returns the equivalent plain
    collection (`pdict`, `plist`, `tdict`, or `tlist`), whose values are the
    `lazy` objects themselves. It never shares mutable storage with `coll`.

    `holdlazy(obj)` calls `obj.__holdlazy__()` if `obj`'s type defines that
    method. Otherwise, it returns `obj` unchanged, or, if `require_lazy` is
    true, raises `TypeError`.
    """
    method = getattr(type(obj), '__holdlazy__', None)
    if method is not None:
        return method(obj)
    if require_lazy:
        raise TypeError(f"__holdlazy__ method not found for type {type(obj)}")
    return obj


#===============================================================================
# The lazy collections
#
# The lazy collections follow one rule: every read from a lazy collection
# computes the lazy values it returns. Reads include indexing, `get`,
# iteration over values and items, `values()`, `items()`, `pop`, equality,
# hashing, and conversion to another collection. `repr` and `str` show
# lazy values without computing them. The raw `lazy` objects are available
# through `getlazy`, `holdlazy`, and the `held_*` methods.
#
# Lazy values stored in any other collection are ordinary objects.

def _seqstr_lazy(coll, maxlen=None):
    if maxlen is None:
        return seqstr(coll, tostr=reprlazy)
    return seqstr(coll, maxlen=maxlen, tostr=reprlazy)

class llist(plist):
    """A persistent list whose `lazy` elements are computed when read.

    `llist` is a `plist` whose reads (indexing, iteration, comparison,
    hashing, and conversion to other collections) compute the `lazy`
    elements they return. `llist(x)` keeps the lazy elements of `x`
    uncomputed. `getlazy(i)` and `held_plist()` return the raw `lazy`
    objects.
    """
    empty = None
    __slots__ = ()
    _holds_lazy = True
    def __new__(cls, *args, **kw):
        return plist.__new__(cls, *map(holdlazy, args), **kw)
    def __iter__(self):
        return map(unlazy, plist.__iter__(self))
    def __getitem__(self, k):
        return unlazy(plist.__getitem__(self, k))
    def __str__(self):
        return f"[|{_seqstr_lazy(self.held_plist(), 60)}|]"
    def __repr__(self):
        return f"[|{_seqstr_lazy(self.held_plist())}|]"
    def __reduce__(self):
        return (type(self), (list(self),))
    def is_lazy(self, index):
        """Returns `True` if the element at `index` is a `lazy` object."""
        return isinstance(plist.__getitem__(self, index), lazy)
    def is_ready(self, index):
        """Returns `True` if the element at `index` is not a `lazy` object or
        is a `lazy` object whose value has been computed."""
        v = plist.__getitem__(self, index)
        return v.is_ready() if isinstance(v, lazy) else True
    def ready_all(self):
        """Computes all lazy elements, then returns the list."""
        for el in plist.__iter__(self):
            if isinstance(el, lazy):
                el()
        return self
    def held_plist(self):
        """Returns a `plist` of the list's elements with `lazy` elements left
        uncomputed."""
        return plist._new(self._phamt, self._start)
    __holdlazy__ = held_plist
    def getlazy(self, index):
        """Like `self[index]`, but returns a `lazy` element itself rather than
        its value."""
        return plist.__getitem__(self, index)
llist.empty = llist._new(PHAMT.empty, 0)

class tllist(tlist):
    """A transient list whose `lazy` elements are computed when read.

    `tllist` is to `llist` as `tlist` is to `plist`.
    """
    __slots__ = ()
    _holds_lazy = True
    def __new__(cls, *args, **kw):
        return tlist.__new__(cls, *map(holdlazy, args), **kw)
    def __iter__(self):
        return map(unlazy, tlist.__iter__(self))
    def __getitem__(self, k):
        return unlazy(tlist.__getitem__(self, k))
    def __str__(self):
        return f"[<{_seqstr_lazy(self.held_tlist(), 60)}>]"
    def __repr__(self):
        return f"[<{_seqstr_lazy(self.held_tlist())}>]"
    def __reduce__(self):
        return (type(self), (list(self),))
    def pop(self, index=-1):
        return unlazy(tlist.pop(self, index))
    def is_lazy(self, index):
        """Returns `True` if the element at `index` is a `lazy` object."""
        return isinstance(tlist.__getitem__(self, index), lazy)
    def is_ready(self, index):
        """Returns `True` if the element at `index` is not a `lazy` object or
        is a `lazy` object whose value has been computed."""
        v = tlist.__getitem__(self, index)
        return v.is_ready() if isinstance(v, lazy) else True
    def ready_all(self):
        """Computes all lazy elements, then returns the list."""
        for el in list(tlist.__iter__(self)):
            if isinstance(el, lazy):
                el()
        return self
    def held_tlist(self):
        """Returns a `tlist` of the list's elements with `lazy` elements left
        uncomputed. The two lists do not share mutable storage."""
        (th, start, _) = self._snapshot()
        return tlist._new(THAMT(th), start)
    __holdlazy__ = held_tlist
    def getlazy(self, index):
        """Like `self[index]`, but returns a `lazy` element itself rather than
        its value."""
        return tlist.__getitem__(self, index)

class ldict_items(pdict_items):
    __slots__ = ()
    def _from_kv(self, kv):
        v = kv[1]
        return (kv[0], v()) if isinstance(v, lazy) else kv
class ldict_values(pdict_values):
    __slots__ = ()
    def _from_kv(self, kv):
        return unlazy(kv[1])
class ldict(pdict):
    """A persistent dict whose `lazy` values are computed when read.

    `ldict` is a `pdict` whose reads (indexing, `get`, `values()`,
    `items()`, comparison, hashing, and conversion to other collections)
    compute the `lazy` values they return. `ldict(x)` keeps the lazy values
    of `x` uncomputed. `getlazy(k)` and `held_pdict()` return the raw `lazy`
    objects.
    """
    empty = None
    __slots__ = ()
    _holds_lazy = True
    def __new__(cls, *args, **kw):
        return pdict.__new__(cls, *map(holdlazy, args), **kw)
    def __getitem__(self, key):
        return unlazy(pdict.__getitem__(self, key))
    def __str__(self):
        return f"{{|{_seqstr_lazy(self.held_pdict(), 60)}|}}"
    def __repr__(self):
        return f"{{|{_seqstr_lazy(self.held_pdict())}|}}"
    def get(self, key, default=None):
        return unlazy(pdict.get(self, key, default))
    def items(self):
        return ldict_items(self)
    def values(self):
        return ldict_values(self)
    def is_lazy(self, key):
        """Returns `True` if `key` is mapped to a `lazy` object."""
        return isinstance(pdict.__getitem__(self, key), lazy)
    def is_ready(self, key):
        """Returns `True` if `key` is mapped to a value that is not a `lazy`
        object or is a `lazy` object whose value has been computed."""
        v = pdict.__getitem__(self, key)
        return v.is_ready() if isinstance(v, lazy) else True
    def ready_all(self):
        """Computes all lazy values, then returns the dict."""
        for v in pdict_values(self):
            if isinstance(v, lazy):
                v()
        return self
    def held_pdict(self):
        """Returns a `pdict` of the dict's items with `lazy` values left
        uncomputed."""
        return pdict._new(self._els, self._idx, self._top, self._count,
                          self._ndeleted)
    __holdlazy__ = held_pdict
    def getlazy(self, key, default=None):
        """Like `get`, but returns a `lazy` value itself rather than its
        value."""
        return pdict.get(self, key, default)
ldict.empty = ldict._new(FAT.empty, AMT.empty, 0, 0, 0)

class tldict_items(tdict_items):
    __slots__ = ()
    def _from_kv(self, kv):
        v = kv[1]
        return (kv[0], v()) if isinstance(v, lazy) else kv
class tldict_values(tdict_values):
    __slots__ = ()
    def _from_kv(self, kv):
        return unlazy(kv[1])
class tldict(tdict):
    """A transient dict whose `lazy` values are computed when read.

    `tldict` is to `ldict` as `tdict` is to `pdict`.
    """
    __slots__ = ()
    _holds_lazy = True
    def __new__(cls, *args, **kw):
        return tdict.__new__(cls, *map(holdlazy, args), **kw)
    def __str__(self):
        return f"{{<{_seqstr_lazy(self.held_tdict(), 60)}>}}"
    def __repr__(self):
        return f"{{<{_seqstr_lazy(self.held_tdict())}>}}"
    def is_lazy(self, key):
        """Returns `True` if `key` is mapped to a `lazy` object."""
        return isinstance(tdict.__getitem__(self, key), lazy)
    def is_ready(self, key):
        """Returns `True` if `key` is mapped to a value that is not a `lazy`
        object or is a `lazy` object whose value has been computed."""
        v = tdict.__getitem__(self, key)
        return v.is_ready() if isinstance(v, lazy) else True
    def ready_all(self):
        """Computes all lazy values, then returns the dict."""
        for v in list(tdict_values(self)):
            if isinstance(v, lazy):
                v()
        return self
    def held_tdict(self):
        """Returns a `tdict` of the dict's items with `lazy` values left
        uncomputed. The two dicts do not share mutable storage."""
        (els, idx, top, count, ndeleted, _) = self._snapshot()
        return tdict._new(TFAT(els), TAMT(idx), top, count, ndeleted)
    __holdlazy__ = held_tdict
    def getlazy(self, key, default=None):
        """Like `get`, but returns a `lazy` value itself rather than its
        value."""
        return tdict.get(self, key, default)
    def __getitem__(self, key):
        return unlazy(tdict.__getitem__(self, key))
    def get(self, key, default=None):
        return unlazy(tdict.get(self, key, default))
    def pop(self, *args):
        return unlazy(tdict.pop(self, *args))
    def items(self):
        return tldict_items(self)
    def values(self):
        return tldict_values(self)


llist.__transient_type__ = tllist
tllist.__persistent_type__ = llist
ldict.__transient_type__ = tldict
tldict.__persistent_type__ = ldict
