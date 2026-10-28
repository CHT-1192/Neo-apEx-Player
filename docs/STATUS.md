# Project status

Snapshot of where the Neo-apEx-Player port stands. Read this first when
resuming; `docs/spec/*.md` holds the reverse-engineered formats and
`docs/DESIGN-d3d11-shim.md` the renderer contract.

## Goal

A cross-platform Vulkan player for the apEx demotool (Conspiracy, used since
Supermode in 2014; the acceptance title is Clean Slate, Revision 2021),
validated by loading and rendering `Projects/Clean Slate/cleanslate.apx`.

**Scope, stated plainly:** Clean Slate is a **64k intro** -- its released
executable holds engine, content and music in under 65,536 bytes.  This project
does not reproduce that and does not claim to.  apEx is an open-source release,
so what runs here is the work in the form its authors published it: the engine
sources (113,812 lines) built into a **2.8 MB** player, playing
`demo/cleanslate.apx` at 13.4 MB with 1.0 MB of song data.  Nothing is unpacked
-- the packed executable is a separate build product of the original Windows
toolchain, which this project neither produces nor consumes.  What is validated
here is the picture and the timeline on other platforms, and every "verified"
claim below is about that, not about size.

## Which songs the closed-source synth can render

The `MVX (Windows)` workflow now renders **all ten** `.mvm` files the apEx release
ships.  Run on 2026-10-22, it split them cleanly:

| Song | Project | Result |
| --- | --- | --- |
| `step010padding` | Clean Slate | **284.44 s** |
| `step010` | Clean Slate (alternate) | **275.90 s** |
| `dissipate_007c_noglitch` | One of These Days The Sky's Gonna Break | **169.83 s** |
| `off0021_things` | Offscreen Colonies | **244.80 s** |
| `veins022-rc3` | Vessel | **335.74 s** |
| `e0015_roughmaster` | Supermode | dies in `mvxSystem_LoadMusic` |
| `unintendedconsequence016` | Universal Sequence | dies in `mvxSystem_LoadMusic` |
| `impending-013-fullcut` | Darkness Lay Your Eyes Upon Me | dies in `mvxSystem_LoadMusic` |
| `purge015`, `purge015-sh` | When Silence Dims the Stars Above | die in `mvxSystem_LoadMusic` |

So four of the eight bundled projects can have their soundtrack rendered by this
tool and four cannot.  The failing five print the file banner and stop with **no
message and exit code 0**, which is why the workflow demands the output files
rather than trusting `errorlevel`.

It is not a format version mismatch that a header check could catch: all ten carry
the same `MVM\0` magic and their header fields decode plausibly (92-190 bpm,
1472-3104 ticks, 46-122 machines).  It cannot be fixed in this repository --
`third_party/apEx/MVX/` is `mvx.h` and the x86-32 static libraries, with no
implementation to read.  The five are listed as `KNOWN_UNLOADABLE` in the
workflow so the next person does not spend an evening proving the harness works.

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
| All 8 bundled projects | 11 `.apx` files across Clean Slate, Darkness Lay Your Eyes Upon Me, Offscreen Colonies, One of These Days The Sky's Gonna Break, Supermode, Universal Sequence, Vessel and When Silence Dims the Stars Above all load and render; 0 pipeline-creation failures each (MILESTONE 9) |

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

### Running the player must not take the desktop's focus

A sweep runs the player once per case, and SDL maps a window in
`SDL_CreateWindow` which macOS then activates -- so the sweep put a new window
in front of whoever was at the keyboard once per case, which makes the machine
unusable for anything else.  the real cause turned out to be
`SDL_HINT_MAC_BACKGROUND_APP`: its default of `"0"` means SDL **brings the
application to the foreground when it launches on macOS**, which has nothing to
do with the window -- hiding the window changed nothing.  `--headless` now sets
that hint (and exports the environment variable of the same name, because the
hint is only fully effective before `SDL_Init()`), creates the window with
`SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE`, and skips the
`SDL_ShowWindow`/`SDL_RaiseWindow` pair entirely, so nothing is mapped and
nothing is activated.  It logs both the window flags and the hint it got, which
is how the behaviour is checked rather than assumed:
`(hidden 1, not focusable 1)` and `SDL_MAC_BACKGROUND_APP=1`.  `tools/run_hostile_apx.py`,
`tools/dshrc-apexcompare.sh` and `.dshrc` all pass it.  (The crash dialogs were
a second, unrelated source: killing `CoreServicesUIAgent` dismissed the pending
ones and `defaults write com.apple.CrashReporter DialogType none` stops new
ones.)

## MILESTONE 10: a hostile .apx, and what it broke

