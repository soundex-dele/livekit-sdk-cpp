#include "audio_samples_macos.hpp"
#include "internal.hpp"
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <thread>
// FFmpeg and AVFoundation both declare AVMediaType. Only FFmpeg's C enum
// is renamed in this translation unit; its C linkage is unchanged.
#define AVMediaType FFmpegMediaType
extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#undef AVMediaType

namespace {
using namespace livekit::sdk;
struct CaptureState {
  std::mutex mutex;
  std::shared_ptr<const StudioImage> image;
  std::deque<float> audio;
  std::string error;
  std::atomic<bool> active{true};
  studio_detail::MacAudioSamples converter;
  void fail(const std::string &message) {
    std::lock_guard lock(mutex);
    error = message;
  }
};
std::string message(NSError *error) {
  return error ? std::string(error.localizedDescription.UTF8String) : "macOS 采集失败";
}
void waitFor(dispatch_semaphore_t ready, const std::atomic<bool> *active = nullptr) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC))) {
    if (active && !active->load())
      throw std::runtime_error("采集已停止");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("系统设备请求超时，请检查权限后重试");
  }
}
SCShareableContent *content(const std::atomic<bool> *active = nullptr) {
  __block SCShareableContent *result = nil;
  __block NSError *failure = nil;
  auto ready = dispatch_semaphore_create(0);
  [SCShareableContent
      getShareableContentExcludingDesktopWindows:YES
                             onScreenWindowsOnly:YES
                               completionHandler:^(SCShareableContent *value, NSError *error) {
                                 result = value;
                                 failure = error;
                                 dispatch_semaphore_signal(ready);
                               }];
  waitFor(ready, active);
  if (!result)
    throw std::runtime_error("屏幕录制权限或显示器不可用：" + message(failure));
  return result;
}
void authorize(AVMediaType type, const std::atomic<bool> *active) {
  auto status = [AVCaptureDevice authorizationStatusForMediaType:type];
  if (status == AVAuthorizationStatusNotDetermined) {
    __block BOOL granted = NO;
    auto ready = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:type
                             completionHandler:^(BOOL value) {
                               granted = value;
                               dispatch_semaphore_signal(ready);
                             }];
    waitFor(ready, active);
    if (granted)
      return;
  } else if (status == AVAuthorizationStatusAuthorized)
    return;
  throw std::runtime_error("请在系统设置 → 隐私与安全性中允许摄像头或麦克风权限");
}
NSArray<AVCaptureDevice *> *cameras() {
  return [AVCaptureDeviceDiscoverySession
             discoverySessionWithDeviceTypes:@[
               AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal
             ]
                                   mediaType:AVMediaTypeVideo
                                    position:AVCaptureDevicePositionUnspecified]
      .devices;
}
void videoSample(const std::shared_ptr<CaptureState> &state, CMSampleBufferRef sample) {
  auto buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer)
    return;
  if (CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA)
    return;
  if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
    return;
  struct Unlock {
    CVPixelBufferRef value;
    ~Unlock() { CVPixelBufferUnlockBaseAddress(value, kCVPixelBufferLock_ReadOnly); }
  } unlock{buffer};
  auto image = std::make_shared<StudioImage>();
  image->width = int(CVPixelBufferGetWidth(buffer));
  image->height = int(CVPixelBufferGetHeight(buffer));
  if (image->width < 1 || image->height < 1 || image->width > 8192 || image->height > 8192)
    return;
  image->rgba.resize(size_t(image->width) * image->height * 4);
  auto *bytes = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
  auto stride = CVPixelBufferGetBytesPerRow(buffer);
  if (!bytes)
    return;
  for (int y = 0; y < image->height; ++y)
    for (int x = 0; x < image->width; ++x) {
      auto *p = bytes + size_t(y) * stride + x * 4;
      auto *q = image->rgba.data() + (size_t(y) * image->width + x) * 4;
      q[0] = p[2];
      q[1] = p[1];
      q[2] = p[0];
      q[3] = 255;
    }
  std::lock_guard lock(state->mutex);
  state->image = std::move(image);
}
void audioSample(const std::shared_ptr<CaptureState> &state, CMSampleBufferRef sample) {
  auto samples = state->converter.convert(sample);
  std::lock_guard lock(state->mutex);
  state->audio.insert(state->audio.end(), samples.begin(), samples.end());
  while (state->audio.size() > 19200)
    state->audio.pop_front();
}

} // namespace

