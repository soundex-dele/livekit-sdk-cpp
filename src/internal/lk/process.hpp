#pragma once
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
  std::size_t read(void* dst, std::size_t size);
  bool read_exact(void* dst, std::size_t size);
  int wait();
  void terminate();
  static std::string capture(const std::vector<std::string>& args);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::wstring windows_quote(const std::wstring& arg);
}
