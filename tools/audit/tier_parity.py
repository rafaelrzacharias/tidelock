#!/usr/bin/env python3
"""netcode/ship flag parity. Spec: docs/BUILD.md §3 - "netcode and ship produce the same
fingerprint class only if their flag sets are identical except for symbol stripping".

The W0 review found that claim enforced nowhere. It is enforced here, and by comparing the
RESOLVED compile command of every TU (compile_commands.json), not the flag string a human wrote:
the two presets must differ only by the tier defines listed in ALLOWED_DELTA. Anything else -
an optimisation level, a warning flag, a sanitiser, a standard version - means peers running
`ship` and peers running `netcode` are not compiling the same program.
"""
import argparse, json, os, re, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# The documented difference between the two tiers (docs/BUILD.md §3): the tier marker and the
# NDEBUG-class stripping. Adding a row here is a ruling in docs/BUILD.md §9.
ALLOWED_DELTA = re.compile(
    r'^[-/]D(TL_TIER_(NETCODE|SHIP)=1'          # the tier marker
    r'|TL_TIER_NAME=.*'                          # the tier's display name
    r'|NDEBUG=1)$')                              # the stripping define


def commands(out_dir, preset):
    """{tokenised source path: [tokens]} for one preset's compile database.

    Keyed by full path, not basename: src/platform/platform.cpp is compiled into two libs and
    three exes share tests/stub_main.cpp, so basenames silently merged 18 entries into 14 and hid
    whatever the merged ones disagreed about. The preset's own binary directory is tokenised in
    the key as well as in the command, because generated TUs (build_id.cpp) live inside it and
    would otherwise read as "compiled in one tier and not the other"."""
    path = os.path.join(out_dir, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit("tier_parity: %s does not exist - configure the %s preset first" % (path, preset))
    out_abs = os.path.abspath(out_dir).replace("\\", "/")
    repo_abs = os.path.dirname(os.path.dirname(out_abs))

    def tokenise(s):
        s = s.replace("\\", "/").replace(out_abs, "$OUT")
        return s.replace(repo_abs, "$REPO").replace("out/" + preset, "$OUT")

    out = {}
    for e in json.load(open(path, encoding="utf-8")):
        cmd = tokenise(e.get("command") or " ".join(e.get("arguments", [])))
        out[tokenise(e["file"])] = [t for t in cmd.split() if "$OUT" not in t]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--netcode", required=True, help="out/<netcode preset>")
    ap.add_argument("--ship", required=True, help="out/<ship preset>")
    a = ap.parse_args()

    n = commands(a.netcode, os.path.basename(os.path.normpath(a.netcode)))
    s = commands(a.ship, os.path.basename(os.path.normpath(a.ship)))

    errors = []
    only = set(n) ^ set(s)
    for f in sorted(only):
        errors.append("%s is compiled in one tier and not the other" % f)

    for f in sorted(set(n) & set(s)):
        delta = [t for t in (set(n[f]) ^ set(s[f])) if not ALLOWED_DELTA.match(t)]
        for t in sorted(delta):
            errors.append("%s: netcode/ship differ by %s, which is not stripping" % (f, t))

    for e in errors:
        print("ERROR tier_parity: " + e)
    print("tier_parity: %d TUs compared, %d violations" % (len(set(n) & set(s)), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