namespace livekit::sdk::studio_detail {
MacAudioSamples::~MacAudioSamples() { swr_free(&resampler); }
std::vector<float> MacAudioSamples::convert(CMSampleBufferRef sample) {
  auto *format =
      CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
  if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mChannelsPerFrame < 1 ||
      format->mChannelsPerFrame > 8)
    throw std::runtime_error("不支持的音频格式");
  const bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
  AVSampleFormat input = AV_SAMPLE_FMT_NONE;
  if ((format->mFormatFlags & kAudioFormatFlagIsFloat) && format->mBitsPerChannel == 32)
    input = planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
  else if ((format->mFormatFlags & kAudioFormatFlagIsSignedInteger) &&
           format->mBitsPerChannel == 16)
    input = planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
  else if ((format->mFormatFlags & kAudioFormatFlagIsSignedInteger) &&
           format->mBitsPerChannel == 32)
    input = planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
  if (input == AV_SAMPLE_FMT_NONE)
    throw std::runtime_error("不支持的音频采样格式");
  size_t needed = 0;
  CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &needed, nullptr, 0, nullptr,
                                                          nullptr, 0, nullptr);
  std::vector<uint8_t> storage(needed);
  auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
  CMBlockBufferRef block = nullptr;
  if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
          sample, nullptr, list, needed, nullptr, nullptr,
          kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr)
    throw std::runtime_error("无法读取音频采样");
  struct Release {
    CMBlockBufferRef value;
    ~Release() {
      if (value)
        CFRelease(value);
    }
  } release{block};
  const int rate = int(format->mSampleRate), channels = int(format->mChannelsPerFrame),
            frames = int(CMSampleBufferGetNumSamples(sample));
  if (rate < 8000 || rate > 192000 || frames < 1 || frames > 192000)
    throw std::runtime_error("无效音频采样参数");
  if (!resampler || inputRate != rate || inputChannels != channels || inputFormat != input) {
    swr_free(&resampler);
    AVChannelLayout in{}, out = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&in, channels);
    int result = swr_alloc_set_opts2(&resampler, &out, AV_SAMPLE_FMT_FLT, 48000, &in, input, rate,
                                     0, nullptr);
    av_channel_layout_uninit(&in);
    if (result < 0 || swr_init(resampler) < 0)
      throw std::runtime_error("音频重采样初始化失败");
    inputRate = rate;
    inputChannels = channels;
    inputFormat = input;
  }
  const uint8_t *source[8]{};
  if (list->mNumberBuffers != (planar ? unsigned(channels) : 1u))
    throw std::runtime_error("无效音频缓冲布局");
  for (unsigned i = 0; i < list->mNumberBuffers; ++i) {
    if (list->mBuffers[i].mDataByteSize <
        size_t(frames) * (format->mBitsPerChannel / 8) * (planar ? 1 : channels))
      throw std::runtime_error("音频缓冲不足");
    source[i] = static_cast<const uint8_t *>(list->mBuffers[i].mData);
  }
  int capacity = swr_get_out_samples(resampler, frames);
  std::vector<float> samples(size_t(capacity) * 2);
  auto *destination = reinterpret_cast<uint8_t *>(samples.data());
  int count = swr_convert(resampler, &destination, capacity, source, frames);
  if (count < 0)
    throw std::runtime_error("音频重采样失败");
  samples.resize(size_t(count) * 2);
  return samples;
}
} // namespace livekit::sdk::studio_detail

@interface AVStudioCaptureDelegate
    : NSObject <SCStreamOutput, SCStreamDelegate, AVCaptureVideoDataOutputSampleBufferDelegate,
                AVCaptureAudioDataOutputSampleBufferDelegate> {
@public
  std::shared_ptr<CaptureState> state;
}
@end
@implementation AVStudioCaptureDelegate
- (void)consume:(CMSampleBufferRef)sample audio:(BOOL)audio {
  if (!state || !state->active || !CMSampleBufferIsValid(sample) ||
      !CMSampleBufferDataIsReady(sample))
    return;
  @autoreleasepool {
    try {
      if (audio)
        audioSample(state, sample);
      else
        videoSample(state, sample);
    } catch (const std::exception &e) {
      state->fail(e.what());
    }
  }
}
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
  if (type == SCStreamOutputTypeScreen) {
    auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
      NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
      NSNumber *status = info[SCStreamFrameInfoStatus];
      if (status && status.integerValue != SCFrameStatusComplete)
        return;
    }
  }
  [self consume:sample audio:type == SCStreamOutputTypeAudio];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  if (state->active)
    state->fail(message(error));
}
- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection *)connection {
  [self consume:sample audio:[output isKindOfClass:AVCaptureAudioDataOutput.class]];
}
@end

