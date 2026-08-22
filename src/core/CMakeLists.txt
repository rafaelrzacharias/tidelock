# world/ECS + reflection, events, input, assets, loop/time (docs/ECS.md, FRAME-LOOP.md,
# INPUT.md, ASSETS-AND-DATA.md). Sees foundation and the platform contract headers.
tl_module(tl_core)
target_link_libraries(tl_core PUBLIC tl_foundation)
target_include_directories(tl_core PRIVATE "${CMAKE_SOURCE_DIR}/src/platform")
