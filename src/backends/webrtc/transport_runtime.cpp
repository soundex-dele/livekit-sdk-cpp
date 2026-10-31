#include "backends/webrtc/transport_runtime.hpp"
#include "backends/webrtc/encoded_video_sender.hpp"
#include "api/enable_media.h"
#include "api/field_trials.h"
#include "api/audio/audio_device.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/sctp/dcsctp_transport.h"
#include "rtc_base/ssl_adapter.h"

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

TransportRuntime::TransportRuntime(bool enable_flexfec,
                                   webrtc::PeerConnectionFactoryDependencies dependencies)
    : flexfec_enabled(enable_flexfec) {
#ifdef _WIN32
  if (winsock_.error()) {
    failure_ = make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                            "Winsock initialization failed");
    cleanup();
    return;
  }
#endif
  if (!webrtc::InitializeSSL()) {
    failure_ = make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                            "WebRTC SSL initialization failed");
    cleanup();
    return;
  }
  ssl_ = true;
  {
    auto deps = std::move(dependencies);
    network = webrtc::Thread::CreateWithSocketServer();
    worker = webrtc::Thread::Create();
    signaling = webrtc::Thread::Create();
    if (!network || !worker || !signaling || !network->Start() || !worker->Start() ||
        !signaling->Start()) {
      failure_ = make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                              "Cannot start native transport threads");
      cleanup();
      return;
    }
    // Per-factory trials avoid changing other SDK sessions or global WebRTC state.
    deps.trials = webrtc::FieldTrials::CreateNoGlobal(
        enable_flexfec ? "WebRTC-FlexFEC-03/Enabled/WebRTC-FlexFEC-03-Advertised/Enabled/"
                       : "WebRTC-FlexFEC-03/Disabled/WebRTC-FlexFEC-03-Advertised/Disabled/");
    deps.network_thread = network.get();
    deps.worker_thread = worker.get();
    deps.signaling_thread = signaling.get();
    deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
    deps.sctp_factory = std::make_unique<SctpFactory>(network.get());
    deps.adm = worker->BlockingCall([&] {
      return webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kDummyAudio,
                                               deps.task_queue_factory.get());
    });
    if (!deps.adm) {
      failure_ = make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                              "Cannot create transport audio module");
      cleanup();
      return;
    }
    install_transport_audio_codecs(deps, codec_counters);
    deps.video_encoder_factory = create_transport_video_encoder_factory();
    deps.video_decoder_factory = create_transport_video_decoder_factory(codec_counters);
    // EnableMedia preserves our explicit boundaries and does not install the
    // built-in audio processing or codec factories.
    webrtc::EnableMedia(deps);
    factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
    if (!factory) {
      failure_ = make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                              "Cannot create native transport factory");
      cleanup();
      return;
    }
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.network_ignore_mask = 0;
    factory->SetOptions(options);
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
