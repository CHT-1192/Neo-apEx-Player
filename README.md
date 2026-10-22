# Neo-apEx-Player

**Plays apEx projects on macOS.** For example Conspiracy's
[**Clean Slate**](https://www.pouet.net/prod.php?which=88524) — the 64k intro that
won the pc 64k compo at Revision 2021, by BoyC (code), Gargaj (audio) and zoom
(visuals). 284 seconds, 34132 frames at 120 fps, with its music, end to end.

It is a Vulkan player for `.apx` projects, built by porting the original
[apEx engine](https://github.com/Conspiracy/apEx-public) rather than
reimplementing it — the intro runs on the real engine code, through a
D3D11-on-Vulkan shim.

> [!IMPORTANT]
> **Unofficial.** Not affiliated with, endorsed by, or authorised by Conspiracy,
> who made apEx and Clean Slate. The engine, the project data and the music are
> their work; this project claims only its own source code, and ships no
> recording of the intro or its soundtrack. See [Licence](#licence).

## Quick start

macOS with Homebrew:

```sh
brew install molten-vk vulkan-loader vulkan-headers shaderc sdl3

cmake -S . -B build -DAPEX_BUILD_PLAYER=ON
cmake --build build --target neo_apex_player -j8

./build/neo_apex_player demo/cleanslate.apx
```

The demo is in the repository, so that is the whole thing. **Esc** quits; there
is no pause and no seek, because the original player had neither.

Without the music it still plays — silently, on the wall clock, which reproduces
the original's frame timing exactly. To get the sound:

```sh
gh run download --name cleanslate-music          # CI renders it on Windows
mv cleanslate.apxsndcache demo/step010padding.apxsndcache
```

The synthesizer is 32-bit Windows code, so it cannot run here; the cache carries
the tick markers the player uses to drive the picture from the audio.
`--music FILE` takes a 16-bit PCM WAV instead, if you have one.

## Recording it

No screen capture: the player writes finished frames to a pipe and ffmpeg encodes
them, so what you get is what was rendered.

```sh
./build/neo_apex_player demo/cleanslate.apx --headless --no-ui \
  --raw-video --width 1920 --height 1080 \
  | ffmpeg -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 120 -i - \
      -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p cleanslate.mp4
```

Or let a script read the size and rate from the player's own header:

```sh
.claude/skills/record-demo/record.sh demo/cleanslate.apx out.mp4 1920x1080
```

Two things to know: `-s` must match the announced size or ffmpeg silently shears
every frame, and raw RGB24 is 746 MB/s at 1080p120 — 3 GB/s at 4K120, which no
pipe will take. [The skill](.claude/skills/record-demo/SKILL.md) has the rest.

## Options

The ones worth knowing; `--help` lists everything.

| Option | What it does |
| --- | --- |
| `--no-ui` | Skip the setup dialog and start playing (what scripts pass) |
| `--music FILE` | Play a pre-rendered track and take the clock from it |
| `--fullscreen`, `--width N --height N` | Window mode; with `--no-ui`, the size |
| `--frames N`, `--start-frame N` | Render a bounded range |
| `--fixed-fps` | One demo frame per rendered frame — deterministic, not for watching |
| `--raw-video` | Frames to stdout as RGB24, for an encoder |
| `--headless` | Never show or focus a window |
| `--yield PCT` | Sleep PCT% of each frame's cost, so a long render leaves the machine usable |
| `--dump-frame N=FILE` | Write one frame as PPM (repeatable) |
| `--validate` | Vulkan validation layers |

Interactive runs open the release player's **setup dialog** first — Resolution,
Monitor Aspect, Fullscreen, V-sync, GO! — and `--no-ui` takes those from the
command line instead.

## What works, and what does not

Clean Slate runs correctly: loading screen, all 284 seconds, particles, text,
shaders, audio sync. The other bundled apEx projects load and render too, and the
port is clean under `VK_LAYER_KHRONOS_validation`.

Known limits, all logged at runtime:

* **No geometry shaders on MoltenVK.** Pipelines that ask for one are emulated —
  the shader's appends are run on the CPU and fed back as a fat vertex — and the
  emulation refuses anything it cannot do faithfully rather than drawing
  something wrong.
* **The music has to be rendered on Windows**, by CI or by hand: the synth is
  x86-32 Windows code.
* **D24 depth is unsupported** here and is substituted with `D32_SFLOAT_S8_UINT`.
* Text comes from **CoreText** outlines instead of Windows GDI, so substituted
  faces are logged and Arial Black comes out slightly smaller.

macOS is the target platform and the only one tested. Windows would work with
`compat/` disabled; Linux needs a case-sensitivity shim and a decision about the
GDI subset. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains how the port
is put together.

## Development

The renderer alone builds in seconds and is where most work happens:

```sh
cmake -S . -B build && cmake --build build -j8
./build/phx_smoke              # headless render, exercises the shim
./build/phx_xml_test           # .apx XML round trip
./build/phx_shadertool --entry p --stage frag <shader>.hlsl   # HLSL -> SPIR-V
```

`tools/apply_patches.py` is the only way the vendored engine is modified: every
patch is listed there with its reason, marked `[port]` in the source it touches,
and the script is idempotent.

## Documentation

* [`docs/STATUS.md`](docs/STATUS.md) — where the port stands, what is verified, and how
* [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how it works, and how the engine stays reproducible
* [`docs/DESIGN-d3d11-shim.md`](docs/DESIGN-d3d11-shim.md) — the D3D11-on-Vulkan contract
* [`docs/DESIGN-player-app.md`](docs/DESIGN-player-app.md) — startup, frame loop, audio
* [`docs/spec/`](docs/spec/) — the `.apx` format and engine internals, reverse-engineered from source
* [`.claude/skills/`](.claude/skills/) — agent skills for [recording](.claude/skills/record-demo/SKILL.md) and [authoring projects](.claude/skills/author-apx/SKILL.md)

## Licence

The port's own code — `src/`, `tools/`, `tests/`, `docs/` — is **Apache 2.0**.
The vendored engine, the project and the song are **Conspiracy's**, under
**CC BY-NC 4.0**: attribution, modifications indicated, **no commercial use**.

The repository as a whole is therefore not for commercial use, and, CC BY-NC not
being an OSI licence, not open source as a whole either.
[`LICENSES.md`](LICENSES.md) has the path-by-path map, including the one file in
`src/` that belongs to the other side.

If you hold the rights to something here and would rather it were not, it is one
file to remove — `LICENSES.md` says which, and the player also runs without music.
