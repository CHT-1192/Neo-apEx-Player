// The Win32 GDI subset the engine needs, implemented on FreeType.
//
// Linux counterpart of gdi_posix.cpp (which uses CoreText/CoreGraphics).  Only
// the parts Phoenix actually reaches are here:
//
//   Phoenix/Mesh.cpp:1874-2012   CreateFontA / SelectObject / GetGlyphOutline
//                                (GGO_NATIVE, walked as TTPOLYGONHEADER records
//                                and tessellated with GLU) / gmCellIncX
//   Phoenix/Texgen*.cpp          a memory DC: CreateCompatibleDC,
//                                CreateCompatibleBitmap, SelectObject, FillRect,
//                                GetDeviceCaps, GetDIBits, DeleteObject
//   Texgen / ImageDecompressor   OleLoadPicture(Ex) into an IPicture
//
// The glyph path is the important one: Clean Slate's screens are mostly text
// meshes, so a build where text silently renders as nothing is not a port.  The
// outline serialisation below produces exactly what Mesh.cpp walks -- line
// segments and quadratic curves in 16.16 fixed point -- because FreeType's
// TrueType outlines are quadratic too.
//
// What is NOT here: rasterising text into a DC (TextOut), and the picture path's
// scaling quality (nearest neighbour).  Both log once and move on rather than
// pretending.
#include "../../compat/Windows.h"
#include "olectl.h"   // IStream / IPicture, same vtable layout as Windows

#include "rhi/vk_core.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////
// logging

namespace
{
// Constants the compat headers do not carry: compat/wingdi.h declares the
// functions but has no GDI_ERROR and no OBJ_* values.  Same values as the SDK.
const DWORD kGdiError = 0xFFFFFFFFu;
const DWORD kObjBitmap = 7;
const COLORREF kClrInvalid = 0xFFFFFFFFu;
const int kTcRasPrinter = 2;

void LogOnce(const char *key, const char *fmt, ...)
{
  static std::vector<std::string> seen;
  for (const std::string &s : seen)
    if (s == key) return;
  seen.push_back(key);
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  phxvk::LogWarn("gdi/linux: %s", buf);
}
} // namespace

//////////////////////////////////////////////////////////////////////////////
// objects

namespace
{
struct GdiFont
{
  std::string family;
  int height = 12;      // GDI cell height, positive
  int charset = 0;
  bool symbol = false;

  FT_Face face = nullptr;
  int pixelHeight = 12;
};

struct GdiBitmap
{
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba; // RGBA8, top row first
};

struct GdiDC
{
  GdiBitmap *bitmap = nullptr;
  GdiFont *font = nullptr;
  COLORREF textColor = 0;
  COLORREF bkColor = 0x00FFFFFF;
  int bkMode = OPAQUE;
  int charExtra = 0;
  bool ownsBitmap = false;

  struct Saved
  {
    GdiFont *font;
    COLORREF textColor, bkColor;
    int bkMode, charExtra;
  };
  std::vector<Saved> stack;
};

FT_Library g_ft = nullptr;
std::vector<GdiBitmap *> g_bitmaps;
} // namespace

