// Monocypher allocates nothing (docs/PLATFORM.md §9.5) - no adaptor .cpp, no pool. This proves
// the archive links and that BLAKE2b, crypto_verify32 and the optional Ed25519 module
// (docs/NETCODE.md §8/§19.9: identity signing, handoffs) all produce real, checkable output.
// Rubric: docs/TESTING.md §7.
#include "runner/tl_test.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>
#include <string.h>

TL_TEST(monocypher_blake2b_matches_a_known_vector, "vendor_glue,monocypher,smoke") {
    // BLAKE2b-512 of the empty input, RFC 7693 test vector.
    static const u8 expect[64] = {
        0x78, 0x6a, 0x02, 0xf7, 0x42, 0x01, 0x59, 0x03, 0xc6, 0xc6, 0xfd, 0x85, 0x25, 0x52, 0xd2, 0x72,
        0x91, 0x2f, 0x47, 0x40, 0xe1, 0x58, 0x47, 0x61, 0x8a, 0x86, 0xe2, 0x17, 0xf7, 0x1f, 0x54, 0x19,
        0xd2, 0x5e, 0x10, 0x31, 0xaf, 0xee, 0x58, 0x53, 0x13, 0x89, 0x64, 0x44, 0x93, 0x4e, 0xb0, 0x4b,
        0x90, 0x3a, 0x68, 0x5b, 0x14, 0x48, 0xb7, 0x55, 0xd5, 0x6f, 0x70, 0x1a, 0xfe, 0x9b, 0xe2, 0xce};
    u8 hash[64];
    crypto_blake2b(hash, sizeof hash, nullptr, 0);
    TL_EXPECT_TRUE(memcmp(hash, expect, sizeof hash) == 0);
}

TL_TEST(monocypher_crypto_verify32_matches_and_differs, "vendor_glue,monocypher,smoke") {
    u8 a[32]; u8 b[32];
    memset(a, 0xAB, sizeof a);
    memset(b, 0xAB, sizeof b);
    TL_EXPECT_TRUE(crypto_verify32(a, b) == 0);
    b[31] ^= 1u;
    TL_EXPECT_TRUE(crypto_verify32(a, b) != 0);
}

TL_TEST(monocypher_ed25519_sign_check_round_trips, "vendor_glue,monocypher,smoke") {
    u8 seed[32]; memset(seed, 0x11, sizeof seed);
    u8 secret_key[64]; u8 public_key[32];
    crypto_ed25519_key_pair(secret_key, public_key, seed);

    const u8 message[] = "tidelock handoff";
    u8 signature[64];
    crypto_ed25519_sign(signature, secret_key, message, sizeof message);
    TL_EXPECT_TRUE(crypto_ed25519_check(signature, public_key, message, sizeof message) == 0);

    signature[0] ^= 1u;
    TL_EXPECT_TRUE(crypto_ed25519_check(signature, public_key, message, sizeof message) != 0);
}
