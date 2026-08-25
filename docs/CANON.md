# CANON — the cross-cutting constants, names and types (tidelock)

> **The one sheet every doc must agree with.** If a doc and this sheet disagree, the doc has a bug;
> fix the doc. If a new constant is needed, it is added here first, then used. Rev 1, 2026-08-22.
> Owning doc in the right column; this sheet states values, never rationale.

## Types (`src/foundation/tl_types.h`)

| Name | Definition |
|---|---|
| `u8 u16 u32 u64 i8 i16 i32 i64` | fixed-width integers from `<stdint.h>` |
| `f32 f64` | `float`/`double` — render/editor/tools/platform only; banned tokens in sim TUs |
| `usize` | `u64` (same width as `size_t` on every supported target; a distinct *type* identity would make a `usize`-keyed template select differently per target - CPP-SUBSET §5) |
| `uint_fit<N>` | `constexpr` selector: N≤8→u8, ≤16→u16, ≤32→u32, else u64 |
| `StrView { const char* ptr; u32 len; }` | non-owning string |
| `NameHash` = `u64` | `constexpr` FNV-1a 64 of a literal: `"player"_id` (offset 0xcbf29ce484222325, prime 0x100000001b3) |
| `StrId` = `u16` | interned id, process-stable, never serialized |
| `Result<T> { T value; ErrCode err; }` · `ErrCode` = `enum : u16`, 0 = OK, per-module ranges | the only failure shape |
| `Rect_f32 / Rect_i32 / Rect_u16 { x, y, w, h }` | three concrete structs in `foundation/rect.h` (not a template); `min/max` derived; never `top/bottom` |
| `Transform { pos_t x, y; angle_t rot; u32 flags }` | engine component; `flags` bit 0 = snap, bits 1..31 zero |
| `Handle<Tag, IDX_BITS, GEN_BITS>` | `bits == 0` is null; generation 0 never issued; gen starts at 1 |
| `Entity` = `Handle<EntityTag, 22, 10>` (u32) | 4M slots / 1023 usable gens (gen 0 never issued; the wrapping remove quarantines the slot - W1 containers review); wrap → slot quarantined |
| resource handles `Handle<_, 12, 4>` (u16) | textures, fonts, audio clips, clips, data tables |
| `ComponentId` = `u16` dense (< 1024) · `ActionId` = `u16` dense (< 32) · `EventTypeId` = `NameHash` | |
| `w->sched.running = { index, label }` | published by `run_phase` before every system call (trampolines, profiler) | ECS §10.6 |
| Luau bindings | `ecs.component` / `ecs.system` / `ecs.each` / `ecs.get` (one spelling; `declare_component` does not exist) | LUAU-LAYER §10.6 |

## World constants (`fx_palette.h`)

| Name | Value |
|---|---|
| world unit | 1 unit = 1 m |
| `TEXEL` | 1/16 m (`pos_t` raw `1 << 14`) |
| `CHUNK_TEXELS` | 128 → chunk = 8 m |
| world extent | ±4,096 m (`CHUNK_GRID` = 1024 × 1024 chunks) |
| `V_MAX_WORLD` | 512 m/s |
| `TICK_HZ` | 60 · `FIXED_DT_SECONDS` = 1/60 (render-side f64 only) |
| `SUBSTEPS` | 8 · `H` = `dt_t(1/480)` · `INV_H` = 480 (plain int) |
| `MASS_RATIO_CLAMP` | 4096 (2¹²), effective per-pair inv-mass clamp |
| `G_SUBSTEP` | `vel_t(9.81 × 1/480)` precomputed |
| `MAX_STEPS` | 5 (spiral-of-death cap; drop time) |

## The fx palette (`FX-PALETTE.md` §3, rev 1.1)

| Row | Format | Range | Notes |
|---|---|---|---|
| `pos_t` | `fx<i32,18>` | ±8,192 m | 3.8 µm quantum |
| `vel_t` | `fx<i32,20>` | ±2,048 m/s | |
| `invmass_t` | `fx<i32,18>` | ±8,192 | statics exactly 0 |
| `stiff_t` | `fx<i32,30>` | ±2 | α̃ = α/h², precomputed at init |
| `q_t` | `fx<i32,30>` | ±2 | unitless, kernels on q = r/h |
| `angle_t` | `fx<i32,30>` | ±2 turns | turns; wraps by masking |
| `omega_t` | `fx<i32,22>` | ±512 turn/s | retuned rev 2: |ω| ≤ 240 turn/s structurally (`FX-PALETTE.md` §3) |
| `dt_t` | `fx<i32,30>` | ±2 s | only `H` |
| `scalar_t` | `fx<i32,16>` | ±32,768 | unitless scalars; `lambda_t` is an alias |
| conserved quanta | `i32`/`i64` | — | saturating ops only |
| SDF texel distance | `i16`, 4 frac bits | ±2,048 texels | storage, not a row |
| `FX_PALETTE_REV` | 2 | in `build_id` |