Clean Slate is one project, written by one team, and the port had passed
everything it could throw at it while a real defect was still sitting there
(MILESTONE 9 only showed up on the *other* bundled demos).  So there is now a
generator for projects Clean Slate is not:
`tools/make_hostile_apx.py` writes one `.apx` per case, each a copy of a real
project with exactly one thing changed -- a geometry shader shape at or past the
edge of what the emulator accepts, a shader interface D3D11 links and Vulkan
does not, or a numeric value the loader trusts -- and
`tools/run_hostile_apx.py` runs them all with a per-case watchdog and reports
which ones crash, hang, or merely log errors.  Cases and their intent are in
`work/hostile/cases/CASES.md` (generated, gitignored); the four defects below all
came out of the first sweep.

**A geometry shader's input array was assumed to be called `input`.**
`TranslateGeometryShaderToVertex` emits a vertex entry point that runs the real
vertex shader once per input vertex into a local array and then walks the
geometry shader body; it declared that array as `input[N]` unconditionally, so
any shader whose parameter had another name produced HLSL that did not compile:

```
[d3d11] geometry shader emulation compiled nothing (g): gs-emu/vs_5_0:88: error: 'i' : unknown variable
```

The emulation was then refused, the stage dropped, and the message blamed the
shape rather than the code generator.  Every geometry shader in Clean Slate
happens to call the parameter `input`, so the port had only ever worked by luck.
The array is now declared under the shader's own parameter name, found as the
identifier before the `[` in the parameter list.

**The fat vertex can need more vertex attributes than the device has.**  The
emulation packs all K vertices of one input primitive into a single vertex by
repeating the input layout K times, so the attribute count is
`elements * inputVertices`.  Metal allows 31; the engine's six element layout
behind a `triangleadj` geometry shader that reads all six input vertices wants
36, and MoltenVK then refuses the pipeline:

```
program_source:52:43: error: 'attribute' attribute parameter is out of bounds: must be between 0 and 30
    float4 gsemu_packed_a5_UV [[attribute(34)]];
```

