#include "media_nodes/capture/microphone_transport.hpp"
#include "api/audio/audio_device.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "rtc_base/thread.h"
#include <algorithm>
#include <cstring>

namespace livekit::sdk::detail {
namespace {
class MicrophoneTransport final : public DeviceMicrophoneTransport {
 public:
  explicit MicrophoneTransport(MicrophoneDriver::Callback callback)
      : callback_(std::move(callback)) {}

  int32_t RecordedDataIsAvailable(const void* samples, size_t frames, size_t bytes_per_frame,
                                  size_t channels, uint32_t rate, uint32_t delay, int32_t,
                                  uint32_t level, bool, uint32_t& new_level) override {
    new_level = level;
    // ADM uses bytes per interleaved frame here, not bytes per channel sample.
    if (!samples || !callback_)
      return -1;
    MicrophoneBlock block;
    block.sample_rate = int(rate);
    block.channels = channels;
    block.captured =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(std::min(delay, 4000u));
    if ((channels == 1 || channels == 2) && bytes_per_frame == channels * sizeof(int16_t) &&
        frames <= 960)
      block.samples = {static_cast<const int16_t*>(samples), frames * channels};
    // Invalid input reaches session validation without throwing through ADM.
    callback_(block);
    return 0;
  }

  int32_t NeedMorePlayData(size_t frames, size_t bytes_per_frame, size_t channels, uint32_t,
                           void* samples, size_t& out, int64_t* elapsed, int64_t* ntp) override {
    out = frames * channels;
    if (elapsed)
      *elapsed = -1;
    if (ntp)
      *ntp = -1;
    if (samples)
      std::memset(samples, 0, frames * bytes_per_frame);
    return 0;
  }

  void PullRenderData(int bits, int, size_t channels, size_t frames, void* samples,
                      int64_t* elapsed, int64_t* ntp) override {
    if (elapsed)
      *elapsed = -1;
    if (ntp)
      *ntp = -1;
    if (samples && bits > 0)
      std::memset(samples, 0, frames * channels * size_t(bits / 8));
  }

  MicrophoneDriver::Callback callback_;
};

class NativeMicrophone final : public MicrophoneDriver {
 public:
  ~NativeMicrophone() override {
    stop();
    if (thread_)
      thread_->Stop();
  }

  Result<void> start(const MicrophoneOptions& options, Callback callback) override {
    if (!thread_) {
      thread_ = webrtc::Thread::Create();
      if (!thread_->Start()) {
        thread_.reset();
        return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                  "Cannot start microphone control thread"));
      }
    }
    auto result = Result<void>::success();
    thread_->BlockingCall([&] {
      result = [&]() -> Result<void> {
        if (adm_)
          return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_state,
                                                    "Microphone already started"));
        transport_ = create_microphone_transport(std::move(callback));
        queues_ = webrtc::CreateDefaultTaskQueueFactory();
        adm_ = webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kPlatformDefaultAudio,
                                                 queues_.get());
        if (!adm_)
          return Result<void>::failure(make_failure(
              ErrorDomain::media, ErrorCode::processing_failed, "Cannot create microphone device"));
        if (auto checked = require(adm_->Init(), "initialize microphone device"); !checked) {
          return checked;
        }
        if (auto checked = require(adm_->RegisterAudioCallback(transport_.get()),
                                   "register microphone callback");
            !checked) {
          return checked;
        }
        if (options.device_index) {
          if (auto checked =
                  require(adm_->SetRecordingDevice(*options.device_index), "select microphone");
              !checked) {
            return checked;
          }
        } else {
#ifdef _WIN32
          if (auto checked = require(
                  adm_->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice),
                  "select default microphone");
              !checked) {
            return checked;
          }
#else
          if (auto checked =
                  require(adm_->SetRecordingDevice(uint16_t(0)), "select default microphone");
              !checked) {
            return checked;
          }
#endif
        }
        if (auto checked = require(adm_->InitMicrophone(), "initialize microphone mixer");
            !checked) {
          return checked;
        }
        bool stereo = false;
        if (options.channels == 2)
          adm_->StereoRecordingIsAvailable(&stereo);
        if (auto checked =
                require(adm_->SetStereoRecording(stereo), "configure microphone channels");
            !checked) {
          return checked;
        }
        if (auto checked = require(adm_->InitRecording(), "initialize microphone capture");
            !checked) {
          return checked;
        }
        if (auto checked = require(adm_->StartRecording(), "start microphone capture"); !checked) {
          return checked;
        }
        return Result<void>::success();
      }();
    });
    if (!result) {
      stop();
    }
    return result;
  }

  void stop() noexcept override {
    if (!thread_)
      return;
    thread_->BlockingCall([&] {
      if (adm_) {
        adm_->StopRecording();
        adm_->RegisterAudioCallback(nullptr);
        adm_->Terminate();
        adm_ = nullptr;
      }
      transport_.reset();
      queues_.reset();
    });
  }

 private:
  static Result<void> require(int32_t result, const char* operation) {
    if (result) {
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                std::string("Cannot ") + operation, false, result));
    }
    return Result<void>::success();
  }

  std::unique_ptr<webrtc::Thread> thread_;
  std::unique_ptr<webrtc::TaskQueueFactory> queues_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  std::unique_ptr<DeviceMicrophoneTransport> transport_;
};
}  // namespace

std::unique_ptr<DeviceMicrophoneTransport> create_microphone_transport(
    MicrophoneDriver::Callback callback) {
  return std::make_unique<MicrophoneTransport>(std::move(callback));
}

std::unique_ptr<MicrophoneDriver> create_native_microphone_driver() {
  return std::make_unique<NativeMicrophone>();
}
}  // namespace livekit::sdk::detail
