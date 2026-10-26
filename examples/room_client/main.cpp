#include <livekit/livekit.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace sdk = livekit::sdk;
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: livekit_sdk_example URL [ROOM] [publish|subscribe]; set LIVEKIT_TOKEN\n";
    return 2;
  }
  try {
    const char* token = std::getenv("LIVEKIT_TOKEN");
    sdk::RoomCallback callbacks;
    callbacks.on_participant_connected = [](const sdk::RemoteParticipant& p) { std::cout << "Participant: " << p.identity() << '\n'; };
    callbacks.on_track_subscribed = [](const sdk::TrackPublication& t, const sdk::RemoteParticipant& p) { std::cout << "Subscribed: " << p.identity() << '/' << t.sid() << '\n'; };
    callbacks.on_data_received = [](const std::vector<uint8_t>& data, const sdk::DataReceiveParams& params) { std::cout << "Data: " << params.topic << ", " << data.size() << " bytes\n"; };
    sdk::RoomOptions options;
    if (argc > 2) options.room = argv[2]; // optional consistency check against token
    auto room = sdk::connect_to_room_with_token(argv[1], token ? token : "", callbacks, options);
    std::cout << "Connected: " << room->info().name << '\n';
    auto local = room->local_participant();
    std::vector<sdk::TrackPublication> publications;
    if (argc > 3 && std::string(argv[3]) == "publish") {
      sdk::MediaSpec video; video.demo = true;
      sdk::MediaSpec audio; audio.demo = true; audio.video = false;
      publications.push_back(local.publish_track(video, {"demo-video"}).get());
      publications.push_back(local.publish_track(audio, {"demo-audio"}).get());
      local.publish_data({'h', 'i'}, {true, "hello"}).get();
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    for (const auto& pub : publications) local.unpublish_track(pub.sid()).get();
    room->disconnect(); room->wait();
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
