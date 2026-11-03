#include "backends/livekit/rtc_session.hpp"
#include "backends/livekit/signal_client.hpp"
#include "backends/livekit/peer_transport.hpp"
#include "backends/livekit/media/encoded_audio_sender.hpp"
#include "backends/livekit/media/encoded_video_sender.hpp"
#include "api/make_ref_counted.h"
#include "support/logger.h"
#include "backends/livekit/data/native_room_data.hpp"
#include "backends/livekit/data/frame_cryptor.hpp"
#include "backends/livekit/data/data_message_wire.hpp"
#include <algorithm>
#include <set>
#include <thread>

namespace livekit::sdk::client::detail {
namespace {
PublicationInfo publication(const livekit::TrackInfo& p) {
  PublicationInfo value;
  value.sid = p.sid();
  value.name = p.name();
  value.mime_type = p.mime_type();
  value.stream = p.stream();
  value.kind = static_cast<PublicationKind>(p.type());
  value.source = static_cast<PublicationSource>(p.source());
  value.muted = p.muted();
  value.width = p.width();
  value.height = p.height();
  return value;
}

ParticipantSnapshot participant(const livekit::ParticipantInfo& p) {
  ParticipantSnapshot v;
  v.sid = p.sid();
  v.identity = p.identity();
  v.name = p.name();
  v.metadata = p.metadata();
  v.attributes.insert(p.attributes().begin(), p.attributes().end());
  v.version = p.version();
  v.state = p.state();
  v.kind = p.kind();
  v.disconnect_reason = p.disconnect_reason();
  for (const auto& track : p.tracks()) {
    v.publications[track.sid()] = publication(track);
  }
  if (p.has_permission()) {
    ParticipantPermissions permission;
    permission.can_publish = p.permission().can_publish();
    permission.can_subscribe = p.permission().can_subscribe();
    permission.can_publish_data = p.permission().can_publish_data();
    permission.hidden = p.permission().hidden();
    permission.can_update_metadata = p.permission().can_update_metadata();
    for (auto source : p.permission().can_publish_sources()) {
      permission.can_publish_sources.push_back(static_cast<PublicationSource>(source));
    }
    v.permissions = std::move(permission);
  }
  return v;
}

RoomInfo room(const livekit::Room& p) {
  return {p.sid(), p.name(), p.metadata(), p.active_recording()};
}

std::string codec(Codec value) {
  switch (value) {
    case Codec::opus:
      return "opus";
    case Codec::vp8:
      return "VP8";
    case Codec::h264:
      return "H264";
    case Codec::h265:
      return "H265";
  }
  return {};
}

// Rust RtcSession responsibilities: signaling requests, two peers, negotiation,
// native senders and data channels. No participant/publication registry here.
class NativeRtcSession final : public RtcSession {
 public:
  ~NativeRtcSession() override {
    close();
  }

  Result<JoinState> connect(const ConnectionOptions&, Epoch, const Operation&) override;
  Result<void> poll(const Operation&) override;
  void close() override;
  Result<PublicationInfo> add_track(const TrackConfig&, const Operation&) override;
  Result<void> create_sender(const TrackSid&, std::shared_ptr<TrackData>,
                             const Operation&) override;
  Result<void> remove_track(const TrackSid&, const Operation&) override;
  Result<void> subscribe(const TrackSid&, std::shared_ptr<TrackData>, bool,
                         const Operation&) override;
  Result<void> receive_options(const TrackSid&, ReceiveOptions) override;
  Result<void> mute(const TrackSid&, bool) override;

  Result<void> keyframe(const TrackSid& sid) override {
    return subscriber_->keyframe(sid);
  }

  Result<void> publish_data(const DataPacket&, const Operation&) override;
  Result<void> update(const ParticipantUpdate&, const Operation&) override;
  Result<void> publish_dtmf(const DtmfDigit&, const std::vector<std::string>&,
                            const Operation&) override;
  Result<void> publish_transcription(const Transcription&, const std::vector<std::string>&,
                                     const Operation&) override;

  Result<std::vector<PeerStatistics>> statistics(const Operation& op) override {
    std::vector<PeerStatistics> values;
    for (auto* peer : {publisher_.get(), subscriber_.get()}) {
      auto result = peer->statistics(op);
      if (!result) {
        return Result<std::vector<PeerStatistics>>::failure(result.failure_record());
      }
      result->target = peer == publisher_.get() ? 0 : 1;
      if (peer == publisher_.get()) {
        for (const auto& [sid, cid] : cids_) {
          result->publication_tracks[cid] = sid;
        }
      }
      values.push_back(std::move(*result));
    }
    return Result<std::vector<PeerStatistics>>::success(std::move(values));
  }

