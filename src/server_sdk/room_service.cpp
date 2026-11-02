#include "livekit/room_service.hpp"

namespace livekit::sdk {
RoomServiceClient::RoomServiceClient(std::string url, Credentials credentials)
    : ServiceClient(std::move(url), std::move(credentials)) {}

Result<Json> RoomServiceClient::call(const std::string& method, const Json& grant,
                                     const Json& body) const {
  return request("RoomService", method, body, {{"video", grant}}, {});
}

Result<Json> RoomServiceClient::list_rooms(const std::vector<std::string>& names) const {
  Json request = Json::object();
  if (!names.empty())
    request["names"] = names;
  return call("ListRooms", {{"roomList", true}}, request);
}

Result<Json> RoomServiceClient::create_room(const std::string& name, unsigned timeout, unsigned max,
                                            const std::string& metadata) const {
  return call("CreateRoom", {{"roomCreate", true}},
              {{"name", name},
               {"emptyTimeout", timeout},
               {"maxParticipants", max},
               {"metadata", metadata}});
}

Result<Json> RoomServiceClient::delete_room(const std::string& room) const {
  return call("DeleteRoom", {{"roomCreate", true}}, {{"room", room}});
}

Result<Json> RoomServiceClient::update_room_metadata(const std::string& room,
                                                     const std::string& metadata) const {
  return call("UpdateRoomMetadata", {{"roomAdmin", true}, {"room", room}},
              {{"room", room}, {"metadata", metadata}});
}

Result<Json> RoomServiceClient::list_participants(const std::string& room) const {
  return call("ListParticipants", {{"roomAdmin", true}, {"room", room}}, {{"room", room}});
}

Result<Json> RoomServiceClient::get_participant(const std::string& room,
                                                const std::string& identity) const {
  return call("GetParticipant", {{"roomAdmin", true}, {"room", room}},
              {{"room", room}, {"identity", identity}});
}

Result<Json> RoomServiceClient::remove_participant(const std::string& room,
                                                   const std::string& identity) const {
  return call("RemoveParticipant", {{"roomAdmin", true}, {"room", room}},
              {{"room", room}, {"identity", identity}});
}

Result<Json> RoomServiceClient::mute_published_track(const std::string& room,
                                                     const std::string& identity,
                                                     const std::string& sid, bool muted) const {
  return call("MutePublishedTrack", {{"roomAdmin", true}, {"room", room}},
              {{"room", room}, {"identity", identity}, {"trackSid", sid}, {"muted", muted}});
}
}  // namespace livekit::sdk
