#include "shader_compiler.h"
#include "vk_core.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <map>
#include <set>
#include <cstdlib>
#include <cctype>
#include <sstream>

namespace phxvk
{

const char *ShaderStageName(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex: return "vertex";
  case ShaderStage::Pixel: return "fragment";
  case ShaderStage::Geometry: return "geometry";
  case ShaderStage::Hull: return "tesscontrol";
  case ShaderStage::Domain: return "tesseval";
  case ShaderStage::Compute: return "compute";
  }
  return "?";
}

static shaderc_shader_kind ShadercKind(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex: return shaderc_vertex_shader;
  case ShaderStage::Pixel: return shaderc_fragment_shader;
  case ShaderStage::Geometry: return shaderc_geometry_shader;
  case ShaderStage::Hull: return shaderc_tess_control_shader;
  case ShaderStage::Domain: return shaderc_tess_evaluation_shader;
  case ShaderStage::Compute: return shaderc_compute_shader;
  }
  return shaderc_vertex_shader;
}

//////////////////////////////////////////////////////////////////////////////
// HLSL rewriting
//
// We only need to inject [[vk::binding]] attributes in front of resource
// declarations.  Everything else is left byte-for-byte intact so that error
// messages keep pointing at the original code.

namespace
{

struct ResourceType
{
  const char *hlsl;
  RegisterClass cls;
};

// Types that can carry a register() annotation we care about.  Ordered longest
// first so that `RWStructuredBuffer` wins over `Buffer`.
const ResourceType kResourceTypes[] = {
    {"RWStructuredBuffer", RegisterClass::UAV},
    {"StructuredBuffer", RegisterClass::Texture},
    {"RWTexture2DArray", RegisterClass::UAV},
    {"RWTexture3D", RegisterClass::UAV},
    {"RWTexture2D", RegisterClass::UAV},
    {"RWTexture1D", RegisterClass::UAV},
    {"Texture2DMSArray", RegisterClass::Texture},
    {"Texture2DMS", RegisterClass::Texture},
    {"Texture2DArray", RegisterClass::Texture},
    {"TextureCubeArray", RegisterClass::Texture},
    {"TextureCube", RegisterClass::Texture},
    {"Texture3D", RegisterClass::Texture},
    {"Texture2D", RegisterClass::Texture},
    {"Texture1D", RegisterClass::Texture},
    {"ConstantBuffer", RegisterClass::ConstantBuffer},
    {"SamplerComparisonState", RegisterClass::Sampler},
    {"SamplerState", RegisterClass::Sampler},
    // The legacy lowercase `sampler` keyword.  Dear ImGui's pixel shader uses it
    // ("sampler sampler0; Texture2D texture0;"), and without an entry here it
    // received no binding and defaulted to 0, colliding with the constant buffer
    // at binding 0 - the UI then sampled the constant buffer as its font atlas.
    {"sampler", RegisterClass::Sampler},
    {"RWBuffer", RegisterClass::UAV},
    {"Buffer", RegisterClass::Texture},
    {"cbuffer", RegisterClass::ConstantBuffer},
    {"ByteAddressBuffer", RegisterClass::Texture},
    {"RWByteAddressBuffer", RegisterClass::UAV},
};

bool IsIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Skips a balanced <...> or (...) group starting at `i` (which must point at
// the opening bracket); returns the index just past the closing bracket, or
// npos if unbalanced.
size_t SkipBalanced(const std::string &s, size_t i, char open, char close)
{
  int depth = 0;
  for (; i < s.size(); i++)
  {
    char c = s[i];
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
    {
      while (i < s.size() && s[i] != '\n') i++;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
    {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) i++;
      i++;
      continue;
    }
    if (c == open) depth++;
    else if (c == close)
    {
      depth--;
      if (depth == 0) return i + 1;
    }
  }
  return std::string::npos;
}

uint32_t BindingFor(RegisterClass cls, uint32_t index)
{
  switch (cls)
  {
  case RegisterClass::ConstantBuffer: return kBindingConstantBuffer + index;
  case RegisterClass::Texture: return kBindingTexture + index;
  case RegisterClass::Sampler: return kBindingSampler + index;
  case RegisterClass::UAV: return kBindingUAV + index;
  default: return 0;
  }
}

bool MatchesAt(const std::string &s, size_t i, const char *word)
{
  size_t n = strlen(word);
  if (i + n > s.size()) return false;
  if (s.compare(i, n, word) != 0) return false;
  if (i > 0 && IsIdentChar(s[i - 1])) return false;
  if (i + n < s.size() && IsIdentChar(s[i + n])) return false;
  return true;
}

} // namespace

std::string RewriteHLSLForVulkan(const std::string &hlsl)
{
  std::string out;
  out.reserve(hlsl.size() + 512);

  // D3D11 numbers each register class independently, in declaration order, for
  // declarations that do not name a register.  The engine's own shaders always
  // write `: register(xN)` explicitly, so this only matters for third-party HLSL
  // - Dear ImGui's pixel shader declares `Texture2D texture0;` and
  // `SamplerState sampler0;` with no register at all, and without this they
  // received no [[vk::binding]] and all landed on binding 0 together with the
  // constant buffer.  The shader then sampled the constant buffer as a texture,
  // which is what the striped garbage on screen was.
  uint32_t nextRegister[4] = {0, 0, 0, 0}; // indexed by RegisterClass

  size_t i = 0;
  const size_t n = hlsl.size();
  while (i < n)
  {
    // Pass comments and string literals through untouched.
    if (hlsl[i] == '/' && i + 1 < n && hlsl[i + 1] == '/')
    {
      size_t e = hlsl.find('\n', i);
      if (e == std::string::npos) e = n;
      out.append(hlsl, i, e - i);
      i = e;
      continue;
    }
    if (hlsl[i] == '/' && i + 1 < n && hlsl[i + 1] == '*')
    {
      size_t e = hlsl.find("*/", i + 2);
      e = (e == std::string::npos) ? n : e + 2;
      out.append(hlsl, i, e - i);
      i = e;
      continue;
    }

    // Try to match a resource declaration keyword at this position.
    const ResourceType *matched = nullptr;
    for (const auto &rt : kResourceTypes)
    {
      if (MatchesAt(hlsl, i, rt.hlsl))
      {
        matched = &rt;
        break;
      }
    }
    if (!matched)
    {
      out.push_back(hlsl[i++]);
      continue;
    }

    // Do not rewrite if the declaration already carries an attribute.
    size_t back = out.size();
    while (back > 0 && (out[back - 1] == ' ' || out[back - 1] == '\t' || out[back - 1] == '\n' || out[back - 1] == '\r')) back--;
    if (back >= 8 && out.compare(back - 8, 8, "::binding") == 0)
    {
      out.append(hlsl, i, strlen(matched->hlsl));
      i += strlen(matched->hlsl);
      continue;
    }

    size_t kwEnd = i + strlen(matched->hlsl);

    // Optional <template> argument.
    size_t p = kwEnd;
    while (p < n && (hlsl[p] == ' ' || hlsl[p] == '\t')) p++;
    if (p < n && hlsl[p] == '<')
    {
      size_t e = SkipBalanced(hlsl, p, '<', '>');
      if (e == std::string::npos) e = p + 1;
      p = e;
    }

    // Name (skip for cbuffer, where the name follows immediately as well).
    while (p < n && (hlsl[p] == ' ' || hlsl[p] == '\t' || hlsl[p] == '\n' || hlsl[p] == '\r')) p++;
    size_t nameStart = p;
    while (p < n && IsIdentChar(hlsl[p])) p++;
    if (p == nameStart)
    {
      // Not a declaration shape we understand; emit verbatim.
      out.append(hlsl, i, kwEnd - i);
      i = kwEnd;
      continue;
    }

    // Optional : register(xN)
    while (p < n && (hlsl[p] == ' ' || hlsl[p] == '\t')) p++;
    if (p < n && hlsl[p] == ':')
    {
      size_t q = p + 1;
      while (q < n && isspace((unsigned char)hlsl[q])) q++;
      if (MatchesAt(hlsl, q, "register"))
      {
        size_t r = q + strlen("register");
        while (r < n && isspace((unsigned char)hlsl[r])) r++;
        if (r < n && hlsl[r] == '(')
        {
          size_t close = hlsl.find(')', r);
          if (close == std::string::npos) close = n - 1;
          std::string args = hlsl.substr(r + 1, close - r - 1);
          char letter = 0;
          uint32_t index = 0;
          {
            size_t k = 0;
            while (k < args.size() && isspace((unsigned char)args[k])) k++;
            if (k < args.size() && isalpha((unsigned char)args[k])) letter = args[k++];
            while (k < args.size() && isspace((unsigned char)args[k])) k++;
            index = (uint32_t)strtoul(args.c_str() + k, nullptr, 10);
          }

          // Map the register letter onto our canonical range.  `c` is the
          // implicit constant buffer and glslang already places it at
          // binding 0, so it is left alone.
          bool rewrite = false;
          RegisterClass cls = matched->cls;
          if (letter == 'b' || letter == 't' || letter == 's' || letter == 'u')
          {
            if (letter == 'b') cls = RegisterClass::ConstantBuffer;
            if (letter == 't') cls = RegisterClass::Texture;
            if (letter == 's') cls = RegisterClass::Sampler;
            if (letter == 'u') cls = RegisterClass::UAV;
            rewrite = true;
          }

          if (rewrite)
          {
            // An explicit register fixes that slot; later implicit declarations
            // continue after it, exactly as D3D11 does.
            const uint32_t clsIndex = (uint32_t)cls;
            if (clsIndex < 4 && index + 1 > nextRegister[clsIndex]) nextRegister[clsIndex] = index + 1;

            char attr[64];
            snprintf(attr, sizeof(attr), "[[vk::binding(%u,0)]] ", BindingFor(cls, index));
            out.append(attr);
          }
          // Emit the declaration up to (not including) the register annotation.
          out.append(hlsl, i, p - i);
          i = close + 1;
          // consume the rest of the line-remainder of the annotation? no - the
          // register() was the whole annotation, so continue normally.
          continue;
        }
      }
    }

    // No register(): D3D11 assigns the next free register of that class, so do
    // the same and state it explicitly for glslang.
    {
      const uint32_t clsIndex = (uint32_t)matched->cls;
      if (clsIndex < 4)
      {
        const uint32_t index = nextRegister[clsIndex]++;
        char attr[64];
        snprintf(attr, sizeof(attr), "[[vk::binding(%u,0)]] ", BindingFor(matched->cls, index));
        out.append(attr);
      }
    }
    out.append(hlsl, i, kwEnd - i);
    i = kwEnd;
  }

  return out;
}

//////////////////////////////////////////////////////////////////////////////
// SPIR-V reflection

namespace
{

constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpMemberName = 6;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeMatrix = 24;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpConstant = 43;

constexpr uint32_t kDecorationBlock = 2;
constexpr uint32_t kDecorationBufferBlock = 3;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kDecorationOffset = 35;

struct SpirvType
{
  enum Kind
  {
    Unknown,
    Void,
    Float,
    Int,
    Vector,
    Matrix,
    Image,
    Sampler,
    SampledImage,
    Struct,
    Array,
    RuntimeArray,
    Pointer,
  } kind = Unknown;
  uint32_t elem = 0;      // vector/matrix/array element, pointer pointee
  uint32_t count = 0;     // vector size / array length id
  uint32_t bitWidth = 0;
  bool isSigned = false;
  uint32_t storageClass = 0; // for pointer
  std::vector<uint32_t> members;
};

struct SpirvModule
{
  std::vector<uint32_t> words;
  std::map<uint32_t, SpirvType> types;
  std::map<uint32_t, std::string> names;
  std::map<uint32_t, std::map<uint32_t, uint32_t>> decorations; // id -> decoration -> arg
  std::map<uint32_t, std::map<uint32_t, std::map<uint32_t, uint32_t>>> memberDecorations;
  std::map<uint32_t, uint32_t> constants;
  struct Var
  {
    uint32_t type;
    uint32_t storage;
  };
  std::map<uint32_t, Var> variables;

