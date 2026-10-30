#pragma once
#include "livekit/result.hpp"
#include <functional>

namespace livekit::sdk::rtc {
// Control calls return admission status: rejection never invokes completion.
// Accepted callbacks are retained until delivery on the room's serial notification
// thread, outside internal locks. They may run before the submitting call returns,
// but never inline on its stack. Callbacks may submit controls, but must not wait
// for SDK work or destroy the engine. Results own their data and may be retained.
// Stop concurrent callers before destruction and destroy outside SDK callbacks.
// For blocking adapters: waiting on SDK work from these threads would deadlock.
bool is_callback_thread() noexcept;

template <class T>
using Completion = std::function<void(Result<T>)>;
}  // namespace livekit::sdk::rtc
