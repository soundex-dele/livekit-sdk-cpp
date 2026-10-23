#pragma once
#include "livekit/api_client.hpp"
#include <vector>

namespace livekit::sdk {
// No implicit environment reads. An explicit bearer token takes precedence.
class RoomServiceClient : public ServiceClient {
 public:
  RoomServiceClient(std::string url, Credentials credentials);
  // Exact literal overloads avoid ambiguity with the protobuf-JSON request API.
  Json create_room(const char* name, unsigned timeout = 300, unsigned max = 0, const std::string& metadata = "") const { return create_room(std::string(name), timeout, max, metadata); }
  Json delete_room(const char* room) const { return delete_room(std::string(room)); }
  Json list_participants(const char* room) const { return list_participants(std::string(room)); }
  Json list_rooms(const std::vector<std::string>& names = {}) const;
  Json create_room(const std::string& name, unsigned empty_timeout = 300,
                   unsigned max_participants = 0, const std::string& metadata = "") const;
  Json delete_room(const std::string& room) const;
  Json update_room_metadata(const std::string& room, const std::string& metadata) const;
  Json list_participants(const std::string& room) const;
  Json get_participant(const std::string& room, const std::string& identity) const;
  Json remove_participant(const std::string& room, const std::string& identity) const;
  Json mute_published_track(const std::string& room, const std::string& identity,
                            const std::string& track_sid, bool muted = true) const;
  Json create_room(const Json& request, const RequestOptions& options = {}) const;
  Json list_rooms(const Json& request, const RequestOptions& options = {}) const;
  Json delete_room(const Json& request, const RequestOptions& options = {}) const;
  Json list_participants(const Json& request, const RequestOptions& options = {}) const;
  Json get_participant(const Json& request, const RequestOptions& options = {}) const;
  Json remove_participant(const Json& request, const RequestOptions& options = {}) const;
  Json forward_participant(const Json& request, const RequestOptions& options = {}) const;
  Json move_participant(const Json& request, const RequestOptions& options = {}) const;
  Json mute_published_track(const Json& request, const RequestOptions& options = {}) const;
  Json update_participant(const Json& request, const RequestOptions& options = {}) const;
  Json update_subscriptions(const Json& request, const RequestOptions& options = {}) const;
  Json update_room_metadata(const Json& request, const RequestOptions& options = {}) const;
  Json send_data(const Json& request, const RequestOptions& options = {}) const;
 private:
  Json call(const std::string& method, const Json& grant, const Json& request) const;
};
}