 private:
  struct Sender {
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> native;
    webrtc::scoped_refptr<EncodedAudioSender> audio;
    webrtc::scoped_refptr<EncodedVideoSender> video;
    std::shared_ptr<TrackData> track;
    uint64_t feedback_revision = 0;

    void close() {
      if (audio) {
        audio->close();
      }
      if (video) {
        video->close();
      }
    }
  };

  template <class F>
  Result<void> wait(F condition, const Operation& op) {
    while (!condition()) {
      if (auto result = op.status(); !result) {
        return result;
      }
      if (auto result = poll(op); !result) {
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return op.status();
  }

  Result<void> negotiate(const Operation&);
  Result<bool> send_packet(livekit::DataPacket);
  Result<void> send_packet_wait(livekit::DataPacket, const Operation&);

  void service_event(RoomEvent value) {
    SessionEvent event{SessionEvent::Kind::service};
    event.service_event = std::move(value);
    if (events) {
      events(std::move(event));
    }
  }

  struct CryptorReports {
    std::mutex mutex;
    std::deque<EncryptionStateEvent> events;

    void push(EncryptionStateEvent event) {
      std::lock_guard lock(mutex);
      if (events.size() < 256) {
        events.push_back(std::move(event));
      }
    }
  };

  std::shared_ptr<NativeRoomData> services_;
  std::shared_ptr<MediaCryptorContext> cryptor_;
  std::shared_ptr<CryptorReports> cryptor_reports_ = std::make_shared<CryptorReports>();
  Result<void> response(livekit::SignalResponse, const Operation&);
  ConnectionOptions options_;
  Epoch epoch_ = 0;
  std::unique_ptr<SignalClient> signal_;
  std::shared_ptr<WebRtcContext> context_;
  std::unique_ptr<PeerTransport> publisher_, subscriber_;
  std::optional<livekit::JoinResponse> join_;
  std::map<TrackSid, Sender> senders_;
  std::map<TrackSid, std::weak_ptr<TrackData>> receivers_;
  std::map<TrackSid, std::string> cids_;
  std::string pending_cid_;
  std::optional<PublicationInfo> published_;
  std::optional<uint32_t> pending_request_;
  bool request_done_ = false, negotiating_ = false, ready_ = false;
  Failure request_failure_, failure_;
  uint32_t request_id_ = 0;
  std::map<TrackSid, Failure> subscription_errors_;
  std::map<TrackSid, SteadyClock::time_point> keyframe_requests_;
  SteadyClock::time_point last_ping_, last_pong_;
};

Result<JoinState> NativeRtcSession::connect(const ConnectionOptions& options, Epoch epoch,
                                            const Operation& op) {
  options_ = options;
  epoch_ = epoch;
  signal_ = std::make_unique<SignalClient>();
  if (auto result = signal_->connect(options); !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  Operation handshake{std::min(op.deadline, SteadyClock::now() + options.regions.endpoint_timeout),
                      op.stopped};
  while (!join_) {
    if (auto result = handshake.status(); !result) {
      return Result<JoinState>::failure(result.failure_record());
    }
    if (auto result = signal_->status(); !result) {
      return Result<JoinState>::failure(result.failure_record());
    }
    if (auto incoming = signal_->next()) {
      if (incoming->has_join()) {
        join_ = incoming->join();
      } else if (incoming->has_leave()) {
        return Result<JoinState>::failure(make_failure(
            ErrorDomain::signaling, ErrorCode::permission_denied, "Server rejected join"));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  if (!join_->alternative_url().empty()) {
    auto failure = std::make_shared<FailureInfo>(*make_failure(
        ErrorDomain::signaling, ErrorCode::request_failed, "Server requested a region redirect"));
    failure->backend_code = "region_redirect";
    failure->backend_data = join_->alternative_url();
    return Result<JoinState>::failure(std::move(failure));
  }
  services_ = std::static_pointer_cast<NativeRoomData>(data_services);
  if (services_) {
    auto result = services_->connect(options.encryption);
    if (!result) {
      return Result<JoinState>::failure(result.failure_record());
    }
  }
  cryptor_ = std::make_shared<MediaCryptorContext>();
  cryptor_->options = options.encryption;
  cryptor_->sif.assign(join_->sif_trailer().begin(), join_->sif_trailer().end());
  cryptor_->report = [reports = cryptor_reports_](const std::string& sid, bool ok, int key) {
    reports->push({sid, true, false, ok, key});
  };
  cryptor_->participant(join_->participant());
  for (const auto& p : join_->other_participants()) {
    cryptor_->participant(p);
    if (services_) {
      services_->participant(p);
    }
  }
  context_ = std::make_shared<WebRtcContext>();
  if (auto status = context_->status(); !status) {
    return Result<JoinState>::failure(status.failure_record());
  }
  publisher_ = std::make_unique<PeerTransport>(*context_, *join_, options.max_events,
                                               options.media_transport, cryptor_);
  subscriber_ = std::make_unique<PeerTransport>(*context_, *join_, options.max_events,
                                                options.media_transport, cryptor_);
  if (auto result = publisher_->status(); !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  if (auto result = subscriber_->status(); !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  if (auto result = publisher_->create_data_channels(); !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  last_ping_ = last_pong_ = SteadyClock::now();
  if (auto result = negotiate(op); !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  // JoinResponse alone is not connection success. Rust waits for the primary PC.
  if (auto result = wait(
          [&] {
            return join_->subscriber_primary() ? subscriber_->connected() : publisher_->connected();
          },
          op);
      !result) {
    return Result<JoinState>::failure(result.failure_record());
  }
  JoinState state;
  state.room = room(join_->room());
  state.local = participant(join_->participant());
  for (const auto& remote : join_->other_participants()) {
    state.participants.push_back(participant(remote));
  }
  ready_ = true;
  return Result<JoinState>::success(std::move(state));
}

Result<void> NativeRtcSession::negotiate(const Operation& op) {
  auto offer = publisher_->local_description(true, op);
  if (!offer) {
    return Result<void>::failure(offer.failure_record());
  }
  livekit::SignalRequest request;
  request.mutable_offer()->set_type("offer");
  request.mutable_offer()->set_sdp(*offer);
  negotiating_ = true;
  if (auto result = signal_->send(request); !result) {
    return result;
  }
  return wait([&] { return !negotiating_ && publisher_->connected(); }, op);
}

Result<void> NativeRtcSession::response(livekit::SignalResponse message, const Operation& op) {
  if (services_) {
    if (auto result = services_->signal(message); !result) {
      return result;
    }
  }
  if (message.has_offer()) {
    if (auto result = subscriber_->remote_description(message.offer(), op); !result) {
      return result;
    }
    auto answer = subscriber_->local_description(false, op);
    if (!answer) {
      return Result<void>::failure(answer.failure_record());
    }
    livekit::SignalRequest request;
    request.mutable_answer()->set_type("answer");
    request.mutable_answer()->set_sdp(*answer);
    request.mutable_answer()->set_id(message.offer().id());
    return signal_->send(request);
  }
  if (message.has_answer()) {
    if (auto result = publisher_->remote_description(message.answer(), op); !result) {
      return result;
    }
    negotiating_ = false;
  } else if (message.has_trickle()) {
    return (message.trickle().target() == livekit::PUBLISHER ? publisher_ : subscriber_)
        ->add_ice(message.trickle());
  } else if (message.has_update()) {
    for (const auto& p : message.update().participants()) {
      if (cryptor_) {
        cryptor_->participant(p);
      }
      if (services_) {
        services_->participant(p);
      }
      if (!ready_) {
        if (p.identity() == join_->participant().identity()) {
          *join_->mutable_participant() = p;
        } else {
          auto* peers = join_->mutable_other_participants();
          auto found = std::find_if(peers->begin(), peers->end(), [&](const auto& value) {
            return value.identity() == p.identity();
          });
          if (found != peers->end()) {
            *found = p;
          } else {
            *peers->Add() = p;
          }
        }
        continue;
      }
      SessionEvent event{SessionEvent::Kind::participant};
      event.participant = participant(p);
      if (events) {
        events(std::move(event));
      }
      RoomEvent data_event;
      data_event.kind = RoomEventKind::data_tracks_changed;
      data_event.participant_identity = p.identity();
      service_event(std::move(data_event));
    }
  } else if (message.has_speakers_changed()) {
    SessionEvent event{SessionEvent::Kind::speakers};
    for (const auto& speaker : message.speakers_changed().speakers()) {
      ParticipantSnapshot value;
      value.sid = speaker.sid();
      value.speaking = speaker.active();
      value.audio_level = speaker.level();
      event.speakers.push_back(std::move(value));
    }
    if (events) {
      events(std::move(event));
    }
  } else if (message.has_connection_quality()) {
    for (const auto& update : message.connection_quality().updates()) {
      SessionEvent event{SessionEvent::Kind::quality};
      event.participant.sid = update.participant_sid();
      event.participant.connection_quality = update.quality();
      if (events) {
        events(std::move(event));
      }
    }
  } else if (message.has_room_update()) {
    SessionEvent event{SessionEvent::Kind::room};
    event.room = room(message.room_update().room());
    if (events) {
      events(std::move(event));
    }
  } else if (message.has_track_published()) {
    if (message.track_published().cid() == pending_cid_) {
      published_ = publication(message.track_published().track());
    }
  } else if (message.has_track_unpublished()) {
    // Local unpublish completes on sender removal/negotiation, like Rust.
    // This unsolicited server notification is not a remove operation ACK.
  } else if (message.has_subscription_response() && message.subscription_response().err()) {
    if (subscription_errors_.size() < options_.max_tracks) {
      subscription_errors_[message.subscription_response().track_sid()] = make_failure(
          ErrorDomain::media, ErrorCode::request_failed, "Subscription rejected by server");
    }
  } else if (message.has_request_response()) {
    const auto& r = message.request_response();
    if ((pending_request_ && r.request_id() == *pending_request_) ||
        (!pending_cid_.empty() && r.has_add_track() && r.add_track().cid() == pending_cid_)) {
      if (r.reason() != livekit::RequestResponse::QUEUED) {
        request_done_ = true;
        if (r.reason() != livekit::RequestResponse::OK) {
          request_failure_ =
              make_failure(ErrorDomain::signaling, ErrorCode::request_failed, r.message());
        }
      }
    }
  } else if (message.has_subscribed_quality_update()) {
    auto found = senders_.find(message.subscribed_quality_update().track_sid());
    if (found != senders_.end() && found->second.video &&
        found->second.track->config.follow_subscriber_demand) {
      bool active = false;
      for (const auto& q : message.subscribed_quality_update().subscribed_qualities()) {
        active |= q.enabled();
      }
      for (const auto& c : message.subscribed_quality_update().subscribed_codecs()) {
        for (const auto& q : c.qualities()) {
          active |= q.enabled();
        }
      }
      found->second.video->set_demand(active);
    }
  } else if (message.has_refresh_token()) {
    SessionEvent event{SessionEvent::Kind::token};
    event.token = message.refresh_token();
    if (events) {
      events(std::move(event));
    }
  } else if (message.has_mute()) {
    SessionEvent event{SessionEvent::Kind::mute};
    event.sid = message.mute().sid();
    event.muted = message.mute().muted();
    if (events) {
      events(std::move(event));
    }
  } else if (message.has_leave()) {
    return Result<void>::failure(
        make_failure(ErrorDomain::signaling, ErrorCode::connection_lost, "Server requested leave",
                     message.leave().can_reconnect() || message.leave().action() != 0,
                     message.leave().reason()));
  } else if (message.has_pong() || message.has_pong_resp()) {
    last_pong_ = SteadyClock::now();
  }
  return Result<void>::success();
}

Result<void> NativeRtcSession::poll(const Operation& op) {
  if (context_->codec_counters->pcm_encode_calls || context_->codec_counters->pcm_decode_calls ||
      context_->codec_counters->unexpected_video_decode_calls) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::internal_error,
                     "Unexpected raw-media codec call in encoded-only client"));
  }
  if (failure_) {
    return Result<void>::failure(failure_);
  }
  // A server DISCONNECT can be followed immediately by a WebSocket close.
  // Consume the explicit protocol decision before treating the socket error as retryable.
  std::vector<livekit::SignalResponse> pending;
  for (int i = 0; i < 64; ++i) {
    auto message = signal_->next();
    if (!message) {
      break;
    }
    if (message->has_leave()) {
      return response(std::move(*message), op);
    }
    pending.push_back(std::move(*message));
  }
  for (auto& message : pending) {
    if (auto result = response(std::move(message), op); !result) {
      return result;
    }
  }
  if (auto result = signal_->status(); !result) {
    return result;
  }
  if (auto result = publisher_->status(); !result) {
    return result;
  }
  if (auto result = subscriber_->status(); !result) {
    return result;
  }
  for (auto* peer : {publisher_.get(), subscriber_.get()}) {
    for (const auto& ice : peer->take_ice()) {
      livekit::SignalRequest request;
      request.mutable_trickle()->set_candidateinit(ice);
      request.mutable_trickle()->set_target(peer == publisher_.get() ? livekit::PUBLISHER
                                                                     : livekit::SUBSCRIBER);
      if (auto result = signal_->send(request); !result) {
        return result;
      }
    }
    for (const auto& incoming : peer->take_data()) {
      if (incoming.label == "_data_track") {
        if (services_) {
          auto result = services_->receive_track(
              {reinterpret_cast<const uint8_t*>(incoming.bytes.data()), incoming.bytes.size()});
          if (!result) {
            RoomEvent event;
            event.kind = RoomEventKind::data_error;
            event.failure = result.failure_record();
            service_event(std::move(event));
          }
        }
        continue;
      }
      livekit::DataPacket data;
      if (!data.ParseFromString(incoming.bytes)) {
        continue;
      }
      auto decrypted = decrypt_packet(data, options_.encryption);
      if (!decrypted) {
        RoomEvent event;
        event.kind = RoomEventKind::data_error;
        event.failure = decrypted.failure_record();
        event.participant_identity = data.participant_identity();
        service_event(std::move(event));
        continue;
      }
      data = std::move(*decrypted);
      if (services_) {
        services_->receive(data);
      }
      if (data.has_sip_dtmf()) {
        DtmfDigit value{data.sip_dtmf().code(), data.sip_dtmf().digit()};
        if (validate_dtmf(value)) {
          RoomEvent event;
          event.kind = RoomEventKind::dtmf_received;
          event.dtmf = std::move(value);
          event.participant_identity = data.participant_identity();
          service_event(std::move(event));
        }
      }
      if (data.has_transcription()) {
        Transcription value;
        value.participant_identity = data.transcription().transcribed_participant_identity();
        value.track_id = data.transcription().track_id();
        for (const auto& segment : data.transcription().segments()) {
          value.segments.push_back({segment.id(), segment.text(), segment.start_time(),
                                    segment.end_time(), segment.final(), segment.language()});
        }
        if (validate_transcription(value)) {
          RoomEvent event;
          event.kind = RoomEventKind::transcription_received;
          event.transcription = std::move(value);
          event.participant_identity = data.participant_identity();
          service_event(std::move(event));
        }
      }
      if (data.has_speaker()) {
        SessionEvent event{SessionEvent::Kind::speakers};
        event.full_speaker_update = true;
        for (const auto& speaker : data.speaker().speakers()) {
          ParticipantSnapshot value;
          value.sid = speaker.sid();
          value.speaking = speaker.active();
          value.audio_level = speaker.level();
          event.speakers.push_back(std::move(value));
        }
        if (events) {
          events(std::move(event));
        }
      }
      if (!data.has_user()) {
        continue;
      }
      SessionEvent event{SessionEvent::Kind::data};
      event.data.payload.assign(data.user().payload().begin(), data.user().payload().end());
      event.data.topic = data.user().topic();
      event.data.reliable = data.kind() == 0;
      event.identity = data.participant_identity().empty() ? data.user().participant_identity()
                                                           : data.participant_identity();
      if (events) {
        events(std::move(event));
      }
    }
  }
  if (ready_ && services_) {
    auto result = services_->pump(
        [this](const livekit::DataPacket& packet) { return send_packet(packet); },
        [this](const livekit::SignalRequest& request) { return signal_->send(request); },
        [this](const std::vector<uint8_t>& bytes) {
          return publisher_->send_data("_data_track", std::string(bytes.begin(), bytes.end()));
        });
    if (!result) {
      return result;
    }
  }
  std::deque<EncryptionStateEvent> reports;
  {
    std::lock_guard lock(cryptor_reports_->mutex);
    reports.swap(cryptor_reports_->events);
  }
  for (auto& report : reports) {
    RoomEvent event;
    event.kind = RoomEventKind::encryption_changed;
    event.encryption = std::move(report);
    service_event(std::move(event));
  }
  for (auto& [sid, weak] : receivers_) {
    if (auto track = weak.lock()) {
      subscriber_->bind_receiver(sid, track);
      bool request;
      {
        std::lock_guard lock(track->mutex);
        request = track->needs_keyframe && track->config.kind == MediaKind::video;
      }
      if (request &&
          SteadyClock::now() - keyframe_requests_[sid] >= std::chrono::milliseconds(500)) {
        auto ignored = subscriber_->keyframe(sid);
        (void)ignored;
        keyframe_requests_[sid] = SteadyClock::now();
      }
    }
  }
  auto now = SteadyClock::now();
  for (auto& [sid, sender] : senders_) {
    if (sender.video) {
      auto feedback = sender.video->feedback();
      if (feedback.revision != sender.feedback_revision) {
        sender.feedback_revision = feedback.revision;
        SessionEvent event{SessionEvent::Kind::feedback};
        event.sid = sid;
        event.feedback = std::move(feedback);
        if (events) {
          events(std::move(event));
        }
      }
    }
  }
  if (now - last_ping_ >= std::chrono::seconds(std::max(1, join_->ping_interval()))) {
    livekit::SignalRequest request;
    request.mutable_ping_req()->set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (auto result = signal_->send(request); !result) {
      return result;
    }
    last_ping_ = now;
  }
  if (now - last_pong_ > std::chrono::seconds(std::max(10, join_->ping_timeout()))) {
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::connection_lost,
                                              "Signaling heartbeat timed out", true));
  }
  return Result<void>::success();
}

Result<PublicationInfo> NativeRtcSession::add_track(const TrackConfig& config,
                                                    const Operation& op) {
  livekit::SignalRequest request;
  auto* add = request.mutable_add_track();
  pending_cid_ = config.id + "-" + std::to_string(epoch_) + "-" + std::to_string(++request_id_);
  add->set_cid(pending_cid_);
  add->set_name(config.name);
  add->set_type(config.kind == MediaKind::audio ? livekit::AUDIO : livekit::VIDEO);
  add->set_source(static_cast<livekit::TrackSource>(config.source));
  add->set_stream(config.stream_group.empty() ? "livekit" : config.stream_group);
  add->set_muted(config.muted);
  add->set_disable_red(true);
  if (options_.encryption.keys && options_.encryption.media) {
    auto supported = encryption_codec(codec(config.format.codec));
    if (!supported) {
      return Result<PublicationInfo>::failure(supported.failure_record());
    }
    add->set_encryption(livekit::Encryption::GCM);
  }
  if (config.kind == MediaKind::audio) {
    add->set_stereo(config.format.channels == 2);
  } else {
    add->set_width(config.format.width);
    add->set_height(config.format.height);
    auto* layer = add->add_layers();
    layer->set_quality(2);
    layer->set_width(config.format.width);
    layer->set_height(config.format.height);
    layer->set_bitrate(config.encodings.front().max_bitrate_bps);
    auto* encoding = add->add_simulcast_codecs();
    auto name = codec(config.format.codec);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    encoding->set_codec(name);
    encoding->set_cid(pending_cid_);
    *encoding->mutable_layers() = add->layers();
  }
  published_.reset();
  request_failure_.reset();
  request_done_ = false;
  auto result = signal_->send(request);
  if (result) {
    result = wait([&] { return published_.has_value() || request_failure_; }, op);
  }
  if (result && request_failure_) {
    result = Result<void>::failure(request_failure_);
  }
  if (!result) {
    // No remove-track signal exists for an acknowledged track without a sender.
    // Rejoin fences late responses and discards any uncertain server publication.
    if (result.error().code == ErrorCode::timeout) {
      failure_ = make_failure(ErrorDomain::signaling, ErrorCode::outcome_unknown,
                              "AddTrack outcome unknown; rejoin required", true);
    }
    pending_cid_.clear();
    return Result<PublicationInfo>::failure(result.failure_record());
  }
  cids_[published_->sid] = std::move(pending_cid_);
  pending_cid_.clear();
  return Result<PublicationInfo>::success(*published_);
}

Result<void> NativeRtcSession::create_sender(const TrackSid& sid, std::shared_ptr<TrackData> track,
                                             const Operation& op) {
  auto cid = cids_.find(sid);
  if (cid == cids_.end()) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::stale_handle,
                                              "Sender requires acknowledged AddTrack"));
  }
  TrackConfig config;
  {
    std::lock_guard lock(track->mutex);
    config = track->config;
  }
  Sender sender;
  sender.track = track;
  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> native;
  if (config.kind == MediaKind::audio) {
    sender.audio = webrtc::make_ref_counted<EncodedAudioSender>(epoch_, config.queue,
                                                                config.encodings.front().id);
    sender.audio->set_muted(config.muted);
    native = context_->factory->CreateAudioTrack(cid->second, nullptr);
  } else {
    sender.video = context_->signaling->BlockingCall(
        [&] { return webrtc::make_ref_counted<EncodedVideoSender>(epoch_, config); });
    native = context_->factory->CreateVideoTrack(sender.video, cid->second);
  }
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
  init.stream_ids = {config.stream_group.empty() ? "livekit" : config.stream_group};
  webrtc::RtpEncodingParameters encoding;
  encoding.active = config.encodings.front().active;
  encoding.max_bitrate_bps = int(config.encodings.front().max_bitrate_bps);
  init.send_encodings.push_back(encoding);
  auto added = publisher_->pc->AddTransceiver(native, init);
  if (!added.ok()) {
    return Result<void>::failure(
        make_failure(ErrorDomain::transport, ErrorCode::request_failed, added.error().message()));
  }
  sender.native = added.value()->sender();
  if (options_.encryption.keys && options_.encryption.media) {
    auto format = encryption_codec(codec(config.format.codec));
    if (!format) {
      return Result<void>::failure(format.failure_record());
    }
    sender.native->SetFrameEncryptor(webrtc::make_ref_counted<NativeFrameEncryptor>(
        options_.encryption.keys, *format, [reports = cryptor_reports_, sid](bool ok, int key) {
          reports->push({sid, true, true, ok, key});
        }));
  }
  senders_[sid] = sender;
  if (sender.audio) {
    sender.native->SetEncoderToPacketizerFrameTransformer(sender.audio);
  }
  auto kind = config.kind == MediaKind::audio ? webrtc::MediaType::AUDIO : webrtc::MediaType::VIDEO;
  auto codecs = select_codec_envelopes(context_->factory->GetRtpSenderCapabilities(kind).codecs,
                                       codec(config.format.codec), config.format.fmtp, true);
  if (codecs.empty()) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::unsupported,
                                              "Requested codec/fmtp unavailable"));
  }
  auto selected = added.value()->SetCodecPreferences(codecs);
  if (!selected.ok()) {
    return Result<void>::failure(
        make_failure(ErrorDomain::transport, ErrorCode::request_failed, selected.message()));
  }
  if (auto result = negotiate(op); !result) {
    return result;
  }
  if (sender.audio) {
    auto params = sender.native->GetParameters();
    auto payload = std::find_if(params.codecs.begin(), params.codecs.end(),
                                [](auto& c) { return c.name == "opus"; });
    if (params.encodings.empty() || !params.encodings.front().ssrc ||
        payload == params.codecs.end()) {
      return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::request_failed,
                                                "Missing negotiated Opus SSRC/payload type"));
    }
    auto configured =
        sender.audio->configure(*params.encodings.front().ssrc, uint8_t(payload->payload_type));
    if (!configured) {
      return configured;
    }
  }
  {
    std::lock_guard lock(track->mutex);
    // Captured context keeps native threads alive until a concurrent send finishes.
    track->send = [audio = sender.audio, video = sender.video,
                   context = context_](const EncodedFrame& frame) {
      return audio ? audio->send(frame) : video->send(frame);
    };
    track->mute_sender = [audio = sender.audio, video = sender.video](bool muted) {
      if (audio) {
        audio->set_muted(muted);
      } else {
        video->set_muted(muted);
      }
    };
    track->feedback.epoch = epoch_;
    track->feedback.revision = 1;
    track->feedback.encodings[config.encodings.front().id].target_bitrate_bps =
        config.encodings.front().max_bitrate_bps;
    if (sender.video) {
      track->sender_feedback = [video = sender.video] { return video->feedback(); };
    }
  }
  return Result<void>::success();
}

