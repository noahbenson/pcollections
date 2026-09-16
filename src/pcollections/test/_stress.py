# -*- coding: utf-8 -*-
################################################################################
# pcollections/test/_stress.py
# Long-running randomized tests that compare each type with a builtin.
# By Noah C. Benson

"""Randomized state-machine tests, run against every backend.

Each test keeps a builtin collection (the reference) alongside the
pcollections collections that should behave like it, applies a long random
sequence of operations to all of them, and after every operation checks that
they agree: the operation's result (or the type of exception it raised), the
contents and their order, the length, equality with the reference, and, for
the persistent types, the hash. Old versions of the persistent collections
are kept and checked from time to time to make sure that later operations
never change them.

Each test runs ``PCOLLECTIONS_STRESS_STEPS`` operations (default 100,000),
from a fixed random seed; set ``PCOLLECTIONS_STRESS_SEED`` to an integer to
run different sequences. A failure names the seed, the step, and the
operation.
"""

import os
import random

from ._backends import make_tests


def _steps():
    return int(os.environ.get('PCOLLECTIONS_STRESS_STEPS', '100000'))


def _seed(n):
    return int(os.environ.get('PCOLLECTIONS_STRESS_SEED', '0')) * 10 + n


def _identity(x):
    return x


class _Collide:
    """A key whose hash collides with those of many other keys."""
    __slots__ = ('v',)
    def __init__(self, v):
        self.v = v
    def __hash__(self):
        return self.v % 3
    def __eq__(self, other):
        return isinstance(other, _Collide) and self.v == other.v
    def __repr__(self):
        return f"_Collide({self.v})"


def _outcome(fn):
    """Returns `('ok', result)`, or `('err', exception type)`."""
    try:
        return ('ok', fn())
    except Exception as e:
        return ('err', type(e))


class _Machine:
    """The shared parts of the state machines."""

    def __init__(self, test, seed):
        self.test = test
        self.B = test
        self.rng = random.Random(seed)
        self.seed = seed
        self.step = 0
        self.op = None
        self.snapshots = []

    def fail(self, what, *details):
        self.test.fail(f"{what} (seed {self.seed}, step {self.step}, "
                       f"operation {self.op}): " +
                       '; '.join(repr(d) for d in details))

    def same(self, ref, *outs, check_value=True):
        """Checks that the outcomes `outs` match the reference outcome."""
        for out in outs:
            if out[0] != ref[0] or (ref[0] == 'err' and out[1] is not ref[1]):
                self.fail("outcomes differ", ref, out)
            if check_value and ref[0] == 'ok' and not self.equal(out[1], ref[1]):
                self.fail("results differ", ref[1], out[1])

    @staticmethod
    def equal(a, b):
        return a is b or a == b

    def maybe_lazy(self, value):
        if self.rng.random() < 0.3:
            return self.B.lazy(_identity, value)
        return value

    def snapshot(self, persistent, contents):
        self.snapshots.append((persistent, contents))
        if len(self.snapshots) > 12:
            del self.snapshots[0]

    def check_snapshots(self, contents_of):
        for (p, contents) in self.snapshots:
            if contents_of(p) != contents:
                self.fail("an old version changed", contents, contents_of(p))

    def run(self, steps):
        ops = self.ops()
        names = [name for (name, weight) in ops]
        weights = [weight for (name, weight) in ops]
        for self.step in range(steps):
            self.op = self.rng.choices(names, weights)[0]
            getattr(self, 'op_' + self.op)()
            self.check()
            if self.step % 97 == 0:
                self.take_snapshots()
            if self.step % 1009 == 0:
                self.check_all_snapshots()


#-------------------------------------------------------------------------------
# Dictionaries: dict vs. tdict, tldict, pdict, and ldict.

