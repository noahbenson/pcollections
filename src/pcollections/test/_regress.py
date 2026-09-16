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
            env=env, capture_output=True, text=True, timeout=timeout)
        detail = (f"exit status {proc.returncode}\n"
                  f"--- stdout ---\n{proc.stdout[-2000:]}\n"
                  f"--- stderr ---\n{proc.stderr[-2000:]}")
        self.assertEqual(proc.returncode, 0, detail)
        self.assertEqual(proc.stdout.strip().splitlines()[-1:], ['ok'], detail)

    # Overwriting values in a large transient dict ---------------------------
    # The in-place leaf overwrite released the old entry before taking
    # references for the new one. A tdict entry is rewritten by copying the
    # old entry and changing its value, so the key object is in both; when
    # the entry held the only reference to the key (any int > 256 that the
    # caller built separately), the key was freed and then used.
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
    @known_failure('c')
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
    @known_failure('c')
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

    @known_failure('c')
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

    @known_failure('c', 'python')
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

    @known_failure('c', 'python')
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
    @known_failure('c', 'python')
    def test_reentrant_key_eq(self):
        self.run_scenario("""
            class Key:
                box = None
                def __init__(self, v):
                    self.v = v
                def __hash__(self):
                    return 7
                def __eq__(self, other):
                    t = Key.box
                    if len(t):
                        t.clear()
                        for i in range(200):
                            if isinstance(t, tdict):
                                t[('f', i)] = i
                            else:
                                t.add(('f', i))
                    return False
            for make in (tdict, tset):
                t = make()
                Key.box = t
                for i in range(20):
                    k = Key(i)
                    if make is tdict:
                        t[k] = i
                    else:
                        t.add(k)
                raised = 0
                for i in range(20):
                    try:
                        Key(100 + i) in t
                    except RuntimeError:
                        raised += 1
                assert raised == 20, (make, raised)
            print('ok')
        """)


make_tests('TestRegressions', _RegressionTests, globals())
