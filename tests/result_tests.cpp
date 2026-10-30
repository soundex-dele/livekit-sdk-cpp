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
  auto empty = Result<void>::success();
  check(empty.has_value() && !empty.error_if());
  auto value = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));
  check(value.has_value() && **value == 42 && value.value_if());
  auto moved = *std::move(value);
  check(*moved == 42);
  auto error = describe_failure(
      std::make_exception_ptr(
          SDKError(ErrorDomain::signaling, ErrorCode::permission_denied, "denied", false, 403)),
      {"publish", "track", {}, 7});
  auto failed = Result<int>::failure(error);
  auto failed_void = Result<void>::failure(error);
  check(!failed && !failed.value_if() && failed.error_if() == error.get());
  check(failed.error().id == failed_void.error().id && failed.error().context.epoch == 7);
  auto converted = rtc::detail::control_boundary([&]() -> Result<void> { throw SDKError(error); });
  check(!converted && converted.error().id == error->id && converted.error().backend_reason == 403);
  auto invalid = rtc::detail::control_boundary([] { throw std::invalid_argument("invalid"); });
  check(!invalid && invalid.error().code == ErrorCode::invalid_argument);
  auto same_type = Result<std::shared_ptr<const FailureInfo>>::success(error);
  check(same_type && (*same_type)->id == error->id);
  std::cout << "Result contracts passed\n";
}
