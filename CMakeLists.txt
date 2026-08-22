# tidelock - root build. Spec: docs/BUILD.md §10.1/§10.2.
# One static lib per module folder; tools/ is a separate project (docs/BUILD.md §2).
cmake_minimum_required(VERSION 3.25)

project(tidelock LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "tidelock builds with clang only (docs/BUILD.md §1); got ${CMAKE_CXX_COMPILER_ID}")
endif()

set(TL_TIER "dev" CACHE STRING "build tier: debug | dev | netcode | ship (docs/BUILD.md §3)")
set_property(CACHE TL_TIER PROPERTY STRINGS debug dev netcode ship)
option(TL_SANITIZE "UBSan+ASan on the sim test set (docs/TESTING.md §5)" OFF)
set(TL_PRESET_NAME "${TL_TIER}" CACHE STRING "the preset that configured this tree (audit reporting)")

# Tiers replace CMake build types (docs/BUILD.md §3): TL_TIER owns optimisation, debug info and
# defines, so no CMAKE_BUILD_TYPE flag set is allowed to fight it.
set(CMAKE_BUILD_TYPE "" CACHE STRING "unused - tidelock uses TL_TIER (docs/BUILD.md §3)" FORCE)
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")   # exceptions are off by policy

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/tier.cmake)
include(cmake/testlist.cmake)
include(cmake/fingerprint.cmake)

add_subdirectory(vendor)
add_subdirectory(src/foundation)
add_subdirectory(src/platform)
add_subdirectory(src/sim)
add_subdirectory(src/core)
add_subdirectory(src/render)
add_subdirectory(src/net)
add_subdirectory(src/script)
add_subdirectory(src/editor)
add_subdirectory(src/app)
add_subdirectory(tests)

include(cmake/audit.cmake)   # after the module targets exist: the audits name them

message(STATUS "tidelock: tier=${TL_TIER} sanitize=${TL_SANITIZE} compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
