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
- The physical *network-soak* bench is the two PCs (x86-64 Windows) now, the Steam Deck (x86-64
  Linux) when it joins; the Pi 4 left the program (ruled 2026-08-25), and where perf is *graded*
  is `WORKFLOW.md` §4's policy. The *target* set is
  `CANON.md`'s {Windows, Linux} × {x86-64, arm64} matrix — cross-ISA conformance runs on the
  hosted CI arm64 legs, not on owned hardware.

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
| Cross-ISA proof | Gate 0 G-06 (cross-leg bit-compare on the CI target matrix) + Hovel Milestone E |
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
> A 4-core handheld and a 16-core desktop share a match. The sim must produce identical hashes at 1/2/8/16
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
nothing else. It replaces the `InputFrame` *producer*; it does not modify the loop. (Exact call
order inside both systems and the producer: §20.5.)

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

### 5.5 NAT traversal — RULED 2026-08-22: LAN / direct-IP for v1

The project's trade-off axes are *single-process, deterministic, no services; not
latency/SPOF/CAP*. STUN + hole punching, a TURN relay, and session auth are a **service
dependency** — a new axis, not an implementation detail.

1. **Accept a signalling service.** Standard, works, adds an operated dependency.
2. **LAN / direct-IP only for v1.** No service. LAN parties and port forwarding.
3. **Platform-provided sessions** (Steam Datagram Relay or equivalent). Trades the service for a
   platform SDK — must live strictly outside the sim boundary and behind `PLATFORM.md`.

**Ruling: option 2.** One port-forwarded host (or LAN) is the normal configuration in this genre
(Valheim, Terraria, Factorio); it keeps the project's "no services" axis intact; ENet is
NAT-agnostic so nothing in this doc changes if a relay is added later. **Reopening condition:** a
shipped game that wants public matchmaking — then option 3 (platform relay behind `PLATFORM.md`,
an FFI dependency outside the sim boundary) is the next step, never option 1's own service. The
lobby seating (§9.2) needs the full RTT matrix, which direct IP provides.

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
for fixed point: pass at 20k particles per `GATE0-BENCH.md` §2's PC threshold (the Pi half left
with the Pi, 2026-08-25); > 8 ms PC at 20k is pivot-level. If 20k does not fit, the budget moves (counts, substeps), not the verdict. The budget
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
(Thresholds, the sequencing record and the raise/lower frame rule: §20.3(d).)

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
derived for 60 Hz. Integer-only, inside the boundary (INV-3). (Exact state, counts, tie-break and
phantom mask: §20.3(j).)

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
(Which bitmaps count, how they are broadcast, and the fold pseudocode: §20.3(b).)

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

Every peer computes the same successor with zero protocol (list operations and the cyclic
eligibility rule: §20.3(g)). **Appended, not reinserted:** naive
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
> irreversible-event display delay.** One constant, three consequences documented next to it.
> **`CONFIRMATION_HORIZON_TICKS = 6` (100 ms) — RULED 2026-08-22.** It equals the upper bound of
> `LOCAL_INPUT_DELAY_TICKS` (a peer at max delay is exactly at the horizon), satisfies
> `REDUNDANCY_TICKS (9) ≥ 6`, and is the depth every budget in §7.2 and §16.2 was computed at.
> Hovel S-13 measures the degradation curve around it; the value moves only by a recorded ruling.

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
(Local suspicion thresholds and the sequenced `SUSPECT`/`RESUME`/`EVICT` records: §20.3(e); the
full state × event table: §20.4.)

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
> gaps under ~7 s. Cold and warm rejoin are the same path. (Checkpoint choice, `BULK` framing,
> catch-up driver and hash verification: §20.3(h).)

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
may be discarded. (File format: §20.2.8; write procedure and chain append: §20.3(i).) **Torn-write asymmetry:** a corrupt checkpoint costs a replay under `Match` and
the colony under `Persistent`; the write-temp/fsync/rename discipline is not optional there.

### 11.5 World identity — the hash chain

```
  chain[0] = BLAKE2b(tick0_state_hash, world_seed, creation_nonce)
  chain[K] = BLAKE2b(chain[K-1], log_segment_hash[K-1 -> K], state_hash[K],
                     session_fingerprint[K], custody[K])
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
- **`last_confirmed_tick`** carries the §9.5 constraint: 8 B per packet (a `u64`, per the tick
  width rule in `FRAME-LOOP.md` §1), and it is what makes a behind-successor detectable.
  Exact header and body layout: §20.2.1–§20.2.2; encoder: §20.3(a).

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

(Byte layout: §20.2.9 — the channel byte is the stream header of the columnar block, each record
inside a stream is `(delta_tick, new_value)`.)

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
  3. Package: log segment (~28 KB) + last matching checkpoint + `build_id` + `session_fingerprint`
     (which covers the reflection tables) + world chain entry + each peer's
     platform/ISA/opt-level/worker-count.
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
| **Struct layout** | `WIRE_STRUCT` static_asserts (compile time) + the reflection tables inside `session_fingerprint` (handshake) | Pinned by assert |
| **Sim behaviour** | `build_id` + `session_fingerprint` in handshake, checkpoint header, chain entry; refuse on mismatch | Not frozen |
| **Session model** | `session_model` + `origin` in handshake; mismatch fails at handshake | Per world, at creation |

**`build_id`** (build-time) and **`session_fingerprint`** (init-time) are defined once, in
`BUILD.md` §5, and used under those names everywhere. `build_id` covers the source tree, the
semantic compile defines, the tier, the palette rev and the sim-script bytecode - and is
deliberately **target-independent** (`BUILD.md` §9 R-8), which is what lets one `build_id` cover the
every bench machine in §19.5's runbook; the compiler, triple and flag spellings live in
`build_env`, reported in CSVs and crash reports and never compared. `session_fingerprint` covers
`build_id` plus the
reflection field tables (the cross-peer layout check for hashed state that is not a hand-written
`WIRE_STRUCT`), the arena registry order, the action map, the compiled data tables and the
`SIM` cvars. Anything that can change a tick's bytes is in one of them; clang is pinned per
release, so two peers on one release agree by construction.

**Handshake, concretely:**

```cpp
struct Handshake {                      // WIRE_STRUCT, 120 B
    u32 format_version;                 // 0
    u32 session_model;                  // 4   SessionModel
    u32 origin;                         // 8   Origin
    u32 max_actions;                    // 12  MAX_ACTIONS, belt and braces
    u8  build_id[32];                   // 16  BLAKE2b-256 (BUILD.md §5)
    u8  session_fingerprint[32];        // 48  BLAKE2b-256 (BUILD.md §5)
    u64 tick0_state_hash;               // 80  fold of per-arena rapidhash
    u8  world_chain_head[32];           // 88  chain[K] in full
};
static_assert(sizeof(Handshake) == 120);
static_assert(offsetof(Handshake, build_id) == 16);
static_assert(offsetof(Handshake, session_fingerprint) == 48);
static_assert(offsetof(Handshake, world_chain_head) == 88);
```

Sent before any input flows. Any mismatch ends the session with a named diagnostic. A peer with a
**different world chain head** is told it holds a fork (§11.5), not silently joined. (Identity and
slot assignment ride alongside it in `JoinChallenge`/`JoinRequest`/`JoinReply`, §20.2.7.)

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
horizon stays short. Per-peer, unaffected by peer count. The weakest-peer question is §19.4.

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
| Confirmation horizon | **6 ticks (100 ms)** — **also failover cost and irreversible-display delay** (§9.5) |
| Failover, detected | 1 tick + one horizon-depth rollback |
| Failover, graceful | 0 rollback — sequenced at a known tick (§10.3) |
| Rejoin, warm (≤ 5 s hot checkpoint) | snapshot transfer + ≤ 2.4 s replay |
| Rejoin, cold (`Restored` start) | §11.3 table + ≤ 2.4 s replay |

---

## 17. Risks — every entry closed, accepted, or gated (2026-08-22 sweep)

Ordered by cost if discovered late. Rev 3 numbering preserved. Nothing here is an open question:
each risk is **closed** (ruled), **accepted** (the residual is named and owned), or **gated** (a
measurement with a pre-committed response).

**R1 — Hidden information. CLOSED** (2026-08-21, PIVOT §8): full-world visibility. Reopening is a
redesign trigger.

**R2 — `v_max` enforcement is data-level (§3.3). ACCEPTED.** The validator covers data; the
integrator's debug assert on per-tick displacement covers code paths in testing; in release an
overrun can only produce a *late* input (a missed causal deadline), which the quorum fold handles
as a missing frame — degraded, never divergent. That bound is why Medium confidence is enough.

**R3 — Closure-scoped restore, both halves (§3.7, §7.3). GATED — the highest-risk unknown.**
(a) *cost:* scatter restore over global SoA pools; (b) *scope:* closure growth under dense load.
Gate 4 / T-A-01 measures both; S-14 measures (b) early on Hovel. **Pre-committed response:** fail
either half → delay-only lockstep, no speculation (§18.2), which this design already supports
with §7.4 + §7.5 carrying the feel. No third option exists.

**R5 — Gap re-sequencing under partial views (§9.5). CLOSED by construction, verified by S-08.**
The log-completeness constraint makes a behind-successor's claim unacknowledgeable.

**R6 — Eviction idempotency under re-sequencing (§10.2). CLOSED — the rule:** every sequenced
one-shot (eviction, leave, custody handoff, construction commit) carries a stable id
`(originating_slot, seq)` assigned at creation, never at sequencing. Re-sequencing changes the
event's *tick*, not its id. Because re-sequencing always follows a rollback to the last confirmed
tick, any earlier application of the event is undone by the rollback itself and the event is
applied exactly once in the new ordering; the id additionally makes a duplicate delivery (same
id, same or later tick) a no-op. Idempotency therefore follows from (rollback-then-reapply) +
(id-keyed dedup), and it is tested by S-06/S-07 run together.

**R10 — Quorum-loss termination vs partition. CLOSED — the argument:** let `L` be the sequenced-
live set when a partition splits it into `A` and `B`. An eviction is sequenced only with quorum of
the *pre-eviction* live set, i.e. > |L|/2 acknowledgements. A side with |A| ≤ |L|/2 cannot gather
them from its own members, so it can never sequence an eviction and its denominator never
shrinks; it reaches `QUORUM_LOSS_TICKS` and terminates (§8.3). A side with |B| > |L|/2 holds quorum
of `L`, evicts one `A` member, and now holds quorum of `L \ {a}` (it lost no members), so it can
continue inductively until `A` is fully evicted. Exactly half on each side: neither has a strict
majority, both terminate, both write identical checkpoints (§10.8). Sequential departures *before*
the split each shrank `L` under quorum of the then-live set, so the induction base holds. Hence no
sequence of evictions lets a minority bootstrap itself into legitimacy. Verified by S-09 at N=8.

**R11 — Inter-session world forking (`Persistent`). CLOSED — the product rule:** the chain whose
latest entry carries a *signed, acked* custody handoff is canonical; a forced-takeover chain is
presented as a **new world** named `<world> (fork of <date>)` with the fork point shown, never as
the same world. Two forks are never merged; the UI offers to keep either or both. The netcode's
obligation (make the fork unambiguous and locatable) is met by §11.5; this rule is the product
half, ruled here so no game re-derives it.

**R12 — CSPRNG. CLOSED** — entropy behind the platform seam (`PLATFORM.md`), Monocypher
Ed25519/BLAKE2b, symbol gate proves unreachability. `Persistent` is not blocked.

**R13 — Coordinator capability drift (§16.1). ACCEPTED.** Capability-ordered seating at the
lobby; degradation over a long session is ended by end-and-reseat (free under `Persistent`).
Reopening condition: measured sessions routinely on an under-capacity coordinator → one single
gate (upstream headroom), never a score.

**R16 — UB is the residual silent-desync class. ACCEPTED and instrumented.** *(new in tidelock)* Fixed point removes FP as a
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
                        G-06: run twice + cross-leg on the CI target matrix, identical hash traces
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

**Hardware:** PC and PC 2 (both x86-64 Windows), plus the Steam Deck (x86-64 Linux) when it
joins the bench; until then the third peer is a headless process (§19.10). The Pi 4 left the
program (ruled 2026-08-25) — cross-ISA on silicon is the CI arm64 legs' job now.

| Proves | Does not prove |
|---|---|
| Cross-ISA determinism of the *new stack* under a colony-shaped workload, on silicon | That Alloy is deterministic — Alloy does not exist yet |
| Consensus, succession, failover, eviction, rejoin under real packet loss | Real-world NAT (one LAN; §5.5 untested) |
| `Persistent` end-to-end: leave, solo, resume, custody, fork detection | Absolute performance of the real sim |
| Closure-explosion dynamics (R3b) with a region analogue | Closure *restore cost* in Alloy's SoA pools (R3a) — Gate 4 |
| INV-7 across 1/2/4/8 workers on genuinely different core counts | 8-peer behaviour until more machines exist (§19.10) |
| Whether min-spec hardware holds a lockstep budget (the Deck, once benched) | Console/handheld certification |

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
- Per-arena rapidhash over used extents computed every tick at `LAST` (`DETERMINISM.md` §8 R-1),
  exchanged on the mesh every `CHECKSUM_INTERVAL_TICKS`.
- `WIRE_STRUCT` on every wire, checkpoint, and chain struct.
- **Gameplay mapping runs in a Luau sim VM** with the stock `math` library removed, so the
  bytecode-in-fingerprint mechanism (§15.1) is exercised before Alloy depends on it.

**Ballast.** Hovel's real arena set is ~80 KB — too small to test §11.3 honestly. `BALLAST_ARENA_BYTES`
is a deterministically filled registered arena, snapshotted and hashed, never read by the sim.
Set to 1 / 4 / 16 MB to measure session-start and rejoin against §11.3's table.

### 19.4 Hardware roles and sizing

| Machine | ISA / OS | Role | Why |
|---|---|---|---|
| PC | x86-64 / Windows | Peer + usual coordinator + build host | Fastest; WinSock + Windows timer path under ENet |
| PC 2 | x86-64 / Windows | Peer | Second physical box on a real LAN; already proved cross-machine bit-identity (Gate 0, 2026-08-25) |
| Steam Deck | x86-64 / Linux | Peer, **binding constraint** once it joins | Different OS/libc — isolates OS effects; the slowest bench machine anchors min-spec |

**Sizing rule: dial the load so the slowest peer on the bench hits ~4 ms/tick, not the fastest.**
Hovel is a **dial**: `SIM_LOAD` scales grid, pawns, buildings, and field iterations together.

```
  For each machine: sweep SIM_LOAD, record p50/p95/p99 tick time.
  Report: load at which each machine hits 4 ms, 8 ms, 16.7 ms.
  Derive: heterogeneity ratio across the bench (PC : PC 2 : Deck).
