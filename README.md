# pcollections

A persistent collections library for Python.


## About

`pcollections` is a library of persistent (immutable) collections inspired by
the immutable data structures of [Clojure](https://clojure.org) but built to
resemble the native Python collections as closely as possible. It is
implemented in C, with a pure-Python fallback, and has no dependencies.

Documentation: <https://nben.net/pcollections/>

```sh
pip install pcollections
```

The library implements three persistent types: `plist`, `pset`, and
`pdict`. These are immutable versions of the builtin `list`, `set`, and `dict`
types. The persistent object interfaces are as similar as possible to the native
types, but the method signatures differ in ways necessary to accommodate
efficient immutable ways of doing things. For example, the `pdict` constructor
is identical to the `dict` constructor and always returns a `pdict` equal to the
`dict` that would be created with the same arguments. However, instead of
supporting operations like `d[key] = val`, `pdicts` support a `set` method: `d =
d.set(key, val)`.

In addition to the persistent types, there are two lazy types, `llist` and
`ldict`. These types are enabled by the `lazy` type. A `lazy` object is
basically a `partial` object that, when called, caches the function's return
value and returns that value without rerunning the function on subsequent
calls. The `llist` and `ldict` types are equivalent to the `plist` and `pdict`
types with one exception. Elements of an `llist` and values of an `ldict` that
are of the `lazy` type are dereferenced when requested. This allows a programmer
to easily create data structures (potentially nested data structures) whose
items are the results of complex or long-running computations that only get
computed once requested. The persistent data structures allow the arguments to
these lazy functions to be safe from mutation.

### Lazy values

- A `lazy` is an ordinary object in any collection other than the lazy
  collections: `pdict(x=lazy(f))['x']` is the `lazy` itself.
- Every read from a lazy collection computes the lazy values it returns. That
  includes indexing, `get`, `values()`, `items()`, iteration, `pop`,
  comparison, hashing, pickling, and converting to another collection:
  `pdict(ld)` computes the values of the `ldict` `ld`. `str` and `repr` show
  `<lazy>` without computing anything.
- The lazy constructors (`ldict(x)`, `llist(x)`, `tldict(x)`, `tllist(x)`)
  keep lazy values uncomputed. To get the raw `lazy` objects out of a lazy
  collection, use `getlazy`, `holdlazy(coll)`, or the `held_pdict`,
  `held_tdict`, `held_plist`, and `held_tlist` methods.
- A value is computed at most once, even when several threads request it at
  the same time. If the computation raises an exception, the failure is
  remembered: every request raises a new `LazyError` whose `__cause__` is the
  original exception, and whose message says where the `lazy` was created.
  Set `lazy.trace = True` (or the environment variable
  `PCOLLECTIONS_LAZY_TRACE=1`) to also record the full stack at creation.
  `with lazy_error_unwrap:` re-raises the original exception instead.
- A lazy value whose computation requests its own value raises `LazyError`.
  Two threads that each compute a lazy value needed by the other deadlock;
  this can't happen when lazy values are built from immutable data, because a
  lazy value can then only depend on lazy values that existed before it.
- Pickling a `lazy` computes it and pickles its value.
- `lazy` can be subclassed. A subclass may override `__call__` and call
  `super().__call__()`; lazy collections and `unlazy` call the override.

Finally, the persistent and lazy types have transient counterparts that enable
more efficient batch-mutation of the persistent types. The transient types
`tlist`, `tset`, `tdict`, `tllist`, and `tldict` all have interfaces equivalent
to their standard mutable counterparts (transient types are mutable).

### Threads

The persistent types can be shared freely between threads, including on
free-threaded ("no-GIL") builds of Python, which `pcollections` supports.

The transient types are meant to be used by one thread at a time, like the
builtin `list`, `set`, and `dict`. They are not corrupted if that rule is
broken: a modification that overlaps another modification of the same
transient (from another thread, or from code such as a key's `__eq__` that runs
during the modification) raises `RuntimeError`, as does a lookup whose
transient changes during a key comparison. As with `dict` and `set`, changing
the keys of a `tdict` or `tset` while iterating over it raises `RuntimeError`;
iterating over a `tlist` while changing it behaves like iterating over a
`list`.

### Behavior shared with the builtins

See [Differences from the builtins](https://nben.net/pcollections/differences.html)
for the full list.

- Lookups match objects by identity or equality, as `dict`, `set`, and `list`
  do, so `nan in pset([nan])` is true when `nan` is the same object.
- `hash(pset(x)) == hash(frozenset(x))`, so a `pset` and an equal `frozenset`
  are interchangeable as dictionary keys. Transient types are unhashable.
- `pdict` and `tdict` support `|` (and `tdict` supports `|=`), `fromkeys`,
  and `reversed`. `tlist` supports slice assignment and deletion.
- Persistent types have `drop(key, error=False)`, which returns the
  collection unchanged when the key (or index) isn't present, or raises
  `KeyError` (or `IndexError`) when `error` is true. `delete(key)` always
  raises.
- All types can be weakly referenced and subscripted for type hints
  (`pdict[str, int]`).

### Subclassing

All of the types can be subclassed, and methods that return a new collection
return an instance of the subclass. A persistent class names its transient
partner in the class attribute `__transient_type__` (used by `transient()`),
and a transient class names its persistent partner in `__persistent_type__`
(used by `persistent()`):

```python
class MyDict(pdict):
    __slots__ = ()
class MyTDict(tdict):
    __slots__ = ()
MyDict.__transient_type__ = MyTDict
MyTDict.__persistent_type__ = MyDict
```

### Backends

`pcollections` is implemented in C, with a pure-Python fallback. If the C
extension can't be loaded, `pcollections` uses the (much slower) Python
backend and issues a `RuntimeWarning`; `pcollections.using_c_extension` says
which backend is in use, and `pcollections.backend_error` holds the exception
that prevented loading the C extension. Two environment variables control
this:

- `PCOLLECTIONS_NO_C_EXTENSIONS=1` selects the Python backend without a
  warning.
- `PCOLLECTIONS_REQUIRE_C=1` makes importing `pcollections` fail when the C
  backend isn't available.

Objects pickled with one backend can be unpickled with the other.


## License

MIT License

Copyright (c) 2022-2026 Noah C. Benson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


