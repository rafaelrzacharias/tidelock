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
}
BACKEND_FREE = ("src/platform/impl_sdl3", "src/platform/impl_headless")   # OS headers live here

BACKEND_HEADERS = {                       # token in the include path -> allowed path prefixes
    "SDL3": ("src/platform/impl_sdl3",),
    "SDL_ttf": ("src/platform/impl_sdl3",),
    "imgui": ("src/editor",),
    "enet": ("src/net",),
    "luau": ("src/script",),
    "lua.h": ("src/script",),
    "monocypher": ("src/net",),
    "stb_": ("src/platform/impl_sdl3", "src/core"),
}

# The include DAG of docs/ARCHITECTURE.md §1: module -> the modules it may include (itself
# included). Dependencies point down only; render is the one module allowed a sim header, and
# only sim/views.h.
MODULE_DAG = {
    "foundation": ("foundation",),
    "platform": ("platform", "foundation"),
    "sim": ("sim", "foundation"),
    "core": ("core", "foundation", "platform"),
    "render": ("render", "core", "foundation", "platform"),
    "net": ("net", "core", "foundation", "platform"),
    "editor": ("editor", "core", "render", "foundation", "platform"),
    "script": ("script", "core", "foundation", "platform"),
    "app": ("app", "editor", "net", "render", "script", "sim", "core", "platform", "foundation"),
}
RENDER_SIM_HEADER = "sim/views.h"

# Named exceptions to the sim-TU type bans, both from the docs: tl_types.h must declare f32/f64
# (docs/CPP-SUBSET.md §1) and fx_float.h is the render/editor/tools bridge (docs/FX-PALETTE.md §6).
TYPE_EXEMPT_PATHS = {"src/foundation/tl_types.h", "src/foundation/fx_float.h"}
THREAD_LOCAL_EXEMPT = {"src/foundation/jobs.cpp", "src/foundation/jobs.h"}   # the worker slot

INC_SYS = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
INC_LOCAL = re.compile(r'^\s*#\s*include\s*"([^"]+)"')

# `static`/namespace-scope MUTABLE state. A static *function* has internal linkage and no state;
# `static u32 const K` is a constant. Only a variable with no const/constexpr qualifier counts.
# (docs/CPP-SUBSET.md §1's own `^static [^c]` grep has this same defect and is corrected there.)
STATIC_DECL = re.compile(r'^\s*static\s+(?!_assert\b)(.*)$')
CONST_QUAL = re.compile(r'\b(const|constexpr|consteval|constinit)\b')

FLOAT_TOKENS = re.compile(r'\b(float|double|f32|f64)\b')
# Target-variable integer spellings. `int`/`short` are 32/16-bit on all three targets and are not
# banned here; `long`, `char` and `wchar_t` are not.
ABI_TOKENS = re.compile(r'\b(long|char|wchar_t|char8_t|char16_t|char32_t)\b')
# `const char*` / `const char[]` for message literals stays legal: TL_FATAL takes one.
CHAR_LITERAL_USE = re.compile(r'\b(?:const\s+char|char\s+const)\s*(\*|\[|\(\s*&)')

GREP_BANS = (
    (re.compile(r'\bstd::'), "std:: in src/ (docs/CPP-SUBSET.md §1)"),
    (re.compile(r'\b(asm|__asm|__asm__)\b'), "inline asm (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'\b__?rdtsc\b'), "rdtsc (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'__builtin_ia32_'), "ISA intrinsic builtin (docs/CPP-SUBSET.md §4)"),
)
TLS_SPELLINGS = re.compile(r'\bthread_local\b|\b__thread\b|__declspec\s*\(\s*thread\s*\)')

CONTAINER_OPEN = re.compile(r'^\s*(?:template\s*<.*>\s*)?'
                            r'(?:namespace\b|struct\b|class\b|union\b|enum\b|extern\s*"C")')
TEMPLATE_HEAD = re.compile(r'^\s*template\s*<')
NOT_A_DECL = re.compile(
    r'^\s*(#|//|/\*|\*|\}|\{|\)|template\s*<.*>\s*$|namespace\b|struct\b|class\b|enum\b|union\b'
    r'|using\b|typedef\b|static_assert\b|extern\s*"C"|public:|private:|protected:|return\b'
    r'|if\b|for\b|while\b|switch\b|do\b|else\b)')
IDENT_CALL = re.compile(r'([A-Za-z_]\w*)\s*\(')
ALNUM = re.compile(r'[A-Za-z0-9]')


def nondet_stems(root):
    """The one home for the foundation det/non-det split is src/foundation/CMakeLists.txt
    (docs/BUILD.md §10.2 describes it; this parses it, so the list has a single home)."""
    path = os.path.join(root, "src", "foundation", "CMakeLists.txt")
    text = open(path, encoding="utf-8").read()
    m = re.search(r"set\(TL_FOUNDATION_NONDET([^)]*)\)", text)
    if not m:
        sys.exit("includes: src/foundation/CMakeLists.txt has no set(TL_FOUNDATION_NONDET ...)")
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


