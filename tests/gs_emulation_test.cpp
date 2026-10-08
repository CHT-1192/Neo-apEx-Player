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

static const char *kPbrMixed = R"GSSRC(
Texture2D t_0:register(t0);
Texture2D t_1:register(t1);
Texture2D t_2:register(t2);
Texture2D t_3:register(t3);
SamplerState Sampler:register(s0);
SamplerComparisonState Sampler3:register(s2);

struct lightdata
{
	float4 pos;
	float4 ambient;
	float4 diffuse;
	float4 specular;
	float4 spotdirection;
	float4 sda;
};

cbuffer b : register(b0)
{
	float4x4 viewmat;
	float4x4 projmat;
	float4 campos;
	float4 lightcount;
	lightdata lights[8];
}

cbuffer c : register(b1)
{
	float4x4 worldmat;
	float4x4 itworldmat;
	float4 dat;
	float4 shd;
	float4 albedoMul;
}

struct VSIN
{
	float3 Position : POSITION0;
	float3 Position2: POSITION1;
	float3 Normal : NORMAL0;
	float4 Color : COLOR0;
	float4 UV : TEXCOORD0;
	float4 UV2: TEXCOORD1;
};

struct VSOUT
{
	float4 Position : SV_POSITION;
	float3 Normal: TEXCOORD1;
	float4 uv:TEXCOORD0;
	float3 p:TEXCOORD2;
	float3 lp:TEXCOORD3;
};

float4x4 buildLightMatrix( float3 eye, float3 dir)
{
	float3 z = normalize(dir);
	float3 x = normalize(cross(float3(0,1,0), z));
	float3 y = cross(z, x);

	float4x4 viewmat=float4x4(x.x,y.x,z.x,0,
	                          x.y,y.y,z.y,0,
	                          x.z,y.z,z.z,0,
	                          -dot(x,eye), -dot(y,eye), -dot(z,eye),1);
	return transpose(viewmat);
}

float4x4 buildOrthoMatrix( float w, float h, float r )
{
	float zf=100;
	float zn=0;
	float4x4 orthomat=float4x4(2/w,0,0,0,
	                           0,2/h,0,0,
	                           0,0,1/(zn-zf),0,
	                           0,0,-zn/(zn-zf),1);
	return transpose(orthomat);
}

VSOUT v(VSIN x)
{
	VSOUT k;
	k.p=mul(worldmat,float4(x.Position,1)).xyz;
	k.Normal=mul(itworldmat,float4(x.Normal,0)).xyz;
	k.Position=mul(projmat,mul(viewmat,float4(k.p,1)));
	k.uv=x.UV;

	float4x4 lm=buildLightMatrix(lights[0].spotdirection.xyz,lights[0].pos.xyz);
	float4x4 om=buildOrthoMatrix(lights[0].ambient.w,lights[0].diffuse.w,0);
	
	float4 lp=mul(om,mul(lm,float4(k.p,1)));
	lp/=lp.w;
	k.lp=lp.xyz;
	k.lp.y*=-1;
	k.lp.xy=k.lp.xy*0.5+0.5;

	return k;
}

[maxvertexcount(6)]
void g(triangle VSOUT input[3], inout TriangleStream<VSOUT> OutputStream)
{
	for (int x=0; x<3; x++)
	  OutputStream.Append(input[x]);	
	OutputStream.RestartStrip();	

	if (dat.z>0.5) 	
	{
		for (int x=2; x>=0; x--)
		{
			VSOUT v=input[x];
			v.Normal*=-1;
	  		OutputStream.Append(v);
		}
		OutputStream.RestartStrip();	
	}
}


#define PI 3.14159265

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}  

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
	
    return num / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2  = GeometrySchlickGGX(NdotV, roughness);
    float ggx1  = GeometrySchlickGGX(NdotL, roughness);
	
    return ggx1 * ggx2;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
	
    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
	
    return num / denom;
}

