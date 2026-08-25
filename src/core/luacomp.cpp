// luacomp.cpp - the Luau-declared component/event packer (docs/ECS.md §6.1/§10.7,
// docs/LUAU-LAYER.md §10.6). Contracts in world.h (the two world_register_*_luau doors).
// The packer is pure C++: its input is the ordered (name, kind, count, default) list the
// `ecs.component`/`ecs.event` bindings decode from the script (the luau-bindings lane, W3);
// the script is the same bytecode on every peer, so every peer builds the same table.
#include "core/world.h"
#include "foundation/interner.h"
#include <string.h>

namespace {

// Copies a name onto the meta arena with a terminating NUL (FieldInfo.name is a C string).
const char* lc_copy_name(VMemArena* meta, StrView s) {
    char* p = (char*)arena_push(meta, (u64)s.len + 1u, 1u);
    if (s.len != 0u) { memcpy(p, s.ptr, s.len); }
    p[s.len] = '\0';
    return p;
}

// Builds "_padN" on the meta arena (N < 1000 by construction: at most one pad per field + 1).
const char* lc_pad_name(VMemArena* meta, u32 n) {
    char buf[16];
    u32 len = 0;
    buf[len++] = '_'; buf[len++] = 'p'; buf[len++] = 'a'; buf[len++] = 'd';
    char digits[8];
    u32 d = 0;
    do { digits[d++] = (char)('0' + (n % 10u)); n /= 10u; } while (n != 0u);
    while (d != 0u) { buf[len++] = digits[--d]; }
    return lc_copy_name(meta, StrView{ buf, len });
}

// strlen for the copied pad names (spelled locally: <string.h>'s allowlist is
// memcpy/memset/memcmp/memmove only - docs/CPP-SUBSET.md §1).
u32 lc_name_len(const char* s) {
    u32 n = 0;
    while (s[n] != '\0') { n += 1u; }
    return n;
}

// Appends one explicit pad row (kind K_u8, `gap` bytes) - docs/LUAU-LAYER.md §10.6: every
// interior gap and the tail are explicit fields so sizeof == sum of field sizes holds and the
// row hashes as zeros.
void lc_emit_pad(VMemArena* meta, FieldInfo* rows, u32* row_count, u32* pad_counter,
                 u32 offset, u32 gap) {
    const char* name = lc_pad_name(meta, *pad_counter);
    *pad_counter += 1u;
    rows[*row_count] = FieldInfo{ name, fnv1a64(name, lc_name_len(name)), K_u8, 0,
                                  (u16)gap, offset, gap };
    *row_count += 1u;
}

// The packer + info builder (shared by the component and event doors). On success the returned
// info (and everything it points at) lives on the world's meta arena.
Result<const ComponentInfo*> lc_build(World* w, StrView name, const LuauFieldDecl* fields,
                                      u32 field_count, u32 flags) {
    Result<const ComponentInfo*> r;
    r.value = nullptr;
    r.err = ERR_OK;
    if (field_count == 0u || fields == nullptr || name.len == 0u
        || field_count > LUACOMP_MAX_FIELDS) {
        r.err = ERR_ECS_TABLE_FULL;
        return r;
    }
    for (u32 i = 0; i < field_count; ++i) {
        const LuauFieldDecl* fd = &fields[i];
        if (fd->kind >= K_COUNT) { r.err = ERR_ECS_BAD_KIND; return r; }
        if (fd->count == 0u || fd->count > 255u) { r.err = ERR_ECS_BAD_COUNT; return r; }
        if (fd->name.len == 0u) { r.err = ERR_ECS_BAD_NAME; return r; }
        // The _pad* namespace belongs to the synthesized pads: a user field there would
        // collide with a pad's name hash and be wire-zero-enforced (review 1 D1).
        if (fd->name.len >= 4u && fd->name.ptr[0] == '_' && fd->name.ptr[1] == 'p'
            && fd->name.ptr[2] == 'a' && fd->name.ptr[3] == 'd') {
            r.err = ERR_ECS_BAD_NAME;
            return r;
        }
        // Defaults are validated at the door, never truncated (review 1 D6): the bits must fit
        // the field's scalar width, and handle/StrId kinds have no default but null/0
        // (docs/LUAU-LAYER.md §10.6 - a fabricated handle is never a "default").
        if (fd->default_bits != 0u) {
            const u32 scalar = kind_scalar_size(fd->kind);
            if (scalar < 8u && (fd->default_bits >> (8u * scalar)) != 0u) {
                r.err = ERR_ECS_BAD_DEFAULT;
                return r;
            }
            if (fd->kind >= K_Entity && fd->kind <= K_Basin) { r.err = ERR_ECS_BAD_DEFAULT; return r; }
            if (fd->kind == K_StrId) { r.err = ERR_ECS_BAD_DEFAULT; return r; }
            // A bool holds {0, 1}: any other byte is an invalid representation the C++ twin of
            // a Luau-declared column would load as UB (review 2 R2).
            if (fd->kind == K_bool && fd->default_bits > 1u) { r.err = ERR_ECS_BAD_DEFAULT; return r; }
        }
        for (u32 j = 0; j < i; ++j) {   // the encoder keys fields by name hash - names are unique
            if (sv_eq(fd->name, fields[j].name)) { r.err = ERR_ECS_DUPLICATE_NAME; return r; }
        }
    }

    // Worst case: one interior pad per field + the tail pad.
    FieldInfo* rows = (FieldInfo*)arena_push(&w->meta, ((u64)field_count * 2u + 1u) * sizeof(FieldInfo),
                                             alignof(FieldInfo));
    u32 decl_row[LUACOMP_MAX_FIELDS];   // decl i -> its emitted row (structural, never name-sniffed - review 1 D1)
    u32 row_count = 0;
    u32 pad_counter = 0;
    u32 offset = 0;
    u32 max_align = 1;
    bool any_default = false;
    for (u32 i = 0; i < field_count; ++i) {
        const u32 align = kind_scalar_size(fields[i].kind);   // natural alignment == scalar size
        if (align > max_align) { max_align = align; }
        const u32 aligned = (offset + align - 1u) & ~(align - 1u);
        if (aligned != offset) { lc_emit_pad(&w->meta, rows, &row_count, &pad_counter, offset, aligned - offset); }
        offset = aligned;
        const char* fname = lc_copy_name(&w->meta, fields[i].name);
        decl_row[i] = row_count;
        rows[row_count] = FieldInfo{ fname, fnv1a64(fields[i].name.ptr, fields[i].name.len),
                                     fields[i].kind, 0, fields[i].count, offset,
                                     align * fields[i].count };
        row_count += 1u;
        offset += align * fields[i].count;
        if (fields[i].default_bits != 0u) { any_default = true; }
        if (w->interner != nullptr) { (void)intern(w->interner, fields[i].name); }
    }
    const u32 size = (offset + max_align - 1u) & ~(max_align - 1u);
    if (size != offset) { lc_emit_pad(&w->meta, rows, &row_count, &pad_counter, offset, size - offset); }

    // The default row (world.h: a full default instance; the default broadcasts to every array
    // element). Null when every default is 0 - zeroed memory already is the default then.
    const void* default_row = nullptr;
    if (any_default) {
        u8* def = (u8*)arena_push(&w->meta, size, max_align);
        memset(def, 0, size);
        for (u32 i = 0; i < field_count; ++i) {
            const FieldInfo* row = &rows[decl_row[i]];
            const u32 scalar = kind_scalar_size(fields[i].kind);
            for (u32 e = 0; e < fields[i].count; ++e) {
                u8* p = def + row->offset + (u64)e * scalar;
                const u64 bits = fields[i].default_bits;
                switch (scalar) {
                    case 1: { u8 v = (u8)bits;   memcpy(p, &v, 1); break; }
                    case 2: { u16 v = (u16)bits; memcpy(p, &v, 2); break; }
                    case 4: { u32 v = (u32)bits; memcpy(p, &v, 4); break; }
                    default: { memcpy(p, &bits, 8); break; }
                }
            }
        }
        default_row = def;
    }

    ComponentInfo* info = (ComponentInfo*)arena_push(&w->meta, sizeof(ComponentInfo), alignof(ComponentInfo));
    info->name = lc_copy_name(&w->meta, name);
    info->name_hash = fnv1a64(name.ptr, name.len);
    info->size = size;
    info->align = max_align;
    info->fields = rows;
    info->field_count = row_count;
    info->flags = flags;
    info->default_row = default_row;
    if (w->interner != nullptr) { (void)intern(w->interner, name); }
    r.value = info;
    return r;
}

}  // namespace

