#include "watermark.hpp"
#include "backends/livekit/room_session.hpp"
#include "backends/livekit/track_state.hpp"
#include "rtc_video_fixtures.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::client;
using namespace std::chrono_literals;
namespace impl = livekit::sdk::client::detail;
namespace nodes = livekit::sdk::rtc::nodes;

namespace livekit::sdk::client::detail {
struct RoomTestAccess {
  static void use(Room& room, SessionFactory factory) {
    room.session_ = std::make_shared<RoomSession>(std::move(factory));
  }
};

struct MediaEngineTestAccess {
  static std::unique_ptr<MediaEngine> create(std::unique_ptr<Room> room) {
    return std::unique_ptr<MediaEngine>(new MediaEngine(std::move(room), {}));
  }
};
}  // namespace livekit::sdk::client::detail

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::abort();
  }
}

template <class F>
void until(F predicate, const char* message) {
  const auto end = SteadyClock::now() + 5s;
  while (!predicate()) {
    check(SteadyClock::now() < end, message);
    std::this_thread::sleep_for(2ms);
  }
}

struct Wire {
  bool audio = false;
  std::mutex mutex;
  std::weak_ptr<impl::TrackData> receiver;
  std::atomic<int> publications{0}, removals{0}, subscriptions{0}, sent{0}, keyframes{0};
};

class Session final : public impl::RtcSession {
 public:
  explicit Session(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

  Result<impl::JoinState> connect(const ConnectionOptions&, Epoch epoch,
                                  const impl::Operation&) override {
    epoch_ = epoch;
    impl::JoinState join;
    join.local.identity = "local";
    ParticipantSnapshot peer;
    peer.identity = "remote";
    peer.sid = "PA_remote";
    peer.state = 2;
    PublicationInfo info;
    info.sid = "TR_video";
    info.kind = wire_->audio ? PublicationKind::audio : PublicationKind::video;
    info.mime_type = wire_->audio ? "audio/opus" : "video/VP8";
    info.width = 64;
    info.height = 48;
    peer.publications[info.sid] = info;
    join.participants.push_back(peer);
    return Result<impl::JoinState>::success(std::move(join));
  }

  Result<void> poll(const impl::Operation&) override {
    return Result<void>::success();
  }

  void close() override {}

  Result<PublicationInfo> add_track(const impl::TrackConfig& config,
                                    const impl::Operation&) override {
    ++wire_->publications;
    PublicationInfo info;
    info.sid = "TR_" + config.id;
    info.kind = config.kind == MediaKind::audio ? PublicationKind::audio : PublicationKind::video;
    return Result<PublicationInfo>::success(info);
  }

  Result<void> create_sender(const TrackSid&, std::shared_ptr<impl::TrackData> track,
                             const impl::Operation&) override {
    std::lock_guard lock(track->mutex);
    track->feedback.encodings[""] = {500000, true, true, 1};
    track->send = [wire = wire_](const EncodedFrame& frame) {
      ++wire->sent;
      std::shared_ptr<impl::TrackData> receiver;
      {
        std::lock_guard lock(wire->mutex);
        receiver = wire->receiver.lock();
      }
      if (receiver) {
        receiver->receive(frame);
      }
      return WriteStatus::accepted;
    };
    return Result<void>::success();
  }

  Result<void> remove_track(const TrackSid&, const impl::Operation&) override {
    ++wire_->removals;
    return Result<void>::success();
  }

  Result<void> subscribe(const TrackSid& sid, std::shared_ptr<impl::TrackData> track, bool enabled,
                         const impl::Operation&) override {
    wire_->subscriptions += enabled ? 1 : -1;
    track->transition(enabled ? TrackState::ready : TrackState::suspended, epoch_, sid);
    std::lock_guard lock(wire_->mutex);
    wire_->receiver = enabled ? track : std::shared_ptr<impl::TrackData>{};
    return Result<void>::success();
  }

  Result<void> receive_options(const TrackSid&, ReceiveOptions) override {
    return Result<void>::success();
  }

  Result<void> mute(const TrackSid&, bool) override {
    return Result<void>::success();
  }

  Result<void> keyframe(const TrackSid&) override {
    ++wire_->keyframes;
    return Result<void>::success();
  }

  Result<void> publish_data(const DataPacket&, const impl::Operation&) override {
    return Result<void>::success();
  }

  Result<void> update(const ParticipantUpdate&, const impl::Operation&) override {
    return Result<void>::success();
  }

 private:
  std::shared_ptr<Wire> wire_;
  Epoch epoch_ = 0;
};

struct Fixture {
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  std::unique_ptr<MediaEngine> engine;

