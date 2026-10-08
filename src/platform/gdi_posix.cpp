// src/platform/gdi_posix.cpp
//
// GDI subset for macOS, implemented on CoreText / CoreGraphics / ImageIO.
//
// This is the implementation half of compat/wingdi.h (+ compat/olectl.h): the
// portable replacement for the Win32 calls the vendored apEx runtime makes.
//
// The call sites this file exists for (verified with
//
//   grep -rnE "\b(GetDC|...|GetSystemMetrics)\b" third_party/apEx/Phoenix \
//                                              third_party/apEx/Phoenix_Tool
//
// and all of them were read before anything here was written):
//
//   Phoenix/Mesh.cpp:1874-2012     CreateFontA/SelectObject/GetGlyphOutline
//                                  (GGO_NATIVE)/DeleteObject  -- text meshes
//   Phoenix/Texgen.cpp:187         GetDC(NULL)                -- the shared DC
//   Phoenix/Texgen.cpp:257-298     CreateCompatibleDC/CreateCompatibleBitmap/
//                                  CreateFontA/SelectObject/SetBkMode/
//                                  SetTextColor/SetTextCharacterExtra/TextOut/
//                                  GetDIBits/DeleteDC/DeleteObject -- text
//                                  texture filter
//   Phoenix/Texgen.cpp:352-406     SHCreateMemStream/OleLoadPictureEx/
//                                  IPicture::Render/FillRect/GetStockObject/
//                                  GetDIBits  -- vector image filter
//                                  (compiled: PhoenixConfig.h:5 defines
//                                  PHX_TEXGEN_IMAGE for this port)
//   Phoenix/Project.cpp:290        GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)
//   Phoenix_Tool/apxProject.cpp:1772  same
//
// What is implemented exactly, approximated or refused is documented at each
// entry point below; the short version is in the report accompanying the
// commit.  Two rules were followed throughout:
//
//   * No C++ exception ever crosses the C API boundary: every entry point is
//     wrapped, and a caught exception is logged (once) and turned into the
//     Win32 failure code.
//   * Nothing is silently faked.  Unsupported requests return GDI_ERROR /
//     FALSE / NULL and log once with a "[gdi_posix]" line, so a missing
//     feature shows up in a run's stderr instead of as wrong pixels.
//
// Layout contract (Mesh.cpp walks the outline with its own pointer arithmetic,
// so these must match the Windows SDK byte for byte -- they are the compat
// structs, and the static_asserts below pin them):
//
//   TTPOLYGONHEADER  16 bytes: DWORD cb, DWORD dwType, POINTFX pfxStart
//   TTPOLYCURVE       4 + 8*cpfx bytes: WORD wType, WORD cpfx, POINTFX apfx[]
//   POINTFX           8 bytes: two 16.16 FIXED
//
// Every contour block is a multiple of 4 bytes (the header is 16, each curve
// record is 4 + 8*cpfx), so Win32's 4-byte alignment padding is never needed.

