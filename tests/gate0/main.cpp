// tl_gate0 - the Gate 0 bench CLI (docs/GATE0-BENCH.md §8.1): scenario dispatch, the per-tick
// CSV, the verdict lines, the run-twice hash compare, the FLOAT-SHADOW run. Tests are not sim
// code: printf-class io/clock/filesystem is the exemption of docs/TESTING.md §8 R-2.
//
//   tl_gate0 --scenario G01|G02|G03|G03b|G04|G05|G06|all --substeps 4|8|16 [--particles n] [--ticks n]
//            [--ladder 0|1|2|3] --out dir [--shadow] [--once] [--csv-every n] [--seed n] [--dump] [--perturb q]
//
// Exit codes: 0 = every requested verdict is PASS or INVESTIGATE; 3 = a FAIL verdict or a G-06
// divergence; 1 = a usage error. Verdict lines (docs/GATE0-BENCH.md §8.1):
//   VERDICT G-01 substeps=8 PASS|INVESTIGATE|FAIL <metric>=<value> ...
// Per-tick CSV (docs/GATE0-BENCH.md §4 + §7 R-2): tick, substeps, scenario, jitter_p95_texel,
// max_penetration_texel, density_err_p95, energy_i64, solve_us, broadphase_us, hash_lo64.
// Units in the CSV: texel columns are fixed-point with 4 decimals (raw / TEXEL); density_err_p95
// is a fraction with 6 decimals; energy_i64 is the documented KE>>40 + PE>>18 sum with mass in
// 1/4096 kg quanta and g = 9.81, i.e. raw units of 1/409600 J (docs/GATE0-BENCH.md §8.4).
#include "gate0/scene.h"
#include "gate0/solver_fx.h"
#if TL_GATE0_HAS_SHADOW
#include "gate0/shadow.h"
#endif
#include "foundation/scratch.h"
#include "foundation/sort.h"
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>

