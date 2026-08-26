// T0 (docs/NETCODE.md §20.6): every §20.2 wire struct - sizeof/offsetof pins, little-endian
// round-trip, pad-nonzero refusal, newer-version refusal, truncated-buffer refusal.
//
// Driven off each struct's own reflection table rather than 26 hand-written round-trips: a
// hand-written one tests the struct the author remembered, and the failure mode this test exists
// to catch is a struct nobody remembered. TL_NET_WIRE_STRUCTS below is the roll call, and the
// count assert at the bottom fails when §20.2 grows a struct that was not added to it.
#include "runner/tl_test.h"
#include "net/net_test_util.h"

// The roll call lives in net/wire.h and DECLARES the structs, so a struct missing from it does
// not exist and this file cannot silently stop covering one.

// Fills every NON-pad field with bytes derived from (seed, field index, element) and sets
// format_version to the current one. Pads stay zero: a filled pad is what the refusal row below
// tests deliberately, and would otherwise make every round-trip fail for the wrong reason.
static void wl_fill(const ComponentInfo* info, void* row, u64 seed) {
    u8* base = (u8*)row;
    // Zero first: the callers reuse one stack buffer across structs, and a pad left holding the
    // PREVIOUS struct's bytes would be written to the wire and then correctly refused - a test
    // bug that presents exactly like a codec bug.
    memset(base, 0, (usize)info->size);
    for (u32 i = 0; i < info->field_count; ++i) {
        const FieldInfo* f = &info->fields[i];
        if (tl_field_is_pad(f)) { continue; }
        for (u32 b = 0; b < f->size; ++b) {
            base[f->offset + b] = (u8)(nt_mix64(seed, ((u64)i << 20) | b) & 0xFFu);
        }
    }
    const u32 fv = NET_FORMAT_VERSION;
    memcpy(base, &fv, sizeof(fv));   // field 0, always at offset 0
}

// Writes `row` through the little-endian pair and returns the bytes written.
static u64 wl_write(const ComponentInfo* info, const void* row, u8* buf, u64 cap) {
    ByteWriter w;
    bw_init(&w, buf, cap);
    tl_wire_put_row(&w, info->fields, info->field_count, row);
    return w.len;
}

TL_TEST(wire_every_struct_round_trips_little_endian, "net,wire,layout,smoke,fast") {
    u8 src[256], dst[256], buf[256];
#define WL_ROUND_TRIP(Name, Size)                                                             \
    do {                                                                                      \
        TL_ASSERT_EQ(sizeof(Name), (u64)(Size));                                              \
        TL_ASSERT_LE(sizeof(Name), sizeof(src));                                              \
        const ComponentInfo* info = &Name##_info;                                             \
        /* the table describes exactly the struct: no field outside it, no byte unaccounted */ \
        TL_EXPECT_EQ((u64)info->size, (u64)sizeof(Name));                                     \
        wl_fill(info, src, 0x5EEDu + (u64)(Size));                                            \
        const u64 n = wl_write(info, src, buf, sizeof(buf));                                  \
        /* a wire struct's encoded length is its sizeof: it is explicitly padded, so there is  \
           no host padding to skip and no compression to account for */                        \
        TL_EXPECT_EQ(n, (u64)sizeof(Name));                                                   \
        ByteReader r;                                                                          \
        br_init(&r, buf, n);                                                                   \
        TL_EXPECT_EQ(tl_wire_get_row(&r, info->fields, info->field_count, dst), ERR_OK);      \
        TL_EXPECT_EQ(memcmp(src, dst, sizeof(Name)), 0);                                      \
        TL_EXPECT_EQ(r.pos, r.len);                                                            \
    } while (0);
    TL_NET_WIRE_STRUCTS(WL_ROUND_TRIP)
#undef WL_ROUND_TRIP
}

TL_TEST(wire_every_struct_encodes_low_byte_first, "net,wire,layout,fast") {
    // The round-trip above passes on a big-endian host too if write and read agree with each
    // other. This pins the actual byte ORDER: a u64 field set to 1 puts its 0x01 in the field's
    // FIRST wire byte, never its last. One field per struct is enough - they share one engine.
    u8 row[256], buf[256];
#define WL_ORDER(Name, Size)                                                                  \
    do {                                                                                      \
        const ComponentInfo* info = &Name##_info;                                             \
        memset(row, 0, sizeof(Name));                                                          \
        const u32 fv = NET_FORMAT_VERSION;                                                     \
        memcpy(row, &fv, sizeof(fv));                                                          \
        /* find the first multi-byte non-pad field after format_version and set it to 1 */     \
        u32 chosen = 0xFFFFFFFFu;                                                              \
        for (u32 i = 1; i < info->field_count; ++i) {                                          \
            const FieldInfo* f = &info->fields[i];                                             \
            if (!tl_field_is_pad(f) && kind_scalar_size(f->kind) > 1u) { chosen = i; break; }  \
            }                                                                                  \
        if (chosen != 0xFFFFFFFFu) {                                                           \
            const FieldInfo* f = &info->fields[chosen];                                        \
            row[f->offset] = 1u;   /* host LE low byte; the wire must agree */                 \
            const u64 n = wl_write(info, row, buf, sizeof(buf));                               \
            TL_EXPECT_EQ(n, (u64)sizeof(Name));                                                \
            TL_EXPECT_EQ(buf[f->offset], (u8)1u);                                              \
            for (u32 b = 1; b < kind_scalar_size(f->kind); ++b) {                              \
                TL_EXPECT_EQ(buf[f->offset + b], (u8)0u);                                      \
            }                                                                                  \
        }                                                                                      \
    } while (0);
    TL_NET_WIRE_STRUCTS(WL_ORDER)
#undef WL_ORDER
}

