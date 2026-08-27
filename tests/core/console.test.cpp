// console.test.cpp - tokenizer (quotes/escapes/comment/overflow/syntax), registration+find,
// dispatch (argc bounds, lockstep refusal), completion, history.
// Spec: docs/TOOLING.md §3, §9.2, §9.3.5. Rubric: docs/TESTING.md §7.
//
// TL_TEST's generated signature is `(TestCtx* t)` - every ConsoleState local here is `cs`.
#include "runner/tl_test.h"
#include "core/console.h"

#include <stdio.h>   // snprintf - tests/ carries the printf-class io exemption (docs/TESTING.md section 8 R-2)
#include <string.h>

namespace {

Result<u32> fn_echo(World*, u32 argc, const StrView* argv, Span<char> reply) {
    if (argc == 0u) { return Result<u32>{ 0, ERR_OK }; }
    const u32 n = (argv[0].len < reply.count) ? argv[0].len : reply.count;
    memcpy(reply.data, argv[0].ptr, n);
    return Result<u32>{ n, ERR_OK };
}

Result<u32> fn_noop(World*, u32, const StrView*, Span<char>) { return Result<u32>{ 0, ERR_OK }; }

ConsoleCmd cmd_echo() {
    ConsoleCmd c{};
    c.key = "echo"_id; c.name = "echo"; c.usage = "echo <word>"; c.fn = fn_echo;
    c.argc_min = 1; c.argc_max = 1;
    return c;
}
ConsoleCmd cmd_noop(const char* name, u8 flags = 0, u8 argc_min = 0, u8 argc_max = 0) {
    ConsoleCmd c{};
    c.key = fnv1a64(name, strlen(name)); c.name = name; c.usage = name; c.fn = fn_noop;
    c.flags = flags; c.argc_min = argc_min; c.argc_max = argc_max;
    return c;
}

void build(ConsoleState* cs) {
    console_init(cs);
    ConsoleCmd echo = cmd_echo();
    console_register(cs, &echo);
    ConsoleCmd sp = cmd_noop("spawn", 0, 1, 3);
    console_register(cs, &sp);
    ConsoleCmd sim = cmd_noop("set_speculation", CONSOLE_SIM_AFFECTING, 1, 1);
    console_register(cs, &sim);
    ConsoleCmd sc = cmd_noop("scale", 0, 0, 2);
    console_register(cs, &sc);
}

}  // namespace

TL_TEST(console_register_and_find, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    TL_ASSERT_EQ(cs.count, 4u);
    TL_ASSERT_NOT_NULL(console_find(&cs, sv_lit("echo")));
    TL_EXPECT_EQ(console_find(&cs, sv_lit("echo"))->key, "echo"_id);
    TL_EXPECT_NULL(console_find(&cs, sv_lit("nope")));
    TL_EXPECT_NULL(console_find(&cs, sv_lit("ech")));    // no partial match on find (only complete)
}

TL_TEST(console_sorted_index_is_bytewise_ascending, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    for (u32 i = 1; i < cs.count; ++i) {
        TL_EXPECT_LT(strcmp(cs.cmds[cs.sorted[i - 1]].name, cs.cmds[cs.sorted[i]].name), 0);
    }
}

TL_TEST(console_tokenize_basic_split, "core,editor,console,fast") {
    ConsoleToken toks[CONSOLE_MAX_TOKENS];
    char scratch[CONSOLE_MAX_TOKENS * CONSOLE_TOKEN_CAP];
    ErrCode err;
    const u32 n = console_tokenize("spawn  goblin \t 10 20", toks, CONSOLE_MAX_TOKENS, scratch, sizeof(scratch), &err);
    TL_ASSERT_EQ(err, ERR_OK);
    TL_ASSERT_EQ(n, 4u);
    TL_EXPECT_EQ(memcmp(toks[0].ptr, "spawn", 5), 0); TL_EXPECT_EQ(toks[0].len, 5u);
    TL_EXPECT_EQ(memcmp(toks[1].ptr, "goblin", 6), 0); TL_EXPECT_EQ(toks[1].len, 6u);
    TL_EXPECT_EQ(memcmp(toks[2].ptr, "10", 2), 0);
    TL_EXPECT_EQ(memcmp(toks[3].ptr, "20", 2), 0);
}

