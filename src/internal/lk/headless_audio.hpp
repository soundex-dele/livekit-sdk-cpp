#pragma once
#include "api/audio/audio_device.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
namespace lk {
// A clocked, silent output device. Pulling every 10 ms drives NetEq decoding and
// remote AudioTrack sinks even on machines with no sound card.
class HeadlessAudioDevice : public webrtc::AudioDeviceModule {
 public:
  ~HeadlessAudioDevice() override { Terminate(); }
  int32_t ActiveAudioLayer(AudioLayer* layer) const override { *layer = kDummyAudio; return 0; }
  int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override { std::lock_guard lock(mutex_); transport_ = transport; return 0; }
  int32_t Init() override { initialized_ = true; return 0; }
  int32_t Terminate() override { StopPlayout(); recording_ = false; initialized_ = false; return 0; }
  bool Initialized() const override { return initialized_; }
  int16_t PlayoutDevices() override { return 1; }
  int16_t RecordingDevices() override { return 0; }
  int32_t PlayoutDeviceName(uint16_t, char* name, char* guid) override { std::strcpy(name, "Headless PCM output"); guid[0] = 0; return 0; }
  int32_t RecordingDeviceName(uint16_t, char* name, char* guid) override { name[0] = guid[0] = 0; return -1; }
  int32_t SetPlayoutDevice(uint16_t) override { return 0; }
  int32_t SetPlayoutDevice(WindowsDeviceType) override { return 0; }
  int32_t SetRecordingDevice(uint16_t) override { return 0; }
  int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }
  int32_t PlayoutIsAvailable(bool* value) override { *value = true; return 0; }
  int32_t RecordingIsAvailable(bool* value) override { *value = false; return 0; }
  int32_t InitPlayout() override { playout_initialized_ = true; return 0; }
  int32_t InitRecording() override { recording_initialized_ = true; return 0; }
  bool PlayoutIsInitialized() const override { return playout_initialized_; }
  bool RecordingIsInitialized() const override { return recording_initialized_; }
  int32_t StartPlayout() override {
    if (playing_.exchange(true)) return 0;
    thread_ = std::thread([this] {
      int16_t output[480]{};
      auto next = std::chrono::steady_clock::now();
      while (playing_) {
        { std::lock_guard lock(mutex_); if (transport_) {
          size_t count = 0; int64_t elapsed = 0, ntp = 0;
          transport_->NeedMorePlayData(480, 2, 1, 48000, output, count, &elapsed, &ntp);
        } }
        next += std::chrono::milliseconds(10);
        if (next < std::chrono::steady_clock::now() - std::chrono::milliseconds(50)) next = std::chrono::steady_clock::now();
        std::this_thread::sleep_until(next);
      }
    }); return 0;
  }
  int32_t StopPlayout() override { playing_ = false; if (thread_.joinable()) thread_.join(); return 0; }
  bool Playing() const override { return playing_; }
  int32_t StartRecording() override { recording_ = true; return 0; }
  int32_t StopRecording() override { recording_ = false; return 0; }
  bool Recording() const override { return recording_; }
  int32_t InitSpeaker() override { return 0; }
  int32_t InitMicrophone() override { return 0; }
  bool SpeakerIsInitialized() const override { return initialized_; }
  bool MicrophoneIsInitialized() const override { return initialized_; }
  int32_t SpeakerVolumeIsAvailable(bool* value) override { *value = false; return 0; }
  int32_t MicrophoneVolumeIsAvailable(bool* value) override { *value = false; return 0; }
  int32_t SetSpeakerVolume(uint32_t) override { return -1; }
  int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
  int32_t SpeakerVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t MicrophoneVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t MaxSpeakerVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t MinSpeakerVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t MaxMicrophoneVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t MinMicrophoneVolume(uint32_t* value) const override { *value = 0; return 0; }
  int32_t SpeakerMuteIsAvailable(bool* value) override { *value = false; return 0; }
  int32_t MicrophoneMuteIsAvailable(bool* value) override { *value = false; return 0; }
  int32_t SetSpeakerMute(bool) override { return -1; }
  int32_t SetMicrophoneMute(bool) override { return -1; }
  int32_t SpeakerMute(bool* value) const override { *value = false; return 0; }
  int32_t MicrophoneMute(bool* value) const override { *value = false; return 0; }
  int32_t StereoPlayoutIsAvailable(bool* value) const override { *value = false; return 0; }
  int32_t StereoRecordingIsAvailable(bool* value) const override { *value = false; return 0; }
  int32_t SetStereoPlayout(bool value) override { return value ? -1 : 0; }
  int32_t SetStereoRecording(bool value) override { return value ? -1 : 0; }
  int32_t StereoPlayout(bool* value) const override { *value = false; return 0; }
  int32_t StereoRecording(bool* value) const override { *value = false; return 0; }
  int32_t PlayoutDelay(uint16_t* value) const override { *value = 0; return 0; }
  bool BuiltInAECIsAvailable() const override { return false; }
  bool BuiltInAGCIsAvailable() const override { return false; }
  bool BuiltInNSIsAvailable() const override { return false; }
  int32_t EnableBuiltInAEC(bool) override { return -1; }
  int32_t EnableBuiltInAGC(bool) override { return -1; }
  int32_t EnableBuiltInNS(bool) override { return -1; }
 private:
  bool initialized_ = false, playout_initialized_ = false, recording_initialized_ = false, recording_ = false;
  std::atomic<bool> playing_{false};
  std::mutex mutex_;
  webrtc::AudioTransport* transport_ = nullptr;
  std::thread thread_;
};
}
