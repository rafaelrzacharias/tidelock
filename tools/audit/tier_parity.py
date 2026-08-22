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


def commands(path, preset):
    if not os.path.exists(path):
        sys.exit("tier_parity: %s does not exist - configure the %s preset first" % (path, preset))
    out = {}
    for e in json.load(open(path, encoding="utf-8")):
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        rel = os.path.relpath(e["file"], e.get("directory", ".")).replace("\\", "/")
        # Tokenise the preset's own output directory so object/dep paths do not read as a delta.
        cmd = cmd.replace("\\", "/").replace("out/" + preset, "$OUT")
        out[os.path.basename(rel)] = [t for t in cmd.split() if "$OUT" not in t]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--netcode", required=True, help="out/<netcode preset>")
    ap.add_argument("--ship", required=True, help="out/<ship preset>")
    a = ap.parse_args()

    n = commands(os.path.join(a.netcode, "compile_commands.json"), os.path.basename(a.netcode))
    s = commands(os.path.join(a.ship, "compile_commands.json"), os.path.basename(a.ship))

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
