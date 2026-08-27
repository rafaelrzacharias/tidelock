#pragma once
// ---------------------------------------------------------------------------------------------
// assets.h - AssetRegistry: name-hash-deduped, refcounted textures and fonts; sync loaders.
//
// Spec: docs/ASSETS-AND-DATA.md §1 (design, D14 carried), §8.1 (file layout), §8.2 (this header,
//   the build contract), §8.5 (tests); docs/CANON.md "Platform extras" (TexHandles are minted
//   by the platform DrawApi; the registry holds them, never a second id).
// Purpose: engine-owned generational handles for textures and fonts, deduped by name hash,
//   refcounted, freed at zero. Components store handles, never paths or strings (§1). Loaders
//   (loaders/image.cpp, loaders/font.cpp) are this header's only two implementation TUs beyond
//   assets.cpp itself.
// Invariants: the registry is NOT a registered arena - handles are stable for the run, but the
//   *contents* (pixels, glyphs) are not sim state (§1); nothing here is hashed or snapshotted.
//   `by_name` is shared across both kinds (textures and fonts occupy disjoint NameHash space by
//   convention - callers name assets by their file path, which already disambiguates kind by
//   extension) and stores a packed u32: kind in bits 16..31, the kind-specific handle's raw bits
//   (u16) in bits 0..15.
// Determinism: never read from a sim path (§1: "the sim never touches" asset contents); this
//   header is engine-side only (render/, editor/, app/), same firewall as platform.h.
// Threading: one AssetRegistry, one writer (main thread); no locking (docs/PLATFORM.md §0 draw
//   is main-thread-only, and asset loads route through it for texture_create/upload).
// Includes: core/reflect.h (FieldKind, the token-keyed kind constants this header adds for
//   Tex/Font), foundation/{slotmap,map,vmem_arena,strview}.h, platform.h (TexHandle, DrawApi,
//   FileApi, PixelFmt, TexUsage).
//
// Signatures refined over §8.2's pseudocode (recorded in TODO.md, W3 assets+data lane notes,
// the "signature added over spec" precedent slotmap_init/world_init/interner_init already set):
// the registry is not sim state and is not threaded through World (World carries no PlatformApi
// or AssetRegistry member, and this lane does not touch world.h - not its module); loaders take
// AssetRegistry*/PlatformApi*/a caller-owned scratch VMemArena* explicitly, the same shape
// ScriptVm* callers already use for an engine-side facility that is not World-reached. `kind` on
// AssetRec resolves which SlotMap a `by_name` hit belongs to (doc's "AssetHandle" spans two
// distinct C++ types, TexHandle and FontHandle - kept as two functions per verb rather than one
// tagged-union type, matching the closed-instantiation-set rule for handles, CPP-SUBSET.md §2).
// ---------------------------------------------------------------------------------------------
#include "core/reflect.h"
#include "foundation/slotmap.h"
#include "foundation/map.h"
#include "foundation/vmem_arena.h"
#include "foundation/strview.h"
#include "platform/platform.h"

// The assets module's ErrCode range is 0x033x (docs/CANON.md "Types": per-module ranges; core's
// other files hold 0x0301, 0x0311-0x0317, 0x0320-0x0322 - see reflect.h/world.h/encoder.h).
constexpr ErrCode ERR_ASSET_NOT_FOUND     = (ErrCode)0x0330;  // resolve(name) found no file under the content root
constexpr ErrCode ERR_ASSET_FILE_IO       = (ErrCode)0x0331;  // platform->file.read_all failed (wraps the ErrPlatform code)
constexpr ErrCode ERR_ASSET_IMAGE_DECODE  = (ErrCode)0x0332;  // stb_image rejected the bytes
constexpr ErrCode ERR_ASSET_FONT_DECODE   = (ErrCode)0x0333;  // SDL_ttf rejected the bytes
constexpr ErrCode ERR_ASSET_LIMIT         = (ErrCode)0x0334;  // the kind's handle domain (4096) is exhausted
constexpr ErrCode ERR_ASSET_BAD_ARG       = (ErrCode)0x0335;  // null/empty name, zero streaming dims

