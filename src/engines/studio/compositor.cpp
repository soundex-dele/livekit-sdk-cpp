#include "internal.hpp"
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif
#include "libyuv/convert.h"
#include "libyuv/convert_argb.h"

namespace livekit::sdk {
void studio_mix(std::vector<float>& a, const std::vector<float>& b, float gain) {
  if (!std::isfinite(gain) || gain < 0 || gain > 4)
    throw std::invalid_argument("Invalid audio gain");
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    if (std::isfinite(b[i]))
      a[i] += b[i] * gain;
}

StudioImage studio_composite_cpu(int w, int h, const studio_detail::Layers& layers) {
  if (w < 2 || h < 2 || w > 3840 || h > 3840)
    throw std::invalid_argument("Invalid canvas");
  StudioImage out{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  for (size_t i = 3; i < out.rgba.size(); i += 4)
    out.rgba[i] = 255;
  for (const auto& [s, img] : layers) {
    if (!s.visible || !img || s.transform.width <= 0 || s.transform.height <= 0)
      continue;
    const auto& t = s.transform;
    float a = t.rotation * 3.14159265f / 180, cs = cos(a), sn = sin(a);
    float cx = (t.x + t.width / 2) * w, cy = (t.y + t.height / 2) * h;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        float dx = x + .5f - cx, dy = y + .5f - cy;
        float u = (dx * cs + dy * sn) / (t.width * w) + .5f,
              v = (-dx * sn + dy * cs) / (t.height * h) + .5f;
        if (u < 0 || v < 0 || u >= 1 || v >= 1)
          continue;
        u = t.crop_left + u * (1 - t.crop_left - t.crop_right);
        v = t.crop_top + v * (1 - t.crop_top - t.crop_bottom);
        int sx = std::clamp(int(u * img->width), 0, img->width - 1),
            sy = std::clamp(int(v * img->height), 0, img->height - 1);
        auto* p = &img->rgba[(size_t(sy) * img->width + sx) * 4];
        float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f, alpha = p[3] / 255.f;
        if (s.chroma.enabled) {
          auto& k = s.chroma;
          float d = sqrt((r - k.color[0]) * (r - k.color[0]) + (g - k.color[1]) * (g - k.color[1]) +
                         (b - k.color[2]) * (b - k.color[2]));
          float f = std::clamp((d - k.similarity) / std::max(.001f, k.smoothness), 0.f, 1.f);
          alpha *= f;
          if (k.color[0] > std::max(k.color[1], k.color[2]) + .001f)
            r -= std::max(0.f, r - std::max(g, b)) * k.spill;
          else if (k.color[1] > std::max(k.color[0], k.color[2]) + .001f)
            g -= std::max(0.f, g - std::max(r, b)) * k.spill;
          else if (k.color[2] > std::max(k.color[0], k.color[1]) + .001f)
            b -= std::max(0.f, b - std::max(r, g)) * k.spill;
        }
        auto* q = &out.rgba[(size_t(y) * w + x) * 4];
        q[0] = uint8_t(std::clamp(r * 255 * alpha + q[0] * (1 - alpha), 0.f, 255.f));
        q[1] = uint8_t(std::clamp(g * 255 * alpha + q[1] * (1 - alpha), 0.f, 255.f));
        q[2] = uint8_t(std::clamp(b * 255 * alpha + q[2] * (1 - alpha), 0.f, 255.f));
      }
  }
  return out;
}

namespace studio_detail {
#ifdef _WIN32
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr) {
  if (FAILED(hr))
    throw std::runtime_error("Studio D3D11 error: " + std::to_string(uint32_t(hr)));
}

static const char* shader = R"(
cbuffer Settings : register(b0) { float4 transform; float4 crop; float4 key; float4 params; float4 canvas; };
struct V { float4 p:SV_POSITION;float2 uv:TEXCOORD0; };
V vs(uint id:SV_VertexID) { float2 p=float2((id==1||id==2||id==4)?1:0,(id==2||id==4||id==5)?1:0);float2 center=(transform.xy+transform.zw*.5)*canvas.xy;float2 d=(p-.5)*transform.zw*canvas.xy;float a=params.w;float2 r=float2(d.x*cos(a)-d.y*sin(a),d.x*sin(a)+d.y*cos(a))+center;V o;o.p=float4(r.x/canvas.x*2-1,1-r.y/canvas.y*2,0,1);o.uv=crop.xy+p*(1-crop.xy-crop.zw);return o; }
Texture2D image:register(t0);SamplerState sampler0:register(s0);
float4 ps(V i):SV_TARGET { float4 c=image.Sample(sampler0,i.uv);if(key.w>0){float d=distance(c.rgb,key.rgb);c.a*=saturate((d-params.x)/max(params.y,.001));float3 mask=step(max(key.gbr,key.brg)+.001,key.rgb);c.rgb-=max(0,c.rgb-max(c.gbr,c.brg))*mask*params.z;}return c; }
)";

