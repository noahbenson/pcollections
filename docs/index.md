# pcollections

`pcollections` provides persistent (immutable) collections for Python that
behave as much like the builtin `list`, `set`, and `dict` as possible, along
with lazy collections whose values are computed only when they are needed.

```python
>>> from pcollections import pdict, lazy, ldict
>>> d = pdict(a=1, b=2)
>>> d2 = d.set('c', 3)      # a new pdict; d is unchanged
>>> d
{|'a': 1, 'b': 2|}
>>> d2
{|'a': 1, 'b': 2, 'c': 3|}
>>> t = d2.transient()      # a mutable copy, made in constant time
>>> t['d'] = 4
>>> t.persistent()
{|'a': 1, 'b': 2, 'c': 3, 'd': 4|}
>>> ld = ldict(x=lazy(sum, range(10)))
>>> ld['x']                 # computed now, once
45
```

## Installation

```sh
pip install pcollections
```

`pcollections` supports CPython 3.8 through 3.15 (support for 3.8 is
deprecated) and has no dependencies.

```{important}
`pcollections` has a C backend and a much slower pure-Python backend, and an
installation that can't load the C backend still works, using the Python
backend. Check which one you have:

    python -c "import pcollections; print(pcollections.using_c_extension)"

and see [Installation and backends](install.md) for how to make sure you get
the C backend.
```

```{toctree}
:maxdepth: 2

install
guide
differences
api
changelog
```
