#!/usr/bin/env python3
"""Generate deliberately hostile `.apx` projects, one per case, to hunt for bugs.

Why this exists
---------------
The port passed everything Clean Slate could throw at it and still had a real
defect that only showed up on the *other* bundled demos (see docs/STATUS.md
MILESTONE 9).  Clean Slate is one project, written by one team, and it happens to
exercise a narrow slice of the format: every geometry shader in it, for example,
calls its input array `input`, which is exactly the name the emulator used to
hardcode.  A project that named it anything else produced HLSL that did not
compile, and the emulator reported the shader as unemulatable rather than
reporting that it had generated broken code.

So this tool builds the projects Clean Slate is not: geometry shader shapes the
emulator has to refuse or must reproduce exactly, shader interfaces that D3D11
links and Vulkan does not, and numeric values the loader trusts.

How it works
------------
Each case is a copy of a real project with ONE thing changed -- usually the
`<Code>` of a technique that a scene object actually draws, so that the geometry
shader really runs, but sometimes a single root value.  One case per file, so a
crash is attributable to exactly one change.

    tools/make_hostile_apx.py --out work/hostile/cases
    tools/run_hostile_apx.sh work/hostile/cases

The generated files are derived from the game's own project data and stay in
`work/` (gitignored); only this script is committed.
"""

import argparse
import os
import re
import sys

# --------------------------------------------------------------------------
# The project the cases are cut from, and the technique they rewrite.
#
# `E29A95A4D3FD630C418CF73778137606` is "Colored Texture".  In
# depispace13_final_final.apx it is the only technique of material
# 12D5FB8DDBDFAABA4C5755702B02FD76, which is the material of model
# 9CEDCB00F71544F7D376300262092EC8 ("title1", a Plane) used by scene
# 5F4287C2C4E93E206CCD3BC18038005F ("flare"), which a type-3 event renders.
# So a geometry shader put here is drawn on real triangles.

DEFAULT_BASE = os.path.expanduser(
    "~/Repos/apEx-public/Projects/One of These Days The Sky's Gonna Break/"
    "depispace13_final_final.apx")
DEFAULT_TECH = "E29A95A4D3FD630C418CF73778137606"

# --------------------------------------------------------------------------
# The shader scaffold.  Everything here is copied from the real technique's
# <Code> so that the vertex input layout, the constant buffers and the render
# target count stay exactly what the engine sets up: only the stage bodies
# change.  A hostile case that also changed the layout would fail for the wrong
# reason.

PREAMBLE = r"""
Texture2D t_0:register(t0);
SamplerState Sampler:register(s0);

cbuffer b : register(b0)
{
	float4x4 viewmat;
	float4x4 projmat;
	float4 campos;
}

cbuffer c : register(b1)
{
	float4x4 worldmat;
	float4x4 itworldmat;
	float4 color;
	float4 alphatest;
}
"""

VSIN = r"""
struct VSIN
{
	float3 Position : POSITION0;
	float3 Position2: POSITION1;
	float3 Normal : NORMAL0;
	float4 Color : COLOR0;
	float4 UV : TEXCOORD0;
	float4 UV2: TEXCOORD1;
};
"""

VSOUT = r"""
struct VSOUT
{
	float4 Position : SV_POSITION;
	float3 Normal: TEXCOORD1;
	float2 uv:TEXCOORD0;
	float3 p:TEXCOORD2;
};
"""

PSOUT = r"""
struct PSOUT
{
	float4 Color:SV_TARGET0;
	float4 NormalZ:SV_TARGET1;
};
"""

VS = r"""
VSOUT v(VSIN x)
{
	VSOUT k;
	k.p=mul(worldmat,float4(x.Position,1)).xyz;
	k.Position=mul(projmat,mul(viewmat,float4(k.p,1)));
	k.uv=x.UV.xy;
	k.Normal=0;
	return k;
}
"""

PS = r"""
PSOUT p(VSOUT v)
{
	PSOUT o;
	o.Color=float4(1,0.5,0.25,1);
	o.NormalZ=0;
	return o;
}
"""


def shader(gs=None, vs=None, ps=None, vsout=None, psout=None, extra=""):
    """A complete <Code> blob: the real scaffold plus this case's stages."""
    out = PREAMBLE + (vsout if vsout is not None else VSOUT) + (psout if psout is not None else PSOUT)
    out += VSIN + (vs if vs is not None else VS)
    if gs:
        out += gs
    out += ps if ps is not None else PS
    out += extra
    return out


def esc(code):
    """The <Code> element is XML text, so HLSL's < and > have to be escaped.

    The real files do exactly this (`if (a&gt;b)`), and an unescaped
    `TriangleStream<VSOUT>` makes the XML parser see an element and the project
    load with no render targets at all -- which is how this was discovered.
    """
    return code.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# --------------------------------------------------------------------------
# Geometry shader cases.  Each is a `g` entry point; the shape is deliberately
# at or beyond the edge of what src/rhi/gs_emulation.cpp accepts.

