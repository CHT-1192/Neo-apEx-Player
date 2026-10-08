// tests/gs_emulation_test.cpp
//
// Validation for src/rhi/gs_emulation.{h,cpp}: the geometry shader emulation
// that stands in for the stage Metal cannot run.
//
// The shader below is copied VERBATIM from the Clean Slate project -- the
// <Code> of the rendertechnique named "Wireframe", which is the material behind
// the background grid (++scene / titles -> background/background wire).  That
// is deliberate: the point of this test is the real shader, not a synthetic
// one that happens to suit the parser.  Its geometry shader turns each line
// into a screen-space quad, and without it the grid collapses to one-pixel
// hairlines.
//
// Build/run:
//   cmake --build build --target phx_gs_test && ./build/phx_gs_test
//
// Exit status is the number of failed checks (0 == pass).

#include "rhi/gs_emulation.h"
#include "rhi/shader_compiler.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char msg[512];
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  printf("%s %s\n", ok ? "  ok  " : "  FAIL", msg);
  if (!ok) g_failures++;
}

static const char *kWireframe = R"GSSRC(
SamplerState sm:register(s0);
Texture2D Textur:register(t0);

cbuffer b : register(b0)
{
	float4x4 viewmat;
	float4x4 projmat;
}

cbuffer c : register(b1)
{
	float4x4 worldmat;
	float4x4 itworldmat;
	float4 color;
	float4 width;
	float2 t;
}

struct VSIN
{
	float3 Position : POSITION0;
	float3 Position2 : POSITION1;
	float3 Normal : NORMAL0;
	float4 Color : COLOR0;
	float4 UV : TEXCOORD0;
	float4 UV2: TEXCOORD1;
};

struct VSOUT
{
	float4 Position : SV_POSITION;
	float4 Normal : TEXCOORD0;
	float anim: TEXCOORD1;
};

VSOUT v(VSIN x)
{
	VSOUT o;
	o.Position=float4(mul(worldmat,float4(x.Position,1)).xyz,1);
	o.Normal=float4(normalize(mul(worldmat,float4(x.Normal,1)).xyz),1);
	o.anim=Textur.SampleLevel(sm,x.UV.xy,0).x;
	return o;
}

struct GSOUT
{
	float4 Position : SV_POSITION;
	float t : TEXCOORD0;
	float c : TEXCOORD1;
};

[maxvertexcount(6)]
void g( line VSOUT input[2], inout TriangleStream<GSOUT> OutputStream )
{
	float tc=saturate(saturate(t.x-(input[0].anim+input[1].anim)*0.95*0.5)/0.05);

	float4 p1=mul(projmat,mul(viewmat,input[0].Position+input[0].Normal*t.y));
	float4 p2=mul(projmat,mul(viewmat,input[1].Position+input[1].Normal*t.y));
	
	p2=lerp(p1,p2,tc);
	
	float2 d = normalize((p2/p2.w).xy-(p1/p1.w).xy);
	float4 n = float4(-d.y,d.x,0,0)*width.x*0.05;
	
	GSOUT o;
	o.c=tc;//input[0].anim;
	
	o.Position=p1+n*p1.w;
	o.t=1;
	OutputStream.Append(o);

	o.Position=p1-n*p1.w;
	o.t=-1;
	OutputStream.Append(o);

	o.Position=p2-n*p2.w;
	OutputStream.Append(o);
	
	OutputStream.RestartStrip();	

	o.Position=p1+n*p1.w;
	o.t=1;
	OutputStream.Append(o);

	o.Position=p2-n*p2.w;
	o.t=-1;
	OutputStream.Append(o);

	o.Position=p2+n*p2.w;
	o.t=1;
	OutputStream.Append(o);
	
	OutputStream.RestartStrip();	
}

float4 p(GSOUT v) : SV_TARGET0
{
	float r=pow(abs(1-abs(v.t)),1+width.y*30);
	if (r<=1/255.0f)
		discard;
	return r*color*v.c*(1+width.z*10);;
}
)GSSRC";

