// src/rhi/shader_interface.cpp
//
// See shader_interface.h for why this exists.  This file is a small, closed
// SPIR-V rewriter: it only ever widens a varying's type and pads the values
// stored into it, and it refuses -- leaving the module untouched -- anything it
// cannot follow exactly: an access chain into a varying, a copy into one, or a
// varying that is a matrix, an array or a struct.

#include "shader_interface.h"

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace phxvk
{
namespace
{

// ---------------------------------------------------------------------------
// SPIR-V binary encoding.  SPIR-V 1.x section 3 (binary form) plus the unified
// grammar for the handful of opcodes and decorations named here; the framework
// headers are not installed next to shaderc, and d3d11_shader.cpp reads the
// module directly for the same reason.
//
// "Position p of an instruction" below means word `at + p`, so position 0 is
// the word holding the word count and the opcode.  The SPIR-V specification
// numbers operands from 1, so operand n is at position n.

enum Op : uint16_t
{
  kOpTypeInt = 21,
  kOpTypeFloat = 22,
  kOpTypeVector = 23,
  kOpTypePointer = 32,
  kOpConstant = 43,
  kOpFunction = 54,
  kOpVariable = 59,
  kOpLoad = 61,
  kOpStore = 62,
  kOpAccessChain = 65,
  kOpInBoundsAccessChain = 66,
  kOpPtrAccessChain = 67,
  kOpDecorate = 71,
  kOpVectorShuffle = 79,
  kOpCompositeConstruct = 80,
  kOpCompositeExtract = 81,
};

enum : uint32_t
{
  kDecorationBuiltIn = 11,
  kDecorationLocation = 30,
  kStorageClassInput = 1,
  kStorageClassOutput = 3,
  kSpirvMagic = 0x07230203u,
  kNoStorage = 0xFFFFFFFFu,
  // The bit pattern of 1.0f: the .w a two or three component float varying
  // never wrote.  D3D11 reads an unwritten component as 0 and an unwritten .w
  // as 1, which is what its input assembler does for a missing component.
  kFloatOne = 0x3f800000u,
};

struct Inst
{
  size_t at = 0;    // word index of the instruction's first word
  uint32_t len = 0; // its word count
  uint16_t op = 0;
};

struct Module
{
  const uint32_t *words = nullptr;
  uint32_t bound = 0;
  std::vector<Inst> insts;

  bool Parse(const uint32_t *w, size_t n)
  {
    if (n < 5 || w[0] != kSpirvMagic) return false;
    words = w;
    bound = w[3];
    size_t i = 5;
    while (i < n)
    {
      const uint32_t head = w[i];
      const uint32_t len = head >> 16;
      if (len == 0 || i + len > n) return false;
      insts.push_back(Inst{i, len, (uint16_t)(head & 0xffff)});
      i += len;
    }
    return true;
  }

  uint32_t At(size_t idx, size_t position) const { return words[insts[idx].at + position]; }
  bool Is(size_t idx, Op op) const { return insts[idx].op == (uint16_t)op; }

  // The instruction whose result id is `id`, or insts.size().  Position 1 is
  // the result id for a type, a constant and an OpVariable's *pointer type*,
  // but NOT for an OpVariable, whose result id is at position 2 -- callers
  // that start from an OpVariable must use the index they already have.
  size_t ResultDefinitionOf(uint32_t id) const
  {
    for (size_t i = 0; i < insts.size(); i++)
      if (insts[i].len >= 2 && words[insts[i].at + 1] == id) return i;
    return insts.size();
  }

  bool EndsBefore(size_t idx, size_t end) const { return insts[idx].at + insts[idx].len <= end; }
};

uint32_t Pack(uint32_t op, uint32_t wordCount) { return (wordCount << 16) | (op & 0xffffu); }

// The shape of one varying's type, as far as this file can rewrite it.
struct Pointee
{
  bool rewritable = false;    // a 32-bit scalar, or a vector of one
  uint32_t componentType = 0; // the OpTypeFloat / OpTypeInt
  uint32_t components = 1;    // 1..4
};

Pointee DescribePointee(const Module &m, uint32_t typeId)
{
  const Pointee none;
  const size_t def = m.ResultDefinitionOf(typeId);
  if (def == m.insts.size()) return none;

  if (m.Is(def, kOpTypeFloat) || m.Is(def, kOpTypeInt))
  {
    if (m.insts[def].len < 3 || m.At(def, 2) != 32) return none; // only 32-bit values are padded
    Pointee p;
    p.rewritable = true;
    p.componentType = typeId;
    p.components = 1;
    return p;
  }

  if (m.Is(def, kOpTypeVector) && m.insts[def].len >= 4)
  {
    const uint32_t component = m.At(def, 2);
    const uint32_t count = m.At(def, 3);
    if (count < 1 || count > 4) return none;
    const Pointee c = DescribePointee(m, component);
    if (!c.rewritable || c.components != 1) return none;
    Pointee p;
    p.rewritable = true;
    p.componentType = component;
    p.components = count;
    return p;
  }

  return none; // a matrix, an array, a struct, ... -- left untouched
}

// A planned replacement for one instruction.
struct Patch
{
  std::vector<uint32_t> before;      // whole instructions to emit before it
  std::vector<uint32_t> replacement; // empty: keep the original instruction
  std::vector<uint32_t> after;       // whole instructions to emit after it
};

struct Varying
{
  uint32_t variable = 0;
  uint32_t location = 0;
  uint32_t pointerType = 0;
  size_t definition = 0; // the OpVariable's own instruction index
  Pointee pointee;
};

// The numbered varyings the entry point declares for one storage class.
// Output is what a vertex shader hands the pixel shader, Input is what a pixel
// shader reads.  Built-ins (SV_Position and friends) carry a BuiltIn
// decoration instead of a Location and are not varyings.
std::vector<Varying> CollectVaryings(const Module &m, uint32_t storage)
{
  std::unordered_map<uint32_t, uint32_t> location;
  std::unordered_set<uint32_t> builtin;
  for (size_t i = 0; i < m.insts.size(); i++)
  {
    if (!m.Is(i, kOpDecorate) || m.insts[i].len < 3) continue;
    const uint32_t target = m.At(i, 1);
    const uint32_t decoration = m.At(i, 2);
    if (decoration == kDecorationLocation && m.insts[i].len >= 4) location[target] = m.At(i, 3);
    else if (decoration == kDecorationBuiltIn) builtin.insert(target);
  }

  std::vector<Varying> out;
  for (size_t i = 0; i < m.insts.size(); i++)
  {
    if (!m.Is(i, kOpVariable) || m.insts[i].len < 4) continue;
    if (m.At(i, 3) != storage) continue;
    const uint32_t id = m.At(i, 2);
    auto it = location.find(id);
    if (it == location.end() || builtin.count(id)) continue;

    Varying v;
    v.variable = id;
    v.location = it->second;
    v.pointerType = m.At(i, 1);
    v.definition = i;
    // OpTypePointer %id %storage %pointee
    const size_t ptr = m.ResultDefinitionOf(v.pointerType);
    if (ptr != m.insts.size() && m.Is(ptr, kOpTypePointer) && m.insts[ptr].len >= 4)
      v.pointee = DescribePointee(m, m.At(ptr, 3));
    out.push_back(v);
  }
  return out;
}

uint32_t StorageFor(ShaderStage stage)
{
  if (stage == ShaderStage::Vertex) return kStorageClassOutput;
  if (stage == ShaderStage::Pixel) return kStorageClassInput;
  return kNoStorage; // this stage has no varying interface in this port
}

} // namespace

bool ReadStageVaryings(const uint32_t *words, size_t wordCount, ShaderStage stage,
                       std::vector<StageVarying> *out, std::string *why)
{
  out->clear();
  Module m;
  if (!m.Parse(words, wordCount))
  {
    if (why) *why = "not a well formed SPIR-V module";
    return false;
  }
  const uint32_t storage = StorageFor(stage);
  if (storage == kNoStorage)
  {
    if (why) *why = "this stage has no varying interface";
    return false;
  }

  for (const Varying &v : CollectVaryings(m, storage))
  {
    StageVarying s;
    s.variable = v.variable;
    s.location = v.location;
    s.componentType = v.pointee.rewritable ? v.pointee.componentType : 0;
    s.components = v.pointee.rewritable ? v.pointee.components : 0;
    out->push_back(s);
  }
  return true;
}

NormaliseResult NormaliseVaryings(const std::vector<uint32_t> &spirv, ShaderStage stage,
                                  std::vector<uint32_t> *out, std::string *why,
                                  uint32_t componentBudget)
{
  Module m;
  if (!m.Parse(spirv.data(), spirv.size()))
  {
    if (why) *why = "not a well formed SPIR-V module";
    out->assign(spirv.begin(), spirv.end());
    return NormaliseResult::Refused;
  }

  const uint32_t storage = StorageFor(stage);
  const std::vector<Varying> varyings = CollectVaryings(m, storage);
  bool anyNarrow = false;
  for (const Varying &v : varyings)
    if (v.pointee.rewritable && v.pointee.components < 4) anyNarrow = true;

  if (anyNarrow && componentBudget)
  {
    // What the stage would declare afterwards: four components for each varying
    // that gets widened, whatever it declared for the rest.
    uint64_t after = 0;
    for (const Varying &v : varyings)
      after += (v.pointee.rewritable && v.pointee.components < 4) ? 4u : (uint64_t)v.pointee.components;
    if (after > componentBudget)
    {
      if (why)
      {
        char buf[192];
        snprintf(buf, sizeof buf,
                 "widening to four components would declare %llu varying components and this device allows %u",
                 (unsigned long long)after, componentBudget);
        *why = buf;
      }
      out->assign(spirv.begin(), spirv.end());
      return NormaliseResult::Refused;
    }
  }

  if (storage == kNoStorage || !anyNarrow)
  {
    // Either every varying is four components wide already, or none of them is
    // a shape this file can rewrite.  Anything it cannot rewrite is left
    // exactly as it was, which is what the port did before this file existed.
    out->assign(spirv.begin(), spirv.end());
    return NormaliseResult::Unchanged;
  }

  size_t firstFunction = m.insts.size();
  for (size_t i = 0; i < m.insts.size(); i++)
    if (m.Is(i, kOpFunction)) { firstFunction = i; break; }
  if (firstFunction == m.insts.size())
  {
    if (why) *why = "module has no function";
    out->assign(spirv.begin(), spirv.end());
    return NormaliseResult::Refused;
  }

  // Every mention of a varying inside a function body has to be something this
  // rewrite leaves correct.  It is allowed to be
  //
  //   * the pointer operand of an OpLoad or an OpStore, which are the two
  //     instructions it rewrites, or
  //   * the base operand of an access chain, which indexes one component of
  //     the varying.  Widening a vecN to a vec4 leaves components 0..N-1 where
  //     they were, so every index the original could have used still means
  //     what it meant -- the chain itself needs no change.
  //
  // Any other mention is either a use this file cannot follow -- so the whole
  // module is refused -- or a literal that merely happens to have the same
  // value, in which case refusing is merely conservative.  Both are safe.
  for (const Varying &v : varyings)
  {
    if (!v.pointee.rewritable || v.pointee.components >= 4) continue;
    for (size_t i = firstFunction; i < m.insts.size(); i++)
    {
      const Inst &in = m.insts[i];
      for (size_t p = 1; p < in.len; p++)
      {
        if (m.words[in.at + p] != v.variable) continue;
        if (in.op == (uint16_t)kOpLoad && p == 3) continue;
        if (in.op == (uint16_t)kOpStore && p == 1) continue;
        if ((in.op == (uint16_t)kOpAccessChain || in.op == (uint16_t)kOpInBoundsAccessChain ||
             in.op == (uint16_t)kOpPtrAccessChain) && p == 3)
          continue;
        if (why)
        {
          char buf[192];
          snprintf(buf, sizeof buf,
                   "varying %u at location %u is mentioned by opcode %u, which this rewrite cannot follow",
                   v.variable, v.location, (unsigned)in.op);
          *why = buf;
        }
        out->assign(spirv.begin(), spirv.end());
        return NormaliseResult::Refused;
      }
    }
  }

  // ---- the types, constants and pointer types the rewrite needs ----------
  // A varying's OpVariable cannot simply be retyped in place.  Its new type has
  // to be one the module already declares -- SPIR-V forbids a duplicate
  // `OpTypeVector %float 4` -- and glslang interleaves each type with the
  // variable that uses it, so the existing one is often declared *below* the
  // variable.  Types, constants and global variables all live in one section in
  // which every id must be defined before use but which is otherwise unordered,
  // so the variables this touches are moved to the end of that section, after
  // the new types.  Nothing else refers to a varying except OpEntryPoint, whose
  // interface list and decorations are in the sections before it.
  uint32_t nextId = m.bound ? m.bound : 1;
  std::vector<uint32_t> newGlobals;
  std::vector<uint32_t> movedVariables;
  std::unordered_set<size_t> movedAway; // instruction indices that must not be re-emitted

  std::unordered_map<uint32_t, uint32_t> vec4Of;    // component type -> vec4 type
  std::unordered_map<uint64_t, uint32_t> ptrOf;     // (storage << 32) | pointee -> pointer
  std::unordered_map<uint64_t, uint32_t> constants; // (type << 32) | bits -> id
  for (size_t i = 0; i < firstFunction; i++)
  {
    if (m.insts[i].len < 3) continue;
    // OpTypeVector %resultId %component %count
    if (m.Is(i, kOpTypeVector) && m.insts[i].len >= 4 && m.At(i, 3) == 4)
      vec4Of.emplace(m.At(i, 2), m.At(i, 1));
    // OpTypePointer %resultId %storage %pointee
    else if (m.Is(i, kOpTypePointer) && m.insts[i].len >= 4)
      ptrOf.emplace(((uint64_t)m.At(i, 2) << 32) | m.At(i, 3), m.At(i, 1));
    // OpConstant %resultType %resultId %literal
    else if (m.Is(i, kOpConstant) && m.insts[i].len == 4)
      constants.emplace(((uint64_t)m.At(i, 1) << 32) | m.At(i, 3), m.At(i, 2));
  }

  auto constantFor = [&](uint32_t type, uint32_t bits) -> uint32_t {
    const uint64_t key = ((uint64_t)type << 32) | bits;
    auto it = constants.find(key);
    if (it != constants.end()) return it->second;
    const uint32_t id = nextId++;
    const uint32_t words[4] = {Pack(kOpConstant, 4), type, id, bits};
    newGlobals.insert(newGlobals.end(), words, words + 4);
    constants.emplace(key, id);
    return id;
  };

  auto vec4For = [&](uint32_t component) -> uint32_t {
    auto it = vec4Of.find(component);
    if (it != vec4Of.end()) return it->second;
    const uint32_t id = nextId++;
    const uint32_t words[4] = {Pack(kOpTypeVector, 4), id, component, 4};
    newGlobals.insert(newGlobals.end(), words, words + 4);
    vec4Of.emplace(component, id);
    return id;
  };

  auto pointerFor = [&](uint32_t component) -> uint32_t {
    const uint32_t vec4 = vec4For(component);
    const uint64_t key = ((uint64_t)storage << 32) | vec4;
    auto it = ptrOf.find(key);
    if (it != ptrOf.end()) return it->second;
    const uint32_t id = nextId++;
    const uint32_t words[4] = {Pack(kOpTypePointer, 4), id, storage, vec4};
    newGlobals.insert(newGlobals.end(), words, words + 4);
    ptrOf.emplace(key, id);
    return id;
  };

  // ---- plan --------------------------------------------------------------
  std::unordered_map<size_t, Patch> patches; // keyed by instruction index

  for (const Varying &v : varyings)
  {
    if (!v.pointee.rewritable || v.pointee.components >= 4) continue;

    const uint32_t vec4Type = vec4For(v.pointee.componentType);
    const uint32_t newPointerType = pointerFor(v.pointee.componentType);
    const uint32_t components = v.pointee.components;

    const size_t componentDef = m.ResultDefinitionOf(v.pointee.componentType);
    const bool floating = componentDef != m.insts.size() && m.Is(componentDef, kOpTypeFloat);
    const uint32_t zero = constantFor(v.pointee.componentType, 0u);
    const uint32_t one = floating ? constantFor(v.pointee.componentType, kFloatOne) : zero;

    // The variable itself has to point at a four component value, so it is
    // rewritten and moved to the end of the globals section (see above).  A
    // varying never has an initializer; refuse rather than move an instruction
    // whose initializer could name something defined below it.
    const size_t varDef = v.definition;
    if (m.insts[varDef].len != 4)
    {
      if (why) *why = "the varying's OpVariable has an initializer";
      out->assign(spirv.begin(), spirv.end());
      return NormaliseResult::Refused;
    }
    {
      // OpVariable %resultType %resultId %storage
      movedVariables.push_back(Pack(kOpVariable, 4));
      movedVariables.push_back(newPointerType);
      movedVariables.push_back(v.variable);
      movedVariables.push_back(m.At(varDef, 3));
      movedAway.insert(varDef);
    }

    for (size_t i = firstFunction; i < m.insts.size(); i++)
    {
      const Inst &in = m.insts[i];

      if (in.op == (uint16_t)kOpLoad && in.len >= 4 && m.At(i, 3) == v.variable)
      {
        // OpLoad %oldType %result %pointer [%memoryAccess]
        // Load a vec4 under a fresh id, then narrow it back to the declared
        // type under the original id, so every existing use stays valid.
        const uint32_t oldType = m.At(i, 1);
        const uint32_t oldId = m.At(i, 2);
        const uint32_t loaded = nextId++;
        Patch &p = patches[i];
        p.replacement.push_back(Pack(kOpLoad, in.len));
        p.replacement.push_back(vec4Type);
        p.replacement.push_back(loaded);
        for (size_t k = 3; k < in.len; k++) p.replacement.push_back(m.At(i, k));

        if (components == 1)
        {
          const uint32_t narrow[5] = {Pack(kOpCompositeExtract, 5), oldType, oldId, loaded, 0};
          p.after.insert(p.after.end(), narrow, narrow + 5);
        }
        else
        {
          p.after.push_back(Pack(kOpVectorShuffle, 5 + components));
          p.after.push_back(oldType);
          p.after.push_back(oldId);
          p.after.push_back(loaded);
          p.after.push_back(loaded);
          for (uint32_t c = 0; c < components; c++) p.after.push_back(c);
        }
      }
      else if (in.op == (uint16_t)kOpStore && in.len >= 3 && m.At(i, 1) == v.variable)
      {
        // OpStore %pointer %value [%memoryAccess]
        // Widen the value to a vec4 before storing it.
        const uint32_t value = m.At(i, 2);
        Patch &p = patches[i];

        if (components == 1)
        {
          const uint32_t padded = nextId++;
          const uint32_t construct[7] = {Pack(kOpCompositeConstruct, 7), vec4Type, padded, value, zero, zero, one};
          p.before.insert(p.before.end(), construct, construct + 7);
          p.replacement.push_back(Pack(kOpStore, in.len));
          p.replacement.push_back(v.variable);
          p.replacement.push_back(padded);
        }
        else
        {
          std::vector<uint32_t> parts;
          for (uint32_t c = 0; c < components; c++)
          {
            const uint32_t element = nextId++;
            const uint32_t extract[5] = {Pack(kOpCompositeExtract, 5), v.pointee.componentType, element, value, c};
            p.before.insert(p.before.end(), extract, extract + 5);
            parts.push_back(element);
          }
          while (parts.size() < 3) parts.push_back(zero); // the .z a vec2 never wrote
          parts.push_back(one);                          // and the .w

          const uint32_t padded = nextId++;
          p.before.push_back(Pack(kOpCompositeConstruct, 3 + (uint32_t)parts.size()));
          p.before.push_back(vec4Type);
          p.before.push_back(padded);
          for (uint32_t part : parts) p.before.push_back(part);

          p.replacement.push_back(Pack(kOpStore, in.len));
          p.replacement.push_back(v.variable);
          p.replacement.push_back(padded);
        }
        for (size_t k = 3; k < in.len; k++) p.replacement.push_back(m.At(i, k));
      }
    }
  }

  // ---- emit --------------------------------------------------------------
  std::vector<uint32_t> result;
  result.reserve(spirv.size() + newGlobals.size() + movedVariables.size() + 64);
  result.push_back(spirv[0]);
  result.push_back(spirv[1]);
  result.push_back(spirv[2]);
  result.push_back(nextId); // the bound has to cover every id minted above
  result.push_back(spirv[4]);

  for (size_t i = 0; i < m.insts.size(); i++)
  {
    // The end of the globals section: the new types go here, and so do the
    // retyped variables that have to follow them.
    if (i == firstFunction)
    {
      result.insert(result.end(), newGlobals.begin(), newGlobals.end());
      result.insert(result.end(), movedVariables.begin(), movedVariables.end());
    }
    if (movedAway.count(i)) continue;
    auto it = patches.find(i);
    if (it != patches.end()) result.insert(result.end(), it->second.before.begin(), it->second.before.end());
    if (it != patches.end() && !it->second.replacement.empty())
      result.insert(result.end(), it->second.replacement.begin(), it->second.replacement.end());
    else
      result.insert(result.end(), m.words + m.insts[i].at, m.words + m.insts[i].at + m.insts[i].len);
    if (it != patches.end()) result.insert(result.end(), it->second.after.begin(), it->second.after.end());
  }

  *out = std::move(result);
  return NormaliseResult::Rewritten;
}

} // namespace phxvk
