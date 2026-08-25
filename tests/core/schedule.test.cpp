// schedule.h - topo-sort determinism: registration-order ties, before/after refinement, cycle /
// unknown-label / cross-phase / duplicate-label fatals, and rebuild reproducibility.
// Spec: docs/ECS.md §3, §10.6, §10.8 (schedule.test.cpp line). Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"
#include "core/schedule.h"
#include "foundation/vmem_test_api.h"

namespace {

void sys_noop(World*) {}

// A schedule + its meta arena + scratch, fresh per test.
struct SchedFixture {
    VMemApi api;
    VMemArena meta;
    Scratch scratch;
    Schedule s;
};

bool sched_fixture_init(SchedFixture* f) {
    f->api = test_vmem_api();
    if (vmem_arena_init(&f->meta, "sched.meta"_id, 4u * 1024u * 1024u, 0u, &f->api) != ERR_OK) { return false; }
    if (scratch_init(&f->scratch, "sched.scratch"_id, 4u * 1024u * 1024u, &f->api) != ERR_OK) { return false; }
    schedule_init(&f->s, &f->meta);
    return true;
}

// Registers a system with optional single before/after label (0 = none).
void sched_add(SchedFixture* f, NameHash label, Phase phase, NameHash before, NameHash after) {
    SystemDesc d;
    d.fn = sys_noop;
    d.label = label;
    d.phase = phase;
    d.reads = Span<const ComponentId>{ nullptr, 0 };
    d.writes = Span<const ComponentId>{ nullptr, 0 };
    d.before = Span<const NameHash>{ before != 0u ? &before : nullptr, before != 0u ? 1u : 0u };
    d.after = Span<const NameHash>{ after != 0u ? &after : nullptr, after != 0u ? 1u : 0u };
    d.flags = 0;
    schedule_register(&f->s, &f->meta, &d);   // spans point at locals: registration must copy
}

// The label at order position i (asserting the index chain), for readable expectations.
NameHash order_label(const Schedule* s, u32 i) {
    return s->systems.data[s->order.data[i]].d.label;
}

}  // namespace

TL_TEST(schedule_ties_break_by_registration_order_across_phases, "core,ecs,schedule,smoke,fast") {
    // Interleaved registration across phases, no edges: each phase's slice is registration
    // order, and phase_begin tiles the order array in phase-enum order.
    SchedFixture f;
    TL_ASSERT_TRUE(sched_fixture_init(&f));
    sched_add(&f, "u1"_id, PHASE_UPDATE, 0, 0);
    sched_add(&f, "f1"_id, PHASE_FIRST, 0, 0);
    sched_add(&f, "u2"_id, PHASE_UPDATE, 0, 0);
    sched_add(&f, "l1"_id, PHASE_LAST, 0, 0);
    sched_add(&f, "u3"_id, PHASE_UPDATE, 0, 0);
    schedule_build(&f.s, &f.scratch);
    TL_ASSERT_EQ(f.s.order.count, 5u);
    TL_EXPECT_EQ(order_label(&f.s, 0), "f1"_id);
    TL_EXPECT_EQ(order_label(&f.s, 1), "u1"_id);
    TL_EXPECT_EQ(order_label(&f.s, 2), "u2"_id);
    TL_EXPECT_EQ(order_label(&f.s, 3), "u3"_id);
    TL_EXPECT_EQ(order_label(&f.s, 4), "l1"_id);
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_FIRST], 0u);
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_PRE_UPDATE], 1u);
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_UPDATE], 1u);      // empty phase: zero-width slice
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_POST_UPDATE], 4u);
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_LAST], 4u);
    TL_EXPECT_EQ(f.s.phase_begin[PHASE_COUNT], 5u);
    // phase_pos is each system's own coordinate in the order.
    for (u32 i = 0; i < 5u; ++i) { TL_EXPECT_EQ(f.s.systems.data[f.s.order.data[i]].phase_pos, i); }
}

