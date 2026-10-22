#include "frame_dump.h"

#include "rhi/vk_core.h"

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

  ID3D11Texture2D *staging = nullptr;
  if (FAILED(phxDev->CreateTexture2D(&sdesc, nullptr, &staging)) || !staging)
  {
    back->Release();
    return fail("could not create the staging texture");
  }

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

bool ReadBackBufferRGB(std::vector<uint8_t> &out, UINT *width, UINT *height, std::string *error)
{
  if (!SwapChain)
  {
    if (error) *error = "no swap chain";
    phxvk::LogError("frame readback: no swap chain");
    return false;
  }
  ID3D11Texture2D *back = nullptr;
  if (FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back)
  {
    if (error) *error = "SwapChain::GetBuffer(0) failed";
    phxvk::LogError("frame readback: SwapChain::GetBuffer(0) failed");
    return false;
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
