#include "backends/webrtc/transport_runtime.hpp"
#include "backends/webrtc/encoded_video_sender.hpp"
#include "api/enable_media.h"
#include "api/audio/audio_device.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/sctp/dcsctp_transport.h"
#include "rtc_base/ssl_adapter.h"
#include <stdexcept>

namespace livekit::sdk::rtc::detail {
namespace {
class SctpFactory : public webrtc::SctpTransportFactoryInterface {
 public:
  explicit SctpFactory(webrtc::Thread* network) : network_(network) {}

  std::unique_ptr<webrtc::SctpTransportInternal> CreateSctpTransport(
      const webrtc::Environment& env, webrtc::DtlsTransportInternal* dtls) override {
    return std::make_unique<webrtc::DcSctpTransport>(env, network_, dtls);
  }

 private:
  webrtc::Thread* network_;
};
}  // namespace

TransportRuntime::TransportRuntime() {
#ifdef _WIN32
  if (winsock_.error())
    throw std::runtime_error("Winsock initialization failed");
#endif
  if (!webrtc::InitializeSSL())
    throw std::runtime_error("WebRTC SSL initialization failed");
  ssl_ = true;
  try {
    network = webrtc::Thread::CreateWithSocketServer();
    worker = webrtc::Thread::Create();
    signaling = webrtc::Thread::Create();
    if (!network->Start() || !worker->Start() || !signaling->Start())
      throw std::runtime_error("Cannot start native transport threads");
    webrtc::PeerConnectionFactoryDependencies deps;
    deps.network_thread = network.get();
    deps.worker_thread = worker.get();
    deps.signaling_thread = signaling.get();
    deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
    deps.sctp_factory = std::make_unique<SctpFactory>(network.get());
    deps.adm = worker->BlockingCall([&] {
      return webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kDummyAudio,
                                               deps.task_queue_factory.get());
    });
    if (!deps.adm)
      throw std::runtime_error("Cannot create transport audio module");
    install_transport_audio_codecs(deps, codec_counters);
    deps.video_encoder_factory = create_transport_video_encoder_factory();
    deps.video_decoder_factory = create_transport_video_decoder_factory(codec_counters);
    // EnableMedia preserves our explicit boundaries and does not install the
    // built-in audio processing or codec factories.
    webrtc::EnableMedia(deps);
    factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
    if (!factory)
      throw std::runtime_error("Cannot create native transport factory");
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.network_ignore_mask = 0;
    factory->SetOptions(options);
  } catch (...) {
    cleanup();
    throw;
  }
}

TransportRuntime::~TransportRuntime() {
  cleanup();
}

void TransportRuntime::cleanup() {
  if (factory)
    signaling->BlockingCall([&] { factory = nullptr; });
  if (signaling)
    signaling->Stop();
  if (worker)
    worker->Stop();
  if (network)
    network->Stop();
  if (ssl_) {
    webrtc::CleanupSSL();
    ssl_ = false;
  }
}
}  // namespace livekit::sdk::rtc::detail
