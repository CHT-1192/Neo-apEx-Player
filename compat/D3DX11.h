// compat/D3DX11.h
//
// Portable stand-in for the DirectX SDK's <D3DX11.h> (D3DX11 core + texture
// helpers).
//
// The vendored engine uses D3DX11 in three places:
//   * CCoreTexture2D::Create(Data, Size)   -> D3DX11CreateTextureFromMemory
//   * CphxMesh::CalculateTint              -> D3DX11SaveTextureToMemory
//   * Phoenix_Tool screenshot export       -> D3DX11SaveTextureToFileA
// plus the D3DX11_IMAGE_LOAD_INFO / D3DX11_IMAGE_INFO descriptors and the
// D3DX11_IFF_* file-format enum.
//
// Declarations only.  On this port the implementations are expected from the
// DDS/PNG loader shims (stb_image / the existing DDSTextureLoader) behind these
// entry points.
#pragma once

#include "Windows.h"
#include "dxgiformat.h"
#include "D3Dcommon.h"
#include "D3Dcompiler.h"
#include "d3d11.h"

//////////////////////////////////////////////////////////////////////////////
// Image file formats
//
// NOTE: the numbering is the DirectX SDK's, gaps and all (2 and 5 are unused) -
// it is part of the public ABI of d3dx11.dll.
//////////////////////////////////////////////////////////////////////////////

typedef enum _D3DX11_IMAGE_FILE_FORMAT
{
    D3DX11_IFF_BMP = 0,
    D3DX11_IFF_JPG = 1,
    D3DX11_IFF_PNG = 3,
    D3DX11_IFF_DDS = 4,
    D3DX11_IFF_TIFF = 6,
    D3DX11_IFF_GIF = 7,
    D3DX11_IFF_WMP = 8,
    D3DX11_IFF_FORCE_DWORD = 0x7fffffff
} D3DX11_IMAGE_FILE_FORMAT;

//////////////////////////////////////////////////////////////////////////////
// D3DX11_IMAGE_LOAD_INFO / D3DX11_IMAGE_INFO
//////////////////////////////////////////////////////////////////////////////

typedef struct _D3DX11_IMAGE_LOAD_INFO
{
    UINT                     Width;
    UINT                     Height;
    UINT                     Depth;
    UINT                     FirstMipLevel;
    UINT                     MipLevels;
    D3DX11_IMAGE_FILE_FORMAT Format;
    UINT                     Filter;
    UINT                     MipFilter;
    UINT                     MiscFlags;
    UINT                     BindFlags;
    UINT                     CpuAccessFlags;
    UINT                     Usage;
    UINT                     FilterFlags;
} D3DX11_IMAGE_LOAD_INFO;

typedef struct _D3DX11_IMAGE_INFO
{
    UINT                      Width;
    UINT                      Height;
    UINT                      Depth;
    UINT                      ArraySize;
    UINT                      MipLevels;
    UINT                      MiscFlags;
    DXGI_FORMAT               Format;
    D3D11_RESOURCE_DIMENSION  ResourceDimension;
    D3DX11_IMAGE_FILE_FORMAT  ImageFileFormat;
} D3DX11_IMAGE_INFO;

//////////////////////////////////////////////////////////////////////////////
// Thread pump
//
// Only ever passed as NULL by the engine; kept opaque so that the D3DX11
// entry-point signatures match the SDK exactly.
//////////////////////////////////////////////////////////////////////////////

struct ID3DX11ThreadPump;

// D3DX11_FILTER_* / D3DX11_FILTER_FLAG_* (used when a load info asks for
// resampling); harmless to provide, the engine passes NULL load infos.
#define D3DX11_FILTER_NONE          (0)
#define D3DX11_FILTER_POINT         (D3DX11_FILTER_NONE + 1)
#define D3DX11_FILTER_LINEAR        (D3DX11_FILTER_NONE + 2)
#define D3DX11_FILTER_TRIANGLE      (D3DX11_FILTER_NONE + 3)
#define D3DX11_FILTER_BOX           (D3DX11_FILTER_NONE + 4)
#define D3DX11_FILTER_MIRROR_U      (0x00010000)
#define D3DX11_FILTER_MIRROR_V      (0x00020000)
#define D3DX11_FILTER_MIRROR_W      (0x00040000)
#define D3DX11_FILTER_MIRROR        (D3DX11_FILTER_MIRROR_U | D3DX11_FILTER_MIRROR_V | D3DX11_FILTER_MIRROR_W)
#define D3DX11_FILTER_DITHER        (0x00080000)
#define D3DX11_FILTER_DITHER_DIFFUSION (0x00100000)
#define D3DX11_FILTER_SRGB_IN       (0x00200000)
#define D3DX11_FILTER_SRGB_OUT      (0x00400000)
#define D3DX11_FILTER_SRGB          (D3DX11_FILTER_SRGB_IN | D3DX11_FILTER_SRGB_OUT)

