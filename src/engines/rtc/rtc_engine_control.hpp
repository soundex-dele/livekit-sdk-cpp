#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/rtc_execution_context.hpp"
#include "engines/rtc/track_graph_registry.hpp"
#include "engines/rtc/media_graph_contract.hpp"

namespace livekit::sdk::rtc::engine {
// Public operations have one admission boundary; execution and notification
// policy stay here rather than being repeated in every facade method.
template <class Function, class Callback>
ErrorCode RtcEngine::submit(std::string key, Function function, Callback completion, bool barrier,
                            bool terminal) {
  return detail::control_boundary([&] {
    return control->async->submit(
        std::move(key),
        [this, function = std::move(function)]() mutable {
          return lifecycle.submit(std::move(function));
        },
        std::move(completion), barrier, terminal);
  });
}

// Runs inline and never retains the callable; references to caller arguments are safe.
template <class Function>
auto RtcEngine::execute(Function function, bool allow_closed) const {
  using Value = std::invoke_result_t<Function>;
  using Return = std::conditional_t<std::is_void_v<Value>, ErrorCode, Value>;
  const auto fail = [](ErrorCode code, std::string message) -> Return {
    if constexpr (std::is_same_v<Return, ErrorCode>) {
      return code;
    } else {
      return Return::failure(std::make_shared<FailureInfo>(
          FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, std::move(message)}));
    }
  };
  return detail::control_boundary([&]() -> Return {
    if (detail::callback_depth || detail::active_session) {
      return fail(ErrorCode::invalid_thread,
                  "RTC control cannot be called from callbacks or nested sessions");
    }
    detail::OperationGuard guard(*control);
    if (control->closed && !allow_closed) {
      return fail(ErrorCode::session_closed, "RTC engine closed");
    }
    auto detached = detach(graphs->take_failed());
    if (detached != ErrorCode::ok) {
      return fail(detached, "RTC track detachment failed");
    }
    if constexpr (std::is_void_v<Value>) {
      function();
      return ErrorCode::ok;
    } else {
      return function();
    }
  });
}

}  // namespace livekit::sdk::rtc::engine
