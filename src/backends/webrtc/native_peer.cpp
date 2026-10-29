#include "backends/webrtc/native_peer.hpp"
#include "api/make_ref_counted.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace livekit::sdk::rtc::detail {
namespace {
using PC = webrtc::PeerConnectionInterface;

std::exception_ptr failure(webrtc::RTCError error) {
  return std::make_exception_ptr(std::runtime_error(error.message()));
}

template <class T>
struct Completion {
  CompletionSource<T> promise;
  std::atomic<bool> settled{false};

  void reject(std::exception_ptr error) {
    if (!settled.exchange(true))
      promise.set_exception(std::move(error));
  }

  ~Completion() {
    // Closing a PeerConnection may discard an observer without invoking it.
    // Always settle the public operation with a lifecycle error in that case.
    if (!settled.load())
      reject(std::make_exception_ptr(
          std::runtime_error("Native negotiation closed before completion")));
  }
};

class SetRemote : public webrtc::SetSessionDescriptionObserver {
 public:
  Completion<void> completion;

  void OnSuccess() override {
    if (!completion.settled.exchange(true))
      completion.promise.set_value();
  }

  void OnFailure(webrtc::RTCError error) override {
    completion.reject(failure(std::move(error)));
  }
};

class StatisticsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  Completion<TransportStatistics> completion;

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    try {
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
        if (result.json.size() > 1024 * 1024)
          throw std::runtime_error("Native statistics report exceeds 1 MiB");
      }
      result.json += ']';
      if (!completion.settled.exchange(true))
        completion.promise.set_value(std::move(result));
    } catch (...) {
      completion.reject(std::current_exception());
    }
  }
};

class SetLocal : public webrtc::SetSessionDescriptionObserver {
 public:
  SetLocal(std::shared_ptr<Completion<NativeDescription>> completion, NativeDescription description)
      : completion_(std::move(completion)), description_(std::move(description)) {}

  void OnSuccess() override {
    if (!completion_->settled.exchange(true))
      completion_->promise.set_value(std::move(description_));
  }

  void OnFailure(webrtc::RTCError error) override {
    completion_->reject(failure(std::move(error)));
  }

 private:
  std::shared_ptr<Completion<NativeDescription>> completion_;
  NativeDescription description_;
};

class CreateLocal : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateLocal(webrtc::scoped_refptr<PC> pc,
              std::shared_ptr<Completion<NativeDescription>> completion)
      : pc_(std::move(pc)), completion_(std::move(completion)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* value) override {
    std::unique_ptr<webrtc::SessionDescriptionInterface> description(value);
    try {
      NativeDescription result{description->GetType(), {}};
      if (!description->ToString(&result.sdp))
        throw std::runtime_error("Cannot serialize local SDP");
      auto setter = webrtc::make_ref_counted<SetLocal>(completion_, std::move(result));
      pc_->SetLocalDescription(setter.get(), description.release());
    } catch (...) {
      completion_->reject(std::current_exception());
    }
  }

  void OnFailure(webrtc::RTCError error) override {
    completion_->reject(failure(std::move(error)));
  }

 private:
  webrtc::scoped_refptr<PC> pc_;
  std::shared_ptr<Completion<NativeDescription>> completion_;
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
    try {
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
    } catch (...) {
      peer.failed = true;
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
  if (!max_events)
    throw std::invalid_argument("Native peer needs a bounded event queue");
  configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  auto created = runtime.factory->CreatePeerConnectionOrError(
      configuration, webrtc::PeerConnectionDependencies(this));
  if (!created.ok())
    throw std::runtime_error(created.error().message());
  pc = created.MoveValue();
  pc->SetAudioPlayout(false);
  pc->SetAudioRecording(false);
}

NativePeer::~NativePeer() {
  close();
}

