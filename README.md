# Neo-apEx-Player

A cross-platform **Vulkan player for apEx projects**, built by porting the
original [apEx demotool](https://github.com/Conspiracy/apEx-public) engine
rather than reimplementing it.

The target acceptance test is Conspiracy's **Clean Slate** (2023):

```sh
./build/neo_apex_player "/path/to/Projects/Clean Slate/cleanslate.apx"
```

## How it works

The apEx engine is a Windows/Direct3D 11 codebase (~90k lines) that speaks
D3D11 directly and has no platform seam. Instead of rewriting it, this project
**shadows the Windows headers** and implements the small part of the Win32,
D3D11, DXGI and D3DX10 APIs the engine actually uses:

```
compat/              drop-in replacements for <Windows.h>, <d3d11.h>,
                     <dxgi.h>, <D3Dcommon.h>, <D3Dcompiler.h>, <D3DX11.h>,
                     <D3DX10Math.h>, <tchar.h>, <wingdi.h>, ... They are put
                     first on the include path, so the vendored code compiles
                     unchanged.
src/rhi/             Vulkan bring-up (vk_core) and runtime HLSL -> SPIR-V
                     compilation with reflection (shader_compiler)
src/d3d11/           the D3D11 subset, implemented on Vulkan ("mini-DXVK"):
                     device, context, resources, shaders, states, swap chain
src/d3dx/            the portable replacement for d3dx10.lib
src/platform/        POSIX implementations: Win32 CRT/threading/file APIs,
                     the SDL3 window layer, a GDI subset on CoreText, stb
                     image decoding, MSVC-compatible rand()
src/player/          the player itself: project loading, the song clock, the
                     frame loop
third_party/apEx/    the vendored engine (Bedrock, CoRE2, Phoenix,
                     Phoenix_Tool), patched only through tools/apply_patches.py
```

Two layers come for free from this: Bedrock's **CoRE2** already contains a
complete D3D11 renderer backend (`DX11*.cpp`) written against exactly this API,
so it compiles and runs on the shim; and the whole **Phoenix** engine plus the
tool-side `.apx` loader (`CapexProject`) come along with it.

### The interesting parts

**Shaders are HLSL source, compiled at load time.** Every material pass and
texture filter is HLSL embedded in the `.apx`. `src/rhi/shader_compiler.cpp`
rewrites each resource declaration to carry an explicit `[[vk::binding(n,0)]]`
attribute and then compiles it with libshaderc (glslang's HLSL front end)
instead of fxc:

| D3D11 declaration | Vulkan binding |
|---|---|
| `cbuffer bN : register(bN)` | `N` |
| `float4 x : register(cN)` (implicit `$Globals`) | `0` — the same slot D3D11 uses |
| `Texture2D tN : register(tN)` | `64 + N` |
| `SamplerState sN : register(sN)` | `192 + N` |
| `RW… uN : register(uN)` | `208 + N` |

That mapping is what lets the engine's unmodified register-based code address
the right resources. glslang also emits **D3D constant-buffer packing**, not
std140 — `float t, aspect, xres, yres;` in a `cbuffer` lands at byte offsets
0/4/8/12, matching the engine's `memcpy`-style constant writes.

**The frame clock is the song.** The release player derives everything from the
music position: `frame = floor(song_milliseconds / 1000 * framerate)`, so the
timeline and the audio are one timebase. `src/player/song_clock.cpp` uses the
device's audio play cursor when music is playing and a monotonic wall clock
otherwise, and gets the song's length from the `.mvm` tick timeline (Clean Slate:
`step010padding.mvm`, 284.4357 s = 34132 frames at 120 fps).

**The music is the real thing.** MVX — "REALTIME BUZZ-BASED SYNTHESIZER" by
Gargaj/Conspiracy — is closed source: the release ships `mvx.h` and x86-32 MSVC
static libraries, nothing else. It cannot be ported to this player's platforms,
and it does not need to be. It runs natively on **32-bit Windows**, and GitHub's
`windows-latest` runners execute x86-32 through WoW64, so the
[`MVX (Windows)`](.github/workflows/mvx-windows.yml) workflow *is* the synth:
[`tools/mvx_render.cpp`](tools/mvx_render.cpp) links the shipped library, drives
it using the same call sequence as the tool's own offline renderer
(`phxSongMVM.cpp:OfflineRenderSong`), and writes both a WAV and the original's
`!CSA` SoundCache container, which the player reads and auto-discovers:

```
demo/step010padding.mvm
   └─ MVX (original closed-source library, on a Windows runner)
        └─ cleanslate.apxsndcache        ← 284.44 s, 2432 tick markers
             └─ ./build/neo_apex_player ... (macOS / MoltenVK / arm64)
```

Rendered output and the upload's own soundtrack correlate at **0.85** across all
284 s at a **23 ms** offset, which is the Opus codec's own delay — i.e. it is the
same recording, produced by the original code rather than copied from anywhere.

Two things about that sequence are easy to get wrong, and both are commented in
the harness: every machine's `Tick()` must be called by hand each tick
(`mvxRenderOffline()` is exported but does not do it and crashes), and the x87
control word must be set to single precision, because the DSP was written for
24-bit-mantissa arithmetic.

## Building

Requires macOS with Homebrew (`vulkan-loader`, `molten-vk`, `vulkan-headers`,
`shaderc`, `sdl3`; `vulkan-validationlayers` and `vulkan-tools` are optional
but recommended).

```sh
cmake -S . -B build -DAPEX_BUILD_PLAYER=ON
cmake --build build --target neo_apex_player -j8

./build/neo_apex_player "/path/to/cleanslate.apx"
```

Without `-DAPEX_BUILD_PLAYER=ON` only the renderer pieces are built, which is
much faster while working on the shim:

```sh
cmake -S . -B build && cmake --build build -j8
./build/phx_shadertool --entry p --stage frag <shader>.hlsl   # HLSL -> SPIR-V + reflection
./build/phx_smoke                                             # headless render smoke test
./build/phx_window_test                                       # SDL window + device
./build/phx_xml_test                                          # .apx XML round trip
./build/phx_gdi_test                                          # GDI glyph outlines
```

### Player options

```
--song FILE          .mvm to take the timeline from (default: the project's <songfile>)
--width N --height N pre-select this mode in the setup dialog's Resolution list;
                     without the dialog, the window size
--fullscreen         --no-ui only: start fullscreen
--windowed           pre-select the setup dialog's Fullscreen box off
--ui / --no-ui       show / skip the setup dialog (shown by default, as in the
                     release player; --no-ui is what scripts pass)
--frames N           render N presents and exit (with the default real-time clock
                     the demo still advances in real time and skips frames)
--start-frame N
--fixed-fps          advance exactly one demo frame per rendered frame.  For
                     deterministic offline rendering only: this player cannot
                     render Clean Slate at 120 fps, so it runs slower than real
                     time.  Omit it to watch the demo at its original speed.
--fps N              override the project framerate
--no-vsync
--music FILE         play a pre-rendered track (WAV, or the Windows tool's
                     .apxsndcache) and take the clock from its play cursor
--no-audio           never open an audio device
--frame-timeout MS   abort if one frame exceeds MS (default 5000; 0 disables)
--validate           enable Vulkan validation layers
--dump-frame N=FILE  write frame N to FILE as binary PPM (repeatable)
```

## Playing it

The repository ships the demo, so this is the whole thing:

```sh
# 1. once: build
cmake -S . -B build -DAPEX_BUILD_PLAYER=ON
cmake --build build --target neo_apex_player -j8

# 2. once: get the music.  The synth is Windows-only, so CI renders it (see
#    "The music is the real thing" above) and publishes the result.
gh run download --name cleanslate-music        # cleanslate-music.wav + .apxsndcache
mv cleanslate.apxsndcache demo/step010padding.apxsndcache

# 3. watch it
./build/neo_apex_player demo/cleanslate.apx
```

It plays the demo end to end — 284 s, 34132 frames at 120 fps — with the original
music, and exits at the end. **ESC** quits early, and so does closing the window;
those are the only controls once it is running, because they are the only controls
the Win32 release player had (`apEx/MinimalPlayer/WindowHandler.cpp`). There is no
pause or seek in the original, so there is none here.

### The setup dialog

`./build/neo_apex_player demo/cleanslate.apx` opens the release player's setup
dialog first, in its own window: **Resolution**, **Monitor Aspect**,
**Fullscreen**, **V-sync**, **GO!**, **Quit**, with the release and the group in
the frame above. This is not an addition — it is what the release player does
before it creates anything at all (`MinimalPlayer.cpp:147`,
`SetupDialog.cpp:195`), and the port keeps the same order and the same defaults
(Fullscreen and V-sync ticked, Resolution on the display's current mode, Monitor
Aspect on `(auto)`). **Enter** accepts, **Esc** cancels; the window has no
caption, so it is dragged by its client area, as the original's was.

It is SDL3 + Dear ImGui on its own `SDL_Renderer`: no Vulkan, no swap chain, no
device. It is torn down before the engine's window exists, which is what the
original's modal `DialogBox` amounts to. `--no-ui` skips it and takes the
settings from the command line instead, which is what the scripts in `ci/` and
`tools/` and the `apexcompare` helper pass.

Two of its answers reach the engine rather than just the window: the resolution
and the fullscreen flag go into `CCoreWindowParameters`, and **Monitor Aspect**
is the value `CapexRenderTargetCollection::UpdateCollection()` uses to letterbox
the demo's authored aspect into the window (see
`src/platform/phx_monitor_aspect.h`). `(auto)` — the default — means "whatever
display this is", which is what the port hard-coded before.

