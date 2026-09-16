# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_gc.py
# Garbage-collection tests, run against every backend.
# By Noah C. Benson

"""Garbage-collection tests.

Collections can take part in reference cycles (an object that holds a
collection that holds the object), and such cycles must be collected even
when several versions of a collection share parts of their structure. For the
C backend, these tests also check the trie nodes' tracking rule: a node is
tracked by the collector exactly when it can reach an object the collector
might free.
"""

import gc
import random
import weakref

from ._backends import make_tests


class _Owner:
    """A plain object that can sit in a reference cycle."""
    def __init__(self):
        self.ref = None


class _GCTests:
    """Instantiated once per backend by ``make_tests``."""

    def trie_stats(self, coll):
        """(nodes, tracked, violations) for the C backend; None otherwise."""
        if self.backend_name != 'c':
            return None
        from .._c import _core
        return _core._trie_stats(coll)

    def assert_collected(self, make_cycle):
        """make_cycle(owner) stores a collection referring to owner on owner;
        after dropping owner, the collector must free it."""
        gc.collect()
        owner = _Owner()
        make_cycle(owner)
        wr = weakref.ref(owner)
        del owner
        gc.collect()
        self.assertIsNone(wr(), "reference cycle was not collected")

    def test_cycle_through_each_type(self):
        n = 3000  # large enough for multi-level tries
        makers = {
            'pdict': lambda o: self.pdict({i: (o if i == 7 else i)
                                           for i in range(n)}),
            'tdict': lambda o: self.tdict({i: (o if i == 7 else i)
                                           for i in range(n)}),
            'pset':  lambda o: self.pset([(o,)] + list(range(n))),
            'tset':  lambda o: self.tset([(o,)] + list(range(n))),
            'plist': lambda o: self.plist([o] + list(range(n))),
            'tlist': lambda o: self.tlist([o] + list(range(n))),
        }
        for name, make in makers.items():
            with self.subTest(type=name):
                def cycle(o, make=make):
                    o.ref = make(o)
                self.assert_collected(cycle)

    def test_cycle_through_shared_versions(self):
        n = 3000
        def cycle(o):
            base = self.pdict({i: (o if i == 0 else i) for i in range(n)})
            o.ref = [base.set(n - 1, k) for k in range(5)]
        self.assert_collected(cycle)
        def cycle(o):
            base = self.plist([o] + list(range(n)))
            o.ref = [base.set(n - 1, k) for k in range(5)] + [base]
        self.assert_collected(cycle)

    def test_cycle_through_transient_and_snapshot(self):
        n = 3000
        def cycle(o):
            t = self.tdict((i, i) for i in range(n))
            t[5] = o
            p = t.persistent()
            t[6] = 'changed'
            o.ref = (t, p)
        self.assert_collected(cycle)
        def cycle(o):
            t = self.pdict((i, i) for i in range(n)).transient()
            t[n // 2] = o
            o.ref = t
        self.assert_collected(cycle)

    def test_cycle_through_lazy(self):
        def cycle(o):
            o.ref = self.ldict(a=self.lazy(lambda: o), b=1)
        self.assert_collected(cycle)
        def cycle(o):
            ll = self.llist([self.lazy(lambda: o)] * 100)
            ll[0]  # compute one
            o.ref = ll
        self.assert_collected(cycle)

    def test_live_items_survive_collection(self):
        # Items referenced from outside survive, however many versions share
        # the nodes that hold them.
        n = 2000
        for k in range(1, 6):
            item = [1, 2, 3]
            base = self.pdict({i: (item if i == 0 else i) for i in range(n)})
            versions = [base.set(n - 1, j) for j in range(k)]
            del base
            junk = [versions]
            junk.append(junk)
            del versions, junk
            gc.collect()
            self.assertEqual(item, [1, 2, 3])

    def test_deep_nesting_releases(self):
        # Releasing a deeply nested structure must not exhaust the C stack.
        x = self.plist()
        for _ in range(100000):
            x = self.plist([x])
        del x
        d = self.pdict()
        for _ in range(100000):
            d = self.pdict(a=d)
        del d

    # Tracking rule (C backend) ---------------------------------------------
    def test_atomic_contents_are_untracked(self):
        if self.backend_name != 'c':
            self.skipTest("C backend only")
        n = 3000
        for coll in (self.pdict((i, str(i)) for i in range(n)),
                     self.pset(range(n)),
                     self.plist(range(n)),
                     self.plist([1.5, None, 'x', b'y'] * n)):
            nodes, tracked, bad = self.trie_stats(coll)
            self.assertGreater(nodes, 1)
            self.assertEqual((tracked, bad), (0, 0), type(coll))
        # One container anywhere makes its path tracked, and only its path.
        d = self.pdict((i, i) for i in range(n)).set(n // 2, [])
        nodes, tracked, bad = self.trie_stats(d)
        self.assertEqual(bad, 0)
        self.assertTrue(0 < tracked < nodes)

    def test_tracking_rule_under_random_updates(self):
        if self.backend_name != 'c':
            self.skipTest("C backend only")
        rng = random.Random(12345)
        values = [0, 'a', 2.5, None, (), (1,), [], {}, frozenset()]
        for kind in ('dict', 'set', 'list'):
            p = getattr(self, 'p' + kind)()
            t = getattr(self, 't' + kind)()
            for step in range(4000):
                v = rng.choice(values)
                k = rng.randrange(1500)
                if kind == 'dict':
                    if rng.random() < 0.7:
                        p = p.set(k, v)
                        t[k] = v
                    else:
                        p = p.drop(k)
                        t.pop(k, None)
                elif kind == 'set':
                    item = (k, v) if not isinstance(v, (list, dict)) else k
                    if rng.random() < 0.7:
                        p = p.add(item)
                        t.add(item)
                    else:
                        p = p.discard(item)
                        t.discard(item)
                else:
                    r = rng.random()
                    if r < 0.5 or len(t) == 0:
                        p = p.append(v)
                        t.append(v)
                    elif r < 0.8:
                        i = rng.randrange(len(t))
                        p = p.set(i, v)
                        t[i] = v
                    else:
                        p = p.delete(-1)
                        t.pop()
                if step % 250 == 0:
                    for coll in (p, t, t.persistent()):
                        self.assertEqual(self.trie_stats(coll)[2], 0,
                                         (kind, step))
            for coll in (p, t, t.persistent()):
                self.assertEqual(self.trie_stats(coll)[2], 0, kind)

    def test_collection_during_updates(self):
        # With the collector running at nearly every allocation, it sees
        # nodes in every intermediate state an update passes through.
        if self.backend_name != 'c':
            self.skipTest("C backend only")
        rng = random.Random(7)
        values = [0, 'x', [], (1,), {}]
        old = gc.get_threshold()
        gc.set_threshold(1, 1, 1)
        try:
            p, t, ref = self.pdict(), self.tdict(), {}
            snaps = []
            for step in range(600):
                k = rng.randrange(300)
                v = rng.choice(values)
                if rng.random() < 0.7:
                    p = p.set(k, v)
                    t[k] = v
                    ref[k] = v
                else:
                    p = p.drop(k)
                    t.pop(k, None)
                    ref.pop(k, None)
                if step % 50 == 0:
                    snaps = (snaps + [t.persistent()])[-3:]
            l, tl = self.plist(), self.tlist()
            for step in range(600):
                v = rng.choice(values)
                if rng.random() < 0.6 or not tl:
                    l = l.append(v)
                    tl.append(v)
                else:
                    l = l.delete(0)
                    del tl[0]
        finally:
            gc.set_threshold(*old)
        self.assertEqual(dict(p.items()), ref)
        self.assertEqual(dict(t.items()), ref)
        self.assertEqual(list(l), list(tl))
        for coll in [p, t, l, tl] + snaps:
            self.assertEqual(self.trie_stats(coll)[2], 0)

    def test_untracked_nodes_are_not_gc_objects(self):
        if self.backend_name != 'c':
            self.skipTest("C backend only")
        gc.collect()
        before = len(gc.get_objects())
        keep = [self.pdict((i, i) for i in range(5000)) for _ in range(20)]
        after = len(gc.get_objects())
        # 20 pdicts (and a few helpers) are tracked; their nodes are not.
        self.assertLess(after - before, 100)
        del keep


make_tests('TestGC', _GCTests, globals())