which loses the whole draw -- worse than dropping the geometry stage, which is
what happens to every shape the emulator refuses.  Two changes: the unused
location filter now applies to the emulated path as well (it was skipped there on
the false premise that the packed struct reads every member -- it holds the
shader's own input struct, so it reads exactly what that reads), and the
remaining count is taken from the *emulated* stage's reflection, which is the
number the driver will actually need, rather than the worst case.  A shape that
still does not fit is refused with the numbers spelled out.  The proper fix, not
done, is to carry the fat vertex in a storage buffer instead of attributes,
which has no limit at all.

**A hand-written .apx could kill the player during import, three ways.**  The
loader trusts the file, and the vendored tool-side code indexes arrays and
dereferences pointers with what it finds:

| XML | Where | Was |
| --- | --- | --- |
| `<event><Type>7</Type></event>` (or negative, or huge) | `apxProject.cpp:504` | `CreateEvent` returns NULL for a type outside `PHXEVENTTYPE`; the caller imported through it -- SIGSEGV |
| `<Object Type="99">` in a scene (or negative, or a `<model>` GUID that resolves to nothing) | `Scene_tool.cpp:254` | `AddObject` returns NULL and the caller does not check -- SIGSEGV |
| `<clipspline type="66">` | `Scene_tool.cpp:2102`, `Scene.cpp:303` | the type is read as a `PHXSPLINETYPE` with no range check and used as an index into `SplineResults[Spline_Count]` -- SIGBUS, every frame |

All three are fixed in `tools/apply_patches.py`: the import sites skip the
element and say so, and the read side in `Scene.cpp` keeps a silent bounds check
as well.  Eight cases that used to crash on import now exit 0, and Clean Slate's
0:31 / 1:06 / 1:18 frames still dump byte-identically.

**Still broken: a malformed geometry shader crashes glslang.**  `Append(1.0)`
(an argument that is not the stream's element type) and `Append(undefined())`
(an argument that does not resolve) both take glslang down inside
`HlslParseContext::finalizeAppendMethods()`, which calls `handleAssign` on a
null argument node:

```
std::__function::__func<glslang::HlslParseContext::handleAssign...>
glslang::HlslParseContext::handleAssign(...)
glslang::HlslParseContext::finalizeAppendMethods()
glslang::HlslParseContext::finish()
...
phxvk::CompileHLSL(...)
```

That is a bug in glslang, not in this port, and no real project ships a broken
geometry shader -- but the port should not die on one.  The fix is to stop
compiling the original `g` entry point altogether: nothing on this device can run
it, the shim only needs it as a registry key and a reflection carrier, and
*translated* sources have no `Append` methods left to finalise, so the crash
disappears with the compile.  That is a change to how the emulation is keyed, so
it is deliberately not bundled into this round.  Reproduce with
`work/hostile/cases/gs-append-scalar.apx`.

### Review fixes

A read-through of `src/rhi/gs_emulation.cpp` turned up four things worth
recording, two of them correctness bugs:

* **`ScanRange`'s non-literal-loop branch tested a variable it never wrote.**
  The recursion wrote into `scan` and the guard then looked at a fresh, empty
  `inner`, so `scan.unknown` was never set on that path and a loop whose trip
  count cannot be read was scanned as if it ran once.  A `RestartStrip` inside
  such a loop therefore placed the index buffer's restarts by guesswork.  The
  body is now scanned into `inner`, its Appends are merged once and a restart in
  it marks the shape unsupported.

* **`IsInsideConditional` was wrong in both directions.**  Anchored on a `}` it
  treated the *closed* block as the one containing the restart, so
  `if (a) {} else {} RestartStrip();` -- unconditional -- was called conditional
  and refused.  And it had no case for `{` at all, so a `RestartStrip` inside a
  *braced* branch was missed entirely: that is the dangerous direction, because
  missing it means a wrong picture rather than a refusal.  The walk now asks
  what opens the brace it is standing in, and steps over a `}` back to its
  matching `{` because that block has already ended.  Verified both ways: a
  braced conditional restart followed by Appends is refused, and a restart after
  an empty `if/else` is emulated.

* `BuildFatGeometry` took `firstIndex` but not D3D11's `BaseVertexLocation`, and
  the caller refused any draw with a non-zero base vertex instead.  The
  parameter is now in the API, added to every index the way the input assembler
  does, so such a draw emulates instead of losing its geometry stage.

* `ReadIntLiteral` accepted a lone `+`/`-` as a literal, because it only checked
  that a sign had been consumed: `atoi("+")` is 0, so `k < +abc` read as a trip
  count of zero.  It now requires at least one digit.

## Performance, measured against real hardware

**Read the numbers below with the machine's load in mind.**  Re-measuring the
same 600 frames later in the day gave 79 ms/frame at 320x180 and 277 ms/frame
for 100 frames of the same work -- a 3-5x spread on identical code, because
something else was using the machine.  Frame timings from a desktop that is
being used are not comparable with each other unless they were taken back to
back.

Somebody's laptop RTX 4060 runs this demo at **80 fps peak, 42 fps low** (a
comment from 2023).  A 10-core M5 sits at roughly that same tier -- between a
laptop 4060 and a 5060, mostly nearer the 4060.  So the reference point for
"is this port slow" is 12.5-24 ms per frame, not the 8.3 ms the demo was
authored against.

Measured on the M5, per frame, 1200 frames of Clean Slate (`--slow-frame-ms 25`):

```
frames slower than 25 ms: 930 of 1200
median 32 ms   p90 37 ms   max 1788 ms
over 100 ms: 2      over 250: 2      over 1000: 2
```

So steady state is **~1.3x the reference's low frame**, with a D3D11-to-Vulkan
translation layer and the geometry-shader emulation in between.  That is not a
frame-rate problem.  What it is, is **two stalls**: frame 0 (1788 ms, loading)
and frame 341 (1456-1026 ms), and frame 341 is simply the first frame of event
20 -- a new event starting.  Its breakdown says where those milliseconds are:

```
slow frame 341: 672 ms (update 645, timeline 7, generator 0, dump 0, present 20)
```

`update` is the engine initialising the event; `timeline` -- every draw we issue
-- is single-digit milliseconds.  So the stall is the original's own lazy event
initialisation, not the shim.

Two dead ends worth recording so nobody repeats them:

* **`--preheat` halves the event stall and is opt-in, not default.**  Calling
  `ForceUpdateContent()`/`UpdateMaterialState()` for every event before playback
  moves the engine's `update` half of the hitch into the loading screen
  (update 692 -> 0 ms at frame 341) but not the `timeline` half, which is the
  event's first *draw* -- pipelines, descriptors, texture uploads.  Net: the
  hitch goes 1412 -> 805 ms on Clean Slate, and loading goes 5.4 -> 7.0 s of
  generation.  Half a second for 1.6 s of loading is not a default, so
  `--preheat` turns it on.  My first read of this measurement said the stall was
  gone, because the grep used to filter the log was `[0-9]{4,}` and quietly
  dropped every frame under a second -- the same class of mistake as the
  single-frame dump above, committed by me twice in one session.

* **A persistent `VkPipelineCache` was implemented, measured, and reverted.**  It
  looked like a 5x win on two frames (timeline 1357 -> 189 ms at frame 0), but
  that measurement was contaminated: the first run after a rebuild pays Metal's
  shader compilation regardless, and MoltenVK already caches what it can.  Three
  runs each, cold and warm, put `timeline` at 9-13 ms cold and 4-7 ms warm, and
  the wall clock for 420 frames never moved outside 16.8-18.5 s for either.  The
  code did not pay for itself and was removed.

* **Sampling the process before it has finished loading measures the loader.**
  A `sample(1)` taken 3 s in reported 69% in `vkWaitForFences` and 31% in
  `PrepareDraw`, which looked like a per-frame pipeline rebuild; it was the
  texture generator and a one-off compilation.  Wait for steady state.

`--slow-frame-ms MS` now exists to make this reproducible: `0` reports every
frame with the update/timeline/generator/dump/present split, and the default
(250) keeps the log quiet.

## Two things about the reference video that invalidate naive frame comparisons

`ffprobe` on the release video:

    width=3840 height=2160  r_frame_rate=7561/126  (60.008 fps)

The demo runs at **120 fps**, so the reference is a **60 fps capture of it**.
Consequences, both of which have already caused wrong conclusions here:

* Sampling the video at demo-frame granularity (1/120 s) hits the SAME video
  frame twice in a row about half the time.  Successive samples came back
  bit-identical (`mean abs diff 0.00`) and looked like "the port is ahead" or
  "the port is behind" when they were showing an artefact of the sampling.
* The video resolves 16.7 ms.  A claim like "the port runs 0.25 s early" is 15
  video frames -- measurable, but only by aligning a window, not by eyeballing a
  pair of frames.

Also worth knowing: `--dump-frame N=FILE` matches the **iteration index**, not the
clock frame (`d.frame == rendered`), so with `--start-frame 3600` the dump you
want is `0=FILE`, not `3600=FILE`.  Asking for 3600 writes nothing and says
nothing.

## Particles render. The 3:00 cloud is about which emitter is live, not the engine

With the frame number and the particle life added to the diagnostic, a run over
the first particle event says:

    frame 16074: alive 571, emit/s 64.00, life 4.00, spawns 14277
    frame 16141: alive 281, emit/s 25.00, life 40.00, spawns 14434
    peak alive 584

and frame 16100, rendered and put beside the same frame of the reference, shows
the same clock, the same shadow, and **the same dust motes** in both.  The
particle path is correct end to end: they are born, they age, they are drawn.

So the 3:00 difference is not the particle engine.  At frame 21600 the port's
emitter is at the end of its life -- the emission spline has decayed to zero
there -- and the reference still has a cloud.  The question left is which
emitter or sub-scene should be live at that instant, which is a timelines
question, not a simulation one.  Earlier theories that the emitted mesh is not
drawn, that the life spline reads zero, or that the sub-scene phase is a fast
one-second loop are all dead, each with a measurement.

## Particles do spawn now; 3:00 is a phase question, not a rendering one

After the three particle fixes, a full low-resolution playthrough reports
**peak 1000 live particles** (of 1024 slots, so the emitter is nearly saturated)
and `spawns` climbing into the hundreds.  The system works.

Frame 21600 still renders without the cloud, and the reason is now specific
rather than mysterious.  A sub-scene loops on
`fabs(fmod(SplineResults[Spline_SubScene_Time] + offset, 1.0))`, and at that
frame the port's phase is **0.964** -- just short of the wrap.  The emission
spline is nearly zero there, so nothing new is born, and whatever was alive has
aged out.

The reference has a cloud at that instant, which means its phase is somewhere the
emission is high.  A 1-second loop phase is sensitive to exactly the kind of
error the port is already known to have: it runs about **0.25 s early** on scene
changes (recorded under the open 55 s item).  0.25 s is a quarter of that loop,
which is the right order of magnitude to move 0.964 to a live phase.

So the remaining 3:00 difference is very likely the timing offset, not the
particle path -- which is why it is being tracked as part of that item rather
than patched here.  Verifying it means another full playthrough (twenty minutes
of the machine) or reading the sub-scene time spline analytically; the second is
cheaper and is what to try next.

## Particles were never simulated at all

`grep SimulateParticles()` over the vendored tree returns exactly one hit: the
definition.  The editor's frame loop (`../apEx/apExRoot.cpp`, not vendored) was
the only caller, and the player replaced that loop without it -- so no particle
system ever advanced.  `Ticks` never accumulated, no particle was ever born, and
an emitter that draws a sub-scene or a mesh per particle drew nothing.

Wired into the player's per-active-scene loop.  Two consequences worth knowing:

* The particle step is taken from the wall clock (`globalTimer.GetTime()` inside
  `CphxScene_Tool::SimulateParticles`).  That is right in real time and wrong for
  `--fixed-fps`, which draws frames far faster than they play: at 1100 fps the
  step is 0.9 ms instead of 8.3 and everything moves nine times too slowly.  The
  player now feeds one frame's worth of time when it steps at a fixed rate, via a
  `[port]` global in Scene_tool.cpp; zero means "wall clock", the original
  behaviour.

* With the step fixed, the emitter measured at 3:00 still reports
  `emit/s 0.0000`, so that particular cloud is still missing.  The project has 57
  `<clipspline type="40">` (emission) entries, so the spline evaluates rather
  than being absent; what is left to explain is why it reads zero at that moment.

The 0:31 frame is unchanged by this (3.286 against the video, in line with the
3.09-3.23 range recorded before), which is what a fix that only affects particle
systems should look like.

## The 3:00 difference is a missing particle cloud, not a rounding error

Frame 21600 (3:00) had been recorded as "13.195 away from the reference" and
left unexplained.  Putting the two frames side by side answers it: the reference
is a dense cloud of red spheres and this port draws **one**.  That is not a
metric drift, it is most of the frame's content gone.

Two leads are dead already:

* the clip-spline bounds check does not reject any valid type -- a run logs zero
  "clip spline type ... outside" lines, and the project's types (1..20) are all
  inside `Spline_Count`;
* it is not a precision or timing difference either: the rest of the frame
  (background, lighting, the one sphere) is in the right place.

What is left is the `EmitedScene` path: a CPU particle emitter can emit a whole
sub-scene per particle (`Scene_tool.cpp`'s ForceUpdateContent calls
`EmitedScene->ForceUpdateContent()`), and that is what the spheres are.  One
sphere instead of a cloud points at the instance/clip expansion rather than at
the draw.

## The loading screen renders. The tool said otherwise.

The loading screen was recorded here as blank -- pure black for
`precalcminimal`, noise for `precalcphoenix_shader` -- for months, and it was
wrong.  Sampling every frame of a load instead of one frame at one moment:

```
13 sampled frames (every 8th), Clean Slate:
  render target   mean 0.24 -> 0.85, max 186, black frames: 0 of 13
  back buffer     mean 0.32 -> 1.15, max 255, black frames: 0 of 13
```

The bar is there from the first sampled frame to the last, and it is presented.
`--dump-frame loading=FILE` wrote **one** frame, the first with `pos >= 0.5`, and
that frame came out black with both buffers empty -- reproducibly, at
`pos = 0.5068` after 73 precalc renders, while a full-frame scan shows that same
iteration carrying the bar.  So the one instrument that ever reported this was
the broken one.  It now writes a short sequence (`FILE.<n>` and
`FILE.<n>.target.ppm`, every 8th frame) which is both harder to fool and enough
to see the bar move.

Four hypotheses died on the way, each to a single run rather than an argument:

* the pass has no render target -- it has one, 1280x720;
* the pass has no pixel shader -- it has one, and cbuffer b0 is
  `{t, 1.7778, 1280, 720}` with `t` climbing through the load;
* the shader-toy draws into one target and the blit reads another -- `[blit]`
  and `[shadertoy]` print the same pointer;
* multisampled render targets -- both project loaders ask for
  `D3D11_RTV_DIMENSION_TEXTURE2DMS` on single-sample textures and the shim has
  always corrected that, so it cannot separate the loader that works from the
  one that does not.

Two mistakes of mine are worth recording because they nearly turned into
"fixes": a `grep '^\[quad\]'` cannot match a line the logger prefixes with
`[phxvk warn] `, which made three working probes look silent and produced a
confident, wrong conclusion that the engine never reached our device context;
and the single-frame dump above.

**Still unconfirmed: the window.** Everything here is measured headless, on the
render target and the presented back buffer.  Running the player windowed steals
the desktop's focus, so it is not something this project does; if the loading
screen is blank on screen, the next step is a windowed capture on a machine that
can afford it, not another headless experiment.

## MILESTONE 11: weird but legal geometry shaders

The corpus above started from deliberately *invalid* shaders.  This round is the
opposite: shaders that compile, that fxc and D3D11 accept, and that the emulator
still got wrong.  Nine of them (`gs-weird-*`):

| Shader | Was | Now |
| --- | --- | --- |
| `void g(VSOUT i[3], inout TriangleStream<VSOUT> o)` -- the primitive modifier is optional and HLSL defaults it to `triangle` | refused, "input primitive is not point/line/triangle" | emulated; the shape reader reads `type name` first and only treats a leading word as a modifier if it is one |
| `TriangleStream < VSOUT > o`, `i [ 3 ]`, `o . Append ( i [ 0 ] )` | refused; the stream element type kept its spaces (`" VSOUT " has no Position member`), and the `.` receiver was only stripped when there was no space around the dot | emulated; tokens are trimmed and the receiver is skipped on both sides of the dot |
| `uint pid : SV_PrimitiveID`, `uint inst : SV_GSInstanceID` | the translated stage named `pid`, which it never defined: `error: 'pid' : unknown variable`, emulation refused | emulated. The emulation draws one input primitive as `slots` consecutive vertices, so `SV_PrimitiveID` is `gsemu_vid / slots` and there is no GS instancing, so `SV_GSInstanceID` is 0. Any *other* system value is now refused by name in `AnalyseGeometryShader` |
| `#define AP(x) o.Append(x)` | refused as "the geometry shader appends nothing" -- the scan reads the source before the preprocessor runs | still not emulated, but it says so: *"no Append call is visible to a text scan -- if the Append is written through a #define, the emulation cannot see it"*. Fixing it properly means analysing the preprocessed source, which moves the whole translation onto that source |
| Appends in mutually exclusive `switch` branches, against `maxvertexcount(3)` | refused as "more Append calls than maxvertexcount allows" | still refused (the count is static and cannot tell branches apart), but the message says the count is static |
| **A `RestartStrip` inside a branch, followed by more Appends** | **emulated, with a wrong picture.** The index buffer's primitive restarts are decided statically, so a restart the stream does not take splits a strip that should have stayed whole -- 6 vertices in one strip (four triangles) drawn as two strips of three (two triangles) | refused: *"a RestartStrip inside a branch is followed by more Appends, so the strip layout is only known at run time"*. An unconditional restart with Appends after it is still emulated (`gs-6-restart-middle`), and so is Clean Slate's wireframe, whose conditional restart is its last statement -- nothing can follow it, so the static layout is exact |

That last row is the one worth remembering: it was not a refusal that was too
strict, it was a **wrong picture**, which is the failure mode this file exists to
avoid.  The check is deliberately narrow -- it walks out of the enclosing blocks
from the `RestartStrip`, including braceless `if (...) o.RestartStrip();`, and
only refuses when an `Append` follows.

## MILESTONE 9: D3D11 links shaders by signature, Vulkan links them by location

Running the other bundled projects rather than only Clean Slate turned up a
pipeline that could not be created in **10 of the 11 of them**:

```
[phxvk error] validation: VK_ERROR_INITIALIZATION_FAILED: Render pipeline compile failed:
Fragment input(s) `user(locn0)` mismatching vertex shader output type(s) or not written by vertex shader.
[phxvk error] vkCreateGraphicsPipelines failed: VK_ERROR_INITIALIZATION_FAILED (topology 4, 1 RTs, depth 0)
```

**Geometry shaders are not involved**, which is worth recording because that is
where the search goes first. Over all 11 projects the same audit finds exactly
one failing pipeline per project, always the same shader pair and always with
`emu=0`: the engine's own 2D quad vertex shader (`shader[]` in
`Phoenix/phxEngine.cpp:149`, entry point `u`) writing `float2 texcoord0`, against
a texture-generator or loading-screen pixel shader that declares
`float4 texcoord0`. The three geometry-shader pipelines that exist outside Clean
Slate (two in Universal Sequence, one in Clean Slate) all emulate, none are
refused, and none fail. `--validate` gives the pair's identity directly.

