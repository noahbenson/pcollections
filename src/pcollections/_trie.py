# -*- coding: utf-8 -*-
################################################################################
# pcollections/_trie.py
# A pure-Python port of pcollections/_c/{trie,amt,fat}.h's persistent and
# transient trie data structures, used as a drop-in replacement for the
# external `phamt` package (PHAMT/THAMT) that the pure-Python reference
# implementation (_dict.py/_list.py/_set.py/_lazy.py) previously depended on.
#
# This module exposes four classes with an interface deliberately compatible
# with phamt.PHAMT/phamt.THAMT, since that's the exact surface _dict.py/
# _list.py/_set.py already use (`.empty`, `.assoc(k,v)`, `.dissoc(k)`,
# `.get(k,default)`, `__getitem__`, `__len__`, `__iter__` yielding `(k,v)`
# pairs, `TAMT(amt)`/`TFAT(fat)` wrapping a persistent instance into a
# transient one, and `.persistent()` converting back):
#   - AMT / TAMT   -- persistent / transient "array-mapped trie", mirroring
#                     _c/amt.h; used for pdict/pset's `_idx` (hash(key) ->
#                     FAT-chain-head-index) and, generically, for plist's
#                     `_phamt` (index -> element, including negative indices
#                     during prepends -- see "AMT and FAT are unified" below).
#   - FAT / TFAT   -- persistent / transient "fixed-arity trie", mirroring
#                     _c/fat.h; used for pdict/pset's `_els` (dense
#                     insertion-ordered index -> value-table entry).
#
# AMT and FAT are unified in this port
# -------------------------------------------------------------------------
# _c/trie.h says of the C implementation: "Because AMT and FAT tries share
# most of their layout and code, as much functionality as possible is placed
# in the trienode_* functions." In C, what's left genuinely different
# between them is entirely about *memory layout* performance: FAT uses a
# fixed branching factor (FAT_CELLS in _c/trie.h, chosen so one node fits in
# 256 bytes / 4 cache lines) with every one of its cells physically always
# allocated regardless of occupancy, and maintains a
# "dense tree" invariant (every depth from a node down to its leaves is
# explicitly materialized, even a branch with only 1 occupied cell) rather
# than AMT's "minimal tree" invariant (single-child branches are collapsed
# away, and amt_subjoin() skips straight to the first depth where two
# subtrees' prefixes actually diverge). _c/fat.h's own file header comment
# confirms this dense-tree behavior is a design *choice*, not a correctness
# requirement: fat_lookup() is written as a plain loop that re-reads each
# node's real depth every iteration (exactly like amt_lookup()) specifically
# so it stays correct "if FAT's per-depth materialization were ever relaxed
# again in the future."
#
# None of that C-specific cache-layout optimization has any equivalent
# benefit in Python: there's no fixed-size cell array to pre-allocate, no
# cache-line packing to reason about, and Python's own int is already
# arbitrary-precision, so there's no analog of FAT's base-29 digit
# arithmetic (chosen purely to make the fixed-size layout fit in 256 bytes)
# to replicate either. So this port implements ONE generic, minimal-tree,
# path-compressed trie engine (functions prefixed `_node_*`/`_trie_*` below)
# -- structurally an AMT in the _c/amt.h sense -- and defines FAT/TFAT as
# thin, identically-behaved subclasses of AMT/TAMT. This is a deliberate
# engineering simplification, not a shortcut: it cuts the amount of new,
# security/correctness-sensitive trie code roughly in half (one algorithm to
# get right instead of two near-duplicates), and every *externally
# observable* behavior FAT is actually relied on for by _dict.py/_list.py/
# _set.py (dense ascending-order iteration in insertion order in
# particular -- see "Iteration order" below) still holds.
#
# The one place AMT and FAT still differ, deliberately, is key normalization
# (see AMT._normalize_key()/FAT._normalize_key() below) -- kept as a real,
# distinct hook (not just a naming difference) so the two remain genuinely
# separate types, matching dict.c.h's own `idx` (AMT) vs `els` (FAT) roles,
# and so a future divergence in behavior (if one is ever needed) has
# somewhere to live without disturbing the other.
#
# Reference counting: unlike _c/trie.h's manual, atomic `refcount` field and
# hand-written trienode_incref()/decref()/free() (needed in C because
# malloc'd trie nodes are otherwise invisible to CPython's own object
# lifetime tracking), this port's trie nodes are ordinary Python objects.
# Python's own reference counting / garbage collector is solely responsible
# for their lifetime; this file never touches a refcount directly. (Per
# Noah's explicit instruction: "It can use the Python reference tracking
# system instead of its own tracking system.")
#
# Thread-safety (no-GIL): a *persistent* AMT/FAT (and any of its nodes) is
# never mutated after construction -- assoc()/dissoc() always build new
# nodes and structurally share the rest, exactly like the C implementation
# -- so persistent instances are safe to share and read from multiple
# threads with no locking of any kind, GIL or no GIL. A *transient*
# TAMT/TFAT follows the same single-owner convention as Python's own
# `dict`/`list` (or Clojure's transients): it is meant to be mutated by
# exactly one owner at a time, with no internal locking on the hot path, and
# concurrent misuse from multiple threads is a caller bug, not something
# this module guards against with locks. What this module *does* guarantee,
# with or without the GIL, is memory safety: every mutation here is an
# ordinary Python attribute/list assignment (`node.bitmap = ...`,
# `node.cells[i] = ...`, never raw/`ctypes`-style memory access), so even
# under a genuine data race from concurrent misuse under a free-threaded
# (no-GIL) build, the worst outcome is a caller seeing a stale or
# inconsistent *value* -- never a crash, a segfault, or corrupted Python
# refcounts/GC state, since ordinary attribute and list mutation is exactly
# the operation CPython's free-threaded build itself guarantees stays
# memory-safe under concurrent access. See test/_trie.py's
# `test_thread_stress` for a GIL-based (this sandbox has no free-threaded
# Python build available to test against real no-GIL semantics -- confirmed
# acceptable with Noah) multithreaded stress test exercising this.
#
# The claiming discipline that makes single-owner mutation safe: every node
# has an `owner` slot, normally `None` (meaning "persistent / shared -- copy
# before mutating"). A transient trie carries its own private, unique
# `_token` object (just a fresh `object()`); a node is "claimed" by that
# transient -- i.e. safe to mutate in place -- exactly when
# `node.owner is token`. Mutating operations check this per node as they
# walk down the tree: an already-claimed node is mutated directly; a
# not-yet-claimed (persistent or, in principle, someone else's transient)
# node is copied first, with the copy's `owner` set to `token`, before being
# mutated -- mirroring amtnode_set()/amtnode_del()'s "claim-or-mutate"
# contract in _c/amt.h exactly, just without any refcounting to manage.
#
# Iteration order: `_iter_node()` below walks bit-index 0 upward at every
# level (matching trienode_first_bitindex()/trienode_next_bitindex()'s
# ascending scan in _c/trie.h), and each level's own bit-index corresponds
# to the *most significant* still-undetermined chunk of the key
# (amtdepth_shift() extracts from the top down: the root's own digit is the
# highest AMT_DIVBITS bits, not the lowest). A most-significant-digit-first,
# ascending-at-each-level walk over a fixed-width unsigned integer is
# exactly radix/lexicographic order, which for equal-width unsigned
# integers is exactly ascending numeric order -- so `_iter_node()` always
# yields keys in ascending order of their *normalized* (masked-to-unsigned,
# see _normalize_key()) representation, regardless of how the tree happens
# to be shaped by path compression. This matters concretely for FAT/TFAT's
# `_els`-table role: pdict/pset's `__iter__` relies on ascending index order
# to reproduce insertion order (see _dict.py's `_els` usage) -- and for
# plist's `_phamt` role, which stores possibly-*negative* indices (via
# prepend) directly: _list.py's own __iter__ already special-cases the
# negative/non-negative boundary explicitly (splitting the walk at 0)
# precisely because masking a negative Python int to an unsigned width
# makes it sort *after* every non-negative key -- so this module only needs
# to guarantee ascending order *within* a run of same-signed keys (which two's
# -complement masking preserves exactly, since unsigned comparison of two
# same-width two's-complement values of the same sign matches their signed
# comparison), which is exactly what callers already assume.

