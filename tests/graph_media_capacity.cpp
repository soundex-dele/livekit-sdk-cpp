#include "graph_media_profile_round.hpp"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Each worker reaches exactly two measured phases in order. A worker that fails
// before either rendezvous must release its peers; failure is sticky for late arrivals.
class RoundGate {
 public:
  explicit RoundGate(int sessions) : sessions_(sessions) {}

  bool arrive() {
    std::unique_lock lock(mutex_);
    if (failed_) {
      return false;
    }
    const int phase = phase_;
    if (++waiting_ == sessions_) {
      waiting_ = 0;
      ++phase_;
      changed_.notify_all();
    } else {
      changed_.wait(lock, [&] { return failed_ || phase_ != phase; });
    }
    return !failed_;
  }

  void cancel() {
    std::lock_guard lock(mutex_);
    failed_ = true;
    changed_.notify_all();
  }

 private:
  const int sessions_;
  int phase_ = 0, waiting_ = 0;
  bool failed_ = false;
  std::mutex mutex_;
  std::condition_variable changed_;
};

// Never run a sampler thread during a "closed" sample. Otherwise the sampling
// thread/handles themselves become part of the resource baseline.
bool sample(std::ofstream& log, Clock::time_point origin, int round, const char* phase) {
  HANDLE self = GetCurrentProcess();
  PROCESS_MEMORY_COUNTERS_EX memory{};
  DWORD handles = 0, threads = 0;
  if (!GetProcessHandleCount(self, &handles) ||
      !GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                            sizeof(memory)))
    return false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return false;
  THREADENTRY32 entry{sizeof(entry)};
  if (Thread32First(snapshot, &entry)) {
    do {
      threads += entry.th32OwnerProcessID == GetCurrentProcessId();
      entry.dwSize = sizeof(entry);
    } while (Thread32Next(snapshot, &entry));
  }
  auto error = GetLastError();
  CloseHandle(snapshot);
  if (!threads || error != ERROR_NO_MORE_FILES)
    return false;
  nlohmann::json row = {
      {"elapsedMs",
       std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count()},
      {"round", round},
      {"phase", phase},
      {"privateBytes", memory.PrivateUsage},
      {"threads", threads},
      {"handles", handles},
      {"pid", GetCurrentProcessId()}};
  log << row.dump() << std::endl;
  return bool(log);
}
}  // namespace

int mediaCapacity(const std::filesystem::path& root, int frames, int sessions, int rounds) {
  if (sessions < 1 || sessions > 4 || rounds < 3 || rounds > 12 || frames < 25 || frames > 1500)
    return 2;
  std::ofstream resources(root / "resources.jsonl");
  auto origin = Clock::now();
  if (!sample(resources, origin, -1, "initial"))
    return 1;
  for (int round = 0; round < rounds; ++round) {
    std::vector<std::filesystem::path> dirs;
    for (int i = 0; i < sessions; ++i) {
      std::error_code ec;
      dirs.push_back(root / ("round-" + std::to_string(round) + "-session-" + std::to_string(i)));
      if (!std::filesystem::create_directory(dirs.back(), ec))
        return 1;
    }
    std::vector<int> results(sessions, -1);
    bool countersOk = true;
    {
      RoundGate phases(sessions);
      std::mutex mutex;
      std::condition_variable done;
      int remaining = sessions;
      std::vector<std::thread> workers;
      for (int i = 0; i < sessions; ++i) {
        workers.emplace_back([&, i] {
          results[i] = mediaProfile(dirs[i], frames, [&] { return phases.arrive(); });
          if (results[i] != 0) {
            phases.cancel();
          }
          {
            std::lock_guard lock(mutex);
            --remaining;
          }
          done.notify_one();
        });
      }
      for (;;) {
        countersOk &= sample(resources, origin, round, "active");
        std::unique_lock lock(mutex);
        if (done.wait_for(lock, 200ms, [&] { return remaining == 0; }))
          break;
      }
      for (auto& worker : workers)
        worker.join();
    }
    if (!sample(resources, origin, round, "closed") || !countersOk ||
        std::any_of(results.begin(), results.end(), [](int rc) { return rc != 0; })) {
      std::cerr << "incomplete capacity round " << round << '\n';
      return 1;
    }
  }
  return 0;
}