namespace
{
bool EnsureFreeType()
{
  if (g_ft) return true;
  if (FT_Init_FreeType(&g_ft) != 0)
  {
    LogOnce("ft.init", "FreeType failed to initialise; text meshes will be empty");
    g_ft = nullptr;
    return false;
  }
  return true;
}

// Font lookup.  fc-match knows the system's fonts and their aliases, which is
// the difference between "Arial Black" finding something and finding nothing on
// a machine that has never heard of Arial.
std::string FindFontFile(const char *family)
{
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "fc-match -f '%%{file}' '%s' 2>/dev/null", family);
  FILE *p = popen(cmd, "r");
  if (!p) return std::string();
  char path[1024] = {0};
  if (!fgets(path, sizeof(path), p))
  {
    pclose(p);
    return std::string();
  }
  pclose(p);
  std::string s(path);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

bool OpenFont(GdiFont *font)
{
  if (!font || font->face) return font != nullptr;
  if (!EnsureFreeType()) return false;

  std::string file = FindFontFile(font->family.c_str());
  if (file.empty() || file == "unknown")
  {
    LogOnce(std::string("font." + font->family).c_str(), "no font matches '%s'; text using it will be empty",
            font->family.c_str());
    return false;
  }
  FT_Face face = nullptr;
  if (FT_New_Face(g_ft, file.c_str(), 0, &face) != 0 || !face)
  {
    LogOnce(std::string("face." + font->family).c_str(), "cannot open %s for '%s'", file.c_str(), font->family.c_str());
    return false;
  }
  // GDI's lfHeight is the cell height: ascent + descent.  FreeType's pixel size
  // is nominal, so scale until the cell height matches what was asked for.
  font->pixelHeight = font->height > 0 ? font->height : 12;
  FT_Set_Pixel_Sizes(face, 0, (FT_UInt)font->pixelHeight);
  const int cell = (int)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
  if (cell > 0 && font->height > 0)
  {
    const int adjusted = (int)std::lround((double)font->pixelHeight * (double)font->height / (double)cell);
    if (adjusted > 0 && adjusted != font->pixelHeight)
    {
      font->pixelHeight = adjusted;
      FT_Set_Pixel_Sizes(face, 0, (FT_UInt)font->pixelHeight);
    }
  }
  font->face = face;
  return true;
}

// Win32 FIXED is a pair of 16-bit halves, not an int: `value` holds the signed
// integer part and `fract` the fraction.  Mesh.cpp reads them with
// `*(int*)&x / 65536.0f`, i.e. as a little-endian 32-bit 16.16 -- which is the
// same bytes this packing produces.
inline double FixedToDouble(const FIXED &f)
{
  return (double)((int)f.value * 65536 + (int)f.fract) / 65536.0;
}

inline void SetFixed(FIXED &f, double pixels)
{
  const long long v = (long long)std::llround(pixels * 65536.0);
  f.fract = (WORD)(v & 0xFFFF);
  f.value = (SHORT)(v >> 16);
}

// 26.6 (FreeType pixels) -> FIXED
inline void SetFixed26_6(FIXED &f, long v) { SetFixed(f, (double)v / 64.0); }
} // namespace

//////////////////////////////////////////////////////////////////////////////
// GGO_NATIVE serialisation
//
// Mesh.cpp walks the buffer as a sequence of TTPOLYGONHEADER, each followed by
// TTPOLYCURVE records until hdr->cb bytes are consumed.  Everything is in device
// pixels as 16.16 fixed point.

namespace
{
struct OutlineWriter
{
  std::vector<uint8_t> *out = nullptr;
  size_t polygonStart = 0;
  size_t curveStart = 0;
  WORD curveType = 0;
  int curveCount = 0;
  POINTFX last = {};

  void BeginPolygon(POINTFX start)
  {
    FlushCurve();
    polygonStart = out->size();
    TTPOLYGONHEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cb = sizeof(TTPOLYGONHEADER); // patched by EndPolygon
    hdr.dwType = TT_POLYGON_TYPE;
    hdr.pfxStart = start;
    const uint8_t *p = (const uint8_t *)&hdr;
    out->insert(out->end(), p, p + sizeof(hdr));
    curveStart = 0;
    curveCount = 0;
    last = start;
  }

  void EnsureCurve(WORD type)
  {
    if (curveType == type && curveStart) return;
    FlushCurve();
    curveType = type;
    curveStart = out->size();
    TTPOLYCURVE rec;
    memset(&rec, 0, sizeof(rec));
    rec.wType = type;
    rec.cpfx = 0;
    const uint8_t *p = (const uint8_t *)&rec;
    out->insert(out->end(), p, p + offsetof(TTPOLYCURVE, apfx));
  }

  void AddPoint(POINTFX pt)
  {
    const uint8_t *p = (const uint8_t *)&pt;
    out->insert(out->end(), p, p + sizeof(pt));
    curveCount++;
    last = pt;
  }

  void Line(POINTFX pt)
  {
    EnsureCurve(TT_PRIM_LINE);
    AddPoint(pt);
  }

