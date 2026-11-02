#include "backends/livekit/peer_transport.hpp"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace livekit::sdk::client::detail {
namespace {
Failure native_error(const webrtc::RTCError& error) {
  return make_failure(ErrorDomain::transport, ErrorCode::request_failed, error.message());
}

template <class T>
struct Answer {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<Result<T>> result;

  void finish(Result<T> value) {
    std::lock_guard lock(mutex);
    result.emplace(std::move(value));
    changed.notify_all();
  }

  Result<T> wait(const Operation& op) {
    std::unique_lock lock(mutex);
    while (!result) {
      if (auto error = op.status(); !error) {
        return Result<T>::failure(error.failure_record());
      }
      changed.wait_for(lock, std::chrono::milliseconds(5));
    }
    return std::move(*result);
  }
};

class SetDescription : public webrtc::SetSessionDescriptionObserver {
 public:
  explicit SetDescription(std::shared_ptr<Answer<void>> answer) : answer_(std::move(answer)) {}

  void OnSuccess() override {
    answer_->finish(Result<void>::success());
  }

  void OnFailure(webrtc::RTCError e) override {
    answer_->finish(Result<void>::failure(native_error(e)));
  }

 private:
  std::shared_ptr<Answer<void>> answer_;
};

class CreateDescription : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateDescription(
      std::shared_ptr<Answer<std::unique_ptr<webrtc::SessionDescriptionInterface>>> answer)
      : answer_(std::move(answer)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* sdp) override {
    answer_->finish(Result<std::unique_ptr<webrtc::SessionDescriptionInterface>>::success(
        std::unique_ptr<webrtc::SessionDescriptionInterface>(sdp)));
  }

  void OnFailure(webrtc::RTCError e) override {
    answer_->finish(
        Result<std::unique_ptr<webrtc::SessionDescriptionInterface>>::failure(native_error(e)));
  }

 private:
  std::shared_ptr<Answer<std::unique_ptr<webrtc::SessionDescriptionInterface>>> answer_;
};
}  // namespace

// Terminal transformer: native codecs never see received encoded media.
class PeerTransport::Receiver : public webrtc::FrameTransformerInterface {
 public:
  std::mutex mutex;
  std::weak_ptr<TrackData> track;
  std::optional<uint32_t> previous;
  uint64_t timestamp = 0;

  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> native) override {
    std::lock_guard lock(mutex);
    auto target = track.lock();
    if (!target || !native) {
      return;
    }
    const auto bytes = native->GetData();
    if (bytes.empty()) {
      return;
    }
    auto payload = target->budget->copy({bytes.data(), bytes.size()});
    if (!payload) {
      std::lock_guard lock(target->mutex);
      target->frames.clear();
      target->needs_keyframe = true;
      ++target->info.dropped;
      return;
    }
    auto current = native->GetTimestamp();
    uint32_t ticks = 0;
    if (previous) {
      int32_t delta = int32_t(current - *previous);
      if (delta < 0) {
        return;
      }
      timestamp += uint32_t(delta);
      ticks = uint32_t(delta);
    } else {
      timestamp = current;
    }
    previous = current;
    EncodedFrame frame;
    frame.buffer = std::move(*payload);
    frame.timestamp = timestamp;
    if (auto* video = dynamic_cast<webrtc::TransformableVideoFrameInterface*>(native.get())) {
      frame.keyframe = video->IsKeyFrame();
      frame.duration = std::chrono::microseconds(uint64_t(ticks) * 1000000 / 90000);
    }
    if (auto* audio = dynamic_cast<webrtc::TransformableAudioFrameInterface*>(native.get())) {
      frame.sequence_number = audio->SequenceNumber();
      const uint8_t toc = bytes[0];
      const int samples = (toc & 0x80)            ? (48000 << ((toc >> 3) & 3)) / 400
                          : (toc & 0x60) == 0x60  ? ((toc & 8) ? 960 : 480)
                          : ((toc >> 3) & 3) == 3 ? 2880
                                                  : (48000 << ((toc >> 3) & 3)) / 100;
      const int code = toc & 3;
      const int count = code == 0 ? 1 : code < 3 ? 2 : bytes.size() > 1 ? bytes[1] & 63 : 0;
      if (count && samples * count <= 5760) {
        frame.duration = std::chrono::microseconds(int64_t(samples) * count * 1000000 / 48000);
      }
    }
    if (auto captured = native->CaptureTime(); captured && captured->IsFinite()) {
      frame.sender_capture_time_us = captured->us();
    }
    target->receive(std::move(frame));
  }
};

