if(NOT TARGET utoolkit_logging)
  add_subdirectory("${LIVEKIT_GRAPHFLOW_ROOT}/utoolkit/logging" "utoolkit-logging" EXCLUDE_FROM_ALL)
endif()

add_library(livekit_client_logging STATIC src/backends/livekit/logging.cpp)
target_link_libraries(livekit_client_logging PRIVATE utoolkit_logging)

add_library(livekit_client STATIC
  src/backends/livekit/room.cpp
  src/backends/livekit/room_session.cpp
  src/backends/livekit/rtc_engine.cpp
  src/backends/livekit/local_participant.cpp
  src/backends/livekit/participant_services.cpp
  src/backends/livekit/track_publication.cpp
  src/backends/livekit/track.cpp)
add_library(LiveKit::Client ALIAS livekit_client)
target_compile_features(livekit_client PUBLIC cxx_std_17)
target_include_directories(livekit_client PUBLIC include PRIVATE src)
target_compile_definitions(livekit_client PRIVATE UT_LOG_TAG="LiveKit")
target_link_libraries(livekit_client PRIVATE utoolkit_logging livekit_client_logging Threads::Threads)
livekit_without_exceptions(livekit_client)
if(LIVEKIT_BUILD_RTC_NATIVE)
  find_package(ZLIB QUIET)
  if(NOT ZLIB_FOUND)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(livekit_zlib URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(livekit_zlib)
    target_include_directories(zlibstatic PUBLIC "${livekit_zlib_SOURCE_DIR}" "${livekit_zlib_BINARY_DIR}")
    set(_sdk_zlib zlibstatic)
  else()
    set(_sdk_zlib ZLIB::ZLIB)
  endif()
  target_link_libraries(livekit_client PRIVATE ${_sdk_zlib} OpenSSL::Crypto)
  target_compile_definitions(livekit_client PRIVATE LIVEKIT_CLIENT_NATIVE=1)
  target_sources(livekit_client PRIVATE
    src/backends/livekit/data/native_room_data.cpp
    src/backends/livekit/data/rpc_session.cpp
    src/backends/livekit/data/stream_session.cpp
    src/backends/livekit/data/data_track_session.cpp
    src/backends/livekit/data/encryption.cpp
    src/backends/livekit/rtc_session.cpp
    src/backends/livekit/signal_client.cpp
    src/backends/livekit/region_discovery.cpp
    src/backends/livekit/peer_transport.cpp
    src/backends/livekit/media/encoded_audio_sender.cpp
    src/backends/livekit/media/encoded_video_sender.cpp
    src/backends/livekit/media/codec_envelopes.cpp
    src/backends/livekit/media/webrtc_context.cpp)
  target_link_libraries(livekit_client PRIVATE livekit_protocol ixwebsocket
    nlohmann_json::nlohmann_json WebRTC::webrtc)
endif()
if(ANDROID)
  target_link_libraries(livekit_client PRIVATE log)
endif()
if(WIN32)
  target_compile_definitions(livekit_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(LIVEKIT_BUILD_TESTS)
  file(GLOB client_headers "${CMAKE_CURRENT_SOURCE_DIR}/include/livekit/client/*.hpp")
  set(client_header_checks "")
  foreach(header IN LISTS client_headers)
    get_filename_component(name "${header}" NAME)
    set(check "${CMAKE_CURRENT_BINARY_DIR}/client_headers/${name}.cpp")
    file(GENERATE OUTPUT "${check}" CONTENT "#include \"livekit/client/${name}\"\n")
    list(APPEND client_header_checks "${check}")
  endforeach()
  add_library(livekit_client_headers OBJECT ${client_header_checks})
  target_include_directories(livekit_client_headers PRIVATE include)
  target_compile_features(livekit_client_headers PRIVATE cxx_std_17)
  livekit_without_exceptions(livekit_client_headers)
  add_executable(livekit_client_tests tests/livekit_client/room_tests.cpp)
  add_dependencies(livekit_client_tests livekit_client_headers)
  target_include_directories(livekit_client_tests PRIVATE src)
  target_link_libraries(livekit_client_tests PRIVATE LiveKit::Client)
  livekit_without_exceptions(livekit_client_tests)
  add_test(NAME livekit_client COMMAND livekit_client_tests)
  set_tests_properties(livekit_client PROPERTIES TIMEOUT 45)
  add_test(NAME livekit_client_dependencies COMMAND ${CMAKE_COMMAND}
    "-DSDK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/livekit_client/dependencies.cmake")
  if(LIVEKIT_BUILD_RTC_NATIVE)
    add_executable(livekit_client_services_tests tests/livekit_client/services_tests.cpp)
    target_include_directories(livekit_client_services_tests PRIVATE src tests)
    target_link_libraries(livekit_client_services_tests PRIVATE LiveKit::Client livekit_protocol ${_sdk_zlib})
    livekit_without_exceptions(livekit_client_services_tests)
    add_test(NAME livekit_client_services COMMAND livekit_client_services_tests)
    set_tests_properties(livekit_client_services PROPERTIES TIMEOUT 40)
    if(TARGET LiveKit::ServerSDK)
      add_executable(livekit_client_services_integration tests/livekit_client/services_integration.cpp)
      target_include_directories(livekit_client_services_integration PRIVATE tests)
      target_link_libraries(livekit_client_services_integration PRIVATE LiveKit::Client LiveKit::ServerSDK)
      livekit_without_exceptions(livekit_client_services_integration)
      add_test(NAME livekit_client_services_integration COMMAND livekit_client_services_integration)
      set_tests_properties(livekit_client_services_integration PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()
    add_executable(livekit_client_integration tests/livekit_client/integration.cpp)
    target_include_directories(livekit_client_integration PRIVATE tests)
    target_link_libraries(livekit_client_integration PRIVATE LiveKit::Client WebRTC::webrtc)
    add_test(NAME livekit_client_integration COMMAND livekit_client_integration)
    set_tests_properties(livekit_client_integration PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
  endif()
endif()
if(LIVEKIT_BUILD_EXAMPLES AND LIVEKIT_BUILD_RTC_NATIVE)
  add_executable(livekit_basic_room_encoded examples/basic_room_encoded/main.cpp)
  target_link_libraries(livekit_basic_room_encoded PRIVATE LiveKit::Client)
endif()