D3D11 links a draw's shaders by **signature**: the pixel shader's input
signature is matched against the vertex shader's output signature by semantic
name and index, and a component the vertex shader never wrote reads as 0 (and
`.w` as 1). A pixel shader declaring `float4 uv : TEXCOORD0` is therefore
perfectly happy with a vertex shader that wrote `float2 uv : TEXCOORD0`. Vulkan
links by **location** and requires the same type on both sides, so SPIRV-Cross
refuses the pipeline and the draw is dropped.

`src/rhi/shader_interface.cpp` lowers the D3D11 rule onto Vulkan: every
floating point or integer varying a stage declares is widened to four
components, padding what the shader did not write exactly as D3D11 reads it.
Four components on both sides always match, and it is what a D3D11 signature
register is anyway -- four components plus a write mask, and Vulkan has no mask.
It runs once per shader at creation (`d3d11_shader.cpp`), so it does not depend
on which vertex and pixel shader end up in the same pipeline, and it also covers
the emulated geometry-shader vertex stage, which is created through the same
path. A shader whose varyings are all four wide already is returned untouched,
byte for byte.

Two SPIR-V details are easy to get wrong and are what the test pins:

* A varying's `OpVariable` cannot be retyped in place. SPIR-V forbids a
  duplicate `OpTypeVector %float 4`, so the existing four component type has to
  be reused -- and glslang interleaves each type with the variable that uses it,
  so that type is often declared *below* the variable. The touched variables are
  moved to the end of the types/constants/globals section, after the new types.
