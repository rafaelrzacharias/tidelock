# Reserved seams — designed, not built (tidelock, rev 1)

*2026-08-22. Lineage: harvested from `../foundry/3d-engine-design/ENGINE-DESIGN.md` (the
retired Ore engine's 3D-expansion ruling, 22 areas) and the Foundry core/extraction ledgers;
only the dimension-agnostic and 2D-relevant "design-and-defer" areas survive here, re-based on
`PIVOT-DESIGN.md`. Nothing else in foundry is a live reference for these seams.*

**The standing rule.** Design-and-defer is a *completed* state, not a placeholder. Each seam
below has a pinned shape and a pinned trigger; nothing about it is missing except a consumer.
Building one before its trigger fires is speculative breadth — the fault that stalled Layr.

**The agnostic-core litmus** (applied to every entry): *if the next game were a different genre,
would this belong in the engine unchanged?* If no, it is a Luau-layer concern, not a seam.

**Promotion-ladder policy** (`PIVOT-DESIGN.md` §9, carried as organizational policy): a seam is
built only when pulled by a real consumer; an API is promoted to engine-level only after **two
structurally different consumers**; a promoted API carries **no game nouns**; every promoted
module ships **headless tests** in the determinism harness. Until then the code lives game-side
(Luau or `script/`) and is moved, not rewritten, when promoted (`ARCHITECTURE.md`).

**Reading the entries.** Each is `## N. Name — reserved (trigger: …)` with four blocks:
**Shape** (decided, compressed), **Tidelock deltas** (fixed point, Luau, C++ subset, no DLL),
**Must not become** (the Layr trap: a mode that forks every system), **Ore-era detail dropped**.
Sim-side = inside the lockstep contract, fixed point only, hashed (`DETERMINISM.md`).
Render-side = floats legal, never authoritative, never written back.

---

## 1. Audio — reserved (trigger: the first sound — dig / footstep / UI click)

**Shape.**
- Stack: SDL3 audio device + SDL3_mixer (decode, bulk mix, device I/O) + **own SoA
  spatializer** hooked through the post-mix callback. Vendor the cold solved part, own the
  gameplay-coupled hot part. miniaudio demoted, not banned (its only edge is the spatializer
  we write anyway). Both are pure C, vendored per `PLATFORM.md`.
- Voices are SoA columns (position / gain / pitch / state), batch-spatialized. Real-voice cap
  + virtualization: past the cap, track the quietest/farthest unmixed, promote on audibility.
- Spatializer owns: movable listener, selectable attenuation (inverse / linear / exponential),
  stereo panning, doppler, cones. Buses (SFX / music / voice / ambient / master) + a post-mix
  DSP hook (low-pass occlusion, FDN parametric reverb) are engine primitives; content is not.
- **Own thread** (the mixer device callback); sim ↔ audio over a **lock-free SPSC ring**. The
  sim posts typed events ("play clip X at position P") through the `EventQueue<T>` mechanism
  (`ECS.md`); the audio thread drains and mixes. **One-directional, output-only**: audio never
  writes back, so there is no inbound side to seal — strictly simpler than input/net.
- Zones (reverb profile / low-pass) are data-table content queried through the **spatial
  index** (§3), never an audio-owned structure. Occlusion v1 = listener→source raycast via the
  same query. Sound *propagation* (around corners) is a later tier behind the same zone API.
- Clips are `u16` resource handles (`Handle<Audio,12,4>`, `ASSETS-AND-DATA.md`). Small SFX
  sync-load; music/ambient loops ride the async loader (§10) when it exists.
- Backend isolation: engine/Luau speak voice/bus/listener handles; no `MIX_*` or SDL type
  crosses the wrap (`PLATFORM.md` rule). Headless harness builds register no audio phase slot
  — compiled out by absence, not a runtime `if`.

**Tidelock deltas.**
- Voice positions arrive as `pos_t` in the event payload and are **converted to `f32` at the
  event boundary** — audio is render-side, so floats are legal there and nowhere upstream.
  ±8,192 m in f32 is sub-millimetre; no precision concern.
- Events are deterministic (fixed emission order, replay reproduces the cues); mixing is not
  state and never gates the hash chain. `DETERMINISM.md`'s free-to-be-nondeterministic table.
- Music / footstep-material / bark / dynamic-loop logic is **Luau** (game content on the wrap);
  the engine exposes `audio.play(clip, pos)`, `audio.bus_gain`, `audio.listener` bindings in
  the UI VM, and `emit_sound_event` from the sim VM (which only *posts*; it cannot read audio).
- Every peer renders, so every peer mixes — but a peer with no device (a headless soak) runs
  the same build with the audio slot unregistered.
- No destructors: voice lifetime is slot-generation in a `SlotMap`; device teardown is an
  explicit `audio_shutdown()` in the platform seam's ordered shutdown list.

**Must not become.** A second spatial structure; a sim-readable "is sound playing" query
(that is the write-back path); an audio-driven gameplay trigger (hearing is a *sim* event to
the AI toolkit §5, not a mixer readback); per-game bus layouts baked into the engine.

**Ore-era detail dropped.** `f32x4` SIMD-layer dependency, Ore FFI callback notes, Steam
Audio geometric acoustics as a target (still a reference, not a plan), 3D cone/HRTF ambitions.

---

## 2. Game UI toolkit — reserved (trigger: an Overburden-class HUD)

**Shape.**
- **Immediate-mode core + a retained STATE side-table** (not retained widgets): widgets are
  functions called every frame; only persistent state (focus / hover / scroll / cursor /
  anim timers / computed sizes) lives in an `id → state` table. A health bar *is*
  `ui.bar(player.hp)` — the stale-copy bug class is structurally impossible.
- Output is a **leaf draw list** (quads / glyph runs / clip rects / texture) consumed by
  `RENDER2D.md`'s command buffer, frame-arena allocated. UI never owns a GPU resource.
- Text on **SDL_ttf** (rasterization only); layout (wrap, alignment, ink bounds, CJK kinsoku)
  is an engine-side stateless layout kernel into caller buffers.
- Layout = flex + grid + anchors, two-pass measure→place. Theme/brush keyed by
  (widget-type, state): Solid / Gradient / Texture / NineSlice, CPU-expanded to quads.
- **Event routing** capture→target→bubble, consume-stops-propagation, hit-tested against
  widget rects, fed by the input airlock's UI-rate visual snapshot (`INPUT.md`). Gamepad nav
  is a focus-ring walk over the same rects.
- **Render-rate, non-deterministic.** UI animation = a retained tween/spring table keyed by
  widget id, TTL-GC'd, owner-scoped. A gameplay-affecting animation (a door gating
  collision) is a *sim* tween in a component — never this table.

