# Player application — integration contract

How the player drives the reused engine. Written after reading
`Phoenix_Tool/apxProject.cpp`, `Phoenix_Tool/phxResource.cpp` and the tool's
main loop (`~/Repos/apEx-public/apEx/apEx/apEx.cpp:255`).

## Startup sequence

```
1.  CCoreWindowHandler::Init(...)            (src/platform/window_sdl.cpp, SDL3)
2.  CCoreDevice::Initialize(Window)          (Bedrock/CoRE2, our Vulkan backend)
3.  phxDev      = device->GetHandle();       // ID3D11Device*   (our shim)
    phxContext  = device->GetContext();      // ID3D11DeviceContext*
4.  InitializePhoenix()                      // apEx/Phoenix/phxEngine.cpp
    - creates the shared constant buffers, the texgen quad, the render
      pixel/vertex shaders and the sampler states
5.  Project.Import(apxPath, nullptr)         // CapexProject (tool classes)
    - parses the XML, builds CapexTexGenPage / CphxMaterial_Tool /
      CphxModel_Tool / CphxScene_Tool / CphxTimeline_Tool / CphxEvent_Tool
    - the `hwnd` argument is only used by the MVX music path, which this port
      does not provide (see "Audio" below)
6.  Project.UpdateRTCollections()            // from Import()
7.  mark everything the timeline needs as required:
        ClearRequiredFlagForAllResources();
        for each event in Project.Timeline->Timeline->Events:
            event->MarkAsRequired();
    then drain the generator:
        while (UpdateQueue.NumItems()) ProcessGeneratorQueue(40, device);
    (this is exactly what apEx/apEx/apEx.cpp:250-256 does per frame, we just do
    it to completion once at load time)
8.  Configure the song clock from the project's <songfile> (see below)
```

## Frame loop

```
frame = clock.Frame()                     // floor(song_ms / 1000 * framerate)
Project.Timeline->Timeline->Render(frame) // CphxTimeline::Render, Timeline.cpp:318
device->Present()                         // vsync
```

`CphxTimeline::Render` does the whole frame: clears every render target and the
depth buffer, evaluates each event whose `StartFrame <= frame < EndFrame`,
dispatches it, and finally blits the composite target to the back buffer in a
centred 1:1 viewport.  Nothing else is needed from us.

Events can require GPU resources that were not marked at load time; the tool
re-arms `RequestContent()` every frame, so the loop also runs
`ProcessGeneratorQueue(40, device)` once per frame before rendering (cheap when
the queue is empty).

## Audio

The MVX synthesiser is closed-source 32-bit Windows object code with no
portable source (`docs/spec/music-mvx.md` §4), so the music **cannot be
synthesised** by this player.  Two consequences:

* The demo clock is unaffected.  The release player's frame number is
  `floor(song_milliseconds/1000*fps)` and the song's timeline is fully
  recoverable from the `.mvm` header, so `src/player/song_clock.cpp` reproduces
  the original timing exactly with a monotonic clock (verified: Clean Slate's
  `step010padding.mvm` = 284.4357 s = 34132 frames at 120 fps).
* Audio output is therefore *optional* and supplied externally:
  - `--audio <file>` plays a user-provided OGG/MP3/WAV through SDL3 audio and
    drives the clock from the audio position instead of the wall clock, or
  - `--audio-cache <file>` loads an `.apxsndcache` PCM dump (format documented
    in the music spec §6) produced by the original tool.
  Both paths feed `SongClock::SetAudioPositionMs()`.

## Command line

```
neo_apex_player <project.apx> [options]
  --width N --height N        window size (default: derived from AspectX/AspectY)
  --fullscreen
  --frames N                  render exactly N frames and exit (deterministic;
                              implies --fixed-step)
  --start-frame N             begin at a given frame
  --fixed-fps N               ignore the clock, advance one frame per present
  --audio FILE | --audio-cache FILE
  --dump-frame N=path.png     write one frame to disk
  --validate                  enable Vulkan validation layers
  --vsync | --no-vsync
```

## First milestone / acceptance test

```
./build/neo_apex_player "~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx" \
    --frames 600 --no-vsync
```
must
* load the project without unresolved-GUID or missing-resource errors,
* finish `GenerateAll` (texture pages, materials, models, scenes all built),
* render frames 0..599 (the first ~5 s of Clean Slate) and present them,
* exit with code 0.

The first frames of Clean Slate are a mostly black screen with an off-screen
composite target; frame ~600 starts the first visible content, so `--frames`
output is also dumped to PNG for eyeballing.
