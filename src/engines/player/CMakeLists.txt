get_filename_component(sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include(${sdk_root}/cmake/PlayerFFmpeg.cmake)
add_library(livekit_media_nodes_ffmpeg STATIC
  "${sdk_root}/src/media_nodes/demux/ffmpeg_file_io.cpp"
  "${sdk_root}/src/media_nodes/demux/ffmpeg_demuxer.cpp"
  "${sdk_root}/src/media_nodes/codecs/ffmpeg_decoder.cpp"
  "${sdk_root}/src/media_nodes/process/ffmpeg_video_converter.cpp"
  "${sdk_root}/src/media_nodes/process/ffmpeg_audio_resampler.cpp")
target_include_directories(livekit_media_nodes_ffmpeg PRIVATE "${sdk_root}/src")
target_link_libraries(livekit_media_nodes_ffmpeg PUBLIC LiveKit::MediaCore PRIVATE GraphFlow::core
  livekit_ffmpeg_avformat livekit_ffmpeg_avcodec livekit_ffmpeg_avutil livekit_ffmpeg_swscale livekit_ffmpeg_swresample)
add_library(livekit_player STATIC "${sdk_root}/src/engines/player/player.cpp" "${sdk_root}/src/engines/player/playback_graph.cpp")
add_library(LiveKit::Player ALIAS livekit_player)
target_include_directories(livekit_player PRIVATE "${sdk_root}/src" "${LIVEKIT_FFMPEG_INCLUDE}")
target_link_libraries(livekit_player PUBLIC LiveKit::MediaCore PRIVATE GraphFlow::core livekit_media_nodes_ffmpeg)

option(LIVEKIT_PLAYER_BUILD_TESTS "Build player integration tests" ${PROJECT_IS_TOP_LEVEL})
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
  target_link_libraries(livekit_playback_graph_tests PRIVATE LiveKit::Player GraphFlow::core)
  livekit_player_runtime(livekit_playback_graph_tests)
  add_test(NAME playback_graph COMMAND livekit_playback_graph_tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set_tests_properties(playback_graph PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 20)
  option(LIVEKIT_BUILD_GRAPH_RECORDING_TESTS "Verify GraphFlow with the Studio encoder (requires Python and H.264/AAC encoders)" OFF)
  if(WIN32 AND LIVEKIT_BUILD_GRAPH_RECORDING_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Exercise the real Studio codec/mux implementation without starting Studio devices or UI.
    add_executable(livekit_graph_recording_tests "${sdk_root}/tests/graph_recording_tests.cpp" "${sdk_root}/tests/graph_media_profile.cpp"
      "${sdk_root}/tests/graph_media_capacity.cpp"
      "${sdk_root}/src/engines/studio/recorder.cpp")
    target_include_directories(livekit_graph_recording_tests PRIVATE "${sdk_root}/include" "${sdk_root}/src" "${LIVEKIT_FFMPEG_INCLUDE}")
    target_compile_definitions(livekit_graph_recording_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(livekit_graph_recording_tests PRIVATE LiveKit::Player GraphFlow::json psapi livekit_ffmpeg_avformat
      livekit_ffmpeg_avcodec livekit_ffmpeg_avutil livekit_ffmpeg_swscale)
    livekit_player_runtime(livekit_graph_recording_tests)
    add_test(NAME graph_file_recording COMMAND "${Python3_EXECUTABLE}" "${sdk_root}/tests/graph_recording_verify.py"
      $<TARGET_FILE:livekit_graph_recording_tests> "${LIVEKIT_PLAYER_FFMPEG_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/recording-evidence")
    set_tests_properties(graph_file_recording PROPERTIES TIMEOUT 60)
    add_test(NAME graph_recording_interruption COMMAND "${Python3_EXECUTABLE}" "${sdk_root}/tests/graph_recording_interruption.py"
      $<TARGET_FILE:livekit_graph_recording_tests> "${LIVEKIT_PLAYER_FFMPEG_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/interruption-evidence")
    set_tests_properties(graph_recording_interruption PROPERTIES TIMEOUT 100)
    set(LIVEKIT_GRAPH_MEDIA_BUDGETS "" CACHE FILEPATH "Optional deployment limits for the 150-frame software media test")
    set(profile_args)
    if(LIVEKIT_GRAPH_MEDIA_BUDGETS)
      list(APPEND profile_args --budgets "${LIVEKIT_GRAPH_MEDIA_BUDGETS}")
    endif()
    add_test(NAME graph_media_profile COMMAND "${Python3_EXECUTABLE}" "${sdk_root}/tests/graph_media_profile_verify.py"
      $<TARGET_FILE:livekit_graph_recording_tests> "${CMAKE_CURRENT_BINARY_DIR}/media-evidence" ${profile_args})
    set_tests_properties(graph_media_profile PROPERTIES TIMEOUT 130)
    add_test(NAME graph_media_capacity COMMAND "${Python3_EXECUTABLE}" "${sdk_root}/tests/graph_media_capacity_verify.py"
      $<TARGET_FILE:livekit_graph_recording_tests> "${CMAKE_CURRENT_BINARY_DIR}/media-capacity")
    set_tests_properties(graph_media_capacity PROPERTIES TIMEOUT 100)
  endif()
  if(WIN32)
    add_executable(livekit_player_windows_io_tests "${sdk_root}/tests/player_windows_io_tests.cpp")
    target_include_directories(livekit_player_windows_io_tests PRIVATE "${sdk_root}/src")
    target_link_libraries(livekit_player_windows_io_tests PRIVATE LiveKit::Player GraphFlow::core)
    livekit_player_runtime(livekit_player_windows_io_tests)
    add_test(NAME player_windows_io COMMAND livekit_player_windows_io_tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    set_tests_properties(player_windows_io PROPERTIES FIXTURES_REQUIRED player_media TIMEOUT 20)
  endif()
  add_executable(livekit_media_flow_tests "${sdk_root}/tests/media_flow_tests.cpp")
  target_include_directories(livekit_media_flow_tests PRIVATE "${sdk_root}/src")
  target_link_libraries(livekit_media_flow_tests PRIVATE LiveKit::MediaCore GraphFlow::core)
  add_test(NAME media_flow COMMAND livekit_media_flow_tests)
  set_tests_properties(media_flow PROPERTIES TIMEOUT 10)
endif()

foreach(target livekit_media_nodes_ffmpeg livekit_player livekit_player_tests livekit_playback_graph_tests livekit_media_flow_tests livekit_player_windows_io_tests livekit_graph_recording_tests)
  if(TARGET ${target})
    livekit_without_exceptions(${target})
  endif()
endforeach()
