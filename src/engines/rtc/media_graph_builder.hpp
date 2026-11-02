#pragma once
#include "engines/rtc/media_graph_contract.hpp"
#include "engines/rtc/media_resources.hpp"

namespace livekit::sdk::rtc::engine {
class RtcMediaGraphBuilder final : public MediaGraphBuilder {
 public:
  explicit RtcMediaGraphBuilder(RtcMediaResources& resources) : resources_(resources) {}

  Result<PreparedMediaGraph> build_publish(MediaPublishSpec,
                                           std::shared_ptr<nodes::RtcPublishNode>) override;
  Result<PreparedMediaGraph> build_subscribe(MediaSubscribeSpec,
                                             std::shared_ptr<nodes::RtcSubscribeNode>) override;
  Result<std::shared_ptr<SharedMediaInput>> create_shared_input(TrackDescriptor,
                                                                MediaInput) override;

 private:
  RtcMediaResources& resources_;
};
}  // namespace livekit::sdk::rtc::engine
