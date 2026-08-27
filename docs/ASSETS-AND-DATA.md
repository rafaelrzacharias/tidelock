# Assets, game data tables, saves (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §7. Carries D14 / C3-runtime; the data
> path is new (Luau-authored tables → compiled POD); saves are the reflection encoder (PIVOT §6).
> **Owns:** `src/core/assets.h`, `data_tables.h`, `save.h`; `tools/cook` later.
> **Build (2026-08-26, w3-assets-data):** §8.2/§8.3/§8.4's pseudocode-level structs got real
> construction signatures over the spec (the `slotmap_init`/`world_init` precedent - registry
> init, loader function shapes not threaded through `World`, `data_compile`'s explicit schema-list
> and `compile_pool` parameters, `SaveArenaDesc`/`SaveDesc`); the reconciliation is recorded in
> `TODO.md`'s W3 assets+data lane notes, not restated here. RR-21 (`TODO.md`, RULED) granted this
> lane a scoped exception to add a generic C++-side Luau table reader to `src/script/script.h`;
> `data_compile`'s body (integer/bool field kinds, scope noted at its RULED entry) now lives in
> `src/script/data_compile.cpp` - the module DAG (`ARCHITECTURE.md` §1) puts the only module that
> can drive both `core/`'s types and the Luau VM downstream of `core`, not inside it.

---

## 0. Three different things, three mechanisms

| Thing | Identity | Lifetime | Mechanism |
|---|---|---|---|
| **Assets** (textures, fonts, audio clips later) | name hash → `u16` handle | refcounted; load at startup / free at shutdown at v0 | slotmap + loaders (§1) |
| **Game data tables** (materials, species, reactions, recipes, tuning) | Luau table → compiled POD table in a registered arena | per world init; reloadable in dev as a sealed command | compiler + validator (§3) |
| **Saves / checkpoints** | file | durable | reflection encoder over registered arenas (§5) — *not* the memcpy snapshot |

---

## 1. Assets (DECIDED — D14 carried)

```cpp
Result<TexHandle>  asset_load_texture(World*, NameHash name);     // dedup by name hash → refcount++ ; sync stb_image → SDL texture
void               asset_release(World*, AssetHandle);             // refcount--; free at 0; generation catches use-after-free
const Texture*     asset_get(World*, TexHandle);                   // null if freed / wrong generation
```

- Engine-owned generational `SlotMap` per asset kind; per-domain `u16 (12+4)` handles
  (`MEMORY.md` §3). Components store handles, never paths or strings.
- Kinds at v0: `IMAGE` (stb_image → streaming or static texture), `FONT` (SDL_ttf face, dev text
  + later game text), `DATA` (a Luau file compiled by §3). Reserved: `AUDIO`, `SHADER`.
- Names resolve through a content root + the platform file seam (`PLATFORM.md` §3); the name
  hash is the cross-machine identity (save files and the wire carry hashes, never paths).
- **Hot-reload in place** (dev): a file watcher republishes into the same slot; holders see new
  content transparently. Reserved; cheap once the loader exists. **Async load** behind the same
  handle (`Unloaded → Queued → Loading → Resident`) is reserved for a streaming consumer — and in
  lockstep, "resident" is a sealed command: a peer still loading stalls the barrier, never
  diverges (`RESERVED-SEAMS.md` §10).
- The registry is not a registered arena (handles are stable for the run; the *contents* are not
  sim state). What the sim sees of an asset is its handle and, for data, the compiled table.

---

## 2. Streaming textures (the sim view — D9's one hard v0 need)

`TexHandle tex_streaming(World*, u16 w, u16 h, PixelFmt)` + `u8* tex_lock/unlock` — a texture whose
backing the render extract writes each frame from Alloy's SDF/material/particle views
(`RENDER2D.md` §5). Engine-owned; the sim never touches it.

---

## 3. Game data tables — Luau-authored, compiled at init (DECIDED)

**Alternatives weighed:**

