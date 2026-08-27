// cvar.h - registration (sorted-index insertion), typed access, sim fingerprint fold, and a
// pure console/archive formatter+parser standing in for fmt_buf (foundation/fmt.h is a STUB,
// blocked on vendor/stb_sprintf - see that header's own contract block; a real formatter would
// TL_FATAL today). Spec: docs/TOOLING.md §3, §9.1, §9.2, §9.3.5.
#include "core/cvar.h"

#include "foundation/fx.h"   // fx_parse_decimal_raw (RR-38) - the CVAR_FX_RAW decimal-literal half

#include <string.h>   // memcpy - the sanctioned raw<->typed conversion (docs/CPP-SUBSET.md section 1)

void cvar_table_init(CvarTable* t) {
    memset(t, 0, sizeof(CvarTable));
}

const CvarDesc* cvar_find(const CvarTable* t, NameHash key) {
    // Binary search over `sorted` (key-ascending index into desc/bits).
    u32 lo = 0, hi = t->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        const NameHash mk = t->desc[t->sorted[mid]]->key;
        if (mk == key) { return t->desc[t->sorted[mid]]; }
        if (mk < key) { lo = mid + 1; } else { hi = mid; }
    }
    return nullptr;
}

u32 cvar_find_index(const CvarTable* t, NameHash key) {
    u32 lo = 0, hi = t->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        const u16 idx = t->sorted[mid];
        const NameHash mk = t->desc[idx]->key;
        if (mk == key) { return idx; }
        if (mk < key) { lo = mid + 1; } else { hi = mid; }
    }
    return CVAR_TABLE_CAP;
}

void cvar_register(CvarTable* t, const CvarDesc* desc) {
    if (t->count >= CVAR_TABLE_CAP) { TL_FATAL("cvar_register: CVAR_TABLE_CAP exhausted"); }
    if (cvar_find(t, desc->key) != nullptr) { TL_FATAL("cvar_register: duplicate cvar key"); }

    const u32 idx = t->count;
    t->desc[idx] = desc;
    t->bits[idx] = desc->default_bits;

    // Insert idx into `sorted` at its key-ascending position (insertion sort - init-only).
    u32 pos = t->count;
    for (u32 i = 0; i < t->count; ++i) {
        if (desc->key < t->desc[t->sorted[i]]->key) { pos = i; break; }
    }
    for (u32 i = t->count; i > pos; --i) { t->sorted[i] = t->sorted[i - 1]; }
    t->sorted[pos] = (u16)idx;
    t->count += 1u;
}

u32 cvar_get_raw(const CvarTable* t, NameHash key) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    return t->bits[idx];
}

i32 cvar_get_i32(const CvarTable* t, NameHash key) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    TL_CHECK(t->desc[idx]->kind == CVAR_I32);
    i32 v; memcpy(&v, &t->bits[idx], sizeof(v));
    return v;
}

u32 cvar_get_u32(const CvarTable* t, NameHash key) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    TL_CHECK(t->desc[idx]->kind == CVAR_U32);
    return t->bits[idx];
}

f32 cvar_get_f32(const CvarTable* t, NameHash key) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    TL_CHECK(t->desc[idx]->kind == CVAR_F32);
    f32 v; memcpy(&v, &t->bits[idx], sizeof(v));
    return v;
}

bool cvar_get_bool(const CvarTable* t, NameHash key) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    TL_CHECK(t->desc[idx]->kind == CVAR_BOOL);
    return t->bits[idx] != 0u;
}

i32 cvar_get_fx_raw(const CvarTable* t, NameHash key, u8* out_frac) {
    const u32 idx = cvar_find_index(t, key);
    TL_CHECK(idx < t->count);
    TL_CHECK(t->desc[idx]->kind == CVAR_FX_RAW);
    *out_frac = t->desc[idx]->frac_bits;
    i32 v; memcpy(&v, &t->bits[idx], sizeof(v));
    return v;
}

