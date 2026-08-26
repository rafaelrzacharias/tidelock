# Vendored third-party libs at pinned commits (docs/BUILD.md §4). Each gets its own
# CMakeLists.txt, its own flags (the C++ subset does not apply), and its allocator hooked to a
# mem_pool (docs/MEMORY.md §1.5) from src/vendor_glue/. SDL3, SDL_ttf, Dear ImGui, ENet,
# Monocypher and stb are vendored by the W2 vendor lane; Luau by the W2 luau-vm lane.
# vendor/VERSIONS records every pin and hashes into build_id.

add_subdirectory(rapidhash)

# SDL3 ships its own full CMake project (options + platform backend detection); we vendor the
# tree verbatim (docs/BUILD.md §4) minus test/examples/docs/IDE-project directories that its own
# CMakeLists.txt never add_subdirectory()s unless SDL_TESTS/SDL_EXAMPLES/SDL_INSTALL_DOCS are ON
# (all forced OFF here), so removing them changes nothing reachable from the build. Cache
# variables are set (not passed as -D) so they take effect before add_subdirectory() evaluates
# SDL3's own option()/dep_option() defaults.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(sdl3)