  void Conic(POINTFX ctrl, POINTFX to)
  {
    EnsureCurve(TT_PRIM_QSPLINE);
    AddPoint(ctrl);
    AddPoint(to);
  }

  // Cubics (CFF/OTF fonts) are sampled into line segments: Mesh.cpp has no
  // TT_PRIM_CSPLINE branch, so emitting one would silently drop the curve.
  void Cubic(POINTFX c1, POINTFX c2, POINTFX to)
  {
    const double x0 = FixedToDouble(last.x), y0 = FixedToDouble(last.y);
    const double x1 = FixedToDouble(c1.x), y1 = FixedToDouble(c1.y);
    const double x2 = FixedToDouble(c2.x), y2 = FixedToDouble(c2.y);
    const double x3 = FixedToDouble(to.x), y3 = FixedToDouble(to.y);
    const int steps = 8;
    for (int i = 1; i <= steps; i++)
    {
      const double t = (double)i / steps, u = 1.0 - t;
      POINTFX p;
      SetFixed(p.x, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3);
      SetFixed(p.y, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
      Line(p);
    }
  }

  void FlushCurve()
  {
    if (!curveStart || !curveCount) { curveStart = 0; curveType = 0; curveCount = 0; return; }
    TTPOLYCURVE *rec = (TTPOLYCURVE *)&(*out)[curveStart];
    rec->cpfx = (WORD)curveCount;
    curveStart = 0;
    curveType = 0;
    curveCount = 0;
  }

  void EndPolygon()
  {
    FlushCurve();
    if (!polygonStart) { polygonStart = 0; return; }
    TTPOLYGONHEADER *hdr = (TTPOLYGONHEADER *)&(*out)[polygonStart];
    hdr->cb = (DWORD)(out->size() - polygonStart);
    polygonStart = 0;
  }
};

int MoveTo(const FT_Vector *to, void *user)
{
  OutlineWriter &w = *(OutlineWriter *)user;
  w.EndPolygon();
  POINTFX p;
  SetFixed26_6(p.x, to->x);
  SetFixed26_6(p.y, to->y);
  w.BeginPolygon(p);
  return 0;
}

int LineTo(const FT_Vector *to, void *user)
{
  OutlineWriter &w = *(OutlineWriter *)user;
  POINTFX p;
  SetFixed26_6(p.x, to->x);
  SetFixed26_6(p.y, to->y);
  w.Line(p);
  return 0;
}

int ConicTo(const FT_Vector *ctrl, const FT_Vector *to, void *user)
{
  OutlineWriter &w = *(OutlineWriter *)user;
  POINTFX c, t;
  SetFixed26_6(c.x, ctrl->x);
  SetFixed26_6(c.y, ctrl->y);
  SetFixed26_6(t.x, to->x);
  SetFixed26_6(t.y, to->y);
  w.Conic(c, t);
  return 0;
}

int CubicTo(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user)
{
  OutlineWriter &w = *(OutlineWriter *)user;
  POINTFX a, b, t;
  SetFixed26_6(a.x, c1->x);
  SetFixed26_6(a.y, c1->y);
  SetFixed26_6(b.x, c2->x);
  SetFixed26_6(b.y, c2->y);
  SetFixed26_6(t.x, to->x);
  SetFixed26_6(t.y, to->y);
  w.Cubic(a, b, t);
  return 0;
}
} // namespace

//////////////////////////////////////////////////////////////////////////////
// GDI entry points

extern "C" {
extern "C" const GUID IID_IPicture = { 0x7BF80980, 0xBF32, 0x101A, { 0x8B, 0xBB, 0x00, 0xAA, 0x00, 0x30, 0x0C, 0xAB } };


// The engine asks for this IID by name (Texgen.cpp:360), so the symbol has to
// exist even though the picture path itself is not implemented here.
// IID_IPicture is declared `extern const GUID` by compat/olectl.h and is the

HDC WINAPI GetDC(HWND)
{
  GdiDC *dc = new GdiDC();
  return (HDC)dc;
}

HDC WINAPI GetWindowDC(HWND)
{
  return GetDC(nullptr);
}

int WINAPI ReleaseDC(HWND, HDC hDC)
{
  return DeleteDC(hDC);
}

HDC WINAPI CreateCompatibleDC(HDC hDC)
{
  GdiDC *dc = new GdiDC();
  GdiDC *src = (GdiDC *)hDC;
  if (src)
  {
    dc->textColor = src->textColor;
    dc->bkColor = src->bkColor;
    dc->bkMode = src->bkMode;
  }
  return (HDC)dc;
}

BOOL WINAPI DeleteDC(HDC hDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return FALSE;
  if (dc->ownsBitmap && dc->bitmap) delete dc->bitmap;
  delete dc;
  return TRUE;
}

int WINAPI SaveDC(HDC hDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return 0;
  dc->stack.push_back({dc->font, dc->textColor, dc->bkColor, dc->bkMode, dc->charExtra});
  return (int)dc->stack.size();
}

BOOL WINAPI RestoreDC(HDC hDC, int nSavedDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || dc->stack.empty()) return FALSE;
  if (nSavedDC < 0) nSavedDC = (int)dc->stack.size();
  const size_t idx = (size_t)nSavedDC - 1;
  if (idx >= dc->stack.size()) return FALSE;
  const GdiDC::Saved &s = dc->stack[idx];
  dc->font = s.font;
  dc->textColor = s.textColor;
  dc->bkColor = s.bkColor;
  dc->bkMode = s.bkMode;
  dc->charExtra = s.charExtra;
  dc->stack.resize(idx);
  return TRUE;
}

int WINAPI GetDeviceCaps(HDC hDC, int nIndex)
{
  GdiDC *dc = (GdiDC *)hDC;
  switch (nIndex)
  {
    case BITSPIXEL: return 32;
    case PLANES: return 1;
    case NUMCOLORS: return -1;
    case HORZRES: return dc && dc->bitmap ? dc->bitmap->width : 1024;
    case VERTRES: return dc && dc->bitmap ? dc->bitmap->height : 1024;
    case LOGPIXELSY: return 96;
    case TECHNOLOGY: return kTcRasPrinter;
    default: return 0;
  }
}

HBITMAP WINAPI CreateCompatibleBitmap(HDC hDC, int cx, int cy)
{
  GdiBitmap *bm = new GdiBitmap();
  bm->width = cx > 0 ? cx : 1;
  bm->height = cy > 0 ? cy : 1;
  bm->rgba.assign((size_t)bm->width * bm->height * 4, 0);
  g_bitmaps.push_back(bm);
  (void)hDC;
  return (HBITMAP)bm;
}

HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT, UINT, const void *lpBits)
{
  HBITMAP h = CreateCompatibleBitmap(nullptr, nWidth, nHeight);
  if (lpBits && h)
  {
    GdiBitmap *bm = (GdiBitmap *)h;
    memcpy(bm->rgba.data(), lpBits, (size_t)bm->width * bm->height * 4);
  }
  return h;
}

