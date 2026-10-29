#include "internal.hpp"
#import <Metal/Metal.h>
#include <cstring>

namespace livekit::sdk::studio_detail {
// Each dispatch composites one layer into an opaque program canvas. Buffers
// use shared storage so the same path works on Intel and Apple Silicon.
static NSString* const shader = @R"(
#include <metal_stdlib>
using namespace metal;
struct Settings { float4 transform, crop, key, params, canvas; };
kernel void composite(device uchar4* dst [[buffer(0)]],
                      const device uchar4* src [[buffer(1)]],
                      constant Settings& s [[buffer(2)]], uint2 p [[thread_position_in_grid]]) {
 uint w=uint(s.canvas.x), h=uint(s.canvas.y);
 if(p.x>=w || p.y>=h) return;
 float2 d=float2(p)+.5-(s.transform.xy+s.transform.zw*.5)*s.canvas.xy;
 float cs=cos(s.params.w), sn=sin(s.params.w);
 float2 uv=float2(d.x*cs+d.y*sn,-d.x*sn+d.y*cs)/(s.transform.zw*s.canvas.xy)+.5;
 if(any(uv<0) || any(uv>=1)) return;
 uv=s.crop.xy+uv*(1-s.crop.xy-s.crop.zw);
 uint2 size=uint2(s.canvas.zw);
 uint2 q=min(uint2(max(uv,0.0)*float2(size)),size-1);
 float4 c=float4(src[q.y*size.x+q.x])/255.;
 if(s.key.w>0) {
   c.a*=clamp((distance(c.rgb,s.key.rgb)-s.params.x)/max(.001,s.params.y),0.,1.);
   float3 mask=step(max(s.key.gbr,s.key.brg)+.001,s.key.rgb);
   c.rgb-=max(float3(0),c.rgb-max(c.gbr,c.brg))*mask*s.params.z;
 }
 float3 previous=float3(dst[p.y*w+p.x].rgb)/255.;
 dst[p.y*w+p.x]=uchar4(uchar3(clamp((c.rgb*c.a+previous*(1-c.a))*255.,0.,255.)),255);
}
)";

struct Compositor::Impl {
  int w, h;
  bool software;
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> output;

  Impl(int width, int height, bool soft) : w(width), h(height), software(soft) {
    if (software)
      return;
    @autoreleasepool {
      device = MTLCreateSystemDefaultDevice();
      if (!device) {
        software = true;
        return;
      }
      NSError* error = nil;
      id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
      if (!library)
        throw std::runtime_error("Metal shader: " +
                                 std::string(error.localizedDescription.UTF8String));
      pipeline =
          [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"composite"]
                                                error:&error];
      queue = [device newCommandQueue];
      output = [device newBufferWithLength:size_t(w) * h * 4 options:MTLResourceStorageModeShared];
      if (!pipeline || !queue || !output)
        throw std::runtime_error("无法初始化 Metal 合成器");
    }
  }

  StudioImage render(const Layers& layers) {
    if (software)
      return studio_composite_cpu(w, h, layers);
    @autoreleasepool {
      auto* pixels = static_cast<uint8_t*>(output.contents);
      std::memset(pixels, 0, size_t(w) * h * 4);
      for (size_t i = 3; i < size_t(w) * h * 4; i += 4)
        pixels[i] = 255;
      id<MTLCommandBuffer> command = [queue commandBuffer];
      if (!command)
        throw std::runtime_error("Metal command allocation failed");
      for (const auto& [s, img] : layers) {
        if (!s.visible || !img || img->width < 1 || img->height < 1 || s.transform.width <= 0 ||
            s.transform.height <= 0)
          continue;
        id<MTLBuffer> source = [device newBufferWithBytes:img->rgba.data()
                                                   length:img->rgba.size()
                                                  options:MTLResourceStorageModeShared];
        if (!source)
          throw std::runtime_error("Metal source allocation failed");
        const auto& t = s.transform;
        const auto& k = s.chroma;
        float values[]{
            t.x,          t.y,          t.width,           t.height,
            t.crop_left,  t.crop_top,   t.crop_right,      t.crop_bottom,
            k.color[0],   k.color[1],   k.color[2],        k.enabled ? 1.f : 0.f,
            k.similarity, k.smoothness, k.spill,           t.rotation * 3.14159265f / 180,
            float(w),     float(h),     float(img->width), float(img->height)};
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:source offset:0 atIndex:1];
        [encoder setBytes:values length:sizeof(values) atIndex:2];
        NSUInteger tw = pipeline.threadExecutionWidth,
                   th = std::min<NSUInteger>(8, pipeline.maxTotalThreadsPerThreadgroup / tw);
        [encoder dispatchThreadgroups:MTLSizeMake((w + tw - 1) / tw, (h + th - 1) / th, 1)
                threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
        [encoder endEncoding];
      }
      [command commit];
      [command waitUntilCompleted];
      if (command.status == MTLCommandBufferStatusError)
        throw std::runtime_error("Metal 合成失败");
      return {w, h, std::vector<uint8_t>(pixels, pixels + size_t(w) * h * 4)};
    }
  }
};

Compositor::Compositor(int w, int h, bool soft) : impl_(std::make_unique<Impl>(w, h, soft)) {}

Compositor::~Compositor() = default;

StudioImage Compositor::render(const Layers& layers) {
  return impl_->render(layers);
}
}  // namespace livekit::sdk::studio_detail
