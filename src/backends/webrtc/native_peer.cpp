#include "livekit/compat/span.hpp"
#include "livekit/compat/algorithm.hpp"
#include "backends/webrtc/native_peer.hpp"
#include "api/make_ref_counted.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include <algorithm>
#include <functional>

namespace livekit::sdk::rtc::detail {
namespace {
using PC = webrtc::PeerConnectionInterface;

Failure failure(webrtc::RTCError error) {
  return make_failure(ErrorDomain::transport, ErrorCode::request_failed, error.message(), false,
                      static_cast<int>(error.type()));
}

template <class T>
struct NativeCompletion {
  Completion<T> callback;
  std::atomic<bool> settled{false};

  void finish(Result<T> result) {
    if (!settled.exchange(true)) {
      auto done = std::move(callback);
      if (done) {
        done(std::move(result));
      }
    }
  }

  void reject(Failure error) {
    finish(Result<T>::failure(std::move(error)));
  }

  ~NativeCompletion() {
    // Closing a PeerConnection may discard an observer without invoking it.
    if (!settled.load()) {
      reject(make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                          "Native operation closed before completion"));
    }
  }
};

class SetRemote : public webrtc::SetSessionDescriptionObserver {
 public:
  NativeCompletion<void> completion;

  void OnSuccess() override {
    completion.finish(Result<void>::success());
  }

  void OnFailure(webrtc::RTCError error) override {
    completion.reject(failure(std::move(error)));
  }
};

class StatisticsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  NativeCompletion<TransportStatistics> completion;

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    {
      TransportStatistics result;
      result.timestamp_us = report->timestamp().us();
      result.json = "[";
      for (const auto& stat : *report) {
        const std::string_view type = stat.type();
        if (type != "outbound-rtp" && type != "inbound-rtp" && type != "remote-inbound-rtp" &&
            type != "media-source" && type != "candidate-pair" && type != "transport")
          continue;
        if (result.json.size() > 1)
          result.json += ',';
        result.json += stat.ToJson();
        if (result.json.size() > 1024 * 1024) {
          completion.reject(make_failure(ErrorDomain::transport, ErrorCode::resource_exhausted,
                                         "Native statistics report exceeds 1 MiB"));
          return;
        }
      }
      result.json += ']';
      completion.finish(Result<TransportStatistics>::success(std::move(result)));
    }
  }
};

class SetLocal : public webrtc::SetSessionDescriptionObserver {
 public:
  SetLocal(std::shared_ptr<NativeCompletion<NativeDescription>> completion,
           NativeDescription description)
      : completion_(std::move(completion)), description_(std::move(description)) {}

  void OnSuccess() override {
    completion_->finish(Result<NativeDescription>::success(std::move(description_)));
  }

  void OnFailure(webrtc::RTCError error) override {
    completion_->reject(failure(std::move(error)));
  }

 private:
  std::shared_ptr<NativeCompletion<NativeDescription>> completion_;
  NativeDescription description_;
};

class CreateLocal : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateLocal(webrtc::scoped_refptr<PC> pc,
              std::shared_ptr<NativeCompletion<NativeDescription>> completion)
      : pc_(std::move(pc)), completion_(std::move(completion)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* value) override {
    std::unique_ptr<webrtc::SessionDescriptionInterface> description(value);
    {
      NativeDescription result{description->GetType(), {}};
      if (!description->ToString(&result.sdp)) {
        completion_->reject(make_failure(ErrorDomain::transport, ErrorCode::processing_failed,
                                         "Cannot serialize local SDP"));
        return;
      }
      auto setter = webrtc::make_ref_counted<SetLocal>(completion_, std::move(result));
      pc_->SetLocalDescription(setter.get(), description.release());
    }
  }

  void OnFailure(webrtc::RTCError error) override {
    completion_->reject(failure(std::move(error)));
  }

 private:
  webrtc::scoped_refptr<PC> pc_;
  std::shared_ptr<NativeCompletion<NativeDescription>> completion_;
};
}  // namespace

struct NativePeer::Channel : webrtc::DataChannelObserver {
  NativePeer& peer;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> native;
  std::string label;

  Channel(NativePeer& peer, webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
      : peer(peer), native(std::move(channel)), label(native->label()) {
    native->RegisterObserver(this);
  }

  ~Channel() override {
    native->UnregisterObserver();
  }

  void OnStateChange() override {
    peer.notify();
  }

  void OnBufferedAmountChange(uint64_t) override {
    peer.notify();
  }

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    {
      if (!buffer.binary || buffer.data.size() > 16384)
        return;
      std::lock_guard lock(peer.mutex_);
      if (peer.closed_)
        return;
      if (peer.data_.size() >= std::min<size_t>(256, peer.max_events_) ||
          buffer.data.size() > max_data_bytes - peer.data_bytes_) {
        // Reliable stream/RPC data cannot be silently truncated.
        if (label == "_reliable") {
          peer.failed = true;
          peer.notify();
        }
        return;
      }
      peer.data_.push_back({label, {buffer.data.begin(), buffer.data.end()}});
      peer.data_bytes_ += buffer.data.size();
      peer.notify();
    }
  }
};