ErrCode cvar_set_bits_unchecked(CvarTable* t, NameHash key, u32 bits) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    t->bits[idx] = bits;
    return ERR_OK;
}

ErrCode cvar_set_raw(CvarTable* t, NameHash key, u32 bits) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    const CvarDesc* d = t->desc[idx];
    if (d->flags & CVAR_READONLY) { return ERR_CVAR_READONLY; }
    if (d->flags & CVAR_SIM) { return ERR_CVAR_SIM_UNROUTED; }
    t->bits[idx] = bits;
    return ERR_OK;
}

ErrCode cvar_apply_sim_raw(CvarTable* t, NameHash key, u32 bits) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    if (t->desc[idx]->flags & CVAR_READONLY) { return ERR_CVAR_READONLY; }
    t->bits[idx] = bits;
    return ERR_OK;
}

ErrCode cvar_set_i32(CvarTable* t, NameHash key, i32 v) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    TL_CHECK(t->desc[idx]->kind == CVAR_I32);
    u32 bits; memcpy(&bits, &v, sizeof(bits));
    return cvar_set_raw(t, key, bits);
}

ErrCode cvar_set_u32(CvarTable* t, NameHash key, u32 v) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    TL_CHECK(t->desc[idx]->kind == CVAR_U32);
    return cvar_set_raw(t, key, v);
}

ErrCode cvar_set_f32(CvarTable* t, NameHash key, f32 v) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    TL_CHECK(t->desc[idx]->kind == CVAR_F32);
    u32 bits; memcpy(&bits, &v, sizeof(bits));
    return cvar_set_raw(t, key, bits);
}

ErrCode cvar_set_bool(CvarTable* t, NameHash key, bool v) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    TL_CHECK(t->desc[idx]->kind == CVAR_BOOL);
    return cvar_set_raw(t, key, v ? 1u : 0u);
}

u64 cvar_sim_fold_bits(const CvarTable* t, u64 h) {
    for (u32 i = 0; i < t->count; ++i) {
        const u32 idx = t->sorted[i];
        const CvarDesc* d = t->desc[idx];
        if ((d->flags & CVAR_SIM) == 0u) { continue; }
        h = tl_hash64(&d->key, sizeof(d->key), h);
        h = tl_hash64(&t->bits[idx], sizeof(t->bits[idx]), h);
    }
    return h;
}

// --- pure formatting/parsing (no libc, no fmt_buf - see this file's top comment) --------------

namespace {

// Writes `v` (decimal, may be negative) into `out[0..out_cap)`; returns bytes written excluding
// the NUL, or 0 on overflow. i64 so INT32_MIN's magnitude (2147483648, exceeds i32 range) is
// representable without UB (docs/CPP-SUBSET.md section 5 - the unsigned-domain-difference idiom
// this codebase already uses elsewhere, tests/runner/tl_test.h's tl_near_fx).
u32 fmt_decimal_i64(i64 v, char* out, u32 out_cap) {
    char buf[24];
    u32 n = 0;
    const bool neg = v < 0;
    u64 mag = neg ? ((u64)0 - (u64)v) : (u64)v;
    do { buf[n++] = (char)('0' + (mag % 10u)); mag /= 10u; } while (mag != 0u && n < sizeof(buf));
    const u32 len = n + (neg ? 1u : 0u);
    if (len >= out_cap) { return 0u; }   // >= : room for the NUL too
    u32 w = 0;
    if (neg) { out[w++] = '-'; }
    while (n > 0u) { out[w++] = buf[--n]; }
    out[w] = '\0';
    return w;
}

// Strict: optional leading '-', then 1+ ASCII digits, nothing else. Rejects "", "12x", "-", "+1"
// (docs/LESSONS.md: "atoll answers 0 for 'abc'... a CLI that takes numbers needs a strict
// parser"). Range-checked against [lo, hi] (both inclusive, as i64 so i32/u32 share one parser).
bool parse_decimal_i64(const char* s, i64 lo, i64 hi, i64* out) {
    if (s == nullptr || s[0] == '\0') { return false; }
    u32 i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i = 1u; if (s[1] == '\0') { return false; } }
    u64 mag = 0;
    for (; s[i] != '\0'; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') { return false; }
        if (mag > (u64)0x1999999999999999ULL) { return false; }   // overflow guard, generous
        mag = mag * 10u + (u64)(c - '0');
    }
    const i64 v = neg ? -(i64)mag : (i64)mag;
    if (v < lo || v > hi) { return false; }
    *out = v;
    return true;
}

