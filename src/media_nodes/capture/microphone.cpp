#include "media_nodes/capture/microphone_driver.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "backends/webrtc/pcm_resampler.hpp"
#include <mutex>

namespace livekit::sdk {
using namespace std::chrono_literals;

struct MicrophoneSession::Impl {
  const MicrophoneOptions options;
  std::shared_ptr<PcmAudioSource> source;
  std::unique_ptr<detail::MicrophoneDriver> driver;
  std::mutex control;
  mutable std::mutex mutex;
  MicrophoneSnapshot stats;
  std::function<void(std::exception_ptr)> failure_handler;
  std::unique_ptr<detail::PcmResampler> resampler;
  int rate = 0;
  uint64_t generation = 0;
  std::optional<std::chrono::steady_clock::time_point> next;

  Impl(MicrophoneOptions opts, std::unique_ptr<detail::MicrophoneDriver> value)
      : options(opts), driver(std::move(value)) {
    if (!driver)
      throw std::invalid_argument("Microphone driver is required");
    PcmSourceOptions pcm;
    pcm.channels = options.channels;
    pcm.max_queue_duration = options.max_queue_duration;
    source = std::make_shared<PcmAudioSource>(pcm);
  }

  void capture(const detail::MicrophoneBlock& block) noexcept {
    std::unique_lock lock(mutex);
    if (!stats.running || !stats.error.empty())
      return;
    try {
      if (block.sample_rate < 8000 || block.sample_rate > 96000 || block.sample_rate % 100 ||
          (block.channels != 1 && block.channels != 2) ||
          block.samples.size() != size_t(block.sample_rate / 100) * block.channels)
        throw std::invalid_argument("Microphone must supply 10 ms interleaved PCM16");
      const auto state = source->stats();
      if (!state.accepting) {
        ++stats.dropped_blocks;
        return;
      }
      if (state.generation != generation || rate != block.sample_rate) {
        if (state.generation != generation)
          next = block.captured;
        generation = state.generation;
        rate = block.sample_rate;
        resampler = std::make_unique<detail::PcmResampler>(rate, options.channels);
      }
      if (!next)
        next = block.captured;
      // Preserve sample cadence despite callback jitter. Long interruptions are
      // explicit gaps in whole 10 ms slots, never a compressed audio timeline.
      if (block.captured > *next + 30ms)
        *next += 10ms * ((block.captured - *next) / 10ms);
      ++stats.captured_blocks;
      // Bound accumulated device/steady-clock drift in the other direction too.
      // A fast device drops a block instead of producing ever more future audio.
      if (*next > block.captured + 30ms) {
        ++stats.dropped_blocks;
        return;
      }
      std::vector<int16_t> converted(size_t(rate / 100) * options.channels);
      for (size_t i = 0; i < size_t(rate / 100); ++i) {
        if (options.channels == block.channels) {
          for (size_t c = 0; c < options.channels; ++c)
            converted[i * options.channels + c] = block.samples[i * block.channels + c];
        } else if (options.channels == 1)
          converted[i] = int16_t((int(block.samples[i * 2]) + int(block.samples[i * 2 + 1])) / 2);
        else
          converted[i * 2] = converted[i * 2 + 1] = block.samples[i];
      }
      const auto pcm = resampler->process(converted);
      PcmInputFrame frame;
      frame.samples.assign(pcm.begin(), pcm.end());
      frame.capture_time = *next;
      *next += 10ms;
      if (detail::PcmSourceWriter::write(*source, frame, generation) != AudioWriteStatus::accepted)
        ++stats.dropped_blocks;
    } catch (...) {
      const auto error =
          normalized_failure(std::current_exception(), {"capture", {}, "microphone.device", 0},
                             ErrorDomain::media, ErrorCode::processing_failed);
      stats.failure = describe_failure(error);
      stats.error = stats.failure->message;
      auto handler = failure_handler;
      source->close();
      lock.unlock();
      if (handler) {
        try {
          handler(error);
        } catch (...) {
        }
      }
    }
  }
};

MicrophoneSession::MicrophoneSession(MicrophoneOptions options)
    : MicrophoneSession(options, detail::create_native_microphone_driver()) {}

MicrophoneSession::MicrophoneSession(MicrophoneOptions options,
                                     std::unique_ptr<detail::MicrophoneDriver> driver)
    : impl_(std::make_unique<Impl>(options, std::move(driver))) {}

MicrophoneSession::~MicrophoneSession() {
  stop();
}

std::shared_ptr<PcmAudioSource> MicrophoneSession::source() const {
  return impl_->source;
}

void MicrophoneSession::start() {
  auto& s = *impl_;
  std::lock_guard control(s.control);
  {
    std::lock_guard lock(s.mutex);
    if (s.stats.failure) {
      throw SDKError(s.stats.failure);
    }
    if (s.stats.running)
      return;
    const auto state = s.source->stats();
    if (!state.accepting || !state.attached || state.closed)
      throw std::logic_error("Start microphone source reader before device capture");
    s.rate = 0;
    s.resampler.reset();
    s.stats.running = true;
  }
  try {
    s.driver->start(s.options, [&s](const auto& block) { s.capture(block); });
  } catch (...) {
    s.driver->stop();
    std::lock_guard lock(s.mutex);
    s.stats.running = false;
    throw;
  }
}

void MicrophoneSession::stop() {
  auto& s = *impl_;
  std::lock_guard control(s.control);
  // Never hold the callback mutex across the driver's callback barrier.
  s.driver->stop();
  std::lock_guard lock(s.mutex);
  s.stats.running = false;
  s.resampler.reset();
  s.rate = 0;
}

MicrophoneSnapshot MicrophoneSession::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

void MicrophoneSession::set_failure_handler(std::function<void(std::exception_ptr)> handler) {
  std::shared_ptr<const FailureInfo> failure;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->failure_handler = handler;
    failure = impl_->stats.failure;
  }
  if (handler && failure) {
    handler(std::make_exception_ptr(SDKError(failure)));
  }
}

void MicrophoneSession::throw_if_failed() const {
  const auto state = snapshot();
  if (state.failure) {
    throw SDKError(state.failure);
  }
}
}  // namespace livekit::sdk