GS_CASES = {
    # ---- names and simple loops ------------------------------------------
    "gs-name-i-loop": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<3;k++)
	{
		VSOUT r=i[k];
		r.Position.xy*=1.5;
		o.Append(r);
	}
}
""",
    "gs-name-verts": r"""
[maxvertexcount(3)]
void g(triangle VSOUT verts[3], inout TriangleStream<VSOUT> o)
{
	o.Append(verts[0]);
	o.Append(verts[1]);
	o.Append(verts[2]);
}
""",
    "gs-name-shadowed": r"""
[maxvertexcount(3)]
void g(triangle VSOUT inverts[3], inout TriangleStream<VSOUT> o)
{
	VSOUT a=inverts[0], b=inverts[1], c=inverts[2];
	o.Append(a);
	o.Append(b);
	o.Append(c);
}
""",
    # ---- restarts --------------------------------------------------------
    "gs-6-restart-middle": r"""
[maxvertexcount(6)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<3;k++) o.Append(i[k]);
	o.RestartStrip();
	[unroll] for (int k=0;k<3;k++)
	{
		VSOUT r=i[k];
		r.Position.xy*=1.2;
		o.Append(r);
	}
}
""",
    "gs-double-restart": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
	o.RestartStrip();
	o.RestartStrip();
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-trailing-restart": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
	o.Append(i[1]);
	o.Append(i[2]);
	o.RestartStrip();
}
""",
    "gs-restart-in-if": r"""
[maxvertexcount(6)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<3;k++) o.Append(i[k]);
	if (i[0].uv.x >= 0) o.RestartStrip();
	[unroll] for (int k=0;k<3;k++) o.Append(i[k]);
}
""",
    "gs-restart-in-braceless-if": r"""
[maxvertexcount(6)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
	o.Append(i[1]);
	o.Append(i[2]);
	if (i[0].p.x > 0) o.RestartStrip();
	o.Append(i[0]);
}
""",
    # ---- loops the emulator has to refuse or must survive ----------------
    "gs-dynamic-restart-loop": r"""
[maxvertexcount(6)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	uint n = (uint)max(1.0, i[0].p.x);
	for (uint k=0;k<n;k++)
	{
		o.Append(i[k%3]);
		o.RestartStrip();
	}
}
""",
    "gs-dynamic-append-overrun": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	uint n = (uint)max(1.0, i[0].p.x);
	[loop] for (uint k=0;k<n;k++) o.Append(i[k%3]);
}
""",
    "gs-while-append": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	int k=0;
	while (k<3) { o.Append(i[k]); k++; }
}
""",
    "gs-do-while-append": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	int k=0;
	do { o.Append(i[k]); k++; } while (k<3);
}
""",
    "gs-nested-loops": r"""
[maxvertexcount(4)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int a=0;a<2;a++)
	{
		[unroll] for (int b=0;b<2;b++)
		{
			VSOUT r=i[(a+b)%3];
			r.Position.xy*=1.0+0.1*a;
			o.Append(r);
		}
	}
}
""",
    "gs-overrun-64": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<64;k++) o.Append(i[k%3]);
}
""",
    "gs-maxvertex-32": r"""
[maxvertexcount(32)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<32;k++)
	{
		VSOUT r=i[k%3];
		r.Position.xy*=1.0+0.01*k;
		o.Append(r);
	}
}
""",
    "gs-maxvertex-1": r"""
[maxvertexcount(1)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
}
""",
    # ---- control flow ----------------------------------------------------
    "gs-branch-both-append": r"""
[maxvertexcount(4)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	if (i[0].p.x > 0) { o.Append(i[0]); o.Append(i[1]); }
	else { o.Append(i[1]); o.Append(i[2]); }
}
""",
    "gs-append-in-else-only": r"""
[maxvertexcount(2)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	if (i[0].p.x <= 0)
	{
		o.Append(i[0]);
		o.Append(i[1]);
	}
}
""",
    "gs-early-return-midstrip": r"""
[maxvertexcount(6)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
	o.Append(i[1]);
	if (i[0].p.x > 0) return;
	o.Append(i[2]);
}
""",
    "gs-empty": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
}
""",
    "gs-append-function-call": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(makemiddle(i[0],i[1]));
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    # ---- other primitives and streams ------------------------------------
    "gs-point-4": r"""
[maxvertexcount(4)]
void g(point VSOUT i[1], inout PointStream<VSOUT> o)
{
	[unroll] for (int k=0;k<4;k++)
	{
		VSOUT r=i[0];
		r.Position.xy+=float2(k-2,k-2)*0.02;
		o.Append(r);
	}
}
""",
    "gs-point-trianglestream": r"""
