# Differences from the builtins

The transient types behave like `list`, `set`, and `dict`, and the
persistent types behave like them wherever immutability allows. This page
lists the differences.

## Persistent types return new collections

Methods that change a builtin collection instead return a new collection:

| Builtin                  | Persistent                                     |
|--------------------------|------------------------------------------------|
| `l.append(x)`            | `p = p.append(x)`                              |
| `l[i] = x`               | `p = p.set(i, x)`                              |
| `del l[i]`               | `p = p.delete(i)` or `p = p.drop(i)`           |
| `x = l.pop()`            | `(x, p) = p.pop()`                             |
| `l.sort()`               | `p = p.sort()`                                 |
| `s.add(x)`               | `p = p.add(x)`                                 |
| `s.discard(x)`           | `p = p.discard(x)`                             |
| `x = s.pop()`            | `(x, p) = p.pop()`                             |
| `d[k] = v`               | `p = p.set(k, v)`                              |
| `del d[k]`               | `p = p.delete(k)` or `p = p.drop(k)`           |
| `v = d.pop(k)`           | `(v, p) = p.pop(k)`                            |
| `(k, v) = d.popitem()`   | `((k, v), p) = p.popitem()`                    |
| `d.update(other)`        | `p = p.update(other)` or `p = p \| other`      |
| `v = d.setdefault(k, x)` | `p = p.setdefault(k, x)` (returns the mapping) |
| `l.clear()`              | `p = p.clear()` (the empty collection)         |

Persistent collections also have methods that the builtins lack:
`plist.prepend`, `pset.addall`, `pset.discardall`, `pset.removeall`,
`pdict.setall`, `pdict.dropall`, and `pdict.deleteall`. The transient types
have `tlist.prepend`, `tset.addall`, `tset.discardall`, and `tset.removeall`.

`drop` and `delete` differ only when the key (or index) isn't present:
`delete` raises `KeyError` (or `IndexError`), and `drop` returns the
collection unchanged, or raises if its `error` argument is true.

The persistent types have no in-place operators; `p += x` rebinds `p` to
`p + x`.

## Hashing and equality

- Persistent collections are hashable when their elements are. A `pset`
  hashes like an equal `frozenset`. Transient collections, like `list`,
  `set`, and `dict`, aren't hashable.
- A `plist` or `tlist` equals a `list` with equal elements, and never equals
  a `tuple` (as with `list`). A `pset` or `tset` equals an equal `set` or
  `frozenset`, and a `pdict` or `tdict` equals an equal `dict`.
- `list + plist` returns a `plist` (and `list + tlist` a `tlist`), since
  `list` leaves the operation to the other operand. For the same reason,
  `dict | pdict` returns a `pdict` and `set | pset` returns a `pset`.
- A persistent collection isn't an instance of the corresponding builtin (a
  `pset` isn't a `frozenset`), but every type is registered with the
  matching abstract base class in `collections.abc` (`Sequence`, `Set`,
  `Mapping`, or their mutable versions) and with those in
  `pcollections.abc`.

## Construction

- `pdict.empty`, `plist.empty`, and `pset.empty` are the empty collections.
  For the transient types, `empty()` is a class method that returns a new
  empty collection.
- The constructors accept the same arguments as those of the builtins.
  `plist(p)`, `pset(p)`, and `pdict(p)` return `p` itself when it is
  already of that type.

## Order

`pdict`, `tdict`, `pset`, and `tset` iterate in the order in which their
elements were added. (`set` and `frozenset` don't guarantee an order.)

`popitem()` (on `pdict` and `tdict`) removes the last item, the one most
recently added, as `dict.popitem()` does. `pop()` (on `pset` and `tset`)
also removes the last element, where `set.pop()` removes an arbitrary one.

## Printing

Persistent collections print with `|` inside their brackets (`[|1, 2|]`,
`{|1, 2|}`, `{|'a': 1|}`), and transient collections with `<` and `>`
(`[<1, 2>]`).

## Performance

Lookups and modifications take time that grows at most logarithmically
with the size of the collection, and `transient()` and `persistent()` take
constant time. The exceptions are in the list types: inserting or deleting an
element in the middle of a `plist` or `tlist`, and slice assignment and
deletion on a `tlist`, take time proportional to the length of the list.
(Appending, prepending, and removing the first or last element don't.)

The builtins are faster for most single operations. The persistent types are
much faster than copying a builtin collection to make a changed version of
it.

## Transients and threads

A modification of a transient collection that overlaps another modification
of the same collection, from another thread or from code that runs during the
modification, raises `RuntimeError` instead of corrupting the collection, and
so does a read made while a modification is in progress. See
[Threads](guide.md#threads).

## Subclasses

Methods that return new collections return instances of the subclass (where
`list`, `set`, and `dict` return instances of the builtin), and instances of
persistent subclasses can't have attributes set. See
[Subclassing](guide.md#subclassing).
