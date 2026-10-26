#pragma once
#include "engines/rtc/rtc_graph.hpp"
#include "api/audio/audio_device.h"
#include <cstring>

namespace livekit::sdk::detail {
// Dedicated nodes on the ADM callback path. No PCM allocation or extra queue:
// WebRTC still owns APM, AEC reference processing, NetEq and device timing.
class AudioCaptureNode final : public MediaNode {
 public:
  AudioCaptureNode() : MediaNode("AudioCaptureNode", NodeExecution::source, "PCM16") {}
  template<class F> int32_t capture(F&& deliver) {
    ++inputs; auto result = deliver(); if (result == 0) ++outputs; else ++errors; return result;
  }
};
class AudioOutputNode final : public MediaNode {
 public:
  AudioOutputNode() : MediaNode("AudioOutputNode", NodeExecution::sink, "PCM16") {}
  template<class F> int32_t render(F&& pull) {
    ++inputs; auto result = pull(); if (result == 0) ++outputs; else ++errors; return result;
  }
};
class GraphAudioDevice : public webrtc::AudioDeviceModule, private webrtc::AudioTransport {
 public:
  GraphAudioDevice(webrtc::scoped_refptr<webrtc::AudioDeviceModule> device, const std::shared_ptr<RtcGraph>& graph)
      : device_(std::move(device)), capture_(graph->add(std::make_shared<AudioCaptureNode>())),
        output_(graph->add(std::make_shared<AudioOutputNode>())) { capture_->start(); output_->start(); }
  ~GraphAudioDevice() override { Terminate(); capture_->release(); output_->release(); }
  int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override {
    // The delegate owns the callback barrier, just as with a direct transport.
    auto result = device_->RegisterAudioCallback(nullptr);
    if (result != 0) return result;
    transport_.store(transport);
    return device_->RegisterAudioCallback(transport ? this : nullptr);
  }
  int32_t Terminate() override {
    device_->StopRecording(); device_->StopPlayout();
    device_->RegisterAudioCallback(nullptr); transport_.store(nullptr);
    return device_->Terminate();
  }
  int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override { return device_->ActiveAudioLayer(audioLayer); }
  int32_t Init()  override { return device_->Init(); }
  bool Initialized() const override { return device_->Initialized(); }
  int16_t PlayoutDevices()  override { return device_->PlayoutDevices(); }
  int16_t RecordingDevices()  override { return device_->RecordingDevices(); }
  int32_t PlayoutDeviceName(uint16_t index, char* name, char* guid) override { return device_->PlayoutDeviceName(index, name, guid); }
  int32_t RecordingDeviceName(uint16_t index, char* name, char* guid) override { return device_->RecordingDeviceName(index, name, guid); }
  int32_t SetPlayoutDevice(uint16_t index)  override { return device_->SetPlayoutDevice(index); }
  int32_t SetPlayoutDevice(WindowsDeviceType device)  override { return device_->SetPlayoutDevice(device); }
  int32_t SetRecordingDevice(uint16_t index)  override { return device_->SetRecordingDevice(index); }
  int32_t SetRecordingDevice(WindowsDeviceType device)  override { return device_->SetRecordingDevice(device); }
  int32_t PlayoutIsAvailable(bool* available)  override { return device_->PlayoutIsAvailable(available); }
  int32_t InitPlayout()  override { return device_->InitPlayout(); }
  bool PlayoutIsInitialized() const override { return device_->PlayoutIsInitialized(); }
  int32_t RecordingIsAvailable(bool* available)  override { return device_->RecordingIsAvailable(available); }
  int32_t InitRecording()  override { return device_->InitRecording(); }
  bool RecordingIsInitialized() const override { return device_->RecordingIsInitialized(); }
  int32_t StartPlayout()  override { return device_->StartPlayout(); }
  int32_t StopPlayout()  override { return device_->StopPlayout(); }
  bool Playing() const override { return device_->Playing(); }
  int32_t StartRecording()  override { return device_->StartRecording(); }
  int32_t StopRecording()  override { return device_->StopRecording(); }
  bool Recording() const override { return device_->Recording(); }
  int32_t InitSpeaker()  override { return device_->InitSpeaker(); }
  bool SpeakerIsInitialized() const override { return device_->SpeakerIsInitialized(); }
  int32_t InitMicrophone()  override { return device_->InitMicrophone(); }
  bool MicrophoneIsInitialized() const override { return device_->MicrophoneIsInitialized(); }
  int32_t SpeakerVolumeIsAvailable(bool* available)  override { return device_->SpeakerVolumeIsAvailable(available); }
  int32_t SetSpeakerVolume(uint32_t volume)  override { return device_->SetSpeakerVolume(volume); }
  int32_t SpeakerVolume(uint32_t* volume) const override { return device_->SpeakerVolume(volume); }
  int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override { return device_->MaxSpeakerVolume(maxVolume); }
  int32_t MinSpeakerVolume(uint32_t* minVolume) const override { return device_->MinSpeakerVolume(minVolume); }
  int32_t MicrophoneVolumeIsAvailable(bool* available)  override { return device_->MicrophoneVolumeIsAvailable(available); }
  int32_t SetMicrophoneVolume(uint32_t volume)  override { return device_->SetMicrophoneVolume(volume); }
  int32_t MicrophoneVolume(uint32_t* volume) const override { return device_->MicrophoneVolume(volume); }
  int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const override { return device_->MaxMicrophoneVolume(maxVolume); }
  int32_t MinMicrophoneVolume(uint32_t* minVolume) const override { return device_->MinMicrophoneVolume(minVolume); }
  int32_t SpeakerMuteIsAvailable(bool* available)  override { return device_->SpeakerMuteIsAvailable(available); }
  int32_t SetSpeakerMute(bool enable)  override { return device_->SetSpeakerMute(enable); }
  int32_t SpeakerMute(bool* enabled) const override { return device_->SpeakerMute(enabled); }
  int32_t MicrophoneMuteIsAvailable(bool* available)  override { return device_->MicrophoneMuteIsAvailable(available); }
  int32_t SetMicrophoneMute(bool enable)  override { return device_->SetMicrophoneMute(enable); }
  int32_t MicrophoneMute(bool* enabled) const override { return device_->MicrophoneMute(enabled); }
  int32_t StereoPlayoutIsAvailable(bool* available) const override { return device_->StereoPlayoutIsAvailable(available); }
  int32_t SetStereoPlayout(bool enable)  override { return device_->SetStereoPlayout(enable); }
  int32_t StereoPlayout(bool* enabled) const override { return device_->StereoPlayout(enabled); }
  int32_t StereoRecordingIsAvailable(bool* available) const override { return device_->StereoRecordingIsAvailable(available); }
  int32_t SetStereoRecording(bool enable)  override { return device_->SetStereoRecording(enable); }
  int32_t StereoRecording(bool* enabled) const override { return device_->StereoRecording(enabled); }
  int32_t PlayoutDelay(uint16_t* delayMS) const override { return device_->PlayoutDelay(delayMS); }
  bool BuiltInAECIsAvailable() const override { return device_->BuiltInAECIsAvailable(); }
  bool BuiltInAGCIsAvailable() const override { return device_->BuiltInAGCIsAvailable(); }
  bool BuiltInNSIsAvailable() const override { return device_->BuiltInNSIsAvailable(); }
  int32_t EnableBuiltInAEC(bool enable)  override { return device_->EnableBuiltInAEC(enable); }
  int32_t EnableBuiltInAGC(bool enable)  override { return device_->EnableBuiltInAGC(enable); }
  int32_t EnableBuiltInNS(bool enable)  override { return device_->EnableBuiltInNS(enable); }

