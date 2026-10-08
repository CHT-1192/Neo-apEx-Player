//////////////////////////////////////////////////////////////////////////////
//
//  compat/D3DX10Math.h
//
//  Portable, dependency-free replacement for the DirectX SDK's D3DX10Math.h
//  (June 2010).  Written for the cross-platform (macOS / Vulkan) port of the
//  apEx demotool; it must not be confused with the project's own BaseLib
//  CVector3/CMatrix4x4 types (Bedrock/BaseLib/Vector.h), which are unrelated.
//
//  Compatibility contract
//  ----------------------
//   * Member layout, member names and the operator sets of every D3DX type
//     match the SDK, so existing apEx sources compile unchanged.
//   * Storage is row-major and the convention is the D3DX row-vector one:
//     v' = v * M, translation therefore lives in _41.._43 (m[3][0..2]) and
//     quaternions are ordered (x, y, z, w).
//   * D3DXMatrixInverse(NULL output) returns NULL and leaves *pOut untouched
//     when the determinant is exactly zero, like the SDK.
//   * Trivial functions are inline in this header exactly where the SDK had
//     them inline (D3DX10math.inl).  Everything that lived in d3dx10.lib is
//     declared here and defined in src/d3dx/d3dxmath.cpp; that translation
//     unit must be part of the build.
//
//  Deviations from the SDK (deliberate)
//  ------------------------------------
//   * Default constructors zero-initialise (the SDK left them indeterminate).
//   * D3DXFLOAT16::value is public and its conversion to float is const, so
//     const D3DXFLOAT16 objects can be read.  There is deliberately no
//     D3DXFLOAT16(double) / operator=(double) overload: next to the float one
//     it would make every conversion from an integer ambiguous.  Doubles still
//     work through the standard double -> float conversion.
//   * operator % is provided for the vector types as a cross-product alias for
//     BaseLib's CVector3::operator % (D3DX itself has no such operator).
//   * D3DXVec3Transform3 is an apEx extension, not part of the SDK.
//   * D3DXMatrixStack, the D3DXSH* spherical-harmonics family and the
//     D3DX_CPU_OPTIMIZATION helpers are not provided (unused by apEx).
//   * The inline bodies and signatures are derived from the DirectX SDK's
//     D3DX10Math.h / D3DX10Math.inl, (C) Microsoft Corporation.
//
//  SDK quirks that are preserved on purpose (see src/d3dx/d3dxmath.cpp)
//  -------------------------------------------------------------------
//   * D3DXIntersectTri rejects back facing triangles (det < 0.0001) but never
//     tests t >= 0, so front facing triangles behind the ray origin still hit.
//   * D3DXVec3TransformCoord / D3DXQuaternionNormalize divide without checking
//     for a zero w / zero length, as in the SDK.
//   * D3DXFloat32To16Array collapses every NaN to 0x7FFF (canonical quiet NaN)
//     and saturates values above 65504 to infinity, like XMConvertFloatToHalf.
//   * D3DXQuaternionSlerp interpolates along the shortest arc, which is what
//     D3DXQuaternionSquadSetup assumes of the control points it builds.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef NEOAPEX_COMPAT_D3DX10MATH_H
#define NEOAPEX_COMPAT_D3DX10MATH_H

// The SDK used the D3DX9 math guard for both headers; keep it so that a real
// d3dx9math.h / D3DX10Math.h can never be included twice.
#ifndef __D3DX9MATH_H__
#define __D3DX9MATH_H__

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <new>

//----------------------------------------------------------------------------
// Scalars.  compat/Windows.h (or a real <windows.h>) may already provide
// these; the C++ standard allows an identical typedef to be repeated, and
// every definition below is intentionally identical to the SDK/compat one.
//----------------------------------------------------------------------------
#ifndef CONST
#define CONST const
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef D3DX10INLINE
#define D3DX10INLINE inline
#endif

typedef int         BOOL;
typedef uint16_t    WORD;
typedef uint32_t    UINT;
typedef uint32_t    DWORD;
typedef float       FLOAT;

#if !defined(_HRESULT_DEFINED) && !defined(__COMPAT_WINDOWS_HRESULT_DEFINED)
#define _HRESULT_DEFINED
typedef int32_t HRESULT;
#endif

// Status codes / boolean constants used by the D3DX results.  A real
// <windows.h> or compat/Windows.h already provides them, so they are only
// defined when missing (the header must be usable on its own).
#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

//----------------------------------------------------------------------------
// D3D10_VIEWPORT: needed by the D3DXVec3Project/Unproject family.  Provided
// here unless a D3D10/D3D11 header already defined it.
//----------------------------------------------------------------------------
#if !defined(D3D10_VIEWPORT_DEFINED) && !defined(__d3d10_h__) && \
    !defined(__d3d11_h__) && !defined(D3D10_VIEWPORT_DEFINED_BY_COMPAT)
typedef struct D3D10_VIEWPORT
{
    UINT TopLeftX;
    UINT TopLeftY;
    UINT Width;
    UINT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
} D3D10_VIEWPORT;
#define D3D10_VIEWPORT_DEFINED
#endif

// Note: D3D11_VIEWPORT is deliberately NOT defined here - it belongs to
// compat/d3d11.h.  Only the D3D10_VIEWPORT that D3DXVec3Project/Unproject take
// is provided (the SDK got it from D3D10.h).

#ifndef D3DVECTOR_DEFINED
typedef struct _D3DVECTOR {
    float x;
    float y;
    float z;
} D3DVECTOR;
#define D3DVECTOR_DEFINED
#endif

#ifndef D3DMATRIX_DEFINED
typedef struct _D3DMATRIX {
    union {
        struct {
            float        _11, _12, _13, _14;
            float        _21, _22, _23, _24;
            float        _31, _32, _33, _34;
            float        _41, _42, _43, _44;

        };
        float m[4][4];
    };
} D3DMATRIX;
#define D3DMATRIX_DEFINED
#endif

//===========================================================================
//
// General purpose utilities
//
//===========================================================================
#define D3DX_PI    (3.14159265358979323846)
#define D3DX_1BYPI ( 1.0 / D3DX_PI )

#define D3DXToRadian( degree ) ((degree) * (D3DX_PI / 180.0))
#define D3DXToDegree( radian ) ((radian) * (180.0 / D3DX_PI))



//===========================================================================
//
// 16 bit floating point numbers
//
//===========================================================================

#define D3DX_16F_DIG          3                // # of decimal digits of precision
#define D3DX_16F_EPSILON      4.8875809e-4f    // smallest such that 1.0 + epsilon != 1.0
#define D3DX_16F_MANT_DIG     11               // # of bits in mantissa
#define D3DX_16F_MAX          6.550400e+004    // max value
#define D3DX_16F_MAX_10_EXP   4                // max decimal exponent
#define D3DX_16F_MAX_EXP      15               // max binary exponent
#define D3DX_16F_MIN          6.1035156e-5f    // min positive value
#define D3DX_16F_MIN_10_EXP   (-4)             // min decimal exponent
#define D3DX_16F_MIN_EXP      (-14)            // min binary exponent
#define D3DX_16F_RADIX        2                // exponent radix
#define D3DX_16F_ROUNDS       1                // addition rounding: near
#define D3DX_16F_SIGN_MASK    0x8000
#define D3DX_16F_EXP_MASK     0x7C00
#define D3DX_16F_FRAC_MASK    0x03FF

typedef struct D3DXFLOAT16
{
#ifdef __cplusplus
public:
    D3DXFLOAT16() : value(0) {};
    D3DXFLOAT16( FLOAT );
    // NOTE: there is deliberately no D3DXFLOAT16(double) / operator=(double)
    // overload.  Together with D3DXFLOAT16(FLOAT) it would make every implicit
    // conversion from an integer ambiguous (apEx uses D3DXFLOAT16(0),
    // max(1, halfValue) and friends); doubles still work through the standard
    // double -> float conversion, e.g. D3DXFLOAT16(3.5) and h = 3.5.
    D3DXFLOAT16( CONST D3DXFLOAT16& ) = default;
    D3DXFLOAT16& operator = ( CONST D3DXFLOAT16& ) = default;

    D3DXFLOAT16& operator = ( FLOAT );

    // casting
    operator FLOAT () const;

    // binary operators
    BOOL operator == ( CONST D3DXFLOAT16& ) const;
    BOOL operator != ( CONST D3DXFLOAT16& ) const;

public:
#endif //__cplusplus
    WORD value;
} D3DXFLOAT16, *LPD3DXFLOAT16;



//===========================================================================
//
// Vectors
//
//===========================================================================


//--------------------------
// 2D Vector
//--------------------------
typedef struct D3DXVECTOR2
{
#ifdef __cplusplus
public:
    D3DXVECTOR2() : x(0), y(0) {};
    D3DXVECTOR2( CONST FLOAT * );
    D3DXVECTOR2( CONST D3DXFLOAT16 * );
    D3DXVECTOR2( FLOAT x, FLOAT y );

    // casting
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXVECTOR2& operator += ( CONST D3DXVECTOR2& );
    D3DXVECTOR2& operator -= ( CONST D3DXVECTOR2& );
    D3DXVECTOR2& operator *= ( FLOAT );
    D3DXVECTOR2& operator /= ( FLOAT );

    // unary operators
    D3DXVECTOR2 operator + () const;
    D3DXVECTOR2 operator - () const;

    // binary operators
    D3DXVECTOR2 operator + ( CONST D3DXVECTOR2& ) const;
    D3DXVECTOR2 operator - ( CONST D3DXVECTOR2& ) const;
    D3DXVECTOR2 operator * ( FLOAT ) const;
    D3DXVECTOR2 operator / ( FLOAT ) const;

    // apEx extension: % is the cross product (2D: returns its z component)
    FLOAT operator % ( CONST D3DXVECTOR2& ) const;

    friend D3DXVECTOR2 operator * ( FLOAT, CONST D3DXVECTOR2& );

    BOOL operator == ( CONST D3DXVECTOR2& ) const;
    BOOL operator != ( CONST D3DXVECTOR2& ) const;


public:
#endif //__cplusplus
    FLOAT x, y;
} D3DXVECTOR2, *LPD3DXVECTOR2;



//--------------------------
// 2D Vector (16 bit)
//--------------------------

typedef struct D3DXVECTOR2_16F
{
#ifdef __cplusplus
public:
    D3DXVECTOR2_16F() {};
    D3DXVECTOR2_16F( CONST FLOAT * );
    D3DXVECTOR2_16F( CONST D3DXFLOAT16 * );
    D3DXVECTOR2_16F( CONST D3DXFLOAT16 &x, CONST D3DXFLOAT16 &y );

    // casting
    operator D3DXFLOAT16* ();
    operator CONST D3DXFLOAT16* () const;

    // binary operators
    BOOL operator == ( CONST D3DXVECTOR2_16F& ) const;
    BOOL operator != ( CONST D3DXVECTOR2_16F& ) const;

public:
#endif //__cplusplus
    D3DXFLOAT16 x, y;

} D3DXVECTOR2_16F, *LPD3DXVECTOR2_16F;



//--------------------------
// 3D Vector
//--------------------------
#ifdef __cplusplus
typedef struct D3DXVECTOR3 : public D3DVECTOR
{
public:
    D3DXVECTOR3() : D3DVECTOR() {};
    D3DXVECTOR3( CONST FLOAT * );
    D3DXVECTOR3( CONST D3DVECTOR& );
    D3DXVECTOR3( CONST D3DXFLOAT16 * );
    D3DXVECTOR3( FLOAT x, FLOAT y, FLOAT z );

    // casting
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXVECTOR3& operator += ( CONST D3DXVECTOR3& );
    D3DXVECTOR3& operator -= ( CONST D3DXVECTOR3& );
    D3DXVECTOR3& operator *= ( FLOAT );
    D3DXVECTOR3& operator /= ( FLOAT );

    // unary operators
    D3DXVECTOR3 operator + () const;
    D3DXVECTOR3 operator - () const;

    // binary operators
    D3DXVECTOR3 operator + ( CONST D3DXVECTOR3& ) const;
    D3DXVECTOR3 operator - ( CONST D3DXVECTOR3& ) const;
    D3DXVECTOR3 operator * ( FLOAT ) const;
    D3DXVECTOR3 operator / ( FLOAT ) const;

    // apEx extension: % is the cross product (same as D3DXVec3Cross)
    D3DXVECTOR3 operator % ( CONST D3DXVECTOR3& ) const;

    friend D3DXVECTOR3 operator * ( FLOAT, CONST struct D3DXVECTOR3& );

    BOOL operator == ( CONST D3DXVECTOR3& ) const;
    BOOL operator != ( CONST D3DXVECTOR3& ) const;

} D3DXVECTOR3, *LPD3DXVECTOR3;

