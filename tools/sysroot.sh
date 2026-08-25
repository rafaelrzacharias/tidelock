#!/usr/bin/env bash
# Capture a cross-compile sysroot from a live target. Spec: docs/BUILD.md §7, ruling R-3.
# Usage: tools/sysroot.sh <host> [out-dir]     e.g. tools/sysroot.sh deck@tidelock-deck
# The tarball is stored outside git; its BLAKE2b goes into toolchain/VERSIONS.
set -euo pipefail

host="${1:?usage: sysroot.sh <host> [out-dir]}"
outdir="${2:-out/sysroot}"
name="$(echo "$host" | tr -c 'A-Za-z0-9._-' '-')"
stage="$outdir/$name"

mkdir -p "$stage"
rsync -aL --delete \
  --include='/usr/' --include='/usr/include/***' --include='/usr/lib/***' \
  --include='/lib/***' --exclude='*' \
  "$host:/" "$stage/"

tar -czf "$outdir/$name.tar.gz" -C "$stage" .
python3 - "$outdir/$name.tar.gz" <<'PY'
import hashlib, sys
h = hashlib.blake2b(digest_size=32)
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
print("%s  blake2b-256 %s" % (sys.argv[1], h.hexdigest()))
print("pin this line in toolchain/VERSIONS (docs/BUILD.md §9 R-3)")
PY
