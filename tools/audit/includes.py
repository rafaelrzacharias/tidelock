#!/usr/bin/env python3
"""Include firewall + source-discipline grep. Spec: docs/CPP-SUBSET.md §1, §4, §6;
docs/TESTING.md §5. Scope: src/ only - vendor/ and tools/ are exempt by design, and tests/ has
the io exemption of docs/TESTING.md §8 R-2.

Gates, all blocking:
  1. system includes outside the allowlist (per-directory)
  2. backend headers outside their wrap module
  3. `float`/`double` tokens in sim TUs (src/sim/ and the det half of src/foundation/)
  4. `static` mutable state, `thread_local`, `std::`
  5. header contracts: a public header opens with a contract block naming its spec section, and
     every declared public function carries a contract comment (docs/CPP-SUBSET.md §6)
"""

import sys as _sys
_sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import argparse, os, re, sys

SYS_ALLOW = {"stdint.h", "stddef.h", "string.h", "limits.h"}
SYS_ALLOW_DIRS = {                        # additional system headers, by path prefix
    "src/render": {"math.h"},
    "src/editor": {"math.h"},
    "src/platform": {"math.h"},
}
BACKEND_FREE = ("src/platform/impl_sdl3", "src/platform/impl_headless")   # OS headers live here
MODULE_DIRS = ("foundation", "core", "sim", "render", "net", "platform", "editor", "app", "script")

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

# The float/double token ban covers every TU a sim build links. Two named exceptions, both from
# the docs: tl_types.h must declare f32/f64 (docs/CPP-SUBSET.md §1) and fx_float.h is the render/
# editor/tools bridge (docs/FX-PALETTE.md §6). The non-det foundation stems are the
# tl_foundation half of docs/BUILD.md §10.2 and are not sim TUs.
DET_ROOTS = ("src/sim", "src/foundation")
FLOAT_EXEMPT = {"tl_types.h", "fx_float.h",
                "jobs.cpp", "jobs.h", "mem_pool.cpp", "mem_pool.h", "fmt.cpp", "fmt.h",
                "interner.cpp", "interner.h", "atomic.cpp", "atomic.h",
                "alloc_shim.cpp", "alloc_shim.h"}
THREAD_LOCAL_EXEMPT = {"src/foundation/jobs.cpp", "src/foundation/jobs.h"}   # the worker slot

INC_SYS = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
INC_LOCAL = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
STATIC_MUT = re.compile(r'^\s*static\s+(?!(?:const|constexpr|inline\s+const)\b)')
FUNC_DECL = re.compile(r'^[A-Za-z_][A-Za-z0-9_:<>,\s\*&\[\]]*\b\w+\s*\([^;{]*\)\s*(?:const\s*)?;\s*$')


def rel(root, path):
    return os.path.relpath(path, root).replace("\\", "/")


def check_file(root, path, errors):
    r = rel(root, path)
    text = open(path, encoding="utf-8").read()
    lines = text.splitlines()
    is_header = path.endswith(".h")
    in_backend_free = any(r.startswith(p) for p in BACKEND_FREE)

    allow = set(SYS_ALLOW)
    for prefix, extra in SYS_ALLOW_DIRS.items():
        if r.startswith(prefix):
            allow |= extra

    for i, line in enumerate(lines, 1):
        m = INC_SYS.match(line)
        if m:
            hdr = m.group(1)
            if hdr not in allow and not in_backend_free:
                errors.append("%s:%d: system include <%s> is not on the allowlist "
                              "(docs/CPP-SUBSET.md §1)" % (r, i, hdr))
        m = INC_LOCAL.match(line)
        if m and not m.group(1).startswith(MODULE_DIRS):
            errors.append('%s:%d: include "%s" must use the module path '
                          "(docs/CPP-SUBSET.md §6)" % (r, i, m.group(1)))
        for token, prefixes in BACKEND_HEADERS.items():
            if (INC_SYS.match(line) or INC_LOCAL.match(line)) and token in line:
                if not any(r.startswith(p) for p in prefixes):
                    errors.append("%s:%d: backend header %s outside its wrap module %s "
                                  "(docs/BUILD.md §4)" % (r, i, token, prefixes))
        if STATIC_MUT.match(line):
            errors.append("%s:%d: static mutable state (docs/CPP-SUBSET.md §1): %s"
                          % (r, i, line.strip()[:70]))
        if "thread_local" in line and r not in THREAD_LOCAL_EXEMPT:
            errors.append("%s:%d: thread_local outside the job system (docs/CPP-SUBSET.md §1)" % (r, i))
        if "std::" in line:
            errors.append("%s:%d: std:: in src/ (docs/CPP-SUBSET.md §1)" % (r, i))

    if any(r.startswith(p) for p in DET_ROOTS) and os.path.basename(r) not in FLOAT_EXEMPT:
        for i, line in enumerate(lines, 1):
            if re.search(r'\b(float|double)\b', line):
                errors.append("%s:%d: float/double token in a sim TU (docs/CANON.md) : %s"
                              % (r, i, line.strip()[:70]))

    if is_header:
        head = "\n".join(lines[:30])
        if not re.search(r'//.*Spec:\s*docs/[A-Z0-9-]+\.md\s*§', head):
            errors.append("%s:1: public header has no contract block naming its spec section "
                          "(docs/CPP-SUBSET.md §6)" % r)
        for i, line in enumerate(lines, 1):
            if FUNC_DECL.match(line) and not line.lstrip().startswith("//"):
                prev = ""
                for j in range(i - 2, -1, -1):
                    if lines[j].strip():
                        prev = lines[j].strip()
                        break
                if not prev.startswith("//"):
                    errors.append("%s:%d: public function has no contract comment "
                                  "(docs/CPP-SUBSET.md §6): %s" % (r, i, line.strip()[:60]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    a = ap.parse_args()
    src = os.path.join(a.root, "src")
    errors = []
    for dirpath, _dirs, files in os.walk(src):
        for name in sorted(files):
            if name.endswith((".h", ".cpp")):
                check_file(a.root, os.path.join(dirpath, name), errors)
    for e in errors:
        print("ERROR " + e)
    print("includes: %d files checked, %d violations"
          % (sum(1 for d, _x, f in os.walk(src) for n in f if n.endswith((".h", ".cpp"))), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