AsyncResult<TransportStatistics> NativePeer::statistics() {
  auto observer = webrtc::make_ref_counted<StatisticsObserver>();
  auto result = observer->completion.promise.get_result();
  runtime_.signaling->BlockingCall([&] {
    if (pc)
      pc->GetStats(observer.get());
    else
      observer->completion.reject(
          std::make_exception_ptr(std::runtime_error("Native peer is closed")));
  });
  return result;
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

AsyncResult<NativeDescription> NativePeer::create_local(bool offer, bool ice_restart) {
  auto completion = std::make_shared<Completion<NativeDescription>>();
  auto future = completion->promise.get_result();
  if (!pc) {
    completion->reject(std::make_exception_ptr(std::runtime_error("Native peer is closed")));
    return future;
  }
  for (const auto& transceiver : pc->GetTransceivers()) {
    auto extensions = transceiver->GetHeaderExtensionsToNegotiate();
    for (auto& extension : extensions) {
      if (extension.uri == webrtc::RtpExtension::kAbsoluteCaptureTimeUri) {
        extension.direction = webrtc::RtpTransceiverDirection::kSendRecv;
      }
    }
    const auto error = transceiver->SetHeaderExtensionsToNegotiate(extensions);
    if (!error.ok()) {
      completion->reject(failure(error));
      return future;
    }
  }
  auto observer = webrtc::make_ref_counted<CreateLocal>(pc, completion);
  PC::RTCOfferAnswerOptions options;
  options.ice_restart = ice_restart;
  if (offer)
    pc->CreateOffer(observer.get(), options);
  else
    pc->CreateAnswer(observer.get(), options);
  return future;
}

AsyncResult<void> NativePeer::set_remote(NativeDescription description,
                                         std::map<std::string, std::string> mid_to_track) {
  auto observer = webrtc::make_ref_counted<SetRemote>();
  auto future = observer->completion.promise.get_result();
  webrtc::SdpParseError error;
  auto native = webrtc::CreateSessionDescription(description.type, description.sdp, &error);
  if (!pc || !native) {
    observer->completion.reject(std::make_exception_ptr(std::runtime_error(
        pc ? "Invalid remote SDP: " + error.description : "Native peer is closed")));
    return future;
  }
  runtime_.signaling->BlockingCall([&] { mids_ = std::move(mid_to_track); });
  pc->SetRemoteDescription(observer.get(), native.release());
  return future;
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

void NativePeer::add_ice(const NativeIceCandidate& candidate) {
  if (candidate.sdp.empty())
    return;
  if (!pc)
    throw std::runtime_error("Native peer is closed");
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> native(
      webrtc::CreateIceCandidate(candidate.mid, candidate.mline_index, candidate.sdp, &error));
  if (!native || !pc->AddIceCandidate(native.get()))
    throw std::runtime_error("Native peer rejected ICE candidate");
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
  try {
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
  } catch (...) {
    failed = true;
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
  try {
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
      if (id.starts_with("PA_") && separator != std::string::npos &&
          id.substr(separator + 1).starts_with("TR_"))
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
  } catch (...) {
    failed = true;
    notify();
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

void NativePeer::create_data_channel(const std::string& label, bool reliable) {
  runtime_.signaling->BlockingCall([&] {
    webrtc::DataChannelInit init;
    init.ordered = reliable;
    if (!reliable)
      init.maxRetransmits = 0;
    auto created = pc->CreateDataChannelOrError(label, &init);
    if (!created.ok())
      throw std::runtime_error(created.error().message());
    OnDataChannel(created.MoveValue());
  });
}

bool NativePeer::data_ready(const std::string& label, size_t bytes) {
  if (bytes > 16384)
    return false;
  return runtime_.signaling->BlockingCall([&] {
    for (const auto& channel : channels_)
      if (channel->label == label)
        return channel->native->state() == webrtc::DataChannelInterface::kOpen &&
               channel->native->buffered_amount() <= max_data_bytes - bytes;
    return false;
  });
}

bool NativePeer::send_data(const std::string& label, std::span<const uint8_t> bytes) {
  if (bytes.size() > 16384)
    throw std::invalid_argument("Native data packet exceeds 16 KiB");
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

void NativePeer::request_keyframe(const std::string& sid) {
  webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
  {
    std::lock_guard lock(mutex_);
    const auto found = remote_tracks_.find(sid);
    if (found == remote_tracks_.end() || found->second.kind != MediaKind::video)
      throw std::invalid_argument("Video receiver is not subscribed");
    receiver = found->second.receiver;
  }
  static_cast<webrtc::VideoTrackInterface*>(receiver->track().get())
      ->GetSource()
      ->GenerateKeyFrame();
}
}  // namespace livekit::sdk::rtc::detail
