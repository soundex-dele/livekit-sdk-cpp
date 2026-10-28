#pragma once
#include "engines/rtc/subscribed_quality.hpp"
#include "engines/rtc/session.hpp"
#include <stdexcept>

inline void subscribed_quality_contract() {
  using livekit::sdk::detail::subscribed_video_enabled;
  auto check = [](bool ok) { if (!ok) throw std::runtime_error("Subscribed quality contract failed"); };
  livekit::SignalResponse response;
  // Upstream field 14, track T, H264 HIGH disabled. Not a self-serialization test.
  check(response.ParseFromString(std::string("\x72\x0f\x0a\x01T\x1a\x0a\x0a\x04h264\x12\x02\x08\x02", 17)));
  check(response.has_subscribed_quality_update());
  auto update = response.subscribed_quality_update();
  check(update.track_sid() == "T" && subscribed_video_enabled(update, "h264") == false);
  check(!subscribed_video_enabled(update, "h265").has_value());
  auto* c = update.mutable_subscribed_codecs(0);
  c->set_codec("video/H264"); c->mutable_qualities(0)->set_enabled(true);
  check(subscribed_video_enabled(update, "h264") == true);
  c->mutable_qualities(0)->set_quality(static_cast<livekit::VideoQuality>(17));
  check(!subscribed_video_enabled(update, "h264").has_value());
  update.Clear(); check(!subscribed_video_enabled(update, "h264").has_value());
  auto* legacy = update.add_subscribed_qualities(); legacy->set_quality(livekit::HIGH); legacy->set_enabled(true);
  check(subscribed_video_enabled(update, "h264") == true);
  legacy->set_enabled(false); check(subscribed_video_enabled(update, "h264") == false);
  // A foreign-codec message must never fall back to the deprecated qualities.
  update.add_subscribed_codecs()->set_codec("h265");
  check(!subscribed_video_enabled(update, "h264").has_value());
  livekit::sdk::MediaSpec media; media.dynacast = true; media.demo = true;
  livekit::sdk::detail::validate_media(media);
  media.video = false;
  bool rejected = false; try { livekit::sdk::detail::validate_media(media); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected);
  media.video = true; media.demo = false; media.input = "recording.mp4";
  rejected = false; try { livekit::sdk::detail::validate_media(media); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected);
}
