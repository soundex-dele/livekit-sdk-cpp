#pragma once
#include "livekit/result.hpp"
#include <cstdlib>
#include <iostream>
#include <type_traits>

template <class T>
T server_result(livekit::sdk::Result<T> result) {
  if (!result) {
    std::cerr << result.error().message << '\n';
    std::abort();
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}
