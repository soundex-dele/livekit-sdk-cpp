#include "backends/rtc/livekit/transport_factory.hpp"
#include "livekit/rtc/livekit.hpp"
#include "backends/rtc/room_transport.hpp"
#include "backends/rtc/control_context.hpp"
#include "backends/rtc/bounded_events.hpp"
#include "support/ready_set.hpp"
#include "backends/webrtc/native_peer.hpp"
#include "backends/webrtc/encoded_audio_sender.hpp"
#include "backends/webrtc/encoded_video_sender.hpp"
#include "backends/rtc/livekit/signal_session.hpp"
#include "backends/rtc/livekit/room_service_state.hpp"
#include "backends/rtc/livekit/session_roster.hpp"
#include "backends/rtc/livekit/client_configuration.hpp"
#include "backends/rtc/livekit/subscribed_quality.hpp"
#include "backends/rtc/livekit/subscription_policy_wire.hpp"
#include "backends/rtc/livekit/frame_cryptor.hpp"
#include "backends/webrtc/transport_policy.hpp"
#include "api/make_ref_counted.h"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <set>
#include <thread>

namespace livekit::sdk::rtc::detail {
namespace {
using PC = webrtc::PeerConnectionInterface;
using SignalRecoveryState = livekit::sdk::detail::SignalRecoveryState;
using livekit::sdk::ErrorCode;
using livekit::sdk::ErrorDomain;

// Native completions post back to the session owner. At most two SDP operations
// per peer and two statistics requests can be outstanding; the queue is bounded
// independently and carries no transport/peer owner pointer.
struct CompletionTasks {
  std::mutex mutex;
  std::deque<std::function<void()>> tasks;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup;
  bool overflow = false;

  void post(std::function<void()> task) {
    {
      std::lock_guard lock(mutex);
      if (tasks.size() >= 64) {
        overflow = true;
      } else {
        tasks.push_back(std::move(task));
      }
    }
    // Report dispatch failure on the session loop, never through WebRTC's
    // observer callback or an observer destructor completing a cancelled SDP.
    wakeup->signal();
  }

  Result<void> run() {
    std::deque<std::function<void()>> ready;
    {
      std::lock_guard lock(mutex);
      if (overflow) {
        return Result<void>::failure(make_failure(ErrorDomain::lifecycle,
                                                  ErrorCode::resource_exhausted,
                                                  "Native completion queue exhausted"));
      }
      ready.swap(tasks);
    }
    for (auto& task : ready) {
      task();
    }
    return Result<void>::success();
  }
};

Result<std::string> codec_name(Codec codec) {
  switch (codec) {
    case Codec::opus:
      return Result<std::string>::success("opus");
    case Codec::vp8:
      return Result<std::string>::success("VP8");
    case Codec::h264:
      return Result<std::string>::success("H264");
    case Codec::h265:
      return Result<std::string>::success("H265");
  }
  return Result<std::string>::failure(
      make_failure(ErrorDomain::media, ErrorCode::invalid_argument, "Unknown encoded codec"));
}

std::chrono::microseconds opus_duration(std::span<const uint8_t> bytes) {
  if (bytes.empty())
    return {};
  const uint8_t toc = bytes[0];
  int samples;
  if (toc & 0x80)
    samples = (48000 << ((toc >> 3) & 3)) / 400;
  else if ((toc & 0x60) == 0x60)
    samples = (toc & 8) ? 960 : 480;
  else
    samples = ((toc >> 3) & 3) == 3 ? 2880 : (48000 << ((toc >> 3) & 3)) / 100;
  const int code = toc & 3;
  const int count = code == 0 ? 1 : code < 3 ? 2 : bytes.size() > 1 ? bytes[1] & 63 : 0;
  if (!count || samples * count > 5760)
    return {};
  return std::chrono::microseconds(int64_t(samples) * count * 1000000 / 48000);
}

TransportEvent event(TransportEventKind kind, BindingToken binding = {}, std::string error = {}) {
  TransportEvent value{kind, std::move(binding)};
  value.error = std::move(error);
  return value;
}

struct Publication {
  // Keep native threads alive until any controller-side send snapshot is gone.
  std::shared_ptr<TransportRuntime> runtime;
  BindingToken binding;
  TrackDescriptor descriptor;
  std::string cid, sid;
  webrtc::scoped_refptr<PC> pc;
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
  webrtc::scoped_refptr<EncodedAudioSender> audio;
  webrtc::scoped_refptr<EncodedVideoSender> video;
  std::atomic<bool> ready{false}, closed{false};
  uint64_t feedback_revision = 0;  // session worker only

  void close() {
    closed = true;
    ready = false;
    if (audio)
      audio->close();
    if (video)
      video->close();
  }
};

struct Subscription {
  BindingToken binding;
  TrackDescriptor descriptor;
  webrtc::scoped_refptr<EncodedReceiver> receiver;
  std::shared_ptr<QueueBudget> budget;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> media_wakeup;
  // Timestamp and dependency state belongs exclusively to the session worker.
  std::optional<uint32_t> last_timestamp;
  uint64_t timestamp = 0;
  std::optional<std::pair<uint64_t, int64_t>> sender_clock;
  bool waiting_for_keyframe = true;
};

struct Command {
  enum Kind {
    publish,
    subscribe,
    remove,
    keyframe,
    metadata,
    mute,
    receive_demand,
    permissions
  } kind;

  BindingToken binding;
  TrackDescriptor descriptor;
  RoomTransport::Removed completion;
  std::shared_ptr<QueueBudget> budget;
  std::optional<ParticipantUpdateRequest> participant_update;
  std::optional<MuteRequest> mute_request;
  std::optional<ReceiveDemandRequest> demand_request;
  std::optional<SubscriptionPermissionsRequest> permissions_request;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> media_wakeup;
};

class LiveKitTransport final : public RoomTransport {
 public:
  ~LiveKitTransport() override {
    stop();
  }

  std::shared_ptr<RoomServices> services() const override {
    return services_->handle();
  }

  void set_queue_budget(std::shared_ptr<QueueBudget> budget) override {
    budget_ = std::move(budget);
  }

  BackendCapabilities capabilities() const override {
    BackendCapabilities result;
    result.backend = "livekit";
    result.participant_update_ack = options_.signal_protocol >= 15;
    result.subscription_permissions = true;
    result.receive_demand = true;
    result.room_services = true;
    result.media_encryption = true;
    result.full_rejoin = true;
    return result;
  }

