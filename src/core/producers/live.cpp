// live.cpp - the Live InputProducer's fold_tick (docs/INPUT.md §9.3). See live.h for the contract
// and this lane's recorded deviations (axis-selector reuse, DZ_RADIAL, the pointer passthrough).
#include "core/producers/live.h"
#include "foundation/fx_float.h"
#include <string.h>

namespace {

constexpr f32 LIVE_MOUSE_AXIS_PIXELS_PER_UNIT = 32.0f;

u32 popcount8(u8 x) {
    u32 n = 0u;
    while (x != 0u) { n += (u32)(x & 1u); x = (u8)(x >> 1); }
    return n;
}

f32 clamp_f32(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// docs/INPUT.md §2: axial/radial/trigger deadzone, applied before sensitivity. DZ_RADIAL is
// applied per-axis here (a recorded simplification - see live.h's contract block).
f32 apply_deadzone(Deadzone dz, f32 v, f32 radius) {
    const f32 r = clamp_f32(radius, 0.0f, 0.999f);
    switch (dz) {
        case DZ_NONE: return v;
        case DZ_AXIAL:
        case DZ_RADIAL: {
            const f32 av = v < 0.0f ? -v : v;
            if (av < r) { return 0.0f; }
            const f32 sign = v < 0.0f ? -1.0f : 1.0f;
            return sign * clamp_f32((av - r) / (1.0f - r), 0.0f, 1.0f);
        }
        case DZ_TRIGGER: {
            if (v < r) { return 0.0f; }
            return clamp_f32((v - r) / (1.0f - r), 0.0f, 1.0f);
        }
    }
    return v;
}

bool is_mouse_event(u8 kind) { return kind == EV_MOUSE_MOVE || kind == EV_MOUSE_BUTTON || kind == EV_WHEEL; }
bool is_key_event(u8 kind)   { return kind == EV_KEY || kind == EV_TEXT; }

}  // namespace

void live_producer_init(LiveProducer* lp, ActionMap* map, u8 local_slot, ImGuiCaptureApi capture) {
    TL_CHECK(map->bindings.cap <= LIVE_MAX_BINDINGS);
    memset(lp, 0, sizeof(LiveProducer));
    lp->map = map;
    lp->local_slot = local_slot;
    lp->capture = capture;
}

ProduceResult live_produce_frame(LiveProducer* lp, const RawEvent* events, u32 event_count,
                                 u64 tick, InputFrame* out, u8* live_mask) {
    lp->mouse_dx_accum = 0.0f;
    lp->mouse_dy_accum = 0.0f;

    u8 want_mouse = 0u, want_keyboard = 0u;
    if (lp->capture.ctx != nullptr) { lp->capture.want_capture(lp->capture.ctx, &want_mouse, &want_keyboard); }

    ActionMap* m = lp->map;
    const u32 binding_count = m->bindings.count;
    TL_CHECK(binding_count <= LIVE_MAX_BINDINGS);

    for (u32 i = 0; i < event_count; ++i) {
        const RawEvent& ev = events[i];
        if (want_mouse != 0u && is_mouse_event(ev.kind)) { continue; }
        if (want_keyboard != 0u && is_key_event(ev.kind)) { continue; }
        switch (ev.kind) {
            case EV_KEY: {
                const u32 sc = ev.u.key.scancode;
                if (sc < LIVE_MAX_KEYS) {
                    const bool was_down = lp->key_down[sc] != 0u;
                    lp->key_down[sc] = ev.u.key.down;
                    // SOCD "most recent press wins" is an ARRIVAL-ORDER fact (docs/INPUT.md §2):
                    // update DEV_KEYS_AXIS SOCD state inline, in event order, rather than from a
                    // before/after-tick snapshot comparison, which cannot see which of two keys
                    // pressed in the SAME tick came first.
                    if (ev.u.key.down != 0u && !was_down) {
                        for (u32 bi = 0; bi < binding_count; ++bi) {
                            const Binding& bb = array_at(&m->bindings, bi);
                            if (bb.dev != DEV_KEYS_AXIS) { continue; }
                            if (bb.socd == SOCD_LAST_WINS) {
                                if (bb.code_pos == sc) { lp->socd[bi].last_dir = 1; }
                                else if (bb.code_neg == sc) { lp->socd[bi].last_dir = -1; }
                            } else if (bb.socd == SOCD_FIRST_WINS && lp->socd[bi].last_dir == 0) {
                                if (bb.code_pos == sc) { lp->socd[bi].last_dir = 1; }
                                else if (bb.code_neg == sc) { lp->socd[bi].last_dir = -1; }
                            }
                        }
                    }
                }
                lp->mods = ev.u.key.mods;
                break;
            }
            case EV_MOUSE_BUTTON: {
                if ((u32)ev.u.mouse_button.button < LIVE_MAX_MOUSE_BUTTONS) {
                    lp->mouse_button_down[ev.u.mouse_button.button] = ev.u.mouse_button.down;
                }
                lp->mouse_x = ev.u.mouse_button.x;
                lp->mouse_y = ev.u.mouse_button.y;
                break;
            }
            case EV_MOUSE_MOVE: {
                lp->mouse_x = ev.u.mouse_move.x;
                lp->mouse_y = ev.u.mouse_move.y;
                lp->mouse_dx_accum += (f32)ev.u.mouse_move.dx;
                lp->mouse_dy_accum += (f32)ev.u.mouse_move.dy;
                break;
            }
            case EV_PAD_AXIS: {
                if ((u32)ev.u.pad_axis.pad < LIVE_MAX_PADS && (u32)ev.u.pad_axis.axis < LIVE_MAX_PAD_AXES) {
                    lp->pad_axis_raw[ev.u.pad_axis.pad][ev.u.pad_axis.axis] = ev.u.pad_axis.value;
                }
                break;
            }
            case EV_PAD_BUTTON: {
                if ((u32)ev.u.pad_button.pad < LIVE_MAX_PADS && (u32)ev.u.pad_button.button < LIVE_MAX_PAD_BUTTONS) {
                    lp->pad_button_down[ev.u.pad_button.pad][ev.u.pad_button.button] = ev.u.pad_button.down;
                }
                break;
            }
            case EV_PAD_CONNECT: {
                if ((u32)ev.u.pad_connect.pad < LIVE_MAX_PADS) {
                    const u32 pad = ev.u.pad_connect.pad;
                    lp->pad_connected[pad] = ev.u.pad_connect.connected;
                    if (ev.u.pad_connect.connected == 0u) {
                        memset(lp->pad_axis_raw[pad], 0, sizeof(lp->pad_axis_raw[pad]));
                        memset(lp->pad_button_down[pad], 0, sizeof(lp->pad_button_down[pad]));
                    }
                }
                break;
            }
            default: break;   // text/window events ignored (docs/INPUT.md section 9.3)
        }
    }

    // Chord specificity (docs/INPUT.md §2/§9.3): a physically-satisfied DIGITAL-family binding is
    // suppressed if another satisfied binding shares its physical (dev, code) with a strictly
    // higher modifier-bit count.
    u8 phys_ok[LIVE_MAX_BINDINGS];
    u32 phys_spec[LIVE_MAX_BINDINGS];
    for (u32 i = 0; i < binding_count; ++i) {
        const Binding& b = array_at(&m->bindings, i);
        phys_ok[i] = 0u;
        phys_spec[i] = 0u;
        if (b.context != m->active_context) { continue; }
        bool physically_down = false;
        if (b.dev == DEV_KEY) {
            physically_down = b.code_pos < LIVE_MAX_KEYS && lp->key_down[b.code_pos] != 0u;
        } else if (b.dev == DEV_MOUSE_BUTTON) {
            physically_down = b.code_pos < LIVE_MAX_MOUSE_BUTTONS && lp->mouse_button_down[b.code_pos] != 0u;
        } else if (b.dev == DEV_PAD_BUTTON) {
            physically_down = b.code_neg < LIVE_MAX_PADS && b.code_pos < LIVE_MAX_PAD_BUTTONS
                            && lp->pad_button_down[b.code_neg][b.code_pos] != 0u;
        } else {
            continue;   // not a digital-family binding
        }
        if (!physically_down) { continue; }
        if ((lp->mods & b.modifiers) != b.modifiers) { continue; }   // required modifiers not all held
        phys_ok[i] = 1u;
        phys_spec[i] = popcount8(b.modifiers);
    }
    u8 active[LIVE_MAX_BINDINGS];
    for (u32 i = 0; i < binding_count; ++i) {
        active[i] = phys_ok[i];
        if (phys_ok[i] == 0u) { continue; }
        const Binding& bi = array_at(&m->bindings, i);
        for (u32 j = 0; j < binding_count; ++j) {
            if (j == i || phys_ok[j] == 0u) { continue; }
            const Binding& bj = array_at(&m->bindings, j);
            if (bj.dev != bi.dev) { continue; }
            const bool same_physical = (bi.dev == DEV_PAD_BUTTON)
                ? (bj.code_neg == bi.code_neg && bj.code_pos == bi.code_pos)
                : (bj.code_pos == bi.code_pos);
            if (same_physical && phys_spec[j] > phys_spec[i]) { active[i] = 0u; break; }
        }
    }

    InputFrame frame = input_zero_frame();
    frame.tick = (u32)tick;

    for (u32 a = 0; a < m->action_count; ++a) {
        const Action& act = m->actions[a];
        bool down = false;
        f32 raw = 0.0f;
        for (u32 i = 0; i < binding_count; ++i) {
            const Binding& b = array_at(&m->bindings, i);
            if (b.action != a || b.context != m->active_context) { continue; }
            if (act.kind == ACT_DIGITAL) {
                if ((b.dev == DEV_KEY || b.dev == DEV_MOUSE_BUTTON || b.dev == DEV_PAD_BUTTON) && active[i] != 0u) {
                    down = true;
                }
                continue;
            }
            // ACT_ANALOG
            if (b.dev == DEV_KEYS_AXIS) {
                const bool neg_down = b.code_neg < LIVE_MAX_KEYS && lp->key_down[b.code_neg] != 0u;
                const bool pos_down = b.code_pos < LIVE_MAX_KEYS && lp->key_down[b.code_pos] != 0u;
                LiveSocdState& s = lp->socd[i];
                i8 dir;
                if (b.socd == SOCD_NEUTRAL) {
                    dir = (pos_down && neg_down) ? (i8)0 : (pos_down ? (i8)1 : (neg_down ? (i8)-1 : (i8)0));
                } else {
                    // SOCD_LAST_WINS/SOCD_FIRST_WINS: last_dir was set in ARRIVAL order by the
                    // event loop above (this header's contract block - a before/after-tick
                    // snapshot cannot see which of two same-tick presses came first). Here we
                    // only apply the release-driven fallback: if the currently-winning side
                    // released, fall back to the other side if it is still held, else neutral.
                    if (!pos_down && !neg_down) { s.last_dir = 0; }
                    else if (s.last_dir == 1 && !pos_down) { s.last_dir = neg_down ? (i8)-1 : (i8)0; }
                    else if (s.last_dir == -1 && !neg_down) { s.last_dir = pos_down ? (i8)1 : (i8)0; }
                    dir = s.last_dir;
                }
                raw += (f32)dir;
            } else if (b.dev == DEV_MOUSE_AXIS) {
                const f32 delta = (b.code_pos == 0u) ? lp->mouse_dx_accum : lp->mouse_dy_accum;
                const f32 v = clamp_f32(delta / LIVE_MOUSE_AXIS_PIXELS_PER_UNIT, -1.0f, 1.0f);
                raw += apply_deadzone(b.dz, v, b.dz_radius) * b.sensitivity;
            } else if (b.dev == DEV_PAD_AXIS) {
                const u32 pad = b.code_neg, axis = b.code_pos;
                const i16 raw16 = (pad < LIVE_MAX_PADS && axis < LIVE_MAX_PAD_AXES) ? lp->pad_axis_raw[pad][axis] : (i16)0;
                const f32 v = (f32)raw16 / 32768.0f;
                raw += apply_deadzone(b.dz, v, b.dz_radius) * b.sensitivity;
            }
        }

        i8 value;
        bool down_for_edge;
        if (act.kind == ACT_DIGITAL) {
            value = (i8)(down ? 1 : 0);
            down_for_edge = down;
        } else {
            raw = clamp_f32(raw, -1.0f, 1.0f);
            value = (i8)fx::fx_rint_f32(raw * 127.0f);
            down_for_edge = value != 0;
        }

        const bool prev = lp->prev_action_down[a] != 0u;
        const u8 flags = (u8)((down_for_edge ? AS_DOWN : 0u)
                            | ((down_for_edge && !prev) ? AS_PRESSED : 0u)
                            | ((!down_for_edge && prev) ? AS_RELEASED : 0u));
        lp->prev_action_down[a] = down_for_edge ? 1u : 0u;

        frame.actions[a].value = value;
        frame.actions[a].flags = flags;
    }

    // Pointer: an IDENTITY passthrough (window px as world-space units) until render2d's camera
    // exists (see this file's header contract).
    const pos_t px = fx::from_f32_quantized<pos_t>((f32)lp->mouse_x);
    const pos_t py = fx::from_f32_quantized<pos_t>((f32)lp->mouse_y);
    frame.pointer_x = px.v;
    frame.pointer_y = py.v;

    out[lp->local_slot] = frame;
    *live_mask = (u8)(1u << lp->local_slot);
    return PRODUCE_READY;
}

ProduceResult live_produce(void* ctx, u64 tick, InputFrame* out, u8* live_mask) {
    LiveProducerCtx* lc = (LiveProducerCtx*)ctx;
    RawEvent drained[LIVE_MAX_EVENTS_PER_TICK];
    u32 n = 0u;
    while (n < LIVE_MAX_EVENTS_PER_TICK && !ring_empty(lc->ring)) {
        drained[n] = ring_pop(lc->ring);
        n += 1u;
    }
    // A ring holding more than LIVE_MAX_EVENTS_PER_TICK between two produce() calls would leave
    // events undrained here; engine_frame's ring is sized to LIVE_MAX_EVENTS_PER_TICK precisely
    // so that never happens in practice (docs/PLATFORM.md's default event_ring_cap_log2 matches).
    return live_produce_frame(lc->lp, drained, n, tick, out, live_mask);
}