```

That ratio tells the real engine what fraction of the desktop budget the weakest peer carries.
There is no measured anchor from the Ore soak — its workload ran a different stack; start the
sweep from G-05's PC figure instead.

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
| **S-05** | Peer C's cable pulled 10 s, restored before evict | SUSPECT, phantom via Markov predictor | Phantom inputs identical on the remaining peers; no divergence on resume |
| **S-06** | Peer C pulled 40 s (> `SUSPECT_TO_EVICT_TICKS`) | Sequenced eviction; R6 idempotency | All peers drop the avatar on the same tick; rejoin appends to succession |
| **S-07** | PC (coordinator) killed mid-session | §9.5 failover, gap re-sequencing | Deck takes over within 1 tick + horizon rollback; no peer accepts a tick it had not confirmed |
| **S-08** | Stall one peer's confirmation, then kill the coordinator | **Behind-successor** — the R5 hole | The stalled peer's epoch claim is *refused*; a caught-up peer succeeds |
| **S-09** | Kill 2 of 3 simultaneously | §8.3 quorum-loss termination; R10 | Survivor writes a checkpoint and stops; checkpoint bit-identical to the last confirmed |
| **S-10** | Peers B and C leave; PC solo 30 min; both rejoin | `live = 1`; §10.4 crossover | `QUORUM(1) = 1` with no special-casing; rejoin **must** use snapshot; replay-only measured as wrong |
| **S-11** | Full session end; all machines rebooted; resumed next day | `Persistent` + `Restored`, §11.3 | `tick0_state_hash` matches on all three; chain head agrees; play continues |
| **S-12** | Custody PC → peer B; then force-takeover from peer C against a held baton | §11.6, R11 | Handoff acked, signed, chained; forced takeover yields a **detected fork** with the correct point |
| **S-13** | Impairment sweep: latency 20/60/150 ms, jitter 0/10/40 ms, loss 0/1/5% | §7.4, `REDUNDANCY_TICKS`, degradation shape | Adaptive delay isolates the impaired peer; others' rollback depth unchanged |
| **S-14** | Scripted mass-merge: one bridging tile merges 6 regions, under rollback | **R3(b) closure explosion** | Closure size logged per rollback; the distribution is the deliverable |
| **S-15** | 10 h continuous, three machines, mixed activity (= Milestone E) | Soak; the named successor to the 43 M-tick run | Zero divergence; no leak; checkpoint cadence stable |

### 19.7 Impairment shim

**Do not use `tc netem`.** Needs root, absent on Windows, not reproducible.

Impairment lives **in the transport layer, outside the determinism boundary**, as a debug shim
between the `src/net/` protocol code and the ENet wrapper:

```cpp
struct Impair { u16 delay_ms; u16 jitter_ms; u8 drop_pct; u8 reorder_pct; u8 _pad0[2]; };  // 8 B, explicit pad (§20.3(k))
struct ImpairmentShim {
    u64    seed;                   // its own PRNG stream; NEVER the sim's keyed RNG
    Impair per_peer[MAX_PEERS];    // asymmetric impairment
};
```

1. **Reproducible.** Same seed, same drops, same run.
2. **Cross-platform.** Identical on every bench machine and CI target leg.
3. **Legally nondeterministic.** Transport side of INV-2; own PRNG stream; **compiled out of
   release builds** and `static_assert`ed absent there.

Per-peer asymmetry matters: the interesting case is one bad peer among good ones. (Receive-side
placement, PRNG stream and the delay queue: §20.3(k).)

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
2. **Heterogeneity ratio** across the bench at equal `SIM_LOAD`.
3. **Rollback frequency and depth** vs measured `p`, against §7.5's model.
4. **Coordinator upstream, measured**, against 1.40 Mbit/s.
5. **Rejoin and session-start wall time** at 1 / 4 / 16 MB ballast, against §11.3.

### 19.9 Pass/fail thresholds (frozen before results exist)

| Metric | Pass | Investigate | Fail |
|---|---|---|---|
| Hash divergence | 0 | — | ≥ 1 unexplained |
| Closure size, p95 (S-14) | < 5% of world | 5–20% | > 20% sustained |
| Rollback tick cost, p95 | < 6 ms | 6–10 ms | > 10 ms |
| Slowest-peer tick time at target load, p99 | < 8 ms | 8–14 ms | > 16.7 ms |
| Coordinator upstream | within 20% of 1.40 Mbit/s | 20–50% over | > 2× |
| Rejoin, 4 MB ballast | < 10 s | 10–30 s | > 60 s |
| Archive size, 30 min, 3 peers | < 80 KB | 80–150 KB | > 300 KB |

### 19.10 Scaling to 8

Extra physical peers are whatever x86-64 boxes are available (old laptops, mini-PCs — the Pi
fleet option left with the Pi, 2026-08-25). Two free steps first:

- **Headless peers on one machine** — multiple Hovel processes over loopback. Exercises
  `QUORUM(live)` arithmetic, succession, shadow selection, eviction at N = 8 *logically*.
- **Mixed: 3 real + 5 headless.** Real heterogeneity where it matters; the coordinator's upstream
  measurement stays honest if the shim applies per-peer impairment.

What 8 adds that 3 cannot: the `1−(1−p)^7` curve is only meaningful at 7 remotes; `QUORUM` 8→5
transitions; the simultaneous-loss deadlock (R10) in its real shape.

**The nightly four-leg battletest (added 2026-08-25; lands in W5 with net-p3..p8):** the *same*
8-process loopback match — seeded scripted inputs via the Script producer, the §19.7
`ImpairmentShim` injecting deterministic loss/jitter/latency (the shim exists only under
`TL_DEV=1`, so this job runs the **dev tier**; timing is not graded here, so the tier costs
nothing) — real ENet over loopback, exercising WinSock and the POSIX socket paths — run
**independently on each of the four `CANON.md` target legs**, then a cross-leg diff of the
per-tick hash traces. The cross-leg identity holds only when the *sequenced input log* is
pinned, which the harness enforces rather than hopes: **fixed input delay for the run (no
adaptive-delay movement) and zero deadline-miss substitutions**, both asserted per run — a run
that trips either assert is *invalid and re-run*, not a red diff, because adaptive delay (§7.4)
and per-action-class substitution (§8.4) are legitimate log differences, not bugs. With the log
pinned, the four runs are the same match: an 8-player game proven bit-identical across
{Windows, Linux} × {x86-64, arm64} every night, with zero inter-machine packets required.
Boundary, stated: no live packets ever flow *between* ISAs here (wire-format asymmetry is the
job of the `TL_WIRE_STRUCT` static asserts and the encoder goldens/byte-stability tests), and
real NAT stays deferred exactly as §5.5 records. Spectating a match is free by construction —
every peer holds the full authoritative world, so "seeing" the other seven players is seven
extra local camera viewports, a render/editor playtest feature filed in `TODO.md`.

### 19.11 What would falsify the moat

Stated in advance, so the answer is not negotiated after the fact:

1. **Closure size explodes** (S-14 p95 > 20%). Speculation is dead; fall back to delay-only
   lockstep (what Factorio ships). Does not kill the project; the "fluid under contention" claim
   goes.
2. **Cross-ISA determinism fails on the new stack** (Milestone A or E, or S-02 under workers).
   By construction this can only be UB or a logic bug; it is the highest-information result the
   harness can produce and is hunted with sanitizers, not negotiated. A failure that survives the
   hunt would mean fixed-point-by-construction is not the guarantee this program rests on.
3. **Min-spec hardware (the Deck, once benched) cannot hold budget.** Not fatal; redraws
   minimum spec.
4. **Rejoin or session-start is unacceptable at realistic arena sizes** (S-10, S-11 at 16 MB).
   `Persistent` would need incremental snapshots — a significant addition to §11.3.
5. **R10 reproduces as a real deadlock** (S-09 at N = 8). Correctness rework of §8.3 before
   `Persistent` ships.

Any of 1, 3, 4 changes the design. Only 2 threatens the thesis.

---

## 20. Implementation specification

Binding for `src/net/`. Where §1–§19 left a detail open, this section decides it; the earlier
section carries a "(see §20.x)" pointer. Every tick value is `u64`; `InputFrame.tick` is the low
32 bits; every container carries `u64 base_tick` (`FRAME-LOOP.md` §1). `net` is outside the sim
boundary: it may read the wall clock and entropy, may not include sim internals, and every value
it feeds the sim is sequenced. `f32`/`f64` are not used in `net` either (`CANON.md` types table);
every measured quantity is an integer in microseconds, bytes, or Q8.

Constants decided here (App. B holds the rest): `NET_FORMAT_VERSION 1` · `SLOT_RING_TICKS 32`
(per-slot frame ring, power of two; ≥ `REDUNDANCY_TICKS + CONFIRMATION_HORIZON_TICKS + 6`,
`static_assert`ed) · `NET_HASH_RING 64` · `PHI_SUSPECT_Q8 2048` (φ = 8.0) · `SUSPECT_FLOOR_US
500000` · `SUSPICION_GOSSIP_TICKS 6` · `SUSPICION_TTL_US 1000000` · `DELAY_RAISE_MISSES 2` in
`DELAY_WINDOW_TICKS 60` · `DELAY_LOWER_CLEAN_TICKS 600` · `DELAY_COOLDOWN_TICKS 60` ·
`CLAIM_RETRY_US 500000` · `SNAPSHOT_CHUNK_BYTES 32768` · `CATCHUP_BATCH_TICKS 16` ·
`REPLAY_ONLY_MAX_TICKS 420` · `MAX_LOG_RECORDS_PER_PACKET 8` · `INPUT_PACER_BPS 262144` (reserved)
· `BULK_PACER_BPS 1048576` (default; subordinate) · `ErrCode` range `ERR_NET_* = 0x0400..0x04FF`.

### 20.1 File layout of `src/net/`

| File | Contents | Tier |
|---|---|---|
| `net/net.h` | Public API to `app/`: `net_create_session`, `net_join`, `net_leave`, `net_set_tick_fn`, `net_producer()` → `InputProducer`, `net_register_systems(World*)`, `net_phantom_allow`, `net_slot_delay`, `net_stats` | all |
| `net/net_internal.h` | `NetState` (one struct, lives in a **non-registered** arena), slot/peer tables, rings, the `LogRecord` store; included only by `net/*.cpp` | all |
| `net/producer.cpp` | `NetworkProducer`: `INPUT.md` §4 `InputProducer` fn-ptr; local capture via the core fold, READY/WAIT, speculation, correction detection, catch-up batches (§20.3(c)) | all |
| `net/transport.cpp` | **The only TU that includes ENet headers.** Host create/connect, channel map `INPUT=0 CONTROL=1 BULK=2`, per-channel token-bucket pacers, receive dispatch; the receive path calls the shim hook under `#if TL_DEV` (`static_assert(!TL_IMPAIR_ENABLED)` in `netcode`/`ship`) | all |
| `net/wire.h` | Every `TL_WIRE_STRUCT` of §20.2 + LE read/write pairs + varint/zigzag helpers. `MAX_PEERS`'s C++ home is `foundation/net_limits.h` (shared with `core/input.h`; module-DAG-forced — net depends on core, never the reverse, so the constant lives below both — `TODO.md` RR-24, resolved by `w3-loop-input`) | all |
| `net/encode.cpp` | `InputFrame` column encoder/decoder, §20.3(a) | all |
| `net/sequencer.cpp` | Coordinator role: per-slot rings, bitmap sequencing, `finalize(T)`, substitution, `LogRecord` assignment, downstream assembly; follower side: apply seq sections, confirmation advance | all |
| `net/peer.cpp` | Per-slot LIVE/SUSPECT/EVICTED/DEPARTED machine (sequenced half) + integer φ-accrual (measured half), suspicion gossip | all |
| `net/succession.cpp` | Epochs, claim/ack, log-completeness check, succession list ops, shadow selection | all |
| `net/session.cpp` | Lobby probing + seating, challenge/handshake, `SessionModel`/`Origin` validation, join/rejoin orchestration, quorum-loss termination, session state machine (§20.4) | all |
| `net/checkpoint.cpp` | Hot ring (2) + durable writer (temp→fsync→rename), checkpoint file image, chain file append/verify/fork-point | all |
| `net/archive.cpp` | Columnar + transition encoder/decoder, segment writer/reader, crc32, retention, `RecordedInput` adapter | all |
| `net/predictor.cpp` | Markov predictor (trained from the confirmed log only), phantom intents | all |
| `net/rollback.cpp` | Ring restore + resim driver; the only caller of the tick fn outside the loop | all |
| `net/systems.cpp` | `sys_net_receive` (`FIRST`) and `sys_net_send` (`LAST`) descriptors and bodies (§20.5) | all |
| `net/impair.cpp` | `ImpairmentShim` implementation (§20.3(k)); compiled only when `TL_DEV=1` | `dev`/`debug` only |
| `net/telemetry.cpp` | Per-tick CSV row buffer (§19.8) and event stream; flushed off the hot path | `dev`/`netcode` (stub in `ship`) |

`tl_net` links `tl_core`, `tl_foundation`, `tl_platform_*`, vendored ENet and Monocypher. It
never links `tl_sim`. Nothing in `net/` is in the symbol-audited lib set; `platform/entropy.h` is
reachable only from `net/session.cpp`, `net/checkpoint.cpp` (custody signing) and `app/`.

### 20.2 Wire structs

All are `TL_WIRE_STRUCT` (`CPP-SUBSET.md` §9 R-2): concrete, non-template, explicitly padded,
leading `u32 format_version` (`= NET_FORMAT_VERSION`), `static_assert` on `sizeof` and every
`offsetof`, written/read through the little-endian byte pair, never `memcpy`.

**Exemption — interior records (ruled 2026-08-26).** Three of the structs below carry no
`format_version` of their own: `CheckpointArenaEntry` (§20.2.8), `ChainRecord` (§20.2.8) and
`ArchiveStreamHeader` (§20.2.9). Each is a repeated element *inside* a container that has
already stated the version once in its own header, so they are versioned by their **container's**
`format_version`, never per record — a per-element copy would be redundant bytes on every row,
and their pinned sizes only close without one. `CheckpointArenaEntry` and `ChainRecord` carry the
same `sizeof`/`offsetof` pins and the same little-endian discipline; only the version field is
absent. `ArchiveStreamHeader` is the exception to the exception — since the framing ruling below
it is a **decoded form only**, its wire form being two canonical uvarints, so it has no on-wire
struct layout to pin at all. Readers refuse a
newer `format_version`, assert every `_padN` is zero, and reject any message whose declared
`payload_bytes` disagrees with the datagram length (`ERR_NET_MALFORMED`; the whole packet is
dropped, nothing partially applied). Offsets are natural-alignment offsets; every struct is
listed with the number the `static_assert` pins.

#### 20.2.1 `PacketHeader` (`INPUT` channel, every packet)

```cpp
struct PacketHeader {            // 40 B
    u32 format_version;          //  0
    u8  kind;                    //  4  PK_UPSTREAM=1 PK_DOWNSTREAM=2 PK_MIRROR=3 PK_KEEPALIVE=4
    u8  slot;                    //  5  sender slot
    u8  frame_count;             //  6  frames per column, 0..MAX_TICKS_PER_PACKET
    u8  slot_mask;               //  7  bit s set ⇒ a column for slot s follows (ascending s)
    u64 base_tick;               //  8  tick of frame index 0 of every column
    u64 last_confirmed_tick;     // 16  sender's confirmed frontier (§9.5)
    u64 hold_base_tick;          // 24  tick of hold[0]
    u32 epoch;                   // 32
    u8  hold_count;              // 36  0..MAX_TICKS_PER_PACKET
    u8  delay_ticks;             // 37  sender's current delay, informational copy (sequenced value is LR_DELAY)
    u16 payload_bytes;           // 38  bytes after the header
};
static_assert(sizeof(PacketHeader) == 40);
static_assert(offsetof(PacketHeader, base_tick) == 8 && offsetof(PacketHeader, hold_base_tick) == 24
           && offsetof(PacketHeader, epoch) == 32 && offsetof(PacketHeader, payload_bytes) == 38);
```

#### 20.2.2 `INPUT` packet body

```
  u8  hold[hold_count]                      hold[i] = "frames I hold for tick hold_base_tick+i", bit s = slot s
                                            (this is §12.2's ack_bitmap; MAX_PEERS = 8 ⇒ one byte)
  for s in ascending set bits of slot_mask:
      Column                                frame_count frames, self-contained delta chain, §20.3(a)
  if kind == PK_DOWNSTREAM:
      for i in 0..hold_count:               SeqSection for tick hold_base_tick+i
          u8  reported_mask                 0 ⇒ tick not finalized yet; else the slots whose bitmaps were sequenced
          u8  bitmaps[popcount(reported_mask)]   ascending slot order — THE fold inputs (§20.3(b))
          u8  record_count                  LogRecords effective at this tick (confirmed set)
          LogRecord[record_count]           §20.2.3, ascending (origin_slot, seq)
      u8  pending_count                     ≤ MAX_LOG_RECORDS_PER_PACKET; records announced ahead of their tick
      LogRecord[pending_count]
```

Upstream: `slot_mask = 1 << own_slot`, `frame_count = min(MAX_TICKS_PER_PACKET, frames held)`.
Downstream to slot `r`: `slot_mask = live_mask & ~(1 << r)`. `PK_MIRROR` is the upstream packet
re-addressed to the two shadows, byte-identical. `PK_KEEPALIVE` has `frame_count = 0` and carries
only `hold[]`.

**Column byte layout** (byte-aligned; no bit packing across fields). Frame 0 is encoded against
`ZERO_FRAME` (every `ActionState {0,0}`, pointer `(0,0)`, prior velocity `(0,0)`); frame `i` against
decoded frame `i-1`:

```
  changed    : uvarint(u32)    bit a set ⇔ actions[a] != prev.actions[a]  (1 B when 0)
  for a in ascending set bits of changed:
      u8 rec = (flags & 7) | (value_follows << 3)        bits 4..7 must be 0
               value_follows = (value != (i8)(flags & 1))  — 0 for every digital action by construction
      [i8 value]  present iff value_follows
  svarint(dvx), svarint(dvy)   where v_i = p_i - p_{i-1}, dv_i = v_i - v_{i-1}  (p_{-1} = v_{-1} = 0)
```

`uvarint` = LEB128, 7 bits per byte, `0x80` continuation, ≤ 5 bytes for a `u32`; `svarint(v)` =
`uvarint(zigzag32(v))`, `zigzag32(v) = (u32(v) << 1) ^ u32(v >> 31)`.

**The column format is CANONICAL: one frame set has exactly one byte encoding** (ruled
2026-08-26). This is load-bearing, not tidiness — §20.2.8 hashes the archive's bytes into
`ChainEntry.log_segment_hash`, so two peers that encode the same confirmed input must produce the
same bytes or the chain forks with no divergence behind it. Decoders therefore refuse, beyond a
truncated column: a **non-minimal `uvarint`** (a multi-byte varint whose final byte is 0); a
**value byte carrying the value the flags already imply** (§20.2.2 states `value_follows` as a
biconditional); and a **`changed` bit whose decoded `ActionState` equals the previous frame's**
(§20.2.2 states that rule as `⇔`). Every one only tightens — no stream a conforming encoder
produces is refused. Decoder arithmetic is
`wrap_add` on `i32`. Steady state: `1 + 1 + 1 = 3 B`; an idle peer's column of 9 frames is 27 B;
frame 0's absolute pointer costs ≤ 10 B per column per packet. A decoded frame's `tick` field is
set to `u32(base_tick + i)` by the decoder, never transmitted.

#### 20.2.3 `LogRecord` — every sequenced one-shot (R6 stable id = `(origin_slot, seq)`)

**Per origin, `seq` ascends strictly in segment record order (ruled 2026-08-26).** The
sequencer's frontier only grows, so per `origin_slot` a later `seq` can never carry an earlier
`effective_tick` — the rule states what the protocol already produces. It subsumes R6's global
`(origin_slot, seq)` uniqueness (a repeat cannot ascend) and turns the archive decoder's
duplicate scan from O(n²) into eight counters; encoder and decoder both enforce it.

```cpp
struct LogRecord {               // 24 B
    u32 format_version;          //  0
    u8  kind;                    //  4  LR_JOIN=1 LR_LEAVE=2 LR_SUSPECT=3 LR_RESUME=4 LR_EVICT=5 LR_DELAY=6 LR_EPOCH=7 LR_CUSTODY=8 LR_END=9
    u8  slot;                    //  5  subject slot
    u8  origin_slot;             //  6  creator (coordinator for SUSPECT/RESUME/EVICT/DELAY/EPOCH; the peer itself for LEAVE/CUSTODY)
    u8  _pad0;                   //  7
    u32 seq;                     //  8  creator-local counter, assigned at creation, never at sequencing
    u32 payload;                 // 12  LR_DELAY: new delay · LR_EPOCH: epoch · LR_CUSTODY: handoff_seq · LR_SUSPECT: suspicion count · LR_END: reason
    u64 effective_tick;          // 16
};
static_assert(sizeof(LogRecord) == 24 && offsetof(LogRecord, seq) == 8 && offsetof(LogRecord, effective_tick) == 16);
```

Effective tick rule: the coordinator assigns `effective_tick = max(requested, frontier + 1)` where
`frontier` is the highest tick it has sent in any downstream column; a record is announced in
`pending` from creation until its tick is finalized, then travels in that tick's `SeqSection`.
`LR_EVICT.effective_tick` **must** equal `LR_SUSPECT.effective_tick + SUSPECT_TO_EVICT_TICKS` for the
same slot; a follower that receives any other value treats the coordinator as faulty (raises local
suspicion of it) and ignores the record. Duplicate ids are no-ops (R6).

#### 20.2.4 `CONTROL` channel (mesh, unreliable)

```cpp
struct ControlHeader {           // 24 B
    u32 format_version;          //  0
    u8  kind;                    //  4  CK_SUSPICION=1 CK_EPOCH_CLAIM=2 CK_EPOCH_ACK=3 CK_HASH_DIGEST=4
                                 //     CK_MEASUREMENT=5 CK_CUSTODY=6 CK_LEAVE=7 CK_LOBBY_PROBE=8
    u8  slot;                    //  5  sender
    u16 payload_bytes;           //  6
    u64 tick;                    //  8  sender's world.tick at send (measured, informational)
    u32 epoch;                   // 16
    u32 _pad0;                   // 20
};
struct Suspicion {               // 16 B   gossip; idempotent; re-sent every SUSPICION_GOSSIP_TICKS while nonzero
    u32 format_version;          //  0
    u8  suspect_mask;            //  4  bit s ⇒ sender locally suspects slot s (§20.3(e))
    u8  _pad0[3];                //  5
    u32 seq;                     //  8  sender-local; receivers keep the highest
    u32 _pad1;                   // 12
};
struct EpochClaim {              // 24 B
    u32 format_version;          //  0
    u32 epoch;                   //  4  claimed (current + 1)
    u64 last_confirmed_tick;     //  8  claimant's, for the log-completeness check
    u32 claim_seq;               // 16
    u8  candidate_slot;          // 20
    u8  _pad0[3];                // 21
};
struct EpochAck {                // 24 B
    u32 format_version;          //  0
    u32 epoch;                   //  4
    u64 my_last_confirmed_tick;  //  8
    u32 claim_seq;               // 16
    u8  granted;                 // 20  0/1
    u8  refuse_reason;           // 21  0 none · 1 behind (claimant < mine) · 2 not eligible · 3 stale epoch · 4 coordinator not suspected
    u8  _pad0[2];                // 22
};
struct HashDigest {              // 24 B + 8·arena_count
    u32 format_version;          //  0
    u32 arena_count;             //  4  0 ⇒ fold only; else per-arena vector follows, registry order
    u64 tick;                    //  8  a CONFIRMED tick, multiple of CHECKSUM_INTERVAL_TICKS (or a tail end, §20.3(h))
    u64 fold;                    // 16  world hash (DETERMINISM.md §4)
    // u64 arenas[arena_count] follows
};
struct MeasurementVector {       // 48 B   lobby seating input; measured, never sequenced
    u32 format_version;          //  0
    u32 upstream_kbps;           //  4  measured upstream headroom
    u16 rtt_p50_ms[MAX_PEERS];   //  8
    u16 rtt_p95_ms[MAX_PEERS];   // 24
    u8  loss_pct[MAX_PEERS];     // 40
};
struct CustodyHandoff {          // 184 B   signed over bytes [0,120)
    u32 format_version;          //  0
    u32 handoff_seq;             //  4
    u8  from_pubkey[32];         //  8
    u8  to_pubkey[32];           // 40
    u8  chain_head[32];          // 72  chain[K] the handoff is made against
    u64 effective_tick;          // 104 requested
    u8  forced;                  // 112 1 ⇒ signed by `to` (forced takeover, §11.6)
    u8  _pad0[7];                // 113
    u8  signature[64];           // 120 Ed25519 by `from` (or `to` when forced)
};
struct Leave {                   // 16 B   to the coordinator; coordinator sequences LR_LEAVE
    u32 format_version;          //  0
    u32 leave_seq;               //  4
    u64 requested_effective_tick;//  8
};
struct LobbyProbe {              // 32 B
    u32 format_version;          //  0
    u32 probe_seq;               //  4
    u64 send_time_us;            //  8  originator's clock (echoed back verbatim)
    u64 echo_time_us;            // 16  responder's clock, 0 in the request
    u8  is_reply;                // 24
    u8  _pad0[7];                // 25
};
static_assert(sizeof(ControlHeader) == 24 && sizeof(Suspicion) == 16 && sizeof(EpochClaim) == 24
           && sizeof(EpochAck) == 24 && sizeof(HashDigest) == 24 && sizeof(MeasurementVector) == 48
           && sizeof(CustodyHandoff) == 184 && offsetof(CustodyHandoff, signature) == 120
           && sizeof(Leave) == 16 && sizeof(LobbyProbe) == 32);
```

#### 20.2.5 `BULK` channel (reliable + fragmented, point-to-point)

```cpp
struct BulkHeader {              // 32 B
    u32 format_version;          //  0
    u8  kind;                    //  4  BK_JOIN_CHALLENGE=1 BK_JOIN_REQUEST=2 BK_HANDSHAKE=3 BK_JOIN_REPLY=4
                                 //     BK_SNAPSHOT_CHUNK=5 BK_LOG_SEGMENT=6 BK_LOG_REQUEST=7 BK_ACK=8
    u8  slot;                    //  5  sender
    u16 _pad0;                   //  6
    u64 transfer_id;             //  8  one per snapshot/tail transfer; chunks of one transfer share it
    u32 chunk_index;             // 16
    u32 chunk_count;             // 20
    u32 offset;                  // 24  byte offset into the transfer image (SnapshotChunk: the checkpoint file image)
    u32 len;                     // 28  payload bytes after this header
};
struct LogRequest {              // 24 B   "send me the confirmed log for [from, to]"
    u32 format_version;          //  0
    u32 _pad0;                   //  4
    u64 from_tick;               //  8
    u64 to_tick;                 // 16
};
struct BulkAck {                 // 24 B   the §5.4 epilogue [final_tick][final_ref_hash][all_agree]
    u32 format_version;          //  0
    u8  all_agree;               //  4
    u8  _pad0[3];                //  5
    u64 final_tick;              //  8
    u64 final_ref_hash;          // 16
};
static_assert(sizeof(BulkHeader) == 32 && offsetof(BulkHeader, offset) == 24
           && sizeof(LogRequest) == 24 && sizeof(BulkAck) == 24);
```

`BK_SNAPSHOT_CHUNK` payload = `len ≤ SNAPSHOT_CHUNK_BYTES` bytes of the checkpoint file image
(§20.2.8) starting at `offset`; chunk 0 therefore begins with `CheckpointHeader`. `BK_LOG_SEGMENT`
payload = one `ArchiveSegment` (§20.2.9) followed by a `HashDigest` with `arena_count > 0` for the
segment's last tick. `BK_ACK` closes every transfer and every leave/custody/end phase.

#### 20.2.6 `Handshake` — §15.1, reused verbatim (120 B)

#### 20.2.7 Join: `JoinChallenge`, `JoinRequest`, `JoinReply`

```cpp
struct JoinChallenge {           // 40 B   server → joiner, first message on connect
    u32 format_version;          //  0
    u32 _pad0;                   //  4
    u8  nonce[32];               //  8  OS entropy (PLATFORM.md §5)
};
struct JoinRequest {             // 144 B  joiner → server, immediately followed by its Handshake
    u32 format_version;          //  0
    u32 requested_slot;          //  4  0xFFFFFFFF = any; under Persistent the seat owned by identity_pubkey
    u8  identity_pubkey[32];     //  8  Ed25519
    u8  held_chain_head[32];     // 40  zero if none
    u64 held_durable_tick;       // 72  tick of the newest durable checkpoint the joiner holds (0 if none)
    u8  signature[64];           // 80  Ed25519 over nonce ‖ Handshake bytes ‖ JoinRequest bytes [0,80)
};
struct JoinReply {               // 48 B
    u32 format_version;          //  0
    u32 epoch;                   //  4
    u64 join_effective_tick;     //  8  LR_JOIN.effective_tick (0 if refused)
    u64 checkpoint_tick;         // 16  tick of the checkpoint that will be streamed; == held_durable_tick ⇒ tail only
    u64 confirmed_tick;          // 24  server's confirmed frontier at reply
    u8  succession[MAX_PEERS];   // 32  current list, 0xFF-terminated
    u8  accepted;                // 40
    u8  slot;                    // 41
    u8  reason;                  // 42  0 ok · 1 build_id · 2 session_fingerprint · 3 model/origin · 4 fork (chain head) · 5 seat owned · 6 full · 7 bad signature
    u8  coordinator_slot;        // 43
    u8  live_mask;               // 44
    u8  _pad0[3];                // 45
};
static_assert(sizeof(JoinChallenge) == 40 && sizeof(JoinRequest) == 144 && offsetof(JoinRequest, signature) == 80
           && sizeof(JoinReply) == 48 && offsetof(JoinReply, succession) == 32);
```

`PeerSlots.slot_player_id[s]` (`INPUT.md` §8 R-2) = low 64 bits of `BLAKE2b-256(identity_pubkey)`.

#### 20.2.8 Checkpoint file image

```cpp
struct CheckpointHeader {        // 192 B
    u32 format_version;          //  0
    u32 session_model;           //  4  SessionModel
    u32 origin;                  //  8  Origin
    u32 arena_count;             // 12
    u8  build_id[32];            // 16  BUILD.md §5
    u8  session_fingerprint[32]; // 48  BUILD.md §5
    u64 tick;                    // 80  a confirmed tick
    u64 seed;                    // 88  world seed
    u64 state_hash;              // 96  world fold at `tick`
    u8  custody_pubkey[32];      // 104 current holder (zero under Match)
    u8  chain_entry[32];         // 136 chain[K] this checkpoint produced (zero for hot tier)
    u32 chain_index;             // 168 K (0 for hot tier)
    u32 tier;                    // 172 0 hot · 1 durable
    u64 payload_bytes;           // 176 bytes after the header (entries + arenas)
    u32 payload_crc32;           // 184 crc32 over the payload
    u32 header_crc32;            // 188 crc32 over bytes [0,188)
};
struct CheckpointArenaEntry {    // 16 B, one per registered SNAPSHOT arena, registry order
    u64 id;                      //  0  NameHash
    u64 used;                    //  8  bytes of [base, used)
};
static_assert(sizeof(CheckpointHeader) == 192 && offsetof(CheckpointHeader, tick) == 80
           && offsetof(CheckpointHeader, custody_pubkey) == 104 && offsetof(CheckpointHeader, chain_entry) == 136
           && offsetof(CheckpointHeader, header_crc32) == 188 && sizeof(CheckpointArenaEntry) == 16);
// Image = CheckpointHeader + CheckpointArenaEntry[arena_count] + arena bytes in the same order, packed.
```

The image is produced from one snapshot-ring slot (`registry_snapshot` layout: per-arena used
extents) and is the exact byte stream both written to disk and streamed in `SnapshotChunk`s. It
is the within-build snapshot path of `ASSETS-AND-DATA.md` §5; across builds the reflection-encoded
save is the migration unit (§15.3) and is not this format.

**Chain:**

```cpp
struct ChainGenesis {            // 56 B   chain[0] = BLAKE2b-256(le_bytes(ChainGenesis))
    u32 format_version;          //  0
    u32 _pad0;                   //  4
    u64 tick0_state_hash;        //  8
    u64 world_seed;              // 16
    u8  creation_nonce[32];      // 24  OS entropy at world creation
};
struct ChainEntry {              // 152 B  chain[K] = BLAKE2b-256(le_bytes(ChainEntry)), K ≥ 1
    u32 format_version;          //  0
    u32 chain_index;             //  4  K
    u8  prev[32];                //  8  chain[K-1]
    u8  log_segment_hash[32];    // 40  BLAKE2b-256 over the ArchiveSegments covering (tick[K-1], tick[K]]
    u64 state_hash;              // 72  world fold at tick[K]
    u64 tick;                    // 80  tick[K]
    u8  session_fingerprint[32]; // 88
    u8  custody_pubkey[32];      // 120
};
struct ChainRecord {             // 184 B  one line of the chain file
    ChainEntry entry;            //  0
    u8  hash[32];                // 152 chain[K]
};
static_assert(sizeof(ChainGenesis) == 56 && sizeof(ChainEntry) == 152 && offsetof(ChainEntry, custody_pubkey) == 120
           && sizeof(ChainRecord) == 184 && offsetof(ChainRecord, hash) == 152);
// chain file = ChainGenesis + ChainRecord[K]; verify: recompute every hash, each prev == previous hash.
```

Paths (under `pref_path`): `worlds/<chain0_hex16>/chain.tlc`, `worlds/<…>/ckpt/<tick:020>.tlck`,
`worlds/<…>/hot/{0,1}.tlck`, `worlds/<…>/log/<base_tick:020>.tla`. Under `Match`, the same tree
under `matches/<session_nonce_hex16>/` with only `hot/` and `log/`.

#### 20.2.9 Archive segment

An archive FILE is one `ArchiveFileHeader` followed by its segments. The build and session
identity is stated once, in that header; each segment names its file with a 4-byte `file_id`.
(Ruled 2026-08-26: repeating `build_id` + `session_fingerprint` in all 360 segments of a
30-minute session cost 23 KB of identical bytes, and the fixed 8-byte stream header another
54 KB — together 60 KB of the 124 KB an early implementation measured, against §20.8's 80 KB
criterion. Shrinking both took the same session to 64 KB.)

Two format bounds, both ruled 2026-08-26 and enforced by encoder AND decoder: **a segment's
`tick_count` is at most `CHECKPOINT_HOT_TICKS`** (segments close on the hot-checkpoint cadence
anyway; the cap also bounds the decoder's frame buffer and its cost — `BK_LOG_SEGMENT` makes
`tick_count` an untrusted peer's choice, and unbounded it bought a 61-second decode at 40,000
ticks), and **at most `MAX_LOG_RECORDS_PER_PACKET` log records per `effective_tick`** — the
per-tick bound the store already applied is the format's own rule now, so store, encoder and
decoder agree (the aggregate `≤ MAX_LOG_RECORDS_PER_PACKET × tick_count` remains as the
pre-parse header check it always was).

```cpp
struct ArchiveFileHeader {       // 72 B, one per archive file
    u32 format_version;          //  0
    u32 file_id;                 //  4  every segment in this file carries it
    u8  build_id[32];            //  8
    u8  session_fingerprint[32]; // 40
};
struct ArchiveSegmentHeader {    // 56 B
    u32 format_version;          //  0
    u32 max_actions;             //  4  MAX_ACTIONS at write
    u64 base_tick;               //  8
    u32 tick_count;              // 16
    u8  slot_mask;               // 20  slots sequenced-live at any tick of the segment
    u8  _pad0[3];                // 21
    u32 record_count;            // 24  total transition records over all streams
    u32 log_record_count;        // 28
    u32 file_id;                 // 32  the ArchiveFileHeader this segment belongs to
    u32 payload_bytes;           // 36
    u32 payload_crc32;           // 40
    u32 segment_seq;             // 44  monotonic per world/session
    u32 header_crc32;            // 48  over bytes [0,48)
    u8  _pad1[4];                // 52  the struct aligns to 8; outside the crc'd span
};
static_assert(sizeof(ArchiveFileHeader) == 72 && offsetof(ArchiveFileHeader, build_id) == 8);
static_assert(sizeof(ArchiveSegmentHeader) == 56 && offsetof(ArchiveSegmentHeader, file_id) == 32
           && offsetof(ArchiveSegmentHeader, header_crc32) == 48);
```

**Reader obligation:** a segment's `file_id` must equal the `file_id` of the `ArchiveFileHeader`
it is read against; a segment carrying another file's id must be refused, because the identity it
would otherwise be read under is not its own. Nothing inside the segment decoder can check this —
it never sees the file header — so it is the reader's, and `net/wire.h`'s
`archive_check_segment_file` is its name.

A stream header is **two canonical uvarints**, not a fixed struct:

```
  uvarint record_count
  uvarint key            key = slot * 35 + channel
                         channel: 0..31 action · 32 pointer_x · 33 pointer_y · 34 flag escape
```

35 channels exist, so the key is slot-major over 35 and ascending key means ascending
`(slot, channel)` — the order a segment's streams are already required to be in. (A single
packed byte cannot carry it: slot needs 3 bits and channel 6.) A stream with
`record_count == 0` is never written and is refused on read: **empty streams are OMITTED**, so a
segment carries only the streams that have records, and the stream region is read to where the
`LogRecord` array begins, which `payload_bytes` and `log_record_count` locate.

```
// File    = ArchiveFileHeader + ArchiveSegment[]
// Segment = ArchiveSegmentHeader
//         + for each NON-EMPTY stream, ascending by (slot, channel): stream header + records
//         + LogRecord[log_record_count] sorted by (effective_tick, origin_slot, seq)
```

Records are `(uvarint delta_tick, value)`; `delta_tick` counts from `base_tick` for the first
record and from the previous record otherwise. Channel values: actions `uvarint(u16(u8(value)) << 1
| (flags & 1))`; pointer channels `svarint`, first record mandatory at `delta_tick = 0` carrying
the **absolute position** at `base_tick`, later records carrying the new **velocity** (held constant
between records, `p += v` each tick); flag escape `uvarint(action << 3 | flags)` for a frame whose
`pressed/released` differ from the derived edges (`pressed = down && !down_prev`, `released = !down
&& down_prev`). Every stream is self-contained: a non-`ZERO` state at `base_tick` is emitted as a
`delta_tick = 0` record. The segment stores the **confirmed applied** frame of every live slot
(substituted and phantom frames literally, §10.6); a slot outside `slot_mask` decodes as `ZERO`.
Segments close every `CHECKPOINT_HOT_TICKS` ticks and at every `LR_*` boundary that ends the session.

**`RecordedInput` sharing (decided):** `INPUT.md` §4's recorder file is
`ArchiveSegment + HashTraceHeader + u64 fold[tick_count] [+ u64 arenas[tick_count][arena_count]]`
with `HashTraceHeader { u32 format_version; u32 arena_count; u64 base_tick; u32 tick_count; u32 _pad0; }`
(24 B). One codec serves the recorder, the replay producer, the rejoin tail and the desync package.

### 20.3 Algorithms

Each item states its ordering and which values are **sequenced** (deterministic, in the log) vs
**measured** (local, never enters the sim).

**(a) Column encode/decode and the redundancy window.** Sender keeps `own_ring: RingBuffer<InputFrame>`
(`SLOT_RING_TICKS`). `encode_column(out, frames[n])`: `prev = ZERO_FRAME; v_prev = (0,0); for i in
0..n: changed = Σ_a (frames[i].actions[a] != prev.actions[a]) << a; write uvarint(changed); for a
ascending in changed: write rec(+value); v = p_i - p_prev; write svarint(v.x - v_prev.x),
svarint(v.y - v_prev.y); prev = frames[i]; v_prev = v`. Decoder mirrors it; it rejects `rec & 0xF0
!= 0`, a `changed` bit ≥ `MAX_ACTIONS`, or a truncated column. The window is the last
`min(MAX_TICKS_PER_PACKET, held)` frames ending at the newest captured tick; `base_tick = newest -
frame_count + 1`. **Backoff (T-N-07):** if the assembled packet exceeds 1200 B, drop `frame_count`
by one and re-encode, floor 3; below the floor send anyway and count `oversize_packets`. Receiver:
for each decoded frame, `tick = base_tick + i`; discard if `tick ≤ slot.last_finalized` or already
held; else store in the slot ring. Lossless by construction (every operation is an integer
identity). Test: §20.6 T1.

**(b) Sequencer.** State per slot `s`: `ring[s]` (frames by tick), `held[s]` bitset over the ring,
`bitmaps[T][s]` (received hold bitmap for tick `T`, with `reported[T]` mask), `last_present_tick[s]`,
`last_present_frame[s]`. Ordering: packets are processed in arrival order (measured); everything
the coordinator derives from them is then broadcast, so the derived values are sequenced.

```
  on upstream packet from slot s (epoch == mine, else drop):
     for i in 0..frame_count: place frame (base_tick+i) into ring[s] if not held
     for i in 0..hold_count:  T = hold_base_tick+i; if T > confirmed and !reported[T].has(s):
                                 bitmaps[T][s] = hold[i]; reported[T] |= 1<<s        // sequenced by broadcast
     peer_note_arrival(s, now_us)                                                     // measured

  finalize_step():  T = confirmed + 1
     while popcount(reported[T] & live_mask(T)) >= QUORUM(live_mask(T)):
         present = 0
         for x in 0..MAX_PEERS: if popcount over r in reported[T] of (bitmaps[T][r] >> x & 1) >= QUORUM(live_mask(T)): present |= 1<<x
         for s in live_mask(T):
             if present has s: frame[T][s] = ring[s][T]; last_present_tick[s] = T; last_present_frame[s] = frame
             else              frame[T][s] = substitute(s, T)
         records[T] = every LogRecord with effective_tick == T (announced earlier, else none)
         confirmed = T; T += 1
```

`live_mask(T)` is a pure function of the confirmed records with `effective_tick ≤ T` (`LR_JOIN`
adds, `LR_LEAVE`/`LR_EVICT` remove; `LR_SUSPECT` does not change it). `QUORUM(m) = popcount(m)/2 + 1`.
Bitmaps arriving for `T` after `reported[T]` reached quorum are dropped — the sequenced set is
exactly the first-quorum set. The coordinator's own bitmap is sequenced like any other. The
finalize loop is run once per `sys_net_receive` and once per WAIT-path pump.

`substitute(s, T)` with `k = T - last_present_tick[s]` (k ≥ 1), `last = last_present_frame[s]`
(`ZERO_FRAME` if none), per action class from the action map:

```
  LATCHED: value = last.value; flags = last.flags & 1                            // hold, edges cleared
  AXIS:    d = SUB_DECAY_TICKS; v0 = i32(last.value)
           value = i8( k >= d ? 0 : (v0 * (d - k)) / d )  // i32 arithmetic, truncation toward zero; |v0| ≤ 127 so no overflow
           flags = value != 0 ? 1 : 0
  EDGE:    value = 0; flags = (k == 1 && (last.flags & 1)) ? 4 /*released*/ : 0
  pointer: p = last.p + k * last.v clamped to ±(1<<30)  where last.v = velocity of the last two present frames (0 if one)
  tick   = u32(T)
