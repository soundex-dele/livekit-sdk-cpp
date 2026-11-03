#include "livekit/media_engine.hpp"
#include "engines/media/media_pipeline.hpp"
#include "backends/livekit/rtc_engine.hpp"
#include "support/logger.h"
#include <algorithm>
#include <cctype>

namespace livekit::sdk::client {
namespace {
bool callback_thread() {
  return detail::sdk_thread || rtc::detail::callback_depth != 0;
}

Failure invalid_thread() {
  return make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                      "Media control is not allowed on SDK or media callback threads");
}

Result<void> validate_output(MediaKind kind, const MediaOutput& output) {
  if ((kind == MediaKind::video && (output.audio || output.playout)) ||
      (kind == MediaKind::audio && output.video) || !output.queue.max_frames ||
      !output.queue.max_bytes || output.queue.max_age.count() <= 0) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                              "Invalid media output kind or queue"));
  }
  return Result<void>::success();
}
}  // namespace

struct MediaEngine::Impl {
  std::unique_ptr<Room> room;
  std::mutex control;
  std::atomic<bool> closed{false};
  std::shared_ptr<rtc::AudioRenderReference> echo;
  std::shared_ptr<rtc::AudioOutputSession> output;
  std::map<TrackId, std::shared_ptr<detail::MediaPipeline>> publications, subscriptions;
  std::map<std::pair<std::string, std::string>, std::weak_ptr<rtc::detail::MediaSyncClock>> clocks;

  Impl(std::unique_ptr<Room> value, AudioOutputOptions options) : room(std::move(value)) {
    echo = options.echo_reference ? options.echo_reference
                                  : std::make_shared<rtc::AudioRenderReference>();
    options.echo_reference = echo;
    output = std::make_shared<rtc::AudioOutputSession>(std::move(options));
  }
};

MediaTrack::MediaTrack(std::shared_ptr<detail::MediaPipeline> pipeline)
    : pipeline_(std::move(pipeline)) {}

MediaTrackSnapshot MediaTrack::snapshot() const {
  return pipeline_->snapshot();
}

Result<std::vector<PeerStatistics>> MediaEngine::statistics() {
  if (callback_thread()) {
    return Result<std::vector<PeerStatistics>>::failure(invalid_thread());
  }
  return impl_->room->statistics();
}

MediaEngine::MediaEngine(AudioOutputOptions options)
    : MediaEngine(std::make_unique<Room>(), std::move(options)) {}

MediaEngine::MediaEngine(std::unique_ptr<Room> room, AudioOutputOptions options)
    : impl_(std::make_unique<Impl>(std::move(room), std::move(options))) {}

MediaEngine::~MediaEngine() {
  set_observer(nullptr);
  close();
}

void MediaEngine::set_observer(RoomObserver* observer) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "set_observer: invalid callback thread");
    return;
  }
  impl_->room->set_observer(observer);
}

bool MediaEngine::connect(ConnectionOptions options) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "connect: invalid callback thread");
    return false;
  }
  std::lock_guard lock(impl_->control);
  return !impl_->closed && impl_->room->connect(std::move(options));
}

void MediaEngine::close() {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "close: invalid callback thread");
    return;
  }
  impl_->closed = true;
  // Cancel a synchronous connect/publish/subscribe before waiting for control.
  impl_->room->close();
  std::lock_guard lock(impl_->control);
  for (auto& [id, pipeline] : impl_->publications) {
    pipeline->close();
  }
  for (auto& [id, pipeline] : impl_->subscriptions) {
    pipeline->close();
  }
  impl_->publications.clear();
  impl_->subscriptions.clear();
  impl_->clocks.clear();
}

ConnectionState MediaEngine::connection_state() const {
  return impl_->room->connection_state();
}

RoomInfo MediaEngine::info() const {
  return impl_->room->info();
}

LocalParticipant MediaEngine::local_participant() const {
  return impl_->room->local_participant();
}

std::vector<RemoteParticipant> MediaEngine::remote_participants() const {
  return impl_->room->remote_participants();
}

