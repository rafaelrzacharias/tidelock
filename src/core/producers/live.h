#pragma once
// ---------------------------------------------------------------------------------------------
// live.h - the Live InputProducer: fold_tick over the platform's raw event ring.
//
// Spec: docs/INPUT.md §4 (producers table), §5 (dev-only ImGui mask), §9.3 (fold_tick, this
//   file's algorithm), §7 (test list: edge derivation, analog quantization, deadzone shapes,
//   SOCD, chord specificity, context-switch synthetic release).
// Purpose: the ONLY producer that reads real devices (docs/INPUT.md §0's load-bearing rule).
//   Maintains continuous device state (key/mouse/pad down, mouse motion accumulation) across
//   ticks, folds the platform's RawEvent ring plus that state into one InputFrame per produce()
//   call through the registered ActionMap.
// Invariants: chord specificity (docs/INPUT.md §2/§9.3) is resolved across DIGITAL-family
//   bindings (DEV_KEY/DEV_MOUSE_BUTTON/DEV_PAD_BUTTON) sharing one physical (dev, code): among
//   bindings whose modifier requirement is currently satisfied, only the ones with the STRICTLY
//   HIGHEST modifier-bit count for that physical input stay "active" this tick; ties (equal
//   count) all stay active (the doc's "resolved against one snapshot" - no further tiebreak is
//   specified). ANALOG bindings never suppress each other - their raw values SUM
//   (docs/INPUT.md §9.3: "raw = Sigma bound axis values"). Per-action down/pressed/released is
//   recomputed from scratch every call (never accumulated), so a context switch's "synthetic
//   release" (docs/INPUT.md §9.3) falls out for free: an action whose new context excludes every
//   currently-held binding naturally resolves to down=false this tick, which IS the released edge.
// Deviations recorded (docs are "best so far", TODO.md carries these): (1) DEV_MOUSE_AXIS reuses
//   Binding.code_pos as an axis selector (0=dx,1=dy) and DEV_PAD_AXIS reuses code_neg as the pad
//   index / code_pos as the axis index - the doc's Binding struct has no dedicated axis-selector
//   field. (2) DZ_RADIAL is applied per-axis (identical to DZ_AXIAL) - true 2D-joint radial
//   deadzone needs a paired-axis concept this struct does not carry; filed for whoever adds one.
//   (3) The pointer is an IDENTITY passthrough (window px treated as world-space units) - the
//   real screen->world camera projection is render2d's (RENDER2D.md §2), not landed yet.
// Determinism: this producer is explicitly NOT deterministic (real devices, real time) - legal
//   because it is this peer's own input and only the InputFrame it emits is sent/applied
//   (docs/INPUT.md §1). Analog quantization is the one place a float becomes sim-reachable data,
//   through fx::from_f32_quantized (docs/FX-PALETTE.md §6) - RNE at the row's quantum.
// Threading: single-threaded; live_producer_init is init-only, live_produce runs once per tick
//   from engine_frame's accumulator loop.
// Includes: core/input.h, core/action_map.h.
// ---------------------------------------------------------------------------------------------
#include "core/input.h"
#include "core/action_map.h"
#include "foundation/ring.h"
#include "platform/platform.h"

enum : u32 {
    LIVE_MAX_KEYS         = 512u,   // SDL_Scancode's range comfortably fits
    LIVE_MAX_MOUSE_BUTTONS = 8u,    // platform.h RawEvent.mouse_button.button is 1..5
    LIVE_MAX_PADS         = 4u,
    LIVE_MAX_PAD_AXES     = 8u,
    LIVE_MAX_PAD_BUTTONS  = 32u,
    LIVE_MAX_BINDINGS     = 128u,   // action_map_init's max_bindings must not exceed this
    LIVE_MAX_EVENTS_PER_TICK = 1024u,   // the drain buffer cap (core/loop.h's ENGINE_EVENT_RING_CAP mirrors this)
};

