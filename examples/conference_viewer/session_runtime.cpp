#include "session.hpp"
#include <algorithm>
#include <deque>

namespace demo {
namespace {
using namespace rtc;
using namespace std::chrono_literals;
using Json = sdk::Json;

struct Events final : RoomObserver {
  std::mutex mutex;
  sdk::Failure failure;

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::connection_changed &&
        event.connection == ConnectionState::failed) {
      std::lock_guard lock(mutex);
      failure = event.failure;
    }
  }
};

struct Track {
  bool local = false;
  MediaKind kind = MediaKind::video;
  std::shared_ptr<MediaTrack> media;
  LocalTrackPublication publication;
  std::map<std::string, uint64_t> metric_ids;

  std::string sid() const {
    return media->snapshot().transport.server_sid;
  }

  void metrics(Json& report, uint64_t& serial, const std::string& id) {
    const auto value = media->snapshot();
    for (const auto& node : value.nodes) {
      auto [it, added] = metric_ids.try_emplace(node.name, serial);
      if (added) {
        ++serial;
      }
      report["nodes"].push_back({{"id", it->second},
                                 {"name", id + "/" + node.name},
                                 {"trackId", sid()},
                                 {"kind", node.kind},
                                 {"generation", value.transport.epoch},
                                 {"outputs", node.outputs}});
    }
    for (const auto& edge : value.edges) {
      report["edges"].push_back({{"from", id + "/" + edge.from},
                                 {"to", id + "/" + edge.to},
                                 {"dataType", edge.data_type}});
    }
  }
};

Participant participant(const ParticipantSnapshot& value) {
  Participant result{value.sid, value.identity, value.name, value.state};
  for (const auto& [sid, track] : value.publications) {
    result.tracks.push_back(track);
  }
  return result;
}
}  // namespace

Session::Session()
    : Session([] {
        return sdk::Result<std::unique_ptr<MediaEngine>>::success(std::make_unique<MediaEngine>());
      }) {}

Session::Session(EngineFactory factory) : engine_factory_(std::move(factory)) {}