// Log-side name for an asset ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_asset_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_ASSET_NOT_FOUND ? "ERR_ASSET_NOT_FOUND"
         : e == ERR_ASSET_FILE_IO ? "ERR_ASSET_FILE_IO"
         : e == ERR_ASSET_IMAGE_DECODE ? "ERR_ASSET_IMAGE_DECODE"
         : e == ERR_ASSET_FONT_DECODE ? "ERR_ASSET_FONT_DECODE"
         : e == ERR_ASSET_LIMIT ? "ERR_ASSET_LIMIT"
         : e == ERR_ASSET_BAD_ARG ? "ERR_ASSET_BAD_ARG" : "ERR_?";
}

// FontHandle: the resource-handle shape (docs/CANON.md "Types": Handle<_, 12, 4>, u16). TexHandle
// is already minted by platform.h (the device owns textures); FontHandle is this lane's to mint.
struct FontTag;
typedef Handle<struct FontTag, 12, 4> FontHandle;
static_assert(sizeof(FontHandle) == 2, "docs/CANON.md: resource handles are u16");

// The token-keyed reflection kind constants for the handle domains this lane owns (reflect.h
// §10.2's K_Tex/K_Font rows already exist; the token constant beside the type is this owner's
// job per reflect.h's own comment). AUDIO/CLIP stay unbuilt - reserved, docs/ASSETS-AND-DATA.md
// §1 - so no tl_field_kind_AudioHandle/_ClipHandle constant exists yet either.
constexpr FieldKind tl_field_kind_TexHandle  = K_Tex;
constexpr FieldKind tl_field_kind_FontHandle = K_Font;

enum AssetKind : u8 { ASSET_IMAGE = 0, ASSET_FONT = 1, ASSET_STREAMING = 2 };

// v0 has one state: a load either fails (nothing recorded) or succeeds resident (§1's
// Unloaded/Queued/Loading/Resident state machine is reserved for the async streaming consumer).
enum AssetState : u8 { ASSET_STATE_RESIDENT = 0 };

// One registry slot (docs/ASSETS-AND-DATA.md §8.2, field for field). `kind_specific`: for
// ASSET_IMAGE/ASSET_STREAMING, the platform DrawApi's own TexHandle bits (the real device
// texture - this registry's own TexHandle, from slotmap_insert, is a DIFFERENT value in the
// same handle SHAPE, which is what "never a second id [TYPE]" means, docs/CANON.md); for
// ASSET_FONT, reserved for the face index loaders/font.cpp mints (not built until that loader
// lands - font faces are engine-owned in `render/text.cpp`'s glyph atlas per §8.1).
struct AssetRec {
    NameHash name;
    u32      refcount;
    u32      kind_specific;
    u16      w, h;
    u8       kind;
    u8       state;
    u16      _pad0;
};
static_assert(sizeof(AssetRec) == 24, "explicit padding (docs/CPP-SUBSET.md section 5)");
static_assert(__is_trivially_copyable(AssetRec), "");

// docs/ASSETS-AND-DATA.md §8.2. `arena` backs `by_name`'s growth only - the registry itself is
// not a registered arena (§1: handles are stable for the run, contents are not sim state).
struct AssetRegistry {
    SlotMap<AssetRec, TexHandle>  textures;
    SlotMap<AssetRec, FontHandle> fonts;
    Map<NameHash, u32>            by_name;   // packed: (kind << 16) | handle.bits
    VMemArena                     arena;
};

// Reserves the registry's arenas (textures: 4096 slots, fonts: 4096 slots, both Handle<_,12,4>'s
// full domain; by_name: a fixed-capacity map sized 2x the combined domain at load <= 0.75) under
// fixed literal NameHash ids ("assets.tex.slots" etc. - one process ever has one AssetRegistry,
// so a caller-supplied prefix buys nothing a literal doesn't already give, docs/CONTAINERS.md
// §8.6's "four distinct ids, not derived" - deriving four columns from one caller id is exactly
// what that rule warns against), and wires every member. Returns the first failing vmem/slotmap
// init's code.
ErrCode asset_registry_init(AssetRegistry* reg, const VMemApi* os);

