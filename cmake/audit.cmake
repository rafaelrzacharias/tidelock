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

# --layer order is the module DAG bottom-up: a lib may only reference symbols from itself or
# from a layer named before it (docs/ARCHITECTURE.md §1). The .data/.bss check runs on every
# src/ lib, audited or not - docs/CPP-SUBSET.md §1 bans static mutable state in all of src/.
add_custom_target(tl_audit_symbols
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/audit/symbols.py"
          --nm "${TL_LLVM_NM}" --objdump "${TL_LLVM_OBJDUMP}"
          --allow "${CMAKE_SOURCE_DIR}/tools/audit/allow.txt"
          --layer tl_foundation_det=$<TARGET_FILE:tl_foundation_det>
          --layer tl_sim=$<TARGET_FILE:tl_sim>
          --data-only tl_foundation=$<TARGET_FILE:tl_foundation>
          --data-only tl_core=$<TARGET_FILE:tl_core>
          --data-only tl_render=$<TARGET_FILE:tl_render>
          --data-only tl_net=$<TARGET_FILE:tl_net>
          --data-only tl_script=$<TARGET_FILE:tl_script>
          --data-only tl_platform_headless=$<TARGET_FILE:tl_platform_headless>
          --data-only tl_platform_sdl3=$<TARGET_FILE:tl_platform_sdl3>
  DEPENDS tl_foundation_det tl_sim tl_foundation tl_core tl_render tl_net tl_script
          tl_platform_headless tl_platform_sdl3
  COMMENT "audit: symbol layering + mutable-global sections"
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