NativePeer::NativePeer(TransportRuntime& runtime, PC::RTCConfiguration configuration,
                       QueueLimits receive_limits, size_t max_events,
                       std::shared_ptr<QueueBudget> budget,
                       std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup)
    : runtime_(runtime),
      receive_limits_(receive_limits),
      budget_(std::move(budget)),
      max_events_(max_events),
      wakeup_(std::move(wakeup)) {
  if (auto initialized = runtime.status(); !initialized) {
    failure_ = initialized.failure_record();
    failed = true;
    return;
  }
  if (!max_events || !receive_limits.max_frames || !receive_limits.max_bytes ||
      receive_limits.max_age.count() <= 0) {
    failure_ = make_failure(ErrorDomain::transport, ErrorCode::invalid_argument,
                            "Native peer needs bounded nonempty receive and event queues");
    failed = true;
    return;
  }
  configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  auto created = runtime.factory->CreatePeerConnectionOrError(
      configuration, webrtc::PeerConnectionDependencies(this));
  if (!created.ok()) {
    failure_ = failure(created.error());
    failed = true;
    return;
  }
  pc = created.MoveValue();
  pc->SetAudioPlayout(false);
  pc->SetAudioRecording(false);
}

NativePeer::~NativePeer() {
  close();
}

void NativePeer::statistics(Completion<TransportStatistics> completion) {
  auto observer = webrtc::make_ref_counted<StatisticsObserver>();
  observer->completion.callback = std::move(completion);
  if (failure_) {
    observer->completion.reject(failure_);
    return;
  }
  runtime_.signaling->BlockingCall([&] {
    if (pc)
      pc->GetStats(observer.get());
    else
      observer->completion.reject(
          make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "Native peer is closed"));
  });
  return;
}

void NativePeer::close() {
  if (!pc)
    return;
  runtime_.signaling->BlockingCall([&] {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    channels_.clear();
    pc->Close();
    pc = nullptr;
    std::lock_guard lock(mutex_);
    for (auto& [sid, track] : remote_tracks_)
      track.output->close();
    remote_tracks_.clear();
    tracks_.clear();
    ice_.clear();
    data_.clear();
    data_bytes_ = 0;
  });
  connected = false;
}

void NativePeer::create_local(bool offer, bool ice_restart,
                              Completion<NativeDescription> callback) {
  auto completion = std::make_shared<NativeCompletion<NativeDescription>>();
  completion->callback = std::move(callback);
  if (!pc) {
    completion->reject(
        make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "Native peer is closed"));
    return;
  }
  for (const auto& transceiver : pc->GetTransceivers()) {
    // WebRTC advertises receive-side FlexFEC even with its sending trial off.
    // Apply the SDK opt-out to answers as well as offers.
    if (!runtime_.flexfec_enabled && transceiver->media_type() == webrtc::MediaType::VIDEO &&
        !transceiver->stopped()) {
      auto codecs = transceiver->codec_preferences();
      if (codecs.empty()) {
        codecs = runtime_.factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO).codecs;
      }
      livekit::compat::erase_if(codecs,
                                [](const auto& codec) { return codec.name == "flexfec-03"; });
      const auto error = transceiver->SetCodecPreferences(codecs);
      if (!error.ok()) {
        completion->reject(failure(error));
        return;
      }
    }
    auto extensions = transceiver->GetHeaderExtensionsToNegotiate();
    for (auto& extension : extensions) {
      if (extension.uri == webrtc::RtpExtension::kAbsoluteCaptureTimeUri) {
        extension.direction = webrtc::RtpTransceiverDirection::kSendRecv;
      }
    }
    const auto error = transceiver->SetHeaderExtensionsToNegotiate(extensions);
    if (!error.ok()) {
      completion->reject(failure(error));
      return;
    }
  }
  auto observer = webrtc::make_ref_counted<CreateLocal>(pc, completion);
  PC::RTCOfferAnswerOptions options;
  options.ice_restart = ice_restart;
  if (offer)
    pc->CreateOffer(observer.get(), options);
  else
    pc->CreateAnswer(observer.get(), options);
  return;
}