#else //!__cplusplus
typedef struct _D3DVECTOR D3DXVECTOR3, *LPD3DXVECTOR3;
#endif //!__cplusplus



//--------------------------
// 3D Vector (16 bit)
//--------------------------
typedef struct D3DXVECTOR3_16F
{
#ifdef __cplusplus
public:
    D3DXVECTOR3_16F() {};
    D3DXVECTOR3_16F( CONST FLOAT * );
    D3DXVECTOR3_16F( CONST D3DVECTOR& );
    D3DXVECTOR3_16F( CONST D3DXFLOAT16 * );
    D3DXVECTOR3_16F( CONST D3DXFLOAT16 &x, CONST D3DXFLOAT16 &y, CONST D3DXFLOAT16 &z );

    // casting
    operator D3DXFLOAT16* ();
    operator CONST D3DXFLOAT16* () const;

    // binary operators
    BOOL operator == ( CONST D3DXVECTOR3_16F& ) const;
    BOOL operator != ( CONST D3DXVECTOR3_16F& ) const;

public:
#endif //__cplusplus
    D3DXFLOAT16 x, y, z;

} D3DXVECTOR3_16F, *LPD3DXVECTOR3_16F;



//--------------------------
// 4D Vector
//--------------------------
typedef struct D3DXVECTOR4
{
#ifdef __cplusplus
public:
    D3DXVECTOR4() : x(0), y(0), z(0), w(0) {};
    D3DXVECTOR4( CONST FLOAT* );
    D3DXVECTOR4( CONST D3DXFLOAT16* );
    D3DXVECTOR4( CONST D3DVECTOR& xyz, FLOAT w );
    D3DXVECTOR4( FLOAT x, FLOAT y, FLOAT z, FLOAT w );

    // casting
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXVECTOR4& operator += ( CONST D3DXVECTOR4& );
    D3DXVECTOR4& operator -= ( CONST D3DXVECTOR4& );
    D3DXVECTOR4& operator *= ( FLOAT );
    D3DXVECTOR4& operator /= ( FLOAT );

    // unary operators
    D3DXVECTOR4 operator + () const;
    D3DXVECTOR4 operator - () const;

    // binary operators
    D3DXVECTOR4 operator + ( CONST D3DXVECTOR4& ) const;
    D3DXVECTOR4 operator - ( CONST D3DXVECTOR4& ) const;
    D3DXVECTOR4 operator * ( FLOAT ) const;
    D3DXVECTOR4 operator / ( FLOAT ) const;

    // apEx extension: % is the 4D cross product (same as D3DXVec4Cross)
    D3DXVECTOR4 operator % ( CONST D3DXVECTOR4& ) const;

    friend D3DXVECTOR4 operator * ( FLOAT, CONST D3DXVECTOR4& );

    BOOL operator == ( CONST D3DXVECTOR4& ) const;
    BOOL operator != ( CONST D3DXVECTOR4& ) const;

public:
#endif //__cplusplus
    FLOAT x, y, z, w;
} D3DXVECTOR4, *LPD3DXVECTOR4;


//--------------------------
// 4D Vector (16 bit)
//--------------------------
typedef struct D3DXVECTOR4_16F
{
#ifdef __cplusplus
public:
    D3DXVECTOR4_16F() {};
    D3DXVECTOR4_16F( CONST FLOAT * );
    D3DXVECTOR4_16F( CONST D3DXFLOAT16* );
    D3DXVECTOR4_16F( CONST D3DXVECTOR3_16F& xyz, CONST D3DXFLOAT16& w );
    D3DXVECTOR4_16F( CONST D3DXFLOAT16& x, CONST D3DXFLOAT16& y, CONST D3DXFLOAT16& z, CONST D3DXFLOAT16& w );

    // casting
    operator D3DXFLOAT16* ();
    operator CONST D3DXFLOAT16* () const;

    // binary operators
    BOOL operator == ( CONST D3DXVECTOR4_16F& ) const;
    BOOL operator != ( CONST D3DXVECTOR4_16F& ) const;

public:
#endif //__cplusplus
    D3DXFLOAT16 x, y, z, w;

} D3DXVECTOR4_16F, *LPD3DXVECTOR4_16F;



//===========================================================================
//
// Matrices
//
//===========================================================================
#ifdef __cplusplus
typedef struct D3DXMATRIX : public D3DMATRIX
{
public:
    D3DXMATRIX()
    {
        for( int r = 0; r < 4; ++r )
            for( int c = 0; c < 4; ++c )
                m[ r ][ c ] = 0.0f;
    };
    D3DXMATRIX( CONST FLOAT * );
    D3DXMATRIX( CONST D3DMATRIX& );
    D3DXMATRIX( CONST D3DXFLOAT16 * );
    D3DXMATRIX( FLOAT _11, FLOAT _12, FLOAT _13, FLOAT _14,
                FLOAT _21, FLOAT _22, FLOAT _23, FLOAT _24,
                FLOAT _31, FLOAT _32, FLOAT _33, FLOAT _34,
                FLOAT _41, FLOAT _42, FLOAT _43, FLOAT _44 );


    // access grants
    FLOAT& operator () ( UINT Row, UINT Col );
    FLOAT  operator () ( UINT Row, UINT Col ) const;

    // casting operators
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXMATRIX& operator *= ( CONST D3DXMATRIX& );
    D3DXMATRIX& operator += ( CONST D3DXMATRIX& );
    D3DXMATRIX& operator -= ( CONST D3DXMATRIX& );
    D3DXMATRIX& operator *= ( FLOAT );
    D3DXMATRIX& operator /= ( FLOAT );

    // unary operators
    D3DXMATRIX operator + () const;
    D3DXMATRIX operator - () const;

    // binary operators
    D3DXMATRIX operator * ( CONST D3DXMATRIX& ) const;
    D3DXMATRIX operator + ( CONST D3DXMATRIX& ) const;
    D3DXMATRIX operator - ( CONST D3DXMATRIX& ) const;
    D3DXMATRIX operator * ( FLOAT ) const;
    D3DXMATRIX operator / ( FLOAT ) const;

    friend D3DXMATRIX operator * ( FLOAT, CONST D3DXMATRIX& );

    BOOL operator == ( CONST D3DXMATRIX& ) const;
    BOOL operator != ( CONST D3DXMATRIX& ) const;

} D3DXMATRIX, *LPD3DXMATRIX;

#else //!__cplusplus
typedef struct _D3DMATRIX D3DXMATRIX, *LPD3DXMATRIX;
#endif //!__cplusplus


//---------------------------------------------------------------------------
// Aligned Matrices
//
// This class helps keep matrices 16-byte aligned as preferred by P4 cpus.
// It aligns matrices on the stack and on the heap or in global scope.
// It does this using __declspec(align(16)) which works on VC7 and on VC 6
// with the processor pack. Unfortunately there is no way to detect the 
// latter so this is turned on only on VC7. On other compilers this is the
// the same as D3DXMATRIX.
//
// Using this class on a compiler that does not actually do the alignment
// can be dangerous since it will not expose bugs that ignore alignment.
// E.g if an object of this class in inside a struct or class, and some code
// memcopys data in it assuming tight packing. This could break on a compiler
// that eventually start aligning the matrix.
//---------------------------------------------------------------------------
#ifdef __cplusplus
typedef struct alignas(16) _D3DXMATRIXA16 : public D3DXMATRIX
{
    _D3DXMATRIXA16() {};
    _D3DXMATRIXA16( CONST FLOAT * );
    _D3DXMATRIXA16( CONST D3DMATRIX& );
    _D3DXMATRIXA16( CONST D3DXFLOAT16 * );
    _D3DXMATRIXA16( FLOAT _11, FLOAT _12, FLOAT _13, FLOAT _14,
                    FLOAT _21, FLOAT _22, FLOAT _23, FLOAT _24,
                    FLOAT _31, FLOAT _32, FLOAT _33, FLOAT _34,
                    FLOAT _41, FLOAT _42, FLOAT _43, FLOAT _44 );

    // new operators
    void* operator new   ( size_t );
    void* operator new[] ( size_t );

    // delete operators
    void operator delete   ( void* );   // These are NOT virtual; Do not 
    void operator delete[] ( void* );   // cast to D3DXMATRIX and delete.
    
    // assignment operators
    _D3DXMATRIXA16& operator = ( CONST D3DXMATRIX& );

} _D3DXMATRIXA16;

#else //!__cplusplus
typedef D3DXMATRIX  _D3DXMATRIXA16;
#endif //!__cplusplus



#define D3DX_ALIGN16 alignas(16)

typedef _D3DXMATRIXA16 D3DXMATRIXA16, *LPD3DXMATRIXA16;



//===========================================================================
//
//    Quaternions
//
//===========================================================================
typedef struct D3DXQUATERNION
{
#ifdef __cplusplus
public:
    D3DXQUATERNION() : x(0), y(0), z(0), w(0) {};
    D3DXQUATERNION( CONST FLOAT * );
    D3DXQUATERNION( CONST D3DXFLOAT16 * );
    D3DXQUATERNION( FLOAT x, FLOAT y, FLOAT z, FLOAT w );

    // casting
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXQUATERNION& operator += ( CONST D3DXQUATERNION& );
    D3DXQUATERNION& operator -= ( CONST D3DXQUATERNION& );
    D3DXQUATERNION& operator *= ( CONST D3DXQUATERNION& );
    D3DXQUATERNION& operator *= ( FLOAT );
    D3DXQUATERNION& operator /= ( FLOAT );

    // unary operators
    D3DXQUATERNION  operator + () const;
    D3DXQUATERNION  operator - () const;

    // binary operators
    D3DXQUATERNION operator + ( CONST D3DXQUATERNION& ) const;
    D3DXQUATERNION operator - ( CONST D3DXQUATERNION& ) const;
    D3DXQUATERNION operator * ( CONST D3DXQUATERNION& ) const;
    D3DXQUATERNION operator * ( FLOAT ) const;
    D3DXQUATERNION operator / ( FLOAT ) const;

    friend D3DXQUATERNION operator * (FLOAT, CONST D3DXQUATERNION& );

    BOOL operator == ( CONST D3DXQUATERNION& ) const;
    BOOL operator != ( CONST D3DXQUATERNION& ) const;

#endif //__cplusplus
    FLOAT x, y, z, w;
} D3DXQUATERNION, *LPD3DXQUATERNION;


//===========================================================================
//
// Planes
//
//===========================================================================
typedef struct D3DXPLANE
{
#ifdef __cplusplus
public:
    D3DXPLANE() : a(0), b(0), c(0), d(0) {};
    D3DXPLANE( CONST FLOAT* );
    D3DXPLANE( CONST D3DXFLOAT16* );
    D3DXPLANE( FLOAT a, FLOAT b, FLOAT c, FLOAT d );

    // casting
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXPLANE& operator *= ( FLOAT );
    D3DXPLANE& operator /= ( FLOAT );

    // unary operators
    D3DXPLANE operator + () const;
    D3DXPLANE operator - () const;

    // binary operators
    D3DXPLANE operator * ( FLOAT ) const;
    D3DXPLANE operator / ( FLOAT ) const;

    friend D3DXPLANE operator * ( FLOAT, CONST D3DXPLANE& );

    BOOL operator == ( CONST D3DXPLANE& ) const;
    BOOL operator != ( CONST D3DXPLANE& ) const;

#endif //__cplusplus
    FLOAT a, b, c, d;
} D3DXPLANE, *LPD3DXPLANE;


//===========================================================================
//
// Colors
//
//===========================================================================

typedef struct D3DXCOLOR
{
#ifdef __cplusplus
public:
    D3DXCOLOR() : r(0), g(0), b(0), a(0) {};
    D3DXCOLOR( UINT  argb );
    D3DXCOLOR( CONST FLOAT * );
    D3DXCOLOR( CONST D3DXFLOAT16 * );
    D3DXCOLOR( FLOAT r, FLOAT g, FLOAT b, FLOAT a );

    // casting
    operator UINT  () const;

    operator FLOAT* ();
    operator CONST FLOAT* () const;

    // assignment operators
    D3DXCOLOR& operator += ( CONST D3DXCOLOR& );
    D3DXCOLOR& operator -= ( CONST D3DXCOLOR& );
    D3DXCOLOR& operator *= ( FLOAT );
    D3DXCOLOR& operator /= ( FLOAT );

    // unary operators
    D3DXCOLOR operator + () const;
    D3DXCOLOR operator - () const;

    // binary operators
    D3DXCOLOR operator + ( CONST D3DXCOLOR& ) const;
    D3DXCOLOR operator - ( CONST D3DXCOLOR& ) const;
    D3DXCOLOR operator * ( FLOAT ) const;
    D3DXCOLOR operator / ( FLOAT ) const;

    friend D3DXCOLOR operator * ( FLOAT, CONST D3DXCOLOR& );

    BOOL operator == ( CONST D3DXCOLOR& ) const;
    BOOL operator != ( CONST D3DXCOLOR& ) const;

#endif //__cplusplus
    FLOAT r, g, b, a;
} D3DXCOLOR, *LPD3DXCOLOR;