struct PeerTransport::Channel : webrtc::DataChannelObserver {
  PeerTransport& owner;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> native;

  Channel(PeerTransport& owner, webrtc::scoped_refptr<webrtc::DataChannelInterface> native)
      : owner(owner), native(std::move(native)) {
    this->native->RegisterObserver(this);
  }

  ~Channel() override {
    native->UnregisterObserver();
  }

  void OnStateChange() override {}

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    std::lock_guard lock(owner.mutex_);
    if (owner.data_.size() >= owner.max_events_ || buffer.data.size() > 16384 ||
        owner.data_bytes_ + buffer.data.size() > 4 * 1024 * 1024) {
      owner.failure_ =
          make_failure(ErrorDomain::data, ErrorCode::resource_exhausted, "Data channel inbox full");
      return;
    }
    owner.data_.emplace_back(reinterpret_cast<const char*>(buffer.data.data()), buffer.data.size());
    owner.data_bytes_ += buffer.data.size();
  }
};

PeerTransport::PeerTransport(WebRtcContext& context, const livekit::JoinResponse& join,
                             size_t max_events)
    : context_(context), max_events_(max_events) {
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  config.continual_gathering_policy = webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
  if (join.client_configuration().force_relay() == livekit::ENABLED) {
    config.type = webrtc::PeerConnectionInterface::kRelay;
  }
  for (auto& server : join.ice_servers()) {
    webrtc::PeerConnectionInterface::IceServer ice;
    ice.urls.assign(server.urls().begin(), server.urls().end());
    ice.username = server.username();
    ice.password = server.credential();
    config.servers.push_back(ice);
  }
  auto result = context_.factory->CreatePeerConnectionOrError(
      config, webrtc::PeerConnectionDependencies(this));
  if (!result.ok()) {
    failure_ = native_error(result.error());
  } else {
    pc = result.MoveValue();
  }
}

PeerTransport::~PeerTransport() {
  close();
}

Result<std::string> PeerTransport::local_description(bool offer, const Operation& op) {
  auto answer = std::make_shared<Answer<std::unique_ptr<webrtc::SessionDescriptionInterface>>>();
  auto observer = webrtc::make_ref_counted<CreateDescription>(answer);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  if (offer) {
    pc->CreateOffer(observer.get(), options);
  } else {
    pc->CreateAnswer(observer.get(), options);
  }
  auto created = answer->wait(op);
  if (!created) {
    return Result<std::string>::failure(created.failure_record());
  }
  std::string sdp;
  (*created)->ToString(&sdp);
  auto applied = std::make_shared<Answer<void>>();
  auto setter = webrtc::make_ref_counted<SetDescription>(applied);
  pc->SetLocalDescription(setter.get(), created->release());
  auto result = applied->wait(op);
  return result ? Result<std::string>::success(std::move(sdp))
                : Result<std::string>::failure(result.failure_record());
}

