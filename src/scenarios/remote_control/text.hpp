#pragma once
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

namespace livekit::sdk::detail {
// Strict UTF-8, at most 256 Unicode scalars / 1024 bytes. Newlines and tabs
// are text; other C0/C1 controls are rejected. Never replace malformed input.
inline std::optional<std::u16string> control_text_utf16(std::string_view text) {
  if (text.empty() || text.size() > 1024)
    return {};
  std::u16string out;
  size_t scalars = 0;
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    uint32_t value;
    int extra;
    uint32_t minimum;
    if (first < 0x80) {
      value = first;
      extra = 0;
      minimum = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
      value = first & 0x1f;
      extra = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      value = first & 0xf;
      extra = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      value = first & 7;
      extra = 3;
      minimum = 0x10000;
    } else
      return {};
    while (extra--) {
      if (i == text.size())
        return {};
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80)
        return {};
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
        ++scalars > 256)
      return {};
    if ((value < 0x20 && value != '\n' && value != '\r' && value != '\t') ||
        (value >= 0x7f && value <= 0x9f))
      return {};
    if (value <= 0xffff)
      out.push_back(static_cast<char16_t>(value));
    else {
      value -= 0x10000;
      out.push_back(static_cast<char16_t>(0xd800 + (value >> 10)));
      out.push_back(static_cast<char16_t>(0xdc00 + (value & 0x3ff)));
    }
  }
  return out;
}
}  // namespace livekit::sdk::detail
