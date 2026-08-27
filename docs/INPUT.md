# Input — the action map and the `InputFrame` airlock (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §8. Carries D13 / E2 into C++; the
> frame is now all-integer and `MAX_ACTIONS = 32` is ruled. **Implemented by w3-loop-input,
> 2026-08-26/27** (§9's files: input.h, action_map.h/.cpp, producers/{live,script,replay},
> recorder.h/.cpp) — `input_set_producer` lives on `Engine` (`core/loop.h`), not `World*` as §4
> literally spells it (the producer is never registered/hashed/snapshotted; see `core/input.h`'s
> contract block). `MAX_PEERS`'s C++ symbol is defined in `core/input.h` rather than `net/`
> (module-DAG reason: `TODO.md` RR-24) — **RULED 2026-08-26 (Rafael)**: `net/wire.h` gets a
> scoped exception, `#include "core/input.h"` for the constant, rather than carrying its own copy.
> Two non-blocking ruling requests remain open: `TODO.md` `RR-25`..`RR-26`.
> **Owns:** `src/core/input.h`, `action_map.h`, `producers/{live,script,replay}.h`
> (`network` lives in `src/net/`).

---

## 0. The load-bearing rule

**The sim reads only the `InputFrame` — never live devices.** Live polling happens only inside
the `Live` producer. Whoever produces the frame is swappable, so headless testing, replay and
lockstep are one mechanism. Input is the original sealed-sim airlock; commands and events copy
its shape.

---

## 1. The frame (DECIDED — WIRE_STRUCT)

```cpp
struct ActionState { i8 value; u8 flags; };      // 2 B. value: digital 0/1; analog −127..127 (snorm8, quantized at capture)
                                                 // flags: bit0 down · bit1 pressed-this-tick · bit2 released-this-tick
enum { MAX_ACTIONS = 32 };                       // compile-time; changing it is a wire-format version bump
struct InputFrame {                              // 76 B, explicitly laid out, static_assert sizeof + every offsetof
    ActionState actions[MAX_ACTIONS];            // 64 B, indexed by ActionId (dense u16 < 32)
    i32 pointer_x, pointer_y;                    // world-space pos_t raw bits, quantized at capture
    u32 tick;                                    // low 32 bits of the u64 world tick (FRAME-LOOP.md §1 tick width rule)
};
```

- **All-integer.** No float ever enters the sim through input. Analog axes are quantized to 8 bits
  *at capture* (below perceptual threshold for a stick); the pointer is quantized to the `pos_t`
  quantum at capture. **The wire encoding is lossless over these integers** (`NETCODE.md` §12.2):
  what a peer sends is exactly what every peer — including itself — applies.
- The pointer is **world-space**, computed by the Live producer through the render camera
  (float, non-deterministic across machines — legal, because it is *this peer's own input* and
  every peer applies the same transmitted integer). A screen-space pointer would force the sim to
  know the camera.
- `MAX_ACTIONS = 32`: ruled 2026-08-21; every netcode payload figure assumes it. A game needing
  more actions multiplexes through contexts (§3), not a bigger frame.

---

## 2. The action map (DECIDED)

Registered at init, from Luau (the game owns the vocabulary):

```lua
input.action("jump",   "DIGITAL", "EDGE")       -- kind: DIGITAL | ANALOG ;  class: LATCHED | AXIS | EDGE
input.action("move_x", "ANALOG",  "AXIS")
input.bind("jump", "key:space"); input.bind("jump", "pad:a")
input.bind("move_x", "axis:ls_x", { deadzone = "radial", sensitivity = 1.0 })
input.bind("move_x", "keys:a,d")                -- keyboard-as-axis → ±127
```

- **Action ids are dense `u16`** in registration order (`< MAX_ACTIONS`, fatal otherwise). The
  name hash is the persistence/wire identity; the index is process-stable for the run. The
  registered action list (names + kinds + classes, in order) hashes into the build fingerprint —
  two peers with different action maps cannot handshake.
- **Class** is the netcode substitution policy (`NETCODE.md` §8.4): `LATCHED` → hold, `AXIS` →
  decay to neutral over `SUB_DECAY_TICKS`, `EDGE` → null. It is declared here because it is a
  property of the action, shipped identically to every peer.
- **Bindings are data** (rebindable; v0 sets them in Luau; serialization + rebind UI reserved).
  Deadzone (axial/radial/trigger), sensitivity curves and SOCD cleaning (neutral / last-wins) are
  per-binding resolution policies applied *before* quantization.
- **Chords** (`ctrl+s`) live here (modifier-flag bindings, most-specific wins, resolved against
  one snapshot). **Combos** (`↓↘→+punch`, ordered over time) are gameplay — a Luau state machine
  over a ring of recent action edges. Boundary pinned now.
