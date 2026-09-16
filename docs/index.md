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

`pcollections` requires Python 3.8 or later and has no dependencies. Wheels
with the compiled C implementation are available for most platforms; on
other platforms, `pip` compiles it, or `pcollections` uses its pure-Python
implementation (see [Backends](guide.md#backends)).

```{toctree}
:maxdepth: 2

guide
differences
api
changelog
```
