// time.cpp - Clock (docs/FRAME-LOOP.md §1, §8.1). See time.h for the contract.
#include "core/time.h"

void clock_init(Clock* c, const ClockApi* api) {
    TL_ASSERT(api != nullptr);
    c->api = api;
    c->freq = api->frequency(api->ctx);
    c->last_ticks = api->ticks(api->ctx);
    c->primed = 1u;
    for (u32 i = 0; i < 7u; ++i) { c->_pad0[i] = 0u; }
}

f64 clock_tick(Clock* c) {
    TL_ASSERT(c->primed != 0u);
    const u64 now = c->api->ticks(c->api->ctx);
    const u64 delta = now - c->last_ticks;   // ticks() is monotonic (docs/PLATFORM.md §3)
    c->last_ticks = now;
    f64 dt = (f64)delta / (f64)c->freq;
    if (dt < 0.0) { dt = 0.0; }
    if (dt > 0.25) { dt = 0.25; }
    return dt;
}
