#include "backends/rtc/livekit/packet_encryption.hpp"
#include "livekit/result.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <map>
#include <mutex>

namespace livekit::sdk {
struct ExternalKeyProvider::Impl {
  mutable std::mutex mutex;
  std::map<uint8_t, EncryptionKey> keys;
  uint8_t current = 0;

  ~Impl() {
    for (auto& [index, key] : keys) {
      OPENSSL_cleanse(key.data(), key.size());
    }
  }
};

ExternalKeyProvider::ExternalKeyProvider() : impl_(std::make_unique<Impl>()) {}

ExternalKeyProvider::~ExternalKeyProvider() = default;

Result<void> ExternalKeyProvider::set_key(uint8_t index, std::span<const uint8_t> key) {
  using Return = Result<void>;
  if (key.size() != 16) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "E2EE requires a 16-byte AES key"));
  }
  std::lock_guard lock(impl_->mutex);
  std::copy(key.begin(), key.end(), impl_->keys[index].begin());
  impl_->current = index;
  return Return::success();
}

Result<void> ExternalKeyProvider::set_passphrase(uint8_t index, const std::string& passphrase) {
  using Return = Result<void>;
  if (passphrase.empty() || passphrase.size() > 1024 * 1024) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Invalid E2EE passphrase"));
  }
  EncryptionKey key{};
  constexpr char salt[] = "LKFrameEncryptionKey";
  if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()),
                        reinterpret_cast<const uint8_t*>(salt), sizeof(salt) - 1, 100000,
                        EVP_sha256(), key.size(), key.data()) != 1) {
    OPENSSL_cleanse(key.data(), key.size());
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::processing_failed,
                                        "E2EE key derivation failed"));
  }
  auto stored = set_key(index, key);
  OPENSSL_cleanse(key.data(), key.size());
  return stored;
}

void ExternalKeyProvider::remove_key(uint8_t index) {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->keys.find(index);
  if (it != impl_->keys.end()) {
    OPENSSL_cleanse(it->second.data(), it->second.size());
    impl_->keys.erase(it);
  }
}

uint8_t ExternalKeyProvider::current_key_index() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->current;
}

Result<EncryptionKey> ExternalKeyProvider::key(uint8_t index) const {
  using Return = Result<EncryptionKey>;
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->keys.find(index);
  if (it == impl_->keys.end()) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::permission_denied, "E2EE key unavailable"));
  }
  return Return::success(it->second);
}

namespace {

struct Secret {
  EncryptionKey key;

  ~Secret() {
    OPENSSL_cleanse(key.data(), key.size());
  }
};

using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

Result<size_t> header_bytes(std::span<const uint8_t> data, EncryptionCodec codec) {
  using Return = Result<size_t>;
  if (codec == EncryptionCodec::opus) {
    if (data.empty()) {
      return Return::failure(
          make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Empty Opus frame"));
    }
    return Return::success(1);
  }
  for (size_t i = 0; i + 3 < data.size(); ++i) {
    size_t at = 0;
    if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
        data[i + 3] == 1) {
      at = i + 4;
    } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      at = i + 3;
    }
    if (!at || at >= data.size()) {
      continue;
    }
    const auto type = codec == EncryptionCodec::h264 ? (data[at] & 31) : ((data[at] >> 1) & 63);
    if (codec == EncryptionCodec::h264 ? (type == 1 || type == 5)
                                       : (type <= 9 || (type >= 16 && type <= 21))) {
      return Return::success(std::min(at + 2, data.size()));
    }
  }
  return Return::success(data.size());
}

std::vector<uint8_t> rbsp(std::span<const uint8_t> data, bool escape) {
  std::vector<uint8_t> result;
  result.reserve(data.size() + data.size() / 32);
  if (escape) {
    unsigned zeros = 0;
    for (auto c : data) {
      if (c <= 3 && zeros >= 2) {
        result.push_back(3);
        zeros = 0;
      }
      result.push_back(c);
      zeros = c == 0 ? zeros + 1 : 0;
    }
  } else {
    for (size_t i = 0; i < data.size();) {
      if (i + 2 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
        result.push_back(0);
        result.push_back(0);
        i += 3;
      } else {
        result.push_back(data[i++]);
      }
    }
  }
  return result;
}
}  // namespace

