#include "backends/rtc/livekit/session_roster.hpp"
#include "backends/rtc/livekit/subscription_policy_wire.hpp"
#include "backends/rtc/livekit/subscribed_quality.hpp"
#include <iostream>
#include <limits>

using namespace livekit::sdk::rtc;
using detail::SessionRoster;

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

void aggregate_demand() {
  using livekit::sdk::detail::subscribed_video_enabled;
  livekit::SignalResponse response;
  check(response.ParseFromString(
            std::string("\x72\x0f\x0a\x01T\x1a\x0a\x0a\x04h264\x12\x02\x08\x02", 17)),
        "Cannot parse upstream aggregate demand field");
  auto update = response.subscribed_quality_update();
  check(update.track_sid() == "T" && subscribed_video_enabled(update, "h264") == false,
        "Aggregate OFF lost");
  check(!subscribed_video_enabled(update, "vp8"), "Foreign codec treated as OFF");
  auto* codec = update.mutable_subscribed_codecs(0);
  codec->set_codec("video/H264");
  codec->mutable_qualities(0)->set_enabled(true);
  check(subscribed_video_enabled(update, "h264") == true, "Codec normalization lost aggregate ON");
  codec->mutable_qualities(0)->set_quality(static_cast<livekit::VideoQuality>(17));
  check(!subscribed_video_enabled(update, "h264"), "Future quality treated as OFF");
  update.Clear();
  check(!subscribed_video_enabled(update, "vp8"), "Empty demand treated as OFF");
  auto* quality = update.add_subscribed_qualities();
  quality->set_quality(livekit::HIGH);
  quality->set_enabled(true);
  check(subscribed_video_enabled(update, "vp8") == true, "Legacy aggregate ON lost");
  update.add_subscribed_codecs()->set_codec("h264");
  check(!subscribed_video_enabled(update, "vp8"), "Foreign codec fell back to legacy aggregate");
}

livekit::ParticipantInfo participant(std::string sid, uint32_t version) {
  livekit::ParticipantInfo p;
  p.set_sid(sid);
  p.set_identity("identity-" + sid);
  p.set_name("name-" + sid);
  p.set_version(version);
  p.set_state(2);
  p.set_metadata("metadata");
  (*p.mutable_attributes())["role"] = "speaker";
  auto* permission = p.mutable_permission();
  permission->set_can_publish(true);
  permission->set_can_subscribe(true);
  permission->set_can_publish_data(true);
  permission->set_can_update_metadata(true);
  permission->add_can_publish_sources(livekit::MICROPHONE);
  auto* track = p.add_tracks();
  track->set_sid("track-" + sid);
  track->set_name("camera");
  track->set_type(livekit::VIDEO);
  track->set_source(livekit::CAMERA);
  track->set_mime_type("video/av1");
  track->set_width(1920);
  track->set_height(1080);
  track->set_stream("group");
  return p;
}

livekit::JoinResponse join() {
  livekit::JoinResponse response;
  response.mutable_room()->set_sid("RM1");
  response.mutable_room()->set_name("room");
  *response.mutable_participant() = participant("local", 1);
  return response;
}

void policy_mapping() {
  SubscriptionPermissions policy{false, {{"viewer", false, {"camera", "screen"}}}};
  auto wire = detail::subscription_permissions_wire(policy, {});
  check(!wire.all_participants() && !wire.track_permissions(0).all_tracks() &&
            wire.track_permissions(0).track_sids().empty(),
        "Unpublished IDs expanded access");
  wire = detail::subscription_permissions_wire(policy,
                                               {{"camera", "old-sid"}, {"unlisted", "secret"}});
  check(wire.track_permissions(0).track_sids_size() == 1 &&
            wire.track_permissions(0).track_sids(0) == "old-sid",
        "Selective policy included unlisted publication");
  wire = detail::subscription_permissions_wire(policy,
                                               {{"camera", "new-sid"}, {"screen", "screen-sid"}});
  check(wire.track_permissions(0).track_sids_size() == 2 &&
            wire.track_permissions(0).track_sids(0) == "new-sid",
        "Policy retained previous-session SID");
  wire = detail::subscription_permissions_wire(policy, {{"screen", "screen-sid"}});
  check(wire.track_permissions(0).track_sids_size() == 1 &&
            wire.track_permissions(0).track_sids(0) == "screen-sid",
        "Removed publication retained access");
  policy.participants[0].track_ids.clear();
  policy.participants[0].all_tracks = true;
  wire = detail::subscription_permissions_wire(policy, {});
  check(wire.track_permissions(0).all_tracks() &&
            wire.track_permissions(0).participant_identity() == "viewer",
        "All-track identity missing");
}

