#pragma once
#include <cstddef>
#include <string_view>

namespace livekit::sdk::detail {
inline bool valid_utf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80)
      continue;
    unsigned value = 0, remaining = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      value = first & 0x1f;
      remaining = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      value = first & 0xf;
      remaining = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      value = first & 7;
      remaining = 3;
      minimum = 0x10000;
    } else
      return false;
    if (remaining > text.size() - i)
      return false;
    while (remaining--) {
      auto c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (c & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}
}  // namespace livekit::sdk::detail