Result<void> NativeRtcSession::remove_track(const TrackSid& sid, const Operation& op) {
  auto found = senders_.find(sid);
  if (found == senders_.end()) {
    if (cids_.erase(sid)) {
      failure_ = make_failure(ErrorDomain::signaling, ErrorCode::outcome_unknown,
                              "Publication without sender requires rejoin", true);
    }
    return failure_ ? Result<void>::failure(failure_) : Result<void>::success();
  }
  found->second.close();
  auto removed = publisher_->pc->RemoveTrackOrError(found->second.native);
  senders_.erase(found);
  cids_.erase(sid);
  if (!removed.ok()) {
    return Result<void>::failure(
        make_failure(ErrorDomain::transport, ErrorCode::request_failed, removed.message()));
  }
  if (auto result = negotiate(op); !result) {
    failure_ = make_failure(ErrorDomain::signaling, ErrorCode::outcome_unknown,
                            "RemoveTrack negotiation incomplete", true);
    return result;
  }
  return Result<void>::success();
}

Result<void> NativeRtcSession::subscribe(const TrackSid& sid, std::shared_ptr<TrackData> track,
                                         bool enabled, const Operation& op) {
  livekit::SignalRequest request;
  request.mutable_subscription()->add_track_sids(sid);
  request.mutable_subscription()->set_subscribe(enabled);
  if (!enabled) {
    subscriber_->unbind_receiver(sid);
    receivers_.erase(sid);
    track->transition(TrackState::suspended);
  }
  if (auto result = signal_->send(request); !result) {
    return result;
  }
  if (!enabled) {
    return Result<void>::success();
  }
  subscription_errors_.erase(sid);
  auto result = wait(
      [&] { return subscription_errors_.count(sid) || subscriber_->bind_receiver(sid, track); },
      op);
  if (result && subscription_errors_.count(sid)) {
    result = Result<void>::failure(subscription_errors_[sid]);
  }
  if (!result) {
    subscriber_->unbind_receiver(sid);
    request.mutable_subscription()->set_subscribe(false);
    auto ignored = signal_->send(request);
    (void)ignored;
    return result;
  }
  receivers_[sid] = track;
  track->transition(TrackState::ready, epoch_, sid);
  return Result<void>::success();
}

