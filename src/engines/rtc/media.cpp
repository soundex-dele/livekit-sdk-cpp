#include "engines/rtc/media.hpp"
#include "media_nodes/render/video_playout.hpp"
#include "media_nodes/render/video_frames.hpp"
#include "api/make_ref_counted.h"
#include <algorithm>
namespace lk {
void AudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); sinks_.push_back(sink); }
void AudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) { std::lock_guard lock(mutex_); std::erase(sinks_, sink); }
void AudioSource::push(const livekit::sdk::detail::PcmFrame& frame) {
  std::lock_guard lock(mutex_);
  for (auto* sink : sinks_) sink->OnData(frame.data, frame.bits, frame.rate, frame.channels, frame.frames, frame.capture_timestamp_ms);
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
                     VideoPlayout* playout, VideoFrames* output_frames, std::string id, bool backup)
    : capture_(std::make_shared<CaptureNode>(std::move(value))), spec(capture_->spec), frames(capture_->frames) {
  using namespace livekit::sdk::detail;
  if (!graph) graph = std::make_shared<RtcGraph>();
  pipeline_ = std::make_unique<RtcPipeline>(graph);
  std::vector<std::shared_ptr<MediaNode>> nodes{capture_};
  std::vector<RtcPipeline::Connection> edges;
  if (spec.video) {
    video = webrtc::make_ref_counted<VideoSource>(spec.capture == livekit::sdk::CaptureKind::screen);
    auto output = std::make_shared<MediaEventNode<EncodedVideoFrame>>("RtcVideoTrackSinkNode", NodeExecution::sink,
        [source = video](EncodedVideoFrame& event) { source->push(event.frame); });
    if (spec.encoded_source) {
      edges.push_back(RtcPipeline::connect<EncodedVideoFrame>(capture_, output));
    } else {
      auto encoder = create_video_encoder_node();
      nodes.push_back(encoder);
      edges.push_back(RtcPipeline::connect<RawVideoFrame>(capture_, encoder));
      edges.push_back(RtcPipeline::connect<EncodedVideoFrame>(encoder, output));
    }
    nodes.push_back(output);
    if(backup){
      if(spec.encoded_source)throw std::invalid_argument("Backup codec requires raw video input");
      backup_video=webrtc::make_ref_counted<VideoSource>(spec.capture==livekit::sdk::CaptureKind::screen);
      auto encoder = create_video_encoder_node();
      auto sink = std::make_shared<MediaEventNode<EncodedVideoFrame>>("RtcVideoTrackSinkNode", NodeExecution::sink,
          [source = backup_video](EncodedVideoFrame& event) { source->push(event.frame); });
      nodes.push_back(encoder); nodes.push_back(sink);
      edges.push_back(RtcPipeline::connect<RawVideoFrame>(capture_, encoder));
      edges.push_back(RtcPipeline::connect<EncodedVideoFrame>(encoder, sink));
    }
    if (spec.local_preview) {
      auto preview = std::make_shared<VideoRenderNode>("local-" + id, playout, output_frames);
      nodes.push_back(preview); edges.push_back(RtcPipeline::connect<RawVideoFrame>(capture_, preview));
    }
  } else {
    audio = webrtc::make_ref_counted<AudioSource>();
    auto output = std::make_shared<MediaEventNode<PcmFrame>>("AudioTrackInputNode", NodeExecution::sink,
        [source = audio](PcmFrame& event) { source->push(event); });
    nodes.push_back(output); edges.push_back(RtcPipeline::connect<PcmFrame>(capture_, output));
  }
  for (const auto& node : nodes) node->set_track_id(id);
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
TrackSink::TrackSink(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::filesystem::path& directory, VideoPlayout* playout, VideoFrames* frames, std::string track_id, std::shared_ptr<livekit::sdk::detail::RtcGraph> graph, std::vector<uint32_t> ssrcs, std::shared_ptr<livekit::sdk::detail::AudioFrameOutput> audio_output)
    : track_(std::move(track)), track_id_(track_id.empty() ? track_->id() : std::move(track_id)), playout_(playout), frame_output_(frames), audio_output_(std::move(audio_output)) {
  if (!graph) graph = std::make_shared<livekit::sdk::detail::RtcGraph>();
  using namespace livekit::sdk::detail;
  registry_ = graph;
  ssrcs_ = std::move(ssrcs);
  if (track_->kind() == "video") {
    receive_video_ = std::make_shared<VideoReceiveGraph>(graph, track_id_, directory, playout, frames);
    return;
  }
  pipeline_ = std::make_unique<RtcPipeline>(graph);
  audio_input_ = std::make_shared<MediaEventNode<PcmFrame>>("RtcSubscribeNode", NodeExecution::source);
  subscribe_node_ = audio_input_;
  std::vector<std::shared_ptr<MediaNode>> nodes{subscribe_node_};
  std::vector<RtcPipeline::Connection> edges;
  if (audio_output_) {
    auto output = std::make_shared<MediaEventNode<PcmFrame>>("AudioFrameOutputNode", NodeExecution::sink,
        [this](PcmFrame& frame) { audio_output_->submit(track_id_, audio_lease_, frame.data, frame.bits,
            frame.rate, frame.channels, frame.frames, frame.capture_timestamp_ms); });
    nodes.push_back(output); edges.push_back(RtcPipeline::connect<PcmFrame>(subscribe_node_, output));
  }
  if (!directory.empty()) {
    recorder_ = std::make_shared<RecordingNode>(directory, track_id_, false);
    nodes.push_back(recorder_);
    edges.push_back(RtcPipeline::connect<PcmFrame>(subscribe_node_, recorder_));
  }
  pipeline_->build(nodes, edges);
}
TrackSink::~TrackSink() { detach(); if(registry_)registry_->unbind_sync(track_id_); }
void TrackSink::attach() {
  if (attached_) return;
  if (receive_video_) {
    if (!receive_video_->start()) throw std::runtime_error("media_graph: receive video graph start failed");
    try { registry_->bind_video(ssrcs_, receive_video_); }
    catch (...) { receive_video_->stop(); throw; }
    attached_ = true;
    return;
  }
  if (!pipeline_->start()) throw std::runtime_error("media_graph: receive pipeline start failed");
  try {
    if (audio_output_) audio_lease_ = audio_output_->add(track_id_);
    static_cast<webrtc::AudioTrackInterface*>(track_.get())->AddSink(this);
  } catch (...) {
    if (audio_output_) audio_output_->remove(track_id_, audio_lease_);
    pipeline_->stop(); throw;
  }
  attached_ = true;
}
void TrackSink::detach() {
  if (!attached_) return;
  if (receive_video_) {
    registry_->unbind_video(receive_video_.get());
    receive_video_->stop();
    attached_ = false;
    return;
  }
  static_cast<webrtc::AudioTrackInterface*>(track_.get())->RemoveSink(this);
  if (audio_output_) audio_output_->remove(track_id_, audio_lease_);
  pipeline_->stop();
  attached_ = false;
}
void TrackSink::OnData(const void* data, int bits, int rate, size_t channels, size_t frames) {
  OnData(data, bits, rate, channels, frames, {});
}
void TrackSink::OnData(const void* data, int bits, int rate, size_t channels, size_t frames, std::optional<int64_t> capture_timestamp_ms) {
  try {
    { std::lock_guard lock(mutex_); ++frames_; bytes_ += frames * channels * (bits / 8); }
    audio_input_->emit(livekit::sdk::detail::PcmFrame(data, bits, rate, channels, frames, capture_timestamp_ms));
  } catch (const std::exception& error) {
    ++subscribe_node_->errors; std::lock_guard lock(mutex_); error_ = error.what();
  }
}
std::string TrackSink::error() const {
  if (receive_video_) return receive_video_->error();
  { std::lock_guard lock(mutex_); if (!error_.empty()) return error_; }
  return recorder_ ? recorder_->error() : std::string{};
}
Json TrackSink::stats() const {std::lock_guard lock(mutex_);Json result{{"track",track_id_},{"ssrcs",ssrcs_},{"kind",track_->kind()},{"frames",receive_video_?receive_video_->frames():frames_},{"source","GraphEngine"}};
  if(receive_video_){auto nodes=receive_video_->metrics();result["decoded"]=nodes[0].outputs;result["codec"]=nodes[0].format;result["decodeErrors"]=nodes[0].errors;result["presented"]=nodes[1].outputs;result["presentationDropped"]=nodes[1].dropped;result["renderSubmitted"]=nodes[2].outputs;}return result; }
}
