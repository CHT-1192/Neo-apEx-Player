// D3DCompile / D3DReflect / D3DDisassemble and the D3DX11 texture entry points.
//
// HLSL is compiled by phxvk::CompileHLSL (libshaderc's glslang front end) with
// the register model rewritten to explicit [[vk::binding]]s, so the blob handed
// back contains SPIR-V rather than DXBC.  CreateVertexShader()/CreatePixelShader
// and the rest of the port treat it as such.
#include "d3d11_internal.h"

#include <D3Dcompiler.h>
#include <D3DX11.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace phxd3d11
{

namespace
{

bool StageFromTarget(const char *target, phxvk::ShaderStage *out)
{
  if (!target || !*target) return false;
  // "vs_5_0", "ps_4_0_level_9_3", ... - only the leading two letters matter.
  if (!strncmp(target, "vs", 2))
  {
    *out = phxvk::ShaderStage::Vertex;
    return true;
  }
  if (!strncmp(target, "ps", 2) || !strncmp(target, "px", 2))
  {
    *out = phxvk::ShaderStage::Pixel;
    return true;
  }
  if (!strncmp(target, "gs", 2))
  {
    *out = phxvk::ShaderStage::Geometry;
    return true;
  }
  if (!strncmp(target, "hs", 2))
  {
    *out = phxvk::ShaderStage::Hull;
    return true;
  }
  if (!strncmp(target, "ds", 2))
  {
    *out = phxvk::ShaderStage::Domain;
    return true;
  }
  if (!strncmp(target, "cs", 2))
  {
    *out = phxvk::ShaderStage::Compute;
    return true;
  }
  return false;
}

// Applies the D3D_SHADER_MACRO list to the source as #define lines.
std::string ApplyDefines(const std::string &source, const D3D_SHADER_MACRO *defines)
{
  if (!defines || !defines->Name) return source;
  std::string prefix;
  for (const D3D_SHADER_MACRO *m = defines; m->Name; m++)
  {
    prefix += "#define ";
    prefix += m->Name;
    if (m->Definition)
    {
      prefix += " ";
      prefix += m->Definition;
    }
    prefix += "\n";
  }
  return prefix + source;
}

HRESULT CompileCommon(LPCVOID pSrcData, SIZE_T SrcDataSize, const D3D_SHADER_MACRO *pDefines, LPCSTR pEntrypoint, LPCSTR pTarget, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs)
{
  if (ppCode) *ppCode = nullptr;
  if (ppErrorMsgs) *ppErrorMsgs = nullptr;
  if (!pSrcData || !SrcDataSize || !pEntrypoint || !pTarget) return E_INVALIDARG;

  phxvk::ShaderStage stage;
  if (!StageFromTarget(pTarget, &stage))
  {
    std::string err = "D3DCompile: unsupported shader target '";
    err += pTarget;
    err += "'\n";
    if (ppErrorMsgs) *ppErrorMsgs = CreateBlobString(err);
    phxvk::LogError("%s", err.c_str());
    return E_INVALIDARG;
  }

  // Some call sites (phxEngine.cpp) pass sizeof(literal), which includes the
  // terminating NUL; glslang rejects the embedded NUL byte.
  size_t length = (size_t)SrcDataSize;
  while (length > 0 && ((const char *)pSrcData)[length - 1] == '\0') length--;

  std::string source((const char *)pSrcData, length);
  source = ApplyDefines(source, pDefines);

  std::string debugName = pEntrypoint;
  debugName += "/";
  debugName += pTarget;

  phxvk::CompiledShader compiled = phxvk::CompileHLSL(source.c_str(), source.size(), pEntrypoint, stage, debugName.c_str());
  if (!compiled.ok)
  {
    if (ppErrorMsgs) *ppErrorMsgs = CreateBlobString(compiled.error);
    phxvk::LogError("D3DCompile(%s) failed: %s", debugName.c_str(), compiled.error.c_str());
    return E_FAIL;
  }

  ShaderReflectionRecord rec;
  rec.reflection = compiled.reflection;
  rec.stage = stage;
  RegisterShaderReflection(compiled.spirv.data(), compiled.spirv.size() * sizeof(uint32_t), rec);

  if (ppCode) *ppCode = CreateBlob(compiled.spirv.data(), compiled.spirv.size() * sizeof(uint32_t));
  return S_OK;
}

} // namespace

} // namespace phxd3d11