Result<void> NativeRtcSession::receive_options(const TrackSid& sid, ReceiveOptions options) {
  livekit::SignalRequest request;
  auto* p = request.mutable_track_setting();
  p->add_track_sids(sid);
  p->set_disabled(options.disabled);
  p->set_quality(int(options.quality));
  p->set_width(options.width);
  p->set_height(options.height);
  p->set_fps(options.fps);
  p->set_priority(options.priority);
  return signal_->send(request);
}

Result<void> NativeRtcSession::mute(const TrackSid& sid, bool muted) {
  livekit::SignalRequest request;
  request.mutable_mute()->set_sid(sid);
  request.mutable_mute()->set_muted(muted);
  auto result = signal_->send(request);
  if (result && events) {
    SessionEvent event{SessionEvent::Kind::mute};
    event.sid = sid;
    event.muted = muted;
    events(std::move(event));
  }
  return result;
}

Result<void> NativeRtcSession::publish_data(const DataPacket& packet, const Operation& op) {
  livekit::DataPacket data;
  data.set_kind(packet.reliable ? 0 : 1);
  data.set_participant_identity(join_->participant().identity());
  auto* user = data.mutable_user();
  user->set_payload(packet.payload.data(), packet.payload.size());
  user->set_topic(packet.topic);
  for (auto& identity : packet.destinations) {
    data.add_destination_identities(identity);
    user->add_destination_identities(identity);
  }
  return send_packet_wait(std::move(data), op);
}

