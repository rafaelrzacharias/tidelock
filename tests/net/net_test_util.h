#pragma once
// ---------------------------------------------------------------------------------------------
// net_test_util.h - test-local input-frame FIXTURES for the net lane (RR-17 ruling, 2026-08-26:
//   "input-frame geometry pinned to docs/INPUT.md §1's constants and test-local frame
//   fixtures"). The geometry itself lives in net/wire.h's mirror; what is local to the tests is
//   the business of building concrete frames to feed the codec, which is core/input.h's Live
//   producer's job in the real engine and must not be invented in src/ by this lane.
//
// Spec: docs/NETCODE.md §20.6 (T0-T2), §20.2.2 (the column layout these frames are encoded to),
//   docs/INPUT.md §1 (the frame). Rubric: docs/TESTING.md §7.
// Determinism: every builder is a pure function of its arguments and a caller-supplied seed;
//   no clock, no global state, so a fixture is reproducible from its call site alone.
// ---------------------------------------------------------------------------------------------
#include "net/wire.h"

// A frame with every action clear and the pointer at the origin - the codec's ZERO_FRAME, built
// through the same door the tests use for every other fixture so a mismatch shows up here.
inline WireFrame nt_zero_frame(u32 tick) {
    WireFrame f = wire_zero_frame();
    f.tick = tick;
    return f;
}

// A frame holding `down_mask`'s actions down (bit a -> actions[a]), value 1, flags bit0 set,
// pointer at (px, py). Digital actions only - the shape an idle-to-active column exercises.
inline WireFrame nt_digital_frame(u32 tick, u32 down_mask, i32 px, i32 py) {
    WireFrame f = wire_zero_frame();
    f.tick = tick;
    f.pointer_x = px;
    f.pointer_y = py;
    for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
        if ((down_mask >> a) & 1u) { f.actions[a].value = 1; f.actions[a].flags = 1u; }
    }
    return f;
}

// Sets one action's raw (value, flags) - the door the analog and edge-flag rows need, including
// the deliberately inconsistent combinations the archive's flag-escape path exists for.
inline void nt_set_action(WireFrame* f, u32 action, i8 value, u8 flags) {
    TL_ASSERT(f != nullptr && action < NET_FRAME_MAX_ACTIONS);
    f->actions[action].value = value;
    f->actions[action].flags = flags;
}

// True when two frames are byte-equal in every field the codec is required to preserve. The
// codec sets `tick` from base_tick + i rather than transmitting it (docs/NETCODE.md §20.2.2),
// so a round-trip comparison that included `tick` would be testing the caller, not the codec -
// nt_frames_equal_payload is the honest comparison and nt_frames_equal is the strict one.
inline bool nt_frames_equal_payload(const WireFrame* a, const WireFrame* b) {
    if (a->pointer_x != b->pointer_x || a->pointer_y != b->pointer_y) { return false; }
    for (u32 i = 0; i < NET_FRAME_MAX_ACTIONS; ++i) {
        if (a->actions[i].value != b->actions[i].value) { return false; }
        if (a->actions[i].flags != b->actions[i].flags) { return false; }
    }
    return true;
}

// Payload equality plus the decoder-assigned tick.
inline bool nt_frames_equal(const WireFrame* a, const WireFrame* b) {
    return a->tick == b->tick && nt_frames_equal_payload(a, b);
}

// A tiny stateless keyed generator for the fuzz and volume fixtures. Not foundation/rng.h's
// rng_for: these fixtures are test scaffolding outside the sim, and a self-contained splitmix64
// keeps a fixture's bytes a pure function of (seed, counter) with no dependency on a sim-facing
// keying scheme that would make a fixture's meaning move when that scheme is retuned.
inline u64 nt_mix64(u64 seed, u64 counter) {
    u64 z = seed + 0x9E3779B97F4A7C15ull * (counter + 1ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