| Option | Determinism | Performance | LOC / cognitive | Compile-time | Iteration |
|---|---|---|---|---|---|
| hand-written C++ constant tables | trivially identical per build | best | high per table; every tweak is a C++ rebuild | rebuild on tweak | worst |
| JSON/TOML files + a C++ parser | identical if the parser is; floats in JSON must be rejected | good | a parser + schema per table | none | good, but a second authoring language next to Luau |
| **Luau tables → validator → POD** (chosen) | identical: the compiled table is hashed into the fingerprint; Luau numbers are exact integers in range, fx rows are written as raw bits or as validated `"12.5"`-style scaled literals | POD arrays, id-indexed, in a registered arena — the sim never touches Luau | one compiler + per-table schema; the schema *is* the reflection field table | none | best — the same language as the game; dev reload is a sealed command |

Mechanism:

1. A table schema is a reflected POD row (`TL_COMPONENT`-style X-macro, or Luau-declared —
   `ECS.md` §6.1). Alloy ships its schemas (`SolidMaterial`, `LiquidSpecies`, …, `ALLOY.md` §11)
   in C++; a game declares its own tuning tables in Luau.
2. At `init`, the **data VM** (the unrestricted VM — tables are authored, not simulated) runs the
   data scripts, which return Luau tables. The compiler walks each row against the schema's
   `FieldInfo`: every field present or defaulted; integers in range; fx fields converted through
   `from_literal<R>` (a decimal string or an integer × scale, RNE to the row quantum — never a
   double round-trip) and range-checked against the palette row; references resolved by name to
   dense ids; **fail-loud** with table/row/field named.
3. Rows are written into `DataTables` in a registered arena (`HASHED | SNAPSHOT`), id-indexed,
   with a name→id `SortedMap` for Luau lookups. The cross-table validators (mass balance, hysteresis
   gap, divisor floors, `v_max` fold — `ALLOY.md` §11.1) run next.
4. `hash(DataTables)` joins the build fingerprint. Two peers with different data cannot handshake.
5. **Dev reload:** editing a data script and reloading recompiles the tables as a *sealed command*
   at the next barrier (it changes sim behaviour, so it is tick-stamped and recorded in the replay
   log — `DETERMINISM.md` §2.9). In a lockstep session a data reload is refused (it would change
   the fingerprint mid-session).

The Luau side sees tables read-only through bindings (`data.material("granite").density`), backed
by the POD rows, not by the original Luau table (which is discarded after compile — it is not
authoritative).

---

## 4. Definition vs instance

Tables hold shared **definitions**; components hold per-entity **state**; an entity references a
row by dense id (flyweight). The engine provides the machinery; the game declares the schemas and
never enumerates them in C++.

---

## 5. Saves — the reflection encoder (DECIDED — M2; "the disk half of gate Task A is answered by construction")

Distinct from the snapshot (`MEMORY.md` §5), which is a raw memcpy valid only for the exact build.
A **save** must survive a rebuild and a schema edit:

```
SaveFile = Header { magic, format_version, build_id[32], session_fingerprint[32], seed, tick u64,
                    name_table (interned names used by handles/ids), arena_count }
         + per registered arena: ArenaBlock { arena_name_hash, encoder_kind, byte_len, payload }
```

- **ECS columns** and any pool with a reflection table encode **name-keyed**: per component, a
  field list `(name_hash, kind)` followed by rows; the decoder matches by name hash, applies
  **alias entries** for renames and **declared defaults** for added fields, and rejects a field
  whose kind changed (that is an explicit, versioned migration function in C++, keyed by
  `format_version` — no generic `migrate(T)` magic).
- **Alloy pools** with their own layouts (SDF stores, graphs) encode through the same reflection
  tables (every pool row is a reflected struct) plus a per-pool WIRE_STRUCT header; undirtied
  terrain chunks are not stored (regenerated from seed, `ALLOY.md` §12).
- **Data tables** are not stored: the save records the data-script names + their hash; on load the
  tables are recompiled and the hash checked (mismatch = the save is from different content —
  fail loud with a named error; a content patch ships a migration or accepts the refusal).
- **Fail-loud** `LoadError` codes; no silent partial loads; write-temp → fsync → rename for the
  durable tier (`NETCODE.md` §11.4 — a torn checkpoint costs the colony under `Persistent`).
- Luau state is **not** in a save — by the state-boundary rule there is nothing to store.

