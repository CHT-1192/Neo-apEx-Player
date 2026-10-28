// Runtime HLSL -> SPIR-V translation for the apEx player.
//
// The engine hands us D3D11-style HLSL (Texture2D/SamplerState/cbuffer/
// register(tN) and D3D9-era `register(cN)` globals).  We compile it with
// libshaderc (glslang's HLSL front end) instead of fxc, but first rewrite every
// resource declaration to carry an explicit `[[vk::binding(...)]]` so that the
// D3D11 register model maps onto a deterministic Vulkan descriptor layout:
//
//      cbuffer bN      -> binding   0 + N      (N < 32)
//      Texture2D tN    -> binding  64 + N      (N < 128)
//      SamplerState sN -> binding 192 + N      (N < 16)
//      RW... uN        -> binding 208 + N      (N < 16)
//
// `register(cN)` globals need no rewrite: glslang puts them in the implicit
// "$Globals" constant buffer at binding 0, which is exactly D3D11's b0 slot.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace phxvk
{

enum class ShaderStage
{
  Vertex = 0,
  Pixel,
  Geometry,
  Hull,
  Domain,
  Compute,
};

// Descriptor ranges (see the table above).
constexpr uint32_t kBindingConstantBuffer = 0;
constexpr uint32_t kBindingTexture = 64;
constexpr uint32_t kBindingSampler = 192;
constexpr uint32_t kBindingUAV = 208;

enum class RegisterClass
{
  ConstantBuffer = 0,
  Texture,
  Sampler,
  UAV,
  Unknown,
};

struct DescriptorBinding
{
  uint32_t binding = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  RegisterClass regClass = RegisterClass::Unknown;
  uint32_t regIndex = 0;
  uint32_t arraySize = 1;
  uint32_t blockSize = 0; // for uniform/storage buffers
  std::string name;
};

struct VertexInput
{
  uint32_t location = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t components = 0;
  uint32_t vecSize = 0; // 1..4
  std::string name;
};

struct Reflection
{
  std::vector<DescriptorBinding> bindings;
  std::vector<VertexInput> inputs;
};

// What a geometry shader consumes and emits, read out of the HLSL by
// DescribeGeometryShader().  Metal has no geometry shader stage, so the shim
// drops it and every primitive that shader would have built is absent; to put
// them back the emulator has to know the shape of the shader it is replacing.
struct GeometryShaderInfo
{
  bool present = false;
  std::string inputPrimitive; // "point", "line", "triangle", "lineadj", "triangleadj"
  int inputVertices = 0;      // vertices the input primitive carries (1, 2, 3, ...)
  int maxVertexCount = 0;     // the [maxvertexcount(N)] attribute
  std::string inputType;      // the element type the vertex shader produced
  std::string outputStream;   // "TriangleStream", "PointStream", "LineStream"
  std::string outputType;     // the stream's element type
  int appends = 0;            // OutputStream.Append(...) call sites
  int restarts = 0;           // OutputStream.RestartStrip() call sites
};

struct CompiledShader
{
  bool ok = false;
  std::vector<uint32_t> spirv;
  Reflection reflection;
  GeometryShaderInfo geometry;
  std::string error;
  std::string rewrittenSource; // post-preprocessing, useful for debugging
};

// Reads a geometry shader's shape out of its HLSL source.  Returns
// present == false when the source has no [maxvertexcount] attribute.
GeometryShaderInfo DescribeGeometryShader(const char *source, size_t length, const char *entryPoint);

// Compiles one entry point.  `debugName` is only used in diagnostics.
CompiledShader CompileHLSL(const char *source, size_t length, const char *entryPoint, ShaderStage stage, const char *debugName);

// Exposed for testing / tooling.
std::string RewriteHLSLForVulkan(const std::string &hlsl);

// The source after the HLSL preprocessor has run: macros expanded, includes
// inlined.  Empty when it fails.  The geometry-shader emulator needs this
// because it decides from the *text* whether a shader appends anything, and an
// Append written through a #define is invisible in the unexpanded source.
std::string PreprocessHLSL(const std::string &src, ShaderStage stage, const char *entryPoint);

const char *ShaderStageName(ShaderStage stage);

} // namespace phxvk
