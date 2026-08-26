// replay.test.cpp - record -> replay -> identical frames and hash trace; fingerprint mismatch
// refused (docs/DETERMINISM.md §9.2, docs/INPUT.md §7/§9.4).
#include "runner/tl_test.h"
#include "core/producers/replay.h"
#include "core/producers/script.h"
#include "core/world_test_util.h"

namespace {

u8 g_build_id[32] = { 1, 2, 3, 4 };
u8 g_fingerprint[32] = { 5, 6, 7, 8 };

}  // namespace

TL_TEST(recorder_write_read_roundtrip, "core,input,replay,recorder,fast") {
    WorldFixture* wf = wt_fixture(0);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1234u));
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.rec"_id, 1024u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 16u, 0u, 1234u, 1u, 0b1u, g_build_id, g_fingerprint);

    InputFrame frames[MAX_PEERS];
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
        frames[0].tick = i;
        frames[0].actions[0].value = (i8)(i + 1u);
        frames[0].actions[0].flags = AS_DOWN;
        recorder_tick(&rec, &wf->w, frames);
        wf->w.state->tick += 1u;
    }
    TL_ASSERT_EQ(rec.rows.count, 3u);

    VMemArena buf_arena;
    TL_ASSERT_EQ(vmem_arena_init(&buf_arena, "replay.test.buf"_id, 1024u * 1024u, 0u, &wf->api), ERR_OK);
    const u64 needed = recorder_bytes_needed(&rec);
    u8* buf = (u8*)arena_push(&buf_arena, needed, 8u);
    ByteWriter w;
    bw_init(&w, buf, needed);
    const u64 written = recorder_write(&rec, &w);
    TL_ASSERT_EQ(written, needed);

    ByteReader r;
    br_init(&r, buf, written);
    RecordedInputHeader header{};
    TL_ASSERT_EQ(recorder_read_header(&r, &header, g_fingerprint), ERR_OK);
    TL_EXPECT_EQ(header.frame_count, 3u);
    TL_EXPECT_EQ(header.peer_count, 1u);
    TL_EXPECT_EQ(header.live_mask, (u8)0b1u);

    RecordedInputRow rows[3];
    TL_ASSERT_EQ(recorder_read_body(&r, &header, rows), ERR_OK);
    for (u32 i = 0; i < 3u; ++i) {
        TL_EXPECT_EQ(rows[i].frames[0].tick, i);
        TL_EXPECT_EQ(rows[i].frames[0].actions[0].value, (i8)(i + 1u));
        TL_EXPECT_EQ(rows[i].world_hash, rec.rows.data[i].world_hash);
    }
}

TL_TEST(recorder_read_refuses_fingerprint_mismatch, "core,input,replay,recorder,fast") {
    WorldFixture* wf = wt_fixture(1);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1u));
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.rec2"_id, 256u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 4u, 0u, 1u, 1u, 0b1u, g_build_id, g_fingerprint);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    recorder_tick(&rec, &wf->w, frames);

    VMemArena buf_arena;
    TL_ASSERT_EQ(vmem_arena_init(&buf_arena, "replay.test.buf2"_id, 256u * 1024u, 0u, &wf->api), ERR_OK);
    const u64 needed = recorder_bytes_needed(&rec);
    u8* buf = (u8*)arena_push(&buf_arena, needed, 8u);
    ByteWriter w;
    bw_init(&w, buf, needed);
    recorder_write(&rec, &w);

    ByteReader r;
    br_init(&r, buf, w.len);
    RecordedInputHeader header{};
    u8 wrong_fingerprint[32] = { 9, 9, 9 };
    TL_EXPECT_EQ(recorder_read_header(&r, &header, wrong_fingerprint), ERR_RECORDER_FINGERPRINT);
}

TL_TEST(replay_producer_serves_rows_then_waits, "core,input,replay,fast") {
    RecordedInputHeader header{};
    header.format_version = RECORDED_INPUT_FORMAT_VERSION;
    header.base_tick = 10u;
    header.frame_count = 2u;
    header.peer_count = 1u;
    header.live_mask = 0b1u;

    RecordedInputRow rows[2];
    for (u32 i = 0; i < 2u; ++i) {
        rows[i] = RecordedInputRow{};
        rows[i].frames[0] = input_zero_frame();
        rows[i].frames[0].tick = 10u + i;
        rows[i].frames[0].actions[0].value = (i8)(i + 1u);
        rows[i].world_hash = 0x1000u + i;
    }

    ReplayProducer rp;
    replay_producer_init(&rp, &header, rows);

    InputFrame out[MAX_PEERS];
    u8 live_mask = 0u;
    TL_ASSERT_EQ(replay_produce(&rp, 10u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[0].value, (i8)1);
    TL_EXPECT_EQ(replay_last_hash(&rp), 0x1000u);
    TL_EXPECT_FALSE(replay_exhausted(&rp));

    TL_ASSERT_EQ(replay_produce(&rp, 11u, out, &live_mask), PRODUCE_READY);
    TL_EXPECT_EQ(out[0].actions[0].value, (i8)2);
    TL_EXPECT_EQ(replay_last_hash(&rp), 0x1001u);
    TL_EXPECT_TRUE(replay_exhausted(&rp));

    TL_EXPECT_EQ(replay_produce(&rp, 12u, out, &live_mask), PRODUCE_WAIT);
}