// A dev-only capture mask (docs/INPUT.md §5): when set, mouse/keyboard RawEvents are dropped
// from the fold before device state updates, so a UI click does not also fire a bound action.
// Not part of the replay stream (dev-only, non-deterministic by nature). Nullable.
struct ImGuiCaptureApi { void* ctx; void (*want_capture)(void* ctx, u8* want_mouse, u8* want_keyboard); };

// Per-binding SOCD state for DEV_KEYS_AXIS bindings with SOCD_LAST_WINS/SOCD_FIRST_WINS
// (docs/INPUT.md §2); indexed by the binding's position in ActionMap::bindings.
struct LiveSocdState { i8 last_dir; u8 first_locked; u8 _pad0[2]; };

// The Live producer's persistent state (docs/INPUT.md §9.3's "state:" line, plus the chord/SOCD
// bookkeeping this lane adds - see this header's contract block).
struct LiveProducer {
    ActionMap* map;
    u8 local_slot;
    u8 last_context;
    u8 _pad0[2];
    ImGuiCaptureApi capture;                 // capture.ctx may be null: no masking

    u8 key_down[LIVE_MAX_KEYS];
    u8 prev_key_down[LIVE_MAX_KEYS];         // last tick's snapshot, for fresh-press detection
    u16 mods;                                // current modifier bits (platform.h RawEvent.key.mods)

    u8 mouse_button_down[LIVE_MAX_MOUSE_BUTTONS];
    i32 mouse_x, mouse_y;                    // last known logical window position
    f32 mouse_dx_accum, mouse_dy_accum;      // accumulated this tick; reset every produce() call

    i16 pad_axis_raw[LIVE_MAX_PADS][LIVE_MAX_PAD_AXES];
    u8  pad_button_down[LIVE_MAX_PADS][LIVE_MAX_PAD_BUTTONS];
    u8  pad_connected[LIVE_MAX_PADS];

    u8  prev_action_down[MAX_ACTIONS];       // edge derivation (docs/INPUT.md §9.3)
    LiveSocdState socd[LIVE_MAX_BINDINGS];
};

// Wires lp to `map` and `local_slot`; zeroes all device/edge state. `capture` may be
// ImGuiCaptureApi{} (ctx null) for "no masking". TL_CHECK(map->bindings.cap <= LIVE_MAX_BINDINGS).
void live_producer_init(LiveProducer* lp, ActionMap* map, u8 local_slot, ImGuiCaptureApi capture);

// The InputProducer::produce fn (docs/INPUT.md §4/§9.3): drains the platform's raw-event ring
// (already ImGui-masked by the caller or by this call - see live.cpp), folds it into device
// state, resolves every action, fills out[local_slot], sets live_mask = 1 << local_slot. Never
// returns PRODUCE_WAIT (docs/INPUT.md §4: "Live... never wait"). `events`/`event_count` is the
// platform ring's contents for this call, already drained by the caller (engine_frame owns the
// ring; this keeps live.cpp free of a RingBuffer<RawEvent> dependency it would otherwise need
// only for draining).
ProduceResult live_produce_frame(LiveProducer* lp, const RawEvent* events, u32 event_count,
                                 u64 tick, InputFrame* out, u8* live_mask);

// Binds a LiveProducer to the RawEvent ring it should drain (docs/INPUT.md §4's InputProducer is
// a plain (ctx, tick, out, live_mask) fn-ptr with no ring parameter, so the ring reference has to
// travel inside ctx). `ring` must outlive the InputProducer registration (Engine owns it).
struct LiveProducerCtx { LiveProducer* lp; RingBuffer<RawEvent>* ring; };

// The InputProducer::produce fn (ctx = LiveProducerCtx*, docs/INPUT.md §4): drains *ring fully
// into a bounded stack buffer (capped at LIVE_MAX_EVENTS_PER_TICK; a ring holding more than that
// between two produce() calls silently drops the oldest excess - engine_frame's ring is sized to
// match, core/loop.h ENGINE_EVENT_RING_CAP), then calls live_produce_frame.
ProduceResult live_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask);