Result<ComponentId> world_register_component_luau(World* w, StrView name,
                                                  const LuauFieldDecl* fields, u32 field_count,
                                                  u32 flags) {
    Result<ComponentId> r;
    r.value = 0;
    r.err = ERR_OK;
    if (w->sealed != 0u) { r.err = ERR_ECS_SEALED; return r; }
    if (w->comp_count == MAX_COMPONENT_TYPES) { r.err = ERR_ECS_TABLE_FULL; return r; }
    if (world_find_component(w, fnv1a64(name.ptr, name.len)) != MAX_COMPONENT_TYPES) {
        r.err = ERR_ECS_DUPLICATE_NAME;
        return r;
    }
    const u64 mark = arena_mark(&w->meta);
    Result<const ComponentInfo*> built = lc_build(w, name, fields, field_count, flags);
    if (built.err != ERR_OK) {
        arena_reset_to(&w->meta, mark);   // a rejected declaration leaves no meta residue
        r.err = built.err;
        return r;
    }
    r.value = world_register_component(w, built.value);
    return r;
}

Result<EventTypeId> world_register_event_luau(World* w, StrView name,
                                              const LuauFieldDecl* fields, u32 field_count,
                                              u32 cap) {
    Result<EventTypeId> r;
    r.value = 0;
    r.err = ERR_OK;
    if (w->sealed != 0u) { r.err = ERR_ECS_SEALED; return r; }
    if (w->events.count == MAX_EVENT_TYPES) { r.err = ERR_ECS_TABLE_FULL; return r; }
    if (world_find_event(w, fnv1a64(name.ptr, name.len)) != MAX_EVENT_TYPES) {
        r.err = ERR_ECS_DUPLICATE_NAME;
        return r;
    }
    const u64 mark = arena_mark(&w->meta);
    Result<const ComponentInfo*> built = lc_build(w, name, fields, field_count, 0u);
    if (built.err != ERR_OK) {
        arena_reset_to(&w->meta, mark);
        r.err = built.err;
        return r;
    }
    r.value = world_register_event(w, built.value, cap);
    return r;
}
