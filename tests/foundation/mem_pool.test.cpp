// mem_pool.h - every class alloc/free/reuse, the large path, realloc, budget refusal, stats.
// Spec: docs/MEMORY.md §8.6, §8.7 test list. Rubric: docs/TESTING.md §7.
// (The Luau-VM-under-the-pool lifecycle test belongs to tests/script/ with the W2 vendor lane.)
#include "runner/tl_test.h"
#include "foundation/mem_pool.h"
#include "vmem_test_api.h"

#include <string.h>

namespace {
bool pool_up(MemPool* p, VMemApi* api, u64 budget) {
    *api = test_vmem_api();
    return pool_init(p, 0x9001u, 512u << 20, budget, api) == ERR_OK;
}
void pool_down(MemPool* p, VMemApi* api) {
    api->release(api->ctx, p->arena.base, p->arena.reserved);
}
}  // namespace

TL_TEST(pool_every_class_alloc_free_reuse, "foundation,mem,smoke") {
    VMemApi api; MemPool p;
    TL_ASSERT_TRUE(pool_up(&p, &api, 256u << 20));

    for (u32 c = 0; c < POOL_CLASS_COUNT; ++c) {
        const u64 csize = (u64)16u << c;
        void* a = pool_alloc(&p, csize);          // exactly the class size
        void* b = pool_alloc(&p, csize - 1u);     // one under: same class
        TL_ASSERT_TRUE(a != nullptr && b != nullptr && a != b);
        TL_EXPECT_EQ(((u64)a) & 15u, (u64)0);     // 16-byte aligned for every class
        TL_EXPECT_EQ(((u64)b) & 15u, (u64)0);
        TL_EXPECT_EQ(pool_stats(&p)->live_count[c], 2u);

        memset(a, (int)(0xA0u + c), (usize)csize);
        pool_free(&p, b);
        void* b2 = pool_alloc(&p, csize);         // freelist reuse: the freed block comes back
        TL_EXPECT_TRUE(b2 == b);
        // Reuse must not have disturbed the neighbour block's bytes.
        u32 bad = 0;
        for (u64 i = 0; i < csize; ++i) { bad += (((u8*)a)[i] != (u8)(0xA0u + c)) ? 1u : 0u; }
        TL_EXPECT_EQ(bad, 0u);
        pool_free(&p, a);
        pool_free(&p, b2);
        TL_EXPECT_EQ(pool_stats(&p)->live_count[c], 0u);
    }
    TL_EXPECT_EQ(pool_stats(&p)->live_bytes, (u64)0);   // cleanup: counts back to baseline
    TL_EXPECT_GT(pool_stats(&p)->peak_bytes, (u64)0);

    // One-past-boundary sizes select the next class (17 -> 32, 33 -> 64).
    void* q17 = pool_alloc(&p, 17u);
    void* q33 = pool_alloc(&p, 33u);
    TL_EXPECT_EQ(pool_stats(&p)->live_count[1], 1u);
    TL_EXPECT_EQ(pool_stats(&p)->live_count[2], 1u);
    pool_free(&p, q17); pool_free(&p, q33);

    // Zero-size: null, no state change.
    TL_EXPECT_NULL(pool_alloc(&p, 0u));
    pool_free(&p, nullptr);   // no-op
    TL_EXPECT_EQ(pool_stats(&p)->live_bytes, (u64)0);

    pool_down(&p, &api);
}

TL_TEST(pool_large_path_and_page_return, "foundation,mem,smoke") {
    VMemApi api; MemPool p;
    TL_ASSERT_TRUE(pool_up(&p, &api, 256u << 20));

    // > 64K goes to the large path; the block is usable end to end.
    const u64 big = (300u << 10) + 7u;   // 300 KB + 7, deliberately unround
    u8* q = (u8*)pool_alloc(&p, big);
    TL_ASSERT_TRUE(q != nullptr);
    TL_EXPECT_EQ(((u64)q) & 15u, (u64)0);
    q[0] = 1u; q[big - 1u] = 2u;
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 1u);
    TL_EXPECT_EQ(pool_stats(&p)->live_bytes, big);

    // Free returns the pages (footprint drops) and the address space is never reused.
    const u64 carved_before = p.carved_bytes;
    pool_free(&p, q);
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 0u);
    TL_EXPECT_EQ(pool_stats(&p)->live_bytes, (u64)0);
    TL_EXPECT_LT(p.carved_bytes, carved_before);
    u8* r = (u8*)pool_alloc(&p, big);
    TL_ASSERT_TRUE(r != nullptr);
    TL_EXPECT_TRUE(r != q);   // bump pointer was not moved back
    pool_free(&p, r);

    // A 64K-exact allocation is the top CLASS (not large), in its two-granule page.
    u8* k64 = (u8*)pool_alloc(&p, 64u * 1024u);
    TL_ASSERT_TRUE(k64 != nullptr);
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 0u);
    TL_EXPECT_EQ(pool_stats(&p)->live_count[POOL_CLASS_COUNT - 1u], 1u);
    memset(k64, 0x42, 64u * 1024u);   // the block must not overlap its own page header
    u8* k64b = (u8*)pool_alloc(&p, 40u * 1024u);   // (32K, 64K] shares the class
    TL_ASSERT_TRUE(k64b != nullptr);
    u32 bad = 0;
    for (u64 i = 0; i < 64u * 1024u; ++i) { bad += (k64[i] != 0x42u) ? 1u : 0u; }
    TL_EXPECT_EQ(bad, 0u);
    pool_free(&p, k64); pool_free(&p, k64b);

    pool_down(&p, &api);
}