namespace detail {
std::vector<uint8_t> canonical_media_frame(std::span<const uint8_t> frame, EncryptionCodec codec) {
  if (codec == EncryptionCodec::opus) {
    return {frame.begin(), frame.end()};
  }
  std::vector<uint8_t> result;
  result.reserve(frame.size() + 32);
  for (size_t i = 0; i < frame.size();) {
    size_t prefix = 0;
    if (i + 3 < frame.size() && frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 0 &&
        frame[i + 3] == 1) {
      prefix = 4;
    } else if (i + 2 < frame.size() && frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1) {
      prefix = 3;
    }
    if (prefix) {
      result.insert(result.end(), {0, 0, 0, 1});
      i += prefix;
    } else {
      result.push_back(frame[i++]);
    }
  }
  return result;
}

Result<SealedPayload> encrypt_payload(const KeyProvider& keys, std::span<const uint8_t> plaintext,
                                      std::span<const uint8_t> aad) {
  using Return = Result<SealedPayload>;
  if (plaintext.size() > 16 * 1024 * 1024 || aad.size() > 16 * 1024 * 1024) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "E2EE payload limit exceeded"));
  }
  SealedPayload out;
  out.key_index = keys.current_key_index();
  auto fetched_key = keys.key(out.key_index);
  if (!fetched_key) {
    return Return::failure(fetched_key.failure_record());
  }
  Secret key{*fetched_key};
  OPENSSL_cleanse(fetched_key->data(), fetched_key->size());
  if (!(RAND_bytes(out.iv.data(), out.iv.size()) == 1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!(bool(ctx))) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, key.key.data(), out.iv.data()) ==
        1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  int written = 0, tail = 0;
  if (!aad.empty()) {
    if (!(EVP_EncryptUpdate(ctx.get(), nullptr, &written, aad.data(),
                            static_cast<int>(aad.size())) == 1)) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                          "E2EE encryption or authentication failed"));
    }
  }
  out.ciphertext.resize(plaintext.size() + 16);
  if (!(EVP_EncryptUpdate(ctx.get(), out.ciphertext.data(), &written, plaintext.data(),
                          static_cast<int>(plaintext.size())) == 1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(EVP_EncryptFinal_ex(ctx.get(), out.ciphertext.data() + written, &tail) == 1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(size_t(written + tail) == plaintext.size())) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16,
                            out.ciphertext.data() + plaintext.size()) == 1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  return Return::success(out);
}

Result<std::vector<uint8_t>> decrypt_payload(const KeyProvider& keys, uint8_t index,
                                             std::span<const uint8_t> iv,
                                             std::span<const uint8_t> ciphertext,
                                             std::span<const uint8_t> aad) {
  using Return = Result<std::vector<uint8_t>>;
  if (iv.size() != 12 || ciphertext.size() < 16 || ciphertext.size() > 16 * 1024 * 1024 ||
      aad.size() > 16 * 1024 * 1024) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::malformed_message, "Invalid encrypted payload"));
  }
  auto fetched_key = keys.key(index);
  if (!fetched_key) {
    return Return::failure(fetched_key.failure_record());
  }
  Secret key{*fetched_key};
  OPENSSL_cleanse(fetched_key->data(), fetched_key->size());
  Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!(bool(ctx))) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, key.key.data(), iv.data()) ==
        1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  int written = 0, tail = 0;
  if (!aad.empty()) {
    if (!(EVP_DecryptUpdate(ctx.get(), nullptr, &written, aad.data(),
                            static_cast<int>(aad.size())) == 1)) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                          "E2EE encryption or authentication failed"));
    }
  }
  std::vector<uint8_t> out(ciphertext.size());
  if (!(EVP_DecryptUpdate(ctx.get(), out.data(), &written, ciphertext.data(),
                          static_cast<int>(ciphertext.size() - 16)) == 1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  if (!(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                            const_cast<uint8_t*>(ciphertext.data() + ciphertext.size() - 16)) ==
        1)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "E2EE encryption or authentication failed"));
  }
  const auto ok = EVP_DecryptFinal_ex(ctx.get(), out.data() + written, &tail);
  if (ok != 1) {
    OPENSSL_cleanse(out.data(), out.size());
    if (!(false)) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                          "E2EE encryption or authentication failed"));
    }
  }
  out.resize(written + tail);
  return Return::success(out);
}

