#include "livekit/async_result.hpp"
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void check(bool value) {
  if (!value) {
    std::cerr << "Async result contract failed\n";
    std::exit(1);
  }
}

void continuations() {
  std::deque<std::function<void()>> work;
  auto dispatch = [&](std::function<void()> task) {
    work.push_back(std::move(task));
    return Result<void>::success();
  };
  CompletionSource<int> source;
  auto result = source.get_result().then(dispatch, [](AsyncResult<int> ready) {
    auto value = ready.get();
    if (!value) {
      return Result<std::unique_ptr<int>>::failure(value.failure_record());
    }
    return Result<std::unique_ptr<int>>::success(std::make_unique<int>(*value + 1));
  });
  check(work.empty());
  check(source.set_value(41) == ErrorCode::ok);
  check(work.size() == 1 && result.wait_for(0ms) == std::future_status::timeout);
  auto task = std::move(work.front());
  work.pop_front();
  task();
  auto value = result.get();
  check(value && **value == 42);
  check(!result.get() && !result.wait());

  CompletionSource<void> already_ready;
  already_ready.set_value();
  auto chained = already_ready.get_result().then(dispatch, [](AsyncResult<void> ready) {
    auto initial = ready.get();
    CompletionSource<int> second;
    auto value = second.get_result();
    if (!initial) {
      second.set_failure(initial.failure_record());
    } else {
      second.set_value(7);
    }
    return value;
  });
  check(work.size() == 1);
  work.front()();
  work.pop_front();
  auto nested = chained.get();
  check(nested && *nested == 7);
}

void failures() {
  auto inline_dispatch = [](auto task) { task(); };
  AsyncResult<int> abandoned;
  {
    CompletionSource<int> source;
    abandoned = source.get_result();
  }
  auto recovered = std::move(abandoned).then(inline_dispatch, [](AsyncResult<int> ready) {
    auto result = ready.get();
    return !result && result.error().code == ErrorCode::cancelled;
  });
  auto recovery = recovered.get();
  check(recovery && *recovery);

  auto failure = make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "queue stopped");
  CompletionSource<void> rejected;
  bool called = false;
  auto result =
      rejected.get_result().then([failure](auto) { return Result<void>::failure(failure); },
                                 [&](AsyncResult<void>) { called = true; });
  rejected.set_value();
  auto rejection = result.get();
  check(!called && !rejection && rejection.failure_record() == failure);

  CompletionSource<int> original;
  auto propagated = original.get_result().then(inline_dispatch,
                                               [](AsyncResult<int> ready) { return ready.get(); });
  original.set_failure(failure);
  auto error = propagated.get();
  check(!error && error.failure_record() == failure);

  auto invalid = AsyncResult<int>{}.then(inline_dispatch, [](AsyncResult<int>) { return 0; });
  auto invalid_result = invalid.get();
  check(!invalid_result && invalid_result.error().code == ErrorCode::invalid_state);
  CompletionSource<int> duplicate;
  auto first = duplicate.get_result();
  check(!duplicate.get_result().valid());
  check(duplicate.set_value(1) == ErrorCode::ok);
  check(duplicate.set_value(2) == ErrorCode::invalid_state);
  auto first_value = first.get();
  check(first_value && *first_value == 1);
}

void concurrent_completion() {
  CompletionSource<int> source;
  auto result = source.get_result();
  check(result.wait_for(0ms) == std::future_status::timeout);
  std::jthread worker([&] { source.set_value(42); });
  check(result.wait_until(std::chrono::steady_clock::now() + 5s) == std::future_status::ready);
  auto future = std::move(result).as_future();
  auto value = future.get();
  check(value && *value == 42);

  std::future<Result<int>> cancelled;
  {
    CompletionSource<int> pending;
    cancelled = pending.get_result().as_future();
  }
  auto failure = cancelled.get();
  check(!failure && failure.error().code == ErrorCode::cancelled);
}
}  // namespace

int main() {
  continuations();
  failures();
  concurrent_completion();
  std::cout << "Async result contracts passed\n";
}
