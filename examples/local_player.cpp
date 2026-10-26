#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "utoolkit/logging/logger.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {
struct Options {
  std::string path, screenshot;
  bool auto_exit = false, hidden = false;
  int frames = 0;
};
void check(bool ok) { if (!ok) throw std::runtime_error(SDL_GetError()); }
void screenshot(SDL_Renderer* renderer, const std::string& filename) {
  int width, height; check(SDL_GetRendererOutputSize(renderer, &width, &height) == 0);
  auto* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  check(surface != nullptr);
  const bool read = SDL_RenderReadPixels(renderer, nullptr, surface->format->format, surface->pixels, surface->pitch) == 0;
  const int saved = read ? SDL_SaveBMP(surface, filename.c_str()) : -1;
  SDL_FreeSurface(surface); check(read && saved == 0);
}
int run(const Options& options) {
  SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
  bool platform = false, backend = false;
  int result = 0;
  try {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
    float dpi = 96; if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0) dpi = 96;
    const float scale = std::clamp(dpi / 96, 1.0f, 2.5f);
    window = SDL_CreateWindow("AV Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(1080 * scale), static_cast<int>(740 * scale), SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_ALLOW_HIGHDPI | (options.hidden ? SDL_WINDOW_HIDDEN : 0));
    check(window != nullptr);
    SDL_SetWindowMinimumSize(window, static_cast<int>(800 * scale), static_cast<int>(520 * scale));
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    check(renderer != nullptr);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    player_demo::load_font(scale); player_demo::apply_style(scale);
    platform = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    backend = ImGui_ImplSDLRenderer2_Init(renderer);
    if (!platform || !backend) throw std::runtime_error("Cannot initialize ImGui SDL backends");
    {
      player_demo::Session session(renderer); player_demo::View view;
      if (!options.path.empty()) session.open(options.path);
      int frame_count = 0;
      bool captured = false;
      while (!session.finished()) {
        const auto started = SDL_GetTicks64();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
          ImGui_ImplSDL2_ProcessEvent(&event);
          if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) session.close();
          if (event.type == SDL_DROPFILE) {
            session.open(event.drop.file); SDL_free(event.drop.file);
          }
          if (event.type != SDL_KEYDOWN || event.key.repeat || ImGui::GetIO().WantTextInput ||
              ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) continue;
          switch (event.key.keysym.sym) {
            case SDLK_SPACE: session.toggle_play(); break;
            case SDLK_LEFT: session.seek(session.snapshot().position_us - 5000000); break;
            case SDLK_RIGHT: session.seek(session.snapshot().position_us + 5000000); break;
            case SDLK_s: session.stop(); break;
            case SDLK_m: session.muted = !session.muted; break;
            case SDLK_f: view.toggle_fullscreen(window); break;
            case SDLK_ESCAPE: if (view.fullscreen) view.toggle_fullscreen(window); else session.close(); break;
            case SDLK_o: if (event.key.keysym.mod & KMOD_CTRL) view.browse(session, window); break;
          }
        }
        session.update();
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        view.draw(session, window);
        ImGui::Render(); SDL_SetRenderDrawColor(renderer, 14, 17, 23, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        ++frame_count;
        if (!options.screenshot.empty() && !captured && frame_count > 3 &&
            (options.path.empty() || session.texture() || !session.error().empty() || session.snapshot().media.has_audio)) {
          screenshot(renderer, options.screenshot); captured = true;
        }
        SDL_RenderPresent(renderer);
        if (options.auto_exit && !session.error().empty()) { result = 1; std::cerr << session.error() << '\n'; session.close(); }
        if (options.auto_exit && session.drained()) session.close();
        if (options.frames > 0 && frame_count >= options.frames) session.close();
        const auto elapsed = SDL_GetTicks64() - started;
        if (elapsed < 8) SDL_Delay(static_cast<Uint32>(8 - elapsed));
      }
    }
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
  if (backend) ImGui_ImplSDLRenderer2_Shutdown();
  if (platform) ImGui_ImplSDL2_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
  return result;
}
int entry(const std::vector<std::string>& args) {
  try {
    utoolkit::logging::Logger::GetInstance().Initialize(
        "cli_test", "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] %v",
        utoolkit::logging::LogLevel::Trace);
    Options options;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--auto-exit") options.auto_exit = true;
      else if (args[i] == "--hidden") options.hidden = true;
      else if (args[i] == "--screenshot" && i + 1 < args.size()) options.screenshot = args[++i];
      else if (args[i] == "--frames" && i + 1 < args.size()) options.frames = std::stoi(args[++i]);
      else if (args[i] == "--help") {
        std::cout << "Usage: livekit_local_player [local-file] [--auto-exit] [--hidden] [--screenshot output.bmp] [--frames count]\n"; return 0;
      } else if (options.path.empty() && !args[i].starts_with("--")) options.path = args[i];
      else throw std::invalid_argument("Unknown argument: " + args[i]);
    }
    if (options.auto_exit && options.path.empty()) throw std::invalid_argument("--auto-exit requires a media file");
    return run(options);
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) { auto text = std::filesystem::path(argv[i]).u8string(); args.emplace_back(text.begin(), text.end()); }
  return entry(args);
}
#else
int main(int argc, char** argv) { return entry({argv, argv + argc}); }
#endif
