// console.h - tokenizer, registration (name-sorted index, linear-scan dispatch lookup),
// completion, history. Spec: docs/TOOLING.md §3, §9.2, §9.3.5.
#include "core/console.h"

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
                if (len < dst_cap_left) { dst[len] = c; }   // TL_CHECK'd via the header contract: excess truncates, never overflows
                ++len;
                ++i;
            }
            if (!closed) { *out_err = ERR_CONSOLE_SYNTAX; return 0; }
            const u32 clamped = (len < dst_cap_left) ? len : dst_cap_left;
            out[count].ptr = dst;
            out[count].len = clamped;
            esc_used += clamped;
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
