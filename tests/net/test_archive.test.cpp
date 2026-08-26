// T2 (docs/NETCODE.md §20.6): the archive segment codec - segment round-trip, self-contained
// decode from a delta_tick = 0 state, the flag-escape path, crc32 detection of every single-byte
// corruption, and the 30-minute synthetic 3-peer size target (< 80 KB).
//
// The RecordedInput-through-Replay-producer row of T2 is DEFERRED to Phase 2's gate by the RR-17
// ruling (2026-08-26): that producer is docs/INPUT.md §9.4's, a W3 loop+input deliverable.
//
// Spec: docs/NETCODE.md §20.2.9 (layout and per-channel record rules), §13.3 (the encoding),
// §13.4 (the size target). Buffers come from a VMemArena, never the stack: 300 ticks x 3 slots
// of 76-byte frames is 68 KB, and a Windows child process gets a 1 MB stack (LESSONS).
#include "runner/tl_test.h"
#include "net/net_test_util.h"
#include "foundation/vmem_arena.h"
#include "foundation/crc32.h"
#include <stdio.h>
#include "foundation/vmem_test_api.h"

enum : u32 {
    AR_SEG_TICKS     = 300u,      // CHECKPOINT_HOT_TICKS (docs/CANON.md) - a segment's length
    AR_SESSION_TICKS = 108000u,   // 30 min at 60 Hz (docs/NETCODE.md §13.3)
    AR_SIZE_PEERS    = 3u,        // T2's "30-min synthetic 3-peer"
};

// One file id for every fixture in this file: segments name their file, they no longer repeat
// the build and session identity (RR ruling 2026-08-26, option A).
enum : u32 { AR_FILE_ID = 0xFEED0001u };

// The segment header's geometry, named once rather than spelled as literals at a dozen call
// sites - the option-A ruling moved every one of them and each literal was a separate edit.
enum : u32 {
    AR_T_HDR       = (u32)sizeof(ArchiveSegmentHeader),   // 56
    AR_T_TICKS_OFF = 16u,
    AR_T_MASK_OFF  = 20u,
    AR_T_RECS_OFF  = 24u,
    AR_T_LOGN_OFF  = 28u,
    AR_T_PBYTES_OFF= 36u,
    AR_T_PCRC_OFF  = 40u,
    AR_T_HCRC_OFF  = 48u,
    AR_T_HCRC_SPAN = 48u,
};
static_assert(AR_T_HDR == 56u, "the option-A ruling put the segment header at 56 bytes");

// Repairs both checksums after a forger has edited the bytes.
static void ar_repair_crcs(u8* seg, u64 n) {
    const u32 pcrc = crc32(seg + AR_T_HDR, n - AR_T_HDR);
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_PCRC_OFF + i] = (u8)((pcrc >> (8u * i)) & 0xFFu); }
    const u32 hcrc = crc32(seg, AR_T_HCRC_SPAN);
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_HCRC_OFF + i] = (u8)((hcrc >> (8u * i)) & 0xFFu); }
}

static void ar_ids(u8 build_id[32], u8 fingerprint[32]) {
    for (u32 i = 0; i < 32u; ++i) { build_id[i] = (u8)(i + 1u); fingerprint[i] = (u8)(0x40u + i); }
}

// Encode `slot_count` slots' frames as one segment into `buf`; returns the byte count.
static u64 ar_encode(u8* buf, u64 cap, const WireFrame* per_slot, u32 slot_count, u32 tick_count,
                     const LogRecord* records, u32 record_count, u32 segment_seq) {
    ArchiveInput inputs[MAX_PEERS];
    for (u32 s = 0; s < slot_count; ++s) {
        inputs[s].frames = per_slot + (u64)s * tick_count;
        inputs[s].slot = s;
        inputs[s]._pad0 = 0u;
    }
    ByteWriter w;
    bw_init(&w, buf, cap);
    return archive_encode_segment(&w, 1000u, tick_count, inputs, slot_count,
                                  records, record_count, segment_seq, AR_FILE_ID);
}

// A synthetic peer's frame stream: mostly still, with occasional presses, an analog axis that
// ramps, and a pointer that moves in straight runs - the shape the transition encoding is FOR.
// A uniformly random stream would measure the worst case, not the design target.
static void ar_synth_frames(WireFrame* out, u32 tick_count, u32 slot, u64 seed,
                            bool busy = false) {
    i32 px = 1000 + (i32)slot * 37;
    i32 py = -500 - (i32)slot * 11;
    i32 vx = 0, vy = 0;
    u8 down[NET_FRAME_MAX_ACTIONS] = {};
    i8 axis = 0;
    for (u32 i = 0; i < tick_count; ++i) {
        const u64 h = nt_mix64(seed ^ slot, i);
        // A button changes roughly every 40 ticks - ~5,000-8,000 transitions over 30 minutes,
        // "set by the human, not the clock" (docs/NETCODE.md §13.3). `busy` raises that rate for
        // the FUZZ corpus only: at the session rate a 5-tick fuzz segment averages 0.23 toggles,
        // so 97% of its streams were pointer streams and it produced 22 escape streams in a
        // million segments - it was a pointer-channel fuzz wearing an archive fuzz's name
        // (round 4 finding F2). The size measurement keeps the session rate, which is the point
        // of that row.
        if ((h & (busy ? 0x3u : 0x3Fu)) == 0u) {
            const u32 a = (u32)((h >> 8) % 12u);      // a dozen actions see real use
            down[a] = (u8)(down[a] ? 0u : 1u);
        }
        // The pointer moves in straight runs: a new velocity every ~90 ticks.
        if (((h >> 16) & 0x7Fu) == 0u) {
            vx = (i32)((h >> 24) & 0x7u) - 3;
            vy = (i32)((h >> 32) & 0x7u) - 3;
        }
        if (((h >> 40) & 0xFFu) == 0u) { axis = (i8)((h >> 48) & 0x7Fu); }

        WireFrame f = wire_zero_frame();
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            if (down[a]) {
                f.actions[a].value = 1;
                f.actions[a].flags = 1u;
                // The derived edge: pressed on the tick the bit rose.
                const bool prev_down = (i > 0u) && (out[i - 1].actions[a].flags & 1u) != 0u;
                if (!prev_down) { f.actions[a].flags = (u8)(1u | 2u); }
            } else if (i > 0u && (out[i - 1].actions[a].flags & 1u) != 0u) {
                f.actions[a].flags = 4u;   // released
            }
        }
        f.actions[5].value = axis;                       // an analog axis, down bit clear
        if (axis != 0) { f.actions[5].flags = 0u; }
        if (busy) {
            // Flags the derived edges cannot produce, so the escape channel is actually
            // exercised: "pressed" with no down bit, and "released" while still down.
            if (((h >> 52) & 0x7u) == 0u) { f.actions[9].value = 0; f.actions[9].flags = 2u; }
            if (((h >> 56) & 0x7u) == 0u) { f.actions[11].value = 1; f.actions[11].flags = 5u; }
        }
        px += vx; py += vy;
        f.pointer_x = px;
        f.pointer_y = py;
        f.tick = i;
        out[i] = f;
    }
}