Result<void> PeerTransport::remote_description(const livekit::SessionDescription& description,
                                               const Operation& op) {
  webrtc::SdpParseError parse;
  std::unique_ptr<webrtc::SessionDescriptionInterface> sdp(
      webrtc::CreateSessionDescription(description.type(), description.sdp(), &parse));
  if (!sdp) {
    return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                              "Invalid remote SDP: " + parse.description));
  }
  context_.signaling->BlockingCall([&] {
    for (const auto& [mid, sid] : description.mid_to_track_id()) {
      mids_[mid] = sid;
    }
  });
  auto applied = std::make_shared<Answer<void>>();
  auto setter = webrtc::make_ref_counted<SetDescription>(applied);
  pc->SetRemoteDescription(setter.get(), sdp.release());
  auto result = applied->wait(op);
  if (!result) {
    return result;
  }
  has_remote_ = true;
  context_.signaling->BlockingCall([&] {
    for (auto& t : pc->GetTransceivers()) {
      OnTrack(t);
    }
  });
  auto candidates = std::move(pending_ice_);
  pending_ice_.clear();
  for (const auto& candidate : candidates) {
    if (auto error = add_ice(candidate); !error) {
      return error;
    }
  }
  return Result<void>::success();
}

Result<void> PeerTransport::add_ice(const livekit::TrickleRequest& request) {
  if (request.candidateinit().empty()) {
    return Result<void>::success();
  }
  if (!has_remote_) {
    if (pending_ice_.size() >= max_events_) {
      return Result<void>::failure(
          make_failure(ErrorDomain::transport, ErrorCode::resource_exhausted, "ICE queue full"));
    }
    pending_ice_.push_back(request);
    return Result<void>::success();
  }
  auto json = nlohmann::json::parse(request.candidateinit(), nullptr, false);
  if (!json.is_object() || !json.contains("candidate") || !json["candidate"].is_string()) {
    return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                              "Invalid trickle candidate"));
  }
  std::string mid;
  int index = 0;
  if (json.contains("sdpMid") && json["sdpMid"].is_string()) {
    mid = json["sdpMid"].get<std::string>();
  }
  if (json.contains("sdpMLineIndex") && json["sdpMLineIndex"].is_number_integer()) {
    index = json["sdpMLineIndex"].get<int>();
  }
  webrtc::SdpParseError parse;
  std::unique_ptr<webrtc::IceCandidateInterface> candidate(
      webrtc::CreateIceCandidate(mid, index, json["candidate"].get<std::string>(), &parse));
  if (!candidate || !pc->AddIceCandidate(candidate.get())) {
    return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::request_failed,
                                              "Cannot apply ICE candidate"));
  }
  return Result<void>::success();
}

void PeerTransport::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  std::string sdp;
  candidate->ToString(&sdp);
  auto json = nlohmann::json{{"candidate", sdp},
                             {"sdpMid", candidate->sdp_mid()},
                             {"sdpMLineIndex", candidate->sdp_mline_index()}}
                  .dump();
  std::lock_guard lock(mutex_);
  if (ice_.size() >= max_events_) {
    failure_ =
        make_failure(ErrorDomain::transport, ErrorCode::resource_exhausted, "ICE output full");
  } else {
    ice_.push_back(std::move(json));
  }
}

void PeerTransport::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  state_ = state;
}

Result<void> PeerTransport::status() const {
  std::lock_guard lock(mutex_);
  if (failure_) {
    return Result<void>::failure(failure_);
  }
  if (state_ == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
    return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::connection_lost,
                                              "PeerConnection failed", true));
  }
  return Result<void>::success();
}

std::vector<std::string> PeerTransport::take_ice() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  while (!ice_.empty()) {
    out.push_back(std::move(ice_.front()));
    ice_.pop_front();
  }
  return out;
}

void PeerTransport::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  auto receiver = transceiver->receiver();
  if (!receiver || !receiver->track()) {
    return;
  }
  auto parameters = receiver->GetParameters();
  if (std::none_of(parameters.encodings.begin(), parameters.encodings.end(),
                   [](auto& e) { return e.ssrc && *e.ssrc; })) {
    return;
  }
  auto sid = receiver->track()->id();
  auto mapped = mids_.find(transceiver->mid().value_or(""));
  if (mapped != mids_.end()) {
    sid = mapped->second;
  }
  for (const auto& stream : receiver->streams()) {
    const auto id = stream->id();
    auto position = id.find("|TR_");
    if (position != std::string::npos) {
      sid = id.substr(position + 1);
    }
  }
  auto found = receivers_.find(sid);
  if (found != receivers_.end() && found->second.receiver == receiver) {
    return;
  }
  OnRemoveTrack(receiver);
  auto sink = webrtc::make_ref_counted<Receiver>();
  receiver->SetDepacketizerToDecoderFrameTransformer(sink);
  receivers_[sid] = {receiver, sink};
}