#define D3DX11_FILTER_FLAG_NONE             (0)
#define D3DX11_FILTER_FLAG_POINT            (1 << 0)
#define D3DX11_FILTER_FLAG_LINEAR           (1 << 1)
#define D3DX11_FILTER_FLAG_TRIANGLE         (1 << 2)
#define D3DX11_FILTER_FLAG_BOX              (1 << 3)
#define D3DX11_FILTER_FLAG_MIRROR           (1 << 4)
#define D3DX11_FILTER_FLAG_DITHER           (1 << 5)
#define D3DX11_FILTER_FLAG_SRGB_IN          (1 << 6)
#define D3DX11_FILTER_FLAG_SRGB_OUT         (1 << 7)
#define D3DX11_FILTER_FLAG_SRGB             (D3DX11_FILTER_FLAG_SRGB_IN | D3DX11_FILTER_FLAG_SRGB_OUT)

//////////////////////////////////////////////////////////////////////////////
// Entry points
//////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI D3DX11CreateTextureFromMemory(
    ID3D11Device *pDevice,
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    D3DX11_IMAGE_LOAD_INFO *pLoadInfo,
    ID3DX11ThreadPump *pPump,
    ID3D11Resource **ppTexture,
    HRESULT *pHResult);

HRESULT WINAPI D3DX11CreateTextureFromFileA(
    ID3D11Device *pDevice,
    LPCSTR pSrcFile,
    D3DX11_IMAGE_LOAD_INFO *pLoadInfo,
    ID3DX11ThreadPump *pPump,
    ID3D11Resource **ppTexture,
    HRESULT *pHResult);

HRESULT WINAPI D3DX11CreateTextureFromFileW(
    ID3D11Device *pDevice,
    LPCWSTR pSrcFile,
    D3DX11_IMAGE_LOAD_INFO *pLoadInfo,
    ID3DX11ThreadPump *pPump,
    ID3D11Resource **ppTexture,
    HRESULT *pHResult);

HRESULT WINAPI D3DX11SaveTextureToFileA(
    ID3D11DeviceContext *pContext,
    ID3D11Resource *pSrcTexture,
    D3DX11_IMAGE_FILE_FORMAT DestFormat,
    LPCSTR pDestFile);

HRESULT WINAPI D3DX11SaveTextureToFileW(
    ID3D11DeviceContext *pContext,
    ID3D11Resource *pSrcTexture,
    D3DX11_IMAGE_FILE_FORMAT DestFormat,
    LPCWSTR pDestFile);

HRESULT WINAPI D3DX11SaveTextureToMemory(
    ID3D11DeviceContext *pContext,
    ID3D11Resource *pSrcTexture,
    D3DX11_IMAGE_FILE_FORMAT DestFormat,
    ID3DBlob **ppDestBlob,
    UINT Flags);

HRESULT WINAPI D3DX11GetImageInfoFromMemory(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    ID3DX11ThreadPump *pPump,
    D3DX11_IMAGE_INFO *pSrcInfo,
    HRESULT *pHResult);

HRESULT WINAPI D3DX11GetImageInfoFromFileA(
    LPCSTR pSrcFile,
    ID3DX11ThreadPump *pPump,
    D3DX11_IMAGE_INFO *pSrcInfo,
    HRESULT *pHResult);

HRESULT WINAPI D3DX11GetImageInfoFromFileW(
    LPCWSTR pSrcFile,
    ID3DX11ThreadPump *pPump,
    D3DX11_IMAGE_INFO *pSrcInfo,
    HRESULT *pHResult);

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////
// ANSI/UNICODE aliases
//
// This port builds the engine without UNICODE (TCHAR == char), exactly like the
// original MSVC tool project, so the unsuffixed spellings must resolve to the
// "A" entry points - Phoenix_Tool/Texgen_tool.cpp calls
// D3DX11SaveTextureToFile(...) directly.
//////////////////////////////////////////////////////////////////////////////

#ifndef D3DX11CreateTextureFromFile
#define D3DX11CreateTextureFromFile D3DX11CreateTextureFromFileA
#endif
#ifndef D3DX11GetImageInfoFromFile
#define D3DX11GetImageInfoFromFile D3DX11GetImageInfoFromFileA
#endif
#ifndef D3DX11SaveTextureToFile
#define D3DX11SaveTextureToFile D3DX11SaveTextureToFileA
#endif
