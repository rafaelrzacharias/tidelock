// solver.cpp - the disposable Gate 0 solver body (contract: solver.h). Compiled twice: as the
// fixed-point solver (namespace g0, via solver_fx.h) and, in dev tiers, as the double shadow
// (namespace g0s, via solver_shadow.cpp). Every arithmetic line goes through g0_ops.h.
// Spec: docs/GATE0-BENCH.md §8.3; docs/ALLOY.md §14.4.B, §14.4.3 (the order and the arithmetic).
#ifdef GATE0_SHADOW
#include "gate0/solver_dbl.h"
#else
#include "gate0/solver_fx.h"
#endif

#include "foundation/hash.h"

#include <stdio.h>
#include <string.h>

namespace G0_NS {

namespace {

// --- helpers over the columns ---------------------------------------------------------------
template <typename T>
T* col(VMemArena* a, u32 n) {
    return (T*)arena_push(a, u64(n) * sizeof(T), 16u);
}
template <typename T>
T* tmp(Scratch* s, u32 n) {
    return (T*)scratch_push(s, u64(n) * sizeof(T) + 16u, 16u);   // +16: a zero-count push still returns a valid pointer
}
inline bool body_static(const World* w, u32 b) { return (w->bflags[b] & BF_STATIC) != 0; }
inline bool part_liquid(const World* w, u32 p) { return (w->pflags[p] & PF_LIQUID) != 0; }
inline bool part_dynamic(const World* w, u32 p) { return !is_zero_w(w->pinv_m[p]); }

// The per-tick travel bound of a carrier: |v| * h * substeps per axis, summed (a Manhattan
// bound over the Euclidean one, conservative). docs/ALLOY.md §1.2: the contact margin covers
// the maximum substep travel so contacts generated once per tick stay valid for every substep.
inline pos_t travel_of(const World* w, vel_t vx, vel_t vy) {
    const pos_t tx = pos_abs(predict_delta(vx, w->k.h));
    const pos_t ty = pos_abs(predict_delta(vy, w->k.h));
    return pos_mul_int(pos_add(tx, ty), i32(w->k.substeps));
}

// --- broadphase runs: sorted (key, index) pairs -> runs, plus a binary search ---------------
struct Runs { u32* key; u32* begin; u32* end; u32 n; };

Runs build_runs(Scratch* s, const u32* sorted_keys, u32 n) {
    Runs r;
    r.key = tmp<u32>(s, n); r.begin = tmp<u32>(s, n); r.end = tmp<u32>(s, n); r.n = 0;
    u32 i = 0;
    while (i < n) {
        u32 j = i + 1;
        while (j < n && sorted_keys[j] == sorted_keys[i]) { j += 1; }
        r.key[r.n] = sorted_keys[i]; r.begin[r.n] = i; r.end[r.n] = j; r.n += 1;
        i = j;
    }
    return r;
}
// Returns the run index for `key` or 0xFFFFFFFF.
u32 find_run(const Runs* r, u32 key) {
    u32 lo = 0, hi = r->n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (r->key[mid] < key) { lo = mid + 1; } else { hi = mid; }
    }
    return (lo < r->n && r->key[lo] == key) ? lo : 0xFFFFFFFFu;
}

// --- contact generation (docs/GATE0-BENCH.md §7 R-1: corner-vs-SDF deepest point) ---------
struct Cand { pos_t sd; vec2<pos_t> point; vec2<q_t> n; };

// Inserts c into the DEEPEST_K smallest-sd set, ascending by sd (ties keep insertion order).
void keep_deepest(Cand* set, u32* count, Cand c) {
    u32 pos = *count;
    while (pos > 0 && pos_lt(c.sd, set[pos - 1].sd)) { pos -= 1; }
    if (pos >= DEEPEST_K) return;
    const u32 last = *count < DEEPEST_K ? *count : DEEPEST_K - 1;
    for (u32 k = last; k > pos; k -= 1) { set[k] = set[k - 1]; }
    set[pos] = c;
    if (*count < DEEPEST_K) *count += 1;
}

void push_contact(World* w, u8 kind, u32 i, u32 j, vec2<pos_t> point, vec2<q_t> n, pos_t sd) {
    if (w->nc >= w->cap_c) { TL_FATAL("gate0: contact capacity exceeded - raise cap_c"); }
    Contact* c = &w->contacts[w->nc];
    w->nc += 1;
    c->i = i; c->j = j; c->kind = kind; c->_pad0[0] = c->_pad0[1] = c->_pad0[2] = 0; c->color = 0;
    c->point = point; c->n = n; c->depth = pos_neg(sd);
    c->lam_n = local_zero(); c->lam_t = local_zero();
    // lever arms and the pair-clamped inverse masses (docs/ALLOY.md §14.4.3 w_eff)
    invmass_t wi_raw, wj_raw;
    if (kind == CK_PB) {
        wi_raw = w->pinv_m[i];
        c->rn_i = pos_zero();
    } else {
        wi_raw = w->binv_m[i];
        const vec2<pos_t> ri = vsub(point, { w->bx[i], w->by[i] });
        c->rn_i = cross_pn(ri, n);
    }
    wj_raw = w->binv_m[j];
    const vec2<pos_t> rj = vsub(point, { w->bx[j], w->by[j] });
    c->rn_j = cross_pn(rj, n);
    c->wi = w_clamp(wi_raw, wj_raw);
    c->wj = w_clamp(wj_raw, wi_raw);
    c->wi_ang = (kind == CK_PB || is_zero_w(wi_raw)) ? local_zero() : w_ang30(w->binv_i[i], c->rn_i);
    c->wj_ang = is_zero_w(wj_raw) ? local_zero() : w_ang30(w->binv_i[j], c->rn_j);
}

// a's corners against b's SDF; contacts with i = a, j = b, n out of b.
void gen_corners(World* w, u32 a, u32 b, q_t sa, q_t ca, q_t sb, q_t cb, pos_t margin) {
    Cand set[DEEPEST_K]; u32 cnt = 0;
    const pos_t hw = w->bhw[a], hh = w->bhh[a];
    const vec2<pos_t> corners[4] = { { hw, hh }, { pos_neg(hw), hh }, { pos_neg(hw), pos_neg(hh) }, { hw, pos_neg(hh) } };
    const vec2<pos_t> xa = { w->bx[a], w->by[a] };
    const vec2<pos_t> xb = { w->bx[b], w->by[b] };
    const vec2<pos_t> hint = rot(vsub(xa, xb), q_neg(sb), cb);        // a's centre in b space (the corner tie-break)
    for (u32 k = 0; k < 4; ++k) {
        const vec2<pos_t> world = vadd(xa, rot(corners[k], sa, ca));
        const vec2<pos_t> l = rot(vsub(world, xb), q_neg(sb), cb);   // world -> b space: rotate by -theta_b
        vec2<q_t> nl;
        const pos_t sd = box_sdf(l, w->bhw[b], w->bhh[b], hint, &nl);
        if (pos_lt(sd, margin)) {
            Cand c; c.sd = sd; c.point = world; c.n = rotate_q(nl, sb, cb);
            keep_deepest(set, &cnt, c);
        }
    }
    for (u32 k = 0; k < cnt; ++k) { push_contact(w, CK_BB, a, b, set[k].point, set[k].n, set[k].sd); }
}

void gen_bb(World* w, u32 a, u32 b) {
    q_t sa, ca, sb, cb;
    sincos_of(w->bth[a], &sa, &ca);
    sincos_of(w->bth[b], &sb, &cb);
    const pos_t margin = pos_add(w->k.contact_margin,
                                 pos_add(travel_of(w, w->bvx[a], w->bvy[a]), travel_of(w, w->bvx[b], w->bvy[b])));
    gen_corners(w, a, b, sa, ca, sb, cb, margin);   // a's corners in b
    gen_corners(w, b, a, sb, cb, sa, ca, margin);   // b's corners in a
}

void gen_pb(World* w, u32 p, u32 b, q_t sb, q_t cb, pos_t margin_b) {
    const vec2<pos_t> xp = { w->px[p], w->py[p] };
    const vec2<pos_t> l = rot(vsub(xp, { w->bx[b], w->by[b] }), q_neg(sb), cb);
    vec2<q_t> nl;
    const pos_t sd = box_sdf(l, w->bhw[b], w->bhh[b], l, &nl);       // a particle's own position is the tie-break
    const pos_t margin = pos_add(margin_b, travel_of(w, w->pvx[p], w->pvy[p]));
    if (pos_lt(sd, margin)) { push_contact(w, CK_PB, p, b, xp, rotate_q(nl, sb, cb), sd); }
}

// --- colouring (docs/ALLOY.md §14.4.3 "Colouring", greedy in stable order) -----------------
struct Colorer { u64* mask; u32 np; };   // mask[(carrier * COLOR_WORDS) + word]; carriers: particles, then bodies

inline u32 carrier_body(const Colorer* c, u32 b) { return c->np + b; }

u32 lowest_free(const Colorer* c, const u32* carriers, u32 n) {
    u64 used[COLOR_WORDS];
    for (u32 word = 0; word < COLOR_WORDS; ++word) used[word] = 0;
    for (u32 k = 0; k < n; ++k) {
        const u64* m = &c->mask[u64(carriers[k]) * COLOR_WORDS];
        for (u32 word = 0; word < COLOR_WORDS; ++word) used[word] |= m[word];
    }
    for (u32 word = 0; word < COLOR_WORDS; ++word) {
        if (used[word] != ~u64(0)) {
            u32 bit = 0;
            while ((used[word] >> bit) & 1u) { bit += 1; }
            return word * 64u + bit;
        }
    }
    TL_FATAL("gate0: more than MAX_COLORS colours - a carrier shares more than MAX_COLORS constraints");
}
void mark(Colorer* c, const u32* carriers, u32 n, u32 color) {
    for (u32 k = 0; k < n; ++k) {
        c->mask[u64(carriers[k]) * COLOR_WORDS + (color >> 6)] |= u64(1) << (color & 63u);
    }
}

}  // namespace

