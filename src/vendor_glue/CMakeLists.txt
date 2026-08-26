# Per-lib vendor allocator hookups (docs/MEMORY.md §8.6, docs/PLATFORM.md §9.5). Each vendored
# lib's headers are wrap-module-gated to this directory (tools/audit/includes.py BACKEND_HEADERS),
# so tl_vendor_glue links exactly the vendor archives its own TUs name - one per commit, per
# docs/ROADMAP.md §2's W2 vendor lane.
file(GLOB TL_VENDOR_GLUE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(tl_vendor_glue STATIC ${TL_VENDOR_GLUE_SRC})
# SDL3 is PUBLIC: tests/vendor_glue/*.test.cpp calls the real SDL3 API through this lib to prove
# the hookup end to end, so it needs SDL3's headers too, not just tl_vendor_glue's own.
target_link_libraries(tl_vendor_glue PRIVATE tl_flags_common tl_foundation PUBLIC SDL3::SDL3)
tl_register_lib(tl_vendor_glue FALSE)
