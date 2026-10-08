// compat/dxgi.h
//
// Portable stand-in for the Windows SDK <dxgi.h>.
//
// The apEx engine talks to the swap chain and the presentation layer through
// DXGI, so the descriptors and IDXGISwapChain below are the ones that matter
// most for the Vulkan port.  Everything else (adapters, factories, outputs) is
// declared only deep enough for the engine to hold pointers to it.
//
// Field order and enum values match the real SDK exactly; several vendored
// call sites rely on positional aggregate initialisation.
#pragma once

#include "Windows.h"
#include "dxgiformat.h"
#include "D3Dcommon.h"

// ---------------------------------------------------------------------------
// HRESULT status/error codes (winerror.h on Windows)
// ---------------------------------------------------------------------------

#define DXGI_STATUS_OCCLUDED                    ((HRESULT)0x087A0001L)
#define DXGI_STATUS_CLIPPED                     ((HRESULT)0x087A0002L)
#define DXGI_STATUS_NO_REDIRECTION              ((HRESULT)0x087A0004L)
#define DXGI_STATUS_NO_DESKTOP_ACCESS           ((HRESULT)0x087A0005L)
#define DXGI_STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE ((HRESULT)0x087A0006L)
#define DXGI_STATUS_MODE_CHANGED                ((HRESULT)0x087A0007L)
#define DXGI_STATUS_MODE_CHANGE_IN_PROGRESS     ((HRESULT)0x087A0008L)

#define DXGI_ERROR_INVALID_CALL                 ((HRESULT)0x887A0001L)
#define DXGI_ERROR_NOT_FOUND                    ((HRESULT)0x887A0002L)
#define DXGI_ERROR_MORE_DATA                    ((HRESULT)0x887A0003L)
#define DXGI_ERROR_UNSUPPORTED                  ((HRESULT)0x887A0004L)
#define DXGI_ERROR_DEVICE_REMOVED               ((HRESULT)0x887A0005L)
#define DXGI_ERROR_DEVICE_HUNG                  ((HRESULT)0x887A0006L)
#define DXGI_ERROR_DEVICE_RESET                 ((HRESULT)0x887A0007L)
#define DXGI_ERROR_WAS_STILL_DRAWING            ((HRESULT)0x887A000AL)
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT    ((HRESULT)0x887A000BL)
#define DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE ((HRESULT)0x887A000CL)
#define DXGI_ERROR_DRIVER_INTERNAL_ERROR        ((HRESULT)0x887A0020L)
#define DXGI_ERROR_NONEXCLUSIVE                 ((HRESULT)0x887A0021L)
#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE      ((HRESULT)0x887A0022L)
#define DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED   ((HRESULT)0x887A0023L)
#define DXGI_ERROR_REMOTE_OUTOFMEMORY           ((HRESULT)0x887A0024L)
#define DXGI_ERROR_MODE_CHANGE_IN_PROGRESS      ((HRESULT)0x887A0025L)
#define DXGI_ERROR_ACCESS_LOST                  ((HRESULT)0x887A0026L)
#define DXGI_ERROR_WAIT_TIMEOUT                 ((HRESULT)0x887A0027L)
#define DXGI_ERROR_SESSION_DISCONNECTED         ((HRESULT)0x887A0028L)
#define DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE     ((HRESULT)0x887A0029L)
#define DXGI_ERROR_CANNOT_PROTECT_CONTENT       ((HRESULT)0x887A002AL)
#define DXGI_ERROR_ACCESS_DENIED                ((HRESULT)0x887A002BL)
#define DXGI_ERROR_NAME_ALREADY_EXISTS          ((HRESULT)0x887A002CL)
#define DXGI_ERROR_SDK_COMPONENT_MISSING        ((HRESULT)0x887A002DL)
#define DXGI_ERROR_NOT_CURRENT                  ((HRESULT)0x887A002EL)
#define DXGI_ERROR_HW_PROTECTION_OUTOFMEMORY    ((HRESULT)0x887A0030L)

