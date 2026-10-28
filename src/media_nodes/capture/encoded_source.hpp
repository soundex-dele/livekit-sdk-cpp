#pragma once
#include "livekit/encoded_media.hpp"

namespace livekit::sdk::detail {
class EncodedSourceReader {
 public:
  explicit EncodedSourceReader(std::shared_ptr<EncodedMediaSource> source);~EncodedSourceReader();
  void start();void stop();
  std::optional<EncodedMediaFrame> read(std::stop_token stop,bool wait=true);
  void feedback(bool keyframe,uint32_t bitrate=0);
  EncodedSourceOptions options()const{return source_->options();}
 private:std::shared_ptr<EncodedMediaSource> source_;uint64_t lease_=0;
};
class EncodedOutput {
 public:
  explicit EncodedOutput(std::shared_ptr<EncodedMediaStream> stream);~EncodedOutput();
  uint64_t advance();
  uint64_t generation()const;
  void push(EncodedMediaFrame frame,uint64_t generation);
  void close();
 private:std::shared_ptr<EncodedMediaStream> stream_;
};
}