HBITMAP WINAPI CreateDIBSection(HDC hDC, const BITMAPINFO *lpbmi, UINT, void **ppvBits, HANDLE, DWORD)
{
  if (!lpbmi) return nullptr;
  HBITMAP h = CreateCompatibleBitmap(hDC, lpbmi->bmiHeader.biWidth, lpbmi->bmiHeader.biHeight);
  if (ppvBits && h) *ppvBits = ((GdiBitmap *)h)->rgba.data();
  return h;
}

// Windows DIBs are bottom-up BGRA by default; biHeight < 0 means top-down.
int WINAPI GetDIBits(HDC hDC, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT)
{
  GdiBitmap *bm = (GdiBitmap *)hbm;
  if (!bm || !lpbmi) return 0;
  const int bpp = lpbmi->bmiHeader.biBitCount;
  const bool topDown = lpbmi->bmiHeader.biHeight < 0;
  const int height = bm->height;
  const UINT lines = cLines == 0 ? (UINT)height : cLines;
  if (lpvBits)
  {
    uint8_t *dst = (uint8_t *)lpvBits;
    for (UINT y = 0; y < lines; y++)
    {
      const int sy = topDown ? (int)(start + y) : height - 1 - (int)(start + y);
      if (sy < 0 || sy >= height) continue;
      const uint8_t *src = bm->rgba.data() + (size_t)sy * bm->width * 4;
      for (int x = 0; x < bm->width; x++)
      {
        const uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1], b = src[x * 4 + 2], a = src[x * 4 + 3];
        if (bpp == 32)
        {
          dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = a;
          dst += 4;
        }
        else if (bpp == 24)
        {
          dst[0] = b; dst[1] = g; dst[2] = r;
          dst += 3;
        }
      }
      if (bpp == 24)
      {
        // DIB rows are 4-byte aligned.
        while (((uintptr_t)dst - (uintptr_t)lpvBits) % 4) *dst++ = 0;
      }
    }
    (void)hDC;
    return (int)lines;
  }
  lpbmi->bmiHeader.biHeight = height;
  lpbmi->bmiHeader.biWidth = bm->width;
  lpbmi->bmiHeader.biPlanes = 1;
  return height;
}

