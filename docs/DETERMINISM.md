# Determinism — the contract, the ordering rules, the harness (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §8. The arithmetic half of the old
> language/engine split is now `FX-PALETTE.md` (bit-exact by construction); this doc owns
> *order*, *state*, *randomness*, *hashing*, and the *harness* — the places determinism is lost
> despite correct arithmetic.
> **Lineage:** harvests `../foundry/DETERMINISM-DESIGN.md` (ordering rules, §4 harness survive) and
> `FOUNDRY-DECISIONS.md` D11 under `PIVOT-DESIGN.md` §2–§3.

---

## 0. The contract

**Same initial state + same ordered input stream ⇒ bit-identical authoritative state on every
machine, every tick, every run, every worker count, every ISA (x86-64 ↔ aarch64).**

| Must be deterministic (feeds the state hash) | Free to be non-deterministic |
|---|---|
| the registered arena set: ECS columns, Alloy pools/graphs, RNG keys, tick counter | rendering, interpolation, camera |
| anything read back into next tick's state | audio, VFX, ImGui, profiling |
| input application order (`InputFrame` per peer per tick) | wall-clock, frame rate, thread scheduling |
| the Luau sim VM's effects on components (not its heap) | the editor/UI VM |

Lockstep model: only inputs cross the network. Every peer simulates every tick identically. A
divergence is a **desync = a bug**, caught by the per-arena hash. Non-deterministic code is
allowed and encouraged outside the sim — it just never writes back (INV-6).

---

## 1. What the pivot made free, and what it didn't (DECIDED — recorded so it isn't re-proposed)

**Void — no floats on any sim path:** NaN/−0 canonicalization before hashing; denormal/FTZ policy;
FP-environment pinning per tick and per worker; `@fast_math` escape-hatch risk; mixed arch-flag
agreement (SSE2 vs AVX2 peers); FMA contraction; transcendental bit-exactness across libms. None of
these can affect an integer result.

**Not free — the five things that still break it:**

1. **Order** — iteration order, reduction order, command application order (§2).
2. **State outside the registered arenas** — statics, Luau heap, thread-locals (§2.5, `MEMORY.md`).
3. **Randomness whose draw order depends on scheduling** (§3).
4. **Reading the wrong bytes into the hash** — capacity vs used extents, padding (§4).
5. **Undefined behaviour** — the one way integer code diverges between compilers (`CPP-SUBSET.md`
   §5); sanitizers in CI are the gate.

Enforcement moved from compiler attributes to: the symbol-audit gate (`CPP-SUBSET.md` §4), the
arena-offset guard (`MEMORY.md` §2), the record→replay harness (§6), and UBSan/ASan in CI. The
harness was always the real safety net; it is now the only one, so it lands first.

---

## 2. Ordering rules (DECIDED — every sim/engine system obeys all of them)

1. **Iterate by stable id / dense index, never by address or arrival.** ECS columns iterate
   `0..count` in packed order; `SlotMap` iterates `0..slot_cap` skipping dead slots; Alloy pools
   iterate handle-sorted lists. Pointer comparison other than equality is banned.
2. **Fixed phase pipeline + barriers.** `FIRST · PRE_UPDATE · UPDATE · POST_UPDATE · LAST`, a
   barrier between; within a phase the system order is the topo-sorted registration order, total
   and reproducible; a cycle is fatal at registration (`FRAME-LOOP.md` §2).
3. **Structural change only through command buffers**, applied at the barrier in **chunk order**
   (single-threaded: system order, record order within a system). Never mid-iteration.
4. **Reductions fold in chunk-index order** — the `JOBS.md` rule. Integer sums are order-free, the
   rule is kept anyway so a future non-commutative combine cannot break it.
5. **Worker identity is invisible to results**: outputs keyed by chunk id, never worker id; scratch
   is per-worker but command buffers are tagged by chunk. Work-stealing is therefore permitted.
6. **Fixed timestep, tick counter.** The sim sees `H` (`FX-PALETTE.md`) and `tick: u64`; never
   `real_dt`, never the frame rate, never a clock (symbol gate).
