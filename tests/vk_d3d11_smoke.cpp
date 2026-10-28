// Headless render smoke test for the D3D11-on-Vulkan shim.
//
// Every case is a real render pass whose result is read back through a staging
// texture, so a shim that merely links or silently ignores state fails here.
//
//   1. quad              the engine's full-screen quad shader, its 2-element
//                        vxdecl layout and a sampled texture (the original
//                        smoke test, kept unchanged as the baseline).
//   2. cbuffer           R1 + R7: `cbuffer b{float t,aspect,xres,yres;}` must
//                        keep D3D packing (offsets 0/4/8/12, not std140's
//                        0/16/32/48), and four Map(WRITE_DISCARD) updates
//                        *inside one frame* must each be seen by their own draw
//                        (the ring-renaming path of docs/DESIGN-d3d11-shim.md §1).
//   3. stripped input    the engine's 6-element vxdecl2, whose Position1 is
//                        declared by the shader and never read.  glslang strips
//                        it, so the SPIR-V has a hole at location 1; element[i]
//                        must still pair with location i (DESIGN §4).  UVs come
//                        from element 4, so a compacting layout samples the
//                        wrong vertex data and the four quadrants come out equal.
//                        This also pins the R5 orientation convention.
//   4. mrt               one draw into two render targets.
//   5. depth             D32_FLOAT depth, near/far draws in both orders: the
//                        near draw must win twice.
//   6. typeless depth    R3: an R24G8_TYPELESS image with a D24_UNORM_S8_UINT
//                        DSV and an R24_UNORM_X8_TYPELESS SRV bound at t7, read
//                        back through a shader (Project.cpp:94-96,221-228).
//   7. generate mips     R8: a MipLevels=0 + MISC_GENERATE_MIPS
//                        R16G16B16A16_FLOAT render target; every generated level
//                        is copied out and checked against the source pattern.
//
// Usage:  ./phx_smoke [--validation]
//         --validation enables VK_LAYER_KHRONOS_validation (D3D11_CREATE_DEVICE_DEBUG).
#include <d3d11.h>
#include <dxgi.h>
#include <D3Dcompiler.h>
#include <D3DX11.h>

#include "../src/rhi/vk_core.h"
#include "../src/rhi/shader_compiler.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////
// case bookkeeping

namespace
{

struct CaseResult
{
  const char *name;
  bool ok;
  const char *detail;
};

CaseResult g_results[16];
int g_resultCount = 0;
int g_failures = 0;
const char *g_currentCase = "";

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;

void Fail(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  printf("  FAIL: ");
  vprintf(fmt, args);
  printf("\n");
  va_end(args);
  g_failures++;
  g_results[g_resultCount].name = g_currentCase;
  g_results[g_resultCount].ok = false;
  g_results[g_resultCount].detail = fmt;
  g_resultCount++;
}

#define CHECK(cond, ...)                 \
  do                                     \
  {                                      \
    if (!(cond))                         \
    {                                    \
      Fail(__VA_ARGS__);                 \
      return false;                      \
    }                                    \
  } while (0)

// A case that returned early after a CHECK failure has already been recorded;
// this records the success.

void Passed()
{
  g_results[g_resultCount].name = g_currentCase;
  g_results[g_resultCount].ok = true;
  g_results[g_resultCount].detail = nullptr;
  g_resultCount++;
}

//////////////////////////////////////////////////////////////////////////////
// helpers

struct Target
{
  ID3D11Texture2D *tex = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;

  void Release()
  {
    if (srv) srv->Release();
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    srv = nullptr;
    rtv = nullptr;
    tex = nullptr;
  }
};

struct Readback
{
  std::vector<uint8_t> bytes;
  UINT rowPitch = 0;
  UINT width = 0;
  UINT height = 0;
  UINT bpp = 4;

  const uint8_t *Texel(UINT x, UINT y) const { return bytes.data() + (size_t)y * rowPitch + (size_t)x * bpp; }
};

// A render target plus its SRV, and a same-format staging texture to read it
// back through.
bool MakeTarget(UINT w, UINT h, DXGI_FORMAT format, UINT bindFlags, UINT miscFlags, UINT mipLevels, Target &out)
{
  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = mipLevels;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bindFlags;
  desc.MiscFlags = miscFlags;

  if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &out.tex)) || !out.tex)
  {
    Fail("CreateTexture2D(%ux%u fmt %d) failed", w, h, (int)format);
    return false;
  }
  if ((bindFlags & D3D11_BIND_RENDER_TARGET) &&
      (FAILED(g_device->CreateRenderTargetView(out.tex, nullptr, &out.rtv)) || !out.rtv))
  {
    Fail("CreateRenderTargetView(%ux%u fmt %d) failed", w, h, (int)format);
    return false;
  }
  if (bindFlags & D3D11_BIND_SHADER_RESOURCE)
  {
    if (FAILED(g_device->CreateShaderResourceView(out.tex, nullptr, &out.srv)) || !out.srv)
    {
      Fail("CreateShaderResourceView(%ux%u fmt %d) failed", w, h, (int)format);
      return false;
    }
  }
  return true;
}