UINT WINAPI GetObjectType(HGDIOBJ h)
{
  if (!h) return 0;
  return kObjBitmap;
}

int WINAPI GetObjectA(HGDIOBJ h, int c, LPVOID pv)
{
  if (!h || !pv) return 0;
  BITMAP *bm = (BITMAP *)pv;
  if (c < (int)sizeof(BITMAP)) return 0;
  GdiBitmap *src = (GdiBitmap *)h;
  memset(bm, 0, sizeof(*bm));
  bm->bmType = 0;
  bm->bmWidth = src->width;
  bm->bmHeight = src->height;
  bm->bmWidthBytes = (LONG)src->width * 4;
  bm->bmBitsPixel = 32;
  bm->bmPlanes = 1;
  bm->bmBits = src->rgba.data();
  return (int)sizeof(BITMAP);
}

int WINAPI GetObjectW(HGDIOBJ h, int c, LPVOID pv) { return GetObjectA(h, c, pv); }

HGDIOBJ WINAPI SelectObject(HDC hDC, HGDIOBJ h)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return nullptr;
  // The engine selects both fonts and bitmaps; the object kind is not carried
  // in the handle, so both registries are consulted.
  for (GdiBitmap *bm : g_bitmaps)
    if ((HGDIOBJ)bm == h)
    {
      if (dc->ownsBitmap && dc->bitmap) delete dc->bitmap;
      dc->bitmap = bm;
      dc->ownsBitmap = false;
      return (HGDIOBJ)bm;
    }
  dc->font = (GdiFont *)h;
  return h;
}

BOOL WINAPI DeleteObject(HGDIOBJ h)
{
  if (!h) return FALSE;
  for (size_t i = 0; i < g_bitmaps.size(); i++)
    if ((HGDIOBJ)g_bitmaps[i] == h)
    {
      delete g_bitmaps[i];
      g_bitmaps.erase(g_bitmaps.begin() + i);
      return TRUE;
    }
  delete (GdiFont *)h;
  return TRUE;
}

HGDIOBJ WINAPI GetStockObject(int i)
{
  static GdiFont *fonts[32];
  const int idx = i & 31;
  if (!fonts[idx]) fonts[idx] = new GdiFont();
  return (HGDIOBJ)fonts[idx];
}

HBRUSH WINAPI CreateSolidBrush(COLORREF color) { return (HBRUSH)(uintptr_t)(color | 0xFF000000u); }
HPEN WINAPI CreatePen(int, int, COLORREF color) { return (HPEN)(uintptr_t)(color | 0xFE000000u); }

