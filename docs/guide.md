# User guide

## Persistent and transient collections

A persistent collection can't be changed. Methods that would change a
`list`, `set`, or `dict` instead return a new collection, and the original
is unchanged:

```python
>>> from pcollections import plist, pset, pdict
>>> p = plist([1, 2, 3])
>>> p.append(4)
[|1, 2, 3, 4|]
>>> p
[|1, 2, 3|]
>>> p.pop()                   # the value and the new list
(3, [|1, 2|])
>>> pset({1, 2}).add(3)
{|1, 2, 3|}
>>> pdict(a=1).set('b', 2)
{|'a': 1, 'b': 2|}
```

The new collection shares most of its structure with the original, so these
operations are fast (they take time logarithmic in the size of the
collection) and use little memory. Persistent collections are hashable when
their elements are, and can be shared freely between threads.

Each persistent type has a transient partner, which is a mutable collection
with the interface of the corresponding builtin. `transient()` and
`persistent()` convert between the two in constant time, so a transient is
the efficient way to make many changes at once:

```python
>>> t = p.transient()
>>> t.append(4)
>>> t[0] = 0
>>> t
[<0, 2, 3, 4>]
>>> t.persistent()
[|0, 2, 3, 4|]
```

| Builtin | Persistent | Transient | Lazy persistent | Lazy transient |
|---------|------------|-----------|-----------------|----------------|
| `list`  | `plist`    | `tlist`   | `llist`         | `tllist`       |
| `set`   | `pset`     | `tset`    |                 |                |
| `dict`  | `pdict`    | `tdict`   | `ldict`         | `tldict`       |

Persistent collections print with `|` inside their brackets (`[|1, 2|]`),
and transient collections with `<` and `>` (`[<1, 2>]`).

`pdict` and `pset` remember the order in which their elements were added, as
`dict` does.

The persistent types have these methods, among others (see the
[API reference](api.md)):

- `plist`: `set(index, value)`, `append`, `prepend`, `extend`, `insert`,
  `delete(index)`, `drop(index, error=False)`, `remove(value)`,
  `pop(index)`, `sort`, `reverse`.
- `pset`: `add`, `addall`, `discard`, `discardall`, `remove`, `removeall`,
  `drop(value, error=False)`, `pop()`, and the set operations.
- `pdict`: `set(key, value)`, `setall(keys, values)`, `update`,
  `setdefault`, `delete(key)`, `deleteall(keys)`, `drop(key, error=False)`,
  `dropall(keys)`, `pop(key)`, `popitem()`, and `|`.

`delete` raises an error when the key (or index) isn't present, and `drop`
returns the collection unchanged unless `error` is true.

## Lazy values

A `lazy` object holds a function and its arguments, and computes the
function's value the first time it is requested:

```python
>>> from pcollections import lazy, ldict, llist
>>> x = lazy(sum, range(10))
>>> x.is_ready()
False
>>> x()
45
```

The lazy collections `llist`, `tllist`, `ldict`, and `tldict` compute their
lazy elements (or values) when they are read. This makes it easy to build
collections, including nested ones, whose values are the results of
expensive computations that run only if they are needed:

```python
>>> d = ldict(small=1, big=lazy(sum, range(10**6)))
>>> d['small']                # the sum hasn't been computed
1
>>> d['big']                  # now it has
499999500000
```

The rules:

- A `lazy` is an ordinary object in any collection other than the lazy
  collections: `pdict(x=lazy(f))['x']` is the `lazy` itself.
- Every read from a lazy collection computes the lazy values it returns. That
  includes indexing, `get`, `values()`, `items()`, iteration, `pop`,
  comparison, hashing, pickling, and converting to another collection:
  `pdict(d)` computes the values of the `ldict` `d`. `str` and `repr` show
  `<lazy>` without computing anything.
- The lazy constructors (`ldict(x)`, `llist(x)`, `tldict(x)`, `tllist(x)`)
  keep lazy values uncomputed. To get the `lazy` objects themselves out of a
  lazy collection, use `getlazy`, `holdlazy(coll)`, or the `held_pdict`,
  `held_tdict`, `held_plist`, and `held_tlist` methods.
- `is_lazy(key)` says whether a value is a `lazy`, `is_ready(key)` whether it
  has been computed, and `ready_all()` computes every value.
- A value is computed at most once, even when several threads request it at
  the same time.
- `lazy` can be subclassed. A subclass may override `__call__` and call
  `super().__call__()`; the lazy collections and `unlazy` call the override.
