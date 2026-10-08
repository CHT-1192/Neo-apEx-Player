# apEx / Phoenix Mesh System — Implementation Spec for a Cross-Platform Vulkan Player

Source of truth (READ-ONLY): `~/Repos/apEx-public/`
Reference project analysed: `~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx`

Every statement below is traced to `file:line`. Statements I could not fully verify are marked
**[UNCERTAIN]**. Latent bugs / portability hazards are marked **[QUIRK]** — these are real
behaviour in the shipped code and a port must reproduce or deliberately fix them.

Legend for paths used in this document (all relative to the repo root):

| Short name | File |
|---|---|
| `phxEngine.h` | `apEx/Phoenix/phxEngine.h` |
| `phxEngine.cpp` | `apEx/Phoenix/phxEngine.cpp` |
| `Mesh.h` / `Mesh.cpp` | `apEx/Phoenix/Mesh.h` / `Mesh.cpp` |
| `Model.h` / `Model.cpp` | `apEx/Phoenix/Model.h` / `Model.cpp` |
| `Project.cpp` | `apEx/Phoenix/Project.cpp` (the runtime `.apx`-blob loader) |
| `Material.cpp` / `Material.h` | `apEx/Phoenix/Material.cpp` / `.h` |
| `RenderLayer.cpp` / `.h` | `apEx/Phoenix/RenderLayer.cpp` / `.h` |
| `Scene.cpp` | `apEx/Phoenix/Scene.cpp` |
| `MinimalExport.cpp` | `apEx/apEx/MinimalExport.cpp` (tool → runtime binary compiler) |
| `Model_Tool.cpp` / `.h` | `apEx/Phoenix_Tool/Model_Tool.cpp` / `.h` |

Two build configurations exist and they differ only in `PhoenixConfig.h` vs `PhoenixConfig_full.h`
(3 unrelated flags differ — `PHX_SPLINE_WAVEFORM_SUPPORT`, `PHX_QUATERNION_HAS_QUADRATIC`,
`SETUPBOX_HAS_SOCIAL`; no `PHX_MESH_*` flag differs, so **all mesh enum numbers are stable across
both configs**, verified by `diff apEx/Phoenix/PhoenixConfig.h apEx/Phoenix/PhoenixConfig_full.h`).

---

## 1. Vertex format and index buffers

### 1.1 The size macro

```c
// phxEngine.h:53
#define PHXVERTEXFORMATSIZE ((3+3+3+2*4)*sizeof(float)+sizeof(unsigned int))
```
= `(3+3+3+8)*4 + 4` = `17*4 + 4` = **72 bytes**.

### 1.2 The authoritative declaration

The runtime declares the input layout as an explicit `D3D11_INPUT_ELEMENT_DESC` array — it does
**not** use the Bedrock/CoRE2 vertex-format abstraction (see §1.6):

```c
// phxEngine.cpp:136-144
static const D3D11_INPUT_ELEMENT_DESC vxdecl2[ 6 ] = {
  { "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, sizeof(float)*0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "Position", 1, DXGI_FORMAT_R32G32B32_FLOAT,    0, sizeof(float)*3,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "Normal",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, sizeof(float)*6,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "Color",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, sizeof(float)*9,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float)*10, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  { "Texcoord", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float)*14, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
// phxEngine.cpp:190 → phxDev->CreateInputLayout( vxdecl2, 6, <VS blob "c">, ..., &RenderVertexFormat );
```

The matching CPU-side struct is `PHXVERTEXDATA`:

```c
// Mesh.h:119-127
struct PHXVERTEXDATA {
  D3DXVECTOR3 Position;   // 12
  D3DXVECTOR3 Position2;  // 12  → semantic "Position1" (POSITION1)
  D3DXVECTOR3 Normal;     // 12
  unsigned int Color;     //  4  → R8G8B8A8_UNORM
  D3DXVECTOR4 Texcoord;   // 16  → TEXCOORD0
  D3DXVECTOR4 Texcoord2;  // 16  → TEXCOORD1
};                        // sizeof == 72 == PHXVERTEXFORMATSIZE (no padding; all members 4-byte aligned)
```

### 1.3 Exact per-vertex layout

| Offset | Bytes | HLSL semantic | DXGI format | CPU field | Meaning |
|---|---|---|---|---|---|
| 0 | 12 | `POSITION0` (float3) | `R32G32B32_FLOAT` | `Position` | object-space position |
| 12 | 12 | `POSITION1` (float3) | `R32G32B32_FLOAT` | `Position2` | **second position, NOT a tangent** |
| 24 | 12 | `NORMAL0` (float3) | `R32G32B32_FLOAT` | `Normal` | **per-polygon-corner** normal |
| 36 | 4 | `COLOR0` (float4 input) | `R8G8B8A8_UNORM` | `Color` (uint32) | vertex tint, RGBA byte order |
| 40 | 16 | `TEXCOORD0` (float4) | `R32G32B32A32_FLOAT` | `Texcoord` | UV set 0 in `.xy`; `.zw` normally 0 |
| 56 | 16 | `TEXCOORD1` (float4) | `R32G32B32A32_FLOAT` | `Texcoord2` | UV set 1 in `.xy` |
| **72** | | | | | **stride** |

There is **no tangent, no bitangent, no blend weights/indices, no bone data** in this format.
`Position2` is a plain second position used by effects such as vegetation/voxelising; the Clean
Slate shaders declare it but never read it (verified: `grep "Position2" cleanslate.apx` returns only
the 18 declarations of the form `float3 Position2: POSITION1;` inside `struct VSIN`, no uses).
The canonical shader-side struct, taken verbatim from Clean Slate
(`cleanslate.apx:41543-41551`, repeated in every `rendertechnique/Pass/Code`):

```hlsl
struct VSIN {
  float3 Position  : POSITION0;
  float3 Position2 : POSITION1;
  float3 Normal    : NORMAL0;
  float4 Color     : COLOR0;
  float4 UV        : TEXCOORD0;
  float4 UV2       : TEXCOORD1;
};
```

**Color byte order:** `BuildMesh` builds `unsigned char Color[4]` as
`Color[z] = clamp(Vertices[...].Color[z]*255)` for `z = 0..3`, then stores it as a `uint32` by
`((unsigned int*)Color)[0]` (`Mesh.cpp:406-413`). On little-endian this means byte 0 = `Color[0]`
= R. With `R8G8B8A8_UNORM` the hardware reads byte 0 as R — consistent. A Vulkan port should use
`VK_FORMAT_R8G8B8A8_UNORM`; using `B8G8R8A8` (which is what CoRE2's `COREVXATTR_COLOR4` maps to,
`Bedrock/CoRE2/DX11VertexFormat.cpp:99-107`) would swap R and B. The intermediate `CphxVertex`
keeps colour as **floats in [0,1]** plus a `ColorCount`, and quantises only at `BuildMesh` time
(`Mesh.h:142`, `Mesh.cpp:408`).

### 1.4 Vertices are never shared

`BuildMesh` emits `p->VertexCount` vertices **per polygon** (4 for a quad, 3 for a triangle) and the
normal written is `p->Normals[y]`, i.e. a per-polygon-corner normal
(`Mesh.cpp:379-429`). There is no vertex welding anywhere, and no vertex cache. Vertex order is
"walk polygons in order, walk each polygon's corners in order" — a stable, dependency-free order
that a port can reproduce directly.

### 1.5 Index buffer format

* `BuildMesh` allocates `int*` (32-bit) index arrays (`Mesh.cpp:387-391`) and creates the buffers
  with the same `D3D11_BUFFER_DESC` (`D3D11_USAGE_DEFAULT`, `D3D11_BIND_INDEX_BUFFER`,
  `Mesh.cpp:484-507`). Note `BindFlags` is reused for the wire buffer, which was previously set to
  `D3D11_BIND_INDEX_BUFFER` (`Mesh.cpp:497-499`) — so the wire buffer is an index buffer too.
* Binding is always `DXGI_FORMAT_R32_UINT` (`RenderLayer.cpp:88`, `UberTool.cpp:1389`) →
  **`VK_INDEX_TYPE_UINT32`**.
* Triangle indices, per polygon with base vertex `VxPos` (`Mesh.cpp:431-448`):
  * any polygon: `(VxPos+0, VxPos+2, VxPos+1)`
  * quad additionally: `(VxPos+0, VxPos+3, VxPos+2)`
  * "forced one way triangulation of quads" is an explicit comment in the source (`Mesh.cpp:441`).
* `TriCount = Σ (poly.VertexCount − 2)` (`Mesh.cpp:383`); the draw call uses
  `Model->TriCount * 3` as the index count (`Material.cpp:201`).
* Wireframe indices are `(VxPos+y, VxPos+(y+1)%VertexCount)` emitted once per edge
  (`Edges[...].PolyIDs[0] == x` test, `Mesh.cpp:451-458`), giving `EdgeCount` line segments;
  the draw uses `Model->EdgeCount * 2` (`Material.cpp:201`) with
  `D3D11_PRIMITIVE_TOPOLOGY_LINELIST` (`RenderLayer.cpp:48`).
* Topology for the indexed path is `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST`
  (`RenderLayer.cpp:48`); for non-indexed particle buffers it is `POINTLIST` (`RenderLayer.cpp:52`).

**Winding / culling (port-critical).** The camera is right-handed
(`D3DXMatrixLookAtRH`, `Timeline.cpp:174`; `D3DXMatrixPerspectiveOffCenterRH`, `Timeline.cpp:188`)
and D3D11's rasteriser default is `FrontCounterClockwise = FALSE`, i.e. **clockwise = front face**
(also the explicit tool-side default in `Project.cpp:1852` `RasterDesc.CullMode = D3D11_CULL_BACK`).
Since `BuildMesh` reverses the polygon's vertex order when making triangles, a port must
empirically confirm whether to set `VK_FRONT_FACE_CLOCKWISE` or to negate the viewport Y; the
combination of an RH projection with D3D's Y convention is the standard source of this confusion.
**[UNCERTAIN]** — I did not render it; treat front-face selection as a runtime-verifiable detail and
keep both options behind a flag. Depth state: `DepthClipEnable = true`, solid fill, back-face
culling (`Project.cpp:1852-1854`).

### 1.6 Relationship to `Bedrock/CoRE2/VertexFormat.h`

`Bedrock/CoRE2/VertexFormat.h` defines the **tool/engine abstraction** `CCoreVertexFormat`
(`VertexFormat.h:7-19`) built from `COREVERTEXATTRIBUTE` enum values (`Bedrock/CoRE2/Enums.h:22-32`),
mapped to `D3D11_INPUT_ELEMENT_DESC` in `Bedrock/CoRE2/DX11VertexFormat.cpp:36-135`:

| CoRE2 attribute | semantic | format | size |
|---|---|---|---|
| `COREVXATTR_POSITION3` | `Position` +idx | `R32G32B32_FLOAT` | 12 |
| `COREVXATTR_POSITION4` | `Position` +idx | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_NORMAL3` | `Normal` +idx | `R32G32B32_FLOAT` | 12 |
| `COREVXATTR_TEXCOORD2` | `Texcoord` +idx | `R32G32_FLOAT` | 8 |
| `COREVXATTR_TEXCOORD4` | `Texcoord` +idx | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_COLOR4` | `Color` +idx | **`B8G8R8A8_UNORM`** | 4 |
| `COREVXATTR_COLOR16` | `Color` +idx | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_POSITIONT4` | `PositionT` +idx | `R32G32B32A32_FLOAT` | 16 |

Offsets are computed by accumulating `Size` in declaration order
(`DX11VertexFormat.cpp:44-46`), so a CoRE2 declaration cannot express `PHXVERTEXDATA` exactly
(it has no 4-component texcoord→2-component pairing, and its colour format is BGRA). The **only**
place the two systems touch is the tool's helper-mesh display
(`apEx/apEx/UberTool.cpp:88-90`, `1377-1395`), where CoRE2's `VertexFormat` is the pair
`{COREVXATTR_POSITION3, COREVXATTR_TEXCOORD2}` (20-byte assumed layout) but the *Phoenix* vertex
buffer is bound with `stride = PHXVERTEXFORMATSIZE` (72). Position reads correctly (offset 0); the
`Texcoord` semantic reads bytes 12..20, i.e. `Position2.xy`. **[QUIRK]** — harmless in the original
because that shader only needs position; do not copy this into a port.

---

## 2. Mesh object model

### 2.1 Class hierarchy

```
CphxModel                       Model.h:108-121   CphxArray<CphxModelObject*> Objects;
 ├── CphxModelObject            Model.h:14-40     abstract; parentobjcount/parentobjects (DAG edges),
 │                                                TransformationF16[12], Primitive, ClonedObjects,
 │                                                virtual CreateRenderDataInstances(...)
 ├── CphxModelObject_Mesh       Model.h:62-94     MeshParameters[14], Text, StoredVertices/Polygons,
 │                                                VxBuffer/IndexBuffer/WireBuffer + VxCount/TriCount/
 │                                                EdgeCount, dataBuffer/dataBufferView,
 │                                                MaterialState, Material, CphxMesh Mesh,
 │                                                FilterCount, PHXMESHFILTERDATA* FilterData
 └── CphxModelObject_Clone      Model.h:96-106    only ClonedObjects[] (refers to other objects)
