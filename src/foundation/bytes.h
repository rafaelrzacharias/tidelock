#pragma once
// ---------------------------------------------------------------------------------------------
// bytes.h - ByteWriter / ByteReader: the one little-endian byte pair every serialized struct in
//   the tree is written and read through.
//
// Spec: docs/NETCODE.md §1 ("own little-endian byte writers/readers in src/foundation/") is the
//   placement ruling; docs/ECS.md §10.2 (TL_WIRE_STRUCT's generated pairs write through these);
//   docs/CPP-SUBSET.md §9 R-2 (wire structs are "written/read through the little-endian byte
//   pair, never memcpy of the struct"). Landed by the W2 ecs lane as TL_WIRE_STRUCT's first
//   consumer; the net lane's wire.h adds varint/zigzag on top (docs/NETCODE.md §20.1).
// Purpose: endian-independent serialization by construction - every multi-byte value is written
//   as explicit byte stores, low byte first, so the encoded stream is identical on every target
//   regardless of host endianness, and reading it back never type-puns through a wider load.
// Invariants: writer overflow is a bug (the producer sized the buffer - TL_CHECK, all tiers);
//   reader underflow is DATA (a truncated/malformed input is the normal failure of an untrusted
//   byte stream) and is recoverable: the reader carries a STICKY ErrCode - the first failed read
//   sets it, that read and every later one returns 0 / zero-fills, and the caller checks r->err
//   ONCE at the end instead of per field (docs/CPP-SUBSET.md §3's Result shape per read would
//   make every generated field read a branch for no information gain; the sticky code is checked
//   before any decoded byte is acted on).
// Determinism: pure functions over caller memory; no io, no alloc, no floats; det-half header
//   (sim TUs may include it; the ErrCode range 0x06xx is bytes').
// Threading: one writer/reader, one thread; values only.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, <string.h> (memcpy/memset - the
//   docs/CPP-SUBSET.md §1 allowlist).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include <string.h>

// The bytes module's ErrCode range is 0x06xx (mem holds 0x01xx, jobs 0x02xx, net 0x04xx,
// alloy 0x0Axx). One code: every reader failure is a truncation - the reader has no format
// knowledge, so malformed-CONTENT codes belong to the format's own reader (wire, encoder).
constexpr ErrCode ERR_BYTES_TRUNCATED = (ErrCode)0x0601;  // read past the end of the input

// Log-side name for a bytes ErrCode; "ERR_?" outside the bytes range.
constexpr const char* err_bytes_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK" : e == ERR_BYTES_TRUNCATED ? "ERR_BYTES_TRUNCATED" : "ERR_?";
}

// Bump-writer over a caller-owned buffer. len is the bytes written so far; overflow is TL_CHECK
// (a producer that blows its own buffer is a bug, not a data condition).
struct ByteWriter {
    u8* base;
    u64 cap;
    u64 len;
};

// Cursor-reader over a caller-owned buffer. `err` is sticky: once set, every later read is a
// no-op returning 0 (out-buffers zero-filled), so generated field-by-field readers need exactly
// one check at the end. `_pad0` keeps the layout explicit; the struct is transient, never hashed.
struct ByteReader {
    const u8* base;
    u64 len;
    u64 pos;
    ErrCode err;
    u8 _pad0[6];
};

// Wires w onto [buf, buf+cap); len = 0. buf may be null only when cap == 0.
inline void bw_init(ByteWriter* w, u8* buf, u64 cap) {
    TL_ASSERT(buf != nullptr || cap == 0);
    w->base = buf; w->cap = cap; w->len = 0;
}

// Appends one byte. TL_CHECK on overflow (all tiers - the writer's buffer is the caller's budget).
inline void bw_put_u8(ByteWriter* w, u8 v) {
    TL_CHECK(w->len + 1 <= w->cap);
    w->base[w->len] = v;
    w->len += 1;
}

// Appends v as 2 bytes, low byte first. TL_CHECK on overflow.
inline void bw_put_u16(ByteWriter* w, u16 v) {
    TL_CHECK(w->len + 2 <= w->cap);
    w->base[w->len + 0] = (u8)(v & 0xFFu);
    w->base[w->len + 1] = (u8)((v >> 8) & 0xFFu);
    w->len += 2;
}

