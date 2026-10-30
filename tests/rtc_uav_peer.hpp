#include "rtc_blocking_bridge.hpp"
#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "livekit/rtc/graph_api.hpp"
#include "livekit/rtc/livekit.hpp"
#pragma once
#include "rtc_event_mailbox.hpp"
#include <livekit/rtc/input_node.hpp>
#include <livekit/rtc/engine.hpp>
#include <condition_variable>
#include <deque>
#include <set>

namespace transport = livekit::sdk::rtc;
using TrackGraph = transport::engine::TrackGraphSpec;

struct TestTrack {
  std::string id, sid, mime_type;
};

struct DecodedAudio {
  std::shared_ptr<const AudioFrame> frame;
};

// Test supervisor: all lifecycle operations run on its one engine thread.
// This is not an SDK facade; observations and test commands are explicit.
struct Peer {
  std::atomic<uint64_t> video_bytes{0}, video_stats_rows{0}, stats_samples{0};
  std::atomic<size_t> publications{0};
  std::atomic<bool> connected{false};

  Peer(const std::string& url, const std::string& identity, const std::string& codec,
       const std::filesystem::path& directory)
      : codec_(codec), events_(directory / (identity + ".jsonl")) {
    std::promise<void> started;
    auto ready = started.get_future();
    worker_ = std::jthread(
        [this, url, identity, started = std::move(started)](std::stop_token stop) mutable {
          bool announced = false;
          try {
            engine_ = require_result(livekit::sdk::app_support::create_blocking_engine());
            engine_events_.attach(*engine_);
            transport::LiveKitRoomOptions options;
            options.url = url;
            options.token = token(identity);
            options.media_transport = test_transport;
            options.stats_interval = 1s;
            options.reconnect.enabled = false;
            await(engine_->join(options));
            connected = true;
            started.set_value();
            announced = true;
            while (!stop.stop_requested()) {
              std::function<void()> command;
              {
                std::lock_guard lock(mutex_);
                if (!commands_.empty()) {
                  command = std::move(commands_.front());
                  commands_.pop_front();
                }
              }
              if (command)
                command();
              tick();
              std::this_thread::sleep_for(2ms);
            }
          } catch (...) {
            {
              std::lock_guard lock(mutex_);
              failure_ = std::current_exception();
            }
            if (!announced)
              started.set_exception(std::current_exception());
          }
          if (engine_)
            try {
              require_result(engine_->close());
            } catch (...) {
              std::lock_guard lock(mutex_);
              if (!failure_)
                failure_ = std::current_exception();
            }
          connected = false;
          {
            std::lock_guard lock(mutex_);
            finished_ = true;
          }
          audio_ready_.notify_all();
        });
    ready.get();
  }

  ~Peer() {
    stopping_ = true;
    worker_.request_stop();
    if (worker_.joinable())
      worker_.join();
  }

  VideoObservation observation(const std::string& sid) {
    std::lock_guard lock(mutex_);
    rethrow();
    return video_[sid];
  }

  bool has_demand(const std::string& sid, bool enabled) {
    std::lock_guard lock(mutex_);
    rethrow();
    const auto found = demand_.find(sid);
    return found != demand_.end() && found->second == enabled;
  }

  TestTrack remote(const std::string& who, const std::string& sid) {
    TestTrack result;
    until(
        [&] {
          std::lock_guard lock(mutex_);
          rethrow();
          for (const auto& [id, descriptor] : known_)
            if (descriptor.source_id == who && descriptor.remote_sid == sid) {
              result = {id, sid, mime(descriptor)};
              return true;
            }
          return false;
        },
        "missing remote publication: " + sid);
    return result;
  }

  bool subscribed(const std::string& id) {
    std::lock_guard lock(mutex_);
    rethrow();
    return active_.contains(id);
  }

  DecodedAudio read_audio(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    audio_ready_.wait_for(lock, timeout, [&] { return !audio_.empty() || finished_; });
    rethrow();
    if (audio_.empty())
      return {};
    auto frame = std::move(audio_.front());
    audio_.pop_front();
    return {std::move(frame)};
  }

