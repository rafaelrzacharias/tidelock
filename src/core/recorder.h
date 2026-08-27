#pragma once
// ---------------------------------------------------------------------------------------------
// recorder.h - RecordedInput: the file format (docs/DETERMINISM.md §9.2) and the LAST-phase
//   recorder that appends to it every tick.
//
// Spec: docs/DETERMINISM.md §9.2 (the format, shared by the recorder, the Replay producer's
//   reader, and Hovel), docs/INPUT.md §9.1 (core/recorder.cpp), §9.5 (this file's algorithm).
// Purpose: any producer's output can be recorded (frames + the world hash) into one
//   RecordedInput stream; replaying it through the Replay producer and comparing the hash trace
//   IS the determinism test (docs/INPUT.md §4).
// Invariants: `RecordedInputHeader` is 128 B, TL_WIRE_STRUCT-built (docs/CPP-SUBSET.md §9 R-2) -
//   little-endian, explicit padding, a leading `format_version`. The body is NOT a fixed struct
//   (`frame_count` and `peer_count` are runtime-sized) - `recorder_write`/`recorder_read_*` walk
//   it by hand through the same ByteWriter/ByteReader pair (foundation/bytes.h) every wire format
//   in the tree uses. `world_hash` is `registry_hash_all`'s return (docs/MEMORY.md §8.3) -
//   COMPUTED HERE, not read from a "checkpoint system": no such system exists anywhere in the
//   tree yet (docs/FRAME-LOOP.md §2's LAST-phase "determinism checkpoint" row names a mechanism
//   this lane could not find a consumer or owner for - filed in TODO.md, same entry as the
//   interp-pairs/alpha gap in core/loop.h: SystemFn's `void(*)(World*)` shape has no path to
//   Engine-level context, so `recorder_tick` is called directly from `engine_tick_once`, not
//   registered as a LAST-phase system). Per-arena hashes (HAS_ARENA_HASHES) are NOT written by
//   this lane's recorder (flag always 0) - filed in TODO.md for whoever needs the dev-only
//   per-arena desync breakdown; the single combined `world_hash` is what the replay-trace
//   determinism test compares.
// Determinism: `recorder_tick`/`recorder_write` are pure functions of World's registered arenas
//   and the caller's frames - no floats, no clock, no allocation outside the caller's arena.
// Threading: single-threaded; `recorder_tick` runs once per tick, called from engine_tick_once
//   (docs/FRAME-LOOP.md §2: "LAST... record->replay log").
// Includes: core/input.h, core/world.h, foundation/{vmem_arena,array,bytes}.h.
// ---------------------------------------------------------------------------------------------
#include "core/input.h"
#include "core/world.h"
#include "foundation/array.h"
#include "foundation/bytes.h"

enum : u16 { RECORDED_INPUT_FLAG_HAS_ARENA_HASHES = 1u << 0 };

constexpr u8 RECORDED_INPUT_MAGIC[4] = { (u8)'T', (u8)'L', (u8)'R', (u8)'I' };

// docs/DETERMINISM.md §9.2's header, 128 B, field for field (reordered only to insert the one
// explicit alignment pad frame_count's u64 needs - the doc's prose order is otherwise preserved).
#define TL_FIELDS_RecordedInputHeader(X, XA, XH) \
    XA(u8, magic, 4) \
    XA(u8, build_id, 32) \
    XA(u8, session_fingerprint, 32) \
    X(u64, seed) \
    X(u64, base_tick) \
    X(u8, peer_count) \
    X(u8, live_mask) \
    X(u16, flags) \
    XA(u8, _pad1, 4) \
    X(u64, frame_count) \
    XA(u8, _pad2, 24)
#define TL_OFFSETS_RecordedInputHeader(XO) \
    XO(magic, 4) \
    XO(build_id, 8) \
    XO(session_fingerprint, 40) \
    XO(seed, 72) \
    XO(base_tick, 80) \
    XO(peer_count, 88) \
    XO(live_mask, 89) \
    XO(flags, 90) \
    XO(_pad1, 92) \
    XO(frame_count, 96) \
    XO(_pad2, 104)
TL_WIRE_STRUCT(RecordedInputHeader)
static_assert(sizeof(RecordedInputHeader) == 128u, "docs/DETERMINISM.md section 9.2");