* A load has to keep the type the shader body was written against, so the vec4
  load gets a fresh id and an `OpVectorShuffle` under the *original* id narrows
  it back; a store is preceded by an `OpCompositeConstruct`. An access chain
  into a varying needs no change at all, because widening a `vecN` leaves
  components `0..N-1` where they were.

**The fix is pixel-neutral on everything measured**, which is the point: frames
300, 3724 (0:31), 7920 (1:06) and 9360 (1:18) of Clean Slate, and frames 300 and
1500 of Supermode, Offscreen Colonies and Darkness, dump byte-identically with
and without it, against a binary built from the previous commit. What it changes
is that the engine's texture-generator draw now happens instead of being
dropped.

`tests/shader_interface_test.cpp` compiles the real pair and checks both
directions (a narrow vertex stage widened, a narrow pixel stage narrowed), that
a four component shader comes back untouched, and that a stage with no varying
interface is left alone.

Two further defects found by the same sweep, and where they stand:

* **`Projects/Clean Slate/Precalc.apx` segfaulted on startup**, in `main`, before
  a frame. `CString::Initialize()` sets its buffer to `NULL`
  (`BaseLib/String.cpp:3-10`), and a project with no `<songfile>` -- which this
  one has none of -- handed that `NULL` to `std::string`. Fixed.
