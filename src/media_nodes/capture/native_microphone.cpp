#include "media_nodes/capture/microphone_transport.hpp"
#include "api/audio/audio_device.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "rtc_base/thread.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace livekit::sdk::detail {
namespace {
class MicrophoneTransport final : public DeviceMicrophoneTransport {
 public:
  explicit MicrophoneTransport(MicrophoneDriver::Callback callback) : callback_(std::move(callback)) {}
  int32_t RecordedDataIsAvailable(const void* samples,size_t frames,size_t bytes_per_frame,size_t channels,
      uint32_t rate,uint32_t delay,int32_t,uint32_t level,bool,uint32_t& new_level) override {
    new_level=level;
    // ADM uses bytes per interleaved frame here, not bytes per channel sample.
    if (!samples || !callback_) return -1;
    MicrophoneBlock block;
    block.sample_rate=int(rate); block.channels=channels;
    block.captured=std::chrono::steady_clock::now()-std::chrono::milliseconds(std::min(delay,4000u));
    if ((channels==1 || channels==2) && bytes_per_frame==channels*sizeof(int16_t) && frames<=960)
      block.samples={static_cast<const int16_t*>(samples),frames*channels};
    // Invalid input reaches session validation without throwing through ADM.
    callback_(block); return 0;
  }
  int32_t NeedMorePlayData(size_t frames,size_t bytes_per_frame,size_t channels,uint32_t,void* samples,
      size_t& out,int64_t* elapsed,int64_t* ntp) override {
    out=frames*channels; if(elapsed)*elapsed=-1; if(ntp)*ntp=-1;
    if(samples)std::memset(samples,0,frames*bytes_per_frame); return 0;
  }
  void PullRenderData(int bits,int,size_t channels,size_t frames,void* samples,int64_t* elapsed,int64_t* ntp) override {
    if(elapsed)*elapsed=-1; if(ntp)*ntp=-1;
    if(samples && bits>0)std::memset(samples,0,frames*channels*size_t(bits/8));
  }
  MicrophoneDriver::Callback callback_;
};
class NativeMicrophone final : public MicrophoneDriver {
 public:
  ~NativeMicrophone() override { stop(); if(thread_) thread_->Stop(); }
  void start(const MicrophoneOptions& options, Callback callback) override {
    if (!thread_) {
      thread_=webrtc::Thread::Create();
      if (!thread_->Start()) { thread_.reset(); throw std::runtime_error("Cannot start microphone control thread"); }
    }
    const auto failure=thread_->BlockingCall([&]() -> std::exception_ptr {
      try {
        if (adm_) throw std::logic_error("Microphone already started");
        transport_=create_microphone_transport(std::move(callback));
        queues_=webrtc::CreateDefaultTaskQueueFactory();
        adm_=webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kPlatformDefaultAudio,queues_.get());
        if (!adm_) throw std::runtime_error("Cannot create microphone device");
        require(adm_->Init(),"initialize microphone device");
        require(adm_->RegisterAudioCallback(transport_.get()),"register microphone callback");
        if (options.device_index) require(adm_->SetRecordingDevice(*options.device_index),"select microphone");
        else {
#ifdef _WIN32
          require(adm_->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice),"select default microphone");
#else
          require(adm_->SetRecordingDevice(uint16_t(0)),"select default microphone");
#endif
        }
        require(adm_->InitMicrophone(),"initialize microphone mixer");
        bool stereo=false;
        if (options.channels==2) adm_->StereoRecordingIsAvailable(&stereo);
        require(adm_->SetStereoRecording(stereo),"configure microphone channels");
        require(adm_->InitRecording(),"initialize microphone capture");
        require(adm_->StartRecording(),"start microphone capture");
        return {};
      } catch (...) { return std::current_exception(); }
    });
    if (failure) { stop(); std::rethrow_exception(failure); }
  }
  void stop() noexcept override {
    if (!thread_) return;
    thread_->BlockingCall([&] {
      if (adm_) {
        adm_->StopRecording();
        adm_->RegisterAudioCallback(nullptr);
        adm_->Terminate(); adm_=nullptr;
      }
      transport_.reset(); queues_.reset();
    });
  }
 private:
  static void require(int32_t result,const char* operation) {
    if (result) throw std::runtime_error(std::string("Cannot ")+operation);
  }
  std::unique_ptr<webrtc::Thread> thread_;
  std::unique_ptr<webrtc::TaskQueueFactory> queues_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  std::unique_ptr<DeviceMicrophoneTransport> transport_;
};
}
std::unique_ptr<DeviceMicrophoneTransport> create_microphone_transport(MicrophoneDriver::Callback callback) { return std::make_unique<MicrophoneTransport>(std::move(callback)); }
std::unique_ptr<MicrophoneDriver> create_native_microphone_driver() { return std::make_unique<NativeMicrophone>(); }
}