namespace {

// --- CLI ------------------------------------------------------------------------------------
struct Args {
    const char* scenario;
    u32 substeps;
    u32 particles;      // 0 = the scenario's default
    u64 ticks;          // 0 = the scenario's default
    u32 ladder;
    const char* out;
    u8 shadow;
    u8 once;            // skip the second (bit-compare) run
    u32 csv_every;
    u64 seed;
    u8 dump;            // per-tick body columns to stderr (debugging aid)
    u8 _pad0[3];
    u32 watch;          // --watch n: the shadow dump prints particle n in both worlds every tick (0xFFFFFFFF = none)
    u32 mu_percent;     // friction coefficient x 100 (50 = the bench default; a debugging knob)
    u32 alpha_nano;     // PBF density compliance alpha x 1e9 (physical; alpha~ = alpha / h^2); default 1302 = alpha~ 0.3 at 480 Hz
    u32 iters;          // density Jacobi passes per substep (1 = the doc)
};

u32 g_perturb = 0;   // --perturb q: G-01's odd stack boxes start q pos_t quanta off-axis (W2 gate0 review: proves the jitter metric can move)

// Strict decimal parser (LESSONS.md: atoll answers 0 for "abc"): every char a digit, no empty.
bool parse_u64(const char* s, u64* out) {
    if (s == nullptr || *s == 0) return false;
    u64 v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const u64 d = u64(*p - '0');
        if (v > (~u64(0) - d) / 10u) return false;
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

void usage() {
    fprintf(stderr,
        "tl_gate0 --scenario G01|G02|G03|G03b|G04|G05|G06|all --substeps 4|8|16 [--particles n] [--ticks n]\n"
        "         [--ladder 0|1|2|3] --out dir [--shadow] [--once] [--csv-every n] [--seed n] [--dump] [--perturb q]\n"
        "(docs/GATE0-BENCH.md section 8.1)\n");
}

bool parse_args(int argc, char** argv, Args* a) {
    memset(a, 0, sizeof(*a));
    a->substeps = 8; a->ladder = 1; a->csv_every = 1; a->seed = 1; a->mu_percent = 50; a->alpha_nano = 1302; a->iters = 1; a->watch = 0xFFFFFFFFu;
    for (int i = 1; i < argc; ++i) {
        const char* f = argv[i];
        const bool has_val = i + 1 < argc;
        u64 v = 0;
        if (!strcmp(f, "--scenario") && has_val) { a->scenario = argv[++i]; }
        else if (!strcmp(f, "--substeps") && has_val && parse_u64(argv[++i], &v) && v >= 1 && v <= 64) { a->substeps = u32(v); }
        else if (!strcmp(f, "--particles") && has_val && parse_u64(argv[++i], &v) && v >= 1 && v <= 1000000) { a->particles = u32(v); }
        else if (!strcmp(f, "--ticks") && has_val && parse_u64(argv[++i], &v) && v >= 1) { a->ticks = v; }
        else if (!strcmp(f, "--ladder") && has_val && parse_u64(argv[++i], &v) && v <= 3) { a->ladder = u32(v); }
        else if (!strcmp(f, "--out") && has_val) { a->out = argv[++i]; }
        else if (!strcmp(f, "--shadow")) { a->shadow = 1; }
        else if (!strcmp(f, "--once")) { a->once = 1; }
        else if (!strcmp(f, "--dump")) { a->dump = 1; }
        else if (!strcmp(f, "--mu") && has_val && parse_u64(argv[++i], &v) && v <= 200) { a->mu_percent = u32(v); }
        else if (!strcmp(f, "--alpha") && has_val && parse_u64(argv[++i], &v) && v <= 8000) { a->alpha_nano = u32(v); }
        else if (!strcmp(f, "--iters") && has_val && parse_u64(argv[++i], &v) && v >= 1 && v <= 16) { a->iters = u32(v); }
        else if (!strcmp(f, "--watch") && has_val && parse_u64(argv[++i], &v)) { a->watch = u32(v); }
        else if (!strcmp(f, "--perturb") && has_val && parse_u64(argv[++i], &v) && v <= 1000000) { g_perturb = u32(v); }
        else if (!strcmp(f, "--csv-every") && has_val && parse_u64(argv[++i], &v) && v >= 1) { a->csv_every = u32(v); }
        else if (!strcmp(f, "--seed") && has_val && parse_u64(argv[++i], &v)) { a->seed = v; }
        else { fprintf(stderr, "tl_gate0: bad flag or value at '%s'\n", f); return false; }
    }
    if (a->scenario == nullptr || a->out == nullptr) { fprintf(stderr, "tl_gate0: --scenario and --out are required\n"); return false; }
    return true;
}

// --- the boot: headless platform (vmem + clock), one scratch --------------------------------
struct Boot {
    const PlatformApi* api;
    Scratch scratch;
    VMemArena scene_arena;
};

Boot boot() {
    Boot b;
    PlatformConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.title = StrView{ "tl_gate0", 8 }; cfg.org = StrView{ "tidelock", 8 }; cfg.app = StrView{ "gate0", 5 };
    cfg.event_ring_cap_log2 = 4;
    b.api = platform_headless_init(&cfg);
    TL_CHECK(b.api != nullptr);
    TL_CHECK(scratch_init(&b.scratch, "gate0.scratch"_id, u64(4) << 30, &b.api->vmem) == ERR_OK);
    TL_CHECK(vmem_arena_init(&b.scene_arena, "gate0.scene"_id, u64(1) << 30, 0u, &b.api->vmem) == ERR_OK);
    return b;
}

u64 now_us(const PlatformApi* api) {
    const u64 t = api->clock.ticks(api->clock.ctx);
    const u64 f = api->clock.frequency(api->clock.ctx);
    return (t / f) * 1000000ull + ((t % f) * 1000000ull) / f;
}

// --- percentiles over u32 samples (radix sort in scratch; nearest-rank) --------------------
u32 percentile(u32* samples, u32 n, u32 pct, Scratch* s) {
    if (n == 0) return 0;
    TL_SCRATCH_SCOPE_BEGIN(s);
    u32* keys = (u32*)scratch_push(s, u64(n) * 4u + 16u, 16u);
    u32* vals = (u32*)scratch_push(s, u64(n) * 4u + 16u, 16u);
    memcpy(keys, samples, u64(n) * 4u);
    for (u32 i = 0; i < n; ++i) vals[i] = i;
    sort_u32_kv(keys, vals, n, s);
    u32 rank = (u32)((u64(n) * pct + 99u) / 100u);   // ceil(n * p / 100), nearest-rank
    if (rank == 0) rank = 1;
    const u32 r = keys[rank - 1];
    TL_SCRATCH_SCOPE_END(s);
    return r;
}
u64 percentile64(u64* samples, u32 n, u32 pct, Scratch* s) {
    // solve_us fits u32 for any sane tick; clamp so the radix sort applies
    TL_SCRATCH_SCOPE_BEGIN(s);
    u32* v = (u32*)scratch_push(s, u64(n) * 4u + 16u, 16u);
    for (u32 i = 0; i < n; ++i) v[i] = samples[i] > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(samples[i]);
    const u32 r = percentile(v, n, pct, s);
    TL_SCRATCH_SCOPE_END(s);
    return r;
}

// texel units with 4 decimals from a pos_t raw value (TEXEL.v = 2^14)
void fmt_texel(char* buf, u32 cap, i64 raw) {
    const i64 a = raw < 0 ? -raw : raw;
    const i64 whole = a / fx::TEXEL.v;
    const i64 frac = ((a % fx::TEXEL.v) * 10000 + fx::TEXEL.v / 2) / fx::TEXEL.v;
    snprintf(buf, cap, "%s%lld.%04lld", raw < 0 ? "-" : "", (long long)whole, (long long)(frac >= 10000 ? 9999 : frac));
}
// fraction with 6 decimals from a q_t raw value
void fmt_q(char* buf, u32 cap, i64 raw) {
    const i64 a = raw < 0 ? -raw : raw;
    const i64 whole = a / q_t::ONE;
    const i64 frac = ((a % q_t::ONE) * 1000000 + q_t::ONE / 2) / q_t::ONE;
    snprintf(buf, cap, "%s%lld.%06lld", raw < 0 ? "-" : "", (long long)whole, (long long)(frac >= 1000000 ? 999999 : frac));
}

// --- one scenario run: builds the scene, steps, records, judges -----------------------------
struct RunOut {
    u64* hash;            // per-tick world hash (scratch, `ticks` long)
    u32 verdict;          // 0 PASS, 1 INVESTIGATE, 2 FAIL
    char detail[512];
    u64 solve_p50, solve_p95, solve_p99;   // over ticks 200..ticks_run (warm-up excluded), solve + broadphase
    u32 max_colors, sat_hits, nbr_overflow, max_nbr, sat_vel, sat_omega, corr_clamps, vmax_clamps, max_degree;
    u64 pair_evals, contact_evals, ticks_run;   // ticks_run = ticks actually stepped (< ticks when the run stopped on an escape)
    u64 us_broadphase, us_predict, us_density, us_colors, us_writeback, us_velocity;   // per-phase totals over the run (W2 gate0 review: the G-05 cost accounting)
    u8 escaped;           // a dynamic carrier left the sealed box: tunneling, the run stopped early
    u8 _pad[7];
};

enum { V_PASS = 0, V_INVESTIGATE = 1, V_FAIL = 2 };
u8 g_dump = 0;   // set from --dump before any run (a test-side switch, never sim state)
const char* VNAME[3] = { "PASS", "INVESTIGATE", "FAIL" };

struct ScenarioDesc { const char* id; const char* csv_name; u64 default_ticks; u8 has_particles; };

void build_scene(g0scene::Scene* sc, const char* name, u64 seed, u32 particles, VMemArena* arena) {
    if (!strcmp(name, "G01"))       { g0scene::scene_init(sc, arena, 16, 1, 1); g0scene::scene_g01(sc, seed, particles);
                                      if (g_perturb) { u32 k = 0; for (u32 b = 0; b < sc->nb; ++b) { if (sc->bodies[b].inv_m.v == 0) continue; if (k & 1u) sc->bodies[b].x.v += i32(g_perturb); k += 1; } } }
    else if (!strcmp(name, "G02a")) { g0scene::scene_init(sc, arena, 16, 1, 1); g0scene::scene_g02a(sc, seed); }
    else if (!strcmp(name, "G02b")) { g0scene::scene_init(sc, arena, 16, 1, 1); g0scene::scene_g02b(sc, seed); }
    else if (!strcmp(name, "G03"))  { g0scene::scene_init(sc, arena, 8, particles, 1); g0scene::scene_g03(sc, seed, particles); }
    else if (!strcmp(name, "G04"))  { g0scene::scene_init(sc, arena, 64, 2200, 128); g0scene::scene_g04(sc, seed); }
    else if (!strcmp(name, "G05"))  { g0scene::scene_init(sc, arena, 2100, particles, 1); g0scene::scene_g05(sc, seed, particles); }
    else { TL_FATAL("gate0: unknown scene"); }
}

// Steps `ticks` ticks of `sc`, writes CSV rows (every `csv_every`), fills `out`. `judge` selects
// the verdict rule: 1 G-01, 2 G-02, 3 G-03, 4 G-04, 5 G-05 (timing only).
void run_scenario(Boot* bt, const g0scene::Scene* sc, const char* csv_name, u32 judge, u32 substeps, u32 ladder, u32 mu_percent, u32 alpha_nano, u32 iters,
                  u64 ticks, u32 csv_every, FILE* csv, RunOut* out) {
    Scratch* s = &bt->scratch;
    const VMemApi* os = &bt->api->vmem;
    g0::Consts k = g0::consts_make(substeps, ladder, mu_percent, alpha_nano, iters);
    g0::World* w = (g0::World*)scratch_push(s, sizeof(g0::World), 64u);
    g0::world_init(w, os, s, sc->nb, sc->np, sc->nd, &k);
    g0::world_load(w, sc);
    w->debug_density = g_dump;
    const u32 nt = u32(ticks);
    out->hash = (u64*)scratch_push(s, u64(nt) * 8u + 16u, 16u);
    u64* cost = (u64*)scratch_push(s, u64(nt) * 8u + 16u, 16u);
    // metric state
    pos_t* prev_bx = (pos_t*)scratch_push(s, u64(sc->nb) * 4u + 16u, 16u);
    pos_t* prev_by = (pos_t*)scratch_push(s, u64(sc->nb) * 4u + 16u, 16u);
    u32* jitter_all = (u32*)scratch_push(s, u64(nt) * sc->nb * 4u + 16u, 16u);   // texel*1e4 per (tick, body) after settle
    u32 n_jit = 0;
    u32* per_tick_j = (u32*)scratch_push(s, u64(sc->nb) * 4u + 16u, 16u);
    u32* per_tick_d = (u32*)scratch_push(s, u64(sc->np) * 4u + 16u, 16u);
    u32 pop = 0; i64 top_y_at_settle = 0, top_y_end = 0;
    i64 pen_run = 0; i64 pen_sustained_max = 0; u8 tunnel_any = 0; u8 escaped = 0;
    // §7 R-5 graded-body metrics (judge 2 with sc->graded_body set): the boulder's own
    // penetration/tunneling; every other carrier's escape is recorded, not graded.
    i64 gpen_run = 0, gpen_sustained_max = 0; u8 gtunnel_any = 0;
    u32 escape_body = 0xFFFFFFFFu;   // the first escaping BODY (a particle escape leaves it NONE; its index is on the stderr line)
    u32 dens_worst_p95 = 0;                    // max over post-settle ticks of the per-tick p95 (q_t raw)
    // energy windows (1k ticks): windowed max non-increasing (G-04), KE windows for boiling (G-03)
    i64 win_max = INT64_MIN, prev_win_max = INT64_MIN, e_initial = 0, e_max = INT64_MIN; u32 env_increases = 0;
    i64 ke_win_max = INT64_MIN, ke_prev_win_max = INT64_MIN; u32 ke_increases = 0;
    const u32 WIN = 1000;
    // intra-tick probe (G-01): the largest body displacement between two consecutive SUBSTEP
    // writebacks after settle, in texel*1e4. The per-tick jitter metric cannot see an oscillation
    // whose period divides a tick; this can (W2 gate0 review).
    pos_t* sub_bx = (pos_t*)scratch_push(s, u64(sc->nb) * 4u + 16u, 16u);
    pos_t* sub_by = (pos_t*)scratch_push(s, u64(sc->nb) * 4u + 16u, 16u);
    u64 intra_max = 0;
    u64 us_predict = 0, us_density = 0, us_colors = 0, us_writeback = 0, us_velocity = 0, us_broadphase = 0;
    u32 ticks_run = 0;
    for (u32 b = 0; b < sc->nb; ++b) { prev_bx[b] = w->bx[b]; prev_by[b] = w->by[b]; }
    for (u32 t = 0; t < nt; ++t) {
        const u64 t0 = now_us(bt->api);
        g0::tick_begin(w);
        const u64 t1 = now_us(bt->api);
        for (u32 b = 0; b < sc->nb; ++b) { sub_bx[b] = w->bx[b]; sub_by[b] = w->by[b]; }
        for (u32 ss = 0; ss < substeps; ++ss) {
            const u64 p0 = now_us(bt->api);
            g0::substep_predict(w, ss);
            const u64 p1 = now_us(bt->api);
            g0::substep_density(w);
            const u64 p2 = now_us(bt->api);
            for (u32 c = 0; c < w->n_colors; ++c) g0::substep_project_color(w, c);
            const u64 p3 = now_us(bt->api);
            g0::substep_writeback(w);
            const u64 p4 = now_us(bt->api);
            g0::substep_velocity(w);
            const u64 p5 = now_us(bt->api);
            us_predict += p1 - p0; us_density += p2 - p1; us_colors += p3 - p2; us_writeback += p4 - p3; us_velocity += p5 - p4;
            if (judge == 1 && t > sc->settle_tick) {
                for (u32 b = 0; b < sc->nb; ++b) {
                    if (w->bflags[b] & g0::BF_STATIC) continue;
                    const fx::vec2<pos_t> d = { w->bx[b] - sub_bx[b], w->by[b] - sub_by[b] };
                    const u64 tx = (u64(fx::len(d).v) * 10000u) / u64(fx::TEXEL.v);
                    if (tx > intra_max) intra_max = tx;
                    sub_bx[b] = w->bx[b]; sub_by[b] = w->by[b];
                }
            }
        }
        const u64 t2 = now_us(bt->api);
        us_broadphase += t1 - t0;
        ticks_run = t + 1u;
        // ---- metrics (fx world, after the substeps, before tick_end frees the transients) ----
        u32 jit_p95 = 0;
        {
            u32 nb_dyn = 0;
            for (u32 b = 0; b < sc->nb; ++b) {
                if (w->bflags[b] & g0::BF_STATIC) continue;
                const fx::vec2<pos_t> d = { w->bx[b] - prev_bx[b], w->by[b] - prev_by[b] };
                const i64 dl = fx::len(d).v;
                const u32 tx = u32((dl * 10000) / fx::TEXEL.v > 0xFFFFFFFF ? 0xFFFFFFFF : (dl * 10000) / fx::TEXEL.v);
                per_tick_j[nb_dyn++] = tx;
                if (t > sc->settle_tick) { jitter_all[n_jit++] = tx; if (dl > fx::TEXEL.v) pop = 1; }
                prev_bx[b] = w->bx[b]; prev_by[b] = w->by[b];
            }
            jit_p95 = percentile(per_tick_j, nb_dyn, 95, s);
        }
        u8 tunnel = 0;
        const pos_t pen = g0::world_max_penetration(w, &tunnel);
        if (judge == 2 && sc->graded_body != 0xFFFFFFFFu) {
            u8 gt = 0;
            const pos_t gpen = g0::world_body_penetration(w, sc->graded_body, &gt);
            if (gt) gtunnel_any = 1;
            if (gpen.v > 0) { gpen_run += 1; if (gpen_run > 3 && gpen.v > gpen_sustained_max) gpen_sustained_max = gpen.v; } else gpen_run = 0;
        }
        {   // a carrier outside the sealed box went THROUGH a wall: tunneling, and the grid keys would wrap - stop the run
            const i64 m = fx::TEXEL.v * 4;
            for (u32 b = 0; b < sc->nb && !escaped; ++b) {
                if (w->bflags[b] & g0::BF_STATIC) continue;
                if (w->bx[b].v < sc->box_lo_x.v - m || w->bx[b].v > sc->box_hi_x.v + m || w->by[b].v < sc->box_lo_y.v - m || w->by[b].v > sc->box_hi_y.v + m) { escaped = 1; escape_body = b; }
            }
            for (u32 p = 0; p < sc->np && !escaped; ++p) {
                if (w->px[p].v < sc->box_lo_x.v - m || w->px[p].v > sc->box_hi_x.v + m || w->py[p].v < sc->box_lo_y.v - m || w->py[p].v > sc->box_hi_y.v + m) {
                    escaped = 1;
                    fprintf(stderr, "gate0: particle %u escaped at x=%lld y=%lld vx=%lld vy=%lld rho=%lld" "%c", p, (long long)w->px[p].v, (long long)w->py[p].v,
                            (long long)w->pvx[p].v, (long long)w->pvy[p].v, (long long)w->rho[p].v, 10);
                }
            }
            if (escaped) tunnel = 1;
        }
        if (tunnel) tunnel_any = 1;
        if (pen.v > 0) { pen_run += 1; if (pen_run > 3 && pen.v > pen_sustained_max) pen_sustained_max = pen.v; } else pen_run = 0;
        u32 dens_p95 = 0;
        if (judge == 3 || judge == 4) {
            u32 n = 0;
            for (u32 p = 0; p < sc->np; ++p) {
                if (!(w->pflags[p] & g0::PF_LIQUID)) continue;
                const i64 e = i64(w->rho[p].v) - q_t::ONE;   // one-sided: rho < rho0 at a wall/free surface is the unilateral constraint's design
                per_tick_d[n++] = u32(e < 0 ? 0 : e);
            }
            dens_p95 = percentile(per_tick_d, n, 95, s);
            if (t > sc->settle_tick && dens_p95 > dens_worst_p95) dens_worst_p95 = dens_p95;
        }
        i64 ke = 0, pe = 0;
        const i64 e = g0::world_energy_cj(w, &ke, &pe);
        if (t == 0) e_initial = e;
        if (e > e_max) e_max = e;
        if (e > win_max) win_max = e;
        if (ke > ke_win_max) ke_win_max = ke;
        if ((t + 1u) % WIN == 0) {
            if (prev_win_max != INT64_MIN && win_max > prev_win_max) env_increases += 1;
            prev_win_max = win_max; win_max = INT64_MIN;
            if (t > sc->settle_tick) { if (ke_prev_win_max != INT64_MIN && ke_win_max > ke_prev_win_max) ke_increases += 1; ke_prev_win_max = ke_win_max; }
            ke_win_max = INT64_MIN;
        }
        if (sc->stack_top_body != 0xFFFFFFFFu) {
            if (t == sc->settle_tick || t == 0) top_y_at_settle = w->by[sc->stack_top_body].v;
            top_y_end = w->by[sc->stack_top_body].v;
        }
        if (g_dump) {
            for (u32 p = 0; p < sc->np && sc->np <= 64; ++p) {
                fprintf(stderr, "t=%u p=%u x=%lld y=%lld vx=%lld vy=%lld rho=%lld" "%c", t, p, (long long)w->px[p].v, (long long)w->py[p].v,
                        (long long)w->pvx[p].v, (long long)w->pvy[p].v, (long long)w->rho[p].v, 10);
            }
            for (u32 b = 0; b < sc->nb; ++b) {
                if (w->bflags[b] & g0::BF_STATIC) continue;
                fprintf(stderr, "t=%u b=%u x=%lld y=%lld th=%lld vx=%lld vy=%lld w=%lld\n", t, b, (long long)w->bx[b].v, (long long)w->by[b].v,
                        (long long)w->bth[b].v, (long long)w->bvx[b].v, (long long)w->bvy[b].v, (long long)w->bw[b].v);
            }
        }
        u64 per[MAX_ARENAS];
        out->hash[t] = g0::world_hash(w, per);
        cost[t] = (t2 - t0);
        if (csv && (t % csv_every) == 0) {
            char jb[32], pb[32], db[32];
            fmt_texel(jb, sizeof jb, i64(jit_p95) * fx::TEXEL.v / 10000);
            fmt_texel(pb, sizeof pb, pen.v);
            fmt_q(db, sizeof db, dens_p95);
            fprintf(csv, "%u,%u,%s,%s,%s,%s,%lld,%llu,%llu,%016llx\n", t, substeps, csv_name, jb, pb, db,
                    (long long)e, (unsigned long long)(t2 - t1), (unsigned long long)(t1 - t0), (unsigned long long)out->hash[t]);
        }
        g0::tick_end(w);
        if (escaped) {   // the trace is truncated: the hash array's tail stays zero, which the run-twice compare still matches
            fprintf(stderr, "gate0: %s: a carrier left the sealed box at tick %u - tunneling, run stopped" "%c", csv_name, t, 10);
            for (u32 r = t + 1; r < nt; ++r) { out->hash[r] = 0; cost[r] = 0; }
            break;
        }
    }
    out->escaped = escaped; out->pair_evals = w->pair_evals; out->contact_evals = w->contact_evals; out->ticks_run = ticks_run;
    out->us_broadphase = us_broadphase; out->us_predict = us_predict; out->us_density = us_density; out->us_colors = us_colors; out->us_writeback = us_writeback; out->us_velocity = us_velocity;
    // ---- timing over ticks 200..ticks_run (the ticks that RAN: a run stopped by an escape has
    // a zero tail in `cost`, which used to pull every percentile to 0 - W2 gate0 review) ----
    const u32 from = ticks_run > 250 ? 200 : 0;
    out->solve_p50 = percentile64(cost + from, ticks_run - from, 50, s);
    out->solve_p95 = percentile64(cost + from, ticks_run - from, 95, s);
    out->solve_p99 = percentile64(cost + from, ticks_run - from, 99, s);
    out->max_colors = w->max_colors_seen; out->sat_hits = w->sat_hits; out->nbr_overflow = w->nbr_overflow; out->max_nbr = w->max_neighbours_seen;
    out->sat_vel = w->sat_vel; out->sat_omega = w->sat_omega; out->corr_clamps = w->corr_clamps; out->vmax_clamps = w->vmax_clamps; out->max_degree = w->max_degree_seen;
    // ---- verdict ----
    char jb[32], pb[32], db[32];
    out->verdict = V_PASS;
    // the cost accounting, printed for G-05 whether or not the run completed (the cost of the
    // ticks that ran is evidence either way; the verdict is not)
    char costd[400];
    {
        const u64 tr = ticks_run ? ticks_run : 1;
        const u64 per_tick_pairs = out->pair_evals / tr, per_tick_contacts = out->contact_evals / tr;
        snprintf(costd, sizeof costd, "p50_us=%llu p95_us=%llu p99_us=%llu ticks_run=%u pair_evals_per_tick=%llu contact_evals_per_tick=%llu ns_per_pair_eval=%llu phase_us_per_tick[broadphase,predict,density,colors,writeback,velocity]=%llu,%llu,%llu,%llu,%llu,%llu",
                 (unsigned long long)out->solve_p50, (unsigned long long)out->solve_p95, (unsigned long long)out->solve_p99, ticks_run,
                 (unsigned long long)per_tick_pairs, (unsigned long long)per_tick_contacts,
                 (unsigned long long)(per_tick_pairs ? (out->solve_p50 * 1000ull) / per_tick_pairs : 0),
                 (unsigned long long)(us_broadphase / tr), (unsigned long long)(us_predict / tr), (unsigned long long)(us_density / tr),
                 (unsigned long long)(us_colors / tr), (unsigned long long)(us_writeback / tr), (unsigned long long)(us_velocity / tr));
    }
    // §7 R-5: for a graded-body scenario (judge 2), a NON-graded carrier leaving the box stops
    // the run (the mechanism: grid keys would wrap) but is recorded, not graded - the verdict
    // below is computed over the ticks that ran. Every other judge grades any escape as before.
    const u8 graded_scene = (judge == 2 && sc->graded_body != 0xFFFFFFFFu) ? 1 : 0;
    const u8 graded_escaped = (escaped && graded_scene && escape_body == sc->graded_body) ? 1 : 0;
    if (escaped && (!graded_scene || graded_escaped)) {   // a carrier through a wall is tunneling whatever the scenario measures: FAIL, and the metrics below are of a truncated run
        out->verdict = V_FAIL;
        snprintf(out->detail, sizeof out->detail, "tunneling=1 (a carrier left the sealed box at tick %u; the trace is truncated)%s%s", ticks_run - 1u, judge == 5 ? " " : "", judge == 5 ? costd : "");
    } else if (graded_scene) {
        // docs/GATE0-BENCH.md §2 G-02 as amended by §7 R-5: graded on the boulder - it tunnels
        // through neither the feather nor the floor, its sustained penetration inside the bands;
        // graded on the feather: the clamps engaged and were COUNTED (the counts are on the
        // verdict line), its state bounded (V_MAX/omega clamps bound it by construction; an
        // ejection through a wall stops the run and is recorded below). Counted saturations are
        // reported, never a FAIL - the §2 FAIL column reads "any UNCOUNTED saturation".
        fmt_texel(pb, sizeof pb, gpen_sustained_max);
        if (gtunnel_any) out->verdict = V_FAIL;
        else if (gpen_sustained_max >= 2 * fx::TEXEL.v) out->verdict = V_FAIL;
        else if (gpen_sustained_max >= fx::TEXEL.v) out->verdict = V_INVESTIGATE;
        char ej[96];
        if (escaped) snprintf(ej, sizeof ej, "feather_ejected_at_tick=%u (recorded, not graded; run stopped, trace truncated)", ticks_run - 1u);
        else snprintf(ej, sizeof ej, "feather_ejected=0");
        snprintf(out->detail, sizeof out->detail, "boulder_pen_sustained_texel=%s boulder_tunnel=%u any_tunnel_recorded=%u %s lambda_saturations=%u",
                 pb, gtunnel_any, tunnel_any, ej, out->sat_hits);
    } else if (judge == 1) {
        const u32 p95 = percentile(jitter_all, n_jit, 95, s);
        const i64 sink = top_y_end - top_y_at_settle;
        const i64 sink_a = sink < 0 ? -sink : sink;
        fmt_texel(jb, sizeof jb, i64(p95) * fx::TEXEL.v / 10000);
        fmt_texel(pb, sizeof pb, sink);
        // PASS: p95 < 0.1 texel, |sink| < 0.1 texel, no pop; INVESTIGATE: p95 < 0.5 texel; FAIL otherwise
        if (pop || p95 >= 5000) out->verdict = V_FAIL;
        else if (p95 >= 1000 || sink_a >= fx::TEXEL.v / 10) out->verdict = V_INVESTIGATE;
        char ib[32]; fmt_texel(ib, sizeof ib, i64(intra_max) * fx::TEXEL.v / 10000);
        snprintf(out->detail, sizeof out->detail, "jitter_p95_texel=%s top_drift_texel=%s pop=%u intra_tick_max_texel=%s perturb_quanta=%u", jb, pb, pop, ib, g_perturb);
    } else if (judge == 3) {
        fmt_q(db, sizeof db, dens_worst_p95);
        const i64 two_pct = i64(q_t::ONE) * 2 / 100, five_pct = i64(q_t::ONE) * 5 / 100;
        if (ke_increases > 0 || dens_worst_p95 >= five_pct) out->verdict = V_FAIL;
        else if (dens_worst_p95 >= two_pct) out->verdict = V_INVESTIGATE;
        snprintf(out->detail, sizeof out->detail, "density_err_p95=%s ke_window_increases_after_settle=%u", db, ke_increases);
    } else if (judge == 4) {
        // PASS: windowed max never increases; INVESTIGATE: bounded within 1% of initial; FAIL: growth beyond
        const i64 bound = e_initial + (e_initial < 0 ? -e_initial : e_initial) / 100;
        if (env_increases > 0 && e_max > bound) out->verdict = V_FAIL;
        else if (env_increases > 0) out->verdict = V_INVESTIGATE;
        snprintf(out->detail, sizeof out->detail, "envelope_increases=%u energy_initial=%lld energy_max=%lld", env_increases, (long long)e_initial, (long long)e_max);
    } else {
        snprintf(out->detail, sizeof out->detail, "%s", costd);
    }
    g0::world_release(w, os);
}

// Runs a scenario twice (unless once) and bit-compares the hash traces. Returns 1 on divergence.
u32 run_twice(Boot* bt, const g0scene::Scene* sc, const char* csv_name, u32 judge, const Args* a, u64 ticks, FILE* csv, RunOut* out, u64* diverge_tick) {
    Scratch* s = &bt->scratch;
    TL_SCRATCH_SCOPE_BEGIN(s);
    run_scenario(bt, sc, csv_name, judge, a->substeps, a->ladder, a->mu_percent, a->alpha_nano, a->iters, ticks, a->csv_every, csv, out);
    u32 diverged = 0;
    if (!a->once) {
        RunOut second;
        memset(&second, 0, sizeof(second));
        TL_SCRATCH_SCOPE_BEGIN(s);
        run_scenario(bt, sc, csv_name, judge, a->substeps, a->ladder, a->mu_percent, a->alpha_nano, a->iters, ticks, a->csv_every, nullptr, &second);
        for (u64 t = 0; t < ticks; ++t) { if (out->hash[t] != second.hash[t]) { diverged = 1; *diverge_tick = t; break; } }
        TL_SCRATCH_SCOPE_END(s);
    }
    TL_SCRATCH_SCOPE_END(s);
    out->hash = nullptr;   // the trace lived in the scope just closed; only the compare above read it
    return diverged;
}

// One scene -> one verdict line (with the run-twice status folded in). No lambdas: docs/CPP-SUBSET.md §1.
struct Ctx { Boot* bt; Args* a; };

// `recorded` = docs/GATE0-BENCH.md §2 G-03b: the run and its metrics are recorded and the
// run-twice compare still applies, but the verdict word is RECORDED and the return value is
// V_PASS unless the two in-process runs diverged (determinism is graded even where physics
// is not, until the RR-10 design pass lands).
u32 judge_scene(Ctx* c, const char* gid, const char* scene_name, const char* csv_name, u32 judge, u64 default_ticks, u32 particles, FILE* csv, RunOut* out, u8 recorded = 0) {
    Scratch* s = &c->bt->scratch;
    TL_SCRATCH_SCOPE_BEGIN(s);
    g0scene::Scene sc;
    const u64 mark = arena_mark(&c->bt->scene_arena);
    build_scene(&sc, scene_name, c->a->seed, particles, &c->bt->scene_arena);
    const u64 ticks = c->a->ticks ? c->a->ticks : default_ticks;
    u64 dt = 0;
    RunOut o; memset(&o, 0, sizeof o);
    const u32 diverged = run_twice(c->bt, &sc, csv_name, judge, c->a, ticks, csv, &o, &dt);
    arena_reset_to(&c->bt->scene_arena, mark);
    TL_SCRATCH_SCOPE_END(s);
    if (diverged) { o.verdict = V_FAIL; snprintf(o.detail, sizeof o.detail, "run_twice_divergence_at_tick=%llu (UB until proven otherwise)", (unsigned long long)dt); }
    *out = o;
    printf("VERDICT %s substeps=%u %s %s ticks=%llu run_twice=%s max_colors=%u max_nbr=%u nbr_overflow=%u lambda_sat=%u vel_clamps=%u omega_clamps=%u corr_clamps=%u vmax_clamps=%u max_degree=%u\n",
           gid, c->a->substeps, (recorded && !diverged) ? "RECORDED" : VNAME[o.verdict], o.detail, (unsigned long long)ticks,
           c->a->once ? "skipped" : (diverged ? "DIVERGED" : "identical"), o.max_colors, o.max_nbr, o.nbr_overflow, o.sat_hits, o.sat_vel, o.sat_omega, o.corr_clamps, o.vmax_clamps, o.max_degree);
    fflush(stdout);
    return (recorded && !diverged) ? V_PASS : o.verdict;
}

bool want(const Args* a, const char* id) { return !strcmp(a->scenario, "all") || !strcmp(a->scenario, id); }

FILE* open_csv(const char* dir, const char* name, u32 substeps, u32 ladder, const char* header) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_s%u_l%u.csv", dir, name, substeps, ladder);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "tl_gate0: cannot open %s\n", path); return nullptr; }
    fputs(header, f);
    return f;
}

const char* CSV_HEADER = "tick,substeps,scenario,jitter_p95_texel,max_penetration_texel,density_err_p95,energy_i64,solve_us,broadphase_us,hash_lo64\n";

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, &a)) { usage(); return 1; }
    g_dump = a.dump;
    Boot bt = boot();
    Scratch* s = &bt.scratch;
    (void)s;   // used by the shadow block only (dev tiers)
    Ctx cx; cx.bt = &bt; cx.a = &a;
    int rc = 0;

    if (want(&a, "G01")) {
        FILE* f = open_csv(a.out, "G01", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        RunOut o; const u32 v = judge_scene(&cx, "G-01", "G01", "G01", 1, 10000, a.particles, f, &o); fclose(f);
        if (v == V_FAIL) rc = 3;
    }
    if (want(&a, "G02")) {
        FILE* f = open_csv(a.out, "G02", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        RunOut oa, ob;
        const u32 va = judge_scene(&cx, "G-02a", "G02a", "G02a", 2, 2000, 0, f, &oa);
        const u32 vb = judge_scene(&cx, "G-02b", "G02b", "G02b", 2, 600, 0, f, &ob);
        fclose(f);
        const u32 v = va > vb ? va : vb;
        printf("VERDICT G-02 substeps=%u %s %s | %s\n", a.substeps, VNAME[v], oa.detail, ob.detail);
        if (v == V_FAIL) rc = 3;
    }
    if (want(&a, "G03")) {
        // THE G-03 is 1,000 particles (docs/GATE0-BENCH.md §2 as amended by §7 R-4: the count is
        // the spec, the geometry derives from CANON spacing - ~7.8 m at the 2-texel lattice).
        FILE* f = open_csv(a.out, "G03", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        RunOut o; const u32 v = judge_scene(&cx, "G-03", "G03", "G03", 3, 7000, a.particles ? a.particles : 1000, f, &o); fclose(f);
        if (v == V_FAIL) rc = 3;
    }
    if (want(&a, "G03b")) {
        // The 5k stress variant: RECORDED, not graded, until the RR-10 liquid design pass lands
        // (docs/GATE0-BENCH.md §2 G-03b, §7 R-4). Run-twice bit-identity is still enforced.
        FILE* f = open_csv(a.out, "G03b", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        RunOut o; const u32 v = judge_scene(&cx, "G-03b", "G03", "G03b", 3, 7000, a.particles ? a.particles : 5000, f, &o, 1); fclose(f);
        if (v == V_FAIL) rc = 3;   // only a run-twice divergence can fail a recorded scenario
    }
    if (want(&a, "G04")) {
        FILE* f = open_csv(a.out, "G04", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        RunOut o; const u32 v = judge_scene(&cx, "G-04", "G04", "G04", 4, 1000000, 0, f, &o); fclose(f);
        if (v == V_FAIL) rc = 3;
    }
    if (want(&a, "G05")) {
        FILE* f = open_csv(a.out, "G05", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        const u32 counts[3] = { 10000, 20000, 50000 };
        u64 p95_20k = 0;
        for (u32 i = 0; i < 3; ++i) {
            const u32 n = a.particles ? a.particles : counts[i];
            char name[32]; snprintf(name, sizeof name, "G05_%u", n);
            RunOut o;
            (void)judge_scene(&cx, name, "G05", name, 5, 500, n, f, &o);
            if (n == 20000) p95_20k = o.solve_p95;
            if (a.particles) break;
        }
        fclose(f);
        if (p95_20k) {
            // docs/GATE0-BENCH.md §2 as amended by §7 R-3: 20k <= 32 ms PC single-thread
            // (= ALLOY.md §11.2's 4 ms x 8 cores stated in the protocol's units); INVESTIGATE
            // to 64 ms; FAIL beyond. (The Pi half of the old threshold left with the Pi,
            // 2026-08-25 - the arm64 evidence is the CI cross-leg diff, never a Pi timing.)
            const u32 v = p95_20k <= 32000 ? V_PASS : (p95_20k <= 64000 ? V_INVESTIGATE : V_FAIL);
            printf("VERDICT G-05 substeps=%u %s pc_20k_p95_us=%llu\n", a.substeps, VNAME[v], (unsigned long long)p95_20k);
            if (v == V_FAIL) rc = 3;
        }
    }
    if (want(&a, "G06")) {
        // every scenario twice in one process (judge_scene already does that unless --once);
        // the verdict is the run-twice comparison alone, on shortened default ticks
        if (a.once) { fprintf(stderr, "tl_gate0: G06 needs the second run; drop --once\n"); return 1; }
        FILE* f = open_csv(a.out, "G06", a.substeps, a.ladder, CSV_HEADER); if (!f) return 1;
        // THE G-03 leg is the 1k column (§7 R-4); the 5k column keeps its bit-compare coverage
        // as a G03b leg (recorded physics, graded determinism).
        const char* scenes[7] = { "G01", "G02a", "G02b", "G03", "G03b", "G04", "G05" };
        const u32 judges[7] = { 1, 2, 2, 3, 3, 4, 5 };
        const u64 ticks6[7] = { 2000, 600, 300, 1500, 300, 3000, 300 };
        u32 diverged = 0;
        for (u32 i = 0; i < 7; ++i) {
            RunOut o;
            const u64 saved = a.ticks; if (!a.ticks) a.ticks = ticks6[i];
            char name[32]; snprintf(name, sizeof name, "G06_%s", scenes[i]);
            const char* scene_name = !strcmp(scenes[i], "G03b") ? "G03" : scenes[i];
            const u32 n6 = !strcmp(scenes[i], "G05") ? 10000u : (!strcmp(scenes[i], "G03") ? 1000u : (!strcmp(scenes[i], "G03b") ? 5000u : 0u));   // only G03/G03b/G05 take a count; 0 = the scene default
            (void)judge_scene(&cx, name, scene_name, name, judges[i], ticks6[i], n6, f, &o, (u8)(!strcmp(scenes[i], "G03b") ? 1 : 0));
            a.ticks = saved;
            if (strstr(o.detail, "run_twice_divergence")) diverged = 1;
        }
        fclose(f);
        printf("VERDICT G-06 substeps=%u %s pc_two_runs=%s\n", a.substeps, diverged ? "FAIL" : "PASS", diverged ? "DIVERGED" : "identical");
        if (diverged) rc = 3;
    }
    if (a.shadow) {
#if TL_GATE0_HAS_SHADOW
        const char* scenes[6] = { "G01", "G02a", "G02b", "G03", "G04", "G05" };
        const u64 ticks6[6] = { 1200, 300, 120, 600, 600, 60 };
        for (u32 i = 0; i < 6; ++i) {
            char nm[16]; snprintf(nm, sizeof nm, "G0%c", scenes[i][2]);
            if (!want(&a, nm) && !(want(&a, "G02") && !strncmp(scenes[i], "G02", 3))) continue;
            TL_SCRATCH_SCOPE_BEGIN(s);
            g0scene::Scene sc;
            const u64 mark = arena_mark(&bt.scene_arena);
            const bool takes_n = !strcmp(scenes[i], "G03") || !strcmp(scenes[i], "G05");
            build_scene(&sc, scenes[i], a.seed, takes_n ? (a.particles ? a.particles : (!strcmp(scenes[i], "G05") ? 10000 : 1000)) : a.particles, &bt.scene_arena);   // G03's shadow default is THE G-03 (1k, §7 R-4)
            char name[32]; snprintf(name, sizeof name, "shadow_%s", scenes[i]);
            FILE* f = open_csv(a.out, name, a.substeps, a.ladder, "");
            if (!f) return 1;
            g0::Consts k = g0::consts_make(a.substeps, a.ladder, a.mu_percent, a.alpha_nano, a.iters);
            const i64 worst = g0shadow::shadow_run(&sc, &k, a.mu_percent, a.alpha_nano, u32(a.ticks ? a.ticks : ticks6[i]), f, s, &bt.api->vmem, a.dump, a.watch);
            fclose(f);
            arena_reset_to(&bt.scene_arena, mark);
            TL_SCRATCH_SCOPE_END(s);
            char wb[32]; fmt_texel(wb, sizeof wb, worst);
            printf("SHADOW %s substeps=%u max_abs_err_texel=%s (raw %lld)\n", scenes[i], a.substeps, wb, (long long)worst);
        }
#else
        fprintf(stderr, "tl_gate0: --shadow needs a dev/debug tier build (docs/GATE0-BENCH.md section 1)\n");
        return 1;
#endif
    }
    platform_headless_shutdown(bt.api);
    return rc;
}
