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
  // At least one digit: `+` on its own used to pass this test and atoi("+")
  // returned 0, so `k < +abc` looked like a literal trip count of zero.
  const size_t digits = p;
  while (p < s.size() && isdigit((unsigned char)s[p])) p++;
  if (p == digits) return false;
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

// One of `if` / `switch`: only one arm runs, so the Appends count is the largest
// arm rather than their sum.  Returns the position just past the construct.
// A branch that contains a RestartStrip is left to the conservative path: the
// strip layout would then depend on which arm runs.
size_t ScanConditional(const std::string &text, size_t at, size_t end, CallScan &scan, int depth);
size_t ScanSwitch(const std::string &text, size_t at, size_t end, CallScan &scan, int depth);
bool IdentAt(const std::string &text, size_t at, const char *word, size_t end);

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
      // A loop whose trip count cannot be read: walk its body ONCE, into its own
      // scan, so its Appends are counted once (the loop may run any number of
      // times) and a RestartStrip inside it is noticed.  It has to be a separate
      // scan: a restart inside it makes the whole shape unsupported, because the
      // strip layout would depend on the trip count.
      //
      // This used to pass `scan` to the recursion and then test a fresh, empty
      // `inner`, so the flag was never set and such a shader was emulated with a
      // strip layout guessed from "the loop runs once".
      std::string ignored;
      size_t bodyStart = 0, bodyEnd = 0;
      if (ExtractBody(text, at, ignored, &bodyStart, &bodyEnd))
      {
        CallScan inner;
        ScanRange(text, bodyStart, bodyEnd, inner, depth + 1);
        scan.appends += inner.appends;
        scan.unknown = scan.unknown || !inner.restartsAfter.empty();
        at = bodyEnd + 1;
        continue;
      }
    }

    if (IdentAt(text, at, "if", end))
    {
      at = ScanConditional(text, at, end, scan, depth + 1);
      continue;
    }
    if (IdentAt(text, at, "switch", end))
    {
      at = ScanSwitch(text, at, end, scan, depth + 1);
      continue;
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


bool IdentAt(const std::string &text, size_t at, const char *word, size_t end)
{
  const size_t n = strlen(word);
  if (at + n > end) return false;
  if (text.compare(at, n, word) != 0) return false;
  if (at > 0 && IsIdentChar(text[at - 1])) return false;
  if (at + n < text.size() && IsIdentChar(text[at + n])) return false;
  return true;
}

// Position just past a '(' ... ')' group starting at `open`.
static size_t MatchParen(const std::string &text, size_t open, size_t end)
{
  int d = 0;
  for (size_t q = open; q < end; q++)
  {
    if (text[q] == '(') d++;
    else if (text[q] == ')' && --d == 0) return q + 1;
  }
  return std::string::npos;
}

// The extent of one statement/block starting at `from`: a brace block, or a
// single statement up to its semicolon.  `after` is one past the construct --
// ExtractBody() would stop ON the closing brace, which makes the caller walk the
// block a second time (that is how an if/else counted its arms twice).
static bool StatementRange(const std::string &text, size_t from, size_t end, size_t &begin, size_t &after)
{
  size_t p = from;
  SkipSpace(text, p);
  if (p >= end) return false;
  if (text[p] == '{')
  {
    int depth = 0;
    for (size_t q = p; q < end; q++)
    {
      if (text[q] == '{') depth++;
      else if (text[q] == '}' && --depth == 0) { begin = p + 1; after = q + 1; return true; }
    }
    return false;
  }
  begin = p;
  after = StatementEnd(text, p, end);
  return after > begin && after <= end;
}

size_t ScanConditional(const std::string &text, size_t at, size_t end, CallScan &scan, int depth)
{
  if (depth > 6) { scan.unknown = true; return at + 2; }
  const size_t open = text.find('(', at);
  if (open == std::string::npos || open >= end) return at + 2;
  const size_t afterCond = MatchParen(text, open, end);
  if (afterCond == std::string::npos) return at + 2;

  size_t thenBegin = 0, thenAfter = 0;
  if (!StatementRange(text, afterCond, end, thenBegin, thenAfter)) return at + 2;

  CallScan thenScan, elseScan;
  ScanRange(text, thenBegin, thenAfter, thenScan, depth + 1);

  size_t next = thenAfter;
  bool haveElse = false;
  {
    size_t p = next;
    SkipSpace(text, p);
    if (IdentAt(text, p, "else", end))
    {
      size_t q = p + 4;
      SkipSpace(text, q);
      haveElse = true;
      if (IdentAt(text, q, "if", end))
      {
        next = ScanConditional(text, q, end, elseScan, depth + 1);
      }
      else
      {
        size_t begin = 0, after = 0;
        if (StatementRange(text, q, end, begin, after))
        {
          ScanRange(text, begin, after, elseScan, depth + 1);
          next = after;
        }
      }
    }
  }

  const bool restarts = !thenScan.restartsAfter.empty() || !elseScan.restartsAfter.empty();
  if (restarts)
  {
    // Conservative: keep both arms' counts and restarts, so a restart inside a
    // branch is still noticed by ConditionalRestartFollowedByAppend().
    scan.appends += thenScan.appends + elseScan.appends;
    for (int n : thenScan.restartsAfter) scan.restartsAfter.push_back(scan.appends - elseScan.appends - thenScan.appends + n);
    for (int n : elseScan.restartsAfter) scan.restartsAfter.push_back(scan.appends - elseScan.appends + n);
    scan.unknown = scan.unknown || thenScan.unknown || elseScan.unknown;
  }
  else
  {
    scan.appends += thenScan.appends > elseScan.appends ? thenScan.appends : elseScan.appends;
    scan.unknown = scan.unknown || thenScan.unknown || elseScan.unknown;
  }
  (void)haveElse;
  return next;
}

// case/default labels at brace depth 0 inside the switch body.
size_t ScanSwitch(const std::string &text, size_t at, size_t end, CallScan &scan, int depth)
{
  if (depth > 6) { scan.unknown = true; return at + 6; }
  const size_t open = text.find('(', at);
  if (open == std::string::npos || open >= end) return at + 6;
  const size_t afterCond = MatchParen(text, open, end);
  if (afterCond == std::string::npos) return at + 6;

  std::string ignored;
  size_t bodyBegin = 0, bodyAfter = 0;
  if (!StatementRange(text, afterCond, end, bodyBegin, bodyAfter)) return at + 6;
  if (bodyAfter > bodyBegin) bodyAfter--; // the label splitter wants the braces excluded

  // Split into arms at labels, then take the largest suffix sum: fallthrough
  // means an arm can run the arms after it, so the suffix is the worst case.
  std::vector<std::pair<size_t, size_t>> arms;
  {
    size_t armStart = bodyBegin;
    int brace = 0;
    for (size_t q = bodyBegin; q < bodyAfter; q++)
    {
      if (text[q] == '{') brace++;
      else if (text[q] == '}') brace--;
      else if (brace == 0 && (IdentAt(text, q, "case", bodyAfter) || IdentAt(text, q, "default", bodyAfter)))
      {
        if (q > armStart) arms.push_back({armStart, q});
        armStart = q;
      }
    }
    if (armStart < bodyAfter) arms.push_back({armStart, bodyAfter});
  }
  if (arms.empty()) { ScanRange(text, bodyBegin, bodyAfter, scan, depth + 1); return bodyAfter; }

  std::vector<CallScan> counts(arms.size());
  std::vector<bool> falls(arms.size(), false);
  bool anyRestart = false;
  for (size_t i = 0; i < arms.size(); i++)
  {
    ScanRange(text, arms[i].first, arms[i].second, counts[i], depth + 1);
    anyRestart = anyRestart || !counts[i].restartsAfter.empty();
    // An arm without a `break` or `return` falls through into the next one.
    bool exits = false;
    for (size_t q = arms[i].first; q < arms[i].second; q++)
      if (IdentAt(text, q, "break", arms[i].second) || IdentAt(text, q, "return", arms[i].second)) { exits = true; break; }
    falls[i] = !exits;
  }

  if (anyRestart)
  {
    for (size_t i = 0; i < counts.size(); i++)
    {
      scan.appends += counts[i].appends;
      scan.unknown = scan.unknown || counts[i].unknown;
    }
    return bodyAfter;
  }

  // Worst case from each arm: itself plus everything it can fall through into.
  std::vector<int> worst(counts.size() + 1, 0);
  for (size_t i = counts.size(); i-- > 0;)
    worst[i] = counts[i].appends + (falls[i] ? worst[i + 1] : 0);
  int best = 0;
  for (int w : worst)
    if (w > best) best = w;
  scan.appends += best;
  for (const CallScan &c : counts) scan.unknown = scan.unknown || c.unknown;
  return bodyAfter;
}

CallScan ScanBody(const std::string &body)
{
  CallScan scan;
  ScanRange(body, 0, body.size(), scan, 0);
  return scan;
}

} // namespace