void roster_contract() {
  SessionRoster state(join(), 7);
  auto p = participant("remote", 3);
  check(state.participant(p), "Remote ignored");
  const auto before = state.snapshot();
  check(before->epoch == 7 && before->room.name == "room" &&
            before->local.identity == "identity-local",
        "Join snapshot lost state");
  const auto& remote = before->remote.at("remote");
  check(remote.permissions && remote.permissions->can_publish_data &&
            remote.permissions->can_update_metadata &&
            remote.permissions->can_publish_sources == std::vector{PublicationSource::microphone},
        "Permission mapping failed");
  check(remote.publications.at("track-remote").mime_type == "video/av1",
        "Unsupported codec disappeared from roster");
  p.set_version(2);
  p.set_name("stale");
  check(!state.participant(p), "Stale participant version accepted");
  p.set_version(4);
  p.set_name("renamed");
  p.set_metadata("updated");
  p.mutable_attributes()->clear();
  p.mutable_tracks(0)->set_muted(true);
  p.mutable_permission()->set_can_publish(false);
  check(state.participant(p), "New participant version rejected");
  const auto updated = state.snapshot();
  check(updated->remote.at("remote").name == "renamed" &&
            updated->remote.at("remote").attributes.empty() &&
            updated->remote.at("remote").publications.at("track-remote").muted &&
            !updated->remote.at("remote").permissions->can_publish,
        "Participant update omitted fields");
  check(before->remote.at("remote").name == "name-remote" &&
            !before->remote.at("remote").publications.at("track-remote").muted,
        "Retained snapshot mutated");
  p.set_state(3);
  check(state.participant(p), "Same-version disconnect ignored");
  p.set_state(2);
  p.set_version(100);
  check(!state.participant(p) && state.snapshot()->remote.empty(), "Disconnected SID revived");
  auto replacement = participant("replacement", 1);
  replacement.set_identity(p.identity());
  replacement.clear_permission();
  state.participant(replacement);
  check(!state.snapshot()->remote.at("replacement").permissions,
        "Absent permissions became an explicit denial");
  SessionRoster rejoined(join(), 8);
  check(rejoined.participant(p), "History leaked into new session");
}

void observations() {
  SessionRoster state(join(), 1);
  auto p = participant("remote", 1);
  state.participant(p);
  livekit::SpeakersChanged delta;
  auto* speaker = delta.add_speakers();
  speaker->set_sid("remote");
  speaker->set_active(true);
  speaker->set_level(.6f);
  check(state.speakers(delta.speakers()), "Speaker delta ignored");
  speaker->set_sid("local");
  speaker->set_level(.4f);
  state.speakers(delta.speakers());
  check(state.snapshot()->remote.at("remote").speaking && state.snapshot()->local.speaking,
        "Delta cleared other active speakers");
  livekit::ConnectionQualityUpdate quality;
  auto* item = quality.add_updates();
  item->set_participant_sid("remote");
  item->set_quality(2);
  check(state.quality(quality), "Quality ignored");
  p.set_version(2);
  state.participant(p);
  check(state.snapshot()->remote.at("remote").speaking &&
            state.snapshot()->remote.at("remote").connection_quality == 2,
        "Participant update erased orthogonal observations");
  speaker->set_level(std::numeric_limits<float>::quiet_NaN());
  check(!state.speakers(delta.speakers()), "Nonfinite audio level accepted");
  speaker->set_level(.2f);
  speaker->set_active(false);
  state.speakers(delta.speakers(), true);
  check(!state.snapshot()->local.speaking && !state.snapshot()->remote.at("remote").speaking,
        "Full speaker list failed to reset activity");
  auto room = join().room();
  room.set_metadata("new room metadata");
  room.set_active_recording(true);
  state.room(room);
  check(state.snapshot()->room.recording && state.snapshot()->room.metadata == "new room metadata",
        "Room state lost");
  auto track = participant("published", 1).tracks(0);
  state.local_publication(track);
  state.local_muted(track.sid(), true);
  check(state.snapshot()->local.publications.at(track.sid()).muted, "Local mute not observed");
  check(state.local_unpublished(track.sid()) &&
            !state.snapshot()->local.publications.contains(track.sid()),
        "Local remove not observed");
}

void local_publication_ordering() {
  SessionRoster state(join(), 1);
  const auto first = participant("first", 1).tracks(0), second = participant("second", 1).tracks(0);
  state.local_publication(first);
  state.local_publication(second);
  auto self = participant("local", 2);  // Snapshot predating both AddTrack responses.
  state.participant(self);
  check(state.snapshot()->local.publications.contains(first.sid()) &&
            state.snapshot()->local.publications.contains(second.sid()),
        "Delayed self update erased confirmed local publications");
  state.local_unpublished(first.sid());
  *self.add_tracks() = first;
  self.set_version(3);
  state.participant(self);
  check(!state.snapshot()->local.publications.contains(first.sid()) &&
            state.snapshot()->local.publications.contains(second.sid()),
        "Delayed self update revived an unpublished track or removed its sibling");
  check(!state.local_publication(first), "Late AddTrack response revived retired SID");
  SessionRoster rejoined(join(), 2);
  check(rejoined.local_publication(first), "Retired publication history leaked into new session");
}

void identity_replacement() {
  SessionRoster state(join(), 1);
  auto old = participant("old", 8);
  old.set_identity("viewer");
  state.participant(old);
  auto next = participant("new", 1);
  next.set_identity("viewer");
  state.participant(next);
  check(state.snapshot()->remote.size() == 1 && state.snapshot()->remote.contains("new"),
        "Rapid same-identity rejoin left two roster incarnations");
  old.set_version(9);
  old.set_state(3);
  check(!state.participant(old) &&
            state.snapshot()->remote.at("new").publications.contains("track-new"),
        "Retired SID disconnect changed the current identity");
  old.set_state(2);
  old.set_version(10);
  check(!state.participant(old) && state.snapshot()->remote.size() == 1,
        "Retired identity incarnation revived");
  for (int i = 0; i < 4095; ++i)
    state.participant(participant("filler-" + std::to_string(i), 1));
  auto replacement = participant("at-capacity", 1);
  replacement.set_identity("viewer");
  check(state.participant(replacement) && state.snapshot()->remote.size() == 4096 &&
            state.snapshot()->remote.contains("at-capacity"),
        "Full roster rejected same-identity replacement");
  auto absent = participant("late-disconnect", 1);
  absent.set_state(3);
  check(state.participant(absent) && state.snapshot()->remote.size() == 4096,
        "Full roster rejected a departure notification");
}
}  // namespace

int main() {
  try {
    aggregate_demand();
    policy_mapping();
    roster_contract();
    observations();
    local_publication_ordering();
    identity_replacement();
    std::cout << "RTC session roster contracts passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
