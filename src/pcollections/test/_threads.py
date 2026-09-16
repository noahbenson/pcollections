# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_threads.py
# Multi-threaded stress tests, run against every backend.
# By Noah C. Benson

"""Multi-threaded stress tests.

Persistent collections may be shared freely between threads. Transient
collections are meant for one thread at a time; these tests check that
breaking that rule raises ``RuntimeError`` (or gives some consistent
result) rather than crashing or corrupting the collection. They are most
meaningful on free-threaded builds of Python, where the threads really run
at once; with the GIL, a very short switch interval makes the threads
interleave as often as possible.

Each scenario runs in a subprocess (see ``_regress.py``), so that a crash
fails one test rather than the whole run.
"""

from ._backends import make_tests
from ._regress import _RegressionTests


_THREAD_PRELUDE = """
import random, sys, threading
sys.setswitchinterval(1e-6)
NTHREADS = 8
def run_threads(worker, n=NTHREADS):
    errors = []
    barrier = threading.Barrier(n)
    def main(seed):
        try:
            barrier.wait()
            worker(seed)
        except BaseException as e:
            errors.append(e)
    threads = [threading.Thread(target=main, args=(i,)) for i in range(n)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    if errors:
        raise errors[0]
class Key:
    # A key whose comparisons run Python code (so that threads switch in the
    # middle of lookups) and whose hashes collide often.
    __slots__ = ('v',)
    def __init__(self, v):
        self.v = v
    def __hash__(self):
        return self.v % 17
    def __eq__(self, other):
        return isinstance(other, Key) and self.v == other.v
    def __repr__(self):
        return f"Key({self.v})"
"""


