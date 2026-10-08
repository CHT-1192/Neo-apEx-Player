# D3D11-on-Vulkan shim — design

The apEx engine speaks Direct3D 11 directly (`apEx/Phoenix/*.cpp` use
`ID3D11Device*`, `ID3D11DeviceContext*`, `ID3D11Buffer*`, …) and Bedrock's
CoRE2 layer has a complete D3D11 backend (`Bedrock/CoRE2/DX11*.cpp`).

Rather than rewriting ~30k lines of engine and tool code, we shadow `<d3d11.h>`
with `compat/d3d11.h` and implement that subset **on top of Vulkan** in
`src/d3d11/`. CoRE2's DX11 backend and the Phoenix engine then compile
unchanged.

This file is the contract for that implementation.

## 1. Resource model

| D3D11 | Vulkan |
|---|---|
| `D3D11_USAGE_IMMUTABLE` / `DEFAULT` buffer | device-local `VkBuffer` (+ staging copy recorded in the frame command buffer) |
| `D3D11_USAGE_DYNAMIC` buffer | suballocation from the **frame ring** (host visible, coherent); `Map` returns a pointer straight into the ring |
| `D3D11_USAGE_STAGING` | host-visible `VkBuffer` |
| `ID3D11Texture2D` | `VkImage` (optimal tiling) + optional `VkImageView` |
| `ID3D11ShaderResourceView` | `VkImageView` + remembered `VkImageLayout` |
| `ID3D11RenderTargetView` / `DepthStencilView` | `VkImageView` + `VkImageLayout` |
| `ID3D11SamplerState` | `VkSampler` |
| `ID3D11InputLayout` | stored `D3D11_INPUT_ELEMENT_DESC[]` (see §4) |
| `ID3D11*Shader` | `VkShaderModule` + `phxvk::Reflection` |
| `ID3D11BlendState`/`DepthStencilState`/`RasterizerState` | plain state structs |

`Map`/`Unmap` on a dynamic resource must be **renaming**, not in-place:
`Map(WRITE_DISCARD)` grabs a fresh `RingAllocate(ByteWidth)`, returns
`alloc.mapped`, and the resource remembers `alloc.offset`.  `Unmap` only
flushes.  This is what makes per-draw constant buffer updates correct without
stalling the GPU.

## 2. Register model and descriptor layout

The shader compiler (`src/rhi/shader_compiler.cpp`) rewrites every HLSL
resource declaration to an explicit `[[vk::binding]]`:

| D3D11 slot | Vulkan binding |
|---|---|
| `register(bN)` and the implicit `$Globals` (from `register(cN)`) | `0 + N` |
| `register(tN)` | `64 + N` |
| `register(sN)` | `192 + N` |
| `register(uN)` | `208 + N` |

The device context keeps, per stage, arrays of the currently bound objects
(`cb[16]`, `srv[128]`, `sampler[16]`, `uav[16]`) exactly like D3D11.

Pipeline layout = union of the reflections of the bound shader stages.  Every
uniform-buffer binding is declared `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`
so a binding is `(VkBuffer, offset)`; the dynamic offsets are taken from the
resource's current ring allocation.  Unused-but-declared bindings get a dummy
64 KiB zero buffer.

A descriptor set is allocated from a per-frame pool and written immediately
before each draw.  The pool is reset (not freed) at `Present`.

## 3. Pipelines

D3D11 creates VS/PS/GS/input-layout separately; Vulkan needs a monolithic
pipeline.  Build them lazily at draw time and cache them keyed by:

* the currently bound VS/PS/GS/HS/DS `VkShaderModule`s,
* the bound input layout,
* blend / depth-stencil / rasterizer state objects,
* primitive topology class (point / line / triangle / patch),
* the render target + depth formats of the currently bound views.

When `OMSetRenderTargets` changes the format set, subsequent draws simply
create a differently-keyed pipeline.

**Geometry shaders are unavailable on MoltenVK** (`geometryShader = false`).
If a pipeline requests a GS, log it once per technique and create the pipeline
without the GS stage so the rest of the demo still renders.

## 4. Vertex input

`CphxScene`/Phoenix use a fixed 72-byte vertex (see `docs/spec/mesh-system.md`
§1).  D3D11 matches input elements to the shader by *semantic name+index*;
SPIR-V only keeps locations.  glslang (with `auto_map_locations`) assigns
locations **by declaration index of the input struct**, and it does *not*
compact them when a member is unused.

Measured over the Clean Slate shaders (`build/phx_shadertool`):

```
tech006 (uses Position, Normal, Color, UV)  -> locations 0,2,3,4   (1 stripped)
tech000 (uses Position, UV only)            -> locations 0,4
tech013 (particle vertex)                   -> locations 0,1
```

So the mapping rule is **location N ⇄ `D3D11_INPUT_ELEMENT_DESC[N]`** — *not*
"the Nth surviving SPIR-V input ⇄ element N", which would mis-pair every shader
whose Position1 is unused (i.e. most of them, and Position1 is never read in
Clean Slate).

Implementation:

* the `D3D11_INPUT_ELEMENT_DESC` array gives, for element `i`, the buffer slot,
  aligned byte offset and DXGI format → `VkVertexInputAttributeDescription`
  with `location = i`;
* the SPIR-V reflection then only has to confirm that every reflected location
  is `< elementCount` — the types already agree, because the declaration order
  and the descs come from the same struct;
* if a reflected location is `>= elementCount`, log an error and fall back to
  building the layout purely from the SPIR-V types (tightly packed, slot 0).

DXGI → Vk format conversion table lives in `src/d3d11/dxgi_formats.cpp`.

## 5. Dynamic rendering

MoltenVK on this machine reports Vulkan 1.1 with `VK_KHR_dynamic_rendering`, so
`vkCmdBeginRenderingKHR`/`vkCmdEndRenderingKHR` are used and no `VkRenderPass`
/`VkFramebuffer` objects are needed.  `OMSetRenderTargets*` records the
currently bound views; the render pass instance is (re)opened lazily whenever
the set changes and closed on `Present`/`ClearState`.

Image layout transitions (`COLOR_ATTACHMENT_OPTIMAL` ⇄
`SHADER_READ_ONLY_OPTIMAL`) are tracked per image subresource and emitted as
barriers when the binding changes.

## 6. Frame lifecycle

D3D11's immediate context maps onto a single `VkCommandBuffer`:

```
Present()          -> vkQueueSubmit + vkQueuePresentKHR, then
                      vkResetCommandPool, ring offset = 0
first draw/cmd     -> vkBeginCommandBuffer (lazy)
```

`Present` blocking on the previous frame's fence means at most two frames are
in flight, which is what the original engine assumes.

## 7. Compilation path

`D3DCompile` in `src/d3d11/d3dcompiler_shim.cpp` forwards to
`phxvk::CompileHLSL`, and the returned `ID3DBlob` contains **SPIR-V**, not
DXBC.  `CreateVertexShader`/`CreatePixelShader`/… therefore treat the blob as
SPIR-V.  `D3DReflect` returns the reflection we already computed.

`D3DX11CompileFromMemory` is an alias of `D3DCompile`.

## 8. What is intentionally not implemented

* Geometry shader execution (MoltenVK limitation, see §3).
* Compute-shader particle emitters (`PHX_VOLUMETRIC_RENDERTARGETS` is off in
  `PhoenixConfig.h`).
* `ID3D11Query` occlusion queries: they are created and mapped to no-ops; the
  engine only ever checks their existence.
* D3D9 (`Bedrock/CoRE2/DX9*.cpp` is excluded from the build).