7. **No hash-map iteration in sim code.** `Map<K,V>` walk order is deterministic per insertion
   sequence but order-fragile across refactors; anything whose order outlives the binary (saves,
   wire, Luau-visible) uses `SortedMap`/`SortedSet` or a sorted copy (`CONTAINERS.md` §3).
8. **No strings or string-hash order in authoritative state** — interned `u16` ids and integers
   only inside the tick (`CONTAINERS.md` §5).
9. **All external inputs are sealed**: player input, network frames, editor mutations, Luau script
   reload effects, asset-load completions cross only as tick-stamped entries in the input/command
   stream. The sim never branches on load *state* — "asset ready" is a recorded command, not a
   poll.
10. **Sim-side RNG is keyed, never sequential** (§3).

### 2.5 State hygiene

- **No static mutable state in `src/` sim/engine TUs** — the dual-sim test runs two worlds in one
  process; rollback restores only registered arenas (`MEMORY.md` §2). CI grep.
- **Systems are stateless free functions**; persistent system state is a singleton component in a
  registered arena (`ECS.md` §3). Systems-as-singletons are banned.
- **Authoritative state never lives in the Luau heap** (`LUAU-LAYER.md` §2). Script tables are
  transient working data, reconstructible from world state, never carried across ticks.
- **Ticks allocate nothing** outside scratch — the arena-offset guard asserts it every tick in
  debug (`MEMORY.md` §2).

---

## 3. RNG (DECIDED)

**Stateless keyed mixing, not a sequential generator.**

```cpp
// the one entry point — the name every doc uses
u64  rng_for(u64 seed, u64 tick, u32 system_id, u64 carrier_id, u32 draw /* = 0 */);
u32  rng_below(u64 r, u32 n);        // Lemire multiply-shift on a rng_for() result; no rejection loop
q_t  rng_q(u64 r);                   // top 30 bits → fx<i32,30> in [0,1)
template <typename R> R rng_range(u64 r, R lo, R hi);   // lo + mul<R>(rng_q, hi - lo)

// rng_for = mix64(mix64(mix64(mix64(seed ^ K0) + tick) + (u64(system_id) << 32 | draw)) + carrier_id)
// mix64   = the splitmix64 finalizer: x ^= x>>30; x *= 0xbf58476d1ce4e5b9; x ^= x>>27;
//           x *= 0x94d049bb133111eb; x ^= x>>31.   K0 = 0x9e3779b97f4a7c15.
```

- A draw's value is a pure function of `(seed, tick, system_id, carrier_id, draw)`. Scheduling,
  worker count, and iteration order cannot change it. `draw` is a per-carrier counter the caller
  owns locally within the tick (never stored across ticks). `system_id` values are a closed enum
  in `rng_systems.h` (one per sim system/pass/rule family); adding one is a registration, and the
  enum is part of `build_id` by being source. The enum reserves a 256-wide block
  `RNG_SYS_LUAU_BASE .. +255` assigned to Luau-registered systems by registration ordinal
  (`LUAU-LAYER.md` §10.6), so a script's draws are keyed without editing the header.
- Render/UI/editor use their own separate sequential PRNG (`xoshiro256**`, render-side); it never
  touches sim state.
- World-gen uses its own key family `hash(seed, cx, cy, channel)` — generating a chunk at tick 0 or
  10⁶ yields identical content (`ALLOY.md` §12).
- **Never `std::` distributions** (implementation-defined — the classic lockstep desync).
- **CSPRNG** (session auth, custody signing, nonces): OS entropy behind the platform seam
  (`PLATFORM.md` §5), in a header physically unreachable from sim code; the symbol gate proves it.

---

## 4. Hashing (DECIDED)

| Use | Hash | Rule |
|---|---|---|
| state hash, desync CRC, `Map` buckets, ids | **rapidhash** (vendored, pure C, pinned seed `TL_HASH_SEED`, pinned implementation) | part of the lockstep contract; changing seed or implementation bumps the build fingerprint |
| compile-time name ids | **`constexpr` FNV-1a 64** (`"player_spawn"_id` → `NameHash`) | debug side-table (hash → literal) for inspector display; collisions are a registration-time fatal |
| crypto (chain, signing, commit/reveal) | **Monocypher** BLAKE2b / Ed25519 | never roll own crypto |

