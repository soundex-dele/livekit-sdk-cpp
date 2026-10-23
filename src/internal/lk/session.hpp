#pragma once
#include "livekit/room.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace livekit::sdk::detail {
struct Command {
  std::string operation;
  Json args;
  MediaSpec media;
  TrackPublishOptions publication;
  std::chrono::steady_clock::time_point deadline;
  std::function<void(const Json&, std::exception_ptr)> completion;
  bool completed = false; // only the native session thread resolves a removed command
  void resolve(const Json& result = Json::object()) { if (!completed) { completed = true; completion(result, {}); } }
  void reject(std::exception_ptr error) { if (!completed) { completed = true; completion({}, error); } }
};
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
  std::exception_ptr failure;
  std::string error;
  std::thread::id runner;
  RoomCallback callbacks;
  std::deque<std::shared_ptr<Command>> commands;
  void enqueue(uint64_t expected_generation, const std::shared_ptr<Command>& command);
  std::deque<std::shared_ptr<Command>> take_commands();
};
TrackInfo parse_track(const Json& value);
ParticipantInfo parse_participant(const Json& value);
void validate_media(const MediaSpec& media);
void process_event(const std::shared_ptr<RoomState>& state, const Json& event);
}
