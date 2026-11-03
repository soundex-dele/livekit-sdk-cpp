#pragma once

#include "livekit/client/room_observer.hpp"
#include <functional>

namespace livekit::sdk::client {
namespace detail {
class RoomSession;
struct RoomTestAccess;
}  // namespace detail

// Synchronous room owner. Destroy on an external owner thread. close() may
// interrupt operations on other threads, but destruction requires those calls
// to have returned. A closed/failed Room is not reused for a new connection.
class Room {
 public:
  Room();
  ~Room();
  Room(const Room&) = delete;
  Room& operator=(const Room&) = delete;
  void set_observer(RoomObserver* observer);
  bool connect(ConnectionOptions options);
  void close();
  ConnectionState connection_state() const;
  RoomInfo info() const;
  LocalParticipant local_participant() const;
  std::vector<RemoteParticipant> remote_participants() const;
  Result<std::vector<PeerStatistics>> statistics();

 private:
  friend struct detail::RoomTestAccess;
  std::shared_ptr<detail::RoomSession> session_;
};
}  // namespace livekit::sdk::client
