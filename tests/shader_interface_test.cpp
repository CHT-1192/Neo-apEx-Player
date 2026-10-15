// tests/shader_interface_test.cpp
//
// Validation for src/rhi/shader_interface.{h,cpp}: the rewriter that lowers
// D3D11's signature-based shader linkage onto Vulkan's location-based one.
//
// The two shaders below are the pair that exposed the defect, copied verbatim
// from the game rather than invented:
//
//   * the vertex shader is the engine's own 2D quad shader (`shader[]` in
//     Phoenix/phxEngine.cpp, compiled for entry point "u"), which every
//     generated texture and the loading screen are drawn with.  It writes
//     TEXCOORD0 as a float2.
//
//   * the pixel shader is the texture filter `float4 p(float4 t:texcoord0)` out
//     of the loading-screen project embedded as precalcphoenix_shader.64k, the
//     one that draws the apEx logo.  It reads TEXCOORD0 as a float4.
//
// D3D11 links those two happily; Vulkan refuses the pipeline outright, and
// every one of the bundled projects except Clean Slate lost a generated texture
// to it (10 of 11 -- see src/rhi/shader_interface.h).
//
// Build/run:
//   cmake --build build --target phx_interface_test && ./build/phx_interface_test
//
// Exit status is the number of failed checks (0 == pass).

#include "rhi/shader_interface.h"
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

// Phoenix/phxEngine.cpp:149, the engine's shared 2D vertex shader.
static const char *kEngineQuadShader = R"HLSL(
SamplerState o:register(s1);
Texture2D r:register(t0);
struct l{float4 o:positiont;float2 r:texcoord0;};
struct i{float4 o:position,r:texcoord0;};
struct n{float3 o:position0,r:position1,u:normal0;float4 c:color0,s:texcoord0,a:texcoord1;};
struct t{float2 o:texcoord0;float4 r:sv_position;};
t u(l c){t s;s.r=c.o;s.o=c.r;return s;}
t c(n s){t a;a.r=0;a.o=0;return a;}
t s(i a){t e;e.r=0;e.o=0;return e;}
float4 a(t e):sv_target0{return r.Sample(o,e.o);}
)HLSL";

