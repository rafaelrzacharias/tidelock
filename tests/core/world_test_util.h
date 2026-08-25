#pragma once
// world_test_util.h - the shared World fixture for tests/core: a real registry + scratch over
// the test-owned VMemApi, plus the tick driver the loop lane will eventually own.
// Spec context: docs/ECS.md §7, §10.8; docs/FRAME-LOOP.md §8.2 (the wiring order mirrored here).
#include "runner/tl_test.h"
#include "core/world.h"
#include "foundation/vmem_test_api.h"
#include <string.h>

// Test components shared across tests/core TUs (ODR-safe: the macro emits inline definitions).
#define TL_FIELDS_WPos(X, XA, XH) \
    X(i32, x) X(i32, y)
TL_COMPONENT(WPos)

#define TL_FIELDS_WVel(X, XA, XH) \
    X(i32, dx) X(i32, dy)
TL_COMPONENT(WVel)

#define TL_FIELDS_WCfg(X, XA, XH) \
    X(u64, gravity) X(u32, mode) XA(u8, _pad0, 4)
TL_COMPONENT_FLAGS(WCfg, COMP_SINGLETON)

#define TL_FIELDS_WEvSpawned(X, XA, XH) \
    XH(Entity, who) X(u32, n)
TL_COMPONENT(WEvSpawned)

struct WorldFixture {
    VMemApi api;
    ArenaRegistry reg;
    Scratch scratch;
    World w;
};

// Fixture storage lives in statics, never on the stack: World carries comps[1024] (~256 KB)
// and a Windows child process gets a 1 MB stack - two stack fixtures crashed every Windows CI
// leg while Linux's 8 MB default hid it. Tests are exempt from the src/ static ban
// (docs/TESTING.md §8 R-2); every user re-runs world_fixture_init, which re-zeroes the slot.
inline WorldFixture* wt_fixture(u32 slot) {
    static WorldFixture s[4];
    TL_CHECK(slot < 4u);
    return &s[slot];
}

// Registry + scratch + an empty world. Registration of components/events/systems is each
// test's own business (order is part of what the tests pin).
inline bool world_fixture_init(WorldFixture* f, u64 seed) {
    f->api = test_vmem_api();
    memset(&f->reg, 0, sizeof(f->reg));
    if (scratch_init(&f->scratch, "wt.scratch"_id, 32u * 1024u * 1024u, &f->api) != ERR_OK) { return false; }
    WorldDesc d;
    memset(&d, 0, sizeof(d));
    d.seed = seed;
    return world_init(&f->w, &f->reg, &f->scratch, &f->api, &d) == ERR_OK;
}

// The standard three-component registration most tests want: WPos, WVel, the WCfg singleton,
// and the WEvSpawned event, in that fixed order.
inline void world_fixture_register_std(WorldFixture* f) {
    world_register_component(&f->w, &WPos_info);
    world_register_component(&f->w, &WVel_info);
    world_register_component(&f->w, &WCfg_info);
    world_register_event(&f->w, &WEvSpawned_info, 64u);
}

// One sim tick, the shape docs/FRAME-LOOP.md §8.3 gives engine_tick_once (render phases and
// interpolation are other lanes'): run the five sim phases (each ends in a command barrier),
// swap events, advance the tick.
inline void wt_tick(World* w) {
    for (u32 p = PHASE_FIRST; p <= (u32)PHASE_SIM_LAST; ++p) { run_phase(w, (Phase)p); }
    world_events_swap(w);
    w->state->tick += 1u;
}
