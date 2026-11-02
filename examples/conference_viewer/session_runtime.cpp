#include "livekit/compat/thread.hpp"
#include "session.hpp"
#include <livekit/rtc/engine.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <variant>

namespace demo {
namespace {
using namespace rtc;
using namespace std::chrono_literals;
using Json = sdk::Json;

struct Track {
  TrackDescriptor descriptor;
  bool publication = false, ready = false, removing = false;
  bool initial = false, video_command = false;
  std::shared_ptr<engine::MediaTrack> media;

  std::string sid() const {
    return publication && media ? media->snapshot().transport.server_sid : descriptor.remote_sid;
  }

  std::map<std::string, uint64_t> metric_ids;

  void metrics(Json& report, uint64_t& next_id) {
    const auto state = media->snapshot();
    for (const auto& node : state.nodes) {
      auto [id, added] = metric_ids.try_emplace(node.name, next_id);
      if (added)
        ++next_id;
      report["nodes"].push_back({{"id", id->second},
                                 {"name", descriptor.id + "/" + node.name},
                                 {"trackId", sid()},
                                 {"kind", node.kind},
                                 {"generation", state.transport.epoch},
                                 {"outputs", node.outputs}});
    }
    for (const auto& edge : state.edges)
      report["edges"].push_back({{"from", descriptor.id + "/" + edge.from},
                                 {"to", descriptor.id + "/" + edge.to},
                                 {"dataType", edge.data_type}});
  }
};

struct Joined {
  sdk::Result<RoomSnapshot> result;
};

struct Ready {
  std::shared_ptr<Track> track;
  std::shared_ptr<engine::MediaTrack> media;
  sdk::Failure failure;
};

struct Removed {
  std::shared_ptr<Track> track;
  sdk::Result<void> result;
};

// Callbacks only enqueue owned values; the session worker owns all track/UI state.
class SessionInbox final : public engine::RtcObserver {
 public:
  using Message = std::variant<RoomEvent, Joined, Ready, Removed>;

  struct Batch {
    std::deque<Message> messages;
    bool overflow = false;
  };

  void push(Message message) {
    std::lock_guard lock(mutex_);
    if (messages_.size() >= 4096) {
      overflow_ = true;
    } else {
      messages_.push_back(std::move(message));
    }
    changed_.notify_one();
  }

  void on_room_event(const RoomEvent& event) override {
    push(event);
  }

  Batch take(livekit::compat::stop_token stop) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, stop, 5ms, [&] { return overflow_ || !messages_.empty(); });
    Batch batch;
    batch.messages.swap(messages_);
    batch.overflow = overflow_;
    return batch;
  }

 private:
  std::mutex mutex_;
  livekit::compat::condition_variable_any changed_;
  std::deque<Message> messages_;
  bool overflow_ = false;
};

Participant participant(const ParticipantSnapshot& value) {
  Participant result{value.sid, value.identity, value.name, value.state};
  for (const auto& [sid, track] : value.publications)
    result.tracks.push_back(track);
  return result;
}
}  // namespace

Session::Session() : Session([] { return engine::create_livekit_engine(AudioOutputOptions{}); }) {}

Session::Session(EngineFactory factory) : engine_factory_(std::move(factory)) {}

Session::~Session() {
  stop();
  if (worker_.joinable())
    worker_.join();
}

