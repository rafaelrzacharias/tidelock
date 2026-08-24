# foundation - split into two libs (docs/BUILD.md §10.2):
#   tl_foundation_det  fx, det_math, rng, hash, containers, vmem_arena, registry, scratch, handle
#                      -> symbol-audited, compiled with tl_flags_sim
#   tl_foundation      jobs, mem_pool, fmt, interner, atomic, alloc_shim -> links det
# The split is by an explicit NON-det stem list, so anything a later lane adds defaults to the
# audited half: a file that does not belong there fails the symbol audit loudly, whereas the
# inverse default would let it escape the gate silently (docs/CPP-SUBSET.md §4).
# The non-det half of docs/BUILD.md §10.2, plus the tooling runtimes: tl_assert/log/prof/probe/
# crash all reach io, so the panic ABI of docs/CPP-SUBSET.md §9 R-3 is *defined* here and only
# *referenced* from the audited half. Left off this list they would default into tl_foundation_det
# and fail its own audit - which is what R-3 claimed was impossible.
# Stems are the .cpp FILENAME (this list is matched against, per source file, below) - docs/
# TOOLING.md §9.1's file table names the implementations log.cpp/prof.cpp/probe.cpp, not
# tl_log.cpp/tl_prof.cpp/tl_probe.cpp; only the HEADERS carry the tl_ prefix.
# arena_guard is non-det on purpose: guard_tick_end reads tl_crt_alloc_count from alloc_shim,
# an upward symbol the det audit forbids; the guard is dev-only engine-side tooling, never
# sim-called (docs/MEMORY.md §8.4, TODO.md W1 mem notes).
# `rect` has no .cpp (header-only), so this entry moves nothing between libs; it exists only so
# tools/audit/includes.py classifies foundation/rect.h as non-det - it carries Rect_f32 (f32),
# which the sim-TU float ban would otherwise reject in the one header every sim TU could reach
# through foundation/ (docs/RENDER2D.md §9.2, landed from the W1 platform lane, 2026-08-24).
set(TL_FOUNDATION_NONDET jobs mem_pool fmt interner atomic alloc_shim arena_guard rect
                         tl_assert log prof probe crash)

# RR-7 ruling (docs/CPP-SUBSET.md §1, docs/TOOLING.md §9): the tooling plane - never hashed, never
# snapshotted, never part of a world's registered arena set - is the only non-det stem set allowed
# real io and namespace-scope mutable state (a ring buffer, the crash-writer install slot). A
# strict subset of TL_FOUNDATION_NONDET above; both tools/audit/includes.py and
# tools/audit/symbols.py parse THIS line, so the exemption can only widen by editing it here, never
# by a second hand-typed copy drifting out of sync (LESSONS.md: that drift class has bitten twice).
# Adding a stem here is the ruling, not a convenience - it is read by every future det/tooling
# boundary question the same way.
set(TL_FOUNDATION_TOOLING tl_assert log prof probe crash)

file(GLOB TL_FOUNDATION_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(TL_DET_SRC "")
set(TL_HOST_SRC "")
foreach(src IN LISTS TL_FOUNDATION_SRC)
  get_filename_component(stem "${src}" NAME_WE)
  if(stem IN_LIST TL_FOUNDATION_NONDET)
    list(APPEND TL_HOST_SRC "${src}")
  else()
    list(APPEND TL_DET_SRC "${src}")
  endif()
endforeach()

add_library(tl_foundation_det STATIC ${TL_DET_SRC})
target_link_libraries(tl_foundation_det PRIVATE tl_flags_common tl_flags_sim rapidhash)

add_library(tl_foundation STATIC ${TL_HOST_SRC})
target_link_libraries(tl_foundation PRIVATE tl_flags_common PUBLIC tl_foundation_det)

tl_register_lib(tl_foundation_det TRUE)    # audited: symbol layering + writable-static
tl_register_lib(tl_foundation FALSE)       # writable-static only
