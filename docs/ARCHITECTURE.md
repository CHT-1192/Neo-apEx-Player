# Architecture and getting the port reproducible

Two things that used to live in the README and are too long to keep there: how
the port works, and how the vendored engine stays reproducible. For where the
port stands, see [STATUS.md](STATUS.md); for the D3D11-on-Vulkan contract, see
[DESIGN-d3d11-shim.md](DESIGN-d3d11-shim.md).

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
[`MVX (Windows)`](../.github/workflows/mvx-windows.yml) workflow *is* the synth:
[`tools/mvx_render.cpp`](../tools/mvx_render.cpp) links the shipped library, drives
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


## Reference audio, and comparing frames

The synthesizer is Windows code, but the demo's own soundtrack can also be taken
from the [release upload](https://www.youtube.com/watch?v=O3T1-nadehU) — it is the
same music (measured: 0.85 waveform correlation across all 284 s):

```sh
ffmpeg -i "Clean Slate [O3T1-nadehU].webm" -vn -ac 2 -ar 48000 -c:a pcm_s16le music.wav
./build/neo_apex_player demo/cleanslate.apx --music music.wav
```

A frame-by-frame comparison against that video is what `apexcompare` in
[`.dshrc`](../.dshrc) does: `source ./.dshrc && apexcompare 2999`. Remember that
the video is **60.008 fps** while the demo is 120, so it resolves 16.7 ms and
half of any demo-frame pairing is a duplicated frame; for frame-exact work, use
`--raw-video` instead.
