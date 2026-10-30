#include "livekit/result.hpp"
#include "backends/rtc/control_context.hpp"
#include <memory>
#include <stdexcept>
#include <iostream>
using namespace livekit::sdk;

void check(bool value) {
  if (!value) {
    throw std::runtime_error("Result contract failed");
  }
}

int main() {
  auto done = Result<void>::success();
  check(done.has_value() && !done.error_if());
  auto void_error = describe_failure(std::make_exception_ptr(
      SDKError(ErrorDomain::protocol, ErrorCode::timeout, "completion timeout")));
  auto failed_void = Result<void>::failure(void_error);
  check(!failed_void && failed_void.error_if() == void_error.get());
  check(failed_void.failure_record() == void_error &&
        failed_void.error().code == ErrorCode::timeout);
  auto empty = rtc::detail::control_boundary([] {});
  static_assert(std::is_same_v<decltype(empty), ErrorCode>);
  check(empty == ErrorCode::ok);
  auto value = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));
  check(value.has_value() && **value == 42 && value.value_if());
  auto moved = *std::move(value);
  check(*moved == 42);
  auto error = describe_failure(
      std::make_exception_ptr(
          SDKError(ErrorDomain::signaling, ErrorCode::permission_denied, "denied", false, 403)),
      {"publish", "track", {}, 7});
  auto failed = Result<int>::failure(error);
  check(!failed && !failed.value_if() && failed.error_if() == error.get());
  check(failed.error().id == error->id && failed.error().context.epoch == 7);
  auto converted = rtc::detail::control_boundary([&]() -> Result<int> { throw SDKError(error); });
  check(!converted && converted.error().id == error->id && converted.error().backend_reason == 403);
  auto code = rtc::detail::control_boundary([&]() -> ErrorCode { throw SDKError(error); });
  check(code == ErrorCode::permission_denied);
  check(rtc::detail::control_boundary([&] { throw SDKError(error); }) ==
        ErrorCode::permission_denied);
  check(rtc::detail::control_boundary([] { return ErrorCode::ok; }) == ErrorCode::ok);
  check(rtc::detail::control_boundary([] { return ErrorCode::cancelled; }) == ErrorCode::cancelled);
  auto invalid = rtc::detail::control_boundary([] { throw std::invalid_argument("invalid"); });
  check(invalid == ErrorCode::invalid_argument);
  check(rtc::detail::control_boundary([]() -> ErrorCode { throw std::logic_error("state"); }) ==
        ErrorCode::invalid_state);
  check(rtc::detail::control_boundary([] { throw std::runtime_error("failure"); }) ==
        ErrorCode::internal_error);
  check(rtc::detail::control_boundary([]() -> ErrorCode { throw 42; }) ==
        ErrorCode::internal_error);
  auto same_type = Result<std::shared_ptr<const FailureInfo>>::success(error);
  check(same_type && (*same_type)->id == error->id);
  std::cout << "Result contracts passed\n";
}