```

Followers apply `SeqSection`s in tick order; a follower advances `confirmed` to `T` only when it
holds every frame `present` marks for `T` (redundancy supplies them; a follower whose `confirmed`
falls more than `REDUNDANCY_TICKS` behind the coordinator's `last_confirmed_tick` sends a
`LogRequest` on `BULK`). Downstream assembly per recipient `r` at `LAST`: columns for every slot in
`live_mask & ~(1<<r)` over `[frontier - frame_count + 1, frontier]`, `hold[]` = the coordinator's own
hold bitmaps, `SeqSection`s for `[confirmed - hold_count + 1, confirmed]` (re-sent for the whole
window, so a lost packet costs nothing), `pending` = announced records not yet finalized.

**(c) `NetworkProducer.produce(ctx, tick, out, live_mask)`.** Let `C = confirmed`, `N = tick`.

```
  1. if state != RUNNING:  if SYNCING: run_catchup_batch(); return WAIT          // §20.3(h)
                           else return WAIT
  2. capture: fold the platform raw-event ring through the core Live fold → f; f.tick = u32(N + delay)
     apply the raise/lower rule of (d); push f into own_ring (ZERO frames for ticks < first capture)
  3. correction check: for t in (C_prev_seen, C]: if applied[t] != frame[t] (any live slot) → T = first such t;
        rollback_run(T, N) (§20.3(c)-ii); applied[t] = frame[t] for re-run ticks
  4. if N <= C:          speculative = false                                          // frames confirmed
     else if !speculation_enabled || N - C > CONFIRMATION_HORIZON_TICKS: pump(); keepalive(); return WAIT
     else                speculative = true
  5. for s in 0..MAX_PEERS: if !live_mask(N) has s: out[s] = ZERO_FRAME; continue
        out[s] = confirmed ? frame[N][s]
               : s == own ? own_ring[N]
               : held(ring[s][N]) ? ring[s][N]                                        // received, unconfirmed
               : predictor_predict(s, N)                                               // §20.3(j)
     applied[N] = out; *live_mask = live_mask(N)
  6. return READY