void PeerTransport::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  for (auto it = receivers_.begin(); it != receivers_.end();) {
    if (it->second.receiver == receiver) {
      auto sink = it->second.sink;
      std::lock_guard lock(sink->mutex);
      sink->track.reset();
      it = receivers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool PeerTransport::bind_receiver(const TrackSid& sid, std::shared_ptr<TrackData> track) {
  return context_.signaling->BlockingCall([&] {
    auto found = receivers_.find(sid);
    if (found == receivers_.end()) {
      return false;
    }
    std::lock_guard lock(found->second.sink->mutex);
    found->second.sink->track = track;
    return true;
  });
}

void PeerTransport::unbind_receiver(const TrackSid& sid) {
  context_.signaling->BlockingCall([&] {
    auto found = receivers_.find(sid);
    if (found == receivers_.end()) {
      return;
    }
    std::lock_guard lock(found->second.sink->mutex);
    found->second.sink->track.reset();
    found->second.sink->previous.reset();
  });
}

Result<void> PeerTransport::keyframe(const TrackSid& sid) {
  // BlockingCall's value overload requires a default-constructible return type.
  auto result = Result<void>::success();
  context_.signaling->BlockingCall([&] {
    auto found = receivers_.find(sid);
    if (found == receivers_.end() || found->second.receiver->track()->kind() != "video") {
      result = Result<void>::failure(
          make_failure(ErrorDomain::media, ErrorCode::invalid_state, "Video receiver unavailable"));
      return;
    }
    static_cast<webrtc::VideoTrackInterface*>(found->second.receiver->track().get())
        ->GetSource()
        ->GenerateKeyFrame();
  });
  return result;
}

void PeerTransport::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (channels_.size() >= 2 || (channel->label() != "_reliable" && channel->label() != "_lossy")) {
    channel->Close();
    return;
  }
  channels_.push_back(std::make_unique<Channel>(*this, std::move(channel)));
}

Result<void> PeerTransport::create_data_channels() {
  auto result = Result<void>::success();
  context_.signaling->BlockingCall([&] {
    for (bool reliable : {true, false}) {
      webrtc::DataChannelInit init;
      init.ordered = reliable;
      if (!reliable) {
        init.maxRetransmits = 0;
      }
      auto channel = pc->CreateDataChannelOrError(reliable ? "_reliable" : "_lossy", &init);
      if (!channel.ok()) {
        result = Result<void>::failure(native_error(channel.error()));
        return;
      }
      OnDataChannel(channel.MoveValue());
    }
  });
  return result;
}

bool PeerTransport::send_data(const std::string& label, const std::string& bytes) {
  return context_.signaling->BlockingCall([&] {
    for (const auto& channel : channels_) {
      if (channel->native->label() == label &&
          channel->native->state() == webrtc::DataChannelInterface::kOpen &&
          channel->native->buffered_amount() + bytes.size() <= 4 * 1024 * 1024) {
        return channel->native->Send(
            webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true));
      }
    }
    return false;
  });
}

std::vector<std::string> PeerTransport::take_data() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  while (!data_.empty()) {
    out.push_back(std::move(data_.front()));
    data_.pop_front();
  }
  data_bytes_ = 0;
  return out;
}

void PeerTransport::close() {
  if (!pc) {
    return;
  }
  context_.signaling->BlockingCall([&] {
    channels_.clear();
    pc->Close();
    receivers_.clear();
    pc = nullptr;
  });
}
}  // namespace livekit::sdk::client::detail