//--------------------------
// Float16
//--------------------------

// non-inline

// Converts an array 32-bit floats to 16-bit floats
D3DXFLOAT16*  D3DXFloat32To16Array
    ( D3DXFLOAT16 *pOut, CONST FLOAT *pIn, UINT n );

// Converts an array 16-bit floats to 32-bit floats
FLOAT*  D3DXFloat16To32Array
    (  FLOAT *pOut,  CONST D3DXFLOAT16 *pIn, UINT n );

//--------------------------
// 2D Vector
//--------------------------

// inline

FLOAT D3DXVec2Length
    ( CONST D3DXVECTOR2 *pV );

FLOAT D3DXVec2LengthSq
    ( CONST D3DXVECTOR2 *pV );

FLOAT D3DXVec2Dot
    ( CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

// Z component of ((x1,y1,0) cross (x2,y2,0))
FLOAT D3DXVec2CCW
    ( CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

D3DXVECTOR2* D3DXVec2Add
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

D3DXVECTOR2* D3DXVec2Subtract
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

// Minimize each component.  x = min(x1, x2), y = min(y1, y2)
D3DXVECTOR2* D3DXVec2Minimize
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

// Maximize each component.  x = max(x1, x2), y = max(y1, y2)
D3DXVECTOR2* D3DXVec2Maximize
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 );

D3DXVECTOR2* D3DXVec2Scale
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, FLOAT s );

// Linear interpolation. V1 + s(V2-V1)
D3DXVECTOR2* D3DXVec2Lerp
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2,
      FLOAT s );

// non-inline

D3DXVECTOR2*  D3DXVec2Normalize
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV );

// Hermite interpolation between position V1, tangent T1 (when s == 0)
// and position V2, tangent T2 (when s == 1).
D3DXVECTOR2*  D3DXVec2Hermite
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pT1,
      CONST D3DXVECTOR2 *pV2, CONST D3DXVECTOR2 *pT2, FLOAT s );

// CatmullRom interpolation between V1 (when s == 0) and V2 (when s == 1)
D3DXVECTOR2*  D3DXVec2CatmullRom
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV0, CONST D3DXVECTOR2 *pV1,
      CONST D3DXVECTOR2 *pV2, CONST D3DXVECTOR2 *pV3, FLOAT s );

// Barycentric coordinates.  V1 + f(V2-V1) + g(V3-V1)
D3DXVECTOR2*  D3DXVec2BaryCentric
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2,
      CONST D3DXVECTOR2 *pV3, FLOAT f, FLOAT g);

// Transform (x, y, 0, 1) by matrix.
D3DXVECTOR4*  D3DXVec2Transform
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM );

// Transform (x, y, 0, 1) by matrix, project result back into w=1.
D3DXVECTOR2*  D3DXVec2TransformCoord
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM );

// Transform (x, y, 0, 0) by matrix.
D3DXVECTOR2*  D3DXVec2TransformNormal
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, CONST D3DXMATRIX *pM );
     
// Transform Array (x, y, 0, 1) by matrix.
D3DXVECTOR4*  D3DXVec2TransformArray
    ( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n);

// Transform Array (x, y, 0, 1) by matrix, project result back into w=1.
D3DXVECTOR2*  D3DXVec2TransformCoordArray
    ( D3DXVECTOR2 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );

// Transform Array (x, y, 0, 0) by matrix.
D3DXVECTOR2*  D3DXVec2TransformNormalArray
    ( D3DXVECTOR2 *pOut, UINT OutStride, CONST D3DXVECTOR2 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );
    
    

//--------------------------
// 3D Vector
//--------------------------

// inline

FLOAT D3DXVec3Length
    ( CONST D3DXVECTOR3 *pV );

FLOAT D3DXVec3LengthSq
    ( CONST D3DXVECTOR3 *pV );

FLOAT D3DXVec3Dot
    ( CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

D3DXVECTOR3* D3DXVec3Cross
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

D3DXVECTOR3* D3DXVec3Add
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

D3DXVECTOR3* D3DXVec3Subtract
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

// Minimize each component.  x = min(x1, x2), y = min(y1, y2), ...
D3DXVECTOR3* D3DXVec3Minimize
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

// Maximize each component.  x = max(x1, x2), y = max(y1, y2), ...
D3DXVECTOR3* D3DXVec3Maximize
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 );

D3DXVECTOR3* D3DXVec3Scale
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, FLOAT s);

// Linear interpolation. V1 + s(V2-V1)
D3DXVECTOR3* D3DXVec3Lerp
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2,
      FLOAT s );

// non-inline

D3DXVECTOR3*  D3DXVec3Normalize
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV );

// Hermite interpolation between position V1, tangent T1 (when s == 0)
// and position V2, tangent T2 (when s == 1).
D3DXVECTOR3*  D3DXVec3Hermite
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pT1,
      CONST D3DXVECTOR3 *pV2, CONST D3DXVECTOR3 *pT2, FLOAT s );

// CatmullRom interpolation between V1 (when s == 0) and V2 (when s == 1)
D3DXVECTOR3*  D3DXVec3CatmullRom
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV0, CONST D3DXVECTOR3 *pV1,
      CONST D3DXVECTOR3 *pV2, CONST D3DXVECTOR3 *pV3, FLOAT s );

// Barycentric coordinates.  V1 + f(V2-V1) + g(V3-V1)
D3DXVECTOR3*  D3DXVec3BaryCentric
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2,
      CONST D3DXVECTOR3 *pV3, FLOAT f, FLOAT g);

// Transform (x, y, z, 1) by matrix.
D3DXVECTOR4*  D3DXVec3Transform
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM );

// Transform (x, y, z, 1) by matrix, project result back into w=1.
D3DXVECTOR3*  D3DXVec3TransformCoord
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM );

// Transform (x, y, z, 0) by matrix.  If you transforming a normal by a 
// non-affine matrix, the matrix you pass to this function should be the 
// transpose of the inverse of the matrix you would use to transform a coord.
D3DXVECTOR3*  D3DXVec3TransformNormal
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3DXMATRIX *pM );
    
    
// Transform Array (x, y, z, 1) by matrix. 
D3DXVECTOR4*  D3DXVec3TransformArray
    ( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );

// Transform Array (x, y, z, 1) by matrix, project result back into w=1.
D3DXVECTOR3*  D3DXVec3TransformCoordArray
    ( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );

// Transform (x, y, z, 0) by matrix.  If you transforming a normal by a 
// non-affine matrix, the matrix you pass to this function should be the 
// transpose of the inverse of the matrix you would use to transform a coord.
D3DXVECTOR3*  D3DXVec3TransformNormalArray
    ( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );

// Project vector from object space into screen space
D3DXVECTOR3*  D3DXVec3Project
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3D10_VIEWPORT *pViewport,
      CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld);

// Project vector from screen space into object space
D3DXVECTOR3*  D3DXVec3Unproject
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, CONST D3D10_VIEWPORT *pViewport,
      CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld);
      
// Project vector Array from object space into screen space
D3DXVECTOR3*  D3DXVec3ProjectArray
    ( D3DXVECTOR3 *pOut, UINT OutStride,CONST D3DXVECTOR3 *pV, UINT VStride,CONST D3D10_VIEWPORT *pViewport,
      CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld, UINT n);

// Project vector Array from screen space into object space
D3DXVECTOR3*  D3DXVec3UnprojectArray
    ( D3DXVECTOR3 *pOut, UINT OutStride, CONST D3DXVECTOR3 *pV, UINT VStride, CONST D3D10_VIEWPORT *pViewport,
      CONST D3DXMATRIX *pProjection, CONST D3DXMATRIX *pView, CONST D3DXMATRIX *pWorld, UINT n);

//--------------------------
// 4D Vector
//--------------------------

// inline

FLOAT D3DXVec4Length
    ( CONST D3DXVECTOR4 *pV );

FLOAT D3DXVec4LengthSq
    ( CONST D3DXVECTOR4 *pV );

FLOAT D3DXVec4Dot
    ( CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2 );

D3DXVECTOR4* D3DXVec4Add
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2);

D3DXVECTOR4* D3DXVec4Subtract
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2);

// Minimize each component.  x = min(x1, x2), y = min(y1, y2), ...
D3DXVECTOR4* D3DXVec4Minimize
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2);

// Maximize each component.  x = max(x1, x2), y = max(y1, y2), ...
D3DXVECTOR4* D3DXVec4Maximize
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2);

D3DXVECTOR4* D3DXVec4Scale
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, FLOAT s);

// Linear interpolation. V1 + s(V2-V1)
D3DXVECTOR4* D3DXVec4Lerp
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
      FLOAT s );

// non-inline

// Cross-product in 4 dimensions.
D3DXVECTOR4*  D3DXVec4Cross
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
      CONST D3DXVECTOR4 *pV3);

D3DXVECTOR4*  D3DXVec4Normalize
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV );

// Hermite interpolation between position V1, tangent T1 (when s == 0)
// and position V2, tangent T2 (when s == 1).
D3DXVECTOR4*  D3DXVec4Hermite
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pT1,
      CONST D3DXVECTOR4 *pV2, CONST D3DXVECTOR4 *pT2, FLOAT s );

// CatmullRom interpolation between V1 (when s == 0) and V2 (when s == 1)
D3DXVECTOR4*  D3DXVec4CatmullRom
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV0, CONST D3DXVECTOR4 *pV1,
      CONST D3DXVECTOR4 *pV2, CONST D3DXVECTOR4 *pV3, FLOAT s );

// Barycentric coordinates.  V1 + f(V2-V1) + g(V3-V1)
D3DXVECTOR4*  D3DXVec4BaryCentric
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
      CONST D3DXVECTOR4 *pV3, FLOAT f, FLOAT g);

// Transform vector by matrix.
D3DXVECTOR4*  D3DXVec4Transform
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, CONST D3DXMATRIX *pM );
    
// Transform vector array by matrix.
D3DXVECTOR4*  D3DXVec4TransformArray
    ( D3DXVECTOR4 *pOut, UINT OutStride, CONST D3DXVECTOR4 *pV, UINT VStride, CONST D3DXMATRIX *pM, UINT n );

//--------------------------
// 4D Matrix
//--------------------------

// inline

D3DXMATRIX* D3DXMatrixIdentity
    ( D3DXMATRIX *pOut );

BOOL D3DXMatrixIsIdentity
    ( CONST D3DXMATRIX *pM );

// non-inline

FLOAT  D3DXMatrixDeterminant
    ( CONST D3DXMATRIX *pM );

HRESULT  D3DXMatrixDecompose
    ( D3DXVECTOR3 *pOutScale, D3DXQUATERNION *pOutRotation, 
	  D3DXVECTOR3 *pOutTranslation, CONST D3DXMATRIX *pM );

D3DXMATRIX*  D3DXMatrixTranspose
    ( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM );

// Matrix multiplication.  The result represents the transformation M2
// followed by the transformation M1.  (Out = M1 * M2)
D3DXMATRIX*  D3DXMatrixMultiply
    ( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2 );

// Matrix multiplication, followed by a transpose. (Out = T(M1 * M2))
D3DXMATRIX*  D3DXMatrixMultiplyTranspose
    ( D3DXMATRIX *pOut, CONST D3DXMATRIX *pM1, CONST D3DXMATRIX *pM2 );

// Calculate inverse of matrix.  Inversion my fail, in which case NULL will
// be returned.  The determinant of pM is also returned it pfDeterminant
// is non-NULL.
D3DXMATRIX*  D3DXMatrixInverse
    ( D3DXMATRIX *pOut, FLOAT *pDeterminant, CONST D3DXMATRIX *pM );

// Build a matrix which scales by (sx, sy, sz)
D3DXMATRIX*  D3DXMatrixScaling
    ( D3DXMATRIX *pOut, FLOAT sx, FLOAT sy, FLOAT sz );

// Build a matrix which translates by (x, y, z)
D3DXMATRIX*  D3DXMatrixTranslation
    ( D3DXMATRIX *pOut, FLOAT x, FLOAT y, FLOAT z );

// Build a matrix which rotates around the X axis
D3DXMATRIX*  D3DXMatrixRotationX
    ( D3DXMATRIX *pOut, FLOAT Angle );

// Build a matrix which rotates around the Y axis
D3DXMATRIX*  D3DXMatrixRotationY
    ( D3DXMATRIX *pOut, FLOAT Angle );

// Build a matrix which rotates around the Z axis
D3DXMATRIX*  D3DXMatrixRotationZ
    ( D3DXMATRIX *pOut, FLOAT Angle );

