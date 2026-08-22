#!/usr/bin/env python3
"""The two build-time fingerprints. Spec: docs/BUILD.md §5, §9 R-8, §10.3 (tools/ is exempt from
the C++ subset, docs/CPP-SUBSET.md §0).

`build_id` - TARGET-INDEPENDENT, and the value peers refuse a session over. It covers exactly the
things that can change a tick's bytes, and nothing else:

  1. the git tree hash of src/, cmake/, CMakeLists.txt, vendor/, script/sim/, script/lib/ and
     toolchain/VERSIONS, plus `git diff HEAD` over them and the content of every untracked-or-
     ignored source file under them (a .gitignore'd .cpp is still compiled by the glob)
  2. the canonical compile tokens of the TUs under src/: EVERY token except an explicit drop-list
     of target-inherent and cosmetic ones (driver name, triple, include paths, output and
     dependency paths, warning flags, optimisation and debug-info levels, and the two driver
     spellings of the same switch). A drop-list, not a keep-list, on purpose: an unrecognised
     flag is hashed, so a new one shows up as a loud peer mismatch instead of a silent hole -
     `-include evil.h`, `-U TL_DEV`, `-funsigned-char`, `-fshort-enums` and `-fpack-struct` all
     change layout or values and all used to be dropped.
  3. the tier name
  4. FX_PALETTE_REV, parsed from src/foundation/fx_palette.h
  5. the precompiled sim-script bytecode manifest, in load order

What it deliberately does NOT cover: the compiler, its version and target triple, the optimisation
level, debug-info and warning flags, and driver spelling. Under fixed point those cannot change a
result except through UB (docs/BUILD.md §1), and hashing them made a PC + Deck + Pi session
impossible to hand-shake - the one thing docs/NETCODE.md §19.5 exists to do. `cmake/` is in the
tree hash, so a change to the flag set itself is still caught, portably.

`build_env` - LOCAL. Compiler string plus the full resolved compile commands. Reported in CSV
headers, crash reports and soak metadata, never compared between peers: it is what a desync
investigation reads first, not a reason to refuse a connection.

Emits build_id.cpp (TL_BUILD_ID[32] and TL_BUILD_ENV[32]), build_id.txt, build_env.txt and
flags.txt; each is rewritten only when its content changes, so a stable tree never relinks.
"""
import argparse, hashlib, json, os, subprocess, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FINGERPRINTED = ["src", "cmake", "CMakeLists.txt", "CMakePresets.json",
                 "vendor", "script/sim", "script/lib", "toolchain/VERSIONS"]

# Extensions that can reach a translation unit or the build graph. An untracked-or-ignored file
# with one of these under a fingerprinted path is hashed.
SOURCE_EXT = (".h", ".hpp", ".inc", ".c", ".cc", ".cpp", ".luau", ".cmake", ".txt", ".json",
              ".s", ".S")

# -ffast-math is banned in every tier (docs/CPP-SUBSET.md §7). It is not a fingerprint question:
# a build carrying it is not a tidelock build, so the fingerprint refuses rather than recording it.
FAST_MATH = ("-ffast-math", "-Ofast", "/fp:fast", "-funsafe-math-optimizations",
             "-ffinite-math-only", "-fassociative-math", "-freciprocal-math")

# Defines the platform or the CMake generator injects; dropping them is part of what lets
# win/linux/pi4 agree.
PLATFORM_DEFINES = {
    "WIN32", "_WINDOWS", "UNICODE", "_UNICODE", "_MT", "_DLL",
    "_HAS_EXCEPTIONS", "_CRT_SECURE_NO_WARNINGS", "_GNU_SOURCE",
}