// `name` is the asset's path relative to the content root (`sv_hash(name)` is the dedup key and
// the returned record's identity - §1 "the name hash is the cross-machine identity"; a save file
// or the wire carries only that hash, never this call's string, so this is the one door a path
// string crosses). Dedup by that hash (refcount++) or load from disk: platform->file.read_all
// into `scratch`, decode with stb_image (forced 4 channels), platform->draw.texture_create +
// texture_upload, insert a new AssetRec, refcount 1. Returns ERR_ASSET_NOT_FOUND/ERR_ASSET_FILE_IO
// (wrapping the ErrPlatform code) or ERR_ASSET_IMAGE_DECODE; ERR_ASSET_LIMIT if the texture
// domain is exhausted. Defined in loaders/image.cpp. `scratch` is rolled back by the caller (this
// call only pushes, never scopes).
Result<TexHandle> asset_load_texture(AssetRegistry* reg, const PlatformApi* platform,
                                     VMemArena* scratch, StrView name);

// Creates a streaming texture (docs/ASSETS-AND-DATA.md §2 - the sim view Alloy's render extract
// writes each frame) through the same registry, kind = ASSET_STREAMING, no name (never entered
// into `by_name` - `asset_release` still frees it by refcount reaching 0, but a second
// `asset_create_streaming` call for "the same" texture always allocates a fresh handle).
// ERR_ASSET_BAD_ARG on w == 0 || h == 0; ERR_ASSET_LIMIT if the texture domain is exhausted.
Result<TexHandle> asset_create_streaming(AssetRegistry* reg, const PlatformApi* platform,
                                         u16 w, u16 h, PixelFmt fmt);

// Dedup by name hash (refcount++) or load from disk: platform->file.read_all into `scratch`,
// hand the bytes to SDL_ttf (TTF_OpenFontFromMem-shaped), insert a new AssetRec, refcount 1.
// Returns ERR_ASSET_NOT_FOUND/ERR_ASSET_FILE_IO or ERR_ASSET_FONT_DECODE; ERR_ASSET_LIMIT if the
// font domain is exhausted. Defined in loaders/font.cpp.
//
// NOT YET IMPLEMENTED (recorded in TODO.md, W3 assets+data lane notes, the same "no real
// consumer yet" scope cut as save.h's SAVE_ENC_RAW_POOL/CHUNK_STORE): render/text.cpp (the
// glyph-atlas consumer this loader hands a face to, §8.1) does not exist yet - no render2d work
// has landed - and opening a real SDL_ttf face from `src/core` needs `tools/audit/includes.py`'s
// BACKEND_HEADERS widened for the "SDL_ttf" token (currently `src/platform/impl_sdl3` and
// `src/vendor_glue` only), a shared-gate-file edit this lane can make (cone discipline: "your
// OWN entries in the shared gate files... tools/audit allowlists") but which building against a
// guessed glyph-atlas shape would be the Layr trap either way. TL_FATAL stub until render2d
// lands render/text.cpp.
Result<FontHandle> asset_load_font(AssetRegistry* reg, const PlatformApi* platform,
                                   VMemArena* scratch, StrView name);

// Decrements h's refcount; at 0, destroys the platform texture (draw.texture_destroy) and
// removes the slot from both `textures` and (if named) `by_name`. A stale/null h is a no-op
// (TL_ASSERT in debug - a double-release is a caller bug, same shape as slotmap_remove).
void asset_release_texture(AssetRegistry* reg, const PlatformApi* platform, TexHandle h);

// The font twin of asset_release_texture. Font device teardown lands with loaders/font.cpp.
void asset_release_font(AssetRegistry* reg, const PlatformApi* platform, FontHandle h);

// Returns the record for h, or null if h is null, stale, or dead (queryable absence, no assert -
// docs/CPP-SUBSET.md §3). Pointer is valid until the next asset_load_*/asset_release_* call on
// this registry (a SlotMap grow can relocate the slots array).
const AssetRec* asset_get_texture(const AssetRegistry* reg, TexHandle h);

// The font twin of asset_get_texture.
const AssetRec* asset_get_font(const AssetRegistry* reg, FontHandle h);