TL_TEST(console_tokenize_quoted_with_escapes, "core,editor,console,fast") {
    ConsoleToken toks[CONSOLE_MAX_TOKENS];
    char scratch[CONSOLE_MAX_TOKENS * CONSOLE_TOKEN_CAP];
    ErrCode err;
    const u32 n = console_tokenize("say \"hi \\\"there\\\" \\\\ world\" done", toks, CONSOLE_MAX_TOKENS, scratch, sizeof(scratch), &err);
    TL_ASSERT_EQ(err, ERR_OK);
    TL_ASSERT_EQ(n, 3u);
    TL_EXPECT_EQ(memcmp(toks[0].ptr, "say", 3), 0);
    TL_ASSERT_EQ(toks[1].len, (u32)strlen("hi \"there\" \\ world"));
    TL_EXPECT_EQ(memcmp(toks[1].ptr, "hi \"there\" \\ world", toks[1].len), 0);
    TL_EXPECT_EQ(memcmp(toks[2].ptr, "done", 4), 0);
}

TL_TEST(console_tokenize_comment_drops_rest_of_line, "core,editor,console,fast") {
    ConsoleToken toks[CONSOLE_MAX_TOKENS];
    char scratch[CONSOLE_MAX_TOKENS * CONSOLE_TOKEN_CAP];
    ErrCode err;
    const u32 n = console_tokenize("scale 2 # trailing comment ignored", toks, CONSOLE_MAX_TOKENS, scratch, sizeof(scratch), &err);
    TL_ASSERT_EQ(err, ERR_OK);
    TL_ASSERT_EQ(n, 2u);
    TL_EXPECT_EQ(memcmp(toks[1].ptr, "2", 1), 0);
}

TL_TEST(console_tokenize_too_many_tokens_is_error, "core,editor,console,fast") {
    ConsoleToken toks[4];
    char scratch[4 * CONSOLE_TOKEN_CAP];
    ErrCode err;
    const u32 n = console_tokenize("a b c d e", toks, 4u, scratch, sizeof(scratch), &err);
    TL_EXPECT_EQ(err, ERR_CONSOLE_TOO_MANY_ARGS);
    TL_EXPECT_EQ(n, 0u);
}

TL_TEST(console_tokenize_unterminated_quote_is_syntax_error, "core,editor,console,fast") {
    ConsoleToken toks[CONSOLE_MAX_TOKENS];
    char scratch[CONSOLE_MAX_TOKENS * CONSOLE_TOKEN_CAP];
    ErrCode err;
    const u32 n = console_tokenize("say \"never closed", toks, CONSOLE_MAX_TOKENS, scratch, sizeof(scratch), &err);
    TL_EXPECT_EQ(err, ERR_CONSOLE_SYNTAX);
    TL_EXPECT_EQ(n, 0u);
}

TL_TEST(console_exec_dispatches_and_returns_reply, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    Result<u32> r = console_exec(&cs, nullptr, false, "echo hello", Span<char>{ reply, sizeof(reply) });
    TL_ASSERT_EQ(r.err, ERR_OK);
    TL_ASSERT_EQ(r.value, 5u);
    TL_EXPECT_EQ(memcmp(reply, "hello", 5), 0);
}

TL_TEST(console_exec_unknown_command, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    Result<u32> r = console_exec(&cs, nullptr, false, "nope 1 2", Span<char>{ reply, sizeof(reply) });
    TL_EXPECT_EQ(r.err, ERR_CONSOLE_UNKNOWN_CMD);
}