namespace {
class MacCapture {
public:
  std::shared_ptr<CaptureState> state = std::make_shared<CaptureState>();
  AVStudioCaptureDelegate *delegate;
  dispatch_queue_t queue;
  SCStream *stream;
  AVCaptureSession *session;
  id runtimeObserver;
  std::jthread worker;
  MacCapture(const StudioSource *source, bool system = false) {
    // Device startup and permission prompts must not stall program composition,
    // recording or the 10 ms audio clock on the Studio worker.
    auto copy = source ? std::optional<StudioSource>(*source) : std::nullopt;
    worker = std::jthread(
        [this, copy = std::move(copy), system] { configure(copy ? &*copy : nullptr, system); });
  }
  void configure(const StudioSource *source, bool system) {
    @autoreleasepool {
      bool configuring = false;
      try {
        if (!state->active)
          return;
        queue = dispatch_queue_create("org.avplatform.studio.capture", DISPATCH_QUEUE_SERIAL);
        delegate = [AVStudioCaptureDelegate new];
        delegate->state = state;
        if ((source && (source->kind == StudioSourceKind::screen ||
                        source->kind == StudioSourceKind::window)) ||
            system) {
          SCShareableContent *available = content(&state->active);
          if (!state->active)
            return;
          SCContentFilter *filter = nil;
          int width = 2, height = 2;
          if (source && source->kind == StudioSourceKind::window) {
            SCWindow *selected = nil;
            for (SCWindow *window in available.windows)
              if (std::to_string(window.windowID) == source->input) {
                selected = window;
                break;
              }
            if (!selected)
              throw std::runtime_error("窗口已关闭，请重新选择");
            filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:selected];
            width = int(selected.frame.size.width);
            height = int(selected.frame.size.height);
          } else {
            SCDisplay *selected = nil;
            for (SCDisplay *display in available.displays)
              if (!source || source->input.empty() ||
                  std::to_string(display.displayID) == source->input) {
                selected = display;
                break;
              }
            if (!selected)
              throw std::runtime_error("显示器不可用，请重新选择");
            filter = [[SCContentFilter alloc] initWithDisplay:selected excludingWindows:@[]];
            width = int(selected.width);
            height = int(selected.height);
          }
          SCStreamConfiguration *config = [SCStreamConfiguration new];
          double scale = std::min(1., 1920. / std::max(width, height));
          config.width = system ? 2 : std::max(2, int(width * scale));
          config.height = system ? 2 : std::max(2, int(height * scale));
          config.pixelFormat = kCVPixelFormatType_32BGRA;
          config.minimumFrameInterval = CMTimeMake(1, system ? 1 : 30);
          config.queueDepth = 3;
          config.showsCursor = !system;
          config.capturesAudio = system;
          config.excludesCurrentProcessAudio = YES;
          config.sampleRate = 48000;
          config.channelCount = 2;
          stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:delegate];
          NSError *error = nil;
          if (![stream addStreamOutput:delegate
                                  type:system ? SCStreamOutputTypeAudio : SCStreamOutputTypeScreen
                    sampleHandlerQueue:queue
                                 error:&error])
            throw std::runtime_error(message(error));
          auto ready = dispatch_semaphore_create(0);
          __block NSError *failure = nil;
          auto shared = state;
          SCStream *starting = stream;
          [stream startCaptureWithCompletionHandler:^(NSError *e) {
            failure = e;
            if (!shared->active)
              [starting stopCaptureWithCompletionHandler:nil];
            dispatch_semaphore_signal(ready);
          }];
          waitFor(ready, &state->active);
          if (failure)
            throw std::runtime_error(message(failure));
        } else {
          AVMediaType type = source ? AVMediaTypeVideo : AVMediaTypeAudio;
          authorize(type, &state->active);
          if (!state->active)
            return;
          AVCaptureDevice *device = nil;
          if (source) {
            for (AVCaptureDevice *item in cameras())
              if (source->input == item.uniqueID.UTF8String) {
                device = item;
                break;
              }
          } else
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
          if (!device)
            throw std::runtime_error("采集设备不可用，请重新选择");
          NSError *error = nil;
          auto input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
          if (!input)
            throw std::runtime_error(message(error));
          session = [AVCaptureSession new];
          [session beginConfiguration];
          configuring = true;
          if (![session canAddInput:input])
            throw std::runtime_error("无法连接采集设备");
          [session addInput:input];
          AVCaptureOutput *output = nil;
          if (source) {
            auto video = [AVCaptureVideoDataOutput new];
            video.videoSettings =
                @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
            video.alwaysDiscardsLateVideoFrames = YES;
            [video setSampleBufferDelegate:delegate queue:queue];
            output = video;
            if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720])
              session.sessionPreset = AVCaptureSessionPreset1280x720;
          } else {
            auto audio = [AVCaptureAudioDataOutput new];
            audio.audioSettings = @{
              AVFormatIDKey : @(kAudioFormatLinearPCM),
              AVSampleRateKey : @48000,
              AVNumberOfChannelsKey : @2,
              AVLinearPCMIsFloatKey : @YES,
              AVLinearPCMBitDepthKey : @32,
              AVLinearPCMIsBigEndianKey : @NO,
              AVLinearPCMIsNonInterleaved : @NO
            };
            [audio setSampleBufferDelegate:delegate queue:queue];
            output = audio;
          }
          if (![session canAddOutput:output])
            throw std::runtime_error("无法创建采集输出");
          [session addOutput:output];
          [session commitConfiguration];
          configuring = false;
          auto shared = state;
          runtimeObserver = [NSNotificationCenter.defaultCenter
              addObserverForName:AVCaptureSessionRuntimeErrorNotification
                          object:session
                           queue:nil
                      usingBlock:^(NSNotification *note) {
                        shared->fail(message(note.userInfo[AVCaptureSessionErrorKey]));
                      }];
          [session startRunning];
          if (!session.running)
            throw std::runtime_error("采集未启动，请检查设备和权限");
        }
      } catch (const std::exception &e) {
        state->active = false;
        state->fail(e.what());
        if (configuring)
          [session commitConfiguration];
        if (stream)
          [stream stopCaptureWithCompletionHandler:nil];
        if (session.running)
          [session stopRunning];
      }
    }
  }
  ~MacCapture() {
    @autoreleasepool {
      state->active = false;
      if (worker.joinable())
        worker.join();
      if (runtimeObserver)
        [NSNotificationCenter.defaultCenter removeObserver:runtimeObserver];
      if (session.running)
        [session stopRunning];
      if (stream)
        [stream stopCaptureWithCompletionHandler:nil];
      if (queue)
        dispatch_sync(queue, ^{
                      });
    }
  }
};
} // namespace
namespace livekit::sdk::studio_detail {
struct Capture::Impl : MacCapture {
  explicit Impl(const StudioSource &source) : MacCapture(&source) {}
};
Capture::Capture(const StudioSource &source) : impl_(std::make_unique<Impl>(source)) {}
Capture::~Capture() = default;
std::shared_ptr<const StudioImage> Capture::frame() {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->image;
}
std::string Capture::error() {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->error;
}
struct AudioCapture::Impl : MacCapture {
  explicit Impl(bool system) : MacCapture(nullptr, system) {}
};
AudioCapture::AudioCapture(bool system) : impl_(std::make_unique<Impl>(system)) {}
AudioCapture::~AudioCapture() = default;
std::vector<float> AudioCapture::take(size_t frames) {
  std::lock_guard lock(impl_->state->mutex);
  std::vector<float> out(frames * 2);
  auto &q = impl_->state->audio;
  for (size_t i = 0; i < out.size() && !q.empty(); ++i) {
    out[i] = q.front();
    q.pop_front();
  }
  return out;
}
std::string AudioCapture::error() {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->error;
}
struct Monitor::Impl {
  std::mutex mutex;
  std::deque<float> samples;
  std::string failure;
  AudioQueueRef queue = nullptr;
  static void fill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto *self = static_cast<Impl *>(context);
    auto *data = static_cast<float *>(buffer->mAudioData);
    {
      std::lock_guard lock(self->mutex);
      for (int i = 0; i < 960; ++i) {
        data[i] = self->samples.empty() ? 0 : self->samples.front();
        if (!self->samples.empty())
          self->samples.pop_front();
      }
    }
    buffer->mAudioDataByteSize = 960 * sizeof(float);
    auto status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    if (status) {
      std::lock_guard lock(self->mutex);
      self->failure = "监听音频输出失败：" + std::to_string(status);
    }
  }
  Impl() {
    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket = 8;
    auto status = AudioQueueNewOutput(&format, fill, this, nullptr, nullptr, 0, &queue);
    if (!status)
      for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        status = AudioQueueAllocateBuffer(queue, 960 * sizeof(float), &buffer);
        if (status)
          break;
        fill(this, queue, buffer);
      }
    if (!status)
      status = AudioQueueStart(queue, nullptr);
    if (status) {
      std::lock_guard lock(mutex);
      failure = "无法开启音频监听：" + std::to_string(status);
    }
  }
  ~Impl() {
    if (queue) {
      AudioQueueStop(queue, true);
      AudioQueueDispose(queue, true);
    }
  }
};
Monitor::Monitor() : impl_(std::make_unique<Impl>()) {}
Monitor::~Monitor() = default;
void Monitor::write(std::span<const float> samples) {
  std::lock_guard lock(impl_->mutex);
  impl_->samples.insert(impl_->samples.end(), samples.begin(), samples.end());
  while (impl_->samples.size() > 9600)
    impl_->samples.pop_front();
}
void Monitor::clear() {
  std::lock_guard lock(impl_->mutex);
  impl_->samples.clear();
}
std::string Monitor::error() {
  std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}

