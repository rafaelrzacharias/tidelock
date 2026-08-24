# The porting seam (docs/PLATFORM.md): one contract, two impls. Both link foundation only;
# neither is reachable from sim/ or foundation/ (docs/ARCHITECTURE.md §1 rule 2).
file(GLOB TL_PLATFORM_COMMON CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# os_win_vmem.cpp / os_posix_vmem.cpp: "one compiled per OS" (docs/PLATFORM.md §9.1) - the glob
# above picks up both, so drop whichever one this target isn't building for.
if(WIN32)
  list(REMOVE_ITEM TL_PLATFORM_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/os_posix_vmem.cpp")
else()
  list(REMOVE_ITEM TL_PLATFORM_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/os_win_vmem.cpp")
endif()
file(GLOB TL_PLATFORM_HEADLESS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/impl_headless/*.cpp")
file(GLOB TL_PLATFORM_SDL3 CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/impl_sdl3/*.cpp")

add_library(tl_platform_headless STATIC ${TL_PLATFORM_COMMON} ${TL_PLATFORM_HEADLESS})
target_link_libraries(tl_platform_headless PRIVATE tl_flags_common PUBLIC tl_foundation)

add_library(tl_platform_sdl3 STATIC ${TL_PLATFORM_COMMON} ${TL_PLATFORM_SDL3})
target_link_libraries(tl_platform_sdl3 PRIVATE tl_flags_common PUBLIC tl_foundation)
tl_register_lib(tl_platform_headless FALSE)
tl_register_lib(tl_platform_sdl3 FALSE)

if(WIN32)
  # os_entropy.cpp: BCryptGenRandom (docs/PLATFORM.md §9.3 "entropy").
  target_link_libraries(tl_platform_headless PRIVATE bcrypt)
  target_link_libraries(tl_platform_sdl3 PRIVATE bcrypt)
endif()

if(MSVC)
  # os_file_atomic.cpp's dev-only test hook calls getenv(); the MSVC CRT marks it deprecated in
  # favour of _dupenv_s, which does not exist elsewhere (same exemption as tests/CMakeLists.txt).
  target_compile_definitions(tl_platform_headless PRIVATE _CRT_SECURE_NO_WARNINGS=1)
  target_compile_definitions(tl_platform_sdl3 PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()

# SDL3 is vendored by the W1 platform lane; vendor/CMakeLists.txt then links it here.
