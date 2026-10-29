#include "livekit/rtc/audio_nodes.hpp"
#include "backends/webrtc/pcm_resampler.hpp"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/environment/environment_factory.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <thread>
#include <variant>

namespace livekit::sdk::rtc::nodes {
struct AudioCodecNode::Impl {
  AudioCodecNode& owner;
  const TrackDescriptor descriptor;
  const bool encode;
  const AudioCodecOptions options;
  struct Timing { Epoch epoch; uint64_t timestamp, generation; SteadyClock::time_point captured; };
  struct Job { std::variant<AudioFrameEvent, EncodedFrame> frame; Timing timing; size_t bytes; };
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  std::deque<Job> jobs;
  AudioCodecSnapshot stats;
  std::jthread worker;
  bool active = false, closed = false;
  Epoch epoch = 0;
  uint64_t generation = 0;
  std::optional<uint64_t> last_timestamp;
  SenderFeedback feedback;
  SteadyClock::time_point input_boundary{};

  Impl(AudioCodecNode& owner, TrackDescriptor descriptor, bool encode, AudioCodecOptions options)
      : owner(owner), descriptor(std::move(descriptor)), encode(encode), options(options) {
    const auto& d = this->descriptor;
    if (d.kind != MediaKind::audio || d.format.codec != Codec::opus || d.format.clock_rate != 48000 ||
        (d.format.channels != 1 && d.format.channels != 2) || d.encodings.size() != 1 ||
        !options.max_frames || options.max_frames > 500 || !options.max_bytes || options.max_bytes > 4 * 1024 * 1024 ||
        options.max_age.count() <= 0 || options.max_age > std::chrono::seconds(5) ||
        (options.packet_duration_ms != 10 && options.packet_duration_ms != 20 && options.packet_duration_ms != 40 && options.packet_duration_ms != 60) ||
        options.bitrate_bps < 6000 || options.bitrate_bps > 510000)
      throw std::invalid_argument("Invalid audio codec node configuration");
  }
  void discard_locked() {
    stats.dropped += jobs.size(); jobs.clear(); stats.queued_frames = stats.queued_bytes = 0; ++generation;
  }
  void fail(std::string message) {
    std::lock_guard lock(mutex); stats.error = std::move(message); active = false; discard_locked(); changed.notify_all();
  }
  void update(const SenderFeedback& value) {
    std::lock_guard lock(mutex);
    if (value.epoch < epoch || (value.epoch == feedback.epoch && value.revision <= feedback.revision)) return;
    const auto previous_generation = generation;
    if (value.epoch != epoch) { discard_locked(); epoch = value.epoch; last_timestamp.reset(); }
    if (value.media_generation != feedback.media_generation) discard_locked();
    const auto next = value.encodings.find(descriptor.encodings.front().id);
    const auto previous = feedback.encodings.find(descriptor.encodings.front().id);
    if (next != value.encodings.end() && (previous == feedback.encodings.end() || previous->second.active != next->second.active))
      discard_locked();
    if (generation != previous_generation) input_boundary = SteadyClock::now();
    feedback = value;
  }
  void enqueue(Job job) {
    std::lock_guard lock(mutex);
    if (!active || closed) return;
    if (encode) {
      const auto current = feedback.encodings.find(descriptor.encodings.front().id);
      if (current != feedback.encodings.end() && !current->second.active) { ++stats.dropped; return; }
    }
    if (!job.timing.epoch || job.timing.epoch < epoch || (encode && job.timing.epoch != epoch) ||
        job.timing.timestamp > std::numeric_limits<uint64_t>::max() - 5760) { ++stats.dropped; return; }
    if (job.timing.epoch != epoch) { discard_locked(); epoch = job.timing.epoch; last_timestamp.reset(); }
    const auto now = SteadyClock::now();
    if ((encode && job.timing.captured < input_boundary) || job.timing.captured > now + std::chrono::seconds(1) || now - job.timing.captured > options.max_age ||
        (last_timestamp && (job.timing.timestamp <= *last_timestamp || (encode && job.timing.timestamp - *last_timestamp < 480)))) {
      ++stats.dropped; return;
    }
    if (job.bytes > options.max_bytes) { ++stats.dropped; return; }
    if (jobs.size() >= options.max_frames || stats.queued_bytes > options.max_bytes - job.bytes) discard_locked();
    job.timing.generation = generation; last_timestamp = job.timing.timestamp;
    stats.queued_bytes += job.bytes; jobs.push_back(std::move(job)); stats.queued_frames = jobs.size();
    ++stats.admitted; changed.notify_all();
  }
  void raw(const AudioFrameEvent& value) {
    if (!value.pcm) return;
    const auto& pcm = *value.pcm;
    const auto count = size_t(pcm.sample_rate / 100) * pcm.channels;
    bool valid = pcm.sample_rate >= 8000 && pcm.sample_rate <= 96000 && pcm.sample_rate % 100 == 0 &&
        pcm.channels == descriptor.format.channels;
    if (pcm.format == PcmFormat::s16) valid = valid && pcm.samples.size() == count && pcm.float_samples.empty();
    else if (pcm.format == PcmFormat::f32) valid = valid && pcm.float_samples.size() == count && pcm.samples.empty() &&
        std::all_of(pcm.float_samples.begin(), pcm.float_samples.end(), [](float sample) { return std::isfinite(sample); });
    else valid = false;
    if (!valid) { std::lock_guard lock(mutex); ++stats.dropped; return; }
    enqueue({value, {value.epoch,value.timestamp,0,value.captured}, count * (pcm.format == PcmFormat::s16 ? 2 : 4)});
  }
  void encoded(const EncodedFrame& value) {
    if (value.buffer.size()) enqueue({value, {value.epoch,value.timestamp,0,SteadyClock::now()}, value.buffer.size()});
  }
  bool output_allowed(const Timing& timing) {
    std::lock_guard lock(mutex);
    if (!active || timing.epoch != epoch || timing.generation != generation ||
        SteadyClock::now() - timing.captured > options.max_age) { ++stats.dropped; return false; }
    ++stats.completed; return true;
  }
  void discontinuity() { std::lock_guard lock(mutex); ++stats.discontinuities; }
  void invalid_packet() { std::lock_guard lock(mutex); ++stats.invalid_packets; ++stats.dropped; }

