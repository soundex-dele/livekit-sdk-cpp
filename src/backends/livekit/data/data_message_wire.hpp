#pragma once
#include "livekit/data_messages.hpp"
#include "support/utf8.hpp"
#include "livekit/json.hpp"
#include "livekit.pb.h"
#include "livekit/result.hpp"
#include <string_view>

namespace livekit::sdk::client::detail {
using ::livekit::sdk::detail::valid_utf8;

inline Result<void> validate_dtmf(const DtmfDigit& v) {
  constexpr std::string_view digits = "0123456789*#ABCD";
  if (v.code >= digits.size() || v.digit.size() != 1 || v.digit[0] != digits[v.code]) {
    return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                              "DTMF code/digit must match RFC 4733 events 0..15"));
  }
  return Result<void>::success();
}

inline Result<void> validate_transcription(const Transcription& v) {
  if (v.participant_identity.empty() || v.participant_identity.size() > 1024 ||
      !valid_utf8(v.participant_identity) || v.track_id.empty() || v.track_id.size() > 256 ||
      !valid_utf8(v.track_id) || v.segments.empty() || v.segments.size() > 64) {
    return Result<void>::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                     "Invalid transcription identity, track or segment count"));
  }
  size_t text_bytes = 0;
  for (const auto& s : v.segments) {
    if (s.id.empty() || s.id.size() > 256 || !valid_utf8(s.id) || s.language.size() > 64 ||
        !valid_utf8(s.language) || s.end_time < s.start_time || !valid_utf8(s.text) ||
        s.text.size() > 12000) {
      return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                "Invalid transcription segment"));
    }
    text_bytes += s.text.size();
    if (text_bytes > 12000) {
      return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                "Transcription text limit exceeded"));
    }
  }
  return Result<void>::success();
}

inline Result<livekit::DataPacket> encode_dtmf(const DtmfDigit& v) {
  if (auto valid = validate_dtmf(v); !valid) {
    return Result<livekit::DataPacket>::failure(valid.failure_record());
  }
  livekit::DataPacket packet;
  packet.mutable_sip_dtmf()->set_code(v.code);
  packet.mutable_sip_dtmf()->set_digit(v.digit);
  return Result<livekit::DataPacket>::success(std::move(packet));
}

inline Result<livekit::DataPacket> encode_transcription(const Transcription& v) {
  if (auto valid = validate_transcription(v); !valid) {
    return Result<livekit::DataPacket>::failure(valid.failure_record());
  }
  livekit::DataPacket packet;
  auto* t = packet.mutable_transcription();
  t->set_transcribed_participant_identity(v.participant_identity);
  t->set_track_id(v.track_id);
  for (const auto& s : v.segments) {
    auto* segment = t->add_segments();
    segment->set_id(s.id);
    segment->set_text(s.text);
    segment->set_start_time(s.start_time);
    segment->set_end_time(s.end_time);
    segment->set_final(s.final);
    segment->set_language(s.language);
  }
  if (packet.ByteSizeLong() > 15000) {
    return Result<livekit::DataPacket>::failure(make_failure(
        ErrorDomain::data, ErrorCode::invalid_argument, "Transcription packet limit exceeded"));
  }
  return Result<livekit::DataPacket>::success(std::move(packet));
}

inline Result<Json> transcription_json(const livekit::Transcription& v) {
  if (v.segments_size() > 64) {
    return Result<Json>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                              "Transcription segment limit exceeded"));
  }
  Json segments = Json::array();
  for (const auto& s : v.segments()) {
    segments.push_back({{"id", s.id()},
                        {"text", s.text()},
                        {"start", s.start_time()},
                        {"end", s.end_time()},
                        {"final", s.final()},
                        {"language", s.language()}});
  }
  return Result<Json>::success({{"participant", v.transcribed_participant_identity()},
                                {"track", v.track_id()},
                                {"segments", segments}});
}

inline Result<Transcription> parse_transcription(const Json& v) {
  auto invalid = [] {
    return Result<Transcription>::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Invalid transcription JSON"));
  };
  if (!v.is_object() || !v.contains("participant") || !v["participant"].is_string() ||
      !v.contains("track") || !v["track"].is_string() || !v.contains("segments") ||
      !v["segments"].is_array() || v["segments"].size() > 64) {
    return invalid();
  }
  Transcription t{v["participant"].get<std::string>(), v["track"].get<std::string>()};
  for (const auto& s : v["segments"]) {
    if (!s.is_object()) {
      return invalid();
    }
    for (const auto* field : {"id", "text", "language"}) {
      if (!s.contains(field) || !s[field].is_string()) {
        return invalid();
      }
    }
    if (!s.contains("start") || !s["start"].is_number_integer() || !s.contains("end") ||
        !s["end"].is_number_integer() || s["start"] < 0 || s["end"] < 0 || !s.contains("final") ||
        !s["final"].is_boolean()) {
      return invalid();
    }
    t.segments.push_back({s["id"], s["text"], s["start"], s["end"], s["final"], s["language"]});
  }
  if (auto valid = validate_transcription(t); !valid) {
    return Result<Transcription>::failure(valid.failure_record());
  }
  return Result<Transcription>::success(std::move(t));
}
}  // namespace livekit::sdk::client::detail
