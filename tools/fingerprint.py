#!/usr/bin/env python3
"""build_id - the build fingerprint. Spec: docs/BUILD.md §5, §10.3 (tools/ is exempt from the
C++ subset, docs/CPP-SUBSET.md §0).

build_id = BLAKE2b-256 over, in this fixed order:
  1. the compiler id/version/target string CMake resolved
  2. the tier name and the tier's flag set
  3. the RESOLVED compile command line of every TU (from compile_commands.json, with absolute
     paths tokenised). This is the input that matters: it carries the compile definitions, the
     language standard, anything a CXXFLAGS environment variable injected, and the exact set of
     files being compiled - none of which the flag string or the git tree hashes can see.
  4. the content of every source file those commands name - so a git-ignored .cpp that the
     CONFIGURE_DEPENDS glob picked up is fingerprinted, not invisible
  5. the git tree hash of src/, vendor/, script/sim/, script/lib/, toolchain/VERSIONS, plus the
     hash of `git diff HEAD` and of every untracked-or-ignored source file under those paths
     (headers are not in compile_commands.json, so this is what covers them)
  6. FX_PALETTE_REV, parsed from src/foundation/fx_palette.h
  7. the precompiled sim-script bytecode manifest, in load order

Paths that do not exist yet are recorded as an explicit `absent:<path>` token - never skipped -
so the value changes the moment one appears. Emits build_id.cpp (const u8 TL_BUILD_ID[32]),
build_id.txt and flags.txt; each is rewritten only when its content changes, so a stable tree
never relinks. flags.txt is what tools/audit/tier_parity.py diffs (docs/BUILD.md §3).
"""
import argparse, hashlib, json, os, subprocess, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FINGERPRINTED = ["src", "vendor", "script/sim", "script/lib", "toolchain/VERSIONS"]
# Extensions that can reach a translation unit or the build graph. An untracked-or-ignored file
# with one of these under a fingerprinted path is hashed; anything else there cannot change what
# is compiled without also changing a compile command (which is input 3).
SOURCE_EXT = (".h", ".hpp", ".inc", ".c", ".cc", ".cpp", ".luau", ".cmake", ".txt", ".s", ".S")

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


def compile_commands(h, repo, path, binary_dir):
    """Input 3 + 4. Absolute paths are tokenised so two clones of the same tree agree."""
    if not path or not os.path.exists(path):
        feed(h, "absent:compile_commands.json")
        return
    entries = json.load(open(path, encoding="utf-8"))
    repo_n = os.path.normpath(repo).replace("\\", "/")
    bin_n = os.path.normpath(binary_dir or "").replace("\\", "/")

    def tokenise(s):
        s = s.replace("\\", "/")
        if bin_n:
            s = s.replace(bin_n, "$BUILD")
        return s.replace(repo_n, "$REPO")

    rows = []
    for e in entries:
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        rows.append((tokenise(e["file"]), tokenise(cmd)))
    for f, cmd in sorted(rows):
        feed(h, "cc:" + f, cmd.encode("utf-8"))
    for f, _cmd in sorted(set(rows)):
        real = f.replace("$REPO", repo_n).replace("$BUILD", bin_n)
        feed_file(h, "ccsrc:" + f, real)


def tree(h, repo):
    """Input 5."""
    for path in FINGERPRINTED:
        if not os.path.exists(os.path.join(repo, path)):
            feed(h, "absent:" + path)
            continue
        out = git(repo, "rev-parse", "HEAD:" + path)
        feed(h, "tree:" + path, (out or b"uncommitted").strip())
    feed(h, "diff", git(repo, "diff", "HEAD", "--", *FINGERPRINTED) or b"")
    # No --exclude-standard: a .gitignore'd source under src/ is still compiled, and .gitignore
    # itself is outside the fingerprinted paths, so ignoring ignored files was a silent bypass.
    others = git(repo, "ls-files", "--others", "--", *FINGERPRINTED)
    for rel in sorted((others or b"").decode().splitlines()):
        if rel.lower().endswith(SOURCE_EXT):
            feed_file(h, "other:" + rel, os.path.join(repo, rel))


def palette_rev(h, repo):
    """Input 6."""
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
    """Input 7."""
    manifest = os.path.join(repo, "out", "luac", "manifest.tsv")
    if not os.path.exists(manifest):
        feed(h, "absent:sim-bytecode")                    # tools/luauc arrives with the W2 lane
        return
    for line in open(manifest, encoding="utf-8"):
        if line.strip():
            feed(h, "luac", line.strip().encode())


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path) and open(path, encoding="utf-8", newline="").read() == text:
        return False
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    return True


def emit(digest, out_cpp, provenance):
    rows = ",\n    ".join(", ".join("0x%02x" % b for b in digest[i:i + 8]) for i in range(0, 32, 8))
    write_if_changed(out_cpp,
                     "// generated by tools/fingerprint.py - do not edit (docs/BUILD.md §5)\n"
                     "// %s\n"
                     "using u8 = unsigned char;\n"
                     "extern const u8 TL_BUILD_ID[32];\n"
                     "const u8 TL_BUILD_ID[32] = {\n    %s\n};\n" % (provenance, rows))


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
    ap.add_argument("--out-flags", default=None)
    a = ap.parse_args()

    if a.flags == "configure":
        # Configure-time placeholder: an all-zero id, written only if nothing is there yet. The
        # build step computes the real value; app/main.cpp exits non-zero on an all-zero id, so a
        # placeholder that survives into a binary is loud, not silent.
        if not os.path.exists(a.out_cpp):
            emit(bytes(32), a.out_cpp, "placeholder - the build step computes the real id")
        return 0

    h = hashlib.blake2b(digest_size=32)
    feed(h, "compiler", a.compiler.encode("utf-8"))
    feed(h, "tier", a.tier.encode("utf-8"))
    feed(h, "flags", a.flags.encode("utf-8"))
    compile_commands(h, a.repo, a.compile_commands, a.binary_dir)
    tree(h, a.repo)
    palette_rev(h, a.repo)
    bytecode(h, a.repo)

    digest = h.digest()
    emit(digest, a.out_cpp, "tier=%s compiler=%s" % (a.tier, a.compiler))
    if a.out_txt:
        write_if_changed(a.out_txt, digest.hex() + "\n")
    if a.out_flags:
        write_if_changed(a.out_flags, "\n".join(sorted(a.flags.split())) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
