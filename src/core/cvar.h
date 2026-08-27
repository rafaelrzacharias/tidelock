#pragma once
// ---------------------------------------------------------------------------------------------
// cvar.h - TL_CVAR, CvarDesc/CvarTable, cvar_get_*/cvar_set_*: the reflected cvar table.
//
// Spec: docs/TOOLING.md §3 (design), §9.1 (macro text - amended: flags added to TL_CVAR's
//   parameter list, reconciling this doc's own contradiction with docs/CPP-SUBSET.md §7b's
//   catalogue row `TL_CVAR(type, name, default, flags, help)`, which the rev-1 §9.1 expansion
//   silently dropped, hard-coding every cvar's flags to 0; the doc-integrity finding is recorded
//   in TODO.md), §9.2 (CvarKind/CvarDesc/CvarTable, this header's structs); docs/CANON.md
//   "Cvars" (the `module.name` lowercase-dotted naming convention, the closed rev-1 SIM set);
//   docs/CPP-SUBSET.md §3 (Result<T>/ErrCode), §7b (the macro catalogue row).
// Purpose: a caller-owned, reflected table of named/typed/flagged config values, registered at
//   init from constexpr CvarDesc rows (TL_CVAR) and read/written by name hash from the console,
//   the inspector, or C++ callers. No global table anywhere (docs/CPP-SUBSET.md §1's static-
//   mutable ban) - the caller supplies and owns the CvarTable instance, same shape as
//   ConsoleCmd/CvarTable's own doc precedent ("in World (non-registered arena)", TOOLING.md
//   §9.1); this lane does not itself add a `cvars` field to core/world.h (an ecs-lane file,
//   merged and closed - cone discipline, docs/ROADMAP.md §0 rule 2) since nothing here requires
//   the table to live inside World specifically - a future World integration is a one-line
//   pointer addition, filed in TODO.md as this lane's own follow-up, not a blocker for the table
//   itself.
// Invariants: `CVAR_TABLE_CAP` (256) live cvars; registration keeps `sorted` as a key-ascending
//   index over `desc`/`bits` (insertion-sort on register - init-only, not perf-sensitive) so
//   lookup and completion (TOOLING.md §9.3.5) both binary-search it. A name collision (two
//   registrations with the same `key`) is TL_FATAL, matching every other registration door in
//   the tree (reflect.h's TL_COMPONENT precedent) - init-time misconfiguration, not a runtime
//   error. `default_bits` is the raw bit pattern for the cvar's kind (an f32's IEEE754 bits, a
//   bool's 0/1, an fx row's raw i32) - never a typed union member, so CvarDesc/CvarTable carry
//   no float TOKEN even though CVAR_F32 cvars exist (core/ is not a sim TU - docs/CPP-SUBSET.md
//   §1's float ban is scoped to `src/sim/` and the det half of `src/foundation/` only; core/
//   already carries real `f32` members, e.g. core/action_map.h's Binding::sensitivity).
// Determinism: a SIM-flagged cvar's raw bits are part of session_fingerprint (docs/CANON.md
//   "Cvars"); folding them into that hash is NOT this module's job (docs/BUILD.md §5 owns the
//   fingerprint, not built yet) - this module only exposes `cvar_sim_fold_bits` as the seam a
//   future fingerprint pass calls, in registration order. `cvar_set_raw` (the ordinary path)
//   REFUSES a SIM-flagged key outright (ERR_CVAR_SIM_UNROUTED): docs/TOOLING.md §3 says a SIM
//   cvar's change must be a sealed, tick-stamped command (`CMD_SET_CVAR`) so a lockstep session
//   can refuse it and every peer stays byte-identical - but `CMD_SET_CVAR` does not exist in
//   core/commands.h yet (that enum and its applier are an ecs-lane file, out of this lane's
//   cone; the addition is filed as a ruling request in TODO.md). `cvar_apply_sim_raw` is the
//   seam a future `CMD_SET_CVAR` applier calls once that command exists - naming makes plain
//   that no other caller may reach it. A non-SIM cvar is ordinary config and never enters any
//   hash. No cvar value is ever read on a sim path outside the sealed-command boundary above.
// Threading: none - a CvarTable is caller-owned; the caller supplies (and, if it matters to the
//   caller, synchronizes) its own instance, exactly like ConsoleCmd's table.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/hash.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/hash.h"