```

`speculation_enabled` is a `SIM` cvar (`net.speculation`, in `session_fingerprint`); off = delay-only
lockstep (R3's fallback): step 4 returns WAIT whenever `N > C`. WAIT never deadlocks: the WAIT path
pumps the transport, runs `finalize_step`, and re-sends the last upstream/downstream windows as
`PK_KEEPALIVE`/full packets at most once per 16,667 µs (measured clock).

(c)-ii **`rollback_run(T, N)`** (`rollback.cpp`): precondition `N - T ≤ CONFIRMATION_HORIZON_TICKS`
and `T ≥ 1`; ring slot `(T-1) mod CONFIRMATION_HORIZON_TICKS` holds the post-tick snapshot of `T-1`
(pushed in `LAST`; invariant from step 4: the slot of `C` is never overwritten while `N - C ≤
HORIZON`). `registry_restore(slot)`; `post_restore` barrier (`MEMORY.md` §5); `world.tick = T`;
`net.resim_depth = N - T`; `for t in T..N-1: frames = (t ≤ C) ? frame[t] : speculative set per step 5;
tick_fn(world, frames, live_mask(t))` — `tick_fn` is the loop's tick body (`run_phases_sim +
barrier_end_of_tick + tick++`), handed to `net_set_tick_fn` by `app/` at init; `net.resim_depth = 0`.
Whole-arena restore until T-A-01 lands; `rollback_restore(tick, closure)` is the seam where
closure-scoped restore replaces it with no caller change. Telemetry: `rollback_fired`,
`rollback_depth`. Re-entrancy rule: §20.5.

**(d) Adaptive delay.** Sequenced and a pure function of the log: a *miss* for slot `s` at tick `T`
is `present(T)` lacking `s` while `s ∈ live_mask(T)`. The coordinator, at `finalize(T)`: `misses[s]`
over the last `DELAY_WINDOW_TICKS` confirmed ticks; if `misses ≥ DELAY_RAISE_MISSES` and `delay[s] <
6` and `T - last_change[s] ≥ DELAY_COOLDOWN_TICKS` → create `LR_DELAY{slot s, payload delay+1}`; if
zero misses over the last `DELAY_LOWER_CLEAN_TICKS` and `delay[s] > 3` and cooldown elapsed →
`LR_DELAY{payload delay-1}`. Followers recompute the same rule from the confirmed log and raise local
suspicion of a coordinator whose records disagree with it. `delay[s]` (sequenced) is the value the
peer uses for captures whose target tick `≥ effective_tick`. Frame rule at the peer: raise `d→d+1`
at capture tick `t` leaves tick `t+d` without a capture → send a copy of frame `t+d-1` with edges
cleared as `t+d`; lower `d→d-1` at capture `t` collides with `t+d-1` → discard capture `t`. Initial
`delay = 3`; measured RTT plays no part (INV-2). Exported read-only via `net_slot_delay(slot)`.

**(e) Suspicion — integer φ-accrual.** Measured, per remote slot, from inter-arrival times of any
`INPUT`/`CONTROL` packet: `x = now - last_rx_us; mean += (x - mean) >> 4; dev += (|x - mean| - dev)
>> 4; last_rx_us = now` (init `mean = 16667`, `dev = 4000`). `phi_q8 = ((now - last_rx_us) - mean) *
256 / max(dev, 1000)`; local suspicion ⇔ `phi_q8 ≥ PHI_SUSPECT_Q8 && now - last_rx_us ≥
SUSPECT_FLOOR_US`; cleared on the next packet. Gossip: `Suspicion{suspect_mask}` to every peer every
`SUSPICION_GOSSIP_TICKS` while nonzero and immediately on change; a received message is valid for
`SUSPICION_TTL_US`. Sequenced half (coordinator, at `finalize_step`): for slot `s ≠ coordinator`, if
the valid messages (own included) from `≥ QUORUM(live_mask)` distinct slots set bit `s` and `s` is
LIVE → `LR_SUSPECT{s, payload = count}`; if `s` is SUSPECT and a frame from `s` for a tick `>
suspect.effective_tick` arrives → `LR_RESUME{s}`; if `s` is SUSPECT and `frontier + 1 ≥
suspect.effective_tick + SUSPECT_TO_EVICT_TICKS` → `LR_EVICT{s, effective_tick =
suspect.effective_tick + SUSPECT_TO_EVICT_TICKS}`. The counter is `effective_tick` arithmetic, never
a clock. Suspicion of the coordinator itself feeds (f) only.

**(f) Epoch claim/ack.** Candidate `c` (per (g), eligible) with local quorum-suspicion of the
coordinator sends `EpochClaim{epoch+1, last_confirmed_tick, claim_seq}` to every peer. A receiver
grants iff: `claim.epoch == my_epoch + 1` (else 3), `c` is the next eligible entry in my list given
my local suspect set (else 2), I locally suspect the coordinator (else 4), and **`claim.last_confirmed_tick
≥ my confirmed`** (else 1 — the §9.5 log-completeness constraint). `granted` acks from `≥
QUORUM(live_mask)` slots (self included) → `c` sets `epoch += 1`, role COORDINATOR, sequences
`LR_EPOCH{slot c, payload epoch, effective_tick = confirmed + 1}` as its first record, re-sequences
from `confirmed + 1` using its rings (mirrored as a shadow plus the first packet each peer sends it).
No quorum within `CLAIM_RETRY_US` → `claim_seq += 1`, retry; a refusal with reason 1 ends the
candidacy (a caught-up peer will claim). Any peer that sees `epoch' > epoch` on a downstream packet
from a sequenced-live slot adopts it, redirects upstream, and rolls back to its `confirmed` (a
correction at `confirmed + 1`, depth ≤ horizon). An old coordinator seeing a higher epoch steps down
to FOLLOWER (§8.6). Measured: suspicion, timing. Sequenced: the epoch, via `LR_EPOCH`.

