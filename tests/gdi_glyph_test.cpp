// tests/gdi_glyph_test.cpp
//
// Validation for src/platform/gdi_posix.cpp, the CoreText/CoreGraphics
// implementation of the Win32 GDI subset the vendored apEx runtime calls.
//
// It exercises the calls in *exactly* the form Phoenix/Mesh.cpp and
// Phoenix/Texgen.cpp use them, because the point is the byte-level contract,
// not a synthetic API tour:
//
//   1. Mesh.cpp:1874-2012 -- GetDC(NULL)/CreateFontA(32,...)/SelectObject/
//      GetGlyphOutline(GGO_NATIVE) on the 23-name engine font list's default
//      face, walking the returned TTPOLYGONHEADER/TTPOLYCURVE/POINTFX stream
//      with Mesh.cpp's own pointer arithmetic (including its
//      `ppp += sizeof(TTPOLYCURVE) + sizeof(POINTFX)*(cpfx-1)` step, which must
//      land exactly on the end of each contour block), and decoding the
//      16.16 fixed-point values with Mesh.cpp's TOFLOAT macro verbatim.
//   2. Texgen.cpp:257-298 -- CreateCompatibleDC/CreateCompatibleBitmap/
//      SelectObject/SetBkMode/SetTextColor/TextOut/GetDIBits: the bitmap must
//      have ink where GDI would put it, in GDI's bottom-up GetDIBits order.
//   3. Texgen.cpp:274 -- SetTextCharacterExtra must widen the rendered string.
//
// Build/run:
//   cmake --build build --target phx_gdi_test && ./build/phx_gdi_test
//
// Exit status is the number of failed checks (0 == pass).