// ---------------------------------------------------------------------------
// Misc DXGI constants / flags
// ---------------------------------------------------------------------------

#define DXGI_MAX_SWAP_CHAIN_BUFFERS 16

#define DXGI_PRESENT_TEST                   0x00000001UL
#define DXGI_PRESENT_DO_NOT_SEQUENCE        0x00000002UL
#define DXGI_PRESENT_RESTART                0x00000004UL
#define DXGI_PRESENT_DO_NOT_WAIT            0x00000008UL
#define DXGI_PRESENT_STEREO_PREFER_RIGHT    0x00000010UL
#define DXGI_PRESENT_STEREO_TEMPORARY_MONO  0x00000020UL
#define DXGI_PRESENT_RESTRICT_TO_OUTPUT     0x00000040UL
#define DXGI_PRESENT_USE_DURATION           0x00000100UL
#define DXGI_PRESENT_ALLOW_TEARING          0x00000200UL

#define DXGI_ADAPTER_FLAG_NONE      0
#define DXGI_ADAPTER_FLAG_REMOTE    1
#define DXGI_ADAPTER_FLAG_SOFTWARE  2

#define DXGI_ENUM_MODES_INTERLACED  0x00000001UL
#define DXGI_ENUM_MODES_SCALING     0x00000002UL

#define DXGI_USAGE_SHADER_INPUT            0x00000010UL
#define DXGI_USAGE_RENDER_TARGET_OUTPUT    0x00000020UL
#define DXGI_USAGE_BACK_BUFFER             0x00000040UL
#define DXGI_USAGE_SHARED                  0x00000080UL
#define DXGI_USAGE_READ_ONLY               0x00000100UL
#define DXGI_USAGE_DISCARD_ON_PRESENT      0x00000200UL
#define DXGI_USAGE_UNORDERED_ACCESS        0x00000400UL

typedef UINT DXGI_USAGE;

// winnt.h owns LUID on Windows; compat/Windows.h does not provide it.
#ifndef _DSH_LUID_DEFINED
#define _DSH_LUID_DEFINED
typedef struct _LUID
{
    DWORD LowPart;
    LONG  HighPart;
} LUID, *PLUID;
#endif

// D3D11_SDK_VERSION is requested through dxgi.h by several call sites even
// though it lives in d3d11.h on Windows; define it here (guarded) so both
// headers agree.
#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