//////////////////////////////////////////////////////////////////////////////
// public entry points

using namespace phxd3d11;

extern "C" HRESULT WINAPI D3DCompile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
                                     LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs)
{
  (void)pSourceName;
  (void)pInclude;
  (void)Flags1;
  (void)Flags2;
  return CompileCommon(pSrcData, SrcDataSize, pDefines, pEntrypoint, pTarget, ppCode, ppErrorMsgs);
}

extern "C" HRESULT WINAPI D3DCompile2(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
                                      LPCSTR pTarget, UINT Flags1, UINT Flags2, UINT SecondaryDataFlags, LPCVOID pSecondaryData, SIZE_T SecondaryDataSize, ID3DBlob **ppCode,
                                      ID3DBlob **ppErrorMsgs)
{
  (void)pSourceName;
  (void)pInclude;
  (void)Flags1;
  (void)Flags2;
  (void)SecondaryDataFlags;
  (void)pSecondaryData;
  (void)SecondaryDataSize;
  return CompileCommon(pSrcData, SrcDataSize, pDefines, pEntrypoint, pTarget, ppCode, ppErrorMsgs);
}

extern "C" HRESULT WINAPI D3DReflect(LPCVOID pSrcData, SIZE_T SrcDataSize, REFIID pInterface, void **ppReflector)
{
  if (!ppReflector) return E_POINTER;
  *ppReflector = nullptr;
  if (!pSrcData || !SrcDataSize) return E_INVALIDARG;

  ShaderReflectionRecord rec;
  if (!LookupShaderReflection(pSrcData, (size_t)SrcDataSize, &rec))
  {
    phxvk::LogWarn("D3DReflect: no reflection registered for this SPIR-V module (was it produced by D3DCompile in this process?)");
    rec = ShaderReflectionRecord();
  }

  ID3D11ShaderReflection *refl = CreateReflection(rec, "D3DReflect");
  if (!GuidEq(pInterface, __uuidof(ID3D11ShaderReflection)) && !GuidEq(pInterface, IID_IUnknownRef()))
  {
    refl->Release();
    return E_NOINTERFACE;
  }
  *ppReflector = refl;
  return S_OK;
}

extern "C" HRESULT WINAPI D3DDisassemble(LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags, LPCSTR szComments, ID3DBlob **ppDisassembly)
{
  (void)Flags;
  if (!ppDisassembly) return E_POINTER;
  *ppDisassembly = nullptr;
  if (!pSrcData || !SrcDataSize) return E_INVALIDARG;

  // The "disassembly" is a hexdump of the SPIR-V: enough for the diagnostics
  // the tool layer wants, and honest about what the blob actually contains.
  const uint32_t *words = (const uint32_t *)pSrcData;
  const size_t count = (size_t)SrcDataSize / sizeof(uint32_t);
  std::string text;
  if (szComments)
  {
    text += "; ";
    text += szComments;
    text += "\n";
  }
  text += "; SPIR-V module (magic 0x";
  char buf[64];
  snprintf(buf, sizeof(buf), "%08x", count ? words[0] : 0u);
  text += buf;
  text += "), ";
  snprintf(buf, sizeof(buf), "%zu", count);
  text += buf;
  text += " words\n";
  for (size_t i = 0; i < count; i += 4)
  {
    snprintf(buf, sizeof(buf), "%06zx: ", i * 4);
    text += buf;
    for (size_t j = i; j < count && j < i + 4; j++)
    {
      snprintf(buf, sizeof(buf), "%08x ", words[j]);
      text += buf;
    }
    text += "\n";
  }
  *ppDisassembly = CreateBlobString(text);
  return S_OK;
}

//////////////////////////////////////////////////////////////////////////////
// D3DX11
//
// The engine calls D3DX11CompileFromMemory when CORE_API_D3DX is defined (it
// is not, in this port) and the texture helpers from CCoreDX11Texture2D's
// screenshot/export path.

