// compat/d3d11.h
//
// Portable stand-in for the Windows SDK <d3d11.h>.
//
// This is the *declaration* layer only: no implementations live here (the
// mini-DXVK behind these interfaces is in src/d3d11/*.cpp).  Every enum value,
// struct field order and method signature is transcribed from the real
// Direct3D 11 SDK, because
//
//   * the vendored engine uses positional aggregate initialisation for
//     D3D11_BUFFER_DESC / D3D11_INPUT_ELEMENT_DESC / D3D11_SAMPLER_DESC /
//     D3D11_TEXTURE2D_DESC / the view descs, and
//   * it stores these values in files/shaders, so the numbering is part of the
//     format.
//
// Interfaces are declared in their real inheritance shape
// (IUnknown -> ID3D11DeviceChild -> ...).  Only the methods the vendored code
// actually calls are declared, plus the mandatory IUnknown trio, so the
// implementation side only has to provide what is referenced.
#pragma once

#include "Windows.h"
#include "dxgiformat.h"
#include "D3Dcommon.h"
#include "dxgi.h"

//////////////////////////////////////////////////////////////////////////////
// SDK include-guard / "already declared" markers
//
// The real <d3d11.h> defines __d3d11_h__, and the D3DX math headers use that
// plus D3D11_VIEWPORT_DEFINED to decide whether they still have to supply the
// viewport struct themselves.  compat/D3DX10Math.h is written defensively in
// exactly that style, so publish both markers here; without them the two
// headers each define D3D11_VIEWPORT and every TU that sees both fails with a
// redefinition error.
//////////////////////////////////////////////////////////////////////////////

#ifndef __d3d11_h__
#define __d3d11_h__ 1
#endif
#ifndef D3D11_VIEWPORT_DEFINED
#define D3D11_VIEWPORT_DEFINED
#endif

