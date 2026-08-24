// shadow.cpp - the FLOAT-SHADOW lockstep (contract: shadow.h). Both worlds are stepped through
// the same pass sequence; after every pass the positions are compared carrier by carrier and
// the max error is attributed to the constraint kind that touched that carrier in that pass
// (exact within a colour: the colouring guarantees one constraint per dynamic carrier per
// colour). Passes: 0 predict, 1 project (per kind), 2 writeback, 3 velocity.
#include "gate0/shadow.h"
#include "gate0/solver_dbl.h"

#include <string.h>

namespace g0shadow {

namespace {

const char* KIND_NAME[4] = { "distance", "contact", "density", "-" };

i64 abs64(i64 x) { return x < 0 ? -x : x; }

// max |fx - dbl| over every dynamic carrier's position (pos_t raw), plus per-carrier errors.
i64 compare(const g0::World* a, const g0s::World* b, i64* err_b, i64* err_p) {
    i64 m = 0;
    for (u32 i = 0; i < a->nb; ++i) {
        const i64 ex = abs64(g0::raw_pos(a->bx[i]) - g0s::raw_pos(b->bx[i]));
        const i64 ey = abs64(g0::raw_pos(a->by[i]) - g0s::raw_pos(b->by[i]));
        err_b[i] = ex > ey ? ex : ey;
        if (err_b[i] > m) m = err_b[i];
    }
    for (u32 i = 0; i < a->np; ++i) {
        const i64 ex = abs64(g0::raw_pos(a->px[i]) - g0s::raw_pos(b->px[i]));
        const i64 ey = abs64(g0::raw_pos(a->py[i]) - g0s::raw_pos(b->py[i]));
        err_p[i] = ex > ey ? ex : ey;
        if (err_p[i] > m) m = err_p[i];
    }
    return m;
}
// Same over the solver locals (mid-substep), in pos_t raw units.
i64 compare_locals(const g0::World* a, const g0s::World* b, i64* err_b, i64* err_p) {
    i64 m = 0;
    for (u32 i = 0; i < a->nb; ++i) {
        const i64 ex = abs64(g0::raw_pos(g0::pos_from_xl(a->xl_bx[i])) - g0s::raw_pos(g0s::pos_from_xl(b->xl_bx[i])));
        const i64 ey = abs64(g0::raw_pos(g0::pos_from_xl(a->xl_by[i])) - g0s::raw_pos(g0s::pos_from_xl(b->xl_by[i])));
        err_b[i] = ex > ey ? ex : ey;
        if (err_b[i] > m) m = err_b[i];
    }
    for (u32 i = 0; i < a->np; ++i) {
        const i64 ex = abs64(g0::raw_pos(g0::pos_from_xl(a->xl_px[i])) - g0s::raw_pos(g0s::pos_from_xl(b->xl_px[i])));
        const i64 ey = abs64(g0::raw_pos(g0::pos_from_xl(a->xl_py[i])) - g0s::raw_pos(g0s::pos_from_xl(b->xl_py[i])));
        err_p[i] = ex > ey ? ex : ey;
        if (err_p[i] > m) m = err_p[i];
    }
    return m;
}

}  // namespace

i64 shadow_run(const g0scene::Scene* scene, const g0::Consts* k, u32 ticks, FILE* csv, Scratch* scratch, const VMemApi* os, u8 dump, u32 watch) {
    g0::World* wa = (g0::World*)scratch_push(scratch, sizeof(g0::World), 64u);
    g0s::World* wb = (g0s::World*)scratch_push(scratch, sizeof(g0s::World), 64u);
    g0::world_init(wa, os, scratch, scene->nb, scene->np, scene->nd, k);
    g0s::Consts kb = g0s::consts_make(k->substeps, k->ladder, 50, 1302, k->density_iters);
    g0s::world_init(wb, os, scratch, scene->nb, scene->np, scene->nd, &kb);
    g0::world_load(wa, scene);
    g0s::world_load(wb, scene);
    i64* err_b = (i64*)scratch_push(scratch, u64(scene->nb) * 8u + 16u, 16u);
    i64* err_p = (i64*)scratch_push(scratch, u64(scene->np) * 8u + 16u, 16u);
    i64* prev_b = (i64*)scratch_push(scratch, u64(scene->nb) * 8u + 16u, 16u);
    i64* prev_p = (i64*)scratch_push(scratch, u64(scene->np) * 8u + 16u, 16u);
    fprintf(csv, "tick,substep,pass,constraint_kind,max_abs_err_fx_vs_double\n");
    i64 worst = 0;
    for (u32 t = 0; t < ticks; ++t) {
        g0::tick_begin(wa);
        g0s::tick_begin(wb);
        for (u32 s = 0; s < k->substeps; ++s) {
            g0::substep_predict(wa, s);
            g0s::substep_predict(wb, s);
            i64 m = compare_locals(wa, wb, err_b, err_p);
            fprintf(csv, "%u,%u,predict,-,%lld\n", t, s, (long long)m);
            if (m > worst) worst = m;
            const u32 nc = wa->n_colors > wb->n_colors ? wa->n_colors : wb->n_colors;
            i64 kind_max[3] = { 0, 0, 0 };
            memcpy(prev_p, err_p, u64(wa->np) * 8u);
            g0::substep_density(wa);
            g0s::substep_density(wb);
            compare_locals(wa, wb, err_b, err_p);
            for (u32 p = 0; p < wa->np; ++p) if (err_p[p] - prev_p[p] > kind_max[2]) kind_max[2] = err_p[p] - prev_p[p];
            for (u32 c = 0; c < nc; ++c) {
                memcpy(prev_b, err_b, u64(wa->nb) * 8u);
                memcpy(prev_p, err_p, u64(wa->np) * 8u);
                if (c < wa->n_colors) g0::substep_project_color(wa, c);
                if (c < wb->n_colors) g0s::substep_project_color(wb, c);
                compare_locals(wa, wb, err_b, err_p);
                if (c >= wa->n_colors) continue;
                // attribute each carrier's error growth to the fx-side constraint of colour c that owns it
                for (u32 it = wa->level_begin[c]; it < wa->level_begin[c + 1]; ++it) {
                    const u32 item = wa->level_items[it];
                    const u32 kind = item >> 30, idx = item & 0x3FFFFFFFu;
                    i64 g = 0;
                    if (kind == g0::LV_DIST) {
                        const u32 a = wa->da[idx], b = wa->db[idx];
                        g = err_p[a] - prev_p[a]; if (err_p[b] - prev_p[b] > g) g = err_p[b] - prev_p[b];
                    } else if (kind == g0::LV_CONTACT) {
                        const g0::Contact* ct = &wa->contacts[idx];
                        g = ct->kind == g0::CK_PB ? err_p[ct->i] - prev_p[ct->i] : err_b[ct->i] - prev_b[ct->i];
                        if (err_b[ct->j] - prev_b[ct->j] > g) g = err_b[ct->j] - prev_b[ct->j];
                    } else {
                        continue;   // density is its own pass now
                    }
                    if (g > kind_max[kind]) kind_max[kind] = g;
                }
            }
            for (u32 kd = 0; kd < 3; ++kd) fprintf(csv, "%u,%u,project,%s,%lld\n", t, s, KIND_NAME[kd], (long long)kind_max[kd]);
            g0::substep_writeback(wa);
            g0s::substep_writeback(wb);
            m = compare(wa, wb, err_b, err_p);
            fprintf(csv, "%u,%u,writeback,-,%lld\n", t, s, (long long)m);
            if (m > worst) worst = m;
            g0::substep_velocity(wa);
            g0s::substep_velocity(wb);
            m = compare(wa, wb, err_b, err_p);
            fprintf(csv, "%u,%u,velocity,-,%lld\n", t, s, (long long)m);
            if (m > worst) worst = m;
        }
        if (watch < wa->np) {
            const u32 p = watch;
            fprintf(stderr, "t=%u p=%u fx x=%lld y=%lld rho=%lld | dbl x=%lld y=%lld rho=%lld" "%c", t, p,
                    (long long)g0::raw_pos(wa->px[p]), (long long)g0::raw_pos(wa->py[p]), (long long)g0::raw_q(wa->rho[p]),
                    (long long)g0s::raw_pos(wb->px[p]), (long long)g0s::raw_pos(wb->py[p]), (long long)g0s::raw_q(wb->rho[p]), 10);
        }
        if (dump) {
            for (u32 p = 0; p < wa->np && wa->np <= 64; ++p) {
                fprintf(stderr, "t=%u p=%u fx x=%lld y=%lld rho=%lld | dbl x=%lld y=%lld rho=%lld" "%c", t, p,
                        (long long)g0::raw_pos(wa->px[p]), (long long)g0::raw_pos(wa->py[p]), (long long)g0::raw_q(wa->rho[p]),
                        (long long)g0s::raw_pos(wb->px[p]), (long long)g0s::raw_pos(wb->py[p]), (long long)g0s::raw_q(wb->rho[p]), 10);
            }
            for (u32 b = 0; b < wa->nb; ++b) {
                if (wa->bflags[b] & g0::BF_STATIC) continue;
                fprintf(stderr, "t=%u b=%u fx x=%lld y=%lld th=%lld | dbl x=%lld y=%lld th=%lld" "%c", t, b,
                        (long long)g0::raw_pos(wa->bx[b]), (long long)g0::raw_pos(wa->by[b]), (long long)g0::raw_ang(wa->bth[b]),
                        (long long)g0s::raw_pos(wb->bx[b]), (long long)g0s::raw_pos(wb->by[b]), (long long)g0s::raw_ang(wb->bth[b]), 10);
            }
        }
        g0::tick_end(wa);
        g0s::tick_end(wb);
    }
    g0::world_release(wa, os);
    g0s::world_release(wb, os);
    return worst;
}

}  // namespace g0shadow
