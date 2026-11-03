#pragma once
#include "livekit/compat/span.hpp"
#include <zlib.h>
#include <array>
#include "livekit/result.hpp"
#include <string>

namespace livekit::sdk::client::detail {
class InflateRaw {
 public:
  InflateRaw() : initialized_(inflateInit2(&state_, -MAX_WBITS) == Z_OK) {}

  ~InflateRaw() {
    if (initialized_) {
      inflateEnd(&state_);
    }
  }

  InflateRaw(const InflateRaw&) = delete;

  Result<std::string> append(livekit::compat::span<const uint8_t> input, size_t limit,
                             bool final = false) {
    if (!initialized_) {
      return Result<std::string>::failure(make_failure(
          ErrorDomain::data, ErrorCode::processing_failed, "Cannot initialize DEFLATE"));
    }
    if (input.size() > 1024 * 1024 || (ended_ && !input.empty())) {
      return Result<std::string>::failure(make_failure(
          ErrorDomain::data, ErrorCode::malformed_message, "Invalid compressed payload"));
    }
    if (ended_) {
      return Result<std::string>::success({});
    }
    state_.next_in = const_cast<Bytef*>(input.data());
    state_.avail_in = static_cast<uInt>(input.size());
    std::string output;
    std::array<char, 8192> buffer;
    for (;;) {
      state_.next_out = reinterpret_cast<Bytef*>(buffer.data());
      state_.avail_out = static_cast<uInt>(buffer.size());
      const auto result = inflate(&state_, final ? Z_FINISH : Z_NO_FLUSH);
      const auto bytes = buffer.size() - state_.avail_out;
      if (bytes > limit - output.size()) {
        return Result<std::string>::failure(make_failure(ErrorDomain::data,
                                                         ErrorCode::resource_exhausted,
                                                         "Decompressed payload exceeds limit"));
      }
      output.append(buffer.data(), bytes);
      if (result == Z_STREAM_END) {
        ended_ = true;
        if (state_.avail_in) {
          return Result<std::string>::failure(make_failure(
              ErrorDomain::data, ErrorCode::malformed_message, "Trailing compressed data"));
        }
        break;
      }
      if (result != Z_OK && result != Z_BUF_ERROR) {
        return Result<std::string>::failure(make_failure(
            ErrorDomain::data, ErrorCode::malformed_message, "Malformed DEFLATE data"));
      }
      if (state_.avail_in == 0 && state_.avail_out) {
        if (final) {
          return Result<std::string>::failure(make_failure(
              ErrorDomain::data, ErrorCode::malformed_message, "Truncated DEFLATE data"));
        }
        break;
      }
      if (result == Z_BUF_ERROR && bytes == 0) {
        return Result<std::string>::failure(make_failure(
            ErrorDomain::data, ErrorCode::malformed_message, "Stalled DEFLATE stream"));
      }
    }
    return Result<std::string>::success(std::move(output));
  }

 private:
  z_stream state_{};
  bool ended_ = false;
  bool initialized_ = false;
};

inline Result<std::string> inflate_raw(const std::string& input, size_t limit) {
  InflateRaw inflater;
  return inflater.append({reinterpret_cast<const uint8_t*>(input.data()), input.size()}, limit,
                         true);
}

inline Result<std::string> deflate_raw(const std::string& input) {
  if (input.size() > 1024 * 1024) {
    return Result<std::string>::failure(
        make_failure(ErrorDomain::data, ErrorCode::resource_exhausted, "Compression input limit"));
  }
  z_stream state{};
  if (deflateInit2(&state, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    return Result<std::string>::failure(make_failure(
        ErrorDomain::data, ErrorCode::processing_failed, "Cannot initialize DEFLATE encoder"));
  }

  struct End {
    z_stream* state;

    ~End() {
      deflateEnd(state);
    }
  } end{&state};

  std::string output(deflateBound(&state, static_cast<uLong>(input.size())), 0);
  state.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  state.avail_in = static_cast<uInt>(input.size());
  state.next_out = reinterpret_cast<Bytef*>(output.data());
  state.avail_out = static_cast<uInt>(output.size());
  if (deflate(&state, Z_FINISH) != Z_STREAM_END) {
    return Result<std::string>::failure(
        make_failure(ErrorDomain::data, ErrorCode::processing_failed, "DEFLATE failed"));
  }
  output.resize(state.total_out);
  return Result<std::string>::success(std::move(output));
}
}  // namespace livekit::sdk::client::detail
