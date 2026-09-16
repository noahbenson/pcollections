"""Uses of the pcollections API, checked against the type stubs by mypy
(see .github/workflows/tests.yml). This file is not run."""

from typing import Dict, List, Tuple

from typing_extensions import assert_type

from pcollections import (
    LazyError, holdlazy, lazy, lazy_error_unwrap, ldict, llist, pdict, plist,
    pset, tdict, tldict, tlist, tllist, tset, unlazy)


def dicts() -> None:
    d = pdict({'a': 1})
    assert_type(d, pdict[str, int])
    assert_type(d['a'], int)
    assert_type(d.get('a'), 'int | None')
    assert_type(d.set('b', 2), pdict[str, int])
    assert_type(d.drop('a', error=True), pdict[str, int])
    assert_type(d.pop('a'), 'tuple[int, pdict[str, int]]')
    assert_type(d | {'x': 1.5}, 'pdict[str, int | float]')
    t = d.transient()
    assert_type(t, tdict[str, int])
    t['c'] = 3
    t |= {'d': 4}
    assert_type(t.pop('c'), int)
    assert_type(t.persistent(), pdict[str, int])
    assert_type(pdict.fromkeys(['a'], 0), pdict[str, int])
    assert_type(pdict(x=1.0), pdict[str, float])
    pairs: List[Tuple[int, str]] = [(1, 'a')]
    assert_type(pdict(pairs), pdict[int, str])
    hashable: Dict[object, int] = {d: 1}
    assert hashable


def lists() -> None:
    p = plist([1, 2, 3])
    assert_type(p, plist[int])
    assert_type(p[0], int)
    assert_type(p[1:], plist[int])
    assert_type(p.append(4), plist[int])
    assert_type(p.pop(), 'tuple[int, plist[int]]')
    assert_type(p + ['a'], 'plist[int | str]')
    assert_type(p * 2, plist[int])
    t = p.transient()
    assert_type(t, tlist[int])
    t[0] = 5
    t[1:2] = [6, 7]
    del t[::2]
    t += (8,)
    assert_type(t.pop(), int)
    assert_type(t.persistent(), plist[int])
    xs: List[int] = list(p)
    assert p < xs


def sets() -> None:
    s = pset([1, 2])
    assert_type(s, pset[int])
    assert_type(s.add(3), pset[int])
    assert_type(s.add('a'), 'pset[int | str]')
    assert_type(s | {'b'}, 'pset[int | str]')
    assert_type(s & {1}, pset[int])
    assert_type(s.pop(), 'tuple[int, pset[int]]')
    t = s.transient()
    assert_type(t, tset[int])
    t.add(4)
    t |= {5}
    assert_type(t.persistent(), pset[int])
    hashable: Dict[object, int] = {s: 1, frozenset([1]): 2}
    assert hashable


def lazies() -> None:
    x = lazy(int, '5')
    assert_type(x, lazy[int])
    assert_type(x(), int)
    assert_type(unlazy(x), int)
    assert_type(unlazy(5), int)
    ld = ldict[str, int](a=x, b=6)
    assert_type(ld, ldict[str, int])
    assert_type(ld['a'], int)
    assert_type(ld.getlazy('a'), 'int | lazy[int] | None')
    assert_type(ld.held_pdict(), 'pdict[str, int | lazy[int]]')
    assert_type(holdlazy(ld), 'pdict[str, int | lazy[int]]')
    assert_type(ld.transient(), tldict[str, int])
    ll = llist[int]([x, 1])
    assert_type(ll, llist[int])
    assert_type(ll[0], int)
    assert_type(ll.transient(), tllist[int])
    assert_type(holdlazy(ll), 'plist[int | lazy[int]]')
    try:
        with lazy_error_unwrap:
            ld['a']
    except LazyError as e:
        assert_type(e.cause, 'BaseException | None')
        assert_type(e.root_cause, 'BaseException | None')
        assert_type(lazy_error_unwrap(e), BaseException)
