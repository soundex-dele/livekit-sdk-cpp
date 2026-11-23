#pragma once
#include <cstdio>
#include <cstdlib>
#include "livekit/rtc/track_graph.hpp"

namespace livekit::sdk::test {
template <class Event>
rtc::TrackConnection connect(const std::shared_ptr<rtc::TrackNode>& a,
                             const std::shared_ptr<rtc::TrackNode>& b) {
  return rtc::TrackConnection::between<Event>(a->name(), b->name());
}

// A blocked sibling receiver must not prevent a control command reaching its target.
inline void wait_receiver(graphflow::Graph& graph, const std::string& name) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < until) {
    auto snap = graph.snapshot();
    if (snap) {
      for (const auto& n : snap->nodes) {
        if (n.id == name && !n.waiting && !n.executing) {
          return;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::fprintf(stderr, "Receiver did not settle: %s\n", name.c_str());
  std::abort();
}

struct CodecWorkerProbe {
  template <class T>
  static void after_packet(T& codec, std::function<void()> hook) {
    codec.after_packet_ = std::move(hook);
  }
};
}  // namespace livekit::sdk::test