TL_TEST(archive_segment_round_trips_three_slots, "net,archive,smoke,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C11u, 8u << 20, 0u, &api), ERR_OK);

    const u32 ticks = 64u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * AR_SIZE_PEERS, 16u);
    TL_ASSERT_NOT_NULL(src);
    for (u32 s = 0; s < AR_SIZE_PEERS; ++s) { ar_synth_frames(src + (u64)s * ticks, ticks, s, 0x2E57u); }

    LogRecord recs[2] = {};
    for (u32 i = 0; i < 2u; ++i) {
        recs[i].format_version = NET_FORMAT_VERSION;
        recs[i].kind = (u8)LR_DELAY;
        recs[i].slot = (u8)i;
        recs[i].origin_slot = 0u;
        recs[i]._pad0 = 0u;
        recs[i].seq = i;
        recs[i].payload = 4u + i;
        recs[i].effective_tick = 1000u + (u64)i * 10u;
    }

    const u64 cap = archive_segment_max_bytes(AR_SIZE_PEERS, ticks, 2u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    TL_ASSERT_NOT_NULL(buf);
    const u64 n = ar_encode(buf, cap, src, AR_SIZE_PEERS, ticks, recs, 2u, 7u);
    TL_ASSERT_GT(n, (u64)0);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord got_recs[8] = {};
    u32 got_rec_count = 0;
    ByteReader r;
    br_init(&r, buf, n);
    TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, ticks, got_recs, 8u, &got_rec_count), ERR_OK);

    TL_EXPECT_EQ(h.format_version, NET_FORMAT_VERSION);
    TL_EXPECT_EQ(h.base_tick, (u64)1000u);
    TL_EXPECT_EQ(h.tick_count, ticks);
    TL_EXPECT_EQ(h.max_actions, NET_FRAME_MAX_ACTIONS);
    TL_EXPECT_EQ(h.slot_mask, (u8)0x07u);         // slots 0,1,2
    TL_EXPECT_EQ(h.segment_seq, 7u);
    TL_EXPECT_EQ(got_rec_count, 2u);

    for (u32 s = 0; s < AR_SIZE_PEERS; ++s) {
        for (u32 i = 0; i < ticks; ++i) {
            TL_ASSERT_TRUE(nt_frames_equal_payload(&src[(u64)s * ticks + i], &got[(u64)s * ticks + i]));
        }
    }
    // A slot outside slot_mask decodes as ZERO (docs/NETCODE.md §20.2.9).
    for (u32 i = 0; i < ticks; ++i) {
        const WireFrame z = wire_zero_frame();
        TL_ASSERT_TRUE(nt_frames_equal_payload(&z, &got[(u64)7 * ticks + i]));
    }
    for (u32 i = 0; i < 2u; ++i) {
        TL_EXPECT_EQ(got_recs[i].kind, (u8)LR_DELAY);
        TL_EXPECT_EQ(got_recs[i].seq, i);
        TL_EXPECT_EQ(got_recs[i].effective_tick, 1000u + (u64)i * 10u);
    }
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_segment_is_self_contained_from_a_nonzero_state, "net,archive,edge,fast") {
    // docs/NETCODE.md §20.2.9: "Every stream is self-contained: a non-ZERO state at base_tick is
    // emitted as a delta_tick = 0 record." A segment whose very first tick already has buttons
    // down and the pointer far from the origin must decode without its predecessor.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C12u, 4u << 20, 0u, &api), ERR_OK);

    const u32 ticks = 8u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) {
        src[i] = wire_zero_frame();
        src[i].actions[0].value = 1; src[i].actions[0].flags = 1u;   // held from tick 0
        src[i].actions[3].value = (i8)-77; src[i].actions[3].flags = 0u;
        src[i].pointer_x = 123456 + (i32)i;
        src[i].pointer_y = -98765 - (i32)i * 2;
    }
    const u64 cap = archive_segment_max_bytes(1u, ticks, 0u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(buf, cap, src, 1u, ticks, nullptr, 0u, 0u);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, buf, n);
    TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    for (u32 i = 0; i < ticks; ++i) { TL_ASSERT_TRUE(nt_frames_equal_payload(&src[i], &got[i])); }
    // A segment that starts mid-hold pays one escape per held action: the DERIVED edge at tick 0
    // is "pressed" (down with no previous down), but this source frame says plain down, so the
    // escape channel carries the correction. The value that comes back is the source's.
    TL_EXPECT_EQ(got[0].actions[0].flags, (u8)1u);
    TL_EXPECT_EQ(got[0].pointer_x, 123456);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_flag_escape_carries_undrivable_edges, "net,archive,edge,fast") {
    // The escape channel exists for a frame whose stored pressed/released differ from the edges
    // derived from the down bits (docs/NETCODE.md §20.2.9). Without it these frames would decode
    // to the derived flags and the round-trip would silently lose them.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C13u, 4u << 20, 0u, &api), ERR_OK);

    const u32 ticks = 6u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = wire_zero_frame(); }
    // Down the whole time, but claiming "pressed" on a tick that is not a rising edge, and
    // "released" while still down - neither is derivable.
    for (u32 i = 0; i < ticks; ++i) { src[i].actions[2].value = 1; src[i].actions[2].flags = 1u; }
    src[3].actions[2].flags = (u8)(1u | 2u);
    src[4].actions[2].flags = (u8)(1u | 4u);
    // Two actions escaping on ONE tick - the case that makes delta_tick 0 legal on this channel.
    src[4].actions[6].value = 0; src[4].actions[6].flags = 2u;

    const u64 cap = archive_segment_max_bytes(1u, ticks, 0u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(buf, cap, src, 1u, ticks, nullptr, 0u, 0u);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, buf, n);
    TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    for (u32 i = 0; i < ticks; ++i) { TL_ASSERT_TRUE(nt_frames_equal_payload(&src[i], &got[i])); }
    TL_EXPECT_EQ(got[3].actions[2].flags, (u8)3u);
    TL_EXPECT_EQ(got[4].actions[2].flags, (u8)5u);
    TL_EXPECT_EQ(got[4].actions[6].flags, (u8)2u);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_crc32_detects_every_single_byte_corruption, "net,archive,edge,fast") {
    // docs/NETCODE.md §20.6 T2: "crc32 detection of every single-byte corruption". Every byte of
    // the segment, flipped, must be refused - header bytes by the header crc, payload bytes by
    // the payload crc, and the crc fields themselves by disagreeing with what they cover.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C14u, 8u << 20, 0u, &api), ERR_OK);

    const u32 ticks = 24u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * 2u, 16u);
    for (u32 s = 0; s < 2u; ++s) { ar_synth_frames(src + (u64)s * ticks, ticks, s, 0xC0C0u); }
    const u64 cap = archive_segment_max_bytes(2u, ticks, 0u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(buf, cap, src, 2u, ticks, nullptr, 0u, 3u);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    u32 refused = 0;
    for (u64 pos = 0; pos < n; ++pos) {
        const u8 old = buf[pos];
        buf[pos] = (u8)(old ^ 0xFFu);
        ByteReader r;
        br_init(&r, buf, n);
        const ErrCode e = archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc);
        if (e != ERR_OK) { ++refused; }
        buf[pos] = old;
    }
    // Counted from the outcome, not incremented unconditionally after an assert - as written
    // before, this comparison could not fail and only read as coverage.
    TL_EXPECT_EQ(refused, (u32)n);
    // Restored, it decodes again - the corruption was the only objection.
    ByteReader r;
    br_init(&r, buf, n);
    TL_EXPECT_EQ(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_refuses_a_truncated_segment, "net,archive,edge,fast") {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C15u, 4u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 12u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    ar_synth_frames(src, ticks, 0u, 0x7Cu);
    const u64 cap = archive_segment_max_bytes(1u, ticks, 0u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(buf, cap, src, 1u, ticks, nullptr, 0u, 0u);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    for (u64 cut = 0; cut < n; ++cut) {
        ByteReader r;
        br_init(&r, buf, cut);
        TL_ASSERT_NE(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    }
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_thirty_minute_three_peer_session_size, "net,archive,size,slow") {
    // Phase 1's done criterion (docs/NETCODE.md §20.8) and T2's last row: a 30-minute synthetic
    // 3-peer input archives to under 80 KB. 108,000 ticks in 300-tick segments = 360 segments.
    // docs/NETCODE.md §13.4 targets ~165 KB for EIGHT peers, so three should land near 60 KB.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C16u, 64u << 20, 0u, &api), ERR_OK);

    WireFrame* src = (WireFrame*)arena_push(&arena,
        sizeof(WireFrame) * AR_SEG_TICKS * AR_SIZE_PEERS, 16u);
    TL_ASSERT_NOT_NULL(src);
    const u64 cap = archive_segment_max_bytes(AR_SIZE_PEERS, AR_SEG_TICKS, 0u);
    u8* buf = (u8*)arena_push(&arena, cap, 16u);
    TL_ASSERT_NOT_NULL(buf);
    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena,
        sizeof(WireFrame) * AR_SEG_TICKS * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;

    u64 total = 0;
    const u32 segments = AR_SESSION_TICKS / AR_SEG_TICKS;
    for (u32 seg = 0; seg < segments; ++seg) {
        for (u32 s = 0; s < AR_SIZE_PEERS; ++s) {
            // Each segment continues the session: the seed advances with the segment index, so
            // this is 30 minutes of distinct input, not one segment measured 360 times.
            ar_synth_frames(src + (u64)s * AR_SEG_TICKS, AR_SEG_TICKS, s,
                            nt_mix64(0x3017u, seg));
        }
        const u64 n = ar_encode(buf, cap, src, AR_SIZE_PEERS, AR_SEG_TICKS, nullptr, 0u, seg);
        total += n;
        // Every segment must also decode - a size number for a stream that does not round-trip
        // would be measuring the wrong thing.
        ByteReader r;
        br_init(&r, buf, n);
        TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, AR_SEG_TICKS, none, 1u, &rc), ERR_OK);
    }

    // THE PHASE 1 GATE (docs/NETCODE.md §20.8, §20.6 T2): a 30-minute synthetic 3-peer archive
    // under 80 KB. Asserted, not merely measured - the option-A framing ruling (2026-08-26) took
    // this from 124.1 KB to 64.1 KB by shrinking the stream header to two uvarints and moving
    // the build/session identity out of every segment into one file header.
    TL_EXPECT_LT(total, (u64)(80u * 1024u));
    // A floor as well as a ceiling: a sudden collapse means the fixture stopped producing input,
    // not that the encoder got better. The 30-min 8-peer figure docs/NETCODE.md §13.4 targets is
    // ~165 KB, so three peers landing near 60 KB is the shape the doc predicts.
    TL_EXPECT_GT(total, (u64)(40u * 1024u));
    // The measurement itself, printed so the phase gate can copy it into LESSONS.md with the
    // build_id (docs/NETCODE.md §20.8). tests/ may use printf-class io (docs/TESTING.md §8 R-2).
    fprintf(stderr, "archive: 30-min 3-peer = %llu bytes (%.1f KB) in %u segments - "
                    "Phase 1 gate is < 80 KB\n",
            (unsigned long long)total, (double)total / 1024.0, segments);
    api.release(api.ctx, arena.base, arena.reserved);
}