class _DictMachine(_Machine):
    def __init__(self, test, seed):
        super().__init__(test, seed)
        B = self.B
        self.ref = {}
        self.t = B.tdict()
        self.lt = B.tldict()
        self.p = B.pdict()
        self.lp = B.ldict()

    def ops(self):
        return [('set', 30), ('delete', 10), ('drop', 4), ('pop', 6),
                ('popitem', 4), ('setdefault', 4), ('get', 6),
                ('getitem', 6), ('contains', 4), ('update', 3), ('ior', 2),
                ('or', 2), ('setall', 2), ('dropall', 2), ('clear', 0.05),
                ('roundtrip', 1), ('copy', 1), ('views', 2), ('fromkeys', 0.5)]

    def key(self):
        r = self.rng.random()
        if r < 0.5:
            return self.rng.randrange(40)
        elif r < 0.8:
            return 'k' + str(self.rng.randrange(10))
        else:
            return _Collide(self.rng.randrange(12))

    def value(self):
        r = self.rng.random()
        if r < 0.7:
            return self.rng.randrange(1000)
        elif r < 0.9:
            return 'v' + str(self.rng.randrange(100))
        return None

    def op_set(self):
        (k, v) = (self.key(), self.value())
        self.ref[k] = v
        self.t[k] = v
        self.lt[k] = self.maybe_lazy(v)
        self.p = self.p.set(k, v)
        self.lp = self.lp.set(k, self.maybe_lazy(v))

    def op_delete(self):
        k = self.key()
        ref = _outcome(lambda: self.ref.__delitem__(k))
        self.same(ref, _outcome(lambda: self.t.__delitem__(k)),
                  _outcome(lambda: self.lt.__delitem__(k)))
        outs = []
        for name in ('p', 'lp'):
            out = _outcome(lambda: getattr(self, name).delete(k))
            outs.append(out)
            if out[0] == 'ok':
                setattr(self, name, out[1])
        self.same(ref, *outs, check_value=False)

    def op_drop(self):
        k = self.key()
        present = k in self.ref
        self.ref.pop(k, None)
        for name in ('p', 'lp'):
            old = getattr(self, name)
            new = old.drop(k)
            if not present and new is not old:
                self.fail("drop of a missing key made a new mapping")
            setattr(self, name, new)
        if present:
            del self.t[k]
            del self.lt[k]
        err = _outcome(lambda: self.p.drop(k, error=True))
        if err != ('err', KeyError):
            self.fail("drop(error=True) of a missing key", err)

    def op_pop(self):
        k = self.key()
        args = (k,) if self.rng.random() < 0.5 else (k, 'default')
        ref = _outcome(lambda: self.ref.pop(*args))
        self.same(ref, _outcome(lambda: self.t.pop(*args)),
                  _outcome(lambda: self.lt.pop(*args)))
        for name in ('p', 'lp'):
            out = _outcome(lambda: getattr(self, name).pop(*args))
            if out[0] == 'ok':
                (value, new) = out[1]
                self.same(ref, ('ok', value))
                setattr(self, name, new)
            else:
                self.same(ref, out)

    def op_popitem(self):
        # As dict does, the pcollections mappings remove their last item.
        ref = _outcome(self.ref.popitem)
        self.same(ref, _outcome(self.t.popitem), _outcome(self.lt.popitem))
        for name in ('p', 'lp'):
            out = _outcome(getattr(self, name).popitem)
            if out[0] == 'ok':
                (item, new) = out[1]
                self.same(ref, ('ok', item))
                setattr(self, name, new)
            else:
                self.same(ref, out)

    def op_setdefault(self):
        (k, v) = (self.key(), self.value())
        ref = _outcome(lambda: self.ref.setdefault(k, v))
        self.same(ref, _outcome(lambda: self.t.setdefault(k, v)),
                  _outcome(lambda: self.lt.setdefault(k, v)))
        self.p = self.p.setdefault(k, v)
        self.lp = self.lp.setdefault(k, v)

    def op_get(self):
        k = self.key()
        args = (k,) if self.rng.random() < 0.5 else (k, 'default')
        ref = _outcome(lambda: self.ref.get(*args))
        self.same(ref, *(_outcome(lambda c=c: c.get(*args))
                         for c in (self.t, self.lt, self.p, self.lp)))

    def op_getitem(self):
        k = self.key()
        ref = _outcome(lambda: self.ref[k])
        self.same(ref, *(_outcome(lambda c=c: c[k])
                         for c in (self.t, self.lt, self.p, self.lp)))

    def op_contains(self):
        k = self.key()
        ref = ('ok', k in self.ref)
        self.same(ref, *(('ok', k in c)
                         for c in (self.t, self.lt, self.p, self.lp)))

    def other(self):
        n = self.rng.randrange(6)
        return {self.key(): self.value() for _ in range(n)}

    def op_update(self):
        other = self.other()
        r = self.rng.random()
        if r < 0.3:
            (args, kw) = ((other,), {})
        elif r < 0.6:
            (args, kw) = ((list(other.items()),), {})
        else:
            kw = {'s' + str(self.rng.randrange(5)): self.value()
                  for _ in range(self.rng.randrange(3))}
            (args, kw) = ((other,), kw)
        self.ref.update(*args, **kw)
        self.t.update(*args, **kw)
        self.lt.update(*args, **kw)
        self.p = self.p.update(*args, **kw)
        self.lp = self.lp.update(*args, **kw)

    def op_ior(self):
        other = self.other()
        self.ref.update(other)
        t = self.t
        self.t |= other
        if self.t is not t:
            self.fail("|= made a new tdict")
        self.lt |= list(other.items())
        self.p |= other
        self.lp = self.lp | other

    def op_or(self):
        other = self.other()
        expect = dict(self.ref)
        expect.update(other)
        for c in (self.t, self.p, self.lt, self.lp):
            result = c | other
            if list(result.items()) != list(expect.items()):
                self.fail("| gave the wrong result", expect, result)
        swapped = dict(other)
        swapped.update(self.ref)
        result = other | self.p if isinstance(other, dict) else None
        if result is not None and dict(result.items()) != swapped:
            self.fail("dict | pdict gave the wrong result", swapped, result)

    def op_setall(self):
        other = self.other()
        self.ref.update(other)
        self.t.update(other)
        self.lt.update(other)
        self.p = self.p.setall(list(other.keys()), list(other.values()))
        self.lp = self.lp.setall(other.keys(), other.values())

    def op_dropall(self):
        keys = [self.key() for _ in range(self.rng.randrange(6))]
        for k in keys:
            self.ref.pop(k, None)
            self.t.pop(k, None)
            self.lt.pop(k, None)
        self.p = self.p.dropall(keys)
        self.lp = self.lp.dropall(keys)
        err = _outcome(lambda: self.p.deleteall(keys + ['missing key']))
        if err != ('err', KeyError):
            self.fail("deleteall of a missing key", err)

    def op_clear(self):
        self.ref.clear()
        self.t.clear()
        self.lt.clear()
        self.p = self.p.clear()
        self.lp = self.lp.clear()

    def op_roundtrip(self):
        self.snapshot(self.t.persistent(), list(self.ref.items()))
        self.t = self.t.persistent().transient()
        self.lt = self.lt.persistent().transient()
        self.p = self.p.transient().persistent()
        self.lp = self.lp.transient().persistent()

    def op_copy(self):
        for name in ('t', 'lt'):
            c = getattr(self, name)
            dup = c.copy()
            dup['copy-only'] = 1
            if 'copy-only' in c:
                self.fail("changing a copy changed the original")
            setattr(self, name, dup)
            del dup['copy-only']

    def op_views(self):
        keys = list(self.ref.keys())
        values = list(self.ref.values())
        items = list(self.ref.items())
        for c in (self.t, self.lt, self.p, self.lp):
            if (list(c.keys()) != keys or list(c.values()) != values
                    or list(reversed(c)) != keys[::-1]
                    or list(reversed(c.items())) != items[::-1]):
                self.fail("views differ", c)
            if items and items[0] not in c.items():
                self.fail("an item is missing from items()", items[0])

    def op_fromkeys(self):
        keys = [self.key() for _ in range(self.rng.randrange(5))]
        expect = list(dict.fromkeys(keys, 0).items())
        for cls in (self.B.pdict, self.B.tdict, self.B.ldict, self.B.tldict):
            if list(cls.fromkeys(keys, 0).items()) != expect:
                self.fail("fromkeys differs", cls)

    def check(self):
        items = list(self.ref.items())
        n = len(items)
        for c in (self.t, self.lt, self.p, self.lp):
            if len(c) != n:
                self.fail("lengths differ", n, len(c), type(c))
            if list(c.items()) != items:
                self.fail("contents differ", items, list(c.items()))
            if c != self.ref:
                self.fail("not equal to the reference", type(c))
        h = hash(self.p)
        if h != hash(self.B.pdict(items)) or h != hash(self.lp):
            self.fail("hashes differ")

    def take_snapshots(self):
        items = list(self.ref.items())
        self.snapshot(self.p, items)
        self.snapshot(self.lp, items)

    def check_all_snapshots(self):
        self.check_snapshots(lambda p: list(p.items()))


