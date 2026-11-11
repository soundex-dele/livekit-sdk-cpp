#include "livekit/editor.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <condition_variable>

using namespace livekit::sdk;

namespace {
int failures = 0;

void check(bool ok, const char* name) {
  if (!ok) {
    std::cerr << "FAIL: " << name << '\n';
    ++failures;
  }
}

template <class T>
bool good(const Result<T>& result, const char* name) {
  check(bool(result), name);
  if (!result) {
    std::cerr << result.error().message << '\n';
  }
  return bool(result);
}

EditorProject sample() {
  EditorProject p;
  p.width = 160;
  p.height = 90;
  p.assets.push_back({"a", "test", "C:/media/test.mp4", 5000000, 160, 90, true, true, false});
  p.tracks.push_back({"v1", "Video", false, false, false, false, {{"c1", "a", 0, 0, 60}}});
  p.tracks.push_back({"v2", "Overlay", false, false, false, false, {}});
  return p;
}

void model(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto p = sample();
  EditorDocument d;
  good(d.replace(p), "replace");
  good(d.split("c1", 30, "c2"), "split");
  check(d.project().tracks[0].clips[0].duration == 30 &&
            d.project().tracks[0].clips[1].source_in == 30,
        "split ranges");
  d.mark_saved();
  check(!d.dirty(), "mark saved");
  check(d.undo() && d.dirty(), "undo dirty");
  check(d.redo() && !d.dirty(), "redo saved identity");
  check(!d.move("c2", "v1", 15), "overlap rejected");
  good(d.move("c2", "v2", 15), "cross track move");
  good(d.trim("c2", 20, 40), "trim");
  check(!d.trim("c2", -1, 10), "negative trim rejected");
  p = d.project();
  p.tracks[1].locked = true;
  good(d.edit(p), "lock track");
  check(!d.move("c2", "v1", 90), "locked source move rejected");
  check(!d.trim("c2", 0, 5), "locked trim rejected");
  p = d.project();
  p.tracks[1].locked = false;
  good(d.edit(p), "unlock");
  check(!d.split("c2", 15, "c3"), "split endpoint rejected");
  check(editor_time_us(30000, {30000, 1001}) == 1001000000, "rational frame conversion");
  check(editor_time_us(100000000, {240000, 240000}) == INT64_C(100000000000000),
        "large rational time does not overflow");
  const auto path = dir / "roundtrip.freecut.json";
  good(editor_save(path, d.project()), "save");
  auto loaded = editor_load(path);
  if (good(loaded, "load")) {
    check(loaded->tracks[1].clips[0].source_in == 20 && loaded->rate.numerator == 30,
          "roundtrip values");
  }
  {
    std::ofstream out(dir / "bad.json");
    out << "{\"version\":1}";
  }
  check(!editor_load(dir / "bad.json"), "malformed project rejected");
  auto invalid = sample();
  invalid.tracks[0].clips[0].id.clear();
  check(!d.edit(invalid), "invalid edit rejected");
  const auto before = d.project().tracks[1].clips[0].duration;
  check(before == 40, "failed edits preserve document");
}

void media(const std::filesystem::path& dir) {
  std::shared_ptr<const EditorFrame> latest;
  EditorSession session({[&](auto frame) { latest = frame; }, {}, {}});
  auto red = session.probe((dir / "red.mp4").u8string()).get();
  auto blue = session.probe((dir / "blue.png").u8string()).get();
  auto tone = session.probe((dir / "tone.wav").u8string()).get();
  check(!session.probe((dir / "missing.mp4").u8string()).get(), "missing source rejected");
  {
    std::ofstream corrupt(dir / "corrupt.mp4");
    corrupt << "not a media container";
  }
  check(!session.probe((dir / "corrupt.mp4").u8string()).get(), "corrupt source rejected");
  if (!good(red, "probe video") || !good(blue, "probe image") || !good(tone, "probe audio")) {
    return;
  }
  check(red->asset.video && red->asset.audio && blue->asset.image && tone->asset.audio,
        "probe flags");
  check(red->thumbnail.width == 160, "thumbnail generated");
  EditorProject p;
  p.width = 160;
  p.height = 90;
  p.assets = {red->asset, blue->asset, tone->asset};
  p.tracks = {{"base", "Base", false, false, false, false, {{"red", red->asset.id, 0, 0, 60}}},
              {"overlay",
               "Overlay",
               false,
               false,
               false,
               true,
               {{"blue", blue->asset.id, 15, 0, 30, 0, 0, .5, .5, 1}}},
              {"audio",
               "Audio",
               true,
               false,
               false,
               false,
               {{"tone", tone->asset.id, 0, 0, 60, 0, 0, 1, 1, .25}}}};
  if (!good(session.set_project(p, 7).get(), "set project")) {
    return;
  }
  if (!good(session.seek(30).get(), "paused seek")) {
    return;
  }
  auto copy = latest;
  check(copy && copy->frame == 30 && copy->generation == 7, "paused frame and generation");
  if (copy) {
    const size_t center = (45 * 160 + 80) * 4;
    check(copy->image.rgba[center] > 90 && copy->image.rgba[center + 2] > 90, "alpha composition");
    check(copy->image.rgba[0] > 200 && copy->image.rgba[2] < 30, "scale leaves border");
    check(std::any_of(copy->audio.begin(), copy->audio.end(),
                      [](float f) { return std::abs(f) > .001; }),
          "mixed audio");
  }
  p.tracks[0].muted = true;
  p.tracks[2].muted = true;
  good(session.set_project(p, 8).get(), "mute update");
  good(session.seek(30).get(), "muted seek");
  check(latest &&
            std::all_of(latest->audio.begin(), latest->audio.end(), [](float f) { return f == 0; }),
        "mute silence");
  p.tracks[1].hidden = true;
  good(session.set_project(p, 9).get(), "hide update");
  check(latest && latest->image.rgba[(45 * 160 + 80) * 4 + 2] < 30, "hidden layer");
  p.tracks[0].muted = false;
  p.tracks[2].muted = false;
  p.tracks[1].hidden = false;
  std::vector<AsyncResult<void>> seeks;
  for (int i = 0; i < 200; ++i) {
    seeks.push_back(session.seek(i % 60));
  }
  auto last = session.seek(20);
  for (auto& seek : seeks) {
    auto result = seek.get();
    check(bool(result) || result.error().code == ErrorCode::cancelled, "seek coalescing result");
  }
  good(last.get(), "latest seek survives scrub burst");
  check(latest && latest->frame == 20, "latest scrub frame");
  good(session.seek(60).get(), "seek timeline end for append");
  check(latest && latest->frame == 60 && latest->image.rgba[0] == 0 && latest->audio[0] == 0,
        "exclusive timeline end is black and silent");
  auto gaps = p;
  gaps.tracks[0].clips[0].duration = 10;
  gaps.tracks[1].clips[0].position = 20;
  gaps.tracks[1].clips[0].duration = 10;
  gaps.tracks[2].clips.clear();
  good(session.set_project(gaps, 10).get(), "gap project");
  good(session.seek(15).get(), "seek gap");
  check(latest && latest->image.rgba[0] == 0 && latest->image.rgba[2] == 0 &&
            std::all_of(latest->audio.begin(), latest->audio.end(), [](float f) { return f == 0; }),
        "black and silent gap");
  good(session.seek(20).get(), "seek clip boundary");
  check(latest && latest->image.rgba[(45 * 160 + 80) * 4 + 2] > 100,
        "next clip includes left boundary");
  good(session.set_project(p, 11).get(), "restore export project");
  // Playback uses a separate session so asynchronous frame delivery is fenced
  // before inspecting the vector. No physical sound device is opened.
  std::mutex playback_mutex;
  std::condition_variable playback_done;
  bool ended = false;
  std::vector<int64_t> played;
  {
    EditorSession playback({[&](auto f) {
                              std::lock_guard lock(playback_mutex);
                              played.push_back(f->frame);
                            },
                            {},
                            [&] {
                              std::lock_guard lock(playback_mutex);
                              ended = true;
                              playback_done.notify_one();
                            }});
    good(playback.set_project(gaps, 1).get(), "playback project");
    {
      std::lock_guard lock(playback_mutex);
      played.clear();
    }
    good(playback.play().get(), "play");
    std::unique_lock lock(playback_mutex);
    check(playback_done.wait_for(lock, std::chrono::seconds(5), [&] { return ended; }),
          "playback completes across gap");
    lock.unlock();
    good(playback.pause().get(), "pause fences playback");
  }
  check(played.size() == 30 && played.front() == 0 && played.back() == 29,
        "playback delivers all timeline frames");
  auto offset = session.probe((dir / "offset.mp4").u8string()).get();
  if (good(offset, "probe offset media")) {
    EditorProject delayed;
    delayed.width = 160;
    delayed.height = 90;
    delayed.assets = {offset->asset};
    delayed.tracks = {{"delayed",
                       "Delayed",
                       false,
                       false,
                       false,
                       false,
                       {{"offset", offset->asset.id, 0, 0, 60}}}};
    good(session.set_project(delayed, 12).get(), "delayed video project");
    check(latest && latest->image.rgba[1] == 0, "video before its first timestamp stays black");
    good(session.seek(20).get(), "seek into delayed video");
    check(latest && latest->image.rgba[1] > 70, "delayed video appears at its timestamp");
  }
  EditorExport exporter;
  good(exporter.start(p, dir / "output.mp4").get(), "export MP4");
  auto exported = session.probe((dir / "output.mp4").u8string()).get();
  if (good(exported, "probe output")) {
    check(exported->asset.audio && exported->asset.video, "output streams");
    check(std::abs(exported->asset.duration_us - 2000000) < 33334, "output duration");
  }
  const auto protected_path = dir / "protected.mp4";
  {
    std::ofstream out(protected_path);
    out << "original";
  }
  auto cancelled = exporter
                       .start(p, protected_path,
                              [&](auto progress) {
                                if (progress.completed_frames == 1) {
                                  exporter.cancel();
                                }
                              })
                       .get();
  check(!cancelled && cancelled.error().code == ErrorCode::cancelled, "cancel export");
  {
    std::ifstream in(protected_path);
    std::string text;
    in >> text;
    check(text == "original", "cancel preserves existing output");
  }
  check(!exporter.start(p, dir / "missing-dir" / "output.mp4").get(), "invalid output directory");
  check(!exporter.start(p, dir / "red.mp4").get(), "source overwrite rejected");
  auto fractional = p;
  fractional.rate = {30000, 1001};
  fractional.tracks[0].clips[0].duration = 30;
  fractional.tracks.resize(1);
  good(exporter.start(fractional, dir / "fractional.mp4").get(), "fractional frame rate export");
  p.assets[0].path = (dir / "missing.mp4").u8string();
  check(!exporter.start(p, dir / "missing-output.mp4").get(), "missing source fails export");
  check(!std::filesystem::exists(dir / "missing-output.mp4"), "failed export not published");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  const auto dir = std::filesystem::u8path(argv[2]);
  if (std::string(argv[1]) == "model") {
    model(dir);
  } else {
    media(dir);
  }
  std::cout << "Editor failures: " << failures << '\n';
  return failures ? 1 : 0;
}