TL_TEST(console_exec_argc_out_of_bounds, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    TL_EXPECT_EQ(console_exec(&cs, nullptr, false, "echo", Span<char>{ reply, sizeof(reply) }).err, ERR_CONSOLE_ARGC);
    TL_EXPECT_EQ(console_exec(&cs, nullptr, false, "echo a b", Span<char>{ reply, sizeof(reply) }).err, ERR_CONSOLE_ARGC);
    TL_EXPECT_EQ(console_exec(&cs, nullptr, false, "scale", Span<char>{ reply, sizeof(reply) }).err, ERR_OK);   // argc_min 0
}

TL_TEST(console_exec_sim_affecting_refused_under_lockstep, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    TL_EXPECT_EQ(console_exec(&cs, nullptr, false, "set_speculation 1", Span<char>{ reply, sizeof(reply) }).err, ERR_OK);
    TL_EXPECT_EQ(console_exec(&cs, nullptr, true, "set_speculation 1", Span<char>{ reply, sizeof(reply) }).err, ERR_CONSOLE_LOCKSTEP_REFUSED);
    // A non-SIM_AFFECTING command is unaffected by lockstep.
    TL_EXPECT_EQ(console_exec(&cs, nullptr, true, "scale", Span<char>{ reply, sizeof(reply) }).err, ERR_OK);
}

TL_TEST(console_exec_appends_history_even_on_failure, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    TL_ASSERT_EQ(cs.hist_count, 0u);
    (void)console_exec(&cs, nullptr, false, "echo a", Span<char>{ reply, sizeof(reply) });
    (void)console_exec(&cs, nullptr, false, "nope", Span<char>{ reply, sizeof(reply) });   // fails, still recorded
    TL_ASSERT_EQ(cs.hist_count, 2u);
    TL_EXPECT_EQ(strcmp(console_history_at(&cs, 0), "echo a"), 0);
    TL_EXPECT_EQ(strcmp(console_history_at(&cs, 1), "nope"), 0);
}

TL_TEST(console_history_wraps_past_capacity, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    char reply[64];
    char line[32];
    for (u32 i = 0; i < CONSOLE_HISTORY_CAP + 3u; ++i) {
        snprintf(line, sizeof(line), "line%u", i);
        (void)console_exec(&cs, nullptr, false, line, Span<char>{ reply, sizeof(reply) });
    }
    TL_ASSERT_EQ(cs.hist_count, (u32)CONSOLE_HISTORY_CAP);
    // Oldest surviving line is #3 (0..2 were overwritten); newest is #(CAP+2).
    TL_EXPECT_EQ(strcmp(console_history_at(&cs, 0), "line3"), 0);
    TL_EXPECT_EQ(strcmp(console_history_at(&cs, (u32)CONSOLE_HISTORY_CAP - 1u), "line66"), 0);
}

TL_TEST(console_complete_prefix_walk, "core,editor,console,fast") {
    ConsoleState cs;
    build(&cs);
    const ConsoleCmd* out[8];
    // "s" matches "scale", "set_speculation", "spawn" (bytewise order).
    u32 n = console_complete(&cs, sv_lit("s"), out, 8u);
    TL_ASSERT_EQ(n, 3u);
    TL_EXPECT_EQ(strcmp(out[0]->name, "scale"), 0);
    TL_EXPECT_EQ(strcmp(out[1]->name, "set_speculation"), 0);
    TL_EXPECT_EQ(strcmp(out[2]->name, "spawn"), 0);

    n = console_complete(&cs, sv_lit("sp"), out, 8u);
    TL_ASSERT_EQ(n, 1u);
    TL_EXPECT_EQ(strcmp(out[0]->name, "spawn"), 0);

    n = console_complete(&cs, sv_lit("zzz"), out, 8u);
    TL_EXPECT_EQ(n, 0u);

    n = console_complete(&cs, sv_lit(""), out, 8u);   // empty prefix matches everything
    TL_EXPECT_EQ(n, 4u);
}