TL_TEST(pool_realloc_same_and_cross_class, "foundation,mem,smoke") {
    VMemApi api; MemPool p;
    TL_ASSERT_TRUE(pool_up(&p, &api, 256u << 20));

    // Same class: pointer stable, bytes untouched.
    u8* a = (u8*)pool_alloc(&p, 100u);   // class 128
    memset(a, 0x5C, 100u);
    u8* a2 = (u8*)pool_realloc(&p, a, 128u);
    TL_EXPECT_TRUE(a2 == a);
    u8* a3 = (u8*)pool_realloc(&p, a2, 65u);   // still class 128
    TL_EXPECT_TRUE(a3 == a);

    // Growing class: new block, prefix preserved.
    u8* b = (u8*)pool_realloc(&p, a3, 4000u);   // class 4K
    TL_ASSERT_TRUE(b != nullptr && b != a);
    u32 bad = 0;
    for (u32 i = 0; i < 100u; ++i) { bad += (b[i] != 0x5Cu) ? 1u : 0u; }
    TL_EXPECT_EQ(bad, 0u);

    // Shrinking class: new block, min(old,new) preserved.
    u8* c = (u8*)pool_realloc(&p, b, 20u);   // class 32
    TL_ASSERT_TRUE(c != nullptr && c != b);
    bad = 0;
    for (u32 i = 0; i < 20u; ++i) { bad += (c[i] != 0x5Cu) ? 1u : 0u; }
    TL_EXPECT_EQ(bad, 0u);

    // Class -> large -> class round trip.
    u8* d = (u8*)pool_realloc(&p, c, 200u << 10);
    TL_ASSERT_TRUE(d != nullptr);
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 1u);
    TL_EXPECT_EQ(d[0], (u8)0x5C);
    memset(d, 0x77, 200u << 10);
    u8* e = (u8*)pool_realloc(&p, d, (200u << 10) + 100u);   // same carve: pointer stable
    TL_EXPECT_TRUE(e == d);
    u8* f = (u8*)pool_realloc(&p, e, 48u);
    TL_ASSERT_TRUE(f != nullptr);
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 0u);
    TL_EXPECT_EQ(f[0], (u8)0x77);

    // realloc(null, n) == alloc; realloc(p, 0) == free -> null.
    u8* g = (u8*)pool_realloc(&p, nullptr, 64u);
    TL_ASSERT_TRUE(g != nullptr);
    TL_EXPECT_NULL(pool_realloc(&p, g, 0u));
    pool_free(&p, f);
    TL_EXPECT_EQ(pool_stats(&p)->live_bytes, (u64)0);

    pool_down(&p, &api);
}

TL_TEST(pool_budget_refuses_with_null, "foundation,mem,fast") {
    VMemApi api; MemPool p;
    TL_ASSERT_TRUE(pool_up(&p, &api, 256u * 1024u));   // budget: 4 granules

    // Two class pages fit; the third carve would cross the budget -> null, state intact.
    void* a = pool_alloc(&p, 100u);            // carves one 64K page (class 128)
    void* b = pool_alloc(&p, 5000u);           // carves one 64K page (class 8K)
    TL_ASSERT_TRUE(a != nullptr && b != nullptr);
    void* big = pool_alloc(&p, 200u << 10);    // needs 256K carve: budget says no
    TL_EXPECT_NULL(big);
    TL_EXPECT_EQ(pool_stats(&p)->large_count, 0u);

    // A fitting alloc still works after the refusal (no partial state).
    void* c2 = pool_alloc(&p, 120u);           // reuses the class-128 page
    TL_EXPECT_NOT_NULL(c2);

    // Freeing a large block gives its budget back.
    pool_free(&p, a); pool_free(&p, b); pool_free(&p, c2);
    void* d = pool_alloc(&p, 100u << 10);      // 128K carve fits beside the two pages
    TL_ASSERT_TRUE(d != nullptr);
    TL_EXPECT_NULL(pool_alloc(&p, 100u << 10));   // second one does not fit
    pool_free(&p, d);                              // pages returned
    TL_EXPECT_NOT_NULL(pool_alloc(&p, 100u << 10));   // now it fits again

    // A single request over the whole budget: refused outright.
    TL_EXPECT_NULL(pool_alloc(&p, 1u << 20));

    pool_down(&p, &api);
}
