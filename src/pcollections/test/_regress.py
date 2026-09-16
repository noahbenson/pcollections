# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_regress.py
# Regression tests for crashes and memory-safety bugs.
# By Noah C. Benson

"""Regression tests for crashes and memory corruption.

Each test runs its scenario in a fresh child interpreter, so a segfault
fails that one test instead of killing the whole suite. A scenario prints
``ok`` when every check passes; anything else (a crash, an exception, a
wrong answer) fails the test.

Tests for bugs that are still open are marked with ``known_failure`` for
the affected backends (see ``_backends.known_failure``).
"""

import os
import subprocess
import sys
import textwrap

from ._backends import make_tests, known_failure



_PKG_PARENT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

_PRELUDE = """\
import gc, sys
from pcollections.test._backends import BACKENDS
B = BACKENDS[{backend!r}]
pdict, tdict = B['pdict'], B['tdict']
plist, tlist = B['plist'], B['tlist']
pset, tset = B['pset'], B['tset']
"""


class _RegressionTests:
    """Scenario tests, instantiated once per backend by ``make_tests``."""

    def run_scenario(self, code, timeout=300):
        src = _PRELUDE.format(backend=self.backend_name) + textwrap.dedent(code)
        env = dict(os.environ)
        env['PYTHONPATH'] = os.pathsep.join(
            [_PKG_PARENT] + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
        env['PYTHONFAULTHANDLER'] = '1'
        proc = subprocess.run(
            [sys.executable, '-c', src],
            env=env, capture_output=True, text=True, errors='replace',
            timeout=timeout)
        detail = (f"exit status {proc.returncode}\n"
                  f"--- stdout ---\n{proc.stdout[-2000:]}\n"
                  f"--- stderr ---\n{proc.stderr[-2000:]}")
        self.assertEqual(proc.returncode, 0, detail)
        self.assertEqual(proc.stdout.strip().splitlines()[-1:], ['ok'], detail)

    # Overwriting values in a large transient dict ---------------------------
    # A tdict entry is rewritten in place by copying the old entry and
    # changing its value, so the key object is in both. The overwrite must
    # take references for the new entry before releasing the old one;
    # otherwise, when the entry holds the only reference to the key (any
    # int > 256 that the caller built separately), the key is freed and then
    # used.
    def test_tdict_overwrite_large(self):
        self.run_scenario("""
            for n in (1, 28, 29, 30, 257, 300, 840, 841, 842, 1000, 3000):
                t = tdict()
                for k in range(n):
                    t[k] = k
                for k in range(n):
                    t[k] = -k
                gc.collect()
                assert list(t.items()) == [(k, -k) for k in range(n)], n
                t = pdict((k, k) for k in range(n)).transient()
                for k in range(n):
                    t[k] = -k
                assert list(t.items()) == [(k, -k) for k in range(n)], n
            print('ok')
        """)

    def test_tdict_modular_churn(self):
        self.run_scenario("""
            for M in (100, 300, 500, 1000):
                for a, d in ((3, 2), (7, 3), (3, 5)):
                    t, ref = tdict(), {}
                    s, sref = tset(), {}
                    for j in range(4000):
                        t[j % M] = j; ref[j % M] = j
                        s.add(j % M * 1000); sref[j % M * 1000] = None
                        if j % d == 0:
                            t.pop((j * a) % M, None); ref.pop((j * a) % M, None)
                            s.discard((j * a) % M * 1000)
                            sref.pop((j * a) % M * 1000, None)
                    gc.collect()
                    assert list(t.items()) == list(ref.items()), (M, a, d)
                    assert list(s) == list(sref), (M, a, d)
            print('ok')
        """)

    # The garbage collector must not clear live objects ----------------------
    # Several versions of a collection share trie nodes. If each version
    # reports every item in the shared nodes, the collector over-counts
    # internal references and can decide a live, externally referenced
    # object is garbage.
    def test_gc_shared_nodes_keep_items_alive(self):
        self.run_scenario("""
            def trial(make, selfref, k):
                gc.collect()
                L = [1, 2, 3]
                if selfref:
                    L.append(L)
                base = make(L)
                versions = [base.set(1999, j) for j in range(k)]
                del base
                junk = [versions]
                junk.append(junk)
                del versions, junk
                gc.collect()
                return len(L)
            makers = {
                'pdict': lambda L: pdict({i: (L if i == 0 else i)
                                          for i in range(2000)}),
                'plist': lambda L: plist([L] + list(range(1999))),
            }
            for name, make in makers.items():
                for selfref in (False, True):
                    for k in range(1, 6):
                        got = trial(make, selfref, k)
                        assert got == (4 if selfref else 3), (name, selfref, k, got)
            print('ok')
        """)

    # Mutating a transient while iterating it --------------------------------
    # tdict/tset behave like dict/set: a size change during iteration raises
    # RuntimeError on the next step. tlist behaves like list: iteration is by
    # index, never raises, and stops when the index reaches the length.
    def test_tdict_delete_during_iteration(self):
        self.run_scenario("""
            for view in ('keys', 'items', 'values'):
                t = tdict((k, k) for k in range(2000))
                try:
                    for x in getattr(t, view)():
                        t.popitem()
                except RuntimeError:
                    pass
                else:
                    raise AssertionError(view + ': no RuntimeError')
            print('ok')
        """)

    def test_tdict_clear_with_live_iterator(self):
        self.run_scenario("""
            t = tdict((k, k) for k in range(2000))
            it = iter(t)
            next(it)
            t.clear()
            for k in range(3000):
                t[('new', k)] = k
            try:
                next(it)
            except RuntimeError:
                print('ok')
        """)

    def test_tset_churn_during_iteration(self):
        self.run_scenario("""
            s = tset(range(2000))
            try:
                for x in s:
                    s.discard(x)
                    s.add(-x - 1)
            except RuntimeError:
                print('ok')
        """)

    def test_tlist_mutation_during_iteration_is_list_like(self):
        self.run_scenario("""
            def run(make):
                l = make(range(2000))
                seen = []
                for x in l:
                    seen.append(x)
                    l.pop(0)
                    if len(seen) % 7 == 0:
                        l.append(-len(seen))
                return seen, list(l)
            assert run(tlist) == run(list)
            l = tlist(range(2000))
            it = iter(l)
            next(it)
            l.clear()
            assert list(it) == []
            print('ok')
        """)

    # Key __eq__/__hash__ that mutate the collection -------------------------
    # User code that runs during a lookup must not be able to corrupt the
    # transient. A mutation from inside such code (or one that lands between
    # the lookup's steps) raises RuntimeError instead.
    def test_reentrant_key_eq(self):
        self.run_scenario("""
            class Key:
                box = None
                armed = False
                def __init__(self, v):
                    self.v = v
                def __hash__(self):
                    return 7
                def __eq__(self, other):
                    t = Key.box
                    if Key.armed and len(t):
                        Key.armed = False
                        t.clear()
                        for i in range(200):
                            if isinstance(t, tdict):
                                t[('f', i)] = i
                            else:
                                t.add(('f', i))
                    return self is other
            def fill(make):
                Key.armed = False
                t = make()
                Key.box = t
                keys = [Key(i) for i in range(20)]
                for k in keys:
                    if make is tdict:
                        t[k] = k.v
                    else:
                        t.add(k)
                return t, keys
            def lookups(t):
                yield lambda: Key(100) in t
                if isinstance(t, tdict):
                    yield lambda: t[Key(100)]
                    yield lambda: t.get(Key(100))
                    yield lambda: t.__setitem__(Key(100), 0)
                    yield lambda: t.pop(Key(100), None)
                else:
                    yield lambda: t.add(Key(100))
                    yield lambda: t.discard(Key(100))
            for make in (tdict, tset):
                t, _ = fill(make)
                for i, op in enumerate(list(lookups(t))):
                    t, keys = fill(make)
                    op = list(lookups(t))[i]
                    Key.armed = True
                    try:
                        op()
                    except RuntimeError:
                        pass
                    else:
                        raise AssertionError((make, i, 'no RuntimeError'))
                    # The collection is still usable and self-consistent.
                    items = list(t)
                    assert len(items) == len(t), (make, i)
                    for x in items:
                        assert x in t, (make, i, x)
                    t.clear()
                    assert len(t) == 0 and list(t) == []
            print('ok')
        """)

    # Values replaced during iteration ---------------------------------------
    # Like dict, replacing values while iterating over a tdict is allowed; the
    # iterator must find its place again after the trie changes underneath.
    def test_tdict_value_updates_during_iteration(self):
        self.run_scenario("""
            for n in (5, 30, 1000, 3000):
                t = tdict((k, k) for k in range(n))
                p = t.persistent()   # later writes must copy shared nodes
                seen = []
                for k in t:
                    seen.append(k)
                    t[k] = [k]
                    t[(k * 7) % n] = -k
                assert seen == list(range(n)), n
                del p
                seen = [v for v in t.values()]
                assert len(seen) == n
            print('ok')
        """)

    # Use of a transient during its own modification ------------------------
    # A __del__ that runs while a transient is being modified sees a
    # consistent collection, and modifying it from there raises.
    def test_transient_use_from_del_during_update(self):
        self.run_scenario("""
            import sys
            errors = []
            sys.unraisablehook = lambda u: errors.append(u.exc_type)
            class Noisy:
                box = None
                def __del__(self):
                    t = Noisy.box
                    list(t)
                    len(t)
                    if isinstance(t, tdict):
                        t['x'] = 1
                    elif isinstance(t, tset):
                        t.add('x')
                    else:
                        t.append('x')
            for make in (tdict, tset, tlist):
                t = make()
                Noisy.box = t
                for i in range(50):
                    if make is tdict:
                        t[i] = Noisy()
                    elif make is tset:
                        t.add(Noisy())
                    else:
                        t.append(Noisy())
                if make is tdict:
                    for i in range(50):
                        t[i] = i
                elif make is tset:
                    for x in list(t):
                        t.discard(x)
                else:
                    for i in range(50):
                        t[i] = i
            assert errors and set(errors) == {RuntimeError}, errors
            print('ok')
        """)

    # Sequence equality must not require orderable elements -----------------
    # Equality must not compare elements with `<`; otherwise
    # plist([None]) == plist([None]) raises TypeError.
    def test_sequence_equality_of_unorderable_elements(self):
        self.run_scenario("""
            for make in (plist, tlist):
                assert make([None, {}]) == make([None, {}])
                assert make([None]) != make([object()])
                assert make([None]) == [None]
            print('ok')
        """)


    # Reading a transient while it is being modified ------------------------
    # A compaction rehashes the remaining keys, which runs user code after the
    # modification has begun and before the old trie is released. An iterator
    # started there must not walk the old trie afterward; reads during a
    # modification raise RuntimeError.
    def test_read_during_compaction(self):
        self.run_scenario("""
            outcomes = set()
            holder = []
            class Key:
                armed = None
                skip = None
                def __init__(self, v):
                    self.v = v
                def __hash__(self):
                    if Key.armed is not None and self is not Key.skip:
                        coll, Key.armed = Key.armed, None
                        try:
                            it = iter(coll)
                            next(it)
                            holder.append(it)
                            outcomes.add('started')
                        except RuntimeError:
                            outcomes.add('refused')
                        for probe in (lambda: Key(1) in coll,
                                      lambda: list(coll)):
                            try:
                                probe()
                                outcomes.add('read')
                            except RuntimeError:
                                pass
                    return hash(self.v)
                def __eq__(self, other):
                    return isinstance(other, Key) and self.v == other.v
            def add_d(c, k): c[k] = 0
            def rem_d(c, k): del c[k]
            for (make, add, remove) in ((tdict, add_d, rem_d),
                                        (tset, tset.add, tset.discard)):
                for n in (40, 400, 4000):
                    coll = make()
                    keys = [Key(i) for i in range(n)]
                    for k in keys:
                        add(coll, k)
                    for k in keys[:-2]:
                        Key.armed, Key.skip = coll, k
                        remove(coll, k)
                        Key.armed = None
                        for it in holder:
                            try:
                                for x in it:
                                    pass
                            except RuntimeError:
                                pass
                        del holder[:]
                    assert len(coll) == 2
            assert outcomes == {'refused'}, outcomes
            print('ok')
        """)

    # Reads from __del__ methods run by a modification (as the modification
    # releases old values or shifts list elements) raise RuntimeError instead
    # of seeing the collection part way through the change; a value released
    # after the modification is done may read it normally.
    def test_read_from_del_during_modification(self):
        self.run_scenario("""
            results = []
            class Probe:
                def __init__(self, action):
                    self.action = action
                def __del__(self):
                    try:
                        self.action()
                        results.append('ok')
                    except (RuntimeError, StopIteration):
                        results.append('refused')
            for n in (50, 300, 3000):
                t = tdict((i, i) for i in range(n))
                it = iter(t.items())
                next(it)
                t[0] = Probe(lambda: [next(it) for _ in range(5)])
                t[1] = Probe(lambda: (1 in t, t.get(2), list(t)[:3]))
                del t[0]
                del t[1]
                for i in range(2, n - 2):
                    del t[i]
                gc.collect()
                assert dict(t.items()) == {n - 2: n - 2, n - 1: n - 1}
            for n in (50, 300):
                l = tlist(range(n))
                it = iter(l)
                next(it)
                l[n // 2] = Probe(lambda: ([next(it) for _ in range(5)],
                                           l[3], l[1:4]))
                del l[n // 2]
                for i in range(n // 3):
                    del l[len(l) // 2]
                gc.collect()
                assert len(list(l)) == len(l)
            assert results and set(results) <= {'ok', 'refused'}, results
            print('ok')
        """)

make_tests('TestRegressions', _RegressionTests, globals())
