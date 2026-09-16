# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_trie.py
# Structural tests for pcollections._trie's AMT/TAMT/FAT/TFAT classes, the
# pure-Python counterparts of pcollections/_c/amt.h and fat.h (see
# pcollections/_trie.py's module comment for their design).
#
# Unlike test/_dict.py/_list.py/_set.py, which run against both backends via
# _backends.make_tests, these tests use pcollections._trie directly: the C
# tries are not exposed as Python objects. They cover structural invariants,
# edge cases, and randomized comparison against a plain dict.
# By Noah C. Benson

import random
import threading
import unittest

from .._trie import AMT, TAMT, FAT, TFAT, MASK_WIDTH


class _TrieTestMixin:
    """Shared structural tests, run once against (AMT, TAMT) and once
    against (FAT, TFAT) -- see TestAMT/TestFAT below. FAT uses the same
    algorithm as AMT (see pcollections/_trie.py's module comment), so the
    two are expected to behave identically here; TestFAT additionally checks
    the dense, non-negative-key case FAT is used for in _dict.py/_set.py
    (ascending order == insertion order; see
    test_dense_ascending_order)."""

    # Set by subclasses.
    persistent_cls = None
    transient_cls = None

    def test_empty_singleton(self):
        cls = self.persistent_cls
        self.assertIs(cls.empty, cls())
        self.assertEqual(len(cls.empty), 0)
        self.assertFalse(cls.empty)
        self.assertNotIn(0, cls.empty)
        self.assertIsNone(cls.empty.get(0))
        self.assertEqual(cls.empty.get(0, 'x'), 'x')
        with self.assertRaises(KeyError):
            cls.empty[0]

    def test_assoc_get_basic(self):
        cls = self.persistent_cls
        a = cls.empty
        a1 = a.assoc(1, 'one')
        self.assertEqual(len(a1), 1)
        self.assertEqual(a1.get(1), 'one')
        self.assertEqual(a1[1], 'one')
        self.assertIn(1, a1)
        # The original is untouched (persistence).
        self.assertEqual(len(a), 0)
        self.assertNotIn(1, a)
        # Overwriting an existing key doesn't change the length.
        a2 = a1.assoc(1, 'uno')
        self.assertEqual(len(a2), 1)
        self.assertEqual(a2[1], 'uno')
        self.assertEqual(a1[1], 'one')  # a1 is still untouched

    def test_dissoc_basic(self):
        cls = self.persistent_cls
        a = cls.empty.assoc(1, 'a').assoc(2, 'b').assoc(3, 'c')
        a2 = a.dissoc(2)
        self.assertEqual(len(a2), 2)
        self.assertNotIn(2, a2)
        self.assertEqual(a2.get(1), 'a')
        self.assertEqual(a2.get(3), 'c')
        # dissoc of an absent key is a no-op (mirrors amt_butitem()).
        self.assertIs(a2.dissoc(999), a2)
        # a is untouched.
        self.assertEqual(len(a), 3)
        self.assertIn(2, a)
        # Draining to nothing converges on the canonical empty singleton.
        drained = a.dissoc(1).dissoc(2).dissoc(3)
        self.assertIs(drained, cls.empty)

    def test_structural_sharing(self):
        # A chain of persistent updates never mutates any earlier version.
        cls = self.persistent_cls
        versions = [cls.empty]
        for i in range(300):
            versions.append(versions[-1].assoc(i, i * i))
        for i, v in enumerate(versions):
            self.assertEqual(len(v), i)
            for k in range(i):
                self.assertEqual(v.get(k), k * k)
            for k in range(i, 300):
                self.assertNotIn(k, v)

    def test_persistent_stress_vs_dict(self):
        cls = self.persistent_cls
        rnd = random.Random(1234)
        ref = {}
        t = cls.empty
        keyrange = self._keyrange()
        for _ in range(4000):
            k = rnd.choice(keyrange)
            if rnd.random() < 0.7:
                v = rnd.random()
                ref[k] = v
                t = t.assoc(k, v)
            else:
                ref.pop(k, None)
                t = t.dissoc(k)
            self.assertEqual(len(t), len(ref))
        for k, v in ref.items():
            self.assertEqual(t.get(k), v)
            self.assertIn(k, t)
        for k in keyrange:
            if k not in ref:
                self.assertNotIn(k, t)

    def test_transient_claim_discipline(self):
        # Mutating a TAMT/TFAT built from a persistent snapshot never
        # mutates that snapshot (copy-on-write "claiming" -- see
        # pcollections/_trie.py's module comment), and, symmetrically,
        # mutating the transient further after calling .persistent() never
        # mutates the persistent snapshot just handed out.
        pcls, tcls = self.persistent_cls, self.transient_cls
        base = pcls.empty
        for i in range(50):
            base = base.assoc(i, i)
        t = tcls(base)
        for i in range(50, 100):
            t[i] = i
        # base is unchanged.
        self.assertEqual(len(base), 50)
        for i in range(50, 100):
            self.assertNotIn(i, base)
        snap = t.persistent()
        self.assertEqual(len(snap), 100)
        # Further mutation of t must not affect snap.
        for i in range(100, 150):
            t[i] = i
        del t[0]
        self.assertEqual(len(snap), 100)
        self.assertIn(0, snap)
        for i in range(100, 150):
            self.assertNotIn(i, snap)
        self.assertEqual(len(t), 149)
        self.assertNotIn(0, t)

    def test_transient_stress_vs_dict(self):
        tcls = self.transient_cls
        rnd = random.Random(5678)
        ref = {}
        t = tcls()
        keyrange = self._keyrange()
        for _ in range(4000):
            k = rnd.choice(keyrange)
            if rnd.random() < 0.7:
                v = rnd.random()
                ref[k] = v
                t[k] = v
            else:
                if k in ref:
                    del ref[k]
                    del t[k]
                else:
                    with self.assertRaises(KeyError):
                        del t[k]
            self.assertEqual(len(t), len(ref))
        for k, v in ref.items():
            self.assertEqual(t[k], v)
        p = t.persistent()
        for k, v in ref.items():
            self.assertEqual(p.get(k), v)
        self.assertEqual(len(p), len(ref))

    def test_transient_delitem_missing_raises(self):
        tcls = self.transient_cls
        t = tcls()
        t[1] = 'a'
        with self.assertRaises(KeyError):
            del t[2]
        del t[1]
        with self.assertRaises(KeyError):
            del t[1]

    def test_equality(self):
        cls = self.persistent_cls
        a1 = cls.empty.assoc(1, 2).assoc(3, 4)
        a2 = cls.empty.assoc(3, 4).assoc(1, 2)
        self.assertEqual(a1, a2)
        self.assertEqual(hash(a1), hash(a2))
        a3 = a1.assoc(5, 6)
        self.assertNotEqual(a1, a3)

    def test_deep_shared_prefix_keys(self):
        # Keys that agree on almost every bit (differing only in the very
        # lowest few) exercise the deepest part of the trie (near/at twig
        # depth) and its path-compression (amt_subjoin()-equivalent) logic.
        cls = self.persistent_cls
        base = 0xDEADBEEF00 << 8
        t = cls.empty
        for i in range(40):
            t = t.assoc(base + i, i)
        self.assertEqual(len(t), 40)
        for i in range(40):
            self.assertEqual(t.get(base + i), i)
        t2 = t
        for i in range(0, 40, 2):
            t2 = t2.dissoc(base + i)
        self.assertEqual(len(t2), 20)
        for i in range(40):
            if i % 2 == 0:
                self.assertNotIn(base + i, t2)
            else:
                self.assertEqual(t2.get(base + i), i)

    def _keyrange(self):
        return list(range(-500, 500))