  std::future<TestTrack> publish(MediaSpec spec, std::string name,
                                 transport::PublicationSource role) {
    return command<TestTrack>([this, spec = std::move(spec), name = std::move(name), role] {
      transport::TrackDescriptor d;
      d.id = "source-" + std::to_string(++sequence_);
      d.name = name;
      d.stream_group = "device-01";
      d.source = role;
      d.kind = spec.video ? transport::MediaKind::video : transport::MediaKind::audio;
      d.format.codec = !spec.video        ? transport::Codec::opus
                       : codec_ == "h264" ? transport::Codec::h264
                       : codec_ == "h265" ? transport::Codec::h265
                                          : transport::Codec::vp8;
      d.format.clock_rate = spec.video ? 90000 : 48000;
      d.format.channels = spec.video ? 0 : spec.audio_source->options().channels;
      d.format.width = spec.video ? spec.width : 0;
      d.format.height = spec.video ? spec.height : 0;
      d.follow_subscriber_demand = spec.dynacast;
      transport::nodes::MediaInputOptions input;
      input.video_source = spec.video_source;
      input.audio_source = spec.audio_source;
      input.fps = spec.fps;
      await(engine_->graph().publish_graph(d, [d, input](auto sink) {
        auto source = std::make_shared<transport::nodes::MediaInputNode>("input", d, input);
        const auto build = [&]<class Codec, class Event>(std::shared_ptr<Codec> codec) {
          TrackGraph graph;
          graph.nodes = {source, codec};
          graph.connections = {
              TrackGraph::connect<Event>(source, codec),
              TrackGraph::connect<transport::nodes::EncodedFrameEvent>(codec, sink),
              TrackGraph::connect<transport::nodes::SenderFeedbackEvent>(sink, codec),
              TrackGraph::connect<transport::nodes::SenderFeedbackEvent>(sink, source)};
          graph.activate = [source] { source->activate(); };
          graph.quiesce = [source, codec] {
            source->quiesce();
            codec->quiesce();
          };
          return graph;
        };
        if (d.kind == transport::MediaKind::video)
          return build.template
          operator()<transport::nodes::VideoCodecNode, transport::nodes::VideoFrameEvent>(
              std::make_shared<transport::nodes::VideoCodecNode>("encoder", d, true));
        return build.template
        operator()<transport::nodes::AudioCodecNode, transport::nodes::AudioFrameEvent>(
            std::make_shared<transport::nodes::AudioCodecNode>("encoder", d, true));
      }));
      TestTrack track{d.id, engine_->publication(d.id).server_sid, mime(d)};
      outgoing_[d.id] = track;
      publications = outgoing_.size();
      return track;
    });
  }

  std::future<void> unpublish(const std::string& id) {
    return command<void>([this, id] {
      require_result(engine_->unpublish(id));
      outgoing_.erase(id);
      publications = outgoing_.size();
    });
  }

  std::future<void> set_subscribed(const std::string& id, bool enabled) {
    return command<void>([this, id, enabled] {
      if (enabled) {
        transport::TrackDescriptor d;
        {
          std::lock_guard lock(mutex_);
          d = known_.at(id);
        }
        subscribe(d);
      } else {
        require_result(engine_->unsubscribe(id));
        std::lock_guard lock(mutex_);
        active_.erase(id);
      }
    });
  }

 private:
  class Observe final : public graphflow::core::GraphNode {
   public:
    Observe(Peer& peer, std::string sid)
        : GraphNode("observe", nullptr), peer_(peer), sid_(std::move(sid)) {}

   private:
    bool doInit() override {
      subscribe<transport::nodes::VideoFrameEvent>([this](const auto& event) {
        const auto& frame = *event.pixels;
        uint64_t sum = 0;
        for (int y = 0; y < frame.height; ++y)
          for (int x = 0; x < frame.width; ++x)
            sum += frame.y.at(y * frame.stride_y + x);
        std::lock_guard lock(peer_.mutex_);
        auto& observation = peer_.video_[sid_];
        ++observation.frames;
        observation.width = frame.width;
        observation.height = frame.height;
        observation.y = double(sum) / (frame.width * frame.height);
      });
      subscribe<transport::nodes::AudioFrameEvent>([this](const auto& event) {
        auto frame = std::make_shared<AudioFrame>();
        frame->track_id = sid_;
        frame->sample_rate = event.pcm->sample_rate;
        frame->channels = event.pcm->channels;
        frame->samples_per_channel = event.pcm->samples_per_channel();
        frame->sample_index = event.timestamp;
        frame->samples = event.pcm->samples;
        std::lock_guard lock(peer_.mutex_);
        if (peer_.audio_.size() == 500)
          peer_.audio_.pop_front();
        peer_.audio_.push_back(std::move(frame));
        peer_.audio_ready_.notify_one();
      });
      return true;
    }

    Peer& peer_;
    std::string sid_;
  };

  static std::string mime(const transport::TrackDescriptor& d) {
    return d.kind == transport::MediaKind::audio      ? "audio/opus"
           : d.format.codec == transport::Codec::vp8  ? "video/vp8"
           : d.format.codec == transport::Codec::h264 ? "video/h264"
                                                      : "video/h265";
  }

  template <class Future>
  auto await(Future future) {
    return require_result(std::move(future));
  }

