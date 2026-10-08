// compat/DirectXPackedVector.h
//
// Minimal stand-in for the DirectXMath <DirectXPackedVector.h>.
//
// The apEx port needs exactly one thing out of DirectXMath: the half-float
// unpacking helper DX11Texture.cpp uses when it exports a DDS texture whose
// FourCC is 113 ("D3DFMT_A16B16G16R16F" - four half floats per texel):
//
//     DirectX::PackedVector::XMConvertHalfToFloatStream(
//         img2, 4, (const DirectX::PackedVector::HALF *)Data, 2,
//         head.dwWidth * head.dwHeight * 4);
//
// The stream helpers are real inline implementations (not stubs), so texture
// export genuinely works; the rest of DirectXMath (XMVECTOR/XMMATRIX, the
// SIMD intrinsics, DirectXMathMisc, ...) is deliberately not provided.  Add to
// this file if a future caller needs more.
//
// Semantics and signatures follow the DirectXMath header (MIT licensed,
// (C) Microsoft Corporation): strides are in BYTES.
#pragma once

#include "Windows.h"

#include <cstdint>
#include <cstring>

namespace DirectX
{
namespace PackedVector
{

// A 16-bit IEEE 754-2008 binary16 value, stored as its raw bit pattern.
struct HALF
{
    uint16_t v;
};

inline float XMConvertHalfToFloat(HALF Value)
{
    const uint32_t sign = ((uint32_t)Value.v & 0x8000u) << 16;
    uint32_t exponent = ((uint32_t)Value.v >> 10) & 0x1fu;
    uint32_t mantissa = (uint32_t)Value.v & 0x3ffu;
    uint32_t bits;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            // +/- zero
            bits = sign;
        }
        else
        {
            // Subnormal half -> normal float: renormalise the mantissa.
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 31)
    {
        // Inf / NaN
        bits = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }

    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

inline HALF XMConvertFloatToHalf(float Value)
{
    uint32_t bits;
    memcpy(&bits, &Value, sizeof(bits));

    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    const uint32_t exponent = (bits >> 23) & 0xffu;
    const uint32_t mantissa = bits & 0x7fffffu;

    HALF result;
    if (exponent == 0xffu)
    {
        // Inf / NaN (keep the two quiet NaN payload bits)
        result.v = (uint16_t)(sign | 0x7c00u | (mantissa ? 0x0200u | (mantissa >> 13) : 0u));
    }
    else if (exponent - 127u + 15u >= 0x1fu)
    {
        // Overflow -> Inf
        result.v = (uint16_t)(sign | 0x7c00u);
    }
    else if (exponent - 127u + 15u <= 0u)
    {
        // Underflow -> zero or subnormal
        if (exponent < 113u)
            result.v = sign;
        else
            result.v = (uint16_t)(sign | ((mantissa | 0x800000u) >> (126u - exponent + 13u)));
    }
    else
    {
        result.v = (uint16_t)(sign | ((exponent - 127u + 15u) << 10) | (mantissa >> 13));
    }
    return result;
}

// Convert HalfCount half floats, reading with InputStride bytes per element and
// writing with OutputStride bytes per element.
inline void XMConvertHalfToFloatStream(
    float *pOutputStream,
    size_t OutputStride,
    const HALF *pInputStream,
    size_t InputStride,
    size_t HalfCount)
{
    const uint8_t *pInput = (const uint8_t *)pInputStream;
    uint8_t *pOutput = (uint8_t *)pOutputStream;

    for (size_t i = 0; i < HalfCount; ++i)
    {
        HALF value;
        memcpy(&value, pInput, sizeof(HALF));
        const float converted = XMConvertHalfToFloat(value);
        memcpy(pOutput, &converted, sizeof(float));

        pInput += InputStride;
        pOutput += OutputStride;
    }
}

inline void XMConvertFloatToHalfStream(
    HALF *pOutputStream,
    size_t OutputStride,
    const float *pInputStream,
    size_t InputStride,
    size_t FloatCount)
{
    const uint8_t *pInput = (const uint8_t *)pInputStream;
    uint8_t *pOutput = (uint8_t *)pOutputStream;

    for (size_t i = 0; i < FloatCount; ++i)
    {
        float value;
        memcpy(&value, pInput, sizeof(float));
        const HALF converted = XMConvertFloatToHalf(value);
        memcpy(pOutput, &converted, sizeof(HALF));

        pInput += InputStride;
        pOutput += OutputStride;
    }
}

} // namespace PackedVector
} // namespace DirectX
