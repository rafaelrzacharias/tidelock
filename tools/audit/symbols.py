#!/usr/bin/env python3
"""The link-granularity gate - the @deterministic replacement. Spec: docs/CPP-SUBSET.md §4,
docs/ARCHITECTURE.md §1, docs/TESTING.md §5.

Three checks over the audited static libs, in layer order (bottom first):

  1. Undefined symbols. A symbol is allowed only if it is defined in this lib, in a lib BELOW it,
     or matched by tools/audit/allow.txt. Unioning the defined set across all layers would let
     tl_foundation_det reference a tl_sim symbol - an upward dependency the DAG forbids - so the
     lower-layer set is built incrementally.
  2. Mutable global state. Every object file in an audited lib must have zero bytes of writable
     static storage: .data, .bss and the thread-local sections. This is what catches what no
     regex can: anonymous-namespace globals, `inline static` members, plain namespace-scope
     globals, static locals, `__thread`. Rollback restores only the registered arenas
     (docs/MEMORY.md), so a byte of it in a sim lib is state the world hash cannot see.
     `.data.rel.ro` is NOT writable static storage - it is const data holding relocations, which
     is where clang puts a const function-pointer table under the PIE default on Linux and the
     Pi. Rejecting it would have passed on Windows and failed every W1 lane in CI.
  3. Banned symbols still matched by the allowlist are reported, not silently waved through:
     the allowlist is for what a build genuinely needs, never for the tripwires the audit exists
     to trip (libm, malloc, clocks, entropy, io, floating-point markers).
"""
import argparse, fnmatch, os, re, subprocess, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def run(tool, args, target):
    r = subprocess.run([tool] + args + [target], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("symbols: %s failed on %s:\n%s" % (tool, target, r.stderr.strip()))
    return r.stdout


# Undefined symbol classes: U is a plain undefined reference, w/v are weak undefined ones. A weak
# undefined `sqrt` links to libm when libm is present and to nothing when it is not - exactly the
# kind of thing the audit exists to refuse, so it counts.
UNDEF_TYPES = ("U", "w", "v")


def names(lines, undefined):
    out = set()
    for line in lines.splitlines():
        parts = line.split()
        if not parts:
            continue
        kind = parts[0] if len(parts) == 2 else (parts[-2] if len(parts) >= 2 else "")
        if undefined:
            if kind in UNDEF_TYPES:
                out.add(parts[-1])
        else:
            if len(parts) >= 2 and kind not in UNDEF_TYPES and kind.lower() != "u":
                out.add(parts[-1])
    return out


def writable_static(section):
    """True for sections that are writable static storage (docs/CPP-SUBSET.md §1)."""
    if section.startswith(".data.rel.ro"):
        return False                       # const-after-relocation; PIE puts const tables here
    return (section == ".data" or section.startswith(".data.")
            or section == ".bss" or section.startswith(".bss.")
            or section.startswith((".tbss", ".tdata", ".tls$")))


SECTION = re.compile(r'^\s*\d+\s+(\.[\w.$]+)\s+([0-9a-fA-F]+)')
MEMBER = re.compile(r'^(.*)\((.*)\):\s+file format')


def data_bss_offenders(objdump, lib):
    """(object, section, size) for every non-empty .data/.bss in the archive."""
    out = []
    member = os.path.basename(lib)
    for line in run(objdump, ["-h"], lib).splitlines():
        m = MEMBER.match(line)
        if m:
            member = m.group(2)
            continue
        if line.strip().endswith("file format"):
            continue
        s = SECTION.match(line)
        if not s:
            continue
        section, size = s.group(1), int(s.group(2), 16)
        if size and writable_static(section):
            out.append((member, section, size))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--allow", required=True)
    ap.add_argument("--layer", action="append", default=[], metavar="NAME=PATH",
                    help="an audited lib, lowest layer first; a lib may only reference symbols "
                         "from itself or from layers given before it")
    ap.add_argument("--data-only", action="append", default=[], metavar="NAME=PATH",
                    help="a src/ lib that gets the writable-static check but not the symbol-"
                         "layering check - docs/CPP-SUBSET.md §1 bans static mutable state in all "
                         "of src/")
    ap.add_argument("--sanitized", action="store_true",
                    help="declare that this build has sanitizers on; the audit then refuses to "
                         "run rather than reporting the sanitizer runtime's own globals")
    a = ap.parse_args()

    if a.sanitized:
        sys.exit("symbols: refusing to audit a sanitized build - ASan/UBSan add their own .bss "
                 "and __asan_* symbols to every object, so a pass here would mean nothing "
                 "(docs/TESTING.md §5). Run the audit on an unsanitized preset.")

    def parse(specs, flag):
        out = []
        for spec in specs:
            if "=" not in spec:
                sys.exit("symbols: %s wants NAME=PATH, got %r" % (flag, spec))
            name, path = spec.split("=", 1)
            out.append((name, path))
        return out

    layers = parse(a.layer, "--layer")
    data_only = parse(a.data_only, "--data-only")
    if not layers:
        sys.exit("symbols: no --layer given")

    patterns = [l.split("#")[0].strip() for l in open(a.allow, encoding="utf-8")]
    patterns = [p for p in patterns if p]

    violations = []
    below = set()
    for name, path in layers:
        defined = names(run(a.nm, ["--defined-only"], path), undefined=False)
        for sym in sorted(names(run(a.nm, ["--undefined-only"], path), undefined=True)):
            if sym in defined or sym in below:
                continue
            bare = sym.lstrip("_")
            if any(fnmatch.fnmatch(sym, p) or fnmatch.fnmatch(bare, p) for p in patterns):
                continue
            violations.append("%s: undefined symbol outside the allowlist: %s" % (name, sym))
        for member, section, size in data_bss_offenders(a.objdump, path):
            violations.append("%s: %s has %d bytes of %s - writable static storage in an "
                              "audited lib (docs/CPP-SUBSET.md §1, docs/MEMORY.md)"
                              % (name, member, size, section))
        below |= defined

    for name, path in data_only:
        for member, section, size in data_bss_offenders(a.objdump, path):
            violations.append("%s: %s has %d bytes of %s - writable static storage in src/ "
                              "(docs/CPP-SUBSET.md §1)" % (name, member, size, section))

    for v in violations:
        print("ERROR " + v)
    print("symbols: %d audited layers + %d data-only libs, %d violations"
          % (len(layers), len(data_only), len(violations)))
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
