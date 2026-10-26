#include "media_nodes/recording.hpp"
#include <cstring>
#include <cctype>
namespace lk {
static std::string safe_name(const std::string& value) {
  std::string result; for (unsigned char c : value) result += (std::isalnum(c) || c == '-' || c == '_') ? c : '_';
  return result.empty() ? "track" : result;
}
void RecordingNode::open_video(int width, int height) {
  if (file_.is_open()) file_.close();
  width_ = width; height_ = height;
  auto path = base_; path += "-" + std::to_string(part_++) + ".y4m";
  if (std::filesystem::exists(path)) throw std::runtime_error("Recording already exists: " + path.string());
  file_.open(path, std::ios::binary); if (!file_) throw std::runtime_error("Cannot open video recording");
  // Y4M has a nominal 30 fps time base; it does not preserve RTP timestamps.
  file_ << "YUV4MPEG2 W" << width << " H" << height << " F30:1 Ip A1:1 C420jpeg\n";
}
void RecordingNode::write_video(const webrtc::VideoFrame& frame) {
  try {
    if (!error().empty()) return;
    auto b = frame.video_frame_buffer()->ToI420();
    if (!b) throw std::runtime_error("Video frame cannot convert to I420");
    if (!file_.is_open() || width_ != b->width() || height_ != b->height()) open_video(b->width(), b->height());
    file_ << "FRAME\n";
    for (int y = 0; y < height_; ++y) file_.write(reinterpret_cast<const char*>(b->DataY() + y * b->StrideY()), width_);
    for (int y = 0; y < (height_ + 1) / 2; ++y) file_.write(reinterpret_cast<const char*>(b->DataU() + y * b->StrideU()), (width_ + 1) / 2);
    for (int y = 0; y < (height_ + 1) / 2; ++y) file_.write(reinterpret_cast<const char*>(b->DataV() + y * b->StrideV()), (width_ + 1) / 2);
    if (!file_) throw std::runtime_error("Video recording write failed");
    ++outputs;
  } catch (const std::exception& e) { fail(e.what()); }
}
static void le(std::ostream& out, uint32_t value, int size) { for (int i = 0; i < size; ++i) out.put(static_cast<char>(value >> (8 * i))); }
void RecordingNode::close_audio() {
  if (video_ || !file_.is_open()) return;
  file_.seekp(4); le(file_, static_cast<uint32_t>(bytes_ + 36), 4);
  file_.seekp(40); le(file_, static_cast<uint32_t>(bytes_), 4); file_.close();
}
void RecordingNode::write_audio(const void* data, int bits, int rate, size_t channels, size_t frames) {
  try {
    if (!error().empty()) return;
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
    ++outputs;
  } catch (const std::exception& e) { fail(e.what()); }
}

RecordingNode::RecordingNode(const std::filesystem::path& directory, const std::string& id, bool video)
    : MediaNode("RecordingNode", livekit::sdk::detail::NodeExecution::sink), video_(video) {
  std::filesystem::create_directories(directory); base_ = directory / safe_name(id);
}
RecordingNode::~RecordingNode() { release(); }
bool RecordingNode::doInit() {
  if (video_) subscribe<livekit::sdk::detail::RawVideoFrame>([this](auto& event) { submit(event.frame); });
  else subscribe<livekit::sdk::detail::PcmFrame>([this](auto& event) {
    submit(event.data, event.bits, event.rate, event.channels, event.frames);
  });
  return true;
}
bool RecordingNode::doStart() { queue_.reopen(); thread_ = std::thread([this] { run(); }); return true; }
void RecordingNode::doStop() {
  queue_.close(true); if (thread_.joinable()) thread_.join(); close_audio();
  if (file_.is_open()) file_.close();
}
void RecordingNode::submit(const webrtc::VideoFrame& frame) {
  ++inputs;
  const size_t bytes = static_cast<size_t>(frame.width()) * frame.height() * 4;
  if (!queue_.push(Item(frame), bytes, 0)) { ++dropped; fail("Recording queue overflow"); }
}
void RecordingNode::submit(const void* data, int bits, int rate, size_t channels, size_t frames) {
  ++inputs;
  if (bits != 16 || channels < 1 || channels > 8 || frames > 5760) { fail("Invalid recording PCM format"); return; }
  AudioBlock block{rate, channels, frames, std::vector<int16_t>(frames * channels)};
  std::memcpy(block.data.data(), data, block.data.size() * sizeof(int16_t));
  if (!queue_.push(Item(std::move(block)), frames * channels * 2, 0)) { ++dropped; fail("Recording queue overflow"); }
}
void RecordingNode::run() {
  while (auto item = queue_.wait_pop()) {
    if (auto* video = std::get_if<webrtc::VideoFrame>(&*item)) write_video(*video);
    else { auto& audio = std::get<AudioBlock>(*item);
      write_audio(audio.data.data(), 16, audio.rate, audio.channels, audio.frames); }
  }
}
void RecordingNode::fail(const std::string& message) {
  ++errors; std::lock_guard lock(mutex_); if (error_.empty()) error_ = message;
}
std::string RecordingNode::error() const { std::lock_guard lock(mutex_); return error_; }
}
