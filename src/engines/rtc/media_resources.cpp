#include "engines/rtc/media_resources.hpp"

namespace livekit::sdk::rtc::engine {
RtcMediaResources::RtcMediaResources(AudioOutputOptions options)
    : echo_reference(options.echo_reference ? options.echo_reference
                                            : std::make_shared<AudioRenderReference>()),
      output(std::make_shared<AudioOutputSession>([&] {
        options.echo_reference = echo_reference;
        return options;
      }())) {}
}  // namespace livekit::sdk::rtc::engine
