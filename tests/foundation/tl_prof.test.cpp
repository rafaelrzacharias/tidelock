// tl_prof.test.cpp - docs/TOOLING.md §9.5 "prof_zero_alloc_and_tree". ProfState is a plain
// namespace-scope static (RR-7, matching probe.cpp/log.cpp's precedent - see prof.cpp's top
// comment), never a VMemArena, so "zero alloc" here means "no allocation function exists on this
// path at all" (stronger than an arena high-water check) rather than an arena-offset delta.
#include "runner/tl_test.h"
#include "foundation/tl_prof.h"

TL_TEST(prof_begin_end_records_one_node, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    tl_prof_begin(0, "scope.a"_id, "scope.a", PROF_NODE_NONE);
    tl_prof_end(0);
    tl_prof_frame_end(7u);

    TL_ASSERT_EQ(tl_prof_ring_count(), 1u);
    const ProfFrame* f = tl_prof_ring_at(0);
    TL_ASSERT_EQ(f->node_count, 1u);
    TL_EXPECT_EQ(f->tick, 7u);
    TL_EXPECT_EQ(f->nodes[0].key, "scope.a"_id);
    TL_EXPECT_EQ(f->nodes[0].parent, (u32)PROF_NODE_NONE);
    TL_EXPECT_EQ(f->nodes[0].depth, (u16)0);
    TL_EXPECT_EQ(f->nodes[0].worker, (u8)0);
    TL_EXPECT_GE(f->nodes[0].t_end, f->nodes[0].t_begin);
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_nested_scopes_build_correct_tree, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    // outer { inner_a; inner_b { innermost } }
    tl_prof_begin(0, "outer"_id, "outer", PROF_NODE_NONE);
      tl_prof_begin(0, "inner_a"_id, "inner_a", PROF_NODE_NONE);
      tl_prof_end(0);
      tl_prof_begin(0, "inner_b"_id, "inner_b", PROF_NODE_NONE);
        tl_prof_begin(0, "innermost"_id, "innermost", PROF_NODE_NONE);
        tl_prof_end(0);
      tl_prof_end(0);
    tl_prof_end(0);
    tl_prof_frame_end(0);

    const ProfFrame* f = tl_prof_ring_at(0);
    TL_ASSERT_EQ(f->node_count, 4u);
    // Recorded in begin-order: outer(0) inner_a(1) inner_b(2) innermost(3).
    TL_EXPECT_EQ(f->nodes[0].key, "outer"_id);
    TL_EXPECT_EQ(f->nodes[0].depth, (u16)0);
    TL_EXPECT_EQ(f->nodes[0].parent, (u32)PROF_NODE_NONE);

    TL_EXPECT_EQ(f->nodes[1].key, "inner_a"_id);
    TL_EXPECT_EQ(f->nodes[1].depth, (u16)1);
    TL_EXPECT_EQ(f->nodes[1].parent, 0u);

    TL_EXPECT_EQ(f->nodes[2].key, "inner_b"_id);
    TL_EXPECT_EQ(f->nodes[2].depth, (u16)1);
    TL_EXPECT_EQ(f->nodes[2].parent, 0u);

    TL_EXPECT_EQ(f->nodes[3].key, "innermost"_id);
    TL_EXPECT_EQ(f->nodes[3].depth, (u16)2);
    TL_EXPECT_EQ(f->nodes[3].parent, 2u);

    for (u32 i = 0; i < f->node_count; ++i) { TL_EXPECT_GE(f->nodes[i].t_end, f->nodes[i].t_begin); }
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_scope_macro_matches_manual_begin_end, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    TL_PROF_SCOPE("macro.scope") { TL_PROF_SCOPE("macro.child") {} }
    tl_prof_frame_end(0);
    const ProfFrame* f = tl_prof_ring_at(0);
    TL_ASSERT_EQ(f->node_count, 2u);
    TL_EXPECT_EQ(f->nodes[0].key, "macro.scope"_id);
    TL_EXPECT_EQ(f->nodes[1].key, "macro.child"_id);
    TL_EXPECT_EQ(f->nodes[1].parent, 0u);
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_1000_frames_ring_wraps_and_stays_balanced, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    for (u32 frame = 0; frame < 1000u; ++frame) {
        tl_prof_begin(0, "per_frame"_id, "per_frame", PROF_NODE_NONE);
        tl_prof_begin(0, "per_frame.child"_id, "per_frame.child", PROF_NODE_NONE);
        tl_prof_end(0);
        tl_prof_end(0);
        tl_prof_frame_end((u64)frame);
    }
    // The ring holds at most PROF_RING_FRAMES (60) frames - it must not grow past that even
    // after 1000 frames (no allocation, fixed buffers - docs/TOOLING.md section 9.3.1).
    TL_ASSERT_EQ(tl_prof_ring_count(), (u32)PROF_RING_FRAMES);
    const ProfFrame* latest = tl_prof_ring_at(0);
    TL_EXPECT_EQ(latest->tick, 999u);
    TL_EXPECT_EQ(latest->node_count, 2u);
    const ProfFrame* older = tl_prof_ring_at((u32)PROF_RING_FRAMES - 1u);
    TL_EXPECT_EQ(older->tick, (u64)(1000u - PROF_RING_FRAMES));
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_worker_overflow_caps_node_count_without_crashing, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    // PROF_WORKER_NODES_CAP is 8192; push past it (8200 scopes, unnested so depth stays under
    // PROF_STACK_CAP) - "overflow at 8193 scopes counted, not crashed" (docs/TOOLING.md §9.5).
    for (u32 i = 0; i < 8200u; ++i) {
        tl_prof_begin(0, "flood"_id, "flood", PROF_NODE_NONE);
        tl_prof_end(0);
    }
    tl_prof_frame_end(0);
    const ProfFrame* f = tl_prof_ring_at(0);
    TL_EXPECT_EQ(f->node_count, (u32)PROF_WORKER_NODES_CAP);   // capped, not 8200
    TL_EXPECT_EQ(f->dropped, 0u);   // frame-level cap (16384) never reached by one worker
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_two_workers_merge_in_worker_order, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    // Record worker 1's scope FIRST, worker 0's second - merge order must still be worker-index
    // order (0 before 1), not call order (docs/TOOLING.md §9.3.1: "merge every worker's nodes
    // ... in worker order").
    tl_prof_begin(1, "w1.scope"_id, "w1.scope", PROF_NODE_NONE);
    tl_prof_end(1);
    tl_prof_begin(0, "w0.scope"_id, "w0.scope", PROF_NODE_NONE);
    tl_prof_end(0);
    tl_prof_frame_end(0);

    const ProfFrame* f = tl_prof_ring_at(0);
    TL_ASSERT_EQ(f->node_count, 2u);
    TL_EXPECT_EQ(f->nodes[0].key, "w0.scope"_id);
    TL_EXPECT_EQ(f->nodes[0].worker, (u8)0);
    TL_EXPECT_EQ(f->nodes[1].key, "w1.scope"_id);
    TL_EXPECT_EQ(f->nodes[1].worker, (u8)1);
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST(prof_counters_set_and_add, "foundation") {
#if TL_DEV
    tl_prof_test_reset();
    tl_prof_counter("draw_calls"_id, "draw_calls", 10, 0);
    tl_prof_counter("draw_calls"_id, "draw_calls", 5, 1);   // ADD: 10 + 5 = 15
    tl_prof_counter("particles"_id, "particles", 3, 0);
    TL_ASSERT_EQ(tl_prof_counter_count(), 2u);
    TL_EXPECT_EQ(tl_prof_counter_at(0)->value, (i64)15);
    TL_EXPECT_EQ(tl_prof_counter_at(1)->value, (i64)3);

    tl_prof_frame_end(0);
    const ProfFrame* f = tl_prof_ring_at(0);
    TL_EXPECT_EQ(f->counters[0], (i64)15);
    TL_EXPECT_EQ(f->counters[1], (i64)3);
    tl_prof_test_reset();
#else
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); no symbol in this tier");
#endif
}

TL_TEST_EXPECT_FATAL(prof_frame_end_asserts_worker0_balanced, "foundation,slow") {
#if TL_DEV
    tl_prof_test_reset();
    ++t->checks;   // the child never returns to a normal exit; this just touches `t` (see schedule_unknown_label_is_fatal's precedent)
    tl_prof_begin(0, "unbalanced"_id, "unbalanced", PROF_NODE_NONE);   // no matching tl_prof_end
    tl_prof_frame_end(0);   // TL_ASSERT(workers[0].depth == 0) must fire
#else
    // The trigger (TL_ASSERT) is dev-only; runner_core.h's tl_child_verdict honors this SKIP.
    TL_SKIP("profiler is dev-only (TOOLING.md section 9); TL_ASSERT does not fire outside TL_DEV");
#endif
}