[maxvertexcount(4)]
void g(point VSOUT i[1], inout TriangleStream<VSOUT> o)
{
	VSOUT a=i[0], b=i[0], c=i[0], d=i[0];
	a.Position.xy+=float2(-0.02,-0.02);
	b.Position.xy+=float2( 0.02,-0.02);
	c.Position.xy+=float2( 0.02, 0.02);
	d.Position.xy+=float2(-0.02, 0.02);
	o.Append(a); o.Append(b); o.Append(c);
	o.RestartStrip();
	o.Append(a); o.Append(c); o.Append(d);
}
""",
    "gs-point-restart": r"""
[maxvertexcount(2)]
void g(point VSOUT i[1], inout PointStream<VSOUT> o)
{
	o.Append(i[0]);
	o.RestartStrip();
	o.Append(i[0]);
}
""",
    "gs-line-2": r"""
[maxvertexcount(2)]
void g(line VSOUT i[2], inout LineStream<VSOUT> o)
{
	o.Append(i[1]);
	o.Append(i[0]);
}
""",
    "gs-line-trianglestream": r"""
[maxvertexcount(4)]
void g(line VSOUT i[2], inout TriangleStream<VSOUT> o)
{
	VSOUT a=i[0], b=i[0], c=i[1], d=i[1];
	a.Position.y-=0.01; b.Position.y+=0.01;
	c.Position.y+=0.01; d.Position.y-=0.01;
	o.Append(a); o.Append(b); o.Append(c); o.Append(d);
}
""",
    "gs-lineadj-4": r"""
[maxvertexcount(4)]
void g(lineadj VSOUT i[4], inout LineStream<VSOUT> o)
{
	o.Append(i[0]);
	o.Append(i[1]);
	o.Append(i[2]);
	o.Append(i[3]);
}
""",
    "gs-lineadj-trianglestream": r"""
[maxvertexcount(3)]
void g(lineadj VSOUT i[4], inout TriangleStream<VSOUT> o)
{
	o.Append(i[1]); o.Append(i[2]); o.Append(i[3]);
}
""",
    "gs-triangleadj-3": r"""
[maxvertexcount(3)]
void g(triangleadj VSOUT i[6], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]); o.Append(i[2]); o.Append(i[4]);
}
""",
    "gs-triangleadj-6": r"""
[maxvertexcount(6)]
void g(triangleadj VSOUT i[6], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]); o.Append(i[2]); o.Append(i[4]);
	o.RestartStrip();
	o.Append(i[1]); o.Append(i[3]); o.Append(i[5]);
}
""",
    # ---- system values and interface -------------------------------------
    "gs-primitiveid": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], uint pid : SV_PrimitiveID, inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<3;k++)
	{
		VSOUT r=i[k];
		r.uv += (float)(pid & 1) * 0.1;
		o.Append(r);
	}
}
""",
    "gs-narrow-output": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<GSOUTN> o)
{
	[unroll] for (int k=0;k<3;k++)
	{
		GSOUTN r;
		r.Position=i[k].Position;
		r.uv2=i[k].uv;
		o.Append(r);
	}
}
""",
    "gs-render-target-array-index": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<GSOUTRT> o)
{
	[unroll] for (int k=0;k<3;k++)
	{
		GSOUTRT r;
		r.Position=i[k].Position;
		r.uv=i[k].uv;
		r.rti=0;
		o.Append(r);
	}
}
""",
    "gs-no-maxvertexcount": r"""
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]); o.Append(i[1]); o.Append(i[2]);
}
""",
    "gs-append-scalar": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(1.0);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-nonvoid-return": r"""
[maxvertexcount(3)]
float g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]); o.Append(i[1]); o.Append(i[2]);
	return 1.0;
}
""",
    "gs-hs-ds-too": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]); o.Append(i[1]); o.Append(i[2]);
}
""",
    # ---- malformed Append / RestartStrip calls --------------------------
    # glslang crashed on two of these (docs/STATUS.md MILESTONE 10); the rest
    # are here to keep the guard honest.
    "gs-append-no-args": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append();
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-append-two-args": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0], i[1]);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-append-position-only": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0].Position);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-append-undefined-function": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(noSuchFunction(i[0]));
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-restartstrip-with-arg": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[0]);
	o.RestartStrip(1);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    # ---- out of range input access --------------------------------------
    # The translator copies the body verbatim, so an index the shader should
    # never have written ends up indexing the generated local array.
    "gs-input-index-oob": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[99]);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    "gs-input-index-negative": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	o.Append(i[-1]);
	o.Append(i[1]);
	o.Append(i[2]);
}
""",
    # ---- extreme counts --------------------------------------------------
    "gs-maxvertex-0": r"""
[maxvertexcount(0)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
}
""",
    "gs-maxvertex-1024": r"""
