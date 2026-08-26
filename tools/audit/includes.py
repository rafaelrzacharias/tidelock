#!/usr/bin/env python3
"""Source-discipline gate over src/. Spec: docs/CPP-SUBSET.md §1, §4, §6; docs/ARCHITECTURE.md
§1; docs/CANON.md; docs/TESTING.md §5. vendor/ and tools/ are exempt by design; tests/ has the io
exemption of docs/TESTING.md §8 R-2.

What a *regex* can prove is only half the job - the other half is docs/CPP-SUBSET.md §4's link
gate (tools/audit/symbols.py), which is where mutable globals and stray libm calls are actually
caught. This file covers what never becomes a symbol.

Gates, all blocking:
  1. system includes outside the per-directory allowlist
  2. backend headers outside their wrap module
  3. the module DAG: every quoted include is a module below the includer (docs/ARCHITECTURE.md §1)
  4. sim TUs: no float/double/f32/f64, and no target-variable integer type. `char` is signed on
     x86-64 and unsigned on aarch64; `long` is 32-bit on Windows and 64-bit on Linux. Either one
     changes a tick's bytes between a PC peer and a Pi peer with no UB anywhere - which is why
     docs/BUILD.md §9 R-8 can only claim what it claims with this ban enforced.
  5. everywhere in src/: no std::, no thread_local in any spelling, no mutable namespace-scope or
     static-storage variable, no asm, __rdtsc or __builtin_ia32_* (docs/CPP-SUBSET.md §4)
  6. header contracts: a public header opens with a contract block naming its spec section, and
     every public function - declaration, inline definition or template - carries a contract
     comment of its own
  7. NOMINMAX before every <windows.h>, in src/ AND tests/ (docs/LESSONS.md; ruled 2026-08-24).
     Gate 1 confines windows.h to src/platform and the two impl_* dirs, and tests/ may spell it
     under docs/TESTING.md §8 R-2 - so the sites are enumerable, which is what makes this
     checkable at all. This is the ONE gate that walks tests/ as well as src/.

Comments and string literals are blanked before any token check, so prose about floats does not
fail a sim header.
"""
import argparse, os, re, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SCAN_EXT = (".h", ".hpp", ".inc", ".cpp")

SYS_ALLOW = {"stdint.h", "stddef.h", "string.h", "limits.h"}
SYS_ALLOW_DIRS = {                        # additional system headers, by path prefix
    "src/render": {"math.h"},
    "src/editor": {"math.h"},
    "src/platform": {"math.h"},
    "src/foundation": {"rapidhash.h"},
    # Luau is vendored with its own include dirs on the target (vendor/luau/CMakeLists.txt),
    # so its public headers are spelled bare and reach gate 1 as system includes. The set is
    # closed: lua.h (the C API), lualib.h (luaopen_*/luaL_*), luacode.h (luau_compile).
    "src/script": {"lua.h", "lualib.h", "luacode.h"},
    # The vendor allocator hookups (docs/MEMORY.md §8.6) include their lib's own headers to reach
    # its SetMemoryFunctions/SetAllocatorFunctions/STBI_MALLOC hook API. stdlib.h is here because
    # one vendored buffer - the one luau_compile returns - is malloc'd by upstream contract and
    # must be free()d; the grant also admits malloc, and the folder whose job is allocators is
    # the only one that may have it.
    "src/vendor_glue": {"SDL3/SDL.h", "imgui.h", "enet/enet.h", "stb_image.h", "stb_sprintf.h",
                        "stdarg.h", "stdlib.h"},
    # core/loaders/image.cpp reaches stb_image.h declaration-only (docs/ASSETS-AND-DATA.md §8.2:
    # "pixels = stbi_load_from_memory(...)"); the one real STB_IMAGE_IMPLEMENTATION TU is
    # vendor/stb/stb_impl.c (STBI_MALLOC hooked to pool_vendor there), linked in via the `stb`
    # CMake target - same declaration-only shape src/vendor_glue/stb_glue.cpp already uses.
    # BACKEND_HEADERS' "stb_" token already named src/core as an allowed prefix; this completes
    # the other half of the same grant (both gates check independently, LESSONS.md).
    "src/core": {"stb_image.h"},
}
BACKEND_FREE = ("src/platform/impl_sdl3", "src/platform/impl_headless")   # OS headers live here


def is_backend_free(rel):
    """True where a real OS header is legal. The two impl_* dirs, plus - docs/PLATFORM.md §9.1 -
    the os_*.cpp TUs that sit directly in src/platform/ (not under either impl_*): they are the
    single implementation shared by both impls (os_win_vmem.cpp/os_posix_vmem.cpp,
    os_entropy.cpp, os_file_atomic.cpp, os_crash_win.cpp/os_crash_posix.cpp), so they cannot live
    under impl_sdl3 or impl_headless without being compiled twice or picking a fake owner. The
    "os_" prefix is the doc's own naming convention, not a filename list to keep in sync here."""
    if any(rel.startswith(p) for p in BACKEND_FREE):
        return True
    d, base = os.path.split(rel)
    return d == "src/platform" and base.startswith("os_") and base.endswith(".cpp")

BACKEND_HEADERS = {                       # token in the include path -> allowed path prefixes
    # The vendor allocator hookups (docs/MEMORY.md §8.6) include their lib's own headers to
    # reach its hook API, so src/vendor_glue joins each wrap module's prefix.
    "SDL3": ("src/platform/impl_sdl3", "src/vendor_glue"),
    "SDL_ttf": ("src/platform/impl_sdl3", "src/vendor_glue"),
    "imgui": ("src/editor", "src/vendor_glue"),
    "enet": ("src/net", "src/vendor_glue"),
    # The vendored Luau tree spells its public headers bare (<lua.h>, <lualib.h>, <luacode.h>)
    # and its internal ones under Luau/. The pre-vendoring token here was a bare "luau",
    # which matched no upstream spelling at all AND matched our own
    # #include "vendor_glue/luau_alloc.h" - a gate that fires on the wrong file and never
    # on the right one. Measured against the real tree at the 0.696 pin (W2 luau-vm).
    "lua.h": ("src/script",),
    "lualib.h": ("src/script",),
    "luacode.h": ("src/script",),
    "Luau/": ("src/script",),
    "monocypher": ("src/net",),
    "stb_": ("src/platform/impl_sdl3", "src/core", "src/vendor_glue"),
    "rapidhash": ("src/foundation",),
}

