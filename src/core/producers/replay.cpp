// replay.cpp - the Replay InputProducer (docs/INPUT.md §9.4). See replay.h for the contract.
#include "core/producers/replay.h"

void replay_producer_init(ReplayProducer* rp, const RecordedInputHeader* header, const RecordedInputRow* rows) {
    rp->header = header;
    rp->rows = rows;
    rp->cursor = 0u;
}

ProduceResult replay_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask) {
    ReplayProducer* rp = (ReplayProducer*)ctx;
    if (rp->cursor >= rp->header->frame_count) { return PRODUCE_WAIT; }
    TL_CHECK(tick == rp->header->base_tick + rp->cursor);
    const RecordedInputRow& row = rp->rows[rp->cursor];
    for (u32 p = 0; p < rp->header->peer_count; ++p) { out[p] = row.frames[p]; }
    *live_mask = rp->header->live_mask;
    rp->cursor += 1u;
    return PRODUCE_READY;
}

u64 replay_last_hash(const ReplayProducer* rp) {
    TL_CHECK(rp->cursor > 0u);
    return rp->rows[rp->cursor - 1u].world_hash;
}

bool replay_exhausted(const ReplayProducer* rp) {
    return rp->cursor >= rp->header->frame_count;
}
