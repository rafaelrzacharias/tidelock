#pragma once
// ---------------------------------------------------------------------------------------------
// console.h - ConsoleCmd, the tokenizer, dispatch, completion, and history.
//
// Spec: docs/TOOLING.md §3 (design), §9.2 (ConsoleCmd, this header's struct), §9.3.5 (tokenizer/
//   dispatch/completion algorithm); docs/CPP-SUBSET.md §3 (Result<T>/ErrCode).
// Purpose: `console_register("spawn", fn, "spawn <name> <x> <y>", arg_hints)` builds a name-
//   sorted command table; `console_exec` tokenizes one line and dispatches it. Every
//   sim-affecting command is refused outright in a lockstep session (the caller states whether
//   one is live via `console_exec`'s `lockstep` parameter - this module has no session concept
//   of its own, matching cvar.h's caller-owned shape).
// Invariants: `CONSOLE_TABLE_CAP` (512) live commands, keyed by name hash, duplicate registration
//   is TL_FATAL (matching cvar_register/TL_COMPONENT's precedent - init-time misconfiguration).
//   `sorted` is a name-BYTEWISE-ascending index over `cmds` (insertion sort at register time, not
//   perf-sensitive) - completion (docs/TOOLING.md §9.3.5) walks a `lower_bound` prefix range over
//   it; dispatch itself looks up by NAME HASH (a separate, unordered scan - CONSOLE_TABLE_CAP is
//   512, so a linear scan is cheap and this module has no Map<K,V> dependency to keep it caller-
//   owned/self-contained like cvar.h). Tokenizer: ASCII space/tab split; a `"`-quoted token keeps
//   spaces and honours `\"`/`\\`; `#` starts a comment (rest of the line dropped); max
//   `CONSOLE_MAX_TOKENS` (16) tokens; an unterminated quote is a syntax error. History is a fixed
//   64-line ring (`char[64][CONSOLE_LINE_CAP]`), not a `RingBuffer<T>` (no VMemArena dependency -
//   same self-contained shape as cvar.h's `CvarTable`).
// Determinism: never hashed, never snapshotted - console output and history are dev-plane state
//   (docs/CPP-SUBSET.md §9 R-4's tooling-plane reasoning, though this module is caller-owned
//   rather than a static, so it doesn't need the RR-7 exemption itself). A `ConsoleFn` that
//   mutates sim state MUST do so only through a command (`core/commands.h`'s `CMD_*` kinds) -
//   this header cannot enforce that at the type level; it is the contract every `ConsoleFn`
//   implementation must honour (docs/TOOLING.md §0 "tools never poke sim state").
// Threading: none - a ConsoleState is caller-owned, single-threaded (dev console input is
//   render-rate, not tick-rate).
// Tier: dev only (docs/TOOLING.md §9.1 file table: `editor/console.cpp`) - moved here from
// core/console.h/.cpp, where it was first built (w3-editor's own mistake: cvar.h is legitimately
// all-tier, this is not - the console UI/REPL has no reason to exist in netcode/ship, unlike a
// CVAR_SIM cvar's own storage). `src/editor/` compiles only on debug/dev tiers (its CMakeLists.txt
// is empty on netcode/ship), so this file's tier gating is structural (by directory), not a
// `#if TL_DEV` inside it.
// Includes: foundation/tl_types.h, foundation/tl_assert.h, foundation/hash.h, foundation/strview.h,
//   core/world.h (ConsoleFn's World* parameter only - no World field is added by this header).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/hash.h"
#include "foundation/strview.h"
#include "core/world.h"