bool encryptable(const livekit::DataPacket& p) {
  return p.has_user() || p.has_rpc_request() || p.has_rpc_ack() || p.has_rpc_response() ||
         p.has_stream_header() || p.has_stream_chunk() || p.has_stream_trailer();
}

Result<livekit::DataPacket> encrypt_packet(const livekit::DataPacket& p,
                                           const EncryptionOptions& options) {
  using Return = Result<livekit::DataPacket>;
  if (!options.keys || !options.data || !encryptable(p)) {
    return Return::success(p);
  }
  livekit::EncryptedPacketPayload payload;
  if (p.has_user()) {
    *payload.mutable_user() = p.user();
  } else if (p.has_rpc_request()) {
    *payload.mutable_rpc_request() = p.rpc_request();
  } else if (p.has_rpc_ack()) {
    *payload.mutable_rpc_ack() = p.rpc_ack();
  } else if (p.has_rpc_response()) {
    *payload.mutable_rpc_response() = p.rpc_response();
  } else if (p.has_stream_header()) {
    *payload.mutable_stream_header() = p.stream_header();
  } else if (p.has_stream_chunk()) {
    *payload.mutable_stream_chunk() = p.stream_chunk();
  } else {
    *payload.mutable_stream_trailer() = p.stream_trailer();
  }
  const auto plain = payload.SerializeAsString();
  auto encrypted = encrypt_payload(*options.keys,
                                   {reinterpret_cast<const uint8_t*>(plain.data()), plain.size()});
  if (!encrypted) {
    return Return::failure(encrypted.failure_record());
  }
  auto result = p;
  result.clear_value();
  auto* e = result.mutable_encrypted_packet();
  e->set_encryption_type(livekit::Encryption::GCM);
  e->set_key_index(encrypted->key_index);
  e->set_iv(encrypted->iv.data(), encrypted->iv.size());
  e->set_encrypted_value(encrypted->ciphertext.data(), encrypted->ciphertext.size());
  return Return::success(result);
}

Result<livekit::DataPacket> decrypt_packet(const livekit::DataPacket& p,
                                           const EncryptionOptions& options) {
  using Return = Result<livekit::DataPacket>;
  if (!p.has_encrypted_packet()) {
    if (options.keys && options.data && options.require_encrypted && encryptable(p)) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                          "Unencrypted incoming packet rejected"));
    }
    return Return::success(p);
  }
  if (!options.keys || !options.data) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                        "No data decryptor configured"));
  }
  const auto& e = p.encrypted_packet();
  if (e.encryption_type() != livekit::Encryption::GCM || e.key_index() > 255) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::unsupported, "Unsupported packet encryption"));
  }
  auto plain = decrypt_payload(
      *options.keys, uint8_t(e.key_index()),
      {reinterpret_cast<const uint8_t*>(e.iv().data()), e.iv().size()},
      {reinterpret_cast<const uint8_t*>(e.encrypted_value().data()), e.encrypted_value().size()});
  if (!plain) {
    return Return::failure(plain.failure_record());
  }
  livekit::EncryptedPacketPayload payload;
  if (!payload.ParseFromArray(plain->data(), static_cast<int>(plain->size()))) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::malformed_message,
                                        "Invalid encrypted packet payload"));
  }
  auto result = p;
  result.clear_value();
  if (payload.has_user()) {
    *result.mutable_user() = payload.user();
  } else if (payload.has_rpc_request()) {
    *result.mutable_rpc_request() = payload.rpc_request();
  } else if (payload.has_rpc_ack()) {
    *result.mutable_rpc_ack() = payload.rpc_ack();
  } else if (payload.has_rpc_response()) {
    *result.mutable_rpc_response() = payload.rpc_response();
  } else if (payload.has_stream_header()) {
    *result.mutable_stream_header() = payload.stream_header();
  } else if (payload.has_stream_chunk()) {
    *result.mutable_stream_chunk() = payload.stream_chunk();
  } else if (payload.has_stream_trailer()) {
    *result.mutable_stream_trailer() = payload.stream_trailer();
  } else {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::unsupported, "Unknown encrypted payload type"));
  }
  return Return::success(result);
}
}  // namespace detail

