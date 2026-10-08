# Project status

Snapshot of where the Neo-apEx-Player port stands. Read this first when
resuming; `docs/spec/*.md` holds the reverse-engineered formats and
`docs/DESIGN-d3d11-shim.md` the renderer contract.

## Goal

A cross-platform Vulkan player for the apEx demotool (Conspiracy, 2014-2023),
validated by loading and rendering `Projects/Clean Slate/cleanslate.apx`.

## Architecture (decided, do not relitigate)

Reuse the original engine rather than rewriting it:

```
compat/                  shadow headers that replace <Windows.h>, <d3d11.h>,
                         <D3DX10Math.h>, <tchar.h>, ... (first on the include
                         path) + POSIX implementations in src/platform/
src/rhi/                 Vulkan bring-up (vk_core) and runtime HLSL -> SPIR-V
                         with reflection (shader_compiler)
src/d3d11/               D3D11 subset implemented on Vulkan ("mini-DXVK")
third_party/apEx/        vendored Bedrock + apEx (patched only via
                         tools/apply_patches.py)
```

CoRE2's existing `DX11*.cpp` backend and the whole Phoenix engine are compiled
**unchanged** on top of `src/d3d11/`. There is a single Vulkan implementation
behind both APIs.

## Verified so far

| Item | Evidence |
|---|---|
| MoltenVK works | `work/vkprobe/probe` -> Apple M5, Vulkan 1.1.357, `VK_KHR_dynamic_rendering` + `VK_KHR_swapchain` |
| **Geometry shaders unavailable** | `VkPhysicalDeviceFeatures::geometryShader == false`; 5 Clean Slate techniques use a GS -> must be skipped |
| HLSL -> SPIR-V | all 83 Clean Slate shaders, 107 applicable entry points, **0 failures** (`build/phx_shadertool`) |
| D3D11 register -> Vulkan binding mapping | `$Globals`(c0,c1) -> binding 0; `tN` -> 64+N; `sN` -> 192+N; `bN` -> N (verified by reflection) |
| Bedrock/BaseLib compiles | 28/32 files; the 4 excluded ones are Windows-only (Socket, CrashTracker, StackTracker, ImageDecompressor - all replaced) |
| Song clock | `.mvm` header parser reproduces the original timeline exactly: step010 = 275.90 s, step010padding = 284.44 s (the padding variant is what `cleanslate.apx` names) |

## Key facts that constrain the implementation

* **Frame clock = song milliseconds**, `frame = floor(ms/1000*fps)`, fps=120 for
  Clean Slate. `MUSICDATA.TickData` is editor-only. The MVX synth is
  closed-source 32-bit Windows code with no portable source, so audio cannot be
  synthesised here; the wall clock reproduces the original timing exactly.
* The release player ignores every event's `TargetRT` and composites into
  render target 0.
* Spline values in the `.apx` are raw IEEE binary16 bit patterns printed as
  decimal integers.
* `.apx` files are plain XML with stray whitespace text nodes; all lookups are
  by tag name + occurrence index.
* The canonical vertex is 72 bytes: POSITION0 f32x3, POSITION1 f32x3,
  NORMAL0 f32x3, COLOR0 R8G8B8A8, TEXCOORD0 f32x4, TEXCOORD1 f32x4 (always
  zero); 32-bit indices.
* Mesh text generation uses Windows GDI glyph outlines + the GLU tessellator;
  macOS provides GLU via the OpenGL framework (verified to build and run).

## MILESTONE 8: the loading screen, and frames drawn before they were ready

Two things the port got wrong about starting up, both reported from watching it
run rather than from a diff.

**Frames were drawn before their resources existed.** The frame loop asked for
whatever an active event was missing, drew the frame anyway, and only then
serviced the generator queue:

```cpp
if (!e->ContentReady()) e->RequestContent();       // request it
Project.Timeline->Timeline->Render(frame, ...);    // ...and draw regardless
ProcessGeneratorQueue(20, &device);                // generate afterwards
```

