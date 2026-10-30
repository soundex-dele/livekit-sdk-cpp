#pragma once
#include "livekit/rtc/types.hpp"
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

// Engine construction is exposed by create_livekit_engine() in engine.hpp.
}  // namespace livekit::sdk::rtc
