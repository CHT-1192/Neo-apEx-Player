//////////////////////////////////////////////////////////////////////////////
//
//  src/d3dx/d3dxmath.cpp
//
//  Non-inline part of the portable D3DX10 math replacement for the macOS /
//  Vulkan port of apEx.  Everything that the DirectX SDK shipped inside
//  d3dx10.lib is implemented here; the trivially inline functions live in
//  compat/D3DX10Math.h exactly where the SDK had them in D3DX10math.inl.
//
//  Conventions (identical to the DirectX SDK):
//    * matrices are stored row-major, m[0][0] .. m[3][3] == _11 .. _44,
//    * the vector convention is the row-vector one, v' = v * M, so matrix
//      translation lives in _41, _42, _43 (m[3][0..2]),
//    * quaternions are ordered (x, y, z, w),
//    * D3DXMatrixInverse(pOut, pDet, pM) returns NULL and leaves *pOut
//      untouched when det(M) == 0.
//
//  Algorithm semantics follow the SDK; signatures and the inline bodies are
//  derived from the DirectX SDK's D3DX10Math.h / D3DX10Math.inl,
//  (C) Microsoft Corporation.
//
//////////////////////////////////////////////////////////////////////////////

#include <D3DX10Math.h>

#include <float.h>
#include <math.h>
#include <string.h>
#include <stddef.h>

// This translation unit deliberately does not depend on compat/Windows.h, so
// make sure the two COM status codes it uses exist.
#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

//===========================================================================
// local helpers
//===========================================================================
namespace
{

//---------------------------------------------------------------------------
// 32 -> 16 bit float conversion.
// Round-to-nearest-even, overflow saturates to +/-Infinity, NaN payloads are
// kept in the top 10 mantissa bits.  This is the DirectXMath
// XMConvertFloatToHalf (F16C) algorithm the D3DX10 conversion routines use;
// the bit pattern therefore matches the reference in
// third_party/apEx/Phoenix/float16.cpp.
//---------------------------------------------------------------------------
inline WORD FloatToHalfBits( FLOAT value )
{
    UINT bits;
    memcpy( &bits, &value, sizeof( bits ) );

    const UINT sign = ( bits & 0x80000000u ) >> 16u;
    UINT ival = bits & 0x7FFFFFFFu;

    if( ival > 0x477FE000u )                    // too large for a half
    {
        if( ( ( ival & 0x7F800000u ) == 0x7F800000u ) && ( ival & 0x7FFFFFu ) )
            return (WORD)( sign | 0x7FFFu );    // NaN
        return (WORD)( sign | 0x7C00u );        // +/-Infinity
    }

    if( ival < 0x38800000u )                    // denormalized half (or zero)
    {
        const UINT shift = 113u - ( ival >> 23u );      // 113 == 127 - 14
        if( shift >= 32u )
            ival = 0u;                          // far below the smallest half
        else
            ival = ( 0x800000u | ( ival & 0x7FFFFFu ) ) >> shift;
    }
    else
    {
        ival += 0xC8000000u;                    // rebias the exponent to half
    }

    // round-to-nearest-even on the 13 bits that are about to be dropped
    return (WORD)( sign | ( ( ival + 0x0FFFu + ( ( ival >> 13u ) & 1u ) ) >> 13u ) );
}

//---------------------------------------------------------------------------
// 16 -> 32 bit float conversion (exact for every half bit pattern).
//---------------------------------------------------------------------------
inline FLOAT HalfBitsToFloat( WORD value )
{
    const UINT sign = (UINT)( value & 0x8000u ) << 16;
    UINT exp  = ( value >> 10 ) & 0x1Fu;
    UINT mant = value & 0x3FFu;
    UINT bits;

    if( exp == 0 )
    {
        if( mant == 0 )
        {
            bits = sign;                        // +/- zero
        }
        else
        {
            int shift = 0;                      // normalize the denormal
            while( ( mant & 0x400u ) == 0 )
            {
                mant <<= 1;
                ++shift;
            }
            mant &= 0x3FFu;
            bits = sign | ( (UINT)( 113 - shift ) << 23 ) | ( mant << 13 );
        }
    }
    else if( exp == 31 )
    {
        bits = sign | 0x7F800000u | ( mant << 13 );     // Inf / NaN
    }
    else
    {
        bits = sign | ( ( exp + 112u ) << 23 ) | ( mant << 13 );
    }

    FLOAT result;
    memcpy( &result, &bits, sizeof( result ) );
    return result;
}

//---------------------------------------------------------------------------
// 1/length, or 0 for a degenerate vector (the SDK's normalize behaviour).
//---------------------------------------------------------------------------
inline FLOAT InverseLength( FLOAT length )
{
    return ( length >= FLT_MIN ) ? ( 1.0f / length ) : 0.0f;
}

} // anonymous namespace

//===========================================================================
//
// 16 bit floating point numbers
//
//===========================================================================

D3DXFLOAT16::D3DXFLOAT16( FLOAT f )
{
    value = FloatToHalfBits( f );
}

D3DXFLOAT16& D3DXFLOAT16::operator = ( FLOAT f )
{
    value = FloatToHalfBits( f );
    return *this;
}

D3DXFLOAT16::operator FLOAT () const
{
    return HalfBitsToFloat( value );
}

BOOL D3DXFLOAT16::operator == ( CONST D3DXFLOAT16& f ) const
{
    // At least one is NaN
    if( ( ( value & D3DX_16F_EXP_MASK ) == D3DX_16F_EXP_MASK && ( value & D3DX_16F_FRAC_MASK ) )
        || ( ( f.value & D3DX_16F_EXP_MASK ) == D3DX_16F_EXP_MASK && ( f.value & D3DX_16F_FRAC_MASK ) ) )
        return false;
    // +/- Zero
    else if( ( value & ~D3DX_16F_SIGN_MASK ) == 0 && ( f.value & ~D3DX_16F_SIGN_MASK ) == 0 )
        return true;
    else
        return value == f.value;
}

BOOL D3DXFLOAT16::operator != ( CONST D3DXFLOAT16& f ) const
{
    // At least one is NaN
    if( ( ( value & D3DX_16F_EXP_MASK ) == D3DX_16F_EXP_MASK && ( value & D3DX_16F_FRAC_MASK ) )
        || ( ( f.value & D3DX_16F_EXP_MASK ) == D3DX_16F_EXP_MASK && ( f.value & D3DX_16F_FRAC_MASK ) ) )
        return true;
    // +/- Zero
    else if( ( value & ~D3DX_16F_SIGN_MASK ) == 0 && ( f.value & ~D3DX_16F_SIGN_MASK ) == 0 )
        return false;
    else
        return value != f.value;
}

D3DXFLOAT16* D3DXFloat32To16Array( D3DXFLOAT16 *pOut, CONST FLOAT *pIn, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        pOut[i].value = FloatToHalfBits( pIn[i] );
    return pOut;
}

FLOAT* D3DXFloat16To32Array( FLOAT *pOut, CONST D3DXFLOAT16 *pIn, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        pOut[i] = HalfBitsToFloat( pIn[i].value );
    return pOut;
}

//===========================================================================
//
// 2D, 3D and 4D vectors
//
//===========================================================================

D3DXVECTOR2* D3DXVec2Normalize( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV )
{
    FLOAT f = InverseLength( D3DXVec2Length( pV ) );
    pOut->x = pV->x * f;
    pOut->y = pV->y * f;
    return pOut;
}

D3DXVECTOR2* D3DXVec2Hermite( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pT1,
                              CONST D3DXVECTOR2 *pV2, CONST D3DXVECTOR2 *pT2, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    const FLOAT h1 =  2.0f * s3 - 3.0f * s2 + 1.0f;
    const FLOAT h2 =         s3 - 2.0f * s2 + s;
    const FLOAT h3 = -2.0f * s3 + 3.0f * s2;
    const FLOAT h4 =         s3 -        s2;

    pOut->x = h1 * pV1->x + h2 * pT1->x + h3 * pV2->x + h4 * pT2->x;
    pOut->y = h1 * pV1->y + h2 * pT1->y + h3 * pV2->y + h4 * pT2->y;
    return pOut;
}

D3DXVECTOR2* D3DXVec2CatmullRom( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV0, CONST D3DXVECTOR2 *pV1,
                                 CONST D3DXVECTOR2 *pV2, CONST D3DXVECTOR2 *pV3, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    pOut->x = 0.5f * ( 2.0f * pV1->x + ( -pV0->x + pV2->x ) * s
                     + ( 2.0f * pV0->x - 5.0f * pV1->x + 4.0f * pV2->x - pV3->x ) * s2
                     + ( -pV0->x + 3.0f * pV1->x - 3.0f * pV2->x + pV3->x ) * s3 );
    pOut->y = 0.5f * ( 2.0f * pV1->y + ( -pV0->y + pV2->y ) * s
                     + ( 2.0f * pV0->y - 5.0f * pV1->y + 4.0f * pV2->y - pV3->y ) * s2
                     + ( -pV0->y + 3.0f * pV1->y - 3.0f * pV2->y + pV3->y ) * s3 );
    return pOut;
}