## Sizes and caps

| Name | Value | Owner |
|---|---|---|
| `MAX_PEERS` | 8 | NETCODE |
| `MAX_ACTIONS` | 32 (compile-time; wire-format bump to change) | INPUT |
| `sizeof(ActionState)` / `sizeof(InputFrame)` | 2 B / 76 B | INPUT |
| `MAX_COMPONENT_TYPES` | 1024 | ECS |
| `MAX_ARENAS` | 64 | MEMORY |
| sparse-set page | 4096 entries (`u32` dense index) | ECS |
| particle hot row | 32 B | ALLOY §1.4 |
| particle spacing / kernel radius | 2 texels / 4 texels (= fine-tier cell) | ALLOY §3.5 |
| coarse sampling cell | 4 texels | ALLOY §4.4 |
| `TL_HASH_SEED` | `0x7469646c6f636b31` ("tidelock1") | DETERMINISM |

## Netcode constants (`NETCODE.md` App. B) — all in sim ticks at 60 Hz

`CONFIRMATION_HORIZON_TICKS` 6 · `LOCAL_INPUT_DELAY_TICKS` 3–6 adaptive · `REDUNDANCY_TICKS` 9 ·
`MAX_TICKS_PER_PACKET` 9 (floor 3) · `SUB_DECAY_TICKS` 6 · `AOE_ISLAND_LIMIT` 4 (min telegraph 6) ·
`QUORUM` = strict majority of sequenced-live · `QUORUM_LOSS_TICKS` 600 · `SUSPECT_TO_EVICT_TICKS`
1800 · `SHADOW_COUNT` 2 · `CHECKPOINT_HOT_TICKS` 300 · `CHECKPOINT_DURABLE_TICKS` 18000 ·
`DURABLE_KEEP` 5 · `CHECKSUM_INTERVAL_TICKS` 30 · `LOBBY_PROBE_HZ` 1 · ENet channels `INPUT`=0
`CONTROL`=1 `BULK`=2 · NAT = LAN/direct-IP for v1.

## Wire struct sizes (`NETCODE.md` §15.1/§20.2 are the authority; listed so nothing drifts)

`InputFrame` 76 B · `Handshake` 120 B · `PacketHeader` 40 B (`last_confirmed_tick` is `u64`) ·
`CheckpointHeader` 192 B · `MoveIntent` per `ALLOY.md` §8.2 · `RecordedInput` header 128 B ·
`SaveHeader` 160 B. Every one is a `TL_WIRE_STRUCT` with `sizeof`/`offsetof` static_asserts.

## Cvars (`TOOLING.md` §3)

Names are `module.name`, lowercase, dotted. `SIM`-flagged cvars are in `session_fingerprint` and
a lockstep session refuses changing them. The `SIM` set at rev 1: `net.speculation` (bool),
`net.phantom_mask` (u32 action mask), `script.budget_safepoints` (u32), `script.gc_step_kb` (u32).
Adding a `SIM` cvar is a ruling recorded here.

## Platform extras settled during the spec pass

`PlatformApi` gained `CrashApi crash` (appended, abi 1) — the out-of-process dumper is the same
exe run as `tidelock --dump` (no new exe). `TexHandle`s are minted by the platform `DrawApi`
(`TEX_STATIC / TEX_STREAMING / TEX_TARGET`); the asset registry holds them, never a second id.
`FileApi` has `append` (log/TSV sinks). Headless links no SDL.

## Luau sim VM — the exact removal list (`LUAU-LAYER.md` §10.2)

