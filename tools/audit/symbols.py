#!/usr/bin/env python3
"""Symbol audit - the @deterministic replacement. Spec: docs/CPP-SUBSET.md §4, docs/TESTING.md §5.

Runs `llvm-nm --undefined-only` over every audited static lib and fails on any undefined symbol
that is neither defined elsewhere in the audited set nor matched by tools/audit/allow.txt. This
is a callgraph ban at link granularity: malloc, libm, clocks, entropy and io cannot reach sim
code without showing up here.
"""

import sys as _sys
_sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import argparse, fnmatch, subprocess, sys, os


def nm(nm_path, args, lib):
    r = subprocess.run([nm_path] + args + [lib], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("symbols: %s failed on %s:\n%s" % (nm_path, lib, r.stderr.strip()))
    return r.stdout.splitlines()


def names(lines, undefined):
    out = set()
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if undefined:
            # "                 U name"  /  "U name"
            if parts[0] != "U" and (len(parts) < 2 or parts[-2] != "U"):
                continue
            out.add(parts[-1])
        else:
            if len(parts) >= 2 and parts[-2] not in ("U", "u"):
                out.add(parts[-1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--allow", required=True)
    ap.add_argument("libs", nargs="+")
    a = ap.parse_args()

    patterns = [l.split("#")[0].strip() for l in open(a.allow, encoding="utf-8")]
    patterns = [p for p in patterns if p]

    defined = set()
    for lib in a.libs:
        defined |= names(nm(a.nm, ["--defined-only"], lib), undefined=False)

    violations = []
    for lib in a.libs:
        for sym in sorted(names(nm(a.nm, ["--undefined-only"], lib), undefined=True)):
            if sym in defined:
                continue
            bare = sym.lstrip("_")
            if any(fnmatch.fnmatch(sym, p) or fnmatch.fnmatch(bare, p) for p in patterns):
                continue
            violations.append((os.path.basename(lib), sym))

    for lib, sym in violations:
        print("ERROR %s: undefined symbol outside the allowlist: %s" % (lib, sym))
    print("symbols: %d libs, %d violations" % (len(a.libs), len(violations)))
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