```

* A `CphxModel` is an **array of objects**, not a tree (`Model.cpp:6-10` iterates `Objects` and
  recurses only through clones).
* The dependency structure is a **DAG of mesh operators** encoded in `parentobjcount` /
  `parentobjects` (`Model.h:18-19`, filled at `Project.cpp:838-841`). Primitives consume 0 parents,
  except: `Mesh_Copy`/`Mesh_Clone`/`Mesh_Merge` = N parents (count read from
  `Stream_ModelReferences`), `Mesh_Scatter` and `Mesh_Loft` = exactly 2 parents
  (`Project.cpp:788-811`), and `ModelFilter_CSG` adds one more implicit dependency on the object
  index stored in its first filter parameter (`Project.cpp:878-882`).
* Each mesh object additionally owns an ordered **filter chain** `FilterData[0..FilterCount-1]`
  (`Model.h:85-86`) — this is the "mesh op" list. Filters are applied strictly in array order after
  the primitive is generated (`Project.cpp:903-991`).
* `CphxModelObject::GetMatrix()` expands the 12 stored half-floats into a 4x4 with an identity
  bottom row (`Model.cpp:14-23`, layout `m[x][y] = TransformationF16[y + x*3]`).

### 2.2 How an object is created and executed at load time

There is **no per-operator C++ class in the runtime**. The "factory" is a pair of `switch`
statements driven by the serialized enums:

1. **Instantiation** (`Project.cpp:686-720`): read `PHXMESHPRIMITIVE` byte → `new CphxModelObject_Clone`
   if `Mesh_Clone`, else `new CphxModelObject_Mesh`.
2. **Primitive generation** — `GenerateMesh(CphxModelObject*, CphxModel*)` at `Project.cpp:1702-1848`,
   a `switch (o->Primitive)` that calls one `CphxMesh` method per case (this is the factory that maps
   a primitive enum to generator code).
3. **Filter application** — the `switch (m->FilterData[z].Type)` at `Project.cpp:907-991` that maps
   each `PHXMESHFILTER` to a `CphxMesh` method call.
4. **GPU upload** — a single unconditional `m->Mesh.BuildMesh(...)` per object at
   `Project.cpp:1005`.
5. **Ordering**: a work-list loop with a `minimalGenerated` flag runs the whole set until every
   object is produced, so parents are always finished before children
   (`Project.cpp:852-900`). Objects whose parents are unfinished are skipped and retried, and
   `Mesh_Clone` objects are marked done as soon as their `ClonedObjects` list is resolved
   (`Project.cpp:861-869`).
6. On the **tool** side the mapping really is class-based but still one class for all filters:
   `CphxMeshFilter_Tool` (`Model_Tool.h:11-44`) holds `PHXMESHFILTER Filter`, `unsigned char
   Parameters[3]`, `D3DXFLOAT16 srt[12]`, `Enabled` and a `Texture` reference. It is created by
   `CphxModelObject_Tool_Mesh::AddFilter(PHXMESHFILTER)` at `Model_Tool.cpp:698-712`. Adding a
   filter never changes the C++ type.

### 2.3 Serialized per-object record (runtime binary; the `.apx` is a different, XML-ish format)

Fixed-order record per object, read at `Project.cpp:686-853` (writer: `MinimalExport.cpp:960-1105`):

| # | Source stream | Size | Meaning |
|---|---|---|---|
| 1 | `Stream_ModelPrimitives` | 1 | `PHXMESHPRIMITIVE` |
| 2 | `Stream_ModelTransformation` | 24 | 12 × `D3DXFLOAT16` (3x4 matrix) |
| 3 | `Stream_ModelParameters` | `MeshPrimitiveParameterCounts[prim]` | primitive params |
| 4 | *special* | varies | `Mesh_Text`: ASCIIZ text; `Mesh_Stored`: inline vx/poly blob; `Mesh_StoredMini`: 1-byte index; `Mesh_Spline`: nothing. See §5 and §7 |
| 5 | `Stream_ModelReferences` | `parentobjcount` bytes | parent object indices |
| 6 | `Stream_Main` | material data | skipped for Line/Arc/Spline/Clone (`Project.cpp:814-826`) |
| 7 | `Stream_ModelFilterCount` | 1 | number of filters |
| 8 | per filter: `Stream_ModelFilterType` | 1 | `PHXMESHFILTER` |
| 9 | per filter: `Stream_ModelFilterData` | `MeshFilterParameterCounts[type]` | filter params |
| 10 | per filter: `Stream_ModelTransformation` (or `Stream_TextureOperatorReference` for `TintMesh`) | `MeshFilterExtraDataSize[type]` | SRT / texture-op index |

Two authoritative lookup tables in the runtime:

```c
// Project.cpp:107-129
MeshPrimitiveParameterCounts[] = { 0,2,5,3,4,3,1,2,7,0,0,2,14,0,6,7,2,0,0,0 };  // Mesh_Cube..Mesh_Merge
// Project.cpp:131-144
MeshFilterParameterCounts[] = { 2,1,0,2,1,2,3,1,2,2,3 };  // UVMap,Bevel,MapXForm,Smooth,SmoothGroup,Tint,TintShape,Replicate,NormalDeform,CSG,Greeble
// Project.cpp:146-158
MeshFilterExtraDataSize[]  = { 20,0,20,0,0,2,20,24,0,0 };   // ONLY 10 ENTRIES
```

**[QUIRK / real bug]** `MeshFilterExtraDataSize[]` has only 10 entries (indices 0..9 = `UVMap` ..
`CSG`), but `Project.cpp:850` indexes it unconditionally with the filter type — so
`ModelFilter_Greeble (=10)` reads past the end of the array, and `ModelFilter_Invert (11)` /
`ModelFilter_SavePos2 (12)` would read further still. The writer (`MinimalExport.cpp:762-808`)
emits **no** extra bytes for Greeble/Bevel/MeshSmooth/SmoothGroup/NormalDeform/CSG, so the correct
behaviour is "0 extra bytes". Since Clean Slate uses 14 Greeble filters and loads correctly, the
out-of-bounds byte must be 0 in the shipped build. **A port must treat Greeble's extra data size as
0.** The same class of bug exists for `MeshFilterParameterCounts` if any filter ≥ 11 is used
(they are never used, see §3).

### 2.4 `PHXMESHFILTERDATA` and the SRT "filter transform"

```c
// Mesh.h:129-134
struct PHXMESHFILTERDATA { PHXMESHFILTER Type; unsigned char *FilterParams; D3DXFLOAT16 *filtertransform; };
```

`filtertransform` points into the transformation stream and is interpreted per filter as
`scale(3 halves) + quaternion(4 halves) + translate(3 halves)` = 10 halves = 20 bytes
(`Project.cpp:910-924`, `D3DXQUATERNION(filtertransform + 3)`, `D3DXVECTOR3(filtertransform + 7)`),
except `Replicate`, which reads a full 3x4 matrix = 12 halves = 24 bytes (`Mesh.cpp:2028-2035`), and
`TintMesh`, whose "extra" 2 bytes are a `unsigned short` index into the texture-operator table
(`Project.cpp:836-847`, `Mesh.cpp` usage at `Project.cpp:934-937`).

---

### 2.5 Full enum reference

#### 2.5.1 `PHXMESHFILTER` (mesh operators) — `Mesh.h:77-117`

Name strings: note the source comment "if you add one here add it to the `ModelFilterNames[]`
variable in the tool as well". Two name arrays exist and **neither is complete**:
* `apEx/apEx/ModelGraph.cpp:10-23` `ModelFilterNames[]` — UI names, **11 entries**, missing
  `Invert` and `SavePos2`.
* `apEx/apEx/MinimalExport.cpp:3132-3147` `meshFilterNames[]` — C names, complete, used only in
  logging/symbol dumps.

| # | Enum | UI name (`ModelGraph.cpp`) | Runtime dispatch (`Project.cpp`) | C++ impl | Params | Extra bytes | What it does |
|---|---|---|---|---|---|---|---|
| 0 | `ModelFilter_UVMap` | "UV Map" | `:909-915` | `CalculateTextureCoordinates` `Mesh.cpp:1002` | 2: `[0]`=`PHXTEXTUREMAPTYPE` 0..4, `[1]`= channel (low nibble) \| clip bit (high nibble) | 20 (SRT) | project/generate UVs for one channel |
| 1 | `ModelFilter_Bevel` | "Bevel" | `:950-953` | `Bevel` `Mesh.cpp:1282` | 1: percent×255 | 0 | inset each face by `percent`, offset the shared vertices, bridge with quads |
| 2 | `ModelFilter_MapXForm` | "Map XForm" | `:917-923` | `MapXForm` `Mesh.cpp:1263` | 0 | 20 (SRT) | displace/rotate/scale each vertex by SRT scaled by `Color[0]` |
| 3 | `ModelFilter_MeshSmooth` | "Mesh Smooth" | `:925-928` | `CatmullClark` `Mesh.cpp:521` | 2: `[0]`!=0 → linear subdivision, `[1]`=iterations | 0 | Catmull–Clark (or linear) subdivision |
| 4 | `ModelFilter_SmoothGroup` | "Smooth Group" | `:929-933` | sets `SmoothGroupSeparation` | 1: `sep` | 0 | sets shading mode/angle: 0=flat, <2=angle threshold, ≥2=Gouraud (see §4.4) |
| 5 | `ModelFilter_TintMesh` | "Tint" | `:934-937` | `CalculateTint` `Mesh.cpp:1121` | 2: `[0]`=texture channel, `[1]`=saturation | 2 (texop idx) | read back a texture op's result and bake per-vertex colour |
| 6 | `ModelFilter_TintMeshShape` | "Tint With Shape" | `:938-949` | `CalculateTintShape` `Mesh.cpp:1201` | 3: shape(0 box,1 sphere,2 plane), op(0 set,1 add,2 sub,3 mul), power | 20 (SRT) | procedural vertex-colour shape |
| 7 | `ModelFilter_Replicate` | "Replicate" | `:955-958` | `Replicate` `Mesh.cpp:2023` | 1: count | 24 (3x4 matrix) | apply matrix to base, then emit `count` repeated copies compounding the matrix |
| 8 | `ModelFilter_NormalDeform` | "Normal Deform" | `:959-962` | `NormalDeform` `Mesh.cpp:649` | 2: `[0]`/255 × (`[1]`−127) | 0 | `Position += Normal * factor * Color[0]` |
| 9 | `ModelFilter_CSG` | "Bool" | `:967-979` | `CSG` `Mesh.cpp:2606` | 2: `[0]`=other object index, `[1]`=op(0 union,1 diff,2 intersect) | 0 | BSP constructive solid geometry against another object |
| 10 | `ModelFilter_Greeble` | "Greeble" | `:980-983` | `Greeble` `Mesh.cpp:2784` | 3: seed, extrude×255, taper×255 | **0** (see §2.3 quirk) | randomly split faces, then extrude/taper each face along its normal |
| 11 | `ModelFilter_Invert` | *(absent)* | **no case — never executed** | `Invert` `Mesh.cpp:2882` | 0 | — | reverse each polygon's vertex order |
| 12 | `ModelFilter_SavePos2` | *(absent)* | **no case — never executed** | `SavePos2` `Mesh.cpp:2898` | 0 | — | `Position2 = Position` |
| 13 | `ModelFilter_NONE` | — | — | — | — | — | sentinel, tool only (`Mesh.h:116`) |

`ModelFilter_Invert` and `ModelFilter_SavePos2` are **dead at runtime**: `grep ModelFilter_Invert`
over the whole repo finds only the enum, the `MinimalExport` name string and the implementation —
there is no dispatch case, and `MeshFilterExtraDataSize` has no entry for them either. A port does
not need them to run existing content, and re-adding them would change the trailing enum values
(safe, since they are last).

#### 2.5.2 `PHXMESHPRIMITIVE` (generators) — `Mesh.h:15-75`

| # | Enum | Tool default name (`Model_Tool.cpp:103-225`) | Impl | Params (`GenerateMesh` `Project.cpp:1717-1835`) |
|---|---|---|---|---|
| 0 | `Mesh_Cube` | "Cube" | `Mesh.cpp:685` | — (0) |
| 1 | `Mesh_Plane` | "Plane" | `Mesh.cpp:753` | XRes, YRes |
| 2 | `Mesh_Sphere` | "Sphere" | `Mesh.cpp:731` | XRes, YRes, TopCut, BottomCut, Caps |
| 3 | `Mesh_Cylinder` | "Cylinder" | `Mesh.cpp:792` | XRes, YRes, Caps |
| 4 | `Mesh_Cone` | "Cone" | `Mesh.cpp:772` | XRes, YRes, TopCut, Caps |
| 5 | `Mesh_Arc` | "Arc" | `Mesh.cpp:834` | Res, Degree, HaveLastSegment |
| 6 | `Mesh_Line` | "Line" | `Mesh.cpp:806` | Res |
| 7 | `Mesh_Spline` | "Spline" | `Mesh.cpp:869` | Res, Loop (**not generated at load** — see §4.2) |
| 8 | `Mesh_Loft` | "Loft" | `Mesh.cpp:906` | PathClosed, SliceClosed, Rotation, StartScale, EndScale (+2 parents) |
| 9 | `Mesh_Clone` | "Clone" | `Model.cpp:48` | N/A — instantiates parents with its own matrix |
| 10 | `Mesh_Copy` | "Copy" | `Mesh.cpp:981` | 1 parent, copy transform |
| 11 | `Mesh_GeoSphere` | "GeoSphere" | `Mesh.cpp:1439` | Iterations |
| 12 | `Mesh_Scatter` | "Scatter" | `Mesh.cpp:1551` | 14 (+2 parents) |
| 13 | `Mesh_Stored` | "Stored" | `Mesh.cpp:1694` | inline stored mesh |
| 14 | `Mesh_Tree` | "Tree" | `Mesh.cpp:1762` | species, seed, 4 densities |
| 15 | `Mesh_TreeLeaves` | "TreeLeaves" | `Mesh.cpp:1776` | species, seed, 5 densities |
| 16 | `Mesh_Text` | "Text" | `Mesh.cpp:1870` | font index, deviation |
| 17 | `Mesh_Marched` | *(no tool default case)* | `Mesh.cpp:2073` | 0 in the count table (**[UNCERTAIN]** — `GenerateMesh` has no `case Mesh_Marched`, so marching-mesh objects cannot be generated at runtime from a project; the entry exists only in the tool's `MinimalExport`) |
| 18 | `Mesh_StoredMini` | "StoredMini" | `Mesh.cpp:1706` | mini-mesh index byte |
| 19 | `Mesh_Merge` | "Merge" | `Mesh.cpp:989` | N parents |

Note the enum numbering is what gets written to `cleanslate.apx` as `<Object Type="N">`
(`Model_Tool.cpp:37`, `Objects[x]->GetPrimitive()`), and it matches the runtime table order — the
array index and the enum value coincide because all `PHX_MESH_*` guards are enabled in both configs.

---

## 3. Which mesh ops Clean Slate actually uses

### 3.1 Method and evidence

`cleanslate.apx` is **plain text XML** (`<apExProject build="0498_19821r">`), not a binary blob; only
the exported runtime blob is binary. The tool writes one `<Object Type="N">` per mesh object with
`<Filter Type="N">` children (`Model_Tool.cpp:433-468`) and `AddFilter` on import
(`Model_Tool.cpp:554-566`). Extraction was done with a Python script that tracks element nesting
(depth/indentation) so that only `<Filter>` elements whose direct parent is `<Object>` are counted —
**all 429 typed `<Filter>` elements in the project are inside `<Object>`**, and the `<Filter>GUID
</Filter>` elements elsewhere (event/model references, 1149 of them) are excluded. Script kept at
`docs/spec/_scratch/parse_apx.py`, `docs/spec/_scratch/params.py`.

Two more checks that matter for the numbers below:
* Every object stores all **12** `<parameter>` entries regardless of the primitive's real parameter
  count (`Model_Tool.cpp:448-453` writes `Parameters[0..11]` unconditionally), while the runtime
  table `MeshPrimitiveParameterCounts` (`Project.cpp:107-129`) defines how many are meaningful and
  `MinimalExport.cpp:982-985` only exports that many. **Parameter values beyond the count are
  uninitialised leftovers** and must be ignored.
* The exporter skips `<Filter>` nodes with `<enabled>0</enabled>` (`MinimalExport.cpp:743-754`).
  I checked all 429: **all of them are enabled** (`<enabled>1</enabled>` or absent), so the ranking
  is unaffected. 503 of the 680 objects have no filters at all.

### 3.2 Object/primitive ranking (680 objects, 105 models)

| Rank | Type | Enum | Count | Share |
|---|---|---|---|---|
| 1 | Cone | `Mesh_Cone` (4) | 165 | 24.3% |
| 2 | Cube | `Mesh_Cube` (0) | 139 | 20.4% |
| 3 | Cylinder | `Mesh_Cylinder` (3) | 101 | 14.9% |
| 4 | Text | `Mesh_Text` (16) | 45 | 6.6% |
| 5 | Arc | `Mesh_Arc` (5) | 39 | 5.7% |
| 6 | Clone | `Mesh_Clone` (9) | 37 | 5.4% |
| 7 | Loft | `Mesh_Loft` (8) | 35 | 5.1% |
| 8= | Plane | `Mesh_Plane` (1) | 31 | 4.6% |
| 8= | Line | `Mesh_Line` (6) | 31 | 4.6% |
| 10 | GeoSphere | `Mesh_GeoSphere` (11) | 27 | 4.0% |
| 11 | Sphere | `Mesh_Sphere` (2) | 9 | 1.3% |
| 12 | Scatter | `Mesh_Scatter` (12) | 8 | 1.2% |
| 13 | Copy | `Mesh_Copy` (10) | 6 | 0.9% |
| 14= | StoredMini | `Mesh_StoredMini` (18) | 3 | 0.4% |
| 14= | Spline | `Mesh_Spline` (7) | 3 | 0.4% |
| 16 | Merge | `Mesh_Merge` (19) | 1 | 0.1% |

**Never used:** `Mesh_Stored` (13), `Mesh_Tree` (14), `Mesh_TreeLeaves` (15), `Mesh_Marched` (17).

### 3.3 Filter ranking (429 filter applications)

| Rank | Filter | Enum | Count | Notes from the project's own parameter values |
|---|---|---|---|---|
| 1 | UV Map | 0 | 130 | maptype: planar 102, `TransformOriginal` 10, box 9, cylindrical 7, spherical 2; channel 0 in 127, channel 1 in 3; **UV clipping never enabled** (high nibble always 0) |
| 2 | Smooth Group | 4 | 104 | 255 (→ separation 2.0 = Gouraud) in 70, 0 (flat) in 7, other angles in 27 |
| 3 | Mesh Smooth | 3 | 69 | linear flag 0 in 39 / 1 in 30; iterations 1:30, 2:21, 3:16, 4:2 |
| 4 | Tint | 5 | 33 | texture channel 0 in 32, 1 in 1; **saturation always 0** |
| 5 | Bevel | 1 | 31 | percent 7..120 (×255) |
| 6 | Map XForm | 2 | 30 | 23 distinct SRT matrices |
| 7 | Replicate | 7 | 18 | count 0 in 8 (matrix-only transform), 7 in 4, 2..6 in 6 |
| 8 | Greeble | 10 | 14 | seed 0 in 13 / 16 in 1; extrude and taper varied |

**Never used:** `TintMeshShape` (6), `NormalDeform` (8), `CSG` (9), `Invert` (11), `SavePos2` (12).

### 3.4 Minimal set required to render Clean Slate correctly

Exactly the union of the two tables above — nothing more:

**Generators (12):** `Mesh_Cube`, `Mesh_Plane`, `Mesh_Sphere`, `Mesh_Cylinder`, `Mesh_Cone`,
`Mesh_Arc`, `Mesh_Line`, `Mesh_Spline`, `Mesh_Loft`, `Mesh_Copy`, `Mesh_GeoSphere`,
`Mesh_StoredMini`, plus the `Mesh_Clone` container type (needed for 37 objects; it is not a
generator but a scene-graph node), and `Mesh_Merge` (1 instance).
*Not needed:* `Mesh_Stored`, `Mesh_Tree`, `Mesh_TreeLeaves`, `Mesh_Marched`.
Note `Mesh_Spline` (7) is listed as used (3 objects) but `GenerateMesh`'s `case Mesh_Spline` is
**empty/comment-out** (`Project.cpp:1757-1761`: `//m->Mesh.Spline(NULL, 0, Data[0], false);`), so
those 3 objects generate an empty mesh at load time. A port can no-op them; to *reproduce* the demo
you must also produce empty meshes there. **[UNCERTAIN]** — the spline objects may exist purely as
Loft/Scatter paths in the tool; note their `CphxMesh` is never populated at runtime.

