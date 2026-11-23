#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace livekit::sdk::detail {
// Private to the FFmpeg custom AVIO adapter. The caller owns a manual-reset
// event and serializes reads. The file must be opened FILE_FLAG_OVERLAPPED.
struct PlaybackReadResult {
  DWORD bytes = 0, error = ERROR_SUCCESS;
  bool interrupted = false;
};

template <class Interrupted>
PlaybackReadResult playback_read_at(HANDLE file, HANDLE event, uint64_t offset, void* buffer,
                                    DWORD size, Interrupted interrupted) {
  OVERLAPPED io{};
  io.Offset = static_cast<DWORD>(offset);
  io.OffsetHigh = static_cast<DWORD>(offset >> 32);
  io.hEvent = event;
  DWORD count = 0;
  if (!ReadFile(file, buffer, size, &count, &io)) {
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING)
      return {0, error};
    for (;;) {
      const bool cancel = interrupted();
      const DWORD wait = cancel ? WAIT_TIMEOUT : WaitForSingleObject(event, 10);
      if (wait == WAIT_OBJECT_0)
        break;
      if (!cancel && wait == WAIT_TIMEOUT)
        continue;
      const DWORD waitError = cancel ? ERROR_SUCCESS : GetLastError();
      // Cancellation can race normal completion (ERROR_NOT_FOUND). In either
      // case the kernel must be finished with io and buffer before we return.
      CancelIoEx(file, &io);
      const DWORD completion =
          GetOverlappedResult(file, &io, &count, TRUE) ? ERROR_SUCCESS : GetLastError();
      return {count, cancel ? completion : waitError, cancel};
    }
  }
  if (!GetOverlappedResult(file, &io, &count, TRUE))
    return {0, GetLastError()};
  return {count, ERROR_SUCCESS};
}
}  // namespace livekit::sdk::detail
#endif