// The loading-screen project's logo filter, from precalcphoenix_shader.64k.
// Only the tail matters here: the pixel shader reads TEXCOORD0 as a float4.
static const char *kLogoFilterShader = R"HLSL(
float o(float2 e,float2 y,float2 a,float2 t){float2 l=a-e,x=y-e,f=t-e;float u=dot(l,l),r=dot(l,x),s=dot(l,f),n=dot(x,x),b=dot(x,f),w=1./(u*n-r*r),S=(n*s-r*b)*w,i=(u*b-r*s)*w;float2 c=1+i*float2(-.5,-1)-S;if(!(S>=0&&i>=0&&S+i<1.))return 0;return saturate(-(c.x*c.x-c.y)*70);}
float e(float2 y,float2 a,float2 t){return y.x<=t.x&&a.x>t.x&&y.y<=t.y&&a.y>t.y;}
float y(float2 a){a+=float2(.008,0);float t=0;t+=e(float2(.375,.21),float2(.45,.7),a);t+=o(float2(.32,.25),float2(.18,.5),float2(.344,.65),a);t-=o(float2(.32,.25),float2(.28,.41),float2(.367,.533),a);t+=e(float2(.33,.47),float2(.38,.7),a);t=saturate(t);t+=o(float2(.38,0),float2(.555,.15),float2(.45,.5),a);t-=o(float2(.38,0),float2(.46,.16),float2(.365,.325),a);t=saturate(t);t+=o(float2(.38,.533),float2(.32,.44),float2(.38,.29),a);t+=o(float2(.218,.51),float2(.24,.76),float2(.45,.745),a);t=saturate(t);t-=o(float2(.22,.50),float2(.235,.61),float2(.344,.65),a);return saturate(t);}
float h(float2 a){float t=0;t+=o(float2(.408,.748),float2(.325,.91),float2(.53,1),a);t-=o(float2(.47,.87),float2(.47,.96),float2(.53,1),a);t=saturate(t);t+=o(float2(.456,.845),float2(.45,.97),float2(.61,.955),a);t-=o(float2(.53,.89),float2(.54,.93),float2(.61,.955),a);t=saturate(t);t+=o(float2(.535,.9),float2(.62,.86),float2(.575,.77),a);t+=o(float2(.408,.75),float2(.53,1.07),float2(.575,.77),a);t-=o(float2(.49,.76),float2(.425,.81),float2(.48,.848),a);t-=o(float2(.464,.835),float2(.505,.878),float2(.553,.83),a);t=saturate(t);t-=o(float2(.51,.7),float2(.4,.97),float2(.58,.73),a);t-=o(float2(.48,.84),float2(.577,.870),float2(.58,.74),a);t+=o(float2(.4,.75),float2(.46,.748),float2(.485,.76),a);return saturate(t);}
float a(float2 t){float l=0;l+=y(t);l+=y(float2(1-t.x,t.y));l+=o(float2(.503,.641),float2(.81,.64),float2(.56,.742),t);l=saturate(l);l-=o(float2(.503,.642),float2(.554,.69),float2(.56,.742),t);l+=o(float2(.485,.629),float2(.625,.78),float2(.485,.825),t);l+=e(float2(.41,.63),float2(.486,.755),t);l+=e(float2(.37,.67),float2(.42,.72),t);l=saturate(l);l+=o(float2(.555,.75),float2(.557,.845),float2(.485,.825),t);l-=o(float2(.38,.742),float2(.575,.737),float2(.485,.825),t)*1.5;l=saturate(l);l-=o(float2(.49,.755),float2(.542,.787),float2(.48,.832),t);l=saturate(l);l+=h(t);return l;}
float4 p(float4 t:texcoord0):sv_target0{return a(t.xy)+.01;}
)HLSL";

// The same quad shader, but writing TEXCOORD0 as a float4, for the mirrored
// case where the pixel shader is the narrow side.
static const char *kWideQuadShader = R"HLSL(
struct l{float4 o:positiont;float2 r:texcoord0;};
struct t{float4 o:texcoord0;float4 r:sv_position;};
t u(l c){t s;s.r=c.o;s.o=float4(c.r,0,1);return s;}
)HLSL";

// A pixel shader that reads TEXCOORD0 as a float2.
static const char *kNarrowPs = R"HLSL(
float4 p(float2 t:texcoord0):sv_target0{return float4(t,0,1);}
)HLSL";

// A pixel shader that already reads TEXCOORD0 as a float4, so that a pair which
// needs no rewriting can be checked to come back untouched.
static const char *kAlreadyWide = R"HLSL(
float4 p(float4 t:texcoord0):sv_target0{return t;}
)HLSL";

// Reports the component count a shader declares at `location`, and the total
// number of varyings it declares.
static bool VaryingAt(const std::vector<uint32_t> &spirv, phxvk::ShaderStage stage, uint32_t location,
                      uint32_t *components, size_t *count, std::string *why)
{
  std::vector<phxvk::StageVarying> varyings;
  if (!phxvk::ReadStageVaryings(spirv.data(), spirv.size(), stage, &varyings, why)) return false;
  *count = varyings.size();
  for (const phxvk::StageVarying &v : varyings)
    if (v.location == location) { *components = v.components; return true; }
  return false;
}

// Counts the occurrences of an opcode in a module, for checking what the
// rewrite emitted.
static int CountOpcode(const std::vector<uint32_t> &spirv, uint16_t opcode)
{
  int n = 0;
  for (size_t i = 5; i < spirv.size();)
  {
    const uint32_t w = spirv[i];
    const uint32_t len = w >> 16;
    if (!len || i + len > spirv.size()) break;
    if ((w & 0xffff) == opcode) n++;
    i += len;
  }
  return n;
}