typedef enum DXGI_SWAP_EFFECT
{
    DXGI_SWAP_EFFECT_DISCARD         = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL      = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD    = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_SWAP_CHAIN_FLAG
{
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT = 8,
    DXGI_SWAP_CHAIN_FLAG_SHARE_RESOURCE = 16,
    DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO = 32,
    DXGI_SWAP_CHAIN_FLAG_DISCARD_BACKING_BUFFER = 64,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 128,
    DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER = 256,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 512
} DXGI_SWAP_CHAIN_FLAG;

typedef enum DXGI_MODE_SCANLINE_ORDER
{
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED        = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE        = 1,
    DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST  = 2,
    DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST  = 3
} DXGI_MODE_SCANLINE_ORDER;

typedef enum DXGI_MODE_SCALING
{
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED    = 1,
    DXGI_MODE_SCALING_STRETCHED   = 2
} DXGI_MODE_SCALING;

typedef enum DXGI_RESIDENCY
{
    DXGI_RESIDENCY_FULLY_RESIDENT = 1,
    DXGI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY = 2,
    DXGI_RESIDENCY_EVICTED_TO_DISK = 3
} DXGI_RESIDENCY;

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

typedef struct DXGI_RATIONAL
{
    UINT Numerator;
    UINT Denominator;
} DXGI_RATIONAL;

typedef struct DXGI_SAMPLE_DESC
{
    UINT Count;
    UINT Quality;
} DXGI_SAMPLE_DESC;

typedef struct DXGI_MODE_DESC
{
    UINT                     Width;
    UINT                     Height;
    DXGI_RATIONAL            RefreshRate;
    DXGI_FORMAT              Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING        Scaling;
} DXGI_MODE_DESC;

// DXGI_SWAP_CHAIN_DESC: field order is load-bearing - DX11Device.cpp memsets it
// and then assigns members by name, and DDSTextureLoader-era code initialises it
// positionally.
typedef struct DXGI_SWAP_CHAIN_DESC
{
    DXGI_MODE_DESC   BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE       BufferUsage;
    UINT             BufferCount;
    HWND             OutputWindow;
    BOOL             Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT             Flags;
} DXGI_SWAP_CHAIN_DESC;

typedef struct DXGI_FRAME_STATISTICS
{
    UINT         PresentCount;
    UINT         PresentRefreshCount;
    UINT         SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_FRAME_STATISTICS;

typedef struct DXGI_ADAPTER_DESC
{
    WCHAR         Description[128];
    UINT          VendorId;
    UINT          DeviceId;
    UINT          SubSysId;
    UINT          Revision;
    SIZE_T        DedicatedVideoMemory;
    SIZE_T        DedicatedSystemMemory;
    SIZE_T        SharedSystemMemory;
    LUID          AdapterLuid;
} DXGI_ADAPTER_DESC;

// ---------------------------------------------------------------------------
// Interfaces
//
// The real DXGI hierarchy is IUnknown -> IDXGIObject -> {IDXGIAdapter,
// IDXGIFactory, IDXGIDevice, ...} with IDXGISwapChain hanging off
// IDXGIDeviceSubObject.  Only the methods the engine actually calls are
// declared, so the mini-DXVK implementation only has to provide those.
// ---------------------------------------------------------------------------

struct IDXGIObject : public IUnknown
{
};

struct IDXGIAdapter : public IDXGIObject
{
};

struct IDXGIAdapter1 : public IDXGIAdapter
{
};

struct IDXGIFactory : public IDXGIObject
{
};

struct IDXGIFactory1 : public IDXGIFactory
{
};

struct IDXGIDevice : public IDXGIObject
{
};

struct IDXGIDeviceSubObject : public IDXGIObject
{
};

struct IDXGIOutput : public IDXGIObject
{
};

struct IDXGISwapChain : public IDXGIDeviceSubObject
{
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) = 0;
};

// ---------------------------------------------------------------------------
// IIDs
//
// Real Windows IID values, exposed as function-local statics so that the
// __uuidof macro in D3Dcommon.h can hand out references to them from a header
// without any ODR/linkage concerns.
// ---------------------------------------------------------------------------

namespace dsh_iid
{
inline const GUID &id_IDXGIObject()
{
    static const GUID g = { 0xaec22fb8, 0x76f3, 0x4639, { 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e } };
    return g;
}
inline const GUID &id_IDXGIAdapter()
{
    static const GUID g = { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0 } };
    return g;
}
inline const GUID &id_IDXGIAdapter1()
{
    static const GUID g = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
    return g;
}
inline const GUID &id_IDXGIFactory()
{
    static const GUID g = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
    return g;
}
inline const GUID &id_IDXGIDevice()
{
    static const GUID g = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
    return g;
}
inline const GUID &id_IDXGIDeviceSubObject()
{
    static const GUID g = { 0x3d3e0379, 0xf9de, 0x4d58, { 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6 } };
    return g;
}
inline const GUID &id_IDXGIOutput()
{
    static const GUID g = { 0xae02eedb, 0xc735, 0x4690, { 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa } };
    return g;
}
inline const GUID &id_IDXGISwapChain()
{
    static const GUID g = { 0x310d36a0, 0xd2e7, 0x4c0a, { 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a } };
    return g;
}
} // namespace dsh_iid
