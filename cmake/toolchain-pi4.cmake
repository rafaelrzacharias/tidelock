# Raspberry Pi 4 (aarch64) cross toolchain. Spec: docs/BUILD.md §1, §7, ruling R-3.
# TL_SYSROOT must point at the sysroot tarball's extracted root; its BLAKE2b is pinned in
# toolchain/VERSIONS. Fails loudly when it is absent - no silent host-header fallback.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED TL_SYSROOT)
  set(TL_SYSROOT "$ENV{TL_SYSROOT}")
endif()
if(TL_SYSROOT STREQUAL "" OR NOT IS_DIRECTORY "${TL_SYSROOT}")
  message(FATAL_ERROR
    "TL_SYSROOT is unset or not a directory ('${TL_SYSROOT}').\n"
    "Capture one with tools/sysroot.sh <pi-host> and pin its hash in toolchain/VERSIONS "
    "(docs/BUILD.md §9 R-3).")
endif()

set(TL_TRIPLE aarch64-linux-gnu)
find_program(CMAKE_C_COMPILER   NAMES clang   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
set(CMAKE_C_COMPILER_TARGET   ${TL_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${TL_TRIPLE})
set(CMAKE_SYSROOT "${TL_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${TL_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