**Per-arena, per-tick:** each registered arena hashes `[base, used)` at `LAST` (`MEMORY.md` §2);
the world hash is the hash of the arena hashes in registry order. Per-arena granularity is what
the netcode's pass × arena desync bisection needs (`NETCODE.md` §14).

**What is hashed / not hashed:**

| Hashed | Not hashed |
|---|---|
| every registered arena's used extent (ECS columns, Alloy pools, graphs, bulk basins, RNG seed, tick) | scratch arenas; broadphase/coarse-sampling transients (derivable) |
| the compiled data tables (once, into the fingerprint — not per tick) | event queues (notifications; their *effects* are hashed) |
| | render/interpolation state, Luau heaps, net buffers |

**Hash-region integrity invariant** (from Alloy, now engine-wide): mutating a transient buffer must
NOT move the hash; mutating any authoritative pool MUST. Tested per pool.

---

## 5. Snapshots and rollback (DECIDED — mechanism in `MEMORY.md` §2)

Snapshot = memcpy of every registered arena's used extent, stamped with the build fingerprint,
fail-loud on mismatch. The rollback ring holds N recent confirmed snapshots allocated once; restore
= memcpy back. Whole-arena restore is too coarse for island-scoped rollback — **T-A-01** survives
the pivot as filed and is still the netcode's speculation gate (`ALLOY.md` open items).

The same mechanism powers: record→replay keyframes (seek without re-sim), late-join/resync, the
desync reproduction package, play-in-editor forks, and M2 durable saves (via the reflection
encoder, not raw memcpy — `ASSETS-AND-DATA.md` §5).

---

## 6. The harness (DECIDED — non-negotiable; lands in the foundation week before any subsystem)

Determinism bugs are silent until a player desyncs. Make them fail a test. Run continuously in CI:

1. **Dual-sim:** two worlds in one process, identical input log, compare per-arena hashes every
   tick. (This is why static state is banned.)
2. **Record → replay:** record the `InputFrame` stream + per-tick hashes; replay; assert the hash
   trace is identical. The `Replay` input producer (`INPUT.md` §4) is built for this, at v0.
3. **Worker-count invariance:** 1 / 2 / 8 / 16 workers → identical trace. Blocking release gate
   (T-F-02), plus one mixed-pair run (A at 4, B at 16) once transport exists.
4. **Long-run / fuzz:** seeded random input streams over long horizons (slow drift).
5. **Cross-ISA:** PC x86-64 vs Pi 4 aarch64 (cross-compiled from the same tree), hash traces
   compared. Infra-gated only on having the Pi on the bench; it is *required*, not optional. The
   Deck (x86-64 Linux) separates OS effects from ISA effects.
6. **Sanitizer runs:** the dual-sim and replay tests under UBSan+ASan (timing ignored).

A failing hash pinpoints the first diverging tick; per-arena hashing pinpoints the arena; the
reflection field tables give a field-by-field diff of the diverging component (`TOOLING.md` §5).

**Gate 0's G-06 is this harness in miniature** and is the first time it runs.

---

## 7. The desync workflow (DECIDED)

1. Binary-search the divergence tick against the retained input log.
2. Pass × arena bisection localizes to a phase/pass and an arena.
3. Reflection diff names the field.
4. Package: log segment + last matching snapshot + build fingerprint + each peer's platform/ISA.
   A perfect deterministic reproduction, replayable offline on any machine.
5. Default hypothesis: **UB** (run the repro under UBSan/ASan first), then order, then a state
   leak (static / Luau heap / thread-local).

---

## 8. Rulings (closed 2026-08-22 — nothing open)

- **R-1 Hash cadence:** every tick in the local harness (dual-sim, replay, worker sweep);
  `CHECKSUM_INTERVAL_TICKS = 30` on the wire for three-machine soaks (NETCODE App. B). The hash is
  *computed* every tick in every tier; only the exchange is sampled.
- **R-2 The per-arena hash is full, never incremental.** ≈0.5 ms at nominal load is inside the
  budget; a dirty-range hash is a second correctness surface (a missed dirty mark is a silent
  hole) for a win G-05 has not asked for. If G-05 shows hashing binding, the lever is chunk-
  parallel hashing (`JOBS.md` §4), not incrementality.