// CopySubresourceRegion into a fresh staging texture and map it.
bool ReadTexture(ID3D11Texture2D *src, UINT subresource, UINT w, UINT h, DXGI_FORMAT format, Readback &out)
{
  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = w;
  desc.Height = h;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ID3D11Texture2D *staging = nullptr;
  if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
  {
    Fail("CreateTexture2D(staging %ux%u fmt %d) failed", w, h, (int)format);
    return false;
  }

  g_context->CopySubresourceRegion(staging, 0, 0, 0, 0, src, subresource, nullptr);

  D3D11_MAPPED_SUBRESOURCE mapped;
  memset(&mapped, 0, sizeof(mapped));
  if (FAILED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
  {
    Fail("Map(staging %ux%u) failed", w, h);
    staging->Release();
    return false;
  }

  out.rowPitch = mapped.RowPitch;
  out.width = w;
  out.height = h;
  out.bytes.resize((size_t)mapped.RowPitch * h);
  memcpy(out.bytes.data(), mapped.pData, out.bytes.size());
  g_context->Unmap(staging, 0);
  staging->Release();
  return true;
}

bool CompileShader(const char *source, const char *entry, const char *target, ID3DBlob **blobOut, const char *what)
{
  ID3DBlob *errors = nullptr;
  *blobOut = nullptr;
  HRESULT hr = D3DCompile(source, strlen(source), what, nullptr, nullptr, entry, target, 0, 0, blobOut, &errors);
  if (FAILED(hr) || !*blobOut)
  {
    Fail("D3DCompile(%s, entry '%s', %s) failed: %s", what, entry, target,
         errors ? (const char *)errors->GetBufferPointer() : "(no message)");
    if (errors) errors->Release();
    return false;
  }
  if (errors) errors->Release();
  CHECK((*blobOut)->GetBufferSize() >= 4, "%s: empty blob", what);
  CHECK(*(const uint32_t *)(*blobOut)->GetBufferPointer() == 0x07230203, "%s: the blob is not SPIR-V", what);
  return true;
}

// Prints the Offset member decorations of a module: the answer to the spec's
// §7 test 1 (cbuffer packing) without needing spirv-dis.
void PrintMemberOffsets(const void *blob, size_t size, const char *label)
{
  const uint32_t *w = (const uint32_t *)blob;
  const size_t words = size / sizeof(uint32_t);
  if (words < 5 || w[0] != 0x07230203) return;

  printf("  %s cbuffer member offsets:", label);
  for (size_t i = 5; i < words;)
  {
    const uint32_t inst = w[i];
    const uint32_t wc = inst >> 16;
    const uint32_t op = inst & 0xFFFF;
    if (wc == 0 || i + wc > words) return false;
    if (op == 72 /*OpMemberDecorate*/ && wc >= 5 && (w[i + 3] & 0xFFFF) == 35 /*Offset*/)
      printf(" [%u]=%u", w[i + 2], w[i + 4]);
    i += wc;
  }
  printf("\n");
}

bool MakeVertexShader(const char *source, const char *entry, const char *what, ID3DBlob **blobOut, ID3D11VertexShader **vsOut)
{
  if (!CompileShader(source, entry, "vs_5_0", blobOut, what)) return false;
  if (FAILED(g_device->CreateVertexShader((*blobOut)->GetBufferPointer(), (*blobOut)->GetBufferSize(), nullptr, vsOut)) || !*vsOut)
  {
    Fail("CreateVertexShader(%s) failed", what);
    return false;
  }
  return true;
}

bool MakePixelShader(const char *source, const char *entry, const char *what, ID3D11PixelShader **psOut)
{
  ID3DBlob *blob = nullptr;
  if (!CompileShader(source, entry, "ps_5_0", &blob, what)) return false;
  bool ok = SUCCEEDED(g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, psOut)) && *psOut;
  blob->Release();
  if (!ok) Fail("CreatePixelShader(%s) failed", what);
  return ok;
}

// The engine's quad geometry (phxEngine.cpp:125) and its vxdecl layout (:126-131).
const float kVxBuffer[] = {-1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, -1, 0, 1, 1, 1,
                           -1, 1, 0, 1, 0, 0, 1, -1, 0, 1, 1, 1, -1, -1, 0, 1, 0, 1};