  explicit Fixture(bool audio = false) {
    wire->audio = audio;
    auto room = std::make_unique<Room>();
    impl::RoomTestAccess::use(*room, [wire = wire] { return std::make_unique<Session>(wire); });
    engine = impl::MediaEngineTestAccess::create(std::move(room));
    ConnectionOptions options;
    options.url = "ws://test.invalid";
    options.token = "test";
    options.auto_subscribe = false;
    check(engine->connect(options), "connect fake transport");
  }

  RemoteTrackPublication remote() {
    return engine->remote_participants().front().track_publications().front();
  }
};

MediaPublishSpec video() {
  MediaPublishSpec spec;
  spec.name = "graph";
  spec.format.codec = Codec::vp8;
  spec.format.width = 64;
  spec.format.height = 48;
  spec.input.generated = true;
  spec.input.fps = 15;
  return spec;
}

class EncodedSource final : public graphflow::core::GraphNode {
 public:
  explicit EncodedSource(std::string name = "source") : GraphNode(std::move(name), nullptr) {}

  std::atomic<int> emitted{0}, stopped{0}, released{0};
  Failure failure;
  bool fail_init = false, fail_start = false;

  void fail() {
    reportError({failure->message, failure});
  }

 protected:
  bool doInit() override {
    if (fail_init) {
      fail();
      return false;
    }
    subscribe<nodes::SenderFeedbackEvent>([this](const auto& event) {
      const auto encoding = event.feedback.encodings.find("");
      if (encoding == event.feedback.encodings.end() || !encoding->second.active) {
        return;
      }
      rtc::EncodedFrame frame;
      frame.buffer = rtc::EncodedBuffer(fixture_video(rtc::Codec::vp8, false));
      frame.timestamp = uint64_t(++emitted) * 6000;
      frame.duration = 66667us;
      frame.epoch = event.feedback.epoch;
      frame.config_revision = 1;
      frame.keyframe = true;
      frame.captured = SteadyClock::now();
      publish(nodes::EncodedFrameEvent(std::move(frame)));
    });
    return true;
  }

  bool doStart() override {
    return !fail_start;
  }

  void doStop() override {
    ++stopped;
  }

  void doRelease() override {
    ++released;
  }
};

std::shared_ptr<MediaGraphOptions> source_graph(const std::shared_ptr<EncodedSource>& source) {
  auto options = std::make_shared<MediaGraphOptions>();
  options->build = [source](MediaGraphContext context) {
    MediaGraph graph;
    graph.nodes = {context.defaults.nodes.front(), source};
    graph.connect<nodes::EncodedFrameEvent>(source->getName(), context.port);
    graph.connect<nodes::SenderFeedbackEvent>(context.port, source->getName());
    return Result<MediaGraph>::success(std::move(graph));
  };
  return options;
}

class EncodedSink final : public graphflow::core::GraphNode {
 public:
  EncodedSink() : GraphNode("sink", nullptr) {}

  std::atomic<int> received{0};

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      const auto expected = fixture_video(rtc::Codec::vp8, false);
      auto bytes = event.frame.buffer.bytes();
      check(std::vector<uint8_t>(bytes.begin(), bytes.end()) == expected,
            "custom subscription receives exact encoded bytes");
      if (++received == 1) {
        publish(nodes::KeyframeRequestEvent{});
      }
    });
    return true;
  }
};