TL_TEST(wire_every_struct_refuses_a_truncated_buffer, "net,wire,layout,edge,fast") {
    // Every prefix shorter than the struct is refused, and refused as DATA (the sticky
    // truncation code), never as a partial decode the caller might act on.
    u8 src[256], dst[256], buf[256];
#define WL_TRUNC(Name, Size)                                                                  \
    do {                                                                                      \
        const ComponentInfo* info = &Name##_info;                                             \
        wl_fill(info, src, 0xC0FFEEu + (u64)(Size));                                          \
        const u64 n = wl_write(info, src, buf, sizeof(buf));                                  \
        /* every prefix: 0 .. n-1. A struct is only decodable in full. */                     \
        for (u64 cut = 0; cut < n; ++cut) {                                                   \
            ByteReader r;                                                                      \
            br_init(&r, buf, cut);                                                             \
            memset(dst, 0xAB, sizeof(Name));                                                   \
            TL_EXPECT_EQ(tl_wire_get_row(&r, info->fields, info->field_count, dst),           \
                         ERR_BYTES_TRUNCATED);                                                 \
        }                                                                                      \
    } while (0);
    TL_NET_WIRE_STRUCTS(WL_TRUNC)
#undef WL_TRUNC
}

TL_TEST(wire_structs_with_pads_refuse_a_nonzero_pad, "net,wire,layout,edge,fast") {
    // docs/NETCODE.md §20.2: readers assert every _padN is zero. A nonzero pad is a stream from
    // something that is not this format - refused, not tolerated. Structs with no pad field are
    // skipped explicitly rather than silently: the counter below proves the row ran on real ones.
    u8 src[256], dst[256], buf[256];
    u32 with_pads = 0;
#define WL_PAD(Name, Size)                                                                    \
    do {                                                                                      \
        const ComponentInfo* info = &Name##_info;                                             \
        wl_fill(info, src, 0xBADu + (u64)(Size));                                             \
        const u64 n = wl_write(info, src, buf, sizeof(buf));                                  \
        for (u32 i = 0; i < info->field_count; ++i) {                                          \
            const FieldInfo* f = &info->fields[i];                                             \
            if (!tl_field_is_pad(f)) { continue; }                                             \
            ++with_pads;                                                                       \
            for (u32 b = 0; b < f->size; ++b) {                                                \
                buf[f->offset + b] = 0x01u;   /* one pad byte at a time */                     \
                ByteReader r;                                                                  \
                br_init(&r, buf, n);                                                           \
                TL_EXPECT_EQ(tl_wire_get_row(&r, info->fields, info->field_count, dst),       \
                             ERR_WIRE_PAD_NONZERO);                                            \
                buf[f->offset + b] = 0x00u;                                                    \
            }                                                                                  \
            /* zeroed again, the same bytes decode cleanly - the pad was the only objection */ \
            ByteReader ok;                                                                     \
            br_init(&ok, buf, n);                                                              \
            TL_EXPECT_EQ(tl_wire_get_row(&ok, info->fields, info->field_count, dst), ERR_OK); \
        }                                                                                      \
    } while (0);
    TL_NET_WIRE_STRUCTS(WL_PAD)
#undef WL_PAD
    TL_EXPECT_GT(with_pads, 0u);   // an empty selection is a failure (docs/TESTING.md §1)
}

TL_TEST(wire_every_struct_refuses_a_newer_format_version, "net,wire,layout,edge,fast") {
    // The generated reader decodes field 0 and hands it back; wire_check_version is where the
    // policy lives. Both halves are exercised here: the value survives the decode, and the
    // policy refuses it - an older or equal one is accepted.
    u8 src[256], dst[256], buf[256];
#define WL_VER(Name, Size)                                                                    \
    do {                                                                                      \
        const ComponentInfo* info = &Name##_info;                                             \
        wl_fill(info, src, 0x1234u + (u64)(Size));                                            \
        u64 n = wl_write(info, src, buf, sizeof(buf));                                        \
        const u32 newer = NET_FORMAT_VERSION + 1u;                                            \
        memcpy(buf, &newer, sizeof(newer));   /* field 0 is LE at offset 0 both sides */      \
        ByteReader r;                                                                          \
        br_init(&r, buf, n);                                                                   \
        TL_EXPECT_EQ(tl_wire_get_row(&r, info->fields, info->field_count, dst), ERR_OK);      \
        u32 got = 0;                                                                           \
        memcpy(&got, dst, sizeof(got));                                                        \
        TL_EXPECT_EQ(got, newer);                                                              \
        TL_EXPECT_EQ(wire_check_version(got), ERR_NET_VERSION);                                \
        const u32 cur = NET_FORMAT_VERSION;                                                    \
        memcpy(buf, &cur, sizeof(cur));                                                        \
        ByteReader r2;                                                                          \
        br_init(&r2, buf, n);                                                                   \
        TL_EXPECT_EQ(tl_wire_get_row(&r2, info->fields, info->field_count, dst), ERR_OK);     \
        memcpy(&got, dst, sizeof(got));                                                        \
        TL_EXPECT_EQ(wire_check_version(got), ERR_OK);                                         \
    } while (0);
    TL_NET_WIRE_STRUCTS(WL_VER)
#undef WL_VER
}

