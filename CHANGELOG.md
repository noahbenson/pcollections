# Changelog

## 1.0.0 (unreleased)

Version 1.0 adds a C implementation of every type, makes the types behave
more like the builtins, and redesigns lazy values. Some of these changes are
not backward compatible; they are marked **(breaking)**.

### Implementation

- The types are implemented in C. The pure-Python implementation remains as
  a fallback, and both are tested against the same test suite.
- If the C extension can't be loaded, importing `pcollections` issues a
  `RuntimeWarning`. `pcollections.using_c_extension` says which
  implementation is in use and `pcollections.backend_error` says why the C
  extension couldn't be loaded. The environment variables
  `PCOLLECTIONS_NO_C_EXTENSIONS` and `PCOLLECTIONS_REQUIRE_C` choose the
  implementation.
- `pcollections` no longer depends on `phamt`, or on any other package.
- Objects pickled with one implementation can be unpickled with the other.
- Type stubs are included.
- Python 3.8 through 3.15 are supported, including the free-threaded
  builds of 3.13 and later. Python 3.7 is no longer supported. **(breaking)**

### Transient collections

- A modification of a transient collection that overlaps another
  modification of the same collection (from another thread, or from a key's
  `__eq__` or `__hash__`) raises `RuntimeError` instead of corrupting the
  collection. So does a read (a lookup or a step of an iteration) made while
  a modification is in progress.
- Changing the keys of a `tdict` or `tset` while iterating over it raises
  `RuntimeError`, as it does for `dict` and `set`. Iterating over a `tlist`
  while changing it behaves as it does for `list`.
- `tlist` supports slice assignment and deletion (`t[1:3] = ...`,
  `del t[::2]`). `tlist += iterable` accepts any iterable.
- Transient collections are unhashable. **(breaking)**

### Behavior shared with the builtins

- Lookups, `==`, `in`, `index`, `count`, and `remove` match elements by
  identity or equality, as `dict`, `set`, and `list` do, so a NaN object is
  found in a collection that contains it. **(breaking)**
- Sequences compare as `list` does: the first pair of unequal elements
  decides the order. Sequences whose elements are equal but not orderable can
  be compared for equality.
- `hash(pset(x)) == hash(frozenset(x))`. The hash of a `pdict` doesn't depend
  on the order of its items.
- Operators return `NotImplemented` for operands they don't support, so that
  the other operand can handle the operation. `plist * 2.0` raises
  `TypeError`. **(breaking)**
- Error messages match those of `list`, `set`, and `dict`.
- `pdict` and `tdict` support `|`, `|=` (`tdict` only), `fromkeys`, and
  `reversed()`, as do their views.
- All types can be subscripted in type hints (`pdict[str, int]`) and weakly
  referenced.

### Persistent collections

- `drop(key, error=False)` is available on `pdict`, `pset`, and `plist`. It
  returns the collection unchanged when the key or index isn't present, or,
  if `error` is true, raises `KeyError` (or `IndexError`). `delete(key)`
  always raises.

### Subclasses

- All types can be subclassed, and methods that return a new collection
  return an instance of the subclass.
- The class attributes `__transient_type__` and `__persistent_type__` name a
  class's partner type, used by `transient()` and `persistent()`.

### Lazy values

- A read from a lazy collection computes the lazy values it returns;
  lazy values in any other collection are ordinary objects. Converting a lazy
  collection to another collection computes its values. **(breaking)**
- A lazy value is computed at most once, even when several threads request
  it at the same time.
- A failed computation is remembered: every later request raises a new
  `LazyError` whose `__cause__` is the original exception and whose message
  says where the `lazy` was created. `LazyError` has the attributes `func`,
  `func_args`, `func_kwargs`, `origin`, `origin_stack`, and `cause`.
  **(breaking)**
- `lazy.trace = True`, or the environment variable
  `PCOLLECTIONS_LAZY_TRACE=1`, records the stack where each `lazy` is
  created.
- `lazy_error_unwrap` returns the cause of a `LazyError`, and, as a context
  manager, re-raises the cause in place of the `LazyError`.
- A lazy value whose computation requests its own value raises `LazyError`.
- `lazy` can be subclassed, and a subclass's `__call__` is used by the lazy
  collections and by `unlazy`.
- Pickling a `lazy` computes it and pickles its value.
- `ldict.as_pdict`, `llist.as_plist`, and `tldict.as_tdict` are renamed
  `held_pdict`, `held_plist`, and `held_tdict`, and `tllist.held_tlist` is
  new. **(breaking)**
- `holdlazy(coll)` returns a lazy collection as the equivalent plain
  collection, with its lazy values uncomputed.

## 0.4.0

- Added `lazy_error_unwrap`.
- `pdict(ld)` computes the lazy values of the `ldict` `ld`, and `ldict(d)`
  keeps lazy values uncomputed for subclasses of `ldict`.
- `tllist` and `tldict` are exported from `pcollections`, and `tldict` has
  the lazy-value methods of `ldict`.
