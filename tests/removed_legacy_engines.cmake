cmake_minimum_required(VERSION 3.24)
foreach(path src/backends/rtc src/engines/rtc src/backends/webrtc src/media_nodes/rtc)
  file(GLOB_RECURSE obsolete "${SDK_SOURCE_DIR}/${path}/*")
  if(obsolete)
    message(FATAL_ERROR "Legacy implementation remains: ${obsolete}")
  endif()
endforeach()
foreach(header engine room_engine livekit room_services track_endpoint completion)
  if(EXISTS "${SDK_SOURCE_DIR}/include/livekit/rtc/${header}.hpp")
    message(FATAL_ERROR "Legacy API remains: ${header}")
  endif()
endforeach()
file(GLOB_RECURSE sources "${SDK_SOURCE_DIR}/src/*.cpp" "${SDK_SOURCE_DIR}/src/*.hpp"
  "${SDK_SOURCE_DIR}/include/*.hpp" "${SDK_SOURCE_DIR}/tools/*.cpp"
  "${SDK_SOURCE_DIR}/examples/*.cpp" "${SDK_SOURCE_DIR}/support/*.hpp")
foreach(path IN LISTS sources)
  file(READ "${path}" source)
  if(source MATCHES "#include[^\n]*(backends/rtc/|engines/rtc/|backends/webrtc/|rtc/(engine|room_engine|room_services|livekit|completion)\\.hpp)")
    message(FATAL_ERROR "Legacy include: ${path}")
  endif()
endforeach()
file(READ "${SDK_SOURCE_DIR}/CMakeLists.txt" build)
if(build MATCHES "LiveKit::(RTCBackend|RTCEngine|RTCMediaEngine|RTCNative|RTCLiveKit|RTCSignaling|RTCServices)")
  message(FATAL_ERROR "Legacy build target remains")
endif()

# Do not silently reintroduce a legacy graph runtime via a public node, an
# example, a test fixture, or a transitive engine link in a subdirectory.
file(GLOB_RECURSE graph_consumers "${SDK_SOURCE_DIR}/include/*.hpp" "${SDK_SOURCE_DIR}/src/*.cpp"
  "${SDK_SOURCE_DIR}/src/*.hpp" "${SDK_SOURCE_DIR}/src/*.mm" "${SDK_SOURCE_DIR}/examples/*.cpp"
  "${SDK_SOURCE_DIR}/examples/*.hpp" "${SDK_SOURCE_DIR}/tests/*.cpp" "${SDK_SOURCE_DIR}/tests/*.hpp"
  "${SDK_SOURCE_DIR}/src/CMakeLists.txt" "${SDK_SOURCE_DIR}/src/*/CMakeLists.txt"
  "${SDK_SOURCE_DIR}/cmake/*.cmake")
list(APPEND graph_consumers "${SDK_SOURCE_DIR}/CMakeLists.txt")
foreach(path IN LISTS graph_consumers)
  file(READ "${path}" source)
  if(source MATCHES "graphflow::(core|engine)([^A-Za-z0-9_]|$)|#include[^\n]*(engine/graph_(engine|builder)[.]h|core/(graph_node|event_bus|event)[.]h)")
    message(FATAL_ERROR "Legacy GraphFlow dependency: ${path}")
  endif()
endforeach()