// Build a matrix which rotates around an arbitrary axis
D3DXMATRIX*  D3DXMatrixRotationAxis
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pV, FLOAT Angle );

// Build a matrix from a quaternion
D3DXMATRIX*  D3DXMatrixRotationQuaternion
    ( D3DXMATRIX *pOut, CONST D3DXQUATERNION *pQ);

// Yaw around the Y axis, a pitch around the X axis,
// and a roll around the Z axis.
D3DXMATRIX*  D3DXMatrixRotationYawPitchRoll
    ( D3DXMATRIX *pOut, FLOAT Yaw, FLOAT Pitch, FLOAT Roll );

// Build transformation matrix.  NULL arguments are treated as identity.
// Mout = Msc-1 * Msr-1 * Ms * Msr * Msc * Mrc-1 * Mr * Mrc * Mt
D3DXMATRIX*  D3DXMatrixTransformation
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pScalingCenter,
      CONST D3DXQUATERNION *pScalingRotation, CONST D3DXVECTOR3 *pScaling,
      CONST D3DXVECTOR3 *pRotationCenter, CONST D3DXQUATERNION *pRotation,
      CONST D3DXVECTOR3 *pTranslation);

// Build 2D transformation matrix in XY plane.  NULL arguments are treated as identity.
// Mout = Msc-1 * Msr-1 * Ms * Msr * Msc * Mrc-1 * Mr * Mrc * Mt
D3DXMATRIX*  D3DXMatrixTransformation2D
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR2* pScalingCenter, 
      FLOAT ScalingRotation, CONST D3DXVECTOR2* pScaling, 
      CONST D3DXVECTOR2* pRotationCenter, FLOAT Rotation, 
      CONST D3DXVECTOR2* pTranslation);

// Build affine transformation matrix.  NULL arguments are treated as identity.
// Mout = Ms * Mrc-1 * Mr * Mrc * Mt
D3DXMATRIX*  D3DXMatrixAffineTransformation
    ( D3DXMATRIX *pOut, FLOAT Scaling, CONST D3DXVECTOR3 *pRotationCenter,
      CONST D3DXQUATERNION *pRotation, CONST D3DXVECTOR3 *pTranslation);

// Build 2D affine transformation matrix in XY plane.  NULL arguments are treated as identity.
// Mout = Ms * Mrc-1 * Mr * Mrc * Mt
D3DXMATRIX*  D3DXMatrixAffineTransformation2D
    ( D3DXMATRIX *pOut, FLOAT Scaling, CONST D3DXVECTOR2* pRotationCenter, 
      FLOAT Rotation, CONST D3DXVECTOR2* pTranslation);

// Build a lookat matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixLookAtRH
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pEye, CONST D3DXVECTOR3 *pAt,
      CONST D3DXVECTOR3 *pUp );

// Build a lookat matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixLookAtLH
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR3 *pEye, CONST D3DXVECTOR3 *pAt,
      CONST D3DXVECTOR3 *pUp );

// Build a perspective projection matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveRH
    ( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf );

// Build a perspective projection matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveLH
    ( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf );

// Build a perspective projection matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveFovRH
    ( D3DXMATRIX *pOut, FLOAT fovy, FLOAT Aspect, FLOAT zn, FLOAT zf );

// Build a perspective projection matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveFovLH
    ( D3DXMATRIX *pOut, FLOAT fovy, FLOAT Aspect, FLOAT zn, FLOAT zf );

// Build a perspective projection matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveOffCenterRH
    ( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn,
      FLOAT zf );

// Build a perspective projection matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixPerspectiveOffCenterLH
    ( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn,
      FLOAT zf );

// Build an ortho projection matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixOrthoRH
    ( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf );

// Build an ortho projection matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixOrthoLH
    ( D3DXMATRIX *pOut, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf );

// Build an ortho projection matrix. (right-handed)
D3DXMATRIX*  D3DXMatrixOrthoOffCenterRH
    ( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn,
      FLOAT zf );

// Build an ortho projection matrix. (left-handed)
D3DXMATRIX*  D3DXMatrixOrthoOffCenterLH
    ( D3DXMATRIX *pOut, FLOAT l, FLOAT r, FLOAT b, FLOAT t, FLOAT zn,
      FLOAT zf );

// Build a matrix which flattens geometry into a plane, as if casting
// a shadow from a light.
D3DXMATRIX*  D3DXMatrixShadow
    ( D3DXMATRIX *pOut, CONST D3DXVECTOR4 *pLight,
      CONST D3DXPLANE *pPlane );

// Build a matrix which reflects the coordinate system about a plane
D3DXMATRIX*  D3DXMatrixReflect
    ( D3DXMATRIX *pOut, CONST D3DXPLANE *pPlane );

//--------------------------
// Quaternion
//--------------------------

// inline

FLOAT D3DXQuaternionLength
    ( CONST D3DXQUATERNION *pQ );

// Length squared, or "norm"
FLOAT D3DXQuaternionLengthSq
    ( CONST D3DXQUATERNION *pQ );

FLOAT D3DXQuaternionDot
    ( CONST D3DXQUATERNION *pQ1, CONST D3DXQUATERNION *pQ2 );

// (0, 0, 0, 1)
D3DXQUATERNION* D3DXQuaternionIdentity
    ( D3DXQUATERNION *pOut );

BOOL D3DXQuaternionIsIdentity
    ( CONST D3DXQUATERNION *pQ );

// (-x, -y, -z, w)
D3DXQUATERNION* D3DXQuaternionConjugate
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ );

// non-inline

// Compute a quaternin's axis and angle of rotation. Expects unit quaternions.
void  D3DXQuaternionToAxisAngle
    ( CONST D3DXQUATERNION *pQ, D3DXVECTOR3 *pAxis, FLOAT *pAngle );

// Build a quaternion from a rotation matrix.
D3DXQUATERNION*  D3DXQuaternionRotationMatrix
    ( D3DXQUATERNION *pOut, CONST D3DXMATRIX *pM);

// Rotation about arbitrary axis.
D3DXQUATERNION*  D3DXQuaternionRotationAxis
    ( D3DXQUATERNION *pOut, CONST D3DXVECTOR3 *pV, FLOAT Angle );

// Yaw around the Y axis, a pitch around the X axis,
// and a roll around the Z axis.
D3DXQUATERNION*  D3DXQuaternionRotationYawPitchRoll
    ( D3DXQUATERNION *pOut, FLOAT Yaw, FLOAT Pitch, FLOAT Roll );

// Quaternion multiplication.  The result represents the rotation Q2
// followed by the rotation Q1.  (Out = Q2 * Q1)
D3DXQUATERNION*  D3DXQuaternionMultiply
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
      CONST D3DXQUATERNION *pQ2 );

D3DXQUATERNION*  D3DXQuaternionNormalize
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ );

// Conjugate and re-norm
D3DXQUATERNION*  D3DXQuaternionInverse
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ );

// Expects unit quaternions.
// if q = (cos(theta), sin(theta) * v); ln(q) = (0, theta * v)
D3DXQUATERNION*  D3DXQuaternionLn
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ );

// Expects pure quaternions. (w == 0)  w is ignored in calculation.
// if q = (0, theta * v); exp(q) = (cos(theta), sin(theta) * v)
D3DXQUATERNION*  D3DXQuaternionExp
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ );
      
// Spherical linear interpolation between Q1 (t == 0) and Q2 (t == 1).
// Expects unit quaternions.
D3DXQUATERNION*  D3DXQuaternionSlerp
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
      CONST D3DXQUATERNION *pQ2, FLOAT t );

// Spherical quadrangle interpolation.
// Slerp(Slerp(Q1, C, t), Slerp(A, B, t), 2t(1-t))
D3DXQUATERNION*  D3DXQuaternionSquad
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
      CONST D3DXQUATERNION *pA, CONST D3DXQUATERNION *pB,
      CONST D3DXQUATERNION *pC, FLOAT t );

// Setup control points for spherical quadrangle interpolation
// from Q1 to Q2.  The control points are chosen in such a way 
// to ensure the continuity of tangents with adjacent segments.
void  D3DXQuaternionSquadSetup
    ( D3DXQUATERNION *pAOut, D3DXQUATERNION *pBOut, D3DXQUATERNION *pCOut,
      CONST D3DXQUATERNION *pQ0, CONST D3DXQUATERNION *pQ1, 
      CONST D3DXQUATERNION *pQ2, CONST D3DXQUATERNION *pQ3 );

// Barycentric interpolation.
// Slerp(Slerp(Q1, Q2, f+g), Slerp(Q1, Q3, f+g), g/(f+g))
D3DXQUATERNION*  D3DXQuaternionBaryCentric
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ1,
      CONST D3DXQUATERNION *pQ2, CONST D3DXQUATERNION *pQ3,
      FLOAT f, FLOAT g );

//--------------------------
// Plane
//--------------------------

// inline

// ax + by + cz + dw
FLOAT D3DXPlaneDot
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR4 *pV);

// ax + by + cz + d
FLOAT D3DXPlaneDotCoord
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR3 *pV);

// ax + by + cz
FLOAT D3DXPlaneDotNormal
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR3 *pV);

D3DXPLANE* D3DXPlaneScale
    (D3DXPLANE *pOut, CONST D3DXPLANE *pP, FLOAT s);

// non-inline

// Normalize plane (so that |a,b,c| == 1)
D3DXPLANE*  D3DXPlaneNormalize
    ( D3DXPLANE *pOut, CONST D3DXPLANE *pP);

// Find the intersection between a plane and a line.  If the line is
// parallel to the plane, NULL is returned.
D3DXVECTOR3*  D3DXPlaneIntersectLine
    ( D3DXVECTOR3 *pOut, CONST D3DXPLANE *pP, CONST D3DXVECTOR3 *pV1,
      CONST D3DXVECTOR3 *pV2);

// Construct a plane from a point and a normal
D3DXPLANE*  D3DXPlaneFromPointNormal
    ( D3DXPLANE *pOut, CONST D3DXVECTOR3 *pPoint, CONST D3DXVECTOR3 *pNormal);

// Construct a plane from 3 points
D3DXPLANE*  D3DXPlaneFromPoints
    ( D3DXPLANE *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2,
      CONST D3DXVECTOR3 *pV3);

// Transform a plane by a matrix.  The vector (a,b,c) must be normal.
// M should be the inverse transpose of the transformation desired.
D3DXPLANE*  D3DXPlaneTransform
    ( D3DXPLANE *pOut, CONST D3DXPLANE *pP, CONST D3DXMATRIX *pM );
    
// Transform an array of planes by a matrix.  The vectors (a,b,c) must be normal.
// M should be the inverse transpose of the transformation desired.
D3DXPLANE*  D3DXPlaneTransformArray
    ( D3DXPLANE *pOut, UINT OutStride, CONST D3DXPLANE *pP, UINT PStride, CONST D3DXMATRIX *pM, UINT n );

//--------------------------
// Color
//--------------------------

// inline

// (1-r, 1-g, 1-b, a)
D3DXCOLOR* D3DXColorNegative
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC);

D3DXCOLOR* D3DXColorAdd
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2);

D3DXCOLOR* D3DXColorSubtract
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2);

D3DXCOLOR* D3DXColorScale
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT s);

// (r1*r2, g1*g2, b1*b2, a1*a2)
D3DXCOLOR* D3DXColorModulate
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2);

// Linear interpolation of r,g,b, and a. C1 + s(C2-C1)
D3DXCOLOR* D3DXColorLerp
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2, FLOAT s);

// non-inline

// Interpolate r,g,b between desaturated color and color.
// DesaturatedColor + s(Color - DesaturatedColor)
D3DXCOLOR*  D3DXColorAdjustSaturation
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT s);

// Interpolate r,g,b between 50% grey and color.  Grey + s(Color - Grey)
D3DXCOLOR*  D3DXColorAdjustContrast
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT c);

//--------------------------
// Misc
//--------------------------

// Calculate Fresnel term given the cosine of theta (likely obtained by
// taking the dot of two normals), and the refraction index of the material.
FLOAT  D3DXFresnelTerm
    (FLOAT CosTheta, FLOAT RefractionIndex);     

//===========================================================================

// Math intersection functions

BOOL  D3DXIntersectTri 
(
    CONST D3DXVECTOR3 *p0,           // Triangle vertex 0 position
    CONST D3DXVECTOR3 *p1,           // Triangle vertex 1 position
    CONST D3DXVECTOR3 *p2,           // Triangle vertex 2 position
    CONST D3DXVECTOR3 *pRayPos,      // Ray origin
    CONST D3DXVECTOR3 *pRayDir,      // Ray direction
    FLOAT *pU,                         // Barycentric Hit Coordinates
    FLOAT *pV,                         // Barycentric Hit Coordinates
    FLOAT *pDist);                     // Ray-Intersection Parameter Distance

