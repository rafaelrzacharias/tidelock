#!/usr/bin/env python3
"""Source-discipline gate over src/. Spec: docs/CPP-SUBSET.md §1, §4, §6; docs/ARCHITECTURE.md
§1; docs/TESTING.md §5. vendor/ and tools/ are exempt by design; tests/ has the io exemption of
docs/TESTING.md §8 R-2.

What a *regex* can prove is only half the job - the other half is docs/CPP-SUBSET.md §4's link
gate (tools/audit/symbols.py), which is where mutable globals and stray libm calls are actually
caught. This file covers what never becomes a symbol.

Gates, all blocking:
  1. system includes outside the per-directory allowlist
  2. backend headers outside their wrap module
  3. the module DAG: every quoted include is a module below the includer (docs/ARCHITECTURE.md §1)
  4. sim TUs: no float/double/f32/f64 token, no non-det foundation header
  5. everywhere in src/: no std::, no thread_local, no static/namespace-scope mutable, no asm,
     __rdtsc or __builtin_ia32_* (docs/CPP-SUBSET.md §4's grep line)
  6. header contracts: a public header opens with a contract block naming its spec section, and
     every public function - declaration OR inline definition - carries a contract comment

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
    "app": tuple(("app", "editor", "net", "render", "script", "sim", "core", "platform", "foundation")),
}
RENDER_SIM_HEADER = "sim/views.h"

# Named exceptions to the sim-TU float ban, both from the docs: tl_types.h must declare f32/f64
# (docs/CPP-SUBSET.md §1) and fx_float.h is the render/editor/tools bridge (docs/FX-PALETTE.md §6).
FLOAT_EXEMPT_PATHS = {"src/foundation/tl_types.h", "src/foundation/fx_float.h"}
THREAD_LOCAL_EXEMPT = {"src/foundation/jobs.cpp", "src/foundation/jobs.h"}   # the worker slot

INC_SYS = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
INC_LOCAL = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
STATIC_MUT = re.compile(r'^\s*static\s+(?!(?:const|constexpr|inline\s+const)\b)')
FLOAT_TOKENS = re.compile(r'\b(float|double|f32|f64)\b')
GREP_BANS = (
    (re.compile(r'\bstd::'), "std:: in src/ (docs/CPP-SUBSET.md §1)"),
    (re.compile(r'\b(asm|__asm|__asm__)\b'), "inline asm (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'\b__?rdtsc\b'), "rdtsc (docs/CPP-SUBSET.md §4)"),
    (re.compile(r'__builtin_ia32_'), "ISA intrinsic builtin (docs/CPP-SUBSET.md §4)"),
)

# A "looks like a function" logical line: an identifier followed by a parameter list, ending in
# ';' (declaration) or '{' (inline definition). Deliberately loose - a false positive costs a
# comment, a false negative costs the gate.
NOT_A_FUNCTION = re.compile(
    r'^\s*(#|//|/\*|\*|\}|\{|\)|template\s*<.*>\s*$|namespace\b|struct\b|class\b|enum\b|union\b'
    r'|using\b|typedef\b|static_assert\b|extern\s*"C"|public:|private:|protected:)')
FUNC_LIKE = re.compile(r'[A-Za-z_]\w*\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?[;{]\s*$')


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
    error message containing the word "float" does not fail a sim TU."""
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


def logical_units(lines):
    """Yield (start_index, joined_text) for each column-0 logical line, joining continuations
    until the parentheses balance - so multi-line declarations are seen whole."""
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line or line[0] in " \t\n":
            i += 1
            continue
        start = i
        text = line
        depth = text.count("(") - text.count(")")
        while depth > 0 and i + 1 < len(lines):
            i += 1
            text += " " + lines[i].strip()
            depth = text.count("(") - text.count(")")
        yield start, text.strip()
        i += 1


def check_file(root, path, nondet, errors):
    rel = os.path.relpath(path, root).replace("\\", "/")
    raw = open(path, encoding="utf-8").read()
    raw_lines = raw.splitlines()
    code_lines = strip_comments(raw, blank_strings=False).splitlines()   # includes keep their paths
    token_lines = strip_comments(raw, blank_strings=True).splitlines()   # bans ignore literals
    module = module_of(rel)
    in_backend_free = any(rel.startswith(p) for p in BACKEND_FREE)
    stem = os.path.splitext(os.path.basename(rel))[0]

    is_det_tu = rel.startswith("src/sim/") or (
        rel.startswith("src/foundation/") and stem not in nondet)
    if rel in FLOAT_EXEMPT_PATHS:
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
        if STATIC_MUT.match(line):
            errors.append("%s:%d: static mutable state (docs/CPP-SUBSET.md §1): %s"
                          % (rel, i, raw_lines[i - 1].strip()[:70]))
        if "thread_local" in line and rel not in THREAD_LOCAL_EXEMPT:
            errors.append("%s:%d: thread_local outside the job system (docs/CPP-SUBSET.md §1)" % (rel, i))
        tline = token_lines[i - 1] if i - 1 < len(token_lines) else ""
        for rx, why in GREP_BANS:
            if rx.search(tline):
                errors.append("%s:%d: %s" % (rel, i, why))
        if is_det_tu:
            m3 = FLOAT_TOKENS.search(tline)
            if m3:
                errors.append("%s:%d: '%s' in a sim TU (docs/CANON.md; f32/f64 are the same ban): %s"
                              % (rel, i, m3.group(1), raw_lines[i - 1].strip()[:60]))

    if path.endswith((".h", ".hpp")):
        if not re.search(r'//.*Spec:\s*docs/[A-Z0-9-]+\.md\s*§', "\n".join(raw_lines[:30])):
            errors.append("%s:1: public header has no contract block naming its spec section "
                          "(docs/CPP-SUBSET.md §6)" % rel)
        for idx, unit in logical_units(code_lines):
            if NOT_A_FUNCTION.match(unit) or not FUNC_LIKE.search(unit):
                continue
            prev = ""
            for j in range(idx - 1, -1, -1):
                if raw_lines[j].strip():
                    prev = raw_lines[j].strip()
                    break
            if not prev.startswith(("//", "*", "/*")):
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
