#pragma once

#include "livekit/error.hpp"
#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace livekit::sdk {
// A completed operation. Accessing the inactive alternative violates the contract.
template <class T>
class [[nodiscard]] Result {
 public:
  static Result success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  static Result failure(std::shared_ptr<const FailureInfo> error) {
    assert(error);
    return Result(std::in_place_index<1>, std::move(error));
  }

  bool has_value() const noexcept {
    return state_.index() == 0;
  }

  explicit operator bool() const noexcept {
    return has_value();
  }

  T* value_if() noexcept {
    return std::get_if<0>(&state_);
  }

  const T* value_if() const noexcept {
    return std::get_if<0>(&state_);
  }

  const FailureInfo* error_if() const noexcept {
    const auto* error = std::get_if<1>(&state_);
    return error ? error->get() : nullptr;
  }

  T& operator*() & {
    assert(has_value());
    return *value_if();
  }

  const T& operator*() const& {
    assert(has_value());
    return *value_if();
  }

  T&& operator*() && {
    assert(has_value());
    return std::move(*value_if());
  }

  T* operator->() {
    assert(has_value());
    return value_if();
  }

  const T* operator->() const {
    assert(has_value());
    return value_if();
  }

  const FailureInfo& error() const {
    assert(!has_value());
    return *error_if();
  }

  const std::shared_ptr<const FailureInfo>& failure_record() const {
    assert(!has_value());
    return std::get<1>(state_);
  }

 private:
  explicit Result(std::in_place_index_t<0>, T value)
      : state_(std::in_place_index<0>, std::move(value)) {}

  explicit Result(std::in_place_index_t<1>, std::shared_ptr<const FailureInfo> error)
      : state_(std::in_place_index<1>, std::move(error)) {}

  std::variant<T, std::shared_ptr<const FailureInfo>> state_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  static Result success() {
    return Result({});
  }

  static Result failure(std::shared_ptr<const FailureInfo> error) {
    assert(error);
    return Result(std::move(error));
  }

  bool has_value() const noexcept {
    return !error_;
  }

  explicit operator bool() const noexcept {
    return has_value();
  }

  const FailureInfo* error_if() const noexcept {
    return error_.get();
  }

  const FailureInfo& error() const {
    assert(error_);
    return *error_;
  }

  const std::shared_ptr<const FailureInfo>& failure_record() const {
    assert(error_);
    return error_;
  }

 private:
  explicit Result(std::shared_ptr<const FailureInfo> error) : error_(std::move(error)) {}

  std::shared_ptr<const FailureInfo> error_;
};
}  // namespace livekit::sdk