A frame presented in that window has half-built scene meshes, textures and
shader states, which is what showed as untextured models, as black flashes, and
as a glare that appeared in some runs of the same frame and not others: the same
0:31 frame measured 3.2 against the reference video on one run and 20.9 on the
next. The original cannot show such a frame -- `CphxTimeline_Tool::Render`
refuses to draw an event that is not `ContentReady()`. The requests are now
collected first and the queue is drained to empty before the draw, which takes
`generator` out of the slow-frame reports entirely and makes the frame
reproducible (3.228 on every run).

**There was no loading screen.** The original's progress display is not UI: it
is a separate, tiny binary project. `MinimalPlayer.cpp:263-266` loads
`precalcminimal.64k` -- chosen for a CNS demo by `apExRoot.cpp:1768-1775` from
the project's `<minimalprecalc>1</minimalprecalc>` and
`<farbrauschprecalc>0</farbrauschprecalc>`, which is what cleanslate.apx says --
and then renders *that* every 40 ms from `CphxProject::DoPrecalc()` at a
position that advances with the load. The progress bar is frame 0..1000 of that
project. The port now does the same, including the part that is easy to miss:
the two projects share the engine's globals, so the precalc's
`phxDepthBufferView` is saved and put back around each loading frame.

Using the binary loader meant fixing it for a 64-bit build, which nothing had
needed to do before. Its stream table was built by rewriting the file's first
`StreamCount` dwords into pointers in place --

```cpp
datastreams = (unsigned char**)DATA;
for ( x ) datastreams[ x ] += (int)DATA;    // (int) drops the top half
```

-- which is exact when a pointer is 32 bits and lands every stream in unmapped
memory when it is not. The table is now built separately, leaving the buffer
untouched. This is in `tools/apply_patches.py` with the rest of the patches.

## MILESTONE 7: the geometry shader stage is not optional

The background grid in Clean Slate (visible at 0:31, 0:33, 0:39, and behind the
title cards) is missing on this port, and the first thing written about
geometry shaders here said the opposite -- "their stage is skipped at pipeline
creation so the demo still renders". That sentence was wrong, and it cost real
time before it was caught. It is corrected in the code; this is why.

**The grid is a wireframe material, and its thickness comes from a geometry
shader.** Reading the project rather than the binary settles it:

* event 2's scene is named `++scene / titles`; its object `background/background
  wire` is a `Plane` whose material GUID resolves to a material literally named
  `Wireframe`;
* that material's technique (`2A9BCBB81A1352D113B8B5BF0982D887`) has a PS/VS pair
  and a GS whose whole job is to turn each line into a screen-space quad:

  ```hlsl
  [maxvertexcount(6)]
  void g( line VSOUT input[2], inout TriangleStream<GSOUT> OutputStream )
  {
      float2 d = normalize((p2/p2.w).xy - (p1/p1.w).xy);
      float4 n = float4(-d.y, d.x, 0, 0) * width.x * 0.05;   // the line's width
      ... six Appends forming two triangles ...
  }
  ```

* Metal has no geometry shader stage, so `src/d3d11/d3d11_context.cpp` drops it.
  What is left is one-pixel hairlines, which is why forcing `LINELIST` to
  `TRIANGLE_LIST` moved 0.2% of the pixels: there was almost nothing there.
* There is no cheap way out either. The port never uses `wideLines`, and
  MoltenVK does not offer it, so the width cannot be recovered by drawing
  thicker lines.

**It is not just the grid.** 5 of the project's 34 render techniques carry a
geometry shader, and they are the main ones:

| technique | what its geometry shader builds | lost without it |
|---|---|---|
| `Wireframe` | a screen-space quad per line | the background grid |
| `[PBR Mixed Rendering]` | the reversed copy of a triangle when `dat.z > 0.5` | two-sided geometry |
| `[PBR Mixed Rendering with vertex offset]` | a flat normal from the triangle, plus the reversed copy | flat shading, two-sided geometry |
| `Default Particle` | a camera-facing quad per point | particles |
| `PointSprites` | a quad per triangle at its centroid | point sprites |

`maxvertexcount` is 4 or 6 in every case, i.e. all five are the same shape of
shader: a fixed-size expansion of one input primitive into a small triangle
stream. The port now counts and names the affected pipelines instead of
announcing the skip once and moving on.

**This is now emulated** (`src/rhi/gs_emulation.{h,cpp}`, `tests/gs_emulation_test.cpp`).
The shape of a geometry shader is read out of its HLSL when it compiles, the
entry point is translated into a vertex shader, and the draw is rebuilt:

  * the input primitive's K vertices are packed into one "fat" vertex and N of
    those are emitted per primitive, so the translated stage can run the
    original vertex shader on every vertex of the primitive;
  * the translated stage walks the geometry shader's body and returns at the
    `Append` whose running index equals `SV_VertexID % N`; a slot the body never
    reached collapses to a zero-w vertex and contributes no geometry;
  * the emitted vertices are drawn as a triangle strip with a primitive restart
    wherever the body called `RestartStrip`, so the fixed-function assembler
    rebuilds the strips the geometry shader would have streamed;
  * `Append` calls inside a loop with a literal trip count are expanded, which
    `[PBR Mixed Rendering]` depends on -- reading the body linearly puts its
    restarts after slot 1 and 2 instead of 3 and 6 and every triangle built from
    that strip is wrong.

A shader that does not fit is refused by name and the stage is dropped as
before: an output stream that is not a `TriangleStream`, an input primitive
that is not point/line/triangle, or a `RestartStrip` in a loop whose trip count
is not a literal. Everything the emulation builds is cached per distinct draw --
the expansion for the grid alone is 16k vertices and 21k indices, and rebuilding
it into the per-frame ring exhausted the ring inside one frame.

Against the reference video (mean absolute difference, 30-frame warm-up):

| time | before | after |
|---|---|---|
| 0:31 | 4.170 | **3.208** |
| 0:39 | 6.957 | **6.174** |
| 1:06 | 3.653 | 3.653 |
| 1:18 | 6.622 | 6.622 |
| 3:00 | 13.195 | 13.195 |

One open question found while sweeping, and it is **not** from this work: at
55 s the port renders the incoming scene about a quarter of a second before the
reference does. Checking out the commit before the emulation and capturing the
same frame reproduces it exactly (the striped ridge is there both with and
without), so it is a pre-existing frame/cut question.

## MILESTONE 6: the 50-second hang was RAND_MAX

The demo hung for 90+ seconds and the only way out was Force Quit. It was not
resource generation, and not the shader compiler, and not the GPU. A stack
sample of the stalled process said it plainly:

```
main.cpp:919 -> CphxTimeline::Render (Timeline.cpp:355)
  -> CphxEvent_ParticleCalc::Render (Timeline.cpp:264)
    -> CphxObject_ParticleEmitter_CPU::UpdateParticles
      -> CphxObject_ParticleEmitter_CPU::SpawnParticle
        -> rand()                                       (msvc_rand.cpp:20)
```

`SpawnParticle` (Phoenix/Scene.cpp:403-406) rejection-samples a point in
`[-0.5, 0.5]^3` until it lands inside the unit sphere for a spherical emitter:

```cpp
do { p = D3DXVECTOR3( rand()/(float)RAND_MAX - 0.5f, ... ); }
while ( !( EmitterType != 1 || D3DXVec3LengthSq( &p ) < 0.25 ) );
```

`RAND_MAX` in that translation unit was **0x7fffffff** - macOS's value - not the
MSVC CRT's 0x7fff, so `rand()/(float)RAND_MAX` was in `0..1.5e-5` and the point
was pinned at the cube's corner, `|p|^2 = 0.75`. The condition was never true
and the loop never ended.

Three things made this hard to see:

* `src/platform/msvc_rand.cpp` already reproduced the Windows *generator*, and
  its comment names this exact failure mode ("would push every
  `rand()/(float)RAND_MAX` far out of the intended 0..1 range") - the constant
  was the half that was missed.
* `Bedrock/BaseLib/Random.h` does `#define RAND_MAX 0x7fff`, but only
  translation units that include `BaseLib.h` ever see it, and
  `compat/Windows.h` pulls in `<cstdlib>` which redefines it to the macOS value
  for whoever reads it afterwards.  `Scene.cpp` never includes `BaseLib.h` at
  all: preprocessing it showed the system `0x7fffffff` as the *only* definition
  in the translation unit.  `-Wno-everything` hides the redefinition warning.
* `phxSpline.cpp`'s header comment describes the port relying on Random.h's
  define, which is exactly the assumption that does not hold.