* **The loading screen drawn *during* a load is blank** -- black for a project
  using `precalcminimal`, uniform noise for one using `precalcphoenix_shader` --
  although the same project renders its bar correctly when played as a demo.
  This is pre-existing (identical with and without the change above) and is NOT
  yet fixed. It is reproducible now: `--dump-frame loading=FILE` writes the
  loading frame, which the frame loop's `--dump-frame N` cannot reach because
  the loading screen is presented from inside the resource-generation loop. The
  mechanism is `CphxEvent_Shadertoy::Render` (`Phoenix/Timeline.cpp:33-123`)
  drawing a 6-vertex quad with the first 16 bytes of `SceneDataBuffer` set to
  `{t, aspect, targetWidth, targetHeight}`, which is what
  `precalcminimal`'s `cbuffer b{float4 i}` reads for the bar's length (`i.z`)
  and its progress (`i.x`).

* **`Projects/Offscreen Colonies/off0021_things.mvm` is rejected by the song
  reader** as `degenerate tempo (tpb or bpm is zero)`, so both of that project's
  `.apx` files run on the wall clock. Not investigated; every other bundled
  `.mvm` parses.

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

What is left, as of the last verified state.  Each item says what would close it,
because "needs investigation" is how a list like this rots.

1. **Frame 21600 (3:00) renders without the reference's particle cloud.**  The
   particle path is not the problem any more: a low-resolution playthrough peaks
   at **1000 live particles**, `emit/s` reads 64 and 128 where it used to read 0,
   and frame 16100 side by side with the reference shows the same dust motes.
   What is left is a *timelines* question -- which emitter or sub-scene should be
   live at that instant; at 21600 the port's emitter is at the end of its life
   and the emission spline has decayed to zero there.
   *Closing it:* one full playthrough at 320x180 (~4 min), comparing port and
   reference frame by frame across the particle section.

