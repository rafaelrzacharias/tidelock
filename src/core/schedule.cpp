// schedule.cpp - registration storage, the topo-sorted build, run_phase. Spec: docs/ECS.md
// §10.6; contracts in schedule.h.
#include "core/schedule.h"
#include "core/world.h"
#include "foundation/map.h"
#include <string.h>

namespace {

// Copies a span's contents onto the meta arena so descs outlive their registration call sites.
Span<const ComponentId> sched_copy_ids(VMemArena* meta, Span<const ComponentId> s) {
    if (s.count == 0) { return Span<const ComponentId>{ nullptr, 0 }; }
    ComponentId* p = (ComponentId*)arena_push(meta, (u64)s.count * sizeof(ComponentId), alignof(ComponentId));
    memcpy(p, s.data, (usize)s.count * sizeof(ComponentId));
    return Span<const ComponentId>{ p, s.count };
}

// Same for label lists.
Span<const NameHash> sched_copy_labels(VMemArena* meta, Span<const NameHash> s) {
    if (s.count == 0) { return Span<const NameHash>{ nullptr, 0 }; }
    NameHash* p = (NameHash*)arena_push(meta, (u64)s.count * sizeof(NameHash), alignof(NameHash));
    memcpy(p, s.data, (usize)s.count * sizeof(NameHash));
    return Span<const NameHash>{ p, s.count };
}

// Resolves a before/after label to a system index IN THE SAME PHASE as `self`. Unknown and
// cross-phase labels take the same fatal path (docs/ECS.md §10.6; phases already order across
// phases, so a cross-phase edge is a registration bug, not a refinement).
u32 sched_resolve(Map<NameHash, u32>* by_label, const Schedule* s, u32 self, NameHash label) {
    u32* found = map_get(by_label, label);
    if (found == nullptr) { TL_FATAL("schedule: unknown before/after label"); }
    if (s->systems.data[*found].d.phase != s->systems.data[self].d.phase) {
        TL_FATAL("schedule: before/after label crosses phases");
    }
    return *found;
}

}  // namespace

void schedule_init(Schedule* s, VMemArena* meta) {
    array_init_fixed(&s->systems, meta, MAX_SYSTEMS);
    array_init_fixed(&s->order, meta, MAX_SYSTEMS);
    for (u32 p = 0; p <= PHASE_COUNT; ++p) { s->phase_begin[p] = 0; }
    s->running.index = RUNNING_NONE;
    s->running.label = 0;
    s->built = 0;
    s->_pad0[0] = s->_pad0[1] = s->_pad0[2] = 0;
}

void schedule_register(Schedule* s, VMemArena* meta, const SystemDesc* desc) {
    TL_CHECK(desc != nullptr && desc->fn != nullptr);
    TL_CHECK(s->built == 0);   // init only; a script reload resets and re-registers (the world's sequencing)
    TL_CHECK(desc->phase < PHASE_COUNT);
    if (s->systems.count == MAX_SYSTEMS) { TL_FATAL("schedule: MAX_SYSTEMS exceeded"); }
    for (u32 i = 0; i < s->systems.count; ++i) {
        if (s->systems.data[i].d.label == desc->label) {
            TL_FATAL("schedule: duplicate system label");   // labels are the before/after edge keys
        }
    }
    SystemRec rec;
    memset(&rec, 0, sizeof(rec));
    rec.d = *desc;
    rec.d.reads = sched_copy_ids(meta, desc->reads);
    rec.d.writes = sched_copy_ids(meta, desc->writes);
    rec.d.before = sched_copy_labels(meta, desc->before);
    rec.d.after = sched_copy_labels(meta, desc->after);
    rec.reg_index = s->systems.count;
    rec.phase_pos = RUNNING_NONE;
    array_push(&s->systems, rec);
}