struct Compositor::Impl {
  int w, h;
  ComPtr<ID3D11Device> d;
  ComPtr<ID3D11DeviceContext> c;
  ComPtr<ID3D11Texture2D> output, read;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11Buffer> settings;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11RasterizerState> raster;

  Impl(int width, int height, bool software) : w(width), h(height) {
    auto hr = D3D11CreateDevice(nullptr, software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                D3D11_SDK_VERSION, &d, nullptr, &c);
    if (FAILED(hr))
      check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d,
                              nullptr, &c));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(d->CreateTexture2D(&td, nullptr, &output));
    check(d->CreateRenderTargetView(output.Get(), nullptr, &target));
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(d->CreateTexture2D(&td, nullptr, &read));
    ComPtr<ID3DBlob> v, p, error;
    check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &v,
                     &error));
    check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &p,
                     &error));
    check(d->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs));
    check(d->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(d->CreateBuffer(&bd, nullptr, &settings));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check(d->CreateSamplerState(&sd, &sampler));
    D3D11_BLEND_DESC b{};
    auto& rt = b.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(d->CreateBlendState(&b, &blend));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    check(d->CreateRasterizerState(&rd, &raster));
  }

  StudioImage render(const Layers& layers) {
    float black[]{0, 0, 0, 1};
    c->ClearRenderTargetView(target.Get(), black);
    c->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    c->OMSetBlendState(blend.Get(), nullptr, 0xffffffff);
    c->RSSetState(raster.Get());
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    c->RSSetViewports(1, &vp);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(ps.Get(), nullptr, 0);
    c->VSSetConstantBuffers(0, 1, settings.GetAddressOf());
    c->PSSetConstantBuffers(0, 1, settings.GetAddressOf());
    c->PSSetSamplers(0, 1, sampler.GetAddressOf());
    for (auto& [s, img] : layers) {
      if (!s.visible || !img || img->width < 1 || img->height < 1)
        continue;
      D3D11_TEXTURE2D_DESC td{};
      td.Width = img->width;
      td.Height = img->height;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data{img->rgba.data(), UINT(img->width * 4), 0};
      ComPtr<ID3D11Texture2D> tex;
      ComPtr<ID3D11ShaderResourceView> view;
      check(d->CreateTexture2D(&td, &data, &tex));
      check(d->CreateShaderResourceView(tex.Get(), nullptr, &view));
      const auto& t = s.transform;
      auto& k = s.chroma;
      float values[]{t.x,
                     t.y,
                     t.width,
                     t.height,
                     t.crop_left,
                     t.crop_top,
                     t.crop_right,
                     t.crop_bottom,
                     k.color[0],
                     k.color[1],
                     k.color[2],
                     k.enabled ? 1.f : 0.f,
                     k.similarity,
                     k.smoothness,
                     k.spill,
                     t.rotation * 3.14159265f / 180,
                     float(w),
                     float(h),
                     0,
                     0};
      c->UpdateSubresource(settings.Get(), 0, nullptr, values, 0, 0);
      c->PSSetShaderResources(0, 1, view.GetAddressOf());
      c->Draw(6, 0);
      ID3D11ShaderResourceView* empty = nullptr;
      c->PSSetShaderResources(0, 1, &empty);
    }
    c->CopyResource(read.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(c->Map(read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    StudioImage out{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (int y = 0; y < h; ++y)
      memcpy(out.rgba.data() + size_t(y) * w * 4,
             static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(w) * 4);
    c->Unmap(read.Get(), 0);
    return out;
  }
};

Compositor::Compositor(int w, int h, bool soft) : impl_(std::make_unique<Impl>(w, h, soft)) {}

Compositor::~Compositor() = default;

StudioImage Compositor::render(const Layers& l) {
  return impl_->render(l);
}

#endif
std::shared_ptr<StudioImage> from_i420(const VideoFrame& f) {
  auto img = std::make_shared<StudioImage>();
  img->width = f.width;
  img->height = f.height;
  img->rgba.resize(size_t(f.width) * f.height * 4);
  if (libyuv::I420ToABGR(f.y.data(), f.stride_y, f.u.data(), f.stride_u, f.v.data(), f.stride_v,
                         img->rgba.data(), f.width * 4, f.width, f.height) != 0)
    throw std::runtime_error("Invalid I420 frame");
  return img;
}

VideoFrame to_i420(const StudioImage& i) {
  VideoFrame f;
  f.width = i.width;
  f.height = i.height;
  f.stride_y = f.width;
  f.stride_u = f.stride_v = (f.width + 1) / 2;
  f.y.resize(size_t(f.width) * f.height);
  f.u.resize(size_t(f.stride_u) * ((f.height + 1) / 2));
  f.v.resize(f.u.size());
  if (libyuv::ABGRToI420(i.rgba.data(), i.width * 4, f.y.data(), f.stride_y, f.u.data(), f.stride_u,
                         f.v.data(), f.stride_v, f.width, f.height) != 0)
    throw std::runtime_error("RGBA conversion failed");
  return f;
}
}  // namespace studio_detail
}  // namespace livekit::sdk