const D3D11_INPUT_ELEMENT_DESC kVxDecl[2] = {
    {"PositionT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Texcoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

// The engine's 6-element mesh layout (phxEngine.cpp:133-141), verbatim.
const D3D11_INPUT_ELEMENT_DESC kVxDecl2[6] = {
    {"Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Position", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float) * 3, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float) * 6, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, sizeof(float) * 9, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 10, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"Texcoord", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 14, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

ID3D11Buffer *MakeQuadVertexBuffer()
{
  D3D11_BUFFER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.ByteWidth = sizeof(kVxBuffer);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA data;
  memset(&data, 0, sizeof(data));
  data.pSysMem = kVxBuffer;
  ID3D11Buffer *vb = nullptr;
  if (FAILED(g_device->CreateBuffer(&desc, &data, &vb)) || !vb) Fail("CreateBuffer(quad vertices) failed");
  return vb;
}

ID3D11Buffer *MakeDynamicConstantBuffer(UINT bytes)
{
  D3D11_BUFFER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.ByteWidth = bytes;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer *cb = nullptr;
  if (FAILED(g_device->CreateBuffer(&desc, nullptr, &cb)) || !cb) Fail("CreateBuffer(constant, %u bytes) failed", bytes);
  return cb;
}

ID3D11SamplerState *MakePointSampler()
{
  D3D11_SAMPLER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *s = nullptr;
  if (FAILED(g_device->CreateSamplerState(&desc, &s)) || !s) Fail("CreateSamplerState failed");
  return s;
}

// A solid-colour texture, as in the original smoke test.
ID3D11Texture2D *MakeSolidTexture(UINT size, const float rgba[4], ID3D11ShaderResourceView **srvOut)
{
  std::vector<float> texels((size_t)size * size * 4);
  for (size_t i = 0; i < (size_t)size * size; i++)
    for (int c = 0; c < 4; c++) texels[i * 4 + c] = rgba[c];

  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = size;
  desc.Height = size;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data;
  memset(&data, 0, sizeof(data));
  data.pSysMem = texels.data();
  data.SysMemPitch = size * 4 * sizeof(float);

  ID3D11Texture2D *tex = nullptr;
  if (FAILED(g_device->CreateTexture2D(&desc, &data, &tex)) || !tex)
  {
    Fail("CreateTexture2D(solid %ux%u) failed", size, size);
    return nullptr;
  }
  if (FAILED(g_device->CreateShaderResourceView(tex, nullptr, srvOut)) || !*srvOut)
  {
    Fail("CreateShaderResourceView(solid) failed");
    return nullptr;
  }
  return tex;
}

// A 2x2 texture with four distinct colours; POINT sampling makes each quadrant
// of a full-screen quad sample exactly one texel.
ID3D11Texture2D *MakeQuadrantTexture(ID3D11ShaderResourceView **srvOut)
{
  const uint8_t texels[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = 2;
  desc.Height = 2;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data;
  memset(&data, 0, sizeof(data));
  data.pSysMem = texels;
  data.SysMemPitch = 8;

  ID3D11Texture2D *tex = nullptr;
  if (FAILED(g_device->CreateTexture2D(&desc, &data, &tex)) || !tex)
  {
    Fail("CreateTexture2D(2x2 quadrants) failed");
    return nullptr;
  }
  if (FAILED(g_device->CreateShaderResourceView(tex, nullptr, srvOut)) || !*srvOut)
  {
    Fail("CreateShaderResourceView(2x2 quadrants) failed");
    return nullptr;
  }
  return tex;
}

// Viewport and scissor are context state in D3D11 and survive a frame, so every
// case sets both: case 2 narrows the scissor to one 64px strip, and without
// this the cases after it would inherit that rectangle.
void SetFullViewport(UINT size)
{
  D3D11_VIEWPORT vp{0, 0, (FLOAT)size, (FLOAT)size, 0, 1};
  g_context->RSSetViewports(1, &vp);
  D3D11_RECT scissor{0, 0, (LONG)size, (LONG)size};
  g_context->RSSetScissorRects(1, &scissor);
}

ID3D11RasterizerState *MakeRasterizerState()
{
  D3D11_RASTERIZER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.FillMode = D3D11_FILL_SOLID;
  desc.CullMode = D3D11_CULL_NONE; // the quad is drawn in both orders; no culling
  desc.DepthClipEnable = TRUE;
  ID3D11RasterizerState *rs = nullptr;
  if (FAILED(g_device->CreateRasterizerState(&desc, &rs)) || !rs) Fail("CreateRasterizerState failed");
  return rs;
}

//////////////////////////////////////////////////////////////////////////////
// shaders

// Verbatim from third_party/apEx/Phoenix/phxEngine.cpp:146-156.
const char kQuadShader[] =
    "SamplerState o:register(s1);"
    "Texture2D r:register(t0);"
    "struct l{float4 o:positiont;float2 r:texcoord0;};"
    "struct i{float4 o:position,r:texcoord0;};"
    "struct n{float3 o:position0,r:position1,u:normal0;float4 c:color0,s:texcoord0,a:texcoord1;};"
    "struct t{float2 o:texcoord0;float4 r:sv_position;};"
    "t u(l c){t s;s.r=c.o;s.o=c.r;return s;}"
    "t c(n s){t a;a.r=0;a.o=0;return a;}"
    "t s(i a){t e;e.r=0;e.o=0;return e;}"
    "float4 a(t e):sv_target0{return r.Sample(o,e.o);}";

// The scalar prologue of the Clean Slate post-process shaders, with the same
// member names and no register(): the implicit $Globals constant buffer is b0.
// (The struct cannot be called `t` here: the cbuffer already has a member `t`.)
const char kCbufferShader[] =
    "cbuffer b{float t;float aspect;float xres;float yres;};"
    "struct l{float4 o:positiont;float2 r:texcoord0;};"
    "struct qt{float2 o:texcoord0;float4 r:sv_position;};"
    "qt u(l c){qt s;s.r=c.o;s.o=c.r;return s;}"
    "float4 a(qt e):sv_target0{return float4(t,aspect,xres,yres);}";

// The engine's mesh vertex shader shape (struct n from phxEngine.cpp:149):
// Position1 is declared but never read, so glslang strips it and location 1 is
// a hole.  The shader reads Position0 (0), Normal0 (2) and Texcoord0 (4); UVs
// come from element 4 (R32G32B32A32 at byte 40), so a layout that compacted the
// holes would feed location 4 from element 2 (Normal0 = 0.5,0.5,0.5) instead,
// and sampling at uv (0,0) would give one flat colour everywhere.
const char kStrippedInputShader[] =
    "Texture2D r:register(t0);"
    "SamplerState o:register(s1);"
    "struct n{float3 o:position0,r:position1,u:normal0;float4 c:color0,s:texcoord0,a:texcoord1;};"
    "struct t{float4 r:sv_position;float2 o:texcoord0;float n:normal0;};"
    "t u(n s){t a;a.r=float4(s.o,1);a.o=s.s.xy;a.n=s.u.x;return a;}"
    "float4 p(t e):sv_target0{return r.Sample(o,e.o)*(e.n*2);}";

// MRT: target 0 follows the texture, target 1 is a fixed colour.
const char kMrtShader[] =
    "Texture2D r:register(t0);"
    "SamplerState o:register(s1);"
    "struct l{float4 o:positiont;float2 r:texcoord0;};"
    "struct t{float2 o:texcoord0;float4 r:sv_position;};"
    "struct p{float4 a:sv_target0;float4 b:sv_target1;};"
    "t u(l c){t s;s.r=c.o;s.o=c.r;return s;}"
    "p a(t e){p q;q.a=r.Sample(o,e.o);q.b=float4(0.25,0.5,0.75,1);return q;}";

// Depth test: the vertex shader takes z from the constant buffer, the pixel
// shader returns the buffer's colour.
const char kDepthShader[] =
    "cbuffer cb{float z;float pad0;float pad1;float pad2;float4 col;};"
    "struct l{float4 o:positiont;};"
    "struct t{float4 r:sv_position;};"
    "t u(l c){t s;s.r=float4(c.o.xy,z,1);return s;}"
    "float4 a(t e):sv_target0{return col;}";

// R3: the depth SRV is hard-wired to t7 by the engine's post effects.
const char kDepthSrvShader[] =
    "Texture2D z:register(t7);"
    "SamplerState o:register(s1);"
    "struct l{float4 o:positiont;float2 r:texcoord0;};"
    "struct t{float2 o:texcoord0;float4 r:sv_position;};"
    "t u(l c){t s;s.r=c.o;s.o=c.r;return s;}"
    "float4 a(t e):sv_target0{float d=z.Sample(o,e.o).x;return float4(d,d,d,1);}";

// R8: left half black, right half white, so every generated mip level has an
// exactly known content at every texel.
const char kMipShader[] =
    "struct l{float4 o:positiont;};"
    "struct t{float4 r:sv_position;float s:texcoord0;};"
    "t u(l c){t q;q.r=c.o;q.s=c.o.x;return q;}"
    "float4 a(t e):sv_target0{return e.s<0 ? float4(0,0,0,1) : float4(1,1,1,1);}";

//////////////////////////////////////////////////////////////////////////////
// 1 - the engine's quad

bool CaseQuad()
{
  g_currentCase = "quad (engine shader, vxdecl, sampled texture)";
  printf("\n=== 1. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  Target rt, textureHolder;
  ID3D11ShaderResourceView *srv = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11Buffer *vb = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3D11RasterizerState *rs = nullptr;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!CompileShader(kQuadShader, "u", "vs_5_0", &vsBlob, "quad.hlsl")) return false;
    if (!CompileShader(kQuadShader, "a", "ps_5_0", &psBlob, "quad.hlsl")) return false;

    ID3D11ShaderReflection *reflection = nullptr;
    if (FAILED(D3DReflect(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), IID_ID3D11ShaderReflection, (void **)&reflection)) || !reflection)
    {
      Fail("D3DReflect failed");
      return false;
    }
    D3D11_SHADER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    if (FAILED(reflection->GetDesc(&desc)))
    {
      reflection->Release();
      Fail("reflection GetDesc failed");
      return false;
    }
    printf("  vs reflection: %u inputs\n", desc.InputParameters);
    reflection->Release();
    if (desc.InputParameters < 2)
    {
      Fail("the vertex shader should report at least 2 input parameters, got %u", desc.InputParameters);
      return false;
    }

    if (FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) || !vs)
    {
      Fail("CreateVertexShader failed");
      return false;
    }
    if (FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)) || !ps)
    {
      Fail("CreatePixelShader failed");
      return false;
    }
    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }

    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt)) return false;

    const float texel[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    textureHolder.tex = MakeSolidTexture(4, texel, &srv);
    if (!textureHolder.tex) return false;

    sampler = MakePointSampler();
    if (!sampler) return false;
    vb = MakeQuadVertexBuffer();
    if (!vb) return false;
    cb = MakeDynamicConstantBuffer(256);
    if (!cb) return false;
    rs = MakeRasterizerState();
    if (!rs) return false;

    // The quad shader has no cbuffer, but the engine's dynamic path must work.
    for (int pass = 0; pass < 2; pass++)
    {
      D3D11_MAPPED_SUBRESOURCE mapped;
      memset(&mapped, 0, sizeof(mapped));
      if (FAILED(g_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
      {
        Fail("Map(constant buffer) failed");
        return false;
      }
      float *f = (float *)mapped.pData;
      for (int i = 0; i < 64; i++) f[i] = (float)pass;
      g_context->Unmap(cb, 0);
    }
    g_context->VSSetConstantBuffers(0, 1, &cb);
    g_context->PSSetConstantBuffers(0, 1, &cb);

    SetFullViewport(256);
    g_context->RSSetState(rs);

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
    g_context->ClearRenderTargetView(rt.rtv, clear);

    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &srv);
    g_context->PSSetSamplers(1, 1, &sampler);
    g_context->Draw(6, 0);

    Readback rb;
    if (!ReadTexture(rt.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb)) return false;

    size_t nonZero = 0;
    for (UINT y = 0; y < 256; y++)
      for (UINT x = 0; x < 256; x++)
      {
        const uint8_t *p = rb.Texel(x, y);
        if (p[0] || p[1] || p[2]) nonZero++;
      }

    const uint8_t *centre = rb.Texel(128, 128);
    const int expected[4] = {(int)lroundf(texel[0] * 255.0f), (int)lroundf(texel[1] * 255.0f), (int)lroundf(texel[2] * 255.0f), (int)lroundf(texel[3] * 255.0f)};
    printf("  render target: %zu/65536 pixels non-zero\n", nonZero);
    printf("  centre pixel: R=%d G=%d B=%d A=%d (expected %d %d %d %d)\n", centre[0], centre[1], centre[2], centre[3], expected[0], expected[1],
           expected[2], expected[3]);

    if (nonZero < 256u * 256u / 2)
    {
      Fail("too few pixels were written by the draw (%zu)", nonZero);
      return false;
    }
    const int tolerance = 2;
    if (abs(centre[0] - expected[0]) > tolerance || abs(centre[1] - expected[1]) > tolerance || abs(centre[2] - expected[2]) > tolerance ||
        abs(centre[3] - expected[3]) > tolerance)
    {
      Fail("the centre pixel does not match the sampled texture value");
      return false;
    }
    return true;
  }();

  if (rs) rs->Release();
  if (cb) cb->Release();
  if (vb) vb->Release();
  if (sampler) sampler->Release();
  if (srv) srv->Release();
  if (textureHolder.tex) textureHolder.tex->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (psBlob) psBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 2 - cbuffer packing (R1) and per-draw renaming within one frame (R7)

bool CaseCbufferAndRenaming()
{
  g_currentCase = "cbuffer packing (R1) + 4 dynamic CB updates in one frame (R7)";
  printf("\n=== 2. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  Target rt;
  ID3D11Buffer *vb = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3D11RasterizerState *rs = nullptr;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kCbufferShader, "u", "cbuffer.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kCbufferShader, "a", "cbuffer.hlsl", &ps)) return false;
    // The offsets must be D3D's (0/4/8/12), not std140's (0/16/32/48).  They
    // are read from the *pixel* shader: the vertex shader never touches the
    // cbuffer, so glslang drops the whole block from its module.
    {
      ID3DBlob *psBlob = nullptr;
      if (!CompileShader(kCbufferShader, "a", "ps_5_0", &psBlob, "cbuffer.hlsl")) return false;
      PrintMemberOffsets(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), "ps");

      const uint32_t *w = (const uint32_t *)psBlob->GetBufferPointer();
      const size_t words = psBlob->GetBufferSize() / 4;
      uint32_t offsets[8] = {0};
      bool seen[8] = {false};
      for (size_t i = 5; i < words;)
      {
        const uint32_t inst = w[i];
        const uint32_t wc = inst >> 16;
        const uint32_t op = inst & 0xFFFF;
        if (wc == 0 || i + wc > words) return false;
        if (op == 72 /*OpMemberDecorate*/ && wc >= 5 && (w[i + 3] & 0xFFFF) == 35 /*Offset*/ && w[i + 2] < 8)
        {
          offsets[w[i + 2]] = w[i + 4];
          seen[w[i + 2]] = true;
        }
        i += wc;
      }
      psBlob->Release();
      CHECK(seen[0] && seen[1] && seen[2] && seen[3] && offsets[0] == 0 && offsets[1] == 4 && offsets[2] == 8 && offsets[3] == 12,
            "R1: cbuffer member offsets are %u/%u/%u/%u, expected 0/4/8/12 (std140 would be 0/16/32/48)", offsets[0], offsets[1], offsets[2],
            offsets[3]);
    }

    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }
    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt)) return false;
    vb = MakeQuadVertexBuffer();
    if (!vb) return false;
    cb = MakeDynamicConstantBuffer(256);
    if (!cb) return false;
    rs = MakeRasterizerState();
    if (!rs) return false;

    SetFullViewport(256);
    g_context->RSSetState(rs);
    g_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
    const float clear[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(rt.rtv, clear);

    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    // Bound once, exactly like a pass does before its per-instance loop.
    g_context->PSSetConstantBuffers(0, 1, &cb);

    // Four strips, four updates, all inside one frame: no Present, no readback
    // and therefore no implicit submit between them.
    const float values[4][4] = {
        {1.00f, 0.50f, 0.25f, 1.00f},
        {0.00f, 0.75f, 1.00f, 1.00f},
        {0.50f, 0.50f, 0.50f, 1.00f},
        {0.25f, 0.00f, 0.75f, 1.00f},
    };

    for (int strip = 0; strip < 4; strip++)
    {
      D3D11_MAPPED_SUBRESOURCE mapped;
      memset(&mapped, 0, sizeof(mapped));
      if (FAILED(g_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
      {
        Fail("Map(constant buffer) failed on strip %d", strip);
        return false;
      }
      memcpy(mapped.pData, values[strip], sizeof(values[strip]));
      g_context->Unmap(cb, 0);

      D3D11_RECT scissor{strip * 64, 0, (strip + 1) * 64, 256};
      g_context->RSSetScissorRects(1, &scissor);
      g_context->Draw(6, 0);
    }

    Readback rb;
    if (!ReadTexture(rt.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb)) return false;

    bool mismatch = false;
    for (int strip = 0; strip < 4; strip++)
    {
      const uint8_t *p = rb.Texel((UINT)(strip * 64 + 32), 128);
      const int expected[4] = {(int)lroundf(values[strip][0] * 255.0f), (int)lroundf(values[strip][1] * 255.0f), (int)lroundf(values[strip][2] * 255.0f),
                               (int)lroundf(values[strip][3] * 255.0f)};
      printf("  strip %d: R=%d G=%d B=%d A=%d (expected %d %d %d %d)\n", strip, p[0], p[1], p[2], p[3], expected[0], expected[1], expected[2],
             expected[3]);
      if (abs(p[0] - expected[0]) > 2 || abs(p[1] - expected[1]) > 2 || abs(p[2] - expected[2]) > 2 || abs(p[3] - expected[3]) > 2) mismatch = true;
    }
    CHECK(!mismatch, "a strip did not show the constant buffer update that preceded its draw (R7 renaming or R1 offsets are wrong)");
    return true;
  }();

  if (rs) rs->Release();
  if (cb) cb->Release();
  if (vb) vb->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 3 - a stripped vertex input must not shift the element -> location pairs

bool CaseStrippedInput()
{
  g_currentCase = "stripped input layout (vxdecl2, Position1 unused)";
  printf("\n=== 3. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  ID3D11Texture2D *texture = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11Buffer *vb = nullptr;
  Target rt;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kStrippedInputShader, "u", "stripped.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kStrippedInputShader, "p", "stripped.hlsl", &ps)) return false;

    // The reflected input locations must be a subset of [0, 6) and must *not*
    // have been compacted: the shader declares Position0/Position1/Normal0/
    // Color0/Texcoord0/Texcoord1 and reads 0, 2 and 4.
    {
      phxvk::CompiledShader compiled = phxvk::CompileHLSL(kStrippedInputShader, strlen(kStrippedInputShader), "u", phxvk::ShaderStage::Vertex, "stripped.hlsl");
      CHECK(compiled.ok, "recompiling the stripped-input shader through phxvk failed: %s", compiled.error.c_str());
      printf("  reflected input locations:");
      for (auto &in : compiled.reflection.inputs) printf(" %u(%s)", in.location, in.name.c_str());
      printf("\n");
      // The declaration order is Position0, Position1, Normal0, Color0,
      // Texcoord0, Texcoord1 and the shader reads indices 0, 2 and 4.  glslang
      // does not compact locations, so those exact three must come back.
      std::vector<uint32_t> locations;
      for (auto &in : compiled.reflection.inputs)
      {
        CHECK(in.location < 6, "reflected location %u is outside the 6-element layout", in.location);
        locations.push_back(in.location);
      }
      std::sort(locations.begin(), locations.end());
      CHECK(locations.size() == 3 && locations[0] == 0 && locations[1] == 2 && locations[2] == 4,
            "the input locations are not the declared 0/2/4 (the Position1 hole did not survive at location 1)");
    }

    if (FAILED(g_device->CreateInputLayout(kVxDecl2, 6, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl2, 6 elements) failed");
      return false;
    }

    // 6 vertices x 18 floats = the engine's 72-byte mesh vertex.
    // [0..2] Position0, [3..5] Position1 (never read - deliberately bogus),
    // [6..8] Normal0, [9] Color0 (RGBA8 packed), [10..13] Texcoord0, [14..17] Texcoord1.
    const float quad[6][6] = {
        {-1, 1, 0, 0, 0, 0}, {1, 1, 0, 1, 0, 0}, {1, -1, 0, 1, 1, 0}, {-1, 1, 0, 0, 0, 0}, {1, -1, 0, 1, 1, 0}, {-1, -1, 0, 0, 1, 0},
    };
    float vertices[6][18];
    memset(vertices, 0, sizeof(vertices));
    for (int v = 0; v < 6; v++)
    {
      vertices[v][0] = quad[v][0];
      vertices[v][1] = quad[v][1];
      vertices[v][2] = quad[v][2];
      vertices[v][3] = 9.0f; // Position1: if the layout mispairs, the UVs come from here
      vertices[v][4] = 9.0f;
      vertices[v][5] = 9.0f;
      vertices[v][6] = vertices[v][7] = vertices[v][8] = 0.5f; // Normal0
      ((uint32_t *)&vertices[v][9])[0] = 0xFFFFFFFFu;           // Color0 = R8G8B8A8_UNORM
      vertices[v][10] = quad[v][3];                             // Texcoord0.xy
      vertices[v][11] = quad[v][4];
    }

    D3D11_BUFFER_DESC vbDesc;
    memset(&vbDesc, 0, sizeof(vbDesc));
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData;
    memset(&vbData, 0, sizeof(vbData));
    vbData.pSysMem = vertices;
    if (FAILED(g_device->CreateBuffer(&vbDesc, &vbData, &vb)) || !vb)
    {
      Fail("CreateBuffer(mesh vertices) failed");
      return false;
    }

    texture = MakeQuadrantTexture(&srv);
    if (!texture) return false;
    sampler = MakePointSampler();
    if (!sampler) return false;
    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt)) return false;

    SetFullViewport(256);
    g_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
    const float clear[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(rt.rtv, clear);

    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 18;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &srv);
    g_context->PSSetSamplers(1, 1, &sampler);
    g_context->Draw(6, 0);

    Readback rb;
    if (!ReadTexture(rt.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb)) return false;

    // Quadrant centres: with the D3D convention (uv 0,0 at the top left) these
    // are the four texels of the 2x2 source.
    const struct
    {
      UINT x, y;
      const char *name;
      uint8_t r, g, b;
    } quadrants[4] = {
        {64, 64, "top-left", 255, 0, 0}, {192, 64, "top-right", 0, 255, 0}, {64, 192, "bottom-left", 0, 0, 255}, {192, 192, "bottom-right", 255, 255, 255},
    };

    bool mismatch = false;
    for (int q = 0; q < 4; q++)
    {
      const uint8_t *p = rb.Texel(quadrants[q].x, quadrants[q].y);
      printf("  %s: R=%d G=%d B=%d (expected %d %d %d)\n", quadrants[q].name, p[0], p[1], p[2], quadrants[q].r, quadrants[q].g, quadrants[q].b);
      if (abs(p[0] - quadrants[q].r) > 2 || abs(p[1] - quadrants[q].g) > 2 || abs(p[2] - quadrants[q].b) > 2) mismatch = true;
    }
    CHECK(!mismatch, "the quadrants did not sample the 2x2 texture correctly: element[i] <-> location i pairing or the y-flip is wrong");
    return true;
  }();

  if (vb) vb->Release();
  if (sampler) sampler->Release();
  if (srv) srv->Release();
  if (texture) texture->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 4 - multiple render targets

bool CaseMrt()
{
  g_currentCase = "MRT (two render targets in one draw)";
  printf("\n=== 4. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  ID3D11Texture2D *texture = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11Buffer *vb = nullptr;
  Target rt0, rt1;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kMrtShader, "u", "mrt.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kMrtShader, "a", "mrt.hlsl", &ps)) return false;
    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }

    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt0)) return false;
    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt1)) return false;

    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    texture = MakeSolidTexture(4, red, &srv);
    if (!texture) return false;
    sampler = MakePointSampler();
    if (!sampler) return false;
    vb = MakeQuadVertexBuffer();
    if (!vb) return false;

    SetFullViewport(256);
    ID3D11RenderTargetView *views[2] = {rt0.rtv, rt1.rtv};
    g_context->OMSetRenderTargets(2, views, nullptr);
    const float clear[4] = {0, 1, 0, 1};
    g_context->ClearRenderTargetView(rt0.rtv, clear);
    g_context->ClearRenderTargetView(rt1.rtv, clear);

    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &srv);
    g_context->PSSetSamplers(1, 1, &sampler);
    g_context->Draw(6, 0);

    Readback rb0, rb1;
    if (!ReadTexture(rt0.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb0)) return false;
    if (!ReadTexture(rt1.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb1)) return false;

    const uint8_t *p0 = rb0.Texel(128, 128);
    const uint8_t *p1 = rb1.Texel(128, 128);
    printf("  target 0 (texture): R=%d G=%d B=%d A=%d (expected 255 0 0 255)\n", p0[0], p0[1], p0[2], p0[3]);
    printf("  target 1 (constant): R=%d G=%d B=%d A=%d (expected 64 128 191 255)\n", p1[0], p1[1], p1[2], p1[3]);

    CHECK(abs(p0[0] - 255) <= 2 && abs(p0[1] - 0) <= 2 && abs(p0[2] - 0) <= 2, "render target 0 did not receive the sampled texture");
    CHECK(abs(p1[0] - 64) <= 2 && abs(p1[1] - 128) <= 2 && abs(p1[2] - 191) <= 2, "render target 1 did not receive sv_target1");
    return true;
  }();

  if (vb) vb->Release();
  if (sampler) sampler->Release();
  if (srv) srv->Release();
  if (texture) texture->Release();
  rt0.Release();
  rt1.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 5 - depth testing