int main()
{
  printf("shader interface normalisation\n");

  // ---- the real pair -----------------------------------------------------
  phxvk::CompiledShader vs = phxvk::CompileHLSL(kEngineQuadShader, strlen(kEngineQuadShader), "u",
                                                phxvk::ShaderStage::Vertex, "engine-quad/vs_5_0");
  phxvk::CompiledShader ps = phxvk::CompileHLSL(kLogoFilterShader, strlen(kLogoFilterShader), "p",
                                                phxvk::ShaderStage::Pixel, "logo-filter/ps_5_0");
  Check(vs.ok, "the engine's quad vertex shader compiles");
  Check(ps.ok, "the logo filter pixel shader compiles");
  if (!vs.ok || !ps.ok)
  {
    printf("%s\n%s\n", vs.error.c_str(), ps.error.c_str());
    return g_failures;
  }

  std::string why;
  uint32_t vsComponents = 0, psComponents = 0;
  size_t vsCount = 0, psCount = 0;
  Check(VaryingAt(vs.spirv, phxvk::ShaderStage::Vertex, 0, &vsComponents, &vsCount, &why),
        "the vertex shader declares location 0");
  Check(VaryingAt(ps.spirv, phxvk::ShaderStage::Pixel, 0, &psComponents, &psCount, &why),
        "the pixel shader declares location 0");
  // This disagreement is the defect: D3D11 links them, Vulkan refuses the
  // pipeline, and the draw silently disappears.
  Check(vsComponents == 2 && psComponents == 4,
        "before the rewrite the pair disagrees: vertex writes %u, pixel reads %u", vsComponents, psComponents);

  // ---- the rewrite -------------------------------------------------------
  std::vector<uint32_t> fixedVs, fixedPs;
  const phxvk::NormaliseResult rv = phxvk::NormaliseVaryings(vs.spirv, phxvk::ShaderStage::Vertex, &fixedVs, &why);
  Check(rv == phxvk::NormaliseResult::Rewritten, "the vertex shader is widened (%s)",
        rv == phxvk::NormaliseResult::Rewritten ? "Rewritten" : why.c_str());
  // Only the narrow side has to change: the pixel shader already reads four.
  const phxvk::NormaliseResult rp = phxvk::NormaliseVaryings(ps.spirv, phxvk::ShaderStage::Pixel, &fixedPs, &why);
  Check(rp == phxvk::NormaliseResult::Unchanged, "the pixel shader needs no change (%s)",
        rp == phxvk::NormaliseResult::Unchanged ? "Unchanged" : why.c_str());
  Check(fixedPs == ps.spirv, "and its bytecode comes back untouched");

  uint32_t a = 0, b = 0;
  size_t ac = 0, bc = 0;
  Check(VaryingAt(fixedVs, phxvk::ShaderStage::Vertex, 0, &a, &ac, &why) && a == 4,
        "the rewritten vertex shader writes four components at location 0 (got %u)", a);
  Check(VaryingAt(fixedPs, phxvk::ShaderStage::Pixel, 0, &b, &bc, &why) && b == 4,
        "the pixel shader still reads four components at location 0 (got %u)", b);
  Check(a == b, "the two stages now agree (%u == %u)", a, b);
  Check(ac == vsCount, "the rewrite did not invent or lose a vertex varying (%zu -> %zu)", vsCount, ac);
  Check(bc == psCount, "the rewrite did not invent or lose a pixel varying (%zu -> %zu)", psCount, bc);

  // The widening is a CompositeConstruct feeding each store: the two components
  // the shader wrote, then 0 and 1 for the .z and .w it did not.
  Check(CountOpcode(fixedVs, 80) == CountOpcode(vs.spirv, 80) + 1,
        "the vertex shader grew one OpCompositeConstruct per store (%d -> %d)",
        CountOpcode(vs.spirv, 80), CountOpcode(fixedVs, 80));
  Check(fixedVs[3] > vs.spirv[3], "the vertex module's bound was raised (%u -> %u)", vs.spirv[3], fixedVs[3]);

  // ---- the other direction: the pixel shader reads fewer components -------
  // A vertex shader that writes four against a pixel shader that reads two is
  // the same defect mirrored, and is fixed by narrowing under the original id,
  // so the shader body keeps the type it was written against.
  {
    phxvk::CompiledShader wideVs = phxvk::CompileHLSL(kWideQuadShader, strlen(kWideQuadShader), "u",
                                                      phxvk::ShaderStage::Vertex, "wide-quad/vs_5_0");
    phxvk::CompiledShader narrowPs = phxvk::CompileHLSL(kNarrowPs, strlen(kNarrowPs), "p",
                                                        phxvk::ShaderStage::Pixel, "narrow/ps_5_0");
    Check(wideVs.ok && narrowPs.ok, "the wide-vertex / narrow-pixel pair compiles");

    std::vector<uint32_t> outVs, outPs;
    const phxvk::NormaliseResult r1 = phxvk::NormaliseVaryings(wideVs.spirv, phxvk::ShaderStage::Vertex, &outVs, &why);
    const phxvk::NormaliseResult r2 = phxvk::NormaliseVaryings(narrowPs.spirv, phxvk::ShaderStage::Pixel, &outPs, &why);
    Check(r1 == phxvk::NormaliseResult::Unchanged, "the four component vertex shader needs no change");
    Check(r2 == phxvk::NormaliseResult::Rewritten, "the two component pixel shader is widened (%s)",
          r2 == phxvk::NormaliseResult::Rewritten ? "Rewritten" : why.c_str());
    uint32_t c = 0, d = 0;
    size_t cc = 0, dc = 0;
    Check(VaryingAt(outPs, phxvk::ShaderStage::Pixel, 0, &c, &dc, &why) && c == 4,
          "the rewritten pixel shader reads four components at location 0 (got %u)", c);
    (void)cc;
    (void)dc;
    Check(CountOpcode(outPs, 79) > CountOpcode(narrowPs.spirv, 79),
          "and it grew an OpVectorShuffle to narrow each load back (%d -> %d)",
          CountOpcode(narrowPs.spirv, 79), CountOpcode(outPs, 79));
  }

  // ---- a shader that needs nothing ---------------------------------------
  phxvk::CompiledShader wide = phxvk::CompileHLSL(kAlreadyWide, strlen(kAlreadyWide), "p",
                                                  phxvk::ShaderStage::Pixel, "already-wide/ps_5_0");
  Check(wide.ok, "the already-wide pixel shader compiles");
  if (wide.ok)
  {
    std::vector<uint32_t> untouched;
    const phxvk::NormaliseResult r = phxvk::NormaliseVaryings(wide.spirv, phxvk::ShaderStage::Pixel, &untouched, &why);
    Check(r == phxvk::NormaliseResult::Unchanged, "a four component varying needs no rewrite (%s)",
          r == phxvk::NormaliseResult::Unchanged ? "Unchanged" : why.c_str());
    Check(untouched == wide.spirv, "and its bytecode comes back untouched");
  }

  // ---- a stage with no varying interface --------------------------------
  // Only a vertex stage's Outputs and a pixel stage's Inputs are a varying
  // interface; anything else has to be left alone.
  {
    std::vector<uint32_t> out;
    const phxvk::NormaliseResult r = phxvk::NormaliseVaryings(vs.spirv, phxvk::ShaderStage::Geometry, &out, &why);
    Check(r == phxvk::NormaliseResult::Unchanged, "a stage with no varying interface is left alone");
    Check(out == vs.spirv, "and that module comes back untouched");
  }

  printf("%d failed check(s)\n", g_failures);
  return g_failures;
}