// The Apple frameworks are included *before* the compat Windows headers: when
// compat/D3Dcommon.h (pulled in by olectl.h) defines `interface` as `struct`
// -- as the real d3dcommon.h does -- it collides with IOKit's
// IOHIDTypes.h, which has a struct member literally named `interface`.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include "Windows.h"
#include "wingdi.h"
#include "olectl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// IID_IPicture is declared by compat/olectl.h (extern "C", defined below);
// these two are not declared anywhere in compat/, so they live here.
namespace
{
// compat/Windows.h defines the Windows `min`/`max` function-like macros, which
// make `std::min(...)`/`std::max(...)` unspellable in this translation unit.
template <typename T>
inline T MinT(T a, T b) { return a < b ? a : b; }
template <typename T>
inline T MaxT(T a, T b) { return a > b ? a : b; }

const GUID kIID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IStream = { 0x0000000C, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
} // namespace

//////////////////////////////////////////////////////////////////////////////
// Constants the compat headers do not carry
//
// compat/wingdi.h declares the functions but has no GDI_ERROR and no OBJ_*
// values (reported, not patched -- the header is not mine to edit).
//////////////////////////////////////////////////////////////////////////////

namespace
{

const DWORD kGdiError = 0xFFFFFFFFu;

// GetObjectType() values from the Windows SDK.
const DWORD kObjPen = 1, kObjBrush = 2, kObjDC = 3, kObjFont = 6, kObjBitmap = 7, kObjMemDC = 10;

} // namespace

static_assert(sizeof(FIXED) == 4, "FIXED must be 16.16 split exactly like the SDK");
static_assert(sizeof(POINTFX) == 8, "POINTFX must be two packed FIXED");
static_assert(sizeof(TTPOLYCURVE) == 12, "TTPOLYCURVE is a 4-byte header plus apfx[1]");
static_assert(sizeof(TTPOLYGONHEADER) == 16, "TTPOLYGONHEADER must pack without padding");

namespace
{

//////////////////////////////////////////////////////////////////////////////
// Logging
//
// One line per distinct message key per process.  Deliberately allocation-light
// and exception-proof: this is called from the outside of a catch handler as
// well as from failure paths inside the API.
//////////////////////////////////////////////////////////////////////////////

// One mutex + one "already logged" set per process.
//
// These live in function-local statics that are intentionally *never*
// destroyed (the classic C-API leaky singleton): Phoenix/Texgen.cpp:187 is
// `HDC hdc = GetDC(NULL);` at namespace scope, so this whole layer also runs
// during static construction, and a namespace-scope std::set here would still
// be unconstructed at that point.  Leaking also removes any exit-order hazard
// from engine globals whose destructors call DeleteObject/DeleteDC.
std::mutex &LogMutex()
{
  static std::mutex *m = new std::mutex();
  return *m;
}

std::set<std::string> &Logged()
{
  static std::set<std::string> *s = new std::set<std::string>();
  return *s;
}

void LogOnce(const char *key, const char *fmt, ...)
{
  try
  {
    {
      std::lock_guard<std::mutex> lock(LogMutex());
      if (!Logged().insert(std::string(key)).second) return;
    }
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[gdi_posix] %s\n", msg);
    fflush(stderr);
  }
  catch (...)
  {
    // A diagnostic must never be the thing that kills a frame.
  }
}

void RefuseOnce(const char *key, const char *what)
{
  LogOnce(key, "%s is not implemented by the CoreText/CoreGraphics GDI port; returning failure (this message is printed once)", what);
}

//////////////////////////////////////////////////////////////////////////////
// 16.16 fixed point (Win32 FIXED)
//
// Mesh.cpp reads the raw 32-bit pattern as `value << 16 | fract`, so the
// round trip below is the identity: build the int32 two's-complement value and
// split it into the SDK's {WORD fract; SHORT value;} fields.
//////////////////////////////////////////////////////////////////////////////

inline FIXED FixedFromDouble(double v)
{
  const int32_t raw = (int32_t)llround(v * 65536.0);
  FIXED f;
  f.fract = (WORD)(uint16_t)((uint32_t)raw & 0xFFFFu);
  f.value = (SHORT)(int16_t)(((uint32_t)raw >> 16) & 0xFFFFu);
  return f;
}

inline double FixedToDouble(const FIXED &f)
{
  const uint32_t raw = (uint32_t)(uint16_t)f.fract | ((uint32_t)(uint16_t)f.value << 16);
  return (double)(int32_t)raw / 65536.0;
}

//////////////////////////////////////////////////////////////////////////////
// MAT2
//////////////////////////////////////////////////////////////////////////////

struct Mat2
{
  double m11 = 1.0, m12 = 0.0, m21 = 0.0, m22 = 1.0;
};

Mat2 MatFromMAT2(const MAT2 *m)
{
  Mat2 r;
  if (!m) return r;
  // GDI applies the matrix to the row vector: [x y] * [[eM11 eM12][eM21 eM22]].
  r.m11 = FixedToDouble(m->eM11);
  r.m12 = FixedToDouble(m->eM12);
  r.m21 = FixedToDouble(m->eM21);
  r.m22 = FixedToDouble(m->eM22);
  return r;
}

inline void ApplyMat(const Mat2 &m, double x, double y, double &ox, double &oy)
{
  ox = x * m.m11 + y * m.m21;
  oy = x * m.m12 + y * m.m22;
}

//////////////////////////////////////////////////////////////////////////////
// Object model
//
// Every GDI handle is a pointer to one of these structs.  Handles are looked up
// in the live-object table *before* being dereferenced, so a wild or already-deleted handle
// is rejected instead of read (important because the engine hands back handles
// it stored in project data).
//////////////////////////////////////////////////////////////////////////////

enum : uint32_t
{
  kMagicDC = 0x47444320u,     // 'GDC '
  kMagicBitmap = 0x47424954u, // 'GBIT'
  kMagicFont = 0x47464f4eu,   // 'GFON'
  kMagicBrush = 0x47425255u,  // 'GBRU'
  kMagicPen = 0x4750454eu,    // 'GPEN'
};

// The container table uses leaky function-local singletons for the same
// static-initialisation reason as the logging state above: everything is
// created on first use, so a GetDC() from a global constructor is safe, and
// objects a caller leaked stay alive until the process exits (which is what
// Win32 GDI object leaks do too).
std::mutex &Lock() // guards every table below and every live GDI object
{
  static std::mutex *m = new std::mutex();
  return *m;
}

std::set<void *> &Objects() // every live GDI object (handle validity check)
{
  static std::set<void *> *s = new std::set<void *>();
  return *s;
}

std::map<int, HGDIOBJ> &Stock() // GetStockObject() cache
{
  static std::map<int, HGDIOBJ> *m = new std::map<int, HGDIOBJ>();
  return *m;
}

struct GdiBitmap
{
  uint32_t magic = kMagicBitmap;
  int width = 0;
  int height = 0;
  // Top-down BGRA8, stride = width * 4.  Row 0 is the *visual top* row, which
  // is what a y-down GDI DC and a CGBitmapContext agree on (verified against
  // CGBitmapContextCreate's memory layout: memory row 0 == user y == height).
  std::vector<uint8_t> bits;
  bool isDefault = false; // the 1x1 bitmap a fresh DC starts with
  bool pendingDelete = false;
  int selected = 0;
};

struct GdiFont
{
  uint32_t magic = kMagicFont;
  CTFontRef ct = nullptr;
  LOGFONTA lf{};
  bool symbol = false;    // SYMBOL_CHARSET: bytes are glyph indices
  bool isStock = false;
  bool pendingDelete = false;
  int selected = 0;
};

struct GdiBrush
{
  uint32_t magic = kMagicBrush;
  COLORREF color = 0x00FFFFFF;
  bool hollow = false;
  bool isStock = false;
};

struct GdiPen
{
  uint32_t magic = kMagicPen;
  int style = PS_SOLID;
  int width = 1;
  COLORREF color = 0x00000000;
  bool isStock = false;
};

struct DcState
{
  GdiBitmap *bitmap = nullptr;
  GdiFont *font = nullptr;
  GdiBrush *brush = nullptr;
  GdiPen *pen = nullptr;
  COLORREF textColor = 0x00000000;
  COLORREF bkColor = 0x00FFFFFF;
  int bkMode = OPAQUE;
  int charExtra = 0;
};

struct GdiDC
{
  uint32_t magic = kMagicDC;
  GdiBitmap *defBitmap = nullptr; // owned by the DC
  DcState state;
  std::vector<DcState> saved;
  bool isScreen = false;
};

// ---------------------------------------------------------------------------
// table helpers (all require Lock() to be held by the caller)

template <typename T>
T *Lookup(void *h, uint32_t magic)
{
  if (!h) return nullptr;
  if (Objects().find(h) == Objects().end()) return nullptr;
  T *t = static_cast<T *>(h);
  return t->magic == magic ? t : nullptr;
}

template <typename T>
T *Register(T *t)
{
  Objects().insert(t);
  return t;
}

inline GdiBitmap *AsBitmap(HBITMAP h) { return Lookup<GdiBitmap>(h, kMagicBitmap); }
inline GdiFont *AsFont(HFONT h) { return Lookup<GdiFont>(h, kMagicFont); }
inline GdiBrush *AsBrush(HBRUSH h) { return Lookup<GdiBrush>(h, kMagicBrush); }
inline GdiPen *AsPen(HPEN h) { return Lookup<GdiPen>(h, kMagicPen); }
inline GdiDC *AsDC(HDC h) { return Lookup<GdiDC>(h, kMagicDC); }

void DestroyObject(void *h)
{
  if (!h) return;
  GdiDC *dc = Lookup<GdiDC>(h, kMagicDC);
  if (dc)
  {
    Objects().erase(h);
    delete dc;
    return;
  }
  GdiBitmap *bm = Lookup<GdiBitmap>(h, kMagicBitmap);
  if (bm)
  {
    Objects().erase(h);
    delete bm;
    return;
  }
  GdiFont *ft = Lookup<GdiFont>(h, kMagicFont);
  if (ft)
  {
    if (ft->ct) CFRelease(ft->ct);
    Objects().erase(h);
    delete ft;
    return;
  }
  GdiBrush *br = Lookup<GdiBrush>(h, kMagicBrush);
  if (br)
  {
    Objects().erase(h);
    delete br;
    return;
  }
  GdiPen *pn = Lookup<GdiPen>(h, kMagicPen);
  if (pn)
  {
    Objects().erase(h);
    delete pn;
  }
}

// DeleteObject on an object that is currently selected into a DC is deferred
// rather than refused: Win32 fails the delete and leaks the object, and
// Mesh.cpp:1884+2011 selects a font into the shared DC and then deletes it, so
// refusing would leak one font per text mesh.  The object is freed when it is
// deselected or when its DC dies; DeleteObject still reports TRUE.

// ---------------------------------------------------------------------------
// bitmap construction

GdiBitmap *NewBitmap(int w, int h)
{
  if (w <= 0 || h <= 0) return nullptr;
  GdiBitmap *bm = new GdiBitmap();
  bm->width = w;
  bm->height = h;
  bm->bits.assign((size_t)w * (size_t)h * 4u, 0); // Win32 DIBs start black
  return Register(bm);
}

// ---------------------------------------------------------------------------
// font construction

CFStringRef CFStringFromUtf8(const char *s)
{
  if (!s) return nullptr;
  return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

std::string CFStringToUtf8(CFStringRef s)
{
  if (!s) return std::string();
  char buf[256];
  if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return std::string(buf);
  return std::string();
}

// Fonts that look heavy already must not get the bold trait on top: the mesh
// font list's "Arial Black" is a separate family, and Mesh.cpp passes weight 0
// (FW_DONTCARE) for it while Texgen.cpp passes FW_BOLD.
bool FamilyLooksHeavy(const std::string &family)
{
  static const char *kHeavy[] = { "black", "bold", "heavy", "ultra", "semibold", "demibold" };
  std::string lower;
  lower.reserve(family.size());
  for (char c : family) lower.push_back((char)tolower((unsigned char)c));
  for (const char *h : kHeavy)
    if (lower.find(h) != std::string::npos) return true;
  return false;
}

// Returns true if CoreText resolved `requested` to a font whose family name is
// the requested one (case-insensitive).  CoreText never fails: an unknown name
// silently resolves to the default face, which is exactly the substitution
// GDI does too -- but we want to say so out loud once.
bool FamilyMatches(CTFontRef font, const char *requested)
{
  if (!requested || !*requested) return true;
  CFStringRef fam = CTFontCopyFamilyName(font);
  const std::string resolved = CFStringToUtf8(fam);
  if (fam) CFRelease(fam);
  std::string req, got;
  for (const char *c = requested; *c; ++c) req.push_back((char)tolower((unsigned char)*c));
  for (char c : resolved) got.push_back((char)tolower((unsigned char)c));
  if (req == got) return true;
  // "Arial Black" style: the family is what matters, but accept a full name
  // match too ("Arial Black" family vs "Arial-Black" PostScript name).
  return got.find(req) != std::string::npos;
}

// GDI's LOGFONT height rules, implemented as:
//   lfHeight > 0 -> the character cell height (tmAscent + tmDescent) in pixels
//   lfHeight < 0 -> the em height in pixels
//   lfHeight == 0 -> the mapper's default (12 px here)
// CoreText's size *is* the em size, and for a positive height GDI scales a
// TrueType face so that OS/2 usWinAscent + usWinDescent maps to lfHeight, so
// the em size is |lfHeight| * unitsPerEm / (usWinAscent + usWinDescent).  Both
// tables are read straight out of the font file (CTFontCopyTable), because
// CoreText's own ascent/descent come from hhea and differ from the Windows
// metrics for some faces (Helvetica: hhea 1577/-471 vs win 1946/461) -- using
// them would silently change the text size relative to the Windows build.
double EmSizeForCellHeight(CTFontRef font, double cellHeight)
{
  CFDataRef os2 = CTFontCopyTable(font, kCTFontTableOS2, kCTFontTableOptionNoOptions);
  CFDataRef head = CTFontCopyTable(font, kCTFontTableHead, kCTFontTableOptionNoOptions);
  if (os2 && head && CFDataGetLength(os2) >= 78 && CFDataGetLength(head) >= 20)
  {
    const UInt8 *o = CFDataGetBytePtr(os2);
    const UInt8 *h = CFDataGetBytePtr(head);
    const unsigned upm = ((unsigned)h[18] << 8) | (unsigned)h[19];
    const unsigned winAsc = ((unsigned)o[74] << 8) | (unsigned)o[75];
    const unsigned winDesc = ((unsigned)o[76] << 8) | (unsigned)o[77];
    if (upm > 0 && winAsc + winDesc > 0)
    {
      if (os2) CFRelease(os2);
      if (head) CFRelease(head);
      return cellHeight * (double)upm / (double)(winAsc + winDesc);
    }
    LogOnce("font.badmetrics", "CreateFontA: OS/2 usWinAscent/usWinDescent are unusable (upm %u, %u/%u); falling back to CoreText's ascent/descent", upm,
            winAsc, winDesc);
  }
  if (os2) CFRelease(os2);
  if (head) CFRelease(head);

  const double asc = (double)CTFontGetAscent(font);
  const double dsc = (double)CTFontGetDescent(font);
  const double size = (double)CTFontGetSize(font);
  if (asc + dsc > 0.001) return cellHeight * size / (asc + dsc);
  return cellHeight;
}

CTFontRef CreateCTFont(const LOGFONTA &lf, std::string *resolvedFamily)
{
  const char *face = lf.lfFaceName;
  CFStringRef name = CFStringFromUtf8(face && *face ? face : "Arial");
  if (!name) return nullptr;

  double size = 12.0;
  const bool cellHeight = lf.lfHeight > 0;
  if (lf.lfHeight > 0) size = (double)lf.lfHeight;
  else if (lf.lfHeight < 0) size = (double)-lf.lfHeight;

  CTFontRef font = CTFontCreateWithName(name, (CGFloat)size, nullptr);
  CFRelease(name);
  if (!font) return nullptr;

  if (cellHeight)
  {
    const double em = EmSizeForCellHeight(font, (double)lf.lfHeight);
    if (em > 0.01 && std::fabs(em - size) > 0.005)
    {
      CTFontRef scaled = CTFontCreateCopyWithAttributes(font, (CGFloat)em, nullptr, nullptr);
      if (scaled)
      {
        CFRelease(font);
        font = scaled;
      }
    }
  }

  // Traits: bold/italic.  Only ask for bold when the family is not already a
  // heavy face (see FamilyLooksHeavy) -- "Arial Black" would otherwise resolve
  // to a synthesised or wrong face.
  CFStringRef famStr = CTFontCopyFamilyName(font);
  const std::string family = CFStringToUtf8(famStr);
  if (famStr) CFRelease(famStr);

  CTFontSymbolicTraits want = 0;
  if (lf.lfWeight >= FW_SEMIBOLD && !FamilyLooksHeavy(family)) want |= kCTFontTraitBold;
  if (lf.lfItalic) want |= kCTFontTraitItalic;
  if (want)
  {
    CTFontRef withTraits = CTFontCreateCopyWithSymbolicTraits(font, 0.0, nullptr, want, want);
    if (withTraits)
    {
      CFRelease(font);
      font = withTraits;
    }
  }

  if (lf.lfEscapement != 0 || lf.lfOrientation != 0)
    LogOnce("font.rotation", "CreateFontA: lfEscapement/lfOrientation (%ld/%ld) are ignored; the outline is unrotated (the engine passes 0)",
            (long)lf.lfEscapement, (long)lf.lfOrientation);
  if (lf.lfWidth != 0)
    LogOnce("font.width", "CreateFontA: lfWidth (%ld) is ignored; CoreText synthesises no width-matched face (the engine passes 0)", (long)lf.lfWidth);
  if (lf.lfUnderline || lf.lfStrikeOut)
    LogOnce("font.decoration", "CreateFontA: underline/strikeout are not drawn (the engine passes 0)");
  if (lf.lfQuality == NONANTIALIASED_QUALITY)
    LogOnce("font.quality", "CreateFontA: NONANTIALIASED_QUALITY is treated as ANTIALIASED_QUALITY (grayscale AA)");

  if (!FamilyMatches(font, face))
  {
    CFStringRef ps = CTFontCopyPostScriptName(font);
    const std::string psn = CFStringToUtf8(ps);
    if (ps) CFRelease(ps);
    char key[64 + LF_FACESIZE];
    snprintf(key, sizeof key, "font.subst.%s", face && *face ? face : "(null)");
    LogOnce(key, "font '%s' is not installed; CoreText substituted '%s' (%s).  Glyph shapes for this face will differ from the Windows build.",
            face && *face ? face : "(null)", family.c_str(), psn.c_str());
  }

  if (resolvedFamily) *resolvedFamily = family;
  return font;
}

GdiFont *NewFont(const LOGFONTA &lf, bool isStock, bool asSymbol)
{
  GdiFont *f = new GdiFont();
  f->lf = lf;
  f->symbol = asSymbol;
  f->isStock = isStock;
  f->ct = CreateCTFont(lf, nullptr);
  if (!f->ct)
  {
    delete f;
    return nullptr;
  }
  return Register(f);
}

// The mutable default font used when a DC has none selected.  Closest macOS
// analogue of DEFAULT_GUI_FONT (a UI sans at ~13px).
GdiFont *DefaultFont()
{
  GdiFont *f = AsFont(Stock()[DEFAULT_GUI_FONT]);
  if (f) return f;

  LOGFONTA lf{};
  lf.lfHeight = -13;
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  snprintf(lf.lfFaceName, LF_FACESIZE, "Helvetica");
  f = NewFont(lf, true, false);
  if (f) Stock()[DEFAULT_GUI_FONT] = f;
  return f;
}

GdiBrush *StockBrush(int idx)
{
  auto it = Stock().find(idx);
  if (it != Stock().end()) return AsBrush(it->second);
  GdiBrush *b = new GdiBrush();
  b->isStock = true;
  switch (idx)
  {
    case WHITE_BRUSH: b->color = 0x00FFFFFF; break;
    case LTGRAY_BRUSH: b->color = 0x00C0C0C0; break;
    case GRAY_BRUSH: b->color = 0x00808080; break;
    case DKGRAY_BRUSH: b->color = 0x00404040; break;
    case BLACK_BRUSH: b->color = 0x00000000; break;
    case NULL_BRUSH: b->hollow = true; break;
    default: b->color = 0x00FFFFFF; break;
  }
  Register(b);
  Stock()[idx] = b;
  return b;
}

GdiPen *StockPen(int idx)
{
  auto it = Stock().find(idx);
  if (it != Stock().end()) return AsPen(it->second);
  GdiPen *p = new GdiPen();
  p->isStock = true;
  switch (idx)
  {
    case WHITE_PEN: p->color = 0x00FFFFFF; break;
    case BLACK_PEN: p->color = 0x00000000; break;
    case NULL_PEN: p->style = PS_NULL; break;
    default: break;
  }
  Register(p);
  Stock()[idx] = p;
  return p;
}

GdiFont *StockFont(int idx)
{
  auto it = Stock().find(idx);
  if (it != Stock().end()) return AsFont(it->second);

  LOGFONTA lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  switch (idx)
  {
    case OEM_FIXED_FONT:
    case ANSI_FIXED_FONT:
    case SYSTEM_FIXED_FONT:
      lf.lfHeight = -12;
      lf.lfWeight = FW_NORMAL;
      lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
      snprintf(lf.lfFaceName, LF_FACESIZE, "Menlo");
      break;
    default: // SYSTEM_FONT / DEFAULT_GUI_FONT / ANSI_VAR_FONT / DEVICE_DEFAULT_FONT
      lf.lfHeight = -13;
      lf.lfWeight = FW_NORMAL;
      lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
      snprintf(lf.lfFaceName, LF_FACESIZE, "Helvetica");
      break;
  }
  GdiFont *f = NewFont(lf, true, false);
  if (f) Stock()[idx] = f;
  return f;
}

// ---------------------------------------------------------------------------
// DC construction / selection bookkeeping

void DeselectFromDC(GdiDC *dc, void *h)
{
  if (!dc) return; // no DC: there is nothing to deselect from
  GdiBitmap *bm = Lookup<GdiBitmap>(h, kMagicBitmap);
  if (bm)
  {
    if (dc->state.bitmap == bm) dc->state.bitmap = dc->defBitmap;
    if (bm->selected > 0) bm->selected--;
    if (bm->pendingDelete && bm->selected == 0) DestroyObject(bm);
    return;
  }
  GdiFont *ft = Lookup<GdiFont>(h, kMagicFont);
  if (ft)
  {
    if (dc->state.font == ft) dc->state.font = nullptr;
    if (ft->selected > 0) ft->selected--;
    if (ft->pendingDelete && ft->selected == 0) DestroyObject(ft);
    return;
  }
  GdiBrush *br = Lookup<GdiBrush>(h, kMagicBrush);
  if (br)
  {
    if (dc->state.brush == br) dc->state.brush = StockBrush(WHITE_BRUSH);
    return;
  }
  GdiPen *pn = Lookup<GdiPen>(h, kMagicPen);
  if (pn)
  {
    if (dc->state.pen == pn) dc->state.pen = StockPen(BLACK_PEN);
    return;
  }
}

GdiDC *NewDC(bool isScreen)
{
  GdiDC *dc = new GdiDC();
  Register(dc);
  dc->isScreen = isScreen;
  dc->defBitmap = NewBitmap(1, 1);
  if (dc->defBitmap) dc->defBitmap->isDefault = true;
  dc->state.bitmap = dc->defBitmap;
  dc->state.brush = StockBrush(WHITE_BRUSH);
  dc->state.pen = StockPen(BLACK_PEN);
  dc->state.font = nullptr;
  return dc;
}

// The screen DC: singleton, 1x1 backing (nothing in this port draws to the
// screen through GDI; it is only the compatibility reference for
// CreateCompatibleDC/CreateCompatibleBitmap and the DC Mesh.cpp renders glyph
// outlines with).
GdiDC *ScreenDC()
{
  for (void *o : Objects())
  {
    GdiDC *dc = Lookup<GdiDC>(o, kMagicDC);
    if (dc && dc->isScreen) return dc;
  }
  return NewDC(true);
}

// ---------------------------------------------------------------------------
// colours and pixels

inline void UnpackColor(COLORREF c, uint8_t &r, uint8_t &g, uint8_t &b)
{
  r = (uint8_t)(c & 0xFF);
  g = (uint8_t)((c >> 8) & 0xFF);
  b = (uint8_t)((c >> 16) & 0xFF);
}

inline void StorePixel(GdiBitmap *bm, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
  uint8_t *p = bm->bits.data() + ((size_t)y * (size_t)bm->width + (size_t)x) * 4u;
  p[0] = b; // BGRA byte order, matching DXGI_FORMAT_B8G8R8A8_UNORM
  p[1] = g;
  p[2] = r;
  p[3] = a;
}

// A CGContext over the DC's pixels.  No CTM flip is applied: the context's
// native y-up space is converted per call with CgY() below, which keeps glyphs
// upright (a context-wide flip would mirror the text).
CGContextRef CreateDCContext(GdiDC *dc)
{
  GdiBitmap *bm = dc->state.bitmap;
  if (!bm || bm->width <= 0 || bm->height <= 0 || bm->bits.empty()) return nullptr;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  if (!cs) return nullptr;
  CGContextRef ctx = CGBitmapContextCreate(bm->bits.data(), (size_t)bm->width, (size_t)bm->height, 8, (size_t)bm->width * 4u, cs,
                                           kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
  CGColorSpaceRelease(cs);
  if (!ctx) return nullptr;
  CGContextSetShouldAntialias(ctx, true);
  CGContextSetAllowsAntialiasing(ctx, true);
  // Grayscale AA only: subpixel (LCD) smoothing would make the three channels
  // differ, and Texgen.cpp's text filter reads the blue channel as coverage.
  CGContextSetShouldSmoothFonts(ctx, false);
  CGContextSetAllowsFontSmoothing(ctx, false);
  CGContextSetBlendMode(ctx, kCGBlendModeNormal);
  return ctx;
}

// GDI logical y (down, 0 = top) -> CG user y (up, 0 = bottom).
inline double CgY(const GdiBitmap *bm, double gdiY) { return (double)bm->height - gdiY; }

CGColorRef ColorRefToCG(COLORREF c)
{
  uint8_t r, g, b;
  UnpackColor(c, r, g, b);
  return CGColorCreateGenericRGB(r / 255.0, g / 255.0, b / 255.0, 1.0);
}

// FillRect's body.  Requires Lock() (the exported entry point takes it) and is
// also used by TextOutA's OPAQUE background and PatBlt, which already hold it.
void FillRectInternal(GdiDC *dc, const RECT *lprc, GdiBrush *br)
{
  if (!dc || !lprc) return;
  GdiBitmap *bm = dc->state.bitmap;
  if (!bm) return;
  if (!br) br = dc->state.brush; // Win32 uses the DC brush when hbr is NULL
  if (!br || br->hollow) return;

  uint8_t r, g, b;
  UnpackColor(br->color, r, g, b);

  int x0 = lprc->left, x1 = lprc->right, y0 = lprc->top, y1 = lprc->bottom;
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  x0 = MaxT(0, x0);
  y0 = MaxT(0, y0);
  x1 = MinT(bm->width, x1);
  y1 = MinT(bm->height, y1);

  const uint32_t px = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
  for (int y = y0; y < y1; y++)
  {
    uint32_t *p = (uint32_t *)(bm->bits.data() + ((size_t)y * (size_t)bm->width + (size_t)x0) * 4u);
    for (int x = x0; x < x1; x++) *p++ = px;
  }
}

// GetDeviceCaps' body (needs Lock() for the display query only).
int DeviceCapsInternal(GdiDC *dc, int nIndex)
{
  switch (nIndex)
  {
    case LOGPIXELSX:
    case LOGPIXELSY:
      return 96; // logical pixels per inch; what the OLE himetric maths expects
    case BITSPIXEL:
      return 32;
    case PLANES:
      return 1;
    case NUMCOLORS:
      return -1; // "many colours", as on any true-colour device
    case HORZRES:
    case VERTRES:
    {
      GdiBitmap *bm = dc ? dc->state.bitmap : nullptr;
      if (bm && bm->width > 1 && bm->height > 1)
        return (nIndex == HORZRES) ? bm->width : bm->height;
      // Screen DC (its backing is the 1x1 placeholder): report the display.
      const CGDirectDisplayID d = CGMainDisplayID();
      return (nIndex == HORZRES) ? (int)CGDisplayPixelsWide(d) : (int)CGDisplayPixelsHigh(d);
    }
    case HORZSIZE:
    case VERTSIZE:
    {
      const int px = DeviceCapsInternal(dc, nIndex == HORZSIZE ? HORZRES : VERTRES);
      return (int)llround(px * 25.4 / 96.0); // millimetres, as Win32 reports
    }
    case RASTERCAPS:
      return RC_BITBLT | RC_BITMAP64 | RC_DI_BITMAP | RC_STRETCHBLT;
    case NUMBRUSHES:
    case NUMPENS:
    case NUMFONTS:
      return -1;
    case TECHNOLOGY:
      return 1; // DT_RASDISPLAY
    default:
      LogOnce("getdevicecaps.other", "GetDeviceCaps(0x%X) is not implemented; returning 0", nIndex);
      return 0;
  }
}

//////////////////////////////////////////////////////////////////////////////
// Glyph outlines
//
// Win32 GGO_NATIVE output: a sequence of TTPOLYGONHEADER blocks, one per
// contour, y-up with the glyph origin on the baseline at (0,0), values in
// 16.16 fixed point, 4-byte aligned.  A contour is a start point plus curve
// records; TT_PRIM_QSPLINE records encode a run of quadratic B-splines using
// TrueType's implicit-midpoint convention, which Mesh.cpp re-expands with
// exactly that rule (Mesh.cpp:1950-1985).
//
// CoreText already hands back explicit on-curve points
// (CGPathAddQuadCurveToPoint has a real control point and endpoint), so each
// CGPath quadratic becomes one QSPLINE record with cpfx == 2: control, end.
// That decodes to precisely the original quadratic in Mesh.cpp, and keeps cpfx
// well below the engine's `MAX_Q_POINTS` (10) fixed buffer.
//
// Linear segments are coalesced into one TT_PRIM_LINE record; cubics (CFF
// fonts such as a substituted .otf face) are approximated by a chain of
// quadratics -- Win32 has TT_PRIM_CSPLINE for them, but Mesh.cpp has no branch
// for it (Mesh.cpp:1911-1985 handles only LINE and QSPLINE), so emitting
// CSPLINE would silently drop geometry.
//////////////////////////////////////////////////////////////////////////////

struct Pt
{
  double x = 0, y = 0;
};

struct Curve
{
  WORD type = TT_PRIM_LINE;
  std::vector<Pt> pts;
};

struct Contour
{
  Pt start;
  std::vector<Curve> curves;
};

class OutlineBuilder
{
public:
  void MoveTo(const Pt &p)
  {
    contours.push_back(Contour{p, {}});
    cur = (int)contours.size() - 1;
    haveQuad = false;
  }

  void LineTo(const Pt &p)
  {
    Ensure(p);
    Contour &c = contours[(size_t)cur];
    if (!c.curves.empty() && c.curves.back().type == TT_PRIM_LINE) c.curves.back().pts.push_back(p);
    else
    {
      Curve cv;
      cv.type = TT_PRIM_LINE;
      cv.pts.push_back(p);
      c.curves.push_back(std::move(cv));
    }
    quadCtrl = quadEnd = Pt{};
    haveQuad = false;
  }

  // One CGPath quadratic becomes one TT_PRIM_QSPLINE record [control, end]
  // (cpfx == 2), which decodes to exactly that quadratic under the engine's
  // reconstruction rule (Mesh.cpp:1950-1985).
  //
  // Consecutive quadratics whose shared on-curve point *is* the midpoint of
  // their two control points are merged into a single record -- that is the
  // TrueType implied-on-curve case, and it is the layout GDI itself emits
  // (apfx = control..., final endpoint).  The merge is exact under the engine's
  // midpoint rule, and it is only done when the midpoint identity holds to
  // within float noise, so no geometry ever moves.
  void QuadTo(const Pt &ctrl, const Pt &end)
  {
    Ensure(ctrl);
    Contour &c = contours[(size_t)cur];

    if (haveQuad && !c.curves.empty() && c.curves.back().type == TT_PRIM_QSPLINE && c.curves.back().pts.size() < kMaxCpfx)
    {
      const Pt m = Mid(quadCtrl, ctrl);
      const double err = std::hypot(m.x - quadEnd.x, m.y - quadEnd.y);
      if (err <= midEps)
      {
        // [.., quadCtrl, quadEnd] + (ctrl, end) -> [.., quadCtrl, ctrl, end]
        c.curves.back().pts.back() = ctrl;
        c.curves.back().pts.push_back(end);
        quadCtrl = ctrl;
        quadEnd = end;
        return;
      }
    }

    Curve cv;
    cv.type = TT_PRIM_QSPLINE;
    cv.pts.push_back(ctrl);
    cv.pts.push_back(end);
    c.curves.push_back(std::move(cv));
    quadCtrl = ctrl;
    quadEnd = end;
    haveQuad = true;
  }

  void CubicTo(const Pt &p1, const Pt &p2, const Pt &p3, double tol)
  {
    Ensure(p1);
    const Pt p0 = current();
    FlattenCubic(p0, p1, p2, p3, tol, 0);
  }

  void Close()
  {
    cur = -1;
    haveQuad = false;
  }

  // Sets the tolerance used to recognise an implied on-curve midpoint.  The
  // engine's fixed `points[MAX_Q_POINTS]` buffer needs cpfx <= 9; GDI's own
  // records stay far below that.
  void SetMidpointEpsilon(double eps) { midEps = eps; }

  std::vector<Contour> contours;

private:
  static const size_t kMaxCpfx = 9;
  int cur = -1;
  bool haveQuad = false;
  Pt quadCtrl{}, quadEnd{};
  double midEps = 1e-6;

  Pt current() const
  {
    const Contour &c = contours[(size_t)cur];
    if (!c.curves.empty())
    {
      const Curve &cv = c.curves.back();
      if (!cv.pts.empty()) return cv.pts.back();
    }
    return c.start;
  }

  void Ensure(const Pt &p)
  {
    if (cur < 0) MoveTo(p);
  }

  static Pt Mid(const Pt &a, const Pt &b) { return Pt{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5}; }

  // Single-quadratic fit of a cubic (control = (3P1 + 3P2 - P0 - P3)/4) with
  // de Casteljau subdivision until the mid-point error is below `tol`.
  void FlattenCubic(const Pt &p0, const Pt &p1, const Pt &p2, const Pt &p3, double tol, int depth)
  {
    const Pt ctrl{(3 * p1.x + 3 * p2.x - p0.x - p3.x) / 4.0, (3 * p1.y + 3 * p2.y - p0.y - p3.y) / 4.0};
    const Pt quadMid{p0.x / 4 + ctrl.x / 2 + p3.x / 4, p0.y / 4 + ctrl.y / 2 + p3.y / 4};
    const Pt cubicMid{(p0.x + 3 * p1.x + 3 * p2.x + p3.x) / 8.0, (p0.y + 3 * p1.y + 3 * p2.y + p3.y) / 8.0};
    const double err = std::hypot(quadMid.x - cubicMid.x, quadMid.y - cubicMid.y);

    if (err <= tol || depth >= 18)
    {
      QuadTo(ctrl, p3);
      return;
    }

    const Pt a = Mid(p0, p1), b = Mid(p1, p2), c = Mid(p2, p3);
    const Pt d = Mid(a, b), e = Mid(b, c);
    const Pt f = Mid(d, e);
    FlattenCubic(p0, a, d, f, tol, depth + 1);
    FlattenCubic(f, e, c, p3, tol, depth + 1);
  }
};

void SerializeContours(const std::vector<Contour> &contours, const Mat2 &mat, std::vector<uint8_t> &out)
{
  for (const Contour &c : contours)
  {
    if (c.curves.empty()) continue; // degenerate move/close: Win32 emits nothing

    const size_t blockStart = out.size();
    TTPOLYGONHEADER hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.dwType = TT_POLYGON_TYPE;
    double sx, sy;
    ApplyMat(mat, c.start.x, c.start.y, sx, sy);
    hdr.pfxStart.x = FixedFromDouble(sx);
    hdr.pfxStart.y = FixedFromDouble(sy);
    const uint8_t *hp = (const uint8_t *)&hdr;
    out.insert(out.end(), hp, hp + sizeof hdr);

    for (const Curve &cv : c.curves)
    {
      const uint8_t head[4] = { (uint8_t)(cv.type & 0xFF), (uint8_t)((cv.type >> 8) & 0xFF), (uint8_t)(cv.pts.size() & 0xFF),
                                (uint8_t)((cv.pts.size() >> 8) & 0xFF) };
      out.insert(out.end(), head, head + 4);
      for (const Pt &p : cv.pts)
      {
        double tx, ty;
        ApplyMat(mat, p.x, p.y, tx, ty);
        POINTFX pf;
        pf.x = FixedFromDouble(tx);
        pf.y = FixedFromDouble(ty);
        const uint8_t *pp = (const uint8_t *)&pf;
        out.insert(out.end(), pp, pp + sizeof pf);
      }
      // 4 + 8*cpfx is always a multiple of 4, so no alignment padding exists.
    }

    const DWORD cb = (DWORD)(out.size() - blockStart);
    memcpy(&out[blockStart], &cb, sizeof cb);
  }
}

struct WalkState
{
  OutlineBuilder *builder = nullptr;
  double tol = 0.05;
};

void CGPathApplier(void *info, const CGPathElement *el)
{
  WalkState *st = (WalkState *)info;
  if (!st || !el) return;
  switch (el->type)
  {
    case kCGPathElementMoveToPoint:
      st->builder->MoveTo(Pt{el->points[0].x, el->points[0].y});
      break;
    case kCGPathElementAddLineToPoint:
      st->builder->LineTo(Pt{el->points[0].x, el->points[0].y});
      break;
    case kCGPathElementAddQuadCurveToPoint:
      st->builder->QuadTo(Pt{el->points[0].x, el->points[0].y}, Pt{el->points[1].x, el->points[1].y});
      break;
    case kCGPathElementAddCurveToPoint:
      st->builder->CubicTo(Pt{el->points[0].x, el->points[0].y}, Pt{el->points[1].x, el->points[1].y}, Pt{el->points[2].x, el->points[2].y},
                           st->tol);
      break;
    case kCGPathElementCloseSubpath:
      st->builder->Close();
      break;
    default:
      break;
  }
}

// ANSI (cp1252) -> Unicode for the 0x80..0x9F range; everything else is
// identity, exactly like the Windows ANSI code page.
UniChar Cp1252ToUniChar(unsigned char c)
{
  static const UniChar kHigh[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
  };
  if (c >= 0x80 && c <= 0x9F) return kHigh[c - 0x80];
  return (UniChar)c;
}

// Resolve the character code to a font glyph the way the DC's charset implies.
CGGlyph GlyphForChar(GdiFont *font, UINT uChar, bool glyphIndex, bool &mapped)
{
  mapped = true;
  if (glyphIndex || font->symbol)
  {
    // GGO_GLYPH_INDEX is Win32's "the code is already an index", and
    // SYMBOL_CHARSET selects the symbol cmap, where Win32 indexes the font's
    // glyph space with the byte code.  macOS ships no Wingdings/Webdings, so
    // for those two faces this is the closest available behaviour: the byte is
    // used as a glyph index in the substituted face (giving decorative symbol
    // shapes rather than whatever letter that face maps the byte to).  The
    // substitution itself is logged by CreateCTFont.
    const CGGlyph g = (CGGlyph)(uChar & 0xFFFF);
    if (g >= (CGGlyph)CTFontGetGlyphCount(font->ct))
    {
      LogOnce("glyph.symbolrange", "GetGlyphOutline/TextOut: glyph index %u is beyond the %ld glyphs of the selected face; using glyph 0 (.notdef)",
              (unsigned)g, (long)CTFontGetGlyphCount(font->ct));
      mapped = false;
      return 0;
    }
    return g;
  }

  UniChar u = (uChar <= 0xFF) ? Cp1252ToUniChar((unsigned char)uChar) : (UniChar)(uChar & 0xFFFF);
  CGGlyph g = 0;
  if (CTFontGetGlyphsForCharacters(font->ct, &u, &g, 1)) return g;

  // CoreText cannot map it: Win32 would hand back the font's default glyph
  // (glyph 0), which is what we do too, but the caller should know.
  mapped = false;
  return 0;
}

struct GlyphMetricsResult
{
  GLYPHMETRICS gm{};
  std::vector<uint8_t> outline;
  bool blank = false;
};

// Builds the outline + metrics for one character.  `format` selects the output
// encoding; only GGO_NATIVE/GGO_METRICS are produced (see GetGlyphOutlineA).
bool BuildGlyph(GdiFont *font, UINT uChar, BOOL glyphIndex, const MAT2 *lpmat2, GlyphMetricsResult &out)
{
  const Mat2 mat = MatFromMAT2(lpmat2);

  bool mapped = true;
  const CGGlyph glyph = GlyphForChar(font, uChar, glyphIndex != FALSE, mapped);
  const CFIndex glyphCount = CTFontGetGlyphCount(font->ct);
  if (glyph >= glyphCount)
  {
    LogOnce("glyph.indexrange", "GetGlyphOutline: glyph index %u is beyond the %ld glyphs of the selected face; returning an empty outline", (unsigned)glyph,
            (long)glyphCount);
    out.blank = true;
    return true;
  }
  if (!mapped)
    LogOnce("glyph.nomap", "GetGlyphOutline: character 0x%02X has no mapping in the selected face; using glyph 0 (.notdef)", (unsigned)uChar);

  CGPathRef path = CTFontCreatePathForGlyph(font->ct, glyph, nullptr);
  if (path)
  {
    // Cubic flattening tolerance: 0.1% of the em, comparable to the engine's
    // own flattening tolerance (fDeviation = 0.001 em).
    OutlineBuilder builder;
    WalkState st;
    st.builder = &builder;
    const double em = (double)CTFontGetSize(font->ct);
    // Cubic flattening tolerance: 0.1% of the em, comparable to the engine's
    // own flattening tolerance (fDeviation = 0.001 em).  The midpoint epsilon
    // is far tighter: it only absorbs float noise in the exact-midpoint test,
    // while a genuinely non-midpoint off-curve pair differs by at least half a
    // font unit (~0.0001 em at 2048 upm).
    st.tol = MaxT(0.01, em * 0.001);
    builder.SetMidpointEpsilon(MaxT(1e-9, em * 1e-6));
    CGPathApply(path, &st, CGPathApplier);

    SerializeContours(builder.contours, mat, out.outline);
  }

  // --- metrics ------------------------------------------------------------
  // gmptGlyphOrigin is the upper-left corner of the ink box relative to the
  // baseline origin, y-up, so .y is the ink ascent.  The MAT2 rotates/scales
  // the box's corners here, keeping the metrics consistent with the outline.
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  bool hasBox = false;
  if (path)
  {
    const CGRect box = CGPathGetPathBoundingBox(path);
    if (!CGRectIsNull(box) && !CGRectIsEmpty(box))
    {
      const double xs[4] = { box.origin.x, box.origin.x + box.size.width, box.origin.x, box.origin.x + box.size.width };
      const double ys[4] = { box.origin.y, box.origin.y, box.origin.y + box.size.height, box.origin.y + box.size.height };
      for (int i = 0; i < 4; i++)
      {
        double tx, ty;
        ApplyMat(mat, xs[i], ys[i], tx, ty);
        if (!hasBox) { minX = maxX = tx; minY = maxY = ty; hasBox = true; }
        else
        {
          minX = MinT(minX, tx);
          maxX = MaxT(maxX, tx);
          minY = MinT(minY, ty);
          maxY = MaxT(maxY, ty);
        }
      }
    }
  }

  CGSize advance{0, 0};
  CTFontGetAdvancesForGlyphs(font->ct, kCTFontOrientationHorizontal, &glyph, &advance, 1);

  memset(&out.gm, 0, sizeof out.gm);
  if (hasBox)
  {
    out.gm.gmBlackBoxX = (UINT)MaxT(0.0, std::ceil(maxX - minX));
    out.gm.gmBlackBoxY = (UINT)MaxT(0.0, std::ceil(maxY - minY));
    out.gm.gmptGlyphOrigin.x = (SHORT)llround(minX);
    out.gm.gmptGlyphOrigin.y = (SHORT)llround(maxY);
  }
  // GDI reports the hinted, integer advance here, and Mesh.cpp accumulates
  // exactly this value into the string's pen position (Mesh.cpp:1997).
  out.gm.gmCellIncX = (SHORT)llround(advance.width);
  out.gm.gmCellIncY = 0;

  out.blank = out.outline.empty();
  if (path) CFRelease(path);
  return true;
}

//////////////////////////////////////////////////////////////////////////////
// COM plumbing for compat/olectl.h
//////////////////////////////////////////////////////////////////////////////

bool GuidEquals(REFIID a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

class MemStream final : public IStream
{
public:
  MemStream(const BYTE *p, UINT cb)
  {
    if (p && cb) data.assign(p, p + cb);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (GuidEquals(riid, kIID_IStream) || GuidEquals(riid, kIID_IUnknown))
    {
      *ppv = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG n = --refs;
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override
  {
    if (pcbRead) *pcbRead = 0;
    if (!pv && cb) return E_POINTER;
    const ULONGLONG avail = (ULONGLONG)data.size() > pos ? (ULONGLONG)data.size() - pos : 0;
    const ULONG n = (ULONG)MinT<ULONGLONG>(avail, cb);
    if (n) memcpy(pv, data.data() + (size_t)pos, n);
    pos += n;
    if (pcbRead) *pcbRead = n;
    return (n == cb) ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override
  {
    RefuseOnce("stream.write", "IStream::Write on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override
  {
    LONGLONG base = 0;
    if (dwOrigin == STREAM_SEEK_CUR) base = (LONGLONG)pos;
    else if (dwOrigin == STREAM_SEEK_END) base = (LONGLONG)data.size();
    else if (dwOrigin != STREAM_SEEK_SET) return E_INVALIDARG;

    const LONGLONG target = base + dlibMove.QuadPart;
    if (target < 0) return E_INVALIDARG;
    pos = (ULONGLONG)target;
    if (plibNewPosition) plibNewPosition->QuadPart = pos;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
  {
    RefuseOnce("stream.setsize", "IStream::SetSize on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) override
  {
    RefuseOnce("stream.copyto", "IStream::CopyTo on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert(void) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    RefuseOnce("stream.lock", "IStream::LockRegion on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
  {
    RefuseOnce("stream.unlock", "IStream::UnlockRegion on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD) override
  {
    if (!pstatstg) return E_POINTER;
    memset(pstatstg, 0, sizeof *pstatstg);
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = data.size();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream **) override
  {
    RefuseOnce("stream.clone", "IStream::Clone on a SHCreateMemStream stream");
    return E_NOTIMPL;
  }

private:
  ULONG refs = 1;
  std::vector<BYTE> data;
  ULONGLONG pos = 0;
};

class Picture final : public IPicture
{
public:
  explicit Picture(CGImageRef img) : image(img), pxW(CGImageGetWidth(img)), pxH(CGImageGetHeight(img)) {}

  ~Picture()
  {
    if (image) CGImageRelease(image);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (GuidEquals(riid, IID_IPicture) || GuidEquals(riid, kIID_IUnknown))
    {
      *ppv = static_cast<IPicture *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG n = --refs;
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE get_Handle(OLE_HANDLE *) override
  {
    RefuseOnce("picture.handle", "IPicture::get_Handle");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_hPal(OLE_HANDLE *) override
  {
    RefuseOnce("picture.hpal", "IPicture::get_hPal");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_Type(SHORT *ptype) override
  {
    if (!ptype) return E_POINTER;
    *ptype = PICTYPE_BITMAP; // the ImageIO decode always yields a raster image
    return S_OK;
  }

  // OLE sizes are HIMETRIC (0.01 mm); the engine only uses these for the
  // aspect ratio, so the 96 dpi assumption is enough.
  HRESULT STDMETHODCALLTYPE get_Width(OLE_XSIZE_HIMETRIC *pwidth) override
  {
    if (!pwidth) return E_POINTER;
    *pwidth = (OLE_XSIZE_HIMETRIC)llround(pxW * 2540.0 / 96.0);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Height(OLE_YSIZE_HIMETRIC *pheight) override
  {
    if (!pheight) return E_POINTER;
    *pheight = (OLE_YSIZE_HIMETRIC)llround(pxH * 2540.0 / 96.0);
    return S_OK;
  }

  // Source-over composite of the decoded raster image into the destination
  // DC.  The engine calls this twice (black background, then white) and diffs
  // the two renders for anti-aliased alpha, so blending -- not replacing -- is
  // the required semantic.
  HRESULT STDMETHODCALLTYPE Render(HDC hdc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC xSrc, OLE_YPOS_HIMETRIC ySrc,
                                   OLE_XSIZE_HIMETRIC cxSrc, OLE_YSIZE_HIMETRIC cySrc, LPCRECT) override;

  // Pixel size of the decoded image (used by Render).
  double width() const { return pxW; }
  double height() const { return pxH; }

  HRESULT STDMETHODCALLTYPE set_hPal(OLE_HANDLE) override
  {
    RefuseOnce("picture.sethpal", "IPicture::set_hPal");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_CurDC(HDC *) override
  {
    RefuseOnce("picture.curdc", "IPicture::get_CurDC");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE SelectPicture(HDC, HDC *, OLE_HANDLE *) override
  {
    RefuseOnce("picture.select", "IPicture::SelectPicture");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_KeepOriginalFormat(BOOL *pfkeep) override
  {
    if (!pfkeep) return E_POINTER;
    *pfkeep = TRUE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE put_KeepOriginalFormat(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PictureChanged(void) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE SaveAsFile(LPSTREAM, BOOL, LONG *) override
  {
    RefuseOnce("picture.save", "IPicture::SaveAsFile");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE get_Attributes(DWORD *pdwAttr) override
  {
    if (!pdwAttr) return E_POINTER;
    *pdwAttr = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE put_Attributes(DWORD) override { return S_OK; }

private:
  ULONG refs = 1;
  CGImageRef image = nullptr;
  double pxW = 0, pxH = 0;
};

} // namespace

// IID_IPicture is declared `extern const GUID` by compat/olectl.h and is the
// IID the engine passes back in; the identity comparison in QueryInterface is
// against this same object, so the byte value only has to match the SDK's (it
// does) for interop with any other code compiled against the real olectl.h.
extern "C" const GUID IID_IPicture = { 0x7BF80980, 0xBF32, 0x101A, { 0x8B, 0xBB, 0x00, 0xAA, 0x00, 0x30, 0x0C, 0xAB } };

//////////////////////////////////////////////////////////////////////////////
// GDI entry points
//////////////////////////////////////////////////////////////////////////////

#define GDI_GUARD_TRY try {
#define GDI_GUARD_CATCH(ret, what)                       \
  }                                                      \
  catch (...)                                            \
  {                                                      \
    LogOnce("gdi.exception." what, "%s: unhandled C++ exception; returning failure", what); \
    return ret;                                          \
  }

extern "C" {

//////////////////////////////////////////////////////////////////////////////
// Device contexts
//////////////////////////////////////////////////////////////////////////////

HDC WINAPI GetDC(HWND hWnd)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  // Only GetDC(NULL) is reachable (Texgen.cpp:187).  A non-NULL window handle
  // has no meaning in this port; the engine never passes one.
  if (hWnd)
    LogOnce("getdc.hwnd", "GetDC(non-NULL window): the handle is ignored; the same offscreen DC is returned");
  return ScreenDC();
  GDI_GUARD_CATCH(nullptr, "GetDC")
}

HDC WINAPI GetWindowDC(HWND)
{
  RefuseOnce("getwindowdc", "GetWindowDC");
  return nullptr;
}

int WINAPI ReleaseDC(HWND, HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return 0;
  // Only common (screen/window) DCs are released here; the engine's screen DC
  // is a process-lifetime singleton, so this is effectively a validity check.
  return dc->isScreen ? 1 : 0;
  GDI_GUARD_CATCH(0, "ReleaseDC")
}

HDC WINAPI CreateCompatibleDC(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  // Win32 ignores the source DC's *size* here; CreateCompatibleBitmap is what
  // inherits it.  A NULL source means "compatible with the screen", which is
  // the same 32bpp device RGB in this port.
  if (hDC && !AsDC(hDC))
  {
    LogOnce("createdc.bad", "CreateCompatibleDC: invalid source DC; creating an independent 32bpp RGB DC");
  }
  return NewDC(false);
  GDI_GUARD_CATCH(nullptr, "CreateCompatibleDC")
}

BOOL WINAPI DeleteDC(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc || dc->isScreen)
  {
    if (dc && dc->isScreen) LogOnce("deletedc.screen", "DeleteDC on the GetDC() screen DC; ignored (Win32 requires ReleaseDC)");
    return FALSE;
  }

  // Objects deleted while still selected are released now that the DC dies.
  DeselectFromDC(dc, dc->state.bitmap);
  DeselectFromDC(dc, dc->state.font);
  if (dc->defBitmap) DestroyObject(dc->defBitmap);
  DestroyObject(dc);
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "DeleteDC")
}

int WINAPI SaveDC(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return 0;
  dc->saved.push_back(dc->state);
  return (int)dc->saved.size();
  GDI_GUARD_CATCH(0, "SaveDC")
}

BOOL WINAPI RestoreDC(HDC hDC, int nSavedDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return FALSE;
  if (nSavedDC < 0) nSavedDC = (int)dc->saved.size() + 1 + nSavedDC; // negative = relative
  if (nSavedDC <= 0 || (size_t)nSavedDC > dc->saved.size()) return FALSE;
  dc->state = dc->saved[(size_t)nSavedDC - 1];
  dc->saved.resize((size_t)nSavedDC - 1);
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "RestoreDC")
}

int WINAPI GetDeviceCaps(HDC hDC, int nIndex)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  return DeviceCapsInternal(AsDC(hDC), nIndex);
  GDI_GUARD_CATCH(0, "GetDeviceCaps")
}

//////////////////////////////////////////////////////////////////////////////
// Bitmaps
//////////////////////////////////////////////////////////////////////////////

HBITMAP WINAPI CreateCompatibleBitmap(HDC hDC, int cx, int cy)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (hDC && !AsDC(hDC)) LogOnce("createbitmap.baddc", "CreateCompatibleBitmap: invalid DC; creating an independent 32bpp bitmap");
  if (cx <= 0 || cy <= 0) return nullptr;
  // The compatible device in this port is always 32bpp BGRA (see
  // CreateCompatibleDC); CreateDIBSection is available when a caller needs a
  // specific format.
  return NewBitmap(cx, cy);
  GDI_GUARD_CATCH(nullptr, "CreateCompatibleBitmap")
}

HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void *lpBits)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (nWidth <= 0 || nHeight <= 0) return nullptr;
  if (nPlanes != 1)
  {
    LogOnce("createbitmap.planes", "CreateBitmap: %u planes is not supported; only 1 (bitCount %u)", nPlanes, nBitCount);
    return nullptr;
  }
  GdiBitmap *bm = NewBitmap(nWidth, nHeight);
  if (!bm) return nullptr;

  if (lpBits)
  {
    if (nBitCount == 32)
    {
      memcpy(bm->bits.data(), lpBits, bm->bits.size());
    }
    else if (nBitCount == 24)
    {
      const uint8_t *src = (const uint8_t *)lpBits;
      const size_t srcStride = (((size_t)nWidth * 3u + 3u) / 4u) * 4u;
      for (int y = 0; y < nHeight; y++)
        for (int x = 0; x < nWidth; x++)
        {
          const uint8_t *p = src + srcStride * (size_t)y + (size_t)x * 3u;
          StorePixel(bm, x, y, p[2], p[1], p[0], 255);
        }
    }
    else if (nBitCount == 1)
    {
      const uint8_t *src = (const uint8_t *)lpBits;
      const size_t srcStride = (((size_t)nWidth + 31u) / 32u) * 4u;
      for (int y = 0; y < nHeight; y++)
        for (int x = 0; x < nWidth; x++)
        {
          const bool on = (src[srcStride * (size_t)y + (size_t)(x / 8)] >> (7 - (x % 8))) & 1;
          StorePixel(bm, x, y, on ? 255 : 0, on ? 255 : 0, on ? 255 : 0, 255);
        }
    }
    else
    {
      LogOnce("createbitmap.bpp", "CreateBitmap: %u bpp is not supported (32, 24 and 1 are); the bitmap is left black", nBitCount);
    }
  }
  return bm;
  GDI_GUARD_CATCH(nullptr, "CreateBitmap")
}

HBITMAP WINAPI CreateDIBSection(HDC hDC, const BITMAPINFO *lpbmi, UINT, void **ppvBits, HANDLE hSection, DWORD)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (!lpbmi || lpbmi->bmiHeader.biSize < sizeof(BITMAPINFOHEADER) || !ppvBits) return nullptr;
  if (hSection)
  {
    RefuseOnce("dibsection.section", "CreateDIBSection with a file-mapping section");
    return nullptr;
  }
  if (hDC && !AsDC(hDC)) LogOnce("dibsection.baddc", "CreateDIBSection: invalid DC; the DC is not otherwise used");

  const BITMAPINFOHEADER &h = lpbmi->bmiHeader;
  const int w = h.biWidth;
  const int ht = h.biHeight < 0 ? -h.biHeight : h.biHeight;
  if (w <= 0 || ht <= 0) return nullptr;

  if (h.biBitCount != 32)
    LogOnce("dibsection.bpp", "CreateDIBSection: %u bpp requested; the surface is stored as 32bpp BGRA (0x%X)", h.biBitCount, h.biBitCount);

  // NOTE: unlike Win32, the bits returned here are top-down even for a
  // positive biHeight, because that is how this port's DCs store pixels (the
  // engine never calls CreateDIBSection -- Texgen.cpp uses
  // CreateCompatibleBitmap + GetDIBits).
  GdiBitmap *bm = NewBitmap(w, ht);
  if (!bm) return nullptr;
  *ppvBits = bm->bits.data();
  return bm;
  GDI_GUARD_CATCH(nullptr, "CreateDIBSection")
}

int WINAPI GetDIBits(HDC hDC, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (!lpbmi || lpbmi->bmiHeader.biSize < sizeof(BITMAPINFOHEADER)) return 0;

  GdiBitmap *bm = AsBitmap(hbm);
  if (!bm)
  {
    GdiDC *dc = AsDC(hDC);
    if (dc) bm = dc->state.bitmap;
  }
  if (!bm) return 0;

  BITMAPINFOHEADER &h = lpbmi->bmiHeader;
  const UINT bpp = h.biBitCount ? h.biBitCount : 32u;
  const bool topDown = h.biHeight < 0;
  const UINT rows = (UINT)bm->height;

  // With a NULL buffer Win32 fills in the header and reports the scanline
  // count; Texgen.cpp always passes a buffer.
  if (!lpvBits)
  {
    h.biWidth = bm->width;
    h.biHeight = topDown ? -bm->height : bm->height;
    h.biPlanes = 1;
    h.biBitCount = (WORD)bpp;
    h.biCompression = BI_RGB;
    h.biSizeImage = 0;
    h.biClrUsed = 0;
    h.biClrImportant = 0;
    return (int)rows;
  }

  if (h.biWidth != bm->width || (UINT)(topDown ? -h.biHeight : h.biHeight) != rows)
    LogOnce("getdibits.size", "GetDIBits: requested %ldx%ld but the bitmap is %dx%d; %s", (long)h.biWidth, (long)h.biHeight, bm->width, bm->height,
            "the request is clamped to the bitmap");

  const UINT first = start > rows ? rows : start;
  UINT count = cLines == 0 ? rows - first : cLines;
  if (count > rows - first) count = rows - first;

  size_t dstStride = 0;
  switch (bpp)
  {
    case 32: dstStride = (size_t)bm->width * 4u; break;
    case 24: dstStride = (((size_t)bm->width * 3u + 3u) / 4u) * 4u; break;
    case 8: dstStride = (((size_t)bm->width + 3u) / 4u) * 4u; break;
    default:
      RefuseOnce("getdibits.bpp", "GetDIBits with a bit count other than 32, 24 or 8");
      return 0;
  }

  if (h.biWidth != bm->width) h.biWidth = bm->width;
  h.biHeight = topDown ? -(LONG)count : (LONG)count;
  h.biPlanes = 1;
  h.biBitCount = (WORD)bpp;
  h.biCompression = BI_RGB;
  h.biSizeImage = (DWORD)(dstStride * count);

  uint8_t *dst = (uint8_t *)lpvBits;
  for (UINT i = 0; i < count; i++)
  {
    // `start` counts from the DIB's first scanline, which for a
    // positive biHeight is the *bottom* row of the image (Win32 convention).
    const UINT logical = first + i;
    const UINT srcRow = topDown ? logical : (rows - 1u - logical);
    const uint8_t *src = bm->bits.data() + (size_t)srcRow * (size_t)bm->width * 4u;
    uint8_t *row = dst + dstStride * (size_t)i;
    if (bpp == 32)
    {
      memcpy(row, src, (size_t)bm->width * 4u);
    }
    else if (bpp == 24)
    {
      for (int x = 0; x < bm->width; x++)
      {
        row[(size_t)x * 3u + 0] = src[(size_t)x * 4u + 0];
        row[(size_t)x * 3u + 1] = src[(size_t)x * 4u + 1];
        row[(size_t)x * 3u + 2] = src[(size_t)x * 4u + 2];
      }
    }
    else
    {
      for (int x = 0; x < bm->width; x++)
      {
        // Grayscale: Win32's 8bpp DIB is palette based; the port writes the
        // luma so a reader that ignores the palette still sees the image.
        const uint8_t *p = src + (size_t)x * 4u;
        row[x] = (uint8_t)(((unsigned)p[2] * 77u + (unsigned)p[1] * 151u + (unsigned)p[0] * 28u) >> 8);
      }
    }
  }
  return (int)count;
  GDI_GUARD_CATCH(0, "GetDIBits")
}

UINT WINAPI GetObjectType(HGDIOBJ h)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (AsBitmap((HBITMAP)h)) return kObjBitmap;
  if (AsFont((HFONT)h)) return kObjFont;
  if (AsBrush((HBRUSH)h)) return kObjBrush;
  if (AsPen((HPEN)h)) return kObjPen;
  {
    GdiDC *dc = AsDC((HDC)h);
    if (dc) return dc->isScreen ? kObjDC : kObjMemDC;
  }
  return 0;
  GDI_GUARD_CATCH(0, "GetObjectType")
}

int WINAPI GetObjectA(HGDIOBJ h, int c, LPVOID pv)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (!pv) return 0;

  GdiBitmap *bm = AsBitmap((HBITMAP)h);
  if (bm)
  {
    if (c < (int)sizeof(BITMAP)) return 0;
    BITMAP b;
    memset(&b, 0, sizeof b);
    b.bmType = 0;
    b.bmWidth = bm->width;
    b.bmHeight = bm->height;
    b.bmWidthBytes = bm->width * 4;
    b.bmPlanes = 1;
    b.bmBitsPixel = 32;
    b.bmBits = bm->bits.data();
    memcpy(pv, &b, sizeof b);
    return (int)sizeof(BITMAP);
  }

  GdiFont *ft = AsFont((HFONT)h);
  if (ft)
  {
    // Win32 answers a LOGFONT request with the font that was actually selected.
    if (c < (int)sizeof(LOGFONTA)) return 0;
    memcpy(pv, &ft->lf, sizeof(LOGFONTA));
    return (int)sizeof(LOGFONTA);
  }

  LogOnce("getobject.other", "GetObjectA: only BITMAP and LOGFONT requests are implemented; returning 0");
  return 0;
  GDI_GUARD_CATCH(0, "GetObjectA")
}

int WINAPI GetObjectW(HGDIOBJ h, int c, LPVOID pv) { return GetObjectA(h, c, pv); }

//////////////////////////////////////////////////////////////////////////////
// Objects
//////////////////////////////////////////////////////////////////////////////

HGDIOBJ WINAPI SelectObject(HDC hDC, HGDIOBJ h)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return nullptr;

  if (!h)
  {
    // Win32 rejects NULL here.  Texgen.cpp:294 does SelectObject(mdc, fnOld)
    // with fnOld == NULL (no font had been selected before), which Win32
    // ignores -- so we do too, quietly, and the pending DeleteObject(hf)
    // below still has to succeed (see RequestDelete).
    return nullptr;
  }

  GdiBitmap *bm = AsBitmap((HBITMAP)h);
  if (bm)
  {
    GdiBitmap *old = dc->state.bitmap;
    if (bm->selected == 0 && bm->isDefault && bm != dc->defBitmap)
      LogOnce("select.defaultbm", "SelectObject: a DC's default bitmap was selected into another DC");
    dc->state.bitmap = bm;
    bm->selected++;
    if (old && old != bm && old->selected > 0)
    {
      old->selected--;
      if (old->pendingDelete && old->selected == 0) DestroyObject(old);
    }
    return old;
  }

  GdiFont *ft = AsFont((HFONT)h);
  if (ft)
  {
    GdiFont *old = dc->state.font;
    dc->state.font = ft;
    ft->selected++;
    if (old && old != ft)
    {
      if (old->selected > 0) old->selected--;
      if (old->pendingDelete && old->selected == 0) DestroyObject(old);
    }
    return old;
  }

  GdiBrush *br = AsBrush((HBRUSH)h);
  if (br)
  {
    GdiBrush *old = dc->state.brush;
    dc->state.brush = br;
    return old;
  }

  GdiPen *pn = AsPen((HPEN)h);
  if (pn)
  {
    GdiPen *old = dc->state.pen;
    dc->state.pen = pn;
    return old;
  }

  LogOnce("selectobject.bad", "SelectObject: handle is not a live GDI object of a supported type; ignored");
  return nullptr;
  GDI_GUARD_CATCH(nullptr, "SelectObject")
}

BOOL WINAPI DeleteObject(HGDIOBJ h)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (!h) return FALSE;

  GdiBitmap *bm = AsBitmap((HBITMAP)h);
  if (bm)
  {
    if (bm->isDefault)
    {
      LogOnce("delete.defaultbm", "DeleteObject: a DC's built-in default bitmap cannot be deleted (as in Win32)");
      return FALSE;
    }
    if (bm->pendingDelete) return TRUE;
    if (bm->selected > 0) { bm->pendingDelete = true; return TRUE; }
    DestroyObject(bm);
    return TRUE;
  }

  GdiFont *ft = AsFont((HFONT)h);
  if (ft)
  {
    if (ft->isStock)
    {
      LogOnce("delete.stock", "DeleteObject: GetStockObject() handles cannot be deleted (as in Win32)");
      return FALSE;
    }
    if (ft->pendingDelete) return TRUE;
    if (ft->selected > 0) { ft->pendingDelete = true; return TRUE; } // Mesh.cpp:2011 deletes a selected font
    DestroyObject(ft);
    return TRUE;
  }

  GdiBrush *br = AsBrush((HBRUSH)h);
  if (br)
  {
    if (br->isStock)
    {
      LogOnce("delete.stockbrush", "DeleteObject: GetStockObject() brushes cannot be deleted (as in Win32)");
      return FALSE;
    }
    DestroyObject(br);
    return TRUE;
  }

  GdiPen *pn = AsPen((HPEN)h);
  if (pn)
  {
    if (pn->isStock)
    {
      LogOnce("delete.stockpen", "DeleteObject: GetStockObject() pens cannot be deleted (as in Win32)");
      return FALSE;
    }
    DestroyObject(pn);
    return TRUE;
  }

  LogOnce("deleteobject.bad", "DeleteObject: handle is not a live GDI object of a supported type");
  return FALSE;
  GDI_GUARD_CATCH(FALSE, "DeleteObject")
}

HGDIOBJ WINAPI GetStockObject(int i)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  switch (i)
  {
    case WHITE_BRUSH:
    case LTGRAY_BRUSH:
    case GRAY_BRUSH:
    case DKGRAY_BRUSH:
    case BLACK_BRUSH:
    case NULL_BRUSH: return StockBrush(i);
    case WHITE_PEN:
    case BLACK_PEN:
    case NULL_PEN: return StockPen(i);
    case OEM_FIXED_FONT:
    case ANSI_FIXED_FONT:
    case SYSTEM_FIXED_FONT:
    case ANSI_VAR_FONT:
    case SYSTEM_FONT:
    case DEVICE_DEFAULT_FONT:
    case DEFAULT_GUI_FONT: return StockFont(i);
    default:
      LogOnce("getstockobject.other", "GetStockObject(%d) is not implemented; returning NULL", i);
      return nullptr;
  }
  GDI_GUARD_CATCH(nullptr, "GetStockObject")
}

HBRUSH WINAPI CreateSolidBrush(COLORREF color)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiBrush *b = new GdiBrush();
  b->color = color;
  return Register(b);
  GDI_GUARD_CATCH(nullptr, "CreateSolidBrush")
}

HPEN WINAPI CreatePen(int iStyle, int cWidth, COLORREF color)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiPen *p = new GdiPen();
  p->style = iStyle;
  p->width = cWidth;
  p->color = color;
  // Pens are stored, and strokes are drawn by the callers that need them;
  // nothing in the compiled engine draws a line (FillRect/TextOut only).
  return Register(p);
  GDI_GUARD_CATCH(nullptr, "CreatePen")
}

//////////////////////////////////////////////////////////////////////////////
// Drawing
//////////////////////////////////////////////////////////////////////////////

int WINAPI FillRect(HDC hDC, const RECT *lprc, HBRUSH hbr)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return 0;
  GdiBrush *br = AsBrush(hbr);
  FillRectInternal(dc, lprc, br);
  return 1;
  GDI_GUARD_CATCH(0, "FillRect")
}

COLORREF WINAPI SetPixel(HDC hDC, int X, int Y, COLORREF crColor)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  GdiBitmap *bm = dc ? dc->state.bitmap : nullptr;
  if (!bm || X < 0 || Y < 0 || X >= bm->width || Y >= bm->height) return (COLORREF)-1;
  uint8_t r, g, b;
  UnpackColor(crColor, r, g, b);
  StorePixel(bm, X, Y, r, g, b, 255);
  return crColor;
  GDI_GUARD_CATCH((COLORREF)-1, "SetPixel")
}

COLORREF WINAPI GetPixel(HDC hDC, int X, int Y)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  GdiBitmap *bm = dc ? dc->state.bitmap : nullptr;
  if (!bm || X < 0 || Y < 0 || X >= bm->width || Y >= bm->height) return (COLORREF)-1;
  const uint8_t *p = bm->bits.data() + ((size_t)Y * (size_t)bm->width + (size_t)X) * 4u;
  return (COLORREF)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[2]);
  GDI_GUARD_CATCH((COLORREF)-1, "GetPixel")
}

BOOL WINAPI PatBlt(HDC hdcDest, int x, int y, int w, int h, DWORD rop)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hdcDest);
  if (!dc) return FALSE;
  RECT r{x, y, x + w, y + h};
  switch (rop)
  {
    case PATCOPY:
      FillRectInternal(dc, &r, nullptr);
      return TRUE;
    case BLACKNESS:
    {
      GdiBrush b;
      b.color = 0x00000000;
      FillRectInternal(dc, &r, &b);
      return TRUE;
    }
    case WHITENESS:
    {
      GdiBrush b;
      b.color = 0x00FFFFFF;
      FillRectInternal(dc, &r, &b);
      return TRUE;
    }
    default:
      RefuseOnce("patblt.rop", "PatBlt with a raster op other than PATCOPY/BLACKNESS/WHITENESS");
      return FALSE;
  }
  GDI_GUARD_CATCH(FALSE, "PatBlt")
}

BOOL WINAPI BitBlt(HDC hdcDest, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dst = AsDC(hdcDest);
  GdiDC *src = AsDC(hdcSrc);
  if (!dst || !src) return FALSE;
  if (rop != SRCCOPY)
  {
    RefuseOnce("bitblt.rop", "BitBlt with a raster op other than SRCCOPY");
    return FALSE;
  }
  GdiBitmap *d = dst->state.bitmap, *s = src->state.bitmap;
  if (!d || !s || cx <= 0 || cy <= 0) return FALSE;

  // Materialise the source region first: the two DCs may share a bitmap.
  std::vector<uint32_t> tmp((size_t)cx * (size_t)cy, 0u);
  for (int j = 0; j < cy; j++)
  {
    const int sy = y1 + j;
    if (sy < 0 || sy >= s->height) continue;
    for (int i = 0; i < cx; i++)
    {
      const int sx = x1 + i;
      if (sx < 0 || sx >= s->width) continue;
      memcpy(&tmp[(size_t)j * (size_t)cx + (size_t)i], s->bits.data() + ((size_t)sy * (size_t)s->width + (size_t)sx) * 4u, 4);
    }
  }
  for (int j = 0; j < cy; j++)
  {
    const int dy = y + j;
    if (dy < 0 || dy >= d->height) continue;
    for (int i = 0; i < cx; i++)
    {
      const int dx = x + i;
      if (dx < 0 || dx >= d->width) continue;
      memcpy(d->bits.data() + ((size_t)dy * (size_t)d->width + (size_t)dx) * 4u, &tmp[(size_t)j * (size_t)cx + (size_t)i], 4);
    }
  }
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "BitBlt")
}

//////////////////////////////////////////////////////////////////////////////
// Text
//////////////////////////////////////////////////////////////////////////////

BOOL WINAPI TextOutA(HDC hDC, int x, int y, LPCSTR lpString, int c)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc || !lpString) return FALSE;
  if (c <= 0) return TRUE; // Win32 draws nothing for an empty string

  GdiFont *font = dc->state.font ? dc->state.font : DefaultFont();
  if (!font || !font->ct)
  {
    LogOnce("textout.nofont", "TextOut: no usable font is selected and no default font could be created; nothing drawn");
    return FALSE;
  }
  GdiBitmap *bm = dc->state.bitmap;
  if (!bm || !bm->width || !bm->height) return FALSE;

  CGContextRef ctx = CreateDCContext(dc);
  if (!ctx)
  {
    LogOnce("textout.noctx", "TextOut: could not create a CoreGraphics context over the DC's bitmap");
    return FALSE;
  }

  // GDI's default text alignment is TA_LEFT|TA_TOP: (x, y) is the top-left of
  // the character cell, so the baseline sits tmAscent pixels lower.
  const double ascent = (double)CTFontGetAscent(font->ct);
  const double descent = (double)CTFontGetDescent(font->ct);

  // --- glyphs, advances and positions -------------------------------------
  std::vector<CGGlyph> glyphs((size_t)c, 0);
  std::vector<CGSize> advances((size_t)c, CGSize{0, 0});
  for (int i = 0; i < c; i++)
  {
    bool mapped = true;
    glyphs[(size_t)i] = GlyphForChar(font, (UINT)(unsigned char)lpString[i], false, mapped);
    if (!mapped) LogOnce("textout.nomap", "TextOut: some characters have no mapping in the selected face; glyph 0 is used");
  }
  CTFontGetAdvancesForGlyphs(font->ct, kCTFontOrientationHorizontal, glyphs.data(), advances.data(), c);

  double total = 0;
  // GDI inserts the extra space after *every* character cell, the last one
  // included, so both the pen positions below and the reported extent use it
  // for all c characters.
  for (int i = 0; i < c; i++) total += advances[(size_t)i].width + dc->state.charExtra;

  // --- background ---------------------------------------------------------
  if (dc->state.bkMode == OPAQUE)
  {
    RECT r{x, y, x + (int)std::ceil(total), y + (int)std::ceil(ascent + descent)};
    GdiBrush bg;
    bg.color = dc->state.bkColor;
    FillRectInternal(dc, &r, &bg); // Lock() is already held: not the exported FillRect
  }

  // --- draw ---------------------------------------------------------------
  CGColorRef color = ColorRefToCG(dc->state.textColor);
  if (color)
  {
    CGContextSetFillColorWithColor(ctx, color);
    std::vector<CGPoint> positions((size_t)c);
    double pen = (double)x;
    const double baseline = CgY(bm, (double)y + ascent);
    for (int i = 0; i < c; i++)
    {
      positions[(size_t)i] = CGPointMake((CGFloat)pen, (CGFloat)baseline);
      pen += advances[(size_t)i].width + dc->state.charExtra;
    }
    CTFontDrawGlyphs(font->ct, glyphs.data(), positions.data(), (size_t)c, ctx);
    CGColorRelease(color);
  }
  CGContextRelease(ctx);
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "TextOutA")
}

int WINAPI SetBkMode(HDC hDC, int mode)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return 0;
  const int old = dc->state.bkMode;
  dc->state.bkMode = mode;
  return old;
  GDI_GUARD_CATCH(0, "SetBkMode")
}

int WINAPI GetBkMode(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  return dc ? dc->state.bkMode : 0;
  GDI_GUARD_CATCH(0, "GetBkMode")
}

COLORREF WINAPI SetTextColor(HDC hDC, COLORREF color)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return (COLORREF)-1;
  const COLORREF old = dc->state.textColor;
  dc->state.textColor = color;
  return old;
  GDI_GUARD_CATCH((COLORREF)-1, "SetTextColor")
}