**Tidelock deltas — the game UI is driven from Luau.** Three candidate shapes were weighed:

| | A. Luau-immediate over C++ primitives | B. Dear ImGui as game UI | C. C++ retained toolkit |
|---|---|---|---|
| Iteration loop | script reload (the ruling's loop) | recompile | recompile |
| Skinnable / game-styled | yes (brush table is data) | poor; ImGui is a dev aesthetic | yes |
| Fits "games bring meaning" | yes — layout + flow are content | no | no — layout is C++ |
| C++ subset cost | small: primitives + layout kernel + state table | zero new code | large; retained trees want RAII/inheritance |
| Determinism exposure | none (UI VM, render-side) | none | none |
| Per-frame binding cost | N widget calls/frame across the VM boundary | none | none |
| Layr lesson (retained widget = friction) | honoured | n/a | violated |

**Pick: A.** The binding cost is bounded (a HUD is tens of widgets, not thousands; a list view
batches through one `ui.list(n, fn)` call) and everything else favours it. ImGui stays
**dev-only** (inspector, console, profiler, editor shell, `TOOLING.md`) and is never the game
UI; a shipped build compiles ImGui out. C is rejected outright: a retained tree is the one
shape that fights the §2 subset on every line.
- Widget *state* lives in the C++ side-table (not the Luau heap) so script reload does not
  lose focus/scroll — but it is render-side state, so it is not hashed and not rolled back.
- The Luau UI VM reads world state through the reflection glue (`LUAU-LAYER.md`), read-only;
  UI input that affects the sim goes out as an action through the airlock, never a direct
  component write.
- `stb_sprintf` for formatting; no string class — labels are interned ids or `StrView` into
  the frame arena.
- Binding shape: one C++ `ui_begin(frame) … ui_end()` pair per frame; each primitive is a
  light-userdata call with integer ids (`"hp_bar"_id` hashed at load, not per frame). A
  missing `ui_end` is a fatal assert — an unbalanced frame is a bug, not a recoverable state.
- Dev builds may overlay ImGui on top of the game UI draw list (same frame, later pass) for
  widget-rect debugging; the overlay is never part of the game draw list.

**Must not become.** A second toolkit (one draw-list path serves HUD, menus, and the replay
transport §7); an ImGui skin layer; a CSS engine / data-binding VM (immediate reads replace
binding); a retained widget object model; a UI-driven frame loop (UI is a RENDER-phase system).

**Ore-era detail dropped.** Comptime `@fields`/`@attr(ui.*)` reflection dispatch (tidelock's
equivalent is the X-macro walker, dev-only); rich-text markup, per-glyph typewriter/sine
effects, composite brushes, visual builder — all "grow behind the interface when pulled".

---

## 3. Spatial index — reserved (trigger: a game with many queryable entities — AI, projectiles)

**Shape.**
- A **reusable module**, `core/spatial`. The non-negotiable rule and the reason it exists:
  **ECS position is authoritative; every index is a derived, refit-per-tick cache.** This is
  what deletes the two-sources-of-truth desync.
- One interface — `insert / remove / refit / query(aabb | circle | ray | point)` — over the
  2D structures: **uniform grid** and **spatial hash** (evenly spread many-entity cases:
  bullets, units), **quadtree** (clustered / variable density). Structure choice per instance.
- **Shared module, not one shared instance.** One *primary* world index (refit in `LAST`
  after transforms) serves general consumers — AI perception, audio zones, ad-hoc Luau
  queries; specialized consumers (a game's projectile set, a tilemap's chunk cull) instantiate
  their own. Nobody hand-rolls spatial code.
- Refit moved entities only; statics flagged and skipped. Query results are written to
  caller-provided spans in a **deterministic order** (cell order, then stable entity index).

**Tidelock deltas.**
- Queries take **integer `fx` AABB / circle / ray** (`pos_t` extents; radius² compared in
  widened integer — no sqrt on the query path). Cell coordinates are `pos_t >> k`, exact.
- `on_origin_shift` is **absent** (§13 — there is no origin shift).
- **Alloy is not a client.** Alloy owns its own tiered uniform spatial hash for particles and
  bodies (`ALLOY.md` §1.2: broadphase + neighbor graph, plain indices, tick-scoped). This seam
  indexes *gameplay entities* (the small population with generational handles, `ECS.md`);
  a gameplay query that needs sim citizens goes through Alloy's query API, not this index.
- Index storage is a registered arena only if it is ever read inside the tick by a hashed
  system; as a pure derived cache it lives in a non-hashed arena and is rebuilt after rollback.
  Rebuild-after-rollback is the cheaper rule: a refit is O(moved) and a rollback is rare.
- Cell size is a per-instance constant (power-of-two texels) chosen by the consumer; the
  primary index defaults to one Alloy chunk (8 m) so gameplay and sim cell addressing line up.
- Refit runs as one `parallel_for` over the dirty-entity list chunked by a pure function of
  `(N, grain)` (`PIVOT-DESIGN.md` §12a); per-chunk insert lists are merged in chunk order, so
  the index is identical at 1 or 16 workers.
- Luau binding: `spatial.query_aabb(index, rect, out)` fills an ordered array; iteration over
  the array part is deterministic by construction (`LUAU-LAYER.md`).

**Must not become.** A scene graph (hierarchy stays a `Parent` component + one pass); a
physics broadphase for Alloy; a per-consumer hand-rolled grid in some system (the Layr
desync); a frustum/3D structure family behind the same name.

**Ore-era detail dropped.** Loose octree, BVH, frustum queries, cube-sphere cell scheme,
zone/portal refinement, GPU-driven cull hand-off, `Rect<f32>`.

---

## 4. Tilemap terrain — reserved (trigger: a NON-Alloy 2D game — Terraria / Spelunky-class)

**Shape.**
- **Chunked grid of tile ids** (`u16` per cell, chunk = 64² or 128², sized to match Alloy's
  8 m chunk so streaming units agree if both ever coexist).
- **Tile-definition table**, Luau-authored (`ASSETS-AND-DATA.md` data-table mechanism):
  `tile_id → {source_rect, solid, collision shape (full/slope/platform), material, flags}`.
  Engine provides the table and the validator; the game says what a tile means.
- Render: visible chunks culled by viewport, **dirty-chunk re-batch** only; per-tile quads via
  `SDL_RenderGeometry` or chunk-baked-to-texture for dense maps (`RENDER2D.md`).
- Query: `tile_at(x,y)`, **`solid_at(x,y)`** — O(1). Tile collision is its own fast path for
  2D movement; it does not go through §3.
- Modify: `set_tile(x,y,id)` marks the chunk dirty + invalidates collision. Dig = set-to-empty;
  explosion = batched region clear. Sub-tile destruction mask = a reserved separate bit layer.

**Tidelock deltas.**
- Tile coordinates are `pos_t >> TILE_SHIFT`; with `TEXEL = 1/16 m` a 1 m tile is 16 texels —
  the world constants already make tile addressing exact integers.
- The tile grid is a **registered arena** (it is authoritative, hashed, rolled back); chunks
  commit pages on demand from a `VMemArena` (the same extension ALLOY §13 uses).
- `set_tile` from Luau goes through the ordered command channel, applied at the barrier — a
  script never pokes the grid mid-tick.
- **Alloy's SDF solids make this irrelevant for the sim games.** Overburden / Quench terrain
  *is* Alloy (`ALLOY.md` §2). The cellular sim stays Alloy/game-owned. This seam exists only so
  a cheap tile-based game is not forced to boot the matter sim.

**Must not become.** A terrain abstraction Alloy must also satisfy (no unified
`terrain.query`); a tile-aware render2d (render2d sees quads); a place where gameplay rules
(what water does) live; a required module in the sim-game builds.

**Ore-era detail dropped.** 3D heightmap (clipmap / CDLOD / SVT / control-map), voxel
terrain, city↔terrain seam, sparse residency backing store — the entire 3D terrain model.

---

## 5. 2D navigation + AI toolkit — reserved (trigger: Quench-class creatures)

**Shape.**
- Navigation substrate options, picked per game: **grid A\* / JPS** over a solidity query
  (§4 `solid_at`, or an Alloy occupancy query); **flow fields** (integer cost field + Dijkstra
  flood, O(1) per agent — the crowd case); **platformer link graph** (walkable surfaces joined
  by jump-arc / fall / ladder links — the side-view case). No polygon navmesh until pulled.
- AI toolkit, dimension-agnostic: reactive **behaviour tree** as executive glue, **utility**
  selectors inside it, **FSM** at leaf level only, **blackboard = the agent's component data**
  (never a per-agent hash map) + a small shared squad blackboard, **perception** layered
  (spatial query → distance → FOV → LOS raycast, graded, sense-rate LOD, hearing as a sim
  event), **activity tiers** (Active / Drowsy / Dormant) by proximity-to-any-player, never
  by camera.
- Node arrays are flat, immutable, switch-dispatched; per-agent execution state is SoA.

**Tidelock deltas.**
- **Decision logic lives in Luau (sim VM, deterministic)** over C++ primitives. The engine
  builds: the flow-field flood (`nav.flood(cost_field, goals) → dir field`), grid A\*/JPS over
  an integer cost callback, LOS raycast via Alloy's SDF query, the activity-tier bucketing.
  BT / utility / FSM / blackboard *composition* is a Luau library — structure-as-data
  interpreted by script, state in POD components (`LUAU-LAYER.md` state rule).
- **ORCA-style float avoidance is VOID.** Any local avoidance is fixed-point: a
  separation-steer over §3 neighbours in `vel_t`, or none (Alloy's AgentBody already
  resolves contact). Utility scores are `fx<i32,16>`-class, ties broken by keyed RNG
  (`rng_for(seed, tick, system, agent)`) then stable id.
- Perception order: agents iterated in stable-id order; neighbour lists in §3's deterministic
  order; hearing stimuli drained from the event queue in emission order.
- Budgeting: A\* is sliced by a per-tick node budget that is a **constant**, never a time
  slice — a wall-clock budget is a desync.
- Promotion: the Luau BT library is promoted to C++ only if profiling shows the interpreter
  is the bottleneck for a second structurally different consumer.
- Flow fields are **integer cost fields** (`u16` per cell, `0xFFFF` = impassable) flooded by a
  deterministic BFS/Dijkstra in cell order; the direction field is a `u8` per cell. Cost
  fields are registered-arena state if an agent reads them inside the tick.

**Must not become.** A "director" / pacing system, traffic, squad tactics, RTS strategy —
all game-layer compositions; a behaviour editor before a second consumer; a planner (HTN /
GOAP) on spec; a perception system that reads Alloy internals instead of its query API.

**Ore-era detail dropped.** Recast / Detour / dtTileCache, HPA\* tiling, planetoid
cross-face stitching, ORCA / RVO2, strict-float tie-breaking rules, influence maps (reserve
as a Luau 2D grid until pulled).

---

## 6. Frame / spritesheet animation — reserved (trigger: the first animated sprite — walk / dig)

**Shape.**
- **`Clip` asset** (data table): `frames: [{source_rect: Rect<u16>, duration, events: [StrId]}]`
  + `mode: Once | Loop | PingPong`. `source_rect` is the existing texel rect `Sprite` uses.
- **`Animator` POD component**: `{clip: ClipHandle, frame: u16, elapsed, speed, flags}`.
- **System** (sim tick): `elapsed += speed`; on `elapsed ≥ duration[frame]` step `frame`
  (wrap / bounce per mode), carry the remainder; write `frames[frame].source_rect` into
  `Sprite.source_rect`. Frame **events** (`footstep`, `hitbox_on`) emitted through the event
  queue when a tagged frame is entered — the decoupled gameplay hook.
- `cosmetic` flag: the clip advances at render rate in the RENDER phase, is not hashed, and
  may never carry events. Gameplay-coupled clips (attack windows) stay on the sim tick.
- A clip is a leaf a Luau state machine drives (idle / walk / attack set the active clip).
  ~200 lines + the asset.

**Tidelock deltas.**
- **Durations are integer ticks**, never `f32` seconds. `speed` is `fx<i32,16>` (1.0 = one
  tick per tick) and `elapsed` accumulates in the same format; the comparison is integer.
  Cosmetic clips may use `f32` seconds — they are render-side.
- `Animator` is registered through the X-macro (`ECS.md`): inspectable, saved, hashed. It lives
  in a registered arena, so rollback rewinds animation phase with everything else.
- Clip selection logic is Luau; the advance system is C++ (it runs per animated entity every
  tick — the one place the binding cost would show).
- `ClipHandle` is a `u16` resource handle; frame-event tags are interned `StrId`s.
- Render interpolation between ticks does not apply to `source_rect` (a frame is discrete);
  the render system reads the current frame's rect and interpolates only the transform.
- The clip table validator rejects a `duration` of 0 ticks and a `Loop` clip with one frame
  carrying events (an event every tick is almost always authoring error) — fail at init.

**Far-later (mechanism 2): 2D skeletal** (Spine-class — bone hierarchy, 2D rigid-attach or
mesh-deform skinning, pose sampling SoA). Same seam, different leaf output (a pose instead of a
rect). Not designed further until a game wants production-value characters; `angle_t` in turns
is the natural bone rotation format when it is.

**Must not become.** An animation graph engine; a skeletal system smuggled in as "mechanism
1 plus bones"; an animation-drives-position path (the sim drives animation for replay actors);
a tween library (UI tweens are §2's; gameplay tweens are components).

**Ore-era detail dropped.** ozz-style SoA pose pipeline, IK, retargeting, morph targets,
motion matching, root-motion, significance-budget culling, ACL compression — all 3D-skeletal.

---

## 7. Replay UI & cinematics — reserved (trigger: trailer capture / a story-driven demo)

**Shape — replay.** The mechanism exists from v0: the record→replay harness
(`DETERMINISM.md`: seed + initial snapshot + tick-stamped input stream + periodic keyframes +
hash log; mismatched build fingerprint refused loudly). This seam is only the layer on top:
- **Scrub / transport panel**: play / pause / speed / seek; seek = nearest keyframe + re-sim
  forward, bit-exact because it *is* a re-sim. The widget is §2's — built once, reused by
  cinematics authoring.
- **Free-cam spectator**: a synthetic camera entity driven by viewer input at render rate,
  never fed back to sim (the one-way render boundary). Camera is a plain component.
- **Clip export**: `(start_tick, end_tick, camera_track)` over a replay — re-sim the window,
  drive the camera, capture frames; encode via an **out-of-process CLI** (ffmpeg-class, in
  `tools/`, exempt from the subset). Never an in-runtime encoder.

**Shape — cinematics.** Track-based timeline (camera / animation / audio / event tracks),
keyframe columns SoA, evaluation at `t` = binary search + interpolate, zero-alloc.
- **Presentation tracks are render-rate** (camera, sprite anim, audio cues), non-authoritative,
  `f32` legal.
- **The event track goes through the tick-stamped command channel** — a cutscene that spawns
  or sets a flag is an external input like any other, never a branch on playback state.
- **Skip = event-track catch-up**: evaluate the event track fully in tick order, compressed in
  wall time; drop the presentation tracks. Sim state is identical watched or skipped.
- Not the dialogue graph (§9): a timeline is linear and time-driven; a graph is branching
  and predicate-driven. They compose (an event can start a dialogue node).

**Tidelock deltas.**
- Replay panel and transport are **ImGui (dev) first** — it is tooling; the §2 game-UI
  version exists only if a shipped game exposes replays to players.
- Cinematic timelines are **Luau data** (a table of tracks) interpreted by a C++ evaluator;
  keyframe times on the event track are **ticks**; presentation keyframes may be seconds.
- A 8-peer lockstep session plays a cinematic **per peer**: the event track's commands are
  issued by the *triggering* peer as ordinary inputs; other peers receive them through the
  input stream. No "cinematic state" is ever network state.
- Spectator of a live session = a peer that submits empty inputs; the free-cam is local.
- Clip export runs the re-sim in the **headless harness build** (no window) and writes raw
  frames to a ring on disk; the encoder CLI is invoked by a `tools/` script, never by the
  engine process. A failed encode is reported, never retried silently.
- Replay files embed the build fingerprint (`PIVOT-DESIGN.md` §8); the scrub panel refuses a
  mismatched file with the same named error the netcode handshake uses.

**Must not become.** A second recording mechanism; a playback approximation (every scrub is a
re-sim); a cinematic camera type; a coroutine / stackful script system for sequencing (the
Layr `IEnumerator` lesson — data-bound targets only).

**Ore-era detail dropped.** `Fork()` as a distinct primitive (tidelock's is the registered-
arena snapshot), multi-camera render-to-texture views, letterbox as a composition layer.

---

## 8. Modding — reserved (trigger: a shipped game committing to mods)

**Shape — the four-tier ladder.**

| Tier | What | Mechanism | Net-new |
|---|---|---|---|
| 1 Asset | swap textures / sounds | §10 pak mount layers `base → patch → DLC → mods`, last-wins | none |
| 2 Data | items, recipes, tile defs, clips | Luau data tables + stable-id registries | none |
| 3 Script | sandboxed logic | **a third Luau VM profile** | the real work: profile + metering + lifecycle |
| 4 Total conversion | a mod that is a new game | DLC-format package (pak + registries + catalog) | packaging only |

**Tidelock delta — the notable win: Luau IS a sandboxable VM.** The Ore design needed a new
typed-bytecode VM for tier 3. Tidelock does not: Luau is Roblox's production sandbox. A
restricted VM has no `os` / `io` / `debug` / `loadstring`; **memory limits** via the allocator
hook (`LUAU-LAYER.md`: the allocator is already ours); **instruction budget** via the interrupt
callback (over-budget = suspended and reported, never a hung frame); no native codegen.
Tier 3 is therefore two more library profiles on the VM we already embed:
- **`sim-mod`** = the restricted sim VM's library set **minus engine-internal bindings** (no
  arena access, no raw component pointers, no netcode/checkpoint verbs): det math, component
  read/write through the reflection glue, the ordered command channel, keyed RNG handed in,
  the event queue. Deterministic by the same construction as first-party sim scripts.
- **`client-mod`** = the UI VM subset: §2 draw primitives, read-only world access, audio cues;
  authoritative verbs **do not exist** in the profile (absence, not a runtime check).
- **Mod bytecode joins the build fingerprint** exactly like first-party sim scripts
  (`PIVOT-DESIGN.md` §8): mods + versions + settings fold into the handshake hash; a mismatch
  is the ordinary desync/handshake refusal. Every peer runs the same mod set or does not join.
- Mod storage is a component / singleton component in a registered arena (the state rule),
  so it rounds through save / rollback / replay with no mod-specific path.

**Lifecycle contract (Factorio's, adopted as-is):** `on_init` creates the mod's storage;
`on_load` is **read-only** (re-register handlers and caches, nothing else — anything more
desyncs); `on_config_changed` runs migrations, name-tracked, at most once. Missing-mod load is
explicit: quarantine the storage, clean orphaned references, never a silent skip.

**Multiplayer policy.** Lockstep co-op (the only netcode): mods are checksummed sim
participants; a joining peer reconstructs VM state from the checkpoint with `on_load` only.
There is no server-authoritative tier to design. Full-world visibility (`PIVOT-DESIGN.md` §8
R1) means a client-mod cannot leak hidden information — there is none.

**Distribution / SDK** — recorded, not decided: mods are cooked packages through §10 (the cook
validators run on every mod); the mod SDK is the dev build's ImGui shell + script reload; mod.io
/ Workshop are product decisions.

**Must not become.** A second VM or language; a per-mod snapshot path; an "unrestricted"
mod tier; a runtime permission check where a library-profile absence suffices; a mod API that
reaches Alloy internals (Alloy's query/command API is the ceiling).

**Ore-era detail dropped.** Own typed-bytecode VM + wasm3/WAMR challenger, `server-mod`
profile, WoW-model client/server split, Layr-blueprint inline VM debugger.

---

## 9. Game-logic substrate — reserved (trigger: Quench clearing its gate / an RPG-shaped demo)

**Shape — six primitives; games compose the systems.** Dialogue, quests, inventory, loot, and
combat are not engine subsystems; they decompose into:

| Primitive | Powers |
|---|---|
| Hierarchical tag registry | condition vocabulary for everything below |
| Predicate evaluator + namespaced state store | triggers, branches, quest conditions |
| Attribute + modifier aggregation | combat stats *and* equipment stats, one engine |
| Effects-as-data + native escape hatch | buffs / DoT / procs / status (burn, wet, poison) |
| Graph VM (Ink-model) | dialogue + quests, one machine |
| Deterministic loot + item def/instance | loot, inventory, vendors, crafting |

Build order when pulled: tags + predicate/state store (cheapest, broadest), then aggregation,
then effects, then graph VM and loot (heaviest, most content-shaped).

**Tidelock deltas.**
- **These are Luau-level libraries over POD components, not C++ subsystems** — unless
  profiling pulls one down (aggregation over thousands of entities per tick is the likely
  candidate; the graph VM never is). The litmus: a tag registry is `script/lib/tags.luau`.
- **State lives in components, never the Luau heap.** Tags are bitsets in a component;
  attributes are a fixed-width `fx` array component; the state store is a singleton
  component keyed by interned `StrId`; a dialogue cursor is `{graph_id, node, choice_mask}`
  in a component. Script-side tables are working data, rebuilt each tick.
- All arithmetic is `fx` (a modifier is `{add: fx, mul: fx}` applied in fixed stage order:
  base → add → mul → clamp); loot rolls use `rng_for(seed, tick, system_id, entity)`; no
  hash-part iteration — the sorted-map bindings are the ordered containers.
- The predicate evaluator's **dev split**: in dev builds predicates are Luau closures; in
  ship builds the same tables compile to the same bytecode — there is no second evaluator.
- Overburden's wetness / sabotage / "Cycle" flags are plain component fields now; the formal
  registry earns its place only with a second consumer (the promotion rule).

**Must not become.** A GAS clone; a C++ "RPG layer"; a quest/dialogue editor before a graph
consumer exists; a string-keyed runtime store inside the tick (interned ids only); a place
where game nouns (`mana`, `reagent`) appear in a promoted API.

**Ore-era detail dropped.** Comptime `@fields` registries, tagged-union effect policies,
name-keyed hot-reload function tables, server-authority split (§8 of the old area — no server).

---

## 10. Asset streaming & cook pipeline — reserved (trigger: a large-world game or the first shipped build)

**Shape — runtime.**
- **Async load state machine** `Unloaded → Queued → Loading → Resident → Unloading` behind the
  **same handle** the sync path returns today (`ASSETS-AND-DATA.md`): the handle is instant;
  the load runs on the low-priority IO job (`PIVOT-DESIGN.md` §12a); consumers tolerate
  not-resident via a placeholder; the frame never blocks.
- **Scope-owned lifetime**: a cell / level / chunk owns its assets and frees them as a unit;
  refcount only for cross-scope shared assets. Dev hot-reload republishes into the same slot.
- Cell residency + hysteresis islands for large worlds (the same shape as ALLOY §13 chunk
  streaming — one residency policy, two payload kinds).

**Shape — cook.**
- `import → cook → package`; input of record = source + committed `.meta` (GUID).
- **Cook key = hash(source + meta + importer/tool versions + platform + Σ dep hashes)** —
  versions-in-the-key is non-negotiable (the stale-cache bug, reinvented four times).
  **Content-addressed store** + action map → incremental, CI-shared, patch-ready.
- **Deterministic cooking** (no wall-clock, no iteration-order, no float-mode drift) +
  **cook-twice-diff CI gate** — what makes a shared cache *correct*.
- Validators as cook steps (budgets, lint, missing refs) fail the cook loudly, never the
  runtime quietly. Luau sim scripts are cooked to bytecode here and fingerprinted.
- **Pak**: TOC + block compression (zstd/LZ4, pure C), ordered **mount layers
  `base → patch → DLC → mods`**, last-wins by GUID; trimmed runtime catalog in the pak.

**Tidelock deltas.**
- **Offline tools are exempt from the C++ subset** (`tools/`, `BUILD.md`): the cooker may use
  anything — STL, Python, third-party CLIs. The runtime links only pure-C decoders.
- A load completion is an **external input**: "spawn-on-loaded" is a tick-stamped command, and
  the sim never branches on residency (the sealed-sim rule, `DETERMINISM.md`). In lockstep, a
  peer that has not finished loading **stalls the barrier**, it does not diverge.
- Cooked data tables carry their **fx format per column**; the `init()` validator checks
  ranges against the palette (`PIVOT-DESIGN.md` §3.1). Floats in a cooked sim table are a
  cook error.
- Handles stay `u16 <12,4>`; the async state is a parallel `u8` column, not a handle bit.
- Save games are not cooked assets (they are the M2 reflection encoder's output, `ECS.md`).
- Test artifacts (golden hash traces, replay benchmarks, fuzz corpora) are CAS content with
  hashes in git — the same store, not a second one.
- Pak reads go through the platform file seam (`PLATFORM.md`); the runtime pak reader is
  pure C-style C++ in the subset (it ships), the pak *writer* is a `tools/` program.

**Must not become.** A daemon + SQLite asset DB before a team exists; a prefab-override DOM
(spawns are Luau functions); a GPU-upload transfer thread (SDL_Render streams textures; §11
revisits); a runtime that reads source assets in ship builds.

**Ore-era detail dropped.** gltfpack / meshoptimizer / toktx, `server` cook target, async
copy-engine upload, o3de prefab DOM, `mesh/` primitives, Blender level-export sidecar.

---

## 11. Second render path: SDL_GPU — reserved (trigger: a shader a demo needs that SDL_Render cannot express)

**Shape.**
- Same seam: render2d's command buffer → **`submit_draw`** (`RENDER2D.md`); the backend
  behind it is SDL_Render at v0, SDL_GPU when pulled. Callers never see which.
- **Opaque material-first bucket** via the **reserved blend bit** in the 64-bit sort key:
  opaque quads sort by material then depth (state changes minimized); translucent quads sort
  painter's-order as today. The bit exists in the key layout now so promoting it is a
  comparator change, not a key re-layout.
- Shaders compiled **offline via SDL_shadercross** (`tools/`, exempt); the runtime loads
  precompiled blobs per backend from the cook (§10). No runtime shader compiler.
- `gpu` wrap: engine-typed handles, frames-in-flight ring + fence, a simple sub-allocator.
  No render graph — a static pass list with explicit barriers is enough for 2D.
- **Compute** reserved for a future **GPU-side sim view**: the CPU pixel buffer becomes a
  compute-written texture (e.g. SDF → material shading, liquid surface). The sim stays on
  the CPU; compute only *presents* it.

**Tidelock deltas.**
- Positions cross the seam as `f32` (render-side; `pos_t → f32` at extract). The GPU never
  sees `fx`.
- Shader sources are a new artifact class for the cook key; bytecode per platform is CAS
  content. Shaders are **not** in the build fingerprint (they cannot affect sim state).
- Steam Deck target: Vulkan via SDL_GPU; SDL_Render stays the fallback so a Vulkan driver
  regression on a low-end peer never blocks it (the Pi, which motivated this, left the program
  2026-08-25 — the reasoning stands for any weak GPU).
- ImGui moves from the SDLRenderer3 backend to the SDL_GPU backend in the same change.
- The two backends are a **conformance pair** in the harness: the same command buffer rendered
  by both must produce the same draw-call count and sort order (structural check, no GPU);
  pixel goldens are nightly, never PR-blocking (`TOOLING.md`).
- Build-time selection, not runtime: a build carries one backend; there is no `if (gpu)`.

**Must not become.** A 3D renderer; a render-graph DSL; bindless / GPU-driven culling; a
second sort key; a place where the sim reads back GPU results (compute output is never
authoritative).

**Ore-era detail dropped.** Forward+, Filament PBR, CSM, Hillaire sky, GI tiers, TAA, SSAO,
particles-on-compute, visibility buffer, reverse-Z, PSO cache — the entire 3D path.

---

## 12. Multi-window / editor shell — reserved (trigger: level authoring beyond Luau-scripted spawns)

**Shape.**
- **The shell is free**: Dear ImGui docking + multi-viewport already gives tear-off panels,
  N OS windows, layout persistence (`TOOLING.md`). The generic inspector, console, profiler,
  replay transport (§7) are ImGui panels over the X-macro reflection — they exist as dev
  tooling, not as "editor" work.
- What is reserved is the **editor world model**:
  - **Retained editor world**: the authored scene is ordinary world state in registered
    arenas, edited through the deferred command buffer (never mid-frame column pokes).
  - **Play-in-editor = a fork of the registered arena set**: snapshot the edit world
    (memcpy per arena, the M1 unit), play, restore on stop. "Keep changes" = a recorded
    migration applied back through the command channel.
  - **Undo stack**: transactional apply/revert commands (a gizmo drag is one step), a
    *different* recorder from the replay stream (bidirectional editor mutations vs
    forward-only sim inputs) — never conflate the two.
  - **Selection service**: single/multi-select + change events; gizmos (translate / scale,
    pick, pan/zoom) drawn through debug-draw.
- Editor input is an editor-only **context** reading the raw event stream at render rate
  (`INPUT.md`); play-in-viewport pushes a gameplay context that feeds the airlock.

**Tidelock deltas.**
- Dev builds only (`BUILD.md`): the editor, ImGui, and the undo stack compile out of ship and
  netcode-soak builds; there is no `{game, debug}` docking tier to maintain separately.
- Level content is **Luau data** (spawn tables, tile/clip tables): the editor's save path is
  "write the Luau table back" through the reflection encoder, so hand-edited and
  editor-edited levels are the same artifact.
- The Luau step-debugger (Tier 1, `PIVOT-DESIGN.md` §7) is a panel in this shell when it
  lands, not a separate tool.
- No retained UI toolkit is needed for the shell — the ImGui-for-dev / §2-for-game split
  holds; the Ore design's "editor is a game client of the retained UI" premise is void.

**Must not become.** A retained UI toolkit justified by the editor; a Qt/Avalonia island; an
editor that edits the Luau heap (edits are component writes); a second world representation
(prefab DOM, scene file format) — the world *is* the scene.

**Ore-era detail dropped.** Window-manager-over-N-swapchains, `{editor}` / `{game,debug}` /
`{game,release}` tiers, node-graph widget family, Blender export pipeline, shader text editor.

---

## 13. Floating origin / large-world coordinates — VOID

There is no origin to float. `pos_t = fx<i32,18>` spans ±8,192 m with a uniform 3.8 µm
quantum everywhere in the ±4,096 m world (`PIVOT-DESIGN.md` §3.1a/b): precision does not
decay with distance, so the f32-jitter problem the Ore C4 area solved cannot arise. Render
extract converts `pos_t → f32` relative to the camera each frame, which is all a 2D GPU path
needs. Beyond the extent is **ALLOY §13 streaming** (chunk residency, analytic idle), never a
coordinate scheme; raising the extent is a palette-row edit before saves exist and a migration
after. `on_origin_shift` does not exist in §3's interface. Sectors, rebasing barriers, and the
f64-storage split are not reserved — they are dropped.

---

## 14. Explicitly out

Not reserved, not to be re-proposed. **3D-only** (the Ore engine's bulk): PBR / IBL / GI
tiers (LPV, DDGI, SSGI) / CSM and point shadows / clustered-forward lighting / TAA / SSAO /
post-FX stack / particle-on-compute / 3D skeletal animation, IK, retargeting, morph targets,
motion matching / Recast-Detour navmesh, dtTileCache, HPA\*, ORCA / 3D heightmap terrain,
clipmaps, SVT, control maps, voxels / floating origin and sectors / cube-sphere cells and
planetoid seams / archetype ECS challenger and `(Relation, Target)` pairs / render-graph
auto-barriers, GPU-driven culling, bindless, visibility buffer / glTF, gltfpack,
meshoptimizer, `mesh/` primitives / snapshot-interpolation netcode, AoI, ghost priority,
voice chat. **Game-layer, never engine:** vehicles and traffic of any kind; director / pacing
AI; tactical / RTS strategy; camera effects (shake, confine, zones); combo detection; the
cellular / fluid / heat rules themselves (Alloy is the sim, games bring meaning). **Rejected
on the ruling:** a second scripting VM or language (Luau is the language; §8 adds profiles,
not VMs); a general heap allocator (arenas + `SlotMap` only — wanting one is a design smell);
a render-graph / shader DSL; a premade component kit; a general `String` class; std
distributions or libm on any sim path. **Commercial infra:** license / telemetry / AI-assistant
/ cloud-save / matchmaking / i18n — product work, designed when a product exists.

---

*Rev 1 — harvested 2026-08-22 from the Ore-era 3D ruling after the C++ pivot
(`PIVOT-DESIGN.md`). Every entry is complete as written; the next edit to any of them is a
trigger firing, not a design gap. Revisions bump this file's rev and name the consumer.*
