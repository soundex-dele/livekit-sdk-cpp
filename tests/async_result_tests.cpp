#include "livekit/async_result.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void check(bool value) {
  if (!value) {
    throw std::runtime_error("Async result contract failed");
  }
}

void continuations() {
  std::deque<std::function<void()>> work;
  auto dispatch = [&](std::function<void()> task) { work.push_back(std::move(task)); };
  CompletionSource<int> source;
  auto result = source.get_result().then(
      dispatch, [](AsyncResult<int> ready) { return std::make_unique<int>(ready.get() + 1); });
  check(work.empty());
  source.set_value(41);
  check(work.size() == 1 && result.wait_for(0ms) == std::future_status::timeout);
  auto task = std::move(work.front());
  work.pop_front();
  task();
  check(*result.get() == 42);

  CompletionSource<void> already_ready;
  already_ready.set_value();
  auto chained = already_ready.get_result().then(dispatch, [](AsyncResult<void> ready) {
    ready.get();
    CompletionSource<int> second;
    auto value = second.get_result();
    second.set_value(7);
    return value;
  });
  check(work.size() == 1);
  work.front()();
  work.pop_front();
  check(chained.get() == 7);
}

void failures() {
  auto inline_dispatch = [](auto task) { task(); };
  AsyncResult<int> abandoned;
  {
    CompletionSource<int> source;
    abandoned = source.get_result();
  }
  auto recovered = std::move(abandoned).then(inline_dispatch, [](AsyncResult<int> ready) {
    try {
      ready.get();
    } catch (const std::future_error& failure) {
      return failure.code() == std::make_error_code(std::future_errc::broken_promise);
    }
    return false;
  });
  check(recovered.get());

  CompletionSource<void> rejected;
  auto result = rejected.get_result().then([](auto) { throw std::runtime_error("queue stopped"); },
                                           [](AsyncResult<void>) {});
  rejected.set_value();
  try {
    result.get();
    check(false);
  } catch (const std::runtime_error& failure) {
    check(std::string(failure.what()) == "queue stopped");
  }

  CompletionSource<int> original;
  auto propagated = original.get_result().then(inline_dispatch,
                                               [](AsyncResult<int> ready) { return ready.get(); });
  original.set_exception(std::make_exception_ptr(std::runtime_error("original failure")));
  try {
    propagated.get();
    check(false);
  } catch (const std::runtime_error& failure) {
    check(std::string(failure.what()) == "original failure");
  }
}
}  // namespace

int main() {
  try {
    continuations();
    failures();
    std::cout << "Async result contracts passed\n";
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
