#!/usr/bin/env python3
"""Cross-target divergence gate - measurement, not pattern-matching.
Spec: docs/CPP-SUBSET.md §5, docs/BUILD.md §9 R-8, docs/TESTING.md §5.

Four adversarial reviews found target-variable constructs a regex missed, because the space cannot
be enumerated: `#pragma pack`, `alignas`, `[[no_unique_address]]`, `#ifdef __GNUC__`, and every
macro outside whatever denylist someone thought of. This gate measures instead. For every sim TU,
on all three supported triples:

  1. **Preprocess** (`clang -E`) and diff, once per tier define set. Any line of OUR source that
     differs between targets is a per-target program - the violation, not noise. Lines from clang's
     resource-dir headers are dropped by following the `# N "file"` markers.
  2. **Record layouts** (`-Xclang -fdump-record-layouts-complete`) and diff. Any record whose
     sizeof, align or field offsets differ changes the arena bytes and therefore the world hash.

THE DESIGN RULE OF THIS FILE, learned the hard way twice: **a filter must never decide what gets
compared.** The first version dropped every line whose path failed a broken match and compared two
empty lists - passing everything. The second keyed records by a name parsed out of the dump, so an
anonymous-namespace record, an unnamed typedef'd one, a second local record with the same name, or
one a lane happened to call `__Cell` simply vanished. Both were silent passes, which is the only
failure mode that matters in a gate.

So: records are compared **positionally** (the dump order is a function of the TU, identical on
every target) with a count check, names are used only in messages, and the system records to skip
are **measured** per triple from a TU containing nothing but the sanctioned headers - not matched
by prefix. Any structural surprise is an error, never a silent skip.

What this gate does NOT cover, and the token bans in includes.py do: value divergence over
identical text and identical layouts (`char` signedness, `long`/`size_t`/`int_fast*` in an
expression, wide literals, high escape bytes), and records instantiated only from modules outside
`src/sim` + `src/foundation` (see docs/CPP-SUBSET.md §5's stated boundary).

No sysroot needed: `<stdint.h>`, `<stddef.h>` and `<limits.h>` come from clang's resource dir under
`-nostdlibinc`, and `<string.h>` is stubbed with the four declarations docs/CPP-SUBSET.md §1
allows. The boundary of that model is real and stated in §5: the freestanding `<stdint.h>` defines
the `int_fast*` family differently from every hosted libc, so those names are token-banned rather
than measured here, and `__has_include` of a platform header is uniformly false and is banned too.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TRIPLES = [
    ("win", "x86_64-pc-windows-msvc"),
    ("linux", "x86_64-unknown-linux-gnu"),
    ("pi", "aarch64-unknown-linux-gnu"),
]

# The tier define sets that reach a sim TU (docs/BUILD.md §3). The preprocess leg runs once per
# set: a `#if TL_DEV` guarding a per-target record is invisible if only one tier is measured.
TIER_DEFINES = [
    ("netcode", ["-DTL_DEV=0", "-DTL_TIER_NETCODE=1"]),
    ("dev", ["-DTL_DEV=1", "-DTL_TIER_DEV=1"]),
]

# The four declarations of docs/CPP-SUBSET.md §1. size_t comes from <stddef.h> either way.
STRING_H_STUB = """#pragma once
#include <stddef.h>
extern "C" void* memcpy(void*, const void*, size_t);
extern "C" void* memmove(void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);
extern "C" int memcmp(const void*, const void*, size_t);
"""
# Used to measure which records the sanctioned headers themselves define, per triple.
SYSTEM_PROBE = '#include <stdint.h>\n#include <stddef.h>\n#include <limits.h>\n#include <string.h>\n'

# -nostdlibinc, NOT -nostdinc: the latter drops clang's own resource dir too, which is where the
# freestanding <stdint.h>/<stddef.h>/<limits.h> live.
BASE_FLAGS = ["-std=c++20", "-ffreestanding", "-nostdlibinc", "-nostdinc++",
              "-fno-exceptions", "-fno-rtti", "-DTL_SIM_TU=1",
              # _MSC_VER is a banned platform macro in every src/ TU (docs/CPP-SUBSET.md §5,
              # enforced by includes.py's token ban) - the only thing that can still branch on it
              # is a vendor header (rapidhash.h does, for <intrin.h>). Clang predefines it for the
              # win triple regardless, and clang's OWN resource-dir intrin.h then declares ~90
              # SIMD-intrinsic records the linux/pi triples never see - a real preprocessed-text
              # difference but an inert one: rapidhash's __SIZEOF_INT128__ branch always wins on
              # every triple we build for, so the intrin.h include is dead code either way.
              # Undefining it here removes the noise at its source instead of laundering it.
              "-U_MSC_VER"]
LAYOUT_FLAGS = ["-fsyntax-only", "-Xclang", "-fdump-record-layouts-complete"]

LINE_MARKER = re.compile(r'^#\s+\d+\s+"([^"]*)"')

# int64_t is `long long` on windows-msvc and `long` on linux/aarch64 - the same 64-bit type spelled
# two ways, which made every fx<i64,FRAC> and Result<u64> read as a divergence. Both spellings
# collapse to one canonical name. This is only sound because `long` and the `int_fast*` family are
# token-banned in sim TUs (docs/CPP-SUBSET.md §5), so a `long` in a dump can only have come from a
# 64-bit typedef - never from a 32-bit Windows `long`.
CANON_TYPES = [("unsigned long long", "u64"), ("unsigned long", "u64"),
               ("long long", "i64"), ("long", "i64")]

# Integer-literal suffixes are MAPPED, never stripped. Stripping made `1LL` identical to `1`, which
# hid a genuine 64-bit-vs-32-bit shift; and a decimal-only pattern left UINT64_C(0x...) reading as
# a divergence between ULL and UL. Hex, binary and digit separators are all covered.
LITERAL = re.compile(r'\b(0[xX][0-9a-fA-F\']+|0[bB][01\']+|[0-9][0-9\']*)'
                     r'([uU][lL]{1,2}|[lL]{1,2}[uU]|[uU]|[lL]{1,2})\b')
ABI_NOISE = re.compile(r'\b(dsize|nvsize|nvalign)=\d+,?\s*')
RECORD_START = re.compile(r'^\s*\*\*\* Dumping AST Record Layout')
RECORD_NAME = re.compile(r'\|\s*(?:struct|class|union)\s+(.+?)\s*$')


def canon_literal(m):
    """1u -> 1<U>, 1L/1LL -> 1<I64>, 1UL/1ULL -> 1<U64>. Width is preserved, spelling is not."""
    suffix = m.group(2).lower()
    if "l" in suffix:
        kind = "<U64>" if "u" in suffix else "<I64>"
    else:
        kind = "<U>"
    return m.group(1).replace("'", "") + kind


def canon_text(line):
    out = ABI_NOISE.sub("", line.rstrip())
    for spelling, name in CANON_TYPES:
        out = re.sub(r'\b' + spelling + r'\b', name, out)
    return LITERAL.sub(canon_literal, out)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout, r.stderr


def norm_path(p):
    """Line markers escape backslashes, so a Windows path arrives as C:\\\\dir\\\\file."""
    return re.sub(r'[\\/]+', '/', p).lower()


def det_sources(root, nondet):
    """Every sim TU: the .cpp files, plus one generated TU per public sim header, so a header-only
    module (fx.h is one for its whole first week) is measured before any .cpp includes it."""
    out = []
    for sub in ("sim", "foundation"):
        base = os.path.join(root, "src", sub)
        for dirpath, dirs, files in os.walk(base):
            dirs.sort()                            # host-independent order, so is the error order
            for name in sorted(files):
                stem, ext = os.path.splitext(name)
                if ext not in (".cpp", ".h") or (sub == "foundation" and stem in nondet):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace("\\", "/")
                if rel == "src/foundation/fx_float.h":
                    continue                       # the render/editor bridge, not a sim TU
                out.append(rel)
    return sorted(out)


def tu_for(root, rel, tmp):
    if rel.endswith(".cpp"):
        return os.path.join(root, rel.replace("/", os.sep))
    path = os.path.join(tmp, "hdr_" + rel.replace("/", "_") + ".cpp")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write('#include "%s"\n' % rel[len("src/"):])
    return path


def preprocess(clang, triple, defines, tu, incs, root):
    rc, out, err = run([clang, "--target=" + triple, "-E"] + BASE_FLAGS + defines + incs + [tu])
    if rc != 0:
        return None, err
    ours = (norm_path(os.path.join(root, "src")), norm_path(tu))
    keep, current = [], ""
    for line in out.splitlines():
        m = LINE_MARKER.match(line)
        if m:
            current = norm_path(m.group(1))
            continue
        if line.strip() and current.startswith(ours):
            keep.append(canon_text(line))
    if not keep:
        return None, ("no preprocessed line came from our own source - the line-marker filter is "
                      "wrong and this gate would compare two empty lists")
    return keep, ""


def parse_records(dump):
    """[(name, normalised block)] in dump order. The name is for the message only - nothing is
    dropped or keyed by it."""
    blocks, name, body = [], None, None
    for line in dump.splitlines():
        if RECORD_START.match(line):
            if body is not None:
                blocks.append((name or "<unnamed>", "\n".join(body)))
            name, body = None, []
            continue
        if body is None:
            continue
        if name is None:
            m = RECORD_NAME.search(line)
            if m:
                name = m.group(1).strip()
        body.append(canon_text(line))
    if body is not None:
        blocks.append((name or "<unnamed>", "\n".join(body)))
    return blocks


def record_layouts(clang, triple, tu, incs, system_names):
    rc, out, err = run([clang, "--target=" + triple] + LAYOUT_FLAGS + BASE_FLAGS
                       + TIER_DEFINES[0][1] + incs + [tu])
    if rc != 0:
        return None, err
    return [b for b in parse_records(out) if b[0] not in system_names], ""


def system_record_names(clang, triple, incs, tmp):
    """Measured, not guessed: whatever the sanctioned headers alone define on this triple. A prefix
    filter here was user-controllable - a record named `__Cell` or `max_align_tx` disappeared."""
    probe = os.path.join(tmp, "system_probe.cpp")
    with open(probe, "w", encoding="utf-8", newline="\n") as f:
        f.write(SYSTEM_PROBE)
    rc, out, _err = run([clang, "--target=" + triple] + LAYOUT_FLAGS + BASE_FLAGS
                        + TIER_DEFINES[0][1] + incs + [probe])
    return {b[0] for b in parse_records(out)} if rc == 0 else set()


def check(clang, root, rel, tmp, incs, system_names, errors):
    tu = tu_for(root, rel, tmp)
    base_tag = TRIPLES[0][0]

    for tier, defines in TIER_DEFINES:
        pre = {}
        for tag, triple in TRIPLES:
            p, err = preprocess(clang, triple, defines, tu, incs, root)
            if p is None:
                first = [l for l in err.strip().splitlines() if l.strip()]
                errors.append("%s: preprocessing failed for %s (%s tier):\n    %s"
                              % (rel, triple, tier, first[0] if first else "(no diagnostic)"))
                return
            pre[tag] = p
        for tag, _t in TRIPLES[1:]:
            for i, (x, y) in enumerate(zip(pre[base_tag], pre[tag])):
                if x != y:
                    errors.append("%s: preprocessed source differs %s vs %s in the %s tier - a "
                                  "per-target program (docs/CPP-SUBSET.md §5)\n    %s: %s\n    %s: %s"
                                  % (rel, base_tag, tag, tier, base_tag, x[:100], tag, y[:100]))
                    break
            else:
                if len(pre[base_tag]) != len(pre[tag]):
                    errors.append("%s: %d preprocessed lines on %s vs %d on %s in the %s tier"
                                  % (rel, len(pre[base_tag]), base_tag, len(pre[tag]), tag, tier))

    lay = {}
    for tag, triple in TRIPLES:
        r, err = record_layouts(clang, triple, tu, incs, system_names[tag])
        if r is None:
            first = [l for l in err.strip().splitlines() if l.strip()]
            errors.append("%s: layout dump failed for %s:\n    %s"
                          % (rel, triple, first[0] if first else "(no diagnostic)"))
            return
        lay[tag] = r

    for tag, _t in TRIPLES[1:]:
        a, b = lay[base_tag], lay[tag]
        if len(a) != len(b):
            errors.append("%s: %d records laid out on %s but %d on %s - a record exists on one "
                          "target and not the other (docs/CPP-SUBSET.md §5)"
                          % (rel, len(a), base_tag, len(b), tag))
            continue
        for pos, ((na, ba), (nb, bb)) in enumerate(zip(a, b)):
            if ba != bb:
                errors.append("%s: record #%d ('%s'/'%s') has a different layout %s vs %s - the "
                              "arena bytes and therefore the world hash differ "
                              "(docs/CPP-SUBSET.md §5)" % (rel, pos, na, nb, base_tag, tag))


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

    root = os.path.abspath(a.root)
    sources = [a.only] if a.only else det_sources(root, nondet_stems(root))
    errors = []
    with tempfile.TemporaryDirectory(prefix="tl_targets_") as tmp:
        with open(os.path.join(tmp, "string.h"), "w", encoding="utf-8", newline="\n") as f:
            f.write(STRING_H_STUB)
        # vendor/rapidhash: the one vendor header a det TU includes (src/foundation/hash.cpp),
        # per its wrap-module restriction in tools/audit/includes.py's BACKEND_HEADERS.
        incs = ["-I" + os.path.join(root, "src"), "-I" + tmp,
                "-I" + os.path.join(root, "vendor", "rapidhash")]

        # The layout dump has to work for EVERY triple, not just the host's: clang 18 crashes
        # dumping layouts for x86_64-pc-windows-msvc from Linux, and a host-only probe walked past
        # it. This clang need not be the pinned major - it compares targets, it ships nothing.
        probe = os.path.join(tmp, "probe.cpp")
        with open(probe, "w", encoding="utf-8", newline="\n") as f:
            f.write("struct S { int a; };\n")
        _rc, ver, _e = run([a.clang, "--version"])
        vline = ver.strip().splitlines()[0] if ver.strip() else "?"
        system_names = {}
        for tag, triple in TRIPLES:
            rc, _out, err = run([a.clang, "--target=" + triple] + LAYOUT_FLAGS + BASE_FLAGS
                                + TIER_DEFINES[0][1] + [probe])
            if rc != 0:
                detail = [l for l in err.strip().splitlines() if l.strip()]
                sys.exit("targets: %s cannot dump record layouts for %s, which this gate depends "
                         "on.\n    compiler: %s\n    said: %s\nInstall a clang that can - any "
                         "recent one will do, it need not be the pinned major."
                         % (a.clang, triple, vline, detail[0] if detail else "(no diagnostic)"))
            system_names[tag] = system_record_names(a.clang, triple, incs, tmp)

        for rel in sources:
            check(a.clang, root, rel, tmp, incs, system_names, errors)

    for e in errors:
        print("ERROR " + e)
    print("targets: %d sim TU(s) x %d triples x %d tiers, %d divergences"
          % (len(sources), len(TRIPLES), len(TIER_DEFINES), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