[maxvertexcount(1024)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<1024;k++)
	{
		VSOUT r=i[k%3];
		r.Position.xy*=1.0+0.0001*k;
		o.Append(r);
	}
}
""",
    "gs-literal-loop-10000": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	[unroll] for (int k=0;k<10000;k++) o.Append(i[k%3]);
}
""",
    "gs-deep-nesting": r"""
[maxvertexcount(3)]
void g(triangle VSOUT i[3], inout TriangleStream<VSOUT> o)
{
	if (i[0].p.x > -100) { if (i[0].p.x > -99) { if (i[0].p.x > -98) { if (i[0].p.x > -97) {
	if (i[0].p.x > -96) { if (i[0].p.x > -95) { if (i[0].p.x > -94) { if (i[0].p.x > -93) {
	if (i[0].p.x > -92) { if (i[0].p.x > -91) { if (i[0].p.x > -90) { if (i[0].p.x > -89) {
	if (i[0].p.x > -88) { if (i[0].p.x > -87) { if (i[0].p.x > -86) { if (i[0].p.x > -85) {
	if (i[0].p.x > -84) { if (i[0].p.x > -83) { if (i[0].p.x > -82) { if (i[0].p.x > -81) {
	if (i[0].p.x > -80) { if (i[0].p.x > -79) { if (i[0].p.x > -78) { if (i[0].p.x > -77) {
		o.Append(i[0]); o.Append(i[1]); o.Append(i[2]);
	} } } } } } } } } } } } } } } } } } } } } } } } }
}
""",
}

# Structs and helper functions the GS cases above refer to.
GS_SUPPORT = r"""
struct GSOUTN
{
	float4 Position : SV_POSITION;
	float4 uv : TEXCOORD0;
};

struct GSOUTRT
{
	float4 Position : SV_POSITION;
	float2 uv : TEXCOORD0;
	uint rti : SV_RenderTargetArrayIndex;
};

VSOUT makemiddle(VSOUT a, VSOUT b)
{
	VSOUT r=a;
	r.Position=(a.Position+b.Position)*0.5;
	return r;
}
"""

# Cases where the GS output struct differs, so the pixel shader has to match it
# (or deliberately not match it, which is the point of the case).
GS_ALT_PS = {
    "gs-narrow-output": r"""
PSOUT p(GSOUTN v)
{
	PSOUT o;
	o.Color=float4(v.uv.xy,0,1);
	o.NormalZ=0;
	return o;
}
""",
    "gs-render-target-array-index": r"""
PSOUT p(GSOUTRT v)
{
	PSOUT o;
	o.Color=float4(v.uv,0,1);
	o.NormalZ=0;
	return o;
}
""",
}

# A case whose GS output is widened relative to what the pixel shader reads: the
# emulated vertex stage has to be reconciled with the pixel stage by
# src/rhi/shader_interface.cpp, exactly like a hand-written vertex shader.
GS_NARROW_PIXEL = r"""
PSOUT p(GSOUTN v)
{
	PSOUT o;
	o.Color=float4(v.uv,0,1);
	o.NormalZ=0;
	return o;
}
"""

# --------------------------------------------------------------------------
# Vertex/pixel interface cases: D3D11 links these by signature, Vulkan by
# location and exact type.

IFACE_VSOUT_WIDE = r"""
struct VSOUTW
{
	float4 Position : SV_POSITION;
	float4 uv : TEXCOORD0;
	float4 tint : COLOR0;
};
"""