FrameCryptor::FrameCryptor(std::shared_ptr<KeyProvider> keys, EncryptionCodec codec)
    : keys_(std::move(keys)), codec_(codec) {}

Result<std::vector<uint8_t>> FrameCryptor::encrypt(std::span<const uint8_t> frame) const {
  using Return = Result<std::vector<uint8_t>>;
  if (frame.size() > 8 * 1024 * 1024) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Encrypted frame limit exceeded"));
  }
  if (!keys_ || (codec_ != EncryptionCodec::opus && codec_ != EncryptionCodec::h264 &&
                 codec_ != EncryptionCodec::h265)) {
    return Return::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                        "Frame cryptor requires keys and a supported codec"));
  }
  const auto header = header_bytes(frame, codec_);
  if (!header) {
    return Return::failure(header.failure_record());
  }
  const auto n = *header;
  if (codec_ != EncryptionCodec::opus && n >= frame.size()) {
    return Return::success(std::vector<uint8_t>(frame.begin(), frame.end()));
  }
  auto encrypted = detail::encrypt_payload(*keys_, frame.subspan(n), frame.first(n));
  if (!encrypted) {
    return Return::failure(encrypted.failure_record());
  }
  auto bytes = std::move(encrypted->ciphertext);
  bytes.insert(bytes.end(), encrypted->iv.begin(), encrypted->iv.end());
  bytes.push_back(12);
  bytes.push_back(encrypted->key_index);
  if (codec_ != EncryptionCodec::opus) {
    bytes = rbsp(bytes, true);
  }
  std::vector<uint8_t> result(frame.begin(), frame.begin() + n);
  result.insert(result.end(), bytes.begin(), bytes.end());
  return Return::success(result);
}

Result<std::optional<std::vector<uint8_t>>> FrameCryptor::decrypt(
    std::span<const uint8_t> frame, std::span<const uint8_t> sif) const {
  using Return = Result<std::optional<std::vector<uint8_t>>>;
  if (!sif.empty() && frame.size() >= sif.size() &&
      std::equal(sif.begin(), sif.end(), frame.end() - sif.size())) {
    return Return::success({});
  }
  if (frame.size() > 16 * 1024 * 1024) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Encrypted frame limit exceeded"));
  }
  if (!keys_ || (codec_ != EncryptionCodec::opus && codec_ != EncryptionCodec::h264 &&
                 codec_ != EncryptionCodec::h265)) {
    return Return::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                        "Frame cryptor requires keys and a supported codec"));
  }
  const auto header = header_bytes(frame, codec_);
  if (!header) {
    return Return::failure(header.failure_record());
  }
  const auto n = *header;
  if (codec_ != EncryptionCodec::opus && n >= frame.size()) {
    return Return::success(std::vector<uint8_t>(frame.begin(), frame.end()));
  }
  auto body = codec_ == EncryptionCodec::opus ? std::vector<uint8_t>(frame.begin() + n, frame.end())
                                              : rbsp(frame.subspan(n), false);
  if (body.size() < 30 || body[body.size() - 2] != 12) {
    return Return::failure(make_failure(ErrorDomain::media, ErrorCode::malformed_message,
                                        "Invalid encrypted media frame"));
  }
  const auto size = body.size() - 14;
  auto plain =
      detail::decrypt_payload(*keys_, body.back(), std::span<const uint8_t>(body).subspan(size, 12),
                              std::span<const uint8_t>(body).first(size), frame.first(n));
  std::vector<uint8_t> result(frame.begin(), frame.begin() + n);
  if (!plain) {
    return Return::failure(plain.failure_record());
  }
  result.insert(result.end(), plain->begin(), plain->end());
  return Return::success(result);
}
}  // namespace livekit::sdk
