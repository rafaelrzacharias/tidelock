# Deterministic lockstep netcode (tidelock, rev 1)

**Date:** 2026-08-22. **Status:** design, pre-code. Gated behind Gate 0 (fixed-point bench) and v0.
**Lineage:** supersedes `../foundry/NETCODE-DESIGN.md` rev 3 (2026-07-26). Everything above the
transport is re-homed, not redesigned; the deltas are the ones `PIVOT-DESIGN.md` §8 rules.
**Target:** 8 concurrent peers, one shared 2D level, dense map, no dedicated server, 60 Hz.
**Stack:** lean C++ engine (`PIVOT-DESIGN.md` §2) → Luau game layer → Alloy sim; netcode in
`src/net/` over vendored ENet; Monocypher for crypto.
**Session models:** `Match` (bounded, seeded) and `Persistent` (resumable, restored).

---

## 0. Provenance and scope

### 0.1 What this document is

The design for the `src/net/` subsystem that sequences `InputFrame`s across 8 peers so every
peer's simulation stays bit-identical. It owns transport, input ordering, consensus, succession,
session establishment, and the archive format.

It owns **no** simulation, no arithmetic, and no ordering *inside* a tick. Those belong to the
foundation (`FX-PALETTE.md`, `DETERMINISM.md`), the core (`ECS.md`, `FRAME-LOOP.md`), and Alloy
(`ALLOY.md`), and are specified there.

### 0.2 Where rev 3 came from (compressed)

tidelock readers never saw rev 2. The table exists so the reasoning is not re-litigated.

| Rev 2 position | Rev 3 (and this doc) | Why |
|---|---|---|
| 32 peers | **8 peers**; 16 a documented stretch, no gate | Coordinator cost ∝ (N−1)²; 32-player genres are PvP, which INV-1's maphack rules out |
| 20 Hz net tick + hold, 60 Hz sim | **One rate, 60 Hz** | Deletes hold semantics, 50 ms quantization, the unit split, rate-as-tunable |
| Scored coordinator election, Vivaldi | **Succession list = pure function of the log**; quality is lobby seating | No bandwidth scarcity at 8 |
| Shadow sequences in parallel | **Mirror only, top two** | Ordering disagreement is not evidence |
| Quorum = majority of `MAX_PEERS` | **Majority of the sequenced-live set** | 5-of-8 would end a session when three friends leave |
| Rollback scope = island | **Island closure** + global resim for coupled passes | Islands merge/split inside the window |
| Rejoin is replay | **Snapshot + tail** | Replay CPU does not stay trivial above ~7 s |
| Implicit bounded match | `SessionModel { Match, Persistent }` as boundary policy | Persistence is a product requirement |

### 0.3 Assumptions carried (replaces rev 3's "verified-as-of" and Appendix C)

- The 43 M-tick Ore soak (2026-07-26, PC + Deck + Pi 4, 0 divergences) validated a **strict-float
  stack that no longer runs**. Its *method* transfers — three real machines, checkpoint compare,
  10 h continuous — its *evidence* does not. Hovel Milestone E (§19.5) is its named successor.
- **Nothing in tidelock is built yet.** Every "shipped as" claim in rev 3 is now a "will ship in"
  (§1.1). Every budget in §7, §10.4, §16 is a **model** pending Gate 0 (G-05) and Hovel.
- Cross-ISA bit-exactness is delivered by fixed-point-by-construction (`DETERMINISM.md`), not by a
  compiler. The residual silent-desync class is UB, not FP — hence sanitizers stay in the gate.
- The three-machine reference set is unchanged: PC x86-64 Windows, Steam Deck x86-64 Linux,
  Pi 4 aarch64 Linux. G-05's severity split (PIVOT §10) decides whether the Pi stays a reference
  peer or becomes best-effort.

---

## 1. Layer ownership

The determinism contract is split across four layers. This doc's job is to **add nothing to the
first three**.

| Layer | Owns | Doc |
|---|---|---|
| **Foundation** | Bit-exact integer arithmetic by construction: the closed fx palette, det math (ported kernels), keyed stateless RNG, pinned integer hash, arenas | `FX-PALETTE.md`, `DETERMINISM.md`, `MEMORY.md` |
| **Core** | *Order*: ECS iteration by stable id, fixed phase pipeline `FIRST … LAST` with barriers, command buffers applied in chunk order, fixed timestep, per-arena hash at `LAST` | `ECS.md`, `FRAME-LOOP.md`, `JOBS.md` |
| **Alloy** | Integer quanta authoritative for everything conserved, fixed-point XPBD/PBF on the palette, ID-sorted neighbour lists, island/union-find topology, per-arena hashes | `ALLOY.md` |
| **Netcode (this doc)** | *Agreement*: which inputs, in which order, on which tick — plus session establishment and termination, and nothing else | this |

> **The netcode never reaches past a seam.** It does not order entities, does not touch arithmetic,
> does not schedule work. It hands the frame loop an `InputFrame` per peer per tick and compares
> hashes. A netcode feature that requires changing how the sim iterates or accumulates is in the
> wrong layer.

### 1.1 What this design needs, and where it will ship

Rev 3's "already shipped in Ore" table is **void**. Nothing is built. Each need maps to a tidelock
doc or a vendored library:

| Need | Will ship in |
|---|---|
| Bit-exact arithmetic across ISA/compiler/opt-level | `FX-PALETTE.md` + `DETERMINISM.md` — integer ops only on sim paths; `<math.h>` banned in sim TUs |
| Nondeterminism unreachable from sim | `DETERMINISM.md` / `BUILD.md` — sim compiles to its own static lib; CI `llvm-nm` fails on any undefined symbol outside an allowlist (no alloc, libm, clock, entropy, io, sockets) + a grep line for intrinsics/inline asm (PIVOT §2) |
| Zero per-tick allocation | `MEMORY.md` — arena-offset guard at tick start/end + debug counting shim |
| Stable iteration | `ECS.md` — sparse-set columns walked by stable id; `Map` never walked in sim; sorted map/set for ordered walks (`DETERMINISM.md`) |
| Worker-count invariance | `JOBS.md` — outputs keyed by chunk id, merges in chunk order, stealing permitted (PIVOT §12a) |
| Transport | **ENet** (vendored pure C, compiled once) behind `src/net/`; nothing above `src/net/` sees an `ENetPeer*` |
| Endian-independent byte I/O | own little-endian byte writers/readers in `src/foundation/` |
| Hashing / signing | **Monocypher**: BLAKE2b (chain, commit/reveal), Ed25519 (custody), `crypto_verify32` (constant-time compare). State hash: **rapidhash**, pinned seed + pinned implementation (`DETERMINISM.md`) |
| Checksums | own table-driven crc32 in `src/foundation/` |
| OS entropy | `PLATFORM.md` — `BCryptGenRandom` / `getrandom(2)` in a header physically unreachable from sim code; the symbol gate proves it |
| Input capture, all-integer `InputFrame` | `INPUT.md` |
| Phase hooks `FIRST` / `LAST` | `FRAME-LOOP.md` |
| Snapshot ring | `MEMORY.md` — the registered arena set, N copies allocated once (PIVOT §4) |
| Sim-script bytecode in the lockstep contract | `LUAU-LAYER.md` |
| Test runner, record→replay, per-arena hash trace | `TESTING.md` |
| Cross-ISA proof | Gate 0 G-06 (PC-vs-Pi cross-compiled bit-compare) + Hovel Milestone E |
| N-way lockstep harness | `tests/hovel/` (§19) |

---

## 2. Core invariants

Ten rules. Every subsystem below either enforces or exploits one. A change violating any of them
is a redesign, not a tuning change.

> **INV-1 — Consensus on inputs, never on state.**
> Peers agree on which `InputFrame`s apply to which tick. They never exchange or accept
> authoritative simulation state *during play*. State is derived locally.
>
> **Carve-out, the only one:** *session establishment* may transfer state (§11.3), as may *cold
> rejoin* (§10.4). Both are outside the tick loop, both are hash-verified against quorum-confirmed
> values before a single input is applied, and neither can occur while the local sim is running.

> **INV-2 — Determinism boundary.**
> Measured data (RTT, loss, wall clock, arrival order, core count, CPU headroom) may enter the sim
> only by being quantized, sequenced into the input log, and confirmed. The sim lib + symbol gate
> enforces the code half (no clock/entropy/socket symbols reachable from sim); this doc enforces
> the protocol half.

> **INV-3 — Every agreed decision derives from confirmed state.**
> Island membership, eviction, finalization, succession order, and quorum size are pure functions
> of the confirmed log and confirmed sim state. Never from local observation, never negotiation.

> **INV-4 — Detection is local, action is sequenced.**
> A peer may *suspect* anything locally. It may not act on that suspicion in a way that changes sim
> state until the suspicion is sequenced and quorum-confirmed.

> **INV-5 — The coordinator orders; it does not simulate.**
> Its only power is assigning sequence. Every peer holds all inputs and re-derives state, so a
> faulty coordinator can delay or reorder — both detectable — but cannot fabricate.

> **INV-6 — Rendering is outside the boundary.**
> Interpolation, smoothing, extrapolation, and the float camera never feed back. The one
> render→sim crossing is pointer capture, and it crosses as an integer that every peer applies
> identically (§4.2).

> **INV-7 — Worker-count invariance is a peer-facing requirement.**
> A Pi 4 and a 16-core desktop share a match. The sim must produce identical hashes at 1/2/8/16
> workers or lockstep fails regardless of netcode quality. Delivered by `JOBS.md`'s chunk-keyed
> rule (outputs keyed by chunk id, merges in chunk order, stealing permitted). The netcode's role
> is to make it a **gate** (§7.7).

> **INV-8 — Hash only authoritative arenas over their used extents.** *(rewritten for tidelock)*
> Hash `[base, used)` of every registered authoritative arena, never capacity. Every hashed
> struct is explicitly padded; arenas are zero-filled; nothing hashed holds a pointer. The
> per-arena hash is rapidhash with a pinned seed and pinned implementation that is **part of the
> lockstep contract** — changing it bumps the build fingerprint. No canonicalization pass exists
> because nothing hashed is a float.

> **INV-9 — The star is a data plane. The control plane is a mesh.**
> `INPUT` flows peer→coordinator→peer. `CONTROL` — suspicion gossip, epoch claims, measurement
> vectors, state-hash digests — flows **peer-to-peer direct, never relayed through the
> coordinator**. Relaying control through the coordinator would let a faulty coordinator suppress
> suspicion about itself, so quorum could never form and §10.2's eviction could never fire.

> **INV-10 — The session model is a boundary policy, never a tick-loop branch.**
> `SessionModel` affects how tick 0 is established and how the session terminates. It must not
> appear in any code path that runs per tick.

---

## 3. The dependency model

### 3.1 Why a compiler-enforced reach is not used

Rev 1 proposed a compiler-enforced spatial reach per write, then a broadphase over confirmed state
to derive an interaction graph. Withdrawn: there is no compiler to enforce it in C++, and the
broadphase is redundant — Alloy pass 5 (Topology) already maintains a **union-find island layer**
(carving, fracture/re-island, cavity re-flood, sleep) with island ids on the body pool. Islands
are bounded-degree in 2D by contact, deterministic by construction, maintained for the solver's
own reasons, and a pure function of confirmed state.

### 3.2 Dependency scope = Alloy island

Two entities in the same island can affect each other this tick. Two in different islands cannot,
until pass 5 merges them.

- Zero new machinery; read off state the solver already maintains.
- Deterministic and negotiation-free (INV-3).
- The solver's own unit: the XPBD solve is island-scoped, so resimulating one island is a defined
  operation.
- Sleep is free scope reduction; Alloy's `slept == stepped` invariant test proves sleep does not
  change results.

### 3.3 `v_max` is data validation

