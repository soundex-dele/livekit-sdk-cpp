#pragma once
#include "livekit/data_messages.hpp"
#include "support/utf8.hpp"
#include "livekit/json.hpp"
#include "livekit.pb.h"
#include <stdexcept>
#include <string_view>

namespace livekit::sdk::detail {
inline void validate_dtmf(const DtmfDigit& v) {
  constexpr std::string_view digits="0123456789*#ABCD";
  if (v.code>=digits.size() || v.digit.size()!=1 || v.digit[0]!=digits[v.code])
    throw std::invalid_argument("DTMF code/digit must match RFC 4733 events 0..15");
}
inline void validate_transcription(const Transcription& v) {
  if (v.participant_identity.empty() || v.participant_identity.size()>1024 || !valid_utf8(v.participant_identity) ||
      v.track_id.empty() || v.track_id.size()>256 || !valid_utf8(v.track_id) || v.segments.empty() || v.segments.size()>64)
    throw std::invalid_argument("Invalid transcription identity, track or segment count");
  size_t text_bytes=0;
  for (const auto& s:v.segments) {
    if (s.id.empty() || s.id.size()>256 || !valid_utf8(s.id) || s.language.size()>64 || !valid_utf8(s.language) ||
        s.end_time<s.start_time || !valid_utf8(s.text) || s.text.size()>12000)
      throw std::invalid_argument("Invalid transcription segment");
    text_bytes+=s.text.size();
    if (text_bytes>12000) throw std::invalid_argument("Transcription text limit exceeded");
  }
}
inline livekit::DataPacket encode_dtmf(const DtmfDigit& v) {
  validate_dtmf(v); livekit::DataPacket packet;
  packet.mutable_sip_dtmf()->set_code(v.code); packet.mutable_sip_dtmf()->set_digit(v.digit); return packet;
}
inline livekit::DataPacket encode_transcription(const Transcription& v) {
  validate_transcription(v); livekit::DataPacket packet; auto* t=packet.mutable_transcription();
  t->set_transcribed_participant_identity(v.participant_identity); t->set_track_id(v.track_id);
  for (const auto& s:v.segments) {
    auto* segment=t->add_segments(); segment->set_id(s.id); segment->set_text(s.text);
    segment->set_start_time(s.start_time); segment->set_end_time(s.end_time); segment->set_final(s.final); segment->set_language(s.language);
  }
  if (packet.ByteSizeLong()>15000) throw std::invalid_argument("Transcription packet limit exceeded");
  return packet;
}
inline Json transcription_json(const livekit::Transcription& v) {
  if(v.segments_size()>64)throw std::invalid_argument("Transcription segment limit exceeded");
  Json segments=Json::array();
  for (const auto& s:v.segments()) segments.push_back({{"id",s.id()},{"text",s.text()},{"start",s.start_time()},{"end",s.end_time()},{"final",s.final()},{"language",s.language()}});
  return {{"participant",v.transcribed_participant_identity()},{"track",v.track_id()},{"segments",segments}};
}
inline Transcription parse_transcription(const Json& v) {
  Transcription t{v.at("participant").get<std::string>(),v.at("track").get<std::string>()};
  for (const auto& s:v.at("segments")) t.segments.push_back({s.at("id"),s.at("text"),s.at("start"),s.at("end"),s.at("final"),s.at("language")});
  validate_transcription(t); return t;
}
}