**(g) Succession list.** `u8 list[MAX_PEERS]; u8 count` in the confirmed log state. `init`: seated
slot order. `remove(s)` on `LR_LEAVE`/`LR_EVICT` at `effective_tick`. `append(s)` on `LR_JOIN` (a
rejoining ex-member is removed at its leave/evict and appended at its join). `coordinator_index` =
position of the current coordinator. Eligibility for a claim is **cyclic from
`coordinator_index + 1`**: entry `list[(coordinator_index + j) mod count]`, `j ≥ 1`, is eligible iff
every entry with smaller `j` is in the acker's local suspect set. Shadows = the first two eligible
entries by the same walk with an empty suspect set (positions `j = 1, 2`). All pure functions of
the log; the suspect set only affects acks.

**(h) Rejoin.** Joiner connects to any live peer (the *server*) over `BULK`: `JoinChallenge` ←;
`JoinRequest` + `Handshake` →; server verifies signature, `build_id`, `session_fingerprint`,
model/origin, seat ownership (Persistent: pubkey ↔ slot from the last session's seat table), chain
head (a mismatch with a common prefix is reason 4 with the fork point in the event log). Checkpoint
choice: `H` = newest hot checkpoint (always confirmed by construction); if
`joiner.held_chain_head == chain record hash at joiner.held_durable_tick` on the server and
`confirmed - held_durable_tick ≤ REPLAY_ONLY_MAX_TICKS` → tail only from `held_durable_tick`;
else stream `H`'s image in `SnapshotChunk`s. Then stream `LogSegment`s for `(checkpoint_tick,
confirmed]`, each with its trailing `HashDigest`; the server forwards the `JoinReply` after asking
the coordinator to sequence `LR_JOIN{slot, effective_tick}`. Joiner: `registry_restore` from the image
(fail-loud on fingerprint), session state SYNCING; `run_catchup_batch()` inside `produce` runs ≤
`CATCHUP_BATCH_TICKS` ticks per call through `tick_fn` with the decoded tail frames (the rollback
driver's path, same re-entrancy rule), verifying at every segment end that its per-arena vector
equals the segment's `HashDigest` (mismatch → `ERR_NET_DESYNC_ON_REJOIN`, P0 telemetry package,
abort join). When `world.tick > confirmed` from live downstream, state RUNNING. A joiner's frames for
ticks `< join_effective_tick + delay` are absent → substituted by (b). `Restored` session start is
this procedure with every peer as joiner against the custody holder, followed by a `BulkAck` round
(`final_tick = tick0`, `final_ref_hash = tick0_state_hash`) before any `INPUT` flows (§11.3).

**(i) Checkpoint write and chain append.** Hot: at `finalize` of `T` with `T % CHECKPOINT_HOT_TICKS ==
0`, copy ring slot `T mod HORIZON` into `hot[(T / CHECKPOINT_HOT_TICKS) & 1]` (in-memory image,
§20.2.8) and `write_all` it to `hot/<i>.tlck` (best effort). Durable (`Persistent` only): at
`finalize` of `T` with `T % CHECKPOINT_DURABLE_TICKS == 0`, on `LR_LEAVE` of the local slot, on
quorum-loss termination, on `LR_END`: (1) close the open archive segment; `log_segment_hash` =
BLAKE2b-256 over the segment images `(tick[K-1], T]`; (2) build `ChainEntry{K, prev = chain[K-1],
log_segment_hash, state_hash = fold(T), T, session_fingerprint, custody}`; `chain[K] = BLAKE2b(entry)`;
(3) fill `CheckpointHeader` (tier 1, `chain_entry = chain[K]`), compute `payload_crc32` then
`header_crc32`; (4) `write_atomic(ckpt/<T>.tlck)` (`PLATFORM.md` §3: temp → fsync → rename); (5) only
after (4) returns OK: `write_atomic(chain.tlc)` with `ChainRecord[K]` appended (whole-file rewrite;
≤ 2.3 KB / 6 h); (6) retention: delete checkpoints older than the newest `DURABLE_KEEP` and segments
older than the oldest kept. A crash between (4) and (5) leaves a checkpoint without a chain record;
on load a checkpoint whose `chain_index` exceeds the chain's length is discarded (the previous one
is canonical). Readers verify both crc32s, the fingerprints, then the chain. Ordering: all of this
runs in `sys_net_send` after the hash read, off the sim's critical path only by being cheap
(Hovel measures it: §19.8 `checkpoint writes`).

**(j) Markov predictor.** State per slot, per action `a`, in the net arena (rebuilt from the log,
never hashed): `u16 n[32][2]` where context `ctx = down[t-1] | (changed[t-1] << 1) | (bucket << 2)`,
`changed[t-1] = (state[t-1] != state[t-2])`, `bucket` = log2 bucket of ticks since the last change of
`a` clamped to 0..7 (`{0},{1},{2-3},{4-7},{8-15},{16-31},{32-63},{64+}`), `n[ctx][next_down]`.
Training: at every confirmation of `T`, for every live slot, from the **confirmed applied** frame
(substituted frames included): `n[ctx][down[T]] += 1`; when either count reaches `0xFFFF`, halve
both. Prediction of digital `down[t]`: `n[ctx][1] > n[ctx][0] ? 1 : n[ctx][1] < n[ctx][0] ? 0 :
down[t-1]` (tie → no transition); `value = down`; edges derived. Analog (`AXIS`): `value[t] =
value[t-1]` (hold). Pointer: constant velocity from the last two confirmed frames. Multi-tick
prediction iterates from the last confirmed frame, updating the context with its own output.
**Phantom intents** (§10.2): the phantom frame for a SUSPECT slot = predicted frame with every action
outside the phantom mask zeroed, pointer held; mask = actions of class `AXIS` ∪ the set registered
by `net_phantom_allow(ActionId)` at init, stored as the `SIM` cvar `net.phantom_mask` (so it is in
`session_fingerprint`). The coordinator substitutes SUSPECT slots with the phantom frame instead of
(b)'s class rule (both are pure functions of the log; the phantom is the §10.2 choice) and stores it
literally (§10.6). Determinism test: §20.6 T9.

**(k) Impairment shim.** Receive side of `transport.cpp`, per source slot, between ENet and
dispatch: `r = splitmix64_next(&stream[slot])` (stream seeded `seed ^ (slot + 1) *
0x9E3779B97F4A7C15`); drop if `r % 100 < drop_pct`; else `deliver_at = now + delay_ms*1000 +
(jitter_ms ? (r >> 8) % (2*jitter_ms*1000) - jitter_ms*1000 : 0)`; if `reorder_pct` and `(r >> 32) %
100 < reorder_pct` swap `deliver_at` with the previously queued packet's; push into a fixed
`Array<QueuedPacket>` (cap 4096, `TL_FATAL` on overflow); dispatch every queued packet with
`deliver_at ≤ now` in `deliver_at` order (stable). Reproducible given the same packet sequence and
clock sequence — the test (§20.6 T10) drives both. `BULK` is impaired too (it is reliable; ENet
retransmits).

**(l) Quorum-loss termination.** Measured wall clock, decided locally and identically: if
`confirmed` has not advanced for `QUORUM_LOSS_TICKS × 16,667 µs` while `produce` is returning WAIT
(or, on the coordinator, while `popcount(reported[confirmed+1] & live_mask) < QUORUM` persists)
→ session state TERMINATING: write a durable checkpoint of `confirmed` via (i) (its ring slot is
intact by the (c) invariant) under `Persistent`, a hot image under `Match`; emit `LR_END` locally to
the archive with `effective_tick = confirmed + 1`; close the host; state ENDED with
`ERR_NET_QUORUM_LOST`. Every survivor holds the same confirmed log, so the checkpoints are
bit-identical (§8.3, R10).

### 20.4 State machines

**Peer (per slot, sequenced; the transition tick is the record's `effective_tick`):**

| State | Event (sequenced record) | Action | Next |
|---|---|---|---|
| — | `LR_JOIN` | add to `live_mask`; `append` to succession; predictor reset for slot | LIVE |
| LIVE | `LR_SUSPECT` | frames → phantom (§20.3(j)); start evict counter at `effective_tick` | SUSPECT |
| LIVE | `LR_LEAVE` | remove from `live_mask` and succession; `QUORUM` recomputes; `Persistent`: durable checkpoint if local | DEPARTED |
| SUSPECT | `LR_RESUME` | frames → received/substituted as normal | LIVE |
| SUSPECT | `LR_EVICT` (tick = suspect + 1800) | remove from `live_mask` and succession; avatar dropped by the game on this tick | EVICTED |
| SUSPECT | `LR_LEAVE` | as LIVE → DEPARTED (a late leave beats the counter) | DEPARTED |
| EVICTED / DEPARTED | `LR_JOIN` | as — → LIVE (slot appended, not reinserted) | LIVE |
| any | `LR_END` | session terminates | — |

Measured inputs (`phi`, gossip) never move this machine; they only cause the coordinator to
*create* records, and the coordinator's role to move below.

**Coordinator role (per peer, local view of a sequenced fact):**

| Role | Event | Action | Next |
|---|---|---|---|
| FOLLOWER | local quorum-suspicion of coordinator ∧ self eligible (§20.3(g)) | send `EpochClaim`; start `CLAIM_RETRY_US` | CANDIDATE |
| FOLLOWER | downstream with `epoch' > epoch` from a sequenced-live slot | adopt; redirect upstream; rollback to `confirmed` | FOLLOWER |
| FOLLOWER | self in shadow positions (§20.3(g)) | receive `PK_MIRROR`, fill rings, never finalize | SHADOW (a FOLLOWER sub-state) |
| CANDIDATE | `granted` acks ≥ `QUORUM` | `epoch += 1`; sequence `LR_EPOCH`; re-sequence from `confirmed + 1` | COORDINATOR |
| CANDIDATE | refusal reason 1, or `epoch' > claimed` observed | drop candidacy | FOLLOWER |
| CANDIDATE | retry timer | `claim_seq += 1`; resend | CANDIDATE |
| COORDINATOR | downstream observed with `epoch' > epoch` | step down; redirect | FOLLOWER |
| COORDINATOR | own `LR_LEAVE` reaches `effective_tick` | last downstream carries the record; successor claims with no gap | — (DEPARTED) |
| any | `LR_END` / quorum loss | terminate (§20.3(l)) | — |

