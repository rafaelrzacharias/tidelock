# Dear ImGui (docking branch) - vendored verbatim (docs/BUILD.md §4; pin in vendor/VERSIONS).
# Core only: imgui.cpp/imgui_draw.cpp/imgui_tables.cpp/imgui_widgets.cpp. imgui_demo.cpp is not
# built - the demo window has no caller yet (no speculative breadth); add it to TL_IMGUI_SRC the
# day something calls ImGui::ShowDemoWindow(). backends/ ships vendored but unbuilt: each backend
# TU (imgui_impl_sdl3.cpp, ...) is compiled by whichever lane owns that integration
# (src/platform/impl_sdl3, src/editor), not by this target - docs/BUILD.md §4's "own flags" per
# lib does not mean one flag set fits a backend TU that also needs SDL3's headers.
set(TL_IMGUI_SRC
  "${CMAKE_CURRENT_SOURCE_DIR}/imgui.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/imgui_draw.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/imgui_tables.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/imgui_widgets.cpp")
add_library(imgui STATIC ${TL_IMGUI_SRC})
target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
