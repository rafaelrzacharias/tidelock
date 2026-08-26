#pragma once
// ---------------------------------------------------------------------------------------------
// wire.h - the netcode wire vocabulary: the §20 constants, the varint/zigzag helpers the column
//   codec is written in, and (below, from the struct section) every TL_WIRE_STRUCT of §20.2.
//
// Spec: docs/NETCODE.md §20.1 (this file's contents), §20.2 (the structs and the read/refuse
//   rules), §20.2.2 (the varint/zigzag definitions and the column byte layout), §20 preamble
//   (the constants decided there); docs/CANON.md (MAX_PEERS and the tunables it owns);
//   docs/CPP-SUBSET.md §9 R-2 (wire structs are written/read through the little-endian byte
//   pair, never memcpy of the struct).
// Purpose: one home for the bytes that cross the network, so the encoder, the archive and the
//   (later-phase) transport all agree on layout by construction rather than by convention.
// Invariants: every wire struct leads with u32 format_version == NET_FORMAT_VERSION; every gap
//   is a named _padN and readers refuse a nonzero one; a reader refuses a format_version newer
//   than NET_FORMAT_VERSION (the POLICY is the caller's - reflect.h's generated reader hands the
//   decoded value back, so wire_check_version below is the one place that policy is spelled).
//   Varints are LEB128, 7 bits per byte, 0x80 continuation, <= 5 bytes for a u32; a 6th
//   continuation byte or a value that would exceed 32 bits is malformed, not truncated.
// Determinism: pure functions over caller memory; no io, no alloc, no floats, no wall clock.
//   Every operation is an integer identity, so encode->decode is lossless by construction
//   (docs/NETCODE.md §20.3(a)) and byte-identical on every target in the CANON.md matrix.
// Threading: values only; one writer/reader per thread. `net` is outside the sim boundary
//   (docs/NETCODE.md §20) - nothing here is hashed into world state.
// Includes: foundation/tl_types.h, foundation/bytes.h, foundation/tl_assert.h, core/reflect.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/bytes.h"
#include "core/reflect.h"

// --- ErrCode range ---------------------------------------------------------------------------
// net holds 0x04xx (docs/NETCODE.md §20; mem 0x01xx, jobs 0x02xx, bytes 0x06xx, alloy 0x0Axx).
// Truncation is bytes' code (ERR_BYTES_TRUNCATED) and arrives through the reader's sticky err;
// the codes here are malformed CONTENT, which only a reader that knows the format can name.
constexpr ErrCode ERR_NET_MALFORMED       = (ErrCode)0x0401;  // a field's value cannot occur in a valid stream
constexpr ErrCode ERR_NET_VERSION         = (ErrCode)0x0402;  // format_version newer than this build understands
constexpr ErrCode ERR_NET_PAD_NONZERO     = (ErrCode)0x0403;  // a _padN byte was not zero (docs/NETCODE.md §20.2)
constexpr ErrCode ERR_NET_VARINT_OVERFLOW = (ErrCode)0x0404;  // a uvarint ran past the width of its target

// Log-side name for a net ErrCode; "ERR_?" outside the net range.
constexpr const char* err_net_name(ErrCode e) {
    return e == ERR_OK                    ? "ERR_OK"
         : e == ERR_NET_MALFORMED         ? "ERR_NET_MALFORMED"
         : e == ERR_NET_VERSION           ? "ERR_NET_VERSION"
         : e == ERR_NET_PAD_NONZERO       ? "ERR_NET_PAD_NONZERO"
         : e == ERR_NET_VARINT_OVERFLOW   ? "ERR_NET_VARINT_OVERFLOW"
         : "ERR_?";
}

// --- constants (docs/NETCODE.md §20 preamble; the tunables' doc home is docs/CANON.md) --------
constexpr u32 NET_FORMAT_VERSION = 1u;    // every wire struct's field 0
constexpr u32 MAX_PEERS          = 8u;    // docs/CANON.md; the slot_mask/hold bitmaps are one byte
constexpr u32 SLOT_RING_TICKS    = 32u;   // per-slot frame ring, power of two

// The redundancy window and the confirmation horizon the ring must cover (docs/CANON.md).
constexpr u32 REDUNDANCY_TICKS             = 9u;
constexpr u32 MAX_TICKS_PER_PACKET         = 9u;   // floor 3 under the §20.3(a) backoff
constexpr u32 MIN_TICKS_PER_PACKET         = 3u;
constexpr u32 CONFIRMATION_HORIZON_TICKS   = 6u;
constexpr u32 SUB_DECAY_TICKS              = 6u;
constexpr u32 MAX_LOG_RECORDS_PER_PACKET   = 8u;