COLORREF WINAPI GetTextColor(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  return dc ? dc->state.textColor : (COLORREF)-1;
  GDI_GUARD_CATCH((COLORREF)-1, "GetTextColor")
}

COLORREF WINAPI SetBkColor(HDC hDC, COLORREF color)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return (COLORREF)-1;
  const COLORREF old = dc->state.bkColor;
  dc->state.bkColor = color;
  return old;
  GDI_GUARD_CATCH((COLORREF)-1, "SetBkColor")
}

int WINAPI SetTextCharacterExtra(HDC hDC, int nCharExtra)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc) return 0;
  const int old = dc->state.charExtra;
  dc->state.charExtra = nCharExtra;
  return old;
  GDI_GUARD_CATCH(0, "SetTextCharacterExtra")
}

int WINAPI GetTextCharacterExtra(HDC hDC)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  return dc ? dc->state.charExtra : 0;
  GDI_GUARD_CATCH(0, "GetTextCharacterExtra")
}

BOOL WINAPI GetTextMetricsA(HDC hDC, LPTEXTMETRICA lptm)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc || !lptm) return FALSE;
  GdiFont *font = dc->state.font ? dc->state.font : DefaultFont();
  if (!font || !font->ct) return FALSE;

  memset(lptm, 0, sizeof *lptm);
  const double ascent = (double)CTFontGetAscent(font->ct);
  const double descent = (double)CTFontGetDescent(font->ct);
  const double em = (double)CTFontGetSize(font->ct);
  lptm->tmAscent = (LONG)llround(ascent);
  lptm->tmDescent = (LONG)llround(descent);
  lptm->tmHeight = lptm->tmAscent + lptm->tmDescent;
  lptm->tmInternalLeading = (LONG)MaxT(0.0, (double)lptm->tmHeight - em);
  lptm->tmExternalLeading = (LONG)llround((double)CTFontGetLeading(font->ct));
  lptm->tmAveCharWidth = (LONG)llround((double)CTFontGetXHeight(font->ct) * 0.66);
  lptm->tmMaxCharWidth = lptm->tmAveCharWidth * 3;
  lptm->tmWeight = font->lf.lfWeight;
  lptm->tmOverhang = 0;
  lptm->tmDigitizedAspectX = 96;
  lptm->tmDigitizedAspectY = 96;
  lptm->tmFirstChar = 0x20;
  lptm->tmLastChar = 0xFF;
  lptm->tmDefaultChar = '?';
  lptm->tmBreakChar = ' ';
  lptm->tmItalic = font->lf.lfItalic;
  lptm->tmUnderlined = font->lf.lfUnderline;
  lptm->tmStruckOut = font->lf.lfStrikeOut;
  lptm->tmPitchAndFamily = (BYTE)(TMPF_TRUETYPE | font->lf.lfPitchAndFamily);
  lptm->tmCharSet = font->lf.lfCharSet;
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "GetTextMetricsA")
}