2. **The other seven projects have run, but not been compared.**  All eight
   bundled projects now load and render **3000 frames** (25 s at 120 fps) with
   exit code 0 and content on the last frame:

   | Project | exit | `[import]` lines | peak byte at frame 3000 |
   | --- | --- | --- | --- |
   | Clean Slate | 0 | 0 | 0 (still fading in) |
   | Darkness Lay Your Eyes Upon Me | 0 | 0 | 114 |
   | Offscreen Colonies | 0 | 0 | 255 |
   | One of These Days The Sky's Gonna Break | 0 | 0 | 131 |
   | Supermode | 0 | 0 | 255 |
   | Universal Sequence | 0 | 0 | 253 |
   | Vessel | 0 | 98 | 245 |
   | When Silence Dims the Stars Above | 0 | 0 | 251 |

   Vessel's 98 lines are **one GUID referenced by 98 events**, the same tolerated
   class as the 35 that `demo/cleanslate.apx` reports -- a shadertoy parameter the
   imported technique does not carry.

   What this is **not**: a comparison against each project's own video.  Those
   are not in the repository and are gigabytes each, so for the seven,
   *correctness* is still asserted rather than measured; what is measured is that
   the importer accepts each file and the renderer produces frames.  The README
   says "load and render" and now has a table behind it.
   *Closing it:* obtain one reference per project and run the dump-and-compare
   loop that was used for Clean Slate.

