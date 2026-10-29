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
  std::vector<int16_t> remainder;
  std::chrono::steady_clock::time_point remainder_time{};
  std::optional<std::chrono::steady_clock::time_point> previous_input_time;
  size_t previous_input_frames=0;double ratio=1,fraction=0;
  std::optional<std::chrono::steady_clock::time_point> written_end, next_capture;
  size_t samples() const { return options.sample_rate / 100; }
  size_t bytes() const { return samples() * options.channels * sizeof(int16_t); }
  void drop() {
    queue.pop_front(); stats.dropped_samples += samples();
    stats.queued_blocks = queue.size(); stats.queued_bytes -= bytes();
  }
  void clear() { stats.dropped_samples+=remainder.size()/options.channels;stats.queued_bytes-=remainder.size()*2;remainder.clear();while (!queue.empty()) drop(); written_end.reset(); next_capture.reset();previous_input_time.reset();previous_input_frames=0;ratio=1;fraction=0; }
};
PcmAudioSource::PcmAudioSource(PcmSourceOptions options) {
  const std::vector<int> rates{8000, 16000, 24000, 32000, 44100, 48000, 96000};
  if (std::find(rates.begin(), rates.end(), options.sample_rate) == rates.end() ||
      (options.channels != 1 && options.channels != 2) ||
      (options.format != AudioSampleFormat::pcm16 && options.format != AudioSampleFormat::float32) ||
      options.max_queue_duration < 10ms || options.max_queue_duration > 5s || options.max_queue_duration.count() % 10 ||
      options.max_bytes < static_cast<size_t>(options.sample_rate / 100) * options.channels * 2 ||
      options.max_bytes > 64u * 1024 * 1024 || options.max_drift_ppm<0 || options.max_drift_ppm>5000)
    throw std::invalid_argument("Invalid PCM source rate, channels, format or queue capacity");
  impl_ = std::make_unique<Impl>(options);
}
PcmAudioSource::~PcmAudioSource() = default;
PcmSourceOptions PcmAudioSource::options() const { return impl_->options; }
AudioWriteStatus PcmAudioSource::write(const PcmInputFrame& frame) {
  return write(frame,{});
}
AudioWriteStatus PcmAudioSource::write(const PcmInputFrame& frame,std::optional<uint64_t> expected_generation) {
  auto& s = *impl_; std::lock_guard lock(s.mutex);
  if (s.stats.closed) return AudioWriteStatus::closed;
  if(expected_generation && *expected_generation!=s.stats.generation) { ++s.stats.rejected_writes; return AudioWriteStatus::backpressure; }
  const bool floating = s.options.format == AudioSampleFormat::float32;
  const auto input_count = floating ? frame.float_samples.size() : frame.samples.size();
  auto count=input_count;
  const auto block_samples = s.samples() * s.options.channels;
  if ((floating ? !frame.samples.empty() : !frame.float_samples.empty()) || !count || count%s.options.channels || count>block_samples*10)
    throw std::invalid_argument("PCM input requires complete interleaved samples, at most 100ms");
  if(floating&&!std::all_of(frame.float_samples.begin(),frame.float_samples.end(),[](float v){return std::isfinite(v)&&v>=-1&&v<=1;}))throw std::invalid_argument("Invalid float PCM sample");
  const auto now=std::chrono::steady_clock::now();
  auto captured=frame.capture_time.value_or(s.written_end.value_or(s.next_capture.value_or(now)));
  if(s.remainder.empty()&&!frame.capture_time&&s.next_capture&&captured<*s.next_capture)captured=*s.next_capture;
  if(frame.capture_time&&(captured<now-5s||captured>now+100ms||(!s.options.drift_compensation&&s.written_end&&captured+1us<*s.written_end)))throw std::invalid_argument("Invalid PCM capture time");
  std::vector<int16_t> samples;samples.reserve(input_count);
  if(floating)for(float value:frame.float_samples)samples.push_back(static_cast<int16_t>(std::clamp(std::lround(value*32768.0f),-32768l,32767l)));
  else samples=frame.samples;
  double ratio=s.ratio,fraction=s.fraction;
  if(s.options.drift_compensation){
    if(frame.capture_time&&s.previous_input_time&&s.previous_input_frames){
      const double elapsed=std::chrono::duration<double>(*frame.capture_time-*s.previous_input_time).count();
      if(elapsed<=0)throw std::invalid_argument("Non-monotonic PCM capture clock");
      const double observed=elapsed*s.options.sample_rate/s.previous_input_frames;
      if(observed>.95&&observed<1.05){const double bound=s.options.max_drift_ppm/1000000.0;ratio=.95*ratio+.05*std::clamp(observed,1-bound,1+bound);}
    }
    const auto frames=input_count/s.options.channels;const double target=frames*ratio+fraction;
    const size_t output_frames=size_t(target);fraction=target-output_frames;
    std::vector<int16_t> adjusted(output_frames*s.options.channels);
    for(size_t i=0;i<output_frames;++i){const double at=output_frames>1?double(i)*(frames-1)/(output_frames-1):0;const auto first=size_t(at),next=std::min(first+1,frames-1);const double blend=at-first;
      for(size_t c=0;c<s.options.channels;++c)adjusted[i*s.options.channels+c]=int16_t(std::lround(samples[first*s.options.channels+c]*(1-blend)+samples[next*s.options.channels+c]*blend));}
    samples=std::move(adjusted);count=samples.size();if(s.written_end)captured=*s.written_end;
  }
  if(!s.remainder.empty()&&s.written_end&&std::chrono::abs(captured-*s.written_end)>1ms)throw std::invalid_argument("Discontinuous PCM input with partial block");
  const size_t max_samples=size_t(s.options.sample_rate)*s.options.channels*s.options.max_queue_duration.count()/1000;
  if(!s.stats.accepting||count>max_samples||s.stats.queued_bytes/2>max_samples-count||count>s.options.max_bytes/2||s.stats.queued_bytes>s.options.max_bytes-count*2){++s.stats.rejected_writes;return AudioWriteStatus::backpressure;}
  auto combined=s.remainder;combined.reserve(combined.size()+count);
  combined.insert(combined.end(),samples.begin(),samples.end());
  const auto begin=s.remainder.empty()?captured:s.remainder_time;
  const auto blocks=combined.size()/block_samples;
  std::list<detail::PcmSourceBlock> pending;
  for(size_t i=0;i<blocks;++i){detail::PcmSourceBlock b;b.captured=begin+10ms*i;b.generation=s.stats.generation;b.samples.assign(combined.begin()+i*block_samples,combined.begin()+(i+1)*block_samples);pending.push_back(std::move(b));}
  std::vector<int16_t> tail(combined.begin()+blocks*block_samples,combined.end());
  s.queue.splice(s.queue.end(),pending);s.remainder=std::move(tail);s.remainder_time=begin+10ms*blocks;
  s.stats.queued_blocks=s.queue.size();s.stats.queued_bytes+=count*2;s.stats.accepted_samples+=input_count/s.options.channels;
  s.ratio=ratio;s.fraction=fraction;s.previous_input_time=frame.capture_time;s.previous_input_frames=input_count/s.options.channels;
  s.stats.estimated_drift_ppm=(ratio-1)*1000000;s.stats.corrected_samples+=(int64_t(count)-int64_t(input_count))/int64_t(s.options.channels);
  s.written_end=begin+std::chrono::nanoseconds((int64_t(combined.size()/s.options.channels)*1000000000)/s.options.sample_rate);
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