std::string PrecedingWord(const std::string &body, size_t at);
bool IsInsideConditional(const std::string &body, size_t at);

// True when a RestartStrip sits somewhere that may not execute -- the body of a
// braceless if/for/while, or inside a brace block opened by one -- and an Append
// follows it.  Only then is the static strip layout a guess.
bool ConditionalRestartFollowedByAppend(const std::string &body)
{
  for (size_t at = body.find("RestartStrip"); at != std::string::npos; at = body.find("RestartStrip", at + 1))
  {
    if (body.find("Append", at) == std::string::npos) continue; // nothing after it
    if (!IsInsideConditional(body, at)) continue;
    return true;
  }
  return false;
}

// True when the block opened by the brace at `brace` is a conditional or a
// loop body rather than a plain scope or a function body.
bool OpenerIsConditional(const std::string &body, size_t brace)
{
  size_t r = brace;
  while (r > 0 && (body[r - 1] == ' ' || body[r - 1] == '\t' || body[r - 1] == '\n' || body[r - 1] == '\r')) r--;
  if (r > 0 && body[r - 1] == ')')
  {
    int depth = 0;
    size_t q = r;
    while (q > 0)
    {
      q--;
      if (body[q] == ')') depth++;
      else if (body[q] == '(' && --depth == 0) break;
    }
    const std::string head = PrecedingWord(body, q);
    return head == "if" || head == "for" || head == "while" || head == "switch";
  }
  const std::string head = PrecedingWord(body, r);
  return head == "else" || head == "do";
}