// --- the rows finding 5 of the 2026-08-26 adversarial review said were missing ----------------
// Every archive byte above is produced by the encoder and fed straight back to the decoder, so
// nothing above can see a format with TWO spellings of one segment. That is how four
// canonicality holes survived T2. These two rows close the gap: a mutation fuzz with a re-encode
// comparison (the shape LESSONS.md already prescribes) and one hand-transcribed segment.

// Re-encodes the frames a decode produced, for the slots the header named, so the result can be
// compared byte for byte with what was consumed.
static u64 ar_reencode(u8* out, u64 cap, const ArchiveSegmentHeader* h, const WireFrame* frames,
                       const LogRecord* recs, u32 rec_count) {
    // file_id comes from the DECODED header: it is decoded content like everything else, so
    // canonicality means re-encoding reproduces what was read.
    ArchiveInput in[MAX_PEERS];
    u32 n = 0;
    for (u32 s = 0; s < MAX_PEERS; ++s) {
        if (((h->slot_mask >> s) & 1u) == 0u) { continue; }
        in[n].frames = frames + (u64)s * h->tick_count;
        in[n].slot = s;
        in[n]._pad0 = 0u;
        ++n;
    }
    ByteWriter w;
    bw_init(&w, out, cap);
    return archive_encode_segment(&w, h->base_tick, h->tick_count, in, n, recs, rec_count,
                                  h->segment_seq, h->file_id);
}

// The body of the mutation property. `structural` adds whole-stream edits (delete, duplicate,
// truncate) and slot_mask widening on top of bit flips: review round 2 measured that a single
// payload bit-flip cannot delete a stream or reach tick_count == 0, so bit flips alone could not
// see three of the four holes round 1 found. The header is in the mutation domain too, and BOTH
// crc32s are repaired afterwards - otherwise the checksums refuse everything and the row tests
// the crc rather than the codec.
static void ar_fuzz_cases(TestCtx* t, u64 seed_base, u32 cases, bool structural) {
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C21u, 64u << 20, 0u, &api), ERR_OK);

    const u32 max_ticks = 11u;   // wider than 5: multi-byte varints and longer runs
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * max_ticks * MAX_PEERS, 16u);
    const u64 cap = archive_segment_max_bytes(MAX_PEERS, max_ticks, 8u * max_ticks);
    u8* buf   = (u8*)arena_push(&arena, cap, 16u);
    u8* again = (u8*)arena_push(&arena, cap, 16u);
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * max_ticks * MAX_PEERS, 16u);
    u32 rc = 0;

    u32 accepted = 0, refused = 0;
    for (u32 c = 0; c < cases; ++c) {
        const u64 seed = nt_mix64(seed_base, c);
        // Shape varies too: tick_count 0..5 and slot_count 0..3, so the degenerate cases are
        // inside the corpus rather than outside it.
        const u32 ticks = (u32)(seed % (u64)max_ticks);
        const u32 slots = (u32)((seed >> 8) % (u64)(MAX_PEERS + 1u));
        for (u32 sl = 0; sl < slots; ++sl) {
            ar_synth_frames(src + (u64)sl * (ticks ? ticks : 1u), ticks, sl, seed, true);
        }
        ArchiveInput in[MAX_PEERS];
        for (u32 sl = 0; sl < slots; ++sl) {
            in[sl].frames = src + (u64)sl * (ticks ? ticks : 1u);
            in[sl].slot = sl;
            in[sl]._pad0 = 0u;
        }
        // Log records are IN the corpus: round 3 found the whole array outside it, so none of
        // its validation was fuzzed. Counts stay inside the format bound
        // (MAX_LOG_RECORDS_PER_PACKET per tick) and ids stay unique and ascending.
        LogRecord lr[4] = {};
        u32 lr_count = ticks == 0u ? 0u : (u32)((seed >> 40) % 5u);
        if ((u64)lr_count > (u64)MAX_LOG_RECORDS_PER_PACKET * (u64)ticks) { lr_count = 0u; }
        for (u32 i = 0; i < lr_count; ++i) {
            lr[i].format_version = NET_FORMAT_VERSION;
            lr[i].kind = (u8)(LR_JOIN + (u8)(nt_mix64(seed, 300u + i) % 9u));
            lr[i].slot = (u8)(nt_mix64(seed, 400u + i) % MAX_PEERS);
            lr[i].origin_slot = (u8)(i % MAX_PEERS);
            lr[i].seq = i;
            lr[i].effective_tick = 1000u + (u64)(i % ticks);
        }
        // The array must be ascending by (effective_tick, origin_slot, seq) for the encoder.
        for (u32 i = 1; i < lr_count; ++i) {
            if (!(lr[i - 1].effective_tick < lr[i].effective_tick
               || (lr[i - 1].effective_tick == lr[i].effective_tick
                   && lr[i - 1].origin_slot < lr[i].origin_slot))) { lr_count = i; break; }
        }
        ByteWriter w;
        bw_init(&w, buf, cap);
        u64 n = archive_encode_segment(&w, 1000u, ticks, in, slots, lr, lr_count, c, AR_FILE_ID);
        TL_ASSERT_GE(n, (u64)AR_T_HDR);

        if (!structural || (seed & 1u) == 0u) {
            // One to three bit flips anywhere, header included.
            const u32 flips = 1u + (u32)((seed >> 16) % 3u);
            for (u32 f = 0; f < flips; ++f) {
                const u64 off = nt_mix64(seed, 100u + f) % n;
                buf[off] = (u8)(buf[off] ^ (u8)(1u << (nt_mix64(seed, 200u + f) & 7u)));
            }
        } else {
            // Structural: widen slot_mask, or truncate the stream region, or duplicate a chunk.
            const u32 pick = (u32)((seed >> 24) % 3u);
            if (pick == 0u) {
                buf[AR_T_MASK_OFF] = (u8)(buf[AR_T_MASK_OFF] | (u8)(1u << ((seed >> 32) % MAX_PEERS)));
            } else if (pick == 1u && n > (u64)AR_T_HDR + 8u) {
                n -= 1u + (nt_mix64(seed, 7u) % 8u) % (n - (u64)AR_T_HDR - 1u);
            } else if (n + 8u <= cap && n > (u64)AR_T_HDR + 8u) {
                for (u32 k = 0; k < 8u; ++k) { buf[n + k] = buf[AR_T_HDR + k]; }
                n += 8u;
            }
        }
        // payload_bytes must agree with the length or the decoder rejects on that alone.
        const u32 pb = (u32)(n - AR_T_HDR);
        for (u32 i = 0; i < 4u; ++i) { buf[AR_T_PBYTES_OFF + i] = (u8)((pb >> (8u*i)) & 0xFFu); }
        ar_repair_crcs(buf, n);

        ArchiveSegmentHeader h = {};
        LogRecord out_lr[8] = {};
        ByteReader r;
        br_init(&r, buf, n);
        if (archive_decode_segment(&r, &h, got, max_ticks, out_lr, 8u, &rc) != ERR_OK) {
            ++refused;
            continue;
        }
        ++accepted;
        const u64 n2 = ar_reencode(again, cap, &h, got, out_lr, rc);
        TL_ASSERT_EQ(n2, r.pos);
        TL_ASSERT_EQ(memcmp(buf, again, (usize)r.pos), 0);
    }
    TL_EXPECT_GT(refused, 0u);
    TL_EXPECT_GT(accepted, 0u);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_mutated_segments_are_refused_or_canonical, "net,archive,fuzz,fast") {
    ar_fuzz_cases(t, 0xA12C11ull ^ (u64)t->seed, 3000u, false);
}