int WINAPI FillRect(HDC hDC, const RECT *lprc, HBRUSH hbr)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->bitmap || !lprc) return 0;
  const COLORREF c = (COLORREF)(uintptr_t)hbr;
  const int x0 = lprc->left < 0 ? 0 : lprc->left;
  const int y0 = lprc->top < 0 ? 0 : lprc->top;
  const int x1 = lprc->right > dc->bitmap->width ? dc->bitmap->width : lprc->right;
  const int y1 = lprc->bottom > dc->bitmap->height ? dc->bitmap->height : lprc->bottom;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
    {
      uint8_t *p = &dc->bitmap->rgba[((size_t)y * dc->bitmap->width + x) * 4];
      p[0] = GetRValue(c); p[1] = GetGValue(c); p[2] = GetBValue(c); p[3] = 255;
    }
  return 1;
}

COLORREF WINAPI SetPixel(HDC hDC, int X, int Y, COLORREF crColor)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->bitmap || X < 0 || Y < 0 || X >= dc->bitmap->width || Y >= dc->bitmap->height) return crColor;
  uint8_t *p = &dc->bitmap->rgba[((size_t)Y * dc->bitmap->width + X) * 4];
  p[0] = GetRValue(crColor); p[1] = GetGValue(crColor); p[2] = GetBValue(crColor); p[3] = 255;
  return crColor;
}

COLORREF WINAPI GetPixel(HDC hDC, int X, int Y)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->bitmap || X < 0 || Y < 0 || X >= dc->bitmap->width || Y >= dc->bitmap->height) return kClrInvalid;
  const uint8_t *p = &dc->bitmap->rgba[((size_t)Y * dc->bitmap->width + X) * 4];
  return RGB(p[0], p[1], p[2]);
}

BOOL WINAPI PatBlt(HDC hdcDest, int x, int y, int w, int h, DWORD rop)
{
  RECT r = {x, y, x + w, y + h};
  GdiDC *dc = (GdiDC *)hdcDest;
  const COLORREF c = rop == BLACKNESS ? 0 : 0x00FFFFFF;
  (void)dc;
  return FillRect(hdcDest, &r, (HBRUSH)(uintptr_t)(c | 0xFF000000u)) != 0;
}

BOOL WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD)
{
  GdiDC *dst = (GdiDC *)hdcDest;
  GdiDC *src = (GdiDC *)hdcSrc;
  if (!dst || !src || !dst->bitmap || !src->bitmap) return FALSE;
  for (int row = 0; row < cy; row++)
    for (int col = 0; col < cx; col++)
    {
      const int sx = x1 + col, sy = y1 + row, dx = x + col, dy = y + row;
      if (sx < 0 || sy < 0 || sx >= src->bitmap->width || sy >= src->bitmap->height) continue;
      if (dx < 0 || dy < 0 || dx >= dst->bitmap->width || dy >= dst->bitmap->height) continue;
      memcpy(&dst->bitmap->rgba[((size_t)dy * dst->bitmap->width + dx) * 4],
             &src->bitmap->rgba[((size_t)sy * src->bitmap->width + sx) * 4], 4);
    }
  return TRUE;
}

int WINAPI SetBkMode(HDC hDC, int mode)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return 0;
  const int old = dc->bkMode;
  dc->bkMode = mode;
  return old;
}

int WINAPI GetBkMode(HDC hDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  return dc ? dc->bkMode : OPAQUE;
}

COLORREF WINAPI SetTextColor(HDC hDC, COLORREF color)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return 0;
  const COLORREF old = dc->textColor;
  dc->textColor = color;
  return old;
}

COLORREF WINAPI GetTextColor(HDC hDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  return dc ? dc->textColor : 0;
}

COLORREF WINAPI SetBkColor(HDC hDC, COLORREF color)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return 0;
  const COLORREF old = dc->bkColor;
  dc->bkColor = color;
  return old;
}

int WINAPI SetTextCharacterExtra(HDC hDC, int nCharExtra)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc) return 0;
  const int old = dc->charExtra;
  dc->charExtra = nCharExtra;
  return old;
}

int WINAPI GetTextCharacterExtra(HDC hDC)
{
  GdiDC *dc = (GdiDC *)hDC;
  return dc ? dc->charExtra : 0;
}