#-------------------------------------------------------------------------------
# Lists: list vs. tlist, tllist, plist, and llist.

class _ListMachine(_Machine):
    MAXLEN = 60

    def __init__(self, test, seed):
        super().__init__(test, seed)
        B = self.B
        self.ref = []
        self.t = B.tlist()
        self.lt = B.tllist()
        self.p = B.plist()
        self.lp = B.llist()

    def ops(self):
        return [('append', 10), ('prepend', 5), ('insert', 6), ('pop', 8),
                ('delitem', 4), ('setitem', 8), ('getitem', 6),
                ('getslice', 4), ('setslice', 3), ('delslice', 2),
                ('extend', 3), ('remove', 3), ('index', 3), ('count', 2),
                ('reverse', 1), ('sort', 1), ('add', 2), ('mul', 1),
                ('compare', 3), ('clear', 0.05), ('roundtrip', 1),
                ('drop', 2)]

    def value(self):
        return self.rng.randrange(-30, 30)

    def index(self):
        n = len(self.ref)
        return self.rng.randrange(-n - 3, n + 3)

    def a_slice(self):
        n = len(self.ref)
        def bound():
            return None if self.rng.random() < 0.2 else \
                self.rng.randrange(-n - 3, n + 4)
        step = self.rng.choice([None, 1, 1, -1, 2, -2, 3, 0])
        return slice(bound(), bound(), step)

    def items(self, k=None):
        if k is None:
            k = self.rng.randrange(5)
        return [self.value() for _ in range(k)]

    def trim(self):
        # Keeps the lists short; this is not an operation under test.
        while len(self.ref) > self.MAXLEN:
            self.ref.pop(0)
            self.t.pop(0)
            self.lt.pop(0)
            self.p = self.p.delete(0)
            self.lp = self.lp.delete(0)

    def all_transient(self, fn):
        return [_outcome(lambda c=c: fn(c)) for c in (self.t, self.lt)]

    def persistent_update(self, ref, fn, returns_pair=False):
        """Applies `fn` to the persistent lists, which returns the new list
        (or a (value, new list) pair), and compares with `ref`."""
        for name in ('p', 'lp'):
            out = _outcome(lambda: fn(getattr(self, name)))
            if out[0] == 'ok':
                if returns_pair:
                    (value, new) = out[1]
                    self.same(ref, ('ok', value))
                else:
                    self.same(ref, ('ok', ref[1] if ref[0] == 'ok' else None),
                              check_value=False)
                    new = out[1]
                setattr(self, name, new)
            else:
                self.same(ref, out)

    def op_append(self):
        x = self.value()
        self.ref.append(x)
        self.t.append(x)
        self.lt.append(self.maybe_lazy(x))
        self.p = self.p.append(x)
        self.lp = self.lp.append(self.maybe_lazy(x))
        self.trim()

    def op_prepend(self):
        x = self.value()
        self.ref.insert(0, x)
        self.t.prepend(x)
        self.lt.prepend(self.maybe_lazy(x))
        self.p = self.p.prepend(x)
        self.lp = self.lp.prepend(self.maybe_lazy(x))
        self.trim()

    def op_insert(self):
        (i, x) = (self.index(), self.value())
        self.ref.insert(i, x)
        self.t.insert(i, x)
        self.lt.insert(i, x)
        self.p = self.p.insert(i, x)
        self.lp = self.lp.insert(i, x)
        self.trim()

    def op_pop(self):
        args = () if self.rng.random() < 0.3 else (self.index(),)
        ref = _outcome(lambda: self.ref.pop(*args))
        self.same(ref, *self.all_transient(lambda c: c.pop(*args)))
        self.persistent_update(ref, lambda p: p.pop(*args), returns_pair=True)

    def op_delitem(self):
        i = self.index()
        ref = _outcome(lambda: self.ref.__delitem__(i))
        self.same(ref, *self.all_transient(lambda c: c.__delitem__(i)))
        self.persistent_update(ref, lambda p: p.delete(i))

    def op_drop(self):
        i = self.index()
        n = len(self.ref)
        present = -n <= i < n
        if present:
            del self.ref[i]
            del self.t[i]
            del self.lt[i]
        for name in ('p', 'lp'):
            old = getattr(self, name)
            new = old.drop(i)
            if not present and new is not old:
                self.fail("drop of a missing index made a new list")
            setattr(self, name, new)

    def op_setitem(self):
        (i, x) = (self.index(), self.value())
        ref = _outcome(lambda: self.ref.__setitem__(i, x))
        self.same(ref, *self.all_transient(lambda c: c.__setitem__(i, x)))
        self.persistent_update(ref, lambda p: p.set(i, x))

    def op_getitem(self):
        i = self.index()
        ref = _outcome(lambda: self.ref[i])
        self.same(ref, *(_outcome(lambda c=c: c[i])
                         for c in (self.t, self.lt, self.p, self.lp)))

    def op_getslice(self):
        s = self.a_slice()
        ref = _outcome(lambda: self.ref[s])
        for c in (self.t, self.lt, self.p, self.lp):
            out = _outcome(lambda: c[s])
            self.same(ref, out, check_value=False)
            if out[0] == 'ok':
                if type(out[1]) is not type(c):
                    self.fail("a slice has the wrong type", type(out[1]))
                if list(out[1]) != ref[1]:
                    self.fail("slices differ", ref[1], list(out[1]))

    def op_setslice(self):
        s = self.a_slice()
        n = len(range(*s.indices(len(self.ref)))) \
            if s.step not in (None, 1, 0) else None
        new = self.items(n)
        ref = _outcome(lambda: self.ref.__setitem__(s, new))
        self.same(ref, *self.all_transient(lambda c: c.__setitem__(s, new)))
        if ref[0] == 'ok':
            self.p = self.B.plist(self.ref)
            self.lp = self.B.llist(self.ref)
        self.trim()

    def op_delslice(self):
        s = self.a_slice()
        ref = _outcome(lambda: self.ref.__delitem__(s))
        self.same(ref, *self.all_transient(lambda c: c.__delitem__(s)))
        if ref[0] == 'ok':
            self.p = self.B.plist(self.ref)
            self.lp = self.B.llist(self.ref)

    def op_extend(self):
        new = self.items()
        self.ref.extend(new)
        if self.rng.random() < 0.5:
            self.t.extend(new)
            self.lt.extend(iter(new))
        else:
            self.t += tuple(new)
            self.lt += new
        self.p = self.p.extend(new)
        self.lp = self.lp.extend(new)
        self.trim()

    def op_remove(self):
        x = self.value()
        ref = _outcome(lambda: self.ref.remove(x))
        self.same(ref, *self.all_transient(lambda c: c.remove(x)))
        self.persistent_update(ref, lambda p: p.remove(x))

    def op_index(self):
        x = self.value()
        args = (x,) if self.rng.random() < 0.5 else (x, self.index())
        ref = _outcome(lambda: self.ref.index(*args))
        self.same(ref, *(_outcome(lambda c=c: c.index(*args))
                         for c in (self.t, self.lt, self.p, self.lp)))

    def op_count(self):
        x = self.value()
        ref = ('ok', self.ref.count(x))
        self.same(ref, *(('ok', c.count(x)) for c in
                         (self.t, self.lt, self.p, self.lp)))
        self.same(('ok', x in self.ref), *(('ok', x in c) for c in
                                            (self.t, self.lt, self.p, self.lp)))

    def op_reverse(self):
        self.ref.reverse()
        self.t.reverse()
        self.lt.reverse()
        self.p = self.p.reverse()
        self.lp = self.lp.reverse()

    def op_sort(self):
        kw = {}
        if self.rng.random() < 0.5:
            kw['key'] = lambda v: v % 7
        if self.rng.random() < 0.5:
            kw['reverse'] = True
        self.ref.sort(**kw)
        self.t.sort(**kw)
        self.lt.sort(**kw)
        self.p = self.p.sort(**kw)
        self.lp = self.lp.sort(**kw)

    def op_add(self):
        other = self.items()
        expect = self.ref + other
        rexpect = other + self.ref
        for c in (self.t, self.lt, self.p, self.lp):
            (a, b) = (c + other, other + c)
            if type(a) is not type(c) or list(a) != expect:
                self.fail("+ differs", expect, a)
            if type(b) is not type(c) or list(b) != rexpect:
                self.fail("reflected + differs", rexpect, b)

    def op_mul(self):
        k = self.rng.randrange(-1, 3)
        expect = self.ref * k
        for c in (self.t, self.lt, self.p, self.lp):
            for r in (c * k, k * c):
                if type(r) is not type(c) or list(r) != expect:
                    self.fail("* differs", expect, r)
        self.ref *= k
        self.t *= k
        self.lt *= k
        self.p = self.p * k
        self.lp = self.lp * k
        self.trim()

    def op_compare(self):
        other = list(self.ref)
        r = self.rng.random()
        if other and r < 0.3:
            other[self.rng.randrange(len(other))] = self.value()
        elif r < 0.5:
            other = other[:self.rng.randrange(len(other) + 1)]
        elif r < 0.7:
            other.append(self.value())
        for op in ('__eq__', '__ne__', '__lt__', '__le__', '__gt__', '__ge__'):
            expect = getattr(self.ref, op)(other)
            for c in (self.t, self.lt, self.p, self.lp):
                for o in (other, type(c)(other)):
                    if getattr(c, op)(o) != expect:
                        self.fail("comparisons differ", op, other)

    def op_clear(self):
        self.ref.clear()
        self.t.clear()
        self.lt.clear()
        self.p = self.p.clear()
        self.lp = self.lp.clear()

    def op_roundtrip(self):
        self.snapshot(self.t.persistent(), list(self.ref))
        self.t = self.t.persistent().transient()
        self.lt = self.lt.persistent().transient()
        self.p = self.p.transient().persistent()
        self.lp = self.lp.transient().persistent()

    def check(self):
        ref = self.ref
        n = len(ref)
        for c in (self.t, self.lt, self.p, self.lp):
            if len(c) != n:
                self.fail("lengths differ", n, len(c), type(c))
            if list(c) != ref:
                self.fail("contents differ", ref, list(c))
            if c != ref:
                self.fail("not equal to the reference", type(c))
        h = hash(self.p)
        if h != hash(self.B.plist(ref)) or h != hash(self.lp):
            self.fail("hashes differ")

    def take_snapshots(self):
        self.snapshot(self.p, list(self.ref))
        self.snapshot(self.lp, list(self.ref))

    def check_all_snapshots(self):
        self.check_snapshots(list)


