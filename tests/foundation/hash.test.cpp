// hash.h - tl_hash64 (vendored rapidhash) and NameHash/operator""_id.
// Spec: docs/DETERMINISM.md §4, §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/hash.h"

TL_TEST(hash_seed_constant, "foundation,smoke,fast") {
    // CANON.md: TL_HASH_SEED = 0x7469646c6f636b31 ("tidelock1").
    TL_EXPECT_EQ(TL_HASH_SEED, (u64)0x7469646c6f636b31ull);
}

// Known-answer vectors. They were first taken from the vendored implementation itself, which
// proves only that the code equals itself; they are now re-derived independently by
// tools/rapidhash_ref.py (rapidhash v3 COMPACT+FAST written out in Python from the algorithm,
// never run through the header) and agree - `python tools/rapidhash_ref.py --check`. Upstream
// ships no vectors at the pinned commit, which is why that reference exists at all
// (docs/DETERMINISM.md §9.5). A change to vendor/rapidhash/rapidhash.h, to the
// RAPIDHASH_COMPACT/FAST config or to TL_HASH_SEED moves these.
TL_TEST(hash_known_answer_vectors, "foundation,smoke,fast") {
    TL_EXPECT_EQ(tl_hash64(nullptr, (usize)0, TL_HASH_SEED), (u64)0xa9289ddd02105011ull);
    TL_EXPECT_EQ(tl_hash64("a", (usize)1, TL_HASH_SEED), (u64)0x7425490bf2d09d36ull);
    TL_EXPECT_EQ(tl_hash64("abc", (usize)3, TL_HASH_SEED), (u64)0x4080f4b00e1442b9ull);
    TL_EXPECT_EQ(tl_hash64("the quick brown fox jumps over the lazy dog", (usize)43, TL_HASH_SEED),
                 (u64)0xc077819f6f45f995ull);
    // A 128-byte input crosses rapidhash's RAPIDHASH_COMPACT unroll boundary (> 112 bytes).
    char buf[128];
    for (u32 i = 0; i < 128; ++i) buf[i] = (char)(u8)i;
    TL_EXPECT_EQ(tl_hash64(buf, (usize)128, TL_HASH_SEED), (u64)0x63d79b273d2c819cull);
}

TL_TEST(hash_seed_sensitivity, "foundation,smoke,fast") {
    // Same bytes, different seed -> different hash (state hash per docs/DETERMINISM.md §4 relies
    // on this: TL_HASH_SEED changing must move every hash).
    const u64 a = tl_hash64("player", (usize)6, TL_HASH_SEED);
    const u64 b = tl_hash64("player", (usize)6, TL_HASH_SEED ^ 1);
    TL_EXPECT_NE(a, b);
}

TL_TEST(hash_len_sensitivity, "foundation,smoke,fast") {
    // A length change must move the hash even when the extra bytes are zero (used bytes only,
    // never capacity - docs/DETERMINISM.md §4).
    char buf[4] = { 'a', 'b', 0, 0 };
    const u64 h2 = tl_hash64(buf, (usize)2, TL_HASH_SEED);
    const u64 h3 = tl_hash64(buf, (usize)3, TL_HASH_SEED);
    TL_EXPECT_NE(h2, h3);
}

TL_TEST(name_hash_fnv1a_known_answer, "foundation,smoke,fast") {
    // FNV-1a 64 offset basis with no bytes folded in is the offset itself.
    TL_EXPECT_EQ(""_id, (NameHash)0xcbf29ce484222325ull);
    // Single byte 'a' (0x61): (offset ^ 0x61) * prime.
    TL_EXPECT_EQ("a"_id, (NameHash)0xaf63dc4c8601ec8cull);
    TL_EXPECT_EQ("player"_id, fnv1a64("player", (usize)6));
}

TL_TEST(name_hash_high_bytes_are_unsigned, "foundation,smoke,fast") {
    // The u8 cast in fnv1a64 is the whole cross-ISA claim of this hash and had no test: `char` is
    // signed on x86-64 and unsigned on aarch64, so `h ^= u64(s[i])` over a byte >= 0x80 sign-
    // extends on one and not the other (docs/CPP-SUBSET.md §5). Sim-TU literals must stay ASCII
    // so this input cannot appear there, but fnv1a64 takes a pointer and a length - an interner
    // or an asset name can hand it any byte, and the cast is what makes that safe.
    //
    // The golden is the UNSIGNED reading, computed by hand (FNV-1a 64 over 80 ff 41 c3 a9). This
    // machine has signed `char`, so deleting the u8 cast turns it into 0xd05320c608f3293b and the
    // test fails here; on an aarch64 host it would pass either way, which is exactly why the
    // value is pinned to the unsigned reading and not to "whatever this compiler does".
    const char bytes[5] = { (char)0x80, (char)0xff, (char)0x41, (char)0xc3, (char)0xa9 };
    TL_EXPECT_EQ(fnv1a64(bytes, (usize)5), (NameHash)0xb83a05347aabdb3bull);
}

TL_TEST(name_hash_constexpr_equals_runtime, "foundation,smoke,fast") {
    // "lit"_id evaluated at compile time equals the same bytes hashed at runtime through the
    // non-constexpr call path (docs/DETERMINISM.md §9.5).
    constexpr NameHash ct = "player_spawn"_id;
    const char* s = "player_spawn";
    const NameHash rt = fnv1a64(s, (usize)12);
    TL_EXPECT_EQ(ct, rt);
}

TL_TEST(name_hash_distinct_literals, "foundation,smoke,fast") {
    TL_EXPECT_NE("player"_id, "playerx"_id);
    TL_EXPECT_NE("a"_id, "b"_id);
    // Order matters: FNV-1a is not commutative over the byte sequence.
    TL_EXPECT_NE("ab"_id, "ba"_id);
}