// docs/TOOLING.md §9.2, verbatim.
enum CvarKind : u8 { CVAR_I32, CVAR_U32, CVAR_F32, CVAR_BOOL, CVAR_FX_RAW };
enum : u8 { CVAR_ARCHIVE = 1, CVAR_CHEAT = 2, CVAR_READONLY = 4, CVAR_SIM = 8 };

// core module's cvar sub-range (0x036x; the 0x03xx block is core's - docs/core/reflect.h's own
// comment names the module ranges; 0x030x reflect, 0x031x ecs, 0x032x encoder/recorder (NOTE:
// these two collide at 0x0321/0x0322 - a pre-existing cross-lane bug, out of this lane's cone,
// filed in TODO.md rather than fixed here), 0x033x assets, 0x034x data_tables, 0x035x save).
constexpr ErrCode ERR_CVAR_NOT_FOUND     = (ErrCode)0x0360;  // no cvar registered under that key
constexpr ErrCode ERR_CVAR_DUPLICATE     = (ErrCode)0x0361;  // key already registered (TL_FATAL site, listed for completeness)
constexpr ErrCode ERR_CVAR_TABLE_FULL    = (ErrCode)0x0362;  // CVAR_TABLE_CAP registrations already live (TL_FATAL site)
constexpr ErrCode ERR_CVAR_READONLY      = (ErrCode)0x0363;  // CVAR_READONLY flag set; write refused
constexpr ErrCode ERR_CVAR_SIM_UNROUTED  = (ErrCode)0x0364;  // CVAR_SIM flag set; must go through CMD_SET_CVAR (not yet wired)
constexpr ErrCode ERR_CVAR_KIND_MISMATCH = (ErrCode)0x0365;  // typed accessor kind != the registered CvarDesc::kind
constexpr ErrCode ERR_CVAR_PARSE         = (ErrCode)0x0366;  // console `set` value did not parse for the cvar's kind

// The literal name of one of this header's ErrCodes (or "ERR_OK"/"ERR_?"), for logging. Pure.
inline const char* cvar_err_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_CVAR_NOT_FOUND ? "ERR_CVAR_NOT_FOUND"
         : e == ERR_CVAR_DUPLICATE ? "ERR_CVAR_DUPLICATE"
         : e == ERR_CVAR_TABLE_FULL ? "ERR_CVAR_TABLE_FULL"
         : e == ERR_CVAR_READONLY ? "ERR_CVAR_READONLY"
         : e == ERR_CVAR_SIM_UNROUTED ? "ERR_CVAR_SIM_UNROUTED"
         : e == ERR_CVAR_KIND_MISMATCH ? "ERR_CVAR_KIND_MISMATCH"
         : e == ERR_CVAR_PARSE ? "ERR_CVAR_PARSE" : "ERR_?";
}

// docs/TOOLING.md §9.2: `NameHash key; const char* name; const char* help; u32 default_bits;
// u8 kind; u8 flags; u8 frac_bits; u8 _pad0;` - 32 B (two pointers + NameHash u64 = 24, +8).
struct CvarDesc {
    NameHash    key;
    const char* name;
    const char* help;
    u32         default_bits;
    u8          kind;    // CvarKind
    u8          flags;   // CVAR_ARCHIVE | CVAR_CHEAT | CVAR_READONLY | CVAR_SIM
    u8          frac_bits;
    u8          _pad0;
};
static_assert(sizeof(CvarDesc) == 32, "docs/TOOLING.md section 9.2");

enum { CVAR_TABLE_CAP = 256 };

// docs/TOOLING.md §9.2. `sorted` is a key-ascending index over `desc`/`bits`, maintained by
// cvar_register (insertion sort - registration is init-only). `bits` holds each cvar's CURRENT
// value (the desc's own `default_bits` is never mutated after registration).
struct CvarTable {
    const CvarDesc* desc[CVAR_TABLE_CAP];
    u32             bits[CVAR_TABLE_CAP];
    u16             sorted[CVAR_TABLE_CAP];
    u32             count;
    u32             _pad0;
};

