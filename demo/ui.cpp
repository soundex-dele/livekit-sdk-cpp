#include "ui.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <set>

namespace demo {
View::View() { std::snprintf(url.data(), url.size(), "ws://127.0.0.1:7880"); std::snprintf(room.data(), room.size(), "demo"); }
sdk::RoomOptions View::options() const {
  sdk::RoomOptions o; o.url = url.data(); o.room = room.data(); o.token = token.data(); o.audio_playout = audio;
  if (video) { sdk::MediaSpec m; m.demo = true; o.media.push_back(m); }
  if (tone) { sdk::MediaSpec m; m.demo = true; m.video = false; o.media.push_back(m); }
  return o;
}
void apply_style(float scale) {
  ImGui::GetStyle() = ImGuiStyle{};
  ImGui::StyleColorsDark();
  auto& s = ImGui::GetStyle(); s.WindowPadding = {20, 20}; s.FramePadding = {10, 8};
  s.ItemSpacing = {12, 12}; s.WindowRounding = 0; s.ChildRounding = 8; s.FrameRounding = 5;
  s.Colors[ImGuiCol_WindowBg] = {0.055f, 0.068f, 0.09f, 1};
  s.Colors[ImGuiCol_ChildBg] = {0.085f, 0.10f, 0.13f, 1};
  s.Colors[ImGuiCol_Button] = {0.14f, 0.32f, 0.50f, 1};
  s.Colors[ImGuiCol_ButtonHovered] = {0.20f, 0.44f, 0.64f, 1};
  s.Colors[ImGuiCol_FrameBg] = {0.12f, 0.15f, 0.20f, 1};
  s.ScaleAllSizes(scale); ImGui::GetIO().FontGlobalScale = scale;
}
Action View::draw(SDL_Renderer* renderer, const Snapshot& snapshot) {
  const float scale = ImGui::GetIO().FontGlobalScale;
  auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("Conference", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  Action action = Action::None;
  ImGui::TextUnformatted("LIVEKIT  /  CONFERENCE");
  ImGui::SameLine(); ImGui::TextDisabled("%s", state_name(snapshot.state));
  ImGui::Separator();
  if (snapshot.finished) {
    textures_.clear();
    ImGui::TextUnformatted("Join a room");
    ImGui::TextWrapped("Watch participants together, or publish a test pattern to try multiple clients.");
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Server URL", url.data(), url.size());
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Room", room.data(), room.size());
    ImGui::SetNextItemWidth(std::min(620.0f * scale, ImGui::GetContentRegionAvail().x * .75f));
    ImGui::InputText("Token", token.data(), token.size(), ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Play remote audio", &audio);
    ImGui::Checkbox("Publish test video", &video);
    ImGui::Checkbox("Publish test tone", &tone);
    ImGui::TextDisabled("Test tone: 440 Hz. Each client needs its own participant token.");
    ImGui::BeginDisabled(!url[0] || !room[0] || !token[0]);
    if (ImGui::Button("Join room", {160 * scale, 0})) { error.clear(); action = Action::Join; }
    ImGui::EndDisabled();
    const auto& message = error.empty() ? snapshot.error : error;
    if (!message.empty()) { ImGui::Spacing(); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .5f, .46f, 1)); ImGui::TextWrapped("%s", message.c_str()); ImGui::PopStyleColor(); }
  } else {
    ImGui::Text("Room: %s", snapshot.room.c_str()); ImGui::SameLine();
    ImGui::TextDisabled("%zu participants", snapshot.participants.size()); ImGui::SameLine();
    ImGui::BeginDisabled(snapshot.state == State::Disconnecting);
    if (ImGui::Button(snapshot.state == State::Connecting ? "Cancel" : "Leave")) action = Action::Leave;
    ImGui::EndDisabled();
    if (snapshot.state == State::Disconnecting) ImGui::TextUnformatted("Leaving room. Waiting for media and network cleanup...");
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("Members", {std::min(220 * scale, available * .28f), 0}, ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("PARTICIPANTS"); ImGui::Separator();
    for (const auto& [sid, p] : snapshot.participants) {
      ImGui::PushID(sid.c_str());
      ImGui::TextWrapped("%s%s", (p.name.empty() ? p.identity : p.name).c_str(), sid == snapshot.local_sid ? " (you)" : "");
      bool has_audio = false, muted = true;
      for (const auto& t : p.tracks) if (t.kind == sdk::TrackKind::audio) { has_audio = true; muted &= t.muted; }
      ImGui::TextDisabled("%s", !has_audio ? "No audio" : muted ? "Audio muted" : "Audio available");
      ImGui::Separator(); ImGui::PopID();
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("Videos", {0, 0});
    auto views = tiles(snapshot);
    if (views.empty()) ImGui::TextWrapped("Waiting for participants. Open another client with a different token to start the demo.");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, std::min(3, static_cast<int>((width + gap) / (300 * scale + gap))));
    const float tile_width = std::max(1.0f, (width - gap * (columns - 1)) / columns);
    std::set<std::string> active;
    for (size_t i = 0; i < views.size(); ++i) {
      const auto& tile = views[i]; active.insert(tile.id);
      if (i % columns) ImGui::SameLine();
      ImGui::PushID(tile.id.c_str());
      ImGui::BeginChild("Tile", {tile_width, tile_width * 9 / 16 + 55 * scale}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
      ImGui::TextUnformatted(tile.label.c_str());
      auto pos = ImGui::GetCursorScreenPos(); auto area = ImGui::GetContentRegionAvail();
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(pos, {pos.x + area.x, pos.y + area.y}, IM_COL32(12, 15, 20, 255), 4 * scale);
      if (tile.frame) {
        auto& cached = textures_[tile.id];
        if (cached.last != tile.frame) { cached.texture.upload(renderer, *tile.frame); cached.last = tile.frame; }
        const auto fit = fit_video(pos.x, pos.y, area.x, area.y, cached.texture.width(), cached.texture.height());
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(cached.texture.get())), {fit.x, fit.y}, {fit.x + fit.w, fit.y + fit.h});
      } else {
        textures_.erase(tile.id);
        auto size = ImGui::CalcTextSize(tile.status.c_str());
        draw->AddText({pos.x + std::max(0.0f, (area.x - size.x) / 2), pos.y + area.y / 2}, IM_COL32(155, 171, 191, 255), tile.status.c_str());
      }
      ImGui::Dummy(area); ImGui::EndChild(); ImGui::PopID();
    }
    std::erase_if(textures_, [&](const auto& item) { return !active.contains(item.first); });
    ImGui::EndChild();
  }
  ImGui::End(); return action;
}
}
