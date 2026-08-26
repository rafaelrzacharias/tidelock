# Luau - the script VM, vendored verbatim at the pin in vendor/VERSIONS (docs/BUILD.md §4).
# Wrap module: src/script (tl_script) - the ONLY place a Luau header or a lua_State* may appear
# (docs/ARCHITECTURE.md §1, docs/LUAU-LAYER.md §10.1); tools/audit/includes.py's BACKEND_HEADERS
# + SYS_ALLOW_DIRS entries are the enforcement, tools/audit/targets.py carries the -I.
#
# Subset vendored (docs/LUAU-LAYER.md §10.1): Common + VM (the interpreter), Ast + Compiler (the
# bytecode pipeline: tools/luauc and the dev on-load compile, §10.9). CodeGen (native codegen) is
# NOT vendored - the sim and data VMs are interpreter-only by §1, and the UI VM's NCG is the W3
# lane's to add with bind_ui.cpp; see the ruling request in TODO.md, which carries the measured
# build-time cost that decision turns on. Analysis/Config/EqSat/Require are type-checking and
# module-resolution surfaces this engine never uses (require() is manifest-driven, §10.9).
#
# The C++ subset does NOT apply to these TUs (docs/CPP-SUBSET.md §1: vendored libs are exempt in
# their own TUs), so tl_flags_common is deliberately absent: its -Werror -Wconversion set is our
# discipline, not upstream's. Exceptions ARE enabled here and stop at this boundary:
# LUA_USE_LONGJMP=1 (docs/BUILD.md §4) makes the VM unwind with longjmp rather than throw, so no
# C++ exception can cross into an extern "C" binding; the Compiler's own Luau::CompileError is
# caught inside luau_compile before it reaches us.
add_library(luau_vm STATIC)
add_library(luau_compiler STATIC)

file(GLOB LUAU_VM_SRC       CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VM/src/*.cpp")
file(GLOB LUAU_AST_SRC      CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Ast/src/*.cpp")
file(GLOB LUAU_COMPILER_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/src/*.cpp")
target_sources(luau_vm PRIVATE ${LUAU_VM_SRC})
target_sources(luau_compiler PRIVATE ${LUAU_AST_SRC} ${LUAU_COMPILER_SRC})

# Common/include carries Luau/Common.h etc.; VM/include is lua.h/lualib.h/luaconf.h; the
# Compiler's public header is luacode.h. All PUBLIC so tl_script inherits exactly these.
target_include_directories(luau_vm PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/Common/include" "${CMAKE_CURRENT_SOURCE_DIR}/VM/include")
target_include_directories(luau_compiler PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/Common/include" "${CMAKE_CURRENT_SOURCE_DIR}/Ast/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/Compiler/include")
target_link_libraries(luau_compiler PUBLIC luau_vm)   # for the shared Common headers only

# extern "C" linkage on the public API (upstream's LUAU_EXTERN_C): our bindings are C++ but the
# audit's symbol layering reads mangled names, and the C names are what docs/LUAU-LAYER.md
# §10.12's "lua_*/luau_* only in tl_script" check greps for.
target_compile_definitions(luau_vm PUBLIC LUA_USE_LONGJMP=1 LUA_API=extern\"C\")
target_compile_definitions(luau_compiler PUBLIC LUACODE_API=extern\"C\")

foreach(t luau_vm luau_compiler)
  target_compile_features(${t} PRIVATE cxx_std_17)
  if(MSVC)
    # The root CMakeLists strips /EHsc from CMAKE_CXX_FLAGS (exceptions are off by project
    # policy); the vendored Compiler needs them for Luau::CompileError, which luau_compile
    # catches internally. /GR- stays: upstream uses no RTTI.
    target_compile_options(${t} PRIVATE /EHsc /GR- /MD /W0)
  else()
    target_compile_options(${t} PRIVATE -fexceptions -fno-rtti -w)
  endif()
endforeach()
if(NOT MSVC)
  # Upstream's own flag: the VM never reads errno from libm (docs/BUILD.md §4 pin).
  target_compile_options(luau_vm PRIVATE -fno-math-errno)
endif()
