#include "livekit/compat/thread.hpp"
#include "session.hpp"
#include "livekit/media_engine.hpp"
#include "media_nodes/render/video_playout.hpp"
#include "backends/livekit/peer_transport.hpp"
#include "api/video/i420_buffer.h"
#include <algorithm>
#include <thread>
#include <set>
#include <mutex>

namespace livekit::cli {
using namespace sdk::client;
using sdk::client::Codec;
using sdk::client::DataPacket;
using namespace std::chrono_literals;
using Json = sdk::Json;
using sdk::ErrorCode;
using sdk::ErrorDomain;
using sdk::make_failure;
using sdk::Result;

namespace {
Result<void> operation_result(ErrorCode code) {
  return code == ErrorCode::ok ? Result<void>::success()
                               : Result<void>::failure(make_failure(ErrorDomain::lifecycle, code,
                                                                    "CLI RTC operation failed"));
}

Result<void> operation_result(Result<void> result) {
  return result;
}
}  // namespace

#define LK_CLI_TRY(expression)                                \
  do {                                                        \
    auto checked = operation_result(expression);              \
    if (!checked) {                                           \
      return Result<void>::failure(checked.failure_record()); \
    }                                                         \
  } while (false)
#define LK_CLI_ASSIGN(name, expression)                           \
  auto name##_result = (expression);                              \
  if (!name##_result) {                                           \
    return Result<void>::failure(name##_result.failure_record()); \
  }                                                               \
  auto name = std::move(*name##_result)

namespace {
Result<Codec> codec(const std::string& value) {
  if (value == "vp8")
    return Result<Codec>::success(Codec::vp8);
  if (value == "h264")
    return Result<Codec>::success(Codec::h264);
  if (value == "h265")
    return Result<Codec>::success(Codec::h265);
  return Result<Codec>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                             "Unsupported video codec: " + value));
}

class Output final {
 public:
  Output(TrackId id, std::shared_ptr<lk::VideoPlayout> display)
      : id_(std::move(id)), display_(std::move(display)) {
    if (display_)
      display_->add_track(id_);
  }

  ~Output() {
    close();
  }

  void close() {
    if (display_) {
      display_->remove_track(id_);
      display_.reset();
    }
  }

  void video(const VideoSample& event) {
    if (!display_)
      return;
    const auto& p = *event.pixels;
    auto buffer = webrtc::I420Buffer::Copy(p.width, p.height, p.y.data(), p.stride_y, p.u.data(),
                                           p.stride_u, p.v.data(), p.stride_v);
    display_->submit(id_, webrtc::VideoFrame::Builder()
                              .set_video_frame_buffer(buffer)
                              .set_timestamp_us(p.timestamp_us)
                              .build());
  }

 private:
  TrackId id_;
  std::shared_ptr<lk::VideoPlayout> display_;
};

// CLI-only display/publication metadata; not a transport API.
struct TrackDescriptor {
  std::string id, name, remote_sid, stream_group;
  MediaKind kind = MediaKind::video;
  CodecFormat format;
  PublicationSource source = PublicationSource::unknown;
};

class Inbox final : public RoomObserver {
 public:
  void on_room_event(const RoomEvent& event) override {
    std::lock_guard lock(mutex);
    if (events.size() >= 4096) {
      overflow = true;
    } else {
      events.push_back(event);
    }
  }

  Result<std::vector<RoomEvent>> take() {
    std::lock_guard lock(mutex);
    if (overflow) {
      return Result<std::vector<RoomEvent>>::failure(make_failure(
          ErrorDomain::lifecycle, ErrorCode::resource_exhausted, "CLI event queue overflow"));
    }
    std::vector<RoomEvent> result;
    result.swap(events);
    return Result<std::vector<RoomEvent>>::success(std::move(result));
  }

 private:
  std::mutex mutex;
  std::vector<RoomEvent> events;
  bool overflow = false;
};

struct Track {
  TrackDescriptor descriptor;
  bool publication = false, closing = false, reported = false, ready = false;
  std::shared_ptr<MediaTrack> media;
  std::shared_ptr<Output> output;

  Result<void> check() const {
    if (!media || !media->snapshot().failure) {
      return Result<void>::success();
    }
    const auto snapshot = media->snapshot();
    return Result<void>::failure(
        snapshot.failure
            ? snapshot.failure
            : make_failure(ErrorDomain::media, ErrorCode::processing_failed, "Media track failed"));
  }

  Json summary() const {
    const auto state = media->snapshot();
    return {{"event", publication ? "publish-summary" : "track-summary"},
            {"track", descriptor.remote_sid.empty() ? descriptor.id : descriptor.remote_sid},
            {"kind", descriptor.kind == MediaKind::video ? "video" : "audio"},
            {"frames", state.frames},
            {"bytes", state.bytes}};
  }

  Json metrics() const {
    Json list = Json::array(), edges = Json::array();
    const auto state = media->snapshot();
    for (const auto& node : state.nodes)
      list.push_back({{"name", node.name},
                      {"kind", node.kind},
                      {"inputs", node.inputs},
                      {"outputs", node.outputs},
                      {"dropped", node.dropped},
                      {"errors", node.errors}});
    for (const auto& edge : state.edges)
      edges.push_back({{"from", edge.from}, {"to", edge.to}, {"dataType", edge.data_type}});
    return {{"event", "media-graph-stats"},
            {"track", descriptor.id},
            {"publication", publication},
            {"nodes", list},
            {"edges", edges}};
  }
};
}  // namespace

Result<void> doctor(std::function<void(const Json&)> report) {
  sdk::client::detail::WebRtcContext runtime;
  LK_CLI_TRY(runtime.status());
  sdk::client::detail::PeerTransport peer(runtime, {}, 64, sdk::MediaTransport::automatic);
  LK_CLI_TRY(peer.status());
  LK_CLI_TRY(peer.create_data_channels());
  std::atomic<bool> stopped{false};
  sdk::client::detail::Operation operation{SteadyClock::now() + 10s, &stopped};
  LK_CLI_ASSIGN(description, peer.local_description(true, operation));
  report({{"event", "webrtc-smoke"},
          {"ok", true},
          {"sdpBytes", description.size()},
          {"transportOnly", true}});
  peer.close();
  return Result<void>::success();
}

Result<void> run_session(const SessionOptions& options, std::function<void(const Json&)> report,
                         livekit::compat::stop_token stop) {
  if (options.stats_interval_ms &&
      (options.stats_interval_ms < 200 || options.stats_interval_ms > 60000))
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                                              "Stats interval must be 0 or 200..60000 ms"));
  lk::EventEmitter display_events(report);
  std::map<TrackId, std::shared_ptr<Track>> tracks;
  auto display =
      options.video_playout ? std::make_shared<lk::VideoPlayout>(display_events) : nullptr;
  if (display)
    LK_CLI_TRY(display->status());
  Inbox inbox;
  auto engine = std::make_unique<MediaEngine>();
  engine->set_observer(&inbox);

  struct CloseEngine {
    decltype(engine)& value;

    ~CloseEngine() {
      if (value) {
        (void)value->close();
      }
    }
  } cleanup{engine};

  ConnectionOptions room;
  room.url = options.url;
  room.token = options.token;
  room.connect_timeout = options.connect_timeout;
  room.operation_timeout = std::chrono::seconds(options.connect_timeout);
  room.auto_subscribe = false;
  // Subscriptions are explicit graph operations; server auto-subscribe stays off.
  livekit::compat::stop_callback cancel(stop, [&] { engine->close(); });
  if (!engine->connect(room)) {
    if (stop.stop_requested()) {
      return Result<void>::success();
    }
    return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::connection_lost,
                                              "CLI room connection failed"));
  }
  if (stop.stop_requested()) {
    engine->close();
    return Result<void>::success();
  }
  report({{"event", "joined"}, {"room", options.room}});
  const auto joined = engine->info();
  if (joined.name != options.room)
    return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::invalid_state,
                                              "Joined room does not match the CLI room argument"));
  LK_CLI_ASSIGN(video_codec, codec(options.codec));
  auto media = options.media;
  if (options.microphone) {
    sdk::MediaSpec mic;
    mic.video = false;
    media.push_back(mic);
  }
  for (size_t index = 0; index < media.size(); ++index) {
    const auto& spec = media[index];
    auto track = std::make_shared<Track>();
    track->publication = true;
    auto& d = track->descriptor;
    d.id = d.name = "publish-" + std::to_string(index);
    d.kind = spec.video ? MediaKind::video : MediaKind::audio;
    d.format.codec = spec.video ? video_codec : Codec::opus;
    d.format.clock_rate = spec.video ? 90000 : 48000;
    d.format.channels = spec.video ? 0 : 1;
    d.format.width = spec.video ? spec.width : 0;
    d.format.height = spec.video ? spec.height : 0;
    d.stream_group = "input-" + std::to_string(index);
    MediaInput input;
    input.generated = spec.demo;
    input.input = spec.input;
    input.ffmpeg = spec.ffmpeg;
    input.loop = spec.loop;
    input.capture = spec.capture;
    input.device_index = spec.device_index;
    input.screen_id = spec.screen_id;
    input.fps = spec.fps;
    if (options.microphone && index + 1 == media.size()) {
      input.microphone = sdk::MicrophoneOptions{};
      d.source = PublicationSource::microphone;
    } else if (spec.capture == sdk::CaptureKind::camera)
      d.source = PublicationSource::camera;
    else if (spec.capture == sdk::CaptureKind::screen)
      d.source = PublicationSource::screen;
    MediaPublishSpec config;
    config.name = d.name;
    config.kind = d.kind;
    config.format = d.format;
    config.options.source = d.source;
    config.options.stream_group = d.stream_group;
    config.input = input;
    LK_CLI_ASSIGN(operation, engine->publish_media(std::move(config)));
    d.id = operation.publication.snapshot().track_id;
    d.remote_sid = operation.publication.snapshot().sid;
    track->media = std::move(operation.track);
    track->ready = true;
    tracks.emplace(d.id, std::move(track));
  }
  if (!options.publish_data.empty()) {
    DataPacket data;
    data.topic = options.topic;
    data.payload.assign(options.publish_data.begin(), options.publish_data.end());
    engine->publish_data(std::move(data));
    report({{"event", "data-sent"}, {"localSubmission", true}});
  }
  const auto start = SteadyClock::now();
  auto next_stats = start;
  std::optional<SteadyClock::time_point> eof;
  bool announced = false;
  const auto subscribe = [&](RemoteTrackPublication publication) -> Result<void> {
    const auto state = publication.snapshot();
    TrackDescriptor d;
    d.id = d.remote_sid = state.sid;
    d.kind = state.kind;
    d.name = state.name;
    if (!options.subscribe || tracks.count(d.id))
      return Result<void>::success();
    if (!options.subscribe_tracks.empty() &&
        std::find(options.subscribe_tracks.begin(), options.subscribe_tracks.end(), d.remote_sid) ==
            options.subscribe_tracks.end())
      return Result<void>::success();
    auto track = std::make_shared<Track>();
    track->descriptor = d;
    MediaSubscribeSpec config;
    config.publication = publication;
    config.output.recording = options.record;
    if (d.kind == MediaKind::video && display) {
      track->output = std::make_shared<Output>(d.id, display);
      config.output.video = [output = track->output](const auto& frame) {
        output->video(frame);
        return Result<void>::success();
      };
    }
    if (d.kind == MediaKind::audio)
      config.output.playout = options.audio_playout;
    LK_CLI_ASSIGN(operation, engine->subscribe_media(std::move(config)));
    track->media = std::move(operation.track);
    track->ready = true;
    tracks.emplace(d.id, std::move(track));
    return Result<void>::success();
  };
  {
    while (!stop.stop_requested() &&
           (!options.duration ||
            SteadyClock::now() - start < std::chrono::seconds(options.duration))) {
      LK_CLI_ASSIGN(events, inbox.take());
      for (const auto& event : events) {
        if (event.kind == RoomEventKind::data_received && event.data) {
          report({{"event", "data"},
                  {"identity", event.participant_identity},
                  {"topic", event.data->topic},
                  {"bytes", event.data->payload}});
        } else if (event.kind == RoomEventKind::connection_changed) {
          if (event.connection == ConnectionState::failed ||
              event.connection == ConnectionState::closed) {
            if (stop.stop_requested()) {
              break;
            }
            return Result<void>::failure(event.failure ? event.failure
                                                       : make_failure(ErrorDomain::transport,
                                                                      ErrorCode::connection_lost,
                                                                      "Room disconnected"));
          }
          if (event.connection == ConnectionState::reconnecting) {
            report({{"event", "reconnecting"}, {"epoch", event.epoch}});
          }
        } else if (event.kind == RoomEventKind::track_muted && event.publication) {
          report({{"event", "track-muted"},
                  {"track", event.publication->sid},
                  {"muted", event.publication->muted}});
        }
      }
      std::set<TrackSid> available;
      if (engine->connection_state() == ConnectionState::connected) {
        for (const auto& participant : engine->remote_participants()) {
          for (const auto& pub : participant.track_publications()) {
            available.insert(pub.snapshot().sid);
            LK_CLI_TRY(subscribe(pub));
          }
        }
        for (const auto& [id, track] : tracks) {
          if (!track->publication && !available.count(id)) {
            engine->unsubscribe_track(id);
            track->closing = true;
          }
        }
      }
      bool publications_ready = true, inputs_finished = !media.empty();
      for (auto it = tracks.begin(); it != tracks.end();) {
        auto& track = *it->second;
        LK_CLI_TRY(track.check());
        if (track.publication) {
          publications_ready &= track.ready;
          inputs_finished &=
              track.media->snapshot().input && track.media->snapshot().input->finished;
        }
        if (track.closing) {
          LK_CLI_TRY(track.check());
          report(track.summary());
          if (track.output)
            track.output->close();
          it = tracks.erase(it);
        } else
          ++it;
      }
      if (publications_ready && !announced) {
        report({{"event", "connected"}, {"room", options.room}});
        announced = true;
      }
      if (options.exit_after_publish && inputs_finished) {
        if (!eof)
          eof = SteadyClock::now();
        if (SteadyClock::now() - *eof > 250ms)
          break;
      }
      if (display) {
        LK_CLI_ASSIGN(visible, display->poll());
        if (!visible)
          break;
      }
      display_events.dispatch();
      LK_CLI_TRY(display_events.status());
      if (options.stats_interval_ms && SteadyClock::now() >= next_stats &&
          engine->connection_state() == ConnectionState::connected) {
        LK_CLI_ASSIGN(reports, engine->statistics());
        for (const auto& stats : reports) {
          const auto native = Json::parse(stats.json, nullptr, false);
          Json selected = Json::array();
          if (native.is_array()) {
            for (const auto& entry : native) {
              const auto type = entry.value("type", "");
              if (type == "outbound-rtp" || type == "inbound-rtp" || type == "remote-inbound-rtp" ||
                  type == "media-source" || type == "candidate-pair" || type == "transport") {
                selected.push_back(entry);
              }
            }
          }
          report({{"event", "rtc-stats"},
                  {"target", stats.target},
                  {"timestampUs", stats.timestamp_us},
                  {"stats", std::move(selected)}});
        }
        for (const auto& [id, track] : tracks)
          if (track->ready)
            report(track->metrics());
        next_stats = SteadyClock::now() + std::chrono::milliseconds(options.stats_interval_ms);
      }
      std::this_thread::sleep_for(5ms);
    }
    engine->close();
    for (const auto& [id, track] : tracks) {
      LK_CLI_TRY(track->check());
      report(track->summary());
      if (track->output)
        track->output->close();
    }
    display.reset();
    display_events.dispatch();
    LK_CLI_TRY(display_events.status());
  }
  return Result<void>::success();
}
}  // namespace livekit::cli
