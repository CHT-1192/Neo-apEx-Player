// src/rhi/shader_interface.h
//
// D3D11 links a draw's shaders by SIGNATURE.  The pixel shader's input
// signature is matched against the vertex shader's output signature by
// semantic name and index, and a component the vertex shader never wrote
// simply reads as 0 (and .w as 1).  A pixel shader declaring
// `float4 uv : TEXCOORD0` is therefore perfectly happy with a vertex shader
// that wrote `float2 uv : TEXCOORD0`.
//
// Vulkan links by LOCATION and requires both sides to have the *same* type.
// SPIRV-Cross refuses the mismatch outright, so vkCreateGraphicsPipelines
// returns VK_ERROR_INITIALIZATION_FAILED and the draw disappears:
//
//   Fragment input(s) `user(locn0)` mismatching vertex shader output type(s)
//   or not written by vertex shader.
//
// That is not hypothetical: the engine draws every generated texture through
// its own quad vertex shader (phxEngine.cpp:149), which emits
// `float2 texcoord0`, while a project's texture-generator pixel shader is
// free to declare `float4 texcoord0`.  10 of the 11 bundled projects do
// exactly that, and each one loses a generated texture to a pipeline that
// could not be created.
//
// This file lowers the D3D11 rule into Vulkan: every floating point or
// integer varying a stage declares is widened to FOUR components, with the
// ones the shader did not write filled in as D3D11 would read them.  Four
// components on both sides always match, which is also what a D3D11
// signature register is -- four components plus a write mask, and Vulkan has
// no mask.
//
// The rewrite is per stage, so it does not depend on which vertex and pixel
// shader end up in the same pipeline; it is applied once, at shader creation,
// and is a no-op for a module whose varyings are all four wide already.

#pragma once

#include "shader_compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phxvk
{

// One numbered varying of a module's entry point.
struct StageVarying
{
  uint32_t variable = 0;      // the OpVariable's result id
  uint32_t location = 0;      // its Location decoration
  uint32_t componentType = 0; // OpTypeFloat / OpTypeInt that the components are
  uint32_t components = 0;    // how many the shader declared (1..4)
};

// Reads the numbered varyings the entry point declares -- its Outputs for a
// vertex shader, its Inputs for a fragment shader.  Built-ins (SV_Position and
// friends) carry a BuiltIn decoration instead of a Location and are not
// reported.  Varyings of a shape that cannot be rewritten (a matrix, an array,
// a struct) are reported as well, with components == 0, so a caller can say
// what it could not fix.
bool ReadStageVaryings(const uint32_t *words, size_t wordCount, ShaderStage stage,
                       std::vector<StageVarying> *out, std::string *why);

enum class NormaliseResult
{
  Unchanged, // every varying is four wide already; `out` is the input
  Rewritten, // `out` holds the module to use instead
  Refused,   // `out` is the input; at least one varying could not be rewritten
};

// Widens every varying of `spirv` to four components.  Returns Unchanged when
// there is nothing to do, and Refused (leaving `out` as a copy of the input)
// when a varying is referenced in a way this rewrite cannot follow -- the
// caller then keeps today's behaviour for that module rather than being handed
// a broken one.
// `componentBudget` is how many varying components the stage may declare
// afterwards (maxVertexOutputComponents for a vertex stage,
// maxFragmentInputComponents for a pixel stage); 0 means no limit.  Widening
// every varying to four components is what makes any pair link, but it is also
// four times the components -- a shader with 32 float varyings would need 128,
// which is at or past what the device has, and the pipeline would fail to
// compile where it used to work.  When the widening does not fit, the module is
// left exactly as it was: a shader that many varyings is far more likely to
// already agree on both sides than to need reconciling, and one that does need
// it could not be run on this device anyway.
NormaliseResult NormaliseVaryings(const std::vector<uint32_t> &spirv, ShaderStage stage,
                                  std::vector<uint32_t> *out, std::string *why,
                                  uint32_t componentBudget = 0);

} // namespace phxvk
