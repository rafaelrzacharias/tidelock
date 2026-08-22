# Input — the action map and the `InputFrame` airlock (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §8. Carries D13 / E2 into C++; the
> frame is now all-integer and `MAX_ACTIONS = 32` is ruled.
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
    u32 tick;                                    // the tick this frame is for (replay/lockstep alignment)
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

*Rev 1 — 2026-08-22.*
