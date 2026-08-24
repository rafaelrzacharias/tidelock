#pragma once
// ---------------------------------------------------------------------------------------------
// sort.h - sort_u32_kv / sort_u64_kv: LSD radix, base 256, stable. The ONE sort in the runtime.
//
// Spec: docs/CONTAINERS.md §4 (design), §8.5 (this header).
// Purpose: the broadphase's sort, render's `sort_u64_kv` batch order (docs/RENDER2D.md §9.3.5),
//   any deterministic integer-keyed ordering. There is no generic `sort<T,Cmp>` in the runtime -
//   sim sorts are on integer keys by rule (docs/NETCODE.md §14.1 heritage) and a stable integer
//   radix is deterministic by construction; a comparison sort exists only for `tools/`
//   (docs/CONTAINERS.md §6).
// Invariants: stable (equal keys preserve relative input order, across every pass - the counting-
//   sort placement scans input in ascending index order); a pass whose histogram has exactly one
//   nonzero bucket is skipped (the byte carries no ordering information at this position - the
//   docs' own "all-equal keys early-out" test, §8.7). n up to 2^32-1. The scratch buffer comes
//   from the caller's Scratch (docs/CONTAINERS.md §4 - "comes from the scratch arena").
// Determinism: pure function of (keys, vals, n) - counting sort has no comparator, no branch on
//   key VALUE beyond bucket index, so it is bit-identical for the same input on every target.
// Threading: none - reentrant given distinct Scratch instances (one per worker,
//   docs/MEMORY.md §1.3).
// Includes: foundation/tl_types.h, foundation/scratch.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/scratch.h"

// Sorts (keys[0..n), vals[0..n)) ascending by key, in place, stable. 4 passes (bytes 0..3 of each
// u32 key), each skipped if the byte carries no ordering information (single-bucket histogram).
void sort_u32_kv(u32* keys, u32* vals, u32 n, Scratch* s);

// As sort_u32_kv, 8 passes over a u64 key with a u32 value.
void sort_u64_kv(u64* keys, u32* vals, u32 n, Scratch* s);
