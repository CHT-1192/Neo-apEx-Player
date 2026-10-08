// compat/DirectXMath.h
//
// Minimal stand-in for the DirectXMath <DirectXMath.h>.
//
// Only one vendored translation unit includes this header (CoRE2/DX11Texture.cpp,
// in its texture-export block) and it uses no DirectXMath symbol from it - the
// one thing it needs, DirectX::PackedVector::XMConvertHalfToFloatStream, comes
// from <DirectXPackedVector.h>, which the real DirectXMath.h also pulls in.
//
// So this header exists to satisfy the include and to hand on the packed-vector
// helpers.  The XMVECTOR/XMMATRIX SIMD layer, DirectXMathMisc, DirectXColors
// and the rest are deliberately absent: the engine's math comes from
// compat/D3DX10Math.h (Bedrock/Phoenix) and Bedrock/BaseLib.
#pragma once

#include "Windows.h"
#include "DirectXPackedVector.h"

// The scalar float vector containers are cheap and are the most likely next
// thing a caller reaches for, so provide the plain (non-SIMD) forms.
namespace DirectX
{

struct XMFLOAT2
{
    float x, y;
    XMFLOAT2() : x(0), y(0) {}
    XMFLOAT2(float _x, float _y) : x(_x), y(_y) {}
    explicit XMFLOAT2(const float *p) : x(p[0]), y(p[1]) {}
};

struct XMFLOAT3
{
    float x, y, z;
    XMFLOAT3() : x(0), y(0), z(0) {}
    XMFLOAT3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    explicit XMFLOAT3(const float *p) : x(p[0]), y(p[1]), z(p[2]) {}
};

struct XMFLOAT4
{
    float x, y, z, w;
    XMFLOAT4() : x(0), y(0), z(0), w(0) {}
    XMFLOAT4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    explicit XMFLOAT4(const float *p) : x(p[0]), y(p[1]), z(p[2]), w(p[3]) {}
};

} // namespace DirectX