Checkpoints for lockstep rejoin (`NETCODE.md` §11) use the *snapshot* path within a build and the
*save* path across builds; the chain records the fingerprint at each durable entry.

---

## 6. Tests

Asset: load/dedup/refcount/free/stale-handle; missing file → named error; malformed image →
named error (fuzz the stb path under ASan). Data: every validator rule has a failing fixture;
a compiled table hashes identically across two processes; fx literal conversion is RNE and
range-checked; reload-as-command appears in the replay log. Save: round-trip equality per arena;
rename via alias; added field via default; kind change → refusal; corrupted file → refusal;
cross-build load of a fixture saved by the previous commit (a nightly job).

---

## 7. Rulings (closed 2026-08-22 — nothing open)

- **R-1 The data VM is a throwaway VM per compile** — created, run, walked, destroyed. Fresh
  state per reload; nothing leaks between compiles; the UI VM never sees raw data tables.
- **R-2 fx literals in data are constructor calls with exactly-representable arguments**:
  `fx.pos(12.5)`, `fx.q(0.25)`. The binding receives the Luau number and accepts it only if
  `value × 2^FRAC` is an integer within the row's range (i.e. the literal is exactly
  representable at the row quantum); anything else is a named compile error naming table/row/
  field. Decimal strings are rejected (a second parser for no gain). Computed values must be
  built from integer arithmetic and `fx.raw(bits)` — the data author sees the quantum explicitly.

## 8. Implementation specification

### 8.1 Files

`core/assets.h/.cpp` (registry, `asset_load_*`, `asset_release`, `asset_get`), `core/loaders/image.cpp`
(stb_image → `DrawApi` texture), `core/loaders/font.cpp` (SDL_ttf face handle; glyph atlas lives in
`render/text.cpp`), `core/data_tables.h/.cpp` (the compiler/validator + `DataTables`),
`core/save.h/.cpp` (the save writer/reader over `core/encoder.cpp`).

### 8.2 Asset registry

```cpp
struct AssetRec { NameHash name; u32 refcount; u32 kind_specific /* texture: DrawApi texture id; font: face index */; u16 w, h; u8 kind; u8 state; u16 _pad; };
struct AssetRegistry { SlotMap<AssetRec, TexHandle> textures; SlotMap<AssetRec, FontHandle> fonts; Map<NameHash, u32> by_name; VMemArena arena; };
Result<TexHandle> asset_load_texture(World* w, NameHash name) {
    if (u32* h = map_get(by_name, name)) { rec.refcount++; return handle; }
    path = resolve(name)                                   // content root + interned name → StrView; ERR_ASSET_NOT_FOUND
    bytes = platform->file.read_all(path, main_scratch)    // ERR_FILE_*
    pixels = stbi_load_from_memory(...)                    // ERR_IMAGE_DECODE; 4 channels forced
    tex = draw.texture_create(w, h, FMT_RGBA8, /*streaming*/ false); draw.texture_upload(tex, pixels)
    stbi_image_free → pool; insert rec; map_put; return handle
}
```

`asset_release` decrements; at 0 destroys the texture and removes from both maps. `asset_get`
returns the record pointer or null (stale generation). Streaming textures (§2) are created through
the same registry with `kind = STREAMING` and no name.

### 8.3 Data-table compiler

```cpp
struct TableSchema { const ComponentInfo* row; NameHash table_name; u32 max_rows; u32 flags; };   // Alloy registers its schemas in C++; Luau via ecs.component-style declaration
struct DataTable  { const TableSchema* schema; u8* rows; u32 count; SortedMap<NameHash, u16> by_name; };
struct DataTables { VMemArena arena /* registered: HASHED|SNAPSHOT */; DataTable t[MAX_TABLES /*64*/]; u32 count; u64 hash; };
```

`data_compile(World*, Span<StrView> script_paths) → Result<DataTables*>`:

1. Create the data VM (`LUAU-LAYER.md` §1); run each script; each returns a table
   `{ <table_name> = { {name="granite", ...}, ... }, ... }`.
2. For each schema in registration order: fetch the Luau array by `table_name`; rows in array
   order; `count ≤ max_rows` else `ERR_DATA_TOO_MANY_ROWS`.