HFONT WINAPI CreateFontA(int cHeight, int cWidth, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                         DWORD, LPCSTR lpszFace)
{
  (void)cWidth;
  GdiFont *f = new GdiFont();
  f->height = cHeight < 0 ? -cHeight : cHeight;
  if (f->height <= 0) f->height = 12;
  f->family = lpszFace ? lpszFace : "sans";
  return (HFONT)f;
}

HFONT WINAPI CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR)
{
  return nullptr; // the engine only uses the A form; say so by failing
}

HFONT WINAPI CreateFontIndirectA(const LOGFONTA *lplf)
{
  if (!lplf) return nullptr;
  GdiFont *f = new GdiFont();
  f->height = lplf->lfHeight < 0 ? -lplf->lfHeight : lplf->lfHeight;
  if (f->height <= 0) f->height = 12;
  f->family = lplf->lfFaceName;
  f->charset = lplf->lfCharSet;
  f->symbol = lplf->lfCharSet == SYMBOL_CHARSET;
  return (HFONT)f;
}

BOOL WINAPI GetTextMetricsA(HDC hDC, LPTEXTMETRICA lptm)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !lptm) return FALSE;
  GdiFont *f = dc->font;
  if (!f || !OpenFont(f)) return FALSE;
  const FT_Size_Metrics &m = f->face->size->metrics;
  memset(lptm, 0, sizeof(*lptm));
  lptm->tmHeight = (LONG)((m.ascender - m.descender) >> 6);
  lptm->tmAscent = (LONG)(m.ascender >> 6);
  lptm->tmDescent = (LONG)(-m.descender >> 6);
  lptm->tmInternalLeading = 0;
  lptm->tmExternalLeading = 0;
  lptm->tmMaxCharWidth = (LONG)(m.max_advance >> 6);
  lptm->tmAveCharWidth = (LONG)(m.max_advance >> 7);
  lptm->tmPitchAndFamily = 0x22;
  return TRUE;
}

BOOL WINAPI GetTextMetricsW(HDC hDC, LPTEXTMETRICW lptm)
{
  TEXTMETRICA a;
  if (!GetTextMetricsA(hDC, &a)) return FALSE;
  memset(lptm, 0, sizeof(*lptm));
  lptm->tmHeight = a.tmHeight;
  lptm->tmAscent = a.tmAscent;
  lptm->tmDescent = a.tmDescent;
  lptm->tmMaxCharWidth = a.tmMaxCharWidth;
  lptm->tmAveCharWidth = a.tmAveCharWidth;
  lptm->tmPitchAndFamily = a.tmPitchAndFamily;
  return TRUE;
}

static FT_UInt GlyphFor(GdiFont *f, UINT ch)
{
  if (!f || !OpenFont(f)) return 0;
  if (f->symbol && ch < (UINT)f->face->num_glyphs) return (FT_UInt)ch;
  return FT_Get_Char_Index(f->face, ch);
}

