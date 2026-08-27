// probe.cpp - the tick-throttled TSV sink behind TL_PROBE_* (docs/TOOLING.md §9.2, §9.3.3).
// Tooling plane (RR-7, docs/CPP-SUBSET.md §9 R-4): real io and namespace-scope mutable state are
// sanctioned here - see foundation/tl_probe.h's contract block for the current simplifications
// (u64 key instead of NameHash, a linear scan instead of Map<K,V>, no ClockApi/StrView/tick_ptr).
#include "foundation/tl_probe.h"

#include "foundation/tl_assert.h"
#include "foundation/tl_log.h"

#include <stdio.h>
#include <string.h>

// docs/TOOLING.md §9.1's file table: "TL_DEV only (prof.cpp not built otherwise)" - probe.cpp is
// the same shape. The whole runtime is compiled out here rather than excluded from the CMake
// source list, so a stray call site outside TL_DEV is a link error, not silent absence.
#if TL_DEV

namespace {

struct ProbeState {
    ProbeKey keys[1024];   // linear scan; Map<K,V> (CONTAINERS.md) replaces this once that lane lands
    u32 count;
    char staging[65536];
    u32 staging_used;
};
ProbeState g_probe = {};
u64 g_tick = 0;   // FRAME-LOOP.md's tick counter is not wired yet (TODO.md)

void append(const char* s) {
    const usize len = strlen(s);
    // The staging buffer is bounded (docs/TOOLING.md §9.2); a diagnostic sink drops the tail
    // rather than corrupting memory or faulting - there is no consumer for a partial-write signal
    // yet (the flush-to-disk path waits for PlatformApi.file.append, same as tl_log.h's).
    if (g_probe.staging_used + len >= sizeof(g_probe.staging)) { return; }
    memcpy(g_probe.staging + g_probe.staging_used, s, len);
    g_probe.staging_used += (u32)len;
}

ProbeKey* find(u64 key) {
    for (u32 i = 0; i < g_probe.count; ++i) {
        if (g_probe.keys[i].key == key) { return &g_probe.keys[i]; }
    }
    return nullptr;
}

ProbeKey& lookup_or_insert(u64 key, const char* name, u8 kind, u8 frac) {
    ProbeKey* found = find(key);
    if (found) { return *found; }
    TL_CHECK(g_probe.count < 1024u);   // "insert: fatal at 1024" (docs/TOOLING.md §9.3.3)
    ProbeKey& k = g_probe.keys[g_probe.count++];
    k = ProbeKey{};
    k.key = key;
    k.name = name;
    k.kind = kind;
    k.frac_bits = frac;
    k.enabled = 1;
    return k;
}

f64 scale_down(i64 raw, u8 frac) {
    f64 v = (f64)raw;
    for (u8 i = 0; i < frac; ++i) { v *= 0.5; }
    return v;
}

// TSV row: "<tick>\t<key>\t<value>\n" (docs/TOOLING.md §9.2). `has_value` false is MARK's empty
// value field.
void emit_row(const ProbeKey& k, bool has_value) {
    char line[192];
    if (!has_value) {
        snprintf(line, sizeof(line), "%llu\t%s\t\n", (unsigned long long)g_tick, k.name);
    } else if (k.frac_bits) {
        snprintf(line, sizeof(line), "%llu\t%s\t%.9g\n", (unsigned long long)g_tick, k.name,
                 scale_down(k.last_raw, k.frac_bits));
    } else {
        snprintf(line, sizeof(line), "%llu\t%s\t%lld\n", (unsigned long long)g_tick, k.name,
                 (long long)k.last_raw);
    }
    append(line);
}

// Shared stats update for LOG/ON_CHANGE (docs/TOOLING.md §9.3.3: min/max/sum/first/last, changes
// counted against the PREVIOUS raw - the first observation establishes the baseline, it is not
// itself a change).
void update_stats(ProbeKey& k, i64 raw) {
    const f64 v = scale_down(raw, k.frac_bits);
    if (k.count == 0u) {
        k.min = k.max = k.first = v;
        k.sum = 0.0;
    } else {
        if (v < k.min) { k.min = v; }
        if (v > k.max) { k.max = v; }
        if (raw != k.last_raw) { k.changes += 1u; }
    }
    k.sum += v;
    k.last = v;
    k.last_raw = raw;
    k.last_tick = g_tick;
    k.count += 1u;
}

}  // namespace