// The core module's ErrCode range is 0x03xx (reflect.h already carries ERR_WIRE_PAD_NONZERO
// there); this row is the recorder's own.
constexpr ErrCode ERR_RECORDER_BAD_MAGIC   = (ErrCode)0x0321;  // header magic != "TLRI"
constexpr ErrCode ERR_RECORDER_FINGERPRINT = (ErrCode)0x0322;  // session_fingerprint mismatch (docs/DETERMINISM.md section 9.2)
constexpr ErrCode ERR_RECORDER_VERSION     = (ErrCode)0x0323;  // format_version this build has no reader for
constexpr ErrCode ERR_RECORDER_CRC         = (ErrCode)0x0324;  // body crc32 mismatch
constexpr ErrCode ERR_RECORDER_PEER_COUNT  = (ErrCode)0x0325;  // header peer_count > MAX_PEERS (RecordedInputRow::frames overflow)

constexpr u32 RECORDED_INPUT_FORMAT_VERSION = 1u;

// One recorded tick's row: every live peer's frame (docs/DETERMINISM.md §9.2: "InputFrame[peer_
// count]") plus the combined world hash. Stored MAX_PEERS-wide for a fixed row size regardless of
// peer_count (the header's peer_count says how many are meaningful on read-back).
struct RecordedInputRow { InputFrame frames[MAX_PEERS]; u64 world_hash; };

// The in-memory recorder (docs/INPUT.md §9.5): a fixed-capacity row array (the dev "2 min ring"
// and the driver's "unbounded, buffered write" are both future refinements over this - filed in
// TODO.md; v0 is one bounded in-memory table, TL_FATAL on overflow like every other fixed array
// in the tree).
struct Recorder {
    Array<RecordedInputRow> rows;
    u64 base_tick;
    u64 seed;
    u8  peer_count;
    u8  live_mask;
    u8  build_id[32];
    u8  session_fingerprint[32];
};

// Sizes `rows` from `arena` (array_init_fixed, max_rows capacity); base_tick is the first
// recorded tick's number (docs/FRAME-LOOP.md §1 tick width rule - the file's own base_tick).
void recorder_init(Recorder* rec, VMemArena* arena, u32 max_rows, u64 base_tick, u64 seed,
                   u8 peer_count, u8 live_mask, const u8 build_id[32], const u8 session_fingerprint[32]);

// Appends one row: `frames` (MAX_PEERS entries - the caller's Engine::frames buffer) and the
// world hash computed by `registry_hash_all(w->registry, ...)` (docs/MEMORY.md §8.3). Called from
// engine_tick_once, once per tick, after the LAST phase has run (see this header's contract
// block for why this is a direct call rather than a registered LAST-phase system). TL_FATAL on
// `rows` overflow.
void recorder_tick(Recorder* rec, World* w, const InputFrame* frames);

// Bytes `recorder_write` needs for the current row count (header + body; no trailer - the crc32
// is computed over exactly this many bytes of body, see recorder_write).
u64 recorder_bytes_needed(const Recorder* rec);

// Writes header + body + a trailing crc32 over the body (docs/DETERMINISM.md §9.2). Returns the
// bytes written. TL_CHECK on caller buffer overflow (bytes.h's own contract).
u64 recorder_write(const Recorder* rec, ByteWriter* w);

// Reads a RecordedInput stream back: validates magic/format_version/the body crc32, checks
// `expected_session_fingerprint` (null to skip the check) against the stored one
// (ERR_RECORDER_FINGERPRINT on mismatch, docs/DETERMINISM.md §9.2: "the Replay producer refuses a
// file whose session_fingerprint differs"). Also validates the two counts the caller is about to
// SIZE A BUFFER FROM, before handing them back: `peer_count` <= MAX_PEERS (ERR_RECORDER_PEER_COUNT
// - otherwise recorder_read_body's `row.frames[p]` loop overflows RecordedInputRow's fixed
// MAX_PEERS-wide array) and `frame_count` fits in the reader's remaining bytes at the declared
// per-row size (the sticky truncation code otherwise - a corrupt/truncated file's frame_count is
// fully attacker/corruption-controlled and would otherwise size the caller's out_rows buffer, or
// drive recorder_read_body's write loop, from an unbounded value). `out_rows` must hold the
// decoded header's frame_count entries - callers read the header FIRST (this function) to size it.
// Returns ERR_RECORDER_BAD_MAGIC, the sticky truncation code, ERR_RECORDER_PEER_COUNT,
// ERR_RECORDER_FINGERPRINT, or ERR_RECORDER_VERSION.
ErrCode recorder_read_header(ByteReader* r, RecordedInputHeader* out, const u8 expected_session_fingerprint[32]);

// Reads `header->frame_count` rows plus the trailing body crc32; `out_rows` must hold
// `header->frame_count` entries (sized from a prior recorder_read_header call). Returns the
// sticky truncation code or ERR_RECORDER_CRC on a body crc32 mismatch.
ErrCode recorder_read_body(ByteReader* r, const RecordedInputHeader* header, RecordedInputRow* out_rows);
