# Vendored third-party libs at pinned commits (docs/BUILD.md §4). Each gets its own
# CMakeLists.txt, its own flags (the C++ subset does not apply), and its allocator hooked to a
# mem_pool (docs/MEMORY.md §1.5). SDL3 + stb arrive with the W1 platform lane, Luau/ENet/
# Monocypher/imgui/SDL_ttf with the W2 luau-vm, net-p1 and vendor lanes.
# vendor/VERSIONS records every pin and hashes into build_id.

add_subdirectory(rapidhash)
