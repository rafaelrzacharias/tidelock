#pragma once
// ---------------------------------------------------------------------------------------------
// net_internal.h - net's private state: the per-slot frame rings and the LogRecord store.
//
// Spec: docs/NETCODE.md §20.1 (this file's contents and its "included only by net/*.cpp" rule),
//   §20.3(a) (the sender's own_ring and the receiver's per-slot placement rules), §20.2.3 and
//   §20.3(b) (the LogRecord store's identity and ordering rules).
// Scope: PHASE 1 ONLY (docs/NETCODE.md §20.8 as amended by the RR-17 ruling, 2026-08-26). The
//   rings and the record store are here because the encoder and the archive need somewhere to
//   put frames and records. NetState's peer tables, the sequencer's bitmaps, the φ accrual and
//   the session machine arrive with Phases 2 and 5 - this header is deliberately not a stub of
//   them, because a stub of an unbuilt design is a guess that later reads as a decision.
// Purpose: hold frames by tick with O(1) placement and lookup, and hold the sequenced one-shots
//   with the duplicate-suppression R6 requires.
// Invariants: SLOT_RING_TICKS is a power of two and covers the redundancy window plus the
//   confirmation horizon (asserted in net/wire.h); a ring slot is occupied only if its stored
//   tick matches the query, so a wrapped-past entry can never be mistaken for a live one - the
//   ring stores the tick alongside the frame rather than trusting an index.
// Determinism: pure integer state, no io, no alloc, no floats, no wall clock. NetState lives in
//   a NON-REGISTERED arena (docs/NETCODE.md §20.1): net is outside the sim boundary and nothing
//   here is hashed into world state or snapshotted.
// Threading: single-threaded; touched from sys_net_receive/sys_net_send only (§20.5).
// Includes: net/wire.h.
// ---------------------------------------------------------------------------------------------
#include "net/wire.h"

// One slot's frame ring. Frames are placed by absolute tick and read back by absolute tick; the
// stored tick is what makes a slot occupied, so nothing needs clearing on wrap.
struct SlotRing {
    WireFrame frames[SLOT_RING_TICKS];
    u64       ticks[SLOT_RING_TICKS];   // the absolute tick each slot holds
    u8        occupied[SLOT_RING_TICKS];
};
// occupied[] ends 8-aligned, so there is no trailing gap to name. Two u32s sat here calling
// themselves _pad0/_pad1: they were eight REAL bytes that slot_ring_clear never zeroed, which
// made this header's "every gap is a named _padN" claim false for exactly those bytes.
static_assert(sizeof(SlotRing) == sizeof(WireFrame) * SLOT_RING_TICKS
                                + sizeof(u64) * SLOT_RING_TICKS
                                + SLOT_RING_TICKS,
              "SlotRing carries no implicit padding");

// Clears every entry. Cheap enough to call at session start; never called per tick.
inline void slot_ring_clear(SlotRing* r) {
    TL_ASSERT(r != nullptr);
    for (u32 i = 0; i < SLOT_RING_TICKS; ++i) {
        r->ticks[i] = 0u;
        r->occupied[i] = 0u;
    }
}

// The ring index a tick maps to. SLOT_RING_TICKS is a power of two (asserted in wire.h), so this
// is a mask, and it is written as one rather than a modulo so the intent survives a retune.
inline u32 slot_ring_index(u64 tick) { return (u32)(tick & (u64)(SLOT_RING_TICKS - 1u)); }

// Stores `f` at `tick`, overwriting whatever the slot held. The caller decides whether a frame
// is wanted (docs/NETCODE.md §20.3(a): a receiver discards a tick at or below last_finalized, or
// one it already holds) - this is the placement primitive, not that policy.
inline void slot_ring_put(SlotRing* r, u64 tick, const WireFrame* f) {
    TL_ASSERT(r != nullptr && f != nullptr);
    const u32 i = slot_ring_index(tick);
    r->frames[i] = *f;
    r->ticks[i] = tick;
    r->occupied[i] = 1u;
}

// True iff the ring holds the frame for exactly `tick`. A tick that has wrapped out reads as
// absent rather than as the newer frame occupying its index.
inline bool slot_ring_has(const SlotRing* r, u64 tick) {
    TL_ASSERT(r != nullptr);
    const u32 i = slot_ring_index(tick);
    return r->occupied[i] != 0u && r->ticks[i] == tick;
}

// The frame at `tick`, or nullptr when the ring does not hold it.
inline const WireFrame* slot_ring_get(const SlotRing* r, u64 tick) {
    TL_ASSERT(r != nullptr);
    const u32 i = slot_ring_index(tick);
    if (r->occupied[i] == 0u || r->ticks[i] != tick) { return nullptr; }
    return &r->frames[i];
}

// How many LogRecords the store holds - net's in-memory sequenced-record working set. The
// durable history lives in the archive, not here (docs/NETCODE.md §13.5). The VALUE's home is
// docs/CANON.md's netcode tunables (homed there by the 2026-08-26 ruling, which is what stopped
// it being a number this lane had picked); this line is the code's copy of it.
constexpr u32 LOG_STORE_CAPACITY = 256u;

