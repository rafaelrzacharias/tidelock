#!/usr/bin/env python3
"""Independent reference for the two hash families tests/foundation/hash.test.cpp and
tests/foundation/rng.test.cpp pin as known-answer vectors.
Spec: docs/DETERMINISM.md §3 (rng_for/mix64), §4 and §9.5 (tl_hash64 = rapidhash, TL_HASH_SEED).

WHY THIS FILE EXISTS. docs/DETERMINISM.md §9.5 asks for "rapidhash known-answer vectors from
upstream". Upstream (github.com/Nicoshev/rapidhash at the pin in vendor/VERSIONS) ships a
benchmark and a collision harness and NO test vectors, so the W1 rng/hash lane computed the
goldens by running the vendored header - which proves only that the code equals itself. A golden
that comes out of the implementation it guards is not a known answer, it is a screenshot.

So the vectors are re-derived here, in another language, from the ALGORITHM (rapidhash v3 with
RAPIDHASH_COMPACT + RAPIDHASH_FAST, the config pinned in src/foundation/hash.cpp) rather than from
the C header's object code. That is the strongest independence available without a second
published implementation: it cannot catch a mistake shared with the transcription, but it does
catch the ones that actually threaten us - a wrong config branch, a wrong seed, a wrong secret, a
32/64-bit or endianness slip, an unroll boundary off by one, and any future silent change to the
vendored file or its #defines.

Usage:  python tools/rapidhash_ref.py            # print the vectors both test files pin
        python tools/rapidhash_ref.py --check    # exit 1 unless they match the committed goldens
"""
import sys

M = (1 << 64) - 1

# docs/CANON.md "Ticks, hashes, fingerprints, RNG": TL_HASH_SEED = "tidelock1".
TL_HASH_SEED = 0x7469646C6F636B31

# rapidhash v3's default secret (vendor/rapidhash/rapidhash.h, `rapid_secret`).
SECRET = [0x2D358DCCAA6C78A5, 0x8BB84B93962EACC9, 0x4B33A62ED433D4A3, 0x4D5A2DA51DE1AA47,
          0xA0761D6478BD642F, 0xE7037ED1A0B428DB, 0x90ED1765281C388C, 0xAAAAAAAAAAAAAAAA]


def _mum(a, b):
    """RAPIDHASH_FAST (not PROTECTED): *A = lo, *B = hi of the 128-bit product."""
    c = a * b
    return c & M, (c >> 64) & M


def _mix(a, b):
    lo, hi = _mum(a, b)
    return lo ^ hi


def _r64(d, o):
    return int.from_bytes(d[o:o + 8], "little")     # every supported triple is little-endian


def _r32(d, o):
    return int.from_bytes(d[o:o + 4], "little")


def rapidhash(key, seed):
    """rapidhash_withSeed with RAPIDHASH_COMPACT + RAPIDHASH_FAST = tl_hash64."""
    d, n = key, len(key)
    p, i = 0, n
    seed ^= _mix(seed ^ SECRET[2], SECRET[1])
    a = b = 0
    if n <= 16:
        if n >= 4:
            seed ^= n
            if n >= 8:
                a, b = _r64(d, p), _r64(d, p + n - 8)
            else:
                a, b = _r32(d, p), _r32(d, p + n - 4)
        elif n > 0:
            a, b = ((d[p] << 45) | d[p + n - 1]) & M, d[p + (n >> 1)]
    else:
        s1 = s2 = s3 = s4 = s5 = s6 = seed
        if i > 112:                                  # the COMPACT unroll: 112 bytes per pass
            while True:
                seed = _mix(_r64(d, p) ^ SECRET[0], _r64(d, p + 8) ^ seed)
                s1 = _mix(_r64(d, p + 16) ^ SECRET[1], _r64(d, p + 24) ^ s1)
                s2 = _mix(_r64(d, p + 32) ^ SECRET[2], _r64(d, p + 40) ^ s2)
                s3 = _mix(_r64(d, p + 48) ^ SECRET[3], _r64(d, p + 56) ^ s3)
                s4 = _mix(_r64(d, p + 64) ^ SECRET[4], _r64(d, p + 72) ^ s4)
                s5 = _mix(_r64(d, p + 80) ^ SECRET[5], _r64(d, p + 88) ^ s5)
                s6 = _mix(_r64(d, p + 96) ^ SECRET[6], _r64(d, p + 104) ^ s6)
                p += 112
                i -= 112
                if i <= 112:
                    break
            seed ^= s1
            s2 ^= s3
            s4 ^= s5
            seed ^= s6
            s2 ^= s4
            seed ^= s2
        if i > 16:
            seed = _mix(_r64(d, p) ^ SECRET[2], _r64(d, p + 8) ^ seed)
            if i > 32:
                seed = _mix(_r64(d, p + 16) ^ SECRET[2], _r64(d, p + 24) ^ seed)
                if i > 48:
                    seed = _mix(_r64(d, p + 32) ^ SECRET[1], _r64(d, p + 40) ^ seed)
                    if i > 64:
                        seed = _mix(_r64(d, p + 48) ^ SECRET[1], _r64(d, p + 56) ^ seed)
                        if i > 80:
                            seed = _mix(_r64(d, p + 64) ^ SECRET[2], _r64(d, p + 72) ^ seed)
                            if i > 96:
                                seed = _mix(_r64(d, p + 80) ^ SECRET[1], _r64(d, p + 88) ^ seed)
        a, b = _r64(d, p + i - 16) ^ i, _r64(d, p + i - 8)
    a ^= SECRET[1]
    b ^= seed
    a, b = _mum(a, b)
    return _mix(a ^ SECRET[7], b ^ SECRET[1] ^ i)


