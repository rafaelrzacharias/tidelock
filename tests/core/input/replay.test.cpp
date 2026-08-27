// replay.test.cpp - record -> replay -> identical frames and hash trace; fingerprint mismatch
// refused (docs/DETERMINISM.md §9.2, docs/INPUT.md §7/§9.4).
#include "runner/tl_test.h"
#include "core/loop.h"
#include "core/producers/replay.h"
#include "core/producers/script.h"
#include "core/world_test_util.h"
#include <string.h>

namespace {

u8 g_build_id[32] = { 1, 2, 3, 4 };
u8 g_fingerprint[32] = { 5, 6, 7, 8 };

// The lane's central done criterion (docs/INPUT.md §7, §9.6; docs/FRAME-LOOP.md §8.4): record ->
// replay -> identical frames and identical hash trace. Folds w->input[0]'s raw action 0 value
// into every WPos entity's x every tick, so the recorder's world_hash actually moves with input
// (a hash trace that never depended on input would pass this test for the wrong reason).
void sys_fold_input_into_wpos(World* w) {
    Span<WPos> col = world_column<WPos>(w);
    for (u32 i = 0; i < col.count; ++i) { col.data[i].x += w->input[0].actions[0].value; }
}

// One Engine + its own recorder, wired identically to the other side (same seed, same
// registration order, same fold system) - the two halves of the record/replay comparison.
struct ReplayEngineHalf {
    VMemApi api;
    ArenaRegistry reg;
    Scratch scratch;
    VMemArena rec_arena;
    Engine e;
    Recorder rec;
};

bool replay_engine_half_init(ReplayEngineHalf* h, u64 seed, NameHash scratch_id, NameHash rec_arena_id) {
    h->api = test_vmem_api();
    memset(&h->reg, 0, sizeof(h->reg));
    if (scratch_init(&h->scratch, scratch_id, 32u * 1024u * 1024u, &h->api) != ERR_OK) { return false; }
    WorldDesc d{};
    d.seed = seed;
    if (engine_init(&h->e, &h->reg, &h->scratch, &h->api, nullptr, &d) != ERR_OK) { return false; }
    world_register_component(&h->e.world, &WPos_info);
    SystemDesc sd{};
    sd.fn = sys_fold_input_into_wpos;
    sd.label = "fold_input"_id;
    sd.phase = PHASE_UPDATE;
    sd.reads = Span<const ComponentId>{ nullptr, 0 };
    sd.writes = Span<const ComponentId>{ nullptr, 0 };
    sd.before = Span<const NameHash>{ nullptr, 0 };
    sd.after = Span<const NameHash>{ nullptr, 0 };
    sd.flags = 0;
    world_register_system(&h->e.world, &sd);
    world_build_schedule(&h->e.world);
    registry_seal(&h->reg);   // registry_hash_all (recorder_tick's) requires it (arena_registry.cpp)
    Entity ent = world_spawn(&h->e.world);
    world_add(&h->e.world, ent, WPos{ 0, 0 });
    world_flush(&h->e.world);
    if (vmem_arena_init(&h->rec_arena, rec_arena_id, 1024u * 1024u, 0u, &h->api) != ERR_OK) { return false; }
    recorder_init(&h->rec, &h->rec_arena, 16u, 0u, seed, 1u, 0b1u, g_build_id, g_fingerprint);
    recorder_attach(&h->e, &h->rec);
    return true;
}

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

TL_TEST(recorder_tick_zeroes_frames_past_peer_count, "core,input,replay,recorder,fast") {
    // Review round 1 finding 10: the caller's Engine::frames buffer persists across ticks and a
    // producer only ever writes its own live slots (docs/INPUT.md §4) - slots >= peer_count can
    // carry an earlier tick's non-live garbage. recorder_tick must not memcpy that garbage into
    // the row it stores, since only [0, peer_count) round-trips through recorder_write/read.
    WorldFixture* wf = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1u));
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.zero.rec"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 4u, 0u, 1u, 1u, 0b1u, g_build_id, g_fingerprint);   // peer_count = 1

    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    // Garbage in a non-live slot (>= peer_count) - as if left over from a producer that once
    // populated it on an earlier, different-peer_count tick.
    frames[1].tick = 999u;
    frames[1].actions[0].value = (i8)-77;
    frames[1].pointer_x = 12345;

    recorder_tick(&rec, &wf->w, frames);

    InputFrame zero = input_zero_frame();
    TL_EXPECT_EQ(memcmp(&rec.rows.data[0].frames[1], &zero, sizeof(InputFrame)), 0);
}

