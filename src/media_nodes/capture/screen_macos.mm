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

namespace {
struct CaptureState {
  std::mutex mutex;
  bool active = true, started = false;
  std::string error;
  std::function<void(const webrtc::VideoFrame &)> output;
  int width = 0, height = 0;
};
struct ContentResult {
  std::mutex mutex;
  bool done = false;
  SCShareableContent *content = nil;
  std::string error;
};
std::string errorText(NSError *e) {
  return e ? std::string(e.localizedDescription.UTF8String)
           : "Screen capture failed";
}
} // namespace
@interface AVScreenCastOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  std::shared_ptr<CaptureState> state;
}
@end
@implementation AVScreenCastOutput
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  std::lock_guard lock(state->mutex);
  if (state->active)
    state->error = errorText(error);
}
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample))
    return;
  auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0)
    return;
  NSDictionary *info =
      (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
  NSNumber *status = info[SCStreamFrameInfoStatus];
  if (!status || status.integerValue != SCFrameStatusComplete)
    return;
  auto pixel = CMSampleBufferGetImageBuffer(sample);
  if (!pixel ||
      CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_32BGRA)
    return;
  std::lock_guard lock(state->mutex);
  if (!state->active)
    return;
  try {
    if (CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) !=
        kCVReturnSuccess)
      throw std::runtime_error("Cannot lock screen frame");
    struct Unlock {
      CVPixelBufferRef p;
      ~Unlock() {
        CVPixelBufferUnlockBaseAddress(p, kCVPixelBufferLock_ReadOnly);
      }
    } unlock{pixel};
    const int w = int(CVPixelBufferGetWidth(pixel)),
              h = int(CVPixelBufferGetHeight(pixel));
    auto *data = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pixel));
    if (!data || w < 2 || h < 2 || w > 16384 || h > 16384)
      throw std::runtime_error("Invalid screen frame");
    auto native = webrtc::I420Buffer::Create(w, h);
    if (libyuv::ARGBToI420(data, int(CVPixelBufferGetBytesPerRow(pixel)),
                           native->MutableDataY(), native->StrideY(),
                           native->MutableDataU(), native->StrideU(),
                           native->MutableDataV(), native->StrideV(), w, h))
      throw std::runtime_error("Screen conversion failed");
    auto buffer = native;
    if (w != state->width || h != state->height) {
      buffer = webrtc::I420Buffer::Create(state->width, state->height);
      buffer->ScaleFrom(*native);
    }
    state->output(webrtc::VideoFrame::Builder()
                      .set_video_frame_buffer(buffer)
                      .set_timestamp_us(webrtc::TimeMicros())
                      .build());
  } catch (const std::exception &e) {
    state->error = e.what();
  }
}
@end

