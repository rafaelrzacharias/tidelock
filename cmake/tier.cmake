# Tier flag sets as interface targets. Spec: docs/BUILD.md §3, docs/CPP-SUBSET.md §7.
#   tl_flags_common - every TU under src/ and tests/
#   tl_flags_sim    - additionally on the audited libs (tl_foundation_det, tl_sim)
# clang-cl is a cl-compatible driver: the /-flags below are its MSVC spellings of the same
# switches; the -W and -f flags are accepted verbatim in both driver modes.

set(TL_TIERS debug dev netcode ship)
if(NOT TL_TIER IN_LIST TL_TIERS)
  message(FATAL_ERROR "TL_TIER must be one of ${TL_TIERS}; got '${TL_TIER}'")
endif()

add_library(tl_flags_common INTERFACE)
add_library(tl_flags_sim INTERFACE)

# --- warnings + language, identical in every tier (docs/CPP-SUBSET.md §7) ---------------------
# One warning set, two spellings. In cl mode `-Wall` means MSVC's /Wall, which clang maps to
# -Weverything (it fires -Wc++98-compat on every line of C++20); /W4 is the cl spelling of
# `-Wall -Wextra`. The remaining -W flags are clang spellings and work in both driver modes.
target_compile_options(tl_flags_common INTERFACE
  -Wconversion -Wsign-conversion -Wshadow -Wvla -fno-strict-aliasing)
if(MSVC)
  target_compile_options(tl_flags_common INTERFACE /W4 /WX)
else()
  target_compile_options(tl_flags_common INTERFACE -Wall -Wextra -Werror)
endif()
if(MSVC)
  # clang-cl spellings of -fno-exceptions / -fno-rtti / -fno-threadsafe-statics; the clang
  # driver rejects the GNU spellings in cl mode.
  target_compile_options(tl_flags_common INTERFACE /EHs-c- /GR- /Zc:threadSafeInit- /MD)
  target_compile_definitions(tl_flags_common INTERFACE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(tl_flags_common INTERFACE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
endif()

target_include_directories(tl_flags_common INTERFACE "${CMAKE_SOURCE_DIR}/src")

# --- per-tier optimisation, debug info, defines (docs/BUILD.md §3) ----------------------------
if(MSVC)
  set(TL_O0 /Od)
  set(TL_O1 /O1)
  set(TL_O2 /O2)
  set(TL_G  /Z7)
else()
  set(TL_O0 -O0)
  set(TL_O1 -O1)
  set(TL_O2 -O2)
  set(TL_G  -g)
endif()
set(TL_G1 -gline-tables-only)

if(TL_TIER STREQUAL "debug")
  set(TL_OPT ${TL_O0})
  set(TL_DBG ${TL_G})
  set(TL_DEFS TL_TIER_DEBUG=1 TL_DEV=1)
elseif(TL_TIER STREQUAL "dev")
  set(TL_OPT ${TL_O1})
  set(TL_DBG ${TL_G})
  set(TL_DEFS TL_TIER_DEV=1 TL_DEV=1)
elseif(TL_TIER STREQUAL "netcode")
  set(TL_OPT ${TL_O2})
  set(TL_DBG ${TL_G1})
  set(TL_DEFS TL_TIER_NETCODE=1 TL_DEV=0)
else()
  set(TL_OPT ${TL_O2})
  set(TL_DBG ${TL_G1})
  set(TL_DEFS TL_TIER_SHIP=1 TL_DEV=0 NDEBUG=1)
endif()

target_compile_options(tl_flags_common INTERFACE ${TL_OPT} ${TL_DBG})
target_compile_definitions(tl_flags_common INTERFACE ${TL_DEFS} TL_TIER_NAME="${TL_TIER}")

# netcode and ship must differ only by stripping (docs/BUILD.md §3); tools/fingerprint.py
# re-asserts this over the recorded flag string.

# --- sanitizers: the determinism CI's UB net (docs/CPP-SUBSET.md §5) --------------------------
if(TL_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "TL_SANITIZE is the ubuntu-clang lane (docs/TESTING.md §6); not clang-cl")
  endif()
  # -fno-sanitize-recover=all: UBSan RECOVERS by default - it prints the report and the process
  # continues to exit 0, so a signed overflow in the sanitizer lane was a green CI job (the W1 fx
  # review read it in the log of a passing run). A finding is a failure, or the gate is a log.
  target_compile_options(tl_flags_common INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(tl_flags_common INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

# --- the sim half: no libm builtins, no C++ standard headers ---------------------------------
target_compile_definitions(tl_flags_sim INTERFACE TL_SIM_TU=1)
target_compile_options(tl_flags_sim INTERFACE -fno-builtin)
if(MSVC)
  target_compile_options(tl_flags_sim INTERFACE /clang:-nostdinc++)
else()
  target_compile_options(tl_flags_sim INTERFACE -nostdinc++)
endif()

# tl_module(<name> LIB <target> [SIM]) - a module static lib with the tree's conventions.
# Sources are globbed (CONFIGURE_DEPENDS) so a W1+ lane adding a .cpp never edits this file
# (docs/ROADMAP.md §0 rule 2).
function(tl_module target)
  cmake_parse_arguments(A "SIM" "" "" ${ARGN})
  file(GLOB TL_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
  add_library(${target} STATIC ${TL_SRC})
  target_link_libraries(${target} PRIVATE tl_flags_common)
  if(A_SIM)
    target_link_libraries(${target} PRIVATE tl_flags_sim)
  endif()
  tl_register_lib(${target} ${A_SIM})
endfunction()

# tl_register_lib(<target> <is_sim>) - put a lib into the audit's input list. Every module lib
# calls this, including the two that cannot use tl_module() because their folder builds more than
# one target (foundation, platform). cmake/audit.cmake reads the two global properties, so a lane
# that adds a lib is audited automatically instead of silently escaping the gate - which is what
# happened while TL_AUDITED_LIBS was written and never read.
function(tl_register_lib target is_sim)
  if(is_sim)
    set_property(GLOBAL APPEND PROPERTY TL_AUDITED_LIBS ${target})
  else()
    set_property(GLOBAL APPEND PROPERTY TL_MODULE_LIBS ${target})
  endif()
endfunction()