bool CaseDepth()
{
  g_currentCase = "depth test (D32_FLOAT, near/far in both orders)";
  printf("\n=== 5. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  ID3D11DepthStencilState *dsState = nullptr;
  ID3D11Texture2D *depthTex = nullptr;
  ID3D11DepthStencilView *dsv = nullptr;
  ID3D11Buffer *vb = nullptr;
  ID3D11Buffer *cb = nullptr;
  Target rt;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kDepthShader, "u", "depth.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kDepthShader, "a", "depth.hlsl", &ps)) return false;
    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }

    D3D11_TEXTURE2D_DESC dd;
    memset(&dd, 0, sizeof(dd));
    dd.Width = 256;
    dd.Height = 256;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_device->CreateTexture2D(&dd, nullptr, &depthTex)) || !depthTex)
    {
      Fail("CreateTexture2D(depth D32_FLOAT) failed");
      return false;
    }
    if (FAILED(g_device->CreateDepthStencilView(depthTex, nullptr, &dsv)) || !dsv)
    {
      Fail("CreateDepthStencilView failed");
      return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds;
    memset(&ds, 0, sizeof(ds));
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(g_device->CreateDepthStencilState(&ds, &dsState)) || !dsState)
    {
      Fail("CreateDepthStencilState failed");
      return false;
    }

    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt)) return false;
    vb = MakeQuadVertexBuffer();
    if (!vb) return false;
    cb = MakeDynamicConstantBuffer(256);
    if (!cb) return false;

    SetFullViewport(256);
    g_context->OMSetRenderTargets(1, &rt.rtv, dsv);
    g_context->OMSetDepthStencilState(dsState, 0);
    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, &cb);
    g_context->PSSetConstantBuffers(0, 1, &cb);

    // Writes z at offset 0 and the colour at offset 16, the D3D layout of
    // {float z; float pad[3]; float4 col;}.
    struct DepthConstants
    {
      float z;
      float pad[3];
      float col[4];
    };

    auto SetConstants = [&](float z, float r, float g, float b) -> bool {
      D3D11_MAPPED_SUBRESOURCE mapped;
      memset(&mapped, 0, sizeof(mapped));
      if (FAILED(g_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) return false;
      DepthConstants *c = (DepthConstants *)mapped.pData;
      c->z = z;
      c->pad[0] = c->pad[1] = c->pad[2] = 0;
      c->col[0] = r;
      c->col[1] = g;
      c->col[2] = b;
      c->col[3] = 1.0f;
      g_context->Unmap(cb, 0);
      return true;
    };

    // D3D11 clears are explicit: clearing the depth buffer between draws would
    // legitimately reset the comparison, so each sub-check clears once.
    auto Clear = [&](const float *color, float depth) {
      g_context->ClearRenderTargetView(rt.rtv, color);
      g_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, depth, 0);
    };

    auto Pixel = [&](const char *what, int r, int g, int b) -> bool {
      Readback rb;
      if (!ReadTexture(rt.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb)) return false;
      const uint8_t *p = rb.Texel(128, 128);
      printf("  %s: R=%d G=%d B=%d (expected %d %d %d)\n", what, p[0], p[1], p[2], r, g, b);
      if (abs(p[0] - r) > 2 || abs(p[1] - g) > 2 || abs(p[2] - b) > 2)
      {
        Fail("%s: got R=%d G=%d B=%d", what, p[0], p[1], p[2]);
        return false;
      }
      return true;
    };

    const float black[4] = {0, 0, 0, 1};
    const float green[4] = {0, 1, 0, 1};

    // 1. near then far: only a working depth test keeps the near colour.
    Clear(black, 1.0f);
    if (!SetConstants(0.25f, 1, 0, 0)) { Fail("Map(constant buffer) failed (near)"); return false; }
    g_context->Draw(6, 0);
    if (!SetConstants(0.75f, 0, 0, 1)) { Fail("Map(constant buffer) failed (far)"); return false; }
    g_context->Draw(6, 0);
    if (!Pixel("near(0.25, red) then far(0.75, blue)", 255, 0, 0)) return false;

    // 2. far then near: the near draw still wins.
    Clear(black, 1.0f);
    if (!SetConstants(0.75f, 0, 0, 1)) { Fail("Map(constant buffer) failed (far first)"); return false; }
    g_context->Draw(6, 0);
    if (!SetConstants(0.25f, 1, 0, 0)) { Fail("Map(constant buffer) failed (near second)"); return false; }
    g_context->Draw(6, 0);
    if (!Pixel("far(0.75, blue) then near(0.25, red)", 255, 0, 0)) return false;

    // 3. the comparison itself: with depth cleared to 0, a 0.75 draw must be
    //    rejected outright, leaving the clear colour.
    Clear(green, 0.0f);
    if (!SetConstants(0.75f, 0, 0, 1)) { Fail("Map(constant buffer) failed (rejected)"); return false; }
    g_context->Draw(6, 0);
    if (!Pixel("depth clear 0.0, draw z=0.75 (must be rejected)", 0, 255, 0)) return false;

    // 4. depth *writes*: A(0.9) then B(0.5) leaves 0.5, so C(0.7) must lose
    //    even though 0.7 < the original clear value of 1.0.
    Clear(black, 1.0f);
    if (!SetConstants(0.9f, 1, 0, 0)) { Fail("Map(constant buffer) failed (A)"); return false; }
    g_context->Draw(6, 0);
    if (!SetConstants(0.5f, 0, 1, 0)) { Fail("Map(constant buffer) failed (B)"); return false; }
    g_context->Draw(6, 0);
    if (!SetConstants(0.7f, 0, 0, 1)) { Fail("Map(constant buffer) failed (C)"); return false; }
    g_context->Draw(6, 0);
    if (!Pixel("A(0.9) B(0.5) C(0.7): the depth written by B must reject C", 0, 255, 0)) return false;
    return true;
  }();

  if (cb) cb->Release();
  if (vb) vb->Release();
  if (dsv) dsv->Release();
  if (depthTex) depthTex->Release();
  if (dsState) dsState->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 6 - R3: typeless depth read back through an SRV at t7

