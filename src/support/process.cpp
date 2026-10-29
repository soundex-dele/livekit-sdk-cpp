#include "support/process.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
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
std::wstring windows_quote(const std::wstring& arg) {
  std::wstring out = L"\""; std::size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') { ++slashes; continue; }
    out.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
    if (c == L'"') out += L'\\';
    out += c;
  }
  out.append(slashes * 2, L'\\'); out += L'"'; return out;
}
struct Process::Impl {
  std::mutex mutex, wait_mutex;
  int code = -1; bool waited = false;
#ifdef _WIN32
  HANDLE process = nullptr, pipe = nullptr;
#else
  pid_t process = -1; int pipe = -1;
#endif
};
#ifdef _WIN32
static std::wstring wide(const std::string& s) {
  auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (!count && !s.empty()) throw std::runtime_error("Invalid UTF-8 process argument");
  std::wstring result(count, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), count);
  return result;
}
#endif
Process::Process(const std::vector<std::string>& args) : impl_(std::make_unique<Impl>()) {
  if (args.empty()) throw std::runtime_error("Empty child process command");
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE writer = nullptr;
  if (!CreatePipe(&impl_->pipe, &writer, &sa, 0)) throw std::runtime_error("CreatePipe failed");
  SetHandleInformation(impl_->pipe, HANDLE_FLAG_INHERIT, 0);
  auto null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  HANDLE err = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &err, 0, TRUE, DUPLICATE_SAME_ACCESS);
  if (!err) err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = null_input; si.StartupInfo.hStdOutput = writer; si.StartupInfo.hStdError = err;
  SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> attributes(bytes);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes)) {
    CloseHandle(writer); CloseHandle(null_input); CloseHandle(err); CloseHandle(impl_->pipe);
    throw std::runtime_error("Process attributes initialization failed");
  }
  HANDLE inherited[]{writer, null_input, err};
  bool updated = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
  std::wstring command;
  for (const auto& arg : args) { if (!command.empty()) command += L' '; command += windows_quote(wide(arg)); }
  PROCESS_INFORMATION pi{};
  bool ok = updated && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
  auto error = GetLastError();
  DeleteProcThreadAttributeList(si.lpAttributeList);
  CloseHandle(writer); CloseHandle(null_input); CloseHandle(err);
  if (!ok) { CloseHandle(impl_->pipe); throw std::runtime_error("Cannot start " + args[0] + " (Windows error " + std::to_string(error) + ")"); }
  CloseHandle(pi.hThread); impl_->process = pi.hProcess;
#else
  int pipes[2];
#if defined(__linux__)
  if (::pipe2(pipes, O_CLOEXEC)) throw std::runtime_error("pipe2 failed");
#else
  if (::pipe(pipes)) throw std::runtime_error("pipe failed");
  fcntl(pipes[0], F_SETFD, FD_CLOEXEC); fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
#endif
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipes[0]);
  posix_spawn_file_actions_addclose(&actions, pipes[1]);
  std::vector<char*> argv; for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str())); argv.push_back(nullptr);
  auto error = posix_spawnp(&impl_->process, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions); ::close(pipes[1]);
  if (error) { ::close(pipes[0]); throw std::runtime_error("Cannot start " + args[0]); }
  impl_->pipe = pipes[0];
#endif
}
Process::~Process() {
  terminate(); wait();
#ifdef _WIN32
  CloseHandle(impl_->pipe); CloseHandle(impl_->process);
#else
  ::close(impl_->pipe);
#endif
}
std::size_t Process::read(void* dst, std::size_t size) {
#ifdef _WIN32
  DWORD count = 0;
  if (!ReadFile(impl_->pipe, dst, static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20)), &count, nullptr)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
    throw std::runtime_error("Child stdout read failed");
  }
  return count;
#else
  ssize_t n; do { n = ::read(impl_->pipe, dst, size); } while (n < 0 && errno == EINTR);
  if (n < 0) throw std::runtime_error("Child stdout read failed"); return static_cast<std::size_t>(n);
#endif
}
bool Process::read_exact(void* dst, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) { auto n = read(static_cast<char*>(dst) + offset, size - offset); if (!n) { if (offset) throw std::runtime_error("Truncated decoded media frame"); return false; } offset += n; }
  return true;
}
int Process::wait() {
  // Serialize reaping, but never keep terminate() out while the child is alive.
  std::lock_guard waiter(impl_->wait_mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool killed = false;
  for (;;) {
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->waited) return impl_->code;
#ifdef _WIN32
      const auto result = WaitForSingleObject(impl_->process, 0);
      if (result == WAIT_FAILED) throw std::runtime_error("Child process wait failed");
      if (result == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (!GetExitCodeProcess(impl_->process, &code)) throw std::runtime_error("Cannot read child exit code");
        impl_->code = static_cast<int>(code); impl_->waited = true; return impl_->code;
      }
      if (!killed && std::chrono::steady_clock::now() >= deadline) {
        if (!TerminateProcess(impl_->process, 1) && WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0)
          throw std::runtime_error("Cannot terminate timed-out child");
        killed = true;
      }
#else
      int status = 0;
      const auto result = waitpid(impl_->process, &status, WNOHANG);
      if (result == impl_->process) {
        impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        impl_->waited = true; return impl_->code;
      }
      if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
      if (!killed && std::chrono::steady_clock::now() >= deadline) {
        if (::kill(impl_->process, SIGKILL) && errno != ESRCH) throw std::runtime_error("Cannot terminate timed-out child");
        killed = true;
      }
#endif
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
void Process::terminate() {
  // wait() only holds this mutex while polling/reaping, so an EOF child can
  // still be stopped immediately instead of waiting for its normal exit.
  std::lock_guard lock(impl_->mutex);
  if (impl_->waited) return;
#ifdef _WIN32
  if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) TerminateProcess(impl_->process, 1);
#else
  ::kill(impl_->process, SIGKILL);
#endif
}
std::string Process::capture(const std::vector<std::string>& args) {
  Process p(args); std::string result; char buf[4096];
  for (;;) { auto n = p.read(buf, sizeof(buf)); if (!n) break; result.append(buf, n); if (result.size() > 8 * 1024 * 1024) throw std::runtime_error("Child output exceeds 8 MiB"); }
  if (p.wait()) throw std::runtime_error(args.front() + " failed"); return result;
}
}