// --- constants ------------------------------------------------------------------------------
Consts consts_make(u32 substeps, u32 ladder, u32 mu_percent, u32 alpha_nano, u32 density_iters) {
    TL_CHECK(substeps >= 1 && substeps <= 64 && ladder <= 3);
    Consts k;
    memset(&k, 0, sizeof(k));
    k.substeps = substeps;
    k.ladder = ladder;
    k.inv_h = i32(fx::TICK_HZ) * i32(substeps);
    const ::dt_t  h = fx::fx_lit<::dt_t>(1, k.inv_h);
    const ::vel_t g = fx::fx_lit<::vel_t>(981, i64(100) * k.inv_h);
    if (substeps == u32(fx::SUBSTEPS)) {
        TL_CHECK(h.v == fx::H.v && g.v == fx::G_SUBSTEP.v && k.inv_h == fx::INV_H);   // docs/CANON.md
    }
    k.h = cvt_dt(h);
    k.g_sub = cvt_vel(g);
    k.inv_two_pi = cvt_q(fx::fx_lit<::q_t>(1000000000, 6283185307));
    k.two_pi = cvt_scalar(fx::fx_lit<::scalar_t>(6283185307, 1000000000));
    k.mu = cvt_q(fx::fx_lit<::q_t>(i64(mu_percent), 100));
    k.restitution = q_zero();
    k.rest_vel_min = cvt_vel(fx::fx_raw<::vel_t>(g.v * 2));
    k.contact_margin = cvt_pos(fx::fx_raw<::pos_t>(2 * fx::TEXEL.v));
    k.h_kernel = cvt_pos(fx::fx_raw<::pos_t>(4 * fx::TEXEL.v));
    {   // q = r / h_kernel as an exact shift (docs/GATE0-BENCH.md §7 R-3 normalize-once): the
        // raw kernel radius is a power of two by construction, asserted, so div<q_t>(r, h) and
        // (r.v << q_shift) are the same bits.
        const i32 hk_raw = 4 * fx::TEXEL.v;
        TL_CHECK((hk_raw & (hk_raw - 1)) == 0);
        u32 hs = 0;
        while ((i32(1) << hs) != hk_raw) { hs += 1; }
        k.q_shift = 30u - hs;
    }
    k.c_visc = cvt_q(fx::fx_lit<::q_t>(1, 20));
    // alpha~ = alpha / h^2 = alpha_nano * 1e-9 * inv_h^2 (docs/ALLOY.md §8.1: alpha~ precomputed at init from the data alpha);
    // alpha_nano 1302 = alpha~ 0.3 at 480 Hz - the smallest compliance that holds a 7.8 m column
    // (measured: alpha~ <= 0.1 tunnels the floor on landing, 1.0 gives 4.2% density error).
    k.at_density = cvt_stiff(fx::fx_lit<::stiff_t>(i64(alpha_nano) * i64(k.inv_h) * i64(k.inv_h), 1000000000));
    k.at_contact = cvt_stiff(fx::fx_raw<::stiff_t>(0));
    k.density_iters = density_iters == 0 ? 1 : density_iters;
    // kw: rho == ONE on the rest lattice (spacing 2 texels, radius 4 texels): the eight
    // neighbours at q = 1/2 (four) and q = sqrt(1/2) (four), plus the self term W(0) = 1 -
    // evaluated with the SAME fx kernel arithmetic the substep uses, then one rne_div.
    {
        const ::q_t one = fx::fx_raw<::q_t>(::q_t::ONE);
        const ::pos_t s = fx::fx_raw<::pos_t>(2 * fx::TEXEL.v);
        const ::pos_t hk = fx::fx_raw<::pos_t>(4 * fx::TEXEL.v);
        const fx::vec2<::pos_t> d1 = { s, fx::fx_raw<::pos_t>(0) };
        const fx::vec2<::pos_t> d2 = { s, s };
        const ::q_t q1 = fx::div<::q_t>(fx::len(d1), hk);
        const ::q_t q2 = fx::div<::q_t>(fx::len(d2), hk);
        const ::q_t a1 = one - fx::mul<::q_t>(q1, q1);
        const ::q_t a2 = one - fx::mul<::q_t>(q2, q2);
        const ::q_t w1 = fx::mul<::q_t>(fx::mul<::q_t>(a1, a1), a1);
        const ::q_t w2 = fx::mul<::q_t>(fx::mul<::q_t>(a2, a2), a2);
        const i64 sum30 = i64(one.v) + 4 * i64(w1.v) + 4 * i64(w2.v);          // 51/16 at frac 30
        const i64 kw30 = fx::rne_div(i64(one.v) * i64(one.v), sum30);            // ONE / sum
        TL_CHECK(fx::fx_fits<::q_t>(kw30));
        k.kw = cvt_q(fx::fx_raw<::q_t>(i32(kw30)));
    }
    return k;
}

