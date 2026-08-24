// hash.h - tl_hash64 (vendored rapidhash) and NameHash/operator""_id.
// Spec: docs/DETERMINISM.md §4, §9.5. Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "foundation/hash.h"

TL_TEST(hash_seed_constant, "foundation,smoke,fast") {
    // CANON.md: TL_HASH_SEED = 0x7469646c6f636b31 ("tidelock1").
    TL_EXPECT_EQ(TL_HASH_SEED, (u64)0x7469646c6f636b31ull);
}

// Known-answer vectors: computed once from the pinned vendored implementation and pinned here as
// goldens, so a future change to vendor/rapidhash/rapidhash.h or the RAPIDHASH_COMPACT/FAST
// config is caught (docs/DETERMINISM.md §9.5 "rapidhash known-answer vectors from upstream").
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