**Session:**

| State | Event | Action | Next |
|---|---|---|---|
| IDLE | `net_create_session(model, origin)` | validate §11.2 table; open host; `Seeded`: compute `tick0_state_hash`; `Persistent`+`Restored`: load newest valid durable checkpoint + chain | LOBBY |
| IDLE | `net_join(addr)` | connect | JOINING |
| LOBBY | probes for `≥ 3 s` at `LOBBY_PROBE_HZ` from every peer | seat by p95 RTT + headroom (one sort); exchange `Handshake`s | HANDSHAKE |
| HANDSHAKE | all match | `Seeded`: tick 0 locally; `Restored`: custody holder streams to all (§20.3(h)); `BulkAck` round | RESTORING → RUNNING |
| HANDSHAKE | any mismatch | end with named `ErrCode` (`ERR_NET_BUILD_ID`, `_FINGERPRINT`, `_MODEL`, `_FORK`) | ENDED |
| JOINING | `JoinChallenge` | send `JoinRequest` + `Handshake` | JOINING |
| JOINING | `JoinReply.accepted` | receive image (if any) then tail | RESTORING |
| JOINING | `JoinReply` refused | end with reason code | ENDED |
| RESTORING | image complete, `registry_restore` OK | start catch-up | SYNCING |
| SYNCING | catch-up reaches live frontier, every segment hash verified | `produce` returns READY | RUNNING |
| SYNCING | segment hash mismatch | P0 package; abort | ENDED |
| RUNNING | `net_leave()` | send `Leave`; wait for `LR_LEAVE` effective; durable checkpoint (`Persistent`); `BulkAck` epilogue | ENDED |
| RUNNING | quorum loss (§20.3(l)) | checkpoint; `LR_END` | TERMINATING → ENDED |
| RUNNING | `LR_END` received | checkpoint; epilogue | ENDED |