bool CaseTypelessDepthSrv()
{
  g_currentCase = "typeless depth SRV at t7 (R3)";
  printf("\n=== 6. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  ID3D11Texture2D *depthTex = nullptr;
  ID3D11DepthStencilView *dsv = nullptr;
  ID3D11ShaderResourceView *depthSrv = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11Buffer *vb = nullptr;
  Target rt;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kDepthSrvShader, "u", "depthread.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kDepthSrvShader, "a", "depthread.hlsl", &ps)) return false;
    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }

    // Project.cpp:96
    D3D11_TEXTURE2D_DESC dd;
    memset(&dd, 0, sizeof(dd));
    dd.Width = 256;
    dd.Height = 256;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&dd, nullptr, &depthTex)) || !depthTex)
    {
      Fail("CreateTexture2D(R24G8_TYPELESS depth) failed");
      return false;
    }

    // Project.cpp:94
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
    memset(&dvd, 0, sizeof(dvd));
    dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateDepthStencilView(depthTex, &dvd, &dsv)) || !dsv)
    {
      Fail("CreateDepthStencilView(D24_UNORM_S8_UINT over R24G8_TYPELESS) failed");
      return false;
    }

    // Project.cpp:95
    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    memset(&svd, 0, sizeof(svd));
    svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MostDetailedMip = 0;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(depthTex, &svd, &depthSrv)) || !depthSrv)
    {
      Fail("CreateShaderResourceView(R24_UNORM_X8_TYPELESS over R24G8_TYPELESS) failed");
      return false;
    }

    sampler = MakePointSampler();
    if (!sampler) return false;
    vb = MakeQuadVertexBuffer();
    if (!vb) return false;
    if (!MakeTarget(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 1, rt)) return false;

    SetFullViewport(256);

    // Fill the depth buffer with a known value.
    g_context->OMSetRenderTargets(1, &rt.rtv, dsv);
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(rt.rtv, black);
    g_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);

    // Second pass: sample the depth through the SRV at t7.
    g_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->PSSetShaderResources(7, 1, &depthSrv);
    g_context->PSSetSamplers(1, 1, &sampler);
    g_context->Draw(6, 0);

    Readback rb;
    if (!ReadTexture(rt.tex, 0, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, rb)) return false;
    const uint8_t *p = rb.Texel(128, 128);
    printf("  sampled depth written as 0.5: R=%d G=%d B=%d (expected ~128)\n", p[0], p[1], p[2]);
    CHECK(abs(p[0] - 128) <= 4, "the depth SRV read back %d, expected ~128 (R3: typeless depth aliasing)", p[0]);
    return true;
  }();

  if (vb) vb->Release();
  if (sampler) sampler->Release();
  if (depthSrv) depthSrv->Release();
  if (dsv) dsv->Release();
  if (depthTex) depthTex->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

