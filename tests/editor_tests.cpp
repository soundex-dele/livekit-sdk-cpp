#include "livekit/editor.hpp"
#include "../src/engines/editor/editor_internal.hpp"
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

void advanced(const std::filesystem::path& dir) {
  auto p = sample();
  p.main_track_id = "v1";
  p.magnetic = true;
  EditorDocument d;
  good(d.replace(p), "magnetic replace");
  p.tracks[0].clips.push_back({"c2", "a", 90, 0, 30});
  good(d.edit(p), "magnetic append");
  check(d.project().tracks[0].clips[1].position == 60, "magnetic closes gap");
  check(d.undo() && d.project().tracks[0].clips.size() == 1, "magnetic transaction undo");
  p = d.project();
  p.tracks[0].clips[0].animations = {{"x", {{0, 0}, {60, 1}}}};
  good(d.edit(p), "keyframes accepted");
  good(d.split("c1", 30, "right"), "animated split");
  check(std::abs(editor_evaluate(d.project().tracks[0].clips[1], 0).x - .5) < 1e-9,
        "split animation continuity");
  good(editor_save(dir / "v2.freecut.json", d.project()), "v2 save");
  auto loaded = editor_load(dir / "v2.freecut.json");
  check(loaded && loaded->magnetic && loaded->tracks[0].clips[0].animations.size() == 1,
        "v2 effect roundtrip");
  {
    std::ofstream old(dir / "legacy.freecut.json");
    old << R"({"version":1,"name":"Legacy","width":320,"height":180,"fps_num":30,"fps_den":1,"assets":[],"tracks":[]})";
  }
  auto legacy = editor_load(dir / "legacy.freecut.json");
  check(legacy && !legacy->magnetic, "v1 retains nonmagnetic behavior");
  {
    std::ofstream future(dir / "future.freecut.json");
    future
        << R"({"version":3,"name":"Future","width":320,"height":180,"fps_num":30,"fps_den":1,"assets":[],"tracks":[]})";
  }
  check(!editor_load(dir / "future.freecut.json"), "future schema refused");
  EditorSession probe;
  auto red = probe.probe((dir / "red.mp4").u8string()).get();
  auto blue = probe.probe((dir / "blue.png").u8string()).get();
  if (!good(red, "advanced video") || !good(blue, "advanced image")) {
    return;
  }
  check(!red->waveform.empty(), "real waveform generated");
  p = {};
  p.width = 320;
  p.height = 180;
  p.assets = {red->asset, blue->asset};
  p.tracks = {{"v",
               "Video",
               false,
               false,
               false,
               false,
               {{"red", red->asset.id, 0, 0, 30}, {"blue", blue->asset.id, 30, 0, 30}}}};
  p.tracks[0].clips[0].transition = {"blue", "fade", 10};
  good(editor_validate(p), "transition handles valid");
  auto invalid_transition = p;
  invalid_transition.tracks[0].clips[0].transition.duration = 120;
  check(!editor_validate(invalid_transition), "insufficient transition handles rejected");
  std::atomic<bool> cancel{false};
  editor_detail::RenderGraph graph(p, cancel);
  auto start = graph.render(25, 1), middle = graph.render(30, 1), end = graph.render(34, 1);
  if (good(start, "transition start") && good(middle, "transition midpoint") &&
      good(end, "transition end")) {
    check((*start)->image.rgba[0] > 200 && (*end)->image.rgba[2] > 200, "transition endpoints");
    check((*middle)->image.rgba[0] > 50 && (*middle)->image.rgba[2] > 50,
          "transition blends pixels");
  }
  p.tracks[0].clips.resize(1);
  auto& clip = p.tracks[0].clips[0];
  clip.transition = {};
  clip.color.saturation = 0;
  editor_detail::RenderGraph gray(p, cancel);
  auto gray_frame = gray.render(5, 1);
  if (good(gray_frame, "native color grade")) {
    check(std::abs(int((*gray_frame)->image.rgba[0]) - int((*gray_frame)->image.rgba[1])) < 2,
          "zero saturation gray");
  }
  clip.color = {};
  clip.speed = 2;
  clip.duration = 30;
  clip.fade_in = 10;
  editor_detail::RenderGraph speed(p, cancel);
  auto fast = speed.render(5, 1);
  good(fast, "native time stretch with fade");
  EditorTrack text;
  text.id = "text";
  text.name = "Text";
  EditorClip title;
  title.id = "title";
  title.duration = 30;
  title.text.content = "FreeCut 中文";
  title.text.size = 28;
  text.clips.push_back(title);
  p.tracks.push_back(text);
  editor_detail::RenderGraph subtitles(p, cancel);
  auto caption = subtitles.render(5, 1);
  if (good(caption, "libass Chinese subtitle") && fast) {
    check((*caption)->image.rgba != (*fast)->image.rgba, "subtitle changes rendered pixels");
  }
  EditorExport exporter;
  good(editor_save(dir / "advanced.freecut.json", p), "save advanced editable project");
  good(exporter.start(p, dir / "advanced.mp4").get(), "advanced export");
  auto exported = probe.probe((dir / "advanced.mp4").u8string()).get();
  good(exported, "advanced exported media readable");
  if (exported && caption) {
    EditorProject verification;
    verification.width = p.width;
    verification.height = p.height;
    verification.assets = {exported->asset};
    verification.tracks = {
        {"v", "Export", false, false, false, false, {{"check", exported->asset.id, 0, 0, 30}}}};
    editor_detail::RenderGraph decoded(verification, cancel);
    auto actual = decoded.render(5, 1);
    if (good(actual, "decode exported subtitle frame")) {
      double error = 0;
      for (size_t i = 0; i < (*actual)->image.rgba.size(); ++i) {
        error += std::abs(int((*actual)->image.rgba[i]) - int((*caption)->image.rgba[i]));
      }
      check(error / (*actual)->image.rgba.size() < 12, "preview and H264 export agree");
    }
  }
  p.tracks.resize(1);
  auto& tone_clip = p.tracks[0].clips[0];
  tone_clip.fade_in = 0;
  editor_detail::RenderGraph pitch(p, cancel);
  int crossings = 0;
  float previous = 0;
  size_t samples = 0;
  for (int frame = 5; frame < 20; ++frame) {
    auto audio = pitch.render(frame, 1);
    if (!good(audio, "pitch render")) {
      break;
    }
    for (size_t i = 0; i < (*audio)->audio.size(); i += 2) {
      float value = (*audio)->audio[i];
      if (previous <= 0 && value > 0) {
        ++crossings;
      }
      previous = value;
      ++samples;
    }
  }
  const double frequency = samples ? crossings * 48000. / samples : 0;
  check(std::abs(frequency - 440) < 25, "2x speed preserves 440Hz pitch");
  auto again = pitch.render(5, 2);
  editor_detail::RenderGraph fresh(p, cancel);
  auto expected = fresh.render(5, 2);
  check(again && expected && (*again)->audio == (*expected)->audio, "seek audio deterministic");
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

void hot_reload(const std::filesystem::path& dir) {
  std::atomic<bool> cancel{false};
  auto red = editor_detail::probe((dir / "red.mp4").u8string(), cancel);
  auto blue = editor_detail::probe((dir / "blue.png").u8string(), cancel);
  if (!good(red, "reload red fixture") || !good(blue, "reload blue fixture")) {
    return;
  }
  EditorProject p;
  p.width = 160;
  p.height = 90;
  p.assets = {red->asset};
  p.tracks = {{"video", "Video", false, false, false, false, {{"clip", red->asset.id, 0, 0, 60}}}};
  editor_detail::RenderGraph graph(p, cancel);
  if (!good(graph.render(5, 1), "warm graph")) {
    return;
  }
  const auto initial = graph.stats();
  check(initial.decoder_opens == 2 && initial.cached_decoders == 2, "video/audio decoders warmed");

  auto matches_cold = [&](const EditorProject& project, int64_t frame) {
    auto actual = graph.render(frame, 42);
    editor_detail::RenderGraph cold(project, cancel);
    auto expected = cold.render(frame, 42);
    if (!good(actual, "hot render") || !good(expected, "cold reference")) {
      return;
    }
    check((*actual)->generation == 42, "new generation delivered");
    check((*actual)->image.rgba == (*expected)->image.rgba, "hot image equals cold image");
    check((*actual)->audio.size() == (*expected)->audio.size(), "new rate audio block size");
    if ((*actual)->audio.size() == (*expected)->audio.size()) {
      double difference = 0;
      for (size_t i = 0; i < (*actual)->audio.size(); ++i) {
        difference += std::abs((*actual)->audio[i] - (*expected)->audio[i]);
      }
      const double mean = difference / std::max<size_t>(1, (*actual)->audio.size());
      if (mean >= .0001) {
        std::cerr << "audio difference=" << mean << " frame=" << frame
                  << " speed=" << project.tracks[0].clips[0].speed
                  << " source=" << project.tracks[0].clips[0].source_in << '\n';
      }
      check(mean < .0001, "hot audio agrees with cold reference");
    }
  };

  p.tracks[0].clips[0].opacity = .5;
  good(graph.update_project(p), "hot opacity edit");
  matches_cold(p, 5);
  check(graph.stats().decoder_opens == initial.decoder_opens, "opacity edit retains decoders");
  check(graph.stats().topology_reloads == 1, "same graph hot-reloaded");
  auto invalid = p;
  invalid.width = 0;
  check(!graph.update_project(invalid), "invalid edit rejected");
  check(graph.stats().topology_reloads == 1, "invalid edit preserves old graph");
  matches_cold(p, 5);

  // Same clip ID, changed speed/source/rate: cached stretched audio must not survive.
  p.tracks[0].clips[0].speed = 2;
  p.tracks[0].clips[0].duration = 20;
  good(graph.update_project(p), "hot speed 2");
  matches_cold(p, 5);
  p.tracks[0].clips[0].speed = .5;
  p.tracks[0].clips[0].source_in = 5;
  p.rate = {24, 1};
  good(graph.update_project(p), "hot speed source and rate");
  matches_cold(p, 5);
  check(graph.stats().decoder_opens == initial.decoder_opens, "timing edits retain decoders");

  EditorClip title;
  title.id = "title";
  title.duration = 20;
  title.text.content = "Before";
  title.text.size = 20;
  p.tracks.push_back({"titles", "Titles", false, false, false, false, {title}});
  good(graph.update_project(p), "add title");
  auto before = graph.render(5, 3);
  p.tracks[1].clips[0].text.content = "After";
  good(graph.update_project(p), "edit same title ID");
  auto after = graph.render(5, 4);
  if (good(before, "old text") && good(after, "new text")) {
    check((*before)->image.rgba != (*after)->image.rgba, "same-ID text cache invalidated");
  }
  matches_cold(p, 5);
  p.width = 320;
  p.height = 180;
  good(graph.update_project(p), "hot canvas resize");
  matches_cold(p, 5);

  // Relink the same asset ID to another source; both old decoders must be retired.
  const auto asset_id = p.assets[0].id;
  p.assets[0] = blue->asset;
  p.assets[0].id = asset_id;
  good(graph.update_project(p), "same-ID relink");
  check(graph.stats().cached_decoders == 0, "relinked decoders retired");
  matches_cold(p, 5);
  check(graph.stats().decoder_opens == initial.decoder_opens + 1, "new image decoder opened");

  // Detect an on-disk change even when the asset ID/path/metadata are unchanged.
  const auto local = dir / "reload-image.png";
  std::error_code ec;
  std::filesystem::copy_file(dir / "blue.png", local,
                             std::filesystem::copy_options::overwrite_existing, ec);
  check(!ec, "copy local reload fixture");
  p.assets[0].path = local.u8string();
  good(graph.update_project(p), "local image source");
  good(graph.render(5, 5), "warm local image");
  const auto old_stamp = std::filesystem::last_write_time(local, ec);
  check(!ec, "read local image stamp");
  std::filesystem::last_write_time(local, old_stamp + std::chrono::seconds(2), ec);
  check(!ec, "change local image stamp");
  good(graph.update_project(p), "same-path disk change");
  check(graph.stats().cached_decoders == 0, "changed file invalidates decoder");
  matches_cold(p, 5);
  good(graph.update_project(EditorProject{}), "clear project");
  check(graph.stats().cached_decoders == 0, "removed asset releases decoder");

  // The actual main GraphEngine owns algorithms and converters. Parameter edits
  // replace only affected algorithms; unchanged nodes keep their instances.
  auto instance = [&](const std::string& id) -> uintptr_t {
    for (const auto& node : graph.stats().nodes) {
      if (node.id == id) {
        return node.instance;
      }
    }
    return 0;
  };
  check(graph.stats().nodes.size() == 7, "empty project has only main stages");
  p = {};
  p.width = 160;
  p.height = 90;
  p.assets = {red->asset, blue->asset};
  p.tracks = {{"video",
               "Video",
               false,
               false,
               false,
               false,
               {{"red", red->asset.id, 0, 0, 30}, {"blue", blue->asset.id, 30, 0, 30}}}};
  p.tracks[0].clips[0].color.saturation = .4;
  good(graph.update_project(p), "add color algorithm branch");
  const std::string color = "editor.clip.3:red.color";
  const auto converter = instance(color + ".rgba_to_rgb");
  const auto grade = instance(color + ".grade");
  const auto saturation = instance(color + ".saturation_contrast");
  const auto main_stage = instance("editor.decode");
  check(graph.stats().nodes.size() == 11 && converter && grade && saturation &&
            instance(color + ".rgb_to_rgba"),
        "color algorithms and converters in main graph");
  auto connected = [&](const std::string& from, const std::string& to) {
    for (const auto& node : graph.stats().nodes) {
      if (node.id == from) {
        return std::find(node.outputs.begin(), node.outputs.end(), from + "->" + to) !=
               node.outputs.end();
      }
    }
    return false;
  };
  check(connected("editor.decode", "editor.route") &&
            connected("editor.route", color + ".rgba_to_rgb") &&
            connected(color + ".rgba_to_rgb", color + ".grade") &&
            connected(color + ".grade", color + ".saturation_contrast") &&
            connected(color + ".saturation_contrast", color + ".rgb_to_rgba") &&
            connected(color + ".rgb_to_rgba", "editor.composite") &&
            !connected("editor.decode", "editor.composite"),
        "conversion and algorithm edges belong to main graph");
  matches_cold(p, 5);
  p.tracks[0].clips[0].color.brightness = .2;
  good(graph.update_project(p), "change color algorithm parameters");
  check(instance(color + ".grade") != grade && instance(color + ".rgba_to_rgb") == converter &&
            instance(color + ".saturation_contrast") == saturation &&
            instance("editor.decode") == main_stage && graph.stats().reused_effect_nodes == 3,
        "only changed algorithm replaced, converters and main stage reused");
  matches_cold(p, 5);
  // A different clip must not receive this clip's effect or stale result.
  matches_cold(p, 35);
  p.tracks[0].clips[1].color.temperature = .5;
  good(graph.update_project(p), "add second clip color branch");
  check(graph.stats().nodes.size() == 15, "per-clip branches coexist");
  matches_cold(p, 35);
  matches_cold(p, 5);
  p.tracks[0].clips[1].color = {};
  p.tracks[0].clips[0].speed = 2;
  p.tracks[0].clips[0].duration = 20;
  good(graph.update_project(p), "add audio algorithm branch");
  const std::string audio = "editor.clip.3:red.audio";
  check(instance(audio + ".float_to_audio") && instance(audio + ".time_stretch") &&
            instance(audio + ".audio_to_float"),
        "audio algorithm and converters in main graph");
  check(connected(color + ".rgb_to_rgba", audio + ".float_to_audio") &&
            connected(audio + ".audio_to_float", audio + ".window") &&
            connected(audio + ".window", "editor.composite") &&
            connected(audio + ".float_to_audio", audio + ".window") &&
            connected(color + ".rgba_to_rgb", audio + ".float_to_audio"),
        "graph edges order color, stretch, audio window and composition");
  matches_cold(p, 5);
  // Cache hits and absent media still traverse explicit bypass edges to output.
  matches_cold(p, 6);
  p.tracks[0].hidden = true;
  good(graph.update_project(p), "hide video with active algorithms");
  matches_cold(p, 5);
  p.tracks[0].muted = true;
  good(graph.update_project(p), "mute audio with active algorithms");
  matches_cold(p, 6);
  p.tracks[0].hidden = false;
  p.tracks[0].muted = false;
  p.tracks[0].clips[0].fade_in = 8;
  good(graph.update_project(p), "add audio fade algorithm");
  check(instance("editor.clip.3:red.fade.audio_fade"), "fade is a main graph node");
  matches_cold(p, 3);
  p.tracks[0].clips[0].fade_in = 0;
  p.tracks[0].clips[0].speed = 1;
  p.tracks[0].clips[0].duration = 30;
  p.tracks[0].clips[0].transition = {"blue", "fade", 10};
  good(graph.update_project(p), "remove stretch and add transition branch");
  const std::string transition = "editor.clip.3:red.transition";
  const auto transition_converter = instance(transition + ".rgba_to_rgb");
  const auto transition_algorithm = instance(transition + ".transition");
  check(!instance(audio + ".time_stretch") && transition_converter && transition_algorithm &&
            instance(transition + ".rgb_to_rgba"),
        "dynamic audio removal and transition addition");
  check(connected("editor.result_route", transition + ".join") &&
            connected(transition + ".join", transition + ".rgba_to_rgb") &&
            connected(transition + ".rgb_to_rgba", transition + ".commit") &&
            connected(transition + ".commit", "editor.output"),
        "transition joins both layer events and publishes through to output");
  for (const auto frame : {25, 29, 34}) {
    matches_cold(p, frame);
  }
  p.tracks[0].clips[0].transition.effect = "wipe";
  good(graph.update_project(p), "replace transition algorithm");
  check(instance(transition + ".transition") != transition_algorithm &&
            instance(transition + ".rgba_to_rgb") == transition_converter,
        "transition replacement reuses converter");
  matches_cold(p, 29);
  p.tracks[0].clips[0].transition = {};
  p.tracks[0].clips[0].color = {};
  good(graph.update_project(p), "remove all algorithm branches");
  check(graph.stats().nodes.size() == 7 && instance("editor.decode") == main_stage,
        "effect removal retains main stages");
  matches_cold(p, 5);
  p.tracks[0].clips[0].color.contrast = 1.2;
  good(graph.update_project(p), "re-add removed effect");
  matches_cold(p, 5);
  p.tracks[0].clips.erase(p.tracks[0].clips.begin());
  good(graph.update_project(p), "delete clip with effect");
  check(graph.stats().nodes.size() == 7, "deleted clip retires its algorithm nodes");
  matches_cold(p, 35);

  // Exercise the public session path, including continuing and pausing playback.
  p = {};
  p.width = 160;
  p.height = 90;
  p.assets = {blue->asset};
  p.tracks = {
      {"video", "Video", false, false, false, false, {{"clip", blue->asset.id, 0, 0, 120}}}};
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::pair<uint64_t, int64_t>> delivered;
  int ended = 0;
  EditorSession session({[&](auto frame) {
                           std::lock_guard lock(mutex);
                           delivered.emplace_back(frame->generation, frame->frame);
                           ready.notify_all();
                         },
                         {},
                         [&] {
                           std::lock_guard lock(mutex);
                           ++ended;
                           ready.notify_all();
                         }});
  good(session.set_project(p, 100, 10, true).get(), "start playback at requested frame");
  {
    std::unique_lock lock(mutex);
    check(ready.wait_for(lock, std::chrono::seconds(3),
                         [&] {
                           return !delivered.empty() && delivered.back().first == 100 &&
                                  delivered.back().second >= 12;
                         }),
          "playback advances before edit");
  }
  p.tracks[0].clips[0].opacity = .25;
  good(session.set_project(p, 101, 12, true).get(), "live edit keeps playback running");
  {
    std::unique_lock lock(mutex);
    check(ready.wait_for(lock, std::chrono::seconds(3),
                         [&] {
                           return !delivered.empty() && delivered.back().first == 101 &&
                                  delivered.back().second >= 15;
                         }),
          "playback advances after edit without play call");
  }
  good(session.pause().get(), "pause fences callbacks");
  {
    std::lock_guard lock(mutex);
    int64_t expected = 12;
    bool new_generation = false;
    for (const auto& entry : delivered) {
      if (entry.first == 101) {
        new_generation = true;
        check(entry.second == expected++, "hot update has no duplicated playback frame");
      } else {
        check(!new_generation, "old generation does not follow new generation");
      }
    }
    check(new_generation, "new generation observed");
  }
  good(session.set_project(p, 102, 120, true).get(), "editing at exclusive end stops playback");
  {
    std::lock_guard lock(mutex);
    check(ended == 1 && delivered.back() == std::make_pair(uint64_t(102), int64_t(120)),
          "end update delivers terminal frame and ends once");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  const auto dir = std::filesystem::u8path(argv[2]);
  if (std::string(argv[1]) == "model") {
    model(dir);
  } else if (std::string(argv[1]) == "hot_reload") {
    hot_reload(dir);
  } else if (std::string(argv[1]) == "advanced") {
    advanced(dir);
  } else {
    media(dir);
  }
  std::cout << "Editor failures: " << failures << '\n';
  return failures ? 1 : 0;
}
