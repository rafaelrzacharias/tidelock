// probes_panel.cpp - see probes_panel.h's contract block. Spec: docs/TOOLING.md §9.3.3, §9.4.
#include "editor/probes_panel.h"

#include "foundation/tl_probe.h"

#include <imgui.h>

namespace {
const char* kind_label(u8 kind) {
    switch (kind) {
        case PROBE_LOG:       return "LOG";
        case PROBE_ON_CHANGE: return "ON_CHANGE";
        case PROBE_MARK:      return "MARK";
        default:              return "ASSERT";
    }
}
}  // namespace

void probes_panel_register(Editor* ed) { editor_register_panel(ed, "Probes", probes_panel_draw, true); }

f64 probes_panel_mean(const ProbeKey* k) { return (k->count != 0u) ? (k->sum / (f64)k->count) : 0.0; }

void probes_panel_draw(Editor* /*ed*/, World* /*w*/) {
    if (!ImGui::Begin("Probes")) { ImGui::End(); return; }

    const u32 n = tl_probe_key_count();
    ImGui::Text("keys: %u", n);
    ImGui::Separator();

    for (u32 i = 0; i < n; ++i) {
        const ProbeKey* k = tl_probe_key_at(i);
        const f64 mean = probes_panel_mean(k);
        ImGui::PushID((int)i);
        ImGui::Text("%s [%s]%s", k->name, kind_label(k->kind), k->enabled ? "" : " (disabled)");
        ImGui::Text("  count=%llu changes=%llu min=%.9g max=%.9g mean=%.9g last=%.9g tick=%llu",
                    (unsigned long long)k->count, (unsigned long long)k->changes, k->min, k->max,
                    mean, k->last, (unsigned long long)k->last_tick);
        ImGui::PopID();
    }

    ImGui::End();
}
