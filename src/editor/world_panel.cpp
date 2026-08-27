// world_panel.cpp - see world_panel.h's contract block. Spec: docs/TOOLING.md §9.4.
#include "editor/world_panel.h"

#include "foundation/handle.h"

#include <imgui.h>

#include <string.h>

namespace {

// The single predicate deciding whether slot `idx` is a live entity - `draw_entities`'s clipper
// loop and `world_panel_visible_slots` both call this, so they cannot drift into disagreement.
bool slot_visible(const World* w, u32 idx) { return bitset_test(&w->entities.live, idx); }

void ensure_hash_buffers(Editor* ed) {
    if (ed->world_arena_hash_cur != nullptr) { return; }
    ed->world_arena_hash_cur = (u64*)arena_push(&ed->dev_arena, sizeof(u64) * MAX_ARENAS, alignof(u64));
    ed->world_arena_hash_prev = (u64*)arena_push(&ed->dev_arena, sizeof(u64) * MAX_ARENAS, alignof(u64));
}

void draw_entities(Editor* ed, World* w) {
    const u32 cap = slotmap_slot_cap(&w->entities);
    ImGui::Text("entities: %u live / %u slots (quarantined %u)",
                w->entities.live_count, cap, w->entities.quarantined);

    ImGui::BeginChild("entities", ImVec2(0.0f, 200.0f), true);
    ImGuiListClipper clipper;
    clipper.Begin((int)cap);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const u32 idx = (u32)i;
            if (!slot_visible(w, idx)) { continue; }   // dead slot in this window
            const Entity e = handle_make<Entity>(idx, w->entities.gen.data[idx]);
            const EntityRecord& rec = w->entities.slots.data[idx];
            ImGui::PushID(i);
            ImGui::Text("#%u g%u  %u comps", idx, (u32)w->entities.gen.data[idx], (u32)rec.comp_count);
            ImGui::SameLine();
            if (ImGui::SmallButton("select")) { ed->sel = e; }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void draw_singletons(World* w) {
    ImGui::TextUnformatted("singletons");
    for (u32 c = 0; c < w->comp_count; ++c) {
        const ComponentInfo* info = w->comps[c].info;
        if ((info->flags & COMP_SINGLETON) != 0u) { ImGui::BulletText("%s", info->name); }
    }
}

void draw_arenas(Editor* ed, World* w) {
    const ArenaRegistry* r = w->registry;
    ImGui::Text("arenas: %u/%u", r->count, (u32)MAX_ARENAS);

    // registry_hash_all is TL_CHECK-fatal on an unsealed registry (foundation/arena_registry.cpp)
    // - sealing is the registry's OWNER's job (registry_seal, called once at end of init), which
    // is `app/`'s (W4, not built - editor.h's own Status note names the same "blocked on app/"
    // shape for editor_frame). Showing the button anyway would crash the whole dev session the
    // first time anyone clicked it in any world this lane can build today - guarded instead of
    // shipped as a live grenade (this file's own Invariants note in world_panel.h).
    if (r->sealed == 0u) {
        ImGui::TextUnformatted("(arena registry not sealed yet - app/ wiring not built)");
        return;
    }
    if (ImGui::Button("rehash arenas")) { world_panel_rehash_arenas(ed, w); }

    if (!ed->world_arena_hash_valid) {
        ImGui::TextUnformatted("(press \"rehash arenas\" to compute)");
        return;
    }
    for (u32 i = 0; i < r->count; ++i) {
        const ArenaEntry& e = r->e[i];
        const bool changed = ed->world_arena_hash_have_prev
                            && ed->world_arena_hash_prev[i] != ed->world_arena_hash_cur[i];
        ImGui::PushID((int)i);
        ImGui::Text("id=0x%016llx %s%s%s used=%llu/%llu hash=0x%016llx%s",
                    (unsigned long long)e.id,
                    (e.flags & ARENA_HASHED) ? "H" : "-",
                    (e.flags & ARENA_SNAPSHOT) ? "S" : "-",
                    (e.flags & ARENA_GROWS_AT_BARRIER) ? "G" : "-",
                    (unsigned long long)e.arena->used, (unsigned long long)e.arena->reserved,
                    (unsigned long long)ed->world_arena_hash_cur[i], changed ? "  *changed*" : "");
        ImGui::PopID();
    }
}

}  // namespace

void world_panel_register(Editor* ed) { editor_register_panel(ed, "World", world_panel_draw, true); }

u32 world_panel_visible_slots(const World* w, u32* out, u32 cap) {
    const u32 slot_cap = slotmap_slot_cap(&w->entities);
    u32 written = 0u;
    for (u32 idx = 0; idx < slot_cap; ++idx) {
        if (!slot_visible(w, idx)) { continue; }
        if (written < cap) { out[written] = idx; }
        written += 1u;
    }
    return written;
}

void world_panel_rehash_arenas(Editor* ed, World* w) {
    // B-10 (2026-08-27): world_panel_draw guards w == nullptr before ever reaching the button
    // that calls this; this exported entry point (world_panel.h: "exposed and testable directly")
    // did not, so a caller other than draw's own button - any future one - could crash on the
    // very first dereference below.
    if (w == nullptr) { return; }
    if (w->registry->sealed == 0u) { return; }   // registry_hash_all is TL_CHECK-fatal until
                                                   // registry_seal runs (app/'s job, not built)
    ensure_hash_buffers(ed);
    if (ed->world_arena_hash_valid) {
        memcpy(ed->world_arena_hash_prev, ed->world_arena_hash_cur, sizeof(u64) * MAX_ARENAS);
        ed->world_arena_hash_have_prev = 1u;
    }
    (void)registry_hash_all(w->registry, ed->world_arena_hash_cur);
    ed->world_arena_hash_valid = 1u;
}

void world_panel_draw(Editor* ed, World* w) {
    if (!ImGui::Begin("World")) { ImGui::End(); return; }
    if (w == nullptr) { ImGui::TextUnformatted("(no world)"); ImGui::End(); return; }

    draw_entities(ed, w);
    ImGui::Separator();
    draw_singletons(w);
    ImGui::Separator();
    draw_arenas(ed, w);

    ImGui::End();
}
