#include "frame_dump.h"

#include "rhi/vk_core.h"
#include "d3d11/d3d11_internal.h"

#include "DX11Device.h"
#include "phxEngine.h"

// phxEngine.h declares the D3D11 globals but not the swap chain; it lives in
// Phoenix/phxEngine.cpp and is published by the player at startup.
extern IDXGISwapChain *SwapChain;

#include <cstdio>
#include <cstring>

namespace phxplayer
{

namespace
{

struct Rgba
{
  int r, g, b, a;
};

// Half -> float, for the R16G16B16A16_FLOAT render targets the engine uses
// everywhere.  Without this the dump of a float target is meaningless bytes.
float HalfToFloat(unsigned short h)
{
  const unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
  float v;
  if (exp == 0)
    v = (float)mant * (1.0f / 16777216.0f); // subnormal
  else if (exp == 31)
    v = mant ? 0.0f : 1e30f; // NaN -> 0, Inf -> saturated
  else
    v = (float)(mant | 0x400) * powf(2.0f, (float)exp - 25.0f);
  return sign ? -v : v;
}

// The engine works in linear light and the composite is HDR, so show the
// values with a simple Reinhard curve; raw linear would look black.
int LinearToByte(float v)
{
  if (!(v > 0.0f)) return 0;
  float m = v / (1.0f + v);
  m = powf(m, 1.0f / 2.2f);
  int b = (int)(m * 255.0f + 0.5f);
  return b < 0 ? 0 : (b > 255 ? 255 : b);
}

// DXGI formats we can meet on the back buffer, and how to turn a pixel into
// 8-bit RGB.
Rgba DecodePixel(const unsigned char *p, DXGI_FORMAT fmt)
{
  Rgba c{0, 0, 0, 255};
  switch (fmt)
  {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    c.r = p[0];
    c.g = p[1];
    c.b = p[2];
    c.a = p[3];
    break;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    c.b = p[0];
    c.g = p[1];
    c.r = p[2];
    c.a = p[3];
    break;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  {
    const unsigned short *h = (const unsigned short *)p;
    c.r = LinearToByte(HalfToFloat(h[0]));
    c.g = LinearToByte(HalfToFloat(h[1]));
    c.b = LinearToByte(HalfToFloat(h[2]));
    break;
  }
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  {
    unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    c.r = (int)((v & 0x3ff) * 255 / 1023);
    c.g = (int)(((v >> 10) & 0x3ff) * 255 / 1023);
    c.b = (int)(((v >> 20) & 0x3ff) * 255 / 1023);
    c.a = 255;
    break;
  }
  default:
    // Unknown: copy the first three bytes through so the dump is still
    // diagnosable rather than blank.
    c.r = p[0];
    c.g = p[1];
    c.b = p[2];
    break;
  }
  return c;
}

} // namespace

bool ReadTextureRGB24(ID3D11Texture2D *srcTexture, std::vector<uint8_t> &out, UINT *width, UINT *height,
                      std::string *error)
{
  auto fail = [&](const char *what) {
    if (error) *error = what;
    phxvk::LogError("frame readback: %s", what);
    return false;
  };

  if (!srcTexture) return fail("no source texture");
  if (!phxDev || !phxContext) return fail("no D3D11 device/context");

  ID3D11Texture2D *back = srcTexture;
  back->AddRef();

  D3D11_TEXTURE2D_DESC desc;
  back->GetDesc(&desc);

  D3D11_TEXTURE2D_DESC sdesc = desc;
  sdesc.Usage = D3D11_USAGE_STAGING;
  sdesc.BindFlags = 0;
  sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sdesc.MiscFlags = 0;
  sdesc.MipLevels = 1;
  sdesc.ArraySize = 1;

  // One staging texture per size, not one per frame: creating and freeing a
  // 1080p staging image every frame cost 21 ms/frame (measured: 720p and 1080p
  // both 31.1 ms/frame, versus 9.9 ms/frame without the readback at all).
  static ID3D11Texture2D *cachedStaging = nullptr;
  static UINT cachedWidth = 0, cachedHeight = 0;
  static DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;
  if (cachedStaging && (cachedWidth != sdesc.Width || cachedHeight != sdesc.Height ||
                        cachedFormat != sdesc.Format))
  {
    cachedStaging->Release();
    cachedStaging = nullptr;
  }
  if (!cachedStaging)
  {
    if (FAILED(phxDev->CreateTexture2D(&sdesc, nullptr, &cachedStaging)) || !cachedStaging)
    {
      back->Release();
      return fail("could not create the staging texture");
    }
    cachedWidth = sdesc.Width;
    cachedHeight = sdesc.Height;
    cachedFormat = sdesc.Format;
  }
  ID3D11Texture2D *staging = cachedStaging;
  staging->AddRef();

  phxContext->CopyResource(staging, back);

  D3D11_MAPPED_SUBRESOURCE mapped;
  memset(&mapped, 0, sizeof(mapped));
  if (FAILED(phxContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
  {
    staging->Release();
    back->Release();
    return fail("Map(staging) failed");
  }

  const size_t bytesPerPixel =
      (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
      (desc.Format == DXGI_FORMAT_R16G16_FLOAT) ? 4 :
      (desc.Format == DXGI_FORMAT_R16_UNORM || desc.Format == DXGI_FORMAT_R16_FLOAT) ? 2 : 4;

  out.resize((size_t)desc.Width * desc.Height * 3);
  for (UINT y = 0; y < desc.Height; y++)
  {
    const unsigned char *src = (const unsigned char *)mapped.pData + (size_t)y * mapped.RowPitch;
    unsigned char *dst = out.data() + (size_t)y * desc.Width * 3;
    for (UINT x = 0; x < desc.Width; x++)
    {
      Rgba c = DecodePixel(src + (size_t)x * bytesPerPixel, desc.Format);
      dst[x * 3 + 0] = (unsigned char)c.r;
      dst[x * 3 + 1] = (unsigned char)c.g;
      dst[x * 3 + 2] = (unsigned char)c.b;
    }
  }

  phxContext->Unmap(staging, 0);
  staging->Release();
  back->Release();

  if (width) *width = desc.Width;
  if (height) *height = desc.Height;
  return true;
}

bool DumpTextureToPPM(ID3D11Texture2D *srcTexture, const char *path, std::string *error)
{
  std::vector<uint8_t> pixels;
  UINT width = 0, height = 0;
  if (!ReadTextureRGB24(srcTexture, pixels, &width, &height, error)) return false;

  FILE *f = fopen(path, "wb");
  if (!f)
  {
    if (error) *error = "could not open the output file";
    phxvk::LogError("frame dump: could not open %s", path);
    return false;
  }
  fprintf(f, "P6\n%u %u\n255\n", width, height);
  fwrite(pixels.data(), 1, pixels.size(), f);
  fclose(f);
  phxvk::LogInfo("frame dump: wrote %s (%ux%u)", path, width, height);
  return true;
}

// Set by the player when it wants frames larger than the window can be: the
// swap chain is bound to the window, but the engine only reads the *back buffer
// view*, so an off-screen texture of any size can stand in for it.
ID3D11Texture2D *g_offscreenBackBuffer = nullptr;

void SetOffscreenBackBuffer(ID3D11Texture2D *texture) { g_offscreenBackBuffer = texture; }

bool ReadBackBufferRGB(std::vector<uint8_t> &out, UINT *width, UINT *height, std::string *error)
{
  ID3D11Texture2D *back = nullptr;
  if (g_offscreenBackBuffer)
  {
    back = g_offscreenBackBuffer;
    back->AddRef();
  }
  else
  {
    if (!SwapChain)
    {
      if (error) *error = "no swap chain";
      phxvk::LogError("frame readback: no swap chain");
      return false;
    }
    if (FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back)
    {
      if (error) *error = "SwapChain::GetBuffer(0) failed";
      phxvk::LogError("frame readback: SwapChain::GetBuffer(0) failed");
      return false;
    }
  }
  bool ok = ReadTextureRGB24(back, out, width, height, error);
  back->Release();
  return ok;
}

bool DumpBackBufferToPPM(const char *path, std::string *error)
{
  if (!SwapChain)
  {
    if (error) *error = "no swap chain";
    phxvk::LogError("frame dump: no swap chain");
    return false;
  }
  ID3D11Texture2D *back = nullptr;
  if (FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back)
  {
    if (error) *error = "SwapChain::GetBuffer(0) failed";
    phxvk::LogError("frame dump: SwapChain::GetBuffer(0) failed");
    return false;
  }
  bool ok = DumpTextureToPPM(back, path, error);
  back->Release();
  return ok;
}

} // namespace phxplayer

namespace phxplayer
{

// Asynchronous readback for video export.
//
// The synchronous path copies into a staging texture and then maps it, and Map
// waits for the GPU (ContextImpl::Map -> SubmitAndWait), so every frame stalls:
// the CPU waits for the render and then copies while the GPU idles.  That is the
// 69 ms/frame at 1080p against ~15 ms of actual rendering.
//
// Three staging textures, read the one from two frames ago: FrameBegin waits the
// fence of the slot that frame used, so when frame N is being recorded, frame
// N-2's copy has completed and reading it needs no wait at all.
namespace
{
struct AsyncReadback
{
  static const int kRing = 3;
  ID3D11Texture2D *staging[kRing] = {};
  void *mapped[kRing] = {};
  unsigned rowPitch[kRing] = {};
  unsigned width = 0, height = 0;
  int next = 0;
  int submitted = 0;
};
AsyncReadback g_async;
} // namespace

bool ReadBackBufferRGBAsync(std::vector<uint8_t> &out, UINT *width, UINT *height, bool *haveFrame)
{
  if (haveFrame) *haveFrame = false;

  ID3D11Texture2D *back = g_offscreenBackBuffer;
  if (back)
  {
    back->AddRef();
  }
  else
  {
    if (!SwapChain) return false;
    if (FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back) return false;
  }

  D3D11_TEXTURE2D_DESC desc;
  back->GetDesc(&desc);
  if (g_async.width != desc.Width || g_async.height != desc.Height)
  {
    for (int i = 0; i < AsyncReadback::kRing; i++)
      if (g_async.staging[i]) { g_async.staging[i]->Release(); g_async.staging[i] = nullptr; }
    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sdesc.MiscFlags = 0;
    sdesc.MipLevels = 1;
    sdesc.ArraySize = 1;
    for (int i = 0; i < AsyncReadback::kRing; i++)
    {
      if (FAILED(phxDev->CreateTexture2D(&sdesc, nullptr, &g_async.staging[i])) || !g_async.staging[i])
      {
        back->Release();
        return false;
      }
      g_async.mapped[i] = phxd3d11::StagingHostPointer(g_async.staging[i], &g_async.rowPitch[i]);
    }
    g_async.width = desc.Width;
    g_async.height = desc.Height;
    g_async.next = 0;
    g_async.submitted = 0;
  }

  // Submit this frame's copy, then read the slot filled two frames ago.
  const int cur = g_async.next;
  phxContext->CopyResource(g_async.staging[cur], back);
  back->Release();
  g_async.next = (cur + 1) % AsyncReadback::kRing;
  g_async.submitted++;
  if (g_async.submitted <= 2) return true; // nothing has completed yet

  const int read = (cur + 1) % AsyncReadback::kRing; // == (cur - 2) mod 3
  if (!g_async.mapped[read]) return false;

  // BGRA straight through: the frame is already in that order, and converting it
  // here cost ~2000000 iterations per frame at 1080p.  ffmpeg reads bgra with
  // -pix_fmt bgra, so the export does a row copy and nothing else.
  const size_t rowBytes = (size_t)g_async.width * 4;
  out.resize(rowBytes * g_async.height);
  const unsigned char *src = (const unsigned char *)g_async.mapped[read];
  for (unsigned y = 0; y < g_async.height; y++)
    memcpy(out.data() + (size_t)y * rowBytes, src + (size_t)y * g_async.rowPitch[read], rowBytes);
  if (width) *width = g_async.width;
  if (height) *height = g_async.height;
  if (haveFrame) *haveFrame = true;
  return true;
}

} // namespace phxplayer