Result<MediaPublication> MediaEngine::publish_media(MediaPublishSpec spec) {
  if (callback_thread()) {
    return Result<MediaPublication>::failure(invalid_thread());
  }
  std::lock_guard lock(impl_->control);
  if (impl_->closed || connection_state() != ConnectionState::connected) {
    return Result<MediaPublication>::failure(make_failure(
        ErrorDomain::lifecycle, ErrorCode::invalid_state, "MediaEngine is not connected"));
  }
  if (auto result = validate_output(spec.kind, spec.preview); !result) {
    return Result<MediaPublication>::failure(result.failure_record());
  }
  if (spec.options.encodings.size() != 1 || !spec.options.encodings.front().max_bitrate_bps) {
    return Result<MediaPublication>::failure(
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                     "MediaEngine requires one encoding with a positive bitrate"));
  }
  if (spec.input.encoded_source &&
      (spec.preview.video || spec.preview.audio || spec.preview.frame || spec.preview.playout ||
       !spec.preview.recording.empty())) {
    return Result<MediaPublication>::failure(
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                     "Encoded input cannot produce a raw preview"));
  }
  auto track = spec.kind == MediaKind::video
                   ? LocalTrack::create_video(spec.name, spec.format, spec.queue)
                   : LocalTrack::create_audio(spec.name, spec.format, spec.queue);
  if (!track) {
    return Result<MediaPublication>::failure(track.failure_record());
  }
  if (!spec.input.echo_reference) {
    spec.input.echo_reference = impl_->echo;
  }
  auto pipeline = std::make_shared<detail::MediaPipeline>();
  if (auto result = pipeline->prepare(spec, *track, impl_->output); !result) {
    return Result<MediaPublication>::failure(result.failure_record());
  }
  auto publication = local_participant().publish_track(*track, spec.options);
  if (!publication) {
    return Result<MediaPublication>::failure(publication.failure_record());
  }
  pipeline->publication = *publication;
  auto started = pipeline->start();
  if (!started || impl_->closed) {
    pipeline->close();
    local_participant().unpublish_track(publication->snapshot().sid);
    return Result<MediaPublication>::failure(
        !started ? started.failure_record()
                 : make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                                "MediaEngine closed during publish"));
  }
  impl_->publications[track->snapshot().id] = pipeline;
  return Result<MediaPublication>::success(
      {*publication, std::shared_ptr<MediaTrack>(new MediaTrack(pipeline))});
}

Result<MediaSubscription> MediaEngine::subscribe_media(MediaSubscribeSpec spec) {
  if (callback_thread()) {
    return Result<MediaSubscription>::failure(invalid_thread());
  }
  std::lock_guard lock(impl_->control);
  const auto info = spec.publication.snapshot();
  if (impl_->closed || connection_state() != ConnectionState::connected || !spec.publication) {
    return Result<MediaSubscription>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                     "MediaEngine is not connected or publication is invalid"));
  }
  if (impl_->subscriptions.count(info.sid)) {
    return Result<MediaSubscription>::failure(
        make_failure(ErrorDomain::media, ErrorCode::busy, "Media subscription already exists"));
  }
  if (auto result = validate_output(info.kind, spec.output); !result) {
    return Result<MediaSubscription>::failure(result.failure_record());
  }
  if (spec.presentation_delay.count() < 0 ||
      spec.presentation_delay > std::chrono::milliseconds(500)) {
    return Result<MediaSubscription>::failure(make_failure(
        ErrorDomain::media, ErrorCode::invalid_argument, "Invalid presentation delay"));
  }
  std::optional<PublicationInfo> wire;
  for (const auto& remote : remote_participants()) {
    auto participant = remote.snapshot();
    auto found = participant.publications.find(info.sid);
    if (participant.identity == info.participant_identity &&
        found != participant.publications.end()) {
      wire = found->second;
    }
  }
  if (!wire) {
    return Result<MediaSubscription>::failure(make_failure(
        ErrorDomain::media, ErrorCode::stale_handle, "Publication does not belong to this Room"));
  }
  rtc::TrackDescriptor d;
  d.id = d.remote_sid = info.sid;
  d.kind = static_cast<rtc::MediaKind>(info.kind);
  d.source_id = info.participant_identity;
  d.stream_group = wire->stream;
  auto mime = wire->mime_type;
  std::transform(mime.begin(), mime.end(), mime.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (mime == "audio/opus") {
    d.format.codec = rtc::Codec::opus;
  } else if (mime == "video/vp8") {
    d.format.codec = rtc::Codec::vp8;
  } else if (mime == "video/h264") {
    d.format.codec = rtc::Codec::h264;
  } else if (mime == "video/h265") {
    d.format.codec = rtc::Codec::h265;
  } else {
    return Result<MediaSubscription>::failure(make_failure(
        ErrorDomain::media, ErrorCode::unsupported, "Unsupported publication codec: " + mime));
  }
  d.format.clock_rate = info.kind == MediaKind::audio ? 48000 : 90000;
  d.format.channels = info.kind == MediaKind::audio ? 1 : 0;
  d.format.width = wire->width;
  d.format.height = wire->height;
  spec.publication.set_subscribed(true);
  auto track = spec.publication.track();
  if (!track) {
    auto failure = spec.publication.snapshot().failure;
    return Result<MediaSubscription>::failure(
        failure ? failure
                : make_failure(ErrorDomain::media, ErrorCode::request_failed,
                               "Room subscription did not become ready"));
  }
  std::shared_ptr<rtc::detail::MediaSyncClock> clock;
  if (spec.synchronize) {
    auto key = std::make_pair(d.source_id, d.stream_group);
    clock = impl_->clocks[key].lock();
    if (!clock) {
      impl_->clocks[key] = clock =
          std::make_shared<rtc::detail::MediaSyncClock>(spec.presentation_delay);
    }
  }
  auto pipeline = std::make_shared<detail::MediaPipeline>();
  auto result = pipeline->prepare(spec, *track, d, impl_->output, std::move(clock));
  if (result) {
    result = pipeline->start();
  }
  if (!result || impl_->closed) {
    pipeline->close();
    spec.publication.set_subscribed(false);
    return Result<MediaSubscription>::failure(
        !result ? result.failure_record()
                : make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                               "MediaEngine closed during subscribe"));
  }
  impl_->subscriptions[info.sid] = pipeline;
  return Result<MediaSubscription>::success(
      {spec.publication, std::shared_ptr<MediaTrack>(new MediaTrack(pipeline))});
}