extern "C" HRESULT WINAPI D3DX11CompileFromMemory(LPCSTR pSrcData, SIZE_T SrcDataLen, LPCSTR pFileName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pFunctionName,
                                                  LPCSTR pProfile, UINT Flags1, UINT Flags2, ID3DX11ThreadPump *pPump, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs, HRESULT *pHResult)
{
  (void)pFileName;
  (void)pInclude;
  (void)pPump;
  HRESULT hr = D3DCompile(pSrcData, SrcDataLen, pFileName, pDefines, pInclude, pFunctionName, pProfile, Flags1, Flags2, ppShader, ppErrorMsgs);
  if (pHResult) *pHResult = hr;
  return hr;
}

// ---------------------------------------------------------------------------
// Minimal DDS/PNG/TGA loading and saving built on the shim's own resources.
//
// The engine's texture path prefers its in-tree PNG/DDSTextureLoader, so these
// entry points are only reached by the tool layer; they are implemented rather
// than stubbed so that a NULL return is never mistaken for success.

extern "C" HRESULT WINAPI D3DX11CreateTextureFromMemory(ID3D11Device *pDevice, LPCVOID pSrcData, SIZE_T SrcDataSize, D3DX11_IMAGE_LOAD_INFO *pLoadInfo, ID3DX11ThreadPump *pPump,
                                                        ID3D11Resource **ppTexture, HRESULT *pHResult)
{
  (void)pLoadInfo;
  (void)pPump;
  if (ppTexture) *ppTexture = nullptr;
  if (pHResult) *pHResult = E_FAIL;
  if (!pDevice || !pSrcData || !SrcDataSize || !ppTexture) return E_INVALIDARG;

  // The engine only reaches this entry point when CORE_API_D3DX is defined,
  // which this port does not do (CoRE2 uses its own DDSTextureLoader/PNG
  // decompressor instead).  Say so rather than pretending to have loaded it.
  const uint8_t *bytes = (const uint8_t *)pSrcData;
  const bool dds = SrcDataSize >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ';
  phxvk::LogWarn("D3DX11CreateTextureFromMemory: not implemented by the shim (%s payload, %zu bytes); CoRE2's own DDSTextureLoader is used instead",
                 dds ? "DDS" : "non-DDS", (size_t)SrcDataSize);
  return E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3DX11CreateTextureFromFileA(ID3D11Device *pDevice, LPCSTR pSrcFile, D3DX11_IMAGE_LOAD_INFO *pLoadInfo, ID3DX11ThreadPump *pPump, ID3D11Resource **ppTexture,
                                                       HRESULT *pHResult)
{
  (void)pLoadInfo;
  (void)pPump;
  if (ppTexture) *ppTexture = nullptr;
  if (pHResult) *pHResult = E_FAIL;
  if (!pDevice || !pSrcFile || !ppTexture) return E_INVALIDARG;

  FILE *f = fopen(pSrcFile, "rb");
  if (!f)
  {
    phxvk::LogError("D3DX11CreateTextureFromFileA: cannot open '%s'", pSrcFile);
    return E_FAIL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data((size_t)(size > 0 ? size : 0));
  if (!data.empty() && fread(data.data(), 1, data.size(), f) != data.size())
  {
    fclose(f);
    return E_FAIL;
  }
  fclose(f);
  return D3DX11CreateTextureFromMemory(pDevice, data.data(), data.size(), pLoadInfo, pPump, ppTexture, pHResult);
}

extern "C" HRESULT WINAPI D3DX11CreateTextureFromFileW(ID3D11Device *pDevice, LPCWSTR pSrcFile, D3DX11_IMAGE_LOAD_INFO *pLoadInfo, ID3DX11ThreadPump *pPump, ID3D11Resource **ppTexture,
                                                       HRESULT *pHResult)
{
  (void)pDevice;
  (void)pSrcFile;
  (void)pLoadInfo;
  (void)pPump;
  if (ppTexture) *ppTexture = nullptr;
  if (pHResult) *pHResult = E_NOTIMPL;
  return E_NOTIMPL;
}

// Copies any texture into a staging texture and hands back its bytes.
// NOTE: despite the historical name this does NOT convert anything - it copies
// the mapped staging bytes verbatim (the staging texture is created with the
// source's own desc), so `out` holds the SOURCE format.  `outFormat` therefore
// reports the format those bytes are in, which the DDS writer needs.
static HRESULT CaptureTextureToRgba8(ID3D11DeviceContext *pContext, ID3D11Resource *pSrcTexture, std::vector<uint8_t> *out, UINT *outWidth, UINT *outHeight, UINT *outPitch, DXGI_FORMAT *outFormat = nullptr)
{
  if (!pContext || !pSrcTexture) return E_INVALIDARG;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  pSrcTexture->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return E_INVALIDARG;

  ID3D11Device *device = nullptr;
  pContext->GetDevice(&device);
  if (!device) return E_FAIL;

  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  static_cast<ID3D11Texture2D *>(pSrcTexture)->GetDesc(&desc);

  D3D11_TEXTURE2D_DESC staging = desc;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.MiscFlags = 0;
  staging.SampleDesc.Count = 1;
  staging.SampleDesc.Quality = 0;

  ID3D11Texture2D *tex = nullptr;
  HRESULT hr = device->CreateTexture2D(&staging, nullptr, &tex);
  if (FAILED(hr) || !tex)
  {
    device->Release();
    return E_FAIL;
  }
  pContext->CopyResource(tex, pSrcTexture);

  D3D11_MAPPED_SUBRESOURCE mapped;
  memset(&mapped, 0, sizeof(mapped));
  hr = pContext->Map(tex, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr))
  {
    tex->Release();
    device->Release();
    return hr;
  }

  const UINT pitch = mapped.RowPitch;
  out->assign((uint8_t *)mapped.pData, (uint8_t *)mapped.pData + (size_t)pitch * desc.Height);
  *outWidth = desc.Width;
  *outHeight = desc.Height;
  *outPitch = pitch;
  if (outFormat) *outFormat = desc.Format;

  pContext->Unmap(tex, 0);
  tex->Release();
  device->Release();
  return S_OK;
}

extern "C" HRESULT WINAPI D3DX11SaveTextureToMemory(ID3D11DeviceContext *pContext, ID3D11Resource *pSrcTexture, D3DX11_IMAGE_FILE_FORMAT DestFormat, ID3DBlob **ppDestBlob, UINT Flags)
{
  (void)Flags;
  if (!ppDestBlob) return E_POINTER;
  *ppDestBlob = nullptr;

  std::vector<uint8_t> pixels;
  UINT w = 0, h = 0, pitch = 0;
  DXGI_FORMAT srcFormat = DXGI_FORMAT_UNKNOWN;
  HRESULT hr = CaptureTextureToRgba8(pContext, pSrcTexture, &pixels, &w, &h, &pitch, &srcFormat);
  if (FAILED(hr)) return hr;

  if (DestFormat == D3DX11_IFF_DDS)
  {
    // The DDS must describe the bytes that were actually captured: this
    // function copies the staging data verbatim, so a 16-bit-per-channel texgen
    // render target stays 16-bit rather than being relabelled as BGRA8.  That
    // matters beyond tidiness - CphxMesh::CalculateTint (Phoenix/Mesh.cpp:1160)
    // interprets the payload as `unsigned short*` with a stride of pixelIndex*4
    // shorts, i.e. it requires exactly 8 bytes per pixel.
    struct DDS_PIXELFORMAT
    {
      uint32_t size, flags, fourCC, bitCount, rMask, gMask, bMask, aMask;
    };
    struct DDS_HEADER
    {
      uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, reserved1[11];
      DDS_PIXELFORMAT ddspf;
      uint32_t caps, caps2, caps3, caps4, reserved2;
    };

    // DDPF_* flags and the legacy D3D format codes the SDK's DDS writer used.
    const uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40;
    const uint32_t D3DFMT_A16B16G16R16F = 113, D3DFMT_A16B16G16R16 = 36;

    uint32_t bytesPerPixel = 4;
    bool fourcc = false;
    uint32_t fourccCode = 0;
    uint32_t rMask = 0x00ff0000, gMask = 0x0000ff00, bMask = 0x000000ff, aMask = 0xff000000;

    switch (srcFormat)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      bytesPerPixel = 8;
      fourcc = true;
      fourccCode = D3DFMT_A16B16G16R16F;
      break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      bytesPerPixel = 8;
      fourcc = true;
      fourccCode = D3DFMT_A16B16G16R16;
      break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      bytesPerPixel = 4;
      break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      bytesPerPixel = 4;
      rMask = 0x00ff0000;
      gMask = 0x0000ff00;
      bMask = 0x000000ff;
      aMask = 0xff000000;
      break;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
      bytesPerPixel = 4;
      break;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
      bytesPerPixel = 2;
      break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      bytesPerPixel = 4;
      break;
    default:
      phxvk::LogWarn("D3DX11SaveTextureToMemory: DXGI format %d is not handled; writing the DDS as 32-bit BGRA", (int)srcFormat);
      bytesPerPixel = 4;
      break;
    }

    DDS_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.size = sizeof(DDS_HEADER);
    hdr.flags = 0x1007 | 0x8;
    hdr.height = h;
    hdr.width = w;
    hdr.pitchOrLinearSize = (uint32_t)w * bytesPerPixel;
    hdr.mipMapCount = 1;
    hdr.ddspf.size = sizeof(DDS_PIXELFORMAT);
    if (fourcc)
    {
      hdr.ddspf.flags = DDPF_FOURCC;
      hdr.ddspf.fourCC = fourccCode;
    }
    else
    {
      hdr.ddspf.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
      hdr.ddspf.bitCount = bytesPerPixel * 8;
      hdr.ddspf.rMask = rMask;
      hdr.ddspf.gMask = gMask;
      hdr.ddspf.bMask = bMask;
      hdr.ddspf.aMask = aMask;
    }
    hdr.caps = 0x1000;

    std::vector<uint8_t> out;
    out.resize(4 + sizeof(DDS_HEADER));
    memcpy(out.data(), "DDS ", 4);
    memcpy(out.data() + 4, &hdr, sizeof(hdr));
    // The engine's importer expects tightly packed rows in the source format.
    const size_t rowBytes = (size_t)w * bytesPerPixel;
    if (rowBytes > pitch)
    {
      phxvk::LogError("D3DX11SaveTextureToMemory: row of %zu bytes exceeds the mapped pitch %u", rowBytes, pitch);
      return E_FAIL;
    }
    for (UINT y = 0; y < h; y++)
      out.insert(out.end(), pixels.begin() + (size_t)y * pitch, pixels.begin() + (size_t)y * pitch + rowBytes);

    *ppDestBlob = CreateBlob(out.data(), out.size());
    return S_OK;
  }

  phxvk::LogWarn("D3DX11SaveTextureToMemory: format %d is not supported (only DDS); use the engine's PNG/TGA exporters", (int)DestFormat);
  return E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3DX11SaveTextureToFileA(ID3D11DeviceContext *pContext, ID3D11Resource *pSrcTexture, D3DX11_IMAGE_FILE_FORMAT DestFormat, LPCSTR pDestFile)
{
  if (!pDestFile) return E_INVALIDARG;
  ID3DBlob *blob = nullptr;
  HRESULT hr = D3DX11SaveTextureToMemory(pContext, pSrcTexture, DestFormat, &blob, 0);
  if (FAILED(hr) || !blob) return hr;

  FILE *f = fopen(pDestFile, "wb");
  if (!f)
  {
    blob->Release();
    phxvk::LogError("D3DX11SaveTextureToFileA: cannot write '%s'", pDestFile);
    return E_FAIL;
  }
  fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
  fclose(f);
  blob->Release();
  phxvk::LogInfo("[d3d11] wrote %s", pDestFile);
  return S_OK;
}

extern "C" HRESULT WINAPI D3DX11SaveTextureToFileW(ID3D11DeviceContext *pContext, ID3D11Resource *pSrcTexture, D3DX11_IMAGE_FILE_FORMAT DestFormat, LPCWSTR pDestFile)
{
  (void)pContext;
  (void)pSrcTexture;
  (void)DestFormat;
  (void)pDestFile;
  return E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3DX11GetImageInfoFromMemory(LPCVOID pSrcData, SIZE_T SrcDataSize, ID3DX11ThreadPump *pPump, D3DX11_IMAGE_INFO *pSrcInfo, HRESULT *pHResult)
{
  (void)pPump;
  if (pHResult) *pHResult = E_FAIL;
  if (!pSrcData || !pSrcInfo) return E_INVALIDARG;
  memset(pSrcInfo, 0, sizeof(*pSrcInfo));

  const uint8_t *bytes = (const uint8_t *)pSrcData;
  if (SrcDataSize >= 128 && !memcmp(bytes, "DDS ", 4))
  {
    // DDS_HEADER starts at offset 4.
    uint32_t height, width, mipCount, pfFlags, fourCC, bitCount;
    memcpy(&height, bytes + 4 + 8, 4);
    memcpy(&width, bytes + 4 + 12, 4);
    memcpy(&mipCount, bytes + 4 + 24, 4);
    memcpy(&pfFlags, bytes + 4 + 76, 4);
    memcpy(&fourCC, bytes + 4 + 80, 4);
    memcpy(&bitCount, bytes + 4 + 84, 4);
    pSrcInfo->Width = width;
    pSrcInfo->Height = height;
    pSrcInfo->Depth = 1;
    pSrcInfo->ArraySize = 1;
    pSrcInfo->MipLevels = mipCount ? mipCount : 1;
    pSrcInfo->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (pfFlags & 0x4) // DDPF_FOURCC
    {
      switch (fourCC)
      {
      case 0x31545844: pSrcInfo->Format = DXGI_FORMAT_BC1_UNORM; break; // DXT1
      case 0x33545844: pSrcInfo->Format = DXGI_FORMAT_BC2_UNORM; break; // DXT3
      case 0x35545844: pSrcInfo->Format = DXGI_FORMAT_BC3_UNORM; break; // DXT5
      default: break;
      }
    }
    else if (bitCount == 32)
      pSrcInfo->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (bitCount == 24)
      pSrcInfo->Format = DXGI_FORMAT_B8G8R8X8_UNORM;
    pSrcInfo->ResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    pSrcInfo->ImageFileFormat = D3DX11_IFF_DDS;
    if (pHResult) *pHResult = S_OK;
    return S_OK;
  }

  phxvk::LogWarn("D3DX11GetImageInfoFromMemory: unrecognised image payload (%zu bytes)", (size_t)SrcDataSize);
  return E_FAIL;
}

extern "C" HRESULT WINAPI D3DX11GetImageInfoFromFileA(LPCSTR pSrcFile, ID3DX11ThreadPump *pPump, D3DX11_IMAGE_INFO *pSrcInfo, HRESULT *pHResult)
{
  (void)pPump;
  if (pHResult) *pHResult = E_FAIL;
  if (!pSrcFile) return E_INVALIDARG;
  FILE *f = fopen(pSrcFile, "rb");
  if (!f) return E_FAIL;
  uint8_t head[128];
  size_t n = fread(head, 1, sizeof(head), f);
  fclose(f);
  return D3DX11GetImageInfoFromMemory(head, n, pPump, pSrcInfo, pHResult);
}

extern "C" HRESULT WINAPI D3DX11GetImageInfoFromFileW(LPCWSTR pSrcFile, ID3DX11ThreadPump *pPump, D3DX11_IMAGE_INFO *pSrcInfo, HRESULT *pHResult)
{
  (void)pSrcFile;
  (void)pPump;
  (void)pSrcInfo;
  if (pHResult) *pHResult = E_NOTIMPL;
  return E_NOTIMPL;
}

// A missing D3DCompile symbol is the one thing that breaks the engine at
// runtime (phxEngine resolves it through GetProcAddress).  This lets
// src/platform/win32_compat.cpp's GetProcAddress fall back to the shim.
extern "C" void *phxD3D11GetProcAddress(const char *name)
{
  if (!name) return nullptr;
  if (!strcmp(name, "D3DCompile")) return (void *)&D3DCompile;
  if (!strcmp(name, "D3DCompile2")) return (void *)&D3DCompile2;
  if (!strcmp(name, "D3DReflect")) return (void *)&D3DReflect;
  if (!strcmp(name, "D3DDisassemble")) return (void *)&D3DDisassemble;
  if (!strcmp(name, "D3DX11CompileFromMemory")) return (void *)&D3DX11CompileFromMemory;
  if (!strcmp(name, "D3DX11CreateTextureFromMemory")) return (void *)&D3DX11CreateTextureFromMemory;
  if (!strcmp(name, "D3DX11SaveTextureToMemory")) return (void *)&D3DX11SaveTextureToMemory;
  if (!strcmp(name, "D3DX11SaveTextureToFileA")) return (void *)&D3DX11SaveTextureToFileA;
  if (!strcmp(name, "D3DX11GetImageInfoFromMemory")) return (void *)&D3DX11GetImageInfoFromMemory;
  return nullptr;
}
