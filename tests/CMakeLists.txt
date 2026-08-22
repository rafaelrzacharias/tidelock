# Test binaries (docs/TESTING.md §9, docs/BUILD.md §10.1). tests/ obeys the C++ subset with the
# two exemptions of docs/TESTING.md §8 R-2: the generated test list, and printf-class io/clock.
file(GLOB_RECURSE TL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.test.cpp")

add_executable(tl_tests runner/main.cpp ${TL_TEST_SOURCES})
target_link_libraries(tl_tests PRIVATE tl_flags_common
  tl_foundation tl_core tl_sim tl_script tl_platform_headless)
target_include_directories(tl_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  # The runner uses fopen/system - the io exemption of docs/TESTING.md §8 R-2. The MSVC CRT
  # marks them deprecated in favour of the _s variants, which do not exist elsewhere.
  target_compile_definitions(tl_tests PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()
tl_add_test_list(tl_tests "${CMAKE_CURRENT_SOURCE_DIR}")

# The headless driver, the Gate 0 bench and the Hovel harness are their own lanes
# (docs/TESTING.md §9.2, docs/GATE0-BENCH.md, docs/NETCODE.md §19). W0 ships the targets so the
# link graph and the presets are complete from day one.
foreach(exe tl_driver tl_gate0 tl_hovel)
  add_executable(${exe} "${CMAKE_CURRENT_SOURCE_DIR}/stub_main.cpp")
  target_compile_definitions(${exe} PRIVATE TL_STUB_EXE="${exe}")
  target_link_libraries(${exe} PRIVATE tl_flags_common tl_foundation tl_core tl_sim tl_platform_headless)
endforeach()
