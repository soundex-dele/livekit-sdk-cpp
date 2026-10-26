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
add_library(livekit_demo_session STATIC session.cpp)
target_include_directories(livekit_demo_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(livekit_demo_session PUBLIC LiveKit::SDK)
add_executable(livekit_conference_demo main.cpp ui.cpp video_texture.cpp)
target_link_libraries(livekit_conference_demo PRIVATE livekit_demo_session livekit_demo_imgui)
if(LIVEKIT_BUILD_TESTS)
  add_executable(livekit_demo_integration ../../tests/demo_integration.cpp)
  target_link_libraries(livekit_demo_integration PRIVATE livekit_demo_session)
  add_executable(livekit_demo_tests ../../tests/demo_tests.cpp ui.cpp video_texture.cpp)
  target_link_libraries(livekit_demo_tests PRIVATE livekit_demo_session livekit_demo_imgui)
  add_test(NAME demo_render COMMAND livekit_demo_tests)
  set_tests_properties(demo_render PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")
endif()