// Appends v as 4 bytes, low byte first. TL_CHECK on overflow.
inline void bw_put_u32(ByteWriter* w, u32 v) {
    TL_CHECK(w->len + 4 <= w->cap);
    w->base[w->len + 0] = (u8)(v & 0xFFu);
    w->base[w->len + 1] = (u8)((v >> 8) & 0xFFu);
    w->base[w->len + 2] = (u8)((v >> 16) & 0xFFu);
    w->base[w->len + 3] = (u8)((v >> 24) & 0xFFu);
    w->len += 4;
}

// Appends v as 8 bytes, low byte first. TL_CHECK on overflow.
inline void bw_put_u64(ByteWriter* w, u64 v) {
    TL_CHECK(w->len + 8 <= w->cap);
    for (u32 i = 0; i < 8; ++i) { w->base[w->len + i] = (u8)((v >> (8u * i)) & 0xFFu); }
    w->len += 8;
}

// Appends n raw bytes verbatim (an already-encoded payload, a string's bytes - the caller owns
// their byte order). TL_CHECK on overflow; p may be null only when n == 0.
inline void bw_put_bytes(ByteWriter* w, const void* p, u64 n) {
    TL_CHECK(n <= w->cap - w->len);   // spelled as a subtraction: len + n can wrap u64 for a bogus n
    TL_ASSERT(p != nullptr || n == 0);
    if (n != 0) { memcpy(w->base + w->len, p, n); }
    w->len += n;
}

// Wires r onto [buf, buf+len); pos = 0, err = ERR_OK. buf may be null only when len == 0.
inline void br_init(ByteReader* r, const u8* buf, u64 len) {
    TL_ASSERT(buf != nullptr || len == 0);
    r->base = buf; r->len = len; r->pos = 0; r->err = ERR_OK;
    r->_pad0[0] = r->_pad0[1] = r->_pad0[2] = r->_pad0[3] = r->_pad0[4] = r->_pad0[5] = 0;
}

// True while no read has failed; the sticky-error check callers make once at the end.
inline bool br_ok(const ByteReader* r) { return r->err == ERR_OK; }

// Reads one byte; 0 with err = ERR_BYTES_TRUNCATED on underflow or after a prior failure.
inline u8 br_get_u8(ByteReader* r) {
    if (r->err != ERR_OK || r->pos + 1 > r->len) { r->err = ERR_BYTES_TRUNCATED; return 0; }
    u8 v = r->base[r->pos];
    r->pos += 1;
    return v;
}

// Reads 2 bytes, low byte first; 0 with the sticky code on underflow or after a prior failure.
inline u16 br_get_u16(ByteReader* r) {
    if (r->err != ERR_OK || r->pos + 2 > r->len) { r->err = ERR_BYTES_TRUNCATED; return 0; }
    u16 v = (u16)((u16)r->base[r->pos + 0] | ((u16)r->base[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

// Reads 4 bytes, low byte first; 0 with the sticky code on underflow or after a prior failure.
inline u32 br_get_u32(ByteReader* r) {
    if (r->err != ERR_OK || r->pos + 4 > r->len) { r->err = ERR_BYTES_TRUNCATED; return 0; }
    u32 v = (u32)r->base[r->pos + 0] | ((u32)r->base[r->pos + 1] << 8)
          | ((u32)r->base[r->pos + 2] << 16) | ((u32)r->base[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

// Reads 8 bytes, low byte first; 0 with the sticky code on underflow or after a prior failure.
inline u64 br_get_u64(ByteReader* r) {
    if (r->err != ERR_OK || r->pos + 8 > r->len) { r->err = ERR_BYTES_TRUNCATED; return 0; }
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i) { v |= (u64)r->base[r->pos + i] << (8u * i); }
    r->pos += 8;
    return v;
}

// Copies n raw bytes into out; zero-fills out with the sticky code on underflow or after a prior
// failure (a partially-read struct never carries stale caller memory). out non-null unless n == 0.
inline void br_get_bytes(ByteReader* r, void* out, u64 n) {
    TL_ASSERT(out != nullptr || n == 0);
    if (n == 0) { return; }
    // n is DATA (a decoded length field): the bound is spelled as a subtraction because
    // pos + n can wrap u64 for a hostile n and a wrapped sum passes a `<= len` check.
    if (r->err != ERR_OK || n > r->len - r->pos) {
        r->err = ERR_BYTES_TRUNCATED;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->base + r->pos, n);
    r->pos += n;
}

static_assert(__is_trivially_copyable(ByteWriter) && __is_trivially_copyable(ByteReader), "");
