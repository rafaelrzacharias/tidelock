#pragma once
// ---------------------------------------------------------------------------------------------
// replay.h - the Replay InputProducer: reads a decoded RecordedInput stream sequentially.
//
// Spec: docs/INPUT.md §4 (producers table), §9.4 (this file's algorithm), §9.6/docs/DETERMINISM.md
//   §9.2 (the RecordedInput format it reads, core/recorder.h).
// Purpose: replays a recorded session frame-for-frame, exposing the recorded per-tick world hash
//   so a harness can compare it against the LIVE hash as it re-runs the ticks - this comparison
//   IS the determinism test (docs/INPUT.md §4: "record it and replaying it through Replay and
//   comparing the hash trace").
// Invariants: READY while rows remain, PRODUCE_WAIT once exhausted (docs/INPUT.md §4: "the
//   driver stops"); this is the ONE producer besides Network that can return WAIT deliberately
//   at end-of-stream (not a lockstep stall). `tick` passed to produce() must equal
//   header->base_tick + the row cursor (TL_CHECK) - a replay is driven by the same
//   engine_tick_once loop that always advances tick by exactly one, so a mismatch means the
//   caller skipped or repeated a tick, which would silently desync the compare this producer
//   exists to support.
// Determinism: pure - serves caller-owned, already-decoded rows in order; no io, no clock.
// Threading: single-threaded; replay_producer_init is init-only.
// Includes: core/input.h, core/recorder.h.
// ---------------------------------------------------------------------------------------------
#include "core/input.h"
#include "core/recorder.h"

struct ReplayProducer {
    const RecordedInputHeader* header;   // caller-owned, outlives the producer
    const RecordedInputRow*    rows;     // caller-owned, header->frame_count entries
    u64 cursor;                          // next row index to serve
};

// Wires rp to an already-decoded header + rows (recorder_read_header/recorder_read_body,
// core/recorder.h); cursor starts at 0.
void replay_producer_init(ReplayProducer* rp, const RecordedInputHeader* header, const RecordedInputRow* rows);

// The InputProducer::produce fn (ctx = ReplayProducer*): TL_CHECK(tick == header->base_tick +
// cursor); PRODUCE_WAIT once cursor == header->frame_count (docs/INPUT.md §4), else fills
// out[0..peer_count) from the current row, sets live_mask = header->live_mask, advances cursor.
ProduceResult replay_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask);

// The world hash recorded for the row LAST served (the one the harness's just-run tick should
// match). TL_CHECK(rp->cursor > 0) - nothing has been served yet otherwise.
u64 replay_last_hash(const ReplayProducer* rp);

// True iff every row has been served (docs/INPUT.md §4: "the driver stops"). Pure.
bool replay_exhausted(const ReplayProducer* rp);
