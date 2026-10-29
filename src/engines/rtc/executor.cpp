#include "livekit/rtc/executor.hpp"
#include "livekit/error.hpp"
#include "utoolkit/task_queue/task_queue.h"
#include <mutex>

namespace livekit::sdk::rtc::engine {
namespace {
std::exception_ptr cancelled() {
  return std::make_exception_ptr(
      SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "RTC executor stopped"));
}
}  // namespace

struct RtcExecutor::Impl {
  struct Pending {
    std::function<bool()> complete;
    std::function<void(std::exception_ptr)> cancel;
  };

  const ExecutorOptions options;
  mutable std::mutex mutex;
  std::mutex shutdown_mutex;
  ExecutorSnapshot stats;
  bool accepting = true;
  size_t outstanding = 0;
  std::unique_ptr<vi::TaskQueue> queue;
  std::unique_ptr<RtcEngine> engine;
  std::vector<Pending> pending;  // Queue only.
  bool completion_scheduled = false;

  explicit Impl(ExecutorOptions value) : options(value) {
    if (!options.max_operations || options.max_operations > 65536) {
      throw std::invalid_argument("Invalid RTC executor configuration");
    }
    queue = vi::TaskQueue::create("rtc.commands");
  }

  ~Impl() {
    // Join before members referenced by deferred tasks are destroyed.
    queue.reset();
  }

  void finished() {
    std::lock_guard lock(mutex);
    --outstanding;
    ++stats.commands_completed;
  }

  void finish_futures() {
    completion_scheduled = false;
    for (auto it = pending.begin(); it != pending.end();) {
      bool complete = false;
      try {
        complete = it->complete();
      } catch (...) {
        it->cancel(std::current_exception());
        complete = true;
      }
      if (complete) {
        it = pending.erase(it);
        finished();
      } else {
        ++it;
      }
    }
    {
      std::lock_guard lock(mutex);
      stats.pending_operations = pending.size();
    }
    if (!pending.empty() && !completion_scheduled) {
      completion_scheduled = true;
      // std::future has no continuation API. This adapter only checks the
      // caller's returned futures; it never advances the engine or graph.
      try {
        queue->postDelayedTask([this] { finish_futures(); }, 2);
      } catch (...) {
        completion_scheduled = false;
        const auto failure = std::current_exception();
        for (auto& operation : pending) {
          operation.cancel(failure);
          finished();
        }
        pending.clear();
        std::lock_guard lock(mutex);
        stats.pending_operations = 0;
      }
    }
  }
};

RtcExecutor::RtcExecutor(Factory factory, ExecutorOptions options)
    : impl_(std::make_unique<Impl>(options)) {
  if (!factory) {
    throw std::invalid_argument("RTC executor requires an engine factory");
  }
  std::packaged_task<void()> create([&] {
    impl_->engine = factory();
    if (!impl_->engine) {
      throw std::invalid_argument("RTC executor factory returned no engine");
    }
    std::lock_guard lock(impl_->mutex);
    impl_->stats.running = true;
  });
  auto ready = create.get_future();
  impl_->queue->postSyncTask([&] { create(); });
  ready.get();
}

RtcExecutor::~RtcExecutor() {
  shutdown();
}

void RtcExecutor::enqueue(Command command) {
  std::exception_ptr rejection;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting) {
      rejection = cancelled();
    } else if (impl_->outstanding >= impl_->options.max_operations) {
      rejection =
          std::make_exception_ptr(std::runtime_error("RTC executor operation limit reached"));
    } else {
      ++impl_->outstanding;
      ++impl_->stats.queued_operations;
      try {
        impl_->queue->postTask([state = impl_.get(), command = std::move(command)]() mutable {
          bool accepting;
          {
            std::lock_guard lock(state->mutex);
            --state->stats.queued_operations;
            accepting = state->accepting;
            ++state->stats.commands_started;
          }
          if (!accepting) {
            command.cancel(cancelled());
            state->finished();
            return;
          }
          const auto begin = SteadyClock::now();
          try {
            auto waiting = command.start(*state->engine);
            if (waiting) {
              state->pending.push_back({std::move(waiting), command.cancel});
              if (!state->completion_scheduled) {
                state->finish_futures();
              }
            } else {
              state->finished();
            }
          } catch (...) {
            command.cancel(std::current_exception());
            state->finished();
          }
          std::lock_guard lock(state->mutex);
          state->stats.max_command_duration = std::max(
              state->stats.max_command_duration,
              std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - begin));
          state->stats.pending_operations = state->pending.size();
        });
      } catch (...) {
        --impl_->outstanding;
        --impl_->stats.queued_operations;
        throw;
      }
      return;
    }
  }
  command.cancel(rejection);
}

ExecutorSnapshot RtcExecutor::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

void RtcExecutor::shutdown() {
  if (impl_->queue->isCurrent()) {
    throw std::logic_error("Cannot shut down RTC executor from its own task");
  }
  std::lock_guard shutdown_lock(impl_->shutdown_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
  }
  impl_->queue->postSyncTask([this] {
    for (auto& operation : impl_->pending) {
      operation.cancel(cancelled());
      impl_->finished();
    }
    impl_->pending.clear();
    impl_->engine.reset();
    std::lock_guard lock(impl_->mutex);
    impl_->stats.running = false;
    impl_->stats.pending_operations = 0;
  });
}
}  // namespace livekit::sdk::rtc::engine