// A simple fixed-notation f32 formatter/parser (no exponent form): sign, integer digits,
// optional '.', up to 6 fractional digits. Stands in for stb_sprintf's "%.9g" until fmt_buf
// lands (this file's top comment) - dev-only display precision, not a hashed/fingerprinted
// value (no CVAR_F32 is CVAR_SIM in docs/CANON.md's rev-1 set).
u32 fmt_fixed_f32(f32 v, char* out, u32 out_cap) {
    const bool neg = v < 0.0f;
    f32 av = neg ? -v : v;
    i64 ip = (i64)av;                       // truncates toward zero (av >= 0)
    f32 frac = av - (f32)ip;
    u32 fp = (u32)(frac * 1000000.0f + 0.5f);   // 6 fractional digits, rounded
    if (fp >= 1000000u) { fp -= 1000000u; ip += 1; }   // carry from rounding 0.9999995+

    char ipbuf[24];
    u32 ipn = fmt_decimal_i64(neg ? -ip : ip, ipbuf, sizeof(ipbuf));
    if (ipn == 0u) { return 0u; }

    // Trim trailing zero fractional digits (never all six unless the value is exact).
    char fpbuf[7];
    for (i32 d = 5; d >= 0; --d) { fpbuf[d] = (char)('0' + (fp % 10u)); fp /= 10u; }
    u32 fpn = 6u;
    while (fpn > 0u && fpbuf[fpn - 1] == '0') { fpn -= 1u; }

    const u32 total = ipn + (fpn > 0u ? 1u + fpn : 0u);
    if (total >= out_cap) { return 0u; }
    memcpy(out, ipbuf, ipn);
    u32 w = ipn;
    if (fpn > 0u) { out[w++] = '.'; memcpy(out + w, fpbuf, fpn); w += fpn; }
    out[w] = '\0';
    return w;
}

// Strict fixed-notation parser matching fmt_fixed_f32's spelling: optional '-', 1+ digits,
// optional '.' + 1..6 digits. Rejects exponent forms, empty, trailing garbage.
bool parse_fixed_f32(const char* s, f32* out) {
    if (s == nullptr || s[0] == '\0') { return false; }
    u32 i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i = 1u; }
    if (s[i] < '0' || s[i] > '9') { return false; }
    i64 ip = 0;
    u32 ip_digits = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        if (ip_digits >= 18u) { return false; }   // overflow guard
        ip = ip * 10 + (i64)(s[i] - '0');
        ++i; ++ip_digits;
    }
    f32 frac = 0.0f;
    if (s[i] == '.') {
        ++i;
        if (s[i] < '0' || s[i] > '9') { return false; }   // "12." is rejected - require a digit
        f32 scale = 0.1f;
        u32 frac_digits = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            if (frac_digits >= 6u) { return false; }   // more precision than the formatter emits
            frac += (f32)(s[i] - '0') * scale;
            scale *= 0.1f;
            ++i; ++frac_digits;
        }
    }
    if (s[i] != '\0') { return false; }   // trailing garbage (exponent forms land here)
    const f32 mag = (f32)ip + frac;
    *out = neg ? -mag : mag;
    return true;
}

}  // namespace

