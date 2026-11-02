#pragma once
#include "livekit/result.hpp"
#include <cstdlib>
#include <iostream>
#include <utility>

template <class T>
T service_value(livekit::sdk::Result<T> result) {
  if (!result) {
    std::cerr << "Unexpected service failure: " << result.error().message << '\n';
    std::abort();
  }
  return std::move(*result);
}

inline void service_value(livekit::sdk::Result<void> result) {
  if (!result) {
    std::cerr << "Unexpected service failure: " << result.error().message << '\n';
    std::abort();
  }
}
