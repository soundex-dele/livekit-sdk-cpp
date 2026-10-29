#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include "session.hpp"
#include "livekit/rtc/engine.hpp"
#include "media_nodes/render/video_playout.hpp"
#include "backends/webrtc/native_peer.hpp"
#include "api/video/i420_buffer.h"
#include <algorithm>
#include <thread>

namespace livekit::cli {
using namespace sdk::rtc;
using namespace std::chrono_literals;
using Json = sdk::Json;

namespace {
Codec codec(const std::string& value) {
  if (value == "vp8")
    return Codec::vp8;
  if (value == "h264")
    return Codec::h264;
  if (value == "h265")
    return Codec::h265;
  throw std::invalid_argument("Unsupported video codec: " + value);
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

  void video(const engine::VideoSample& event) {
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

struct Track {
  TrackDescriptor descriptor;
  bool publication = false, closing = false, reported = false, ready = false;
  std::future<TrackId> pending;
  std::shared_ptr<engine::MediaTrack> media;
  std::shared_ptr<Output> output;

  void check() const {
    if (media)
      media->throw_if_failed();
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

void doctor(std::function<void(const Json&)> report) {
  sdk::rtc::detail::TransportRuntime runtime;
  sdk::rtc::detail::NativePeer peer(runtime, {});
  peer.create_data_channel("doctor", true);
  auto offer = peer.create_local(true);
  if (offer.wait_for(10s) != std::future_status::ready)
    throw std::runtime_error("Native SDP creation timed out");
  const auto description = offer.get();
  report({{"event", "webrtc-smoke"},
          {"ok", true},
          {"sdpBytes", description.sdp.size()},
          {"transportOnly", true}});
  peer.close();
}

void run_session(const SessionOptions& options, std::function<void(const Json&)> report,
                 std::stop_token stop) {
  if (options.stats_interval_ms &&
      (options.stats_interval_ms < 200 || options.stats_interval_ms > 60000))
    throw std::invalid_argument("Stats interval must be 0 or 200..60000 ms");
  lk::EventEmitter display_events(report);
  std::map<TrackId, std::shared_ptr<Track>> tracks;
  auto display =
      options.video_playout ? std::make_shared<lk::VideoPlayout>(display_events) : nullptr;
  auto engine = engine::create_livekit_engine({});  // Destroy before consumers/display.
  livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
  LiveKitRoomOptions room;
  room.url = options.url;
  room.token = options.token;
  room.connect_timeout = options.connect_timeout;
  room.operation_timeout = std::chrono::seconds(options.connect_timeout);
  room.stats_interval = std::chrono::milliseconds(options.stats_interval_ms);
  // Subscriptions are explicit graph operations; server auto-subscribe stays off.
  auto joining = engine->join(room);
  while (joining.wait_for(0ms) != std::future_status::ready) {
    if (stop.stop_requested()) {
      engine->close().get();
      return;
    }

    std::this_thread::sleep_for(5ms);
  }
  joining.get();
  report({{"event", "joined"}, {"room", options.room}});
  const auto joined = engine->room().snapshot().session;
  if (!joined || joined->room.name != options.room)
    throw std::runtime_error("Joined room does not match the CLI room argument");
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
    d.format.codec = spec.video ? codec(options.codec) : Codec::opus;
    d.format.clock_rate = spec.video ? 90000 : 48000;
    d.format.channels = spec.video ? 0 : 1;
    d.format.width = spec.video ? spec.width : 0;
    d.format.height = spec.video ? spec.height : 0;
    d.stream_group = "input-" + std::to_string(index);
    engine::MediaInput input;
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
    engine::MediaPublishSpec config;
    config.track = d;
    config.input = input;
    auto operation = engine->publish_media(std::move(config));
    track->media = std::move(operation.track);
    track->pending = std::move(operation.ready);
    tracks.emplace(d.id, std::move(track));
  }
  std::optional<std::future<void>> sending;
  if (!options.publish_data.empty()) {
    DataSendOptions data;
    data.topic = options.topic;
    sending = engine->room().services()->send_data(
        {options.publish_data.begin(), options.publish_data.end()}, data);
  }
  const auto start = SteadyClock::now();
  auto next_stats = start;
  std::optional<SteadyClock::time_point> eof;
  bool announced = false;
  const auto subscribe = [&](TrackDescriptor d) {
    if (!options.subscribe || tracks.contains(d.id))
      return;
    if (!options.subscribe_tracks.empty() &&
        std::find(options.subscribe_tracks.begin(), options.subscribe_tracks.end(), d.remote_sid) ==
            options.subscribe_tracks.end())
      return;
    auto track = std::make_shared<Track>();
    track->descriptor = d;
    engine::MediaSubscribeSpec config;
    config.track = d;
    config.output.recording = options.record;
    if (d.kind == MediaKind::video && display) {
      track->output = std::make_shared<Output>(d.id, display);
      config.output.video = [output = track->output](const auto& frame) { output->video(frame); };
    }
    if (d.kind == MediaKind::audio)
      config.output.playout = options.audio_playout;
    auto operation = engine->subscribe_media(std::move(config));
    track->media = std::move(operation.track);
    track->pending = std::move(operation.ready);
    tracks.emplace(d.id, std::move(track));
  };
  try {
    while (!stop.stop_requested() &&
           (!options.duration ||
            SteadyClock::now() - start < std::chrono::seconds(options.duration))) {
      while (auto event = engine_events.take_event(0ms)) {
        if (event->kind == RoomEventKind::remote_track_added && event->descriptor)
          subscribe(*event->descriptor);
        else if (event->kind == RoomEventKind::track_removed) {
          auto found = tracks.find(event->track_id);
          if (found != tracks.end()) {
            found->second->closing = true;
            auto removing = found->second->publication ? engine->unpublish(found->first)
                                                       : engine->unsubscribe(found->first);
            removing.get();
          }
          report(
              {{"event", "track-removed"}, {"track", event->track_id}, {"reason", event->message}});
        } else if (event->kind == RoomEventKind::remote_track_removed) {
          auto found = tracks.find(event->track_id);
          if (found != tracks.end() && !found->second->publication) {
            found->second->closing = true;
            engine->unsubscribe(found->first).get();
          }
        } else if (event->kind == RoomEventKind::failed || event->kind == RoomEventKind::closed)
          throw std::runtime_error(event->message.empty() ? "Room disconnected" : event->message);
        else if (event->kind == RoomEventKind::reconnecting)
          report({{"event", "reconnecting"}, {"epoch", event->epoch}});
        else if (event->kind == RoomEventKind::track_mute_changed) {
          auto it = tracks.find(event->track_id);
          if (it != tracks.end() && it->second->media)
            report({{"event", "track-muted"},
                    {"track", it->second->media->snapshot().transport.server_sid},
                    {"muted", it->second->media->snapshot().transport.muted}});
        } else if (event->kind == RoomEventKind::statistics && event->statistics)
          report({{"event", "rtc-stats"},
                  {"target", event->statistics->target},
                  {"epoch", event->epoch},
                  {"timestampUs", event->statistics->timestamp_us},
                  {"stats", Json::parse(event->statistics->json)}});
      }

      while (auto failure = engine_events.take_failure()) {
        auto it = tracks.find(failure->id);
        if (it != tracks.end() && it->second->closing)
          continue;
        throw std::runtime_error("Track " + failure->id + ": " + failure->message);
      }
      bool publications_ready = true, inputs_finished = !media.empty();
      for (auto it = tracks.begin(); it != tracks.end();) {
        auto& track = *it->second;
        if (track.pending.valid() && track.pending.wait_for(0ms) == std::future_status::ready) {
          if (track.closing) {
            try {
              track.pending.get();
            } catch (...) {
            }
          } else {
            track.pending.get();
            track.ready = true;
            report({{"event", track.publication ? "published" : "subscribed"},
                    {"track", track.publication ? track.media->snapshot().transport.server_sid
                                                : track.descriptor.remote_sid}});
          }
        }
        if (track.publication) {
          publications_ready &= track.ready;
          inputs_finished &=
              track.media->snapshot().input && track.media->snapshot().input->finished;
        }
        if (track.closing) {
          track.check();
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
      if (sending && sending->wait_for(0ms) == std::future_status::ready) {
        sending->get();
        sending.reset();
        report({{"event", "data-sent"}});
      }
      while (auto data = engine->room().services()->next_event(0ms)) {
        if (const auto* value = std::get_if<ReceivedData>(&data->value))
          report({{"event", "data"},
                  {"identity", data->sender_identity},
                  {"topic", value->topic},
                  {"bytes", value->bytes}});
        if (const auto* error = std::get_if<DataServiceError>(&data->value))
          throw std::runtime_error(error->message);
      }
      if (options.exit_after_publish && inputs_finished) {
        if (!eof)
          eof = SteadyClock::now();
        if (SteadyClock::now() - *eof > 250ms)
          break;
      }
      if (display && !display->poll())
        break;
      display_events.dispatch();
      display_events.rethrow();
      if (options.stats_interval_ms && SteadyClock::now() >= next_stats) {
        for (const auto& [id, track] : tracks)
          if (track->ready)
            report(track->metrics());
        next_stats = SteadyClock::now() + std::chrono::milliseconds(options.stats_interval_ms);
      }
      std::this_thread::sleep_for(5ms);
    }
    engine->close().get();
    for (const auto& [id, track] : tracks) {
      track->check();
      report(track->summary());
      if (track->output)
        track->output->close();
    }
    display.reset();
    display_events.dispatch();
    display_events.rethrow();
  } catch (...) {
    engine->close().get();
    throw;
  }
}
}  // namespace livekit::cli
