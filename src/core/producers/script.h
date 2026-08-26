#pragma once
// ---------------------------------------------------------------------------------------------
// script.h - the Script InputProducer: a test's scripted timeline (docs/INPUT.md §4/§9.4).
//
// Spec: docs/INPUT.md §4 (producers table), §9.4 (ScriptedEvent/ScriptProducer, this header's
//   structures), §7 (test list: "Script producer timing (exact tick)").
// Purpose: headless tests build a fixed, deterministic timeline (`press`/`hold`/`set`) instead of
//   a real device; the harness advances tick-by-tick and gets exactly the scripted edges at
//   exactly the scripted ticks - the same mechanism headless CI and Hovel use.
// Invariants: `events` must be appended in NON-DECREASING tick order (TL_CHECK) - `produce`
//   advances a monotonic cursor rather than searching, so an out-of-order append would silently
//   skip or duplicate ticks. `live_mask` is fixed at init (docs/INPUT.md §4: "single-player is
//   live_mask = 0b1 with slot 0") - a scripted timeline never changes which slots are live.
// Determinism: pure function of the recorded events and the tick sequence it is asked for - the
//   canonical "known input" half of a determinism test (docs/INPUT.md §7's replay pairing).
// Threading: single-threaded; script_producer_init and the script_press/hold/set builders are
//   test-setup-time only (called before the first produce()).
// Includes: core/input.h, foundation/array.h.
// ---------------------------------------------------------------------------------------------
#include "core/input.h"
#include "foundation/array.h"

// docs/INPUT.md §9.4. SET holds until the next event on the same (slot, action, docs/INPUT.md
// §9.4's op vocabulary); PRESS is a one-tick pulse (auto-clears on the FOLLOWING produce() call,
// before that call's own events apply); HOLD_START/HOLD_END bracket a continuous hold.
enum ScriptOp : u8 { SCRIPT_OP_SET = 0, SCRIPT_OP_PRESS = 1, SCRIPT_OP_HOLD_START = 2, SCRIPT_OP_HOLD_END = 3 };

// docs/INPUT.md §9.4, field for field.
struct ScriptedEvent {
    u64      tick;
    ActionId action;
    i8       value;
    u8       op;      // ScriptOp
    u8       slot;
    u8       _pad0[3];
};
static_assert(sizeof(ScriptedEvent) == 16u, "docs/INPUT.md section 9.4, explicit padding");

// docs/INPUT.md §9.4: `cur` is replaced here by explicit down/value/pending-release state (see
// this header's contract block for PRESS's auto-clear mechanism) - the doc's `ActionState cur[]`
// name is kept as a comment for traceability, not as a member (ActionState.flags would have to be
// recomputed from `down` every tick anyway, so storing it twice bought nothing).
struct ScriptProducer {
    Array<ScriptedEvent> events;   // sorted by tick (append-time TL_CHECK enforces it)
    u32 cursor;
    u8  live_mask;                 // fixed at init (docs/INPUT.md §4)
    u8  _pad0[3];
    i8  value[MAX_PEERS][MAX_ACTIONS];
    u8  down[MAX_PEERS][MAX_ACTIONS];
    u8  pending_release[MAX_PEERS][MAX_ACTIONS];   // PRESS's one-tick pulse bookkeeping
};

// Sizes `events` from `arena` (array_init_fixed); zeroes all state. `live_mask` is fixed for the
// producer's lifetime (docs/INPUT.md §4).
void script_producer_init(ScriptProducer* sp, VMemArena* arena, u32 max_events, u8 live_mask);

// Appends one event. TL_CHECK: tick is >= the last appended event's tick (non-decreasing order,
// this header's contract block); slot < MAX_PEERS; action < MAX_ACTIONS. TL_FATAL on `events`
// overflow (array_push's fixed-array contract).
void script_add_event(ScriptProducer* sp, u64 tick, ActionId action, i8 value, ScriptOp op, u8 slot);

// Convenience builders (docs/INPUT.md §9.4: "helpers press(a, tick=10), hold(a, v, from, to)").
// One-tick pulse: down this tick only, auto-clears on the next produce() call.
void script_press(ScriptProducer* sp, ActionId action, u64 tick, i8 value, u8 slot);
// Continuous hold over [from, to]: HOLD_START at `from`, HOLD_END at `to`. TL_CHECK(from <= to).
void script_hold(ScriptProducer* sp, ActionId action, i8 value, u64 from, u64 to, u8 slot);
// Sets the value/down state at `tick`, holding until the next event on this (slot, action).
void script_set(ScriptProducer* sp, ActionId action, i8 value, u64 tick, u8 slot);

// The InputProducer::produce fn (ctx = ScriptProducer*): applies every event at exactly `tick`
// (array order), derives edges against the PRE-mutation down state, fills every slot's frame
// (non-live slots get a zero frame - harmless, live_mask says which ones matter), and returns
// PRODUCE_READY (docs/INPUT.md §4: Script never waits).
ProduceResult script_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask);
