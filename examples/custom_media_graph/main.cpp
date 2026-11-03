#include "watermark.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: livekit_custom_media_graph_example <url> <token>\n";
    return 1;
  }
  using namespace livekit::sdk::client;
  MediaEngine engine;
  ConnectionOptions connection;
  connection.url = argv[1];
  connection.token = argv[2];
  if (!engine.connect(std::move(connection))) {
    return 1;
  }
  MediaPublishSpec spec;
  spec.name = "watermarked-video";
  spec.format.codec = Codec::vp8;
  spec.format.width = 640;
  spec.format.height = 360;
  spec.input.generated = true;
  spec.graph = livekit::sdk::examples::watermark_graph();
  auto publication = engine.publish_media(std::move(spec));
  if (!publication) {
    std::cerr << publication.error().message << '\n';
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(5));
  auto snapshot = publication->track->snapshot();
  engine.close();
  std::cout << "Published " << snapshot.frames << " encoded frames through "
            << snapshot.nodes.size() << " graph nodes\n";
  return snapshot.failure || !snapshot.frames ? 1 : 0;
}