BOOL 
    D3DXSphereBoundProbe(
        CONST D3DXVECTOR3 *pCenter,
        FLOAT Radius,
        CONST D3DXVECTOR3 *pRayPosition,
        CONST D3DXVECTOR3 *pRayDirection);

BOOL  
    D3DXBoxBoundProbe(
        CONST D3DXVECTOR3 *pMin, 
        CONST D3DXVECTOR3 *pMax,
        CONST D3DXVECTOR3 *pRayPosition,
        CONST D3DXVECTOR3 *pRayDirection);

HRESULT  
    D3DXComputeBoundingSphere(
        CONST D3DXVECTOR3 *pFirstPosition,	// pointer to first position
        DWORD NumVertices, 
        DWORD dwStride,							// count in bytes to subsequent position vectors
        D3DXVECTOR3 *pCenter, 
        FLOAT *pRadius);

HRESULT  
    D3DXComputeBoundingBox(
        CONST D3DXVECTOR3 *pFirstPosition,	// pointer to first position
        DWORD NumVertices, 
        DWORD dwStride,							// count in bytes to subsequent position vectors
        D3DXVECTOR3 *pMin, 
        D3DXVECTOR3 *pMax);

//////////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) Microsoft Corporation.  All Rights Reserved.
//
//  File:       d3dx10math.inl
//  Content:    D3DX10 math inline functions
//
//////////////////////////////////////////////////////////////////////////////

//===========================================================================
//
// Inline Class Methods
//
//===========================================================================

//--------------------------
// Float16
//--------------------------

//--------------------------
// 2D Vector
//--------------------------

inline
D3DXVECTOR2::D3DXVECTOR2( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    x = pf[0];
    y = pf[1];
}

inline
D3DXVECTOR2::D3DXVECTOR2( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&x, pf, 2);    
}

inline
D3DXVECTOR2::D3DXVECTOR2( FLOAT fx, FLOAT fy )
{
    x = fx;
    y = fy;
}

// casting
inline
D3DXVECTOR2::operator FLOAT* ()
{
    return (FLOAT *) &x;
}

inline
D3DXVECTOR2::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &x;
}

// assignment operators
inline D3DXVECTOR2&
D3DXVECTOR2::operator += ( CONST D3DXVECTOR2& v )
{
    x += v.x;
    y += v.y;
    return *this;
}

inline D3DXVECTOR2&
D3DXVECTOR2::operator -= ( CONST D3DXVECTOR2& v )
{
    x -= v.x;
    y -= v.y;
    return *this;
}

inline D3DXVECTOR2&
D3DXVECTOR2::operator *= ( FLOAT f )
{
    x *= f;
    y *= f;
    return *this;
}

inline D3DXVECTOR2&
D3DXVECTOR2::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    x *= fInv;
    y *= fInv;
    return *this;
}

// unary operators
inline D3DXVECTOR2
D3DXVECTOR2::operator + () const
{
    return *this;
}

inline D3DXVECTOR2
D3DXVECTOR2::operator - () const
{
    return D3DXVECTOR2(-x, -y);
}

// binary operators
inline D3DXVECTOR2
D3DXVECTOR2::operator + ( CONST D3DXVECTOR2& v ) const
{
    return D3DXVECTOR2(x + v.x, y + v.y);
}

inline D3DXVECTOR2
D3DXVECTOR2::operator - ( CONST D3DXVECTOR2& v ) const
{
    return D3DXVECTOR2(x - v.x, y - v.y);
}

inline D3DXVECTOR2
D3DXVECTOR2::operator * ( FLOAT f ) const
{
    return D3DXVECTOR2(x * f, y * f);
}

inline D3DXVECTOR2
D3DXVECTOR2::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXVECTOR2(x * fInv, y * fInv);
}

inline D3DXVECTOR2
operator * ( FLOAT f, CONST D3DXVECTOR2& v )
{
    return D3DXVECTOR2(f * v.x, f * v.y);
}

inline BOOL
D3DXVECTOR2::operator == ( CONST D3DXVECTOR2& v ) const
{
    return x == v.x && y == v.y;
}

inline BOOL
D3DXVECTOR2::operator != ( CONST D3DXVECTOR2& v ) const
{
    return x != v.x || y != v.y;
}

//--------------------------
// 2D Vector (16 bit)
//--------------------------

inline
D3DXVECTOR2_16F::D3DXVECTOR2_16F( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat32To16Array(&x, pf, 2);
}

inline
D3DXVECTOR2_16F::D3DXVECTOR2_16F( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    *((UINT *) &x) = *((UINT *) &pf[0]);
}

inline
D3DXVECTOR2_16F::D3DXVECTOR2_16F( CONST D3DXFLOAT16 &fx, CONST D3DXFLOAT16 &fy )
{
    x = fx;
    y = fy;
}

// casting
inline
D3DXVECTOR2_16F::operator D3DXFLOAT16* ()
{
    return (D3DXFLOAT16*) &x;
}

inline
D3DXVECTOR2_16F::operator CONST D3DXFLOAT16* () const
{
    return (CONST D3DXFLOAT16*) &x;
}

// binary operators
inline BOOL 
D3DXVECTOR2_16F::operator == ( CONST D3DXVECTOR2_16F &v ) const
{
    return x == v.x && y == v.y;
}

inline BOOL 
D3DXVECTOR2_16F::operator != ( CONST D3DXVECTOR2_16F &v ) const
{
    return x != v.x || y != v.y;
}

//--------------------------
// 3D Vector
//--------------------------
inline
D3DXVECTOR3::D3DXVECTOR3( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    x = pf[0];
    y = pf[1];
    z = pf[2];
}

inline
D3DXVECTOR3::D3DXVECTOR3( CONST D3DVECTOR& v )
{
    x = v.x;
    y = v.y;
    z = v.z;
}

inline
D3DXVECTOR3::D3DXVECTOR3( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&x, pf, 3);
}

inline
D3DXVECTOR3::D3DXVECTOR3( FLOAT fx, FLOAT fy, FLOAT fz )
{
    x = fx;
    y = fy;
    z = fz;
}

// casting
inline
D3DXVECTOR3::operator FLOAT* ()
{
    return (FLOAT *) &x;
}

inline
D3DXVECTOR3::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &x;
}

// assignment operators
inline D3DXVECTOR3&
D3DXVECTOR3::operator += ( CONST D3DXVECTOR3& v )
{
    x += v.x;
    y += v.y;
    z += v.z;
    return *this;
}

inline D3DXVECTOR3&
D3DXVECTOR3::operator -= ( CONST D3DXVECTOR3& v )
{
    x -= v.x;
    y -= v.y;
    z -= v.z;
    return *this;
}

inline D3DXVECTOR3&
D3DXVECTOR3::operator *= ( FLOAT f )
{
    x *= f;
    y *= f;
    z *= f;
    return *this;
}

inline D3DXVECTOR3&
D3DXVECTOR3::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    x *= fInv;
    y *= fInv;
    z *= fInv;
    return *this;
}

// unary operators
inline D3DXVECTOR3
D3DXVECTOR3::operator + () const
{
    return *this;
}

inline D3DXVECTOR3
D3DXVECTOR3::operator - () const
{
    return D3DXVECTOR3(-x, -y, -z);
}

// binary operators
inline D3DXVECTOR3
D3DXVECTOR3::operator + ( CONST D3DXVECTOR3& v ) const
{
    return D3DXVECTOR3(x + v.x, y + v.y, z + v.z);
}

inline D3DXVECTOR3
D3DXVECTOR3::operator - ( CONST D3DXVECTOR3& v ) const
{
    return D3DXVECTOR3(x - v.x, y - v.y, z - v.z);
}

inline D3DXVECTOR3
D3DXVECTOR3::operator * ( FLOAT f ) const
{
    return D3DXVECTOR3(x * f, y * f, z * f);
}

inline D3DXVECTOR3
D3DXVECTOR3::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXVECTOR3(x * fInv, y * fInv, z * fInv);
}

inline D3DXVECTOR3
operator * ( FLOAT f, CONST struct D3DXVECTOR3& v )
{
    return D3DXVECTOR3(f * v.x, f * v.y, f * v.z);
}

inline BOOL
D3DXVECTOR3::operator == ( CONST D3DXVECTOR3& v ) const
{
    return x == v.x && y == v.y && z == v.z;
}

inline BOOL
D3DXVECTOR3::operator != ( CONST D3DXVECTOR3& v ) const
{
    return x != v.x || y != v.y || z != v.z;
}

//--------------------------
// 3D Vector (16 bit)
//--------------------------

inline
D3DXVECTOR3_16F::D3DXVECTOR3_16F( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat32To16Array(&x, pf, 3);
}

inline
D3DXVECTOR3_16F::D3DXVECTOR3_16F( CONST D3DVECTOR& v )
{
    D3DXFloat32To16Array(&x, &v.x, 1);
    D3DXFloat32To16Array(&y, &v.y, 1);
    D3DXFloat32To16Array(&z, &v.z, 1);
}

inline
D3DXVECTOR3_16F::D3DXVECTOR3_16F( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    *((UINT *) &x) = *((UINT *) &pf[0]);
    *((WORD  *) &z) = *((WORD  *) &pf[2]);
}

inline
D3DXVECTOR3_16F::D3DXVECTOR3_16F( CONST D3DXFLOAT16 &fx, CONST D3DXFLOAT16 &fy, CONST D3DXFLOAT16 &fz )
{
    x = fx;
    y = fy;
    z = fz;
}

// casting
inline
D3DXVECTOR3_16F::operator D3DXFLOAT16* ()
{
    return (D3DXFLOAT16*) &x;
}

inline
D3DXVECTOR3_16F::operator CONST D3DXFLOAT16* () const
{
    return (CONST D3DXFLOAT16*) &x;
}

// binary operators
inline BOOL 
D3DXVECTOR3_16F::operator == ( CONST D3DXVECTOR3_16F &v ) const
{
    return x == v.x && y == v.y && z == v.z;
}

inline BOOL 
D3DXVECTOR3_16F::operator != ( CONST D3DXVECTOR3_16F &v ) const
{
    return x != v.x || y != v.y || z != v.z;
}

//--------------------------
// 4D Vector
//--------------------------
inline
D3DXVECTOR4::D3DXVECTOR4( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    x = pf[0];
    y = pf[1];
    z = pf[2];
    w = pf[3];
}

inline
D3DXVECTOR4::D3DXVECTOR4( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&x, pf, 4);
}

inline
D3DXVECTOR4::D3DXVECTOR4( CONST D3DVECTOR& v, FLOAT f )
{
    x = v.x;
    y = v.y;
    z = v.z;
    w = f;
}

inline
D3DXVECTOR4::D3DXVECTOR4( FLOAT fx, FLOAT fy, FLOAT fz, FLOAT fw )
{
    x = fx;
    y = fy;
    z = fz;
    w = fw;
}

// casting
inline
D3DXVECTOR4::operator FLOAT* ()
{
    return (FLOAT *) &x;
}

inline
D3DXVECTOR4::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &x;
}

// assignment operators
inline D3DXVECTOR4&
D3DXVECTOR4::operator += ( CONST D3DXVECTOR4& v )
{
    x += v.x;
    y += v.y;
    z += v.z;
    w += v.w;
    return *this;
}

inline D3DXVECTOR4&
D3DXVECTOR4::operator -= ( CONST D3DXVECTOR4& v )
{
    x -= v.x;
    y -= v.y;
    z -= v.z;
    w -= v.w;
    return *this;
}

inline D3DXVECTOR4&
D3DXVECTOR4::operator *= ( FLOAT f )
{
    x *= f;
    y *= f;
    z *= f;
    w *= f;
    return *this;
}

inline D3DXVECTOR4&
D3DXVECTOR4::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    x *= fInv;
    y *= fInv;
    z *= fInv;
    w *= fInv;
    return *this;
}

// unary operators
inline D3DXVECTOR4
D3DXVECTOR4::operator + () const
{
    return *this;
}

inline D3DXVECTOR4
D3DXVECTOR4::operator - () const
{
    return D3DXVECTOR4(-x, -y, -z, -w);
}

// binary operators
inline D3DXVECTOR4
D3DXVECTOR4::operator + ( CONST D3DXVECTOR4& v ) const
{
    return D3DXVECTOR4(x + v.x, y + v.y, z + v.z, w + v.w);
}

inline D3DXVECTOR4
D3DXVECTOR4::operator - ( CONST D3DXVECTOR4& v ) const
{
    return D3DXVECTOR4(x - v.x, y - v.y, z - v.z, w - v.w);
}

inline D3DXVECTOR4
D3DXVECTOR4::operator * ( FLOAT f ) const
{
    return D3DXVECTOR4(x * f, y * f, z * f, w * f);
}