**Filters (8):** `ModelFilter_UVMap`, `ModelFilter_MeshSmooth`, `ModelFilter_SmoothGroup`,
`ModelFilter_TintMesh`, `ModelFilter_Bevel`, `ModelFilter_MapXForm`, `ModelFilter_Replicate`,
`ModelFilter_Greeble`.
The project needs these code paths: `CatmullClark`, `Bevel`, `Greeble`/`Greeble_SplitPoly`,
`Replicate`, `MapXForm`, `CalculateTextureCoordinates`, `CalculateTint` (with GPU texture readback),
and the normals/`SmoothGroupSeparation` logic.

Cross-check: the exporter auto-generates a minimal `PhoenixConfig.h` from exactly these usage
counters (`MinimalExport.cpp:3281-3315`) — the same set, confirming this is the intended minimum.
The per-feature Scatter sub-switches it also generates
(`PHX_SCATTER_HAS_VERTEX_SUPPORT` / `_EDGE_` / `_POLY_` / `_POLY_MORE_THAN_1_INSTANCE_` and the four
orientation modes, `MinimalExport.cpp:3309-3314`) are driven by Scatter's own parameters. Decoding
all 8 Scatter objects from the apx:

| vxprob | edgeprob | polyprob | orientation | OffsetThreshold | objects |
|---|---|---|---|---|---|
| 255 (=1.0) | 0 | 0 | 1 (`Normal`) | 0 | 4 |
| 255 (=1.0) | 0 | 0 | 0 (`Original`) | 0 | 1 |
| 0 | 255 (=1.0) | 0 | 1 (`Normal`) | 0 | 3 |

All 8 have `randseed = 0`, `maxperpoly = 0`, `probtint = 0`, `scalethreshold = 0`
(so `1 − ScaleThreshold = 1` ⇒ **no random scale**), `forceyscale = 0`, `scaletint = 0` and
`OffsetThreshold = 0`. So Clean Slate needs **vertex *and* edge** Scatter support (both at
probability 1.0) with orientation modes `Normal` and `Original`, and never needs the polygon pass,
the multi-instance-per-polygon pass, or any random scale — but a general port should still implement
all of them.

---

## 4. Algorithm reference (enough to reimplement each op)

### 4.0 Shared substrate (read this first — every op builds on it)

```c
// Mesh.h:136-154
class CphxVertex {
  D3DXVECTOR3 Position, Position2, StoredPosition;
  D3DXVECTOR3 Normal;
  float Color[4];      // accumulation buffer, 0..1
  int ColorCount;      // how many times Color was accumulated (used for averaging / weights)
  CphxArray<int> EdgeIDs;
};
// Mesh.h:171-184
class CphxPolygon {
  int VertexCount;     // 3 or 4 in practice
  int VertexIDs[4];
  int EdgeIDs[4];      // parallel to VertexIDs: edge (y → y+1)
  D3DXVECTOR3 PolyNormal;
  D3DXVECTOR3 Normals[4];        // per-corner normals; THIS is what reaches the GPU
  D3DXVECTOR2 Texcoords[4][4];   // [corner][channel] — 4 UV channels
  bool TouchedByNormalCalculator; // reused as "was split" by Greeble
};
```

* `AddVertex(x,y,z)` sets `Position = Position2 = (x,y,z)` (`Mesh.cpp:89-101`).
* `AddPolygon(a,b,c,d,...)` stores `VertexCount = (c==d) ? 3 : 4` — the idiom used everywhere to
  emit a triangle is to pass the last vertex twice (`Mesh.cpp:113-130`). Passing `d == c` means a
  triangle; `a,b,c,c`.
* **`RebuildEdgeList(BuildVertexEdgeList)`** (`Mesh.cpp:152-251`): builds a temporary
  `{min(vx1,vx2), max(vx1,vx2), sourcePoly, sourceVertex}` array (one per polygon corner), sorts it
  with `qsort`/`EdgeSorter` (`Mesh.cpp:143-150`), then collapses adjacent duplicates into
  `CphxEdge{VertexIDs[2], PolyIDs[2]}`; the second occurrence of a pair sets `PolyIDs[1]`,
  otherwise `PolyIDs[1] = -1` (open edge). `Polygons[p].EdgeIDs[corner]` is filled in. If
  `BuildVertexEdgeList` is set, each vertex also gets the list of incident edges in
  `Vertices[v].EdgeIDs`. **This is the only topology builder and it requires a manifold-ish mesh.**
