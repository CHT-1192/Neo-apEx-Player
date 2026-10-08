// compat/D3Dcompiler.h
//
// Portable stand-in for the Windows SDK <D3Dcompiler.h>.
//
// On Windows the engine loads D3DCompile out of d3dcompiler_47.dll at runtime
// (see CoRE2/DX11Shader.cpp).  In the Vulkan port HLSL is compiled through
// libshaderc instead, but the *declarations* have to stay identical so the
// vendored call sites compile unchanged:
//
//   typedef HRESULT(__stdcall d3d_compile_func)(LPCVOID, SIZE_T, LPCSTR,
//       const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
//       UINT, UINT, ID3DBlob **, ID3DBlob **);
//   D3DCompileFunc(code, strlen(code), NULL, NULL, NULL, entry, version,
//                  0, 0, &PS, &Error);
//
// Declarations only - the implementations are expected from the shader
// compiler shim (libshaderc / shaderc_compiler) behind these entry points.
#pragma once

#include "Windows.h"
#include "dxgiformat.h"
#include "D3Dcommon.h"
#include "d3d11.h"

//////////////////////////////////////////////////////////////////////////////
// D3DCOMPILE_* flags (d3dcompiler.h / d3dcompile.h)
//
// Real SDK values; phxEngine.cpp guards its own fallback definition with
// #ifndef D3DCOMPILE_OPTIMIZATION_LEVEL3, so these must be macros.
//////////////////////////////////////////////////////////////////////////////

#ifndef D3DCOMPILE_DEBUG
#define D3DCOMPILE_DEBUG                                (1 << 0)
#define D3DCOMPILE_SKIP_VALIDATION                      (1 << 1)
#define D3DCOMPILE_SKIP_OPTIMIZATION                    (1 << 2)
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR                (1 << 3)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR             (1 << 4)
#define D3DCOMPILE_PARTIAL_PRECISION                    (1 << 5)
#define D3DCOMPILE_FORCE_VS_SOFTWARE_NO_OPT             (1 << 6)
#define D3DCOMPILE_FORCE_PS_SOFTWARE_NO_OPT             (1 << 7)
#define D3DCOMPILE_NO_PRESHADER                         (1 << 8)
#define D3DCOMPILE_AVOID_FLOW_CONTROL                   (1 << 9)
#define D3DCOMPILE_PREFER_FLOW_CONTROL                  (1 << 10)
#define D3DCOMPILE_ENABLE_STRICTNESS                    (1 << 11)
#define D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY       (1 << 12)
#define D3DCOMPILE_IEEE_STRICTNESS                      (1 << 13)
#define D3DCOMPILE_OPTIMIZATION_LEVEL0                  (1 << 14)
#define D3DCOMPILE_OPTIMIZATION_LEVEL1                  0
#define D3DCOMPILE_OPTIMIZATION_LEVEL2                  ((1 << 14) | (1 << 15))
#define D3DCOMPILE_OPTIMIZATION_LEVEL3                  (1 << 15)
#define D3DCOMPILE_RESERVED16                           (1 << 16)
#define D3DCOMPILE_RESERVED17                           (1 << 17)
#define D3DCOMPILE_WARNINGS_ARE_ERRORS                  (1 << 18)
#define D3DCOMPILE_RESOURCES_MAY_ALIAS                  (1 << 19)
#define D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES   (1 << 20)
#define D3DCOMPILE_ALL_RESOURCES_BOUND                  (1 << 21)
#define D3DCOMPILE_DEBUG_NAME_FOR_SOURCE                (1 << 22)
#define D3DCOMPILE_DEBUG_NAME_FOR_BINARY                (1 << 23)
#endif

#ifndef D3DCOMPILE_EFFECT_CHILD_EFFECT
#define D3DCOMPILE_EFFECT_CHILD_EFFECT                  (1 << 0)
#define D3DCOMPILE_EFFECT_ALLOW_SLOW_OPS                (1 << 1)
#endif

//////////////////////////////////////////////////////////////////////////////
// D3D_DISASM_* flags (D3DDisassemble)
//////////////////////////////////////////////////////////////////////////////

