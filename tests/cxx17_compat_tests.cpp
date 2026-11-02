#include "livekit/compat/algorithm.hpp"
#include "livekit/compat/span.hpp"
#include "livekit/compat/thread.hpp"

#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>

using namespace std::chrono_literals;
namespace compat = livekit::compat;

namespace {
void check(bool value) {
  if (!value) {
    std::cerr << "C++17 compatibility contract failed\n";
    std::abort();
  }
}

void views_and_algorithms() {
  static_assert(!std::is_constructible_v<compat::span<int>, const std::vector<int>&>);
  static_assert(std::is_constructible_v<compat::span<const int>, const std::vector<int>&>);
  static_assert(!std::is_constructible_v<compat::span<int>, std::vector<int>&&>);
  int data[]{1, 2, 3};
  check(compat::span<int>(data, 0).empty());
  check(compat::span<const int>(data, data + 3).size() == 3);
  compat::span view(data);
  view[1] = 4;
  compat::span<const int> read_only = view;
  check(read_only.data() == data && read_only.size() == 3 && read_only[1] == 4);
  check(view.first(2).back() == 4 && view.last(1).front() == 3);
  check(view.subspan(1).size() == 2 && view.subspan(3).empty());
  check(compat::as_bytes(view).size() == sizeof(data));
  check(compat::span<int>{}.subspan(0).data() == nullptr);
  std::array<int, 3> expected{1, 4, 3};
  check(compat::equal(view, expected));
  check(!compat::equal(view.first(2), expected));
  std::vector<int> values{1, 2, 3, 2};
  check(compat::erase_if(values, [](int v) { return v == 2; }) == 2);
  check(values == std::vector<int>({1, 3}));
  std::map<int, int> entries{{1, 2}, {3, 4}};
  check(compat::erase_if(entries, [](auto& v) { return v.first == 1; }) == 1);
  check(entries.size() == 1 && entries.begin()->first == 3);
  check(compat::starts_with("abc", "ab") && compat::ends_with("abc", 'c'));
  check(!compat::ends_with("", 'c') && !compat::starts_with("ab", "abc"));
}

void cancellation() {
  compat::stop_token expired;
  {
    compat::stop_source source;
    expired = source.get_token();
    check(expired.stop_possible());
  }
  check(!expired.stop_possible() && !expired.stop_requested());
  compat::stop_source source;
  auto copy = source;
  int calls = 0;
  {
    compat::stop_callback removed(source.get_token(), [&] { ++calls; });
  }
  compat::stop_callback retained(source.get_token(), [&] { ++calls; });
  check(copy.request_stop() && source.stop_requested());
  check(!source.request_stop() && calls == 1);
  compat::stop_callback late(source.get_token(), [&] { ++calls; });
  check(calls == 2);

  // A callback can unregister and destroy itself during a stop request.
  compat::stop_source self_source;
  using Callback = compat::stop_callback<std::function<void()>>;
  std::unique_ptr<Callback> self;
  self = std::make_unique<Callback>(self_source.get_token(), [&] { self.reset(); });
  check(self_source.request_stop() && !self);

  // Removal on another thread must wait for an executing callback to finish.
  compat::stop_source concurrent;
  std::promise<void> entered, release, removing, removed;
  auto proceed = release.get_future().share();
  auto callback = std::make_unique<Callback>(concurrent.get_token(), [&] {
    entered.set_value();
    proceed.wait();
  });
  std::thread requester([&] { concurrent.request_stop(); });
  entered.get_future().wait();
  auto finished = removed.get_future();
  std::thread remover([&] {
    removing.set_value();
    callback.reset();
    removed.set_value();
  });
  removing.get_future().wait();
  check(finished.wait_for(20ms) == std::future_status::timeout);
  release.set_value();
  requester.join();
  remover.join();
  check(finished.wait_for(0ms) == std::future_status::ready);
}

void interruptible_waits() {
  // Exercise both stop-before-wait and stop-during-wait races repeatedly.
  for (int i = 0; i != 100; ++i) {
    if (i % 25 == 0) {
      std::cout << "stop/wait race " << i << std::endl;
    }
    compat::condition_variable_any changed;
    std::mutex mutex;
    std::promise<void> entered;
    std::atomic<bool> done{false};
    {
      compat::jthread worker([&](compat::stop_token stop) {
        std::unique_lock<std::mutex> lock(mutex);
        entered.set_value();
        check(!changed.wait(lock, stop, [] { return false; }));
        check(lock.owns_lock());
        done = true;
      });
      if (i % 2) {
        entered.get_future().wait();
      }
      compat::jthread moved(std::move(worker));
      check(!worker.joinable() && moved.joinable());
    }
    check(done.load());
  }
  std::cout << "timed waits and notification" << std::endl;
  compat::condition_variable_any changed;
  std::mutex mutex;
  std::unique_lock<std::mutex> lock(mutex);
  compat::stop_source source;
  check(!changed.wait_for(lock, source.get_token(), 1ms, [] { return false; }));
  source.request_stop();
  check(changed.wait(lock, source.get_token(), [] { return true; }));
  check(!changed.wait_until(lock, source.get_token(), std::chrono::steady_clock::now() + 1h,
                            [] { return false; }));

  lock.unlock();
  bool ready = false;
  compat::jthread notifier([&] {
    std::lock_guard<std::mutex> guard(mutex);
    ready = true;
    changed.notify_all();
  });
  lock.lock();
  check(changed.wait_for(lock, {}, 2s, [&] { return ready; }));
}
}  // namespace

int main() {
  std::cout << "views and algorithms" << std::endl;
  views_and_algorithms();
  std::cout << "cancellation" << std::endl;
  cancellation();
  std::cout << "interruptible waits" << std::endl;
  interruptible_waits();
  std::cout << "compatibility contracts passed" << std::endl;
}