float3 perturb_normal(float3 N, float3 p, float2 uv, float3 map)
{
    float3 dp1 = ddx( p );
    float3 dp2 = ddy( p );
    float2 duv1 = ddx( uv );
    float2 duv2 = ddy( uv );
 
    // solve the linear system
    float3 dp2perp = cross( dp2, N );
    float3 dp1perp = cross( N, dp1 );
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
 
    // construct a scale-invariant frame 
    float invmax = rsqrt( max( dot(T,T), dot(B,B) ) );

	float3x3 tangentFrame=float3x3( T * invmax, B * invmax, N );
	
	if (dot(map,map)==0) map=float3(0.5,0.5,1); //hack to fix not selected texture
	
	map=map*2-1;

	return normalize( mul( map, tangentFrame ) );
}

float ApplyModifier( float value, float modifier )
{
	if (modifier<=127/255.0)
		return value*modifier/(127/255.0);
	else
		return lerp(value,1,(modifier-127/255.0)/(128/255.0));	
}

struct PSOUT
{
	float4 c:SV_TARGET0;
	float4 am:SV_TARGET1;
	float4 nr:SV_TARGET2;
};

void GetLightData( lightdata l, float3 pos, out float3 dir, out float distance, inout float spotcutoff)
{
	if (l.pos.w)
	{
		dir=l.pos.xyz-pos;
		distance=length(dir);
		dir/=distance;
		if (l.sda.y)
		{
			spotcutoff=dot(-l.spotdirection.xyz,dir);
			spotcutoff=(spotcutoff-l.sda.y)/(1-l.sda.y);
			if (spotcutoff>0)
				spotcutoff=pow(abs(saturate(spotcutoff)),l.sda.x);
			else spotcutoff=0;
		}
	}
	else
	{
		dir=normalize(l.pos.xyz);
		distance=1;
	}	
}

float VSM(float2 TexBase, float LightCompare, float shdw)
{
	float shadow=0;

	for (int x=0; x<5; x++)
		for (int y=0; y<5; y++)
			shadow+=t_3.SampleCmpLevelZero(Sampler3, TexBase+float2(x-2,y-2)*0.02*shdw, LightCompare);
	
	return shadow/25.0f;
}

float VSM2(float3 LightMap, float bias, float shdw)
{
	return VSM(LightMap.xy, LightMap.z - 0.01*(bias-0.5), shdw);//saturate((max(VSM(LightMap.xy, LightMap.z - 0.01*(windoffset.x-0.5)), 0.0)-0.0)/1.0);
}

PSOUT CalcLight(VSOUT v, float4 albedo, float4 normalMap, float4 emissiveMap, float4 data, float4 shdw)
{
	if (shdw.y<emissiveMap.w)
	  discard;

	float3 N=perturb_normal(normalize(v.Normal),v.p,v.uv.xy, normalMap.xyz);
	float3 V=normalize(campos.xyz-v.p);
	
	float3 F0 = 0.04;
	float metallic=ApplyModifier(albedo.w,data.y);
	float roughness=ApplyModifier(normalMap.w,data.x);
    F0 = lerp(F0, albedo.xyz, metallic);

	float shadow=1;
	if ( (lights[0].ambient.w*lights[0].diffuse.w)>0.01)
	 shadow=VSM2(v.lp.xyz,data.w,shdw.x);

	
	float3 Lo = emissiveMap.xyz;
    for(int i = 0; i < lightcount[0]; ++i)
    {
		lightdata l=lights[i];
		float3 L;
		float distance;
		float spotcutoff=1;
		GetLightData(l,v.p,L,distance,spotcutoff);

		// calculate per-light radiance
        float3 H = normalize(V + L);
        //float distance    = length(l.pos.xyz - v.p);
        float attenuation = 1.0 / (distance * distance);
        float3 radiance     = l.diffuse.xyz * attenuation * spotcutoff;
        
        // cook-torrance brdf
        float NDF = DistributionGGX(N, H, roughness);        
        float G   = GeometrySmith(N, V, L, roughness);      
        float3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);       
        
        float3 kS = F;
        float3 kD = 1 - kS;
        kD *= 1.0 - metallic;	  
        
        float3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
        float3 specular     = numerator / max(denominator, 0.001);  
            
        // add to outgoing radiance Lo
        float NdotL = max(dot(N, L), 0.0);  
		if ( i == 0 )
		  NdotL *= shadow;              
        Lo += (kD * albedo.xyz / PI + specular) * radiance * NdotL; 
    }   
  
	PSOUT p;
	p.c=float4(Lo, 1.0);
	p.nr=float4(N,roughness);
	p.am=float4(albedo.xyz,metallic);
	
	return p;
}