// Barycentric coordinates: V1 + f*(V2-V1) + g*(V3-V1)
D3DXVECTOR2* D3DXVec2BaryCentric( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2,
                                  CONST D3DXVECTOR2 *pV3, FLOAT f, FLOAT g )
{
    pOut->x = pV1->x + f * ( pV2->x - pV1->x ) + g * ( pV3->x - pV1->x );
    pOut->y = pV1->y + f * ( pV2->y - pV1->y ) + g * ( pV3->y - pV1->y );
    return pOut;
}

// Transform (x, y, 0, 1) by the matrix.
D3DXVECTOR4* D3DXVec2Transform( D3DXVECTOR4 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM )
{
    const D3DXVECTOR4 v( pV->x, pV->y, 0.0f, 1.0f );
    pOut->x = v.x * pM->m[0][0] + v.y * pM->m[1][0] + v.z * pM->m[2][0] + v.w * pM->m[3][0];
    pOut->y = v.x * pM->m[0][1] + v.y * pM->m[1][1] + v.z * pM->m[2][1] + v.w * pM->m[3][1];
    pOut->z = v.x * pM->m[0][2] + v.y * pM->m[1][2] + v.z * pM->m[2][2] + v.w * pM->m[3][2];
    pOut->w = v.x * pM->m[0][3] + v.y * pM->m[1][3] + v.z * pM->m[2][3] + v.w * pM->m[3][3];
    return pOut;
}

// Transform (x, y, 0, 1) by the matrix and project the result back to w == 1.
D3DXVECTOR2* D3DXVec2TransformCoord( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM )
{
    D3DXVECTOR4 v;
    D3DXVec2Transform( &v, pV, pM );
    const FLOAT norm = 1.0f / v.w;
    pOut->x = v.x * norm;
    pOut->y = v.y * norm;
    return pOut;
}

// Transform (x, y, 0, 0) by the matrix.
D3DXVECTOR2* D3DXVec2TransformNormal( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM )
{
    const FLOAT x = pV->x, y = pV->y;
    pOut->x = x * pM->m[0][0] + y * pM->m[1][0];
    pOut->y = x * pM->m[0][1] + y * pM->m[1][1];
    return pOut;
}

