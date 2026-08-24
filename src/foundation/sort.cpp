// ---------------------------------------------------------------------------------------------
// sort.cpp - LSD radix sort_u32_kv / sort_u64_kv (docs/CONTAINERS.md §8.5). See sort.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/sort.h"
#include <string.h>   // memcpy only - the final copy-back when the result lands in the scratch buffer

// One counting-sort pass over byte `shift/8` of every key in [src_k, src_k+n). Returns false (no
// buffers swapped) if the byte carries no ordering information at this position (a single nonzero
// histogram bucket) - the doc's own all-equal-keys early-out.
template <typename K>
static bool radix_pass(const K* src_k, const u32* src_v, K* dst_k, u32* dst_v, u32 n, u32 shift) {
    u32 hist[256];
    for (u32 i = 0; i < 256u; ++i) { hist[i] = 0u; }
    for (u32 i = 0; i < n; ++i) {
        u32 b = (u32)((src_k[i] >> shift) & (K)0xFFu);
        hist[b] += 1u;
    }
    u32 nonzero_buckets = 0u;
    for (u32 i = 0; i < 256u; ++i) {
        if (hist[i] != 0u) { nonzero_buckets += 1u; }
    }
    if (nonzero_buckets <= 1u) { return false; }

    u32 prefix[256];
    u32 sum = 0u;
    for (u32 i = 0; i < 256u; ++i) {
        prefix[i] = sum;
        sum += hist[i];
    }
    for (u32 i = 0; i < n; ++i) {
        u32 b = (u32)((src_k[i] >> shift) & (K)0xFFu);
        u32 pos = prefix[b];
        prefix[b] = pos + 1u;
        dst_k[pos] = src_k[i];
        dst_v[pos] = src_v[i];
    }
    return true;
}

template <typename K>
static void sort_kv(K* keys, u32* vals, u32 n, Scratch* s, u32 pass_count) {
    if (n < 2u) { return; }
    scratch_scope_begin(s);
    K* tmp_k = (K*)scratch_push(s, (u64)n * sizeof(K), alignof(K));
    u32* tmp_v = (u32*)scratch_push(s, (u64)n * sizeof(u32), alignof(u32));

    K* src_k = keys; u32* src_v = vals;
    K* dst_k = tmp_k; u32* dst_v = tmp_v;

    for (u32 pass = 0; pass < pass_count; ++pass) {
        u32 shift = pass * 8u;
        if (radix_pass<K>(src_k, src_v, dst_k, dst_v, n, shift)) {
            K* tk = src_k; src_k = dst_k; dst_k = tk;
            u32* tv = src_v; src_v = dst_v; dst_v = tv;
        }
    }

    if (src_k != keys) {
        memcpy(keys, src_k, (usize)n * sizeof(K));
        memcpy(vals, src_v, (usize)n * sizeof(u32));
    }
    scratch_scope_end(s);
}

void sort_u32_kv(u32* keys, u32* vals, u32 n, Scratch* s) { sort_kv<u32>(keys, vals, n, s, 4u); }
void sort_u64_kv(u64* keys, u32* vals, u32 n, Scratch* s) { sort_kv<u64>(keys, vals, n, s, 8u); }