TL_TEST(archive_structurally_mutated_segments_are_refused_or_canonical, "net,archive,fuzz,fast") {
    ar_fuzz_cases(t, 0x5712AC71ull ^ (u64)t->seed, 3000u, true);
}

TL_TEST(archive_mutated_segments_are_refused_or_canonical_million, "net,archive,fuzz,slow") {
    ar_fuzz_cases(t, 0xA12C11ull ^ (u64)t->seed, 500000u, false);
    ar_fuzz_cases(t, 0x5712AC71ull ^ (u64)t->seed, 500000u, true);
}

TL_TEST(archive_hand_written_segment_decodes_to_known_frames, "net,archive,golden,fast") {
    // One segment transcribed by hand from docs/NETCODE.md §20.2.9, so the layout is pinned by
    // something other than the encoder that produced it. Slot 0, two ticks: action 0 down for
    // both, pointer (5, -3) then (7, -3).
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C18u, 4u << 20, 0u, &api), ERR_OK);

    const u32 ticks = 2u;
    u8 seg[512];
    for (u32 i = 0; i < sizeof(seg); ++i) { seg[i] = 0u; }
    // --- header, field by field, little-endian (docs/NETCODE.md §20.2.9 as amended) ---
    u32 o = 0;
    auto put32 = [&](u32 v) { for (u32 i = 0; i < 4u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 4u; };
    auto put64 = [&](u64 v) { for (u32 i = 0; i < 8u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 8u; };
    auto putvar = [&](u32 v) { while (v >= 0x80u) { seg[o++] = (u8)(v | 0x80u); v >>= 7; } seg[o++] = (u8)v; };
    put32(NET_FORMAT_VERSION);            //  0 format_version
    put32(NET_FRAME_MAX_ACTIONS);         //  4 max_actions
    put64(1000u);                         //  8 base_tick
    put32(ticks);                         // 16 tick_count
    seg[20] = 0x01u; o = 24u;             // 20 slot_mask = slot 0; 21..23 _pad0
    put32(5u);                            // 24 record_count: 1 action + 2 ptr_x + 1 ptr_y + 1 escape
    put32(0u);                            // 28 log_record_count
    put32(AR_FILE_ID);                    // 32 file_id - the identity lives in the FILE header
    put32(0u);                            // 36 payload_bytes  (patched below)
    put32(0u);                            // 40 payload_crc32  (patched below)
    put32(0u);                            // 44 segment_seq
    put32(0u);                            // 48 header_crc32   (patched below)
    o = AR_T_HDR;                         // 52..55 _pad1, already zero
    const u32 payload_start = o;

    // Each stream header is two canonical uvarints: record_count, then slot*35 + channel.
    // --- slot 0, channel 0 (action 0): one record, tick 0, word = value<<1|down ---
    putvar(1u); putvar(archive_stream_key(0u, 0u));
    seg[o++] = 0x00u;                     // delta_tick 0
    seg[o++] = 0x03u;                     // uvarint(value 1 << 1 | down 1) = 3
    // --- slot 0, channel 32 (pointer_x): absolute 5, then velocity +2 at tick 1 ---
    putvar(2u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_X));
    seg[o++] = 0x00u; seg[o++] = 0x0Au;   // delta 0, svarint(5)  = zigzag 10
    seg[o++] = 0x01u; seg[o++] = 0x04u;   // delta 1, svarint(2)  = zigzag 4
    // --- slot 0, channel 33 (pointer_y): absolute -3, no velocity records ---
    putvar(1u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_Y));
    seg[o++] = 0x00u; seg[o++] = 0x05u;   // delta 0, svarint(-3) = zigzag 5
    // --- slot 0, channel 34 (escape): tick 0's derived flags are down|pressed, but the frames
    //     say plain down, so one escape carries (action 0, flags 1) ---
    putvar(1u); putvar(archive_stream_key(0u, ARCHIVE_CH_FLAG_ESCAPE));
    seg[o++] = 0x00u;                     // delta_tick 0
    seg[o++] = 0x01u;                     // uvarint(action 0 << 3 | flags 1)

    const u32 payload_bytes = o - payload_start;
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_PBYTES_OFF + i] = (u8)((payload_bytes >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(seg, o);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, seg, o);
    TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    TL_EXPECT_EQ(h.tick_count, ticks);
    TL_EXPECT_EQ(h.slot_mask, (u8)0x01u);
    TL_EXPECT_EQ(got[0].actions[0].value, (i8)1);
    TL_EXPECT_EQ(got[0].actions[0].flags, (u8)1u);   // the escape overrode the derived press
    TL_EXPECT_EQ(got[1].actions[0].value, (i8)1);
    TL_EXPECT_EQ(got[1].actions[0].flags, (u8)1u);   // still down, no edge
    TL_EXPECT_EQ(got[0].pointer_x, 5);
    TL_EXPECT_EQ(got[1].pointer_x, 7);
    TL_EXPECT_EQ(got[0].pointer_y, -3);
    TL_EXPECT_EQ(got[1].pointer_y, -3);

    // And the encoder agrees with the hand-written bytes: this is the round trip closing on a
    // stream the code under test did not author.
    u8 again[512];
    const u64 n2 = ar_reencode(again, sizeof(again), &h, got, nullptr, 0u);
    TL_EXPECT_EQ(n2, (u64)o);
    TL_EXPECT_EQ(memcmp(seg, again, (usize)o), 0);
    api.release(api.ctx, arena.base, arena.reserved);
}

// --- regression rows, one per closed canonicality hole ----------------------------------------
// Review round 2 found that reverting ANY of round 1's four fixes left all 46 net rows green:
// the fuzz alone could not see them (a single payload bit-flip cannot delete a stream or reach
// tick_count == 0). A fuzz is the net; these are what stop a specific hole re-opening. Each row
// builds the forged segment BY HAND and requires the decoder to refuse it.

// Builds a valid one-slot segment into `seg`, returns its length. `ticks` frames, action 0 held
// down throughout, pointer still - the smallest shape with all four stream kinds present.
static u64 ar_valid_one_slot(u8* seg, u64 cap, u32 ticks, VMemArena* arena) {
    WireFrame* src = (WireFrame*)arena_push(arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) {
        src[i] = wire_zero_frame();
        src[i].actions[0].value = 1;
        src[i].actions[0].flags = 1u;   // plain down: forces an escape at tick 0
    }
    return ar_encode(seg, cap, src, 1u, ticks, nullptr, 0u, 0u);
}