  void run(std::stop_token stop, std::promise<void> started) {
    bool ready = false;
    try {
      // Keep the environment and factories alive until codec destruction.
      const auto env = webrtc::CreateEnvironment();
      auto encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
      auto decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
      auto parameters = descriptor.format.fmtp;
      parameters["stereo"] = descriptor.format.channels == 2 ? "1" : "0";
      parameters["ptime"] = std::to_string(options.packet_duration_ms);
      parameters["maxaveragebitrate"] = std::to_string(options.bitrate_bps);
      const webrtc::SdpAudioFormat format("opus",48000,2,parameters);
      std::unique_ptr<webrtc::AudioEncoder> encoder;
      std::unique_ptr<webrtc::AudioDecoder> decoder;
      if (encode) {
        webrtc::AudioEncoderFactory::Options codec_options; codec_options.payload_type = 111;
        encoder = encoder_factory->Create(env,format,codec_options);
        if (!encoder || encoder->SampleRateHz() != 48000 || encoder->NumChannels() != descriptor.format.channels)
          throw std::runtime_error("Opus encoder configuration failed");
      } else {
        decoder = decoder_factory->Create(env,format,{});
        if (!decoder || decoder->SampleRateHz() != 48000 || decoder->Channels() != descriptor.format.channels)
          throw std::runtime_error("Opus decoder configuration failed");
      }
      started.set_value(); ready = true;
      Epoch codec_epoch = 0;
      uint64_t codec_generation = 0;
      uint32_t input_rate = 0;
      std::optional<uint64_t> expected;
      std::optional<Timing> packet_start;
      size_t packet_chunks = 0, packet_length = 0;
      std::unique_ptr<livekit::sdk::detail::PcmResampler> resampler;
      while (!stop.stop_requested()) {
        std::optional<Job> job;
        uint32_t bitrate = options.bitrate_bps;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock,stop,[&] { return !jobs.empty() || !active; });
          if (stop.stop_requested() || !active) break;
          job = std::move(jobs.front()); jobs.pop_front(); stats.queued_frames = jobs.size(); stats.queued_bytes -= job->bytes;
          if (job->timing.epoch != epoch || job->timing.generation != generation || SteadyClock::now() - job->timing.captured > options.max_age) {
            ++stats.dropped; expected.reset(); packet_start.reset(); codec_epoch = 0; continue;
          }
          if (encode) {
            auto it = feedback.encodings.find(descriptor.encodings.front().id);
            if (it != feedback.encodings.end()) {
              if (!it->second.active) { ++stats.dropped; codec_epoch = 0; continue; }
              if (it->second.target_bitrate_bps) bitrate = std::clamp(it->second.target_bitrate_bps,6000u,options.bitrate_bps);
            }
          }
        }
        const auto timing = job->timing;
        const bool new_generation = codec_epoch != timing.epoch || codec_generation != timing.generation;
        if (!new_generation && expected && timing.timestamp < *expected) { invalid_packet(); continue; }
        bool boundary = new_generation || !expected || timing.timestamp != *expected;
        if (encode) {
          const auto& input = std::get<AudioFrameEvent>(job->frame);
          const auto& pcm = *input.pcm;
          boundary = boundary || input.discontinuity || input_rate != pcm.sample_rate;
          if (boundary) {
            encoder->Reset(); packet_start.reset(); packet_chunks = 0;
            resampler = std::make_unique<livekit::sdk::detail::PcmResampler>(pcm.sample_rate,pcm.channels);
            input_rate = pcm.sample_rate; discontinuity();
          }
          encoder->SetTargetBitrate(int(bitrate));
          std::vector<int16_t> converted;
          std::span<const int16_t> input_samples = pcm.samples;
          if (pcm.format == PcmFormat::f32) {
            converted.reserve(pcm.float_samples.size());
            for (const auto sample : pcm.float_samples) converted.push_back(int16_t(std::clamp(std::lround(std::clamp(sample,-1.f,1.f)*32768.f),-32768l,32767l)));
            input_samples = converted;
          }
          const auto normalized = resampler->process(input_samples);
          if (!packet_start) {
            packet_start = timing; packet_chunks = 0; packet_length = encoder->Num10MsFramesInNextPacket();
            if (!packet_length || packet_length > 12) throw std::runtime_error("Invalid Opus packet duration");
          }
          webrtc::Buffer bytes;
          const auto result = encoder->Encode(uint32_t(timing.timestamp),{normalized.data(),normalized.size()},&bytes);
          ++packet_chunks; expected = timing.timestamp + 480;
          if (result.encoded_bytes) {
            if (result.encoded_timestamp != uint32_t(packet_start->timestamp) || bytes.size() != result.encoded_bytes)
              throw std::runtime_error("Opus encoder lost packet timing");
            if (bytes.size() <= options.max_bytes && output_allowed(*packet_start)) {
              EncodedFrame frame; frame.buffer = EncodedBuffer(std::vector<uint8_t>(bytes.data(),bytes.data()+bytes.size()));
              frame.epoch = packet_start->epoch; frame.timestamp = packet_start->timestamp; frame.captured = packet_start->captured;
              frame.duration = std::chrono::milliseconds(packet_chunks*10); frame.encoding_id = descriptor.encodings.front().id;
              owner.publish(EncodedFrameEvent(std::move(frame)));
            }
            packet_start.reset();
          } else if (packet_chunks >= packet_length) packet_start.reset(); // DTX can intentionally suppress a packet.
        } else {
          if (boundary) { decoder->Reset(); discontinuity(); }
          const auto& frame = std::get<EncodedFrame>(job->frame);
          const auto bytes = frame.buffer.bytes();
          const int samples = decoder->PacketDuration(bytes.data(),bytes.size());
          if (samples <= 0 || samples > 5760 || size_t(samples)*descriptor.format.channels*2 > options.max_bytes) {
            invalid_packet(); expected.reset(); codec_epoch = 0; continue;
          }
          auto pcm = std::make_shared<PcmAudioFrame>(); pcm->channels = descriptor.format.channels;
          pcm->samples.resize(size_t(samples)*pcm->channels);
          webrtc::AudioDecoder::SpeechType speech;
          const int decoded = decoder->Decode(bytes.data(),bytes.size(),48000,pcm->samples.size()*2,pcm->samples.data(),&speech);
          if (decoded <= 0 || size_t(decoded) != pcm->samples.size()) {
            invalid_packet(); expected.reset(); codec_epoch = 0; continue;
          }
          expected = timing.timestamp + samples;
          if (output_allowed(timing)) {
            AudioFrameEvent output; output.pcm = std::move(pcm); output.timestamp = timing.timestamp;
            output.epoch = timing.epoch; output.captured = timing.captured; output.discontinuity = boundary;
            owner.publish(output);
          }
        }
        codec_epoch = timing.epoch; codec_generation = timing.generation;
      }
    } catch (const std::exception& error) {
      if (!ready) started.set_exception(std::current_exception()); fail(error.what());
    } catch (...) {
      if (!ready) started.set_exception(std::current_exception()); fail("Unknown audio codec error");
    }
  }
};
AudioCodecNode::AudioCodecNode(std::string name, TrackDescriptor descriptor, bool encode, AudioCodecOptions options)
    : GraphNode(name,nullptr), impl_(std::make_unique<Impl>(*this,std::move(descriptor),encode,options)) {}
