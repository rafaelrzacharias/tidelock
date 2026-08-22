# The game host: main() + the wiring file (docs/ARCHITECTURE.md §1). app/ sees everything.
add_executable(tidelock main.cpp)
target_link_libraries(tidelock PRIVATE tl_flags_common
  tl_build_id tl_core tl_sim tl_render tl_net tl_script tl_editor tl_platform_sdl3)