def declaration_sites(code_lines):
    """Yield (line index, joined text) for every declaration at namespace or struct scope.

    Function bodies are skipped by tracking what opened each brace: a `struct`/`namespace` brace
    keeps us in a scope where declarations live, any other brace is a body. Without this, every
    member function of fx<> would be invisible to the contract-comment gate."""
    scopes = []                                  # True = container scope, False = body
    i = 0
    while i < len(code_lines):
        line = code_lines[i]
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue
        in_container = all(scopes)
        start = i
        text = line
        depth = text.count("(") - text.count(")")
        while depth > 0 and i + 1 < len(code_lines):
            i += 1
            text += " " + code_lines[i].strip()
            depth = text.count("(") - text.count(")")
        if in_container:
            yield start, text.strip()
        opened = CONTAINER_OPEN.match(text) is not None
        for ch in text:
            if ch == "{":
                scopes.append(opened)
            elif ch == "}" and scopes:
                scopes.pop()
        i += 1


def looks_like_function(unit):
    """A declaration or definition of a function, as opposed to a variable initialised by a call.

    `constexpr u32 C = c_lit(1);` is a variable; `u32 f(u32 a);`, `inline u32 f(u32 a) { ... }`,
    `void f() = delete;` and `auto f(int) -> u32;` are functions."""
    if NOT_A_DECL.match(unit):
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


def check_file(root, path, nondet, errors):
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
    in_backend_free = any(rel.startswith(p) for p in BACKEND_FREE)
    stem = os.path.splitext(os.path.basename(rel))[0]

    is_det_tu = rel.startswith("src/sim/") or (
        rel.startswith("src/foundation/") and stem not in nondet)
    if rel in TYPE_EXEMPT_PATHS:
        is_det_tu = False

    allow = set(SYS_ALLOW)
    for prefix, extra in SYS_ALLOW_DIRS.items():
        if rel.startswith(prefix):
            allow |= extra

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
                if inc_stem in nondet:
                    errors.append('%s:%d: a sim TU includes the non-det foundation header "%s" '
                                  "(docs/BUILD.md §10.2)" % (rel, i, inc))
        if m or m2:
            for token, prefixes in BACKEND_HEADERS.items():
                if token in line and not any(rel.startswith(p) for p in prefixes):
                    errors.append("%s:%d: backend header %s outside its wrap module %s "
                                  "(docs/BUILD.md §4)" % (rel, i, token, prefixes))

        tline = token_lines[i - 1] if i - 1 < len(token_lines) else ""
        if is_mutable_static(tline):
            errors.append("%s:%d: static mutable state (docs/CPP-SUBSET.md §1): %s"
                          % (rel, i, raw_lines[i - 1].strip()[:70]))
        if TLS_SPELLINGS.search(tline) and rel not in THREAD_LOCAL_EXEMPT:
            errors.append("%s:%d: thread-local storage outside the job system "
                          "(docs/CPP-SUBSET.md §1)" % (rel, i))
        for rx, why in GREP_BANS:
            if rx.search(tline):
                errors.append("%s:%d: %s" % (rel, i, why))
        if is_det_tu:
            m3 = FLOAT_TOKENS.search(tline)
            if m3:
                errors.append("%s:%d: '%s' in a sim TU (docs/CANON.md; f32/f64 are the same ban): %s"
                              % (rel, i, m3.group(1), raw_lines[i - 1].strip()[:60]))
            m4 = ABI_TOKENS.search(CHAR_LITERAL_USE.sub("", tline))
            if m4:
                errors.append("%s:%d: '%s' in a sim TU - its width or signedness differs between "
                              "x86-64 and aarch64; use the fixed-width types of docs/CANON.md: %s"
                              % (rel, i, m4.group(1), raw_lines[i - 1].strip()[:60]))

    if path.endswith((".h", ".hpp")):
        if not re.search(r'//.*Spec:\s*docs/[A-Z0-9-]+\.md\s*§', "\n".join(raw_lines[:30])):
            errors.append("%s:1: public header has no contract block naming its spec section "
                          "(docs/CPP-SUBSET.md §6)" % rel)
        body_start = contract_block_end(raw_lines)
        for idx, unit in declaration_sites(code_lines):
            if idx < body_start or not looks_like_function(unit):
                continue
            # A template head belongs to the function below it, so the contract comment sits above
            # it, not between it and the signature.
            prev, prev_idx = "", -1
            for j in range(idx - 1, -1, -1):
                if raw_lines[j].strip() and not TEMPLATE_HEAD.match(raw_lines[j]):
                    prev, prev_idx = raw_lines[j].strip(), j
                    break
            if not (is_contract_comment(prev) and prev_idx >= body_start):
                errors.append("%s:%d: public function has no contract comment "
                              "(docs/CPP-SUBSET.md §6): %s" % (rel, idx + 1, unit[:60]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    a = ap.parse_args()
    src = os.path.join(a.root, "src")
    nondet = nondet_stems(a.root)
    errors = []
    scanned = 0
    for dirpath, _dirs, files in os.walk(src):
        for name in sorted(files):
            if name.endswith(SCAN_EXT):
                scanned += 1
                check_file(a.root, os.path.join(dirpath, name), nondet, errors)
    for e in errors:
        print("ERROR " + e)
    print("includes: %d files checked, %d violations" % (scanned, len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
