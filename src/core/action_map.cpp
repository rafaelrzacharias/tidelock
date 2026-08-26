// action_map.cpp - Action/Binding registration, lookup, context, fingerprint.
// See action_map.h for the contract.
#include "core/action_map.h"
#include "foundation/bytes.h"
#include <string.h>

void action_map_init(ActionMap* m, VMemArena* arena, u32 max_bindings) {
    memset(m, 0, sizeof(ActionMap));
    array_init_fixed(&m->bindings, arena, max_bindings);
    m->active_context = CONTEXT_DEFAULT;
}

ActionId action_register(ActionMap* m, NameHash name, StrId sid, ActionKind kind, ActionClass cls) {
    if (m->action_count >= MAX_ACTIONS) { TL_FATAL("action_register: MAX_ACTIONS exceeded"); }
    for (u32 i = 0; i < m->action_count; ++i) {
        if (m->actions[i].name == name) { TL_FATAL("action_register: duplicate action name"); }
    }
    const ActionId id = (ActionId)m->action_count;
    Action a{};
    a.name = name;
    a.sid = sid;
    a.kind = kind;
    a.cls = cls;
    a._pad0 = 0u;
    m->actions[id] = a;
    m->action_count += 1u;
    return id;
}

ActionId action_find(const ActionMap* m, NameHash name) {
    for (u32 i = 0; i < m->action_count; ++i) {
        if (m->actions[i].name == name) { return (ActionId)i; }
    }
    return ACTION_ID_NONE;
}

void action_bind(ActionMap* m, const Binding& b) {
    TL_CHECK(b.action < m->action_count);
    array_push(&m->bindings, b);
}

void action_map_set_context(ActionMap* m, u8 context) {
    m->active_context = context;
}

u64 action_map_fingerprint(const ActionMap* m) {
    u64 h = TL_HASH_SEED;
    for (u32 i = 0; i < m->action_count; ++i) {
        u8 buf[24];
        ByteWriter w;
        bw_init(&w, buf, sizeof(buf));
        bw_put_u64(&w, m->actions[i].name);
        bw_put_u64(&w, (u64)m->actions[i].kind);
        bw_put_u64(&w, (u64)m->actions[i].cls);
        h = tl_hash64(buf, w.len, h);
    }
    return h;
}
