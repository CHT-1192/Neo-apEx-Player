#pragma once
// compat/wingdi.h
//
// Portable stand-in for the Windows SDK <wingdi.h> (plus the three winuser.h
// entry points the vendored engine reaches for: GetDC/ReleaseDC/FillRect/
// GetSystemMetrics).
//
// This is the declaration half of the GDI port; the implementation lives in
// src/platform/gdi_posix.cpp and is built on CoreText/CoreGraphics.
//
// Scope: exactly the surface the vendored apEx code calls, verified with
//
//   grep -rhoE "\b(GetDC|ReleaseDC|CreateCompatibleDC|DeleteDC|...)\b" \
//     third_party/apEx/Phoenix third_party/apEx/Phoenix_Tool \
//     third_party/apEx/Bedrock/CoRE2 | sort | uniq -c
//
//   Fonts      CreateFontA/CreateFontIndirectA/SelectObject/DeleteObject,
//              GetGlyphOutline(GGO_NATIVE/GGO_BITMAP/GGO_GRAY8_BITMAP),
//              GetTextMetricsA/GetTextExtentPoint32A
//   DC+bitmaps GetDC(NULL)/ReleaseDC, CreateCompatibleDC/DeleteDC,
//              CreateCompatibleBitmap/CreateBitmap/CreateDIBSection,
//              GetStockObject, GetDIBits, GetObjectA, CreateSolidBrush,
//              CreatePen, FillRect, SetPixel, GetPixel, BitBlt
//   Text       TextOutA, SetBkMode, SetTextColor, SetTextCharacterExtra
//   Misc       GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN), GetDeviceCaps
//
// Layout rule (same as the rest of compat/): every struct below is
// byte-for-byte the Windows layout, because the engine walks the GGO_NATIVE
// glyph stream with its own pointer arithmetic.
//
// NOTE: compat/Windows.h already pulls this header in (guarded by
// __has_include), which is also what makes the A/W macros below visible to
// every translation unit that includes <Windows.h>.

#include "Windows.h"