The cache is found automatically because it sits next to the song the project
names. `--music FILE` overrides it, and dropping `--music` and the cache gives the
silent wall-clock timing instead.

To watch it without downloading anything, the demo's own audio can be taken from
the [release upload](https://www.youtube.com/watch?v=O3T1-nadehU) instead — it is
the same music (measured: 0.85 waveform correlation across all 284 s):

```sh
ffmpeg -i "Clean Slate [O3T1-nadehU].webm" -vn -ac 2 -ar 48000 -c:a pcm_s16le music.wav
./build/neo_apex_player demo/cleanslate.apx --music music.wav
```

A frame-by-frame comparison against that video is what `apexcompare` in
[`.dshrc`](.dshrc) does: `source ./.dshrc && apexcompare 2999`.

## Reproducing the port

`third_party/apEx` is a copy of the original release with a set of small,
individually-reasoned patches — const-correctness for temporaries clang will
not bind, `uintptr_t` where the original stored a pointer in an `int`, dropped
tool-UI dependencies, and so on. They are all listed, with their reasons, in
`tools/apply_patches.py`:

```sh
python3 tools/apply_patches.py       # idempotent: a second run prints only "already"
./tools/revendor_patched.sh --force  # restore the patched files from the pristine release
```

## Platform status

* **macOS / MoltenVK** — the target platform; built and run here.
* **Windows** — would need `compat/` disabled (the real SDK headers would be
  used); the vendored code is the original Windows code, so this is the
  lowest-risk port.
* **Linux** — needs the case-sensitivity shim for `compat/` (see below) and a
  decision on the GDI subset.

Known platform caveats, all logged at runtime:

* **MoltenVK has no geometry shaders** (`VkPhysicalDeviceFeatures::geometryShader`
  is false). Pipelines that request one are created without that stage so the
  rest of the demo still renders.
* `VK_FORMAT_D24_UNORM_S8_UINT` is unsupported on this device; depth targets are
  substituted with `D32_SFLOAT_S8_UINT`.
* Mesh text is generated from **CoreText outlines**; the original used Windows
  GDI with fonts that are not on macOS, so substituted faces are logged and
  Arial Black comes out at a smaller scale (the font file's own win metrics).
* Music needs the `MVX (Windows)` workflow to have run once (or the Windows tool
  to have been run locally) to produce the `!CSA` cache; the player itself cannot
  synthesise, because the synth is x86-32 Windows code. Without the cache it runs
  on the wall clock, which reproduces the original frame timing exactly — only the
  sound is missing.

## Documentation

* [`docs/STATUS.md`](docs/STATUS.md) — where the port stands, what is verified
  and how.
* [`docs/DESIGN-d3d11-shim.md`](docs/DESIGN-d3d11-shim.md) — the D3D11-on-Vulkan
  contract: resource model, register mapping, pipeline caching, frame lifecycle.
* [`docs/DESIGN-player-app.md`](docs/DESIGN-player-app.md) — the player's
  startup sequence, frame loop and audio story.
* `docs/spec/` — reverse-engineered specifications produced from the original
  source: the `.apx` XML format, the engine render pipeline, the mesh system,
  the D3D11 API surface inventory, the MVX music format, and the platform-port
  gap analysis.

## Third-party content and attribution

`demo/cleanslate.apx` and `demo/step010padding.mvm` are Conspiracy's Clean Slate
project and music data, taken from the
[apEx source release](https://github.com/ConspiracyHu/apEx-public) and used here
under its **CC BY-NC 4.0** licence (`third_party/apEx/LICENSE.txt`). That licence
restricts commercial use, **not** redistribution: the files are committed so that
CI can load a real project and run the real MVX synthesiser rather than a stub.
The demo was made by Conspiracy; this project only plays it back on another
platform, and is itself non-commercial. The music is produced by their own MVX
synthesiser, not re-recorded.

## Licence

The vendored `third_party/apEx` code is the Conspiracy apEx source release and
is under the **CC BY-NC** licence in `third_party/apEx/LICENSE.txt`. This port
adds no commercial functionality and exists to satisfy curiosity and to learn
from, in the spirit of the demoscene — the same terms the original release asks
for.
