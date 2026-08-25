// diff.cpp - field-by-field diff of two component rows (desync dumps). Spec: docs/ECS.md §6
// consumer 3, §10.1; docs/DETERMINISM.md §7 step 3 ("reflection diff names the field").
// Contract on reflect_diff_rows in core/reflect.h.
#include "core/reflect.h"
#include <string.h>

namespace {

// The element's raw bits at p, zero-extended (the DiffLine currency).
u64 diff_element_bits(FieldKind k, const u8* p) {
    switch (kind_scalar_size(k)) {
        case 1: { u8 v;  memcpy(&v, p, 1); return v; }
        case 2: { u16 v; memcpy(&v, p, 2); return v; }
        case 4: { u32 v; memcpy(&v, p, 4); return v; }
        default: { u64 v; memcpy(&v, p, 8); return v; }
    }
}

}  // namespace

u32 reflect_diff_rows(const ComponentInfo* info, const void* a, const void* b,
                      DiffLine* out, u32 max_lines) {
    TL_CHECK(info != nullptr && a != nullptr && b != nullptr && (out != nullptr || max_lines == 0u));
    const u8* pa = (const u8*)a;
    const u8* pb = (const u8*)b;
    u32 found = 0;
    for (u32 i = 0; i < info->field_count; ++i) {
        const FieldInfo* f = &info->fields[i];
        const u32 scalar = kind_scalar_size(f->kind);
        for (u32 e = 0; e < f->count; ++e) {
            const u64 off = f->offset + (u64)e * scalar;
            if (memcmp(pa + off, pb + off, scalar) == 0) { continue; }
            if (found < max_lines) {
                out[found].field = f;
                out[found].element = e;
                out[found]._pad0 = 0;
                out[found].a_bits = diff_element_bits(f->kind, pa + off);
                out[found].b_bits = diff_element_bits(f->kind, pb + off);
            }
            found += 1u;
        }
    }
    return found;
}
