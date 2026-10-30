#include "engines/rtc/rtc_engine_control.hpp"
#include <utility>

namespace livekit::sdk::rtc::engine {
namespace {
bool latched(const ParticipantSnapshot& participant) {
  auto it = participant.attributes.find(kVideoH264LatchAttribute);
  return it != participant.attributes.end() && it->second == "1";
}

bool requires_h264(const ParticipantSnapshot& participant) {
  auto it = participant.attributes.find(kVideoDecodeCodecsAttribute);
  // Deliberately strict: unknown or malformed declarations cannot authorize H265.
  return latched(participant) || it == participant.attributes.end() || it->second != "h264,h265";
}
}  // namespace

void RtcEngine::reset_video_codec_policy(std::optional<RoomVideoCodecPolicy> options) {
  codec_policy = options;
  policy_advertised_epoch = 0;
  policy_advertised_latch = false;
  std::lock_guard lock(policy_mutex);
  policy_status = {};
  policy_status.enabled = options.has_value();
  policy_status.h264_latched = options && !options->can_decode_h265;
}

void RtcEngine::notify_video_codec_policy() {
  VideoCodecPolicySnapshot status;
  {
    std::lock_guard lock(policy_mutex);
    status = policy_status;
  }
  runtime->post_notification([observer = room_observer.get(), status] {
    if (auto target = observer.lock()) {
      target->on_video_codec_policy_changed(status);
    }
  });
}

void RtcEngine::use_h264(MediaPublishSpec& spec) {
  if (spec.track.kind != MediaKind::video || spec.track.format.codec == Codec::h264) {
    return;
  }
  if (spec.track.format.codec != Codec::h265) {
    throw SDKError(ErrorDomain::media, ErrorCode::unsupported,
                   "Room codec policy only accepts H264/H265 video");
  }
  if (spec.input.encoded_source) {
    if (!spec.h264_fallback_input) {
      throw SDKError(ErrorDomain::media, ErrorCode::unsupported,
                     "H265 encoded input requires h264_fallback_input");
    }
    detail::CallbackScope scope;
    spec.input = spec.h264_fallback_input();
    spec.shared_input.reset();
  }
  spec.track.format.codec = Codec::h264;
  // HEVC fmtp must never leak into AVC negotiation. Use the SDK's interoperable
  // constrained-baseline, packetization-mode 1 default.
  spec.track.format.fmtp.clear();
}

Result<MediaPublication> RtcEngine::start_media_locked(MediaPublishSpec spec) {
  return detail::control_boundary([&]() -> Result<MediaPublication> {
    if (codec_policy) {
      const auto snapshot = runtime->snapshot();
      if (snapshot.state != ConnectionState::connected || !snapshot.session) {
        return detail::failure_result<MediaPublication>(ErrorCode::invalid_state,
                                                        "Codec policy requires a current roster");
      }
      bool h264;
      {
        std::lock_guard lock(policy_mutex);
        h264 = policy_status.h264_latched;
      }
      if (h264) {
        use_h264(spec);
      }
    }
    auto recipe = spec;
    std::shared_ptr<MediaTrack> track;
    struct Building {
      bool& flag;
      explicit Building(bool& flag) : flag(flag) {
        flag = true;
      }
      ~Building() {
        flag = false;
      }
    } building(building_media);
    auto ready = publish_graph(spec.track, [&](auto node) {
      auto prepared = media->build_publish(std::move(spec), std::move(node));
      track = std::move(prepared.track);
      return std::move(prepared.graph);
    });
    if (!ready) {
      return Result<MediaPublication>::failure(ready.failure_record());
    }
    if (codec_policy && recipe.track.kind == MediaKind::video) {
      media_publications.insert_or_assign(*ready, std::move(recipe));
    }
    return Result<MediaPublication>::success({*ready, std::move(track)});
  });
}

void RtcEngine::reconcile_video_codec_policy() {
  if (!codec_policy || closed || control->interrupted) {
    return;
  }
  const auto snapshot = runtime->snapshot();
  if (snapshot.state != ConnectionState::connected) {
    return;
  }
  const auto fail = [&](std::shared_ptr<const FailureInfo> failure) {
    std::lock_guard lock(policy_mutex);
    policy_status.failure = std::move(failure);
  };
  if (!snapshot.session) {
    // Transport establishment can precede the new roster during recovery.
    // Defer until session_changed; never use a stale roster from the old epoch.
    return;
  }
  std::erase_if(media_publications,
                [&](const auto& entry) { return graphs->publication(entry.first).id.empty(); });
  bool h264;
  {
    std::lock_guard lock(policy_mutex);
    h264 = policy_status.h264_latched;
  }
  h264 = h264 || !codec_policy->can_decode_h265 || latched(snapshot.session->local);
  for (const auto& [sid, participant] : snapshot.session->remote) {
    if (requires_h264(participant)) {
      h264 = true;
    }
  }
  bool changed = false;
  {
    std::lock_guard lock(policy_mutex);
    changed = h264 != policy_status.h264_latched;
    policy_status.h264_latched = h264;
    policy_status.transitioning = changed;
  }
  if (changed) {
    notify_video_codec_policy();
  }

  // Stop every old producer before negotiating any replacement. Retire graphs
  // before releasing capture leases; stale frames cannot enter the new endpoint.
  std::vector<MediaPublishSpec> replacements;
  if (h264) {
    for (auto it = media_publications.begin(); it != media_publications.end();) {
      if (it->second.track.format.codec != Codec::h265) {
        ++it;
        continue;
      }
      auto spec = it->second;
      const auto current = graphs->publication(it->first);
      it = media_publications.erase(it);
      if (current.id.empty() || current.state == TrackState::failed ||
          current.state == TrackState::closed) {
        continue;  // Never resurrect a failed or explicitly removed publication.
      }
      spec.track.muted = current.muted;
      const auto retired = graphs->take(spec.track.id, true);
      const auto removed = runtime
                               ->invoke<void>([&](RoomEngine& protocol, auto done) {
                                 protocol.unpublish(spec.track.id, std::move(done));
                               })
                               .get();
      if (removed != ErrorCode::ok) {
        auto result = detail::operation_result(removed, "codec_policy.unpublish");
        fail(result.failure_record());
        runtime->post_notification([observer = publication_observer.get(), id = spec.track.id,
                                    failure = result.failure_record()] {
          if (auto target = observer.lock()) {
            target->on_video_codec_republished(id, Result<MediaPublication>::failure(failure));
          }
        });
        continue;
      }
      replacements.push_back(std::move(spec));
    }
  }

  if (policy_advertised_epoch != snapshot.epoch || policy_advertised_latch != h264) {
    policy_advertised_epoch = snapshot.epoch;
    policy_advertised_latch = h264;
    ParticipantUpdate update;
    update.attributes[kVideoDecodeCodecsAttribute] =
        codec_policy->can_decode_h265 ? "h264,h265" : "h264";
    update.attributes[kVideoH264LatchAttribute] = h264 ? "1" : "0";
    const auto result =
        runtime
            ->invoke<void>([&](RoomEngine& protocol, auto done) {
              protocol.update_local_participant(std::move(update), {}, std::move(done));
            })
            .get();
    if (result != ErrorCode::ok) {
      fail(detail::operation_result(result, "codec_policy.advertise").failure_record());
    }
    changed = true;
  }
  for (auto& spec : replacements) {
    const auto id = spec.track.id;
    auto result = start_media_locked(std::move(spec));
    if (!result) {
      fail(result.failure_record());
    }
    runtime->post_notification(
        [observer = publication_observer.get(), id, result = std::move(result)]() mutable {
          if (auto target = observer.lock()) {
            target->on_video_codec_republished(id, std::move(result));
          }
        });
    changed = true;
  }
  {
    std::lock_guard lock(policy_mutex);
    policy_status.transitioning = false;
  }
  if (changed) {
    notify_video_codec_policy();
  }
}
}  // namespace livekit::sdk::rtc::engine