void custom_loopback() {
  Fixture f;
  auto source = std::make_shared<EncodedSource>();
  auto sink = std::make_shared<EncodedSink>();
  MediaSubscribeSpec receive;
  receive.publication = f.remote();
  auto graph = std::make_shared<MediaGraphOptions>();
  graph->build = [sink](MediaGraphContext context) {
    MediaGraph value;
    value.nodes = {context.defaults.nodes.front(), sink};
    value.connect<nodes::EncodedFrameEvent>(context.port, "sink");
    value.connect<nodes::KeyframeRequestEvent>("sink", context.port);
    return Result<MediaGraph>::success(std::move(value));
  };
  receive.graph = graph;
  auto subscription = f.engine->subscribe_media(receive);
  check(bool(subscription), "custom subscribe");
  auto spec = video();
  spec.input.generated = false;  // Entire capture/codec chain is supplied by the custom graph.
  spec.graph = source_graph(source);
  auto publication = f.engine->publish_media(spec);
  check(bool(publication), "custom publish");
  until([&] { return sink->received >= 3 && f.wire->keyframes > 0; }, "encoded graph loopback");
  auto snapshot = publication->track->snapshot();
  check(snapshot.nodes.size() == 2 && snapshot.edges.size() == 2, "custom topology snapshot");
  auto reused = f.engine->publish_media(spec);
  check(!reused && reused.error().code == ErrorCode::invalid_argument,
        "active graph node cannot be reused");
  const auto previous = source->emitted.load();
  until([&] { return source->emitted > previous; }, "reuse rejection does not stop original graph");
  f.engine->set_muted(publication->publication.snapshot().track_id, true);
  std::this_thread::sleep_for(40ms);
  const auto muted = source->emitted.load();
  std::this_thread::sleep_for(40ms);
  check(source->emitted == muted, "custom source receives mute feedback");
  f.engine->set_muted(publication->publication.snapshot().track_id, false);
  until([&] { return source->emitted > muted; }, "custom source resumes");
  f.engine->close();
  const auto stopped = source->emitted.load();
  std::this_thread::sleep_for(30ms);
  check(source->emitted == stopped && source->stopped == 1 && source->released == 1,
        "close fences custom graph and releases once");
}

void watermark_loopback() {
  Fixture f;
  std::atomic<int> decoded{0}, preview{0};
  MediaSubscribeSpec receive;
  receive.publication = f.remote();
  receive.synchronize = false;
  receive.output.video = [&](const VideoSample& sample) {
    if (sample.pixels && sample.pixels->y.size() > 4 && sample.pixels->y[4] > 220) {
      ++decoded;
    }
    return Result<void>::success();
  };
  auto subscription = f.engine->subscribe_media(receive);
  check(bool(subscription), "decoder subscription");
  auto spec = video();
  spec.graph = examples::watermark_graph();
  spec.preview.video = [&](const VideoSample& sample) {
    check(sample.pixels && sample.pixels->y[4] == 235, "preview sees transformed pixels");
    ++preview;
    return Result<void>::success();
  };
  auto publication = f.engine->publish_media(spec);
  check(bool(publication), "watermark before encoder");
  until([&] { return preview >= 3 && decoded >= 3; }, "watermark survives real VP8 encode/decode");
  f.engine->close();
}

class SilenceNode final : public graphflow::core::GraphNode {
 public:
  SilenceNode() : GraphNode("silence", nullptr) {}

 protected:
  bool doInit() override {
    subscribe<nodes::AudioFrameEvent>([this](const auto& event) {
      auto pcm = std::make_shared<rtc::PcmAudioFrame>(*event.pcm);
      std::fill(pcm->samples.begin(), pcm->samples.end(), 0);
      std::fill(pcm->float_samples.begin(), pcm->float_samples.end(), 0.0f);
      auto output = event;
      output.pcm = std::move(pcm);
      publish(output);
    });
    return true;
  }
};

void audio_loopback() {
  Fixture f(true);
  std::atomic<int> decoded{0}, preview{0};
  MediaSubscribeSpec receive;
  receive.publication = f.remote();
  receive.synchronize = false;
  receive.audio.jitter_buffer = false;
  receive.output.audio = [&](const AudioSample& sample) {
    check(sample.pcm && !sample.pcm->samples.empty(), "decoded audio samples");
    if (std::all_of(sample.pcm->samples.begin(), sample.pcm->samples.end(),
                    [](int value) { return value >= -2 && value <= 2; })) {
      ++decoded;
    }
    return Result<void>::success();
  };
  check(bool(f.engine->subscribe_media(receive)), "audio subscription");
  MediaPublishSpec spec;
  spec.kind = MediaKind::audio;
  spec.format.codec = Codec::opus;
  spec.format.clock_rate = 48000;
  spec.format.channels = 1;
  spec.input.generated = true;
  auto options = std::make_shared<MediaGraphOptions>();
  options->build = [](MediaGraphContext context) {
    auto graph = std::move(context.defaults);
    const auto type = graphflow::core::detail::prettyName<nodes::AudioFrameEvent>();
    for (auto& edge : graph.connections) {
      if (edge.from == context.input && edge.dataType == type) {
        edge.from = "silence";
      }
    }
    graph.nodes.push_back(std::make_shared<SilenceNode>());
    graph.connect<nodes::AudioFrameEvent>(context.input, "silence");
    return Result<MediaGraph>::success(std::move(graph));
  };
  spec.graph = options;
  spec.preview.audio = [&](const AudioSample& sample) {
    check(sample.pcm && std::all_of(sample.pcm->samples.begin(), sample.pcm->samples.end(),
                                    [](int value) { return value == 0; }),
          "audio preview sees processed PCM");
    ++preview;
    return Result<void>::success();
  };
  check(bool(f.engine->publish_media(spec)), "audio custom processing graph");
  until([&] { return preview >= 3 && decoded >= 3; }, "processed PCM survives Opus roundtrip");
  f.engine->close();
}

