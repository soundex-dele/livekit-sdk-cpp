#pragma once
#include "livekit/rtc/session_info.hpp"
#include "livekit.pb.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include "livekit/result.hpp"

namespace livekit::sdk::rtc::detail {
// Session-worker-owned reducer. Never exposes protobuf/native types publicly.
// A disconnected SID is terminal for this session, including delayed updates.
class SessionRoster {
 public:
  static Result<SessionRoster> create(const livekit::JoinResponse& join, uint64_t epoch) {
    SessionRoster roster;
    roster.state_.epoch = epoch;
    if (auto updated = roster.room(join.room()); !updated) {
      return Result<SessionRoster>::failure(updated.failure_record());
    }
    roster.state_.local.sid = join.participant().sid();
    if (auto updated = roster.participant(join.participant()); !updated) {
      return Result<SessionRoster>::failure(updated.failure_record());
    }
    return Result<SessionRoster>::success(std::move(roster));
  }

  std::shared_ptr<const SessionInfo> snapshot() const {
    return std::make_shared<const SessionInfo>(state_);
  }

  Result<bool> participant(const livekit::ParticipantInfo& p) {
    if (p.sid().empty() || p.identity().empty())
      return Result<bool>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                                "Participant has no SID/identity", false, 0,
                                                {"session_roster"}));
    if (departed_.contains(p.sid()))
      return Result<bool>::success(false);
    const auto version = versions_.find(p.sid());
    if (version != versions_.end() && p.version() < version->second)
      return Result<bool>::success(false);
    if (version == versions_.end() && versions_.size() >= 16384)
      return Result<bool>::failure(
          make_failure(ErrorDomain::protocol, ErrorCode::resource_exhausted,
                       "Session participant history exhausted", false, 0, {"session_roster"}));
    const bool local = p.sid() == state_.local.sid;
    if (!local && p.state() != 3 && !state_.remote.contains(p.sid()) &&
        state_.remote.size() >= 4096 &&
        std::none_of(state_.remote.begin(), state_.remote.end(),
                     [&](const auto& row) { return row.second.identity == p.identity(); }))
      return Result<bool>::failure(
          make_failure(ErrorDomain::protocol, ErrorCode::resource_exhausted,
                       "Session roster exhausted", false, 0, {"session_roster"}));
    // Validate the complete update before changing version/history or replacing an identity.
    auto parsed = parse(p);
    if (!parsed) {
      return Result<bool>::failure(parsed.failure_record());
    }
    versions_[p.sid()] = p.version();
    if (p.state() == 3 && !local) {
      departed_.insert(p.sid());
      state_.remote.erase(p.sid());
      ++state_.revision;
      return Result<bool>::success(true);
    }
    if (!local) {
      // A rapid rejoin may replace an identity before its old SID's disconnect
      // update is delivered. Keep one incarnation per identity, and retire the
      // old SID so its delayed updates cannot erase/revive the replacement.
      for (auto it = state_.remote.begin(); it != state_.remote.end();) {
        if (it->first != p.sid() && it->second.identity == p.identity()) {
          departed_.insert(it->first);
          it = state_.remote.erase(it);
        } else
          ++it;
      }
    }
    auto next = std::move(*parsed);
    const auto previous = find(p.sid());
    if (previous) {
      next.speaking = previous->speaking;
      next.audio_level = previous->audio_level;
      next.connection_quality = previous->connection_quality;
      if (local) {
        // AddTrack acknowledgements have no participant version. A delayed
        // self update may therefore omit an already-confirmed publication.
        // Explicit unpublish observations own local removal, not omission.
        for (const auto& [sid, track] : previous->publications)
          next.publications.try_emplace(sid, track);
      }
    }
    if (local)
      for (const auto& sid : unpublished_)
        next.publications.erase(sid);
    if (local)
      state_.local = std::move(next);
    else
      state_.remote[p.sid()] = std::move(next);
    ++state_.revision;
    return Result<bool>::success(true);
  }

  Result<void> room(const livekit::Room& value) {
    if (!state_.room.sid.empty() && state_.room.sid != value.sid())
      return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                                                "Room update changed session identity", false, 0,
                                                {"session_roster"}));
    state_.room = {value.sid(), value.name(), value.metadata(), value.active_recording()};
    ++state_.revision;
    return Result<void>::success();
  }

  bool local_publication(const livekit::TrackInfo& value) {
    if (value.sid().empty() || unpublished_.contains(value.sid()))
      return false;
    state_.local.publications[value.sid()] = parse(value);
    ++state_.revision;
    return true;
  }

  Result<bool> local_unpublished(const std::string& sid) {
    if (sid.empty())
      return Result<bool>::success(false);
    if (!unpublished_.contains(sid) && unpublished_.size() >= 16384)
      return Result<bool>::failure(
          make_failure(ErrorDomain::protocol, ErrorCode::resource_exhausted,
                       "Session publication history exhausted", false, 0, {"session_roster"}));
    unpublished_.insert(sid);
    if (!state_.local.publications.erase(sid))
      return Result<bool>::success(false);
    ++state_.revision;
    return Result<bool>::success(true);
  }

  bool local_muted(const std::string& sid, bool muted) {
    auto it = state_.local.publications.find(sid);
    if (it == state_.local.publications.end() || it->second.muted == muted)
      return false;
    it->second.muted = muted;
    ++state_.revision;
    return true;
  }

  // Signaling uses deltas; legacy data-channel speaker updates are full lists.
  template <class Speakers>
  bool speakers(const Speakers& speakers, bool full = false) {
    bool changed = false;
    std::set<std::string> active;
    for (const auto& speaker : speakers) {
      auto p = find(speaker.sid());
      if (!p || !std::isfinite(speaker.level()))
        continue;
      const bool speaking = speaker.active();
      const float level = speaking ? std::clamp(speaker.level(), 0.f, 1.f) : 0.f;
      active.insert(speaker.sid());
      if (p->speaking != speaking || p->audio_level != level) {
        p->speaking = speaking;
        p->audio_level = level;
        changed = true;
      }
    }
    if (full) {
      auto clear = [&](ParticipantSnapshot& p) {
        if (!active.contains(p.sid) && (p.speaking || p.audio_level != 0)) {
          p.speaking = false;
          p.audio_level = 0;
          changed = true;
        }
      };
      clear(state_.local);
      for (auto& [sid, p] : state_.remote)
        clear(p);
    }
    if (changed)
      ++state_.revision;
    return changed;
  }

  bool quality(const livekit::ConnectionQualityUpdate& update) {
    bool changed = false;
    for (const auto& info : update.updates())
      if (auto p = find(info.participant_sid()); p && p->connection_quality != info.quality()) {
        p->connection_quality = info.quality();
        changed = true;
      }
    if (changed)
      ++state_.revision;
    return changed;
  }

 private:
  SessionRoster() = default;
  SessionInfo state_;
  std::map<std::string, uint32_t> versions_;
  std::set<std::string> departed_;
  std::set<std::string> unpublished_;

  ParticipantSnapshot* find(const std::string& sid) {
    if (sid == state_.local.sid)
      return &state_.local;
    auto it = state_.remote.find(sid);
    return it == state_.remote.end() ? nullptr : &it->second;
  }

  static PublicationInfo parse(const livekit::TrackInfo& t) {
    return {t.sid(),
            t.name(),
            t.mime_type(),
            t.stream(),
            PublicationKind(t.type()),
            PublicationSource(t.source()),
            t.muted(),
            t.width(),
            t.height()};
  }

  static Result<ParticipantSnapshot> parse(const livekit::ParticipantInfo& p) {
    ParticipantSnapshot value;
    value.sid = p.sid();
    value.identity = p.identity();
    value.name = p.name();
    value.metadata = p.metadata();
    value.version = p.version();
    value.state = p.state();
    value.kind = p.kind();
    value.disconnect_reason = p.disconnect_reason();
    value.attributes.insert(p.attributes().begin(), p.attributes().end());
    for (const auto& t : p.tracks()) {
      if (t.sid().empty() || !value.publications.emplace(t.sid(), parse(t)).second)
        return Result<ParticipantSnapshot>::failure(
            make_failure(ErrorDomain::protocol, ErrorCode::malformed_message,
                         "Participant has invalid publication SID", false, 0, {"session_roster"}));
    }
    if (p.has_permission()) {
      const auto& permission = p.permission();
      value.permissions = ParticipantPermissions{
          permission.can_subscribe(), permission.can_publish(), permission.can_publish_data(),
          permission.can_update_metadata(), permission.hidden()};
      for (const auto source : permission.can_publish_sources())
        value.permissions->can_publish_sources.push_back(PublicationSource(source));
    }
    return Result<ParticipantSnapshot>::success(std::move(value));
  }
};
}  // namespace livekit::sdk::rtc::detail
