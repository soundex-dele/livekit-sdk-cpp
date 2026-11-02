#pragma once
#include "service_test_result.hpp"
#include "backends/rtc/livekit/data_message_wire.hpp"

inline void data_messages_contract() {
  using namespace livekit::sdk;
  auto check = [](bool ok) {
    if (!ok) {
      std::cerr << "Data message contract failed\n";
      std::abort();
    }
  };
  auto reject = [&](auto fn) { check(!fn()); };
  for (uint32_t i = 0; i < 16; ++i) {
    auto p = service_value(detail::encode_dtmf({i, std::string(1, "0123456789*#ABCD"[i])}));
    livekit::DataPacket decoded;
    check(decoded.ParseFromString(p.SerializeAsString()));
    check(decoded.has_sip_dtmf() && decoded.sip_dtmf().code() == i && !decoded.has_user());
  }
  reject([] { return detail::encode_dtmf({16, "!"}); });
  reject([] { return detail::encode_dtmf({11, "*"}); });
  Transcription value{"subject", "TR_test", {{"segment", "你好 🌍", 123, 456, false, "zh"}}};
  auto packet = service_value(detail::encode_transcription(value));
  auto roundtrip = service_value(detail::parse_transcription(
      service_value(detail::transcription_json(packet.transcription()))));
  check(roundtrip.segments[0].text == value.segments[0].text &&
        roundtrip.segments[0].end_time == 456);
  check(detail::valid_utf8(value.segments[0].text));
  for (const auto& invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                              std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8")}) {
    check(!detail::valid_utf8(invalid));
  }
  value.segments[0].end_time = 1;
  reject([&] { return detail::encode_transcription(value); });
  value.segments[0].end_time = 456;
  value.segments[0].text = std::string(12001, 'x');
  reject([&] { return detail::encode_transcription(value); });
}