void schedule_build(Schedule* s, Scratch* scratch) {
    const u32 n = s->systems.count;
    s->order.count = 0;   // rebuild-safe: the order array is refilled in place
    TL_SCRATCH_SCOPE_BEGIN(scratch);
    {
        // Label -> index over ALL systems (resolution then checks the phase).
        Map<NameHash, u32> by_label;
        map_init(&by_label, &scratch->a, n < 8u ? 8u : n);
        for (u32 i = 0; i < n; ++i) { map_put(&by_label, s->systems.data[i].d.label, i); }

        // Edge lists in CSR form on scratch: edge u -> v means "u runs before v".
        u32* indeg = (u32*)scratch_push(scratch, (u64)n * 4u + 4u, 4u);
        u32* head_count = (u32*)scratch_push(scratch, (u64)n * 4u + 4u, 4u);
        memset(indeg, 0, (usize)n * 4u + 4u);
        memset(head_count, 0, (usize)n * 4u + 4u);
        u32 edge_total = 0;
        for (u32 i = 0; i < n; ++i) {
            edge_total += s->systems.data[i].d.before.count + s->systems.data[i].d.after.count;
        }
        u32* edge_from = (u32*)scratch_push(scratch, (u64)edge_total * 4u + 4u, 4u);
        u32* edge_to = (u32*)scratch_push(scratch, (u64)edge_total * 4u + 4u, 4u);
        u32 ec = 0;
        for (u32 i = 0; i < n; ++i) {
            const SystemDesc* d = &s->systems.data[i].d;
            for (u32 k = 0; k < d->after.count; ++k) {
                const u32 u = sched_resolve(&by_label, s, i, d->after.data[k]);
                edge_from[ec] = u; edge_to[ec] = i; ++ec;
            }
            for (u32 k = 0; k < d->before.count; ++k) {
                const u32 v = sched_resolve(&by_label, s, i, d->before.data[k]);
                edge_from[ec] = i; edge_to[ec] = v; ++ec;
            }
        }
        for (u32 e = 0; e < ec; ++e) { head_count[edge_from[e]] += 1u; indeg[edge_to[e]] += 1u; }
        // CSR offsets + scatter.
        u32* offs = (u32*)scratch_push(scratch, ((u64)n + 1u) * 4u, 4u);
        offs[0] = 0;
        for (u32 i = 0; i < n; ++i) { offs[i + 1u] = offs[i] + head_count[i]; }
        u32* adj = (u32*)scratch_push(scratch, (u64)ec * 4u + 4u, 4u);
        u32* fill = (u32*)scratch_push(scratch, (u64)n * 4u + 4u, 4u);
        memset(fill, 0, (usize)n * 4u + 4u);
        for (u32 e = 0; e < ec; ++e) {
            const u32 u = edge_from[e];
            adj[offs[u] + fill[u]] = edge_to[e];
            fill[u] += 1u;
        }

        // Per phase: Kahn with the ready set scanned for the LOWEST reg_index (the tie-break -
        // docs/ECS.md §10.6). done[] marks emitted systems; indeg goes to RUNNING_NONE on emit.
        u8* done = (u8*)scratch_push(scratch, (u64)n + 1u, 1u);
        memset(done, 0, (usize)n + 1u);
        for (u32 p = 0; p < PHASE_COUNT; ++p) {
            s->phase_begin[p] = s->order.count;
            u32 members = 0;
            for (u32 i = 0; i < n; ++i) {
                if (s->systems.data[i].d.phase == (Phase)p) { ++members; }
            }
            for (u32 emitted = 0; emitted < members; ++emitted) {
                u32 pick = RUNNING_NONE;
                for (u32 i = 0; i < n; ++i) {   // lowest reg_index first: i IS reg_index
                    if (!done[i] && s->systems.data[i].d.phase == (Phase)p && indeg[i] == 0u) {
                        pick = i;
                        break;
                    }
                }
                if (pick == RUNNING_NONE) { TL_FATAL("schedule: before/after cycle"); }
                done[pick] = 1;
                s->systems.data[pick].phase_pos = s->order.count;
                array_push(&s->order, pick);
                for (u32 a = offs[pick]; a < offs[pick] + head_count[pick]; ++a) {
                    TL_ASSERT(indeg[adj[a]] > 0u);
                    indeg[adj[a]] -= 1u;
                }
            }
        }
        s->phase_begin[PHASE_COUNT] = s->order.count;
        TL_CHECK(s->order.count == n);
    }
    TL_SCRATCH_SCOPE_END(scratch);
    s->built = 1;
}

void run_phase(World* w, Phase p) {
    Schedule* s = &w->sched;
    TL_CHECK(s->built == 1 && p < PHASE_COUNT);
    for (u32 i = s->phase_begin[p]; i < s->phase_begin[p + 1u]; ++i) {
        const u32 si = s->order.data[i];
        // Published before every call: the Luau trampoline and the profiler auto-scope read it
        // (docs/CANON.md "w->sched.running"; docs/ECS.md §10.6).
        s->running.index = si;
        s->running.label = s->systems.data[si].d.label;
        s->systems.data[si].d.fn(w);
        s->running.index = RUNNING_NONE;
        s->running.label = 0;
    }
    apply_commands(w);   // every phase boundary is a command barrier (docs/ECS.md §4)
}