TL_TEST(wire_repeated_element_structs_are_pinned, "net,wire,layout,fast") {
    // The three §20.2 structs with no per-element format_version (see wire.h's note): they are
    // elements inside an already-versioned container, so they carry pins but no version field.
    TL_EXPECT_EQ(sizeof(CheckpointArenaEntry), (u64)16u);
    TL_EXPECT_EQ((u64)offsetof(CheckpointArenaEntry, id), (u64)0u);
    TL_EXPECT_EQ((u64)offsetof(CheckpointArenaEntry, used), (u64)8u);

    TL_EXPECT_EQ(sizeof(ChainRecord), (u64)184u);
    TL_EXPECT_EQ((u64)offsetof(ChainRecord, entry), (u64)0u);
    TL_EXPECT_EQ((u64)offsetof(ChainRecord, hash), (u64)152u);
    TL_EXPECT_EQ(sizeof(ChainEntry), (u64)152u);   // ChainRecord's first member, hence its offset

    TL_EXPECT_EQ(sizeof(ArchiveStreamHeader), (u64)8u);
    TL_EXPECT_EQ((u64)offsetof(ArchiveStreamHeader, record_count), (u64)0u);
    TL_EXPECT_EQ((u64)offsetof(ArchiveStreamHeader, channel), (u64)4u);
    TL_EXPECT_EQ((u64)offsetof(ArchiveStreamHeader, slot), (u64)5u);
    TL_EXPECT_EQ((u64)offsetof(ArchiveStreamHeader, _pad0), (u64)6u);
}

TL_TEST(wire_kind_enums_match_the_spec_values, "net,wire,fast") {
    // Hand-transcribed from docs/NETCODE.md §20.2's `kind` comments. 0 is unused in every one,
    // so a zero-filled buffer never presents as a valid kind.
    TL_EXPECT_EQ((u8)PK_UPSTREAM, (u8)1u);   TL_EXPECT_EQ((u8)PK_DOWNSTREAM, (u8)2u);
    TL_EXPECT_EQ((u8)PK_MIRROR, (u8)3u);     TL_EXPECT_EQ((u8)PK_KEEPALIVE, (u8)4u);

    TL_EXPECT_EQ((u8)LR_JOIN, (u8)1u);    TL_EXPECT_EQ((u8)LR_LEAVE, (u8)2u);
    TL_EXPECT_EQ((u8)LR_SUSPECT, (u8)3u); TL_EXPECT_EQ((u8)LR_RESUME, (u8)4u);
    TL_EXPECT_EQ((u8)LR_EVICT, (u8)5u);   TL_EXPECT_EQ((u8)LR_DELAY, (u8)6u);
    TL_EXPECT_EQ((u8)LR_EPOCH, (u8)7u);   TL_EXPECT_EQ((u8)LR_CUSTODY, (u8)8u);
    TL_EXPECT_EQ((u8)LR_END, (u8)9u);

    TL_EXPECT_EQ((u8)CK_SUSPICION, (u8)1u);   TL_EXPECT_EQ((u8)CK_LOBBY_PROBE, (u8)8u);
    TL_EXPECT_EQ((u8)BK_JOIN_CHALLENGE, (u8)1u); TL_EXPECT_EQ((u8)BK_ACK, (u8)8u);

    TL_EXPECT_EQ((u32)ARCHIVE_CH_POINTER_X, 32u);
    TL_EXPECT_EQ((u32)ARCHIVE_CH_POINTER_Y, 33u);
    TL_EXPECT_EQ((u32)ARCHIVE_CH_FLAG_ESCAPE, 34u);
    TL_EXPECT_EQ(ARCHIVE_CH_COUNT, 36u);
}

// wire.h's roll call declares the structs, so coverage is structural rather than asserted: a
// struct absent from it does not compile. The count is still pinned, to make GROWING §20.2 a
// deliberate edit here as well as there.
#define WL_COUNT_ONE(Name, Size) + 1u
static_assert((0u TL_NET_WIRE_STRUCTS(WL_COUNT_ONE)) == 23u,
              "docs/NETCODE.md §20.2 grew or shrank: update this count with the roll call");
#undef WL_COUNT_ONE