// D3D10_VIEWPORT lives in d3d10.h on Windows, but D3DX10Math.h's
// D3DXVec3Project/Unproject family takes const D3D10_VIEWPORT*, and its own
// fallback definition is suppressed once __d3d11_h__ is defined.  Provide it
// here (guarded) so both include orders work.
#ifndef D3D10_VIEWPORT_DEFINED
#define D3D10_VIEWPORT_DEFINED
typedef struct D3D10_VIEWPORT
{
    INT   TopLeftX;
    INT   TopLeftY;
    UINT  Width;
    UINT  Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D10_VIEWPORT;
#endif

//////////////////////////////////////////////////////////////////////////////
// D3D10 resource limits
//
// DDSTextureLoader.cpp clamps its mip chain with the D3D10_REQ_* constants when
// it retries a load for feature level 10.x.  They live in d3d10.h on Windows
// and the loader only includes <d3d11.h> (via Core2_Config.h), so they are
// mirrored here.  Values transcribed from the DirectX SDK's D3D10.h.
//////////////////////////////////////////////////////////////////////////////

#define D3D10_DEFAULT_BLEND_FACTOR_ALPHA        ( 1.0f )
#define D3D10_DEFAULT_BLEND_FACTOR_BLUE         ( 1.0f )
#define D3D10_DEFAULT_BLEND_FACTOR_GREEN        ( 1.0f )
#define D3D10_DEFAULT_BLEND_FACTOR_RED          ( 1.0f )
#define D3D10_DEFAULT_BORDER_COLOR_COMPONENT    ( 0.0f )
#define D3D10_DEFAULT_DEPTH_BIAS_CLAMP          ( 0.0f )
#define D3D10_DEFAULT_MAX_ANISOTROPY            ( 16.0f )
#define D3D10_DEFAULT_MIP_LOD_BIAS              ( 0.0f )
#define D3D10_DEFAULT_SLOPE_SCALED_DEPTH_BIAS   ( 0.0f )
#define D3D10_DEFAULT_VIEWPORT_MAX_DEPTH        ( 0.0f )
#define D3D10_DEFAULT_VIEWPORT_MIN_DEPTH        ( 0.0f )

#define D3D10_FLOAT32_MAX                       ( 3.402823466e+38f )
#define D3D10_MAX_BORDER_COLOR_COMPONENT        ( 1.0f )
#define D3D10_MAX_DEPTH                         ( 1.0f )
#define D3D10_MAX_POSITION_VALUE                ( 3.402823466e+34f )
#define D3D10_MIN_BORDER_COLOR_COMPONENT        ( 0.0f )
#define D3D10_MIN_DEPTH                         ( 0.0f )
#define D3D10_MIP_LOD_BIAS_MAX                  ( 15.99f )
#define D3D10_MIP_LOD_BIAS_MIN                  ( -16.0f )

#define D3D10_REQ_BLEND_OBJECT_COUNT_PER_CONTEXT                ( 4096 )
#define D3D10_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP          ( 27 )
#define D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT                 ( 4096 )
#define D3D10_REQ_DEPTH_STENCIL_OBJECT_COUNT_PER_CONTEXT        ( 4096 )
#define D3D10_REQ_DRAWINDEXED_INDEX_COUNT_2_TO_EXP              ( 32 )
#define D3D10_REQ_DRAW_VERTEX_COUNT_2_TO_EXP                    ( 32 )
#define D3D10_REQ_FILTERING_HW_ADDRESSABLE_RESOURCE_DIMENSION   ( 8192 )
#define D3D10_REQ_GS_INVOCATION_32BIT_OUTPUT_COMPONENT_LIMIT    ( 1024 )
#define D3D10_REQ_IMMEDIATE_CONSTANT_BUFFER_ELEMENT_COUNT       ( 4096 )
#define D3D10_REQ_MAXANISOTROPY                                 ( 16 )
#define D3D10_REQ_MIP_LEVELS                                    ( 14 )
#define D3D10_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES         ( 2048 )
#define D3D10_REQ_RASTERIZER_OBJECT_COUNT_PER_CONTEXT           ( 4096 )
#define D3D10_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH                 ( 8192 )
#define D3D10_REQ_RESOURCE_SIZE_IN_MEGABYTES                    ( 128 )
#define D3D10_REQ_RESOURCE_VIEW_COUNT_PER_CONTEXT_2_TO_EXP      ( 20 )
#define D3D10_REQ_SAMPLER_OBJECT_COUNT_PER_CONTEXT              ( 4096 )
#define D3D10_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION                ( 512 )
#define D3D10_REQ_TEXTURE1D_U_DIMENSION                         ( 8192 )
#define D3D10_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION                ( 512 )
#define D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION                    ( 8192 )
#define D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION                  ( 2048 )
#define D3D10_REQ_TEXTURECUBE_DIMENSION                         ( 8192 )

// D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT / D3D10_CLEAR_* are also d3d10.h
// citizens, and Phoenix's RenderLayer.cpp and Timeline.cpp use the D3D10
// spellings of the clear flags.
#define D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT                  ( 8 )
#define D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ( 16 )
#define D3D10_VIEWPORT_BOUNDS_MAX                               ( 32767 )
#define D3D10_VIEWPORT_BOUNDS_MIN                               ( -32768 )
#define D3D10_SHADER_MAJOR_VERSION                              ( 4 )
#define D3D10_SHADER_MINOR_VERSION                              ( 0 )

typedef enum D3D10_CLEAR_FLAG
{
    D3D10_CLEAR_DEPTH = 0x1L,
    D3D10_CLEAR_STENCIL = 0x2L
} D3D10_CLEAR_FLAG;

// D3D10_RESOURCE_DIMENSION / D3D10_RESOURCE_MISC_FLAG: DX11Texture.cpp keeps a
// small D3D10-shaped resource description next to its D3D11 one, and
// D3D10_STANDARD_MULTISAMPLE_PATTERN is the "let the driver choose" quality
// value it stores in D3D11_SAMPLE_DESC::Quality.  Same numbering as the D3D11
// spellings, as in the SDK.
typedef enum D3D10_RESOURCE_DIMENSION
{
    D3D10_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D10_RESOURCE_DIMENSION_BUFFER = 1,
    D3D10_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4
} D3D10_RESOURCE_DIMENSION;

typedef enum D3D10_RESOURCE_MISC_FLAG
{
    D3D10_RESOURCE_MISC_GENERATE_MIPS = 0x1L,
    D3D10_RESOURCE_MISC_SHARED = 0x2L,
    D3D10_RESOURCE_MISC_TEXTURECUBE = 0x4L,
    D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x10L,
    D3D10_RESOURCE_MISC_GDI_COMPATIBLE = 0x20L
} D3D10_RESOURCE_MISC_FLAG;

#ifndef D3D10_STANDARD_MULTISAMPLE_PATTERN
#define D3D10_STANDARD_MULTISAMPLE_PATTERN 0xffffffff
#endif
#ifndef D3D10_CENTER_MULTISAMPLE_PATTERN
#define D3D10_CENTER_MULTISAMPLE_PATTERN   0xfffffffe
#endif
#ifndef D3D10_APPEND_ALIGNED_ELEMENT
#define D3D10_APPEND_ALIGNED_ELEMENT 0xffffffff
#endif

// D3D11 equivalents (d3d11.h on Windows).
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT                  ( 8 )
#define D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX                ( 15 )
#define D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ( 16 )
#define D3D11_VIEWPORT_BOUNDS_MAX                               ( 32767 )
#define D3D11_VIEWPORT_BOUNDS_MIN                               ( -32768 )

//////////////////////////////////////////////////////////////////////////////
// SAL annotations
//
// The vendored DDS/mesh helpers are annotated with the Windows SDK's
// _In_/_Out_/_Inout_ macros.  sal.h is not present in compat/, so provide the
// subset that appears in the tree (and a few obvious neighbours).
//////////////////////////////////////////////////////////////////////////////

#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(x)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif
#ifndef _In_reads_bytes_opt_
#define _In_reads_bytes_opt_(x)
#endif
#ifndef _In_reads_or_z_
#define _In_reads_or_z_(x)
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Out_writes_opt_
#define _Out_writes_opt_(x)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(x)
#endif
#ifndef _Out_writes_bytes_opt_
#define _Out_writes_bytes_opt_(x)
#endif
#ifndef _Out_writes_to_
#define _Out_writes_to_(x, y)
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_opt_
#define _Outptr_opt_
#endif
#ifndef _Outptr_opt_result_maybenull_
#define _Outptr_opt_result_maybenull_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _COM_Outptr_
#define _COM_Outptr_
#endif
#ifndef _COM_Outptr_opt_
#define _COM_Outptr_opt_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_
#define _Ret_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#ifndef _When_
#define _When_(a, b)
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#ifndef _Field_size_
#define _Field_size_(x)
#endif
#ifndef _Field_size_opt_
#define _Field_size_opt_(x)
#endif
#ifndef _Field_size_bytes_
#define _Field_size_bytes_(x)
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Post_equals_last_error_
#define _Post_equals_last_error_
#endif
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif
#ifndef _Reserved_
#define _Reserved_
#endif
#ifndef _In_range_
#define _In_range_(a, b)
#endif
#ifndef _Analysis_assume_
#define _Analysis_assume_(x)
#endif

#ifndef D3D11_SDK_VERSION
#define D3D11_SDK_VERSION 7
#endif

//////////////////////////////////////////////////////////////////////////////
// Forward declarations
//////////////////////////////////////////////////////////////////////////////

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Buffer;
struct ID3D11Texture1D;
struct ID3D11Texture2D;
struct ID3D11Texture3D;
struct ID3D11View;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11UnorderedAccessView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11GeometryShader;
struct ID3D11HullShader;
struct ID3D11DomainShader;
struct ID3D11ComputeShader;
struct ID3D11InputLayout;
struct ID3D11SamplerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11Query;
struct ID3D11Predicate;
struct ID3D11Counter;
struct ID3D11Asynchronous;
struct ID3D11ClassInstance;
struct ID3D11ClassLinkage;
struct ID3D11CommandList;
struct ID3D11ShaderReflection;
struct ID3D11ShaderReflectionType;
struct ID3D11ShaderReflectionVariable;
struct ID3D11ShaderReflectionConstantBuffer;
struct ID3D11FunctionReflection;
struct ID3D11Debug;

//////////////////////////////////////////////////////////////////////////////
// Enums
//////////////////////////////////////////////////////////////////////////////

typedef enum D3D11_USAGE
{
    D3D11_USAGE_DEFAULT = 0,
    D3D11_USAGE_IMMUTABLE = 1,
    D3D11_USAGE_DYNAMIC = 2,
    D3D11_USAGE_STAGING = 3
} D3D11_USAGE;

typedef enum D3D11_BIND_FLAG
{
    D3D11_BIND_VERTEX_BUFFER = 0x1L,
    D3D11_BIND_INDEX_BUFFER = 0x2L,
    D3D11_BIND_CONSTANT_BUFFER = 0x4L,
    D3D11_BIND_SHADER_RESOURCE = 0x8L,
    D3D11_BIND_STREAM_OUTPUT = 0x10L,
    D3D11_BIND_RENDER_TARGET = 0x20L,
    D3D11_BIND_DEPTH_STENCIL = 0x40L,
    D3D11_BIND_UNORDERED_ACCESS = 0x80L,
    D3D11_BIND_DECODER = 0x200L,
    D3D11_BIND_VIDEO_ENCODER = 0x400L
} D3D11_BIND_FLAG;

typedef enum D3D11_CPU_ACCESS_FLAG
{
    D3D11_CPU_ACCESS_WRITE = 0x10000L,
    D3D11_CPU_ACCESS_READ = 0x20000L
} D3D11_CPU_ACCESS_FLAG;

typedef enum D3D11_RESOURCE_MISC_FLAG
{
    D3D11_RESOURCE_MISC_GENERATE_MIPS = 0x1L,
    D3D11_RESOURCE_MISC_SHARED = 0x2L,
    D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4L,
    D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS = 0x10L,
    D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20L,
    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40L,
    D3D11_RESOURCE_MISC_RESOURCE_CLAMP = 0x80L,
    D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x100L,
    D3D11_RESOURCE_MISC_GDI_COMPATIBLE = 0x200L,
    D3D11_RESOURCE_MISC_SHARED_NTHANDLE = 0x800L,
    D3D11_RESOURCE_MISC_RESTRICTED_CONTENT = 0x1000L,
    D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE = 0x2000L,
    D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER = 0x4000L,
    D3D11_RESOURCE_MISC_GUARDED = 0x8000L,
    D3D11_RESOURCE_MISC_TILE_POOL = 0x20000L,
    D3D11_RESOURCE_MISC_TILED = 0x40000L,
    D3D11_RESOURCE_MISC_HW_PROTECTED = 0x80000L
} D3D11_RESOURCE_MISC_FLAG;

typedef enum D3D11_RESOURCE_DIMENSION
{
    D3D11_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D11_RESOURCE_DIMENSION_BUFFER = 1,
    D3D11_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D11_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D11_RESOURCE_DIMENSION_TEXTURE3D = 4
} D3D11_RESOURCE_DIMENSION;

typedef enum D3D11_DSV_DIMENSION
{
    D3D11_DSV_DIMENSION_UNKNOWN = 0,
    D3D11_DSV_DIMENSION_TEXTURE1D = 1,
    D3D11_DSV_DIMENSION_TEXTURE1DARRAY = 2,
    D3D11_DSV_DIMENSION_TEXTURE2D = 3,
    D3D11_DSV_DIMENSION_TEXTURE2DARRAY = 4,
    D3D11_DSV_DIMENSION_TEXTURE2DMS = 5,
    D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY = 6
} D3D11_DSV_DIMENSION;

typedef enum D3D11_RTV_DIMENSION
{
    D3D11_RTV_DIMENSION_UNKNOWN = 0,
    D3D11_RTV_DIMENSION_BUFFER = 1,
    D3D11_RTV_DIMENSION_TEXTURE1D = 2,
    D3D11_RTV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D11_RTV_DIMENSION_TEXTURE2D = 4,
    D3D11_RTV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D11_RTV_DIMENSION_TEXTURE2DMS = 6,
    D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D11_RTV_DIMENSION_TEXTURE3D = 8
} D3D11_RTV_DIMENSION;

typedef enum D3D11_UAV_DIMENSION
{
    D3D11_UAV_DIMENSION_UNKNOWN = 0,
    D3D11_UAV_DIMENSION_BUFFER = 1,
    D3D11_UAV_DIMENSION_TEXTURE1D = 2,
    D3D11_UAV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D11_UAV_DIMENSION_TEXTURE2D = 4,
    D3D11_UAV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D11_UAV_DIMENSION_TEXTURE3D = 8
} D3D11_UAV_DIMENSION;

// D3D11_SRV_DIMENSION is NOT a distinct enum in the SDK: d3d11.h aliases the
// D3Dcommon one, which is why DDSTextureLoader.cpp can assign a
// D3D_SRV_DIMENSION straight into a D3D11_SHADER_RESOURCE_VIEW_DESC.
typedef D3D_SRV_DIMENSION D3D11_SRV_DIMENSION;

typedef enum D3D11_MAP
{
    D3D11_MAP_READ = 1,
    D3D11_MAP_WRITE = 2,
    D3D11_MAP_READ_WRITE = 3,
    D3D11_MAP_WRITE_DISCARD = 4,
    D3D11_MAP_WRITE_NO_OVERWRITE = 5
} D3D11_MAP;

typedef enum D3D11_MAP_FLAG
{
    D3D11_MAP_FLAG_DO_NOT_WAIT = 0x100000L
} D3D11_MAP_FLAG;

typedef enum D3D11_QUERY
{
    D3D11_QUERY_EVENT = 0,
    D3D11_QUERY_OCCLUSION = (D3D11_QUERY_EVENT + 1),
    D3D11_QUERY_TIMESTAMP = (D3D11_QUERY_OCCLUSION + 1),
    D3D11_QUERY_TIMESTAMP_DISJOINT = (D3D11_QUERY_TIMESTAMP + 1),
    D3D11_QUERY_PIPELINE_STATISTICS = (D3D11_QUERY_TIMESTAMP_DISJOINT + 1),
    D3D11_QUERY_OCCLUSION_PREDICATE = (D3D11_QUERY_PIPELINE_STATISTICS + 1),
    D3D11_QUERY_SO_STATISTICS = (D3D11_QUERY_OCCLUSION_PREDICATE + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE = (D3D11_QUERY_SO_STATISTICS + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM0 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 = (D3D11_QUERY_SO_STATISTICS_STREAM0 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM1 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 = (D3D11_QUERY_SO_STATISTICS_STREAM1 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM2 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 = (D3D11_QUERY_SO_STATISTICS_STREAM2 + 1),
    D3D11_QUERY_SO_STATISTICS_STREAM3 = (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 + 1),
    D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3 = (D3D11_QUERY_SO_STATISTICS_STREAM3 + 1)
} D3D11_QUERY;

typedef enum D3D11_QUERY_MISC_FLAG
{
    D3D11_QUERY_MISC_PREDICATEHINT = 0x1L
} D3D11_QUERY_MISC_FLAG;

typedef enum D3D11_FILL_MODE
{
    D3D11_FILL_WIREFRAME = 2,
    D3D11_FILL_SOLID = 3
} D3D11_FILL_MODE;

typedef enum D3D11_CULL_MODE
{
    D3D11_CULL_NONE = 1,
    D3D11_CULL_FRONT = 2,
    D3D11_CULL_BACK = 3
} D3D11_CULL_MODE;

typedef enum D3D11_COMPARISON_FUNC
{
    D3D11_COMPARISON_NEVER = 1,
    D3D11_COMPARISON_LESS = 2,
    D3D11_COMPARISON_EQUAL = 3,
    D3D11_COMPARISON_LESS_EQUAL = 4,
    D3D11_COMPARISON_GREATER = 5,
    D3D11_COMPARISON_NOT_EQUAL = 6,
    D3D11_COMPARISON_GREATER_EQUAL = 7,
    D3D11_COMPARISON_ALWAYS = 8
} D3D11_COMPARISON_FUNC;

typedef enum D3D11_DEPTH_WRITE_MASK
{
    D3D11_DEPTH_WRITE_MASK_ZERO = 0,
    D3D11_DEPTH_WRITE_MASK_ALL = 1
} D3D11_DEPTH_WRITE_MASK;

typedef enum D3D11_STENCIL_OP
{
    D3D11_STENCIL_OP_KEEP = 1,
    D3D11_STENCIL_OP_ZERO = 2,
    D3D11_STENCIL_OP_REPLACE = 3,
    D3D11_STENCIL_OP_INCR_SAT = 4,
    D3D11_STENCIL_OP_DECR_SAT = 5,
    D3D11_STENCIL_OP_INVERT = 6,
    D3D11_STENCIL_OP_INCR = 7,
    D3D11_STENCIL_OP_DECR = 8
} D3D11_STENCIL_OP;

typedef enum D3D11_BLEND
{
    D3D11_BLEND_ZERO = 1,
    D3D11_BLEND_ONE = 2,
    D3D11_BLEND_SRC_COLOR = 3,
    D3D11_BLEND_INV_SRC_COLOR = 4,
    D3D11_BLEND_SRC_ALPHA = 5,
    D3D11_BLEND_INV_SRC_ALPHA = 6,
    D3D11_BLEND_DEST_ALPHA = 7,
    D3D11_BLEND_INV_DEST_ALPHA = 8,
    D3D11_BLEND_DEST_COLOR = 9,
    D3D11_BLEND_INV_DEST_COLOR = 10,
    D3D11_BLEND_SRC_ALPHA_SAT = 11,
    D3D11_BLEND_BLEND_FACTOR = 14,
    D3D11_BLEND_INV_BLEND_FACTOR = 15,
    D3D11_BLEND_SRC1_COLOR = 16,
    D3D11_BLEND_INV_SRC1_COLOR = 17,
    D3D11_BLEND_SRC1_ALPHA = 18,
    D3D11_BLEND_INV_SRC1_ALPHA = 19
} D3D11_BLEND;

typedef enum D3D11_BLEND_OP
{
    D3D11_BLEND_OP_ADD = 1,
    D3D11_BLEND_OP_SUBTRACT = 2,
    D3D11_BLEND_OP_REV_SUBTRACT = 3,
    D3D11_BLEND_OP_MIN = 4,
    D3D11_BLEND_OP_MAX = 5
} D3D11_BLEND_OP;

typedef enum D3D11_TEXTURE_ADDRESS_MODE
{
    D3D11_TEXTURE_ADDRESS_WRAP = 1,
    D3D11_TEXTURE_ADDRESS_MIRROR = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP = 3,
    D3D11_TEXTURE_ADDRESS_BORDER = 4,
    D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5
} D3D11_TEXTURE_ADDRESS_MODE;

typedef enum D3D11_FILTER_TYPE
{
    D3D11_FILTER_TYPE_POINT = 0,
    D3D11_FILTER_TYPE_LINEAR = 1
} D3D11_FILTER_TYPE;

typedef enum D3D11_FILTER_REDUCTION_TYPE
{
    D3D11_FILTER_REDUCTION_TYPE_STANDARD = 0,
    D3D11_FILTER_REDUCTION_TYPE_COMPARISON = 1,
    D3D11_FILTER_REDUCTION_TYPE_MINIMUM = 2,
    D3D11_FILTER_REDUCTION_TYPE_MAXIMUM = 3
} D3D11_FILTER_REDUCTION_TYPE;

typedef enum D3D11_FILTER
{
    D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
    D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
    D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
    D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
    D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D11_FILTER_ANISOTROPIC = 0x55,
    D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT = 0x80,
    D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR = 0x81,
    D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x84,
    D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR = 0x85,
    D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT = 0x90,
    D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x91,
    D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT = 0x94,
    D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR = 0x95,
    D3D11_FILTER_COMPARISON_ANISOTROPIC = 0xd5,
    D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT = 0x100,
    D3D11_FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x101,
    D3D11_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x104,
    D3D11_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x105,
    D3D11_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x110,
    D3D11_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x111,
    D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x114,
    D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR = 0x115,
    D3D11_FILTER_MINIMUM_ANISOTROPIC = 0x155,
    D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT = 0x180,
    D3D11_FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x181,
    D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x184,
    D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x185,
    D3D11_FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x190,
    D3D11_FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x191,
    D3D11_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x194,
    D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR = 0x195,
    D3D11_FILTER_MAXIMUM_ANISOTROPIC = 0x1d5
} D3D11_FILTER;

typedef enum D3D11_INPUT_CLASSIFICATION
{
    D3D11_INPUT_PER_VERTEX_DATA = 0,
    D3D11_INPUT_PER_INSTANCE_DATA = 1
} D3D11_INPUT_CLASSIFICATION;

typedef enum D3D11_DEVICE_CONTEXT_TYPE
{
    D3D11_DEVICE_CONTEXT_IMMEDIATE = 0,
    D3D11_DEVICE_CONTEXT_DEFERRED = 1
} D3D11_DEVICE_CONTEXT_TYPE;

typedef enum D3D11_CREATE_DEVICE_FLAG
{
    D3D11_CREATE_DEVICE_DEBUG = 0x2,
    D3D11_CREATE_DEVICE_SWITCH_TO_REF = 0x4,
    D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS = 0x8,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20,
    D3D11_CREATE_DEVICE_DEBUGGABLE = 0x40,
    D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY = 0x80,
    D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT = 0x100,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT = 0x800
} D3D11_CREATE_DEVICE_FLAG;

typedef enum D3D11_FORMAT_SUPPORT
{
    D3D11_FORMAT_SUPPORT_BUFFER = 0x1,
    D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER = 0x2,
    D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER = 0x4,
    D3D11_FORMAT_SUPPORT_SO_BUFFER = 0x8,
    D3D11_FORMAT_SUPPORT_TEXTURE1D = 0x10,
    D3D11_FORMAT_SUPPORT_TEXTURE2D = 0x20,
    D3D11_FORMAT_SUPPORT_TEXTURE3D = 0x40,
    D3D11_FORMAT_SUPPORT_TEXTURECUBE = 0x80,
    D3D11_FORMAT_SUPPORT_SHADER_LOAD = 0x100,
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE = 0x200,
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON = 0x400,
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_MONO_TEXT = 0x800,
    D3D11_FORMAT_SUPPORT_MIP = 0x1000,
    D3D11_FORMAT_SUPPORT_MIP_AUTOGEN = 0x2000,
    D3D11_FORMAT_SUPPORT_RENDER_TARGET = 0x4000,
    D3D11_FORMAT_SUPPORT_BLENDABLE = 0x8000,
    D3D11_FORMAT_SUPPORT_DEPTH_STENCIL = 0x10000,
    D3D11_FORMAT_SUPPORT_CPU_LOCKABLE = 0x20000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE = 0x40000,
    D3D11_FORMAT_SUPPORT_DISPLAY = 0x80000,
    D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT = 0x100000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET = 0x200000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD = 0x400000,
    D3D11_FORMAT_SUPPORT_SHADER_GATHER = 0x800000,
    D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST = 0x1000000,
    D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW = 0x2000000,
    D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON = 0x4000000,
    D3D11_FORMAT_SUPPORT_DECODER_OUTPUT = 0x8000000,
    D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT = 0x10000000,
    D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT = 0x20000000,
    D3D11_FORMAT_SUPPORT_VIDEO_ENCODER = 0x40000000
} D3D11_FORMAT_SUPPORT;

typedef enum D3D11_CLEAR_FLAG
{
    D3D11_CLEAR_DEPTH = 0x1L,
    D3D11_CLEAR_STENCIL = 0x2L
} D3D11_CLEAR_FLAG;

typedef enum D3D11_RLDO_FLAGS
{
    D3D11_RLDO_SUMMARY = 0x1,
    D3D11_RLDO_DETAIL = 0x2,
    D3D11_RLDO_IGNORE_INTERNAL = 0x4
} D3D11_RLDO_FLAGS;

typedef enum D3D11_SHADER_MIN_PRECISION_SUPPORT
{
    D3D11_SHADER_MIN_PRECISION_10_BIT = 0x1,
    D3D11_SHADER_MIN_PRECISION_16_BIT = 0x2
} D3D11_SHADER_MIN_PRECISION_SUPPORT;

// Render-target write mask bits (D3D11_RENDER_TARGET_BLEND_DESC::
// RenderTargetWriteMask).  The engine spells the full mask out by name.
typedef enum D3D11_COLOR_WRITE_ENABLE
{
    D3D11_COLOR_WRITE_ENABLE_RED = 1,
    D3D11_COLOR_WRITE_ENABLE_GREEN = 2,
    D3D11_COLOR_WRITE_ENABLE_BLUE = 4,
    D3D11_COLOR_WRITE_ENABLE_ALPHA = 8,
    D3D11_COLOR_WRITE_ENABLE_ALL = (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                    D3D11_COLOR_WRITE_ENABLE_BLUE | D3D11_COLOR_WRITE_ENABLE_ALPHA)
} D3D11_COLOR_WRITE_ENABLE;

//////////////////////////////////////////////////////////////////////////////
// D3D11_* aliases for the d3dcommon.h enums
//
// The real d3d11.h typedefs these names so that D3D11 code never has to spell
// the api-agnostic D3D_* name.  Several vendored call sites use
// D3D11_PRIMITIVE_TOPOLOGY, so the aliases are not optional.
//////////////////////////////////////////////////////////////////////////////

typedef D3D_PRIMITIVE_TOPOLOGY       D3D11_PRIMITIVE_TOPOLOGY;
typedef D3D_PRIMITIVE                D3D11_PRIMITIVE;
typedef D3D_REGISTER_COMPONENT_TYPE  D3D11_REGISTER_COMPONENT_TYPE;
typedef D3D_CBUFFER_TYPE             D3D11_CBUFFER_TYPE;
typedef D3D_SHADER_INPUT_TYPE        D3D11_SHADER_INPUT_TYPE;
typedef D3D_RESOURCE_RETURN_TYPE     D3D11_RESOURCE_RETURN_TYPE;
typedef D3D_INTERPOLATION_MODE       D3D11_INTERPOLATION_MODE;
typedef D3D_MIN_PRECISION            D3D11_MIN_PRECISION;
typedef D3D_NAME                     D3D11_NAME;
typedef D3D_SHADER_VARIABLE_CLASS    D3D11_SHADER_VARIABLE_CLASS;
typedef D3D_SHADER_VARIABLE_TYPE     D3D11_SHADER_VARIABLE_TYPE;
typedef D3D_TESSELLATOR_DOMAIN       D3D11_TESSELLATOR_DOMAIN;
typedef D3D_TESSELLATOR_PARTITIONING D3D11_TESSELLATOR_PARTITIONING;
typedef D3D_TESSELLATOR_OUTPUT_PRIMITIVE D3D11_TESSELLATOR_OUTPUT_PRIMITIVE;

//////////////////////////////////////////////////////////////////////////////
// Scalar constants
//////////////////////////////////////////////////////////////////////////////

#define D3D11_STANDARD_MULTISAMPLE_PATTERN 0xffffffff
#define D3D11_CENTER_MULTISAMPLE_PATTERN   0xfffffffe

#define D3D11_APPEND_ALIGNED_ELEMENT 0xffffffff

#define D3D11_16BIT_INDEX_STRIP_CUT_VALUE ( 0xffff )
#define D3D11_32BIT_INDEX_STRIP_CUT_VALUE ( 0xffffffff )
#define D3D11_8BIT_INDEX_STRIP_CUT_VALUE ( 0xff )

#define D3D11_ARRAY_AXIS_ADDRESS_RANGE_BIT_COUNT ( 9 )

#define D3D11_CLIP_OR_CULL_DISTANCE_COUNT ( 8 )
#define D3D11_CLIP_OR_CULL_DISTANCE_ELEMENT_COUNT ( 2 )

#define D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ( 14 )
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS ( 4 )
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_COMPONENT_BIT_COUNT ( 32 )
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_HW_SLOT_COUNT ( 15 )
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COMPONENTS ( 4 )
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT ( 15 )
#define D3D11_COMMONSHADER_FLOWCONTROL_NESTING_LIMIT ( 64 )
#define D3D11_COMMONSHADER_IMMEDIATE_CONSTANT_BUFFER_REGISTER_COMPONENTS ( 4 )
#define D3D11_COMMONSHADER_IMMEDIATE_CONSTANT_BUFFER_REGISTER_COUNT ( 1 )
#define D3D11_COMMONSHADER_INPUT_RESOURCE_REGISTER_COMPONENTS ( 1 )
#define D3D11_COMMONSHADER_INPUT_RESOURCE_REGISTER_COUNT ( 128 )
#define D3D11_COMMONSHADER_SAMPLER_REGISTER_COMPONENTS ( 1 )
#define D3D11_COMMONSHADER_SAMPLER_REGISTER_COUNT ( 16 )
#define D3D11_COMMONSHADER_SUBROUTINE_NESTING_LIMIT ( 32 )
#define D3D11_COMMONSHADER_TEMP_REGISTER_COMPONENTS ( 4 )
#define D3D11_COMMONSHADER_TEMP_REGISTER_COUNT ( 4096 )
#define D3D11_CS_4_X_BUCKET00_MAX_BYTES_TGSM_WRITABLE_PER_THREAD ( 256 )
#define D3D11_CS_THREAD_GROUP_MAX_X ( 1024 )
#define D3D11_CS_THREAD_GROUP_MAX_Y ( 1024 )
#define D3D11_CS_THREAD_GROUP_MAX_Z ( 64 )
#define D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP ( 1024 )
#define D3D11_CS_THREADID_GROUP_BIT_COUNT ( 10 )
#define D3D11_IA_DEFAULT_INDEX_BUFFER_OFFSET_IN_BYTES ( 0 )
#define D3D11_IA_DEFAULT_PRIMITIVE_TOPOLOGY ( 0 )
#define D3D11_IA_DEFAULT_VERTEX_BUFFER_OFFSET_IN_BYTES ( 0 )
#define D3D11_IA_INDEX_INPUT_RESOURCE_SLOT_COUNT ( 1 )
#define D3D11_IA_INSTANCE_ID_BIT_COUNT ( 32 )
#define D3D11_IA_INTEGER_ARITHMETIC_BIT_COUNT ( 32 )
#define D3D11_IA_PATCH_MAX_CONTROL_POINT_COUNT ( 32 )
#define D3D11_IA_PRIMITIVE_ID_BIT_COUNT ( 32 )
#define D3D11_IA_VERTEX_ID_BIT_COUNT ( 32 )
#define D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ( 32 )
#define D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS ( 128 )
#define D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT ( 32 )
#define D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT ( 32 )
#define D3D11_OM_BlendState_Object_COUNT ( 1 )
#define D3D11_OM_DEPTH_STENCIL_OBJECT_COUNT ( 1 )
#define D3D11_OM_RENDER_TARGET_OBJECT_COUNT ( 8 )
#define D3D11_PS_INPUT_REGISTER_COMPONENTS ( 4 )
#define D3D11_PS_INPUT_REGISTER_COUNT ( 32 )
#define D3D11_PS_OUTPUT_REGISTER_COMPONENTS ( 4 )
#define D3D11_PS_OUTPUT_REGISTER_COUNT ( 8 )
#define D3D11_REQ_BLEND_OBJECT_COUNT_PER_DEVICE ( 4096 )
#define D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP ( 27 )
#define D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT ( 4096 )
#define D3D11_REQ_DEPTH_STENCIL_OBJECT_COUNT_PER_DEVICE ( 4096 )
#define D3D11_REQ_DRAWINDEXED_INDEX_COUNT_2_TO_EXP ( 32 )
#define D3D11_REQ_DRAW_VERTEX_COUNT_2_TO_EXP ( 32 )
#define D3D11_REQ_FILTERING_HW_ADDRESSABLE_RESOURCE_DIMENSION ( 16384 )
#define D3D11_REQ_GS_INVOCATION_32BIT_OUTPUT_COMPONENT_LIMIT ( 1024 )
#define D3D11_REQ_IMMEDIATE_CONSTANT_BUFFER_ELEMENT_COUNT ( 4096 )
#define D3D11_REQ_MAXANISOTROPY ( 16 )
#define D3D11_REQ_MIP_LEVELS ( 15 )
#define D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES ( 2048 )
#define D3D11_REQ_RASTERIZER_OBJECT_COUNT_PER_DEVICE ( 4096 )
#define D3D11_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH ( 16384 )
#define D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM ( 128 )
#define D3D11_REQ_RESOURCE_VIEW_COUNT_PER_DEVICE_2_TO_EXP ( 20 )
#define D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE ( 4096 )
#define D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION ( 2048 )
#define D3D11_REQ_TEXTURE1D_U_DIMENSION ( 16384 )
#define D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ( 2048 )
#define D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ( 16384 )
#define D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION ( 2048 )
#define D3D11_REQ_TEXTURECUBE_DIMENSION ( 16384 )
#define D3D11_RESINFO_INSTRUCTION_MISSING_COMPONENT_RETVAL ( 0 )
#define D3D11_SHADER_MAJOR_VERSION ( 5 )
#define D3D11_SHADER_MAX_INSTANCES ( 65535 )
#define D3D11_SHADER_MAX_INTERFACES ( 253 )
#define D3D11_SHADER_MAX_INTERFACE_CALL_SITES ( 4096 )
#define D3D11_SHADER_MAX_TYPES ( 65535 )
#define D3D11_SHADER_MINOR_VERSION ( 0 )
#define D3D11_SHADER_VERSION_MAJOR_VERSION ( 5 )
#define D3D11_SHADER_VERSION_MINOR_VERSION ( 0 )
#define D3D11_VS_INPUT_REGISTER_COMPONENTS ( 4 )
#define D3D11_VS_INPUT_REGISTER_COUNT ( 32 )
#define D3D11_VS_OUTPUT_REGISTER_COMPONENTS ( 4 )
#define D3D11_VS_OUTPUT_REGISTER_COUNT ( 32 )
#define D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE ( 16 )
#define D3D11_VIEWPORT_BOUNDS_MAX ( 32767 )
#define D3D11_VIEWPORT_BOUNDS_MIN ( -32768 )

// The subresource index formula the engine uses when resolving MSAA textures.
#define D3D11CalcSubresource(MipSlice, ArraySlice, MipLevels) \
    ((MipSlice) + ((ArraySlice) * (MipLevels)))

//////////////////////////////////////////////////////////////////////////////
// Structures
//
// NOTE: all of these are plain aggregates - no constructors, no virtuals - so
// that the brace-initialised static literals in the vendored code keep working.
//////////////////////////////////////////////////////////////////////////////

typedef struct D3D11_BUFFER_DESC
{
    UINT        ByteWidth;
    D3D11_USAGE Usage;
    UINT        BindFlags;
    UINT        CPUAccessFlags;
    UINT        MiscFlags;
    UINT        StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct D3D11_TEXTURE1D_DESC
{
    UINT           Width;
    UINT           MipLevels;
    UINT           ArraySize;
    DXGI_FORMAT    Format;
    D3D11_USAGE    Usage;
    UINT           BindFlags;
    UINT           CPUAccessFlags;
    UINT           MiscFlags;
} D3D11_TEXTURE1D_DESC;

typedef struct D3D11_TEXTURE2D_DESC
{
    UINT             Width;
    UINT             Height;
    UINT             MipLevels;
    UINT             ArraySize;
    DXGI_FORMAT      Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE      Usage;
    UINT             BindFlags;
    UINT             CPUAccessFlags;
    UINT             MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct D3D11_TEXTURE3D_DESC
{
    UINT        Width;
    UINT        Height;
    UINT        Depth;
    UINT        MipLevels;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT        BindFlags;
    UINT        CPUAccessFlags;
    UINT        MiscFlags;
} D3D11_TEXTURE3D_DESC;

typedef struct D3D11_BUFFER_SRV
{
    union
    {
        UINT FirstElement;
        UINT ElementOffset;
    };
    union
    {
        UINT NumElements;
        UINT ElementWidth;
    };
} D3D11_BUFFER_SRV;

typedef struct D3D11_BUFFEREX_SRV
{
    UINT FirstElement;
    UINT NumElements;
    UINT Flags;
} D3D11_BUFFEREX_SRV;

typedef struct D3D11_TEX1D_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEX1D_SRV;

typedef struct D3D11_TEX1D_ARRAY_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX1D_ARRAY_SRV;

typedef struct D3D11_TEX2D_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEX2D_SRV;

typedef struct D3D11_TEX2D_ARRAY_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2D_ARRAY_SRV;

typedef struct D3D11_TEX3D_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEX3D_SRV;

typedef struct D3D11_TEXCUBE_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D11_TEXCUBE_SRV;

typedef struct D3D11_TEXCUBE_ARRAY_SRV
{
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT First2DArrayFace;
    UINT NumCubes;
} D3D11_TEXCUBE_ARRAY_SRV;

typedef struct D3D11_TEX2DMS_SRV
{
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_SRV;

typedef struct D3D11_TEX2DMS_ARRAY_SRV
{
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2DMS_ARRAY_SRV;

typedef struct D3D11_SHADER_RESOURCE_VIEW_DESC
{
    DXGI_FORMAT         Format;
    D3D11_SRV_DIMENSION ViewDimension;

    union
    {
        D3D11_BUFFER_SRV        Buffer;
        D3D11_TEX1D_SRV         Texture1D;
        D3D11_TEX1D_ARRAY_SRV   Texture1DArray;
        D3D11_TEX2D_SRV         Texture2D;
        D3D11_TEX2D_ARRAY_SRV   Texture2DArray;
        D3D11_TEX2DMS_SRV       Texture2DMS;
        D3D11_TEX2DMS_ARRAY_SRV Texture2DMSArray;
        D3D11_TEX3D_SRV         Texture3D;
        D3D11_TEXCUBE_SRV       TextureCube;
        D3D11_TEXCUBE_ARRAY_SRV TextureCubeArray;
        D3D11_BUFFEREX_SRV      BufferEx;
    };
} D3D11_SHADER_RESOURCE_VIEW_DESC;

typedef struct D3D11_BUFFER_RTV
{
    union
    {
        UINT FirstElement;
        UINT ElementOffset;
    };
    union
    {
        UINT NumElements;
        UINT ElementWidth;
    };
} D3D11_BUFFER_RTV;

typedef struct D3D11_TEX1D_RTV
{
    UINT MipSlice;
} D3D11_TEX1D_RTV;

typedef struct D3D11_TEX1D_ARRAY_RTV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX1D_ARRAY_RTV;

typedef struct D3D11_TEX2D_RTV
{
    UINT MipSlice;
} D3D11_TEX2D_RTV;

typedef struct D3D11_TEX2DMS_RTV
{
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_RTV;

typedef struct D3D11_TEX2D_ARRAY_RTV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2D_ARRAY_RTV;

typedef struct D3D11_TEX2DMS_ARRAY_RTV
{
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2DMS_ARRAY_RTV;

typedef struct D3D11_TEX3D_RTV
{
    UINT MipSlice;
    UINT FirstWSlice;
    UINT WSize;
} D3D11_TEX3D_RTV;

typedef struct D3D11_RENDER_TARGET_VIEW_DESC
{
    DXGI_FORMAT         Format;
    D3D11_RTV_DIMENSION ViewDimension;

    union
    {
        D3D11_BUFFER_RTV        Buffer;
        D3D11_TEX1D_RTV         Texture1D;
        D3D11_TEX1D_ARRAY_RTV   Texture1DArray;
        D3D11_TEX2D_RTV         Texture2D;
        D3D11_TEX2D_ARRAY_RTV   Texture2DArray;
        D3D11_TEX2DMS_RTV       Texture2DMS;
        D3D11_TEX2DMS_ARRAY_RTV Texture2DMSArray;
        D3D11_TEX3D_RTV         Texture3D;
    };
} D3D11_RENDER_TARGET_VIEW_DESC;

typedef struct D3D11_TEX1D_DSV
{
    UINT MipSlice;
} D3D11_TEX1D_DSV;

typedef struct D3D11_TEX1D_ARRAY_DSV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX1D_ARRAY_DSV;

typedef struct D3D11_TEX2D_DSV
{
    UINT MipSlice;
} D3D11_TEX2D_DSV;

typedef struct D3D11_TEX2DMS_DSV
{
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_DSV;

typedef struct D3D11_TEX2D_ARRAY_DSV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2D_ARRAY_DSV;

typedef struct D3D11_TEX2DMS_ARRAY_DSV
{
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2DMS_ARRAY_DSV;

// Field order matters: DX11Device.cpp's error path and the SDK sample code
// positionally initialise this as { Format, ViewDimension, Flags, ... }.
typedef struct D3D11_DEPTH_STENCIL_VIEW_DESC
{
    DXGI_FORMAT         Format;
    D3D11_DSV_DIMENSION ViewDimension;
    UINT                Flags;

    union
    {
        D3D11_TEX1D_DSV         Texture1D;
        D3D11_TEX1D_ARRAY_DSV   Texture1DArray;
        D3D11_TEX2D_DSV         Texture2D;
        D3D11_TEX2D_ARRAY_DSV   Texture2DArray;
        D3D11_TEX2DMS_DSV       Texture2DMS;
        D3D11_TEX2DMS_ARRAY_DSV Texture2DMSArray;
    };
} D3D11_DEPTH_STENCIL_VIEW_DESC;

typedef struct D3D11_BUFFER_UAV
{
    UINT FirstElement;
    UINT NumElements;
    UINT Flags;
} D3D11_BUFFER_UAV;

typedef struct D3D11_TEX1D_UAV
{
    UINT MipSlice;
} D3D11_TEX1D_UAV;

typedef struct D3D11_TEX1D_ARRAY_UAV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX1D_ARRAY_UAV;

typedef struct D3D11_TEX2D_UAV
{
    UINT MipSlice;
} D3D11_TEX2D_UAV;

typedef struct D3D11_TEX2D_ARRAY_UAV
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2D_ARRAY_UAV;

typedef struct D3D11_TEX2DMS_UAV
{
    UINT UnusedField_NothingToDefine;
} D3D11_TEX2DMS_UAV;

typedef struct D3D11_TEX2DMS_ARRAY_UAV
{
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D11_TEX2DMS_ARRAY_UAV;

typedef struct D3D11_TEX3D_UAV
{
    UINT MipSlice;
    UINT FirstWSlice;
    UINT WSize;
} D3D11_TEX3D_UAV;

typedef struct D3D11_UNORDERED_ACCESS_VIEW_DESC
{
    DXGI_FORMAT         Format;
    D3D11_UAV_DIMENSION ViewDimension;

    union
    {
        D3D11_BUFFER_UAV        Buffer;
        D3D11_TEX1D_UAV         Texture1D;
        D3D11_TEX1D_ARRAY_UAV   Texture1DArray;
        D3D11_TEX2D_UAV         Texture2D;
        D3D11_TEX2D_ARRAY_UAV   Texture2DArray;
        D3D11_TEX2DMS_UAV       Texture2DMS;
        D3D11_TEX2DMS_ARRAY_UAV Texture2DMSArray;
        D3D11_TEX3D_UAV         Texture3D;
    };
} D3D11_UNORDERED_ACCESS_VIEW_DESC;

typedef struct D3D11_SAMPLER_DESC
{
    D3D11_FILTER               Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU;
    D3D11_TEXTURE_ADDRESS_MODE AddressV;
    D3D11_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT                      MipLODBias;
    UINT                       MaxAnisotropy;
    D3D11_COMPARISON_FUNC      ComparisonFunc;
    FLOAT                      BorderColor[4];
    FLOAT                      MinLOD;
    FLOAT                      MaxLOD;
} D3D11_SAMPLER_DESC;

typedef struct D3D11_RENDER_TARGET_BLEND_DESC
{
    BOOL           BlendEnable;
    D3D11_BLEND    SrcBlend;
    D3D11_BLEND    DestBlend;
    D3D11_BLEND_OP BlendOp;
    D3D11_BLEND    SrcBlendAlpha;
    D3D11_BLEND    DestBlendAlpha;
    D3D11_BLEND_OP BlendOpAlpha;
    UINT8          RenderTargetWriteMask;
} D3D11_RENDER_TARGET_BLEND_DESC;

typedef struct D3D11_BLEND_DESC
{
    BOOL                           AlphaToCoverageEnable;
    BOOL                           IndependentBlendEnable;
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8];
} D3D11_BLEND_DESC;

typedef struct D3D11_DEPTH_STENCILOP_DESC
{
    D3D11_STENCIL_OP      StencilFailOp;
    D3D11_STENCIL_OP      StencilDepthFailOp;
    D3D11_STENCIL_OP      StencilPassOp;
    D3D11_COMPARISON_FUNC StencilFunc;
} D3D11_DEPTH_STENCILOP_DESC;

typedef struct D3D11_DEPTH_STENCIL_DESC
{
    BOOL                       DepthEnable;
    D3D11_DEPTH_WRITE_MASK     DepthWriteMask;
    D3D11_COMPARISON_FUNC      DepthFunc;
    BOOL                       StencilEnable;
    UINT8                      StencilReadMask;
    UINT8                      StencilWriteMask;
    D3D11_DEPTH_STENCILOP_DESC FrontFace;
    D3D11_DEPTH_STENCILOP_DESC BackFace;
} D3D11_DEPTH_STENCIL_DESC;

typedef struct D3D11_RASTERIZER_DESC
{
    D3D11_FILL_MODE FillMode;
    D3D11_CULL_MODE CullMode;
    BOOL            FrontCounterClockwise;
    INT             DepthBias;
    FLOAT           DepthBiasClamp;
    FLOAT           SlopeScaledDepthBias;
    BOOL            DepthClipEnable;
    BOOL            ScissorEnable;
    BOOL            MultisampleEnable;
    BOOL            AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;

typedef struct D3D11_VIEWPORT
{
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D11_VIEWPORT;

typedef RECT D3D11_RECT;

typedef struct D3D11_BOX
{
    UINT left;
    UINT top;
    UINT front;
    UINT right;
    UINT bottom;
    UINT back;
} D3D11_BOX;

typedef struct D3D11_MAPPED_SUBRESOURCE
{
    void *pData;
    UINT RowPitch;
    UINT DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

typedef struct D3D11_SUBRESOURCE_DATA
{
    const void *pSysMem;
    UINT        SysMemPitch;
    UINT        SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

typedef struct D3D11_INPUT_ELEMENT_DESC
{
    LPCSTR                     SemanticName;
    UINT                       SemanticIndex;
    DXGI_FORMAT                Format;
    UINT                       InputSlot;
    UINT                       AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass;
    UINT                       InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

typedef struct D3D11_QUERY_DESC
{
    D3D11_QUERY Query;
    UINT        MiscFlags;
} D3D11_QUERY_DESC;

//////////////////////////////////////////////////////////////////////////////
// Shader reflection structures
//////////////////////////////////////////////////////////////////////////////

typedef struct _D3D11_SHADER_DESC
{
    UINT                        Version;
    LPCSTR                      Creator;
    UINT                        Flags;
    UINT                        ConstantBuffers;
    UINT                        BoundResources;
    UINT                        InputParameters;
    UINT                        OutputParameters;
    UINT                        InstructionCount;
    UINT                        TempRegisterCount;
    UINT                        TempArrayCount;
    UINT                        DefCount;
    UINT                        DclCount;
    UINT                        TextureNormalInstructions;
    UINT                        TextureLoadInstructions;
    UINT                        TextureCompInstructions;
    UINT                        TextureBiasInstructions;
    UINT                        TextureGradientInstructions;
    UINT                        FloatInstructionCount;
    UINT                        IntInstructionCount;
    UINT                        UintInstructionCount;
    UINT                        StaticFlowControlCount;
    UINT                        DynamicFlowControlCount;
    UINT                        MacroInstructionCount;
    UINT                        ArrayInstructionCount;
    UINT                        CutInstructionCount;
    UINT                        EmitInstructionCount;
    D3D_PRIMITIVE_TOPOLOGY      GSOutputTopology;
    UINT                        GSMaxOutputVertexCount;
    D3D_PRIMITIVE               InputPrimitive;
    UINT                        PatchConstantParameters;
    UINT                        cGSInstanceCount;
    UINT                        cControlPoints;
    D3D_TESSELLATOR_OUTPUT_PRIMITIVE HSOutputPrimitive;
    D3D_TESSELLATOR_PARTITIONING     HSPartitioning;
    D3D_TESSELLATOR_DOMAIN           TessellatorDomain;
    UINT                        cBarrierInstructions;
    UINT                        cInterlockedInstructions;
    UINT                        cTextureStoreInstructions;
} D3D11_SHADER_DESC;

typedef struct _D3D11_SHADER_BUFFER_DESC
{
    LPCSTR           Name;
    D3D_CBUFFER_TYPE Type;
    UINT             Variables;
    UINT             Size;
    UINT             uFlags;
} D3D11_SHADER_BUFFER_DESC;

typedef struct _D3D11_SHADER_VARIABLE_DESC
{
    LPCSTR Name;
    UINT   StartOffset;
    UINT   Size;
    UINT   uFlags;
    LPVOID DefaultValue;
    UINT   StartTexture;
    UINT   TextureSize;
    UINT   StartSampler;
    UINT   SamplerSize;
} D3D11_SHADER_VARIABLE_DESC;

typedef struct _D3D11_SHADER_TYPE_DESC
{
    D3D_SHADER_VARIABLE_CLASS Class;
    D3D_SHADER_VARIABLE_TYPE  Type;
    UINT                      Rows;
    UINT                      Columns;
    UINT                      Elements;
    UINT                      Members;
    UINT                      Offset;
    LPCSTR                    Name;
} D3D11_SHADER_TYPE_DESC;

typedef struct _D3D11_SHADER_INPUT_BIND_DESC
{
    LPCSTR                  Name;
    D3D_SHADER_INPUT_TYPE   Type;
    UINT                    BindPoint;
    UINT                    BindCount;
    UINT                    uFlags;
    D3D_RESOURCE_RETURN_TYPE ReturnType;
    D3D_SRV_DIMENSION       Dimension;
    UINT                    NumSamples;
} D3D11_SHADER_INPUT_BIND_DESC;

typedef struct _D3D11_SIGNATURE_PARAMETER_DESC
{
    LPCSTR                      SemanticName;
    UINT                        SemanticIndex;
    UINT                        Register;
    D3D_NAME                    SystemValueType;
    D3D_REGISTER_COMPONENT_TYPE ComponentType;
    BYTE                        Mask;
    BYTE                        ReadWriteMask;
    UINT                        Stream;
    D3D_MIN_PRECISION           MinPrecision;
} D3D11_SIGNATURE_PARAMETER_DESC;

//////////////////////////////////////////////////////////////////////////////
// Interfaces
//////////////////////////////////////////////////////////////////////////////

struct ID3D11DeviceChild : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) = 0;
};

struct ID3D11Resource : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) = 0;
    virtual void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) = 0;
    virtual UINT STDMETHODCALLTYPE GetEvictionPriority(void) = 0;
};

struct ID3D11Buffer : public ID3D11Resource
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC *pDesc) = 0;
};

struct ID3D11Texture1D : public ID3D11Resource
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC *pDesc) = 0;
};

struct ID3D11Texture2D : public ID3D11Resource
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC *pDesc) = 0;
};

struct ID3D11Texture3D : public ID3D11Resource
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC *pDesc) = 0;
};

struct ID3D11View : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) = 0;
};

struct ID3D11ShaderResourceView : public ID3D11View
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc) = 0;
};

struct ID3D11RenderTargetView : public ID3D11View
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC *pDesc) = 0;
};

