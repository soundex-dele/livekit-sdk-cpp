#include "media_nodes/capture/audio_source.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>

namespace livekit::sdk {
using namespace std::chrono_literals;
struct PcmAudioSource::Impl {
  explicit Impl(PcmSourceOptions value) : options(value) {}
  const PcmSourceOptions options;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  AudioSourceStats stats;
  std::list<detail::PcmSourceBlock> queue;
  std::optional<std::chrono::steady_clock::time_point> written_end, next_capture;
  size_t samples() const { return options.sample_rate / 100; }
  size_t bytes() const { return samples() * options.channels * sizeof(int16_t); }
  void drop() {
    queue.pop_front(); stats.dropped_samples += samples();
    stats.queued_blocks = queue.size(); stats.queued_bytes -= bytes();
  }
  void clear() { while (!queue.empty()) drop(); written_end.reset(); next_capture.reset(); }
};
PcmAudioSource::PcmAudioSource(PcmSourceOptions options) {
  const std::vector<int> rates{8000, 16000, 24000, 32000, 44100, 48000, 96000};
  if (std::find(rates.begin(), rates.end(), options.sample_rate) == rates.end() ||
      (options.channels != 1 && options.channels != 2) ||
      (options.format != AudioSampleFormat::pcm16 && options.format != AudioSampleFormat::float32) ||
      options.max_queue_duration < 10ms || options.max_queue_duration > 5s || options.max_queue_duration.count() % 10 ||
      options.max_bytes < static_cast<size_t>(options.sample_rate / 100) * options.channels * 2 ||
      options.max_bytes > 64u * 1024 * 1024)
    throw std::invalid_argument("Invalid PCM source rate, channels, format or queue capacity");
  impl_ = std::make_unique<Impl>(options);
}
PcmAudioSource::~PcmAudioSource() = default;
PcmSourceOptions PcmAudioSource::options() const { return impl_->options; }
AudioWriteStatus PcmAudioSource::write(const PcmInputFrame& frame) {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return AudioWriteStatus::closed;
  const bool floating = s.options.format == AudioSampleFormat::float32;
  const auto count = floating ? frame.float_samples.size() : frame.samples.size();
  const auto block_samples = s.samples() * s.options.channels;
  if ((floating ? !frame.samples.empty() : !frame.float_samples.empty()) ||
      !count || count % block_samples || count / block_samples > 10)
    throw std::invalid_argument("PCM input must contain 10..100ms of complete interleaved samples in the source format");
  if (floating && !std::all_of(frame.float_samples.begin(), frame.float_samples.end(),
      [](float value) { return std::isfinite(value) && value >= -1 && value <= 1; }))
    throw std::invalid_argument("Float PCM samples must be finite and normalized to [-1,1]");
  const auto blocks = count / block_samples;
  const auto now = std::chrono::steady_clock::now();
  auto captured = frame.capture_time.value_or(s.written_end.value_or(s.next_capture.value_or(now)));
  if (!frame.capture_time && s.next_capture && captured < *s.next_capture) captured = *s.next_capture;
  if (frame.capture_time && (captured < now - 5s || captured > now + 100ms ||
      (s.written_end && captured < *s.written_end)))
    throw std::invalid_argument("PCM capture time is outside the live clock window or overlaps accepted samples");
  const auto max_blocks = static_cast<size_t>(s.options.max_queue_duration.count() / 10);
  if (!s.stats.accepting || blocks > max_blocks || s.queue.size() > max_blocks - blocks ||
      blocks > s.options.max_bytes / s.bytes() || s.stats.queued_bytes > s.options.max_bytes - blocks * s.bytes()) {
    ++s.stats.rejected_writes; return AudioWriteStatus::backpressure;
  }
  // Build new nodes before committing so allocation failure cannot
  // partially admit a multi-block write or advance its timeline.
  std::list<detail::PcmSourceBlock> pending;
  for (size_t i = 0; i < blocks; ++i) {
    detail::PcmSourceBlock block; block.captured = captured + 10ms * i; block.generation = s.stats.generation;
    block.samples.resize(block_samples);
    if (floating) for (size_t n = 0; n < block_samples; ++n)
      block.samples[n] = static_cast<int16_t>(std::clamp(std::lround(frame.float_samples[i * block_samples + n] * 32768.0f), -32768l, 32767l));
    else std::copy_n(frame.samples.data() + i * block_samples, block_samples, block.samples.data());
    pending.push_back(std::move(block));
  }
  s.queue.splice(s.queue.end(), pending); s.stats.queued_blocks = s.queue.size(); s.stats.queued_bytes += blocks * s.bytes();
  s.stats.accepted_samples += blocks * s.samples(); s.written_end = captured + 10ms * blocks;
  s.changed.notify_one(); return AudioWriteStatus::accepted;
}
AudioSourceStats PcmAudioSource::stats() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void PcmAudioSource::close() {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  s.stats.closed = true; s.stats.accepting = false; s.clear(); s.changed.notify_all();
}
namespace detail {
PcmSourceReader::PcmSourceReader(std::shared_ptr<PcmAudioSource> source) : source_(std::move(source)) {
  if (!source_) throw std::invalid_argument("PCM source is required");
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  if (s.stats.attached || s.stats.closed) throw std::invalid_argument("PCM source is already published or closed");
  s.clear(); s.stats.attached = true;
}
PcmSourceReader::~PcmSourceReader() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.stats.accepting = false; s.stats.attached = false; s.clear(); s.changed.notify_all();
}
void PcmSourceReader::start() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.clear(); ++s.stats.generation; s.stats.accepting = !s.stats.closed; s.changed.notify_all();
}
void PcmSourceReader::stop() {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  s.stats.accepting = false; s.clear(); s.changed.notify_all();
}
std::optional<PcmSourceBlock> PcmSourceReader::read(std::stop_token stop) {
  auto& s = *source_->impl_; std::unique_lock lock(s.mutex);
  s.changed.wait(lock, stop, [&] { return !s.stats.accepting || s.next_capture || !s.queue.empty(); });
  if (stop.stop_requested() || !s.stats.accepting) return {};
  if (!s.next_capture) s.next_capture = s.queue.front().captured;
  const auto captured = *s.next_capture;
  // Quantize capture-time jitter to the nearest 10ms slot. Late blocks never
  // pull the outgoing sample clock backwards or silently stretch the timeline.
  while (!s.queue.empty() && s.queue.front().captured < captured - 5ms) s.drop();
  PcmSourceBlock result;
  if (!s.queue.empty() && s.queue.front().captured < captured + 5ms) {
    result = std::move(s.queue.front()); s.queue.pop_front();
    s.stats.queued_blocks = s.queue.size(); s.stats.queued_bytes -= s.bytes(); s.stats.consumed_samples += s.samples();
  } else {
    result.samples.assign(s.samples() * s.options.channels, 0);
    result.silence = true; s.stats.silence_samples += s.samples();
  }
  result.captured = captured; result.generation = s.stats.generation;
  s.next_capture = captured + 10ms; return result;
}
void PcmSourceReader::skip(size_t blocks) {
  auto& s = *source_->impl_; std::lock_guard lock(s.mutex);
  if (!s.stats.accepting || !s.next_capture) return;
  s.next_capture = *s.next_capture + 10ms * blocks; s.stats.skipped_samples += blocks * s.samples();
  while (!s.queue.empty() && s.queue.front().captured < *s.next_capture - 5ms) s.drop();
}
}
}
