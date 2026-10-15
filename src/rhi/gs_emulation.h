// src/rhi/gs_emulation.h
//
// Geometry shader emulation for devices that have no geometry shader stage.
//
// Metal does not, so MoltenVK does not, so the D3D11 shim drops the stage
// (d3d11_context.cpp).  That is not cosmetic: 5 of Clean Slate's 34 render
// techniques carry a geometry shader and they are the main ones -- the
// background grid, particles, point sprites, two-sided triangles.  Everything
// the dropped stage would have built is simply absent from the frame.
//
// The emulation works because every one of those shaders has the same shape: a
// fixed-size expansion of ONE input primitive into a small triangle stream,
// written as straight-line `OutputStream.Append(...)` calls.  So:
//
//   * the input primitive's K vertices are packed into one "fat" vertex, and N
//     such vertices are emitted per primitive;
//   * the geometry shader body is translated into a vertex shader that runs the
//     original vertex shader on all K vertices, then walks the body and returns
//     at the append whose running index equals `SV_VertexID % N`;
//   * the emitted vertices are drawn as a triangle strip, with a primitive
//     restart wherever the body called RestartStrip, so the fixed-function
//     assembler rebuilds the same strips the geometry shader would have
//     streamed.
//
// Anything that does not fit that shape is refused (ok == false) and the caller
// keeps the current behaviour, rather than being handed a wrong picture.

#pragma once

#include "shader_compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phxvk
{

// The index value that restarts a strip, as Vulkan spells it.  The emulated
// index buffers are 32-bit.
constexpr uint32_t kPrimitiveRestartIndex = 0xFFFFFFFFu;

struct GsProgram
{
  bool ok = false;
  std::string reason;      // why not, when !ok
  int slots = 0;           // vertices emitted per input primitive (maxvertexcount)
  int inputVertices = 0;   // K: vertices the input primitive carries
  std::string inputType;   // the element type the vertex shader produced
  std::string outputStream; // "TriangleStream", "LineStream" or "PointStream"
  // Whether emitted vertices are joined into strips, so that input primitives
  // and RestartStrips have to be separated by a restart index.  A PointStream
  // emits independent points and needs no separators at all.
  bool strips = true;
  std::vector<int> restartsAfter; // slot index after which the strip restarts
};

// Reads the body of `gsEntry` and checks it against the emulatable shape.
// `shape` comes from DescribeGeometryShader().
GsProgram AnalyseGeometryShader(const std::string &source, const char *gsEntry, const char *vertexEntry, const GeometryShaderInfo &shape);

// Appends a vertex entry point that reproduces the geometry shader, to be
// compiled as the vertex stage in place of the original vertex entry point.
// Returns an empty string when the shader cannot be translated.
std::string TranslateGeometryShaderToVertex(const std::string &source, const GsProgram &gs, const char *gsEntry, const char *vertexEntry, const char *packedInputName);

// K input vertices per emitted vertex, `slots` emitted vertices per primitive.
struct FatGeometry
{
  std::vector<uint8_t> vertices;   // (primitives * slots) * (K * stride) bytes
  std::vector<uint32_t> indices;   // triangle strip with kPrimitiveRestartIndex
  uint32_t vertexCount = 0;        // vertices.size() / (K * stride)
};

// Builds the fat geometry for one draw.  `indexCount` indices are read from
// `indices` starting at `firstIndex`, or, when `indices` is null, the vertices
// are taken sequentially (D3D11's non-indexed Draw, which is how the engine
// draws particles).  `stride` is the original vertex stride and `vb`/`vbSize`
// the buffer the indices point into.  Only 16/32-bit indices and
// triangle/line/point lists are accepted.
bool BuildFatGeometry(const GsProgram &gs, const void *vb, size_t vbSize, size_t stride, bool indicesAre32Bit,
                      const void *indices, size_t indexCount, size_t firstIndex, FatGeometry &out, std::string *error);

} // namespace phxvk
