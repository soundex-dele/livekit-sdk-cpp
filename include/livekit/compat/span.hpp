#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace livekit::compat {

// A non-owning, dynamically sized contiguous view. The caller retains ownership.
// Keep this type identical in C++17 and newer builds so the SDK ABI is stable.
template <class T>
class span {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using iterator = T*;
  using reference = T&;
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  constexpr span() noexcept = default;

  constexpr span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

  template <class U, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
  constexpr span(T* first, U* last) noexcept
      : data_(first), size_(first == last ? 0 : static_cast<std::size_t>(last - first)) {}

  template <class U, std::size_t N,
            std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
  constexpr span(U (&data)[N]) noexcept : data_(data), size_(N) {}

  template <class U, std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
  constexpr span(span<U> other) noexcept : data_(other.data()), size_(other.size()) {}

  template <class Container, class Pointer = decltype(std::data(std::declval<Container&>())),
            std::enable_if_t<std::is_convertible_v<std::remove_pointer_t<Pointer> (*)[], T (*)[]>,
                             int> = 0>
  constexpr span(Container& data) noexcept : data_(std::data(data)), size_(std::size(data)) {}

  template <
      class Container, class Pointer = decltype(std::data(std::declval<const Container&>())),
      std::enable_if_t<std::is_const_v<T> &&
                           std::is_convertible_v<std::remove_pointer_t<Pointer> (*)[], T (*)[]>,
                       int> = 0>
  constexpr span(const Container& data) noexcept : data_(std::data(data)), size_(std::size(data)) {}

  constexpr T* data() const noexcept {
    return data_;
  }

  constexpr std::size_t size() const noexcept {
    return size_;
  }

  constexpr std::size_t size_bytes() const noexcept {
    return size_ * sizeof(T);
  }

  constexpr bool empty() const noexcept {
    return size_ == 0;
  }

  constexpr iterator begin() const noexcept {
    return data_;
  }

  constexpr iterator end() const noexcept {
    return size_ ? data_ + size_ : data_;
  }

  constexpr reference operator[](std::size_t i) const noexcept {
    assert(i < size_);
    return data_[i];
  }

  constexpr reference front() const noexcept {
    return (*this)[0];
  }

  constexpr reference back() const noexcept {
    return (*this)[size_ - 1];
  }

  constexpr span first(std::size_t count) const noexcept {
    return subspan(0, count);
  }

  constexpr span last(std::size_t count) const noexcept {
    assert(count <= size_);
    return subspan(size_ - count, count);
  }

  constexpr span subspan(std::size_t offset, std::size_t count = npos) const noexcept {
    assert(offset <= size_);
    assert(count == npos || count <= size_ - offset);
    return {offset ? data_ + offset : data_, count == npos ? size_ - offset : count};
  }

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
};

template <class Container>
span(Container&) -> span<std::remove_pointer_t<decltype(std::data(std::declval<Container&>()))>>;
template <class T, std::size_t N>
span(T (&)[N]) -> span<T>;
template <class T>
span(T*, std::size_t) -> span<T>;
template <class T>
span(T*, T*) -> span<T>;

template <class T>
span<const std::byte> as_bytes(span<T> value) noexcept {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size_bytes()};
}

}  // namespace livekit::compat