namespace lk {
std::vector<livekit::sdk::ScreenCaptureDevice> mac_screen_devices() {
  uint32_t count = 0;
  if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess)
    throw std::runtime_error("Cannot enumerate displays");
  std::vector<CGDirectDisplayID> ids(count);
  if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
    throw std::runtime_error("Cannot enumerate displays");
  std::vector<livekit::sdk::ScreenCaptureDevice> out;
  for (uint32_t i = 0; i < count; ++i) {
    auto id = ids[i];
    auto mode = CGDisplayCopyDisplayMode(id);
    if (!mode)
      continue;
    int w = int(CGDisplayModeGetPixelWidth(mode)),
        h = int(CGDisplayModeGetPixelHeight(mode));
    const int rotation = int(std::lround(CGDisplayRotation(id))) % 360;
    const int hz = int(std::lround(CGDisplayModeGetRefreshRate(mode)));
    CGDisplayModeRelease(mode);
    auto bounds = CGDisplayBounds(id);
    // Some display modes report unrotated pixel dimensions. Align them to
    // the current desktop orientation without applying rotation twice.
    if ((rotation == 90 || rotation == 270) &&
        ((w > h) != (bounds.size.width > bounds.size.height)))
      std::swap(w, h);
    out.push_back(
        {std::to_string(id),
         std::string(CGDisplayIsBuiltin(id) ? "内建显示器" : "外接显示器") +
             " " + std::to_string(i + 1),
         int(std::lround(bounds.origin.x)), int(std::lround(bounds.origin.y)),
         w, h, id == CGMainDisplayID(), rotation, hz});
  }
  return out;
}
void capture_screen_macos(
    const livekit::sdk::MediaSpec &spec, std::atomic<bool> &stop,
    const std::function<void(const webrtc::VideoFrame &)> &output) {
  @autoreleasepool {
    if (stop)
      return;
    if (!CGPreflightScreenCaptureAccess())
      throw std::runtime_error("Screen recording permission denied");
    if (spec.width < 2 || spec.height < 2 || spec.fps < 1)
      throw std::runtime_error("Invalid screen capture settings");
    auto selected = select_screen(mac_screen_devices(), spec.screen_id);
    auto result = std::make_shared<ContentResult>();
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(
                                     SCShareableContent *content,
                                     NSError *error) {
                                   std::lock_guard lock(result->mutex);
                                   result->content = content;
                                   result->error =
                                       error ? errorText(error) : "";
                                   result->done = true;
                                 }];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    SCShareableContent *content = nil;
    while (!stop) {
      {
        std::lock_guard lock(result->mutex);
        if (result->done) {
          if (!result->error.empty())
            throw std::runtime_error(result->error);
          content = result->content;
          break;
        }
      }
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("Screen content request timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop)
      return;
    SCDisplay *display = nil;
    for (SCDisplay *d in content.displays)
      if (std::to_string(d.displayID) == selected.id) {
        display = d;
        break;
      }
    if (!display)
      throw std::runtime_error("Selected display unavailable");
    auto shared = std::make_shared<CaptureState>();
    shared->output = output;
    shared->width = spec.width;
    shared->height = spec.height;
    AVScreenCastOutput *delegate = [AVScreenCastOutput new];
    delegate->state = shared;
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];
    SCStreamConfiguration *config = [SCStreamConfiguration new];
    config.width = spec.width;
    config.height = spec.height;
    config.minimumFrameInterval = CMTimeMake(1, spec.fps);
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = YES;
    config.capturesAudio = NO;
    config.queueDepth = 3;
    config.scalesToFit = YES;
    SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:config
                                               delegate:delegate];
    dispatch_queue_t queue = dispatch_queue_create(
        "org.avplatform.screen-cast.frames", DISPATCH_QUEUE_SERIAL);
    NSError *error = nil;
    if (![stream addStreamOutput:delegate
                            type:SCStreamOutputTypeScreen
              sampleHandlerQueue:queue
                           error:&error])
      throw std::runtime_error(errorText(error));
    [stream startCaptureWithCompletionHandler:^(NSError *failure) {
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
        [stream stopCaptureWithCompletionHandler:^(NSError *){
        }];
    }];
    std::string failure;
    const auto startDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    try {
      while (!stop) {
        {
          std::lock_guard lock(shared->mutex);
          if (!shared->error.empty())
            throw std::runtime_error(shared->error);
          if (!shared->started &&
              std::chrono::steady_clock::now() > startDeadline)
            throw std::runtime_error("Screen capture start timed out");
        }
        validate_screen(selected, mac_screen_devices());
        for (int i = 0; i < 10 && !stop; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    } catch (const std::exception &e) {
      failure = e.what();
    }
    {
      std::lock_guard lock(shared->mutex);
      shared->active = false;
      shared->output = {};
    }
    auto stopped = dispatch_semaphore_create(0);
    [stream stopCaptureWithCompletionHandler:^(NSError *) {
      dispatch_semaphore_signal(stopped);
    }];
    // Callback gate is closed before waiting, so late system completions own no
    // SDK resources.
    dispatch_semaphore_wait(stopped,
                            dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    [stream removeStreamOutput:delegate
                          type:SCStreamOutputTypeScreen
                         error:nil];
    dispatch_sync(queue, ^{
                  });
    if (!failure.empty())
      throw std::runtime_error(failure);
  }
}
} // namespace lk