// --- registration ------------------------------------------------------------------------------

// Zero-initializes `t` (count = 0). Call once before any cvar_register.
void cvar_table_init(CvarTable* t);

// Registers `desc` (a constexpr TL_CVAR row, or a module's array of them, one call per row):
// current value = default_bits; inserted into `sorted` at its key-ascending position.
// TL_FATAL: table full (ERR_CVAR_TABLE_FULL's condition), duplicate key (ERR_CVAR_DUPLICATE's
// condition) - both are init-time misconfiguration, the same class TL_COMPONENT already treats
// as fatal (reflect.h precedent), not a Result-returning path.
void cvar_register(CvarTable* t, const CvarDesc* desc);

// Binary search on `sorted` by key. Null when not registered. Pure.
const CvarDesc* cvar_find(const CvarTable* t, NameHash key);
// The dense index of `key` in desc/bits (NOT `sorted`'s position), or CVAR_TABLE_CAP when
// unknown - the shared lookup cvar_get_raw/cvar_set_raw and the console completion walk use.
u32 cvar_find_index(const CvarTable* t, NameHash key);

// --- typed default/bit conversion (thin wrappers - docs/CPP-SUBSET.md section 2's sanctioned
//     "typed wrapper over a type-erased call" shape, the world_get<T> precedent) --------------

template <typename T> constexpr CvarKind cvar_kind_of();
template <> constexpr CvarKind cvar_kind_of<i32>()  { return CVAR_I32; }
template <> constexpr CvarKind cvar_kind_of<u32>()  { return CVAR_U32; }
template <> constexpr CvarKind cvar_kind_of<f32>()  { return CVAR_F32; }
template <> constexpr CvarKind cvar_kind_of<bool>() { return CVAR_BOOL; }

// `__builtin_bit_cast` (docs/CPP-SUBSET.md section 1: "type traits come from clang builtins") -
// constexpr, so a TL_CVAR row's default_bits computes at compile time with no runtime memcpy.
template <typename T> constexpr u32 cvar_bits_of(T v) { return __builtin_bit_cast(u32, v); }
template <> constexpr u32 cvar_bits_of<bool>(bool v)  { return v ? 1u : 0u; }

// docs/TOOLING.md §9.1, amended: flags is now a parameter (see this header's contract block) -
// `TL_CVAR(f32, render_zoom, 1.0f, 0, "camera zoom")` / `TL_CVAR(bool, net_speculation, false,
// CVAR_SIM, "speculative execution")`.
#define TL_CVAR(type, name, def, flags_, help) \
    constexpr CvarDesc CVAR_##name = { #name##_id, #name, help, \
        cvar_bits_of<type>((type)(def)), (u8)cvar_kind_of<type>(), (u8)(flags_), 0, 0 }

// --- typed access --------------------------------------------------------------------------

// The raw current bits of `key`. TL_CHECK: registered (callers that must not fatal use
// cvar_find first). Pure.
u32 cvar_get_raw(const CvarTable* t, NameHash key);
// `key`'s current value. TL_CHECK: registered AND its CvarDesc::kind == CVAR_I32 (a mismatched
// kind is a programmer bug, not a runtime error - fatal, matching the other typed getters below).
i32 cvar_get_i32(const CvarTable* t, NameHash key);
// Same contract as cvar_get_i32, for CVAR_U32.
u32 cvar_get_u32(const CvarTable* t, NameHash key);
// Same contract as cvar_get_i32, for CVAR_F32.
f32 cvar_get_f32(const CvarTable* t, NameHash key);
// Same contract as cvar_get_i32, for CVAR_BOOL (bits != 0).
bool cvar_get_bool(const CvarTable* t, NameHash key);
// `key`'s current raw i32 value plus its registered FRAC bit count (CvarDesc::frac_bits) into
// `out_frac`. TL_CHECK: registered AND kind == CVAR_FX_RAW.
i32 cvar_get_fx_raw(const CvarTable* t, NameHash key, u8* out_frac);

