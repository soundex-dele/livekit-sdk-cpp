#include "backends/ffmpeg/windows_file_read.hpp"
#include "engines/player/playback_graph.hpp"
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>

using namespace livekit::sdk;
using namespace livekit::sdk::detail;
using namespace std::chrono_literals;

namespace {
struct Handle {
  HANDLE h;

  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
};

#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      return false;                          \
    }                                        \
  } while (false)

bool pending_reads() {
  const auto path = L"\\\\.\\pipe\\livekit-player-read-" + std::to_wstring(GetCurrentProcessId());
  Handle server{CreateNamedPipeW(
      path.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256, 256, 0,
      nullptr)};
  CHECK(server.h != INVALID_HANDLE_VALUE);
  Handle client{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  CHECK(client.h != INVALID_HANDLE_VALUE);
  OVERLAPPED connect{};
  CHECK(ConnectNamedPipe(server.h, &connect) || GetLastError() == ERROR_PIPE_CONNECTED);
  Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  CHECK(event.h);
  for (bool deadline : {false, true}) {
    unsigned char byte = 0;
    std::atomic<bool> cancel{false};
    std::promise<void> pending;
    auto began = pending.get_future();
    auto worker = std::async(std::launch::async, [&] {
      bool first = true;
      auto until = std::chrono::steady_clock::now() + 100ms;
      return playback_read_at(server.h, event.h, 0, &byte, 1, [&] {
        if (first) {
          first = false;
          pending.set_value();
        }
        return cancel.load() || (deadline && std::chrono::steady_clock::now() >= until);
      });
    });
    bool entered = began.wait_for(2s) == std::future_status::ready;
    if (!deadline || !entered)
      cancel = true;
    CHECK(entered && worker.wait_for(2s) == std::future_status::ready);
    auto r = worker.get();
    CHECK(r.interrupted && r.error == ERROR_OPERATION_ABORTED && byte == 0);
    // Reuse the same handle/event and buffer. A stale read must not consume this byte.
    const unsigned char sent = 0xa5;
    DWORD wrote = 0;
    CHECK(WriteFile(client.h, &sent, 1, &wrote, nullptr) && wrote == 1);
    auto read = playback_read_at(server.h, event.h, 0, &byte, 1, [] { return false; });
    CHECK(!read.interrupted && read.error == ERROR_SUCCESS && read.bytes == 1 && byte == sent);
  }
  return true;
}

bool locked_media(const std::filesystem::path& original) {
  // Do not byte-lock the shared CTest fixture: other Player tests may run in parallel.
  auto path =
      original.parent_path() / ("read-lock-" + std::to_string(GetCurrentProcessId()) + ".mp4");
  std::error_code ec;
  CHECK(std::filesystem::copy_file(original, path, std::filesystem::copy_options::none, ec));

  struct Remove {
    std::filesystem::path path;

    ~Remove() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } remove{path};

  std::atomic<bool> cancel{false};
  PlaybackGraph rolledBack(cancel, {});
  {
    Handle lock{CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
    CHECK(lock.h != INVALID_HANDLE_VALUE);
    OVERLAPPED range{};
    CHECK(LockFileEx(lock.h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 4096, 0,
                     &range));
    auto opened = rolledBack.open(path.u8string());
    CHECK(!opened && opened.error().context.node == "file.demuxer" &&
          opened.error().context.operation == "read" &&
          opened.error().backend_reason == ERROR_LOCK_VIOLATION);
    for (const auto& node : rolledBack.nodes())
      CHECK(node.state == MediaState::Released);
    CHECK(UnlockFileEx(lock.h, 0, 4096, 0, &range));
  }
  {
    PlaybackGraph graph(cancel, {});
    auto info = graph.open(path.u8string());
    CHECK(info && info->has_audio && info->has_video && graph.start() && graph.seek(500000) &&
          graph.step());
  }
  // Exclusive reopen after release catches either the demux handle or a fixture handle still open.
  Handle exclusive{CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  CHECK(exclusive.h != INVALID_HANDLE_VALUE);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  return argc == 2 && pending_reads() && locked_media(std::filesystem::path(argv[1]) / "av.mp4")
             ? 0
             : 1;
}