//////////////////////////////////////////////////////////////////////////////
// 7 - R8: GenerateMips on a MipLevels=0 render target

bool CaseGenerateMips()
{
  g_currentCase = "GenerateMips on a MipLevels=0 MISC_GENERATE_MIPS RT (R8)";
  printf("\n=== 7. %s ===\n", g_currentCase);

  ID3DBlob *vsBlob = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *layout = nullptr;
  ID3D11Buffer *vb = nullptr;
  Target rt;

  // The body runs in a lambda so that a CHECK failure returns out of the
  // lambda while the cleanup below still runs (no leaked D3D11 objects).
  bool ok = [&]() -> bool
  {
    if (!MakeVertexShader(kMipShader, "u", "mips.hlsl", &vsBlob, &vs)) return false;
    if (!MakePixelShader(kMipShader, "a", "mips.hlsl", &ps)) return false;
    if (FAILED(g_device->CreateInputLayout(kVxDecl, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)) || !layout)
    {
      Fail("CreateInputLayout(vxdecl) failed");
      return false;
    }

    // Project.cpp:199-209: a render target with the full mip chain.
    if (!MakeTarget(64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_GENERATE_MIPS, 0, rt))
      return false;

    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    rt.tex->GetDesc(&desc);
    printf("  MipLevels=0 texture reports %u mip levels (expected 7)\n", desc.MipLevels);
    CHECK(desc.MipLevels == 7, "MipLevels=0 was not expanded into the full chain (got %u)", desc.MipLevels);

    vb = MakeQuadVertexBuffer();
    if (!vb) return false;

    SetFullViewport(64);
    g_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(rt.rtv, black);
    g_context->IASetInputLayout(layout);
    UINT stride = sizeof(float) * 6;
    UINT offset = 0;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs, nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    g_context->Draw(6, 0);

    g_context->GenerateMips(rt.srv);

    // Every generated level keeps the left-black / right-white split, so the
    // bits are exactly half(0.0)=0x0000 and half(1.0)=0x3C00.
    struct Level
    {
      UINT mip;
      UINT size;
      UINT leftX;
      UINT rightX;
    };
    const Level levels[2] = {{1, 32, 2, 29}, {2, 16, 2, 13}};

    for (int l = 0; l < 2; l++)
    {
      Readback rb;
      if (!ReadTexture(rt.tex, levels[l].mip, levels[l].size, levels[l].size, DXGI_FORMAT_R16G16B16A16_FLOAT, rb)) return false;
      rb.bpp = 8;
      const uint16_t *left = (const uint16_t *)rb.Texel(levels[l].leftX, levels[l].size / 2);
      const uint16_t *right = (const uint16_t *)rb.Texel(levels[l].rightX, levels[l].size / 2);
      printf("  mip %u (%ux%u): left texel r=0x%04x (expect 0x0000), right texel r=0x%04x (expect 0x3c00)\n", levels[l].mip, levels[l].size, levels[l].size,
             left[0], right[0]);
      CHECK(left[0] == 0x0000, "mip %u is not black on the left (got 0x%04x): GenerateMips did not produce this level", levels[l].mip, left[0]);
      CHECK(right[0] == 0x3C00, "mip %u is not white on the right (got 0x%04x): GenerateMips did not produce this level", levels[l].mip, right[0]);
    }

    // The engine saves texgen render targets with D3DX11SaveTextureToMemory and
    // CphxMesh::CalculateTint (Phoenix/Mesh.cpp:1160) then reads the payload as
    // `unsigned short*` with four shorts per pixel starting at byte 128 - i.e.
    // it requires a 16-bit-per-channel DDS of exactly w*h*8 bytes at offset
    // 128.  A 32-bit BGRA header would make it read twice as far as the buffer.
    {
      ID3DBlob *dds = nullptr;
      CHECK(SUCCEEDED(D3DX11SaveTextureToMemory(g_context, rt.tex, D3DX11_IFF_DDS, &dds, 0)) && dds, "D3DX11SaveTextureToMemory(DDS) failed");

      const uint8_t *bytes = (const uint8_t *)dds->GetBufferPointer();
      const SIZE_T size = dds->GetBufferSize();
      const uint32_t *hdr = (const uint32_t *)(bytes + 4); // past the "DDS " magic
      const uint32_t width = hdr[3], height = hdr[2], pitch = hdr[4];
      const uint32_t pfFlags = hdr[19], fourCC = hdr[20], bitCount = hdr[21];
      const uint32_t expectPayload = 64u * 64u * 8u;
      printf("  DDS: %zu bytes (expected %u), %ux%u pitch %u, pfFlags 0x%x fourCC %u bitCount %u\n", (size_t)size, 128u + expectPayload, width, height,
             pitch, pfFlags, fourCC, bitCount);

      CHECK(size == 128 + expectPayload, "the DDS payload is %zu bytes, expected %u (%ux%u at 8 bytes per pixel)", (size_t)size - 128, expectPayload, 64u,
            64u);
      CHECK(width == 64 && height == 64, "the DDS header says %ux%u", width, height);
      CHECK((pfFlags & 0x4u) != 0 && fourCC == 113, "the DDS pixel format is not A16B16G16R16F (flags 0x%x, fourCC %u)", pfFlags, fourCC);

      // The same left-black/right-white pattern, read the way the engine reads it.
      const uint16_t *payload = (const uint16_t *)(bytes + 128);
      printf("  DDS texel 0: r=0x%04x g=0x%04x b=0x%04x a=0x%04x (expect 0000 0000 0000 3c00)\n", payload[0], payload[1], payload[2], payload[3]);
      const uint16_t *right = payload + 32 * 4;
      printf("  DDS texel 32: r=0x%04x (expect 0x3c00)\n", right[0]);
      CHECK(payload[0] == 0x0000 && payload[3] == 0x3C00, "the DDS payload does not hold the drawn half-float value");
      CHECK(right[0] == 0x3C00, "the DDS row stride is wrong (texel 32 is not the right half)");
      dds->Release();
    }
    return true;
  }();

  if (vb) vb->Release();
  rt.Release();
  if (layout) layout->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (vsBlob) vsBlob->Release();
  if (ok) Passed();
  return ok;
}

} // namespace

