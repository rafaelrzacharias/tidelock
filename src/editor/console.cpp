// console.h - tokenizer, registration (name-sorted index, binary-search dispatch lookup by
// NAME - console_find's own comment; corrected 2026-08-27, B-5), completion, history, and the
// Console panel's draw_fn. Spec: docs/TOOLING.md §3, §9.2, §9.3.5.
#include "editor/console.h"
#include "editor/editor.h"

#include <imgui.h>

#include <string.h>

namespace {

int name_cmp(const char* a, const char* b) { return strcmp(a, b); }

}  // namespace

void console_init(ConsoleState* s) {
    memset(s, 0, sizeof(ConsoleState));
}

const ConsoleCmd* console_find(const ConsoleState* s, StrView name) {
    // Bytewise binary search on the name-sorted index; StrView is not NUL-terminated, so compare
    // length-then-bytes rather than reusing name_cmp (which needs two C strings).
    u32 lo = 0, hi = s->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        const ConsoleCmd* c = &s->cmds[s->sorted[mid]];
        const usize clen = strlen(c->name);
        const usize n = clen < name.len ? clen : name.len;
        int cmp = n > 0u ? memcmp(c->name, name.ptr, n) : 0;
        if (cmp == 0) { cmp = (clen < name.len) ? -1 : (clen > name.len ? 1 : 0); }
        if (cmp == 0) { return c; }
        if (cmp < 0) { lo = mid + 1u; } else { hi = mid; }
    }
    return nullptr;
}

void console_register(ConsoleState* s, const ConsoleCmd* cmd) {
    if (s->count >= CONSOLE_TABLE_CAP) { TL_FATAL("console_register: CONSOLE_TABLE_CAP exhausted"); }
    if (console_find(s, sv(cmd->name)) != nullptr) { TL_FATAL("console_register: duplicate command name"); }

    const u32 idx = s->count;
    s->cmds[idx] = *cmd;
    const NameHash key = fnv1a64(cmd->name, strlen(cmd->name));
    // A caller that leaves `key` zero gets it computed here; one that populates it must agree -
    // console_find never reads it (B-5, 2026-08-27: dispatch is by name, not by hash), but a
    // silently-wrong key would still be a real registration bug the day something DOES read it.
    TL_CHECK(cmd->key == 0u || cmd->key == key);
    s->cmds[idx].key = key;

    u32 pos = s->count;
    for (u32 i = 0; i < s->count; ++i) {
        if (name_cmp(cmd->name, s->cmds[s->sorted[i]].name) < 0) { pos = i; break; }
    }
    for (u32 i = s->count; i > pos; --i) { s->sorted[i] = s->sorted[i - 1]; }
    s->sorted[pos] = (u16)idx;
    s->count += 1u;
}

u32 console_tokenize(const char* line, ConsoleToken* out, u32 out_cap,
                      char* unescape_buf, u32 unescape_cap, ErrCode* out_err) {
    *out_err = ERR_OK;
    u32 count = 0;
    u32 esc_used = 0;
    usize i = 0;

    while (line[i] != '\0') {
        while (line[i] == ' ' || line[i] == '\t') { ++i; }
        if (line[i] == '\0' || line[i] == '#') { break; }   // comment: drop the rest of the line

        if (count >= out_cap) { *out_err = ERR_CONSOLE_TOO_MANY_ARGS; return 0; }

        if (line[i] == '"') {
            ++i;
            char* dst = unescape_buf + esc_used;
            u32 dst_cap_left = (esc_used <= unescape_cap) ? (unescape_cap - esc_used) : 0u;
            u32 len = 0;
            bool closed = false;
            while (line[i] != '\0') {
                char c = line[i];
                if (c == '"') { ++i; closed = true; break; }
                if (c == '\\' && (line[i + 1] == '"' || line[i + 1] == '\\')) { c = line[i + 1]; ++i; }
                if (len < dst_cap_left) { dst[len] = c; }   // never overflows regardless of out_err below
                ++len;
                ++i;
            }
            if (!closed) { *out_err = ERR_CONSOLE_SYNTAX; return 0; }
            // B-11 (2026-08-27): a token longer than the caller's remaining scratch space used to
            // truncate silently (out_err left ERR_OK); named instead, so a caller relying on the
            // full argument text finds out rather than silently acting on a cut one.
            if (len > dst_cap_left) { *out_err = ERR_CONSOLE_TOKEN_TOO_LONG; return 0; }
            out[count].ptr = dst;
            out[count].len = len;
            esc_used += len;
            ++count;
            continue;
        }

        const usize start = i;
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' && line[i] != '#') { ++i; }
        out[count].ptr = line + start;
        out[count].len = (u32)(i - start);
        ++count;
    }
    return count;
}