class TestAMT(_TrieTestMixin, unittest.TestCase):
    persistent_cls = AMT
    transient_cls = TAMT

    def test_negative_keys_and_masking(self):
        # AMT keys are masked to an unsigned MASK_WIDTH-bit representation
        # (see AMT._normalize_key() and the "Iteration order" section of
        # pcollections/_trie.py's module comment) -- get/assoc/dissoc all
        # normalize the same way, so lookups stay correct for negative keys
        # even though the *iterated* key differs from the original signed
        # one.
        a = AMT.empty.assoc(-1, 'neg-one').assoc(1, 'pos-one')
        self.assertEqual(a.get(-1), 'neg-one')
        self.assertEqual(a.get(1), 'pos-one')
        self.assertEqual(len(a), 2)
        # The masked representation of -1 is MASK_WIDTH bits of 1s.
        keys = dict(iter(a))
        self.assertIn(MASK_WIDTH, keys)
        self.assertEqual(keys[MASK_WIDTH], 'neg-one')

    def test_ascending_order_within_signed_runs(self):
        # See pcollections/_trie.py's "Iteration order" section: ascending
        # order is guaranteed only within a run of same-signed keys, which
        # is what _list.py's plist.__iter__ assumes.
        rnd = random.Random(99)
        negkeys = list(range(-40, 0))
        rnd.shuffle(negkeys)
        t = AMT.empty
        for k in negkeys:
            t = t.assoc(k, k)
        self.assertEqual([v for (_k, v) in t], sorted(range(-40, 0)))

        poskeys = list(range(0, 40))
        rnd.shuffle(poskeys)
        t2 = AMT.empty
        for k in poskeys:
            t2 = t2.assoc(k, k)
        self.assertEqual([v for (_k, v) in t2], sorted(range(0, 40)))


