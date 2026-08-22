# The porting seam (docs/PLATFORM.md): one contract, two impls. Both link foundation only;
# neither is reachable from sim/ or foundation/ (docs/ARCHITECTURE.md §1 rule 2).
file(GLOB TL_PLATFORM_COMMON CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB TL_PLATFORM_HEADLESS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/impl_headless/*.cpp")
file(GLOB TL_PLATFORM_SDL3 CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/impl_sdl3/*.cpp")

add_library(tl_platform_headless STATIC ${TL_PLATFORM_COMMON} ${TL_PLATFORM_HEADLESS})
target_link_libraries(tl_platform_headless PRIVATE tl_flags_common PUBLIC tl_foundation)

add_library(tl_platform_sdl3 STATIC ${TL_PLATFORM_COMMON} ${TL_PLATFORM_SDL3})
target_link_libraries(tl_platform_sdl3 PRIVATE tl_flags_common PUBLIC tl_foundation)
# SDL3 is vendored by the W1 platform lane; vendor/CMakeLists.txt then links it here.
