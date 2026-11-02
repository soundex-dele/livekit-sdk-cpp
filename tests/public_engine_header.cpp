// Public control headers do not require GraphFlow include directories.
#include "livekit/rtc/engine.hpp"
#include <type_traits>
using namespace livekit::sdk::rtc;
using engine::RtcEngine;
static_assert(!std::is_copy_constructible_v<RtcEngine>);
static_assert(!std::is_constructible_v<RtcEngine, RoomEngine&>);
static_assert(std::is_same_v<decltype(create_livekit_room()), std::unique_ptr<RoomEngine>>);
static_assert(std::is_base_of_v<RoomObserver, RtcEngine>);
static_assert(std::is_same_v<decltype(std::declval<RtcEngine&>().join({})), void>);
static_assert(std::is_same_v<decltype(std::declval<RtcEngine&>().publish({})), void>);
static_assert(std::is_same_v<decltype(std::declval<RtcEngine&>().publish_media({})), void>);
static_assert(std::is_same_v<decltype(std::declval<RoomEngine&>().join({})), void>);
static_assert(
    std::is_same_v<decltype(engine::create_livekit_engine()), std::unique_ptr<RtcEngine>>);

// Results arrive through callbacks; neither facade exposes a polling API.
template <class T, class = void>
struct HasPoll : std::false_type {};

template <class T>
struct HasPoll<T, std::void_t<decltype(std::declval<T&>().poll())>> : std::true_type {};

static_assert(!HasPoll<RtcEngine>::value);
static_assert(!HasPoll<RoomEngine>::value);
