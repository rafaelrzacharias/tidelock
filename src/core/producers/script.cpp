// script.cpp - the Script InputProducer (docs/INPUT.md §9.4). See script.h for the contract.
#include "core/producers/script.h"
#include <string.h>

void script_producer_init(ScriptProducer* sp, VMemArena* arena, u32 max_events, u8 live_mask) {
    memset(sp, 0, sizeof(ScriptProducer));
    array_init_fixed(&sp->events, arena, max_events);
    sp->live_mask = live_mask;
}

void script_add_event(ScriptProducer* sp, u64 tick, ActionId action, i8 value, ScriptOp op, u8 slot) {
    TL_CHECK(slot < MAX_PEERS);
    TL_CHECK(action < MAX_ACTIONS);
    TL_CHECK(sp->events.count == 0u || array_at(&sp->events, sp->events.count - 1u).tick <= tick);
    ScriptedEvent ev{};
    ev.tick = tick;
    ev.action = action;
    ev.value = value;
    ev.op = (u8)op;
    ev.slot = slot;
    array_push(&sp->events, ev);
}

void script_press(ScriptProducer* sp, ActionId action, u64 tick, i8 value, u8 slot) {
    script_add_event(sp, tick, action, value, SCRIPT_OP_PRESS, slot);
}

void script_hold(ScriptProducer* sp, ActionId action, i8 value, u64 from, u64 to, u8 slot) {
    TL_CHECK(from <= to);
    script_add_event(sp, from, action, value, SCRIPT_OP_HOLD_START, slot);
    script_add_event(sp, to, action, 0, SCRIPT_OP_HOLD_END, slot);
}

void script_set(ScriptProducer* sp, ActionId action, i8 value, u64 tick, u8 slot) {
    script_add_event(sp, tick, action, value, SCRIPT_OP_SET, slot);
}

ProduceResult script_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask) {
    ScriptProducer* sp = (ScriptProducer*)ctx;

    u8 prev_down[MAX_PEERS][MAX_ACTIONS];
    memcpy(prev_down, sp->down, sizeof(prev_down));

    // Auto-clear PRESS pulses scheduled by the previous produce() call, before this tick's own
    // events apply (docs/INPUT.md §9.4's op vocabulary comment - one-tick pulse semantics).
    for (u32 s = 0; s < MAX_PEERS; ++s) {
        for (u32 a = 0; a < MAX_ACTIONS; ++a) {
            if (sp->pending_release[s][a] != 0u) {
                sp->down[s][a] = 0u;
                sp->value[s][a] = 0;
                sp->pending_release[s][a] = 0u;
            }
        }
    }

    while (sp->cursor < sp->events.count) {
        const ScriptedEvent& ev = array_at(&sp->events, sp->cursor);
        if (ev.tick != tick) { break; }
        switch ((ScriptOp)ev.op) {
            case SCRIPT_OP_SET:
                sp->down[ev.slot][ev.action] = (ev.value != 0) ? 1u : 0u;
                sp->value[ev.slot][ev.action] = ev.value;
                break;
            case SCRIPT_OP_PRESS:
                sp->down[ev.slot][ev.action] = 1u;
                sp->value[ev.slot][ev.action] = ev.value;
                sp->pending_release[ev.slot][ev.action] = 1u;
                break;
            case SCRIPT_OP_HOLD_START:
                sp->down[ev.slot][ev.action] = 1u;
                sp->value[ev.slot][ev.action] = ev.value;
                break;
            case SCRIPT_OP_HOLD_END:
                sp->down[ev.slot][ev.action] = 0u;
                sp->value[ev.slot][ev.action] = 0;
                break;
        }
        sp->cursor += 1u;
    }

    for (u32 s = 0; s < MAX_PEERS; ++s) {
        InputFrame f = input_zero_frame();
        f.tick = (u32)tick;
        for (u32 a = 0; a < MAX_ACTIONS; ++a) {
            const bool down_now = sp->down[s][a] != 0u;
            const bool down_prev = prev_down[s][a] != 0u;
            const u8 flags = (u8)((down_now ? AS_DOWN : 0u)
                                | ((down_now && !down_prev) ? AS_PRESSED : 0u)
                                | ((!down_now && down_prev) ? AS_RELEASED : 0u));
            f.actions[a].value = sp->value[s][a];
            f.actions[a].flags = flags;
        }
        out[s] = f;
    }
    *live_mask = sp->live_mask;
    return PRODUCE_READY;
}