  int32_t GetPlayoutUnderrunCount() const override { return device_->GetPlayoutUnderrunCount(); }
  std::optional<Stats> GetStats() const override { return device_->GetStats(); }
  bool IsStopOnMuteModeEnabled() const override { return device_->IsStopOnMuteModeEnabled(); }
  int32_t SetObserver(webrtc::AudioDeviceObserver* o) override { return device_->SetObserver(o); }
  int32_t GetPlayoutDevice() const override { return device_->GetPlayoutDevice(); }
  int32_t GetRecordingDevice() const override { return device_->GetRecordingDevice(); }
 private:
  int32_t RecordedDataIsAvailable(const void* data, size_t samples, size_t bytes,
      size_t channels, uint32_t rate, uint32_t delay, int32_t drift, uint32_t level,
      bool key, uint32_t& new_level) override {
    return RecordedDataIsAvailable(data, samples, bytes, channels, rate, delay, drift, level, key, new_level, std::nullopt);
  }
  int32_t RecordedDataIsAvailable(const void* data, size_t samples, size_t bytes,
      size_t channels, uint32_t rate, uint32_t delay, int32_t drift, uint32_t level,
      bool key, uint32_t& new_level, std::optional<int64_t> capture_time) override {
    new_level = level;
    return capture_->capture([&] {
      auto* transport = transport_.load();
      return transport ? transport->RecordedDataIsAvailable(data, samples, bytes, channels,
          rate, delay, drift, level, key, new_level, capture_time) : -1;
    });
  }
  int32_t NeedMorePlayData(size_t samples, size_t bytes, size_t channels, uint32_t rate,
      void* data, size_t& count, int64_t* elapsed, int64_t* ntp) override {
    return output_->render([&] {
      if (auto* transport = transport_.load())
        return transport->NeedMorePlayData(samples, bytes, channels, rate, data, count, elapsed, ntp);
      std::memset(data, 0, samples * bytes * channels); count = samples;
      if (elapsed) *elapsed = -1; if (ntp) *ntp = -1; return int32_t(0);
    });
  }
  void PullRenderData(int bits, int rate, size_t channels, size_t frames, void* data,
      int64_t* elapsed, int64_t* ntp) override {
    output_->render([&] {
      if (auto* transport = transport_.load()) transport->PullRenderData(bits, rate, channels, frames, data, elapsed, ntp);
      else { std::memset(data, 0, frames * channels * (bits / 8));
        if (elapsed) *elapsed = -1; if (ntp) *ntp = -1; }
      return int32_t(0);
    });
  }
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> device_;
  std::shared_ptr<AudioCaptureNode> capture_;
  std::shared_ptr<AudioOutputNode> output_;
  std::atomic<webrtc::AudioTransport*> transport_{nullptr};
};
}