// Decodes `seg`; returns the ErrCode.
static ErrCode ar_try_decode(const u8* seg, u64 n, u32 ticks, VMemArena* arena) {
    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord recs[8] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, seg, n);
    return archive_decode_segment(&r, &h, got, ticks, recs, 8u, &rc);
}

TL_TEST(archive_regression_escape_may_not_move_the_down_bit, "net,archive,regression,fast") {
    // Round 1 finding 1. The escape channel carries edge bits; the down bit belongs to the
    // action channel. An escape that changes bit 0 is a second spelling of one frame set.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C19u, 4u << 20, 0u, &api), ERR_OK);
    u8 seg[1024];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), 2u, &arena);
    TL_ASSERT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_OK);

    // The escape stream is last; its single record is the final two bytes: (delta 0, word).
    // word = action << 3 | flags, so flipping flags' bit 0 asks the escape to clear `down`.
    const u8 word = seg[n - 1];
    TL_ASSERT_EQ((u32)(word >> 3), 0u);            // action 0
    seg[n - 1] = (u8)(word ^ 1u);                  // flip the down bit inside the escape
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_NET_MALFORMED);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_slot_in_mask_must_carry_both_pointer_streams, "net,archive,regression,fast") {
    // Round 1 finding 2. A slot named in slot_mask with no streams decodes as ZERO - the same
    // frames a segment that omitted the slot produces.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Au, 4u << 20, 0u, &api), ERR_OK);
    u8 seg[1024];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), 3u, &arena);
    TL_ASSERT_EQ(ar_try_decode(seg, n, 3u, &arena), ERR_OK);

    // Widen slot_mask (offset 20) to name slot 1 as well, without adding its streams.
    seg[AR_T_MASK_OFF] = (u8)(seg[AR_T_MASK_OFF] | 0x02u);
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, 3u, &arena), ERR_NET_MALFORMED);

    // And a header claiming only slot 1, whose streams are all slot 0's, is refused too.
    seg[AR_T_MASK_OFF] = 0x02u;
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, 3u, &arena), ERR_NET_MALFORMED);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_stream_key_splits_to_slot_and_channel, "net,archive,regression,fast") {
    // Round 1 finding 3 was "channel 35 aliases onto the escape channel" under the retired
    // byte-per-field header. The option-A key makes channel 35 UNREPRESENTABLE, so that forgery
    // no longer means what it said (round 4 finding F6). What matters now is the split: a key
    // one past the escape channel is slot 1 channel 0, which the slot_mask rule refuses, and a
    // key past the whole space is refused by the bound - the row below covers that half.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Bu, 4u << 20, 0u, &api), ERR_OK);
    u8 seg[1024];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), 2u, &arena);
    TL_ASSERT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_OK);

    // The escape stream is last; its header is uvarint(record_count) then uvarint(key), and for
    // slot 0 both fit one byte. Raise the key from channel 34 to the non-existent channel 35.
    u64 key_off = 0;
    for (u64 i = AR_T_HDR; i + 1u < n; ++i) {
        if (seg[i] == (u8)archive_stream_key(0u, ARCHIVE_CH_FLAG_ESCAPE)) { key_off = i; }
    }
    TL_ASSERT_NE(key_off, (u64)0);
    seg[key_off] = (u8)(archive_stream_key(0u, ARCHIVE_CH_FLAG_ESCAPE) + 1u);   // channel 35
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_NET_MALFORMED);
    TL_EXPECT_EQ((u32)ARCHIVE_CH_MAX, 34u);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_zero_tick_segment_round_trips, "net,archive,regression,edge,fast") {
    // Round 1 finding 4: a zero-tick segment encoded a pointer stream its own decoder refused.
    // Round 2 finding B2: with no streams, slot_mask is unpinnable, so it must be empty.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Cu, 4u << 20, 0u, &api), ERR_OK);
    for (u32 slots = 0; slots <= 3u; ++slots) {
        ArchiveInput in[MAX_PEERS];
        for (u32 s = 0; s < slots; ++s) { in[s].frames = nullptr; in[s].slot = s; in[s]._pad0 = 0u; }
        u8 seg[512];
        ByteWriter w;
        bw_init(&w, seg, sizeof(seg));
        const u64 n = archive_encode_segment(&w, 900u, 0u, in, slots, nullptr, 0u, 0u, AR_FILE_ID);
        TL_ASSERT_EQ(n, (u64)sizeof(ArchiveSegmentHeader));   // header only, whatever the slots
        TL_EXPECT_EQ(seg[AR_T_MASK_OFF], (u8)0u);         // one spelling: slot_mask is empty at zero ticks
        TL_EXPECT_EQ(ar_try_decode(seg, n, 0u, &arena), ERR_OK);
        // A non-empty mask at zero ticks would be 256 spellings of one segment.
        seg[AR_T_MASK_OFF] = 0x01u;
        ar_repair_crcs(seg, n);
        TL_EXPECT_EQ(ar_try_decode(seg, n, 0u, &arena), ERR_NET_MALFORMED);
    }
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_log_array_is_validated, "net,archive,regression,fast") {
    // Round 2 finding A2: the LogRecord array was decoded with no checks at all, so N! orderings
    // of one log set were all valid segments, a repeated R6 id was accepted, and kind 0 -
    // which wire.h says can never occur - decoded fine.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Du, 8u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 4u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }

    LogRecord recs[2] = {};
    for (u32 i = 0; i < 2u; ++i) {
        recs[i].format_version = NET_FORMAT_VERSION;
        recs[i].kind = (u8)LR_DELAY;
        recs[i].slot = 0u;
        recs[i].origin_slot = 0u;
        recs[i].seq = i;
        recs[i].effective_tick = 1000u + i;
    }
    const u64 cap = archive_segment_max_bytes(1u, ticks, 2u);
    u8* seg = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(seg, cap, src, 1u, ticks, recs, 2u, 0u);
    TL_ASSERT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_OK);

    // The array is the last 48 bytes: two 24-byte records. Each forgery is repaired and retried.
    const u64 rec0 = n - 48u;
    const u64 rec1 = n - 24u;
    // (a) descending effective_tick - swap the two records wholesale.
    u8 tmp[24];
    for (u32 i = 0; i < 24u; ++i) { tmp[i] = seg[rec0 + i]; }
    for (u32 i = 0; i < 24u; ++i) { seg[rec0 + i] = seg[rec1 + i]; }
    for (u32 i = 0; i < 24u; ++i) { seg[rec1 + i] = tmp[i]; }
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_NET_MALFORMED);
    // restore
    for (u32 i = 0; i < 24u; ++i) { tmp[i] = seg[rec0 + i]; }
    for (u32 i = 0; i < 24u; ++i) { seg[rec0 + i] = seg[rec1 + i]; }
    for (u32 i = 0; i < 24u; ++i) { seg[rec1 + i] = tmp[i]; }
    ar_repair_crcs(seg, n);
    TL_ASSERT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_OK);

    // (b) a repeated R6 stable id. Note the two records sit at DIFFERENT effective ticks, so
    // they stay correctly ordered and the adjacent-ascending check cannot see the duplicate -
    // only a scan over the whole array can, which is the point of this row.
    const u8 saved_seq = seg[rec1 + 8u];
    seg[rec1 + 8u] = seg[rec0 + 8u];
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_NET_MALFORMED);
    seg[rec1 + 8u] = saved_seq;

    // (c) kind 0 - "0 is deliberately unused, so a zero-filled buffer is never a valid kind".
    const u8 saved_kind = seg[rec0 + 4u];
    seg[rec0 + 4u] = 0u;
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_NET_MALFORMED);
    seg[rec0 + 4u] = saved_kind;

    // (d) effective_tick outside the segment's range (offset 16).
    seg[rec1 + 16u] = 0xFFu;
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_NET_MALFORMED);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_version_zero_is_refused, "net,archive,regression,fast") {
    // Round 2 finding A1: wire_check_version accepted 0 as "an older build's stream", so every
    // segment had two spellings of its header and two more per log record.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Eu, 4u << 20, 0u, &api), ERR_OK);
    u8 seg[1024];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), 2u, &arena);
    TL_ASSERT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_OK);
    for (u32 i = 0; i < 4u; ++i) { seg[i] = 0u; }      // format_version = 0
    ar_repair_crcs(seg, n);
    TL_EXPECT_EQ(ar_try_decode(seg, n, 2u, &arena), ERR_NET_VERSION);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_capacity_shortfall_is_not_a_verdict_on_the_sender, "net,archive,edge,fast") {
    // Round 2 finding B1: a caller's buffer being too small is LOCAL - the same bytes decode
    // fine on a peer with a bigger buffer - so it must not be reported as malformed, which
    // §20.2.5 would turn into a peer-misbehaviour verdict from a local resource limit.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C1Fu, 8u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 4u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }
    LogRecord recs[2] = {};
    for (u32 i = 0; i < 2u; ++i) {
        recs[i].format_version = NET_FORMAT_VERSION;
        recs[i].kind = (u8)LR_DELAY;
        recs[i].seq = i;
        recs[i].effective_tick = 1000u + i;
    }
    const u64 cap = archive_segment_max_bytes(1u, ticks, 2u);
    u8* seg = (u8*)arena_push(&arena, cap, 16u);
    const u64 n = ar_encode(seg, cap, src, 1u, ticks, recs, 2u, 0u);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord out[2] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, seg, n);
    TL_EXPECT_EQ(archive_decode_segment(&r, &h, got, ticks, out, 1u, &rc), ERR_NET_CAPACITY);
    br_init(&r, seg, n);
    TL_EXPECT_EQ(archive_decode_segment(&r, &h, got, ticks - 1u, out, 2u, &rc), ERR_NET_CAPACITY);
    br_init(&r, seg, n);
    TL_EXPECT_EQ(archive_decode_segment(&r, &h, got, ticks, out, 2u, &rc), ERR_OK);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_decode_sets_the_derived_tick, "net,archive,fast") {
    // Round 2 finding B3: the archive left WireFrame.tick at 0 while decode_column derives it,
    // so an archived frame disagreed with the same frame off the wire. Every T2 row missed it
    // by comparing payloads only.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C20u, 4u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 5u;
    u8 seg[1024];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), ticks, &arena);
    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord none[1] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, seg, n);
    TL_ASSERT_EQ(archive_decode_segment(&r, &h, got, ticks, none, 1u, &rc), ERR_OK);
    for (u32 i = 0; i < ticks; ++i) { TL_EXPECT_EQ(got[i].tick, (u32)(h.base_tick + (u64)i)); }
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_untested_stream_and_record_rules, "net,archive,regression,fast") {
    // Round 3 finding F4: six load-bearing refusals had no test at all - reverting each left the
    // whole suite green. Each forgery below is refused today and re-opens a second spelling if
    // its check is removed. Grouped because they share one valid segment as a base.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C22u, 8u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 4u;
    u8 seg[2048];
    const u64 n = ar_valid_one_slot(seg, sizeof(seg), ticks, &arena);
    TL_ASSERT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_OK);

    // (1) An empty stream must never be ENCODED - omission is canonical, not optional. The base
    // here is all-zero frames, so its only streams are the two pointer axes (32, 33); splicing a
    // record_count-0 header for channel 0 in front keeps (slot, channel) ASCENDING, so the only
    // rule that can refuse it is the empty-stream refusal itself. (Spliced out of order it was
    // refused by the ascending-key check and the row proved nothing.)
    {
        WireFrame* z = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
        for (u32 i = 0; i < ticks; ++i) { z[i] = nt_zero_frame(0u); }
        u8 base[1024];
        const u64 bn = ar_encode(base, sizeof(base), z, 1u, ticks, nullptr, 0u, 0u);
        TL_ASSERT_EQ(ar_try_decode(base, bn, ticks, &arena), ERR_OK);
        // first stream's key: uvarint(record_count) then uvarint(key) - slot 0 channel 32 = 32
        TL_ASSERT_EQ(base[AR_T_HDR + 1u], (u8)archive_stream_key(0u, ARCHIVE_CH_POINTER_X));

        u8 forged[1024];
        for (u64 i = 0; i < AR_T_HDR; ++i) { forged[i] = base[i]; }
        u64 o = AR_T_HDR;
        // Two canonical uvarints, the post-option-A stream header. Spliced as the RETIRED
        // 8-byte struct these bytes parsed as several (record_count 0, key 0) headers and the
        // segment was refused for a different reason than this row names - round 4 finding F1.
        forged[o++] = 0u;                                     // uvarint record_count = 0
        forged[o++] = (u8)archive_stream_key(0u, 0u);         // uvarint key = slot 0, channel 0
        for (u64 i = AR_T_HDR; i < bn; ++i) { forged[o++] = base[i]; }
        const u32 pb = (u32)(o - AR_T_HDR);
        for (u32 i = 0; i < 4u; ++i) { forged[AR_T_PBYTES_OFF + i] = (u8)((pb >> (8u*i)) & 0xFFu); }
        ar_repair_crcs(forged, o);
        TL_EXPECT_EQ(ar_try_decode(forged, o, ticks, &arena), ERR_NET_MALFORMED);
    }

    // (1b) An escape record that RESTATES the flags the derived edges already produce carries no
    // information and is a second spelling. ar_valid_one_slot's escape says flags 1 where the
    // derived value at tick 0 is 3 (down + pressed); rewriting it to 3 makes it a no-op.
    {
        u8 forged[2048];
        for (u64 i = 0; i < n; ++i) { forged[i] = seg[i]; }
        const u8 word = forged[n - 1u];
        TL_ASSERT_EQ((u32)(word & 7u), 1u);
        forged[n - 1u] = (u8)((word & (u8)~7u) | 3u);   // flags 3 == the derived edges
        ar_repair_crcs(forged, n);
        TL_EXPECT_EQ(ar_try_decode(forged, n, ticks, &arena), ERR_NET_MALFORMED);
    }

    // (2) A LogRecord whose slot / origin_slot is outside MAX_PEERS.
    {
        WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
        for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }
        LogRecord rec = {};
        rec.format_version = NET_FORMAT_VERSION;
        rec.kind = (u8)LR_DELAY;
        rec.slot = 0u;
        rec.origin_slot = 0u;
        rec.seq = 0u;
        rec.effective_tick = 1000u;
        const u64 cap2 = archive_segment_max_bytes(1u, ticks, 1u);
        u8* s2 = (u8*)arena_push(&arena, cap2, 16u);
        const u64 n2 = ar_encode(s2, cap2, src, 1u, ticks, &rec, 1u, 0u);
        TL_ASSERT_EQ(ar_try_decode(s2, n2, ticks, &arena), ERR_OK);
        // slot is at offset 5 of the record; the array is the last 24 bytes.
        s2[n2 - 24u + 5u] = (u8)MAX_PEERS;         // one past the last legal slot
        ar_repair_crcs(s2, n2);
        TL_EXPECT_EQ(ar_try_decode(s2, n2, ticks, &arena), ERR_NET_MALFORMED);
        s2[n2 - 24u + 5u] = 0u;
        s2[n2 - 24u + 6u] = (u8)MAX_PEERS;         // origin_slot, offset 6
        ar_repair_crcs(s2, n2);
        TL_EXPECT_EQ(ar_try_decode(s2, n2, ticks, &arena), ERR_NET_MALFORMED);
    }

    // (3) An escape record that restates the flags the derived edges already produced carries
    // nothing, and (4) two escapes on one tick must ascend by action index.
    {
        // Two actions escaping on one tick gives a stream with two records to work with.
        WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * 2u, 16u);
        for (u32 i = 0; i < 2u; ++i) { src[i] = wire_zero_frame(); }
        nt_set_action(&src[0], 1u, (i8)0, 2u);     // pressed with no down bit - not derivable
        nt_set_action(&src[0], 3u, (i8)0, 2u);
        const u64 cap3 = archive_segment_max_bytes(1u, 2u, 0u);
        u8* s3 = (u8*)arena_push(&arena, cap3, 16u);
        const u64 n3 = ar_encode(s3, cap3, src, 1u, 2u, nullptr, 0u, 0u);
        TL_ASSERT_EQ(ar_try_decode(s3, n3, 2u, &arena), ERR_OK);
        // The escape stream's two records are the last four bytes: (0, w1) (0, w3).
        const u8 w_first = s3[n3 - 3u];
        const u8 w_second = s3[n3 - 1u];
        TL_ASSERT_LT(w_first >> 3, w_second >> 3);        // ascending by action today
        s3[n3 - 3u] = w_second;                            // swap -> descending within the tick
        s3[n3 - 1u] = w_first;
        ar_repair_crcs(s3, n3);
        TL_EXPECT_EQ(ar_try_decode(s3, n3, 2u, &arena), ERR_NET_MALFORMED);
    }
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_log_record_count_is_bounded_by_the_format, "net,archive,edge,fast") {
    // Round 3 finding F1: the duplicate scan is O(n^2) in log_record_count, which an untrusted
    // peer supplies (§20.2.5's BK_LOG_SEGMENT). 200,000 records was 4.6 MB on the wire and
    // 17.8 s to decode against 19 ms to encode. The bound is derived from tick_count, which the
    // caller already bounds, so there is no attacker-controlled dimension left.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C23u, 8u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 2u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }
    const u64 cap = archive_segment_max_bytes(1u, ticks, 1u);
    u8* seg = (u8*)arena_push(&arena, cap, 16u);
    LogRecord rec = {};
    rec.format_version = NET_FORMAT_VERSION;
    rec.kind = (u8)LR_DELAY;
    rec.effective_tick = 1000u;
    const u64 n = ar_encode(seg, cap, src, 1u, ticks, &rec, 1u, 0u);
    TL_ASSERT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_OK);

    // A DECLARED count is already bounded by payload_bytes, so editing the header alone tests
    // the wrong check. The segment must really carry the records. Build one with room for them
    // (tick_count 16 allows 8*16 = 128), then shrink tick_count so the same bytes exceed the
    // bound - all-zero frames keep every stream record at tick 0, so nothing else objects.
    const u32 wide = 16u;
    const u32 many = 100u;
    WireFrame* wz = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * wide, 16u);
    for (u32 i = 0; i < wide; ++i) { wz[i] = nt_zero_frame(0u); }
    LogRecord* recs = (LogRecord*)arena_push(&arena, sizeof(LogRecord) * many, 16u);
    for (u32 i = 0; i < many; ++i) {
        recs[i] = LogRecord{};
        recs[i].format_version = NET_FORMAT_VERSION;
        recs[i].kind = (u8)LR_DELAY;
        recs[i].slot = 0u;
        // All at ONE tick, one origin, ascending seq: unique ids, correctly ordered, and inside
        // the range for ANY tick_count >= 1. Spreading them over ticks made the effective_tick
        // RANGE check fire when tick_count shrank, so the row passed without testing the bound.
        recs[i].origin_slot = 0u;
        recs[i].seq = i;
        recs[i].effective_tick = 1000u;
    }
    const u64 wcap = archive_segment_max_bytes(1u, wide, many);
    u8* wseg = (u8*)arena_push(&arena, wcap, 16u);
    const u64 wn = ar_encode(wseg, wcap, wz, 1u, wide, recs, many, 0u);
    // Decoded directly: ar_try_decode's buffer holds 8 records, and 100 would come back as
    // ERR_NET_CAPACITY - a caller-side condition, not the format verdict this row is about.
    ArchiveSegmentHeader wh = {};
    WireFrame* wgot = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * wide * MAX_PEERS, 16u);
    LogRecord* wout = (LogRecord*)arena_push(&arena, sizeof(LogRecord) * many, 16u);
    u32 wrc = 0;
    ByteReader wr;
    br_init(&wr, wseg, wn);
    TL_ASSERT_EQ(archive_decode_segment(&wr, &wh, wgot, wide, wout, many, &wrc), ERR_OK);
    TL_ASSERT_EQ(wrc, many);

    // 100 records against a 12-tick segment exceeds 8 per tick; every record still falls inside
    // the shrunk range and the ids stay unique, so ONLY the format bound can refuse this.
    const u32 narrow = 12u;
    for (u32 i = 0; i < 4u; ++i) { wseg[AR_T_TICKS_OFF + i] = (u8)((narrow >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(wseg, wn);
    TL_EXPECT_GT((u64)many, (u64)MAX_LOG_RECORDS_PER_PACKET * (u64)narrow);
    br_init(&wr, wseg, wn);
    TL_EXPECT_EQ(archive_decode_segment(&wr, &wh, wgot, wide, wout, many, &wrc),
                 ERR_NET_MALFORMED);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_file_header_carries_the_identity_once, "net,archive,fast") {
    // The option-A ruling (2026-08-26) moved build_id and session_fingerprint out of every
    // segment and into one file header; a segment names its file with file_id. This row proves
    // the pair round-trips and that a segment's file_id is what ties it to that identity.
    u8 build_id[32], fingerprint[32];
    ar_ids(build_id, fingerprint);

    u8 buf[256];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    const u64 n = archive_write_file_header(&w, AR_FILE_ID, build_id, fingerprint);
    TL_ASSERT_EQ(n, (u64)sizeof(ArchiveFileHeader));
    TL_EXPECT_EQ(n, (u64)72u);

    ArchiveFileHeader h = {};
    ByteReader r;
    br_init(&r, buf, n);
    TL_ASSERT_EQ(archive_read_file_header(&r, &h), ERR_OK);
    TL_EXPECT_EQ(h.format_version, NET_FORMAT_VERSION);
    TL_EXPECT_EQ(h.file_id, AR_FILE_ID);
    for (u32 i = 0; i < 32u; ++i) {
        TL_EXPECT_EQ(h.build_id[i], build_id[i]);
        TL_EXPECT_EQ(h.session_fingerprint[i], fingerprint[i]);
    }
    TL_EXPECT_EQ(r.pos, r.len);

    // A newer format_version is refused here as everywhere else.
    const u32 newer = NET_FORMAT_VERSION + 1u;
    for (u32 i = 0; i < 4u; ++i) { buf[i] = (u8)((newer >> (8u * i)) & 0xFFu); }
    br_init(&r, buf, n);
    TL_EXPECT_EQ(archive_read_file_header(&r, &h), ERR_NET_VERSION);

    // The segment header no longer carries the identity at all - that is the whole saving.
    TL_EXPECT_EQ(sizeof(ArchiveSegmentHeader), (u64)56u);
    TL_EXPECT_LT(sizeof(ArchiveSegmentHeader), (u64)112u);
}

// --- rows for the refusals round 4 found untested ---------------------------------------------
// Each was verified live: deleting its rule left all 60 net rows green, including the 10^6-case
// fuzz. F3 and F4 are real second spellings; F5 is the only guard against key aliasing.

TL_TEST(archive_regression_action_records_must_advance_a_tick, "net,archive,regression,fast") {
    // Round 4 finding F3. Two action records both at delta_tick 0: the first closes an empty run
    // and contributes nothing, so the segment decodes to exactly what a one-record segment
    // produces - two byte spellings of one frame set, in a format whose bytes are hashed into
    // ChainEntry.log_segment_hash (§20.2.8).
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C24u, 4u << 20, 0u, &api), ERR_OK);

    u8 seg[512];
    for (u32 i = 0; i < sizeof(seg); ++i) { seg[i] = 0u; }
    u32 o = 0;
    auto put32 = [&](u32 v) { for (u32 i = 0; i < 4u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 4u; };
    auto put64 = [&](u64 v) { for (u32 i = 0; i < 8u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 8u; };
    auto putvar = [&](u32 v) { while (v >= 0x80u) { seg[o++] = (u8)(v | 0x80u); v >>= 7; } seg[o++] = (u8)v; };
    const u32 ticks = 2u;
    put32(NET_FORMAT_VERSION); put32(NET_FRAME_MAX_ACTIONS); put64(1000u); put32(ticks);
    seg[20] = 0x01u; o = 24u;
    put32(4u);                       // record_count: 2 action + 1 ptr_x + 1 ptr_y
    put32(0u); put32(AR_FILE_ID); put32(0u); put32(0u); put32(0u); put32(0u);
    o = AR_T_HDR;
    const u32 payload_start = o;
    putvar(2u); putvar(archive_stream_key(0u, 0u));   // action 0, TWO records
    seg[o++] = 0x00u; seg[o++] = 0x03u;               // delta 0, word 3
    seg[o++] = 0x00u; seg[o++] = 0x05u;               // delta 0 AGAIN - the forgery
    putvar(1u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_X));
    seg[o++] = 0x00u; seg[o++] = 0x00u;
    putvar(1u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_Y));
    seg[o++] = 0x00u; seg[o++] = 0x00u;
    const u32 pb = o - payload_start;
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_PBYTES_OFF + i] = (u8)((pb >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(seg, o);
    TL_EXPECT_EQ(ar_try_decode(seg, o, ticks, &arena), ERR_NET_MALFORMED);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_pointer_stream_may_not_over_declare, "net,archive,regression,fast") {
    // Round 4 finding F4. A pointer stream declaring a record the tick walk can never REACH: the
    // surplus is never consumed, so the segment decodes to what a correctly-declared segment
    // produces - a second spelling. `record_count == seen_records` cannot see it (seen_records
    // sums the DECLARED counts) and `record_count > tick_count` cannot either.
    //
    // Built by hand: bumping a declared count on an encoded segment is NOT enough, because the
    // decoder then simply reads one more record out of the following stream's bytes and the
    // counts still agree at the end. The record has to be unreachable, which needs the velocity
    // records to run out before the tick walk does.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C25u, 4u << 20, 0u, &api), ERR_OK);

    u8 seg[512];
    for (u32 i = 0; i < sizeof(seg); ++i) { seg[i] = 0u; }
    u32 o = 0;
    auto put32 = [&](u32 v) { for (u32 i = 0; i < 4u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 4u; };
    auto put64 = [&](u64 v) { for (u32 i = 0; i < 8u; ++i) { seg[o + i] = (u8)((v >> (8u*i)) & 0xFFu); } o += 8u; };
    auto putvar = [&](u32 v) { while (v >= 0x80u) { seg[o++] = (u8)(v | 0x80u); v >>= 7; } seg[o++] = (u8)v; };
    const u32 ticks = 3u;
    put32(NET_FORMAT_VERSION); put32(NET_FRAME_MAX_ACTIONS); put64(1000u); put32(ticks);
    seg[20] = 0x01u; o = 24u;
    put32(4u);                       // record_count = 3 (declared by ptr_x) + 1 (ptr_y)
    put32(0u); put32(AR_FILE_ID); put32(0u); put32(0u); put32(0u); put32(0u);
    o = AR_T_HDR;
    const u32 payload_start = o;
    // pointer_x DECLARES 3 but carries 2: absolute at tick 0, one velocity at tick 2. The tick
    // walk finishes at tick 2, so a third record could never be read.
    putvar(3u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_X));
    seg[o++] = 0x00u; seg[o++] = 0x00u;               // delta 0, svarint(0) - absolute
    seg[o++] = 0x02u; seg[o++] = 0x02u;               // delta 2, svarint(1) - velocity at tick 2
    putvar(1u); putvar(archive_stream_key(0u, ARCHIVE_CH_POINTER_Y));
    seg[o++] = 0x00u; seg[o++] = 0x00u;
    const u32 pb = o - payload_start;
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_PBYTES_OFF + i] = (u8)((pb >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(seg, o);
    TL_EXPECT_EQ(ar_try_decode(seg, o, ticks, &arena), ERR_NET_MALFORMED);

    // Declared correctly, the same bytes decode: the over-declaration was the only objection.
    seg[AR_T_HDR] = 2u;                                        // ptr_x declares 2
    for (u32 i = 0; i < 4u; ++i) { seg[AR_T_RECS_OFF + i] = (u8)((3u >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(seg, o);
    TL_EXPECT_EQ(ar_try_decode(seg, o, ticks, &arena), ERR_OK);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_stream_key_bound_stops_aliasing, "net,archive,regression,fast") {
    // Round 4 finding F5. slot and channel are both narrowed to u8 by the key split, so without
    // the bound `key` and `key + 256*35` name the same stream: two spellings of one segment.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C26u, 4u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 2u;
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }
    u8 seg[512];
    const u64 n = ar_encode(seg, sizeof(seg), src, 1u, ticks, nullptr, 0u, 0u);
    TL_ASSERT_EQ(ar_try_decode(seg, n, ticks, &arena), ERR_OK);

    // Rewrite pointer_x's key as key + 256*35, which truncates to the same (slot, channel).
    const u32 aliased = archive_stream_key(0u, ARCHIVE_CH_POINTER_X) + 256u * ARCHIVE_CH_REAL_COUNT;
    u8 forged[512];
    for (u64 i = 0; i < AR_T_HDR + 1u; ++i) { forged[i] = seg[i]; }
    u64 o = AR_T_HDR + 1u;
    u32 v = aliased;
    while (v >= 0x80u) { forged[o++] = (u8)(v | 0x80u); v >>= 7; }
    forged[o++] = (u8)v;
    for (u64 i = AR_T_HDR + 2u; i < n; ++i) { forged[o++] = seg[i]; }
    const u32 pb = (u32)(o - AR_T_HDR);
    for (u32 i = 0; i < 4u; ++i) { forged[AR_T_PBYTES_OFF + i] = (u8)((pb >> (8u*i)) & 0xFFu); }
    ar_repair_crcs(forged, o);
    TL_EXPECT_EQ(ar_try_decode(forged, o, ticks, &arena), ERR_NET_MALFORMED);
    // The bound itself, stated: the last legal key is slot 7's escape channel.
    TL_EXPECT_EQ(archive_stream_key((u8)(MAX_PEERS - 1u), ARCHIVE_CH_MAX),
                 MAX_PEERS * ARCHIVE_CH_REAL_COUNT - 1u);
    api.release(api.ctx, arena.base, arena.reserved);
}

TL_TEST(archive_regression_effective_tick_range_survives_a_u64_wrap, "net,archive,regression,edge,fast") {
    // Round 4 finding F7a. The range test is spelled as a subtraction because
    // base_tick + tick_count wraps u64 near the top of the range - and the additive form would
    // then refuse every record in a legitimate segment. No fuzz corpus reaches this (~2^-60 per
    // draw), so it needs a row that goes there deliberately.
    VMemApi api = test_vmem_api();
    VMemArena arena = {};
    TL_ASSERT_EQ(vmem_arena_init(&arena, 0xA5C27u, 4u << 20, 0u, &api), ERR_OK);
    const u32 ticks = 4u;
    const u64 base = 0xFFFFFFFFFFFFFFFCull;   // base + ticks wraps to 0
    WireFrame* src = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks, 16u);
    for (u32 i = 0; i < ticks; ++i) { src[i] = nt_zero_frame(0u); }
    LogRecord rec = {};
    rec.format_version = NET_FORMAT_VERSION;
    rec.kind = (u8)LR_DELAY;
    rec.effective_tick = base;                // the first tick of the segment

    ArchiveInput in[1];
    in[0].frames = src; in[0].slot = 0u; in[0]._pad0 = 0u;
    const u64 cap = archive_segment_max_bytes(1u, ticks, 1u);
    u8* seg = (u8*)arena_push(&arena, cap, 16u);
    ByteWriter w;
    bw_init(&w, seg, cap);
    const u64 n = archive_encode_segment(&w, base, ticks, in, 1u, &rec, 1u, 0u, AR_FILE_ID);
    TL_ASSERT_GT(n, (u64)0);

    ArchiveSegmentHeader h = {};
    WireFrame* got = (WireFrame*)arena_push(&arena, sizeof(WireFrame) * ticks * MAX_PEERS, 16u);
    LogRecord out[2] = {};
    u32 rc = 0;
    ByteReader r;
    br_init(&r, seg, n);
    TL_EXPECT_EQ(archive_decode_segment(&r, &h, got, ticks, out, 2u, &rc), ERR_OK);
    TL_EXPECT_EQ(rc, 1u);
    TL_EXPECT_EQ(out[0].effective_tick, base);
    TL_EXPECT_EQ(h.base_tick, base);
    api.release(api.ctx, arena.base, arena.reserved);
}
