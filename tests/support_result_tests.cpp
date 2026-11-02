#include "support/process.hpp"
#include "support/ready_set.hpp"
#include "support/deadline_index.hpp"
#include "support/control_executor.hpp"
#include "support/task_executor.hpp"
#include "livekit/async_result.hpp"
#include <cstdlib>
#include <iostream>
#include <future>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void indexed_work() {
  auto wake = std::make_shared<detail::WakeEvent>();
  auto ready = std::make_shared<detail::ReadySet<int>>(wake, 1);
  auto watched = ready->watch(7);
  check(bool(watched), "Watch rejected");
  auto duplicate = ready->watch(7);
  check(!duplicate && duplicate.error().code == ErrorCode::invalid_state,
        "Duplicate watch accepted");
  auto full = ready->watch(8);
  check(!full && full.error().code == ErrorCode::resource_exhausted, "Watch capacity ignored");
  (*watched)->signal();
  check(ready->take() == std::set<int>{7}, "Watch failed to notify");
  ready->forget(7);
  (*watched)->signal();
  check(ready->take().empty(), "Retired watch remained active");
  detail::ReadySet<int> invalid({}, 0);
  auto invalid_watch = invalid.watch(1);
  check(!invalid_watch && invalid_watch.error().code == ErrorCode::invalid_argument,
        "Invalid ready set accepted registration");
  detail::DeadlineIndex<int> deadlines;
  auto now = std::chrono::steady_clock::now();
  deadlines.set(1, now);
  deadlines.set(1, now + 1s);
  check(deadlines.size() == 1 && deadlines.take_due(now).empty(), "Stale deadline retained");
  check(deadlines.take_due(now + 1s) == std::vector<int>{1}, "Updated deadline lost");
}

void task_failures() {
  auto failure =
      make_failure(ErrorDomain::lifecycle, ErrorCode::callback_failed, "callback failed");
  bool reported = false;
  detail::ControlExecutor executor(4, [&] { reported = true; });
  check(bool(executor.post([failure] { return Result<void>::failure(failure); })),
        "Control callback rejected");
  auto drained = executor.drain();
  check(!drained && drained.failure_record() == failure && reported, "Control failure lost");
  auto rejected = executor.post([] { return Result<void>::success(); });
  check(!rejected && rejected.failure_record() == failure, "Failed control lane accepted work");
  detail::TaskExecutor tasks(1, 4);
  auto completion = std::make_shared<CompletionSource<int>>();
  auto result = completion->get_result();
  check(tasks.post([completion] { completion->set_value(42); }), "Task rejected");
  auto value = result.get();
  check(value && *value == 42, "Task completion lost");
}

void task_shutdown_reentry() {
  detail::TaskExecutor tasks(1, 4);
  std::promise<void> started, release;
  auto began = started.get_future();
  auto gate = release.get_future().share();
  check(tasks.post([&started, gate] {
    started.set_value();
    gate.wait();
  }),
        "Worker rejected");
  check(began.wait_for(5s) == std::future_status::ready, "Worker did not start");
  auto source = std::make_shared<CompletionSource<int>>();
  auto cancelled = source->get_result().then(
      [](auto task) { task(); },
      [&tasks](AsyncResult<int> ready) {
        auto result = ready.get();
        return !result && result.error().code == ErrorCode::cancelled && !tasks.post([] {});
      });
  check(tasks.post([source] { source->set_value(1); }), "Queued completion rejected");
  source.reset();
  std::jthread stopping([&] { tasks.stop(); });
  check(cancelled.wait_for(5s) == std::future_status::ready,
        "Cancellation reentered executor while its queue lock was held");
  auto result = cancelled.get();
  check(result && *result, "Shutdown did not cancel queued work explicitly");
  release.set_value();
  stopping.join();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--echo") {
    std::cout << argv[2];
    return 0;
  }
  indexed_work();
  task_failures();
  task_shutdown_reentry();
  lk::Process empty({});
  check(!empty.status() && !empty.wait(), "Invalid process must report failure");
  auto invalid = lk::Process::capture({argv[0], std::string("a\0b", 3)});
  check(!invalid && invalid.error().code == ErrorCode::invalid_argument, "NUL argument accepted");
  const std::string text = "two words \"quote\" trailing\\ $() & ;";
  auto output = lk::Process::capture({argv[0], "--echo", text});
  check(output && *output == text, "Process argument changed or capture failed");
  auto missing = lk::Process::capture({"__livekit_missing_process_for_test__"});
  check(!missing, "Missing executable did not report failure");
  std::cout << "Support result contracts passed\n";
}