3. ~~Geometry shader coverage~~ -- **closed**.  All three holes the list used to
   carry are gone:

   * `Append` counting follows the longest path through `if`/`else` and `switch`
     (through fallthrough) instead of summing mutually exclusive arms;
   * the analysis runs on the **preprocessed** source, so an `Append` written
     through a `#define` is seen, and the translated stage is generated from that
     same text so the two cannot disagree;
   * the fat vertex now travels in a **storage buffer** (`StructuredBuffer<...>`
     at `register(t15)`, bound by the shim at `kBindingTexture + 15`), not in
     vertex attributes, so no shape is refused for its attribute count.  The
     shader generation, the buffer usage and the descriptor write are all in this
     change; the demo's own geometry shader frame is byte-identical before and
     after it, which is how the structured-buffer element layout was checked
     against the CPU-side vertex layout rather than reasoned about.

   Evidence, all measured on this machine: `phx_smoke` 9/9, including a case that
   asks for **33 attribute locations on a device that allows 31** -- impossible
   under the attribute scheme, and rendering now; plus the emulated path's demo
   frame and the 0:31 regression frame, both byte-identical.

4. **Linux builds and passes its tests; nothing has played Clean Slate on it.**
   The `Linux` workflow configures with clang, builds every target, passes 40 CPU
   checks and renders 7/7 smoke cases on Mesa's lavapipe (which, unlike MoltenVK,
   does have geometry shaders).  What that is not: a demonstration that the demo
   runs there.  No real GPU has run it, and the FreeType text backend has only
   been exercised as far as compiling.
   *Closing it:* run the player on a Linux machine with a GPU and compare a frame.

   Windows is **not** on this list and is not a gap.  The engine is native
   Windows software and the original player runs there; building this shim for
   Windows would be fixing a problem nobody has.  (It would also be a second
   backend rather than a flag: `compat/` off, the engine's own D3D11 in.  MinGW
   cannot compile the vendored code at all -- it needs clang's
   `-fdelayed-template-parsing` -- so clang-cl would be the only option.)

5. **Five of the ten shipped songs cannot be loaded by the closed-source synth**
   (table above).  Not fixable here: the release has `mvx.h` and the x86-32
   library, no implementation.  Recorded so nobody re-derives it.

6. **`--fixed-fps` still cannot hold 120 fps** at the resolutions tested, so it
   remains an offline rendering mode rather than a playback mode.  Two frames in
   flight took a 320x180 frame from 17.99 ms to 9.61 ms against an 8.33 ms budget
   -- close, not met at 720p.

Fixed since this list was first written:

* **The "0.25 s early" scene-change offset was a sampling artefact.**  Window
  alignment over 60 frames at 30 s, and again at 55 s, puts the port within
  **+17 to +33 ms** of the reference, and the correlation peak is flat across
  +/-2 frames because the release video resolves only 16.7 ms.  Two independent
  windows agree.  The original reading came from comparing single frames on a
  60 fps video, where adjacent demo frames are very often the same video frame --
  which is the trap recorded further up this file.

* **The frames 14990-15010 crash no longer reproduces.**  It was
  `CphxMaterialSpline::CalculateValue` walking a freed spline, and two full-length
  runs since (16100 and 21601 frames) pass straight through that window.
* **The loading screen renders**, and the setup dialog's resize crash is handled
  (MILESTONE 5, MILESTONE 8).
* **The MVX synth is still Windows-only** but no longer a manual one-off:
  `MVX (Windows)` has a matrix over every shipped `.mvm`.

## The "state leak" that was not one

Writing the wide-input smoke case turned up what looked like a shim bug: the case
rendered correctly alone and rendered nothing when it ran after the GenerateMips
case, with the draw still logged and `renderingActive`, the render-target count
and the pipeline handle all identical between the two runs.  `ClearState()` fixed
it, which made it look like state was surviving where it should not.

It was **the test**: viewport and scissor are D3D11 context state and persist
across draws, case 7 leaves a 64x64 scissor behind, and the new case set only the
viewport -- so its centre pixel fell outside the inherited scissor and came back
black.  The file had documented exactly this two hundred lines above, in the
comment on `SetFullViewport()`: "every case sets both: case 2 narrows the scissor
to one 64px strip, and without this the cases after it would inherit that
rectangle."

Recorded because the first version of this note claimed the shim was leaking
state, and the probe that would have shown it (a scissor of 64x64) took ten
minutes once it was actually looked at.

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
