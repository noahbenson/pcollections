# -*- coding: utf-8 -*-
################################################################################
# pcollections/_compact.py
# Shared tombstone-based deletion + periodic compaction helpers for pdict/
# tdict (_dict.py) and pset/tset (_set.py), mirroring pcollections/_c/dict.c's
# `els`/`idx` design (see that file's header comment for the full rationale;
# _c/set.c follows the identical pattern for sets).
#
# Both pdict/pset (and their transient counterparts) encode their contents as
# a pair of tries: `_idx` (an AMT mapping hash(key)/hash(element) -> the
# FAT-chain-head index) and `_els` (a FAT mapping a dense, insertion-ordered
# integer index -> a collision-chain link, `(payload, next_index)`, where
# `payload` is `(key, val)` for a dict or a bare element for a set).
#
# Removing an entry unlinks it from its collision chain (repointing `_idx`
# or the previous link's `next`, exactly as a real removal would) but does
# NOT remove its slot from `_els` outright: `_els`' dense, insertion-ordered
# indexing means removing a slot from the middle would require renumbering
# every later index to close the gap, which would silently invalidate every
# other `_idx` entry and collision-chain `next` pointer at or past that
# index. So a deleted entry's slot is instead *overwritten* with a
# tombstone (see TOMBSTONE/is_tombstone() below) -- physically present in
# `_els`, burning that index, but unreachable from `_idx` (a tombstoned
# slot is always first fully unlinked from its collision chain before being
# tombstoned, so ordinary chain-walking lookups never encounter one).
#
# Left unchecked, a dict/set that churns through many add/remove cycles
# would see its insertion counter (`top`) -- and therefore its trie depth --
# grow without bound even though the live element count stays small. So
# every mutating deletion checks, after it completes, whether `ndeleted`
# (slots burned by deletions since the last compaction) has crossed
# should_compact()'s threshold, and if so, rebuilds `_els`/`_idx` from
# scratch with the live entries renumbered 0..count-1 in their original
# (insertion) order, via compact_els() below.

# The tombstone sentinel. A dead `_els` slot's `payload` (the first element
# of its `(payload, next_index)` value) is overwritten with this shared
# object -- never a real key/value pair or set element, so `is_tombstone()`
# is a simple identity check, and ordinary code that reads a definitely-live
# slot's payload (every collision-chain walk in pdict/pset/tdict/tset) never
# needs to check for it at all, by construction.
TOMBSTONE = object()


def is_tombstone(payload):
    """True if `payload` -- the first element of an `_els` slot's
    `(payload, next_index)` value -- marks a tombstoned (deleted-but-not-
    yet-compacted) slot."""
    return payload is TOMBSTONE


# Compaction thresholds -- identical to DICT_COMPACT_ABS_THRESHOLD/
# DICT_COMPACT_FRAC_NUM/DEN in pcollections/_c/dict.c (and the matching
# constants in _c/set.c).
COMPACT_ABS_THRESHOLD = 1024 * 1024
COMPACT_FRAC_NUM = 3
COMPACT_FRAC_DEN = 10


def should_compact(count, ndeleted):
    """Mirrors dict_should_compact()/_c/dict.c: True once enough `_els`
    slots have been burned by deletions (relative to how many live elements
    remain) that a full rebuild is worthwhile."""
    if ndeleted <= 0:
        return False
    if ndeleted > COMPACT_ABS_THRESHOLD:
        return True
    return ndeleted * COMPACT_FRAC_DEN > count * COMPACT_FRAC_NUM


def live_payloads(els):
    """Yields every live (non-tombstoned) slot's `payload` from `els` (a
    FAT or TFAT), in ascending index (== original insertion) order."""
    for (_ii, (payload, _next)) in els:
        if payload is not TOMBSTONE:
            yield payload
