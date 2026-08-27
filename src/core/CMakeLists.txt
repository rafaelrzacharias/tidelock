# world/ECS + reflection, events, input, assets, loop/time (docs/ECS.md, FRAME-LOOP.md,
# INPUT.md, ASSETS-AND-DATA.md). Sees foundation and the platform contract headers.
tl_module(tl_core)
target_link_libraries(tl_core PUBLIC tl_foundation)
target_include_directories(tl_core PRIVATE "${CMAKE_SOURCE_DIR}/src/platform")

# tl_module's glob is "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" - flat, non-recursive (docs/LESSONS.md:
# a lane adds a flat .cpp with no CMake edit). docs/ASSETS-AND-DATA.md §8.1 puts the asset loaders
# one level down (core/loaders/{image,font}.cpp) so assets.cpp/data_tables.cpp/save.cpp stay
# readable at the module's top level; the glob cannot see a subdirectory, so those two are named
# explicitly here (W3 assets+data, the tree's first module with a .cpp subdirectory - no existing
# precedent to copy, recorded in TODO.md).
target_sources(tl_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/loaders/image.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/loaders/font.cpp"
)

# loaders/image.cpp reaches <stb_image.h> declaration-only (tools/audit/includes.py's "stb_"
# BACKEND_HEADERS token grants src/core exactly this); PRIVATE because only that one TU calls it.
target_link_libraries(tl_core PRIVATE stb)