BOOL WINAPI GetTextExtentPoint32A(HDC hDC, LPCSTR lpString, int c, LPSIZE psizl)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hDC);
  if (!dc || !psizl) return FALSE;
  psizl->cx = 0;
  psizl->cy = 0;
  if (c <= 0 || !lpString) return TRUE;

  GdiFont *font = dc->state.font ? dc->state.font : DefaultFont();
  if (!font || !font->ct) return FALSE;

  std::vector<CGGlyph> glyphs((size_t)c, 0);
  std::vector<CGSize> advances((size_t)c, CGSize{0, 0});
  for (int i = 0; i < c; i++)
  {
    bool mapped = true;
    glyphs[(size_t)i] = GlyphForChar(font, (UINT)(unsigned char)lpString[i], false, mapped);
  }
  CTFontGetAdvancesForGlyphs(font->ct, kCTFontOrientationHorizontal, glyphs.data(), advances.data(), c);

  double w = 0;
  for (int i = 0; i < c; i++) w += advances[(size_t)i].width + dc->state.charExtra;
  psizl->cx = (LONG)llround(w);
  psizl->cy = (LONG)llround((double)CTFontGetAscent(font->ct) + (double)CTFontGetDescent(font->ct));
  return TRUE;
  GDI_GUARD_CATCH(FALSE, "GetTextExtentPoint32A")
}

