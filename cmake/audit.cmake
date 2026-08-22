# Static gates as build targets. Spec: docs/TESTING.md §5, docs/CPP-SUBSET.md §4/§6.
#   tl_audit_symbols   llvm-nm over every audited lib vs tools/audit/allow.txt
#   tl_audit_includes  the grep rules (banned includes, float in sim TUs, static mutable, ...)
#   tl_audit_docs      tools/docaudit/docaudit.py (also a PR gate on docs-only commits)
#   tl_rebuild_budget  full/incremental rebuild timing vs docs/BUILD.md §3
#   tl_audit           all of the above
find_program(TL_LLVM_NM NAMES llvm-nm llvm-nm-22 REQUIRED)

add_custom_target(tl_audit_symbols
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/symbols.py"
          --nm "${TL_LLVM_NM}" --allow "${CMAKE_SOURCE_DIR}/tools/audit/allow.txt"
          "$<TARGET_FILE:tl_foundation_det>" "$<TARGET_FILE:tl_sim>"
  DEPENDS tl_foundation_det tl_sim
  COMMENT "audit: undefined symbols in the audited libs"
  VERBATIM)

add_custom_target(tl_audit_includes
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/includes.py" --root "${CMAKE_SOURCE_DIR}"
  COMMENT "audit: include firewall, float ban, header contracts"
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

add_custom_target(tl_audit DEPENDS tl_audit_symbols tl_audit_includes tl_audit_docs)
