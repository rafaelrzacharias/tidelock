# vendor_glue - the per-library allocator adaptors (docs/MEMORY.md §8.6). The ONE folder outside
# mem_pool.cpp allowed to call pool_* (tools/audit/includes.py enforces it); each vendored lib's
# hook API is routed here so no vendor heap reaches the CRT. Sees foundation only.
# Owners are per-lane: luau_* is the W2 luau-vm lane's, the imgui/sdl/enet/stb adaptors the W2
# vendor lane's.
tl_module(tl_vendor_glue)
target_link_libraries(tl_vendor_glue PUBLIC tl_foundation)
