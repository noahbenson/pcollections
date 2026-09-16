# -*- coding: utf-8 -*-
################################################################################
# pcollections/_trie.py
# Pure-Python persistent and transient integer-keyed tries, the pure-Python
# counterpart of pcollections/_c/{trie,amt,fat}.h. They back the pure-Python
# backend (_dict.py/_list.py/_set.py/_lazy.py).
#
# Interface (used by _dict.py/_list.py/_set.py): `.empty`, `.assoc(k,v)`,
# `.dissoc(k)`, `.get(k,default)`, `__getitem__`, `__len__`, `__iter__`
# yielding `(k,v)` pairs, `TAMT(amt)`/`TFAT(fat)` wrapping a persistent
# instance into a transient one, and `.persistent()` converting back.
#   - AMT / TAMT   -- persistent / transient array-mapped trie (cf. _c/amt.h);
#                     used for pdict/pset's `_idx` (hash(key) ->
#                     FAT-chain-head-index).
#   - FAT / TFAT   -- persistent / transient fixed-arity trie (cf. _c/fat.h);
#                     used for pdict/pset's `_els` (dense insertion-ordered
#                     index -> value-table entry) and plist's `_phamt`
#                     (index -> element, including negative indices from
#                     prepends).
#
# AMT and FAT share one implementation
# -------------------------------------------------------------------------
# In C, FAT differs from AMT only for memory-layout reasons: a fixed
# branching factor with all cells allocated, and a "dense tree" invariant
# (every depth is materialized) instead of AMT's "minimal tree" invariant
# (single-child branches are collapsed). None of that helps in Python, so
# this module implements one minimal-tree, path-compressed trie (the
# module-level functions below) and defines FAT/TFAT as subclasses of
# AMT/TAMT. Everything _dict.py/_list.py/_set.py rely on FAT for (in
# particular ascending-order iteration; see "Iteration order" below) holds.
# The classes differ only in their key-normalization hook
# (`_normalize_key()`), which keeps them separate types matching the C
# `idx` (AMT) vs `els` (FAT) roles.
#
# Memory management: trie nodes are ordinary Python objects, so Python's
# reference counting and garbage collector manage their lifetimes; there is
# no manual refcounting as in _c/trie.h.
#
# Thread-safety: a persistent AMT/FAT and its nodes are never mutated after
# construction (assoc()/dissoc() build new nodes and share the rest), so
# persistent instances can be read from multiple threads without locking. A
# transient TAMT/TFAT, like `dict`/`list`, is meant to have a single owner
# at a time; concurrent mutation is a caller error and is not guarded by
# locks. Because every mutation is an ordinary Python attribute or list
# assignment, such misuse can produce stale or inconsistent values but not
# memory corruption, even on a free-threaded build. test/_trie.py's
# TestTrieThreadStress exercises this.
#
# Claiming discipline: every node has an `owner` slot, normally `None`
# (persistent/shared; copy before mutating). A transient carries a private
# `_token` (a fresh `object()`), and a node may be mutated in place iff
# `node.owner is token`. Mutating operations check this per node as they
# walk down: a claimed node is mutated directly; any other node is copied,
# the copy's `owner` set to `token`, and the copy mutated. This mirrors
# amtnode_set()/amtnode_del() in _c/amt.h.
#
# Iteration order: `_iter_node()` visits bit-indices in ascending order at
# every level, and each level's bit-index is the most significant
# still-undetermined digit of the key (the root holds the highest
# AMT_DIVBITS bits). That is ascending numeric order of the normalized
# (masked-to-unsigned; see _normalize_key()) keys, regardless of tree shape.
# pdict/pset rely on this to iterate `_els` in insertion order. For plist's
# `_phamt`, masking makes negative keys sort after all non-negative ones;
# _list.py's __iter__ handles this by splitting the walk at 0, so only
# ascending order within same-signed keys is required, which masking
# preserves.

__all__ = ["AMT", "TAMT", "FAT", "TFAT"]