# The include DAG of docs/ARCHITECTURE.md §1: module -> the modules it may include (itself
# included). Dependencies point down only; render is the one module allowed a sim header, and
# only sim/views.h.
MODULE_DAG = {
    "foundation": ("foundation",),
    # vendor_glue (docs/PLATFORM.md §9.5, docs/MEMORY.md §8.6): per-lib allocator hookups. It
    # sits directly above foundation and below every wrap module - it never touches
    # core/platform/render, and its vendor headers arrive via BACKEND_HEADERS-gated system
    # includes, not this local-include DAG.
    "vendor_glue": ("vendor_glue", "foundation"),
    # platform/net/editor carry a DOWNWARD-only entry to vendor_glue (review round 1, D3):
    # sdl3_glue.h names the impl_sdl3 platform lane as `vendor_glue_sdl3_install()`'s caller,
    # enet_glue.h names net/, imgui_glue.h names src/editor. script's entry is the luau glue's
    # (vendor_glue/luau_alloc.h). sim/foundation are deliberately left out: neither has any
    # vendored-lib install to make, and sim especially must never reach a vendor allocator.
    "platform": ("platform", "foundation", "vendor_glue"),
    "sim": ("sim", "foundation"),
    "core": ("core", "foundation", "platform"),
    "render": ("render", "core", "foundation", "platform"),
    "net": ("net", "core", "foundation", "platform", "vendor_glue"),
    "editor": ("editor", "core", "render", "foundation", "platform", "vendor_glue"),
    "script": ("script", "core", "foundation", "platform", "vendor_glue"),
    "app": ("app", "editor", "net", "render", "script", "sim", "core", "platform",
            "vendor_glue", "foundation"),
}
RENDER_SIM_HEADER = "sim/views.h"

# Named exceptions to the sim-TU type bans, both from the docs: tl_types.h must declare f32/f64
# (docs/CPP-SUBSET.md §1) and fx_float.h is the render/editor/tools bridge (docs/FX-PALETTE.md §6).
TYPE_EXEMPT_PATHS = {"src/foundation/tl_types.h", "src/foundation/fx_float.h"}
# The panic ABI (docs/CPP-SUBSET.md §9 R-3): tl_assert.h is the one non-det-STEM header a sim TU
# may include. Its runtime (tl_assert.cpp, the crash writer) reaches io and stays in the non-det
# half, but the header is three extern "C" declarations and three macros, and every TL_CHECK in
# fx.h needs it. The stem-keyed split cannot express "header det, runtime non-det", so the
# exemption is by full path - the .cpp is still barred.
PANIC_ABI_HEADER = "foundation/tl_assert.h"
# docs/PLATFORM.md §5: EntropyApi's verb "is absent from every sim lib's include path" and, more
# than that, from every module but the one that implements it and the two that legitimately need
# OS randomness - net/ (keygen, nonces, commit/reveal) and app/ (init wiring). The MODULE_DAG
# above only bars it from sim/foundation (they cannot reach src/platform/* at all); this closes
# the gap for core/render/editor/script, which the DAG otherwise lets include anything under
# platform/. A doc sentence naming a restriction with no code behind it is a phantom gate
# (LESSONS.md) - this is that gate, for real, not just the CI grep §5 also names.
ENTROPY_HEADER = "platform/entropy.h"
ENTROPY_ALLOWED_MODULES = ("platform", "net", "app")
_ENTROPY_CARRIERS = {}                    # root -> frozenset of headers that expose the verb
# RR-7 (docs/CPP-SUBSET.md §1): the tooling plane's io allowance. Narrower than SYS_ALLOW's base
# set - `<math.h>` is still not granted here, and neither is any OS header; the crash writer's raw
# OS calls belong to platform/, not foundation/ (docs/TOOLING.md §9.3.9).
TOOLING_SYS_ALLOW = {"stdio.h", "stdlib.h", "stdarg.h"}

# docs/CPP-SUBSET.md §1's writable-static ban has exactly two exemptions, and BOTH live in one
# file so neither gate can carry a copy that drifts: tools/audit/static_allow.txt. RR-7's
# tooling plane is the other, and it lives in src/foundation/CMakeLists.txt for the same reason.
# Keyed by DIRECTORY + STEM here (this gate sees paths) and by LIB + STEM in symbols.py (that one
# sees archives); a row must match both or the exemption is only half real.
STATIC_ALLOW_FILE = os.path.join("tools", "audit", "static_allow.txt")


def static_allow_dirs(root):
    """{(directory, stem)} that may hold namespace-scope mutable state. Parsed, never guessed: a
    malformed row is a hard error rather than a silently empty exemption set, because an empty
    set here looks exactly like a correct one until the day it does not."""
    out = set()
    path = os.path.join(root, STATIC_ALLOW_FILE)
    if not os.path.exists(path):
        return out
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        row = line.split("#")[0].split()
        if not row:
            continue
        if len(row) != 3:
            sys.exit("%s:%d: want '<lib> <directory> <stem>', got %r" % (STATIC_ALLOW_FILE, n, line.strip()))
        out.add((row[1], row[2]))
    return out


INC_SYS = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
INC_LOCAL = re.compile(r'^\s*#\s*include\s*"([^"]+)"')

# `static`/namespace-scope MUTABLE state. A static *function* has internal linkage and no state;
# `static u32 const K` is a constant. Only a variable with no const/constexpr qualifier counts.
# (docs/CPP-SUBSET.md §1's own `^static [^c]` grep has this same defect and is corrected there.)
STATIC_DECL = re.compile(r'^\s*static\s+(?!_assert\b)(.*)$')
CONST_QUAL = re.compile(r'\b(const|constexpr|consteval|constinit)\b')

