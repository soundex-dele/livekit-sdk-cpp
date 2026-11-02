#pragma once
#include "livekit/compat/span.hpp"
#include "livekit/result.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace livekit::sdk {
using EncryptionKey = std::array<uint8_t, 16>;

// Implementations are invoked on transport/media workers and must not block.
class KeyProvider {
 public:
  virtual ~KeyProvider() = default;
  virtual uint8_t current_key_index() const = 0;
  virtual Result<EncryptionKey> key(uint8_t index) const = 0;
};

class ExternalKeyProvider final : public KeyProvider {
 public:
  ExternalKeyProvider();
  ~ExternalKeyProvider();
  Result<void> set_key(uint8_t index, livekit::compat::span<const uint8_t> key);
  Result<void> set_passphrase(uint8_t index, const std::string& passphrase);
  void remove_key(uint8_t index);
  uint8_t current_key_index() const override;
  Result<EncryptionKey> key(uint8_t index) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct EncryptionOptions {
  std::shared_ptr<KeyProvider> keys;
  bool data = true, media = false;
  // Fail closed for encryptable incoming packets/frames when encryption is enabled.
  bool require_encrypted = true;
};

struct EncryptionStateEvent {
  std::string track_id;
  bool media = false, encrypting = false, ok = false;
  int key_index = -1;
};
enum class EncryptionCodec { opus, h264, h265 };

class FrameCryptor {
 public:
  FrameCryptor(std::shared_ptr<KeyProvider> keys, EncryptionCodec codec);
  Result<std::vector<uint8_t>> encrypt(livekit::compat::span<const uint8_t> frame) const;
  // nullopt identifies a server-injected sentinel frame, not decryption failure.
  Result<std::optional<std::vector<uint8_t>>> decrypt(
      livekit::compat::span<const uint8_t> frame,
      livekit::compat::span<const uint8_t> sif_trailer = {}) const;

 private:
  std::shared_ptr<KeyProvider> keys_;
  EncryptionCodec codec_;
};
}  // namespace livekit::sdk
