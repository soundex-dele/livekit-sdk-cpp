#include "lk/media.hpp"
#include "lk/video_playout.hpp"
#include "lk/video_frames.hpp"
#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/time_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
namespace lk {
void AudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); sinks_.push_back(sink); }
void AudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); std::erase(sinks_, sink); }
void AudioSource::push(const int16_t* samples) {
  std::lock_guard lock(mutex_);
  for (auto* sink : sinks_) sink->OnData(samples, 16, 48000, 1, 480);
}
static std::vector<std::string> input_args(std::string input) {
  for (const auto& codec : {"h264", "h265", "vp8", "opus"}) {
    auto prefix = std::string(codec) + "://";
    if (input.starts_with(prefix)) {
      auto address = input.substr(prefix.size());
      auto transport = address.starts_with('/') ? "unix:" + address : "tcp://" + address;
      auto format = std::string(codec) == "h265" ? "hevc" : std::string(codec) == "vp8" ? "ivf" : std::string(codec) == "opus" ? "ogg" : "h264";
      return {"-probesize", "32768", "-analyzeduration", "0", "-f", format, "-i", transport};
    }
  }
  return {"-i", input};
}
std::vector<MediaSpec> probe_media(const std::string& input, const std::string& ffmpeg, bool loop, int width, int height, int fps) {
  bool video = false, audio = false;
  // A raw socket is single-consumer: identify its type without opening it twice.
  if (input.starts_with("h264://") || input.starts_with("h265://") || input.starts_with("vp8://")) video = true;
  else if (input.starts_with("opus://")) audio = true;
  else {
    auto probe = std::filesystem::u8path(ffmpeg);
    probe.replace_filename(probe.extension() == ".exe" ? "ffprobe.exe" : "ffprobe");
    auto u8 = probe.u8string();
    std::vector<std::string> args{std::string(u8.begin(), u8.end()), "-v", "error", "-rw_timeout", "10000000", "-show_entries", "stream=codec_type", "-of", "json", "-i", input};
    auto info = Json::parse(Process::capture(args));
    for (const auto& stream : info.at("streams")) { video |= stream.value("codec_type", "") == "video"; audio |= stream.value("codec_type", "") == "audio"; }
  }
  std::vector<MediaSpec> result;
  for (bool is_video : {true, false}) if (is_video ? video : audio) result.push_back({input, ffmpeg, is_video, false, loop, width, height, fps});
  if (result.empty()) throw std::runtime_error("Input has no audio or video stream: " + input);
  return result;
}
MediaPump::MediaPump(MediaSpec value) : spec(std::move(value)) {
  if (spec.video) video = webrtc::make_ref_counted<VideoSource>();
  else audio = webrtc::make_ref_counted<AudioSource>();
}
MediaPump::~MediaPump() { stop(); }
void MediaPump::start() { if (!thread_.joinable()) thread_ = std::thread([this] { run(); }); }
void MediaPump::stop() {
  stop_ = true;
  { std::lock_guard lock(mutex_); if (process_) process_->terminate(); }
  if (thread_.joinable()) thread_.join();
}
std::string MediaPump::error() const { std::lock_guard lock(mutex_); return error_; }
void MediaPump::run() {
  try {
    std::shared_ptr<Process> child;
    if (!spec.demo) {
      std::vector<std::string> args{spec.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1", "-filter_threads", "1", "-rw_timeout", "10000000"};
      if (spec.loop) args.insert(args.end(), {"-stream_loop", "-1"});
      auto in = input_args(spec.input); args.insert(args.end(), in.begin(), in.end());
      if (spec.video) args.insert(args.end(), {"-map", "0:v:0", "-an", "-vf", "scale=" + std::to_string(spec.width) + ":" + std::to_string(spec.height) + ":force_original_aspect_ratio=decrease,pad=" + std::to_string(spec.width) + ":" + std::to_string(spec.height) + ":(ow-iw)/2:(oh-ih)/2,fps=" + std::to_string(spec.fps), "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"});
      else args.insert(args.end(), {"-map", "0:a:0", "-vn", "-ac", "1", "-ar", "48000", "-f", "s16le", "pipe:1"});
      std::lock_guard lock(mutex_);
      if (stop_) { finished_ = true; return; }
      process_ = child = std::make_shared<Process>(args);
    }
    const auto period = std::chrono::microseconds(spec.video ? 1000000 / spec.fps : 10000);
    auto next = std::chrono::steady_clock::now();
    std::vector<uint8_t> pixels(static_cast<size_t>(spec.width) * spec.height * 3 / 2);
    int16_t samples[480]{};
    while (!stop_) {
      if (spec.video) {
        if (child && !child->read_exact(pixels.data(), pixels.size())) break;
        auto buffer = webrtc::I420Buffer::Create(spec.width, spec.height);
        if (child) {
          for (int row = 0; row < spec.height; ++row) std::memcpy(buffer->MutableDataY() + row * buffer->StrideY(), pixels.data() + row * spec.width, spec.width);
          for (int row = 0; row < spec.height / 2; ++row) {
            std::memcpy(buffer->MutableDataU() + row * buffer->StrideU(), pixels.data() + spec.width * spec.height + row * spec.width / 2, spec.width / 2);
            std::memcpy(buffer->MutableDataV() + row * buffer->StrideV(), pixels.data() + spec.width * spec.height * 5 / 4 + row * spec.width / 2, spec.width / 2);
          }
        } else {
          for (int y = 0; y < spec.height; ++y) for (int x = 0; x < spec.width; ++x) buffer->MutableDataY()[y * buffer->StrideY() + x] = static_cast<uint8_t>(16 + ((x + y + frames.load() * 3) % 220));
          std::memset(buffer->MutableDataU(), 96, buffer->StrideU() * spec.height / 2);
          std::memset(buffer->MutableDataV(), 160, buffer->StrideV() * spec.height / 2);
        }
        video->push(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(webrtc::TimeMicros()).build());
      } else {
        if (child) {
          size_t used = 0;
          while (used < sizeof(samples)) { auto count = child->read(reinterpret_cast<char*>(samples) + used, sizeof(samples) - used); if (!count) break; used += count; }
          if (!used) break;
          if (used % 2) throw std::runtime_error("Truncated PCM sample");
          // A finite audio file need not end on a 10 ms boundary.
          std::memset(reinterpret_cast<char*>(samples) + used, 0, sizeof(samples) - used);
        }
        if (!child) for (int i = 0; i < 480; ++i) samples[i] = static_cast<int16_t>(6000 * std::sin(2 * std::numbers::pi * 440 * (frames.load() * 480 + i) / 48000.0));
        audio->push(samples);
      }
      ++frames;
      next += period;
      if (next < std::chrono::steady_clock::now() - period * 5) next = std::chrono::steady_clock::now();
      std::this_thread::sleep_until(next);
    }
    if (child && !stop_ && child->wait() != 0) throw std::runtime_error("FFmpeg decoder exited with an error");
    if (!stop_ && frames == 0) throw std::runtime_error("Input produced no media frames");
  } catch (const std::exception& e) { if (!stop_) { std::lock_guard lock(mutex_); error_ = e.what(); } }
  finished_ = true;
}
static std::string safe_name(const std::string& value) {
  std::string result; for (unsigned char c : value) result += (std::isalnum(c) || c == '-' || c == '_') ? c : '_';
  return result.empty() ? "track" : result;
}
TrackSink::TrackSink(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::filesystem::path& directory, VideoPlayout* playout, VideoFrames* frames)
    : track_(std::move(track)), playout_(playout), frame_output_(frames) {
  if (!directory.empty()) { std::filesystem::create_directories(directory); base_ = directory / safe_name(track_->id()); }
}
TrackSink::~TrackSink() { detach(); close_audio(); }
void TrackSink::attach() {
  if (track_->kind() == "video") {
    if (playout_) playout_->add_track(track_->id());
    if (frame_output_) frame_output_->add(track_->id());
    static_cast<webrtc::VideoTrackInterface*>(track_.get())->AddOrUpdateSink(this, webrtc::VideoSinkWants{});
  }
  else static_cast<webrtc::AudioTrackInterface*>(track_.get())->AddSink(this);
  attached_ = true;
}
void TrackSink::detach() {
  if (!attached_) return;
  if (track_->kind() == "video") {
    static_cast<webrtc::VideoTrackInterface*>(track_.get())->RemoveSink(this);
    if (playout_) playout_->remove_track(track_->id());
    if (frame_output_) frame_output_->remove(track_->id());
  }
  else static_cast<webrtc::AudioTrackInterface*>(track_.get())->RemoveSink(this);
  attached_ = false;
}
void TrackSink::open_video(int width, int height) {
  if (file_.is_open()) file_.close();
  width_ = width; height_ = height;
  auto path = base_; path += "-" + std::to_string(part_++) + ".y4m";
  if (std::filesystem::exists(path)) throw std::runtime_error("Recording already exists: " + path.string());
  file_.open(path, std::ios::binary); if (!file_) throw std::runtime_error("Cannot open video recording");
  // Y4M has a nominal 30 fps time base; it does not preserve RTP timestamps.
  file_ << "YUV4MPEG2 W" << width << " H" << height << " F30:1 Ip A1:1 C420jpeg\n";
}
void TrackSink::OnFrame(const webrtc::VideoFrame& frame) {
  std::lock_guard lock(mutex_);
  try {
    ++frames_;
    if (playout_) playout_->submit(track_->id(), frame);
    if (frame_output_) frame_output_->submit(track_->id(), frame);
    if (base_.empty() || !error_.empty()) return;
    auto b = frame.video_frame_buffer()->ToI420();
    if (!b) throw std::runtime_error("Video frame cannot convert to I420");
    if (!file_.is_open() || width_ != b->width() || height_ != b->height()) open_video(b->width(), b->height());
    file_ << "FRAME\n";
    for (int y = 0; y < height_; ++y) file_.write(reinterpret_cast<const char*>(b->DataY() + y * b->StrideY()), width_);
    for (int y = 0; y < (height_ + 1) / 2; ++y) file_.write(reinterpret_cast<const char*>(b->DataU() + y * b->StrideU()), (width_ + 1) / 2);
    for (int y = 0; y < (height_ + 1) / 2; ++y) file_.write(reinterpret_cast<const char*>(b->DataV() + y * b->StrideV()), (width_ + 1) / 2);
    if (!file_) throw std::runtime_error("Video recording write failed");
  } catch (const std::exception& e) { error_ = e.what(); }
}
static void le(std::ostream& out, uint32_t value, int size) { for (int i = 0; i < size; ++i) out.put(static_cast<char>(value >> (8 * i))); }
void TrackSink::close_audio() {
  if (track_->kind() != "audio" || !file_.is_open()) return;
  file_.seekp(4); le(file_, static_cast<uint32_t>(bytes_ + 36), 4);
  file_.seekp(40); le(file_, static_cast<uint32_t>(bytes_), 4); file_.close();
}
void TrackSink::OnData(const void* data, int bits, int rate, size_t channels, size_t frames) {
  std::lock_guard lock(mutex_);
  try {
    ++frames_;
    if (base_.empty() || !error_.empty()) return;
    if (bits != 16) throw std::runtime_error("Expected 16-bit decoded PCM");
    auto count = frames * channels * 2;
    if (!file_.is_open() || rate_ != rate || channels_ != static_cast<int>(channels) || bytes_ + count > 0xffff0000ULL) {
      close_audio(); bytes_ = 0; rate_ = rate; channels_ = static_cast<int>(channels);
      auto path = base_; path += "-" + std::to_string(part_++) + ".wav";
      if (std::filesystem::exists(path)) throw std::runtime_error("Recording already exists");
      file_.open(path, std::ios::binary); if (!file_) throw std::runtime_error("Cannot open audio recording");
      file_ << "RIFF"; le(file_, 36, 4); file_ << "WAVEfmt "; le(file_, 16, 4); le(file_, 1, 2); le(file_, channels_, 2); le(file_, rate, 4); le(file_, rate * channels_ * 2, 4); le(file_, channels_ * 2, 2); le(file_, 16, 2); file_ << "data"; le(file_, 0, 4);
    }
    file_.write(static_cast<const char*>(data), count); bytes_ += count;
    if (!file_) throw std::runtime_error("Audio recording write failed");
  } catch (const std::exception& e) { error_ = e.what(); }
}
std::string TrackSink::error() const { std::lock_guard lock(mutex_); return error_; }
Json TrackSink::stats() const { std::lock_guard lock(mutex_); return {{"track", track_->id()}, {"kind", track_->kind()}, {"frames", frames_}}; }
}