# Tokens that are target-inherent or cosmetic. EVERYTHING ELSE IS HASHED. Two spellings of one
# switch both appear here (e.g. -fno-exceptions and /EHs-c-) because the fact they encode is
# already covered by the tier and by cmake/ being in the tree hash. If a token that matters ever
# lands here the cross-target build_id job in pr.yml is what catches it: it builds netcode-win and
# netcode-linux from one checkout and diffs build_id.txt.
DROP_EXACT = {
    # `--` ends option parsing on the clang-cl command line CMake generates and does not appear on
    # the GNU driver's. It is pure driver syntax, and leaving it hashed made build_id differ
    # between netcode-win and netcode-linux - R-8 unmet in practice, found by the third review
    # before pr.yml's cross-target job could go red.
    "--",
    "-c", "-nologo", "/nologo", "-TP", "-TC", "/TP", "/TC",
    "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "/EHs-c-", "/EHsc", "/GR-", "/Zc:threadSafeInit-",
    "/MD", "/MDd", "/MT", "/MTd", "/Z7", "/Zi", "/ZI",
    "-nostdinc++", "/clang:-nostdinc++", "-fPIC", "-fPIE", "-fno-PIC", "-fno-PIE",
    "-pipe", "-w", "/WX", "-Werror",
}
DROP_PREFIX = (
    "-o", "/Fo", "/Fd", "/Fp", "-MD", "-MT", "-MF", "-MMD", "-clang:-M", "-M",
    "-I", "/I", "-isystem", "-iquote", "-idirafter",
    "-W", "/W", "-O", "/O", "-g", "/DEBUG",
    "--target=", "-target", "--sysroot", "-fdiagnostics", "-fcolor-diagnostics",
    "-fansi-escape-codes", "--driver-mode", "-x", "/std-",
)


def drop(token):
    if token in DROP_EXACT:
        return True
    return token.startswith(DROP_PREFIX)

SEP = b"\x00"


def git(repo, *args):
    try:
        r = subprocess.run(["git", "-C", repo] + list(args), capture_output=True)
        return r.stdout if r.returncode == 0 else None
    except OSError:
        return None


def feed(h, label, data=b""):
    h.update(label.encode("utf-8"))
    h.update(SEP)
    h.update(data)
    h.update(SEP)


def feed_file(h, label, path):
    try:
        with open(path, "rb") as f:
            feed(h, label, f.read())
    except OSError:
        feed(h, "unreadable:" + label)


def load_commands(path, repo, binary_dir):
    """[(repo-relative file, tokenised command)], or None when there is no compile database."""
    if not path or not os.path.exists(path):
        return None
    repo_n = os.path.normpath(repo).replace("\\", "/")
    bin_n = os.path.normpath(binary_dir or "").replace("\\", "/")

    def tokenise(s):
        s = s.replace("\\", "/")
        if bin_n:
            s = s.replace(bin_n, "$BUILD")
        return s.replace(repo_n, "$REPO")

    rows = []
    for e in json.load(open(path, encoding="utf-8")):
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        rows.append((tokenise(e["file"]), tokenise(cmd)))
    return sorted(rows)