- Devices at v0: keyboard + mouse. Gamepad (SDL3 gamepad, hot-plug, mapping DB) is new bindings,
  no system changes; `ControllerImage` glyphs reserved for a gamepad-facing UI. Touch deferred.

---

## 3. Contexts (DECIDED — v0: one)

Actions belong to a context (`gameplay`, `menu`, …); the active context selects live bindings.
v0 ships one context; the **context stack + synthetic-release-on-switch** (a held action whose
binding becomes blocked is released with a `released` edge so the sim never sees a stuck hold)
is reserved behind the same API. A context switch is itself sealed: it happens at the fold, so
the frame's edges are consistent.

---

## 4. Producers (DECIDED — T-F-03 resolved: a fn-ptr, not a system)

```cpp
enum ProduceResult { PRODUCE_READY, PRODUCE_WAIT };
struct InputProducer { void* ctx; ProduceResult (*produce)(void* ctx, u64 tick, InputFrame* out /*[MAX_PEERS]*/, u8* live_mask); };
void input_set_producer(World*, InputProducer);   // init only
```

A producer gates the tick (it can say WAIT), which a system cannot; so it is a registered fn-ptr
the loop calls before `FIRST` (`FRAME-LOOP.md` §0). Four producers, one mechanism:

| Producer | Frames come from | Built |
|---|---|---|
| **Live** | `fold_tick` over the platform's raw event buffer: continuous state (down/axis) persists, edges are derived prev↔current, analog quantized, pointer projected + quantized, dev-only ImGui capture mask applied first | v0 |
| **Script** | a test's scripted frames (`press("jump", tick=10)`, `hold("move_x", 127, 0..30)`), built by the harness | v0 (headless tests) |
| **Replay** | a recorded stream (`RecordedInput` = header + tick-stamped frames + per-tick hashes) | v0 (the determinism guard) |
| **Network** | the sequencer's confirmed frames for all live slots (`NETCODE.md`) | Hovel |

Single-player is `live_mask = 0b1` with slot 0. The sim reads `world.input[slot]`; the game's
action-map script decides which slot is the local player.

**Recording** is not a producer: any producer's output can be recorded by the `LAST`-phase
recorder (frames + hashes) into `RecordedInput`; replaying it through `Replay` and comparing the
hash trace is the determinism test.

---

## 5. Dev-only: ImGui and the editor (DECIDED)

ImGui consumes the raw SDL event stream directly (render-rate, non-deterministic — it never
touches sim state). When ImGui wants the mouse/keyboard (`WantCaptureMouse/Keyboard`) the Live
producer masks those events before folding, so a UI click does not also fire `jump`. This mask is
dev-only and not part of the replay stream: an editor session is not replay-faithful by design
(editor mutations reach the sim only as commands, which *are* recorded — `ECS.md` §4).

Game UI (Luau, render-rate) gets a separate **visual-rate snapshot** of the same action state
(reserved until the UI toolkit exists, `RESERVED-SEAMS.md` §2); it never reads devices directly
either.

---

## 6. Lockstep implications (recorded here because the action map is the contract)

- The action map, classes, and `MAX_ACTIONS` are build-shipped and fingerprinted.
- The mapping frame → MoveIntent/edit commands runs in the Luau **sim VM** on every peer
  identically — the sim-script bytecode is therefore in the fingerprint (`LUAU-LAYER.md` §6).
- Commitment windows are a `MoveIntent` property (`ALLOY.md` §8.2, T-A-04), not an input
  concept; input is never buffered or altered by the netcode except by substitution.

---

## 7. Tests

Fold: edge derivation table (down/pressed/released over 3 ticks); analog quantization
round-trip bounds; deadzone shapes; SOCD; chord specificity; context switch synthetic release;
Script producer timing (exact tick); Replay producer: record → replay → identical frames and hash
trace; `InputFrame` static_asserts; fingerprint changes when the action list changes.

---

## 8. Rulings (closed 2026-08-22 — nothing open)

- **R-1 Pointer capture quantizes to the `pos_t` quantum** (RNE from the float projection). One
  quantum in the system, not two; the wire's second-order delta absorbs the sub-texel entropy
  (a few bytes per tick, NETCODE §12.2). If the archive target (NETCODE §13.4) is ever missed on
  pointer entropy, the fix is a coarser *capture* grid declared here (a constant), never a lossy
  wire — "sent == applied" stays by construction.
- **R-2 `PeerSlots` is a singleton component** (`live_mask`, `local_slot`, `slot_player_id[8]`)
  in a registered arena: snapshotted, hashed, restored with the world (`FRAME-LOOP.md` §7 R-1).
  The frame array itself is per-tick input, recorded but not hashed.

## 9. Implementation specification

### 9.1 Files

