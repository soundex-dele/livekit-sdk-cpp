#include "ui.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

namespace player_demo {
namespace {
constexpr ImVec4 muted_color{.49f, .55f, .64f, 1};
constexpr ImVec4 accent{.26f, .85f, .69f, 1};
const char* state_label(sdk::PlayerState state) {
  switch (state) {
    case sdk::PlayerState::ready: return "就绪";
    case sdk::PlayerState::playing: return "正在播放";
    case sdk::PlayerState::paused: return "已暂停";
    case sdk::PlayerState::ended: return "播放结束";
    case sdk::PlayerState::failed: return "打开失败";
    default: return "未打开文件";
  }
}
const char* operation_label(Operation operation) {
  switch (operation) {
    case Operation::open: return "正在打开文件…";
    case Operation::seek: return "正在定位…";
    case Operation::close: return "正在关闭…";
    default: return "正在处理…";
  }
}
std::string filename(const std::string& path) {
  const auto text = std::filesystem::u8path(path).filename().u8string();
  return std::string(text.begin(), text.end());
}
void centered(ImDrawList* draw, ImVec2 center, const char* text, ImU32 color) {
  auto size = ImGui::CalcTextSize(text);
  draw->AddText({center.x - size.x / 2, center.y - size.y / 2}, color, text);
}
}
std::string format_time(int64_t value) {
  const auto seconds = std::max<int64_t>(0, value / 1000000);
  char text[32];
  if (seconds >= 3600) std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
  else std::snprintf(text, sizeof(text), "%02lld:%02lld", seconds / 60, seconds % 60);
  return text;
}
void load_font(float scale) {
  auto& io = ImGui::GetIO();
#ifdef _WIN32
  wchar_t directory[MAX_PATH]{};
  if (GetWindowsDirectoryW(directory, MAX_PATH)) {
    const auto font = std::filesystem::path(directory) / "Fonts" / "msyh.ttc";
    if (std::filesystem::exists(font)) {
      ImFontConfig config; config.OversampleH = config.OversampleV = 1;
      static ImVector<ImWchar> ranges;
      ImFontGlyphRangesBuilder builder;
      builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
      builder.AddText("←→×…"); builder.BuildRanges(&ranges);
      io.Fonts->AddFontFromFileTTF(font.string().c_str(), 18 * scale, &config, ranges.Data);
      return;
    }
  }
#endif
  io.Fonts->AddFontDefault();
}
void apply_style(float scale) {
  ImGui::GetStyle() = ImGuiStyle{};
  ImGui::StyleColorsDark();
  auto& s = ImGui::GetStyle();
  s.WindowPadding = {24, 20}; s.ItemSpacing = {12, 10}; s.FramePadding = {14, 9};
  s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.WindowRounding = 0;
  s.FrameRounding = 7; s.GrabRounding = 7; s.ChildRounding = 12; s.PopupRounding = 10;
  s.Colors[ImGuiCol_WindowBg] = {.055f, .068f, .09f, 1};
  s.Colors[ImGuiCol_ChildBg] = {.075f, .09f, .12f, 1};
  s.Colors[ImGuiCol_PopupBg] = {.09f, .11f, .15f, 1};
  s.Colors[ImGuiCol_Text] = {.91f, .94f, .97f, 1};
  s.Colors[ImGuiCol_TextDisabled] = muted_color;
  s.Colors[ImGuiCol_FrameBg] = {.13f, .16f, .21f, 1};
  s.Colors[ImGuiCol_FrameBgHovered] = {.18f, .23f, .29f, 1};
  s.Colors[ImGuiCol_FrameBgActive] = {.2f, .28f, .32f, 1};
  s.Colors[ImGuiCol_Button] = {.13f, .17f, .22f, 1};
  s.Colors[ImGuiCol_ButtonHovered] = {.19f, .27f, .32f, 1};
  s.Colors[ImGuiCol_ButtonActive] = {.22f, .35f, .37f, 1};
  s.Colors[ImGuiCol_SliderGrab] = accent;
  s.Colors[ImGuiCol_SliderGrabActive] = {.45f, 1, .82f, 1};
  s.Colors[ImGuiCol_CheckMark] = accent;
  s.Colors[ImGuiCol_Border] = {.15f, .19f, .24f, 1};
  s.ScaleAllSizes(scale);
}
void View::toggle_fullscreen(SDL_Window* window) {
  const bool next = !fullscreen;
  if (SDL_SetWindowFullscreen(window, next ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) fullscreen = next;
}
void View::browse(Session& session, SDL_Window* window) {
  if (session.busy() || session.closing()) return;
#ifdef _WIN32
  const bool resume = session.snapshot().state == sdk::PlayerState::playing;
  session.pause([this, &session, window, resume] {
    std::array<wchar_t, 32768> path{};
    SDL_SysWMinfo wm{}; SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = wm.info.win.window;
    dialog.lpstrTitle = L"打开媒体文件";
    dialog.lpstrFilter = L"媒体文件\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.ts;*.m4v;*.mp3;*.wav;*.flac;*.aac;*.m4a;*.ogg\0所有文件\0*.*\0";
    dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
      auto utf8 = std::filesystem::path(path.data()).u8string();
      session.open(std::string(utf8.begin(), utf8.end()));
    } else {
      if (CommDlgExtendedError()) session.set_error("无法打开系统文件选择窗口，请拖放文件或输入路径。");
      if (resume) session.toggle_play();
    }
  });
#else
  path_popup_ = true;
#endif
}
void View::draw(Session& session, SDL_Window* window) {
  scale_ = ImGui::GetFontSize() / 18;
  const float s = scale_;
  auto snapshot = session.snapshot();
  const bool loaded = snapshot.state != sdk::PlayerState::closed && snapshot.state != sdk::PlayerState::failed;
  auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("播放器", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(accent, "AV"); ImGui::SameLine(0, 10 * s);
  ImGui::TextUnformatted("PLAYER"); ImGui::SameLine(0, 16 * s);
  ImGui::TextDisabled("本地播放器");
  ImGui::SameLine(ImGui::GetWindowWidth() - 260 * s);
  ImGui::BeginDisabled(session.busy() || session.closing());
  if (ImGui::Button("打开文件", {125 * s, 0})) browse(session, window);
  ImGui::SameLine();
  if (ImGui::Button("输入路径", {95 * s, 0})) path_popup_ = true;
  ImGui::EndDisabled();
  ImGui::Spacing();

  const bool has_message = !session.error().empty() || !session.warning().empty();
  const float stage_height = std::max(100 * s, ImGui::GetContentRegionAvail().y - (has_message ? 250 : 205) * s);
  const auto origin = ImGui::GetCursorScreenPos();
  const ImVec2 size{ImGui::GetContentRegionAvail().x, stage_height};
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(5, 9, 14, 255), 12 * s);
  ImGui::InvisibleButton("##video", size);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) toggle_fullscreen(window);
  if (session.texture()) {
    const float fit = std::min(size.x / session.video_width(), size.y / session.video_height());
    const ImVec2 image_size{session.video_width() * fit, session.video_height() * fit};
    const ImVec2 start{origin.x + (size.x - image_size.x) / 2, origin.y + (size.y - image_size.y) / 2};
    draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(session.texture())), start,
        {start.x + image_size.x, start.y + image_size.y});
  } else {
    const ImVec2 center{origin.x + size.x / 2, origin.y + size.y / 2};
    draw->AddCircle({center.x, center.y - 42 * s}, 28 * s, IM_COL32(55, 118, 112, 255), 48, 1.5f * s);
    draw->AddTriangleFilled({center.x - 6 * s, center.y - 53 * s}, {center.x - 6 * s, center.y - 31 * s},
        {center.x + 12 * s, center.y - 42 * s}, IM_COL32(85, 219, 181, 255));
    const char* title = session.busy() ? operation_label(session.operation()) : !loaded ? "打开一个文件，开始播放" :
        snapshot.media.has_audio && !snapshot.media.has_video ? "正在播放音频" : "点击播放以显示画面";
    centered(draw, {center.x, center.y + 10 * s}, title, IM_COL32(226, 235, 242, 255));
    centered(draw, {center.x, center.y + 43 * s}, !loaded ? "拖放音视频文件到这里，或点击右上角打开文件" :
        snapshot.media.has_video ? "已清除旧画面，继续播放后显示新位置" : "让声音成为主角", IM_COL32(121, 141, 159, 255));
  }
  draw->AddRect(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(36, 48, 61, 255), 12 * s);
  ImGui::Spacing();

  const std::string name = session.path().empty() ? "尚未选择媒体文件" : filename(session.path());
  // Clip a long filename without pushing the status or controls out of the window.
  ImGui::BeginChild("##title", {-165 * s, 28 * s}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  ImGui::TextUnformatted(name.c_str());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", session.path().c_str());
  ImGui::EndChild(); ImGui::SameLine();
  ImGui::TextColored(session.busy() ? muted_color : accent, "%s", session.busy() ? operation_label(session.operation()) : state_label(snapshot.state));

  if (!seek_editing_) seek_seconds_ = static_cast<float>(snapshot.position_us / 1000000.0);
  const float duration = static_cast<float>(snapshot.media.duration_us / 1000000.0);
  ImGui::BeginDisabled(!loaded || duration <= 0 || session.busy() || session.closing());
  ImGui::SetNextItemWidth(-1);
  const bool changed = ImGui::SliderFloat("##seek", &seek_seconds_, 0, std::max(.001f, duration), "", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
  if (ImGui::IsItemActivated()) seek_editing_ = true;
  if (ImGui::IsItemHovered() || seek_editing_) ImGui::SetTooltip("%s", format_time(static_cast<int64_t>(seek_seconds_ * 1000000)).c_str());
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    session.seek(static_cast<int64_t>(seek_seconds_ * 1000000)); seek_editing_ = false;
  } else if (ImGui::IsItemDeactivated()) seek_editing_ = false;
  (void)changed;
  ImGui::EndDisabled();
  ImGui::TextDisabled("%s / %s", format_time(static_cast<int64_t>(seek_seconds_ * 1000000)).c_str(),
      duration > 0 ? format_time(snapshot.media.duration_us).c_str() : "--:--");
  if (loaded) {
    if (snapshot.media.has_video) { ImGui::SameLine(ImGui::GetWindowWidth() - 315 * s); ImGui::TextDisabled("%d × %d", snapshot.media.width, snapshot.media.height); }
    ImGui::SameLine(ImGui::GetWindowWidth() - 165 * s);
    ImGui::TextDisabled("%s", snapshot.media.has_audio ? "立体声音频" : "无音频");
  }

  ImGui::BeginDisabled(!loaded || session.busy() || session.closing());
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.18f, .56f, .45f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.22f, .66f, .53f, 1));
  if (ImGui::Button(snapshot.state == sdk::PlayerState::playing ? "暂停" : snapshot.state == sdk::PlayerState::ended ? "重新播放" : "播放", {100 * s, 42 * s})) session.toggle_play();
  ImGui::PopStyleColor(2); ImGui::SameLine();
  if (ImGui::Button("停止", {70 * s, 42 * s})) session.stop();
  ImGui::SameLine();
  if (ImGui::Button("-5 秒", {70 * s, 42 * s})) session.seek(snapshot.position_us - 5000000);
  ImGui::SameLine();
  if (ImGui::Button("+5 秒", {70 * s, 42 * s})) session.seek(snapshot.position_us + 5000000);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(session.muted ? "取消静音" : "静音", {95 * s, 42 * s})) session.muted = !session.muted;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::max(70 * s, ImGui::GetContentRegionAvail().x - 110 * s));
  int volume = static_cast<int>(session.volume * 100 + .5f);
  if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) session.volume = volume / 100.0f;
  ImGui::SameLine();
  if (ImGui::Button(fullscreen ? "退出全屏" : "全屏", {95 * s, 42 * s})) toggle_fullscreen(window);

  if (has_message) {
    ImGui::PushStyleColor(ImGuiCol_Text, session.error().empty() ? ImVec4(.93f, .73f, .4f, 1) : ImVec4(1, .47f, .45f, 1));
    ImGui::BeginChild("##error", {0, 42 * s}, ImGuiChildFlags_None);
    ImGui::TextWrapped("%s", session.error().empty() ? session.warning().c_str() : session.error().c_str());
    ImGui::EndChild(); ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  ImGui::TextDisabled("空格 播放/暂停    ← → 跳转    M 静音    F 全屏    Ctrl+O 打开");

  if (path_popup_) { ImGui::OpenPopup("打开文件路径"); path_popup_ = false; }
  ImGui::SetNextWindowSize({560 * s, 0}, ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("打开文件路径", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("输入本地音视频文件的完整路径");
    ImGui::SetNextItemWidth(520 * s);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##path", path_input_.data(), path_input_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(!path_input_[0] || session.busy());
    if (ImGui::Button("打开并播放") || (enter && path_input_[0] && !session.busy())) {
      session.open(path_input_.data()); ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::Button("取消")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::End();
}
}
