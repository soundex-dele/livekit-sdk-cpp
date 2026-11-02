#include "livekit/compat/algorithm.hpp"
#include "livekit/media.hpp"
#include "support/process.hpp"
#include <nlohmann/json.hpp>
#include <charconv>

namespace livekit::sdk {
Result<std::vector<MediaSpec>> probe_media(const std::string& input, const std::string& ffmpeg,
                                           bool loop, int width, int height, int fps) {
  if (livekit::compat::starts_with(input, "camera://") || input == "screen://primary") {
    MediaSpec spec{input, ffmpeg, true, false, false, width, height, fps};
    if (livekit::compat::starts_with(input, "camera://")) {
      auto index = input.substr(9);
      const auto parsed =
          std::from_chars(index.data(), index.data() + index.size(), spec.device_index);
      if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() ||
          spec.device_index < 0) {
        return Result<std::vector<MediaSpec>>::failure(
            make_failure(ErrorDomain::media, ErrorCode::invalid_argument, "Invalid camera index"));
      }
      spec.capture = livekit::sdk::CaptureKind::camera;
    } else
      spec.capture = livekit::sdk::CaptureKind::screen;
    return Result<std::vector<MediaSpec>>::success({std::move(spec)});
  }
  bool video = false, audio = false;
  // A raw socket is single-consumer: identify its type without opening it twice.
  if (livekit::compat::starts_with(input, "h264://") ||
      livekit::compat::starts_with(input, "h265://") ||
      livekit::compat::starts_with(input, "vp8://"))
    video = true;
  else if (livekit::compat::starts_with(input, "opus://"))
    audio = true;
  else {
    const auto separator = ffmpeg.find_last_of("/\\");
    const auto directory = separator == std::string::npos ? std::string{} : ffmpeg.substr(0, separator + 1);
    const auto probe =
        directory + (livekit::compat::ends_with(ffmpeg, ".exe") ? "ffprobe.exe" : "ffprobe");
    std::vector<std::string> args{
        probe, "-v",   "error", "-rw_timeout", "10000000", "-show_entries", "stream=codec_type",
        "-of", "json", "-i",    input};
    auto captured = lk::Process::capture(args);
    if (!captured) {
      return Result<std::vector<MediaSpec>>::failure(captured.failure_record());
    }
    auto info = nlohmann::json::parse(*captured, nullptr, false);
    if (!info.is_object() || !info.contains("streams") || !info["streams"].is_array()) {
      return Result<std::vector<MediaSpec>>::failure(
          make_failure(ErrorDomain::media, ErrorCode::malformed_message,
                       "ffprobe returned an invalid stream list"));
    }
    for (const auto& stream : info["streams"]) {
      if (!stream.is_object() || !stream.contains("codec_type") ||
          !stream["codec_type"].is_string()) {
        return Result<std::vector<MediaSpec>>::failure(
            make_failure(ErrorDomain::media, ErrorCode::malformed_message,
                         "ffprobe returned an invalid codec type"));
      }
      video |= stream["codec_type"] == "video";
      audio |= stream["codec_type"] == "audio";
    }
  }
  std::vector<MediaSpec> result;
  for (bool is_video : {true, false})
    if (is_video ? video : audio)
      result.push_back({input, ffmpeg, is_video, false, loop, width, height, fps});
  if (result.empty())
    return Result<std::vector<MediaSpec>>::failure(
        make_failure(ErrorDomain::media, ErrorCode::unsupported,
                     "Input has no audio or video stream: " + input));
  return Result<std::vector<MediaSpec>>::success(std::move(result));
}
}  // namespace livekit::sdk