Fixed in `compat/stdlib.h`: a wrapper that `#include_next`s the real header and
then pins `RAND_MAX` to 0x7fff, so every translation unit gets the Windows
semantics the 36 `rand()/(float)RAND_MAX` call sites were written against.

Measured with `--no-ui --no-audio --fixed-fps` (40 frames each, and each of
these regions used to cost 7.4 s or more *per frame*):

| frames | before | after |
|---|---|---|
| 5980-6024 (the old hard hang) | `--frame-timeout 90000` fired | 9 s, exit 0, worst frame 390 ms |
| 3700-3739 | ~7.4 s/frame | 10 s, worst 338 ms |
| 4900-4939 | ~7.4 s/frame | 9 s, worst 329 ms |
| 6050-6089 | hung at 5999, never reached | 9 s, worst 385 ms |
| 15000-15039 | never reached | 7 s, **then a segfault** - see Open work |

Frame 2999 (24.99 s) still matches the reference video: port mean
`15.39/46.75/61.97` against the original's `14.19/44.37/59.93`, mean absolute
difference 4.8/5.3/5.1 per channel, unchanged from before the fix.

`src/player/main.cpp` now reports every frame that exceeds 250 ms with a phase
breakdown (update / timeline / generator / dump / present) and the list of
active events, and the startup resource drain reports its slowest round. That
is what turned "frame 6009 is slow" into the sampled stack above; it is worth
more than the guesswork it replaced.

## MILESTONE 5: the release setup dialog, and the resize crash

### The dialog

`src/player/ui.{h,cpp}`. The release player shows this before anything exists:

```
MinimalPlayer.cpp:147   if ( OpenSetupDialog( Strings ) )
MinimalPlayer.cpp:257   InitWindow( hInstance, setupcfg.mode.dmPelsWidth, ..., setupcfg.fullscreen )
SetupDialog.cpp:195     DialogBox( 0, MAKEINTRESOURCE( IDD_SETUP ), 0, DlgFunc )
```

so it is a parentless, borderless, screen-centred popup that owns the whole event
loop on no graphics device at all, and only its answer is passed to `InitWindow`.
The port keeps that order: `main()` runs `RunSetupDialog()` before
`CCoreDX11Device`/`CCoreWindowHandlerWin` exist.

The controls come from the resource, which is a *resourceless* DLGTEMPLATE
(`Setup_Dialog_Without_Social.h`, 209x146 DLU, 12 items, Verdana 8pt); its exact
geometry was recovered by parsing that blob, and `ui.cpp` scales those
proportions to ImGui's 13px font (400x280). Two entries are easy to get wrong:

* the combo is `AspectRatiosX/Y = {4,5,16,21,1,16,3,17,64}/{3,4,9,9,1,10,2,9,27}`
  behind a `"%d:%d (auto)"` first entry - **not** a short hard-coded list;
* both checkboxes are ticked on open (`SetupDialog.cpp:123-124`), and `Fullscreen`
  defaults on, which is why `--no-ui` runs are windowed but the dialog is not.

Built on SDL3 + ImGui with `SDL_CreateRenderer` (sdlrenderer3), so it never
touches the D3D11/Vulkan path: no swap chain, no render pass, no image layout.
That was the whole point - an earlier attempt drew the dialog *inside* the demo
window and fought the engine's render target state until it crashed.

Two answers reach the engine: the resolution and fullscreen flag go into
`CCoreWindowParameters`, and **Monitor Aspect** is written to
`src/platform/phx_monitor_aspect.*`, which `CapexRenderTargetCollection::
UpdateCollection()` (Phoenix_Tool/apxProject.cpp) reads where it used to read the
unvendored editor's `Config::MonitorAspectX/Y`. `(auto)` = the real display, which
is the value the port had hard-coded, so nothing changes unless the user picks
something else.

### The resize crash (this was the intermittent one)

The port makes the window resizable; the original never resized anything
(`InitWindow` creates `phxBackBufferView` once, `WindowHandler.cpp:97-98`). So
`CCoreDX11Device::Resize` -> `IDXGISwapChain::ResizeBuffers` is reachable, e.g.
on the fullscreen transition, and it segfaulted every time.

