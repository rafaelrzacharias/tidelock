// hash.h's runtime half: the vendored-rapidhash wrap. Spec: docs/DETERMINISM.md §4, §9.1/§9.5.
// RAPIDHASH_COMPACT / RAPIDHASH_FAST are rapidhash's own defaults - pinned explicitly here so a
// future upstream default change cannot silently move the algorithm out from under
// TL_HASH_SEED's "pinned implementation" claim (docs/DETERMINISM.md §4).
#include "foundation/hash.h"

#define RAPIDHASH_COMPACT
#define RAPIDHASH_FAST
#include <rapidhash.h>

u64 tl_hash64(const void* data, usize len, u64 seed) {
    return rapidhash_withSeed(data, len, seed);
}