* **`CalculateNormals()`** (`Mesh.cpp:318-366`) — called by `BuildMesh` (`Mesh.cpp:370`), by
  `Bevel`, `NormalDeform`, `Scatter` and `CalculateTextureCoordinates`:
  1. `RebuildEdgeList()`; **if `SkipNormalCalculation` → return immediately** (line 322), leaving
     `PolyNormal`/`Normals` untouched (used by stored/CSG/marching meshes).
  2. Reset vertex normals; `PolyNormal = CalculatePolyNormal(p)`, which for each corner `x` uses
     `A = P[(x+1)%n]`, `B = P[x]`, `C = P[(x+2)%n]` and accumulates
     `normalize(A−C) × normalize(A−B)` (`D3DXVec3Cross(&a,&b,&a)` with `a = A−B`, `b = A−C`,
     `Mesh.cpp:262-279`; note the commented-out alternative on line 273 is the negated order).
     The result is a **corner sum and is never normalised** as a face normal. Each `PolyNormal` is
     then added to its vertices, and the vertex normals are normalised (`Mesh.cpp:328-339`).
  3. Per corner, choose the shading model from `SmoothGroupSeparation` (`Mesh.h:218`,
     `DEFAULTSMOOTHGROUPSEPARATION 0`):
     * `== 0` → **flat**: corner normal = `PolyNormal` (line 350).
     * `< 2` → **angle-based smoothing**: flood-fill across edges whose
       `1 − dot(n1,n2) <= SmoothGroupSeparation` via recursive `CalculatePolyVertexNormal`
       (`Mesh.cpp:286-316`), accumulating poly normals; result normalised (lines 353-361).
     * `>= 2` → **Gouraud**: corner normal = averaged vertex normal (line 363).
     The `SmoothGroup` filter sets this to `param/255*2` (`Project.cpp:929-933`), so 255 → 2.0 →
     Gouraud, 128 → ~1.0 → angle threshold ~57°, 0 → flat. Default (no filter) is **flat**.
     **Order matters**: `CalculateTextureCoordinates` (the `UV Map` filter) also calls
     `CalculateNormals`, but `BuildMesh` recomputes at the end with the final setting unless the
     generator set `SkipNormalCalculation` (built by `LoadStoredMesh`, `CreateMarchingMesh`, `CSG`;
     cleared by `Bevel` and by the `SmoothGroup` filter).
* Every vertex is duplicated per corner at `BuildMesh` time and `Normal` comes from
  `p->Normals[corner]` (`Mesh.cpp:410-414`) — so "smoothing" is expressed entirely through
  `Normals[]`, not through shared indices.

### 4.1 Generators (`GenerateMesh`, `Project.cpp:1702-1848`)

All primitive builders live in unit space (a diameter-1 or 1×1 shape) and rely on the object matrix
for scale, so a port must preserve the exact unit conventions below.

**`CreateCube()` — `Mesh.cpp:685-700`** (no params). 8 hard-coded vertices at ±0.5
(`CubeVertexData`, `Mesh.cpp:663-673`) and 6 hard-coded quads (`CubePolyData`, `Mesh.cpp:675-683`),
emitted as `AddPolygon(data[3], data[2], data[1], data[0])` — i.e. **each quad is reversed** when
transferred. Then `CalculateTextureCoordinates(flareTextureMap_Box, ch)` for all 4 channels.

**`AddCapsBuildCylindricalTopologyAndCalculateUV(Cap1Y, Cap2Y, XRes, YRes, flagA, flagB, MapType)`
— `Mesh.cpp:703-728`** — the shared helper for sphere/cone/cylinder. **[QUIRK]** the two `bool`
parameters are named `TopCap, BottomCap` in the header (`Mesh.h:215`) but `BottomCap, TopCap` in the
definition (`Mesh.cpp:703`) — the names are contradictory; only the mechanics below matter:
1. `vxc = Vertices.NumItems()`; append 2 centre vertices at `(0,Cap1Y,0)` → index `vxc`, and
   `(0,Cap2Y,0)` → index `vxc+1`.
2. `if (flagA)` fan `XRes` triangles **from `vxc+1` to the most recently added ring**
   (`vxc - (x+1)%XRes - 1`, `vxc - x - 1`).
3. `if (flagB)` fan `XRes` triangles **from `vxc` to the first added ring** (`(x+1)%XRes`, `x`).
4. Side quads for `y in 0..YRes-2`, `x in 0..XRes-1`, wrapping in both directions
   (`(x+1)%XRes`, `(y+1)%YRes`) — the caller must therefore pass `YRes = rings+1`.
5. All 4 UV channels are generated with `MapType`.
   So: **flagA caps the last ring at height `Cap2Y`, flagB caps the first ring at height `Cap1Y`.**
   A port must reproduce this exactly, including the cross-wired-looking extra conditions at the
   sphere call site below.

**`CreateSphere(XRes, YRes, TopCut, BottomCut, Caps)` — `Mesh.cpp:731-749`.**
`GenerateMesh` passes `TopCut=(Data[2]+1)/256.0f`, `BottomCut=Data[3]/256.0f`, `Caps=Data[4]!=0`
(`Project.cpp:1732-1736`). Vertices: `theta = (lerp(BottomCut,TopCut, y/(YRes−1)) − 0.5)*π`,
`phi = x/XRes*2π`, `pos = (cosθ·sinφ, sinθ, cosθ·cosφ)*0.5`, for rows
`y = v1 .. YRes−v2−1` where `v1 = (BottomCut>0) ? 0 : 1` and `v2 = (TopCut<1) ? 0 : 1`.
So `BottomCut == 0` **drops** the first (pole) row and `TopCut == 1` **drops** the last (pole) row;
a non-zero/non-one cut *keeps* a cut ring at that height. `YRes` is bumped by 1 if the row count
would be zero. The helper is then called as
`AddCaps( Cap1Y = sin((BottomCut−0.5)π)·0.5, Cap2Y = sin((TopCut−0.5)π)·0.5, XRes, YRes−v1−v2,
flagA = Caps || v1==1, flagB = Caps || v2==1, Spherical )` — i.e. **[QUIRK]** the flag that caps the
*last* ring is forced by `v1` (the *first* row having been dropped) and vice versa. For a full
sphere (`v1 = v2 = 1`) both forces fire and the result is closed; for partial cuts with `Caps = 0`
the pairing is what decides whether the shell is left open, so **port the flags verbatim**.

**`CreatePlane(XRes, YRes)` — `Mesh.cpp:753-768`.** `(XRes+1)×(YRes+1)` vertices at
`(x/XRes−0.5, 0, y/YRes−0.5)`; quads with the winding
`(x+(y+1)*W, x+1+(y+1)*W, x+1+y*W, x+y*W)` where `W = XRes+1`; planar UVs on all channels.

**`CreateCone(XRes, YRes, TopCut, Caps)` — `Mesh.cpp:772-788`.**
`GenerateMesh` passes `TopCut=(Data[2]+1)/256`, `Caps=Data[3]!=0` (`Project.cpp:1742-1746`).
`v2 = TopCut<1 ? 1 : 0`; rings `y in 0..YRes+v2−1` with `h = y/YRes*TopCut`, `r = 1−h`,
`pos = (sin t·0.5r, h−0.5, cos t·0.5r)`; then the helper with `Cap1Y=−0.5`,
`Cap2Y=TopCut−0.5`, `flagA(2nd centre)=Caps`, `flagB(1st centre)=Caps || v2==0`, cylindrical UVs
(so the base is capped whenever the tip is a real tip rather than a cut disc).

**`CreateCylinder(XRes, YRes, Caps)` — `Mesh.cpp:792-802`.**
`(YRes+1)` rings of `XRes` vertices `(sin t·0.5, y/YRes−0.5, cos t·0.5)`; helper with
`(−0.5, 0.5, XRes, YRes+1, Caps, Caps)`.

**`CreateLine(Res)` — `Mesh.cpp:806-830`.** `Res` vertices along z from −0.5 to +0.5. Then it emits
**degenerate quads** `AddPolygon(x, x+1, x, x+1)` (`VertexCount=4`, so 2 zero-area triangles) with
`Texcoords[0][*].x = x/Res`, `Texcoords[1][*].x = (x+1)/Res`. Sets `ArcStartDir = ArcEndDir = z`.
Its purpose is to act as a **path/curve provider for Loft** and as a tool-visible gizmo; in the
runtime, Line/Spline/Arc objects get no material (`Project.cpp:814-826`) so their degenerate
triangles are never drawn. A port must still generate the *vertices* (Loft reads them) but can skip
the degenerate quads — except that `BuildMesh` counts them into `TriCount`, and the object may be
referenced by Loft. Reproduce them for exactness.

**`CreateArc(Res, Degree, HaveLastSegment)` — `Mesh.cpp:834-865`.**
`GenerateMesh` passes `Degree = Data[1]/255.0f` (`Project.cpp:1747-1751`). `r = Res + (HaveLastSegment?1:0)`;
`ArcStartDir = z`; `phase = (r−1)*Degree*2π/Res`; `ArcEndDir = (−sin phase, 0, cos phase)`;
vertices at `(0.5 cos(y*Degree*2π/Res), 0, 0.5 sin(...))`; same degenerate-quad gizmo pattern with
`Texcoords` x = `x/r` and `(x+1)/r`.

**`CreateGeoSphere(Iterations)` — `Mesh.cpp:1439-1478`.**
Start from the 12-vertex / 20-triangle icosahedron (`IcosaVertexData` `Mesh.cpp:1399-1413`,
`IcosaPolyData` `Mesh.cpp:1415-1437`). Per iteration: `RebuildEdgeList()`; for every edge add a new
vertex `normalize(v0+v1)/2` (note: normalise then halve → still on the unit-radius-0.5 sphere);
then walk polygons **backwards** (`for z = Polygons.NumItems()−1; z >= 0; z--`) and for each of the
3 corners add an edge-corner triangle and rewrite the original corner to the new midpoint
(`Mesh.cpp:1466-1473`). Result: 4× triangles per iteration. Finally spherical UVs.
`Iterations` comes from `Data[0]`; Clean Slate uses 0..5.

### 4.2 Path-based ops

