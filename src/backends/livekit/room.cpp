#include "livekit/client/room.hpp"
#include "backends/livekit/room_session.hpp"

namespace livekit::sdk::client {
Room::Room() : session_(std::make_shared<detail::RoomSession>()) {}

Room::~Room() {
  session_->observer(nullptr);
  session_->close();
}

void Room::set_observer(RoomObserver* observer) {
  session_->observer(observer);
}

bool Room::connect(ConnectionOptions options) {
  return session_->connect(std::move(options));
}

void Room::close() {
  session_->close();
}

ConnectionState Room::connection_state() const {
  return session_->state();
}

RoomInfo Room::info() const {
  return session_->info();
}

LocalParticipant Room::local_participant() const {
  return session_->local();
}

std::vector<RemoteParticipant> Room::remote_participants() const {
  return session_->remotes();
}
}  // namespace livekit::sdk::client
