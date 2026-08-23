# tools/ is its own project (docs/BUILD.md §2): offline binaries, fully exempt from the C++
# subset - they may use the STL. Configure it separately from the engine tree:
#   cmake -S tools -B out/tools -G Ninja -DCMAKE_CXX_COMPILER=clang++   (clang-cl on Windows)
#   cmake --build out/tools && out/tools/fxcheck --quick
# fingerprint, audit and rebuild_budget are Python (docs/BUILD.md §9 R-4). luauc arrives with the
# W2 luau-vm lane.
cmake_minimum_required(VERSION 3.25)
project(tidelock_tools LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# fxcheck - layers 1 + 2 of the det-math oracle (docs/FX-PALETTE.md §4.4): the exhaustive sweeps
# and the differential run against the vendored FixPointCS. Layer 3 is tools/fxcheck/oracle.py
# (mpmath). Includes the production headers from src/ and is NOT a sim TU: the include firewall
# does not scan tools/, and the panic ABI is defined inside fxcheck.cpp.
add_executable(fxcheck fxcheck/fxcheck.cpp "${CMAKE_SOURCE_DIR}/../src/foundation/det_math.cpp")
target_include_directories(fxcheck PRIVATE "${CMAKE_SOURCE_DIR}/../src" "${CMAKE_SOURCE_DIR}/fxcheck")
target_compile_definitions(fxcheck PRIVATE TL_DEV=1)
if(MSVC)
  target_compile_options(fxcheck PRIVATE /O2 /W3 /EHsc -Wno-unused-function -Wno-bitwise-op-parentheses -Wno-deprecated-declarations)   # the vendored reference is not ours to fix
else()
  target_compile_options(fxcheck PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()