3. For each row: allocate `schema->row->size` zeroed bytes; for each `FieldInfo` (declaration
   order): look up the key by name; missing → the declared default (a per-field default table
   registered alongside the schema; no default → `ERR_DATA_MISSING_FIELD`); convert by kind:
   integer kinds require `x == floor(x)` and range; fx kinds accept only the `fx.<row>(literal)`
   userdata (carrying the raw bits, §7 R-2) or `fx.raw(bits)`; handle/ref kinds accept a string
   name resolved **after** all tables are loaded (pass 2) → dense id; `StrId` kinds intern.
   Errors name `table/row-name/field`.
4. Pass 2: resolve name references across tables (`melt_into = "lava"`), `ERR_DATA_DANGLING_REF`.
5. Cross-table validators registered by Alloy (`ALLOY.md` §11.1) and by the game run in order;
   any `ErrCode` aborts with table/row/field.
6. `hash = tl_hash64` over every table's rows in order (the rows are POD; names are not in the
   hash — only their resolved ids). Destroy the data VM.

`data.*` Luau bindings read rows by dense id through `FieldInfo` (`LUAU-LAYER.md` §10).

### 8.4 Save format (byte layout)

```
SaveHeader (WIRE_STRUCT, 160 B): magic "TLSV", format_version u32, build_id[32], session_fingerprint[32],
    seed u64, tick u64, session_model u32, origin u32, name_table_len u32, arena_count u32, flags u32, _pad
NameTable: name_table_len × { NameHash h; u16 len; char[len] }        // every interned name referenced by a StrId field
ArenaBlock × arena_count (registry order):
    { NameHash arena_id; u8 encoder_kind /* 0 = raw pool rows, 1 = reflected rows, 2 = ECS column, 3 = chunk store */; u8 _pad[3]; u32 byte_len; payload }
    reflected payload: { u32 field_count; field_count × { NameHash name; u8 kind; u8 _pad; u16 count; u32 size } ; u32 row_count; rows (packed per the field list, little-endian) }
    ECS column payload: reflected payload + entity list (u32 bits per row)
    chunk store payload (Alloy terrain): { u32 dirty_chunk_count; per chunk { i32 cx, cy; i16 sdf[128*128]; u8 material[128*128] } }
Trailer: crc32 over the whole file (header included; everything but the trailer's own 4 bytes)
```

Reconciled 2026-08-27 (round 1 review D6): this used to read "crc32 over everything after the
header", which is what `save_write`/`save_read` shipped — the 160-byte `SaveHeader` (`seed`,
`tick`, `format_version`, `origin`, `name_table_len`, `arena_count`) sat outside the integrity
check, so a single-bit flip in any of those fields loaded as `ERR_OK` with silently wrong values
(measured: a corrupted `tick` byte loaded successfully with the wrong tick). This was a design
defect in the format, not a code/doc mismatch — the doc and the code agreed, on the wrong window.
Both are fixed to the wording above in the same commit.