`v_max` is needed for the causal deadline (§3.4) and the telegraph rule (§3.5). Alloy's `init()`
table validator (`ALLOY.md`; the same slot as the fx range validator, PIVOT §3.1) folds every
material/projectile/motor/field entry's declared maximum propagation speed to a world `v_max`
and rejects any entry that cannot state one. `V_MAX_WORLD = 512 m/s` (PIVOT §3.1a) is the cap.

**Cost:** a *code path* that outruns its declared entry is caught only by a debug assert in the
integrator on per-tick displacement. Caught in testing, not release. **Confidence: Medium.** R2.

### 3.4 The causal deadline

Peer `A` needs peer `B`'s tick-`T` input before `A` simulates tick
`T + floor(distance(A,B) / v_max)`, and not one tick sooner.

On a dense map this buys little most of the time. Retained because it is free, degrades
gracefully when fights break up, and is the correct model if the map constraint is ever relaxed.
Do not build a cell-aggregator hierarchy (App. A).

### 3.5 Area effects and telegraphing

One explosion touching 20 bodies merges 20 islands at once. **Fix the detonation tick at throw
time**, so the merge is scheduled rather than discovered. Alloy's pass 4 buffered edits and its
composed-verb explosions already provide the mechanism.

**Design rule (belongs in the combat design doc too):** any effect that can merge more than
`AOE_ISLAND_LIMIT` islands (4) must telegraph for at least the confirmation horizon.

### 3.6 Hidden information — CLOSED

Every peer holds full world state (INV-1); maphack is unfixable without breaking determinism.
**Ruled 2026-08-21 (PIVOT §8): full-world visibility.** The games are 8-player co-op where
everyone sees the whole world. Reopening is a **redesign trigger**, not a feature request. R1 is
closed.

### 3.7 Rollback scope is the island **closure**, not one island

Islands merge and split — that is what pass 5 does. Over a depth-`d` rollback, the set of entities
in island `I` at tick `T` is not the set at `T+d`. The restore scope is the **transitive closure of
every island that merged with or split from `I` during the window**:

```
  closure(I, T, d) = fixpoint over ticks T..T+d of
                     { islands sharing any entity with the set so far }
```

- **Quiet play:** closure ≈ one island. Cheap.
- **Dense fight:** closure approaches the whole world; cost returns to the unaffordable full-world
  figure (§7.2).
- Therefore **Gate 4 measures closure size under load**, not merely that a single-island restore is
  a memcpy. Two halves of R3.

**Globally-coupled passes are exempt and resim globally.** Alloy pass 1 (fields) is a global
lattice with no island structure.

> **Rule:** island-scoped passes resim over the closure; globally-coupled passes resim globally at
> full depth. At the model's ~0.5 ms for pass 1, a depth-6 global field resim is ~3.0 ms — counted
> in §16.2.

---

## 4. The sequencing seam — `InputFrame`

### 4.1 The fork

**Option A — sequence `InputFrame`.** Peers exchange per-player action state; each peer maps
frames to intents + edit commands locally and steps the sim.
**Option B — sequence the sim's `step(inputs)` argument** (intents + edit commands).
**Option C — sequence state.** Coordinator simulates and broadcasts. *(Rejected; App. A.)*

### 4.2 Trade-off and pick — A stands (DECIDED)

| Axis | **A — InputFrame** | B — intents + edits | C — state |
|---|---|---|---|
| Determinism | Strong. POD with a `tick` field; producer swappable so Live/Script/Replay/Network are one mechanism | Strong, but game→intent mapping runs *before* the wire | Discards it |
| Performance | Smallest payload, uniform shape, compresses well | Larger, variable — brush geometry, spawn payloads | 10–50× bandwidth; scales with entities |
| LOC / cognitive | Lowest: hooks two existing barriers | A serialization format per command variant, versioned | Delta baselines, quantization, per-peer acks |
| Stability | `InputFrame` is a frozen `WIRE_STRUCT` | Command set churns with gameplay | Snapshot format churns with every sim change |
| Test surface | One struct, one encoder; record→replay→compare | Every variant round-trips + malformed input | Widest |
| Iteration | Game code changes freely without touching the wire | Every new verb touches the wire | n/a |

**One consequence that is new in tidelock.** The game→intent mapping now runs in the **Luau sim
VM** on every peer identically (`LUAU-LAYER.md`). That puts the sim-script **bytecode** inside the
lockstep contract — which is why it is part of the build fingerprint (§15.1). A keeps the wire
narrow; it does not keep game code out of the contract, and this doc does not pretend otherwise.

**`InputFrame` is all-integer** (DECIDED in `INPUT.md`):

```cpp
// WIRE_STRUCT — explicitly padded, static_assert on sizeof and every offsetof (§12.1)
struct ActionState {          // 2 B
    i8 value;                 // digital 0/1; analog -127..127, quantized AT CAPTURE
    u8 flags;                 // bit0 down, bit1 pressed, bit2 released; bits 3..7 zero
};
struct InputFrame {           // 76 B
    ActionState actions[MAX_ACTIONS];   // 64 B, MAX_ACTIONS = 32
    i32 pointer_x;            // world-space pos_t raw bits (fx<i32,18>)
    i32 pointer_y;
    u32 tick;
};
static_assert(sizeof(ActionState) == 2);
static_assert(sizeof(InputFrame) == 76);
static_assert(offsetof(InputFrame, pointer_x) == 64);
static_assert(offsetof(InputFrame, tick) == 72);
```

The pointer is quantized at capture from the render-side float camera. This is **legal**: each
peer transmits whatever integer it captured; determinism only requires every peer to apply the
same transmitted value. The render→sim crossing happens once, at capture, as an integer (INV-6).
Analog 8-bit quantization is likewise done at capture, not by the encoder.

### 4.3 Where it hooks

```
  FIRST        input drain + action map, NET RECEIVE   <- netcode: deliver confirmed frames
  PRE_UPDATE
  UPDATE
  POST_UPDATE
  LAST         per-arena hash, NET SEND                <- netcode: hash exchange + send
  --- command-buffer apply / events swap barrier ---
```

The netcode registers two systems (`FRAME-LOOP.md`), one at `FIRST` and one at `LAST`, and touches
nothing else. It replaces the `InputFrame` *producer*; it does not modify the loop.

### 4.4 One tick rate — 60 Hz (DECIDED)

Network tick = sim tick. Unifying the rates deletes hold semantics, the 50 ms input quantization,
the net-tick/sim-tick unit split, and rate-as-wire-tunable. **Every tick count in this document is
a sim tick at 60 Hz.** Tunables carry it in the name (`*_TICKS`); the type should carry it too.
Substitution for a missing frame is per-action-class (§8.4), not "hold".

---

## 5. Transport — ENet (rewritten for tidelock)

### 5.1 Payload reality

```
  ActionState   = { i8 value; u8 flags }                        ->  2 B
  InputFrame    = ActionState[32] + i32 pointer_x/y + u32 tick  -> 76 B RAW (was 268 B)

  60 Hz x 8 peers, raw, one copy each at the coordinator          ~36.5 KB/s inbound
  coordinator downstream raw, 7 slots x 76 B x 60 Hz x 8 peers    ~255 KB/s  ≈ 2.0 Mbit/s
  same with the 9-tick redundancy window                          ~2.3 MB/s  ≈ 18 Mbit/s
  one downstream packet, 7 slots x 9 ticks x 76 B                 ~4.8 KB    ≈ 3.3x MTU
  30-min 8-peer archive, raw                                      ~66 MB
```

The drop from 268 B to 76 B is real but **does not make raw viable**. Stated honestly: raw fits an
MTU at one tick per packet with zero loss tolerance; at the redundancy window it fragments every
packet; and the archive is ~66 MB per half hour against a ~165 KB target. **The §12 encoder
remains mandatory infrastructure on the MTU-headroom and archive-size arguments**, not on the
bandwidth argument alone. Post-encoding planning figure: **6 B per peer per tick** (§12.3).

### 5.2 Protocol — ENet, unreliable for inputs, no retransmission

**ENet is the one socket.** Vendored pure C, compiled once in its own TU, wrapped entirely inside
`src/net/`. Nothing above `src/net/` sees an `ENetPeer*`, `ENetHost*`, or an ENet header; the
seam exposes slot ids, channel ids, and byte spans.

**Inputs go on an unreliable-sequenced channel, never a reliable one.** ENet's reliable channel
retransmits and holds later packets behind a lost one — retransmit + per-channel head-of-line
stall is the exact failure this architecture exists to avoid. A reliable `INPUT` channel is
rejected for the same reason TCP was (App. A).

**No retransmission on `INPUT`.** Every packet carries the last `REDUNDANCY_TICKS` ticks of that
peer's input. A dropped packet is repaired by the next one at zero round-trip. Out-of-order
delivery is harmless: a frame for a tick already held is discarded.

**Redundancy is a time window.** `REDUNDANCY_TICKS = 9` gives 150 ms at 60 Hz for ~54 B of payload
per peer — comparable to the IP+UDP+ENet header already paid.

> **Rule: `REDUNDANCY_TICKS ≥ CONFIRMATION_HORIZON_TICKS`.** Any successor reconstructs the entire
> unconfirmed window from the *first* packet each peer sends it — no retransmission request, no
> round trip (§9.5). `static_assert` it.

**ENet reliable + fragmentation is used off the game path**, for `BULK` (§5.3). That replaces
rev 3's TCP side channel: ordered delivery and fragmentation for snapshot and rejoin streaming,
one socket, and head-of-line blocking there is harmless.

### 5.3 Channels

| Channel | ENet mode | Topology | Budget | Contents |
|---|---|---|---|---|
| `INPUT` | **unreliable-sequenced** | **Star** (§6.1) | Reserved, never yielded | Input frames (redundancy window), ack bitmaps, epoch, `last_confirmed_tick` |
| `CONTROL` | **unreliable** | **Mesh** (§6.2, INV-9) | Small, bursty | Suspicion gossip, epoch claims, quantized measurement vectors, per-arena hash digests |
| `BULK` | **reliable + fragmented** | Point-to-point | Whatever is left | Arena snapshots, rejoin log deltas, session-start distribution |

`BULK` **can never starve `INPUT`.** Token-bucket pacer per channel above ENet, `BULK` explicitly
subordinate; ENet's own throttle is not trusted to express that priority. A peer streaming an
arena set to a joiner must not degrade the session.

`CONTROL` is unreliable because every message on it is either idempotent gossip (re-sent on the
next interval) or acked at the application level (§5.4). A reliable `CONTROL` would reintroduce
head-of-line stalls on the path that must stay live when the coordinator misbehaves.

### 5.4 Delivery semantics — the ack rule