  Result<void> start(const RoomOptions& common, Epoch initial_epoch) override {
    return [&]() -> Result<void> {
      const auto* connection = dynamic_cast<const LiveKitConnectionOptions*>(common.backend.get());
      if (!connection) {
        return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                  ErrorCode::invalid_argument,
                                                  "LiveKit transport requires LiveKitRoomOptions"));
      }
      LiveKitRoomOptions options;
      static_cast<RoomSettings&>(options) = common;
      static_cast<SignalingOptions&>(options) = connection->signaling;
      options.encryption = connection->encryption;
      options.enable_flexfec = connection->enable_flexfec;
      if (options.url.empty() || (options.token.empty() && !options.token_provider)) {
        return Result<void>::failure(
            make_failure(ErrorDomain::protocol, ErrorCode::invalid_argument,
                         "LiveKit requires a URL and token or token provider"));
      }
      epoch_ = initial_epoch;
      detached_epoch_ = initial_epoch;
      if (worker_.joinable())
        return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                                                  "LiveKit transport is already started"));
      if (options.single_peer_connection)
        return Result<void>::failure(
            make_failure(ErrorDomain::protocol, ErrorCode::unsupported,
                         "Single-PC negotiation is not yet available in the encoded room adapter"));
      if (auto valid = livekit::sdk::detail::validate_reconnect(options.reconnect); !valid) {
        return valid;
      }
      if (options.encryption.media && !options.encryption.keys)
        return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                  ErrorCode::invalid_argument,
                                                  "Media E2EE requires a key provider"));
      if (auto attached = services_->attach_wakeup(session_wakeup_); !attached) {
        return attached;
      }
      if (auto configured = services_->configure(options.encryption, options.operation_timeout);
          !configured) {
        return configured;
      }
      options_ = options;
      media_ready_ = std::make_shared<livekit::sdk::detail::ReadySet<BindingToken>>(
          session_wakeup_, options.max_tracks);
      if (options.subscription_permissions) {
        if (auto valid = validate_subscription_permissions(*options.subscription_permissions);
            !valid) {
          return valid;
        }
      }
      permissions_ = options.subscription_permissions;
      worker_done_ = false;
      worker_ = std::jthread([this](std::stop_token stop) {
        run(stop);
        services_->close();
        std::lock_guard lock(mutex_);
        worker_done_ = true;
        commands_.clear();
        changed_.notify_all();
      });
      return Result<void>::success();
    }();
  }

  Result<void> publish(const BindingToken& binding, const TrackDescriptor& descriptor) override {
    return [&]() -> Result<void> {
      auto codec = codec_name(descriptor.format.codec);
      if (!codec) {
        return Result<void>::failure(codec.failure_record());
      }
      if (options_.encryption.media) {
        auto encrypted_codec = livekit::sdk::detail::encryption_codec(*codec);
        if (!encrypted_codec) {
          return Result<void>::failure(encrypted_codec.failure_record());
        }
      }
      if (descriptor.encodings.size() != 1)
        return Result<void>::failure(
            make_failure(ErrorDomain::media, ErrorCode::unsupported,
                         "Encoded native publication currently requires one encoding per track"));
      Command command{Command::publish, binding, descriptor};
      auto wake = media_ready_->watch(binding);
      if (!wake) {
        return Result<void>::failure(wake.failure_record());
      }
      command.media_wakeup = std::move(*wake);
      auto submitted = post(std::move(command));
      if (!submitted) {
        media_ready_->forget(binding);
      }
      return submitted;
    }();
  }

  Result<void> subscribe(const BindingToken& binding, const TrackDescriptor& descriptor,
                         std::shared_ptr<QueueBudget> budget) override {
    return [&]() -> Result<void> {
      if (descriptor.remote_sid.empty())
        return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                  ErrorCode::invalid_argument,
                                                  "Subscription requires a discovered remote_sid"));
      Command command{Command::subscribe, binding, descriptor};
      command.budget = std::move(budget);
      auto wake = media_ready_->watch(binding);
      if (!wake) {
        return Result<void>::failure(wake.failure_record());
      }
      command.media_wakeup = std::move(*wake);
      auto submitted = post(std::move(command));
      if (!submitted) {
        media_ready_->forget(binding);
      }
      return submitted;
    }();
  }

  Result<void> remove(const BindingToken& binding, bool publication,
                      Removed completion = {}) override {
    if (media_ready_) {
      media_ready_->forget(binding);
    }
    if (binding.epoch < detached_epoch_.load()) {
      if (completion) {
        completion({});
      }
      return Result<void>::success();
    }
    Command command{Command::remove, binding, {}};
    command.completion = std::move(completion);
    return post(std::move(command));
  }

  SendResult send(const BindingToken& binding, const EncodedFrame& frame) override {
    std::shared_ptr<Publication> sender;
    {
      std::lock_guard lock(mutex_);
      auto it = publications_.find(binding.id);
      if (it == publications_.end() || it->second->binding != binding)
        return SendResult::dropped;
      sender = it->second;
    }
    if (!sender->ready)
      return SendResult::would_block;
    return sender->audio ? sender->audio->send(frame) : sender->video->send(frame);
  }

  void request_keyframe(const BindingToken& binding) override {
    auto posted = post({Command::keyframe, binding, {}});
    if (!posted) {
      auto failed = event(TransportEventKind::operation_failed, binding, posted.error().message);
      failed.failure = posted.failure_record();
      emit(std::move(failed));
    }
  }

  Result<void> update_participant(ParticipantUpdateRequest request) override {
    Command command{Command::metadata, {{}, request.epoch}, {}};
    command.participant_update = std::move(request);
    return post(std::move(command));
  }

  Result<void> set_muted(MuteRequest request) override {
    Command command{Command::mute, request.binding, {}};
    command.mute_request = std::move(request);
    return post(std::move(command));
  }

  Result<void> set_receive_demand(ReceiveDemandRequest request) override {
    Command command{Command::receive_demand, request.binding, {}};
    command.demand_request = std::move(request);
    return post(std::move(command));
  }

  Result<void> set_subscription_permissions(SubscriptionPermissionsRequest request) override {
    Command command{Command::permissions, {{}, request.epoch}, {}};
    command.permissions_request = std::move(request);
    return post(std::move(command));
  }

  Result<void> attach_notifications(Dispatch dispatch,
                                    std::function<void(Failure)> failure) override {
    return services_->set_dispatcher(
        [dispatch = std::move(dispatch)](auto task) {
          dispatch(std::move(task));
          return Result<void>::success();
        },
        std::move(failure));
  }

 private:
  void deliver_events() {
    std::vector<TransportEvent> result;
    std::vector<std::shared_ptr<Publication>> publications;
    std::vector<std::shared_ptr<Subscription>> subscriptions;
    const auto ready = media_ready_->take();
    {
      std::unique_lock lock(mutex_);
      while (!events_.empty()) {
        result.push_back(std::move(events_.front()));
        events_.pop_front();
      }
      for (const auto& binding : ready) {
        if (binding.publication) {
          const auto found = publications_.find(binding.id);
          if (found != publications_.end() && found->second->binding == binding &&
              found->second->ready) {
            publications.push_back(found->second);
          }
        } else {
          const auto found = subscriptions_.find(binding.id);
          if (found != subscriptions_.end() && found->second->binding == binding &&
              found->second->receiver) {
            subscriptions.push_back(found->second);
          }
        }
      }
    }
    for (const auto& sender : publications) {
      result.push_back(event(TransportEventKind::writable, sender->binding));
      if (!sender->video) {
        continue;
      }
      auto feedback = sender->video->feedback();
      if (feedback.revision == sender->feedback_revision)
        continue;
      sender->feedback_revision = feedback.revision;
      auto value = event(TransportEventKind::feedback, sender->binding);
      value.feedback = std::move(feedback);
      result.push_back(std::move(value));
    }
    for (const auto& receiver : subscriptions) {
      const bool video = receiver->descriptor.kind == MediaKind::video;
      if (video && receiver->receiver->take_keyframe_request())
        request_keyframe(receiver->binding);
      for (auto& unit : receiver->receiver->take_frames()) {
        uint64_t timestamp = unit.timestamp;
        if (receiver->last_timestamp) {
          const auto delta = int32_t(unit.timestamp - *receiver->last_timestamp);
          if (delta <= 0 && video) {
            receiver->waiting_for_keyframe = true;
            request_keyframe(receiver->binding);
            continue;
          }
          if (delta < 0 && uint64_t(-int64_t(delta)) > receiver->timestamp) {
            continue;
          }
          timestamp = delta < 0 ? receiver->timestamp - uint64_t(-int64_t(delta))
                                : receiver->timestamp + uint32_t(delta);
          // Keep the unwrap reference at the newest timestamp. Audio reordering
          // is resolved by the upper-layer jitter buffer, not discarded here.
          if (delta > 0) {
            receiver->timestamp = timestamp;
            receiver->last_timestamp = unit.timestamp;
          }
        } else {
          receiver->timestamp = timestamp;
          receiver->last_timestamp = unit.timestamp;
        }
        if (video && receiver->waiting_for_keyframe && !unit.keyframe)
          continue;
        if (unit.keyframe)
          receiver->waiting_for_keyframe = false;
        EncodedFrame frame;
        frame.buffer = std::move(unit.buffer);
        frame.encoding_id = receiver->descriptor.encodings.front().id;
        frame.epoch = receiver->binding.epoch;
        frame.timestamp = timestamp;
        frame.sequence_number = unit.sequence_number;
        if (unit.sender_capture_time_us) {
          receiver->sender_clock = std::pair{timestamp, *unit.sender_capture_time_us};
        }
        if (receiver->sender_clock) {
          const auto distance = int64_t(timestamp) - int64_t(receiver->sender_clock->first);
          // Extrapolate infrequent absolute-capture extensions in this track's
          // negotiated clock, without guessing a relationship between RTP origins.
          frame.sender_capture_time_us =
              receiver->sender_clock->second +
              distance * 1000000 / receiver->descriptor.format.clock_rate;
        }
        frame.keyframe = unit.keyframe;
        if (!video) {
          frame.duration = opus_duration(frame.buffer.bytes());
          if (frame.duration.count() == 0)
            continue;
        }
        auto value = event(TransportEventKind::frame, receiver->binding);
        value.frame = std::move(frame);
        result.push_back(std::move(value));
      }
    }
    notify_events(std::move(result));
  }

 public:
  void stop() noexcept override {
    worker_.request_stop();
    changed_.notify_all();
    session_wakeup_->signal();
    if (worker_.joinable())
      worker_.join();
    services_->close();
    std::lock_guard lock(mutex_);
    for (auto& [id, value] : publications_)
      value->close();
    for (auto& [id, value] : subscriptions_)
      if (value->receiver)
        value->receiver->close();
    publications_.clear();
    subscriptions_.clear();
    commands_.clear();
    events_.clear();
    if (media_ready_) {
      media_ready_->clear();
    }
  }

 private:
  std::shared_ptr<RoomServiceState> services_ = std::make_shared<RoomServiceState>();
  LiveKitRoomOptions options_;
  std::optional<SubscriptionPermissions>
      permissions_;  // Session-worker intent, retained across full rejoin.
  std::shared_ptr<QueueBudget> budget_;
  std::shared_ptr<livekit::sdk::detail::ReadySet<BindingToken>> media_ready_;
  std::shared_ptr<livekit::sdk::detail::WakeEvent> session_wakeup_ =
      std::make_shared<livekit::sdk::detail::WakeEvent>();
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  bool worker_done_ = true;
  std::deque<Command> commands_;
  std::deque<TransportEvent> events_;
  std::map<TrackId, std::shared_ptr<Publication>> publications_;
  std::map<TrackId, std::shared_ptr<Subscription>> subscriptions_;
  std::atomic<Epoch> detached_epoch_{1};
  Epoch epoch_ = 1;  // worker only; advanced exactly when interrupted is emitted

  Result<void> post(Command command) {
    std::lock_guard lock(mutex_);
    if (worker_done_)
      return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed,
                                                "LiveKit session worker is closed"));
    if (commands_.size() >= 1024)
      return Result<void>::failure(make_failure(ErrorDomain::lifecycle,
                                                ErrorCode::resource_exhausted,
                                                "RTC native control queue exhausted"));
    commands_.push_back(std::move(command));
    changed_.notify_all();
    session_wakeup_->signal();
    return Result<void>::success();
  }

  void emit(TransportEvent value) {
    std::lock_guard lock(mutex_);
    emit_locked(std::move(value));
  }

  void emit_locked(TransportEvent value) {
    if (value.kind == TransportEventKind::operation_failed ||
        value.kind == TransportEventKind::removed) {
      media_ready_->forget(value.binding);
    }
    const auto result =
        enqueue_event(events_, std::move(value), options_.max_events,
                      TransportEventKind::statistics, [](auto& queued, auto& incoming) {
                        // Transport state updates carry snapshots; only replace adjacent ones.
                        if (incoming.kind == TransportEventKind::session_changed &&
                            !queued.empty() && queued.back().kind == incoming.kind &&
                            queued.back().binding.epoch == incoming.binding.epoch) {
                          queued.back() = std::move(incoming);
                          return true;
                        }
                        return false;
                      });
    if (result == EventAdmission::dropped) {
      return;
    }
    if (result == EventAdmission::overflow) {
      events_.clear();
      events_.push_back(
          event(TransportEventKind::terminal_error, {}, "RTC native event queue exhausted"));
      worker_.request_stop();
    }
    changed_.notify_all();
    session_wakeup_->signal();
  }

  void run(std::stop_token stop) noexcept {
    const auto terminal = [this](Failure failure) {
      auto value = event(TransportEventKind::terminal_error, {}, failure->message);
      value.failure = std::move(failure);
      emit(std::move(value));
    };
    SignalRecoveryState recovery;
    recovery.token = options_.token;
    recovery.budget = livekit::sdk::detail::RecoveryBudget(options_.reconnect);
    recovery.initial_deadline = SteadyClock::now() + std::chrono::seconds(options_.connect_timeout);
    bool recovering = false;
    while (!stop.stop_requested()) {
      auto result = session(stop, recovery, recovering);
      if (result || stop.stop_requested()) {
        return;
      }
      const auto& error = result.error();
      if (!recovery.ever_connected) {
        if (options_.regions.initial_failover && error.retryable &&
            (error.domain == ErrorDomain::transport || error.domain == ErrorDomain::signaling) &&
            SteadyClock::now() < recovery.initial_deadline) {
          const auto previous = recovery.regions.active_url;
          recovery.regions.select();
          if (recovery.regions.active_url != previous) {
            continue;
          }
        }
        terminal(result.failure_record());
        return;
      }
      if (!error.retryable || !options_.reconnect.enabled) {
        terminal(result.failure_record());
        return;
      }
      if (!recovering) {
        recovering = true;
        ++epoch_;
        detached_epoch_ = epoch_;
        auto interrupted = event(TransportEventKind::interrupted, {}, error.message);
        interrupted.failure = result.failure_record();
        emit(std::move(interrupted));
      }
      auto attempt = recovery.budget.next_attempt();
      if (!attempt) {
        terminal(attempt.failure_record());
        return;
      }
      recovery.rejoining = true;
      const auto delay = livekit::sdk::detail::reconnect_delay(options_.reconnect, *attempt, 0.5);
      if (!livekit::sdk::detail::wait_reconnect(
              stop, std::min(recovery.budget.deadline(), SteadyClock::now() + delay))) {
        return;
      }
    }
  }

  Result<void> session(std::stop_token stop, SignalRecoveryState& recovery, bool& recovering);
};