// docs/NETCODE.md §20: SLOT_RING_TICKS is a power of two and covers the window the sequencer
// can still be asked about. Both are asserted here rather than commented, per the spec.
static_assert((SLOT_RING_TICKS & (SLOT_RING_TICKS - 1u)) == 0u, "SLOT_RING_TICKS is a power of two");
static_assert(SLOT_RING_TICKS >= REDUNDANCY_TICKS + CONFIRMATION_HORIZON_TICKS + 6u,
              "SLOT_RING_TICKS must cover the redundancy window + the confirmation horizon + 6 (docs/NETCODE.md §20)");
static_assert(MAX_PEERS <= 8u, "slot_mask, live_mask and the hold bitmaps are one byte wide (docs/NETCODE.md §20.2.2)");

// The §20.3(a) packet-size backoff floor is a real floor, not advice.
static_assert(MIN_TICKS_PER_PACKET <= MAX_TICKS_PER_PACKET, "");

// --- the input-frame geometry (docs/INPUT.md §1) ----------------------------------------------
// core/input.h is the W3 loop+input lane's file and is NOT defined here (RR-17 ruling,
// 2026-08-26): net-p1 builds the column codec against a GEOMETRY MIRROR of docs/INPUT.md §1's
// InputFrame, pinned field-for-field to that section's numbers, and the tests supply their own
// frame fixtures. The mirror carries net-scoped names so that when core/input.h lands there is
// no redeclaration to unpick - the handoff is one line:
//
//     W3: #include "core/input.h"
//         static_assert(sizeof(InputFrame) == NET_FRAME_BYTES);
//         static_assert(MAX_ACTIONS == NET_FRAME_MAX_ACTIONS);
//         using WireFrame = InputFrame;     // replaces the mirror; delete NetInputFrame
//
// Until then WireFrame is the mirror, and every net TU speaks WireFrame, never the mirror's own
// name, so that swap touches exactly this block. The numbers are docs/INPUT.md §1's and
// docs/CANON.md's (MAX_ACTIONS 32) - restating them here is the mirror's whole job, and the
// static_asserts below are what stops the restatement from drifting silently.
constexpr u32 NET_FRAME_MAX_ACTIONS = 32u;   // docs/INPUT.md §1 MAX_ACTIONS; a change is a wire-format bump
constexpr u32 NET_FRAME_BYTES       = 76u;   // docs/INPUT.md §1 InputFrame, explicitly laid out

// One action's state. value: digital 0/1, analog -127..127 (snorm8, quantized at capture).
// flags: bit0 down, bit1 pressed-this-tick, bit2 released-this-tick (docs/INPUT.md §1).
struct NetActionState {
    i8 value;
    u8 flags;
};
static_assert(sizeof(NetActionState) == 2u, "docs/INPUT.md §1: ActionState is 2 B");

// The geometry mirror of docs/INPUT.md §1's InputFrame. Layout, not ownership.
struct NetInputFrame {
    NetActionState actions[NET_FRAME_MAX_ACTIONS];  //  0, 64 B, indexed by ActionId (dense u16 < 32)
    i32            pointer_x;                       // 64, world-space pos_t raw bits
    i32            pointer_y;                       // 68
    u32            tick;                            // 72, low 32 bits of the u64 world tick
};
static_assert(sizeof(NetInputFrame) == NET_FRAME_BYTES, "docs/INPUT.md §1: InputFrame is 76 B");
static_assert(offsetof(NetInputFrame, actions)   ==  0u, "docs/INPUT.md §1 frame layout");
static_assert(offsetof(NetInputFrame, pointer_x) == 64u, "docs/INPUT.md §1 frame layout");
static_assert(offsetof(NetInputFrame, pointer_y) == 68u, "docs/INPUT.md §1 frame layout");
static_assert(offsetof(NetInputFrame, tick)      == 72u, "docs/INPUT.md §1 frame layout");
static_assert(__is_trivially_copyable(NetInputFrame), "frames are POD - they are rung, copied and encoded");

// The name every net TU uses. One line changes at the W3 handoff (see the block above).
using WireFrame = NetInputFrame;

// ZERO_FRAME: every ActionState {0,0}, pointer (0,0), tick 0. Frame 0 of a column is encoded
// against this (docs/NETCODE.md §20.2.2), and substitute() falls back to it when a slot has no
// present frame yet (§20.3(b)). A function, not a global: docs/CPP-SUBSET.md bans writable
// statics, and a constexpr value returned by value costs nothing at -O2.
constexpr WireFrame wire_zero_frame() { return WireFrame{}; }

// --- varint / zigzag (docs/NETCODE.md §20.2.2) ------------------------------------------------
// uvarint = LEB128, 7 bits per byte, 0x80 continuation, <= 5 bytes for a u32.
constexpr u32 WIRE_UVARINT_MAX_BYTES = 5u;