IFACE_CASES = {
    # vertex writes two components, pixel reads four (the MILESTONE 9 defect)
    "iface-vs2-ps4": dict(
        vsout=r"""
struct VSOUTN
{
	float4 Position : SV_POSITION;
	float2 uv : TEXCOORD0;
};
""",
        vs=r"""
VSOUTN v(VSIN x)
{
	VSOUTN k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV.xy;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTN v)
{
	PSOUT o;
	float4 t=t_0.Sample(Sampler,v.uv);
	o.Color=t;
	o.NormalZ=0;
	return o;
}
"""),
    # vertex writes four, pixel reads two
    "iface-vs4-ps2": dict(
        vsout=r"""
struct VSOUTW
{
	float4 Position : SV_POSITION;
	float4 uv : TEXCOORD0;
};
""",
        vs=r"""
VSOUTW v(VSIN x)
{
	VSOUTW k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTW v)
{
	PSOUT o;
	o.Color=t_0.Sample(Sampler,v.uv.xy);
	o.NormalZ=0;
	return o;
}
"""),
    # scalar varying
    "iface-scalar": dict(
        vsout=r"""
struct VSOUTS
{
	float4 Position : SV_POSITION;
	float fade : TEXCOORD0;
};
""",
        vs=r"""
VSOUTS v(VSIN x)
{
	VSOUTS k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.fade=x.UV.x;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTS v)
{
	PSOUT o;
	o.Color=float4(v.fade,v.fade,v.fade,1);
	o.NormalZ=0;
	return o;
}
"""),
    # three components on both sides
    "iface-vs3-ps3": dict(
        vsout=r"""
struct VSOUT3
{
	float4 Position : SV_POSITION;
	float3 uv : TEXCOORD0;
};
""",
        vs=r"""
VSOUT3 v(VSIN x)
{
	VSOUT3 k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV.xyz;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUT3 v)
{
	PSOUT o;
	o.Color=float4(v.uv,1);
	o.NormalZ=0;
	return o;
}
"""),
    # integer varying, flat
    "iface-int-varying": dict(
        vsout=r"""
struct VSOUTI
{
	float4 Position : SV_POSITION;
	nointerpolation uint2 idx : TEXCOORD0;
};
""",
        vs=r"""
VSOUTI v(VSIN x)
{
	VSOUTI k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.idx=uint2(1,2);
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTI v)
{
	PSOUT o;
	o.Color=float4(v.idx,0,1)/255.0;
	o.NormalZ=0;
	return o;
}
"""),
    # integer narrowing: vertex writes four, pixel reads two
    "iface-int-narrow": dict(
        vsout=r"""
struct VSOUTI
{
	float4 Position : SV_POSITION;
	nointerpolation uint4 idx : TEXCOORD0;
};
""",
        vs=r"""
VSOUTI v(VSIN x)
{
	VSOUTI k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.idx=uint4(1,2,3,4);
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTI v)
{
	PSOUT o;
	o.Color=float4(v.idx.xy,0,1)/255.0;
	o.NormalZ=0;
	return o;
}
"""),
    # noperspective, three components against four
    "iface-noperspective": dict(
        vsout=r"""
struct VSOUTP
{
	float4 Position : SV_POSITION;
	noperspective float3 uv : TEXCOORD0;
};
""",
        vs=r"""
VSOUTP v(VSIN x)
{
	VSOUTP k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV.xyz;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTP v)
{
	PSOUT o;
	o.Color=float4(v.uv,1);
	o.NormalZ=0;
	return o;
}
"""),
    # the pixel shader reads a location the vertex shader never writes
    "iface-missing-location": dict(
        vsout=r"""
struct VSOUTM
{
	float4 Position : SV_POSITION;
	float2 uv : TEXCOORD0;
};
""",
        vs=r"""
VSOUTM v(VSIN x)
{
	VSOUTM k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV.xy;
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTM v)
{
	PSOUT o;
	o.Color=float4(v.uv,0,1);
	o.NormalZ=0;
	return o;
}
""",
        # extra parameter on an unused semantic: glslang numbers varyings by
        # declaration order, not by semantic, so this shifts the interface
        ps_note="see iface-semantic-swap"),
    # the vertex writes no varying at all
    "iface-no-varying": dict(
        vsout=r"""
struct VSOUTV
{
	float4 Position : SV_POSITION;
};
""",
        vs=r"""
VSOUTV v(VSIN x)
{
	VSOUTV k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTV v)
{
	PSOUT o;
	o.Color=float4(1,0,0,1);
	o.NormalZ=0;
	return o;
}
"""),
    # matrix varying
    "iface-matrix": dict(
        vsout=r"""
struct VSOUTX
{
	float4 Position : SV_POSITION;
	float2x2 basis : TEXCOORD0;
};
""",
        vs=r"""
VSOUTX v(VSIN x)
{
	VSOUTX k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.basis=float2x2(1,0,0,1);
	return k;
}
""",
        ps=r"""
PSOUT p(VSOUTX v)
{
	PSOUT o;
	o.Color=float4(v.basis[0],v.basis[1]);
	o.NormalZ=0;
	return o;
}
"""),
    # the two stages declare the same semantics in the opposite order, so
    # glslang's declaration-order location numbering puts them on the wrong
    # locations.  This LINKS, so nothing warns; the picture is wrong.
    "iface-semantic-swap": dict(
        vsout=r"""
struct VSOUTS
{
	float4 Position : SV_POSITION;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR0;
};
""",
        vs=r"""
VSOUTS v(VSIN x)
{
	VSOUTS k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
	k.uv=x.UV.xy;
	k.tint=float4(1,0,0,1);
	return k;
}
""",
        psout=r"""
struct PSOUT
{
	float4 Color:SV_TARGET0;
	float4 NormalZ:SV_TARGET1;
};

struct PSIN
{
	float4 tint : COLOR0;
	float2 uv : TEXCOORD0;
};
""",
        ps=r"""
PSOUT p(PSIN v)
{
	PSOUT o;
	o.Color=float4(v.uv,0,1)*v.tint;
	o.NormalZ=0;
	return o;
}
"""),
}


# --------------------------------------------------------------------------
# Value and structure cases: one root value or one block changed.

def sub_once(old, new, count=1):
    def apply(xml):
        if xml.count(old) < count:
            raise SystemExit("value case: %r not found %d time(s)" % (old, count))
        return xml.replace(old, new, count)
    return apply


def sub_all(old, new):
    def apply(xml):
        return xml.replace(old, new)
    return apply