// The '{' that opens the block closed by the '}' at `close`.
size_t MatchingOpenBrace(const std::string &body, size_t close)
{
  int depth = 0;
  size_t q = close + 1;
  while (q > 0)
  {
    q--;
    if (body[q] == '}') depth++;
    else if (body[q] == '{' && --depth == 0) return q;
  }
  return 0;
}

// Walks outwards from `at` through the blocks that actually contain it.
//
// The first version got two things wrong.  Anchored on a `}` it decided the
// closed block was the container, so `if (a) {} else {} RestartStrip();` -- an
// unconditional restart -- was called conditional.  And it had no case for `{`
// at all, so a RestartStrip inside a *braced* branch was missed entirely: the
// one direction that matters, because missing it means a wrong picture rather
// than a refusal.  Anchored on `{` we are inside that block, so ask what opens
// it; anchored on `}` the block has already ended, so step over it and keep
// walking out.
bool IsInsideConditional(const std::string &body, size_t at)
{
  size_t p = at;
  // `OutputStream.RestartStrip()`: step back over the receiver so the walk
  // starts at the statement, not in the middle of an identifier.
  {
    while (p > 0 && (body[p - 1] == ' ' || body[p - 1] == '\t')) p--;
    if (p > 0 && body[p - 1] == '.')
    {
      p--;
      while (p > 0 && IsIdentChar(body[p - 1])) p--;
    }
  }
  for (int guard = 0; guard < 32 && p > 0; guard++)
  {
    while (p > 0 && (body[p - 1] == ' ' || body[p - 1] == '\t' || body[p - 1] == '\n' || body[p - 1] == '\r')) p--;
    if (p == 0) return false;
    const char c = body[p - 1];
    if (c == ';' || c == ',')
    {
      p--;
      continue;
    }
    if (c == '}')
    {
      // A block that has already closed: not our container.
      p = MatchingOpenBrace(body, p - 1);
      continue;
    }
    if (c == '{')
    {
      // We are inside this one.
      if (OpenerIsConditional(body, p - 1)) return true;
      p--;
      continue;
    }
    if (c == ')')
    {
      // A braceless body: `if (x) RestartStrip();`
      if (OpenerIsConditional(body, p)) return true;
      int depth = 0;
      size_t q = p;
      while (q > 0)
      {
        q--;
        if (body[q] == ')') depth++;
        else if (body[q] == '(' && --depth == 0) break;
      }
      p = q;
      continue;
    }
    return false; // in the middle of a statement: nothing conditional around it
  }
  return false;
}

