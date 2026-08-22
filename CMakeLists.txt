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

# The toolchain pin of docs/BUILD.md §1 is only real if something compares against it. Drift is
# loud everywhere and fatal under TL_STRICT_TOOLCHAIN - which any build whose binaries other peers
# will run must set (release, soak, cross-ISA). It is not fatal by default because build_id already
# carries the compiler string: a peer built on the wrong clang cannot silently join a session, it
# is refused at the handshake (docs/NETCODE.md §15.1). See docs/BUILD.md §9 R-7.
option(TL_STRICT_TOOLCHAIN "fail configure when the compiler is not the pinned major" OFF)
file(STRINGS "${CMAKE_SOURCE_DIR}/toolchain/VERSIONS" TL_VERSIONS_LINES REGEX "^clang[ \t]+")
if(TL_VERSIONS_LINES)
  string(REGEX REPLACE "^clang[ \t]+([0-9]+)\\..*" "\\1" TL_CLANG_PIN_MAJOR "${TL_VERSIONS_LINES}")
  string(REGEX REPLACE "^([0-9]+)\\..*" "\\1" TL_CLANG_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
  if(NOT TL_CLANG_MAJOR STREQUAL TL_CLANG_PIN_MAJOR)
    set(TL_PIN_MSG "clang ${CMAKE_CXX_COMPILER_VERSION} != toolchain/VERSIONS pin (major ${TL_CLANG_PIN_MAJOR}) - docs/BUILD.md §1")
    if(TL_STRICT_TOOLCHAIN)
      message(FATAL_ERROR "${TL_PIN_MSG}; install the pinned major or move the pin as a ruling")
    endif()
    message(WARNING "${TL_PIN_MSG}; build_id differs from a pinned build, so peers will refuse it")
  endif()
endif()

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
