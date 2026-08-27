// profiler_panel.cpp - see profiler_panel.h's contract block. Spec: docs/TOOLING.md §9.3.1, §9.4.
#include "editor/profiler_panel.h"

#include "foundation/tl_prof.h"

#include <imgui.h>

namespace {

void draw_node(const ProfNode& n) {
    // t_end == 0 means the scope was still open when its frame closed (a worker other than 0 -
    // tl_prof.h's own contract only asserts worker 0's stack is balanced at frame end); the
    // subtraction would underflow, so this shows 0 rather than a huge wrapped duration.
    const u64 dt = (n.t_end > n.t_begin) ? (n.t_end - n.t_begin) : 0u;
    ImGui::Text("%*s%s", (int)(n.depth * 2u), "", n.name);
    ImGui::SameLine();
    if (n.job_id == 0xFFFFFFFFu) {
        ImGui::Text("dt=%llu w%u", (unsigned long long)dt, (u32)n.worker);
    } else {
        ImGui::Text("dt=%llu w%u job=%u", (unsigned long long)dt, (u32)n.worker, n.job_id);
    }
}

}  // namespace

void profiler_panel_register(Editor* ed) { editor_register_panel(ed, "Profiler", profiler_panel_draw, true); }

void profiler_panel_draw(Editor* ed, World* /*w*/) {
    if (!ImGui::Begin("Profiler")) { ImGui::End(); return; }

    const u32 ring_count = tl_prof_ring_count();
    ImGui::Text("frames: %u/%u", ring_count, (u32)PROF_RING_FRAMES);

    if (ring_count == 0u) {
        ImGui::TextUnformatted("(no frames recorded yet)");
        ImGui::End();
        return;
    }

    bool paused = ed->prof_paused != 0u;
    ImGui::Checkbox("pause", &paused);
    ed->prof_paused = paused ? 1u : 0u;

    u32 slot = 0u;
    if (paused) {
        if (!ed->prof_view_frame_valid) {
            // Freshly paused (or never pinned since the last unpause): pin the absolute frame
            // under view, not a `slots_back` offset - the ring keeps advancing underneath a paused
            // panel (nothing stops `prof.cpp`), so a stored `slots_back` names a DIFFERENT frame on
            // every later draw (the B-1 bug this replaces).
            ed->prof_view_frame = tl_prof_ring_at(0)->frame;
            ed->prof_view_frame_valid = 1u;
        }
        // Re-derive this frame's CURRENT slots_back every draw - it moves as the ring advances.
        bool found = false;
        for (u32 i = 0; i < ring_count; ++i) {
            if (tl_prof_ring_at(i)->frame == ed->prof_view_frame) { slot = i; found = true; break; }
        }
        if (!found) {
            // The pinned frame aged out past PROF_RING_FRAMES - fall back to the oldest still-live
            // frame and say so, rather than silently jumping to an unrelated frame.
            slot = ring_count - 1u;
            ed->prof_view_frame = tl_prof_ring_at(slot)->frame;
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "pinned frame aged out of the ring - showing the oldest live frame");
        }
        ed->prof_view_slot = slot;

        int s = (int)slot;
        ImGui::SliderInt("frame", &s, 0, (int)ring_count - 1);
        if ((u32)s != slot) {
            // The user picked a different frame - re-pin to ITS absolute frame number so it, too,
            // stays put under an advancing ring rather than drifting back to a `slots_back` offset.
            slot = (u32)s;
            ed->prof_view_slot = slot;
            ed->prof_view_frame = tl_prof_ring_at(slot)->frame;
        }
    } else {
        ed->prof_view_slot = 0u;         // always follows the latest frame while unpaused
        ed->prof_view_frame_valid = 0u;  // re-pin fresh on the next pause
    }

    const ProfFrame* f = tl_prof_ring_at(slot);
    ImGui::Text("frame %llu  tick %llu  nodes %u  dropped %u",
                (unsigned long long)f->frame, (unsigned long long)f->tick, f->node_count, f->dropped);

    ImGui::Separator();
    for (u32 i = 0; i < f->node_count; ++i) {
        ImGui::PushID((int)i);
        draw_node(f->nodes[i]);
        ImGui::PopID();
    }

    const u32 counter_count = tl_prof_counter_count();
    if (counter_count > 0u) {
        ImGui::Separator();
        ImGui::TextUnformatted("counters");
        for (u32 i = 0; i < counter_count; ++i) {
            const ProfCounter* c = tl_prof_counter_at(i);
            ImGui::PushID((int)(0x10000u + i));
            ImGui::Text("%s = %lld", c->name, (long long)c->value);
            ImGui::PopID();
        }
    }

    ImGui::End();
}
