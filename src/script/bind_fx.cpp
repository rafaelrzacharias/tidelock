// bind_fx.cpp - the `fx` binding table: the only arithmetic a sim script may do on a palette row.
// Contract: script/vm.h (script_bind_fx).
// Spec: docs/LUAU-LAYER.md §10.3 (the signature table, verbatim), §2 (numbers and fx);
//   docs/FX-PALETTE.md §3/§3.1 (the rows and the closed mixed-op table); docs/CANON.md
//   "The fx palette" owns the values.
//
// The load-bearing rule: Luau NEVER sees a scaled double. Every fx value crosses as the raw i32
// bits of its row, carried in a Luau number, and the ROW is fixed by the binding's signature -
// never by the value. `+`/`-`/`<`/`==` on raw bits of one row are exact in script and need no
// binding; `*` and `/` between fx values are what these functions exist to replace.
//
// Every entry point range-checks BEFORE it calls a foundation helper. fx.h's helpers TL_ASSERT
// their preconditions, and an assert is a process kill: a script handing fx.dist four corner
// coordinates must get a Luau error it can see, never a TL_FATAL in a peer's tick.
#include <lua.h>
#include <lualib.h>

#include "foundation/det_math.h"
#include "foundation/fx_palette.h"
#include "foundation/rng.h"
#include "script/vm.h"