Removed globals: `math`, `os`, `io`, `debug`, `pairs`, `next`, `coroutine`, `string.rep`,
`loadstring`, `collectgarbage`, `print` (→ `log.info`), `require` after init, `getfenv`/`setfenv`,
`rawequal`/`rawget`/`rawset` (the proxies forbid raw access), `tostring` on userdata replaced (no
address leaks), `string.format %p` replaced. Provided: `ipairs`, `sortedpairs`, `fx.*`, engine
binding tables, pure `string`/`table` functions. The interrupt budget counts **safepoints**
(Luau's interrupt granularity), not instructions — equally deterministic.

## Ticks, hashes, fingerprints, RNG

| Name | Definition | Owner |
|---|---|---|
| tick | `u64` in state/snapshots/saves/log; `InputFrame.tick` = low 32 bits; containers carry `u64 base_tick` | FRAME-LOOP §1 |
| state hash | rapidhash64, seed `TL_HASH_SEED`, per registered arena over `[base, used)`; world hash = rapidhash over the per-arena u64s in registry order | DETERMINISM §4 |
| `build_id` | BLAKE2b-256 at build, **target-independent**: tree hash of `src/ cmake/ vendor/ script/sim/ script/lib/ toolchain/VERSIONS`, the semantic compile defines, tier, `FX_PALETTE_REV`, sim+lib bytecode. Compared between peers; a mismatch ends the session | BUILD §5, §9 R-8 |
| `build_env` | BLAKE2b-256 at build, **local**: compiler id/version/triple and the full resolved compile commands. Reported in CSV headers, crash reports and soak metadata; never compared between peers | BUILD §5 |
| `session_fingerprint` | BLAKE2b-256 at init (`build_id` ‖ reflection tables ‖ arena order ‖ action map ‖ data-table hash ‖ SIM cvars) | BUILD §5 |
| `rng_for(seed, tick, system_id, carrier_id, draw)` → u64 | splitmix64-finalizer mix; `rng_below`, `rng_q`, `rng_range<R>`; `system_id` enum in `rng_systems.h` with a 256-wide `RNG_SYS_LUAU_BASE` block | DETERMINISM §3 |
| world-gen RNG | `gen_hash(seed, cx, cy, channel)` — separate family | ALLOY §12 |
| crypto | Monocypher BLAKE2b (chain, commit/reveal), Ed25519 (custody, identity), `crypto_verify32` | NETCODE |

## Phases and the barrier

`FIRST · PRE_UPDATE · UPDATE · POST_UPDATE · LAST` (sim, per tick) · `PRE_RENDER · RENDER` (render,
per frame). Reserved append-only: `INIT`, `FRAME_END`. Every phase boundary flushes command
buffers. End-of-tick barrier order: (1) apply commands in chunk order; (2) swap events, clear
write side; (3) `prev ← current` ping-pong; (4) reset worker scratch. Per-arena hash is taken in
`LAST` *before* the barrier. `alloy_step` is one `UPDATE` system.

## Memory tiers and arenas

`VMemArena` (reserve/commit) · permanent arenas in the **registered set** (`HASHED | SNAPSHOT |
GROWS_AT_BARRIER`) · per-worker scratch (reset per tick/barrier; debug poison `0xDD`) · `SlotMap`
(the only slot reuse) · `mem_pool` (vendor heaps only). Snapshot ring = `CONFIRMATION_HORIZON_TICKS`
slots, allocated once. Registration order = lockstep contract.

## Build tiers and targets (`BUILD.md`)

**Targets (ruled 2026-08-25 — the home of the target set):** the platform matrix
**{Windows, Linux} × {x86-64, arm64}** — triples `x86_64-pc-windows-msvc`,
`aarch64-pc-windows-msvc`, `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`. Targets are
OS × ISA pairs, not machines: any conforming hardware is a peer. CI's hosted native runners are
the conformance instances (bit-exact traces, one `build_id` — `BUILD.md` §10.4); the dev PC,
Steam Deck and Pi 4 remain the physical perf + soak reference set (`NETCODE.md` §19.4,
`GATE0-BENCH.md` §4), and perf is graded only there — never on shared runners. A new OS or ISA
enters this list by ruling, not by drift.

Tiers `debug` (`-O0 -g`), `dev` (`-O1 -g`, `TL_DEV=1`), `netcode` (`-O2 -g1`), `ship`. Libs
`tl_foundation`, `tl_foundation_det` (audited), `tl_core`, `tl_sim` (audited), `tl_net`,
`tl_render`, `tl_platform_sdl3`, `tl_platform_headless`, `tl_editor` (dev only), `tl_script`. Exes
`tidelock`, `tl_tests`, `tl_driver`, `tl_gate0`, `tl_hovel`. Tools `fingerprint`, `luauc`, `audit`,
`fxcheck`, `sysroot`, `deploy`. Toolchain: clang (clang-cl on Windows), C++20, pinned in
`toolchain/VERSIONS`. Budgets: full rebuild < 10 s, incremental < 2 s, cold.

## Luau VMs (`LUAU-LAYER.md`)

**sim** (restricted — the exact removal list is in the "Luau sim VM" section above; `ipairs`,
`sortedpairs`, `fx.*`, engine bindings; interpreter only; frozen globals; safepoint budget) · **ui** (stock + draw/ImGui bindings; read-only world; NCG allowed) · **data**
(throwaway per compile). Handles = tagged lightuserdata. fx values cross as raw `i32` bits.

## Module/include firewall (`ARCHITECTURE.md` §1, `CPP-SUBSET.md` §4)

`foundation` ← nothing · `core` ← foundation, platform · `sim` ← foundation only (symbol-audited;
no `float`/`double` tokens) · `render`, `net`, `editor` ← core (+ `sim/views.h` for render) ·
`app` ← everything. Vendor headers never above their wrap module.