TL_TEST(schedule_before_and_after_refine_registration_order, "core,ecs,schedule,fast") {
    // C declares before A: Kahn's ready set starts {B, C}; the lowest reg_index (B) goes first,
    // then C, then A - registration order everywhere the edge does not force otherwise.
    SchedFixture f;
    TL_ASSERT_TRUE(sched_fixture_init(&f));
    sched_add(&f, "a"_id, PHASE_UPDATE, 0, 0);
    sched_add(&f, "b"_id, PHASE_UPDATE, 0, 0);
    sched_add(&f, "c"_id, PHASE_UPDATE, "a"_id, 0);
    schedule_build(&f.s, &f.scratch);
    TL_ASSERT_EQ(f.s.order.count, 3u);
    TL_EXPECT_EQ(order_label(&f.s, 0), "b"_id);
    TL_EXPECT_EQ(order_label(&f.s, 1), "c"_id);
    TL_EXPECT_EQ(order_label(&f.s, 2), "a"_id);

    // B declares after C in a fresh schedule: A, then C, then B.
    SchedFixture g;
    TL_ASSERT_TRUE(sched_fixture_init(&g));
    sched_add(&g, "a"_id, PHASE_UPDATE, 0, 0);
    sched_add(&g, "b"_id, PHASE_UPDATE, 0, "c"_id);
    sched_add(&g, "c"_id, PHASE_UPDATE, 0, 0);
    schedule_build(&g.s, &g.scratch);
    TL_EXPECT_EQ(order_label(&g.s, 0), "a"_id);
    TL_EXPECT_EQ(order_label(&g.s, 1), "c"_id);
    TL_EXPECT_EQ(order_label(&g.s, 2), "b"_id);
}

TL_TEST(schedule_build_is_reproducible_and_dirty_scratch_proof, "core,ecs,schedule,determinism,fast") {
    // Two schedules, same registrations, one built against a pre-dirtied scratch: identical
    // order arrays (the build's only inputs are the registrations).
    SchedFixture a;
    SchedFixture b;
    TL_ASSERT_TRUE(sched_fixture_init(&a));
    TL_ASSERT_TRUE(sched_fixture_init(&b));
    const NameHash labels[6] = { "s0"_id, "s1"_id, "s2"_id, "s3"_id, "s4"_id, "s5"_id };
    for (u32 i = 0; i < 6u; ++i) {
        sched_add(&a, labels[i], (Phase)(i % 3u), 0, i == 4u ? labels[1] : (NameHash)0);
        sched_add(&b, labels[i], (Phase)(i % 3u), 0, i == 4u ? labels[1] : (NameHash)0);
    }
    // Dirty b's scratch, then release it, so the build sees reused (poisoned) bytes.
    void* junk = scratch_push(&b.scratch, 64u * 1024u, 16u);
    memset(junk, 0x5A, 64u * 1024u);
    scratch_reset(&b.scratch);
    schedule_build(&a.s, &a.scratch);
    schedule_build(&b.s, &b.scratch);
    TL_ASSERT_EQ(a.s.order.count, b.s.order.count);
    for (u32 i = 0; i < a.s.order.count; ++i) {
        TL_EXPECT_EQ(a.s.order.data[i], b.s.order.data[i]);
    }
    // Scratch is back at its entry mark after each build (the scope pair held).
    TL_EXPECT_EQ(arena_mark(&a.scratch.a), 0u);
}

TL_TEST_EXPECT_FATAL(schedule_cycle_is_fatal, "core,ecs,schedule,fatal") {
    SchedFixture f;
    if (!sched_fixture_init(&f)) { return; }
    sched_add(&f, "a"_id, PHASE_UPDATE, "b"_id, 0);   // a before b
    sched_add(&f, "b"_id, PHASE_UPDATE, "a"_id, 0);   // b before a
    ++t->checks;
    schedule_build(&f.s, &f.scratch);   // must TL_FATAL: before/after cycle
}

TL_TEST_EXPECT_FATAL(schedule_unknown_label_is_fatal, "core,ecs,schedule,fatal") {
    SchedFixture f;
    if (!sched_fixture_init(&f)) { return; }
    sched_add(&f, "a"_id, PHASE_UPDATE, 0, "ghost"_id);
    ++t->checks;
    schedule_build(&f.s, &f.scratch);   // must TL_FATAL: unknown label
}

TL_TEST_EXPECT_FATAL(schedule_cross_phase_label_is_fatal, "core,ecs,schedule,fatal") {
    // Phases already order across phases; a cross-phase before/after is a registration bug
    // (the same fatal path as an unknown label - schedule.h contract).
    SchedFixture f;
    if (!sched_fixture_init(&f)) { return; }
    sched_add(&f, "a"_id, PHASE_FIRST, 0, 0);
    sched_add(&f, "b"_id, PHASE_UPDATE, 0, "a"_id);
    ++t->checks;
    schedule_build(&f.s, &f.scratch);   // must TL_FATAL: label crosses phases
}

TL_TEST_EXPECT_FATAL(schedule_duplicate_label_is_fatal, "core,ecs,schedule,fatal") {
    SchedFixture f;
    if (!sched_fixture_init(&f)) { return; }
    sched_add(&f, "a"_id, PHASE_UPDATE, 0, 0);
    ++t->checks;
    sched_add(&f, "a"_id, PHASE_LAST, 0, 0);   // must TL_FATAL: labels are edge keys
}