//////////////////////////////////////////////////////////////////////////////
// Fonts
//////////////////////////////////////////////////////////////////////////////

HFONT WINAPI CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut,
                         DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());

  LOGFONTA lf;
  memset(&lf, 0, sizeof lf);
  lf.lfHeight = cHeight;
  lf.lfWidth = cWidth;
  lf.lfEscapement = cEscapement;
  lf.lfOrientation = cOrientation;
  lf.lfWeight = cWeight;
  lf.lfItalic = (BYTE)(bItalic ? 1 : 0);
  lf.lfUnderline = (BYTE)(bUnderline ? 1 : 0);
  lf.lfStrikeOut = (BYTE)(bStrikeOut ? 1 : 0);
  lf.lfCharSet = (BYTE)iCharSet;
  lf.lfOutPrecision = (BYTE)iOutPrecision;
  lf.lfClipPrecision = (BYTE)iClipPrecision;
  lf.lfQuality = (BYTE)iQuality;
  lf.lfPitchAndFamily = (BYTE)iPitchAndFamily;
  if (pszFaceName && *pszFaceName) snprintf(lf.lfFaceName, LF_FACESIZE, "%s", pszFaceName);
  else snprintf(lf.lfFaceName, LF_FACESIZE, "Helvetica");

  // Mesh.cpp:1874 asks for SYMBOL_CHARSET for face names starting with 'W'
  // (Webdings/Wingdings) -- see GlyphForChar for what that means here.
  const bool symbol = (iCharSet == SYMBOL_CHARSET);
  GdiFont *f = NewFont(lf, false, symbol);
  if (!f)
  {
    LogOnce("createfont.fail", "CreateFontA: CoreText could not create a font for '%s' at %dpx; returning NULL", lf.lfFaceName, cHeight);
    return nullptr;
  }
  return f;
  GDI_GUARD_CATCH(nullptr, "CreateFontA")
}

