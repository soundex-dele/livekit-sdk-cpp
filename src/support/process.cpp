#include "support/process.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <limits>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace lk {
using livekit::sdk::ErrorCode;
using livekit::sdk::ErrorDomain;
using livekit::sdk::Failure;
using livekit::sdk::make_failure;
using livekit::sdk::Result;

namespace {
Failure process_failure(std::string message, ErrorCode code = ErrorCode::processing_failed,
                        int backend_reason = 0) {
  return make_failure(ErrorDomain::media, code, std::move(message), false, backend_reason,
                      {"process"});
}
}  // namespace
std::wstring windows_quote(const std::wstring& arg) {
  std::wstring out = L"\"";
  std::size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (c == L'"')
      out += L'\\';
    out += c;
  }
  out.append(slashes * 2, L'\\');
  out += L'"';
  return out;
}

struct Process::Impl {
  std::mutex mutex, wait_mutex;
  int code = -1;
  bool waited = false;
  Failure failure;
#ifdef _WIN32
  HANDLE process = nullptr, pipe = nullptr;
#else
  pid_t process = -1;
  int pipe = -1;
#endif
};
#ifdef _WIN32
static Result<std::wstring> wide(const std::string& s) {
  if (s.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return Result<std::wstring>::failure(
        process_failure("Process argument too large", ErrorCode::invalid_argument));
  }
  auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
  if (!count && !s.empty())
    return Result<std::wstring>::failure(
        process_failure("Invalid UTF-8 process argument", ErrorCode::invalid_argument));
  std::wstring result(count, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                      result.data(), count);
  return Result<std::wstring>::success(std::move(result));
}
#endif
Process::Process(const std::vector<std::string>& args) : impl_(std::make_unique<Impl>()) {
  auto started = [&]() -> Result<void> {
    if (args.empty() || args.front().empty()) {
      return Result<void>::failure(
          process_failure("Empty child process command", ErrorCode::invalid_argument));
    }
    for (const auto& arg : args) {
      if (arg.find('\0') != std::string::npos) {
        return Result<void>::failure(
            process_failure("Embedded NUL in process argument", ErrorCode::invalid_argument));
      }
    }
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : args) {
      auto converted = wide(arg);
      if (!converted) {
        return Result<void>::failure(converted.failure_record());
      }
      if (!command.empty()) {
        command += L' ';
      }
      command += windows_quote(*converted);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE writer = nullptr;
    if (!CreatePipe(&impl_->pipe, &writer, &sa, 0))
      return Result<void>::failure(
          process_failure("CreatePipe failed", ErrorCode::processing_failed, int(GetLastError())));
    SetHandleInformation(impl_->pipe, HANDLE_FLAG_INHERIT, 0);
    auto null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr);
    HANDLE err = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &err,
                    0, TRUE, DUPLICATE_SAME_ACCESS);
    if (!err)
      err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, 0, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null_input;
    si.StartupInfo.hStdOutput = writer;
    si.StartupInfo.hStdError = err;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes)) {
      const auto error = GetLastError();
      CloseHandle(writer);
      CloseHandle(null_input);
      CloseHandle(err);
      CloseHandle(impl_->pipe);
      impl_->pipe = nullptr;
      return Result<void>::failure(process_failure("Process attributes initialization failed",
                                                   ErrorCode::processing_failed, int(error)));
    }
    HANDLE inherited[]{writer, null_input, err};
    bool updated =
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    bool ok = updated && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &si.StartupInfo, &pi);
    auto error = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(writer);
    CloseHandle(null_input);
    CloseHandle(err);
    if (!ok) {
      CloseHandle(impl_->pipe);
      impl_->pipe = nullptr;
      return Result<void>::failure(process_failure(
          "Cannot start " + args[0] + " (Windows error " + std::to_string(error) + ")",
          ErrorCode::processing_failed, int(error)));
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
#else
    int pipes[2];
#if defined(__linux__)
    if (::pipe2(pipes, O_CLOEXEC))
      return Result<void>::failure(
          process_failure("pipe2 failed", ErrorCode::processing_failed, errno));
#else
    if (::pipe(pipes))
      return Result<void>::failure(
          process_failure("pipe failed", ErrorCode::processing_failed, errno));
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    std::vector<char*> argv;
    for (const auto& arg : args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    auto error = posix_spawnp(&impl_->process, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipes[1]);
    if (error) {
      ::close(pipes[0]);
      return Result<void>::failure(
          process_failure("Cannot start " + args[0], ErrorCode::processing_failed, error));
    }
    impl_->pipe = pipes[0];
#endif
    return Result<void>::success();
  }();
  if (!started) {
    impl_->failure = started.failure_record();
  }
}

Result<void> Process::status() const {
  return impl_->failure ? Result<void>::failure(impl_->failure) : Result<void>::success();
}