#-------------------------------------------------------------------------------
# Sets: set vs. tset and pset. Sets are compared as sets, and their order is
# checked against the order in which the elements were added (kept by the
# `order` dict).

class _SetMachine(_Machine):
    def __init__(self, test, seed):
        super().__init__(test, seed)
        B = self.B
        self.ref = set()
        self.order = {}
        self.t = B.tset()
        self.p = B.pset()

    def ops(self):
        return [('add', 30), ('discard', 12), ('remove', 5), ('drop', 3),
                ('pop', 5), ('contains', 6), ('update', 4),
                ('difference_update', 3), ('intersection_update', 2),
                ('symmetric_difference_update', 2), ('operators', 3),
                ('methods', 3), ('relations', 3), ('clear', 0.05),
                ('roundtrip', 1), ('copy', 1)]

    def element(self):
        r = self.rng.random()
        if r < 0.5:
            return self.rng.randrange(60)
        elif r < 0.75:
            return 'e' + str(self.rng.randrange(15))
        return _Collide(self.rng.randrange(20))

    def elements(self):
        return [self.element() for _ in range(self.rng.randrange(8))]

    # The reference order, as a set with insertion order.
    def ref_add(self, x):
        if x not in self.ref:
            self.ref.add(x)
            self.order[x] = None

    def ref_discard(self, x):
        self.ref.discard(x)
        self.order.pop(x, None)

    def op_add(self):
        x = self.element()
        self.ref_add(x)
        self.t.add(x)
        self.p = self.p.add(x)

    def op_discard(self):
        x = self.element()
        present = x in self.ref
        self.ref_discard(x)
        self.t.discard(x)
        old = self.p
        self.p = self.p.discard(x)
        if not present and self.p is not old:
            self.fail("discard of a missing element made a new set")

    def op_remove(self):
        x = self.element()
        ref = _outcome(lambda: self.ref.remove(x))
        if ref[0] == 'ok':
            del self.order[x]
        self.same(ref, _outcome(lambda: self.t.remove(x)))
        out = _outcome(lambda: self.p.remove(x))
        self.same(ref, out, check_value=False)
        if out[0] == 'ok':
            self.p = out[1]

    def op_drop(self):
        x = self.element()
        present = x in self.ref
        self.ref_discard(x)
        self.t.discard(x)
        old = self.p
        self.p = self.p.drop(x)
        if not present and self.p is not old:
            self.fail("drop of a missing element made a new set")
        if _outcome(lambda: self.p.drop(x, error=True)) != ('err', KeyError):
            self.fail("drop(error=True) of a missing element")

    def op_pop(self):
        # The pcollections sets remove their last element.
        if self.ref:
            x = next(reversed(self.order))
            self.ref_discard(x)
            ref = ('ok', x)
        else:
            ref = ('err', KeyError)
        self.same(ref, _outcome(self.t.pop))
        out = _outcome(self.p.pop)
        if out[0] == 'ok':
            (x, self.p) = out[1]
            out = ('ok', x)
        self.same(ref, out)

    def op_contains(self):
        x = self.element()
        self.same(('ok', x in self.ref), ('ok', x in self.t),
                  ('ok', x in self.p))

    def op_update(self):
        new = self.elements()
        if self.rng.random() < 0.5:
            self.t.update(new)
            self.p = self.p.union(new)
        else:
            # Elements are added in the order of the set's iteration.
            new = set(new)
            self.t |= new
            self.p = self.p.addall(iter(new))
        for x in new:
            self.ref_add(x)

    def op_difference_update(self):
        new = self.elements()
        for x in new:
            self.ref_discard(x)
        self.t.difference_update(new)
        self.p = self.p.difference(new) if self.rng.random() < 0.5 \
            else self.p.discardall(new)

    def op_intersection_update(self):
        new = self.elements() + list(self.rng.sample(
            list(self.order), min(len(self.order), self.rng.randrange(20))))
        for x in list(self.order):
            if x not in new:
                self.ref_discard(x)
        self.t.intersection_update(new)
        self.p = self.p.intersection(new)

    def op_symmetric_difference_update(self):
        new = self.elements()
        other = set(new)
        # The pcollections sets remove the elements they share with the
        # other set, then add the others in the other set's order.
        shared = [x for x in self.order if x in other]
        for x in shared:
            self.ref_discard(x)
        for x in other:
            if x not in shared:
                self.ref_add(x)
        if self.rng.random() < 0.5:
            self.t.symmetric_difference_update(new)
            self.p = self.p.symmetric_difference(new)
        else:
            self.t ^= other
            self.p = self.p ^ other

    def op_operators(self):
        other = set(self.elements())
        for c in (self.t, self.p):
            for (op, expect) in ((c.__or__, self.ref | other),
                                 (c.__and__, self.ref & other),
                                 (c.__sub__, self.ref - other),
                                 (c.__xor__, self.ref ^ other)):
                result = op(other)
                if type(result) is not type(c) or result != expect:
                    self.fail("set operators differ", op, expect, result)
            for (result, expect) in ((other | c, other | self.ref),
                                     (other & c, other & self.ref),
                                     (other - c, other - self.ref),
                                     (other ^ c, other ^ self.ref)):
                if result != expect:
                    self.fail("reflected set operators differ", expect, result)

    def op_methods(self):
        new = self.elements()
        for c in (self.t, self.p):
            for name in ('union', 'intersection', 'difference',
                         'symmetric_difference'):
                expect = getattr(self.ref, name)(new)
                result = getattr(c, name)(iter(new))
                if type(result) is not type(c) or result != expect:
                    self.fail("set methods differ", name, expect, result)

    def op_relations(self):
        other = set(self.elements())
        if self.rng.random() < 0.5:
            other |= set(self.rng.sample(list(self.ref),
                                         min(len(self.ref), 3)))
        for c in (self.t, self.p):
            for name in ('isdisjoint', 'issubset', 'issuperset'):
                expect = getattr(self.ref, name)(other)
                if getattr(c, name)(list(other)) != expect:
                    self.fail("set relations differ", name)
            for op in ('__eq__', '__ne__', '__lt__', '__le__', '__gt__',
                       '__ge__'):
                expect = getattr(self.ref, op)(other)
                for o in (other, frozenset(other), type(c)(other)):
                    if getattr(c, op)(o) != expect:
                        self.fail("set comparisons differ", op)

    def op_clear(self):
        self.ref.clear()
        self.order.clear()
        self.t.clear()
        self.p = self.p.clear()

    def op_roundtrip(self):
        self.snapshot(self.t.persistent(), list(self.order))
        self.t = self.t.persistent().transient()
        self.p = self.p.transient().persistent()

    def op_copy(self):
        dup = self.t.copy()
        dup.add('copy-only')
        if 'copy-only' in self.t:
            self.fail("changing a copy changed the original")
        dup.discard('copy-only')
        self.t = dup

    def check(self):
        order = list(self.order)
        n = len(order)
        for c in (self.t, self.p):
            if len(c) != n:
                self.fail("lengths differ", n, len(c), type(c))
            if list(c) != order:
                self.fail("contents or order differ", order, list(c))
            if c != self.ref:
                self.fail("not equal to the reference", type(c))
        if hash(self.p) != hash(frozenset(self.ref)):
            self.fail("hash differs from the frozenset's")

    def take_snapshots(self):
        self.snapshot(self.p, list(self.order))

    def check_all_snapshots(self):
        self.check_snapshots(list)


class _StressTests:
    """Instantiated once per backend by ``make_tests``."""

    def run_machine(self, cls, seed):
        cls(self, seed).run(_steps())

    def test_dict_state_machine(self):
        self.run_machine(_DictMachine, _seed(1))

    def test_list_state_machine(self):
        self.run_machine(_ListMachine, _seed(2))

    def test_set_state_machine(self):
        self.run_machine(_SetMachine, _seed(3))


make_tests('TestStress', _StressTests, globals())