inline D3DXVECTOR4
D3DXVECTOR4::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXVECTOR4(x * fInv, y * fInv, z * fInv, w * fInv);
}

inline D3DXVECTOR4
operator * ( FLOAT f, CONST D3DXVECTOR4& v )
{
    return D3DXVECTOR4(f * v.x, f * v.y, f * v.z, f * v.w);
}

inline BOOL
D3DXVECTOR4::operator == ( CONST D3DXVECTOR4& v ) const
{
    return x == v.x && y == v.y && z == v.z && w == v.w;
}

inline BOOL
D3DXVECTOR4::operator != ( CONST D3DXVECTOR4& v ) const
{
    return x != v.x || y != v.y || z != v.z || w != v.w;
}

//--------------------------
// 4D Vector (16 bit)
//--------------------------

inline
D3DXVECTOR4_16F::D3DXVECTOR4_16F( CONST FLOAT *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat32To16Array(&x, pf, 4);
}

inline
D3DXVECTOR4_16F::D3DXVECTOR4_16F( CONST D3DXFLOAT16 *pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    *((UINT *) &x) = *((UINT *) &pf[0]);
    *((UINT *) &z) = *((UINT *) &pf[2]);
}

inline
D3DXVECTOR4_16F::D3DXVECTOR4_16F( CONST D3DXVECTOR3_16F& v, CONST D3DXFLOAT16& f )
{
    x = v.x;
    y = v.y;
    z = v.z;
    w = f;
}

inline
D3DXVECTOR4_16F::D3DXVECTOR4_16F( CONST D3DXFLOAT16 &fx, CONST D3DXFLOAT16 &fy, CONST D3DXFLOAT16 &fz, CONST D3DXFLOAT16 &fw )
{
    x = fx;
    y = fy;
    z = fz;
    w = fw;
}

// casting
inline
D3DXVECTOR4_16F::operator D3DXFLOAT16* ()
{
    return (D3DXFLOAT16*) &x;
}

inline
D3DXVECTOR4_16F::operator CONST D3DXFLOAT16* () const
{
    return (CONST D3DXFLOAT16*) &x;
}

// binary operators
inline BOOL 
D3DXVECTOR4_16F::operator == ( CONST D3DXVECTOR4_16F &v ) const
{
    return x == v.x && y == v.y && z == v.z && w == v.w;
}

inline BOOL 
D3DXVECTOR4_16F::operator != ( CONST D3DXVECTOR4_16F &v ) const
{
    return x != v.x || y != v.y || z != v.z || w != v.w;
}

//--------------------------
// Matrix
//--------------------------
inline
D3DXMATRIX::D3DXMATRIX( CONST FLOAT* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    memcpy(&_11, pf, sizeof(D3DXMATRIX));
}

inline
D3DXMATRIX::D3DXMATRIX( CONST D3DMATRIX& mat )
{
    memcpy(&_11, &mat, sizeof(D3DXMATRIX));
}

inline
D3DXMATRIX::D3DXMATRIX( CONST D3DXFLOAT16* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&_11, pf, 16);
}

inline
D3DXMATRIX::D3DXMATRIX( FLOAT f11, FLOAT f12, FLOAT f13, FLOAT f14,
                        FLOAT f21, FLOAT f22, FLOAT f23, FLOAT f24,
                        FLOAT f31, FLOAT f32, FLOAT f33, FLOAT f34,
                        FLOAT f41, FLOAT f42, FLOAT f43, FLOAT f44 )
{
    _11 = f11; _12 = f12; _13 = f13; _14 = f14;
    _21 = f21; _22 = f22; _23 = f23; _24 = f24;
    _31 = f31; _32 = f32; _33 = f33; _34 = f34;
    _41 = f41; _42 = f42; _43 = f43; _44 = f44;
}

// access grants
inline FLOAT&
D3DXMATRIX::operator () ( UINT iRow, UINT iCol )
{
    return m[iRow][iCol];
}

inline FLOAT
D3DXMATRIX::operator () ( UINT iRow, UINT iCol ) const
{
    return m[iRow][iCol];
}

// casting operators
inline
D3DXMATRIX::operator FLOAT* ()
{
    return (FLOAT *) &_11;
}

inline
D3DXMATRIX::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &_11;
}

// assignment operators
inline D3DXMATRIX&
D3DXMATRIX::operator *= ( CONST D3DXMATRIX& mat )
{
    D3DXMatrixMultiply(this, this, &mat);
    return *this;
}

inline D3DXMATRIX&
D3DXMATRIX::operator += ( CONST D3DXMATRIX& mat )
{
    _11 += mat._11; _12 += mat._12; _13 += mat._13; _14 += mat._14;
    _21 += mat._21; _22 += mat._22; _23 += mat._23; _24 += mat._24;
    _31 += mat._31; _32 += mat._32; _33 += mat._33; _34 += mat._34;
    _41 += mat._41; _42 += mat._42; _43 += mat._43; _44 += mat._44;
    return *this;
}

inline D3DXMATRIX&
D3DXMATRIX::operator -= ( CONST D3DXMATRIX& mat )
{
    _11 -= mat._11; _12 -= mat._12; _13 -= mat._13; _14 -= mat._14;
    _21 -= mat._21; _22 -= mat._22; _23 -= mat._23; _24 -= mat._24;
    _31 -= mat._31; _32 -= mat._32; _33 -= mat._33; _34 -= mat._34;
    _41 -= mat._41; _42 -= mat._42; _43 -= mat._43; _44 -= mat._44;
    return *this;
}

inline D3DXMATRIX&
D3DXMATRIX::operator *= ( FLOAT f )
{
    _11 *= f; _12 *= f; _13 *= f; _14 *= f;
    _21 *= f; _22 *= f; _23 *= f; _24 *= f;
    _31 *= f; _32 *= f; _33 *= f; _34 *= f;
    _41 *= f; _42 *= f; _43 *= f; _44 *= f;
    return *this;
}

inline D3DXMATRIX&
D3DXMATRIX::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    _11 *= fInv; _12 *= fInv; _13 *= fInv; _14 *= fInv;
    _21 *= fInv; _22 *= fInv; _23 *= fInv; _24 *= fInv;
    _31 *= fInv; _32 *= fInv; _33 *= fInv; _34 *= fInv;
    _41 *= fInv; _42 *= fInv; _43 *= fInv; _44 *= fInv;
    return *this;
}

// unary operators
inline D3DXMATRIX
D3DXMATRIX::operator + () const
{
    return *this;
}

inline D3DXMATRIX
D3DXMATRIX::operator - () const
{
    return D3DXMATRIX(-_11, -_12, -_13, -_14,
                      -_21, -_22, -_23, -_24,
                      -_31, -_32, -_33, -_34,
                      -_41, -_42, -_43, -_44);
}

// binary operators
inline D3DXMATRIX
D3DXMATRIX::operator * ( CONST D3DXMATRIX& mat ) const
{
    D3DXMATRIX matT;
    D3DXMatrixMultiply(&matT, this, &mat);
    return matT;
}

inline D3DXMATRIX
D3DXMATRIX::operator + ( CONST D3DXMATRIX& mat ) const
{
    return D3DXMATRIX(_11 + mat._11, _12 + mat._12, _13 + mat._13, _14 + mat._14,
                      _21 + mat._21, _22 + mat._22, _23 + mat._23, _24 + mat._24,
                      _31 + mat._31, _32 + mat._32, _33 + mat._33, _34 + mat._34,
                      _41 + mat._41, _42 + mat._42, _43 + mat._43, _44 + mat._44);
}

inline D3DXMATRIX
D3DXMATRIX::operator - ( CONST D3DXMATRIX& mat ) const
{
    return D3DXMATRIX(_11 - mat._11, _12 - mat._12, _13 - mat._13, _14 - mat._14,
                      _21 - mat._21, _22 - mat._22, _23 - mat._23, _24 - mat._24,
                      _31 - mat._31, _32 - mat._32, _33 - mat._33, _34 - mat._34,
                      _41 - mat._41, _42 - mat._42, _43 - mat._43, _44 - mat._44);
}

inline D3DXMATRIX
D3DXMATRIX::operator * ( FLOAT f ) const
{
    return D3DXMATRIX(_11 * f, _12 * f, _13 * f, _14 * f,
                      _21 * f, _22 * f, _23 * f, _24 * f,
                      _31 * f, _32 * f, _33 * f, _34 * f,
                      _41 * f, _42 * f, _43 * f, _44 * f);
}

inline D3DXMATRIX
D3DXMATRIX::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXMATRIX(_11 * fInv, _12 * fInv, _13 * fInv, _14 * fInv,
                      _21 * fInv, _22 * fInv, _23 * fInv, _24 * fInv,
                      _31 * fInv, _32 * fInv, _33 * fInv, _34 * fInv,
                      _41 * fInv, _42 * fInv, _43 * fInv, _44 * fInv);
}

inline D3DXMATRIX
operator * ( FLOAT f, CONST D3DXMATRIX& mat )
{
    return D3DXMATRIX(f * mat._11, f * mat._12, f * mat._13, f * mat._14,
                      f * mat._21, f * mat._22, f * mat._23, f * mat._24,
                      f * mat._31, f * mat._32, f * mat._33, f * mat._34,
                      f * mat._41, f * mat._42, f * mat._43, f * mat._44);
}

inline BOOL
D3DXMATRIX::operator == ( CONST D3DXMATRIX& mat ) const
{
    return 0 == memcmp(this, &mat, sizeof(D3DXMATRIX));
}

inline BOOL
D3DXMATRIX::operator != ( CONST D3DXMATRIX& mat ) const
{
    return 0 != memcmp(this, &mat, sizeof(D3DXMATRIX));
}

//--------------------------
// Aligned Matrices
//--------------------------

inline
_D3DXMATRIXA16::_D3DXMATRIXA16( CONST FLOAT* f ) : 
    D3DXMATRIX( f ) 
{
}

inline
_D3DXMATRIXA16::_D3DXMATRIXA16( CONST D3DMATRIX& m ) : 
    D3DXMATRIX( m ) 
{
}

inline
_D3DXMATRIXA16::_D3DXMATRIXA16( CONST D3DXFLOAT16* f ) : 
    D3DXMATRIX( f ) 
{
}

inline
_D3DXMATRIXA16::_D3DXMATRIXA16( FLOAT _11, FLOAT _12, FLOAT _13, FLOAT _14,
                                FLOAT _21, FLOAT _22, FLOAT _23, FLOAT _24,
                                FLOAT _31, FLOAT _32, FLOAT _33, FLOAT _34,
                                FLOAT _41, FLOAT _42, FLOAT _43, FLOAT _44 ) :
    D3DXMATRIX(_11, _12, _13, _14,
               _21, _22, _23, _24,
               _31, _32, _33, _34,
               _41, _42, _43, _44) 
{
}

inline void* _D3DXMATRIXA16::operator new( size_t s )
{
    return ::operator new( s, std::align_val_t(16) );
}

inline void* _D3DXMATRIXA16::operator new[]( size_t s )
{
    return ::operator new[]( s, std::align_val_t(16) );
}

inline void _D3DXMATRIXA16::operator delete( void* p )
{
    ::operator delete( p, std::align_val_t(16) );
}

inline void _D3DXMATRIXA16::operator delete[]( void* p )
{
    ::operator delete[]( p, std::align_val_t(16) );
}

inline _D3DXMATRIXA16& 
_D3DXMATRIXA16::operator=(CONST D3DXMATRIX& rhs)
{
    memcpy(&_11, &rhs, sizeof(D3DXMATRIX));
    return *this;
}

//--------------------------
// Quaternion
//--------------------------

inline
D3DXQUATERNION::D3DXQUATERNION( CONST FLOAT* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    x = pf[0];
    y = pf[1];
    z = pf[2];
    w = pf[3];
}

inline
D3DXQUATERNION::D3DXQUATERNION( CONST D3DXFLOAT16* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&x, pf, 4);
}

inline
D3DXQUATERNION::D3DXQUATERNION( FLOAT fx, FLOAT fy, FLOAT fz, FLOAT fw )
{
    x = fx;
    y = fy;
    z = fz;
    w = fw;
}

// casting
inline
D3DXQUATERNION::operator FLOAT* ()
{
    return (FLOAT *) &x;
}

inline
D3DXQUATERNION::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &x;
}

// assignment operators
inline D3DXQUATERNION&
D3DXQUATERNION::operator += ( CONST D3DXQUATERNION& q )
{
    x += q.x;
    y += q.y;
    z += q.z;
    w += q.w;
    return *this;
}

inline D3DXQUATERNION&
D3DXQUATERNION::operator -= ( CONST D3DXQUATERNION& q )
{
    x -= q.x;
    y -= q.y;
    z -= q.z;
    w -= q.w;
    return *this;
}