Root cause: `SwapChainImpl::DestroyBackBufferObjects()` released the back-buffer
`Texture2DImpl`, and views do **not** hold a reference to their resource in this
shim. The player's global `phxBackBufferView` - the render target the timeline
composites the finished frame into - was left pointing at freed memory, which read
back as "no VkImage", so the layout transition was skipped and
`vkCmdBeginRendering` got a null image view.

Fix: the back buffer wrapper is *reused* across a resize (only its cached
width/height/format are refreshed), because everything in it - `vkImage()`,
`vkSwapchainView()`, `currentLayout()` - already resolves to the live swap chain,
so nothing about it belongs to one generation of images. `ResizeBuffers` now drops
only the swap chain's own RTV. Verified: `--no-ui --fullscreen --frames 60`
reaches frame 60 and exits 0 through the 1470x786 -> 1470x923 recreate, and the
post-resize frame keeps the sRGB-encoded levels (mean 0.25/10.98/17.88 against
0.40/13.97/22.66 for the same present windowed).

Worth knowing: the back-buffer RTV's format is **not** carried by the image view
(`ViewImpl::vkView()` returns the swap chain's own view for a dynamic swap-chain
view) but by `PipelineKey::color[]`, which feeds
`VkPipelineRenderingCreateInfo::pColorAttachmentFormats`. That is why the sRGB
back-buffer view encodes correctly, and why a resize cannot lose it.

## MILESTONE 4: the original MVX synthesiser produces the music

MVX is closed source - "REALTIME BUZZ-BASED SYNTHESIZER" by Gargaj/Conspiracy,
shipped as `mvx.h` plus x86-32 MSVC static libraries. It cannot be ported to
macOS/arm64. It does not have to be.

**The insight: GitHub's `windows-latest` runners execute x86-32 through WoW64, so
CI is the one environment where the real synth can run as part of the build.**
`.github/workflows/mvx-windows.yml` builds `tools/mvx_render.cpp` as Win32, links
the shipped `.lib`, renders `demo/step010padding.mvm`, and uploads the result.

```
mvx_render: 46 machines, 2432 ticks, 135 bpm / 5 tpb, 46 connections
mvx_render: 12543616 frames (284.44 s)
mvx_render: wrote cleanslate.apxsndcache (2432 markers)
```

12,543,616 frames = 284.44 s, which is what `song_clock.cpp` derives from the
`.mvm` header **independently** - two unrelated computations agreeing is the
check that the container parse is right.

The audio is the original recording, not a copy of anything:

