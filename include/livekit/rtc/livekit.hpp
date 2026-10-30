#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/signaling_options.hpp"

namespace livekit::sdk::rtc {
struct LiveKitConnectionOptions final : BackendOptions {
  SignalingOptions signaling;
  EncryptionOptions encryption;
  // Offer FlexFEC video protection when supported by the remote endpoint.
  // Negotiation may fall back to unprotected video; redundancy is loss-adaptive.
  bool enable_flexfec = true;

  std::string backend_id() const override {
    return "livekit";
  }
};

// Value builder: conversion snapshots the backend-specific connection settings.
// No LiveKit protocol/region/token configuration leaks into RoomEngine itself.
struct LiveKitRoomOptions : RoomSettings, SignalingOptions {
  EncryptionOptions encryption;
  bool enable_flexfec = true;

  operator RoomOptions() const {
    RoomOptions result;
    static_cast<RoomSettings&>(result) = *this;
    auto connection = std::make_shared<LiveKitConnectionOptions>();
    connection->signaling = *this;
    connection->encryption = encryption;
    connection->enable_flexfec = enable_flexfec;
    result.backend = std::move(connection);
    return result;
  }
};

// Creates the LiveKit signaling/WebRTC adapter. Links LiveKit::RTCLiveKit;
// no engine, media graph, device or actual codec factory is required.
Result<std::unique_ptr<RoomEngine>> create_livekit_room();
}  // namespace livekit::sdk::rtc