D3DXVECTOR4* D3DXVec2TransformArray( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride,
                                     CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec2Transform( (D3DXVECTOR4*)( (unsigned char*)pOut + i * OutStride ),
                           (CONST D3DXVECTOR2*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

D3DXVECTOR2* D3DXVec2TransformCoordArray( D3DXVECTOR2 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride,
                                          CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec2TransformCoord( (D3DXVECTOR2*)( (unsigned char*)pOut + i * OutStride ),
                                (CONST D3DXVECTOR2*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

D3DXVECTOR2* D3DXVec2TransformNormalArray( D3DXVECTOR2 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride,
                                           CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec2TransformNormal( (D3DXVECTOR2*)( (unsigned char*)pOut + i * OutStride ),
                                 (CONST D3DXVECTOR2*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

D3DXVECTOR3* D3DXVec3Normalize( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV )
{
    FLOAT f = InverseLength( D3DXVec3Length( pV ) );
    pOut->x = pV->x * f;
    pOut->y = pV->y * f;
    pOut->z = pV->z * f;
    return pOut;
}

D3DXVECTOR3* D3DXVec3Hermite( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pT1,
                              CONST D3DXVECTOR3 *pV2, CONST D3DXVECTOR3 *pT2, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    const FLOAT h1 =  2.0f * s3 - 3.0f * s2 + 1.0f;
    const FLOAT h2 =         s3 - 2.0f * s2 + s;
    const FLOAT h3 = -2.0f * s3 + 3.0f * s2;
    const FLOAT h4 =         s3 -        s2;

    pOut->x = h1 * pV1->x + h2 * pT1->x + h3 * pV2->x + h4 * pT2->x;
    pOut->y = h1 * pV1->y + h2 * pT1->y + h3 * pV2->y + h4 * pT2->y;
    pOut->z = h1 * pV1->z + h2 * pT1->z + h3 * pV2->z + h4 * pT2->z;
    return pOut;
}

D3DXVECTOR3* D3DXVec3CatmullRom( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV0, CONST D3DXVECTOR3 *pV1,
                                 CONST D3DXVECTOR3 *pV2, CONST D3DXVECTOR3 *pV3, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    pOut->x = 0.5f * ( 2.0f * pV1->x + ( -pV0->x + pV2->x ) * s
                     + ( 2.0f * pV0->x - 5.0f * pV1->x + 4.0f * pV2->x - pV3->x ) * s2
                     + ( -pV0->x + 3.0f * pV1->x - 3.0f * pV2->x + pV3->x ) * s3 );
    pOut->y = 0.5f * ( 2.0f * pV1->y + ( -pV0->y + pV2->y ) * s
                     + ( 2.0f * pV0->y - 5.0f * pV1->y + 4.0f * pV2->y - pV3->y ) * s2
                     + ( -pV0->y + 3.0f * pV1->y - 3.0f * pV2->y + pV3->y ) * s3 );
    pOut->z = 0.5f * ( 2.0f * pV1->z + ( -pV0->z + pV2->z ) * s
                     + ( 2.0f * pV0->z - 5.0f * pV1->z + 4.0f * pV2->z - pV3->z ) * s2
                     + ( -pV0->z + 3.0f * pV1->z - 3.0f * pV2->z + pV3->z ) * s3 );
    return pOut;
}

// Barycentric coordinates: V1 + f*(V2-V1) + g*(V3-V1)
D3DXVECTOR3* D3DXVec3BaryCentric( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2,
                                  CONST D3DXVECTOR3 *pV3, FLOAT f, FLOAT g )
{
    pOut->x = pV1->x + f * ( pV2->x - pV1->x ) + g * ( pV3->x - pV1->x );
    pOut->y = pV1->y + f * ( pV2->y - pV1->y ) + g * ( pV3->y - pV1->y );
    pOut->z = pV1->z + f * ( pV2->z - pV1->z ) + g * ( pV3->z - pV1->z );
    return pOut;
}

// Transform (x, y, z, 1) by the matrix.
D3DXVECTOR4* D3DXVec3Transform( D3DXVECTOR4 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM )
{
    const D3DXVECTOR4 v( pV->x, pV->y, pV->z, 1.0f );
    pOut->x = v.x * pM->m[0][0] + v.y * pM->m[1][0] + v.z * pM->m[2][0] + v.w * pM->m[3][0];
    pOut->y = v.x * pM->m[0][1] + v.y * pM->m[1][1] + v.z * pM->m[2][1] + v.w * pM->m[3][1];
    pOut->z = v.x * pM->m[0][2] + v.y * pM->m[1][2] + v.z * pM->m[2][2] + v.w * pM->m[3][2];
    pOut->w = v.x * pM->m[0][3] + v.y * pM->m[1][3] + v.z * pM->m[2][3] + v.w * pM->m[3][3];
    return pOut;
}

// Transform (x, y, z, 1) by the matrix and project the result back to w == 1.
D3DXVECTOR3* D3DXVec3TransformCoord( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM )
{
    D3DXVECTOR4 v;
    D3DXVec3Transform( &v, pV, pM );
    const FLOAT norm = 1.0f / v.w;
    pOut->x = v.x * norm;
    pOut->y = v.y * norm;
    pOut->z = v.z * norm;
    return pOut;
}

// Transform (x, y, z, 0) by the matrix.
D3DXVECTOR3* D3DXVec3TransformNormal( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM )
{
    const FLOAT x = pV->x, y = pV->y, z = pV->z;
    pOut->x = x * pM->m[0][0] + y * pM->m[1][0] + z * pM->m[2][0];
    pOut->y = x * pM->m[0][1] + y * pM->m[1][1] + z * pM->m[2][1];
    pOut->z = x * pM->m[0][2] + y * pM->m[1][2] + z * pM->m[2][2];
    return pOut;
}

D3DXVECTOR4* D3DXVec3TransformArray( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride,
                                     CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec3Transform( (D3DXVECTOR4*)( (unsigned char*)pOut + i * OutStride ),
                           (CONST D3DXVECTOR3*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

D3DXVECTOR3* D3DXVec3TransformCoordArray( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride,
                                          CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec3TransformCoord( (D3DXVECTOR3*)( (unsigned char*)pOut + i * OutStride ),
                                (CONST D3DXVECTOR3*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

D3DXVECTOR3* D3DXVec3TransformNormalArray( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride,
                                           CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec3TransformNormal( (D3DXVECTOR3*)( (unsigned char*)pOut + i * OutStride ),
                                 (CONST D3DXVECTOR3*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

//---------------------------------------------------------------------------
// Viewport <-> clip space helpers shared by Project/Unproject.
//---------------------------------------------------------------------------
namespace
{

// Matrix that maps normalized device coordinates to viewport coordinates,
// using the D3D depth range [MinDepth, MaxDepth].
inline void MakeViewportMatrix( D3DXMATRIX *pOut, CONST D3D10_VIEWPORT *pViewport )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] =  (FLOAT)pViewport->Width  / 2.0f;
    pOut->m[1][1] = -(FLOAT)pViewport->Height / 2.0f;
    pOut->m[2][2] =  pViewport->MaxDepth - pViewport->MinDepth;
    pOut->m[3][0] =  (FLOAT)pViewport->TopLeftX + (FLOAT)pViewport->Width  / 2.0f;
    pOut->m[3][1] =  (FLOAT)pViewport->TopLeftY + (FLOAT)pViewport->Height / 2.0f;
    pOut->m[3][2] =  pViewport->MinDepth;
    pOut->m[3][3] =  1.0f;
}

} // anonymous namespace

// Project a world space position into viewport space.
D3DXVECTOR3* D3DXVec3Project( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3D10_VIEWPORT *pViewport,
                              CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld )
{
    D3DXMATRIX m, viewport;
    D3DXVECTOR4 v;

    D3DXMatrixMultiply( &m, pWorld, pView );
    D3DXMatrixMultiply( &m, &m, pProjection );
    MakeViewportMatrix( &viewport, pViewport );
    D3DXMatrixMultiply( &m, &m, &viewport );

    D3DXVec3Transform( &v, pV, &m );

    const FLOAT norm = 1.0f / v.w;
    pOut->x = v.x * norm;
    pOut->y = v.y * norm;
    pOut->z = v.z * norm;
    return pOut;
}

// Map a viewport space position back into world space.
D3DXVECTOR3* D3DXVec3Unproject( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3D10_VIEWPORT *pViewport,
                                CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld )
{
    D3DXMATRIX m, viewport;
    D3DXVECTOR4 v;

    D3DXMatrixMultiply( &m, pWorld, pView );
    D3DXMatrixMultiply( &m, &m, pProjection );
    MakeViewportMatrix( &viewport, pViewport );
    D3DXMatrixMultiply( &m, &m, &viewport );

    D3DXMatrixInverse( &m, NULL, &m );

    D3DXVec3Transform( &v, pV, &m );

    const FLOAT norm = 1.0f / v.w;
    pOut->x = v.x * norm;
    pOut->y = v.y * norm;
    pOut->z = v.z * norm;
    return pOut;
}

D3DXVECTOR3* D3DXVec3ProjectArray( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride,
                                   CONST D3D10_VIEWPORT *pViewport, CONST D3DXMATRIX *pProjection,
                                   CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec3Project( (D3DXVECTOR3*)( (unsigned char*)pOut + i * OutStride ),
                         (CONST D3DXVECTOR3*)( (CONST unsigned char*)pV + i * VStride ),
                         pViewport, pProjection, pView, pWorld );
    return pOut;
}

D3DXVECTOR3* D3DXVec3UnprojectArray( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride,
                                     CONST D3D10_VIEWPORT *pViewport, CONST D3DXMATRIX *pProjection,
                                     CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec3Unproject( (D3DXVECTOR3*)( (unsigned char*)pOut + i * OutStride ),
                           (CONST D3DXVECTOR3*)( (CONST unsigned char*)pV + i * VStride ),
                           pViewport, pProjection, pView, pWorld );
    return pOut;
}

// 4D cross product of three vectors (the SDK's D3DXVec4Cross).
D3DXVECTOR4* D3DXVec4Cross( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
                            CONST D3DXVECTOR4 *pV3 )
{
    const FLOAT a = pV2->z * pV3->w - pV2->w * pV3->z;
    const FLOAT b = pV2->y * pV3->w - pV2->w * pV3->y;
    const FLOAT c = pV2->y * pV3->z - pV2->z * pV3->y;
    const FLOAT d = pV2->x * pV3->w - pV2->w * pV3->x;
    const FLOAT e = pV2->x * pV3->z - pV2->z * pV3->x;
    const FLOAT f = pV2->x * pV3->y - pV2->y * pV3->x;

    const FLOAT x =  pV1->y * a - pV1->z * b + pV1->w * c;
    const FLOAT y = -pV1->x * a + pV1->z * d - pV1->w * e;
    const FLOAT z =  pV1->x * b - pV1->y * d + pV1->w * f;
    const FLOAT w = -pV1->x * c + pV1->y * e - pV1->z * f;

    pOut->x = x;
    pOut->y = y;
    pOut->z = z;
    pOut->w = w;
    return pOut;
}

D3DXVECTOR4* D3DXVec4Normalize( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV )
{
    FLOAT f = InverseLength( D3DXVec4Length( pV ) );
    pOut->x = pV->x * f;
    pOut->y = pV->y * f;
    pOut->z = pV->z * f;
    pOut->w = pV->w * f;
    return pOut;
}

D3DXVECTOR4* D3DXVec4Hermite( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pT1,
                              CONST D3DXVECTOR4 *pV2, CONST D3DXVECTOR4 *pT2, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    const FLOAT h1 =  2.0f * s3 - 3.0f * s2 + 1.0f;
    const FLOAT h2 =         s3 - 2.0f * s2 + s;
    const FLOAT h3 = -2.0f * s3 + 3.0f * s2;
    const FLOAT h4 =         s3 -        s2;

    pOut->x = h1 * pV1->x + h2 * pT1->x + h3 * pV2->x + h4 * pT2->x;
    pOut->y = h1 * pV1->y + h2 * pT1->y + h3 * pV2->y + h4 * pT2->y;
    pOut->z = h1 * pV1->z + h2 * pT1->z + h3 * pV2->z + h4 * pT2->z;
    pOut->w = h1 * pV1->w + h2 * pT1->w + h3 * pV2->w + h4 * pT2->w;
    return pOut;
}

D3DXVECTOR4* D3DXVec4CatmullRom( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV0, CONST D3DXVECTOR4 *pV1,
                                 CONST D3DXVECTOR4 *pV2, CONST D3DXVECTOR4 *pV3, FLOAT s )
{
    const FLOAT s2 = s * s;
    const FLOAT s3 = s * s2;

    pOut->x = 0.5f * ( 2.0f * pV1->x + ( -pV0->x + pV2->x ) * s
                     + ( 2.0f * pV0->x - 5.0f * pV1->x + 4.0f * pV2->x - pV3->x ) * s2
                     + ( -pV0->x + 3.0f * pV1->x - 3.0f * pV2->x + pV3->x ) * s3 );
    pOut->y = 0.5f * ( 2.0f * pV1->y + ( -pV0->y + pV2->y ) * s
                     + ( 2.0f * pV0->y - 5.0f * pV1->y + 4.0f * pV2->y - pV3->y ) * s2
                     + ( -pV0->y + 3.0f * pV1->y - 3.0f * pV2->y + pV3->y ) * s3 );
    pOut->z = 0.5f * ( 2.0f * pV1->z + ( -pV0->z + pV2->z ) * s
                     + ( 2.0f * pV0->z - 5.0f * pV1->z + 4.0f * pV2->z - pV3->z ) * s2
                     + ( -pV0->z + 3.0f * pV1->z - 3.0f * pV2->z + pV3->z ) * s3 );
    pOut->w = 0.5f * ( 2.0f * pV1->w + ( -pV0->w + pV2->w ) * s
                     + ( 2.0f * pV0->w - 5.0f * pV1->w + 4.0f * pV2->w - pV3->w ) * s2
                     + ( -pV0->w + 3.0f * pV1->w - 3.0f * pV2->w + pV3->w ) * s3 );
    return pOut;
}

D3DXVECTOR4* D3DXVec4BaryCentric( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
                                  CONST D3DXVECTOR4 *pV3, FLOAT f, FLOAT g )
{
    pOut->x = pV1->x + f * ( pV2->x - pV1->x ) + g * ( pV3->x - pV1->x );
    pOut->y = pV1->y + f * ( pV2->y - pV1->y ) + g * ( pV3->y - pV1->y );
    pOut->z = pV1->z + f * ( pV2->z - pV1->z ) + g * ( pV3->z - pV1->z );
    pOut->w = pV1->w + f * ( pV2->w - pV1->w ) + g * ( pV3->w - pV1->w );
    return pOut;
}

D3DXVECTOR4* D3DXVec4Transform( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, CONST D3DXMATRIX *pM )
{
    const D3DXVECTOR4 v = *pV;
    pOut->x = v.x * pM->m[0][0] + v.y * pM->m[1][0] + v.z * pM->m[2][0] + v.w * pM->m[3][0];
    pOut->y = v.x * pM->m[0][1] + v.y * pM->m[1][1] + v.z * pM->m[2][1] + v.w * pM->m[3][1];
    pOut->z = v.x * pM->m[0][2] + v.y * pM->m[1][2] + v.z * pM->m[2][2] + v.w * pM->m[3][2];
    pOut->w = v.x * pM->m[0][3] + v.y * pM->m[1][3] + v.z * pM->m[2][3] + v.w * pM->m[3][3];
    return pOut;
}

D3DXVECTOR4* D3DXVec4TransformArray( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR4 *pV, UINT VStride,
                                     CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXVec4Transform( (D3DXVECTOR4*)( (unsigned char*)pOut + i * OutStride ),
                           (CONST D3DXVECTOR4*)( (CONST unsigned char*)pV + i * VStride ), pM );
    return pOut;
}

//===========================================================================
//
// Matrices
//
//===========================================================================

namespace
{

// Cofactor of m[i][j] (3x3 minor with the sign of the checkerboard).
inline FLOAT Cofactor( CONST D3DXMATRIX *pM, int i, int j )
{
    FLOAT sub[3][3];
    int r = 0;
    for( int rr = 0; rr < 4; ++rr )
    {
        if( rr == i )
            continue;
        int c = 0;
        for( int cc = 0; cc < 4; ++cc )
        {
            if( cc == j )
                continue;
            sub[r][c] = pM->m[rr][cc];
            ++c;
        }
        ++r;
    }

    const FLOAT minor = sub[0][0] * ( sub[1][1] * sub[2][2] - sub[1][2] * sub[2][1] )
                      - sub[0][1] * ( sub[1][0] * sub[2][2] - sub[1][2] * sub[2][0] )
                      + sub[0][2] * ( sub[1][0] * sub[2][1] - sub[1][1] * sub[2][0] );

    return ( ( i + j ) & 1 ) ? -minor : minor;
}

} // anonymous namespace

FLOAT D3DXMatrixDeterminant( CONST D3DXMATRIX *pM )
{
    return pM->m[0][0] * Cofactor( pM, 0, 0 )
         + pM->m[0][1] * Cofactor( pM, 0, 1 )
         + pM->m[0][2] * Cofactor( pM, 0, 2 )
         + pM->m[0][3] * Cofactor( pM, 0, 3 );
}

D3DXMATRIX* D3DXMatrixInverse( D3DXMATRIX *pOut, FLOAT *pDeterminant, CONST D3DXMATRIX *pM )
{
    FLOAT cof[4][4];
    for( int i = 0; i < 4; ++i )
        for( int j = 0; j < 4; ++j )
            cof[i][j] = Cofactor( pM, i, j );

    const FLOAT det = pM->m[0][0] * cof[0][0] + pM->m[0][1] * cof[0][1]
                    + pM->m[0][2] * cof[0][2] + pM->m[0][3] * cof[0][3];

    if( pDeterminant )
        *pDeterminant = det;

    if( det == 0.0f )
        return NULL;                        // singular: *pOut is left alone

    const FLOAT invDet = 1.0f / det;

    D3DXMATRIX m;
    for( int i = 0; i < 4; ++i )
        for( int j = 0; j < 4; ++j )
            m.m[i][j] = cof[j][i] * invDet; // adjugate == transposed cofactors

    *pOut = m;
    return pOut;
}

// Decomposes a matrix into scale, rotation and translation.  Reflections are
// not handled (they cannot be represented by a quaternion), exactly like the
// SDK.  Any of the output pointers may be NULL.
HRESULT D3DXMatrixDecompose( D3DXVECTOR3 *pOutScale, D3DXQUATERNION *pOutRotation,
                             D3DXVECTOR3 *pOutTranslation, CONST D3DXMATRIX *pM )
{
    D3DXMATRIX mat = *pM;

    if( pOutScale )
    {
        pOutScale->x = sqrtf( mat.m[0][0] * mat.m[0][0] + mat.m[0][1] * mat.m[0][1] + mat.m[0][2] * mat.m[0][2] );
        pOutScale->y = sqrtf( mat.m[1][0] * mat.m[1][0] + mat.m[1][1] * mat.m[1][1] + mat.m[1][2] * mat.m[1][2] );
        pOutScale->z = sqrtf( mat.m[2][0] * mat.m[2][0] + mat.m[2][1] * mat.m[2][1] + mat.m[2][2] * mat.m[2][2] );

        if( pOutScale->x != 0.0f )
        {
            const FLOAT i = 1.0f / pOutScale->x;
            mat.m[0][0] *= i; mat.m[0][1] *= i; mat.m[0][2] *= i;
        }
        if( pOutScale->y != 0.0f )
        {
            const FLOAT i = 1.0f / pOutScale->y;
            mat.m[1][0] *= i; mat.m[1][1] *= i; mat.m[1][2] *= i;
        }
        if( pOutScale->z != 0.0f )
        {
            const FLOAT i = 1.0f / pOutScale->z;
            mat.m[2][0] *= i; mat.m[2][1] *= i; mat.m[2][2] *= i;
        }
    }

    if( pOutRotation )
        D3DXQuaternionRotationMatrix( pOutRotation, &mat );

    if( pOutTranslation )
    {
        pOutTranslation->x = mat.m[3][0];
        pOutTranslation->y = mat.m[3][1];
        pOutTranslation->z = mat.m[3][2];
    }

    return S_OK;
}

D3DXMATRIX* D3DXMatrixTranspose( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM )
{
    D3DXMATRIX m;
    for( int i = 0; i < 4; ++i )
        for( int j = 0; j < 4; ++j )
            m.m[i][j] = pM->m[j][i];
    *pOut = m;
    return pOut;
}

D3DXMATRIX* D3DXMatrixMultiply( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2 )
{
    D3DXMATRIX m;
    for( int i = 0; i < 4; ++i )
    {
        for( int j = 0; j < 4; ++j )
        {
            m.m[i][j] = pM1->m[i][0] * pM2->m[0][j]
                      + pM1->m[i][1] * pM2->m[1][j]
                      + pM1->m[i][2] * pM2->m[2][j]
                      + pM1->m[i][3] * pM2->m[3][j];
        }
    }
    *pOut = m;
    return pOut;
}

D3DXMATRIX* D3DXMatrixMultiplyTranspose( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2 )
{
    D3DXMATRIX m;
    D3DXMatrixMultiply( &m, pM1, pM2 );
    return D3DXMatrixTranspose( pOut, &m );
}

D3DXMATRIX* D3DXMatrixScaling( D3DXMATRIX *pOut, FLOAT sx, FLOAT sy, FLOAT sz )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = sx;
    pOut->m[1][1] = sy;
    pOut->m[2][2] = sz;
    return pOut;
}

D3DXMATRIX* D3DXMatrixTranslation( D3DXMATRIX *pOut, FLOAT x, FLOAT y, FLOAT z )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[3][0] = x;
    pOut->m[3][1] = y;
    pOut->m[3][2] = z;
    return pOut;
}

D3DXMATRIX* D3DXMatrixRotationX( D3DXMATRIX *pOut, FLOAT Angle )
{
    const FLOAT s = sinf( Angle );
    const FLOAT c = cosf( Angle );

    D3DXMatrixIdentity( pOut );
    pOut->m[1][1] = c;
    pOut->m[1][2] = s;
    pOut->m[2][1] = -s;
    pOut->m[2][2] = c;
    return pOut;
}

D3DXMATRIX* D3DXMatrixRotationY( D3DXMATRIX *pOut, FLOAT Angle )
{
    const FLOAT s = sinf( Angle );
    const FLOAT c = cosf( Angle );

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = c;
    pOut->m[0][2] = -s;
    pOut->m[2][0] = s;
    pOut->m[2][2] = c;
    return pOut;
}

D3DXMATRIX* D3DXMatrixRotationZ( D3DXMATRIX *pOut, FLOAT Angle )
{
    const FLOAT s = sinf( Angle );
    const FLOAT c = cosf( Angle );

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = c;
    pOut->m[0][1] = s;
    pOut->m[1][0] = -s;
    pOut->m[1][1] = c;
    return pOut;
}

D3DXMATRIX* D3DXMatrixRotationAxis( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pV, FLOAT Angle )
{
    // The SDK builds this from the axis/angle quaternion, which normalizes
    // the axis first.
    D3DXQUATERNION q;
    D3DXQuaternionRotationAxis( &q, pV, Angle );
    return D3DXMatrixRotationQuaternion( pOut, &q );
}

D3DXMATRIX* D3DXMatrixRotationQuaternion( D3DXMATRIX *pOut, CONST D3DXQUATERNION *pQ )
{
    const FLOAT xx = pQ->x * pQ->x, yy = pQ->y * pQ->y, zz = pQ->z * pQ->z;
    const FLOAT xy = pQ->x * pQ->y, xz = pQ->x * pQ->z, yz = pQ->y * pQ->z;
    const FLOAT xw = pQ->x * pQ->w, yw = pQ->y * pQ->w, zw = pQ->z * pQ->w;

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 1.0f - 2.0f * ( yy + zz );
    pOut->m[0][1] =        2.0f * ( xy + zw );
    pOut->m[0][2] =        2.0f * ( xz - yw );
    pOut->m[1][0] =        2.0f * ( xy - zw );
    pOut->m[1][1] = 1.0f - 2.0f * ( xx + zz );
    pOut->m[1][2] =        2.0f * ( yz + xw );
    pOut->m[2][0] =        2.0f * ( xz + yw );
    pOut->m[2][1] =        2.0f * ( yz - xw );
    pOut->m[2][2] = 1.0f - 2.0f * ( xx + yy );
    return pOut;
}

D3DXMATRIX* D3DXMatrixRotationYawPitchRoll( D3DXMATRIX *pOut, FLOAT Yaw, FLOAT Pitch, FLOAT Roll )
{
    D3DXQUATERNION q;
    D3DXQuaternionRotationYawPitchRoll( &q, Yaw, Pitch, Roll );
    return D3DXMatrixRotationQuaternion( pOut, &q );
}

D3DXMATRIX* D3DXMatrixTransformation( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pScalingCenter,
                                      CONST D3DXQUATERNION *pScalingRotation, CONST D3DXVECTOR3 *pScaling,
                                      CONST D3DXVECTOR3 *pRotationCenter, CONST D3DXQUATERNION *pRotation,
                                      CONST D3DXVECTOR3 *pTranslation )
{
    D3DXMATRIX m, s;

    D3DXMatrixIdentity( &s );

    if( pScalingCenter )
    {
        D3DXMatrixTranslation( &m, -pScalingCenter->x, -pScalingCenter->y, -pScalingCenter->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pScalingRotation )
    {
        D3DXMatrixRotationQuaternion( &m, pScalingRotation );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pScaling )
    {
        D3DXMatrixScaling( &m, pScaling->x, pScaling->y, pScaling->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pScalingCenter && ( pScaling || pScalingRotation ) )
    {
        D3DXMatrixTranslation( &m, pScalingCenter->x, pScalingCenter->y, pScalingCenter->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    if( pRotationCenter )
    {
        D3DXMatrixTranslation( &m, -pRotationCenter->x, -pRotationCenter->y, -pRotationCenter->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pRotation )
    {
        D3DXMatrixRotationQuaternion( &m, pRotation );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pRotationCenter && pRotation )
    {
        D3DXMatrixTranslation( &m, pRotationCenter->x, pRotationCenter->y, pRotationCenter->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    if( pTranslation )
    {
        D3DXMatrixTranslation( &m, pTranslation->x, pTranslation->y, pTranslation->z );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    *pOut = s;
    return pOut;
}

D3DXMATRIX* D3DXMatrixTransformation2D( D3DXMATRIX *pOut, CONST D3DXVECTOR2 *pScalingCenter,
                                        FLOAT ScalingRotation, CONST D3DXVECTOR2 *pScaling,
                                        CONST D3DXVECTOR2 *pRotationCenter, FLOAT Rotation,
                                        CONST D3DXVECTOR2 *pTranslation )
{
    D3DXMATRIX m, s;

    D3DXMatrixIdentity( &s );

    if( pScalingCenter )
    {
        D3DXMatrixTranslation( &m, -pScalingCenter->x, -pScalingCenter->y, 0.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( ScalingRotation != 0.0f )
    {
        D3DXMatrixRotationZ( &m, ScalingRotation );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pScaling )
    {
        D3DXMatrixScaling( &m, pScaling->x, pScaling->y, 1.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pScalingCenter && ( pScaling || ScalingRotation != 0.0f ) )
    {
        D3DXMatrixTranslation( &m, pScalingCenter->x, pScalingCenter->y, 0.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    if( pRotationCenter )
    {
        D3DXMatrixTranslation( &m, -pRotationCenter->x, -pRotationCenter->y, 0.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( Rotation != 0.0f )
    {
        D3DXMatrixRotationZ( &m, Rotation );
        D3DXMatrixMultiply( &s, &s, &m );
    }
    if( pRotationCenter && Rotation != 0.0f )
    {
        D3DXMatrixTranslation( &m, pRotationCenter->x, pRotationCenter->y, 0.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    if( pTranslation )
    {
        D3DXMatrixTranslation( &m, pTranslation->x, pTranslation->y, 0.0f );
        D3DXMatrixMultiply( &s, &s, &m );
    }

    *pOut = s;
    return pOut;
}

D3DXMATRIX* D3DXMatrixAffineTransformation( D3DXMATRIX *pOut, FLOAT Scaling,
                                            CONST D3DXVECTOR3 *pRotationCenter,
                                            CONST D3DXQUATERNION *pRotation,
                                            CONST D3DXVECTOR3 *pTranslation )
{
    const D3DXVECTOR3 scaling( Scaling, Scaling, Scaling );
    return D3DXMatrixTransformation( pOut, NULL, NULL, &scaling, pRotationCenter, pRotation, pTranslation );
}

D3DXMATRIX* D3DXMatrixAffineTransformation2D( D3DXMATRIX *pOut, FLOAT Scaling,
                                              CONST D3DXVECTOR2 *pRotationCenter, FLOAT Rotation,
                                              CONST D3DXVECTOR2 *pTranslation )
{
    const D3DXVECTOR2 scaling( Scaling, Scaling );
    return D3DXMatrixTransformation2D( pOut, NULL, 0.0f, &scaling, pRotationCenter, Rotation, pTranslation );
}

D3DXMATRIX* D3DXMatrixLookAtRH( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pEye, CONST D3DXVECTOR3 *pAt,
                                CONST D3DXVECTOR3 *pUp )
{
    D3DXVECTOR3 xaxis, yaxis, zaxis;
    zaxis = *pEye - *pAt;                       // right handed: camera looks down -Z
    D3DXVec3Normalize( &zaxis, &zaxis );
    D3DXVec3Cross( &xaxis, pUp, &zaxis );
    D3DXVec3Normalize( &xaxis, &xaxis );
    D3DXVec3Cross( &yaxis, &zaxis, &xaxis );

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = xaxis.x; pOut->m[0][1] = yaxis.x; pOut->m[0][2] = zaxis.x;
    pOut->m[1][0] = xaxis.y; pOut->m[1][1] = yaxis.y; pOut->m[1][2] = zaxis.y;
    pOut->m[2][0] = xaxis.z; pOut->m[2][1] = yaxis.z; pOut->m[2][2] = zaxis.z;
    pOut->m[3][0] = -D3DXVec3Dot( &xaxis, pEye );
    pOut->m[3][1] = -D3DXVec3Dot( &yaxis, pEye );
    pOut->m[3][2] = -D3DXVec3Dot( &zaxis, pEye );
    return pOut;
}

D3DXMATRIX* D3DXMatrixLookAtLH( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pEye, CONST D3DXVECTOR3 *pAt,
                                CONST D3DXVECTOR3 *pUp )
{
    D3DXVECTOR3 xaxis, yaxis, zaxis;
    zaxis = *pAt - *pEye;                       // left handed: camera looks down +Z
    D3DXVec3Normalize( &zaxis, &zaxis );
    D3DXVec3Cross( &xaxis, pUp, &zaxis );
    D3DXVec3Normalize( &xaxis, &xaxis );
    D3DXVec3Cross( &yaxis, &zaxis, &xaxis );

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = xaxis.x; pOut->m[0][1] = yaxis.x; pOut->m[0][2] = zaxis.x;
    pOut->m[1][0] = xaxis.y; pOut->m[1][1] = yaxis.y; pOut->m[1][2] = zaxis.y;
    pOut->m[2][0] = xaxis.z; pOut->m[2][1] = yaxis.z; pOut->m[2][2] = zaxis.z;
    pOut->m[3][0] = -D3DXVec3Dot( &xaxis, pEye );
    pOut->m[3][1] = -D3DXVec3Dot( &yaxis, pEye );
    pOut->m[3][2] = -D3DXVec3Dot( &zaxis, pEye );
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveRH( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f * zn / w;
    pOut->m[1][1] = 2.0f * zn / h;
    pOut->m[2][2] = zf / ( zn - zf );
    pOut->m[2][3] = -1.0f;
    pOut->m[3][2] = zn * zf / ( zn - zf );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveLH( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f * zn / w;
    pOut->m[1][1] = 2.0f * zn / h;
    pOut->m[2][2] = zf / ( zf - zn );
    pOut->m[2][3] = 1.0f;
    pOut->m[3][2] = -zn * zf / ( zf - zn );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveFovRH( D3DXMATRIX *pOut, FLOAT fovy, FLOAT Aspect, FLOAT zn, FLOAT zf )
{
    const FLOAT h = 1.0f / tanf( fovy * 0.5f );
    const FLOAT w = h / Aspect;

    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = w;
    pOut->m[1][1] = h;
    pOut->m[2][2] = zf / ( zn - zf );
    pOut->m[2][3] = -1.0f;
    pOut->m[3][2] = zn * zf / ( zn - zf );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveFovLH( D3DXMATRIX *pOut, FLOAT fovy, FLOAT Aspect, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    const FLOAT h = 1.0f / tanf( fovy * 0.5f );
    const FLOAT w = h / Aspect;
    pOut->m[0][0] = w;
    pOut->m[1][1] = h;
    pOut->m[2][2] = zf / ( zf - zn );
    pOut->m[2][3] = 1.0f;
    pOut->m[3][2] = -zn * zf / ( zf - zn );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveOffCenterRH( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t,
                                              FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f * zn / ( r - l );
    pOut->m[1][1] = 2.0f * zn / ( t - b );
    pOut->m[2][0] = ( l + r ) / ( r - l );
    pOut->m[2][1] = ( t + b ) / ( t - b );
    pOut->m[2][2] = zf / ( zn - zf );
    pOut->m[2][3] = -1.0f;
    pOut->m[3][2] = zn * zf / ( zn - zf );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixPerspectiveOffCenterLH( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t,
                                              FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f * zn / ( r - l );
    pOut->m[1][1] = 2.0f * zn / ( t - b );
    pOut->m[2][0] = -( l + r ) / ( r - l );
    pOut->m[2][1] = -( t + b ) / ( t - b );
    pOut->m[2][2] = zf / ( zf - zn );
    pOut->m[2][3] = 1.0f;
    pOut->m[3][2] = -zn * zf / ( zf - zn );
    pOut->m[3][3] = 0.0f;
    return pOut;
}

D3DXMATRIX* D3DXMatrixOrthoRH( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f / w;
    pOut->m[1][1] = 2.0f / h;
    pOut->m[2][2] = 1.0f / ( zn - zf );
    pOut->m[3][2] = zn / ( zn - zf );
    return pOut;
}

D3DXMATRIX* D3DXMatrixOrthoLH( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f / w;
    pOut->m[1][1] = 2.0f / h;
    pOut->m[2][2] = 1.0f / ( zf - zn );
    pOut->m[3][2] = -zn / ( zf - zn );
    return pOut;
}

D3DXMATRIX* D3DXMatrixOrthoOffCenterRH( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t,
                                        FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f / ( r - l );
    pOut->m[1][1] = 2.0f / ( t - b );
    pOut->m[2][2] = 1.0f / ( zn - zf );
    pOut->m[3][0] = ( l + r ) / ( l - r );
    pOut->m[3][1] = ( t + b ) / ( b - t );
    pOut->m[3][2] = zn / ( zn - zf );
    return pOut;
}

D3DXMATRIX* D3DXMatrixOrthoOffCenterLH( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t,
                                        FLOAT zn, FLOAT zf )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 2.0f / ( r - l );
    pOut->m[1][1] = 2.0f / ( t - b );
    pOut->m[2][2] = 1.0f / ( zf - zn );
    pOut->m[3][0] = ( l + r ) / ( l - r );
    pOut->m[3][1] = ( t + b ) / ( b - t );
    pOut->m[3][2] = -zn / ( zf - zn );
    return pOut;
}

// Projection onto the plane pPlane as seen from the point pLight.
// D3DX uses the documented formula
//     P = normalize(pPlane), L = pLight, d = -dot(P, L)
//     m[i][j] = P[i] * L[j] + (i == j ? d : 0)
// (the result is only defined up to a scale factor, so the projective divide
// by w removes it).  A light w of 0 is a directional light.
D3DXMATRIX* D3DXMatrixShadow( D3DXMATRIX *pOut, CONST D3DXVECTOR4 *pLight, CONST D3DXPLANE *pPlane )
{
    D3DXPLANE plane;
    D3DXPlaneNormalize( &plane, pPlane );

    const FLOAT L[4] = { pLight->x, pLight->y, pLight->z, pLight->w };
    const FLOAT P[4] = { plane.a, plane.b, plane.c, plane.d };
    const FLOAT d = -( P[0] * L[0] + P[1] * L[1] + P[2] * L[2] + P[3] * L[3] );

    for( int i = 0; i < 4; ++i )
        for( int j = 0; j < 4; ++j )
            pOut->m[i][j] = P[i] * L[j] + ( ( i == j ) ? d : 0.0f );

    return pOut;
}

// Reflection about a plane: p' = p - 2 (n.p + d) n
D3DXMATRIX* D3DXMatrixReflect( D3DXMATRIX *pOut, CONST D3DXPLANE *pPlane )
{
    D3DXMatrixIdentity( pOut );
    pOut->m[0][0] = 1.0f - 2.0f * pPlane->a * pPlane->a;
    pOut->m[0][1] =      - 2.0f * pPlane->a * pPlane->b;
    pOut->m[0][2] =      - 2.0f * pPlane->a * pPlane->c;
    pOut->m[1][0] =      - 2.0f * pPlane->a * pPlane->b;
    pOut->m[1][1] = 1.0f - 2.0f * pPlane->b * pPlane->b;
    pOut->m[1][2] =      - 2.0f * pPlane->b * pPlane->c;
    pOut->m[2][0] =      - 2.0f * pPlane->a * pPlane->c;
    pOut->m[2][1] =      - 2.0f * pPlane->b * pPlane->c;
    pOut->m[2][2] = 1.0f - 2.0f * pPlane->c * pPlane->c;
    pOut->m[3][0] =      - 2.0f * pPlane->a * pPlane->d;
    pOut->m[3][1] =      - 2.0f * pPlane->b * pPlane->d;
    pOut->m[3][2] =      - 2.0f * pPlane->c * pPlane->d;
    return pOut;
}

//===========================================================================
//
// Quaternions
//
//===========================================================================

D3DXQUATERNION* D3DXQuaternionMultiply( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
                                        CONST D3DXQUATERNION *pQ2 )
{
    const D3DXQUATERNION q1 = *pQ1;

    pOut->x = q1.w * pQ2->x + q1.x * pQ2->w + q1.y * pQ2->z - q1.z * pQ2->y;
    pOut->y = q1.w * pQ2->y - q1.x * pQ2->z + q1.y * pQ2->w + q1.z * pQ2->x;
    pOut->z = q1.w * pQ2->z + q1.x * pQ2->y - q1.y * pQ2->x + q1.z * pQ2->w;
    pOut->w = q1.w * pQ2->w - q1.x * pQ2->x - q1.y * pQ2->y - q1.z * pQ2->z;
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionNormalize( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ )
{
    FLOAT f = InverseLength( D3DXQuaternionLength( pQ ) );
    pOut->x = pQ->x * f;
    pOut->y = pQ->y * f;
    pOut->z = pQ->z * f;
    pOut->w = pQ->w * f;
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionInverse( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ )
{
    D3DXQUATERNION q;
    D3DXQuaternionConjugate( &q, pQ );

    const FLOAT l = D3DXQuaternionLengthSq( pQ );
    if( l != 0.0f )
    {
        const FLOAT f = 1.0f / l;
        q.x *= f;
        q.y *= f;
        q.z *= f;
        q.w *= f;
    }

    *pOut = q;
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionLn( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ )
{
    const FLOAT n2 = pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z;

    if( n2 > 0.0f )
    {
        const FLOAT n = sqrtf( n2 );
        const FLOAT theta = atan2f( n, pQ->w );
        const FLOAT s = theta / n;
        pOut->x = pQ->x * s;
        pOut->y = pQ->y * s;
        pOut->z = pQ->z * s;
        pOut->w = 0.5f * logf( pQ->w * pQ->w + n2 );
    }
    else
    {
        pOut->x = 0.0f;
        pOut->y = 0.0f;
        pOut->z = 0.0f;
        pOut->w = ( pQ->w > 0.0f ) ? logf( pQ->w ) : 0.0f;
    }
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionExp( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ )
{
    const FLOAT n = sqrtf( pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z );
    const FLOAT e = expf( pQ->w );
    const FLOAT s = ( n > 0.0f ) ? ( e * sinf( n ) / n ) : 0.0f;

    pOut->x = pQ->x * s;
    pOut->y = pQ->y * s;
    pOut->z = pQ->z * s;
    pOut->w = e * cosf( n );
    return pOut;
}

void D3DXQuaternionToAxisAngle( CONST D3DXQUATERNION *pQ, D3DXVECTOR3 *pAxis, FLOAT *pAngle )
{
    if( pAxis )
    {
        const D3DXVECTOR3 v( pQ->x, pQ->y, pQ->z );
        D3DXVec3Normalize( pAxis, &v );
    }
    if( pAngle )
    {
        FLOAT w = pQ->w;
        if( w < -1.0f )
            w = -1.0f;
        else if( w > 1.0f )
            w = 1.0f;
        *pAngle = 2.0f * acosf( w );
    }
}

D3DXQUATERNION* D3DXQuaternionRotationMatrix( D3DXQUATERNION *pOut, CONST D3DXMATRIX *pM )
{
    // Shepperd's method; the exact inverse of D3DXMatrixRotationQuaternion
    // (row-vector convention).
    const FLOAT m00 = pM->m[0][0], m01 = pM->m[0][1], m02 = pM->m[0][2];
    const FLOAT m10 = pM->m[1][0], m11 = pM->m[1][1], m12 = pM->m[1][2];
    const FLOAT m20 = pM->m[2][0], m21 = pM->m[2][1], m22 = pM->m[2][2];
    const FLOAT trace = m00 + m11 + m22;

    if( trace > 0.0f )
    {
        FLOAT s = sqrtf( trace + 1.0f );
        pOut->w = 0.5f * s;
        s = 0.5f / s;
        pOut->x = ( m12 - m21 ) * s;
        pOut->y = ( m20 - m02 ) * s;
        pOut->z = ( m01 - m10 ) * s;
    }
    else if( m00 > m11 && m00 > m22 )
    {
        FLOAT s = sqrtf( 1.0f + m00 - m11 - m22 );
        pOut->x = 0.5f * s;
        s = 0.5f / s;
        pOut->w = ( m12 - m21 ) * s;
        pOut->y = ( m10 + m01 ) * s;
        pOut->z = ( m20 + m02 ) * s;
    }
    else if( m11 > m22 )
    {
        FLOAT s = sqrtf( 1.0f + m11 - m00 - m22 );
        pOut->y = 0.5f * s;
        s = 0.5f / s;
        pOut->w = ( m20 - m02 ) * s;
        pOut->x = ( m10 + m01 ) * s;
        pOut->z = ( m21 + m12 ) * s;
    }
    else
    {
        FLOAT s = sqrtf( 1.0f + m22 - m00 - m11 );
        pOut->z = 0.5f * s;
        s = 0.5f / s;
        pOut->w = ( m01 - m10 ) * s;
        pOut->x = ( m20 + m02 ) * s;
        pOut->y = ( m21 + m12 ) * s;
    }
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionRotationAxis( D3DXQUATERNION *pOut, CONST D3DXVECTOR3 *pV, FLOAT Angle )
{
    D3DXVECTOR3 v;
    D3DXVec3Normalize( &v, pV );

    const FLOAT s = sinf( Angle * 0.5f );
    pOut->x = v.x * s;
    pOut->y = v.y * s;
    pOut->z = v.z * s;
    pOut->w = cosf( Angle * 0.5f );
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionRotationYawPitchRoll( D3DXQUATERNION *pOut, FLOAT Yaw, FLOAT Pitch, FLOAT Roll )
{
    // q = qy(yaw) * qx(pitch) * qz(roll): roll is applied first, the same
    // order as D3DXMatrixRotationYawPitchRoll.
    const FLOAT sy = sinf( Yaw   * 0.5f ), cy = cosf( Yaw   * 0.5f );
    const FLOAT sp = sinf( Pitch * 0.5f ), cp = cosf( Pitch * 0.5f );
    const FLOAT sr = sinf( Roll  * 0.5f ), cr = cosf( Roll  * 0.5f );

    pOut->x = cy * sp * cr + sy * cp * sr;
    pOut->y = sy * cp * cr - cy * sp * sr;
    pOut->z = cy * cp * sr - sy * sp * cr;
    pOut->w = cy * cp * cr + sy * sp * sr;
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionSlerp( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
                                     CONST D3DXQUATERNION *pQ2, FLOAT t )
{
    // Interpolate along the shortest arc, i.e. flip q2 when it points away
    // from q1 (D3DXQuaternionSquadSetup assumes this behaviour for the
    // control points it builds).
    D3DXQUATERNION q2 = *pQ2;
    FLOAT dot = D3DXQuaternionDot( pQ1, &q2 );

    if( dot < 0.0f )
    {
        q2 = -q2;
        dot = -dot;
    }

    if( dot > 1.0f - 1e-6f )            // nearly parallel: linear blend
    {
        pOut->x = pQ1->x + t * ( q2.x - pQ1->x );
        pOut->y = pQ1->y + t * ( q2.y - pQ1->y );
        pOut->z = pQ1->z + t * ( q2.z - pQ1->z );
        pOut->w = pQ1->w + t * ( q2.w - pQ1->w );
    }
    else
    {
        const FLOAT theta = acosf( dot );
        const FLOAT sinTheta = sinf( theta );
        const FLOAT s0 = sinf( ( 1.0f - t ) * theta ) / sinTheta;
        const FLOAT s1 = sinf( t * theta ) / sinTheta;

        pOut->x = s0 * pQ1->x + s1 * q2.x;
        pOut->y = s0 * pQ1->y + s1 * q2.y;
        pOut->z = s0 * pQ1->z + s1 * q2.z;
        pOut->w = s0 * pQ1->w + s1 * q2.w;
    }
    return pOut;
}

D3DXQUATERNION* D3DXQuaternionSquad( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
                                     CONST D3DXQUATERNION *pA, CONST D3DXQUATERNION *pB,
                                     CONST D3DXQUATERNION *pC, FLOAT t )
{
    D3DXQUATERNION q1, q2;
    D3DXQuaternionSlerp( &q1, pQ1, pC, t );
    D3DXQuaternionSlerp( &q2, pA, pB, t );
    return D3DXQuaternionSlerp( pOut, &q1, &q2, 2.0f * t * ( 1.0f - t ) );
}

void D3DXQuaternionSquadSetup( D3DXQUATERNION *pAOut, D3DXQUATERNION *pBOut, D3DXQUATERNION *pCOut,
                               CONST D3DXQUATERNION *pQ0, CONST D3DXQUATERNION *pQ1,
                               CONST D3DXQUATERNION *pQ2, CONST D3DXQUATERNION *pQ3 )
{
    // The SDK flips the outer control points so that the whole curve flows
    // along the shortest path:  q = |Qa + Qb| < |Qa - Qb| ? -Q : Q
    // which is equivalent to dot(Qa, Qb) < 0.
    const D3DXQUATERNION q1 = *pQ1;
    const D3DXQUATERNION q0 = ( D3DXQuaternionDot( pQ0, pQ1 ) < 0.0f ) ? -(*pQ0) : *pQ0;
    const D3DXQUATERNION q2 = ( D3DXQuaternionDot( pQ1, pQ2 ) < 0.0f ) ? -(*pQ2) : *pQ2;
    const D3DXQUATERNION q3 = ( D3DXQuaternionDot( pQ2, pQ3 ) < 0.0f ) ? -(*pQ3) : *pQ3;

    D3DXQUATERNION inv, e, l0, l1;

    // AOut = q1 * exp( -0.25 * ( ln( q1^-1 q0 ) + ln( q1^-1 q2 ) ) )
    D3DXQuaternionInverse( &inv, &q1 );
    D3DXQuaternionMultiply( &l0, &inv, &q0 );
    D3DXQuaternionLn( &l0, &l0 );
    D3DXQuaternionMultiply( &l1, &inv, &q2 );
    D3DXQuaternionLn( &l1, &l1 );
    e.x = -0.25f * ( l0.x + l1.x );
    e.y = -0.25f * ( l0.y + l1.y );
    e.z = -0.25f * ( l0.z + l1.z );
    e.w = -0.25f * ( l0.w + l1.w );
    D3DXQuaternionExp( &e, &e );
    D3DXQuaternionMultiply( pAOut, &q1, &e );

    // BOut = q2 * exp( -0.25 * ( ln( q2^-1 q1 ) + ln( q2^-1 q3 ) ) )
    D3DXQuaternionInverse( &inv, &q2 );
    D3DXQuaternionMultiply( &l0, &inv, &q1 );
    D3DXQuaternionLn( &l0, &l0 );
    D3DXQuaternionMultiply( &l1, &inv, &q3 );
    D3DXQuaternionLn( &l1, &l1 );
    e.x = -0.25f * ( l0.x + l1.x );
    e.y = -0.25f * ( l0.y + l1.y );
    e.z = -0.25f * ( l0.z + l1.z );
    e.w = -0.25f * ( l0.w + l1.w );
    D3DXQuaternionExp( &e, &e );
    D3DXQuaternionMultiply( pBOut, &q2, &e );

    // COut = q2
    *pCOut = q2;
}

// Barycentric interpolation of three quaternions, renormalized.
D3DXQUATERNION* D3DXQuaternionBaryCentric( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
                                           CONST D3DXQUATERNION *pQ2, CONST D3DXQUATERNION *pQ3,
                                           FLOAT f, FLOAT g )
{
    const FLOAT w0 = 1.0f - f - g;

    D3DXQUATERNION q;
    q.x = w0 * pQ1->x + f * pQ2->x + g * pQ3->x;
    q.y = w0 * pQ1->y + f * pQ2->y + g * pQ3->y;
    q.z = w0 * pQ1->z + f * pQ2->z + g * pQ3->z;
    q.w = w0 * pQ1->w + f * pQ2->w + g * pQ3->w;

    return D3DXQuaternionNormalize( pOut, &q );
}

//===========================================================================
//
// Planes
//
//===========================================================================

D3DXPLANE* D3DXPlaneNormalize( D3DXPLANE *pOut, CONST D3DXPLANE *pP )
{
    const FLOAT norm = sqrtf( pP->a * pP->a + pP->b * pP->b + pP->c * pP->c );

    if( norm != 0.0f )
    {
        const FLOAT f = 1.0f / norm;
        pOut->a = pP->a * f;
        pOut->b = pP->b * f;
        pOut->c = pP->c * f;
        pOut->d = pP->d * f;
    }
    else
    {
        *pOut = *pP;
    }
    return pOut;
}

D3DXVECTOR3* D3DXPlaneIntersectLine( D3DXVECTOR3 *pOut, CONST D3DXPLANE *pP,
                                     CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
    const D3DXVECTOR3 direction = *pV2 - *pV1;
    const D3DXVECTOR3 normal( pP->a, pP->b, pP->c );
    const FLOAT dot = D3DXVec3Dot( &normal, &direction );

    if( dot == 0.0f )                   // line is parallel to the plane
    {
        pOut->x = 0.0f;
        pOut->y = 0.0f;
        pOut->z = 0.0f;
    }
    else
    {
        const FLOAT t = -( pP->d + D3DXVec3Dot( &normal, pV1 ) ) / dot;
        pOut->x = pV1->x + t * direction.x;
        pOut->y = pV1->y + t * direction.y;
        pOut->z = pV1->z + t * direction.z;
    }
    return pOut;
}

D3DXPLANE* D3DXPlaneFromPointNormal( D3DXPLANE *pOut, CONST D3DXVECTOR3 *pPoint,
                                     CONST D3DXVECTOR3 *pNormal )
{
    pOut->a = pNormal->x;
    pOut->b = pNormal->y;
    pOut->c = pNormal->z;
    pOut->d = -D3DXVec3Dot( pPoint, pNormal );
    return pOut;
}

D3DXPLANE* D3DXPlaneFromPoints( D3DXPLANE *pOut, CONST D3DXVECTOR3 *pV1,
                                CONST D3DXVECTOR3 *pV2, CONST D3DXVECTOR3 *pV3 )
{
    const D3DXVECTOR3 v1 = *pV2 - *pV1;
    const D3DXVECTOR3 v2 = *pV3 - *pV1;
    D3DXVECTOR3 n;

    D3DXVec3Cross( &n, &v1, &v2 );
    D3DXVec3Normalize( &n, &n );

    pOut->a = n.x;
    pOut->b = n.y;
    pOut->c = n.z;
    pOut->d = -D3DXVec3Dot( &n, pV1 );
    return pOut;
}

D3DXPLANE* D3DXPlaneTransform( D3DXPLANE *pOut, CONST D3DXPLANE *pP, CONST D3DXMATRIX *pM )
{
    D3DXPLANE p;
    for( int i = 0; i < 4; ++i )
    {
        ( (FLOAT*)&p )[i] = pP->a * pM->m[0][i] + pP->b * pM->m[1][i]
                         + pP->c * pM->m[2][i] + pP->d * pM->m[3][i];
    }
    *pOut = p;
    return pOut;
}

D3DXPLANE* D3DXPlaneTransformArray( D3DXPLANE *pOut, UINT OutStride, CONST D3DXPLANE *pP, UINT PStride,
                                    CONST D3DXMATRIX *pM, UINT n )
{
    for( UINT i = 0; i < n; ++i )
        D3DXPlaneTransform( (D3DXPLANE*)( (unsigned char*)pOut + i * OutStride ),
                            (CONST D3DXPLANE*)( (CONST unsigned char*)pP + i * PStride ), pM );
    return pOut;
}

//===========================================================================
//
// Colors
//
//===========================================================================

D3DXCOLOR* D3DXColorAdjustSaturation( D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT s )
{
    const FLOAT grey = pC->r * 0.2125f + pC->g * 0.7154f + pC->b * 0.0721f;

    pOut->r = grey + s * ( pC->r - grey );
    pOut->g = grey + s * ( pC->g - grey );
    pOut->b = grey + s * ( pC->b - grey );
    pOut->a = pC->a;
    return pOut;
}

D3DXCOLOR* D3DXColorAdjustContrast( D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT c )
{
    pOut->r = c * ( pC->r - 0.5f ) + 0.5f;
    pOut->g = c * ( pC->g - 0.5f ) + 0.5f;
    pOut->b = c * ( pC->b - 0.5f ) + 0.5f;
    pOut->a = pC->a;
    return pOut;
}

//===========================================================================
//
// Misc
//
//===========================================================================

FLOAT D3DXFresnelTerm( FLOAT CosTheta, FLOAT RefractionIndex )
{
    const FLOAT g2 = RefractionIndex * RefractionIndex + CosTheta * CosTheta - 1.0f;
    if( g2 < 0.0f )
        return 1.0f;                        // total internal reflection

    const FLOAT g = sqrtf( g2 );
    const FLOAT a = ( g - CosTheta ) / ( g + CosTheta );
    const FLOAT b = ( CosTheta * ( g + CosTheta ) - 1.0f ) / ( CosTheta * ( g - CosTheta ) + 1.0f );

    return 0.5f * a * a * ( 1.0f + b * b );
}

//===========================================================================
//
// Intersection functions
//
//===========================================================================

// Moller-Trumbore ray/triangle intersection.
BOOL D3DXIntersectTri( CONST D3DXVECTOR3 *p0, CONST D3DXVECTOR3 *p1, CONST D3DXVECTOR3 *p2,
                       CONST D3DXVECTOR3 *pRayPos, CONST D3DXVECTOR3 *pRayDir,
                       FLOAT *pU, FLOAT *pV, FLOAT *pDist )
{
    D3DXVECTOR3 edge1, edge2, pvec, tvec, qvec;
    FLOAT det, invDet, u, v, t;

    edge1 = *p1 - *p0;
    edge2 = *p2 - *p0;

    D3DXVec3Cross( &pvec, pRayDir, &edge2 );
    det = D3DXVec3Dot( &edge1, &pvec );

    if( det < 0.0001f )                     // ray is parallel or points away
        return FALSE;

    invDet = 1.0f / det;

    tvec = *pRayPos - *p0;
    u = D3DXVec3Dot( &tvec, &pvec ) * invDet;
    if( u < 0.0f || u > 1.0f )
        return FALSE;

    D3DXVec3Cross( &qvec, &tvec, &edge1 );
    v = D3DXVec3Dot( pRayDir, &qvec ) * invDet;
    if( v < 0.0f || u + v > 1.0f )
        return FALSE;

    t = D3DXVec3Dot( &edge2, &qvec ) * invDet;

    if( pU )
        *pU = u;
    if( pV )
        *pV = v;
    if( pDist )
        *pDist = t;
    return TRUE;
}

BOOL D3DXSphereBoundProbe( CONST D3DXVECTOR3 *pCenter, FLOAT Radius,
                           CONST D3DXVECTOR3 *pRayPosition, CONST D3DXVECTOR3 *pRayDirection )
{
    // |rayPos + t*dir - center|^2 == r^2
    const D3DXVECTOR3 m = *pRayPosition - *pCenter;
    const FLOAT a = D3DXVec3LengthSq( pRayDirection );
    const FLOAT b = 2.0f * D3DXVec3Dot( pRayDirection, &m );
    const FLOAT c = D3DXVec3LengthSq( &m ) - Radius * Radius;

    if( a == 0.0f )
        return c <= 0.0f;

    FLOAT d = b * b - 4.0f * a * c;
    if( d < 0.0f )
        return FALSE;

    d = sqrtf( d );
    return ( -b + d ) >= 0.0f;              // at least one root in front
}

BOOL D3DXBoxBoundProbe( CONST D3DXVECTOR3 *pMin, CONST D3DXVECTOR3 *pMax,
                        CONST D3DXVECTOR3 *pRayPosition, CONST D3DXVECTOR3 *pRayDirection )
{
    FLOAT tmin = -FLT_MAX;
    FLOAT tmax =  FLT_MAX;

    const FLOAT origin[3] = { pRayPosition->x, pRayPosition->y, pRayPosition->z };
    const FLOAT dir[3]    = { pRayDirection->x, pRayDirection->y, pRayDirection->z };
    const FLOAT lo[3]     = { pMin->x, pMin->y, pMin->z };
    const FLOAT hi[3]     = { pMax->x, pMax->y, pMax->z };

    for( int i = 0; i < 3; ++i )
    {
        if( dir[i] == 0.0f )
        {
            if( origin[i] < lo[i] || origin[i] > hi[i] )
                return FALSE;
        }
        else
        {
            FLOAT t1 = ( lo[i] - origin[i] ) / dir[i];
            FLOAT t2 = ( hi[i] - origin[i] ) / dir[i];
            if( t1 > t2 )
            {
                const FLOAT tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if( t1 > tmin )
                tmin = t1;
            if( t2 < tmax )
                tmax = t2;
            if( tmin > tmax )
                return FALSE;
        }
    }

    return tmax >= 0.0f;
}

HRESULT D3DXComputeBoundingSphere( CONST D3DXVECTOR3 *pFirstPosition, DWORD NumVertices, DWORD dwStride,
                                   D3DXVECTOR3 *pCenter, FLOAT *pRadius )
{
    if( NumVertices == 0 || pFirstPosition == NULL || pCenter == NULL || pRadius == NULL )
        return E_INVALIDARG;

    D3DXVECTOR3 vMin, vMax;
    D3DXComputeBoundingBox( pFirstPosition, NumVertices, dwStride, &vMin, &vMax );

    pCenter->x = 0.5f * ( vMin.x + vMax.x );
    pCenter->y = 0.5f * ( vMin.y + vMax.y );
    pCenter->z = 0.5f * ( vMin.z + vMax.z );

    FLOAT maxDistSq = 0.0f;
    const unsigned char *p = (const unsigned char*)pFirstPosition;
    for( DWORD i = 0; i < NumVertices; ++i )
    {
        const D3DXVECTOR3 *v = (const D3DXVECTOR3*)( p + i * dwStride );
        const FLOAT dx = v->x - pCenter->x;
        const FLOAT dy = v->y - pCenter->y;
        const FLOAT dz = v->z - pCenter->z;
        const FLOAT d = dx * dx + dy * dy + dz * dz;
        if( d > maxDistSq )
            maxDistSq = d;
    }

    *pRadius = sqrtf( maxDistSq );
    return S_OK;
}

HRESULT D3DXComputeBoundingBox( CONST D3DXVECTOR3 *pFirstPosition, DWORD NumVertices, DWORD dwStride,
                                D3DXVECTOR3 *pMin, D3DXVECTOR3 *pMax )
{
    if( NumVertices == 0 || pFirstPosition == NULL || pMin == NULL || pMax == NULL )
        return E_INVALIDARG;

    D3DXVECTOR3 vMin = *pFirstPosition;
    D3DXVECTOR3 vMax = *pFirstPosition;

    const unsigned char *p = (const unsigned char*)pFirstPosition;
    for( DWORD i = 1; i < NumVertices; ++i )
    {
        const D3DXVECTOR3 *v = (const D3DXVECTOR3*)( p + i * dwStride );
        if( v->x < vMin.x ) vMin.x = v->x;
        if( v->y < vMin.y ) vMin.y = v->y;
        if( v->z < vMin.z ) vMin.z = v->z;
        if( v->x > vMax.x ) vMax.x = v->x;
        if( v->y > vMax.y ) vMax.y = v->y;
        if( v->z > vMax.z ) vMax.z = v->z;
    }

    *pMin = vMin;
    *pMax = vMax;
    return S_OK;
}

//===========================================================================
//
// apEx extension: 3D transform of a position that ignores the resulting w
// (no perspective divide).  third_party/apEx/Phoenix/Mesh.cpp defines its own
// copy of this function; the weak definition below keeps the compat library
// self-contained when Mesh.cpp is not part of the link.
//
//===========================================================================
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void D3DXVec3Transform3( D3DXVECTOR3 *out, D3DXVECTOR3 *in, D3DXMATRIX *m )
{
    D3DXVECTOR4 v;
    D3DXVec3Transform( &v, in, m );
    *out = *(D3DXVECTOR3*)&v;
}