HFONT WINAPI CreateFontIndirectA(const LOGFONTA *lplf)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());
  if (!lplf) return nullptr;
  GdiFont *f = NewFont(*lplf, false, lplf->lfCharSet == SYMBOL_CHARSET);
  if (!f)
  {
    LogOnce("createfontindirect.fail", "CreateFontIndirectA: CoreText could not create a font for '%s'", lplf->lfFaceName);
    return nullptr;
  }
  return f;
  GDI_GUARD_CATCH(nullptr, "CreateFontIndirectA")
}

//////////////////////////////////////////////////////////////////////////////
// Glyph outlines
//////////////////////////////////////////////////////////////////////////////

DWORD WINAPI GetGlyphOutlineA(HDC hDC, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, const MAT2 *lpmat2)
{
  GDI_GUARD_TRY
  std::lock_guard<std::mutex> lock(Lock());

  GdiDC *dc = AsDC(hDC);
  GdiFont *font = dc ? (dc->state.font ? dc->state.font : DefaultFont()) : nullptr;
  if (!font || !font->ct)
  {
    // Return 0, not GDI_ERROR: Mesh.cpp:1904 does `int nSize = GetGlyphOutline(...)`
    // and then `new char[nSize]`, so a failure code would be worse than an
    // empty outline.  The reason is logged.
    LogOnce("glyphoutline.nofont", "GetGlyphOutline: no usable font on the DC (CreateFontA/SelectObject failed?); returning an empty outline");
    return 0;
  }

  const UINT format = fuFormat & ~(UINT)(GGO_GLYPH_INDEX | GGO_UNHINTED);
  if (format != GGO_NATIVE && format != GGO_METRICS)
  {
    // The only call sites (Mesh.cpp:1904,1907) request GGO_NATIVE.  GGO_BITMAP
    // and GGO_GRAY8_BITMAP are refused rather than guessed at: their exact
    // scanline padding/level conventions are not exercised anywhere here, and
    // a wrong bitmap would look like working code.
    RefuseOnce("glyphoutline.format", "GetGlyphOutline with a format other than GGO_NATIVE/GGO_METRICS");
    return kGdiError;
  }

  GlyphMetricsResult res;
  if (!BuildGlyph(font, uChar, (fuFormat & GGO_GLYPH_INDEX) ? TRUE : FALSE, lpmat2, res)) return 0;

  if (lpgm) *lpgm = res.gm;
  if (format == GGO_METRICS) return 0;

  const DWORD needed = (DWORD)res.outline.size();
  if (cjBuffer == 0) return needed; // size query, exactly as Win32
  if (cjBuffer < needed)
  {
    LogOnce("glyphoutline.buffer", "GetGlyphOutline: buffer of %u bytes is smaller than the %u bytes required; returning GDI_ERROR", cjBuffer, needed);
    return kGdiError;
  }
  if (needed && pvBuffer) memcpy(pvBuffer, res.outline.data(), needed);
  return needed;
  GDI_GUARD_CATCH(kGdiError, "GetGlyphOutlineA")
}

