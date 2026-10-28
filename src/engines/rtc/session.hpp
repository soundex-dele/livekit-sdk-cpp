#pragma once
#include "livekit/room.hpp"
#include "engines/rtc/recovery_state.hpp"
#include "engines/rtc/command.hpp"
#include "engines/rtc/operation_deadlines.hpp"
#include "media_nodes/render/video_stream.hpp"
#include "media_nodes/render/audio_stream.hpp"
#include "engines/rtc/rpc_session.hpp"
#include "engines/rtc/stream_session.hpp"
#include "engines/rtc/data_track_session.hpp"
#include "media_nodes/capture/encoded_source.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "support/control_executor.hpp"

namespace livekit::sdk::detail {
struct RoomState {
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool running = false;
  uint64_t generation = 0;
  ConnectionState connection = ConnectionState::disconnected;
  RoomInfo room;
  ParticipantInfo local;
  std::map<std::string, ParticipantInfo> remote;
  std::map<std::string, bool> subscriptions;
  std::stop_source stop;
  int reason = 0, command_timeout = 30;
  int signal_protocol = 12;
  SessionCapabilities capabilities;
  std::exception_ptr failure;
  std::string error;
  std::thread::id runner;
  RoomCallback callbacks;
  std::shared_ptr<ControlExecutor> control;
  void callback(std::function<void()> fn){if(control)control->post(std::move(fn));else fn();}
  std::deque<std::shared_ptr<Command>> commands;
  std::unique_ptr<OperationDeadlines> deadlines;
  std::shared_ptr<VideoFrameOutput> video_output;
  std::shared_ptr<AudioFrameOutput> audio_output;
  std::shared_ptr<EncodedOutput> encoded_output;
  std::shared_ptr<RpcSession> rpc;
  std::shared_ptr<StreamSession> streams;
  std::shared_ptr<DataTrackSession> data_tracks=std::make_shared<DataTrackSession>();
  // Caller holds mutex. Handler registrations survive explicit Room reuse.
  std::shared_ptr<StreamSession> stream_session() {
    if(!streams)streams=std::make_shared<StreamSession>();
    if(running&&!stop.stop_requested()&&!local.sid.empty()&&connection==ConnectionState::connected)streams->connect();
    return streams;
  }
  RecoveryState recovery; // runner-owned, no application-thread access
  void enqueue(uint64_t expected_generation, const std::shared_ptr<Command>& command);
  std::deque<std::shared_ptr<Command>> take_commands();
  // Caller holds mutex. Allocated only when RPC is used.
  std::shared_ptr<RpcSession> rpc_session() {
    if(!rpc)rpc=std::make_shared<RpcSession>();
    if(running&&!stop.stop_requested()&&!local.sid.empty()&&
        (connection==ConnectionState::connected||connection==ConnectionState::connecting))rpc->connect();
    return rpc;
  }
};
TrackInfo parse_track(const Json& value);
ParticipantInfo parse_participant(const Json& value);
void validate_media(const MediaSpec& media);
void process_event(const std::shared_ptr<RoomState>& state, const Json& event);
}
