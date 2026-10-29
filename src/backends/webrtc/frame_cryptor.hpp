#pragma once
#include "livekit/encryption.hpp"
#include "backends/rtc/encryption.hpp"
#include "livekit.pb.h"
#include "api/crypto/frame_encryptor_interface.h"
#include "api/crypto/frame_decryptor_interface.h"
#include "api/make_ref_counted.h"
#include <algorithm>
#include <map>
#include <cctype>
#include <mutex>
#include <functional>
#include <atomic>

namespace livekit::sdk::detail {
inline EncryptionCodec encryption_codec(std::string codec) {
  std::transform(codec.begin(), codec.end(), codec.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (codec == "opus" || codec == "audio/opus")
    return EncryptionCodec::opus;
  if (codec == "h264" || codec == "video/h264")
    return EncryptionCodec::h264;
  if (codec == "h265" || codec == "video/h265")
    return EncryptionCodec::h265;
  throw std::invalid_argument("E2EE codec must be Opus, H264 or H265");
}

struct MediaCryptorContext {
  struct Track {
    std::string owner, codec;
    bool encrypted = false;
    std::string sync_group;
  };

  mutable std::mutex mutex;
  std::map<std::string, Track> tracks;
  std::function<void(const std::string&, bool, int)> report;
  EncryptionOptions options;
  std::vector<uint8_t> sif;

  void participant(const livekit::ParticipantInfo& p) {
    std::lock_guard lock(mutex);
    std::erase_if(tracks, [&](const auto& t) { return t.second.owner == p.identity(); });
    if (p.state() != 3)
      for (auto& t : p.tracks())
        tracks[t.sid()] = {
            p.identity(), t.mime_type(), t.encryption() != livekit::Encryption::NONE,
            p.sid() + "|" +
                (t.stream().empty() ? (t.source() == livekit::SCREEN_SHARE ||
                                               t.source() == livekit::SCREEN_SHARE_AUDIO
                                           ? "screen"
                                           : "camera")
                                    : t.stream())};
  }
};

class NativeFrameEncryptor : public webrtc::FrameEncryptorInterface {
 public:
  NativeFrameEncryptor(std::shared_ptr<KeyProvider> keys, EncryptionCodec codec,
                       std::function<void(bool, int)> report = {})
      : keys_(keys), cryptor_(std::move(keys), codec), codec_(codec), report_(std::move(report)) {}

  int Encrypt(webrtc::MediaType, uint32_t, webrtc::ArrayView<const uint8_t>,
              webrtc::ArrayView<const uint8_t> frame, webrtc::ArrayView<uint8_t> output,
              size_t* written) override {
    *written = 0;
    try {
      auto canonical = canonical_media_frame({frame.data(), frame.size()}, codec_);
      auto bytes = cryptor_.encrypt(canonical);
      if (bytes.size() > output.size())
        return 1;
      std::copy(bytes.begin(), bytes.end(), output.begin());
      *written = bytes.size();
      report(true, keys_->current_key_index());
      return 0;
    } catch (...) {
      report(false, -1);
      return 1;
    }
  }

  size_t GetMaxCiphertextByteSize(webrtc::MediaType, size_t size) override {
    return size * 2 + 64;
  }

 private:
  void report(bool ok, int key) noexcept {
    const int state = ok ? key + 1 : -1;
    if (last_.exchange(state) != state && report_)
      try {
        report_(ok, key);
      } catch (...) {
      }
  }

  std::shared_ptr<KeyProvider> keys_;
  FrameCryptor cryptor_;
  EncryptionCodec codec_;
  std::function<void(bool, int)> report_;
  std::atomic<int> last_{-2};
};

class NativeFrameDecryptor : public webrtc::FrameDecryptorInterface {
 public:
  NativeFrameDecryptor(std::shared_ptr<MediaCryptorContext> context, std::string track)
      : context_(std::move(context)), track_(std::move(track)) {}

  Result Decrypt(webrtc::MediaType, const std::vector<uint32_t>&, webrtc::ArrayView<const uint8_t>,
                 webrtc::ArrayView<const uint8_t> encrypted,
                 webrtc::ArrayView<uint8_t> output) override {
    try {
      MediaCryptorContext::Track track;
      {
        std::lock_guard lock(context_->mutex);
        auto it = context_->tracks.find(track_);
        if (it == context_->tracks.end()) {
          report(false, -1);
          return {Status::kRecoverable, 0};
        }
        track = it->second;
      }
      if (!track.encrypted) {
        if (context_->options.require_encrypted || encrypted.size() > output.size()) {
          report(false, -1);
          return {Status::kFailedToDecrypt, 0};
        }
        std::copy(encrypted.begin(), encrypted.end(), output.begin());
        return {Status::kOk, encrypted.size()};
      }
      FrameCryptor cryptor(context_->options.keys, encryption_codec(track.codec));
      auto frame = cryptor.decrypt({encrypted.data(), encrypted.size()}, context_->sif);
      if (!frame)
        return {Status::kRecoverable, 0};
      if (frame->size() > output.size())
        return {Status::kFailedToDecrypt, 0};
      std::copy(frame->begin(), frame->end(), output.begin());
      report(true, encrypted.empty() ? -1 : encrypted[encrypted.size() - 1]);
      return {Status::kOk, frame->size()};
    } catch (...) {
      report(false, -1);
      return {Status::kFailedToDecrypt, 0};
    }
  }

  size_t GetMaxPlaintextByteSize(webrtc::MediaType, size_t size) override {
    return size;
  }

 private:
  void report(bool ok, int key) noexcept {
    const int state = ok ? key + 1 : -1;
    if (last_.exchange(state) != state && context_->report)
      try {
        context_->report(track_, ok, key);
      } catch (...) {
      }
  }

  std::shared_ptr<MediaCryptorContext> context_;
  std::string track_;
  std::atomic<int> last_{-2};
};
}  // namespace livekit::sdk::detail
