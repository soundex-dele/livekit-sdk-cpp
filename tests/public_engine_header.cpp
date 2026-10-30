// This target deliberately has no GraphFlow include directories or libraries.
#include "livekit/rtc/engine.hpp"
#include <type_traits>

static_assert(!std::is_copy_constructible_v<livekit::sdk::rtc::engine::RtcEngine>);

using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
static_assert(std::is_same_v<decltype(std::declval<PublishedTrack&>().set_muted(true)), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<PublishedTrack&>().close()), ErrorCode>);
static_assert(
    std::is_same_v<decltype(std::declval<SubscribedTrack&>().request_keyframe()), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<SubscribedTrack&>().set_demand({})), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<SubscribedTrack&>().close()), ErrorCode>);
static_assert(
    std::is_same_v<decltype(std::declval<const engine::MediaTrack&>().check()), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<RoomEngine&>().join({})), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<RoomEngine&>().publish({})),
                             std::shared_ptr<PublishedTrack>>);
static_assert(std::is_same_v<decltype(std::declval<engine::RtcEngine&>().publish({})), TrackId>);
static_assert(std::is_same_v<decltype(std::declval<engine::RtcEngine&>().publish_media({})),
                             engine::MediaPublication>);
