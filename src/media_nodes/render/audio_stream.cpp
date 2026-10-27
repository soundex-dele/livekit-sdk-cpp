#include "media_nodes/render/audio_stream.hpp"
#include "livekit/error.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

namespace livekit::sdk {
struct AudioFrameStream::Impl {
  explicit Impl(AudioStreamOptions value) : options(value) {}
  struct Track { uint64_t lease = 0, timeline = 0, sequence = 0, sample_index = 0; int rate = 0; std::size_t channels = 0; };
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  AudioStreamOptions options;
  AudioStreamStats stats;
  bool attached = false;
  uint64_t next_lease = 0, next_timeline = 0;
  std::map<std::string, Track> tracks;
  std::deque<std::shared_ptr<const AudioFrame>> frames;
  static std::size_t bytes(const AudioFrame& frame) { return frame.samples.size() * sizeof(int16_t) + frame.float_samples.size() * sizeof(float); }
  auto discard(std::deque<std::shared_ptr<const AudioFrame>>::iterator it) {
    stats.queued_bytes -= bytes(**it); ++stats.dropped; stats.dropped_samples += (*it)->samples_per_channel;
    auto next = frames.erase(it); stats.queued_frames = frames.size(); return next;
  }
  void discard_all() { while (!frames.empty()) discard(frames.begin()); }
};
AudioFrameStream::AudioFrameStream(AudioStreamOptions options) {
  if (!options.max_frames || options.max_frames > 1024 || !options.max_bytes || options.max_bytes > 64u * 1024 * 1024 ||
      (options.format != AudioSampleFormat::pcm16 && options.format != AudioSampleFormat::float32))
    throw std::invalid_argument("Audio stream requires 1..1024 frames, 1 byte..64 MiB, and PCM16/float32 output");
  impl_ = std::make_unique<Impl>(options);
}
AudioFrameStream::~AudioFrameStream() = default;
AudioReadResult AudioFrameStream::read(std::chrono::milliseconds timeout, std::stop_token stop) { return read_for({}, timeout, stop); }
AudioReadResult AudioFrameStream::read_for(const std::string& id, std::chrono::milliseconds timeout, std::stop_token stop) {
  if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24))
    throw std::invalid_argument("Audio read timeout must be between zero and 24 hours");
  auto& s = *impl_; std::unique_lock lock(s.mutex);
  auto find = [&] { return std::find_if(s.frames.begin(), s.frames.end(), [&](const auto& frame) { return id.empty() || frame->track_id == id; }); };
  s.changed.wait_for(lock, stop, timeout, [&] { return s.stats.closed || find() != s.frames.end(); });
  if (stop.stop_requested()) return {AudioReadStatus::cancelled, {}};
  if (s.stats.closed) return {AudioReadStatus::closed, {}};
  auto it = find(); if (it == s.frames.end()) return {AudioReadStatus::timeout, {}};
  auto frame = std::move(*it); s.frames.erase(it);
  s.stats.queued_bytes -= Impl::bytes(*frame); s.stats.queued_frames = s.frames.size();
  return {AudioReadStatus::frame, std::move(frame)};
}
AudioStreamStats AudioFrameStream::stats() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void AudioFrameStream::close() {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  s.stats.closed = true; s.discard_all(); s.tracks.clear(); s.stats.tracks = 0; s.changed.notify_all();
}
namespace detail {
AudioFrameOutput::AudioFrameOutput(std::shared_ptr<AudioFrameStream> stream) : stream_(std::move(stream)) {
  if (!stream_) throw std::invalid_argument("Audio output requires a stream");
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.attached || s.stats.closed) throw std::invalid_argument("Audio stream is already attached or closed");
  s.attached = true;
}
AudioFrameOutput::~AudioFrameOutput() { close(); }
uint64_t AudioFrameOutput::add(const std::string& id) {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return 0;
  if (id.empty() || s.tracks.contains(id)) throw std::invalid_argument("Duplicate or empty audio track ID");
  if (s.tracks.size() >= 1024) throw SDKError(ErrorDomain::media, ErrorCode::resource_exhausted, "Too many audio output tracks");
  const auto lease = ++s.next_lease;
  s.tracks.emplace(id, AudioFrameStream::Impl::Track{lease, ++s.next_timeline}); s.stats.tracks = s.tracks.size();
  return lease;
}
void AudioFrameOutput::remove(const std::string& id, uint64_t lease) {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  const auto found = s.tracks.find(id);
  if (found == s.tracks.end() || found->second.lease != lease) return;
  for (auto it = s.frames.begin(); it != s.frames.end();) {
    if ((*it)->track_id == id) it = s.discard(it); else ++it;
  }
  s.tracks.erase(found); s.stats.tracks = s.tracks.size();
}
void AudioFrameOutput::submit(const std::string& id, uint64_t lease, const void* data, int bits,
    int rate, std::size_t channels, std::size_t count, std::optional<int64_t> capture) {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  const auto found = s.tracks.find(id);
  if (s.stats.closed || found == s.tracks.end() || !lease || found->second.lease != lease) return;
  if (!data || bits != 16 || rate < 8000 || rate > 192000 || !channels || channels > 8 || !count || count > static_cast<std::size_t>(rate / 10))
    throw SDKError(ErrorDomain::media, ErrorCode::unsupported, "Invalid native PCM block");
  auto& track = found->second;
  const bool format_changed = track.rate != rate || track.channels != channels;
  if (format_changed) {
    // Old-rate samples cannot share the new sample-index time base.
    for (auto it = s.frames.begin(); it != s.frames.end();) {
      if ((*it)->track_id == id) it = s.discard(it); else ++it;
    }
    track.timeline = ++s.next_timeline; track.rate = rate; track.channels = channels; track.sequence = track.sample_index = 0;
  }
  const auto sequence = track.sequence++, index = track.sample_index; track.sample_index += count; ++s.stats.received;
  const auto bytes = channels * count * (s.options.format == AudioSampleFormat::pcm16 ? sizeof(int16_t) : sizeof(float));
  if (bytes > s.options.max_bytes) { ++s.stats.dropped; s.stats.dropped_samples += count; return; }
  while (!s.frames.empty() && (s.frames.size() >= s.options.max_frames || s.stats.queued_bytes > s.options.max_bytes - bytes)) s.discard(s.frames.begin());
  auto frame = std::make_shared<AudioFrame>();
  frame->track_id = id; frame->format = s.options.format; frame->sample_rate = rate; frame->channels = channels; frame->samples_per_channel = count;
  frame->stream_generation = s.stats.generation; frame->track_generation = track.timeline; frame->sequence = sequence;
  frame->sample_index = index; frame->format_changed = format_changed; frame->capture_timestamp_ms = capture;
  if (frame->format == AudioSampleFormat::pcm16) {
    frame->samples.resize(count * channels); std::memcpy(frame->samples.data(), data, bytes);
  } else {
    frame->float_samples.resize(count * channels);
    // memcpy also supports native buffers without int16_t alignment.
    const auto input = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < count * channels; ++i) { int16_t sample; std::memcpy(&sample, input + i * 2, 2); frame->float_samples[i] = sample / 32768.0f; }
  }
  s.frames.push_back(std::move(frame)); s.stats.queued_bytes += bytes; s.stats.queued_frames = s.frames.size();
  s.changed.notify_all(); // different readers may wait for different tracks
}
void AudioFrameOutput::advance() {
  auto& s = *stream_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return;
  s.discard_all(); ++s.stats.generation;
  for (auto& [id, track] : s.tracks) { track.rate = 0; track.channels = 0; track.sequence = track.sample_index = 0; }
}
void AudioFrameOutput::close() { stream_->close(); }
}
}