FLOAT_TOKENS = re.compile(r'\b(float|double|f32|f64|_Float16|_Float32|_Float64|_Float128|__fp16|__bf16|__float128)\b')
# A floating literal is a float with no type token in sight: `decltype(1.0) x`, `auto y = 0x1p3`,
# `k * 2.5` (the product is a double). Checked on the comments-and-strings-blanked text. The
# three spellings: decimal with a point, decimal with an exponent, hex with a binary exponent.
# `a.v` / `p.x` never match (a digit is required on at least one side of the point and nothing
# alphanumeric may precede it), and `1'000` digit separators are allowed inside.
FLOAT_LITERAL = re.compile(
    r"(?<![\w.])(?:\d[\d']*\.\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlL]?(?![\w.])"
    r"|\b\d[\d']*[eE][+-]?\d+[fFlL]?\b"
    r"|\b0[xX][0-9a-fA-F']*(?:\.[0-9a-fA-F']*)?[pP][+-]?\d+[fFlL]?\b")
# An integer literal suffixed L/l (or UL/LU in either case) is a `long` with no `long` token:
# `decltype(1L) x` is 32-bit on Windows and 64-bit on Linux. `LL`/`ULL` stay legal (long long is
# 64-bit on every target). The W1 fx review planted all three of these past the token bans.
LONG_SUFFIX = re.compile(r"\b(?:0[xX][0-9a-fA-F']+|\d[\d']*)(?:[uU][lL]|[lL][uU]?)(?![lL\w])")
# Target-variable integer spellings. `int`/`short` are 32/16-bit on all three targets and are not
# banned here; `long`, `char` and `wchar_t` are not.
# size_t/ptrdiff_t/intptr_t/max_align_t are the same class `usize` was: the same width on all three
# targets but not reliably the same TYPE, so an overload or specialisation keyed on one selects
# different code per target (docs/CANON.md).
# The int_fast/int_least families are the measured boundary of the no-sysroot model in
# tools/audit/targets.py: clang's freestanding <stdint.h> defines them as the LEAST types and no
# hosted libc does - MSVC's int_fast16_t is `int`, glibc's is `long`, so a record holding one is
# 8 B on Windows and 16 B on Linux while the gate's three legs all see 8. Enumeration is the
# correct tool for a closed family the measurement cannot reach.
ABI_TOKENS = re.compile(r'\b(long|char|wchar_t|char8_t|char16_t|char32_t'
                        r'|size_t|ptrdiff_t|intptr_t|uintptr_t|max_align_t'
                        r'|u?int_fast(?:8|16|32|64)_t|u?int_least(?:8|16|32|64)_t|wint_t)\b')
# Compile-time wall clock. __FILE__/__LINE__ are deliberately NOT here: they are deterministic
# given the tree, TL_CHECK expands them into every sim TU, and they never feed sim state.
BUILD_CLOCK = re.compile(r'\b(__DATE__|__TIME__|__TIMESTAMP__)\b')
# Atomic/interlocked builtins called DIRECTLY in a det TU (W1 jobs review). The sanctioned wrap
# is foundation/atomic.h, which #errors under TL_SIM_TU - but the #error only guards the header:
# a det TU that spells `__atomic_fetch_add(&x, 1, 5)` itself includes nothing, emits NO symbol on
# x86-64 (a 32-bit RMW inlines to `lock xadd`), and the `__aarch64_*` outline-atomic tripwire in
# allow.txt exists only on the Pi leg, where the symbol audit does not run. An atomic in det code
# is worker-observable ordering (docs/DETERMINISM.md section 2 rule 5), so the TOKEN is banned -
# clang's `__atomic_*`/`__c11_atomic_*`, GCC-legacy `__sync_*`, and MSVC's `_Interlocked*`.
ATOMIC_BUILTIN = re.compile(r'\b(__atomic_[a-z_]+|__c11_atomic_[a-z_]+|__sync_[a-z_]+'
                            r'|_Interlocked[A-Za-z0-9_]*)\b')
# A wide or unicode literal carries the target's wchar_t/char16_t width with no `wchar_t` token in
# sight: sizeof(L"x") is 4 on windows-msvc and 8 on linux/aarch64. Text and record layouts are
# identical, so only a token ban sees it (docs/CPP-SUBSET.md §5).
WIDE_LITERAL = re.compile(r'(?<![A-Za-z0-9_])(L|u8|u|U)["\']')
# A hex or octal escape >= 0x80 is a non-ASCII byte the source-character scan cannot see:
# `"\xE9"[0]` sign-extends where `char` is signed and does not where it is unsigned.
HIGH_ESCAPE = re.compile(r'\\x[89a-fA-F][0-9a-fA-F]|\\[2-3][0-7][0-7]')
# `const char*` / `const char[]` for message literals stays legal: TL_FATAL takes one.
CHAR_LITERAL_USE = re.compile(r'\b(?:const\s+char|char\s+const)\s*(\*|\[|\(\s*&)')

# Layout and target-selection hazards that are not types. Each was measured to differ between
# windows-msvc and linux/aarch64 by the third W0 review, with no UB involved:
#   bit-fields          `struct { u8 a:4; u16 c:8; }` is 4 B on windows-msvc, 2 B on linux/arm
#   platform macros     a sim TU that branches on _WIN32 or __aarch64__ is two different programs
#   custom sections     section(".x") hides a mutable global from the .data/.bss gate
#   unfixed enum base   the underlying type is the compiler's choice; hashed state cannot have one
# A bit-field in this subset is `<fixed-width type> [name] :`. Anchoring on the TYPE is what
# keeps `return x < 0 ? -1 : 1;` from reading as one (it did) and catches the four spellings
# the width-anchored version missed (`: W;`, `: sizeof(u8)*8;`, `: 0x4;`, `: 4 = 0;`).
BITFIELD = re.compile(r'\b(?:u8|u16|u32|u64|i8|i16|i32|i64|bool)\s*(?:[A-Za-z_]\w*)?\s*:(?!:)')
PLATFORM_MACRO = re.compile(
    r'\b(_WIN32|_WIN64|_MSC_VER|_M_X64|_M_ARM64|__aarch64__|__x86_64__|__i386__|__arm__'
    r'|__linux__|__APPLE__|__unix__|__ARM_ARCH|__SIZEOF_LONG__|__CHAR_BIT__'
    r'|__LP64__|__LLP64__|_LP64|__CHAR_UNSIGNED__|__SIZEOF_WCHAR_T__|__GNUC__)\b')