//////////////////////////////////////////////////////////////////////////////
// System metrics
//////////////////////////////////////////////////////////////////////////////

int WINAPI GetSystemMetrics(int nIndex)
{
  GDI_GUARD_TRY
  switch (nIndex)
  {
    case SM_CXSCREEN:
    case SM_CYSCREEN:
    {
      const CGDirectDisplayID d = CGMainDisplayID();
      const int w = (int)CGDisplayPixelsWide(d);
      const int h = (int)CGDisplayPixelsHigh(d);
      if (w > 0 && h > 0) return nIndex == SM_CXSCREEN ? w : h;
      LogOnce("sysmetrics.nodisplay", "GetSystemMetrics: the main display reports no pixels; using 1920x1080");
      return nIndex == SM_CXSCREEN ? 1920 : 1080;
    }
    case SM_CXVSCROLL: return 16;
    case SM_CYHSCROLL: return 16;
    case SM_CYCAPTION: return 22;
    case SM_CXBORDER:
    case SM_CYBORDER: return 1;
    case SM_CXFRAME:
    case SM_CYFRAME: return 4;
    case SM_CXICON:
    case SM_CYICON: return 32;
    case SM_CXSMICON:
    case SM_CYSMICON: return 16;
    case SM_CMONITORS:
    {
      uint32_t n = 0;
      if (CGGetActiveDisplayList(0, nullptr, &n) == kCGErrorSuccess && n > 0) return (int)n;
      return 1;
    }
    default:
      LogOnce("sysmetrics.other", "GetSystemMetrics(%d) is not implemented; returning 0 (only SM_CXSCREEN/SM_CYSCREEN are used by the engine)", nIndex);
      return 0;
  }
  GDI_GUARD_CATCH(0, "GetSystemMetrics")
}

