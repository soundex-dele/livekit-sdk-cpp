#pragma once
#include "livekit/result.hpp"
#include <cstdlib>
#include <iostream>
#include <utility>

template <class T>
T media_result(livekit::sdk::Result<T> result) {
  if (!result) {
    std::cerr << result.error().message << '\n';
    std::abort();
  }
  return std::move(*result);
}

inline void media_result(livekit::sdk::Result<void> result) {
  if (!result) {
    std::cerr << result.error().message << "\n";
    std::abort();
  }
}