class _ThreadTests:
    """Instantiated once per backend by ``make_tests``."""

    run_scenario = _RegressionTests.run_scenario

    def run_threaded(self, code, timeout=600):
        import textwrap
        self.run_scenario(_THREAD_PRELUDE + textwrap.dedent(code),
                          timeout=timeout)

    def test_shared_persistent_collections(self):
        self.run_threaded("""
            n = 3000
            base_d = pdict((i, i) for i in range(n))
            base_l = plist(range(n))
            base_s = pset(range(n))
            hashes = []
            def worker(seed):
                r = random.Random(seed)
                # Hash the shared collections concurrently (the cached hash
                # is written by whichever thread finishes first).
                hashes.append((hash(base_d), hash(base_l), hash(base_s)))
                d, l, s = base_d, base_l, base_s
                for step in range(2000):
                    k = r.randrange(2 * n)
                    d = d.set(k, -k) if r.random() < 0.7 else d.drop(k)
                    l = l.set(r.randrange(len(l)), k).append(k)
                    s = s.add(-k - 1) if r.random() < 0.7 else s.discard(k)
                    assert base_d.get(k) == (k if k < n else None)
                    assert (k in base_s) == (k < n)
                    assert base_l[k % n] == k % n
                t = d.transient()
                for k in list(t)[:100]:
                    del t[k]
                assert len(t.persistent()) == len(d) - 100
                assert list(base_l) == list(range(n))
            run_threads(worker)
            assert len(set(hashes)) == 1
            assert dict(base_d.items()) == {i: i for i in range(n)}
            assert set(base_s) == set(range(n))
            print('ok')
        """)

    def test_shared_transient_dict(self):
        self.run_threaded("""
            t = tdict((Key(i), i) for i in range(100))
            snapshots = []
            def worker(seed):
                r = random.Random(seed)
                for step in range(3000):
                    k = Key(r.randrange(200))
                    op = r.random()
                    try:
                        if op < 0.35:
                            t[k] = step
                        elif op < 0.5:
                            t.pop(k, None)
                        elif op < 0.7:
                            t.get(k)
                            k in t
                        elif op < 0.8:
                            for x in t.items():
                                pass
                        elif op < 0.85:
                            snapshots.append(t.persistent())
                        elif op < 0.9:
                            len(t)
                            list(t.values())
                        elif op < 0.92:
                            t.clear()
                        else:
                            del t[k]
                    except (RuntimeError, KeyError):
                        pass
            run_threads(worker)
            items = list(t.items())
            assert len(items) == len(t)
            assert len(set(k for (k, v) in items)) == len(items)
            for (k, v) in items:
                assert t[k] == v
            assert dict(t.persistent().items()) == dict(items)
            for p in snapshots[-20:]:
                ps = list(p.items())
                assert len(ps) == len(p)
                for (k, v) in ps:
                    assert p[k] == v
            print('ok')
        """)

    def test_shared_transient_set(self):
        self.run_threaded("""
            t = tset(Key(i) for i in range(100))
            def worker(seed):
                r = random.Random(seed)
                for step in range(3000):
                    k = Key(r.randrange(200))
                    op = r.random()
                    try:
                        if op < 0.35:
                            t.add(k)
                        elif op < 0.6:
                            t.discard(k)
                        elif op < 0.8:
                            k in t
                        elif op < 0.9:
                            for x in t:
                                pass
                        elif op < 0.95:
                            t.persistent()
                        else:
                            t.clear()
                    except RuntimeError:
                        pass
            run_threads(worker)
            els = list(t)
            assert len(els) == len(t) == len(set(els))
            for x in els:
                assert x in t
            assert set(t.persistent()) == set(els)
            print('ok')
        """)

    def test_shared_transient_list(self):
        self.run_threaded("""
            t = tlist(range(100))
            def worker(seed):
                r = random.Random(seed)
                for step in range(3000):
                    op = r.random()
                    try:
                        n = len(t)
                        if op < 0.25:
                            t.append(step)
                        elif op < 0.35:
                            t.prepend(step)
                        elif op < 0.45:
                            t.insert(r.randrange(n + 1), step)
                        elif op < 0.6:
                            t.pop(r.randrange(n)) if n else None
                        elif op < 0.7:
                            t[r.randrange(n)] = step if n else None
                        elif op < 0.85:
                            for x in t:
                                pass
                        elif op < 0.9:
                            t.persistent()
                        elif op < 0.95:
                            t[r.randrange(n)] if n else None
                        elif op < 0.97:
                            del t[r.randrange(n)]
                        else:
                            t.clear()
                    except (RuntimeError, IndexError, ValueError):
                        pass
            run_threads(worker)
            els = list(t)
            assert len(els) == len(t)
            assert [t[i] for i in range(len(t))] == els
            assert list(t.persistent()) == els
            print('ok')
        """)

    def test_iteration_while_other_threads_modify(self):
        self.run_threaded("""
            td = tdict((i, i) for i in range(500))
            ts = tset(range(500))
            tl = tlist(range(500))
            stop = threading.Event()
            def worker(seed):
                r = random.Random(seed)
                if seed % 2 == 0:
                    # Modify values only: iteration of td may continue.
                    for step in range(3000):
                        k = r.randrange(500)
                        try:
                            td[k] = step
                            tl[k] = step
                        except (RuntimeError, IndexError):
                            pass
                        if step % 50 == 0:
                            try:
                                ts.discard(k)
                            except RuntimeError:
                                pass
                            while True:
                                try:
                                    ts.add(k)
                                    break
                                except RuntimeError:
                                    pass
                else:
                    for step in range(40):
                        for coll in (td, ts, tl):
                            try:
                                n = 0
                                for x in coll:
                                    n += 1
                                assert n <= 500, n
                            except RuntimeError:
                                pass
            run_threads(worker)
            assert sorted(td) == list(range(500))
            assert sorted(ts) == list(range(500))
            assert len(tl) == 500
            print('ok')
        """)

    def test_shared_lazy_values(self):
        self.run_threaded("""
            lazy, ldict = B['lazy'], B['ldict']
            LazyError = B['LazyError']
            counts = [0] * 400
            lock = threading.Lock()
            def compute(i):
                with lock:
                    counts[i] += 1
                if i % 7 == 0:
                    raise ValueError(i)
                # Depend on earlier values (never on later ones).
                return i + (d[i - 1] if i % 7 != 1 and i > 0 else 0)
            d = None
            d = ldict((i, lazy(compute, i)) for i in range(400))
            def worker(seed):
                r = random.Random(seed)
                for step in range(2000):
                    i = r.randrange(400)
                    try:
                        v = d[i]
                        assert v >= i
                    except LazyError as e:
                        assert isinstance(e.__cause__, (ValueError, LazyError))
            run_threads(worker)
            assert max(counts) == 1, counts
            print('ok')
        """)


make_tests('TestThreads', _ThreadTests, globals())