# __has_include of a platform header answers differently in the real build (true on Windows) and
# under the cross-target gate's freestanding model (uniformly false) - a divergence that gate is
# blind to by construction. The include allowlist makes it useless in a sim TU anyway.
HAS_INCLUDE = re.compile(r'__has_include\b')
# Checked on the strings-blanked text, so the section NAME is already gone - match the
# attribute itself, not its argument.
CUSTOM_SECTION = re.compile(r'section\s*\(|__declspec\s*\(\s*allocate')
UNFIXED_ENUM = re.compile(r'\benum\s+(?:class\s+|struct\s+)?[A-Za-z_]\w*\s*\{')

GREP_BANS = (
    (re.compile(r'\bstd::'), "std:: in src/ (docs/CPP-SUBSET.md §1)"),
    (re.compile(r'\b(asm|__asm|__asm__)\b'), "inline asm (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'\b__?rdtsc\b'), "rdtsc (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'__builtin_ia32_'), "ISA intrinsic builtin (docs/CPP-SUBSET.md §4)"),
)
# docs/MEMORY.md §1.5/§8.6: mem_pool is the ONE general allocator in the binary and it exists for
# VENDOR heaps. Engine and sim code never call it - the doc has promised a CI grep for this since
# rev 1 and TODO.md carried "not built yet" until the first caller (the Luau VM pool) arrived.
# A phantom gate is worth nothing (LESSONS.md), so it lands with its first user.
# The three allocation verbs only: pool_init/pool_reset are lifecycle (app/ wiring builds the
# pools) and pool_stats is the profiler's read, all legitimate above this line.
POOL_VERBS = re.compile(r'\bpool_(?:alloc|realloc|free)\b')
# mem_pool.h's own declarations are named here for the same reason the doc names them: the header
# IS the API, and TODO.md's entry for this gate asks for exactly this exemption.
POOL_ALLOWED = ("src/foundation/mem_pool.cpp", "src/foundation/mem_pool.h", "src/vendor_glue/")

TLS_SPELLINGS = re.compile(r'\bthread_local\b|\b__thread\b|__declspec\s*\(\s*thread\s*\)')

CONTAINER_OPEN = re.compile(r'^\s*(?:template\s*<.*>\s*)?'
                            r'(?:namespace\b|struct\b|class\b|union\b|enum\b|extern\s*"C")')
TEMPLATE_HEAD = re.compile(r'^\s*template\s*<')
NOT_A_DECL = re.compile(
    r'^\s*(#|//|/\*|\*|\}|\{|\)|template\s*<.*>\s*$|namespace\b|struct\b|class\b|enum\b|union\b'
    r'|using\b|typedef\b|static_assert\b|extern\s*"C"|public:|private:|protected:|return\b'
    r'|if\b|for\b|while\b|switch\b|do\b|else\b)')
# `operator+(`, `operator==(`, `operator bool(` and friends are functions too - fx.h is mostly
# operators, and an identifier-only match made the whole set invisible to the contract rule.
IDENT_CALL = re.compile(r'(operator\s*(?:[A-Za-z_]\w*|[^\w\s(]+)|[A-Za-z_]\w*)\s*\(')
ATTRIBUTES = re.compile(r'__attribute__\s*\(\(.*?\)\)|__declspec\s*\(.*?\)|\[\[.*?\]\]')
# A line that is only an attribute or a preprocessor directive sits between a contract comment
# and the signature it documents; skip it when scanning back.
SKIPPABLE_ABOVE = re.compile(r'^\s*(\[\[.*\]\]\s*$|__attribute__|__declspec|#)')
ALNUM = re.compile(r'[A-Za-z0-9]')
LITERAL = re.compile(r'"[^"]*"' + r"|'[^']*'")


def nondet_stems(root):
    """The one home for the foundation det/non-det split is src/foundation/CMakeLists.txt
    (docs/BUILD.md §10.2 describes it; this parses it, so the list has a single home)."""
    path = os.path.join(root, "src", "foundation", "CMakeLists.txt")
    text = open(path, encoding="utf-8").read()
    m = re.search(r"set\(TL_FOUNDATION_NONDET([^)]*)\)", text)
    if not m:
        sys.exit("includes: src/foundation/CMakeLists.txt has no set(TL_FOUNDATION_NONDET ...)")
    return set(m.group(1).split())


def tooling_stems(root):
    """RR-7 (docs/CPP-SUBSET.md §1): the ONE non-det stem set allowed real io and namespace-scope
    mutable state. A strict subset of nondet_stems() (CMake enforces this at configure time); parsed
    from the same file's TL_FOUNDATION_TOOLING line so this list and symbols.py's copy cannot drift
    from the one that actually ships."""
    path = os.path.join(root, "src", "foundation", "CMakeLists.txt")
    text = open(path, encoding="utf-8").read()
    m = re.search(r"set\(TL_FOUNDATION_TOOLING([^)]*)\)", text)
    if not m:
        sys.exit("includes: src/foundation/CMakeLists.txt has no set(TL_FOUNDATION_TOOLING ...)")
    return set(m.group(1).split())


def strip_comments(text, blank_strings=True):
    """Blank comments - and, when asked, string/char literals - preserving line structure.

    Two variants are needed: include paths live inside string literals, so the include and DAG
    checks read the strings-kept version; the token bans read the strings-blanked one, so an
    error message containing the word "float" does not fail a sim TU.

    A `'` that follows an alphanumeric is a C++14 digit separator (1'000u), NOT the start of a
    character literal. Treating it as one used to swallow the rest of the file - every token ban
    downstream of the first separator went blind."""
    out = []
    i, n = 0, len(text)
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == "'" and i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"):
                out.append(c)                    # digit separator, not a literal
                i += 1
                continue
            if c in ('"', "'"):
                state = c
                out.append(" " if blank_strings else c)
                i += 1
                continue
            out.append(c)
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
                i += 1
                continue
            out.append(" ")
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append(c if c == "\n" else " ")
        else:                                   # inside a string or char literal
            if c == "\\":
                out.append(text[i:i + 2] if not blank_strings else "  ")
                i += 2
                continue
            if c == state:
                state = "code"
            out.append(c if (c == "\n" or not blank_strings) else " ")
        i += 1
    return "".join(out)


