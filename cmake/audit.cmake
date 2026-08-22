# Static gates as build targets. Spec: docs/TESTING.md §5, docs/CPP-SUBSET.md §4/§6.
#   tl_audit_symbols   llvm-nm undefined-symbol layering + llvm-objdump .data/.bss check
#   tl_audit_includes  the source-discipline grep (firewall, module DAG, float ban, contracts)
#   tl_audit_docs      tools/docaudit/docaudit.py (also a PR gate on docs-only commits)
#   tl_rebuild_budget  full/incremental rebuild timing vs docs/BUILD.md §3
#   tl_audit           all of the above
find_program(TL_LLVM_NM NAMES llvm-nm llvm-nm-22 REQUIRED)
find_program(TL_LLVM_OBJDUMP NAMES llvm-objdump llvm-objdump-22 REQUIRED)
find_program(TL_LLVM_AR NAMES llvm-ar llvm-ar-22 REQUIRED)
find_program(TL_CLANGXX NAMES clang++ clang-cl REQUIRED)

# The audit's inputs come from the two global properties tl_register_lib fills, in
# add_subdirectory order - which is the module DAG bottom-up (docs/ARCHITECTURE.md §1). A lane
# that adds a lib is covered without editing this file; a lane that adds one and forgets to
# register it fails configure below rather than silently escaping the gate.
get_property(TL_AUDITED GLOBAL PROPERTY TL_AUDITED_LIBS)
get_property(TL_MODULES GLOBAL PROPERTY TL_MODULE_LIBS)
if(NOT TL_AUDITED)
  message(FATAL_ERROR "no audited libs registered - tl_register_lib(<target> TRUE) is missing")
endif()

set(TL_SYMBOL_ARGS "")
foreach(lib IN LISTS TL_AUDITED)
  list(APPEND TL_SYMBOL_ARGS --layer "${lib}=$<TARGET_FILE:${lib}>")
endforeach()
foreach(lib IN LISTS TL_MODULES)
  list(APPEND TL_SYMBOL_ARGS --data-only "${lib}=$<TARGET_FILE:${lib}>")
endforeach()
if(TL_SANITIZE)
  list(APPEND TL_SYMBOL_ARGS --sanitized)   # the audit then refuses, loudly, instead of lying
endif()

add_custom_target(tl_audit_symbols
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/symbols.py"
          --nm "${TL_LLVM_NM}" --objdump "${TL_LLVM_OBJDUMP}"
          --allow "${CMAKE_SOURCE_DIR}/tools/audit/allow.txt"
          ${TL_SYMBOL_ARGS}
  DEPENDS ${TL_AUDITED} ${TL_MODULES}
  COMMENT "audit: symbol layering + writable static storage"
  VERBATIM)

add_custom_target(tl_audit_includes
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/includes.py" --root "${CMAKE_SOURCE_DIR}"
  COMMENT "audit: include firewall, module DAG, float ban, header contracts"
  VERBATIM)

add_custom_target(tl_audit_docs
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/docaudit/docaudit.py"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "audit: docs"
  VERBATIM)

add_custom_target(tl_rebuild_budget
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/rebuild_budget.py" --preset "${TL_PRESET_NAME}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "audit: rebuild-time budget"
  VERBATIM)

# The gates' own negative tests (docs/TESTING.md §5). An audit nobody plants a violation
# against is a decoration; this runs the planted violations on every PR.
add_custom_target(tl_audit_selftest
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/selftest.py"
          --nm "${TL_LLVM_NM}" --objdump "${TL_LLVM_OBJDUMP}"
          --ar "${TL_LLVM_AR}" --cxx "${TL_CLANGXX}"
  COMMENT "audit: selftest (the gates' planted violations)"
  VERBATIM)

add_custom_target(tl_audit DEPENDS tl_audit_symbols tl_audit_includes tl_audit_docs tl_audit_selftest)