// core module's console sub-range (0x037x; see core/cvar.h's contract block for the rest of the
// 0x03xx block's layout, including the pre-existing 0x0321/0x0322 collision it notes).
constexpr ErrCode ERR_CONSOLE_UNKNOWN_CMD      = (ErrCode)0x0370;  // no command registered under that name
constexpr ErrCode ERR_CONSOLE_TOO_MANY_ARGS    = (ErrCode)0x0371;  // more than CONSOLE_MAX_TOKENS tokens
constexpr ErrCode ERR_CONSOLE_SYNTAX           = (ErrCode)0x0372;  // unterminated quote
constexpr ErrCode ERR_CONSOLE_ARGC             = (ErrCode)0x0373;  // argc outside [argc_min, argc_max]
constexpr ErrCode ERR_CONSOLE_LOCKSTEP_REFUSED = (ErrCode)0x0374;  // SIM_AFFECTING command under lockstep=true
constexpr ErrCode ERR_CONSOLE_DUPLICATE        = (ErrCode)0x0375;  // name already registered (TL_FATAL site)
constexpr ErrCode ERR_CONSOLE_TABLE_FULL       = (ErrCode)0x0376;  // CONSOLE_TABLE_CAP already live (TL_FATAL site)

// The literal name of one of this header's ErrCodes (or "ERR_OK"/"ERR_?"), for logging. Pure.
inline const char* console_err_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_CONSOLE_UNKNOWN_CMD ? "ERR_CONSOLE_UNKNOWN_CMD"
         : e == ERR_CONSOLE_TOO_MANY_ARGS ? "ERR_CONSOLE_TOO_MANY_ARGS"
         : e == ERR_CONSOLE_SYNTAX ? "ERR_CONSOLE_SYNTAX"
         : e == ERR_CONSOLE_ARGC ? "ERR_CONSOLE_ARGC"
         : e == ERR_CONSOLE_LOCKSTEP_REFUSED ? "ERR_CONSOLE_LOCKSTEP_REFUSED"
         : e == ERR_CONSOLE_DUPLICATE ? "ERR_CONSOLE_DUPLICATE"
         : e == ERR_CONSOLE_TABLE_FULL ? "ERR_CONSOLE_TABLE_FULL" : "ERR_?";
}

enum { CONSOLE_MAX_TOKENS = 16, CONSOLE_TOKEN_CAP = 128, CONSOLE_TABLE_CAP = 512 };
enum { CONSOLE_HISTORY_CAP = 64, CONSOLE_LINE_CAP = 256 };
enum : u8 { CONSOLE_SIM_AFFECTING = 1 };

// docs/TOOLING.md §9.3.5: `argc`/`argv` exclude the command name itself (argv[0] in the doc's own
// wording is the first ARGUMENT). `reply` is where the command writes its human-readable result;
// the return value's `err` is ERR_OK or a command-specific ErrCode, `value` the bytes written to
// `reply` (0 on failure). `w` may be null for a command that touches no World (console self-test,
// `help`, etc.) - a ConsoleFn that needs one must TL_CHECK it itself.
typedef Result<u32> (*ConsoleFn)(World* w, u32 argc, const StrView* argv, Span<char> reply);

// docs/TOOLING.md §9.2, verbatim shape (`lua_ref`/`from_luau` are placeholders - no Luau binding
// exists yet, docs/LUAU-LAYER.md's binding layer is a different, not-yet-built lane; a call
// through them is unreachable today, not merely untested).
struct ConsoleCmd {
    NameHash    key;
    const char* name;
    const char* usage;
    ConsoleFn   fn;
    const char* arg_hints[4];   // "entity" | "cvar" | "cmd" | "file:<dir>" | "enum:a|b|c" | null
    u8          flags;          // CONSOLE_SIM_AFFECTING
    u8          argc_min, argc_max;
    u8          from_luau;
    u32         lua_ref;
};