class TestFAT(_TrieTestMixin, unittest.TestCase):
    persistent_cls = FAT
    transient_cls = TFAT

    def _keyrange(self):
        # FAT's role (pdict/pset's `_els`) only ever sees dense,
        # non-negative keys.
        return list(range(0, 1000))

    def test_dense_ascending_order(self):
        # FAT's role in _dict.py/_set.py (the `_els` insertion-order
        # value table) depends on this: iterating in ascending index order
        # must reproduce insertion order.
        rnd = random.Random(42)
        n = 2000
        vals = [rnd.random() for _ in range(n)]
        t = TFAT(FAT.empty)
        order = list(range(n))
        rnd.shuffle(order)
        for i in order:
            t[i] = vals[i]
        p = t.persistent()
        self.assertEqual([v for (_k, v) in p], vals)
        self.assertEqual([k for (k, _v) in p], list(range(n)))

    def test_is_distinct_type_from_amt(self):
        # FAT/AMT share an implementation (see pcollections/_trie.py) but
        # are distinct classes.
        self.assertTrue(issubclass(FAT, AMT))
        self.assertIsNot(FAT, AMT)
        self.assertIsNot(FAT.empty, AMT.empty)
        self.assertIsInstance(FAT.empty, AMT)  # FAT IS-A AMT structurally
        self.assertNotIsInstance(AMT.empty, FAT)  # but not vice versa


