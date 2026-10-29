#include "livekit/rtc/executor.hpp"
#include "livekit/error.hpp"
#include <map>
#include <mutex>

namespace livekit::sdk::rtc::engine {
namespace {
std::exception_ptr cancelled() {
  return std::make_exception_ptr(
      SDKError(ErrorDomain::lifecycle, ErrorCode::cancelled, "RTC executor stopped"));
}
}  // namespace

struct RtcExecutor::Impl {
  struct State {
    mutable std::mutex mutex;
    ExecutorSnapshot stats;
    bool accepting = true;
    uint64_t next_id = 0;
    std::map<uint64_t, std::function<void(std::exception_ptr)>> pending;

    void finish(uint64_t id) {
      std::lock_guard lock(mutex);
      if (pending.erase(id)) {
        ++stats.commands_completed;
        stats.pending_operations = pending.size() - stats.queued_operations;
      }
    }
  };

  ExecutorOptions options;
  std::shared_ptr<State> state = std::make_shared<State>();
  std::unique_ptr<RtcEngine> engine;
  std::mutex shutdown_mutex;

  explicit Impl(ExecutorOptions value) : options(value) {
    if (!options.max_operations || options.max_operations > 65536) {
      throw std::invalid_argument("Invalid RTC executor configuration");
    }
  }
};

RtcExecutor::RtcExecutor(Factory factory, ExecutorOptions options)
    : impl_(std::make_unique<Impl>(options)) {
  if (!factory) {
    throw std::invalid_argument("RTC executor requires an engine factory");
  }
  impl_->engine = factory();
  if (!impl_->engine) {
    throw std::invalid_argument("RTC executor factory returned no engine");
  }
  impl_->state->stats.running = true;
}

RtcExecutor::~RtcExecutor() {
  shutdown();
}

void RtcExecutor::enqueue(Command command) {
  auto state = impl_->state;
  std::exception_ptr rejection;
  {
    std::lock_guard lock(state->mutex);
    if (!state->accepting) {
      rejection = cancelled();
    } else if (state->pending.size() >= impl_->options.max_operations) {
      rejection =
          std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                                           "Executor operation limit reached"));
    } else {
      const auto id = ++state->next_id;
      state->pending.emplace(id, command.cancel);
      ++state->stats.queued_operations;
      auto operation = std::make_shared<Command>(std::move(command));
      try {
        impl_->engine->post_control([state, id, operation, engine = impl_->engine.get()] {
          bool accepting;
          {
            std::lock_guard lock(state->mutex);
            --state->stats.queued_operations;
            ++state->stats.commands_started;
            state->stats.pending_operations =
                state->pending.size() - state->stats.queued_operations;
            accepting = state->accepting;
          }
          if (!accepting) {
            operation->cancel(cancelled());
            state->finish(id);
            return;
          }
          const auto begin = SteadyClock::now();
          operation->start(*engine, [state, id] { state->finish(id); });
          std::lock_guard lock(state->mutex);
          state->stats.max_command_duration = std::max(
              state->stats.max_command_duration,
              std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - begin));
        });
        return;
      } catch (...) {
        --state->stats.queued_operations;
        state->pending.erase(id);
        rejection = std::current_exception();
        command = std::move(*operation);
      }
    }
  }
  command.cancel(rejection);
}

ExecutorSnapshot RtcExecutor::snapshot() const {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->stats;
}

void RtcExecutor::shutdown() {
  // Borrowed engine tasks cannot join the queue on which they are running.
  // shutdown callers must serialize object destruction with other API callers.
  std::lock_guard shutdown_lock(impl_->shutdown_mutex);
  if (!impl_->engine) {
    return;
  }
  if (impl_->engine->on_control_thread()) {
    throw std::logic_error("Cannot shut down RTC executor from its own task");
  }
  auto state = impl_->state;
  {
    std::lock_guard lock(state->mutex);
    state->accepting = false;
  }
  try {
    impl_->engine->close().get();
  } catch (const std::exception& error) {
    std::lock_guard lock(state->mutex);
    state->stats.error = error.what();
  }
  impl_->engine.reset();  // Fences and joins all queued engine commands.
  std::map<uint64_t, std::function<void(std::exception_ptr)>> pending;
  {
    std::lock_guard lock(state->mutex);
    pending.swap(state->pending);
    state->stats.commands_completed += pending.size();
    state->stats.pending_operations = state->stats.queued_operations = 0;
    state->stats.running = false;
  }
  for (auto& [id, cancel] : pending) {
    cancel(cancelled());
  }
}
}  // namespace livekit::sdk::rtc::engine