- Pickling a `lazy` computes it and pickles its value.

### Errors

If a computation raises an exception, the failure is remembered: every
request for the value raises a new `LazyError` whose `__cause__` is the
original exception. The message says where the `lazy` was created, and the
error's `func`, `func_args`, `func_kwargs`, and `origin` attributes describe
the computation. The error is a new `LazyError` rather than the original
exception so that a failed computation can't be mistaken for the
collection's own errors: a `KeyError` raised by a computation, for example,
would otherwise make `ld.get(key, default)` return `default`.

```python
>>> from pcollections import LazyError, lazy_error_unwrap
>>> d = ldict(x=lazy(lambda: 1 / 0))
>>> try:
...     d['x']
... except LazyError as e:
...     print(type(e.__cause__).__name__)
ZeroDivisionError
>>> try:
...     with lazy_error_unwrap:     # raise the original exception instead
...         d['x']
... except ZeroDivisionError:
...     print('unwrapped')
unwrapped
```

A lazy value can fail because a lazy value it depends on failed. Its
`LazyError` then names that dependency, and its cause is the dependency's
`LazyError`, so the chain of causes (which a traceback shows) follows the
dependencies down to the exception that started the failure. That exception
is the error's `root_cause`, and it is what `lazy_error_unwrap` raises:

```python
>>> base = ldict(v=lazy(lambda: 1 / 0))
>>> derived = ldict(w=lazy(lambda: base['v'] + 1))
>>> try:
...     derived['w']
... except LazyError as e:
...     print(type(e.cause).__name__, type(e.root_cause).__name__)
LazyError ZeroDivisionError
>>> try:
...     with lazy_error_unwrap:
...         derived['w']
... except ZeroDivisionError:
...     print('unwrapped')
unwrapped
```

`lazy_error_unwrap(e)` also works as a function, returning the root cause of
the `LazyError` `e`. The lazy value doesn't run its computation again, so a
failure that might not happen on a second try (a network error, say) needs a
new `lazy` object.

Set `lazy.trace = True`, or the environment variable
`PCOLLECTIONS_LAZY_TRACE=1`, to also record the full stack at each `lazy`'s
creation (in `origin_stack`).

A lazy value whose computation requests its own value raises `LazyError`.
Two threads that each compute a lazy value needed by the other deadlock.
That can't happen when lazy values are built from immutable data, because a
lazy value can then only depend on lazy values that existed before it.

## Threads

The persistent types can be shared freely between threads, including on
free-threaded ("no-GIL") builds of Python, which `pcollections` supports.

The transient types are meant to be used by one thread at a time, like the
builtin `list`, `set`, and `dict`. They are not corrupted if that rule is
broken: a modification that overlaps another modification of the same transient
(from another thread, or from code such as a key's `__eq__` that runs during
the modification) raises `RuntimeError`. So does a read (a lookup, or a step of
an iteration) that is made while a modification is in progress, or whose
transient changes during a key comparison. As with `dict` and `set`, changing
the keys of a `tdict` or `tset` while iterating over it raises `RuntimeError`;
iterating over a `tlist` while changing it behaves like iterating over a
`list`.

## Subclassing

All of the types can be subclassed, and methods that return a new collection
return an instance of the subclass. A persistent class names its transient
partner in the class attribute `__transient_type__` (used by `transient()`),
and a transient class names its persistent partner in `__persistent_type__`
(used by `persistent()`):

```python
from pcollections import pdict, tdict

class MyDict(pdict):
    __slots__ = ()

class MyTDict(tdict):
    __slots__ = ()

MyDict.__transient_type__ = MyTDict
MyTDict.__persistent_type__ = MyDict
```

A subclass that doesn't set these attributes uses the partner of its base
class. Instances of persistent subclasses are immutable too: their attributes
can't be set.

## Type hints

The types are generic (`plist[int]`, `pdict[str, float]`), and
`pcollections` includes type stubs. The lazy collections are typed by their
computed values: an `ldict[str, int]` may hold `lazy` objects that compute
`int` values.

## Backends

`pcollections` is implemented in C, with a pure-Python fallback that has the
same interface. If the C extension can't be loaded, `pcollections` uses the
(much slower) Python implementation and issues a `RuntimeWarning`;
`pcollections.using_c_extension` says which implementation is in use. See
[Installation and backends](install.md) for how to check and choose the
implementation, and for how the implementations behave in subinterpreters.
