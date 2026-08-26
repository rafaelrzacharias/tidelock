// ---------------------------------------------------------------------------------------------
// encode.cpp - the InputFrame column encoder/decoder.
//
// Spec: docs/NETCODE.md §20.2.2 (the column byte layout), §20.3(a) (the algorithm and the
//   decoder's refusals), docs/INPUT.md §1 (the frame). Declarations: net/wire.h.
// Determinism: every operation is an integer identity, so encode -> decode is lossless by
//   construction and the bytes are identical on every target in the docs/CANON.md matrix. No
//   floats, no allocation, no wall clock, no global state - a column is a pure function of the
//   frames handed in.
// ---------------------------------------------------------------------------------------------
#include "net/wire.h"

// The column's pointer coding is a SECOND difference (docs/NETCODE.md §20.2.2):
//   v_i  = p_i - p_{i-1}      (p_{-1} = 0)
//   dv_i = v_i - v_{i-1}      (v_{-1} = 0)
// so a pointer held still costs 1 byte per axis per frame and one moving at a constant rate
// costs the same. Both differences wrap on i32 rather than trapping: the encoder writes a
// difference that may overflow, and the decoder's additions must reproduce the same bits
// (docs/NETCODE.md §20.2.2 - "decoder arithmetic is wrap_add on i32").

void encode_column(ByteWriter* w, const WireFrame* frames, u32 n) {
    TL_ASSERT(w != nullptr);
    TL_ASSERT(frames != nullptr || n == 0u);

    WireFrame prev = wire_zero_frame();
    i32 vx_prev = 0;
    i32 vy_prev = 0;

    for (u32 i = 0; i < n; ++i) {
        const WireFrame* f = &frames[i];

        // `changed` bit a is set iff the whole ActionState differs - value OR flags. Comparing
        // the pair rather than the value is what makes an edge flag (pressed/released) with an
        // unchanged value cross the wire at all.
        u32 changed = 0u;
        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            const bool differs = f->actions[a].value != prev.actions[a].value
                              || f->actions[a].flags != prev.actions[a].flags;
            if (differs) { changed |= (1u << a); }
        }
        wire_put_uvarint(w, changed);

        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            if (((changed >> a) & 1u) == 0u) { continue; }
            const i8 value = f->actions[a].value;
            const u8 flags = f->actions[a].flags;
            // Only the three docs/INPUT.md §1 bits exist on the wire. TL_CHECK, not TL_ASSERT:
            // off TL_DEV the assert is ((void)0), and `flags & 7` below would then silently drop
            // a higher bit - the sender applies its own frame and every receiver applies the
            // truncated one, which is a silent desync and breaks §12.2's promise that "what a
            // peer sends is exactly what every peer - including itself - applies". One branch
            // per CHANGED action, not per action.
            TL_CHECK((flags & (u8)~WIRE_FLAG_BITS) == 0u);
            const bool value_follows = (value != wire_implied_value(flags));
            const u8 rec = (u8)((flags & WIRE_FLAG_BITS)
                              | (value_follows ? WIRE_REC_VALUE_FOLLOWS : 0u));
            bw_put_u8(w, rec);
            if (value_follows) { bw_put_u8(w, (u8)value); }
        }

        const i32 vx = wire_wrap_sub_i32(f->pointer_x, prev.pointer_x);
        const i32 vy = wire_wrap_sub_i32(f->pointer_y, prev.pointer_y);
        wire_put_svarint(w, wire_wrap_sub_i32(vx, vx_prev));
        wire_put_svarint(w, wire_wrap_sub_i32(vy, vy_prev));

        prev = *f;
        vx_prev = vx;
        vy_prev = vy;
    }
}

