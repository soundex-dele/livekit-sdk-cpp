#include "backends/rtc/legacy_control_boundary.hpp"
#include "livekit/result.hpp"
#include "backends/rtc/control_context.hpp"
#include <memory>
#include <cstdlib>
#include <iostream>
using namespace livekit::sdk;

void check(bool value) {
  if (!value) {
    std::cerr << "Result contract failed\n";
    std::exit(1);
  }
}

int main() {
  auto done = Result<void>::success();
  check(done.has_value() && !done.error_if());
  auto void_error = make_failure(ErrorDomain::protocol, ErrorCode::timeout, "completion timeout");
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
  auto original_error = make_failure(ErrorDomain::signaling, ErrorCode::permission_denied,
                                     "denied", false, 403);
  auto error = with_context(original_error, {"publish", "track", {}, 7});
  check(error->id == original_error->id);
  auto failed = Result<int>::failure(error);
  check(!failed && !failed.value_if() && failed.error_if() == error.get());
  check(failed.error().id == error->id && failed.error().context.epoch == 7);
  auto converted = rtc::detail::control_boundary([&] { return Result<int>::failure(error); });
  check(!converted && converted.failure_record() == error);
  check(rtc::detail::control_boundary([] { return ErrorCode::ok; }) == ErrorCode::ok);
  check(rtc::detail::control_boundary([] { return ErrorCode::cancelled; }) == ErrorCode::cancelled);
  auto same_type = Result<std::shared_ptr<const FailureInfo>>::success(error);
  check(same_type && (*same_type)->id == error->id);
  std::cout << "Result contracts passed\n";
}