Result<void> LiveKitTransport::session(std::stop_token stop, SignalRecoveryState& recovery,
                                       bool& recovering) {
  return [&]() -> Result<void> {
    bool connected = false;
    std::deque<TransportEvent> initial_events;
    Failure initial_event_failure;
    // Do not expose a roster or tracks from an endpoint whose ICE connection
    // never succeeds. A failed initial attempt discards this bounded staging area.
    auto emit = [&](TransportEvent value) {
      if (connected) {
        this->emit(std::move(value));
        return;
      }
      if (initial_events.size() >= options_.max_events) {
        if (!initial_event_failure) {
          initial_event_failure =
              make_failure(ErrorDomain::lifecycle, ErrorCode::resource_exhausted,
                           "Initial room event queue exhausted");
        }
        return;
      }
      initial_events.push_back(std::move(value));
    };
    lk::SignalSession signal(options_, stop, lk::EventEmitter{}, &recovery, session_wakeup_);
    if (auto connected = signal.connect(); !connected) {
      return connected;
    }
    livekit::JoinResponse join;
    while (!stop.stop_requested() && join.participant().sid().empty()) {
      auto joined = [&]() -> Result<void> {
        auto received = signal.receive_until(signal.connect_deadline());
        if (!received) {
          return Result<void>::failure(received.failure_record());
        }
        if (*received) {
          livekit::SignalResponse response;
          if (!response.ParseFromString(**received)) {
            return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                      ErrorCode::malformed_message,
                                                      "Invalid LiveKit join response"));
          }
          if (response.has_leave()) {
            return Result<void>::failure(make_failure(
                ErrorDomain::signaling, ErrorCode::session_closed, "Server rejected join"));
          }
          if (response.has_join()) {
            if (!response.join().alternative_url().empty()) {
              return signal.redirect(response.join().alternative_url());
            }
            join = response.join();
          }
        }
        if (SteadyClock::now() >= signal.connect_deadline()) {
          return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::timeout,
                                                    "LiveKit join timed out", true));
        }
        return Result<void>::success();
      }();
      if (!joined) {
        auto fallback = signal.failover(joined.error());
        if (!fallback) {
          return Result<void>::failure(fallback.failure_record());
        }
        if (!*fallback) {
          return joined;
        }
      }
    }
    if (stop.stop_requested()) {
      return Result<void>::success();
    }
    signal.bind_participant(join.participant().sid());
    signal.configure_client(join.client_configuration());
    PC::RTCConfiguration config;
    for (const auto& ice : join.ice_servers()) {
      PC::IceServer server;
      server.urls.assign(ice.urls().begin(), ice.urls().end());
      server.username = ice.username();
      server.password = ice.credential();
      config.servers.push_back(std::move(server));
    }
    auto relay = livekit::sdk::detail::relay_required(
        false, options_.media_transport != livekit::sdk::MediaTransport::automatic,
        join.client_configuration());
    if (!relay) {
      return Result<void>::failure(relay.failure_record());
    }
    if (*relay) {
      config.type = PC::kRelay;
    }
    if (auto applied =
            livekit::sdk::detail::apply_media_transport(config, options_.media_transport, false);
        !applied) {
      return applied;
    }
    auto runtime = std::make_shared<TransportRuntime>(options_.enable_flexfec);
    if (auto initialized = runtime->status(); !initialized) {
      return initialized;
    }
    NativePeer publisher(*runtime, config, {}, options_.max_events, budget_, session_wakeup_),
        subscriber(*runtime, config, {}, options_.max_events, budget_, session_wakeup_);
    if (auto initialized = publisher.status(); !initialized) {
      return initialized;
    }
    if (auto initialized = subscriber.status(); !initialized) {
      return initialized;
    }
    auto cryptor = std::make_shared<livekit::sdk::detail::MediaCryptorContext>();
    cryptor->options = options_.encryption;
    cryptor->sif.assign(join.sif_trailer().begin(), join.sif_trailer().end());
    const auto session_epoch = epoch_;
    cryptor->report = [this, session_epoch](const std::string& sid, bool ok, int key) {
      auto report =
          event(TransportEventKind::encryption_state_changed, {sid, session_epoch, 0, false});
      report.encryption = EncryptionStateEvent{sid, true, false, ok, key};
      this->emit(std::move(report));
    };
    if (options_.encryption.media)
      subscriber.set_receiver_transform([cryptor](const std::string& sid, MediaKind kind) {
        auto decryptor =
            webrtc::make_ref_counted<livekit::sdk::detail::NativeFrameDecryptor>(cryptor, sid);
        return [decryptor,
                kind](std::span<const uint8_t> bytes) -> std::optional<std::vector<uint8_t>> {
          std::vector<uint8_t> output(bytes.size());
          auto result = decryptor->Decrypt(
              kind == MediaKind::audio ? webrtc::MediaType::AUDIO : webrtc::MediaType::VIDEO, {},
              {}, {bytes.data(), bytes.size()}, {output.data(), output.size()});
          if (result.status == webrtc::FrameDecryptorInterface::Status::kRecoverable)
            return {};
          if (result.status != webrtc::FrameDecryptorInterface::Status::kOk) {
            // The decryptor reports authentication failure; drop this access
            // unit and its dependent chain without unwinding native callbacks.
            return {};
          }
          output.resize(result.bytes_written);
          return output;
        };
      });
    std::map<std::string, std::shared_ptr<Publication>> pending_publications;
    std::map<TrackId, std::shared_ptr<Subscription>> pending_subscriptions;
    auto send_permissions = [&]() -> Result<void> {
      if (!permissions_)
        return Result<void>::success();
      std::map<TrackId, std::string> sids;
      for (const auto& [cid, publication] : pending_publications)
        if (!publication->closed && !publication->sid.empty())
          sids.emplace(publication->binding.id, publication->sid);
      livekit::SignalRequest request;
      auto wire = subscription_permissions_wire(*permissions_, sids);
      if (!wire) {
        return Result<void>::failure(wire.failure_record());
      }
      *request.mutable_subscription_permission() = std::move(*wire);
      return signal.send(request);
    };
    std::map<std::string, std::set<std::string>> participants;
    std::map<std::string, TrackDescriptor> remote_tracks;
    auto created_roster = SessionRoster::create(join, epoch_);
    if (!created_roster) {
      return Result<void>::failure(created_roster.failure_record());
    }
    auto session_roster = std::move(*created_roster);
    auto emit_session = [&] {
      auto value = event(TransportEventKind::session_changed, {{}, epoch_, 0, false});
      value.session = session_roster.snapshot();
      emit(std::move(value));
    };
    bool publisher_dirty = true, offer_pending = false;
    bool stats_pending[2] = {};
    std::optional<Result<TransportStatistics>> completed_stats[2];
    auto next_stats = SteadyClock::now() + options_.stats_interval;

    struct Negotiation {
      bool local_pending = false, remote_pending = false;
      std::optional<Result<NativeDescription>> local;
      std::optional<Result<void>> remote;
      std::deque<livekit::SessionDescription> offers;
      std::vector<NativeIceCandidate> ice;
      bool remote_set = false;
      uint32_t id = 0;
    } publish_sdp, subscribe_sdp;

    // Destroy queued closures before their session-owned result slots. A late
    // native callback sees an expired weak queue and cancels its continuation.
    auto completions = std::make_shared<CompletionTasks>();
    completions->wakeup = session_wakeup_;
    auto on_complete = [weak = std::weak_ptr(completions)](auto start, auto& destination,
                                                           bool& pending) {
      pending = true;
      start([weak, &destination, &pending](auto result) mutable {
        if (auto queue = weak.lock()) {
          queue->post([&destination, &pending, result = std::move(result)]() mutable {
            destination.emplace(std::move(result));
            pending = false;
          });
        }
      });
    };

    auto clean = [&] {
      services_->interrupt();
      {
        std::lock_guard lock(mutex_);
        for (auto& [id, value] : publications_)
          value->close();
        for (auto& [id, value] : subscriptions_)
          if (value->receiver)
            value->receiver->close();
        publications_.clear();
        subscriptions_.clear();
        media_ready_->clear();
      }
      for (auto& [cid, value] : pending_publications)
        value->close();
      publisher.close();
      subscriber.close();
      livekit::SignalRequest leave;
      leave.mutable_leave();
      (void)signal.send(leave);  // Best effort during unconditional socket teardown.
      signal.close();
    };
    struct Cleanup {
      decltype(clean)& stop;
      ~Cleanup() {
        stop();
      }
    } cleanup{clean};
    const auto remove_remote = [&](const std::string& sid) {
      const auto found = remote_tracks.find(sid);
      if (found != remote_tracks.end()) {
        emit(event(TransportEventKind::remote_track_removed, {found->second.id, epoch_, 0, false}));
        for (auto it = pending_subscriptions.begin(); it != pending_subscriptions.end();) {
          const auto subscription = it->second;
          if (subscription->descriptor.remote_sid != sid) {
            ++it;
            continue;
          }
          if (subscription->receiver)
            subscription->receiver->close();
          {
            std::lock_guard lock(mutex_);
            subscriptions_.erase(it->first);
          }
          emit(event(TransportEventKind::removed, subscription->binding,
                     "Remote publication removed"));
          it = pending_subscriptions.erase(it);
        }
        remote_tracks.erase(found);
      }
    };
    auto roster = [&](const livekit::ParticipantInfo& participant) -> Result<void> {
      const auto previous = session_roster.snapshot();
      auto updated = session_roster.participant(participant);
      if (!updated) {
        return Result<void>::failure(updated.failure_record());
      }
      if (!*updated) {
        return Result<void>::success();
      }
      const auto current_roster = session_roster.snapshot();
      for (const auto& [sid, old] : previous->remote)
        if (!current_roster->remote.contains(sid)) {
          if (sid != participant.sid()) {
            livekit::ParticipantInfo departed;
            departed.set_sid(sid);
            departed.set_identity(old.identity);
            departed.set_state(3);
            services_->participant(departed);
            cryptor->participant(departed);
          }
          if (auto it = participants.find(sid); it != participants.end()) {
            for (const auto& track : it->second)
              remove_remote(track);
            participants.erase(it);
          }
        }
      emit_session();
      services_->participant(participant);
      cryptor->participant(participant);
      // Local publication announcements are room state, never remote discovery.
      if (participant.sid() == join.participant().sid()) {
        return Result<void>::success();
      }
      std::set<std::string> current;
      if (participant.state() != 3)
        for (const auto& track : participant.tracks()) {
          if (track.type() != livekit::AUDIO && track.type() != livekit::VIDEO)
            continue;
          current.insert(track.sid());
          if (remote_tracks.contains(track.sid()))
            continue;
          TrackDescriptor descriptor;
          descriptor.id = "remote/" + track.sid();
          descriptor.remote_sid = track.sid();
          descriptor.name = track.name();
          descriptor.stream_group = track.stream();
          descriptor.source_id = participant.identity();
          descriptor.source = static_cast<PublicationSource>(track.source());
          descriptor.kind = track.type() == livekit::AUDIO ? MediaKind::audio : MediaKind::video;
          if (descriptor.kind == MediaKind::audio) {
            descriptor.format.codec = Codec::opus;
            descriptor.format.clock_rate = 48000;
            descriptor.format.channels = 2;
          } else {
            auto mime = track.mime_type();
            std::transform(mime.begin(), mime.end(), mime.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (mime == "video/vp8")
              descriptor.format.codec = Codec::vp8;
            else if (mime == "video/h264")
              descriptor.format.codec = Codec::h264;
            else if (mime == "video/h265")
              descriptor.format.codec = Codec::h265;
            else
              continue;  // Unsupported/unknown codecs cannot be encoded endpoints.
            descriptor.format.width = track.width();
            descriptor.format.height = track.height();
          }
          remote_tracks.emplace(track.sid(), descriptor);
          auto value =
              event(TransportEventKind::remote_track_added, {descriptor.id, epoch_, 0, false});
          value.server_sid = track.sid();
          value.descriptor = descriptor;
          emit(std::move(value));
        }
      for (const auto& sid : participants[participant.sid()])
        if (!current.contains(sid))
          remove_remote(sid);
      if (participant.state() != 3)
        participants[participant.sid()] = std::move(current);
      else
        participants.erase(participant.sid());
      return Result<void>::success();
    };
    auto send_receive_demand = [&](const std::string& sid,
                                   const ReceiveDemand& demand) -> Result<void> {
      livekit::SignalRequest request;
      auto* settings = request.mutable_track_setting();
      settings->add_track_sids(sid);
      settings->set_disabled(demand.disabled);
      settings->set_quality(static_cast<int>(demand.quality));
      settings->set_width(demand.width);
      settings->set_height(demand.height);
      settings->set_fps(demand.fps);
      settings->set_priority(demand.priority);
      return signal.send(request);
    };
    auto subscribe_request = [&](const std::string& sid, bool enable) -> Result<void> {
      livekit::SignalRequest request;
      request.mutable_subscription()->set_subscribe(enable);
      request.mutable_subscription()->add_track_sids(sid);
      return signal.send(request);
    };
    auto local_description = [&](const NativeDescription& source,
                                 livekit::SessionDescription* destination) {
      destination->set_type(source.type == webrtc::SdpType::kOffer ? "offer" : "answer");
      destination->set_sdp(source.sdp);
    };
    auto process_sdp = [&](NativePeer& peer, Negotiation& negotiation,
                           bool publishing) -> Result<void> {
      if (negotiation.remote && !negotiation.remote_pending) {
        auto result = std::move(*negotiation.remote);
        negotiation.remote.reset();
        if (!result) {
          return result;
        }
        negotiation.remote_set = true;
        for (const auto& ice : negotiation.ice) {
          auto added = peer.add_ice(ice);
          if (!added) {
            return added;
          }
        }
        negotiation.ice.clear();
        if (!publishing) {
          on_complete([&](auto done) { return peer.create_local(false, false, std::move(done)); },
                      negotiation.local, negotiation.local_pending);
        } else {
          offer_pending = false;
          peer.refresh_receivers();
        }
      }
      if (negotiation.local && !negotiation.local_pending) {
        auto result = std::move(*negotiation.local);
        negotiation.local.reset();
        if (!result) {
          return Result<void>::failure(result.failure_record());
        }
        const auto description = std::move(*result);
        livekit::SignalRequest request;
        auto* payload = publishing ? request.mutable_offer() : request.mutable_answer();
        local_description(description, payload);
        payload->set_id(negotiation.id);
        if (auto sent = signal.send(request); !sent) {
          return sent;
        }
        if (!publishing)
          peer.refresh_receivers();
      }
      if (!publishing && !negotiation.remote_pending && !negotiation.local_pending &&
          !negotiation.offers.empty()) {
        auto description = std::move(negotiation.offers.front());
        negotiation.offers.pop_front();
        negotiation.id = description.id();
        std::map<std::string, std::string> mids(description.mid_to_track_id().begin(),
                                                description.mid_to_track_id().end());
        on_complete(
            [&](auto done) {
              return peer.set_remote({webrtc::SdpType::kOffer, description.sdp()}, std::move(mids),
                                     std::move(done));
            },
            negotiation.remote, negotiation.remote_pending);
      }
      for (const auto& ice : peer.take_ice()) {
        livekit::SignalRequest request;
        request.mutable_trickle()->set_target(publishing ? livekit::PUBLISHER
                                                         : livekit::SUBSCRIBER);
        request.mutable_trickle()->set_candidateinit(Json{
            {"candidate", ice.sdp},
            {"sdpMid", ice.mid},
            {"sdpMLineIndex", ice.mline_index}}.dump());
        if (auto sent = signal.send(request); !sent) {
          return sent;
        }
      }
      return Result<void>::success();
    };
    {
      // Install a restrictive policy before accepting any publication commands.
      // Stable local IDs are resolved again as new server SIDs are acknowledged.
      if (auto applied = send_permissions(); !applied) {
        return applied;
      }
      if (auto applied = publisher.create_data_channel("_reliable", true); !applied) {
        return applied;
      }
      if (auto applied = publisher.create_data_channel("_lossy", false); !applied) {
        return applied;
      }
      if (auto applied = publisher.create_data_channel("_data_track", false); !applied) {
        return applied;
      }
      emit_session();
      for (const auto& participant : join.other_participants())
        if (auto updated = roster(participant); !updated) {
          return updated;
        }
      auto last_ping = SteadyClock::now(), last_pong = last_ping;
      const auto deadline = std::min(signal.operation_deadline(), signal.connect_deadline());
      while (!stop.stop_requested()) {
        if (initial_event_failure) {
          return Result<void>::failure(initial_event_failure);
        }
        const auto observed = session_wakeup_->revision();
        if (auto budget = recovery.budget.check(); !budget) {
          return budget;
        }
        if (auto dispatched = completions->run(); !dispatched) {
          return dispatched;
        }
        std::deque<Command> commands;
        {
          std::lock_guard lock(mutex_);
          commands.swap(commands_);
        }
        for (const auto& command : commands) {
          if (command.binding.epoch != epoch_) {
            // Previous-session peers have already been closed by session cleanup.
            if (command.completion)
              command.completion({});
            continue;
          }
          {
            auto submitted = [&]() -> Result<void> {
              if (command.kind == Command::metadata) {
                const auto& operation = *command.participant_update;
                if (operation.stop.stop_requested() || SteadyClock::now() >= operation.deadline) {
                  auto result = event(TransportEventKind::control_completed, command.binding,
                                      "Participant update cancelled or expired before send");
                  result.operation_id = operation.id;
                  result.control_error =
                      operation.stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::timeout;
                  emit(std::move(result));
                  return Result<void>::success();
                }
                livekit::SignalRequest request;
                auto* update = request.mutable_update_metadata();
                update->set_request_id(operation.id);
                update->set_name(operation.update.name);
                update->set_metadata(operation.update.metadata);
                for (const auto& [key, value] : operation.update.attributes)
                  (*update->mutable_attributes())[key] = value;
                if (auto sent = signal.send(request); !sent) {
                  return sent;
                }
              } else if (command.kind == Command::permissions) {
                const auto& operation = *command.permissions_request;
                auto result = event(TransportEventKind::control_completed, command.binding);
                result.operation_id = operation.id;
                if (operation.stop.stop_requested() || SteadyClock::now() >= operation.deadline) {
                  result.control_error =
                      operation.stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::timeout;
                  result.error =
                      "Permission update cancelled or expired before signaling submission";
                } else {
                  permissions_ = operation.permissions;
                  if (auto applied = send_permissions(); !applied) {
                    return applied;
                  }
                }
                emit(std::move(result));
              } else if (command.kind == Command::receive_demand) {
                const auto& operation = *command.demand_request;
                auto result = event(TransportEventKind::control_completed, command.binding);
                result.operation_id = operation.id;
                const auto found = pending_subscriptions.find(command.binding.id);
                if (operation.stop.stop_requested() || SteadyClock::now() >= operation.deadline) {
                  result.control_error =
                      operation.stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::timeout;
                  result.error = "Receive demand cancelled or expired before signaling submission";
                } else if (found == pending_subscriptions.end() ||
                           found->second->binding != command.binding) {
                  result.control_error = ErrorCode::stale_handle;
                  result.error = "Subscription no longer exists";
                } else {
                  if (auto applied = send_receive_demand(found->second->descriptor.remote_sid,
                                                         operation.demand);
                      !applied) {
                    return applied;
                  }
                }
                emit(std::move(result));
              } else if (command.kind == Command::mute) {
                const auto& operation = *command.mute_request;
                auto result = event(TransportEventKind::control_completed, command.binding);
                result.operation_id = operation.id;
                if (operation.stop.stop_requested() || SteadyClock::now() >= operation.deadline) {
                  result.control_error =
                      operation.stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::timeout;
                  result.error = "Mute cancelled or expired before native application";
                } else {
                  const auto found = std::find_if(
                      pending_publications.begin(), pending_publications.end(),
                      [&](const auto& item) { return item.second->binding == command.binding; });
                  if (found == pending_publications.end() || !found->second->ready ||
                      found->second->sid.empty()) {
                    result.control_error = ErrorCode::stale_handle;
                    result.error = "Mute publication binding is no longer ready";
                  } else {
                    const auto publication = found->second;
                    if (publication->audio)
                      publication->audio->set_muted(operation.muted);
                    if (publication->video)
                      publication->video->set_muted(operation.muted);
                    livekit::SignalRequest request;
                    auto* mute = request.mutable_mute();
                    mute->set_sid(publication->sid);
                    mute->set_muted(operation.muted);
                    if (auto sent = signal.send(request); !sent) {
                      return sent;
                    }
                    if (session_roster.local_muted(publication->sid, operation.muted))
                      emit_session();
                  }
                }
                emit(std::move(result));
              } else if (command.kind == Command::remove) {
                {
                  std::lock_guard lock(mutex_);
                  if (command.binding.publication) {
                    auto found = publications_.find(command.binding.id);
                    if (found != publications_.end() && found->second->binding == command.binding) {
                      found->second->close();
                      publications_.erase(found);
                    }
                  } else {
                    auto found = subscriptions_.find(command.binding.id);
                    if (found != subscriptions_.end() &&
                        found->second->binding == command.binding) {
                      if (found->second->receiver)
                        found->second->receiver->close();
                      subscriptions_.erase(found);
                    }
                  }
                }
                if (command.binding.publication) {
                  for (auto it = pending_publications.begin(); it != pending_publications.end();) {
                    auto value = it->second;
                    if (value->binding != command.binding) {
                      ++it;
                      continue;
                    }
                    value->close();
                    publisher.pc->RemoveTrackOrError(value->sender);
                    auto removed = session_roster.local_unpublished(value->sid);
                    if (!removed) {
                      return Result<void>::failure(removed.failure_record());
                    }
                    if (*removed)
                      emit_session();
                    it = pending_publications.erase(it);
                    publisher_dirty = true;
                  }
                  publisher_dirty = true;
                  if (auto applied = send_permissions(); !applied) {
                    return applied;
                  }
                } else {
                  auto found = pending_subscriptions.find(command.binding.id);
                  if (found != pending_subscriptions.end() &&
                      found->second->binding == command.binding) {
                    if (auto applied =
                            subscribe_request(found->second->descriptor.remote_sid, false);
                        !applied) {
                      return applied;
                    }
                    pending_subscriptions.erase(found);
                  }
                }
                if (command.completion)
                  command.completion({});
                emit(event(TransportEventKind::removed, command.binding));
              } else if (command.kind == Command::keyframe) {
                auto found = pending_subscriptions.find(command.binding.id);
                if (found != pending_subscriptions.end() &&
                    found->second->binding == command.binding && found->second->receiver) {
                  if (auto requested =
                          subscriber.request_keyframe(found->second->descriptor.remote_sid);
                      !requested) {
                    return requested;
                  }
                }
              } else if (command.kind == Command::subscribe) {
                if (!remote_tracks.contains(command.descriptor.remote_sid))
                  return Result<void>::failure(make_failure(ErrorDomain::media,
                                                            ErrorCode::invalid_argument,
                                                            "Remote publication no longer exists"));
                auto subscription = std::make_shared<Subscription>();
                subscription->binding = command.binding;
                subscription->descriptor = command.descriptor;
                subscription->budget = command.budget;
                subscription->media_wakeup = command.media_wakeup;
                for (const auto& [id, existing] : pending_subscriptions)
                  if (existing->descriptor.remote_sid == command.descriptor.remote_sid)
                    return Result<void>::failure(make_failure(
                        ErrorDomain::media, ErrorCode::invalid_argument,
                        "Remote track already has a subscription; fan out in the graph"));
                subscriber.reset_receiver(command.descriptor.remote_sid);
                pending_subscriptions[command.binding.id] = subscription;
                if (auto applied = subscribe_request(command.descriptor.remote_sid, true);
                    !applied) {
                  return applied;
                }
                if (auto applied = send_receive_demand(command.descriptor.remote_sid,
                                                       command.descriptor.receive_demand);
                    !applied) {
                  return applied;
                }
              } else if (command.kind == Command::publish) {
                auto codec = codec_name(command.descriptor.format.codec);
                if (!codec) {
                  return Result<void>::failure(codec.failure_record());
                }
                auto publication = std::make_shared<Publication>();
                publication->runtime = runtime;
                publication->binding = command.binding;
                publication->descriptor = command.descriptor;
                publication->cid = "encoded-" + std::to_string(epoch_) + "-" +
                                   std::to_string(command.binding.incarnation);
                publication->pc = publisher.pc;
                webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
                if (command.descriptor.kind == MediaKind::audio) {
                  publication->audio = webrtc::make_ref_counted<EncodedAudioSender>(
                      command.binding, command.descriptor.queue,
                      command.descriptor.encodings.front().id, command.media_wakeup);
                  if (auto initialized = publication->audio->status(); !initialized) {
                    return initialized;
                  }
                  publication->audio->set_muted(command.descriptor.muted);
                  track = runtime->factory->CreateAudioTrack(publication->cid, nullptr);
                } else {
                  publication->video = runtime->signaling->BlockingCall([&] {
                    return webrtc::make_ref_counted<EncodedVideoSender>(
                        command.binding, command.descriptor, command.media_wakeup);
                  });
                  if (auto initialized = publication->video->status(); !initialized) {
                    return initialized;
                  }
                  track = runtime->factory->CreateVideoTrack(publication->video, publication->cid);
                }
                webrtc::RtpTransceiverInit init;
                init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
                init.stream_ids = {command.descriptor.stream_group.empty()
                                       ? "livekit"
                                       : command.descriptor.stream_group};
                webrtc::RtpEncodingParameters encoding;
                encoding.active = command.descriptor.encodings.front().active;
                encoding.max_bitrate_bps =
                    int(command.descriptor.encodings.front().max_bitrate_bps);
                init.send_encodings.push_back(encoding);
                auto added = publisher.pc->AddTransceiver(track, init);
                if (!added.ok())
                  return Result<void>::failure(make_failure(
                      ErrorDomain::transport, ErrorCode::request_failed, added.error().message()));
                publication->sender = added.value()->sender();
                struct PublicationRollback {
                  std::shared_ptr<Publication> publication;
                  bool committed = false;
                  ~PublicationRollback() {
                    if (!committed) {
                      publication->close();
                      publication->pc->RemoveTrackOrError(publication->sender);
                    }
                  }
                } rollback{publication};
                {
                  if (publication->audio)
                    publication->sender->SetEncoderToPacketizerFrameTransformer(publication->audio);
                  if (options_.encryption.media) {
                    auto encrypted_codec = livekit::sdk::detail::encryption_codec(*codec);
                    if (!encrypted_codec) {
                      return Result<void>::failure(encrypted_codec.failure_record());
                    }
                    publication->sender->SetFrameEncryptor(
                        webrtc::make_ref_counted<livekit::sdk::detail::NativeFrameEncryptor>(
                            options_.encryption.keys, *encrypted_codec,
                            [this, binding = command.binding](bool ok, int key) {
                              auto report =
                                  event(TransportEventKind::encryption_state_changed, binding);
                              report.encryption =
                                  EncryptionStateEvent{binding.id, true, true, ok, key};
                              this->emit(std::move(report));
                            }));
                  }
                  const auto kind = command.descriptor.kind == MediaKind::audio
                                        ? webrtc::MediaType::AUDIO
                                        : webrtc::MediaType::VIDEO;
                  auto selected = select_transport_codecs(
                      runtime->factory->GetRtpSenderCapabilities(kind).codecs, *codec,
                      command.descriptor.format.fmtp, options_.enable_flexfec);
                  if (selected.empty())
                    return Result<void>::failure(
                        make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                     "Requested encoded codec/fmtp is unavailable"));
                  const auto selected_codec = added.value()->SetCodecPreferences(selected);
                  if (!selected_codec.ok())
                    return Result<void>::failure(make_failure(ErrorDomain::transport,
                                                              ErrorCode::request_failed,
                                                              selected_codec.message()));
                  livekit::SignalRequest request;
                  auto* add = request.mutable_add_track();
                  add->set_cid(publication->cid);
                  if (options_.encryption.media)
                    add->set_encryption(livekit::Encryption::GCM);
                  add->set_name(command.descriptor.name.empty() ? command.descriptor.id
                                                                : command.descriptor.name);
                  add->set_muted(command.descriptor.muted);
                  add->set_type(publication->audio ? livekit::AUDIO : livekit::VIDEO);
                  add->set_source(static_cast<livekit::TrackSource>(command.descriptor.source));
                  add->set_stream(init.stream_ids.front());
                  add->set_disable_red(true);
                  if (publication->audio)
                    add->set_stereo(command.descriptor.format.channels == 2);
                  else {
                    add->set_width(command.descriptor.format.width);
                    add->set_height(command.descriptor.format.height);
                    auto* layer = add->add_layers();
                    layer->set_quality(2);
                    layer->set_width(command.descriptor.format.width);
                    layer->set_height(command.descriptor.format.height);
                    layer->set_bitrate(command.descriptor.encodings.front().max_bitrate_bps);
                    auto* simulcast_codec = add->add_simulcast_codecs();
                    auto name = *codec;
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    simulcast_codec->set_codec(name);
                    simulcast_codec->set_cid(publication->cid);
                    *simulcast_codec->mutable_layers() = add->layers();
                  }
                  if (auto sent = signal.send(request); !sent) {
                    return sent;
                  }
                  pending_publications[publication->cid] = publication;
                  {
                    std::lock_guard lock(mutex_);
                    publications_[command.binding.id] = publication;
                  }
                  publisher_dirty = true;
                  rollback.committed = true;
                }
              }
              return Result<void>::success();
            }();
            if (!submitted) {
              // Removal has already detached native tracks; a bookkeeping failure
              // must unwind the session instead of leaving a partial publication.
              if (submitted.error().domain == ErrorDomain::signaling ||
                  command.kind == Command::remove || command.kind == Command::metadata ||
                  command.kind == Command::mute || command.kind == Command::receive_demand ||
                  command.kind == Command::permissions) {
                return submitted;
              }
              if (command.completion) {
                command.completion(submitted.failure_record());
              }
              auto failed = event(TransportEventKind::operation_failed, command.binding,
                                  submitted.error().message);
              failed.failure = submitted.failure_record();
              emit(std::move(failed));
            }
          }
        }
        if (auto processed = process_sdp(publisher, publish_sdp, true); !processed) {
          return processed;
        }
        if (auto processed = process_sdp(subscriber, subscribe_sdp, false); !processed) {
          return processed;
        }
        if (publisher_dirty && !offer_pending) {
          publisher_dirty = false;
          offer_pending = true;
          on_complete(
              [&](auto done) { return publisher.create_local(true, false, std::move(done)); },
              publish_sdp.local, publish_sdp.local_pending);
        }
        if (publisher.failed || subscriber.failed)
          return Result<void>::failure(make_failure(ErrorDomain::transport,
                                                    ErrorCode::connection_lost,
                                                    "Native peer transport failed", true));
        if (initial_event_failure) {
          return Result<void>::failure(initial_event_failure);
        }
        if (!connected && (join.subscriber_primary() ? subscriber.connected.load()
                                                     : publisher.connected.load())) {
          connected = true;
          recovering = false;
          recovery.ever_connected = true;
          recovery.rejoining = false;
          recovery.budget.reset();
          if (auto started = services_->connect(); !started) {
            return started;
          }
          for (auto& value : initial_events)
            emit(std::move(value));
          initial_events.clear();
          emit(event(TransportEventKind::connected));
        }
        if (!connected && SteadyClock::now() >= deadline)
          return Result<void>::failure(make_failure(ErrorDomain::transport, ErrorCode::timeout,
                                                    "Native room transport establishment timed out",
                                                    true));
        if (connected && options_.stats_interval.count()) {
          for (int target = 0; target < 2; ++target)
            if (completed_stats[target] && !stats_pending[target]) {
              auto result = std::move(*completed_stats[target]);
              completed_stats[target].reset();
              if (result) {
                auto report = std::move(*result);
                report.target = target;
                auto value = event(TransportEventKind::statistics, {{}, epoch_, 0, false});
                if (target == 0)
                  for (const auto& [cid, publication] : pending_publications)
                    if (!publication->sid.empty())
                      report.publication_tracks[cid] = publication->sid;
                value.statistics = std::make_shared<const TransportStatistics>(std::move(report));
                emit(std::move(value));
              }  // A failed diagnostics request does not interrupt media.
            }
          if (SteadyClock::now() >= next_stats) {
            if (!stats_pending[0]) {
              on_complete([&](auto done) { return publisher.statistics(std::move(done)); },
                          completed_stats[0], stats_pending[0]);
            }
            if (!stats_pending[1]) {
              on_complete([&](auto done) { return subscriber.statistics(std::move(done)); },
                          completed_stats[1], stats_pending[1]);
            }
            next_stats = SteadyClock::now() + options_.stats_interval;
          }
        }
        for (auto& [cid, publication] : pending_publications) {
          if (publication->ready || publication->closed || publication->sid.empty() ||
              !publisher.connected || offer_pending)
            continue;
          if (publication->audio) {
            const auto parameters = publication->sender->GetParameters();
            if (parameters.encodings.empty() || !parameters.encodings.front().ssrc)
              continue;
            const auto codec = std::find_if(parameters.codecs.begin(), parameters.codecs.end(),
                                            [](const auto& item) { return item.name == "opus"; });
            if (codec == parameters.codecs.end())
              continue;
            auto configured = publication->audio->configure(*parameters.encodings.front().ssrc,
                                                            uint8_t(codec->payload_type));
            if (!configured) {
              return configured;
            }
          }
          publication->ready = true;
          media_ready_->mark(publication->binding);
          auto value = event(TransportEventKind::published, publication->binding);
          value.server_sid = publication->sid;
          emit(std::move(value));
        }
        const auto receivers = subscriber.remote_tracks();
        subscriber.take_track_events();
        for (auto& [id, subscription] : pending_subscriptions) {
          const auto found = receivers.find(subscription->descriptor.remote_sid);
          if (found == receivers.end() || subscription->receiver == found->second.output)
            continue;
          // Publish a new routing snapshot; the session worker owns timestamp state.
          auto bound = std::make_shared<Subscription>();
          bound->binding = subscription->binding;
          bound->descriptor = subscription->descriptor;
          bound->receiver = found->second.output;
          bound->budget = subscription->budget;
          bound->media_wakeup = subscription->media_wakeup;
          if (auto configured =
                  bound->receiver->configure_queue(bound->descriptor.queue, bound->budget);
              !configured) {
            return configured;
          }
          bound->receiver->attach_wakeup(bound->media_wakeup);
          subscription = bound;
          auto value = event(TransportEventKind::subscribed, bound->binding);
          value.server_sid = bound->descriptor.remote_sid;
          {
            std::lock_guard lock(mutex_);
            subscriptions_[id] = bound;
            media_ready_->mark(bound->binding);
            emit_locked(std::move(value));
          }
        }
        if (connected) {
          const auto receive_data = [&](const auto& message) -> Result<void> {
            if (message.label == "_reliable" || message.label == "_lossy") {
              livekit::DataPacket packet;
              if (packet.ParseFromArray(message.bytes.data(), int(message.bytes.size())) &&
                  packet.has_speaker() &&
                  session_roster.speakers(packet.speaker().speakers(), true))
                emit_session();
            }
            return services_->receive(message.label, message.bytes);
          };
          for (auto& message : subscriber.take_data()) {
            if (auto received = receive_data(message); !received) {
              return received;
            }
          }
          for (auto& message : publisher.take_data()) {
            if (auto received = receive_data(message); !received) {
              return received;
            }
          }
          auto pumped = services_->pump(
              [&](const std::string& label, std::span<const uint8_t> bytes) {
                return publisher.send_data(label, bytes);
              },
              [&](const livekit::SignalRequest& request) { return signal.send(request); },
              [&] { return publisher.data_ready("_reliable"); });
          if (!pumped) {
            return pumped;
          }
        }
        const auto now = SteadyClock::now();
        if (now - last_ping >= std::chrono::seconds(std::max(1, join.ping_interval()))) {
          livekit::SignalRequest request;
          request.mutable_ping_req()->set_timestamp(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          if (auto sent = signal.send(request); !sent) {
            return sent;
          }
          last_ping = now;
        }
        if (join.ping_timeout() > 0 && now - last_pong > std::chrono::seconds(join.ping_timeout()))
          return Result<void>::failure(make_failure(ErrorDomain::signaling,
                                                    ErrorCode::connection_lost,
                                                    "LiveKit heartbeat timed out", true));
        deliver_events();
        auto bytes = signal.take_message();
        if (!bytes) {
          return Result<void>::failure(bytes.failure_record());
        }
        if (!*bytes) {
          auto next = last_ping + std::chrono::seconds(std::max(1, join.ping_interval()));
          if (join.ping_timeout() > 0) {
            next = std::min(next, last_pong + std::chrono::seconds(join.ping_timeout()));
          }
          if (!connected) {
            next = std::min(next, deadline);
          } else if (options_.stats_interval.count()) {
            next = std::min(next, next_stats);
          }
          if (const auto data_deadline = services_->next_data_deadline()) {
            next = std::min(next, *data_deadline);
          }
          session_wakeup_->wait(observed, stop, next);
          continue;
        }
        livekit::SignalResponse response;
        if (!response.ParseFromString(**bytes))
          return Result<void>::failure(make_failure(
              ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid LiveKit response"));
        if (auto received = services_->receive_signal(response); !received) {
          return received;
        }
        if (response.has_offer()) {
          if (subscribe_sdp.offers.size() >= 32)
            return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                      ErrorCode::resource_exhausted,
                                                      "Too many pending SDP offers"));
          subscribe_sdp.offers.push_back(response.offer());
        } else if (response.has_answer()) {
          if (!offer_pending || publish_sdp.remote_pending || publish_sdp.remote)
            return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                      ErrorCode::malformed_message,
                                                      "Unexpected publisher SDP answer"));
          on_complete(
              [&](auto done) {
                return publisher.set_remote({webrtc::SdpType::kAnswer, response.answer().sdp()}, {},
                                            std::move(done));
              },
              publish_sdp.remote, publish_sdp.remote_pending);
        } else if (response.has_trickle()) {
          if (response.trickle().candidateinit().empty())
            continue;
          const auto json = Json::parse(response.trickle().candidateinit(), nullptr, false);
          if (!json.is_object() || (json.contains("sdpMid") && !json["sdpMid"].is_string()) ||
              (json.contains("sdpMLineIndex") &&
               (!json["sdpMLineIndex"].is_number_integer() || json["sdpMLineIndex"] < 0 ||
                json["sdpMLineIndex"] > 65535)) ||
              (json.contains("candidate") && !json["candidate"].is_string())) {
            return Result<void>::failure(make_failure(
                ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid ICE candidate JSON"));
          }
          NativeIceCandidate candidate{json.value("sdpMid", ""), json.value("sdpMLineIndex", 0),
                                       json.value("candidate", "")};
          const bool publishing = response.trickle().target() == livekit::PUBLISHER;
          auto& negotiation = publishing ? publish_sdp : subscribe_sdp;
          if (negotiation.remote_set) {
            if (auto added = (publishing ? publisher : subscriber).add_ice(candidate); !added) {
              return added;
            }
          } else {
            if (negotiation.ice.size() >= 1024) {
              return Result<void>::failure(make_failure(ErrorDomain::protocol,
                                                        ErrorCode::resource_exhausted,
                                                        "Pending remote ICE overflow"));
            }
            negotiation.ice.push_back(std::move(candidate));
          }
        } else if (response.has_track_published()) {
          auto found = pending_publications.find(response.track_published().cid());
          if (found != pending_publications.end()) {
            const auto publication = found->second;
            publication->sid = response.track_published().track().sid();
            if (session_roster.local_publication(response.track_published().track()))
              emit_session();
            else {
              // An unpublish notification can precede its delayed AddTrack ACK.
              publication->close();
              publisher.pc->RemoveTrackOrError(publication->sender);
              {
                std::lock_guard lock(mutex_);
                const auto active = publications_.find(publication->binding.id);
                if (active != publications_.end() && active->second == publication)
                  publications_.erase(active);
              }
              emit(event(TransportEventKind::removed, publication->binding,
                         "Publication removed before acknowledgement"));
              pending_publications.erase(found);
              publisher_dirty = true;
            }
            if (auto applied = send_permissions(); !applied) {
              return applied;
            }
          }
        } else if (response.has_update()) {
          for (const auto& participant : response.update().participants())
            if (auto updated = roster(participant); !updated) {
              return updated;
            }
        } else if (response.has_room_update()) {
          if (auto updated = session_roster.room(response.room_update().room()); !updated) {
            return updated;
          }
          emit_session();
        } else if (response.has_speakers_changed()) {
          if (session_roster.speakers(response.speakers_changed().speakers()))
            emit_session();
        } else if (response.has_subscribed_quality_update()) {
          const auto& update = response.subscribed_quality_update();
          for (const auto& [cid, publication] : pending_publications) {
            if (!publication->video || !publication->descriptor.follow_subscriber_demand ||
                publication->sid != update.track_sid())
              continue;
            auto codec_result = codec_name(publication->descriptor.format.codec);
            if (!codec_result) {
              return Result<void>::failure(codec_result.failure_record());
            }
            auto codec = std::move(*codec_result);
            std::transform(codec.begin(), codec.end(), codec.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (auto enabled = livekit::sdk::detail::subscribed_video_enabled(update, codec))
              publication->video->set_demand(*enabled);
          }
        } else if (response.has_connection_quality()) {
          if (session_roster.quality(response.connection_quality()))
            emit_session();
        } else if (response.has_mute()) {
          for (const auto& [cid, publication] : pending_publications)
            if (publication->sid == response.mute().sid()) {
              if (publication->audio)
                publication->audio->set_muted(response.mute().muted());
              if (publication->video)
                publication->video->set_muted(response.mute().muted());
              auto changed = event(TransportEventKind::mute_changed, publication->binding);
              changed.muted = response.mute().muted();
              emit(std::move(changed));
            }
          if (session_roster.local_muted(response.mute().sid(), response.mute().muted()))
            emit_session();
        } else if (response.has_subscription_permission_update()) {
          const auto& update = response.subscription_permission_update();
          auto notification =
              event(TransportEventKind::subscription_permission_changed, {{}, epoch_, 0, false});
          notification.permission = SubscriptionPermissionChange{
              update.participant_sid(), update.track_sid(), update.allowed()};
          emit(std::move(notification));
        } else if (response.has_track_unpublished()) {
          const auto& sid = response.track_unpublished().track_sid();
          // Server permission revocation is terminal for this publication, not
          // just a roster update. Stop admission before publishing the removal.
          for (auto it = pending_publications.begin(); it != pending_publications.end();) {
            const auto publication = it->second;
            if (publication->sid != sid) {
              ++it;
              continue;
            }
            publication->close();
            publisher.pc->RemoveTrackOrError(publication->sender);
            {
              std::lock_guard lock(mutex_);
              const auto active = publications_.find(publication->binding.id);
              if (active != publications_.end() && active->second == publication)
                publications_.erase(active);
            }
            emit(event(TransportEventKind::removed, publication->binding,
                       "Publication removed by server"));
            it = pending_publications.erase(it);
            publisher_dirty = true;
          }
          auto removed = session_roster.local_unpublished(sid);
          if (!removed) {
            return Result<void>::failure(removed.failure_record());
          }
          if (*removed)
            emit_session();
          if (auto applied = send_permissions(); !applied) {
            return applied;
          }
        } else if (response.has_pong() || response.has_pong_resp())
          last_pong = SteadyClock::now();
        else if (response.has_refresh_token())
          signal.update_token(response.refresh_token());
        else if (response.has_subscription_response() && response.subscription_response().err()) {
          for (const auto& [id, subscription] : pending_subscriptions)
            if (subscription->descriptor.remote_sid == response.subscription_response().track_sid())
              emit(event(TransportEventKind::operation_failed, subscription->binding,
                         "Server rejected subscription"));
        } else if (response.has_request_response() &&
                   (response.request_response().has_update_metadata() ||
                    response.request_response().request_case() ==
                        livekit::RequestResponse::REQUEST_NOT_SET)) {
          const auto& reply = response.request_response();
          auto result = event(TransportEventKind::control_completed, {{}, epoch_}, reply.message());
          result.operation_id = reply.request_id();
          result.control_reason = reply.reason();
          if (reply.reason())
            result.control_error = reply.reason() == livekit::RequestResponse::NOT_ALLOWED
                                       ? ErrorCode::permission_denied
                                       : ErrorCode::request_failed;
          emit(std::move(result));
        } else if (response.has_request_response() && response.request_response().has_add_track() &&
                   response.request_response().reason()) {
          const auto found =
              pending_publications.find(response.request_response().add_track().cid());
          if (found != pending_publications.end())
            emit(event(TransportEventKind::operation_failed, found->second->binding,
                       response.request_response().message()));
        } else if (response.has_leave()) {
          const auto& leave = response.leave();
          if (leave.has_regions()) {
            if (auto updated = signal.update_regions(leave.regions()); !updated) {
              return updated;
            }
          }
          const bool retry = !livekit::sdk::detail::terminal_leave(leave.reason()) &&
                             (leave.can_reconnect() || leave.action() != 0);
          return Result<void>::failure(
              make_failure(ErrorDomain::signaling, ErrorCode::session_closed,
                           "Server closed room session", retry, leave.reason()));
        }
      }
    }
    return Result<void>::success();
  }();
}
}  // namespace
}  // namespace livekit::sdk::rtc::detail

namespace livekit::sdk::rtc::detail {
std::function<std::unique_ptr<RoomTransport>()> livekit_transport_factory() {
  return [] { return std::make_unique<LiveKitTransport>(); };
}
}  // namespace livekit::sdk::rtc::detail