def first_event_range(start=None, end=None):
    """Rewrite the first event's StartFrame/EndFrame, whatever they were."""
    def apply(xml):
        m = re.search(r"<event>.*?</event>", xml, re.S)
        if not m:
            raise SystemExit("no <event> in the base project")
        blk = m.group(0)
        if start is not None:
            blk = re.sub(r"<StartFrame>-?\d+</StartFrame>", "<StartFrame>%d</StartFrame>" % start, blk, count=1)
        if end is not None:
            blk = re.sub(r"<EndFrame>-?\d+</EndFrame>", "<EndFrame>%d</EndFrame>" % end, blk, count=1)
        return xml[:m.start()] + blk + xml[m.end():]
    return apply


def in_first_block(tag, old, new, count=1):
    """Replace inside the FIRST <tag>...</tag> block only.

    `<Type>3</Type>` and `<Object Type="...">` appear in techniques, models and
    events alike, so a whole-document substitution would hit the wrong one.
    """
    def apply(xml):
        m = re.search(r"<%s>.*?</%s>" % (tag, tag), xml, re.S)
        if not m:
            raise SystemExit("no <%s> block" % tag)
        blk = m.group(0)
        if old not in blk:
            raise SystemExit("%r not in the first <%s> block" % (old, tag))
        blk = blk.replace(old, new, count)
        return xml[:m.start()] + blk + xml[m.end():]
    return apply


def append_before_close(block):
    def apply(xml):
        i = xml.rfind("</apExProject>")
        if i < 0:
            raise SystemExit("no </apExProject>")
        return xml[:i] + block + xml[i:]
    return apply


def drop_tag(tag):
    def apply(xml):
        return re.sub(r"<%s>.*?</%s>" % (tag, tag), "", xml, flags=re.S)
    return apply


def duplicate_block(tag, occurrence=0):
    def apply(xml):
        found = list(re.finditer(r"<%s>.*?</%s>" % (tag, tag), xml, re.S))
        if len(found) <= occurrence:
            raise SystemExit("no %s #%d" % (tag, occurrence))
        b = found[occurrence].group(0)
        return xml[:found[occurrence].end()] + "\n" + b + xml[found[occurrence].end():]
    return apply


def make_events(n, template):
    def apply(xml):
        return append_before_close(template * n)(xml)
    return apply


VALUE_CASES = {
    # ---- root scalars ----------------------------------------------------
    "num-framerate-0": ("<Framerate>100</Framerate>", "<Framerate>0</Framerate>"),
    "num-framerate-neg": ("<Framerate>100</Framerate>", "<Framerate>-100</Framerate>"),
    "num-framerate-huge": ("<Framerate>100</Framerate>", "<Framerate>2000000000</Framerate>"),
    "num-aspect-0": ("<AspectX>64</AspectX>", "<AspectX>0</AspectX>"),
    "num-aspect-0-both": ("<AspectY>27</AspectY>", "<AspectY>0</AspectY>"),
    "num-aspect-neg": ("<AspectX>64</AspectX>", "<AspectX>-64</AspectX>"),
    "num-aspect-huge": ("<AspectY>27</AspectY>", "<AspectY>2000000000</AspectY>"),
    # ---- events: the time maths -----------------------------------------
    "num-event-empty-range": (None, first_event_range(500, 500)),
    "num-event-inverted": (None, first_event_range(1401, 0)),
    "num-event-negative": (None, first_event_range(-1000000, 1401)),
    "num-event-intmax": (None, first_event_range(0, 2147483647)),
    "num-event-start-intmax": (None, first_event_range(2147483647, 2147483647)),
    "num-event-huge-range": (None, first_event_range(0, 2000000000)),
    # ---- splines ---------------------------------------------------------
    "num-spline-time-300": ("<time>255</time>", "<time>300</time>"),
    "num-spline-time-neg": ("<time>255</time>", "<time>-1</time>"),
    "num-spline-time-huge": ("<time>255</time>", "<time>100000</time>"),
    "num-spline-time-dup": ("<time>255</time>", "<time>0</time>"),
    # ---- structure -------------------------------------------------------
    "struct-drop-rendertargets": ("<rendertarget>", None),
    "struct-dup-backbuffer": (None, duplicate_block("rendertarget", 0)),
    "struct-zero-pass": ("<Pass>\n      <Name>", "<Pass>\n      <Name>zz"),
}

# --------------------------------------------------------------------------
# The loader trusts these.  Each one is a place the original code indexes an
# array with a number read straight out of the XML (see work/hostile/
# CRASH-SURFACE.md for the file and line of every one of them).

