# Assets, game data tables, saves (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. **DECIDED** except §7. Carries D14 / C3-runtime; the data
> path is new (Luau-authored tables → compiled POD); saves are the reflection encoder (PIVOT §6).
> **Owns:** `src/core/assets.h`, `data_tables.h`, `save.h`; `tools/cook` later.

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
SaveFile = Header { magic, format_version, build_fingerprint, fx_palette_rev, seed, tick,
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

## 7. Open

- **O-1** Whether the data VM is the editor/UI VM or a third, throwaway VM per compile. Lean:
  a throwaway VM (fresh state per compile; nothing leaks between reloads; it is destroyed after
  the POD is built).
- **O-2** fx literals in Luau data: `"12.5"` strings vs `fx.pos(12.5)` constructor calls (which
  would pass a double through Luau — exact for such literals but a trap for computed values).
  Lean: constructor calls are allowed only with literal arguments; the compiler rejects a
  non-literal double (it can tell — the binding receives the Luau number and refuses anything
  not exactly representable at the row quantum).

*Rev 1 — 2026-08-22.*
