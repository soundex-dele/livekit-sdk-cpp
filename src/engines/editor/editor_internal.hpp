#pragma once
#include "livekit/editor.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace livekit::sdk::editor_detail {
inline Failure failure(std::string text, ErrorCode code = ErrorCode::processing_failed) {
  return make_failure(ErrorDomain::media, code, "freecut/" + text);
}

Result<void> atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);
Result<EditorProbe> probe(const std::string& path, std::atomic<bool>& cancel);

struct RenderGraphStats {
  size_t decoder_opens = 0;
  size_t cached_decoders = 0;
  size_t topology_reloads = 0;
  size_t reused_effect_nodes = 0;

  struct Node {
    std::string id;
    uintptr_t instance = 0;
    std::vector<std::string> outputs;
  };

  std::vector<Node> nodes;
};

class RenderGraph {
 public:
  RenderGraph(const EditorProject& project, std::atomic<bool>& cancel);
  ~RenderGraph();
  // Owner worker only, between synchronous renders. Retains stages/unchanged algorithm nodes
  // and unchanged asset decoders; invalidates project-derived frame caches.
  Result<void> update_project(EditorProject project);
  RenderGraphStats stats() const;
  Result<std::shared_ptr<EditorFrame>> render(int64_t frame, uint64_t generation);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

Result<void> export_mp4(const EditorProject&, const std::filesystem::path&, std::atomic<bool>&,
                        const std::function<void(EditorExportProgress)>&);
}  // namespace livekit::sdk::editor_detail