sdk::Result<void> Session::start(SessionOptions options, bool pause_unused_video) {
  if (std::count_if(options.media.begin(), options.media.end(),
                    [](const auto& m) { return m.video; }) > max_test_videos)
    return sdk::Result<void>::failure(sdk::make_failure(sdk::ErrorDomain::lifecycle,
                                                        sdk::ErrorCode::invalid_argument,
                                                        "Demo supports up to 8 test videos"));
  {
    std::lock_guard lock(mutex_);
    if (!data_.finished)
      return sdk::Result<void>::failure(sdk::make_failure(
          sdk::ErrorDomain::lifecycle, sdk::ErrorCode::invalid_state, "Session already active"));
  }
  if (worker_.joinable())
    worker_.join();
  {
    std::lock_guard lock(mutex_);
    data_ = {};
    video_command_.reset();
    data_.state = State::Connecting;
    data_.finished = false;
    data_.room = options.room;
    data_.pause_unused_video = pause_unused_video;
  }
  {
    worker_ = livekit::compat::jthread([this, options = std::move(options), pause_unused_video](
                                           livekit::compat::stop_token stop) mutable {
      std::string error;
      const auto redact = [&](std::string message) {
        const auto& token = options.connection.token;
        if (!token.empty())
          for (size_t pos; (pos = message.find(token)) != std::string::npos;)
            message.replace(pos, token.size(), "[redacted]");
        return message;
      };
      {
        const auto run = [&]() -> std::string {
          auto inbox = std::make_shared<SessionInbox>();
          std::map<TrackId, std::shared_ptr<Track>> tracks;
          auto created = engine_factory_();
          if (!created) {
            return created.error().message.empty() ? "Engine creation failed"
                                                   : created.error().message;
          }
          auto engine = *std::move(created);
          if (!engine) {
            return "Engine factory returned null";
          }
          if (engine->set_observer(inbox.get()) != sdk::ErrorCode::ok) {
            return "Could not register session observer";
          }
          std::string failure;
          bool joined = false;
          size_t initial_pending = 0;
          std::map<TrackId, TrackDescriptor> advertised;
          const auto finish_video = [&](std::string message = {}) {
            std::lock_guard lock(mutex_);
            data_.changing_video = false;
            data_.video_error = redact(std::move(message));
          };
          const auto erase_track = [&](const std::shared_ptr<Track>& track) {
            {
              std::lock_guard lock(mutex_);
              data_.frames.erase(track->descriptor.id);
              data_.frames.erase(track->sid());
            }
            auto it = tracks.find(track->descriptor.id);
            if (it != tracks.end() && it->second == track) {
              tracks.erase(it);
            }
          };
          const auto report = [&](const Json& event) {
            {
              std::lock_guard lock(mutex_);
              if (data_.state != State::Disconnecting)
                data_.quality.update(event);
            }
            if (options.log_sink) {
              auto logged = options.log_sink(event);
              if (!logged && failure.empty()) {
                failure = logged.error().message;
              }
            }
          };
          options.connection.subscribe = false;
          options.connection.subscribe_tracks.clear();
          options.connection.stats_interval = 1000ms;
          engine->join(options.connection, [inbox](sdk::Result<RoomSnapshot> result) {
            inbox->push(Joined{std::move(result)});
          });
          int video_number = 0;
          uint64_t track_number = 0, metric_number = 1;
          const auto publish = [&](const sdk::MediaSpec& spec, bool initial) {
            auto track = std::make_shared<Track>();
            track->publication = true;
            track->initial = initial;
            track->video_command = !initial;
            initial_pending += initial;
            auto& d = track->descriptor;
            d.id = "local-" + std::to_string(++track_number);
            d.name = spec.video ? "Test video " + std::to_string(++video_number) : "Test tone";
            d.kind = spec.video ? MediaKind::video : MediaKind::audio;
            d.format.codec = spec.video ? Codec::vp8 : Codec::opus;
            d.format.clock_rate = spec.video ? 90000 : 48000;
            d.format.channels = spec.video ? 0 : 1;
            d.format.width = spec.video ? spec.width : 0;
            d.format.height = spec.video ? spec.height : 0;
            d.stream_group = d.id;
            d.follow_subscriber_demand = spec.video && pause_unused_video;
            engine::MediaInput input;
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
            engine::MediaPublishSpec config;
            config.track = d;
            config.input = input;
            tracks.emplace(d.id, track);
            engine->publish_media(std::move(config), [inbox, track](auto result) {
              inbox->push(Ready{track, result ? result->track : nullptr,
                                result ? nullptr : result.failure_record()});
            });
          };
          const auto subscribe = [&](TrackDescriptor descriptor) {
            if (tracks.count(descriptor.id))
              return;
            auto track = std::make_shared<Track>();
            track->descriptor = descriptor;
            engine::MediaSubscribeSpec config;
            config.track = descriptor;
            config.output.recording = options.record;
            if (descriptor.kind == MediaKind::video)
              config.output.video = [this, sid = descriptor.remote_sid](const auto& frame) {
                std::lock_guard lock(mutex_);
                if (data_.state != State::Disconnecting) {
                  data_.frames[sid] = frame.pixels;
                  ++data_.received_frames;
                }
                return sdk::Result<void>::success();
              };
            else {
              config.output.playout = options.audio_playout;
            }
            tracks.emplace(descriptor.id, track);
            engine->subscribe_media(std::move(config), [inbox, track](auto result) {
              inbox->push(Ready{track, result ? result->track : nullptr,
                                result ? nullptr : result.failure_record()});
            });
          };
          const auto remove = [&](const std::shared_ptr<Track>& track, bool command) {
            if (track->removing) {
              return;
            }
            track->removing = true;
            track->video_command = command;
            auto done = [inbox, track](sdk::Result<void> result) {
              inbox->push(Removed{track, std::move(result)});
            };
            if (track->publication) {
              engine->unpublish(track->descriptor.id, std::move(done));
            } else {
              engine->unsubscribe(track->descriptor.id, std::move(done));
            }
          };
          auto next_stats = SteadyClock::now();
          while (!stop.stop_requested() && failure.empty()) {
            auto batch = inbox->take(stop);
            if (stop.stop_requested()) {
              break;
            }
            if (batch.overflow) {
              failure = "Session callback queue exhausted";
              break;
            }
            for (auto& message : batch.messages) {
              if (auto* done = std::get_if<Joined>(&message)) {
                if (!done->result) {
                  failure = done->result.error().message.empty() ? "Join failed"
                                                                 : done->result.error().message;
                  break;
                }
                const auto& session = done->result->session;
                if (!session || session->room.name != options.room) {
                  failure = "Joined room does not match the requested room";
                  break;
                }
                joined = true;
                for (const auto& media : options.media) {
                  publish(media, true);
                }
                for (const auto& [id, descriptor] : advertised) {
                  subscribe(descriptor);
                }
              } else if (auto* done = std::get_if<Ready>(&message)) {
                auto track = done->track;
                if (track->initial) {
                  --initial_pending;
                  track->initial = false;
                }
                const auto found = tracks.find(track->descriptor.id);
                const bool current = found != tracks.end() && found->second == track;
                std::string problem;
                if (done->failure) {
                  problem = done->failure->message.empty() ? "Media operation failed"
                                                           : done->failure->message;
                } else if (!done->media) {
                  problem = "Media operation returned no track";
                } else if (!current) {
                  problem = "Track removed before media became ready";
                }
                if (track->video_command) {
                  finish_video(problem);
                  track->video_command = false;
                } else if (!problem.empty() && !track->removing &&
                           (current ||
                            (done->failure && done->failure->code != sdk::ErrorCode::cancelled))) {
                  failure = "Track " + track->descriptor.id + ": " + problem;
                }
                if (current && !track->removing) {
                  if (problem.empty()) {
                    track->media = std::move(done->media);
                    track->ready = true;
                  } else {
                    erase_track(track);
                  }
                }
              } else if (auto* done = std::get_if<Removed>(&message)) {
                auto track = done->track;
                const auto found = tracks.find(track->descriptor.id);
                const bool current = found != tracks.end() && found->second == track;
                const bool already_removed =
                    !current && !done->result &&
                    done->result.error().code == sdk::ErrorCode::invalid_argument;
                const auto problem =
                    done->result || already_removed
                        ? std::string{}
                        : (done->result.error().message.empty() ? "Track removal failed"
                                                                : done->result.error().message);
                if (track->video_command) {
                  finish_video(problem);
                  track->video_command = false;
                } else if (!problem.empty() && current) {
                  failure = "Track " + track->descriptor.id + ": " + problem;
                }
                if (problem.empty()) {
                  erase_track(track);
                } else {
                  track->removing = false;
                }
              } else {
                const auto& event = std::get<RoomEvent>(message);
                if (event.kind == RoomEventKind::remote_track_added && event.descriptor) {
                  advertised[event.track_id] = *event.descriptor;
                  if (joined) {
                    subscribe(*event.descriptor);
                  }
                } else if (event.kind == RoomEventKind::track_removed) {
                  const auto found = tracks.find(event.track_id);
                  if (found != tracks.end() &&
                      (!event.publication || *event.publication == found->second->publication)) {
                    // The engine already removed this endpoint; only drop local state.
                    auto track = found->second;
                    erase_track(track);
                  }
                } else if (event.kind == RoomEventKind::remote_track_removed) {
                  advertised.erase(event.track_id);
                  const auto found = tracks.find(event.track_id);
                  if (found != tracks.end() && !found->second->publication) {
                    remove(found->second, false);
                  }
                } else if (event.kind == RoomEventKind::track_failed) {
                  const auto found = tracks.find(event.track_id);
                  if (found != tracks.end() && found->second->ready && !found->second->removing) {
                    failure = "Track " + event.track_id + ": " + event.message;
                  }
                } else if (event.kind == RoomEventKind::statistics && event.statistics) {
                  const auto& stats = *event.statistics;
                  auto json = Json::parse(stats.json, nullptr, false);
                  if (json.is_discarded()) {
                    failure = "Invalid transport statistics";
                    break;
                  }
                  report({{"event", "rtc-stats"},
                          {"target", stats.target},
                          {"timestampUs", stats.timestamp_us},
                          {"stats", std::move(json)},
                          {"localTracks", stats.publication_tracks}});
                } else if (event.kind == RoomEventKind::reconnecting) {
                  std::lock_guard lock(mutex_);
                  data_.frames.clear();
                  data_.subscriptions.clear();
                  data_.quality = {};
                  if (data_.state != State::Disconnecting) {
                    data_.state = State::Connecting;
                  }
                } else if (event.kind == RoomEventKind::closed ||
                           event.kind == RoomEventKind::failed) {
                  failure = event.message.empty()
                                ? "Disconnected by server. Join again to reconnect."
                                : event.message;
                }
              }
              if (!failure.empty()) {
                break;
              }
            }
            if (!failure.empty() || !joined) {
              continue;
            }
            std::optional<VideoCommand> command;
            {
              std::lock_guard lock(mutex_);
              command.swap(video_command_);
            }
            if (command) {
              if (command->remove_sid.empty()) {
                sdk::MediaSpec spec;
                spec.demo = true;
                publish(spec, false);
              } else {
                const auto found =
                    std::find_if(tracks.begin(), tracks.end(), [&](const auto& item) {
                      return item.second->publication && item.second->sid() == command->remove_sid;
                    });
                if (found == tracks.end()) {
                  finish_video("Publication no longer exists");
                } else {
                  remove(found->second, true);
                }
              }
            }
            const auto room = engine->snapshot();
            if (room.session) {
              std::vector<Participant> remote;
              for (const auto& [id, value] : room.session->remote)
                remote.push_back(participant(value));
              std::lock_guard lock(mutex_);
              data_.subscriptions.clear();
              for (const auto& [id, track] : tracks)
                if (!track->publication && track->ready &&
                    track->media->snapshot().transport.state == TrackState::ready)
                  data_.subscriptions.insert(track->sid());
              data_.update(participant(room.session->local), remote);
              data_.room = room.session->room.name;
              if (data_.state != State::Disconnecting && room.state == ConnectionState::connected &&
                  initial_pending == 0)
                data_.state = State::Connected;
              data_.quality.participants.clear();
              for (const auto& [id, value] : room.session->remote)
                if (value.connection_quality)
                  data_.quality.participants[id] = *value.connection_quality;
              if (room.session->local.connection_quality)
                data_.quality.participants[room.session->local.sid] =
                    *room.session->local.connection_quality;
            }
            if (SteadyClock::now() >= next_stats) {
              Json graph{{"event", "media-graph-stats"},
                         {"timestampUs", std::chrono::duration_cast<std::chrono::microseconds>(
                                             SteadyClock::now().time_since_epoch())
                                             .count()},
                         {"nodes", Json::array()},
                         {"edges", Json::array()}};
              for (auto& [id, track] : tracks)
                if (track->ready) {
                  track->metrics(graph, metric_number);
                  if (track->publication && track->descriptor.kind == MediaKind::video) {
                    const auto feedback = engine->feedback(id);
                    if (!feedback) {
                      continue;
                    }
                    const bool enabled =
                        std::any_of(feedback->encodings.begin(), feedback->encodings.end(),
                                    [](const auto& entry) { return entry.second.active; });
                    bool changed;
                    {
                      std::lock_guard lock(mutex_);
                      auto [it, added] = data_.video_demand.try_emplace(track->sid(), enabled);
                      changed = added || it->second != enabled;
                      it->second = enabled;
                    }
                    if (changed && options.log_sink) {
                      report({{"event", "source-demand"},
                              {"sid", track->sid()},
                              {"enabled", enabled}});
                    }
                  }
                }
              report(graph);
              next_stats = SteadyClock::now() + 1s;
            }
          }
          // close remains a synchronous lifecycle barrier on the supervisor thread.
          sdk::Failure close_failure;
          engine->close([&](sdk::Result<void> result) {
            if (!result) {
              close_failure = result.failure_record();
            }
          });
          (void)engine->set_observer(nullptr);
          if (failure.empty() && close_failure) {
            failure =
                close_failure->message.empty() ? "Engine close failed" : close_failure->message;
          }
          for (const auto& [id, track] : tracks) {
            if (track->media) {
              auto checked = track->media->check();
              if (checked != sdk::ErrorCode::ok && failure.empty()) {
                const auto state = track->media->snapshot();
                failure =
                    "Track " + id + ": " +
                    (state.transport.failure ? state.transport.failure->message : "Media failure");
              }
            }
          }
          return failure;
        };
        // Media failures are returned explicitly by the supervisor.
        error = redact(run());
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested())
        error.clear();
      data_ = {};
      data_.state = error.empty() ? State::Idle : State::Error;
      data_.error = std::move(error);
      video_command_.reset();
    });
  }
  return sdk::Result<void>::success();
}

void Session::stop() {
  {
    std::lock_guard lock(mutex_);
    if (data_.finished)
      return;
    data_.state = State::Disconnecting;
  }
  worker_.request_stop();
}
}  // namespace demo
