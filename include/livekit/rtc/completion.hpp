#pragma once
#include "livekit/result.hpp"
#include <functional>

namespace livekit::sdk::rtc {
// Controls keep synchronous local setup. Validation and backend failures are
// delivered as Result<T>, without throwing to the caller. The optional completion
// receives the final result on notification, never inline on the submitting stack.
// It may run before the call returns. Replacing an Observer does not affect
// pending completions.
// Callbacks may submit requests, but must not wait for SDK work, leave, replace
// observers or destroy the engine. leave/close remain synchronous owner calls.
// Results own their data. Empty completions explicitly discard operation results.
bool is_callback_thread() noexcept;

template <class T>
using Completion = std::function<void(Result<T>)>;
}  // namespace livekit::sdk::rtc