std::string PrecedingWord(const std::string &body, size_t at)
{
  size_t p = at;
  while (p > 0 && (body[p - 1] == ' ' || body[p - 1] == '\t' || body[p - 1] == '\n' || body[p - 1] == '\r')) p--;
  const size_t end = p;
  while (p > 0 && IsIdentChar(body[p - 1])) p--;
  return body.substr(p, end - p);
}

// The name of a system value in the parameter list that the emulation cannot
// supply, or an empty string.
std::string UnsupportedSystemValue(const std::string &params)
{
  for (size_t at = params.find(": SV_"); at != std::string::npos; at = params.find(": SV_", at + 1))
  {
    size_t p = at + 2;
    while (p < params.size() && (params[p] == ' ' || params[p] == '\t')) p++;
    const size_t start = p;
    while (p < params.size() && IsIdentChar(params[p])) p++;
    const std::string semantic = params.substr(start, p - start);
    if (semantic != "SV_PrimitiveID" && semantic != "SV_GSInstanceID")
    {
      size_t q = at;
      while (q > 0 && (params[q - 1] == ' ' || params[q - 1] == '\t')) q--;
      const size_t nameEnd = q;
      while (q > 0 && IsIdentChar(params[q - 1])) q--;
      return params.substr(q, nameEnd - q) + " : " + semantic;
    }
  }
  return std::string();
}

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
  gs.inputType = shape.inputType;
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
  if (scan.appends == 0)
  {
    // The scan reads the source before the preprocessor has run, so an Append
    // written through a macro is invisible to it.  Say that, rather than
    // claiming the shader appends nothing.
    gs.reason = (source.find("#define") != std::string::npos && source.find("Append") != std::string::npos)
                    ? "no Append call is visible to a text scan -- if the Append is written through a #define, "
                      "the emulation cannot see it"
                    : "the geometry shader appends nothing";
    return gs;
  }
  if (scan.appends > shape.maxVertexCount)
  {
    gs.reason = "more Append calls than maxvertexcount allows: counting the longest path through the shader "
                "(mutually exclusive if/switch arms are counted once each, but fallthrough and loops are "
                "counted in full)";
    return gs;
  }

  // A RestartStrip that may not run, with an Append after it, cannot be drawn:
  // the index buffer's primitive restarts are decided here, statically, and a
  // restart that the stream does not take -- or takes somewhere else -- would
  // join triangles the geometry shader kept apart, or split ones it joined.
  // (Clean Slate's wireframe has a conditional RestartStrip as its last
  // statement, which is fine: nothing can follow it.)
  if (ConditionalRestartFollowedByAppend(gsBody))
  {
    gs.reason = "a RestartStrip inside a branch is followed by more Appends, so the strip layout is only known "
                "at run time";
    return gs;
  }

  // System-value inputs the emulated stage cannot supply.  SV_PrimitiveID and
  // SV_GSInstanceID it can (see TranslateGeometryShaderToVertex); anything else
  // has to be refused here, by name, rather than left as an identifier the
  // generated stage never defines.
  {
    const std::string bad = UnsupportedSystemValue(gsParams);
    if (!bad.empty())
    {
      gs.reason = "the geometry shader takes " + bad + ", which the emulation cannot supply";
      return gs;
    }
  }

  // The output element type must expose a Position member: that is what the
  // emulation has to overwrite to collapse unused slots into nothing.
  // From the shape reader, which trims: `TriangleStream < VSOUT >` is legal
  // HLSL and re-reading the angle brackets here kept the spaces.
  const std::string outType = shape.outputType;
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


