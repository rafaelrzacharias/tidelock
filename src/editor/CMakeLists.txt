# ImGui tooling, dev tiers only (docs/TOOLING.md). netcode/ship never link it.
if(TL_TIER STREQUAL "debug" OR TL_TIER STREQUAL "dev")
  tl_module(tl_editor)
  target_link_libraries(tl_editor PUBLIC tl_core tl_render)
else()
  add_library(tl_editor INTERFACE)   # empty seam so app/ links unconditionally
endif()
