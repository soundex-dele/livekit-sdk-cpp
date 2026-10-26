# The SDK's public API uses C++20 stop_token/jthread. Xcode 16's libc++ does
# not expose them; use Homebrew LLVM and its matching libc++ consistently.
set(LIVEKIT_LLVM_ROOT "$ENV{LIVEKIT_LLVM_ROOT}" CACHE PATH "LLVM prefix with C++20 libc++")
if(NOT LIVEKIT_LLVM_ROOT)
  find_program(_livekit_clang NAMES clang++
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH REQUIRED)
  get_filename_component(_llvm_bin "${_livekit_clang}" DIRECTORY)
  get_filename_component(LIVEKIT_LLVM_ROOT "${_llvm_bin}" DIRECTORY)
endif()
foreach(_language C OBJC)
  set(CMAKE_${_language}_COMPILER "${LIVEKIT_LLVM_ROOT}/bin/clang" CACHE FILEPATH "" FORCE)
endforeach()
foreach(_language CXX OBJCXX)
  set(CMAKE_${_language}_COMPILER "${LIVEKIT_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "" FORCE)
endforeach()
if(NOT EXISTS "${LIVEKIT_LLVM_ROOT}/lib/c++/libc++.dylib")
  message(FATAL_ERROR "Install Homebrew llvm or set LIVEKIT_LLVM_ROOT to an LLVM prefix with libc++")
endif()
# Also propagate the selected prefix to CMake compiler-feature try-compiles.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LIVEKIT_LLVM_ROOT)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L\"${LIVEKIT_LLVM_ROOT}/lib/c++\" -Wl,-rpath,\"${LIVEKIT_LLVM_ROOT}/lib/c++\"")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
