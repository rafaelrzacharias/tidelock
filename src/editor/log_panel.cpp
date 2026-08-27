// log_panel.cpp - see log_panel.h's contract block.
#include "editor/log_panel.h"

#include "foundation/tl_assert.h"
#include "foundation/tl_log.h"

#include <imgui.h>

namespace {
const char* level_label(u8 level) {
    switch (level) {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        default:        return "ERR";
    }
}

// docs/TOOLING.md's log levels read worse the louder they are - ERR red, WARN yellow, the rest
// the theme's default text colour (no per-level tint worth the noise).
ImVec4 level_color(u8 level) {
    switch (level) {
        case LOG_WARN: return ImVec4(0.9f, 0.75f, 0.2f, 1.0f);
        case LOG_ERR:  return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        default:       return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}
}  // namespace

void log_panel_register(Editor* ed) { editor_register_panel(ed, "Log", log_panel_draw, true); }

const LogRecord* log_panel_row_at(u32 row) {
    TL_CHECK(row < tl_log_ring_count());
    // Newest first (write order runs oldest -> newest, docs/TOOLING.md's tl_log_ring_at
    // convention) - a dev watching the panel wants the latest line at the top, not scrolled
    // past 4096 records to find it. Row 0 = the newest = ring index (count - 1).
    return tl_log_ring_at(tl_log_ring_count() - 1u - row);
}

void log_panel_draw(Editor* /*ed*/, World* /*w*/) {
    if (!ImGui::Begin("Log")) { ImGui::End(); return; }
    const u32 n = tl_log_ring_count();
    for (u32 row = 0; row < n; ++row) {
        const LogRecord* r = log_panel_row_at(row);
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(r->level));
        ImGui::Text("[%s] %s:%u: %s", level_label(r->level), r->file, r->line, r->msg);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}
