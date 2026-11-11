#pragma once
#include "livekit/editor.hpp"
#include <atomic>

namespace livekit::sdk::editor_detail {
inline Failure failure(std::string text, ErrorCode code = ErrorCode::processing_failed) {
  return make_failure(ErrorDomain::media, code, "freecut/" + text);
}

Result<void> atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);
Result<EditorProbe> probe(const std::string& path, std::atomic<bool>& cancel);

class RenderGraph {
 public:
  RenderGraph(const EditorProject& project, std::atomic<bool>& cancel);
  ~RenderGraph();
  Result<std::shared_ptr<EditorFrame>> render(int64_t frame, uint64_t generation);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

Result<void> export_mp4(const EditorProject&, const std::filesystem::path&, std::atomic<bool>&,
                        const std::function<void(EditorExportProgress)>&);
}  // namespace livekit::sdk::editor_detail