std::shared_ptr<StudioImage> load_image(const std::string &path) {
  @autoreleasepool {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!source)
      throw std::runtime_error("图片不可用：" + path);
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image)
      throw std::runtime_error("无法解码图片");
    auto out = std::make_shared<StudioImage>();
    out->width = int(CGImageGetWidth(image));
    out->height = int(CGImageGetHeight(image));
    if (out->width < 1 || out->height < 1 || out->width > 8192 || out->height > 8192) {
      CGImageRelease(image);
      throw std::runtime_error("图片尺寸超出限制");
    }
    out->rgba.resize(size_t(out->width) * out->height * 4);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        out->rgba.data(), out->width, out->height, 8, out->width * 4, color,
        CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color);
    if (!context) {
      CGImageRelease(image);
      throw std::runtime_error("无法创建图片画布");
    }
    CGContextDrawImage(context, CGRectMake(0, 0, out->width, out->height), image);
    CGContextRelease(context);
    CGImageRelease(image);
    // The compositor expects straight alpha; CoreGraphics renders premultiplied.
    for (size_t i = 0; i < out->rgba.size(); i += 4) {
      int alpha = out->rgba[i + 3];
      if (alpha)
        for (int c = 0; c < 3; ++c)
          out->rgba[i + c] = uint8_t(std::min(255, int(out->rgba[i + c]) * 255 / alpha));
    }
    return out;
  }
}
std::shared_ptr<StudioImage> text_image(const std::string &text,
                                        const std::array<float, 4> &color) {
  @autoreleasepool {
    auto out = std::make_shared<StudioImage>();
    out->width = 1280;
    out->height = 180;
    out->rgba.resize(1280 * 180 * 4);
    NSBitmapImageRep *bitmap =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:1280
                                                pixelsHigh:180
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:1280 * 4
                                              bitsPerPixel:32];
    if (!bitmap)
      throw std::runtime_error("无法创建文字画布");
    memset(bitmap.bitmapData, 0, 1280 * 180 * 4);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext
        setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
    NSString *value = [NSString stringWithUTF8String:text.c_str()];
    [value drawWithRect:NSMakeRect(4, 4, 1272, 172)
                options:NSStringDrawingUsesLineFragmentOrigin
             attributes:@{
               NSFontAttributeName : [NSFont systemFontOfSize:64 weight:NSFontWeightSemibold],
               NSForegroundColorAttributeName : NSColor.whiteColor
             }];
    [NSGraphicsContext restoreGraphicsState];
    for (size_t i = 0; i < out->rgba.size(); i += 4) {
      for (int c = 0; c < 3; ++c)
        out->rgba[i + c] = uint8_t(color[c] * 255);
      out->rgba[i + 3] = uint8_t(bitmap.bitmapData[i + 3] * color[3]);
    }
    return out;
  }
}
} // namespace livekit::sdk::studio_detail
namespace livekit::sdk {
std::vector<StudioDevice> studio_windows() {
  @autoreleasepool {
    std::vector<StudioDevice> out;
    for (SCWindow *w in content().windows) {
      if (w.owningApplication.processID == NSProcessInfo.processInfo.processIdentifier ||
          !w.title.length)
        continue;
      out.push_back({std::to_string(w.windowID), std::string(w.title.UTF8String)});
    }
    return out;
  }
}
std::vector<StudioDevice> studio_screens() {
  @autoreleasepool {
    std::vector<StudioDevice> out;
    for (SCDisplay *d in content().displays)
      out.push_back({std::to_string(d.displayID), "显示器 " + std::to_string(d.displayID) + " · " +
                                                      std::to_string(d.width) + " × " +
                                                      std::to_string(d.height)});
    return out;
  }
}
std::vector<StudioDevice> studio_cameras() {
  @autoreleasepool {
    std::vector<StudioDevice> out;
    for (AVCaptureDevice *d in cameras())
      out.push_back({d.uniqueID.UTF8String, d.localizedName.UTF8String});
    return out;
  }
}
} // namespace livekit::sdk