#ifndef D3D_DISASM_ENABLE_COLOR_CODE
#define D3D_DISASM_ENABLE_COLOR_CODE           0x00000001
#define D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS 0x00000002
#define D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING 0x00000004
#define D3D_DISASM_ENABLE_INSTRUCTION_CYCLE    0x00000008
#define D3D_DISASM_DISABLE_DEBUG_INFO          0x00000010
#define D3D_DISASM_ENABLE_INSTRUCTION_OFFSET   0x00000020
#define D3D_DISASM_INSTRUCTION_ONLY            0x00000040
#define D3D_DISASM_PRINT_HEX_LITERALS          0x00000080
#endif

//////////////////////////////////////////////////////////////////////////////
// D3D_SHADER_VARIABLE_FLAGS / D3D_SHADER_INPUT_FLAGS / D3D_SHADER_CBUFFER_FLAGS
// (used by the reflection descriptors)
//////////////////////////////////////////////////////////////////////////////

typedef enum _D3D_SHADER_VARIABLE_FLAGS
{
    D3D_SVF_USERPACKED = 1,
    D3D_SVF_USED = 2,
    D3D_SVF_INTERFACE_POINTER = 4,
    D3D_SVF_INTERFACE_PARAMETER = 8,

    D3D10_SVF_USERPACKED = D3D_SVF_USERPACKED,
    D3D10_SVF_USED = D3D_SVF_USED,
    D3D11_SVF_INTERFACE_POINTER = D3D_SVF_INTERFACE_POINTER,
    D3D11_SVF_INTERFACE_PARAMETER = D3D_SVF_INTERFACE_PARAMETER
} D3D_SHADER_VARIABLE_FLAGS;

typedef enum _D3D_SHADER_INPUT_FLAGS
{
    D3D_SIF_USERPACKED = 0x1,
    D3D_SIF_COMPARISON_SAMPLER = 0x2,
    D3D_SIF_TEXTURE_COMPONENT_0 = 0x4,
    D3D_SIF_TEXTURE_COMPONENT_1 = 0x8,
    D3D_SIF_TEXTURE_COMPONENTS = 0xc,
    D3D_SIF_UNUSED = 0x10,

    D3D10_SIF_USERPACKED = D3D_SIF_USERPACKED,
    D3D10_SIF_COMPARISON_SAMPLER = D3D_SIF_COMPARISON_SAMPLER,
    D3D10_SIF_TEXTURE_COMPONENT_0 = D3D_SIF_TEXTURE_COMPONENT_0,
    D3D10_SIF_TEXTURE_COMPONENT_1 = D3D_SIF_TEXTURE_COMPONENT_1,
    D3D10_SIF_TEXTURE_COMPONENTS = D3D_SIF_TEXTURE_COMPONENTS
} D3D_SHADER_INPUT_FLAGS;

typedef enum _D3D_SHADER_CBUFFER_FLAGS
{
    D3D_CBF_USERPACKED = 1,

    D3D10_CBF_USERPACKED = D3D_CBF_USERPACKED
} D3D_SHADER_CBUFFER_FLAGS;

//////////////////////////////////////////////////////////////////////////////
// Entry points
//////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

// HLSL -> shader bytecode.  On this port the bytecode is SPIR-V produced by
// libshaderc; the blob contract (GetBufferPointer/GetBufferSize) is unchanged.
HRESULT WINAPI D3DCompile(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs);

HRESULT WINAPI D3DCompile2(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    UINT SecondaryDataFlags,
    LPCVOID pSecondaryData,
    SIZE_T SecondaryDataSize,
    ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs);

// Bytecode -> ID3D11ShaderReflection.  pInterface is REFIID so that callers can
// pass IID_ID3D11ShaderReflection (or __uuidof(ID3D11ShaderReflection)).
HRESULT WINAPI D3DReflect(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    REFIID pInterface,
    void **ppReflector);

HRESULT WINAPI D3DDisassemble(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    UINT Flags,
    LPCSTR szComments,
    ID3DBlob **ppDisassembly);

#ifdef __cplusplus
}
#endif