#ifdef __cplusplus
extern "C" {
#endif

//////////////////////////////////////////////////////////////////////////////
// Fixed-point and glyph outlines
//////////////////////////////////////////////////////////////////////////////

// Windows.h deliberately has no SHORT (it is only used by the GDI structs).
#ifndef _DSH_SHORT_DEFINED
#define _DSH_SHORT_DEFINED
typedef short SHORT;
typedef SHORT *PSHORT, *LPSHORT;
#endif

// A 16.16 fixed-point scalar split the way GDI splits it: `value` holds the
// (floored) integer part, `fract` the fraction, so
//     (value << 16 | fract) reinterpreted as int32 / 65536.0
// is the value.  Mesh.cpp relies on exactly that (its TOFLOAT macro).
typedef struct _FIXED
{
  WORD  fract;
  SHORT value;
} FIXED;

typedef struct tagPOINTFX
{
  FIXED x;
  FIXED y;
} POINTFX, *LPPOINTFX;

// wType
#define TT_PRIM_LINE    1
#define TT_PRIM_QSPLINE 2
#define TT_PRIM_CSPLINE 3
// dwType
#define TT_POLYGON_TYPE 24

typedef struct tagTTPOLYCURVE
{
  WORD   wType;   // TT_PRIM_LINE / TT_PRIM_QSPLINE / TT_PRIM_CSPLINE
  WORD   cpfx;    // number of POINTFX in apfx
  POINTFX apfx[1];
} TTPOLYCURVE, *LPTTPOLYCURVE;

typedef struct tagTTPOLYGONHEADER
{
  DWORD   cb;       // size of this contour block, header included
  DWORD   dwType;   // TT_POLYGON_TYPE
  POINTFX pfxStart; // first (on-curve) point of the contour
} TTPOLYGONHEADER, *LPTTPOLYGONHEADER;

typedef struct _MAT2
{
  FIXED eM11, eM12, eM21, eM22;
} MAT2, *LPMAT2;

typedef struct _GLYPHMETRICS
{
  UINT  gmBlackBoxX;
  UINT  gmBlackBoxY;
  POINT gmptGlyphOrigin; // top-left of the ink box, y up
  SHORT gmCellIncX;
  SHORT gmCellIncY;
} GLYPHMETRICS, *LPGLYPHMETRICS;

#define GGO_METRICS      0
#define GGO_BITMAP       1
#define GGO_NATIVE       2
#define GGO_BEZIER       3
#define GGO_GRAY2_BITMAP 4
#define GGO_GRAY4_BITMAP 5
#define GGO_GRAY8_BITMAP 6
#define GGO_GLYPH_INDEX  0x0080
#define GGO_UNHINTED     0x0100

//////////////////////////////////////////////////////////////////////////////
// Fonts
//////////////////////////////////////////////////////////////////////////////

#define LF_FACESIZE      32
#define LF_FULLFACESIZE  64

#define FW_DONTCARE  0
#define FW_THIN      100
#define FW_EXTRALIGHT 200
#define FW_ULTRALIGHT FW_EXTRALIGHT
#define FW_LIGHT     300
#define FW_NORMAL    400
#define FW_REGULAR   400
#define FW_MEDIUM    500
#define FW_SEMIBOLD  600
#define FW_DEMIBOLD  FW_SEMIBOLD
#define FW_BOLD      700
#define FW_EXTRABOLD 800
#define FW_ULTRABOLD FW_EXTRABOLD
#define FW_HEAVY     900
#define FW_BLACK     FW_HEAVY

#define ANSI_CHARSET        0
#define DEFAULT_CHARSET     1
#define SYMBOL_CHARSET      2
#define SHIFTJIS_CHARSET    128
#define HANGEUL_CHARSET     129
#define HANGUL_CHARSET      129
#define GB2312_CHARSET      134
#define CHINESEBIG5_CHARSET 136
#define GREEK_CHARSET       161
#define TURKISH_CHARSET     162
#define HEBREW_CHARSET      177
#define ARABIC_CHARSET      178
#define BALTIC_CHARSET      186
#define RUSSIAN_CHARSET     204
#define THAI_CHARSET        222
#define EASTEUROPE_CHARSET  238
#define OEM_CHARSET         255
#define JOHAB_CHARSET       130
#define VIETNAMESE_CHARSET  163
#define MAC_CHARSET         77

#define OUT_DEFAULT_PRECIS     0
#define OUT_STRING_PRECIS      1
#define OUT_CHARACTER_PRECIS   2
#define OUT_STROKE_PRECIS      3
#define OUT_TT_PRECIS          4
#define OUT_DEVICE_PRECIS      5
#define OUT_RASTER_PRECIS      6
#define OUT_TT_ONLY_PRECIS     7
#define OUT_OUTLINE_PRECIS     8
#define OUT_PS_ONLY_PRECIS     10

#define CLIP_DEFAULT_PRECIS    0
#define CLIP_CHARACTER_PRECIS  1
#define CLIP_STROKE_PRECIS     2
#define CLIP_MASK              0xf
#define CLIP_LH_ANGLES         0x10
#define CLIP_TT_ALWAYS         0x20
#define CLIP_DFA_DISABLE       0x40
#define CLIP_EMBEDDED          0x80

#define DEFAULT_QUALITY            0
#define DRAFT_QUALITY              1
#define PROOF_QUALITY              2
#define NONANTIALIASED_QUALITY     3
#define ANTIALIASED_QUALITY        4
#define CLEARTYPE_QUALITY          5
#define CLEARTYPE_NATURAL_QUALITY  6

#define DEFAULT_PITCH       0x00
#define FIXED_PITCH         0x01
#define VARIABLE_PITCH      0x02
#define MONO_FONT           0x08

#define FF_DONTCARE         0x00
#define FF_ROMAN            0x10
#define FF_SWISS            0x20
#define FF_MODERN           0x30
#define FF_SCRIPT           0x40
#define FF_DECORATIVE       0x50

#define TMPF_FIXED_PITCH    0x01
#define TMPF_VECTOR         0x02
#define TMPF_TRUETYPE       0x04
#define TMPF_DEVICE         0x08

typedef struct tagLOGFONTA
{
  LONG  lfHeight;
  LONG  lfWidth;
  LONG  lfEscapement;
  LONG  lfOrientation;
  LONG  lfWeight;
  BYTE  lfItalic;
  BYTE  lfUnderline;
  BYTE  lfStrikeOut;
  BYTE  lfCharSet;
  BYTE  lfOutPrecision;
  BYTE  lfClipPrecision;
  BYTE  lfQuality;
  BYTE  lfPitchAndFamily;
  CHAR  lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *LPLOGFONTA;
typedef LOGFONTA LOGFONTW;
typedef PLOGFONTA PLOGFONTW;

typedef struct tagTEXTMETRICA
{
  LONG tmHeight;
  LONG tmAscent;
  LONG tmDescent;
  LONG tmInternalLeading;
  LONG tmExternalLeading;
  LONG tmAveCharWidth;
  LONG tmMaxCharWidth;
  LONG tmWeight;
  LONG tmOverhang;
  LONG tmDigitizedAspectX;
  LONG tmDigitizedAspectY;
  BYTE tmFirstChar;
  BYTE tmLastChar;
  BYTE tmDefaultChar;
  BYTE tmBreakChar;
  BYTE tmItalic;
  BYTE tmUnderlined;
  BYTE tmStruckOut;
  BYTE tmPitchAndFamily;
  BYTE tmCharSet;
} TEXTMETRICA, *LPTEXTMETRICA, *PTEXTMETRICA;

// --- [port] wide-char aliases -------------------------------------------------
// The engine only uses the ANSI entry points (TCHAR == char here), but some
// prototypes below name the W forms, so the types must exist.  Declared before
// first use.
#ifndef APEX_WINGDI_W_TYPEDEFS
#define APEX_WINGDI_W_TYPEDEFS
typedef LOGFONTA LOGFONTW;
typedef PLOGFONTA PLOGFONTW;
typedef TEXTMETRICA TEXTMETRICW;
typedef PTEXTMETRICA PTEXTMETRICW;
typedef LPTEXTMETRICA LPTEXTMETRICW;
#endif


//////////////////////////////////////////////////////////////////////////////
// Device-independent bitmaps
//////////////////////////////////////////////////////////////////////////////

typedef struct tagRGBQUAD
{
  BYTE rgbBlue;
  BYTE rgbGreen;
  BYTE rgbRed;
  BYTE rgbReserved;
} RGBQUAD, *LPRGBQUAD;

typedef struct tagBITMAPFILEHEADER
{
  WORD  bfType;
  DWORD bfSize;
  WORD  bfReserved1;
  WORD  bfReserved2;
  DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER, *LPBITMAPFILEHEADER;

typedef struct tagBITMAPINFOHEADER
{
  DWORD biSize;
  LONG  biWidth;
  LONG  biHeight;
  WORD  biPlanes;
  WORD  biBitCount;
  DWORD biCompression;
  DWORD biSizeImage;
  LONG  biXPelsPerMeter;
  LONG  biYPelsPerMeter;
  DWORD biClrUsed;
  DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagBITMAPINFO
{
  BITMAPINFOHEADER bmiHeader;
  RGBQUAD          bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

typedef struct tagBITMAP
{
  LONG   bmType;
  LONG   bmWidth;
  LONG   bmHeight;
  LONG   bmWidthBytes;
  WORD   bmPlanes;
  WORD   bmBitsPixel;
  LPVOID bmBits;
} BITMAP, *PBITMAP, *NPBITMAP, *LPBITMAP;

typedef struct tagPALETTEENTRY
{
  BYTE peRed;
  BYTE peGreen;
  BYTE peBlue;
  BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;

#define BI_RGB        0
#define BI_RLE8       1
#define BI_RLE4       2
#define BI_BITFIELDS  3
#define BI_JPEG       4
#define BI_PNG        5

#define DIB_RGB_COLORS  0
#define DIB_PAL_COLORS  1

//////////////////////////////////////////////////////////////////////////////
// DC state constants
//////////////////////////////////////////////////////////////////////////////

#define TRANSPARENT 1
#define OPAQUE      2

#define PS_SOLID        0
#define PS_DASH         1
#define PS_DOT          2
#define PS_DASHDOT      3
#define PS_DASHDOTDOT   4
#define PS_NULL         5
#define PS_INSIDEFRAME  6
#define PS_GEOMETRIC    0x00010000
#define PS_COSMETIC     0x00000000
#define PS_ENDCAP_ROUND 0x00000000
#define PS_JOIN_ROUND   0x00000000

// GetStockObject() indices (winuser/winbase); the values are the Win32 ones.
#define WHITE_BRUSH         0
#define LTGRAY_BRUSH        1
#define GRAY_BRUSH          2
#define DKGRAY_BRUSH        3
#define BLACK_BRUSH         4
#define NULL_BRUSH          5
#define HOLLOW_BRUSH        NULL_BRUSH
#define WHITE_PEN          6
#define BLACK_PEN          7
#define NULL_PEN           8
#define OEM_FIXED_FONT     10
#define ANSI_FIXED_FONT    11
#define ANSI_VAR_FONT      12
#define SYSTEM_FONT        13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE    15
#define SYSTEM_FIXED_FONT  16
#define DEFAULT_GUI_FONT   17
#define DC_BRUSH           18
#define DC_PEN             19
#define STOCK_LAST         19

// Raster capabilities used by GetDeviceCaps().
#define DRIVERVERSION 0
#define TECHNOLOGY    2
#define HORZSIZE      4
#define VERTSIZE      6
#define HORZRES       8
#define VERTRES       10
#define BITSPIXEL     12
#define PLANES        14
#define NUMBRUSHES    16
#define NUMPENS       18
#define NUMMARKERS    20
#define NUMFONTS      22
#define NUMCOLORS     24
#define ASPECTX       40
#define ASPECTY       42
#define LOGPIXELSX    88
#define LOGPIXELSY    90
#define SIZEPALETTE   104
#define RASTERCAPS    38
#define RC_BITBLT     0x0001
#define RC_BITMAP64   0x0008
#define RC_DI_BITMAP  0x0080
#define RC_PALETTE    0x0100
#define RC_STRETCHBLT 0x0800

// Raster ops (only SRCCOPY is implemented; see gdi_posix.cpp).
#define SRCCOPY     0x00CC0020
#define SRCPAINT    0x00EE0086
#define SRCAND      0x008800C6
#define SRCINVERT   0x00660046
#define BLACKNESS   0x00000042
#define WHITENESS   0x00FF0062
#define PATCOPY     0x00F00021
#define DSTINVERT   0x00550009

// GetSystemMetrics() indices.
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CMONITORS 80
#define SM_CXMAXTRACK 59
#define SM_CYMAXTRACK 60

//////////////////////////////////////////////////////////////////////////////
// GDI entry points
//
// All of these are implemented in src/platform/gdi_posix.cpp.  Anything the
// engine never calls is a documented failure there (return 0/NULL and log
// once), never a silent stub.
//////////////////////////////////////////////////////////////////////////////

// --- DCs -------------------------------------------------------------------
HDC  WINAPI GetDC(HWND hWnd);
HDC  WINAPI GetWindowDC(HWND hWnd);
int  WINAPI ReleaseDC(HWND hWnd, HDC hDC);
HDC  WINAPI CreateCompatibleDC(HDC hDC);
BOOL WINAPI DeleteDC(HDC hDC);
int  WINAPI SaveDC(HDC hDC);
BOOL WINAPI RestoreDC(HDC hDC, int nSavedDC);
int  WINAPI GetDeviceCaps(HDC hDC, int nIndex);

// --- bitmaps ---------------------------------------------------------------
HBITMAP  WINAPI CreateCompatibleBitmap(HDC hDC, int cx, int cy);
HBITMAP  WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void *lpBits);
HBITMAP  WINAPI CreateDIBSection(HDC hDC, const BITMAPINFO *lpbmi, UINT usage, void **ppvBits, HANDLE hSection, DWORD offset);
int      WINAPI GetDIBits(HDC hDC, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT usage);
UINT     WINAPI GetObjectType(HGDIOBJ h);
int      WINAPI GetObjectA(HGDIOBJ h, int c, LPVOID pv);
int      WINAPI GetObjectW(HGDIOBJ h, int c, LPVOID pv);

// --- objects ---------------------------------------------------------------
HGDIOBJ  WINAPI SelectObject(HDC hDC, HGDIOBJ h);
BOOL     WINAPI DeleteObject(HGDIOBJ h);
HGDIOBJ  WINAPI GetStockObject(int i);
HBRUSH   WINAPI CreateSolidBrush(COLORREF color);
HPEN     WINAPI CreatePen(int iStyle, int cWidth, COLORREF color);

// --- drawing ---------------------------------------------------------------
int      WINAPI FillRect(HDC hDC, const RECT *lprc, HBRUSH hbr);
COLORREF WINAPI SetPixel(HDC hDC, int X, int Y, COLORREF crColor);
COLORREF WINAPI GetPixel(HDC hDC, int X, int Y);
BOOL     WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
BOOL     WINAPI PatBlt(HDC hdcDest, int x, int y, int w, int h, DWORD rop);

// --- text ------------------------------------------------------------------
DWORD    WINAPI GetGlyphOutlineA(HDC hDC, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, const MAT2 *lpmat2);
DWORD    WINAPI GetGlyphOutlineW(HDC hDC, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, const MAT2 *lpmat2);
HFONT    WINAPI CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                            DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName);
HFONT    WINAPI CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                            DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName);
HFONT    WINAPI CreateFontIndirectA(const LOGFONTA *lplf);
BOOL     WINAPI TextOutA(HDC hDC, int x, int y, LPCSTR lpString, int c);
BOOL     WINAPI TextOutW(HDC hDC, int x, int y, LPCWSTR lpString, int c);
int      WINAPI SetBkMode(HDC hDC, int mode);
int      WINAPI GetBkMode(HDC hDC);
COLORREF WINAPI SetTextColor(HDC hDC, COLORREF color);
COLORREF WINAPI GetTextColor(HDC hDC);
COLORREF WINAPI SetBkColor(HDC hDC, COLORREF color);
int      WINAPI SetTextCharacterExtra(HDC hDC, int nCharExtra);
int      WINAPI GetTextCharacterExtra(HDC hDC);
BOOL     WINAPI GetTextMetricsA(HDC hDC, LPTEXTMETRICA lptm);
BOOL     WINAPI GetTextMetricsW(HDC hDC, LPTEXTMETRICW lptm);
BOOL     WINAPI GetTextExtentPoint32A(HDC hDC, LPCSTR lpString, int c, LPSIZE psizl);
BOOL     WINAPI GetTextExtentPoint32W(HDC hDC, LPCWSTR lpString, int c, LPSIZE psizl);

// --- misc ------------------------------------------------------------------
int      WINAPI GetSystemMetrics(int nIndex);

// A/W aliases, exactly like the Windows SDK's !UNICODE build (compat/tchar.h
// makes TCHAR == char, so all wide paths are dead code).
//
// Deliberately NOT defined here: "GetObject".  The Windows SDK maps it to
// GetObjectA, which is a well known trap: the engine has member functions
// called GetObject() (Phoenix_Tool/Scene_tool.h), and the macro would rewrite
// those calls too.  Only GetObjectA/GetObjectW are declared.
#ifndef UNICODE
#define GetGlyphOutline       GetGlyphOutlineA
#define CreateFont            CreateFontA
#define CreateFontIndirect    CreateFontIndirectA
#define TextOut               TextOutA
#define GetTextMetrics        GetTextMetricsA
#define GetTextExtentPoint32  GetTextExtentPoint32A
#endif

#ifdef __cplusplus
}
#endif