def canonical_tokens(rows):
    """Input 2: the semantic half of the compile line, over TUs in src/ only.

    Vendor TUs carry platform-specific defines of their own and are not sim code; their content is
    covered by the vendor/ tree hash instead."""
    keep = set()
    matched = 0
    for f, cmd in rows or []:
        if "$REPO/src/" not in f:
            continue
        matched += 1
        tokens = []
        for t in cmd.split():
            if t in FAST_MATH or t.startswith("-ffast-math"):
                sys.exit("fingerprint: %s is on the compile line for %s - it is banned in every "
                         "tier (docs/CPP-SUBSET.md §7)" % (t, f))
            # A define smuggled through the preprocessor driver used to be swallowed whole by the
            # -W prefix drop: `-Wp,-DTL_EVIL=1` left build_id unchanged.
            if t.startswith("-Wp,") or t.startswith("-Xpreprocessor,"):
                tokens.extend(t.split(",")[1:])
            else:
                tokens.append(t)
        for i, t in enumerate(tokens):
            if i == 0:
                continue                          # the compiler executable
            if t[:2] in ("-U", "/U") and len(t) > 2:
                keep.add("U:" + t[2:])
            elif t[:2] in ("-D", "/D") and len(t) > 2:
                define = t[2:]
                if define.split("=", 1)[0] not in PLATFORM_DEFINES:
                    keep.add("D:" + define)
            elif t[:1] in ("-", "/") and t[1:5] in ("std=", "std:"):
                # clang spells it -std=c++20, clang-cl spells it -std:c++20 or /std:c++20. Same
                # fact; unnormalised, the two targets disagree on build_id.
                keep.add("std:" + t[5:])
            elif t.endswith((".cpp", ".cc", ".c", ".obj", ".o")):
                continue                          # the source and object of this TU
            elif drop(t):
                continue
            else:
                keep.add("tok:" + t)              # unknown -> hashed, and therefore loud
    if rows and not matched:
        sys.exit("fingerprint: the compile database has %d entries and none of them is under "
                 "src/ - the paths did not tokenise, so input 2 would be empty and build_id "
                 "silently weaker. Check --repo against the database's paths." % len(rows))
    return sorted(keep)


def tree(h, repo, allow_no_git):
    """Input 1.

    Without a git repository this function can see nothing: tree hashes come back empty, the diff
    is empty and ls-files returns nothing, so every edit to src/ would produce the SAME build_id.
    An exported or zip-copied tree therefore fails loudly here rather than fingerprinting air
    (CLAUDE.md: fail loudly and explicitly). --allow-no-git exists for the selftest, which
    fingerprints throwaway trees on purpose."""
    if git(repo, "rev-parse", "--git-dir") is None:
        if not allow_no_git:
            sys.exit("fingerprint: %s is not a git repository - build_id would be blind to every "
                     "source change. Clone the repo instead of copying it, or pass --allow-no-git "
                     "if you know the id is meaningless (docs/BUILD.md §5)." % repo)
        feed(h, "no-git")

    for path in FINGERPRINTED:
        if not os.path.exists(os.path.join(repo, path)):
            feed(h, "absent:" + path)
            continue
        out = git(repo, "rev-parse", "HEAD:" + path)
        feed(h, "tree:" + path, (out or b"uncommitted").strip())

    # Modified and untracked files are hashed by CONTENT, not through `git diff`: diff output
    # depends on the peer's git config (diff.noprefix, diff.algorithm, core.abbrev each produced a
    # different build_id for identical bytes). `--porcelain=v1 -z` is config-independent, and no
    # --exclude-standard, because a .gitignore'd source under src/ is still compiled by the glob
    # while being invisible to a standard-exclusion listing.
    status = git(repo, "-c", "core.quotepath=false", "status", "--porcelain=v1", "-z",
                 "--untracked-files=all", "--ignored=matching", "--", *FINGERPRINTED)
    dirty = []
    for entry in (status or b"").split(b"\x00"):
        if len(entry) > 3:
            dirty.append(entry[3:].decode("utf-8", "replace"))
    for rel in sorted(set(dirty)):
        if rel.lower().endswith(SOURCE_EXT):
            feed_file(h, "dirty:" + rel, os.path.join(repo, rel))


def palette_rev(h, repo):
    """Input 4."""
    path = os.path.join(repo, "src", "foundation", "fx_palette.h")
    if not os.path.exists(path):
        feed(h, "absent:src/foundation/fx_palette.h")     # the W1 fx lane creates it
        return
    for line in open(path, encoding="utf-8"):
        if "FX_PALETTE_REV" in line and "=" in line:
            feed(h, "FX_PALETTE_REV", line.split("=", 1)[1].strip().rstrip(";").encode())
            return
    sys.exit("fingerprint: fx_palette.h has no FX_PALETTE_REV (docs/CANON.md)")