namespace {

// The frac-bit constants of docs/LUAU-LAYER.md §10.3, exported to script for fx.str/fx.to_f64.
// Read from the palette types themselves so a row retune cannot leave them behind.
struct FxRow {
    const char* name;
    const char* upper;
    i32         frac;
};

const FxRow FX_ROWS[] = {
    { "pos", "POS", pos_t::FRAC_BITS },         { "vel", "VEL", vel_t::FRAC_BITS },
    { "invmass", "INVMASS", invmass_t::FRAC_BITS }, { "stiff", "STIFF", stiff_t::FRAC_BITS },
    { "q", "Q", q_t::FRAC_BITS },               { "angle", "ANGLE", angle_t::FRAC_BITS },
    { "omega", "OMEGA", omega_t::FRAC_BITS },   { "scalar", "SCALAR", scalar_t::FRAC_BITS },
    { nullptr, nullptr, 0 },
};

// docs/LUAU-LAYER.md §10.3: the ONE argument check every binding uses. A Luau number that is not
// an exact integer, or is outside [lo, hi], is an argument error naming the function, the
// argument index and the bound - never a silent truncation, which is how a non-integer reaches
// state in a program that has no floats anywhere else.
i64 check_int(lua_State* L, int idx, i64 lo, i64 hi) {
    if (lua_type(L, idx) != LUA_TNUMBER) luaL_typeerror(L, idx, "number");
    const double x = lua_tonumber(L, idx);
    if (!(x >= (double)lo && x <= (double)hi)) {
        // lua_pushfstring's %d is an int, and the integer helpers' bounds are +-2^53 - printing
        // them through it told the script a bound that was not the bound. Print the numbers only
        // where they survive the cast; say so plainly where they do not.
        if (lo >= (i64)INT32_MIN && hi <= (i64)INT32_MAX) {
            luaL_argerror(L, idx, lua_pushfstring(L, "out of range [%d, %d]", (int)lo, (int)hi));
        }
        luaL_argerror(L, idx, "outside the exact-integer range (+-2^53)");
    }
    const i64 i = (i64)x;
    if ((double)i != x) luaL_argerror(L, idx, "not an integer");
    return i;
}

// A raw row value: any i32 bit pattern is a legal member of every 32-bit row.
i32 check_raw(lua_State* L, int idx) { return (i32)check_int(L, idx, INT32_MIN, INT32_MAX); }

// Pushes a raw row value. Every row is 32-bit (docs/CANON.md), so this is exact in a Luau double.
void push_raw(lua_State* L, i32 v) { lua_pushinteger(L, v); }

// Raises when a computed result leaves the 32-bit row it is supposed to land in. Separate from
// check_int because this is an OUTPUT bound: the inputs were legal and the operation overflowed,
// which is a script logic error and is reported as such rather than wrapped.
void check_fits_i32(lua_State* L, i64 q, const char* what) {
    if (q < (i64)INT32_MIN || q > (i64)INT32_MAX) {
        luaL_error(L, "%s: result %d does not fit the row", what, (int)(q >> 32));
    }
}

// --- literal constructors (docs/LUAU-LAYER.md §10.3, docs/ASSETS-AND-DATA.md §7 R-2) ---------

// fx.pos(12.5) and friends. Upvalue 1 is the row's FRAC. `n` is accepted IFF n x 2^FRAC is
// exactly an integer and |n| < 2^(31-FRAC); then THAT integer is the raw value. No rounding ever
// happens here - a literal that cannot be represented is an authoring error, and rounding it
// silently is how two peers end up with two different constants.
//
// Scaling by a power of two is exact in a double, so the test is the multiply itself: if the
// product is not its own truncation, the literal was not representable.
int fx_literal(lua_State* L) {
    const i32 frac = (i32)lua_tointeger(L, lua_upvalueindex(1));
    if (lua_type(L, 1) != LUA_TNUMBER) luaL_typeerror(L, 1, "number");
    const double n = lua_tonumber(L, 1);
    if (n != n) luaL_argerror(L, 1, "not a number");
    double scale = 1.0;
    for (i32 i = 0; i < frac; ++i) scale *= 2.0;          // exact: frac <= 30
    const double scaled = n * scale;
    // |n| < 2^(31-FRAC), i.e. |scaled| < 2^31. Strict on both ends, as the spec spells it: the
    // one value this excludes is INT32_MIN, which fx.raw reaches directly.
    if (!(scaled > -2147483648.0 && scaled < 2147483648.0)) {
        luaL_argerror(L, 1, "out of range for this row");
    }
    if (scaled != (double)(i64)scaled) {
        luaL_argerror(L, 1, "not exactly representable in this row");
    }
    push_raw(L, (i32)(i64)scaled);
    return 1;
}

// fx.raw(bits) - the escape hatch for a computed value: the argument IS the raw representation,
// checked only against the row width.
int fx_raw_fn(lua_State* L) {
    push_raw(L, check_raw(L, 1));
    return 1;
}

// --- the mixed-op table, as raw-bit arithmetic ----------------------------------------------
// Every product below is row-INDEPENDENT once the operands are raw bits: the shift is a property
// of the two FRACs, and the entries of docs/FX-PALETTE.md §3.1 that these serve all share one
// shift. That is why fx.mul_q takes "any row" and fx.mul_scalar takes "any row" rather than
// eight functions each.

// fx.mul_q(q, a) -> a's row. Shift is always 30 (q_t::FRAC), for every row `a`.
int fx_mul_q(lua_State* L) {
    const i64 q = (i64)check_raw(L, 1);
    const i64 a = (i64)check_raw(L, 2);
    const i64 r = fx::rne_shr(q * a, q_t::FRAC_BITS);
    check_fits_i32(L, r, "fx.mul_q");
    push_raw(L, (i32)r);
    return 1;
}

// fx.mul_scalar(s, a) -> a's row. Shift is always 16 (scalar_t::FRAC).
int fx_mul_scalar(lua_State* L) {
    const i64 s = (i64)check_raw(L, 1);
    const i64 a = (i64)check_raw(L, 2);
    const i64 r = fx::rne_shr(s * a, scalar_t::FRAC_BITS);
    check_fits_i32(L, r, "fx.mul_scalar");
    push_raw(L, (i32)r);
    return 1;
}

// fx.div_q(a, b) -> q. Same-row operands only (the shift cancels their FRAC), one RNE rounding.
// b == 0 is an error, never the saturated quotient div<R> returns on a slim tier - a script
// dividing by zero is a bug the script can see and handle.
int fx_div_q(lua_State* L) {
    const i64 a = (i64)check_raw(L, 1);
    const i64 b = (i64)check_raw(L, 2);
    if (b == 0) luaL_error(L, "fx.div_q: division by zero");
    const i64 r = fx::rne_div(a * ((i64)1 << q_t::FRAC_BITS), b);
    check_fits_i32(L, r, "fx.div_q");
    push_raw(L, (i32)r);
    return 1;
}

// fx.mul_pos_vel_dt(x, v) -> pos. The integrate step, x + v*h. There is no dt argument because
// dt_t only ever holds H (docs/CANON.md "World constants").
int fx_mul_pos_vel_dt(lua_State* L) {
    const pos_t x = fx::fx_raw<pos_t>(check_raw(L, 1));
    const vel_t v = fx::fx_raw<vel_t>(check_raw(L, 2));
    push_raw(L, (x + fx::mul<pos_t>(v, fx::H)).v);
    return 1;
}

// fx.vel_from_delta(dx) -> vel. The implicit-velocity step, (x - x_prev) * INV_H, exact (a
// two-bit widening). |dx| is bounded so the widened product still fits vel_t.
int fx_vel_from_delta(lua_State* L) {
    const i64 dx = (i64)check_raw(L, 1);
    const i64 p = dx * (i64)fx::INV_H;
    const i64 r = p * 4;                       // vel_t::FRAC - pos_t::FRAC == 2, exact
    check_fits_i32(L, r, "fx.vel_from_delta");
    push_raw(L, (i32)r);
    return 1;
}

// fx.dist(x0, y0, x1, y1) -> pos. The only path to pos x pos: the exact Q36 square, then one
// correctly-rounded sqrt. Guarded against the two ranges det_math asserts on - a world-spanning
// difference squared overflows i64, and a distance over 8,192 m does not fit pos_t.
int fx_dist(lua_State* L) {
    const i64 x0 = (i64)check_raw(L, 1), y0 = (i64)check_raw(L, 2);
    const i64 x1 = (i64)check_raw(L, 3), y1 = (i64)check_raw(L, 4);
    const i64 dx = (i64)(i32)((u32)(i64)x1 - (u32)(i64)x0);
    const i64 dy = (i64)(i32)((u32)(i64)y1 - (u32)(i64)y0);
    const i64 sx = dx * dx, sy = dy * dy;      // each <= 2^62, exact
    if (sx > INT64_MAX - sy) luaL_error(L, "fx.dist: separation too large to square");
    const i64 n = sx + sy;
    if (n > (i64)INT32_MAX * (i64)INT32_MAX) luaL_error(L, "fx.dist: distance does not fit pos");
    push_raw(L, fx::sqrt<pos_t>(fx::fx_raw<fx::pos2_wide_t>(n)).v);
    return 1;
}

// fx.normalize(dx, dy) -> qx, qy. The zero vector returns (0, 0) by contract - checked here, not
// left to det_math's assert, which would kill the process instead of the script's bad call.
int fx_normalize(lua_State* L) {
    const i64 dx = (i64)check_raw(L, 1), dy = (i64)check_raw(L, 2);
    if (dx == 0 && dy == 0) {
        push_raw(L, 0);
        push_raw(L, 0);
        return 2;
    }
    const i64 sx = dx * dx, sy = dy * dy;
    if (sx > INT64_MAX - sy) luaL_error(L, "fx.normalize: vector too large to square");
    const i64 n = sx + sy;
    if (n > (i64)INT32_MAX * (i64)INT32_MAX) luaL_error(L, "fx.normalize: length does not fit pos");
    const fx::vec2<pos_t> d = { fx::fx_raw<pos_t>((i32)dx), fx::fx_raw<pos_t>((i32)dy) };
    const fx::vec2<q_t> u = fx::normalize(d);
    push_raw(L, u.x.v);
    push_raw(L, u.y.v);
    return 2;
}

// fx.sincos(a) -> qs, qc. Any angle_t value is valid: reduction mod one turn is an exact mask.
int fx_sincos(lua_State* L) {
    q_t s = fx::fx_raw<q_t>(0), c = fx::fx_raw<q_t>(0);
    fx::sincos(fx::fx_raw<angle_t>(check_raw(L, 1)), &s, &c);
    push_raw(L, s.v);
    push_raw(L, c.v);
    return 2;
}

// fx.atan2(y, x) -> angle, over pos_t operands. (0, 0) has no angle: an error, not det_math's
// assert-and-return-zero, for the same reason fx_normalize checks first.
int fx_atan2(lua_State* L) {
    const i32 y = check_raw(L, 1), x = check_raw(L, 2);
    if (y == 0 && x == 0) luaL_error(L, "fx.atan2: (0, 0) has no angle");
    push_raw(L, fx::atan2(fx::fx_raw<pos_t>(y), fx::fx_raw<pos_t>(x)).v);
    return 1;
}

// fx.atan2_q(y, x) -> angle, over q_t operands (unit vectors and normals). Same contract.
int fx_atan2_q(lua_State* L) {
    const i32 y = check_raw(L, 1), x = check_raw(L, 2);
    if (y == 0 && x == 0) luaL_error(L, "fx.atan2_q: (0, 0) has no angle");
    push_raw(L, fx::atan2q(fx::fx_raw<q_t>(y), fx::fx_raw<q_t>(x)).v);
    return 1;
}

// fx.lerp(a, b, t) -> a's row. a + (b - a) * t, one RNE; t is q_t and may lie outside [0, 1]
// (extrapolation is allowed, docs/FX-PALETTE.md). Row-independent in raw bits.
int fx_lerp(lua_State* L) {
    const i64 a = (i64)check_raw(L, 1);
    const i64 b = (i64)check_raw(L, 2);
    const i64 t = (i64)check_raw(L, 3);
    const i64 d = (i64)(i32)((u32)b - (u32)a);           // the row's wrapping subtract
    const i64 r = a + fx::rne_shr(d * t, q_t::FRAC_BITS);
    check_fits_i32(L, r, "fx.lerp");
    push_raw(L, (i32)r);
    return 1;
}

// fx.sat_add(a, b) - saturating i32 add, the quanta paths' only legal addition. Plain `+` on a
// conserved quantum is a review rejection (docs/LUAU-LAYER.md §10.3).
int fx_sat_add(lua_State* L) {
    push_raw(L, fx::sat_add(check_raw(L, 1), check_raw(L, 2)));
    return 1;
}

// fx.sat_sub(a, b) - saturating i32 subtract; same contract as fx.sat_add.
int fx_sat_sub(lua_State* L) {
    push_raw(L, fx::sat_sub(check_raw(L, 1), check_raw(L, 2)));
    return 1;
}

// fx.imin(a, b) - `math` is gone from the sim VM, so the checked integer ops live here. The
// bound is +-2^53, where a Luau number is still an exact integer.
enum : i64 { FX_INT_MAX = 9007199254740992ll };

// fx.imin(a, b) -> the smaller. Both arguments are exact integers within +-2^53.
int fx_imin(lua_State* L) {
    const i64 a = check_int(L, 1, -FX_INT_MAX, FX_INT_MAX), b = check_int(L, 2, -FX_INT_MAX, FX_INT_MAX);
    lua_pushnumber(L, (double)(a < b ? a : b));
    return 1;
}

// fx.imax(a, b) -> the larger. Same bounds as fx.imin.
int fx_imax(lua_State* L) {
    const i64 a = check_int(L, 1, -FX_INT_MAX, FX_INT_MAX), b = check_int(L, 2, -FX_INT_MAX, FX_INT_MAX);
    lua_pushnumber(L, (double)(a > b ? a : b));
    return 1;
}

// fx.iabs(a) -> |a|. Cannot overflow: the bound is +-2^53, well inside i64.
int fx_iabs(lua_State* L) {
    const i64 a = check_int(L, 1, -FX_INT_MAX, FX_INT_MAX);
    lua_pushnumber(L, (double)(a < 0 ? -a : a));
    return 1;
}

// fx.iclamp(x, lo, hi) -> x clamped. lo > hi is an error, not a silently swapped range.
int fx_iclamp(lua_State* L) {
    const i64 x = check_int(L, 1, -FX_INT_MAX, FX_INT_MAX);
    const i64 lo = check_int(L, 2, -FX_INT_MAX, FX_INT_MAX);
    const i64 hi = check_int(L, 3, -FX_INT_MAX, FX_INT_MAX);
    if (lo > hi) luaL_error(L, "fx.iclamp: lo > hi");
    lua_pushnumber(L, (double)(x < lo ? lo : (x > hi ? hi : x)));
    return 1;
}

// fx.str(v, frac) -> a decimal string, for log lines only. Exact per digit: the fraction is
// developed by repeated multiply-by-ten on the remainder, truncated at nine places (a Q30
// fraction has an exact 30-digit expansion; nine is what a log line wants, and the truncation is
// stated here rather than discovered).
//
// Not fmt_buf: docs/LUAU-LAYER.md §10.3 named it, and fmt_buf is still a TL_FATAL stub blocked on
// vendor/stb_sprintf (TODO.md, W1 containers). This is integer-only and needs no formatter.
int fx_str(lua_State* L) {
    const i64 v = (i64)check_raw(L, 1);
    const i64 frac = check_int(L, 2, 0, 30);
    char buf[48];
    u32 n = 0;
    const bool neg = v < 0;
    const u64 mag = neg ? (u64)(-v) : (u64)v;
    const u64 mask = ((u64)1 << (u32)frac) - 1u;
    u64 whole = mag >> (u32)frac;
    char digits[24];
    u32 dn = 0;
    do { digits[dn++] = (char)('0' + (int)(whole % 10u)); whole /= 10u; } while (whole != 0);
    if (neg) buf[n++] = '-';
    while (dn != 0) buf[n++] = digits[--dn];
    buf[n++] = '.';
    u64 rem = mag & mask;
    for (u32 i = 0; i < 9u; ++i) {
        rem *= 10u;
        buf[n++] = (char)('0' + (int)(rem >> (u32)frac));
        rem &= mask;
    }
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

// fx.to_f64(v, frac) -> a double. UI VM ONLY (docs/LUAU-LAYER.md §10.3): a scaled double is
// exactly what must never exist in the sim VM, and the sim table does not carry this entry.
int fx_to_f64(lua_State* L) {
    const i64 v = (i64)check_raw(L, 1);
    const i64 frac = check_int(L, 2, 0, 30);
    double scale = 1.0;
    for (i64 i = 0; i < frac; ++i) scale *= 2.0;
    lua_pushnumber(L, (double)v / scale);
    return 1;
}

// fx.rng_below(carrier, draw, n) -> [0, n). The keyed draw of docs/DETERMINISM.md §3 needs the
// RUNNING system's id and the world's (seed, tick); the schedule publishes them, and the Luau
// system trampoline that reads them is the W3 luau-bindings lane's (docs/LUAU-LAYER.md §10.6).
// Outside a system this is an error BY SPEC (§10.3), which is exactly the state every call is in
// until that lane lands - so this is the specified behaviour, not a stub.
int fx_rng_below(lua_State* L) {
    (void)check_int(L, 1, -FX_INT_MAX, FX_INT_MAX);
    (void)check_int(L, 2, 0, FX_INT_MAX);
    (void)check_int(L, 3, 1, (i64)UINT32_MAX);
    luaL_error(L, "fx.rng_below: no system is running - a keyed draw needs the running system's id");
}

// fx.rng_q(carrier, draw) -> q in [0, 1). Same contract and the same requirement as fx.rng_below.
int fx_rng_q(lua_State* L) {
    (void)check_int(L, 1, -FX_INT_MAX, FX_INT_MAX);
    (void)check_int(L, 2, 0, FX_INT_MAX);
    luaL_error(L, "fx.rng_q: no system is running - a keyed draw needs the running system's id");
}

// Sets t[name] = fn on the table at the top of the stack.
void set_fn(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

// Sets t[name] = value (an exact integer) on the table at the top of the stack.
void set_int(lua_State* L, const char* name, i64 value) {
    lua_pushnumber(L, (double)value);
    lua_setfield(L, -2, name);
}

}  // namespace

void script_bind_fx(ScriptVm* vm) {
    lua_State* L = vm->L;
    lua_createtable(L, 0, 40);

    // The literal constructors, one closure per row over the row's FRAC.
    for (const FxRow* r = FX_ROWS; r->name != nullptr; ++r) {
        lua_pushinteger(L, r->frac);
        lua_pushcclosurek(L, &fx_literal, r->name, 1, nullptr);
        lua_setfield(L, -2, r->name);
    }
    set_fn(L, "raw", &fx_raw_fn);

    // The frac-bit constants (docs/LUAU-LAYER.md §10.3: "for fx.str only").
    for (const FxRow* r = FX_ROWS; r->name != nullptr; ++r) set_int(L, r->upper, r->frac);

    // The world constants, as raw bits of their own rows or as plain integers.
    set_int(L, "H", fx::H.v);
    set_int(L, "INV_H", fx::INV_H);
    set_int(L, "G_SUBSTEP", fx::G_SUBSTEP.v);
    set_int(L, "TEXEL", fx::TEXEL.v);
    set_int(L, "V_MAX_WORLD", fx::V_MAX_WORLD.v);

    set_fn(L, "mul_q", &fx_mul_q);
    set_fn(L, "mul_scalar", &fx_mul_scalar);
    set_fn(L, "div_q", &fx_div_q);
    set_fn(L, "mul_pos_vel_dt", &fx_mul_pos_vel_dt);
    set_fn(L, "vel_from_delta", &fx_vel_from_delta);
    set_fn(L, "dist", &fx_dist);
    set_fn(L, "normalize", &fx_normalize);
    set_fn(L, "sincos", &fx_sincos);
    set_fn(L, "atan2", &fx_atan2);
    set_fn(L, "atan2_q", &fx_atan2_q);
    set_fn(L, "lerp", &fx_lerp);
    set_fn(L, "sat_add", &fx_sat_add);
    set_fn(L, "sat_sub", &fx_sat_sub);
    set_fn(L, "imin", &fx_imin);
    set_fn(L, "imax", &fx_imax);
    set_fn(L, "iabs", &fx_iabs);
    set_fn(L, "iclamp", &fx_iclamp);
    set_fn(L, "str", &fx_str);
    set_fn(L, "rng_below", &fx_rng_below);
    set_fn(L, "rng_q", &fx_rng_q);

    // The UI VM is the only one that may turn a row back into a double: it draws and it prints,
    // and neither feeds state (docs/LUAU-LAYER.md §10.3).
    if (vm->kind == SCRIPT_VM_UI) set_fn(L, "to_f64", &fx_to_f64);

    script_freeze_table(L, -1);
    lua_setglobal(L, "fx");
}
