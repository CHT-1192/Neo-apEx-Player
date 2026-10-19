# The apEx `.apx` project format — loader specification

Target audience: an implementer writing a cross-platform (Vulkan) player that loads apEx
demotool project files (`*.apx`).

Every statement below was verified against the apEx public source release at
`~/Repos/apEx-public/` and against the test project
`~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx`
(13 446 660 bytes, 419 807 lines, `build="0498_19821r"`) and
`.../Clean Slate/Precalc.apx` (54 528 bytes, `build="0479_19468r"`).
Anything that could not be verified from the code or those files is marked **UNCERTAIN**.

---

## 0. Source map (authoritative files)

| Concern | File |
| --- | --- |
| Root import/export, sequencing | `apEx/Phoenix_Tool/apxProject.cpp` (`Import`, `ImportMain`, `ImportMaterialMain`, `ExportToString`, `ImportLibraries`, `MakeTextureOperatorGUIDConnections`) |
| Resource base class (GUID I/O, `Import`/`Export` dispatch) | `apEx/Phoenix_Tool/phxResource.cpp` |
| XML DOM wrapper (child lookup, attributes, values) | `Bedrock/UtilLib/XMLNode.cpp`, `XMLNode.h`, `XMLDocument.cpp` |
| base64 | `Bedrock/BaseLib/String.cpp` (`CString::EncodeToBase64`, `CString::DecodeBase64`) |
| GUID | `apEx/Phoenix_Tool/GUID.cpp`, `GUID.h` |
| Materials / techniques / passes / parameters | `apEx/Phoenix_Tool/Material_Tool.cpp` |
| Models / model objects / mesh filters | `apEx/Phoenix_Tool/Model_Tool.cpp` |
| Texture filters & texgen operators | `apEx/Phoenix_Tool/Texgen_tool.cpp`, `TexgenPage.cpp` |
| Render targets | `apEx/Phoenix_Tool/RenderTargets.cpp` |
| Render layers | `apEx/Phoenix_Tool/RenderLayer.cpp` |
| Scenes / objects / clips / splines | `apEx/Phoenix_Tool/Scene_tool.cpp`, `phxSplineExt.cpp` |
| Timeline events | `apEx/Phoenix_Tool/Timeline_tool.cpp` |
| Tree species | `apEx/Phoenix_Tool/TreeSpecies.cpp` |
| Runtime enums & per-primitive parameter meaning | `apEx/Phoenix/Material.h`, `Mesh.h`, `Scene.h`, `Timeline.h`, `Texgen.h`, `phxSpline.h`, `phxEngine.h`, `Project.cpp`, `RenderLayer.cpp` |
| Build string | `apEx/apEx/BuildInfo.cpp` |

> Note: `apEx/Phoenix_Tool/apxFloat16.cpp` is **entirely commented out** (56 lines, all `//`).
> It contributes nothing. Half-floats in `.apx` are `D3DXFLOAT16` (DirectX SDK), i.e. plain
> IEEE-754 binary16. See §2.3.

---

## 1. File-level facts

### 1.1 Encoding, BOM, line endings

* `cleanslate.apx` and `Precalc.apx` contain **zero** bytes ≥ 0x80, so they are pure ASCII and
  trivially valid UTF-8.
* **No BOM**, **no `<?xml ... ?>` declaration**. The first byte is `<` of the root element.
* Line endings are **LF** (`\n`) only — 419 806 LF and 0 CRLF in `cleanslate.apx`.
  (UNCERTAIN whether a freshly saved file from the shipped tool is byte-identical here;
  MSXML's `get_xml` is the writer (`CXMLDocument::SaveToString`) and was not observable.
  Treat encoding/line-endings as *don't care*: sniff a BOM, else assume UTF-8, and accept
  LF or CRLF.)
* The tool writes the file as `XML.Length() * sizeof(TCHAR)` bytes via `WriteFile`
  (`CapexProject::Export`), i.e. it dumps the XML text buffer directly.

### 1.2 Document structure

```
<apExProject build="0498_19821r">
  ... top-level sections ...
</apExProject>
```

* The root element name **must** be `apExProject` for a project file. `CapexProject::Import`
  rejects a document without it; `ImportMain` then does
  `d.GetDocumentNode().GetChild("apExProject")`.
* `build` is the only root attribute. It is written on export as
  `CString::Format("%.4d_%d", RELEASECOUNT, BUILDCOUNT)` followed by `r` (release) or `d`
  (debug) — see `apEx/apEx/BuildInfo.cpp`. **It is never read on import**: a grep for
  `"build"` across the whole tree finds only the three `SetAttribute` call sites. A loader may
  ignore it (it is useful only for diagnostics).
* Second file type: `apExMaterial` (`*.apxmat`) has the same inner structure but the root is
  `<apExMaterial build="...">` and the body holds `rendertechnique` + `material` children
  (`CapexProject::ImportMaterialMain`; exported by `apEx/MaterialEditor.cpp`
  `ExportTech`/`ExportMaterial`). A player can treat an `apxmat` as "the same sections with a
  different root name".

### 1.3 Top-level element inventory

`CapexProject::ExportToString` emits top-level children in exactly this order; `ImportMain`
reads them by *name*, never by position, so document order is irrelevant to the importer.

| Order | Element | Cardinality in `cleanslate.apx` | Repeatable | Notes |
| --- | --- | --- | --- | --- |
| 1 | `AspectX` | 1 | no | int |
| 2 | `AspectY` | 1 | no | int |
| 3 | `Framerate` | 1 | no | int, timeline fps |
| 4 | `Title` | 1 | no | text |
| 5 | `Group` | 1 | no | text |
| 6 | `www` | 1 | no | text → `Urls[0]` |
| 7 | `pouet` | 1 | no | text → `Urls[1]` |
| 8 | `facebook` | 1 | no | text → `Urls[2]` |
| 9 | `youtube` | 1 | no | text → `Urls[3]` |
| 10 | `share` | 1 | no | text → `Urls[4]` |
| 11 | `shaderminifierenabled` | 1 | no | int/bool, export-only setting |
| 12 | `globalshaderminifierenabled` | 1 | no | int/bool, export-only setting |
| 13 | `minimalprecalc` | 1 | no | int/bool, export-only setting |
| 14 | `farbrauschprecalc` | 1 | no | int/bool, export-only setting |
| 15 | `setuphassocial` | 1 | no | int/bool, export-only setting |
| 16 | `gridbpm` | 1 | no | float (`%g`), timeline-editor grid |
| 17 | `gridprimaryhighlight` | 1 | no | int, timeline-editor grid |
| 18 | `gridsecondaryhighlight` | 1 | no | int, timeline-editor grid |
| 19 | `songfile` | 1 | no | text, path; omitted when empty |
| 20 | `treespecies` | 0 in cleanslate | **yes** | tree generator presets |
| 21 | `texturefilter` | 37 | **yes** | texgen filter shaders |
| 22 | `texturepage` | 47 | **yes** | texgen op graphs |
| 23 | `rendertarget` | 13 | **yes** | |
| 24 | `renderlayer` | 4 | **yes** | |
| 25 | `rendertechnique` | 34 | **yes** | |
| 26 | `material` | 15 | **yes** | |
| 27 | `model` | 105 | **yes** | |
| 28 | `scene` | 93 | **yes** | |
| 29 | `event` | 213 | **yes** | timeline |

`Precalc.apx` (`build="0479_19468r"`) is an older-schema file and omits elements 11–15
entirely — which is exactly why every non-list scalar is read defensively (§4.2).

### 1.4 Whitespace text nodes

The tool builds the DOM with `CXMLNode::AddChild(name, PostEnter)`; the default
`PostEnter = true` appends a text node right after each created element, so the serializer
emits an extra (usually blank, sometimes whitespace-only) line after many elements. Examples
from the file:

* a blank line with **2 spaces** after some top-level elements,
* a blank line with **4 spaces** between `<Object>` children
  (`line 73097 == "    "`),
* a fully **empty** line between top-level sections (`line 93 == ""`),
* `<Code>` elements contain raw multi-line text with real newlines preserved.

Consequence: `CXMLNode::GetChildCount()` (no name) counts text nodes as children. **No import
path uses positional child access**, so this never affects semantics. A loader must ignore
whitespace/empty text nodes and select children by element name.

### 1.5 Children lookup semantics (how "repeats" are addressed)

`CXMLNode::GetChildCount(name)` and `GetChild(name, n)` (`XMLNode.cpp:63`, `:182`) walk the
child list and count/skip only nodes whose `nodeName` equals `name` **exactly** (case
sensitive, `wcscmp`). So:

* `<transformation>` #0 is the first `transformation` child, whatever else is interleaved.
* Every importer either does `if (GetChildCount("X")) value = GetChild("X").GetText()` (first
  occurrence wins) or loops `for (i = 0; i < GetChildCount("X"); i++) ... GetChild("X", i)`.
* There is **no place in the import path that depends on the relative order of differently
  named siblings** — with one deliberate exception: `CapexTexGenPage::Import` re-groups the
  texture-page operators by *tag name* (§6.3), so a page's in-memory operator order is
  `Operator*`, `Load*`, `Save*`, `NOP*`, `Subroutine*`, `SubroutineCall*` regardless of
  document order. Only GUI browsing order is affected; graph connectivity is by GUID.

---

## 2. Value encodings

### 2.1 GUIDs

```cpp
class CphxGUID { char GUID[33]; };            // always NUL-terminated 32-char ASCII
void CphxGUID::SetString(char *s) {
  if (!s || strlen(s) != 32) { ... set "BADGUID!BADGUID!BADGUID!BADGUID!" ... return; }
  memcpy(GUID, s, 32);
}
bool operator==(...) { for (x<32) if (GUID[x] != Other.GUID[x]) return false; return true; }
```

* A GUID is **32 uppercase hex characters** (`CphxGUIDServer::RequestGUID` uses `ToHexa` on
  0–15 nibbles), e.g. `CDEDB0BD255B091EDED9D5D630AB5B75`.
* It is **not** a binary 16-byte GUID and has **no dashes**. Storage is ASCII.
* Comparison is a 32-byte `memcmp`; case therefore matters (`SetString` does not normalize).
* The same 32-character length is required everywhere; a wrong-length GUID is replaced by the
  literal error GUID `BADGUID!BADGUID!BADGUID!BADGUID!` and `LOG_ERR` fires
  (`[guid] Error: GUID '%s' isn't 32 bytes long.`).
* Sentinels used for "no reference":
  * `NONENONENONENONENONENONENONENONE` — "none", used for e.g. `TargetLayer`, `TextureGUID`
    (material parameters), model-object `parentguid`, `materialtexturedata/@textureid`.
  * `BADGUID!BADGUID!BADGUID!BADGUID!` — parse failure.
  * A GUID of all-zero characters (32 × `'0'`) would be a legitimate-looking but unused value.
* GUIDs are effectively a global flat namespace, but `CapexProject::GetResource` only searches
  texture filters, render targets, techniques, materials, render layers, texgen operators,
  models and model objects — **not** scenes, objects, clips, events or tree species.
  De-duplication on import happens per collection: duplicates within a collection are dropped
  (`if (GetTextureFilter(f->GetGUID())) delete f; else Filters += f;`), which makes the
  "Merge project" path idempotent.

### 2.2 Integers / floats / booleans as text

`CXMLNode` (`XMLNode.cpp:578`):

| Setter | Format | Reader |
| --- | --- | --- |
| `SetInt(TS32)` | `"%d"` | `GetValue(TS32&)` → `_stscanf_s("%d")`, returns `true` when 1 item matched |
| `SetFloat(TF32)` | `"%g"` (6 significant digits) | `GetValue(TF32&)` → `_stscanf_s("%g")` |
| `SetText` | raw text | `GetText()` |
| `SetAttributeFromInteger` | `"%d"` | `GetAttributeAsInteger` |
| `SetAttributeFromFloat` | `"%g"` | `GetAttributeAsFloat` |

* Boolean-ish values are written as ints `0`/`1` and read back through `GetValue(TBOOL&)`
  (`!= 0`) or `GetValue(TS32&)` then `(x != 0)`.
* Readers use fixed 20-char buffers (`TCHAR s[20]`) — values longer than 19 characters are
  truncated by the *reader*, so keep numbers short. `%d`/`%g` parse tolerantly (leading
  whitespace allowed).
* Missing elements: the vast majority of importers check `GetChildCount(name)` before reading,
  so an absent element simply leaves the member at its constructor default (§4.2).

### 2.3 Half-floats (`D3DXFLOAT16`)

Half-floats are used pervasively (transforms, spline values, spline waveform parameters,
`floatparameter`, tree-species scalars, some particle parameters). They are stored as the
**raw 16-bit IEEE-754 binary16 bit pattern, zero-extended into a 32-bit signed int, printed in
decimal**:

```cpp
// export (e.g. Model_Tool.cpp:443, phxSplineExt.cpp:77)
TS32 v = *((TU16*)&half);      // TU16 = unsigned short -> value in 0..65535
node.SetInt(v);                // or SetAttributeFromInteger("value", v)
// import
TS32 v = 0; node.GetValue(v);  // or GetAttributeAsInteger("value", &v)
TU16 v2 = v;
half = *((D3DXFLOAT16*)&v2);
```

Verified examples from `cleanslate.apx`:

| decimal | bits | binary16 value |
| --- | --- | --- |
| `0` | 0x0000 | +0.0 |
| `15360` | 0x3C00 | 1.0 |
| `32768` | 0x8000 | −0.0 |
| `18688` | 0x4900 | 10.0 |
| `11878` | 0x2E66 | ≈0.049987 (small scale) |
| `47528` | 0xB9A8 | ≈−0.000366 |

Because the value is read back as `%d` into a `TS32`, **negative XML text must not be used**
for these fields: a negative number such as `-32768` would produce the same bit pattern, but
the tool always writes 0..65535. A loader should parse the decimal as a signed int, truncate to
16 bits, and reinterpret as binary16. `0x8000` therefore means the decimal text `32768`.

`float16.cpp` (`apEx/Phoenix/float16.cpp`) and `apxFloat16.cpp` are entirely commented out;
there is no custom half-float codec in the release.

### 2.4 base64

`CString::EncodeToBase64` / `DecodeBase64` (`Bedrock/BaseLib/String.cpp:942`, `:1015`):

* Standard RFC 4648 alphabet `ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/`
  (`base64_chars`, line 928).
* `=` padding is emitted for 1- and 2-byte remainders; **no line breaks, no whitespace**.
* Decoding stops at the first `=` or at the first character for which `is_base64(c)` is false
  (`_istalnum(c) || c == '+' || c == '/'`). Trailing junk is therefore silently ignored.
* Payloads are raw native-layout structs (little-endian on the shipping builds). No headers,
  no counts — the receiver must know the expected size. Known sizes:

| Payload | Size | Where |
| --- | --- | --- |
| `MATERIALVALUE` (parameter value/default) | **16 B** (`float Color[4]` is the largest union member) | `Parameter/Value`, `Parameter/DefaultValue`, `materialparamdata/@valuebase64` — sample text length is always 24 chars with `==` padding, e.g. `AQAAAAAAAAAAAAAAAAAAAA==` |
| mesh `Vertices/@VertexData` | `count * 6 * 4` B (pos.xyz, normal.xyz as float32) | `Model/Object/Vertices` |
| mesh `Polygons/PolyN s/@PolyData` | `n * 24` B, `StoredPolyVertex{ int index; float normal[3]; float uv[2]; }` | `Model/Object/Polygons` |
| `MiniVerts/@data` | 1 B per stored vertex (3 B per mini-vertex, see §7.6) | `Model/Object/MiniVerts` |
| `MiniTris/@data` | 1 B per index, 3 per triangle | `Model/Object/MiniTris` |
| `textblob` (model/model object/texgen operator) | arbitrary; decoded length is authoritative | text content, not attribute |
| texgen operator `image` | arbitrary (DDS/PNG bytes loaded by the tool) | text content |

