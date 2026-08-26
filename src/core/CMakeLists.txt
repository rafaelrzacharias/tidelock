# world/ECS + reflection, events, input, assets, loop/time (docs/ECS.md, FRAME-LOOP.md,
# INPUT.md, ASSETS-AND-DATA.md). Sees foundation and the platform contract headers.
tl_module(tl_core)
target_link_libraries(tl_core PUBLIC tl_foundation)
target_include_directories(tl_core PRIVATE "${CMAKE_SOURCE_DIR}/src/platform")

# core/producers/ (docs/INPUT.md §9.1: Live/Script/Replay) is a subdirectory - tl_module()'s glob
# is CMAKE_CURRENT_SOURCE_DIR/*.cpp only (non-recursive, matching src/platform/'s impl_*/ pattern
# rather than tests/'s GLOB_RECURSE), so it needs its own line.
file(GLOB TL_CORE_PRODUCERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/producers/*.cpp")
target_sources(tl_core PRIVATE ${TL_CORE_PRODUCERS})