AudioCodecNode::~AudioCodecNode() { quiesce(); release(); }
bool AudioCodecNode::doInit() {
  if (impl_->encode) {
    subscribe<AudioFrameEvent>([this](const auto& value) { impl_->raw(value); });
    subscribe<SenderFeedbackEvent>([this](const auto& value) { impl_->update(value.feedback); });
  } else subscribe<EncodedFrameEvent>([this](const auto& value) { impl_->encoded(value.frame); });
  return true;
}
bool AudioCodecNode::doStart() {
  { std::lock_guard lock(impl_->mutex); if (impl_->closed) return false; impl_->active = true; }
  std::promise<void> started; auto result = started.get_future();
  impl_->worker = std::jthread([this,started=std::move(started)](std::stop_token stop) mutable { impl_->run(stop,std::move(started)); });
  try { result.get(); return true; } catch (...) { quiesce(); throw; }
}
void AudioCodecNode::quiesce() {
  { std::lock_guard lock(impl_->mutex); impl_->active = false; impl_->closed = true; impl_->discard_locked(); }
  impl_->worker.request_stop(); impl_->changed.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
}
void AudioCodecNode::doStop() { quiesce(); }
AudioCodecSnapshot AudioCodecNode::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void AudioCodecNode::throw_if_failed() const { const auto status = snapshot(); if (!status.error.empty()) throw std::runtime_error(status.error); }
} // namespace livekit::sdk::rtc::nodes