void rejection_and_rollback() {
  Fixture f;
  for (int mode = 0; mode < 5; ++mode) {
    auto spec = video();
    auto options = std::make_shared<MediaGraphOptions>();
    options->build = [mode](MediaGraphContext context) {
      auto graph = std::move(context.defaults);
      if (mode == 0) {
        graph.nodes.erase(graph.nodes.begin());
      } else if (mode == 1) {
        graph.connections.front().to = "missing";
      } else if (mode == 2) {
        graph.connect<nodes::VideoFrameEvent>(context.codec, context.input);
      } else if (mode == 3) {
        graph.nodes.push_back(std::make_shared<EncodedSource>("isolated"));
      } else {
        graph.connections.front().dataType = "wrong-port-event";
      }
      return Result<MediaGraph>::success(std::move(graph));
    };
    spec.graph = options;
    auto result = f.engine->publish_media(spec);
    check(!result && result.error().code == ErrorCode::invalid_argument, "invalid graph rejected");
  }
  check(f.wire->publications == 0, "invalid graph rejected before Room publication");
  auto expected = make_failure(ErrorDomain::media, ErrorCode::processing_failed, "custom failure");
  for (int mode = 0; mode < 4; ++mode) {
    auto source = std::make_shared<EncodedSource>();
    source->failure = expected;
    source->fail_init = mode == 1;
    source->fail_start = mode == 2;
    auto spec = video();
    auto options = source_graph(source);
    std::atomic<int> quiesced{0};
    auto builder = options->build;
    options->build = [&, builder, mode](MediaGraphContext context) {
      // Synchronous factory controls must reject reentry instead of deadlocking.
      check(!f.engine->publish_media(video()), "builder reentry rejected");
      if (mode == 0) {
        return Result<MediaGraph>::failure(expected);
      }
      auto result = builder(std::move(context));
      result->quiesce = [&] { ++quiesced; };
      if (mode == 3) {
        result->activate = [] { return false; };
      }
      return result;
    };
    spec.graph = options;
    const auto published = f.wire->publications.load(), removed = f.wire->removals.load();
    auto result = f.engine->publish_media(spec);
    check(!result, "factory/init/start/activate failure returned synchronously");
    if (mode <= 1) {
      check(result.error().code == expected->code && result.error().message == expected->message,
            "precise custom failure preserved");
      check(f.wire->publications == published, "prepare failure creates no publication");
    } else {
      check(f.wire->publications == published + 1 && f.wire->removals == removed + 1,
            "start failure rolls back publication");
    }
    check(quiesced == (mode ? 1 : 0), "quiesce runs once only for adopted graphs");
  }
  auto source = std::make_shared<EncodedSource>();
  source->failure = expected;
  auto spec = video();
  spec.graph = source_graph(source);
  auto published = f.engine->publish_media(spec);
  check(bool(published), "runtime failure graph starts");
  source->fail();
  until([&] { return source->released == 1; }, "runtime failure stops graph");
  const auto failure = published->track->snapshot().failure;
  check(failure && failure->code == expected->code && failure->message == expected->message &&
            failure->context.node == source->getName(),
        "runtime error retained with node context in snapshot");
  f.engine->unpublish_track(published->publication.snapshot().track_id);
  MediaSubscribeSpec receive;
  receive.publication = f.remote();
  auto bad = std::make_shared<MediaGraphOptions>();
  bad->build = [expected](MediaGraphContext) { return Result<MediaGraph>::failure(expected); };
  receive.graph = bad;
  check(!f.engine->subscribe_media(receive) && f.wire->subscriptions == 0,
        "subscription factory failure rolls back Room subscription");
}
}  // namespace

int main() {
  custom_loopback();
  watermark_loopback();
  audio_loopback();
  rejection_and_rollback();
  std::cout << "PASS custom publish/subscribe graphs, watermark codecs, validation and lifecycle\n";
}