def stem_matches(stem, stem_set):
    """A file's stem is in `stem_set` (nondet or tooling), or is that stem's header sibling:
    TOOLING.md §9.1's file table keeps the `tl_` prefix on the HEADER (`tl_log.h`) but drops it
    on the implementation (`log.cpp`) - `tl_assert` is the one stem that keeps its prefix on both,
    which is why the exact stem is tried first."""
    return stem in stem_set or (stem.startswith("tl_") and stem[3:] in stem_set)


def entropy_carriers(root):
    """Every header under src/ that makes EntropyApi's verb visible - entropy.h and, transitively,
    anything that includes it.

    Gating the literal string "platform/entropy.h" is not the restriction docs/PLATFORM.md §5
    states. §5's claim is about the VERB, and two headers in this tree hand it over without ever
    naming entropy.h at the call site: platform/os_entropy.h (which declares
    os_entropy_fill_table, i.e. a way to MINT the table) and
    platform/impl_headless/headless_state.h (which holds a complete EntropyApi by value). Both
    were reachable from core/render/editor/script with a clean audit - measured, by planting each
    in src/core/. Half a gate is still a phantom gate (LESSONS.md), so the rule is the closure,
    computed from the tree instead of a hand-kept list that the next os_*/impl_* header silently
    falls out of.

    Cached per root: this is a one-shot CLI, and selftest.py runs each fixture in its own
    process."""
    hit = _ENTROPY_CARRIERS.get(root)
    if hit is not None:
        return hit
    src = os.path.join(root, "src")
    includers = {}                          # header rel-path -> set of local includes it names
    for dirpath, _dirs, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith((".h", ".hpp", ".inc")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace("\\", "/")
            key = rel[len("src/"):] if rel.startswith("src/") else rel
            try:
                text = open(path, encoding="utf-8").read()
            except (OSError, UnicodeDecodeError):
                continue
            names = set()
            for line in strip_comments(text, blank_strings=False).splitlines():
                m = INC_LOCAL.match(line)
                if m:
                    names.add(m.group(1))
            includers[key] = names
    carriers = {ENTROPY_HEADER}
    changed = True
    while changed:                          # transitive closure; the graph is tiny and acyclic
        changed = False
        for key, names in includers.items():
            if key not in carriers and (names & carriers):
                carriers.add(key)
                changed = True
    out = frozenset(carriers)
    _ENTROPY_CARRIERS[root] = out
    return out


def module_of(rel):
    parts = rel.split("/")
    return parts[1] if len(parts) > 2 and parts[0] == "src" else None


def is_mutable_static(line):
    """True for `static <type> name ...` with no const-ness and no parameter list."""
    m = STATIC_DECL.match(line)
    if not m:
        return False
    rest = m.group(1)
    head = re.split(r'[=;]', rest, maxsplit=1)[0]
    if "(" in head:                              # a function declaration or definition
        return False
    return not CONST_QUAL.search(rest.split("=", 1)[0])


def declaration_sites(code_lines, token_lines):
    """Yield (line index, joined text) for every declaration at namespace or struct scope.

    Function bodies are skipped by tracking what opened each brace: a `struct`/`namespace` brace
    keeps us in a scope where declarations live, any other brace is a body. Without this, every
    member function of fx<> would be invisible to the contract-comment gate.

    Brackets are counted on the STRINGS-BLANKED text and joined from the strings-kept text: a `(`
    inside a literal - TL_CHECK(x, "expected (") - left the paren depth permanently unbalanced and
    swallowed every later declaration in the header into one unit.
    """
    scopes = []                                  # True = container scope, False = body
    i = 0
    while i < len(code_lines):
        line = code_lines[i]
        counted = token_lines[i] if i < len(token_lines) else line
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue
        in_container = all(scopes)
        start = i
        text, count_text = line, counted
        # A template head can wrap: `template <class A,` / `class B>`. Join until the angle
        # brackets balance, then fall through to the parameter-list join below.
        if TEMPLATE_HEAD.match(count_text):
            while count_text.count("<") > count_text.count(">") and i + 1 < len(code_lines):
                i += 1
                text += " " + code_lines[i].strip()
                count_text += " " + (token_lines[i] if i < len(token_lines) else "").strip()
            if "(" not in count_text and i + 1 < len(code_lines):
                i += 1                            # the head alone: pull in what it introduces
                text += " " + code_lines[i].strip()
                count_text += " " + (token_lines[i] if i < len(token_lines) else "").strip()
        depth = count_text.count("(") - count_text.count(")")
        while depth > 0 and i + 1 < len(code_lines):
            i += 1
            text += " " + code_lines[i].strip()
            count_text += " " + (token_lines[i] if i < len(token_lines) else "").strip()
            depth = count_text.count("(") - count_text.count(")")
        if in_container:
            yield start, text.strip()
        opened = CONTAINER_OPEN.match(text) is not None
        for ch in count_text:
            if ch == "{":
                scopes.append(opened)
            elif ch == "}" and scopes:
                scopes.pop()
        i += 1


def looks_like_function(unit):
    """A declaration or definition of a function, as opposed to a variable initialised by a call.

    `constexpr u32 C = c_lit(1);` is a variable; `u32 f(u32 a);`, `inline u32 f(u32 a) { ... }`,
    `void f() = delete;` and `auto f(int) -> u32;` are functions."""
    unit = ATTRIBUTES.sub(" ", unit).strip()
    if not unit or NOT_A_DECL.match(unit):
        return False
    m = IDENT_CALL.search(unit)
    if not m:
        return False
    if "=" in unit[:m.start(1)]:                 # an initialiser, not a parameter list
        return False
    tail = unit[m.end():]
    depth = 1
    for j, ch in enumerate(tail):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                after = tail[j + 1:].strip()
                return after.startswith(("{", ";", "const", "noexcept", "->", "=", ":"))
    return False


def is_template_head(line):
    """A `template <...>` line that introduces the declaration below it. A one-line template
    *definition* is not a head - skipping it let the next function inherit its contract comment."""
    if not TEMPLATE_HEAD.match(line):
        return False
    return "{" not in line and not line.rstrip().endswith(";")


def comment_block_at(raw_lines, j):
    """(text, first line index) of the comment ending at line j, joined across a block comment.
    A `/* ... */` whose last line is a lone `*/` used to read as a two-character comment."""
    line = raw_lines[j].strip()
    if not line.startswith(("//", "*", "/*")):
        return line, j
    start = j
    while start > 0 and raw_lines[start - 1].strip().startswith(("//", "*", "/*")):
        start -= 1
    return " ".join(raw_lines[k].strip() for k in range(start, j + 1)), start


def is_contract_comment(text):
    """A divider (`// ---------`) is not a contract."""
    return text.startswith(("//", "*", "/*")) and len(ALNUM.findall(text)) >= 12


def contract_block_end(raw_lines):
    """Index one past the file's leading comment block: the run of #pragma/comment lines at the
    top, ended by the first blank line or the first line that is neither. A per-function contract
    comment must live after it, or the first declaration in every header would inherit the file's
    contract block and never need one of its own."""
    seen_comment = False
    for j, line in enumerate(raw_lines):
        t = line.strip()
        if not t:
            if seen_comment:
                return j
            continue
        if t.startswith(("//", "/*", "*", "#pragma")):
            seen_comment = True
            continue
        return j
    return len(raw_lines)


def check_file(root, path, nondet, tooling, static_allow, errors):
    rel = os.path.relpath(path, root).replace("\\", "/")
    try:
        raw = open(path, encoding="utf-8").read()
    except UnicodeDecodeError:
        errors.append("%s:1: not valid UTF-8 - sources are UTF-8 (.editorconfig)" % rel)
        return
    raw_lines = raw.splitlines()
    code_lines = strip_comments(raw, blank_strings=False).splitlines()   # includes keep their paths
    token_lines = strip_comments(raw, blank_strings=True).splitlines()   # bans ignore literals
    module = module_of(rel)
    in_backend_free = is_backend_free(rel)
    stem = os.path.splitext(os.path.basename(rel))[0]

    is_det_tu = rel.startswith("src/sim/") or (
        rel.startswith("src/foundation/") and not stem_matches(stem, nondet))
    # RR-7: the tooling plane is the one non-det stem set exempted from the io and .data/.bss
    # bans - never the directory, always the named stem, so a sibling non-det stem (jobs,
    # mem_pool, ...) that is not on the list inherits nothing just by living next to one that is.
    # ...and never the panic-ABI HEADER, even though its stem is on the list. tl_assert.h is the
    # one tooling header a sim TU may include (PANIC_ABI_HEADER above), so granting it io and
    # mutable state would push both through R-3's hole into every det TU in the tree. Measured
    # before this line existed: `#include <stdio.h>` + `static int g_ta = 0;` appended to
    # tl_assert.h produced 0 violations. Only tl_assert.cpp is the tooling plane.
    is_tooling_tu = (rel.startswith("src/foundation/") and stem_matches(stem, tooling)
                     and rel != "src/" + PANIC_ABI_HEADER)
    # tl_types.h declares f32/f64 and StrView's `const char*`, and fx_float.h is the bridge, so
    # both are exempt from the TOKEN bans - but not from the layout and target-selection rules,
    # which apply to every sim TU including the leaf. Exempting them wholesale (as the first
    # version did) left the one header every sim TU includes free to carry a bit-field.
    tokens_exempt = rel in TYPE_EXEMPT_PATHS

    # `const char*` stays legal for message literals, which re-opens the char hole one level
    # down: `h ^= (u64)s[i]` over a literal byte >= 0x80 sign-extends where `char` is signed and
    # not where it is unsigned, so a NameHash over a non-ASCII literal differs per target with no
    # `char` token in sight. Only LITERALS matter - a comment never becomes char data, and the
    # doc-citation style is full of section signs (docs/CPP-SUBSET.md 5).
    if is_det_tu and not tokens_exempt:
        for i, line in enumerate(code_lines, 1):
            for lit in LITERAL.findall(line):
                if any(ord(ch) > 127 for ch in lit) or HIGH_ESCAPE.search(lit):
                    errors.append("%s:%d: non-ASCII byte in a sim-TU literal (directly or via a "
                                  "\\x/\\nnn escape) - it hashes differently where `char` is "
                                  "signed (docs/CPP-SUBSET.md §5)" % (rel, i))
                    break

    allow = set(SYS_ALLOW)
    for prefix, extra in SYS_ALLOW_DIRS.items():
        if rel.startswith(prefix):
            allow |= extra
    if is_tooling_tu:
        allow |= TOOLING_SYS_ALLOW

    for i, line in enumerate(code_lines, 1):
        m = INC_SYS.match(line)
        if m and m.group(1) not in allow and not in_backend_free:
            errors.append("%s:%d: system include <%s> is not on the allowlist "
                          "(docs/CPP-SUBSET.md §1)" % (rel, i, m.group(1)))
        m2 = INC_LOCAL.match(line)
        if m2:
            inc = m2.group(1)
            inc_module = inc.split("/")[0]
            allowed = MODULE_DAG.get(module, ())
            if inc_module not in allowed:
                if not (module == "render" and inc == RENDER_SIM_HEADER):
                    errors.append('%s:%d: include "%s" violates the module DAG - %s may include '
                                  "%s (docs/ARCHITECTURE.md §1)"
                                  % (rel, i, inc, module, ", ".join(allowed)))
            elif is_det_tu and inc_module == "foundation":
                inc_stem = os.path.splitext(os.path.basename(inc))[0]
                if inc_stem == "fx_float":
                    # The bridge is exempt from the float ban because it IS the bridge; that made
                    # it a legal include for a sim TU, i.e. a float path into sim code with no
                    # _fltused tripwire on ELF (docs/FX-PALETTE.md 6).
                    errors.append('%s:%d: a sim TU includes the float bridge "%s" - it is '
                                  "render/editor/tools only (docs/FX-PALETTE.md §6)" % (rel, i, inc))
                elif stem_matches(inc_stem, nondet) and inc != PANIC_ABI_HEADER:
                    errors.append('%s:%d: a sim TU includes the non-det foundation header "%s" '
                                  "(docs/BUILD.md §10.2)" % (rel, i, inc))
            if inc in entropy_carriers(root) and module not in ENTROPY_ALLOWED_MODULES:
                via = "" if inc == ENTROPY_HEADER else ' (it includes "%s")' % ENTROPY_HEADER
                errors.append('%s:%d: include "%s"%s is restricted to platform/net/app '
                              "(docs/PLATFORM.md §5, docs/DETERMINISM.md §2)" % (rel, i, inc, via))
        if m or m2:
            for token, prefixes in BACKEND_HEADERS.items():
                if token in line and not any(rel.startswith(p) for p in prefixes):
                    errors.append("%s:%d: backend header %s outside its wrap module %s "
                                  "(docs/BUILD.md §4)" % (rel, i, token, prefixes))

        tline = token_lines[i - 1] if i - 1 < len(token_lines) else ""
        # docs/PLATFORM.md §9.5: vendor_glue is "the one folder allowed writable static state" -
        # a whole-DIRECTORY exemption, not stem-keyed, because every TU in it is a per-lib
        # mem_pool hookup and legitimately owns one (symbols.py's --vendor-glue-lib is the
        # matching link-level exemption for the same ruling). Everywhere else the exemption is
        # (directory, stem) via tools/audit/static_allow.txt - never the stem alone and never
        # the directory alone: a same-named file in another directory, or another file in the
        # same directory, is an ordinary violation.
        static_exempt = ((os.path.dirname(rel), stem) in static_allow
                         or rel.startswith("src/vendor_glue/"))
        if is_mutable_static(tline) and not is_tooling_tu and not static_exempt:
            errors.append("%s:%d: static mutable state (docs/CPP-SUBSET.md §1): %s"
                          % (rel, i, raw_lines[i - 1].strip()[:70]))
        if TLS_SPELLINGS.search(tline):
            errors.append("%s:%d: thread-local storage outside the job system "
                          "(docs/CPP-SUBSET.md §1)" % (rel, i))
        for rx, why in GREP_BANS:
            if rx.search(tline):
                errors.append("%s:%d: %s" % (rel, i, why))
        if POOL_VERBS.search(tline) and not rel.startswith(POOL_ALLOWED):
            errors.append("%s:%d: mem_pool's allocation API outside %s - it is the vendor heap, "
                          "not an engine allocator (docs/MEMORY.md §1.5, §8.6): %s"
                          % (rel, i, ", ".join(POOL_ALLOWED), raw_lines[i - 1].strip()[:60]))
        if BUILD_CLOCK.search(tline):
            errors.append("%s:%d: compile-time wall clock in src/ - two peers building the same "
                          "tree get different bytes (docs/CPP-SUBSET.md §5)" % (rel, i))
        if is_det_tu and not tokens_exempt:
            m3 = FLOAT_TOKENS.search(tline)
            if m3:
                errors.append("%s:%d: '%s' in a sim TU (docs/CANON.md; f32/f64 are the same ban): %s"
                              % (rel, i, m3.group(1), raw_lines[i - 1].strip()[:60]))
            m3b = FLOAT_LITERAL.search(tline)
            if m3b:
                errors.append("%s:%d: floating literal '%s' in a sim TU - a float with no type token "
                              "(decltype/auto/promotion); spell the value as a rational through "
                              "fx_lit (docs/CANON.md): %s"
                              % (rel, i, m3b.group(0), raw_lines[i - 1].strip()[:60]))
            m3c = LONG_SUFFIX.search(tline)
            if m3c:
                errors.append("%s:%d: integer literal '%s' is a `long` with no `long` token - 32-bit "
                              "on Windows, 64-bit on Linux; use LL/ULL or a fixed-width cast "
                              "(docs/CPP-SUBSET.md §5): %s"
                              % (rel, i, m3c.group(0), raw_lines[i - 1].strip()[:60]))
            m4 = ABI_TOKENS.search(CHAR_LITERAL_USE.sub("", tline))
            if m4:
                errors.append("%s:%d: '%s' in a sim TU - its width or signedness differs between "
                              "x86-64 and aarch64; use the fixed-width types of docs/CANON.md: %s"
                              % (rel, i, m4.group(1), raw_lines[i - 1].strip()[:60]))
        if is_det_tu:
            if WIDE_LITERAL.search(code_lines[i - 1] if i - 1 < len(code_lines) else ""):
                errors.append("%s:%d: wide or unicode literal in a sim TU - it carries the "
                              "target's wchar_t/char16_t width (docs/CPP-SUBSET.md §5): %s"
                              % (rel, i, raw_lines[i - 1].strip()[:60]))
            if BITFIELD.search(tline):
                errors.append("%s:%d: bit-field in a sim TU - the layout differs between "
                              "windows-msvc and linux/aarch64, so the arena bytes differ "
                              "(docs/CPP-SUBSET.md §5): %s" % (rel, i, raw_lines[i - 1].strip()[:60]))
            if HAS_INCLUDE.search(tline):
                errors.append("%s:%d: __has_include in a sim TU - it answers differently in the "
                              "real build and under the cross-target gate's freestanding model "
                              "(docs/CPP-SUBSET.md §5)" % (rel, i))
            m5 = PLATFORM_MACRO.search(tline)
            if m5:
                errors.append("%s:%d: '%s' in a sim TU - a per-target branch makes two different "
                              "programs (docs/CPP-SUBSET.md §5)" % (rel, i, m5.group(1)))
            if CUSTOM_SECTION.search(tline):
                errors.append("%s:%d: a custom section attribute hides storage from the .data/.bss "
                              "gate (docs/CPP-SUBSET.md §1)" % (rel, i))
            if UNFIXED_ENUM.search(tline):
                errors.append("%s:%d: enum without a fixed underlying type in a sim TU - the width "
                              "is the compiler's choice (docs/CANON.md): %s"
                              % (rel, i, raw_lines[i - 1].strip()[:60]))
            m6 = ATOMIC_BUILTIN.search(tline)
            if m6:
                errors.append("%s:%d: atomic builtin '%s' called directly in a sim TU - an atomic "
                              "in det code is worker-observable ordering, and on x86-64 it emits "
                              "no symbol for the audit to catch; the wrap is foundation/atomic.h, "
                              "which a det TU may not reach either (docs/JOBS.md section 6.1, "
                              "docs/DETERMINISM.md section 2): %s"
                              % (rel, i, m6.group(1), raw_lines[i - 1].strip()[:60]))

    if path.endswith((".h", ".hpp")):
        if not re.search(r'//.*Spec:\s*docs/[A-Z0-9-]+\.md\s*§', "\n".join(raw_lines[:30])):
            errors.append("%s:1: public header has no contract block naming its spec section "
                          "(docs/CPP-SUBSET.md §6)" % rel)
        body_start = contract_block_end(raw_lines)
        for idx, unit in declaration_sites(code_lines, token_lines):
            if idx < body_start or not looks_like_function(unit):
                continue
            # A template head belongs to the function below it, so the contract comment sits above
            # it, not between it and the signature.
            prev, prev_idx = "", -1
            for j in range(idx - 1, -1, -1):
                line = raw_lines[j]
                if not line.strip() or is_template_head(line) or SKIPPABLE_ABOVE.match(line):
                    continue
                prev, prev_idx = comment_block_at(raw_lines, j)
                break
            if not (is_contract_comment(prev) and prev_idx >= body_start):
                errors.append("%s:%d: public function has no contract comment "
                              "(docs/CPP-SUBSET.md §6): %s" % (rel, idx + 1, unit[:60]))


# --- gate 7: NOMINMAX before every <windows.h> ---------------------------------------------
# windows.h's raw min/max macros mangle any same-named declaration that follows them, and fx.h
# declares free functions `min`/`max` (docs/FX-PALETTE.md). A TU that reaches both breaks with
# "too many arguments to function-like macro invocation" pointing at an fx template - a message
# that names neither windows.h nor min/max, which is why this cost a build cycle to find
# (LESSONS.md). The root-cause fix would be renaming fx's min/max, and it was REJECTED (ruled
# 2026-08-24, TODO.md R6): those spellings are pinned across FX-PALETTE.md and ALLOY.md, and
# churning a doc-visible vocabulary to dodge a Windows macro is the wrong trade. So the rule is
# preprocessor-side and absolute - every <windows.h> in the tree is preceded by #define NOMINMAX
# in the same file - and this is the gate that keeps it true for files nobody has paired with an
# fx header YET. Per-file by design: it does not chase transitive includes, and it does not need
# to, because gate 1 plus TESTING.md §8 R-2 mean windows.h is spelled in a handful of known TUs.
NOMINMAX_ROOTS = ("src", "tests")
# Case-insensitive: the Windows filesystem is, so <Windows.h> resolves to the same header and
# carries the same macros.
WINDOWS_H_INCLUDE = re.compile(r'^\s*#\s*include\s*<\s*[Ww][Ii][Nn][Dd][Oo][Ww][Ss]\.[Hh]\s*>')
DEFINE_NOMINMAX = re.compile(r'^\s*#\s*define\s+NOMINMAX\b')


def check_nominmax(root, path, errors):
    """Fails if the file includes <windows.h> with no `#define NOMINMAX` on an earlier line.

    Reads the comments-and-strings-blanked text, like every other token check (the module rule at
    the top of this file): a block-commented `#include <windows.h>` at column 0 must not trigger,
    and a `#define NOMINMAX` inside a comment must not satisfy the rule - the 2026-08-25 review
    sweep found gate 7 alone reading raw lines. strip_comments preserves line structure, so the
    reported line numbers still point at the real file."""
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    try:
        lines = strip_comments(open(path, encoding="utf-8", errors="replace").read()).splitlines()
    except OSError:
        return
    defined_at = -1
    for i, line in enumerate(lines, 1):
        if defined_at < 0 and DEFINE_NOMINMAX.match(line):
            defined_at = i
        if WINDOWS_H_INCLUDE.match(line) and defined_at < 0:
            errors.append("%s:%d: <windows.h> with no `#define NOMINMAX` above it in this file - "
                          "its raw min/max macros mangle fx.h's free functions of the same name in "
                          "any TU that reaches both, and the error points at fx, not at this line "
                          "(docs/LESSONS.md; ruled 2026-08-24, TODO.md R6)" % (rel, i))
            return   # one report per file: the rest of the file's includes are the same defect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    a = ap.parse_args()
    src = os.path.join(a.root, "src")
    nondet = nondet_stems(a.root)
    tooling = tooling_stems(a.root)
    static_allow = static_allow_dirs(a.root)
    errors = []
    scanned = 0
    for dirpath, _dirs, files in os.walk(src):
        for name in sorted(files):
            if name.endswith(SCAN_EXT):
                scanned += 1
                check_file(a.root, os.path.join(dirpath, name), nondet, tooling, static_allow, errors)
    # Gate 7 alone also walks tests/ - windows.h is legal there (docs/TESTING.md §8 R-2) and the
    # break it causes is a test-tree break as often as a src/ one. Counted separately so `scanned`
    # keeps meaning "files the src/ gates saw".
    nominmax_scanned = 0
    for sub in NOMINMAX_ROOTS:
        for dirpath, _dirs, files in os.walk(os.path.join(a.root, sub)):
            for name in sorted(files):
                if name.endswith(SCAN_EXT):
                    nominmax_scanned += 1
                    check_nominmax(a.root, os.path.join(dirpath, name), errors)
    if nominmax_scanned == 0:
        errors.append("<tree>:0: the NOMINMAX gate scanned no files at all - a filter that selects "
                      "nothing must be an error, not a clean run (docs/LESSONS.md)")
    for e in errors:
        print("ERROR " + e)
    print("includes: %d files checked, %d violations" % (scanned, len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