void NativePeer::set_remote(NativeDescription description,
                            std::map<std::string, std::string> mid_to_track,
                            Completion<void> completion) {
  auto observer = webrtc::make_ref_counted<SetRemote>();
  observer->completion.callback = std::move(completion);
  webrtc::SdpParseError error;
  auto native = webrtc::CreateSessionDescription(description.type, description.sdp, &error);
  if (!pc || !native) {
    observer->completion.reject(make_failure(
        ErrorDomain::transport, pc ? ErrorCode::malformed_message : ErrorCode::session_closed,
        pc ? "Invalid remote SDP: " + error.description : "Native peer is closed"));
    return;
  }
  runtime_.signaling->BlockingCall([&] { mids_ = std::move(mid_to_track); });
  pc->SetRemoteDescription(observer.get(), native.release());
  return;
}

void NativePeer::refresh_receivers() {
  if (!pc)
    return;
  runtime_.signaling->BlockingCall([&] {
    if (pc->signaling_state() != PC::kStable)
      return;
    for (const auto& transceiver : pc->GetTransceivers()) {
      const auto direction = transceiver->current_direction();
      if (direction && (*direction == webrtc::RtpTransceiverDirection::kRecvOnly ||
                        *direction == webrtc::RtpTransceiverDirection::kSendRecv))
        OnTrack(transceiver);
    }
  });
}

Result<void> NativePeer::add_ice(const NativeIceCandidate& candidate) {
  if (candidate.sdp.empty())
    return Result<void>::success();
  if (!pc)
    return Result<void>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "Native peer is closed"));
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> native(
      webrtc::CreateIceCandidate(candidate.mid, candidate.mline_index, candidate.sdp, &error));
  if (!native || !pc->AddIceCandidate(native.get()))
    return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::malformed_message,
                                              "Native peer rejected ICE candidate"));
  return Result<void>::success();
}

void NativePeer::OnConnectionChange(PC::PeerConnectionState state) {
  connected = state == PC::PeerConnectionState::kConnected;
  if (state == PC::PeerConnectionState::kFailed) {
    failed = true;
  }
  notify();
}

void NativePeer::OnIceGatheringChange(PC::IceGatheringState state) {
  gathered = state == PC::kIceGatheringComplete;
  notify();
}

void NativePeer::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  {
    NativeIceCandidate value{candidate->sdp_mid(), candidate->sdp_mline_index(), {}};
    candidate->ToString(&value.sdp);
    std::lock_guard lock(mutex_);
    if (closed_)
      return;
    if (ice_.size() == max_events_) {
      failed = true;
      notify();
      return;
    }
    ice_.push_back(std::move(value));
    notify();
  }
}

void NativePeer::track_event(bool added, const std::string& sid) {
  if (tracks_.size() == max_events_) {
    failed = true;
    notify();
    return;
  }
  tracks_.push_back({added, sid});
  notify();
}

void NativePeer::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  {
    auto receiver = transceiver->receiver();
    const auto parameters = receiver->GetParameters();
    if (std::none_of(parameters.encodings.begin(), parameters.encodings.end(),
                     [](const auto& encoding) { return encoding.ssrc && *encoding.ssrc; }))
      return;
    std::string sid = receiver->track()->id();
    const auto mid = transceiver->mid().value_or("");
    if (auto mapped = mids_.find(mid); mapped != mids_.end())
      sid = mapped->second;
    for (const auto& stream : receiver->streams()) {
      const auto id = stream->id();
      const auto separator = id.find('|');
      if (livekit::compat::starts_with(id, "PA_") && separator != std::string::npos &&
          livekit::compat::starts_with(id.substr(separator + 1), "TR_"))
        sid = id.substr(separator + 1);
    }
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return;
      if (auto found = remote_tracks_.find(sid);
          found != remote_tracks_.end() && found->second.receiver == receiver)
        return;
    }
    OnRemoveTrack(receiver);
    const auto kind = receiver->track()->kind() == "video" ? MediaKind::video : MediaKind::audio;
    auto output = webrtc::make_ref_counted<EncodedReceiver>(
        kind, receive_limits_, budget_,
        receiver_transform_ ? receiver_transform_(sid, kind) : EncodedReceiver::PayloadTransform{});
    receiver->SetDepacketizerToDecoderFrameTransformer(output);
    std::lock_guard lock(mutex_);
    if (auto old = remote_tracks_.find(sid); old != remote_tracks_.end())
      old->second.output->close();
    remote_tracks_.insert_or_assign(sid, NativeRemoteTrack{sid, mid, kind, receiver, output});
    track_event(true, sid);
  }
}

void NativePeer::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  std::lock_guard lock(mutex_);
  for (auto it = remote_tracks_.begin(); it != remote_tracks_.end();) {
    if (it->second.receiver != receiver) {
      ++it;
      continue;
    }
    it->second.output->close();
    if (!closed_)
      track_event(false, it->first);
    it = remote_tracks_.erase(it);
  }
}

