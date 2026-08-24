// events.cpp - the headless EventApi (docs/PLATFORM.md §9.4 "events": pump is a no-op; tests
// inject events by pushing into the ring directly via headless_event_ring()).
#include "platform/impl_headless/headless_apis.h"
#include "platform/impl_headless/headless_test_api.h"

#include "foundation/tl_assert.h"

namespace {

u32 he_pump(void*, RingBuffer<RawEvent>*) {
    return 0u;
}

// Not a separate counter: nothing headless ever pops this ring (persistent-mode consumption is
// peek-based), so every `tail` advance is an overflow eviction - `head - cap` (once `head > cap`)
// is exactly the drop count. Both are monotonic counters, not wrapped indices (foundation/ring.h).
u32 he_dropped_total(void* ctx) {
    RingBuffer<RawEvent>* r = &((HeadlessState*)ctx)->events;
    return r->head > r->cap ? r->head - r->cap : 0u;
}

}  // namespace

EventApi headless_events_api(HeadlessState* s) {
    return EventApi{ s, he_pump, he_dropped_total };
}

RingBuffer<RawEvent>* headless_event_ring(const PlatformApi* api) {
    TL_CHECK(api != nullptr && api->is_headless);
    return &((HeadlessState*)api->events.ctx)->events;
}
