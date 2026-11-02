#pragma once

namespace livekit::sdk::test {

// GCC, Clang and MSVC provide these call-site intrinsics in C++17 mode.
// Keep test diagnostics without requiring C++20 std::source_location.
class SourceLocation {
 public:
  static constexpr SourceLocation current(const char* file = __builtin_FILE(),
                                          unsigned line = __builtin_LINE()) noexcept {
    return SourceLocation(file, line);
  }

  constexpr const char* file_name() const noexcept {
    return file_;
  }

  constexpr unsigned line() const noexcept {
    return line_;
  }

 private:
  constexpr SourceLocation(const char* file, unsigned line) noexcept : file_(file), line_(line) {}

  const char* file_;
  unsigned line_;
};

}  // namespace livekit::sdk::test
