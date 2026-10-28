#pragma once
#include "livekit/encryption.hpp"
#include "livekit.pb.h"

namespace livekit::sdk::detail {
// RTP depacketizers reconstruct four-byte Annex-B start codes. Canonicalize
// before authenticating a native outgoing frame so its AAD survives RTP.
std::vector<uint8_t> canonical_media_frame(std::span<const uint8_t> frame, EncryptionCodec codec);
struct SealedPayload {std::vector<uint8_t> ciphertext;std::array<uint8_t,12> iv;uint8_t key_index;};
SealedPayload encrypt_payload(const KeyProvider& keys,std::span<const uint8_t> plaintext,std::span<const uint8_t> aad={});
std::vector<uint8_t> decrypt_payload(const KeyProvider& keys,uint8_t index,std::span<const uint8_t> iv,std::span<const uint8_t> ciphertext,std::span<const uint8_t> aad={});
bool encryptable(const livekit::DataPacket& packet);
livekit::DataPacket encrypt_packet(const livekit::DataPacket& packet,const EncryptionOptions& options);
livekit::DataPacket decrypt_packet(const livekit::DataPacket& packet,const EncryptionOptions& options);
}
