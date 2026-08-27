#pragma once
// ---------------------------------------------------------------------------------------------
// queue.h - DrawCommand, the 64-bit sort key pack/unpack, DrawData SoA, ClipTable, Batch,
//   RenderPacket: the data-only pieces render.h's RenderQueue is built from.
//
// Spec: docs/RENDER2D.md §3 (design - D9, one primitive), §9.2 (struct shapes, pinned verbatim -
//   key bit layout, LAYER/DEPTH enums), §9.3.4 (submission/reject algorithm - implemented in
//   queue.cpp), §9.3.5 (sort - batch.cpp calls foundation/sort.h's sort_u64_kv), §9.3.6 (scan-
//   batching - batch.cpp).
// Purpose: keys sort, draw data stays put - the render.h RenderQueue's Array<u64> keys column is
//   what sort_u64_kv touches; data_index/clip_id ride along as the kv-sort's value column
//   (docs/RENDER2D.md §3).
// Invariants: `material` = TexHandle.bits (null = untextured, vertex-colour only); `tiebreak` is
//   caller-supplied (default 0) - submission order is the final tiebreak BY SORT STABILITY, so 1M
//   commands with equal keys present in submission order without consuming bits. Ascending key =
//   draw order: lower depth draws first (back); `layer` is the top byte so all of layer 0
//   precedes layer 1 (one target switch per layer per frame). `depth` is NOT a batch key
//   (docs/RENDER2D.md §9.3.6) - consecutive equal (tex, layer, blend, clip) at different depths
//   merge into one batch, the sort having already fixed their order.
// Determinism: none - render-side only, f32/TexHandle throughout; never hashed (docs/RENDER2D.md
//   caption).
// Threading: none - value types and pure constexpr functions; RenderQueue itself (render.h) is
//   single-writer per frame, reset every render_present (docs/RENDER2D.md §9.4 step 7).
// Includes: foundation/{tl_types,rect,array}.h, platform/platform.h (TexHandle, DrawVertex).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/rect.h"
#include "foundation/array.h"
#include "platform/platform.h"

// key = [63:56] layer:8 | [55] blend:1 | [54:31] depth:24 | [30:15] material:16 | [14:0] tiebreak:15
struct DrawCommand { u64 key; u32 data_index; u16 clip_id; u16 _pad0; };   // 16 B
static_assert(sizeof(DrawCommand) == 16, "docs/RENDER2D.md section 9.2");

// Packs the sort key from its five fields (docs/RENDER2D.md §9.2); every field is masked to its
// width, so an over-wide caller argument cannot corrupt a neighbouring field.
constexpr u64 key_pack(u8 layer, u8 blend, u32 depth24, u16 material, u16 tiebreak15) {
    return (u64(layer) << 56) | (u64(blend & 1u) << 55) | (u64(depth24 & 0xFFFFFFu) << 31)
         | (u64(material) << 15) | u64(tiebreak15 & 0x7FFFu);
}
// Unpacks the layer byte (bits 63:56) - the top byte, so ascending key order groups by layer.
constexpr u8  key_layer(u64 k)    { return u8(k >> 56); }
// Unpacks the blend bit (bit 55, reserved for the future opaque/transparent split - docs/RENDER2D.md §3).
constexpr u8  key_blend(u64 k)    { return u8((k >> 55) & 1u); }
// Unpacks the 24-bit depth field (bits 54:31).
constexpr u32 key_depth(u64 k)    { return u32((k >> 31) & 0xFFFFFFu); }
// Unpacks the 16-bit material field (bits 30:15) - TexHandle.bits, or 0 for untextured.
constexpr u16 key_material(u64 k) { return u16((k >> 15) & 0xFFFFu); }
// Unpacks the 15-bit tiebreak field (bits 14:0) - caller-supplied, default 0.
constexpr u16 key_tiebreak(u64 k) { return u16(k & 0x7FFFu); }
static_assert(key_layer(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 0xAB, "docs/RENDER2D.md section 9.2");
static_assert(key_blend(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 1, "docs/RENDER2D.md section 9.2");
static_assert(key_depth(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 0x123456, "docs/RENDER2D.md section 9.2");
static_assert(key_material(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 0xBEEF, "docs/RENDER2D.md section 9.2");
static_assert(key_tiebreak(key_pack(0xAB, 1, 0x123456, 0xBEEF, 0x7ABC)) == 0x7ABC, "docs/RENDER2D.md section 9.2");

enum : u8  { LAYER_WORLD = 0, LAYER_UI = 1, LAYER_DEBUG = 2, MAX_LAYERS = 8, MAX_VIEWS = 4 };
enum : u32 { DEPTH_BASIN = 0x000000, DEPTH_CHUNK = 0x100000, DEPTH_BODY = 0x200000, DEPTH_PARTICLE = 0x300000,
             DEPTH_SPRITE_BASE = 0x800000 };   // sprite depth24 = DEPTH_SPRITE_BASE + i32(depth_bias) * 256

// DrawData flags (docs/RENDER2D.md §9.2).
enum : u8 { DRAWFLAG_FLIP_X = 1u << 0, DRAWFLAG_FLIP_Y = 1u << 1, DRAWFLAG_SCREEN_SPACE = 1u << 2, DRAWFLAG_NO_SNAP = 1u << 3 };

// SoA on main scratch, reserved once per frame (cap = cvar render_max_commands, default 65536).
struct DrawData {
    f32* x; f32* y; f32* rot_turns; f32* sx; f32* sy;   // centre + rotation + size in the command's space (world m, or target px if flags bit2)
    Rect_u16* uv; u32* rgba; u16* tex; u8* flags;        // flags: bit0 flip_x - bit1 flip_y - bit2 screen_space - bit3 no_snap
    u32 count, cap;
};

struct ClipTable { Rect_i32 rects[256]; u16 count; u16 stack[32]; u8 depth; u8 _pad0[3]; };   // id 0 = no clip; rects in target px of the command's layer

struct Batch { u16 tex; u16 clip; u8 blend; u8 layer; u16 _pad0; u32 first; u32 count; };   // 16 B; first/count index the sorted order
static_assert(sizeof(Batch) == 16, "docs/RENDER2D.md section 9.2");

struct RenderPacket { f32* x; f32* y; f32* rot_turns; f32* sx; f32* sy; u32 count; u32 _pad0; };   // parallel to the Transform column (index = Transform dense index)