PSOUT p(VSOUT v)
{
	float4 albedo=t_0.Sample(Sampler,v.uv.xy);
	albedo.xyz*=float3(shd.zw,albedoMul.x);
	float4 normalMap=t_1.Sample(Sampler,v.uv.xy);
	float4 emissiveMap=t_2.Sample(Sampler,v.uv.xy);

	return CalcLight(v, albedo, normalMap, emissiveMap, dat, shd);	
}
)GSSRC";

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
  Check(vs.find("OutputStream") == std::string::npos, "the OutputStream receiver is consumed with the call");
  Check(vs.find("return o; ++gsemu_at") != std::string::npos, "the Append argument becomes the returned vertex");
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

  // --- the same, for a shader whose Appends live in a loop ----------------
  // [PBR Mixed Rendering] appends three vertices inside `for (int x=0; x<3;
  // x++)` and the reversed copy inside `for (int x=2; x>=0; x--)`.  Reading the
  // body linearly puts the restarts after slot 1 and 2 instead of 3 and 6, and
  // every triangle built from that strip is wrong -- which is exactly what
  // happened to the title card before the scanner learned to expand a loop with
  // a literal trip count.
  printf("\ngs_emulation: [PBR Mixed Rendering] (Appends inside for loops)\n");
  const phxvk::GeometryShaderInfo pbrShape = phxvk::DescribeGeometryShader(kPbrMixed, strlen(kPbrMixed), "g");
  Check(pbrShape.present && pbrShape.inputPrimitive == "triangle", "the PBR technique is a triangle geometry shader");
  Check(pbrShape.maxVertexCount == 6, "maxvertexcount is 6 (got %d)", pbrShape.maxVertexCount);

  const phxvk::GsProgram pbr = phxvk::AnalyseGeometryShader(kPbrMixed, "g", "v", pbrShape);
  Check(pbr.ok, "the PBR shader is accepted for emulation%s%s", pbr.ok ? "" : ": ", pbr.ok ? "" : pbr.reason.c_str());
  Check(pbr.inputVertices == 3, "a triangle carries 3 vertices (got %d)", pbr.inputVertices);
  Check(pbr.restartsAfter.size() == 2 && pbr.restartsAfter[0] == 3 && pbr.restartsAfter[1] == 6,
        "the loops are expanded, so the strips break after slot 3 and 6 (got %zu: %d,%d)", pbr.restartsAfter.size(),
        pbr.restartsAfter.empty() ? -1 : pbr.restartsAfter[0], pbr.restartsAfter.size() < 2 ? -1 : pbr.restartsAfter[1]);

  const std::string pbrVs = phxvk::TranslateGeometryShaderToVertex(kPbrMixed, pbr, "g", "v", "GSEMU_PACKED");
  Check(!pbrVs.empty(), "the PBR geometry shader translates");
  Check(pbrVs.find("GSEMU_PACKED { VSIN a0; VSIN a1; VSIN a2; }") != std::string::npos,
        "the packed input carries all three triangle vertices");

  printf("\n=============================================\n");
  if (g_failures) { printf("%d CHECK(S) FAILED\n", g_failures); return g_failures; }
  printf("ALL CHECKS PASSED\n");
  return 0;
}