A send return means **buffered**, never **delivered**, on every transport including ENet's
reliable channel (ENet acks to the peer's host, not to the application that consumes it). A
process exiting immediately after its last send can lose the queue.

**Therefore every protocol phase that must know its data arrived uses an application-level
ack.** The pattern is a short epilogue `[final_tick][final_ref_hash][all_agree]`. Used for:
session end, snapshot handoff completion, custody handoff (§11.6), succession confirmation.

### 5.5 NAT traversal — OPEN

The project's trade-off axes are *single-process, deterministic, no services; not
latency/SPOF/CAP*. STUN + hole punching, a TURN relay, and session auth are a **service
dependency** — a new axis, not an implementation detail.

1. **Accept a signalling service.** Standard, works, adds an operated dependency.
2. **LAN / direct-IP only for v1.** No service. LAN parties and port forwarding.
3. **Platform-provided sessions** (Steam Datagram Relay or equivalent). Trades the service for a
   platform SDK — must live strictly outside the sim boundary and behind `PLATFORM.md`.

**No pick.** Product decision. Option 2 is a **legitimate v1 answer** for 8-player co-op — one
port-forwarded host is the normal configuration in this genre (Valheim, Terraria, Factorio) — not
a placeholder. ENet is NAT-agnostic: all three options sit below it.

---

## 6. Topology

### 6.1 Data plane — single rotating coordinator, star

7 upstream + 7 downstream links; two hops peer→coordinator→peer.

```
        peer --+                     +-- peer
        peer --+--> COORDINATOR -->--+-- peer      inputs up, ordered log down
        peer --+         |           +-- peer
                         v
                   SHADOW x2   (mirrored inputs, no sequencing, no authority)
```

The coordinator did three jobs; two evaporated at 8 peers (absorbing asymmetric bandwidth;
being well-chosen so the hop does not hurt). The third is the whole reason the role exists:

> **A fixed sequencing point so the quorum fold is deterministic.** The fold rule in §8.2 is "slot
> present iff ≥ Q peers hold it". In a leaderless mesh, peer A folds over bitmaps from
> {1,2,3,4,5} while B folds over {1,2,3,4,6} and reaches a different result. The coordinator fixes
> **which bitmaps count**.

Three leaderless escapes all fail: fold over all N (stalls on the slowest); propose substitutions
as inputs (circular — evaluating proposals for `T+d` needs the log at `T+d`); local timeout, first
substitution wins (an implicitly elected leader, §8.1's failure). The escape that *works* is
classic stall-lockstep (StarCraft/AoE/Factorio) — **rejected on product grounds**: "waiting for
players…" is unacceptable, and per-peer adaptive delay (§7.4) is a strictly better degradation
story.

**Rejected: full mesh for `INPUT`** — on the fold problem, not connectivity (28 links with ~0.84
expected NAT failures is relay-coverable).

### 6.2 Control plane — mesh (INV-9)

1. **A coordinator must not be able to suppress suspicion about itself.** The only remedy for a
   withholding coordinator is sequenced eviction (§10.2), which needs quorum.
2. **The links already have to exist.** Every potential successor must be reachable by every peer
   or failover strands someone.

Cost: `CONTROL` < 2 KB/s aggregate. Stated as an invariant because it is exactly what an
implementer "optimizes" by folding onto the coordinator link.

**Deadlines are island-derived (deterministic). Routes are measured (nondeterministic).** INV-2 is
absolute: a coordinator dying or a route flapping triggers zero rollback, because nothing in the
sim ever read it.

---

## 7. Sim integration

### 7.1 The budget — a model until G-05 reports

```
  Alloy tick, 60 Hz, ~8 cores, nominal load (~20k particles, ~2k awake bodies, ~500 dirty regions)

    pass 1 fields      ~0.5 ms   <- globally coupled, no island structure (§3.7)
    pass 2 forces      ~0.5 ms
    pass 3 solve       ~4.0 ms   <- substep loop (8 substeps), dominant by design
    pass 4 chemistry   ~1.0 ms
    pass 5 topology    ~1.0 ms   (amortized, dirty-driven)
    hashing/bookkeep   ~0.5 ms
    ---------------------------
    total              ~8.0 ms   against a 16.7 ms frame
```

**These are estimates carried from `ALLOY.md`'s float-era budget.** Gate 0 G-05 re-derives them
for fixed point: pass at 20k particles ≤ 4 ms PC *and* ≤ 12 ms Pi; > 8 ms PC at 20k is
pivot-level. If 20k does not fit, the budget moves (counts, substeps), not the verdict. The budget
is per peer and independent of peer count.

### 7.2 Therefore: rollback is not cheap (model)

```
  full-world resim, depth 6  = 6 x 8 ms  = 48 ms   -> 2.9x OVER one frame
  headroom for rollback      = ~8.7 ms/tick        -> depth 1 full-world, useless
```

**(a) Closure-scoped rollback is mandatory.** ~50 awake islands → ~0.16 ms per resimulated tick
for a one-island closure, ~1 ms at depth 6; plus the mandatory global field resim (§3.7) at
`6 × 0.5 = 3.0 ms` → **~4 ms** realistic. Viability rests on the awake-island set staying small
*and* the closure not exploding (§3.7).
**(b) The confirmation horizon must be short.** It is speculation depth, failover cost (§9.5), and
irreversible-display delay (§7.6) at once.
**(c) Adaptive delay (§7.4) and commitment windows (§7.5) carry most of the load**, not rollback.

No cheap partial tick: pass 3 is a substep loop, so a resimulated tick re-runs all substeps.
Numbers above scale with whatever G-05 measures; the *ranking* does not.

### 7.3 Snapshots — the registered arena set

`MEMORY.md` (PIVOT §4) decides this: all authoritative state in **registered flat POD arenas, no
interior pointers, handles/indices only**; each arena registers `(id, base, used)`; the registry
is simultaneously the snapshot unit (memcpy per arena, fingerprint-stamped), the per-tick hash
unit, and the rollback ring.

The netcode's addition is a **ring of N arena-set snapshots**, `N` = confirmation horizon,
allocated once from the permanent arena (ticks allocate nothing). Rollback = pick a snapshot,
restore the arenas of the affected **closure**, resim forward.

**T-A-01 — still the gate (R3).** Alloy's pools are global SoA, not per-island; the snapshot API
is whole-arena `save/restore`. **Confidence: Low** that scoped restore is free.

| Approach | Cost | Why it may fail |
|---|---|---|
| Whole-arena restore | ~48 ms at depth 6 | Unaffordable (§7.2) |
| Per-island arena partitioning | memcpy again | Merge/split relocates memory between arenas — pass 5 does exactly that, every tick |
| **Handle-indexed scatter restore** | ∝ island size | Needs island→handle-set index; **most likely to work** — pass 5 already maintains membership, pools are sorted by stable id |

T-A-01 must measure **both** halves: restore cost (a) *and* closure size vs awake-set size and
merge rate (b). §19.6 S-14 measures (b) on Hovel's region analogue before Alloy exists.

### 7.4 Per-peer adaptive delay — the primary mechanism

> A peer with a bad link is given **more local input delay**, so their input still arrives by its
> deadline.

The laggy peer eats their own latency; everyone else's rollback depth is unaffected (removes the
max-over-N order statistic); it inverts the lag-switch incentive. Adaptation is hysteretic (raise
fast on sustained miss, lower slowly); the delay value is **sequenced into the log** (INV-2).

### 7.5 Commitment windows — the second mechanism

An action declares `k` ticks during which the actor's future inputs are buffered but **cannot
alter sim state**. Rollback frequency for `N−1` remotes each mispredicting with probability `p`:

```
  P(rollback this tick) = 1 - (1-p)^(N-1)
  p        N=8
  0.05     30%
  0.02     13%
  0.005    3.4%
```

Inside a window `p = 0` exactly. Two predictors: **predict actions, not buttons** (continue the
action state machine to completion — a pure function of confirmed history); a **deterministic
Markov predictor** over input transitions, run identically on every peer from the confirmed log,
contexted on `(previous input, action state, hold-duration bucket)`. Hold-duration buckets are
derived for 60 Hz. Integer-only, inside the boundary (INV-3).

### 7.6 Split by rollback tolerance

| Class | Examples | Treatment |
|---|---|---|
| **Reversible** | Position, velocity, facing, animation phase | Roll back freely; render chases sim through a critically-damped spring (render-only, float is legal there) |
| **Perceptually irreversible** | Hit registration, death, pickups, objective capture, resource commit, construction completion | **Do not display until finalized.** Resolve on the confirmed log, display `k` ticks late |

Delayed confirmation reads as latency; an event that un-happens destroys trust. For a colony sim
the irreversible list is longer and *cheaper* — a building appearing 100 ms late is imperceptible.
The carrier is the event stream: notifications, never authoritative state, **never hashed**,
drained after the tick, per-chunk rings merged in chunk order.

### 7.7 Parallelism and worker-count invariance

INV-7's obligations:

- **Gate it.** `JOBS.md`'s invariance harness — identical hash trace at 1/2/8/16 workers — is a
  **blocking release gate for the netcode**, plus one mixed-pair run (peer A at 4 workers, peer B
  at 16, same inputs) once transport exists.
- **Do not sequence core count.** Worker count never enters the log.
- **The chunk-keyed rule is what delivers it** (PIVOT §12a): `parallel_for` splits by a pure
  function of `(N, grain)`, outputs land in chunk-indexed slots, merges fold in chunk order,
  command buffers are tagged by chunk id. Worker identity is invisible to results, so stealing is
  free. Alloy's coloured Gauss-Seidel maps to sequential levels with stable-id-ordered chunks.
- There is no FP-in-workers question: integer ops have no per-thread control register.

---

## 8. Consensus on the input log

### 8.1 The problem

When a peer's frame misses its deadline, a peer **cannot decide locally** to substitute — it may
have arrived at half the peers. Half substitute, half do not: a netcode-manufactured desync.

### 8.2 The mechanism

Peers piggyback an **8-bit "frames I hold for tick T" bitmap** on every packet. Finalization fires
on a deterministic quorum fold over bitmaps everyone has.

```
  finalize(T):
     require: bitmaps for T from >= QUORUM(live) peers, all sequenced into the log
     present = deterministic_fold(bitmaps)     // slot present iff >= Q peers hold it
     for slot in ~present: substitute(slot, T) // per-action-class policy, §8.4
     mark T confirmed
```

Total-order broadcast over a small log — well-trodden ground. **Slots are stable within a
session**; the bitmap is slot-indexed. Under `Persistent`, slots re-seat *between* sessions.

### 8.3 Quorum over the sequenced-live set

> **`QUORUM` is a strict majority of the *sequenced-live* slot set**, not of `MAX_PEERS`.

Evicted and departed peers leave the denominator via the same sequenced event that removes them,
so the value is a pure function of the confirmed log (INV-3).

```
  live:  8  7  6  5  4  3  2  1
  Q:     5  4  4  3  3  2  2  1
```

**Sequential loss is safe all the way down.** **Simultaneous loss is not:** losing 4 of 8 at once
leaves 4 peers needing `QUORUM(8) = 5` to sequence the evictions that would shrink the denominator.
Deadlock, self-perpetuating — one shared house's router rebooting. No consensus escape exists
without split-brain risk (§10.8). **A co-op game has an escape a database does not: it can stop.**

> **Rule — quorum-loss termination.** If quorum cannot form for `QUORUM_LOSS_TICKS`, every
> remaining peer independently writes the last confirmed checkpoint and ends the session. Every
> peer holds the same confirmed log, so every checkpoint is **bit-identical**. Under `Persistent`
> it is a save point; under `Match` a no-contest.

R10: eviction and partition look identical for a few ticks; the interaction with §10.8 needs a
correctness argument.

### 8.4 Substitution is per-action-class

| Class | Substitution | Rationale |
|---|---|---|
| `LATCHED` (fire, sprint, held tool) | **Hold** | Releasing on a dropped packet reads as a dropped input |
| `AXIS` (movement, aim) | **Decay to neutral** over `SUB_DECAY_TICKS` | Hold walks the pawn off the cliff; null snaps |
| `EDGE` (jump, place, confirm) | **Null** | An un-acked one-shot must never fire twice or spuriously |

The class lives in the action map (`INPUT.md`), build-shipped and identical on every peer;
substitution is a pure function of `(class, last confirmed value, ticks since)`. Decay is integer
arithmetic on the `i8` value.

### 8.5 The sequencer

A **coordinator** assigns ordering and drives finalization. No authority over state, no
simulation, swappable in one tick (§9).

> An arbiter of **ordering** is replaceable and cannot lie undetectably. An arbiter of **state**
> is neither.

Full Raft per tick is overkill; sequencer + epochs + log-completeness (§9.5) is enough.

### 8.6 Epochs

Every coordinator claim carries a monotonically increasing epoch; peers reject stale epochs.
Split-brain becomes **impossible rather than unlikely**; a stalled-not-dead coordinator returns,
finds its epoch superseded, and rejoins as an ordinary peer.

### 8.7 Lookahead cheating — unblocked

Hash-commit at `T`, reveal at `T+k`: `commit = BLAKE2b(input ‖ nonce)`, compared with
`crypto_verify32`. A reveal that does not match its commit is discarded and substitution applies.
Costs `k` ticks of latency for everyone. **Ranked/competitive only; off in co-op.**

The nonce comes from OS entropy behind `PLATFORM.md`'s seam — physically unreachable from sim code
(the symbol gate proves it). R12 is resolved; nothing here is blocked.

---

## 9. Succession and failover

### 9.1 The tension

Failure detection is measured, therefore nondeterministic. Succession must be agreed, therefore
deterministic. Resolution: **remove measurement from the succession decision entirely.**

### 9.2 Link quality is a lobby-time seating decision

Before the session, peers measure the full RTT matrix directly — 28 ordered pairs at `LOBBY_PROBE_HZ
= 1` is < 200 B/s per peer. **Every slot is seated by measured capability**: rank on p95 RTT to
all others plus measured upstream headroom; best becomes slot 0, and so on. Outside the session,
where nondeterminism is free. This is the entire mitigation for R13, and it costs one sort.

**In-match opportunistic migration is deleted.** Failover-on-death is the only migration.

### 9.3 The succession list

> **Succession order is a pure function of the confirmed log** (INV-3): initialized to slot order;
> a peer is removed when it leaves or is evicted; a rejoining peer is **appended at the end**.

Every peer computes the same successor with zero protocol. **Appended, not reinserted:** naive
lowest-live-slot would make slot 0 coordinator again the instant it rejoins, so a flaky link flaps
the role on every blip. Appending makes flapping self-correcting with no measured quantity.

### 9.4 Shadows — mirror only, top two

Parallel sequencing is cut: sequencing is a *choice* among valid orderings, so disagreement is not
evidence. What *is* evidence is "input existed and was excluded", and that needs only the mirror.

**Top two**, not one: positions 0 and 1 failing together (one router, two players) would leave
position 2 having mirrored nothing.

```
  peer upstream, 60 Hz, REDUNDANCY_TICKS = 9:
    to coordinator only          ~5.6 KB/s   0.045 Mbit/s
    + 2 shadows (chosen)        ~16.9 KB/s   0.135 Mbit/s
  shadow extra downstream (7 peers mirroring to it)   ~39 KB/s
```

Always-on before any failover logic: a warm path is a tested path.

### 9.5 The unsequenced gap, and the log-completeness constraint

Frames in flight to a dead coordinator reached some peers and not others. The successor
re-sequences from the last quorum-confirmed tick; every peer rolls back to it. Cost: **one rollback
of depth equal to the confirmation horizon**.

> **The confirmation horizon is simultaneously speculation depth, failover cost, and
> irreversible-event display delay.** One tunable, three consequences documented next to it.

Epochs do *not* prevent a **behind** successor claiming `e+1` and re-sequencing over ticks others
had confirmed.

> **Constraint (Raft's log-completeness, narrowed):** a peer may claim epoch `e+1` only if a quorum
> of the live set confirms the claimant's `last_confirmed_tick` is **≥ their own**. The claim carries
> `last_confirmed_tick`; a peer refuses to acknowledge a claimant behind it.

With `REDUNDANCY_TICKS ≥ CONFIRMATION_HORIZON_TICKS` the successor holds the whole unconfirmed
window from the first packet each peer sends. R5 needs a test, not an argument: §19.6 S-08.

### 9.6 When quorum cannot form

The head of the succession list always qualifies. The surviving case is quorum loss — §8.3's
termination rule.

---

## 10. Disconnection, phantoms, leave, rejoin

### 10.1 Organizing principle

> **Detection is measured. Eviction is sequenced.** (INV-4)

Suspicion gossips on the `CONTROL` mesh; **eviction enters the log as an ordinary sequenced event
with a fixed effective tick**. Disconnection becomes a game event — the only form the sim can
safely consume.

### 10.2 Four states

```
   LIVE --quorum suspicion--> SUSPECT --fixed tick count--> EVICTED
     |  ^                         |
     |  +------packets resume-----+
     |
     +--sequenced leave--> DEPARTED        (§10.3, no phantom, no timeout)
```

**During SUSPECT** the avatar is a **phantom** driven by the §7.5 Markov predictor — identical on
every peer, zero desync risk. Two non-negotiable restrictions: **safe action subset only**
(movement, defensive states — never capture, score, trade, spend, commit construction); **fully
vulnerable** (the anti-rage-quit property). Alloy's `AgentBody` takes an externally supplied
`MoveIntent`; a phantom is a different intent producer.

**SUSPECT → EVICTED at a fixed tick count, never wall clock** (INV-2). `SUSPECT_TO_EVICT_TICKS =
1800` (30 s). Every peer drops the avatar on the same tick; `QUORUM` recomputes at a known tick.
**The sim never stalls** — substitution is a fold over shared bitmaps, not a local timeout.

### 10.3 Graceful leave

A player going to bed is the **common** case; 30 s as a vulnerable phantom is wrong for it.

> A `leave` is an ordinary sequenced event with a fixed effective tick. On that tick the peer goes
> LIVE → DEPARTED directly: **no SUSPECT, no phantom, no timeout**; `QUORUM` recomputes at once.

The departing peer knows its effective tick ("leaving in 3…2…1"); under `Persistent` a leave is a
checkpoint trigger (§11.4); a departing coordinator sequences its own leave so failover is planned,
with **no unsequenced gap at all**.

### 10.4 Rejoin — snapshot first, replay the tail

**Replay runs at ~2.08× realtime** in the model (8 ms CPU per 16.7 ms simulated; G-05 will move
this). Therefore:

| Missed simulated time | Log delta (archive-encoded) | Replay CPU (model) |
|---|---|---|
| 30 s | ~1 KB | 14 s |
| 5 min | ~2 KB | 2.4 min |
| 1 h | ~35 KB | 29 min |
| 6 h (a solo session) | ~200 KB | **2.9 h** |

The log stays trivial; replay CPU does not. **Crossover:** replaying `T` s costs `0.48·T`;
transferring a 4 MB arena set at 10 Mbit/s costs 3.2 s. They cross at **T ≈ 7 s**.

> **Rule:** rejoin = **nearest checkpoint + log tail**, always. Replay-only is a special case for
> gaps under ~7 s. Cold and warm rejoin are the same path.

A mid-session join closes its gap at only ~1.08× net (rejoiner replays at 2.08× while the world
advances at 1×) — without the snapshot path a late joiner may never catch up.

- **Self-verifying.** Post-replay per-arena hashes **must** match the live quorum. A mismatch is a
  determinism bug with an exact reproduction attached. P0 telemetry.
- **Any peer can serve it.**
- **`BULK` only**, so catch-up never starves live inputs.

### 10.5 Identity and slots

- Slots stable within a session; re-seated between sessions under `Persistent` (§11.3).
- **The rejoiner asserts nothing about state.** Everything comes from checkpoint + log; its only
  contribution is future inputs.
- A rejoining ex-coordinator returns as an ordinary peer via epoch supersession, appended to the
  succession list.
- **Authenticate the session.** Under `Match` polish; under `Persistent` core — slot reclamation
  across sessions is "prove you are the player who owns this colony". Ed25519 identity keys,
  generated from OS entropy behind the platform seam. Unblocked (R12 resolved).

### 10.6 Phantom input storage

Phantom inputs are deterministic and *could* be a range marker. **Store them literally.** A range
marker couples the archive to the predictor version (§15.2); a disconnected player generates
almost no transitions, so under §13.3 literal storage costs a few hundred bytes per dropout.

### 10.7 Abuse surface

| Attack | Status | Mechanism |
|---|---|---|
| Rage-quit denial | Solved | Phantom vulnerability + grace window (§10.2) |
| Lag switching | Incentive inverted | Per-peer adaptive delay (§7.4) |
| Rejoin-as-reset | Impossible by construction | State from checkpoint + log (§10.5) |
| Drop-flapping to force succession | Absorbed | Rejoin appends to the end (§9.3) |
| Lookahead cheating | Optional | Commit/reveal (§8.7), competitive only |
| Coordinator withholding/reordering | Detectable **and actionable** | Local observation of sequencing latency; suspicion on the `CONTROL` **mesh** (INV-9); remedy is sequenced eviction |
| Save forking / rollback abuse | Detectable | Hash-chained world identity (§11.5) |
| **Maphack** | **Unsolvable** | Inherent to INV-1 — and accepted by the R1 ruling (§3.6) |

### 10.8 Network partition

A clean 4/4 split: both halves have plausible quorum, both continue, **no merge exists for
divergent deterministic histories**.

**Rule: strict majority of the sequenced-live set (§8.3), so at most one side survives.** The
minority stalls and terminates via §8.3 — under `Persistent`, writes a checkpoint and stops
cleanly. At an exact 4/4 neither side has 5, so **both** stop and both write identical checkpoints
of the last confirmed tick. No fork is created. It looks like a failure and is the correct
behaviour.

---

## 11. Session model

### 11.1 The enum

```cpp
enum SessionModel : u32 { Match = 0, Persistent = 1 };
enum Origin       : u32 { Seeded = 0, Restored = 1 };
```

`Match` — bounded; world discarded. `Persistent` — resumable; the world outlives any session.
**Per INV-10 this is boundary policy.** It appears in **no** per-tick path.

### 11.2 The real distinction: how tick 0 is established

| Origin | Meaning | Wire cost |
|---|---|---|
| **Seeded** | Initial state derives from a seed + build-shipped content; every peer computes identical bytes locally | Zero |
| **Restored** | Initial state is a snapshot from a previous session's runtime; cannot be re-derived | Arena set transfer (§11.3) |

Both converge on one gate: the handshake carries `tick0_state_hash`. `Seeded` computes it;
`Restored` transfers and verifies against it. Legal combinations validated at `init()`, fail-loud:

| | Seeded | Restored |
|---|---|---|
| `Match` | normal | rare — replay-continuation or dev tool |
| `Persistent` | **first session only** — a new colony | normal |

### 11.3 Session establishment

**`Restored` session start *is* §10.4's cold rejoin, run simultaneously for everyone.** Nearest
checkpoint + log tail over `BULK`, self-verifying. No new subsystem.

**Slot re-seating** happens here and only here: durable player identity (Ed25519 public key) →
slot, immutable for the session.

**Distribution cost** (arena-set size unknown, T-A-03), owner uploading to 7 peers at 10 Mbit/s:

| Arena set | Star (owner → all 7) | Swarm (log₂ fanout, 3 rounds) |
|---|---|---|
| 1 MB | 5.6 s | ~2.4 s |
| 4 MB | 22 s | ~10 s |
| 16 MB | 90 s | ~38 s |

**Build the star upload. Swarming has a reopening condition at > 4 MB.** Join cost is proportional
to how much you missed: a returning player holding last session's checkpoint needs only the delta.

### 11.4 Two-tier checkpoints

With a 4 MB arena set a 5 s cadence writes 800 KB/s sustained — acceptable only because it
overwrites.

| Tier | Cadence | Retention | Durability | Purpose |
|---|---|---|---|---|
| **Hot ring** | `CHECKPOINT_HOT_TICKS` = 300 (5 s) | Ring of 2 | In-memory + best-effort disk | Bounds rejoin replay to ≤ 5 s |
| **Durable** | `CHECKPOINT_DURABLE_TICKS` = 18000 (5 min), plus on leave, quorum-loss, clean end | Kept | write-temp → `fsync` → rename, double-buffered | Bounds crash loss; anchors identity (§11.5) |

`Match` collapses both into the hot ring. A durable checkpoint **bounds the log**: older segments
may be discarded. **Torn-write asymmetry:** a corrupt checkpoint costs a replay under `Match` and
the colony under `Persistent`; the write-temp/fsync/rename discipline is not optional there.

### 11.5 World identity — the hash chain

```
  chain[0] = BLAKE2b(tick0_state_hash, world_seed, creation_nonce)
  chain[K] = BLAKE2b(chain[K-1], log_segment_hash[K-1 -> K], state_hash[K],
                     build_fingerprint[K], custody[K])
```

32 B per durable checkpoint, computed from hashes already produced. Gives: **identity without log
retention**; **fork detection** (two saves share history iff their chains share a prefix); **fork
*point* identification** ("you split three days ago, at 14 h of colony time").

**Solo play needs no special handling.** `live = 1`, `QUORUM(1) = 1`, coordinator is the single
peer, checkpoints and chain advance identically. Same path, no branch.

### 11.6 Custody

| Model | Solo play | Fork risk | Cost |
|---|---|---|---|
| Fixed owner | Owner only | Near zero | Nobody plays without the owner |
| **Custody baton** | Whoever holds it | Low, detectable | Explicit handoff |
| Free forking | Anyone | Guaranteed | Fork management becomes core UX |

**Chosen: custody baton** — "whoever is currently advancing the world", one party at a time.
Custody is in the durable checkpoint header and enters the chain.

- **Handoff is explicit and acked** (§5.4), sequenced like any event.
- **Forced takeover is permitted** when the holder is unreachable — presented honestly as
  *creating a fork*. The chain makes it visible either way.
- **Handoffs are Ed25519-signed** (Monocypher). Keys from OS entropy behind the seam. Unblocked.

Which save is canonical after a fork is a **product rule**. The netcode's obligation is to make the
fork unambiguous and locatable.

### 11.7 The structural limit

> **Free-play-anytime and no-dedicated-server are mutually exclusive.** No netcode fixes this.

Someone holding a current copy must be online to advance the world. The custody baton is the best
answer *within* the constraint. Recorded so it is not later "fixed" by reintroducing a server.

---

## 12. Wire encoding

### 12.1 Layout constraint — the `WIRE_STRUCT` discipline (DECIDED, PIVOT §8)

Replaces `@preserve_layout` + `@layout_hash`. Every struct that crosses the wire, hits a
checkpoint, or enters the chain is a **`WIRE_STRUCT`**:

- a concrete, **non-template** struct — **no template instantiation crosses the wire** (`static_assert`
  it; an `Array<T>` or `Handle<…>` is unpacked into plain fields at the boundary);
- **explicitly padded** — every padding byte is a named `u8 padN` field, zeroed on write, asserted
  zero on read;
- `static_assert(sizeof(T) == K)` **and** `static_assert(offsetof(T, f) == k)` for **every field**;
- a leading `u32 format_version`; **append-only growth** — new fields go at the end with the
  version bumped; never reorder, never repurpose; a reader refuses a newer-than-known version;
- serialized through the foundation's little-endian byte writers, never `memcpy` of the struct.

This is *stronger* than a fingerprint: a layout drift is a **compile error**, not a handshake
mismatch and never a desync at tick 40,000. The `static_assert`s are the pin; a compiler upgrade
that moved bytes fails to build.

**Do not memcpy `InputFrame` onto the wire even so.** It is 76 B of mostly-unchanged data; encode
it.

### 12.2 Encoding

Per tick, per peer:

```
  [ header: slot, epoch, base_tick, ack_bitmap(8), last_confirmed_tick ]
  [ changed-action bitmap (MAX_ACTIONS = 32 bits) ]
  [ for each changed action: flags(3 bits) + value(i8) ]
  [ pointer delta: 2nd-order (delta of delta), zigzag varint per axis ]
  [ REDUNDANCY_TICKS of the same, delta-chained ]
```

- **Changed-action bitmap** is the main lever: most actions are idle on most ticks. Improves at
  60 Hz (lower per-tick change probability).
- **Digital actions** need only 3 edge flags; the `i8` value is implied.
- **Analog values** are already 8-bit at capture; the encoder carries them verbatim.
- **The encoder is lossless over the captured integers.** It is entropy coding only. The sender
  applies the same bytes it transmits, so "what I sent is what everyone applies" holds by
  construction and no quantize-then-apply ordering rule is needed.
- **Pointer** as second-order integer delta: pointer velocity concentrates near zero; at 60 Hz the
  residual is a few bytes. (Rev 3's polar quantization is dropped — it was lossy, and lossy on the
  wire is only legal if the sender also applies the decoded value. Lossless is simpler and the
  pointer resolution question belongs to `INPUT.md`'s capture quantization, not to the wire.)
- **`tick`** is implied by `base_tick` + index, never transmitted per frame.
- **`ack_bitmap`** is 8 bits at N = 8.
- **`last_confirmed_tick`** carries the §9.5 constraint: 4 B per packet, and it is what makes a
  behind-successor detectable.

### 12.3 Sizes

```
  Raw InputFrame (MAX_ACTIONS = 32)             76 B
  Encoded, steady state                         ~2-6 B      <- planning figure: 6 B
  Encoded, all actions changing                 ~40 B       <- worst case, rare
  Coordinator downstream packet
    (7 slots x 9 ticks x 6 B + 40 B header)     ~418 B      <- comfortably inside MTU
  Same, worst case (7 x 9 x 40 B + 40 B)        ~2560 B     <- exceeds MTU
```

**T-N-07 (ticks-per-packet backoff) is retained at Low severity.** The worst case still fragments.

> Under sustained input churn, reduce ticks-per-packet (and therefore the recovery window) rather
> than emit an oversized datagram. Floor at 3; below that accept fragmentation and log it.

All 32 actions changing on every one of 9 consecutive ticks is not a shape human input produces.
ENet would fragment transparently on a reliable channel; on the unreliable `INPUT` channel a
fragmented datagram is lost if any fragment is, which is why the guard exists above ENet.

---

## 13. Archive format and compression

### 13.1 Two formats

| Property | Wire | Archive |
|---|---|---|
| Causality | Strictly causal | Non-causal — can model the whole session |
| Redundancy | 9 ticks | Zero |
| Framing | Per packet | None |
| Latency sensitivity | Total | None |

Compression latency is irrelevant: the rollback window touches only the last N ticks, which stay
hot and uncompressed in a `RingBuffer`.

### 13.2 The entropy floor

A player generates ~3–5 decisions/s at 4–6 bits each plus a smooth autocorrelated pointer signal:
**30–40 bits/s per player** ≈ 4.5 B/s.

```
  30-minute, 8-peer session at the floor         ~63 KB
  same, wire format stored raw (60 Hz, 6 B)      ~5.2 MB
  same, InputFrame stored raw (60 Hz, 76 B)      ~66 MB     (was ~232 MB at 268 B)
```

The floor is a property of human input; it does not change with tick rate or struct size.

### 13.3 Chosen encoding

**Step 1 — columnar.** Per-peer, then per-channel: each action's flag stream separately, pointer
axes separately. Mixing channels destroys each one's statistics.

**Step 2 — transition encoding.** A button held for 120 ticks is **one event, not 120 samples**.

```
  record: (delta_tick: varint, channel: u8, new_value: varint)
```

A 30-minute session is 108,000 ticks per peer and ~5,000–8,000 transitions — the count is set by
the human, not the clock. `delta_tick` grows ~1.5 bits on average vs 20 Hz; that is the entire
cost of 60 Hz.

**Step 3 — pointer second-order delta.** Store velocity, not position; range-code the residual.

### 13.4 Results

| Stage | 30-min, 8-peer |
|---|---|
| `InputFrame` raw at 60 Hz | ~66 MB |
| Wire format stored raw | ~5.2 MB |
| **Columnar + transition encoding (chosen)** | **~150–175 KB** |
| + predictor-driven arithmetic coding (deferred, §15.2) | ~80–100 KB |
| Entropy floor | ~63 KB |

**Target: ~165 KB for 30 minutes.** Under `Persistent` the log between durable checkpoints (5 min)
is ~28 KB.

**Own crc32 per block** (`src/foundation/`, table-driven) for integrity. No DEFLATE: LZ over
entropy-coded data buys little and adds a decode dependency.

### 13.5 Log retention under `Persistent`

> Segments older than the last **durable** checkpoint may be discarded. World identity survives
> truncation because it is chained over checkpoints, not the log (§11.5).

Retain per world: the full chain (32 B per durable checkpoint — 2.3 KB per six hours), the last
`DURABLE_KEEP` (5) checkpoints, and every segment after the oldest of those.

### 13.6 What the log gives free

One artifact is simultaneously **rejoin tail** (§10.4), **desync reproduction case** (§14.4),
**spectator feed** (a peer that simulates and never inputs), and **replay file** (§15.3 caveat).
Four features that are normally four subsystems, because the sim is exactly reproducible.

---

## 14. State hash and desync diagnosis

### 14.1 What makes a hash honest (INV-8)

No canonicalization pass exists: nothing hashed is a float, so NaN, −0, and denormals cannot occur.
What replaces it is layout discipline, enforced where the arenas are declared (`MEMORY.md`,
`ECS.md`, `ALLOY.md`), not in the netcode:

- hash `[base, used)` of each registered arena, never capacity;
- every hashed struct explicitly padded (`static_assert(sizeof)` against the sum of fields);
- arenas zero-filled on commit (fresh pages from the OS; `memset` on reuse);
- no pointers in hashed state (debug reflection walk asserts it);
- rapidhash, pinned seed, pinned implementation, vendored — changing any of it is a fingerprint
  bump.

**Sort and key on integers only** — trivially satisfied: there are no floats to reach for.

### 14.2 What is hashed

| Hashed | Not hashed |
|---|---|
| Authoritative arenas (particles, bodies, SDF stores, bond/cavity/circuit graphs, constraint lists, bulk basins, ECS component columns) | Transient scratch (broadphase, coarse sampling) — derivable from authoritative state |
| Integer conservation totals (Σ mass-quanta per species, Σ moles, Σ charge, Σ load) | Events — notifications, never authoritative, **never hashed** |
| | Render/interpolation state; the Luau heap (never authoritative, PIVOT §7) |

Alloy's **hash-region integrity** invariant keeps this honest: mutating a transient buffer must
*not* move the hash; mutating any authoritative pool *must*.

### 14.3 Exchange

Per-arena hashes (u64 each, folded to one digest plus the per-arena vector on request) gossiped on
the `CONTROL` **mesh** (INV-9) every `CHECKSUM_INTERVAL_TICKS` (30), computed at `LAST`. Hash
**confirmed** state only — speculative divergence is expected. Mesh delivery matters for the same
reason as suspicion gossip: routing the check through a possibly-faulty coordinator defeats it.

### 14.4 Diagnosis — the valuable half

With arithmetic bit-exact by construction, **a production desync is UB or a logic bug**, not a
condition. Hunt it with UBSan/ASan on the reproduction.

```
  1. Binary-search the divergence tick against the retained log.
  2. Pass x arena bisection localizes to a pass and an arena (then to a field, via the
     reflection tables' field-by-field diff).
  3. Package: log segment (~28 KB) + last matching checkpoint + build_fingerprint
     + reflection_table_hash + world chain entry + each peer's platform/ISA/opt-level/worker-count.
  4. Upload as telemetry.
```

A perfect deterministic reproduction, replayable offline on any machine. Instrumentation writes
off the hot path; **no clock reads inside sim TUs** (symbol gate). Diagnosis tooling lives outside
the sim boundary.

### 14.5 Recovery

Pull the divergent arena's confirmed snapshot from a quorum-agreed peer, splice, resim forward.
The player never drops. Recovery is secondary; if it fires often, §14.4 is what fixes it.

---

## 15. Versioning

### 15.1 Four things must match, and they are not the same thing

| What | Mechanism | Frozen? |
|---|---|---|
| **Wire/archive syntax** | `format_version` leading every `WIRE_STRUCT` | Ours; append-only |
| **Struct layout** | `WIRE_STRUCT` static_asserts (compile time) + `reflection_table_hash` (handshake) | Pinned by assert |
| **Sim behaviour** | `build_fingerprint` in handshake, checkpoint header, chain entry; refuse on mismatch | Not frozen |
| **Session model** | `session_model` + `origin` in handshake; mismatch fails at handshake | Per world, at creation |

**`build_fingerprint`** = hash(compiler version, flag set, source tree, **sim-script Luau
bytecode**, compiled data tables). Anything that can change a tick's bytes is in it; clang is
pinned per release (`BUILD.md`), so two peers on one release agree by construction.

**`reflection_table_hash`** = hash over the ECS X-macro field tables (name-hash + kind + offset,
in declaration order) for every registered component and every hashed pool. It is the cross-peer
layout check for state that is hashed but not a hand-written `WIRE_STRUCT`.

**Handshake, concretely:**

```cpp
struct Handshake {                      // WIRE_STRUCT, 104 B
    u32 format_version;                 // 0
    u32 session_model;                  // 4   SessionModel
    u32 origin;                         // 8   Origin
    u32 max_actions;                    // 12  MAX_ACTIONS, belt and braces
    u8  build_fingerprint[32];          // 16  BLAKE2b-256
    u8  reflection_table_hash[32];      // 48  BLAKE2b-256
    u64 tick0_state_hash;               // 80  fold of per-arena rapidhash
    u8  world_chain_head[16];           // 88  truncated chain[K]; full 32 B follows on BULK
};
static_assert(sizeof(Handshake) == 104);
static_assert(offsetof(Handshake, build_fingerprint) == 16);
static_assert(offsetof(Handshake, world_chain_head) == 88);
```

Sent before any input flows. Any mismatch ends the session with a named diagnostic. A peer with a
**different world chain head** is told it holds a fork (§11.5), not silently joined.

### 15.2 Why predictor-driven archive coding is deferred

Feeding the §7.5 predictor's distribution to an arithmetic coder reaches ~80–100 KB. **Deferred
because it makes the predictor load-bearing for decode:** retrain it and every archive becomes
unreadable unless every predictor version is kept decodable forever. Worse under `Persistent`,
where a retrain would strand worlds mid-lineage. ~45% of size is not worth that.

### 15.3 Sim versioning is inherent

The log stores inputs; reconstructing state requires the same simulation. Retune damage, touch a
det-math kernel, change a script — the log decodes perfectly into a *different world*.

| Use case | Cross-version need | Solution |
|---|---|---|
| Rejoin | None — same build guaranteed | Nothing |
| Desync repro | None — you *want* the exact build | Fingerprint, refuse on mismatch |
| Spectator | None | Nothing |
| **`Persistent` world across a patch** | **Real, unavoidable** | **Checkpoint is the migration unit**: load state via the reflection-driven encoder (`ECS.md`), never replay across the boundary. Chain records the fingerprint per entry |
| Replays surviving patches | Full sim versioning | Ship historical sims, or accept expiry |

A world whose chain crosses builds is legitimate; a *log* that crosses builds is not.

**Compiler upgrades.** Clang is pinned per release. A `WIRE_STRUCT` is pinned by its
`static_assert`s, so a compiler upgrade that moved bytes is a **compile error, not a data-loss
bug**. Checkpoints written by release N are read by release N+1 through the name-keyed reflection
decoder, which is layout-independent by construction.

---

## 16. Budgets

### 16.1 Bandwidth (60 Hz, N = 8, 6 B/peer/tick, `REDUNDANCY_TICKS = 9`)

The coordinator omits a peer's own frames from its downstream — 7 slots, not 8.

| Path | Size |
|---|---|
| Peer upstream → coordinator only | ~5.5 KB/s (0.045 Mbit/s) |
| **Peer upstream → coordinator + 2 shadows** | **~16.5 KB/s (0.135 Mbit/s)** |
| Coordinator downstream, per peer | ~24.5 KB/s |
| **Coordinator total upstream** | **~171 KB/s ≈ 1.40 Mbit/s** |
| Shadow extra downstream | ~38.6 KB/s |
| `CONTROL` mesh, per peer aggregate | < 2 KB/s |
| Lobby RTT probing, per peer | < 0.2 KB/s |

ENet per-packet overhead (~10–14 B command header on top of UDP) is inside the 40 B header
allowance used above.

| Peers | Coordinator upstream (60 Hz) | Rollback freq at p = 0.02 |
|---|---|---|
| **8** | **1.40 Mbit/s** | **13%** |
| 16 | 6.12 Mbit/s | 26% |
| 32 | 25.5 Mbit/s | 47% |

Cost ∝ (N−1)². 16 is marginal, 32 absurd. A 16-peer variant would drop toward 30 Hz and
reintroduce what §4.4 deleted — documented, not gated (App. A).

**Residual capability concern — R13.** Nothing prevents succession handing the role to a peer that
cannot sustain 1.40 Mbit/s. Mitigation is structural: capability-ordered seating (§9.2). Drift
over a long session is accepted as degradation; remedy is end-and-reseat, free under `Persistent`.

### 16.2 CPU (16.7 ms frame; model until G-05)

| Item | Cost |
|---|---|
| Alloy tick, nominal load, 8 cores | ~8.0 ms |
| Encode + decode 8 peers' frames | < 0.1 ms |
| Per-arena hashing (rapidhash over used extents) | ~0.5 ms |
| **Steady state, no rollback** | **~8.1 ms** |
| Closure-scoped rollback, 1 island of ~50, depth 6 | ~1.0 ms |
| **Global field resim, depth 6 (§3.7, mandatory)** | **~3.0 ms** |
| **Tick with a rollback** | **~12.1 ms of 16.7 ms** |
| Full-world rollback, depth 6 | **~48 ms — not affordable** |

Headroom on a rollback tick is ~4.6 ms: affordable, not comfortable, and a second reason the
horizon stays short. Per-peer, unaffected by peer count. The Pi question is §19.4.

### 16.3 Memory and storage

| Item | Size |
|---|---|
| Archive log, 30 min, 8 peers | ~165 KB |
| Log between durable checkpoints (5 min) | ~28 KB |
| Rejoin log tail (≤ 5 s hot checkpoint) | ~1 KB |
| Snapshot hot ring | 2 × arena-set size |
| Rollback ring | `CONFIRMATION_HORIZON_TICKS` × arena-set size, allocated once |
| World chain, 6-hour session | ~2.3 KB |
| Durable checkpoint retention | `DURABLE_KEEP` × arena-set size |
| Arena set size | **Unknown — needs Alloy's real pool sizing (T-A-03)** |

Arena-set size is the one unknown that most affects `Persistent`. §19's ballast measures the shape.

### 16.4 Latency

| Item | Value |
|---|---|
| Sim tick = network tick | 60 Hz (16.7 ms) — **one rate** |
| Local input delay | `LOCAL_INPUT_DELAY_TICKS` 3–6, adaptive per peer (§7.4) |
| Confirmation horizon | Tunable — **also failover cost and irreversible-display delay** |
| Failover, detected | 1 tick + one horizon-depth rollback |
| Failover, graceful | 0 rollback — sequenced at a known tick (§10.3) |
| Rejoin, warm (≤ 5 s hot checkpoint) | snapshot transfer + ≤ 2.4 s replay |
| Rejoin, cold (`Restored` start) | §11.3 table + ≤ 2.4 s replay |

---

## 17. Risks

Ordered by cost if discovered late. Rev 3 numbering preserved where the risk survives.

**R1 — Hidden information. CLOSED** (2026-08-21, PIVOT §8): full-world visibility. Reopening is a
redesign trigger.

**R2 — `v_max` enforcement is data-level (§3.3).** A code path outrunning its declared entry is
caught by a debug assert only. **Confidence: Medium.** The causal deadline's soundness rests on it.

**R3 — Closure-scoped restore, both halves (§3.7, §7.3). Highest-risk unknown.**
(a) *cost:* Alloy's pools are global SoA, so a scoped restore is a scatter; (b) *scope:* the
closure can approach the whole world under dense load. **Confidence: Low** that both are free.
Gate 4 / T-A-01 measures both; §19.6 S-14 measures (b) early on Hovel.

**R5 — Gap re-sequencing under partial views (§9.5). Test, not argument.** The log-completeness
constraint closes the behind-successor hole. §19.6 S-08.

**R6 — Eviction idempotency under re-sequencing (§10.2).** If the coordinator dies mid-eviction the
successor re-sequences and the eviction tick can shift. Every sequenced one-shot (leave, custody
handoff, construction commit) must be **idempotent under re-sequencing**, not merely deterministic.

**R10 — Quorum-loss termination vs partition rule (§8.3, §10.8).** Needs a correctness argument
that no sequence of evictions lets a minority shrink its own denominator into a majority.
**Proposed guard: eviction requires quorum of the *pre-eviction* live set.** Proven, not asserted.

**R11 — Inter-session world forking (§11.5, §11.6; `Persistent` only).** Ordinary use creates
forks (A+B Tuesday, A+C Wednesday). No merge. Mitigated by chain detection + custody baton; the
canonical-save rule is a product decision.

**R12 — CSPRNG. RESOLVED — entropy behind the platform seam.** `BCryptGenRandom` /
`getrandom(2)` in `PLATFORM.md`, header unreachable from sim, symbol gate proves it; Monocypher
Ed25519/BLAKE2b; never roll own crypto. `Persistent` is no longer blocked.

**R13 — Coordinator capability drift (§16.1).** Mitigated by capability-ordered seating; 1.40
Mbit/s is a low bar. Reopening condition: if sessions routinely run on an under-capacity
coordinator, reintroduce a *single* gate — not a score.

**R16 — UB is the residual silent-desync class.** *(new in tidelock)* Fixed point removes FP as a
source of divergence; what remains is signed overflow, uninitialized padding, out-of-bounds
reads, and data races — all silent, all ISA/opt-level-sensitive. Mitigation: sanctioned
wrapping/saturating helpers only in quanta paths, explicit padding, zero-filled arenas, UBSan +
ASan in the determinism CI (PIVOT §2), and §19.6 S-03 as a free regression check. A Gate 0 G-06
divergence is by definition in this class.

**Void by fixed point (recorded so they are not reintroduced):**
- **R14** (`@fast_math` defeating strictness) — no floats, no fast-math on sim paths.
- **R8 / T-O-03** (mixed arch flags) and **T-O-04** (FP inside workers) — integer ops have no
  arch- or thread-dependent rounding. One cheap sanity scenario survives as S-03.
- **R15** (Ore freeze process risk) — Ore is retired.

**Withdrawn earlier:** R4 (coordinator upstream), R7, R9.

---

## 18. Implementation order

### 18.1 The netcode needs *a* deterministic sim, not *the* deterministic sim

Hovel (§19) provides one, decoupling netcode from Alloy for Phases 1–7 and letting R3(b) be
measured on a cheap region analogue **before Alloy exists**.

### 18.2 Order

```
  GATE 0  (foundation)  fixed-point XPBD + PBF convergence & cost bench (PIVOT §10)
                        G-06: run twice + PC-vs-Pi cross-compiled, identical hash traces
  GATE 1  (core)        v0 "window + moving sprite + 60 Hz", test-infra-first
  GATE 2  (core)        per-arena hash + record->replay->compare (TESTING.md)
  ---- netcode may start here, against Hovel rather than Alloy ----
  Phase 1  InputFrame encoder/decoder + archive format + log retention
  Phase 2  Two-peer ENet, fixed coordinator, quorum finalization, hash exchange
  Phase 3  Scale to 8, per-peer adaptive delay, commitment windows, split render tolerance
  Phase 4  Always-on shadows (top two), mirror only
  Phase 5  Failure detection, quorum suspicion on the CONTROL mesh, epochs,
           log-completeness constraint, failover, gap re-sequencing
  Phase 6  Phantoms, graceful leave, eviction, rejoin (snapshot + tail),
           quorum-loss termination, partition majority rule
  Phase 7  SessionModel::Persistent — two-tier checkpoints, hash chain, custody, session auth
           (UNBLOCKED: entropy behind the platform seam + Monocypher)
  Phase 8  Product: NAT/signalling (per §5.5 ruling), spectator, commit-reveal

  In parallel, on the real stack:
  GATE 3  (Alloy)  prototype: island layer + registered arenas + per-arena hashes
  GATE 4  (Alloy)  CLOSURE-SCOPED ARENA RESTORE + closure size under load   <-- R3, both halves
```

**Gate 4 is the decision point for the real game.** If closure-scoped restore is not affordable,
redesign to delay-only lockstep with no speculation — smaller, simpler, shippable, and what Alloy's
snapshot API already assumes. Do not discover this in Phase 3.

Each phase authors its own positive and negative tests; a phase ends on the full green gate.

---

## 19. Hovel — the three-machine harness

### 19.1 What this proves, and what it does not

The moat claim: **a deterministic colony sim, in lockstep, with 8 players and persistence.**
Hovel establishes whether it is buildable at 1/100th scale — and finds the result that would say
it is not.

**Hardware:** PC (x86-64 Windows), Steam Deck (x86-64 Linux), Pi 4 (aarch64 Linux).

| Proves | Does not prove |
|---|---|
| Cross-ISA determinism of the *new stack* under a colony-shaped workload, on silicon | That Alloy is deterministic — Alloy does not exist yet |
| Consensus, succession, failover, eviction, rejoin under real packet loss | Real-world NAT (one LAN; §5.5 untested) |
| `Persistent` end-to-end: leave, solo, resume, custody, fork detection | Absolute performance of the real sim |
| Closure-explosion dynamics (R3b) with a region analogue | Closure *restore cost* in Alloy's SoA pools (R3a) — Gate 4 |
| INV-7 across 1/2/4/8 workers on genuinely different core counts | 8-peer behaviour until more machines exist (§19.10) |
| Whether Pi 4-class hardware holds a lockstep budget | Console/handheld certification |

> **The harness sim is disposable.** A `tests/hovel/` throwaway exe in the engine's C++ subset,
> exercising `src/net/`, deleted afterwards. It must never accrete gameplay; no code in it is
> promoted. **Hovel** — a crude dwelling, built to be abandoned.

Where it sits: between "nothing" (tidelock has no proven baseline — the Ore capstone's evidence
does not transfer, §0.3) and the end-game (the full Alloy world, 8 real players, hours-long
soak). Hovel adds every protocol mechanism in this doc while keeping the sim trivial, so when
Alloy is substituted the sim is the *only* new variable.

### 19.2 Hovel — the throwaway sim

**World.** `W × W` tile grid (default 128 → 16,384 tiles). Per tile: `u8 terrain`,
`u16 building_id`, `u16 region_id`. All in registered arenas (`MEMORY.md`).

**Actors.** Up to 8 pawns per player: integer position, target, job. Movement is
greedy-toward-target — deliberately not A*, since pathfinding cost would dominate the measurement.

**Resources.** Two integer stocks per player (`wood`, `stone`) plus one shared (`power`), so both
private and contended state are exercised.

**Verbs** and their action class (§8.4), covering all three substitution policies:

| Verb | Class | Exercises |
|---|---|---|
| Move cursor | `AXIS` | Decay-to-neutral; the pointer delta encoder |
| Select pawn | `EDGE` | Null substitution |
| Order build at cursor | `EDGE` | **Irreversible display delay** (§7.6); resource commit |
| Order upgrade | `EDGE` | Multi-tick job with a commitment window (§7.5) |
| Hold-to-demolish | `LATCHED` | Hold substitution; region *splitting* |
| Cancel | `EDGE` | Rollback of a pending irreversible |

**Regions are the island analogue.** Connected components of built tiles via deterministic
union-find over dirty tiles — the same merge/split dynamics as Alloy pass 5 at a hundredth of the
complexity. **This is what makes R3(b) measurable before Alloy exists.**

**A deliberate globally-coupled subsystem (Milestone B):** a `32 × 32` **`fx<i32,20>` heat
field**, Jacobi-relaxed, buildings as sources, decaying toward zero. It has no region structure,
so it must resim globally on rollback — the §3.7 rule on a system small enough to instrument.
Decay to exactly zero is a non-event in fixed point (no denormals exist); the field's purpose is
the global-resim path and the fx-arithmetic surface, nothing else. The rev 3 denormal experiment
is void.

### 19.3 Determinism budget — what Hovel may and may not use

Hovel is under the same contract as the real sim:

- All Hovel sim code compiles into the sim static lib and passes the **symbol gate** (no alloc,
  libm, clock, entropy, io, socket symbols); no static mutable state.
- **Milestone A is the whole story** (PIVOT §10): integer-only lockstep, which is now the same
  thing as "the sim". A desync at A is a netcode bug or UB — the search space is already halved by
  construction.
- **Milestone B adds the fx heat field and nothing else.** A desync at B and not at A is in the fx
  helpers or the global-resim path, localized by construction.
- Sorting and keying on integers only (§14.1).
- Per-arena rapidhash every `CHECKSUM_INTERVAL_TICKS` over used extents.
- `WIRE_STRUCT` on every wire, checkpoint, and chain struct.
- **Gameplay mapping runs in a Luau sim VM** with the stock `math` library removed, so the
  bytecode-in-fingerprint mechanism (§15.1) is exercised before Alloy depends on it.

**Ballast.** Hovel's real arena set is ~80 KB — too small to test §11.3 honestly. `BALLAST_ARENA_BYTES`
is a deterministically filled registered arena, snapshotted and hashed, never read by the sim.
Set to 1 / 4 / 16 MB to measure session-start and rejoin against §11.3's table.

### 19.4 Hardware roles and sizing

| Machine | ISA / OS | Role | Why |
|---|---|---|---|
| PC | x86-64 / Windows | Peer + usual coordinator + build host | Fastest; only Windows peer (WinSock + Windows timer path under ENet) |
| Steam Deck | x86-64 / Linux | Peer | Same ISA, different OS/libc — isolates OS effects from ISA effects |
| Pi 4 | aarch64 / Linux | Peer, **binding constraint** | Different ISA, different core count, slow enough to be the realistic worst peer |

**Sizing rule: dial the load so the Pi 4 hits ~4 ms/tick, not the PC.** Hovel is a **dial**:
`SIM_LOAD` scales grid, pawns, buildings, and field iterations together.

```
  For each machine: sweep SIM_LOAD, record p50/p95/p99 tick time.
  Report: load at which each machine hits 4 ms, 8 ms, 16.7 ms.
  Derive: heterogeneity ratio PC : Deck : Pi 4.
```

That ratio tells the real engine what fraction of the desktop budget a Pi-class peer carries.
There is no measured anchor from the Ore soak — its workload ran a different stack; start the
sweep from G-05's Pi figure instead.

**Build and deploy — cross-compile once from the PC.** Clang cross-targets aarch64 natively
(`BUILD.md`); one tree, one flag set, three binaries. A divergence is then a *sim* claim, not a
toolchain-portability claim. **Pin the artifact and freeze it for the run**: record the commit in
run metadata; never rebuild or repin while a soak is in flight.

**S-02 scheduling.** v0 is single-threaded; the job system lands post-v0 (PIVOT §12a). S-02 runs
against `JOBS.md`'s own invariance harness as soon as the parallel implementation exists, and
Hovel's Milestone A result stands without it.

### 19.5 Milestones

| Milestone | Adds | Gate |
|---|---|---|
| **A — integer lockstep (the whole story)** | Hovel integer-only, 3 peers, fixed coordinator, quorum fold, hash exchange | 1 h, 3 machines, zero hash divergence at every checkpoint |
| **B — fx heat field** | `fx<i32,20>` Jacobi field, global resim on rollback (§3.7) | Same gate; any divergence localized to fx helpers or the resim path |
| **C — failure and rejoin** | Suspicion, epochs, succession, failover, phantoms, leave, eviction, rejoin | S-04 … S-09 green |
| **D — persistence** | `Persistent`, two-tier checkpoints, chain, custody, solo play, resume, session auth | S-10 … S-12 green; a world survives three real days and a rebuild |
| **E — soak and scale — the named successor to Ore's 43 M-tick soak** | 10 h continuous on three machines; then 8 peers when hardware allows | Zero divergence; §19.9 thresholds |

A–D need only the three machines on the desk. E's claim is strictly easier than the one the Ore
soak proved (integer vs strict float) and is the one tidelock must actually prove.

### 19.6 Scenario catalogue

Every scenario is scripted, seeded, replayable from its archive — a failure ships as a repro.

| ID | Scenario | Exercises | Pass |
|---|---|---|---|
| **S-01** | 3 peers, 30 min, all building/upgrading concurrently | Baseline; concurrent edits to shared `power` | Zero divergence; all three archives byte-identical after canonical re-encode |
| **S-02** | Each machine runs 1, 2, 4, 8 workers in turn (once `JOBS.md` is parallel) | INV-7; the chunk-keyed rule under stealing on three core counts | Identical hash trace at every worker count on every machine; plus one mixed-pair run |
| **S-03** | PC runs `-O0`, `-O2`, `-O2 -march=native` builds as peers | **Free regression check**, downgraded from R8: fixed point makes this pass by construction | Identical hashes; **a failure is UB** — hunt with UBSan, never gate the handshake on flags |
| **S-04** | Deck sends graceful leave, rejoins 5 min later | §10.3, §10.4 snapshot + tail | No phantom; `QUORUM` recomputes at the sequenced tick; rejoin takes the snapshot path |
| **S-05** | Pi 4 cable pulled 10 s, restored before evict | SUSPECT, phantom via Markov predictor | Phantom inputs identical on PC and Deck; no divergence on resume |
| **S-06** | Pi 4 pulled 40 s (> `SUSPECT_TO_EVICT_TICKS`) | Sequenced eviction; R6 idempotency | All peers drop the avatar on the same tick; rejoin appends to succession |
| **S-07** | PC (coordinator) killed mid-session | §9.5 failover, gap re-sequencing | Deck takes over within 1 tick + horizon rollback; no peer accepts a tick it had not confirmed |
| **S-08** | Stall one peer's confirmation, then kill the coordinator | **Behind-successor** — the R5 hole | The stalled peer's epoch claim is *refused*; a caught-up peer succeeds |
| **S-09** | Kill 2 of 3 simultaneously | §8.3 quorum-loss termination; R10 | Survivor writes a checkpoint and stops; checkpoint bit-identical to the last confirmed |
| **S-10** | Deck and Pi leave; PC solo 30 min; both rejoin | `live = 1`; §10.4 crossover | `QUORUM(1) = 1` with no special-casing; rejoin **must** use snapshot; replay-only measured as wrong |
| **S-11** | Full session end; all machines rebooted; resumed next day | `Persistent` + `Restored`, §11.3 | `tick0_state_hash` matches on all three; chain head agrees; play continues |
| **S-12** | Custody PC → Deck; then force-takeover from Pi against a held baton | §11.6, R11 | Handoff acked, signed, chained; forced takeover yields a **detected fork** with the correct point |
| **S-13** | Impairment sweep: latency 20/60/150 ms, jitter 0/10/40 ms, loss 0/1/5% | §7.4, `REDUNDANCY_TICKS`, degradation shape | Adaptive delay isolates the impaired peer; others' rollback depth unchanged |
| **S-14** | Scripted mass-merge: one bridging tile merges 6 regions, under rollback | **R3(b) closure explosion** | Closure size logged per rollback; the distribution is the deliverable |
| **S-15** | 10 h continuous, three machines, mixed activity (= Milestone E) | Soak; the named successor to the 43 M-tick run | Zero divergence; no leak; checkpoint cadence stable |

### 19.7 Impairment shim

**Do not use `tc netem`.** Needs root, absent on Windows, not reproducible.

Impairment lives **in the transport layer, outside the determinism boundary**, as a debug shim
between the `src/net/` protocol code and the ENet wrapper:

```cpp
struct Impair { u8 drop_pct; u16 delay_ms; u16 jitter_ms; u8 reorder_pct; u8 pad[2]; };
struct ImpairmentShim {
    u64    seed;                   // its own PRNG stream; NEVER the sim's keyed RNG
    Impair per_peer[MAX_PEERS];    // asymmetric impairment
};
```

1. **Reproducible.** Same seed, same drops, same run.
2. **Cross-platform.** Identical on Windows, Deck, Pi.
3. **Legally nondeterministic.** Transport side of INV-2; own PRNG stream; **compiled out of
   release builds** and `static_assert`ed absent there.

Per-peer asymmetry matters: the interesting case is one bad peer among good ones.

### 19.8 Metrics and instrumentation

Per peer, one CSV row per tick, **buffered and written off the hot path — no clock reads inside
sim TUs** (symbol gate):

```
tick, wall_us, sim_us, net_encode_us, net_decode_us, hash_us,
rollback_fired, rollback_depth, closure_regions, closure_tiles,
confirmed_tick, horizon_ticks, local_delay_ticks,
bytes_in, bytes_out, packets_in, packets_out, packets_dropped_shim,
rtt_p50_us, rtt_p95_us, live_count, quorum, coordinator_slot, epoch,
state_hash_lo64
```

Plus an event stream: joins, leaves, suspicions, evictions, failovers, epoch claims (accepted and
refused), checkpoint writes (duration, size), custody transfers, chain entries, hash mismatches.

**Operational discipline (imported from the Ore soak's own findings — the method transfers):**
- Adjudicate progress against the log's own `wall_us`, never the poll clock; a watchdog polling
  faster than the log emits shows repeated values that are not a stall.
- Everything to stderr (stb_sprintf; locale-free), live-readable during a run.
- Specify the CLI signature in the runbook and test it.
- Smoke the exact artifact before a long run, even if yesterday's smoke was green.

**The five numbers that matter most:**
1. **Closure size distribution under load** (S-14) — the R3(b) answer.
2. **Heterogeneity ratio** PC : Deck : Pi 4 at equal `SIM_LOAD`.
3. **Rollback frequency and depth** vs measured `p`, against §7.5's model.
4. **Coordinator upstream, measured**, against 1.40 Mbit/s.
5. **Rejoin and session-start wall time** at 1 / 4 / 16 MB ballast, against §11.3.

### 19.9 Pass/fail thresholds (frozen before results exist)

| Metric | Pass | Investigate | Fail |
|---|---|---|---|
| Hash divergence | 0 | — | ≥ 1 unexplained |
| Closure size, p95 (S-14) | < 5% of world | 5–20% | > 20% sustained |
| Rollback tick cost, p95 | < 6 ms | 6–10 ms | > 10 ms |
| Pi 4 tick time at target load, p99 | < 8 ms | 8–14 ms | > 16.7 ms |
| Coordinator upstream | within 20% of 1.40 Mbit/s | 20–50% over | > 2× |
| Rejoin, 4 MB ballast | < 10 s | 10–30 s | > 60 s |
| Archive size, 30 min, 3 peers | < 80 KB | 80–150 KB | > 300 KB |

### 19.10 Scaling to 8

More Pi 4s are the cheapest path (five units ≈ £300–400). Two free steps first:

- **Headless peers on one machine** — multiple Hovel processes over loopback. Exercises
  `QUORUM(live)` arithmetic, succession, shadow selection, eviction at N = 8 *logically*.
- **Mixed: 3 real + 5 headless.** Real heterogeneity where it matters; the coordinator's upstream
  measurement stays honest if the shim applies per-peer impairment.

What 8 adds that 3 cannot: the `1−(1−p)^7` curve is only meaningful at 7 remotes; `QUORUM` 8→5
transitions; the simultaneous-loss deadlock (R10) in its real shape.

### 19.11 What would falsify the moat

Stated in advance, so the answer is not negotiated after the fact:

1. **Closure size explodes** (S-14 p95 > 20%). Speculation is dead; fall back to delay-only
   lockstep (what Factorio ships). Does not kill the project; the "fluid under contention" claim
   goes.
2. **Cross-ISA determinism fails on the new stack** (Milestone A or E, or S-02 under workers).
   By construction this can only be UB or a logic bug; it is the highest-information result the
   harness can produce and is hunted with sanitizers, not negotiated. A failure that survives the
   hunt would mean fixed-point-by-construction is not the guarantee this program rests on.
3. **Pi 4-class hardware cannot hold budget.** Not fatal; redraws minimum spec per G-05's severity
   split.
4. **Rejoin or session-start is unacceptable at realistic arena sizes** (S-10, S-11 at 16 MB).
   `Persistent` would need incremental snapshots — a significant addition to §11.3.
5. **R10 reproduces as a real deadlock** (S-09 at N = 8). Correctness rework of §8.3 before
   `Persistent` ships.

Any of 1, 3, 4 changes the design. Only 2 threatens the thesis.

---

## Appendix A — Rejected alternatives

| Alternative | Rejected because |
|---|---|
| **32-peer target** | Coordinator cost is quadratic; 32-player genres are PvP, which INV-1's maphack rules out |
| **16-peer target** *(documented stretch)* | 6.12 Mbit/s at 60 Hz would force ~30 Hz and hold semantics back. No gate; reopen on a concrete game concept |
| Client/server authority | Requires servers; discards the determinism investment; operational cost is the constraint the project exists to avoid |
| Coordinator simulates + broadcasts state | Redundant if peers verify, unfalsifiable authority if they don't |
| **Full P2P mesh for `INPUT`** | 28 links is relay-coverable; rejected on the **leaderless fold problem** (§6.1) |
| **Classic stall-lockstep** | Simpler (deletes §8.5–8.6, §9); rejected on product grounds — "waiting for players…" |
| **Mesh inputs + star ordering (hybrid)** *(deferred with reopening condition)* | Coordinator broadcasts only the fold: 0.19 Mbit/s. Decouples ordering from data, needs a fetch path and a new stall mode. **Reopen if** measured coordinator upstream binds or R13 bites |
| **Vivaldi network coordinates** | 8×8 is 28 pairs, directly measurable |
| **Scored election, hysteresis, tenure, migration margins** | Existed for scarcity at 32; replaced by the succession list + lobby seating |
| **Opportunistic in-match migration** | Same; failover-on-death only |
| **Shadow that sequences in parallel** | Ordering disagreement is not evidence; mirror alone gives the evidence that matters |
| **Rejoining ex-coordinator reinserted at slot position** | Flaps the role on every blip; append instead |
| **Quorum over `MAX_PEERS`** | 5-of-8 ends a session when three friends leave |
| **Global "hold" substitution** | Right for fire, wrong for movement; per-action-class instead |
| **20 Hz network tick with hold semantics** | Existed for 32-peer upstream; at 8 peers 60 Hz costs 1.40 Mbit/s and deletes four mechanisms |
| **Fixed world owner** | A solo player would not own their own work |
| **Free forking** | Fork management becomes core UX |
| Compiler-enforced bounded reach | No compiler to enforce it; Alloy's init validator covers it (§3.3) |
| Interaction-graph broadphase | Alloy's island layer already is it |
| Per-entity read/write sets | Nothing produces them |
| Sequencing intents (Option B) | Widens the wire and still puts game code in the contract; A keeps the wire to one struct |
| Cell-aggregator hierarchy | On a dense map islands rarely separate |
| Full Raft per tick | Sequencer + epochs + log-completeness is enough |
| **TCP, or an ENet reliable channel, on the input path** | Retransmit + head-of-line blocking stalls all inputs on one loss — the failure the design exists to avoid |
| **A separate TCP socket for `BULK`** | ENet reliable + fragmentation does the job on the one socket |
| **Reliable `CONTROL` channel** | Head-of-line on the path that must stay live when the coordinator misbehaves; gossip is idempotent, phases are app-acked |
| Input retransmission | 2+ RTT recovery vs 0 for redundancy at comparable bytes |
| **Lossy pointer quantization on the wire** | Only legal if the sender applies the decoded value; lossless entropy coding over integers captured at the source is simpler and makes "sent == applied" hold by construction |
| Predictor-driven archive coding | Predictor becomes load-bearing for decode; strands `Persistent` worlds on retrain |
| Phantom range markers in the archive | Same coupling through the back door |
| Local timeout → substitution | Half substitute, half don't — manufactured desync |
| Local suspicion → immediate eviction | Violates INV-4 |
| Relaying `CONTROL` through the coordinator | A faulty coordinator suppresses suspicion about itself (INV-9) |
| DEFLATE over the encoded archive | LZ over entropy-coded data buys little |
| `tc netem` for impairment | Needs root, absent on Windows, not reproducible |
| Handshake gate on matching arch flags | Existed for R8; void by fixed point — a flag-dependent divergence is UB, gate on sanitizers instead |
| NaN/−0/denormal canonicalization before hashing | No floats in hashed state; replaced by INV-8's layout discipline |

## Appendix B — Tunables

All tick counts are **sim ticks at 60 Hz**. One unit (§4.4).

| Constant | Value | Notes |
|---|---|---|
| `MAX_PEERS` | **8** | 16 feasible, untargeted (App. A) |
| `MAX_ACTIONS` | **32** | Compile-time, in the input header (`INPUT.md`). **Changing it is a wire-format version bump.** Every payload figure assumes it |
| `SIM_TICK_RATE` | 60 Hz | Network tick is the same tick |
| `LOCAL_INPUT_DELAY_TICKS` | 3–6 (50–100 ms) | Adaptive per peer (§7.4), sequenced |
| `REDUNDANCY_TICKS` | 9 (150 ms) | **≥ `CONFIRMATION_HORIZON_TICKS`**, `static_assert`ed. Raise to 16 under measured loss |
| `CONFIRMATION_HORIZON_TICKS` | Tune | **Speculation depth = failover cost = irreversible display delay** |
| `MAX_TICKS_PER_PACKET` | 9, floor 3 under churn | §12.3 MTU guard (T-N-07, Low) |
| `SUB_DECAY_TICKS` | 6 (100 ms) | `AXIS` substitution decay (§8.4) |
| `AOE_ISLAND_LIMIT` | 4 | Above this, telegraph required (§3.5) |
| `QUORUM` | Strict majority of **sequenced-live** | §8.3; also the partition rule |
| `QUORUM_LOSS_TICKS` | 600 (10 s) | Then terminate and checkpoint |
| `SUSPECT_TO_EVICT_TICKS` | 1800 (30 s) | Fixed tick count, never wall clock |
| `SHADOW_COUNT` | 2 | Top two successors, mirror only |
| `CHECKPOINT_HOT_TICKS` | 300 (5 s) | Ring of 2 |
| `CHECKPOINT_DURABLE_TICKS` | 18000 (5 min) | Plus on leave, quorum-loss, clean end |
| `DURABLE_KEEP` | 5 | Chain kept in full |
| `CHECKSUM_INTERVAL_TICKS` | 30 (0.5 s) | Per-arena hashes on the `CONTROL` mesh |
| `LOBBY_PROBE_HZ` | 1 | Full RTT matrix before session start |
| ENet channel ids | `INPUT = 0`, `CONTROL = 1`, `BULK = 2` | Fixed; inside `src/net/` only |

## Appendix C — Verification outcomes

Void as written in rev 3 (every row verified an Ore fact). Replaced by §0.3 "Assumptions
carried". Re-verification for tidelock happens at Gate 0 (G-06), GATE 2, and Hovel Milestone A —
against running code, not documents.

---

*Rev 1 (tidelock, 2026-08-22). Re-homed from `../foundry/NETCODE-DESIGN.md` rev 3 per
`PIVOT-DESIGN.md` §8: ENet transport, `WIRE_STRUCT` layout discipline, all-integer `InputFrame`,
rapidhash/BLAKE2b/Ed25519 via Monocypher, entropy behind the platform seam, INV-8 rewritten for
fixed point, R1 closed, R12 resolved, R8/R14/R15/T-O-03/04 void, Hovel Milestone B re-specified as
an fx field, Milestone E named as the soak's successor. Consensus, succession, disconnection,
session model, archive, and budgets carried whole. All budgets are models until Gate 0 reports.*