Reader: header checks (magic, version ≤ known, `build_id` differences allowed, `session_fingerprint`
differences allowed — this is the cross-build path; data-script hash must match after recompiling
the tables, else `ERR_SAVE_DATA_MISMATCH`; the header's own `arena_count` checked against
`MAX_ARENAS`, `ERR_SAVE_TOO_MANY_ARENAS` on excess — added 2026-08-27, round 2 review R1: this
file-supplied count drives the per-block decode loop into a fixed `MAX_ARENAS`-sized scratch
array, and the bound guarding it had only ever been enforced on the WRITER's `arena_descs.count`,
never on the reader's own file-supplied field); per block, decode by name: for each stored field find
the live field by `name` (then by alias table `{old_hash → new_hash}` registered per component);
kind mismatch → `ERR_SAVE_FIELD_KIND` (a versioned migration function may be registered for
`(component, format_version)` and runs instead); missing live field → skipped; missing stored field
→ default. Row-level copy is field-by-field through offsets, never memcpy of the row.

### 8.5 Tests (`tests/core/assets/`, `tests/core/data/`, `tests/core/save/`)

**Split by owner — RULED 2026-08-27** (`TODO.md` "THREE RULINGS 2026-08-27" #2, PR #14 round 2).
Rev 1 stated one done criterion for a spec section three lanes jointly own, naming tests and
error-code fixtures that cannot exist while their prerequisite lane is unlaunched. Both round 1
and round 2 judged every one of those gaps an honest deferral, not a defect (`CLAUDE.md`'s "no
speculative breadth" — compiling against an Alloy schema, or a font pipeline, that does not exist
yet is the Layr trap). Split below into what `assets+data` owns and has built, and what is
deferred, each clause naming the lane that closes it.

**Owned by `assets+data`, built (PR #14):**
- Assets: load/dedup/refcount/free/stale; missing file and malformed image → named errors;
  bad-arg empty-name refusal; streaming texture create/release
  (`tests/core/assets/assets.test.cpp`). Fuzzing the decoder path under ASan nightly is not yet
  wired into a CI leg — open, this lane's own scope, filed in `TODO.md`'s doc debt.
- Data: every error code `data_compile` can actually raise has a fixture (missing field,
  out-of-range int, unknown table, duplicate name, syntax error, too-many-rows, too-many-schemas);
  the single-process half of "two compiles hash identically" (source field order and Luau-table
  walk order both proven not to reach the hash) (`tests/core/data/data_compile.test.cpp`).
- Save: round-trip equality per arena (reflected singleton + `ECS_COLUMN`); rename via alias;
  added field via default; kind change → refusal and → migration fn path; truncated/corrupt/
  forged file refused (bad magic, CRC mismatch, header byte corruption, forged block `byte_len`,
  kind mismatch, bogus name-table length, forged `arena_count`) (`tests/core/save/save.test.cpp`).
  Nightly cross-build load of the previous commit's fixture is CI-tooling's mechanism to wire,
  not built by this lane.

**Deferred, each clause naming its owning lane:**
- fx literal acceptance/rejection table; reference resolution incl. forward refs —
  `ERR_DATA_BAD_FX_LITERAL` and `ERR_DATA_DANGLING_REF` are declared, `TL_FATAL`'d scope cuts with
  no Alloy schema to compile a real fixture against. Owner: **alloy-substrate** (schema + the
  fx/StrId/handle field kinds land there first).
- reload emits the sealed command — `data_compile` has no reload path; §10.8's reload surface is
  binding work this lane's header does not own. Owner: **luau-bindings**.
- `asset_load_font` (`loaders/font.cpp`, a declared `TL_FATAL`'d stub) — the font-load half.
  **No `ROADMAP.md` lane currently owns this** — render2d (PR #13) is merged and closed, never
  had font/text/glyph work in its Builds column, and shipped `render/text.cpp` as a deliberate
  reserved stub by its own done criterion (`RENDER2D.md` §9). §8.1 above already keeps
  `core/loaders/font.cpp` (the face-handle load) in this lane's own file table; only the glyph
  atlas is `render/text.cpp`'s. The deferral itself stands — `tl_core` links `stb`, not the
  vendored `sdl_ttf`, a build change outside this lane's slice (`src/core/CMakeLists.txt`) — but
  flagged rather than assigned, per `CLAUDE.md`'s "unknown constraint → say so; never invent
  one," same as the two-process clause below.
- `ERR_DATA_VALIDATOR` — a cross-table validator fixture, per its own header comment
  (`data_tables.h`: "a cross-table validator (`ALLOY.md` §11.1 / a game's own) rejected"). This
  waits on Alloy the same as the fx/ref clauses above, not on render2d — attributed to
  **alloy-substrate**, corrected from an earlier draft of this split that grouped it with
  `asset_load_font` only because both are `TL_FATAL`'d stubs sharing one bullet.
- "two compiles of the same scripts hash identically **in two processes**" — the single-process
  half is built (above); the cross-process half needs a process-spawn `PlatformApi` primitive
  that does not exist (`popen`/`fork` are neither portable nor sanctioned outside `platform/`).
  **No `ROADMAP.md` lane currently owns this** — flagged rather than assigned, per `CLAUDE.md`'s
  "unknown constraint → say so; never invent one." Not blocking: both reviews already judged
  deferring it honest.

*Rev 2 — 2026-08-27: split by owner, RULED (`TODO.md`, PR #14 round 2, ruling 2). Rev 1 —
2026-08-22.*