struct ID3D11DepthStencilView : public ID3D11View
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc) = 0;
};

struct ID3D11UnorderedAccessView : public ID3D11View
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc) = 0;
};

struct ID3D11VertexShader : public ID3D11DeviceChild
{
};

struct ID3D11HullShader : public ID3D11DeviceChild
{
};

struct ID3D11DomainShader : public ID3D11DeviceChild
{
};

struct ID3D11GeometryShader : public ID3D11DeviceChild
{
};

struct ID3D11PixelShader : public ID3D11DeviceChild
{
};

struct ID3D11ComputeShader : public ID3D11DeviceChild
{
};

struct ID3D11InputLayout : public ID3D11DeviceChild
{
};

struct ID3D11SamplerState : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC *pDesc) = 0;
};

struct ID3D11BlendState : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC *pDesc) = 0;
};

struct ID3D11DepthStencilState : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC *pDesc) = 0;
};

struct ID3D11RasterizerState : public ID3D11DeviceChild
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC *pDesc) = 0;
};

struct ID3D11Asynchronous : public ID3D11DeviceChild
{
    virtual UINT STDMETHODCALLTYPE GetDataSize(void) = 0;
};

struct ID3D11Query : public ID3D11Asynchronous
{
    virtual void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC *pDesc) = 0;
};