int main()
{
  printf("gs_emulation: the Wireframe material from Clean Slate\n");

  // --- the shape reader (src/rhi/shader_compiler.cpp) ----------------------
  const phxvk::GeometryShaderInfo shape = phxvk::DescribeGeometryShader(kWireframe, strlen(kWireframe), "g");
  Check(shape.present, "the source is recognised as a geometry shader");
  Check(shape.inputPrimitive == "line", "input primitive is 'line' (got '%s')", shape.inputPrimitive.c_str());
  Check(shape.inputVertices == 2, "a line carries 2 vertices (got %d)", shape.inputVertices);
  Check(shape.maxVertexCount == 6, "maxvertexcount is 6 (got %d)", shape.maxVertexCount);
  Check(shape.outputStream == "TriangleStream", "output stream is a TriangleStream (got '%s')", shape.outputStream.c_str());
  Check(shape.appends == 6, "6 Append call sites (got %d)", shape.appends);
  Check(shape.restarts == 2, "2 RestartStrip call sites (got %d)", shape.restarts);

  // --- the analysis --------------------------------------------------------
  const phxvk::GsProgram gs = phxvk::AnalyseGeometryShader(kWireframe, "g", "v", shape);
  Check(gs.ok, "the shader is accepted for emulation%s%s", gs.ok ? "" : ": ", gs.ok ? "" : gs.reason.c_str());
  Check(gs.slots == 6, "6 emitted vertices per line (got %d)", gs.slots);
  Check(gs.inputVertices == 2, "the emulator packs 2 vertices per emitted vertex (got %d)", gs.inputVertices);
  Check(gs.restartsAfter.size() == 2 && gs.restartsAfter[0] == 3 && gs.restartsAfter[1] == 6,
        "the two RestartStrips split the stream after slot 3 and 6 (got %zu: %d,%d)", gs.restartsAfter.size(),
        gs.restartsAfter.empty() ? -1 : gs.restartsAfter[0], gs.restartsAfter.size() < 2 ? -1 : gs.restartsAfter[1]);

  // --- the translation -----------------------------------------------------
  const std::string vs = phxvk::TranslateGeometryShaderToVertex(kWireframe, gs, "g", "v", "GSEMU_PACKED");
  Check(!vs.empty(), "a vertex shader was produced");
  Check(vs.find("GSEMU_PACKED") != std::string::npos, "the packed input struct is declared");
  Check(vs.find("input[0] = v( gsemu_packed.a0 )") != std::string::npos, "the original vertex shader runs on endpoint 0");
  Check(vs.find("input[1] = v( gsemu_packed.a1 )") != std::string::npos, "the original vertex shader runs on endpoint 1");
  Check(vs.find("gsemu_vid % 6u") != std::string::npos, "the emitted vertex is selected with SV_VertexID %% slots");
  Check(vs.find("gsemu_slot == gsemu_at") != std::string::npos, "each Append is turned into a slot test");
  Check(vs.find("RestartStrip") == std::string::npos, "RestartStrip is gone (the index buffer carries it)");
  Check(vs.find("float4( 0, 0, 0, 0 )") != std::string::npos, "unused slots collapse to a degenerate vertex");

  // --- the fat geometry ----------------------------------------------------
  // Two lines: vertices 0..3, each 24 bytes of three floats.
  float verts[4][6] = {};
  for (int i = 0; i < 4; i++) { verts[i][0] = (float)i; verts[i][3] = 0.0f; verts[i][4] = 1.0f; verts[i][5] = 0.0f; }
  const uint32_t idx[4] = { 0, 1, 2, 3 };

  phxvk::FatGeometry fat;
  std::string err;
  const bool built = phxvk::BuildFatGeometry(gs, verts, sizeof verts, sizeof verts[0], true, idx, 4, 0, fat, &err);
  Check(built, "the fat geometry was built%s%s", built ? "" : ": ", built ? "" : err.c_str());
  Check(fat.vertexCount == 12, "2 lines * 6 slots = 12 emitted vertices (got %u)", fat.vertexCount);
  Check(fat.vertices.size() == 12 * 2 * sizeof(verts[0]), "each emitted vertex carries both endpoints (%zu bytes)",
        fat.vertices.size());

  // Strip: 0,1,2 restart 3,4,5 restart | 6,7,8 restart 9,10,11 restart
  const uint32_t expected[16] = { 0, 1, 2, phxvk::kPrimitiveRestartIndex, 3, 4, 5, phxvk::kPrimitiveRestartIndex,
                                  6, 7, 8, phxvk::kPrimitiveRestartIndex, 9, 10, 11, phxvk::kPrimitiveRestartIndex };
  Check(fat.indices.size() == 16, "the strip has 16 indices (got %zu)", fat.indices.size());
  Check(fat.indices == std::vector<uint32_t>(expected, expected + 16), "the strip restarts exactly where the geometry shader did");

  // The first emitted vertex must contain endpoint 0 then endpoint 1.
  float first[12];
  memcpy(first, fat.vertices.data(), sizeof first);
  Check(first[0] == 0.0f && first[6] == 1.0f, "emitted vertex 0 packs endpoints 0 and 1 (got %.1f and %.1f)", first[0], first[6]);

  printf("\n=============================================\n");
  if (g_failures) { printf("%d CHECK(S) FAILED\n", g_failures); return g_failures; }
  printf("ALL CHECKS PASSED\n");
  return 0;
}
