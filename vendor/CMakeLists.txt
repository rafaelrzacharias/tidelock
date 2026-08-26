# Vendored third-party libs at pinned commits (docs/BUILD.md §4). Each gets its own
# CMakeLists.txt, its own flags (the C++ subset does not apply), and its allocator hooked to a
# mem_pool (docs/MEMORY.md §1.5) from src/vendor_glue/. SDL3, SDL_ttf, Dear ImGui, ENet,
# Monocypher and stb are vendored by the W2 vendor lane; Luau by the W2 luau-vm lane.
# vendor/VERSIONS records every pin and hashes into build_id.

# CMake >= 4.0 hard-errors configuring any tree whose cmake_minimum_required() floor is below
# 3.5 (compatibility with older policies was removed outright, not just deprecated). ENet's is
# `2.8.12...3.20` and FreeType's (vendor/sdl_ttf/external/freetype) is `3.0...3.10` - both below
# the cutoff, invisible on this container's older local CMake. Set once, globally, before any
# add_subdirectory() below (a CACHE variable, so it also reaches FreeType's nested
# add_subdirectory() inside SDL_ttf's own CMakeLists.txt): CMake's own documented migration knob
# for exactly this case, so the vendored cmake_minimum_required() lines stay untouched
# (docs/BUILD.md §4 "vendored verbatim") rather than hand-patched.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

add_subdirectory(luau)
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
# SDL3's CMakeLists.txt unconditionally writes an in-BUILD-tree SDL3Config.cmake (not gated on
# SDL_INSTALL, which we force off above) - pointing SDL3_DIR at it is what lets SDL_ttf's
# find_package(SDL3 REQUIRED) below resolve to this in-tree build instead of failing configure.
set(SDL3_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdl3" CACHE PATH "" FORCE)

# SDL_ttf's own CMakeLists.txt add_subdirectory()s external/freetype ITSELF (with EXCLUDE_FROM_ALL
# and its own FT_DISABLE_* forcing) whenever SDLTTF_VENDORED is ON - do not add it a second time
# here, CMake refuses a directory added twice. HarfBuzz (text shaping) and plutosvg/plutovg
# (colour-emoji glyphs) are left OFF independently of SDLTTF_VENDORED: optional per SDL_ttf's own
# CMakeLists.txt, and nothing in this tree consumes shaped or colour text yet - vendoring three
# more trees (plus FreeType's own zlib/libpng/bzip2/brotli codecs those pull in) for zero current
# consumers is exactly the speculative breadth CLAUDE.md's design-phase rules ask to challenge. A
# future text-rendering lane files a ruling to turn them back on when a real consumer needs them.
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)      # FreeType only - HarfBuzz/plutosvg forced off below
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(sdl_ttf)

add_subdirectory(imgui)

add_subdirectory(enet)
# ENet's own CMakeLists.txt uses directory-scoped include_directories(), which does not propagate
# to a consumer outside that directory (unlike SDL3/SDL_ttf, which export modern PUBLIC/INTERFACE
# target properties) - added here so enet_glue.cpp's #include <enet/enet.h> resolves.
target_include_directories(enet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/enet/include")
# ENet's own CMakeLists.txt links ws2_32/winmm only `if (MINGW)` - win32.c's WSAStartup/socket/
# timeGetTime calls are just as undefined on the clang-cl (MSVC-ABI) builds this tree uses
# (docs/BUILD.md §1), so the same libs are added here for the non-MinGW WIN32 case.
if(WIN32 AND NOT MINGW)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

add_subdirectory(monocypher)

add_subdirectory(stb)