u32 cvar_format(const CvarTable* t, NameHash key, char* out, u32 out_cap) {
    TL_CHECK(out_cap > 0u);
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return 0u; }
    const CvarDesc* d = t->desc[idx];
    switch ((CvarKind)d->kind) {
        case CVAR_I32: { i32 v; memcpy(&v, &t->bits[idx], sizeof(v)); return fmt_decimal_i64(v, out, out_cap); }
        case CVAR_U32: { return fmt_decimal_i64((i64)t->bits[idx], out, out_cap); }
        case CVAR_BOOL: { return fmt_decimal_i64(t->bits[idx] != 0u ? 1 : 0, out, out_cap); }
        case CVAR_F32: { f32 v; memcpy(&v, &t->bits[idx], sizeof(v)); return fmt_fixed_f32(v, out, out_cap); }
        case CVAR_FX_RAW: {
            i32 v; memcpy(&v, &t->bits[idx], sizeof(v));
            if (out_cap < 5u) { return 0u; }   // "raw:" + at least one digit + NUL
            memcpy(out, "raw:", 4);
            char num[24];
            const u32 nn = fmt_decimal_i64(v, num, sizeof(num));
            if (nn == 0u || 4u + nn >= out_cap) { return 0u; }
            memcpy(out + 4, num, nn);
            out[4 + nn] = '\0';
            return 4u + nn;
        }
    }
    TL_FATAL("cvar_format: unreachable kind");
}

ErrCode cvar_parse_and_set(CvarTable* t, NameHash key, const char* value) {
    const u32 idx = cvar_find_index(t, key);
    if (idx >= t->count) { return ERR_CVAR_NOT_FOUND; }
    const CvarDesc* d = t->desc[idx];
    switch ((CvarKind)d->kind) {
        case CVAR_I32: {
            i64 v;
            if (!parse_decimal_i64(value, -2147483648LL, 2147483647LL, &v)) { return ERR_CVAR_PARSE; }
            u32 bits; i32 v32 = (i32)v; memcpy(&bits, &v32, sizeof(bits));
            return cvar_set_raw(t, key, bits);
        }
        case CVAR_U32: {
            i64 v;
            if (!parse_decimal_i64(value, 0, (i64)0xFFFFFFFFULL, &v)) { return ERR_CVAR_PARSE; }
            return cvar_set_raw(t, key, (u32)v);
        }
        case CVAR_BOOL: {
            u32 b;
            if (value != nullptr && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0)) { b = 1u; }
            else if (value != nullptr && (strcmp(value, "0") == 0 || strcmp(value, "false") == 0)) { b = 0u; }
            else { return ERR_CVAR_PARSE; }
            return cvar_set_raw(t, key, b);
        }
        case CVAR_F32: {
            f32 v;
            if (!parse_fixed_f32(value, &v)) { return ERR_CVAR_PARSE; }
            u32 bits; memcpy(&bits, &v, sizeof(bits));
            return cvar_set_raw(t, key, bits);
        }
        case CVAR_FX_RAW: {
            if (value == nullptr) { return ERR_CVAR_PARSE; }
            // "raw:<i32>" - the raw bits, unchanged (docs/TOOLING.md §9.3.5).
            if (value[0] == 'r' && value[1] == 'a' && value[2] == 'w' && value[3] == ':') {
                i64 v;
                if (!parse_decimal_i64(value + 4, -2147483648LL, 2147483647LL, &v)) { return ERR_CVAR_PARSE; }
                u32 bits; i32 v32 = (i32)v; memcpy(&bits, &v32, sizeof(bits));
                return cvar_set_raw(t, key, bits);
            }
            // A bare decimal literal, RNE-quantized at this cvar's own FRAC (RR-38, docs/
            // FX-PALETTE.md §9 R-10) - integer-only, no float/double anywhere in the actual
            // quantization; `value`'s length is `strlen` (already included for `memcpy`) since
            // this door's own `const char*` parameter predates `StrView`.
            const Result<i32> parsed = fx::fx_parse_decimal_raw(StrView{ value, (u32)strlen(value) }, d->frac_bits);
            if (parsed.err != ERR_OK) { return ERR_CVAR_PARSE; }
            u32 bits; memcpy(&bits, &parsed.value, sizeof(bits));
            return cvar_set_raw(t, key, bits);
        }
    }
    TL_FATAL("cvar_parse_and_set: unreachable kind");
}