  template <class T, class F>
  std::future<T> command(F operation) {
    auto promise = std::make_shared<std::promise<T>>();
    auto result = promise->get_future();
    std::lock_guard lock(mutex_);
    rethrow();
    require(!finished_, "Peer finished");
    commands_.push_back([promise, operation = std::move(operation)] {
      try {
        if constexpr (std::is_void_v<T>) {
          operation();
          promise->set_value();
        } else
          promise->set_value(operation());
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    });
    return result;
  }

  void rethrow() const {
    if (failure_)
      std::rethrow_exception(failure_);
  }

  void subscribe(const transport::TrackDescriptor& d) {
    require_result(engine_->graph().subscribe_graph(d, [this, d](auto source) {
      auto output = std::make_shared<Observe>(*this, d.remote_sid);
      const auto build = [&]<class Codec, class Event>(std::shared_ptr<Codec> codec) {
        TrackGraph graph;
        graph.nodes = {codec, output};
        graph.connections = {
            TrackGraph::connect<transport::nodes::EncodedFrameEvent>(source, codec),
            TrackGraph::connect<Event>(codec, output)};
        if constexpr (std::is_same_v<Event, transport::nodes::VideoFrameEvent>)
          graph.connections.push_back(
              TrackGraph::connect<transport::nodes::KeyframeRequestEvent>(codec, source));
        graph.quiesce = [codec] { codec->quiesce(); };
        return graph;
      };
      if (d.kind == transport::MediaKind::video)
        return build.template
        operator()<transport::nodes::VideoCodecNode, transport::nodes::VideoFrameEvent>(
            std::make_shared<transport::nodes::VideoCodecNode>("decoder", d, false));
      return build
          .template operator()<transport::nodes::AudioCodecNode, transport::nodes::AudioFrameEvent>(
              std::make_shared<transport::nodes::AudioCodecNode>("decoder", d, false));
    }));

    std::lock_guard lock(mutex_);
    active_.insert(d.id);
  }

  void tick() {
    if (auto failure = engine_events_.take_failure())
      throw std::runtime_error(failure->message);
    while (auto event = engine_events_.take_event(0ms)) {
      if (event->kind == transport::RoomEventKind::closed ||
          event->kind == transport::RoomEventKind::failed)
        throw std::runtime_error(event->message);
      if (event->kind == transport::RoomEventKind::remote_track_added && event->descriptor) {
        {
          std::lock_guard lock(mutex_);
          known_[event->track_id] = *event->descriptor;
        }
        subscribe(*event->descriptor);
      }
      if (event->kind == transport::RoomEventKind::remote_track_removed ||
          event->kind == transport::RoomEventKind::track_removed) {
        require_result(engine_->unsubscribe(event->track_id));
        std::lock_guard lock(mutex_);
        known_.erase(event->track_id);
        active_.erase(event->track_id);
      }
      if (event->kind == transport::RoomEventKind::statistics && event->statistics) {
        const auto& report = *event->statistics;
        auto rows = Json::parse(report.json);
        events_ << Json{{"event", "rtc-stats"}, {"target", report.target}, {"stats", rows}}.dump()
                << '\n';
        if (report.target == 0) {
          uint64_t bytes = 0, count = 0;
          for (const auto& row : rows)
            if (row.value("type", "") == "outbound-rtp" && row.value("kind", "") == "video") {
              bytes += row.value("bytesSent", uint64_t(0));
              ++count;
            }
          video_bytes = bytes;
          video_stats_rows = count;
          ++stats_samples;
        }
      }
    }
    for (const auto& [id, track] : outgoing_) {
      auto feedback = engine_->feedback(id);
      bool enabled = false;
      for (const auto& [layer, status] : feedback.encodings)
        enabled |= status.active;
      std::lock_guard lock(mutex_);
      demand_[track.sid] = enabled;
    }
  }

  std::string codec_;
  std::ofstream events_;
  std::mutex mutex_;
  std::condition_variable audio_ready_;
  std::deque<std::shared_ptr<const AudioFrame>> audio_;
  std::deque<std::function<void()>> commands_;
  std::map<std::string, VideoObservation> video_;
  std::map<std::string, bool> demand_;
  std::map<std::string, transport::TrackDescriptor> known_;
  std::set<std::string> active_;
  std::map<std::string, TestTrack> outgoing_;
  std::unique_ptr<livekit::sdk::app_support::BlockingRtc> engine_;
  livekit::sdk::app_support::RtcEventMailbox engine_events_;
  std::exception_ptr failure_;
  bool finished_ = false;
  uint64_t sequence_ = 0;
  std::atomic<bool> stopping_{false};
  std::jthread worker_;
};