// Apple frameworks first: compat/D3Dcommon.h (pulled in by olectl.h) defines
// `interface` as `struct`, exactly like the real d3dcommon.h, and that collides
// with IOKit's IOHIDTypes.h -- which CoreGraphics.h ends up including.
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include "Windows.h"
#include "wingdi.h"
#include "olectl.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char *fmt, ...)
{
  if (ok) return;
  ++g_failures;
  fputc('\n', stdout);
  fputs("  FAIL: ", stdout);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

// Vertex-for-vertex the macro Mesh.cpp:1899 uses; it relies on FIXED's
// {WORD fract; SHORT value;} layout reading back as a little-endian int32.
#define TOFLOAT(x) ((*(int *)&(x)) / (float)0x10000)

struct Box
{
  bool has = false;
  float minX = 0, maxX = 0, minY = 0, maxY = 0;

  void Add(float x, float y)
  {
    if (!has)
    {
      minX = maxX = x;
      minY = maxY = y;
      has = true;
      return;
    }
    if (x < minX) minX = x;
    if (x > maxX) maxX = x;
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
  }

  float Width() const { return has ? maxX - minX : 0; }
  float Height() const { return has ? maxY - minY : 0; }
};

struct StreamStats
{
  int contours = 0;
  int lineRecords = 0;
  int qsplineRecords = 0;
  int csplineRecords = 0;
  int otherRecords = 0;
  int maxCpfx = 0;
  // Every point, control points included: an upper bound on the ink.
  Box all;
  // The on-curve points only (contour starts, line ends, quadratic ends): a
  // lower bound.  The true outline bounding box is between the two, which is
  // what the GLYPHMETRICS cross-check below uses.
  Box onCurve;
};

// Walks one glyph's GGO_NATIVE buffer with the engine's pointer arithmetic and
// validates every structural invariant Mesh.cpp relies on.
static StreamStats WalkOutline(const char *p, int nSize, const char *what)
{
  StreamStats st;
  const char *end = p + nSize;
  const char *pp = p;

  while (pp < end)
  {
    TTPOLYGONHEADER *hdr = (TTPOLYGONHEADER *)pp;

    Check(end - pp >= (int)sizeof(TTPOLYGONHEADER), "%s: a contour header starts %d bytes before the end of the %d byte buffer", what, (int)(end - pp),
          nSize);
    if (end - pp < (int)sizeof(TTPOLYGONHEADER)) break;

    Check(hdr->dwType == TT_POLYGON_TYPE, "%s: dwType is %u, expected TT_POLYGON_TYPE (%d)", what, (unsigned)hdr->dwType, TT_POLYGON_TYPE);
    Check(hdr->cb >= sizeof(TTPOLYGONHEADER), "%s: cb is %u, smaller than the %zu byte header", what, (unsigned)hdr->cb, sizeof(TTPOLYGONHEADER));
    Check(hdr->cb % 4 == 0, "%s: cb is %u, not a multiple of 4 (Win32 4-byte alignment)", what, (unsigned)hdr->cb);
    Check((size_t)(end - pp) >= (size_t)hdr->cb, "%s: the %u byte contour block runs past the end of the %d byte buffer", what, (unsigned)hdr->cb, nSize);
    if (hdr->cb < sizeof(TTPOLYGONHEADER) || (size_t)(end - pp) < (size_t)hdr->cb) break;

    st.contours++;
    st.all.Add(TOFLOAT(hdr->pfxStart.x), TOFLOAT(hdr->pfxStart.y));
    st.onCurve.Add(TOFLOAT(hdr->pfxStart.x), TOFLOAT(hdr->pfxStart.y));

    // --- Mesh.cpp:1911-1985's inner loop, verbatim --------------------------
    const char *ppp = pp + sizeof(TTPOLYGONHEADER);
    while (ppp - pp < (int)hdr->cb)
    {
      TTPOLYCURVE *curve = (TTPOLYCURVE *)ppp;

      Check((size_t)(pp + hdr->cb - ppp) >= sizeof(TTPOLYCURVE), "%s: a curve header starts %d bytes before the end of its %u byte contour block", what,
            (int)(pp + hdr->cb - ppp), (unsigned)hdr->cb);
      if ((size_t)(pp + hdr->cb - ppp) < sizeof(TTPOLYCURVE)) break;
      Check(curve->cpfx >= 1, "%s: a TT_PRIM_%u curve record has cpfx == 0", what, (unsigned)curve->wType);
      if (curve->cpfx < 1) break;

      // The whole record (4-byte header + cpfx POINTFX) has to stay inside the
      // contour block; Mesh.cpp walks straight into the next record without a
      // bound check.
      const size_t recSize = sizeof(TTPOLYCURVE) + sizeof(POINTFX) * (size_t)(curve->cpfx - 1);
      Check((size_t)(pp + hdr->cb - ppp) >= recSize, "%s: a %u-point TT_PRIM_%u record needs %zu bytes but only %d remain in the block", what,
            (unsigned)curve->cpfx, (unsigned)curve->wType, recSize, (int)(pp + hdr->cb - ppp));
      if ((size_t)(pp + hdr->cb - ppp) < recSize) break;

      if (curve->cpfx > st.maxCpfx) st.maxCpfx = curve->cpfx;
      switch (curve->wType)
      {
        case TT_PRIM_LINE: st.lineRecords++; break;
        case TT_PRIM_QSPLINE: st.qsplineRecords++; break;
        case TT_PRIM_CSPLINE: st.csplineRecords++; break;
        default: st.otherRecords++; break;
      }
      for (int j = 0; j < curve->cpfx; j++) st.all.Add(TOFLOAT(curve->apfx[j].x), TOFLOAT(curve->apfx[j].y));
      // The on-curve end of the record: every point of a LINE record, and only
      // the last point of a QSPLINE/CSPLINE (the rest are control points).
      const POINTFX *last = &curve->apfx[curve->cpfx - 1];
      if (curve->wType == TT_PRIM_LINE)
        for (int j = 0; j < curve->cpfx; j++) st.onCurve.Add(TOFLOAT(curve->apfx[j].x), TOFLOAT(curve->apfx[j].y));
      else
        st.onCurve.Add(TOFLOAT(last->x), TOFLOAT(last->y));

      const char *before = ppp;
      ppp += sizeof(TTPOLYCURVE) + sizeof(POINTFX) * (curve->cpfx - 1);
      Check(ppp > before, "%s: the curve walk did not advance (wType %u, cpfx %u)", what, (unsigned)curve->wType, (unsigned)curve->cpfx);
      if (ppp <= before) break;
    }
    // Mesh.cpp does not check this, but if it fails the engine silently reads
    // the next contour's bytes as curve records.
    Check(ppp - pp == (int)hdr->cb, "%s: the curve walk stopped at offset %d of a %u byte contour block (mis-sized record)", what, (int)(ppp - pp),
          (unsigned)hdr->cb);

    pp += hdr->cb;
  }

  Check(pp == end, "%s: the contour walk consumed %d of %d bytes", what, (int)(pp - p), nSize);
  return st;
}

// ---------------------------------------------------------------------------
// Round-trip check: the returned stream, decoded with Mesh.cpp's own
// TT_PRIM_QSPLINE reconstruction rule, must reproduce the outline CoreText
// hands out for the same glyph (as a shape -- the port is free to choose its
// own em size for a cell height, so both sequences are normalised by their own
// on-curve bounding box).
// ---------------------------------------------------------------------------

struct SeqPt
{
  double x = 0, y = 0;
};

struct SeqContour
{
  std::vector<SeqPt> onCurve;
  std::vector<SeqPt> controls;
};

struct CurveSeq
{
  std::vector<SeqContour> contours;
  bool hasCubic = false;
  int qsplineRecords = 0;
};

static void CoreTextApplier(void *info, const CGPathElement *el)
{
  CurveSeq *s = (CurveSeq *)info;
  if (el->type == kCGPathElementMoveToPoint)
  {
    s->contours.push_back(SeqContour{});
    s->contours.back().onCurve.push_back(SeqPt{ el->points[0].x, el->points[0].y });
    return;
  }
  if (s->contours.empty()) s->contours.push_back(SeqContour{}); // defensive
  SeqContour &c = s->contours.back();
  switch (el->type)
  {
    case kCGPathElementAddLineToPoint:
      c.onCurve.push_back(SeqPt{ el->points[0].x, el->points[0].y });
      break;
    case kCGPathElementAddQuadCurveToPoint:
      c.controls.push_back(SeqPt{ el->points[0].x, el->points[0].y });
      c.onCurve.push_back(SeqPt{ el->points[1].x, el->points[1].y });
      break;
    case kCGPathElementAddCurveToPoint:
      s->hasCubic = true; // flattened by the port, so not comparable element-wise
      c.controls.push_back(SeqPt{ el->points[0].x, el->points[0].y });
      c.controls.push_back(SeqPt{ el->points[1].x, el->points[1].y });
      c.onCurve.push_back(SeqPt{ el->points[2].x, el->points[2].y });
      break;
    default:
      break;
  }
}

static CurveSeq CoreTextOutline(const char *face, unsigned char ch)
{
  CurveSeq s;
  CFStringRef name = CFStringCreateWithCString(nullptr, face, kCFStringEncodingUTF8);
  CTFontRef font = CTFontCreateWithName(name, 100.0, nullptr); // size only scales
  CFRelease(name);
  if (!font) return s;

  UniChar u = (UniChar)ch;
  CGGlyph g = 0;
  if (CTFontGetGlyphsForCharacters(font, &u, &g, 1))
  {
    CGPathRef path = CTFontCreatePathForGlyph(font, g, nullptr);
    if (path)
    {
      CGPathApply(path, &s, CoreTextApplier);
      CFRelease(path);
    }
  }
  CFRelease(font);
  return s;
}

// Mesh.cpp:1909-1985, transcribed: contour start, TT_PRIM_LINE points, and the
// TT_PRIM_QSPLINE midpoint reconstruction into explicit quadratics.
static CurveSeq DecodeStream(const char *p, int nSize)
{
  CurveSeq s;
  for (const char *pp = p; pp - p < nSize;)
  {
    TTPOLYGONHEADER *hdr = (TTPOLYGONHEADER *)pp;
    s.contours.push_back(SeqContour{});
    SeqContour &c = s.contours.back();

    double cx = TOFLOAT(hdr->pfxStart.x), cy = TOFLOAT(hdr->pfxStart.y);
    c.onCurve.push_back(SeqPt{ cx, cy });

    for (const char *ppp = pp + sizeof(TTPOLYGONHEADER); ppp - pp < (int)hdr->cb;)
    {
      TTPOLYCURVE *curve = (TTPOLYCURVE *)ppp;
      if (curve->wType == TT_PRIM_LINE)
      {
        for (int j = 0; j < curve->cpfx; j++)
        {
          cx = TOFLOAT(curve->apfx[j].x);
          cy = TOFLOAT(curve->apfx[j].y);
          c.onCurve.push_back(SeqPt{ cx, cy });
        }
      }
      else if (curve->wType == TT_PRIM_QSPLINE)
      {
        s.qsplineRecords++;
        std::vector<SeqPt> points;
        points.push_back(SeqPt{ cx, cy });
        for (int j = 0; j < curve->cpfx; j++) points.push_back(SeqPt{ TOFLOAT(curve->apfx[j].x), TOFLOAT(curve->apfx[j].y) });

        for (int j = 1; j < curve->cpfx; j++)
        {
          SeqPt end;
          if (j == curve->cpfx - 1) end = points[(size_t)j + 1];
          else
            end = SeqPt{ (points[(size_t)j].x + points[(size_t)j + 1].x) * 0.5, (points[(size_t)j].y + points[(size_t)j + 1].y) * 0.5 };
          c.controls.push_back(points[(size_t)j]);
          c.onCurve.push_back(end);
          cx = end.x;
          cy = end.y;
        }
      }
      ppp += sizeof(TTPOLYCURVE) + sizeof(POINTFX) * (curve->cpfx - 1);
    }
    pp += hdr->cb;
  }
  return s;
}

struct NormMap
{
  double minX = 0, minY = 0, scale = 1;

  SeqPt Apply(const SeqPt &p) const { return SeqPt{ (p.x - minX) / scale, (p.y - minY) / scale }; }
};

static NormMap Normalizer(const CurveSeq &s)
{
  NormMap m;
  bool first = true;
  double maxX = 0, maxY = 0;
  for (const SeqContour &c : s.contours)
    for (const SeqPt &p : c.onCurve)
    {
      if (first)
      {
        m.minX = maxX = p.x;
        m.minY = maxY = p.y;
        first = false;
      }
      if (p.x < m.minX) m.minX = p.x;
      if (p.x > maxX) maxX = p.x;
      if (p.y < m.minY) m.minY = p.y;
      if (p.y > maxY) maxY = p.y;
    }
  const double w = maxX - m.minX, h = maxY - m.minY;
  m.scale = (w > h ? w : h);
  if (m.scale <= 0) m.scale = 1;
  return m;
}

// Both sequences describe the same outline, so after normalising each by its
// own on-curve bounding box every contour, every on-curve point and every
// quadratic control point must coincide.
static void CompareOutline(const char *face, unsigned char ch, const CurveSeq &impl, const char *label)
{
  const CurveSeq ct = CoreTextOutline(face, ch);
  if (ct.contours.empty())
  {
    printf("  %-46s (CoreText has no path; comparison skipped)\n", label);
    return;
  }
  if (ct.hasCubic)
  {
    printf("  %-46s (cubic outlines are flattened; comparison skipped)\n", label);
    return;
  }

  const NormMap a = Normalizer(ct);
  const NormMap b = Normalizer(impl);

  Check(ct.contours.size() == impl.contours.size(), "%s: %zu contours against CoreText's %zu", label, impl.contours.size(), ct.contours.size());
  if (ct.contours.size() != impl.contours.size()) return;

  const double tol = 2e-3; // 16.16 quantisation at ~32px is ~2e-6 after normalising
  for (size_t i = 0; i < ct.contours.size(); i++)
  {
    const SeqContour &x = ct.contours[i];
    const SeqContour &y = impl.contours[i];
    Check(x.onCurve.size() == y.onCurve.size(), "%s: contour %zu has %zu on-curve points, CoreText has %zu", label, i, y.onCurve.size(),
          x.onCurve.size());
    Check(x.controls.size() == y.controls.size(), "%s: contour %zu has %zu quadratic control points, CoreText has %zu", label, i, y.controls.size(),
          x.controls.size());
    const size_t nOn = x.onCurve.size() < y.onCurve.size() ? x.onCurve.size() : y.onCurve.size();
    for (size_t j = 0; j < nOn; j++)
    {
      const SeqPt pa = a.Apply(x.onCurve[j]);
      const SeqPt pb = b.Apply(y.onCurve[j]);
      if (fabs(pa.x - pb.x) > tol || fabs(pa.y - pb.y) > tol)
      {
        Check(false, "%s: contour %zu on-curve point %zu is (%.5f, %.5f) against CoreText's (%.5f, %.5f)", label, i, j, pb.x, pb.y, pa.x, pa.y);
        break;
      }
    }
    const size_t nCtl = x.controls.size() < y.controls.size() ? x.controls.size() : y.controls.size();
    for (size_t j = 0; j < nCtl; j++)
    {
      const SeqPt pa = a.Apply(x.controls[j]);
      const SeqPt pb = b.Apply(y.controls[j]);
      if (fabs(pa.x - pb.x) > tol || fabs(pa.y - pb.y) > tol)
      {
        Check(false, "%s: contour %zu control point %zu is (%.5f, %.5f) against CoreText's (%.5f, %.5f)", label, i, j, pb.x, pb.y, pa.x, pa.y);
        break;
      }
    }
  }
  printf("  %-46s %zu contours, %d qspline record(s)\n", label, ct.contours.size(), impl.qsplineRecords);
}

// The engine's mesh font: index 1 of the 23-name list (apxPhoenix.cpp:19-43)
// is "Arial Black".  macOS ships it in /System/Library/Fonts/Supplemental;
// CoreText substitutes the default face when it is missing, so ask CoreText and
// fall back to Helvetica explicitly (and say which was used).
static std::string EngineDefaultFace(bool &isArialBlack)
{
  CFStringRef want = CFStringCreateWithCString(nullptr, "Arial Black", kCFStringEncodingUTF8);
  CTFontRef probe = CTFontCreateWithName(want, 32.0, nullptr);
  CFRelease(want);

  std::string family;
  if (probe)
  {
    CFStringRef fam = CTFontCopyFamilyName(probe);
    if (fam)
    {
      char buf[256] = { 0 };
      CFStringGetCString(fam, buf, sizeof buf, kCFStringEncodingUTF8);
      family = buf;
      CFRelease(fam);
    }
    CFRelease(probe);
  }
  std::string lower = family;
  for (char &c : lower) c = (char)tolower((unsigned char)c);
  isArialBlack = lower.find("arial black") != std::string::npos;
  return isArialBlack ? "Arial Black" : "Helvetica";
}

struct GlyphRun
{
  int size = 0;
  GLYPHMETRICS gm{};
  StreamStats st;
  std::vector<char> bytes;
};

static GlyphRun LoadGlyph(HDC hdc, unsigned char ch, const char *label)
{
  // Mesh.cpp:1879 -- the fixed MAT2 literal is the identity:
  //   { {0,1}, {0,0}, {0,0}, {0,1} }  =>  eM11 = 1.0, eM22 = 1.0
  static MAT2 m = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
  static MAT2 translated = { { 0, 1 }, { 0, 0 }, { 0x8000, 0 }, { 0, 1 } }; // eM21 = 0.5

  GlyphRun run;
  memset(&run.gm, 0, sizeof run.gm);

  // Exactly Mesh.cpp:1904/1907: query the size, allocate, then fetch.
  const DWORD need = GetGlyphOutline(hdc, ch, GGO_NATIVE, &run.gm, 0, nullptr, &m);
  Check(need != 0xFFFFFFFFu, "%s: the GGO_NATIVE size query returned GDI_ERROR", label);
  Check(need > 0, "%s: the GGO_NATIVE size query returned 0 (no outline)", label);
  Check(need % 4 == 0, "%s: the size query returned %u, not a multiple of 4", label, (unsigned)need);
  if (need == 0 || need == 0xFFFFFFFFu) return run;

  run.size = (int)need;
  run.bytes.assign((size_t)need, 0);
  GLYPHMETRICS gm2;
  memset(&gm2, 0, sizeof gm2);
  const DWORD got = GetGlyphOutline(hdc, ch, GGO_NATIVE, &gm2, need, run.bytes.data(), &m);
  Check(got == need, "%s: the data call returned %u, the size query said %u (the two calls must agree)", label, (unsigned)got, (unsigned)need);
  Check(memcmp(&gm2, &run.gm, sizeof(GLYPHMETRICS)) == 0, "%s: GLYPHMETRICS differs between the size query and the data call", label);

  // A buffer that is one byte too small must fail loudly instead of overrunning.
  if (need > 1)
  {
    std::vector<char> small((size_t)need - 1, 0);
    GLYPHMETRICS gm3;
    const DWORD bad = GetGlyphOutline(hdc, ch, GGO_NATIVE, &gm3, need - 1, small.data(), &m);
    Check(bad == 0xFFFFFFFFu, "%s: an undersized buffer (%u of %u bytes) did not return GDI_ERROR (got %u)", label, (unsigned)need - 1,
          (unsigned)need, (unsigned)bad);
  }

  run.st = WalkOutline(run.bytes.data(), run.size, label);

  // GGO_METRICS must not need a buffer and must agree with the native metrics.
  GLYPHMETRICS gmMetrics;
  memset(&gmMetrics, 0, sizeof gmMetrics);
  GetGlyphOutline(hdc, ch, GGO_METRICS, &gmMetrics, 0, nullptr, &m);
  Check(gmMetrics.gmCellIncX == run.gm.gmCellIncX, "%s: GGO_METRICS advance %d != GGO_NATIVE advance %d", label, (int)gmMetrics.gmCellIncX,
        (int)run.gm.gmCellIncX);

  // The identity MAT2 means eM21 == 0; a translated matrix must actually skew
  // the outline (16.16 x' = x + 0.5*y).
  DWORD skewNeed = GetGlyphOutline(hdc, ch, GGO_NATIVE, &gmMetrics, 0, nullptr, &translated);
  Check(skewNeed == need, "%s: the MAT2 with eM21 = 0.5 changed the outline size (%u vs %u); it should only move points", label, (unsigned)skewNeed,
        (unsigned)need);
  return run;
}

static bool IsInk(unsigned char blue) { return blue > 40; }

struct InkBox
{
  int minRow = -1, maxRow = -1, minCol = -1, maxCol = -1, count = 0;
};

static InkBox FindInk(const std::vector<unsigned char> &buf, int w, int h)
{
  InkBox b;
  for (int r = 0; r < h; r++)
    for (int c = 0; c < w; c++)
      if (IsInk(buf[(size_t)(r * w + c) * 4])) // byte 0 is blue: what Texgen.cpp reads
      {
        b.count++;
        if (b.minRow < 0 || r < b.minRow) b.minRow = r;
        if (b.maxRow < 0 || r > b.maxRow) b.maxRow = r;
        if (b.minCol < 0 || c < b.minCol) b.minCol = c;
        if (b.maxCol < 0 || c > b.maxCol) b.maxCol = c;
      }
  return b;
}

// Renders `text` at (2,2) into a w x h 32bpp compatible bitmap through the DC
// path Texgen.cpp:257-298 uses, and hands back the GetDIBits output.
static InkBox RenderText(HDC screen, const char *face, const char *text, int charExtra, int w, int h, std::vector<unsigned char> &buf,
                         TEXTMETRICA *outTm = nullptr)
{
  HDC mdc = CreateCompatibleDC(screen);
  HBITMAP bm = CreateCompatibleBitmap(screen, w, h);
  Check(mdc != nullptr, "CreateCompatibleDC failed");
  Check(bm != nullptr, "CreateCompatibleBitmap(%d,%d) failed", w, h);

  HGDIOBJ oldBm = SelectObject(mdc, bm);
  HFONT hf = CreateFontA(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         DEFAULT_PITCH, face);
  Check(hf != nullptr, "CreateFontA(24, ..., \"%s\") failed", face);
  HGDIOBJ oldFont = SelectObject(mdc, hf);
  SetBkMode(mdc, TRANSPARENT);
  SetTextColor(mdc, 0x00FFFFFF);
  SetTextCharacterExtra(mdc, charExtra);

  if (outTm)
  {
    memset(outTm, 0, sizeof *outTm);
    Check(GetTextMetricsA(mdc, outTm) != FALSE, "GetTextMetricsA failed");
  }

  const BOOL drew = TextOutA(mdc, 2, 2, text, (int)strlen(text));
  Check(drew != FALSE, "TextOutA(\"%s\") failed", text);

  BITMAPINFO bmi;
  memset(&bmi, 0, sizeof bmi);
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = h; // positive: bottom-up, exactly as Texgen.cpp sets it
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  buf.assign((size_t)w * (size_t)h * 4u, 0);
  const int lines = GetDIBits(mdc, bm, 0, h, buf.data(), &bmi, DIB_RGB_COLORS);
  Check(lines == h, "GetDIBits returned %d scanlines, expected %d", lines, h);
  Check(bmi.bmiHeader.biBitCount == 32, "GetDIBits rewrote biBitCount to %u", (unsigned)bmi.bmiHeader.biBitCount);

  // Texgen.cpp:294-298's teardown, in order.
  SelectObject(mdc, oldFont);
  DeleteObject(hf);
  SelectObject(mdc, oldBm);
  DeleteObject(bm);
  DeleteDC(mdc);

  return FindInk(buf, w, h);
}

int main()
{
  printf("gdi_posix / CoreText glyph outline validation\n");
  printf("=============================================\n");

  bool arialBlack = false;
  const std::string face = EngineDefaultFace(arialBlack);
  printf("engine default face: \"%s\"%s\n", face.c_str(), arialBlack ? "" : " (Arial Black is not installed; fell back like CoreText does)");

  // ---------------------------------------------------------------------
  // Section 1: GetGlyphOutline(GGO_NATIVE) -- the text mesh path.
  // ---------------------------------------------------------------------
  HDC hdc = GetDC(nullptr);
  Check(hdc != nullptr, "GetDC(NULL) returned NULL");
  HFONT hFont = CreateFontA(32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, face.c_str());
  Check(hFont != nullptr, "CreateFontA(32, ..., \"%s\") returned NULL", face.c_str());
  HGDIOBJ old = SelectObject(hdc, hFont);
  (void)old;

  // A face name with no installed font must still produce outlines (CoreText
  // substitutes, exactly as the Windows font mapper does) -- and the port logs
  // the substitution once.
  HFONT missing = CreateFontA(32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Microsoft Sans Serif");
  Check(missing != nullptr, "CreateFontA on an uninstalled face name returned NULL; CoreText should substitute");
  if (missing)
  {
    SelectObject(hdc, missing);
    GLYPHMETRICS gmx;
    const DWORD n = GetGlyphOutline(hdc, 'A', GGO_NATIVE, &gmx, 0, nullptr, nullptr);
    Check(n > 0, "the substituted face produced no outline for 'A'");
    SelectObject(hdc, hFont);
    DeleteObject(missing);
  }

  const unsigned char chars[3] = { 'A', 'g', '.' };
  GlyphRun runs[3];
  for (int i = 0; i < 3; i++)
  {
    char label[64];
    snprintf(label, sizeof label, "'%c'", chars[i]);
    runs[i] = LoadGlyph(hdc, chars[i], label);

    const GlyphRun &r = runs[i];
    printf("\n%c  %s\n", chars[i], label);
    printf("  buffer            : %d bytes (%d contours)\n", r.size, r.st.contours);
    printf("  records           : %d line, %d qspline, %d cspline, %d other (max cpfx %d)\n", r.st.lineRecords, r.st.qsplineRecords,
           r.st.csplineRecords, r.st.otherRecords, r.st.maxCpfx);
    printf("  outline bbox      : x [%.3f .. %.3f]  y [%.3f .. %.3f]  (%.3f x %.3f, 16.16 decoded)\n", r.st.all.minX, r.st.all.maxX, r.st.all.minY,
           r.st.all.maxY, r.st.all.Width(), r.st.all.Height());
    printf("  on-curve bbox     : x [%.3f .. %.3f]  y [%.3f .. %.3f]  (%.3f x %.3f)\n", r.st.onCurve.minX, r.st.onCurve.maxX, r.st.onCurve.minY,
           r.st.onCurve.maxY, r.st.onCurve.Width(), r.st.onCurve.Height());
    printf("  GLYPHMETRICS      : blackBox %ux%u  origin (%d,%d)  cellInc (%d,%d)\n", (unsigned)r.gm.gmBlackBoxX, (unsigned)r.gm.gmBlackBoxY,
           (int)r.gm.gmptGlyphOrigin.x, (int)r.gm.gmptGlyphOrigin.y, (int)r.gm.gmCellIncX, (int)r.gm.gmCellIncY);
    printf("  mesh height (x0.1): %.3f units\n", r.st.all.Height() * 0.1f);

    Check(r.st.contours > 0, "%s: no contours in the outline", label);
    Check(r.st.all.has, "%s: no points in the outline", label);
    Check(r.st.all.Width() > 1.0f, "%s: the outline is degenerate in x (%.3f)", label, r.st.all.Width());
    Check(r.st.all.Height() > 1.0f, "%s: the outline is degenerate in y (%.3f)", label, r.st.all.Height());
    Check(r.gm.gmBlackBoxX > 0 && r.gm.gmBlackBoxY > 0, "%s: GLYPHMETRICS black box is empty", label);
    Check(r.gm.gmCellIncX > 0, "%s: gmCellIncX is %d; Mesh.cpp:1997 advances the pen by it", label, (int)r.gm.gmCellIncX);
    Check(r.st.csplineRecords == 0, "%s: the stream has %d TT_PRIM_CSPLINE records, which Mesh.cpp never handles (geometry would be dropped)", label,
          r.st.csplineRecords);
    Check(r.st.otherRecords == 0, "%s: the stream has %d records of an unknown type", label, r.st.otherRecords);
    Check(r.st.maxCpfx <= 9, "%s: cpfx reached %d; Mesh.cpp's `D3DXVECTOR3 points[MAX_Q_POINTS]` (10) would overflow at 10", label, r.st.maxCpfx);
    Check(r.st.maxCpfx >= 1, "%s: cpfx never reached 1", label);

    // GDI's integer black box must bracket the decoded 16.16 outline: the true
    // curve box is >= the on-curve point box and <= the control-point box.
    Check((double)r.gm.gmBlackBoxX >= (double)r.st.onCurve.Width() - 2.0 && (double)r.gm.gmBlackBoxX <= (double)r.st.all.Width() + 2.0,
          "%s: gmBlackBoxX %u is outside the decoded outline bounds [%.3f .. %.3f]", label, (unsigned)r.gm.gmBlackBoxX, r.st.onCurve.Width(),
          r.st.all.Width());
    Check((double)r.gm.gmBlackBoxY >= (double)r.st.onCurve.Height() - 2.0 && (double)r.gm.gmBlackBoxY <= (double)r.st.all.Height() + 2.0,
          "%s: gmBlackBoxY %u is outside the decoded outline bounds [%.3f .. %.3f]", label, (unsigned)r.gm.gmBlackBoxY, r.st.onCurve.Height(),
          r.st.all.Height());
  }

  // Quadratic B-splines are the whole reason TT_PRIM_QSPLINE exists, and 'g'
  // (a bowl plus a descender bowl) must have them.  'A' and '.' in this face
  // are polygons, so the count is not asserted for them -- what is asserted for
  // every glyph is the round-trip below: decoding the stream with Mesh.cpp's
  // reconstruction rule must reproduce CoreText's outline exactly.
  printf("\nchecks\n");
  Check(runs[1].st.qsplineRecords >= 1, "'g' has %d TT_PRIM_QSPLINE records; a TrueType face must produce at least one", runs[1].st.qsplineRecords);
  Check(runs[1].st.lineRecords >= 1, "'g' has no TT_PRIM_LINE records");
  printf("  %-46s %s\n", "'g' contains TT_PRIM_QSPLINE records", runs[1].st.qsplineRecords >= 1 ? "ok" : "FAILED");
  printf("\nstream -> Mesh.cpp decode vs CoreText's own outline (normalised)\n");
  for (int i = 0; i < 3; i++)
  {
    char label[64];
    snprintf(label, sizeof label, "'%c' decoded outline vs CoreText", chars[i]);
    CompareOutline(face.c_str(), chars[i], DecodeStream(runs[i].bytes.data(), runs[i].size), label);
  }

  // Win32 GGO_NATIVE is y-up with the baseline at y = 0: 'A' sits on or above
  // the baseline and 'g' descends below it.  A y-flip here would silently
  // mirror every text mesh.
  Check(runs[0].st.all.minY > -3.0f, "'A' reaches y = %.3f, far below the baseline (y-up expected)", runs[0].st.all.minY);
  Check(runs[1].st.all.minY < -1.0f, "'g' has no descender (min y = %.3f); the outline looks y-flipped", runs[1].st.all.minY);
  Check(runs[0].st.all.maxY > 10.0f, "'A' only reaches y = %.3f for a 32px font", runs[0].st.all.maxY);
  printf("  %-46s %s\n", "y-up orientation ('A' on the baseline, 'g' below)", (runs[0].st.all.minY > -3.0f && runs[1].st.all.minY < -1.0f) ? "ok" : "FAILED");
  printf("  %-46s %s\n", "'A' is wider than '.'", runs[0].st.all.Width() > runs[2].st.all.Width() ? "ok" : "FAILED");
  Check(runs[0].st.all.Width() > runs[2].st.all.Width(), "'A' (%.3f) is not wider than '.' (%.3f)", runs[0].st.all.Width(), runs[2].st.all.Width());

  // ---------------------------------------------------------------------
  // Section 2: TextOut + GetDIBits -- the text texture path (Texgen.cpp).
  // ---------------------------------------------------------------------
  printf("\nTextOutA + GetDIBits (Texgen.cpp:257-298 path)\n");
  const int W = 64, H = 64;
  std::vector<unsigned char> buf;
  TEXTMETRICA tm;
  const InkBox ink = RenderText(hdc, face.c_str(), "A", 0, W, H, buf, &tm);
  printf("  GetDIBits(64x64, positive biHeight = bottom-up) ink box: rows %d..%d, cols %d..%d, %d pixels\n", ink.minRow, ink.maxRow, ink.minCol,
         ink.maxCol, ink.count);
  printf("  GetTextMetricsA: tmHeight=%ld tmAscent=%ld tmDescent=%ld tmInternalLeading=%ld\n", (long)tm.tmHeight, (long)tm.tmAscent,
         (long)tm.tmDescent, (long)tm.tmInternalLeading);

  Check(ink.count > 0, "TextOut drew nothing (no pixel above the ink threshold)");
  // GDI's DC is y-down and its default text alignment is TA_TOP, so TextOutA's
  // (2,2) is the top-left of the character cell and the baseline sits
  // tmAscent pixels lower.  The buffer is bottom-up (biHeight > 0), so visual
  // row v is buffer row H-1-v: the measured ink must end (its lowest point,
  // the baseline) at visual row 2 + tmAscent.  That single identity pins down
  // the DC's y-down logical space, TextOutA's TA_TOP baseline and GetDIBits'
  // bottom-up row order at once.
  const int visualTop = H - 1 - ink.maxRow;
  const int visualBottom = H - 1 - ink.minRow;
  const int expectedBaseline = 2 + (int)tm.tmAscent;
  printf("  visual rows %d..%d, expected baseline (2 + tmAscent) = %d\n", visualTop, visualBottom, expectedBaseline);
  Check(visualBottom >= expectedBaseline - 3 && visualBottom <= expectedBaseline + 1,
        "ink reaches visual row %d; TextOut(2,2) with tmAscent=%ld should put the baseline at %d", visualBottom, (long)tm.tmAscent, expectedBaseline);
  Check(visualTop >= 2 && visualTop < visualBottom, "ink top is at visual row %d (expected between 2 and %d)", visualTop, visualBottom);
  // A y-flipped image (or a top-down GetDIBits copy) would put the ink in the
  // *lower* half of the 64px bitmap; a 24px cell cannot reach there.
  Check(ink.minRow >= H / 2, "ink starts at buffer row %d: for text at the top of a bottom-up DIB it must be in the upper half (rows >= %d)", ink.minRow, H / 2);
  Check(ink.minCol >= 0 && ink.minCol <= 8, "ink starts at column %d; TextOut(2,2) should start near the left edge", ink.minCol);
  printf("  %-46s %s\n", "text lands in the image's top rows, baseline as TA_TOP", (ink.minRow >= H / 2 && visualBottom >= expectedBaseline - 3 && visualBottom <= expectedBaseline + 1) ? "ok" : "FAILED");

  // The same render, printed top-down (visual row 0 first, i.e. reversed back
  // out of the bottom-up buffer) so the orientation is visible in the output.
  printf("  visual rendering (row 0 = top of the bitmap), 2x2 blocks:\n");
  for (int v = 0; v < H; v += 2)
  {
    const int r = H - 1 - v;
    printf("   ");
    for (int c = 0; c < W; c += 2)
    {
      bool on = false;
      for (int dy = 0; dy < 2 && r - dy >= 0; dy++)
        for (int dx = 0; dx < 2 && c + dx < W; dx++)
          if (IsInk(buf[(size_t)((r - dy) * W + (c + dx)) * 4])) on = true;
      fputc(on ? '#' : '.', stdout);
    }
    fputc('\n', stdout);
  }

  // ---------------------------------------------------------------------
  // Section 3: SetTextCharacterExtra (Texgen.cpp:274).
  // ---------------------------------------------------------------------
  printf("\nSetTextCharacterExtra\n");
  std::vector<unsigned char> tight, loose;
  const InkBox a = RenderText(hdc, face.c_str(), "II", 0, W, H, tight);
  const InkBox b = RenderText(hdc, face.c_str(), "II", 10, W, H, loose);
  printf("  \"II\" ink width: %d px at charExtra 0, %d px at charExtra 10\n", a.maxCol - a.minCol, b.maxCol - b.minCol);
  Check(a.count > 0 && b.count > 0, "one of the charExtra renders drew nothing");
  Check(b.maxCol - b.minCol > a.maxCol - a.minCol + 5, "charExtra 10 only widened \"II\" from %d to %d px", a.maxCol - a.minCol,
        b.maxCol - b.minCol);

  // ---------------------------------------------------------------------
  // Section 4: the surface the engine checks but this port refuses.
  // ---------------------------------------------------------------------
  printf("\nRefusals (expected: one stderr line each, GDI_ERROR returned)\n");
  {
    GLYPHMETRICS gm;
    const DWORD bitmap = GetGlyphOutline(hdc, 'A', GGO_BITMAP, &gm, 0, nullptr, nullptr);
    const DWORD gray8 = GetGlyphOutline(hdc, 'A', GGO_GRAY8_BITMAP, &gm, 0, nullptr, nullptr);
    Check(bitmap == 0xFFFFFFFFu, "GGO_BITMAP returned %u; it is documented as unimplemented and must fail loudly", (unsigned)bitmap);
    Check(gray8 == 0xFFFFFFFFu, "GGO_GRAY8_BITMAP returned %u; it is documented as unimplemented and must fail loudly", (unsigned)gray8);
    printf("  GGO_BITMAP / GGO_GRAY8_BITMAP -> GDI_ERROR (unused by the call sites) %s\n",
           (bitmap == 0xFFFFFFFFu && gray8 == 0xFFFFFFFFu) ? "ok" : "FAILED");
  }

  // ---------------------------------------------------------------------
  // Section 6: the OLE picture path (Texgen.cpp:352-406, PHX_TEXGEN_IMAGE):
  // SHCreateMemStream -> OleLoadPictureEx -> IPicture::get_Width/Height ->
  // Render -> GetDIBits.  The engine's LoadMetafile uses exactly this sequence
  // for embedded images.
  // ---------------------------------------------------------------------
  printf("\nOleLoadPictureEx / IPicture::Render (Texgen.cpp:352-406 path)\n");
  {
    // A 16x8 opaque red PNG, encoded through the same ImageIO the port uses.
    const int PW = 16, PH = 8;
    std::vector<unsigned int> px((size_t)PW * PH, 0xFF0000FFu); // RGBA in memory -> opaque red
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bmp = CGBitmapContextCreate(px.data(), PW, PH, 8, PW * 4, cs, kCGImageAlphaPremultipliedLast);
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    CFMutableDataRef png = CFDataCreateMutable(nullptr, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(png, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    const bool encoded = CGImageDestinationFinalize(dst);
    std::vector<BYTE> bytes;
    if (encoded) bytes.assign(CFDataGetBytePtr(png), CFDataGetBytePtr(png) + CFDataGetLength(png));
    CFRelease(dst);
    CFRelease(png);
    CFRelease(img);
    CFRelease(bmp);
    CFRelease(cs);
    Check(!bytes.empty(), "could not encode a PNG to exercise OleLoadPictureEx");

    LPSTREAM stm = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
    Check(stm != nullptr, "SHCreateMemStream returned NULL");
    LPPICTURE pic = nullptr;
    const HRESULT hr = OleLoadPictureEx(stm, (LONG)bytes.size(), FALSE, IID_IPicture, 300, 300, LP_DEFAULT, (LPVOID *)&pic);
    if (stm) stm->Release();
    Check(hr == S_OK, "OleLoadPictureEx returned 0x%08X", (unsigned)hr);
    Check(pic != nullptr, "OleLoadPictureEx produced no IPicture");

    if (pic)
    {
      OLE_XSIZE_HIMETRIC hw = 0;
      OLE_YSIZE_HIMETRIC hh = 0;
      pic->get_Width(&hw);
      pic->get_Height(&hh);
      printf("  %zu byte PNG -> %dx%d himetric (aspect %.3f; each extent is rounded, so only the ratio is exact)\n", bytes.size(), (int)hw, (int)hh,
             hh ? (double)hw / (double)hh : 0.0);
      Check(hw > 0 && hh > 0 && hw >= 2 * hh - 3 && hw <= 2 * hh + 3, "himetric size %dx%d lost the 2:1 aspect of the 16x8 image", (int)hw, (int)hh);

      void *qi = nullptr;
      Check(pic->QueryInterface(IID_IPicture, &qi) == S_OK && qi == (void *)pic, "QueryInterface(IID_IPicture) failed");
      static const GUID kBogus = { 0xDEADBEEF, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 1 } };
      Check(pic->QueryInterface(kBogus, &qi) == E_NOINTERFACE, "QueryInterface(unknown IID) did not return E_NOINTERFACE");

      // Texgen.cpp:396-398: black background, then Render the metafile, then
      // read the DC back.  (Texgen does this twice, black and white, and diffs
      // the two for alpha; one pass is enough to check the plumbing.)
      HDC mdc = CreateCompatibleDC(hdc);
      HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
      SelectObject(mdc, bm);
      RECT r{0, 0, W, H};
      FillRect(mdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
      const HRESULT rh = pic->Render(mdc, 0, 0, W, H, 0, hh, hw, -hh, &r);
      Check(rh == S_OK, "IPicture::Render returned 0x%08X", (unsigned)rh);

      BITMAPINFO bmi;
      memset(&bmi, 0, sizeof bmi);
      bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
      bmi.bmiHeader.biWidth = W;
      bmi.bmiHeader.biHeight = H;
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      std::vector<unsigned char> out((size_t)W * H * 4u, 0);
      GetDIBits(mdc, bm, 0, H, out.data(), &bmi, DIB_RGB_COLORS);

      // Byte 0 is blue: Texgen.cpp reads exactly that byte as the coverage
      // value and replicates it over RGBA, and DXGI_FORMAT_B8G8R8A8_UNORM wants
      // the same order.
      int red = 0;
      for (int i = 0; i < W * H; i++)
        if (out[(size_t)i * 4 + 2] > 200 && out[(size_t)i * 4 + 1] < 60 && out[(size_t)i * 4 + 0] < 60) red++;
      printf("  Render(0,0,%d,%d) over a black FillRect -> %d of %d pixels red (BGRA byte order preserved)\n", W, H, red, W * H);
      Check(red > W * H / 2, "the picture did not reach the DC (%d red pixels)", red);
      printf("  %-46s %s\n", "OleLoadPictureEx -> Render -> GetDIBits", (hr == S_OK && red > W * H / 2) ? "ok" : "FAILED");

      DeleteObject(bm);
      DeleteDC(mdc);
      pic->Release();
    }
  }

  // ---------------------------------------------------------------------
  // Section 5: GetSystemMetrics (Project.cpp:290, apxProject.cpp:1772).
  // ---------------------------------------------------------------------
  {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    printf("\nGetSystemMetrics: SM_CXSCREEN=%d SM_CYSCREEN=%d (aspect %.3f)\n", sw, sh, sh ? (double)sw / (double)sh : 0.0);
    Check(sw > 0 && sh > 0, "GetSystemMetrics reported an empty display (%dx%d)", sw, sh);
  }

  DeleteObject(hFont);

  printf("\n=============================================\n");
  if (g_failures == 0)
    printf("ALL CHECKS PASSED\n");
  else
    printf("%d CHECK(S) FAILED\n", g_failures);
  return g_failures;
}