class TestTrieThreadStress(unittest.TestCase):
    """Multithreaded stress test: reader threads read a shared persistent
    snapshot (checking that no persistent operation mutates shared state)
    while a single owner thread mutates a transient trie. On a GIL build this
    does not exercise true parallel races (see pcollections/_trie.py's
    thread-safety comment for the free-threaded argument), but it does run
    the single-owner-transient / many-snapshot-readers pattern under
    concurrent scheduling, looking for crashes or corrupted results."""

    def test_concurrent_readers_of_persistent_snapshot(self):
        base = AMT.empty
        ref = {}
        for i in range(2000):
            base = base.assoc(i, i * i)
            ref[i] = i * i
        errors = []
        def reader():
            try:
                for _ in range(200):
                    for i in (0, 999, 1999, 500):
                        if base.get(i) != ref[i]:
                            errors.append((i, base.get(i), ref[i]))
                    self.assertEqual(len(base), len(ref))
            except Exception as e:  # pragma: no cover - failure path
                errors.append(e)
        threads = [threading.Thread(target=reader) for _ in range(8)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        self.assertEqual(errors, [])

    def test_single_owner_transient_under_concurrent_readers(self):
        # One thread owns and mutates a TAMT; other threads concurrently
        # read *persistent snapshots* taken from it via .persistent() (never
        # the transient itself, which would violate the single-owner
        # contract). Each snapshot must be internally
        # consistent even while the owner keeps mutating afterward.
        t = TAMT(AMT.empty)
        errors = []
        snapshots = []
        lock = threading.Lock()

        def owner():
            for i in range(3000):
                t[i] = i
                if i % 100 == 0:
                    with lock:
                        snapshots.append((i, t.persistent()))

        def reader():
            try:
                for _ in range(50):
                    with lock:
                        pairs = list(snapshots)
                    for (i, snap) in pairs:
                        for k in range(0, i + 1, max(1, i // 20 or 1)):
                            v = snap.get(k)
                            if v is not None and v != k:
                                errors.append((k, v))
            except Exception as e:  # pragma: no cover - failure path
                errors.append(e)

        owner_thread = threading.Thread(target=owner)
        reader_threads = [threading.Thread(target=reader) for _ in range(4)]
        owner_thread.start()
        for th in reader_threads:
            th.start()
        owner_thread.join()
        for th in reader_threads:
            th.join()
        self.assertEqual(errors, [])
        final = t.persistent()
        self.assertEqual(len(final), 3000)
        for i in range(3000):
            self.assertEqual(final.get(i), i)


class TestDictSetCompaction(unittest.TestCase):
    """Directly exercises pdict/tdict's and pset/tset's tombstone-based
    deletion + periodic compaction (pcollections/_compact.py), mirroring
    dict_should_compact()/dict_rebuild_compacted() in _c/dict.c.h -- see that
    file's header comment and _compact.py's header comment. These use
    pcollections._dict/_set directly, since `_top`/`_ndeleted` are
    pure-Python implementation details the C pdict/pset don't expose.
    test/_dict.py's and _set.py's test_random exercise compaction
    incidentally; these tests force sustained churn well past the threshold
    and check the bookkeeping itself."""

    def test_dict_compaction_bounds_top_and_resets_ndeleted(self):
        from .._dict import tdict
        rnd = random.Random(2024)
        t = tdict.empty()
        ref = {}
        # Churn: insert, then immediately delete most of what was just
        # inserted, many times over -- `_top` would grow unboundedly across
        # this many cycles without compaction ever kicking in.
        for cycle in range(50):
            for i in range(200):
                k = (cycle, i)
                v = rnd.random()
                t[k] = v
                ref[k] = v
            keys = list(ref.keys())
            rnd.shuffle(keys)
            for k in keys[:180]:
                del t[k]
                del ref[k]
            self.assertEqual(len(t), len(ref))
            for k, v in ref.items():
                self.assertEqual(t[k], v)
        # Compaction must have kept `_top` from growing anywhere near the
        # ~10000 total insertions made across all 50 cycles.
        self.assertLess(t._top, 2000)
        self.assertEqual(t._count, len(ref))
        p = t.persistent()
        self.assertEqual(len(p), len(ref))
        for k, v in ref.items():
            self.assertEqual(p.get(k), v)
        # A dict.items()/keys()/values()/hash()/repr() that all skip
        # tombstones correctly, post-compaction.
        self.assertEqual(dict(p.items()), ref)
        self.assertEqual(set(p.keys()), set(ref.keys()))
        self.assertIsInstance(hash(p), int)
        self.assertIn(str(p)[0], '{')

    def test_set_compaction_bounds_top_and_resets_ndeleted(self):
        from .._set import tset
        rnd = random.Random(7)
        t = tset.empty()
        ref = set()
        for cycle in range(50):
            batch = [(cycle, i) for i in range(200)]
            for x in batch:
                t.add(x)
                ref.add(x)
            elems = list(ref)
            rnd.shuffle(elems)
            for x in elems[:180]:
                t.discard(x)
                ref.discard(x)
            self.assertEqual(len(t), len(ref))
            for x in ref:
                self.assertIn(x, t)
        self.assertLess(t._top, 2000)
        self.assertEqual(t._count, len(ref))
        p = t.persistent()
        self.assertEqual(set(p), ref)
        self.assertIsInstance(hash(p), int)

    def test_pdict_drop_pop_compaction_matches_dict(self):
        from .._dict import pdict
        rnd = random.Random(3)
        p = pdict.empty
        ref = {}
        for cycle in range(30):
            for i in range(100):
                k = (cycle, i)
                v = rnd.random()
                p = p.set(k, v)
                ref[k] = v
            keys = list(ref.keys())
            rnd.shuffle(keys)
            for k in keys[:90]:
                if rnd.random() < 0.5:
                    p = p.drop(k)
                else:
                    (val, p) = p.pop(k)
                    self.assertEqual(val, ref[k])
                del ref[k]
            self.assertEqual(dict(p.items()), ref)
        self.assertLess(p._top, 1500)


def suite():
    loader = unittest.TestLoader()
    import sys
    return loader.loadTestsFromModule(sys.modules[__name__])