inline D3DXQUATERNION&
D3DXQUATERNION::operator *= ( CONST D3DXQUATERNION& q )
{
    D3DXQuaternionMultiply(this, this, &q);
    return *this;
}

inline D3DXQUATERNION&
D3DXQUATERNION::operator *= ( FLOAT f )
{
    x *= f;
    y *= f;
    z *= f;
    w *= f;
    return *this;
}

inline D3DXQUATERNION&
D3DXQUATERNION::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    x *= fInv;
    y *= fInv;
    z *= fInv;
    w *= fInv;
    return *this;
}

// unary operators
inline D3DXQUATERNION
D3DXQUATERNION::operator + () const
{
    return *this;
}

inline D3DXQUATERNION
D3DXQUATERNION::operator - () const
{
    return D3DXQUATERNION(-x, -y, -z, -w);
}

// binary operators
inline D3DXQUATERNION
D3DXQUATERNION::operator + ( CONST D3DXQUATERNION& q ) const
{
    return D3DXQUATERNION(x + q.x, y + q.y, z + q.z, w + q.w);
}

inline D3DXQUATERNION
D3DXQUATERNION::operator - ( CONST D3DXQUATERNION& q ) const
{
    return D3DXQUATERNION(x - q.x, y - q.y, z - q.z, w - q.w);
}

inline D3DXQUATERNION
D3DXQUATERNION::operator * ( CONST D3DXQUATERNION& q ) const
{
    D3DXQUATERNION qT;
    D3DXQuaternionMultiply(&qT, this, &q);
    return qT;
}

inline D3DXQUATERNION
D3DXQUATERNION::operator * ( FLOAT f ) const
{
    return D3DXQUATERNION(x * f, y * f, z * f, w * f);
}

inline D3DXQUATERNION
D3DXQUATERNION::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXQUATERNION(x * fInv, y * fInv, z * fInv, w * fInv);
}

inline D3DXQUATERNION
operator * (FLOAT f, CONST D3DXQUATERNION& q )
{
    return D3DXQUATERNION(f * q.x, f * q.y, f * q.z, f * q.w);
}

inline BOOL
D3DXQUATERNION::operator == ( CONST D3DXQUATERNION& q ) const
{
    return x == q.x && y == q.y && z == q.z && w == q.w;
}

inline BOOL
D3DXQUATERNION::operator != ( CONST D3DXQUATERNION& q ) const
{
    return x != q.x || y != q.y || z != q.z || w != q.w;
}

//--------------------------
// Plane
//--------------------------

inline
D3DXPLANE::D3DXPLANE( CONST FLOAT* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    a = pf[0];
    b = pf[1];
    c = pf[2];
    d = pf[3];
}

inline
D3DXPLANE::D3DXPLANE( CONST D3DXFLOAT16* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&a, pf, 4);
}

inline
D3DXPLANE::D3DXPLANE( FLOAT fa, FLOAT fb, FLOAT fc, FLOAT fd )
{
    a = fa;
    b = fb;
    c = fc;
    d = fd;
}

// casting
inline
D3DXPLANE::operator FLOAT* ()
{
    return (FLOAT *) &a;
}

inline
D3DXPLANE::operator CONST FLOAT* () const
{
    return (CONST FLOAT *) &a;
}

// assignment operators
inline D3DXPLANE&
D3DXPLANE::operator *= ( FLOAT f )
{
    a *= f;
    b *= f;
    c *= f;
    d *= f;
    return *this;
}

inline D3DXPLANE&
D3DXPLANE::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    a *= fInv;
    b *= fInv;
    c *= fInv;
    d *= fInv;
    return *this;
}

// unary operators
inline D3DXPLANE
D3DXPLANE::operator + () const
{
    return *this;
}

inline D3DXPLANE
D3DXPLANE::operator - () const
{
    return D3DXPLANE(-a, -b, -c, -d);
}

// binary operators
inline D3DXPLANE
D3DXPLANE::operator * ( FLOAT f ) const
{
    return D3DXPLANE(a * f, b * f, c * f, d * f);
}

inline D3DXPLANE
D3DXPLANE::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXPLANE(a * fInv, b * fInv, c * fInv, d * fInv);
}

inline D3DXPLANE
operator * (FLOAT f, CONST D3DXPLANE& p )
{
    return D3DXPLANE(f * p.a, f * p.b, f * p.c, f * p.d);
}

inline BOOL
D3DXPLANE::operator == ( CONST D3DXPLANE& p ) const
{
    return a == p.a && b == p.b && c == p.c && d == p.d;
}

inline BOOL
D3DXPLANE::operator != ( CONST D3DXPLANE& p ) const
{
    return a != p.a || b != p.b || c != p.c || d != p.d;
}

//--------------------------
// Color
//--------------------------

inline
D3DXCOLOR::D3DXCOLOR( UINT dw )
{
    CONST FLOAT f = 1.0f / 255.0f;
    r = f * (FLOAT) (unsigned char) (dw >> 16);
    g = f * (FLOAT) (unsigned char) (dw >>  8);
    b = f * (FLOAT) (unsigned char) (dw >>  0);
    a = f * (FLOAT) (unsigned char) (dw >> 24);
}

inline
D3DXCOLOR::D3DXCOLOR( CONST FLOAT* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    r = pf[0];
    g = pf[1];
    b = pf[2];
    a = pf[3];
}

inline
D3DXCOLOR::D3DXCOLOR( CONST D3DXFLOAT16* pf )
{
#ifdef D3DX10_DEBUG
    if(!pf)
        return;
#endif

    D3DXFloat16To32Array(&r, pf, 4);
}

inline
D3DXCOLOR::D3DXCOLOR( FLOAT fr, FLOAT fg, FLOAT fb, FLOAT fa )
{
    r = fr;
    g = fg;
    b = fb;
    a = fa;
}

// casting
inline
D3DXCOLOR::operator UINT () const
{
    UINT dwR = r >= 1.0f ? 0xff : r <= 0.0f ? 0x00 : (UINT) (r * 255.0f + 0.5f);
    UINT dwG = g >= 1.0f ? 0xff : g <= 0.0f ? 0x00 : (UINT) (g * 255.0f + 0.5f);
    UINT dwB = b >= 1.0f ? 0xff : b <= 0.0f ? 0x00 : (UINT) (b * 255.0f + 0.5f);
    UINT dwA = a >= 1.0f ? 0xff : a <= 0.0f ? 0x00 : (UINT) (a * 255.0f + 0.5f);

    return (dwA << 24) | (dwR << 16) | (dwG << 8) | (dwB << 0);
}

inline
D3DXCOLOR::operator FLOAT * ()
{
    return (FLOAT *) &r;
}

inline
D3DXCOLOR::operator CONST FLOAT * () const
{
    return (CONST FLOAT *) &r;
}

// assignment operators
inline D3DXCOLOR&
D3DXCOLOR::operator += ( CONST D3DXCOLOR& c )
{
    r += c.r;
    g += c.g;
    b += c.b;
    a += c.a;
    return *this;
}

inline D3DXCOLOR&
D3DXCOLOR::operator -= ( CONST D3DXCOLOR& c )
{
    r -= c.r;
    g -= c.g;
    b -= c.b;
    a -= c.a;
    return *this;
}

inline D3DXCOLOR&
D3DXCOLOR::operator *= ( FLOAT f )
{
    r *= f;
    g *= f;
    b *= f;
    a *= f;
    return *this;
}

inline D3DXCOLOR&
D3DXCOLOR::operator /= ( FLOAT f )
{
    FLOAT fInv = 1.0f / f;
    r *= fInv;
    g *= fInv;
    b *= fInv;
    a *= fInv;
    return *this;
}

// unary operators
inline D3DXCOLOR
D3DXCOLOR::operator + () const
{
    return *this;
}

inline D3DXCOLOR
D3DXCOLOR::operator - () const
{
    return D3DXCOLOR(-r, -g, -b, -a);
}

// binary operators
inline D3DXCOLOR
D3DXCOLOR::operator + ( CONST D3DXCOLOR& c ) const
{
    return D3DXCOLOR(r + c.r, g + c.g, b + c.b, a + c.a);
}

inline D3DXCOLOR
D3DXCOLOR::operator - ( CONST D3DXCOLOR& c ) const
{
    return D3DXCOLOR(r - c.r, g - c.g, b - c.b, a - c.a);
}

inline D3DXCOLOR
D3DXCOLOR::operator * ( FLOAT f ) const
{
    return D3DXCOLOR(r * f, g * f, b * f, a * f);
}

inline D3DXCOLOR
D3DXCOLOR::operator / ( FLOAT f ) const
{
    FLOAT fInv = 1.0f / f;
    return D3DXCOLOR(r * fInv, g * fInv, b * fInv, a * fInv);
}

inline D3DXCOLOR
operator * (FLOAT f, CONST D3DXCOLOR& c )
{
    return D3DXCOLOR(f * c.r, f * c.g, f * c.b, f * c.a);
}

inline BOOL
D3DXCOLOR::operator == ( CONST D3DXCOLOR& c ) const
{
    return r == c.r && g == c.g && b == c.b && a == c.a;
}

inline BOOL
D3DXCOLOR::operator != ( CONST D3DXCOLOR& c ) const
{
    return r != c.r || g != c.g || b != c.b || a != c.a;
}

//===========================================================================
//
// Inline functions
//
//===========================================================================

//--------------------------
// 2D Vector
//--------------------------

inline FLOAT D3DXVec2Length
    ( CONST D3DXVECTOR2 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

#ifdef __cplusplus
    return sqrtf(pV->x * pV->x + pV->y * pV->y);
#else
    return (FLOAT) sqrt(pV->x * pV->x + pV->y * pV->y);
#endif
}

inline FLOAT D3DXVec2LengthSq
    ( CONST D3DXVECTOR2 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

    return pV->x * pV->x + pV->y * pV->y;
}

inline FLOAT D3DXVec2Dot
    ( CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pV1 || !pV2)
        return 0.0f;
#endif

    return pV1->x * pV2->x + pV1->y * pV2->y;
}

inline FLOAT D3DXVec2CCW
    ( CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pV1 || !pV2)
        return 0.0f;
#endif

    return pV1->x * pV2->y - pV1->y * pV2->x;
}

inline D3DXVECTOR2* D3DXVec2Add
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + pV2->x;
    pOut->y = pV1->y + pV2->y;
    return pOut;
}

inline D3DXVECTOR2* D3DXVec2Subtract
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x - pV2->x;
    pOut->y = pV1->y - pV2->y;
    return pOut;
}

inline D3DXVECTOR2* D3DXVec2Minimize
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x < pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y < pV2->y ? pV1->y : pV2->y;
    return pOut;
}

inline D3DXVECTOR2* D3DXVec2Maximize
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x > pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y > pV2->y ? pV1->y : pV2->y;
    return pOut;
}

inline D3DXVECTOR2* D3DXVec2Scale
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV, FLOAT s )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV)
        return NULL;
#endif

    pOut->x = pV->x * s;
    pOut->y = pV->y * s;
    return pOut;
}

inline D3DXVECTOR2* D3DXVec2Lerp
    ( D3DXVECTOR2 *pOut, CONST D3DXVECTOR2 *pV1, CONST D3DXVECTOR2 *pV2,
      FLOAT s )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + s * (pV2->x - pV1->x);
    pOut->y = pV1->y + s * (pV2->y - pV1->y);
    return pOut;
}

//--------------------------
// 3D Vector
//--------------------------

inline FLOAT D3DXVec3Length
    ( CONST D3DXVECTOR3 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

#ifdef __cplusplus
    return sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
#else
    return (FLOAT) sqrt(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
#endif
}

inline FLOAT D3DXVec3LengthSq
    ( CONST D3DXVECTOR3 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

    return pV->x * pV->x + pV->y * pV->y + pV->z * pV->z;
}

inline FLOAT D3DXVec3Dot
    ( CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pV1 || !pV2)
        return 0.0f;
#endif

    return pV1->x * pV2->x + pV1->y * pV2->y + pV1->z * pV2->z;
}

inline D3DXVECTOR3* D3DXVec3Cross
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
    D3DXVECTOR3 v;

#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    v.x = pV1->y * pV2->z - pV1->z * pV2->y;
    v.y = pV1->z * pV2->x - pV1->x * pV2->z;
    v.z = pV1->x * pV2->y - pV1->y * pV2->x;

    *pOut = v;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Add
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + pV2->x;
    pOut->y = pV1->y + pV2->y;
    pOut->z = pV1->z + pV2->z;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Subtract
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x - pV2->x;
    pOut->y = pV1->y - pV2->y;
    pOut->z = pV1->z - pV2->z;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Minimize
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x < pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y < pV2->y ? pV1->y : pV2->y;
    pOut->z = pV1->z < pV2->z ? pV1->z : pV2->z;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Maximize
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x > pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y > pV2->y ? pV1->y : pV2->y;
    pOut->z = pV1->z > pV2->z ? pV1->z : pV2->z;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Scale
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV, FLOAT s)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV)
        return NULL;
