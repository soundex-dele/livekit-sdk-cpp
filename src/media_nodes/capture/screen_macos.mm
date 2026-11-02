#include "screen_macos.hpp"
#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"
#include "rtc_base/time_utils.h"
#include "screen_selection.hpp"
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using namespace livekit::sdk;

namespace {
struct CaptureState {
  std::mutex mutex;
  bool active = true, started = false;
  std::string error;
  std::function<Result<void>(const webrtc::VideoFrame&)> output;
  Failure failure;
  int width = 0, height = 0;
};

struct ContentResult {
  std::mutex mutex;
  bool done = false;
  SCShareableContent* content = nil;
  std::string error;
};

std::string errorText(NSError* e) {
  return e ? std::string(e.localizedDescription.UTF8String) : "Screen capture failed";
}
}  // namespace

@interface AVScreenCastOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
 @public
  std::shared_ptr<CaptureState> state;
}
@end
@implementation AVScreenCastOutput

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  std::lock_guard lock(state->mutex);
  if (state->active)
    state->error = errorText(error);
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample))
    return;
  auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0)
    return;
  NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
  NSNumber* status = info[SCStreamFrameInfoStatus];
  if (!status || status.integerValue != SCFrameStatusComplete)
    return;
  auto pixel = CMSampleBufferGetImageBuffer(sample);
  if (!pixel || CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_32BGRA)
    return;
  std::lock_guard lock(state->mutex);
  if (!state->active)
    return;
  auto delivered = [&]() -> Result<void> {
    if (CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                "Cannot lock screen frame"));

    struct Unlock {
      CVPixelBufferRef p;

      ~Unlock() {
        CVPixelBufferUnlockBaseAddress(p, kCVPixelBufferLock_ReadOnly);
      }
    } unlock{pixel};

    const int w = int(CVPixelBufferGetWidth(pixel)), h = int(CVPixelBufferGetHeight(pixel));
    auto* data = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixel));
    if (!data || w < 2 || h < 2 || w > 16384 || h > 16384)
      return Result<void>::failure(
          make_failure(ErrorDomain::media, ErrorCode::processing_failed, "Invalid screen frame"));
    auto native = webrtc::I420Buffer::Create(w, h);
    if (libyuv::ARGBToI420(data, int(CVPixelBufferGetBytesPerRow(pixel)), native->MutableDataY(),
                           native->StrideY(), native->MutableDataU(), native->StrideU(),
                           native->MutableDataV(), native->StrideV(), w, h))
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                                                "Screen conversion failed"));
    auto buffer = native;
    if (w != state->width || h != state->height) {
      buffer = webrtc::I420Buffer::Create(state->width, state->height);
      buffer->ScaleFrom(*native);
    }
    return state->output(webrtc::VideoFrame::Builder()
                             .set_video_frame_buffer(buffer)
                             .set_timestamp_us(webrtc::TimeMicros())
                             .build());
  }();
  if (!delivered) {
    state->failure = delivered.failure_record();
    state->error = delivered.error().message;
  }
}

@end

namespace lk {
Result<std::vector<livekit::sdk::ScreenCaptureDevice>> mac_screen_devices() {
  uint32_t count = 0;
  if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess)
    return Result<std::vector<ScreenCaptureDevice>>::failure(
        make_failure(ErrorDomain::media, ErrorCode::request_failed, "Cannot enumerate displays"));
  std::vector<CGDirectDisplayID> ids(count);
  if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
    return Result<std::vector<ScreenCaptureDevice>>::failure(
        make_failure(ErrorDomain::media, ErrorCode::request_failed, "Cannot enumerate displays"));
  std::vector<livekit::sdk::ScreenCaptureDevice> out;
  for (uint32_t i = 0; i < count; ++i) {
    auto id = ids[i];
    auto mode = CGDisplayCopyDisplayMode(id);
    if (!mode)
      continue;
    int w = int(CGDisplayModeGetPixelWidth(mode)), h = int(CGDisplayModeGetPixelHeight(mode));
    const int rotation = int(std::lround(CGDisplayRotation(id))) % 360;
    const int hz = int(std::lround(CGDisplayModeGetRefreshRate(mode)));
    CGDisplayModeRelease(mode);
    auto bounds = CGDisplayBounds(id);
    // Some display modes report unrotated pixel dimensions. Align them to
    // the current desktop orientation without applying rotation twice.
    if ((rotation == 90 || rotation == 270) &&
        ((w > h) != (bounds.size.width > bounds.size.height)))
      std::swap(w, h);
    out.push_back({std::to_string(id),
                   std::string(CGDisplayIsBuiltin(id) ? "内建显示器" : "外接显示器") + " " +
                       std::to_string(i + 1),
                   int(std::lround(bounds.origin.x)), int(std::lround(bounds.origin.y)), w, h,
                   id == CGMainDisplayID(), rotation, hz});
  }
  return Result<std::vector<ScreenCaptureDevice>>::success(std::move(out));
}

