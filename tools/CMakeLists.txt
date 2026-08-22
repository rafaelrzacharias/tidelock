# tools/ is its own project (docs/BUILD.md §2): offline binaries, fully exempt from the C++
# subset - they may use the STL. Configure it separately from the engine tree:
#   cmake -S tools -B out/tools && cmake --build out/tools
# W0 ships no C++ tool: fingerprint, audit and rebuild_budget are Python (docs/BUILD.md §9 R-4).
# luauc arrives with the W2 luau-vm lane, fxcheck with the W1 fx lane.
cmake_minimum_required(VERSION 3.25)
project(tidelock_tools LANGUAGES CXX)
message(STATUS "tidelock tools: nothing to build yet (docs/BUILD.md §10.3)")
