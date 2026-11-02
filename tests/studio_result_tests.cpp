#include "livekit/studio.hpp"
#include "engines/studio/internal.hpp"
#include "engines/studio/studio_graph.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace livekit::sdk;

void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
#ifdef _WIN32
  auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  check(SUCCEEDED(com), "COM initialization");
#endif
#ifdef _WIN32
  std::filesystem::path malformed_path(std::wstring(1, wchar_t(0xd800)) + L".mp4");
  const std::filesystem::path unicode_path(L"录制.mp4");
#else
  const std::filesystem::path malformed_path(std::string(1, char(0xff)) + ".mp4");
  const std::filesystem::path unicode_path("录制.mp4");
#endif
  auto unicode_text = studio_detail::path8(unicode_path);
  check(unicode_text && *unicode_text == "录制.mp4", "recording path must preserve UTF-8");
  auto malformed = studio_detail::path8(malformed_path);
  check(!malformed && malformed.error().code == ErrorCode::invalid_argument,
        "invalid native path must return failure");
  {
    studio_detail::Recorder recorder(malformed_path, 1280, 720, 30);
    auto ready = recorder.status();
    check(!ready && ready.error().code == ErrorCode::invalid_argument,
          "recorder rejects invalid path before opening output");
    check(!recorder.finish(), "invalid recorder retains its failure");
  }
  StudioOptions invalid;
  invalid.width = 0;
  Studio unavailable(invalid);
  check(!unavailable.status(), "invalid Studio options must produce Failure");
  {
    studio_detail::StudioGraphState state(invalid);
    studio_detail::StudioGraph graph(state);
    check(!graph.start(), "invalid graph options must fail before start");
  }
  StudioSource source;
  source.id = "color";
  source.color = {1, 0, 0, 1};
  auto image = std::make_shared<StudioImage>();
  image->width = image->height = 2;
  image->rgba = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  auto composed = studio_composite_cpu(8, 8, {{source, image}});
  check(composed && composed->rgba[0] == 255, "CPU composition");
  image->rgba.clear();
  check(!studio_composite_cpu(8, 8, {{source, image}}), "short image buffer");
  check(!studio_detail::to_i420(*image), "short conversion buffer");
  check(!studio_detail::from_i420(VideoFrame{}), "invalid I420 frame");
  std::vector<float> samples(2);
  check(!studio_mix(samples, samples, std::numeric_limits<float>::infinity()), "invalid mix gain");
  std::error_code ec;
  auto directory = std::filesystem::temp_directory_path(ec) /
                   ("livekit-studio-result-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(!ec, "temporary directory");
  std::filesystem::create_directories(directory, ec);
  check(!ec, "create fixtures");
  auto path = directory / "project.json";
  StudioOptions options;
  options.software_gpu = true;
  StudioAudioSettings audio;
  StudioScene scene;
  scene.sources = {source};
  check(bool(Studio::save_project(path, {scene}, 0, options, audio)), "save valid project");
  size_t selected = 7;
  auto loaded = Studio::load_project(path, selected, options, audio);
  check(loaded && loaded->size() == 1 && selected == 0, "load valid project");
  for (const auto* document : {"{", "{}",
                               "{\"version\":1,\"selected\":0,\"width\":1280,\"height\":720,"
                               "\"scenes\":null,\"audio\":{}}"}) {
    {
      std::ofstream file(path);
      file << document;
    }
    selected = 7;
    check(!Studio::load_project(path, selected, options, audio),
          "malformed project returns failure");
    check(selected == 7, "failed load must not mutate selection");
  }
  {
    studio_detail::StudioGraphState state(options);
    state.scene = scene;
    studio_detail::StudioGraph graph(state);
    check(!graph.step(), "step before start");
    check(bool(graph.start()), "graph starts");
    check(bool(graph.step()) && state.status.frames == 1, "graph produces preview");
    graph.stop();
    check(!graph.step(), "step after stop");
  }
  {
    studio_detail::StudioGraphState state(options);
    state.scene = scene;
    state.settings.microphone_volume = 5;
    studio_detail::StudioGraph graph(state);
    check(bool(graph.start()), "error graph starts");
    auto first = graph.step();
    auto second = graph.step();
    check(!first && !second && first.failure_record() == second.failure_record(),
          "graph preserves processing failure");
  }
  studio_detail::Recorder invalid_recorder(directory / "invalid.mp4", 0, 720, 30);
  check(!invalid_recorder.status() && !invalid_recorder.finish(),
        "recorder initialization failure");
  std::filesystem::remove_all(directory, ec);
#ifdef _WIN32
  CoUninitialize();
#endif
  std::cout << "studio result contracts passed\n";
}