#===============================================================================
# Bit-width configuration.
# Mirrors _c/trie.h's AMT_DIVBITS/TRIEINT_WIDTH-derived constants. The width
# is fixed at 64 bits, which matches Py_hash_t (and the C trieint_t) on
# practically every platform.

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
# that many entries, compacted (via popcount(bitmap & below-bit mask), as in
# amtnode_bit2cellindex()) into ascending digit order. There is no FAT-style
# fully allocated layout; see the module comment's "AMT and FAT share one
# implementation" section.
#
# `prefix` holds the key bits from the root down through (but not
# including) this node's own digit -- i.e., every bit *this node's own
# digit position and below* is zeroed, mirroring amt_1leaf()'s
# `key & ~AMT_TWIG_MASK` and amt_subjoin()'s `prefix & gemask(shift)`. This
# is what lets a twig's original key be reconstructed as
# `node.prefix | bitindex` (see _iter_node()) and what _prefix_match() (via
# a *shallower* node's own prefix check) validates against.
#
# `owner`: see the module comment's "claiming discipline" paragraph.
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
    # Never claimed by any transient (owner stays None), like amt_empty()'s
    # singleton in _c/amt.h.
    return _Node(0, 0, 0, [])


def _1leaf(key, val, owner=None):
    """A new twig node containing one key/value pair. Mirrors
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
      - `found` is True iff `key` is present (`path[-1]` is then
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
    normalized-key order -- see the module comment's "Iteration order"
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
    Mirrors amt_collapse()."""
    if node.depth != MAX_DEPTH and len(node.cells) == 1:
        return node.cells[0]
    return node


#===============================================================================
# Mutating (claim-or-mutate-in-place) node primitives, for transient tries.
# Mirror amtnode_set()/amtnode_del(): if `node` is already claimed by this
# transient (node.owner is token), mutate it directly; otherwise claim a
# fresh copy first. See the module comment's "claiming discipline"
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
# True means a key was removed), letting the public class's
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
# Public classes.

class AMT:
    """A persistent (immutable) array-mapped trie: a mapping from
    fixed-width integer keys (arbitrary Python ints, including negative
    ones -- see _normalize_key()) to arbitrary Python values, path-
    compressed and structurally shared between versions like _c/amt.h's
    AMT (see the module comment for how FAT/TFAT relate).

    Never constructed directly with a value; use `AMT.empty` and build up
    from there with `.assoc(key, val)`, or wrap a `TAMT` and call
    `.persistent()`.
    """
    __slots__ = ('_root', '_count')

    empty = None  # set to AMT._wrap(_empty_node(), 0) below the class body.

    def __new__(cls):
        # `AMT()` (no arguments) returns the canonical empty instance. The
        # backend modules use `.empty`; this form is convenient for tests.
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
        # See the module comment's "Iteration order" section: masking a
        # possibly-negative key to an unsigned WIDTH-bit value (like C's
        # trieint_t reinterpretation of a signed Py_hash_t/ssize_t)
        # preserves ordering within same-signed keys, which is all callers
        # rely on.
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
            # Return the canonical `.empty` singleton (as amt_empty() does
            # in _c/amt.h) rather than a new empty instance.
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
        # Consistent with __eq__. pdict/pset hash their elements, not the
        # AMT/FAT, so this is not used on any hot path.
        return hash((type(self).__name__, frozenset(iter(self))))

    def __repr__(self):
        items = ", ".join(f"{k!r}: {v!r}" for (k, v) in self)
        return f"{type(self).__name__}({{{items}}})"


class TAMT:
    """A transient (in-place-mutable) counterpart to AMT. See the module
    comment's thread-safety section: intended for single-owner mutation,
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
        # The result shares `root` without copying, so drop this
        # transient's claim on every node it owns by switching to a fresh
        # token. Later mutations through this TAMT then copy nodes before
        # changing them instead of corrupting the returned snapshot. Unlike
        # Clojure's persistent!, continued use of the transient is allowed,
        # since tdict.persistent() is not always the last operation on a
        # tdict.
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
    """A persistent "fixed-arity trie": an AMT used for dense, non-negative,
    sequentially-assigned integer keys (the role _c/fat.h plays as
    pdict/pset's `_els` insertion-order value table). See the module
    comment's "AMT and FAT share one implementation" section: FAT uses the
    same algorithm as AMT but is a distinct, separately testable type."""
    __slots__ = ()

    empty = None  # set to FAT._wrap(_empty_node(), 0) below.

    @staticmethod
    def _normalize_key(key):
        # FAT keys are normally non-negative already; applying the same
        # normalization as AMT keeps the two classes interchangeable.
        return key & MASK_WIDTH


FAT.empty = FAT._wrap(_empty_node(), 0)


class TFAT(TAMT):
    """Transient counterpart to FAT. See TAMT and the module comment."""
    __slots__ = ()

    _persistent_class = FAT

    @staticmethod
    def _normalize_key(key):
        return key & MASK_WIDTH
