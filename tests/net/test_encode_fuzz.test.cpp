// T1f (docs/NETCODE.md §20.6): seeded random frame sequences encode -> decode to equality, and
// random byte mutation never crashes - every mutated stream is either refused or decodes to
// something that re-encodes IDENTICALLY.
//
// That second property is only true of a canonical format, which is why the codec refuses
// non-minimal varints and redundant value bytes (wire.h's canonical-form note): the archive's
// bytes are hashed into ChainEntry.log_segment_hash (docs/NETCODE.md §20.2.8), so two encodings
// of one frame set would fork the chain with no divergence behind it. This file is the test that
// would fail first if either refusal were removed.
//
// The `fast` rows run a few thousand cases so the PR lane exercises them every push; the
// slow-tagged rows run the spec's 10^6 and are what the phase gate's 10-minute ASan/UBSan run
// covers (docs/TESTING.md §9.3 excludes `slow` from the fast lane).
#include "runner/tl_test.h"
#include "net/net_test_util.h"

enum { EF_BUF = 4096, EF_MAX_FRAMES = 9 };

// A frame built entirely from the keyed generator: every action's value and flags, and both
// pointer axes. flags is masked to the three docs/INPUT.md §1 bits - a frame with any other bit
// set is not representable and the encoder asserts on it, so generating one would be testing
// the fixture, not the codec.
static WireFrame ef_random_frame(u64 seed, u32 index) {
    WireFrame f = wire_zero_frame();
    for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
        const u64 h = nt_mix64(seed, ((u64)index << 8) | a);
        // Bias towards "unchanged" so columns look like real input: mostly still, occasionally
        // active. A uniformly random frame would make every `changed` bit set on every frame and
        // never exercise the delta path the format exists for.
        if ((h & 0xFu) < 3u) {
            f.actions[a].value = (i8)((h >> 8) & 0xFFu);
            f.actions[a].flags = (u8)((h >> 16) & (u64)WIRE_FLAG_BITS);
        }
    }
    const u64 p = nt_mix64(seed, 0xB01DFACEull + index);
    f.pointer_x = (i32)(u32)p;
    f.pointer_y = (i32)(u32)(p >> 32);
    return f;
}

// Encodes n frames and returns the byte count.
static u64 ef_encode(const WireFrame* frames, u32 n, u8* buf, u64 cap) {
    ByteWriter w;
    bw_init(&w, buf, cap);
    encode_column(&w, frames, n);
    return w.len;
}

// The body of the round-trip property, shared by the fast and slow rows.
static void ef_round_trip_cases(TestCtx* t, u64 seed_base, u32 cases) {
    for (u32 c = 0; c < cases; ++c) {
        const u64 seed = nt_mix64(seed_base, c);
        const u32 n = 1u + (u32)(seed % EF_MAX_FRAMES);
        WireFrame src[EF_MAX_FRAMES];
        for (u32 i = 0; i < n; ++i) { src[i] = ef_random_frame(seed, i); }

        u8 buf[EF_BUF];
        const u64 len = ef_encode(src, n, buf, sizeof(buf));

        WireFrame got[EF_MAX_FRAMES];
        ByteReader r;
        br_init(&r, buf, len);
        TL_ASSERT_EQ(decode_column(&r, got, n, 0u), ERR_OK);
        TL_ASSERT_EQ(r.pos, r.len);
        for (u32 i = 0; i < n; ++i) { TL_ASSERT_TRUE(nt_frames_equal_payload(&src[i], &got[i])); }

        // Canonical: re-encoding the decoded frames reproduces the bytes exactly.
        u8 again[EF_BUF];
        const u64 len2 = ef_encode(got, n, again, sizeof(again));
        TL_ASSERT_EQ(len2, len);
        TL_ASSERT_EQ(memcmp(buf, again, (usize)len), 0);
    }
}

// The body of the mutation property, shared by the fast and slow rows.
static void ef_mutation_cases(TestCtx* t, u64 seed_base, u32 cases) {
    u32 accepted = 0;
    u32 refused = 0;
    for (u32 c = 0; c < cases; ++c) {
        const u64 seed = nt_mix64(seed_base, c);
        const u32 n = 1u + (u32)(seed % EF_MAX_FRAMES);
        WireFrame src[EF_MAX_FRAMES];
        for (u32 i = 0; i < n; ++i) { src[i] = ef_random_frame(seed, i); }

        u8 buf[EF_BUF];
        const u64 len = ef_encode(src, n, buf, sizeof(buf));
        if (len == 0u) { continue; }

        // One byte, one bit pattern - a mutation the encoder could never have produced.
        const u64 m = nt_mix64(seed, 0x3117ull);
        const u64 pos = m % len;
        const u8 old = buf[pos];
        buf[pos] = (u8)(old ^ (u8)(1u << ((m >> 32) & 7u)));

        WireFrame got[EF_MAX_FRAMES];
        ByteReader r;
        br_init(&r, buf, len);
        const ErrCode e = decode_column(&r, got, n, 0u);
        if (e != ERR_OK) {
            ++refused;
            continue;   // refusing a corrupted stream is the correct outcome
        }
        ++accepted;
        // Accepted: then the frames it produced must re-encode to exactly these bytes. A stream
        // that decodes to frames whose canonical encoding differs is the failure this row
        // exists for - it would mean one frame set has two encodings.
        u8 again[EF_BUF];
        const u64 len2 = ef_encode(got, n, again, sizeof(again));
        TL_ASSERT_EQ(len2, r.pos);   // the re-encoding covers exactly what was consumed
        TL_ASSERT_EQ(memcmp(buf, again, (usize)r.pos), 0);
    }
    // Both outcomes must actually occur, or the row is proving nothing: all-refused would mean
    // the mutations never landed anywhere decodable, all-accepted that nothing was checked.
    TL_EXPECT_GT(accepted, 0u);
    TL_EXPECT_GT(refused, 0u);
}

// The seed is the harness's (docs/TESTING.md §1: tl_seed_for(--seed, row index)), mixed with a
// per-row constant. A bare run is reproducible; `--seed N` walks a different corpus, which is
// what makes the phase gate's 10-minute soak cover new ground on every pass instead of redoing
// one fixed corpus. A failure is reproduced by re-running with the same --seed.
TL_TEST(encode_fuzz_random_frames_round_trip, "net,encode,fuzz,fast") {
    ef_round_trip_cases(t, 0xC0DEC0DEull ^ (u64)t->seed, 4000u);
}

TL_TEST(encode_fuzz_mutated_bytes_are_refused_or_canonical, "net,encode,fuzz,fast") {
    ef_mutation_cases(t, 0xFA11EDull ^ (u64)t->seed, 4000u);
}

TL_TEST(encode_fuzz_random_frames_round_trip_million, "net,encode,fuzz,slow") {
    // docs/NETCODE.md §20.6 T1f: 10^6 seeded sequences, run under ASan/UBSan for the phase gate.
    ef_round_trip_cases(t, 0xC0DEC0DEull ^ (u64)t->seed, 1000000u);
}

TL_TEST(encode_fuzz_mutated_bytes_are_refused_or_canonical_million, "net,encode,fuzz,slow") {
    ef_mutation_cases(t, 0xFA11EDull ^ (u64)t->seed, 1000000u);
}
