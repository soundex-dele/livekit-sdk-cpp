#pragma once

#include "livekit/async_result.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace livekit::sdk {

struct EditorRate {
  int numerator = 30;
  int denominator = 1;
};

struct EditorImage {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

struct EditorAsset {
  std::string id, name, path;
  int64_t duration_us = 0;
  int width = 0, height = 0;
  bool video = false, audio = false, image = false;
};

enum class EditorInterpolation { linear, smooth };

struct EditorKeyframe {
  int64_t frame = 0;
  double value = 0;
  EditorInterpolation interpolation = EditorInterpolation::linear;
};

struct EditorAnimation {
  std::string property;
  std::vector<EditorKeyframe> keys;
};

struct EditorText {
  std::string content, font = "Microsoft YaHei";
  int size = 48, alignment = 2;
  uint32_t color = 0xFFFFFF, outline_color = 0;
  double outline = 2;
};

struct EditorColor {
  double brightness = 0, contrast = 1, saturation = 1, temperature = 0;
};

struct EditorTransition {
  std::string next_clip_id, effect = "fade";
  int64_t duration = 0;
};

struct EditorClip {
  std::string id, asset_id;
  int64_t position = 0, source_in = 0, duration = 1;  // Project frames; [in, out).
  double x = 0, y = 0, scale = 1, opacity = 1, volume = 1;
  double rotation = 0, speed = 1;
  int64_t fade_in = 0, fade_out = 0;
  EditorText text;
  EditorColor color;
  EditorTransition transition;
  std::vector<EditorAnimation> animations;
};

struct EditorTrack {
  std::string id, name;
  bool audio_only = false, locked = false, hidden = false, muted = false;
  std::vector<EditorClip> clips;
};

struct EditorProject {
  std::string name = "未命名工程";
  int width = 1920, height = 1080;
  EditorRate rate;
  std::vector<EditorAsset> assets;
  // Increasing order: bottom to top. No overlapping clips within a track.
  std::vector<EditorTrack> tracks;
  std::string main_track_id;
  bool magnetic = false;
};

EditorClip editor_evaluate(const EditorClip& clip, int64_t local_frame);
void editor_retime(EditorClip& clip, int64_t offset, double scale = 1);
int64_t editor_time_us(int64_t frame, EditorRate rate);
int64_t editor_duration(const EditorProject& project);
Result<void> editor_validate(const EditorProject& project);
Result<void> editor_save(const std::filesystem::path& path, const EditorProject& project);
// Missing media is retained, allowing the application to relink it.
Result<EditorProject> editor_load(const std::filesystem::path& path);

// UI-thread-owned transactional command history. Rejected edits do not modify it.
class EditorDocument {
 public:
  const EditorProject& project() const;
  Result<void> replace(EditorProject project);
  Result<void> edit(EditorProject project);
  Result<void> split(const std::string& clip_id, int64_t frame, std::string new_id);
  Result<void> move(const std::string& clip_id, const std::string& track_id, int64_t frame);
  Result<void> trim(const std::string& clip_id, int64_t source_in, int64_t duration);
  bool undo();
  bool redo();
  bool can_undo() const;
  bool can_redo() const;
  bool dirty() const;
  void mark_saved();

 private:
  EditorProject project_;
  std::vector<EditorProject> undo_, redo_;
  std::vector<uint64_t> undo_ids_, redo_ids_;
  uint64_t next_id_ = 0, current_id_ = 0, saved_id_ = 0;
};

struct EditorProbe {
  EditorAsset asset;
  EditorImage thumbnail;
  std::vector<float> waveform;  // Sampled peak envelope across the full asset.
};

struct EditorFrame {
  EditorImage image;
  std::vector<float> audio;  // Interleaved stereo float, 48 kHz, one project frame.
  int64_t frame = 0;
  uint64_t generation = 0;
};

struct EditorCallbacks {
  std::function<void(std::shared_ptr<const EditorFrame>)> frame;
  std::function<void(Failure)> error;
  std::function<void()> ended;
};

// Thread-safe controls; one worker owns the graph, bounded decoders and clock.
// Callbacks run on that worker; they must return promptly and never destroy the
// session or wait for its operations. Set callbacks before submitting work.
// Destruction cancels I/O and joins the worker, fencing all callbacks.
class EditorSession {
 public:
  explicit EditorSession(EditorCallbacks callbacks = {});
  ~EditorSession();
  EditorSession(const EditorSession&) = delete;
  EditorSession& operator=(const EditorSession&) = delete;
  AsyncResult<EditorProbe> probe(std::string path);
  // Pauses and renders the requested frame, clamped to [0, timeline duration].
  // The exclusive end is a black/silent frame, useful for append operations.
  AsyncResult<void> set_project(EditorProject project, uint64_t generation, int64_t frame = 0);
  // New seeks supersede queued seeks. Superseded operations complete cancelled.
  AsyncResult<void> seek(int64_t frame);
  AsyncResult<void> play();
  AsyncResult<void> pause();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct EditorExportProgress {
  int64_t completed_frames = 0, total_frames = 0;
};

// One export per instance. Uses an immutable snapshot and an independent worker.
// Progress runs on the export worker. Cancel is thread-safe; destruction joins.
class EditorExport {
 public:
  EditorExport();
  ~EditorExport();
  EditorExport(const EditorExport&) = delete;
  EditorExport& operator=(const EditorExport&) = delete;
  AsyncResult<void> start(EditorProject project, std::filesystem::path output,
                          std::function<void(EditorExportProgress)> progress = {});
  void cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace livekit::sdk
