#include "ui.hpp"
#include "connection.hpp"
#include "imgui.h"
#include "imgui_impl_sdlrenderer2.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace demo {
View::View() {
  std::snprintf(url.data(), url.size(), "ws://127.0.0.1:7880");
  std::snprintf(room.data(), room.size(), "demo");
}

SessionOptions View::options() const {
  auto o = username_options(url.data(), room.data(), username.data());
  o.audio_playout = audio;
  for (int i = 0; i < std::clamp(video_count, 0, max_test_videos); ++i) {
    sdk::MediaSpec m;
    m.demo = true;
    o.media.push_back(m);
  }
  if (tone) {
    sdk::MediaSpec m;
    m.demo = true;
    m.video = false;
    o.media.push_back(m);
  }
  return o;
}

void apply_style(float scale, float pixel_density) {
  ImGui::GetStyle() = ImGuiStyle{};
  ImGui::StyleColorsDark();
  auto& s = ImGui::GetStyle();
  s.WindowPadding = {20, 20};
  s.FramePadding = {10, 8};
  s.ItemSpacing = {12, 12};
  s.WindowRounding = 0;
  s.ChildRounding = 8;
  s.FrameRounding = 5;
  s.Colors[ImGuiCol_WindowBg] = {0.055f, 0.068f, 0.09f, 1};
  s.Colors[ImGuiCol_ChildBg] = {0.085f, 0.10f, 0.13f, 1};
  s.Colors[ImGuiCol_Button] = {0.14f, 0.32f, 0.50f, 1};
  s.Colors[ImGuiCol_ButtonHovered] = {0.20f, 0.44f, 0.64f, 1};
  s.Colors[ImGuiCol_FrameBg] = {0.12f, 0.15f, 0.20f, 1};
  s.ScaleAllSizes(scale);
  auto& io = ImGui::GetIO();
  // Rasterize at backing-pixel resolution while retaining logical UI sizes.
  // The caller destroys the renderer's font texture before rebuilding fonts.
  pixel_density = std::clamp(pixel_density, 1.0f, 3.0f);
  io.Fonts->Clear();
  ImFontConfig font;
  font.SizePixels = 13.0f * scale * pixel_density;
  io.FontDefault = io.Fonts->AddFontDefault(&font);
  io.FontGlobalScale = 1.0f / pixel_density;
}