ErrCode decode_column(ByteReader* r, WireFrame* out, u32 frame_count, u64 base_tick) {
    TL_ASSERT(r != nullptr);
    TL_ASSERT(out != nullptr || frame_count == 0u);

    // Zero the whole output first: an error return must never leave a caller looking at bytes
    // from a previous decode (the same discipline reflect.h's row reader keeps).
    for (u32 i = 0; i < frame_count; ++i) { out[i] = wire_zero_frame(); }

    WireFrame prev = wire_zero_frame();
    i32 vx_prev = 0;
    i32 vy_prev = 0;

    for (u32 i = 0; i < frame_count; ++i) {
        WireFrame f = prev;   // unchanged actions carry forward; the pointer is overwritten below

        u32 changed = 0u;
        const ErrCode ce = wire_get_uvarint(r, &changed);
        if (ce != ERR_OK) { return ce; }
        // A set bit at or past MAX_ACTIONS names an action this build has no room for. Spelled
        // as a guarded shift because `u32 >> 32` is undefined, and MAX_ACTIONS is 32 today.
        if (NET_FRAME_MAX_ACTIONS < 32u) {
            if ((changed >> (NET_FRAME_MAX_ACTIONS & 31u)) != 0u) { return ERR_NET_MALFORMED; }
        }

        for (u32 a = 0; a < NET_FRAME_MAX_ACTIONS; ++a) {
            if (((changed >> a) & 1u) == 0u) { continue; }
            const u8 rec = br_get_u8(r);
            if (!br_ok(r)) { return ERR_BYTES_TRUNCATED; }
            // Bits 4..7 are reserved and must be zero (docs/NETCODE.md §20.3(a)). Refusing them
            // is what keeps a future format's rec byte from being silently misread as this one's.
            if ((rec & WIRE_REC_RESERVED_MASK) != 0u) { return ERR_NET_MALFORMED; }
            const u8 flags = (u8)(rec & WIRE_FLAG_BITS);
            i8 value;
            if ((rec & WIRE_REC_VALUE_FOLLOWS) != 0u) {
                const u8 raw = br_get_u8(r);
                if (!br_ok(r)) { return ERR_BYTES_TRUNCATED; }
                value = (i8)raw;
                // Canonical form (wire.h): the encoder sets value_follows only when the value
                // DIFFERS from the one the flags imply, so a value byte carrying the implied
                // value is a second encoding of the same frame. Refused, because the archive's
                // bytes are hashed into the chain (docs/NETCODE.md §20.2.8) and two encodings of
                // one frame would fork it.
                if (value == wire_implied_value(flags)) { return ERR_NET_MALFORMED; }
            } else {
                value = wire_implied_value(flags);
            }
            // docs/NETCODE.md §20.2.2 states the rule as a BICONDITIONAL: "bit a set <=>
            // actions[a] != prev.actions[a]". A set bit that decodes to the state the action
            // already had is therefore not a valid stream - and, left accepted, it is a second
            // encoding of the same frame set, which the chain's hash over archive bytes cannot
            // tolerate (docs/NETCODE.md §20.2.8). Found by T1f: a mutation that clears a
            // `changed` bit produces a stream that still decodes, consumes fewer bytes, and
            // re-encodes differently.
            if (value == prev.actions[a].value && flags == prev.actions[a].flags) {
                return ERR_NET_MALFORMED;
            }
            f.actions[a].value = value;
            f.actions[a].flags = flags;
        }

        i32 dvx = 0;
        i32 dvy = 0;
        const ErrCode ex = wire_get_svarint(r, &dvx);
        if (ex != ERR_OK) { return ex; }
        const ErrCode ey = wire_get_svarint(r, &dvy);
        if (ey != ERR_OK) { return ey; }

        const i32 vx = wire_wrap_add_i32(vx_prev, dvx);
        const i32 vy = wire_wrap_add_i32(vy_prev, dvy);
        f.pointer_x = wire_wrap_add_i32(prev.pointer_x, vx);
        f.pointer_y = wire_wrap_add_i32(prev.pointer_y, vy);

        // The tick is derived, never transmitted (docs/NETCODE.md §20.2.2).
        f.tick = (u32)(base_tick + (u64)i);

        out[i] = f;
        prev = f;
        vx_prev = vx;
        vy_prev = vy;
    }
    return ERR_OK;
}
