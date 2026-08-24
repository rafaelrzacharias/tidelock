#pragma once
// ---------------------------------------------------------------------------------------------
// strview.h - StrView, the non-owning string every string parameter in the codebase speaks.
//
// Spec: docs/CONTAINERS.md §8.6 (pinned shape); docs/CANON.md ("Types").
// Purpose: `{ ptr, len }` over caller-owned bytes; no allocation, no ownership, no NUL assumption.
// Invariants: `ptr` may be null only when `len == 0`. Content is not required to be NUL-terminated;
//   callers that need one (platform file paths) copy into a NUL-terminated buffer at the call site.
// Determinism: `sv_hash` is the same FNV-1a 64 as `NameHash` (docs/CANON.md), so an interned name
//   hash and a StrId lookup agree. `char` is legal in sim code for message literals only
//   (docs/CPP-SUBSET.md §5) - sv_hash reads bytes as `u8` specifically to stay signedness-safe.
// Threading: none - a value type, no state.
// Includes: foundation/tl_types.h only.
//
// Landed from the W1 platform lane (2026-08-24), not the containers lane: PLATFORM.md §9's
// contract header (PlatformConfig, FileApi paths) needs the type and the containers lane had not
// started. Transcribed verbatim from the pinned shape in CONTAINERS.md §8.6 - same precedent as
// tl_assert.h landing from the fx lane (LESSONS.md). The containers lane owns this file, and its
// Interner/fmt neighbours, from the moment it starts.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

struct StrView {
    const char* ptr;
    u32 len;
};

// Compile-time view over a string literal - `sv("lit")`. N includes the trailing NUL; len excludes it.
template <usize N>
constexpr StrView sv(const char (&lit)[N]) {
    return StrView{ lit, (u32)(N - 1) };
}

// Byte-exact equality; two different-length views are never equal regardless of content.
constexpr bool sv_eq(StrView a, StrView b) {
    if (a.len != b.len) return false;
    for (u32 i = 0; i < a.len; ++i) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

// FNV-1a 64 over the raw bytes, unsigned - the same algorithm and seed as `NameHash` (docs/CANON.md).
constexpr u64 sv_hash(StrView s) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < s.len; ++i) {
        h ^= (u64)(u8)s.ptr[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// True iff `s` is at least as long as `prefix` and matches it byte for byte.
constexpr bool sv_starts_with(StrView s, StrView prefix) {
    if (s.len < prefix.len) return false;
    return sv_eq(StrView{ s.ptr, prefix.len }, prefix);
}

// Splits at the first occurrence of `sep` (an ASCII byte - `u8`, not `char`: signedness differs
// between x86-64 and aarch64, docs/CPP-SUBSET.md §5). On a match, `*head` excludes `sep` and
// `*tail` starts just past it; returns false (head/tail untouched) when `sep` does not occur.
constexpr bool sv_split_at(StrView s, u8 sep, StrView* head, StrView* tail) {
    for (u32 i = 0; i < s.len; ++i) {
        if ((u8)s.ptr[i] == sep) {
            *head = StrView{ s.ptr, i };
            *tail = StrView{ s.ptr + i + 1, s.len - i - 1 };
            return true;
        }
    }
    return false;
}

static_assert(__is_trivially_copyable(StrView), "");
static_assert(sizeof(StrView) == 16, "const char* (8) + u32 (4) + 4 pad on a 64-bit target");
static_assert(sv("abc").len == 3, "");
static_assert(sv_eq(sv("abc"), sv("abc")), "");
static_assert(!sv_eq(sv("abc"), sv("abd")), "");
static_assert(sv_starts_with(sv("abcdef"), sv("abc")), "");
static_assert(!sv_starts_with(sv("ab"), sv("abc")), "");