### 20.5 Integration contract with the frame loop

Registration (`net_register_systems`, called by `app/` after the core registers its `LAST` hash
system): `SYS_NET_RECEIVE = { sys_net_receive, "net_receive"_id, PHASE_FIRST, reads {}, writes {},
before { "input_drain"_id } }` and `SYS_NET_SEND = { sys_net_send, "net_send"_id, PHASE_LAST, reads
{}, writes {}, after { "hash_checkpoint"_id } }`. `hash_checkpoint` is the core `LAST` system that
computes `world.hash_vec[MAX_ARENAS]` + `world.hash_fold` and pushes ring slot `tick mod HORIZON`.

`sys_net_receive` (`FIRST`), in order: (1) `transport_pump()` — `enet_host_service(host, 0)` until
empty, every event through the shim, dispatched by channel: `INPUT` → sequencer (§20.3(b)) or
follower apply; `CONTROL` → peer/succession/session handlers; `BULK` → session/checkpoint stream
handlers; (2) `finalize_step()` if coordinator; (3) peer φ update for every slot with no packet
this tick; (4) telemetry: `tick_start_us`. It touches **no sim state**: not `world.input`, not
`PeerSlots`, no registered arena. The only sim-visible effects of the netcode are the frames and
`live_mask` returned by `produce` (the loop stores them). When `net.resim_depth > 0` it returns at
step 0.

`sys_net_send` (`LAST`), in order: (1) copy `world.hash_vec`/`hash_fold` for `world.tick` into
`hash_ring[tick mod NET_HASH_RING]`; if `resim_depth > 0` return here; (2) if `tick` is confirmed
and `tick % CHECKSUM_INTERVAL_TICKS == 0`: queue `HashDigest{tick, fold}` to every peer on `CONTROL`;
compare incoming digests for `tick` against the ring, a disagreement where `≥ QUORUM` agree on a
different fold → `desync` event + P0 package (§14.4); (3) checkpoint cadence (§20.3(i)) for any tick
confirmed since the last call; (4) append confirmed ticks to the open archive segment; (5) predictor
training for confirmed ticks; (6) adaptive-delay rule (§20.3(d)) if coordinator; (7) assemble and
send: upstream `INPUT` to coordinator + `PK_MIRROR` to the two shadows; if coordinator, downstream
to every live slot; due `CONTROL` gossip (suspicion, measurement every 60 ticks); (8) `BULK` pacer
step (token bucket: `INPUT` bucket refilled first and never borrowed from; `BULK` sends only from its
own bucket); (9) telemetry row flush to the off-thread buffer. Reads sim state only through
`world.hash_vec`/`hash_fold`/`world.tick`; writes none.

**Rollback driver re-entrancy rule.** `rollback_run` and `run_catchup_batch` call `tick_fn`, which
runs every phase including both net systems. They are therefore called **only from inside
`produce()`**, which the loop invokes between ticks with no phase active and no jobs in flight (v0
is single-threaded; when `JOBS.md` lands, the loop guarantees the worker pool is idle at the
`produce` call). They are never called from a system, a transport callback, or a `CONTROL` handler
— those set a pending-correction flag that `produce` consumes. `TL_CHECK(!net.in_phase)` guards the
entry. Both net systems early-out while `resim_depth > 0` except for the hash-ring write, so a
re-run tick neither sends nor receives.

### 20.6 Tests (`tests/net/`, run by `tl_tests`; every test fresh-state, headless platform)

