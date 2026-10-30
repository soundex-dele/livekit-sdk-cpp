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
                             ErrorCode>);
static_assert(
    std::is_same_v<decltype(std::declval<engine::RtcEngine&>().publish({})), ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<engine::RtcEngine&>().publish_media({})),
                             ErrorCode>);

static_assert(std::is_same_v<decltype(std::declval<RoomEngine&>().subscribe({})),
                             ErrorCode>);
static_assert(std::is_same_v<decltype(std::declval<const RoomView&>().services()),
                             Result<std::shared_ptr<RoomServices>>>);
static_assert(std::is_same_v<decltype(engine::create_livekit_engine()),
                             Result<std::unique_ptr<engine::RtcEngine>>>);
static_assert(std::is_same_v<decltype(engine::create_livekit_engine(AudioOutputOptions{})),
                             Result<std::unique_ptr<engine::RtcEngine>>>);
