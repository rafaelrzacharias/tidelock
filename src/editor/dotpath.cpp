// dotpath.h - tokenizer + three-form resolver, raw get/set over PathRef. Spec: docs/TOOLING.md
// §9.3.6.
#include "editor/dotpath.h"

#include "foundation/interner.h"
#include "core/commands.h"

#include <string.h>

namespace {

// One '.'-split token: [ptr, len).
struct Tok { const char* ptr; u32 len; };

// Splits `path` on '.' into up to `cap` tokens. Returns the count, or 0 if there are more than
// `cap` (this module's forms are 2 or 3 tokens - anything else is ERR_PATH_SYNTAX at the call
// site) or any token is empty (a leading/trailing/doubled '.').
u32 split_dots(StrView path, Tok* out, u32 cap) {
    u32 n = 0;
    u32 start = 0;
    for (u32 i = 0; i <= path.len; ++i) {
        if (i == path.len || path.ptr[i] == '.') {
            const u32 len = i - start;
            if (len == 0u) { return 0u; }
            if (n >= cap) { return 0u; }
            out[n].ptr = path.ptr + start;
            out[n].len = len;
            ++n;
            start = i + 1u;
        }
    }
    return n;
}

// Strict decimal (no sign - an entity index is never negative). Rejects "", "12x", non-digits.
bool parse_index(Tok t, u32* out) {
    if (t.len == 0u) { return false; }
    u64 v = 0;
    for (u32 i = 0; i < t.len; ++i) {
        const char c = t.ptr[i];
        if (c < '0' || c > '9') { return false; }
        if (v > 0xFFFFFFFFull) { return false; }
        v = v * 10u + (u64)(c - '0');
    }
    if (v > 0xFFFFFFFFull) { return false; }
    *out = (u32)v;
    return true;
}

// Splits a field token into its name and optional `[k]` suffix. `has_elem` false means no
// bracket was present (elem stays 0, matching a non-array field's only element). A malformed
// bracket (`bits[`, `bits[x]`, `bits[3`) is ERR_PATH_SYNTAX at the call site (`ok` false).
struct FieldTok { StrView name; u16 elem; bool has_elem; };
bool parse_field_token(Tok t, FieldTok* out) {
    u32 br = t.len;
    for (u32 i = 0; i < t.len; ++i) { if (t.ptr[i] == '[') { br = i; break; } }
    if (br == t.len) { out->name = StrView{ t.ptr, t.len }; out->elem = 0; out->has_elem = false; return true; }
    if (t.len < br + 2u || t.ptr[t.len - 1u] != ']') { return false; }
    out->name = StrView{ t.ptr, br };
    if (out->name.len == 0u) { return false; }
    Tok num{ t.ptr + br + 1u, t.len - br - 2u };
    u32 v;
    if (!parse_index(num, &v) || v > 0xFFFFu) { return false; }
    out->elem = (u16)v;
    out->has_elem = true;
    return true;
}

// Finds `field_name` (with `elem` already parsed) in `info`'s field table. ERR_PATH_NO_FIELD if
// unknown, or `elem` is out of `[0, field.count)`.
Result<u16> find_field(const ComponentInfo* info, StrView field_name, u16 elem) {
    const NameHash want = fnv1a64(field_name.ptr, field_name.len);
    for (u32 i = 0; i < info->field_count; ++i) {
        if (info->fields[i].name_hash == want) {
            if (elem >= info->fields[i].count) { return Result<u16>{ 0, ERR_PATH_NO_FIELD }; }
            return Result<u16>{ (u16)i, ERR_OK };
        }
    }
    return Result<u16>{ 0, ERR_PATH_NO_FIELD };
}

// Resolves an entity by its `Name` component's StrId (docs/TOOLING.md §9.3.6: "a name -
// ERR_PATH_NO_ENTITY if unknown or ambiguous"). Linear scan: this is a human-driven debug path,
// never a per-tick one, so an O(entities) scan needs no index.
Result<Entity> resolve_by_name(World* w, StrView name) {
    if (w->interner == nullptr) { return Result<Entity>{ Entity{ 0 }, ERR_PATH_NO_ENTITY }; }
    const u32 name_comp = world_find_component(w, "Name"_id);
    if (name_comp >= MAX_COMPONENT_TYPES) { return Result<Entity>{ Entity{ 0 }, ERR_PATH_NO_ENTITY }; }
    const ComponentTable* t = &w->comps[name_comp];
    if (t->info->flags & COMP_SINGLETON) { return Result<Entity>{ Entity{ 0 }, ERR_PATH_NO_ENTITY }; }

    // The first K_StrId field is the name (this module's documented assumption - a `Name`
    // component with no StrId field cannot answer a name lookup at all).
    u32 strid_offset = 0xFFFFFFFFu;
    for (u32 i = 0; i < t->info->field_count; ++i) {
        if (t->info->fields[i].kind == K_StrId) { strid_offset = t->info->fields[i].offset; break; }
    }
    if (strid_offset == 0xFFFFFFFFu) { return Result<Entity>{ Entity{ 0 }, ERR_PATH_NO_ENTITY }; }

    const StrId want = intern(w->interner, name);
    Entity found = Entity{ 0 };
    u32 matches = 0;
    for (u32 r = 0; r < t->count; ++r) {
        StrId sid;
        memcpy(&sid, t->dense + (u64)r * t->stride + strid_offset, sizeof(StrId));
        if (sid == want) { found = t->entities[r]; matches += 1u; }
    }
    if (matches != 1u) { return Result<Entity>{ Entity{ 0 }, ERR_PATH_NO_ENTITY }; }
    return Result<Entity>{ found, ERR_OK };
}

}  // namespace