// zigzag32(v) = (u32(v) << 1) ^ u32(v >> 31) - maps small magnitudes of either sign to small
// unsigned values. The shift is on i32 and is arithmetic (C++20 defines it; docs/CPP-SUBSET.md §7).
constexpr u32 wire_zigzag32(i32 v) { return ((u32)v << 1) ^ (u32)(v >> 31); }

// The exact inverse of wire_zigzag32 over the whole u32 domain.
constexpr i32 wire_unzigzag32(u32 z) { return (i32)((z >> 1) ^ (u32)(-(i32)(z & 1u))); }

// Bytes wire_put_uvarint would write for v (1..WIRE_UVARINT_MAX_BYTES). Lets a caller size a
// buffer or a budget without encoding twice.
constexpr u32 wire_uvarint_bytes(u32 v) {
    return v < (1u << 7) ? 1u : v < (1u << 14) ? 2u : v < (1u << 21) ? 3u : v < (1u << 28) ? 4u : 5u;
}

// Appends v as LEB128. Never fails as a format matter; overflowing the caller's buffer is the
// byte writer's TL_CHECK (a producer that blows its own budget is a bug, docs/NETCODE.md §20.1).
inline void wire_put_uvarint(ByteWriter* w, u32 v) {
    while (v >= 0x80u) {
        bw_put_u8(w, (u8)(v | 0x80u));
        v >>= 7;
    }
    bw_put_u8(w, (u8)v);
}

// Appends v as uvarint(zigzag32(v)).
inline void wire_put_svarint(ByteWriter* w, i32 v) { wire_put_uvarint(w, wire_zigzag32(v)); }

// Reads a LEB128 u32 into *out. Truncation is the reader's sticky ERR_BYTES_TRUNCATED (checked
// by the caller once, at the end); a 6th continuation byte, or a 5th byte carrying bits above
// 2^32, is ERR_NET_VARINT_OVERFLOW - malformed CONTENT, which the byte pair cannot name because
// it has no format knowledge. On any failure *out is 0. Never runs inside a tick.
inline ErrCode wire_get_uvarint(ByteReader* r, u32* out) {
    TL_ASSERT(out != nullptr);
    *out = 0u;
    u32 v = 0u;
    for (u32 i = 0; i < WIRE_UVARINT_MAX_BYTES; ++i) {
        const u8 b = br_get_u8(r);
        if (!br_ok(r)) { return ERR_BYTES_TRUNCATED; }
        // The 5th byte carries only the top 4 bits of a u32; anything above them cannot be
        // represented and is a malformed stream, not a big number.
        if (i == WIRE_UVARINT_MAX_BYTES - 1u && (b & 0xF0u) != 0u) { return ERR_NET_VARINT_OVERFLOW; }
        v |= (u32)(b & 0x7Fu) << (7u * i);
        if ((b & 0x80u) == 0u) { *out = v; return ERR_OK; }
    }
    // Five bytes read and the last still had its continuation bit set.
    return ERR_NET_VARINT_OVERFLOW;
}

// Reads a zigzag LEB128 i32 into *out. Same failure modes as wire_get_uvarint; *out is 0 on any.
inline ErrCode wire_get_svarint(ByteReader* r, i32* out) {
    TL_ASSERT(out != nullptr);
    *out = 0;
    u32 z = 0u;
    const ErrCode e = wire_get_uvarint(r, &z);
    if (e != ERR_OK) { return e; }
    *out = wire_unzigzag32(z);
    return ERR_OK;
}

// The version policy every wire reader applies after the generated reader has decoded field 0:
// a stream from an OLDER or equal build is accepted, a NEWER one is refused (docs/NETCODE.md
// §20.2). Kept in one place so no struct's reader can quietly disagree.
constexpr ErrCode wire_check_version(u32 format_version) {
    return format_version > NET_FORMAT_VERSION ? ERR_NET_VERSION : ERR_OK;
}

// Decoder arithmetic on the pointer channels is wrap_add on i32 (docs/NETCODE.md §20.2.2): the
// encoder writes differences, and a difference that overflows i32 must come back bit-identical
// rather than trap under UBSan. Both are spelled through u32 because signed overflow is UB.

// a + b, wrapping on i32 overflow instead of trapping. Total; never fails.
constexpr i32 wire_wrap_add_i32(i32 a, i32 b) { return (i32)((u32)a + (u32)b); }

// a - b, wrapping on i32 overflow instead of trapping. The exact inverse of wire_wrap_add_i32.
constexpr i32 wire_wrap_sub_i32(i32 a, i32 b) { return (i32)((u32)a - (u32)b); }