void MediaEngine::unpublish_track(TrackId id) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "unpublish_track: invalid callback thread");
    return;
  }
  std::lock_guard lock(impl_->control);
  auto found = impl_->publications.find(id);
  if (found == impl_->publications.end()) {
    LOG_ERROR("MediaEngine", "unpublish_track: unknown track " << id);
    return;
  }
  found->second->close();
  local_participant().unpublish_track(found->second->publication.snapshot().sid);
  impl_->publications.erase(found);
}

void MediaEngine::unsubscribe_track(TrackSid sid) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "unsubscribe_track: invalid callback thread");
    return;
  }
  std::lock_guard lock(impl_->control);
  auto found = impl_->subscriptions.find(sid);
  if (found == impl_->subscriptions.end()) {
    LOG_ERROR("MediaEngine", "unsubscribe_track: unknown SID " << sid);
    return;
  }
  found->second->close();
  if (found->second->subscription.snapshot().state != TrackState::closed) {
    found->second->subscription.set_subscribed(false);
  }
  impl_->subscriptions.erase(found);
}

void MediaEngine::set_muted(TrackId id, bool muted) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "set_muted: invalid callback thread");
    return;
  }
  std::lock_guard lock(impl_->control);
  auto found = impl_->publications.find(id);
  if (found == impl_->publications.end()) {
    LOG_ERROR("MediaEngine", "set_muted: unknown track " << id);
    return;
  }
  found->second->local.set_muted(muted);
}

void MediaEngine::set_receive_options(TrackSid sid, ReceiveOptions options) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "set_receive_options: invalid callback thread");
    return;
  }
  std::lock_guard lock(impl_->control);
  auto found = impl_->subscriptions.find(sid);
  if (found == impl_->subscriptions.end()) {
    LOG_ERROR("MediaEngine", "set_receive_options: unknown SID " << sid);
    return;
  }
  found->second->subscription.set_receive_options(options);
}

void MediaEngine::publish_data(DataPacket packet) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "publish_data: invalid callback thread");
    return;
  }
  local_participant().publish_data(std::move(packet));
}

void MediaEngine::update(ParticipantUpdate value) {
  if (callback_thread()) {
    LOG_ERROR("MediaEngine", "update: invalid callback thread");
    return;
  }
  local_participant().update(std::move(value));
}
}  // namespace livekit::sdk::client
