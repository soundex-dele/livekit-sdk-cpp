#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace livekit::sdk {
struct DataReceiveParams {
  std::string sender_identity, topic;
  bool reliable = false;
};

struct DtmfDigit { uint32_t code = 0; std::string digit; };
struct TranscriptionSegment {
  std::string id, text;
  // Raw protocol time values, preserved without conversion or clock inference.
  uint64_t start_time = 0, end_time = 0;
  bool final = false;
  std::string language;
};
struct Transcription {
  // The subject may differ from the authenticated data sender.
  std::string participant_identity, track_id;
  std::vector<TranscriptionSegment> segments;
};
}
