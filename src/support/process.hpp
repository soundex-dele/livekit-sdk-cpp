#pragma once
#include "livekit/result.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace lk {
// Owns exactly one child and its stdout pipe. Arguments never pass through a shell.
class Process {
 public:
  explicit Process(const std::vector<std::string>& args);
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  livekit::sdk::Result<void> status() const;
  livekit::sdk::Result<std::size_t> read(void* dst, std::size_t size);
  livekit::sdk::Result<bool> read_exact(void* dst, std::size_t size);
  livekit::sdk::Result<int> wait();
  void terminate();
  static livekit::sdk::Result<std::string> capture(const std::vector<std::string>& args);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::wstring windows_quote(const std::wstring& arg);
}  // namespace lk