TL_TEST(recorder_tick_zeroes_non_live_frames_within_peer_count, "core,input,replay,recorder,fast") {
    // Review round 2 defect 10: the same staleness round 1 fixed for slots >= peer_count also
    // reaches WITHIN [0, peer_count) - a producer writes only its own live_mask slots (live.cpp,
    // replay.cpp), so a peer_count slot outside live_mask still carries whatever an earlier tick
    // left in Engine::frames. Inert while every consumer honours live_mask, but a full row
    // byte-for-byte comparison (as INPUT.md §9.6's row requires) would see it.
    WorldFixture* wf = wt_fixture(3);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1u));
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.zero2.rec"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 4u, 0u, 1u, 2u, 0b01u, g_build_id, g_fingerprint);   // peer_count = 2, only slot 0 live

    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    // Garbage in slot 1 - within peer_count, but NOT in live_mask (0b01 = only slot 0).
    frames[1].tick = 999u;
    frames[1].actions[0].value = (i8)-77;
    frames[1].pointer_x = 12345;

    recorder_tick(&rec, &wf->w, frames);

    InputFrame zero = input_zero_frame();
    TL_EXPECT_EQ(memcmp(&rec.rows.data[0].frames[1], &zero, sizeof(InputFrame)), 0);
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

TL_TEST(recorder_pointer_round_trips_negative_and_positive_through_the_le_encoding, "core,input,replay,recorder,fast") {
    // Review round 1 finding 12: every recorded frame in every other shipped test has pointer
    // (0, 0) - put_input_frame/get_input_frame's (u32)/(i32) reinterpret round trip for
    // pointer_x/pointer_y was never exercised with a nonzero, negative value.
    WorldFixture* wf = wt_fixture(0);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1u));
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.ptr.rec"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 4u, 0u, 1u, 1u, 0b1u, g_build_id, g_fingerprint);

    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    frames[0].pointer_x = -12345;
    frames[0].pointer_y = 67890;
    recorder_tick(&rec, &wf->w, frames);

    VMemArena buf_arena;
    TL_ASSERT_EQ(vmem_arena_init(&buf_arena, "replay.test.ptr.buf"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    const u64 needed = recorder_bytes_needed(&rec);
    u8* buf = (u8*)arena_push(&buf_arena, needed, 8u);
    ByteWriter w;
    bw_init(&w, buf, needed);
    TL_ASSERT_EQ(recorder_write(&rec, &w), needed);

    ByteReader r;
    br_init(&r, buf, w.len);
    RecordedInputHeader header{};
    TL_ASSERT_EQ(recorder_read_header(&r, &header, g_fingerprint), ERR_OK);
    RecordedInputRow row{};
    TL_ASSERT_EQ(recorder_read_body(&r, &header, &row), ERR_OK);
    TL_EXPECT_EQ(row.frames[0].pointer_x, -12345);
    TL_EXPECT_EQ(row.frames[0].pointer_y, 67890);
}

TL_TEST(recorder_read_header_refuses_frame_count_past_buffer_end, "core,input,replay,recorder,fast") {
    // Review round 1 finding 7: frame_count is fully corruption/attacker-controlled. A header
    // claiming far more rows than the (short, truncated-looking) buffer could possibly hold must
    // be refused here, before a caller sizes out_rows or recorder_read_body walks off the end of
    // one it already sized too small.
    WorldFixture* wf = wt_fixture(2);
    TL_ASSERT_TRUE(world_fixture_init(wf, 1u));

    VMemArena rec_arena;
    TL_ASSERT_EQ(vmem_arena_init(&rec_arena, "replay.test.badcount.rec"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    Recorder rec;
    recorder_init(&rec, &rec_arena, 4u, 0u, 1u, 1u, 0b1u, g_build_id, g_fingerprint);
    InputFrame frames[MAX_PEERS];
    for (u32 p = 0; p < MAX_PEERS; ++p) { frames[p] = input_zero_frame(); }
    world_fixture_register_std(wf);
    world_build_schedule(&wf->w);
    registry_seal(&wf->reg);
    recorder_tick(&rec, &wf->w, frames);   // one real row

    VMemArena buf_arena;
    TL_ASSERT_EQ(vmem_arena_init(&buf_arena, "replay.test.badcount.buf"_id, 64u * 1024u, 0u, &wf->api), ERR_OK);
    const u64 needed = recorder_bytes_needed(&rec);
    u8* buf = (u8*)arena_push(&buf_arena, needed, 8u);
    ByteWriter w;
    bw_init(&w, buf, needed);
    recorder_write(&rec, &w);

    // Corrupt the on-wire frame_count (offset 96, u64 LE - RecordedInputHeader's own field table)
    // to claim far more rows than this short buffer could hold, without changing its length.
    u64 huge_count = 0xFFFFFFFFFFFFu;
    memcpy(buf + 96, &huge_count, 8u);

    ByteReader r;
    br_init(&r, buf, w.len);
    RecordedInputHeader header{};
    TL_EXPECT_EQ(recorder_read_header(&r, &header, g_fingerprint), ERR_BYTES_TRUNCATED);
}

TL_TEST(recorder_read_header_refuses_peer_count_over_max, "core,input,replay,recorder,fast") {
    // Review round 1 finding 7: an unchecked peer_count > MAX_PEERS would overflow
    // RecordedInputRow::frames[MAX_PEERS] in recorder_read_body's per-peer loop.
    u8 buf[128 + 4];
    memset(buf, 0, sizeof(buf));
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    RecordedInputHeader h{};
    h.format_version = RECORDED_INPUT_FORMAT_VERSION;
    memcpy(h.magic, RECORDED_INPUT_MAGIC, 4u);
    memcpy(h.build_id, g_build_id, 32u);
    memcpy(h.session_fingerprint, g_fingerprint, 32u);
    h.seed = 1u;
    h.base_tick = 0u;
    h.peer_count = (u8)(MAX_PEERS + 1u);
    h.live_mask = 0b1u;
    h.flags = 0u;
    h.frame_count = 0u;
    wire_write_RecordedInputHeader(&w, &h);
    bw_put_u32(&w, 0u);   // crc32 trailer (frame_count == 0, so this is the whole body)

    ByteReader r;
    br_init(&r, buf, w.len);
    RecordedInputHeader header{};
    TL_EXPECT_EQ(recorder_read_header(&r, &header, g_fingerprint), ERR_RECORDER_PEER_COUNT);
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

TL_TEST(record_replay_reproduces_identical_hash_trace, "core,input,replay,determinism,fast") {
    // Pass A: 12 ticks off the Script producer, recorded.
    ReplayEngineHalf a;
    TL_ASSERT_TRUE(replay_engine_half_init(&a, 1234u, "replay.test.det.a.scratch"_id, "replay.test.det.a.rec"_id));
    VMemArena sp_arena;
    TL_ASSERT_EQ(vmem_arena_init(&sp_arena, "replay.test.det.sp"_id, 64u * 1024u, 0u, &a.api), ERR_OK);
    ScriptProducer sp;
    script_producer_init(&sp, &sp_arena, 8u, 0b1u);
    script_hold(&sp, (ActionId)0u, 3, 2u, 8u, 0u);   // down (value 3) for ticks [2, 8)

    u8 live_mask = 0u;
    for (u32 i = 0; i < 12u; ++i) {
        script_produce(&sp, a.e.world.state->tick, a.e.frames, &live_mask);
        engine_tick_once(&a.e, a.e.frames);
    }
    TL_ASSERT_EQ(a.rec.rows.count, 12u);
    // review round 2 defect 1: this NE check passes even with the fold system reduced to a no-op,
    // because WorldTickState{tick,seed} lives in an ARENA_HASHED arena and row i's hash is already
    // a function of tick == i alone - it does NOT prove input reached the sim. Replaced below with
    // an exact-value pin on the folded state itself, which does. Kept as a documented non-guard,
    // not deleted, so a future reader doesn't reintroduce it believing it discriminates.
    TL_EXPECT_NE(a.rec.rows.data[0].world_hash, a.rec.rows.data[11].world_hash);

    // The actual proof that input reached the sim: script_hold(value 3, ticks [2,8)) holds the
    // action down for ticks 2,3,4,5,6,7 (HOLD_END at tick 8 clears it before that tick's frame is
    // emitted - script.cpp's event loop applies every event with tick <= the current tick, in
    // order, so HOLD_START then HOLD_END both fire on or before tick 8). That's 6 ticks * value 3
    // folded into WPos.x by sys_fold_input_into_wpos every PHASE_UPDATE = 18. Severing
    // `w->input = frames` (loop.cpp) or reducing the fold system to `x += 0` both leave WPos.x at
    // its spawn value of 0, so this fails under either mutation where the NE guard above does not.
    Span<WPos> a_pos = world_column<WPos>(&a.e.world);
    TL_ASSERT_EQ(a_pos.count, 1u);
    TL_EXPECT_EQ(a_pos.data[0].x, 18);

    // Pass B: a fresh Engine, same seed, driven by Replay over pass A's rows - routed through
    // recorder_write -> bytes -> recorder_read_body first, so the wire serialisation this lane
    // ships (docs/DETERMINISM.md §9.2) is actually inside the record -> replay loop this test
    // claims to cover (INPUT.md §9.6's row: "record -> replay -> frames identical, hashes
    // identical"), not bypassed by handing ReplayProducer pass A's in-memory rows directly.
    VMemArena buf_arena;
    TL_ASSERT_EQ(vmem_arena_init(&buf_arena, "replay.test.det.buf"_id, 1024u * 1024u, 0u, &a.api), ERR_OK);
    const u64 needed = recorder_bytes_needed(&a.rec);
    u8* buf = (u8*)arena_push(&buf_arena, needed, 8u);
    ByteWriter bw;
    bw_init(&bw, buf, needed);
    TL_ASSERT_EQ(recorder_write(&a.rec, &bw), needed);

    ByteReader br;
    br_init(&br, buf, needed);
    RecordedInputHeader header{};
    TL_ASSERT_EQ(recorder_read_header(&br, &header, g_fingerprint), ERR_OK);
    TL_ASSERT_EQ(header.frame_count, a.rec.rows.count);
    RecordedInputRow decoded_rows[12];
    TL_ASSERT_EQ(recorder_read_body(&br, &header, decoded_rows), ERR_OK);

    ReplayEngineHalf b;
    TL_ASSERT_TRUE(replay_engine_half_init(&b, 1234u, "replay.test.det.b.scratch"_id, "replay.test.det.b.rec"_id));

    ReplayProducer rp;
    replay_producer_init(&rp, &header, decoded_rows);

    while (replay_produce(&rp, b.e.world.state->tick, b.e.frames, &live_mask) != PRODUCE_WAIT) {
        engine_tick_once(&b.e, b.e.frames);
    }
    TL_ASSERT_EQ(b.rec.rows.count, a.rec.rows.count);

    // Same pin on the replayed side: the folded state, not just the hash trace, reproduces.
    Span<WPos> b_pos = world_column<WPos>(&b.e.world);
    TL_ASSERT_EQ(b_pos.count, 1u);
    TL_EXPECT_EQ(b_pos.data[0].x, 18);

    // The contract: replaying pass A's recorded (and now wire-round-tripped) frames through a
    // fresh world reproduces the exact same per-tick frames and the exact same hash trace.
    for (u32 i = 0; i < a.rec.rows.count; ++i) {
        TL_EXPECT_EQ(b.rec.rows.data[i].world_hash, a.rec.rows.data[i].world_hash);
        TL_EXPECT_EQ(memcmp(&b.rec.rows.data[i].frames[0], &a.rec.rows.data[i].frames[0], sizeof(InputFrame)), 0);
    }
}
