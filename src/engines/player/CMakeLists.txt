get_filename_component(sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include(${sdk_root}/cmake/PlayerFFmpeg.cmake)
add_library(livekit_media_nodes_ffmpeg STATIC
  "${sdk_root}/src/media_nodes/capture/file_source.cpp"
  "${sdk_root}/src/media_nodes/demux/ffmpeg_demuxer.cpp"
  "${sdk_root}/src/media_nodes/codecs/ffmpeg_decoder.cpp"
  "${sdk_root}/src/media_nodes/process/ffmpeg_video_converter.cpp"
  "${sdk_root}/src/media_nodes/process/ffmpeg_audio_resampler.cpp")
target_include_directories(livekit_media_nodes_ffmpeg PRIVATE "${sdk_root}/src")
target_link_libraries(livekit_media_nodes_ffmpeg PUBLIC LiveKit::MediaCore PRIVATE graphflow::core
  livekit_ffmpeg_avformat livekit_ffmpeg_avcodec livekit_ffmpeg_avutil livekit_ffmpeg_swscale livekit_ffmpeg_swresample)
add_library(livekit_player STATIC "${sdk_root}/src/engines/player/player.cpp" "${sdk_root}/src/engines/player/playback_graph.cpp")
add_library(LiveKit::Player ALIAS livekit_player)
target_include_directories(livekit_player PRIVATE "${sdk_root}/src" "${LIVEKIT_FFMPEG_INCLUDE}")
target_link_libraries(livekit_player PUBLIC LiveKit::MediaCore PRIVATE livekit_media_graph graphflow::engine livekit_media_nodes_ffmpeg)

option(LIVEKIT_PLAYER_BUILD_EXAMPLE "Build the ImGui local audio/video player" ON)
option(LIVEKIT_PLAYER_BUILD_TESTS "Build player integration tests" ${PROJECT_IS_TOP_LEVEL})
if(LIVEKIT_PLAYER_BUILD_EXAMPLE)
  # Isolate this shared SDL dependency from the RTC demo's static video-only SDL.
  set(LIVEKIT_PLAYER_SDL_ROOT "" CACHE PATH "Optional separate SDL2 development prefix")
  find_path(LIVEKIT_PLAYER_SDL_INCLUDE SDL.h HINTS "${LIVEKIT_PLAYER_SDL_ROOT}/include"
    "${LIVEKIT_FFMPEG_ROOT}/include/SDL2" PATH_SUFFIXES SDL2 REQUIRED)
  find_library(LIVEKIT_PLAYER_SDL_LIBRARY NAMES SDL2 HINTS "${LIVEKIT_PLAYER_SDL_ROOT}/lib/x64"
    "${LIVEKIT_PLAYER_SDL_ROOT}/lib" "${LIVEKIT_FFMPEG_ROOT}/lib" REQUIRED)
  include(FetchContent)
  FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
    URL_HASH SHA256=8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(livekit_player_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(livekit_player_imgui PUBLIC "${LIVEKIT_PLAYER_SDL_INCLUDE}"
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(livekit_player_imgui PUBLIC "${LIVEKIT_PLAYER_SDL_LIBRARY}")
  add_library(livekit_player_demo STATIC "${sdk_root}/examples/local_player/session.cpp" "${sdk_root}/examples/local_player/ui.cpp")
  target_include_directories(livekit_player_demo PUBLIC "${sdk_root}/examples/local_player")
  target_link_libraries(livekit_player_demo PUBLIC LiveKit::Player livekit_player_imgui)
  if(WIN32)
    target_link_libraries(livekit_player_demo PRIVATE comdlg32)
  endif()
  add_executable(livekit_local_player "${sdk_root}/examples/local_player/main.cpp")
  target_link_libraries(livekit_local_player PRIVATE livekit_player_demo utoolkit_logging)
  livekit_player_runtime(livekit_local_player)
endif()
if(LIVEKIT_PLAYER_BUILD_TESTS)
  # Also enable at the root so ctest --test-dir <build> discovers these tests.
  enable_testing()
  add_executable(livekit_player_tests "${sdk_root}/tests/player_tests.cpp")
  target_include_directories(livekit_player_tests PRIVATE "${sdk_root}/src")
  target_link_libraries(livekit_player_tests PRIVATE LiveKit::Player)
  livekit_player_runtime(livekit_player_tests)
  find_program(LIVEKIT_PLAYER_FFMPEG_EXECUTABLE ffmpeg HINTS "${LIVEKIT_FFMPEG_ROOT}/bin" REQUIRED)
  add_test(NAME player_fixtures COMMAND ${CMAKE_COMMAND}
    "-DFFMPEG=${LIVEKIT_PLAYER_FFMPEG_EXECUTABLE}" "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    -P "${sdk_root}/tests/player_fixtures.cmake")
  set_tests_properties(player_fixtures PROPERTIES FIXTURES_SETUP player_media TIMEOUT 30)
  add_test(NAME local_player COMMAND livekit_player_tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set_tests_properties(local_player PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 45)
  add_executable(livekit_playback_graph_tests "${sdk_root}/tests/playback_graph_tests.cpp")
  target_include_directories(livekit_playback_graph_tests PRIVATE "${sdk_root}/src")
  target_link_libraries(livekit_playback_graph_tests PRIVATE LiveKit::Player graphflow::engine)
  livekit_player_runtime(livekit_playback_graph_tests)
  add_test(NAME playback_graph COMMAND livekit_playback_graph_tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set_tests_properties(playback_graph PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 20)
  add_executable(livekit_media_flow_tests "${sdk_root}/tests/media_flow_tests.cpp")
  target_include_directories(livekit_media_flow_tests PRIVATE "${sdk_root}/src")
  target_link_libraries(livekit_media_flow_tests PRIVATE LiveKit::MediaCore graphflow::engine)
  add_test(NAME media_flow COMMAND livekit_media_flow_tests)
  set_tests_properties(media_flow PROPERTIES TIMEOUT 10)
  if(LIVEKIT_PLAYER_BUILD_EXAMPLE)
    add_test(NAME local_player_output COMMAND livekit_local_player "${CMAKE_CURRENT_BINARY_DIR}/fixtures/av.mp4" --auto-exit --hidden)
    set_tests_properties(local_player_output PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 15
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")
    add_test(NAME local_player_empty COMMAND livekit_local_player --hidden --frames 5)
    set_tests_properties(local_player_empty PROPERTIES TIMEOUT 15
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")
    add_executable(livekit_player_ui_tests "${sdk_root}/tests/player_ui_tests.cpp")
    target_link_libraries(livekit_player_ui_tests PRIVATE livekit_player_demo)
    livekit_player_runtime(livekit_player_ui_tests)
    add_test(NAME local_player_ui COMMAND livekit_player_ui_tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    set_tests_properties(local_player_ui PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 20
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")
  endif()
endif()
