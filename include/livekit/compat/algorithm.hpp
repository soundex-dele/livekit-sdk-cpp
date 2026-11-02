#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>
#include <vector>
#include <deque>

namespace livekit::compat {

template <class Container, class Predicate>
auto erase_if(Container& container, Predicate predicate) {
  const auto size = container.size();
  for (auto it = container.begin(); it != container.end();) {
    if (predicate(*it)) {
      it = container.erase(it);
    } else {
      ++it;
    }
  }
  return size - container.size();
}

template <class T, class Allocator, class Predicate>
auto erase_if(std::vector<T, Allocator>& container, Predicate predicate) {
  const auto size = container.size();
  container.erase(std::remove_if(container.begin(), container.end(), predicate), container.end());
  return size - container.size();
}

template <class T, class Allocator, class Predicate>
auto erase_if(std::deque<T, Allocator>& container, Predicate predicate) {
  const auto size = container.size();
  container.erase(std::remove_if(container.begin(), container.end(), predicate), container.end());
  return size - container.size();
}

template <class Left, class Right>
bool equal(const Left& left, const Right& right) {
  return std::equal(std::begin(left), std::end(left), std::begin(right), std::end(right));
}

inline bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

inline bool starts_with(std::string_view value, char prefix) {
  return !value.empty() && value.front() == prefix;
}

inline bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool ends_with(std::string_view value, char suffix) {
  return !value.empty() && value.back() == suffix;
}

inline constexpr double pi = 3.141592653589793238462643383279502884;

}  // namespace livekit::compat