Result<bool> NativeRtcSession::send_packet(livekit::DataPacket packet) {
  packet.set_participant_identity(join_->participant().identity());
  auto sealed = encrypt_packet(packet, options_.encryption);
  if (!sealed) {
    return Result<bool>::failure(sealed.failure_record());
  }
  auto bytes = sealed->SerializeAsString();
  if (bytes.size() > 16384) {
    return Result<bool>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                              "Data packet exceeds 16 KiB wire limit"));
  }
  return Result<bool>::success(
      publisher_->send_data(packet.kind() == 0 ? "_reliable" : "_lossy", bytes));
}

Result<void> NativeRtcSession::send_packet_wait(livekit::DataPacket packet, const Operation& op) {
  for (;;) {
    if (auto result = op.status(); !result) {
      return result;
    }
    auto result = send_packet(packet);
    if (!result) {
      return Result<void>::failure(result.failure_record());
    }
    if (*result) {
      return Result<void>::success();
    }
    if (auto polled = poll(op); !polled) {
      return polled;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

Result<void> NativeRtcSession::publish_dtmf(const DtmfDigit& value,
                                            const std::vector<std::string>& destinations,
                                            const Operation& op) {
  auto packet = encode_dtmf(value);
  if (!packet) {
    return Result<void>::failure(packet.failure_record());
  }
  for (const auto& identity : destinations) {
    packet->add_destination_identities(identity);
  }
  return send_packet_wait(std::move(*packet), op);
}

Result<void> NativeRtcSession::publish_transcription(const Transcription& value,
                                                     const std::vector<std::string>& destinations,
                                                     const Operation& op) {
  auto packet = encode_transcription(value);
  if (!packet) {
    return Result<void>::failure(packet.failure_record());
  }
  for (const auto& identity : destinations) {
    packet->add_destination_identities(identity);
  }
  return send_packet_wait(std::move(*packet), op);
}

Result<void> NativeRtcSession::update(const ParticipantUpdate& update, const Operation& op) {
  pending_request_ = ++request_id_;
  request_done_ = false;
  request_failure_.reset();
  livekit::SignalRequest request;
  auto* p = request.mutable_update_metadata();
  p->set_name(update.name);
  p->set_metadata(update.metadata);
  p->set_request_id(*pending_request_);
  p->mutable_attributes()->insert(update.attributes.begin(), update.attributes.end());
  auto result = signal_->send(request);
  if (result) {
    result = wait([&] { return request_done_; }, op);
  }
  pending_request_.reset();
  if (!result) {
    return result;
  }
  return request_failure_ ? Result<void>::failure(request_failure_) : Result<void>::success();
}

void NativeRtcSession::close() {
  // An intentional close must leave the room, not enter the server's reconnect grace period.
  if (signal_ && ready_ && signal_->status()) {
    livekit::SignalRequest request;
    request.mutable_leave()->set_reason(1);  // CLIENT_INITIATED
    auto result = signal_->send(request);
    if (!result) {
      LOG_ERROR("LiveKitClient", "leave: " << result.error().message);
    }
  }
  ready_ = false;
  for (auto& [sid, sender] : senders_) {
    sender.close();
  }
  senders_.clear();
  receivers_.clear();
  if (publisher_) {
    publisher_->close();
    publisher_.reset();
  }
  if (subscriber_) {
    subscriber_->close();
    subscriber_.reset();
  }
  context_.reset();
  if (signal_) {
    signal_->close();
    signal_.reset();
  }
}
}  // namespace

std::unique_ptr<RtcSession> create_rtc_session() {
  return std::make_unique<NativeRtcSession>();
}
}  // namespace livekit::sdk::client::detail
