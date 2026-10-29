#pragma once
#include "livekit.pb.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace livekit::sdk::detail {
// An absent/foreign codec is not an OFF instruction for the current encoder.
// This single-layer policy does not implement backup-codec REGRESSION or SVC.
inline std::optional<bool> subscribed_video_enabled(const livekit::SubscribedQualityUpdate& update,
                                                    const std::string& current_codec) {
  auto enabled = [](const auto& qualities) -> std::optional<bool> {
    if (qualities.empty())
      return {};
    bool any = false;
    for (const auto& q : qualities) {
      if (q.quality() < 0 || q.quality() > 2)
        return {};  // unknown future enum
      any = any || q.enabled();
    }
    return any;
  };
  if (update.subscribed_codecs().empty())
    return enabled(update.subscribed_qualities());
  std::optional<bool> result;
  for (const auto& codec : update.subscribed_codecs()) {
    auto name = codec.codec();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.starts_with("video/"))
      name.erase(0, 6);
    if (name != current_codec)
      continue;
    const auto state = enabled(codec.qualities());
    if (!state)
      return {};
    result = result.value_or(false) || *state;
  }
  return result;
}
}  // namespace livekit::sdk::detail