struct ID3D11Predicate : public ID3D11Query
{
};

struct ID3D11Counter : public ID3D11Asynchronous
{
};

struct ID3D11ClassInstance : public ID3D11DeviceChild
{
};

struct ID3D11ClassLinkage : public ID3D11DeviceChild
{
};

struct ID3D11CommandList : public ID3D11DeviceChild
{
};

struct ID3D11Device : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout **ppInputLayout) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc, ID3D11Predicate **ppPredicate) = 0;
    virtual D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) = 0;
    virtual void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) = 0;
};

struct ID3D11DeviceContext : public ID3D11DeviceChild
{
    //////////////////////////////////////////////////////////////////////////
    // Input assembler
    virtual void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout *pInputLayout) = 0;
    virtual void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets) = 0;
    virtual void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset) = 0;
    virtual void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) = 0;
    virtual void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *pTopology) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Vertex shader
    virtual void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;
    virtual void STDMETHODCALLTYPE VSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Hull / domain / geometry shader
    virtual void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;

    virtual void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;

    virtual void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader *pShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Stream output
    virtual void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer *const *ppSOTargets, const UINT *pOffsets) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Draw
    virtual void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) = 0;
    virtual void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) = 0;
    virtual void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) = 0;
    virtual void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) = 0;
    virtual void STDMETHODCALLTYPE DrawAuto(void) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Compute / pixel shader
    virtual void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) = 0;
    virtual void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;
    virtual void STDMETHODCALLTYPE PSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) = 0;

    virtual void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) = 0;
    virtual void STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) = 0;
    virtual void STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) = 0;
    virtual void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) = 0;
    virtual void STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Output merger
    virtual void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView) = 0;
    virtual void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) = 0;
    virtual void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState *pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) = 0;
    virtual void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState *pDepthStencilState, UINT StencilRef) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Resource manipulation
    virtual void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4]) = 0;
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4]) = 0;
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4]) = 0;
    virtual void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) = 0;
    virtual void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView *pShaderResourceView) = 0;
    virtual void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, ID3D11Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) = 0;
    virtual void STDMETHODCALLTYPE CopyResource(ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource) = 0;
    virtual void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox) = 0;
    virtual void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Rasterizer / state
    virtual void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports) = 0;
    virtual void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects) = 0;
    virtual void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState *pRasterizerState) = 0;
    virtual void STDMETHODCALLTYPE ClearState(void) = 0;
    virtual void STDMETHODCALLTYPE Flush(void) = 0;

    //////////////////////////////////////////////////////////////////////////
    // Queries / mapping
    virtual HRESULT STDMETHODCALLTYPE Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource) = 0;
    virtual void STDMETHODCALLTYPE Unmap(ID3D11Resource *pResource, UINT Subresource) = 0;
    virtual void STDMETHODCALLTYPE Begin(ID3D11Asynchronous *pAsync) = 0;
    virtual void STDMETHODCALLTYPE End(ID3D11Asynchronous *pAsync) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) = 0;
    virtual D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType(void) = 0;
};

