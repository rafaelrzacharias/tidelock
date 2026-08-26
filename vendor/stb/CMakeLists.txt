# stb_image.h + stb_sprintf.h - vendored verbatim (docs/BUILD.md §4; pin in vendor/VERSIONS).
# Only these two of the ~20 stb_*.h headers in the upstream repo are pulled in - the ones
# docs/BUILD.md §4 names; the rest have no consumer and are not vendored speculatively.
#
# stb_impl.c (ours, not upstream's - stb ships no .c of its own) is the ONE TU that defines
# STB_IMAGE_IMPLEMENTATION/STB_SPRINTF_IMPLEMENTATION (docs/BUILD.md §10.1 "stb (one TU)"). It
# compiles here, as vendor code with vendor's own (unrestricted) flags - NOT in src/vendor_glue/,
# because the header bodies were never written to satisfy -Wsign-conversion/-Werror and fail to
# compile under tl_flags_common. tl_stbi_malloc/realloc/free (docs/MEMORY.md §8.6's pool_vendor
# hookup) are declared here with C linkage and DEFINED in src/vendor_glue/stb_glue.cpp.
add_library(stb STATIC "${CMAKE_CURRENT_SOURCE_DIR}/stb_impl.c")
target_include_directories(stb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
