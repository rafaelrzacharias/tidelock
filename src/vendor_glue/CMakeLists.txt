# vendor_glue - the per-library allocator adaptors (docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5).
# The ONE folder outside mem_pool.cpp allowed to call pool_* (tools/audit/includes.py enforces
# it); each vendored lib's hook API is routed here so no vendor heap reaches the CRT. Each lib's
# headers are wrap-module-gated to this directory (tools/audit/includes.py BACKEND_HEADERS), so
# tl_vendor_glue links exactly the vendor archives its own TUs name. Owners are per-lane:
# luau_*/vendor_new.* are the W2 luau-vm lane's, the imgui/sdl/enet/stb/freetype adaptors the
# W2 vendor lane's.
tl_module(tl_vendor_glue)
# SDL3/SDL3_ttf are PUBLIC: tests/vendor_glue/*.test.cpp calls their real API through this lib to
# prove each hookup end to end, so it needs their headers too, not just tl_vendor_glue's own.
# SDL_ttf has no adaptor .cpp of its own for its own 4 SDL_malloc/6 SDL_free call sites -
# docs/PLATFORM.md §9.5: it inherits whatever allocator sdl3_glue installed on SDL3. The font
# memory itself is FreeType's, hooked separately by freetype_glue.cpp (below).
target_link_libraries(tl_vendor_glue PRIVATE stb
  PUBLIC tl_foundation SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui enet monocypher)

# freetype's own ftsystem.c (patched to call this lib's tl_freetype_alloc/realloc/free - see
# freetype_glue.cpp) makes tl_vendor_glue a genuine, if unusual, dependency of SDL_ttf's static
# build - but NOT of the "freetype" CMake target itself: vendor/sdl_ttf/CMakeLists.txt statically
# links freetype by pulling its OBJECT files straight into SDL3_ttf-static
# ($<TARGET_OBJECTS:Freetype::Freetype>, its own "static build" branch), so freetype's own
# target_link_libraries never reaches the final link - the real consumer of tl_freetype_alloc at
# link time is SDL3_ttf-static's own archive (confirmed: libSDL3_ttf.a(ftsystem.c.o), no separate
# libfreetype.a ever appears on the link line). Declaring the dependency on "freetype" instead had
# no effect (verified: link line unchanged, N1 still reproduced under ld.bfd).
#
# This is a real cycle between two STATIC libraries (tl_vendor_glue's existing PUBLIC dependency
# on SDL3_ttf, and now SDL3_ttf-static needing tl_vendor_glue's symbols back), which CMake supports
# and resolves by repeating both archives on the link line as needed (target_link_libraries(3)
# "mutually-dependent STATIC libraries") - order-independent, unlike relying on LLD's archive-order
# tolerance (review round 2, N1: linking without this declared, libSDL3_ttf.a(ftsystem.c.o) has an
# undefined reference to tl_freetype_alloc that a non-order-tolerant linker cannot resolve, because
# nothing told CMake tl_vendor_glue's archive must appear again after SDL3_ttf-static's).
#
# $<BUILD_INTERFACE:...>: defensive, not load-bearing here (review round 3 nit - verified a
# plain PRIVATE link configures clean too). SDL_ttf's install(EXPORT SDL3_ttfTargets ...) sits
# behind SDLTTF_INSTALL, and vendor/CMakeLists.txt forces that OFF, so there is no export set for
# tl_vendor_glue to offend in this tree. The generator expression costs nothing and survives a
# future flip of that option, so it stays; "SDL3_ttf-static" is hardcoded because
# vendor/CMakeLists.txt also forces BUILD_SHARED_LIBS OFF - upstream would otherwise name this
# target SDL3_ttf-shared, and a mismatch here would be a hard configure error, not a silent miss.
target_link_libraries(SDL3_ttf-static PRIVATE $<BUILD_INTERFACE:tl_vendor_glue>)
