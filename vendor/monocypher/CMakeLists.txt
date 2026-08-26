# Monocypher - vendored verbatim (docs/BUILD.md §4; pin in vendor/VERSIONS). No upstream CMake
# build (a plain makefile) - this file is ours, mirroring vendor/rapidhash/CMakeLists.txt's
# convention for a lib with no build system of its own. Core (monocypher.c: X25519, ChaCha20,
# Poly1305, BLAKE2b, Argon2) plus the optional Ed25519 module docs/NETCODE.md §8/§19.9 names by
# name (identity signing, handoffs) - not every "optional/" file, only the one the spec calls for.
# Allocates nothing (docs/PLATFORM.md §9.5): no vendor_glue adaptor, no mem_pool hookup.
add_library(monocypher STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/monocypher.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(monocypher PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/optional")