//////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
  // Unbuffered: a crash inside the shim must not swallow the output that says
  // which case crashed.
  setvbuf(stdout, nullptr, _IONBF, 0);

  const bool validation = argc > 1 && !strcmp(argv[1], "--validation");
  // --case=N runs a single case, which is what makes a flaky case debuggable.
  bool runCase[8] = {false, true, true, true, true, true, true, true};
  bool filtered = false;
  for (int i = 1; i < argc; i++)
  {
    if (!strncmp(argv[i], "--case=", 7))
    {
      if (!filtered)
      {
        for (int c = 0; c < 8; c++) runCase[c] = false;
        filtered = true;
      }
      const char *list = argv[i] + 7;
      while (*list)
      {
        const int n = atoi(list);
        if (n >= 1 && n <= 7) runCase[n] = true;
        while (*list && *list != ',') list++;
        if (*list == ',') list++;
      }
    }
  }

  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, validation ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &g_device, &level, &g_context);
  if (FAILED(hr) || !g_device || !g_context)
  {
    printf("FAIL: D3D11CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
    return 1;
  }
  printf("device: %s (Vulkan %s)\n", phxvk::Vk().props.deviceName, phxvk::Vk().features.geometryShader ? "with GS" : "no GS");
  if (validation) printf("validation layers: %s\n", phxvk::Vk().validationEnabled ? "VK_LAYER_KHRONOS_validation" : "NOT AVAILABLE");

  struct CaseEntry
  {
    int index;
    bool (*run)();
  };
  const CaseEntry cases[] = {
      {1, CaseQuad},        {2, CaseCbufferAndRenaming}, {3, CaseStrippedInput}, {4, CaseMrt},
      {5, CaseDepth},       {6, CaseTypelessDepthSrv},   {7, CaseGenerateMips},
  };
  for (auto &c : cases)
    if (runCase[c.index]) c.run();

  printf("\n================ summary ================\n");
  for (int i = 0; i < g_resultCount; i++)
    printf("  %-4s %s\n", g_results[i].ok ? "PASS" : "FAIL", g_results[i].name);

  g_context->ClearState();
  g_context->Release();
  g_device->Release();
  phxvk::vkShutdown();

  if (g_failures)
  {
    printf("\nFAIL: %d check(s) failed\n", g_failures);
    return 1;
  }
  printf("\nPASS: %d/%d cases rendered correctly on %s\n", g_resultCount, g_resultCount,
         phxvk::DeviceName());
  return 0;
}