Process::~Process() {
  terminate();
  (void)wait();
#ifdef _WIN32
  CloseHandle(impl_->pipe);
  CloseHandle(impl_->process);
#else
  ::close(impl_->pipe);
#endif
}

Result<std::size_t> Process::read(void* dst, std::size_t size) {
  if (impl_->failure) {
    return Result<std::size_t>::failure(impl_->failure);
  }
  if (size && !dst) {
    return Result<std::size_t>::failure(
        process_failure("Null child read buffer", ErrorCode::invalid_argument));
  }
#ifdef _WIN32
  DWORD count = 0;
  if (!ReadFile(impl_->pipe, dst, static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20)), &count,
                nullptr)) {
    if (GetLastError() == ERROR_BROKEN_PIPE)
      return Result<std::size_t>::success(0);
    return Result<std::size_t>::failure(process_failure("Child stdout read failed"));
  }
  return Result<std::size_t>::success(count);
#else
  ssize_t n;
  do {
    n = ::read(impl_->pipe, dst, size);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return Result<std::size_t>::failure(process_failure("Child stdout read failed"));
  return Result<std::size_t>::success(static_cast<std::size_t>(n));
#endif
}

Result<bool> Process::read_exact(void* dst, std::size_t size) {
  if (impl_->failure) {
    return Result<bool>::failure(impl_->failure);
  }
  if (size && !dst) {
    return Result<bool>::failure(
        process_failure("Null child read buffer", ErrorCode::invalid_argument));
  }
  std::size_t offset = 0;
  while (offset < size) {
    auto count = read(static_cast<char*>(dst) + offset, size - offset);
    if (!count) {
      return Result<bool>::failure(count.failure_record());
    }
    if (!*count) {
      if (offset) {
        return Result<bool>::failure(process_failure("Truncated decoded media frame"));
      }
      return Result<bool>::success(false);
    }
    offset += *count;
  }
  return Result<bool>::success(true);
}

Result<int> Process::wait() {
  if (impl_->failure) {
    return Result<int>::failure(impl_->failure);
  }
  // Serialize reaping, but never keep terminate() out while the child is alive.
  std::lock_guard waiter(impl_->wait_mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool killed = false;
  for (;;) {
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->waited)
        return Result<int>::success(impl_->code);
#ifdef _WIN32
      const auto result = WaitForSingleObject(impl_->process, 0);
      if (result == WAIT_FAILED)
        return Result<int>::failure(process_failure("Child process wait failed"));
      if (result == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (!GetExitCodeProcess(impl_->process, &code))
          return Result<int>::failure(process_failure("Cannot read child exit code"));
        impl_->code = static_cast<int>(code);
        impl_->waited = true;
        return Result<int>::success(impl_->code);
      }
      if (!killed && std::chrono::steady_clock::now() >= deadline) {
        if (!TerminateProcess(impl_->process, 1) &&
            WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0)
          return Result<int>::failure(process_failure("Cannot terminate timed-out child"));
        killed = true;
      }
#else
      int status = 0;
      const auto result = waitpid(impl_->process, &status, WNOHANG);
      if (result == impl_->process) {
        impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        impl_->waited = true;
        return Result<int>::success(impl_->code);
      }
      if (result < 0 && errno != EINTR)
        return Result<int>::failure(process_failure("waitpid failed"));
      if (!killed && std::chrono::steady_clock::now() >= deadline) {
        if (::kill(impl_->process, SIGKILL) && errno != ESRCH)
          return Result<int>::failure(process_failure("Cannot terminate timed-out child"));
        killed = true;
      }
#endif
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void Process::terminate() {
  if (impl_->failure) {
    return;
  }
  // wait() only holds this mutex while polling/reaping, so an EOF child can
  // still be stopped immediately instead of waiting for its normal exit.
  std::lock_guard lock(impl_->mutex);
  if (impl_->waited)
    return;
#ifdef _WIN32
  if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT)
    TerminateProcess(impl_->process, 1);
#else
  ::kill(impl_->process, SIGKILL);
#endif
}

Result<std::string> Process::capture(const std::vector<std::string>& args) {
  Process process(args);
  if (auto ready = process.status(); !ready) {
    return Result<std::string>::failure(ready.failure_record());
  }
  std::string result;
  char buf[4096];
  for (;;) {
    auto count = process.read(buf, sizeof(buf));
    if (!count) {
      return Result<std::string>::failure(count.failure_record());
    }
    if (!*count) {
      break;
    }
    result.append(buf, *count);
    if (result.size() > 8 * 1024 * 1024) {
      return Result<std::string>::failure(
          process_failure("Child output exceeds 8 MiB", ErrorCode::resource_exhausted));
    }
  }
  auto exited = process.wait();
  if (!exited) {
    return Result<std::string>::failure(exited.failure_record());
  }
  if (*exited) {
    return Result<std::string>::failure(
        process_failure(args.front() + " failed", ErrorCode::processing_failed, *exited));
  }
  return Result<std::string>::success(std::move(result));
}
}  // namespace lk
