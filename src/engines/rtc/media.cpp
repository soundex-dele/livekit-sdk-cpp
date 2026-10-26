#include "engines/rtc/media.hpp"
#include "media_nodes/render/video_playout.hpp"
#include "media_nodes/render/video_frames.hpp"
#include "api/make_ref_counted.h"
#include <algorithm>
namespace lk {
void AudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); sinks_.push_back(sink); }
void AudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); std::erase(sinks_, sink); }
void AudioSource::push(const int16_t* samples) {
  std::lock_guard lock(mutex_);
  for (auto* sink : sinks_) sink->OnData(samples, 16, 48000, 1, 480);
}
std::vector<MediaSpec> probe_media(const std::string& input, const std::string& ffmpeg, bool loop, int width, int height, int fps) {
  if (input.starts_with("camera://") || input == "screen://primary") {
    MediaSpec spec{input, ffmpeg, true, false, false, width, height, fps};
    if (input.starts_with("camera://")) {
      auto index = input.substr(9); size_t used = 0;
      spec.device_index = std::stoi(index, &used);
      if (used != index.size() || spec.device_index < 0) throw std::invalid_argument("Invalid camera index");
      spec.capture = livekit::sdk::CaptureKind::camera;
    } else spec.capture = livekit::sdk::CaptureKind::screen;
    return {spec};
  }
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
MediaPump::MediaPump(MediaSpec value, std::shared_ptr<livekit::sdk::detail::RtcGraph> graph,
                     VideoPlayout* playout, VideoFrames* output_frames, std::string id)
    : capture_(std::make_shared<CaptureNode>(std::move(value))), spec(capture_->spec), frames(capture_->frames) {
  using namespace livekit::sdk::detail;
  if (!graph) graph = std::make_shared<RtcGraph>();
  pipeline_ = std::make_unique<RtcPipeline>(graph);
  std::vector<std::shared_ptr<MediaNode>> nodes{capture_};
  std::vector<RtcPipeline::Connection> edges;
  if (spec.video) {
    video = webrtc::make_ref_counted<VideoSource>(spec.capture == livekit::sdk::CaptureKind::screen);
    sender_ = std::make_unique<GraphVideoSender>(graph,
        [source = video](const webrtc::VideoFrame& frame) { source->push(frame); }, false);
    auto encoding = sender_->nodes(); nodes.insert(nodes.end(), encoding.begin(), encoding.end());
    edges = sender_->connections();
    edges.push_back(RtcPipeline::connect<RawVideoFrame>(capture_, sender_->input_node()));
    if (spec.local_preview) {
      auto preview = std::make_shared<VideoRenderNode>("local-" + id, playout, output_frames);
      nodes.push_back(preview); edges.push_back(RtcPipeline::connect<RawVideoFrame>(capture_, preview));
    }
  } else {
    audio = webrtc::make_ref_counted<AudioSource>();
    auto output = std::make_shared<MediaEventNode<PcmFrame>>("AudioTrackInputNode", NodeExecution::sink,
        [source = audio](PcmFrame& event) { source->push(static_cast<const int16_t*>(event.data)); });
    nodes.push_back(output); edges.push_back(RtcPipeline::connect<PcmFrame>(capture_, output));
  }
  pipeline_->build(nodes, edges);
}
MediaPump::~MediaPump() { pipeline_->release(); }
bool MediaPump::start() {
  if (!pipeline_->start()) return false;
  try { capture_->activate(); }
  catch (...) { pipeline_->stop(); throw; }
  return true;
}
void MediaPump::stop() { pipeline_->stop(); }
TrackSink::TrackSink(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::filesystem::path& directory, VideoPlayout* playout, VideoFrames* frames, std::string track_id, std::shared_ptr<livekit::sdk::detail::RtcGraph> graph)
    : track_(std::move(track)), track_id_(track_id.empty() ? track_->id() : std::move(track_id)), playout_(playout), frame_output_(frames) {
  if (!graph) graph = std::make_shared<livekit::sdk::detail::RtcGraph>();
  using namespace livekit::sdk::detail;
  pipeline_ = std::make_unique<RtcPipeline>(graph);
  if (track_->kind() == "video") {
    video_input_ = std::make_shared<MediaEventNode<RawVideoFrame>>("RtcSubscribeNode", NodeExecution::source);
    subscribe_node_ = video_input_;
    renderer_ = std::make_shared<VideoRenderNode>(track_id_, playout, frames);
  } else {
    audio_input_ = std::make_shared<MediaEventNode<PcmFrame>>("RtcSubscribeNode", NodeExecution::source);
    subscribe_node_ = audio_input_;
  }
  std::vector<std::shared_ptr<MediaNode>> nodes{subscribe_node_};
  std::vector<RtcPipeline::Connection> edges;
  if (renderer_) {
    nodes.push_back(renderer_); edges.push_back(RtcPipeline::connect<RawVideoFrame>(subscribe_node_, renderer_));
  }
  if (!directory.empty()) {
    recorder_ = std::make_shared<RecordingNode>(directory, track_id_, track_->kind() == "video");
    nodes.push_back(recorder_);
    edges.push_back(video_input_ ? RtcPipeline::connect<RawVideoFrame>(subscribe_node_, recorder_)
                                 : RtcPipeline::connect<PcmFrame>(subscribe_node_, recorder_));
  }
  pipeline_->build(nodes, edges);
}
TrackSink::~TrackSink() { detach(); }
void TrackSink::attach() {
  if (attached_) return;
  if (!pipeline_->start()) throw std::runtime_error("media_graph: receive pipeline start failed");
  if (track_->kind() == "video") {
    static_cast<webrtc::VideoTrackInterface*>(track_.get())->AddOrUpdateSink(this, webrtc::VideoSinkWants{});
  }
  else static_cast<webrtc::AudioTrackInterface*>(track_.get())->AddSink(this);
  attached_ = true;
}
void TrackSink::detach() {
  if (!attached_) return;
  if (track_->kind() == "video") {
    static_cast<webrtc::VideoTrackInterface*>(track_.get())->RemoveSink(this);
  }
  else static_cast<webrtc::AudioTrackInterface*>(track_.get())->RemoveSink(this);
  pipeline_->stop();
  attached_ = false;
}
void TrackSink::OnFrame(const webrtc::VideoFrame& frame) {
  try {
    { std::lock_guard lock(mutex_); ++frames_; }
    video_input_->emit(livekit::sdk::detail::RawVideoFrame(frame));
  } catch (const std::exception& error) {
    ++subscribe_node_->errors; std::lock_guard lock(mutex_); error_ = error.what();
  }
}
void TrackSink::OnData(const void* data, int bits, int rate, size_t channels, size_t frames) {
  try {
    { std::lock_guard lock(mutex_); ++frames_; bytes_ += frames * channels * (bits / 8); }
    audio_input_->emit(livekit::sdk::detail::PcmFrame(data, bits, rate, channels, frames));
  } catch (const std::exception& error) {
    ++subscribe_node_->errors; std::lock_guard lock(mutex_); error_ = error.what();
  }
}
std::string TrackSink::error() const {
  { std::lock_guard lock(mutex_); if (!error_.empty()) return error_; }
  return recorder_ ? recorder_->error() : std::string{};
}
Json TrackSink::stats() const { std::lock_guard lock(mutex_); return {{"track", track_id_}, {"kind", track_->kind()}, {"frames", frames_}}; }
}
