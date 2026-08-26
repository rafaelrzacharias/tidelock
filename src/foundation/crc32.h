#pragma once
// ---------------------------------------------------------------------------------------------
// crc32.h - the project's own table-driven CRC-32, for block integrity on stored and streamed
//   images (archive segments, checkpoint headers and payloads).
//
// Spec: docs/NETCODE.md §1 ("Checksums | own table-driven crc32 in `src/foundation/`") is the
//   placement ruling, and §13.3 ("Own crc32 per block, table-driven") the requirement; the
//   consumers are docs/NETCODE.md §20.2.8's CheckpointHeader.{payload,header}_crc32 and
//   §20.2.9's ArchiveSegmentHeader.{payload,header}_crc32. Landed by the W2 net-p1 lane as the
//   first consumer, following the precedent `foundation/bytes.h` set (docs/ECS.md §10.1: the
//   little-endian byte pair placed by docs/NETCODE.md §1 and built by the W2 ecs lane).
// Purpose: detect corruption in a stored or transferred block. NOT a security primitive - a
//   crc32 detects accident, never tampering. Tamper-evidence is the chain's BLAKE2b
//   (docs/NETCODE.md §11.5), and nothing here should ever be used in its place.
// Variant: CRC-32/ISO-HDLC - the one PNG, zlib and Ethernet use. Reflected input and output,
//   polynomial 0xEDB88320 (the bit-reversed 0x04C11DB7), init and final xor 0xFFFFFFFF. The doc
//   requires "a table-driven crc32" without naming a variant; this is the universal default, and
//   CRC32_CHECK_VALUE below pins it against the algorithm's own published check constant so a
//   future edit cannot quietly change what the bytes on disk mean.
// Determinism: pure integer functions over caller memory - no io, no alloc, no floats, no state.
//   Identical on every target in the docs/CANON.md matrix; the table is const data computed at
//   compile time, so there is no initialization order and no writable static
//   (docs/CPP-SUBSET.md §9).
// Threading: values only; re-entrant.
// Includes: foundation/tl_types.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

// The reflected CRC-32 polynomial (bit-reversed 0x04C11DB7).
constexpr u32 CRC32_POLY_REFLECTED = 0xEDB88320u;

// One table row: the CRC of a single byte value, folded 8 times. constexpr so the whole table is
// const data in the image rather than something built at startup.
// The CRC of a single byte value, folded 8 times - one row of the table. Pure; total.
constexpr u32 crc32_table_entry(u32 byte_value) {
    u32 c = byte_value;
    for (u32 k = 0; k < 8u; ++k) {
        c = (c & 1u) ? (CRC32_POLY_REFLECTED ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

// The 256-entry byte table. `inline constexpr` gives one copy across TUs with no static
// initializer (docs/CPP-SUBSET.md §9's writable-static ban).
struct Crc32Table {
    u32 v[256];
};
// Builds the 256-entry table at compile time. Called once, to initialize CRC32_TABLE.
constexpr Crc32Table crc32_build_table() {
    Crc32Table t{};
    for (u32 i = 0; i < 256u; ++i) { t.v[i] = crc32_table_entry(i); }
    return t;
}
inline constexpr Crc32Table CRC32_TABLE = crc32_build_table();

// Folds n bytes at p into a running CRC. `running` is the value a previous call returned, or
// CRC32_INIT to begin; the result is NOT the final CRC until passed through crc32_final. Lets a
// caller checksum a header and a payload that are not contiguous. p may be null only when n == 0.
constexpr u32 CRC32_INIT = 0xFFFFFFFFu;

// Updates a running CRC with n bytes. Total; never fails.
inline u32 crc32_update(u32 running, const void* p, u64 n) {
    const u8* b = (const u8*)p;
    u32 c = running;
    for (u64 i = 0; i < n; ++i) {
        c = CRC32_TABLE.v[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

// Closes a running CRC into the value that goes on the wire. Total; never fails.
constexpr u32 crc32_final(u32 running) { return running ^ 0xFFFFFFFFu; }

// The CRC-32 of one contiguous block - the common case. p may be null only when n == 0.
inline u32 crc32(const void* p, u64 n) { return crc32_final(crc32_update(CRC32_INIT, p, n)); }

// The variant's published check value: the CRC-32/ISO-HDLC of the nine ASCII bytes "123456789"
// is 0xCBF43926. Exposed as a function so the pin is available to any consumer that wants to
// assert the build agrees with the format on disk, not only to this module's own tests.
constexpr u32 CRC32_CHECK_VALUE = 0xCBF43926u;