// docs/TOOLING.md §9.3.5: a name-sorted (bytewise) index over `cmds` for completion; dispatch
// looks up by name hash via a linear scan (this header's contract block explains why). History
// is a fixed ring; `hist_head` is the NEXT write slot, `hist_count` caps at CONSOLE_HISTORY_CAP.
struct ConsoleState {
    ConsoleCmd cmds[CONSOLE_TABLE_CAP];
    u16        sorted[CONSOLE_TABLE_CAP];   // name-bytewise-ascending index into cmds
    u32        count;
    char       history[CONSOLE_HISTORY_CAP][CONSOLE_LINE_CAP];
    u32        hist_head, hist_count;
};

// Zero-initializes `s` (count = 0, no history).
void console_init(ConsoleState* s);

// Registers `cmd` (copied by value - the caller's `ConsoleCmd` need not outlive the call, unlike
// cvar.h's CvarDesc-by-pointer shape, since ConsoleCmd carries no per-instance mutable state to
// alias). TL_FATAL: table full, duplicate name hash (registration-time misconfiguration, matching
// cvar_register's precedent).
void console_register(ConsoleState* s, const ConsoleCmd* cmd);

// Binary search on `sorted` (bytewise name order) for an exact name match. Null when unknown.
const ConsoleCmd* console_find(const ConsoleState* s, StrView name);

// One resolved token from console_tokenize.
struct ConsoleToken { const char* ptr; u32 len; };

// Tokenizes `line` into `out` (capacity `out_cap`, normally CONSOLE_MAX_TOKENS): splits on
// unquoted ASCII space/tab; a `"`-quoted token keeps embedded spaces and unescapes `\"`/`\\` (no
// other escape is recognised - a bare backslash before any other character is copied literally,
// matching a permissive shell-lite reading since docs/TOOLING.md §9.3.5 only names those two);
// `#` outside a quote starts a comment (the rest of the line is dropped, never tokenized).
// Returns the token count, or a negative-shaped failure via `out_err`: ERR_CONSOLE_TOO_MANY_ARGS
// (more than `out_cap` tokens) or ERR_CONSOLE_SYNTAX (unterminated quote). `out[i].ptr` points
// into `line` (or into `unescape_buf`/`unescape_cap` for a token that needed unescaping - a
// caller-supplied scratch buffer, since this module has no arena of its own); TL_CHECK:
// `unescape_cap` is at least as large as the longest quoted token actually seen, else the token
// is truncated rather than overflowing (never a buffer overrun).
u32 console_tokenize(const char* line, ConsoleToken* out, u32 out_cap,
                      char* unescape_buf, u32 unescape_cap, ErrCode* out_err);

// Tokenizes `line`, resolves the command by its first token, checks argc bounds and (when
// `lockstep`) CONSOLE_SIM_AFFECTING, then calls `fn(w, argc-1, argv+1, reply)`. Appends `line` to
// history unconditionally (even on failure - docs/TOOLING.md §3's console log gets every
// attempted line). Returns the same Result<u32> shape as ConsoleFn; a dispatch-level failure
// (unknown command, syntax, argc, lockstep refusal) never calls `fn` and `value` is 0.
Result<u32> console_exec(ConsoleState* s, World* w, bool lockstep, const char* line, Span<char> reply);

// The completion set for `prefix` (docs/TOOLING.md §9.3.5's "lower_bound on the typed prefix,
// walk while prefix matches", capped at 32 shown): writes up to `out_cap` matching command names
// (bytewise prefix match against `sorted`) into `out[i]` and returns the match count actually
// written (never more than `out_cap`, and never more than 32 regardless of `out_cap` - docs/
// TOOLING.md §9.3.5's own cap). `out[i]` are `ConsoleCmd*` into `s->cmds` (stable while `s` is
// not further registered into).
u32 console_complete(const ConsoleState* s, StrView prefix, const ConsoleCmd** out, u32 out_cap);

// history[i] in OLDEST-to-newest order (i=0 is the oldest live line, i=hist_count-1 the most
// recent - matching tl_log.h's tl_log_test_ring_at "write order" convention). Fatal if
// i >= s->hist_count.
const char* console_history_at(const ConsoleState* s, u32 i);
