#!/usr/bin/env python3
"""Cross-target divergence gate - measurement, not pattern-matching.
Spec: docs/CPP-SUBSET.md §5, docs/BUILD.md §9 R-8, docs/TESTING.md §5.

Four adversarial reviews each found target-variable constructs that a regex missed, because the
space cannot be enumerated: `#pragma pack`, `alignas`, `[[no_unique_address]]`, `#pragma data_seg`,
`#ifdef __GNUC__`, and every macro outside whatever denylist someone thought of. This gate stops
guessing and measures instead. For every sim TU, on all three supported triples:

  1. **Preprocess** (`clang -E`) and diff. Any line of OUR source that differs between targets is a
     per-target program - which is the violation, not noise. Lines coming from clang's own
     resource-dir headers are dropped by following the `# N "file"` markers, so `size_t`'s
     expansion differing per ABI is not a finding.
  2. **Record layouts** (`-Xclang -fdump-record-layouts-complete`) and diff. Any struct whose
     sizeof, align or field offsets differ between targets changes the arena bytes and therefore
     the world hash. This catches every layout hazard in any spelling, including the ones no
     regex saw.

What it does NOT cover, and what the regex gate keeps: value divergence over identical text and
identical layouts (`char` signedness in arithmetic, `long`/`size_t` in an expression, high escape
bytes), and the discipline rules (module DAG, include firewall, contract comments). The two gates
are complements - this one replaces the layout/macro half of the old ruleset, not the whole of it.

No sysroot needed: `<stdint.h>`, `<stddef.h>` and `<limits.h>` come from clang's resource dir under
`-nostdlibinc`, and `<string.h>` - the only sanctioned header that is not - is stubbed with the four
declarations docs/CPP-SUBSET.md §1 allows. That holds exactly as long as the include allowlist
stays freestanding, which the include firewall enforces; if a hosted header is ever allowed, this
gate needs the three real sysroots and says so rather than quietly passing.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TRIPLES = [
    ("win", "x86_64-pc-windows-msvc"),
    ("linux", "x86_64-unknown-linux-gnu"),
    ("pi", "aarch64-unknown-linux-gnu"),
]

# The four declarations of docs/CPP-SUBSET.md §1. size_t comes from <stddef.h> either way, so the
# stub is exact for the sanctioned subset.
STRING_H_STUB = """#pragma once
#include <stddef.h>
extern "C" void* memcpy(void*, const void*, size_t);
extern "C" void* memmove(void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);
extern "C" int memcmp(const void*, const void*, size_t);
"""

# -nostdlibinc, NOT -nostdinc: the latter drops clang's own resource dir too, which is where
# <stdint.h>, <stddef.h> and <limits.h> live under freestanding. We want the builtin headers
# (their definitions come from __INT64_TYPE__-class builtins, i.e. the target's own ABI) and
# no platform headers at all.
BASE_FLAGS = ["-std=c++20", "-ffreestanding", "-nostdlibinc", "-nostdinc++",
              "-fno-exceptions", "-fno-rtti",
              "-DTL_SIM_TU=1", "-DTL_DEV=0", "-DTL_TIER_NETCODE=1"]

LINE_MARKER = re.compile(r'^#\s+\d+\s+"([^"]*)"')
# 1UL / 1ULL / 1L on one target and 1U on another are the same value written for a different ABI.
INT_SUFFIX = re.compile(r'\b(\d+)[uU]?(?:[lL]{1,2})[uU]?\b')
RECORD_START = re.compile(r'^\s*\*\*\* Dumping AST Record Layout')
RECORD_NAME = re.compile(r'^\s*\d+\s*\|\s*(?:struct|class|union)\s+([A-Za-z_][\w:<>, ]*)')
# dsize/nvsize/nvalign are dump bookkeeping that differs per ABI even when sizeof and every offset
# agree; they are format noise, not layout.
ABI_NOISE = re.compile(r'\b(dsize|nvsize|nvalign)=\d+,?\s*')
SYSTEM_RECORD = re.compile(r'^(__|max_align_t|_GUID|_?IMAGE_|_TP_|std::)')


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout, r.stderr


def det_sources(root, nondet):
    """Every sim TU: the .cpp files, plus one generated TU per public sim header so a header-only
    module (fx.h is one, for its whole first week) is measured before any .cpp includes it."""
    out = []
    for sub in ("sim", "foundation"):
        base = os.path.join(root, "src", sub)
        for dirpath, _dirs, files in os.walk(base):
            for name in sorted(files):
                stem, ext = os.path.splitext(name)
                if ext not in (".cpp", ".h"):
                    continue
                if sub == "foundation" and stem in nondet:
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace("\\", "/")
                if rel in ("src/foundation/fx_float.h",):
                    continue                       # the render/editor bridge, not a sim TU
                out.append(rel)
    return out


def tu_for(root, rel, tmp):
    """A compilable TU for a source: the .cpp itself, or a generated includer for a header."""
    if rel.endswith(".cpp"):
        return os.path.join(root, rel.replace("/", os.sep))
    path = os.path.join(tmp, "hdr_" + rel.replace("/", "_") + ".cpp")
    inc = rel[len("src/"):]
    open(path, "w", encoding="utf-8", newline="\n").write('#include "%s"\n' % inc)
    return path


def norm_path(p):
    """A line marker escapes backslashes, so a Windows path arrives as C:\\\\dir\\\\file. Collapsing
    any run of backslashes to one slash is the difference between this gate reading our source and
    silently comparing two empty lists - which is how it passed every fixture on first run."""
    return re.sub(r'[\\/]+', '/', p).lower()


def preprocess(clang, triple, tu, incs, root):
    """Our own preprocessed lines, with resource-dir headers dropped via the line markers."""
    rc, out, err = run([clang, "--target=" + triple, "-E"] + BASE_FLAGS + incs + [tu])
    if rc != 0:
        return None, err
    ours = (norm_path(os.path.join(root, "src")), norm_path(tu))
    keep, current = [], ""
    for line in out.splitlines():
        m = LINE_MARKER.match(line)
        if m:
            current = norm_path(m.group(1))
            continue
        if not line.strip():
            continue
        if current.startswith(ours):
            keep.append(INT_SUFFIX.sub(r"\1", line.rstrip()))
    if not keep:
        return None, ("no preprocessed line came from our own source - the line-marker filter is "
                      "wrong and this gate would pass anything")
    return keep, ""


def record_layouts(clang, triple, tu, incs):
    """{record name: normalised layout} for every record that is ours."""
    rc, out, err = run([clang, "--target=" + triple, "-fsyntax-only",
                        "-Xclang", "-fdump-record-layouts-complete"] + BASE_FLAGS + incs + [tu])
    if rc != 0:
        return None, err
    records, name, body = {}, None, []
    for line in out.splitlines():
        if RECORD_START.match(line):
            if name and not SYSTEM_RECORD.match(name):
                records[name] = "\n".join(body)
            name, body = None, []
            continue
        if name is None:
            m = RECORD_NAME.match(line)
            if m:
                name = m.group(1).strip()
        body.append(ABI_NOISE.sub("", line.rstrip()))
    if name and not SYSTEM_RECORD.match(name):
        records[name] = "\n".join(body)
    return records, ""


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        return min(len(a), len(b)), (longer[min(len(a), len(b))] if longer else ""), "<absent>"
    return None, "", ""


def check(clang, root, rel, tmp, incs, errors):
    tu = tu_for(root, rel, tmp)
    pre, lay = {}, {}
    for tag, triple in TRIPLES:
        p, err = preprocess(clang, triple, tu, incs, root)
        if p is None:
            errors.append("%s: does not compile for %s:\n    %s"
                          % (rel, triple, err.strip().splitlines()[0] if err.strip() else "?"))
            return
        pre[tag] = p
        r, err = record_layouts(clang, triple, tu, incs)
        if r is None:
            first = err.strip().splitlines()
            errors.append("%s: layout dump failed for %s:\n    %s"
                          % (rel, triple, first[0] if first else "(no diagnostic)"))
            return
        lay[tag] = r

    base_tag = TRIPLES[0][0]
    for tag, _triple in TRIPLES[1:]:
        idx, x, y = first_difference(pre[base_tag], pre[tag])
        if idx is not None:
            errors.append("%s: preprocessed source differs %s vs %s - a per-target program "
                          "(docs/CPP-SUBSET.md §5)\n    %s: %s\n    %s: %s"
                          % (rel, base_tag, tag, base_tag, x[:100], tag, y[:100]))
        for name in sorted(set(lay[base_tag]) | set(lay[tag])):
            a, b = lay[base_tag].get(name), lay[tag].get(name)
            if a != b:
                errors.append("%s: record '%s' has a different layout %s vs %s - the arena bytes "
                              "and therefore the world hash differ (docs/CPP-SUBSET.md §5)"
                              % (rel, name, base_tag, tag))


def nondet_stems(root):
    path = os.path.join(root, "src", "foundation", "CMakeLists.txt")
    m = re.search(r"set\(TL_FOUNDATION_NONDET([^)]*)\)", open(path, encoding="utf-8").read())
    if not m:
        sys.exit("targets: src/foundation/CMakeLists.txt has no TL_FOUNDATION_NONDET")
    return set(m.group(1).split())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--clang", default="clang++")
    ap.add_argument("--only", default=None, help="check one repo-relative source (selftest)")
    a = ap.parse_args()

    if not shutil.which(a.clang):
        sys.exit("targets: %s is not on PATH - the cross-target gate cannot run" % a.clang)

    # The record-layout dump is the half of this gate that no other check replaces, and the flag is
    # not in every clang. Probe it once, by name, so an unsupported compiler is a named failure
    # rather than a per-TU mystery. This clang need NOT be the pinned one (docs/BUILD.md §1): the
    # gate compares targets against each other, it does not produce a shipped binary.
    probe = os.path.join(tempfile.gettempdir(), "tl_targets_probe.cpp")
    open(probe, "w", encoding="utf-8").write("struct S { int a; };\n")
    rc, _out, err = run([a.clang, "-fsyntax-only", "-std=c++20", "-ffreestanding", "-nostdlibinc",
                         "-Xclang", "-fdump-record-layouts-complete", probe])
    if rc != 0:
        _rc2, ver, _e2 = run([a.clang, "--version"])
        detail = err.strip().splitlines()[0] if err.strip() else "(no diagnostic)"
        vline = ver.strip().splitlines()[0] if ver.strip() else "?"
        sys.exit("targets: %s does not accept -fdump-record-layouts-complete, which this gate "
                 "depends on.\n    compiler: %s\n    said: %s\n"
                 "Install a clang that supports it - any recent one will do, it need not be the "
                 "pinned major." % (a.clang, vline, detail))

    root = os.path.abspath(a.root)
    sources = [a.only] if a.only else det_sources(root, nondet_stems(root))
    errors = []
    with tempfile.TemporaryDirectory(prefix="tl_targets_") as tmp:
        open(os.path.join(tmp, "string.h"), "w", encoding="utf-8", newline="\n").write(STRING_H_STUB)
        incs = ["-I" + os.path.join(root, "src"), "-I" + tmp]
        for rel in sources:
            check(a.clang, root, rel, tmp, incs, errors)

    for e in errors:
        print("ERROR " + e)
    print("targets: %d sim TU(s) x %d triples, %d divergences"
          % (len(sources), len(TRIPLES), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
