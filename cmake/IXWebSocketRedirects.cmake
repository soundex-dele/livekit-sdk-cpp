# IXWebSocket 11.4.6 follows HTTP redirects with the original Authorization
# header and has no public redirect policy hook. Reject HTTP redirects in the
# SDK's private dependency; LiveKit JoinResponse alternatives are validated by
# SignalSession before connecting. Do not modify downloaded upstream sources.
set(_ix_transport "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXWebSocketTransport.cpp")
file(READ "${_ix_transport}" _ix_source)
set(_ix_redirect "if (result.http_status >= 300 && result.http_status < 400)\n            {")
string(FIND "${_ix_source}" "${_ix_redirect}" _ix_redirect_offset)
if(_ix_redirect_offset EQUAL -1)
  message(FATAL_ERROR "IXWebSocket redirect guard patch no longer matches upstream")
endif()
string(REPLACE "${_ix_redirect}" "${_ix_redirect}\n                result.errorStr = \"HTTP signaling redirects are disabled\";\n                return result;" _ix_patched "${_ix_source}")
set(_ix_output "${CMAKE_CURRENT_BINARY_DIR}/patched/IXWebSocketTransport.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/patched")
# configure_file COPYONLY avoids recompilation when the patch is unchanged.
file(WRITE "${_ix_output}.in" "${_ix_patched}")
configure_file("${_ix_output}.in" "${_ix_output}" COPYONLY)
get_target_property(_ix_sources ixwebsocket SOURCES)
list(FILTER _ix_sources EXCLUDE REGEX "(^|/)IXWebSocketTransport\\.cpp$")
set_property(TARGET ixwebsocket PROPERTY SOURCES "${_ix_sources}")
target_sources(ixwebsocket PRIVATE "${_ix_output}")
target_include_directories(ixwebsocket PRIVATE "${ixwebsocket_SOURCE_DIR}/ixwebsocket")