// Locals standing in for the geometry shader's system-value parameters, which
// the emulated stage is not handed.
std::string SystemValueLocals(const std::string &params, int slots)
{
  std::string out;
  for (size_t at = params.find(": SV_"); at != std::string::npos; at = params.find(": SV_", at + 1))
  {
    size_t p = at + 2;
    while (p < params.size() && (params[p] == ' ' || params[p] == '\t')) p++;
    const size_t start = p;
    while (p < params.size() && IsIdentChar(params[p])) p++;
    const std::string semantic = params.substr(start, p - start);
    size_t q = at;
    while (q > 0 && (params[q - 1] == ' ' || params[q - 1] == '\t')) q--;
    const size_t nameEnd = q;
    while (q > 0 && IsIdentChar(params[q - 1])) q--;
    const std::string name = params.substr(q, nameEnd - q);
    if (name.empty()) continue;
    if (semantic == "SV_PrimitiveID")
      out += "  uint " + name + " = gsemu_vid / " + std::to_string(slots) + "u;\n";
    else if (semantic == "SV_GSInstanceID")
      out += "  uint " + name + " = 0u;\n";
  }
  return out;
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

  // The type the geometry shader reads from the vertex shader.  The shape
  // reader already worked it out -- including the case where HLSL's optional
  // primitive modifier is left out, which reading the parameter list here used
  // to get wrong (it took the parameter *name* for the type).
  size_t p = gsParams.find("inout");
  const std::string inPart = (p == std::string::npos) ? gsParams : gsParams.substr(0, p);
  const std::string stageType = gs.inputType;
  if (stageType.empty()) return std::string();

  // The name the geometry shader gives its input array.  The generated vertex
  // entry point runs the real vertex shader once per input vertex and has to
  // store the results in a local array that the translated body can index, so
  // that array is declared under exactly this name.  It used to be called
  // `input` unconditionally, which silently produced HLSL that did not compile
  // ("error: 'i' : unknown variable") for any geometry shader that named the
  // parameter anything else -- every geometry shader in Clean Slate happens to
  // call it `input`, so the port only ever worked by luck.
  std::string inputName;
  {
    const size_t bracket = inPart.find('[');
    if (bracket != std::string::npos)
    {
      size_t at = bracket;
      while (at > 0 && (inPart[at - 1] == ' ' || inPart[at - 1] == '\t')) at--;
      const size_t end = at;
      while (at > 0 && IsIdentChar(inPart[at - 1])) at--;
      inputName = inPart.substr(at, end - at);
    }
  }
  if (inputName.empty()) return std::string();

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
    size_t r = q - 1; // the '.'
    // `o . Append(...)` is legal HLSL, so skip the whitespace on both sides of
    // the dot before looking for the receiver's name -- skipping only the space
    // before it left the `o` behind and produced `o { if (...) return ... }`.
    while (r > 0 && (body[r - 1] == ' ' || body[r - 1] == '\t')) r--;
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

  // The fat vertices arrive in a storage buffer, not in vertex attributes: a
  // shape whose inputs add up to more attribute locations than the device has
  // would otherwise be refused, and the demo's own shapes come close (24 of 31
  // here, 6 per input vertex on a device at Vulkan's minimum of 16).
  //
  // t15 keeps out of the way of the engine's own t registers, and the shim maps
  // it to binding kBindingTexture + 15.
  out += "StructuredBuffer<" + std::string(packedInputName) + "> gsemu_fat : register(t15);\n";

  out += outType + " " + gsEntry + "_emu( uint gsemu_vid : SV_VertexID )\n{\n";
  out += "  " + std::string(packedInputName) + " gsemu_packed = gsemu_fat[gsemu_vid];\n";
  out += "  " + stageType + " " + inputName + "[" + std::to_string(gs.inputVertices) + "];\n";
  for (int i = 0; i < gs.inputVertices; i++)
    out += "  " + inputName + "[" + std::to_string(i) + "] = " + vertexEntry + "( gsemu_packed.a" + std::to_string(i) + " );\n";
  out += "  const uint gsemu_slot = gsemu_vid % " + std::to_string(gs.slots) + "u;\n";
  out += "  uint gsemu_at = 0u;\n";
  // Geometry shader system-value inputs.  The emulation draws one input
  // primitive as `slots` consecutive vertices, so the primitive index is
  // gsemu_vid / slots; there is no geometry shader instancing here, so
  // SV_GSInstanceID is 0.  AnalyseGeometryShader() has already refused any
  // other system value, so anything left is one of these two.
  out += SystemValueLocals(gsParams, gs.slots);
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
  return out;
}

bool BuildFatGeometry(const GsProgram &gs, const void *vb, size_t vbSize, size_t stride, bool indicesAre32Bit,
                      const void *indices, size_t indexCount, size_t firstIndex, int baseVertex, FatGeometry &out,
                      std::string *error)
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
      // D3D11 adds BaseVertexLocation to every index before it fetches; do the
      // same here rather than making the caller fold it into `vb`.
      const uint32_t raw = !indices ? (uint32_t)at
                                    : (indicesAre32Bit ? ((const uint32_t *)indices)[at]
                                                       : (uint32_t)((const uint16_t *)indices)[at]);
      const uint32_t vx = raw + (uint32_t)baseVertex;
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