TRUST_CASES = {
    # apxProject.cpp:504 -- CreateEvent returns NULL for a type it does not
    # know, and the caller imports through the null pointer.
    "bad-event-type-7": in_first_block("event", "<Type>0</Type>", "<Type>7</Type>"),
    "bad-event-type-neg": in_first_block("event", "<Type>0</Type>", "<Type>-1</Type>"),
    "bad-event-type-huge": in_first_block("event", "<Type>0</Type>", "<Type>999999</Type>"),
    # Scene_tool.cpp:254 -- AddObject returns NULL for a primitive it does not
    # know and the caller does not check.
    "bad-object-type-99": in_first_block("scene", '<Object Type="0">', '<Object Type="99">'),
    "bad-object-type-neg": in_first_block("scene", '<Object Type="0">', '<Object Type="-1">'),
    # Scene_tool.cpp:308 -- a scene object whose <model> GUID resolves to
    # nothing.
    "bad-scene-model-guid": sub_once("<model>9CEDCB00F71544F7D376300262092EC8</model>",
                                     "<model>DEADBEEFDEADBEEFDEADBEEFDEADBEEF</model>"),
    "bad-scene-model-short": sub_once("<model>9CEDCB00F71544F7D376300262092EC8</model>",
                                      "<model>SHORT</model>"),
    # phxSplineExt.cpp:117 -- the id attribute is an unbounded array index.
    "bad-spline-value-id":  sub_once("<value id=\"3\">0</value>", "<value id=\"7\">0</value>"),
    "bad-spline-controlpos-id":  sub_once("<controlpos id=\"1\">15</controlpos>", "<controlpos id=\"5\">15</controlpos>"),
    "bad-spline-controlvalue-id":  sub_once("<controlvalue id=\"5\">0</controlvalue>", "<controlvalue id=\"9\">0</controlvalue>"),
    # Model_Tool.cpp:553 -- TransformationF16[12].
    "bad-transformation-index":  sub_once('<transformation index="11" value="0"/>',
                                 '<transformation index="99" value="0"/>'),
    # Model_Tool.cpp:512 -- Parameters[14].
    "bad-parameter-index":  sub_once('<parameter index="10" value="0"/>', '<parameter index="200" value="0"/>'),
    # Model_Tool.cpp:531 -- ParentGUIDS[2].
    "bad-parentguid-index":  sub_once('<parentguid index="1" value="NONENONENONENONENONENONENONENONE"/>',
                             '<parentguid index="7" value="NONENONENONENONENONENONENONENONE"/>'),
    # Scene.cpp:303 -- SplineResults[58], indexed by the clip spline type.
    "bad-clipspline-type":  sub_once('<clipspline type="10">', '<clipspline type="66">'),
    "bad-clipspline-type-huge":  sub_once('<clipspline type="10">', '<clipspline type="9999">'),
    # Texgen_tool.cpp:374 -- unsigned char Parameters[16].
    "bad-texgen-parameter-id":  sub_once("<Parameter ID=\"0\">", "<Parameter ID=\"200\">"),
}


# --------------------------------------------------------------------------
# The generator itself.

def many_varyings(n):
    """A vertex stage declaring n float varyings, against a pixel stage that
    reads all of them.

    This probes the limit src/rhi/shader_interface.cpp moves: it widens EVERY
    varying to four components, so n scalars become 4n components, and a device
    has only so many vertex output components.
    """
    outs = "\n".join("\tfloat v%d : TEXCOORD%d;" % (i, i) for i in range(n))
    sets = "\n".join("\tk.v%d = x.UV.x * %d.0;" % (i, i) for i in range(n))
    sums = " + ".join("v.v%d" % i for i in range(n))
    return dict(
        vsout="""
struct VSOUTM
{
	float4 Position : SV_POSITION;
%s
};
""" % outs,
        vs="""
VSOUTM v(VSIN x)
{
	VSOUTM k;
	k.Position=mul(projmat,mul(viewmat,mul(worldmat,float4(x.Position,1))));
%s
	return k;
}
""" % sets,
        ps="""
PSOUT p(VSOUTM v)
{
	PSOUT o;
	o.Color=float4((%s)*0.001,1,1,1);
	o.NormalZ=0;
	return o;
}
""" % sums)


def technique_bounds(xml, tech):
    i = xml.find("<rendertechnique>\n    <GUID>%s" % tech)
    if i < 0:
        raise SystemExit("technique %s not found in the base project" % tech)
    j = xml.find("</rendertechnique>", i)
    if j < 0:
        raise SystemExit("unterminated technique %s" % tech)
    return i, j + len("</rendertechnique>")


def replace_code(xml, tech, code):
    i, j = technique_bounds(xml, tech)
    blk = xml[i:j]
    m = re.search(r"<Code>(.*?)</Code>", blk, re.S)
    if not m:
        raise SystemExit("technique %s has no <Code>" % tech)
    return xml[:i] + blk[:m.start(1)] + esc(code) + blk[m.end(1):] + xml[j:]


