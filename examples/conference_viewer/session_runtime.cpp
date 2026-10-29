#include "session.hpp"
#include <livekit/rtc/media_engine.hpp>
#include <algorithm>

namespace demo {
namespace {
using namespace rtc;
using namespace std::chrono_literals;
using Json = sdk::Json;

struct Track {
  TrackDescriptor descriptor;
  bool publication = false, ready = false;
  std::future<TrackId> pending;
  std::shared_ptr<engine::MediaTrack> media;

  std::string sid() const {
    return publication ? media->snapshot().transport.server_sid : descriptor.remote_sid;
  }

  void check() const {
    if (media)
      media->throw_if_failed();
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

Participant participant(const ParticipantSnapshot& value) {
  Participant result{value.sid, value.identity, value.name, value.state};
  for (const auto& [sid, track] : value.publications)
    result.tracks.push_back(track);
  return result;
}
}  // namespace

Session::Session() = default;

Session::~Session() {
  stop();
  if (worker_.joinable())
    worker_.join();
}

void Session::start(SessionOptions options, bool pause_unused_video) {
  if (std::count_if(options.media.begin(), options.media.end(),
                    [](const auto& m) { return m.video; }) > max_test_videos)
    throw std::invalid_argument("Demo supports up to 8 test videos");
  {
    std::lock_guard lock(mutex_);
    if (!data_.finished)
      throw std::logic_error("Session already active");
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
  try {
    worker_ = std::jthread([this, options = std::move(options),
                            pause_unused_video](std::stop_token stop) mutable {
      std::string error;
      const auto redact = [&](std::string message) {
        const auto& token = options.connection.token;
        if (!token.empty())
          for (size_t pos; (pos = message.find(token)) != std::string::npos;)
            message.replace(pos, token.size(), "[redacted]");
        return message;
      };
      try {
        std::map<TrackId, std::shared_ptr<Track>> tracks;
        auto engine = engine::create_livekit_media_engine();
        const auto report = [&](const Json& event) {
          {
            std::lock_guard lock(mutex_);
            if (data_.state != State::Disconnecting)
              data_.quality.update(event);
          }
          if (options.log_sink)
            options.log_sink(event);
        };
        // Only the supervisor owns engine control. Codec callbacks only replace an owned frame.
        const auto await = [&](auto& future) {
          while (future.wait_for(0ms) != std::future_status::ready) {
            if (stop.stop_requested())
              throw std::runtime_error("Session cancelled");
            engine->poll();
            std::this_thread::sleep_for(5ms);
          }
          return future.get();
        };
        options.connection.subscribe = false;
        options.connection.subscribe_tracks.clear();
        options.connection.stats_interval = 1000ms;
        auto joining = engine->join(options.connection);
        await(joining);
        if (const auto joined = engine->room().snapshot().session;
            !joined || joined->room.name != options.room)
          throw std::runtime_error("Joined room does not match the requested room");
        int video_number = 0;
        uint64_t track_number = 0, metric_number = 1;
        const auto publish = [&](const sdk::MediaSpec& spec) {
          auto track = std::make_shared<Track>();
          track->publication = true;
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
          auto operation = engine->publish_media(std::move(config));
          track->media = std::move(operation.track);
          track->pending = std::move(operation.ready);
          tracks.emplace(d.id, track);
          return track;
        };
        for (const auto& media : options.media) {
          auto track = publish(media);
          await(track->pending);
          track->ready = true;
        }
        const auto subscribe = [&](TrackDescriptor descriptor) {
          if (tracks.contains(descriptor.id))
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
            };
          else {
            config.output.playout = options.audio_playout;
          }
          auto operation = engine->subscribe_media(std::move(config));
          track->media = std::move(operation.track);
          track->pending = std::move(operation.ready);
          tracks.emplace(descriptor.id, std::move(track));
        };
        auto next_stats = SteadyClock::now();
        while (!stop.stop_requested()) {
          while (auto event = engine->room().next_event(0ms)) {
            if (event->kind == RoomEventKind::remote_track_added && event->descriptor)
              subscribe(*event->descriptor);
            else if (event->kind == RoomEventKind::track_removed) {
              if (auto found = tracks.find(event->track_id); found != tracks.end()) {
                auto removing = found->second->publication ? engine->unpublish(found->first)
                                                           : engine->unsubscribe(found->first);
                await(removing);
                {
                  std::lock_guard lock(mutex_);
                  data_.frames.erase(found->first);
                  data_.frames.erase(found->second->sid());
                }
                tracks.erase(found);
              }
            } else if (event->kind == RoomEventKind::remote_track_removed) {
              auto found = tracks.find(event->track_id);
              if (found != tracks.end() && !found->second->publication) {
                auto removing = engine->unsubscribe(found->first);
                await(removing);
                {
                  std::lock_guard lock(mutex_);
                  data_.frames.erase(found->second->sid());
                }
                tracks.erase(found);
              }
            } else if (event->kind == RoomEventKind::statistics && event->statistics) {
              const auto& stats = *event->statistics;
              report({{"event", "rtc-stats"},
                      {"target", stats.target},
                      {"timestampUs", stats.timestamp_us},
                      {"stats", Json::parse(stats.json)},
                      {"localTracks", stats.publication_tracks}});
            } else if (event->kind == RoomEventKind::reconnecting) {
              std::lock_guard lock(mutex_);
              data_.frames.clear();
              data_.subscriptions.clear();
              data_.quality = {};
              if (data_.state != State::Disconnecting)
                data_.state = State::Connecting;
            } else if (event->kind == RoomEventKind::closed || event->kind == RoomEventKind::failed)
              throw std::runtime_error(event->message.empty()
                                           ? "Disconnected by server. Join again to reconnect."
                                           : event->message);
          }
          engine->poll();
          while (auto failure = engine->next_failure()) {
            auto it = tracks.find(failure->id);
            if (it != tracks.end())
              throw std::runtime_error("Track " + failure->id + ": " + failure->message);
          }
          for (auto& [id, track] : tracks)
            if (track->pending.valid() &&
                track->pending.wait_for(0ms) == std::future_status::ready) {
              track->pending.get();
              track->ready = true;
            }
          std::optional<VideoCommand> command;
          {
            std::lock_guard lock(mutex_);
            command.swap(video_command_);
          }
          if (command) {
            std::string failure;
            TrackId adding;
            try {
              if (command->remove_sid.empty()) {
                sdk::MediaSpec spec;
                spec.demo = true;
                auto track = publish(spec);
                adding = track->descriptor.id;
                await(track->pending);
                track->ready = true;
              } else {
                const auto found =
                    std::find_if(tracks.begin(), tracks.end(), [&](const auto& item) {
                      return item.second->publication && item.second->sid() == command->remove_sid;
                    });
                if (found == tracks.end())
                  throw std::runtime_error("Publication no longer exists");
                auto removing = engine->unpublish(found->first);
                await(removing);
                tracks.erase(found);
              }
            } catch (const std::exception& e) {
              failure = redact(e.what());
              if (!adding.empty()) {
                try {
                  engine->unpublish(adding).get();
                } catch (...) {
                }
                tracks.erase(adding);
              }
            }
            std::lock_guard lock(mutex_);
            data_.changing_video = false;
            data_.video_error = std::move(failure);
          }
          const auto room = engine->room().snapshot();
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
            if (data_.state != State::Disconnecting && room.state == ConnectionState::connected)
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
                  const bool enabled =
                      std::any_of(feedback.encodings.begin(), feedback.encodings.end(),
                                  [](const auto& entry) { return entry.second.active; });
                  bool changed;
                  {
                    std::lock_guard lock(mutex_);
                    auto [it, added] = data_.video_demand.try_emplace(track->sid(), enabled);
                    changed = added || it->second != enabled;
                    it->second = enabled;
                  }
                  if (changed && options.log_sink)
                    options.log_sink(
                        {{"event", "source-demand"}, {"sid", track->sid()}, {"enabled", enabled}});
                }
              }
            report(graph);
            next_stats = SteadyClock::now() + 1s;
          }
          std::this_thread::sleep_for(5ms);
        }
        engine->close().get();
        for (const auto& [id, track] : tracks)
          track->check();
      } catch (const std::exception& e) {
        error = redact(e.what());
      } catch (...) {
        error = "Unknown session failure";
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested())
        error.clear();
      data_ = {};
      data_.state = error.empty() ? State::Idle : State::Error;
      data_.error = std::move(error);
      video_command_.reset();
    });
  } catch (...) {
    std::lock_guard lock(mutex_);
    data_.finished = true;
    data_.state = State::Error;
    throw;
  }
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