//////////////////////////////////////////////////////////////////////////////
// Shader reflection
//
// ID3D11ShaderReflection derives from IUnknown; the three helper interfaces
// below it do *not* (that is how the real SDK declares them - they are owned by
// the reflection object and must not be released individually).
//////////////////////////////////////////////////////////////////////////////

struct ID3D11ShaderReflectionType
{
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_TYPE_DESC *pDesc) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetMemberTypeByIndex(UINT Index) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetMemberTypeByName(LPCSTR Name) = 0;
    virtual LPCSTR STDMETHODCALLTYPE GetMemberTypeName(UINT Index) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsEqual(ID3D11ShaderReflectionType *pType) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetSubType(void) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetBaseClass(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetNumInterfaces(void) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetInterfaceByIndex(UINT uIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsOfType(ID3D11ShaderReflectionType *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE ImplementsInterface(ID3D11ShaderReflectionType *pBase) = 0;
};

struct ID3D11ShaderReflectionVariable
{
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_VARIABLE_DESC *pDesc) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetType(void) = 0;
    virtual ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetBuffer(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetInterfaceSlot(UINT uArrayIndex) = 0;
};

struct ID3D11ShaderReflectionConstantBuffer
{
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_BUFFER_DESC *pDesc) = 0;
    virtual ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByIndex(UINT Index) = 0;
    virtual ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) = 0;
    virtual ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetType(void) = 0;
};