void tl_probe_log(u64 key, const char* name, i64 raw, u8 frac, u32 n) {
    ProbeKey& k = lookup_or_insert(key, name, PROBE_LOG, frac);
    if (!k.enabled) { return; }
    // `g_tick - k.last_tick`, not `>=` on a difference that can go negative: the tick moves
    // backwards on a replay scrub seek (docs/TOOLING.md §9.3.10) and under
    // tl_probe_test_set_tick, and the u64 wrap then reads as an enormous gap - the throttle
    // becomes a no-op rather than throttling. Clamped, so a backwards tick throttles.
    const u64 since = (g_tick >= k.last_tick) ? (g_tick - k.last_tick) : 0u;
    if (k.count == 0u || since >= (u64)n) {
        update_stats(k, raw);
        emit_row(k, true);
    }
}

void tl_probe_on_change(u64 key, const char* name, i64 raw, i64 eps) {
    TL_CHECK(eps >= 0);   // |raw - last_raw| is unsigned; a negative epsilon has no meaning here
    ProbeKey& k = lookup_or_insert(key, name, PROBE_ON_CHANGE, 0);
    if (!k.enabled) { return; }
    // The magnitude in u64: `raw - k.last_raw` is signed overflow (UB, docs/CPP-SUBSET.md §5) for
    // any pair straddling half the i64 range, and `-diff` is UB outright at INT64_MIN. Unsigned
    // subtraction wraps by definition, and the larger-minus-smaller order makes the result exact.
    const u64 a = (u64)raw, b = (u64)k.last_raw;
    const u64 diff = (raw >= k.last_raw) ? (a - b) : (b - a);
    if (k.count == 0u || diff > (u64)eps) {
        update_stats(k, raw);
        emit_row(k, true);
    }
}

void tl_probe_mark(u64 key, const char* name) {
    ProbeKey& k = lookup_or_insert(key, name, PROBE_MARK, 0);
    if (!k.enabled) { return; }
    k.last_tick = g_tick;
    k.count += 1u;
    emit_row(k, false);
}

void tl_probe_assert(u64 key, const char* name, i64 raw, i64 lo, i64 hi) {
    ProbeKey& k = lookup_or_insert(key, name, PROBE_ASSERT, 0);
    if (!k.enabled) { return; }
    k.last_raw = raw;
    k.last_tick = g_tick;
    k.count += 1u;
    if (raw < lo || raw > hi) {
        emit_row(k, true);
        tl_log_write(LOG_ERR, __FILE__, (u32)__LINE__, "probe %s out of range [%lld,%lld]: %lld",
                     name, (long long)lo, (long long)hi, (long long)raw);
        // `probe_assert_fatal` (default 0 dev, 1 driver) is a core/cvar.h cvar - not built yet
        // (TODO.md), so this never escalates to TL_FATAL today.
    }
}

void tl_probe_write_summary(void) {
    append("#summary\n");
    char line[256];
    for (u32 i = 0; i < g_probe.count; ++i) {
        const ProbeKey& k = g_probe.keys[i];
        const f64 mean = k.count ? (k.sum / (f64)k.count) : 0.0;
        snprintf(line, sizeof(line), "%s\t%llu\t%llu\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\n", k.name,
                 (unsigned long long)k.count, (unsigned long long)k.changes, k.min, k.max, mean,
                 k.first, k.last);
        append(line);
    }
}

u32 tl_probe_key_count(void) { return g_probe.count; }
const ProbeKey* tl_probe_key_at(u32 slot) { TL_CHECK(slot < g_probe.count); return &g_probe.keys[slot]; }
const char* tl_probe_test_staging(void) {
    g_probe.staging[g_probe.staging_used] = 0;
    return g_probe.staging;
}
void tl_probe_test_reset(void) {
    // NOT `g_probe = ProbeState{}` - that value-initializes a ~160 KB TEMPORARY ProbeState
    // (keys[1024] of ProbeKey alone is 1024 * 96 B, plus the 64 KB staging buffer) on the stack
    // before assigning it - the same bug class this file's own module already fixed twice this
    // lane (prof.cpp's ~53 MB ring, log.cpp's ~0.97 MB ring; found here via the literal-pattern
    // grep LESSONS.md's entry for the second instance called for, run again while building the
    // Probes panel). memset zeroes the existing static IN PLACE, no temporary.
    memset(&g_probe, 0, sizeof(g_probe));
    g_tick = 0u;
}
void tl_probe_test_set_tick(u64 tick) { g_tick = tick; }
void tl_probe_test_set_enabled(u64 key, const char* name, u8 enabled) {
    ProbeKey& k = lookup_or_insert(key, name, PROBE_LOG, 0);
    k.enabled = enabled;
}

#endif  // TL_DEV