Result<u32> console_exec(ConsoleState* s, World* w, bool lockstep, const char* line, Span<char> reply) {
    // History gets every attempted line, success or failure (docs/TOOLING.md §3).
    const usize ll = strlen(line);
    const u32 clamped_ll = (ll < (usize)(CONSOLE_LINE_CAP - 1)) ? (u32)ll : (u32)(CONSOLE_LINE_CAP - 1);
    memcpy(s->history[s->hist_head], line, clamped_ll);
    s->history[s->hist_head][clamped_ll] = '\0';
    s->hist_head = (s->hist_head + 1u) % CONSOLE_HISTORY_CAP;
    if (s->hist_count < CONSOLE_HISTORY_CAP) { s->hist_count += 1u; }

    ConsoleToken toks[CONSOLE_MAX_TOKENS];
    char unescape_buf[CONSOLE_MAX_TOKENS * CONSOLE_TOKEN_CAP];
    ErrCode terr = ERR_OK;
    const u32 tok_count = console_tokenize(line, toks, CONSOLE_MAX_TOKENS, unescape_buf, sizeof(unescape_buf), &terr);
    if (terr != ERR_OK) { return Result<u32>{ 0, terr }; }
    if (tok_count == 0u) { return Result<u32>{ 0, ERR_CONSOLE_UNKNOWN_CMD }; }

    StrView name0{ toks[0].ptr, toks[0].len };
    const ConsoleCmd* cmd = console_find(s, name0);
    if (cmd == nullptr) { return Result<u32>{ 0, ERR_CONSOLE_UNKNOWN_CMD }; }

    const u32 argc = tok_count - 1u;
    if (argc < (u32)cmd->argc_min || argc > (u32)cmd->argc_max) { return Result<u32>{ 0, ERR_CONSOLE_ARGC }; }
    if (lockstep && (cmd->flags & CONSOLE_SIM_AFFECTING)) { return Result<u32>{ 0, ERR_CONSOLE_LOCKSTEP_REFUSED }; }

    StrView argv[CONSOLE_MAX_TOKENS];
    for (u32 i = 0; i < argc; ++i) { argv[i] = StrView{ toks[i + 1].ptr, toks[i + 1].len }; }
    return cmd->fn(w, argc, argv, reply);
}

u32 console_complete(const ConsoleState* s, StrView prefix, const ConsoleCmd** out, u32 out_cap) {
    const u32 cap = (out_cap < 32u) ? out_cap : 32u;
    if (cap == 0u) { return 0u; }

    // lower_bound: first sorted entry whose name is >= prefix (bytewise).
    u32 lo = 0, hi = s->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        const char* nm = s->cmds[s->sorted[mid]].name;
        const usize nlen = strlen(nm);
        const usize n = nlen < prefix.len ? nlen : prefix.len;
        int cmp = n > 0u ? memcmp(nm, prefix.ptr, n) : 0;
        if (cmp == 0 && nlen < prefix.len) { cmp = -1; }
        if (cmp < 0) { lo = mid + 1u; } else { hi = mid; }
    }

    u32 n = 0;
    for (u32 i = lo; i < s->count && n < cap; ++i) {
        const char* nm = s->cmds[s->sorted[i]].name;
        const usize nlen = strlen(nm);
        if (nlen < prefix.len || memcmp(nm, prefix.ptr, prefix.len) != 0) { break; }
        out[n++] = &s->cmds[s->sorted[i]];
    }
    return n;
}

const char* console_history_at(const ConsoleState* s, u32 i) {
    TL_CHECK(i < s->hist_count);
    // hist_head is the NEXT write slot; the oldest live line is hist_count behind it (or, once
    // wrapped, exactly hist_head itself - the standard ring "oldest = head - count" identity).
    const u32 idx = (s->hist_head + CONSOLE_HISTORY_CAP - s->hist_count + i) % CONSOLE_HISTORY_CAP;
    return s->history[idx];
}

void console_panel_register(Editor* ed) { editor_register_panel(ed, "Console", console_panel_draw, true); }

void console_panel_draw(Editor* ed, World* w) {
    if (!ImGui::Begin("Console")) { ImGui::End(); return; }

    ImGui::BeginChild("##console_history", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true);
    for (u32 i = 0; i < ed->console.hist_count; ++i) {
        ImGui::TextUnformatted(console_history_at(&ed->console, i));
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) { ImGui::SetScrollHereY(1.0f); }
    ImGui::EndChild();

    if (ed->console_last_reply[0] != '\0' || ed->console_last_err != ERR_OK) {
        ImGui::TextUnformatted(console_err_name(ed->console_last_err));
        ImGui::SameLine();
        ImGui::TextUnformatted(ed->console_last_reply);
    }

    ImGui::SetNextItemWidth(-1);
    const bool submitted = ImGui::InputText("##console_input", ed->console_input, sizeof(ed->console_input),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    if (submitted && ed->console_input[0] != '\0') {
        // No netcode/Hovel session exists yet to ask, so lockstep is always false here - the real
        // source is a follow-up once that lands (TODO.md), not invented in this panel. Zeroed in
        // full, not just [0]: a ConsoleFn's Result<u32>.value is "bytes written", with no promise
        // of a trailing NUL past that, so the buffer must already be NUL past whatever it writes.
        char reply[sizeof(ed->console_last_reply)];
        memset(reply, 0, sizeof(reply));
        const Result<u32> r = console_exec(&ed->console, w, /*lockstep=*/false, ed->console_input,
                                            Span<char>{ reply, (u32)(sizeof(reply) - 1u) });
        ed->console_last_err = r.err;
        memcpy(ed->console_last_reply, reply, sizeof(reply));
        ed->console_input[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
}