**`Spline(Keys, KeyCount, Resolution, Loop)` — `Mesh.cpp:869-902`.**
`CphxMeshSplineKey` (`Mesh.h:203-206`) = `vx[3]` + `Front[3]` + `Back[3]` as **half floats**.
Algorithm: for each segment `x`, build the cubic Bézier control points
`k0 = Keys[x].vx`, `k1 = k0 + Keys[x].Front`, `k3 = Keys[x+1 mod n].vx`,
`k2 = k3 + Keys[x+1 mod n].Back`, then sample `Resolution` points (the last segment of a
non-looping spline gets `Resolution+1`) with `bezier(...)` (from `phxMath.h`). `ArcStartDir`/`ArcEndDir`
are set from `normalize(Keys[0].Front)` and `normalize(Keys[KeyCnt%KeyCount].Back)` so a Loft using
this path gets correct end frames (lines 874-875). Then it emits the same degenerate-quad gizmo
pattern (line 897-898).
**At runtime this never runs** — `GenerateMesh`'s `case Mesh_Spline` is commented out
(`Project.cpp:1757-1761`). Keys are also not exported (`MinimalExport.cpp:1022-1023`: "todo: spline
export"). So a runtime port only needs the *empty mesh* behaviour; the real spline geometry exists
only inside the tool. **[UNCERTAIN]** whether the 3 `Mesh_Spline` objects in Clean Slate are used
purely as tool-side references (e.g. Loft/Scatter paths) — the runtime has no data to build them.

**`Loft(Path, PathPos, Slice, SlicePos, PathClosed, SliceClosed, Rotation, StartScale, EndScale)`
— `Mesh.cpp:906-977`.** `GenerateMesh` passes parents `p[0]` = Path, `p[1]` = Slice and
`PathClosed=Data[2]!=0`, `SliceClosed=Data[3]!=0`, `Rotation = Data[4] ? (Data[4]+1)/32 : 0`,
`StartScale = Data[5]/255`, `EndScale = Data[6]/255` (`Project.cpp:1762-1774`).
It sweeps the slice's **vertices** along the path's **vertices** using a rotation-minimising frame:
1. `InvTransp = transpose(inverse(PathPos))` (for transforming stored arc directions into path space).
2. For each path vertex `x`, `dir = normalize(transform(V[x+1]) − transform(V[x−1]))` (centred
   difference); if `!PathClosed` and `x` is the first/last vertex, `dir` is replaced by
   `Path->ArcStartDir` / `Path->ArcEndDir` respectively (this is why `CreateArc`/`CreateLine`
   store them), transformed by `InvTransp`.
3. `nx = normalize(up × dir)`; `up = normalize(nx × dir) * -1`; then the slice vertex is placed as
   `((nx·v.x) + (up·v.z)) * lerp(StartScale,EndScale, x/(PathVxCount−1)) + pathVertex`
   (lines 939-953). The slice is first rotated about `up` by
   `x/PathVxCount * 2π * Rotation` (`Mesh.cpp:944-947`).
4. Quads are then generated for
   `x in 0..SliceVxCount−1+(SliceClosed?1:0)`, `y in 0..PathVxCount−1+(PathClosed?1:0)` with
   UVs `u = x/SliceVxCount`, `v = y/PathVxCount` — note **UVs are derived from the counts, not from
   arc length** (lines 958-975).
**[QUIRK]** the whole construction is wrapped in `for (int z = 0; z < 101; z++)` and only the
`z == 100` iteration emits vertices (`Mesh.cpp:920`, `944`). The first 100 iterations only advance
the `up` frame, and because `dir` does not depend on `z`, the frame converges after one iteration —
so the loop is a size-optimisation artefact. A port can compute the frame once and emit directly;
**but the endpoint override (`if (z && ...)`, line 929) only triggers for `z != 0`, so a
single-pass port must replicate the final-iteration values** (i.e. apply the arc-direction override
unconditionally for the endpoints).

### 4.3 Composition and instancing

**`CopyInstance(CphxMesh* a, D3DXMATRIX* T)` — `Mesh.cpp:1673-1690`** — the shared instancing
primitive: appends every vertex of `a` (transforming `Position` by `T` via
`D3DXVec3Transform3`, i.e. a 4x4 multiply with implicit w=1) and sets `Position2 = transform(origin)`
(i.e. **Position2 is set to the transformed origin**, `Mesh.cpp:1681` — meaning "Position2" carries
the instance origin for these meshes); then appends all polygons with `VertexIDs += VxCount`.

**`Copy(CphxMesh* a)` — `Mesh.cpp:981-985`** — `CopyInstance(a, identity)`. Used by `Mesh_Copy`
(1 parent, `Project.cpp:1825-1827`). Clean Slate: 6 objects.

**`Merge(CphxModelObject_Mesh** objects, int count)` — `Mesh.cpp:989-993`** — calls
`CopyInstance(&objects[x]->Mesh, &objects[x]->GetMatrix())` for all parents, i.e. bakes each child's
own object matrix. Used by `Mesh_Merge` with N parents (`Project.cpp:1830-1832`). Clean Slate: 1.

**`Clone` (`CphxModelObject_Clone`)** — no geometry at all; it multiplies its own matrix into the
world matrix and recurses into every referenced object (`Model.cpp:48-55`,
`Project.cpp:861-869`). This is how Clean Slate reuses meshes 37 times without duplicating data:
the runtime creates a *separate* `CphxRenderDataInstance` per clone instance, so the same GPU
buffers are drawn with different `worldmat`.

**`Replicate(unsigned char Count, D3DXFLOAT16* Srt)` — `Mesh.cpp:2023-2068**`
(params: `[0]` = count; extra 24 bytes = 3x4 matrix). Deterministic, **no rand**.
1. Unpack the 3x4 matrix: `Transformation.m[x][y] = Srt[x + (y<<2)]` for `y in 0..2`, `x in 0..3`.
2. Transform the base mesh by it (`Position` and `Position2`).
3. For each of `Count` copies: duplicate all vertices and polygons, transform the copy by the
   *current* matrix, then `transform = transform * Transformation` (so copies compound:
   `T, T², T³, …` — `Mesh.cpp:2066`).

**`Scatter(...)` — `Mesh.cpp:1551-1669`** (14 params, 2 parents: `Object` = the thing to place,
`Shape` = the surface to place on). Params from `Project.cpp:1775-1793`:
`Data[2]=RandSeed`, `Data[3]/255=VertexProbability`, `Data[4]/255=EdgeProbability`,
`Data[5]/255=PolyProbability`, `Data[6]=MaxPerPoly`, `Data[7]=ProbabilityTint`,
`Data[8]=Orientation (PHXSCATTERORIENTATIONTYPE)`, `Data[9]/255=ScaleThreshold`,
`Data[10]!=0=ForceYScale`, `Data[11]=ScaleOffsetTint`, and the 2-byte float
`*(D3DXFLOAT16*)&Data[12]` = `OffsetThreshold`.
**It is the primary randomness consumer**: `srand(RandSeed)` (`Mesh.cpp:1554`) then global `rand()`.
Steps:
1. `Shape->CalculateNormals()`; build `InvTransp = transpose(inverse(ShapePos))`;
   `Shape->RebuildEdgeList(VertexProbability > 0)`.
2. **Vertex pass** (`Mesh.cpp:1569-1585`): for every shape vertex, `up` = direction of its first
   incident edge (or +Y); build the instance matrix with `GetScatterTransformation(...)`; accept the
   instance if `rand()/RAND_MAX < VertexProbability*(1 − v.Color[ProbabilityTint])`.
3. **Edge pass** (`1590-1619`): midpoint of each edge, normal = average of the two adjacent polygon
   normals, `up` = the edge vector, extra scaling `(1,1,|edge|)`; accept if
   `rand() < EdgeProbability*(1 − avg tint)`. (`up` is deliberately *not* normalised — the raw edge
   vector is used, so its length doubles as the scale in the frame construction.)
4. **Poly pass** (`1624-1666`): polygon centroid, `PolyNormal`, `up` = first-edge direction; accept
   if `rand() < PolyProbability*(1 − avg tint)`, plus `rand()*(MaxPerPoly)` extra barycentric
   samples.
`GetScatterTransformation` (`Mesh.cpp:1482-1549`) builds the frame `x = normalize(normal × up)`,
`y = normal`, `z = x × normal`; scale
`fScale = lerp(S, 1/S, rand()/RAND_MAX * Tint)` where the callers pass
**`S = 1 − ScaleThreshold` and `Tint = 1 − Color[ScaleOffsetTint]`** (`Mesh.cpp:1581`, `1614`,
`1643`) — i.e. the raw filter parameter `Data[9]/255` is inverted before use, so `Data[9] = 0`
means `S = 1` and therefore **no random scale at all** (which is what Clean Slate does);
`ForceYScale` pins x/z to 1; rotation angle
`rand()` about +Y (or a random sphere axis for `FullRotate`); offset along the normal by
`OffsetThreshold * rand() * Tint` — so `OffsetThreshold` (the object's `<floatparameter>` half float)
is the *maximum offset distance* despite its name; final matrix
`ObjPos * (T * frame) * translate(pos)`; for
`flareScatterOrientation_Original` the frame is dropped and only `T` survives
(`Mesh.cpp:1540-1543`).

> **Determinism / porting note.** `Scatter` and `Greeble` are the only two ops that consume
> randomness, both via the **global C `srand`/`rand`**. Because `GetScatterTransformation` calls
> `rand()` several times *before* the caller's probability test, the exact number and order of
> `rand()` calls (including for instances that are then discarded, and including the
> `#ifdef`-selected orientation branch) determines every downstream result. A Vulkan port that wants
> bit-identical output must use the **MSVC `rand()` LCG** (`state = state*214013 + 2531011;
> return (state>>16) & 0x7fff;`) seeded with `srand(seed)`, and must preserve the call order exactly.
> Any other RNG or any early-out will change the artwork.

### 4.4 Surface / topology ops

**`Bevel(float Percent)` — `Mesh.cpp:1282-1391`** (params: `[0]/255`). Deterministic, no rand.
1. Save/force `SmoothGroupSeparation = 2`, `SkipNormalCalculation = false`, call
   `CalculateNormals()`, restore the separation, then build the per-vertex edge lists
   (lines 1284-1297).
2. For each polygon: compute its centroid; for each corner create a **shrunk** copy
   `nv = (V − center)*(1−Percent) + center`; store that new vertex; for each of the corner's two
   incident edges, record which "side" of the edge the shrunk vertex lies on using
   `cross(edgeDir, nv − edgeV0) · PolyNormal` and the edge's vertex order, writing into
   `edge->NewVertexIDs[edgeEnd][polySide]` (lines 1309-1350). Each original vertex accumulates the
   shrunk positions and a `ColorCount`, then is moved to their average (line 1354-1355).
3. New "edge quads" bridge the two shrunk corners of every edge that got all 4 slots filled
   (lines 1358-1363).
4. "Corner triangles" fan each original vertex to its two shrunk corners, with the winding chosen
   from `cross(v1,v2)·vertexNormal` (lines 1365-1389).
Result: every face is inset and a rounded corner/edge band is added — the classic "bevel by
insetting faces" construction. **`Percent` is the inset fraction, so 0 = no-op, ~0.5 = heavy.**

**`CatmullClark(bool Linear, int Iterations)` — `Mesh.cpp:521-644`** — the `MeshSmooth` filter,
params `[0]` = linear flag, `[1]` = iterations. Deterministic, no rand. Per iteration:
1. `RebuildEdgeList(true)` (vertex→edge lists needed).
2. Append a **face point** per polygon = average of its vertices (lines 531-542). Face points land at
   index `OriginalVertexCount + polyIndex` — later code depends on that exact layout.
3. Append an **edge point** per edge: `Linear` (or a boundary edge, `PolyIDs[1] == -1`) → midpoint;
   otherwise the average of the two endpoints and the two adjacent face points (lines 545-556).
   Edge points land at `OriginalVertexCount + OriginalPolyCount + edgeIndex`.
4. Snapshot all positions into `StoredPosition` (lines 559-563).
5. For each original vertex (unless `Linear`): accumulate
   `R2F = Σ_edges [0.5*(F_a + F_b) + (V_a + V_b)]`, skipping vertices with any open edge (they are
   left where they are), then
   `Position = (R2F/edgeCnt + (edgeCnt − 3)*storedPos) / edgeCnt` (lines 566-600).
   (`edgeCnt − 3` is the classic `(n−3)/n` weight written in a rearranged form.)
6. Rebuild polygons: each original n-gon becomes n quads
   `(facePoint, edgePoint[y−1], originalCorner[y], edgePoint[y])`; the last quad *replaces* the
   original polygon and earlier ones are appended (lines 603-639). UVs are interpolated
   correspondingly: the face point gets the polygon-average UV, the two edge points get the
   adjacent-corner averages, and the corner keeps its own UV (lines 621-630).
Normals/edge lists are intentionally invalidated at the end (comment `Mesh.cpp:642`); `BuildMesh`
recomputes them.

**`NormalDeform(float factor)` — `Mesh.cpp:649-658`** (params: `[0]/255 * ([1] − 127)`).
Deterministic. `CalculateNormals()`, then `Position += Normal * factor * Color[0]` per vertex.
So the vertex colour channel 0 is the per-vertex displacement weight — normally fed by a preceding
`Tint`/`TintShape` filter.

**`Greeble(int seed, float extrude, float taper)` — `Mesh.cpp:2784-2879`** (params: `[0]`=seed,
`[1]/255`=extrude, `[2]/255`=taper). **Uses `srand(seed)` + `rand()`** (`Mesh.cpp:2786`).
1. Clear the "is split" flag on all polygons.
2. **Split pass, two rounds** (`for y in 0..1`, lines 2796-2805): for the original polygons and then
   for the first level of newly created ones, every polygon with `rand() & 1` is split by
   `Greeble_SplitPoly`.
   `Greeble_SplitPoly` (lines 2736-2782) picks `startEdge = rand() % VertexCount` (`:2747`), inserts
   the midpoints of the `startEdge` and `startEdge−2` edges (two `Vertices.Add` with averaged
   colour/UV), and re-emits the polygon as two polygons sharing the new edge; triangles are handled
   as a degenerate quad (parameters passed twice, line 2776). The original polygon is left in the
   array and later discarded via the `TouchedByNormalCalculator` flag.
3. **Drop** every polygon whose flag is set (lines 2809-2815).
4. **Extrude pass** (lines 2821-2877): per remaining polygon, `dist = (rand()&0xff)/255 * extrude`;
   compute the centroid (position and UV); inset the new cap vertices toward the centroid by
   `taper` (`(V − center)/(1/taper) + center`), push them along `normalize(CalculatePolyNormal(x))`
   by `dist`, rescale their UVs by `di/(cd+di)` (a cheap approximation of the side-wall UV),
   emit the side wall as `VertexCount` quads, then repoint the original polygon at the new cap.
   So **`taper` scales the cap: 1 = straight extrusion, <1 = tapering inward**. `taper = 0` is used
   by one enabled Clean Slate filter (seed 0, extrude 177, taper 0) and is **not** a NaN trap:
   `(V − center) / (1/0)` = `finite / +inf` = 0, so every cap vertex lands exactly on
   `center + normal*dist` and the cap degenerates into a single spike point per face; the UV blend
   factor `di/(cd+di)` then becomes 1, i.e. the whole cap takes the centroid UV (verified by
   evaluating the arithmetic at `Mesh.cpp:2853-2865`).
5. `Greeble` never calls `CalculateNormals`, so normals come from the following `BuildMesh`.

**`Invert()` — `Mesh.cpp:2882-2894`** — reverses each polygon's `VertexIDs` in place. Dead at
runtime (§2.5.1).

**`SavePos2()` — `Mesh.cpp:2898-2902`** — `Position2 = Position`. Dead at runtime.

### 4.5 UV and vertex-colour ops

**`CalculateTextureCoordinates(Type, Channel, clipUV, Scale, Rotate, Translate)` — `Mesh.cpp:1002-1113`**
(the `UV Map` filter; `Project.cpp:909-915` passes `Type = params[0]`, `Channel = params[1] & 0x0f`,
`clipUV = (params[1] & 0xf0) != 0`, and the SRT transform as
`Scale+1`, `Rotate`, `Translate` — note the `+1` on scale at `Project.cpp:912`).
1. `CalculateNormals()`.
2. Build `Transform = inverse(transformMatrix(Scale, Rotate, Translate))` and use it for both
   positions and normals — i.e. **UVs are computed in the transform's space**, which is what makes
   the `Map XForm`-style SRT meaningful (rotating/translating the projection).
3. Per polygon, per corner, with `vx = Transform*Position` and `vn = normalize(vx)`,
   `pn = Transform*PolyNormal`, `xd/yd/zd = pn.{x,y,z}/|pn.{x,y,z}|` and
   `tanval = −atan2(vn.x, vn.z)/π/2 + 0.5`:
   * `Planar (0)`: `uv = (vx.x + 0.5, vx.z + 0.5)`
   * `Spherical (1)`: `uv = (−tanval, acos(vn.y)/π)`
   * `Cylindrical (2)`: `uv = (−tanval, −vx.y + 0.5)`; if the face is *not* mostly ±Y, stop here,
     otherwise **fall through to Box**
   * `Box (3)`: pick the dominant axis of `pn` and use `(vx.x·zd, −vx.y)` for X-dominant,
     `(−vx.z·xd, −vx.y)` for Z-dominant (the code's `if/else` chain tests `_ax > _ay > _az`),
     `(vx.x·yd, vx.z)` for Y-dominant, then `+= (0.5, 0.5)`
   * `TransformOriginal (4)`: scales/offsets the *existing* UV with `Scale.x/z` and `Translate.x/z`
     (line 1079-1080)
4. Second pass: the classic **cylindrical seam fix** — if the polygon has corners on both sides of
   the ±X split (`XOver0 && XBelow0 && ZBelow0`) and this corner has `vx.x < 0`, add 1.0 to `u`
   (line 1091). Then, if `PHX_MESH_UV_HAS_CLIPPING` is defined and `clipUV` was requested, clamp
   `uv` to [0,1] (lines 1093-1100). **Clean Slate never enables clipping** (§3.3).
5. Third pass: if a spherical pole made `vn.x == vn.z == 0`, the tangent was flagged and the
   generic corner is re-derived as the average of the other corners' `u` (lines 1104-1110).
`Type`/`Channel`/`clipUV` are all fully deterministic.

**`CalculateTint(int TextureChannel, ID3D11Texture2D* SrcTexture, int Saturation)` — `Mesh.cpp:1121-1197`**
(the `Tint` filter, params `[0]` = UV channel, `[1]` = saturation). **Requires a GPU readback**:
1. `D3DX11SaveTextureToMemory(phxContext, SrcTexture, D3DX11_IFF_DDS, &Blob, NULL)` — the texture is
   the cached result of a texture operator (`Project.cpp:934-937` uses
   `TextureOperators[opIndex].CachedResult->Texture`).
2. Parse a 128-byte DDS header by hand (`DDSHEAD`, lines 1130-1161) and treat the payload as
   **RGBA16F half floats** (`unsigned short* inimg`; the `p` index stride of 4 implies 4 channels).
3. Zero all vertex colours (`Color[4]`, `ColorCount`), then for every polygon corner, sample the
   texel at `(u*width mod width, v*height mod height)` — note the modulo makes it tile — and add
   `(int)(half/256)/256.0f` per channel to the **vertex** colour, incrementing `ColorCount`.
4. Average per vertex (`/= ColorCount`) and apply saturation:
   `Color = min(1, Color * (1 + Saturation))` (line 1192).
All 33 Clean Slate Tint filters use channel 0 and **saturation 0**, so the value is the raw
averaged texel. Deterministic given the texture contents, but **[UNCERTAIN]** as to cross-driver
bit-exactness of the readback (the ping-pong target's format/rounding is driver-dependent).

**`CalculateTintShape(shape, op, power, Scale, Rotate, Translate)` — `Mesh.cpp:1201-1259**`
(params `[0]`=shape, `[1]`=op, `[2]`=power + the 20-byte SRT). Deterministic.
`m = inverse(transform(1+Scale, Rotate, Translate))`, `pos = m*Position`;
`val = 1 − max(|x|,|y|,|z|)*2` for shape 0 (box), `1 − |pos|*2` for shape 1 (sphere), `pos.y` for
shape 2 (plane); `val /= power/16.0f`; then per channel apply op `0=set, 1=add, 2=subtract,
3=multiply` and clamp to [0,1]. Not used by Clean Slate.

**`MapXForm(Scale, Rotate, Translate)` — `Mesh.cpp:1263-1278**` (the `Map XForm` filter; no params,
SRT from the filter transform). Deterministic. For each vertex, `col = Color[0]` acts as the blend
weight: build `m = transform(1 + Scale*col, rotation(Axis, Angle*col), Translate*col)` where
`(Axis,Angle)` is the SRT quaternion decomposed, then `Position = m*Position`. So **it is a
per-vertex weighted affine deformation driven by the vertex colour** — the standard idiom is
`TintShape`/`Tint` (or a Gradient texture op baked into the colour) followed by `MapXForm` to bend a
cylinder into a curve. Clean Slate uses 30 of these with 23 distinct matrices.

### 4.6 Volume and boolean ops

**`CreateMarchingMesh(dimensions, objCount, objType, objPositions, surface, resolution)` —
`Mesh.cpp:2073-2226`** (marching *tetrahedra*; `objType` is declared but never read).
1. Grid of `resolution³` points (cubic only, `res[0]=res[1]=res[2]=resolution`), each at
   `((p/res)*2 − 1) * dimensions` (line 2138).
2. Scalar field: `Value = Σ_j 1/|objPositions[j] * pos|` — an inverse-distance metaball sum
   (line 2140-2145); `Inside = Value < surface`.
3. Normals by central differences on interior points only (lines 2150-2158).
4. For every interior cell, gather 8 corners, split the cube into the 6 tetrahedra listed in
   `Tetraeders[6][4]` (lines 2086-2095), and for each tetrahedron compute the 4-bit inside mask,
   look it up in `TetraMap[16][4]` (lines 2097-2113), interpolate the intersection points on the
   crossing edges (`t = (surface − v1)/(v2 − v1)`, lerping position and normal), emit the resulting
   triangle or quad, and set the vertex normals from the interpolated field normals.
5. `SkipNormalCalculation = true` (line 2225) so the interpolated normals are what reaches the GPU.
**[QUIRK] This function is broken and should be re-implemented, not transcribed:**
* line 2154 computes the grid index as `x*res[0] + y*res[1] + z*res[2]` (a cubic multiple of
  `x+y+z`) instead of the linear `x + y*res[0] + z*res[0]*res[1]` used by the main loop
  (line 2133-2135) — so the normal pass writes to the wrong points;
* line 2166 builds the corner offset as `(i&1) + (i&2)*res[0] + (i&4)*res[1]`, doubling terms
  instead of selecting bits, so the cube corners are not the 8 neighbours of the cell;
* lines 2111-2113 intend to fill `TetraMap[8..15]` with the reversed-winding variants but assign
  `TetraMap[15−x] = TetraMap[x]`, duplicating the same winding instead of reversing it.
Not used by Clean Slate (no `Mesh_Marched` objects, and `GenerateMesh` has no case for it).

**`CSG(CphxMesh* object, D3DXMATRIX* objPos, int operation)` — `Mesh.cpp:2606-2730`**
(the `Bool` filter; params `[0]` = other object index, `[1]` = operation).
A faithful port of the csg.js / Evan Wallace BSP algorithm:
* `CSGPlane{normal,w}`, `CSGPoly` carrying `vertexIDs`, per-corner `{uv[4], vertNormal}` and a plane,
  `csgEpsilon = 0.0001` (lines 2232-2314).
* `SplitPoly` (lines 2316-2433) classifies each vertex as COPLANAR/FRONT/BACK, and for SPANNING
  polygons splits on the plane, **interpolating all 4 UV channels and the per-corner normal**
  (lines 2367-2376) and appending the new vertex to the shared vertex pool.
* `CSGMesh` (2436-2483) copies the source vertices and applies `objPos`, transforms the normals by
  `transpose(inverse(objPos))`, and **triangulates every input polygon** as `(0,1,2)` plus `(0,2,3)`
  for quads — a different triangulation from `BuildMesh`'s `(0,2,1)`/`(0,3,2)`.
* `BSPNode` (2485-2604) with `Build`, `Invert` (flip all polys, flip the plane, swap the children),
  `ClipTo`, `ClipPolygons` and `AllPolygons`.
* The op sequences for union / difference / intersection are exactly csg.js
  (lines 2623-2655).
* The result is assembled as `Vertices = meshA.vertices; Vertices += meshB.vertices;` and triangles
  are re-emitted with `p.vertices == &meshB.vertices` using indices offset by
  `meshA.vertices.NumItems()` (lines 2659-2725); per-corner normals come from the interpolated
  `vertNormal`.
* `SkipNormalCalculation = true` is set on entry (line 2611), so `BuildMesh` does not overwrite the
  CSG normals.
* The filter computes the relative transform as
  `other->GetMatrix() * inverse(this->GetMatrix())` (`Project.cpp:968-975`) so both meshes are
  evaluated in *this* object's local space, and the referenced object is added to the dependency
  set (`Project.cpp:878-882`).
Not used by Clean Slate.

### 4.7 Determinism summary

| Op | Randomness | Notes |
|---|---|---|
| Cube/Plane/Sphere/Cylinder/Cone/Arc/Line/GeoSphere | none | pure |
| Spline / Loft | none | Loft is pure given both parents; spline never runs at runtime |
| Copy / Merge / Clone | none | pure |
| Replicate | none | pure |
| **Scatter** | **global `srand(RandSeed)` + `rand()`** | `Mesh.cpp:1554`; call order is load-bearing |
| Bevel | none | pure |
| MeshSmooth (CatmullClark) | none | pure |
| NormalDeform | none | pure |
| **Greeble** | **global `srand(seed)` + `rand()`** | `Mesh.cpp:2786`, `rand()%VertexCount`, `rand()&1`, `rand()&0xff` |
| UVMap | none | pure |
| Tint | none in C++ | input is a GPU texture readback (driver-dependent rounding) |
| TintShape / MapXForm | none | pure |
| MarchingMesh | none | pure but buggy (above) |
| CSG | none | pure; BSP plane choice = first polygon of the list, so order-dependent |
| Tree / TreeLeaves | **Arbaro's own `randstate` RNG** | `Arbaro.h:155`, `Arbaro.cpp:1072`; seeded from `Seed`, does not touch the C RNG |
| Text | none in C++ | but depends on the OS font rasteriser (see §7) |
| Stored / StoredMini | none | pure |

### 4.8 Trees (Arbaro) — implemented but unused by Clean Slate

* `CreateTree(Seed, levelDensities, Species)` — `Mesh.cpp:1762-1772`: constructs
  `Arbaro::Tree t(Species, Seed)`, `t.make()`, `t.BuildTree(this, levelDensities, treeData)`, then
  `BuildTreeData(branchCount, &dataBuffer, &dataBufferView)`.
* `CreateTreeLeaves` — `Mesh.cpp:1776-1786`: same with `BuildLeaves`.
* Species data comes from `TREESPECIESDESCRIPTOR` records loaded into a `TreeSpecies[]` array
  (`Project.cpp:644-656`) and indexed by `Data[0]`; `Data[1]` is the seed and `Data[2..5]` /
  `Data[2..6]` are the per-level densities (`Project.cpp:1804-1817`).
* `treeData` is a **static 100 000-element `Arbaro::BranchData` array** (`Mesh.cpp:1733`), and
  `BuildTreeData` (lines 1735-1760) creates an **immutable structured buffer + SRV** with
  `StructureByteStride = sizeof(Arbaro::BranchData)` from it. That SRV is stored in
  `CphxMesh::dataBufferView` and copied to the model object's `dataBufferView`
  (`Project.cpp:1006-1007`); it is the per-object "mesh data" SRV that vegetation shaders read.
  (Buffer description: `D3D11_USAGE_IMMUTABLE`, `D3D11_BIND_SHADER_RESOURCE`,
  `D3D11_RESOURCE_MISC_BUFFER_STRUCTURED`, `DXGI_FORMAT_UNKNOWN`, `ElementWidth =
  sizeof(BranchData)`, `NumElements = branchCount`.)
* `TreeGen.h` / `TreeGen.cpp` are **entirely commented out** (`TreeGen.h:11-230`) — an older
  generator, dead code kept for reference.
* None of the 680 Clean Slate objects is a Tree/TreeLeaves, so a port can defer Arbaro entirely.

---

## 5. Stored meshes (`PHX_MESH_LOADSTOREDMESH` / `PHX_MESH_LOADSTOREDMINIMESH`)

There is **no external mesh file and no precalc blob at runtime** — both ops are fully
self-contained in the compiled project blob, which in turn is generated from data embedded in the
`.apx`.

### 5.1 Where the data comes from

1. **Authoring/import.** `apEx/apEx/MeshImport.cpp` (Assimp-based importer) converts a mesh file into
   either
   * a `Mesh_Stored` object (full `CphxVertex`/`CphxPolygon` data, `MeshImport.cpp:150-172`), or
   * a `Mesh_StoredMini` object created when the importer's "miniMesh magic" branch runs
     (`MeshImport.cpp:174-235`), i.e. the compact 8-bit form.
2. **In the `.apx` (text XML).** The tool serialises the data as base64 inside the object
   (`Model_Tool.cpp:800-890`, `479-486`, `588-618`):
   * `Mesh_Stored`: `<Vertices VertexData="base64(float3 pos + float3 normal per vertex)">` and
     `<Polygons PolyCount="N"><Poly3s|Poly4s|… PolyData="base64(StoredPolyVertex[])">` where
     `StoredPolyVertex = { int index; float normal[3]; float uv[2]; }` (`Model_Tool.cpp:784-787`).
   * `Mesh_StoredMini`: `<MiniVerts count="<byte count>" data="base64">` and
     `<MiniTris count="<byte count>" data="base64">`. `count` is a **byte** count, not an element
     count (`Model_Tool.cpp:479-486`; contrast the commented-out variant at `580-586` which used
     `*3`).
3. **In the compiled blob.** `MinimalExport.cpp:2790-2800` writes a **deduplicated mini-mesh table**
   to the head of `Stream_ModelParameters`:
   `WriteByte(miniMeshCount)` then per entry `WriteWord(vxDataSize)`, `WriteWord(polyDataSize)`,
   raw `vxData`, raw `polyData` (both sizes are byte counts; duplicates are detected by
   size+memcmp, `MinimalExport.cpp:2751-2786`). Each `Mesh_StoredMini` object stores only a 1-byte
   index into this table (`MinimalExport.cpp:1058-1063`).
   Full `Mesh_Stored` objects are written inline in `Stream_ModelParameters`
   (`MinimalExport.cpp:1024-1056`): `dword vxCount`, then per vertex 6 floats (pos + normal);
   `dword polyCount`, then per polygon a byte `VertexCount` followed by per corner
   `int index, float3 normal, float2 uv` — exactly the reader at `Project.cpp:743-776`.
4. **At runtime.** The mini-mesh table is read first (`Project.cpp:657-682`) into a
   `MiniMesh miniMeshes[16]` array (note the **hard 16-entry limit**), the objects follow, and
   `GenerateMesh` dispatches:
   * `Mesh_Stored` → `LoadStoredMesh(StoredVertices, StoredVertexCount, StoredPolygons, StoredPolyCount)`
     (`Project.cpp:1794-1797`).
   * `Mesh_StoredMini` → `LoadStoredMiniMesh(StoredVertices /*byte ptr*/, StoredVertexCount/3,
     StoredPolygons /*byte ptr*/, StoredPolyCount/3)` (`Project.cpp:1799-1802`), where the counts
     were set from the mini-mesh *byte* sizes (`Project.cpp:777-785`). The mini-mesh pointer is
     smuggled through the `StoredVertices`/`StoredPolygons` members — the source itself calls this
     "a hack to not need another variable".

### 5.2 `LoadStoredMesh` — `Mesh.cpp:1694-1703`

Straight copy of the vertex and polygon arrays into `CphxMesh`, then `SkipNormalCalculation = true`.
That flag only makes `CalculateNormals` return early (`Mesh.cpp:320-322`); `BuildMesh` still writes
`p->Normals[y]` to the GPU (`Mesh.cpp:412`), and those corner normals came straight from the file —
so the stored normals survive intact. (This is also why `PolyNormal` is never computed for stored
meshes, which is harmless because nothing reads it.)

### 5.3 `LoadStoredMiniMesh` — `Mesh.cpp:1706-1728`

The compact format, exactly:
* `vxDataSize/3` vertices, each **3 bytes = 3 positions in [0,1]**, decoded as
  `(b[0]/255, b[1]/255, b[2]/255)` — i.e. the writer's byte order is the reader's XYZ order.
  However the *importer* writes bytes in **Y, X, Z order** and compensates by setting the object
  matrix to `scale(bbSize.y, bbSize.x, bbSize.z)` (`MeshImport.cpp:196-217`). So the axis swap
  happens in the matrix, not in the data.
* A **mirrored copy** of all vertices is appended with `x` negated (`Mesh.cpp:1710-1711`) — this is
  the "double-sided mini mesh" trick.
* `triDataSize/3` triangles, each 3 bytes of vertex indices. For each triangle **two polygons** are
  added: `(na,nb,nc)` and `(na,nc,nb)`, and before emitting the second one each index is moved into
  the mirrored half **only if the mirror actually differs** (`Vertices[na].Position !=
  Vertices[na+vxc].Position`, `Mesh.cpp:1720-1726`). Net effect: every mini-mesh triangle is drawn
  with both windings, so the mesh is visible from both sides, at the cost of 2× vertices and 2×
  triangles. `SkipNormalCalculation` is **not** set here, so `BuildMesh` recomputes normals — with a
  vertex duplicated as a mirror image the recomputed normals are those of a closed double-sided
  shell. **[UNCERTAIN]** whether that is visually intended or an oversight; the tool preview uses the
  same function (`Model_Tool.cpp:1200-1202`), so at least tool and runtime agree.
* No UVs are produced by this path (the polygons' `Texcoords` are zero), so mini meshes are
  typically used with procedural/planar UVs supplied by a following `UV Map` filter or with
  triplanar shaders.

### 5.4 Clean Slate specifics

3 `Mesh_StoredMini` objects, each with `MiniVerts count=165 data=<165 bytes>` and
`MiniTris count=267 data=<267 bytes>` → 55 vertices / 89 triangles; all three blobs are byte
identical (the exporter would deduplicate them to a single mini-mesh entry, index 0). The 3 decoded
blobs have index max 54 and byte range 0..255 (verified by decoding the base64 directly). There are
**no** `Mesh_Stored` objects in Clean Slate.

---

## 6. GPU upload and drawing

### 6.1 Upload: `CphxMesh::BuildMesh` — `Mesh.cpp:368-518`

Called once per mesh object at load (`Project.cpp:1005`), never per frame.
1. `CalculateNormals()` (`Mesh.cpp:370`); early-out with no buffers if there are no polygons
   (line 373) — this is why Line/Arc/Spline objects have null GPU buffers and must be skipped by the
   renderer (they also get no material, §3.2).
2. Count `VxCount = Σ VertexCount` and `TriCount = Σ (VertexCount−2)` (lines 379-384).
3. Allocate CPU-side `PHXVERTEXDATA[VxCount]`, `int[TriCount*3]` and `int[VxCount*2]`
   (lines 386-391).
4. Fill vertices per polygon corner: `Position`, `Position2`, `Normal = p->Normals[y]`,
   `Color = packed RGBA8`, `memcpy(Texcoord, p->Texcoords[y], sizeof(D3DXVECTOR2)*4)` — copying
   chord 4 UV channels into the `float4` `Texcoord` and leaving **`Texcoord2` (TEXCOORD1) all
   zero** (lines 400-429). **So in practice UV set 1 is always (0,0,0,0)** and the `float4`
   `TEXCOORD0` carries the four UV channels in `.x/.y/.z/.w`. This is the most important
   non-obvious layout fact for a port: `ComputeTextureCoordinates` writes into
   `Texcoords[corner][channel]` for channels 0..3, and channel 1 ends up in `.z`, not in
   `TEXCOORD1`.
5. Fill indices (see §1.5).
6. Create three **immutable** default-usage buffers with `D3D11_SUBRESOURCE_DATA` initial data
   (lines 464-508): vertex buffer, triangle index buffer, wireframe index buffer. No staging, no
   dynamic updates, no per-frame upload — a Vulkan port should use
   `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` + a one-shot staging copy.
   **[QUIRK]** when `EdgeCount == 0` the wire buffer is created with `ByteWidth = 0`, which fails;
   nothing checks the result.

### 6.2 Batching: `CphxRenderDataInstance` — `RenderLayer.h:10-51`

One instance is created per (mesh object × material pass), **per object instance** — clones create
their own instances that share the same buffers (`Material.cpp:117-170`). Fields:

| Field | Meaning |
|---|---|
| `VertexBuffer`, `IndexBuffer`, `WireBuffer` | shared, from the mesh object |
| `TriIndexCount` | **index** count = `TriCount*3` (`Material.cpp:201`, arg named `VertexCount` — misnomer) |
| `WireIndexCount` | **index** count = `EdgeCount*2` |
| `Wireframe` | per-pass flag from the material constant state |
| `Indexed` | false only for CPU particle emitters (`Material.cpp:208`) |
| `VS/PS/GS/HS/DS` | the pass's shaders (`memcpy` of 5 pointers, `Material.cpp:136`) |
| `BlendState/RasterizerState/DepthStencilState/Textures[8]` | `memcpy` of 11 pointers from the material state (`Material.cpp:144`) — order is load-bearing and warned about in the headers |
| `Matrices[2]` | world matrix and inverse-transpose world matrix |
| `MaterialData[MATERIALDATASIZE/4]` | packed material constants (64 floats) |
| `ToolData` | editor association |

Instances are appended to the render layer's list in scene-traversal order
(`Scene.cpp:205-216`) and drawn in that order; `RenderPriority` is stored but **not** used to sort
the runtime queue (`grep RenderPriority` shows only assignment at `Material.cpp:134`).

### 6.3 Draw: `CphxRenderDataInstance::Render()` — `RenderLayer.cpp:27-101`

```c
if (!VS || !PS) return;              // RenderLayer.cpp:29  kill invalid batches
if (!VertexBuffer) return;           // RenderLayer.cpp:30
stride = PHXVERTEXFORMATSIZE;        // RenderLayer.cpp:33  = 72
phxContext->IASetInputLayout(RenderVertexFormat);            // :38
phxContext->IASetVertexBuffers(0, 1, &VertexBuffer, &stride, &offset);  // :47
phxContext->IASetPrimitiveTopology(Wireframe ? LINELIST : TRIANGLELIST); // :48
VSSetShader/GSSetShader/HSSetShader/DSSetShader/PSSetShader   // :55-59
RSSetState(RasterizerState); OMSetBlendState(BlendState,...); OMSetDepthStencilState(...) // :60-62
Map(ObjectMatrixBuffer, WRITE_DISCARD); 
  memcpy(map.pData, Matrices, 16*2*4);                        // :66  128 bytes
  memcpy(map.pData + 128, MaterialData, MATERIALDATASIZE);    // :67  +256 bytes
Unmap;                                                        // :68
VSSetShaderResources(0, 8, Textures); GSSet…; PSSet…;          // :83-85
IASetIndexBuffer(Wireframe ? WireBuffer : IndexBuffer, DXGI_FORMAT_R32_UINT, 0);  // :88
if (Wireframe ? WireIndexCount : TriIndexCount) DrawIndexed(that, 0, 0);          // :94-95
```

Notes for a port:
* The per-object constant buffer is rewritten **per draw call** with a discard-map
  (`RenderLayer.cpp:65-68`). In Vulkan this is a per-draw dynamic-offset or push-constant update;
  the buffer is 384 bytes total (`16*4*2 + MATERIALDATASIZE`, created at `phxEngine.cpp:182-183`).
* Textures are bound as an array of 8 SRVs at slots t0..t7 in all three stages
  (`RenderLayer.cpp:83-85`) — the shaders in Clean Slate use `t0`,`t1`,`t2`,`t4`,`t7`
  plus samplers `s0`,`s1`,`s2`.
* The index buffer and the vertex buffer are both already on the GPU; there is no per-frame mesh
  work anywhere. `grep UpdateSubresource` over the whole engine returns **nothing**; the only
  `Map(..., D3D11_MAP_WRITE_DISCARD, ...)` sites are the two constant buffers
  (`RenderLayer.cpp:65`, `Scene.cpp:153`, `Timeline.cpp:92,98`), the texgen constant buffer
  (`Texgen.cpp:156`) and the **CPU particle** vertex buffer (`Scene.cpp:580`). Mesh geometry is
  created once at load.

### 6.4 Per-object matrices → shader

* `CphxModelObject_Mesh::CreateRenderDataInstances(Clip, parentMatrix, ...)` computes
  `phxWorldMatrix = GetMatrix() * parentMatrix`, then
  `phxITWorldMatrix = transpose(inverse(phxWorldMatrix))`
  (`Model.cpp:25-31`; globals declared `Scene.cpp:12-13`).
  `CphxModelObject_Clone` multiplies its own matrix into the parent matrix and recurses
  (`Model.cpp:48-55`) — this is the whole scene-graph transform chain.
* The pair is copied into the instance at creation time (`Material.cpp:157-158`), and uploaded per
  draw at `RenderLayer.cpp:65-68` as `Matrices[0]` (offset 0) and `Matrices[1]` (offset 64).
* Shader-side (verified verbatim in Clean Slate, `cleanslate.apx:41537-41541`, present in all 71
  pass shaders):
  ```hlsl
  cbuffer c : register(b1) { float4x4 worldmat; float4x4 itworldmat; /* then material params */ }
  ```
  where the material parameter floats occupy bytes 128..383 of the same buffer
  (`MATERIALDATASIZE = 64*sizeof(float) = 256`, `phxEngine.h:50`), packed by
  `CphxMaterialParameterBatch::CollectedData` (`Material.h:123-130`) and copied per pass
  (`Material.cpp:154-155`).
* Scene data is buffer **b0** (`SceneDataBuffer`, `SCENEDATASIZE` = 1200 bytes allocated at
  `phxEngine.cpp:49`, 180; only 1072 are written). Shader declaration, again verbatim:
  ```hlsl
  cbuffer b : register(b0) {
    float4x4 viewmat; float4x4 projmat; float4 campos; float4 lightcount;
    lightdata lights[8];   // struct lightdata { float4 pos, ambient, diffuse, specular, spotdirection, sda; } = 96 bytes
    float4 res; float4x4 iviewmat; float4x4 iprojmat;
  }
  ```
  written at `Scene.cpp:153-182` in exactly that order (`viewmat`, `projmat`, `campos`,
  `lightcount`, `lights[8]`, `res` = render-target resolution + reciprocal, `iviewmat`, `iprojmat`).
  Both buffers are bound to the **same two slots in VS, GS and PS**:
  `VSSetConstantBuffers(y,&Buffer)` for `y = 0` (SceneData) and `y = 1` (ObjectMatrix), likewise
  GS and PS (`Scene.cpp:184-190`). The 128 trailing bytes of `SCENEDATASIZE` are unused slack.
* The camera is right-handed (`D3DXMatrixLookAtRH` + `D3DXMatrixPerspectiveOffCenterRH`,
  `Timeline.cpp:174-189`), and `phxViewMatrix`/`phxProjectionMatrix` are uploaded unmodified, so a
  Vulkan port must either flip Y (`negative viewport height`) or negate the projection's Y row to
  match clip-space conventions, and must then also revisit the front-face winding (§1.5).

### 6.5 Per-object data SRVs (trees, voxels)

`CphxMesh::dataBuffer` / `dataBufferView` (`Mesh.h:226-227`) hold the Arbaro branch data for tree
meshes and are forwarded to the model object at `Project.cpp:1006-1007`. `phxMeshDataShaderView`
(`phxEngine.h:33`) is the global counterpart used by the volumetric/voxel path. Both are created as
`D3D11_RESOURCE_MISC_BUFFER_STRUCTURED` with `DXGI_FORMAT_UNKNOWN` and a non-zero
`StructureByteStride` (`Mesh.cpp:1737-1759`), i.e. `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` +
`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` in Vulkan; the bind slot is `t`-space next to the material
textures. **[UNCERTAIN]** the exact register the mesh-data SRV lands in for tree shaders (no tree
objects exist in Clean Slate to check against); grep of the repo shows it is set as a shader
resource view alongside the 8 material textures.

---

## 7. Text mesh generation (`CREATETEXT`)

### 7.1 The font list — and what "ProFont" really is

* `extern char* EngineFontList[32]` (`phxEngine.h:64`, defined `phxEngine.cpp:86`).
* **`ProFont` is unrelated to mesh text.** It is the tool's UI bitmap font
  (`apEx/apEx/ProFont.cpp`, `apEx/apEx/ProFont.h`, created by `CreateProFont(App,"ProFont")` at
  `apEx/apEx/apEx.cpp:63`). Do not confuse the two.
* The mesh-text font is a **Windows system font face name**:
  ```c
  // apEx/Phoenix_Tool/apxPhoenix.cpp:18-45
  char* fonts[] = { "Arial", "Arial Black", "Calibri", "Cambria", "Candara", "Comic Sans MS",
    "Consolas", "Constantia", "Corbel", "Courier New", "Franklin Gothic Medium", "Georgia",
    "Impact", "Lucida Console", "Lucida Sans Unicode", "Microsoft Sans Serif",
    "Palatino Linotype", "Tahoma", "Times New Roman", "Trebuchet MS", "Verdana",
    "Webdings", "Wingdings" };   // 23 entries, indices 0..22
  ```
  `Mesh_Text`'s `Data[0]` is an **index into that hard-coded tool array**. `.apx` files do *not*
  store font names, only the index.
* At export, indices are remapped into a per-project deduplicated name list
  (`MinimalExport.cpp:980` for objects, `:2484-2499` for texture ops), which is written to the blob
  as `FontNames`: a byte count followed by that many ASCIIZ strings
  (`MinimalExport.cpp:2503-2508`); the runtime reads exactly that into `EngineFontList`
  (`Project.cpp:398-401`).
* Clean Slate's 45 `Mesh_Text` objects use font indices
  `{1:33 "Arial Black", 9:4 "Courier New", 0,3,5,7,13,19,21,22: 1 each}` — i.e. mostly
  **"Arial Black"**, plus "Courier New", "Arial", "Cambria", "Comic Sans MS", "Constantia",
  "Lucida Console", "Trebuchet MS", "Webdings", "Wingdings" — with deviation `param[1]` =
  `{0:36, 32:5, 6:2, 7:1, 2:1}`.

### 7.2 `CreateText(Font, Text, Deviation)` — `Mesh.cpp:1870-2019`

```c
float fDeviation = Deviation / 255.0f + 0.001f;                       // :1872
HFONT hFont = CreateFontA(32, 0,0,0, 0,0,0,0,
    EngineFontList[Font][0]=='W' ? SYMBOL_CHARSET : 0, 0,0,0,0,0,     // :1875
    EngineFontList[Font]);
SelectObject(hdc, hFont);                                             // :1877
GLUtesselator* tess = gluNewTess();                                   // :1884
gluTessCallback(tess, GLU_TESS_BEGIN_DATA,  glyphBegin);
gluTessCallback(tess, GLU_TESS_VERTEX_DATA, glyphVertex);
gluTessCallback(tess, GLU_TESS_EDGE_FLAG,   glyphEdge);
gluTessCallback(tess, GLU_TESS_COMBINE_DATA,glyphCombine);            // (non-minimal build only, :1889-1891)
```

Algorithm:
1. Create a 32-pixel-tall GDI font from the face name; note the `'W'`-prefix hack: any face name
   starting with `W` (i.e. **Webdings and Wingdings**) is created with `SYMBOL_CHARSET`, which is
   what maps the byte codes to symbol glyphs.
2. For each character: `GetGlyphOutline(hdc, ch, GGO_NATIVE, &gm, nSize, buf, &m)` is called twice
   (once for the size, once for the data) with a fixed `MAT2` = identity (`Mesh.cpp:1879-1900`);
   advance by `gm.gmCellIncX` at the end of the character (line 1997).
3. The returned `TTPOLYGONHEADER`/`TTPOLYCURVE` outline data is fed to the **GLU tessellator** with
   the mesh as user data:
   * every contour point (the start point and each `TT_PRIM_LINE` point, `Mesh.cpp:1909-1939`) adds
     **two vertices** — one at `z = 0` and one at `z = DEFAULT_EXTRUSION = 1.0` (`Mesh.cpp:1794`,
     `1920-1921`); the `GLU` "vertex data" is the *index* of the first of the pair
     (`(void*)Vertices.NumItems()` before the two `AddVertex` calls).
   * `TT_PRIM_QSPLINE` (TrueType quadratic splines) are recursively flattened by `BInterp`
     (below) with `fDeviation` as the flatness tolerance, then each generated point is fed to the
     tessellator the same way (`Mesh.cpp:1940-1985`). Note the standard TrueType convention that
     each `apfx[j]` is an off-curve control point and the on-curve points are the midpoints between
     consecutive control points, except at the end of the contour — that is exactly the
     `end = (j == cpfx−1) ? points[j+1] : midpoint(points[j], points[j+1])` logic at
     `Mesh.cpp:1960-1968`, with `start` carried forward.
   * `glyphVertex` (`Mesh.cpp:1806-1826`) receives tessellated triangle corners: it records three
     indices and, when the third arrives, emits
     `AddPolygon(t2,t1,t0)` for the **front face at z=0**, `AddPolygon(t0+1,t1+1,t2+1)` for the
     **back face at z=1**, and — for each tessellated edge flagged as a real outline edge by
     `glyphEdge` (`Mesh.cpp:1838-1841`) — a **side quad**
     `AddPolygon(b, b+1, a+1, a)` joining the two layers (`Mesh.cpp:1815-1823`). The result is a
     closed, extruded 3D text mesh of height 1.0.
   * `glyphCombine` (`Mesh.cpp:1829-1836`, non-minimal build only) creates a merged vertex for
     tessellator intersection vertices: it adds the pair at the given coordinates and returns the
     index of the low one.
4. Post-process (`Mesh.cpp:2007-2018`): every vertex `x`/`y` is multiplied by **0.1**, then the text
   is centred on X by subtracting `(max+min)/2` computed over the *scaled* x. **`z` is not scaled**,
   so the extrusion depth stays 1.0 while the glyph height becomes 3.2 units (32px × 0.1).
5. `hFont` and the tessellator are destroyed; the text string itself is the ASCIIZ read at load
   (`Project.cpp:738-741`).

**`BInterp(pPoints, nPoints, p1, p2, p3, fDeviation)` — `Mesh.cpp:1843-1866`**
Adaptive recursive quadratic-Bézier flattening: compute the two midpoints `s1=mid(p1,p2)`,
`s2=mid(p2,p3)` and their midpoint `m`; if `|p2 − m|² < fDeviation²` emit `p2`, else recurse on
`(p1,s1,m)` and `(m,s2,p3)`. `fDeviation = Deviation/255 + 0.001`, so **`param[1]` is an inverted
quality knob: 0 = maximum tessellation** (Clean Slate uses 0 for 36 of 45 text objects, 32 for 5).

### 7.3 Porting caveats for text

* The geometry comes from **Windows GDI's `GetGlyphOutline`** and the **GLU tessellator** — neither
  is available on Linux/macOS, and both are only approximately specified:
  * you need a TrueType/OpenType outline extractor (e.g. FreeType) that matches
    `GGO_NATIVE`'s quadratic-spline contour output, including the TrueType implied on-curve
    midpoint rule;
  * you need a polygon tessellator that produces the *same* triangulation and the same notion of
    "outline edges" as GLU, otherwise the side walls (which depend on `GLU_TESS_EDGE_FLAG`) differ;
  * the glyph outlines themselves come from whatever font file the OS resolves for the face name
    ("Arial Black", "Wingdings", …), so **[UNCERTAIN] the exact vertex output cannot be reproduced
    without shipping the original font files**.
* A pragmatic port: evaluate `BInterp` (exact, above) and the 0.1-scale/centring (exact), and accept
  a different tessellation. For "Wingdings"/"Webdings" text the glyphs are decorative symbols, so
  the *shape* matters. Where the text is used purely as a texture-atlas donor (Clean Slate has 45
  single-letter objects named "S", "c", "l", … which look like atlas/glyph meshes), a mismatch will
  be visible.
* All of §7 is deterministic given the same font file, GDI version and GLU version.

---

## 8. Port checklist and open items

**Must be exact**
1. 72-byte vertex layout, `UV0` = the 4 UV channels in `.xyzw`, `TEXCOORD1` always zero, colour as
   RGBA8, and the *unused* `POSITION1` element (§1.3).
2. Per-polygon-corner vertex emission and the index pattern `(0,2,1)` / `(0,3,2)` (§1.4-1.5);
   quad triangulation is forced one way.
3. `TriIndexCount = TriCount*3`, `WireIndexCount = EdgeCount*2`, `VK_INDEX_TYPE_UINT32`.
4. `worldmat` at offset 0 and `itworldmat` at offset 64 of the per-object buffer, then 256 bytes of
   material constants; `b0` = scene data, `b1` = object data, bound to VS+GS+PS (§6.4).
5. The `SmoothGroupSeparation` shading rules and that `BuildMesh` recomputes normals last unless
   `SkipNormalCalculation` (§4.0).
6. UV generation, including the cylindrical seam fix and the spherical-pole fix (§4.5).

**Must at least be structurally right**
7. Filter order and the DAG topological generation loop (`Project.cpp:852-1005`), including that
   `Mesh_Clone` creates separate draw instances sharing buffers.
8. `Mesh_StoredMini`'s mirrored double-sided expansion and the 16-entry mini-mesh table limit (§5).

**Deliberately fixable**
9. `MeshFilterExtraDataSize[Greeble]` out-of-bounds read (§2.3) — use 0.
10. `WireBuffer` with `ByteWidth = 0` for edge-less meshes, and the unchecked `CreateBuffer`
    results (`Mesh.cpp:497-508`).
11. `CreateMarchingMesh`'s broken grid indexing and `TetraMap` reversal (§4.6) — re-implement; the
    op is unused by Clean Slate.
12. `Greeble` with `taper == 0` collapses the cap to a spike (see §4.4) — reproduce the arithmetic,
    do not "guard" it with a different formula.
13. `Mesh_Spline` generating nothing at runtime (`Project.cpp:1757-1761`) and having no exported
    data.
14. `ModelFilter_Invert` / `ModelFilter_SavePos2` being unreachable (§2.5.1).

**Open / unverified**
* Front-face winding and the Y-flip convention for Vulkan (§1.5) — verify by rendering.
* Whether the 3 `Mesh_Spline` objects matter (they produce empty meshes at runtime).
* `Mesh_Marched` has no `GenerateMesh` case (§2.5.2), so marching-mesh content cannot currently be
  produced by the runtime loader at all.
* Scatter's parameter slicing (`count−2` bytes + 2-byte float, `MinimalExport.cpp:1006-1007`) versus
  the tool's 14-byte parameter image — implement all three Scatter sources to be safe.
* The exact SRV register for tree/mesh-data buffers (§6.5).

