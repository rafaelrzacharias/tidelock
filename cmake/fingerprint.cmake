# build_id: BLAKE2b-256 over compiler, flag set, tree hashes, FX_PALETTE_REV, sim bytecode.
# Spec: docs/BUILD.md §5, §10.3. The script runs at configure AND at every build (a dirty tree
# must be distinguishable from a clean one) but rewrites build_id.cpp only when the value
# changes, so a stable tree never triggers a relink.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

set(TL_BUILD_ID_CPP "${CMAKE_BINARY_DIR}/generated/build_id.cpp")
set(TL_FINGERPRINT_CMD
  "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/fingerprint.py"
  --repo "${CMAKE_SOURCE_DIR}"
  --tier "${TL_TIER}"
  --compiler "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_CXX_COMPILER_TARGET}"
  --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
  --binary-dir "${CMAKE_BINARY_DIR}"
  --out-cpp "${TL_BUILD_ID_CPP}"
  --out-txt "${CMAKE_BINARY_DIR}/build_id.txt"
  --out-flags "${CMAKE_BINARY_DIR}/flags.txt")

execute_process(COMMAND ${TL_FINGERPRINT_CMD} --flags "configure" RESULT_VARIABLE tl_fp_rc)
if(NOT tl_fp_rc EQUAL 0)
  message(FATAL_ERROR "tools/fingerprint.py failed (${tl_fp_rc})")
endif()

add_custom_target(tl_fingerprint ALL
  COMMAND ${TL_FINGERPRINT_CMD}
          --flags "$<JOIN:$<TARGET_PROPERTY:tl_flags_common,INTERFACE_COMPILE_OPTIONS>, >"
  BYPRODUCTS "${TL_BUILD_ID_CPP}" "${CMAKE_BINARY_DIR}/build_id.txt"
  COMMENT "fingerprint: build_id (${TL_TIER})"
  COMMAND_EXPAND_LISTS VERBATIM)

add_library(tl_build_id STATIC "${TL_BUILD_ID_CPP}")
target_link_libraries(tl_build_id PRIVATE tl_flags_common)
add_dependencies(tl_build_id tl_fingerprint)
