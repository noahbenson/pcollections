# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_lazy.py
# Tests of lazy values and the lazy collections, run against every backend.
# By Noah C. Benson

"""Tests of `lazy`, `LazyError`, and the lazy collections.

The lazy collections follow one rule: a read from a lazy collection computes
the lazy values it returns, and lazy values in any other collection are
ordinary objects.
"""

import gc
import pickle
import sys
import threading
import time
import weakref

from ._backends import make_tests


class _Counter:
    """A function that counts its calls."""
    def __init__(self):
        self.calls = []
    def __call__(self, x):
        self.calls.append(x)
        return x * 10


class _ExplodingError(Exception):
    pass


def _logged_class(base, suffix):
    """A lazy subclass that logs its calls, defined at module level (so that
    it can be pickled)."""
    name = f"_Logged_{suffix}"
    cls = globals().get(name)
    if cls is None:
        class Logged(base):
            log = []
            def __call__(self):
                type(self).log.append('call')
                return ('logged', super().__call__())
        Logged.__name__ = Logged.__qualname__ = name
        Logged.__module__ = __name__
        globals()[name] = cls = Logged
    return cls


class _LazyTests:
    """Instantiated once per backend by ``make_tests``."""

    # lazy ------------------------------------------------------------------
    def test_computes_once(self):
        f = _Counter()
        l = self.lazy(f, 4)
        self.assertFalse(l.is_ready())
        self.assertEqual(l(), 40)
        self.assertEqual(l(), 40)
        self.assertTrue(l.is_ready())
        self.assertEqual(f.calls, [4])
        self.assertIn('ready', repr(l))

    def test_keyword_arguments(self):
        l = self.lazy(int, '11', base=2)
        self.assertEqual(l(), 3)

    def test_constructor_errors(self):
        with self.assertRaises(TypeError):
            self.lazy()
        with self.assertRaises(TypeError):
            self.lazy(5)
        with self.assertRaises(TypeError):
            self.lazy(int)(1)

    def test_releases_function_once_computed(self):
        class Big:
            pass
        big = Big()
        ref = weakref.ref(big)
        l = self.lazy(lambda b: 1, big)
        del big
        self.assertIsNotNone(ref())
        l()
        gc.collect()
        self.assertIsNone(ref())

    def test_failure_is_remembered(self):
        calls = []
        def fail():
            calls.append(1)
            raise _ExplodingError('boom')
        l = self.lazy(fail)
        errors = []
        for _ in range(3):
            with self.assertRaises(self.LazyError) as cm:
                l()
            errors.append(cm.exception)
        self.assertEqual(calls, [1])
        self.assertFalse(l.is_ready())
        self.assertIn('failed', repr(l))
        # A new error each time, all caused by the same exception.
        self.assertEqual(len({id(e) for e in errors}), 3)
        causes = {id(e.__cause__) for e in errors}
        self.assertEqual(len(causes), 1)
        e = errors[0]
        self.assertIsInstance(e.__cause__, _ExplodingError)
        self.assertIs(e.cause, e.__cause__)
        self.assertIsInstance(e, RuntimeError)
        self.assertIs(e.func, fail)
        self.assertEqual(tuple(e.func_args), ())
        self.assertEqual(dict(e.func_kwargs), {})
        # The message names where the lazy value was created and the call.
        (filename, lineno, funcname) = e.origin
        self.assertEqual(filename, __file__)
        self.assertEqual(funcname, 'test_failure_is_remembered')
        msg = str(e)
        self.assertIn('_lazy.py:', msg)
        self.assertIn('fail()', msg)
        self.assertIn('_ExplodingError: boom', msg)
        self.assertIsNone(e.origin_stack)

    def test_error_message_arguments(self):
        l = self.lazy(int, 'z' * 500, base=10)
        with self.assertRaises(self.LazyError) as cm:
            l()
        msg = str(cm.exception)
        self.assertIn("int('zzz", msg)
        self.assertIn('base=10', msg)
        self.assertNotIn(',,', msg)
        self.assertLess(len(msg), 800)
        self.assertEqual(cm.exception.func_args, ('z' * 500,))
        self.assertEqual(cm.exception.func_kwargs, {'base': 10})

    def test_traceback_does_not_grow(self):
        l = self.lazy(lambda: 1 / 0)
        lengths = []
        for _ in range(5):
            try:
                with self.lazy_error_unwrap:
                    l()
            except ZeroDivisionError as e:
                tb, n = e.__traceback__, 0
                while tb is not None:
                    tb, n = tb.tb_next, n + 1
                lengths.append(n)
                self.assertIsNone(e.__context__)
        self.assertEqual(len(set(lengths)), 1, lengths)

    def test_base_exceptions_leave_value_pending(self):
        state = {'n': 0}
        def interrupted():
            state['n'] += 1
            if state['n'] == 1:
                raise KeyboardInterrupt
            return 'done'
        l = self.lazy(interrupted)
        with self.assertRaises(KeyboardInterrupt):
            l()
        self.assertFalse(l.is_ready())
        self.assertEqual(l(), 'done')

    def test_self_dependency(self):
        box = []
        l = self.lazy(lambda: box[0]() + 1)
        box.append(l)
        with self.assertRaises(self.LazyError) as cm:
            l()
        err = cm.exception
        # The inner request raises "depends on itself", which then fails the
        # value.
        self.assertIsInstance(err.__cause__, self.LazyError)
        self.assertIn('depends on itself', str(err.__cause__))
        self.assertIsNone(err.__cause__.__cause__)
        with self.assertRaises(self.LazyError):
            l()

    def test_threads_compute_once(self):
        calls = []
        def slow():
            calls.append(1)
            time.sleep(0.1)
            return 'value'
        l = self.lazy(slow)
        results = []
        def worker():
            results.append(l())
        threads = [threading.Thread(target=worker) for _ in range(8)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        self.assertEqual(calls, [1])
        self.assertEqual(results, ['value'] * 8)

    def test_threads_share_failure(self):
        calls = []
        def slow_fail():
            calls.append(1)
            time.sleep(0.05)
            raise ValueError('no')
        l = self.lazy(slow_fail)
        causes = []
        def worker():
            try:
                l()
            except self.LazyError as e:
                causes.append(e.__cause__)
        threads = [threading.Thread(target=worker) for _ in range(6)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        self.assertEqual(calls, [1])
        self.assertEqual(len(causes), 6)
        self.assertEqual(len({id(c) for c in causes}), 1)

    def test_trace(self):
        lazy = self.lazy
        old = lazy.trace
        try:
            lazy.trace = True
            l = lazy(lambda: 1 / 0)
        finally:
            lazy.trace = old
        with self.assertRaises(self.LazyError) as cm:
            l()
        stack = cm.exception.origin_stack
        self.assertIsInstance(stack, list)
        self.assertTrue(any('test_trace' in line for line in stack))

    def test_unwrap(self):
        LazyError = self.LazyError
        unwrap = self.lazy_error_unwrap
        l = self.lazy(lambda: [][1])
        try:
            l()
        except LazyError as e:
            self.assertIsInstance(unwrap(e), IndexError)
        other = ValueError()
        self.assertIs(unwrap(other), other)
        plain = LazyError('plain')
        self.assertIs(unwrap(plain), plain)
        with self.assertRaises(IndexError):
            with unwrap:
                l()
        with self.assertRaises(KeyError):
            with unwrap:
                raise KeyError('x')
        # The same objects are used by both backends.
        import pcollections
        self.assertIs(LazyError, pcollections.LazyError)
        self.assertIs(unwrap, pcollections.lazy_error_unwrap)

    def test_dependency_failure(self):
        LazyError = self.LazyError
        unwrap = self.lazy_error_unwrap
        def load(name):
            raise FileNotFoundError(f"no such file: {name}")
        base = self.ldict(v=self.lazy(load, 'c.csv'))
        mid = self.ldict(m=self.lazy(lambda: base['v'] * 2))
        top = self.ldict(t=self.lazy(lambda: mid['m'] + 1))
        with self.assertRaises(LazyError) as cm:
            top['t']
        err = cm.exception
        # The causes follow the dependencies down to the original exception.
        self.assertIsInstance(err.cause, LazyError)
        self.assertIsInstance(err.cause.cause, LazyError)
        self.assertIsInstance(err.root_cause, FileNotFoundError)
        self.assertIs(err.cause.cause.cause, err.root_cause)
        self.assertIs(err.cause.root_cause, err.root_cause)
        # The message names the failed dependency and the root cause once.
        msg = str(err)
        self.assertIn('depends on a lazy value', msg)
        self.assertEqual(msg.count('no such file: c.csv'), 1)
        self.assertEqual(msg.count('LazyError'), 0)
        # Unwrapping gives the root cause, as a function and as a context
        # manager, with the traceback of the original failure.
        self.assertIs(unwrap(err), err.root_cause)
        lengths = []
        for _ in range(3):
            try:
                with unwrap:
                    top['t']
            except FileNotFoundError as e:
                self.assertIs(e, err.root_cause)
                self.assertIsNone(e.__context__)
                tb, n = e.__traceback__, 0
                while tb is not None:
                    tb, n = tb.tb_next, n + 1
                lengths.append(n)
        self.assertEqual(len(lengths), 3)
        self.assertEqual(len(set(lengths)), 1, lengths)

    def test_deep_dependency_message(self):
        def fail():
            raise ValueError('bad input')
        prev = self.lazy(fail)
        for _ in range(30):
            prev = self.lazy(lambda p=prev: p() + 1)
        with self.assertRaises(self.LazyError) as cm:
            prev()
        err = cm.exception
        self.assertLess(len(str(err)), 800)
        self.assertEqual(str(err).count('bad input'), 1)
        self.assertIsInstance(err.root_cause, ValueError)
        self.assertIsInstance(self.lazy_error_unwrap(err), ValueError)

    def test_root_cause_without_root(self):
        LazyError = self.LazyError
        unwrap = self.lazy_error_unwrap
        box = []
        l = self.lazy(lambda: box[0]() + 1)
        box.append(l)
        dep = self.lazy(lambda: l() * 2)
        with self.assertRaises(LazyError) as cm:
            dep()
        err = cm.exception
        # The chain ends in the "depends on itself" error, which has no cause.
        self.assertIsNone(err.root_cause)
        self.assertIn('depends on itself', str(err))
        self.assertIs(unwrap(err), err)
        with self.assertRaises(LazyError):
            with unwrap:
                dep()
        # LazyErrors raised by other code work too.
        plain = LazyError('plain')
        self.assertIsNone(plain.root_cause)
        try:
            try:
                raise KeyError('k')
            except KeyError as e:
                raise LazyError('wrapped') from e
        except LazyError as e:
            self.assertIsInstance(e.root_cause, KeyError)
            self.assertIsInstance(unwrap(e), KeyError)
            with self.assertRaises(KeyError):
                with unwrap:
                    raise e

    def test_subclass(self):
        lazy = self.lazy
        Logged = _logged_class(lazy, self.backend_name)
        l = Logged(int, '5')
        self.assertIsInstance(l, lazy)
        self.assertEqual(l(), ('logged', 5))
        self.assertTrue(l.is_ready())
        # Reads from lazy collections and unlazy go through __call__.
        Logged.log.clear()
        ld = self.ldict(a=Logged(int, '6'))
        self.assertEqual(ld['a'], ('logged', 6))
        self.assertEqual(self.unlazy(Logged(int, '7')), ('logged', 7))
        ll = self.llist([Logged(int, '8')])
        self.assertEqual(list(ll), [('logged', 8)])
        self.assertTrue(ll.is_lazy(0))
        self.assertTrue(ld.is_lazy('a'))
        self.assertEqual(len(Logged.log), 3)
        # Subclass instances can carry attributes and pickle them.
        l = Logged(int, '9')
        l.tag = 'x'
        back = pickle.loads(pickle.dumps(l))
        self.assertIs(type(back), Logged)
        self.assertEqual(back.tag, 'x')

    def test_pickle(self):
        lazy = self.lazy
        l = lazy(int, '12')
        self.assertFalse(l.is_ready())
        back = pickle.loads(pickle.dumps(l))
        # Pickling computes the value.
        self.assertTrue(l.is_ready())
        self.assertTrue(back.is_ready())
        self.assertIs(type(back), lazy)
        self.assertEqual(back(), 12)
        bad = lazy(int, 'x')
        with self.assertRaises(self.LazyError):
            pickle.dumps(bad)
        self.assertIs(pickle.loads(pickle.dumps(self.lazy_error_unwrap)),
                      self.lazy_error_unwrap)

    def test_helpers(self):
        l = self.lazy(int, '3')
        self.assertEqual(self.unlazy(l), 3)
        self.assertEqual(self.unlazy(4), 4)
        self.assertEqual(self.holdlazy(5), 5)
        with self.assertRaises(TypeError):
            self.holdlazy(5, require_lazy=True)
        # holdlazy does not hide errors raised by __holdlazy__.
        class Weird:
            def __holdlazy__(self):
                raise AttributeError('inner')
        with self.assertRaises(AttributeError):
            self.holdlazy(Weird())

    # The convention ------------------------------------------------------
    def test_lazies_are_plain_objects_elsewhere(self):
        f = _Counter()
        l = self.lazy(f, 1)
        for make in (lambda: self.pdict(dict(x=l))['x'],
                     lambda: self.pdict(x=l)['x'],
                     lambda: self.tdict(x=l)['x'],
                     lambda: self.plist([l])[0],
                     lambda: list(self.plist([l]))[0],
                     lambda: self.tlist([l])[0],
                     lambda: list(self.pset([l]))[0],
                     lambda: list(self.pdict(x=l).values())[0]):
            self.assertIs(make(), l)
        self.assertEqual(f.calls, [])

    def test_reads_from_lazy_collections_compute(self):
        lazy = self.lazy
        def ld():
            return self.ldict(a=lazy(int, '1'), b=2)
        self.assertEqual(ld()['a'], 1)
        self.assertEqual(ld().get('a'), 1)
        self.assertEqual(list(ld().values()), [1, 2])
        self.assertEqual(list(ld().items()), [('a', 1), ('b', 2)])
        self.assertIn(1, ld().values())
        self.assertIn(('a', 1), ld().items())
        self.assertEqual(ld(), {'a': 1, 'b': 2})
        self.assertEqual(hash(ld()), hash(self.ldict(a=1, b=2)))
        self.assertEqual(ld().pop('a')[0], 1)
        t = ld().transient()
        self.assertEqual(t['a'], 1)
        t = ld().transient()
        self.assertEqual(t.pop('a'), 1)
        t = ld().transient()
        self.assertEqual(t.get('a'), 1)
        self.assertEqual(dict(ld().transient().items()), {'a': 1, 'b': 2})
        def ll():
            return self.llist([lazy(int, '1'), 2])
        self.assertEqual(ll()[0], 1)
        self.assertEqual(ll()[-2], 1)
        self.assertEqual(list(ll()), [1, 2])
        self.assertEqual(ll(), [1, 2])
        self.assertIn(1, ll())
        self.assertEqual(ll().index(1), 0)
        self.assertEqual(ll().count(1), 1)
        self.assertEqual(hash(ll()), hash(self.llist([1, 2])))
        self.assertEqual(ll().pop(0)[0], 1)
        t = ll().transient()
        self.assertEqual(t.pop(0), 1)
        self.assertEqual(list(ll().transient()), [1, 2])
        # A slice of a lazy list is a lazy list.
        s = ll()[:1]
        self.assertIs(type(s), self.llist)
        self.assertTrue(s.is_lazy(0))

    def test_conversions_compute(self):
        lazy = self.lazy
        pairs = [
            (self.pdict, lambda: self.ldict(a=lazy(int, '1'))),
            (self.pdict, lambda: self.ldict(a=lazy(int, '1')).transient()),
            (self.tdict, lambda: self.ldict(a=lazy(int, '1'))),
            (self.tdict, lambda: self.ldict(a=lazy(int, '1')).transient()),
        ]
        for (to, make) in pairs:
            with self.subTest(to=to.__name__):
                r = to(make())
                self.assertIs(type(r), to)
                self.assertEqual(r['a'], 1)
                self.assertNotIsInstance(dict.__getitem__(dict(r.items()), 'a'),
                                         lazy)
        lpairs = [
            (self.plist, lambda: self.llist([lazy(int, '1')])),
            (self.plist, lambda: self.llist([lazy(int, '1')]).transient()),
            (self.tlist, lambda: self.llist([lazy(int, '1')])),
            (self.tlist, lambda: self.llist([lazy(int, '1')]).transient()),
            (self.pset, lambda: self.llist([lazy(int, '1')])),
            (self.tset, lambda: self.llist([lazy(int, '1')])),
        ]
        for (to, make) in lpairs:
            with self.subTest(to=to.__name__):
                r = to(make())
                self.assertIs(type(r), to)
                self.assertEqual(list(r), [1])

    def test_lazy_constructors_keep_lazies(self):
        lazy = self.lazy
        f = _Counter()
        raw = self.pdict(a=lazy(f, 1))
        for make in (self.ldict, self.tldict):
            d = make(raw)
            self.assertTrue(d.is_lazy('a'))
            self.assertFalse(d.is_ready('a'))
        for src in (self.ldict(a=lazy(f, 2)),
                    self.ldict(a=lazy(f, 3)).transient(),
                    self.tldict(a=lazy(f, 4))):
            for make in (self.ldict, self.tldict):
                self.assertTrue(make(src).is_lazy('a'))
        rawl = self.plist([lazy(f, 5)])
        for src in (rawl, self.llist(rawl), self.llist(rawl).transient(),
                    self.tllist(rawl)):
            for make in (self.llist, self.tllist):
                self.assertTrue(make(src).is_lazy(0))
        self.assertEqual(f.calls, [])

    def test_held_collections(self):
        lazy = self.lazy
        ld = self.ldict(a=lazy(int, '1'))
        h = ld.held_pdict()
        self.assertIs(type(h), self.pdict)
        self.assertIsInstance(h['a'], lazy)
        self.assertIs(type(self.holdlazy(ld)), self.pdict)
        t = ld.transient()
        ht = t.held_tdict()
        self.assertIs(type(ht), self.tdict)
        self.assertIsInstance(ht['a'], lazy)
        # The held transient does not share storage with the original.
        ht['b'] = 2
        t['c'] = 3
        self.assertNotIn('b', t)
        self.assertNotIn('c', ht)
        ll = self.llist([lazy(int, '1')])
        self.assertIs(type(ll.held_plist()), self.plist)
        self.assertIsInstance(ll.held_plist()[0], lazy)
        tl = ll.transient()
        htl = tl.held_tlist()
        self.assertIs(type(htl), self.tlist)
        self.assertIsInstance(htl[0], lazy)
        htl.append(2)
        tl.append(3)
        self.assertEqual(len(htl), 2)
        self.assertEqual(list(tl), [1, 3])
        for name in ('as_pdict', 'as_plist', 'as_tdict', 'as_tlist'):
            for obj in (ld, t, ll, tl):
                self.assertFalse(hasattr(obj, name))

    def test_transient_round_trips(self):
        lazy = self.lazy
        ld = self.ldict(a=lazy(int, '1'))
        t = ld.transient()
        self.assertIs(type(t), self.tldict)
        self.assertIs(t.persistent(), ld)
        t['b'] = lazy(int, '2')
        p = t.persistent()
        self.assertIs(type(p), self.ldict)
        self.assertTrue(p.is_lazy('b'))
        self.assertEqual(p['b'], 2)
        # A tldict made from a plain pdict makes an ldict, not the pdict.
        tt = self.tldict(self.pdict(a=1))
        self.assertIs(type(tt.persistent()), self.ldict)
        ll = self.llist([lazy(int, '1')])
        tl = ll.transient()
        self.assertIs(type(tl), self.tllist)
        self.assertIs(tl.persistent(), ll)
        tl.append(lazy(int, '2'))
        pl = tl.persistent()
        self.assertIs(type(pl), self.llist)
        self.assertTrue(pl.is_lazy(1))
        self.assertIs(type(self.tllist([1]).persistent()), self.llist)

    def test_matching_methods(self):
        names = {'is_lazy', 'is_ready', 'ready_all', 'getlazy',
                 '__holdlazy__'}
        for cls in (self.ldict, self.tldict, self.llist, self.tllist):
            for name in names:
                self.assertTrue(hasattr(cls, name), (cls, name))

    def test_ready_all_and_status(self):
        lazy = self.lazy
        for (d, key) in ((self.ldict(a=lazy(int, '1'), b=2), 'a'),
                         (self.tldict(a=lazy(int, '1'), b=2), 'a'),
                         (self.llist([lazy(int, '1'), 2]), 0),
                         (self.tllist([lazy(int, '1'), 2]), 0)):
            with self.subTest(type=type(d).__name__):
                other = 'b' if key == 'a' else 1
                self.assertTrue(d.is_lazy(key))
                self.assertFalse(d.is_lazy(other))
                self.assertFalse(d.is_ready(key))
                self.assertTrue(d.is_ready(other))
                self.assertIs(d.ready_all(), d)
                self.assertTrue(d.is_ready(key))
                self.assertIsInstance(d.getlazy(key), lazy)

    def test_str_and_repr_do_not_compute(self):
        lazy = self.lazy
        f = _Counter()
        cases = [(self.ldict(a=lazy(f, 1)), '{|', '|}'),
                 (self.tldict(a=lazy(f, 1)), '{<', '>}'),
                 (self.llist([lazy(f, 1)]), '[|', '|]'),
                 (self.tllist([lazy(f, 1)]), '[<', '>]')]
        for (coll, left, right) in cases:
            for text in (str(coll), repr(coll)):
                self.assertTrue(text.startswith(left), text)
                self.assertTrue(text.endswith(right), text)
                self.assertIn('<lazy>', text)
        self.assertEqual(f.calls, [])

    def test_lazy_collections_pickle_values(self):
        ld = self.ldict(a=self.lazy(int, '7'))
        back = pickle.loads(pickle.dumps(ld))
        self.assertIs(type(back), self.ldict)
        self.assertEqual(back, {'a': 7})
        tl = self.tllist([self.lazy(int, '8')])
        back = pickle.loads(pickle.dumps(tl))
        self.assertIs(type(back), self.tllist)
        self.assertEqual(list(back), [8])


make_tests('TestLazy', _LazyTests, globals())
