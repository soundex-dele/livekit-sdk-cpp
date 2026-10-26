#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <filesystem>
#include <iostream>
#include <cmath>

static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static std::shared_ptr<demo::sdk::VideoFrame> pixels(const std::string& id, int w = 321, int h = 181) {
  auto f = std::make_shared<demo::sdk::VideoFrame>(); f->track_id = id; f->width = w; f->height = h;
  f->stride_y = w + 7; f->stride_u = (w + 1) / 2 + 9; f->stride_v = (w + 1) / 2 + 11;
  f->y.assign(f->stride_y * h, 81); f->u.assign(f->stride_u * ((h + 1) / 2), 90); f->v.assign(f->stride_v * ((h + 1) / 2), 240); return f;
}
int main(int argc, char** argv) {
  try {
    SDL_SetMainReady(); check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL init");
    auto* window = SDL_CreateWindow("Render test", 0, 0, 1100, 720, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    check(window, "window"); auto* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE); check(renderer, "renderer");
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer); ImGui_ImplSDLRenderer2_Init(renderer); demo::apply_style(1);
    {
      demo::VideoTexture texture; auto frame = pixels("TR_one"); texture.upload(renderer, *frame);
      SDL_RenderCopy(renderer, texture.get(), nullptr, nullptr);
      std::vector<uint32_t> buffer(1100 * 720);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, buffer.data(), 1100 * 4) == 0, "readback");
      auto center = buffer[360 * 1100 + 550];
      check(((center >> 16) & 255) > 220 && ((center >> 8) & 255) < 35 && (center & 255) < 35, "I420 red: planes/colors swapped");
      texture.upload(renderer, *pixels("TR_one", 181, 321)); check(texture.width() == 181, "resolution recreation");
      auto fit = demo::fit_video(0, 0, 500, 500, 640, 360);
      check(std::abs(fit.w - 500) < .01f && std::abs(fit.h - 281.25f) < .01f && fit.y > 100, "aspect ratio");
      auto invalid = *frame; invalid.v.clear(); bool rejected = false;
      try { texture.upload(renderer, invalid); } catch (...) { rejected = true; } check(rejected, "invalid buffer validation");
      demo::Snapshot state; state.state = demo::State::Connected; state.finished = false; state.room = "demo-room";
      demo::sdk::ParticipantInfo local; local.sid = "PA_me"; local.identity = "viewer";
      demo::sdk::ParticipantInfo alice; alice.sid = "PA_alice"; alice.identity = "alice"; alice.name = "Alice";
      demo::sdk::TrackInfo t; t.sid = "TR_one"; t.kind = demo::sdk::TrackKind::video; t.subscribed = true; alice.tracks.push_back(t);
      demo::sdk::ParticipantInfo bob = alice; bob.sid = "PA_bob"; bob.identity = "bob"; bob.name = "Bob"; bob.tracks[0].sid = "TR_two";
      demo::sdk::ParticipantInfo guest; guest.sid = "PA_guest"; guest.identity = "guest";
      state.frames[t.sid] = frame; // pixels may precede participant metadata
      state.update(local, {alice, bob, guest});
      check(demo::tiles(state).size() == 3, "roster tile count");
      state.frames["TR_two"] = pixels("TR_two");
      demo::View view;
      view.video_count = 3; view.tone = true;
      auto publishing = view.options();
      check(publishing.media.size() == 4 && publishing.media[0].video && publishing.media[2].video && !publishing.media[3].video, "Multiple videos and independent tone");
      view.video_count = -1; check(view.options().media.size() == 1, "Negative video count");
      view.video_count = 99; check(view.options().media.size() == demo::max_test_videos + 1, "Video count limit");
      view.video_count = 2; view.tone = false;
      auto extra = t; extra.sid = "TR_three"; extra.name = "Test video 2";
      alice.tracks[0].name = "Test video 1"; alice.tracks.push_back(extra);
      local.tracks = alice.tracks; local.tracks[0].sid = "TR_local1"; local.tracks[1].sid = "TR_local2";
      state.update(local, {alice, bob, guest}); state.frames[extra.sid] = pixels(extra.sid);
      check(demo::tiles(state).size() == 4 && demo::tiles(state)[0].label == "Alice / Test video 1" && demo::tiles(state)[1].label == "Alice / Test video 2", "Same participant video labels");
      auto render = [&](demo::Snapshot snapshot) {
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        view.draw(renderer, snapshot); ImGui::Render(); SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
        demo::render_draw_data(renderer);
      };
      render(state); render(state);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, buffer.data(), 1100 * 4) == 0, "ImGui readback");
      size_t red = 0; for (auto c : buffer) if (((c >> 16) & 255) > 220 && ((c >> 8) & 255) < 35 && (c & 255) < 35) ++red;
      check(red > 30000, "ImGui did not render video textures");
      if (argc > 1) {
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(buffer.data(), 1100, 720, 32, 1100 * 4, SDL_PIXELFORMAT_ARGB8888);
        check(SDL_SaveBMP(surface, argv[1]) == 0, "save visual QA"); SDL_FreeSurface(surface);
      }
      // Emulate a Retina backing buffer without opening a physical window.
      auto* retina = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_TARGET, 2200, 1440);
      check(retina, "Retina target");
      check(SDL_SetRenderTarget(renderer, retina) == 0, "Retina render target");
      ImGui_ImplSDLRenderer2_DestroyFontsTexture(); demo::apply_style(1, 2);
      render(state); render(state);
      check(std::abs(ImGui::GetIO().DisplayFramebufferScale.x - 2) < .01f, "Retina framebuffer scale");
      check(std::abs(ImGui::GetIO().FontDefault->FontSize * ImGui::GetIO().FontGlobalScale - 13) < .01f,
        "Retina must preserve logical font size");
      std::vector<uint32_t> retina_buffer(2200 * 1440);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
        retina_buffer.data(), 2200 * 4) == 0, "Retina readback");
      size_t retina_red = 0;
      for (auto c : retina_buffer)
        if (((c >> 16) & 255) > 220 && ((c >> 8) & 255) < 35 && (c & 255) < 35) ++retina_red;
      if (argc > 1) {
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(retina_buffer.data(), 2200, 1440,
          32, 2200 * 4, SDL_PIXELFORMAT_ARGB8888);
        auto path = std::filesystem::path(argv[1]); path.replace_extension("retina.bmp");
        check(SDL_SaveBMP(surface, path.string().c_str()) == 0, "save Retina visual QA"); SDL_FreeSurface(surface);
      }
      if (!(retina_red > red * 3.8 && retina_red < red * 4.2))
        std::cerr << "Red pixels: 1x=" << red << " 2x=" << retina_red << '\n';
      check(retina_red > red * 3.8 && retina_red < red * 4.2, "Retina video layout/scale");
      check(SDL_SetRenderTarget(renderer, nullptr) == 0, "restore render target");
      SDL_DestroyTexture(retina);
      ImGui_ImplSDLRenderer2_DestroyFontsTexture(); demo::apply_style(1);
      alice.tracks[0].muted = true; state.update(local, {alice, bob, guest});
      check(demo::tiles(state)[0].status == "Video muted", "mute placeholder"); render(state);
      alice.tracks.erase(alice.tracks.begin()); state.update(local, {alice, bob, guest});
      check(!state.frames.contains("TR_one") && state.frames.contains("TR_three"), "Stopping one stream removed another");
      state.changing_video = true; render(state); state.changing_video = false;
      state.update(local, {bob, guest}); check(!state.frames.contains("TR_one"), "departed pixels retained"); render(state);
      SDL_SetWindowSize(window, 640, 480); render(state);
      render({}); // idle view and texture teardown
    }
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    std::cout << "Demo model, I420 and ImGui renderer tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
