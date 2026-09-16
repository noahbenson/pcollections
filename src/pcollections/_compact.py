# -*- coding: utf-8 -*-
################################################################################
# pcollections/_compact.py
# Shared tombstone-based deletion and periodic compaction helpers for pdict/
# tdict (_dict.py) and pset/tset (_set.py), matching the `els`/`idx` design of
# pcollections/_c/dict.c.h (see its header comment) and _c/set.c.h.
#
# pdict/pset (and their transient counterparts) encode their contents as
# a pair of tries: `_idx` (an AMT mapping hash(key)/hash(element) -> the
# FAT-chain-head index) and `_els` (a FAT mapping a dense, insertion-ordered
# integer index -> a collision-chain link, `(payload, next_index)`, where
# `payload` is `(key, val)` for a dict or a bare element for a set).
#
# Removing an entry unlinks it from its collision chain (repointing `_idx`
# or the previous link's `next`) but does not remove its slot from `_els`:
# removing a slot would require renumbering every later index, invalidating
# `_idx` entries and `next` pointers. Instead the slot is overwritten with a
# tombstone (see TOMBSTONE/is_tombstone() below). Tombstoned slots are
# unlinked first, so chain-walking lookups never reach them.
#
# Without compaction, add/remove churn would grow the insertion counter
# (`top`), and so the trie depth, without bound. Each deletion therefore
# checks whether `ndeleted` (slots tombstoned since the last compaction) has
# crossed should_compact()'s threshold, and if so rebuilds `_els`/`_idx` with
# the live entries renumbered 0..count-1 in insertion order (see
# _compact_els() in _dict.py and _set.py).
#
# Deleting the entry in the last slot instead removes that slot and any
# tombstones just before it, lowering `top` (see trim_tail() below).

# The tombstone sentinel. A dead `_els` slot's `payload` (the first element
# of its `(payload, next_index)` value) is overwritten with this shared
# object, which is never a key/value pair or set element, so `is_tombstone()`
# is an identity check. Collision-chain walks only reach live slots and need
# not check for it.
TOMBSTONE = object()


def is_tombstone(payload):
    """True if `payload` -- the first element of an `_els` slot's
    `(payload, next_index)` value -- marks a tombstoned (deleted-but-not-
    yet-compacted) slot."""
    return payload is TOMBSTONE


# Compaction thresholds, the same as DICT_COMPACT_ABS_THRESHOLD/
# DICT_COMPACT_FRAC_NUM/DEN in pcollections/_c/dict.c.h (and the matching
# constants in _c/set.c.h).
COMPACT_ABS_THRESHOLD = 1024 * 1024
COMPACT_FRAC_NUM = 3
COMPACT_FRAC_DEN = 10


def should_compact(count, ndeleted):
    """Mirrors dict_should_compact()/_c/dict.c.h: True once enough `_els`
    slots have been tombstoned by deletions (relative to how many live elements
    remain) that a full rebuild is worthwhile."""
    if ndeleted <= 0:
        return False
    if ndeleted > COMPACT_ABS_THRESHOLD:
        return True
    return ndeleted * COMPACT_FRAC_DEN > count * COMPACT_FRAC_NUM


def trim_tail(els, top, ndeleted):
    """Removes the tombstones at the end of the persistent FAT `els`, whose
    indices are 0..top-1, and returns `(els, top, ndeleted)` with `top` and
    `ndeleted` lowered to match. A deletion of the entry in the last slot
    calls this after tombstoning it, so the slot `top - 1` always holds a live
    entry (unless `top` is 0): the last entry can be found directly (see
    `last_payload()`), and repeatedly removing the last entry leaves no
    tombstones. (See tail trimming in pcollections/_c/dict.c.h.)"""
    while top > 0 and els[top - 1][0] is TOMBSTONE:
        top -= 1
        ndeleted -= 1
        els = els.dissoc(top)
    return (els, top, ndeleted)


def trim_tail_transient(t):
    """Like `trim_tail()`, for a transient `t` (a tdict or tset), in place."""
    (els, top, ndeleted) = (t._els, t._top, t._ndeleted)
    while top > 0 and els[top - 1][0] is TOMBSTONE:
        top -= 1
        ndeleted -= 1
        del els[top]
    object.__setattr__(t, '_top', top)
    object.__setattr__(t, '_ndeleted', ndeleted)


def last_payload(coll):
    """The `payload` of the last live slot of `coll` (a pdict, tdict, pset, or
    tset), which is slot `coll._top - 1` (see `trim_tail()`), or raises
    `KeyError` if `coll` is empty."""
    if coll._count == 0:
        raise KeyError(f"{type(coll).__name__} is empty")
    return coll._els[coll._top - 1][0]


def live_payloads(els):
    """Yields every live (non-tombstoned) slot's `payload` from `els` (a
    FAT or TFAT), in ascending index (== original insertion) order."""
    for (_ii, (payload, _next)) in els:
        if payload is not TOMBSTONE:
            yield payload
