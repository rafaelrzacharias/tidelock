#pragma once
// ---------------------------------------------------------------------------------------------
// dotpath.h - resolve/read/write a dot-path string against a World (the inspector/watch/console
//   introspection substrate).
//
// Spec: docs/TOOLING.md §3 (design: "dot-path introspection reuses the reflection walker"),
//   §9.3.6 (this file's algorithm).
// Purpose: `dotpath_resolve(w, "player.Transform.x")` turns a string into a `PathRef` (the
//   walker's own coordinates: entity, component, field index, array element) ONCE; every reader
//   (console `get`, a watch overlay, the inspector's "go to") reuses the tuple instead of
//   re-parsing the string every frame (this header's contract block below, and `watch.h`'s own).
// Invariants: three token forms (docs/TOOLING.md §9.3.6, examples): `player.Transform.x` (a
//   `Name`-resolved entity: token 0 an arbitrary name, `ERR_PATH_NO_ENTITY` if unknown or
//   ambiguous - ties are refused, never resolved to "the first match"); `#12.Health.hp` (token 0
//   `#<index>` - the CURRENT generation at that index, whatever generation was typed or omitted,
//   since a dot-path is a human's live-debugging tool, not a stored handle); `@PeerSlots.
//   local_slot` (a SINGLETON path - exactly two tokens, no entity token at all: token 0 names
//   the singleton component directly). Field token: a name, optionally followed by `[k]` for an
//   array field's element `k` (`a.Flags.bits[3]`); `k` out of `[0, field.count)` is
//   `ERR_PATH_NO_FIELD`, matching an unknown field name (both are "this path token does not
//   resolve", not two different failure classes).
// Determinism: pure read-time resolution against the World's registration tables and (for the
//   name-resolution form) the current column contents - never itself sim state, never hashed.
//   Writes go through `world_set_field_cmd` (the sealed command channel, docs/ECS.md §4), never
//   a direct memory poke - `dotpath_set_raw` refuses under `lockstep` exactly like
//   `console_exec`'s `SIM_AFFECTING` refusal, with the SAME reasoning: an editor connected to a
//   live lockstep session must not be able to silently fork one peer's state.
// Threading: none - a `PathRef` is a plain value; resolution and read/write are single-threaded
//   calls against a `World*` the caller already owns.
// Includes: foundation/tl_types.h, foundation/strview.h, core/world.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/strview.h"
#include "core/world.h"

// editor module's dotpath sub-range (0x039x; see core/cvar.h's contract block for the rest of
// the 0x03xx block's layout).
constexpr ErrCode ERR_PATH_SYNTAX       = (ErrCode)0x0390;  // wrong token count for the form implied by token 0
constexpr ErrCode ERR_PATH_NO_ENTITY    = (ErrCode)0x0391;  // #index out of the live domain, or a name that is unknown/ambiguous
constexpr ErrCode ERR_PATH_NO_COMPONENT = (ErrCode)0x0392;  // unregistered component name, or @name is not a singleton
constexpr ErrCode ERR_PATH_NO_FIELD     = (ErrCode)0x0393;  // unknown field name, or [k] out of [0, field.count)
constexpr ErrCode ERR_PATH_LOCKSTEP     = (ErrCode)0x0394;  // dotpath_set_raw refused under a lockstep session

// The walker's own coordinates (docs/TOOLING.md §9.3.6): `e` is null (`handle_is_null`) for a
// singleton path - there is no entity to name. `field`/`elem` index `w->comps[comp].info->
// fields[field]`, element `elem` of it (0 for a non-array field).
struct PathRef {
    Entity       e;
    ComponentId  comp;
    u16          field;
    u16          elem;
};

// Resolves `path` (docs/TOOLING.md §9.3.6's three token forms - see this header's contract
// block). Pure: touches no sim state, records no command. `ERR_PATH_SYNTAX` on a malformed
// token count or an unparseable `#<index>`/`[k]`; the other three ErrCodes per this header's
// contract block.
Result<PathRef> dotpath_resolve(World* w, StrView path);

// Reads `ref`'s field bytes into `out` (capacity `out_cap`; TL_CHECK: `out_cap >=` the field's
// element size). Returns the element's byte size (never partial - a too-small `out_cap` is a
// caller bug, TL_CHECK, not a truncation). For a per-entity (non-singleton) `ref`, a dead/stale
// `ref.e` reads as `ERR_PATH_NO_ENTITY` (the queryable-absence idiom, docs/CONTAINERS.md §8.2 -
// exactly what a `watch` uses to detect "re-resolve me", per this header's contract block).
Result<u32> dotpath_get_raw(World* w, PathRef ref, void* out, u32 out_cap);

// Records `world_set_field_cmd` for `ref` with `bytes`/`len` (TL_CHECK: `len` equals the
// field's element size - the same contract `world_set_field_cmd` itself enforces). Refuses with
// `ERR_PATH_LOCKSTEP` when `lockstep` is true (docs/TOOLING.md §0 "tools never poke sim state" -
// the same reasoning as `console_exec`'s `SIM_AFFECTING` refusal); a dead/stale `ref.e` still
// records the command (matching `world_set_field_cmd`'s own contract - the barrier applier is
// what TL_CHECKs liveness, not this door).
ErrCode dotpath_set_raw(World* w, PathRef ref, bool lockstep, const void* bytes, u32 len);
