# rapidhash - header-only, vendored verbatim (docs/BUILD.md §4; pin in vendor/VERSIONS).
# INTERFACE only: every function in rapidhash.h is `inline`/`constexpr`, so there is no object
# file to build and nothing to hook to a mem_pool (the algorithm allocates nothing).
# Consumed exclusively by src/foundation/hash.cpp (docs/BUILD.md §4 "never included above its
# wrap module"); tools/audit/includes.py's BACKEND_HEADERS + SYS_ALLOW_DIRS entries for
# "rapidhash"/"src/foundation" are the enforcement.
add_library(rapidhash INTERFACE)
target_include_directories(rapidhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
