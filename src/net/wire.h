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
// The nonzero-pad refusal docs/NETCODE.md §20.2 requires is ERR_WIRE_PAD_NONZERO (0x0301,
// core/reflect.h) - the generated readers already return it, so net does not mint a second code
// for one condition (CLAUDE.md: one fact, one home).
constexpr ErrCode ERR_NET_MALFORMED       = (ErrCode)0x0401;  // a field's value cannot occur in a valid stream
constexpr ErrCode ERR_NET_VERSION         = (ErrCode)0x0402;  // format_version newer than this build understands
constexpr ErrCode ERR_NET_VARINT_OVERFLOW = (ErrCode)0x0403;  // a uvarint ran past the width of its target
constexpr ErrCode ERR_NET_DUPLICATE_RECORD= (ErrCode)0x0404;  // (origin_slot, seq) already stored - R6 no-op
constexpr ErrCode ERR_NET_STORE_FULL      = (ErrCode)0x0405;  // no room for a sequenced record: DATA LOSS

// Log-side name for a net ErrCode; "ERR_?" outside the net range.
constexpr const char* err_net_name(ErrCode e) {
    // Includes the two codes net's decoders return most but do not own: a truncated stream is
    // bytes', a nonzero pad is reflect.h's. Naming only net's own would log the common cases as
    // "ERR_?".
    return e == ERR_BYTES_TRUNCATED       ? "ERR_BYTES_TRUNCATED"
         : e == ERR_WIRE_PAD_NONZERO      ? "ERR_WIRE_PAD_NONZERO"
         : e == ERR_OK                    ? "ERR_OK"
         : e == ERR_NET_MALFORMED         ? "ERR_NET_MALFORMED"
         : e == ERR_NET_VERSION           ? "ERR_NET_VERSION"
         : e == ERR_NET_VARINT_OVERFLOW   ? "ERR_NET_VARINT_OVERFLOW"
         : e == ERR_NET_DUPLICATE_RECORD  ? "ERR_NET_DUPLICATE_RECORD"
         : e == ERR_NET_STORE_FULL        ? "ERR_NET_STORE_FULL"
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
//
// CANONICAL FORM (added by this lane; recorded in TODO.md for docs/NETCODE.md §20.3(a)'s refusal
// list). Every value has exactly ONE valid encoding, and the decoders refuse the alternatives:
// a non-minimal varint here, and a redundant value byte in the column codec. This is not
// tidiness - docs/NETCODE.md §20.2.8 hashes the archive segment bytes into
// ChainEntry.log_segment_hash, and §20.3's chain is BLAKE2b over that. Two peers that encode the
// same confirmed input MUST produce the same bytes or the chain forks with no divergence behind
// it. The encoder emits canonical output by construction; refusing non-canonical INPUT is what
// stops a third-party or corrupted stream from decoding to frames that re-encode differently.
// It only ever tightens: no stream this encoder produces is refused.
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
// 2^32, is ERR_NET_VARINT_OVERFLOW; a NON-MINIMAL encoding (a multi-byte varint whose last byte
// is 0, e.g. 80 00 for zero) is ERR_NET_MALFORMED - see the canonical-form note below. On any
// failure *out is 0. Never runs inside a tick.
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
        if ((b & 0x80u) == 0u) {
            // Canonical form: a multi-byte varint's final byte carries the value's high bits and
            // cannot be zero - `80 00` and `00` would otherwise both decode to 0 and re-encode
            // to different bytes.
            if (i > 0u && b == 0u) { return ERR_NET_MALFORMED; }
            *out = v;
            return ERR_OK;
        }
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

// --- §20.2 wire structs -----------------------------------------------------------------------
// Every struct below is a TL_WIRE_STRUCT (docs/CPP-SUBSET.md §9 R-2): concrete, non-template,
// explicitly padded, leading u32 format_version, an offsetof static_assert per field generated
// from the parallel TL_OFFSETS_ list, written and read through foundation/bytes.h's little-endian
// pair rather than memcpy'd. The generated reader zero-fills, propagates the sticky truncation
// code, and refuses a nonzero _padN with ERR_WIRE_PAD_NONZERO; refusing a NEWER format_version is
// the caller's policy and is spelled once, in wire_check_version above.
//
// The offsets restate docs/NETCODE.md §20.2's numbers. That restatement is the point - it is what
// pins the layout - and it cannot drift silently: a wrong number fails the build at the
// TL_X_WIRE_OFFSET assert, and the sizeof pin catches a field list that has gone out of step.
//
// NOTE (spec, docs/NETCODE.md §20.2): the section opens "All are TL_WIRE_STRUCT", but three of
// its own struct definitions carry no leading format_version - CheckpointArenaEntry,
// ChainRecord and ArchiveStreamHeader. They are repeated elements INSIDE a container that
// already versioned itself once in its header, so a per-element version would be redundant
// bytes on every row. They are declared as plain PODs at the end of this section with the same
// sizeof/offsetof pins. Recorded in TODO.md rather than resolved here: the concrete definitions
// win over the summary sentence, but the sentence should say so.

// PacketHeader - docs/NETCODE.md §20.2.1: INPUT channel, every packet
#define TL_FIELDS_PacketHeader(X, XA, XH) \
    X(u8, kind) \
    X(u8, slot) \
    X(u8, frame_count) \
    X(u8, slot_mask) \
    X(u64, base_tick) \
    X(u64, last_confirmed_tick) \
    X(u64, hold_base_tick) \
    X(u32, epoch) \
    X(u8, hold_count) \
    X(u8, delay_ticks) \
    X(u16, payload_bytes)
#define TL_OFFSETS_PacketHeader(XO) \
    XO(kind, 4) \
    XO(slot, 5) \
    XO(frame_count, 6) \
    XO(slot_mask, 7) \
    XO(base_tick, 8) \
    XO(last_confirmed_tick, 16) \
    XO(hold_base_tick, 24) \
    XO(epoch, 32) \
    XO(hold_count, 36) \
    XO(delay_ticks, 37) \
    XO(payload_bytes, 38)

// LogRecord - docs/NETCODE.md §20.2.3: every sequenced one-shot; stable id is (origin_slot, seq)
#define TL_FIELDS_LogRecord(X, XA, XH) \
    X(u8, kind) \
    X(u8, slot) \
    X(u8, origin_slot) \
    X(u8, _pad0) \
    X(u32, seq) \
    X(u32, payload) \
    X(u64, effective_tick)
#define TL_OFFSETS_LogRecord(XO) \
    XO(kind, 4) \
    XO(slot, 5) \
    XO(origin_slot, 6) \
    XO(_pad0, 7) \
    XO(seq, 8) \
    XO(payload, 12) \
    XO(effective_tick, 16)

// ControlHeader - docs/NETCODE.md §20.2.4: CONTROL channel (mesh, unreliable)
#define TL_FIELDS_ControlHeader(X, XA, XH) \
    X(u8, kind) \
    X(u8, slot) \
    X(u16, payload_bytes) \
    X(u64, tick) \
    X(u32, epoch) \
    X(u32, _pad0)
#define TL_OFFSETS_ControlHeader(XO) \
    XO(kind, 4) \
    XO(slot, 5) \
    XO(payload_bytes, 6) \
    XO(tick, 8) \
    XO(epoch, 16) \
    XO(_pad0, 20)

// Suspicion - docs/NETCODE.md §20.2.4: gossip; idempotent; re-sent while nonzero
#define TL_FIELDS_Suspicion(X, XA, XH) \
    X(u8, suspect_mask) \
    XA(u8, _pad0, 3) \
    X(u32, seq) \
    X(u32, _pad1)
#define TL_OFFSETS_Suspicion(XO) \
    XO(suspect_mask, 4) \
    XO(_pad0, 5) \
    XO(seq, 8) \
    XO(_pad1, 12)

// EpochClaim - docs/NETCODE.md §20.2.4: successor's claim of current + 1
#define TL_FIELDS_EpochClaim(X, XA, XH) \
    X(u32, epoch) \
    X(u64, last_confirmed_tick) \
    X(u32, claim_seq) \
    X(u8, candidate_slot) \
    XA(u8, _pad0, 3)
#define TL_OFFSETS_EpochClaim(XO) \
    XO(epoch, 4) \
    XO(last_confirmed_tick, 8) \
    XO(claim_seq, 16) \
    XO(candidate_slot, 20) \
    XO(_pad0, 21)

// EpochAck - docs/NETCODE.md §20.2.4: grant or refusal, with a named reason
#define TL_FIELDS_EpochAck(X, XA, XH) \
    X(u32, epoch) \
    X(u64, my_last_confirmed_tick) \
    X(u32, claim_seq) \
    X(u8, granted) \
    X(u8, refuse_reason) \
    XA(u8, _pad0, 2)
#define TL_OFFSETS_EpochAck(XO) \
    XO(epoch, 4) \
    XO(my_last_confirmed_tick, 8) \
    XO(claim_seq, 16) \
    XO(granted, 20) \
    XO(refuse_reason, 21) \
    XO(_pad0, 22)

// HashDigest - docs/NETCODE.md §20.2.4: 24 B + 8*arena_count: the arena vector follows the struct
#define TL_FIELDS_HashDigest(X, XA, XH) \
    X(u32, arena_count) \
    X(u64, tick) \
    X(u64, fold)
#define TL_OFFSETS_HashDigest(XO) \
    XO(arena_count, 4) \
    XO(tick, 8) \
    XO(fold, 16)

// MeasurementVector - docs/NETCODE.md §20.2.4: lobby seating input; measured, never sequenced
#define TL_FIELDS_MeasurementVector(X, XA, XH) \
    X(u32, upstream_kbps) \
    XA(u16, rtt_p50_ms, MAX_PEERS) \
    XA(u16, rtt_p95_ms, MAX_PEERS) \
    XA(u8, loss_pct, MAX_PEERS)
#define TL_OFFSETS_MeasurementVector(XO) \
    XO(upstream_kbps, 4) \
    XO(rtt_p50_ms, 8) \
    XO(rtt_p95_ms, 24) \
    XO(loss_pct, 40)

// CustodyHandoff - docs/NETCODE.md §20.2.4: signed over bytes [0,120)
#define TL_FIELDS_CustodyHandoff(X, XA, XH) \
    X(u32, handoff_seq) \
    XA(u8, from_pubkey, 32) \
    XA(u8, to_pubkey, 32) \
    XA(u8, chain_head, 32) \
    X(u64, effective_tick) \
    X(u8, forced) \
    XA(u8, _pad0, 7) \
    XA(u8, signature, 64)
#define TL_OFFSETS_CustodyHandoff(XO) \
    XO(handoff_seq, 4) \
    XO(from_pubkey, 8) \
    XO(to_pubkey, 40) \
    XO(chain_head, 72) \
    XO(effective_tick, 104) \
    XO(forced, 112) \
    XO(_pad0, 113) \
    XO(signature, 120)

// Leave - docs/NETCODE.md §20.2.4: to the coordinator; it sequences LR_LEAVE
#define TL_FIELDS_Leave(X, XA, XH) \
    X(u32, leave_seq) \
    X(u64, requested_effective_tick)
#define TL_OFFSETS_Leave(XO) \
    XO(leave_seq, 4) \
    XO(requested_effective_tick, 8)

// LobbyProbe - docs/NETCODE.md §20.2.4: RTT probe; send_time_us is echoed back verbatim
#define TL_FIELDS_LobbyProbe(X, XA, XH) \
    X(u32, probe_seq) \
    X(u64, send_time_us) \
    X(u64, echo_time_us) \
    X(u8, is_reply) \
    XA(u8, _pad0, 7)
#define TL_OFFSETS_LobbyProbe(XO) \
    XO(probe_seq, 4) \
    XO(send_time_us, 8) \
    XO(echo_time_us, 16) \
    XO(is_reply, 24) \
    XO(_pad0, 25)

// BulkHeader - docs/NETCODE.md §20.2.5: BULK channel (reliable + fragmented, point-to-point)
#define TL_FIELDS_BulkHeader(X, XA, XH) \
    X(u8, kind) \
    X(u8, slot) \
    X(u16, _pad0) \
    X(u64, transfer_id) \
    X(u32, chunk_index) \
    X(u32, chunk_count) \
    X(u32, offset) \
    X(u32, len)
#define TL_OFFSETS_BulkHeader(XO) \
    XO(kind, 4) \
    XO(slot, 5) \
    XO(_pad0, 6) \
    XO(transfer_id, 8) \
    XO(chunk_index, 16) \
    XO(chunk_count, 20) \
    XO(offset, 24) \
    XO(len, 28)

// LogRequest - docs/NETCODE.md §20.2.5: "send me the confirmed log for [from, to]"
#define TL_FIELDS_LogRequest(X, XA, XH) \
    X(u32, _pad0) \
    X(u64, from_tick) \
    X(u64, to_tick)
#define TL_OFFSETS_LogRequest(XO) \
    XO(_pad0, 4) \
    XO(from_tick, 8) \
    XO(to_tick, 16)

// BulkAck - docs/NETCODE.md §20.2.5: the §5.4 epilogue [final_tick][final_ref_hash][all_agree]
#define TL_FIELDS_BulkAck(X, XA, XH) \
    X(u8, all_agree) \
    XA(u8, _pad0, 3) \
    X(u64, final_tick) \
    X(u64, final_ref_hash)
#define TL_OFFSETS_BulkAck(XO) \
    XO(all_agree, 4) \
    XO(_pad0, 5) \
    XO(final_tick, 8) \
    XO(final_ref_hash, 16)

// Handshake - docs/NETCODE.md §15.1: sent before any input flows; any mismatch ends the session
#define TL_FIELDS_Handshake(X, XA, XH) \
    X(u32, session_model) \
    X(u32, origin) \
    X(u32, max_actions) \
    XA(u8, build_id, 32) \
    XA(u8, session_fingerprint, 32) \
    X(u64, tick0_state_hash) \
    XA(u8, world_chain_head, 32)
#define TL_OFFSETS_Handshake(XO) \
    XO(session_model, 4) \
    XO(origin, 8) \
    XO(max_actions, 12) \
    XO(build_id, 16) \
    XO(session_fingerprint, 48) \
    XO(tick0_state_hash, 80) \
    XO(world_chain_head, 88)

// JoinChallenge - docs/NETCODE.md §20.2.7: server -> joiner, first message on connect
#define TL_FIELDS_JoinChallenge(X, XA, XH) \
    X(u32, _pad0) \
    XA(u8, nonce, 32)
#define TL_OFFSETS_JoinChallenge(XO) \
    XO(_pad0, 4) \
    XO(nonce, 8)

// JoinRequest - docs/NETCODE.md §20.2.7: joiner -> server, immediately followed by its Handshake
#define TL_FIELDS_JoinRequest(X, XA, XH) \
    X(u32, requested_slot) \
    XA(u8, identity_pubkey, 32) \
    XA(u8, held_chain_head, 32) \
    X(u64, held_durable_tick) \
    XA(u8, signature, 64)
#define TL_OFFSETS_JoinRequest(XO) \
    XO(requested_slot, 4) \
    XO(identity_pubkey, 8) \
    XO(held_chain_head, 40) \
    XO(held_durable_tick, 72) \
    XO(signature, 80)

// JoinReply - docs/NETCODE.md §20.2.7: accepted/refused, with the succession list and a named reason
#define TL_FIELDS_JoinReply(X, XA, XH) \
    X(u32, epoch) \
    X(u64, join_effective_tick) \
    X(u64, checkpoint_tick) \
    X(u64, confirmed_tick) \
    XA(u8, succession, MAX_PEERS) \
    X(u8, accepted) \
    X(u8, slot) \
    X(u8, reason) \
    X(u8, coordinator_slot) \
    X(u8, live_mask) \
    XA(u8, _pad0, 3)
#define TL_OFFSETS_JoinReply(XO) \
    XO(epoch, 4) \
    XO(join_effective_tick, 8) \
    XO(checkpoint_tick, 16) \
    XO(confirmed_tick, 24) \
    XO(succession, 32) \
    XO(accepted, 40) \
    XO(slot, 41) \
    XO(reason, 42) \
    XO(coordinator_slot, 43) \
    XO(live_mask, 44) \
    XO(_pad0, 45)

// CheckpointHeader - docs/NETCODE.md §20.2.8: the checkpoint file image's leading header
#define TL_FIELDS_CheckpointHeader(X, XA, XH) \
    X(u32, session_model) \
    X(u32, origin) \
    X(u32, arena_count) \
    XA(u8, build_id, 32) \
    XA(u8, session_fingerprint, 32) \
    X(u64, tick) \
    X(u64, seed) \
    X(u64, state_hash) \
    XA(u8, custody_pubkey, 32) \
    XA(u8, chain_entry, 32) \
    X(u32, chain_index) \
    X(u32, tier) \
    X(u64, payload_bytes) \
    X(u32, payload_crc32) \
    X(u32, header_crc32)
#define TL_OFFSETS_CheckpointHeader(XO) \
    XO(session_model, 4) \
    XO(origin, 8) \
    XO(arena_count, 12) \
    XO(build_id, 16) \
    XO(session_fingerprint, 48) \
    XO(tick, 80) \
    XO(seed, 88) \
    XO(state_hash, 96) \
    XO(custody_pubkey, 104) \
    XO(chain_entry, 136) \
    XO(chain_index, 168) \
    XO(tier, 172) \
    XO(payload_bytes, 176) \
    XO(payload_crc32, 184) \
    XO(header_crc32, 188)

// ChainGenesis - docs/NETCODE.md §20.2.8: chain[0] = BLAKE2b-256(le_bytes(ChainGenesis))
#define TL_FIELDS_ChainGenesis(X, XA, XH) \
    X(u32, _pad0) \
    X(u64, tick0_state_hash) \
    X(u64, world_seed) \
    XA(u8, creation_nonce, 32)
#define TL_OFFSETS_ChainGenesis(XO) \
    XO(_pad0, 4) \
    XO(tick0_state_hash, 8) \
    XO(world_seed, 16) \
    XO(creation_nonce, 24)

// ChainEntry - docs/NETCODE.md §20.2.8: chain[K] = BLAKE2b-256(le_bytes(ChainEntry)), K >= 1
#define TL_FIELDS_ChainEntry(X, XA, XH) \
    X(u32, chain_index) \
    XA(u8, prev, 32) \
    XA(u8, log_segment_hash, 32) \
    X(u64, state_hash) \
    X(u64, tick) \
    XA(u8, session_fingerprint, 32) \
    XA(u8, custody_pubkey, 32)
#define TL_OFFSETS_ChainEntry(XO) \
    XO(chain_index, 4) \
    XO(prev, 8) \
    XO(log_segment_hash, 40) \
    XO(state_hash, 72) \
    XO(tick, 80) \
    XO(session_fingerprint, 88) \
    XO(custody_pubkey, 120)

// ArchiveSegmentHeader - docs/NETCODE.md §20.2.9: one archive segment's header
#define TL_FIELDS_ArchiveSegmentHeader(X, XA, XH) \
    X(u32, max_actions) \
    X(u64, base_tick) \
    X(u32, tick_count) \
    X(u8, slot_mask) \
    XA(u8, _pad0, 3) \
    X(u32, record_count) \
    X(u32, log_record_count) \
    XA(u8, build_id, 32) \
    XA(u8, session_fingerprint, 32) \
    X(u32, payload_bytes) \
    X(u32, payload_crc32) \
    X(u32, segment_seq) \
    X(u32, header_crc32)
#define TL_OFFSETS_ArchiveSegmentHeader(XO) \
    XO(max_actions, 4) \
    XO(base_tick, 8) \
    XO(tick_count, 16) \
    XO(slot_mask, 20) \
    XO(_pad0, 21) \
    XO(record_count, 24) \
    XO(log_record_count, 28) \
    XO(build_id, 32) \
    XO(session_fingerprint, 64) \
    XO(payload_bytes, 96) \
    XO(payload_crc32, 100) \
    XO(segment_seq, 104) \
    XO(header_crc32, 108)

// HashTraceHeader - docs/NETCODE.md §20.2.9: the RecordedInput file's hash-trace header
#define TL_FIELDS_HashTraceHeader(X, XA, XH) \
    X(u32, arena_count) \
    X(u64, base_tick) \
    X(u32, tick_count) \
    X(u32, _pad0)
#define TL_OFFSETS_HashTraceHeader(XO) \
    XO(arena_count, 4) \
    XO(base_tick, 8) \
    XO(tick_count, 16) \
    XO(_pad0, 20)


// --- the roll call ----------------------------------------------------------------------------
// Every TL_WIRE_STRUCT of docs/NETCODE.md §20.2 (+ §15.1's Handshake, which §20.2.6 reuses
// verbatim), with the size its static_assert pins. The structs are DECLARED by expanding this
// list, so a struct that is not named here does not exist - which is what lets tests/net's T0
// drive itself off the same list and genuinely cover every one. (Its previous count assert
// counted the test file's own copy of the list and so could never fire for the case it named.)
#define TL_NET_WIRE_STRUCTS(F) \
    F(PacketHeader, 40) \
    F(LogRecord, 24) \
    F(ControlHeader, 24) \
    F(Suspicion, 16) \
    F(EpochClaim, 24) \
    F(EpochAck, 24) \
    F(HashDigest, 24) \
    F(MeasurementVector, 48) \
    F(CustodyHandoff, 184) \
    F(Leave, 16) \
    F(LobbyProbe, 32) \
    F(BulkHeader, 32) \
    F(LogRequest, 24) \
    F(BulkAck, 24) \
    F(Handshake, 120) \
    F(JoinChallenge, 40) \
    F(JoinRequest, 144) \
    F(JoinReply, 48) \
    F(CheckpointHeader, 192) \
    F(ChainGenesis, 56) \
    F(ChainEntry, 152) \
    F(ArchiveSegmentHeader, 112) \
    F(HashTraceHeader, 24)

#define TL_NET_DECLARE_WIRE_STRUCT(Name, Size)                                                 \
    TL_WIRE_STRUCT(Name)                                                                       \
    static_assert(sizeof(Name) == (Size), "docs/NETCODE.md §20.2 pins this struct's size");
TL_NET_WIRE_STRUCTS(TL_NET_DECLARE_WIRE_STRUCT)
#undef TL_NET_DECLARE_WIRE_STRUCT

// --- the three repeated-element structs (no per-element format_version; see the note above) ---

// One registered SNAPSHOT arena's extent inside a checkpoint image, in registry order
// (docs/NETCODE.md §20.2.8). An element of CheckpointHeader's array, which is already versioned.
struct CheckpointArenaEntry {
    u64 id;      // 0  NameHash
    u64 used;    // 8  bytes of [base, used)
};
static_assert(sizeof(CheckpointArenaEntry) == 16u, "docs/NETCODE.md §20.2.8");
static_assert(offsetof(CheckpointArenaEntry, used) == 8u, "docs/NETCODE.md §20.2.8");

// One line of the chain file: the entry and the hash it produced (docs/NETCODE.md §20.2.8).
// chain file = ChainGenesis + ChainRecord[K].
struct ChainRecord {
    ChainEntry entry;   //   0
    u8         hash[32];// 152  chain[K] = BLAKE2b-256(le_bytes(entry))
};
static_assert(sizeof(ChainRecord) == 184u, "docs/NETCODE.md §20.2.8");
static_assert(offsetof(ChainRecord, hash) == 152u, "docs/NETCODE.md §20.2.8");

// One archive stream's header: a (slot, channel) pair's record count (docs/NETCODE.md §20.2.9).
// An element inside ArchiveSegmentHeader's payload, which carries the version and the crc32.
struct ArchiveStreamHeader {
    u32 record_count;   // 0
    u8  channel;        // 4  0..31 action, 32 pointer_x, 33 pointer_y, 34 flag escape
    u8  slot;           // 5
    u16 _pad0;          // 6
};
static_assert(sizeof(ArchiveStreamHeader) == 8u, "docs/NETCODE.md §20.2.9");
static_assert(offsetof(ArchiveStreamHeader, channel) == 4u, "docs/NETCODE.md §20.2.9");
static_assert(offsetof(ArchiveStreamHeader, slot) == 5u, "docs/NETCODE.md §20.2.9");

// --- the kind enums (docs/NETCODE.md §20.2's `kind` field comments) ---------------------------
// Named so the encoder, the archive and the tests cannot disagree about a magic number. Values
// are the spec's; 0 is deliberately unused in every one, so a zero-filled buffer is never a
// valid kind.
enum PacketKind : u8 { PK_UPSTREAM = 1, PK_DOWNSTREAM = 2, PK_MIRROR = 3, PK_KEEPALIVE = 4 };

enum LogRecordKind : u8 {
    LR_JOIN = 1, LR_LEAVE = 2, LR_SUSPECT = 3, LR_RESUME = 4, LR_EVICT = 5,
    LR_DELAY = 6, LR_EPOCH = 7, LR_CUSTODY = 8, LR_END = 9
};

enum ControlKind : u8 {
    CK_SUSPICION = 1, CK_EPOCH_CLAIM = 2, CK_EPOCH_ACK = 3, CK_HASH_DIGEST = 4,
    CK_MEASUREMENT = 5, CK_CUSTODY = 6, CK_LEAVE = 7, CK_LOBBY_PROBE = 8
};

enum BulkKind : u8 {
    BK_JOIN_CHALLENGE = 1, BK_JOIN_REQUEST = 2, BK_HANDSHAKE = 3, BK_JOIN_REPLY = 4,
    BK_SNAPSHOT_CHUNK = 5, BK_LOG_SEGMENT = 6, BK_LOG_REQUEST = 7, BK_ACK = 8
};

// Archive stream channels (docs/NETCODE.md §20.2.9): 0..31 are the actions, then the three
// non-action streams. 36 streams per slot, written in ascending channel order.
constexpr u8  ARCHIVE_CH_POINTER_X  = 32u;
constexpr u8  ARCHIVE_CH_POINTER_Y  = 33u;
constexpr u8  ARCHIVE_CH_FLAG_ESCAPE = 34u;
// docs/NETCODE.md §20.2.9's layout line says "for ch in 0..35" and "36 streams per slot", but it
// defines 35 channels: 0..31 action, 32, 33, 34. ARCHIVE_CH_COUNT keeps the doc's figure because
// the encoder's size budget is stated in it; ARCHIVE_CH_MAX is the real bound a decoder checks,
// and channel 35 is refused rather than aliased onto the escape channel. Filed in TODO.md.
constexpr u32 ARCHIVE_CH_COUNT      = 36u;
constexpr u8  ARCHIVE_CH_MAX        = ARCHIVE_CH_FLAG_ESCAPE;
static_assert(ARCHIVE_CH_POINTER_X == NET_FRAME_MAX_ACTIONS,
              "the action channels are 0..MAX_ACTIONS-1, so pointer_x starts at MAX_ACTIONS");

// --- the column codec (docs/NETCODE.md §20.2.2 layout, §20.3(a) algorithm) --------------------
// Declared here rather than in net_internal.h because the column IS the wire format and because
// §20.1 scopes net_internal.h to net/*.cpp, which would put the codec out of reach of its own
// tests. The implementation is net/encode.cpp.

// The three bits docs/INPUT.md §1 defines in ActionState.flags (down, pressed, released). The
// wire's rec byte carries exactly these; a frame whose flags have any other bit set is not a
// frame this format can represent, and the encoder treats it as a BUG (TL_ASSERT), not as data.
constexpr u8 WIRE_FLAG_BITS = 0x07u;

// Bit 3 of the rec byte: an explicit value byte follows. Bits 4..7 must be zero - the decoder
// refuses a rec byte with any of them set (docs/NETCODE.md §20.3(a)).
constexpr u8 WIRE_REC_VALUE_FOLLOWS = 0x08u;
constexpr u8 WIRE_REC_RESERVED_MASK = 0xF0u;

// The value a rec byte implies when no value byte follows: docs/NETCODE.md §20.2.2's
// value_follows = (value != (i8)(flags & 1)), i.e. a digital action's value tracks its down bit.
constexpr i8 wire_implied_value(u8 flags) { return (i8)(flags & 1u); }

// Encodes `n` frames as ONE self-contained column (docs/NETCODE.md §20.2.2): frame 0 delta-coded
// against ZERO_FRAME, frame i against frame i-1, pointers as second differences. Writes into w;
// overflowing w is the caller's budget bug (bytes.h TL_CHECKs it), not a data condition.
// `frames` must hold n entries and every ActionState.flags must be within WIRE_FLAG_BITS
// (asserted). n may be 0 - a keepalive's column is empty. Never runs inside a tick.
void encode_column(ByteWriter* w, const WireFrame* frames, u32 n);

// Decodes `frame_count` frames from r into out[], mirroring encode_column. Each decoded frame's
// tick is SET to u32(base_tick + i) - the tick is never transmitted (docs/NETCODE.md §20.2.2).
// Returns ERR_BYTES_TRUNCATED on a short column, ERR_NET_MALFORMED on a rec byte with a reserved
// bit set or a `changed` bit at or past MAX_ACTIONS, ERR_NET_VARINT_OVERFLOW on a bad varint.
// On any error out[] holds only decoded-or-zero frames and must not be acted on. `out` must hold
// frame_count entries; frame_count may be 0. Never runs inside a tick.
ErrCode decode_column(ByteReader* r, WireFrame* out, u32 frame_count, u64 base_tick);

// --- the archive segment codec (docs/NETCODE.md §20.2.9 layout, §13.3 encoding) ---------------
// Implementation: net/archive.cpp. Declared here for the same reason as the column codec.

// One decoded tick's worth of one slot, as the archive stores it: the segment holds the CONFIRMED
// APPLIED frame of every live slot (docs/NETCODE.md §20.2.9), substituted and phantom frames
// included literally. A slot outside the segment's slot_mask decodes as ZERO.
struct ArchiveInput {
    const WireFrame* frames;   // tick_count frames for this slot, ascending from base_tick
    u32              slot;     // 0..MAX_PEERS-1
    u32              _pad0;
};

// Bytes an encoded segment needs, given the worst case for its inputs. A caller sizes its buffer
// with this rather than guessing; archive_encode_segment TL_CHECKs the buffer it is handed, since
// a producer that blows its own budget is a bug (docs/NETCODE.md §20.1).
u64 archive_segment_max_bytes(u32 slot_count, u32 tick_count, u32 log_record_count);

// Encodes one segment: header + per-slot per-channel transition streams + the LogRecord array,
// with both crc32 fields filled (docs/NETCODE.md §20.2.9). `inputs` holds slot_count entries in
// ASCENDING slot order and each carries tick_count frames; `records` holds log_record_count
// entries sorted by (effective_tick, origin_slot, seq) - the caller's ordering contract, asserted.
// Writes into w. Returns the bytes written. Never runs inside a tick.
u64 archive_encode_segment(ByteWriter* w, u64 base_tick, u32 tick_count,
                           const ArchiveInput* inputs, u32 slot_count,
                           const LogRecord* records, u32 log_record_count, u32 segment_seq,
                           const u8 build_id[32], const u8 session_fingerprint[32]);

// Decodes one segment written by archive_encode_segment. `out_frames` must hold
// MAX_PEERS * tick_count frames, indexed [slot * tick_count + i]; slots outside the segment's
// slot_mask are filled with ZERO_FRAME (docs/NETCODE.md §20.2.9). `out_records` must hold at
// least the segment's log_record_count, which *out_record_count reports.
// Returns ERR_BYTES_TRUNCATED on a short segment, ERR_NET_VERSION on a newer format_version,
// ERR_NET_MALFORMED on a failed crc32, an out-of-range channel or slot, a non-canonical stream,
// or a tick_count/record count the caller's buffers cannot hold. On any error nothing decoded
// should be acted on. Never runs inside a tick.
ErrCode archive_decode_segment(ByteReader* r, ArchiveSegmentHeader* out_header,
                               WireFrame* out_frames, u32 out_frame_capacity_per_slot,
                               LogRecord* out_records, u32 out_record_capacity,
                               u32* out_record_count);
