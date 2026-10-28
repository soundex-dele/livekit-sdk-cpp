include(FetchContent)
FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
  URL_HASH SHA256=8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(livekit_demo_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(livekit_demo_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(livekit_demo_imgui PUBLIC SDL2::SDL2-static)
add_library(livekit_demo_session STATIC session.cpp quality.cpp connection.cpp)
target_include_directories(livekit_demo_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(livekit_demo_session PUBLIC LiveKit::SDK)
add_executable(livekit_conference_demo main.cpp ui.cpp video_texture.cpp)
target_link_libraries(livekit_conference_demo PRIVATE livekit_demo_session livekit_demo_imgui)
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set_target_properties(livekit_conference_demo PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.livekit.conference-demo"
    MACOSX_BUNDLE_BUNDLE_NAME "LiveKit Conference"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
  install(TARGETS livekit_conference_demo BUNDLE DESTINATION . COMPONENT Conference)
  # Copy non-system dylibs (OpenSSL, libvpx, dav1d) and rewrite their install
  # names so the installed app does not depend on the developer's Homebrew.
  configure_file(macos/InstallBundle.cmake.in InstallBundle.cmake @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallBundle.cmake" COMPONENT Conference)
endif()
if(LIVEKIT_BUILD_TESTS)
  add_executable(livekit_demo_dynacast_integration ../../tests/demo_dynacast_integration.cpp)
  target_link_libraries(livekit_demo_dynacast_integration PRIVATE livekit_demo_session)
  add_executable(livekit_demo_integration ../../tests/demo_integration.cpp)
  target_link_libraries(livekit_demo_integration PRIVATE livekit_demo_session)
  add_executable(livekit_demo_tests ../../tests/demo_tests.cpp ui.cpp video_texture.cpp)
  target_link_libraries(livekit_demo_tests PRIVATE livekit_demo_session livekit_demo_imgui)
  add_test(NAME demo_render COMMAND livekit_demo_tests)
  set_tests_properties(demo_render PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")
endif()