struct ID3D11ShaderReflection : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_DESC *pDesc) = 0;
    virtual ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetConstantBufferByIndex(UINT Index) = 0;
    virtual ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR Name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT ResourceIndex, D3D11_SHADER_INPUT_BIND_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC *pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC *pDesc) = 0;
    virtual ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR Name, D3D11_SHADER_INPUT_BIND_DESC *pDesc) = 0;
    virtual UINT STDMETHODCALLTYPE GetMovInstructionCount(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetMovcInstructionCount(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetConversionInstructionCount(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetBitwiseInstructionCount(void) = 0;
    virtual D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive(void) = 0;
    virtual BOOL STDMETHODCALLTYPE IsSampleFrequencyShader(void) = 0;
    virtual UINT STDMETHODCALLTYPE GetNumInterfaceSlots(void) = 0;
};

// Stub: nothing in the reused code inspects function-level reflection yet, but
// the type has to exist for D3DReflect-era code and for IID lookups.
struct ID3D11FunctionReflection
{
};

// ---------------------------------------------------------------------------
// Debug layer (d3d11sdklayers.h)
//
// CCoreDX11Device::~CCoreDX11Device() queries the device for ID3D11Debug and
// dumps live objects in debug builds.
// ---------------------------------------------------------------------------

struct ID3D11Debug : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetFeatureMask(UINT Mask) = 0;
    virtual UINT STDMETHODCALLTYPE GetFeatureMask(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPresentPerRenderOpDelay(UINT Milliseconds) = 0;
    virtual UINT STDMETHODCALLTYPE GetPresentPerRenderOpDelay(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain *pSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSwapChain(IDXGISwapChain **ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReportLiveDeviceObjects(D3D11_RLDO_FLAGS Flags) = 0;
};

//////////////////////////////////////////////////////////////////////////////
// IIDs
//
// Real Windows IID values, reachable through the __uuidof macro (see
// D3Dcommon.h).
//////////////////////////////////////////////////////////////////////////////

namespace dsh_iid
{
inline const GUID &id_ID3D11DeviceChild()
{
    static const GUID g = { 0x1841e5c8, 0x16b0, 0x489b, { 0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5, 0xde, 0xae } };
    return g;
}
inline const GUID &id_ID3D11Device()
{
    static const GUID g = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };
    return g;
}
inline const GUID &id_ID3D11DeviceContext()
{
    static const GUID g = { 0xc0bfa96c, 0xe089, 0x44fb, { 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda } };
    return g;
}
inline const GUID &id_ID3D11Resource()
{
    static const GUID g = { 0xdc8e63f3, 0xd12b, 0x4952, { 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d } };
    return g;
}
inline const GUID &id_ID3D11Buffer()
{
    static const GUID g = { 0x48570b85, 0xd1ee, 0x4fcd, { 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37 } };
    return g;
}
inline const GUID &id_ID3D11Texture1D()
{
    static const GUID g = { 0xf8fb5c27, 0xc6b3, 0x4f75, { 0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef, 0x56, 0x4c } };
    return g;
}
inline const GUID &id_ID3D11Texture2D()
{
    static const GUID g = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
    return g;
}
inline const GUID &id_ID3D11Texture3D()
{
    static const GUID g = { 0x037e866e, 0xf56d, 0x4357, { 0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e, 0x25, 0x0e } };
    return g;
}
inline const GUID &id_ID3D11View()
{
    static const GUID g = { 0x839d1216, 0xbb2e, 0x412b, { 0xb7, 0xf4, 0xa9, 0xdb, 0xeb, 0xe0, 0x8e, 0xd1 } };
    return g;
}
inline const GUID &id_ID3D11ShaderResourceView()
{
    static const GUID g = { 0xb0e06fe0, 0x8192, 0x4e1a, { 0xb1, 0xca, 0x36, 0xd7, 0x41, 0x47, 0x10, 0xb2 } };
    return g;
}
inline const GUID &id_ID3D11RenderTargetView()
{
    static const GUID g = { 0xdfdba067, 0x0b8d, 0x4865, { 0x87, 0x5b, 0xd7, 0xb4, 0x51, 0x6c, 0xc1, 0x64 } };
    return g;
}
inline const GUID &id_ID3D11DepthStencilView()
{
    static const GUID g = { 0x9fdac92a, 0x1876, 0x48c3, { 0xaf, 0xad, 0x25, 0xb9, 0x4f, 0x84, 0xa9, 0xb6 } };
    return g;
}
inline const GUID &id_ID3D11UnorderedAccessView()
{
    static const GUID g = { 0x28acf509, 0x7f5c, 0x48f6, { 0x86, 0x11, 0xf3, 0x16, 0x01, 0x0a, 0x63, 0x80 } };
    return g;
}
inline const GUID &id_ID3D11VertexShader()
{
    static const GUID g = { 0x3b301d64, 0xd678, 0x4289, { 0x88, 0x97, 0x22, 0xf8, 0x92, 0x8b, 0x72, 0xf3 } };
    return g;
}
inline const GUID &id_ID3D11HullShader()
{
    static const GUID g = { 0x8e5c6061, 0x628a, 0x4c8e, { 0x82, 0x64, 0xbb, 0xe4, 0x5c, 0xb3, 0xd5, 0xdd } };
    return g;
}
inline const GUID &id_ID3D11DomainShader()
{
    static const GUID g = { 0xf582c508, 0x0f36, 0x490c, { 0x99, 0x77, 0x31, 0xee, 0xce, 0x26, 0x8c, 0xfa } };
    return g;
}
inline const GUID &id_ID3D11GeometryShader()
{
    static const GUID g = { 0x38325b96, 0xeffb, 0x4022, { 0xba, 0x02, 0x2e, 0x79, 0x5b, 0x70, 0x27, 0x5c } };
    return g;
}
inline const GUID &id_ID3D11PixelShader()
{
    static const GUID g = { 0xea82e40d, 0x51dc, 0x4f33, { 0x93, 0xd4, 0xdb, 0x7c, 0x91, 0x25, 0xae, 0x8c } };
    return g;
}
inline const GUID &id_ID3D11ComputeShader()
{
    static const GUID g = { 0x4f5b196e, 0xc2bd, 0x495e, { 0xbd, 0x01, 0x1f, 0xde, 0xd3, 0x8e, 0x49, 0x69 } };
    return g;
}
inline const GUID &id_ID3D11InputLayout()
{
    static const GUID g = { 0xe4819ddc, 0x4cf0, 0x4025, { 0xbd, 0x26, 0x5d, 0xe8, 0x2a, 0x3e, 0x07, 0xb7 } };
    return g;
}
inline const GUID &id_ID3D11SamplerState()
{
    static const GUID g = { 0xda6fea51, 0x564c, 0x4487, { 0x98, 0x10, 0xf0, 0xd0, 0xf9, 0xb4, 0xe3, 0xa5 } };
    return g;
}
inline const GUID &id_ID3D11BlendState()
{
    static const GUID g = { 0x75b68faa, 0x347d, 0x4159, { 0x8f, 0x45, 0xa0, 0x64, 0x0f, 0x01, 0xcd, 0x9a } };
    return g;
}
inline const GUID &id_ID3D11DepthStencilState()
{
    static const GUID g = { 0x03823efb, 0x8d8f, 0x4e1c, { 0x9a, 0xa2, 0xf6, 0x4b, 0xb2, 0xcb, 0xfd, 0xf1 } };
    return g;
}
inline const GUID &id_ID3D11RasterizerState()
{
    static const GUID g = { 0x9bb4ab81, 0xab1a, 0x4d8f, { 0xb5, 0x06, 0xfc, 0x04, 0x20, 0x0b, 0x6e, 0xe7 } };
    return g;
}
inline const GUID &id_ID3D11Asynchronous()
{
    static const GUID g = { 0x4b35d0cd, 0x1e15, 0x4258, { 0x9c, 0x98, 0x1b, 0x13, 0x33, 0xf6, 0xdd, 0x3b } };
    return g;
}
inline const GUID &id_ID3D11Query()
{
    static const GUID g = { 0xd6c00747, 0x87b7, 0x425e, { 0xb8, 0x4d, 0x44, 0xd1, 0x08, 0x56, 0x0a, 0xfd } };
    return g;
}
inline const GUID &id_ID3D11Predicate()
{
    static const GUID g = { 0x9eb576dd, 0x9f77, 0x4d86, { 0x81, 0xaa, 0x8b, 0xab, 0x5f, 0xe4, 0x90, 0xe2 } };
    return g;
}
inline const GUID &id_ID3D11Counter()
{
    static const GUID g = { 0x6e8c49fb, 0xa371, 0x4770, { 0xb4, 0x40, 0x29, 0x08, 0x60, 0x22, 0xb7, 0x41 } };
    return g;
}
inline const GUID &id_ID3D11ClassInstance()
{
    static const GUID g = { 0xa6cd7faa, 0xb0b7, 0x4a2f, { 0x94, 0x36, 0x86, 0x62, 0xa6, 0x57, 0x97, 0xcb } };
    return g;
}
inline const GUID &id_ID3D11ClassLinkage()
{
    static const GUID g = { 0xddf57cba, 0x9543, 0x46e4, { 0xa1, 0x2b, 0xf2, 0x07, 0xa0, 0xfe, 0x7f, 0xed } };
    return g;
}
inline const GUID &id_ID3D11CommandList()
{
    static const GUID g = { 0xa24bc4d1, 0x769e, 0x43f7, { 0x80, 0x13, 0x98, 0xff, 0x56, 0x6c, 0x18, 0xe2 } };
    return g;
}
inline const GUID &id_ID3D11ShaderReflection()
{
    static const GUID g = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };
    return g;
}
inline const GUID &id_ID3D11ShaderReflectionType()
{
    static const GUID g = { 0x6e6ffa6a, 0x9bae, 0x4613, { 0xa5, 0x1e, 0x91, 0x65, 0x2d, 0x50, 0x8c, 0x21 } };
    return g;
}
inline const GUID &id_ID3D11ShaderReflectionVariable()
{
    static const GUID g = { 0x51f23923, 0xf3e5, 0x4bd1, { 0x91, 0xcb, 0x60, 0x61, 0x77, 0xd8, 0xdb, 0x4c } };
    return g;
}
inline const GUID &id_ID3D11ShaderReflectionConstantBuffer()
{
    static const GUID g = { 0xeb62d63d, 0x93dd, 0x4318, { 0x8a, 0xe8, 0xc6, 0xf8, 0x3a, 0xd3, 0x71, 0xb8 } };
    return g;
}
inline const GUID &id_ID3D11FunctionReflection()
{
    static const GUID g = { 0x207bcecb, 0xd683, 0x4a06, { 0xa8, 0xa3, 0x9b, 0x14, 0x9b, 0x9f, 0x73, 0xa4 } };
    return g;
}
inline const GUID &id_ID3D11Debug()
{
    static const GUID g = { 0x79cf2233, 0x7536, 0x4948, { 0x9d, 0x36, 0x1e, 0x46, 0x92, 0xdc, 0x57, 0x60 } };
    return g;
}
} // namespace dsh_iid

// The SDK exposes IID_ID3D11ShaderReflection as an extern const IID.  Here it
// is a macro over the shared accessor so the header stays header-only (and so
// it matches whatever __uuidof(ID3D11ShaderReflection) yields).
#ifndef IID_ID3D11ShaderReflection
#define IID_ID3D11ShaderReflection (::dsh_iid::id_ID3D11ShaderReflection())
#endif
#ifndef IID_ID3D11Texture2D
#define IID_ID3D11Texture2D (::dsh_iid::id_ID3D11Texture2D())
#endif
#ifndef IID_ID3D11Debug
#define IID_ID3D11Debug (::dsh_iid::id_ID3D11Debug())
#endif

// Keep the classic D3D9-era name working where the engine compares HRESULTs.
#ifndef D3D_OK
#define D3D_OK S_OK
#endif

//////////////////////////////////////////////////////////////////////////////
// Entry points (implemented by the Vulkan-backed mini-DXVK in src/d3d11/)
//////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter *pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext);

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain,
    ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext);

#ifdef __cplusplus
}
#endif
