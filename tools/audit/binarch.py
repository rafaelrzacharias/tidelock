#!/usr/bin/env python3
"""Binary-architecture gate - the built binary's machine field must match the ISA the leg claims.
Spec: docs/BUILD.md §10.4 (the CI matrix runs host-native presets with no --target).

The win/linux presets build whatever the host toolchain is. On an arm64 runner, an x64 toolchain
running under emulation quietly produces x86-64 binaries: every test goes green while the leg
tests the wrong ISA - a silent fallback, the one failure mode a gate must not have. This reads
the PE or ELF machine field directly (no `file`, which Windows runners lack) and fails loudly on
a mismatch. Exempt from the C++ subset (tools/).
"""
import argparse
import struct
import sys

ELF_MACHINE = {0x3E: "x86_64", 0xB7: "aarch64"}
PE_MACHINE = {0x8664: "x86_64", 0xAA64: "aarch64"}


def machine(path):
    with open(path, "rb") as f:
        head = f.read(4096)
    if head[:4] == b"\x7fELF" and len(head) >= 0x14:
        return ELF_MACHINE.get(struct.unpack_from("<H", head, 0x12)[0], "unrecognized-elf-machine")
    if head[:2] == b"MZ" and len(head) >= 0x40:
        off = struct.unpack_from("<I", head, 0x3C)[0]
        if off + 6 <= len(head) and head[off:off + 4] == b"PE\0\0":
            return PE_MACHINE.get(struct.unpack_from("<H", head, off + 4)[0], "unrecognized-pe-machine")
        return "pe-header-out-of-range"
    return "not-elf-or-pe"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", required=True, help="binary to inspect")
    ap.add_argument("--expect", required=True, choices=sorted(set(ELF_MACHINE.values())),
                    help="ISA this leg claims to test")
    a = ap.parse_args()
    got = machine(a.file)
    print(f"binarch: {a.file}: {got} (expected {a.expect})")
    if got != a.expect:
        print("binarch: MISMATCH - this leg built for a different ISA than it claims to test")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