DWORD WINAPI GetGlyphOutlineA(HDC hDC, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer,
                              const MAT2 *)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->font || fuFormat != GGO_NATIVE) return kGdiError;
  GdiFont *f = dc->font;
  if (!OpenFont(f)) return kGdiError;

  const FT_UInt glyph = GlyphFor(f, uChar);
  // Scale to pixels first, so the outline is in device space like GDI's.
  if (FT_Load_Glyph(f->face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return kGdiError;

  if (lpgm)
  {
    FT_GlyphSlot slot = f->face->glyph;
    lpgm->gmBlackBoxX = (UINT)(slot->metrics.width >> 6);
    lpgm->gmBlackBoxY = (UINT)(slot->metrics.height >> 6);
    lpgm->gmptGlyphOrigin.x = (LONG)(slot->metrics.horiBearingX >> 6);
    lpgm->gmptGlyphOrigin.y = (LONG)(slot->metrics.horiBearingY >> 6);
    lpgm->gmCellIncX = (short)(slot->metrics.horiAdvance >> 6);
    lpgm->gmCellIncY = 0;
  }

  std::vector<uint8_t> buf;
  OutlineWriter writer;
  writer.out = &buf;
  FT_Outline_Funcs funcs;
  funcs.move_to = MoveTo;
  funcs.line_to = LineTo;
  funcs.conic_to = ConicTo;
  funcs.cubic_to = CubicTo;
  funcs.shift = 0;
  funcs.delta = 0;
  if (FT_Outline_Decompose(&f->face->glyph->outline, &funcs, &writer) != 0) return kGdiError;
  writer.EndPolygon();

  if (!cjBuffer || !pvBuffer) return (DWORD)buf.size();
  const DWORD n = cjBuffer < (DWORD)buf.size() ? cjBuffer : (DWORD)buf.size();
  memcpy(pvBuffer, buf.data(), n);
  return n;
}

DWORD WINAPI GetGlyphOutlineW(HDC hDC, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer,
                              const MAT2 *lpmat2)
{
  return GetGlyphOutlineA(hDC, uChar, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);
}

BOOL WINAPI TextOutA(HDC, int, int, LPCSTR, int)
{
  LogOnce("textout", "TextOut is not implemented: only glyph outlines (GGO_NATIVE) are, which is what the text meshes use");
  return TRUE;
}

BOOL WINAPI TextOutW(HDC, int, int, LPCWSTR, int)
{
  return TextOutA(nullptr, 0, 0, nullptr, 0);
}

BOOL WINAPI GetTextExtentPoint32A(HDC hDC, LPCSTR lpString, int c, LPSIZE psizl)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->font || !psizl) return FALSE;
  GdiFont *f = dc->font;
  if (!OpenFont(f)) return FALSE;
  int total = 0;
  for (int i = 0; i < c; i++)
  {
    const FT_UInt g = GlyphFor(f, (UINT)(unsigned char)lpString[i]);
    if (FT_Load_Glyph(f->face, g, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0)
      total += (int)(f->face->glyph->metrics.horiAdvance >> 6) + dc->charExtra;
  }
  psizl->cx = total;
  psizl->cy = (LONG)((f->face->size->metrics.ascender - f->face->size->metrics.descender) >> 6);
  return TRUE;
}

BOOL WINAPI GetTextExtentPoint32W(HDC hDC, LPCWSTR lpString, int c, LPSIZE psizl)
{
  GdiDC *dc = (GdiDC *)hDC;
  if (!dc || !dc->font || !psizl) return FALSE;
  GdiFont *f = dc->font;
  if (!OpenFont(f)) return FALSE;
  int total = 0;
  for (int i = 0; i < c; i++)
    if (FT_Load_Char(f->face, (FT_ULong)lpString[i], FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0)
      total += (int)(f->face->glyph->metrics.horiAdvance >> 6);
  psizl->cx = total;
  psizl->cy = (LONG)((f->face->size->metrics.ascender - f->face->size->metrics.descender) >> 6);
  return TRUE;
}

int WINAPI GetSystemMetrics(int nIndex)
{
  switch (nIndex)
  {
    case SM_CXSCREEN: return 1920;
    case SM_CYSCREEN: return 1080;
    default: return 0;
  }
}

// The picture path needs a COM IStream and IPicture.  Decoding is available
// (image_stb.cpp), but wiring it up is a separate job; failing honestly here is
// better than an IPicture whose get_Width returns zero.
IStream *WINAPI SHCreateMemStream(const BYTE *, UINT)
{
  LogOnce("picture.stream", "SHCreateMemStream is not implemented on Linux; texgen picture nodes will be skipped");
  return nullptr;
}

HRESULT WINAPI OleLoadPictureEx(LPSTREAM, LONG, BOOL, REFIID, DWORD, DWORD, DWORD, LPVOID *lplpvObj)
{
  LogOnce("picture.load", "OleLoadPicture is not implemented on Linux; texgen picture nodes will be skipped");
  if (lplpvObj) *lplpvObj = nullptr;
  return E_FAIL;
}

HRESULT WINAPI OleLoadPicture(LPSTREAM lpstream, LONG lSize, BOOL fRunmode, REFIID riid, LPVOID *lplpvObj)
{
  return OleLoadPictureEx(lpstream, lSize, fRunmode, riid, 0, 0, 0, lplpvObj);
}

} // extern "C"