//////////////////////////////////////////////////////////////////////////////
// Wide-character entry points
//
// compat/tchar.h makes TCHAR == char and nothing defines UNICODE, so the A/W
// aliases all resolve to the ANSI forms above and these are dead code.  They
// are defined (rather than left undefined) so a future UNICODE build gets a
// diagnostic instead of a link error.
//////////////////////////////////////////////////////////////////////////////

HFONT WINAPI CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR)
{
  RefuseOnce("createfontw", "CreateFontW (the port's engine is an ANSI build: TCHAR == char)");
  return nullptr;
}

DWORD WINAPI GetGlyphOutlineW(HDC, UINT, UINT, LPGLYPHMETRICS, DWORD, LPVOID, const MAT2 *)
{
  RefuseOnce("glyphoutlinew", "GetGlyphOutlineW (the port's engine is an ANSI build: TCHAR == char)");
  return kGdiError;
}

BOOL WINAPI TextOutW(HDC, int, int, LPCWSTR, int)
{
  RefuseOnce("textoutw", "TextOutW (the port's engine is an ANSI build: TCHAR == char)");
  return FALSE;
}

BOOL WINAPI GetTextMetricsW(HDC, LPTEXTMETRICW)
{
  RefuseOnce("textmetricsw", "GetTextMetricsW (the port's engine is an ANSI build: TCHAR == char)");
  return FALSE;
}

BOOL WINAPI GetTextExtentPoint32W(HDC, LPCWSTR, int, LPSIZE)
{
  RefuseOnce("textextentw", "GetTextExtentPoint32W (the port's engine is an ANSI build: TCHAR == char)");
  return FALSE;
}

//////////////////////////////////////////////////////////////////////////////
// OLE pictures (compat/olectl.h)
//
// Reachable: PhoenixConfig.h defines PHX_TEXGEN_IMAGE, so Texgen.cpp's
// PHXTEXTUREFILTER::LoadMetafile is compiled and calls
// SHCreateMemStream + OleLoadPictureEx + IPicture::Render.
//////////////////////////////////////////////////////////////////////////////

IStream *WINAPI SHCreateMemStream(const BYTE *pInit, UINT cbInit)
{
  GDI_GUARD_TRY
  return new MemStream(pInit, cbInit);
  GDI_GUARD_CATCH(nullptr, "SHCreateMemStream")
}

HRESULT WINAPI OleLoadPictureEx(LPSTREAM lpstream, LONG lSize, BOOL, REFIID riid, DWORD, DWORD, DWORD dwFlags, LPVOID *lplpvObj)
{
  GDI_GUARD_TRY
  if (!lplpvObj) return E_POINTER;
  *lplpvObj = nullptr;
  if (!lpstream) return E_INVALIDARG;

  if (!GuidEquals(riid, IID_IPicture))
  {
    LogOnce("oleload.riid", "OleLoadPictureEx: only IID_IPicture is supported; returning E_NOINTERFACE");
    return E_NOINTERFACE;
  }
  if (dwFlags != LP_DEFAULT)
    LogOnce("oleload.flags", "OleLoadPictureEx: dwFlags 0x%X is ignored (LP_DEFAULT is what the engine passes)", dwFlags);

  // Slurp the stream.  lSize is what the caller says it wrote; the stream is
  // authoritative when they disagree.
  std::vector<BYTE> bytes;
  {
    ULARGE_INTEGER end{};
    end.QuadPart = 0;
    if (lSize > 0) bytes.reserve((size_t)lSize);
    BYTE chunk[16384];
    for (;;)
    {
      ULONG got = 0;
      const HRESULT hr = lpstream->Read(chunk, (ULONG)sizeof chunk, &got);
      if (got) bytes.insert(bytes.end(), chunk, chunk + got);
      if (FAILED(hr) || got == 0) break;
    }
  }
  if (bytes.empty())
  {
    LogOnce("oleload.empty", "OleLoadPictureEx: the stream is empty; returning E_FAIL");
    return E_FAIL;
  }

  CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(), (CFIndex)bytes.size());
  if (!data) return E_OUTOFMEMORY;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!source)
  {
    LogOnce("oleload.noimage", "OleLoadPictureEx: ImageIO does not recognise the %zu byte payload", bytes.size());
    return E_FAIL;
  }
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  const size_t typeCount = CGImageSourceGetCount(source);
  CFRelease(source);

  if (!image)
  {
    // WMF/EMF metafiles (what the original LoadMetafile decoded through OLE)
    // are not raster images and ImageIO cannot open them.
    LogOnce("oleload.nodecode",
            "OleLoadPictureEx: ImageIO cannot decode this payload (%zu bytes, %zu image(s)).  Raster formats (PNG/JPEG/GIF/TIFF/BMP) work; WMF/EMF metafiles are not "
            "supported by the CoreGraphics port and this filter will draw nothing.",
            bytes.size(), typeCount);
    return E_FAIL;
  }

  Picture *pic = new Picture(image); // takes the +1 reference from CreateImageAtIndex
  *lplpvObj = static_cast<IPicture *>(pic);
  return S_OK;
  GDI_GUARD_CATCH(E_FAIL, "OleLoadPictureEx")
}

HRESULT WINAPI OleLoadPicture(LPSTREAM lpstream, LONG lSize, BOOL fRunmode, REFIID riid, LPVOID *lplpvObj)
{
  return OleLoadPictureEx(lpstream, lSize, fRunmode, riid, 0, 0, LP_DEFAULT, lplpvObj);
}

} // extern "C"

//////////////////////////////////////////////////////////////////////////////
// IPicture::Render
//
// Out of line because it needs the DC/bitmap helpers above and no locking
// surprises: it takes the same process-wide lock as every other entry point.
//////////////////////////////////////////////////////////////////////////////

namespace
{

HRESULT RenderPicture(Picture &pic, CGImageRef image, HDC hdc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC xSrc, OLE_YPOS_HIMETRIC ySrc,
                      OLE_XSIZE_HIMETRIC cxSrc, OLE_YSIZE_HIMETRIC cySrc)
{
  if (!image) return E_FAIL;
  if (cx <= 0 || cy <= 0) return S_OK; // nothing to draw, not an error

  std::lock_guard<std::mutex> lock(Lock());
  GdiDC *dc = AsDC(hdc);
  if (!dc) return E_INVALIDARG;
  GdiBitmap *bm = dc->state.bitmap;
  if (!bm || !bm->width || !bm->height) return E_FAIL;

  const double pxPerHimetric = 96.0 / 2540.0;

  // OLE source rectangles are HIMETRIC; a negative extent means "the other
  // direction", not "mirror".
  double sx = (double)xSrc * pxPerHimetric;
  double sw = (double)cxSrc * pxPerHimetric;
  double sy = (double)ySrc * pxPerHimetric;
  double sh = (double)cySrc * pxPerHimetric;
  if (sw < 0) { sx += sw; sw = -sw; }
  if (sh < 0) { sy += sh; sh = -sh; }

  const double iw = (double)pic.width();
  const double ih = (double)pic.height();
  if (sx < 0) { sw += sx; sx = 0; }
  if (sy < 0) { sh += sy; sy = 0; }
  if (sx + sw > iw) sw = iw - sx;
  if (sy + sh > ih) sh = ih - sy;
  if (sw <= 0 || sh <= 0) return S_OK;

  CGContextRef ctx = CreateDCContext(dc);
  if (!ctx)
  {
    LogOnce("picture.noctx", "IPicture::Render: could not create a CoreGraphics context over the DC's bitmap");
    return E_FAIL;
  }

  CGImageRef sub = CGImageCreateWithImageInRect(image, CGRectMake((CGFloat)sx, (CGFloat)sy, (CGFloat)sw, (CGFloat)sh));
  if (!sub)
  {
    CGContextRelease(ctx);
    return E_FAIL;
  }

  // GDI destination rect (y down) -> CoreGraphics rect (y up).
  const CGRect dst = CGRectMake((CGFloat)x, (CGFloat)CgY(bm, (double)y + (double)cy), (CGFloat)cx, (CGFloat)cy);
  CGContextSaveGState(ctx);
  CGContextClipToRect(ctx, dst);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, dst, sub);
  CGContextRestoreGState(ctx);
  CGImageRelease(sub);
  CGContextRelease(ctx);
  return S_OK;
}

} // namespace

HRESULT STDMETHODCALLTYPE Picture::Render(HDC hdc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC xSrc, OLE_YPOS_HIMETRIC ySrc,
                                          OLE_XSIZE_HIMETRIC cxSrc, OLE_YSIZE_HIMETRIC cySrc, LPCRECT)
{
  try
  {
    return RenderPicture(*this, image, hdc, x, y, cx, cy, xSrc, ySrc, cxSrc, cySrc);
  }
  catch (...)
  {
    LogOnce("picture.render.exception", "IPicture::Render: unhandled C++ exception");
    return E_FAIL;
  }
}