| ID | File | Phase | What it proves |
|---|---|---|---|
| T0 | `test_wire_layout.cpp` | 1 | every §20.2 struct: `sizeof`/`offsetof` pins, LE round-trip, pad-nonzero refusal, newer-version refusal, truncated-buffer refusal |
| T1 | `test_encode.cpp` | 1 | column round-trip for 1..9 frames; idle column is 3 B/frame; every action changing; pointer at ±(1<<30); `rec & 0xF0` refusal |
| T1f | `test_encode_fuzz.cpp` | 1 | seeded random frame sequences (10⁶) encode→decode equality; random byte mutation never crashes and is refused or decodes to something re-encodable identically |
| T2 | `test_archive.cpp` | 1 | segment round-trip; self-contained decode from a `delta_tick = 0` state; flag-escape path; crc32 detection of every single-byte corruption; 30-min synthetic 3-peer input → size < 80 KB; the `RecordedInput`-through-`Replay`-producer replay row is DEFERRED to Phase 2's gate (RR-17 ruling 2026-08-26 — the producer is `INPUT.md` §9.4's, a W3 deliverable) |
| T3 | `test_sequencer_fold.cpp` | 2 | property test: for N ∈ 1..8, every `live_mask`, every subset of reporting slots and every bitmap assignment on a 3-tick window: fold is order-independent over arrival permutation, `present ⊆ live`, quorum threshold exact; substitution per class (hold/decay table for k = 1..7 in `i8`/edge-null-released) against a hand-written table |
| T4 | `test_loopback_lockstep.cpp` | 2 | two processes (`tl_tests --child`) over loopback ENet, Hovel sim, 3,600 ticks, 1% shim loss: identical hash traces, `confirmed` never regresses, no WAIT longer than 2 horizons |
| T5 | `test_succession.cpp` | 5 | list ops: init/remove/append/cyclic eligibility; two instances fed the same records → identical lists and shadows |
| T6 | `test_epoch_claim.cpp` | 5 | S-08 shape in-process with 3 simulated peers: a claimant with `last_confirmed < peer's` is refused with reason 1; a caught-up claimant is granted; a granted claimant's `LR_EPOCH` is the first record at `confirmed + 1`; a stale-epoch downstream is dropped |
| T7 | `test_peer_fsm.cpp` | 5/6 | every row of §20.4's peer table; `LR_EVICT` at the wrong tick is rejected and raises suspicion; φ: synthetic arrival series crosses `PHI_SUSPECT_Q8` at the expected microsecond and clears on arrival |
| T8 | `test_quorum_loss.cpp` | 6 | S-09 in-process at N = 8: drop 4 at once → no eviction ever sequenced, termination after `QUORUM_LOSS_TICKS`, both halves' checkpoints byte-identical; drop 3 → evictions proceed |
| T9 | `test_predictor_det.cpp` | 3/6 | two predictor instances trained from the same confirmed log predict identical frames for 10⁴ ticks; phantom mask zeroes every non-`AXIS` action; counts halve at `0xFFFF` |
| T10 | `test_impair_repro.cpp` | 2 | same seed + same packet/clock sequence → identical drop/delay/reorder decisions; different seed differs; `static_assert` absent in `netcode` tier (compile-test target) |
| T11 | `test_checkpoint_torn.cpp` | 7 | write image; truncate/corrupt at every 4 KB boundary of the temp file before rename → loader refuses with a named code and the previous checkpoint loads; checkpoint without chain record is discarded |
| T12 | `test_chain_fork.cpp` | 7 | two chains sharing K entries then diverging: verify passes on each, `fork_point` returns K; tampered `prev` fails verify; forced-takeover entry is identified as a fork |
| T13 | `test_rejoin_verify.cpp` | 6 | in-process server + joiner: image + tail → post-catch-up per-arena vector equals the live one; an injected single-bit arena corruption is detected at the first segment boundary |
| T14 | `test_adaptive_delay.cpp` | 3 | synthetic miss patterns: raise at 2 misses/60, no second raise inside cooldown, lower after 600 clean; raise/lower frame rule produces a gap-free tick sequence |

### 20.7 Hovel exe (`tests/hovel/`)

Files: `hovel_main.cpp` (loop via the engine's headless platform, CLI, wiring), `hovel_cli.cpp`
(argument parser; tested by `test_hovel_cli.cpp`), `hovel_sim.cpp/.h` (the §19.2 sim; compiled into
the audited sim lib target `tl_hovel_sim`), `hovel_actions.luau` (action map + verb mapping in the
sim VM), `hovel_scenario.cpp` (S-01…S-15 scripted drivers: scripted inputs, kill/pull/leave/rejoin
actions by tick), `hovel_csv.cpp` (§19.8 row writer over stb_sprintf), `hovel_ballast.cpp`
(`BALLAST_ARENA_BYTES` fill, deterministic from seed), `runbook.md`.

CLI (every flag required unless a default is shown):

```
tl_hovel --name <node> --slot <0..7> --role {peer|coordinator|headless}
         --listen <port> --peers <host:port>[,<host:port>...]
         --model {match|persistent} --origin {seeded|restored} --world <dir> --seed <u64>
         --sim-load <n> --ballast-mb {0|1|4|16} --workers <n=1>
         --shim-seed <u64=0> --shim "<slot>:<drop_pct>/<delay_ms>/<jitter_ms>/<reorder_pct>[;...]"
         --csv <path> --events <path> --scenario <S-xx|none> --ticks <n|0=until ended>
         --speculation {on|off=on}
```

`--role coordinator` only asserts the node is seated at slot 0 for Milestone A (seating is otherwise
by §9.2). Exit code 0 = ended cleanly with zero divergence; 2 = divergence (the event log names the
tick and arena); 3 = quorum loss; 4 = CLI/handshake error.

**Runbook — Milestone A, three machines (PC, PC 2, and the Deck once benched — a headless
loopback peer stands in until then, §19.10):** (1) `cmake --preset netcode-win` on both PCs
(`netcode-deck` for the Deck); build once; `tools/fingerprint` prints one `build_id` for all
peers — record it. (2) `tools/deploy.sh deck <host>` when the Deck is on the bench. (3) Smoke on
the PC alone:
`tl_hovel --name pc --slot 0 --role coordinator --listen 7000 --peers "" --model match --origin
seeded --world out/w --seed 1 --sim-load 1 --ballast-mb 0 --csv out/pc.csv --events out/pc.ev
--ticks 3600 --scenario none`; exit 0 required. (4) Start in slot order within 30 s: PC (slot 0,
coordinator, `--peers pc2:7000,c:7000`), PC 2 (slot 1, `--peers pc:7000,c:7000`), peer C (slot 2,
`--peers pc:7000,pc2:7000`); common `--seed 20260822 --sim-load <L> --ticks 216000 --scenario S-01`.
(5) Watch each node's stderr for `handshake ok`, `session running`, then `digest ok tick=…` every
30 ticks; adjudicate progress against the CSV's `wall_us`. (6) At exit, pull the three CSVs and
event logs; `tools/hovel_compare <csv...>` asserts identical `state_hash_lo64` per tick and reports
p50/p95/p99 `sim_us` per machine, the heterogeneity ratio, and coordinator `bytes_out`. (7) Sweep
`--sim-load` until the slowest peer's p99 `sim_us` ≈ 4,000; record the load at 4/8/16.7 ms per
machine (§19.4). Gate: 1 h, zero divergence, exit 0 on all three.

### 20.8 Build order and "done" criteria

| Phase | Files created | Done when |
|---|---|---|
| 1 Encoder + archive | `wire.h`, `encode.cpp`, `archive.cpp`, `net_internal.h` (rings, `LogRecord` store), `tests/net/` T0–T2 | (Phase 1 cut at the W2/W3 seam — RR-17 ruling 2026-08-26.) T0–T2 green incl. fuzz for 10 min under ASan/UBSan, built against `main`'s `TL_WIRE_STRUCT`/`foundation/bytes.h` with the input-frame geometry pinned to `INPUT.md` §9.1's constants and test-local frame fixtures (`core/input.h` is the W3 loop+input lane's — never defined here); the `RecordedInput`-replay half moves to Phase 2's gate; 30-min synthetic archive < 80 KB recorded in `LESSONS.md` |
| 2 Two-peer ENet | `transport.cpp`, `impair.cpp`, `sequencer.cpp`, `producer.cpp` (delay-only path), `systems.cpp`, `net.h`, `hovel_main/cli/sim/csv` | T3, T4, T10 green; two Hovel processes over loopback 1 h at 1% loss, zero divergence; measured upstream per peer ≤ 6 KB/s recorded |
| 3 Scale + adaptive delay + speculation | `rollback.cpp`, `predictor.cpp` (prediction only), delay rule in `sequencer.cpp`, speculative path in `producer.cpp`, `hovel_scenario.cpp` S-01/S-13 | T9, T14 green; 8 loopback peers 30 min zero divergence; S-13 shows impaired peer's delay rises and others' `rollback_depth` distribution unchanged; rollback tick cost p95 recorded |
| 4 Shadows | mirror send/receive in `transport.cpp`/`sequencer.cpp`, shadow selection in `succession.cpp` | shadow rings hold every frame the coordinator holds (asserted per tick in `dev`); coordinator upstream measured against 1.40 Mbit/s at 8 loopback peers |
| 5 Failure + epochs | `peer.cpp` (φ, gossip), `succession.cpp` (claim/ack), `LR_EPOCH`/`LR_SUSPECT` in `sequencer.cpp`, S-07/S-08 drivers | T5, T6, T7 green; S-07: successor within 1 tick + ≤ 6-tick rollback, no peer accepts a tick below its `confirmed`; S-08: behind claimant refused (reason 1 in the event log) |
| 6 Phantoms, leave, eviction, rejoin, quorum loss | phantom path in `predictor.cpp`, `LR_LEAVE/RESUME/EVICT/JOIN/END`, `session.cpp` (join flow, termination), `checkpoint.cpp` hot tier, S-04/05/06/09/10 | T8, T13 green; S-04…S-06, S-09, S-10 pass per §19.6; rejoin at 4 MB ballast < 10 s recorded |
| 7 Persistent | durable tier + chain + custody in `checkpoint.cpp`, seat table + signatures in `session.cpp`, `Restored` start, S-11/S-12 | T11, T12 green; S-11/S-12 pass; a world survives three real days and a rebuild (chain crosses a `build_id`); torn-write test run on all three machines' filesystems |
| 8 Product | NAT ruling (§5.5: none for v1), spectator (a slot with `live_mask` bit but no frames: substitution only), commit/reveal (`CK_COMMIT` record kind + `BLAKE2b` compare via `crypto_verify32`) | spectator runs a full S-01 with identical trace; commit/reveal round-trips with `k = 2` and a mismatched reveal substitutes; Milestone E (10 h, three machines) zero divergence with §19.9 thresholds recorded |

A phase ends on the full green gate (every earlier phase's tests included) plus the measurement
recorded in `LESSONS.md` with the `build_id`.

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
| `TICK_HZ` | 60 Hz | Network tick is the same tick (`CANON.md`; `FIXED_DT_SECONDS` is render-side only) |
| `LOCAL_INPUT_DELAY_TICKS` | 3–6 (50–100 ms) | Adaptive per peer (§7.4), sequenced |
| `REDUNDANCY_TICKS` | 9 (150 ms) | **≥ `CONFIRMATION_HORIZON_TICKS`**, `static_assert`ed. Raise to 16 under measured loss |
| `CONFIRMATION_HORIZON_TICKS` | **6 (100 ms)** | **Speculation depth = failover cost = irreversible display delay** (§9.5); = max `LOCAL_INPUT_DELAY_TICKS`; ≤ `REDUNDANCY_TICKS` |
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
session model, archive, and budgets carried whole. All budgets are models until Gate 0 reports.
§0.3/§2/§8/§16/§19 swept 2026-08-25 for the Pi 4's removal (target set → `CANON.md` matrix;
bench = the two PCs now, the Deck when it joins; cross-ISA conformance on the hosted CI arm64
legs).*
