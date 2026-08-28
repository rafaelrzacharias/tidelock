# world/ECS + reflection, events, input, assets, loop/time (docs/ECS.md, FRAME-LOOP.md,
# INPUT.md, ASSETS-AND-DATA.md). Sees foundation and the platform contract headers.
tl_module(tl_core)
target_link_libraries(tl_core PUBLIC tl_foundation)
target_include_directories(tl_core PRIVATE "${CMAKE_SOURCE_DIR}/src/platform")

# tl_module()'s glob is "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" - flat and non-recursive (matching
# src/platform/'s impl_*/ pattern rather than tests/'s GLOB_RECURSE), so a .cpp subdirectory needs
# its own line. core/ has two: producers/ (docs/INPUT.md §9.1: Live/Script/Replay) and loaders/
# (docs/ASSETS-AND-DATA.md §8.1: image, font - one level down so assets/data_tables/save stay
# readable at the module's top level).
#
# ONE idiom for both, with a zero-check beside each (RR-32, closed by the W3 sweep's area-B
# defect B-3). These carried two different idioms - a CONFIGURE_DEPENDS glob for producers/ and a
# hand-written file list for loaders/ - which are NOT equivalent: the reviewer dropped a probe
# .cpp into each subdirectory and only producers/ compiled it. The loaders/ probe was not built,
# not mentioned, and absent from compile_commands.json: a clean, green build of a file that is not
# in the build. A referenced symbol would fail loud at link, so today's tree is correct; the
# silent case is a new loader nothing calls yet, which is exactly what §8.5 has scheduled for
# asset_load_font's real body. The FATAL_ERROR is the other half - "a selection that matches
# nothing must be an error, not a clean run" (LESSONS.md, four occurrences of that class).
foreach(_tl_core_subdir producers loaders)
    file(GLOB _tl_core_subdir_srcs CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/${_tl_core_subdir}/*.cpp")
    if(NOT _tl_core_subdir_srcs)
        message(FATAL_ERROR
                "src/core/${_tl_core_subdir}/ matched no .cpp - a glob that selects nothing must "
                "fail, not build clean (LESSONS.md).")
    endif()
    target_sources(tl_core PRIVATE ${_tl_core_subdir_srcs})
endforeach()

# loaders/image.cpp reaches <stb_image.h> declaration-only (tools/audit/includes.py's "stb_"
# BACKEND_HEADERS token grants src/core exactly this); PRIVATE because only that one TU calls it.
target_link_libraries(tl_core PRIVATE stb)
