#pragma once
// ---------------------------------------------------------------------------------------------
// input.h - the InputFrame airlock: ActionState/InputFrame geometry, MAX_ACTIONS/MAX_PEERS,
//   InputProducer, PeerSlots.
//
// Spec: docs/INPUT.md §0 (the load-bearing rule), §1 (the frame), §4 (producers), §8 (rulings),
//   §9.1/§9.2 (this header's files). The action map itself (§2) is core/action_map.h.
// Purpose: the sealed-sim input airlock - the sim reads only InputFrame, never live devices.
//   Whoever produces the frame (Live/Script/Replay/Network) is swappable through one fn-ptr seam
//   (docs/INPUT.md §4), so headless testing, replay and lockstep are one mechanism.
// Invariants: InputFrame is 76 B, explicitly laid out (docs/INPUT.md §1) - it follows the WIRE
//   STRUCT DISCIPLINE (explicit offsets, static_assert-pinned) but is NOT built through the
//   TL_WIRE_STRUCT macro: that macro prepends a `u32 format_version`, which would break the 76 B
//   pin `net/wire.h`'s `WireFrame` geometry mirror shares (docs/NETCODE.md's RR-17 ruling names
//   the eventual `#include "core/input.h"` handoff - filed in TODO.md, not done here: net/ is
//   another lane's module, docs/WORKFLOW.md cone discipline). `MAX_PEERS`'s VALUE is decided by
//   CANON.md ("owned by NETCODE" there); the C++ symbol's home is RULED here (2026-08-26, Rafael,
//   TODO.md RR-24): CANON.md names no header, so `core/input.h` is the owner as the seam's
//   consumer-facing side, and `net/wire.h` includes this header for it (a scoped exception into
//   `net/wire.h`, that one line plus the include, nothing else) rather than carrying its own copy
//   - `tools/audit/includes.py` MODULE_DAG already has net depend on core, so this needs no new
//   edge. PeerSlots is a singleton component (docs/INPUT.md §8 R-2): registered like any other
//   C++ component (world_register_component, elsewhere - wiring is app/wiring.cpp's job,
//   docs/FRAME-LOOP.md §8.2 step 4), snapshotted/hashed/restored with the world (docs/FRAME-LOOP.md
//   §7 R-1).
// Determinism: InputFrame is all-integer (docs/INPUT.md §1) - no float ever enters the sim
//   through it. Per-tick frame storage lives on the Engine (core/loop.h), not World: it is
//   recorded by the LAST-phase recorder but is not itself hashed state (only its EFFECTS, via the
//   action map's edit commands, are).
// Threading: single-threaded; input_set_producer and world_register_component(&PeerSlots_info)
//   are init-only.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, core/reflect.h (PeerSlots' component
//   door), <stddef.h> (offsetof).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "core/reflect.h"
#include <stddef.h>

// docs/CANON.md "MAX_PEERS" (value 8, NETCODE-owned there); this is the one C++ home (ruled
// 2026-08-26, Rafael, TODO.md RR-24) - net/wire.h includes this header for it instead of carrying
// its own copy.
constexpr u32 MAX_PEERS = 8u;
static_assert(MAX_PEERS <= 8u, "slot_mask/live_mask/hold bitmaps are one byte wide (docs/NETCODE.md section 20.2.2)");

// docs/INPUT.md §1 R: compile-time; changing it is a wire-format version bump.
enum : u32 { MAX_ACTIONS = 32u };

// The dense per-world action id (docs/INPUT.md §2): registration order, < MAX_ACTIONS.
using ActionId = u16;
enum : u16 { ACTION_ID_NONE = 0xFFFFu };

// docs/INPUT.md §1: 2 B. value: digital 0/1, analog -127..127 (snorm8, quantized at capture).
// flags: bit0 down, bit1 pressed-this-tick, bit2 released-this-tick.
struct ActionState {
    i8 value;
    u8 flags;
};
static_assert(sizeof(ActionState) == 2u, "docs/INPUT.md section 1");
static_assert(__is_trivially_copyable(ActionState), "docs/CPP-SUBSET.md section 1: POD");

enum : u8 { AS_DOWN = 1u << 0, AS_PRESSED = 1u << 1, AS_RELEASED = 1u << 2 };
constexpr u8 AS_FLAG_BITS = (u8)(AS_DOWN | AS_PRESSED | AS_RELEASED);

// docs/INPUT.md §1 (the wire-struct DISCIPLINE, not the TL_WIRE_STRUCT macro - see the header
// comment). All-integer; no float ever enters the sim through this struct. The pointer is
// world-space, computed by the Live producer through the render camera (docs/INPUT.md §1: legal
// because it is this peer's own input and every peer applies the same transmitted integer).
struct InputFrame {
    ActionState actions[MAX_ACTIONS];   //  0, 64 B, indexed by ActionId (dense u16 < MAX_ACTIONS)
    i32 pointer_x;                      // 64, world-space pos_t raw bits, quantized at capture
    i32 pointer_y;                      // 68
    u32 tick;                           // 72, low 32 bits of the u64 world tick (docs/FRAME-LOOP.md section 1)
};
static_assert(sizeof(InputFrame) == 76u, "docs/INPUT.md section 1");
static_assert(offsetof(InputFrame, actions)   ==  0u, "docs/INPUT.md section 1 frame layout");
static_assert(offsetof(InputFrame, pointer_x) == 64u, "docs/INPUT.md section 1 frame layout");
static_assert(offsetof(InputFrame, pointer_y) == 68u, "docs/INPUT.md section 1 frame layout");
static_assert(offsetof(InputFrame, tick)      == 72u, "docs/INPUT.md section 1 frame layout");
static_assert(__is_trivially_copyable(InputFrame), "docs/CPP-SUBSET.md section 1: POD, memcpy-safe");

// Every ActionState {0,0}, pointer (0,0), tick 0 - the "no input yet" baseline a producer falls
// back to (mirrors net's wire_zero_frame, docs/NETCODE.md section 20.3(b)).
constexpr InputFrame input_zero_frame() { return InputFrame{}; }

enum ProduceResult : u8 { PRODUCE_READY = 0, PRODUCE_WAIT = 1 };

// docs/INPUT.md §4: a fn-ptr, not a system - a producer gates the tick (PRODUCE_WAIT), which a
// system cannot. `out` must hold MAX_PEERS entries; `live_mask` bit i = slot i has a frame this
// tick. Set once at init (world building, docs/FRAME-LOOP.md §8.2 step 7); never reassigned mid-run.
struct InputProducer {
    void* ctx;
    ProduceResult (*produce)(void* ctx, u64 tick, InputFrame* out, u8* live_mask);
};

// docs/INPUT.md §8 R-2: the per-world peer identity singleton. `slot_player_id` is an opaque
// stable identity (netcode's to interpret - docs/NETCODE.md), not itself the network slot index.
// A COMP_SINGLETON component: one instance, world_singleton<PeerSlots>(w), snapshotted/hashed/
// restored with the world (docs/FRAME-LOOP.md §7 R-1). live_mask bit i = slot i is populated this
// tick (docs/INPUT.md §4); local_slot names which slot is "me" for single-player/this peer.
#define TL_FIELDS_PeerSlots(X, XA, XH) \
    X(u8, live_mask) \
    X(u8, local_slot) \
    XA(u8, _pad0, 6) \
    XA(u64, slot_player_id, MAX_PEERS)
TL_COMPONENT_FLAGS(PeerSlots, COMP_SINGLETON)
static_assert(sizeof(PeerSlots) == 8u + 8u * MAX_PEERS, "docs/INPUT.md section 8 R-2");
