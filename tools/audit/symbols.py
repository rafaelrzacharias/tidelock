#!/usr/bin/env python3
"""The link-granularity gate - the @deterministic replacement. Spec: docs/CPP-SUBSET.md §4,
docs/ARCHITECTURE.md §1, docs/TESTING.md §5.

Two checks over the audited static libs, in layer order (bottom first):

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

The allowlist is for what a build genuinely needs, never for the tripwires the audit exists to
trip (libm, malloc, clocks, entropy, io, floating-point markers); tools/audit/allow.txt records
which entries were removed for that reason and why.
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


def tooling_stems(root):
    """RR-7 (docs/CPP-SUBSET.md §1): the ONE non-det stem set allowed writable static storage - the
    tooling plane is never hashed, snapshotted, or part of a world's registered arena set. Parsed
    from src/foundation/CMakeLists.txt's TL_FOUNDATION_TOOLING line, the same single home
    tools/audit/includes.py reads, so the two lists cannot drift apart. Returns the empty set (no
    exemption) when --root is not given, so this check is opt-in, never accidentally silent.

    A stem alone is NOT the exemption - see --tooling-lib. `log`, `prof`, `probe` and `crash` are
    ordinary words; keying only on the archive member's stem exempted a `log.o` in ANY --data-only
    lib (measured: a fabricated tl_platform with 4 bytes of .data in log.o reported 0 violations),
    which is the whole of src/ minus the audited libs. includes.py's twin exemption was already
    scoped to src/foundation/; this one was not."""
    if not root:
        return set()
    path = os.path.join(root, "src", "foundation", "CMakeLists.txt")
    text = open(path, encoding="utf-8").read()
    m = re.search(r"set\(TL_FOUNDATION_TOOLING([^)]*)\)", text)
    if not m:
        sys.exit("symbols: src/foundation/CMakeLists.txt has no set(TL_FOUNDATION_TOOLING ...)")
    return set(m.group(1).split())


# docs/LUAU-LAYER.md section 10.12's other done criterion: "the symbol audit shows lua_*/luau_*
# symbols only in tl_script". The include firewall (tools/audit/includes.py, BACKEND_HEADERS) keeps
# the HEADERS in one module; this keeps the SYMBOLS there, which is the claim that actually
# matters - a hand-written extern declaration needs no header at all, and that is exactly how a
# lua_State* would escape the wrap module without tripping a single include check.
#
# Checked on every registered lib except the wrap lib itself, and on undefined references (a lib
# that CALLS into Luau) as well as definitions (a lib that reimplements a lua_* name). The C
# names are what to match: vendor/luau is built with LUA_API=extern "C", so there is no mangling
# to see through, and that is also why the check can be a prefix match rather than a demangle.
# The C API, matched on the RAW name under either platform's leading-underscore convention.
VENDOR_C_PREFIXES = ("lua_", "luaL_", "luau_", "luaopen_")
# ...and everything else, matched on the DEMANGLED name. Luau's internal VM API is C++-linkage
# (`_Z12luaS_newlstrP9lua_StatePKcm`) and the Compiler carries 8,483 mangled `Luau::` symbols;
# review round 1 (D5) measured that a lib with a hand-written C++-linkage extern taking a
# `lua_State*` - verbatim the escape this gate's error message describes - passed with 0
# violations. Both existing fixtures were `extern "C"`, so neither could ever have caught it.
#
# A word-boundary search rather than a prefix test, because MSVC demangling puts the return type
# and calling convention first (`int __cdecl luaS_newlstr(...)`) where Itanium does not. The
# lookbehind is what keeps our OWN names out: `tl_luau_alloc` has `luau` preceded by `_`.
VENDOR_DEMANGLED = re.compile(r'(?<![A-Za-z0-9_])(lua[A-Za-z0-9_]*|Luau::)')


def vendor_symbol(raw, demangled):
    """True for any Luau symbol - the public C API by raw name, the C++ surface by demangled one.
    Both are checked because a name the demangler does not recognise comes back unchanged, and a
    name it does recognise no longer starts with the identifier."""
    bare = raw.lstrip("_")
    if any(bare.startswith(p) for p in VENDOR_C_PREFIXES):
        return True
    return bool(VENDOR_DEMANGLED.search(demangled))


def symbol_display_pairs(nm, path):
    """[(raw, demangled)] for every symbol in `path`, defined and undefined alike.

    Two nm runs over one archive, zipped by line position: `--demangle` rewrites the NAME column
    and leaves every other column and the line order untouched, so position is a reliable key
    where a name is not (a demangled signature contains spaces, and the raw/demangled forms of
    one symbol share nothing to join on). A length mismatch is a hard error rather than a
    best-effort zip - silently comparing two differently-sized lists is the class docs/LESSONS.md
    tracks as the empty-list silent pass."""
    raw_lines = run(nm, [], path).splitlines()
    dem_lines = run(nm, ["--demangle"], path).splitlines()
    if len(raw_lines) != len(dem_lines):
        sys.exit("symbols: nm and nm --demangle disagree on line count for %s (%d vs %d)"
                 % (path, len(raw_lines), len(dem_lines)))
    out = []
    for rl, dl in zip(raw_lines, dem_lines):
        rp = rl.split()
        # Skip everything that is not a symbol line. An archive listing interleaves MEMBER HEADER
        # lines (`luau_alloc.cpp.o:`) and blanks with the symbols, and a parse that took the last
        # whitespace field unconditionally read those filenames as symbols - which is how the
        # first run of this gate reported `luau_alloc.cpp.o` and `luacomp.cpp.o` as Luau symbols
        # leaking out of the wrap module. The type column is the discriminator: exactly one
        # character, and it is the second-to-last field on a defined symbol and the first on an
        # undefined one.
        if len(rp) < 2:
            continue
        kind = rp[0] if len(rp) == 2 else rp[-2]
        if len(kind) != 1 or not kind.isalpha():
            continue
        raw = rp[-1]
        dp = dl.split()
        # The demangled name is everything after the address+type columns, rejoined: a signature
        # has spaces in it, so parts[-1] would be `long)`.
        dem = " ".join(dp[len(rp) - 1:]) if len(dp) >= len(rp) else dl.strip()
        out.append((raw, dem))
    return out


def static_allow_libs(root):
    """{(lib, stem)} that may hold writable static storage. The SAME file includes.py reads
    (tools/audit/static_allow.txt), keyed by lib + stem here because this gate sees archives and
    not paths. RR-18 and RR-19 are its only rows; each is a ruling recorded in TODO.md.

    Returns the empty set with no --root, so the exemption is opt-in and never accidentally
    silent - the same rule tooling_stems() follows for RR-7."""
    if not root:
        return set()
    path = os.path.join(root, "tools", "audit", "static_allow.txt")
    if not os.path.exists(path):
        return set()
    out = set()
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        row = line.split("#")[0].split()
        if not row:
            continue
        if len(row) != 3:
            sys.exit("tools/audit/static_allow.txt:%d: want '<lib> <directory> <stem>', got %r"
                     % (n, line.strip()))
        out.add((row[0], row[2]))
    return out


def stem_of_member(member):
    """The source stem an archive member's object file was compiled from, independent of the
    object-naming convention the generator used (CMake+Ninja nests it under CMakeFiles/<target>.dir/
    and spells the extension .o on the GNU driver, .obj on the MSVC one)."""
    base = os.path.basename(member.replace("\\", "/"))
    base = re.sub(r"\.(o|obj)$", "", base, flags=re.IGNORECASE)
    base = re.sub(r"\.(cpp|cc|cxx)$", "", base, flags=re.IGNORECASE)
    return base


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
    ap.add_argument("--root", default=None,
                    help="repo root - locates src/foundation/CMakeLists.txt's TL_FOUNDATION_TOOLING "
                         "list (RR-7's writable-static exemption for the non-audited tooling "
                         "plane). Omit to run with no exemption at all.")
    ap.add_argument("--tooling-lib", default=None, metavar="NAME",
                    help="the ONE --data-only lib RR-7's stem exemption applies to (the non-det "
                         "half of src/foundation/). Both this and --root are required for any "
                         "exemption at all: a stem named log/prof/probe/crash in any OTHER lib is "
                         "an ordinary writable-static violation (docs/CPP-SUBSET.md §9 R-4).")
    ap.add_argument("--wrap-lib", action="append", default=[], metavar="NAME",
                    help="a lib that MAY reference the vendored Luau C API (docs/LUAU-LAYER.md "
                         "section 10.12: lua_*/luau_* symbols only in tl_script). Every other "
                         "registered lib is checked, defined AND undefined. Omit for no check at "
                         "all rather than a silently empty one - see the zero-check below.")
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

    static_allow = static_allow_libs(a.root)

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
            if (name, stem_of_member(member)) in static_allow:
                continue   # tools/audit/static_allow.txt: a named ruling, lib + stem
            violations.append("%s: %s has %d bytes of %s - writable static storage in an "
                              "audited lib (docs/CPP-SUBSET.md §1, docs/MEMORY.md)"
                              % (name, member, size, section))
        below |= defined

    tooling = tooling_stems(a.root) if a.tooling_lib else set()
    for name, path in data_only:
        # RR-7 is a LIB + STEM exemption, never a stem alone: only the named non-det foundation
        # lib may hold the tooling plane, so a `log.o` that turns up in core/, platform/ or
        # editor/ is reported exactly like any other writable static storage.
        exempt = tooling if name == a.tooling_lib else set()
        for member, section, size in data_bss_offenders(a.objdump, path):
            if stem_of_member(member) in exempt:
                continue   # RR-7: the tooling plane, named in TL_FOUNDATION_TOOLING, is exempt
            if (name, stem_of_member(member)) in static_allow:
                continue   # tools/audit/static_allow.txt: a named ruling, lib + stem
            violations.append("%s: %s has %d bytes of %s - writable static storage in src/ "
                              "(docs/CPP-SUBSET.md §1)" % (name, member, size, section))

    # The vendored-symbol confinement (docs/LUAU-LAYER.md section 10.12). A filter that matches
    # nothing must be an error, not a clean run (docs/LESSONS.md, third occurrence of that class):
    # if --wrap-lib names a lib that is not registered, the check would silently cover everything
    # or nothing depending on spelling, so the name is verified against the registered set first.
    if a.wrap_lib:
        registered = {n for n, _ in layers} | {n for n, _ in data_only}
        for w in a.wrap_lib:
            if w not in registered:
                sys.exit("symbols: --wrap-lib %s is not a registered lib (%s)"
                         % (w, ", ".join(sorted(registered))))
        for name, path in layers + data_only:
            if name in a.wrap_lib:
                continue
            for raw, dem in sorted(set(symbol_display_pairs(a.nm, path))):
                if vendor_symbol(raw, dem):
                    shown = dem if dem and dem != raw else raw
                    violations.append(
                        "%s: Luau symbol %s outside the wrap module %s - a lua_State* can leave "
                        "the module through a hand-written extern with no #include to catch it "
                        "(docs/LUAU-LAYER.md section 10.12)" % (name, shown, "/".join(a.wrap_lib)))

    for v in violations:
        print("ERROR " + v)
    print("symbols: %d audited layers + %d data-only libs, %d wrap libs, %d violations"
          % (len(layers), len(data_only), len(a.wrap_lib), len(violations)))
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