// --- world ----------------------------------------------------------------------------------
void world_init(World* w, const VMemApi* os, Scratch* scratch, u32 cap_b, u32 cap_p, u32 cap_d, const Consts* k) {
    memset(w, 0, sizeof(*w));
    w->k = *k;
    w->cap_b = cap_b; w->cap_p = cap_p; w->cap_d = cap_d;
    w->scratch = scratch;
    TL_CHECK(vmem_arena_init(&w->a_hdr,  "gate0.hdr"_id,      64u * 1024u,                       ARENA_ZERO_ON_PUSH, os) == ERR_OK);
    TL_CHECK(vmem_arena_init(&w->a_body, "gate0.body"_id,     u64(cap_b) * 256u + 65536u,         ARENA_ZERO_ON_PUSH, os) == ERR_OK);
    TL_CHECK(vmem_arena_init(&w->a_part, "gate0.particle"_id, u64(cap_p) * 128u + 65536u,         ARENA_ZERO_ON_PUSH, os) == ERR_OK);
    TL_CHECK(vmem_arena_init(&w->a_dist, "gate0.dist"_id,     u64(cap_d) * 64u + 65536u,          ARENA_ZERO_ON_PUSH, os) == ERR_OK);
    w->hdr = col<Header>(&w->a_hdr, 1);
    // body columns, fixed order (the arena layout IS the hashed byte order)
    VMemArena* B = &w->a_body;
    w->bx = col<pos_t>(B, cap_b); w->by = col<pos_t>(B, cap_b); w->bpx = col<pos_t>(B, cap_b); w->bpy = col<pos_t>(B, cap_b);
    w->bth = col<angle_t>(B, cap_b); w->bpth = col<angle_t>(B, cap_b);
    w->bvx = col<vel_t>(B, cap_b); w->bvy = col<vel_t>(B, cap_b); w->bw = col<omega_t>(B, cap_b);
    w->binv_m = col<invmass_t>(B, cap_b); w->binv_i = col<invmass_t>(B, cap_b);
    w->bhw = col<pos_t>(B, cap_b); w->bhh = col<pos_t>(B, cap_b);
    w->bmass = col<i32>(B, cap_b); w->bflags = col<u8>(B, cap_b);
    w->bres_x = col<i32>(B, cap_b); w->bres_y = col<i32>(B, cap_b);
    VMemArena* P = &w->a_part;
    w->px = col<pos_t>(P, cap_p); w->py = col<pos_t>(P, cap_p); w->ppx = col<pos_t>(P, cap_p); w->ppy = col<pos_t>(P, cap_p);
    w->pvx = col<vel_t>(P, cap_p); w->pvy = col<vel_t>(P, cap_p);
    w->pinv_m = col<invmass_t>(P, cap_p); w->pmass = col<i32>(P, cap_p); w->pflags = col<u8>(P, cap_p);
    w->pres_x = col<i32>(P, cap_p); w->pres_y = col<i32>(P, cap_p);
    VMemArena* D = &w->a_dist;
    w->da = col<u32>(D, cap_d); w->db = col<u32>(D, cap_d); w->drest = col<pos_t>(D, cap_d);
    w->dat = col<stiff_t>(D, cap_d); w->dlam = col<lambda_t>(D, cap_d); w->dcolor = col<u32>(D, cap_d);
    // the registered set (docs/MEMORY.md §8.3): registration order is the hash fold order
    registry_add(&w->reg, "gate0.hdr"_id,      &w->a_hdr,  ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&w->reg, "gate0.body"_id,     &w->a_body, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&w->reg, "gate0.particle"_id, &w->a_part, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_add(&w->reg, "gate0.dist"_id,     &w->a_dist, ARENA_HASHED | ARENA_SNAPSHOT);
    registry_seal(&w->reg);
}

void world_release(World* w, const VMemApi* os) {
    os->release(os->ctx, w->a_hdr.base, w->a_hdr.reserved);
    os->release(os->ctx, w->a_body.base, w->a_body.reserved);
    os->release(os->ctx, w->a_part.base, w->a_part.reserved);
    os->release(os->ctx, w->a_dist.base, w->a_dist.reserved);
}

void world_load(World* w, const g0scene::Scene* s) {
    TL_CHECK(s->nb <= w->cap_b && s->np <= w->cap_p && s->nd <= w->cap_d);
    w->nb = s->nb; w->np = s->np; w->nd = s->nd;
    w->hdr->tick = 0; w->hdr->nb = s->nb; w->hdr->np = s->np; w->hdr->nd = s->nd;
    w->hdr->substeps = w->k.substeps; w->hdr->ladder = w->k.ladder; w->hdr->_pad0 = 0;
    for (u32 b = 0; b < s->nb; ++b) {
        const g0scene::SceneBody* sb = &s->bodies[b];
        w->bx[b] = cvt_pos(sb->x); w->by[b] = cvt_pos(sb->y);
        w->bth[b] = cvt_ang(sb->th);
        w->bvx[b] = cvt_vel(sb->vx); w->bvy[b] = cvt_vel(sb->vy); w->bw[b] = cvt_omega(sb->w);
        w->binv_m[b] = cvt_w(sb->inv_m); w->binv_i[b] = cvt_w(sb->inv_i);
        w->bhw[b] = cvt_pos(sb->hw); w->bhh[b] = cvt_pos(sb->hh);
        w->bmass[b] = sb->mass_quanta;
        w->bflags[b] = is_zero_w(w->binv_m[b]) ? BF_STATIC : 0;
        w->bres_x[b] = 0; w->bres_y[b] = 0;
        // implicit velocity encoding: px = x - v*h (docs/ALLOY.md §14.4.3 S5)
        w->bpx[b] = pos_sub(w->bx[b], predict_delta(w->bvx[b], w->k.h));
        w->bpy[b] = pos_sub(w->by[b], predict_delta(w->bvy[b], w->k.h));
        w->bpth[b] = ang_sub(w->bth[b], ang_delta(w->bw[b], w->k.h));
    }
    for (u32 p = 0; p < s->np; ++p) {
        const g0scene::SceneParticle* sp = &s->parts[p];
        w->px[p] = cvt_pos(sp->x); w->py[p] = cvt_pos(sp->y);
        w->pvx[p] = cvt_vel(sp->vx); w->pvy[p] = cvt_vel(sp->vy);
        w->pinv_m[p] = cvt_w(sp->inv_m); w->pmass[p] = sp->mass_quanta;
        w->pflags[p] = sp->liquid ? PF_LIQUID : 0;
        w->pres_x[p] = 0; w->pres_y[p] = 0;
        w->ppx[p] = pos_sub(w->px[p], predict_delta(w->pvx[p], w->k.h));
        w->ppy[p] = pos_sub(w->py[p], predict_delta(w->pvy[p], w->k.h));
    }
    for (u32 d = 0; d < s->nd; ++d) {
        w->da[d] = s->dists[d].a; w->db[d] = s->dists[d].b;
        w->drest[d] = cvt_pos(s->dists[d].rest);
        w->dat[d] = cvt_stiff(s->dists[d].alpha_tilde);
        w->dlam[d] = lam_zero();
        w->dcolor[d] = 0;
    }
    // persistent constraints are coloured ONCE (docs/ALLOY.md §14.4.3): greedy, [slot up],
    // carriers = the dynamic particle endpoints. Contacts and density colour around them per tick.
    {
        TL_SCRATCH_SCOPE_BEGIN(w->scratch);
        Colorer col; col.np = w->np;
        col.mask = tmp<u64>(w->scratch, (w->np + w->nb) * COLOR_WORDS);
        memset(col.mask, 0, u64(w->np + w->nb) * COLOR_WORDS * sizeof(u64));
        for (u32 d = 0; d < w->nd; ++d) {
            u32 carriers[2]; u32 n = 0;
            if (part_dynamic(w, w->da[d])) carriers[n++] = w->da[d];
            if (part_dynamic(w, w->db[d])) carriers[n++] = w->db[d];
            const u32 c = lowest_free(&col, carriers, n);
            mark(&col, carriers, n, c);
            w->dcolor[d] = c;
        }
        TL_SCRATCH_SCOPE_END(w->scratch);
    }
}

// --- tick begin: broadphase, contacts, neighbours, colouring -------------------------------
u32 tick_begin(World* w) {
    Scratch* s = w->scratch;
    TL_SCRATCH_SCOPE_BEGIN(s);
    const u32 nb = w->nb, np = w->np;
    w->xl_bx = tmp<local_t>(s, nb); w->xl_by = tmp<local_t>(s, nb); w->thl = tmp<local_t>(s, nb);
    w->xl_px = tmp<local_t>(s, np); w->xl_py = tmp<local_t>(s, np);
    w->xs_bx = tmp<pos_t>(s, nb); w->xs_by = tmp<pos_t>(s, nb); w->xs_bth = tmp<angle_t>(s, nb);
    w->xs_px = tmp<pos_t>(s, np); w->xs_py = tmp<pos_t>(s, np);
    w->xg_bx = tmp<pos_t>(s, nb); w->xg_by = tmp<pos_t>(s, nb); w->xg_bth = tmp<angle_t>(s, nb);
    w->xg_px = tmp<pos_t>(s, np); w->xg_py = tmp<pos_t>(s, np);
    w->vpre_bx = tmp<vel_t>(s, nb); w->vpre_by = tmp<vel_t>(s, nb);
    w->vpre_px = tmp<vel_t>(s, np); w->vpre_py = tmp<vel_t>(s, np);
    w->cap_c = np * 4u + nb * 64u + 4096u;
    w->contacts = tmp<Contact>(s, w->cap_c); w->nc = 0;
    w->nbr_begin = tmp<u32>(s, np + 1u);
    w->nbr = tmp<u32>(s, np * MAX_NEIGHBOURS);
    w->dens_lam = tmp<local_t>(s, np);
    w->dlam30 = tmp<local_t>(s, w->nd);
    w->gcx = tmp<local_t>(s, np); w->gcy = tmp<local_t>(s, np);
    w->pair_gx = tmp<local_t>(s, np * MAX_NEIGHBOURS); w->pair_gy = tmp<local_t>(s, np * MAX_NEIGHBOURS);
    w->rho = tmp<q_t>(s, np);
    w->dens_active = tmp<u8>(s, np);
    for (u32 b = 0; b < nb; ++b) { w->xg_bx[b] = w->bx[b]; w->xg_by[b] = w->by[b]; w->xg_bth[b] = w->bth[b]; }
    for (u32 p = 0; p < np; ++p) { w->xg_px[p] = w->px[p]; w->xg_py[p] = w->py[p]; w->rho[p] = q_zero(); }

    // -- fine grid over particles (docs/ALLOY.md §14.4.B steps 1-4) --
    u32* pkey = tmp<u32>(s, np);
    u32* pidx = tmp<u32>(s, np);
    for (u32 p = 0; p < np; ++p) { pkey[p] = (fine_cx(w->py[p]) << 16) | fine_cx(w->px[p]); pidx[p] = p; }
    sort_u32_kv(pkey, pidx, np, s);
    const Runs pr = build_runs(s, pkey, np);
    // neighbour lists: liquid particles only; 9-cell walk in fixed order, then insertion-sorted by j
    const pos_t hk = w->k.h_kernel;
    u32 total = 0;
    for (u32 p = 0; p < np; ++p) {
        w->nbr_begin[p] = total;
        if (!part_liquid(w, p)) continue;
        const u32 key = (fine_cx(w->py[p]) << 16) | fine_cx(w->px[p]);
        u32* list = &w->nbr[u64(p) * MAX_NEIGHBOURS];
        u32 cnt = 0;
        // docs/ALLOY.md §1.2: the per-tick list carries a support margin covering the substep travel
        // of BOTH particles (each clamped to one cell = 4 texels: 15 m/s per tick, far above any
        // liquid speed in G-03..G-05), so a pair that closes within h during the tick is on the
        // list. A 5x5 cell walk covers h + 4 texels; the density pass re-tests |d| < h per substep.
        const pos_t cell = cvt_pos(fx::fx_raw<::pos_t>(4 * fx::TEXEL.v));
        const pos_t trav_p = pos_min(travel_of(w, w->pvx[p], w->pvy[p]), cell);
        for (i32 dy = -2; dy <= 2; ++dy) for (i32 dx = -2; dx <= 2; ++dx) {
            const u32 ck = u32(i64(key) + i64(dy) * 65536 + i64(dx));
            const u32 run = find_run(&pr, ck);
            if (run == 0xFFFFFFFFu) continue;
            for (u32 t = pr.begin[run]; t < pr.end[run]; ++t) {
                const u32 j = pidx[t];
                if (j == p || !part_liquid(w, j)) continue;
                const vec2<pos_t> d = { pos_sub(w->px[j], w->px[p]), pos_sub(w->py[j], w->py[p]) };
                const pos_t reach = pos_add(hk, pos_add(trav_p, pos_min(travel_of(w, w->pvx[j], w->pvy[j]), cell)));
                if (!within_radius(d, reach)) continue;
                if (cnt == MAX_NEIGHBOURS) { w->nbr_overflow += 1; continue; }
                list[cnt++] = j;
            }
        }
        for (u32 a = 1; a < cnt; ++a) {   // insertion sort by j ascending
            const u32 v = list[a]; u32 b = a;
            while (b > 0 && list[b - 1] > v) { list[b] = list[b - 1]; b -= 1; }
            list[b] = v;
        }
        // compact into the flat array (list already lives at p*MAX; move down to `total`)
        if (total != u64(p) * MAX_NEIGHBOURS) { memmove(&w->nbr[total], list, u64(cnt) * sizeof(u32)); }
        total += cnt;
        if (cnt > w->max_neighbours_seen) w->max_neighbours_seen = cnt;
    }
    w->nbr_begin[np] = total;
    w->nbr_total = total;

    // -- coarse grid over bodies (step 5): every 1 m cell an AABB (expanded by the travel
    //    margin) covers -> (key, body) pairs; runs -> candidate pairs a < b; sort + unique --
    struct BodyBox { u32 cx0, cx1, cy0, cy1; pos_t margin; q_t s, c; };
    BodyBox* bb = tmp<BodyBox>(s, nb);
    u32 ncell = 0;
    for (u32 b = 0; b < nb; ++b) {
        sincos_of(w->bth[b], &bb[b].s, &bb[b].c);
        const pos_t ex = pos_add(pos_abs(pos_scale_q(w->bhw[b], bb[b].c)), pos_abs(pos_scale_q(w->bhh[b], bb[b].s)));
        const pos_t ey = pos_add(pos_abs(pos_scale_q(w->bhw[b], bb[b].s)), pos_abs(pos_scale_q(w->bhh[b], bb[b].c)));
        bb[b].margin = pos_add(w->k.contact_margin, travel_of(w, w->bvx[b], w->bvy[b]));
        const pos_t rx = pos_add(ex, bb[b].margin), ry = pos_add(ey, bb[b].margin);
        bb[b].cx0 = coarse_cx(pos_sub(w->bx[b], rx)); bb[b].cx1 = coarse_cx(pos_add(w->bx[b], rx));
        bb[b].cy0 = coarse_cx(pos_sub(w->by[b], ry)); bb[b].cy1 = coarse_cx(pos_add(w->by[b], ry));
        ncell += (bb[b].cx1 - bb[b].cx0 + 1u) * (bb[b].cy1 - bb[b].cy0 + 1u);
    }
    u32* bkey = tmp<u32>(s, ncell);
    u32* bval = tmp<u32>(s, ncell);
    u32 nk = 0;
    for (u32 b = 0; b < nb; ++b) {
        for (u32 cy = bb[b].cy0; cy <= bb[b].cy1; ++cy) {
            for (u32 cx = bb[b].cx0; cx <= bb[b].cx1; ++cx) { bkey[nk] = (cy << 16) | cx; bval[nk] = b; nk += 1; }
        }
    }
    sort_u32_kv(bkey, bval, nk, s);
    const Runs br = build_runs(s, bkey, nk);
    u32 npair_cap = 0;
    for (u32 r = 0; r < br.n; ++r) { const u32 m = br.end[r] - br.begin[r]; npair_cap += m * (m - 1u) / 2u; }
    u64* pairk = tmp<u64>(s, npair_cap);
    u32* pairv = tmp<u32>(s, npair_cap);
    u32 npair = 0;
    for (u32 r = 0; r < br.n; ++r) {
        for (u32 t = br.begin[r]; t < br.end[r]; ++t) {
            for (u32 u = t + 1; u < br.end[r]; ++u) {
                u32 a = bval[t], b = bval[u];
                if (a == b) continue;
                if (a > b) { const u32 x = a; a = b; b = x; }
                if (body_static(w, a) && body_static(w, b)) continue;
                pairk[npair] = (u64(a) << 32) | u64(b); pairv[npair] = npair; npair += 1;
            }
        }
    }
    sort_u64_kv(pairk, pairv, npair, s);
    // contacts: body-body [pair key up, unique], then particle-body [body slot up, particle up]
    for (u32 t = 0; t < npair; ++t) {
        if (t > 0 && pairk[t] == pairk[t - 1]) continue;
        gen_bb(w, u32(pairk[t] >> 32), u32(pairk[t] & 0xFFFFFFFFu));
    }
    for (u32 b = 0; b < nb; ++b) {
        // fine-cell range of the body's expanded AABB (4-texel cells): re-derive from the coarse box
        const pos_t ex = pos_add(pos_abs(pos_scale_q(w->bhw[b], bb[b].c)), pos_abs(pos_scale_q(w->bhh[b], bb[b].s)));
        const pos_t ey = pos_add(pos_abs(pos_scale_q(w->bhw[b], bb[b].s)), pos_abs(pos_scale_q(w->bhh[b], bb[b].c)));
        const pos_t rx = pos_add(ex, bb[b].margin), ry = pos_add(ey, bb[b].margin);
        const u32 fx0 = fine_cx(pos_sub(w->bx[b], rx)), fx1 = fine_cx(pos_add(w->bx[b], rx));
        const u32 fy0 = fine_cx(pos_sub(w->by[b], ry)), fy1 = fine_cx(pos_add(w->by[b], ry));
        for (u32 cy = fy0; cy <= fy1; ++cy) {
            for (u32 cx = fx0; cx <= fx1; ++cx) {
                const u32 run = find_run(&pr, (cy << 16) | cx);
                if (run == 0xFFFFFFFFu) continue;
                for (u32 t = pr.begin[run]; t < pr.end[run]; ++t) {
                    const u32 p = pidx[t];
                    if (!part_dynamic(w, p) && body_static(w, b)) continue;
                    gen_pb(w, p, b, bb[b].s, bb[b].c, bb[b].margin);
                }
            }
        }
    }
    // stable order: sort contacts by (kind, i, j) - the key is unique per (i, j, kind) except the
    // DEEPEST_K corners of one pair, whose relative order is the generation order (stable sort)
    {
        const u32 nc = w->nc;
        u64* ck = tmp<u64>(s, nc);
        u32* cv = tmp<u32>(s, nc);
        for (u32 c = 0; c < nc; ++c) { ck[c] = (u64(w->contacts[c].kind) << 62) | (u64(w->contacts[c].i) << 31) | u64(w->contacts[c].j); cv[c] = c; }
        sort_u64_kv(ck, cv, nc, s);
        Contact* sorted = tmp<Contact>(s, nc);
        for (u32 c = 0; c < nc; ++c) { sorted[c] = w->contacts[cv[c]]; }
        memcpy(w->contacts, sorted, u64(nc) * sizeof(Contact));
    }

    // -- colouring: dist colours are cached (world_load); contacts then density, greedy --
    {   // degree diagnostic: the carrier with the most constraints (a > 256 fatal below is a content question, TODO.md)
        u32* deg = tmp<u32>(s, np + nb);
        memset(deg, 0, u64(np + nb) * 4u);
        for (u32 d = 0; d < w->nd; ++d) { if (part_dynamic(w, w->da[d])) deg[w->da[d]] += 1; if (part_dynamic(w, w->db[d])) deg[w->db[d]] += 1; }
        for (u32 c = 0; c < w->nc; ++c) {
            const Contact* ct = &w->contacts[c];
            if (ct->kind == CK_PB) { if (part_dynamic(w, ct->i)) deg[ct->i] += 1; } else if (!body_static(w, ct->i)) deg[np + ct->i] += 1;
            if (!body_static(w, ct->j)) deg[np + ct->j] += 1;
        }
        u32 best = 0;
        for (u32 c = 1; c < np + nb; ++c) if (deg[c] > deg[best]) best = c;
        if (deg[best] > w->max_degree_seen) w->max_degree_seen = deg[best];
        if (deg[best] > 200) {
            fprintf(stderr, "gate0: carrier %s %u has %u constraints at tick %llu" "%c", best < np ? "particle" : "body", best < np ? best : best - np, deg[best], (unsigned long long)w->hdr->tick, 10);
        }
    }
    Colorer col; col.np = np;
    col.mask = tmp<u64>(s, (np + nb) * COLOR_WORDS);
    memset(col.mask, 0, u64(np + nb) * COLOR_WORDS * sizeof(u64));
    u32 n_colors = 0;
    for (u32 d = 0; d < w->nd; ++d) {
        u32 carriers[2]; u32 n = 0;
        if (part_dynamic(w, w->da[d])) carriers[n++] = w->da[d];
        if (part_dynamic(w, w->db[d])) carriers[n++] = w->db[d];
        mark(&col, carriers, n, w->dcolor[d]);
        if (w->dcolor[d] + 1u > n_colors) n_colors = w->dcolor[d] + 1u;
    }
    // Substep order: the density solve (substep_density, its own two Jacobi passes) runs before
    // the colour sweep of distance constraints and contacts, so a wall has the last word before
    // writeback - with density after contacts, a bottom particle crushed by the column was pushed
    // back into the floor after its contact had resolved it, and the column leaked through a 1 m
    // wall within a tick (measured; TODO.md, W2 gate0). Density constraints are not level items.
    for (u32 c = 0; c < w->nc; ++c) {
        Contact* ct = &w->contacts[c];
        u32 carriers[2]; u32 n = 0;
        if (ct->kind == CK_PB) { if (part_dynamic(w, ct->i)) carriers[n++] = ct->i; }
        else                   { if (!body_static(w, ct->i)) carriers[n++] = carrier_body(&col, ct->i); }
        if (!body_static(w, ct->j)) carriers[n++] = carrier_body(&col, ct->j);
        ct->color = lowest_free(&col, carriers, n);
        mark(&col, carriers, n, ct->color);
        if (ct->color + 1u > n_colors) n_colors = ct->color + 1u;
    }
    w->n_colors = n_colors;
    if (n_colors > w->max_colors_seen) w->max_colors_seen = n_colors;
    // level lists: count, prefix, fill - same stable order as the colouring walk
    w->level_begin = tmp<u32>(s, n_colors + 1u);
    for (u32 c = 0; c <= n_colors; ++c) w->level_begin[c] = 0;
    for (u32 d = 0; d < w->nd; ++d) w->level_begin[w->dcolor[d] + 1u] += 1;
    for (u32 c = 0; c < w->nc; ++c) w->level_begin[w->contacts[c].color + 1u] += 1;
    for (u32 c = 0; c < n_colors; ++c) w->level_begin[c + 1u] += w->level_begin[c];
    w->n_items = w->level_begin[n_colors];
    w->level_items = tmp<u32>(s, w->n_items);
    u32* fill = tmp<u32>(s, n_colors + 1u);
    for (u32 c = 0; c < n_colors; ++c) fill[c] = w->level_begin[c];
    for (u32 d = 0; d < w->nd; ++d) w->level_items[fill[w->dcolor[d]]++] = (LV_DIST << 30) | d;
    for (u32 c = 0; c < w->nc; ++c) w->level_items[fill[w->contacts[c].color]++] = (LV_CONTACT << 30) | c;
    return w->nc;
}

// --- S1 predict -----------------------------------------------------------------------------
void substep_predict(World* w, u32 s) {
    (void)s;
    const Consts* k = &w->k;
    for (u32 b = 0; b < w->nb; ++b) {
        w->xs_bx[b] = w->bx[b]; w->xs_by[b] = w->by[b]; w->xs_bth[b] = w->bth[b];
        if (body_static(w, b)) {
            w->xl_bx[b] = xl_from_pos(w->bx[b]); w->xl_by[b] = xl_from_pos(w->by[b]); w->thl[b] = thl_from_angle(w->bth[b]);
            w->vpre_bx[b] = vel_zero(); w->vpre_by[b] = vel_zero();
            continue;
        }
        const vel_t vx = vel_from_delta(pos_sub(w->bx[b], w->bpx[b]), k->inv_h, &w->sat_vel);
        const vel_t vy = vel_sub(vel_from_delta(pos_sub(w->by[b], w->bpy[b]), k->inv_h, &w->sat_vel), k->g_sub);
        w->vpre_bx[b] = vx; w->vpre_by[b] = vy;
        w->bpx[b] = w->bx[b]; w->bpy[b] = w->by[b];
        const i32 rx = k->ladder == 3 ? w->bres_x[b] : 0, ry = k->ladder == 3 ? w->bres_y[b] : 0;
        w->xl_bx[b] = local_add(xl_with_residual(w->bx[b], rx), xl_from_pos(predict_delta(vx, k->h)));
        w->xl_by[b] = local_add(xl_with_residual(w->by[b], ry), xl_from_pos(predict_delta(vy, k->h)));
        const omega_t om = omega_from_delta(ang_sub(w->bth[b], w->bpth[b]), k->inv_h, &w->sat_omega);
        w->bpth[b] = w->bth[b];
        w->thl[b] = local_add(thl_from_angle(w->bth[b]), thl_from_angle(ang_delta(om, k->h)));
    }
    for (u32 p = 0; p < w->np; ++p) {
        w->xs_px[p] = w->px[p]; w->xs_py[p] = w->py[p];
        w->dens_lam[p] = local_zero(); w->dens_active[p] = 0;
        if (!part_dynamic(w, p)) {
            w->xl_px[p] = xl_from_pos(w->px[p]); w->xl_py[p] = xl_from_pos(w->py[p]);
            w->vpre_px[p] = vel_zero(); w->vpre_py[p] = vel_zero();
            continue;
        }
        const vel_t vx = vel_from_delta(pos_sub(w->px[p], w->ppx[p]), k->inv_h, &w->sat_vel);
        const vel_t vy = vel_sub(vel_from_delta(pos_sub(w->py[p], w->ppy[p]), k->inv_h, &w->sat_vel), k->g_sub);
        w->vpre_px[p] = vx; w->vpre_py[p] = vy;
        w->ppx[p] = w->px[p]; w->ppy[p] = w->py[p];
        const i32 rx = k->ladder == 3 ? w->pres_x[p] : 0, ry = k->ladder == 3 ? w->pres_y[p] : 0;
        w->xl_px[p] = local_add(xl_with_residual(w->px[p], rx), xl_from_pos(predict_delta(vx, k->h)));
        w->xl_py[p] = local_add(xl_with_residual(w->py[p], ry), xl_from_pos(predict_delta(vy, k->h)));
    }
    for (u32 d = 0; d < w->nd; ++d) { w->dlam30[d] = local_zero(); w->dlam[d] = lam_zero(); }
    for (u32 c = 0; c < w->nc; ++c) { w->contacts[c].lam_n = local_zero(); w->contacts[c].lam_t = local_zero(); }
}

// --- S3 kernels -----------------------------------------------------------------------------
namespace {

void project_dist(World* w, u32 d) {
    const u32 a = w->da[d], b = w->db[d];
    const vec2<pos_t> xa = { pos_from_xl(w->xl_px[a]), pos_from_xl(w->xl_py[a]) };
    const vec2<pos_t> xb = { pos_from_xl(w->xl_px[b]), pos_from_xl(w->xl_py[b]) };
    const vec2<pos_t> dv = vsub(xa, xb);
    const pos_t l = length(dv);
    if (is_zero_pos(l)) return;
    const vec2<q_t> n = unit(dv);
    const pos_t C = pos_sub(l, w->drest[d]);
    const invmass_t wa = w_clamp(w->pinv_m[a], w->pinv_m[b]);
    const invmass_t wb = w_clamp(w->pinv_m[b], w->pinv_m[a]);
    const local_t den = den_of(wa, wb, w->dat[d]);
    if (den_is_zero(den)) return;
    const local_t num = num_of30(c30_from_pos(C), w->dat[d], w->dlam30[d]);
    const local_t dl = dlam_of(num, den, w->k.ladder, &w->sat_hits);
    w->dlam30[d] = local_add(w->dlam30[d], dl);
    const local_t ma = corr_mag(wa, dl, &w->corr_clamps), mb = corr_mag(wb, dl, &w->corr_clamps);
    w->xl_px[a] = local_add(w->xl_px[a], corr_comp(ma, n.x)); w->xl_py[a] = local_add(w->xl_py[a], corr_comp(ma, n.y));
    w->xl_px[b] = local_sub(w->xl_px[b], corr_comp(mb, n.x)); w->xl_py[b] = local_sub(w->xl_py[b], corr_comp(mb, n.y));
}

// The carrier-side of a contact: reads/writes through these so the particle and body cases
// share one projection body.
struct Side {
    local_t *xl, *yl, *thl;       // thl null for a particle
    pos_t xg, yg; angle_t thg;     // generation positions
    pos_t xs, ys; angle_t ths;     // substep start
    invmass_t w, inv_i;
    local_t w_ang;
    pos_t rn;
    bool dynamic;
};

Side side_of(World* w, const Contact* c, bool first) {
    Side s;
    if (first && c->kind == CK_PB) {
        const u32 p = c->i;
        s.xl = &w->xl_px[p]; s.yl = &w->xl_py[p]; s.thl = nullptr;
        s.xg = w->xg_px[p]; s.yg = w->xg_py[p]; s.thg = ang_zero();
        s.xs = w->xs_px[p]; s.ys = w->xs_py[p]; s.ths = ang_zero();
        s.w = c->wi; s.w_ang = local_zero(); s.inv_i = cvt_w(fx::fx_raw<::invmass_t>(0)); s.rn = c->rn_i;
        s.dynamic = part_dynamic(w, p);
        return s;
    }
    const u32 b = first ? c->i : c->j;
    s.xl = &w->xl_bx[b]; s.yl = &w->xl_by[b]; s.thl = &w->thl[b];
    s.xg = w->xg_bx[b]; s.yg = w->xg_by[b]; s.thg = w->xg_bth[b];
    s.xs = w->xs_bx[b]; s.ys = w->xs_by[b]; s.ths = w->xs_bth[b];
    s.w = first ? c->wi : c->wj; s.w_ang = first ? c->wi_ang : c->wj_ang; s.inv_i = w->binv_i[b];
    s.rn = first ? c->rn_i : c->rn_j;
    s.dynamic = !body_static(w, b);
    return s;
}

// (delta of the side since `from` positions) . n + rotation term, frac 30.
local_t side_motion_n(const World* w, const Side* s, pos_t fx_, pos_t fy_, angle_t fth, vec2<q_t> n, pos_t rn) {
    const local_t dx = local_sub(*s->xl, xl_from_pos(fx_));
    const local_t dy = local_sub(*s->yl, xl_from_pos(fy_));
    local_t m = dot30_n(dx, dy, n);
    if (s->thl != nullptr) {
        const local_t dth = local_sub(*s->thl, thl_from_angle(fth));
        m = local_add(m, rad_times_rn(rad30_from_turn30(dth, w->k.two_pi), rn));
    }
    return m;
}

void side_apply(World* w, Side* s, local_t dl, vec2<q_t> dir, pos_t rn, bool positive) {
    if (!s->dynamic) return;
    const local_t mag = corr_mag(s->w, dl, &w->corr_clamps);
    local_t cx = corr_comp(mag, dir.x), cy = corr_comp(mag, dir.y);
    local_t ca = local_zero();
    if (s->thl != nullptr) ca = ang_corr(s->inv_i, dl, rn, w->k.inv_two_pi);
    if (positive) { *s->xl = local_add(*s->xl, cx); *s->yl = local_add(*s->yl, cy); if (s->thl) *s->thl = local_add(*s->thl, ca); }
    else          { *s->xl = local_sub(*s->xl, cx); *s->yl = local_sub(*s->yl, cy); if (s->thl) *s->thl = local_sub(*s->thl, ca); }
}

void project_contact(World* w, u32 ci) {
    w->contact_evals += 1;
    Contact* c = &w->contacts[ci];
    Side si = side_of(w, c, true);
    Side sj = side_of(w, c, false);
    // linearised depth: depth_now = depth - (delta_rel since generation) . n  (ALLOY S3 (4))
    const local_t rel = local_sub(side_motion_n(w, &si, si.xg, si.yg, si.thg, c->n, c->rn_i),
                                  side_motion_n(w, &sj, sj.xg, sj.yg, sj.thg, c->n, c->rn_j));
    const local_t depth_now = local_sub(c30_from_pos(c->depth), rel);
    const pos_t C = pos_from_xl(local_neg(depth_now));     // C = -depth_now, rounded to pos_t (the kernel's C is a pos_t)
    if (!pos_lt(C, pos_zero()) && c->lam_n == local_zero()) return;   // separated and inactive
    const local_t den = den_add(den_of(si.w, sj.w, w->k.at_contact), den_add(si.w_ang, sj.w_ang));
    if (den_is_zero(den)) return;
    const local_t num = num_of30(c30_from_pos(C), w->k.at_contact, c->lam_n);
    local_t dl = dlam_of(num, den, w->k.ladder, &w->sat_hits);
    dl = local_max(dl, local_neg(c->lam_n));                 // unilateral: lambda >= 0
    c->lam_n = local_add(c->lam_n, dl);
    side_apply(w, &si, dl, c->n, c->rn_i, true);
    side_apply(w, &sj, dl, c->n, c->rn_j, false);
    if (!local_lt(local_zero(), c->lam_n)) return;
    // position-level Coulomb friction, docs/ALLOY.md §14.4.3 S3 (4) as written: dp_t = the
    // tangential part of ((xl_a - xs_a) - (xl_b - xs_b)) - the CARRIERS' (centres') relative
    // motion this substep, no lever term; lim = mu * |normal correction this step|; static:
    // cancel fully, dynamic: cancel lim; "split by w'" - a translational correction. (A
    // contact-POINT form with the rotational share was measured to inject a systematic
    // horizontal drift into an aligned stack through the per-corner GS order - the double
    // shadow drifted identically - so the doc's centre form is what the bench runs; TODO.md.)
    const vec2<q_t> t = perp(c->n);
    const local_t dpt = local_sub(dot30_n(local_sub(*si.xl, xl_from_pos(si.xs)), local_sub(*si.yl, xl_from_pos(si.ys)), t),
                                  dot30_n(local_sub(*sj.xl, xl_from_pos(sj.xs)), local_sub(*sj.yl, xl_from_pos(sj.ys)), t));
    if (dpt == local_zero()) return;
    const local_t lim = local_scale_q(local_abs(corr_mag(w_add(si.w, sj.w), dl, &w->corr_clamps)), w->k.mu);
    local_t target = local_neg(dpt);
    if (local_lt(lim, local_abs(dpt))) { target = local_lt(dpt, local_zero()) ? lim : local_neg(lim); }
    const local_t den_t = den_of(si.w, sj.w, w->k.at_contact);
    if (den_is_zero(den_t)) return;
    const local_t dlt = dlam_of(target, den_t, w->k.ladder, &w->sat_hits);
    c->lam_t = local_add(c->lam_t, dlt);
    side_apply(w, &si, dlt, t, pos_zero(), true);    // rn = 0: no angular share (the doc's split by w')
    side_apply(w, &sj, dlt, t, pos_zero(), false);
}

}  // namespace

// --- the PBF density solve (ALLOY S3 "density"), as TWO Jacobi passes over the neighbour lists ---
// Pass 1 (read-only): for every liquid particle i, rho_i from its neighbours at their current
// solver-local positions, grad C_i, den_i, and lambda_i = max(-C_i / den_i, 0) with C = ONE - rho
// (<= 0 when compressed; the unilateral clamp keeps lambda >= 0). The per-pair gradient terms
// kw |W'| n_ij are kept in scratch for pass 2. Pass 2 (owner-only write): dx_i = h * w_i *
// sum_j (lambda_i + lambda_j) kw |W'| n_ij - the standard PBF lambda_i + lambda_j form
// (Macklin & Mueller 2013), owner-only and deterministic (neighbour lists are ID-sorted, nothing
// is written until every lambda is known). The single-constraint "apply only lambda_i, the
// symmetric half when j is the owner" realisation of docs/ALLOY.md §14.4.3 drops the lambda_j
// cross terms: measured to launch a landing block at > 2 m/s in BOTH the fx and the double
// world (alpha~ = 0), and to need alpha~ >= 0.1 (10% density error) to hold - TODO.md (W2 gate0).
void substep_density(World* w) {
    const Consts* k = &w->k;
    const u32 np = w->np;
    for (u32 iter = 0; iter < k->density_iters; ++iter) {
    for (u32 i = 0; i < np; ++i) {
        w->dens_lam[i] = local_zero();
        w->dens_active[i] = 0;
        if (!part_liquid(w, i) || !part_dynamic(w, i)) continue;
        const vec2<pos_t> xi = { pos_from_xl(w->xl_px[i]), pos_from_xl(w->xl_py[i]) };
        local_t acc = rho_term(k->kw, q_one());            // self term W(0) = 1
        local_t gx = local_zero(), gy = local_zero(), denj = local_zero();
        const u32 b0 = w->nbr_begin[i], b1 = w->nbr_begin[i + 1];
        for (u32 t = b0; t < b1; ++t) {
            const u32 j = w->nbr[t];
            const vec2<pos_t> d = vsub(xi, { pos_from_xl(w->xl_px[j]), pos_from_xl(w->xl_py[j]) });
            w->pair_gx[t] = local_zero(); w->pair_gy[t] = local_zero();
            if (!within_radius(d, k->h_kernel)) continue;
            w->pair_evals += 1;
            // normalize-once (docs/GATE0-BENCH.md §7 R-3): one root + one reciprocal per pair.
            // The rev-1 body called length(d), then unit(d) (a SECOND isqrt64 + two divisions)
            // and div<q_t>(r, h) separately - measured 176 ns vs 91 ns for this form (TODO.md D2).
            PairGeom pg;
            const bool has_dir = pair_geom(d, k->h_kernel, k->q_shift, &pg);
            const q_t a = q_sub(q_one(), q_mul(pg.q, pg.q));
            const q_t W = q_mul(q_mul(a, a), a);
            acc = local_add(acc, rho_term(k->kw, W));
            if (!has_dir) continue;
            const q_t b = q_sub(q_one(), pg.q);
            const q_t dW = q_mul(b, b);
            const local_t tx = grad_term(k->kw, dW, pg.n.x), ty = grad_term(k->kw, dW, pg.n.y);
            w->pair_gx[t] = tx; w->pair_gy[t] = ty;
            gx = local_add(gx, tx); gy = local_add(gy, ty);
            denj = local_add(denj, den_grad(w->pinv_m[j], tx, ty));   // |grad C_j|^2 w_j: grad C_j = -kw|W'|n, same magnitude
        }
        const local_t rho = rho_round(acc);
        w->rho[i] = q_sat(rho);
        const local_t C = local_sub(local_one(), rho);           // <= 0 when compressed
        w->gcx[i] = gx; w->gcy[i] = gy;
        if (!local_lt(C, local_zero())) continue;               // not compressed: lambda stays 0
        const local_t den = den_add(den_add(den_grad(w->pinv_m[i], gx, gy), denj),
                                    den_of(cvt_w(fx::fx_raw<::invmass_t>(0)), cvt_w(fx::fx_raw<::invmass_t>(0)), k->at_density));
        if (den_is_zero(den)) continue;
        const local_t num = num_of30(C, k->at_density, local_zero());
        w->dens_lam[i] = local_max(dlam_of(num, den, k->ladder, &w->sat_hits), local_zero());
        w->dens_active[i] = 1;
    }
    for (u32 i = 0; i < np; ++i) {
        if (!part_liquid(w, i) || !part_dynamic(w, i)) continue;
        local_t sx = local_zero(), sy = local_zero();
        const u32 b0 = w->nbr_begin[i], b1 = w->nbr_begin[i + 1];
        for (u32 t = b0; t < b1; ++t) {
            const u32 j = w->nbr[t];
            const local_t l = local_add(w->dens_lam[i], w->dens_lam[j]);
            if (l == local_zero()) continue;
            sx = local_add(sx, corr_comp30(l, w->pair_gx[t]));
            sy = local_add(sy, corr_comp30(l, w->pair_gy[t]));
        }
        if (sx == local_zero() && sy == local_zero()) continue;
        // "scale back once": the gradient was taken on q = r / h, so the correction is x h_kernel
        const local_t cx = corr_clamp30(local_scale_pos(corr_mag_unbounded(w->pinv_m[i], sx), k->h_kernel), &w->corr_clamps);
        const local_t cy = corr_clamp30(local_scale_pos(corr_mag_unbounded(w->pinv_m[i], sy), k->h_kernel), &w->corr_clamps);
        if (w->debug_density && w->debug_prints < 60 && (cx > i64(fx::TEXEL.v) * 4096 || cx < -i64(fx::TEXEL.v) * 4096 || cy > i64(fx::TEXEL.v) * 4096 || cy < -i64(fx::TEXEL.v) * 4096)) {
            w->debug_prints += 1;
            fprintf(stderr, "  dens t=%llu i=%u rho=%lld lam=%lld gx=%lld gy=%lld sx=%lld sy=%lld dx=%lld dy=%lld" "%c", (unsigned long long)w->hdr->tick, i,
                    (long long)raw_local(local_from_q(w->rho[i])), (long long)raw_local(w->dens_lam[i]), (long long)raw_local(w->gcx[i]), (long long)raw_local(w->gcy[i]),
                    (long long)raw_local(sx), (long long)raw_local(sy), (long long)raw_local(cx), (long long)raw_local(cy), 10);
        }
        w->xl_px[i] = local_add(w->xl_px[i], cx);
        w->xl_py[i] = local_add(w->xl_py[i], cy);
    }
    }   // iterations: lambda is recomputed from the corrected positions each pass (XPBD would accumulate it; the doc's one pass makes the two identical)
}

void substep_project_color(World* w, u32 c) {
    TL_ASSERT(c < w->n_colors);
    for (u32 t = w->level_begin[c]; t < w->level_begin[c + 1]; ++t) {
        const u32 item = w->level_items[t];
        const u32 kind = item >> 30, idx = item & 0x3FFFFFFFu;
        if (kind == LV_DIST)         project_dist(w, idx);
        else if (kind == LV_CONTACT) project_contact(w, idx);
        else                         TL_FATAL("density constraints are not level items (substep_density)");
    }
}

// --- S4 writeback ---------------------------------------------------------------------------
void substep_writeback(World* w) {
    const Consts* k = &w->k;
    for (u32 d = 0; d < w->nd; ++d) w->dlam[d] = lam_narrow(w->dlam30[d], &w->sat_hits);   // the once-per-substep lambda narrowing (rung 1)
    for (u32 b = 0; b < w->nb; ++b) {
        if (body_static(w, b)) continue;
        const pos_t x = pos_from_xl(w->xl_bx[b]), y = pos_from_xl(w->xl_by[b]);
        w->bres_x[b] = k->ladder == 3 ? residual_of(w->xl_bx[b], x) : 0;
        w->bres_y[b] = k->ladder == 3 ? residual_of(w->xl_by[b], y) : 0;
        w->bx[b] = x; w->by[b] = y;
        w->bvx[b] = vel_from_delta(pos_sub(x, w->bpx[b]), k->inv_h, &w->sat_vel);
        w->bvy[b] = vel_from_delta(pos_sub(y, w->bpy[b]), k->inv_h, &w->sat_vel);
        const angle_t th = angle_from_thl(w->thl[b]);
        w->bth[b] = th;
        w->bw[b] = omega_from_delta(ang_sub(th, w->bpth[b]), k->inv_h, &w->sat_omega);
    }
    for (u32 p = 0; p < w->np; ++p) {
        if (!part_dynamic(w, p)) continue;
        const pos_t x = pos_from_xl(w->xl_px[p]), y = pos_from_xl(w->xl_py[p]);
        w->pres_x[p] = k->ladder == 3 ? residual_of(w->xl_px[p], x) : 0;
        w->pres_y[p] = k->ladder == 3 ? residual_of(w->xl_py[p], y) : 0;
        w->px[p] = x; w->py[p] = y;
        w->pvx[p] = vel_from_delta(pos_sub(x, w->ppx[p]), k->inv_h, &w->sat_vel);
        w->pvy[p] = vel_from_delta(pos_sub(y, w->ppy[p]), k->inv_h, &w->sat_vel);
    }
}

// --- S5 velocity pass -----------------------------------------------------------------------
void substep_velocity(World* w) {
    const Consts* k = &w->k;
    Scratch* s = w->scratch;
    TL_SCRATCH_SCOPE_BEGIN(s);
    local_t* vbx = tmp<local_t>(s, w->nb); local_t* vby = tmp<local_t>(s, w->nb);
    local_t* vpx = tmp<local_t>(s, w->np); local_t* vpy = tmp<local_t>(s, w->np);
    for (u32 b = 0; b < w->nb; ++b) { vbx[b] = vacc_from_vel(w->bvx[b]); vby[b] = vacc_from_vel(w->bvy[b]); }
    for (u32 p = 0; p < w->np; ++p) { vpx[p] = vacc_from_vel(w->pvx[p]); vpy[p] = vacc_from_vel(w->pvy[p]); }
    // restitution (Jacobi over the pre-pass v; e = 0 removes the residual normal approach)
    for (u32 ci = 0; ci < w->nc; ++ci) {
        const Contact* c = &w->contacts[ci];
        if (!local_lt(local_zero(), c->lam_n)) continue;
        vec2<vel_t> vi, vj, vpi, vpj;
        if (c->kind == CK_PB) { vi = { w->pvx[c->i], w->pvy[c->i] }; vpi = { w->vpre_px[c->i], w->vpre_py[c->i] }; }
        else                  { vi = { w->bvx[c->i], w->bvy[c->i] }; vpi = { w->vpre_bx[c->i], w->vpre_by[c->i] }; }
        vj = { w->bvx[c->j], w->bvy[c->j] }; vpj = { w->vpre_bx[c->j], w->vpre_by[c->j] };
        const vel_t vn = dot_vn({ vel_sub(vi.x, vj.x), vel_sub(vi.y, vj.y) }, c->n);
        const vel_t vn_prev = dot_vn({ vel_sub(vpi.x, vpj.x), vel_sub(vpi.y, vpj.y) }, c->n);
        if (!vel_lt(vn_prev, vel_neg(k->rest_vel_min))) continue;
        vel_t bounce = vel_scale_q(vel_neg(vn_prev), k->restitution);
        if (vel_lt(bounce, vel_zero())) bounce = vel_zero();
        const vel_t dvn = vel_add(vel_neg(vn), bounce);
        const invmass_t wsum = w_add(c->wi, c->wj);
        if (is_zero_w(wsum)) continue;
        const local_t dvl = dvl_of(dvn, wsum);
        if (c->kind == CK_PB) { if (part_dynamic(w, c->i)) { vpx[c->i] = local_add(vpx[c->i], vacc_term(c->wi, dvl, c->n.x)); vpy[c->i] = local_add(vpy[c->i], vacc_term(c->wi, dvl, c->n.y)); } }
        else                  { if (!body_static(w, c->i)) { vbx[c->i] = local_add(vbx[c->i], vacc_term(c->wi, dvl, c->n.x)); vby[c->i] = local_add(vby[c->i], vacc_term(c->wi, dvl, c->n.y)); } }
        if (!body_static(w, c->j)) { vbx[c->j] = local_sub(vbx[c->j], vacc_term(c->wj, dvl, c->n.x)); vby[c->j] = local_sub(vby[c->j], vacc_term(c->wj, dvl, c->n.y)); }
    }
    // XSPH [index up, nbr up]: reads v (Jacobi), kernel on the post-writeback positions
    for (u32 i = 0; i < w->np; ++i) {
        if (!part_liquid(w, i) || !part_dynamic(w, i)) continue;
        const vec2<pos_t> xi = { w->px[i], w->py[i] };
        for (u32 t = w->nbr_begin[i]; t < w->nbr_begin[i + 1]; ++t) {
            const u32 j = w->nbr[t];
            const vec2<pos_t> d = vsub(xi, { w->px[j], w->py[j] });
            if (!within_radius(d, k->h_kernel)) continue;
            w->pair_evals += 1;
            const q_t q = q_of_r(length(d), k->h_kernel, k->q_shift);   // one root, no division (exact shift; §7 R-3)
            const q_t a = q_sub(q_one(), q_mul(q, q));
            const q_t W = q_mul(q_mul(a, a), a);
            vpx[i] = local_add(vpx[i], xsph_term(k->c_visc, W, vel_sub(w->pvx[j], w->pvx[i])));
            vpy[i] = local_add(vpy[i], xsph_term(k->c_visc, W, vel_sub(w->pvy[j], w->pvy[i])));
        }
    }
    // v_final, then px = x - v_final * h (the implicit encoding; x untouched)
    for (u32 b = 0; b < w->nb; ++b) {
        if (body_static(w, b)) continue;
        w->bvx[b] = vel_clamp_vmax(vel_from_vacc(vbx[b], &w->sat_vel), &w->vmax_clamps); w->bvy[b] = vel_clamp_vmax(vel_from_vacc(vby[b], &w->sat_vel), &w->vmax_clamps);
        w->bpx[b] = pos_sub(w->bx[b], predict_delta(w->bvx[b], k->h));
        w->bpy[b] = pos_sub(w->by[b], predict_delta(w->bvy[b], k->h));
        w->bpth[b] = ang_sub(w->bth[b], ang_delta(w->bw[b], k->h));
    }
    for (u32 p = 0; p < w->np; ++p) {
        if (!part_dynamic(w, p)) continue;
        w->pvx[p] = vel_clamp_vmax(vel_from_vacc(vpx[p], &w->sat_vel), &w->vmax_clamps); w->pvy[p] = vel_clamp_vmax(vel_from_vacc(vpy[p], &w->sat_vel), &w->vmax_clamps);
        w->ppx[p] = pos_sub(w->px[p], predict_delta(w->pvx[p], k->h));
        w->ppy[p] = pos_sub(w->py[p], predict_delta(w->pvy[p], k->h));
    }
    TL_SCRATCH_SCOPE_END(s);
}

void tick_end(World* w) {
    w->hdr->tick += 1;
    TL_SCRATCH_SCOPE_END(w->scratch);
    w->contacts = nullptr; w->nc = 0;
}

void world_tick(World* w) {
    tick_begin(w);
    for (u32 s = 0; s < w->k.substeps; ++s) {
        substep_predict(w, s);
        substep_density(w);
        for (u32 c = 0; c < w->n_colors; ++c) substep_project_color(w, c);
        substep_writeback(w);
        substep_velocity(w);
    }
    tick_end(w);
}

u64 world_hash(const World* w, u64 per_arena_out[MAX_ARENAS]) {
    return registry_hash_all(&w->reg, per_arena_out);
}

// --- metric probes --------------------------------------------------------------------------
pos_t world_max_penetration(const World* w, u8* tunnel) {
    pos_t pen = pos_zero();
    *tunnel = 0;
    const pos_t texel = cvt_pos(fx::TEXEL);
    for (u32 s = 0; s < w->nb; ++s) {
        if (!body_static(w, s)) continue;
        q_t ss, cs; sincos_of(w->bth[s], &ss, &cs);
        const vec2<pos_t> xs = { w->bx[s], w->by[s] };
        for (u32 b = 0; b < w->nb; ++b) {
            if (body_static(w, b)) continue;
            q_t sb, cb; sincos_of(w->bth[b], &sb, &cb);
            const vec2<pos_t> xb = { w->bx[b], w->by[b] };
            const pos_t hw = w->bhw[b], hh = w->bhh[b];
            const vec2<pos_t> corners[4] = { { hw, hh }, { pos_neg(hw), hh }, { pos_neg(hw), pos_neg(hh) }, { hw, pos_neg(hh) } };
            vec2<q_t> nl;
            for (u32 k = 0; k < 4; ++k) {
                const vec2<pos_t> l = rot(vsub(vadd(xb, rot(corners[k], sb, cb)), xs), q_neg(ss), cs);
                const pos_t sd = box_sdf(l, w->bhw[s], w->bhh[s], rot(vsub(xb, xs), q_neg(ss), cs), &nl);
                pen = pos_max(pen, pos_neg(sd));
            }
            const vec2<pos_t> lc = rot(vsub(xb, xs), q_neg(ss), cs);
            const pos_t sdc = box_sdf(lc, w->bhw[s], w->bhh[s], lc, &nl);
            if (pos_lt(sdc, pos_neg(texel))) *tunnel = 1;
        }
        for (u32 p = 0; p < w->np; ++p) {
            if (!part_dynamic(w, p)) continue;
            vec2<q_t> nl;
            const vec2<pos_t> lp = rot(vsub({ w->px[p], w->py[p] }, xs), q_neg(ss), cs);
            const pos_t sd = box_sdf(lp, w->bhw[s], w->bhh[s], lp, &nl);
            pen = pos_max(pen, pos_neg(sd));
            if (pos_lt(sd, pos_neg(texel))) *tunnel = 1;
        }
    }
    return pen;
}

pos_t world_body_penetration(const World* w, u32 body, u8* tunnel) {
    TL_ASSERT(body < w->nb && !body_static(w, body));
    pos_t pen = pos_zero();
    *tunnel = 0;
    const pos_t texel = cvt_pos(fx::TEXEL);
    q_t sb, cb; sincos_of(w->bth[body], &sb, &cb);
    const vec2<pos_t> xb = { w->bx[body], w->by[body] };
    const pos_t hw = w->bhw[body], hh = w->bhh[body];
    const vec2<pos_t> corners[4] = { { hw, hh }, { pos_neg(hw), hh }, { pos_neg(hw), pos_neg(hh) }, { hw, pos_neg(hh) } };
    for (u32 o = 0; o < w->nb; ++o) {
        if (o == body) continue;
        q_t so, co; sincos_of(w->bth[o], &so, &co);
        const vec2<pos_t> xo = { w->bx[o], w->by[o] };
        const vec2<pos_t> hint = rot(vsub(xb, xo), q_neg(so), co);
        vec2<q_t> nl;
        for (u32 k = 0; k < 4; ++k) {
            const vec2<pos_t> l = rot(vsub(vadd(xb, rot(corners[k], sb, cb)), xo), q_neg(so), co);
            const pos_t sd = box_sdf(l, w->bhw[o], w->bhh[o], hint, &nl);
            pen = pos_max(pen, pos_neg(sd));
        }
        const pos_t sdc = box_sdf(hint, w->bhw[o], w->bhh[o], hint, &nl);
        if (pos_lt(sdc, pos_neg(texel))) *tunnel = 1;
    }
    return pen;
}

i64 world_energy_cj(const World* w, i64* ke_out, i64* pe_out) {
    i64 ke = 0, pe = 0;
    for (u32 b = 0; b < w->nb; ++b) {
        if (body_static(w, b)) continue;
        const i64 vx = raw_vel(w->bvx[b]), vy = raw_vel(w->bvy[b]);
        const i64 v2 = fx::sat_add(fx::sat_mul(vx, vx), fx::sat_mul(vy, vy));                    // frac 40
        ke = fx::sat_add(ke, fx::rne_shr(fx::sat_mul(v2, i64(w->bmass[b]) * 50), 40));          // centijoules
        pe = fx::sat_add(pe, fx::rne_shr(fx::sat_mul(raw_pos(w->by[b]), i64(w->bmass[b]) * 981), 18));
    }
    for (u32 p = 0; p < w->np; ++p) {
        if (!part_dynamic(w, p)) continue;
        const i64 vx = raw_vel(w->pvx[p]), vy = raw_vel(w->pvy[p]);
        const i64 v2 = fx::sat_add(fx::sat_mul(vx, vx), fx::sat_mul(vy, vy));
        ke = fx::sat_add(ke, fx::rne_shr(fx::sat_mul(v2, i64(w->pmass[p]) * 50), 40));
        pe = fx::sat_add(pe, fx::rne_shr(fx::sat_mul(raw_pos(w->py[p]), i64(w->pmass[p]) * 981), 18));
    }
    *ke_out = ke; *pe_out = pe;
    return fx::sat_add(ke, pe);
}

}  // namespace G0_NS