Session::~Session() {
  stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

sdk::Result<void> Session::start(SessionOptions options, bool pause_unused_video) {
  if (std::count_if(options.media.begin(), options.media.end(),
                    [](const auto& m) { return m.video; }) > max_test_videos) {
    return sdk::Result<void>::failure(sdk::make_failure(sdk::ErrorDomain::lifecycle,
                                                        sdk::ErrorCode::invalid_argument,
                                                        "Demo supports up to 8 test videos"));
  }
  {
    std::lock_guard lock(mutex_);
    if (!data_.finished) {
      return sdk::Result<void>::failure(sdk::make_failure(
          sdk::ErrorDomain::lifecycle, sdk::ErrorCode::invalid_state, "Session already active"));
    }
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  {
    std::lock_guard lock(mutex_);
    data_ = {};
    video_command_.reset();
    data_.state = State::Connecting;
    data_.finished = false;
    data_.room = options.room;
    data_.pause_unused_video = pause_unused_video;
  }
  worker_ = livekit::compat::jthread([this, options = std::move(options), pause_unused_video](
                                         livekit::compat::stop_token stop) mutable {
    auto run = [&]() -> std::string {
      Events events;
      auto created = engine_factory_();
      if (!created) {
        return created.error().message;
      }
      auto engine = std::move(*created);
      if (!engine) {
        return "Engine factory returned null";
      }
      engine->set_observer(&events);
      livekit::compat::stop_callback cancel(stop, [&] { engine->close(); });
      options.connection.auto_subscribe = false;
      if (!engine->connect(options.connection)) {
        std::lock_guard lock(events.mutex);
        return events.failure ? events.failure->message : "Room connection failed";
      }
      if (!options.room.empty() && engine->info().name != options.room) {
        return "Joined room does not match the requested room";
      }
      std::map<TrackId, Track> tracks;
      int video_number = 0;
      uint64_t metric_number = 1;
      auto publish = [&](const sdk::MediaSpec& spec) -> std::string {
        MediaPublishSpec config;
        config.name = spec.video ? "Test video " + std::to_string(++video_number) : "Test tone";
        config.kind = spec.video ? MediaKind::video : MediaKind::audio;
        config.format = {
            spec.video ? Codec::vp8 : Codec::opus,  spec.video ? 90000u : 48000u,
            uint16_t(spec.video ? 0 : 1),           uint32_t(spec.video ? spec.width : 0),
            uint32_t(spec.video ? spec.height : 0), {}};
        config.options.follow_subscriber_demand = spec.video && pause_unused_video;
        auto& input = config.input;
        input.generated = spec.demo;
        input.input = spec.input;
        input.ffmpeg = spec.ffmpeg;
        input.loop = spec.loop;
        input.fps = spec.fps;
        input.capture = spec.capture;
        input.device_index = spec.device_index;
        input.screen_id = spec.screen_id;
        input.video_source = spec.video_source;
        input.audio_source = spec.audio_source;
        input.encoded_source = spec.encoded_source;
        auto result = engine->publish_media(std::move(config));
        if (!result) {
          return result.error().message;
        }
        tracks.emplace(result->publication.snapshot().track_id,
                       Track{true,
                             spec.video ? MediaKind::video : MediaKind::audio,
                             result->track,
                             result->publication,
                             {}});
        return {};
      };
      for (const auto& source : options.media) {
        if (auto error = publish(source); !error.empty()) {
          return error;
        }
      }
      auto next_stats = SteadyClock::now();
      while (!stop.stop_requested()) {
        const auto state = engine->connection_state();
        if (state == ConnectionState::failed || state == ConnectionState::closed) {
          std::lock_guard lock(events.mutex);
          return events.failure ? events.failure->message
                                : "Disconnected by server. Join again to reconnect.";
        }
        if (state != ConnectionState::connected) {
          {
            std::lock_guard lock(mutex_);
            data_.frames.clear();
            data_.video_stalls.clear();
            data_.subscriptions.clear();
            data_.quality = {};
            if (data_.state != State::Disconnecting) {
              data_.state = State::Connecting;
            }
          }
          std::this_thread::sleep_for(5ms);
          continue;
        }
        std::set<TrackSid> advertised;
        for (const auto& peer : engine->remote_participants()) {
          for (auto publication : peer.track_publications()) {
            auto info = publication.snapshot();
            advertised.insert(info.sid);
            if (tracks.count(info.sid)) {
              continue;
            }
            MediaSubscribeSpec config;
            config.publication = publication;
            config.output.recording = options.record;
            if (info.kind == MediaKind::video) {
              config.output.video = [this, sid = info.sid](const auto& frame) {
                std::lock_guard lock(mutex_);
                if (data_.state != State::Disconnecting) {
                  data_.frames[sid] = frame.pixels;
                  if (auto it = data_.video_stalls.find(sid);
                      it != data_.video_stalls.end() && frame.pixels) {
                    it->second.frame(frame.pixels->stream_generation);
                  }
                  ++data_.received_frames;
                }
                return sdk::Result<void>::success();
              };
            } else {
              config.output.playout = options.audio_playout;
            }
            auto result = engine->subscribe_media(std::move(config));
            if (!result) {
              if (publication.snapshot().state == TrackState::closed ||
                  engine->connection_state() != ConnectionState::connected) {
                continue;
              }
              return result.error().message;
            }
            tracks.emplace(info.sid, Track{false, info.kind, result->track, {}, {}});
          }
        }
        for (auto it = tracks.begin(); it != tracks.end();) {
          const auto value = it->second.media->snapshot();
          if (value.failure) {
            return value.failure->message;
          }
          if ((!it->second.local && !advertised.count(it->first)) ||
              value.transport.state == TrackState::closed) {
            if (it->second.local) {
              engine->unpublish_track(it->first);
            } else {
              engine->unsubscribe_track(it->first);
            }
            {
              std::lock_guard lock(mutex_);
              data_.frames.erase(it->second.sid());
            }
            it = tracks.erase(it);
          } else {
            ++it;
          }
        }
        std::optional<VideoCommand> command;
        {
          std::lock_guard lock(mutex_);
          command.swap(video_command_);
        }
        if (command) {
          std::string error;
          if (command->remove_sid.empty()) {
            sdk::MediaSpec spec;
            spec.demo = true;
            error = publish(spec);
          } else {
            auto found = std::find_if(tracks.begin(), tracks.end(), [&](const auto& item) {
              return item.second.local && item.second.sid() == command->remove_sid;
            });
            if (found == tracks.end()) {
              error = "Publication no longer exists";
            } else {
              engine->unpublish_track(found->first);
              tracks.erase(found);
            }
          }
          if (!options.connection.token.empty()) {
            for (size_t pos = 0;
                 (pos = error.find(options.connection.token, pos)) != std::string::npos;
                 pos += 10) {
              error.replace(pos, options.connection.token.size(), "[redacted]");
            }
          }
          std::lock_guard lock(mutex_);
          data_.changing_video = false;
          data_.video_error = std::move(error);
        }
        auto local = engine->local_participant().snapshot();
        std::map<std::string, int> qualities;
        if (local.connection_quality) {
          qualities[local.sid] = *local.connection_quality;
        }
        for (const auto& peer : engine->remote_participants()) {
          auto snapshot = peer.snapshot();
          if (snapshot.connection_quality) {
            qualities[snapshot.sid] = *snapshot.connection_quality;
          }
        }
        std::vector<Participant> remote;
        for (const auto& peer : engine->remote_participants()) {
          remote.push_back(participant(peer.snapshot()));
        }
        {
          std::lock_guard lock(mutex_);
          data_.quality.participants = std::move(qualities);
          data_.subscriptions.clear();
          for (const auto& [id, track] : tracks) {
            if (!track.local && track.media->snapshot().transport.state == TrackState::ready) {
              data_.subscriptions.insert(track.sid());
            }
          }
          data_.update(participant(local), remote);
          data_.room = engine->info().name;
          if (data_.state != State::Disconnecting) {
            data_.state = State::Connected;
          }
        }
        if (SteadyClock::now() >= next_stats) {
          auto report = [&](const Json& value) -> std::string {
            {
              std::lock_guard lock(mutex_);
              data_.quality.update(value);
            }
            if (options.log_sink) {
              auto result = options.log_sink(value);
              if (!result) {
                return result.error().message;
              }
            }
            return {};
          };
          auto stats = engine->statistics();
          if (!stats) {
            if (engine->connection_state() == ConnectionState::connected) {
              return stats.error().message;
            }
          } else {
            for (const auto& value : *stats) {
              Json mapping = Json::array();
              for (const auto& [ssrc, sid] : value.received_tracks) {
                mapping.push_back({{"track", sid}, {"ssrcs", Json::array({ssrc})}});
              }
              if (auto error = report({{"event", "graph-track-stats"},
                                       {"target", value.target},
                                       {"tracks", std::move(mapping)}});
                  !error.empty()) {
                return error;
              }
              auto json = Json::parse(value.json, nullptr, false);
              if (json.is_discarded()) {
                return "Invalid transport statistics";
              }
              auto error = report({{"event", "rtc-stats"},
                                   {"target", value.target},
                                   {"timestampUs", value.timestamp_us},
                                   {"stats", std::move(json)},
                                   {"localTracks", value.publication_tracks}});
              if (!error.empty()) {
                return error;
              }
            }
          }
          Json graph{{"event", "media-graph-stats"},
                     {"timestampUs", std::chrono::duration_cast<std::chrono::microseconds>(
                                         SteadyClock::now().time_since_epoch())
                                         .count()},
                     {"nodes", Json::array()},
                     {"edges", Json::array()}};
          for (auto& [id, track] : tracks) {
            track.metrics(graph, metric_number, id);
            if (track.local && track.kind == MediaKind::video) {
              auto feedback = track.publication.track().feedback();
              bool enabled = std::any_of(feedback.encodings.begin(), feedback.encodings.end(),
                                         [](const auto& item) { return item.second.active; });
              bool changed;
              {
                std::lock_guard lock(mutex_);
                auto [it, added] = data_.video_demand.try_emplace(track.sid(), enabled);
                changed = added || it->second != enabled;
                it->second = enabled;
              }
              if (changed) {
                if (auto error = report(
                        {{"event", "source-demand"}, {"sid", track.sid()}, {"enabled", enabled}});
                    !error.empty()) {
                  return error;
                }
              }
            }
          }
          if (auto error = report(graph); !error.empty()) {
            return error;
          }
          next_stats = SteadyClock::now() + 1s;
        }
        std::this_thread::sleep_for(5ms);
      }
      engine->close();
      for (const auto& [id, track] : tracks) {
        if (auto error = track.media->snapshot().failure) {
          return error->message;
        }
      }
      return {};
    };
    std::string error = run();
    if (!options.connection.token.empty()) {
      for (size_t pos = 0; (pos = error.find(options.connection.token, pos)) != std::string::npos;
           pos += 10) {
        error.replace(pos, options.connection.token.size(), "[redacted]");
      }
    }
    std::lock_guard lock(mutex_);
    if (stop.stop_requested()) {
      error.clear();
    }
    data_ = {};
    data_.state = error.empty() ? State::Idle : State::Error;
    data_.error = std::move(error);
    video_command_.reset();
  });
  return sdk::Result<void>::success();
}

void Session::stop() {
  {
    std::lock_guard lock(mutex_);
    if (data_.finished) {
      return;
    }
    data_.state = State::Disconnecting;
    const auto now = VideoStalls::Clock::now();
    for (auto& [sid, stalls] : data_.video_stalls) {
      stalls.set_active(false, now);
    }
  }
  worker_.request_stop();
}
}  // namespace demo
