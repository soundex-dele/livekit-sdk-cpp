#pragma once
#include "livekit/encoded_media.hpp"
#include <filesystem>

namespace livekit::sdk {
// Demux-only file reader. Requires LIVEKIT_RTC_FFMPEG_CODECS. H26x is converted
// from length-prefixed container samples to Annex-B without decoding/encoding.
class EncodedFileReader {
 public:
  explicit EncodedFileReader(const std::filesystem::path& path,bool video=true);
  ~EncodedFileReader();
  EncodedSourceOptions source_options()const;
  std::optional<EncodedMediaFrame> read();
  void rewind();
  // Blocking, stop-aware paced submission. Run on an application worker.
  // Does not close/unpublish the source, and never replays across generations.
  void pump(const std::shared_ptr<EncodedMediaSource>& source,std::stop_token stop={});
 private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