// Sets `key`'s current bits directly, unconditionally (skips the READONLY/SIM checks below).
// Callers: cvar_set_raw (below, after its checks pass) and cvar_apply_sim_raw (this header's
// contract block - the CMD_SET_CVAR applier seam). Never called from console/inspector code
// directly. ERR_CVAR_NOT_FOUND if `key` is unregistered.
ErrCode cvar_set_bits_unchecked(CvarTable* t, NameHash key, u32 bits);

// The ordinary write path (console `set`, inspector edit, C++ caller): ERR_CVAR_NOT_FOUND,
// ERR_CVAR_READONLY (CVAR_READONLY flag set), or ERR_CVAR_SIM_UNROUTED (CVAR_SIM flag set - see
// this header's Determinism note); otherwise cvar_set_bits_unchecked.
ErrCode cvar_set_raw(CvarTable* t, NameHash key, u32 bits);
// Typed wrapper over cvar_set_raw (same ErrCode set: ERR_CVAR_NOT_FOUND, ERR_CVAR_READONLY,
// ERR_CVAR_SIM_UNROUTED). TL_CHECK: registered AND kind == CVAR_I32.
ErrCode cvar_set_i32(CvarTable* t, NameHash key, i32 v);
// Same contract as cvar_set_i32, for CVAR_U32.
ErrCode cvar_set_u32(CvarTable* t, NameHash key, u32 v);
// Same contract as cvar_set_i32, for CVAR_F32.
ErrCode cvar_set_f32(CvarTable* t, NameHash key, f32 v);
// Same contract as cvar_set_i32, for CVAR_BOOL.
ErrCode cvar_set_bool(CvarTable* t, NameHash key, bool v);

// The CMD_SET_CVAR applier's seam (docs/TOOLING.md §3): bypasses the SIM refusal (the sealed
// command already carries the routing guarantee), still refuses ERR_CVAR_READONLY. NOT callable
// from console/inspector code - those go through cvar_set_raw, which refuses SIM outright until
// a real CMD_SET_CVAR applier exists to call this (TODO.md ruling request).
ErrCode cvar_apply_sim_raw(CvarTable* t, NameHash key, u32 bits);

// --- fingerprint fold (docs/CANON.md "Cvars": SIM cvars are part of session_fingerprint) ------

// Folds every CVAR_SIM-flagged cvar's (key, bits) pair into `h`, in `sorted` (key-ascending,
// hence registration-independent, deterministic) order - a pure function of the table's current
// SIM values. The caller (docs/BUILD.md §5's fingerprint computation, not built yet) owns the
// hash algorithm and the surrounding fold; this is the one place that iteration order is fixed.
u64 cvar_sim_fold_bits(const CvarTable* t, u64 h);

// --- console/archive formatting (docs/TOOLING.md §9.3.5's `set <name> <value>` parse, §3's
//     ARCHIVE persistence to `pref_path/cvars.txt`; both pure - no FileApi/platform dependency
//     here, matching this header's Threading note) -------------------------------------------

// Formats `key`'s current value per its kind into `out` (NUL-terminated): an i32/u32 as a
// decimal literal, an f32 as `%.9g`, a bool as "0"/"1", an fx row as `raw:<i32>`. Returns bytes
// written excluding the NUL, or 0 if `out_cap` is too small (never partial-writes past `out_cap`
// - `TL_CHECK(out_cap > 0)`) or `key` is unregistered.
u32 cvar_format(const CvarTable* t, NameHash key, char* out, u32 out_cap);

// Parses `value` per `key`'s registered kind and calls cvar_set_raw (so READONLY/SIM refusal
// applies identically to a console `set`): CVAR_FX_RAW accepts `raw:<i32>` only (the "or a
// decimal literal quantized RNE" half of docs/TOOLING.md §9.3.5 needs FX-PALETTE.md's
// quantizer, not available to this pure module - TODO.md notes the gap). ERR_CVAR_PARSE on a
// malformed `value` (empty, trailing garbage, out-of-range integer, non-"0"/"1"/"true"/"false"
// bool) - the underlying ERR_CVAR_NOT_FOUND/READONLY/SIM_UNROUTED still surface first.
ErrCode cvar_parse_and_set(CvarTable* t, NameHash key, const char* value);
