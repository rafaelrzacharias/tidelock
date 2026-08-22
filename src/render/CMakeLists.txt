# render2d (docs/RENDER2D.md). Peer of net/ and editor/; reaches sim only through sim/views.h.
tl_module(tl_render)
target_link_libraries(tl_render PUBLIC tl_core)