# --- the other family: docs/DETERMINISM.md §3's keyed RNG ---------------------------------------

def mix64(x):
    """The splitmix64 finalizer, verbatim from docs/DETERMINISM.md §3."""
    x &= M
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & M
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & M
    x ^= x >> 31
    return x


RNG_K0 = 0x9E3779B97F4A7C15


def rng_for(seed, tick, system_id, carrier_id, draw=0):
    r = mix64(seed ^ RNG_K0)
    r = mix64((r + tick) & M)
    r = mix64((r + (((system_id & 0xFFFFFFFF) << 32) | (draw & 0xFFFFFFFF))) & M)
    return mix64((r + carrier_id) & M)


# --- the committed goldens ----------------------------------------------------------------------
# Kept in the same order as the TL_EXPECT_EQ lines they mirror, so a diff is readable.
HASH_VECTORS = [
    (b"", 0xA9289DDD02105011),
    (b"a", 0x7425490BF2D09D36),
    (b"abc", 0x4080F4B00E1442B9),
    (b"the quick brown fox jumps over the lazy dog", 0xC077819F6F45F995),
    (bytes(range(128)), 0x63D79B273D2C819C),          # crosses the COMPACT unroll boundary (>112)
]
# system_id 0 is reserved and is a precondition of rng_for (docs/DETERMINISM.md §3, ruled
# 2026-08-24), so the field-independence vectors are based on RNG_SYS_LUAU_BASE = 256 - a real
# enum id - rather than on 0. Perturbing one field at a time, then a mixed one.
LUAU_BASE = 256
RNG_VECTORS = [
    ((0, 0, LUAU_BASE, 0, 0), 0x21C24BB43807B8B5),
    ((1, 0, LUAU_BASE, 0, 0), 0x8F4E2459DFE9E176),
    ((0, 1, LUAU_BASE, 0, 0), 0x736A2770798C17E6),
    ((0, 0, LUAU_BASE + 1, 0, 0), 0x8BEC1FACBBDD35D1),
    ((0, 0, LUAU_BASE, 1, 0), 0x17C7A9967B655A3A),
    ((0, 0, LUAU_BASE, 0, 1), 0xBE29556F063A9F74),
    ((TL_HASH_SEED, 1000000, LUAU_BASE, 0xDEADBEEF, 7), 0xF2E5E37808D75849),
    ((0, 0, LUAU_BASE, 0xFFFFFFFFFFFFFFFF, 0), 0xF402F9428FCF7195),   # widest carrier_id
]


def main():
    check = "--check" in sys.argv[1:]
    bad = 0
    for key, want in HASH_VECTORS:
        got = rapidhash(key, TL_HASH_SEED)
        ok = got == want
        bad += not ok
        print("%s tl_hash64(len=%-3d) = 0x%016x%s"
              % ("OK  " if ok else "FAIL", len(key), got, "" if ok else "  want 0x%016x" % want))
    for args, want in RNG_VECTORS:
        got = rng_for(*args)
        ok = got == want
        bad += not ok
        print("%s rng_for%-44s = 0x%016x%s"
              % ("OK  " if ok else "FAIL", str(args), got, "" if ok else "  want 0x%016x" % want))
    if check and bad:
        print("rapidhash_ref: %d vector(s) disagree with the committed goldens" % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