Decoding errors are not detected: `DecodeBase64` returns whatever it produced, and callers
guard with `if (DataSize == sizeof(X))` before `memcpy`.

### 2.5 `MATERIALVALUE` layout (the 16-byte parameter blob)

```cpp
union MATERIALVALUE {
  float Float;             // PARAM_FLOAT, PARAM_PARTICLELIFEFLOAT(?)
  float Color[4];          // PARAM_COLOR
  unsigned char ZMode;     // PARAM_ZMODE     (byte 0)
  unsigned char BlendMode; // PARAM_BLENDMODE0..7 (byte 0)
  D3D11_COMPARISON_FUNC ZFunction; // PARAM_ZFUNCTION     (int32 at 0)
  bool Wireframe;          // PARAM_FILLMODE      (byte 0)
  D3D11_CULL_MODE CullMode;// PARAM_CULLMODE      (int32 at 0)
  int RenderPriority;      // PARAM_RENDERPRIORITY(int32 at 0)
  ID3D11ShaderResourceView *TextureView;   // runtime only, never serialized
  CphxRenderTarget *RenderTarget;          // runtime only, never serialized
};
```

Always treat it as **16 bytes**; interpret the leading bytes per the parameter `Type`
(all values little-endian):

| Type id | Name | Stored payload | Runtime meaning (verified in `Phoenix/Project.cpp:1874` `CollectRenderState` and `Material_Tool.cpp:660` `CollectData`) |
| --- | --- | --- | --- |
| 0 | `PARAM_FLOAT` | `float` @ 0 | appended to the constant float stream (1 float) |
| 1 | `PARAM_COLOR` | `float[4]` | appended to the constant float stream (4 floats) |
| 2 | `PARAM_ZMODE` | `u8` @ 0 | `DepthEnable = !(ZMode & 1)`; `DepthWriteMask = (ZMode>>1)&1 ? ZERO : ALL` |
| 3 | `PARAM_ZFUNCTION` | `int32` @ 0 = `D3D11_COMPARISON_FUNC` | `DepthStencilDesc.DepthFunc` |
| 4 | `PARAM_FILLMODE` | `bool`/`u8` @ 0 | `pass->Wireframe` |
| 5 | `PARAM_CULLMODE` | `int32` @ 0 = `D3D11_CULL_MODE` | `RasterizerDesc.CullMode` |
| 6 | `PARAM_RENDERPRIORITY` | `int32` @ 0 | `pass->RenderPriority` (default 127) |
| 7..14 | `PARAM_TEXTURE0..7` | no blob; `TextureGUID` child | binds `Textures[0..7]` |
| 15..22 | `PARAM_BLENDMODE0..7` | `u8` @ 0; low nibble = src blend **code**, high nibble = dst blend **code** | `BlendEnable = true` for RT index `Type-15`; `SrcBlend = (BlendMode + 0x11) & 15`, `DestBlend = (BlendMode + 0x11) >> 4` in the *tool* path (i.e. `D3D11_BLEND = apEx code + 1`); `Phoenix/Project.cpp` uses the nibbles **without** `+0x11` — see §12 |
| 23 | `PARAM_RENDERTARGET` | no blob; `TextureGUID` child | `pass->RenderTarget` (post-process target) |
| 24 | `PARAM_PARTICLELIFEFLOAT` | `float` @ 0 | particle life, animated |
| 25 | `PARAM_DEPTHTEXTURE7` | none | binds `Textures[7] = depth buffer` |
| 26 | `PARAM_3DTEXTURE6` | none | binds `Textures[6] = 3D texture` (volumetric build only) |
| 27..34 | `PARAM_MESHDATA0..7` | none | binds `Textures[0..7] = mesh data texture` |
| 35 | `PARAM_PARTICLELIFE` | `float` @ 0 (see §12) | particle life (integer-ish) |
| 36 | `PARAM_LTC1` | none | binds `Textures[4] = LTC1 LUT` |
| 37 | `PARAM_LTC2` | none | binds `Textures[5] = LTC2 LUT` |

`MaterialParamHasNonGUIDData` (`Material_Tool.cpp:408`) decides whether `Value`/`DefaultValue`
are written at all: it is true for types 0–6, 15–22, 24, 35 and false for 7–14, 23, 25–34,
36, 37. So a texture/rendertarget/meshdata/LTC parameter never carries a blob — only
`<TextureGUID>`.

### 2.6 Spline encoding

A spline is a block of scalars plus keys. Keys are indexed by an 8-bit timestamp:

```cpp
struct CphxSplineKey { unsigned char t; D3DXFLOAT16 Value[4];
                       D3DXFLOAT16 controlvalues[6]; unsigned char controlpositions[2]; };
#define SPLINEPOSITIONCALC(t) ( t ? (t + 1.0f) / 256.0f : 0 )   // phxSpline.h
```

So `t = 0` → time 0.0, `t = 1` → 2/256, …, `t = 255` → 1.0. Keys are kept sorted by `t`
(`CphxSpline_Tool::Sort`) and events/objects address them by normalized time.

Two spline flavours exist and have **different child sets**:

* `CphxSpline_Tool_float16` — `<interpolation>`, `<loop>`, `<waveform>`,
  `<multiplicativewaveform>`, `<wfamplitude>`, `<wffrequency>`, `<wfrandseed>`, then keys.
* `CphxSpline_Tool_Quaternion16` — only `<interpolation>` and `<loop>`, then keys.
  Used **only** for `Spline_Rotation` (`clipspline type="4"`), where the four values are a
  quaternion `(x, y, z, w)` — `Scene_tool.cpp:2231` is the only construction site. Every other
  spline in the format is a float16 spline. The sample in §10.3 confirms `type="4"` blocks have
  no `waveform*` children.

`ExportKeys` (`phxSplineExt.cpp:68`): if there are **no** keys, four constant `<value id="n">`
elements (n = 0..3) are written. Otherwise each `<key>` holds `<time>` and four
`<value id="n">`, then two `<controlpos id="n">` and six `<controlvalue id="n">`.

Field types (from `CphxSpline_float16` / `CphxSplineKey`):

| Field | Stored as |
| --- | --- |
| `interpolation` | int (`SPLINEINTERPOLATION`) |
| `loop`, `multiplicativewaveform` | int/bool |
| `waveform` | int (`SPLINEWAVEFORM`) |
| `wfrandseed` | int |
| `wfamplitude`, `wffrequency` | half-float bit pattern |
| `key`/`time` | int 0..255 |
| `key`/`value`, `key`/`controlvalue` | half-float bit pattern |
| `key`/`controlpos` | int 0..255 |

Enums: `SPLINEINTERPOLATION { CONSTANT=0, LINEAR=1, CUBIC=2, BEZIER=3 }`,
`SPLINEWAVEFORM { NONE=0, SIN=1, SQUARE=2, TRIANGLE=3, SAWTOOTH=4, NOISE=5 }`.

### 2.7 Matrices

Two different serializations exist; do not confuse them.

**(a) Model-object transform** (`Model_Tool.cpp:435` export, `:497` import). 12
`<transformation index="i" value="half">` children plus (implicitly) a 4th row `(0,0,0,1)`.
The storage in C++ is a flat half-float array `Object->TransformationF16[12]`, written by
`SetMatrix` as `TransformationF16[y + x*3] = m.m[x][y]` (`D3DXMATRIX m`), i.e. flat offset
`row*3 + col` for a 4×3 matrix where `m[row][col]` with row 0..3, col 0..2. The XML is the
*transpose* of the flat order:

```
flat_slot(index) = (index / 4) + (index % 4) * 3
                 = col + row * 3          where row = index % 4, col = index / 4
```

Equivalently: **XML index = `row*4 + col` over a 4-row × 3-column matrix**
(row-major of the 3×4 affine matrix; `D3DXMATRIX` row-vector convention, so row 3 is the
translation). Verified against the file: the identity object writes
`index 0 = 15360 (1.0)`, `5 = 15360`, `10 = 15360`, and `3, 7, 11 = 0` (translation 0).
Reconstructed matrix (`row` = first D3DX index):

```
m[0][0..2] = idx 0, 4, 8
m[1][0..2] = idx 1, 5, 9
m[2][0..2] = idx 2, 6, 10
m[3][0..2] = idx 3, 7, 11     <- translation
m[i][3]    = 0,0,0,1
```

**(b) Mesh-filter transform** (`Model_Tool.cpp:1763` export, `:1794` import). Also 12
`<transformation index="i" value="half">`, but there **`index == array slot`** directly
(`srt[i]`), and `srt` is interpreted by `GetRawMatrix()` as
`m.m[x][y] = srt[x + (y<<2)]` for `x` 0..3, `y` 0..2 — i.e. column-major grouping of four
12-byte columns. Combine with `ApplyTransformation` (`srt[0..2]` scale−1, `srt[3..6]`
quaternion xyzw, `srt[7..9]` translation, `srt[10..11]` unused) when composing the matrix.

**(c) No other matrices are serialized.** Shader matrices (`worldmat`, `viewmat`, …) are
engine-provided constants, not file data.

---

## 3. Import pipeline and sequencing

`CapexProject::ImportMain` (apxProject.cpp:420) executes in this fixed order:

1. `ImportTreeSpecies(&RootNode, false)`
2. `ImportTextureFilters(&RootNode, false)`
3. `texturepage` loop — `CreateTexgenPage()` + `Import` (operators only; GUIDs unresolved)
4. `ImportMaterials(&RootNode, false)` — in order: `rendertarget`, `renderlayer`,
   `rendertechnique`, `material`. Then, if no imported render layer has `Pickable` set, the
   **first** render layer gets `HasPicking = true` (a UI affordance).
5. `model` loop — `CreateModel()` + `Import` (objects still unresolved)
6. `scene` **pass 1** — `ImportGUID` only, for all scenes (so sub-scene references resolve)
7. `scene` **pass 2** — full `Import` + `ApplyCameraDataForFirstCam`
8. scalars (`AspectX`, `AspectY`, `Framerate`, `Title`, `Group`, URLs, exporter flags,
   grid, `songfile`)
9. `event` loop — events whose `<Type>` is missing or unparsable are **skipped**; otherwise
   `Timeline->CreateEvent(type,0,0,0,NULL)` then `Import`. Then `Timeline->Sort()`