#endif

    pOut->x = pV->x * s;
    pOut->y = pV->y * s;
    pOut->z = pV->z * s;
    return pOut;
}

inline D3DXVECTOR3* D3DXVec3Lerp
    ( D3DXVECTOR3 *pOut, CONST D3DXVECTOR3 *pV1, CONST D3DXVECTOR3 *pV2,
      FLOAT s )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + s * (pV2->x - pV1->x);
    pOut->y = pV1->y + s * (pV2->y - pV1->y);
    pOut->z = pV1->z + s * (pV2->z - pV1->z);
    return pOut;
}

//--------------------------
// 4D Vector
//--------------------------

inline FLOAT D3DXVec4Length
    ( CONST D3DXVECTOR4 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

#ifdef __cplusplus
    return sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z + pV->w * pV->w);
#else
    return (FLOAT) sqrt(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z + pV->w * pV->w);
#endif
}

inline FLOAT D3DXVec4LengthSq
    ( CONST D3DXVECTOR4 *pV )
{
#ifdef D3DX10_DEBUG
    if(!pV)
        return 0.0f;
#endif

    return pV->x * pV->x + pV->y * pV->y + pV->z * pV->z + pV->w * pV->w;
}

inline FLOAT D3DXVec4Dot
    ( CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2 )
{
#ifdef D3DX10_DEBUG
    if(!pV1 || !pV2)
        return 0.0f;
#endif

    return pV1->x * pV2->x + pV1->y * pV2->y + pV1->z * pV2->z + pV1->w * pV2->w;
}

inline D3DXVECTOR4* D3DXVec4Add
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + pV2->x;
    pOut->y = pV1->y + pV2->y;
    pOut->z = pV1->z + pV2->z;
    pOut->w = pV1->w + pV2->w;
    return pOut;
}

inline D3DXVECTOR4* D3DXVec4Subtract
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x - pV2->x;
    pOut->y = pV1->y - pV2->y;
    pOut->z = pV1->z - pV2->z;
    pOut->w = pV1->w - pV2->w;
    return pOut;
}

inline D3DXVECTOR4* D3DXVec4Minimize
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x < pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y < pV2->y ? pV1->y : pV2->y;
    pOut->z = pV1->z < pV2->z ? pV1->z : pV2->z;
    pOut->w = pV1->w < pV2->w ? pV1->w : pV2->w;
    return pOut;
}

inline D3DXVECTOR4* D3DXVec4Maximize
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x > pV2->x ? pV1->x : pV2->x;
    pOut->y = pV1->y > pV2->y ? pV1->y : pV2->y;
    pOut->z = pV1->z > pV2->z ? pV1->z : pV2->z;
    pOut->w = pV1->w > pV2->w ? pV1->w : pV2->w;
    return pOut;
}

inline D3DXVECTOR4* D3DXVec4Scale
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV, FLOAT s)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV)
        return NULL;
#endif

    pOut->x = pV->x * s;
    pOut->y = pV->y * s;
    pOut->z = pV->z * s;
    pOut->w = pV->w * s;
    return pOut;
}

inline D3DXVECTOR4* D3DXVec4Lerp
    ( D3DXVECTOR4 *pOut, CONST D3DXVECTOR4 *pV1, CONST D3DXVECTOR4 *pV2,
      FLOAT s )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pV1 || !pV2)
        return NULL;
#endif

    pOut->x = pV1->x + s * (pV2->x - pV1->x);
    pOut->y = pV1->y + s * (pV2->y - pV1->y);
    pOut->z = pV1->z + s * (pV2->z - pV1->z);
    pOut->w = pV1->w + s * (pV2->w - pV1->w);
    return pOut;
}

//--------------------------
// 4D Matrix
//--------------------------

inline D3DXMATRIX* D3DXMatrixIdentity
    ( D3DXMATRIX *pOut )
{
#ifdef D3DX10_DEBUG
    if(!pOut)
        return NULL;
#endif

    pOut->m[0][1] = pOut->m[0][2] = pOut->m[0][3] =
    pOut->m[1][0] = pOut->m[1][2] = pOut->m[1][3] =
    pOut->m[2][0] = pOut->m[2][1] = pOut->m[2][3] =
    pOut->m[3][0] = pOut->m[3][1] = pOut->m[3][2] = 0.0f;

    pOut->m[0][0] = pOut->m[1][1] = pOut->m[2][2] = pOut->m[3][3] = 1.0f;
    return pOut;
}

inline BOOL D3DXMatrixIsIdentity
    ( CONST D3DXMATRIX *pM )
{
#ifdef D3DX10_DEBUG
    if(!pM)
        return FALSE;
#endif

    return pM->m[0][0] == 1.0f && pM->m[0][1] == 0.0f && pM->m[0][2] == 0.0f && pM->m[0][3] == 0.0f &&
           pM->m[1][0] == 0.0f && pM->m[1][1] == 1.0f && pM->m[1][2] == 0.0f && pM->m[1][3] == 0.0f &&
           pM->m[2][0] == 0.0f && pM->m[2][1] == 0.0f && pM->m[2][2] == 1.0f && pM->m[2][3] == 0.0f &&
           pM->m[3][0] == 0.0f && pM->m[3][1] == 0.0f && pM->m[3][2] == 0.0f && pM->m[3][3] == 1.0f;
}

//--------------------------
// Quaternion
//--------------------------

inline FLOAT D3DXQuaternionLength
    ( CONST D3DXQUATERNION *pQ )
{
#ifdef D3DX10_DEBUG
    if(!pQ)
        return 0.0f;
#endif

#ifdef __cplusplus
    return sqrtf(pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z + pQ->w * pQ->w);
#else
    return (FLOAT) sqrt(pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z + pQ->w * pQ->w);
#endif
}

inline FLOAT D3DXQuaternionLengthSq
    ( CONST D3DXQUATERNION *pQ )
{
#ifdef D3DX10_DEBUG
    if(!pQ)
        return 0.0f;
#endif

    return pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z + pQ->w * pQ->w;
}

inline FLOAT D3DXQuaternionDot
    ( CONST D3DXQUATERNION *pQ1, CONST D3DXQUATERNION *pQ2 )
{
#ifdef D3DX10_DEBUG
    if(!pQ1 || !pQ2)
        return 0.0f;
#endif

    return pQ1->x * pQ2->x + pQ1->y * pQ2->y + pQ1->z * pQ2->z + pQ1->w * pQ2->w;
}

inline D3DXQUATERNION* D3DXQuaternionIdentity
    ( D3DXQUATERNION *pOut )
{
#ifdef D3DX10_DEBUG
    if(!pOut)
        return NULL;
#endif

    pOut->x = pOut->y = pOut->z = 0.0f;
    pOut->w = 1.0f;
    return pOut;
}

inline BOOL D3DXQuaternionIsIdentity
    ( CONST D3DXQUATERNION *pQ )
{
#ifdef D3DX10_DEBUG
    if(!pQ)
        return FALSE;
#endif

    return pQ->x == 0.0f && pQ->y == 0.0f && pQ->z == 0.0f && pQ->w == 1.0f;
}

inline D3DXQUATERNION* D3DXQuaternionConjugate
    ( D3DXQUATERNION *pOut, CONST D3DXQUATERNION *pQ )
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pQ)
        return NULL;
#endif

    pOut->x = -pQ->x;
    pOut->y = -pQ->y;
    pOut->z = -pQ->z;
    pOut->w =  pQ->w;
    return pOut;
}

//--------------------------
// Plane
//--------------------------

inline FLOAT D3DXPlaneDot
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR4 *pV)
{
#ifdef D3DX10_DEBUG
    if(!pP || !pV)
        return 0.0f;
#endif

    return pP->a * pV->x + pP->b * pV->y + pP->c * pV->z + pP->d * pV->w;
}

inline FLOAT D3DXPlaneDotCoord
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR3 *pV)
{
#ifdef D3DX10_DEBUG
    if(!pP || !pV)
        return 0.0f;
#endif

    return pP->a * pV->x + pP->b * pV->y + pP->c * pV->z + pP->d;
}

inline FLOAT D3DXPlaneDotNormal
    ( CONST D3DXPLANE *pP, CONST D3DXVECTOR3 *pV)
{
#ifdef D3DX10_DEBUG
    if(!pP || !pV)
        return 0.0f;
#endif

    return pP->a * pV->x + pP->b * pV->y + pP->c * pV->z;
}

inline D3DXPLANE* D3DXPlaneScale
    (D3DXPLANE *pOut, CONST D3DXPLANE *pP, FLOAT s)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pP)
        return NULL;
#endif

    pOut->a = pP->a * s;
    pOut->b = pP->b * s;
    pOut->c = pP->c * s;
    pOut->d = pP->d * s;
    return pOut;
}

//--------------------------
// Color
//--------------------------

inline D3DXCOLOR* D3DXColorNegative
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC)
        return NULL;
#endif

    pOut->r = 1.0f - pC->r;
    pOut->g = 1.0f - pC->g;
    pOut->b = 1.0f - pC->b;
    pOut->a = pC->a;
    return pOut;
}

inline D3DXCOLOR* D3DXColorAdd
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC1 || !pC2)
        return NULL;
#endif

    pOut->r = pC1->r + pC2->r;
    pOut->g = pC1->g + pC2->g;
    pOut->b = pC1->b + pC2->b;
    pOut->a = pC1->a + pC2->a;
    return pOut;
}

inline D3DXCOLOR* D3DXColorSubtract
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC1 || !pC2)
        return NULL;
#endif

    pOut->r = pC1->r - pC2->r;
    pOut->g = pC1->g - pC2->g;
    pOut->b = pC1->b - pC2->b;
    pOut->a = pC1->a - pC2->a;
    return pOut;
}

inline D3DXCOLOR* D3DXColorScale
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC, FLOAT s)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC)
        return NULL;
#endif

    pOut->r = pC->r * s;
    pOut->g = pC->g * s;
    pOut->b = pC->b * s;
    pOut->a = pC->a * s;
    return pOut;
}

inline D3DXCOLOR* D3DXColorModulate
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC1 || !pC2)
        return NULL;
#endif

    pOut->r = pC1->r * pC2->r;
    pOut->g = pC1->g * pC2->g;
    pOut->b = pC1->b * pC2->b;
    pOut->a = pC1->a * pC2->a;
    return pOut;
}

inline D3DXCOLOR* D3DXColorLerp
    (D3DXCOLOR *pOut, CONST D3DXCOLOR *pC1, CONST D3DXCOLOR *pC2, FLOAT s)
{
#ifdef D3DX10_DEBUG
    if(!pOut || !pC1 || !pC2)
        return NULL;
#endif

    pOut->r = pC1->r + s * (pC2->r - pC1->r);
    pOut->g = pC1->g + s * (pC2->g - pC1->g);
    pOut->b = pC1->b + s * (pC2->b - pC1->b);
    pOut->a = pC1->a + s * (pC2->a - pC1->a);
    return pOut;
}


//---------------------------------------------------------------------------
// apEx extension operators (see the declarations inside the vector types).
//---------------------------------------------------------------------------
inline FLOAT D3DXVECTOR2::operator % ( CONST D3DXVECTOR2& v ) const
{
    return x * v.y - y * v.x;                       // D3DXVec2CCW
}

inline D3DXVECTOR3 D3DXVECTOR3::operator % ( CONST D3DXVECTOR3& v ) const
{
    return D3DXVECTOR3( y * v.z - z * v.y,
                        z * v.x - x * v.z,
                        x * v.y - y * v.x );        // D3DXVec3Cross
}

inline D3DXVECTOR4 D3DXVECTOR4::operator % ( CONST D3DXVECTOR4& v ) const
{
    // 4D cross product of two vectors: orthogonal to both (D3DXVec4Cross
    // itself takes three vectors)
    FLOAT a = ( z * v.w - w * v.z );
    FLOAT b = ( w * v.x - x * v.w );
    FLOAT c = ( x * v.y - y * v.x );
    FLOAT d = ( y * v.z - z * v.y );
    return D3DXVECTOR4( a, b, c, d );
}

//---------------------------------------------------------------------------
// apEx extension: 3D transform of a position that ignores the resulting w
// (no perspective divide).  Semantics taken from the definition that ships in
// third_party/apEx/Phoenix/Mesh.cpp; a weak fallback definition exists in
// src/d3dx/d3dxmath.cpp so the compat library is self-contained.
//---------------------------------------------------------------------------
void D3DXVec3Transform3( D3DXVECTOR3 *out, D3DXVECTOR3 *in, D3DXMATRIX *m );

#endif // __D3DX9MATH_H__

#endif // NEOAPEX_COMPAT_D3DX10MATH_H