`core/input.h/.cpp` (frame types, `InputProducer`, `input_set_producer`, per-tick frame storage),
`core/action_map.h/.cpp` (actions, bindings, resolution), `core/producers/live.cpp`,
`core/producers/script.cpp`, `core/producers/replay.cpp` (over `RecordedInput`,
`DETERMINISM.md` §9.2), `core/recorder.cpp` (the `LAST` recorder system). `net/producer.cpp` is
the fourth producer.

### 9.2 Action map structures

```cpp
enum ActionKind : u8 { ACT_DIGITAL, ACT_ANALOG };  enum ActionClass : u8 { CLS_LATCHED, CLS_AXIS, CLS_EDGE };
struct Action  { NameHash name; StrId sid; ActionKind kind; ActionClass cls; u16 _pad; };
enum BindDevice : u8 { DEV_KEY, DEV_MOUSE_BUTTON, DEV_MOUSE_AXIS, DEV_KEYS_AXIS, DEV_PAD_BUTTON, DEV_PAD_AXIS };
enum Deadzone : u8 { DZ_NONE, DZ_AXIAL, DZ_RADIAL, DZ_TRIGGER };  enum Socd : u8 { SOCD_NEUTRAL, SOCD_LAST_WINS, SOCD_FIRST_WINS };
struct Binding { ActionId action; BindDevice dev; u8 modifiers /* ctrl/shift/alt bits */; u16 code_neg, code_pos /* keys-axis: two keys */; Deadzone dz; Socd socd; f32 dz_radius; f32 sensitivity; u8 context; u8 _pad[3]; };
struct ActionMap { Action actions[MAX_ACTIONS]; u32 action_count; Array<Binding> bindings; u8 active_context; };
```

The action map hash (for `session_fingerprint`): `tl_hash64` over `(name, kind, cls)` per action
in order. Bindings are **not** fingerprinted (they are local preference; they never affect what
is transmitted, only how this peer produces it).

### 9.3 Live producer — `fold_tick`

```
state: prev_down[32] (bool), axis_raw[32] (f32 accumulated this tick), key_down[], mouse_pos, pad state
for each RawEvent in the platform ring this tick (in arrival order, after the ImGui mask in dev):
    update device state (key_down/mouse/pad); text/window events ignored here
for each action a:
    resolve bindings for the active context, most-specific (most modifier bits) first; SOCD per keys-axis binding:
      digital: down = any bound input down → value = down ? 1 : 0
      analog : raw = Σ bound axis values after deadzone + sensitivity, keys-axis = (pos - neg), clamped to [-1, 1]
               value = i8(lrintf(raw * 127))         // the ONLY quantization; render-side float is legal here
    flags = (down) | (down && !prev_down[a]) << 1 | (!down && prev_down[a]) << 2 ; prev_down[a] = down
pointer: screen → world via the render camera (RENDER2D.md §2), then from_f32_quantized<pos_t>
frame.tick = u32(world.tick); frames[local_slot] = frame; live_mask = 1 << local_slot; return READY
```

Context switch (§3): processed at the top of the fold; every action whose binding set changed and
was `down` emits a synthetic `released` edge this tick.

### 9.4 Script and Replay producers

```cpp
struct ScriptedEvent { u64 tick; ActionId action; i8 value; u8 op /* SET, PRESS(1-tick), HOLD_START, HOLD_END */; u8 slot; };
struct ScriptProducer { Array<ScriptedEvent> events /* sorted by tick */; u32 cursor; ActionState cur[MAX_PEERS][MAX_ACTIONS]; };
```

`produce(tick)`: apply all events with `event.tick == tick` (in array order) to `cur`, derive edges
as in the fold, fill frames, `READY`. Tests build these with helpers `press(a, tick)`, `hold(a, v,
from, to)`. `ReplayProducer` reads `RecordedInput` frames sequentially; `READY` until
`frame_count`, then `WAIT` (the driver stops); it also exposes the recorded hashes to the harness.

### 9.5 Recorder system (`LAST`, after checkpoint)

Appends `frames[peer_count]` + `world.last_hash` (+ per-arena hashes if enabled) to the in-memory
`RecordedInput` ring (dev: 2 min ring; driver: unbounded into the output file via buffered
`file.write_all` at the end or every N ticks).

### 9.6 Tests (`tests/core/input/`)

`fold.test.cpp` (edge table over 3 ticks; analog quantization ±127 and symmetry; deadzone shapes;
SOCD modes; modifier specificity; synthetic release on context switch), `script.test.cpp` (exact
tick delivery), `replay.test.cpp` (record → replay → frames identical, hashes identical;
fingerprint mismatch refused), `frame.test.cpp` (`static_assert`s; action-map hash changes when
an action is added; bindings do not change it).

*Rev 1 — 2026-08-22.*