Result<void> capture_screen_macos(
    const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
    const std::function<Result<void>(const webrtc::VideoFrame&)>& output) {
  @autoreleasepool {
    if (stop)
      return Result<void>::success();
    if (!CGPreflightScreenCaptureAccess())
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                                "Screen recording permission denied"));
    if (spec.width < 2 || spec.height < 2 || spec.fps < 1)
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                                "Invalid screen capture settings"));
    auto devices = mac_screen_devices();
    if (!devices) {
      return Result<void>::failure(devices.failure_record());
    }
    auto selected = select_screen(*devices, spec.screen_id);
    if (!selected) {
      return Result<void>::failure(selected.failure_record());
    }
    auto result = std::make_shared<ContentResult>();
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                   std::lock_guard lock(result->mutex);
                                   result->content = content;
                                   result->error = error ? errorText(error) : "";
                                   result->done = true;
                                 }];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    SCShareableContent* content = nil;
    while (!stop) {
      {
        std::lock_guard lock(result->mutex);
        if (result->done) {
          if (!result->error.empty())
            return Result<void>::failure(
                make_failure(ErrorDomain::media, ErrorCode::request_failed, result->error));
          content = result->content;
          break;
        }
      }
      if (std::chrono::steady_clock::now() > deadline)
        return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                                  "Screen content request timed out"));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop)
      return Result<void>::success();
    SCDisplay* display = nil;
    for (SCDisplay* d in content.displays)
      if (std::to_string(d.displayID) == selected->id) {
        display = d;
        break;
      }
    if (!display)
      return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                                "Selected display unavailable"));
    auto shared = std::make_shared<CaptureState>();
    shared->output = output;
    shared->width = spec.width;
    shared->height = spec.height;
    AVScreenCastOutput* delegate = [AVScreenCastOutput new];
    delegate->state = shared;
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];
    SCStreamConfiguration* config = [SCStreamConfiguration new];
    config.width = spec.width;
    config.height = spec.height;
    config.minimumFrameInterval = CMTimeMake(1, spec.fps);
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = YES;
    config.capturesAudio = NO;
    config.queueDepth = 3;
    config.scalesToFit = YES;
    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:config
                                               delegate:delegate];
    dispatch_queue_t queue =
        dispatch_queue_create("org.avplatform.screen-cast.frames", DISPATCH_QUEUE_SERIAL);
    NSError* error = nil;
    if (![stream addStreamOutput:delegate
                            type:SCStreamOutputTypeScreen
              sampleHandlerQueue:queue
                           error:&error])
      return Result<void>::failure(
          make_failure(ErrorDomain::media, ErrorCode::request_failed, errorText(error)));
    [stream startCaptureWithCompletionHandler:^(NSError* failure) {
      bool abandoned;
      {
        std::lock_guard lock(shared->mutex);
        abandoned = !shared->active;
        shared->started = true;
        if (failure && shared->active)
          shared->error = errorText(failure);
      }
      // A late start completion must never resurrect capture after
      // cancellation.
      if (abandoned)
        [stream stopCaptureWithCompletionHandler:^(NSError*){
        }];
    }];
    Failure failure;
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stop) {
      {
        std::lock_guard lock(shared->mutex);
        if (!shared->error.empty()) {
          failure = shared->failure ? shared->failure
                                    : make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                                   shared->error);
          break;
        }
        if (!shared->started && std::chrono::steady_clock::now() > startDeadline) {
          failure = make_failure(ErrorDomain::media, ErrorCode::timeout,
                                 "Screen capture start timed out");
          break;
        }
      }
      auto current = mac_screen_devices();
      if (!current) {
        failure = current.failure_record();
        break;
      }
      if (auto valid = validate_screen(*selected, *current); !valid) {
        failure = valid.failure_record();
        break;
      }
      for (int i = 0; i < 10 && !stop; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    {
      std::lock_guard lock(shared->mutex);
      shared->active = false;
      shared->output = {};
    }
    auto stopped = dispatch_semaphore_create(0);
    [stream stopCaptureWithCompletionHandler:^(NSError*) {
      dispatch_semaphore_signal(stopped);
    }];
    // Callback gate is closed before waiting, so late system completions own no
    // SDK resources.
    dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    [stream removeStreamOutput:delegate type:SCStreamOutputTypeScreen error:nil];
    dispatch_sync(queue, ^{
                  });
    if (failure) {
      return Result<void>::failure(failure);
    }
    return Result<void>::success();
  }
}
}  // namespace lk
