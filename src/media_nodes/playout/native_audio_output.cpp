#include "media_nodes/playout/audio_output_transport.hpp"
#include "api/audio/audio_device.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "rtc_base/thread.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace livekit::sdk::rtc::detail {
namespace {
class OutputTransport final : public DeviceOutputTransport {
 public:
  OutputTransport(AudioOutputDriver::Render render, AudioOutputDriver::Failure failure)
      : render_(std::move(render)), failure_(std::move(failure)) {}

  int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t, uint32_t, uint32_t, int32_t,
                                  uint32_t level, bool, uint32_t& new_level) override {
    new_level = level;
    return 0;
  }

  void render(size_t frames, size_t channels, uint32_t rate, void* output) {
    if (!output || (channels != 1 && channels != 2) || rate < 8000 || rate > 96000 || rate % 100 ||
        frames != rate / 100) {
      throw std::runtime_error("Audio device requested unsupported playout format");
    }
    std::array<int16_t, 960> stereo{};
    if (render_) {
      render_(stereo);
    }
    if (rate == 48000 && channels == 2) {
      rate_ = rate;
      channels_ = channels;
      resampler_.reset();
      std::memcpy(output, stereo.data(), sizeof(stereo));
      return;
    }
    std::array<int16_t, 480> mono{};
    if (channels == 1) {
      for (size_t i = 0; i < 480; ++i) {
        mono[i] = int16_t((int(stereo[i * 2]) + int(stereo[i * 2 + 1])) / 2);
      }
    }
    if (rate_ != rate || channels_ != channels) {
      resampler_ = std::make_unique<webrtc::PushResampler<int16_t>>(480, frames, channels);
      rate_ = rate;
      channels_ = channels;
    }
    const auto result = resampler_->Resample(
        webrtc::InterleavedView<const int16_t>(channels == 1 ? mono.data() : stereo.data(), 480,
                                               channels),
        webrtc::InterleavedView<int16_t>(static_cast<int16_t*>(output), frames, channels));
    if (result != int(frames * channels)) {
      throw std::runtime_error("Audio device resampler returned incomplete playout block");
    }
  }

  int32_t NeedMorePlayData(size_t frames, size_t bytes_per_frame, size_t channels, uint32_t rate,
                           void* samples, size_t& out, int64_t* elapsed, int64_t* ntp) override {
    out = frames * channels;
    if (elapsed) {
      *elapsed = -1;
    }
    if (ntp) {
      *ntp = -1;
    }
    if (samples) {
      std::memset(samples, 0, frames * bytes_per_frame);
    }
    try {
      if (bytes_per_frame != channels * sizeof(int16_t)) {
        throw std::runtime_error("Audio device requested non-PCM16 playout");
      }
      render(frames, channels, rate, samples);
      return 0;
    } catch (const std::exception& e) {
      if (samples) {
        std::memset(samples, 0, frames * bytes_per_frame);
      }
      if (failure_) {
        failure_(e.what());
      }
      return -1;
    } catch (...) {
      if (samples) {
        std::memset(samples, 0, frames * bytes_per_frame);
      }
      if (failure_) {
        failure_("Unknown audio device playout failure");
      }
      return -1;
    }
  }

  void PullRenderData(int bits, int rate, size_t channels, size_t frames, void* samples,
                      int64_t* elapsed, int64_t* ntp) override {
    if (elapsed) {
      *elapsed = -1;
    }
    if (ntp) {
      *ntp = -1;
    }
    if (samples && bits > 0) {
      std::memset(samples, 0, frames * channels * size_t(bits / 8));
    }
    if (bits != 16) {
      return;
    }
    size_t out = 0;
    NeedMorePlayData(frames, channels * 2, channels, uint32_t(rate), samples, out, elapsed, ntp);
  }

  AudioOutputDriver::Render render_;
  AudioOutputDriver::Failure failure_;
  uint32_t rate_ = 0;
  size_t channels_ = 0;
  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
};

class NativeAudioOutput final : public AudioOutputDriver {
 public:
  ~NativeAudioOutput() override {
    stop();
    if (thread_) {
      thread_->Stop();
    }
  }

  void start(const AudioOutputOptions& options, Render render, Failure failure) override {
    if (!thread_) {
      thread_ = webrtc::Thread::Create();
      if (!thread_->Start()) {
        thread_.reset();
        throw std::runtime_error("Cannot start audio output control thread");
      }
    }
    const auto error = thread_->BlockingCall([&]() -> std::exception_ptr {
      try {
        if (adm_) {
          throw std::logic_error("Audio output device already started");
        }
        transport_ = create_audio_output_transport(std::move(render), std::move(failure));
        queues_ = webrtc::CreateDefaultTaskQueueFactory();
        adm_ = webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kPlatformDefaultAudio,
                                                 queues_.get());
        if (!adm_) {
          throw std::runtime_error("Cannot create audio output device");
        }
        require(adm_->Init(), "initialize audio output device");
        require(adm_->RegisterAudioCallback(transport_.get()), "register audio output callback");
        if (options.device_index) {
          require(adm_->SetPlayoutDevice(*options.device_index), "select audio output device");
        } else {
#ifdef _WIN32
          // Match the system's default playback destination. The communications
          // endpoint can be a different headset or HDMI monitor with no speakers.
          require(adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice),
                  "select default audio output device");
#else
          require(adm_->SetPlayoutDevice(uint16_t(0)), "select default audio output device");
#endif
        }
        require(adm_->InitSpeaker(), "initialize speaker mixer");
        bool stereo = false;
        adm_->StereoPlayoutIsAvailable(&stereo);
        require(adm_->SetStereoPlayout(stereo), "configure speaker channels");
        require(adm_->InitPlayout(), "initialize audio playout");
        require(adm_->StartPlayout(), "start audio playout");
        return {};
      } catch (...) {
        return std::current_exception();
      }
    });
    if (error) {
      stop();
      std::rethrow_exception(error);
    }
  }

  void stop() noexcept override {
    if (!thread_) {
      return;
    }
    thread_->BlockingCall([&] {
      if (adm_) {
        adm_->StopPlayout();
        adm_->RegisterAudioCallback(nullptr);
        adm_->Terminate();
        adm_ = nullptr;
      }
      transport_.reset();
      queues_.reset();
    });
  }

 private:
  static void require(int32_t result, const char* operation) {
    if (result) {
      throw std::runtime_error(std::string("Cannot ") + operation);
    }
  }

  std::unique_ptr<webrtc::Thread> thread_;
  std::unique_ptr<webrtc::TaskQueueFactory> queues_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  std::unique_ptr<DeviceOutputTransport> transport_;
};
}  // namespace

std::unique_ptr<DeviceOutputTransport> create_audio_output_transport(
    AudioOutputDriver::Render render, AudioOutputDriver::Failure failure) {
  return std::make_unique<OutputTransport>(std::move(render), std::move(failure));
}

std::unique_ptr<AudioOutputDriver> create_native_audio_output_driver() {
  return std::make_unique<NativeAudioOutput>();
}
}  // namespace livekit::sdk::rtc::detail