def bytecode(h, repo):
    """Input 5."""
    manifest = os.path.join(repo, "out", "luac", "manifest.tsv")
    if not os.path.exists(manifest):
        feed(h, "absent:sim-bytecode")                    # tools/luauc arrives with the W2 lane
        return
    for line in open(manifest, encoding="utf-8"):
        if line.strip():
            feed(h, "luac", line.strip().encode())


def compute_build_id(repo, tier, rows, allow_no_git):
    h = hashlib.blake2b(digest_size=32)
    feed(h, "tier", tier.encode("utf-8"))
    tree(h, repo, allow_no_git)
    if rows is None:
        feed(h, "absent:compile_commands.json")
    else:
        for t in canonical_tokens(rows):
            feed(h, "tok", t.encode("utf-8"))
    palette_rev(h, repo)
    bytecode(h, repo)
    return h.digest()


def compute_build_env(compiler, flags, rows):
    h = hashlib.blake2b(digest_size=32)
    feed(h, "compiler", compiler.encode("utf-8"))
    feed(h, "flags", flags.encode("utf-8"))
    for f, cmd in rows or []:
        feed(h, "cc:" + f, cmd.encode("utf-8"))
    return h.digest()


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if os.path.exists(path) and open(path, encoding="utf-8", newline="").read() == text:
        return False
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    return True


def array(name, digest):
    rows = ",\n    ".join(", ".join("0x%02x" % b for b in digest[i:i + 8]) for i in range(0, 32, 8))
    return ("extern const u8 %s[32];\nconst u8 %s[32] = {\n    %s\n};\n" % (name, name, rows))


def emit(build_id, build_env, out_cpp, provenance):
    write_if_changed(out_cpp,
                     "// generated by tools/fingerprint.py - do not edit (docs/BUILD.md §5)\n"
                     "// %s\n"
                     "using u8 = unsigned char;\n\n"
                     "// Compared between peers; a mismatch ends the session (docs/NETCODE.md §15.1).\n"
                     "%s\n"
                     "// Reported, never compared: compiler, triple, flags (docs/BUILD.md §9 R-8).\n"
                     "%s" % (provenance, array("TL_BUILD_ID", build_id), array("TL_BUILD_ENV", build_env)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--tier", required=True)
    ap.add_argument("--compiler", default="")
    ap.add_argument("--flags", default="")
    ap.add_argument("--compile-commands", default=None)
    ap.add_argument("--binary-dir", default=None)
    ap.add_argument("--out-cpp", required=True)
    ap.add_argument("--out-txt", default=None)
    ap.add_argument("--out-env", default=None)
    ap.add_argument("--out-flags", default=None)
    ap.add_argument("--print", action="store_true", help="print build_id to stdout (selftest)")
    ap.add_argument("--allow-no-git", action="store_true",
                    help="permit fingerprinting a tree with no git history; the id is then "
                         "blind to source edits and is only meaningful to the selftest")
    a = ap.parse_args()

    if a.flags == "configure":
        # Configure-time placeholder: all-zero ids, written only if nothing is there yet. The build
        # step computes the real values; app/main.cpp exits non-zero on an all-zero build_id, so a
        # placeholder that survives into a binary is loud, not silent.
        if not os.path.exists(a.out_cpp):
            emit(bytes(32), bytes(32), a.out_cpp, "placeholder - the build step computes the real ids")
        return 0

    rows = load_commands(a.compile_commands, a.repo, a.binary_dir)
    build_id = compute_build_id(a.repo, a.tier, rows, a.allow_no_git)
    build_env = compute_build_env(a.compiler, a.flags, rows)

    emit(build_id, build_env, a.out_cpp, "tier=%s compiler=%s" % (a.tier, a.compiler))
    if a.out_txt:
        write_if_changed(a.out_txt, build_id.hex() + "\n")
    if a.out_env:
        write_if_changed(a.out_env, "%s\n%s\n" % (build_env.hex(), a.compiler))
    if a.out_flags:
        write_if_changed(a.out_flags, "\n".join(sorted(a.flags.split())) + "\n")
    if a.print:
        print(build_id.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