## 9. Implementation specification

### 9.1 Files

| File | Contents |
|---|---|
| `foundation/hash.h/.cpp` | `tl_hash64(const void*, usize, u64 seed)` = vendored rapidhash; `NameHash operator""_id`; `TL_HASH_SEED` |
| `foundation/rng.h` | `rng_for`, `rng_below`, `rng_q`, `rng_range<R>`, `mix64`; `rng_systems.h` (the `system_id` enum) |
| `core/record.h/.cpp` | `RecordedInput` writer/reader (format below); the `LAST`-phase recorder system |
| `core/checkpoint_sys.cpp` | the `LAST`-phase determinism checkpoint system: `registry_hash_all` → `world.hash_trace` ring + optional per-tick log |
| `tests/harness/` | `dual_sim.cpp`, `replay.cpp`, `worker_sweep.cpp`, `crossisa.cpp` (driver jobs), `hash_trace.h` (compare utility) |

### 9.2 `RecordedInput` file format (shared by the recorder, the Replay producer, the Script producer's save, and Hovel)

```
Header (WIRE_STRUCT, 128 B): magic "TLRI", format_version u32, build_id[32], session_fingerprint[32],
    seed u64, base_tick u64, peer_count u8, live_mask u8, flags u16 (HAS_ARENA_HASHES), frame_count u64, _pad
Body: frame_count × { InputFrame[peer_count] (76 B each, tick = low 32 bits) ; u64 world_hash ; [u64 arena_hash[arena_count] if HAS_ARENA_HASHES] }
Trailer: crc32 over body
```

Written by the recorder every tick in `dev` (ring-bounded in memory, flushed on "save replay"),
and by the driver unconditionally when `--record` is given. The Replay producer refuses a file
whose `session_fingerprint` differs (named error), supplies frames in order, and the harness
compares the stored hashes against the live ones tick by tick.

### 9.3 The checkpoint system (`LAST`, registered `before` `net_send` and the recorder)

```cpp
void sys_checkpoint(World* w) {
    u64 per_arena[MAX_ARENAS];
    u64 h = registry_hash_all(w->registry, per_arena);
    w->hash_trace.push({w->tick, h});                 // RingBuffer of 64 in a non-registered arena
    memcpy(w->last_arena_hashes, per_arena, ...);     // read by net_send and the recorder this tick
}
```

### 9.4 Harness APIs (`tests/harness/`)

```cpp
// Build two worlds from the same scene script + seed, feed identical frames, compare every tick.
ErrCode harness_dual_sim(const SceneDesc*, u64 ticks, const InputProducer* script, DualReport* out);   // out.first_divergent_tick, per-arena first-differing id
ErrCode harness_replay   (const SceneDesc*, StrView recorded_file, ReplayReport* out);
ErrCode harness_worker_sweep(const SceneDesc*, u64 ticks, const u32 workers[], u32 n, SweepReport* out);
// diff: given two snapshots (or snapshot + live world), walk registry → tables → rows → FieldInfo; print first N differences as "arena/component/entity/field: a vs b"
u32 desync_diff(const Snapshot* a, const Snapshot* b, const World* layout, Span<DiffLine> out);
```

The cross-ISA job is a driver invocation with `--record` on one machine and `--replay --verify`
on the other (`TESTING.md` §4); no special API.

### 9.5 Tests

`hash.test.cpp` (rapidhash known-answer vectors from upstream; seed sensitivity; `"lit"_id`
constexpr equals runtime FNV), `rng.test.cpp` (known-answer vectors for `rng_for` committed as
goldens; `rng_below` uniformity over 2²⁴ draws within 0.5%; `rng_q` range; draw independence
across `system_id`), `record.test.cpp` (write/read round trip; crc corruption refused; fingerprint
mismatch refused), `checkpoint.test.cpp` (hash changes iff a hashed arena changed — the hash-
region integrity test over every registered arena, automated: for each arena flip one byte inside
`used`, assert hash changes; flip one byte above `used`, assert unchanged).

*Rev 1 — 2026-08-22. Supersedes `../foundry/DETERMINISM-DESIGN.md` for this engine.*