10. `ImportLibraries()` — merges `Data/Arboretum.xml`, `Data/FilterLibrary.xml`,
    `Data/MaterialLibrary.xml` (relative to the tool's CWD; missing files only warn). Filters
    coming from a library are marked `External = true`; on export such a resource is only
    written if `HasDependants()`, i.e. **every filter/technique/material that is actually
    referenced is inlined into the `.apx`**. Verified for `cleanslate.apx`: all 32 referenced
    filter GUIDs are among the 37 defined filters, all 28 referenced technique GUIDs are among
    the 34 defined techniques, all referenced material/rendertarget/renderlayer GUIDs resolve.
11. `MakeTextureOperatorGUIDConnections()` — resolves `Load/@LoadedOp` → `LoadedOp`,
    `SubroutineCall/Subroutine` → subroutine operator, and subroutine parameter targets to
    operator IDs.
12. `TexgenPages[i]->BuildOperatorConnections()` — rebuilds the operator graph from geometry
    (see §6.3).
13. For every `EVENT_SHADERTOY` event, `AddParent(GetResource(MaterialData.MaterialTextures[i]))`
    — legacy fixup that wires the shadertoy event's texture parameters to their resources.

Then (only in `CapexProject::Import`, not in `MergeProject`):

14. `UpdateRTCollections()` — builds the device render-target collection.
15. **Song loading**: `if (SongFile.Length() && CanLoadSong())` — see §5.2.

A player that only wants to render can implement steps 1–13 plus its own resource resolution;
`External` flags, `UpdateRTCollections`, picking defaults and exporter flags are tool state.

### 3.1 Deferred reference resolution (things you must not resolve too early)

| Reference | Element/attribute | Resolved in |
| --- | --- | --- |
| `material` → `rendertechnique` | `<Tech>` | immediately (`Project.GetTech`); techs are imported before materials; missing → silently dropped |
| `rendertechnique` → `renderlayer` | `<TargetLayer>` | immediately; missing layer → `LOG_ERR` and no target layer |
| `renderlayer` → `rendertarget` | `<RenderTarget>` | immediately; missing → skipped |
| `event` → `rendertechnique` | `<Tech>` | immediately (`SetTech`) |
| `event` → `rendertarget` | `<TargetRT>` | after all imports (`ApplyRenderTargets`) |
| model object → `material` | `<Material GUID=…>` | immediately |
| model object → other model objects | `<parentguid>`, `<clonedobject>`, `<Filter><referencedguid>` | `CphxModel_Tool::ImportData` end: `Objects[i]->UpdateDependencies()` |
| scene object → object | `<targetobject>`, `<parentobject>` | `CphxScene_Tool::ImportData` end |
| scene object clip → sub-scene | `<clipdata><subscenetarget>` | `CphxObjectClip_Tool::BuildMinimalData` / `UpdateSplines` |
| `Object/Filter/texture` | GUID text | `Project.GetTexgenOp` |
| `materialtexturedata/@textureid` | | `GetResource` at collection time |
| texgen `Load/@LoadedOp` | | `MakeTextureOperatorGUIDConnections` (after all pages) |
| scene ↔ sub-scene cycles | `scene`/`clip` GUIDs | two-pass scene import |

---

## 4. Common patterns

### 4.1 Every resource starts with `<GUID>`

`CphxResource::Export` = `ExportGUID` then `ExportData`; `Import` = `ImportGUID` then
`ImportData` (`phxResource.cpp:207`). Therefore **every** `texturefilter`, texture-page
operator (`Operator`/`Load`/`Save`/`NOP`/`Subroutine`/`SubroutineCall`), `rendertarget`,
`renderlayer`, `rendertechnique`, `material`, `model`, model `Object`, mesh `Filter`, `scene`,
scene `Clip`, scene `Object`, `event` and `treespecies` element begins with `<GUID>`.
A missing `<GUID>` logs `[resource] GUID Missing from resource node in XML!` and substitutes
`BADGUID!BADGUID!BADGUID!BADGUID!`.

Note: texture-page **operators** also carry `<GUID>` (they are `CphxResource`s) — this is the
GUID that `Filter/texture`, `Load/@LoadedOp` and `SubroutineCall/Subroutine` reference.
Operators additionally have a runtime-only integer `ID` (`GlobalID++`) that is **not** written
to XML.

### 4.2 Optional elements, defaults, tolerance

The importer is uniformly tolerant: each field is read only if the element exists, and it
assigns only on successful parse (`if (Node->GetChildCount("X")) Node->GetChild("X").GetValue(v);`
— note that `GetValue` returning false still leaves `v` unchanged because it was pre-loaded
from the member: many fields use the `TS32 x = member; if (...) GetValue(x); member = x;`
idempotent idiom, others ignore the return value). Consequences:

* **Extra unknown elements are ignored** — there is no schema validation and no
  `validateOnParse` (explicitly disabled in `CXMLDocument::Allocate`).
* **Missing elements keep constructor defaults.** Defaults that matter:

| Field | Default | Source |
| --- | --- | --- |
| `AspectX`/`AspectY` | 16 / 9 | `CphxTimeline_Tool::CphxTimeline_Tool` |
| `Framerate` | 120 | same |
| `GridBPM` | (engine default) | |
| material parameter `Scope` | `PARAM_CONSTANT` (0) | `CphxMaterialParameter_Tool` ctor |
| material parameter `Type` | `PARAM_FLOAT` (0) | same |
| `ResolutionDescriptor` | ctor value (0 unless set) | `CphxRenderTarget_Tool` |
| `ZResolution` | 0 | `RenderTargets.cpp:104` |
| `PixelFormat` | 0 (`RGBA16Float`) | `RenderTargets.cpp:110` |
| render layer `ClearRenderTargets` | `true` | `RenderLayer.cpp:13` |
| `PointLight` | false | object ctor |
| particle `TurbulenceFrequency` | 10 | `CphxObjectClipArray_Tool::AddClip` |
| spline `interpolation` | from the spline ctor | `phxSplineExt.cpp` |
| model object `parentguid` | `NONENONENONENONENONENONENONENONE` | `Model_Tool.cpp:381` |
| `Load`/`Save`/`NOP` name | empty → `GetName()` synthesises `"Load"`/`"Save"`/`"NOP"` | `Texgen_tool.cpp` |

* **A repeated element that is missing entirely is often a no-op**, e.g. an object with no
  `clipspline` children simply has no animated tracks; an object clip with no `clipdata` for a
  given scene clip is left at its defaults.
* One asymmetrically *required* element: `model/Object` **must** have a `Type` attribute or it
  is skipped (`if (!n.HasAttribute("Type")) continue;`). Likewise `scene/Object` and
  `clipspline`, and `Filter` (`Type` attribute) — the same for `Object/Filter`.
* `CphxTextureOperator_Load` without `<LoadedOp>` logs
  `[import] Texture load %s missing LoadedOp id field!` and stays unconnected;
  `SubroutineCall` without `<Subroutine>` logs analogously.

### 4.3 The two attribute forms

Both forms occur and mean the same thing — a value attached to a repeated element:

```xml
<Parameter ID="3">17</Parameter>          <!-- texgen operator parameter -->
<transformation index="3" value="32768"/> <!-- model object / mesh filter -->
<value id="2">15360</value>               <!-- spline -->
<controlpos id="1">15</controlpos>
<parentguid index="0" value="NONE..."/>
<materialparamdata paramid="…" valuebase64="…"/>
<clipspline type="8" materialparam="…" groupingobject="…"/>
<Object Type="16">                        <!-- model object primitive -->
<Filter Type="1">                         <!-- mesh filter type -->
<event>…<Type>3</Type>                    <!-- event type is a CHILD, not an attribute -->
```

The `index`/`ID`/`id` attributes are authoritative; importers fall back to the positional index
only when the attribute is absent (e.g. `TS32 attrn = x; if (n.HasAttribute("ID")) n.GetAttributeAsInteger("ID", &attrn);`).
Out-of-range indices from a malformed file would write out of bounds in the original code
(no bounds checks in `Texgen_tool.cpp:374`, `Model_Tool.cpp:529`, `phxSplineExt.cpp:117`) — a
robust loader must range-check.

## 5. Root scalars and `<songfile>`

### 5.1 Scalars

Read order and types (`apxProject.cpp:464-493`):

```cpp
TS32 x = Timeline->Timeline->AspectX; if (has("AspectX")) get("AspectX", x); Timeline->Timeline->AspectX = x;
// same for AspectY, Framerate
if (has("Title")) Title = get("Title").GetText();
if (has("Group")) Group = get("Group").GetText();
if (has("www"))      Urls[0] = …;   if (has("pouet"))    Urls[1] = …;
if (has("facebook")) Urls[2] = …;   if (has("youtube"))  Urls[3] = …;
if (has("share"))    Urls[4] = …;
if (has("shaderminifierenabled"))       get(…) → EnableShaderMinifier;
if (has("globalshaderminifierenabled")) get(…) → EnableGlobalShaderMinifier;
if (has("minimalprecalc"))              get(…) → EnableMinimalPrecalc;
if (has("farbrauschprecalc"))           get(…) → EnableFarbrauschPrecalc;
if (has("setuphassocial"))              get(…) → EnableSetupHasSocial;
if (has("gridbpm"))                  get(…) → Timeline->GridBPM;              // float
if (has("gridprimaryhighlight"))     get(…) → Timeline->BPMPrimaryModulus;    // int
if (has("gridsecondaryhighlight"))   get(…) → Timeline->BPMSecondaryModulus;  // int
if (has("songfile")) SongFile = get("songfile").GetText();
```

* `AspectX`/`AspectY` are the **timeline** aspect (used for the shader `aspect` uniform and for
  aspect-correct render-target sizing), not the output window aspect.
* `shaderminifierenabled`, `globalshaderminifierenabled`, `minimalprecalc`,
  `farbrauschprecalc`, `setuphassocial` are **exporter settings only** — a player can ignore
  them. (`minimalprecalc`/`farbrauschprecalc` control the optional `.mus` "precalc" export
  pipeline, not the `.apx` content. Not verified in code beyond being read into project
  members; treat as opaque toggles. **UNCERTAIN** whether any of them affect runtime
  rendering.)
* `grid*` are timeline-editor snapping aids. Ignore.

### 5.2 `<songfile>` resolution

`SongFile` is stored exactly as written (the Clean Slate project has
`<songfile>.\step010padding.mvm</songfile>`). Resolution (`apxProject.cpp:652-673`) is:

```cpp
if (SongFile.Length() && CanLoadSong()) {
  CStreamReaderFile f;
  bool open = f.Open(SongFile.GetPointer());        // 1) relative to the CURRENT DIRECTORY
  if (!open) {
    TCHAR name[MAX_PATH];
    LPSTR *fileName = nullptr;
    if (GetFullPathName(FileName.GetPointer(), MAX_PATH, name, fileName)) {  // full path of the .apx
      PathRemoveFileSpecA(name);                                            // strip file name
      TCHAR relative[MAX_PATH];
      PathCombineA(relative, name, SongFile.GetPointer());                   // 2) relative to the .apx directory
      open = f.Open(relative);
    }
  }
  if (open) if (f.GetLength()) LoadSong(f, hwnd);
}
```

Rules to implement:

1. Try the path verbatim, relative to the process working directory.
2. Otherwise, resolve it relative to the directory containing the `.apx` file
   (equivalent to `dirname(apxPath) + "/" + songfile`).
3. `.\step010padding.mvm` is a **Windows-style** relative path (`.\` separator). On POSIX you
   must normalise both separators (`\` → `/`) and strip a leading `.\` or `./`.
   `PathCombineA` also handles an absolute `SongFile` by ignoring the base path, so an
   absolute path in `<songfile>` simply resolves to itself.
4. `CanLoadSong()` is a build/config gate (`apxProject.cpp:1430`); if false the file is not
   loaded at all. The expected format is the **MVX** music container (`step010padding.mvm`,
   985 429 B, and `step010paddingpadding.mvm`, 1 025 749 B, ship next to it). The loading is
   `CphxSound::Load(CStreamReader&, MvxProgress, …)`; `Song->GetPlayPosition()` (ms) drives the
   frame counter (`GetFrameToRender`: `frame = position_ms/1000 * Framerate`). A player that
   does not implement MVX can still run the timeline using its own clock, as
   `Timeline::Render(frame)` takes an absolute frame number.
5. `<songfile>` is only written on export `if (SongFile.Length())`, so its absence is normal.
   If the file cannot be found, loading silently does nothing (the demo runs without audio).

---

## 6. `<texturefilter>` — texgen filter

`CphxTextureFilter_Tool` (`Texgen_tool.cpp:35`/`:65`). These are the pixel shaders that bake
the procedural texture pages. A player that does not run texgen can ignore their `<Code>` and
`<Parameter>` bodies, but it **must** parse `<LookupType>` if it wants to reimplement the
matching operator semantics (§6.3).

### 6.1 Children (in export order)

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | GUID | from `CphxResource` |
| 2 | `Name` | 1 | text | |
| 3 | `Code` | 1 | text (multi-line, XML-escaped) | HLSL ps_5_0, entry point `"p"` (`Texgen_tool.cpp:137`) |
| 4 | `Type` | 1 | int | **tool-only** category (a `CWBItemSelector` cursor position in the filter editor, `TexGenFilterEditor.cpp:218`). **UNCERTAIN**: no runtime consumer of this field was found — ignore it |
| 5 | `PassCount` | 1 | int | `Filter.DataDescriptor.PassCount` |
| 6 | `InputCount` | 1 | int | number of input textures the operator must supply |
| 7 | `NeedsRandSeed` | 1 | int/bool | operator needs `<Seed>` |
| 8 | `LookupType` | 0..1 | int | **written only when non-zero**. 0 = none, 1 = image load, 2 = text render, 3 = spline lookup, 4 = one more lookup flavour (`PHXTEXTUREFILTER::GetLookupTexture`, `Phoenix/Texgen.cpp:207`, accepts `LookupType == 4` without extra data). Only 1/2/3 were traced end-to-end; 4 is used by Clean Slate's `CellEdges` filter. **UNCERTAIN**: exact meaning of 4 |
| 9 | `Minifiable` | 1 | bool | shader-export hint, ignorable |
| 10 | `Parameter` | 0..n | group | see below, **positional** (no attribute) |

### 6.2 `<Parameter>` inside a filter

`ExportData` writes `Name`, `Type`, `Min`, `Max` and (unless `Type` ∈ {1,3,4}) `Default`, and
then one `ListItem` per enum entry. `ImportData` reads them **by position**: the *i*-th
`Parameter` child is the *i*-th filter parameter; there is no index attribute.

```cpp
for (x = 0; x < GetChildCount("Parameter"); x++) {
  CphxTextureFilterParameter *p = new …;
  n = GetChild("Parameter", x);
  if (n.GetChildCount("Name"))  p->Name = n.GetChild("Name").GetText();
  if (n.GetChildCount("Type"))  n.GetChild("Type").GetValue(p->Type);
  if (p->Type == 1 || p->Type == 3 || p->Type == 4) p->Default = 0;   // no Default element
  if (n.GetChildCount("Min")) n.GetChild("Min").GetValue(p->Minimum);
  if (n.GetChildCount("Max")) n.GetChild("Max").GetValue(p->Maximum);
  if (n.GetChildCount("Default")) n.GetChild("Default").GetValue(p->Default);
  for (y = 0; y < n.GetChildCount("ListItem"); y++) p->ListItems += n.GetChild("ListItem", y).GetText();
  Parameters += p;
}
```

* Parameter `Type` controls the byte size used when the filter's parameters are packed into an
  operator's `Parameter[16]` array (`CphxTextureFilter_Tool::GetParamSize`,
  `Texgen_tool.cpp:115`): types **0, 1, 3, 4 → 1 byte**, type **2 → 4 bytes**.
  `GetParamStart(i)` is the running sum, so parameters are laid out contiguously in
  `OpData.Parameters[0..15]` in declaration order.
* `ListItem` exists only for enum-style parameters; the GUI uses it for the combo box.

### 6.3 `<texturepage>` — texgen operator graph

`CapexTexGenPage` (`TexgenPage.cpp:160`/`:195`). This is the *bake recipe*: a small dataflow
graph of operators, each referencing one filter and a set of inputs. Pages produce the
render-target-sized procedural textures used by materials (via `Save`/`Load`).

Children:

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `Name` | 1 | page name |
| 2 | `xres` | 1 | exponent nibble: `XRes = 1 << xres` (`GETXRES(x) = 1<<(x>>4)` for the packed byte) |
| 3 | `yres` | 1 | `YRes = 1 << yres` |
| 4 | `hdr` | 1 | bool; HDR pages allocate `R16G16B16A16_FLOAT`, else `R16G16B16A16_UNORM` (`CphxTexturePoolTexture_Tool::Create`) |
| 5..n | operator elements | **yes** | one of `Operator`, `Load`, `Save`, `NOP`, `Subroutine`, `SubroutineCall` — the tag **is** the operator kind |

```cpp
TU8 res = (BaseXRes << 4) + BaseYRes;              // packed resolution byte
for (x = 0; x < GetChildCount("Operator"); x++)       { new CphxTextureOperator_Tool;              … Operators += t; }
for (x = 0; x < GetChildCount("Load"); x++)           { new CphxTextureOperator_Load;              … }
for (x = 0; x < GetChildCount("Save"); x++)           { new CphxTextureOperator_Save;              … }
for (x = 0; x < GetChildCount("NOP"); x++)            { new CphxTextureOperator_NOP;               … }
for (x = 0; x < GetChildCount("Subroutine"); x++)     { new CphxTextureOperator_Subroutine;        … }
for (x = 0; x < GetChildCount("SubroutineCall"); x++) { new CphxTextureOperator_SubroutineCall;    … }
… then every operator's OpData.Resolution = res;  BuildOperatorConnections();
```

Tag ↔ op type (`APEXTEXTUREOPTYPE`, `Texgen_tool.h:74`):
`TEXGEN_OP_NORMAL=0` → `Operator`; `TEXGEN_OP_SAVE=1` → `Save`; `TEXGEN_OP_LOAD=2` → `Load`;
`TEXGEN_OP_NOP=3` → `NOP`; `TEXGEN_OP_SUBROUTINE=4` → `Subroutine`;
`TEXGEN_OP_SUBROUTINECALL=5` → `SubroutineCall`.

`Save` inherits `CphxTextureOperator_NOP`, so a `Save` element has the `NOP` child set (no
`Filter`/`Parameter`); its `<Name>` is the save-slot label that `Load/@LoadedOp` refers to.

**Operator graph edges are not in the file.** `CapexTexGenPage::BuildOperatorConnections`
(`TexgenPage.cpp:91`) derives them from the operators' rectangle geometry:

```cpp
Operators.Sort(OperatorPositionSorter);      // (y2 asc, then x1 asc)   -- TexgenPage.cpp:84
for (x = 0; x < Operators.NumItems(); x++) {
    opx = Operators[x];
    opx->RemoveParents(PHX_TEXTUREOPERATOR); // all edges are rebuilt
    for (y = 0; y < x; y++)                  // only ops earlier in the sorted order
        ConnectOperators(opx, Operators[y], inputcnt);
    // then trim parents beyond opx->GetInputCount(), and re-apply item-specific parents
    // (Load -> its Save, SubroutineCall -> its Subroutine)
}
```

```cpp
// ConnectOperators, TexgenPage.cpp:295
int opbottomrow = opy->Position.y2 - 1;
if ( opbottomrow + 1 == opx->Position.y1                       // == opy.y2 == opx.y1
     && opy->GetOpType() != TEXGEN_OP_SUBROUTINE )
  if ( IntervalIntersection(opx->Position.x1, opx->Position.x2 - 1,
                            opy->Position.x1, opy->Position.x2 - 1) )   // closed intervals
    opx->AddParent(opy);
```

So the rules are:

1. **Rows are the dataflow direction.** `y` grows downward, so a producer sits directly above
   its consumer: `producer.y2 == consumer.y1` (requiring exactly touching rows).
2. **Horizontal overlap** of the closed intervals `[x1, x2-1]` supplies the edge; an operator
   is never its own input and `Subroutine` operators never become geometry-derived inputs.
3. The global order used for graph reconstruction is **`(y2 asc, x1 asc)`**, and this sort also
   replaces the page's in-memory operator list (the XML order is *not* preserved). Because all
   candidate producers of one consumer share the same `y2`, their relative order is by ascending
   `x1`, i.e. **left to right** — that is the input index order for multi-input filters.
4. Parents beyond `GetInputCount()` are removed from the end
   (`GetInputCount` = `Filter.DataDescriptor.InputCount`, `1` for `NOP`/`Save`, and the
   subroutine's input count for `SubroutineCall`); `Load`/`SubroutineCall` re-add their
   item-specific parent afterwards.
5. `TEXGEN_MAX_PARENTS = 3` (`Phoenix/Texgen.h:11`) — at most 3 geometry inputs.

A reimplementation must therefore keep the operators of a page (in the grouped-by-tag order
described in §1.5), keep `x1,y1,x2,y2` verbatim, and rebuild the DAG with exactly the
predicate above.

Explicit GUID references are used only for `Load` (→ the `Save` it mirrors) and subroutines
(→ the `Subroutine` operator and its parameter targets).

### 6.4 Operator element bodies

**`Operator` (`TEXGEN_OP_NORMAL`)** — `Texgen_tool.cpp:265`/`:334`:

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `GUID` | 1 | |
| 2 | `Name` | 0..1 | written only if non-empty |
| 3 | `x1`, `y1`, `x2`, `y2` | 4 | ints, GUI/graph geometry (see above) |
| 4 | `Filter` | 1 | GUID of the `texturefilter` |
| 5 | `Resolution` | 1 | packed resolution byte (overwritten by the page's `(xres<<4)+yres` on import) |
| 6 | `Seed` | 1 | random seed byte for `NeedsRandSeed` filters |
| 7 | `Parameter` | exactly `TEXGEN_MAX_PARAMS` = **16** | `<Parameter ID="n">v</Parameter>`, each 0..255 |
| 8+ | lookup data | 0..n | only when the referenced filter has `LookupType`: 1 → `image` (base64 of the loaded image bytes); 2 → `textposx`, `textposy`, `textsize`, `textcharspacing`, `textitalic`, `textbold`, `textfont`, `textblob` (base64 of the ASCII text — the legacy `<text>` alternative is still *read* but never written); 3 → four `spline` children with `splineid="0..3"`, each a float16 spline block (§2.6) |

`TEXGEN_MAX_PARAMS = 16`, `TEXGEN_MAX_PARENTS` in `Phoenix/Texgen.h`.

**`NOP` / `Save`** (`Texgen_tool.cpp:1100`/`:1109`): `GUID`, optional `Name`, `x1..y2`. Nothing
else.

**`Load`** (`Texgen_tool.cpp:1209`/`:1239`): `GUID`, optional `Name`, `x1..y2`, `Resolution`,
`LoadedOp` (GUID of the `Save` operator whose texture this reads — resolved in a second pass
after all pages are imported, which is why forward references are legal; the sample in §9
shows a `Load` whose target `Save` appears later in the same page).

**`Subroutine`** (`Texgen_tool.cpp:1376`/`:1401`): `GUID`, optional `Name`, `x1..y2`, then
`SubParameter` per parameter:

```xml
<SubParameter>
  <Name>…</Name><Type>0</Type><Min>0</Min><Max>255</Max><Default>0</Default>
  <ParamTarget>
    <Target>GUID-of-target-operator</Target>
    <TargetParameter>i</TargetParameter>     <!-- -1 => the target's RandSeed -->
  </ParamTarget>
  …
</SubParameter>
```

Note the nesting: `ParamTarget` is a child of `SubParameter` (not a sibling). On import the
target GUID is stored and translated to an operator `ID` in
`MakeTextureOperatorGUIDConnections`; `TargetParameter` is an offset into the target operator's
parameter array, computed with `GetParamStart` — for a `SubroutineCall` target it is an offset
into the *subroutine instance's* parameter block, and `-1` means "assign to RandSeed".

**`SubroutineCall`** (`Texgen_tool.cpp:1576`/`:1596`): `GUID`, optional `Name`, `x1..y2`,
`Subroutine` (GUID), `Resolution`, `Parameter` × 16 with `ID` attributes (same as `Operator`).

### 6.5 Sample: a filter

```xml
  <texturefilter>
    <GUID>CDEDB0BD255B091EDED9D5D630AB5B75</GUID>
    <Name>Blur</Name>
    <Code>SamplerState sm:register(s0);
float4 PassData:register(c0);
float4 data1:register(c1);
Texture2D Textur:register(t0);

float4 p( float2 t:TEXCOORD0 ) : SV_TARGET0
{
 #define oneway 24.0f
 float4 res=0;
 … (HLSL body, XML-escaped: &gt; and &lt; appear for > and <) …
 return res/oneway;
}</Code>
    <Type>2</Type>
    <PassCount>6</PassCount>
    <InputCount>1</InputCount>
    <NeedsRandSeed>0</NeedsRandSeed>

    <Minifiable>1</Minifiable>
    <Parameter>
      <Name>X Amount</Name>
      <Type>0</Type>
      <Min>0</Min>
      <Max>255</Max>
      <Default>32</Default></Parameter>

    <Parameter>
      <Name>Y Amount</Name>
      <Type>0</Type>
      <Min>0</Min>
      <Max>255</Max>
      <Default>32</Default></Parameter>
  </texturefilter>
```

(The `…` line replaces a 30-line HLSL body; the whitespace-only lines between elements are the
text nodes described in §1.4, reproduced verbatim.)

---

## 7. Geometry-side sections

### 7.1 `<rendertarget>`

`CphxRenderTarget_Tool` (`RenderTargets.cpp:88`/`:98`). One element per render target.

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | GUID | targeted by `renderlayer/RenderTarget`, `event/TargetRT`, `Parameter/TextureGUID` |
| 2 | `Name` | 1 | text | |
| 3 | `ResolutionDescriptor` | 1 | u8 (`%d`) | packed resolution, see below |
| 4 | `ZResolution` | 1 | u8 | default 0 |
| 5 | `HiddenFromTimeline` | 1 | bool | tool UI only |
| 6 | `PixelFormat` | 1 | int | 0 = `RGBA16Float`, 1 = `R32Float` (`CphxRenderTarget::PixelFormat`; mapped to `COREFMT_R16G16B16A16_FLOAT` / `COREFMT_R32F` by `pixelFormats[]` in `apxProject.cpp:16`) |
| 7 | `CubeMap` | 1 | bool | cube-map target (6 faces) |

`ResolutionDescriptor` bit layout (`CreateResolutionDescription`, `RenderTargets.cpp:121`, and
`GetRenderTargetResolution` in `Phoenix/RenderLayer.cpp:3` — the latter compiled with
`PHX_HAS_NON_DEMO_RELATIVE_RENDERTARGETS`):

```
bit 7      = "demo relative" flag (0x80)
bits 6..3  = XFactor (0..7)
bits 2..0  = YFactor (0..7)

demo-relative:  XRes = DemoResX >> XFactor ;  YRes = DemoResY >> YFactor
else:           XRes = 1 << (XFactor + 5) ;   YRes = 1 << (YFactor + 5)
```

Clean Slate's `Backbuffer` has `ResolutionDescriptor = 128 (0x80)`, i.e. demo-relative with
`XFactor = YFactor = 0` → full demo resolution.

Verified sample:

```xml
  <rendertarget>
    <GUID>B804E9F623AABCBA232F247216EE216B</GUID>
    <Name>Backbuffer</Name>

    <ResolutionDescriptor>128</ResolutionDescriptor>

    <ZResolution>0</ZResolution>

    <HiddenFromTimeline>0</HiddenFromTimeline>
    <PixelFormat>0</PixelFormat>
    <CubeMap>0</CubeMap></rendertarget>
```

### 7.2 `<renderlayer>`

`CphxRenderLayerDescriptor_Tool` (`RenderLayer.cpp:22`/`:34`). A render layer is the
"pass bucket": a technique's `<TargetLayer>` names one, and the layer lists the MRT set it
renders into.

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | GUID | referenced by `rendertechnique/TargetLayer` |
| 2 | `Name` | 1 | text | |
| 3 | `OmitDepthBuffer` | 1 | bool | `OMSetRenderTargets(..., nullptr)` — no depth attachment |
| 4 | `ClearRenderTargets` | 1 | bool | `clearRenderTargets` (default true) |
| 5 | `Voxelizer` | 1 | bool | `RenderLayer.VoxelizerLayer`; also disables depth clip in the tool's raster state |
| 6 | `IgnoreHelperObjects` | 1 | bool | |
| 7 | `Pickable` | 1 | bool | runtime picking; import forces the first layer to true if none is |
| 8 | `RenderTarget` | 0..n | GUID text | **order is the MRT index order** |

Sample (the "Solid Layer" with three MRTs):

```xml
  <renderlayer>
    <GUID>0B660A129B4CFDA2A5A2D3E4E0949FA6</GUID>
    <Name>Solid Layer</Name>
    <OmitDepthBuffer>0</OmitDepthBuffer>
    <ClearRenderTargets>1</ClearRenderTargets>
    <Voxelizer>0</Voxelizer>
    <IgnoreHelperObjects>0</IgnoreHelperObjects>
    <Pickable>1</Pickable>
    <RenderTarget>B804E9F623AABCBA232F247216EE216B</RenderTarget>
    <RenderTarget>A277553146FC64CF028CD78429CB0C26</RenderTarget>
    <RenderTarget>2EAC5AF0314990354101A913294D674C</RenderTarget></renderlayer>
```

### 7.3 `<rendertechnique>`

`CphxMaterialTechnique_Tool` (`Material_Tool.cpp:220`/`:233`). A technique = render state
parameters + N shader passes.

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | GUID | referenced by `material/Tech`, `event/Tech` |
| 2 | `Name` | 1 | text | |
| 3 | `Type` | 1 | int | `TECHNIQUETYPE`: 0 `TECH_MATERIAL`, 1 `TECH_POSTPROCESS`, 2 `TECH_SHADERTOY`, 3 `TECH_PARTICLE`. Clean Slate uses 0 and 2 |
| 4 | `TargetLayer` | 1 | GUID text | must resolve to a `renderlayer`; `NONENONENONENONENONENONENONENONE` = none |
| 5 | `Parameter` | 0..n | group | **technique-level** parameters (render state; see §7.4) |
| 6 | `Pass` | 1..n | group | see below, applied in order |

Import detail: `TargetLayer` is applied *after* the passes are built, via
`Project.GetRenderLayer(TargetLayer)` → `SetTargetLayer` (a missing layer logs
`Error while importing material techinque %s: target layer not found in project!` and leaves
the tech without a layer).

**`<Pass>`** (`Material_Tool.cpp:115`/`:123`):

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `Name` | 1 | text | |
| 2 | `Code` | 1 | text (multi-line, escaped) | one HLSL blob containing **all** stages |
| 3 | `Minifiable` | 1 | bool | export hint |
| 4 | `Parameter` | 0..n | group | pass-level parameters |

Shader compilation (for an implementer mapping to SPIR-V): entry points are
`v`→vs_5_0, `p`→ps_5_0, `g`→gs_5_0, `h`→hs_5_0, `d`→ds_5_0
(`CphxMaterialRenderPass_Tool::CompileShaders`, `Material_Tool.cpp:46`). Missing stages produce
DXC error `X3501` and are ignored. Texture filters use `p`→ps_5_0 only.

### 7.4 `<Parameter>` (material parameter) — the serialized form

`CphxMaterialParameter_Tool` (`Material_Tool.cpp:457`/`:477`). Order (export):

| # | Element | Repeat | Always? | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | yes | parameter identity; referenced by `clipspline/@materialparam` and `ParamData/TargetGUID` |
| 2 | `Name` | 1 | yes | |
| 3 | `Scope` | 1 | yes | `MATERIALPARAMSCOPE`: 0 `PARAM_CONSTANT`, 1 `PARAM_VARIABLE`, 2 `PARAM_ANIMATED` |
| 4 | `Type` | 1 | yes | `MATERIALPARAMTYPE` 0..37 (§2.5) |
| 5 | `Value` | 0..1 | only if the type has a payload **and** `Scope == PARAM_CONSTANT` | base64 `MATERIALVALUE`, 16 B |
| 6 | `DefaultValue` | 0..1 | only if the type has a payload | base64 `MATERIALVALUE`, 16 B |
| 7 | `TextureGUID` | 0..1 | only for `PARAM_TEXTURE0..7`/`PARAM_RENDERTARGET` with `Scope == PARAM_CONSTANT` | GUID of a `texturefilter` save-op or `rendertarget` |

Import behaviour worth reproducing:

* `Parameter.Value` is zeroed before reading, then overwritten only if a `<Value>` decodes to
  exactly 16 bytes.
* For `Scope == PARAM_ANIMATED`, `Value` is taken from `DefaultValue` (the `<Value>` element is
  never written for animated parameters), so an animated parameter's initial value **is** its
  default.
* A `TextureGUID` whose text is not exactly 32 characters is replaced by
  `NONENONENONENONENONENONENONENONE`.
* On import the parameter's runtime `Value.TextureView`/`Value.RenderTarget` are filled from
  `TextureGUID` by `SetTextureGUID`; a rendertarget GUID sets both `Value.RenderTarget` and
  `DefaultValue.RenderTarget`.

Verbatim example of a constant `PARAM_BLENDMODE0` and a constant `PARAM_CULLMODE`:

```xml
      <Parameter>
        <GUID>3A8A183B214EE59C661D5B2CD0C18BF5</GUID>
        <Name>Blending</Name>

        <Scope>0</Scope>

        <Type>15</Type>

        <Value>AQAAAAAAAAAAAAAAAAAAAA==</Value>

        <DefaultValue>AAAAAAAAAAAAAAAAAAAAAA==</DefaultValue>
      </Parameter>

      <Parameter>
        <GUID>98665BBBCB894AC688547A609A2D96F3</GUID>
        <Name>Culling</Name>

        <Scope>0</Scope>

        <Type>5</Type>

        <Value>AQAAAAAAAAAAAAAAAAAAAA==</Value>

        <DefaultValue>AAAAAAAAAAAAAAAAAAAAAA==</DefaultValue>
      </Parameter>
```

`AQAAAAAAAAAAAAAAAAAAAA==` decodes to `01 00 …` → `BlendMode = 1` (src `phxSrcBlend_ONE`,
dst `phxSrcBlend_ZERO`) and `CullMode = 1` (`D3D11_CULL_FRONT`).

### 7.5 Constant-float stream order

A player must build the `c` constant buffer exactly as the tool does
(`CphxMaterialTechnique_Tool::CreateInstancedData`, `CphxParameterDataCollector::Apply`):

* For each pass *p*: `ConstantData` = concat( technique-level `PARAM_FLOAT`/`PARAM_COLOR`
  parameters in document order, pass-*p*-level `PARAM_FLOAT`/`PARAM_COLOR` parameters in
  document order ), taking every parameter whose `Scope != PARAM_ANIMATED`
  (`Material_Tool.cpp:666-743`). `PARAM_FLOAT` contributes 1 float, `PARAM_COLOR` contributes 4.
* Animated (`Scope == PARAM_ANIMATED`) float/color parameters go to a separate animated block
  appended after the static block (`AnimatedData`), filled at runtime by
  `CphxMaterialTechnique::CollectAnimatedData` (`Phoenix/Material.cpp:23`, `:168`) in the same
  tech-then-pass order.
* `ConstantDataSize` is clamped to `MATERIALDATASIZE = 64*sizeof(float) = 256` bytes.
* All other `Type`s affect fixed-function state, not the constant buffer.

---

## 8. `<material>`

`CphxMaterial_Tool` (`Material_Tool.cpp:1101`/`:1108`). Deliberately trivial — a material is
just an ordered list of techniques:

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `GUID` | 1 | referenced by `model/Object/Material/@GUID` and `scene/Object/Material/@GUID` |
| 2 | `Name` | 1 | |
| 3 | `Tech` | 0..n | GUID of a `rendertechnique`; unresolved GUIDs are silently dropped |

```xml
  <material>
    <GUID>81962E9A00AF53D4A3345748437A11A1</GUID>
    <Name>[PBR Mixed With Vertex Offset, forced flatshade]</Name>
    <Tech>AECBA6264493E2316649DA3F4DF38344</Tech>
    <Tech>D67B791119C88E4EAA4A95A10267E430</Tech></material>
```

A material with no `Tech` (one exists in Clean Slate) renders as nothing — the tool injects a
`DEFAULTMATERIAL!DEFAULTMATERIAL!` fallback material and `DEFAULTTECHNIQUEDEFAULTTECHNIQUE`
technique in the constructor (`apxProject.cpp:46-52`) that a player may either implement
identically or treat as "untextured/identity".

There is **no "which technique is active" flag**: the material's techniques are all rendered in
order (see `Phoenix/Model.cpp` / `Scene.cpp` material instance construction). **UNCERTAIN** for
the exact multi-technique blend rule; the exporter always writes the full list.

---

## 9. `<model>` and its objects

`CphxModel_Tool` (`Model_Tool.cpp:30`/`:42`):

```xml
  <model>
    <GUID>…</GUID>
    <Name>…</Name>
    <Object Type="n"> … </Object>   <!-- repeated; Type is the primitive id -->
    …
  </model>
```

* Every `Object` **must** carry a `Type` attribute or it is skipped.
* `<GUID>` for model objects comes from `CphxResource`; the model's `Name` is a path-like
  identifier in practice (`scenes/zebra`).
* After all objects are imported, `UpdateDependencies()` runs per object, resolving
  `parentguid`, `clonedobject` and CSG `referencedguid` references. A loader must do the same
  (two-phase).

### 9.1 `PHXMESHPRIMITIVE` (the `Object/@Type` values)

Numeric values are fixed by the enum in `Phoenix/Mesh.h`; all `PHX_MESH_*` guards are enabled in
`PhoenixConfig.h`, so the sequence is contiguous:

| Value | Name | Param count (`MeshPrimitiveParameterCounts`, `Project.cpp:107`) | Extra children |
| --- | --- | --- | --- |
| 0 | `Mesh_Cube` | 0 | |
| 1 | `Mesh_Plane` | 2 | |
| 2 | `Mesh_Sphere` | 5 | |
| 3 | `Mesh_Cylinder` | 3 | |
| 4 | `Mesh_Cone` | 4 | |
| 5 | `Mesh_Arc` | 3 | |
| 6 | `Mesh_Line` | 1 | |
| 7 | `Mesh_Spline` | 2 | (generation disabled in the runtime) |
| 8 | `Mesh_Loft` | 7 | `parentguid` × 2 (the two rails) |
| 9 | `Mesh_Clone` | 0 | `clonedobject` × n |
| 10 | `Mesh_Copy` | 0 | `parentguid` |
| 11 | `Mesh_GeoSphere` | 2 | |
| 12 | `Mesh_Scatter` | 14 | `parentguid` × 2 (target + scatter source) |
| 13 | `Mesh_Stored` | 0 | `Vertices` + `Polygons` |
| 14 | `Mesh_Tree` | 6 | `parentguid[0]` = `treespecies` GUID |
| 15 | `Mesh_TreeLeaves` | 7 | `parentguid[0]` = `treespecies` GUID |
| 16 | `Mesh_Text` | 2 | `textblob` |
| 17 | `Mesh_Marched` | 0 | |
| 18 | `Mesh_StoredMini` | 0 | `MiniVerts` + `MiniTris` |
| 19 | `Mesh_Merge` | 0 | `parentguid` × n |

Observed distribution in `cleanslate.apx` (models): types 0,1,2,3,4,5,6,8,9,10,11,12,16,18,19.

### 9.2 `Object` children, in export order

`CphxModelObject_Tool::ExportData` (`Model_Tool.cpp:431`) writes, for **every** primitive:

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `GUID` | 1 | |
| 2 | `Name` | 1 | |
| 3 | `transformation` | exactly 12 | `index="0..11"`, `value=` half-float bit pattern; see §2.7(a) |
| 4 | `parameter` | exactly 12 | `index="0..11"`, `value=` **u8 decimal**. Only the first `MeshPrimitiveParameterCounts[primitive]` are meaningful; the rest are padding written as 0 |
| 5 | `floatparameter` | 1 | single `value=` half-float bit pattern (`Object->FloatParameter`) |
| 6 | `parentguid` | exactly 2 | `index="0|1"`, `value=` GUID; up to 2 input objects |
| 7 | `Filter` | 0..n | mesh filters, applied **in document order** |
| 8 | `textblob` | 0..1 | base64 of the `Text` string bytes (no NUL); only written when non-empty |
| 9 | `MiniVerts`, `MiniTris` | 0..2 | written together only when both arrays are non-empty |
| 10 | `clonedobject` | 0..n | GUIDs (also duplicated into `ClonedGuids`; used by `Mesh_Clone`/`Mesh_Merge`) |
| 11 | `Material` | 0..1 | `<Material GUID="…"/>` — an **empty element with only an attribute** |
| 12 | `materialparamdata` | 0..n | material parameter *overrides* for this object (sibling of `Material`, **not** a child) |
| 13 | `materialtexturedata` | 0..n | texture overrides for this object (also a sibling) |

Important quirks:

* The 12 `parameter` entries are *always* written, even for primitives with 0 parameters
  (`Mesh_Cube` etc.), so a loader must use the primitive's parameter count, not the element
  count.
* `materialparamdata`/`materialtexturedata` are emitted by
  `MaterialData.ExportData(Node)` **into the Object node**, so they are siblings of
  `Material`. The same tags appear in an event's `TechData` (§10) and inside
  `Object/Filter`? — no: inside object `Filter` there are no such elements.
* `Material` is written *after* everything else in the base class export; a strict-order parser
  would break, so parse by name.

### 9.3 `MaterialData` payloads

`CphxMaterialDataStorage_Tool` (`Model_Tool.cpp:2110`/`:2130`):

```xml
<materialparamdata paramid="<MATERIALPARAM GUID>" valuebase64="<16-byte MATERIALVALUE>"/>
<materialtexturedata paramid="<MATERIALPARAM GUID>" textureid="<GUID or NONE…>"/>
```

* A `materialparamdata` whose `valuebase64` decodes to exactly 16 bytes **overrides** the
  parameter's `Value` for this object/event only. A legacy `value` attribute (int) is still
  read and stored into `MaterialParams[paramid].ZFunction`.
* `materialtexturedata` overrides the texture/rendertarget bound to that parameter; the
  sentinel `NONENONENONENONENONENONENONENONE` means "no texture" (the tool writes it when a
  parameter is missing from the material).
* Because the override dictionary is keyed by the *global parameter GUID*, an override entry is
  ignored when the referenced material does not contain that parameter
  (`UpdateParams` logs nothing; `TryImport` handles material replacement).

### 9.4 `Filter` (mesh filter)

`CphxMeshFilter_Tool` (`Model_Tool.cpp:1759`/`:1790`). The element's `Type` attribute is the
`PHXMESHFILTER` id (see §11.2) and the importer creates the right filter class from it before
reading the body. The importer ignores `Filter` elements without a `Type` attribute.

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `Name` | 1 | |
| 2 | `transformation` | exactly 12 | `index="0..11"`, `value=` half-float; **index maps directly to `srt[index]`** (§2.7(b)) |
| 3 | `parameter` | exactly 3 | `index="0..2"`, `value=` u8; only `MeshFilterParameterCounts[type]` are meaningful |
| 4 | `texture` | 0..1 | GUID of a **texgen operator** (`Project.GetTexgenOp`), not a filter/target |
| 5 | `referencedguid` | 0..1 | written only for `ModelFilter_CSG`; `value=` GUID of another model object |
| 6 | `enabled` | 1 | bool |

`MeshFilterParameterCounts` (`Project.cpp:131`):
`UVMap 2, Bevel 1, MapXForm 0, MeshSmooth 2, SmoothGroup 1, TintMesh 2, TintMeshShape 3,
Replicate 1, NormalDeform 2, CSG 2, Greeble 3` (the array stops at Greeble; `Invert`,
`SavePos2`, `NONE` have no entry — do not index past it).

Verbatim excerpt (`ModelFilter_UVMap`, type 0 — file lines 51772-51805; the twelve
`transformation` children and three `parameter` children are always present):

```xml
      <Filter Type="0">
        <Name></Name>
        <transformation index="0" value="14677"/>
      
        <transformation index="1" value="14677"/>
      
        <transformation index="2" value="14677"/>
      
        … (transformation indexes 3..5, each value="0") …

        <transformation index="6" value="15360"/>
      
        … (transformation indexes 7..11, each value="0") …

        <parameter index="0" value="3"/>
      
        <parameter index="1" value="0"/>
      
        <parameter index="2" value="67"/>
      
        <enabled>1</enabled>
      </Filter>
```

(`15360` = half 1.0; `14677` ≈ half 0.5917. `UVMap` uses 2 of its 3 parameters; the third is
padding.)

### 9.5 Stored geometry (`Mesh_Stored`, type 13)

`CphxModelObject_Tool_Mesh::ExportData`/`ImportData` (`Model_Tool.cpp:793`/`:895`). Two
encodings are read; the second is the one the current exporter writes.

**Legacy per-vertex/per-poly elements (read only):**

```xml
<Vertex Position="<base64 D3DXVECTOR3>" Normal="<base64 D3DXVECTOR3>"/>   <!-- 12 bytes each -->
<Poly v1="i" v2="i" v3="i" v4="i"/>                                      <!-- 1..4 vertices -->
```

**Current encoding:**

```xml
<Vertices VertexData="<base64: count*24 bytes of float32 xyz position, float32 xyz normal>"/>
<Polygons PolyCount="n">
  <Poly3s PolyData="<base64: m*24 bytes of StoredPolyVertex>"/>
  <Poly4s PolyData="…"/>
</Polygons>
```

* `Poly1s`, `Poly2s`, `Poly3s`, `Poly4s` are read in that fixed order (1→4), each holding
  triangles/quads of that vertex count, packed as `StoredPolyVertex { int32 index;
  float32 normal[3]; float32 uv[2]; }` = 24 bytes per polygon-vertex; the number of polygons in
  a blob is `decodedSize / (24 * vertexCount)`. Order inside a blob is polygon-major, then
  vertex.
* The blob sizes are validated only as "divisible by 24·vc"; otherwise the blob is dropped.
* On export, polygons are re-sorted so all polygons of the same vertex count are contiguous, so
  the original per-polygon order is **not** preserved (relevant only for back-conversion).

### 9.6 Mini mesh (`Mesh_StoredMini`, type 18)

```xml
<MiniVerts count="165" data="<base64 165 bytes>"/>
<MiniTris  count="267" data="<base64 267 bytes>"/>
```

* `count` equals the **decoded byte length**; the importer actually uses the decoded byte count
  (`vxDataLen`, `triDataLen`), not the attribute, so a mismatch is tolerated but should not
  occur.
* `MiniVerts` is 3 bytes per mini-vertex: from `MeshImport.cpp:199` the order is
  **`(y, x, z)`** each quantised to 0..255 over the source mesh's bounding box. `StoredVertexCount
  / 3` gives the vertex count.
* `MiniTris` is 1 byte per index, 3 indices per triangle (`StoredPolyCount / 3` triangles).
  Both arrays are therefore limited to 256 vertices.
* On export the object's `transformation` is set from the bbox scale
  (`D3DXMatrixScaling(bbSize.y, bbSize.x, bbSize.z)`).

### 9.7 Verbatim model-object samples

`Mesh_Cube` (`Type="0"`), no filters, plain material — note every repeated element is separated
by the whitespace text nodes and `Material` is self-closing:

```xml
    <Object Type="0">
      <GUID>72085CE2D246F2849FF435E4117E3488</GUID>
      <Name>Cube</Name>
      <transformation index="0" value="16988"/>

      <transformation index="1" value="0"/>

      <transformation index="2" value="0"/>

      <transformation index="3" value="0"/>

      <transformation index="4" value="0"/>

      <transformation index="5" value="11878"/>

      <transformation index="6" value="0"/>

      <transformation index="7" value="13549"/>

      <transformation index="8" value="0"/>

      <transformation index="9" value="0"/>

      <transformation index="10" value="15953"/>

      <transformation index="11" value="32768"/>

      <parameter index="0" value="99"/>
      … (parameter indexes 1..10) …
      <parameter index="11" value="0"/>

      <floatparameter value="58"/>

      <parentguid index="0" value="NONENONENONENONENONENONENONENONE"/>

      <parentguid index="1" value="NONENONENONENONENONENONENONENONE"/>

      <Material GUID="A84A65F1626DC961D09A5F4D0677DD08"/></Object>
```

`Mesh_Clone` (`Type="9"`) — the only geometry child is `clonedobject`:

```xml
    <Object Type="9">
      <GUID>F1B8E1D0578DB19E13C35F1834C63433</GUID>
      <Name>Clone</Name>
      <transformation index="0" value="48127"/>

      <transformation index="1" value="1058"/>

      <transformation index="2" value="0"/>

      <transformation index="3" value="3933"/>

      <transformation index="4" value="33826"/>

      <transformation index="5" value="48127"/>

      <transformation index="6" value="0"/>

      <transformation index="7" value="16405"/>

      <transformation index="8" value="0"/>

      <transformation index="9" value="0"/>

      <transformation index="10" value="15360"/>

      <transformation index="11" value="0"/>
      … (parameter 0..11) …
      <floatparameter value="20480"/>

      <parentguid index="0" value="NONENONENONENONENONENONENONENONE"/>

      <parentguid index="1" value="NONENONENONENONENONENONENONENONE"/>

      <clonedobject>D4AB3BA210759B2AD626A0A681AB3A1D</clonedobject>
    </Object>
```

`Mesh_Text` (`Type="16"`), showing `textblob` and per-object material overrides:

```xml
      <parentguid index="1" value="NONENONENONENONENONENONENONENONE"/>

      <textblob>bw==</textblob>

      <Material GUID="9D7DA38871AC352A1A4670C233F4AD7F"/>
      <materialparamdata paramid="8404269BD547D9C07CDC36DE505F7811" valuebase64="AAAAAAAAAAAAAAAAAAAAvw=="/>

      <materialparamdata paramid="67CF9857D4EBDBD9EB18E2688C643049" valuebase64="AAAAAAAAAAAAAAAAAAAAAA=="/>

      <materialparamdata paramid="97BF9CDBD9460D236E618FA908C38160" valuebase64="ycjIPQAAAAAAAAAAAAAAAA=="/>

      <materialparamdata paramid="405F0ED14EFE40D2AE7AF265DA5BDFA2" valuebase64="gYAAPwAAAAAAAAAAAAAAAA=="/>

      <materialparamdata paramid="7286301357B2FEC56EBEC87B4A7B704B" valuebase64="AAAAAAAAAAAAAAAAAAAAAA=="/>
      … (further materialparamdata / materialtexturedata elements) …
    </Object>
```

(The `textblob` `bw==` decodes to the single byte `o`; `ycjIPQ…` decodes to the little-endian
float32 `0.049…`, `gYAAPw…` to `0.5000…`.)

`Mesh_StoredMini` (`Type="18"`, object `507B44B6…`, file lines 51680-51909) tail — the base64
attributes are truncated here for readability (`…<N chars>` is the true attribute length):

```xml
      <MiniVerts count="165" data="ACabIQBtAABtISabACWuRCWuAEjtXkjqAAVcABRUHRJVPy+QRB5fvEC…<258 chars>"/>

      <MiniTris count="267" data="AgEABQQDBwYFCAECCggJCAoBDAsBCw4NDA8LEQoQExIQFRQTFhESFhIU…<393 chars>"/>

      <Material GUID="9D7DA38871AC352A1A4670C233F4AD7F"/>
      <materialparamdata paramid="E599D547A78CE0FBA762D53C422203B9" valuebase64="AgAAAAAAAAAAAAAAAAAAAA=="/>

      <materialparamdata paramid="4EC3695F84FCBD9FF5FE4F6DC6287871" valuebase64="AAAAAAAAAAAAAAAAAAAAAA=="/>

      <materialparamdata paramid="87DEFF81A095115449BCA9B089FB5B2A" valuebase64="//7+PgAAAAAAAAAAAAAAAA=="/>
      … (7 further materialparamdata elements, same object) …

      <materialtexturedata paramid="6DD97B71C15690B7FBFE0F10E38F7183" textureid="CAF46D5C0ADB64F6A5E4418F69BC7D9B"/>

      <materialtexturedata paramid="8B8A33FF2B0E726DDD928D6754D0C3BE" textureid="DD4357B87DAEDE6D7F10EA4B3CDF2EF2"/>

      <materialtexturedata paramid="9E5ED9B7E7A751E915C98E152D9CE64D" textureid="NONENONENONENONENONENONENONENONE"/>

      <materialtexturedata paramid="36D414B0177A3E0F0018886A4F6DC146" textureid="NONENONENONENONENONENONENONENONE"/>
    </Object>
```

This object is a `Mesh_StoredMini` (`507B44B6…`) with four mesh filters applied in order —
`Type=1` (`ModelFilter_Bevel`, params 45/0/0), `Type=0` (`ModelFilter_UVMap`, params 3/0/67),
`Type=4` (`ModelFilter_SmoothGroup`, params 255/0/67) and `Type=10` (`ModelFilter_Greeble`,
params 16/1/204) — followed by the geometry, its material (which has 14 overrides: 10
`materialparamdata` + 4 `materialtexturedata` in the file) and the `materialtexturedata`
bindings shown above. Note that the mesh filters come *before* the `Material` element, and
that a `Mesh_StoredMini` object's `transformation` is whatever the importer left in place
(here `index 0 = 11459`, `5 = 113`, `10 = 120`, `6 = 13125`, `7 = 44226`, `9 = 45787`,
`11 = 12154`), not a normalised identity.

---

## 10. `<scene>`, clips and scene objects

`CphxScene_Tool` (`Scene_tool.cpp:168`/`:187`):

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `GUID` | 1 | referenced by `Object/SubScene` clip `subscenetarget` and event `<scene>` |
| 2 | `Name` | 1 | |
| 3 | `Clip` | 1..n | scene clips (animation containers). If a scene has no `Clip`, one is created on import |
| 4 | `Object` | 0..n | `Type` attribute = `PHXOBJECT`; missing `Type` → skipped |

```xml
    <Clip>
      <GUID>256A2C067E1F94DBCEFD2875B954923D</GUID>
      <Name>New Clip</Name></Clip>
```

`CphxSceneClip` exports only `Name` (plus the inherited `GUID`). Clip GUIDs are what
`clipdata/@targetclip`, and the `clip` children of `RENDERSCENE`/`PARTICLECALC`/`CAMERAOVERRIDE`
events reference.

### 10.1 `PHXOBJECT` (`Object/@Type`)

`Phoenix/Scene.h:95`: `Object_Model = 0`, `Object_Light = 1`, `Object_CamEye = 2`,
`Object_Dummy = 3`, `Object_SubScene = 4`, `Object_ParticleEmitter = 5`,
`Object_ParticleEmitterCPU = 6`, `Object_ParticleDrag = 7`, `Object_ParticleGravity = 8`,
`Object_ParticleTurbulence = 9`, `Object_ParticleVortex = 10`, `Object_LogicObject = 11`.

In `cleanslate.apx` only types **0,1,2,3,4,6,7,8,9** occur.

Observed child order per type (from `CphxObject_Tool::ExportData` followed by the derived
class's additions — verify by the tag sequences in §10.3):

| Type | children after `GUID`,`Name` |
| --- | --- |
| 0 `Model` | `clipdata`*, (`targetobject`, `parentobject`, `camcenterx`, `camcentery` when set), `model` |
| 1 `Light` | `clipdata`*, (`parentobject`), `pointlight` |
| 2 `CamEye` | `clipdata`*, `targetobject` |
| 3 `Dummy` | `clipdata`*, (`parentobject`) |
| 4 `SubScene` | `clipdata`* only (the sub-scene link lives in the clip, not here) |
| 6 `ParticleEmitterCPU` | `clipdata`*, `shape`, `buffersize`, `innerradius`, `startcount`, `aging`, `randrotate`, `twodirrot`, `rotatetodir`, `randseed`, `sort`, [`Material` + overrides], [`EmitedObject`], [`EmitedScene`] |
| 7 `ParticleDrag` | `clipdata`*, `shape` |
| 8 `ParticleGravity` | `clipdata`*, `shape`, `directional` |
| 9 `ParticleTurbulence` | `clipdata`*, `shape` (also *read*: `randseed`, `frequency`) |
| 10 `ParticleVortex` | `clipdata`*, `shape` |

Common object children (`CphxObject_Tool::ExportData`, `Scene_tool.cpp:1267`):

| Element | Repeat | Notes |
| --- | --- | --- |
| `Name` | 1 | |
| `clipdata` | 0..n | per scene clip, see §10.2 |
| `targetobject` | 0..1 | GUID of another object in the same scene (camera looks at / cam-eye target) |
| `parentobject` | 0..1 | GUID; both are resolved in a second pass at the end of `CphxScene_Tool::ImportData` |
| `camcenterx`, `camcentery` | 0..2 | ints, written only when non-zero; off-centre projection offset, used as `camCenter/127 * r` (`Phoenix/Timeline.cpp`) |
| `model` | 0..1 | type 0: GUID of a `model` |
| `pointlight` | 1 | type 1: bool |
| `shape` | 1 | particle types: emitter/affector area type |
| `buffersize`, `innerradius`, `startcount`, `randseed` | 1 each | type 6 ints |
| `aging`, `randrotate`, `twodirrot`, `rotatetodir`, `sort` | 1 each | type 6 bools |
| `directional` | 1 | type 8 bool |
| `Material` | 0..1 | type 6 only (emitter material), `GUID=` attribute |
| `EmitedObject`, `EmitedScene` | 0..2 | type 6: GUID of an emitted `model` / `scene` (**note the misspelling `Emited`**). Read at the top of `CphxScene_Tool::ImportData` before the object is constructed |
| `materialparamdata`, `materialtexturedata` | 0..n | type 6 overrides, siblings of `Material` |

### 10.2 `clipdata` — per-clip animation data

`CphxObjectClipArray_Tool`/`CphxObjectClip_Tool` (`Scene_tool.cpp:1854`/`:1864`/`:2055`/`:2092`).
One `<clipdata targetclip="<scene Clip GUID>">` per object clip; the import matches by GUID and
logs `[phx] Failed to identify clip %s during import` if unmatched.

| # | Element | Repeat | Notes |
| --- | --- | --- | --- |
| 1 | `subscenetarget` | 0..1 | GUID of the sub-`scene` for this clip (used by `Object_SubScene`; also re-read in `UpdateSplines`) |
| 2 | `randseed` | 1 | int |
| 3 | `turbulencefreq` | 1 | int |
| 4 | `clipspline` | 0..n | `type="<PHXSPLINETYPE>"`, optional `materialparam="<GUID>"` and `groupingobject="<GUID>"`; one child `<spline>` = the spline block (§2.6) |
| 5 | `ParamData` | 0..n | animated **material** parameter splines: `<TargetGUID>` (parameter GUID), `<GroupingGUID>` (object GUID), then one `<Spline>` per colour channel (1 for `PARAM_FLOAT`, 4 for `PARAM_COLOR`) |

`clipspline` import merges by `(type, Parameter, GroupingObject)`: an existing spline with the
same key is replaced, a new one is appended. Duplicate identical splines log
`[phx] Multiple copies of the same spline found in an object!`.

`ParamData` import requires the parameter to already exist
(`Project.GetMaterialParameter(TargetGUID)`) and updates only the first matching spline entry
(which was created when the object's material batch was built); otherwise it logs
`[import] Event data import error: referenced parameter %s not found`.

### 10.3 Verbatim scene sample (smallest scene in the file)

```xml
  <scene>
    <GUID>2B0CA432E34CE5A90F6C25A5EC624210</GUID>
    <Name>asset / butterfly particle</Name>
    <Clip>
      <GUID>256A2C067E1F94DBCEFD2875B954923D</GUID>
      <Name>New Clip</Name></Clip>

    <Object Type="4">
      <GUID>29EEF5C9747C43D266E269C8379B6D59</GUID>
      <Name>asset / butterfly</Name>
      <clipdata targetclip="256A2C067E1F94DBCEFD2875B954923D">
        <subscenetarget>EBCD425760D813F5F2000FF86E5D047C</subscenetarget>

        <randseed>0</randseed>

        <turbulencefreq>10</turbulencefreq>

        <clipspline type="8">
          <spline>
            <interpolation>1</interpolation>
            <loop>0</loop>
            <waveform>0</waveform>
            <multiplicativewaveform>0</multiplicativewaveform>
            <wfamplitude>15360</wfamplitude>
            <wffrequency>18688</wffrequency>
            <wfrandseed>0</wfrandseed>
            <value id="0">0</value>

            <value id="1">0</value>

            <value id="2">0</value>

            <value id="3">0</value>
          </spline>
        </clipspline>

        <clipspline type="4">
          <spline>
            <interpolation>1</interpolation>
            <loop>0</loop>
            <value id="0">0</value>

            <value id="1">0</value>

            <value id="2">0</value>

            <value id="3">15360</value>
          </spline>
        </clipspline>

        <clipspline type="6">
          <spline>
            <interpolation>1</interpolation>
            <loop>0</loop>
            <waveform>0</waveform>
            <multiplicativewaveform>0</multiplicativewaveform>
            <wfamplitude>15360</wfamplitude>
            <wffrequency>18688</wffrequency>
            <wfrandseed>0</wfrandseed>
            <key>
              <time>0</time>
              <value id="0">0</value>
              <value id="1">0</value>
              <value id="2">0</value>
              <value id="3">0</value>
              <controlpos id="0">15</controlpos>
              <controlpos id="1">15</controlpos>
              <controlvalue id="0">0</controlvalue>
              <controlvalue id="1">0</controlvalue>
              <controlvalue id="2">0</controlvalue>
              <controlvalue id="3">0</controlvalue>
              <controlvalue id="4">0</controlvalue>
              <controlvalue id="5">0</controlvalue>
            </key>

            <key>
              <time>255</time>
              <value id="0">20672</value>
              <value id="1">0</value>
              <value id="2">0</value>
              <value id="3">0</value>
              <controlpos id="0">15</controlpos>
              <controlpos id="1">15</controlpos>
              <controlvalue id="0">0</controlvalue>
              <controlvalue id="1">0</controlvalue>
              <controlvalue id="2">0</controlvalue>
              <controlvalue id="3">0</controlvalue>
              <controlvalue id="4">0</controlvalue>
              <controlvalue id="5">0</controlvalue>
            </key>
          </spline>
        </clipspline>

        <clipspline type="55">… same shape with all-zero values …</clipspline>

        <clipspline type="56">… same shape with all-zero values …</clipspline>
      </clipdata>
    </Object>
  </scene>
```

This object is a sub-scene: everything it needs is in the clip's `subscenetarget` + the
`Spline_Position_*` (8/9/10), `Spline_Rotation` (4), `Spline_SubScene_Clip` (5),
`Spline_SubScene_Time` (6, here animating 0 → 38.0 s), `Spline_SubScene_RepeatCount` (55) and
`Spline_SubScene_RepeatTimeOffset` (56) tracks.

## 11. `<event>` — timeline entries

`CphxEvent_Tool` (`Timeline_tool.cpp:37`/`:52`). All events share a common header; the
type-specific payload follows.

| # | Element | Repeat | Type | Notes |
| --- | --- | --- | --- | --- |
| 1 | `GUID` | 1 | GUID | |
| 2 | `Type` | 1 | int | `PHXEVENTTYPE`; **an event without a parsable `Type` is skipped entirely** |
| 3 | `Pass` | 1 | int | sort/group key, **not** a render pass index (see below) |
| 4 | `StartFrame` | 1 | unsigned short | inclusive (65535 max) |
| 5 | `EndFrame` | 1 | unsigned short | exclusive (65535 max) |
| 6 | `TargetRT` | 1 | GUID text | render target this event draws into |
| 7 | `Name` | 1 | text | may be empty |
| 8 | `TimeSpline` | 1 | group | a **float16** spline block (§2.6) without a `type` attribute |
| 9.. | type-specific children | | | see below |

`PHXEVENTTYPE` (`Phoenix/Timeline.h:12`): `EVENT_ENDDEMO=0`, `EVENT_RENDERDEMO=1`,
`EVENT_SHADERTOY=2`, `EVENT_RENDERSCENE=3`, `EVENT_PARTICLECALC=4`, `EVENT_CAMERASHAKE=5`,
`EVENT_CAMERAOVERRIDE=6`. `cleanslate.apx` contains types 0, 2, 3, 4, 6 (type 1
`RENDERDEMO` is a stub, type 5 `CAMERASHAKE` is absent).

### 11.1 Type-specific payloads (export order)

**Type 2 `SHADERTOY`** (`Timeline_tool.cpp:445`/`:474`):

```xml
<Tech>GUID of a rendertechnique</Tech>
<TechData>                                   <!-- CphxMaterialDataStorage, §9.3 -->
  <materialparamdata paramid="…" valuebase64="…"/>
  <materialtexturedata paramid="…" textureid="…"/>
</TechData>
<!-- repeated, for each ANIMATED (Scope == 2) parameter: -->
<ParamData>
  <TargetGUID>parameter GUID</TargetGUID>
  <Spline>…float16 spline…</Spline>          <!-- 1 spline, or 4 for PARAM_COLOR -->
</ParamData>
```

Read order: `<Tech>` first (the technique must already exist — techniques are imported before
events), then `<TechData>`, then `<ParamData>`. An unmatched `TargetGUID` logs
`[import] Event data import error: referenced parameter %s not found`.

**Type 3 `RENDERSCENE`** (`Timeline_tool.cpp:780`/`:822`), in export order: `scene` (GUID),
`clip` (GUID of a scene `Clip`), `camera` (GUID of a scene object), `clearcolor` (bool),
`clearz` (bool). Reading is order-dependent in one respect: `clip` and `camera` are resolved
only if `scene` was resolved first, because they look the object/clip up *inside* that scene.

**Type 4 `PARTICLECALC`** (`Timeline_tool.cpp:836`/`:846`): `scene`, `clip`, `camera` — same
GUID semantics, no clear flags.

**Type 6 `CAMERAOVERRIDE`** (`Timeline_tool.cpp:1063`/`:1073`): `scene`, `clip`, `camera`.

**Type 5 `CAMERASHAKE`** (`Timeline_tool.cpp:972`/`:984`) — **not present in the test project**:

```xml
<ShakesPerSecond Value="20"/>          <!-- int stored as an ATTRIBUTE -->
<EyeIntensitySpline>…float16 spline…</EyeIntensitySpline>
<TargetIntensitySpline>…float16 spline…</TargetIntensitySpline>
```

Note that `ShakesPerSecond` carries its value in a `Value` attribute, not as element text.
On import the two splines' existing keys are flushed before reading.

**Types 0 `ENDDEMO` / 1 `RENDERDEMO`**: no extra children. `ENDDEMO` sets a global "demo
finished" flag when active; `RENDERDEMO`'s runtime body is empty.

### 11.2 Frame evaluation (how a player must drive the timeline)

`CphxTimeline::Render(Frame, tool, subroutine)` (`Phoenix/Timeline.cpp`):

```cpp
CurrentFrame = (int)Frame;
for each event in Timeline->Events (sorted, see below):
    if (StartFrame <= Frame && Frame < EndFrame) {
        float t     = (Frame     - StartFrame) / (EndFrame - StartFrame);
        float prevt = (Frame - 1 - StartFrame) / (EndFrame - StartFrame);
        if (TimeSpline) { prevt = TimeSpline->CalculateValue(prevt)[0];
                          t     = TimeSpline->CalculateValue(t)[0]; }
        event->Render(t, prevt, AspectX / (float)AspectY, subroutine);
        Target = event->Target;          // last active event wins the blit target
    }
```

* Intervals are **half-open**: `[StartFrame, EndFrame)`.
* `t` is normalized 0..1 within the event and is then *remapped* by the event's `TimeSpline`
  (a float16 spline whose `Value[0]` is the time). If `<TimeSpline>` has no `<key>` children the
  spline is constant at its `<value id="0">`; a typical event has keys at `time 0 → 0` and
  `time 255 → 15360` (i.e. linear 0→1).
* Events are sorted once after import by **`(Pass, StartFrame)`** (`EventSorter`,
  `Timeline_tool.cpp:276`) and then rendered in that order — so `Pass` is an explicit
  rendering-order/priority bucket and overlapping events in the same `Pass` are applied in
  ascending `StartFrame` order. Ties keep the original relative order in practice
  (`CArray::Sort` is not a stable sort — the original sort implementation is not in this
  release; treat tie order as **UNCERTAIN** and avoid relying on it).
* Render targets are cleared once per frame before events run; each event's own target is
  `TargetRT` (or overridden per pass by a `PARAM_RENDERTARGET` parameter).
* `AspectX/AspectY` from the root provide the `aspect` uniform.
* Audio (`<songfile>`, §5.2) only supplies the current frame
  (`frame = playPositionMs/1000 * Framerate`); the timeline is a pure function of the frame
  number, so a player can render offline/deterministically.

### 11.3 Verbatim event samples

`EVENT_RENDERSCENE` (type 3, file lines 410109-410149):

```xml
  <event>
    <GUID>7BDB8937E926EF442864192F11C40605</GUID>
    <Type>3</Type>
    <Pass>3</Pass>
    <StartFrame>21844</StartFrame>
    <EndFrame>22185</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline>
      <interpolation>1</interpolation>
      <loop>0</loop>
      <waveform>0</waveform>
      <multiplicativewaveform>0</multiplicativewaveform>
      <wfamplitude>15360</wfamplitude>
      <wffrequency>18688</wffrequency>
      <wfrandseed>0</wfrandseed>
      <key>
        <time>0</time>
        <value id="0">0</value>
        <value id="1">0</value>
        <value id="2">0</value>
        <value id="3">0</value>
        <controlpos id="0">15</controlpos>
        <controlpos id="1">15</controlpos>
        <controlvalue id="0">0</controlvalue>
        <controlvalue id="1">0</controlvalue>
        <controlvalue id="2">0</controlvalue>
        <controlvalue id="3">0</controlvalue>
        <controlvalue id="4">0</controlvalue>
        <controlvalue id="5">0</controlvalue>
      </key>
    </TimeSpline>

    <scene>4D498D4DB0870F943B90129C21E94CCA</scene>

    <clip>2647FC57DC5A1C43A69C327983123FD3</clip>

    <clearcolor>1</clearcolor>

    <clearz>1</clearz>
  </event>
```

`EVENT_SHADERTOY` (type 2, file lines 418717-418775), showing `Tech` + `TechData` with only
`materialtexturedata` overrides (this technique's parameters are all texture/rendertarget
types, so there are no `materialparamdata` entries and no `ParamData` because no parameter has
`Scope == 2`):

```xml
  <event>
    <GUID>51BCAA615D4D66D49566F62CEE118638</GUID>
    <Type>2</Type>
    <Pass>23</Pass>
    <StartFrame>0</StartFrame>
    <EndFrame>33620</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline>
      <interpolation>1</interpolation>
      <loop>0</loop>
      <waveform>0</waveform>
      <multiplicativewaveform>0</multiplicativewaveform>
      <wfamplitude>15360</wfamplitude>
      <wffrequency>18688</wffrequency>
      <wfrandseed>0</wfrandseed>
      <key>
        <time>0</time>
        <value id="0">0</value>
        … (values 1..3 and control positions/values, all 0) …
      </key>

      <key>
        <time>255</time>
        <value id="0">15360</value>
        … (values 1..3 and control positions/values, all 0) …
      </key>
    </TimeSpline>

    <Tech>26F0435A6BCBCCD866EE3E93DCF79AA2</Tech>

    <TechData>
      <materialtexturedata paramid="6D904C1B670733B387A15EEF7B672488" textureid="B804E9F623AABCBA232F247216EE216B"/>

      <materialtexturedata paramid="2AEC6E83794B135498A1CE0663541CF9" textureid="A9E771D904D13ABD97C18A6AD855585A"/>

      <materialtexturedata paramid="30CCB8D26D8C123F15C2DB9C3827D362" textureid="B804E9F623AABCBA232F247216EE216B"/>
    </TechData>
  </event>
```

The matching technique (file lines 48042-48094) — `TECH_SHADERTOY` with one pass and three
variable-scope texture parameters:

```xml
  <rendertechnique>
    <GUID>26F0435A6BCBCCD866EE3E93DCF79AA2</GUID>
    <Name>Palette</Name>
    <Type>2</Type>
    <TargetLayer>0B660A129B4CFDA2A5A2D3E4E0949FA6</TargetLayer>

    <Pass>
      <Name>New Pass</Name>
      <Code>SamplerState sm:register(s1);
Texture2D Textur:register(t0);
Texture2D Palette:register(t1);

float4 p( float4 t:SV_POSITION ) : SV_TARGET0
{
	float4 col=Textur.Load(int3(t.xy,0));
	float4 ret;
	ret.x=Palette.SampleLevel(sm,float2(saturate(col.x),0),0).x;
	ret.y=Palette.SampleLevel(sm,float2(saturate(col.y),0),0).y;
	ret.z=Palette.SampleLevel(sm,float2(saturate(col.z),0),0).z;
	ret.w=Palette.SampleLevel(sm,float2(saturate(col.w),0),0).w;
	return ret;
}</Code>
      <Minifiable>1</Minifiable>
      <Parameter>
        <GUID>6D904C1B670733B387A15EEF7B672488</GUID>
        <Name>RenderTarget</Name>

        <Scope>1</Scope>

        <Type>23</Type>
      </Parameter>

      <Parameter>
        <GUID>30CCB8D26D8C123F15C2DB9C3827D362</GUID>
        <Name>Input</Name>

        <Scope>1</Scope>

        <Type>7</Type>
      </Parameter>

      <Parameter>
        <GUID>2AEC6E83794B135498A1CE0663541CF9</GUID>
        <Name>Palette</Name>

        <Scope>1</Scope>

        <Type>8</Type>
      </Parameter>
    </Pass>
  </rendertechnique>
```

Note the parameter order does **not** match the HLSL register order (`t0`, `t1`) — registers are
assigned by parameter *index* (`Type - PARAM_TEXTURE0` → `Textures[]` slot), and this pass's
`Input` (type 7) fills `Textures[0]` while `Palette` (type 8) fills `Textures[1]` and
`RenderTarget` (type 23) is not a texture slot at all. The event's `TechData` overrides bind
actual resources to those parameters.

### 11.4 `<treespecies>`

`CphxTreeSpecies` (`TreeSpecies.cpp`). Only present when procedural trees are used; absent from
both test projects. Children, in export order (all are scalar ints unless noted):

```
GUID, Name,
Levels, Lobes, _0BaseSplits, LeafShape, Shape, LeafDistrib,
BaseSize, LeafQuality, LeafBend, Smooth, Flare, Ratio,
AttractionUp, Leaves, RatioPower, PruneRatio, PruneWidth, PruneWidthPeak,
LobeDepth*, Scale*, _0Scale*, _0ScaleV*, LeafScale*, LeafScaleX*, LeafStemLen*,
PrunePowerLow*, PrunePowerHigh*,          (* = half-float bit pattern, §2.3)
Level (repeated; attribute Level="0..4"), with children
    nBranches, nCurveRes, nTaper, nSplitAngle, nSplitAngleV, nBranchDist,
    nRotate, nRotateV, nDownAngle, nDownAngleV, nCurve, nCurveV, nCurveBack,   (ints)
    nLength*, nLengthV*, nSegSplits*                                            (half-floats)
```

`Level` has a `Level` attribute (not `index`); exactly `Levels + 1` of them are written, the
importer skips levels with `Level < 0 || Level > 4`. A player that implements
`Mesh_Tree`/`Mesh_TreeLeaves` needs this section plus `parentguid[0]`; otherwise it can be
skipped.

---

## 12. Tool-authoring state a player can ignore

Everything below is verified to have no rendering consumer in the release (grep for the
corresponding member across `apEx/Phoenix/`), or is explicitly UI-only:

| Item | Reason |
| --- | --- |
| root `build` attribute | never read |
| `shaderminifierenabled`, `globalshaderminifierenabled`, `minimalprecalc`, `farbrauschprecalc`, `setuphassocial` | read into project flags used only by the exporter/precalc pipeline |
| `gridbpm`, `gridprimaryhighlight`, `gridsecondaryhighlight` | timeline editor grid snapping |
| `Title`, `Group`, `www`, `pouet`, `facebook`, `youtube`, `share` | social/export metadata |
| `texturefilter/Type` | filter-editor list cursor (`TexGenFilterEditor.cpp:218`); no runtime reader |
| `texturefilter/Minifiable`, `Parameter/ListItem`, `Parameter/Min`, `Parameter/Max` | editor UI / shader minifier |
| operator `Name` | display only (but `Save` names are conventionally the save-slot labels) |
| operator `x1,y1,x2,y2` | display **and** graph topology — see §6.3; **not ignorable** |
| `renderlayer/HiddenFromTimeline` | editor UI |
| `renderlayer/Pickable`, `IgnoreHelperObjects` | picking/hide-helper affordances; `Pickable` has no render effect |
| `rendertechnique/Pass/Minifiable` | export hint |
| model object `parameter` padding beyond the primitive's parameter count | unused bytes |
| model object `Filter/Name` | UI only (`Filter/enabled` **is** functional: `CphxMeshFilter_Tool::Enabled` gates the filter) |
| `MiniVerts`/`MiniTris` for non-`Mesh_StoredMini` objects | never written for other types |
| `materialparamdata/@value` (legacy int attribute) | only fills `ZFunction`; superseded by `@valuebase64` |
| `scene` object `clipdata` for unused clips | harmless, but wastes memory |
| `event/Pass` | ordering key only (still functionally required — see §11.2) |
| `treespecies` (unless trees are used) | generator presets |
| external library merge (`Data/*.xml`) | the `.apx` already inlines every referenced resource |

Note: `renderlayer/ClearRenderTargets` *is* functional — it gates both the colour and depth
clears in `CphxRenderLayerDescriptor::SetEnvironment` (`Phoenix/RenderLayer.cpp`).

---

## 13. Appendix — enumerations

Copied verbatim from the release headers (values are what appear in the XML).

```cpp
// Phoenix/Material.h
enum MATERIALPARAMSCOPE { PARAM_CONSTANT = 0, PARAM_VARIABLE = 1, PARAM_ANIMATED = 2 };
enum MATERIALPARAMTYPE {
  PARAM_FLOAT = 0, PARAM_COLOR, PARAM_ZMODE, PARAM_ZFUNCTION, PARAM_FILLMODE, PARAM_CULLMODE,
  PARAM_RENDERPRIORITY, PARAM_TEXTURE0, PARAM_TEXTURE1, PARAM_TEXTURE2, PARAM_TEXTURE3,
  PARAM_TEXTURE4, PARAM_TEXTURE5, PARAM_TEXTURE6, PARAM_TEXTURE7,
  PARAM_BLENDMODE0, PARAM_BLENDMODE1, PARAM_BLENDMODE2, PARAM_BLENDMODE3, PARAM_BLENDMODE4,
  PARAM_BLENDMODE5, PARAM_BLENDMODE6, PARAM_BLENDMODE7,
  PARAM_RENDERTARGET, PARAM_PARTICLELIFEFLOAT, PARAM_DEPTHTEXTURE7, PARAM_3DTEXTURE6,
  PARAM_MESHDATA0, PARAM_MESHDATA1, PARAM_MESHDATA2, PARAM_MESHDATA3, PARAM_MESHDATA4,
  PARAM_MESHDATA5, PARAM_MESHDATA6, PARAM_MESHDATA7,
  PARAM_PARTICLELIFE, PARAM_LTC1, PARAM_LTC2, PARAM_COUNT
};                                  // PARAM_COUNT == 38
enum TECHNIQUETYPE { TECH_MATERIAL = 0, TECH_POSTPROCESS = 1, TECH_SHADERTOY = 2, TECH_PARTICLE = 3 };

// apEx blend byte: low nibble = source, high nibble = destination (apEx codes, NOT D3D11 enums)
#define phxSrcBlend_ZERO 0x00 ... phxSrcBlend_INVBLENDFACTOR 0x0e
#define phxDstBlend_ZERO 0x00 ... phxDstBlend_INVBLENDFACTOR 0xe0
// D3D11_BLEND = apEx code + 1  (the tool applies (+0x11) per nibble; see §2.5)

// Phoenix/Mesh.h  (all PHX_MESH_* guards are enabled by PhoenixConfig.h)
enum PHXMESHPRIMITIVE { Mesh_Cube=0, Mesh_Plane=1, Mesh_Sphere=2, Mesh_Cylinder=3, Mesh_Cone=4,
  Mesh_Arc=5, Mesh_Line=6, Mesh_Spline=7, Mesh_Loft=8, Mesh_Clone=9, Mesh_Copy=10,
  Mesh_GeoSphere=11, Mesh_Scatter=12, Mesh_Stored=13, Mesh_Tree=14, Mesh_TreeLeaves=15,
  Mesh_Text=16, Mesh_Marched=17, Mesh_StoredMini=18, Mesh_Merge=19 };

enum PHXMESHFILTER { ModelFilter_UVMap=0, ModelFilter_Bevel=1, ModelFilter_MapXForm=2,
  ModelFilter_MeshSmooth=3, ModelFilter_SmoothGroup=4, ModelFilter_TintMesh=5,
  ModelFilter_TintMeshShape=6, ModelFilter_Replicate=7, ModelFilter_NormalDeform=8,
  ModelFilter_CSG=9, ModelFilter_Greeble=10, ModelFilter_Invert=11, ModelFilter_SavePos2=12,
  ModelFilter_NONE /* tool only */ };

// Phoenix/Scene.h
enum PHXOBJECT { Object_Model=0, Object_Light=1, Object_CamEye=2, Object_Dummy=3,
  Object_SubScene=4, Object_ParticleEmitter=5, Object_ParticleEmitterCPU=6, Object_ParticleDrag=7,
  Object_ParticleGravity=8, Object_ParticleTurbulence=9, Object_ParticleVortex=10,
  Object_LogicObject=11 };
enum PHXSPLINETYPE {
  Spline_MaterialParam=0, Spline_Scale_x=1, Spline_Scale_y=2, Spline_Scale_z=3, Spline_Rotation=4,
  Spline_SubScene_Clip=5, Spline_SubScene_Time=6, Padding0=7,
  Spline_Position_x=8, Spline_Position_y=9, Spline_Position_z=10, Spline_Position_w=11,
  Spline_Light_AmbientR=12, Spline_Light_AmbientG=13, Spline_Light_AmbientB=14, Padding1=15,
  Spline_Light_DiffuseR=16, Spline_Light_DiffuseG=17, Spline_Light_DiffuseB=18, Padding2=19,
  Spline_Light_SpecularR=20, Spline_Light_SpecularG=21, Spline_Light_SpecularB=22, Padding3=23,
  Spot_Direction_X=24, Spot_Direction_Y=25, Spot_Direction_Z=26, Padding4=27,
  Spline_Light_Exponent=28, Spline_Light_Cutoff=29, Spline_Light_Attenuation_Linear=30,
  Spline_Light_Attenuation_Quadratic=31, Padding7=32,
  Spline_Camera_FOV=33, Spline_Camera_Roll=34, Padding8=35, Padding9=36,
  Spline_Particle_Offset_x=37, Spline_Particle_Offset_y=38, Spline_Particle_Offset_z=39,
  Spline_Particle_EmissionPerSecond=40, Spline_Particle_EmissionTrigger=41,
  Spline_Particle_EmissionVelocity=42, Spline_Particle_Life=43,
  Spline_Particle_EmissionRotation=44, Spline_Particle_EmissionVelocityChaos=45,
  Spline_Particle_EmissionRotationChaos=46, Spline_Particle_LifeChaos=47,
  Spline_Light_OrthoX=48, Spline_Light_OrthoY=49, Spline_AffectorPower=50,
  Spline_Particle_Scale=51, Spline_Particle_ScaleChaos=52, Spline_Particle_Stretch_X=53,
  Spline_Particle_Stretch_Y=54, Spline_SubScene_RepeatCount=55, Spline_SubScene_RepeatTimeOffset=56,
  Spline_Count };

// Phoenix/Timeline.h
enum PHXEVENTTYPE { EVENT_ENDDEMO=0, EVENT_RENDERDEMO=1, EVENT_SHADERTOY=2, EVENT_RENDERSCENE=3,
  EVENT_PARTICLECALC=4, EVENT_CAMERASHAKE=5, EVENT_CAMERAOVERRIDE=6, Event_Count };

// Phoenix/phxSpline.h
enum SPLINEINTERPOLATION { INTERPOLATION_CONSTANT=0, INTERPOLATION_LINEAR=1,
  INTERPOLATION_CUBIC=2, INTERPOLATION_BEZIER=3 };
enum SPLINEWAVEFORM { WAVEFORM_NONE=0, WAVEFORM_SIN=1, WAVEFORM_SQUARE=2, WAVEFORM_TRIANGLE=3,
  WAVEFORM_SAWTOOTH=4, WAVEFORM_NOISE=5 };
#define SPLINEPOSITIONCALC(t) ( t ? ( t + 1.0f ) / 256.0f : 0 )

// Phoenix_Tool/Texgen_tool.h
enum APEXTEXTUREOPTYPE { TEXGEN_OP_NORMAL=0, TEXGEN_OP_SAVE=1, TEXGEN_OP_LOAD=2, TEXGEN_OP_NOP=3,
  TEXGEN_OP_SUBROUTINE=4, TEXGEN_OP_SUBROUTINECALL=5 };
#define TEXGEN_MAX_PARAMS 16                       // Phoenix/Texgen.h
#define MATERIALDATASIZE (64*sizeof(float))        // Phoenix/phxEngine.h == 256

// Phoenix_Tool/phxResource.h
enum PHXRESOURCETYPE { PHX_TEXTUREFILTER=0, PHX_TEXTUREOPERATOR=1, PHX_MATERIALTECH=2,
  PHX_RENDERTARGET=3, PHX_EVENT=4, PHX_MATERIALPARAM=5, PHX_MODEL=6, PHX_MODELOBJECT=7,
  PHX_MATERIAL=8, PHX_RENDERLAYERDESCRIPTOR=9, PHX_SCENE=10, PHX_OBJECT=11, PHX_CLIP=12,
  PHX_TREESPECIES=13 };

// Phoenix/RenderTarget.h
enum PixelFormat : unsigned char { RGBA16Float = 0, R32Float = 1 };
// mapped to COREFMT_R16G16B16A16_FLOAT (0) / COREFMT_R32F (1) by pixelFormats[] (apxProject.cpp:16)
```

---

## 14. Loader checklist / gotchas

1. **Parse by name, never by position.** All children are located by exact tag name and (for
   repeats) by the *n*-th occurrence of that name. Document order of differently named
   siblings is not significant.
2. **Ignore whitespace text nodes.** They exist (see §1.4) and vary between elements; a DOM
   that keeps them will otherwise break naive positional navigation.
3. **GUIDs are 32-char uppercase ASCII hex strings.** Compare verbatim; treat
   `NONENONENONENONENONENONENONENONE` as "none" and `BADGUID!…` as "broken".
4. **Half-floats are decimal integers 0..65535 holding IEEE binary16 bits**, not floats. Do not
   run `atof` on them. A handful of fields that look like ints are half-floats:
   `transformation/@value`, `floatparameter/@value`, spline `value`/`controlvalue`/
   `wfamplitude`/`wffrequency`, `treespecies` scalars, particle `frequency`.
5. **base64 has no line breaks** and stops at `=`/non-alphabet; validate the decoded length
   against the expected struct size before use (the original code does).
6. **Two-pass imports are mandatory**: all scene GUIDs → then scene bodies; all model objects →
   then `UpdateDependencies`; all scene objects → then target/parent links; all texture pages →
   then `LoadedOp`/subroutine GUID links.
7. **Rebuild the texgen operator graph from geometry**, not from document order (§6.3).
8. **Sort events by `(Pass, StartFrame)`** before rendering, and use the half-open
   `[StartFrame, EndFrame)` interval plus the event `TimeSpline` remap.
9. **Range-check `index`/`ID`/`id` attributes** before use: the original code writes them
   straight into fixed arrays (`OpData.Parameters[16]`, `Parameters[12]`, `srt[12]`,
   `TransformationF16[12]`, `Spline->Value[4]`, `controlvalues[6]`, `controlpositions[2]`).
10. **`materialparamdata`/`materialtexturedata` are siblings of `Material`**, not children, and
    are keyed by global material-parameter GUID — they only apply to objects/events whose
    material contains that parameter.
11. **Texture references resolve to different resource kinds**: `Parameter/TextureGUID` may name
    either a `rendertarget` or a texture-page **operator** (`Save`), depending on the parameter
    type; `Filter/texture` names a texgen operator; `Load/@LoadedOp` names a `Save` operator.
12. **`material` has no "active technique" flag** — all listed techniques are used.
13. **`<songfile>` needs Windows/POSIX path normalisation** and a two-step fallback (CWD, then
    the `.apx` directory), as in §5.2.
14. **A project may legally lack any section** (e.g. `Precalc.apx` has no scalars 11–15 and no
    `model`/`scene`), so initialise everything to sane defaults.

---

## 15. Explicitly uncertain items

* **Writer byte-for-byte details** (encoding, BOM, LF vs CRLF, presence of an XML declaration).
  The two shipped files are ASCII/LF/declaration-less; `CXMLDocument::SaveToString` uses
  MSXML's `get_xml`, which historically emits a declaration and CRLF. The loader must not care.
  (§1.1)
* **`texturefilter/LookupType == 4`** — only `GetLookupTexture`'s guard
  (`LookupType != 4` may be called without extra data) was traced; Clean Slate's `CellEdges`
  filter uses it and no `image`/`textblob`/`spline` children are written for it. The exact
  semantics of 4 are not determined. (§6.1)
* **`texturefilter/Type`** — stored and edited through a UI list; no runtime consumer found.
  Assumed ignorable. (§6.1)
* **Blend-mode nibble conversion asymmetry** — the tool's `CphxParameterDataCollector`
  (`Material_Tool.cpp:762`) applies `(BlendMode + 0x11)` before splitting the nibbles, while the
  runtime's `CollectRenderState` (`Phoenix/Project.cpp`) splits them raw. Both paths are in the
  release; the `.apx`-loading tool path is the one with `+0x11`, which maps the apEx blend codes
  to `D3D11_BLEND` (code + 1). A loader should apply `+1` per nibble and re-verify against a
  known-good frame. (§2.5)
* **`PARAM_PARTICLELIFE` (35) payload width** — `MaterialParamHasNonGUIDData` returns true and
  `IsDefault()` returns false, but no reader of `Value` for this type was found in the release.
  `PARAM_PARTICLELIFEFLOAT` (24) is a `float`; assume 35 is also a float at offset 0 until
  contradicted. (§2.5)
* **Multiple techniques per material** — the code renders through each technique's own batch
  list, but the exact compositing/blend rule across a material's technique list was not traced
  in `Phoenix/Model.cpp`/`Scene.cpp`. (§8)
* **Event ordering on equal `(Pass, StartFrame)`** — depends on `CArray::Sort`, whose
  implementation is not present in this release. Avoid relying on it. (§11.2)
* **`scene`/`Object Type=4` legacy `<scene>` child** — `CphxScene_Tool::ImportData` still reads
  a `scene` child for `Object_SubScene`, but the exporter never writes it; the live link is
  `clipdata/subscenetarget`. Reading both (with the clip taking precedence) is safest. (§10.1)
* **`apxmat` (`apExMaterial`) files** — the root name and content are established from the
  exporter/importer, but no `.apxmat` sample was available to verify end to end.

---

## 16. Sample provenance

All XML excerpts in this document are verbatim line ranges of
`~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx`, except where a line
contains `…`, which marks text removed for brevity. The relevant file line numbers are quoted
next to each excerpt. No sample was retyped by hand; they were extracted with Python
(`xml.etree.ElementTree` for structure/statistics and a raw line-range extractor for the
verbatim text, so indentation, whitespace-only text-node lines and XML escaping are preserved).



---

## 17. Corrections and additions found by writing a project by hand

§1–16 were derived from reading the code and the shipped files.  Building a
minimal project from scratch (`work/hostile/minimal-draw.apx`) and a set of
deliberately hostile ones (`tools/make_hostile_apx.py`) turned up these:

* **§7.4 is wrong about `CullMode`.** The byte goes straight into
  `D3D11_RASTERIZER_DESC::CullMode` (`Material_Tool.cpp:770`), where `1` is
  `D3D11_CULL_NONE`, not `D3D11_CULL_FRONT`.  `1` is the winding-independent
  choice.
* **§7.5 describes only part of `cbuffer b1`.** The engine prepends `worldmat`
  and `itworldmat` (128 bytes) *before* `MATERIALDATASIZE` (256) --
  `phxEngine.cpp:183`, `RenderLayer.cpp:65` -- so a shader that declares `c` as
  `{float4x4 worldmat; float4x4 itworldmat; float4 color; ...}` is right and one
  that declares only the material parameters is not.
* **The vertex shader's input declaration ORDER is load-bearing.** The shim pairs
  `D3D11_INPUT_ELEMENT_DESC[i]` with glslang location `i`, and glslang assigns
  locations by declaration order, so a `VSIN` must repeat `vxdecl2`'s order
  exactly (POSITION0, POSITION1, NORMAL0, COLOR0, TEXCOORD0, TEXCOORD1).
  Declaring only the fields you use silently shifts every later semantic.
* **`<event>` needs explicit `StartFrame`/`EndFrame`.** An event created with
  `CreateEvent(type, 0, 0, 0, NULL)` zero-inits both, and the activity test is
  `StartFrame <= Frame < EndFrame`, so such an event is never active.
* **`<camera>` is optional** and is auto-picked from the scene's first
  `Object_CamEye`, preferring one with a `<targetobject>`.  Camera *direction* is
  not defaulted by `CphxObject::CalculateAnimation`, so a camera at the origin
  with no `targetobject` gives a degenerate look-at.
* **`Mesh_Plane`'s parameters are subdivision counts, not extents**: the plane
  always spans -0.5..+0.5 in XZ with normal +Y, so the object's
  `<transformation>` has to scale and rotate it, and `Data[0] = 0` divides by
  zero.
* **A scene's `<Clip>` must precede its `<Object>` children**: `ImportData` reads
  the clips first and `AddObject` makes one in-memory clip per existing scene
  clip, so an object with no clip has a null `Clips[]` at render time.
* **The loader trusts the file in ways that crash it** -- an unknown
  `<event><Type>`, an unknown scene `<Object Type=...>` or an unresolvable
  `<model>` GUID, and an out-of-range `<clipspline type=...>`.  See MILESTONE 10
  in `docs/STATUS.md` for the sites and the fixes.

### Corrections found by building a synthwave project from nothing

Written by taking §14 at face value and authoring a project with no scene,
camera, model or material at all -- one render target, one layer, one
`TECH_SHADERTOY` technique and one event.  Each of these cost an iteration.

* **`<StartFrame>` / `<EndFrame>` are `unsigned short`, not `int`** (§11's table
  says int).  `Phoenix/Timeline.h:32` is
  `unsigned short StartFrame, EndFrame;`, so the ceiling is **65535 frames**
  and an `<EndFrame>216000</EndFrame>` silently wraps: the player logs
  `frames [0, 19392)` (216000 - 3x65536) and the event runs for a sixth of the
  time asked for.  Nothing warns.
* **A `TECH_SHADERTOY` pass only writes somewhere if the event binds a render
  target to it.**  `<event><TargetRT>` selects what the final blit *reads*; it
  does **not** give the pass somewhere to draw.  That needs a `<TechData>` entry
  in the event:
  `<materialtexturedata paramid="<PARAM_RENDERTARGET guid>" textureid="<rendertarget guid>"/>`
  with the parameter declared `<Scope>1</Scope><Type>23</Type>`
  (`PARAM_VARIABLE`), as Clean Slate's *Mapdistort* does.  Omit it and the pass
  draws into nothing: black composite, **no `(Err)` line at all**.
* **`b0.x` in a shadertoy is the event's normalised 0..1 time, not seconds.**
  `CphxEvent_Shadertoy::Render` fills `ShaderData = {t, aspect, xres, yres}` from
  the timeline's `t = (Frame - StartFrame) / (EndFrame - StartFrame)` (§11's
  formula), so a shader that wants seconds multiplies by its own duration.
* **Author in linear light.**  The final blit uses an sRGB render-target view, so
  a shadertoy computing display-space colour must output `pow(c, 2.2)` or the
  frame comes out dark and desaturated.  Useful diagnostic: `--dump-frame F`
  writes the sRGB back buffer and `F.target.ppm` the linear render target;
  diffing the two separates an authoring-gamma mistake from a blit bug.
* **No `<scene>`, `<camera>`, `<model>` or `<material>` is required** for a
  shadertoy-only project.  The minimal working set is a `<rendertarget>`, a
  `<renderlayer>`, a `<rendertechnique Type="2">` with one `<Pass>`, and an
  `<event Type="2">`.  The `Data//Arboretum.xml`, `FilterLibrary.xml` and
  `MaterialLibrary.xml` warnings on load are pre-existing and harmless when the
  project has no texgen or materials.
* `fwidth` / `ddx` / `ddy` work inside a `TECH_SHADERTOY` `p`.  `ddx` of a
  computed height is a free facet normal, which replaced two extra height-field
  evaluations and took the pass from 11 ms to 6.1 ms at 1384x779.
* A `<TimeSpline>` with keys 0->0 and 255->15360 gives a clean linear 0..1 ramp,
  and a shadertoy declaring only `float4 b0` accepts the engine's
  `{t, aspect, xres, yres}` plus matrices without complaint.

### Corrections found by building a real 3D scene

Second pass, this time with real geometry: one `Mesh_Plane` at 128x128, three
instances sharing the model, a vertex shader that displaces and scrolls the
height field, a pixel shader drawing the neon grid, and a shadertoy layer for the
sky.  Four more things that cost iterations:

* **A `ParamData` `<Spline>` must contain the spline body directly**
  (`<interpolation>`, `<key>`, ...).  Wrapping it in an extra `<spline>` element
  imports with **no error at all** and yields an all-zero spline, so the animated
  parameter stays 0 -- in this case a scale of 0, i.e. a completely blank scene
  with a clean log.  This was the expensive bug of the exercise.
* **There is no time uniform for `TECH_MATERIAL`.**  Animation comes from an
  animated material parameter (`Scope 2`, `PARAM_FLOAT`) driven by the scene
  object's `clipdata` `ParamData` spline, and its value is the **normalised 0..1
  event time**, not seconds -- so any speed expressed in it depends on the
  event's `EndFrame` unless the shader rescales it.
* **`ParamData`'s `GroupingGUID` must be the model-object GUID**, not the model's
  or the scene's.
* **A layer with `ClearRenderTargets=0` suppresses the depth clear too.**  To
  composite a shadertoy sky under a scene, the layer needs
  `ClearRenderTargets=1` with the event carrying `clearcolor=0` and `clearz=1`.
* Also confirmed: `fwidth`/`ddx`/`ddy` work in a **scene** pixel shader as well,
  not only in a shadertoy; there is no instancing (N instances == N scene
  objects sharing one model); and 65535 frames is 18.2 minutes at 60 fps with no
  way to restart the timeline, so a periodic animation is the way to make it
  endless.