// The sequenced one-shots, kept in the order they were added. R6's stable id is
// (origin_slot, seq) and duplicates are no-ops (docs/NETCODE.md §20.2.3), which is what lets a
// record be announced in `pending` and then arrive again in its tick's SeqSection.
struct LogStore {
    LogRecord records[LOG_STORE_CAPACITY];
    u32       count;
    u32       _pad0;
};

// Empties the store.
inline void log_store_clear(LogStore* s) {
    TL_ASSERT(s != nullptr);
    s->count = 0u;
    s->_pad0 = 0u;   // the same "named padding is zeroed" rule SlotRing's note above states
}

// True iff the store already holds a record with this (origin_slot, seq).
inline bool log_store_has(const LogStore* s, u8 origin_slot, u32 seq) {
    TL_ASSERT(s != nullptr);
    for (u32 i = 0; i < s->count; ++i) {
        if (s->records[i].origin_slot == origin_slot && s->records[i].seq == seq) { return true; }
    }
    return false;
}

// The archive refuses more than MAX_LOG_RECORDS_PER_PACKET records at one effective_tick
// (docs/NETCODE.md §20.2.9, the bound archive_encode_segment TL_CHECKs). Counting them here is
// what stops a caller assembling a tick the encoder would then abort on: the invariant belongs
// where records are ADMITTED, not only where they are written out.
inline u32 log_store_count_at_tick(const LogStore* s, u64 tick) {
    TL_ASSERT(s != nullptr);
    u32 n = 0;
    for (u32 i = 0; i < s->count; ++i) { if (s->records[i].effective_tick == tick) { ++n; } }
    return n;
}

// Adds `rec`. ERR_OK when it was stored; ERR_NET_DUPLICATE_RECORD when its (origin_slot, seq)
// was already present - a no-op per R6, expected rather than exceptional, since a record is
// announced in `pending` and then arrives again in its tick's SeqSection; ERR_NET_STORE_FULL
// when there is no room, which is DATA LOSS and a different thing entirely. A bool conflated
// the two and left the caller unable to fail loudly (CLAUDE.md: fail loudly and explicitly).
inline ErrCode log_store_add(LogStore* s, const LogRecord* rec) {
    TL_ASSERT(s != nullptr && rec != nullptr);
    if (log_store_has(s, rec->origin_slot, rec->seq)) { return ERR_NET_DUPLICATE_RECORD; }
    if (s->count >= LOG_STORE_CAPACITY) { return ERR_NET_STORE_FULL; }
    // ERR_NET_MALFORMED rather than a fatal: a caller that has produced too many records for one
    // tick has a bug, but finding out here beats finding out inside the encoder's TL_CHECK,
    // which aborts the process.
    if (log_store_count_at_tick(s, rec->effective_tick) >= MAX_LOG_RECORDS_PER_PACKET) {
        return ERR_NET_MALFORMED;
    }
    s->records[s->count] = *rec;
    ++s->count;
    return ERR_OK;
}

// Copies the records effective at exactly `tick` into out[], ascending by (origin_slot, seq) -
// the order docs/NETCODE.md §20.2.2 requires of a SeqSection and §20.2.9 of a segment. Returns
// the number written; writes at most `cap`. Selection-ordered rather than sorted in place so the
// store's own order is never disturbed by a read.
//
// `out_truncated` (optional) is set when the tick held MORE records than `cap`. A caller that
// silently ships a short SeqSection drops a sequenced one-shot, and a count alone cannot say
// whether that happened - "fail loudly and explicitly" (CLAUDE.md). Pass nullptr only when the
// buffer is provably large enough.
inline u32 log_store_at_tick(const LogStore* s, u64 tick, LogRecord* out, u32 cap,
                             bool* out_truncated = nullptr) {
    TL_ASSERT(s != nullptr && (out != nullptr || cap == 0u));
    if (out_truncated != nullptr) { *out_truncated = false; }
    u32 n = 0;
    for (;;) {
        // The smallest (origin_slot, seq) at this tick that is strictly greater than the last
        // one emitted. One pass per output record: the counts here are single digits.
        const LogRecord* best = nullptr;
        for (u32 i = 0; i < s->count; ++i) {
            const LogRecord* c = &s->records[i];
            if (c->effective_tick != tick) { continue; }
            if (n > 0u) {
                const LogRecord* prev = &out[n - 1];
                const bool after = (c->origin_slot > prev->origin_slot)
                                || (c->origin_slot == prev->origin_slot && c->seq > prev->seq);
                if (!after) { continue; }
            }
            if (best == nullptr
                || c->origin_slot < best->origin_slot
                || (c->origin_slot == best->origin_slot && c->seq < best->seq)) {
                best = c;
            }
        }
        if (best == nullptr) { break; }
        if (n >= cap) {
            if (out_truncated != nullptr) { *out_truncated = true; }
            break;
        }
        out[n] = *best;
        ++n;
    }
    return n;
}
