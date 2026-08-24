#pragma once
// ---------------------------------------------------------------------------------------------
// strview.h - StrView: the non-owning string parameter every runtime string touches.
//
// Spec: docs/CONTAINERS.md §5 (design), §8.6 (this header); docs/CANON.md "Types" (the struct
//   shape - `StrView { const char* ptr; u32 len; }` - is CANON, not this lane's to redefine).
// Purpose: no `String` class anywhere in the runtime - StrView is every string parameter; owning
//   copies are `arena_copy` (docs/MEMORY.md §4), long-lived names go through the interner
//   (docs/CONTAINERS.md §5).
// Invariants: non-owning - the caller's bytes must outlive the view. Not necessarily
//   NUL-terminated (len is authoritative, never strlen). Sim-TU string literals must stay ASCII
//   (docs/CPP-SUBSET.md §5) - sv_hash reuses hash.h's fnv1a64, which already closes the signed-
//   char hole with an explicit u8 cast.
// Determinism: sv_hash is the same FNV-1a family as NameHash - pure function of bytes, target-
//   invariant. No strings or string-hash ORDER may enter authoritative sim state
//   (docs/DETERMINISM.md §2.7/§8) - interned ids and integers only inside the tick.
// Threading: none - values.
// Includes: foundation/tl_types.h, foundation/hash.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/hash.h"

struct StrView {
    const char* ptr;
    u32 len;
};
static_assert(__is_trivially_copyable(StrView), "");

// A StrView over a NUL-terminated literal/C string. Not constexpr-under-clang for an arbitrary
// `const char*` (no constexpr strlen without <string.h>, which is not sim-legal to include here
// for a runtime function) - callers with a literal use `sv_lit` for the compile-time form.
inline StrView sv(const char* s) {
    u32 n = 0;
    while (s[n] != '\0') { n += 1u; }
    return StrView{ s, n };
}

// The constexpr form for a string literal, where the array bound is known at compile time.
template <usize N>
constexpr StrView sv_lit(const char (&s)[N]) { return StrView{ s, (u32)(N - 1u) }; }

// Byte-for-byte equality. Pure, never fails.
inline bool sv_eq(StrView a, StrView b) {
    if (a.len != b.len) { return false; }
    for (u32 i = 0; i < a.len; ++i) {
        if (a.ptr[i] != b.ptr[i]) { return false; }
    }
    return true;
}

// Same family as NameHash (docs/CANON.md "Types") - the FNV-1a byte loop already reads through an
// explicit u8 cast, so this is target-invariant for the same reason fnv1a64 is (docs/CPP-SUBSET.md
// section 5).
inline NameHash sv_hash(StrView s) { return fnv1a64(s.ptr, s.len); }

// True iff s begins with prefix (the empty prefix always matches). Pure, never fails.
inline bool sv_starts_with(StrView s, StrView prefix) {
    if (prefix.len > s.len) { return false; }
    for (u32 i = 0; i < prefix.len; ++i) {
        if (s.ptr[i] != prefix.ptr[i]) { return false; }
    }
    return true;
}

// Splits at byte index `at` into [0,at) and [at,len). TL_CHECK(at <= len) in all tiers.
inline void sv_split_at(StrView s, u32 at, StrView* out_lo, StrView* out_hi) {
    TL_CHECK(at <= s.len);
    *out_lo = StrView{ s.ptr, at };
    *out_hi = StrView{ s.ptr + at, s.len - at };
}
