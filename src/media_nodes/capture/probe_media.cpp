#include "livekit/media.hpp"
#include "support/process.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>

namespace livekit::sdk {
std::vector<MediaSpec> probe_media(const std::string& input, const std::string& ffmpeg, bool loop,
                                   int width, int height, int fps) {
  if (input.starts_with("camera://") || input == "screen://primary") {
    MediaSpec spec{input, ffmpeg, true, false, false, width, height, fps};
    if (input.starts_with("camera://")) {
      auto index = input.substr(9);
      size_t used = 0;
      spec.device_index = std::stoi(index, &used);
      if (used != index.size() || spec.device_index < 0)
        throw std::invalid_argument("Invalid camera index");
      spec.capture = livekit::sdk::CaptureKind::camera;
    } else
      spec.capture = livekit::sdk::CaptureKind::screen;
    return {spec};
  }
  bool video = false, audio = false;
  // A raw socket is single-consumer: identify its type without opening it twice.
  if (input.starts_with("h264://") || input.starts_with("h265://") || input.starts_with("vp8://"))
    video = true;
  else if (input.starts_with("opus://"))
    audio = true;
  else {
    auto probe = std::filesystem::u8path(ffmpeg);
    probe.replace_filename(probe.extension() == ".exe" ? "ffprobe.exe" : "ffprobe");
    auto u8 = probe.u8string();
    std::vector<std::string> args{std::string(u8.begin(), u8.end()),
                                  "-v",
                                  "error",
                                  "-rw_timeout",
                                  "10000000",
                                  "-show_entries",
                                  "stream=codec_type",
                                  "-of",
                                  "json",
                                  "-i",
                                  input};
    auto info = nlohmann::json::parse(lk::Process::capture(args));
    for (const auto& stream : info.at("streams")) {
      video |= stream.value("codec_type", "") == "video";
      audio |= stream.value("codec_type", "") == "audio";
    }
  }
  std::vector<MediaSpec> result;
  for (bool is_video : {true, false})
    if (is_video ? video : audio)
      result.push_back({input, ffmpeg, is_video, false, loop, width, height, fps});
  if (result.empty())
    throw std::runtime_error("Input has no audio or video stream: " + input);
  return result;
}
}  // namespace livekit::sdk