| | |
|---|---|
| MVX output vs the release upload, whole track | normalised cross-correlation **0.85** |
| best alignment | **-23 ms** (the Opus codec's own delay) |
| mean level | -13.0 dB vs -13.1 dB |

Then the player reads it back and drives the clock from the play cursor:

```
music: cleanslate.apxsndcache (apxsndcache, 284.44 s, 44100 Hz, 2 ch, 2432 tick marker(s))
audio: played 21.023 s of 284.436 s (clock is the device play cursor)
```

### What is easy to get wrong (both cost real debugging time)

1. **The synth must be driven by hand.** `mvxRenderOffline()` is exported and
   looks like the right entry point, but it does not set up the machine graph and
   crashes. The working sequence is copied from the tool's own
   `phxSongMVM.cpp:OfflineRenderSong`: call `Tick()` on every machine once per
   tick, apply the master TPB/BPM/volume sequences, then `mvxMixer_Render()` and
   `mvxSystem_ClipBuffer()`. `mvxSystem_Machines` is not in `mvx.h`; the tool
   declares `class mvxMachine` locally and so does the harness.
2. **The x87 control word must be set to single precision (0x0200).** The DSP
   assumes a 24-bit mantissa; leaving the FPU in extended precision changes the
   output.
3. Linking the 2019 library with VS2022 fails on `__imp__rand` and
   `__except_handler4_common`: it predates the Universal CRT, so the harness links
   `msvcrt.lib` explicitly and drops `libucrt`.

### CI as it now stands

| workflow | runner | what it proves |
|---|---|---|
| `ci.yml` | macos-14 | the whole port compiles from scratch; XML round trip, song/PCM reader, and the D3D11 render cases where a Vulkan device exists |
| `mvx-windows.yml` | windows-latest | the **original closed-source synth** renders Clean Slate's music, checked for level and content |

GitHub's macOS runners expose a paravirtualised GPU that MoltenVK cannot use
(`AppleParavirtDevice` does not implement `newArgumentEncoderWithLayout:`, so
`vkCreateInstance()` throws), so `ci/run-vulkan-test.sh` reports those cases as
SKIP. That is an infrastructure limit, not a passing grade: the script only
tolerates that exact signature and fails on anything else, and the validation step
additionally requires the layer to have reported itself as enabled so a "clean"
result cannot be a silent no-op.

## MILESTONE 3: Clean Slate renders

```
$ ./build/neo_apex_player "/path/to/Projects/Clean Slate/cleanslate.apx" \
      --fixed-fps --frames 3000 --no-vsync --dump-frame 1500=frame.ppm
[phxvk info] timeline: 213 event(s)
[phxvk info] render targets: 13 collection target(s), timeline has 13
[phxvk info] song: .../step010padding.mvm (284.44 s, 2432 ticks, 135 bpm / 5 tpb)
[phxvk info] rendered 3000 frames, exiting
EXIT=0
```

Frame 1500 and frame 2999 are real demo imagery (a light burst, and the "DEMO TAPE
#20 / A 64 MIN / CNS" title card - the text meshes, textures, geometry and
lighting all render).  `--dump-frame N=FILE` writes the composite target *and*
the back buffer as PPM, `--validate` runs the validation layers.

### The five integration bugs that made every frame black

None of these were rendering bugs; they were all pieces the *tool UI* or the
*release player* used to provide and that nothing in the engine sets:

1. `CphxTimeline::EventCount` starts at 0 - the tool sets it per frame. 0 events
   are iterated, so nothing renders.
2. `CphxTimeline_Tool::Render()` passes `tool=true`, and the engine skips BOTH
   the back-buffer clear and the final composite blit when `tool` is set
   (Timeline.cpp:337, :355).  A player must call
   `CphxTimeline::Render(frame, false, false)`.
3. `phxBackBufferView` and `ScreenX/ScreenY` are owned by the release player
   (MinimalPlayer/WindowHandler.cpp creates the view from swap-chain buffer 0).
   Left null, the blit targets nothing.
4. The render targets themselves come from
   `CapexProject::SpawnRenderTargetCollection()` + `ApplyRenderTargets()`, which
   the tool UI called.  Without them the events have no `Target` at all.
5. The per-frame readiness work in `CphxTimeline_Tool::Render` - in particular
   `CphxScene_Tool::ForceUpdateContent()` - is what builds a scene's
   render-data instances.  Skipping it leaves the target at exactly zero.

Plus `phxInternalRenderTarget`, which `CphxProject::LoadProject()` creates on the
binary/precalc path and the `.apx` path never did.

### Deliberate design decisions

* The clock starts **after** resource generation.  Starting it before made the
  demo begin at frame ~1200, because generating Clean Slate's GPU resources
  takes seconds.
* The player exits via `std::_Exit()` rather than unwinding the engine's object
  graph.  apEx was written for an application that ends by falling off the end
  of main; its teardown order walks freed memory after the device has released
  its resources and crashes *after* the last frame is presented.  The original
  release player terminates the same way.

## MILESTONE 2: the whole player builds

```
cmake -S . -B build -DAPEX_BUILD_PLAYER=ON
cmake --build build --target neo_apex_player -j8
-> [100%] Built target neo_apex_player      (1.7 MB executable)
```

`libapex_engine.a` contains the entire vendored engine: BaseLib, UtilLib,
CoRE2 (the D3D11 backend compiled unchanged on top of the shim), Phoenix and
Phoenix_Tool - 87 translation units, all of which compile with zero errors.

What had to be wired up for that to link:
* `src/platform/gdi_posix.cpp` (CoreText/CoreGraphics glyph outlines + the GDI
  bitmap surface) and the macOS frameworks it needs;
* `src/platform/window_handle.cpp` (the engine handle resolver) - dependency
  free so the shim can link it without BaseLib/Logger;
* `src/platform/phxsound_unsupported.cpp` - the honest "music unavailable"
  `CphxSound` that `CapexProject::LoadSong` still references;
* `src/platform/tool_ui_dropped.cpp` - the two globals the unvendored editor
  used to define (`apexBuild`, `pickingEnabled`);
* `Bedrock/UtilLib/PNGDecompressor.cpp` (previously excluded on a false
  OpenSSL assumption) - it owns the single `STB_IMAGE_IMPLEMENTATION`;
* `src/d3dx/d3dxmath.cpp`;
* a `GetSwapChain()` accessor on `CCoreDX11Device`, because the engine's own
  global `SwapChain` was never assigned by anything and two call sites
  (`Project.cpp:182 DoPrecalc`, the player's present) dereference it.

Running it currently segfaults in `gdi_posix.cpp` during static initialisation;
that is with the GDI work and is being fixed there.

## MILESTONE: the D3D11-on-Vulkan shim renders

`./build/phx_smoke` (built with `cmake -S . -B build && cmake --build build --target phx_smoke`):

```
[phxvk info] Vulkan device: Apple M5 (api 1.1.357)
[phxvk info] Vulkan core ready (dynamic rendering: yes)
[phxvk info] [d3d11] device created (Vulkan Apple M5, geometryShader NO)
vs reflection: 2 inputs
render target: 65536/65536 pixels non-zero
centre pixel: R=64 G=128 B=191 A=255 (expected 64 128 191 255)
PASS: the shim compiled SPIR-V, set up the pipeline and rendered the quad
```

That exercises the whole critical path end to end: runtime HLSL -> SPIR-V,
the `register(...)` -> `[[vk::binding]]` descriptor mapping, dynamic-rendering
pipeline creation, D3D11 input-layout -> VkVertexInput pairing, the draw, and
a render-target readback whose centre pixel matches the expected sampled value
exactly.

## Compile progress (round 3)

| Directory | Compiling | Notes |
|---|---|---|
| Bedrock/BaseLib | 28/32 | the 4 failures are the excluded Windows-only files |
| Bedrock/UtilLib | 4/5 | XMLNode/XMLDocument being rewritten on RapidXML |
| Bedrock/CoRE2 | 32/33 | only WindowHandler.cpp, which is excluded and replaced by src/platform/window_sdl.cpp |
| Phoenix | 13/16 | blocked on: GDI (Mesh/Texgen), D3DXFLOAT16 ambiguity (phxSpline) |
| Phoenix_Tool | 12/16 | blocked on: the tool-UI decoupling (Timeline_tool/apxProject), a Model_Tool.h forward declaration, D3DXFLOAT16 |

```
clang++ -std=c++17 -fsyntax-only -fms-extensions -fdelayed-template-parsing -Wno-everything \
  -Icompat -Ithird_party/apEx/Bedrock/UtilLib -Ithird_party/apEx/Bedrock/UtilLib/RapidXML \
  -Ithird_party/apEx/Bedrock/CoRE2 -Ithird_party/apEx/Phoenix -Ithird_party/apEx/Phoenix_Tool \
  -Ithird_party/apEx/LibCTiny -Ithird_party/apEx/MinimalPlayer <file>.cpp
```
(work/scan2.sh wraps exactly this)

**Verified in round 3:** glslang emits exact D3D11 constant-buffer packing, not std140 --
`float tme,aspect,xres,yres` land at offsets 0/4/8/12 and the matrices at 16/80/144/160/224
in `tech016_pass0_Calculate_Coc`, matching the engine's C++ struct byte-for-byte.  This was
risk R1 in `docs/spec/d3d11-shim.md` and it is now closed.

## Patch set

`tools/apply_patches.py` holds every change to the vendored tree (currently ~20 entries,
each with a reason).  It is **fully idempotent**: `tools/revendor_patched.sh` restores the
touched files from the pristine release and the script re-applies them, so the port is
reproducible.  Run both after any `third_party` change.

## Round 5 notes

* `Arbaro.cpp` now compiles.  The first attempt used a blanket
  `D3DXVECTOR3&` -> `const D3DXVECTOR3&` replace-all, which produced
  `const const const` and broke `const`-ness in the wrong direction; it was
  replaced with eight precise, individually-reasoned patch entries (see the
  `--- Arbaro ---` block in `tools/apply_patches.py`).  Lesson: patch precisely,
  never blanket-replace a type.
* `src/player/main.cpp` does not exist yet -- it is the last piece before the
  first end-to-end run.  `docs/DESIGN-player-app.md` has the exact contract.
* `src/d3d11/d3d11_internal.h` marks methods `override` that
  `compat/d3d11.h` does not declare as virtual; the shim and header work need
  to converge on one interface (the shim agent owns the shim side).

## Round 9 additions

* `src/player/main.cpp` — the player entry point, written and compiling.  It
  creates the SDL window + `CCoreDX11Device`, points the engine's `D3DCompileFunc`
  at the shim's `D3Dcompile` (the vendored loader tries
  `LoadLibraryA("d3dcompiler_47.dll")`, which cannot exist here), calls
  `PhoenixInitTool`, imports the `.apx`, drains `ProcessGeneratorQueue` to
  completion, then runs
  `Project.Timeline->Render(frame)` + `Present()` against the song-derived clock.
* `src/platform/window_handle.cpp` + `win32_compat_types.h` — the window-handle
  resolver split out of `window_sdl.cpp` so the D3D11 shim can link it without
  dragging in BaseLib/Logger.  This was the real cause of the `phx_smoke` link
  failure: `window_sdl.cpp.o` was pulled in by the resolver reference and needed
  `_Logger`, `CRect`, `CVector2I`.
* All six `src/platform/*.cpp` compile clean.  Two needed a relative `BaseLib.h`
  include (putting `Bedrock/BaseLib` on the include path is NOT an option: its
  `String.h` shadows `<string.h>` on a case-insensitive filesystem and breaks
  libc++).
* `compat/Windows.h`: `FormatMessageA`/`GlobalAlloc`/`GlobalReAlloc`/`GlobalLock`/
  `GlobalUnlock`/`GlobalFree`/`GlobalSize` now carry `extern "C"` to match their
  definitions.

## Open work

1. `CphxMaterialSpline::CalculateValue` crashes around demo frames 14990-15010.
   `Material.cpp:220-224` walks all four `Splines[x]` with no null check, and one
   of them points at freed memory: a stack sample shows
   `CphxSpline::CalculateValue` taking the `KeyCount == 1` path (so `Keys` and
   `KeyCount` are still intact) and then loading *this object's own vptr* as
   `0x186678`, which is not a vtable - the classic signature of a block reused
   after free.  Reached only from `CphxEvent_RenderScene::Render` for the scene
   active in that window; frames 13000 and 20000 are fine.  Not yet diagnosed
   beyond that.
2. `--fixed-fps` cannot keep up with Clean Slate's 120 fps; it is an offline
   tool, not a playback mode (documented).
3. The MVX synth itself is still Windows-only, so the music comes from a
   pre-rendered track (see MILESTONE 4).

## Verified details worth not re-deriving

* Shader compilation is fast: 83 filters compile in **1.2 s total** (~14 ms
  each), so no on-disk shader cache is needed.
* Constant-buffer sizes from reflection match the engine's expectations
  (tech006: b0 = 928 B, b1 = 192 B).
* Vertex-input locations are **not compacted** by glslang, so the mapping is
  `location N <-> D3D11_INPUT_ELEMENT_DESC[N]` (see DESIGN-d3d11-shim.md §4).
* Resource generation after `.apx` import: mark timeline events as required,
  then drain `UpdateQueue` with `ProcessGeneratorQueue(40, device)` - exactly
  what `apEx/apEx/apEx.cpp:250-256` does per frame.

## Building

```sh
cmake -S . -B build
cmake --build build -j8
./build/phx_shadertool --entry p --stage frag <shader>.hlsl   # shader/reflection check
```

Optional but useful: `VK_LAYER_PATH="$(brew --prefix)/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d"`

## Known risks

* `CMakeLists.txt` is being edited by several agents concurrently; check that no
  target was lost after merging.
* MoltenVK's missing geometry shaders will visibly affect the techniques that
  use them (particles, some light passes).
* Procedural geometry depends on the exact MSVC `rand()` call sequence; that is
  reproduced by `src/platform/msvc_rand.cpp`.
