# D3D11-subset-on-Vulkan shim — implementation spec for the apEx player

**Target deliverable:** a cross-platform Vulkan player for demos exported by the `apEx` demotool.
**Strategy specified here:** do **not** write a new CoRE2 backend and do **not** port `apEx/Phoenix` to a new
rendering API. Instead ship a **`d3d11.h` / `dxgi.h` / `d3dx10math.h` compatible shim** whose `ID3D11*`
objects are backed by Vulkan, and compile the *existing* DX11 code paths
(`Bedrock/CoRE2/DX11*.cpp`, `apEx/Phoenix/*.cpp`, `apEx/MinimalPlayer/WindowHandler.cpp`) unmodified or
near-unmodified against it. HLSL is compiled to SPIR-V at runtime with
libshaderc/glslang instead of `fxc`/`D3DCompile`.

Source analysed (read-only): `~/Repos/apEx-public/`
Scope directories: `apEx/Phoenix/`, `apEx/Phoenix_Tool/`, `Bedrock/CoRE2/`, `apEx/MinimalPlayer/`
(plus `apEx/ThirdParty/D3D/Include/` which is *bundled in-repo* and is the authoritative source for
D3DX signatures used in this document).

Every claim below was verified by scripted grep/parser passes over comment-stripped source
(comments and string literals blanked **without** changing line numbers, so all `file:line`
citations are exact). Items that could not be resolved from the source alone are marked
**`UNCERTAIN:`** with the evidence and the reason.

---

## 0. Executive summary

### 0.1 The shim is a *drop-in header + runtime*, not a re-implementation of the engine

`apEx/Phoenix` bypasses CoRE2 wherever it matters. `phxEngine.h:16-43` declares ~25 **global raw
D3D11 pointers** (`phxDev`, `phxContext`, `phxBackBufferView`, `phxDepthBufferView`, …) and the engine
calls `ID3D11Device`/`ID3D11DeviceContext` directly throughout. At the same time
`apEx/Phoenix_Tool/*.cpp` converts *abstract* CoRE2 objects back into raw D3D11 pointers via
`GetHandle()` and stores them in structures the **player** also uses:

| file:line | code |
|---|---|
| `apEx/Phoenix_Tool/Material_Tool.cpp:105-109` | `Pass->VS = (ID3D11VertexShader*)VS->GetHandle();` … `HS` |
| `apEx/Phoenix_Tool/Scene_tool.cpp:128-149` | `i->VS = (ID3D11VertexShader*)DefaultVertexShader->GetHandle();` etc. |
| `apEx/Phoenix_Tool/Texgen_tool.cpp:139` | `Filter.PixelShader = (ID3D11PixelShader*)Shader->GetHandle();` |
| `apEx/Phoenix/RenderLayer.h:22-37` | `CphxRenderDataInstance` holds `ID3D11Buffer*`, `ID3D11VertexShader*`, `ID3D11RasterizerState*`, `ID3D11ShaderResourceView* Textures[8]` |

So the "abstract" boundary is leaky *by construction*, and the D3D11 types must remain real, usable
handle types across the whole codebase. Providing a Vulkan backend for CoRE2 would **not** remove the
need for the D3D11 shim; it would be a parallel, largely redundant effort. Spec accordingly:
**one shim, both consumers.**

### 0.2 What this buys

Assuming the shim header reproduces the D3D11 declarations bit-compatibly (see §1.4 — several
aggregate initialisers depend on exact field order) and the runtime implements the surface below,
**all five** of these compile unchanged:

* `Bedrock/CoRE2/DX11Device.cpp`, `DX11Texture.cpp`, `DX11Shader.cpp`, `DX11VertexBuffer.cpp`,
  `DX11IndexBuffer.cpp`, `DX11VertexFormat.cpp`, `DX11RenderState.cpp`, `DX11ConstantBuffer.cpp`,
  `DDSTextureLoader.cpp`
* `apEx/Phoenix/{phxEngine,Project,Scene,Timeline,Texgen,RenderLayer,Mesh,Model,Material,Arbor,TreeGen}.cpp`
* `apEx/MinimalPlayer/WindowHandler.cpp` (device + swapchain creation) — with one source fix, see §1.4.9
* `apEx/Phoenix_Tool/*` (the tool also builds, but see §1.7 — a handful of tool-only D3DX11/`IDXGIOutput`
  calls can be stubbed)

### 0.3 The five findings that most change the plan

1. **`ID3D11ShaderReflection` is 100 % dead code.** Every reflection call site is commented out
   (§2.7). There is no `GetConstantBufferByName`, no `GetInputParameterDesc`, no `D3DReflect` call
   anywhere in scope. Descriptor layout can therefore be *convention-based*
   (`b0`/`b1` → scene/object CB, `t0..t7` → SRVs, `s0..s2` → samplers, verified against every exported
   `.apx` in `Projects/`, §2.8), **not** driven by SPIR-V reflection. This removes the single largest
   piece of anticipated work.
2. **Shaders are HLSL *source* in the asset file, compiled at runtime** (`Project.cpp:340-359`,
   `WriteASCIIZ(Stream_Shaders, …)` at `apEx/apEx/MinimalExport.cpp:2940-2948`; confirmed by `strings`
   over 10 real `.apx` projects). There is no DXBC in the assets, so no DXBC→SPIR-V translator is needed —
   but the *whole* SM5.0 subset actually used must be covered by the glslang HLSL frontend (§2.9.4).
3. **D3D *packing* of `cbuffer` members is a hard correctness dependency.** Shaders declare
   `cbuffer b:register(b0){ float t,aspect,xres,yres; }` and C++ `memcpy`s 4 tightly-packed floats
   (`Timeline.cpp:43-45,107`, `Texgen.cpp:123,151-156`). Under `std140` those four scalars occupy 64
   bytes, not 16. The HLSL→SPIR-V path must emit D3D-compatible explicit member offsets (§2.1.4,
   risk R1).
4. **Typeless depth aliasing + `GenerateMips` + `CopyResource` + `ResolveSubresource` are load-bearing,
   not optional.** Depth is created as `DXGI_FORMAT_R24G8_TYPELESS`, viewed as `D24_UNORM_S8_UINT` for
   the DSV and `R24_UNORM_X8_TYPELESS` for the SRV (`Project.cpp:94-96,221-228`), and the depth SRV is
   hard-wired to texture slot **t7** (`Timeline.cpp:85`). Render targets are created with
   `MipLevels=0` + `D3D11_RESOURCE_MISC_GENERATE_MIPS` and `GenerateMips()` is called after essentially
   every pass (`Timeline.cpp:130`, `Texgen.cpp:174`, `RenderLayer.cpp:156`). Vulkan has no equivalent,
   so a mip-generation path must be synthesised (§2.11.2).
5. **A structured buffer shares the SRV slot namespace with sampled textures.**
   `Mesh.cpp:1737-1760` creates `D3D11_RESOURCE_MISC_BUFFER_STRUCTURED` + `D3D11_SRV_DIMENSION_BUFFER`
   (tree branch data). `D3D11` allows `t0` to be *either* a `Texture2D` or a `StructuredBuffer`; Vulkan
   requires the descriptor type to be known when the set layout is created. This forces a design decision
   on descriptor layout (§2.8.4, risk R4).

---

## 1. TASK 1 — exact API surface inventory

### 1.1 Interfaces and the exact methods called

Counts are *static occurrences* of `receiver->method(` where the receiver resolves to a D3D11/DXGI type.
Resolution tiers, in order: (1) a declaration in the same `.cpp`; (2) a declaration in the header with the
same basename (`DX11Texture.cpp` → `DX11Texture.h`); (3) an explicit allow-list of the cross-file globals
declared in `apEx/Phoenix/phxEngine.h:16-43` / `phxEngine.cpp:84,99,105` plus `SwapChain`
(`phxDev`, `phxContext`, `phxBackBufferView`, `phxDepthBufferView`, `phxDepthBufferShaderView`,
`phxMeshDataShaderView`, `phxTexture3D*`, `phxVoxelForceMSAA*`, `ltc1`, `ltc2`, `SwapChain`) — 14 names.
Everything else is *not* counted, which deliberately excludes same-named CoRE receivers
(e.g. `CCoreDevice *Device` in `WindowHandler.h:53`, `CCoreTexture2D *DepthBuffer`, `CphxTexturePoolTexture *Texture`)
so no method name appears on a D3D11 interface unless it is a genuine D3D11 method.
**Total: 402 interface calls across 27 interface groups.** Full per-call-site listings: Appendix A.


#### `ID3D11Device` — 25 methods, 98 calls

| method | n | call sites (first per file; `(+k)` = k further calls in that file) |
|---|---|---|
| `CreateShaderResourceView` | 18 | Bedrock/CoRE2/DDSTextureLoader.cpp:711(+2), Bedrock/CoRE2/DX11Texture.cpp:81(+3), apEx/Phoenix/Mesh.cpp:1759, apEx/Phoenix/Project.cpp:208(+1), apEx/Phoenix/Scene.cpp:765, apEx/Phoenix/Texgen.cpp:44(+2), apEx/Phoenix/phxEngine.cpp:251(+2), apEx/Phoenix_Tool/RenderTargets.cpp:35 |
| `CreateTexture2D` | 17 | Bedrock/CoRE2/DDSTextureLoader.cpp:748, Bedrock/CoRE2/DX11Device.cpp:141, Bedrock/CoRE2/DX11Texture.cpp:73(+5), apEx/Phoenix/Project.cpp:207(+1), apEx/Phoenix/Scene.cpp:764, apEx/Phoenix/Texgen.cpp:35(+1), apEx/Phoenix/phxEngine.cpp:279(+2), apEx/Phoenix_Tool/RenderTargets.cpp:28 |
| `CreateBuffer` | 13 | Bedrock/CoRE2/DX11ConstantBuffer.cpp:39, Bedrock/CoRE2/DX11IndexBuffer.cpp:48, Bedrock/CoRE2/DX11VertexBuffer.cpp:53(+1), apEx/Phoenix/Mesh.cpp:478(+3), apEx/Phoenix/Project.cpp:1178, apEx/Phoenix/phxEngine.cpp:177(+3) |
| `CreateRenderTargetView` | 7 | Bedrock/CoRE2/DX11Device.cpp:92, Bedrock/CoRE2/DX11Texture.cpp:95, apEx/MinimalPlayer/WindowHandler.cpp:98, apEx/Phoenix/Project.cpp:209, apEx/Phoenix/Texgen.cpp:53, apEx/Phoenix/phxEngine.cpp:280, apEx/Phoenix_Tool/RenderTargets.cpp:47 |
| `CreateInputLayout` | 4 | Bedrock/CoRE2/DX11VertexFormat.cpp:131, apEx/Phoenix/phxEngine.cpp:190(+2) |
| `CreatePixelShader` | 4 | Bedrock/CoRE2/DX11Shader.cpp:368, apEx/Phoenix/Project.cpp:372(+1), apEx/Phoenix/phxEngine.cpp:187 |
| `CreateSamplerState` | 4 | Bedrock/CoRE2/DX11RenderState.cpp:206, apEx/Phoenix/phxEngine.cpp:204(+2) |
| `CreateBlendState` | 3 | Bedrock/CoRE2/DX11RenderState.cpp:42, apEx/Phoenix/Project.cpp:1867, apEx/Phoenix_Tool/Material_Tool.cpp:1283 |
| `CreateDepthStencilState` | 3 | Bedrock/CoRE2/DX11RenderState.cpp:93, apEx/Phoenix/Project.cpp:1869, apEx/Phoenix_Tool/Material_Tool.cpp:1285 |
| `CreateDepthStencilView` | 3 | Bedrock/CoRE2/DX11Device.cpp:157, Bedrock/CoRE2/DX11Texture.cpp:508, apEx/Phoenix/Project.cpp:227 |
| `CreateRasterizerState` | 3 | Bedrock/CoRE2/DX11RenderState.cpp:147, apEx/Phoenix/Project.cpp:1868, apEx/Phoenix_Tool/Material_Tool.cpp:1284 |
| `CreateVertexShader` | 3 | Bedrock/CoRE2/DX11Shader.cpp:219, apEx/Phoenix/Project.cpp:556, apEx/Phoenix/phxEngine.cpp:199 |
| `CreateGeometryShader` | 2 | Bedrock/CoRE2/DX11Shader.cpp:517, apEx/Phoenix/Project.cpp:580 |
| `CreateTexture3D` | 2 | Bedrock/CoRE2/DDSTextureLoader.cpp:821, apEx/Phoenix/phxEngine.cpp:242 |
| `Release` | 2 | Bedrock/CoRE2/DX11Device.cpp:57, Bedrock/CoRE2/DX11Texture.cpp:1161 |
| `CheckFormatSupport` | 1 | Bedrock/CoRE2/DX11Texture.cpp:947 |
| `CreateComputeShader` | 1 | Bedrock/CoRE2/DX11Shader.cpp:956 |
| `CreateDomainShader` | 1 | Bedrock/CoRE2/DX11Shader.cpp:663 |
| `CreateHullShader` | 1 | Bedrock/CoRE2/DX11Shader.cpp:809 |
| `CreateQuery` | 1 | Bedrock/CoRE2/DX11Device.cpp:229 |
| `CreateTexture1D` | 1 | Bedrock/CoRE2/DDSTextureLoader.cpp:689 |
| `CreateUnorderedAccessView` | 1 | apEx/Phoenix/phxEngine.cpp:274 |
| `GetDeviceRemovedReason` | 1 | apEx/Phoenix/Project.cpp:1185 |
| `GetFeatureLevel` | 1 | Bedrock/CoRE2/DDSTextureLoader.cpp:1022 |
| `QueryInterface` | 1 | Bedrock/CoRE2/DX11Device.cpp:49 |

#### `ID3D11DeviceContext` — 43 methods, 164 calls

| method | n | call sites (first per file; `(+k)` = k further calls in that file) |
|---|---|---|
| `Unmap` | 11 | Bedrock/CoRE2/DX11ConstantBuffer.cpp:62, Bedrock/CoRE2/DX11IndexBuffer.cpp:88, Bedrock/CoRE2/DX11Texture.cpp:1286(+1), Bedrock/CoRE2/DX11VertexBuffer.cpp:133, apEx/Phoenix/RenderLayer.cpp:68, apEx/Phoenix/Scene.cpp:182(+1), apEx/Phoenix/Texgen.cpp:158, apEx/Phoenix/Timeline.cpp:95(+1) |
| `Map` | 10 | Bedrock/CoRE2/DX11ConstantBuffer.cpp:55, Bedrock/CoRE2/DX11IndexBuffer.cpp:68, Bedrock/CoRE2/DX11Texture.cpp:1275, Bedrock/CoRE2/DX11VertexBuffer.cpp:119, apEx/Phoenix/RenderLayer.cpp:65, apEx/Phoenix/Scene.cpp:153(+1), apEx/Phoenix/Texgen.cpp:156, apEx/Phoenix/Timeline.cpp:92(+1) |
| `PSSetShaderResources` | 9 | Bedrock/CoRE2/DX11Device.cpp:606, Bedrock/CoRE2/DX11Texture.cpp:39, apEx/Phoenix/RenderLayer.cpp:85, apEx/Phoenix/Texgen.cpp:145(+1), apEx/Phoenix/Timeline.cpp:86(+3) |
| `IASetPrimitiveTopology` | 7 | Bedrock/CoRE2/DX11Device.cpp:776(+3), apEx/Phoenix/RenderLayer.cpp:48(+1), apEx/Phoenix/Texgen.cpp:113 |
| `OMSetRenderTargets` | 7 | Bedrock/CoRE2/DX11Device.cpp:212(+2), apEx/Phoenix/RenderLayer.cpp:129, apEx/Phoenix/Texgen.cpp:148, apEx/Phoenix/Timeline.cpp:75(+1) |
| `PSSetSamplers` | 7 | Bedrock/CoRE2/DX11RenderState.cpp:222, apEx/Phoenix/Texgen.cpp:94(+2), apEx/Phoenix/phxEngine.cpp:213(+2) |
| `Draw` | 6 | Bedrock/CoRE2/DX11Device.cpp:786(+1), apEx/Phoenix/RenderLayer.cpp:99, apEx/Phoenix/Texgen.cpp:172, apEx/Phoenix/Timeline.cpp:123(+1) |
| `PSSetShader` | 6 | Bedrock/CoRE2/DX11Device.cpp:693, Bedrock/CoRE2/DX11Shader.cpp:390, apEx/Phoenix/RenderLayer.cpp:59, apEx/Phoenix/Texgen.cpp:125, apEx/Phoenix/Timeline.cpp:80(+1) |
| `DSSetShader` | 5 | Bedrock/CoRE2/DX11Device.cpp:673(+1), Bedrock/CoRE2/DX11Shader.cpp:685, apEx/Phoenix/RenderLayer.cpp:58, apEx/Phoenix/Texgen.cpp:117 |
| `IASetInputLayout` | 5 | Bedrock/CoRE2/DX11Device.cpp:621, Bedrock/CoRE2/DX11VertexFormat.cpp:28, apEx/Phoenix/RenderLayer.cpp:38(+1), apEx/Phoenix/Texgen.cpp:109 |
| `OMSetBlendState` | 5 | Bedrock/CoRE2/DX11Device.cpp:563, Bedrock/CoRE2/DX11RenderState.cpp:58, apEx/Phoenix/RenderLayer.cpp:61, apEx/Phoenix/Texgen.cpp:106, apEx/Phoenix/Timeline.cpp:83 |
| `OMSetDepthStencilState` | 5 | Bedrock/CoRE2/DX11Device.cpp:585, Bedrock/CoRE2/DX11RenderState.cpp:109, apEx/Phoenix/RenderLayer.cpp:62, apEx/Phoenix/Texgen.cpp:107, apEx/Phoenix/Timeline.cpp:84 |
| `PSSetConstantBuffers` | 5 | Bedrock/CoRE2/DX11Device.cpp:838, apEx/Phoenix/Scene.cpp:189, apEx/Phoenix/Texgen.cpp:127, apEx/Phoenix/Timeline.cpp:49(+1) |
| `RSSetState` | 5 | Bedrock/CoRE2/DX11Device.cpp:574, Bedrock/CoRE2/DX11RenderState.cpp:163, apEx/Phoenix/RenderLayer.cpp:60, apEx/Phoenix/Texgen.cpp:105, apEx/Phoenix/Timeline.cpp:82 |
| `RSSetViewports` | 5 | Bedrock/CoRE2/DX11Device.cpp:817, apEx/Phoenix/RenderLayer.cpp:125(+1), apEx/Phoenix/RenderTarget.cpp:13, apEx/Phoenix/Timeline.cpp:370 |
| `ClearRenderTargetView` | 4 | Bedrock/CoRE2/DX11Device.cpp:734, apEx/Phoenix/RenderLayer.cpp:118, apEx/Phoenix/Timeline.cpp:333(+1) |
| `CopyResource` | 4 | Bedrock/CoRE2/DX11Texture.cpp:264(+2), apEx/Phoenix/Timeline.cpp:71 |
| `GSSetShader` | 4 | Bedrock/CoRE2/DX11Device.cpp:653, Bedrock/CoRE2/DX11Shader.cpp:539, apEx/Phoenix/RenderLayer.cpp:56, apEx/Phoenix/Texgen.cpp:115 |
| `GenerateMips` | 4 | apEx/Phoenix/RenderLayer.cpp:156, apEx/Phoenix/Scene.cpp:198, apEx/Phoenix/Texgen.cpp:174, apEx/Phoenix/Timeline.cpp:130 |
| `HSSetShader` | 4 | Bedrock/CoRE2/DX11Device.cpp:663, Bedrock/CoRE2/DX11Shader.cpp:831, apEx/Phoenix/RenderLayer.cpp:57, apEx/Phoenix/Texgen.cpp:116 |
| `IASetVertexBuffers` | 4 | Bedrock/CoRE2/DX11Device.cpp:705, Bedrock/CoRE2/DX11VertexBuffer.cpp:31, apEx/Phoenix/RenderLayer.cpp:47, apEx/Phoenix/Texgen.cpp:112 |
| `VSSetSamplers` | 4 | Bedrock/CoRE2/DX11RenderState.cpp:225, apEx/Phoenix/phxEngine.cpp:212(+2) |
| `VSSetShader` | 4 | Bedrock/CoRE2/DX11Device.cpp:643, Bedrock/CoRE2/DX11Shader.cpp:241, apEx/Phoenix/RenderLayer.cpp:55, apEx/Phoenix/Texgen.cpp:101 |
| `ClearDepthStencilView` | 3 | Bedrock/CoRE2/DX11Device.cpp:737, apEx/Phoenix/RenderLayer.cpp:132, apEx/Phoenix/Timeline.cpp:337 |
| `DrawIndexed` | 3 | Bedrock/CoRE2/DX11Device.cpp:778(+1), apEx/Phoenix/RenderLayer.cpp:95 |
| `GSSetShaderResources` | 3 | Bedrock/CoRE2/DX11Device.cpp:610, Bedrock/CoRE2/DX11Texture.cpp:43, apEx/Phoenix/RenderLayer.cpp:84 |
| `IASetIndexBuffer` | 3 | Bedrock/CoRE2/DX11Device.cpp:633, Bedrock/CoRE2/DX11IndexBuffer.cpp:29, apEx/Phoenix/RenderLayer.cpp:88 |
| `VSSetShaderResources` | 3 | Bedrock/CoRE2/DX11Device.cpp:608, Bedrock/CoRE2/DX11Texture.cpp:41, apEx/Phoenix/RenderLayer.cpp:83 |
| `GSSetConstantBuffers` | 2 | Bedrock/CoRE2/DX11Device.cpp:837, apEx/Phoenix/Scene.cpp:188 |
| `OMSetRenderTargetsAndUnorderedAccessViews` | 2 | apEx/Phoenix/RenderLayer.cpp:127(+1) |
| `UpdateSubresource` | 2 | Bedrock/CoRE2/DX11Texture.cpp:221, Bedrock/CoRE2/DX11VertexBuffer.cpp:102 |
| `VSSetConstantBuffers` | 2 | Bedrock/CoRE2/DX11Device.cpp:836, apEx/Phoenix/Scene.cpp:187 |
| `Begin` | 1 | Bedrock/CoRE2/DX11Device.cpp:978 |
| `CSSetShader` | 1 | Bedrock/CoRE2/DX11Shader.cpp:978 |
| `ClearState` | 1 | Bedrock/CoRE2/DX11Device.cpp:41 |
| `ClearUnorderedAccessViewFloat` | 1 | apEx/Phoenix/RenderLayer.cpp:137 |
| `End` | 1 | Bedrock/CoRE2/DX11Device.cpp:985 |
| `Flush` | 1 | Bedrock/CoRE2/DX11Device.cpp:42 |
| `GSSetSamplers` | 1 | Bedrock/CoRE2/DX11RenderState.cpp:228 |
| `GetData` | 1 | Bedrock/CoRE2/DX11Device.cpp:988 |
| `GetDevice` | 1 | Bedrock/CoRE2/DX11Texture.cpp:1158 |
| `Release` | 1 | Bedrock/CoRE2/DX11Device.cpp:43 |
| `ResolveSubresource` | 1 | Bedrock/CoRE2/DX11Texture.cpp:967 |

#### Resource / view / state interfaces

| interface | methods (count) | call sites |
|---|---|---|
| `ID3D10Blob` | `GetBufferPointer`(27), `GetBufferSize`(12), `Release`(7) | Bedrock/CoRE2/DX11Shader.cpp:278; apEx/Phoenix/Mesh.cpp:1158; apEx/Phoenix/Project.cpp:368; apEx/Phoenix/phxEngine.cpp:187 |
| `ID3D11BlendState/ID3D11DepthStencilState/ID3D11RasterizerState/ID3D11SamplerState` | `Release`(8) | Bedrock/CoRE2/DX11RenderState.cpp:16 |
| `ID3D11Buffer` | `Release`(5) | Bedrock/CoRE2/DX11ConstantBuffer.cpp:15; Bedrock/CoRE2/DX11IndexBuffer.cpp:22; Bedrock/CoRE2/DX11VertexBuffer.cpp:23; apEx/Phoenix_Tool/TexgenPage.cpp:352 |
| `ID3D11ComputeShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:971 |
| `ID3D11Debug` | `ReportLiveDeviceObjects`(1), `Release`(1) | Bedrock/CoRE2/DX11Device.cpp:53 |
| `ID3D11DepthStencilView` | `Release`(4) | Bedrock/CoRE2/DX11Device.cpp:31; Bedrock/CoRE2/DX11Texture.cpp:29 |
| `ID3D11DomainShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:678 |
| `ID3D11GeometryShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:532 |
| `ID3D11HullShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:824 |
| `ID3D11InputLayout` | `Release`(1) | Bedrock/CoRE2/DX11VertexFormat.cpp:21 |
| `ID3D11PixelShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:383 |
| `ID3D11Query` | `Release`(1) | Bedrock/CoRE2/DX11Device.cpp:28 |
| `ID3D11RenderTargetView` | `Release`(4) | Bedrock/CoRE2/DX11Device.cpp:30; Bedrock/CoRE2/DX11Texture.cpp:30 |
| `ID3D11Resource` | `GetType`(1), `QueryInterface`(1) | Bedrock/CoRE2/DX11Texture.cpp:913 |
| `ID3D11SamplerState` | `Release`(3) | apEx/Phoenix_Tool/TexgenPage.cpp:354 |
| `ID3D11ShaderResourceView` | `Release`(2) | Bedrock/CoRE2/DX11Texture.cpp:28; apEx/Phoenix/Scene.cpp:732 |
| `ID3D11Texture1D/ID3D11Texture2D/ID3D11Texture3D` | `Release`(6) | Bedrock/CoRE2/DDSTextureLoader.cpp:715 |
| `ID3D11Texture2D` | `Release`(27), `GetDesc`(3) | Bedrock/CoRE2/DX11Device.cpp:32; Bedrock/CoRE2/DX11Texture.cpp:26; apEx/Phoenix/Scene.cpp:733; apEx/Phoenix/Texgen.cpp:181 |
| `ID3D11VertexShader` | `Release`(1) | Bedrock/CoRE2/DX11Shader.cpp:234 |
| `IDXGIAdapter` | `EnumOutputs`(1) | apEx/MinimalPlayer/MinimalPlayer.cpp:208 |
| `IDXGIFactory` | `EnumAdapters`(1) | apEx/MinimalPlayer/MinimalPlayer.cpp:206 |
| `IDXGIOutput` | `GetDisplayModeList`(2), `Release`(1) | Bedrock/CoRE2/DX11Device.cpp:263; apEx/MinimalPlayer/MinimalPlayer.cpp:210 |
| `IDXGISwapChain` | `GetBuffer`(4), `Present`(4), `SetFullscreenState`(3), `Release`(1), `GetFullscreenState`(1), `GetDesc`(1), `ResizeBuffers`(1) | Bedrock/CoRE2/DX11Device.cpp:35; apEx/MinimalPlayer/MinimalPlayer.cpp:290; apEx/MinimalPlayer/WindowHandler.cpp:97; apEx/Phoenix/Project.cpp:182 |

**Notable call sites for methods that only appear once or twice**, with the file that owns them:

| method | call site | note |
|---|---|---|
| `ID3D11Device::CreateUnorderedAccessView` | `phxEngine.cpp:274` | volumetric path only (`PHX_VOLUMETRIC_RENDERTARGETS`, default off) |
| `ID3D11Device::CheckFormatSupport` | `DX11Texture.cpp:947` | `D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE` |
| `ID3D11Device::QueryInterface` | `DX11Device.cpp:49` | → `ID3D11Debug` |
| `ID3D11DeviceContext::ClearUnorderedAccessViewFloat` | `RenderLayer.cpp:137` | volumetric path only |
| `ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews` | `RenderLayer.cpp:127`, `:144` | one live (`:144`, volumetric) + one in a comment (`:127`) |
| `ID3D11DeviceContext::ResolveSubresource` | `DX11Texture.cpp:967` | inside `SaveDDSTexture` |
| `ID3D11DeviceContext::Begin`/`End`/`GetData` | `DX11Device.cpp:978`, `:985`, `:988` | occlusion query |
| `ID3D11DeviceContext::GetDevice` | `DX11Texture.cpp:1158` | readback path |
| `ID3D11Resource::GetType` / `QueryInterface` | `DX11Texture.cpp:913` / `:919` | readback path |
| `ID3D11Texture2D::GetDesc` | `DX11Texture.cpp:173`, `:823`, `:925` | size/format recovery + staging desc |
| `IDXGIOutput::GetDisplayModeList` | `MinimalPlayer.cpp:210`, `:213` | monitor mode enumeration |

**Not called anywhere in scope** (verified negatives; the shim may stub or omit them):
`Dispatch`, `DrawInstanced`, `DrawIndexedInstanced`, `DrawAuto`, `CopySubresourceRegion`,
`RSSetScissorRects`, `SOSetTargets`, `SetPredication`, `Present1`, `CreateDeferredContext`,
`CreateBlendState1`, `OMSetRenderTargetsAndUnorderedAccessViews` with a non-zero UAV initial count,
`ID3D11DeviceContext::UpdateSubresource` on a texture with a box, `ID3D11DeviceContext::GenerateMips`
on a non-SRV.

### 1.2 `D3D11_*` symbols actually referenced (136 distinct)

`D3D11_BIND_{CONSTANT_BUFFER,DEPTH_STENCIL,INDEX_BUFFER,RENDER_TARGET,SHADER_RESOURCE,UNORDERED_ACCESS,VERTEX_BUFFER}` ·
`D3D11_BLEND` (+ `_ZERO,_ONE,_SRC_COLOR,_INV_SRC_COLOR,_SRC_ALPHA,_INV_SRC_ALPHA,_DEST_ALPHA,_INV_DEST_ALPHA,_DEST_COLOR,_INV_DEST_COLOR,_SRC_ALPHA_SAT,_BLEND_FACTOR,_INV_BLEND_FACTOR`) ·
`D3D11_BLEND_OP` (+`_ADD,_SUBTRACT,_REV_SUBTRACT,_MIN,_MAX`) · `D3D11_BLEND_DESC` · `D3D11_RENDER_TARGET_BLEND_DESC` ·
`D3D11_BOX` · `D3D11_BUFFER_DESC` · `D3D11_CLEAR_DEPTH` · `D3D11_CLEAR_STENCIL` · `D3D11_COLOR_WRITE_ENABLE_ALL` ·
`D3D11_COMPARISON_FUNC` (+ `_NEVER,_LESS,_EQUAL,_LESS_EQUAL,_GREATER,_NOT_EQUAL,_GREATER_EQUAL,_ALWAYS`) ·
`D3D11_CPU_ACCESS_{READ,WRITE}` · `D3D11_CREATE_DEVICE_DEBUG` · `D3D11_CULL_MODE` (+`_NONE,_FRONT,_BACK`) ·
`D3D11_DEPTH_STENCIL_DESC` · `D3D11_DEPTH_STENCIL_VIEW_DESC` · `D3D11_DEPTH_WRITE_MASK_{ALL,ZERO}` ·
`D3D11_DSV_DIMENSION_{TEXTURE2D,TEXTURE2DMS}` · `D3D11_FILL_MODE` (+`_SOLID,_WIREFRAME`) · `D3D11_FILTER` (all 18 values) ·
`D3D11_FLOAT32_MAX` · `D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE` · `D3D11_INPUT_ELEMENT_DESC` · `D3D11_INPUT_PER_VERTEX_DATA` ·
`D3D11_MAPPED_SUBRESOURCE` · `D3D11_MAP_{READ,WRITE_DISCARD}` · `D3D11_PRIMITIVE_TOPOLOGY_{POINTLIST,LINELIST,TRIANGLELIST}` ·
`D3D11_QUERY_DESC` · `D3D11_QUERY_OCCLUSION` · `D3D11_RASTERIZER_DESC` · `D3D11_RENDER_TARGET_VIEW_DESC` ·
`D3D11_REQ_{MIP_LEVELS,TEXTURE1D_U_DIMENSION,TEXTURE1D_ARRAY_AXIS_DIMENSION,TEXTURE2D_U_OR_V_DIMENSION,TEXTURE2D_ARRAY_AXIS_DIMENSION,TEXTURE3D_U_V_OR_W_DIMENSION,TEXTURECUBE_DIMENSION}` ·
`D3D11_RESOURCE_DIMENSION` (+`_UNKNOWN,_TEXTURE1D,_TEXTURE2D,_TEXTURE3D`) · `D3D11_RESOURCE_MISC_{BUFFER_STRUCTURED,GENERATE_MIPS,TEXTURECUBE}` ·
`D3D11_RLDO_DETAIL` · `D3D11_RTV_DIMENSION_{TEXTURE2D,TEXTURE2DMS}` · `D3D11_SAMPLER_DESC` · `D3D11_SDK_VERSION` ·
`D3D11_SHADER_RESOURCE_VIEW_DESC` · `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT` · `D3D11_SRV_DIMENSION_{BUFFER,TEXTURE2D,TEXTURE2DMS,TEXTURE3D}` ·
`D3D11_STANDARD_MULTISAMPLE_PATTERN` · `D3D11_SUBRESOURCE_DATA` · `D3D11_TEXTURE1D_DESC` · `D3D11_TEXTURE2D_DESC` · `D3D11_TEXTURE3D_DESC` ·
`D3D11_TEXTURE_ADDRESS_MODE` (+`_WRAP,_MIRROR,_CLAMP,_BORDER,_MIRROR_ONCE`) · `D3D11_UAV_DIMENSION_TEXTURE3D` ·
`D3D11_UNORDERED_ACCESS_VIEW_DESC` · `D3D11_USAGE_{DEFAULT,DYNAMIC,IMMUTABLE,STAGING}` · `D3D11_VIEWPORT`

### 1.3 `D3D10_*`, `D3D_*` and `DXGI_*` symbols

**`D3D10_*` (the header must also declare these — D3D10 constants are used in D3D11 code):**

| symbol | call site | why |
|---|---|---|
| `D3D10_CLEAR_DEPTH`, `D3D10_CLEAR_STENCIL` | `RenderLayer.cpp:132`, `Timeline.cpp:337` | used **instead of** `D3D11_CLEAR_*`; `RenderLayer.cpp:132` even does `( D3D10_CLEAR_DEPTH * !OmitDepthBuffer ) \| D3D10_CLEAR_STENCIL` — a bool multiplied into a flag, so the value must be an integer expression (D3D10_CLEAR_DEPTH = 1, D3D10_CLEAR_STENCIL = 2) |
| `D3D10_STANDARD_MULTISAMPLE_PATTERN` | `DX11Texture.cpp:489` | `tex.SampleDesc.Quality = MSCount > 1 ? D3D10_STANDARD_MULTISAMPLE_PATTERN : 0` |
| `D3D10_RESOURCE_DIMENSION` | `DDSTextureLoader.cpp` (DDS DX10 header) | |
| `D3D10_RESOURCE_MISC_FLAG` | `DDSTextureLoader.cpp` | |
| `D3D10_REQ_TEXTURE{2D,3D}_U_OR_V_DIMENSION` | `DDSTextureLoader.cpp` | |

**`D3D_*`:** `D3D_DRIVER_TYPE_HARDWARE`, `D3D_SDK_VERSION`, `D3D_SHADER_MACRO`, `D3D_OK`,
`D3D_FEATURE_LEVEL_9_1/9_2/9_3`, `D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION`,
`D3D_FL9_1_REQ_TEXTURE3D_U_V_OR_W_DIMENSION`, `D3D_FL9_1_REQ_TEXTURECUBE_DIMENSION`,
`D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION`, `D3D_SRV_DIMENSION_*` (DDSTextureLoader only).

**`DXGI_*`:** 130 distinct symbols. Most are `DXGI_FORMAT_*` appearing only inside the
`DDSTextureLoader.cpp` format tables (`DX11Texture.cpp:780-900`, `DDSTextureLoader.cpp`); the ones that
matter for the *player's* resource creation are:

| `DXGI_FORMAT` | where created | usage |
|---|---|---|
| `R16G16B16A16_FLOAT` | `Project.cpp:305` (internal RT; pixel-format table `Project.cpp:160-164`), `phxEngine.cpp:231,246,299` | main render target, mipmapped, `GENERATE_MIPS` |
| `R32_FLOAT` | `Project.cpp:205` | alternate RT pixel format |
| `R8G8B8A8_UNORM` | `phxEngine.cpp:276,303` | ltc2 LUT, voxel MSAA texture |
| `R8G8B8A8_UNORM_SRGB` / `R8G8B8A8_UNORM` | `MinimalPlayer/WindowHandler.cpp:44/46` | **swapchain back-buffer** |
| `R24G8_TYPELESS` | `Project.cpp:96,221` | depth texture (typeless) |
| `D24_UNORM_S8_UINT` | `Project.cpp:94,227`, `DX11Device.cpp:133,153`, `DX11Texture.cpp:503` | depth **view** format |
| `R24_UNORM_X8_TYPELESS` | `Project.cpp:95,228`, `DX11Texture.cpp:518` | depth **SRV** format |
| `B8G8R8A8_UNORM` | `DX11Enums.cpp:11` (`COREFMT_A8R8G8B8`), `Texgen.cpp:214`, `DX11VertexFormat.cpp:100` (COLOR4 attribute) | CoRE textures / colour attribute |
| `R32G32B32A32_FLOAT`, `R32_FLOAT`, `R16G16_FLOAT`, `R16G16B16A16_FLOAT` | `DX11Enums.cpp:12-15` | `COREFORMAT` table |
| `R32G32B32_FLOAT`, `R32G32_FLOAT` | `DX11VertexFormat.cpp:60,68,84,92`, `phxEngine.cpp:126-143` | vertex formats |
| `R8G8B8A8_UNORM` | `phxEngine.cpp:141` | particle `Color` attribute |
| `R16_UINT`, `R32_UINT` | `DX11IndexBuffer.cpp:29`, `DX11Device.cpp:633`, `RenderLayer.cpp:88` | index formats |
| `UNKNOWN` | `DX11Device.cpp:288`, `Mesh.cpp:1752` (structured-buffer SRV), `DDSTextureLoader.cpp` | |
| `DXGI_USAGE_RENDER_TARGET_OUTPUT`, `DXGI_SWAP_EFFECT_DISCARD`, `DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED`, `DXGI_MODE_SCALING_UNSPECIFIED`, `DXGI_ENUM_MODES_INTERLACED`, `DXGI_ERROR_DEVICE_REMOVED` | swapchain / monitor enumeration | |
| `DXGI_SAMPLE_DESC`, `DXGI_MODE_DESC`, `DXGI_RATIONAL`, `DXGI_SWAP_CHAIN_DESC` | swapchain | |

> **Note for the shim:** `DXGI_FORMAT` is used as an `unsigned int` in `Project.cpp:160-164`
> (`static unsigned char PixelFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT };`
> → **`DXGI_FORMAT` constants must fit in a `unsigned char`** — `R16G16B16A16_FLOAT = 10` and
> `R32_FLOAT = 41` do, but the array is declared `unsigned char` and the values are re-cast at
> `Project.cpp:316` (`(DXGI_FORMAT)PixelFormats[ pFormat & 0x0f ]`). Do not renumber DXGI formats.)

### 1.4 Structs — the ABI-critical part

Several descriptors are **aggregate-initialised positionally**, so our header must reproduce D3D11's
exact member order, member types (including nested structs/unions) and sizes.

| struct | positional initialiser | required layout |
|---|---|---|
| `D3D11_BUFFER_DESC` | `phxEngine.cpp:161`, `162`; `DX11VertexBuffer.cpp:41-46`; `DX11IndexBuffer.cpp:40-45`; `Mesh.cpp:1737-1744` | `{ UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags; UINT CPUAccessFlags; UINT MiscFlags; UINT StructureByteStride; }` — 6 scalars, `ByteWidth` first |
| `D3D11_SUBRESOURCE_DATA` | `phxEngine.cpp:163`, `Mesh.cpp:1747` | `{ const void* pSysMem; UINT SysMemPitch; UINT SysMemSlicePitch; }` |
| `D3D11_SAMPLER_DESC` | `phxEngine.cpp:164` — **13 values** | `{ D3D11_FILTER Filter; D3D11_TEXTURE_ADDRESS_MODE AddressU,V,W; FLOAT MipLODBias; UINT MaxAnisotropy; D3D11_COMPARISON_FUNC ComparisonFunc; FLOAT BorderColor[4]; FLOAT MinLOD; FLOAT MaxLOD; }` — the two `FLOAT32_MAX` values are `MinLOD`/`MaxLOD`; **`ComparisonFunc` must come before `BorderColor`, not after**, or the values land in the wrong fields |
| `D3D11_TEXTURE2D_DESC` | `phxEngine.cpp:276,299,303`; `Project.cpp:96` | `{ UINT Width,Height,MipLevels,ArraySize; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc; D3D11_USAGE Usage; UINT BindFlags,CPUAccessFlags,MiscFlags; }` — i.e. `SampleDesc.Count`/`Quality` sit between `Format` and `Usage` |
| `D3D11_TEXTURE3D_DESC` | `phxEngine.cpp:225-237` (fully `memset` + field writes; also `D3D11_TEXTURE3D_DESC` from the Nvidia volumetric sample) | `{ Width,Height,Depth,MipLevels,Format,Usage,BindFlags,CPUAccessFlags,MiscFlags }` |
| `D3D11_RENDER_TARGET_VIEW_DESC` | `phxEngine.cpp:277`; `Project.cpp:205`; `RenderTargets.cpp:43` | `{ DXGI_FORMAT Format; D3D11_RTV_DIMENSION ViewDimension; union{…Texture2D.MipSlice…}; }` — the 3rd initialiser lands in `Texture2D.MipSlice` |
| `D3D11_SHADER_RESOURCE_VIEW_DESC` | `phxEngine.cpp:300,304`; `Project.cpp:95`; `Mesh.cpp:1752-1757` | `{ DXGI_FORMAT Format; D3D11_SRV_DIMENSION ViewDimension; union{}; }` where the 3rd/4th values are `Texture2D.MostDetailedMip`, `Texture2D.MipLevels` (for `TEXTURE2D`); `Buffer.ElementWidth`, `Buffer.NumElements` for `BUFFER` |
| `D3D11_DEPTH_STENCIL_VIEW_DESC` | `Project.cpp:94` — 4 values | `{ DXGI_FORMAT Format; D3D11_DSV_DIMENSION ViewDimension; UINT Flags; union{ Texture2D.MipSlice; }; }` — **`Flags` is the 3rd member, before the union** |
| `D3D11_RENDER_TARGET_BLEND_DESC` | `Project.cpp:97` — 8 values | `{ BOOL BlendEnable; D3D11_BLEND SrcBlend,DestBlend; D3D11_BLEND_OP BlendOp; D3D11_BLEND SrcBlendAlpha,DestBlendAlpha; D3D11_BLEND_OP BlendOpAlpha; UINT8 RenderTargetWriteMask; }` |
| `D3D11_INPUT_ELEMENT_DESC` | `phxEngine.cpp:124-144` — 7 values × 6 | `{ LPCSTR SemanticName; UINT SemanticIndex; DXGI_FORMAT Format; UINT InputSlot; UINT AlignedByteOffset; D3D11_INPUT_CLASSIFICATION InputSlotClass; UINT InstanceDataStepRate; }` |
| `D3D11_VIEWPORT` | `RenderTarget.cpp:6-12`; `Timeline.cpp:370` (`{ x, y, w, h, 0, 1 }`); `RenderLayer.cpp:111-115` | `{ FLOAT TopLeftX,TopLeftY,Width,Height,MinDepth,MaxDepth; }` |
| `D3D11_MAPPED_SUBRESOURCE` | `RenderLayer.cpp:66`; `Scene.cpp:154`; `Timeline.cpp:93`; `Texgen.cpp:156` | `{ void* pData; UINT RowPitch; UINT DepthPitch; }` — **`pData` first**; `RowPitch`/`DepthPitch` are never read |
| `D3D11_BOX` | `DX11VertexBuffer.cpp:97-100` | `{ UINT left,top,front,right,bottom,back; }` only `left`/`right` are set |
| `D3D11_QUERY_DESC` | `DX11Device.cpp:224-227` | `{ D3D11_QUERY Query; UINT MiscFlags; }` |
| `D3D11_BLEND_DESC` / `D3D11_DEPTH_STENCIL_DESC` / `D3D11_RASTERIZER_DESC` | field-wise in `Project.cpp:1841-1869`, `Material_Tool.cpp:1226-1265`, `DX11RenderState.cpp:25-38,83-89,134-143` | must match D3D11 field names exactly (`RenderTarget[8]`, `DepthWriteMask`, `SlopeScaledDepthBias`, …). **No positional init**, so order is free; names are not. |
| `DXGI_SWAP_CHAIN_DESC` | `MinimalPlayer/WindowHandler.cpp:44,46` — **15 values** | see §1.4.9 ⚠ |
| `DXGI_RATIONAL` | nested in the above | `{ UINT Numerator; UINT Denominator; }` |

#### 1.4.9 ⚠ `DXGI_SWAP_CHAIN_DESC` aggregate initialiser has one value too many

`apEx/MinimalPlayer/WindowHandler.cpp:44` and `:46` initialise a `DXGI_SWAP_CHAIN_DESC` with **15**
scalar values. The struct as declared in the repo's own bundled header
(`apEx/ThirdParty/D3D/Include/DXGI.h:252-262`) has 8 members = **14** scalars
(`DXGI_MODE_DESC` = 6 per `DXGIType.h:106-114`, `DXGI_SAMPLE_DESC` = 2, plus `BufferUsage`,
`BufferCount`, `OutputWindow`, `Windowed`, `SwapEffect`, `Flags`).

The 15 values map perfectly onto a **7-member `DXGI_MODE_DESC`** (i.e. one with an extra trailing
member such as the vestigial `DXGI_MODE_ROTATION`), and `DXGIType.h:99-107` *still defines*
`DXGI_MODE_ROTATION` — evidence that this code was written against a pre-release DXGI where
`DXGI_MODE_DESC` had a rotation field. `WindowHandler.cpp` is listed as `<ClCompile>` in
`MinimalPlayer.vcxproj:523` with no `ExcludedFromBuild`, so this is not obviously dead code.

**Action:** the portable shim must declare the *standard* 8-member `DXGI_SWAP_CHAIN_DESC`
(15 values would be a hard error). Therefore a **one-line source fix is required** in the player's
vendored copy of `WindowHandler.cpp` (drop the trailing `, 0` or one redundant `0`). Do not paper over
this by padding the struct — padding would silently shift `SwapEffect`/`Flags`.
**Status: `UNCERTAIN`** — I cannot compile MSVC here to confirm whether the file currently builds;
the arithmetic is unambiguous at 14 slots, so a fix is needed either way.

### 1.5 Free functions (not interface methods)

| function | call sites |
|---|---|
| `D3D11CreateDeviceAndSwapChain` | `Bedrock/CoRE2/DX11Device.cpp:189` (debug flags, `D3D11_CREATE_DEVICE_DEBUG`), `:196` (fallback, flags 0), `apEx/MinimalPlayer/WindowHandler.cpp:83` (commented), `:89` (live) |
| `CreateDXGIFactory` | `apEx/MinimalPlayer/MinimalPlayer.cpp:204` → `__uuidof(IDXGIFactory)` |
| `D3DCompile` (via function pointer, see §2.9) | not linked; resolved with `GetProcAddress` |

`__uuidof(ID3D11Texture2D)`, `__uuidof(ID3D11Debug)`, `__uuidof(ID3D11Resource)`,
`__uuidof(ID3D11Texture2D)` appear at `DX11Device.cpp:82,926,1002`, `DX11Device.cpp:49`,
`DX11Texture.cpp:817`, `WindowHandler.cpp:86/97`. A non-MSVC shim needs `__uuidof` (or a macro
substitute) plus `IID_ID3D11*` GUID constants.

### 1.6 Non-math D3DX that must be replaced

| API | call site | replacement |
|---|---|---|
| `D3DX11CreateTextureFromMemory` | `apEx/Phoenix/Texgen.cpp:226`, (`Bedrock/CoRE2/DX11Texture.cpp:120` under `CORE_API_D3DX`, **off**) | image-codec path: use `DecompressImage`/`DecompressPNG`/`CreateDDSTextureFromMemory`, all already present in-repo (`DX11Texture.cpp:130-165`) |
| `D3DX11SaveTextureToMemory(ctx, tex, D3DX11_IFF_DDS, &blob, NULL)` | `apEx/Phoenix/Mesh.cpp:1126` | DDS writer — `SaveDDSTexture` (`DX11Texture.cpp` / `DDSTextureLoader.cpp`) already exists in-repo and is used by `ExportToImage`; but the call returns an `ID3D10Blob*` that the code then reads (`Mesh.cpp:1158`) and releases at `:1196`, so a blob shim is needed |
| `D3DX11SaveTextureToFile(ctx, tex, D3DX11_IFF_DDS, path)` | `apEx/Phoenix_Tool/Texgen_tool.cpp:1037` | **tool only**; can be stubbed for the player |
| `D3DX11_IFF_DDS`, `D3DX11_IFF_PNG` | as above | constants only |
| `DirectX::PackedVector::XMConvertHalfToFloatStream` | `Bedrock/CoRE2/DX11Texture.cpp:406` | half→float stream; `#include <DirectXPackedVector.h>` + `<DirectXMath.h>` at `DX11Texture.cpp:273-274` |
| `ID3D11Debug::ReportLiveDeviceObjects(D3D11_RLDO_DETAIL)` | `DX11Device.cpp:53` | needs `QueryInterface(__uuidof(ID3D11Debug))` to *succeed*; simplest is to return `E_NOINTERFACE` so the `if (dbg)` guard skips (verified safe: `DX11Device.cpp:50-55`) |
| `D3DXCompileShader`, `D3DXCreateTextureFromFileInMemory` | `DX9Device.cpp`, `DX9Texture.cpp` — **DX9 only**, excluded by `Core2_Config.h:19-20` | none |

### 1.7 Deliberately-dead surface (do **not** implement; keep declarations)

These exist in the source but are unreachable in the shipped configuration. Implementing them is wasted
effort; the *declarations* must still exist or the code will not compile.

| feature | evidence |
|---|---|
| **Shader reflection** | every `ID3D11ShaderReflection`, `GetConstantBufferByIndex`, `D3D11_SHADER_BUFFER_DESC`, `D3DReflectFunc`, `IID_ID3D11ShaderReflection` occurrence is inside `//`-comments: `DX11Shader.cpp:293,336,442,485,588,631,734,777,880,923,1027,1070`, `DX11Shader.h:12,34,56,78,100,122`. Repo-wide grep for `GetConstantBufferByName`, `GetInputParameterDesc`, `GetResourceBindingDesc`, `GetVariableByName`, `D3D11_SIGNATURE_PARAMETER_DESC`, `D3DReflect` (non-comment) returns **zero** hits. |
| **Compute dispatch** | `Dispatch` appears only as `DispatchMessage` (`MinimalPlayer.cpp:283`, `Project.cpp:191`, `WindowHandler.cpp:255`). No `Dispatch` on any `ID3D11DeviceContext`. |
| **`PHX_VOLUMETRIC_RENDERTARGETS`** | defined **only** in `apEx/Phoenix/PhxManualConfig.h:32`; **commented out** in both `PhoenixConfig.h:118` and `PhoenixConfig_full.h:118`. Default builds have it off, so the 3D texture / UAV / `OMSetRenderTargetsAndUnorderedAccessViews` / `ClearUnorderedAccessViewFloat` paths in `phxEngine.cpp:221-296` and `RenderLayer.cpp:121-146` are inert. |
| **Hull / Domain shaders** | the material compiler only builds `{ "v","p","g" }` / `{ "vs_5_0","ps_5_0","gs_5_0" }` (`Project.cpp:98-99`); `CreateHullShader`/`CreateDomainShader` at `Project.cpp:592,604` are commented out. `RenderLayer.cpp:57-58` always passes `NULL`. Exported `.apx` projects contain no `hs_5_0`/`ds_5_0`/`numthreads`. |
| **Scissor rects** | `RSSetScissorRects` is never called anywhere. `ScissorEnable` is set explicitly only in tool code (`Material_Tool.cpp:1238`); in the player `BuildPassState` `memset`s the whole `D3D11_RASTERIZER_DESC` to 0 and then sets only `FillMode`, `CullMode`, `DepthClipEnable`, `AntialiasedLineEnable` (`Project.cpp:1844-1850`), so `ScissorEnable == 0`. `CollectRenderState` never writes it either (it writes only `CullMode`, `DepthEnable`, `DepthWriteMask`, `DepthFunc` and the blend source/dest factors — `Project.cpp:1890-1955`). |
| **Vertex-buffer `Lock` with non-zero offset** | `CCoreVertexBuffer::Lock(Result, Offset, size, Flags)` ignores `Offset`/`size` in the DX11 backend and always discards the whole buffer (`DX11VertexBuffer.cpp:108-123`), so the shim only needs whole-buffer `D3D11_MAP_WRITE_DISCARD`. |
| **`phxMeshDataShaderView`** | declared `phxEngine.cpp:74`, assigned at `Phoenix_Tool/Model_Tool.cpp:1529,1542`, but **never bound to any slot** in `apEx/Phoenix` (player). Creation still must succeed. |

---

## 2. TASK 2 — semantics that matter

### 2.1 Buffers

#### 2.1.1 Constant buffers — three different creation/update idioms

| idiom | where | details |
|---|---|---|
| `D3D11_USAGE_DYNAMIC` + `D3D11_BIND_CONSTANT_BUFFER` + `D3D11_CPU_ACCESS_WRITE`, updated per-frame with `Map(WRITE_DISCARD)` | `phxEngine.cpp:161` (static `desc`), `phxEngine.cpp:177,180,183`; `DX11ConstantBuffer.cpp:30-39` | `phxEngine.cpp:161`: `static D3D11_BUFFER_DESC desc = { 0, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };` then `desc.ByteWidth = SHADERDATALENGTH` (80), `SCENEDATASIZE` (1200), `16*4*2 + MATERIALDATASIZE` (192) |
| CoRE2 `CCoreDX11ConstantBuffer::Upload` | `DX11ConstantBuffer.cpp:23-65` | reallocates on growth; `ByteWidth = DataLength` **exactly** (not rounded to 16), then `Map(WRITE_DISCARD)` + `memcpy` + `Unmap` |
| `D3D11_USAGE_DEFAULT` + `D3D11_BIND_VERTEX_BUFFER` + `UpdateSubresource` | `DX11VertexBuffer.cpp:44-53, 92-105` | `Update` uses `D3D11_BOX{left=Offset, right=Offset+Size}` — note `top/right/bottom/back` left 0 by `memset`, so the box is degenerate in Y/Z; harmless for buffers |
| `D3D11_USAGE_IMMUTABLE` + `D3D11_BIND_SHADER_RESOURCE` + `D3D11_RESOURCE_MISC_BUFFER_STRUCTURED` | `Mesh.cpp:1737-1750` | structured buffer, `StructureByteStride = sizeof(Arbaro::BranchData)`; `ByteWidth = stride * count` |

**Buffer sizes actually requested** (`phxEngine.h:48-49`):

```
SCENEDATASIZE    = 16*4*2 + 4*4*2 + 8*(6*4*4) + 16 + 16*4 + 16*4 + 16*4*2 = 1200
MATERIALDATASIZE = 64*sizeof(float)                                        = 256
SHADERDATALENGTH = (4 + 16)*sizeof(float)   [TEXGEN_MAX_PARAMS=16]          = 80
ObjectMatrixBuffer ByteWidth = 16*4*2 + MATERIALDATASIZE                    = 384
```
`SCENEDATASIZE` is *exactly* the D3D-packed size of the scene `cbuffer b` used by the shaders
(`16 + 64 + 64 + 16 + 16 + 768 + 16 + 64 + 64 = 1088` written, 1200 allocated). See §2.1.4.

#### 2.1.2 `Map` / `Unmap` usage

* **Every** `Map` on a CB in `apEx/Phoenix` uses `D3D11_MAP_WRITE_DISCARD` with `Subresource = 0`:
  `RenderLayer.cpp:65`, `Timeline.cpp:91,98`, `Texgen.cpp:156`, `Scene.cpp:153`, plus `DX11ConstantBuffer.cpp:55`.
* The only `D3D11_MAP_READ` is the DDS screenshot readback: `DX11Texture.cpp:1173` on a staging texture
  created at `DX11Texture.cpp:974-975` (`D3D11_USAGE_STAGING`, `CPUAccessFlags = D3D11_CPU_ACCESS_READ`).
* `Unmap(ptr, 0)` everywhere. `Map`'s return value is **never** checked in `apEx/Phoenix`
  (`RenderLayer.cpp:65`, `Timeline.cpp:91,98`, `Texgen.cpp:156` ignore the HRESULT), so the shim must
  always hand back a valid writable pointer.
* **Hot path:** `RenderLayer.cpp:64-68` does one `Map`/`memcpy`/`Unmap` of `ObjectMatrixBuffer`
  **per rendered instance**, and `Timeline.cpp:92-116` does the same per material pass. In Vulkan this
  is one `vkCmdUpdateBuffer` or (better) a per-draw dynamic UBO offset. A literal implementation with a
  single 384-byte UBO rewritten between draws requires either one `vkCmdUpdateBuffer` per draw or a
  ring buffer plus `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`.
* The offset arithmetic in `RenderLayer.cpp:66-67` is a raw `memcpy` of `D3DXMATRIX Matrices[2]`
  (= 128 bytes, exactly the shader's `float4x4 worldmat; float4x4 itworldmat;`) followed by
  `MaterialData` (`MATERIALDATASIZE` = 256 B) at offset 128. **No padding, no `D3D11_BOX`.**
* `Timeline.cpp:93-94` writes `ConstantData` then `AnimatedData` back-to-back at offset 0 of the same
  384-byte buffer — i.e. the same two `float4x4`s are *overwritten* by material constants for the
  post-process path. The shim must not assume the buffer is written once per frame.

#### 2.1.3 `UpdateSubresource`

Only two live call sites, both `Subresource = 0`:
* `DX11VertexBuffer.cpp:102` — buffer, with a `D3D11_BOX` (see above).
* `DX11Texture.cpp:221` — `UpdateSubresource(TextureHandle, 0, NULL, Data, XRes*BytesPerPixel, 0)`
  in `CCoreDX11Texture2D::Update`. Note `DepthPitch = 0`, so the shim must accept `DepthPitch == 0`.

`DDSTextureLoader.cpp` also passes `initData` arrays to `CreateTexture{1,2,3}D`, which requires
`D3D11_SUBRESOURCE_DATA` arrays with `SysMemPitch`/`SysMemSlicePitch` — see §2.11.4.

#### 2.1.4 ⚠ D3D constant-buffer packing is a correctness requirement

Shaders in the exported projects declare, verbatim (extracted from
`Projects/Clean Slate/cleanslate.apx`):

```hlsl
cbuffer b : register(b0)
{
    float t,aspect,xres,yres;                       // 16 bytes under D3D packing
    float4x4 viewmat;  float4x4 projmat;
    float4 campos;     float4 lightcount;
    lightdata lights[8];                            // struct lightdata { float4 x6; } = 96 B each
    float4 res;
    float4x4 iviewmat; float4x4 iprojmat;
}
cbuffer c : register(b1)
{
    float4x4 worldmat; float4x4 itworldmat;
    float4 color; ...
}
```

C++ writes the corresponding bytes tightly:
* `Timeline.cpp:43-45,107` — `float ShaderData[4]` = `{t, aspect, xres, yres}` written at offset 0.
* `Texgen.cpp:123,151-156` — `float ShaderData[TEXGEN_MAX_PARAMS+4]` (20 floats) → offset 0.
* `Scene.cpp:156-173` — `viewmat(64) projmat(64) campos(16) lightcount(16) lights[8](768) res(16) iview(64) iproj(64)`.
* `Project.cpp:1789` and `Scene_tool.cpp` read `D3DXFLOAT16` halves directly out of the asset stream and
  reinterpret them as matrix components.

Consequences for the shim:
1. `t,aspect,xres,yres` must land at byte offsets **0,4,8,12**. A `std140`-translated shader would put
   them at 0,16,32,48 — silently corrupting *every* post-process shader.
2. `lightdata lights[8]` is an array of 96-byte structs of `float4`s → std140-compatible, fine.
3. `float4x4` arrays, `float4` scalars → std140-compatible.
4. `SHADERDATALENGTH` (80) and `SCENEDATASIZE` (1200) must remain `>=` the SPIR-V block size; if the
   compiler emits a **larger** block (e.g. std140 scalar expansion of `ShaderData[20]` → 320 bytes) the
   80-byte D3D11 buffer becomes an out-of-range UBO bind.

**Status: `UNCERTAIN` / must be validated by experiment.** glslang's HLSL frontend is *expected* to
emit explicit `Offset` decorations computed with D3D rules for `cbuffer` members (which SPIR-V then
honours, overriding std140), but this is not something I can confirm from this source tree alone.
§7 lists the exact validation test. If it fails, the fallback is to pre-process HLSL before compiling:
convert `cbuffer` blocks into `layout(std140) uniform` blocks with explicit scalar padding inserted.

### 2.2 Draw calls and primitive topology

| call | topology set immediately before | argument semantics |
|---|---|---|
| `DX11Device.cpp:776-778` | `TRIANGLELIST` | `DrawIndexed(Count*3, 0, 0)` — CoRE2 `Count` is a **triangle** count |
| `DX11Device.cpp:792-794` | `LINELIST` | `DrawIndexed(Count*2, 0, 0)` |
| `DX11Device.cpp:784-786` | `LINELIST` | `Draw(Count*2, 0)` |
| `DX11Device.cpp:800-802` | `TRIANGLELIST` | `Draw(Count*3, 0)` |
| `RenderLayer.cpp:48-52,95-99` | `TRIANGLELIST` (or `LINELIST` when `Wireframe`) / `POINTLIST` when `!Indexed` | `DrawIndexed(TriIndexCount,0,0)` or `Draw(TriIndexCount,0)` |
| `Timeline.cpp:123`, `:373`, `Texgen.cpp:172` | `TRIANGLELIST` | `Draw(6, 0)` — full-screen quad, 6 vertices, non-indexed |

`phxContext->DrawIndexed(..., 0, 0)` → `StartIndexLocation = 0`, `BaseVertexLocation = 0` **always**;
`Draw(..., 0)` → `VertexStart = 0` always. No instancing. No `D3D11_PRIMITIVE_TOPOLOGY_*_STRIP`.

`IASetIndexBuffer` in the player is always `DXGI_FORMAT_R32_UINT`
(`RenderLayer.cpp:88`: `IASetIndexBuffer( Wireframe ? WireBuffer : IndexBuffer, DXGI_FORMAT_R32_UINT, 0)`),
while CoRE2 uses 16- or 32-bit (`DX11IndexBuffer.cpp:29`). Both paths must work.

`IASetVertexBuffers` uses a **single** slot 0 with an explicit `stride` and `offset`:
* `RenderLayer.cpp:32-33,47` — `stride = PHXVERTEXFORMATSIZE` = `(3+3+3+2*4)*4 + 4` = 44 bytes (+4 pad);
  for particles `stride = 4*2*4 = 32` (`RenderLayer.cpp:43`).
* `DX11VertexBuffer.cpp:30-31` — `stride = Device->GetVertexFormatSize()` (the layout's computed size).
* `Texgen.cpp:108-109` — `stride = 6*sizeof(float) = 24`.
* `DX11Device.cpp:705` — `IASetVertexBuffers(0, 1, NULL, 0, NULL)` to unbind.

### 2.3 Input layouts and vertex formats

Two independent sources of input layouts:

**(a) CoRE2 path** — `DX11VertexFormat.cpp:32-142`. Builds `D3D11_INPUT_ELEMENT_DESC[]` from
`CArray<COREVERTEXATTRIBUTE>`:

| `COREVERTEXATTRIBUTE` | SemanticName | SemanticIndex | DXGI format | bytes |
|---|---|---|---|---|
| `COREVXATTR_POSITION3` | `"Position"` | `PosUsages++` | `R32G32B32_FLOAT` | 12 |
| `COREVXATTR_POSITION4` | `"Position"` | `PosUsages++` | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_NORMAL3` | `"Normal"` | `NormUsages++` | `R32G32B32_FLOAT` | 12 |
| `COREVXATTR_TEXCOORD2` | `"Texcoord"` | `UVUsages++` | `R32G32_FLOAT` | 8 |
| `COREVXATTR_TEXCOORD4` | `"Texcoord"` | `UVUsages++` | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_COLOR4` | `"Color"` | `ColUsages++` | **`B8G8R8A8_UNORM`** | 4 |
| `COREVXATTR_COLOR16` | `"Color"` | `ColUsages++` | `R32G32B32A32_FLOAT` | 16 |
| `COREVXATTR_POSITIONT4` | `"PositionT"` | `PosUsages++` | `R32G32B32A32_FLOAT` | 16 |
| anything else | — | — | `default:` → `return false` (`DX11VertexFormat.cpp:122-127`) | |

All entries: `InputSlot=0`, `AlignedByteOffset = running Size`, `InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA`,
`InstanceDataStepRate=0`. `Size` is returned by `GetSize()` and becomes the vertex-buffer stride.
Note the semantic spellings `"Texcoord"` / `"PositionT"` (not the classic `TEXCOORD`/`POSITIONT`) —
HLSL matches semantics case-insensitively, and the shipped shaders use `texcoord0`/`positiont`.

**(b) Phoenix path** — hand-written descriptors in `phxEngine.cpp:124-144`:

```cpp
// vxdecl  [2] "PositionT"(0) R32G32B32A32_FLOAT @0 ; "Texcoord"(0) R32G32_FLOAT @16      -> texgen quad
// vxdecl3 [2] "Position"(0)  R32G32B32A32_FLOAT @0 ; "Texcoord"(0) R32G32B32A32_FLOAT @16  -> particles
// vxdecl2 [6] "Position"0 R32G32B32_FLOAT @0 ; "Position"1 R32G32B32_FLOAT @12 ;
//             "Normal"0 R32G32B32_FLOAT @24 ; "Color"0 R8G8B8A8_UNORM @36 ;
//             "Texcoord"0 R32G32B32A32_FLOAT @40 ; "Texcoord"1 R32G32B32A32_FLOAT @56
```
`CreateInputLayout` is called **with the compiled VS bytecode** (`phxEngine.cpp:190,194,198`;
`DX11VertexFormat.cpp:131`). Under Vulkan there is no VS blob to validate against — the pipeline's
`VkVertexInputAttributeDescription` must be derived from the `D3D11_INPUT_ELEMENT_DESC` array, and the
`location` for each element must be the one glslang assigned to the matching semantic in the SPIR-V.

**Hazard:** `vxdecl2` uses `R8G8B8A8_UNORM` for `"Color"` while the CoRE2 table uses
`B8G8R8A8_UNORM` for `COREVXATTR_COLOR4`. Both feed `"Color"0` at the same byte offset with the same
*size*, so a stride/offset-derived mapping works, but if the shim ever derives locations from *format*
it will disagree. Derive from `(SemanticName, SemanticIndex)` only.

The same `RenderVertexFormat` (`vxdecl2`) is created from the VS compiled for entry `"c"` but then used
to draw meshes whose VS comes from the material shader file — so **the input-layout↔shader binding is
not 1:1** and must not be treated as pipeline-deduplication key material on the D3D11 side. It *is*
valid pipeline key material on the Vulkan side (input layout + shaders both participate).

### 2.4 Render targets, depth, MRT, clear, viewport

**Creation** (`Project.cpp:196-215` for render targets, `:217-231` for depth):

```cpp
// depth texture (Project.cpp:96)
static D3D11_TEXTURE2D_DESC depthBufferDesc =
  { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0,
    D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
// render target (Project.cpp:199-209)
tex.Width/Height = target XRes/YRes;  tex.Format = Format;      // R16G16B16A16_FLOAT or R32_FLOAT
tex.BindFlags   = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
tex.MipLevels   = 0;                                            // => full mip chain
tex.MiscFlags   = D3D11_RESOURCE_MISC_GENERATE_MIPS;
D3D11_RENDER_TARGET_VIEW_DESC rt = { Format, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };
phxDev->CreateShaderResourceView( target.Texture, NULL, &target.View );   // full-view default
phxDev->CreateRenderTargetView ( target.Texture, &rt,  &target.RTView );
```

Three things stand out and all are Vulkan-hostile:

1. **`D3D11_RTV_DIMENSION_TEXTURE2DMS` is used for single-sampled textures.** `SampleDesc.Count` is 1
   (inherited from `depthBufferDesc`), yet `ViewDimension = TEXTURE2DMS` in `Project.cpp:205`,
   `DX11Texture.cpp:96` and `RenderTargets.cpp:46`. D3D11 tolerates the mismatch (it is effectively
   ignored for a non-MSAA resource); the Vulkan backend must **normalise `*_TEXTURE2DMS` → `*_TEXTURE2D`
   when `SampleDesc.Count == 1`** rather than erroring out.
2. **Typeless depth, split view formats:** image is `R24G8_TYPELESS` (no such Vulkan format exists);
   DSV format `D24_UNORM_S8_UINT`; SRV format `R24_UNORM_X8_TYPELESS`. In Vulkan the image must be
   created as `VK_FORMAT_D24_UNORM_S8_UINT` with
   `usage = DEPTH_STENCIL_ATTACHMENT | SAMPLED` and the SRV as a *separate* sampled image view with a
   view format compatible with the image format (typically `VK_FORMAT_X8_D24_UNORM_PACK32` or
   `VK_FORMAT_D32_SFLOAT` sampled as `R32_SFLOAT`), which requires
   `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` **and** per-view format selection. Support for
   `X8_D24_UNORM_PACK32` as a *sampled* format is not universal; a fallback is to sample
   `VK_FORMAT_D32_SFLOAT` (`R32_SFLOAT` view) instead. **Risk R3.**
3. **`MipLevels = 0` + `D3D11_RESOURCE_MISC_GENERATE_MIPS`** → full mip chain on a render target
   (see §2.11.2).

**Render-target binding.** `RenderLayer.cpp:104-130` is the general path:

```cpp
static D3D11_VIEWPORT viewports[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];   // 8
static ID3D11RenderTargetView *rendertargets[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];
for (x in 0..TargetCount) { viewports[x] = {0,0,Targets[x]->XRes,Targets[x]->YRes,0,1};
                           rendertargets[x] = Targets[x]->RTView;
                           if (ClearColor && clearRenderTargets) ClearRenderTargetView(...); }
RSSetViewports(TargetCount, viewports);
OMSetRenderTargets(TargetCount, rendertargets, OmitDepthBuffer ? NULL : phxDepthBufferView);
ClearDepthStencilView(phxDepthBufferView, (D3D10_CLEAR_DEPTH * !OmitDepthBuffer) | D3D10_CLEAR_STENCIL, 1, 0);
```

* **MRT is real**: `TargetCount = nfo & ((1<<6)-1)` (`Project.cpp:327`), i.e. up to 63 render targets
  per layer, with one viewport per RT. Exported projects use more than one in places; the Vulkan
  backend needs `VkPipelineColorBlendStateCreateInfo` with N attachments and either
  `multiViewport` (for per-RT viewports) or a single shared viewport. `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT`
  must be defined as **8** (it sizes the static arrays).
* `RSSetViewports(TargetCount, ...)` with `TargetCount > 1` requires the Vulkan
  `multiViewport` feature (core in Vulkan 1.0 but `maxViewports` must be ≥ TargetCount).
* The depth clear flag expression `(D3D10_CLEAR_DEPTH * !OmitDepthBuffer) | D3D10_CLEAR_STENCIL` means:
  when the layer omits the depth buffer, the code still calls `ClearDepthStencilView` **on
  `phxDepthBufferView`** with only the stencil bit — i.e. it clears the *shared* depth/stencil even
  though it is not bound. Preserve this behaviour (clear even when not bound) or the stencil content
  diverges. (Since all `D3D11_DEPTH_STENCIL_DESC`s set `StencilEnable=false`, the visible effect is nil,
  but the call must not crash.)
* `ClearRenderTargetView(RenderTargets[x]->RTView, (float*)rv)` at `Timeline.cpp:333-336` passes an
  `ID3D11ShaderResourceView **` **cast to `float*`** — after `PSSetShaderResources(0,8,rv)` the array is
  all null so the colour is `{0,0,0,0}` in practice. The shim must copy the 4 floats out and never
  dereference beyond them.
* Other clear sites: `Timeline.cpp:371` binds `OMSetRenderTargets(1, &phxBackBufferView, NULL)` and draws
  a full-screen quad centred in the window via a viewport at
  `{ (ScreenX - W)/2, (ScreenY - H)/2, W, H, 0, 1 }` (`Timeline.cpp:370`).
* Depth-buffer-only setup (`Timeline.cpp:337`) and `OMSetRenderTargets(1, &Target->RTView, NULL)` for
  post-process passes (`Timeline.cpp:75`) — i.e. **no depth attachment** on 2D passes.

### 2.5 Compute, UAVs, 3D textures

* `CCoreDX11ComputeShader` exists and is fully wired (`DX11Shader.cpp:940-985`: `CreateComputeShader`,
  `CSSetShader`, `Release`), and `CCoreDevice::SetComputeShader`/`ApplyComputeShader`
  (`Device.h:129,59`; `Device.cpp:157`) map it into `CORERS_COMPUTESHADER`.
* **Nothing ever dispatches.** No `Dispatch` call exists in scope. Workgroup size declarations
  (`numthreads`) appear in **zero** of the 10 exported `.apx` projects.
* The volumetric path (`PHX_VOLUMETRIC_RENDERTARGETS`, off by default) creates:
  * `D3D11_TEXTURE3D_DESC` 8×8×8, `R16G16B16A16_FLOAT`, `MipLevels=0`, `BindFlags = SRV | UAV | RTV`,
    `MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS` (`phxEngine.cpp:225-242`);
  * SRV desc `{ R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE3D, Texture3D.MipLevels = -1, MostDetailedMip = 0 }`
    (`phxEngine.cpp:244-251`) — note `MipLevels = -1` cast to `UINT`;
  * UAV desc `{ textureDesc.Format, D3D11_UAV_DIMENSION_TEXTURE3D, Texture3D.FirstWSlice = 0, MipSlice = 0, WSize = 8 }`
    (`phxEngine.cpp:265-274`);
  * an 8× MSAA `R8G8B8A8_UNORM` colour target (`SampleDesc.Count = 8`,
    `Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN`) with an `RTV_DIMENSION_TEXTURE2DMS` view
    (`phxEngine.cpp:276-280`);
  * `OMSetRenderTargetsAndUnorderedAccessViews(1, &phxVoxelForceMSAARTView, nullptr, 1, 1, &phxTexture3DUAV, nullptr)`
    (`RenderLayer.cpp:144`) and `ClearUnorderedAccessViewFloat(phxTexture3DUAV, col)` (`RenderLayer.cpp:137`).

  **Recommendation:** declare the types and implement
  `CreateTexture3D`/`CreateUnorderedAccessView`/`ClearUnorderedAccessViewFloat`/
  `OMSetRenderTargetsAndUnorderedAccessViews` to the extent that they *do not fail*, but do not invest in
  the storage-image path until `PHX_VOLUMETRIC_RENDERTARGETS` is actually enabled — it is compiled out of
  both shipped configs (`PhoenixConfig.h:118`, `PhoenixConfig_full.h:118`).

### 2.6 Geometry / hull / domain shaders

* **Geometry shaders are used.** `versions[] = { "vs_5_0", "ps_5_0", "gs_5_0" }` (`Project.cpp:98-99`),
  compiled unconditionally for every material pass (`Project.cpp:531-549`) and created at
  `Project.cpp:580`. 45 `[maxvertexcount(N)]` occurrences exist across `Projects/*/*.apx`, with
  `N ∈ {4, 6}`. `RenderLayer.cpp:56` binds `GS` (or NULL) each draw.
* GS must therefore be a first-class pipeline stage: `VkPipelineShaderStageCreateInfo` with
  `VK_SHADER_STAGE_GEOMETRY_BIT`, plus `VkPipelineInputAssemblyStateCreateInfo` with
  `topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` **and** the GS's own
  `OpExecutionMode Invocations`/`OutputVertices` (glslang derives `OutputVertices` from
  `[maxvertexcount]` and the output topology from the GS's `TriangleStream`/`LineStream`).
  `VkPipelineTessellationStateCreateInfo` is not needed.
* **Hull/Domain are dead** in the default configuration (§1.7). `CreateHullShader`/`CreateDomainShader`
  must still exist and must not be called by anything; `HSSetShader`/`DSSetShader` are called with
  `NULL` on every draw (`RenderLayer.cpp:57-58`, `Texgen.cpp:113-114`) and with a handle only from the
  (unused) CoRE2 CS/HS/DS setter path.
* **Stream output is never used** — no `SOSetTargets`, no `D3D11_SO_DECLARATION_ENTRY`, no
  `CreateGeometryShaderWithStreamOutput`.

### 2.7 Shader reflection — **not used at all**

This is the single most important negative finding. Exhaustive, comment-aware search over all four
scope directories plus `apEx/apEx`:

* `ID3D11ShaderReflection` — **only** in `//`-commented lines: `DX11Shader.cpp:293,336,442,485,588,631,734,777,880,923,1027,1070`;
  `DX11Shader.h:12,34,56,78,100,122`.
* `GetConstantBufferByIndex` — same commented blocks (`DX11Shader.cpp:336,485,631,777,923,1070`).
* `D3D11_SHADER_BUFFER_DESC` — `DX11Shader.cpp:339,488,634,780,926,1073`, all commented.
* `D3DReflectFunc`, `IID_ID3D11ShaderReflection` — `DX11Shader.cpp:293,442,588,734,880,1027`, all commented.
  `d3d_reflect_func` is `typedef`'d at `DX11Shader.cpp:29` but **never assigned and never called**.
* `GetConstantBufferByName`, `GetInputParameterDesc`, `GetOutputParameterDesc`,
  `GetResourceBindingDesc`, `GetVariableByName`, `D3D11_SHADER_INPUT_BIND_DESC`,
  `D3D11_SIGNATURE_PARAMETER_DESC`, `D3DReflect` — **zero** non-comment hits repo-wide.

`ID3D11ShaderReflection*` members are commented out of each shader class
(`DX11Shader.h:12,34,56,78,100,122`), and the `Reflection` field accesses in `DX11Shader.cpp:207,230-232,
356,379-381,505,528-530` are all commented.

**Implication for the descriptor-binding plan:** the shim can and must bind descriptors **by
convention**, not by reflection:

| HLSL register | Vulkan binding | content |
|---|---|---|
| `b0` | scene/misc CB | `SceneDataBuffer` (`Timeline.cpp:98`), or `TexgenBufferPS` (`Texgen.cpp:127`), or the CoRE2 scene buffer (`Device.cpp`/`SetShaderConstants`) |
| `b1` | object CB | `ObjectMatrixBuffer` (`RenderLayer.cpp:65`, `Timeline.cpp:92`) |
| `b2+` | CoRE2 layer/gamedata/techstatic/techdynamic buffers (`COREBUFFERSCOPE`, `ConstantBuffer.h:6-14`, 6 scopes) | assigned by `SetShaderConstants(Slot, Count, Buffers)` → `Set{VS,GS,PS}ConstantBuffers(Slot, Count, ...)` (`DX11Device.cpp:822-839`) |
| `t0..t7` | sampled image array | `Textures[8]` (`RenderLayer.h:37`, `Model.h:49`); `t7` **always** gets the depth SRV (`Timeline.cpp:85`); `Texgen` binds `t0..t4` (`Texgen.cpp:145,169`) |
| `s0..s2` | sampler array | `phxEngine.cpp:212-217` binds identical samplers to VS and PS slots 0/1/2; `Texgen.cpp:94-96` rebinds 1/2 to the no-wrap and shadow-compare samplers |
| structured `t` slot | storage buffer | only `Mesh.cpp`'s tree data (§2.8.4) |

Verified by mining every `register(...)` declaration out of `Projects/Clean Slate/cleanslate.apx`: the
complete set is `cbuffer b:register(b0)` (44×), `cbuffer c:register(b1)` (44×), `Texture2D ...:register(t0..t7)`,
`SamplerState ...:register(s0..s2)`, `SamplerComparisonState Sampler3:register(s2)`. **No `Texture3D`,
no `TextureCube`, no `Texture2DArray`, no `StructuredBuffer`, no `RWTexture`, no `groupshared`,
no `ByteAddressBuffer`.**

### 2.8 Samplers

**Creation** — three at init plus one per CoRE2 sampler state:

```cpp
// phxEngine.cpp:164  (D3D11_SAMPLER_DESC, 13 positional values)
static D3D11_SAMPLER_DESC smpData =
  { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, ..._WRAP, ..._WRAP,
    0 /*MipLODBias*/, 16 /*MaxAnisotropy*/, D3D11_COMPARISON_NEVER,
    0,0,0,0 /*BorderColor[4]*/, -D3D11_FLOAT32_MAX /*MinLOD*/, D3D11_FLOAT32_MAX /*MaxLOD*/ };
phxDev->CreateSamplerState(&smpData, &TexgenSampler);              // :171
smpData.AddressU=AddressV=AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
phxDev->CreateSamplerState(&smpData, &TexgenSampler_NoWrap);       // :173
smpData.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
smpData.ComparisonFunc = D3D11_COMPARISON_LESS;
phxDev->CreateSamplerState(&smpData, &TexgenSampler_ShadowCompare); // :177
```
Note `MipLODBias` is a `FLOAT` and `MinLOD = -D3D11_FLOAT32_MAX`; the shim needs
`D3D11_FLOAT32_MAX` = `FLT_MAX` and must clamp to Vulkan's `-maxLod`/`maxLod` range (Vulkan forbids
`minLod > maxLod`, and `mipLodBias` outside `[-maxSamplerLodBias, +maxSamplerLodBias]` is an error →
clamp).

`CCoreDX11SamplerState::Update` (`DX11RenderState.cpp:182-215`) fills all 13 fields and calls
`CreateSamplerState`; `Apply(CORESAMPLER)` (`:217-231`) maps the CoRE sampler enum to a D3D slot:
`PS → Smp - CORESMP_PS0` (0..15), `VS → Smp - CORESMP_VS0` (0..3), `GS → Smp - CORESMP_GS0` (0..3).
The enum values are the offsets (`CORESMP_PS0=0 … PS15=15, VS0=100…, GS0=200…`, `Enums.h:137-163`) —
see §3.3.

**Binding** —
`phxContext->VSSetSamplers(0,1,&TexgenSampler)` / `PSSetSamplers(0,1,...)`, then slots 1 and 2
(`phxEngine.cpp:212-217`); `Texgen.cpp:94-96` rebinds PS slots 0/1/2 to wrap / no-wrap / shadow-compare.
Because the *same* sampler is bound to the same slot in both VS and PS, a single per-stage sampler set
(or a combined set) works; but `PSSetSamplers(2,1,&TexgenSampler_ShadowCompare)` means slot `s2` may be
either a regular `SamplerState` or a `SamplerComparisonState` **in the same descriptor set** — in Vulkan
those are the same descriptor type (`COMBINED_IMAGE_SAMPLER` vs `SAMPLER` + separate image); since the
sampler is bound by the *stage setter* (`PSSetSamplers`) and the image by `PSSetShaderResources`, the
shim should use **separate sampler objects** in a sampler-only descriptor set, and put comparison-ness
in the sampler (Vulkan allows a comparison sampler with `COMBINED_IMAGE_SAMPLER` only if the image view
is depth; using `VK_DESCRIPTOR_TYPE_SAMPLER` + `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` avoids the problem
entirely — recommend that).

`D3D11_FILTER_*` maps 1:1 onto `(VkFilter min, VkFilter mag, VkSamplerMipmapMode mip, VkBool32 compare)`;
the 18 `COREFILTER` values (`Enums.h:166-186`) are already ordered to match D3D11's
(`DX11Enums.cpp:70-90`).

#### 2.8.4 ⚠ The `t` slot namespace mixes sampled images and storage buffers

`Mesh.cpp:1737-1760`:

```cpp
treeDataBuffer.ByteWidth = sizeof(Arbaro::BranchData) * bufferSize;
treeDataBuffer.Usage     = D3D11_USAGE_IMMUTABLE;
treeDataBuffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
treeDataBuffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
treeDataBuffer.StructureByteStride = sizeof(Arbaro::BranchData);
phxDev->CreateBuffer(&treeDataBuffer, &subData, buffer);
resourceViewDesc.Format = DXGI_FORMAT_UNKNOWN;
resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
resourceViewDesc.Buffer.ElementWidth = sizeof(Arbaro::BranchData);
resourceViewDesc.Buffer.NumElements  = bufferSize;
phxDev->CreateShaderResourceView(*buffer, &resourceViewDesc, bufferView);
```

This is reachable in the player: `PHX_MESH_CREATETREE` / `PHX_MESH_CREATETREELEAVES` are **defined**
(`PhoenixConfig.h:18-19`), `Mesh.cpp:1767,1781` call `BuildTreeData`, and
`Project.cpp:1006-1007` copies the handles into `CphxModelObject::dataBuffer/dataBufferView`.

In the whole `apEx/Phoenix` player the resulting SRV is **never bound to a slot** (grep for
`dataBufferView` outside `Mesh.cpp` finds only the assignments in `Project.cpp:1006-1007`); the tool
publishes it through `phxMeshDataShaderView` (`Model_Tool.cpp:1529,1542`) which is likewise never bound
in the player. So the *creation* must succeed but no shader reads it in the player.

Nonetheless the shim's SRV handling must distinguish the two cases, because a D3D11 `ID3D11ShaderResourceView`
can be a buffer view. Recommended: keep one SRV abstraction that records
`enum { SRV_SAMPLED_IMAGE, SRV_STORAGE_BUFFER }` from `ViewDimension`, and give the descriptor set
**two parallel arrays** (`sampledImages[8]` + `storageBuffers[8]`) or two sets, deciding at
`*SetShaderResources` time which descriptor to write based on the SRV's kind, with the *other* array
bound to a null descriptor (`VK_EXT_robustness2` `nullDescriptor`, or a 1×1 dummy image + dummy buffer).
**Risk R4.**

### 2.9 Shader compilation (`D3DCompile`)

#### 2.9.1 How `D3DCompile` is reached — a function pointer, not a link dependency

```cpp
// phxEngine.h:52-56
typedef HRESULT( __stdcall d3dcompilefunc )( LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs );
extern d3dcompilefunc *D3DCompileCall;
```
* Assigned by the tool before use: `apEx/Phoenix_Tool/apxPhoenix.cpp:9-12`
  (`D3DCompileCall = (d3dcompilefunc*)D3DCompileFunc;`) where `D3DCompileFunc` is
  `Bedrock/CoRE2/DX11Shader.cpp:33` (`typedef HRESULT(__stdcall d3d_compile_func)(...)`), resolved at
  runtime from `d3dcompiler_43..47.dll` (`DX11Shader.cpp:103-130`).
* If unset, `InitializePhoenix` loads it itself from `d3dcompiler_47.dll`
  (`phxEngine.cpp:170-174`).
* **This indirection is the shim's entry point.** The Vulkan shim supplies its own
  `D3DCompile`-compatible function; nothing in the engine links `d3dcompiler.lib`. The shim must also
  define `ID3DBlob` / `ID3D10Blob` with `GetBufferPointer`/`GetBufferSize`/`Release`
  (both typedefs are used: `ID3D10Blob` in `DX11Shader.h`/`Project.cpp`, `ID3DBlob` in the
  `d3dcompilefunc` signature — make them the same type).
* The function-pointer ABI uses `__stdcall` and takes `ID3DInclude*` / `D3D_SHADER_MACRO*`; every
  call site passes `NULL` for both, so `pInclude`/`pDefines` handling can be minimal but the
  parameters must exist.

#### 2.9.2 Every call site, target, entry point and flag

| call site | source | entry | target | Flags1 |
|---|---|---|---|---|
| `phxEngine.cpp:186` | inline `shader[]` string (`phxEngine.cpp:146-156`) | `"a"` | `ps_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL3` |
| `phxEngine.cpp:189` | same | `"c"` | `vs_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL3` |
| `phxEngine.cpp:193` | same | `"s"` | `vs_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL3` (`PHX_HAS_STANDARD_PARTICLES`) |
| `phxEngine.cpp:197` | same | `"u"` | `vs_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL3` |
| `Project.cpp:365` (debug) / `:370` (release) | texture-filter shader from the asset stream | `"p"` | `ps_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL0` |
| `Project.cpp:541` (debug) / `:549` (release) | material pass shader, 3 entries in a loop | `"v"`,`"p"`,`"g"` | `vs_5_0`,`ps_5_0`,`gs_5_0` | `D3DCOMPILE_OPTIMIZATION_LEVEL3` |
| `DX11Shader.cpp:274` (`:272` under `CORE_API_D3DX`) | CoRE2 `CCoreDX11VertexShader::CompileAndCreate` | from `ShaderVersion`/`EntryFunction` | | `D3DCOMPILE_OPTIMIZATION_LEVEL0` (or `0` when `CORE_API_D3DX`) |
| `DX11Shader.cpp:423` (`:421`) | pixel shader | | | same |
| `DX11Shader.cpp:572` (`:570`) | geometry shader | | | same |
| `DX11Shader.cpp:718` (`:716`) | domain shader | | | same |
| `DX11Shader.cpp:864` (`:862`) | hull shader | | | same |
| `DX11Shader.cpp:1011` (`:1009`) | compute shader | | | same |

`Flags2 = 0` everywhere. `pSourceName = NULL`, `pDefines = NULL`, `pInclude = NULL` everywhere.
`D3DCOMPILE_OPTIMIZATION_LEVEL3` is `#define`d defensively at `phxEngine.cpp:119-121` if the header
lacks it. Note `sizeof(shader)` (not `strlen`) is passed at `phxEngine.cpp:186-197`, so the source is
NUL-*terminated and the size includes the terminating NUL — the compiler entry point must tolerate a
trailing NUL byte.

#### 2.9.3 `D3DCompile` return contract used by the code

* `PS`/`Blob` is only consumed when the HRESULT is `S_OK` (`DX11Shader.cpp:270-291` and friends:
  `if (D3DCompileFunc(...) != S_OK) { Success = false; ... }`), and `Err`/`Error` blobs are only
  dereferenced under an `if (Error)`/`if (!Err)` guard — **except** `DX11Shader.cpp:278` and `:427`
  etc., which do `CString((char*)Error->GetBufferPointer())` inside `if (!Err)` without checking
  `Error != NULL`. The shim must therefore return a **non-NULL error blob on failure** (an empty
  string is fine) or those debug paths will crash. This is a genuine latent bug in the original code.
* The code blob is copied (`CCoreShader::FetchBinary`, `Shader.h:29-34`) and also handed to
  `CreateInputLayout` (`DX11VertexFormat.cpp:131`, `phxEngine.cpp:190`). Under the shim, the "binary"
  is SPIR-V, so `CreateInputLayout(desc, n, binary, len, &out)` must use `binary/len` only to associate
  the layout with a shader module — or ignore it (allowed: our `CreateInputLayout` can key the layout
  purely on the `desc` array).
* `D3DXCompileShader` (`DX9Device.cpp`) and `D3DX11CompileFromMemory` (`DX11Shader.cpp` under
  `CORE_API_D3DX`, which is **not** defined — `Core2_Config.h:22`) are inactive.

#### 2.9.4 HLSL feature surface actually required (from all 10 shipped `.apx`)

Extracted by scanning `Projects/*/*.apx` (which contain the minified HLSL as ASCII) and excluding
XML-escaped duplicates:

* **Types/objects:** `Texture2D` (t0..t7), `SamplerState`, `SamplerComparisonState`, `cbuffer`
  with `register(b0)`/`register(b1)`, `float`, `float2..4`, `float3x3`, `float4x4`, `int`, `int2`,
  `int3`, `uint`, `bool`, `struct`, `static const` arrays inside functions.
* **Intrinsics observed:** `mul`, `dot`, `normalize`, `saturate`, `lerp`, `frac`, `fmod`, `pow`, `exp`,
  `log`, `sin`, `cos`, `tan`, `atan2`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `clamp`, `sign`,
  `step`, `smoothstep`, `sqrt`, `rsqrt`, `length`, `distance`, `cross`, `transpose`, `ddx`, `ddy`,
  `tex2D`-free `Texture.Sample/SampleLevel/Load`, `SamplerComparisonState.SampleCmpLevelZero`,
  `discard`, `any`, `all`, `sincos`, `countbits`-free.
  Counts in the shipped projects: `SampleLevel` ×175, `discard` ×90, `ddy` ×81, `ddx` ×68,
  `SampleCmpLevelZero` ×7, `SamplerComparisonState` ×7, `maxvertexcount` ×45.
* **Not used:** `RWTexture*`, `StructuredBuffer`, `RWStructuredBuffer`, `ByteAddressBuffer`,
  `AppendStructuredBuffer`, `ConsumeStructuredBuffer`, `groupshared`, `Interlocked*`, `Texture3D`,
  `TextureCube`, `Texture2DArray`, `Texture1D`, `Gather`, `SampleGrad`, `numthreads`, `[loop]`,
  `[unroll]`, `SV_GroupID`, `SV_DispatchThreadID`, `SV_VertexID`, `SV_InstanceID`,
  domain/hull attributes (`domain(`, `partitioning`, `outputtopology`) — all zero occurrences.
* **Semantics used in VS/PS signatures:** `POSITION0/1`, `NORMAL0`, `COLOR0`, `TEXCOORD0/1/2/3`,
  `SV_POSITION`, `SV_TARGET0`; `POSITIONT` (texgen quad) and `SV_TARGET1+` are absent.

**Assessment:** this is a well-covered subset for glslang's HLSL frontend, with two caveats:
(i) `Texture2D.Load(int3)` on non-integer-typed textures and `SampleCmpLevelZero` are less-travelled
paths that need explicit tests; (ii) the `POSITIONT` semantic (a *pre-transformed* position) is
non-standard for SPIR-V and glslang maps it to an ordinary input location — the texgen VS output must
still write `gl_Position` directly, which the shader does (`t u(l c){t s;s.r=c.o;s.o=c.r;return s;}`
with `float4 o:positiont` → `SV_POSITION`), so no special handling is needed, but verify.

### 2.10 Swapchain, present and gamma

* **Player** creates the swapchain with `D3D11CreateDeviceAndSwapChain` and
  `DXGI_SWAP_CHAIN_DESC.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`
  (`WindowHandler.cpp:44`, under `PHOENIX_ENGINE_LINEAR_RENDER`, which **is** active:
  `phxEngine.h:14` defines it and `MinimalPlayer/WindowHandler.h:2` includes `../Phoenix/phxEngine.h`) and `BufferCount = 1`, `SampleDesc.Count = 1`, `Windowed = !FullScreen`,
  `SwapEffect = DXGI_SWAP_EFFECT_DISCARD` (`WindowHandler.cpp:44`). It then
  `GetBuffer(0, __uuidof(ID3D11Texture2D), &bb)` and `CreateRenderTargetView(bb, NULL, &phxBackBufferView)`
  (`WindowHandler.cpp:96-98`) — i.e. the RTV format is the back-buffer format, which is **sRGB**.
  All shader output is therefore linear and the hardware does linear→sRGB on store. In Vulkan this is
  `VK_FORMAT_R8G8B8A8_SRGB` as the swapchain format (and the RTV must be created with the sRGB format).
  `DXGI_SWAP_EFFECT_DISCARD` + `BufferCount = 1` maps to `VK_PRESENT_MODE_MAILBOX_KHR` or
  `IMMEDIATE_KHR` with a single image — but a single-image swapchain blocks; **use 2–3 images and
  `VK_PRESENT_MODE_FIFO_KHR`/`MAILBOX`** and treat `Present(0,0)` as "no explicit sync".
* **Tool / CoRE2** creates its own swapchain with `DXGI_FORMAT_R8G8B8A8_UNORM` (no sRGB) at
  `DX11Device.cpp:179`, `BufferCount = 1`, `SampleDesc.Count = 1`, and applies gamma in shaders.
  The shim must therefore **honour the requested swapchain format** and not hard-code sRGB.
* `Present` sync flag: `SwapChain->Present(1, 0)` when vsync, `(0, 0)` otherwise
  (`DX11Device.cpp:763-764`); `Present(0,0)` also at `MinimalPlayer.cpp:290` and
  **`Project.cpp:182`** — the precalc path presents every ~40 ms (`Project.cpp:186-192`) without
  rendering. `Present` return value is checked only for `DEBUGINTOFILE` (`Project.cpp:184-188`) and in
  `Flip` (`DX11Device.cpp:771`); return `S_OK` always except on actual device loss.
* Resize path: `DX11Device::Resize` releases the views, calls `SwapChain->ResizeBuffers(desc.BufferCount, 0, 0, DXGI_FORMAT_UNKNOWN, 0)`
  (`DX11Device.cpp:288`) — note width/height **0,0** meaning "derive from the window" — then recreates
  the back-buffer and depth-buffer views. `ResizeTarget` is **not** called (commented at
  `DX11Device.cpp:305-310`). `IsWindowed()` calls `GetFullscreenState(&fs, &i)` and releases the
  `IDXGIOutput` (`DX11Device.cpp:252-265`). `SetFullScreenMode` → `SetFullscreenState` (`:314`).
  Note `DX11Device::DeviceOk()` unconditionally returns `true` (`:323-326`) — device-loss recovery is
  effectively not exercised.
* `GetDesc(&desc)` is used for `BufferDesc.Width/Height` and `BufferCount` (`DX11Device.cpp:272,288`).
* **Monitor enumeration** (`MinimalPlayer.cpp:198-215`) uses
  `CreateDXGIFactory(__uuidof(IDXGIFactory))` → `EnumAdapters(0,&adapter)` → `EnumOutputs(0,&adapterOutput)`
  → `GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, ...)` and
  `DXGI_MODE_DESC`. Only used to pick a refresh-rate numerator/denominator; the shim can return a single
  synthetic mode (`RefreshRate = {60,1}`) — but the interfaces must exist.

### 2.11 MSAA, resolve, mip generation, readback, screenshots

#### 2.11.1 MSAA and `ResolveSubresource`
* `CCoreDX11Texture2D::CreateDepthBuffer(x,y,MSCount)` (`DX11Texture.cpp:476-535`) creates a
  `R24G8_TYPELESS` texture with `SampleDesc.Count = MSCount`,
  `SampleDesc.Quality = MSCount > 1 ? D3D10_STANDARD_MULTISAMPLE_PATTERN : 0`, and picks
  `DSV_DIMENSION_TEXTURE2DMS`/`SRV_DIMENSION_TEXTURE2DMS` when MSAA. `MSCount` comes from
  `CCoreDevice::Initialize(Window, AALevel)`.
* `ResolveSubresource(pTemp, index, pSource, index, fmt)` at `DX11Texture.cpp:967`, inside
  `CCoreDX11Texture2D::ExportToImage`'s capture path — i.e. a single resolve used for readback, not a
  general MSAA-resolve-on-bind facility. Guarded by `CheckFormatSupport(fmt, &support)` +
  `D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE` (`DX11Texture.cpp:947,955`).
* The only MSAA *render target* in the codebase is the 8× voxel target in the disabled volumetric path
  (`phxEngine.cpp:276`).

#### 2.11.2 ⚠ `GenerateMips` — four call sites, no Vulkan equivalent
* `RenderLayer.cpp:156` (`GenMipmaps()`, loops over all layer targets)
* `Timeline.cpp:130` (after rendering a material pass into a target)
* `Texgen.cpp:174` (after **every** texture-filter pass)
* `Scene.cpp:198` (`GenerateMips(phxTexture3DResourceView)`, volumetric path only)
* `DX11Texture.cpp` render-target creation sets `MipLevels = 0` + `GENERATE_MIPS`
  (`DX11Texture.cpp:58-59`, `Project.cpp:201-204`, `RenderTargets.cpp:29-31`), so all of these textures
  have a full chain to generate.
* Implementations, in preference order:
  1. `vkCmdBlitImage` per mip level (requires `VK_FORMAT_FEATURE_BLIT_SRC_BIT|BLIT_DST_BIT` and
     linear-filter support for the format; `R16G16B16A16_FLOAT` usually qualifies).
  2. A generic compute/`vkCmdDraw` mip-downsample pipeline for formats without blit support.
  3. Transcode a `vkCmdPipelineBarrier`-heavy loop.
  `GenerateMips` must also transition the image layout (SRV→TRANSFER_DST→SRV) and the shim must track
  per-subresource layouts because `GenerateMips(Targets[x]->View)` is called on an image that is
  simultaneously used as a bound SRV in earlier passes (`Timeline.cpp:86`).

#### 2.11.3 Readback
* `DX11Texture.cpp:1021-1327` (`SaveDDSTexture` → `CaptureTexture`): `pContext->GetDevice(&d3dDevice)`
  (`:1158`), `pSource->GetType(&resType)` (`:913`), `pSource->QueryInterface(__uuidof(ID3D11Texture2D), &pTexture)`
  (`:919`), `pTexture->GetDesc(&desc)` (`:925`), optional `ResolveSubresource` (`:967`),
  `CopyResource(pStaging, pSource)` (`:1014`, and `:987` from the MSAA temp), then
  `Map(pStaging, 0, D3D11_MAP_READ, 0, &mapped)` (`:1275`) / `Unmap` (`:1286,1302`). **`RowPitch` is not used** — the code computes
  `mapped.pData` offsets itself (`:1283`), ignoring `RowPitch`. The staging texture is `D3D11_USAGE_STAGING` +
  `D3D11_CPU_ACCESS_READ` (`:974-975`, `:1001-1002`) with `MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE`
  (`:972,1000` — a curious bit-and that always yields 0 for non-cube textures).
* This is the screenshot path (`DX11Device::TakeScreenShot`, `DX11Device.cpp:922-954`) and matters for
  `Mesh.cpp:1126`'s `D3DX11SaveTextureToMemory`.
* `CheckFormatSupport` (`DX11Texture.cpp:947`) must report `D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE`
  honestly for the formats in question.

#### 2.11.4 DDS loading
* `DDSTextureLoader.cpp` (≈1250 lines) is a self-contained D3D11 component. Its D3D11 API usage is tiny
  (`CreateTexture1D:689`, `CreateTexture2D:748`, `CreateTexture3D:821`, `CreateShaderResourceView:711,786,833`,
  `GetFeatureLevel:1022`, `Release:715,726,790,801,837,848`) but it also uses D3D11 *constants*
  extensively (`D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION`, all the `DXGI_FORMAT_BC*` enumerators,
  `D3D11_RESOURCE_MISC_TEXTURECUBE`, mip-level math). Two options:
  1. Keep `DDSTextureLoader.cpp` and make the shim satisfy the (small) D3D11 surface plus the
     `DXGI_FORMAT_*` constants it references — cheapest, since the file is already known-good.
  2. Replace it with a native DDS→`VkImage` loader. Only worth it if the file's dependency on
     `D3D11_TEXTURE2D_DESC` arrays and `D3D11_SUBRESOURCE_DATA` arrays becomes a shim maintenance burden.
  **Recommend option 1**, since §1.7 already shows `CreateTexture{1,2,3}D` + `CreateShaderResourceView`
  must exist for the main engine anyway.
* `DDSTextureLoader.cpp:1022` switches on `d3dDevice->GetFeatureLevel()` between
  `D3D_FEATURE_LEVEL_9_1/9_2/9_3` and the 11_* levels for texture-size limits — the shim can report
  `D3D_FEATURE_LEVEL_11_0` unconditionally (then the `9_x` branches are dead).

### 2.12 Queries
Only occlusion queries, only in CoRE2 (`DX11Device.cpp:224-229, 975-996`):
`CreateQuery({D3D11_QUERY_OCCLUSION, 0})`, `Begin(q)`, `End(q)`, then a **busy-wait**
`while (S_OK != GetData(q, &queryData, sizeof(UINT64), 0)) {}` with `Flags = 0` (blocking).
The result is compared `> 0`. In Vulkan this requires
`VK_QUERY_TYPE_OCCLUSION` + a host-visible query-resolve buffer + a fence wait; because the D3D11
semantics here are *synchronous*, the simplest correct shim is to perform the resolve and wait inside
`GetData`. Note `BeginOcclusionQuery`/`EndOcclusionQuery` are called from
`CCoreAtom`/`CCoreRenderLayer` render paths in CoRE2, not from `apEx/Phoenix`. Confirm whether the
*player* ever reaches them; if not, `GetData` can return `S_OK` with a plausible value.

---

## 3. TASK 3 — CoRE2 abstract layer

### 3.1 The backend contract (every pure virtual an alternative backend must implement)

Reproduced verbatim (signatures only abbreviated where the header itself abbreviates). All from
`Bedrock/CoRE2/`. Verified pure-virtual counts outside comments: `CCoreDevice` 54, `Shader.h` 10,
`Texture.h` 10, `VertexBuffer.h` 8, `IndexBuffer.h` 6, `VertexFormat.h` 3, `RenderState.h` 14,
`ConstantBuffer.h` 1, `WindowHandler.h` 12, `SceneNodes.h` 2 — **120 total**, of which a Vulkan
backend would implement the 54 `CCoreDevice` + 10 + 10 + 8 + 6 + 3 + 14 + 1 = 106; the remainder is
engine-side (`CCoreWindowHandler`, `CCoreSceneNode`) and is OS-windowing rather than rendering.

**`CCoreDevice` (`Device.h`)** — **54** pure virtuals (verified count of `= 0;` outside comments):
```
Device.h:37  virtual void  ResetPrivateResources() = 0;
Device.h:38  virtual TBOOL InitAPI(const TU32 hWnd, const TBOOL FullScreen, const TS32 XRes, const TS32 YRes,
                                   const TS32 AALevel = 0, const TS32 RefreshRate = 60) = 0;
Device.h:64  virtual TBOOL ApplyRenderState(const CORESAMPLER Sampler, const CORERENDERSTATE RenderState,
                                            const CORERENDERSTATEVALUE Value) = 0;
Device.h:65  virtual TBOOL SetNoVertexBuffer() = 0;
Device.h:67  virtual TBOOL CommitRenderStates() = 0;
Device.h:74  virtual COREDEVICEAPI GetAPIType() = 0;
Device.h:79  virtual TBOOL Initialize(CCoreWindowHandler *Window, const TS32 AALevel = 0) = 0;
Device.h:81  virtual TBOOL DeviceOk() = 0;
Device.h:82  virtual TBOOL IsWindowed() = 0;
Device.h:83  virtual void  Resize(const TS32 xr, const TS32 yr) = 0;
Device.h:84  virtual void  SetFullScreenMode(const TBOOL FullScreen, const TS32 xr, const TS32 yr) = 0;
Device.h:94  virtual CCoreTexture2D *CreateTexture2D(const TS32 XRes, const TS32 YRes, const TU8 *Data,
                const TS8 BytesPerPixel = 4, const COREFORMAT Format = COREFMT_A8R8G8B8,
                const TBOOL RenderTarget = false) = 0;
Device.h:95  virtual CCoreTexture2D *CreateTexture2D(const TU8 *Data, const TS32 Size) = 0;
Device.h:97  virtual CCoreTexture2D *CopyTexture(CCoreTexture2D *Texture) = 0;
Device.h:102 virtual CCoreVertexBuffer *CreateVertexBuffer(const TU8 *Data, const TS32 Size) = 0;
Device.h:103 virtual CCoreVertexBuffer *CreateVertexBufferDynamic(const TS32 Size) = 0;
Device.h:109 virtual CCoreIndexBuffer  *CreateIndexBuffer(const TS32 IndexCount, const TS32 IndexSize = 2) = 0;
Device.h:115 virtual CCoreVertexFormat *CreateVertexFormat(const CArray<COREVERTEXATTRIBUTE> &Attributes,
                                                           CCoreVertexShader *vs = NULL) = 0;
Device.h:138 virtual TBOOL SetRenderTarget(CCoreTexture2D *RT) = 0;
Device.h:139 virtual TBOOL SetViewport(CRect Viewport) = 0;
Device.h:144 virtual CCoreVertexShader   *CreateVertexShader(LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                             LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:145 virtual CCorePixelShader    *CreatePixelShader (LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                             LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:146 virtual CCoreVertexShader   *CreateVertexShaderFromBlob(TU8 *Code, TS32 CodeSize) = 0;
Device.h:147 virtual CCorePixelShader    *CreatePixelShaderFromBlob (TU8 *Code, TS32 CodeSize) = 0;
Device.h:148 virtual CCoreGeometryShader *CreateGeometryShader(LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                               LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:149 virtual CCoreDomainShader   *CreateDomainShader  (LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                               LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:150 virtual CCoreHullShader     *CreateHullShader    (LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                               LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:151 virtual CCoreComputeShader  *CreateComputeShader (LPCSTR Code, TS32 CodeSize, LPCSTR EntryFunction,
                                                               LPCSTR ShaderVersion, CString *Err = NULL) = 0;
Device.h:152-157 virtual CCore{Vertex,Pixel,Geometry,Domain,Hull,Compute}Shader *Create…() = 0;   // 6 no-arg factories
Device.h:158 virtual void SetShaderConstants(TS32 Slot, TS32 Count, CCoreConstantBuffer **Buffers) = 0;
Device.h:159 virtual CCoreConstantBuffer *CreateConstantBuffer() = 0;
Device.h:161-164 virtual CCore{Blend,DepthStencil,Rasterizer,Sampler}State *Create…State() = 0;
Device.h:169 virtual TBOOL BeginScene() = 0;
Device.h:170 virtual TBOOL EndScene() = 0;
Device.h:171 virtual TBOOL Clear(const TBOOL clearPixels = true, const TBOOL clearDepth = true,
                                 const CColor &Color = CColor((TU32)0), const TF32 Depth = 1,
                                 const TS32 Stencil = 0) = 0;
Device.h:172 virtual TBOOL Flip(TBOOL Vsync = true) = 0;
Device.h:173 virtual TBOOL DrawIndexedTriangles(TS32 Count, TS32 NumVertices) = 0;
Device.h:174 virtual TBOOL DrawIndexedLines(TS32 Count, TS32 NumVertices) = 0;
Device.h:175 virtual TBOOL DrawTriangles(TS32 Count) = 0;
Device.h:176 virtual TBOOL DrawLines(TS32 Count) = 0;
Device.h:183 virtual void  ForceStateReset() = 0;
Device.h:185 virtual void  TakeScreenShot(CString Filename) = 0;
Device.h:187 virtual void  InitializeDebugAPI() = 0;
Device.h:188 virtual void  CaptureCurrentFrame() = 0;
Device.h:196 virtual void  BeginOcclusionQuery() = 0;
Device.h:197 virtual TBOOL EndOcclusionQuery() = 0;
```
Protected pure virtuals (also mandatory): `ResetPrivateResources` (above), plus the non-virtual
protected helpers `ApplyTextureToSampler`, `Apply{Vertex,Geometry,Hull,Domain,Compute,Pixel}Shader`,
`ApplyVertexFormat`, `ApplyIndexBuffer`, `ApplyVertexBuffer`, `CreateDefaultRenderStates`
(`Device.h:54-68`) — these are **shared logic in `Device.cpp`** and call the backend through
`SetToSampler`/`Apply`/`GetHandle`-like virtuals, so a backend only implements the leaf methods.
`GetUVOffset()` is a non-pure virtual with a default (`Device.h:191`).

**`CCoreShader` (`Shader.h`)** — `Create(void*,TS32)` (`:45`), `CompileAndCreate(CString*)` (`:69`),
`CreateFromBlob(void*,TS32)` (`:70`), `GetHandle()` (`:73`), and the private
`Apply()` (`:18`). `CCorePixelShader`/`GeometryShader`/`VertexShader` add **no** pure virtuals except
re-declaring `GetHandle()` in `PixelShader` (`:83`); `HullShader` (`:111`), `DomainShader` (`:121`),
`ComputeShader` (`:131`) add `GetHandle()`. Note `CCoreGeometryShader` and `CCoreVertexShader`
(`:86-102`) do **not** re-declare `GetHandle()`.

**`CCoreTexture` (`Texture.h`)** — `SetToSampler(const CORESAMPLER)` (`:15`, private+friend).
**`CCoreTexture2D`** — `Create(XRes,YRes,Data,Bpp,Format,RenderTarget)` (`:39`),
`Create(Data,Size)` (`:40`), `CreateDepthBuffer(XRes,YRes,MSCount)` (`:41`), `Lock(void**,TS32&)` (`:42`),
`UnLock()` (`:43`), `Update(Data,XRes,YRes,Bpp)` (`:45`), `Copy()` (`:50`),
`ExportToImage(CString&,TBOOL,EXPORTIMAGEFORMAT,bool)` (`:52`). `CCoreTexture3D` (`:55-60`) and
`CCoreTextureCube` (`:62-67`) are **not abstract** — they have no pure virtuals and no factory.

**`CCoreVertexBuffer` (`VertexBuffer.h`)** — `Apply(const TU32 Offset)` (`:7`), `Create(Data,Size)` (`:13`),
`CreateDynamic(Size)` (`:14`), `Update(Offset,Data,Size)` (`:15`), `Lock(Result,Offset,size,Flags)` (`:16`),
`Lock(Result)` (`:17`), `UnLock()` (`:18`), `GetHandle()` (`:19`).

**`CCoreIndexBuffer` (`IndexBuffer.h`)** — `Apply()` (`:7`), `Create(IndexCount,IndexSize=2)` (`:13`),
`Lock(Result,IndexOffset,IndexCount)` (`:14`), `Lock(Result)` (`:15`), `UnLock()` (`:16`), `GetHandle()` (`:17`).

**`CCoreVertexFormat` (`VertexFormat.h`)** — `Apply()` (`:10`),
`Create(const CArray<COREVERTEXATTRIBUTE>&, CCoreVertexShader* = NULL)` (`:15`), `GetSize()` (`:16`).

**`CCoreConstantBuffer` (`ConstantBuffer.h`)** — `Upload()` (non-pure, `:32` virtual with an empty body
in `ConstantBuffer.cpp:40-43`), `GetBufferPointer()` (`:33` pure). Shared: `Reset()` (`:30`),
`AddData(void*,TS32)` (`:31`) implemented in `ConstantBuffer.cpp:17-38` with geometric growth.

**`CCoreRenderStateBatch` (`RenderState.h`)** — `Import(CXMLNode*)` (`:59`), `Export(CXMLNode*)` (`:60`).
Then per state class:
* `CCoreBlendState`: `Update()` (`:76`), `Apply()` (`:77`), `GetHandle()` (`:92`)
* `CCoreDepthStencilState`: `Update()` (`:115`), `Apply()` (`:116`), `GetHandle()` (`:125`)
* `CCoreRasterizerState`: `Update()` (`:147`), `Apply()` (`:148`), `GetHandle()` (`:163`)
* `CCoreSamplerState`: `Update()` (`:186`), `Apply(CORESAMPLER Smp)` (`:187`), `GetHandle()` (`:205`)
(plus shared setters `Set*` and shared `Import`/`Export` in `RenderState.cpp`; the concrete
subclasses override `Import`/`Export` where they add fields — see `RenderState.h:90-91,122-123,161-162,203-204`.)

**`CCoreWindowHandler` (`WindowHandler.h`)** — `HandleResize()` (`:71`), `HandleAltEnter()` (`:72`),
`Initialize(const CCoreWindowParameters&)` (`:80`), `HandleMessages()` (`:83`), `HandleOSMessages()` (`:84`),
`DeviceOK()` (`:85`), `ToggleFullScreen()` (`:86`), `FinalizeMouseCursor()` (`:95`),
`SetWindowTitle(CString&)` (`:103`).

**`CCoreSceneNode` (`SceneNodes.h`)** — `GatherAtomsToRender(...)` (`:26`) and
`CCoreNodeRenderable::CreateAtoms(...)` (`:38`). These are engine-side, not backend-side.

**Non-abstract (shared) classes** that a backend does **not** reimplement:
`CCoreResource` (`Resource.h:18-34`; virtual `OnDeviceLost()`/`OnDeviceReset()` with default bodies),
`CCoreMaterial*` (`Material.h:21-122`), `CCoreMaterialRenderPass` / `CCoreMaterialTechnique`
(`Material.h:46-109`), `CCoreMesh`/`CCoreModel` (`Model.h:17-49`), `CCoreAtom` (`Atom.h:31-33`),
`CCoreObjectGroup` (`ObjectGroup.h:16`), `CCoreScene`/`CCoreRenderLayer*`/`CCoreRenderLayerDescriptor`
(`Scene.h`, `RenderLayer.h`).

### 3.2 How the DX11 backend implements the important ones

| concern | implementation | citations |
|---|---|---|
| **Device + swapchain** | `D3D11CreateDeviceAndSwapChain` with a `DXGI_SWAP_CHAIN_DESC` `memset` to 0, `BufferCount=1`, `BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM`, `SampleDesc.Count=1`, `Windowed=!FullScreen`; debug-flags first, fall back to flags 0 | `DX11Device.cpp:170-206` |
| **Back buffer / depth** | `SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &bb)` → `CreateRenderTargetView(bb, NULL, &BackBufferView)` → `bb->Release()`; depth = `D3D11_TEXTURE2D_DESC{D24_UNORM_S8_UINT, MipLevels=1, ArraySize=1, BIND_DEPTH_STENCIL}` + `DSV_DESC{D24_UNORM_S8_UINT, DSV_DIMENSION_TEXTURE2D, MipSlice=0}` | `DX11Device.cpp:73-168` |
| **Textures** | `CCoreDX11Texture2D::Create`: `MipLevels = rendertarget ? 0 : 1`, `MiscFlags = rendertarget ? GENERATE_MIPS : 0`, `BindFlags = SRV \| (rt ? RT : 0)`, immediate `CreateShaderResourceView(tex, NULL, &View)`, and `RTV_DESC{fmt, RTV_DIMENSION_TEXTURE2DMS, MipSlice=0}` when a render target | `DX11Texture.cpp:48-109` |
| **Depth buffers (CoRE)** | `R24G8_TYPELESS` texture, `SampleDesc.Count = MSCount`, `Quality = MSCount>1 ? D3D10_STANDARD_MULTISAMPLE_PATTERN : 0`, `BindFlags = DS \| SRV`; DSV `D24_UNORM_S8_UINT` with `TEXTURE2DMS`/`TEXTURE2D`; SRV `R24_UNORM_X8_TYPELESS` with `Texture2D.MipLevels = 1` | `DX11Texture.cpp:476-535` |
| **Constant buffers** | allocate on growth with `ByteWidth = DataLength` exactly, `USAGE_DYNAMIC`/`BIND_CONSTANT_BUFFER`/`CPU_ACCESS_WRITE`; upload via `Map(WRITE_DISCARD)`+`memcpy`+`Unmap` | `DX11ConstantBuffer.cpp:23-65` |
| **Vertex buffers** | static: `USAGE_DEFAULT` + `SUBRESOURCE_DATA`; dynamic: `USAGE_DYNAMIC`+`CPU_ACCESS_WRITE`; `Apply(Offset)` binds slot 0 with `stride = Device->GetVertexFormatSize()`; `Update` uses `UpdateSubresource` with a partial `D3D11_BOX` (only `left/right` set); `Lock` is always whole-buffer `WRITE_DISCARD` | `DX11VertexBuffer.cpp:27-135` |
| **Index buffers** | always `USAGE_DYNAMIC`+`CPU_ACCESS_WRITE`, `ByteWidth = IndexCount*IndexSize`, `Apply` picks `R16_UINT`/`R32_UINT` from the stored size | `DX11IndexBuffer.cpp:26-90` |
| **Vertex format** | build `D3D11_INPUT_ELEMENT_DESC[]` (see §2.3) and call `CreateInputLayout(desc, n, vs->GetBinary(), vs->GetBinaryLength(), &handle)`; `Apply()` = `IASetInputLayout`; `GetSize()` returns the accumulated stride | `DX11VertexFormat.cpp:32-147` |
| **Shaders** | `CompileAndCreate` → `D3DCompileFunc(code, strlen(code), NULL, NULL, NULL, entry, version, flags, 0, &blob, &err)` (or `D3DX11CompileFromMemory` under `CORE_API_D3DX`), then `Create(blob->GetBufferPointer(), blob->GetBufferSize())` → `Dev->Create{Vertex,Pixel,...}Shader(binary, len, NULL, &handle)`; `Apply()` = `{VS,PS,GS,HS,DS,CS}SetShader(handle, NULL, 0)`. `CreateFromBlob` skips compilation entirely. `_controlfp_s(_RC_NEAR)` is set around compilation (a D3D compiler FP-determinism workaround) | `DX11Shader.cpp:219-241,272-301,315-330,368-390,517-539,663-685,809-831,956-978,1049-1060` |
| **Blend/depth/raster/sampler states** | `Update()` fills the D3D11 desc from the CoRE fields via the `DX11*[]` lookup tables and calls `Create*State`; `Apply()` compares against `Device->GetCurrent*State()` and only calls `OMSet*/RSSetState` when it changed (a redundant-state cache). Sampler `Apply(Smp)` maps the CoRE sampler enum to a D3D stage+slot | `DX11RenderState.cpp:19-231` |
| **Player-side raster desc** | `BuildPassState` (`Project.cpp:1839-1871`) `memset`s `D3D11_RASTERIZER_DESC`/`D3D11_DEPTH_STENCIL_DESC`/`D3D11_BLEND_DESC` to 0, sets `FillMode=SOLID`, `CullMode=BACK`, `DepthClipEnable=true`, `AntialiasedLineEnable=true`, forces `DepthClipEnable=false` for voxelizer layers (`:1852-1855`), sets `DepthEnable=true`, `DepthWriteMask=ALL`, `DepthFunc=LESS` (`:1857-1859`), copies the static `blenddesc` (`Project.cpp:97`) into all 8 `RenderTarget[]` slots (`:1861`), then lets `CollectRenderState` (twice, tech then pass params) override `CullMode`, `DepthEnable`, `DepthWriteMask`, `DepthFunc`, and blend `BlendEnable`/`SrcBlend`/`DestBlend`. Everything else stays 0.
| **State application** | `CCoreDevice::ApplyRequestedRenderState()` (`Device.cpp:89-125`) diffs `RequestedRenderState` against `CurrentRenderState` and calls the backend's `ApplyRenderState(Smp, RS, Value)`; then rebinds the vertex buffer if changed; then `CommitRenderStates()` (a no-op for DX11, `DX11Device.cpp:709-712`) | `Device.cpp:89-125`, `DX11Device.cpp:555-701` |
| **Draws** | topology is set *before* `ApplyRequestedRenderState()`, then `DrawIndexed(Count*3,0,0)` / `Draw(Count*2,0)` / etc. | `DX11Device.cpp:774-804` |
| **Render target + depth binding** | `SetRenderTarget(NULL)` → `OMSetRenderTargets(1, &BackBufferView, DepthBufferView)`; `SetRenderTarget(non-NULL)` → **`return false`** (unimplemented, `DX11Device.cpp:905`) | `DX11Device.cpp:896-906` |
| **Viewport** | `SetViewport(CRect)` → `RSSetViewports(1, &{x1,y1,max(0,Width),max(0,Height),0,1})` | `DX11Device.cpp:806-820` |
| **Shader constants** | `SetShaderConstants(Slot, Count, CCoreConstantBuffer**)` pulls `GetBufferPointer()` from each and calls `VSSet/GSSet/PSSetConstantBuffers(Slot, Count, buffers)` — note it sets all three stages with the same buffers | `DX11Device.cpp:822-839` |
| **Lock / readback** | `CCoreDX11Texture2D::Lock` returns `false` and `UnLock` returns `true` — **both are stubs with the real bodies commented out** (`DX11Texture.cpp:183-203`); real readback only happens in `ExportToImage` via staging + `Map(READ)` | `DX11Texture.cpp:183-203,1021-1330` |
| **Device lost/reset** | `OnDeviceLost()` releases render-target textures; `OnDeviceReset()` recreates them (`BASEASSERT(Create(...))`) | `DX11Texture.cpp:205-214` |
| **Resource tracking** | every `CCoreResource` ctor/dtor calls `Device->AddResource(this)` / `RemoveResource(this)` (friend access); `Device::~CCoreDevice` frees leftovers | `Resource.cpp:13,18`, `Resource.h:18-34` |
| **Occlusion queries** | one `D3D11_QUERY_OCCLUSION` created at init; `Begin(q)` / `End(q)` / blocking `GetData(q,&u64,8,0)`; result `> 0` | `DX11Device.cpp:224-229,975-996` |
| **Screenshot** | `GetBuffer` → wrap in a transient `CCoreDX11Texture2D` via `SetTextureHandle` → `ExportToImage(..., CORE_PNG, false)` → clear handles | `DX11Device.cpp:922-954` |
| **MSAA / resolve** | see §2.11.1; `ResolveSubresource` only inside `SaveDDSTexture` | `DX11Texture.cpp:967` |
| **Mip generation** | `GenerateMips(srv)` on the D3D11 context, called by engine code (not by CoRE2) | `RenderLayer.cpp:156`, `Timeline.cpp:130`, `Texgen.cpp:174`, `Scene.cpp:198` |

### 3.3 D3D11 leaks into the "abstract" API

This is the full list of places where D3D-specific detail is *not* abstracted away. Each is a place
where the Vulkan shim must reproduce D3D semantics rather than invent its own.

**1. `COREFORMAT` ↔ `DXGI_FORMAT` (and the format table lives in a DX11-named file)**

`Bedrock/CoRE2/DX11Enums.h:7` and `DX11Enums.cpp:8-16`:
```cpp
DXGI_FORMAT DX11Formats[] = {                 // indexed by COREFORMAT
  DXGI_FORMAT_UNKNOWN,               // COREFMT_UNKNOWN          = 0
  DXGI_FORMAT_B8G8R8A8_UNORM,        // COREFMT_A8R8G8B8         = 1
  DXGI_FORMAT_R32G32B32A32_FLOAT,    // COREFMT_A32B32G32R32F    = 2
  DXGI_FORMAT_R32_FLOAT,             // COREFMT_R32F            = 3
  DXGI_FORMAT_R16G16_FLOAT,          // COREFMT_G16R16F         = 4
  DXGI_FORMAT_R16G16B16A16_FLOAT,    // COREFMT_R16G16B16A16_FLOAT = 5
};
```
`GetFormat(DXGI_FORMAT)` (`DX11Enums.cpp:164-174`) has **no case for `DXGI_FORMAT_R16G16B16A16_FLOAT`**
→ it returns `COREFMT_UNKNOWN`, even though `DX11Formats[COREFMT_R16G16B16A16_FLOAT]` produces it. So a
round-trip through `GetFormat` loses that format. `GetFormat` is only called from
`DX11Texture.cpp:177` (`CCoreDX11Texture2D::Create`) to set the member `Format`; the value is not
re-read anywhere that matters for rendering, but **do not "fix" it** if byte-for-byte behavioural
parity matters. Also note the table is a plain `DXGI_FORMAT[]` exported from a header that is
`#ifdef CORE_API_DX11`-guarded (`DX11Enums.h:5`), so a Vulkan build cannot include it — the Vulkan
shim must supply `DXGI_FORMAT` constants and this table itself.

**2. `D3DX11.h` / DXGI types in `PhoenixConfig`-independent headers**

`apEx/Phoenix/phxEngine.h:10` `#include <D3DX11.h>` and `:16-43` raw `ID3D11*` globals; `phxMath.h:4`,
`phxSpline.h:4`, `Mesh.h:7`, `Model.h:8`, `RenderLayer.h:6`, `Scene.h:9` all `#include <D3DX10Math.h>`.
`Bedrock/CoRE2/Core2_Config.h:46-57` `#include <d3d11.h>` + `#pragma comment(lib,"d3d11.lib")`.

**3. `CORELOCK_*` constants are raw `D3DLOCK_*` values**

`Enums.h:190-194`:
```cpp
#define CORELOCK_READONLY    0x00000010L   // == D3DLOCK_READONLY
#define CORELOCK_DISCARD     0x00002000L   // == D3DLOCK_DISCARD
#define CORELOCK_NOOVERWRITE 0x00001000L   // == D3DLOCK_NOOVERWRITE
#define CORELOCK_NOSYSLOCK   0x00000800L   // == D3DLOCK_NOSYSLOCK
#define CORELOCK_DONOTWAIT   0x00004000L   // == D3DLOCK_DONOTWAIT
```
The DX11 vertex/index buffer `Lock` implementations **ignore `Flags` entirely** and always
`WRITE_DISCARD` (`DX11VertexBuffer.cpp:108-123`, `DX11IndexBuffer.cpp:63-78`), so the values are
currently decorative — but `CCoreVertexBuffer::Lock(..., Flags)` is part of the abstract API.

**4. `CORESAMPLER` enum values *are* D3D stage-offset slot numbers**

`Enums.h:137-163`: `CORESMP_PS0..PS15 = 0..15`, `CORESMP_VS0..VS3 = 100..103`,
`CORESMP_GS0..GS3 = 200..203`. `DX11Texture.cpp:34-45`, `DX11Device.cpp:602-611` and
`DX11RenderState.cpp:217-231` all branch on these ranges and subtract the base to get the D3D slot:
```cpp
if (smp >= CORESMP_PS0 && smp <= CORESMP_PS15) PSSetShaderResources(smp, 1, &View);
if (smp >= CORESMP_VS0 && smp <= CORESMP_VS3) VSSetShaderResources(smp - CORESMP_VS0, 1, &View);
if (smp >= CORESMP_GS0 && smp <= CORESMP_GS3) GSSetShaderResources(smp - CORESMP_GS0, 1, &View);
```
`CORE_PS_SAMPLERCOUNT 16`, `CORE_VS_SAMPLERCOUNT 4`, `CORE_GS_SAMPLERCOUNT 4` (`Enums.h:133-135`).

**5. Raw D3D11 enums used as *data* — the values must not be renumbered**

`apEx/Phoenix/Project.cpp:1950-1951`:
```cpp
BlendDesc.RenderTarget[ z ].SrcBlend = (D3D11_BLEND)( ( v.BlendMode & 15 ) );
BlendDesc.RenderTarget[ z ].DestBlend = (D3D11_BLEND)( ( v.BlendMode >> 4 ) );
```
The material file stores a **blend-factor index nibble** that is cast directly to `D3D11_BLEND`.
Stronger still: the *stored parameter types themselves* are D3D11 enums —
`apEx/Phoenix/Material.h:106-110` declares
```cpp
unsigned char ZMode; unsigned char BlendMode;
D3D11_COMPARISON_FUNC ZFunction;
D3D11_CULL_MODE CullMode;
```
inside `CphxMaterialParameterBatch`, and the tool writes `D3D11_CULL_NONE` into it
(`apEx/Phoenix_Tool/apxProject.cpp:58`). Those values are then assigned straight into the D3D11 descs
(`Project.cpp:1890-1891,1894,1950-1951,1955`). Likewise `Project.cpp:1891` uses
`D3D11_DEPTH_WRITE_MASK_ALL`/`_ZERO` as data and `Project.cpp:1858-1859` / `:97` use the enum constants
in aggregate initialisers. **Any divergence in `D3D11_BLEND`, `D3D11_BLEND_OP`,
`D3D11_COMPARISON_FUNC`, `D3D11_CULL_MODE` or `D3D11_DEPTH_WRITE_MASK` numbering silently corrupts
every material.**
**All `D3D11_BLEND`, `D3D11_BLEND_OP`, `D3D11_COMPARISON_FUNC`, `D3D11_DEPTH_WRITE_MASK` values must be
bit-identical to the real D3D11 numbering** or materials will silently render with the wrong blend
factors. Same for `D3D11_COMPARISON_FUNC` via `DX11ComparisonFunctions[]` (`DX11Enums.cpp:32-42`) whose
index is a `CORECOMPARISONFUNCTION` (`Enums.h:35-45`).

**6. Raw D3D11 constants passed by engine/tool code into CoRE2-adjacent structures**

| file:line | code |
|---|---|
| `apEx/Phoenix_Tool/apxProject.cpp:58` | `p->Parameter.Value.CullMode = D3D11_CULL_NONE;` (a `D3D11_CULL_MODE` stored in a material parameter, i.e. the value is round-tripped through the project file) |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1238` | `RasterDesc.ScissorEnable = false;` (D3D11 field name in tool code) |
| `apEx/Phoenix/Project.cpp:1841-1843` | `D3D11_RASTERIZER_DESC RasterDesc; D3D11_DEPTH_STENCIL_DESC DepthStencilDesc; D3D11_BLEND_DESC BlendDesc;` declared as members of `CphxMaterialPassConstantState`-adjacent tool structures |
| `apEx/Phoenix/Project.cpp:84,1873` | `CollectRenderState(..., D3D11_RASTERIZER_DESC&, D3D11_DEPTH_STENCIL_DESC&, D3D11_BLEND_DESC&, ...)` — D3D11 descs in a function *signature* in `apEx/Phoenix` (not the tool) |
| `apEx/Phoenix/Project.cpp:94-97` | file-static D3D11 descs with positional JSON-like initialisers |
| `apEx/Phoenix/RenderLayer.h:22-37` | `ID3D11Buffer/VertexShader/PixelShader/GeometryShader/HullShader/DomainShader/BlendState/RasterizerState/DepthStencilState/ShaderResourceView` as struct members of the player's render-instance structure |
| `apEx/Phoenix/Model.h:49,73-78` | `ID3D11ShaderResourceView *Textures[8]`, `ID3D11Buffer *IndexBuffer, *WireBuffer, *dataBuffer`, `ID3D11ShaderResourceView *dataBufferView` |
| `apEx/Phoenix/Scene.h:280,392-393` | `ID3D11Buffer* VertexBuffer`, commented `ID3D11ComputeShader*` |
| `apEx/Phoenix/Mesh.h:226-227` | `ID3D11Buffer *dataBuffer; ID3D11ShaderResourceView *dataBufferView;` |
| `apEx/Phoenix/RenderTarget.h:17-19` | `ID3D11RenderTargetView *RTView; ID3D11ShaderResourceView *View; ID3D11Texture2D *Texture;` |
| `apEx/Phoenix/Material.h:166-170` | `ID3D11{Vertex,Pixel,Geometry,Hull,Domain}Shader *VS/PS/GS/HS/DS;` with the comment "WARNING! DO NOT CHANGE THE ORDER OF THESE DUE TO A MEMCPY" — i.e. **the layout is ABI** (see §1.1 `Scene_tool.cpp:128-149`) |
| `apEx/Phoenix/phxEngine.h:16-43` | the ~25 global raw D3D11 pointers |
| `apEx/Phoenix/phxEngine.h:52-56` | `typedef HRESULT(__stdcall d3dcompilefunc)(..., ID3DBlob**, ID3DBlob**)` in the public engine header |
| `apEx/Phoenix/RenderLayer.cpp:104-105` | `static D3D11_VIEWPORT viewports[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]` |
| `apEx/Phoenix/RenderLayer.h:41` | `D3DXMATRIX Matrices[2];` immediately followed by `float MaterialData[...]` — a 128-byte-then-256-byte GPU layout asserted only by `memcpy` |
| `apEx/Phoenix/RenderTarget.cpp:6-13` | `D3D11_VIEWPORT v; phxContext->RSSetViewports(1,&v);` — raw D3D11 viewport in the player's render-target class |

**7. `GetHandle()` escape hatches break the abstraction**

`GetHandle()` is declared on `CCoreShader` (`Shader.h:73`), `CCoreVertexBuffer` (`VertexBuffer.h:19`),
`CCoreIndexBuffer` (`IndexBuffer.h:17`), `CCoreRenderStateBatch` subclasses (`RenderState.h:92,125,163,205`)
— all returning `void*` that the DX11 backend fills with the raw D3D11 interface pointer
(`DX11Shader.h:26,48,70`, `DX11RenderState.h:20,36,52,68`, `DX11IndexBuffer.h:28`). Consumers cast it
straight back (table in §0.1). There is **no** `GetDevice()`/`GetDeviceContext()` escape hatch on
`CCoreDevice` in the abstract API (grep found none in `apEx/Phoenix`, `apEx/Phoenix_Tool`,
`apEx/MinimalPlayer`), but `CCoreDX11Device::GetDevice()`/`GetDeviceContext()` are public on the
concrete class (`DX11Device.h`) and are used internally by every `DX11*.cpp`.

**8. Backend-selection mechanism**

`Core2_Config.h:18-59`:
```cpp
//#define CORE_API_DX9        // "DX9 build is not up to date and will not compile."
#define CORE_API_DX11
//#define CORE_API_D3DX
...
#ifdef CORE_API_DX11
  #include <d3d11.h>
  #pragma comment(lib,"d3d11.lib")
  #ifdef CORE_API_D3DX
    #include <D3DX11.h>
    #pragma comment(lib,"d3dx11.lib")
  #endif
#endif
```
`CORE_API_D3DX` is **off**, which means the in-repo `DDSTextureLoader`/`PNGDecompressor` path is the
live one and `D3DX11CreateTextureFromMemory` inside `DX11Texture.cpp:132` is dead.

For the shim the correct move is **not** to add `CORE_API_VULKAN` (that would require a new
`Vulkan*.cpp` twin of every `DX11*.cpp`). Instead:
* keep `CORE_API_DX11` defined, so all `#ifdef CORE_API_DX11` blocks build;
* replace the *headers* `d3d11.h`, `dxgi.h`, and `D3DX10Math.h` on the include path with the shim;
* provide `DX11Enums.cpp`'s tables (they are inside `#ifdef CORE_API_DX11` too, so they compile as-is,
  provided the shim's `DXGI_FORMAT` names match);
* supply `D3DCompile` through `D3DCompileCall`/`D3DCompileFunc` (§2.9.1) rather than linking
  `d3dcompiler.lib`.
Per-file Vulkan equivalents needed: none of `DX11*.cpp` needs a twin. Only
`DDSTextureLoader.cpp` (§2.11.4) and the `d3dcompiler`/`d3dx11` dynamic loading
(`DX11Shader.cpp:36-165`, `Mesh.cpp:1126`, `Texgen.cpp:226`) need Vulkan-side replacements.

---

## 4. TASK 4 — D3DX math surface

`apEx/Phoenix` uses **D3DX10Math** (`#include <D3DX10Math.h>`), not D3DX9. The header is
**bundled in-repo** at `apEx/ThirdParty/D3D/Include/D3DX10math.h` (+ `D3DX10math.inl`), which is
authoritative for signatures and aggregate layout. Its free functions are **library exports from
`d3dx10.lib`** (only class constructors/operators are implemented in the `.inl` — verified: the `.inl`
contains only `D3DX{COLOR,PLANE,QUATERNION,MATRIX,VECTOR2,3,4}[_16F]::` member definitions), so **every
free function must be reimplemented**.

Counts are comment-stripped occurrences across `apEx/Phoenix`, `apEx/Phoenix_Tool`,
`Bedrock/CoRE2`, `apEx/MinimalPlayer` (plus `apEx/Utils`, which uses none).

### 4.1 Types

| type | count | files | required semantics |
|---|---|---|---|
| `D3DXMATRIX` | 198 | `Arbaro.{h,cpp}`, `Mesh.{h,cpp}`, `Model.{h,cpp}`, `Project.cpp`, `Scene.{h,cpp}`, `Timeline.cpp`, `phxSpline.cpp`, `RenderLayer.h`, `phxMath.h`, tool files | **`struct D3DXMATRIX : public D3DMATRIX`** where `_D3DMATRIX` (`D3DX10math.h:39-50`) is `union { struct { FLOAT _11,_12,_13,_14,_21,…,_44; }; FLOAT m[4][4]; }`. 64 bytes, no padding. **Both access forms are used**: `._11`…`._44` at `TreeGen.cpp:574-595` and `Model_Tool.cpp:1873`; `.m[r][c]` at `TreeGen.cpp:45,50`. Also needs ctor from `const FLOAT*`, from `const D3DMATRIX&`, `operator FLOAT*`/`const FLOAT*` (used for `memcpy`/`(float*)` casts), `operator *`, `operator *=`, `operator ==`/`!=`. |
| `D3DXVECTOR3` | 286 | widespread | `D3DXVECTOR3()` (uninitialised), `(const FLOAT*)`, `(const D3DVECTOR&)`, **`(const D3DXFLOAT16*)`**, `(FLOAT,FLOAT,FLOAT)`; `operator FLOAT*`/`const FLOAT*`; `+= -= *= /=`, unary `+ -`, binary `+ - * /`, `friend FLOAT * D3DXVECTOR3`, `== !=`. The `D3DXFLOAT16*` ctor is **used**: `Project.cpp:911,918,943,944` (`D3DXVECTOR3( filtertransform )` where `filtertransform` is `D3DXFLOAT16*`). |
| `D3DXVECTOR4` | 40 | `Mesh`, `Scene`, `Timeline`, `Model_Tool` | same shape, `x,y,z,w`. Used for the camera position and w-divided transforms. |
| `D3DXVECTOR2` | 43 | `Arbaro.cpp`, `Mesh.{h,cpp}` | same shape, `x,y`. |
| `D3DXQUATERNION` | 50 | `Mesh`, `Project`, `Scene`, `phxSpline`, tool | `{FLOAT x,y,z,w;}` with ctor from `const FLOAT*` **and from `const D3DXFLOAT16*`** (`D3DX10math.h:453-459`); `Project.cpp:912,919,944` construct `D3DXQUATERNION( filtertransform + 3 )`. Needs `operator FLOAT*`, arithmetic ops, `==`/`!=`. |
| `D3DXFLOAT16` | 93 | `Mesh.{h,cpp}`, `Arbaro.h`, `Model.h`, `Project.cpp`, `phxSpline.{h,cpp}`, `Scene_tool.cpp`, `phxSplineExt.cpp` | `typedef struct D3DXFLOAT16 { WORD value; }` — **must be exactly 2 bytes** because `CphxSplineKey` (`phxSpline.h:19-24`) stores `D3DXFLOAT16 Value[4]` / `controlvalues[6]` and is read straight out of the asset stream. Needs `operator FLOAT()` (half→float) and `D3DXFLOAT16(FLOAT)` (float→half). Raw reinterpretation is used: `Project.cpp:256-259` `return *((D3DXFLOAT16*)&s)`; `Project.cpp:1789` `*((D3DXFLOAT16*)(&Data[12]))`. |
| `D3DXPLANE`, `D3DXCOLOR`, `D3DCOLOR`, `D3DXMATRIXA16`, `D3DXVECTOR*_16F` | **0 uses** | — | only defined in the bundled header. `D3DXMATRIXA16` is *not* used anywhere in the project code (grep found only the header's own definition at `D3DX10math.h:413-449`), so **16-byte alignment is not required** — but `RenderLayer.h:41` `D3DXMATRIX Matrices[2]` must be exactly 128 bytes with no padding for the `memcpy` at `RenderLayer.cpp:66` to line up with the shader's `float4x4` pair. |
| `DirectX::PackedVector::HALF`, `XMConvertHalfToFloatStream` | 1 each | `Bedrock/CoRE2/DX11Texture.cpp:406` | `#include <DirectXPackedVector.h>` + `<DirectXMath.h>` at `DX11Texture.cpp:273-274`. Needs a half→float **stream** conversion with input stride 2, output stride 4. |
| `XMConvertFloatToHalf` | 1 (raw count; appears only in a comment/other context — **`UNCERTAIN`**) | — | grep found 1 raw occurrence but 0 comment-stripped calls; treat as unused. |
| `D3DXFLOAT16` array converters `D3DXFloat32To16Array` / `D3DXFloat16To32Array` | **0 live** | `float16.cpp:60`, `DX11Texture.cpp:404` — **both inside comments** | `apEx/Phoenix/float16.{h,cpp}` is **entirely commented out** (both files are 100 % `//`), so the project relies on D3DX's `D3DXFLOAT16` member functions, not on these array helpers. A portable header only needs the scalar half↔float conversions unless `D3DXFLOAT16::operator FLOAT()` is implemented via the array function (it is, in the real `.inl` at `D3DX10math.inl:40-47` — reimplement directly). |

### 4.2 Functions

| function | n | semantics to implement (verified signature / doc-comment in `D3DX10math.h`) |
|---|---|---|
| `D3DXMatrixIdentity` | 17 | `m = I` |
| `D3DXMatrixMultiply(pOut, pM1, pM2)` | 27 | `out = M1 * M2` (row-major, row-vector convention: `v' = v*M`) |
| `D3DXMatrixInverse(pOut, pDeterminant, pM)` | 12 | general 4×4 inverse; `pDeterminant` is **passed as `NULL` at all 12 sites** (`Scene.cpp:143,144,551`, `Project.cpp:970`, `Mesh.cpp:1007`, …), so singularity handling must still produce something (D3DX returns NULL only if `pOut` is NULL) |
| `D3DXMatrixTranspose` | 6 | `out = Mᵀ`; used at `Scene.cpp:551` (`D3DXMatrixTranspose(&mx, &phxViewMatrix)`) and in `Mesh.cpp` normal-matrix paths |
| `D3DXMatrixTransformation` | 12 | **documented formula in the bundled header** (`D3DX10math.h:994-999`): `Mout = Msc⁻¹ * Msr⁻¹ * Ms * Msr * Msc * Mrc⁻¹ * Mr * Mrc * Mt`, NULL args treated as identity. `Ms` = scaling, `Msr` = scaling rotation (quaternion→matrix), `Msc` = scaling centre, `Mr` = rotation (quaternion→matrix), `Mrc` = rotation centre, `Mt` = translation. **Every call site passes `pScalingCenter = NULL`, `pScalingRotation = NULL`, `pRotationCenter = NULL`** (`Scene.cpp:250,320,342`; `Mesh.cpp:1007,1204,1275,1537`; `Scene_tool.cpp:1305,1314`; `Model_Tool.cpp:1748,1755`) — so in practice the needed behaviour reduces to `Mout = Ms * Mr * Mt`, i.e. `D3DXMatrixScaling * D3DXMatrixRotationQuaternion * D3DXMatrixTranslation` in **that multiplication order**. Still implement the full formula (2 of the 12 sites are in `apEx/apEx`, outside scope, and use non-NULL centres). `Mesh.cpp:1537` passes the result of `D3DXQuaternionRotationAxis(&q,&Axis,Angle)` inline as `pRotation`. |
| `D3DXMatrixScaling` | 1 | `Mesh.cpp:1204`-adjacent; `diag(sx,sy,sz,1)` |
| `D3DXMatrixTranslation` | 1 | `M41,M42,M43 = t` (row-vector convention) |
| `D3DXMatrixRotationX/Y/Z` | 2 / 1 / 1 | standard axis rotations |
| `D3DXMatrixRotationAxis(pOut,pV,FLOAT Angle)` | 4 | axis-angle → matrix; `Arbaro.cpp`, `Mesh.cpp`, `Timeline.cpp` |
| `D3DXMatrixRotationQuaternion` | (commented in the codebase; declared `D3DX10math.h:985`) | quaternion → matrix; needed indirectly by `D3DXMatrixTransformation` |
| `D3DXMatrixDecompose(pOutScale,pOutRot,pOutTrans,pM)` | 1 | `Scene_tool.cpp` only — tool path |
| `D3DXMatrixIsIdentity` | 1 | `Scene_tool.cpp` |
| `D3DXMatrixLookAtRH(pOut, pEye, pAt, pUp)` | 1 | `Timeline.cpp:174` — **RH**; `up` is passed as `(D3DXVECTOR3*)&rolledup` where `rolledup` is a `D3DXVECTOR4` (`Timeline.cpp:174`) |
| `D3DXMatrixPerspectiveOffCenterRH(pOut,l,r,b,t,zn,zf)` | 1 | `Timeline.cpp:186`: `( &phxProjectionMatrix, -r+xOffset, r+xOffset, -t+yOffset, t+yOffset, zn, 2000.0f )` — **RH**, so the Vulkan depth range must be adapted: D3DX RH perspective produces z ∈ [0,1] with a **reversed** sign convention relative to Vulkan's `[0,1]` window; combined with D3D's `VK_COMPARE_OP_LESS` (`D3D11_COMPARISON_LESS`, `Project.cpp:1859`) and `Clear(depth=1)`, the port must either keep the D3D matrix and use `VK_COMPARE_OP_LESS_OR_EQUAL` with a **negated** Vulkan viewport depth (`VkViewport.minDepth=1,maxDepth=0` is a GL-style trick; Vulkan 1.1+ cannot flip depth via negative-height viewport for z). **Risk R5** — see §6. |
| `D3DXMatrixPerspectiveFovRH` | 0 live (1 in a comment, `Timeline.cpp:188`) | — |
| `D3DXVec3Transform(pOut4, pV3, pM)` | 20 | returns a **`D3DXVECTOR4`**: `out.x = v.x*m11 + v.y*m21 + v.z*m31 + m41` etc. **No w-divide.** |
| `D3DXVec3Transform3` | 12 | **not a D3DX function** — local helper `apEx/Phoenix/Mesh.cpp:17-22`: `D3DXVec3Transform(&v, in, m); *out = *(D3DXVECTOR3*)&v;` (keeps the w component in memory but reinterprets the first 3 floats) |
| `D3DXVec3TransformNormal` | 1 (`Arbaro.cpp`) | 3×3 part only, no translation |
| `D3DXVec4Transform` | 6 | full 4×4, no divide |
| `D3DXVec3Normalize` | 37 | returns the original length; must handle zero-length (all sites ignore the return except `phxMath.cpp:133`) |
| `D3DXVec3Length` / `LengthSq` | 12 / 1 | |
| `D3DXVec3Cross` | 12 | |
| `D3DXVec3Dot` | 9 | |
| `D3DXVec3Unproject(pOut,pV,pViewport,pProj,pView,pWorld)` | 4 | tool-only (`Model_Tool.cpp`, `Scene_tool.cpp`); takes **`CONST D3D10_VIEWPORT*`** (`D3DX10math.h:817-818`), i.e. the shim needs `D3D10_VIEWPORT` too |
| `D3DXVec4Lerp` | 1 | |
| `D3DXQuaternionIdentity` | 5 | |
| `D3DXQuaternionMultiply` | 6 | |
| `D3DXQuaternionInverse` | 1 | |
| `D3DXQuaternionRotationAxis` | 4 | returns `D3DXQUATERNION*` (used inline as an argument, `Mesh.cpp:1537`) |
| `D3DXQuaternionRotationMatrix` | 1 | `Scene.cpp` |
| `D3DXQuaternionToAxisAngle` | 1 | `Mesh.cpp` |
| `D3DXQuaternionSlerp` | 1 | `phxSpline.cpp:239` — under `#ifdef SPLINE_INTERPOLATION_SLERP`; `SPLINE_INTERPOLATION_*` selection is `UNCERTAIN` (both `SLERP` and `SQUAD` blocks are compiled under their own `#ifdef`s; find which is defined in `PhoenixConfig*.h`) |
| **`D3DXQuaternionSquadSetup(qa,qb,qc, q0,q1,q2,q3)`** | 1 | `phxSpline.cpp:256`. Control points chosen for tangent continuity: `qa = q1 * exp(-(ln(q1⁻¹q0) + ln(q1⁻¹q2))/4)` style; the canonical D3DX implementation is |
| **`D3DXQuaternionSquad(qOut, q1, a, b, c, t)`** | 1 | `phxSpline.cpp:257`. Documented in the header (`D3DX10math.h:1171-1174`): **`Slerp(Slerp(Q1, C, t), Slerp(A, B, t), 2t(1-t))`** — i.e. exactly that two-slerp blend with the factor `2t(1-t)` |
| `D3DXIntersectTri(p0,p1,p2,pRayPos,pRayDir,pU,pV,pDist)` | 2 | `BOOL`; Moeller–Trumbore; `Model_Tool.cpp` only |

**`D3DXQuaternionSquadSetup` precision:** the bundled header documents only "control points … chosen to
ensure continuity of tangents with adjacent segments" (`D3DX10math.h:1179-1184`) without giving the
formula. The canonical Microsoft implementation is:
```
q0' = q0 * sign(dot(q0,q1));  q2' = q2 * sign(dot(q2,q1));  q3' = q3 * sign(dot(q3,q2));
qa = q1 * exp( -( ln(q1⁻¹ q0') + ln(q1⁻¹ q2') ) / 4 )
qb = q2 * exp( -( ln(q2⁻¹ q1)  + ln(q2⁻¹ q3') ) / 4 )
qc = q2
```
**Status: `UNCERTAIN`** — this is from the D3DX SDK reference, not from the in-repo source, and the
sign-fixup details (`bFlip`) differ between D3DX versions. Since the site is a single quaternion spline
in `phxSpline.cpp:247-263` behind `#ifdef SPLINE_INTERPOLATION_SQUAD`, the pragmatic move is to
**verify which `#ifdef` is active**, and if it is `SQUAD`, validate against a reference render.
`D3DXQuaternionSquad` itself is unambiguous (documented in the header).

### 4.3 Porting hazards for the math layer

* **Row-vector / row-major convention.** `D3DXMatrixMultiply(out, M1, M2)` = `M1*M2` and
  `D3DXVec3Transform` computes `v * M` (row vector). `Scene.cpp:250-251` relies on this:
  `D3DXMatrixMultiply(&m, &prs, &CurrentMatrix)` (local-then-parent). The **ViewProjection** convention
  in the shaders is the D3D one: `cbuffer b` has `viewmat` then `projmat`, and the shaders do
  `mul(viewmat, ...)`/`mul(projmat, ...)` — `mul(M, v)` with row-major storage means HLSL's `mul` reads
  the matrix as row-major, which matches `D3DXMATRIX.m[4][4]` uploaded verbatim. **Do not transpose on
  upload** except where the code already does (`D3DXMatrixTranspose` at `Scene.cpp:551`, `Mesh.cpp`,
  `Model.cpp`, `Arbaro.cpp`).
* **`RenderLayer.h:41-42` layout assertion**: `D3DXMATRIX Matrices[2]` (128 B) followed by
  `float MaterialData[MATERIALDATASIZE/4]` and copied with
  `memcpy(map.pData, Matrices, 16*2*4)` (`RenderLayer.cpp:66`). So `sizeof(D3DXMATRIX)` **must be exactly
  64** and `sizeof(D3DXVECTOR4)` exactly 16 — no alignment padding on any target ABI. Prefer
  `static_assert(sizeof(D3DXMATRIX) == 64, ...)`.
* **`D3DXFLOAT16` must be `WORD`-sized** because of `CphxSplineKey` (§4.1).
* **`D3DXMatrixTransformation` argument order is a common porting bug** — note it is
  `(out, scalingCentre, scalingRotation, scaling, rotationCentre, rotation, translation)`, and the code
  passes `&D3DXVECTOR3(...)` / `&D3DXQUATERNION(...)` as **addresses of temporaries**, which is legal
  in C++ but means the implementation must not retain the pointers.
* **`Project.cpp:911`** `D3DXVECTOR3( filtertransform ) + *(D3DXVECTOR3*)one` — mixes a
  `D3DXFLOAT16*`→`D3DXVECTOR3` conversion with an alias-cast of a `static float one[] = {1,1,1}` as a
  `D3DXVECTOR3*` (`Project.cpp:100`). Both must work; `D3DXVECTOR3(const D3DXFLOAT16*)` reads 3 halves.
* **`Scene.cpp:551`** transposes `phxViewMatrix` into a local `mx` — used for the unproject math
  (`Scene.cpp:551-560`), not uploaded. `Timeline.cpp:160-161` copies the current view/proj into
  `phxPrevFrameViewMatrix`/`phxPrevFrameProjectionMatrix` which **are** uploaded (into the trailing
  128 bytes of the scene CB, `Timeline.cpp:114-115`-ish, and `Scene.cpp:173-174`).
* **No DirectXMath usage outside `DX11Texture.cpp:273-274,406`** — the rest of the codebase uses D3DX10
  vector/matrix types exclusively, so a single self-contained portable `D3DX10Math.h` replacement is
  sufficient (plus a tiny `DirectXPackedVector` half-conversion shim).

---

## 5. Where the source must be touched (the minimum patch set)

The design goal is "compile the existing DX11 code unchanged". The verified exceptions are:

| # | file:line | change | why |
|---|---|---|---|
| 1 | `apEx/MinimalPlayer/WindowHandler.cpp:44` **and** `:46` | remove one initialiser from the `DXGI_SWAP_CHAIN_DESC` aggregate | §1.4.9 — 15 values for 14 slots |
| 2 | `apEx/MinimalPlayer/WindowHandler.cpp` (device creation) | nothing | `D3D11CreateDeviceAndSwapChain` is provided by the shim |
| 3 | `Bedrock/CoRE2/DDSTextureLoader.cpp` | keep as-is if the shim provides `CreateTexture{1,2,3}D` + `CreateShaderResourceView` + `GetFeatureLevel` + the `DXGI_FORMAT_*`/`D3D11_REQ_*` constants; otherwise replace with a native DDS→`VkImage` loader | §2.11.4 |
| 4 | `apEx/Phoenix/Mesh.cpp:1126`, `apEx/Phoenix/Texgen.cpp:226`, `apEx/Phoenix_Tool/Texgen_tool.cpp:1037` | replace the four `D3DX11*Texture*` calls | §1.6 — they are `d3dx11_43.dll` exports the shim cannot provide |
| 5 | `apEx/Phoenix/Float16.cpp`, `float16.h` | already 100 % commented out — leave | §4.1 |
| 6 | `Bedrock/CoRE2/DX11Shader.cpp:36-165`, `apEx/Phoenix/phxEngine.cpp:170-174` | the `LoadLibraryA("d3dcompiler_47.dll")` / `d3dx11_**.dll` enumeration must not be reached; provide the compile function directly | §2.9.1 |
| 7 | `apEx/MinimalPlayer/MinimalPlayer.cpp:74-82,100-111` | the fixed DLL list (`d3d11.dll`, `d3dx10_43.dll`, `d3dx11_43.dll`, …, `d3dcompiler_47.dll`) is `LoadLibrary`'d at startup and the player aborts if any fails (`MinimalPlayer.cpp:104-111`) — must be made non-fatal or the list pruned | §6 R6 |
| 8 | `Bedrock/CoRE2/Core2_Config.h:46` | `#include <d3d11.h>` resolves to the shim instead of the SDK header — an include-path change, not a source edit | §3.3(8) |

Everything else in scope should compile against the shim unmodified.

### 5.1 D3D11 object → Vulkan object mapping (implementation cheat-sheet)

| D3D11 | Vulkan | notes / gotchas |
|---|---|---|
| `ID3D11Device` | `VkDevice` + `VkPhysicalDevice` + a per-device allocator/queue/pipeline-cache bundle | one global; `QueryInterface(__uuidof(ID3D11Debug))` should return `E_NOINTERFACE` |
| `ID3D11DeviceContext` | deferred command recording: one `VkCommandBuffer` per frame (single-threaded use — verified: no worker threads issue D3D11 calls) | `ClearState`/`Flush` → no-op / `vkQueueSubmit` flush |
| `IDXGISwapChain` | `VkSwapchainKHR` + one `VkImageView` per image + a `GetBuffer(0,...)` that returns the *current* back-buffer as an `ID3D11Texture2D` wrapper | `Present(1,0)`→`vkQueuePresentKHR` after a fence; `BufferCount=1` must be widened to 2–3 |
| `ID3D11Texture2D` | `VkImage` + `VkImageView`(s) + `VkDeviceMemory` (`VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`) | record `(format, usage, sampleCount, mipLevels)` on the wrapper; `D3D11_USAGE_STAGING`/`CPU_ACCESS_*` → host-visible `VkBuffer` (or `VkImage` with `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`) |
| `ID3D11Texture3D` | `VkImage` with `VK_IMAGE_TYPE_3D` | only needed if `PHX_VOLUMETRIC_RENDERTARGETS` is enabled |
| `ID3D11Buffer` | `VkBuffer` + `VkDeviceMemory` | CB `→ UNIFORM_BUFFER`; vertex `→ VERTEX_BUFFER`; index `→ INDEX_BUFFER`; structured `→ STORAGE_BUFFER`; dynamic/`CPU_ACCESS_WRITE` → host-visible + coherent |
| `ID3D11Buffer` (`D3D11_BIND_CONSTANT_BUFFER` + `DYNAMIC`) | slot in a per-frame host-visible ring + `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` | see R7; slot stride must be `max(16, minUniformBufferOffsetAlignment)`-aligned and `>= ` the SPIR-V block size |
| `ID3D11ShaderResourceView` (`TEXTURE2D`/`TEXTURE3D`) | `VkImageView` + `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` | typeless depth SRV needs a *second* view with a compatible format + `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (R3) |
| `ID3D11ShaderResourceView` (`BUFFER`, structured) | `VkBufferView` + `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | different descriptor type in the same `t` slot namespace → R4 |
| `ID3D11RenderTargetView` | `VkImageView` + `VkFramebuffer` (+ `VkRenderPass`/`VkPipelineRenderingCreateInfo`) | `RTV_DIMENSION_TEXTURE2DMS` on a 1-sample texture must be normalised (R10) |
| `ID3D11DepthStencilView` | `VkImageView` (depth aspect) | |
| `ID3D11UnorderedAccessView` | `VkImageView` + `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` | volumetric only |
| `ID3D11SamplerState` | `VkSampler` | `MinLOD`/`MaxLOD`/`mipLodBias` must be clamped to device limits; `ANISOTROPIC` needs `VkPhysicalDeviceFeatures::samplerAnisotropy` |
| `ID3D11BlendState` | `VkPipelineColorBlendStateCreateInfo` (+ `VkPipelineColorBlendAttachmentState`) | per-RT attachments; `IndependentBlend` → attachments may differ (always allowed in Vulkan) |
| `ID3D11DepthStencilState` | `VkPipelineDepthStencilStateCreateInfo` | `StencilEnable=false` everywhere, so stencil state can be left default |
| `ID3D11RasterizerState` | `VkPipelineRasterizationStateCreateInfo` | `FillMode=WIREFRAME`→`VK_POLYGON_MODE_LINE` (`fillModeNonSolid`); `DepthClipEnable=false`→`depthClipEnable=VK_FALSE` (core field); `AntialiasedLineEnable` has **no** Vulkan equivalent (drop) |
| `ID3D11InputLayout` | the pair (`D3D11_INPUT_ELEMENT_DESC[]`, associated VS module) feeding `VkPipelineVertexInputStateCreateInfo` | semantic→`location` must come from the SPIR-V `OpEntryPoint` interface (glslang assigns by declaration order), matched by `(SemanticName, SemanticIndex)` |
| `ID3D11VertexShader`/`PixelShader`/`GeometryShader` | `VkShaderModule` with `VK_SHADER_STAGE_*_BIT` | SPIR-V produced by the `D3DCompile` replacement; the "blob" is SPIR-V, and `GetHandle()`/`GetBinary()` return the same pointer |
| `ID3D11Query` (`OCCLUSION`) | `VkQueryPool` (`VK_QUERY_TYPE_OCCLUSION`) + host-visible resolve buffer | `GetData` is used as a blocking call → resolve + fence wait inside `GetData` |
| `ID3D10Blob` / `ID3DBlob` | plain refcounted heap buffer (`GetBufferPointer`/`GetBufferSize`/`Release`) | make both names the *same* type |
| `D3D11_MAP_WRITE_DISCARD` on a VkBuffer | return a pointer into the ring slot; `Unmap` records the slot | never `vkMapMemory` per call |
| `UpdateSubresource(sub, 0, NULL, ...)` | `vkCmdUpdateBuffer` or a staging `vkCmdCopyBuffer` | `DepthPitch == 0` must be accepted (`DX11Texture.cpp:221`) |
| `CopyResource` | `vkCmdCopyImage` / `vkCmdCopyBuffer` (`TRANSFER_SRC|DST`) | used for RT feedback-loop avoidance (`Timeline.cpp:71`) and staging (`DX11Texture.cpp:987,1014`) |
| `ResolveSubresource` | `vkCmdResolveImage` | `DX11Texture.cpp:967` |
| `GenerateMips(srv)` | per-mip `vkCmdBlitImage` chain or a downsample pipeline | R8; needs per-subresource layout tracking |
| `ClearRenderTargetView` / `ClearDepthStencilView` | `vkCmdClearColorImage` on a bound attachment, or `vkCmdClearAttachments` inside a render pass | `vkCmdClearColorImage` requires `TRANSFER_DST`-compatible layouts; `vkCmdClearAttachments` is the safe choice |
| `ClearUnorderedAccessViewFloat` | `vkCmdClearColorImage` on the storage image | volumetric only |
| `OMSetRenderTargets(n, rtvs, dsv)` | dynamic rendering (`VkRenderingInfo`) or a cached `VkRenderPass`+`VkFramebuffer` keyed by the attachment set | MRT with per-RT viewports needs `multiViewport` (R9) |
| `RSSetViewports(n, vps)` | `VkPipelineViewportStateCreateInfo` (static) or dynamic viewport state | `n>1` needs `multiViewport`; `MinDepth/MaxDepth` map to `minDepth/maxDepth` |
| occlusion `Begin`/`End`/`GetData` | `vkCmdBeginQuery`/`vkCmdEndQuery`/`vkGetQueryPoolResults` | |
| `_controlfp_s(&t, _RC_NEAR, _MCW_RC)` | `fesetround(FE_TONEAREST)` or no-op | R11 |

---

## 6. Risk register (ordered by expected cost)

| id | risk | evidence | mitigation |
|---|---|---|---|
| **R1** | **`cbuffer` packing mismatch.** If the HLSL→SPIR-V path lays out `cbuffer` members with `std140` instead of D3D rules, the scalar prologue `float t,aspect,xres,yres` lands at 0/16/32/48 instead of 0/4/8/12 and **every** post-process/texture-filter shader is wrong. | `Timeline.cpp:43-45,107`; `Texgen.cpp:123,151-156`; shader source in `Projects/*/*.apx` | **Validate first** (§7 test 1). glslang's HLSL frontend is expected to emit explicit `Offset` decorations; if not, pre-process `cbuffer` → `layout(std140) uniform` with explicit padding, or post-process the SPIR-V. |
| **R2** | **Runtime HLSL→SPIR-V must cover the real subset** — 175 `SampleLevel`, 90 `discard`, 81 `ddy`, 68 `ddx`, 45 `[maxvertexcount]`, 7 `SampleCmpLevelZero` on `SamplerComparisonState`, `Texture2D.Load(int3)`, `float3x3` literals, `fmod`, `static const` arrays, `POSITIONT`. | `.apx` mining (§2.9.4) | Build a compile-all-shaders harness over all 10 shipped projects as a CI gate; it will find unsupported constructs far earlier than a render test. |
| **R3** | **Typeless depth aliasing** (`R24G8_TYPELESS` + `D24_UNORM_S8_UINT` DSV + `R24_UNORM_X8_TYPELESS` SRV) has no Vulkan equivalent. The depth SRV is **always at t7** and is read by depth-dependent post effects (`Texture2D z:register(t7)`, `getLinearDepth`, DOF, SSAO). | `Project.cpp:94-96,221-228`; `DX11Texture.cpp:487,503,518`; `Timeline.cpp:85` | Create the image as `D24_UNORM_S8_UINT` with `DEPTH_STENCIL_ATTACHMENT|SAMPLED` and add a second sampled view with a compatible format; needs `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` + per-view format. Prefer `D32_SFLOAT`/`R32_SFLOAT` if `X8_D24_UNORM_PACK32` sampling is unsupported on the target. Validate device support at startup. |
| **R4** | **SRV slot namespace mixes sampled images and storage buffers** (tree data, `Mesh.cpp:1737-1760`). | `Mesh.cpp:1743,1755` | Two parallel descriptor arrays (sampled images + storage buffers) with the unused one bound to a null descriptor (`VK_EXT_robustness2`) or a dummy resource. Decide at `*SetShaderResources` time from the SRV's recorded kind. |
| **R5** | **Depth range / projection convention.** D3DX `...RH` matrices + `D3D11_COMPARISON_LESS` + `Clear(depth=1)` assume D3D's z∈[0,1] "near=0" with `LESS`. Vulkan's z∈[0,1] is compatible, **but** D3DX's RH perspective matrices place the near plane at z=0 with a reversed handedness relative to the Vulkan default NDC y-flip — the y-flip must be handled deliberately (either negate `VkViewport.height` and offset, or use `gl_Position.y = -gl_Position.y` in a preamble injected at compile time). Getting this wrong produces a vertically mirrored or depth-inverted image. | `Timeline.cpp:174,186`; `Project.cpp:96,94`; `Project.h`/`Scene.h` matrix globals | Add the y-flip as an injected `#define`/preamble in the shim's HLSL compile path (one central place), then validate with a known demo. |
| **R6** | **Startup DLL list.** `MinimalPlayer.cpp:74-82` lists `d3d11.dll`, `d3dx10_43.dll`, `d3dx11_43.dll`, …, `d3dcompiler_47.dll`; `:100-111` `LoadLibrary`s all seven and on failure shows a `MessageBoxA` (`:109`) and `return 0`s out of `WinMain` (`:110`). | `MinimalPlayer.cpp:74-82,100-111` | Prune/skip the D3D DLLs in the Vulkan build. |
| **R7** | **Per-draw dynamic CB upload.** `Map(WRITE_DISCARD)`+`memcpy` per instance/per pass (`RenderLayer.cpp:64-68`, `Timeline.cpp:92-116`) maps to either one `vkCmdUpdateBuffer` per draw or a dynamic-offset ring. A naive `vkCmdUpdateBuffer` per draw on a 384-byte buffer is legal but adds a full pipeline barrier per draw in some drivers. | `RenderLayer.cpp:64-68` | Implement `Map(WRITE_DISCARD)` as slot allocation from a per-frame ring buffer of the same size, returning a pointer into host-visible memory; `Unmap` records `(offset, size)`; the CB bind uses `UNIFORM_BUFFER_DYNAMIC` with the recorded offset. Requires `minUniformBufferOffsetAlignment`-aligned slots. |
| **R8** | **`vkCmdBlitImage`-based `GenerateMips`** must be correct for `R16G16B16A16_FLOAT` and must not deadlock with the SRV usage in the same command buffer; 4 call sites run it per pass. | `RenderLayer.cpp:156`, `Timeline.cpp:130`, `Texgen.cpp:174`, `Scene.cpp:198` | Query `VK_FORMAT_FEATURE_BLIT_SRC/DST_BIT` + linear filter support at startup; fall back to a compute/draw downsample pipeline. Keep a per-image subresource-layout tracker. |
| **R9** | **MRT + per-RT viewports.** `TargetCount` up to 63, one viewport per RT, `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT = 8` sizing static arrays. | `RenderLayer.cpp:104-129`; `Project.cpp:331` | Require `multiViewport` with `maxViewports >= TargetCount`; otherwise emulate by rendering each RT in a separate pass (changes blend semantics) — prefer requiring the feature. |
| **R10** | **MSAA sample-count mismatch (`RTV_DIMENSION_TEXTURE2DMS` on a 1-sample texture)** is used pervasively and must be silently normalised, not rejected. | `Project.cpp:205`, `DX11Texture.cpp:96`, `RenderTargets.cpp:46` | Normalise `*_TEXTURE2DMS` → `*_TEXTURE2D` when `SampleDesc.Count <= 1` in every `Create*View`. |
| **R11** | **`_controlfp_s(&tmp, _RC_NEAR, _MCW_RC)`** is set around every compile and restored after (`DX11Shader.cpp:268-270,303-306` etc.) — with `#ifndef _WIN64` guards. Non-Windows needs `fesetround(FE_TONEAREST)` equivalents or a no-op; a no-op is acceptable because the reason (fxc FP determinism) no longer applies, but the *calls* must be behind a macro. | `DX11Shader.cpp:268-270,303-306,410-412,451-454,…` | Provide `_controlfp_s`, `_RC_NEAR`, `_MCW_RC` macros/inlines. |
| **R12** | **Compiler-specific idioms**: `__uuidof`, `__stdcall`, `__fastcall`, `_com_error`, `__forceinline`, `#pragma comment(lib,...)`, `#pragma push_macro("new")`, `_controlfp_s`, `LPCSTR`/`LPVOID`/`ZeroMemory`/`SafeRelease`. | throughout; `phxSpline.h:2-5` (`#pragma push_macro("new")` around `D3DX10Math.h` suggests an overloaded global `operator new`), `Mesh.cpp:4` (`../LibCTiny/libcminimal.h`) | The shim header set must supply these (or a compat header). `LibCTiny` already provides a minimal CRT for the Windows build (`apEx/MinimalPlayer/cruntime.h`, `libcminimal.h`) — porting that is a separate workstream and is **not** covered by this spec. `UNCERTAIN`: whether `LibCTiny` is Windows-only. |

---

## 7. Verification plan (do these in order; each gates the next)

1. **cbuffer packing test (gates everything).** Compile the exact `cbuffer b{float t,aspect,xres,yres;}`
   from `Projects/Clean Slate/cleanslate.apx` to SPIR-V and disassemble. Assert
   `OpMemberDecorate %type_UniformBlock 0 Offset 0`, `1 Offset 4`, `2 Offset 8`, `3 Offset 12`, and
   `OpDecorate %type_UniformBlock Block` + `ArrayStride`/buffer size ≤ 16. Then the scene cbuffer with
   `OpMemberDecorate … 4 Offset 16` (`viewmat`) and `… 6 Offset 144` (`campos`).
2. **Compile-all-shaders harness.** Extract every shader from all 10 `Projects/*/*.apx`
   (`Stream_Shaders` → `skiptext`/`copyshader`, per `Project.cpp:246-253,340-359,527-549`), compile with
   the shim's `D3DCompile` for `{vs,ps,gs}_5_0` × entries `{v,p,g}` / `{p}` / `{a,c,s,u}`, and report
   failures. Zero failures is the acceptance bar for R2.
3. **Interface-layout static asserts.** `static_assert` on `sizeof`/`offsetof` for every struct in §1.4,
   plus `sizeof(D3DXMATRIX)==64`, `sizeof(D3DXVECTOR4)==16`, `sizeof(D3DXFLOAT16)==2`,
   `sizeof(D3D11_BLEND_DESC)`, `offsetof(D3D11_DEPTH_STENCIL_VIEW_DESC, Flags)`.
4. **Compile-cleanliness check.** Build `Bedrock/CoRE2` (DX11 files only, DX9 files excluded via
   `Core2_Config.h:19`) + `apEx/Phoenix` + `apEx/MinimalPlayer` against the shim and diff the set of
   errors against the §5 patch list. Any new error is a missing declaration.
5. **Single-frame correctness.** `Projects/Clean Slate/cleanslate.apx` is the smallest test case
   (13 MB); render frame N and compare against a Windows/D3D11 reference capture of the same frame
   (tolerance ~1 LSB for `R16G16B16A16_FLOAT` intermediate targets).
6. **Depth-dependent effect check** (SSAO/DOF/`getLinearDepth`) specifically exercises R3 and R5.
7. **Orientation check** — any demo with text or a known-asymmetric frame tests R5.

---

## 8. Appendix — generated inventories

The following appendices are generated directly from the source by the analysis scripts, with
comment/string-literal blanking that preserves line numbers. They are the ground truth for TASK 1.

* **Appendix A** — every `receiver->method(` call on a D3D11/DXGI-typed receiver, grouped by interface,
  with every `file:line`.
* **Appendix B** — every `D3D11_*`, `D3D10_*`, `D3D_*` and `DXGI_*` symbol referenced, with every
  `file:line` and the source line text.

---

# Appendix A — full `receiver->method(file:line)` inventory

Generated from the four scope directories. Receiver resolution tiers: (1) declaration in the same
`.cpp`; (2) declaration in the header with the same basename; (3) the explicit 14-name allow-list of
cross-file `phx*` globals plus `SwapChain` and `ltc1`/`ltc2` (see §1.1). Comment and string-literal
content is blanked **without** changing line numbers, so all citations are exact. A slash-joined group
name means the receiver name is declared with several D3D11 types (e.g. `State` is a
`BlendState`/`DepthStencilState`/`RasterizerState`/`SamplerState` member across `DX11RenderState.{h,cpp}`).

### `ID3D10Blob`


**`GetBufferPointer`** ×27

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:278` | `LOG(LOG_ERROR, _T("[core] VertexShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:282` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:290` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:427` | `LOG(LOG_ERROR, _T("[core] PixelShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:431` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:439` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:576` | `LOG(LOG_ERROR, _T("[core] GeometryShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:580` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:585` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:722` | `LOG(LOG_ERROR, _T("[core] DomainShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:726` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:731` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:868` | `LOG(LOG_ERROR, _T("[core] HullShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:872` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:877` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:1015` | `LOG(LOG_ERROR, _T("[core] ComputeShader compilation error: %s"), CString((char*)Error->GetBufferPointer()).GetPointer());` |
| `Bedrock/CoRE2/DX11Shader.cpp:1019` | `if (Err) *Err = Error ? CString((char*)Error->GetBufferPointer()) : _T("");` |
| `Bedrock/CoRE2/DX11Shader.cpp:1024` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `apEx/Phoenix/Mesh.cpp:1158` | `unsigned char *Data = (unsigned char*)Blob->GetBufferPointer();` |
| `apEx/Phoenix/Project.cpp:368` | `ErrorText = (char*)Error->GetBufferPointer();` |
| `apEx/Phoenix/Project.cpp:372` | `phxDev->CreatePixelShader( PS->GetBufferPointer(), PS->GetBufferSize(), NULL, &TextureFilters[ x ].PixelShader );` |
| `apEx/Phoenix/Project.cpp:544` | `ErrorText = (char*)Error->GetBufferPointer();` |
| `apEx/Phoenix/phxEngine.cpp:187` | `phxDev->CreatePixelShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &RenderPixelShader );` |
| `apEx/Phoenix/phxEngine.cpp:190` | `phxDev->CreateInputLayout( vxdecl2, 6, VS->GetBufferPointer(), VS->GetBufferSize(), &RenderVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:194` | `phxDev->CreateInputLayout( vxdecl3, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &ParticleVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:198` | `phxDev->CreateInputLayout( vxdecl, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &TexgenVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:199` | `phxDev->CreateVertexShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &TexgenVertexShader );` |

**`GetBufferSize`** ×12

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:290` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:439` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:585` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:731` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:877` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `Bedrock/CoRE2/DX11Shader.cpp:1024` | `Success = Create(PS->GetBufferPointer(), (TS32)PS->GetBufferSize());` |
| `apEx/Phoenix/Project.cpp:372` | `phxDev->CreatePixelShader( PS->GetBufferPointer(), PS->GetBufferSize(), NULL, &TextureFilters[ x ].PixelShader );` |
| `apEx/Phoenix/phxEngine.cpp:187` | `phxDev->CreatePixelShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &RenderPixelShader );` |
| `apEx/Phoenix/phxEngine.cpp:190` | `phxDev->CreateInputLayout( vxdecl2, 6, VS->GetBufferPointer(), VS->GetBufferSize(), &RenderVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:194` | `phxDev->CreateInputLayout( vxdecl3, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &ParticleVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:198` | `phxDev->CreateInputLayout( vxdecl, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &TexgenVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:199` | `phxDev->CreateVertexShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &TexgenVertexShader );` |

**`Release`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:301` | `PS->Release();` |
| `Bedrock/CoRE2/DX11Shader.cpp:450` | `PS->Release();` |
| `Bedrock/CoRE2/DX11Shader.cpp:596` | `PS->Release();` |
| `Bedrock/CoRE2/DX11Shader.cpp:742` | `PS->Release();` |
| `Bedrock/CoRE2/DX11Shader.cpp:888` | `PS->Release();` |
| `Bedrock/CoRE2/DX11Shader.cpp:1035` | `PS->Release();` |
| `apEx/Phoenix/Mesh.cpp:1196` | `Blob->Release();` |

### `ID3D11BlendState/ID3D11DepthStencilState/ID3D11RasterizerState/ID3D11SamplerState`


**`Release`** ×8

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:16` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:22` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:74` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:80` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:125` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:131` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:179` | `if (State) State->Release();` |
| `Bedrock/CoRE2/DX11RenderState.cpp:185` | `if (State) State->Release();` |

### `ID3D11Buffer`


**`Release`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:15` | `if (Buffer) Buffer->Release();` |
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:28` | `if (Buffer) Buffer->Release();` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:22` | `if (IndexBufferHandle) IndexBufferHandle->Release();` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:23` | `if (VertexBufferHandle) VertexBufferHandle->Release();` |
| `apEx/Phoenix_Tool/TexgenPage.cpp:352` | `TexgenBufferPS->Release();` |

### `ID3D11ComputeShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:971` | `ComputeShaderHandle->Release();` |

### `ID3D11Debug`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:54` | `dbg->Release();` |

**`ReportLiveDeviceObjects`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:53` | `dbg->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);` |

### `ID3D11DepthStencilView`


**`Release`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:31` | `if (DepthBufferView) DepthBufferView->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:117` | `if (DepthBufferView) DepthBufferView->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:282` | `if (DepthBufferView) DepthBufferView->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:29` | `if ( DepthView ) DepthView->Release();` |

### `ID3D11Device`


**`CheckFormatSupport`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:947` | `hr = d3dDevice->CheckFormatSupport( fmt, &support );` |

**`CreateBlendState`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:42` | `HRESULT res = Dev->CreateBlendState(&desc, &State);` |
| `apEx/Phoenix/Project.cpp:1867` | `phxDev->CreateBlendState( &BlendDesc, &p->BlendState );` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1283` | `phxDev->CreateBlendState( &BlendDesc, &p->BlendState );` |

**`CreateBuffer`** ×13

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:39` | `if (Dev->CreateBuffer(&desc, NULL, &Buffer) != S_OK)` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:48` | `HRESULT res = Dev->CreateBuffer(&bd, NULL, &IndexBufferHandle);` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:53` | `HRESULT res = Dev->CreateBuffer(&bd, &vxData, &VertexBufferHandle);` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:78` | `HRESULT res = Dev->CreateBuffer(&bd, NULL, &VertexBufferHandle);` |
| `apEx/Phoenix/Mesh.cpp:478` | `phxDev->CreateBuffer( &bd, &bufData, &VxBuffer );` |
| `apEx/Phoenix/Mesh.cpp:491` | `phxDev->CreateBuffer( &bd, &bufData, &TriIndexBuffer );` |
| `apEx/Phoenix/Mesh.cpp:504` | `phxDev->CreateBuffer( &bd, &bufData, &WireIndexBuffer );` |
| `apEx/Phoenix/Mesh.cpp:1749` | `phxDev->CreateBuffer( &treeDataBuffer, &subData, buffer );` |
| `apEx/Phoenix/Project.cpp:1178` | `phxDev->CreateBuffer( &bd, NULL, &p->VertexBuffer );` |
| `apEx/Phoenix/phxEngine.cpp:177` | `phxDev->CreateBuffer( &desc, NULL, &TexgenBufferPS );` |
| `apEx/Phoenix/phxEngine.cpp:180` | `phxDev->CreateBuffer( &desc, NULL, &SceneDataBuffer );` |
| `apEx/Phoenix/phxEngine.cpp:183` | `phxDev->CreateBuffer( &desc, NULL, &ObjectMatrixBuffer );` |
| `apEx/Phoenix/phxEngine.cpp:202` | `phxDev->CreateBuffer( &bd, &vxData, &TexgenVertexBuffer );` |

**`CreateComputeShader`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:956` | `HRESULT res = Dev->CreateComputeShader(Binary, Length, NULL, &ComputeShaderHandle);` |

**`CreateDepthStencilState`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:93` | `HRESULT res = Dev->CreateDepthStencilState(&desc, &State);` |
| `apEx/Phoenix/Project.cpp:1869` | `phxDev->CreateDepthStencilState( &DepthStencilDesc, &p->DepthStencilState );` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1285` | `phxDev->CreateDepthStencilState( &DepthStencilDesc, &p->DepthStencilState );` |

**`CreateDepthStencilView`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:157` | `res = Device->CreateDepthStencilView(DepthBuffer, &depthStencilViewDesc, &DepthBufferView);` |
| `Bedrock/CoRE2/DX11Texture.cpp:508` | `res = Dev->CreateDepthStencilView( TextureHandle, &deptdesc, &DepthView );` |
| `apEx/Phoenix/Project.cpp:227` | `phxDev->CreateDepthStencilView( DepthBuffer, &depthStencilViewDesc, &phxDepthBufferView );` |

**`CreateDomainShader`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:663` | `HRESULT res = Dev->CreateDomainShader(Binary, Length, NULL, &DomainShaderHandle);` |

**`CreateGeometryShader`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:517` | `HRESULT res = Dev->CreateGeometryShader(Binary, Length, NULL, &GeometryShaderHandle);` |
| `apEx/Phoenix/Project.cpp:580` | `if ( *Blobs_ ) phxDev->CreateGeometryShader( ( *Blobs_ )->GetBufferPointer(), ( *Blobs_ )->GetBufferSize(), NULL, &p->GS ); Blobs_++;` |

**`CreateHullShader`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:809` | `HRESULT res = Dev->CreateHullShader(Binary, Length, NULL, &HullShaderHandle);` |

**`CreateInputLayout`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexFormat.cpp:131` | `HRESULT res = Dev->CreateInputLayout(vxdecl, Attributes.NumItems(), vs->GetBinary(), vs->GetBinaryLength(), &VertexFormatHandle);` |
| `apEx/Phoenix/phxEngine.cpp:190` | `phxDev->CreateInputLayout( vxdecl2, 6, VS->GetBufferPointer(), VS->GetBufferSize(), &RenderVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:194` | `phxDev->CreateInputLayout( vxdecl3, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &ParticleVertexFormat );` |
| `apEx/Phoenix/phxEngine.cpp:198` | `phxDev->CreateInputLayout( vxdecl, 2, VS->GetBufferPointer(), VS->GetBufferSize(), &TexgenVertexFormat );` |

**`CreatePixelShader`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:368` | `HRESULT res = Dev->CreatePixelShader(Binary, Length, NULL, &PixelShaderHandle);` |
| `apEx/Phoenix/Project.cpp:372` | `phxDev->CreatePixelShader( PS->GetBufferPointer(), PS->GetBufferSize(), NULL, &TextureFilters[ x ].PixelShader );` |
| `apEx/Phoenix/Project.cpp:568` | `if ( *Blobs_ ) phxDev->CreatePixelShader( ( *Blobs_ )->GetBufferPointer(), ( *Blobs_ )->GetBufferSize(), NULL, &p->PS ); Blobs_++;` |
| `apEx/Phoenix/phxEngine.cpp:187` | `phxDev->CreatePixelShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &RenderPixelShader );` |

**`CreateQuery`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:229` | `Device->CreateQuery( &queryDesc, &OcclusionQuery );` |

**`CreateRasterizerState`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:147` | `HRESULT res = Dev->CreateRasterizerState(&desc, &State);` |
| `apEx/Phoenix/Project.cpp:1868` | `phxDev->CreateRasterizerState( &RasterDesc, &p->RasterizerState );` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1284` | `phxDev->CreateRasterizerState( &RasterDesc, &p->RasterizerState );` |

**`CreateRenderTargetView`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:92` | `res = Device->CreateRenderTargetView(bb, NULL, &BackBufferView);` |
| `Bedrock/CoRE2/DX11Texture.cpp:95` | `res = Dev->CreateRenderTargetView( TextureHandle, &rt, &RTView );` |
| `apEx/MinimalPlayer/WindowHandler.cpp:98` | `phxDev->CreateRenderTargetView( bb, NULL, &phxBackBufferView );` |
| `apEx/Phoenix/Project.cpp:209` | `phxDev->CreateRenderTargetView( target.Texture, &rt, &target.RTView );` |
| `apEx/Phoenix/Texgen.cpp:53` | `phxDev->CreateRenderTargetView( Texture, &texgenrt, &RTView );` |
| `apEx/Phoenix/phxEngine.cpp:280` | `phxDev->CreateRenderTargetView( phxVoxelForceMSAATexture, &rt, &phxVoxelForceMSAARTView );` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:47` | `result = phxDev->CreateRenderTargetView( Texture, &rt, &RTView );` |

**`CreateSamplerState`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:206` | `HRESULT res = Dev->CreateSamplerState(&desc, &State);` |
| `apEx/Phoenix/phxEngine.cpp:204` | `phxDev->CreateSamplerState( &smpData, &TexgenSampler );` |
| `apEx/Phoenix/phxEngine.cpp:206` | `phxDev->CreateSamplerState( &smpData, &TexgenSampler_NoWrap );` |
| `apEx/Phoenix/phxEngine.cpp:210` | `phxDev->CreateSamplerState( &smpData, &TexgenSampler_ShadowCompare );` |

**`CreateShaderResourceView`** ×18

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:711` | `hr = d3dDevice->CreateShaderResourceView(tex, &SRVDesc, textureView);` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:786` | `hr = d3dDevice->CreateShaderResourceView(tex, &SRVDesc, textureView);` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:833` | `hr = d3dDevice->CreateShaderResourceView(tex, &SRVDesc, textureView);` |
| `Bedrock/CoRE2/DX11Texture.cpp:81` | `res = Dev->CreateShaderResourceView( TextureHandle, NULL, &View );` |
| `Bedrock/CoRE2/DX11Texture.cpp:163` | `HRESULT res = Dev->CreateShaderResourceView( TextureHandle, NULL, &View );` |
| `Bedrock/CoRE2/DX11Texture.cpp:256` | `res = Dev->CreateShaderResourceView( d2, NULL, &v2 );` |
| `Bedrock/CoRE2/DX11Texture.cpp:522` | `res = Dev->CreateShaderResourceView( TextureHandle, &resdesc, &View );` |
| `apEx/Phoenix/Mesh.cpp:1759` | `phxDev->CreateShaderResourceView( *buffer, &resourceViewDesc, bufferView );` |
| `apEx/Phoenix/Project.cpp:208` | `phxDev->CreateShaderResourceView( target.Texture, NULL, &target.View );` |
| `apEx/Phoenix/Project.cpp:228` | `phxDev->CreateShaderResourceView( DepthBuffer, &resdesc, &phxDepthBufferShaderView );` |
| `apEx/Phoenix/Scene.cpp:765` | `phxDev->CreateShaderResourceView( SplineTexture, NULL, &SplineTextureView );` |
| `apEx/Phoenix/Texgen.cpp:44` | `phxDev->CreateShaderResourceView( Texture, NULL, &View );` |
| `apEx/Phoenix/Texgen.cpp:229` | `phxDev->CreateShaderResourceView( out->Texture, NULL, &out->View );` |
| `apEx/Phoenix/Texgen.cpp:342` | `res = phxDev->CreateShaderResourceView( out->Texture, NULL, &out->View );` |
| `apEx/Phoenix/phxEngine.cpp:251` | `phxDev->CreateShaderResourceView( phxTexture3D, &resourceViewDesc, &phxTexture3DResourceView );` |
| `apEx/Phoenix/phxEngine.cpp:319` | `phxDev->CreateShaderResourceView( ltc1, &ltcview16, &ltc1view );` |
| `apEx/Phoenix/phxEngine.cpp:331` | `phxDev->CreateShaderResourceView( ltc2, &ltcview8, &ltc2view );` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:35` | `result = phxDev->CreateShaderResourceView( Texture, NULL, &View );` |

**`CreateTexture1D`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:689` | `hr = d3dDevice->CreateTexture1D(&desc, initData, &tex);` |

**`CreateTexture2D`** ×17

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:748` | `hr = d3dDevice->CreateTexture2D(&desc, initData, &tex);` |
| `Bedrock/CoRE2/DX11Device.cpp:141` | `res = Device->CreateTexture2D(&depthBufferDesc, NULL, &DepthBuffer);` |
| `Bedrock/CoRE2/DX11Texture.cpp:73` | `HRESULT res = Dev->CreateTexture2D( &tex, Data ? &data : NULL, &TextureHandle );` |
| `Bedrock/CoRE2/DX11Texture.cpp:243` | `res = Dev->CreateTexture2D( &desc, NULL, &d2 );` |
| `Bedrock/CoRE2/DX11Texture.cpp:492` | `HRESULT res = Dev->CreateTexture2D( &tex, NULL, &TextureHandle );` |
| `Bedrock/CoRE2/DX11Texture.cpp:934` | `hr = d3dDevice->CreateTexture2D( &desc, 0, &pTemp );` |
| `Bedrock/CoRE2/DX11Texture.cpp:976` | `hr = d3dDevice->CreateTexture2D( &desc, 0, &pStaging );` |
| `Bedrock/CoRE2/DX11Texture.cpp:1004` | `hr = d3dDevice->CreateTexture2D( &desc, 0, &pStaging );` |
| `apEx/Phoenix/Project.cpp:207` | `phxDev->CreateTexture2D( &tex, NULL, &target.Texture );` |
| `apEx/Phoenix/Project.cpp:226` | `phxDev->CreateTexture2D( &dBD, NULL, &DepthBuffer );` |
| `apEx/Phoenix/Scene.cpp:764` | `phxDev->CreateTexture2D( &tex, &data, &SplineTexture );` |
| `apEx/Phoenix/Texgen.cpp:35` | `phxDev->CreateTexture2D( &tex, NULL, &Texture );` |
| `apEx/Phoenix/Texgen.cpp:341` | `HRESULT res = phxDev->CreateTexture2D( &tex, &data, &out->Texture );` |
| `apEx/Phoenix/phxEngine.cpp:279` | `phxDev->CreateTexture2D( &tex, NULL, &phxVoxelForceMSAATexture );` |
| `apEx/Phoenix/phxEngine.cpp:318` | `phxDev->CreateTexture2D( &ltctex16, &subData, &ltc1 );` |
| `apEx/Phoenix/phxEngine.cpp:330` | `phxDev->CreateTexture2D( &ltctex8, &subData, &ltc2 );` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:28` | `HRESULT result = phxDev->CreateTexture2D( &tex, NULL, &Texture );` |

**`CreateTexture3D`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:821` | `hr = d3dDevice->CreateTexture3D(&desc, initData, &tex);` |
| `apEx/Phoenix/phxEngine.cpp:242` | `phxDev->CreateTexture3D( &textureDesc, 0, &phxTexture3D );` |

**`CreateUnorderedAccessView`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:274` | `phxDev->CreateUnorderedAccessView( phxTexture3D, &uav_desc, &phxTexture3DUAV );` |

**`CreateVertexShader`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:219` | `HRESULT res = Dev->CreateVertexShader(Binary, Length, NULL, &VertexShaderHandle);` |
| `apEx/Phoenix/Project.cpp:556` | `if ( *Blobs_ ) phxDev->CreateVertexShader( ( *Blobs_ )->GetBufferPointer(), ( *Blobs_ )->GetBufferSize(), NULL, &p->VS ); Blobs_++;` |
| `apEx/Phoenix/phxEngine.cpp:199` | `phxDev->CreateVertexShader( VS->GetBufferPointer(), VS->GetBufferSize(), NULL, &TexgenVertexShader );` |

**`GetDeviceRemovedReason`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Project.cpp:1185` | `DEBUGLOG( "Error: Device removed. Reason: %x", phxDev->GetDeviceRemovedReason() );` |

**`GetFeatureLevel`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1022` | `switch (d3dDevice->GetFeatureLevel())` |

**`QueryInterface`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:49` | `Device->QueryInterface(__uuidof(ID3D11Debug), reinterpret_cast<void**>(&dbg));` |

**`Release`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:57` | `if (Device) Device->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1161` | `d3dDevice->Release();` |

### `ID3D11DeviceContext`


**`Begin`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:978` | `DeviceContext->Begin( OcclusionQuery );` |

**`CSSetShader`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:978` | `DeviceContext->CSSetShader(ComputeShaderHandle, NULL, 0);` |

**`ClearDepthStencilView`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:737` | `DeviceContext->ClearDepthStencilView(DepthBufferView, D3D11_CLEAR_DEPTH \| D3D11_CLEAR_STENCIL, Depth, Stencil);` |
| `apEx/Phoenix/RenderLayer.cpp:132` | `phxContext->ClearDepthStencilView( phxDepthBufferView, ( D3D10_CLEAR_DEPTH * !OmitDepthBuffer ) \| D3D10_CLEAR_STENCIL, 1, 0 );` |
| `apEx/Phoenix/Timeline.cpp:337` | `phxContext->ClearDepthStencilView( phxDepthBufferView, D3D10_CLEAR_DEPTH \| D3D10_CLEAR_STENCIL, 1, 0 );` |

**`ClearRenderTargetView`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:734` | `DeviceContext->ClearRenderTargetView(BackBufferView, col);` |
| `apEx/Phoenix/RenderLayer.cpp:118` | `phxContext->ClearRenderTargetView( Targets[ x ]->RTView, col );` |
| `apEx/Phoenix/Timeline.cpp:333` | `phxContext->ClearRenderTargetView( phxBackBufferView, (float*)rv );` |
| `apEx/Phoenix/Timeline.cpp:336` | `phxContext->ClearRenderTargetView( RenderTargets[ x ]->RTView, (float*)rv );` |

**`ClearState`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:41` | `DeviceContext->ClearState();` |

**`ClearUnorderedAccessViewFloat`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:137` | `phxContext->ClearUnorderedAccessViewFloat( phxTexture3DUAV, col );` |

**`CopyResource`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:264` | `DeviceContext->CopyResource( d2, TextureHandle );` |
| `Bedrock/CoRE2/DX11Texture.cpp:987` | `pContext->CopyResource( pStaging, pTemp );` |
| `Bedrock/CoRE2/DX11Texture.cpp:1014` | `pContext->CopyResource( pStaging, pSource );` |
| `apEx/Phoenix/Timeline.cpp:71` | `phxContext->CopyResource( phxInternalRenderTarget->Texture, MaterialState[ x ]->RenderTarget->Texture );` |

**`DSSetShader`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:673` | `DeviceContext->DSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Device.cpp:683` | `DeviceContext->DSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Shader.cpp:685` | `DeviceContext->DSSetShader(DomainShaderHandle, NULL, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:58` | `phxContext->DSSetShader( DS, NULL, 0 );` |
| `apEx/Phoenix/Texgen.cpp:117` | `phxContext->DSSetShader( NULL, NULL, 0 );` |

**`Draw`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:786` | `DeviceContext->Draw(Count * 2, 0);` |
| `Bedrock/CoRE2/DX11Device.cpp:802` | `DeviceContext->Draw(Count * 3, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:99` | `phxContext->Draw( TriIndexCount, 0 );` |
| `apEx/Phoenix/Texgen.cpp:172` | `phxContext->Draw( 6, 0 );` |
| `apEx/Phoenix/Timeline.cpp:123` | `phxContext->Draw( 6, 0 );` |
| `apEx/Phoenix/Timeline.cpp:373` | `phxContext->Draw( 6, 0 );` |

**`DrawIndexed`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:778` | `DeviceContext->DrawIndexed(Count * 3, 0, 0);` |
| `Bedrock/CoRE2/DX11Device.cpp:794` | `DeviceContext->DrawIndexed(Count * 2, 0, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:95` | `phxContext->DrawIndexed( Wireframe ? WireIndexCount : TriIndexCount, 0, 0 );` |

**`End`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:985` | `DeviceContext->End( OcclusionQuery );` |

**`Flush`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:42` | `DeviceContext->Flush();` |

**`GSSetConstantBuffers`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:837` | `DeviceContext->GSSetConstantBuffers(Slot, Count, (ID3D11Buffer**)buffers);` |
| `apEx/Phoenix/Scene.cpp:188` | `phxContext->GSSetConstantBuffers( y, 1, &Buffer );` |

**`GSSetSamplers`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:228` | `Context->GSSetSamplers(Smp - CORESMP_GS0, 1, &State);` |

**`GSSetShader`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:653` | `DeviceContext->GSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Shader.cpp:539` | `DeviceContext->GSSetShader(GeometryShaderHandle, NULL, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:56` | `phxContext->GSSetShader( GS, NULL, 0 );` |
| `apEx/Phoenix/Texgen.cpp:115` | `phxContext->GSSetShader( NULL, NULL, 0 );` |

**`GSSetShaderResources`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:610` | `DeviceContext->GSSetShaderResources(Sampler - CORESMP_GS0, 1, null);` |
| `Bedrock/CoRE2/DX11Texture.cpp:43` | `DeviceContext->GSSetShaderResources( smp - CORESMP_GS0, 1, &View );` |
| `apEx/Phoenix/RenderLayer.cpp:84` | `phxContext->GSSetShaderResources( 0, 8, Textures );` |

**`GenerateMips`** ×4

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:156` | `phxContext->GenerateMips( Targets[x]->View );` |
| `apEx/Phoenix/Scene.cpp:198` | `phxContext->GenerateMips( phxTexture3DResourceView );` |
| `apEx/Phoenix/Texgen.cpp:174` | `phxContext->GenerateMips( Target->View );` |
| `apEx/Phoenix/Timeline.cpp:130` | `phxContext->GenerateMips(Target->View);` |

**`GetData`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:988` | `while ( S_OK != DeviceContext->GetData( OcclusionQuery, &queryData, sizeof( UINT64 ), 0 ) )` |

**`GetDevice`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:1158` | `pContext->GetDevice( &d3dDevice );` |

**`HSSetShader`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:663` | `DeviceContext->HSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Shader.cpp:831` | `DeviceContext->HSSetShader(HullShaderHandle, NULL, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:57` | `phxContext->HSSetShader( HS, NULL, 0 );` |
| `apEx/Phoenix/Texgen.cpp:116` | `phxContext->HSSetShader( NULL, NULL, 0 );` |

**`IASetIndexBuffer`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:633` | `DeviceContext->IASetIndexBuffer(NULL, DXGI_FORMAT_R16_UINT, 0);` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:29` | `DeviceContext->IASetIndexBuffer(IndexBufferHandle, IndexSize == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:88` | `phxContext->IASetIndexBuffer( Wireframe ? WireBuffer : IndexBuffer, DXGI_FORMAT_R32_UINT, 0 );` |

**`IASetInputLayout`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:621` | `DeviceContext->IASetInputLayout(NULL);` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:28` | `DeviceContext->IASetInputLayout(VertexFormatHandle);` |
| `apEx/Phoenix/RenderLayer.cpp:38` | `phxContext->IASetInputLayout( RenderVertexFormat );` |
| `apEx/Phoenix/RenderLayer.cpp:42` | `phxContext->IASetInputLayout( ParticleVertexFormat );` |
| `apEx/Phoenix/Texgen.cpp:109` | `phxContext->IASetInputLayout( TexgenVertexFormat );` |

**`IASetPrimitiveTopology`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:776` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);` |
| `Bedrock/CoRE2/DX11Device.cpp:784` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);` |
| `Bedrock/CoRE2/DX11Device.cpp:792` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);` |
| `Bedrock/CoRE2/DX11Device.cpp:800` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);` |
| `apEx/Phoenix/RenderLayer.cpp:48` | `phxContext->IASetPrimitiveTopology( Wireframe ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );` |
| `apEx/Phoenix/RenderLayer.cpp:52` | `phxContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );` |
| `apEx/Phoenix/Texgen.cpp:113` | `phxContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );` |

**`IASetVertexBuffers`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:705` | `DeviceContext->IASetVertexBuffers(0, 1, NULL, 0, NULL);` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:31` | `DeviceContext->IASetVertexBuffers(0, 1, &VertexBufferHandle, &stride, &Offset);` |
| `apEx/Phoenix/RenderLayer.cpp:47` | `phxContext->IASetVertexBuffers( 0, 1, &VertexBuffer, &stride, &offset );` |
| `apEx/Phoenix/Texgen.cpp:112` | `phxContext->IASetVertexBuffers( 0, 1, &TexgenVertexBuffer, &stride, &offset );` |

**`Map`** ×10

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:55` | `if (DeviceContext->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map) != S_OK)` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:68` | `HRESULT res = DeviceContext->Map(IndexBufferHandle, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms);` |
| `Bedrock/CoRE2/DX11Texture.cpp:1275` | `hr = pContext->Map( pStaging, 0, D3D11_MAP_READ, 0, &mapped );` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:119` | `if (DeviceContext->Map(VertexBufferHandle, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms) != S_OK) return false;` |
| `apEx/Phoenix/RenderLayer.cpp:65` | `phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Scene.cpp:153` | `phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Scene.cpp:580` | `phxContext->Map( VertexBuffer, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms );` |
| `apEx/Phoenix/Texgen.cpp:156` | `phxContext->Map( TexgenBufferPS, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Timeline.cpp:92` | `phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Timeline.cpp:98` | `phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |

**`OMSetBlendState`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:563` | `DeviceContext->OMSetBlendState(NULL, NULL, 0xffffffff);` |
| `Bedrock/CoRE2/DX11RenderState.cpp:58` | `Context->OMSetBlendState(State, NULL, 0xffffffff);` |
| `apEx/Phoenix/RenderLayer.cpp:61` | `phxContext->OMSetBlendState( BlendState, NULL, 0xffffffff );` |
| `apEx/Phoenix/Texgen.cpp:106` | `phxContext->OMSetBlendState( NULL, NULL, 0xffffffff );` |
| `apEx/Phoenix/Timeline.cpp:83` | `phxContext->OMSetBlendState( MaterialState[ x ]->BlendState, NULL, 0xffffffff );` |

**`OMSetDepthStencilState`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:585` | `DeviceContext->OMSetDepthStencilState(NULL, 0);` |
| `Bedrock/CoRE2/DX11RenderState.cpp:109` | `Context->OMSetDepthStencilState(State, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:62` | `phxContext->OMSetDepthStencilState( DepthStencilState, 0 );` |
| `apEx/Phoenix/Texgen.cpp:107` | `phxContext->OMSetDepthStencilState( NULL, 0 );` |
| `apEx/Phoenix/Timeline.cpp:84` | `phxContext->OMSetDepthStencilState( MaterialState[ x ]->DepthStencilState, 0 );` |

**`OMSetRenderTargets`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:212` | `DeviceContext->OMSetRenderTargets( 1, &BackBufferView, DepthBufferView );` |
| `Bedrock/CoRE2/DX11Device.cpp:298` | `DeviceContext->OMSetRenderTargets(1, &BackBufferView, DepthBufferView);` |
| `Bedrock/CoRE2/DX11Device.cpp:901` | `DeviceContext->OMSetRenderTargets( 1, &BackBufferView, DepthBufferView );` |
| `apEx/Phoenix/RenderLayer.cpp:129` | `phxContext->OMSetRenderTargets( TargetCount, rendertargets, OmitDepthBuffer ? NULL : phxDepthBufferView );` |
| `apEx/Phoenix/Texgen.cpp:148` | `phxContext->OMSetRenderTargets( 1, &Target->RTView, NULL );` |
| `apEx/Phoenix/Timeline.cpp:75` | `phxContext->OMSetRenderTargets( 1, &MaterialState[ x ]->RenderTarget->RTView, NULL );` |
| `apEx/Phoenix/Timeline.cpp:371` | `phxContext->OMSetRenderTargets( 1, &phxBackBufferView, NULL );` |

**`OMSetRenderTargetsAndUnorderedAccessViews`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:127` | `phxContext->OMSetRenderTargetsAndUnorderedAccessViews( TargetCount, rendertargets, OmitDepthBuffer ? NULL : phxDepthBufferView, TargetCount, 0, NULL, NULL );` |
| `apEx/Phoenix/RenderLayer.cpp:144` | `phxContext->OMSetRenderTargetsAndUnorderedAccessViews( 1, &phxVoxelForceMSAARTView, nullptr, 1, 1, &phxTexture3DUAV, nullptr );` |

**`PSSetConstantBuffers`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:838` | `DeviceContext->PSSetConstantBuffers(Slot, Count, (ID3D11Buffer**)buffers);` |
| `apEx/Phoenix/Scene.cpp:189` | `phxContext->PSSetConstantBuffers( y, 1, &Buffer );` |
| `apEx/Phoenix/Texgen.cpp:127` | `phxContext->PSSetConstantBuffers( 0, 1, &TexgenBufferPS );` |
| `apEx/Phoenix/Timeline.cpp:49` | `phxContext->PSSetConstantBuffers( 0, 1, &SceneDataBuffer );` |
| `apEx/Phoenix/Timeline.cpp:50` | `phxContext->PSSetConstantBuffers( 1, 1, &ObjectMatrixBuffer );` |

**`PSSetSamplers`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:222` | `Context->PSSetSamplers(Smp - CORESMP_PS0, 1, &State);` |
| `apEx/Phoenix/Texgen.cpp:94` | `phxContext->PSSetSamplers( 0, 1, &TexgenSampler );` |
| `apEx/Phoenix/Texgen.cpp:95` | `phxContext->PSSetSamplers( 1, 1, &TexgenSampler_NoWrap );` |
| `apEx/Phoenix/Texgen.cpp:96` | `phxContext->PSSetSamplers( 2, 1, &TexgenSampler_ShadowCompare );` |
| `apEx/Phoenix/phxEngine.cpp:213` | `phxContext->PSSetSamplers( 0, 1, &TexgenSampler );` |
| `apEx/Phoenix/phxEngine.cpp:215` | `phxContext->PSSetSamplers( 1, 1, &TexgenSampler_NoWrap );` |
| `apEx/Phoenix/phxEngine.cpp:217` | `phxContext->PSSetSamplers( 2, 1, &TexgenSampler_ShadowCompare );` |

**`PSSetShader`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:693` | `DeviceContext->PSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Shader.cpp:390` | `DeviceContext->PSSetShader(PixelShaderHandle, NULL, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:59` | `phxContext->PSSetShader( PS, NULL, 0 );` |
| `apEx/Phoenix/Texgen.cpp:125` | `phxContext->PSSetShader( PixelShader, NULL, 0 );` |
| `apEx/Phoenix/Timeline.cpp:80` | `phxContext->PSSetShader( Tech->RenderPasses[ x ]->PS, NULL, 0 );` |
| `apEx/Phoenix/Timeline.cpp:367` | `phxContext->PSSetShader( RenderPixelShader, NULL, 0 );` |

**`PSSetShaderResources`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:606` | `DeviceContext->PSSetShaderResources(Sampler, 1, null);` |
| `Bedrock/CoRE2/DX11Texture.cpp:39` | `DeviceContext->PSSetShaderResources( smp, 1, &View );` |
| `apEx/Phoenix/RenderLayer.cpp:85` | `phxContext->PSSetShaderResources( 0, 8, Textures );` |
| `apEx/Phoenix/Texgen.cpp:145` | `phxContext->PSSetShaderResources( 0, 5, Textures );` |
| `apEx/Phoenix/Texgen.cpp:169` | `phxContext->PSSetShaderResources( 0, 5, Textures );` |
| `apEx/Phoenix/Timeline.cpp:86` | `phxContext->PSSetShaderResources( 0, 8, MaterialState[ x ]->Textures );` |
| `apEx/Phoenix/Timeline.cpp:127` | `phxContext->PSSetShaderResources( 0, 8, rv );` |
| `apEx/Phoenix/Timeline.cpp:372` | `phxContext->PSSetShaderResources( 0, 1, &Target->View );` |
| `apEx/Phoenix/Timeline.cpp:374` | `phxContext->PSSetShaderResources( 0, 1, rv );` |

**`RSSetState`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:574` | `DeviceContext->RSSetState(NULL);` |
| `Bedrock/CoRE2/DX11RenderState.cpp:163` | `Context->RSSetState(State);` |
| `apEx/Phoenix/RenderLayer.cpp:60` | `phxContext->RSSetState( RasterizerState );` |
| `apEx/Phoenix/Texgen.cpp:105` | `phxContext->RSSetState( NULL );` |
| `apEx/Phoenix/Timeline.cpp:82` | `phxContext->RSSetState( MaterialState[ x ]->RasterizerState );` |

**`RSSetViewports`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:817` | `DeviceContext->RSSetViewports(1, &viewport);` |
| `apEx/Phoenix/RenderLayer.cpp:125` | `phxContext->RSSetViewports( TargetCount, viewports );` |
| `apEx/Phoenix/RenderLayer.cpp:143` | `phxContext->RSSetViewports( 1, viewports );` |
| `apEx/Phoenix/RenderTarget.cpp:13` | `phxContext->RSSetViewports( 1, &v );` |
| `apEx/Phoenix/Timeline.cpp:370` | `phxContext->RSSetViewports( 1, &v );` |

**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:43` | `DeviceContext->Release();` |

**`ResolveSubresource`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:967` | `pContext->ResolveSubresource( pTemp, index, pSource, index, fmt );` |

**`Unmap`** ×11

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:62` | `DeviceContext->Unmap(Buffer, 0);` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:88` | `DeviceContext->Unmap(IndexBufferHandle, 0);` |
| `Bedrock/CoRE2/DX11Texture.cpp:1286` | `pContext->Unmap( pStaging, 0 );` |
| `Bedrock/CoRE2/DX11Texture.cpp:1302` | `pContext->Unmap( pStaging, 0 );` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:133` | `DeviceContext->Unmap(VertexBufferHandle, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:68` | `phxContext->Unmap( ObjectMatrixBuffer, 0 );` |
| `apEx/Phoenix/Scene.cpp:182` | `phxContext->Unmap( SceneDataBuffer, 0 );` |
| `apEx/Phoenix/Scene.cpp:604` | `phxContext->Unmap( VertexBuffer, NULL );` |
| `apEx/Phoenix/Texgen.cpp:158` | `phxContext->Unmap( TexgenBufferPS, 0 );` |
| `apEx/Phoenix/Timeline.cpp:95` | `phxContext->Unmap( ObjectMatrixBuffer, 0 );` |
| `apEx/Phoenix/Timeline.cpp:116` | `phxContext->Unmap( SceneDataBuffer, 0 );` |

**`UpdateSubresource`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:221` | `DeviceContext->UpdateSubresource( TextureHandle, 0, NULL, Data, XRes*BytesPerPixel, 0 );` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:102` | `DeviceContext->UpdateSubresource(VertexBufferHandle, 0, &box, Data, Size, 1);` |

**`VSSetConstantBuffers`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:836` | `DeviceContext->VSSetConstantBuffers(Slot, Count, (ID3D11Buffer**)buffers);` |
| `apEx/Phoenix/Scene.cpp:187` | `phxContext->VSSetConstantBuffers( y, 1, &Buffer );` |

**`VSSetSamplers`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:225` | `Context->VSSetSamplers(Smp - CORESMP_VS0, 1, &State);` |
| `apEx/Phoenix/phxEngine.cpp:212` | `phxContext->VSSetSamplers( 0, 1, &TexgenSampler );` |
| `apEx/Phoenix/phxEngine.cpp:214` | `phxContext->VSSetSamplers( 1, 1, &TexgenSampler_NoWrap );` |
| `apEx/Phoenix/phxEngine.cpp:216` | `phxContext->VSSetSamplers( 2, 1, &TexgenSampler_ShadowCompare );` |

**`VSSetShader`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:643` | `DeviceContext->VSSetShader(NULL, NULL, 0);` |
| `Bedrock/CoRE2/DX11Shader.cpp:241` | `DeviceContext->VSSetShader(VertexShaderHandle, NULL, 0);` |
| `apEx/Phoenix/RenderLayer.cpp:55` | `phxContext->VSSetShader( VS, NULL, 0 );` |
| `apEx/Phoenix/Texgen.cpp:101` | `phxContext->VSSetShader( TexgenVertexShader, NULL, 0 );` |

**`VSSetShaderResources`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:608` | `DeviceContext->VSSetShaderResources(Sampler - CORESMP_VS0, 1, null);` |
| `Bedrock/CoRE2/DX11Texture.cpp:41` | `DeviceContext->VSSetShaderResources( smp - CORESMP_VS0, 1, &View );` |
| `apEx/Phoenix/RenderLayer.cpp:83` | `phxContext->VSSetShaderResources( 0, 8, Textures );` |

### `ID3D11DomainShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:678` | `DomainShaderHandle->Release();` |

### `ID3D11GeometryShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:532` | `GeometryShaderHandle->Release();` |

### `ID3D11HullShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:824` | `HullShaderHandle->Release();` |

### `ID3D11InputLayout`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexFormat.cpp:21` | `if (VertexFormatHandle) VertexFormatHandle->Release();` |

### `ID3D11PixelShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:383` | `PixelShaderHandle->Release();` |

### `ID3D11Query`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:28` | `if ( OcclusionQuery ) OcclusionQuery->Release();` |

### `ID3D11RenderTargetView`


**`Release`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:30` | `if (BackBufferView) BackBufferView->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:75` | `if (BackBufferView) BackBufferView->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:281` | `if (BackBufferView) BackBufferView->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:30` | `if ( RTView ) RTView->Release();` |

### `ID3D11Resource`


**`GetType`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:913` | `pSource->GetType( &resType );` |

**`QueryInterface`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:919` | `HRESULT hr = pSource->QueryInterface( __uuidof( ID3D11Texture2D ), (void**)&pTexture );` |

### `ID3D11SamplerState`


**`Release`** ×3

| file:line | source |
|---|---|
| `apEx/Phoenix_Tool/TexgenPage.cpp:354` | `TexgenSampler->Release();` |
| `apEx/Phoenix_Tool/TexgenPage.cpp:356` | `TexgenSampler_NoWrap->Release();` |
| `apEx/Phoenix_Tool/TexgenPage.cpp:358` | `TexgenSampler_ShadowCompare->Release();` |

### `ID3D11ShaderResourceView`


**`Release`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:28` | `if ( View ) View->Release();` |
| `apEx/Phoenix/Scene.cpp:732` | `if ( SplineTextureView ) SplineTextureView->Release();` |

### `ID3D11Texture1D/ID3D11Texture2D/ID3D11Texture3D`


**`Release`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:715` | `tex->Release();` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:726` | `tex->Release();` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:790` | `tex->Release();` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:801` | `tex->Release();` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:837` | `tex->Release();` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:848` | `tex->Release();` |

### `ID3D11Texture2D`


**`GetDesc`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:173` | `TextureHandle->GetDesc( &texturedesc );` |
| `Bedrock/CoRE2/DX11Texture.cpp:232` | `TextureHandle->GetDesc( &desc );` |
| `Bedrock/CoRE2/DX11Texture.cpp:925` | `pTexture->GetDesc( &desc );` |

**`Release`** ×27

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:32` | `if (DepthBuffer) DepthBuffer->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:102` | `res = bb->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:118` | `if (DepthBuffer) DepthBuffer->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:283` | `if (DepthBuffer) DepthBuffer->Release();` |
| `Bedrock/CoRE2/DX11Device.cpp:951` | `bb->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:26` | `if ( TextureHandle ) TextureHandle->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:937` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:950` | `if ( pTemp ) pTemp->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:951` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:957` | `if ( pTemp ) pTemp->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:958` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:979` | `if ( pTemp ) pTemp->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:980` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:988` | `if ( pTemp ) pTemp->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:989` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1007` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1015` | `if ( pTexture ) pTexture->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1165` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1235` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1270` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1279` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1288` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1309` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1315` | `if ( pStaging ) pStaging->Release();` |
| `Bedrock/CoRE2/DX11Texture.cpp:1320` | `if ( pStaging ) pStaging->Release();` |
| `apEx/Phoenix/Scene.cpp:733` | `if ( SplineTexture ) SplineTexture->Release();` |
| `apEx/Phoenix/Texgen.cpp:181` | `Lookup->Texture->Release();` |

### `ID3D11VertexShader`


**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:234` | `VertexShaderHandle->Release();` |

### `IDXGIAdapter`


**`EnumOutputs`** ×1

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/MinimalPlayer.cpp:208` | `result = adapter->EnumOutputs( 0, &adapterOutput );` |

### `IDXGIFactory`


**`EnumAdapters`** ×1

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/MinimalPlayer.cpp:206` | `result = factory->EnumAdapters( 0, &adapter );` |

### `IDXGIOutput`


**`GetDisplayModeList`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/MinimalPlayer.cpp:210` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, NULL );` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:213` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, displayModeList );` |

**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:263` | `if (i) i->Release();` |

### `IDXGISwapChain`


**`GetBuffer`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:82` | `res = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&bb);` |
| `Bedrock/CoRE2/DX11Device.cpp:926` | `HRESULT res = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&bb);` |
| `Bedrock/CoRE2/DX11Device.cpp:1002` | `HRESULT res = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&bb);` |
| `apEx/MinimalPlayer/WindowHandler.cpp:97` | `SwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), (LPVOID*)&bb );` |

**`GetDesc`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:272` | `HRESULT res = SwapChain->GetDesc(&desc);` |

**`GetFullscreenState`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:257` | `if (SwapChain->GetFullscreenState(&fs, &i) != S_OK)` |

**`Present`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:763` | `if (Vsync) res = SwapChain->Present(1, 0);` |
| `Bedrock/CoRE2/DX11Device.cpp:764` | `else res = SwapChain->Present(0, 0);` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:290` | `SwapChain->Present( setupcfg.vsync ? 1 : 0, 0 );` |
| `apEx/Phoenix/Project.cpp:182` | `SwapChain->Present( 0, 0 );` |

**`Release`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:36` | `SwapChain->Release();` |

**`ResizeBuffers`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:288` | `res = SwapChain->ResizeBuffers(desc.BufferCount, 0, 0, DXGI_FORMAT_UNKNOWN, 0);` |

**`SetFullscreenState`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:35` | `SwapChain->SetFullscreenState(false, NULL);` |
| `Bedrock/CoRE2/DX11Device.cpp:314` | `HRESULT res = SwapChain->SetFullscreenState(FullScreen, NULL);` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:303` | `SwapChain->SetFullscreenState( FALSE, NULL );` |

---

# Appendix B — full `D3D11_*` / `D3D10_*` / `D3D_*` / `DXGI_*` symbol inventory

Every occurrence of every `D3D11_*`, `D3D10_*`, `D3D_*` and `DXGI_*` symbol in the four scope
directories, with the exact source line. Use this as the checklist for the shim header: any
symbol listed here must be declared, and any symbol **not** listed here need not be.

## D3D11_/D3D10_/D3D_ symbols


**`D3D10_CLEAR_DEPTH`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:132` | `phxContext->ClearDepthStencilView( phxDepthBufferView, ( D3D10_CLEAR_DEPTH * !OmitDepthBuffer ) \| D3D10_CLEAR_STENCIL, 1, 0 );` |
| `apEx/Phoenix/Timeline.cpp:337` | `phxContext->ClearDepthStencilView( phxDepthBufferView, D3D10_CLEAR_DEPTH \| D3D10_CLEAR_STENCIL, 1, 0 );` |

**`D3D10_CLEAR_STENCIL`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:132` | `phxContext->ClearDepthStencilView( phxDepthBufferView, ( D3D10_CLEAR_DEPTH * !OmitDepthBuffer ) \| D3D10_CLEAR_STENCIL, 1, 0 );` |
| `apEx/Phoenix/Timeline.cpp:337` | `phxContext->ClearDepthStencilView( phxDepthBufferView, D3D10_CLEAR_DEPTH \| D3D10_CLEAR_STENCIL, 1, 0 );` |

**`D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1047` | `: D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;` |

**`D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1046` | `? D3D10_REQ_TEXTURE3D_U_V_OR_W_DIMENSION` |

**`D3D10_RESOURCE_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:356` | `D3D10_RESOURCE_DIMENSION resourceDimension;` |

**`D3D10_STANDARD_MULTISAMPLE_PATTERN`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:489` | `tex.SampleDesc.Quality = MSCount > 1 ? D3D10_STANDARD_MULTISAMPLE_PATTERN : 0;` |

**`D3D10_VIEWPORT`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix_Tool/Model_Tool.cpp:1334` | `D3D10_VIEWPORT vp;` |
| `apEx/Phoenix_Tool/Scene_tool.cpp:1456` | `D3D10_VIEWPORT vp;` |

**`D3D11_BIND_CONSTANT_BUFFER`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:33` | `desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;` |
| `apEx/Phoenix/phxEngine.cpp:161` | `static D3D11_BUFFER_DESC desc = { 0, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };` |

**`D3D11_BIND_DEPTH_STENCIL`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:137` | `depthBufferDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;` |
| `Bedrock/CoRE2/DX11Texture.cpp:490` | `tex.BindFlags = D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE;` |
| `apEx/Phoenix/Project.cpp:96` | `static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE, ` |

**`D3D11_BIND_INDEX_BUFFER`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:45` | `bd.BindFlags = D3D11_BIND_INDEX_BUFFER;` |
| `apEx/Phoenix/Mesh.cpp:485` | `bd.BindFlags = D3D11_BIND_INDEX_BUFFER;` |

**`D3D11_BIND_RENDER_TARGET`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:64` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| ( rendertarget ? D3D11_BIND_RENDER_TARGET : 0 );` |
| `apEx/Phoenix/Project.cpp:202` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET;` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/phxEngine.cpp:236` | `textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_UNORDERED_ACCESS \| D3D11_BIND_RENDER_TARGET;` |
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:26` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET;` |

**`D3D11_BIND_SHADER_RESOURCE`** ×17

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:684` | `desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:743` | `desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:816` | `desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;` |
| `Bedrock/CoRE2/DX11Texture.cpp:64` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| ( rendertarget ? D3D11_BIND_RENDER_TARGET : 0 );` |
| `Bedrock/CoRE2/DX11Texture.cpp:490` | `tex.BindFlags = D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE;` |
| `apEx/Phoenix/Mesh.cpp:1742` | `treeDataBuffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;` |
| `apEx/Phoenix/Project.cpp:96` | `static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE, ` |
| `apEx/Phoenix/Project.cpp:202` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET;` |
| `apEx/Phoenix/Scene.cpp:761` | `D3D11_TEXTURE2D_DESC tex = { 2048,splinecnt,1,1,DXGI_FORMAT_R32_FLOAT,1,0,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0 };` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/Texgen.cpp:215` | `D3D11_TEXTURE2D_DESC tex = { XRes, YRes, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:233` | `textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;` |
| `apEx/Phoenix/phxEngine.cpp:236` | `textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_UNORDERED_ACCESS \| D3D11_BIND_RENDER_TARGET;` |
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |
| `apEx/Phoenix/phxEngine.cpp:299` | `static const D3D11_TEXTURE2D_DESC ltctex16 = { 16, 16, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:303` | `static const D3D11_TEXTURE2D_DESC ltctex8 = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:26` | `tex.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET;` |

**`D3D11_BIND_UNORDERED_ACCESS`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:236` | `textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_UNORDERED_ACCESS \| D3D11_BIND_RENDER_TARGET;` |

**`D3D11_BIND_VERTEX_BUFFER`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:46` | `bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:75` | `bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;` |
| `apEx/Phoenix/Mesh.cpp:464` | `D3D11_BUFFER_DESC bd = { (unsigned char*)LockedVertices - (unsigned char*)lv, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |
| `apEx/Phoenix/Project.cpp:1168` | `bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;` |
| `apEx/Phoenix/phxEngine.cpp:162` | `static const D3D11_BUFFER_DESC bd = { 6 * 6 * sizeof( float ), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |

**`D3D11_BLEND`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:44` | `D3D11_BLEND DX11BlendFactors[] =` |
| `Bedrock/CoRE2/DX11Enums.h:11` | `extern D3D11_BLEND DX11BlendFactors[];` |
| `apEx/Phoenix/Project.cpp:1950` | `BlendDesc.RenderTarget[ z ].SrcBlend = (D3D11_BLEND)( ( v.BlendMode & 15 ) );` |
| `apEx/Phoenix/Project.cpp:1951` | `BlendDesc.RenderTarget[ z ].DestBlend = (D3D11_BLEND)( ( v.BlendMode >> 4 ) );` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:762` | `Collector.BlendDesc.RenderTarget[ z ].SrcBlend = (D3D11_BLEND)( ( Parameters[ x ]->Parameter.Value.BlendMode + 0x11 ) & 15 );` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:763` | `Collector.BlendDesc.RenderTarget[ z ].DestBlend = (D3D11_BLEND)( ( Parameters[ x ]->Parameter.Value.BlendMode + 0x11 ) >> 4 );` |

**`D3D11_BLEND_BLEND_FACTOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:57` | `D3D11_BLEND_BLEND_FACTOR,` |

**`D3D11_BLEND_DESC`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:25` | `D3D11_BLEND_DESC desc;` |
| `apEx/Phoenix/Project.cpp:84` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix/Project.cpp:1843` | `D3D11_BLEND_DESC BlendDesc;` |
| `apEx/Phoenix/Project.cpp:1873` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix_Tool/Material_Tool.h:107` | `D3D11_BLEND_DESC BlendDesc;` |

**`D3D11_BLEND_DEST_ALPHA`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:52` | `D3D11_BLEND_DEST_ALPHA,` |

**`D3D11_BLEND_DEST_COLOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:54` | `D3D11_BLEND_DEST_COLOR,` |

**`D3D11_BLEND_INV_BLEND_FACTOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:58` | `D3D11_BLEND_INV_BLEND_FACTOR,` |

**`D3D11_BLEND_INV_DEST_ALPHA`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:53` | `D3D11_BLEND_INV_DEST_ALPHA,` |

**`D3D11_BLEND_INV_DEST_COLOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:55` | `D3D11_BLEND_INV_DEST_COLOR,` |

**`D3D11_BLEND_INV_SRC_ALPHA`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:51` | `D3D11_BLEND_INV_SRC_ALPHA,` |

**`D3D11_BLEND_INV_SRC_COLOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:49` | `D3D11_BLEND_INV_SRC_COLOR,` |

**`D3D11_BLEND_ONE`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:47` | `D3D11_BLEND_ONE,` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1253` | `BlendDesc.RenderTarget[ x ].SrcBlend = D3D11_BLEND_ONE;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1256` | `BlendDesc.RenderTarget[ x ].SrcBlendAlpha = D3D11_BLEND_ONE;` |

**`D3D11_BLEND_OP`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:61` | `D3D11_BLEND_OP DX11BlendOps[] =` |
| `Bedrock/CoRE2/DX11Enums.h:12` | `extern D3D11_BLEND_OP DX11BlendOps[];` |

**`D3D11_BLEND_OP_ADD`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:63` | `D3D11_BLEND_OP_ADD,` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1255` | `BlendDesc.RenderTarget[ x ].BlendOp = D3D11_BLEND_OP_ADD;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1258` | `BlendDesc.RenderTarget[ x ].BlendOpAlpha = D3D11_BLEND_OP_ADD;` |

**`D3D11_BLEND_OP_MAX`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:67` | `D3D11_BLEND_OP_MAX,` |

**`D3D11_BLEND_OP_MIN`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:66` | `D3D11_BLEND_OP_MIN,` |

**`D3D11_BLEND_OP_REV_SUBTRACT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:65` | `D3D11_BLEND_OP_REV_SUBTRACT,` |

**`D3D11_BLEND_OP_SUBTRACT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:64` | `D3D11_BLEND_OP_SUBTRACT,` |

**`D3D11_BLEND_SRC_ALPHA`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:50` | `D3D11_BLEND_SRC_ALPHA,` |

**`D3D11_BLEND_SRC_ALPHA_SAT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:56` | `D3D11_BLEND_SRC_ALPHA_SAT,` |

**`D3D11_BLEND_SRC_COLOR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:48` | `D3D11_BLEND_SRC_COLOR,` |

**`D3D11_BLEND_ZERO`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:46` | `D3D11_BLEND_ZERO,` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1254` | `BlendDesc.RenderTarget[ x ].DestBlend = D3D11_BLEND_ZERO;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1257` | `BlendDesc.RenderTarget[ x ].DestBlendAlpha = D3D11_BLEND_ZERO;` |

**`D3D11_BOX`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:97` | `D3D11_BOX box;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:98` | `memset(&box, 0, sizeof(D3D11_BOX));` |

**`D3D11_BUFFER_DESC`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:30` | `D3D11_BUFFER_DESC desc;` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:40` | `D3D11_BUFFER_DESC bd;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:41` | `D3D11_BUFFER_DESC bd;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:70` | `D3D11_BUFFER_DESC bd;` |
| `apEx/Phoenix/Mesh.cpp:464` | `D3D11_BUFFER_DESC bd = { (unsigned char*)LockedVertices - (unsigned char*)lv, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |
| `apEx/Phoenix/Mesh.cpp:1737` | `D3D11_BUFFER_DESC treeDataBuffer;` |
| `apEx/Phoenix/Project.cpp:1163` | `D3D11_BUFFER_DESC bd;` |
| `apEx/Phoenix/phxEngine.cpp:161` | `static D3D11_BUFFER_DESC desc = { 0, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:162` | `static const D3D11_BUFFER_DESC bd = { 6 * 6 * sizeof( float ), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |

**`D3D11_CLEAR_DEPTH`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:737` | `DeviceContext->ClearDepthStencilView(DepthBufferView, D3D11_CLEAR_DEPTH \| D3D11_CLEAR_STENCIL, Depth, Stencil);` |

**`D3D11_CLEAR_STENCIL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:737` | `DeviceContext->ClearDepthStencilView(DepthBufferView, D3D11_CLEAR_DEPTH \| D3D11_CLEAR_STENCIL, Depth, Stencil);` |

**`D3D11_COLOR_WRITE_ENABLE_ALL`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1259` | `BlendDesc.RenderTarget[ x ].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;` |

**`D3D11_COMPARISON_ALWAYS`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:41` | `D3D11_COMPARISON_ALWAYS,` |

**`D3D11_COMPARISON_EQUAL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:36` | `D3D11_COMPARISON_EQUAL,` |

**`D3D11_COMPARISON_FUNC`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:32` | `D3D11_COMPARISON_FUNC DX11ComparisonFunctions[] =` |
| `Bedrock/CoRE2/DX11Enums.h:10` | `extern D3D11_COMPARISON_FUNC DX11ComparisonFunctions[];` |
| `apEx/Phoenix/Material.h:108` | `D3D11_COMPARISON_FUNC ZFunction;` |
| `apEx/Phoenix_Tool/Model_Tool.cpp:2143` | `MaterialParams[ p ].ZFunction = (D3D11_COMPARISON_FUNC)v;` |

**`D3D11_COMPARISON_GREATER`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:38` | `D3D11_COMPARISON_GREATER,` |

**`D3D11_COMPARISON_GREATER_EQUAL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:40` | `D3D11_COMPARISON_GREATER_EQUAL,` |

**`D3D11_COMPARISON_LESS`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:35` | `D3D11_COMPARISON_LESS,` |
| `apEx/Phoenix/Project.cpp:1859` | `DepthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;` |
| `apEx/Phoenix/phxEngine.cpp:209` | `smpData.ComparisonFunc = D3D11_COMPARISON_LESS;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1244` | `DepthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;` |

**`D3D11_COMPARISON_LESS_EQUAL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:37` | `D3D11_COMPARISON_LESS_EQUAL,` |

**`D3D11_COMPARISON_NEVER`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:34` | `D3D11_COMPARISON_NEVER,` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |

**`D3D11_COMPARISON_NOT_EQUAL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:39` | `D3D11_COMPARISON_NOT_EQUAL,` |

**`D3D11_CPU_ACCESS_READ`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:973` | `desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;` |
| `Bedrock/CoRE2/DX11Texture.cpp:991` | `else if ( ( desc.Usage == D3D11_USAGE_STAGING ) && ( desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ ) )` |
| `Bedrock/CoRE2/DX11Texture.cpp:1001` | `desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;` |

**`D3D11_CPU_ACCESS_WRITE`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:34` | `desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:46` | `bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:76` | `bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;` |
| `apEx/Phoenix/Project.cpp:1169` | `bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;` |
| `apEx/Phoenix/phxEngine.cpp:161` | `static D3D11_BUFFER_DESC desc = { 0, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };` |

**`D3D11_CREATE_DEVICE_DEBUG`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:189` | `res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &Device, NULL, &Devi` |

**`D3D11_CULL_BACK`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:22` | `D3D11_CULL_BACK,` |
| `apEx/Phoenix/Project.cpp:1848` | `RasterDesc.CullMode = D3D11_CULL_BACK;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1232` | `RasterDesc.CullMode = D3D11_CULL_BACK;` |

**`D3D11_CULL_FRONT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:21` | `D3D11_CULL_FRONT,` |

**`D3D11_CULL_MODE`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:18` | `D3D11_CULL_MODE DX11CullModes[] =` |
| `Bedrock/CoRE2/DX11Enums.h:8` | `extern D3D11_CULL_MODE DX11CullModes[];` |
| `apEx/Phoenix/Material.h:110` | `D3D11_CULL_MODE CullMode;` |

**`D3D11_CULL_NONE`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:20` | `D3D11_CULL_NONE,` |
| `apEx/Phoenix_Tool/apxProject.cpp:58` | `p->Parameter.Value.CullMode = D3D11_CULL_NONE;` |

**`D3D11_DEPTH_STENCIL_DESC`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:83` | `D3D11_DEPTH_STENCIL_DESC desc;` |
| `Bedrock/CoRE2/DX11RenderState.cpp:84` | `memset(&desc, 0, sizeof(D3D11_DEPTH_STENCIL_DESC));` |
| `apEx/Phoenix/Project.cpp:84` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix/Project.cpp:1842` | `D3D11_DEPTH_STENCIL_DESC DepthStencilDesc;` |
| `apEx/Phoenix/Project.cpp:1873` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix_Tool/Material_Tool.h:106` | `D3D11_DEPTH_STENCIL_DESC DepthStencilDesc;` |

**`D3D11_DEPTH_STENCIL_VIEW_DESC`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:125` | `D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc;` |
| `Bedrock/CoRE2/DX11Texture.cpp:500` | `D3D11_DEPTH_STENCIL_VIEW_DESC deptdesc;` |
| `apEx/Phoenix/Project.cpp:94` | `static D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = { DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, 0 };` |

**`D3D11_DEPTH_WRITE_MASK_ALL`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:87` | `desc.DepthWriteMask = ZWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |
| `apEx/Phoenix/Project.cpp:1858` | `DepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;` |
| `apEx/Phoenix/Project.cpp:1891` | `DepthStencilDesc.DepthWriteMask = !( ( v.ZMode >> 1 ) & 1 ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:746` | `Collector.DepthStencilDesc.DepthWriteMask = !( ( Parameters[ x ]->Parameter.Value.ZMode >> 1 ) & 1 ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1243` | `DepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;` |

**`D3D11_DEPTH_WRITE_MASK_ZERO`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:87` | `desc.DepthWriteMask = ZWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |
| `apEx/Phoenix/Project.cpp:1891` | `DepthStencilDesc.DepthWriteMask = !( ( v.ZMode >> 1 ) & 1 ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:746` | `Collector.DepthStencilDesc.DepthWriteMask = !( ( Parameters[ x ]->Parameter.Value.ZMode >> 1 ) & 1 ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;` |

**`D3D11_DSV_DIMENSION_TEXTURE2D`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:154` | `depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;` |
| `Bedrock/CoRE2/DX11Texture.cpp:504` | `deptdesc.ViewDimension = MSCount > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;` |
| `apEx/Phoenix/Project.cpp:94` | `static D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = { DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, 0 };` |

**`D3D11_DSV_DIMENSION_TEXTURE2DMS`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:504` | `deptdesc.ViewDimension = MSCount > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;` |

**`D3D11_FILL_MODE`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:25` | `D3D11_FILL_MODE DX11FillModes[] =` |
| `Bedrock/CoRE2/DX11Enums.h:9` | `extern D3D11_FILL_MODE DX11FillModes[];` |

**`D3D11_FILL_SOLID`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:27` | `D3D11_FILL_SOLID,` |
| `apEx/Phoenix/Project.cpp:1847` | `RasterDesc.FillMode = D3D11_FILL_SOLID;` |
| `apEx/Phoenix_Tool/Material_Tool.cpp:1231` | `RasterDesc.FillMode = D3D11_FILL_SOLID;` |

**`D3D11_FILL_WIREFRAME`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:28` | `D3D11_FILL_WIREFRAME,` |
| `Bedrock/CoRE2/DX11Enums.cpp:29` | `D3D11_FILL_WIREFRAME,` |

**`D3D11_FILTER`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:70` | `D3D11_FILTER DX11Filters[] =` |
| `Bedrock/CoRE2/DX11Enums.h:13` | `extern D3D11_FILTER DX11Filters[];` |

**`D3D11_FILTER_ANISOTROPIC`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:80` | `D3D11_FILTER_ANISOTROPIC,` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |

**`D3D11_FILTER_COMPARISON_ANISOTROPIC`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:89` | `D3D11_FILTER_COMPARISON_ANISOTROPIC,` |

**`D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:85` | `D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT,` |

**`D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:86` | `D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR,` |

**`D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:87` | `D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,` |

**`D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:88` | `D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,` |
| `apEx/Phoenix/phxEngine.cpp:207` | `smpData.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;` |

**`D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:81` | `D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT,` |

**`D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:82` | `D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR,` |

**`D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:83` | `D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT,` |

**`D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:84` | `D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR,` |

**`D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:76` | `D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,` |

**`D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:77` | `D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,` |

**`D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:78` | `D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,` |

**`D3D11_FILTER_MIN_MAG_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:79` | `D3D11_FILTER_MIN_MAG_MIP_LINEAR,` |

**`D3D11_FILTER_MIN_MAG_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:72` | `D3D11_FILTER_MIN_MAG_MIP_POINT,` |

**`D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:73` | `D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,` |

**`D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:74` | `D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,` |

**`D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:75` | `D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,` |

**`D3D11_FLOAT32_MAX`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |

**`D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:955` | `if ( !( support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE ) )` |

**`D3D11_INPUT_ELEMENT_DESC`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexFormat.cpp:45` | `D3D11_INPUT_ELEMENT_DESC *vxdecl = new D3D11_INPUT_ELEMENT_DESC[Attributes.NumItems() + 1];` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:45` | `D3D11_INPUT_ELEMENT_DESC *vxdecl = new D3D11_INPUT_ELEMENT_DESC[Attributes.NumItems() + 1];` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:46` | `memset(vxdecl, 0, sizeof(D3D11_INPUT_ELEMENT_DESC)*(Attributes.NumItems() + 1));` |
| `apEx/Phoenix/phxEngine.cpp:124` | `static const D3D11_INPUT_ELEMENT_DESC vxdecl[ 2 ] =` |
| `apEx/Phoenix/phxEngine.cpp:130` | `static const D3D11_INPUT_ELEMENT_DESC vxdecl3[ 2 ] =` |
| `apEx/Phoenix/phxEngine.cpp:136` | `static const D3D11_INPUT_ELEMENT_DESC vxdecl2[ 6 ] =` |

**`D3D11_INPUT_PER_VERTEX_DATA`** ×11

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11VertexFormat.cpp:52` | `vxdecl[x].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;` |
| `apEx/Phoenix/phxEngine.cpp:126` | `{ "PositionT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:127` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof( float ) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |
| `apEx/Phoenix/phxEngine.cpp:132` | `{ "Position", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:133` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |
| `apEx/Phoenix/phxEngine.cpp:138` | `{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:139` | `{ "Position", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof( float ) * 3, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:140` | `{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof( float ) * 6, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:141` | `{ "Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, sizeof( float ) * 9, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:142` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 10, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:143` | `{ "Texcoord", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 14, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |

**`D3D11_MAPPED_SUBRESOURCE`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:54` | `D3D11_MAPPED_SUBRESOURCE map;` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:67` | `D3D11_MAPPED_SUBRESOURCE ms;` |
| `Bedrock/CoRE2/DX11Texture.cpp:1274` | `D3D11_MAPPED_SUBRESOURCE mapped;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:118` | `D3D11_MAPPED_SUBRESOURCE ms;` |
| `apEx/Phoenix/RenderLayer.cpp:64` | `D3D11_MAPPED_SUBRESOURCE map;` |
| `apEx/Phoenix/Scene.cpp:152` | `D3D11_MAPPED_SUBRESOURCE map;` |
| `apEx/Phoenix/Scene.cpp:579` | `D3D11_MAPPED_SUBRESOURCE ms;` |
| `apEx/Phoenix/Texgen.cpp:131` | `D3D11_MAPPED_SUBRESOURCE map;` |
| `apEx/Phoenix/Timeline.cpp:91` | `D3D11_MAPPED_SUBRESOURCE map;` |

**`D3D11_MAP_READ`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:1275` | `hr = pContext->Map( pStaging, 0, D3D11_MAP_READ, 0, &mapped );` |

**`D3D11_MAP_WRITE_DISCARD`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:55` | `if (DeviceContext->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map) != S_OK)` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:68` | `HRESULT res = DeviceContext->Map(IndexBufferHandle, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms);` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:119` | `if (DeviceContext->Map(VertexBufferHandle, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms) != S_OK) return false;` |
| `apEx/Phoenix/RenderLayer.cpp:65` | `phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Scene.cpp:153` | `phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Scene.cpp:580` | `phxContext->Map( VertexBuffer, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms );` |
| `apEx/Phoenix/Texgen.cpp:156` | `phxContext->Map( TexgenBufferPS, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Timeline.cpp:92` | `phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |
| `apEx/Phoenix/Timeline.cpp:98` | `phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );` |

**`D3D11_PRIMITIVE_TOPOLOGY_LINELIST`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:784` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);` |
| `Bedrock/CoRE2/DX11Device.cpp:792` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);` |
| `apEx/Phoenix/RenderLayer.cpp:48` | `phxContext->IASetPrimitiveTopology( Wireframe ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );` |

**`D3D11_PRIMITIVE_TOPOLOGY_POINTLIST`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:52` | `phxContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );` |

**`D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:776` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);` |
| `Bedrock/CoRE2/DX11Device.cpp:800` | `DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);` |
| `apEx/Phoenix/RenderLayer.cpp:48` | `phxContext->IASetPrimitiveTopology( Wireframe ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );` |
| `apEx/Phoenix/Texgen.cpp:113` | `phxContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );` |

**`D3D11_QUERY_DESC`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:224` | `D3D11_QUERY_DESC queryDesc;` |

**`D3D11_QUERY_OCCLUSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:226` | `queryDesc.Query = D3D11_QUERY_OCCLUSION;` |

**`D3D11_RASTERIZER_DESC`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:134` | `D3D11_RASTERIZER_DESC desc;` |
| `apEx/Phoenix/Project.cpp:84` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix/Project.cpp:1841` | `D3D11_RASTERIZER_DESC RasterDesc;` |
| `apEx/Phoenix/Project.cpp:1873` | `void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, C` |
| `apEx/Phoenix_Tool/Material_Tool.h:105` | `D3D11_RASTERIZER_DESC RasterDesc;` |

**`D3D11_RENDER_TARGET_BLEND_DESC`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Project.cpp:97` | `static D3D11_RENDER_TARGET_BLEND_DESC blenddesc = { true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3` |

**`D3D11_RENDER_TARGET_VIEW_DESC`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:91` | `D3D11_RENDER_TARGET_VIEW_DESC rt;` |
| `apEx/Phoenix/Project.cpp:205` | `D3D11_RENDER_TARGET_VIEW_DESC rt = { Format, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };` |
| `apEx/Phoenix/Texgen.cpp:15` | `static D3D11_RENDER_TARGET_VIEW_DESC texgenrt = { DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:277` | `static const D3D11_RENDER_TARGET_VIEW_DESC rt = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:43` | `D3D11_RENDER_TARGET_VIEW_DESC rt;` |

**`D3D11_REQ_MIP_LEVELS`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:963` | `if (mipCount > D3D11_REQ_MIP_LEVELS)` |

**`D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:971` | `if ((arraySize > D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION) \|\|` |

**`D3D11_REQ_TEXTURE1D_U_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:972` | `(width > D3D11_REQ_TEXTURE1D_U_DIMENSION))` |

**`D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:982` | `if ((arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) \|\|` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:989` | `else if ((arraySize > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) \|\|` |

**`D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:990` | `(width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) \|\|` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:991` | `(height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))` |

**`D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:999` | `(width > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION) \|\|` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1000` | `(height > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION) \|\|` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1001` | `(depth > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION))` |

**`D3D11_REQ_TEXTURECUBE_DIMENSION`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:983` | `(width > D3D11_REQ_TEXTURECUBE_DIMENSION) \|\|` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:984` | `(height > D3D11_REQ_TEXTURECUBE_DIMENSION))` |

**`D3D11_RESOURCE_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:912` | `D3D11_RESOURCE_DIMENSION resType = D3D11_RESOURCE_DIMENSION_UNKNOWN;` |

**`D3D11_RESOURCE_DIMENSION_TEXTURE1D`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:676` | `case D3D11_RESOURCE_DIMENSION_TEXTURE1D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:900` | `case D3D11_RESOURCE_DIMENSION_TEXTURE1D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:970` | `case D3D11_RESOURCE_DIMENSION_TEXTURE1D:` |

**`D3D11_RESOURCE_DIMENSION_TEXTURE2D`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:732` | `case D3D11_RESOURCE_DIMENSION_TEXTURE2D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:906` | `case D3D11_RESOURCE_DIMENSION_TEXTURE2D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:954` | `resDim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:978` | `case D3D11_RESOURCE_DIMENSION_TEXTURE2D:` |
| `Bedrock/CoRE2/DX11Texture.cpp:915` | `if ( resType != D3D11_RESOURCE_DIMENSION_TEXTURE2D )` |
| `Bedrock/CoRE2/DX11Texture.cpp:1246` | `extHeader->resourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;` |

**`D3D11_RESOURCE_DIMENSION_TEXTURE3D`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:807` | `case D3D11_RESOURCE_DIMENSION_TEXTURE3D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:915` | `case D3D11_RESOURCE_DIMENSION_TEXTURE3D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:937` | `resDim = D3D11_RESOURCE_DIMENSION_TEXTURE3D;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:997` | `case D3D11_RESOURCE_DIMENSION_TEXTURE3D:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1032` | `maxsize = (resDim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1039` | `maxsize = (resDim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1045` | `maxsize = (resDim == D3D11_RESOURCE_DIMENSION_TEXTURE3D)` |

**`D3D11_RESOURCE_DIMENSION_UNKNOWN`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:876` | `TU32 resDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;` |
| `Bedrock/CoRE2/DX11Texture.cpp:912` | `D3D11_RESOURCE_DIMENSION resType = D3D11_RESOURCE_DIMENSION_UNKNOWN;` |

**`D3D11_RESOURCE_MISC_BUFFER_STRUCTURED`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Mesh.cpp:1743` | `treeDataBuffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;` |

**`D3D11_RESOURCE_MISC_GENERATE_MIPS`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:59` | `tex.MiscFlags = rendertarget ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;` |
| `apEx/Phoenix/Project.cpp:204` | `tex.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/phxEngine.cpp:237` | `textureDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:22` | `tex.MiscFlags = AutoMipmap ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;` |

**`D3D11_RESOURCE_MISC_TEXTURECUBE`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:745` | `desc.MiscFlags = (isCubeMap) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:907` | `if (d3d10ext->miscFlag & D3D11_RESOURCE_MISC_TEXTURECUBE)` |
| `Bedrock/CoRE2/DX11Texture.cpp:972` | `desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;` |
| `Bedrock/CoRE2/DX11Texture.cpp:1000` | `desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;` |

**`D3D11_RLDO_DETAIL`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:53` | `dbg->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);` |

**`D3D11_RTV_DIMENSION_TEXTURE2D`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Texgen.cpp:15` | `static D3D11_RENDER_TARGET_VIEW_DESC texgenrt = { DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D, 0 };` |

**`D3D11_RTV_DIMENSION_TEXTURE2DMS`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:93` | `rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;` |
| `apEx/Phoenix/Project.cpp:205` | `D3D11_RENDER_TARGET_VIEW_DESC rt = { Format, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:277` | `static const D3D11_RENDER_TARGET_VIEW_DESC rt = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:45` | `rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;` |

**`D3D11_SAMPLER_DESC`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11RenderState.cpp:188` | `D3D11_SAMPLER_DESC desc;` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |

**`D3D11_SDK_VERSION`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:189` | `res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &Device, NULL, &Devi` |
| `Bedrock/CoRE2/DX11Device.cpp:196` | `res = D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &Device, NULL, &DeviceContext );` |
| `apEx/MinimalPlayer/WindowHandler.cpp:89` | `D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &phxDev, NULL, &phxContext );` |

**`D3D11_SHADER_RESOURCE_VIEW_DESC`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:695` | `D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:754` | `D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:827` | `D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;` |
| `Bedrock/CoRE2/DX11Texture.cpp:516` | `D3D11_SHADER_RESOURCE_VIEW_DESC resdesc;` |
| `apEx/Phoenix/Mesh.cpp:1751` | `D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;` |
| `apEx/Phoenix/Project.cpp:95` | `static D3D11_SHADER_RESOURCE_VIEW_DESC resdesc = { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |
| `apEx/Phoenix/phxEngine.cpp:244` | `D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;` |
| `apEx/Phoenix/phxEngine.cpp:300` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview16 = { DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |
| `apEx/Phoenix/phxEngine.cpp:304` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview8 = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |

**`D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT`** ×2

| file:line | source |
|---|---|
| `apEx/Phoenix/RenderLayer.cpp:104` | `static D3D11_VIEWPORT viewports[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];` |
| `apEx/Phoenix/RenderLayer.cpp:105` | `static ID3D11RenderTargetView *rendertargets[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];` |

**`D3D11_SRV_DIMENSION_BUFFER`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Mesh.cpp:1755` | `resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;` |

**`D3D11_SRV_DIMENSION_TEXTURE2D`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:519` | `resdesc.ViewDimension = MSCount > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;` |
| `apEx/Phoenix/Project.cpp:95` | `static D3D11_SHADER_RESOURCE_VIEW_DESC resdesc = { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |
| `apEx/Phoenix/phxEngine.cpp:300` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview16 = { DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |
| `apEx/Phoenix/phxEngine.cpp:304` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview8 = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |

**`D3D11_SRV_DIMENSION_TEXTURE2DMS`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:519` | `resdesc.ViewDimension = MSCount > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;` |

**`D3D11_SRV_DIMENSION_TEXTURE3D`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:247` | `resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;` |

**`D3D11_STANDARD_MULTISAMPLE_PATTERN`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |

**`D3D11_SUBRESOURCE_DATA`** ×12

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:579` | `_Out_writes_(mipCount*arraySize) D3D11_SUBRESOURCE_DATA* initData` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:662` | `_In_reads_(mipCount*arraySize) D3D11_SUBRESOURCE_DATA* initData,` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1009` | `std::unique_ptr<D3D11_SUBRESOURCE_DATA> initData(new D3D11_SUBRESOURCE_DATA[mipCount * arraySize]);` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1009` | `std::unique_ptr<D3D11_SUBRESOURCE_DATA> initData(new D3D11_SUBRESOURCE_DATA[mipCount * arraySize]);` |
| `Bedrock/CoRE2/DX11Texture.cpp:68` | `D3D11_SUBRESOURCE_DATA data;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:48` | `D3D11_SUBRESOURCE_DATA vxData;` |
| `apEx/Phoenix/Mesh.cpp:465` | `D3D11_SUBRESOURCE_DATA bufData = { lv, 0, 0 };` |
| `apEx/Phoenix/Mesh.cpp:1746` | `D3D11_SUBRESOURCE_DATA subData;` |
| `apEx/Phoenix/Scene.cpp:762` | `D3D11_SUBRESOURCE_DATA data = { texturedata, 2048 * 4, 0 };` |
| `apEx/Phoenix/Texgen.cpp:216` | `D3D11_SUBRESOURCE_DATA data = { 0, XRes * 4, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:163` | `static const D3D11_SUBRESOURCE_DATA vxData = { VxBuffer, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:309` | `D3D11_SUBRESOURCE_DATA subData = { ltcdata, 16 * 4 * 2, 0 };` |

**`D3D11_TEXTURE1D_DESC`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:678` | `D3D11_TEXTURE1D_DESC desc;` |

**`D3D11_TEXTURE2D_DESC`** ×21

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:734` | `D3D11_TEXTURE2D_DESC desc;` |
| `Bedrock/CoRE2/DX11Device.cpp:124` | `D3D11_TEXTURE2D_DESC depthBufferDesc;` |
| `Bedrock/CoRE2/DX11Texture.cpp:53` | `D3D11_TEXTURE2D_DESC tex;` |
| `Bedrock/CoRE2/DX11Texture.cpp:54` | `memset( &tex, 0, sizeof( D3D11_TEXTURE2D_DESC ) );` |
| `Bedrock/CoRE2/DX11Texture.cpp:172` | `D3D11_TEXTURE2D_DESC texturedesc;` |
| `Bedrock/CoRE2/DX11Texture.cpp:231` | `D3D11_TEXTURE2D_DESC desc;` |
| `Bedrock/CoRE2/DX11Texture.cpp:481` | `D3D11_TEXTURE2D_DESC tex;` |
| `Bedrock/CoRE2/DX11Texture.cpp:482` | `memset( &tex, 0, sizeof( D3D11_TEXTURE2D_DESC ) );` |
| `Bedrock/CoRE2/DX11Texture.cpp:907` | `static HRESULT CaptureTexture( ID3D11Device *d3dDevice, _In_ ID3D11DeviceContext* pContext, _In_ ID3D11Resource* pSource, _Inout_ D3D11_TEXTURE2D_DESC& desc, _Inout_ ID3D` |
| `Bedrock/CoRE2/DX11Texture.cpp:1154` | `D3D11_TEXTURE2D_DESC desc = { 0 };` |
| `apEx/Phoenix/Project.cpp:96` | `static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE, ` |
| `apEx/Phoenix/Project.cpp:198` | `D3D11_TEXTURE2D_DESC tex = depthBufferDesc;` |
| `apEx/Phoenix/Project.cpp:221` | `D3D11_TEXTURE2D_DESC dBD = depthBufferDesc;` |
| `apEx/Phoenix/Scene.cpp:761` | `D3D11_TEXTURE2D_DESC tex = { 2048,splinecnt,1,1,DXGI_FORMAT_R32_FLOAT,1,0,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0 };` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/Texgen.cpp:215` | `D3D11_TEXTURE2D_DESC tex = { XRes, YRes, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |
| `apEx/Phoenix/phxEngine.cpp:299` | `static const D3D11_TEXTURE2D_DESC ltctex16 = { 16, 16, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:303` | `static const D3D11_TEXTURE2D_DESC ltctex8 = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:16` | `D3D11_TEXTURE2D_DESC tex;` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:17` | `memset( &tex, 0, sizeof( D3D11_TEXTURE2D_DESC ) );` |

**`D3D11_TEXTURE3D_DESC`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:809` | `D3D11_TEXTURE3D_DESC desc;` |
| `apEx/Phoenix/phxEngine.cpp:225` | `D3D11_TEXTURE3D_DESC textureDesc;` |

**`D3D11_TEXTURE_ADDRESS_BORDER`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:97` | `D3D11_TEXTURE_ADDRESS_BORDER,` |

**`D3D11_TEXTURE_ADDRESS_CLAMP`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:96` | `D3D11_TEXTURE_ADDRESS_CLAMP,` |
| `apEx/Phoenix/phxEngine.cpp:205` | `smpData.AddressU = smpData.AddressV = smpData.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;` |

**`D3D11_TEXTURE_ADDRESS_MIRROR`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:95` | `D3D11_TEXTURE_ADDRESS_MIRROR,` |

**`D3D11_TEXTURE_ADDRESS_MIRROR_ONCE`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:98` | `D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,` |

**`D3D11_TEXTURE_ADDRESS_MODE`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:92` | `D3D11_TEXTURE_ADDRESS_MODE DX11TextureAddressModes[] =` |
| `Bedrock/CoRE2/DX11Enums.h:14` | `extern D3D11_TEXTURE_ADDRESS_MODE DX11TextureAddressModes[];` |

**`D3D11_TEXTURE_ADDRESS_WRAP`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Enums.cpp:94` | `D3D11_TEXTURE_ADDRESS_WRAP,` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |
| `apEx/Phoenix/phxEngine.cpp:164` | `static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISO` |

**`D3D11_UAV_DIMENSION_TEXTURE3D`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:269` | `uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;` |

**`D3D11_UNORDERED_ACCESS_VIEW_DESC`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/phxEngine.cpp:265` | `D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;` |

**`D3D11_USAGE_DEFAULT`** ×16

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:683` | `desc.Usage = D3D11_USAGE_DEFAULT;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:742` | `desc.Usage = D3D11_USAGE_DEFAULT;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:815` | `desc.Usage = D3D11_USAGE_DEFAULT;` |
| `Bedrock/CoRE2/DX11Device.cpp:136` | `depthBufferDesc.Usage = D3D11_USAGE_DEFAULT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:233` | `desc.Usage = D3D11_USAGE_DEFAULT;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:44` | `bd.Usage = D3D11_USAGE_DEFAULT;` |
| `apEx/Phoenix/Mesh.cpp:464` | `D3D11_BUFFER_DESC bd = { (unsigned char*)LockedVertices - (unsigned char*)lv, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |
| `apEx/Phoenix/Project.cpp:96` | `static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE, ` |
| `apEx/Phoenix/Scene.cpp:761` | `D3D11_TEXTURE2D_DESC tex = { 2048,splinecnt,1,1,DXGI_FORMAT_R32_FLOAT,1,0,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0 };` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/Texgen.cpp:215` | `D3D11_TEXTURE2D_DESC tex = { XRes, YRes, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:162` | `static const D3D11_BUFFER_DESC bd = { 6 * 6 * sizeof( float ), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:232` | `textureDesc.Usage = D3D11_USAGE_DEFAULT;` |
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |
| `apEx/Phoenix/phxEngine.cpp:299` | `static const D3D11_TEXTURE2D_DESC ltctex16 = { 16, 16, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:303` | `static const D3D11_TEXTURE2D_DESC ltctex8 = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |

**`D3D11_USAGE_DYNAMIC`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11ConstantBuffer.cpp:32` | `desc.Usage = D3D11_USAGE_DYNAMIC;` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:43` | `bd.Usage = D3D11_USAGE_DYNAMIC;` |
| `Bedrock/CoRE2/DX11VertexBuffer.cpp:73` | `bd.Usage = D3D11_USAGE_DYNAMIC;` |
| `apEx/Phoenix/Project.cpp:1166` | `bd.Usage = D3D11_USAGE_DYNAMIC;` |
| `apEx/Phoenix/phxEngine.cpp:161` | `static D3D11_BUFFER_DESC desc = { 0, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };` |

**`D3D11_USAGE_IMMUTABLE`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Mesh.cpp:1741` | `treeDataBuffer.Usage = D3D11_USAGE_IMMUTABLE;` |

**`D3D11_USAGE_STAGING`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:974` | `desc.Usage = D3D11_USAGE_STAGING;` |
| `Bedrock/CoRE2/DX11Texture.cpp:991` | `else if ( ( desc.Usage == D3D11_USAGE_STAGING ) && ( desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ ) )` |
| `Bedrock/CoRE2/DX11Texture.cpp:1002` | `desc.Usage = D3D11_USAGE_STAGING;` |

**`D3D11_VIEWPORT`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:808` | `D3D11_VIEWPORT viewport;` |
| `Bedrock/CoRE2/DX11Device.cpp:809` | `memset(&viewport, 0, sizeof(D3D11_VIEWPORT));` |
| `apEx/Phoenix/RenderLayer.cpp:104` | `static D3D11_VIEWPORT viewports[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];` |
| `apEx/Phoenix/RenderTarget.cpp:6` | `D3D11_VIEWPORT v;` |
| `apEx/Phoenix/Timeline.cpp:369` | `D3D11_VIEWPORT v = { ( ScreenX - Target->XRes ) / 2.0f, ( ScreenY - Target->YRes ) / 2.0f, (float)Target->XRes, (float)Target->YRes, 0, 1 };` |

**`D3D_DRIVER_TYPE_HARDWARE`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:189` | `res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &Device, NULL, &Devi` |
| `Bedrock/CoRE2/DX11Device.cpp:196` | `res = D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &Device, NULL, &DeviceContext );` |
| `apEx/MinimalPlayer/WindowHandler.cpp:89` | `D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &phxDev, NULL, &phxContext );` |

**`D3D_FEATURE_LEVEL_9_1`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1024` | `case D3D_FEATURE_LEVEL_9_1:` |

**`D3D_FEATURE_LEVEL_9_2`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1025` | `case D3D_FEATURE_LEVEL_9_2:` |

**`D3D_FEATURE_LEVEL_9_3`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1038` | `case D3D_FEATURE_LEVEL_9_3:` |

**`D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1034` | `: D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;` |

**`D3D_FL9_1_REQ_TEXTURE3D_U_V_OR_W_DIMENSION`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1033` | `? D3D_FL9_1_REQ_TEXTURE3D_U_V_OR_W_DIMENSION` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1040` | `? D3D_FL9_1_REQ_TEXTURE3D_U_V_OR_W_DIMENSION` |

**`D3D_FL9_1_REQ_TEXTURECUBE_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1028` | `maxsize = D3D_FL9_1_REQ_TEXTURECUBE_DIMENSION;` |

**`D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:1041` | `: D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;` |

**`D3D_OK`** ×56

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX9Device.cpp:365` | `if (h == D3D_OK) return true;` |
| `Bedrock/CoRE2/DX9Device.cpp:519` | `case CORERS_CULLMODE: return Device->SetRenderState(D3DRS_CULLMODE, DX9CullModes[Value.CullMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:520` | `case CORERS_FILLMODE: return Device->SetRenderState(D3DRS_FILLMODE, DX9FillModes[Value.FillMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:521` | `case CORERS_ZENABLE: return Device->SetRenderState(D3DRS_ZENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:522` | `case CORERS_ZWRITEENABLE: return Device->SetRenderState(D3DRS_ZWRITEENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:523` | `case CORERS_ZFUNCT: return Device->SetRenderState(D3DRS_ZFUNC, DX9ComparisonFunctions[Value.ComparisonFunct]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:524` | `case CORERS_ALPHATESTENABLE: return Device->SetRenderState(D3DRS_ALPHATESTENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:525` | `case CORERS_ALPHAREFERENCE: return Device->SetRenderState(D3DRS_ALPHAREF, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:526` | `case CORERS_ALPHAFUNCT: return Device->SetRenderState(D3DRS_ALPHAFUNC, DX9ComparisonFunctions[Value.ComparisonFunct]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:527` | `case CORERS_BLENDENABLE: return Device->SetRenderState(D3DRS_ALPHABLENDENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:528` | `case CORERS_SRCBLEND: return Device->SetRenderState(D3DRS_SRCBLEND, DX9BlendFactors[Value.BlendFactor]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:529` | `case CORERS_DSTBLEND: return Device->SetRenderState(D3DRS_DESTBLEND, DX9BlendFactors[Value.BlendFactor]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:530` | `case CORERS_BLENDOP: return Device->SetRenderState(D3DRS_BLENDOP, DX9BlendOps[Value.BlendOp]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:531` | `case CORERS_STENCILENABLE: return Device->SetRenderState(D3DRS_STENCILENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:532` | `case CORERS_TWOSIDEDSTENCILENABLE: return Device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:533` | `case CORERS_SCISSORENABLE: return Device->SetRenderState(D3DRS_SCISSORTESTENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:534` | `case CORERS_AALINESENABLE: return Device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:537` | `if (!Value.VertexShader) return Device->SetVertexShader(NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:543` | `if (!Value.PixelShader) return Device->SetPixelShader(NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:548` | `if (!Value.Texture) return Device->SetTexture(DX9Samplers[Sampler], 0) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:551` | `case CORERS_MINFILTER: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_MINFILTER, DX9TextureFilters[Value.TextureFilter]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:552` | `case CORERS_MAGFILTER: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_MAGFILTER, DX9TextureFilters[Value.TextureFilter]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:553` | `case CORERS_MIPFILTER: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_MIPFILTER, DX9TextureFilters[Value.TextureFilter]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:554` | `case CORERS_ADDRESSU: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_ADDRESSU, DX9TextureAddressModes[Value.TextureAddressMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:555` | `case CORERS_ADDRESSV: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_ADDRESSV, DX9TextureAddressModes[Value.TextureAddressMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:556` | `case CORERS_ADDRESSW: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_ADDRESSW, DX9TextureAddressModes[Value.TextureAddressMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:557` | `case CORERS_MAXANISOTROPY: return Device->SetSamplerState(DX9Samplers[Sampler], D3DSAMP_MAXANISOTROPY, Value.Int) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:558` | `case CORERS_WRAPMODE: return Device->SetRenderState(DX9WrapModes[Sampler], DX9TextureWrapModes[Value.TextureWrapMode]) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:564` | `return Device->SetVertexDeclaration(NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:572` | `if (!Value.IndexBuffer) return Device->SetIndices(NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:581` | `return (Device->SetStreamSource(0, NULL, 0, 0) == D3D_OK);` |
| `Bedrock/CoRE2/DX9Device.cpp:594` | `return Device->BeginScene() == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:599` | `return Device->EndScene() == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:607` | `return Device->Clear(0, NULL, Flags, Color, Depth, Stencil) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:618` | `return Device->Present(&r, NULL, NULL, NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:620` | `return Device->Present(NULL, NULL, NULL, NULL) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:625` | `return ApplyRequestedRenderState() && Device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, NumVertices, 0, Count) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Device.cpp:630` | `return ApplyRequestedRenderState() && Device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, Count) == D3D_OK;` |
| `Bedrock/CoRE2/DX9IndexBuffer.cpp:28` | `return (Dev->SetIndices(IndexBufferHandle) == D3D_OK);` |
| `Bedrock/CoRE2/DX9IndexBuffer.cpp:51` | `if (Dev->CreateIndexBuffer(idxcount*idxsize, Usage, Format, Pool, &IndexBufferHandle, NULL) != D3D_OK)` |
| `Bedrock/CoRE2/DX9IndexBuffer.cpp:67` | `if (IndexBufferHandle->Lock(idxoffset*IndexSize, idxcount*IndexSize, &Data, 0) != D3D_OK)` |
| `Bedrock/CoRE2/DX9IndexBuffer.cpp:82` | `return IndexBufferHandle->Unlock() == D3D_OK;` |
| `Bedrock/CoRE2/DX9Texture.cpp:29` | `return Dev->SetTexture(DX9Samplers[smp], TextureHandle) == D3D_OK;` |
| `Bedrock/CoRE2/DX9Texture.cpp:47` | `if (Dev->CreateTexture(xres, yres, 0, Usage, txFormat, Pool, &TextureHandle, NULL) != D3D_OK) return false;` |
| `Bedrock/CoRE2/DX9Texture.cpp:49` | `if (TextureHandle->GetSurfaceLevel(0, &SurfaceHandle) != D3D_OK)` |
| `Bedrock/CoRE2/DX9Texture.cpp:74` | `if (D3DXCreateTextureFromFileInMemory(Dev, Data, Size, &TextureHandle) != D3D_OK)` |
| `Bedrock/CoRE2/DX9Texture.cpp:86` | `if (TextureHandle->GetSurfaceLevel(0, &SurfaceHandle) != D3D_OK)` |
| `Bedrock/CoRE2/DX9Texture.cpp:108` | `if (TextureHandle->LockRect(0, &Rect, NULL,                   0) != D3D_OK)` |
| `Bedrock/CoRE2/DX9Texture.cpp:120` | `return TextureHandle->UnlockRect(0) == D3D_OK;` |
| `Bedrock/CoRE2/DX9VertexBuffer.cpp:28` | `return (Dev->SetStreamSource(0, VertexBufferHandle, Offset, Device->GetVertexFormatSize()) == D3D_OK);` |
| `Bedrock/CoRE2/DX9VertexBuffer.cpp:48` | `if (Dev->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY \| D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &VertexBufferHandle, NULL) != D3D_OK)` |
| `Bedrock/CoRE2/DX9VertexBuffer.cpp:63` | `if (Dev->CreateVertexBuffer(Size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &VertexBufferHandle, NULL) != D3D_OK)` |
| `Bedrock/CoRE2/DX9VertexBuffer.cpp:96` | `if (VertexBufferHandle->Lock(Offset, size, &Data, Flags) != D3D_OK)` |
| `Bedrock/CoRE2/DX9VertexBuffer.cpp:111` | `return VertexBufferHandle->Unlock() == D3D_OK;` |
| `Bedrock/CoRE2/DX9VertexFormat.cpp:27` | `return (Dev->SetVertexDeclaration(VertexFormatHandle) == D3D_OK);` |
| `Bedrock/CoRE2/DX9VertexFormat.cpp:133` | `if (Dev->CreateVertexDeclaration(vxdecl, &VertexFormatHandle) != D3D_OK)` |

**`D3D_SDK_VERSION`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX9Device.cpp:205` | `D3D = Direct3DCreate9(D3D_SDK_VERSION);` |

**`D3D_SHADER_MACRO`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Shader.cpp:28` | `typedef HRESULT(__stdcall d3d_compile_func)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEnt` |
| `apEx/Phoenix/phxEngine.h:55` | `typedef HRESULT( __stdcall d3dcompilefunc )( LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEn` |
| `apEx/Phoenix_Tool/apxPhoenix.cpp:5` | `typedef HRESULT( __stdcall d3d_compile_func )( LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR p` |

**`D3D_SRV_DIMENSION_TEXTURE1D`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:707` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE1D;` |

**`D3D_SRV_DIMENSION_TEXTURE1DARRAY`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:701` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE1DARRAY;` |

**`D3D_SRV_DIMENSION_TEXTURE2D`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:782` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D;` |

**`D3D_SRV_DIMENSION_TEXTURE2DARRAY`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:776` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2DARRAY;` |

**`D3D_SRV_DIMENSION_TEXTURE3D`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:830` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE3D;` |

**`D3D_SRV_DIMENSION_TEXTURECUBE`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:770` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBE;` |

**`D3D_SRV_DIMENSION_TEXTURECUBEARRAY`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:762` | `SRVDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBEARRAY;` |

## DXGI symbols


**`DXGI_ENUM_MODES_INTERLACED`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/MinimalPlayer.cpp:210` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, NULL );` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:213` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, displayModeList );` |

**`DXGI_ERROR_DEVICE_REMOVED`** ×1

| file:line | source |
|---|---|
| `apEx/Phoenix/Project.cpp:1183` | `if ( res == DXGI_ERROR_DEVICE_REMOVED )` |

**`DXGI_FORMAT`** ×24

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:117` | `DXGI_FORMAT dxgiFormat;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:129` | `static size_t BitsPerPixel(_In_ DXGI_FORMAT fmt)` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:263` | `_In_ DXGI_FORMAT fmt,` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:359` | `static DXGI_FORMAT GetDXGIFormat(const DDS_PIXELFORMAT& ddpf)` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:571` | `_In_ DXGI_FORMAT format,` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:660` | `_In_ DXGI_FORMAT format,` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:878` | `DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;` |
| `Bedrock/CoRE2/DX11Enums.cpp:8` | `DXGI_FORMAT DX11Formats[] =` |
| `Bedrock/CoRE2/DX11Enums.cpp:164` | `COREFORMAT GetFormat(DXGI_FORMAT Format)` |
| `Bedrock/CoRE2/DX11Enums.h:7` | `extern DXGI_FORMAT DX11Formats[];` |
| `Bedrock/CoRE2/DX11Enums.h:19` | `COREFORMAT GetFormat(DXGI_FORMAT Format);` |
| `Bedrock/CoRE2/DX11Texture.cpp:355` | `DXGI_FORMAT              dxgiFormat;` |
| `Bedrock/CoRE2/DX11Texture.cpp:549` | `static DXGI_FORMAT EnsureNotTypeless( DXGI_FORMAT fmt )` |
| `Bedrock/CoRE2/DX11Texture.cpp:549` | `static DXGI_FORMAT EnsureNotTypeless( DXGI_FORMAT fmt )` |
| `Bedrock/CoRE2/DX11Texture.cpp:577` | `static size_t BitsPerPixel( _In_ DXGI_FORMAT fmt )` |
| `Bedrock/CoRE2/DX11Texture.cpp:738` | `_In_ DXGI_FORMAT fmt,` |
| `Bedrock/CoRE2/DX11Texture.cpp:875` | `static bool IsCompressed( _In_ DXGI_FORMAT fmt )` |
| `Bedrock/CoRE2/DX11Texture.cpp:944` | `DXGI_FORMAT fmt = EnsureNotTypeless( desc.Format );` |
| `Bedrock/CoRE2/DX11Texture.cpp:1116` | `DXGI_FORMAT     dxgiFormat;` |
| `apEx/Phoenix/Project.cpp:196` | `static void CreateRenderTarget( const DXGI_FORMAT Format, CphxRenderTarget& target )` |
| `apEx/Phoenix/Project.cpp:316` | `CreateRenderTarget( (DXGI_FORMAT)PixelFormats[ pFormat & 0x0f ], *Timeline.RenderTargets[ x ] );` |
| `apEx/Phoenix/phxEngine.cpp:109` | `void CreateRenderTarget( const int XRes, const int YRes, const DXGI_FORMAT Format, ID3D11ShaderResourceView *&View, ID3D11RenderTargetView *&RTView, ID3D11Texture2D *&Tex` |
| `apEx/Phoenix_Tool/RenderTargets.cpp:7` | `TBOOL CreateRenderTarget( TS32 XRes, TS32 YRes, DXGI_FORMAT Format, TS32 FormatSize, TBOOL AutoMipmap, ID3D11Texture2D *&Texture, ID3D11ShaderResourceView *&View, ID3D11R` |
| `apEx/Phoenix_Tool/RenderTargets.h:40` | `TBOOL CreateRenderTarget( TS32 XRes, TS32 YRes, DXGI_FORMAT Format, TS32 FormatSize, TBOOL AutoMipmap, ID3D11Texture2D *&Texture, ID3D11ShaderResourceView *&View, ID3D11R` |

**`DXGI_FORMAT_420_OPAQUE`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:675` | `case DXGI_FORMAT_420_OPAQUE:` |
| `Bedrock/CoRE2/DX11Texture.cpp:796` | `case DXGI_FORMAT_420_OPAQUE:` |

**`DXGI_FORMAT_A8P8`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:670` | `case DXGI_FORMAT_A8P8:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1233` | `case DXGI_FORMAT_A8P8:` |

**`DXGI_FORMAT_A8_UNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:220` | `case DXGI_FORMAT_A8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:465` | `return DXGI_FORMAT_A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:684` | `case DXGI_FORMAT_A8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1203` | `case DXGI_FORMAT_A8_UNORM:              memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A8, sizeof( DDS_PIXELFORMAT ) );          break;` |

**`DXGI_FORMAT_AI44`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:685` | `case DXGI_FORMAT_AI44:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1230` | `case DXGI_FORMAT_AI44:` |

**`DXGI_FORMAT_AYUV`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:647` | `case DXGI_FORMAT_AYUV:` |

**`DXGI_FORMAT_B4G4R4A4_UNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:212` | `case DXGI_FORMAT_B4G4R4A4_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:428` | `return DXGI_FORMAT_B4G4R4A4_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:671` | `case DXGI_FORMAT_B4G4R4A4_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1218` | `case DXGI_FORMAT_B4G4R4A4_UNORM:        memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A4R4G4B4, sizeof( DDS_PIXELFORMAT ) );    break;` |

**`DXGI_FORMAT_B5G5R5A1_UNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:211` | `case DXGI_FORMAT_B5G5R5A1_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:418` | `return DXGI_FORMAT_B5G5R5A1_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:669` | `case DXGI_FORMAT_B5G5R5A1_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1214` | `case DXGI_FORMAT_B5G5R5A1_UNORM:        memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A1R5G5B5, sizeof( DDS_PIXELFORMAT ) );    break;` |

**`DXGI_FORMAT_B5G6R5_UNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:210` | `case DXGI_FORMAT_B5G6R5_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:422` | `return DXGI_FORMAT_B5G6R5_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:668` | `case DXGI_FORMAT_B5G6R5_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1213` | `case DXGI_FORMAT_B5G6R5_UNORM:          memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_R5G6B5, sizeof( DDS_PIXELFORMAT ) );      break;` |

**`DXGI_FORMAT_B8G8R8A8_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:192` | `case DXGI_FORMAT_B8G8R8A8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:570` | `case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:643` | `case DXGI_FORMAT_B8G8R8A8_TYPELESS:` |

**`DXGI_FORMAT_B8G8R8A8_UNORM`** ×10

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:189` | `case DXGI_FORMAT_B8G8R8A8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:375` | `return DXGI_FORMAT_B8G8R8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Enums.cpp:11` | `DXGI_FORMAT_B8G8R8A8_UNORM,` |
| `Bedrock/CoRE2/DX11Enums.cpp:168` | `case DXGI_FORMAT_B8G8R8A8_UNORM: return COREFMT_A8R8G8B8;` |
| `Bedrock/CoRE2/DX11Texture.cpp:434` | `case DXGI_FORMAT_B8G8R8A8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:570` | `case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:640` | `case DXGI_FORMAT_B8G8R8A8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1215` | `case DXGI_FORMAT_B8G8R8A8_UNORM:        memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A8R8G8B8, sizeof( DDS_PIXELFORMAT ) );    break;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:100` | `vxdecl[x].Format = DXGI_FORMAT_B8G8R8A8_UNORM;` |
| `apEx/Phoenix/Texgen.cpp:215` | `D3D11_TEXTURE2D_DESC tex = { XRes, YRes, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |

**`DXGI_FORMAT_B8G8R8A8_UNORM_SRGB`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:193` | `case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:644` | `case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:` |
| `apEx/Phoenix/Texgen.cpp:242` | `tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;` |

**`DXGI_FORMAT_B8G8R8X8_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:194` | `case DXGI_FORMAT_B8G8R8X8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:571` | `case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:645` | `case DXGI_FORMAT_B8G8R8X8_TYPELESS:` |

**`DXGI_FORMAT_B8G8R8X8_UNORM`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:190` | `case DXGI_FORMAT_B8G8R8X8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:380` | `return DXGI_FORMAT_B8G8R8X8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:571` | `case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:641` | `case DXGI_FORMAT_B8G8R8X8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1216` | `case DXGI_FORMAT_B8G8R8X8_UNORM:        memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_X8R8G8B8, sizeof( DDS_PIXELFORMAT ) );    break;` |

**`DXGI_FORMAT_B8G8R8X8_UNORM_SRGB`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:195` | `case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:646` | `case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:` |

**`DXGI_FORMAT_BC1_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:226` | `case DXGI_FORMAT_BC1_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:278` | `case DXGI_FORMAT_BC1_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:565` | `case DXGI_FORMAT_BC1_TYPELESS:          return DXGI_FORMAT_BC1_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:693` | `case DXGI_FORMAT_BC1_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:753` | `case DXGI_FORMAT_BC1_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:879` | `case DXGI_FORMAT_BC1_TYPELESS:` |

**`DXGI_FORMAT_BC1_UNORM`** ×8

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:227` | `case DXGI_FORMAT_BC1_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:279` | `case DXGI_FORMAT_BC1_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:472` | `return DXGI_FORMAT_BC1_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:565` | `case DXGI_FORMAT_BC1_TYPELESS:          return DXGI_FORMAT_BC1_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:694` | `case DXGI_FORMAT_BC1_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:754` | `case DXGI_FORMAT_BC1_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:880` | `case DXGI_FORMAT_BC1_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1206` | `case DXGI_FORMAT_BC1_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_DXT1, sizeof( DDS_PIXELFORMAT ) );        break;` |

**`DXGI_FORMAT_BC1_UNORM_SRGB`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:228` | `case DXGI_FORMAT_BC1_UNORM_SRGB:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:280` | `case DXGI_FORMAT_BC1_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:695` | `case DXGI_FORMAT_BC1_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:755` | `case DXGI_FORMAT_BC1_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:881` | `case DXGI_FORMAT_BC1_UNORM_SRGB:` |

**`DXGI_FORMAT_BC2_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:234` | `case DXGI_FORMAT_BC2_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:288` | `case DXGI_FORMAT_BC2_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:566` | `case DXGI_FORMAT_BC2_TYPELESS:          return DXGI_FORMAT_BC2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:701` | `case DXGI_FORMAT_BC2_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:763` | `case DXGI_FORMAT_BC2_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:882` | `case DXGI_FORMAT_BC2_TYPELESS:` |

**`DXGI_FORMAT_BC2_UNORM`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:235` | `case DXGI_FORMAT_BC2_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:289` | `case DXGI_FORMAT_BC2_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:476` | `return DXGI_FORMAT_BC2_UNORM;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:487` | `return DXGI_FORMAT_BC2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:566` | `case DXGI_FORMAT_BC2_TYPELESS:          return DXGI_FORMAT_BC2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:702` | `case DXGI_FORMAT_BC2_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:764` | `case DXGI_FORMAT_BC2_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:883` | `case DXGI_FORMAT_BC2_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1207` | `case DXGI_FORMAT_BC2_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_DXT3, sizeof( DDS_PIXELFORMAT ) );        break;` |

**`DXGI_FORMAT_BC2_UNORM_SRGB`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:236` | `case DXGI_FORMAT_BC2_UNORM_SRGB:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:290` | `case DXGI_FORMAT_BC2_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:703` | `case DXGI_FORMAT_BC2_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:765` | `case DXGI_FORMAT_BC2_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:884` | `case DXGI_FORMAT_BC2_UNORM_SRGB:` |

**`DXGI_FORMAT_BC3_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:237` | `case DXGI_FORMAT_BC3_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:291` | `case DXGI_FORMAT_BC3_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:567` | `case DXGI_FORMAT_BC3_TYPELESS:          return DXGI_FORMAT_BC3_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:704` | `case DXGI_FORMAT_BC3_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:766` | `case DXGI_FORMAT_BC3_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:885` | `case DXGI_FORMAT_BC3_TYPELESS:` |

**`DXGI_FORMAT_BC3_UNORM`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:238` | `case DXGI_FORMAT_BC3_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:292` | `case DXGI_FORMAT_BC3_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:480` | `return DXGI_FORMAT_BC3_UNORM;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:491` | `return DXGI_FORMAT_BC3_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:567` | `case DXGI_FORMAT_BC3_TYPELESS:          return DXGI_FORMAT_BC3_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:705` | `case DXGI_FORMAT_BC3_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:767` | `case DXGI_FORMAT_BC3_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:886` | `case DXGI_FORMAT_BC3_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1208` | `case DXGI_FORMAT_BC3_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_DXT5, sizeof( DDS_PIXELFORMAT ) );        break;` |

**`DXGI_FORMAT_BC3_UNORM_SRGB`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:239` | `case DXGI_FORMAT_BC3_UNORM_SRGB:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:293` | `case DXGI_FORMAT_BC3_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:706` | `case DXGI_FORMAT_BC3_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:768` | `case DXGI_FORMAT_BC3_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:887` | `case DXGI_FORMAT_BC3_UNORM_SRGB:` |

**`DXGI_FORMAT_BC4_SNORM`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:231` | `case DXGI_FORMAT_BC4_SNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:283` | `case DXGI_FORMAT_BC4_SNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:504` | `return DXGI_FORMAT_BC4_SNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:698` | `case DXGI_FORMAT_BC4_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:758` | `case DXGI_FORMAT_BC4_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:890` | `case DXGI_FORMAT_BC4_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1210` | `case DXGI_FORMAT_BC4_SNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_BC4_SNORM, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_BC4_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:229` | `case DXGI_FORMAT_BC4_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:281` | `case DXGI_FORMAT_BC4_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:568` | `case DXGI_FORMAT_BC4_TYPELESS:          return DXGI_FORMAT_BC4_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:696` | `case DXGI_FORMAT_BC4_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:756` | `case DXGI_FORMAT_BC4_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:888` | `case DXGI_FORMAT_BC4_TYPELESS:` |

**`DXGI_FORMAT_BC4_UNORM`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:230` | `case DXGI_FORMAT_BC4_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:282` | `case DXGI_FORMAT_BC4_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:496` | `return DXGI_FORMAT_BC4_UNORM;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:500` | `return DXGI_FORMAT_BC4_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:568` | `case DXGI_FORMAT_BC4_TYPELESS:          return DXGI_FORMAT_BC4_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:697` | `case DXGI_FORMAT_BC4_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:757` | `case DXGI_FORMAT_BC4_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:889` | `case DXGI_FORMAT_BC4_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1209` | `case DXGI_FORMAT_BC4_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_BC4_UNORM, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_BC5_SNORM`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:242` | `case DXGI_FORMAT_BC5_SNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:296` | `case DXGI_FORMAT_BC5_SNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:517` | `return DXGI_FORMAT_BC5_SNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:709` | `case DXGI_FORMAT_BC5_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:771` | `case DXGI_FORMAT_BC5_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:893` | `case DXGI_FORMAT_BC5_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1212` | `case DXGI_FORMAT_BC5_SNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_BC5_SNORM, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_BC5_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:240` | `case DXGI_FORMAT_BC5_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:294` | `case DXGI_FORMAT_BC5_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:569` | `case DXGI_FORMAT_BC5_TYPELESS:          return DXGI_FORMAT_BC5_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:707` | `case DXGI_FORMAT_BC5_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:769` | `case DXGI_FORMAT_BC5_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:891` | `case DXGI_FORMAT_BC5_TYPELESS:` |

**`DXGI_FORMAT_BC5_UNORM`** ×9

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:241` | `case DXGI_FORMAT_BC5_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:295` | `case DXGI_FORMAT_BC5_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:509` | `return DXGI_FORMAT_BC5_UNORM;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:513` | `return DXGI_FORMAT_BC5_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:569` | `case DXGI_FORMAT_BC5_TYPELESS:          return DXGI_FORMAT_BC5_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:708` | `case DXGI_FORMAT_BC5_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:770` | `case DXGI_FORMAT_BC5_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:892` | `case DXGI_FORMAT_BC5_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1211` | `case DXGI_FORMAT_BC5_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_BC5_UNORM, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_BC6H_SF16`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:245` | `case DXGI_FORMAT_BC6H_SF16:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:299` | `case DXGI_FORMAT_BC6H_SF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:712` | `case DXGI_FORMAT_BC6H_SF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:774` | `case DXGI_FORMAT_BC6H_SF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:896` | `case DXGI_FORMAT_BC6H_SF16:` |

**`DXGI_FORMAT_BC6H_TYPELESS`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:243` | `case DXGI_FORMAT_BC6H_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:297` | `case DXGI_FORMAT_BC6H_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:710` | `case DXGI_FORMAT_BC6H_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:772` | `case DXGI_FORMAT_BC6H_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:894` | `case DXGI_FORMAT_BC6H_TYPELESS:` |

**`DXGI_FORMAT_BC6H_UF16`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:244` | `case DXGI_FORMAT_BC6H_UF16:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:298` | `case DXGI_FORMAT_BC6H_UF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:711` | `case DXGI_FORMAT_BC6H_UF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:773` | `case DXGI_FORMAT_BC6H_UF16:` |
| `Bedrock/CoRE2/DX11Texture.cpp:895` | `case DXGI_FORMAT_BC6H_UF16:` |

**`DXGI_FORMAT_BC7_TYPELESS`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:246` | `case DXGI_FORMAT_BC7_TYPELESS:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:300` | `case DXGI_FORMAT_BC7_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:572` | `case DXGI_FORMAT_BC7_TYPELESS:          return DXGI_FORMAT_BC7_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:713` | `case DXGI_FORMAT_BC7_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:775` | `case DXGI_FORMAT_BC7_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:897` | `case DXGI_FORMAT_BC7_TYPELESS:` |

**`DXGI_FORMAT_BC7_UNORM`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:247` | `case DXGI_FORMAT_BC7_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:301` | `case DXGI_FORMAT_BC7_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:572` | `case DXGI_FORMAT_BC7_TYPELESS:          return DXGI_FORMAT_BC7_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:714` | `case DXGI_FORMAT_BC7_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:776` | `case DXGI_FORMAT_BC7_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:898` | `case DXGI_FORMAT_BC7_UNORM:` |

**`DXGI_FORMAT_BC7_UNORM_SRGB`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:248` | `case DXGI_FORMAT_BC7_UNORM_SRGB:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:302` | `case DXGI_FORMAT_BC7_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:715` | `case DXGI_FORMAT_BC7_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:777` | `case DXGI_FORMAT_BC7_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:899` | `case DXGI_FORMAT_BC7_UNORM_SRGB:` |

**`DXGI_FORMAT_D16_UNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:205` | `case DXGI_FORMAT_D16_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:663` | `case DXGI_FORMAT_D16_UNORM:` |

**`DXGI_FORMAT_D16_UNORM_S8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:724` | `case DXGI_FORMAT_D16_UNORM_S8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:809` | `case DXGI_FORMAT_D16_UNORM_S8_UINT:` |

**`DXGI_FORMAT_D24_UNORM_S8_UINT`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:183` | `case DXGI_FORMAT_D24_UNORM_S8_UINT:` |
| `Bedrock/CoRE2/DX11Device.cpp:133` | `depthBufferDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;` |
| `Bedrock/CoRE2/DX11Device.cpp:153` | `depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:503` | `deptdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:634` | `case DXGI_FORMAT_D24_UNORM_S8_UINT:` |
| `apEx/Phoenix/Project.cpp:94` | `static D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = { DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, 0 };` |

**`DXGI_FORMAT_D32_FLOAT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:178` | `case DXGI_FORMAT_D32_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:629` | `case DXGI_FORMAT_D32_FLOAT:` |

**`DXGI_FORMAT_D32_FLOAT_S8X24_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:156` | `case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:604` | `case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:` |

**`DXGI_FORMAT_G8R8_G8B8_UNORM`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:188` | `case DXGI_FORMAT_G8R8_G8B8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:308` | `case DXGI_FORMAT_G8R8_G8B8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:528` | `return DXGI_FORMAT_G8R8_G8B8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:639` | `case DXGI_FORMAT_G8R8_G8B8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:783` | `case DXGI_FORMAT_G8R8_G8B8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1205` | `case DXGI_FORMAT_G8R8_G8B8_UNORM:       memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_G8R8_G8B8, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_IA44`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:686` | `case DXGI_FORMAT_IA44:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1231` | `case DXGI_FORMAT_IA44:` |

**`DXGI_FORMAT_NV11`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:676` | `case DXGI_FORMAT_NV11:` |
| `Bedrock/CoRE2/DX11Texture.cpp:841` | `else if ( fmt == DXGI_FORMAT_NV11 )` |

**`DXGI_FORMAT_NV12`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:674` | `case DXGI_FORMAT_NV12:` |
| `Bedrock/CoRE2/DX11Texture.cpp:795` | `case DXGI_FORMAT_NV12:` |

**`DXGI_FORMAT_P010`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:652` | `case DXGI_FORMAT_P010:` |
| `Bedrock/CoRE2/DX11Texture.cpp:801` | `case DXGI_FORMAT_P010:` |

**`DXGI_FORMAT_P016`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:653` | `case DXGI_FORMAT_P016:` |
| `Bedrock/CoRE2/DX11Texture.cpp:802` | `case DXGI_FORMAT_P016:` |

**`DXGI_FORMAT_P8`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:687` | `case DXGI_FORMAT_P8:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1232` | `case DXGI_FORMAT_P8:` |

**`DXGI_FORMAT_R10G10B10A2_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:161` | `case DXGI_FORMAT_R10G10B10A2_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:558` | `case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:612` | `case DXGI_FORMAT_R10G10B10A2_TYPELESS:` |

**`DXGI_FORMAT_R10G10B10A2_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:163` | `case DXGI_FORMAT_R10G10B10A2_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:614` | `case DXGI_FORMAT_R10G10B10A2_UINT:` |

**`DXGI_FORMAT_R10G10B10A2_UNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:162` | `case DXGI_FORMAT_R10G10B10A2_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:394` | `return DXGI_FORMAT_R10G10B10A2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:558` | `case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:613` | `case DXGI_FORMAT_R10G10B10A2_UNORM:` |

**`DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:721` | `case DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT:` |

**`DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:720` | `case DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT:` |

**`DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:191` | `case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:642` | `case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:` |

**`DXGI_FORMAT_R11G11B10_FLOAT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:164` | `case DXGI_FORMAT_R11G11B10_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:615` | `case DXGI_FORMAT_R11G11B10_FLOAT:` |

**`DXGI_FORMAT_R16G16B16A16_FLOAT`** ×13

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:146` | `case DXGI_FORMAT_R16G16B16A16_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:547` | `return DXGI_FORMAT_R16G16B16A16_FLOAT;` |
| `Bedrock/CoRE2/DX11Enums.cpp:15` | `DXGI_FORMAT_R16G16B16A16_FLOAT,` |
| `Bedrock/CoRE2/DX11Texture.cpp:594` | `case DXGI_FORMAT_R16G16B16A16_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1222` | `case DXGI_FORMAT_R16G16B16A16_FLOAT:    header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 113; break;` |
| `apEx/Phoenix/Project.cpp:162` | `DXGI_FORMAT_R16G16B16A16_FLOAT,` |
| `apEx/Phoenix/Project.cpp:305` | `CreateRenderTarget( DXGI_FORMAT_R16G16B16A16_FLOAT, *phxInternalRenderTarget );` |
| `apEx/Phoenix/Texgen.cpp:20` | `tex.Format = texgenrt.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `apEx/Phoenix/phxEngine.cpp:231` | `textureDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;` |
| `apEx/Phoenix/phxEngine.cpp:246` | `resourceViewDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;` |
| `apEx/Phoenix/phxEngine.cpp:299` | `static const D3D11_TEXTURE2D_DESC ltctex16 = { 16, 16, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:300` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview16 = { DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |
| `apEx/Phoenix_Tool/Texgen_tool.cpp:152` | `return CreateRenderTarget( XRes, YRes, hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM, TEXTUREFORMATSIZE, true, Texture, View, RTView ) != 0;` |

**`DXGI_FORMAT_R16G16B16A16_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:150` | `case DXGI_FORMAT_R16G16B16A16_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:598` | `case DXGI_FORMAT_R16G16B16A16_SINT:` |

**`DXGI_FORMAT_R16G16B16A16_SNORM`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:149` | `case DXGI_FORMAT_R16G16B16A16_SNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:538` | `return DXGI_FORMAT_R16G16B16A16_SNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:597` | `case DXGI_FORMAT_R16G16B16A16_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1224` | `case DXGI_FORMAT_R16G16B16A16_SNORM:    header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 110; break;` |

**`DXGI_FORMAT_R16G16B16A16_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:145` | `case DXGI_FORMAT_R16G16B16A16_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:556` | `case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:593` | `case DXGI_FORMAT_R16G16B16A16_TYPELESS:` |

**`DXGI_FORMAT_R16G16B16A16_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:148` | `case DXGI_FORMAT_R16G16B16A16_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:596` | `case DXGI_FORMAT_R16G16B16A16_UINT:` |

**`DXGI_FORMAT_R16G16B16A16_UNORM`** ×10

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:147` | `case DXGI_FORMAT_R16G16B16A16_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:535` | `return DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:556` | `case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:595` | `case DXGI_FORMAT_R16G16B16A16_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1223` | `case DXGI_FORMAT_R16G16B16A16_UNORM:    header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 36;  break;` |
| `apEx/Phoenix/Texgen.cpp:15` | `static D3D11_RENDER_TARGET_VIEW_DESC texgenrt = { DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D, 0 };` |
| `apEx/Phoenix/Texgen.cpp:16` | `static D3D11_TEXTURE2D_DESC tex = { 0, 0, 0, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE \| D3D11_BIND_RENDER_TARGET, 0, D3D11` |
| `apEx/Phoenix/Texgen.cpp:20` | `tex.Format = texgenrt.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `apEx/Phoenix/Texgen.cpp:308` | `tex.Format = DXGI_FORMAT_R16G16B16A16_UNORM;` |
| `apEx/Phoenix_Tool/Texgen_tool.cpp:152` | `return CreateRenderTarget( XRes, YRes, hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM, TEXTUREFORMATSIZE, true, Texture, View, RTView ) != 0;` |

**`DXGI_FORMAT_R16G16_FLOAT`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:172` | `case DXGI_FORMAT_R16G16_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:544` | `return DXGI_FORMAT_R16G16_FLOAT;` |
| `Bedrock/CoRE2/DX11Enums.cpp:14` | `DXGI_FORMAT_R16G16_FLOAT,` |
| `Bedrock/CoRE2/DX11Enums.cpp:171` | `case DXGI_FORMAT_R16G16_FLOAT: return COREFMT_G16R16F;` |
| `Bedrock/CoRE2/DX11Texture.cpp:623` | `case DXGI_FORMAT_R16G16_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1226` | `case DXGI_FORMAT_R16G16_FLOAT:          header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 112; break;` |

**`DXGI_FORMAT_R16G16_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:176` | `case DXGI_FORMAT_R16G16_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:627` | `case DXGI_FORMAT_R16G16_SINT:` |

**`DXGI_FORMAT_R16G16_SNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:175` | `case DXGI_FORMAT_R16G16_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:626` | `case DXGI_FORMAT_R16G16_SNORM:` |

**`DXGI_FORMAT_R16G16_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:171` | `case DXGI_FORMAT_R16G16_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:560` | `case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:622` | `case DXGI_FORMAT_R16G16_TYPELESS:` |

**`DXGI_FORMAT_R16G16_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:174` | `case DXGI_FORMAT_R16G16_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:625` | `case DXGI_FORMAT_R16G16_UINT:` |

**`DXGI_FORMAT_R16G16_UNORM`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:173` | `case DXGI_FORMAT_R16G16_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:401` | `return DXGI_FORMAT_R16G16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:560` | `case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:624` | `case DXGI_FORMAT_R16G16_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1199` | `case DXGI_FORMAT_R16G16_UNORM:          memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_G16R16, sizeof( DDS_PIXELFORMAT ) );      break;` |

**`DXGI_FORMAT_R16_FLOAT`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:204` | `case DXGI_FORMAT_R16_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:541` | `return DXGI_FORMAT_R16_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:662` | `case DXGI_FORMAT_R16_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1228` | `case DXGI_FORMAT_R16_FLOAT:             header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 111; break;` |

**`DXGI_FORMAT_R16_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:209` | `case DXGI_FORMAT_R16_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:667` | `case DXGI_FORMAT_R16_SINT:` |

**`DXGI_FORMAT_R16_SNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:208` | `case DXGI_FORMAT_R16_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:666` | `case DXGI_FORMAT_R16_SNORM:` |

**`DXGI_FORMAT_R16_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:203` | `case DXGI_FORMAT_R16_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:563` | `case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:661` | `case DXGI_FORMAT_R16_TYPELESS:` |

**`DXGI_FORMAT_R16_UINT`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:207` | `case DXGI_FORMAT_R16_UINT:` |
| `Bedrock/CoRE2/DX11Device.cpp:633` | `DeviceContext->IASetIndexBuffer(NULL, DXGI_FORMAT_R16_UINT, 0);` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:29` | `DeviceContext->IASetIndexBuffer(IndexBufferHandle, IndexSize == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);` |
| `Bedrock/CoRE2/DX11Texture.cpp:665` | `case DXGI_FORMAT_R16_UINT:` |

**`DXGI_FORMAT_R16_UNORM`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:206` | `case DXGI_FORMAT_R16_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:453` | `return DXGI_FORMAT_R16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:563` | `case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:664` | `case DXGI_FORMAT_R16_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1201` | `case DXGI_FORMAT_R16_UNORM:             memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_L16, sizeof( DDS_PIXELFORMAT ) );         break;` |

**`DXGI_FORMAT_R16_UNORM_X8_TYPELESS`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:725` | `case DXGI_FORMAT_R16_UNORM_X8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:810` | `case DXGI_FORMAT_R16_UNORM_X8_TYPELESS:` |

**`DXGI_FORMAT_R1_UNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:223` | `case DXGI_FORMAT_R1_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:690` | `case DXGI_FORMAT_R1_UNORM:` |

**`DXGI_FORMAT_R24G8_TYPELESS`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:182` | `case DXGI_FORMAT_R24G8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:487` | `tex.Format = DXGI_FORMAT_R24G8_TYPELESS;` |
| `Bedrock/CoRE2/DX11Texture.cpp:633` | `case DXGI_FORMAT_R24G8_TYPELESS:` |
| `apEx/Phoenix/Project.cpp:96` | `static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL \| D3D11_BIND_SHADER_RESOURCE, ` |

**`DXGI_FORMAT_R24_UNORM_X8_TYPELESS`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:184` | `case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:518` | `resdesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;` |
| `Bedrock/CoRE2/DX11Texture.cpp:635` | `case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:` |
| `apEx/Phoenix/Project.cpp:95` | `static D3D11_SHADER_RESOURCE_VIEW_DESC resdesc = { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |

**`DXGI_FORMAT_R32G32B32A32_FLOAT`** ×16

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:134` | `case DXGI_FORMAT_R32G32B32A32_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:556` | `return DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11Enums.cpp:12` | `DXGI_FORMAT_R32G32B32A32_FLOAT,` |
| `Bedrock/CoRE2/DX11Enums.cpp:169` | `case DXGI_FORMAT_R32G32B32A32_FLOAT: return COREFMT_A32B32G32R32F;` |
| `Bedrock/CoRE2/DX11Texture.cpp:554` | `case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:582` | `case DXGI_FORMAT_R32G32B32A32_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1221` | `case DXGI_FORMAT_R32G32B32A32_FLOAT:    header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 116; break;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:68` | `vxdecl[x].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:92` | `vxdecl[ x ].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:108` | `vxdecl[x].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:116` | `vxdecl[x].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `apEx/Phoenix/phxEngine.cpp:126` | `{ "PositionT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:132` | `{ "Position", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:133` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |
| `apEx/Phoenix/phxEngine.cpp:142` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 10, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:143` | `{ "Texcoord", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof( float ) * 14, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |

**`DXGI_FORMAT_R32G32B32A32_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:136` | `case DXGI_FORMAT_R32G32B32A32_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:584` | `case DXGI_FORMAT_R32G32B32A32_SINT:` |

**`DXGI_FORMAT_R32G32B32A32_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:133` | `case DXGI_FORMAT_R32G32B32A32_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:554` | `case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:581` | `case DXGI_FORMAT_R32G32B32A32_TYPELESS:` |

**`DXGI_FORMAT_R32G32B32A32_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:135` | `case DXGI_FORMAT_R32G32B32A32_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:583` | `case DXGI_FORMAT_R32G32B32A32_UINT:` |

**`DXGI_FORMAT_R32G32B32_FLOAT`** ×8

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:140` | `case DXGI_FORMAT_R32G32B32_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:555` | `case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:588` | `case DXGI_FORMAT_R32G32B32_FLOAT:` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:60` | `vxdecl[x].Format = DXGI_FORMAT_R32G32B32_FLOAT;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:76` | `vxdecl[x].Format = DXGI_FORMAT_R32G32B32_FLOAT;` |
| `apEx/Phoenix/phxEngine.cpp:138` | `{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:139` | `{ "Position", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof( float ) * 3, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:140` | `{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof( float ) * 6, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |

**`DXGI_FORMAT_R32G32B32_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:142` | `case DXGI_FORMAT_R32G32B32_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:590` | `case DXGI_FORMAT_R32G32B32_SINT:` |

**`DXGI_FORMAT_R32G32B32_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:139` | `case DXGI_FORMAT_R32G32B32_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:555` | `case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:587` | `case DXGI_FORMAT_R32G32B32_TYPELESS:` |

**`DXGI_FORMAT_R32G32B32_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:141` | `case DXGI_FORMAT_R32G32B32_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:589` | `case DXGI_FORMAT_R32G32B32_UINT:` |

**`DXGI_FORMAT_R32G32_FLOAT`** ×7

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:152` | `case DXGI_FORMAT_R32G32_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:553` | `return DXGI_FORMAT_R32G32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:557` | `case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:600` | `case DXGI_FORMAT_R32G32_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1225` | `case DXGI_FORMAT_R32G32_FLOAT:          header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 115; break;` |
| `Bedrock/CoRE2/DX11VertexFormat.cpp:84` | `vxdecl[x].Format = DXGI_FORMAT_R32G32_FLOAT;` |
| `apEx/Phoenix/phxEngine.cpp:127` | `{ "Texcoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof( float ) * 4, D3D11_INPUT_PER_VERTEX_DATA, 0 }` |

**`DXGI_FORMAT_R32G32_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:154` | `case DXGI_FORMAT_R32G32_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:602` | `case DXGI_FORMAT_R32G32_SINT:` |

**`DXGI_FORMAT_R32G32_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:151` | `case DXGI_FORMAT_R32G32_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:557` | `case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:599` | `case DXGI_FORMAT_R32G32_TYPELESS:` |

**`DXGI_FORMAT_R32G32_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:153` | `case DXGI_FORMAT_R32G32_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:601` | `case DXGI_FORMAT_R32G32_UINT:` |

**`DXGI_FORMAT_R32G8X24_TYPELESS`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:155` | `case DXGI_FORMAT_R32G8X24_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:603` | `case DXGI_FORMAT_R32G8X24_TYPELESS:` |

**`DXGI_FORMAT_R32_FLOAT`** ×10

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:179` | `case DXGI_FORMAT_R32_FLOAT:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:407` | `return DXGI_FORMAT_R32_FLOAT;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:550` | `return DXGI_FORMAT_R32_FLOAT;` |
| `Bedrock/CoRE2/DX11Enums.cpp:13` | `DXGI_FORMAT_R32_FLOAT,` |
| `Bedrock/CoRE2/DX11Enums.cpp:170` | `case DXGI_FORMAT_R32_FLOAT: return COREFMT_R32F;` |
| `Bedrock/CoRE2/DX11Texture.cpp:561` | `case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:630` | `case DXGI_FORMAT_R32_FLOAT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1227` | `case DXGI_FORMAT_R32_FLOAT:             header->ddspf.size = sizeof( DDS_PIXELFORMAT ); header->ddspf.flags = DDS_FOURCC; header->ddspf.fourCC = 114; break;` |
| `apEx/Phoenix/Project.cpp:163` | `DXGI_FORMAT_R32_FLOAT,` |
| `apEx/Phoenix/Scene.cpp:761` | `D3D11_TEXTURE2D_DESC tex = { 2048,splinecnt,1,1,DXGI_FORMAT_R32_FLOAT,1,0,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0 };` |

**`DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:157` | `case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:605` | `case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:` |

**`DXGI_FORMAT_R32_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:181` | `case DXGI_FORMAT_R32_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:632` | `case DXGI_FORMAT_R32_SINT:` |

**`DXGI_FORMAT_R32_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:177` | `case DXGI_FORMAT_R32_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:561` | `case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;` |
| `Bedrock/CoRE2/DX11Texture.cpp:628` | `case DXGI_FORMAT_R32_TYPELESS:` |

**`DXGI_FORMAT_R32_UINT`** ×4

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:180` | `case DXGI_FORMAT_R32_UINT:` |
| `Bedrock/CoRE2/DX11IndexBuffer.cpp:29` | `DeviceContext->IASetIndexBuffer(IndexBufferHandle, IndexSize == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);` |
| `Bedrock/CoRE2/DX11Texture.cpp:631` | `case DXGI_FORMAT_R32_UINT:` |
| `apEx/Phoenix/RenderLayer.cpp:88` | `phxContext->IASetIndexBuffer( Wireframe ? WireBuffer : IndexBuffer, DXGI_FORMAT_R32_UINT, 0 );` |

**`DXGI_FORMAT_R8G8B8A8_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:170` | `case DXGI_FORMAT_R8G8B8A8_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:621` | `case DXGI_FORMAT_R8G8B8A8_SINT:` |

**`DXGI_FORMAT_R8G8B8A8_SNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:169` | `case DXGI_FORMAT_R8G8B8A8_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:620` | `case DXGI_FORMAT_R8G8B8A8_SNORM:` |

**`DXGI_FORMAT_R8G8B8A8_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:165` | `case DXGI_FORMAT_R8G8B8A8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:559` | `case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:616` | `case DXGI_FORMAT_R8G8B8A8_TYPELESS:` |

**`DXGI_FORMAT_R8G8B8A8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:168` | `case DXGI_FORMAT_R8G8B8A8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:619` | `case DXGI_FORMAT_R8G8B8A8_UINT:` |

**`DXGI_FORMAT_R8G8B8A8_UNORM`** ×14

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:166` | `case DXGI_FORMAT_R8G8B8A8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:370` | `return DXGI_FORMAT_R8G8B8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Device.cpp:179` | `scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:559` | `case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:617` | `case DXGI_FORMAT_R8G8B8A8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1198` | `case DXGI_FORMAT_R8G8B8A8_UNORM:        memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A8B8G8R8, sizeof( DDS_PIXELFORMAT ) );    break;` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:210` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, NULL );` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:213` | `result = adapterOutput->GetDisplayModeList( DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, displayModeList );` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |
| `apEx/Phoenix/phxEngine.cpp:141` | `{ "Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, sizeof( float ) * 9, D3D11_INPUT_PER_VERTEX_DATA, 0 },` |
| `apEx/Phoenix/phxEngine.cpp:276` | `static const D3D11_TEXTURE2D_DESC tex = { Texture3DSize, Texture3DSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 8, D3D11_STANDARD_MULTISAMPLE_PATTERN, D3D11_USAGE_DEFAULT, D3D1` |
| `apEx/Phoenix/phxEngine.cpp:277` | `static const D3D11_RENDER_TARGET_VIEW_DESC rt = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:303` | `static const D3D11_TEXTURE2D_DESC ltctex8 = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };` |
| `apEx/Phoenix/phxEngine.cpp:304` | `static const D3D11_SHADER_RESOURCE_VIEW_DESC ltcview8 = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };` |

**`DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:167` | `case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:` |
| `Bedrock/CoRE2/DX11Texture.cpp:618` | `case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:` |
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |

**`DXGI_FORMAT_R8G8_B8G8_UNORM`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:187` | `case DXGI_FORMAT_R8G8_B8G8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:307` | `case DXGI_FORMAT_R8G8_B8G8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:524` | `return DXGI_FORMAT_R8G8_B8G8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:638` | `case DXGI_FORMAT_R8G8_B8G8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:782` | `case DXGI_FORMAT_R8G8_B8G8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1204` | `case DXGI_FORMAT_R8G8_B8G8_UNORM:       memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_R8G8_B8G8, sizeof( DDS_PIXELFORMAT ) );   break;` |

**`DXGI_FORMAT_R8G8_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:202` | `case DXGI_FORMAT_R8G8_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:660` | `case DXGI_FORMAT_R8G8_SINT:` |

**`DXGI_FORMAT_R8G8_SNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:201` | `case DXGI_FORMAT_R8G8_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:659` | `case DXGI_FORMAT_R8G8_SNORM:` |

**`DXGI_FORMAT_R8G8_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:198` | `case DXGI_FORMAT_R8G8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:562` | `case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:656` | `case DXGI_FORMAT_R8G8_TYPELESS:` |

**`DXGI_FORMAT_R8G8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:200` | `case DXGI_FORMAT_R8G8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:658` | `case DXGI_FORMAT_R8G8_UINT:` |

**`DXGI_FORMAT_R8G8_UNORM`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:199` | `case DXGI_FORMAT_R8G8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:457` | `return DXGI_FORMAT_R8G8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:562` | `case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:657` | `case DXGI_FORMAT_R8G8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1200` | `case DXGI_FORMAT_R8G8_UNORM:            memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_A8L8, sizeof( DDS_PIXELFORMAT ) );        break;` |

**`DXGI_FORMAT_R8_SINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:219` | `case DXGI_FORMAT_R8_SINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:683` | `case DXGI_FORMAT_R8_SINT:` |

**`DXGI_FORMAT_R8_SNORM`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:218` | `case DXGI_FORMAT_R8_SNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:682` | `case DXGI_FORMAT_R8_SNORM:` |

**`DXGI_FORMAT_R8_TYPELESS`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:215` | `case DXGI_FORMAT_R8_TYPELESS:` |
| `Bedrock/CoRE2/DX11Texture.cpp:564` | `case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:679` | `case DXGI_FORMAT_R8_TYPELESS:` |

**`DXGI_FORMAT_R8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:217` | `case DXGI_FORMAT_R8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:681` | `case DXGI_FORMAT_R8_UINT:` |

**`DXGI_FORMAT_R8_UNORM`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:216` | `case DXGI_FORMAT_R8_UNORM:` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:443` | `return DXGI_FORMAT_R8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:564` | `case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;` |
| `Bedrock/CoRE2/DX11Texture.cpp:680` | `case DXGI_FORMAT_R8_UNORM:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1202` | `case DXGI_FORMAT_R8_UNORM:              memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_L8, sizeof( DDS_PIXELFORMAT ) );          break;` |

**`DXGI_FORMAT_R9G9B9E5_SHAREDEXP`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:186` | `case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:` |
| `Bedrock/CoRE2/DX11Texture.cpp:637` | `case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:` |

**`DXGI_FORMAT_UNKNOWN`** ×6

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:560` | `return DXGI_FORMAT_UNKNOWN;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:878` | `DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;` |
| `Bedrock/CoRE2/DDSTextureLoader.cpp:930` | `if (format == DXGI_FORMAT_UNKNOWN)` |
| `Bedrock/CoRE2/DX11Device.cpp:288` | `res = SwapChain->ResizeBuffers(desc.BufferCount, 0, 0, DXGI_FORMAT_UNKNOWN, 0);` |
| `Bedrock/CoRE2/DX11Enums.cpp:10` | `DXGI_FORMAT_UNKNOWN,` |
| `apEx/Phoenix/Mesh.cpp:1754` | `resourceViewDesc.Format = DXGI_FORMAT_UNKNOWN;` |

**`DXGI_FORMAT_X16_TYPELESS_G8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:726` | `case DXGI_FORMAT_X16_TYPELESS_G8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:811` | `case DXGI_FORMAT_X16_TYPELESS_G8_UINT:` |

**`DXGI_FORMAT_X24_TYPELESS_G8_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:185` | `case DXGI_FORMAT_X24_TYPELESS_G8_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:636` | `case DXGI_FORMAT_X24_TYPELESS_G8_UINT:` |

**`DXGI_FORMAT_X32_TYPELESS_G8X24_UINT`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DDSTextureLoader.cpp:158` | `case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:` |
| `Bedrock/CoRE2/DX11Texture.cpp:606` | `case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:` |

**`DXGI_FORMAT_Y210`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:608` | `case DXGI_FORMAT_Y210:` |
| `Bedrock/CoRE2/DX11Texture.cpp:789` | `case DXGI_FORMAT_Y210:` |

**`DXGI_FORMAT_Y216`** ×2

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:609` | `case DXGI_FORMAT_Y216:` |
| `Bedrock/CoRE2/DX11Texture.cpp:790` | `case DXGI_FORMAT_Y216:` |

**`DXGI_FORMAT_Y410`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:648` | `case DXGI_FORMAT_Y410:` |

**`DXGI_FORMAT_Y416`** ×1

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:607` | `case DXGI_FORMAT_Y416:` |

**`DXGI_FORMAT_YUY2`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Texture.cpp:649` | `case DXGI_FORMAT_YUY2:` |
| `Bedrock/CoRE2/DX11Texture.cpp:784` | `case DXGI_FORMAT_YUY2:` |
| `Bedrock/CoRE2/DX11Texture.cpp:1217` | `case DXGI_FORMAT_YUY2:                  memcpy_s( &header->ddspf, sizeof( header->ddspf ), &DDSPF_YUY2, sizeof( DDS_PIXELFORMAT ) );        break;` |

**`DXGI_MODE_DESC`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/MinimalPlayer.cpp:202` | `DXGI_MODE_DESC* displayModeList;` |
| `apEx/MinimalPlayer/MinimalPlayer.cpp:212` | `displayModeList = new DXGI_MODE_DESC[ numModes ];` |

**`DXGI_MODE_SCALING_UNSPECIFIED`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |

**`DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |

**`DXGI_SWAP_CHAIN_DESC`** ×5

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:175` | `DXGI_SWAP_CHAIN_DESC scd;` |
| `Bedrock/CoRE2/DX11Device.cpp:176` | `memset(&scd, 0, sizeof(DXGI_SWAP_CHAIN_DESC));` |
| `Bedrock/CoRE2/DX11Device.cpp:271` | `DXGI_SWAP_CHAIN_DESC desc;` |
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |

**`DXGI_SWAP_EFFECT_DISCARD`** ×2

| file:line | source |
|---|---|
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |

**`DXGI_USAGE_RENDER_TARGET_OUTPUT`** ×3

| file:line | source |
|---|---|
| `Bedrock/CoRE2/DX11Device.cpp:180` | `scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;` |
| `apEx/MinimalPlayer/WindowHandler.cpp:44` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_REN` |
| `apEx/MinimalPlayer/WindowHandler.cpp:46` | `static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_T` |
