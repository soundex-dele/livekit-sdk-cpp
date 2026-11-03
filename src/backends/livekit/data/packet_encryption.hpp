#pragma once
#include "livekit/compat/span.hpp"
#include "livekit/client/encryption.hpp"
#include "livekit.pb.h"

namespace livekit::sdk::client::detail {
// RTP depacketizers reconstruct four-byte Annex-B start codes. Canonicalize
// before authenticating a native outgoing frame so its AAD survives RTP.
std::vector<uint8_t> canonical_media_frame(livekit::compat::span<const uint8_t> frame,
                                           EncryptionCodec codec);

struct SealedPayload {
  std::vector<uint8_t> ciphertext;
  std::array<uint8_t, 12> iv;
  uint8_t key_index;
};

Result<SealedPayload> encrypt_payload(const KeyProvider& keys,
                                      livekit::compat::span<const uint8_t> plaintext,
                                      livekit::compat::span<const uint8_t> aad = {});
Result<std::vector<uint8_t>> decrypt_payload(const KeyProvider& keys, uint8_t index,
                                             livekit::compat::span<const uint8_t> iv,
                                             livekit::compat::span<const uint8_t> ciphertext,
                                             livekit::compat::span<const uint8_t> aad = {});
bool encryptable(const livekit::DataPacket& packet);
Result<livekit::DataPacket> encrypt_packet(const livekit::DataPacket& packet,
                                           const EncryptionOptions& options);
Result<livekit::DataPacket> decrypt_packet(const livekit::DataPacket& packet,
                                           const EncryptionOptions& options);
}  // namespace livekit::sdk::client::detail