__all__ = ["AMT", "TAMT", "FAT", "TFAT"]


#===============================================================================
# Bit-width configuration.
# Mirrors _c/trie.h's AMT_DIVBITS/TRIEINT_WIDTH-derived constants. Unlike C's
# trieint_t (a size_t, typically but not guaranteed 64 bits), Python's own
# hash() is documented to always fit in a Py_hash_t (a signed word the same
# width as size_t on the build it's running on) -- practically universally
# 64 bits on any platform this fallback is likely to run on today, so 64 is
# hardcoded here rather than probed at import time. This is a faithful
# match, not an arbitrary Python-side restriction: the C implementation has
# exactly the same practical 64-bit limit on essentially every build.

WIDTH = 64
DIVBITS = 5
# Number of layers, and the depth (0-indexed) of the last (twig) layer.
LAYERS = -(-WIDTH // DIVBITS)          # ceil(WIDTH / DIVBITS) = 13
MAX_DEPTH = LAYERS - 1                 # 12
REMBITS = WIDTH % DIVBITS              # 4 (bits consumed by the twig layer)
ROOT_SHIFT = WIDTH - DIVBITS           # 59 (shift amount at depth 0)
DIVMASK = (1 << DIVBITS) - 1           # 0x1F
REMMASK = (1 << REMBITS) - 1           # 0x0F
MASK_WIDTH = (1 << WIDTH) - 1          # mask for normalizing keys to unsigned

if hasattr(int, 'bit_count'):
    def _popcount(x):
        return x.bit_count()
else:
    # Python < 3.10 fallback (pcollections supports Python >= 3.7).
    def _popcount(x):
        return bin(x).count('1')


def _shift(depth):
    """The right-shift amount used to extract a non-twig node's own digit
    from a full-width key at the given depth. Meaningless (and unused) at
    depth == MAX_DEPTH; mirrors amtdepth_shift() in _c/trie.h."""
    return ROOT_SHIFT - depth * DIVBITS


def _shiftmask(depth):
    """The right-shift amount that isolates every bit *above* this depth's
    own digit -- used by _prefix_match(). Mirrors amtdepth_shiftmask()."""
    return WIDTH - depth * DIVBITS


def _bitindex(depth, key):
    """The digit (0..31 for a branch, 0..15 for the twig layer) that `key`
    occupies at the given depth. Mirrors amtdepth_bitindex()."""
    if depth < MAX_DEPTH:
        return (key >> _shift(depth)) & DIVMASK
    else:
        return key & REMMASK


def _prefix_match(depth, prefix, key):
    """True if `key` could plausibly live beneath a node at the given depth
    whose stored prefix is `prefix` -- i.e., every bit *above* this depth's
    own digit agrees. Mirrors amtdepth_prefix_match(); depth 0's shiftmask
    is always >= WIDTH, so the comparison is vacuously true there (nothing
    above the root's own digit to compare)."""
    sm = _shiftmask(depth)
    if sm >= WIDTH:
        return True
    return (prefix >> sm) == (key >> sm)


def _clz(x):
    """Count-leading-zeros of `x` within a WIDTH-bit field. Mirrors
    clz_trieint(); used only by _divergence_depth()."""
    if x == 0:
        return WIDTH
    return WIDTH - x.bit_length()


def _divergence_depth(pa, pb):
    """The shallowest depth at which two (already-normalized, genuinely
    different) prefixes first disagree. Mirrors amt_subjoin()'s use of
    clz_trieint(a^b)/AMT_DIVBITS."""
    depth = _clz(pa ^ pb) // DIVBITS
    return depth if depth < MAX_DEPTH else MAX_DEPTH


#===============================================================================
# Node representation.
#
# A node is either a "branch" (depth < MAX_DEPTH; `cells` holds child nodes)
# or a "twig" (depth == MAX_DEPTH; `cells` holds leaf values directly). Both
# shapes use the same struct: `bitmap` records which of the (32, or 16 for a
# twig) digit slots at this node's depth are occupied, and `cells` holds
# exactly that many entries, compacted (via popcount(bitmap & below-bit
# mask), exactly like AMT's own amtnode_bit2cellindex()) into ascending
# digit order -- there is no FAT-style "always all cells physically
# present" layout anywhere in this port; see the module docstring's "AMT and
# FAT are unified" section for why.
#
# `prefix` holds the real key bits from the root down through (but not
# including) this node's own digit -- i.e., every bit *this node's own
# digit position and below* is zeroed, mirroring amt_1leaf()'s
# `key & ~AMT_TWIG_MASK` and amt_subjoin()'s `prefix & gemask(shift)`. This
# is what lets a twig's original key be reconstructed as
# `node.prefix | bitindex` (see _iter_node()) and what _prefix_match() (via
# a *shallower* node's own prefix check) validates against.
#
# `owner`: see the module docstring's "claiming discipline" paragraph.
# `None` means persistent/shared (must be copied before mutating).

class _Node:
    __slots__ = ('prefix', 'depth', 'bitmap', 'cells', 'owner')

    def __init__(self, prefix, depth, bitmap, cells, owner=None):
        self.prefix = prefix
        self.depth = depth
        self.bitmap = bitmap
        self.cells = cells
        self.owner = owner

    @property
    def is_twig(self):
        return self.depth == MAX_DEPTH


def _empty_node():
    # The canonical empty node (of either kind -- there's nothing
    # kind-specific about an empty node): depth 0, no bits set, no cells.
    # Never claimed by any transient (owner stays None permanently), exactly
    # like amt_empty()'s singleton in _c/amt.h.
    return _Node(0, 0, 0, [])


def _1leaf(key, val, owner=None):
    """A brand-new twig node containing exactly one key/value pair. Mirrors
    amt_1leaf()."""
    bi = key & REMMASK
    prefix = key & ~REMMASK
    return _Node(prefix, MAX_DEPTH, 1 << bi, [val], owner)


#===============================================================================
# Lookup.

def _lookup(root, key):
    """Returns (True, value) if `key` is present beneath `root`, else
    (False, None). Mirrors amt_lookup(), minus the branchless-dummy-node
    trick (a pure C performance hack with no Python equivalent needed)."""
    node = root
    while True:
        if not _prefix_match(node.depth, node.prefix, key):
            return (False, None)
        bi = _bitindex(node.depth, key)
        bit = 1 << bi
        if not (node.bitmap & bit):
            return (False, None)
        ci = _popcount(node.bitmap & (bit - 1))
        if node.depth == MAX_DEPTH:
            return (True, node.cells[ci])
        node = node.cells[ci]


def _find_path(root, key):
    """Walks from `root` toward wherever `key` would live, returning
    `(path, found, is_beneath)`:
      - `path` is a list of `(node, bitindex)` pairs, root first.
      - `found` is True iff `key` is actually present (`path[-1]` is then
        the twig containing it).
      - `is_beneath` is True iff `key` *could* live under `path[-1]`'s
        subtree (its prefix matches) even though it isn't there yet --
        False means the divergence happens strictly above `path[-1]`, and a
        new shared ancestor (via _subjoin()) is needed instead.
    Mirrors triepath_amtfind() in _c/trie.h."""
    path = []
    node = root
    while True:
        bi = _bitindex(node.depth, key)
        ok = _prefix_match(node.depth, node.prefix, key)
        path.append((node, bi))
        if not ok:
            return path, False, False
        if not (node.bitmap & (1 << bi)):
            return path, False, True
        if node.depth == MAX_DEPTH:
            return path, True, True
        ci = _popcount(node.bitmap & ((1 << bi) - 1))
        node = node.cells[ci]


#===============================================================================
# Iteration.

def _iter_node(node):
    """Yields every (key, value) pair beneath `node`, in ascending
    normalized-key order -- see the module docstring's "Iteration order"
    section for why this ordering falls out automatically."""
    bitmap = node.bitmap
    if node.depth == MAX_DEPTH:
        prefix = node.prefix
        ci = 0
        bi = 0
        while bitmap:
            if bitmap & 1:
                yield (prefix | bi, node.cells[ci])
                ci += 1
            bitmap >>= 1
            bi += 1
    else:
        ci = 0
        while bitmap:
            if bitmap & 1:
                for kv in _iter_node(node.cells[ci]):
                    yield kv
                ci += 1
            bitmap >>= 1


def _iter_node_from(node, key):
    """Yields the (key, value) pairs beneath `node` whose normalized key is at
    least `key`, in ascending order. Mirrors fat_seekpath() in _c/trie.h."""
    if not _prefix_match(node.depth, node.prefix, key):
        # Everything beneath the node is either after the key or before it.
        if key < node.prefix:
            yield from _iter_node(node)
        return
    if node.depth == MAX_DEPTH:
        for kv in _iter_node(node):
            if kv[0] >= key:
                yield kv
        return
    target = _bitindex(node.depth, key)
    bitmap = node.bitmap
    ci = 0
    bi = 0
    while bitmap:
        if bitmap & 1:
            if bi == target:
                yield from _iter_node_from(node.cells[ci], key)
            elif bi > target:
                yield from _iter_node(node.cells[ci])
            ci += 1
        bitmap >>= 1
        bi += 1


#===============================================================================
# Non-mutating (copy-and-modify) node primitives.
# Mirror amt_and()/amt_but() in _c/amt.h: never touch their input, always
# either allocate a fresh node or (amt_but only) return the input completely
# unchanged when there's nothing to do.

def _node_and(node, bi, val):
    """A copy of `node` with cell `bi` set to `val` (allocating a new cell
    if that bit wasn't already set)."""
    bit = 1 << bi
    ci = _popcount(node.bitmap & (bit - 1))
    cells = list(node.cells)
    if node.bitmap & bit:
        cells[ci] = val
        bitmap = node.bitmap
    else:
        cells.insert(ci, val)
        bitmap = node.bitmap | bit
    return _Node(node.prefix, node.depth, bitmap, cells)


def _node_but(node, bi):
    """A copy of `node` with cell `bi` unset, or `node` itself, completely
    unchanged, if that bit wasn't set to begin with."""
    bit = 1 << bi
    if not (node.bitmap & bit):
        return node
    ci = _popcount(node.bitmap & (bit - 1))
    cells = list(node.cells)
    del cells[ci]
    return _Node(node.prefix, node.depth, node.bitmap & ~bit, cells)


def _subjoin(a, b):
    """Join two subtrees with disjoint (already-normalized) prefixes under
    a brand-new parent, placed at the shallowest depth their prefixes
    diverge. Mirrors amt_subjoin()."""
    depth = _divergence_depth(a.prefix, b.prefix)
    sh = _shift(depth)
    aii = _bitindex(depth, a.prefix)
    bii = _bitindex(depth, b.prefix)
    prefix = a.prefix & ~((1 << sh) - 1)
    bitmap = (1 << aii) | (1 << bii)
    cells = [a, b] if aii < bii else [b, a]
    return _Node(prefix, depth, bitmap, cells)


def _collapse(node):
    """If `node` is a branch left with exactly one child, replace it with
    that lone child directly (maintaining the minimal-tree invariant: a
    branch always has >= 2 children); otherwise return `node` unchanged.
    Mirrors amt_collapse() -- much simpler here, since there's no manual
    refcounting to juggle around the replacement."""
    if node.depth != MAX_DEPTH and len(node.cells) == 1:
        return node.cells[0]
    return node


#===============================================================================
# Mutating (claim-or-mutate-in-place) node primitives, for transient tries.
# Mirror amtnode_set()/amtnode_del(): if `node` is already claimed by this
# transient (node.owner is token), mutate it directly; otherwise claim a
# fresh copy first. See the module docstring's "claiming discipline"
# paragraph.

def _node_set(node, bi, val, token):
    bit = 1 << bi
    if node.owner is token:
        ci = _popcount(node.bitmap & (bit - 1))
        if node.bitmap & bit:
            node.cells[ci] = val
        else:
            node.cells.insert(ci, val)
            node.bitmap |= bit
        return node
    new = _node_and(node, bi, val)
    new.owner = token
    return new


def _node_del(node, bi, token):
    bit = 1 << bi
    if not (node.bitmap & bit):
        return node
    if node.owner is token:
        ci = _popcount(node.bitmap & (bit - 1))
        del node.cells[ci]
        node.bitmap &= ~bit
        return node
    new = _node_but(node, bi)
    new.owner = token
    return new


#===============================================================================
# Whole-tree, per-item operations.
# assoc_root()/dissoc_root() are the persistent (copy-and-modify) API,
# mirroring amt_anditem()/amt_butitem(); setitem_root()/delitem_root() are
# the transient (claim-and-mutate) API, mirroring tamt_setitem()/
# tamt_delitem(). Each returns `(new_root, changed)`: `changed` is whether
# the live element count went up (assoc/setitem: True means a new key was
# added, not just an existing one overwritten) or down (dissoc/delitem:
# True means a key was actually removed) -- letting the public class's
# `_count` bookkeeping avoid a second, redundant lookup pass.

def assoc_root(root, key, val):
    if root.bitmap == 0:
        return _1leaf(key, val), True
    path, found, is_beneath = _find_path(root, key)
    step = len(path) - 1
    node, bi = path[step]
    if not found:
        if not is_beneath:
            newnode = _subjoin(_1leaf(key, val), node)
        elif node.depth == MAX_DEPTH:
            newnode = _node_and(node, bi, val)
        else:
            newnode = _node_and(node, bi, _1leaf(key, val))
    else:
        newnode = _node_and(node, bi, val)
    while step > 0:
        step -= 1
        parent, pbi = path[step]
        newnode = _node_and(parent, pbi, newnode)
    return newnode, (not found)


def dissoc_root(root, key):
    if root.bitmap == 0:
        return root, False
    path, found, is_beneath = _find_path(root, key)
    if not found:
        return root, False
    step = len(path) - 1
    node, bi = path[step]
    newnode = _node_but(node, bi)
    while step > 0 and newnode.bitmap == 0:
        step -= 1
        parent, pbi = path[step]
        newnode = _node_but(parent, pbi)
    newnode = _collapse(newnode)
    if step == 0 and newnode.bitmap == 0:
        return _empty_node(), True
    while step > 0:
        step -= 1
        parent, pbi = path[step]
        newnode = _node_and(parent, pbi, newnode)
    return newnode, True


def setitem_root(root, key, val, token):
    if root.bitmap == 0:
        return _1leaf(key, val, owner=token), True
    path, found, is_beneath = _find_path(root, key)
    step = len(path) - 1
    node, bi = path[step]
    if not found:
        if not is_beneath:
            fresh = _1leaf(key, val, owner=token)
            newnode = _subjoin(fresh, node)
            newnode.owner = token
        elif node.depth == MAX_DEPTH:
            newnode = _node_set(node, bi, val, token)
        else:
            newnode = _node_set(node, bi, _1leaf(key, val, owner=token), token)
    else:
        newnode = _node_set(node, bi, val, token)
    while step > 0:
        step -= 1
        parent, pbi = path[step]
        ci = _popcount(parent.bitmap & ((1 << pbi) - 1))
        if parent.cells[ci] is newnode:
            return root, (not found)
        newnode = _node_set(parent, pbi, newnode, token)
    return newnode, (not found)


def delitem_root(root, key, token):
    if root.bitmap == 0:
        return root, False
    path, found, is_beneath = _find_path(root, key)
    if not found:
        return root, False
    step = len(path) - 1
    node, bi = path[step]
    newnode = _node_del(node, bi, token)
    while step > 0 and newnode.bitmap == 0:
        step -= 1
        parent, pbi = path[step]
        newnode = _node_del(parent, pbi, token)
    newnode = _collapse(newnode)
    if step == 0 and newnode.bitmap == 0:
        return _empty_node(), True
    while step > 0:
        step -= 1
        parent, pbi = path[step]
        ci = _popcount(parent.bitmap & ((1 << pbi) - 1))
        if parent.cells[ci] is newnode:
            return root, True
        newnode = _node_set(parent, pbi, newnode, token)
    return newnode, True


#===============================================================================
# Public, PHAMT/THAMT-compatible classes.

class AMT:
    """A persistent (immutable) array-mapped trie: a mapping from
    fixed-width integer keys (arbitrary Python ints, including negative
    ones -- see _normalize_key()) to arbitrary Python values, path-
    compressed and structurally shared between versions exactly like
    _c/amt.h's AMT (see the module docstring for how FAT/TFAT relate).

    Never constructed directly with a value; use `AMT.empty` and build up
    from there with `.assoc(key, val)`, or wrap a `TAMT` and call
    `.persistent()`.
    """
    __slots__ = ('_root', '_count')

    empty = None  # set to AMT._wrap(_empty_node(), 0) below the class body.

    def __new__(cls):
        # `AMT()` (no arguments) conveniently returns the canonical empty
        # instance -- phamt.PHAMT is never actually called this way by
        # _dict.py/_list.py/_set.py (they only ever use `.empty`), but
        # supporting it is harmless and convenient for tests.
        return cls.empty

    @classmethod
    def _wrap(cls, root, count):
        self = object.__new__(cls)
        object.__setattr__(self, '_root', root)
        object.__setattr__(self, '_count', count)
        return self

    def __setattr__(self, name, value):
        raise TypeError(f"{type(self).__name__} attributes are immutable")

    @staticmethod
    def _normalize_key(key):
        # See the module docstring's "Iteration order" section: masking a
        # possibly-negative key to an unsigned WIDTH-bit representation
        # (exactly like C's trieint_t reinterpretation of a signed
        # Py_hash_t/ssize_t) preserves ordering *within* same-signed runs of
        # keys, which is all any caller in this codebase relies on.
        return key & MASK_WIDTH

    def __len__(self):
        return self._count

    def __bool__(self):
        return self._count > 0

    def __contains__(self, key):
        found, _ = _lookup(self._root, self._normalize_key(key))
        return found

    def get(self, key, default=None):
        found, val = _lookup(self._root, self._normalize_key(key))
        return val if found else default

    def __getitem__(self, key):
        found, val = _lookup(self._root, self._normalize_key(key))
        if not found:
            raise KeyError(key)
        return val

    def assoc(self, key, val):
        """Returns a new AMT identical to this one except that `key` maps
        to `val`. Does not modify this AMT."""
        key = self._normalize_key(key)
        newroot, added = assoc_root(self._root, key, val)
        return type(self)._wrap(newroot, self._count + 1 if added else self._count)

    def dissoc(self, key):
        """Returns a new AMT identical to this one except that `key` (and
        its value) is absent. A no-op (returns self) if `key` isn't
        present, mirroring amt_butitem()'s no-op contract."""
        key = self._normalize_key(key)
        newroot, removed = dissoc_root(self._root, key)
        if not removed:
            return self
        if newroot.bitmap == 0:
            # A tree that's drained to nothing: hand back the actual
            # canonical `.empty` singleton (like amt_empty()'s own
            # contract in _c/amt.h -- "swap this one-off empty result for
            # the shared canonical one") rather than a distinct, merely
            # equal-by-value empty instance.
            return type(self).empty
        return type(self)._wrap(newroot, self._count - 1)

    def transient(self):
        """Returns a transient copy of this AMT in O(1) time."""
        return _transient_class(type(self))(self)

    def __iter__(self):
        return _iter_node(self._root)

    def __eq__(self, other):
        if self is other:
            return True
        if not isinstance(other, AMT) or type(self) is not type(other):
            return NotImplemented
        if len(self) != len(other):
            return False
        return dict(iter(self)) == dict(iter(other))

    def __hash__(self):
        # Persistent and structurally comparable via __eq__, so a stable
        # hash is meaningful; not used anywhere in this codebase's own
        # hot paths (pdict/pset compute their own hash over elements, not
        # over the AMT/FAT itself), but there's no reason to leave this
        # unhashable given __eq__ is defined.
        return hash((type(self).__name__, frozenset(iter(self))))

    def __repr__(self):
        items = ", ".join(f"{k!r}: {v!r}" for (k, v) in self)
        return f"{type(self).__name__}({{{items}}})"


class TAMT:
    """A transient (in-place-mutable) counterpart to AMT. See the module
    docstring's thread-safety section: intended for single-owner mutation,
    like Python's own `dict`/`list`, not for lock-protected sharing across
    threads."""
    __slots__ = ('_root', '_count', '_token')

    _persistent_class = None  # set below AMT/FAT's class bodies.

    def __new__(cls, source=None):
        self = object.__new__(cls)
        if source is None:
            object.__setattr__(self, '_root', _empty_node())
            object.__setattr__(self, '_count', 0)
        else:
            object.__setattr__(self, '_root', source._root)
            object.__setattr__(self, '_count', source._count)
        object.__setattr__(self, '_token', object())
        return self

    def __setattr__(self, name, value):
        # `_root`/`_count` are mutated internally (via object.__setattr__,
        # bypassing this) by __setitem__/__delitem__ below; nothing external
        # should ever assign to a TAMT's attributes directly.
        raise TypeError(f"{type(self).__name__} attributes are immutable")

    @staticmethod
    def _normalize_key(key):
        return key & MASK_WIDTH

    def __len__(self):
        return self._count

    def __bool__(self):
        return self._count > 0

    def __contains__(self, key):
        found, _ = _lookup(self._root, self._normalize_key(key))
        return found

    def get(self, key, default=None):
        found, val = _lookup(self._root, self._normalize_key(key))
        return val if found else default

    def __getitem__(self, key):
        found, val = _lookup(self._root, self._normalize_key(key))
        if not found:
            raise KeyError(key)
        return val

    def __setitem__(self, key, val):
        key = self._normalize_key(key)
        newroot, added = setitem_root(self._root, key, val, self._token)
        object.__setattr__(self, '_root', newroot)
        if added:
            object.__setattr__(self, '_count', self._count + 1)

    def __delitem__(self, key):
        key = self._normalize_key(key)
        found, _ = _lookup(self._root, key)
        if not found:
            raise KeyError(key)
        newroot, _ = delitem_root(self._root, key, self._token)
        object.__setattr__(self, '_root', newroot)
        object.__setattr__(self, '_count', self._count - 1)

    def __iter__(self):
        return _iter_node(self._root)

    def iter_from(self, key):
        """Iterates over the (key, value) pairs whose key is at least `key`.
        Like iteration, this must not be interleaved with changes."""
        return _iter_node_from(self._root, self._normalize_key(key))

    def persistent(self):
        """Efficiently returns a persistent (AMT) copy of this TAMT.
        Afterward, this TAMT remains independently usable: a further
        mutation claims fresh copies of any nodes the returned persistent
        instance still shares (the same copy-on-write discipline that
        protects any other persistent instance)."""
        cls = self._persistent_class
        root = self._root
        if root.bitmap == 0:
            result = cls.empty
        else:
            result = cls._wrap(root, self._count)
        # Retire this transient's claim on every node it currently owns by
        # rotating to a fresh, private token: the result above shares
        # `root` (and everything reachable from it) directly, with no copy
        # taken here -- this call is meant to be O(1)/O(log n), mirroring
        # tamt_setitem()'s "root is handed off, not copied" contract in
        # _c/amt.h -- so any *further* mutation through this TAMT must
        # claim (copy-on-write) a fresh copy of a node before mutating it
        # in place, even a node this TAMT itself built and previously owned
        # outright, or that mutation would silently corrupt the persistent
        # snapshot just handed back above. This mirrors how a Clojure
        # transient is retired by persistent! (there, continued use after
        # persistent! is instead simply forbidden -- this port allows it,
        # at the cost of this one fresh `object()` allocation, since
        # pdict/tdict's own tdict.persistent() offers no such warning to
        # its callers and is not itself always the last thing done with a
        # given tdict).
        object.__setattr__(self, '_token', object())
        return result

    def __repr__(self):
        items = ", ".join(f"{k!r}: {v!r}" for (k, v) in self)
        return f"{type(self).__name__}({{{items}}})"


def _transient_class(persistent_cls):
    return TAMT if persistent_cls is AMT else TFAT


AMT.empty = AMT._wrap(_empty_node(), 0)
TAMT._persistent_class = AMT


class FAT(AMT):
    """A persistent "fixed-arity trie": in this port, an AMT specialized
    for dense, non-negative, sequentially-assigned integer keys (mirroring
    _c/fat.h's role as pdict/pset's `_els` insertion-order value table).
    See the module docstring's "AMT and FAT are unified" section: FAT is
    behaviorally an AMT here (same path-compressed minimal-tree algorithm),
    kept as a distinct class for interface clarity and so it stays a
    separate, independently-testable type rather than a bare alias."""
    __slots__ = ()

    empty = None  # set to FAT._wrap(_empty_node(), 0) below.

    @staticmethod
    def _normalize_key(key):
        # FAT keys are always already non-negative dense integers (an
        # insertion counter that only grows), so no masking is strictly
        # needed -- but applying the same normalization as AMT costs
        # nothing for a key already in range and keeps both classes
        # trivially substitutable for one another.
        return key & MASK_WIDTH


FAT.empty = FAT._wrap(_empty_node(), 0)


class TFAT(TAMT):
    """Transient counterpart to FAT. See TAMT and the module docstring."""
    __slots__ = ()

    _persistent_class = FAT

    @staticmethod
    def _normalize_key(key):
        return key & MASK_WIDTH