void render_draw_data(SDL_Renderer* renderer) {
  // SDL's ImGui backend scales clipping, but expects the application to scale
  // vertex coordinates via the renderer (as in the upstream SDL example).
  const auto& io = ImGui::GetIO();
  if (SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y) != 0)
    throw std::runtime_error(SDL_GetError());
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void View::draw_quality(const Snapshot& snapshot) {
  const float scale = ImGui::GetFontSize() / 13.0f;
  const auto* viewport = ImGui::GetMainViewport();
  const ImVec2 size{std::min(960 * scale, viewport->WorkSize.x - 24 * scale),
                    std::min(620 * scale, viewport->WorkSize.y - 24 * scale)};
  ImGui::SetNextWindowPos({viewport->WorkPos.x + (viewport->WorkSize.x - size.x) / 2,
                           viewport->WorkPos.y + (viewport->WorkSize.y - size.y) / 2});
  ImGui::SetNextWindowSize(size);
  if (ImGui::Begin("Quality dashboard", &show_quality,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape))
      show_quality = false;
    ImGui::TextUnformatted("CONNECTION & MEDIA QUALITY");
    ImGui::TextWrapped(
        "Refresh: 1 s. -- = unavailable or waiting for samples. Close with X or Escape.");
    ImGui::Separator();
    if (snapshot.finished) {
      ImGui::TextWrapped("No active session. Join a room to collect live quality data.");
    } else {
      auto metric = [](const char* label, std::optional<double> value, const char* unit,
                       int precision = 1) {
        if (value)
          ImGui::Text("%s: %.*f %s", label, precision, *value, unit);
        else
          ImGui::Text("%s: --", label);
      };
      ImGui::TextUnformatted("MEDIA BITRATES / TRACK (kbps)");
      ImGui::TextWrapped(
          "Each row is one track. Simulcast layers are summed only within that track. Retx = "
          "retransmitted payload.");
      if (ImGui::BeginTable(
              "Track bitrates", 4,
              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
              {0, 175 * scale})) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Participant / track / SID", ImGuiTableColumnFlags_WidthStretch);
        for (const char* label : {"RTP", "Retx", "FEC"})
          ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, 75 * scale);
        ImGui::TableHeadersRow();
        bool any = false;
        for (const auto& [sid, participant] : snapshot.participants)
          for (const auto& track : participant.tracks) {
            if (track.kind != rtc::PublicationKind::video &&
                track.kind != rtc::PublicationKind::audio)
              continue;
            any = true;
            const bool local = sid == snapshot.local_sid;
            const char* kind = track.kind == rtc::PublicationKind::video ? "Video" : "Audio";
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped(
                "%s / %s",
                (participant.name.empty() ? participant.identity : participant.name).c_str(),
                track.name.empty() ? kind : track.name.c_str());
            ImGui::TextWrapped("%s %s | %s%s", local ? "Send" : "Receive", kind, track.sid.c_str(),
                               track.muted ? " | Muted"
                                           : (!local && !snapshot.subscriptions.contains(track.sid)
                                                  ? " | Unsubscribed"
                                                  : ""));
            for (const auto rate :
                 {QualitySnapshot::Bitrate::rtp, QualitySnapshot::Bitrate::retransmit,
                  QualitySnapshot::Bitrate::fec}) {
              ImGui::TableNextColumn();
              const auto value = local || snapshot.subscriptions.contains(track.sid)
                                     ? snapshot.quality.bitrate(rate, track.sid, local)
                                     : std::nullopt;
              if (value)
                ImGui::Text("%.1f", *value);
              else
                ImGui::TextUnformatted("--");
            }
          }
        if (!any) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextWrapped("No media tracks yet.");
        }
        ImGui::EndTable();
      }
      ImGui::TextWrapped(
          "Retx/FEC are included in RTP. FEC send and Opus in-band FEC bytes are unavailable. -- "
          "does not mean zero or disabled.");
      ImGui::Separator();
      using Stage = QualitySnapshot::Stage;
      ImGui::TextUnformatted("VIDEO FRAME RATES / TRACK (fps)");
      ImGui::TextWrapped(
          "Local: capture, encode, send. Remote: receive, decode. -- = not applicable, missing or "
          "stale.");
      if (ImGui::BeginTable(
              "Video stages", 6,
              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
              {0, 175 * scale})) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Video track / SID", ImGuiTableColumnFlags_WidthStretch);
        for (const char* label : {"Capture", "Encode", "Send", "Receive", "Decode"})
          ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, 65 * scale);
        ImGui::TableHeadersRow();
        bool any = false;
        for (const auto& [sid, participant] : snapshot.participants)
          for (const auto& track : participant.tracks) {
            if (track.kind != rtc::PublicationKind::video)
              continue;
            any = true;
            const bool local = sid == snapshot.local_sid;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped(
                "%s / %s",
                (participant.name.empty() ? participant.identity : participant.name).c_str(),
                track.name.empty() ? "Video" : track.name.c_str());
            ImGui::TextWrapped("%s | %s%s", local ? "Local" : "Remote", track.sid.c_str(),
                               track.muted ? " | Muted"
                                           : (!local && !snapshot.subscriptions.contains(track.sid)
                                                  ? " | Unsubscribed"
                                                  : ""));
            for (auto stage :
                 {Stage::capture, Stage::encode, Stage::send, Stage::receive, Stage::decode}) {
              ImGui::TableNextColumn();
              const bool applicable =
                  local ? stage <= Stage::send
                        : snapshot.subscriptions.contains(track.sid) && stage >= Stage::receive;
              const auto value = applicable ? snapshot.quality.fps(stage, track.sid) : std::nullopt;
              if (value)
                ImGui::Text("%.1f", *value);
              else
                ImGui::TextUnformatted("--");
            }
          }
        if (!any) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextWrapped("No video tracks yet.");
        }
        ImGui::EndTable();
      }
      ImGui::TextWrapped(
          "Receive counts complete encoded frames entering the graph; decode counts completed "
          "outputs. Send uses RTP framesSent (all layers of this track).");
      ImGui::Separator();
      if (ImGui::BeginTable("Connections", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < snapshot.quality.peers.size(); ++i) {
          ImGui::TableNextColumn();
          const auto& peer = snapshot.quality.peers[i];
          ImGui::TextUnformatted(i == 0 ? "PUBLISH CONNECTION" : "SUBSCRIBE CONNECTION");
          const bool fresh = peer.timestamp_us > 0 && !peer.stale();
          if (!peer.timestamp_us)
            ImGui::TextWrapped("Waiting for WebRTC statistics...");
          else {
            const auto age =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - peer.updated)
                    .count();
            ImGui::Text("%s | %.1f s ago", fresh ? "Live" : "Stale", age);
          }
          metric("Network RTT", fresh ? peer.rtt_ms : std::nullopt, "ms");
        }
        ImGui::EndTable();
      }
      ImGui::Separator();
      ImGui::Text("Decoded video frames delivered: %llu",
                  static_cast<unsigned long long>(snapshot.received_frames));
      ImGui::TextUnformatted("RTP STREAM DETAILS / SSRC");
      ImGui::TextWrapped(
          "Rates use consecutive RTP byte counters. Loss is cumulative on receive streams. RTT is "
          "network round-trip time, not end-to-end media delay.");
      bool has_streams = false;
      for (const auto& peer : snapshot.quality.peers)
        has_streams |= !peer.streams.empty();
      if (!has_streams)
        ImGui::TextWrapped("No media statistics yet. Publish or subscribe to a track.");
      else if (ImGui::BeginTable("Streams", 8,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                                 {0, 170 * scale})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Stream / SSRC", ImGuiTableColumnFlags_WidthFixed, 230 * scale);
        ImGui::TableSetupColumn("RTP kbps", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("Retx kbps", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("FEC kbps", ImGuiTableColumnFlags_WidthFixed, 80 * scale);
        ImGui::TableSetupColumn("Send FPS", ImGuiTableColumnFlags_WidthFixed, 75 * scale);
        ImGui::TableSetupColumn("Lost packets", ImGuiTableColumnFlags_WidthFixed, 105 * scale);
        ImGui::TableSetupColumn("Jitter (ms)", ImGuiTableColumnFlags_WidthFixed, 95 * scale);
        ImGui::TableSetupColumn("Sample", ImGuiTableColumnFlags_WidthFixed, 70 * scale);
        ImGui::TableHeadersRow();
        for (size_t p = 0; p < snapshot.quality.peers.size(); ++p)
          for (const auto& [id, stream] : snapshot.quality.peers[p].streams) {
            const auto& peer = snapshot.quality.peers[p];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s %s / %s", stream.sending ? "Send" : "Recv", stream.kind.c_str(),
                        stream.ssrc.empty() ? id.c_str() : stream.ssrc.c_str());
            const auto track = snapshot.quality.track_id(stream, p);
            if (!track.empty())
              ImGui::TextWrapped("%s", track.c_str());
            auto cell = [](std::optional<double> value, int precision) {
              ImGui::TableNextColumn();
              if (value)
                ImGui::Text("%.*f", precision, *value);
              else
                ImGui::TextUnformatted("--");
            };
            cell(peer.stale() ? std::nullopt : stream.kbps, 1);
            cell(peer.stale() ? std::nullopt : stream.retransmit_kbps, 1);
            cell(peer.stale() ? std::nullopt : stream.fec_kbps, 1);
            cell(peer.stale() ? std::nullopt : stream.fps, 1);
            cell(stream.lost, 0);
            cell(peer.stale() ? std::nullopt : stream.jitter_ms, 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(peer.stale() ? "Stale" : "Live");
          }
        ImGui::EndTable();
      }
      ImGui::Spacing();
      ImGui::TextUnformatted("PARTICIPANT QUALITY (server reported)");
      if (snapshot.participants.empty())
        ImGui::TextUnformatted("Waiting for participants...");
      for (const auto& [sid, participant] : snapshot.participants) {
        const auto quality = snapshot.quality.participants.find(sid);
        ImGui::TextWrapped(
            "%s%s: %s",
            (participant.name.empty() ? participant.identity : participant.name).c_str(),
            sid == snapshot.local_sid ? " (you)" : "",
            quality == snapshot.quality.participants.end() ? "Unknown"
                                                           : quality_name(quality->second));
      }
    }
  }
  ImGui::End();
}