void NativePeer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  const auto label = channel->label();
  if ((label != "_reliable" && label != "_lossy" && label != "_data_track") ||
      channels_.size() >= 3 ||
      std::any_of(channels_.begin(), channels_.end(),
                  [&](const auto& current) { return current->label == label; })) {
    channel->Close();
    return;
  }
  channels_.push_back(std::make_unique<Channel>(*this, std::move(channel)));
  notify();
}

Result<void> NativePeer::create_data_channel(const std::string& label, bool reliable) {
  if (!pc) {
    return Result<void>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "Native peer is closed"));
  }
  auto result = Result<void>::success();
  runtime_.signaling->BlockingCall([&] {
    webrtc::DataChannelInit init;
    init.ordered = reliable;
    if (!reliable)
      init.maxRetransmits = 0;
    auto created = pc->CreateDataChannelOrError(label, &init);
    if (!created.ok()) {
      result = Result<void>::failure(failure(created.error()));
      return;
    }
    OnDataChannel(created.MoveValue());
  });
  return result;
}

bool NativePeer::data_ready(const std::string& label, size_t bytes) {
  if (!pc || bytes > 16384)
    return false;
  return runtime_.signaling->BlockingCall([&] {
    for (const auto& channel : channels_)
      if (channel->label == label)
        return channel->native->state() == webrtc::DataChannelInterface::kOpen &&
               channel->native->buffered_amount() <= max_data_bytes - bytes;
    return false;
  });
}

bool NativePeer::send_data(const std::string& label, livekit::compat::span<const uint8_t> bytes) {
  if (!pc || bytes.size() > 16384)
    return false;
  return runtime_.signaling->BlockingCall([&] {
    for (const auto& channel : channels_) {
      if (channel->label != label)
        continue;
      const auto& native = channel->native;
      if (native->state() != webrtc::DataChannelInterface::kOpen ||
          native->buffered_amount() > max_data_bytes - bytes.size())
        return false;
      return native->Send(
          webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true));
    }
    return false;
  });
}

std::vector<NativeDataMessage> NativePeer::take_data(size_t max_messages) {
  std::lock_guard lock(mutex_);
  std::vector<NativeDataMessage> result;
  while (!data_.empty() && result.size() < max_messages) {
    data_bytes_ -= data_.front().bytes.size();
    result.push_back(std::move(data_.front()));
    data_.pop_front();
  }
  if (!data_.empty()) {
    notify();
  }
  return result;
}

std::vector<NativeIceCandidate> NativePeer::take_ice() {
  std::lock_guard lock(mutex_);
  std::vector<NativeIceCandidate> result;
  while (!ice_.empty()) {
    result.push_back(std::move(ice_.front()));
    ice_.pop_front();
  }
  return result;
}

std::vector<NativeTrackEvent> NativePeer::take_track_events() {
  std::lock_guard lock(mutex_);
  std::vector<NativeTrackEvent> result;
  while (!tracks_.empty()) {
    result.push_back(std::move(tracks_.front()));
    tracks_.pop_front();
  }
  return result;
}

std::map<std::string, NativeRemoteTrack> NativePeer::remote_tracks() const {
  std::lock_guard lock(mutex_);
  return remote_tracks_;
}

void NativePeer::set_receiver_transform(ReceiverTransformFactory factory) {
  runtime_.signaling->BlockingCall([&] { receiver_transform_ = std::move(factory); });
}

void NativePeer::reset_receiver(const std::string& sid) {
  runtime_.signaling->BlockingCall([&] {
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
    webrtc::scoped_refptr<EncodedReceiver> output;
    {
      std::lock_guard lock(mutex_);
      auto found = remote_tracks_.find(sid);
      if (found == remote_tracks_.end())
        return;
      found->second.output->close();
      output = webrtc::make_ref_counted<EncodedReceiver>(
          found->second.kind, receive_limits_, budget_,
          receiver_transform_ ? receiver_transform_(sid, found->second.kind)
                              : EncodedReceiver::PayloadTransform{});
      found->second.output = output;
      receiver = found->second.receiver;
    }
    receiver->SetDepacketizerToDecoderFrameTransformer(output);
  });
}

Result<void> NativePeer::request_keyframe(const std::string& sid) {
  webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
  {
    std::lock_guard lock(mutex_);
    const auto found = remote_tracks_.find(sid);
    if (found == remote_tracks_.end() || found->second.kind != MediaKind::video)
      return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::invalid_state,
                                                "Video receiver is not subscribed"));
    receiver = found->second.receiver;
  }
  static_cast<webrtc::VideoTrackInterface*>(receiver->track().get())
      ->GetSource()
      ->GenerateKeyFrame();
  return Result<void>::success();
}
}  // namespace livekit::sdk::rtc::detail
