// src/rhi/gs_emulation.cpp
//
// See src/rhi/gs_emulation.h for what this is and why it exists.

#include "gs_emulation.h"

#include <cctype>
#include <cstring>

namespace phxvk
{
namespace
{

bool IsIdentChar(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void SkipSpace(const std::string &s, size_t &p)
{
  while (p < s.size() && isspace((unsigned char)s[p])) p++;
}

// Reads an identifier at `p`, advancing it.
std::string ReadIdent(const std::string &s, size_t &p)
{
  SkipSpace(s, p);
  const size_t begin = p;
  while (p < s.size() && IsIdentChar(s[p])) p++;
  return s.substr(begin, p - begin);
}

// The body of a function: the text between its first '{' and the matching '}'.
// `from` is searched forward for the opening brace.
bool ExtractBody(const std::string &s, size_t from, std::string &body, size_t *bodyStart = nullptr, size_t *bodyEnd = nullptr)
{
  const size_t open = s.find('{', from);
  if (open == std::string::npos) return false;
  int depth = 0;
  for (size_t p = open; p < s.size(); p++)
  {
    if (s[p] == '{') depth++;
    else if (s[p] == '}' && --depth == 0)
    {
      body = s.substr(open + 1, p - open - 1);
      if (bodyStart) *bodyStart = open + 1;
      if (bodyEnd) *bodyEnd = p;
      return true;
    }
  }
  return false;
}

// Finds the parameter list of a function called `name`.
bool FindParameters(const std::string &s, const char *name, std::string &out, size_t *after = nullptr)
{
  const size_t nameLen = strlen(name);
  for (size_t at = s.find(name); at != std::string::npos; at = s.find(name, at + 1))
  {
    if (at > 0 && IsIdentChar(s[at - 1])) continue;
    const size_t a = at + nameLen;
    if (a < s.size() && IsIdentChar(s[a])) continue;
    size_t p = a;
    SkipSpace(s, p);
    if (p >= s.size() || s[p] != '(') continue;
    int depth = 0;
    for (size_t q = p; q < s.size(); q++)
    {
      if (s[q] == '(') depth++;
      else if (s[q] == ')' && --depth == 0)
      {
        out = s.substr(p + 1, q - p - 1);
        if (after) *after = q + 1;
        return true;
      }
    }
    return false;
  }
  return false;
}

// The type of the first parameter, e.g. "VSIN" for `VSIN x`.
std::string FirstParameterType(const std::string &params)
{
  size_t p = 0;
  SkipSpace(params, p);
  // Skip the interpolation/primitive modifiers HLSL allows on stage inputs.
  static const char *kModifiers[] = { "in", "out", "inout", "const", "linear", "centroid", "nointerpolation", "noperspective", "sample" };
  for (;;)
  {
    const size_t save = p;
    std::string word = ReadIdent(params, p);
    bool isModifier = false;
    for (const char *m : kModifiers)
      if (word == m) isModifier = true;
    if (!isModifier) { p = save; break; }
  }
  return ReadIdent(params, p);
}

// Where a RestartStrip sits relative to the Appends, in the order the stream
// is built.  Appends inside a loop whose trip count is a literal are expanded,
// because that is what decides where the strips break -- [PBR Mixed Rendering]
// appends three vertices in a `for` loop and one of Clean Slate's own shaders
// would otherwise be laid out wrong.
struct CallScan
{
  int appends = 0;
  std::vector<int> restartsAfter; // cumulative append count at each RestartStrip
  bool unknown = false;           // a restart whose slot depends on a trip count we cannot read
};

bool ReadIntLiteral(const std::string &s, size_t &p, int &out)
{
  SkipSpace(s, p);
  const size_t begin = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) p++;
  while (p < s.size() && isdigit((unsigned char)s[p])) p++;
  if (p == begin) return false;
  out = atoi(s.substr(begin, p - begin).c_str());
  return true;
}

// Recognises `for ( int x = A; x < B; x++ )` and `for ( int x = A; x >= B; x-- )`
// with literal A and B, which is the only shape the engine writes.
bool LoopTrips(const std::string &text, size_t forAt, int &trips)
{
  const size_t open = text.find('(', forAt);
  if (open == std::string::npos) return false;
  int depth = 0;
  size_t close = std::string::npos;
  for (size_t q = open; q < text.size(); q++)
  {
    if (text[q] == '(') depth++;
    else if (text[q] == ')' && --depth == 0) { close = q; break; }
  }
  if (close == std::string::npos) return false;

  const std::string head = text.substr(open + 1, close - open - 1);
  const size_t semi1 = head.find(';');
  const size_t semi2 = (semi1 == std::string::npos) ? std::string::npos : head.find(';', semi1 + 1);
  if (semi1 == std::string::npos || semi2 == std::string::npos) return false;

  const std::string init = head.substr(0, semi1);
  const std::string cond = head.substr(semi1 + 1, semi2 - semi1 - 1);
  const std::string step = head.substr(semi2 + 1);

  size_t p = init.find('=');
  if (p == std::string::npos) return false;
  int start = 0;
  p++;
  if (!ReadIntLiteral(init, p, start)) return false;

  size_t lt = cond.find('<'), ge = cond.find(">=");
  if (lt == std::string::npos && ge == std::string::npos) return false;
  const bool ascending = (lt != std::string::npos && (ge == std::string::npos || lt < ge));
  size_t q = ascending ? lt + 1 : ge + 2;
  int limit = 0;
  if (!ReadIntLiteral(cond, q, limit)) return false;

  if (step.find("++") == std::string::npos && step.find("--") == std::string::npos) return false;
  if (ascending != (step.find("++") != std::string::npos)) return false;

  trips = ascending ? (limit - start) : (start - limit + 1);
  return trips > 0 && trips <= 64;
}

// The end of the statement that starts at `from`: the ';' that closes it at
// bracket depth zero.  Used for a loop written without braces.
size_t StatementEnd(const std::string &text, size_t from, size_t end)
{
  int depth = 0;
  for (size_t p = from; p < end; p++)
  {
    if (text[p] == '(' || text[p] == '{') depth++;
    else if (text[p] == ')' || text[p] == '}') depth--;
    else if (text[p] == ';' && depth <= 0) return p + 1;
  }
  return end;
}

void ScanRange(const std::string &text, size_t begin, size_t end, CallScan &scan, int depth)
{
  if (depth > 4) { scan.unknown = true; return; }
  for (size_t at = begin; at < end;)
  {
    if (at + 3 <= end && text.compare(at, 3, "for") == 0 && (at == begin || !IsIdentChar(text[at - 1])) &&
        (at + 3 >= text.size() || !IsIdentChar(text[at + 3])))
    {
      int trips = 0;
      if (LoopTrips(text, at, trips))
      {
        // The body is either the brace that follows the closing parenthesis or,
        // for a loop written without braces (`for (...) Append(...);`), the
        // single statement up to its semicolon.  Taking the next brace in the
        // file for the body is how the PBR shader ended up scanning the
        // `if (dat.z > 0.5)` block three times over.
        size_t close = std::string::npos;
        {
          size_t open = text.find('(', at);
          int d = 0;
          for (size_t q = open; q != std::string::npos && q < end; q++)
          {
            if (text[q] == '(') d++;
            else if (text[q] == ')' && --d == 0) { close = q; break; }
          }
        }
        size_t bodyStart = 0, bodyEnd = 0;
        bool haveBody = false;
        if (close != std::string::npos)
        {
          size_t p = close + 1;
          SkipSpace(text, p);
          if (p < end && text[p] == '{')
          {
            std::string ignored;
            haveBody = ExtractBody(text, p, ignored, &bodyStart, &bodyEnd) && bodyEnd <= end;
          }
          else
          {
            bodyStart = p;
            bodyEnd = StatementEnd(text, p, end);
            haveBody = bodyEnd > bodyStart;
          }
        }
        if (haveBody)
        {
          for (int i = 0; i < trips; i++) ScanRange(text, bodyStart, bodyEnd, scan, depth + 1);
          at = bodyEnd + (bodyEnd < end && text[bodyEnd] == '}' ? 1 : 0);
          continue;
        }
      }
      // A loop whose trip count cannot be read: walk its body once so that a
      // RestartStrip inside it is still noticed, and mark the shape unsupported.
      std::string ignored;
      size_t bodyStart = 0, bodyEnd = 0;
      if (ExtractBody(text, at, ignored, &bodyStart, &bodyEnd))
      {
        CallScan inner;
        ScanRange(text, bodyStart, bodyEnd, scan, depth + 1);
        if (!inner.restartsAfter.empty()) scan.unknown = true;
        at = bodyEnd + 1;
        continue;
      }
    }

    if (text.compare(at, 6, "Append") == 0 && (at == 0 || !IsIdentChar(text[at - 1])) && (at + 6 >= text.size() || !IsIdentChar(text[at + 6])))
    {
      size_t p = at + 6;
      SkipSpace(text, p);
      if (p < text.size() && text[p] == '(') scan.appends++;
      at += 6;
      continue;
    }
    if (text.compare(at, 12, "RestartStrip") == 0)
    {
      scan.restartsAfter.push_back(scan.appends);
      at += 12;
      continue;
    }
    at++;
  }
}

CallScan ScanBody(const std::string &body)
{
  CallScan scan;
  ScanRange(body, 0, body.size(), scan, 0);
  return scan;
}

} // namespace

GsProgram AnalyseGeometryShader(const std::string &source, const char *gsEntry, const char *vertexEntry, const GeometryShaderInfo &shape)
{
  GsProgram gs;
  if (!shape.present) { gs.reason = "no [maxvertexcount]; not a geometry shader"; return gs; }
  if (shape.maxVertexCount <= 0 || shape.maxVertexCount > 64) { gs.reason = "maxvertexcount out of range"; return gs; }
  if (shape.outputStream != "TriangleStream" && shape.outputStream != "LineStream" && shape.outputStream != "PointStream")
  {
    gs.reason = "output stream '" + shape.outputStream + "' is not one the emulator can draw";
    return gs;
  }
  gs.outputStream = shape.outputStream;
  // A PointStream emits independent points: there are no strips to separate, so
  // no restart index is generated and primitive restart stays off.
  gs.strips = (shape.outputStream != "PointStream");

  gs.inputVertices = shape.inputVertices;
  if (gs.inputVertices != 1 && gs.inputVertices != 2 && gs.inputVertices != 3 && gs.inputVertices != 4 && gs.inputVertices != 6)
  {
    gs.reason = "input primitive is not point/line/triangle (with or without adjacency)";
    return gs;
  }

  std::string gsParams;
  size_t gsAfter = 0;
  if (!FindParameters(source, gsEntry, gsParams, &gsAfter)) { gs.reason = "the geometry entry point has no parameter list"; return gs; }

  std::string gsBody;
  if (!ExtractBody(source, gsAfter, gsBody)) { gs.reason = "the geometry entry point has no body"; return gs; }

  const CallScan scan = ScanBody(gsBody);
  if (scan.unknown) { gs.reason = "a RestartStrip sits in a loop whose trip count is not a literal"; return gs; }
  if (scan.appends == 0) { gs.reason = "the geometry shader appends nothing"; return gs; }
  if (scan.appends > shape.maxVertexCount) { gs.reason = "more Append calls than maxvertexcount allows"; return gs; }

  // The output element type must expose a Position member: that is what the
  // emulation has to overwrite to collapse unused slots into nothing.
  size_t p = gsParams.find("inout");
  std::string outType;
  if (p != std::string::npos)
  {
    const size_t lt = gsParams.find('<', p);
    const size_t gt = (lt == std::string::npos) ? std::string::npos : gsParams.find('>', lt);
    if (lt != std::string::npos && gt != std::string::npos) outType = gsParams.substr(lt + 1, gt - lt - 1);
  }
  if (outType.empty()) { gs.reason = "no output element type"; return gs; }

  const size_t structAt = source.find("struct " + outType);
  std::string structBody;
  if (structAt == std::string::npos || !ExtractBody(source, structAt, structBody) || structBody.find("Position") == std::string::npos)
  {
    gs.reason = outType + " has no Position member";
    return gs;
  }

  // The vertex shader the geometry shader feeds from.
  std::string vsParams;
  size_t vsAfter = 0;
  if (!FindParameters(source, vertexEntry, vsParams, &vsAfter)) { gs.reason = "the vertex entry point has no parameter list"; return gs; }
  if (FirstParameterType(vsParams).empty()) { gs.reason = "cannot read the vertex input type"; return gs; }

  gs.slots = shape.maxVertexCount;
  gs.restartsAfter = scan.restartsAfter;
  gs.ok = true;
  return gs;
}

std::string TranslateGeometryShaderToVertex(const std::string &source, const GsProgram &gs, const char *gsEntry, const char *vertexEntry, const char *packedInputName)
{
  if (!gs.ok) return std::string();

  std::string gsParams;
  size_t gsAfter = 0;
  if (!FindParameters(source, gsEntry, gsParams, &gsAfter)) return std::string();
  std::string gsBody;
  if (!ExtractBody(source, gsAfter, gsBody)) return std::string();

  std::string vsParams;
  if (!FindParameters(source, vertexEntry, vsParams)) return std::string();
  const std::string vsInputType = FirstParameterType(vsParams);
  if (vsInputType.empty()) return std::string();

  // The type the geometry shader reads from the vertex shader.
  size_t p = gsParams.find("inout");
  const std::string inPart = (p == std::string::npos) ? gsParams : gsParams.substr(0, p);
  size_t q = 0;
  const std::string primitiveWord = ReadIdent(inPart, q);
  const std::string stageType = ReadIdent(inPart, q);
  if (stageType.empty()) return std::string();

  std::string outType;
  if (p != std::string::npos)
  {
    const size_t lt = gsParams.find('<', p);
    const size_t gt = (lt == std::string::npos) ? std::string::npos : gsParams.find('>', lt);
    if (lt != std::string::npos && gt != std::string::npos) outType = gsParams.substr(lt + 1, gt - lt - 1);
  }
  if (outType.empty()) return std::string();

  // Rewrite the body: every Append becomes "is this my slot?", every
  // RestartStrip disappears (the index buffer carries the restarts).
  // A call is written `OutputStream.Append(...)`, so replacing only the name
  // would leave a dangling `OutputStream.` behind -- consume the receiver too.
  auto dropReceiver = [](std::string &out, const std::string &body, size_t at) {
    size_t q = at;
    while (q > 0 && (body[q - 1] == ' ' || body[q - 1] == '\t')) q--;
    if (q == 0 || body[q - 1] != '.') return;
    size_t r = q - 1;
    while (r > 0 && IsIdentChar(body[r - 1])) r--;
    const size_t prefix = at - r;
    if (out.size() >= prefix) out.resize(out.size() - prefix);
  };

  std::string translated;
  for (size_t at = 0; at < gsBody.size();)
  {
    if (gsBody.compare(at, 6, "Append") == 0 && (at == 0 || !IsIdentChar(gsBody[at - 1])) && (at + 6 >= gsBody.size() || !IsIdentChar(gsBody[at + 6])))
    {
      size_t a = at + 6;
      SkipSpace(gsBody, a);
      if (a < gsBody.size() && gsBody[a] == '(')
      {
        int depth = 0;
        size_t end = a;
        for (; end < gsBody.size(); end++)
        {
          if (gsBody[end] == '(') depth++;
          else if (gsBody[end] == ')' && --depth == 0) break;
        }
        if (end < gsBody.size())
        {
          const std::string arg = gsBody.substr(a + 1, end - a - 1);
          dropReceiver(translated, gsBody, at);
          translated += "{ if ( gsemu_slot == gsemu_at ) return " + arg + "; ++gsemu_at; }";
          at = end + 1;
          continue;
        }
      }
    }
    if (gsBody.compare(at, 12, "RestartStrip") == 0)
    {
      size_t a = at + 12;
      SkipSpace(gsBody, a);
      if (a < gsBody.size() && gsBody[a] == '(')
      {
        size_t end = gsBody.find(')', a);
        if (end != std::string::npos)
        {
          dropReceiver(translated, gsBody, at);
          translated += ";";
          at = end + 1;
          continue;
        }
      }
    }
    translated += gsBody[at++];
  }

  std::string out;
  out += "\n// ---- geometry shader emulation (src/rhi/gs_emulation.cpp) ----\n";
  out += "struct " + std::string(packedInputName) + " { " + vsInputType + " a0;";
  for (int i = 1; i < gs.inputVertices; i++) out += " " + vsInputType + " a" + std::to_string(i) + ";";
  out += " };\n";

  out += outType + " " + gsEntry + "_emu( " + packedInputName + " gsemu_packed, uint gsemu_vid : SV_VertexID )\n{\n";
  out += "  " + stageType + " input[" + std::to_string(gs.inputVertices) + "];\n";
  for (int i = 0; i < gs.inputVertices; i++)
    out += "  input[" + std::to_string(i) + "] = " + vertexEntry + "( gsemu_packed.a" + std::to_string(i) + " );\n";
  out += "  const uint gsemu_slot = gsemu_vid % " + std::to_string(gs.slots) + "u;\n";
  out += "  uint gsemu_at = 0u;\n";
  out += "  " + outType + " gsemu_out;\n";
  out += "  {\n";
  out += translated;
  out += "\n  }\n";
  // Unused slots (an Append the body skipped on this path) must not contribute
  // geometry: a zero-w vertex collapses every triangle it takes part in.
  out += "  " + outType + " gsemu_dead = gsemu_out;\n";
  out += "  gsemu_dead.Position = float4( 0, 0, 0, 0 );\n";
  out += "  return gsemu_dead;\n";
  out += "}\n";
  (void)primitiveWord;
  return out;
}

bool BuildFatGeometry(const GsProgram &gs, const void *vb, size_t vbSize, size_t stride, bool indicesAre32Bit,
                      const void *indices, size_t indexCount, size_t firstIndex, FatGeometry &out, std::string *error)
{
  out = FatGeometry();
  if (!gs.ok) { if (error) *error = "the geometry shader was not accepted for emulation"; return false; }
  if (!vb || !stride) { if (error) *error = "no vertex data"; return false; }

  const size_t k = (size_t)gs.inputVertices;
  const size_t slots = (size_t)gs.slots;
  if (k == 0 || slots == 0) { if (error) *error = "degenerate geometry shader shape"; return false; }

  const size_t primitives = indexCount / k;
  if (primitives == 0) { if (error) *error = "no whole input primitives in the draw"; return false; }

  const size_t fatStride = k * stride;
  out.vertices.resize(primitives * slots * fatStride);
  out.indices.clear();
  out.indices.reserve(primitives * (slots + gs.restartsAfter.size() + 1));

  std::vector<uint32_t> prim(k);
  uint8_t *dst = out.vertices.data();
  for (size_t p = 0; p < primitives; p++)
  {
    for (size_t i = 0; i < k; i++)
    {
      const size_t at = firstIndex + p * k + i;
      const uint32_t vx = !indices ? (uint32_t)at
                                   : (indicesAre32Bit ? ((const uint32_t *)indices)[at] : (uint32_t)((const uint16_t *)indices)[at]);
      const size_t offset = (size_t)vx * stride;
      if (offset + stride > vbSize)
      {
        if (error) *error = "an index points outside the vertex buffer";
        return false;
      }
      prim[i] = vx;
    }

    for (size_t s = 0; s < slots; s++)
    {
      // Every emitted vertex carries all K input vertices, so the translated
      // vertex shader can run the original stage code on each of them.
      for (size_t i = 0; i < k; i++)
        memcpy(dst + (i * stride), (const uint8_t *)vb + (size_t)prim[i] * stride, stride);
      dst += fatStride;
    }

    size_t restartCursor = 0;
    for (size_t s = 0; s < slots; s++)
    {
      out.indices.push_back((uint32_t)(p * slots + s));
      while (restartCursor < gs.restartsAfter.size() && gs.restartsAfter[restartCursor] == (int)(s + 1))
      {
        out.indices.push_back(kPrimitiveRestartIndex);
        restartCursor++;
      }
    }
    // Input primitives are independent strips, so one has to be separated from
    // the next.  When the body already restarted after its last slot the strip
    // is closed and a second restart would just be a no-op.  A PointStream has
    // no strips to break.
    if (gs.strips && (gs.restartsAfter.empty() || gs.restartsAfter.back() != gs.slots))
      out.indices.push_back(kPrimitiveRestartIndex);
  }

  out.vertexCount = (uint32_t)(out.vertices.size() / fatStride);
  return true;
}

} // namespace phxvk