  void parse(const std::vector<uint32_t> &spv);
  std::string typeDebug(uint32_t id, int depth = 0) const;
  uint32_t arrayLength(uint32_t id) const;
  uint32_t structSize(uint32_t id) const;
  VkFormat formatOf(uint32_t id, uint32_t *components = nullptr) const;
};

void SpirvModule::parse(const std::vector<uint32_t> &spv)
{
  words = spv;
  if (words.size() < 5) return;
  size_t i = 5;
  while (i < words.size())
  {
    uint32_t word = words[i];
    uint32_t op = word & 0xffff;
    uint32_t cnt = word >> 16;
    if (cnt == 0 || i + cnt > words.size()) break;
    const uint32_t *a = words.data() + i + 1;
    switch (op)
    {
    case kOpName:
    {
      const char *s = (const char *)(a + 1);
      names[a[0]] = std::string(s);
      break;
    }
    case kOpDecorate:
      if (cnt >= 3) decorations[a[0]][a[1]] = (cnt >= 4) ? a[2] : 0;
      break;
    case kOpMemberDecorate:
      if (cnt >= 4) memberDecorations[a[0]][a[1]][a[2]] = (cnt >= 5) ? a[3] : 0;
      break;
    case kOpVariable:
      if (cnt >= 4)
      {
        Var v;
        v.type = a[0];
        v.storage = a[2];
        variables[a[1]] = v;
      }
      break;
    case kOpTypePointer:
      if (cnt >= 4)
      {
        SpirvType t;
        t.kind = SpirvType::Pointer;
        t.storageClass = a[1];
        t.elem = a[2];
        types[a[0]] = t;
      }
      break;
    case kOpTypeFloat:
    {
      SpirvType t;
      t.kind = SpirvType::Float;
      t.bitWidth = a[1];
      types[a[0]] = t;
      break;
    }
    case kOpTypeInt:
    {
      SpirvType t;
      t.kind = SpirvType::Int;
      t.bitWidth = a[1];
      t.isSigned = a[2] != 0;
      types[a[0]] = t;
      break;
    }
    case kOpTypeVector:
    {
      SpirvType t;
      t.kind = SpirvType::Vector;
      t.elem = a[1];
      t.count = a[2];
      types[a[0]] = t;
      break;
    }
    case kOpTypeMatrix:
    {
      SpirvType t;
      t.kind = SpirvType::Matrix;
      t.elem = a[1];
      t.count = a[2];
      types[a[0]] = t;
      break;
    }
    case kOpTypeImage:
    {
      SpirvType t;
      t.kind = SpirvType::Image;
      t.elem = a[1];
      types[a[0]] = t;
      break;
    }
    case kOpTypeSampler:
    {
      SpirvType t;
      t.kind = SpirvType::Sampler;
      types[a[0]] = t;
      break;
    }
    case kOpTypeSampledImage:
    {
      SpirvType t;
      t.kind = SpirvType::SampledImage;
      t.elem = a[1];
      types[a[0]] = t;
      break;
    }
    case kOpTypeStruct:
    {
      SpirvType t;
      t.kind = SpirvType::Struct;
      t.members.assign(a + 1, a + cnt - 1);
      types[a[0]] = t;
      break;
    }
    case kOpTypeArray:
    {
      SpirvType t;
      t.kind = SpirvType::Array;
      t.elem = a[1];
      t.count = a[2];
      types[a[0]] = t;
      break;
    }
    case kOpTypeRuntimeArray:
    {
      SpirvType t;
      t.kind = SpirvType::RuntimeArray;
      t.elem = a[1];
      types[a[0]] = t;
      break;
    }
    case kOpConstant:
      if (cnt >= 4) constants[a[1]] = a[2];
      break;
    default: break;
    }
    i += cnt;
  }
}

uint32_t SpirvModule::arrayLength(uint32_t id) const
{
  auto it = constants.find(id);
  return it == constants.end() ? 0 : it->second;
}

VkFormat SpirvModule::formatOf(uint32_t id, uint32_t *components) const
{
  auto it = types.find(id);
  if (it == types.end()) return VK_FORMAT_UNDEFINED;
  const SpirvType &t = it->second;
  if (components) *components = 1;
  switch (t.kind)
  {
  case SpirvType::Float:
    if (components) *components = 1;
    return VK_FORMAT_R32_SFLOAT;
  case SpirvType::Int:
    if (components) *components = 1;
    return t.isSigned ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
  case SpirvType::Vector:
  {
    auto eit = types.find(t.elem);
    if (eit == types.end()) return VK_FORMAT_UNDEFINED;
    const SpirvType &e = eit->second;
    if (components) *components = t.count;
    const bool isFloat = e.kind == SpirvType::Float;
    const bool isSigned = e.kind == SpirvType::Int && e.isSigned;
    const bool isUint = e.kind == SpirvType::Int && !e.isSigned;
    switch (t.count)
    {
    case 2: return isFloat ? VK_FORMAT_R32G32_SFLOAT : (isSigned ? VK_FORMAT_R32G32_SINT : (isUint ? VK_FORMAT_R32G32_UINT : VK_FORMAT_UNDEFINED));
    case 3: return isFloat ? VK_FORMAT_R32G32B32_SFLOAT : (isSigned ? VK_FORMAT_R32G32B32_SINT : (isUint ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_UNDEFINED));
    case 4: return isFloat ? VK_FORMAT_R32G32B32A32_SFLOAT : (isSigned ? VK_FORMAT_R32G32B32A32_SINT : (isUint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_UNDEFINED));
    default: return VK_FORMAT_UNDEFINED;
    }
  }
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

uint32_t SpirvModule::structSize(uint32_t id) const
{
  auto it = types.find(id);
  if (it == types.end() || it->second.kind != SpirvType::Struct) return 0;
  auto md = memberDecorations.find(id);
  if (md == memberDecorations.end()) return 0;
  uint32_t maxEnd = 0;
  for (auto &m : md->second)
  {
    auto oit = m.second.find(kDecorationOffset);
    if (oit == m.second.end()) continue;
    uint32_t off = oit->second;
    // Determine the member's size; only scalars/vectors/matrices are needed
    // for the constant buffers apEx uses.
    uint32_t size = 0;
    if (m.first < it->second.members.size())
    {
      uint32_t mt = it->second.members[m.first];
      auto tit = types.find(mt);
      if (tit != types.end())
      {
        const SpirvType &t = tit->second;
        switch (t.kind)
        {
        case SpirvType::Float:
        case SpirvType::Int: size = 4; break;
        case SpirvType::Vector: size = 4 * (t.count ? t.count : 1); break;
        case SpirvType::Matrix: size = 4 * 4 * (t.count ? t.count : 1); break;
        case SpirvType::Struct: size = structSize(mt); break;
        case SpirvType::Array: size = 0; break; // filled below when possible
        default: size = 4; break;
        }
        if (t.kind == SpirvType::Array)
        {
          uint32_t elemSize = 0;
          auto eit = types.find(t.elem);
          if (eit != types.end())
          {
            auto md2 = memberDecorations.find(mt);
            (void)md2;
            if (eit->second.kind == SpirvType::Struct)
              elemSize = structSize(t.elem);
            else if (eit->second.kind == SpirvType::Vector)
              elemSize = 4 * eit->second.count;
            else
              elemSize = 4;
          }
          uint32_t stride = 0;
          auto ad = decorations.find(mt);
          if (ad != decorations.end())
          {
            auto sit = ad->second.find(6); // ArrayStride
            if (sit != ad->second.end()) stride = sit->second;
          }
          if (!stride) stride = elemSize;
          size = stride * arrayLength(t.count);
        }
      }
    }
    maxEnd = std::max(maxEnd, off + size);
  }
  return maxEnd;
}

RegisterClass ClassifyBinding(uint32_t binding)
{
  if (binding >= kBindingUAV && binding < kBindingUAV + 32) return RegisterClass::UAV;
  if (binding >= kBindingSampler && binding < kBindingSampler + 32) return RegisterClass::Sampler;
  if (binding >= kBindingTexture && binding < kBindingTexture + 128) return RegisterClass::Texture;
  if (binding < 32) return RegisterClass::ConstantBuffer;
  return RegisterClass::Unknown;
}

uint32_t RegisterIndexOf(uint32_t binding)
{
  switch (ClassifyBinding(binding))
  {
  case RegisterClass::ConstantBuffer: return binding - kBindingConstantBuffer;
  case RegisterClass::Texture: return binding - kBindingTexture;
  case RegisterClass::Sampler: return binding - kBindingSampler;
  case RegisterClass::UAV: return binding - kBindingUAV;
  default: return 0;
  }
}

} // namespace

static Reflection ReflectSpirv(const std::vector<uint32_t> &spirv)
{
  SpirvModule m;
  m.parse(spirv);
  Reflection r;

  for (auto &kv : m.variables)
  {
    uint32_t id = kv.first;
    const SpirvModule::Var &v = kv.second;
    if (v.storage == 1 || v.storage == 3 || v.storage == 7) continue; // input/output/function
    if (v.storage == 0 || v.storage == 2 || v.storage == 12)
    {
      auto pit = m.types.find(v.type);
      if (pit == m.types.end() || pit->second.kind != SpirvType::Pointer) continue;
      uint32_t pointee = pit->second.elem;
      auto dit = m.decorations.find(id);
      uint32_t binding = 0;
      if (dit != m.decorations.end())
      {
        auto bit = dit->second.find(kDecorationBinding);
        if (bit != dit->second.end()) binding = bit->second;
      }

      auto tit = m.types.find(pointee);
      if (tit == m.types.end()) continue;

      // Arrays of resources (Texture2D tex[4]) need unwrapping.
      uint32_t arraySize = 1;
      while (tit->second.kind == SpirvType::Array)
      {
        arraySize *= m.arrayLength(tit->second.count);
        pointee = tit->second.elem;
        tit = m.types.find(pointee);
        if (tit == m.types.end()) break;
      }
      if (tit == m.types.end()) continue;

      DescriptorBinding db;
      db.binding = binding;
      db.arraySize = arraySize;
      db.regClass = ClassifyBinding(binding);
      db.regIndex = RegisterIndexOf(binding);
      auto nit = m.names.find(id);
      db.name = (nit != m.names.end()) ? nit->second : "";

      switch (tit->second.kind)
      {
      case SpirvType::Image:
        db.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        break;
      case SpirvType::Sampler:
        db.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        break;
      case SpirvType::SampledImage:
        db.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        break;
      case SpirvType::Struct:
      {
        bool storageBuffer = v.storage == 12;
        db.type = storageBuffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        db.blockSize = m.structSize(pointee);
        if (db.blockSize == 0) db.blockSize = 64;
        break;
      }
      case SpirvType::RuntimeArray:
      case SpirvType::Array:
        db.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        break;
      default:
        db.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        break;
      }
      r.bindings.push_back(db);
    }
  }

  // vertex inputs
  for (auto &kv : m.variables)
  {
    const SpirvModule::Var &v = kv.second;
    if (v.storage != 1) continue;
    auto dit = m.decorations.find(kv.first);
    if (dit == m.decorations.end()) continue;
    auto lit = dit->second.find(kDecorationLocation);
    if (lit == dit->second.end()) continue;

    auto pit = m.types.find(v.type);
    if (pit == m.types.end() || pit->second.kind != SpirvType::Pointer) continue;

    VertexInput in;
    in.location = lit->second;
    uint32_t comps = 1;
    in.format = m.formatOf(pit->second.elem, &comps);
    in.components = comps;
    auto tit = m.types.find(pit->second.elem);
    in.vecSize = (tit != m.types.end() && tit->second.kind == SpirvType::Vector) ? tit->second.count : 1;
    auto nit = m.names.find(kv.first);
    in.name = (nit != m.names.end()) ? nit->second : "";
    r.inputs.push_back(in);
  }

  std::sort(r.inputs.begin(), r.inputs.end(), [](const VertexInput &a, const VertexInput &b) { return a.location < b.location; });
  std::sort(r.bindings.begin(), r.bindings.end(), [](const DescriptorBinding &a, const DescriptorBinding &b) { return a.binding < b.binding; });
  return r;
}

//////////////////////////////////////////////////////////////////////////////
// compile

namespace
{

// Is `name` defined as a function in this source?
//
// The engine compiles the SAME blob once per stage and lets the stages it does
// not use fail - five attempts per technique, of which two or three can never
// succeed.  glslang does not fail cheaply: it preprocesses, parses and links the
// whole shader before reporting "entry point not found", which on a large
// technique costs most of a second.  With 58 such guaranteed failures in a
// 12-frame run, that was ~3.4 s per frame - the player was unusable from the
// point a shadertoy event became active.
//
// "identifier followed by (" is a NECESSARY condition for a definition, so its
// absence proves the entry point does not exist and we can skip shaderc
// entirely.  A false positive only means we compile and fail as before, so this
// can never break a shader that used to work.  The entry names here are single
// letters ("v", "p", "g", "h", "d"), which is why the token boundaries matter.
bool HasEntryPoint(const std::string &src, const char *name)
{
  if (!name || !*name) return true;
  const size_t nameLen = strlen(name);
  auto isIdent = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  for (size_t at = src.find(name); at != std::string::npos; at = src.find(name, at + 1))
  {
    if (at > 0 && isIdent(src[at - 1])) continue;
    const size_t after = at + nameLen;
    if (after < src.size() && isIdent(src[after])) continue;
    size_t p = after;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
    if (p < src.size() && src[p] == '(') return true;
  }
  return false;
}

// Failed compiles are worth remembering: the engine retries the same
// (source, entry, stage) triple on every material rebuild, which happens every
// frame for shadertoy events.
std::mutex g_failedMutex;
std::set<std::string> g_failedKeys;

std::string FailureKey(const std::string &src, const char *entry, int stage)
{
  const size_t h = std::hash<std::string>{}(src);
  return std::to_string(h) + "|" + (entry ? entry : "") + "|" + std::to_string(stage);
}

// ---------------------------------------------------------------------------
// Geometry shader description (see GeometryShaderInfo).
//
// The engine writes one entry point per stage into the same HLSL blob, e.g.
//
//   [maxvertexcount(6)]
//   void g( line VSOUT input[2], inout TriangleStream<GSOUT> OutputStream )
//   { ... OutputStream.Append(o); ... OutputStream.RestartStrip(); ... }
//
// Everything an emulation needs is in that signature and in the body's call
// sites, so read it here instead of rediscovering it later.  IsIdentChar()
// earlier in this unnamed namespace is reused as-is.
// ---------------------------------------------------------------------------

// The text between the parentheses that follow the entry point's identifier.
bool FindEntryParameters(const std::string &src, const char *entry, std::string &out)
{
  if (!entry || !*entry) return false;
  const size_t nameLen = strlen(entry);
  for (size_t at = src.find(entry); at != std::string::npos; at = src.find(entry, at + 1))
  {
    if (at > 0 && IsIdentChar(src[at - 1])) continue;
    const size_t after = at + nameLen;
    if (after < src.size() && IsIdentChar(src[after])) continue;
    size_t p = after;
    while (p < src.size() && isspace((unsigned char)src[p])) p++;
    if (p >= src.size() || src[p] != '(') continue;
    int depth = 0;
    const size_t begin = p;
    for (; p < src.size(); p++)
    {
      if (src[p] == '(') depth++;
      else if (src[p] == ')' && --depth == 0)
      {
        out = src.substr(begin + 1, p - begin - 1);
        return true;
      }
    }
    return false;
  }
  return false;
}

int CountCalls(const std::string &body, const char *name)
{
  int n = 0;
  const size_t len = strlen(name);
  for (size_t at = body.find(name); at != std::string::npos; at = body.find(name, at + len))
  {
    size_t p = at + len;
    while (p < body.size() && isspace((unsigned char)body[p])) p++;
    if (p < body.size() && body[p] == '(') n++;
  }
  return n;
}

} // namespace

static bool IsPrimitiveWord(const std::string &w)
{
  return w == "point" || w == "line" || w == "triangle" || w == "lineadj" || w == "triangleadj";
}

static int PrimitiveVertexCount(const std::string &w)
{
  if (w == "point") return 1;
  if (w == "line") return 2;
  if (w == "triangle") return 3;
  if (w == "lineadj") return 4;
  if (w == "triangleadj") return 6;
  return 0;
}

static std::string Trim(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

GeometryShaderInfo DescribeGeometryShader(const char *source, size_t length, const char *entryPoint)
{
  GeometryShaderInfo info;
  const std::string src(source, length);
  if (src.find("maxvertexcount") == std::string::npos) return info;
  info.present = true;

  {
    const size_t at = src.find("maxvertexcount");
    const size_t open = src.find('(', at);
    if (open != std::string::npos) info.maxVertexCount = atoi(src.c_str() + open + 1);
  }

  std::string params;
  if (FindEntryParameters(src, entryPoint, params))
  {
    const size_t inout = params.find("inout");
    const std::string in = (inout == std::string::npos) ? params : params.substr(0, inout);

    // The input type and its name, then the primitive modifier if there is
    // one: HLSL lets it be left out, in which case the input is a triangle.
    // Read as "type name" first, so `VSOUT i[3]` and `triangle VSOUT i[3]`
    // both work; the modifier, when present, comes first and pushes the pair
    // along by one word.
    std::vector<std::string> words;
    for (size_t p = 0; p < in.size();)
    {
      while (p < in.size() && !IsIdentChar(in[p])) p++;
      const size_t start = p;
      while (p < in.size() && IsIdentChar(in[p])) p++;
      if (p > start) words.push_back(in.substr(start, p - start));
    }
    size_t word = 0;
    if (word < words.size() && IsPrimitiveWord(words[word]))
    {
      info.inputPrimitive = words[word++];
      info.inputVertices = PrimitiveVertexCount(info.inputPrimitive);
    }
    else
    {
      // No modifier: the default, and the only one HLSL defaults to.
      info.inputPrimitive = "triangle";
      info.inputVertices = 3;
    }
    if (word < words.size()) info.inputType = words[word];

    if (inout != std::string::npos)
    {
      const size_t lt = params.find('<', inout);
      const size_t gt = (lt == std::string::npos) ? std::string::npos : params.find('>', lt);
      if (lt != std::string::npos && gt != std::string::npos)
      {
        // `TriangleStream < VSOUT > o` is legal HLSL: trim what is inside.
        info.outputType = Trim(params.substr(lt + 1, gt - lt - 1));
        size_t q = inout + 5;
        while (q < lt && !IsIdentChar(params[q])) q++;
        const size_t s2 = q;
        while (q < lt && IsIdentChar(params[q])) q++;
        info.outputStream = params.substr(s2, q - s2);
      }
    }
  }

  const size_t brace = src.find('{', src.find("maxvertexcount"));
  const std::string body = (brace == std::string::npos) ? src : src.substr(brace);
  info.appends = CountCalls(body, "Append");
  info.restarts = CountCalls(body, "RestartStrip");
  return info;
}

// A separate function, not a mode of CompileHLSL: this returns text, and the
// callers that want it (the geometry-shader analysis) must not pay for a SPIR-V
// compile they do not use.
std::string PreprocessHLSL(const std::string &src, ShaderStage stage, const char *entryPoint)
{
  shaderc_compiler_t compiler = shaderc_compiler_initialize();
  if (!compiler) return std::string();
  shaderc_compile_options_t options = shaderc_compile_options_initialize();
  shaderc_compile_options_set_source_language(options, shaderc_source_language_hlsl);
  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  shaderc_compile_options_set_auto_map_locations(options, true);
  shaderc_compile_options_set_auto_bind_uniforms(options, false);
  shaderc_compile_options_set_hlsl_io_mapping(options, true);
  shaderc_compile_options_set_hlsl_offsets(options, true);

  shaderc_compilation_result_t res = shaderc_compile_into_preprocessed_text(
      compiler, src.c_str(), src.size(), ShadercKind(stage), "shader.hlsl", entryPoint ? entryPoint : "main", options);
  std::string out;
  if (res && shaderc_result_get_compilation_status(res) == shaderc_compilation_status_success)
  {
    const char *text = shaderc_result_get_bytes(res);
    const size_t len = shaderc_result_get_length(res);
    if (text && len) out.assign(text, len);
  }
  if (res) shaderc_result_release(res);
  shaderc_compile_options_release(options);
  shaderc_compiler_release(compiler);
  return out;
}

CompiledShader CompileHLSL(const char *source, size_t length, const char *entryPoint, ShaderStage stage, const char *debugName)
{
  CompiledShader result;
  std::string src(source, length);

  // Cheap rejection for the stages a technique does not implement, then a cache
  // of previous failures, so neither costs a glslang run.  See HasEntryPoint().
  if (!HasEntryPoint(src, entryPoint))
  {
    result.error = std::string("entry point '") + (entryPoint ? entryPoint : "") +
                   "' is not defined in this shader source";
    result.ok = false;
    return result;
  }

  {
    const std::string key = FailureKey(src, entryPoint, (int)stage);
    std::lock_guard<std::mutex> lock(g_failedMutex);
    auto it = g_failedKeys.find(key);
    if (it != g_failedKeys.end())
    {
      result.error = "this (shader, entry point, stage) combination failed to compile before";
      result.ok = false;
      return result;
    }
  }

  std::string rewritten = RewriteHLSLForVulkan(src);
  result.rewrittenSource = rewritten;

  shaderc_compiler_t compiler = shaderc_compiler_initialize();
  shaderc_compile_options_t options = shaderc_compile_options_initialize();
  shaderc_compile_options_set_source_language(options, shaderc_source_language_hlsl);
  shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_3);
  // Keep names and decorations intact: our reflection relies on them and the
  // engine compiles shaders at load time only.
  shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
  // No SPIR-V debug info: it is only OpSource/OpLine, nothing reads it, and it
  // is by far the biggest part of the module for a long HLSL shader.  Names and
  // decorations are unaffected -- reflection depends on those, not on this.
  shaderc_compile_options_set_auto_map_locations(options, true);
  shaderc_compile_options_set_auto_bind_uniforms(options, false);
  shaderc_compile_options_set_hlsl_io_mapping(options, true);
  shaderc_compile_options_set_hlsl_offsets(options, true);

  shaderc_compilation_result_t res = shaderc_compile_into_spv(compiler, rewritten.c_str(), rewritten.size(), ShadercKind(stage), debugName ? debugName : "shader.hlsl", entryPoint, options);

  shaderc_compilation_status status = shaderc_result_get_compilation_status(res);
  if (status != shaderc_compilation_status_success)
  {
    const char *err = shaderc_result_get_error_message(res);
    result.error = err ? err : "unknown shader compilation error";
    result.ok = false;
    {
      std::lock_guard<std::mutex> lock(g_failedMutex);
      g_failedKeys.insert(FailureKey(src, entryPoint, (int)stage));
    }
  }
  else
  {
    size_t bytes = shaderc_result_get_length(res);
    const char *data = shaderc_result_get_bytes(res);
    result.spirv.resize(bytes / 4);
    memcpy(result.spirv.data(), data, bytes);
    result.reflection = ReflectSpirv(result.spirv);
    if (stage == ShaderStage::Geometry) result.geometry = DescribeGeometryShader(source, length, entryPoint);
    result.ok = true;
  }

  shaderc_result_release(res);
  shaderc_compile_options_release(options);
  shaderc_compiler_release(compiler);
  return result;
}

} // namespace phxvk