Action View::draw(SDL_Renderer* renderer, const Snapshot& snapshot) {
  const float scale = ImGui::GetFontSize() / 13.0f;
  auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin(
      "Conference", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  Action action = Action::None;
  remove_video_sid.clear();
  ImGui::TextUnformatted("LIVEKIT  /  CONFERENCE");
  ImGui::SameLine();
  ImGui::TextDisabled("%s", state_name(snapshot.state));
  ImGui::SameLine();
  if (ImGui::Button(show_quality ? "Hide dashboard" : "Quality dashboard"))
    show_quality = !show_quality;
  ImGui::Separator();
  if (snapshot.finished) {
    textures_.clear();
    ImGui::TextUnformatted("Join a room");
    ImGui::TextWrapped(
        "Watch participants together, or publish multiple test videos from one participant.");
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Server URL", url.data(), url.size());
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Room", room.data(), room.size());
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Username", username.data(), username.size());
    ImGui::Checkbox("Play remote audio", &audio);
    ImGui::SetNextItemWidth(200 * scale);
    ImGui::SliderInt("Test video streams", &video_count, 0, max_test_videos, "%d",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Pause video when nobody watches", &pause_unused_video);
    if (pause_unused_video)
      ImGui::TextWrapped(
          "Applies to all test videos, including videos added later. Initial frames and a pause "
          "delay are normal.");
    ImGui::Checkbox("Publish test tone", &tone);
    ImGui::TextWrapped("Use a different username in each window. Test tone: 440 Hz.");
    ImGui::BeginDisabled(!url[0] || !room[0] || !username[0]);
    if (ImGui::Button("Join room", {160 * scale, 0})) {
      error.clear();
      action = Action::Join;
    }
    ImGui::EndDisabled();
    const auto& message = error.empty() ? snapshot.error : error;
    if (!message.empty()) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .5f, .46f, 1));
      ImGui::TextWrapped("%s", message.c_str());
      ImGui::PopStyleColor();
    }
  } else {
    ImGui::Text("Room: %s", snapshot.room.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu participants", snapshot.participants.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(snapshot.state == State::Disconnecting);
    if (ImGui::Button(snapshot.state == State::Connecting ? "Cancel" : "Leave"))
      action = Action::Leave;
    ImGui::EndDisabled();
    if (snapshot.state == State::Disconnecting)
      ImGui::TextUnformatted("Leaving room. Waiting for media and network cleanup...");
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("Members", {std::min(220 * scale, available * .28f), 0},
                      ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("YOUR VIDEO STREAMS");
    const auto local = snapshot.participants.find(snapshot.local_sid);
    int video_count = 0;
    if (local != snapshot.participants.end())
      for (const auto& t : local->second.tracks)
        if (t.kind == rtc::PublicationKind::video)
          ++video_count;
    ImGui::TextDisabled("%d / %d active", video_count, max_test_videos);
    ImGui::TextWrapped("Pause unused video: %s", snapshot.pause_unused_video ? "ON" : "OFF");
    const bool changing =
        snapshot.state != State::Connected || snapshot.changing_video || action != Action::None;
    ImGui::BeginDisabled(changing || video_count >= max_test_videos);
    if (ImGui::Button("Add test video"))
      action = Action::AddVideo;
    ImGui::EndDisabled();
    if (local != snapshot.participants.end())
      for (const auto& t : local->second.tracks) {
        if (t.kind != rtc::PublicationKind::video)
          continue;
        ImGui::PushID(t.sid.c_str());
        ImGui::TextWrapped("%s", t.name.c_str());
        ImGui::TextWrapped("%s", t.sid.c_str());
        if (snapshot.pause_unused_video) {
          const auto demand = snapshot.video_demand.find(t.sid);
          ImGui::TextWrapped("%s", demand == snapshot.video_demand.end()
                                       ? "Video production: waiting for feedback"
                                   : demand->second ? "Video production: enabled"
                                                    : "Video production: paused");
        }
        ImGui::BeginDisabled(changing || action != Action::None);
        if (ImGui::SmallButton("Stop stream")) {
          remove_video_sid = t.sid;
          action = Action::RemoveVideo;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
    if (snapshot.changing_video)
      ImGui::TextWrapped("Updating video streams...");
    if (!snapshot.video_error.empty())
      ImGui::TextWrapped("Video error: %s", snapshot.video_error.c_str());
    ImGui::TextWrapped("Test videos are visible to other participants.");
    ImGui::Separator();
    ImGui::TextUnformatted("PARTICIPANTS");
    ImGui::Separator();
    for (const auto& [sid, p] : snapshot.participants) {
      ImGui::PushID(sid.c_str());
      ImGui::TextWrapped("%s%s", (p.name.empty() ? p.identity : p.name).c_str(),
                         sid == snapshot.local_sid ? " (you)" : "");
      bool has_audio = false, muted = true;
      for (const auto& t : p.tracks)
        if (t.kind == rtc::PublicationKind::audio) {
          has_audio = true;
          muted &= t.muted;
        }
      ImGui::TextDisabled("%s", !has_audio ? "No audio"
                                : muted    ? "Audio muted"
                                           : "Audio available");
      ImGui::Separator();
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Videos", {0, 0});
    auto views = tiles(snapshot);
    if (views.empty())
      ImGui::TextWrapped(
          "Waiting for participants. Open another client with a different username to start the "
          "demo.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns =
        std::max(1, std::min(3, static_cast<int>((width + gap) / (300 * scale + gap))));
    const float tile_width = std::max(1.0f, (width - gap * (columns - 1)) / columns);
    std::set<std::string> active;
    for (size_t i = 0; i < views.size(); ++i) {
      const auto& tile = views[i];
      active.insert(tile.id);
      if (i % columns)
        ImGui::SameLine();
      ImGui::PushID(tile.id.c_str());
      ImGui::BeginChild("Tile", {tile_width, tile_width * 9 / 16 + 55 * scale},
                        ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
      ImGui::TextUnformatted(tile.label.c_str());
      auto pos = ImGui::GetCursorScreenPos();
      auto area = ImGui::GetContentRegionAvail();
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(pos, {pos.x + area.x, pos.y + area.y}, IM_COL32(12, 15, 20, 255),
                          4 * scale);
      if (tile.frame) {
        auto& cached = textures_[tile.id];
        if (cached.last != tile.frame) {
          cached.texture.upload(renderer, *tile.frame);
          cached.last = tile.frame;
        }
        const auto fit = fit_video(pos.x, pos.y, area.x, area.y, cached.texture.width(),
                                   cached.texture.height());
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(cached.texture.get())),
                       {fit.x, fit.y}, {fit.x + fit.w, fit.y + fit.h});
      } else {
        textures_.erase(tile.id);
        auto size = ImGui::CalcTextSize(tile.status.c_str());
        draw->AddText({pos.x + std::max(0.0f, (area.x - size.x) / 2), pos.y + area.y / 2},
                      IM_COL32(155, 171, 191, 255), tile.status.c_str());
      }
      ImGui::Dummy(area);
      ImGui::EndChild();
      ImGui::PopID();
    }
    std::erase_if(textures_, [&](const auto& item) { return !active.contains(item.first); });
    ImGui::EndChild();
  }
  ImGui::End();
  if (show_quality)
    draw_quality(snapshot);
  return action;
}
}  // namespace demo
