#!/usr/bin/env python3
"""Verify a downloaded sysroot tarball against the hash pinned in toolchain/VERSIONS.
Spec: docs/BUILD.md §9 R-3. Fails loudly - an unpinned or mismatched sysroot silently changes
what every cross-built binary was compiled against.
"""
import argparse, hashlib, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def pinned(versions, key):
    for line in open(versions, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == key:
            return parts[1] if len(parts) > 1 else ""
    sys.exit("sysroot_hash: %s has no '%s' row" % (versions, key))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarball", required=True)
    ap.add_argument("--versions", required=True)
    ap.add_argument("--key", required=True)
    a = ap.parse_args()

    want = pinned(a.versions, a.key)
    if want in ("", "unset"):
        sys.exit("sysroot_hash: %s is not pinned yet in %s (docs/BUILD.md §9 R-3)" % (a.key, a.versions))

    h = hashlib.blake2b(digest_size=32)
    with open(a.tarball, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got != want:
        sys.exit("sysroot_hash: %s is %s, %s pins %s" % (a.tarball, got, a.versions, want))
    print("sysroot_hash: %s matches the pin" % a.key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