def build_cases(tech):
    cases = []  # (name, description, expect, transform)

    for name, gs in GS_CASES.items():
        ps = GS_ALT_PS.get(name)
        if name == "gs-narrow-output":
            # deliberately: the GS writes float2 uv, the pixel stage reads four
            ps = GS_NARROW_PIXEL.replace("v.uv", "float4(v.uv2,0,1)")
        extra = GS_SUPPORT
        if name == "gs-hs-ds-too":
            extra += r"""
struct HSCTRL { float4 Position : SV_POSITION; };
struct HSIN  { float4 Position : SV_POSITION; };
struct HSOUT { float4 Position : SV_POSITION; };

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hcp")]
HSOUT h(InputPatch<HSIN,3> patch, uint id : SV_OutputControlPointID)
{
	HSOUT o; o.Position=patch[id].Position; return o;
}

HSCTRL hcp(InputPatch<HSIN,3> patch)
{
	HSCTRL o; o.Position=float4(1,1,1,1); return o;
}

[domain("tri")]
float4 d(HSCTRL c, float3 bary : SV_DomainLocation)
{
	return float4(0,0,0,1);
}
"""
        code = shader(gs=gs, ps=ps, extra=extra)
        desc = "geometry shader: %s" % name[len("gs-"):]
        expect = {
            "gs-no-maxvertexcount": "refused (no [maxvertexcount])",
            "gs-dynamic-restart-loop": "refused (non-literal loop with RestartStrip)",
            "gs-append-scalar": "refused (Append of a scalar)",
            "gs-nonvoid-return": "refused (non-void entry point)",
            "gs-empty": "emulated, draws nothing visible",
            "gs-overrun-64": "emulated, writes past maxvertexcount",
            "gs-dynamic-append-overrun": "emulated, number of Appends is data dependent",
            "gs-render-target-array-index": "unsupported system value in the output",
            "gs-narrow-output": "emulated, and the emulated stage's interface widened",
            "gs-hs-ds-too": "GS plus hull/domain stages on a device with neither",
        }.get(name, "emulated")
        cases.append((name, desc, expect,
                      lambda xml, c=code, t=tech: replace_code(xml, t, c)))

    for name, spec in IFACE_CASES.items():
        code = shader(vs=spec.get("vs"), ps=spec.get("ps"),
                      vsout=spec.get("vsout"), psout=spec.get("psout"))
        desc = "vertex/pixel interface: %s" % name[len("iface-"):]
        expect = {
            "iface-semantic-swap": "links but reads the wrong varying",
            "iface-matrix": "matrix varying, refused by the widening",
            "iface-no-varying": "pixel reads a varying the vertex stage never writes",
        }.get(name, "widened by src/rhi/shader_interface.cpp")
        cases.append((name, desc, expect,
                      lambda xml, c=code, t=tech: replace_code(xml, t, c)))

    for name, fn in TRUST_CASES.items():
        cases.append((name, "loader trust: %s" % name, "no crash", fn))

    # The interface widening in src/rhi/shader_interface.cpp can move a shader
    # over the device's vertex output component limit, because four components
    # per varying is more than one.
    for n in (8, 16, 24, 32, 40, 64):
        spec = many_varyings(n)
        code = shader(vs=spec["vs"], ps=spec["ps"], vsout=spec["vsout"])
        cases.append(("iface-many-varyings-%d" % n,
                      "vertex/pixel interface: %d float varyings" % n,
                      "widened to %d components" % (4 * n),
                      lambda xml, c=code, t=tech: replace_code(xml, t, c)))

    for name, spec in VALUE_CASES.items():
        old, new = spec
        if new is None and old == "<rendertarget>":
            fn = drop_tag("rendertarget")
        elif callable(new):
            fn = new
        else:
            fn = sub_once(old, new)
        cases.append((name, "value: %s" % name, "no crash", fn))

    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=DEFAULT_BASE, help="project to cut the cases from")
    ap.add_argument("--tech", default=DEFAULT_TECH, help="rendertechnique GUID to rewrite")
    ap.add_argument("--out", default="work/hostile/cases", help="where to write the cases")
    ap.add_argument("--only", default="", help="only cases whose name contains this")
    args = ap.parse_args()

    base = open(args.base, encoding="utf-8", errors="replace").read()
    if "<apExProject" not in base:
        raise SystemExit("%s is not an .apx" % args.base)
    os.makedirs(args.out, exist_ok=True)

    cases = build_cases(args.tech)

    manifest = ["# Hostile .apx cases", "",
                "Generated by `tools/make_hostile_apx.py` from `%s`." % args.base,
                "Each file changes exactly one thing.", "",
                "| case | what it is | expected |", "| --- | --- | --- |"]
    written = 0
    for name, desc, expect, fn in cases:
        if args.only and args.only not in name:
            continue
        try:
            xml = fn(base)
        except SystemExit as e:
            print("skip %-34s %s" % (name, e), file=sys.stderr)
            continue
        path = os.path.join(args.out, name + ".apx")
        with open(path, "w", encoding="utf-8") as f:
            f.write(xml)
        manifest.append("| `%s` | %s | %s |" % (name, desc, expect))
        written += 1

    with open(os.path.join(args.out, "CASES.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(manifest) + "\n")
    print("wrote %d case(s) to %s" % (written, args.out))


if __name__ == "__main__":
    main()
