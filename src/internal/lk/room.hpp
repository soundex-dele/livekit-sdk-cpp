#pragma once
#include "lk/media.hpp"
#include "lk/events.hpp"
#include "lk/session.hpp"
namespace lk {
using livekit::sdk::RoomOptions;
void join_room(const RoomOptions& options, std::stop_token stop, EventEmitter event, std::shared_ptr<livekit::sdk::detail::RoomState> session = {});
void webrtc_smoke(EventEmitter event = EventEmitter{});
}