Result<PathRef> dotpath_resolve(World* w, StrView path) {
    Tok toks[3];
    const u32 n = split_dots(path, toks, 3u);

    if (n >= 1u && toks[0].len > 0u && toks[0].ptr[0] == '@') {
        if (n != 2u) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }
        const StrView comp_name{ toks[0].ptr + 1, toks[0].len - 1u };
        if (comp_name.len == 0u) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }
        const u32 comp = world_find_component(w, fnv1a64(comp_name.ptr, comp_name.len));
        if (comp >= MAX_COMPONENT_TYPES) { return Result<PathRef>{ PathRef{}, ERR_PATH_NO_COMPONENT }; }
        if ((w->comps[comp].info->flags & COMP_SINGLETON) == 0u) { return Result<PathRef>{ PathRef{}, ERR_PATH_NO_COMPONENT }; }

        FieldTok ft;
        if (!parse_field_token(toks[1], &ft)) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }
        Result<u16> fi = find_field(w->comps[comp].info, ft.name, ft.elem);
        if (fi.err != ERR_OK) { return Result<PathRef>{ PathRef{}, fi.err }; }
        return Result<PathRef>{ PathRef{ Entity{ 0 }, (ComponentId)comp, fi.value, ft.elem }, ERR_OK };
    }

    if (n != 3u) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }

    Entity e;
    if (toks[0].len > 0u && toks[0].ptr[0] == '#') {
        u32 idx;
        if (!parse_index(Tok{ toks[0].ptr + 1, toks[0].len - 1u }, &idx)) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }
        if (idx >= w->entities.slots.count) { return Result<PathRef>{ PathRef{}, ERR_PATH_NO_ENTITY }; }
        const u32 gen = w->entities.gen.data[idx];   // "any generation typed -> current" (docs/TOOLING.md §9.3.6)
        e = handle_make<Entity>(idx, gen);
    } else {
        Result<Entity> re = resolve_by_name(w, StrView{ toks[0].ptr, toks[0].len });
        if (re.err != ERR_OK) { return Result<PathRef>{ PathRef{}, re.err }; }
        e = re.value;
    }

    const StrView comp_name{ toks[1].ptr, toks[1].len };
    const u32 comp = world_find_component(w, fnv1a64(comp_name.ptr, comp_name.len));
    if (comp >= MAX_COMPONENT_TYPES) { return Result<PathRef>{ PathRef{}, ERR_PATH_NO_COMPONENT }; }
    if (w->comps[comp].info->flags & COMP_SINGLETON) { return Result<PathRef>{ PathRef{}, ERR_PATH_NO_COMPONENT }; }

    FieldTok ft;
    if (!parse_field_token(toks[2], &ft)) { return Result<PathRef>{ PathRef{}, ERR_PATH_SYNTAX }; }
    Result<u16> fi = find_field(w->comps[comp].info, ft.name, ft.elem);
    if (fi.err != ERR_OK) { return Result<PathRef>{ PathRef{}, fi.err }; }
    return Result<PathRef>{ PathRef{ e, (ComponentId)comp, fi.value, ft.elem }, ERR_OK };
}

Result<u32> dotpath_get_raw(World* w, PathRef ref, void* out, u32 out_cap) {
    const ComponentTable* t = &w->comps[ref.comp];
    const FieldInfo* f = &t->info->fields[ref.field];
    const u32 esz = f->size / (u32)f->count;
    TL_CHECK(out_cap >= esz);

    const u8* row;
    if (t->info->flags & COMP_SINGLETON) {
        row = t->dense;
    } else {
        // column_get's own queryable-absence idiom (docs/CONTAINERS.md §8.2) already covers a
        // dead or stale ref.e - null either way, no separate world_entity_alive check needed.
        row = (const u8*)column_get((ComponentTable*)t, ref.e);
        if (row == nullptr) { return Result<u32>{ 0, ERR_PATH_NO_ENTITY }; }
    }
    memcpy(out, row + f->offset + (u64)ref.elem * esz, esz);
    return Result<u32>{ esz, ERR_OK };
}

ErrCode dotpath_set_raw(World* w, PathRef ref, bool lockstep, const void* bytes, u32 len) {
    if (lockstep) { return ERR_PATH_LOCKSTEP; }
    const ComponentTable* t = &w->comps[ref.comp];
    const FieldInfo* f = &t->info->fields[ref.field];
    const u32 esz = f->size / (u32)f->count;
    TL_CHECK(len == esz);
    // world_set_field_cmd's payload is the WHOLE field's bytes at its own offset; an array
    // element write still records field_index = ref.field with len == esz only when count == 1
    // (a non-array field). An array field's single-element write needs a byte offset the
    // existing CMD_SET_FIELD payload shape has no room for (docs/ECS.md §10.5's payload is
    // "u32 field index" + the field's full bytes) - so this door only writes non-array fields
    // for now; TODO.md carries the array-element write gap as a follow-up (a real CMD_SET_FIELD
    // shape change is out of this lane's cone, ecs is merged/closed).
    TL_CHECK(f->count == 1u);
    world_set_field_cmd(w, ref.e, ref.comp, ref.field, bytes, len);
    return ERR_OK;
}
