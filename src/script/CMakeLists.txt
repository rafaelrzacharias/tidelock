# The Luau boundary (docs/LUAU-LAYER.md §10.1). The ONE module that may see a Luau header or a
# lua_State* (docs/ARCHITECTURE.md §1); tools/audit/includes.py's BACKEND_HEADERS entry is the
# gate. Luau itself is vendored at the pin in vendor/VERSIONS.
tl_module(tl_script)
target_link_libraries(tl_script PUBLIC tl_core)
target_link_libraries(tl_script PRIVATE luau_vm luau_compiler tl_vendor_glue)
