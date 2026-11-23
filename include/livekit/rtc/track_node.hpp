#pragma once
#include "livekit/rtc/events.hpp"
#include "livekit/result.hpp"
#include <graphflow/graph.hpp>
#include <mutex>
#include <tuple>

namespace livekit::sdk::rtc {
// RTC has a fixed wire vocabulary; port names are stable diagnostics, not compiler RTTI.
template <class E>
struct TrackPort;
#define LIVEKIT_TRACK_PORT(Event)                              \
  template <>                                                  \
  struct TrackPort<nodes::Event> {                             \
    static constexpr graphflow::In<nodes::Event> in{#Event};   \
    static constexpr graphflow::Out<nodes::Event> out{#Event}; \
  };
LIVEKIT_TRACK_PORT(EncodedFrameEvent)
LIVEKIT_TRACK_PORT(SenderFeedbackEvent)
LIVEKIT_TRACK_PORT(KeyframeRequestEvent)
LIVEKIT_TRACK_PORT(AudioFrameEvent)
LIVEKIT_TRACK_PORT(VideoFrameEvent)
#undef LIVEKIT_TRACK_PORT

inline graphflow::QueueCost track_cost(const nodes::EncodedFrameEvent& event) {
  return {event.frame.buffer.size(), uint64_t(std::max<int64_t>(0, event.frame.duration.count()))};
}

inline graphflow::QueueCost track_cost(const nodes::AudioFrameEvent& event) {
  if (!event.pcm) {
    return {};
  }
  const auto& p = *event.pcm;
  return {p.samples.size() * sizeof(int16_t) + p.float_samples.size() * sizeof(float),
          p.sample_rate ? uint64_t(p.samples_per_channel()) * 1000000 / p.sample_rate : 0};
}

inline graphflow::QueueCost track_cost(const nodes::VideoFrameEvent& event) {
  if (!event.pixels) {
    return {};
  }
  return {event.pixels->y.size() + event.pixels->u.size() + event.pixels->v.size(), 0};
}

inline graphflow::QueueCost track_cost(const nodes::SenderFeedbackEvent&) {
  return {};
}

inline graphflow::QueueCost track_cost(const nodes::KeyframeRequestEvent&) {
  return {};
}

// Owned media processors can also be observed by the SDK for worker statistics.
// The graph exclusively binds/starts/stops each processor once.
class TrackNode : public std::enable_shared_from_this<TrackNode> {
 public:
  explicit TrackNode(std::string name) : name_(std::move(name)) {}

  virtual ~TrackNode() = default;

  const std::string& name() const {
    return name_;
  }

  virtual void bind(graphflow::Binder& ports) = 0;

  virtual Result<void> prepare() {
    return Result<void>::success();
  }

  virtual Result<void> activate_graph() {
    return Result<void>::success();
  }

  virtual void stop_graph() {}

  virtual void release_graph() {}

  Failure last_failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
  }

  void set_failure_handler(std::function<void(Failure)> callback) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(callback);
  }

  uint64_t delivery_drops() const {
    return delivery_drops_.load();
  }

  bool claimed() const {
    return claimed_.load();
  }

  bool claim() {
    return !claimed_.exchange(true);
  }

  void report_failure(Failure error) {
    std::function<void(Failure)> notify;
    {
      std::lock_guard lock(mutex_);
      if (failure_ || !error) {
        return;
      }
      failure_ = error;
      notify = handler_;
    }
    if (notify) {
      notify(std::move(error));
    }
  }

 protected:
  template <class E>
  void output(graphflow::Binder& ports) {
    std::get<graphflow::Emitter<E>>(emitters_) = ports.output(TrackPort<E>::out);
  }

  template <class E>
  void send(E event) {
    const auto cost = track_cost(event);
    auto result = std::get<graphflow::Emitter<E>>(emitters_).emit(std::move(event), {}, cost);
    delivery_drops_.fetch_add(result.evicted + result.skipped.size(), std::memory_order_relaxed);
    switch (result.status) {
      case graphflow::Admission::AcceptedWithDrops:
      case graphflow::Admission::Accepted:
      case graphflow::Admission::NoRoute:
      case graphflow::Admission::Closed:
        return;
      default:
        report_failure(make_failure(
            ErrorDomain::media,
            result.status == graphflow::Admission::Full ? ErrorCode::resource_exhausted
                                                        : ErrorCode::processing_failed,
            name_ + ": delivery failed at " + TrackPort<E>::out.name + " " + result.error.message));
    }
  }

  Result<void> start_error() const {
    auto e = last_failure();
    return Result<void>::failure(e ? e
                                   : make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                  name_ + ": cannot start"));
  }

 private:
  std::string name_;
  std::atomic<uint64_t> delivery_drops_{0};
  std::atomic<bool> claimed_{false};
  mutable std::mutex mutex_;
  Failure failure_;
  std::function<void(Failure)> handler_;
  std::tuple<graphflow::Emitter<nodes::EncodedFrameEvent>,
             graphflow::Emitter<nodes::SenderFeedbackEvent>,
             graphflow::Emitter<nodes::KeyframeRequestEvent>,
             graphflow::Emitter<nodes::AudioFrameEvent>, graphflow::Emitter<nodes::VideoFrameEvent>>
      emitters_;
};
}  // namespace livekit::sdk::rtc
