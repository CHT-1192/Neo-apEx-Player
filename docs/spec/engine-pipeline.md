# apEx / Phoenix engine pipeline — reimplementation spec for a headless-capable Vulkan player

Source of truth (READ-ONLY): `~/Repos/apEx-public/`
Primary data: `Projects/Clean Slate/cleanslate.apx` (XML project, 120 fps, 16:9, song `step010padding.mvm`).

This document specifies **the runtime** (what a player must do), and uses **the tool** only where the
runtime is incomplete or where the tool defines the data format.

Confidence markers used throughout:

* **[VERIFIED]** — read directly from source, or computed from a real project file.
* **[INFERRED]** — consistent with all evidence, not directly proven.
* **[UNCERTAIN]** — plausible reading; confirm before relying on it.

---

## 0. Two builds, one engine

| | `apEx` editor (tool) | `MinimalPlayer` (release player) |
|---|---|---|
| entry | `apEx/apEx/apEx.cpp:176 WinMain` | `apEx/MinimalPlayer/MinimalPlayer.cpp:85 WinMain` |
| config | `PhoenixConfig_Full.h` | `PhoenixConfig.h` (`PHX_MINIMAL_BUILD`) |
| project source | XML `.apx` (`CapexProject::Import`) | packed binary `.64k` blob (`CphxProject::LoadProject`) |
| time source | MVX play position / `globalTimer` / fixed step | `MUSIC_GETSYNC()` = MVX song position in ms |
| frame advance | `Project.Timeline->Render((TF32)frame)` | `Demo.Render(MUSIC_GETSYNC())` |
| scene → draw list | tool rebuilds each draw (`CphxScene_Tool::SortRenderLayers`) | rebuilt inside `CphxScene::UpdateSceneGraph` every event render |
| backbuffer blit | UI `DrawRect` with the target texture | engine does it in `CphxTimeline::Render` |

The **engine core is identical**: `CphxProject` / `CphxTimeline` / `CphxScene` / `CphxObject` /
`CphxMaterial*` / `CphxSpline*` / `CphxRenderLayer*` all live in `apEx/Phoenix/` and are compiled
into both. Only loading (`Project.cpp` vs `apxProject.cpp`) and time driving differ.

### 0.1 File map (runtime-relevant)

| file | role |
|---|---|
| `apEx/Phoenix/phxEngine.h/.cpp` | D3D11 globals, constant buffers, samplers, 2D quad, LTC LUTs, `InitializePhoenix()` |
| `apEx/Phoenix/Project.h/.cpp` | `CphxProject::LoadProject` (binary blob → objects), `CphxProject::Render` |
| `apEx/Phoenix/Timeline.h/.cpp` | `CphxTimeline`, `CphxEvent_*`, the per-frame event loop |
| `apEx/Phoenix/Scene.h/.cpp` | `CphxScene`, `CphxObject`, scene-graph traversal, lights, particles, `CphxScene::Render` |
| `apEx/Phoenix/Model.h/.cpp` | `CphxModel`, `CphxModelObject_*`, `CphxMaterialPassConstantState` |
| `apEx/Phoenix/Material.h/.cpp` | `CphxMaterial`, `CphxMaterialTechnique`, `CphxMaterialRenderPass`, parameter batches |
| `apEx/Phoenix/RenderLayer.h/.cpp` | `CphxRenderLayerDescriptor`, `CphxRenderLayer`, `CphxRenderDataInstance`, RT resolution codec |
| `apEx/Phoenix/RenderTarget.h` | `CphxRenderTarget` (Texture/View/RTView/XRes/YRes) |
| `apEx/Phoenix/Texgen.h/.cpp` | procedural texture pool, filters, operators, `Prepare2dRender`, `SetSamplers` |
| `apEx/Phoenix/phxSpline.h/.cpp` | spline keys, interpolation, waveforms |
| `apEx/MinimalPlayer/*` | release player: window, presentation, music, setup dialog |

### 0.2 The single most important structural fact

**There is no persistent draw list and no persistent render graph.** Every frame, for every active
`EVENT_RENDERSCENE`:

1. `CphxScene::UpdateSceneGraph` **frees every render layer's instance array**,
2. walks the object tree, evaluates splines, and for every mesh **allocates a fresh
   `CphxRenderDataInstance`** per material pass and appends it to the layer named by the technique's
   `TargetLayer`,
3. sorts each layer's instances by `RenderPriority` (descending),
4. `CphxScene::Render` iterates the layers in fixed array order and issues the draws.

So "the render graph" is reconstructed from the scene graph + material techniques every frame.
See §3 and §5.

---

## 1. FRAME LOOP

### 1.1 Where the current time lives

Runtime globals (file-scope, `apEx/Phoenix/Timeline.cpp:232-233`):

```cpp
unsigned int TimelineFramerate;
int CurrentFrame;
```

`CphxTimeline::Render` is the only writer (`Timeline.cpp:318-323`):

```cpp
void CphxTimeline::Render( float Frame, bool tool, bool subroutine )
{
  cameraOverride = nullptr;
  TimelineFramerate = FrameRate;
  CurrentFrame = (int)Frame;
  EyeOffset = TargetOffset = D3DXVECTOR3( 0, 0, 0 );
```

`Frame` is a **float frame number**, not time. Converted from time by the caller as
`time_seconds * FrameRate` (see `CphxProject::Render` below). The tool also owns an integer
`MusicData.CurrentFrame` (`apEx/Phoenix_Tool/apxProject.h:27`) — the authoritative UI position.

### 1.2 The release player drives frames from the music player

`apEx/MinimalPlayer/MinimalPlayer.cpp:268-292` **[VERIFIED]** — this is the whole frame loop:

```cpp
          DEBUGLOG( "Starting demo" );
          if ( !phxDone && MUSIC_INIT( hWnd, music, music_size ) )
          {
            DEBUGLOG( "Initializing Music" );
            MUSIC_PLAY();

            //int tme = timeGetTime();

            //main loop
            while ( !phxDone )
            {
              MSG msg;
              while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) != 0 )
              {
                //TranslateMessage(&msg);
                DispatchMessage( &msg );
              }

              //render demo here

              DEBUGLOG( "Rendering Frame %d", MUSIC_GETSYNC() );
              Demo.Render( MUSIC_GETSYNC() );
              SwapChain->Present( setupcfg.vsync ? 1 : 0, 0 );

            }
```

* `MUSIC_GETSYNC()` expands to `mvxSystem_GetSync()` (`apEx/MinimalPlayer/MusicPlayer.h:32-36`).
  `mvx.h:85` declares `int MVXCALL mvxSystem_GetSync();` — **[VERIFIED]** it returns the **current
  song position in milliseconds** (the tool divides it by 1000 and multiplies by FrameRate; see
  §1.4). MVX internals are covered in the sibling document `docs/spec/music-mvx.md`.
* There is **no frame counter and no delta time** in the release player. Playback is a pure
  function of song position. If the music stops advancing, the demo stops advancing.
* Swap is vsynced; the demo relies on the display running at the song's frame rate (120 Hz for
  Clean Slate; `MinimalPlayer.cpp:227-246` explicitly hunts a 120 Hz display mode).

Translation from milliseconds to a demo frame:

`apEx/Phoenix/Project.cpp:1480-1483` **[VERIFIED]**:

```cpp
void CphxProject::Render( unsigned int Frame )
{
  Timeline.Render( Frame / 1000.0f*Timeline.FrameRate, false, false );
}
```

So the runtime frame number is `floor(ms * FrameRate / 1000)` evaluated in float and truncated by
`(int)Frame` inside `CphxTimeline::Render`. For 120 fps that is `ms * 0.12` frames.

### 1.3 The authoritative per-frame event loop

`apEx/Phoenix/Timeline.cpp:318-376` **[VERIFIED]** — the complete order of operations for one frame:

```cpp
void CphxTimeline::Render( float Frame, bool tool, bool subroutine )
{
  cameraOverride = nullptr;
  TimelineFramerate = FrameRate;
  CurrentFrame = (int)Frame;
  EyeOffset = TargetOffset = D3DXVECTOR3( 0, 0, 0 );

  if ( !subroutine )
  {
    Target = NULL;

    //clear rendertargets at the beginning of the frame
    //FLOAT col[4];
    //col[0] = col[1] = col[2] = col[3] = 0;
    if ( !tool )
      phxContext->ClearRenderTargetView( phxBackBufferView, (float*)rv ); //clean up after precalcbar

    for ( int x = 0; x < RenderTargetCount; x++ )
      phxContext->ClearRenderTargetView( RenderTargets[ x ]->RTView, (float*)rv );
    phxContext->ClearDepthStencilView( phxDepthBufferView, D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1, 0 );
  }

  for ( int x = 0; x < EventCount; x++ )
  {
    if ( Events[ x ]->StartFrame <= (int)Frame && (int)Frame < Events[ x ]->EndFrame )
    {
      float t = ( Frame - Events[ x ]->StartFrame ) / ( Events[ x ]->EndFrame - Events[ x ]->StartFrame ); //position in event
      float prevt = ( Frame - 1 - Events[ x ]->StartFrame ) / ( Events[ x ]->EndFrame - Events[ x ]->StartFrame ); //position in event (previous frame)

      if ( Events[ x ]->Time ) //if time spline is missing, use default 0..1
      {
        Events[ x ]->Time->CalculateValue( prevt );
        prevt = Events[ x ]->Time->Value[ 0 ];
        Events[ x ]->Time->CalculateValue( t ); //time adjusted by event's time spline
        t = Events[ x ]->Time->Value[ 0 ];
      }

      Events[ x ]->Render( t, prevt, AspectX / (float)AspectY, subroutine );// Events[x]->Time.Value[0],subroutine);

      Target = Events[ x ]->Target;
      Events[ x ]->OnScreenLastFrame = true;
    }
    else Events[ x ]->OnScreenLastFrame = false;
  }

  if ( Target && !tool )
  {
    //display frame
    Prepare2dRender();
    phxContext->PSSetShader( RenderPixelShader, NULL, 0 );

    D3D11_VIEWPORT v = { ( ScreenX - Target->XRes ) / 2.0f, ( ScreenY - Target->YRes ) / 2.0f, (float)Target->XRes, (float)Target->YRes, 0, 1 };
    phxContext->RSSetViewports( 1, &v );
    phxContext->OMSetRenderTargets( 1, &phxBackBufferView, NULL );
    phxContext->PSSetShaderResources( 0, 1, &Target->View );
    phxContext->Draw( 6, 0 );
    phxContext->PSSetShaderResources( 0, 1, rv );
  }
}
```

Notes that matter for a reimplementation:

* **Event interval is half-open `[StartFrame, EndFrame)`** at runtime. ⚠ The *tool's* own preview
  uses closed intervals (`Timeline_tool.cpp:215`: `StartFrame <= Frame && EndFrame >= Frame`).
  Frame-by-frame parity with the editor therefore differs on the last frame of every event. Treat
  the runtime as normative.
* `rv` is a `static ID3D11ShaderResourceView *rv[8] = {0,...}` (`Timeline.cpp:28`) reused as a
  "null view" array for clearing and unbinding — the clear colour is effectively **black
  (0,0,0,0)**.
* **All render targets in the project are cleared every frame**, before any event runs, plus the
  depth/stencil buffer. `tool == true` suppresses the backbuffer clear only.
* `t`/`prevt` are **event-local normalised positions**, then optionally remapped through the event's
  `TimeSpline`. `prevt` is `t` of frame `Frame-1`, which can be negative for the first frame of an
  event.
* `Target` ends up being the `Target` of the **last active event**, and that is what gets blitted to
  the backbuffer. Events overwrite `Target` in list order (list order = sorted by `Pass` then
  `StartFrame`, see §1.6).
* `subroutine` (used for nested/sub-timeline rendering, always `false` in the release player)
  suppresses the frame clears and the final blit.

### 1.4 How the tool drives playback (music-tick → frame)

`apEx/Phoenix_Tool/apxProject.cpp:1291-1303` **[VERIFIED]**:

```cpp
TS32 CapexProject::GetFrameToRender()
{
  if ( !MusicData.TimelineDragged )
  {
    if ( MusicData.Playing )
      if ( Song )
        return max( 0, (TS32)( Song->GetPlayPosition() / 1000.0f*Project.Timeline->Timeline->FrameRate ) );
      else
        return max( 0, (TS32)( ( globalTimer.GetTime() - MusicData.PlaybackStart ) / 1000.0f*Project.Timeline->Timeline->FrameRate ) );
  }

  return MusicData.CurrentFrame;
}
```

`apEx/Phoenix_Tool/apxProject.cpp:1377-1386`:

```cpp
void CapexProject::UpdateCurrentFrameFromPlayback()
{
  if ( !MusicData.Playing ) return;

  if ( Song )
    MusicData.CurrentFrame = max( 0, (TS32)( Song->GetPlayPosition() / 1000.0f*Timeline->Timeline->FrameRate ) );
  else
    MusicData.CurrentFrame = max( 0, (TS32)( ( globalTimer.GetTime() - MusicData.PlaybackStart ) / 1000.0f*Timeline->Timeline->FrameRate ) );

}
```

`CphxSound::GetPlayPosition()` is confirmed to be **milliseconds**
(`apEx/Phoenix_Tool/Audio/phxSound.cpp:135-140` → `CphxSongBase::GetOfflinePlayPosition()`,
`phxSongBase.cpp:178-187`):

```cpp
TU32 CphxSongBase::GetOfflinePlayPosition()
{
  if ( !pSecondaryBuffer )
    return false;

  DWORD dwPlayCursor = 0;
  pSecondaryBuffer->GetCurrentPosition( &dwPlayCursor, NULL );

  return MulDiv( dwPlayCursor, 1000, PHXSOUND_SAMPLERATE * PHXSOUND_FRAMESIZE );
}
```

So the tool's frame number is `floor(song_ms * FrameRate / 1000)` — exactly the same mapping the
release player performs inside `CphxProject::Render`. **This is the whole sync model: the music
player's millisecond play position *is* the timeline clock.** There is no separate frame timer
anywhere in the demo runtime.

Playback start / seek (`apxProject.cpp:1344-1416`):

```cpp
  if ( Song )
  {
    LOG_DBG( "[mvx] Starting playback at %d", (TS32)( MusicData.CurrentFrame / (TF32)Timeline->Timeline->FrameRate*1000.0f ) );
    Song->Play( (TS32)( MusicData.CurrentFrame / (TF32)Timeline->Timeline->FrameRate*1000.0f ) );
  }
  else
    MusicData.PlaybackStart = globalTimer.GetTime() - (TS32)( MusicData.CurrentFrame / (TF32)Timeline->Timeline->FrameRate*1000.0f );
```

`TogglePlaying` also re-seeds all particle systems (swap + `ResetParticles()` twice per scene,
`apxProject.cpp:1368-1374`) — particle state is **not** a pure function of time; it is an integrator
(see §5.9).

### 1.5 The tool's per-frame render entry points

| entry | file:line | what it does |
|---|---|---|
| `CapexTimelinePreview::UpdateData` / `CWBDemoPreview::OnDraw` | `apEx/apEx/TimelinePreview.cpp:26-102` | full demo preview: `ApplyRenderTargets` → set states → `Project.Timeline->Render((TF32)frame)` → draw `Timeline->Target->View` with aspect letterboxing |
| `CapexVideoDumperPreview::PrepareFrame` | `apEx/apEx/VideoDumper.cpp:127-136` | deterministic single-frame render for the PNG/TGA dumper |
| `CWBSceneDisplay::OnDraw` | `apEx/apEx/SceneView.cpp:841-960` | editor viewport: `UpdateRendertargetCollection` → `ApplyRenderTargets` → `UpdateSceneContent` → `UpdateSceneGraph(clip,timepos)` → `CalculateMatrices` → `DrawGrid`/helpers → `KillInvalidBatches`/`SortRenderLayers` → `s->Scene.Render(false,false,0)` or `s->Render()` → blit to the UI |
| `CphxScene_Tool::Render` | `apEx/Phoenix_Tool/Scene_tool.cpp:110-113` | `Scene.Render(true, true, 0)` (clear colour + Z) |

`CWBDemoPreview::OnDraw` core (`TimelinePreview.cpp:99-102`) **[VERIFIED]**:

```cpp
  TS32 frame = Project.GetFrameToRender();
  Project.Timeline->Render( (TF32)frame );
```

Before that it applies the tool's own D3D states and the render-target collection:

```cpp
  Project.ApplyRenderTargets( RenderTargets );
  ...
  RenderDepthState_Normal->Apply();
  RenderRasterState->Apply();
  RenderSamplerState->Apply( CORESMP_PS0 );
  RenderSamplerState->Apply( CORESMP_VS0 );
```

### 1.6 Event ordering

`CphxTimeline_Tool::Sort` uses `EventSorter` (`Timeline_tool.cpp:276-288`):

```cpp
TS32 EventSorter( CphxEvent_Tool **a, CphxEvent_Tool **b )
{
  if ( !( a && b && *a && *b ) ) return 0;

  if ( ( *a )->Pass == ( *b )->Pass ) return ( *a )->Event->StartFrame - ( *b )->Event->StartFrame;

  return ( *a )->Pass - ( *b )->Pass;
}
```

Events are executed **in array order**; the array is sorted by `Pass` (ascending), then
`StartFrame` (ascending). `Pass` is the tool's compositing-order control ("render target editor"
pass filter). The exporter writes events in this sorted order (`MinimalExport.cpp:2964-2966`), and
the loader reads them back in the same order, so a reimplementation must preserve it.

The tool can additionally truncate the tail of the list for interactive pass filtering
(`Timeline_tool.cpp:250-273`): events with `Pass > Project.LastPassToDraw` are dropped by
temporarily shrinking `Timeline->EventCount`.

### 1.7 MUSICDATA

`apEx/Phoenix_Tool/apxProject.h:17-32` **[VERIFIED]**:

```cpp
struct MUSICDATA
{
  CArray<TU32> TickData;

  HWND AppHandle; // = NULL;
  TF32 MVXRenderStatus; // = 0;

  TBOOL Playing; // = false;
  TS32 PlaybackStart; // = 0;

  TS32 CurrentFrame; // = 0;
  TBOOL TimelineDragged; // = false;

  TS32 OfflineLength; // = 0;

};
```

* `TickData` is **not** a frame clock. It is filled by `AcquireTickData()`
  (`apxProject.cpp:1418-1428`) with `Song->GetMarkers(...)` — the MVX per-tick millisecond marker
  table — and `OfflineLength = Song->GetSongLength()`. It is used only by the timeline *editor*
  (grid/beat highlighting, `TimelineEditor.cpp`) — **[VERIFIED by usage]**.
* The editor's timeline grid uses `Timeline->GridBPM` / `BPMPrimaryModulus` / `BPMSecondaryModulus`
  (`cleanslate.apx`: 120 / 32 / 8) purely for display, not for timing.
* `MVXRenderStatus` is the offline-render progress 0..1 (`apxProject.cpp:1305-1317`).

**Bottom line for a reimplementation:** you do not need MVX to reproduce demo timing if you know
the song length and can advance a millisecond clock; you need MVX (or a pre-rendered PCM + marker
table) only for audio. For exact frame parity with the original release you should drive frames
from the *same* millisecond source (see `docs/spec/music-mvx.md` for the `.mvm` tick table).

### 1.8 Deterministic frame dumping (the existing "headless" path)

`CapexVideoDumper` is the closest thing in the tree to an offline renderer. It swaps the engine's
`TimerFunction` for a fixed-step stub (`apEx/apEx/apExRoot.cpp:1453-1472`):

```cpp
DWORD __stdcall FixedStepTimer();
extern TS32 FixesStepTimePosition;

void CapexRoot::ToggleVideoDumper()
{
  Project.ResetParticles();
  if ( VideoDumper )
  {
    VideoDumper->MarkForDeletion();
    VideoDumper = NULL;
    TimerFunction = timeGetTime;
    ...
    return;
  }
  VideoDumper = new CapexVideoDumper( this, GetClientRect() );
  VideoDumper->SetFocus();
  TimerFunction = FixedStepTimer;
  FixesStepTimePosition = 0;
}
```

and `apEx/apEx/VideoDumper.cpp:25-29`:

```cpp
TS32 FixesStepTimePosition = 0;
DWORD __stdcall FixedStepTimer()
{
  return FixesStepTimePosition;
}
```

Per dumped image (`VideoDumper.cpp:286-339`) **[VERIFIED]**:

```cpp
  TF32 frame = ( CurrentFrame*Project.Timeline->Timeline->FrameRate ) / Framerate;
  TS32 FrameToRender = (TS32)( frame + 0.5f ); //round to nearest
  ...
    d->PrepareFrame();
    ...
    FixesStepTimePosition = (TS32)( frame / (TF32)Project.Timeline->Timeline->FrameRate * 1000 + 0.5f );
    Project.MusicData.CurrentFrame = FrameToRender;
```

with `PrepareFrame` = `ApplyRenderTargets` → states → `Timeline->Render((TF32)frame)`. Then
`RT->ExportToImage(Filename, true, CORE_PNG|CORE_TGA, true)` on
`d->RenderTargets->GetRenderTarget(Project.Timeline->Timeline->Target)`.

Properties a headless port must reproduce:

* **No wall-clock dependency** in the render path once `TimerFunction` is stubbed. Anything that
  reads real time (particle emitters if they used the timer) must be fed the fixed-step value.
* Output = `Timeline->Target` after the final event, at `Target->XRes × Target->YRes`.
* Frame selection = `round(n * FrameRate_out / FrameRate_demo)`; Start/End frame are user-set and
  default to `[0, firstEndDemoStartFrame)`.
* `Project.ResetParticles()` + `Project.SeekToFrame(0)` at the start of a dump, and all events'
  `RequestContent()` re-armed (`VideoDumper.cpp:370-377`).

### 1.9 Frame-loop summary for the Vulkan port

```
frame f = floor(song_ms * FrameRate / 1000)          # FrameRate from project (120)
timeline.render(f):
    cameraOverride = null; TimelineFramerate = FrameRate; CurrentFrame = f
    EyeOffset = TargetOffset = 0
    clear all project render targets to (0,0,0,0); clear depth+stencil
    for each event in sorted order:
        if StartFrame <= f < EndFrame:
            t     = (f   - StartFrame) / (EndFrame - StartFrame)
            prevt = (f-1 - StartFrame) / (EndFrame - StartFrame)
            if TimeSpline: t = TimeSpline(t); prevt = TimeSpline(prevt)
            event.render(t, prevt, AspectX/AspectY, subroutine=false)
            Target = event.Target
    blit Target into backbuffer viewport
        offset = ((ScreenX - Target.XRes)/2, (ScreenY - Target.YRes)/2), size = Target.XRes×YRes
```


---

## 2. PROJECT RENDER

### 2.1 Resolution, aspect and render-target allocation at load

`apEx/Phoenix/Project.cpp:276-335` **[VERIFIED]** — this is the only place the demo's internal
resolution is decided:

```cpp
  //////////////////////////////////////////////////////////////////////////
  // aspect ratio and framerate info, needed for rendertargets

  *( (unsigned int*)&Timeline.AspectX ) = readdword( Stream_Main ); // read x and y aspects, framerate and rendertarget count

  //////////////////////////////////////////////////////////////////////////
  // load rendertargets

  //calculate demo base resolution

  int XRes = Timeline.ScreenX = setup.mode.dmPelsWidth;
  int YRes = Timeline.ScreenY = setup.mode.dmPelsHeight;

  float demoResolutionAspect = XRes / (float)YRes;
  float monitorResolutionAspect = setup.fullscreen ? demoResolutionAspect : ( GetSystemMetrics( SM_CXSCREEN ) / (float)GetSystemMetrics( SM_CYSCREEN ) );
  float aspect = Timeline.AspectX / (float)Timeline.AspectY / setup.HardwareAspectRatio * monitorResolutionAspect;

  if ( aspect > demoResolutionAspect )
    YRes = (int)( XRes / aspect + 0.5f ); //+0.5f added for rounding precision
  else
    XRes = (int)( YRes * aspect + 0.5f ); //+0.5f added for rounding precision

  CreateDepthBuffer( XRes, YRes );

  //create internal rendertarget
  extern CphxRenderTarget *phxInternalRenderTarget;
  phxInternalRenderTarget = new CphxRenderTarget;
  phxInternalRenderTarget->XRes = XRes;
  phxInternalRenderTarget->YRes = YRes;
  CreateRenderTarget( DXGI_FORMAT_R16G16B16A16_FLOAT, *phxInternalRenderTarget );

  Timeline.RenderTargets = new CphxRenderTarget*[ Timeline.RenderTargetCount ];
  for ( int x = 0; x < Timeline.RenderTargetCount; x++ )
  {
    Timeline.RenderTargets[ x ] = new CphxRenderTarget;
    GetRenderTargetResolution( readbyte( Stream_Main ), Timeline.RenderTargets[ x ]->XRes, Timeline.RenderTargets[ x ]->YRes, XRes, YRes );

    unsigned char pFormat = readbyte( Stream_Main );
    Timeline.RenderTargets[ x ]->cubeMap = ( pFormat & 0xf0 ) != 0;

    CreateRenderTarget( (DXGI_FORMAT)PixelFormats[ pFormat & 0x0f ], *Timeline.RenderTargets[ x ] );
  }
```

`Timeline.AspectX/AspectY/FrameRate/RenderTargetCount` are four consecutive `unsigned char` fields
(`Timeline.h:169-172`) read as one little-endian dword:

```
dword = AspectX | AspectY<<8 | FrameRate<<16 | RenderTargetCount<<24
```

Real check: `precalc.64k` byte 83 = `10 09 78 01` → 16:9, 120 fps, 1 render target — matches
`cleanslate.apx`'s `<AspectX>16</AspectX> <AspectY>9</AspectY> <Framerate>120</Framerate>`.
**[VERIFIED]**

`XRes`/`YRes` (also stored as `Timeline.ScreenX/ScreenY`) is the **demo render resolution**, derived
from the *requested* display mode corrected by the project aspect ratio and the monitor aspect:

```
aspect = (AspectX / AspectY) / HardwareAspectRatio * monitorAspect
if aspect > backbufferAspect: YRes = XRes / aspect
else                          XRes = YRes * aspect
```

`Timeline.ScreenX/ScreenY` is later used as the **backbuffer size for the final letterbox blit**
(§2.5) — note it is the demo resolution, not the actual swap-chain size.

Depth buffer and render targets (`Project.cpp:196-236`) **[VERIFIED]**:

```cpp
static D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = { DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, 0 };
static D3D11_SHADER_RESOURCE_VIEW_DESC resdesc = { DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1 };
static D3D11_TEXTURE2D_DESC depthBufferDesc = { 0, 0, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
...
static void CreateRenderTarget( const DXGI_FORMAT Format, CphxRenderTarget& target )
{
  D3D11_TEXTURE2D_DESC tex = depthBufferDesc;
  tex.Width = target.XRes;
  tex.Height = target.YRes;
  tex.Format = Format;
  tex.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  tex.MipLevels = 0;
  tex.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  D3D11_RENDER_TARGET_VIEW_DESC rt = { Format, D3D11_RTV_DIMENSION_TEXTURE2DMS, 0 };

  phxDev->CreateTexture2D( &tex, NULL, &target.Texture );
  phxDev->CreateShaderResourceView( target.Texture, NULL, &target.View );
  phxDev->CreateRenderTargetView( target.Texture, &rt, &target.RTView );
...
}
```

* Depth: 24-bit depth + 8-bit stencil, typeless resource, sampled as `R24_UNORM_X8_TYPELESS`
  (used as texture 7 by `PARAM_DEPTHTEXTURE7`).
* All render targets: **full mip chain** (`MipLevels = 0`, `GENERATE_MIPS`), so `GenerateMips` after
  each layer is valid and is used for blur chains.
* Formats available: `PixelFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT }`
  (`Project.cpp:160-164`); index is `pFormat & 0x0f`, high nibble = cube map flag. The tool's
  `pixelFormats[]` in `apxProject.cpp:16-20` is the same order.
* `phxInternalRenderTarget` is the full-res scratch buffer used to copy a target that is both
  sampled and written in the same pass (see the shadertoy path, §2.7, and
  `Timeline.cpp:52-71`).

**Render-target resolution encoding** — `apEx/Phoenix/RenderLayer.cpp:3-23` **[VERIFIED]**:

```cpp
void __fastcall GetRenderTargetResolution( unsigned char ResData, int &XRes, int &YRes, int DemoResX, int DemoResY )
{
#ifdef PHX_HAS_NON_DEMO_RELATIVE_RENDERTARGETS
  bool DemoRelative = ( ResData & 0x80 ) != 0;
#endif

  int xfact = ( ResData & 0x38 ) >> 3;
  int yfact = ResData & 0x07;

#ifdef PHX_HAS_NON_DEMO_RELATIVE_RENDERTARGETS
  if ( !DemoRelative )
  {
    XRes = 1 << ( xfact + 5 );
    YRes = 1 << ( yfact + 5 );
    return;
  }
#endif

  XRes = DemoResX >> xfact;
  YRes = DemoResY >> yfact;
}
```

So `ResolutionDescriptor` byte = `bit7 DemoRelative | bits5-3 xfactor | bits2-0 yfactor`.
The tool builds it via `CreateResolutionDescription(DemoResRelative, XFactor, YFactor)`
(`RenderTargets.cpp:121-124`). Examples from `cleanslate.apx` **[VERIFIED]**:

| name | descriptor | meaning |
|---|---|---|
| Backbuffer | 128 (`0x80`) | demo res, /1 × /1 |
| Back X/2 Y/1 | 136 (`0x88`) | demo res /2 × /1 |
| Back X/2 Y/2 | 137 (`0x89`) | /2 × /2 |
| Back X/4 Y/2 | 145 (`0x91`) | /4 × /2 |
| Back X/8 Y/8 | 155 (`0x9b`) | /8 × /8 |

`PHX_HAS_NON_DEMO_RELATIVE_RENDERTARGETS` enables absolute powers of two (32, 64, 128, …) when
bit 7 is clear. Whether the Clean Slate build defines it is a compile-time choice — **[UNCERTAIN]**;
the XML in this repo only uses demo-relative descriptors.

### 2.2 Order of operations for one rendered frame

`CphxProject::Render` is a one-liner (§1.2) — **all** per-frame work is in
`CphxTimeline::Render` (§1.3) plus each event's `Render`. The full pipeline per frame is:

1. **Timeline prologue** — set clock globals, reset `cameraOverride`, `EyeOffset`, `TargetOffset`.
2. **Clear** — every project render target to `(0,0,0,0)`; depth+stencil to 1/0; backbuffer cleared
   too (release player only).
3. **Event sweep** in sorted order. For each active event:
   * compute `t`, `prevt`, remap through `TimeSpline`;
   * `EVENT_RENDERSCENE`: `UpdateSceneGraph` twice (prevt then t) → compute view/projection →
     `CphxScene::Render(clearColor, clearZ, cubeResolution)`;
   * `EVENT_SHADERTOY`: fullscreen-quad post-process pass(es) (may target any render target);
   * `EVENT_PARTICLECALC`: advance particle integrators;
   * `EVENT_CAMERASHAKE`: set `EyeOffset`/`TargetOffset` for the following scene events;
   * `EVENT_CAMERAOVERRIDE`: `UpdateSceneGraph` + set global `cameraOverride`;
   * `EVENT_ENDDEMO`: set `phxDone = true`;
   * `Target = event->Target` (runtime: **always render target 0**, see §2.6).
4. **Present** — `Prepare2dRender()` + blit `Target->View` into the backbuffer with a centred
   viewport (§2.5).

### 2.3 CphxScene::Render — render layers, clearing, texgen and the draw loop

`apEx/Phoenix/Scene.cpp:136-203` **[VERIFIED]**:

```cpp
void CphxScene::Render( bool ClearColor, bool ClearZ, int cubeResolution )
{
  SetSamplers();

  //collect lights

  //calculate needed matrices
  D3DXMatrixInverse( &phxIViewMatrix, NULL, &phxViewMatrix );
  D3DXMatrixInverse( &phxIProjectionMatrix, NULL, &phxProjectionMatrix );

  //upload scene level data

  for ( int x = 0; x < LayerCount; x++ )
  {
    RenderLayers[ x ]->Descriptor->SetEnvironment( ClearColor, ClearZ, cubeResolution );

    D3D11_MAPPED_SUBRESOURCE map;
    phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
    unsigned char* m = (unsigned char*)map.pData;

    float LightCountData[ 4 ];
    LightCountData[ 0 ] = (float)LightCount;

    memcpy( m, &phxViewMatrix, sizeof( phxViewMatrix ) ); m += sizeof( phxViewMatrix );
    memcpy( m, &phxProjectionMatrix, sizeof( phxProjectionMatrix ) ); m += sizeof( phxProjectionMatrix );
    memcpy( m, &phxCameraPos, sizeof( phxCameraPos ) ); m += sizeof( phxCameraPos );
    memcpy( m, &LightCountData, sizeof( LightCountData ) ); m += sizeof( LightCountData );
    memcpy( m, Lights, sizeof( LIGHTDATA )*MAX_LIGHT_COUNT ); m += sizeof( LIGHTDATA )*MAX_LIGHT_COUNT;

    float RTResolution[ 4 ];
    //memset(RTResolution, 16, 0);

    if ( RenderLayers[ x ]->Descriptor->TargetCount )
    {
      RTResolution[ 0 ] = (float)RenderLayers[ x ]->Descriptor->Targets[ 0 ]->XRes;
      RTResolution[ 1 ] = (float)RenderLayers[ x ]->Descriptor->Targets[ 0 ]->YRes;
      RTResolution[ 2 ] = 1 / RTResolution[ 0 ];
      RTResolution[ 3 ] = 1 / RTResolution[ 1 ];
    }

    memcpy( m, RTResolution, 16 ); m += 16;

    memcpy( m, &phxIViewMatrix, sizeof( phxIViewMatrix ) ); m += sizeof( phxIViewMatrix );
    memcpy( m, &phxIProjectionMatrix, sizeof( phxIProjectionMatrix ) ); m += sizeof( phxIProjectionMatrix );


    phxContext->Unmap( SceneDataBuffer, 0 );

    for ( int y = 0; y < 2; y++ )
    {
      ID3D11Buffer *Buffer = y ? ObjectMatrixBuffer : SceneDataBuffer;
      phxContext->VSSetConstantBuffers( y, 1, &Buffer );
      phxContext->GSSetConstantBuffers( y, 1, &Buffer );
      phxContext->PSSetConstantBuffers( y, 1, &Buffer );
    }


    for ( int y = 0; y < RenderLayers[ x ]->RenderInstances.NumItems(); y++ )
      RenderLayers[ x ]->RenderInstances[ y ]->Render();

#ifdef PHX_VOLUMETRIC_RENDERTARGETS
    if ( RenderLayers[ x ]->Descriptor->VoxelizerLayer )
          phxContext->GenerateMips( phxTexture3DResourceView );

#endif
    RenderLayers[ x ]->Descriptor->GenMipmaps();
  }
}
```

Key points:

* **Layers are drawn in array order** — the order they were exported from the tool
  (`cleanslate.apx` order: *Shadow Layer*, *Solid Layer*, *Lighting Layer*, *Transparent Layer*).
  There is no dependency sort; the artist controls order by moving layers in the tool.
* **Clearing is driven by the event's `ClearColor`/`ClearZ` ANDed with the layer's
  `clearRenderTargets` flag** (see `SetEnvironment` in §3.3). An event with `clearcolor=0` clears
  nothing even on a `ClearRenderTargets=1` layer.
* **The same depth buffer is shared by all layers.** `OmitDepthBuffer` detaches it for a layer.
* Scene constants (`b0`) are re-uploaded **per layer**, because `RTResolution` (`.res`) is per-layer
  (taken from the layer's *first* target).
* **Mipmaps are generated after every layer** for all of its targets (`GenMipmaps`), regardless of
  whether the layer actually wrote them.
* The *only* thing resembling "post-processing" inside the scene is that later layers can sample
  earlier layers' targets. Real post-processing is done by `EVENT_SHADERTOY` (§2.7).
* `SetSamplers()` (`Texgen.cpp:92-97`) binds the three global samplers s0/s1/s2
  (wrap-aniso16 / clamp-aniso16 / comparison-linear) for the pixel stage; they were also bound for
  VS at init (`phxEngine.cpp:212-217`).

**Texgen** is *not* per-frame in the release path. All texture operators are generated once during
`LoadProject` (`Project.cpp:480-491`):

```cpp
  //render textures \o/
  for ( int x = 0; x < renderedcount; x++ )
    //if (TextureOperators[x].NeedsRender)
  {
    DEBUGLOG( "Generating Operator: %d", x );
    ( TextureOperators[ x ].CachedResult = TextureOperators[ x ].Generate( TextureFilters, TextureOperators ) )->Deleted = true;
    //TexgenPool->Pool.Delete(TextureOperators[x].CachedResult);
    if ( DoPrecalc( 0.15f + 0.35f*x / (float)renderedcount ) ) return;
    DEBUGLOG( "Rendered Texture %d/%d - resolution %d %d", x, renderedcount, GETXRES( TextureOperators[ x ].Resolution ), GETYRES( TextureOperators[ x ].Resolution ) );
  }
```

`NeedsRender = (index < renderedcount)` (`Project.cpp:425`); the pool textures are flagged
`Deleted = true` so the pool will not hand them out again, but the material parameters keep pointing
at their `View`s (`ImportMaterialParamValue`, `Project.cpp:1512-1532`).

Texture operator generation is recursive over parents
(`Texgen.cpp:464-497`), each filter runs `DataDescriptor.PassCount` ping-pong passes at the
operator's resolution (`GETXRES(res)=1<<(res>>4)`, `GETYRES(res)=1<<(res&0xf)`,
`Texgen.h:97-98`), with `srand(RandSeed)` at the top of `PHXTEXTUREFILTER::Render`
(`Texgen.cpp:120-185`) — **deterministic given the same RandSeed**, which matters for byte-exact
reproduction.

### 2.4 Constant buffer layout actually consumed by shaders

Extracted from the 83 shader blocks in `cleanslate.apx` **[VERIFIED]**, the dominant `b0` layouts are:

| occurrences | layout |
|---|---|
| 7 | `float4x4 viewmat; float4x4 projmat; float4 campos; float4 lightcount; lightdata lights[8]; float4 res; float4x4 iviewmat; float4x4 iprojmat;` |
| 5 | `float4x4 viewmat; float4x4 projmat; float4 campos; float4 lightcount; lightdata lights[8];` |
| 5 | `float4x4 viewmat; float4x4 projmat; float4 campos;` |
| 6 | `float t, aspect, xres, yres;` ← **shadertoy events** |
| 4 | `float t, aspect;` ← shadertoy events |

with (12 occurrences):

```hlsl
struct lightdata { float4 pos; float4 ambient; float4 diffuse; float4 specular; float4 spotdirection; float4 sda; };
```

which matches `LIGHTDATA` in `Scene.h:131-137` byte-for-byte (6 × float4 = 96 B).

`b1` layouts:

| occurrences | layout |
|---|---|
| 4 | `float4x4 worldmat; float4x4 itworldmat; float color;` |
| 2 | `float4x4 worldmat; float4x4 itworldmat; float4 data;` |
| 5 | `float2 blur; float samplecount;` (post-process) |

So `b1` = `Matrices[0]`, `Matrices[1]`, then the pass's **constant-data floats in parameter
declaration order**. `SCENEDATASIZE` reserves `16*4*2 + 4*4*2 + 8*(6*4*4) + 16 + 16*4 + 16*4 +
16*4*2` = 1200 bytes (`phxEngine.h:49`); the scene path actually writes 1072
(§ above), the shadertoy path writes a *different, shorter* layout starting with `{t, aspect, xres,
yres}` (`Timeline.cpp:98-116`) — the two paths reuse `b0` with incompatible meanings, which is safe
only because each pass binds its own shader.

### 2.5 Backbuffer, aspect handling and letterboxing

The engine's own present step is the tail of `CphxTimeline::Render` (§1.3):

```cpp
    D3D11_VIEWPORT v = { ( ScreenX - Target->XRes ) / 2.0f, ( ScreenY - Target->YRes ) / 2.0f, (float)Target->XRes, (float)Target->YRes, 0, 1 };
    phxContext->RSSetViewports( 1, &v );
    phxContext->OMSetRenderTargets( 1, &phxBackBufferView, NULL );
    phxContext->PSSetShaderResources( 0, 1, &Target->View );
    phxContext->Draw( 6, 0 );
```

* The blit is a **fullscreen-quad texture copy** (`RenderPixelShader` = entry `a` of the inline
  shader in `phxEngine.cpp:146-156`: `Texture2D r : register(t0); SamplerState o : register(s1);
  return r.Sample(o, e.o);`) with **no scaling**: viewport size = `Target->XRes × Target->YRes`.
* Offset `((ScreenX - Target->XRes)/2, (ScreenY - Target->YRes)/2)`. Since `ScreenX/Y` is the
  *demo* resolution and `Target` is normally the demo-resolution RT 0, this is a 1:1 copy.
  Letterboxing (black bars) happens **outside the engine**, by the swap chain being the physical
  mode while the demo resolution (and hence the internal RTs) was fitted to the project aspect.
* The 2D quad setup is `Prepare2dRender()` (`Texgen.cpp:99-118`): binds `TexgenVertexShader`,
  `TexgenVertexBuffer` (6 verts, 6 floats each: xyzw + uv), input layout `TexgenVertexFormat`,
  triangle list, `RSSetState(NULL)`, blend/depth NULL, GS/HS/DS NULL.
* Sampler `s1` in the inline blit shader is `TexgenSampler_NoWrap` (clamp, anisotropic 16) — it is
  bound by `SetSamplers()` called from `Prepare2dRender()`.
* The tool does its own aspect-preserving blit into a `CRect` computed from
  `aspect = (tax*max*say)/(tay*may*sax)` (`TimelinePreview.cpp:44-58`, `VideoDumper.cpp:37-58`) —
  the same formula used by `CapexRenderTargetCollection::UpdateCollection` (§2.8). A reimplementation
  should keep the engine's arithmetic exact if it wants frame-identical letterboxing.

### 2.6 Runtime caveat: every event targets render target 0

In the release loader the target written by each event is **not** read from the data:

`apEx/Phoenix/Project.cpp:1471-1474` **[VERIFIED]**:

```cpp
    //int targetid = readbyte(Stream_EventTargets);
    //if (targetid >= 0)
    e->Target = Timeline.RenderTargets[ 0 ];
    e->Time = time;
```

and in `MinimalExport.cpp:1683-1686` the `TargetRT` byte is commented out as well:

```cpp
  //CphxRenderTarget_Tool *t = Project.GetRenderTarget(e->TargetID);
  //int id = -1;
  //if (t) id = t->GetResourceIndex();
  //WriteByte(Stream_EventTargets, id, EventDataSize);
```

Consequences:

* In the release player, **`Target` is always `RenderTargets[0]`**, and `RenderTargets[0]` is the
  first *required* render target — in `cleanslate.apx` that is *"Backbuffer"* (`B804E9F6…`).
* A faithful reimplementation of the *release* therefore composites into RT 0 and blits RT 0.
  A reimplementation of the *editor* should honour each event's `<TargetRT>` GUID (the tool does:
  `ApplyRenderTargets`, `apxProject.cpp:1173-1178`).
* Depth-buffer-derived effects (`PARAM_DEPTHTEXTURE7`) work in both.

### 2.7 Post-processing / "shadertoy" events

`TECH_POSTPROCESS` (`Material.h:194`) is **not referenced anywhere in the current code** — the
post-processing mechanism has been replaced by `EVENT_SHADERTOY` running a `TECH_SHADERTOY`
technique on a fullscreen quad.

`apEx/Phoenix/Timeline.cpp:33-136` **[VERIFIED]** (abridged to the structurally important lines):

```cpp
void CphxEvent_Shadertoy::Render( float t, float prevt, float aspect, bool subroutine )
{
  if ( !Tech ) return;

  MaterialSplines->CalculateValues( t );
  MaterialSplines->ApplyToParameters( NULL );

  for ( int y = 0; y < Tech->PassCount; y++ )
    Tech->CollectAnimatedData( MaterialState[ y ], y );

  float ShaderData[ 4 ];
  ShaderData[ 0 ] = t;
  ShaderData[ 1 ] = aspect;

  Prepare2dRender();

  phxContext->PSSetConstantBuffers( 0, 1, &SceneDataBuffer );
  phxContext->PSSetConstantBuffers( 1, 1, &ObjectMatrixBuffer );

  extern CphxRenderTarget *phxInternalRenderTarget;

  for ( int x = 0; x < Tech->PassCount; x++ )
  {
    if ( MaterialState[ x ]->RenderTarget )
    {
      //copy rendertarget if needed
      bool NeedsRTCopy = false;
      for ( int y = 0; y < 7; y++ )
      {
        MaterialState[ x ]->TextureBackup[ y ] = MaterialState[ x ]->Textures[ y ];
        if ( MaterialState[ x ]->Textures[ y ] == MaterialState[ x ]->RenderTarget->View )
        {
          MaterialState[ x ]->Textures[ y ] = phxInternalRenderTarget->View;
          NeedsRTCopy = true;
        }
      }

      if ( NeedsRTCopy )
        phxContext->CopyResource( phxInternalRenderTarget->Texture, MaterialState[ x ]->RenderTarget->Texture );

      //done copying, go on...
      MaterialState[ x ]->RenderTarget->SetViewport();
      phxContext->OMSetRenderTargets( 1, &MaterialState[ x ]->RenderTarget->RTView, NULL );
    }

    Target = MaterialState[ x ]->RenderTarget;

    phxContext->PSSetShader( Tech->RenderPasses[ x ]->PS, NULL, 0 );

    phxContext->RSSetState( MaterialState[ x ]->RasterizerState );
    phxContext->OMSetBlendState( MaterialState[ x ]->BlendState, NULL, 0xffffffff );
    phxContext->OMSetDepthStencilState( MaterialState[ x ]->DepthStencilState, 0 );
    MaterialState[ x ]->Textures[ 7 ] = phxDepthBufferShaderView;						//DEPTH BUFFER SET TO TEXTURE7 HERE
    phxContext->PSSetShaderResources( 0, 8, MaterialState[ x ]->Textures );

    int constdatasize = MaterialState[ x ]->ConstantDataSize;
    int dyndatasize = MaterialState[ x ]->AnimatedDataSize;

    D3D11_MAPPED_SUBRESOURCE map;
    phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
    memcpy( ( (unsigned char*)map.pData ), MaterialState[ x ]->ConstantData, constdatasize );
    memcpy( ( (unsigned char*)map.pData ) + constdatasize, MaterialState[ x ]->AnimatedData, dyndatasize );
    phxContext->Unmap( ObjectMatrixBuffer, 0 );

    //set layer shader data
    phxContext->Map( SceneDataBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
    unsigned char* m = (unsigned char*)map.pData;

    if ( Target )
    {
      ShaderData[ 2 ] = (float)Target->XRes;
      ShaderData[ 3 ] = (float)Target->YRes;
    }

    memcpy( m, ShaderData, sizeof( ShaderData ) ); m += sizeof( ShaderData );
    memcpy( m, &phxViewMatrix, sizeof( phxViewMatrix ) ); m += sizeof( phxViewMatrix );
    ... (projection, camera pos, inverse view/proj, previous-frame view/proj)
    phxContext->Unmap( SceneDataBuffer, 0 );

    //render here
    phxContext->Draw( 6, 0 );

    //clean up
    phxContext->PSSetShaderResources( 0, 8, rv );

    if ( Target )
      phxContext->GenerateMips(Target->View);

    for ( int y = 0; y < 7; y++ )
      MaterialState[ x ]->Textures[ y ] = MaterialState[ x ]->TextureBackup[ y ];

  }
}
```

Notable semantics for a port:

* Every pass is a **6-vertex fullscreen draw**, no vertex buffer bound by the shader pipeline
  itself (`Prepare2dRender` binds the shared 2D quad).
* `ShaderData = {t, aspect, xres, yres}` is `b0` for shadertoy passes; the rest of `b0` is filled
  with the current view/proj/inverse/previous matrices (mostly unused by post shaders).
* A pass may declare `PARAM_RENDERTARGET` (type 23) as a **variable** parameter; that is where it
  writes. If it also samples that same target, the engine first copies it to
  `phxInternalRenderTarget` and rebinds texture slots 0..6 to the copy — i.e. **automatic
  read-write hazard avoidance for the first 7 texture slots**.
* Texture slot 7 is always overwritten with the **depth buffer SRV**.
* Mipmaps are generated for the written target after each pass (blur chains depend on this).
* If a pass has no `RenderTarget`, the pass writes wherever the previous render target binding
  happens to be — **[UNCERTAIN]**, probably to the currently bound layer targets.

### 2.8 Tool-side render target collection

`apEx/Phoenix_Tool/apxProject.cpp:1727-1784` **[VERIFIED]**:

```cpp
void CapexRenderTargetCollection::UpdateCollection( TS32 xres, TS32 yres, TBOOL AspectCorrect )
{
  LastAspectCorrect = AspectCorrect;
  TBOOL First = !DepthBuffer;

  TS32 odx = XRes;
  TS32 ody = YRes;

  TS32 tax = Project.Timeline->Timeline->AspectX;
  TS32 tay = Project.Timeline->Timeline->AspectY;

  TS32 max = Config::MonitorAspectX;
  TS32 may = Config::MonitorAspectY;

  TS32 sax = GetSystemMetrics( SM_CXSCREEN );
  TS32 say = GetSystemMetrics( SM_CYSCREEN );

  TF32 aspect = ( tax*max*say ) / (TF32)( tay*may*sax );

  XRes = xres;
  YRes = yres;

  if ( AspectCorrect )
  {
    if ( aspect > xres / (TF32)yres )	YRes = (TS32)( XRes / aspect );
    else XRes = (TS32)( YRes*aspect );
  }

  if ( !First && ( XRes == odx && YRes == ody ) ) return; //no change

  SAFEDELETE( Internal );
  SAFEDELETE( DepthBuffer );
  RenderTargets.FreeArray();

  for ( TS32 x = 0; x < Project.GetRenderTargetCount(); x++ )
  {
    CphxRenderTarget_Tool *rt = Project.GetRenderTargetByIndex( x );
    TS32 rtx = 1, rty = 1;
    GetRenderTargetResolution( rt->ResolutionDescriptor, rtx, rty, XRes, YRes );
    CCoreDX11Texture2D *t = new CCoreDX11Texture2D( (CCoreDX11Device*)Dev );
    t->Create( rtx, rty, NULL, 8, pixelFormats[ rt->pixelFormat ], true );
    RenderTargets += t;
  }

  DepthBuffer = new CCoreDX11Texture2D( (CCoreDX11Device*)Dev );
  DepthBuffer->CreateDepthBuffer( XRes, YRes );

  Internal = new CCoreDX11Texture2D( (CCoreDX11Device*)Dev );
  Internal->Create( XRes, YRes, NULL, 8, COREFMT_R16G16B16A16_FLOAT, true );
}
```

`ApplyRenderTargets` then wires the tool's GPU resources into the runtime structs and — importantly
— resolves event target GUIDs and rebuilds the runtime render-target pointer array
(`apxProject.cpp:1144-1190`):

```cpp
void CapexProject::ApplyRenderTargets( CapexRenderTargetCollection *Collection )
{
  extern CphxRenderTarget *phxInternalRenderTarget;
  phxInternalRenderTarget = &InternalRenderTarget.rt;
  InternalRenderTarget.rt.RTView = Collection->Internal->GetRenderTargetView();
  ...
  for ( TS32 x = 0; x < Collection->RenderTargets.NumItems(); x++ )
  {
    RenderTargets[ x ]->rt.RTView = Collection->RenderTargets[ x ]->GetRenderTargetView();
    ...
  }

  SAFEDELETE( Timeline->Timeline->RenderTargets );
  Timeline->Timeline->RenderTargetCount = RenderTargets.NumItems();
  ...
  for ( TS32 x = 0; x < Timeline->Events.NumItems(); x++ )
  {
    CphxRenderTarget_Tool *t = Project.GetRenderTarget( Timeline->Events[ x ]->TargetID );
    Timeline->Events[ x ]->Event->Target = NULL;
    if ( t ) Timeline->Events[ x ]->Event->Target = &t->rt;
  }

  phxDepthBufferView = Collection->DepthBuffer->GetDepthView();
  phxDepthBufferShaderView = Collection->DepthBuffer->GetShaderResourceView();
}
```

Multiple collections can coexist (editor viewport, timeline preview, video dumper), each spawning its
own full set of render targets and re-pointing the globals on every draw
(`SpawnRenderTargetCollection`, `UpdateRTCollections`). For a headless player you need exactly one
collection.


---

## 3. RENDER LAYERS

### 3.1 Types

`apEx/Phoenix/RenderLayer.h:53-83` **[VERIFIED]**:

```cpp
class CphxRenderLayerDescriptor
{

public:

  bool OmitDepthBuffer;
  bool VoxelizerLayer;
  bool clearRenderTargets;
  int TargetCount;
  CphxRenderTarget **Targets;

#ifndef PHX_MINIMAL_BUILD
  bool IgnoreHelperObjects;
#endif

  void SetEnvironment( bool ClearColor, bool ClearZ, int cubeResolution );
  void GenMipmaps();

};

class CphxRenderLayer
{
public:

  CphxRenderLayerDescriptor *Descriptor;
  CphxArray<CphxRenderDataInstance*> RenderInstances;

};

void __fastcall GetRenderTargetResolution( unsigned char ResData, int &XRes, int &YRes, int DemoResX, int DemoResY );
```

A **render layer** is: *a simultaneous set of render targets + a depth-buffer policy + a clear
policy + an ordered bucket of draw instances*. It is **not** a render pass in the Vulkan sense.
A layer maps naturally to a Vulkan render pass instance (multiple color attachments + optional
depth), but note:

* Color attachments have **different sizes** in general (a layer may bind the full-res RT plus two
  half-res RTs). D3D11 allows this because the viewport is clipped per-target; Vulkan requires
  identical dimensions for all attachments of a framebuffer — a port must either restrict layers to
  same-size targets or use `VK_KHR_imageless`/render-pass-per-target passes. In `cleanslate.apx` the
  *Solid Layer* binds `Backbuffer` (full) + `Albedo and Metalness` (full) + `Normal and roughness`
  (full) — same size, so this is not exercised. **[UNCERTAIN]** whether any shipped project mixes
  sizes in one layer.
* `CphxRenderLayer` has **no explicit sort order other than array order**, which is the project's
  `renderlayer` element order.

`apEx/Phoenix/RenderLayer.h:10-51` — the draw item **[VERIFIED]**:

```cpp
class CphxRenderDataInstance
{

public:

  //class CphxModelObject_Mesh *Model;

  int RenderPriority;

  bool Wireframe;
  bool Indexed;

  ID3D11Buffer *VertexBuffer;
  ID3D11Buffer *IndexBuffer;
  ID3D11Buffer *WireBuffer;
  int TriIndexCount;
  int WireIndexCount;

  ID3D11VertexShader *VS;  //WARNING! DO NOT CHANGE THE ORDER OF THESE DUE TO A MEMCPY IN CreateRenderDataInstances() in Material.cpp!
  ID3D11PixelShader *PS;
  ID3D11GeometryShader *GS;
  ID3D11HullShader *HS;
  ID3D11DomainShader *DS;

  ID3D11BlendState *BlendState; //WARNING! DO NOT CHANGE THE ORDER OF THESE DUE TO A MEMCPY IN CreateRenderDataInstances() in Material.cpp!
  ID3D11RasterizerState *RasterizerState;
  ID3D11DepthStencilState *DepthStencilState;
  ID3D11ShaderResourceView *Textures[ 8 ];

  //ID3D11Buffer *Buffers[3]; //buffer 0 is the matrix data, 1 is the static material data, 2 is the dynamic material data	

  D3DXMATRIX Matrices[ 2 ];
  float MaterialData[ MATERIALDATASIZE / 4 ];

  void *ToolData;

  void Render();

};
```

`Matrices[0]` = world, `Matrices[1]` = inverse-transpose world. `MaterialData` = 64 floats =
static constants followed by animated constants (see §6.4). The 5 shader pointers and the 3 state
pointers + 8 textures are filled by `memcpy`, so the *field order is ABI*.

### 3.2 The draw itself

`apEx/Phoenix/RenderLayer.cpp:27-101` **[VERIFIED]**:

```cpp
void CphxRenderDataInstance::Render()
{
  if ( !VS || !PS ) return; //kill invalid batches - required to avoid some driver crashes in the tool
  if ( !VertexBuffer ) return;

  unsigned int offset = 0;
  unsigned int stride = PHXVERTEXFORMATSIZE;

#ifdef PHX_HAS_STANDARD_PARTICLES
  if ( Indexed )
#endif
    phxContext->IASetInputLayout( RenderVertexFormat );
#ifdef PHX_HAS_STANDARD_PARTICLES
  else
  {
    phxContext->IASetInputLayout( ParticleVertexFormat );
    stride = 4 * 2 * 4;
  }
#endif

  phxContext->IASetVertexBuffers( 0, 1, &VertexBuffer, &stride, &offset );
  phxContext->IASetPrimitiveTopology( Wireframe ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

#ifdef PHX_HAS_STANDARD_PARTICLES
  if ( !Indexed )
    phxContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_POINTLIST );
#endif

  phxContext->VSSetShader( VS, NULL, 0 );
  phxContext->GSSetShader( GS, NULL, 0 );
  phxContext->HSSetShader( HS, NULL, 0 );
  phxContext->DSSetShader( DS, NULL, 0 );
  phxContext->PSSetShader( PS, NULL, 0 );
  phxContext->RSSetState( RasterizerState );
  phxContext->OMSetBlendState( BlendState, NULL, 0xffffffff );
  phxContext->OMSetDepthStencilState( DepthStencilState, 0 );

  D3D11_MAPPED_SUBRESOURCE map;
  phxContext->Map( ObjectMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
  memcpy( map.pData, Matrices, 16 * 2 * 4 );
  memcpy( ( (unsigned char*)map.pData ) + sizeof( Matrices ), MaterialData, MATERIALDATASIZE );
  phxContext->Unmap( ObjectMatrixBuffer, 0 );

  ...
  {
    phxContext->VSSetShaderResources( 0, 8, Textures );
    phxContext->GSSetShaderResources( 0, 8, Textures );
    phxContext->PSSetShaderResources( 0, 8, Textures );
  }

  phxContext->IASetIndexBuffer( Wireframe ? WireBuffer : IndexBuffer, DXGI_FORMAT_R32_UINT, 0 );

#ifdef PHX_HAS_STANDARD_PARTICLES
  if ( Indexed )
  {
#endif
    if ( Wireframe ? WireIndexCount : TriIndexCount )
      phxContext->DrawIndexed( Wireframe ? WireIndexCount : TriIndexCount, 0, 0 );
#ifdef PHX_HAS_STANDARD_PARTICLES
  }
  else
    phxContext->Draw( TriIndexCount, 0 ); //particles are non indexed
#endif
}
```

* One dynamic constant buffer (`ObjectMatrixBuffer`, `b1`) is re-uploaded and re-bound
  **per draw call** — the port should use per-draw push constants or a ring buffer.
* Vertex format: `PHXVERTEXFORMATSIZE = (3+3+3+2*4)*4 + 4 = 84` bytes
  (`phxEngine.h:53`), matching `vxdecl2` in `phxEngine.cpp:136-144`:
  `POSITION0 (R32G32B32), POSITION1 (R32G32B32), NORMAL0 (R32G32B32), COLOR0 (R8G8B8A8_UNORM),
  TEXCOORD0 (R32G32B32A32), TEXCOORD1 (R32G32B32A32)`.
* Indices are `R32_UINT`. Wireframe uses a **separate line index buffer** (`WireBuffer`,
  `EdgeCount*2` indices) and `LINELIST`.
* Texture slots 0..7 are bound to VS, GS and PS simultaneously (PS is the usual consumer).
* `TriIndexCount` is actually a **vertex/index count**: `Model->TriCount * 3` for indexed meshes
  (`Material.cpp:201`), `VertexCount` for non-indexed particles.
* Particles use a distinct layout `ParticleVertexFormat` with 32-byte stride (8 floats) and
  `POINTLIST` when not indexed (`phxEngine.cpp:193-195`, `vxdecl3`).

### 3.3 Setting the environment (clearing, viewport, depth)

`apEx/Phoenix/RenderLayer.cpp:103-147` **[VERIFIED]**:

```cpp
static FLOAT col[ 4 ] = { 0, 0, 0, 0 };
static D3D11_VIEWPORT viewports[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];
static ID3D11RenderTargetView *rendertargets[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ];

void CphxRenderLayerDescriptor::SetEnvironment( bool ClearColor, bool ClearZ, int cubeResolution )
{
  for ( int x = 0; x < TargetCount; x++ )
  {
    viewports[ x ].TopLeftX = viewports[ x ].TopLeftY = 0;
    viewports[ x ].Width = !cubeResolution ? (float)Targets[ x ]->XRes : cubeResolution;
    viewports[ x ].Height = !cubeResolution ? (float)Targets[ x ]->YRes : cubeResolution;
    viewports[ x ].MinDepth = 0;
    viewports[ x ].MaxDepth = 1;
    rendertargets[ x ] = Targets[ x ]->RTView;
    if ( ClearColor && clearRenderTargets )
      phxContext->ClearRenderTargetView( Targets[ x ]->RTView, col );
  }

#ifdef PHX_VOLUMETRIC_RENDERTARGETS
  if ( !VoxelizerLayer )
    {
#endif
  phxContext->RSSetViewports( TargetCount, viewports );
#ifdef PHX_VOLUMETRIC_RENDERTARGETS
  phxContext->OMSetRenderTargetsAndUnorderedAccessViews( TargetCount, rendertargets, OmitDepthBuffer ? NULL : phxDepthBufferView, TargetCount, 0, NULL, NULL );
#else
  phxContext->OMSetRenderTargets( TargetCount, rendertargets, OmitDepthBuffer ? NULL : phxDepthBufferView );
#endif
  if ( ClearZ && clearRenderTargets )
    phxContext->ClearDepthStencilView( phxDepthBufferView, ( D3D10_CLEAR_DEPTH * !OmitDepthBuffer ) | D3D10_CLEAR_STENCIL, 1, 0 );
#ifdef PHX_VOLUMETRIC_RENDERTARGETS
  }
    else
    {
      phxContext->ClearUnorderedAccessViewFloat( phxTexture3DUAV, col );
      viewports[ 0 ].TopLeftX = viewports[ 0 ].TopLeftY = 0;
      viewports[ 0 ].Width = 256;
      viewports[ 0 ].Height = 256;
      viewports[ 0 ].MinDepth = 0;
      viewports[ 0 ].MaxDepth = 1;
      phxContext->RSSetViewports( 1, viewports );
      phxContext->OMSetRenderTargetsAndUnorderedAccessViews( 1, &phxVoxelForceMSAARTView, nullptr, 1, 1, &phxTexture3DUAV, nullptr );
    }
#endif
}
```

Semantics to port:

* **Clear happens per layer, inside the layer loop, before that layer's draws.** Combined with the
  frame-level clear in `CphxTimeline::Render`, an artist can clear a target "again" mid-frame from a
  later event/layer.
* Colour clear value is always `(0,0,0,0)`; depth 1.0; stencil 0. `ClearColor`/`ClearZ` come from
  the `EVENT_RENDERSCENE` (`<clearcolor>`, `<clearz>`), `clearRenderTargets` from the layer.
  So the effective predicate is `eventClear && layerClear`.
* `OmitDepthBuffer` → no depth attachment bound *and* no depth clear. This is how the "Lighting
  Layer" (an additive full-screen lighting accumulation layer in Clean Slate) avoids depth testing.
* `cubeResolution != 0` overrides viewport size (used by the commented-out cube-map camera path,
  `Timeline.cpp:207-228`) — currently dead code.
* Viewport per attachment is independent, which is why mixed-resolution targets are drawable in
  D3D11 but not directly in Vulkan.

### 3.4 Mipmap generation

`RenderLayer.cpp:151-158` **[VERIFIED]**:

```cpp
void CphxRenderLayerDescriptor::GenMipmaps()
{
  for ( int x = 0; x < TargetCount; x++ )
  {
    //if ( Targets[x]->DoMips )
      phxContext->GenerateMips( Targets[x]->View );
  }
}
```

Called after every layer (`Scene.cpp:201`) and after every shadertoy pass. Every render target in
the project was created with a full mip chain and `D3D11_RESOURCE_MISC_GENERATE_MIPS`
(§2.1, `Project.cpp:196-217`; tool: `RenderTargets.cpp:7-55` with `AutoMipmap=true`). Downstream
blur/glow shaders sample explicit mip levels, so a port must generate mips (or reproduce the
box-filter chain) after each layer write.

### 3.5 `TargetLayer` — how techniques select a layer

**Authoring side** (`apEx/Phoenix_Tool/Material_Tool.cpp:271-286`) **[VERIFIED]**:

```cpp
void CphxMaterialTechnique_Tool::SetTargetLayer( CphxRenderLayerDescriptor_Tool *t )
{
  CphxRenderLayerDescriptor_Tool *o = Project.GetRenderLayer( TargetLayer );
  if ( o ) RemoveParent( o );

  if ( !t )
  {
    TargetLayer.SetString( _T( "NONENONENONENONENONENONENONENONE" ) );
    Tech->TargetLayer = NULL;
    return;
  }

  AddParent( t );
  TargetLayer = t->GetGUID();
  Tech->TargetLayer = &t->RenderLayer;
}
```

In the XML this is the technique's `<TargetLayer>` GUID (`Material_Tool.cpp:224`); a real example
from `cleanslate.apx` is `<TargetLayer>0BE1808FCCA5B61BC690EAC74EADDEA5</TargetLayer>` =
*"Lighting Layer"*.

**Export** (`apEx/apEx/MinimalExport.cpp:2634-2646`) **[VERIFIED]** — the GUID is resolved to the
layer's **resource index** and written as one byte:

```cpp
      CphxRenderLayerDescriptor_Tool* l = Project.GetRenderLayer( t->TargetLayer );
      TS32 targetlayer = 0;
      if ( l ) targetlayer = l->GetResourceIndex();

      if ( !l && t->Tech->Type == TECH_MATERIAL )
      {
        LOG_ERR( "[minimalexport] Fatal error: Missing target renderlayer in project! (Tech: %s)", t->Name.GetPointer() );
        MinimalExportSuccessful = false;
        return;
      }
      ...
      WriteByte( Stream_Main, targetlayer, MaterialTechDataSize ); //target layer
```

**Load** (`apEx/Phoenix/Project.cpp:512`) **[VERIFIED]**:

```cpp
    Techniques[ x ].TargetLayer = &RenderLayers[ readbyte( Stream_Main ) ];
```

**Binding** (`apEx/Phoenix/Material.cpp:162` and `Scene.cpp:205-216`) **[VERIFIED]**:

```cpp
    //add instance to render queue
    RootScene->AddRenderDataInstance( TargetLayer, ri );
```
```cpp
void CphxScene::AddRenderDataInstance( CphxRenderLayerDescriptor *Layer, CphxRenderDataInstance *RDI )
{
  for ( int x = 0; x < LayerCount; x++ )
    if ( RenderLayers[ x ]->Descriptor == Layer )
    {
      RenderLayers[ x ]->RenderInstances.Add( RDI );
      return;
    }
#ifndef PHX_MINIMAL_BUILD
  delete RDI;
#endif
}
```

All scenes share the **same global** `CphxRenderLayerDescriptor` array (`Project.cpp:324-335`,
scenes reference them by pointer at `Project.cpp:1026-1033`). A render instance whose technique has
no layer is silently dropped (in minimal builds it leaks).

### 3.6 Instance ordering within a layer

`apEx/Phoenix/Scene.cpp:16-49` **[VERIFIED]** — quicksort with **descending** `RenderPriority`:

```cpp
int Pivot( CphxRenderDataInstance **Instances, int first, int last )
{
  int  p = first;
  int pivot = Instances[ first ]->RenderPriority; //moving this value out of the main loop makes this sort stable. I think.

  CphxRenderDataInstance *temp;

  for ( int i = first + 1; i <= last; i++ )
  {
    if ( Instances[ i ]->RenderPriority > pivot )
    {
      p++;
      temp = Instances[ i ];
      Instances[ i ] = Instances[ p ];
      Instances[ p ] = temp;
    }
  }

  temp = Instances[ p ];
  Instances[ p ] = Instances[ first ];
  Instances[ first ] = temp;

  return p;
}

void SortRenderLayer( CphxRenderDataInstance **Instaces, int first, int last )
{
  if ( first < last )
  {
    int pivot = Pivot( Instaces, first, last );
    SortRenderLayer( Instaces, first, pivot - 1 );
    SortRenderLayer( Instaces, pivot + 1, last );
  }
}
```

* `RenderPriority` default is **127** (`Project.cpp:1862`), set by any `PARAM_RENDERPRIORITY`
  (type 6) parameter on the technique or pass (`Project.cpp:1960-1962`).
* Higher priority is drawn **first**. The sort is not stable and is not depth-sorted — transparency
  ordering is manual via `PARAM_RENDERPRIORITY`.
* Called as `SortRenderLayer(..., 0, ItemCount - 1)` (`Scene.cpp:77-78`); with 0 or 1 items the
  bounds are degenerate but harmless.
* `CphxArray::FreeArray` is *not* a destructor-style free: it resets the count
  (`phxarray.h`) — instances are re-`new`ed each frame by `Material.cpp:122`.

### 3.7 Voxelizer layer

`VoxelizerLayer` selects a completely different render path: a 256×256 viewport and an
**Unordered Access View into a 3D texture** (`phxTexture3D`, `R8G8B8A8_UNORM` force-MSAA target)
instead of colour targets (`RenderLayer.cpp:133-146`; setup in `phxEngine.cpp:221-296`).

* The 3D texture is `8×8×8` `R16G16B16A16_FLOAT` with mips and a UAV
  (`phxEngine.cpp:223-274`) — used for voxel cone tracing / volumetric fog.
* After a voxelizer layer, the engine generates mips of the *3D texture*
  (`Scene.cpp:196-200`).
* `BuildPassState` also disables `DepthClipEnable` for voxelizer-target techniques
  (`Project.cpp:1852-1855`).
* ⚠ **The exported render-layer byte does not carry a Voxelizer bit**
  (`MinimalExport.cpp:2287-2292` writes only `targetcount | OmitDepthBuffer<<6 |
  clearRenderTargets<<7`), and the runtime loader never assigns
  `RenderLayers[x].VoxelizerLayer` (`Project.cpp:326-335`). Without
  `PHX_VOLUMETRIC_RENDERTARGETS` and with `new[]` default-initialisation, the field is
  **indeterminate**. **[UNCERTAIN]** whether the shipped Clean Slate build uses the voxelizer at
  all; treat any voxelizer support as build-specific.

### 3.8 `IgnoreHelperObjects` / `Pickable`

Tool-only (`RenderLayer.h:64-66`, `RenderLayer.cpp` XML fields `IgnoreHelperObjects`, `Pickable`).
Never serialised to the release blob (`MinimalExport.cpp:2287-2292`). Not needed for a player.


---

## 4. TIMELINE

### 4.1 Document structure (the `.apx` project file)

The `.apx` file in `Projects/` is **plain XML** (verified: `cleanslate.apx` starts with
`<apExProject build="0498_19821r">`). The root is `apExProject`; children are emitted in this order
by `CapexProject::ExportToString` (`apxProject.cpp:721-846`) and read back by
`CapexProject::ImportMain` (`apxProject.cpp:420-529`):

```
<apExProject build="...">
  <AspectX/> <AspectY/> <Framerate/> <Title/> <Group/> <www/> <pouet/> <facebook/> <youtube/> <share/>
  <shaderminifierenabled/> <globalshaderminifierenabled/> <minimalprecalc/> <farbrauschprecalc/> <setuphassocial/>
  <gridbpm/> <gridprimaryhighlight/> <gridsecondaryhighlight/> <songfile/>
  <treespecies>*            (optional)
  <texturefilter>*          GUID, Name, Code(HLSL pixel shader), DataDescriptor sub-fields
  <texturepage>*            texgen operator graphs
  <rendertarget>*           GUID, Name, ResolutionDescriptor, ZResolution, HiddenFromTimeline, PixelFormat, CubeMap
  <renderlayer>*            GUID, Name, OmitDepthBuffer, ClearRenderTargets, Voxelizer, IgnoreHelperObjects, Pickable, RenderTarget+
  <rendertechnique>*        GUID, Name, Type, TargetLayer, Parameter*, Pass+
  <material>*               GUID, Name, Tech+
  <model>*                  GUID, Name, Object+ (mesh primitives)
  <scene>*                  GUID, Name, Clip+, Object+ (scene objects)
  <event>*                  GUID, Type, Pass, StartFrame, EndFrame, TargetRT, Name, TimeSpline, +type-specific
</apExProject>
```

`ImportMain` order matters and is deliberately two-pass for scenes (`apxProject.cpp:446-460`):
scenes are created GUID-first so subscene references resolve, then filled.

### 4.2 `<renderlayer>` — real example

`cleanslate.apx` (all four layers) **[VERIFIED]**:

```xml
  <renderlayer>
    <GUID>FCE6163CC00DB65A95211DB17ADD9460</GUID>
    <Name>Shadow Layer</Name>
    <OmitDepthBuffer>0</OmitDepthBuffer>
    <ClearRenderTargets>1</ClearRenderTargets>
    <Voxelizer>0</Voxelizer>
    <IgnoreHelperObjects>1</IgnoreHelperObjects>
    <Pickable>0</Pickable>
    <RenderTarget>777C0EC1778502D5674EEA52BFD29691</RenderTarget></renderlayer>

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

  <renderlayer>
    <GUID>0BE1808FCCA5B61BC690EAC74EADDEA5</GUID>
    <Name>Lighting Layer</Name>
    <OmitDepthBuffer>1</OmitDepthBuffer>
    <ClearRenderTargets>0</ClearRenderTargets>
    <Voxelizer>0</Voxelizer>
    <IgnoreHelperObjects>1</IgnoreHelperObjects>
    <Pickable>0</Pickable>
    <RenderTarget>B804E9F623AABCBA232F247216EE216B</RenderTarget></renderlayer>

  <renderlayer>
    <GUID>1424586010B909EA488C988A1F5021F9</GUID>
    <Name>Transparent Layer</Name>
    <OmitDepthBuffer>0</OmitDepthBuffer>
    <ClearRenderTargets>0</ClearRenderTargets>
    <Voxelizer>0</Voxelizer>
    <IgnoreHelperObjects>1</IgnoreHelperObjects>
    <Pickable>0</Pickable>
    <RenderTarget>B804E9F623AABCBA232F247216EE216B</RenderTarget></renderlayer>
```

(formatted as in the file; note D3D11 MRT slot = document order of `<RenderTarget>`.)

Writer/reader are `CphxRenderLayerDescriptor_Tool::ExportData` / `ImportData` /
`AddRenderTarget` (`apEx/Phoenix_Tool/RenderLayer.cpp:22-66`) **[VERIFIED]**:

```cpp
void CphxRenderLayerDescriptor_Tool::ExportData( CXMLNode *Node )
{
  Node->AddChild( _T( "Name" ), false ).SetText( Name.GetPointer() );
  Node->AddChild( _T( "OmitDepthBuffer" ), false ).SetInt( RenderLayer.OmitDepthBuffer );
  Node->AddChild( _T( "ClearRenderTargets" ), false ).SetInt( RenderLayer.clearRenderTargets );
  Node->AddChild( _T( "Voxelizer" ), false ).SetInt( RenderLayer.VoxelizerLayer );
  Node->AddChild( _T( "IgnoreHelperObjects" ), false ).SetInt( RenderLayer.IgnoreHelperObjects );
  Node->AddChild( _T( "Pickable" ), false ).SetInt( HasPicking );
  for ( TS32 x = 0; x < RenderTargets.NumItems(); x++ )
    Node->AddChild( _T( "RenderTarget" ), false ).SetText( RenderTargets[ x ]->GetGUID().GetString() );
}
```

### 4.3 `<rendertarget>` — real examples

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

`ZResolution` is read-only/legacy
(`RenderTargets.cpp:104-106`), `HiddenFromTimeline` only affects the editor's RT picker, and
`PixelFormat` ∈ {0 = `R16G16B16A16_FLOAT`, 1 = `R32_FLOAT`}.
`ResolutionDescriptor` semantics are in §2.1.

### 4.4 `<scene>` — exact structure

`CphxScene_Tool::ExportData` (`Scene_tool.cpp:168-181`) **[VERIFIED]**:

```cpp
void CphxScene_Tool::ExportData( CXMLNode *Node )
{
  Node->AddChild( _T( "Name" ), false ).SetText( Name.GetPointer() );

  for ( TS32 x = 0; x < Clips.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "Clip" ) );
    Clips[ x ]->Export( &n );
  }

  for ( TS32 x = 0; x < Objects.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "Object" ) );
    n.SetAttributeFromInteger( _T( "Type" ), Objects[ x ]->GetObjectType() );
    Objects[ x ]->Export( &n );
  }

}
```

`<Clip>` is `CphxSceneClip` — **only a GUID and a display Name** (`Scene_tool.cpp:1733-1741`):

```xml
      <Clip>
        <GUID>61CF2DA6DBA99B9C946F62C5EEA3135B</GUID>
        <Name>1</Name></Clip>
```

A clip is a *named time-space* — a set of per-object spline sets. `EVENT_RENDERSCENE`'s `<clip>` is
one of these GUIDs; the runtime resolves it to an **index** into the scene's clip array
(`rs->Clip = readbyte(Stream_Main)`, `Project.cpp:1406`) which indexes `Clips[Clip]` in
`CphxObject::CalculateAnimation` (`Scene.cpp:299`). Clips are not exported unless required
(`MarkRequiredClips`, `MinimalExport.cpp:2051-2075`).

**Minimal complete real `<scene>`** (`cleanslate.apx`, *"asset / butterfly particle"*) **[VERIFIED]**:

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
        <clipspline type="8">   <spline><interpolation>1</interpolation><loop>0</loop>
                                <waveform>0</waveform><multiplicativewaveform>0</multiplicativewaveform>
                                <wfamplitude>15360</wfamplitude><wffrequency>18688</wffrequency>
                                <wfrandseed>0</wfrandseed>
                                <value id="0">0</value><value id="1">0</value>
                                <value id="2">0</value><value id="3">0</value>
        </spline></clipspline>
        ... clipspline types 9,10,1,2,3,4,5,6,55,56 ...
      </clipdata>
    </Object>
  </scene>
```

`<Object Type="N">` inside a `<scene>` uses the **`PHXOBJECT`** enum (`Scene.h:95-111`) — i.e.
`Type` ∈ {0 Model, 1 Light, 2 CamEye, 3 Dummy, 4 SubScene, 5 ParticleEmitter, 6
ParticleEmitterCPU, 7 ParticleDrag, 8 ParticleGravity, 9 ParticleTurbulence, 10 ParticleVortex, 11
LogicObject}. Disambiguated from `<Object Type="N">` inside a `<model>`, which uses the
**`PHXMESHPRIMITIVE`** enum (see §4.9). ⚠ Both elements are named `Object` with a `Type` attribute.
**[VERIFIED]** by `Scene_tool.cpp:187-256` (scene) vs `Model_Tool.cpp:42-58` (model).

Scene-object children (all optional, written by `CphxObject_Tool::ExportData`,
`Scene_tool.cpp:1267-1280`):

| element | meaning |
|---|---|
| `<Name>` | display name |
| `<clipdata targetclip="GUID">` | one per scene clip; the object's animation for that clip |
| `<targetobject>` GUID | "look at" target |
| `<parentobject>` GUID | hierarchy parent |
| `<camcenterx>` / `<camcentery>` | signed char off-centre projection offsets (also logic-object payload) |
| type-specific | `<model>` (Model), `<pointlight>` (Light), `<scene>` (SubScene), `<EmitedObject>`/`<EmitedScene>` (ParticleEmitterCPU) |

### 4.5 `<clipdata>` / `<clipspline>` / `<ParamData>` — how clips drive objects

`CphxObjectClipArray_Tool::ExportData` (`Scene_tool.cpp:1854-1862`) **[VERIFIED]**:

```cpp
void CphxObjectClipArray_Tool::ExportData( CXMLNode *Node )
{
  for ( TS32 x = 0; x < Clips.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "clipdata" ) );
    n.SetAttribute( _T( "targetclip" ), Clips[ x ]->GetSceneClipGUID().GetString() );
    Clips[ x ]->ExportData( &n );
  }
}
```

`CphxObjectClip_Tool::ExportData` (`Scene_tool.cpp:2055-2088`) **[VERIFIED]**:

```cpp
void CphxObjectClip_Tool::ExportData( CXMLNode *Node )
{
  if ( SubSceneTarget )
  {
    Node->AddChild( _T( "subscenetarget" ) ).SetText( SubSceneTarget->GetGUID().GetString() );
  }

  Node->AddChild( _T( "randseed" ) ).SetInt( RandSeed );
  Node->AddChild( _T( "turbulencefreq" ) ).SetInt( TurbulenceFrequency );

  for ( TS32 x = 0; x < Splines.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "clipspline" ) );
    Splines[ x ]->ExportData( &n );
  }

  for ( TS32 x = 0; x < MaterialSplines.Splines.NumItems(); x++ )
  {
    if ( MaterialSplines.Splines[ x ]->Spline.Target->Scope == PARAM_ANIMATED )
    {
      CXMLNode n = Node->AddChild( _T( "ParamData" ) );
      n.AddChild( _T( "TargetGUID" ) ).SetText( MaterialSplines.Splines[ x ]->TargetParamGUID.GetString() );
      n.AddChild( _T( "GroupingGUID" ) ).SetText( MaterialSplines.Splines[ x ]->TargetObjectGUID.GetString() );
      CXMLNode s = n.AddChild( _T( "Spline" ) );
      MaterialSplines.Splines[ x ]->Splines[ 0 ].ExportData( s );
      if ( MaterialSplines.Splines[ x ]->Spline.Target->Type == PARAM_COLOR )
      {
        for ( TS32 y = 1; y < 4; y++ )
        {
          s = n.AddChild( _T( "Spline" ) );
          MaterialSplines.Splines[ x ]->Splines[ y ].ExportData( s );
        }
      }
    }
  }
}
```

`CphxClipSpline_Tool::ExportData` (`Scene_tool.cpp:2252-2260`) **[VERIFIED]**:

```cpp
void CphxClipSpline_Tool::ExportData( CXMLNode *Node )
{
  Node->SetAttributeFromInteger( _T( "type" ), MinimalSpline.Type );
  if ( Parameter ) Node->SetAttribute( _T( "materialparam" ), Parameter->GetGUID().GetString() );
  if ( GroupingObject ) Node->SetAttribute( _T( "groupingobject" ), GroupingObject->GetGUID().GetString() );
  CXMLNode n = Node->AddChild( _T( "spline" ) );
  Spline->ExportData( n );
}
```

* `type` = `PHXSPLINETYPE` (see §5.2 for the full enum); e.g. `type="8"` = `Spline_Position_x`,
  `type="4"` = `Spline_Rotation` (quaternion), `type="0"` = `Spline_MaterialParam`.
* `SubSceneTarget` + `Spline_SubScene_Clip/Time/RepeatCount/RepeatTimeOffset` (types 5, 6, 55, 56)
  is the subscene mechanism — see §4.11.
* `randseed` / `turbulencefreq` only feed particle turbulence.

### 4.6 `<spline>` grammar

`CphxSpline_Tool_float16::ExportData` (`apEx/Phoenix_Tool/phxSplineExt.cpp:302-314`) and
`CphxSpline_Tool::ExportKeys` (`phxSplineExt.cpp:61-107`) **[VERIFIED]**:

```xml
<spline>
  <interpolation>0|1|2|3</interpolation>        <!-- CONSTANT | LINEAR | CUBIC | BEZIER -->
  <loop>0|1</loop>
  <waveform>0..5</waveform>                     <!-- NONE|SIN|SQUARE|TRIANGLE|SAWTOOTH|NOISE -->
  <multiplicativewaveform>0|1</multiplicativewaveform>
  <wfamplitude>HALF_BITS</wfamplitude>          <!-- raw IEEE-754 half bit pattern as decimal -->
  <wffrequency>HALF_BITS</wffrequency>
  <wfrandseed>0..255</wfrandseed>

  <!-- EITHER: constant/default form (no keys) -->
  <value id="0">HALF_BITS</value> <value id="1">…</value> <value id="2">…</value> <value id="3">…</value>

  <!-- OR: keyed form, one <key> per key -->
  <key>
    <time>0..255</time>
    <value id="0">HALF_BITS</value> … <value id="3">…</value>
    <controlpos id="0">0..255</controlpos> <controlpos id="1">…</controlpos>
    <controlvalue id="0">HALF_BITS</controlvalue> … <controlvalue id="5">…</controlvalue>
  </key>
</spline>
```

* **Values are `D3DXFLOAT16` raw bits stored as decimal integers.** `15360 == 0x3C00 == half(1.0)`,
  `0 == half(0.0)`. **[VERIFIED]** from real data and from
  `v.SetInt( *((WORD*)&Keys[x]->Key.Value[y]) )`.
* **Key times are bytes 0..255**, mapped to [0,1] by
  `SPLINEPOSITIONCALC(t) = (t ? (t+1)/256 : 0)` (`phxSpline.h:15`) — so `t=255 → 1.0`, `t=0 → 0.0`.
  **[VERIFIED]**
* Bezier control points are stored as two byte **positions** and six half **values**
  (`CphxSplineKey::controlvalues[6]`, `controlpositions[2]`).
* Quaternion splines use `id="0..3"` = x,y,z,w and interpolation `SLERP`/`SQUAD`
  (`phxSplineExt.cpp:536-545`).

Evaluation is exactly `CphxSpline::CalculateValue` — see §5.3.

### 4.7 `<event>` — base element and real examples

Writer (`apEx/Phoenix_Tool/Timeline_tool.cpp:37-47`) **[VERIFIED]**:

```cpp
void CphxEvent_Tool::ExportData( CXMLNode *Node )
{
  Node->AddChild( _T( "Type" ), false ).SetInt( Type );
  Node->AddChild( _T( "Pass" ), false ).SetInt( Pass );
  Node->AddChild( _T( "StartFrame" ), false ).SetInt( Event->StartFrame );
  Node->AddChild( _T( "EndFrame" ), false ).SetInt( Event->EndFrame );
  Node->AddChild( _T( "TargetRT" ), false ).SetText( TargetID.GetString() );
  Node->AddChild( _T( "Name" ), false ).SetText( Name.GetPointer() );
  CXMLNode n = Node->AddChild( _T( "TimeSpline" ) );
  Time->ExportData( n );
  ExportEventData( Node );
}
```

Type values (`Timeline.h:12-23`, `PHXEVENTTYPE`) **[VERIFIED]**:

```cpp
enum PHXEVENTTYPE
{
  EVENT_ENDDEMO = 0,
  EVENT_RENDERDEMO = 1,
  EVENT_SHADERTOY = 2,
  EVENT_RENDERSCENE = 3,
  EVENT_PARTICLECALC = 4,
  EVENT_CAMERASHAKE = 5,
  EVENT_CAMERAOVERRIDE = 6,

  Event_Count
};
```

Real `<event>` elements from `cleanslate.apx` **[VERIFIED]** (splines elided):

```xml
  <!-- Type 0: END DEMO -->
  <event>
    <GUID>65E65420BE2F7C9F072B3658E196EBA8</GUID>
    <Type>0</Type>
    <Pass>0</Pass>
    <StartFrame>33620</StartFrame>
    <EndFrame>34125</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline> …interpolation 1, 2 keys… </TimeSpline>
  </event>

  <!-- Type 3: RENDER SCENE -->
  <event>
    <GUID>3CC6D9E8B6558CDEA1F0500A23BFB696</GUID>
    <Type>3</Type>
    <Pass>1</Pass>
    <StartFrame>3601</StartFrame>
    <EndFrame>4551</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline> …3 keys… </TimeSpline>
    <scene>6253E77F4768F1E6295AE92C7979103A</scene>
    <clip>C4434B47C71BD2D4EFAB32129C0BE84C</clip>
    <clearcolor>1</clearcolor>
    <clearz>1</clearz>
  </event>

  <!-- Type 2: SHADERTOY (post-process) -->
  <event>
    <GUID>F2FD6C08B9A3A90CCE91B5ACF2B8832D</GUID>
    <Type>2</Type>
    <Pass>3</Pass>
    <StartFrame>2714</StartFrame>
    <EndFrame>2743</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline> …2 keys… </TimeSpline>
    <Tech>F4E3856F6D1820356FE044FFE0B23BA8</Tech>
    <TechData>
      <materialparamdata paramid="70AF100D90E431685DD352574806D940" valuebase64="mAAAAAAAAAAAAAAAAAAAAA=="/>
      <materialtexturedata paramid="41DB0CCE0B91D7296B0AA932854751F7" textureid="B804E9F623AABCBA232F247216EE216B"/>
      <materialtexturedata paramid="53DF0586D46C567A6D0DEEDB3F09EC3B" textureid="C60BD2271CDE28488C782439B5BEC852"/>
      <materialtexturedata paramid="1C3D278330DCD093BECDFB3BAADDB5AC" textureid="B804E9F623AABCBA232F247216EE216B"/>
    </TechData>
    <ParamData>
      <TargetGUID>10D9350B96D248DDB08E981865E9D34B</TargetGUID>
      <Spline> … </Spline>
    </ParamData>
    <ParamData>
      <TargetGUID>742A323BA0C28A3C1335FDF98BEF48A9</TargetGUID>
      <Spline> … </Spline>
    </ParamData>
  </event>

  <!-- Type 4: PARTICLE CALC -->
  <event>
    <GUID>4E90A1D654A62DFAC79E370C2AF23F41</GUID>
    <Type>4</Type> <Pass>1</Pass>
    <StartFrame>5461</StartFrame> <EndFrame>5916</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline> …2 keys… </TimeSpline>
    <scene>D2E15A254B4A5088BD223AD58DB0F5A4</scene>
    <clip>52A07E524070C63EDC136A6B9FEE546B</clip>
  </event>

  <!-- Type 6: CAMERA OVERRIDE -->
  <event>
    <GUID>D2849FF0F4EF4E31B25D07B970290FAC</GUID>
    <Type>6</Type> <Pass>0</Pass>
    <StartFrame>0</StartFrame> <EndFrame>33620</EndFrame>
    <TargetRT>B804E9F623AABCBA232F247216EE216B</TargetRT>
    <Name></Name>
    <TimeSpline> …2 keys… </TimeSpline>
    <scene>642E798F148C457C7831EA6B7030EAA8</scene>
    <clip>85E2AAA4E90AB7D86813E3366FD2634D</clip>
    <camera>38C8996C74B80CEE1816F9E73A21C724</camera>
  </event>
```

`cleanslate.apx` contains 213 events; types present are 0, 2, 3, 4, 6 — **no type 1
(`EVENT_RENDERDEMO`) and no type 5 (`EVENT_CAMERASHAKE`)** in this project. **[VERIFIED]** The
camera work in Clean Slate is done by the type-6 override covering frames 0..33620.

Per-type extra elements (writer code in `apEx/Phoenix_Tool/Timeline_tool.cpp`) **[VERIFIED]**:

| Type | extra elements | writer |
|---|---|---|
| 0 ENDDEMO | none | `Timeline_tool.cpp:57-59` |
| 1 RENDERDEMO | none (stub; `CphxEvent_RenderDemo::Render` does nothing) | `Timeline_tool.cpp:75-77`; `Phoenix/Timeline.cpp:21-26` |
| 2 SHADERTOY | `<Tech>` GUID, `<TechData>` (`materialparamdata`/`materialtexturedata`), `<ParamData>`+`<Spline>` per animated parameter | `Timeline_tool.cpp:449-470` |
| 3 RENDERSCENE | `<scene>`, `<clip>`, `<camera>`, `<clearcolor>`, `<clearz>` | `Timeline_tool.cpp:822-834` |
| 4 PARTICLECALC | `<scene>`, `<clip>`, `<camera>` (camera exported but **ignored** on import/load) | `Timeline_tool.cpp:838-843` |
| 5 CAMERASHAKE | `<ShakesPerSecond Value="N"/>`, `<EyeIntensitySpline>`, `<TargetIntensitySpline>` | `Timeline_tool.cpp:970-1005` |
| 6 CAMERAOVERRIDE | `<scene>`, `<clip>`, `<camera>` (all required) | `Timeline_tool.cpp:1060-1072` |

`materialparamdata`'s `valuebase64` is base64 of a raw 16-byte `MATERIALVALUE` union
(§6.1) — e.g. `mAAAAAAAAAAAAAAAAAAAAA==` decodes to `0x98 00 00 …` (a float 2.37e-23 → the raw
padding of a small struct field). **[VERIFIED: 16 bytes]**

### 4.8 StartFrame / EndFrame / TargetRT / TimeSpline semantics

| field | type | semantics |
|---|---|---|
| `StartFrame` | `unsigned short` at runtime (`Timeline.h:32`) | first frame the event is **active** |
| `EndFrame` | `unsigned short` | runtime: **exclusive** (`f < EndFrame`); tool: inclusive (`f <= EndFrame`) ⚠ |
| `Pass` | int (tool only) | sort key; lower = earlier. Also the editor's RT-editor pass filter |
| `TargetRT` | GUID | the render target the event writes to / presents. **Ignored by the release player** (§2.6) |
| `TimeSpline` | `<spline>` | remaps the event-local 0..1 position. Identity = 2 linear keys `t=0→0`, `t=255→1`, which the exporter omits (`SplineStored=false`, `MinimalExport.cpp:1665-1674`) |
| `Name` | string | editor label only |

Because `prevt = (Frame-1-StartFrame)/(EndFrame-StartFrame)` can be **negative**, the first frame of
an event evaluates the part's splines at negative time. `CphxSpline::CalculateValue` handles `t <
Keys[0]->GetTime()` by clamping to the first key for non-looping splines — so the observable effect
is a clamp, not an extrapolation. **[VERIFIED]**

`unsigned short` frames cap a single event at 65535 frames (~9 minutes at 120 fps). Clean Slate's
whole range is 0..34125. **[VERIFIED]**

### 4.9 `<model>` / mesh objects (the other `Object` element)

`CphxModel_Tool::ExportData` (`Model_Tool.cpp:30-40`) **[VERIFIED]**:

```cpp
void CphxModel_Tool::ExportData( CXMLNode *Node )
{
  Node->AddChild( _T( "Name" ), false ).SetText( Name.GetPointer() );

  for ( TS32 x = 0; x < Objects.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "Object" ) );
    n.SetAttributeFromInteger( _T( "Type" ), Objects[ x ]->GetPrimitive() );
    Objects[ x ]->Export( &n );
  }
}
```

with `PHXMESHPRIMITIVE` from `Mesh.h` (indices are asserted by `MeshPrimitiveParameterCounts[]`,
`Project.cpp:107-129`): `0 Cube, 1 Plane, 2 Sphere, 3 Cylinder, 4 Cone, 5 Arc, 6 Line, 7 Spline, 8
Loft, 9 Clone, 10 Copy, 11 GeoSphere, 12 Scatter, 13 Stored, 14 Tree, 15 TreeLeaves, 16 Text, 17
Marched, 18 StoredMini, 19 Merge`. **[VERIFIED]** for the ordering via the array in `Project.cpp`.

Each mesh object holds (`Model_Tool.cpp:431-489`) **[VERIFIED]**:

```xml
<Object Type="2">
  <Name>Sphere</Name>
  <transformation index="0..11" value="HALF_BITS"/>   <!-- 12 halves: 3x4 row-major of a 4x3 matrix, column x = index%4, row y = index/4 -->
  <parameter index="0..11" value="0..255"/>           <!-- up to 12 primitive parameters -->
  <floatparameter value="HALF_BITS"/>
  <parentguid index="0..1" value="GUID|NONE…"/>
  <Filter Type="N">…</Filter>                          <!-- mesh filters -->
  <textblob>BASE64</textblob>                          <!-- text primitive -->
  <MiniVerts count="…" data="BASE64"/> <MiniTris count="…" data="BASE64"/>
  <clonedobject>GUID</clonedobject>
  <Material GUID="…"/>
  <materialparamdata paramid="…" valuebase64="…"/>
  <materialtexturedata paramid="…" textureid="…"/>
</Object>
```

⚠ Index packing: writer uses `idxx = x % 4; idxy = x / 4; TransformationF16[idxy + idxx*3]`, and
the runtime `CphxModelObject::GetMatrix()` reads `m.m[x][y] = TransformationF16[y + x*3]`
(`Model.cpp:14-23`) — so document index `i` → `m[i%4][i/4]`. **[VERIFIED]**

### 4.10 `<rendertechnique>` / `<material>` / `<Parameter>`

`CphxMaterialTechnique_Tool::ExportData` (`Material_Tool.cpp:220-230`) **[VERIFIED]**:

```cpp
void CphxMaterialTechnique_Tool::ExportData( CXMLNode *Node )
{
  Node->AddChild( _T( "Name" ), false ).SetText( Name.GetPointer() );
  Node->AddChild( _T( "Type" ), false ).SetInt( Tech->Type );
  Node->AddChild( _T( "TargetLayer" ) ).SetText( TargetLayer.GetString() );
  TechParameters.Export( Node );
  for ( TS32 x = 0; x < Passes.NumItems(); x++ )
  {
    CXMLNode n = Node->AddChild( _T( "Pass" ) );
    Passes[ x ]->ExportData( &n );
  }
}
```

`CphxMaterialRenderPass_Tool::ExportData` (`Material_Tool.cpp:115-120`) writes `<Name>`, `<Code>`
(the HLSL, entry points `v`/`p`/`g`), `<Minifiable>`, then `<Parameter>` children.
`Type` is `TECHNIQUETYPE` = {0 MATERIAL, 1 POSTPROCESS, 2 SHADERTOY, 3 PARTICLE}.

`<Parameter>` (`Material_Tool.cpp:457-476`) **[VERIFIED]**:

```cpp
  Node->AddChild( _T( "Name" ) ).SetText( Name.GetPointer() );
  Node->AddChild( _T( "Scope" ) ).SetInt( Parameter.Scope );
  Node->AddChild( _T( "Type" ) ).SetInt( Parameter.Type );

  if ( MaterialParamHasNonGUIDData( Parameter.Type ) )
  {
    CString s = CString::EncodeToBase64( (TU8*)&Parameter.Value, sizeof( Parameter.Value ) );
    if ( Parameter.Scope == PARAM_CONSTANT )
      Node->AddChild( _T( "Value" ) ).SetText( s.GetPointer() );

    s = CString::EncodeToBase64( (TU8*)&DefaultValue, sizeof( DefaultValue ) );
    Node->AddChild( _T( "DefaultValue" ) ).SetText( s.GetPointer() );
  }

  if ( ( ( Parameter.Type >= PARAM_TEXTURE0 && Parameter.Type <= PARAM_TEXTURE7 ) || Parameter.Type == PARAM_RENDERTARGET ) && Parameter.Scope == PARAM_CONSTANT )
    Node->AddChild( _T( "TextureGUID" ) ).SetText( TextureGUID.GetString() );
```

Real material technique from `cleanslate.apx` (a *Lighting Layer* material technique) **[VERIFIED]**:

```xml
  <rendertechnique>
    <GUID>9644C93D8CDBDB4A456B25B09E6F73B8</GUID>
    <Name>[Area] Sphere Light diffuseOnly</Name>
    <Type>0</Type>
    <TargetLayer>0BE1808FCCA5B61BC690EAC74EADDEA5</TargetLayer>
    <Pass>
      <Name>Sphere Light LTC</Name>
      <Code>…HLSL…</Code>
      <Minifiable>1</Minifiable>
      <Parameter>
        <GUID>91621A5DAB14AC150346786EB1D734E2</GUID>
        <Name>Blending</Name>
        <Scope>0</Scope>
        <Type>15</Type>
        <Value>EQAAAAAAAAAAAAAAAAAAAA==</Value>
        <DefaultValue>AAAAAAAAAAAAAAAAAAAAAA==</DefaultValue>
      </Parameter>
      <Parameter>
        <GUID>890A856DECE7B2FF1E5E0CE4BDA50E04</GUID>
        <Name>Albedo and Metalness</Name>
        <Scope>0</Scope>
        <Type>7</Type>
        <TextureGUID>A277553146FC64CF028CD78429CB0C26</TextureGUID>
      </Parameter>
      <Parameter>
        <GUID>4B5A77CE070CF4BB175AFB13A80F7F7F</GUID>
        <Name>Depth</Name>
        <Scope>0</Scope>
        <Type>25</Type>
      </Parameter>
      <Parameter>
        <GUID>3BC8043DEF2941B6DDC5262052BC8BAB</GUID>
        <Name>red</Name>
        <Scope>2</Scope>
        <Type>0</Type>
        <DefaultValue>AACAPwAAAAAAAAAAAA…</DefaultValue>
      </Parameter>
    </Pass>
  </rendertechnique>
```

`<material>` is only a name + an ordered list of technique GUIDs (`Material_Tool.cpp:1101-1112`)
**[VERIFIED]**:

```xml
  <material>
    <GUID>A84A65F1626DC961D09A5F4D0677DD08</GUID>
    <Name>Hide Object</Name></material>
```

or with techniques:

```xml
  <material>
    <GUID>…</GUID>
    <Name>…</Name>
    <Tech>GUID</Tech>
    <Tech>GUID</Tech>
  </material>
```

### 4.11 Subscenes (nested timelines / nested scenes)

Two distinct nesting mechanisms exist; do not confuse them.

**(a) SubScene objects** — `Object Type="4"` with a `<clipdata>` carrying `<subscenetarget>GUID</subscenetarget>`.
Runtime: `CphxObject_SubScene::CreateRenderDataInstances` (`Scene.cpp:338-366`) **[VERIFIED]**:

```cpp
void CphxObject_SubScene::CreateRenderDataInstances( int Clip, const D3DXMATRIX &m, CphxScene *RootScene, void *SubSceneData )
{
  D3DXMATRIX mtx = m;
  D3DXMATRIX prs;
  D3DXMatrixTransformation( &prs, NULL, NULL, (D3DXVECTOR3*)&SplineResults[ Spline_Scale_x ], NULL, &RotationResult, (D3DXVECTOR3*)&SplineResults[ Spline_Position_x ] );

#ifndef PHX_MINIMAL_BUILD
  int clipId = max( 0, min( SubSceneTarget->ClipCount - 1, (int)SplineResults[ Spline_SubScene_Clip ] ) );
#endif

  int repeatCount = max( 1, (int)SplineResults[ Spline_SubScene_RepeatCount ] );
  float timeOffset = SplineResults[ Spline_SubScene_RepeatTimeOffset ];

  for ( int x = 0; x < repeatCount; x++ )
  {
    SubSceneTarget->UpdateSceneGraph(
#ifndef PHX_MINIMAL_BUILD
      clipId,
#else
      (int)SplineResults[ Spline_SubScene_Clip ],
#endif
      fabs( fmod( SplineResults[ Spline_SubScene_Time ] + timeOffset * x, 1.0f ) ), mtx, RootScene, SubSceneData ? SubSceneData : ToolData );

    D3DXMatrixMultiply( &mtx, &prs, &mtx );
  }

  RootScene->CollectLights( SubSceneTarget );
}
```

Semantics: the subscene is traversed **once per repeat**, each instance with local time
`frac(SubScene_Time + repeatTimeOffset*x)` and a world matrix accumulated by pre-multiplying the
sub-scene object's own PRS each iteration (`mtx = prs * mtx`). The sub-scene's clip is selected by
`Spline_SubScene_Clip` (an index in the release build, clamped in the tool). Lights of the subscene
are merged into the root scene's light list. Sub-scene objects also go through the **same** clip
evaluation as the parent (`Clip` is passed down unchanged in `TraverseSceneGraph`), but the
`UpdateSceneGraph(..., RootScene, ...)` overload re-enters at the subscene's roots with the
subscene's own clip id.

**(b) `EVENT_RENDERDEMO` / timeline recursion** — `CphxEvent_RenderDemo` is a stub
(`Timeline.cpp:20-26`):

```cpp
void CphxEvent_RenderDemo::Render( float t, float prevt, float aspect, bool subroutine )
{
  if ( subroutine ) return; //no recursion thank you
  //Timeline->Render(lerp(_start,_end,t),true);
}
```

and `CphxTimeline::Render(..., subroutine=true)` exists (clears and present are skipped for
`subroutine`), and `EVENT_RENDERSCENE` renders one clip of one scene. So the *timeline* nesting is
**not implemented** in the shipped code; sequences are expressed as multiple render-scene events in
the single timeline. **[VERIFIED]** — the only recursion-safe path is the `subroutine` flag, which
currently has no caller except the tool's `CphxTimeline_Tool::Render(Frame, RenderAsSubroutine)`.
**[UNCERTAIN]** whether an older build used it.


---

## 5. SCENE GRAPH

### 5.1 Object model

`apEx/Phoenix/Scene.h:139-197` **[VERIFIED]**:

```cpp
class CphxObject
{

public:

  class CphxScene *Scene;
  class CphxScene *SubSceneTarget;

  CphxObject *Parent;
  int ChildCount;
  CphxObject **Children;

  //constant data, spline results are stored here
  float SplineResults[ Spline_Count ];
  D3DXQUATERNION RotationResult;
  D3DXVECTOR3 TargetDirection; //calculated from target

  PHXOBJECT ObjectType;

  //clip data
  CphxObjectClip **Clips;

  D3DXVECTOR3 WorldPosition;
  ...
  char camCenterX; // also used for logic object type
  char camCenterY; // also used for logic object data
  CphxRenderTarget* cameraCubeMapTarget;

  //data for different object types
  CphxObject *Target;
  int TargetID; //minimal import data!
  int minimportParentID;

  D3DXMATRIX currMatrix;
  D3DXMATRIX prevMatrix;
  D3DXMATRIX inverse; // for particle affector stuff

  virtual void CreateRenderDataInstances( int Clip, const D3DXMATRIX &m, CphxScene *RootScene, void *SubSceneData ) {};
  void TraverseSceneGraph( int Clip, float t, D3DXMATRIX CurrentMatrix, CphxScene *RootScene, void *SubSceneData );
  void CalculateAnimation( int Clip, float t );

  unsigned char RandSeed;
  unsigned char TurbulenceFrequency;

  void *ToolData;
  D3DXMATRIX GetWorldMatrix();
  ...
};
```

`SplineResults[Spline_Count]` (size 57, `Spline_Count` in `Scene.h:91`) is the object's **entire
per-frame animation state**, keyed by `PHXSPLINETYPE`. Nothing else caches animation.

### 5.2 `PHXSPLINETYPE` and `PHXOBJECT`

`apEx/Phoenix/Scene.h:17-111` **[VERIFIED]** — reproduced in full because the numeric values are the
wire/XML format:

```cpp
enum PHXSPLINETYPE //if you add one add it to the SplineNames[] and SplineColors[] arrays in the tool as well
{
  Spline_MaterialParam = 0,
  Spline_Scale_x = 1,
  Spline_Scale_y = 2,
  Spline_Scale_z = 3,
  Spline_Rotation = 4,

  Spline_SubScene_Clip = 5,
  Spline_SubScene_Time = 6,

  Padding0 = 7,

  Spline_Position_x = 8,
  Spline_Position_y = 9,
  Spline_Position_z = 10,
  Spline_Position_w = 11,

  Spline_Light_AmbientR = 12,
  Spline_Light_AmbientG = 13,
  Spline_Light_AmbientB = 14,
  Padding1 = 15,

  Spline_Light_DiffuseR = 16,
  Spline_Light_DiffuseG = 17,
  Spline_Light_DiffuseB = 18,
  Padding2 = 19,

  Spline_Light_SpecularR = 20,
  Spline_Light_SpecularG = 21,
  Spline_Light_SpecularB = 22,
  Padding3 = 23,

  Spot_Direction_X = 24,
  Spot_Direction_Y = 25,
  Spot_Direction_Z = 26,
  Padding4 = 27,

  Spline_Light_Exponent = 28,
  Spline_Light_Cutoff = 29,
  Spline_Light_Attenuation_Linear = 30,
  Spline_Light_Attenuation_Quadratic = 31,
  Padding7 = 32,

  Spline_Camera_FOV = 33,
  Spline_Camera_Roll = 34,
  Padding8 = 35,
  Padding9 = 36,

  Spline_Particle_Offset_x = 37,
  Spline_Particle_Offset_y = 38,
  Spline_Particle_Offset_z = 39,
  Spline_Particle_EmissionPerSecond = 40,
  Spline_Particle_EmissionTrigger = 41,
  Spline_Particle_EmissionVelocity = 42,
  Spline_Particle_Life = 43,
  Spline_Particle_EmissionRotation = 44,
  Spline_Particle_EmissionVelocityChaos = 45,
  Spline_Particle_EmissionRotationChaos = 46,
  Spline_Particle_LifeChaos = 47,

  Spline_Light_OrthoX = 48,
  Spline_Light_OrthoY = 49,

  Spline_AffectorPower = 50,

  Spline_Particle_Scale = 51,
  Spline_Particle_ScaleChaos = 52,
  Spline_Particle_Stretch_X = 53,
  Spline_Particle_Stretch_Y = 54,

  Spline_SubScene_RepeatCount = 55,
  Spline_SubScene_RepeatTimeOffset = 56,

  Spline_Count, //don't remove this
};
```

Layout invariant worth exploiting: `SplineResults[8..31]` **is** a `LIGHTDATA`
(`Position, Ambient, Diffuse, Specular, SpotDirection, SpotData` as six float4s) — that is why
`CollectLights` can `memcpy` from `&SplineResults[Spline_Position_x]` (next section).

```cpp
enum PHXOBJECT
{
  Object_Model = 0,
  Object_Light,
  Object_CamEye,
  Object_Dummy,
  Object_SubScene,
  Object_ParticleEmitter,
  Object_ParticleEmitterCPU,
  Object_ParticleDrag,
  Object_ParticleGravity,
  Object_ParticleTurbulence,
  Object_ParticleVortex,
  Object_LogicObject,

  Object_Count,
};
```

### 5.3 Spline evaluation

`apEx/Phoenix/phxSpline.cpp:33-101` **[VERIFIED]** — the whole interpolation core:

```cpp
void CphxSpline::CalculateValue( float t )
{
  //FixFloatingPoint();
  if ( !KeyCount )
    return;

  if ( KeyCount == 1 || ( !Loop && t <= Keys[ 0 ]->GetTime() ) )
  {
    Keys[ 0 ]->GetValue( Value );
    POSTPROCESS;
    return;
  }

  if ( !Loop && t >= Keys[ KeyCount - 1 ]->GetTime() )
  {
    Keys[ KeyCount - 1 ]->GetValue( Value );
    POSTPROCESS;
    return;
  }

  //at this point we have at least two keys and t is between valid key values

  int pos = -1;
  while ( pos < KeyCount - 1 && Keys[ ( pos + 1 ) % KeyCount ]->GetTime() <= t ) pos++;
  pos += KeyCount - 1;

  CphxSplineKey *NeededKeys[ 4 ];
  for ( int x = 0; x < 4; x++ )	NeededKeys[ x ] = Keys[ ( pos + x ) % KeyCount ];

  //t is between neededkeys[1] and neededkeys[2] - if the difference in their time value is negative we have looped so we need to have fract(1+diff)

  float distkeys;//=KeyDistance(NeededKeys[1],NeededKeys[2],Loop);
  distkeys = NeededKeys[ 2 ]->GetTime() - NeededKeys[ 1 ]->GetTime();
  if ( Loop ) distkeys = 1 + distkeys - (int)( 1 + distkeys );

  //same fix for the t value between keys if we looped

  float partialt = t - NeededKeys[ 1 ]->GetTime() + 1; //don't change the order of the substraction here as it introduces float rounding errors in the next line (t-NeededKeys[1]->GetTime() sometimes don't align perfectly then)
  partialt = ( partialt - (int)( partialt ) ) / distkeys;

  switch ( Interpolation )
  {
  case INTERPOLATION_CONSTANT:
    NeededKeys[ 1 ]->GetValue( Value );
    break;
  case INTERPOLATION_LINEAR:
    Lerp( NeededKeys[ 1 ], NeededKeys[ 2 ], partialt );
    break;
  case INTERPOLATION_CUBIC:
    QuadraticInterpolation( NeededKeys[ 0 ], NeededKeys[ 1 ], NeededKeys[ 2 ], NeededKeys[ 3 ], partialt );
    break;
  case INTERPOLATION_BEZIER:
    ( ( CphxSpline_float16* )this )->BezierInterpolation( NeededKeys[ 0 ], NeededKeys[ 1 ], NeededKeys[ 2 ], NeededKeys[ 3 ], partialt );
    break;
  }

postproc:; //OH PLEASE GOD FORGIVE ME, BUT IT'S SMALLER THIS WAY...
  PostProcess( t );
}
```

* `POSTPROCESS` is `goto postproc` (`phxSpline.cpp:31`) — the waveform stage always runs, including
  on the clamp paths.
* Key wrap: `Keys[(pos+x) % KeyCount]`, so **looping splines interpolate across the wrap** using the
  first keys as the "next" keys.
* Interpolators: `CphxSpline_float16::Lerp` = scalar `lerp`;
  `QuadraticInterpolation` = **Catmull-Rom** `catmullrom(a,b,c,d,t)`;
  `BezierInterpolation` = solve the time parameter with `getbeziert(...)` then `bezier(a, a+c1,
  b-c0, b, t)` (`phxSpline.cpp:104-137`).
* Quaternions: `CphxSpline_Quaternion16::Lerp` = `D3DXQuaternionSlerp`;
  `QuadraticInterpolation` = `D3DXQuaternionSquadSetup` + `D3DXQuaternionSquad`
  (`phxSpline.cpp:234-263`).
* `CphxSplineKey::GetTime()` = `SPLINEPOSITIONCALC(t)` = `t ? (t+1)/256 : 0`.

Waveform post-process (`phxSpline.cpp:143-229`) **[VERIFIED]**:

```cpp
void CphxSpline_float16::PostProcess( float t )
{
  if ( Waveform == WAVEFORM_NONE )
    return;

  float wf = 0;

  float ph = t*WaveformFrequency;

  float s = sin( ph*pi*2.0f );

  switch ( Waveform )
  {
  case WAVEFORM_SIN:      wf = s; break;
  case WAVEFORM_SQUARE:   if ( s == 0 ) wf = 1.0; else wf = s / fabs( s ); break;
  case WAVEFORM_SAWTOOTH: { float f = fmodf( ph, 2.0f ); f = ( f > 1.0f ) ? 2 - f : f; wf = ( f - 0.5f ) * 2; break; }
  case WAVEFORM_TRIANGLE: wf = ( fmodf( ph, 1 ) - 0.5f ) * 2; break;
  case WAVEFORM_NOISE:    { … 3-stage box-blurred 8192-entry table, seeded by srand(RandSeed) … }
  }

  wf *= WaveformAmplitude;

  if ( MultiplicativeWaveform )
    Value[ 0 ] *= wf;
  else
    Value[ 0 ] += wf;
}
```

⚠ The waveform output is applied **only to `Value[0]`** (scalar), and only for
`CphxSpline_float16` (the quaternion class has no `PostProcess`). The noise table is
`WAVEFORMSPLINENOISEBUFFERSIZE = 8192` entries produced with `rand()` after
`srand(RandSeed)` — **libc-`rand()`-exact reproduction is required** for byte-identical noise.

### 5.4 `CphxScene::UpdateSceneGraph` — both overloads

`apEx/Phoenix/Scene.cpp:51-97` **[VERIFIED]**:

```cpp
void CphxScene::UpdateSceneGraph( int Clip, float t )
{
  D3DXMATRIX Root;
  D3DXMatrixIdentity( &Root );
  for ( int x = 0; x < LayerCount; x++ )
    RenderLayers[ x ]->RenderInstances.FreeArray();

  LightCount = 0;
  UpdateSceneGraph( Clip, t, Root, this, NULL );

  //calculate target directions
  for ( int x = 0; x < ObjectCount; x++ )
  {
    CphxObject *o = Objects[ x ];
    if ( o->Target )
    {
      o->TargetDirection = o->Target->WorldPosition - o->WorldPosition;
      D3DXVec3Normalize( &o->TargetDirection, &o->TargetDirection );
      o->SplineResults[ Spot_Direction_X ] = o->TargetDirection.x;
      o->SplineResults[ Spot_Direction_Y ] = o->TargetDirection.y;
      o->SplineResults[ Spot_Direction_Z ] = o->TargetDirection.z;
    }
  }

  CollectLights( this );

  for ( int x = 0; x < LayerCount; x++ )
    SortRenderLayer( RenderLayers[ x ]->RenderInstances.Array, 0, RenderLayers[ x ]->RenderInstances.ItemCount - 1 );

  //calculate inverse matrices for particle affectors
  for ( int x = 0; x < ObjectCount; x++ )
    if ( Objects[ x ]->ObjectType == Object_ParticleGravity ||
         Objects[ x ]->ObjectType == Object_ParticleDrag ||
         Objects[ x ]->ObjectType == Object_ParticleTurbulence ||
         Objects[ x ]->ObjectType == Object_ParticleVortex )
    {
      D3DXMATRIX m = Objects[ x ]->GetWorldMatrix();
      D3DXMatrixInverse( &Objects[ x ]->inverse, NULL, &m );
    }
}

void CphxScene::UpdateSceneGraph( int Clip, float t, D3DXMATRIX Root, CphxScene *RootScene, void *SubSceneData )
{
  for ( int x = 0; x < ObjectCount; x++ )
    if ( Objects[ x ]->Parent == NULL )
      Objects[ x ]->TraverseSceneGraph( Clip, t, Root, RootScene, SubSceneData );
}
```

Exact ordering a port must preserve:

1. free all render-instance arrays of **all** layers;
2. reset the light list;
3. traverse the scene roots (only objects with `Parent == nullptr`) with an identity root matrix;
4. **after** traversal, for each object with a `Target`, compute `TargetDirection =
   normalize(target.WorldPosition - WorldPosition)` and **write it into the
   `Spot_Direction_X/Y/Z` spline slots** — i.e. a "target" silently overrides any authored direction
   splines. This is how cameras and lights aim at objects;
5. `CollectLights(this)`;
6. sort every layer by `RenderPriority` (descending);
7. compute inverse world matrices for particle affectors (`inverse` is used by
   `ParticleInside`/`GetForce`).

Note the target-direction pass uses `WorldPosition`, which for a parented object is computed
relative to the **parent's** matrix (see `TraverseSceneGraph` step 3 below) — i.e. positions become
*world* positions only because the recursion multiplies parent transforms into the child's local
offset.

### 5.5 `CphxObject::TraverseSceneGraph` and `CalculateAnimation`

`apEx/Phoenix/Scene.cpp:229-315` **[VERIFIED]**:

```cpp
void CphxObject::TraverseSceneGraph( int Clip, float t, D3DXMATRIX CurrentMatrix, CphxScene *RootScene, void *SubSceneData )
{
  D3DXMATRIX m = CurrentMatrix;
  CalculateAnimation( Clip, t );

  D3DXMATRIX prs;
  D3DXMatrixTransformation( &prs, NULL, NULL, (D3DXVECTOR3*)&SplineResults[ Spline_Scale_x ], NULL, &RotationResult, (D3DXVECTOR3*)&SplineResults[ Spline_Position_x ] );
  D3DXMatrixMultiply( &m, &prs, &CurrentMatrix );

  prevMatrix = currMatrix;
  currMatrix = m;

  //move calculated position to world space
  D3DXVECTOR4 v;
  D3DXVec3Transform( &v, (D3DXVECTOR3*)&SplineResults[ Spline_Position_x ], &CurrentMatrix );
  WorldPosition = *(D3DXVECTOR3*)&v;

  //create render data instances for the object based on the current clip and position here, also handles subscenes correctly
  CreateRenderDataInstances( Clip, m, RootScene, SubSceneData ? SubSceneData : ToolData );

  //go down the hierarchy
  for ( int x = 0; x < ChildCount; x++ )
    Children[ x ]->TraverseSceneGraph( Clip, t, m, RootScene, SubSceneData );
}

void CphxObject::CalculateAnimation( int Clip, float t )
{
  //need to set defaults somewhere, might as well do it here :)
  SplineResults[ Spline_Scale_x ] = 1;
  SplineResults[ Spline_Scale_y ] = 1;
  SplineResults[ Spline_Scale_z ] = 1;
  SplineResults[ Spline_Light_DiffuseR ] = 1;
  SplineResults[ Spline_Light_DiffuseG ] = 1;
  SplineResults[ Spline_Light_DiffuseB ] = 1;
  SplineResults[ Spline_Light_SpecularR ] = 1;
  SplineResults[ Spline_Light_SpecularG ] = 1;
  SplineResults[ Spline_Light_SpecularB ] = 1;
  SplineResults[ Spline_Camera_FOV ] = 1;
  SplineResults[ Spline_Particle_EmissionVelocity ] = 1;
  SplineResults[ Spline_AffectorPower ] = 1;
  SplineResults[ Spline_Particle_Scale ] = 1;
  SplineResults[ Spline_Particle_Stretch_X ] = 1;
  SplineResults[ Spline_Particle_Stretch_Y ] = 1;

  SplineResults[ Spline_Particle_EmissionPerSecond ] = 25;
  SplineResults[ Spline_Particle_Life ] = 10;

  SplineResults[ Spline_Light_OrthoX ] = 1;
  SplineResults[ Spline_Light_OrthoY ] = 1;

  for ( int x = 0; x < Clips[ Clip ]->SplineCount; x++ )
  {
    CphxClipSpline *s = Clips[ Clip ]->Splines[ x ];
    s->Spline->CalculateValue( t );
    SplineResults[ s->Type ] = s->Spline->Value[ 0 ];
    if ( s->Type == Spline_Rotation ) RotationResult = s->Spline->GetQuaternion();
  }

  Clips[ Clip ]->MaterialSplines->CalculateValues( t );
  SubSceneTarget = Clips[ Clip ]->SubSceneTarget;

  if ( ObjectType == Object_ParticleTurbulence )
  {
    RandSeed = Clips[ Clip ]->RandSeed;
    TurbulenceFrequency = Clips[ Clip ]->TurbulenceFrequency;
  }
}
```

Critical details:

* **Only `Value[0]` is copied** into `SplineResults[type]`, except `Spline_Rotation`, which reads
  the full quaternion `Value[0..3]` via `GetQuaternion()`.
* `RotationResult` is **not reset** between clips: an object with no rotation spline in the current
  clip keeps the previous clip's rotation. Same for all other slots not present in the clip (the
  defaults above only cover the listed indices). Anything not defaulted and not present in the clip
  (e.g. `Position_*`, light ambient) is **whatever the previous frame left there**.
  **[VERIFIED] but subtle** — a first-frame zero-init assumption is required.
* `SubSceneTarget` is re-derived from the *clip* every traversal.
* Splines are **not cached per frame**; if an object appears in two active events, its splines are
  evaluated twice.

Matrix convention (`Scene.cpp:249-259`): `m = prs * CurrentMatrix` — the object's local matrix is
**pre-multiplied** onto the parent's accumulated matrix, i.e. row-vector convention
(`v' = v * M`), consistent with D3DX. `WorldPosition` uses the **parent's** matrix
(`D3DXVec3Transform(&v, &localPos, &CurrentMatrix)`), not `m`, which is correct because the local
position is already baked into `m` as the translation row of `prs`.

`CphxObject::GetWorldMatrix()` (`Scene.cpp:317-328`) **ignores parents unless
`PHX_HAS_SCENE_OBJECT_HIERARCHIES` is defined**:

```cpp
D3DXMATRIX CphxObject::GetWorldMatrix()
{
  D3DXMATRIX prs;
  D3DXMatrixTransformation( &prs, NULL, NULL, (D3DXVECTOR3*)&SplineResults[ Spline_Scale_x ], NULL, &RotationResult, (D3DXVECTOR3*)&SplineResults[ Spline_Position_x ] );

#ifdef PHX_HAS_SCENE_OBJECT_HIERARCHIES
  if ( Parent )
    D3DXMatrixMultiply( &prs, &prs, &Parent->GetWorldMatrix() );
#endif

  return prs;
}
```

It is used only for particle-affector inverse matrices and particle spawn matrices. Whether the
Clean Slate build defines `PHX_HAS_SCENE_OBJECT_HIERARCHIES` is a `PhoenixConfig.h` choice —
**[UNCERTAIN]**; it matters because `UpdateParticles` collects child matrices only under the same
macro.

### 5.6 Camera list / lights collection

`apEx/Phoenix/Scene.cpp:101-134` **[VERIFIED]**:

```cpp
void CphxScene::CollectLights( CphxScene* sceneToCollectFrom )
{
  for ( int x = 0; x < sceneToCollectFrom->ObjectCount; x++ )
  {
    if ( LightCount >= 8 )
      return;

    CphxObject* object = sceneToCollectFrom->Objects[ x ];

    if ( object->ObjectType == Object_Light )
    {
      memcpy( &Lights[ LightCount ], &object->SplineResults[ Spline_Position_x ], sizeof( LIGHTDATA ) );
      if ( object->SplineResults[ Spline_Position_w ] != 0 )
      {
        Lights[ LightCount ].Position.x = object->WorldPosition.x;
        Lights[ LightCount ].Position.y = object->WorldPosition.y;
        Lights[ LightCount ].Position.z = object->WorldPosition.z;
      }
      else
      {
        Lights[ LightCount ].SpotDirection.x = object->WorldPosition.x;
        Lights[ LightCount ].SpotDirection.y = object->WorldPosition.y;
        Lights[ LightCount ].SpotDirection.z = object->WorldPosition.z;
        Lights[ LightCount ].Ambient.w = object->SplineResults[ Spline_Light_OrthoX ];
        Lights[ LightCount ].Diffuse.w = object->SplineResults[ Spline_Light_OrthoY ];
      }
      LightCount++;
    }
  }
}
```

* `LIGHTDATA` (`Scene.h:131-137`):
  ```cpp
  struct LIGHTDATA
  {
    D3DXVECTOR4 Position;
    D3DXVECTOR4 Ambient, Diffuse, Specular;
    D3DXVECTOR4 SpotDirection;
    D3DXVECTOR4 SpotData; //exponent, cutoff, linear and quadratic attenuations
  };
  ```
* `Spline_Position_w != 0` ⇒ **point light** (world position used, matrix ignored). Else the light
  is spot/directional: its **world position is written into the direction field** (so a directional
  light's "position" is really its direction, which the target-direction pass may have derived from
  an object target), and `Spline_Light_OrthoX/Y` go into `Ambient.w` / `Diffuse.w` (shadow-map
  ortho extents).
* Hard limit **8 lights** (`MAX_LIGHT_COUNT`, `phxEngine.h:13`); iteration order = object array order
  and **stops entirely at the cap** (not "keeps looking").
* Subscenes merge their lights via `RootScene->CollectLights(SubSceneTarget)` at the end of
  `CphxObject_SubScene::CreateRenderDataInstances`, sharing the same 8-light budget.
* The camera is **not** in this list. Camera objects are selected per event (`<camera>` GUID →
  object index) or globally by `EVENT_CAMERAOVERRIDE`; the tool picks one via
  `CWBSceneDisplay::FindViableCamera` (`SceneView.cpp`).

### 5.7 View / projection matrices, FOV, roll, off-centre

`apEx/Phoenix/Timeline.cpp:152-229` **[VERIFIED]** — `CphxEvent_RenderScene::Render` in full:

```cpp
void CphxEvent_RenderScene::Render( float t, float prevt, float aspect, bool subroutine )
{
  if ( !Scene || ( !Camera && !cameraOverride ) ) return;

  CphxObject* actualCamera = cameraOverride ? cameraOverride : Camera;

  for ( int x = 0; x < 2; x++ )
  {
    phxPrevFrameViewMatrix = phxViewMatrix;
    phxPrevFrameProjectionMatrix = phxProjectionMatrix;

    Scene->UpdateSceneGraph( Clip, x ? t : prevt );
    actualCamera->WorldPosition += EyeOffset;
    D3DXVECTOR3 eye = actualCamera->WorldPosition;
    D3DXVECTOR3 dir = *( (D3DXVECTOR3*)&actualCamera->SplineResults[ Spot_Direction_X ] );
    phxCameraPos = D3DXVECTOR4( eye.x, eye.y, eye.z, 1 );

    //roll camera
    D3DXMATRIX RollMat;
    D3DXVECTOR4 rolledup;
    D3DXVec3Transform( &rolledup, (D3DXVECTOR3*)up, D3DXMatrixRotationAxis( &RollMat, &dir, actualCamera->SplineResults[ Spline_Camera_Roll ] * 3.14159265359f*2.0f ) );

    D3DXMatrixLookAtRH( &phxViewMatrix, &( eye ), &( eye + dir + TargetOffset ), (D3DXVECTOR3*)&rolledup );

    // off-center calculation

    float fovYper2 = ( actualCamera->SplineResults[ Spline_Camera_FOV ] * 3.14159265359f / 4.0f ) / 2.0f;
    float zn = 0.01f;
    float cotFov = cos( fovYper2 ) / sin( fovYper2 );
    float t = zn / cotFov;
    float r = zn * aspect / cotFov;
    float xOffset = actualCamera->camCenterX / 127.0f * r;
    float yOffset = actualCamera->camCenterY / 127.0f * t;

    D3DXMatrixPerspectiveOffCenterRH( &phxProjectionMatrix, -r + xOffset, r + xOffset, -t + yOffset, t + yOffset, zn, 2000.0f );

    //D3DXMatrixPerspectiveFovRH( &phxProjectionMatrix, Camera->SplineResults[ Spline_Camera_FOV ] * 3.14159265359f / 4.0f, aspect, 0.01f, 2000.0f );

#ifdef PHX_OBJ_EMITTERCPU
    {
      for ( int x = 0; x < Scene->ObjectCount; x++ )
        if ( Scene->Objects[ x ]->ObjectType == Object_ParticleEmitterCPU )
        {
          CphxObject_ParticleEmitter_CPU *p = (CphxObject_ParticleEmitter_CPU*)( Scene->Objects[ x ] );
            p->UpdateParticles( 0, true ); //sort particles
        }
    }
#endif
  }

  Scene->Render( ClearColor, ClearZ, 0 );
}
```

Exact semantics:

* **Two full scene-graph traversals**: first at `prevt` (previous frame's local time) and then at
  `t`. This exists so that `prevMatrix`/`currMatrix` on every object (and hence particle
  interpolation) are correct, and so `phxPrevFrameViewMatrix/Projection` describe the previous
  frame. The matrices left in `phxViewMatrix`/`phxProjectionMatrix` at the end correspond to `t`.
* Camera world position is taken **after** the traversal and `+= EyeOffset` (camera shake).
  Note `+=` on the object's `WorldPosition` mutates the object — it is overwritten again by the
  next traversal, so it is idempotent within a frame.
* **View direction** comes from the object's `Spot_Direction_X/Y/Z` spline slots — normally filled
  by the target-direction pass (§5.4). A camera with no target and no direction splines looks along
  (0,0,0).
* `LookAtRH(eye, eye + dir + TargetOffset, rolledup)` — `TargetOffset` (shake) shifts the *look-at
  point*, i.e. rotates the camera, while `EyeOffset` translates it.
* **Roll**: the up vector (0,1,0) is rotated about the view direction by
  `Spline_Camera_Roll * 2π` — so the spline unit is **turns**, not radians or degrees.
* **FOV**: vertical FOV = `Spline_Camera_FOV * 45°` (because `fovYper2` is half of
  `FOV*π/4`). Default spline value is 1 ⇒ **45° vertical FOV**.
* **Off-centre projection** from `camCenterX/camCenterY` ∈ [-127, 127] stored in a signed char:
  `xOffset = camCenterX/127 * r`, `yOffset = camCenterY/127 * t`. This is used for asymmetric
  frusta (tilt-shift / multi-monitor / cubemap faces).
* Near plane **0.01**, far plane **2000.0**, right-handed look-at, RH perspective — i.e. the engine
  is **RH / [-1,1] depth** → a Vulkan port must flip Y and remap Z to [0,1] (or use
  `VK_KHR_maintenance1` negative viewport).
* Particle emitters are force-sorted (`UpdateParticles(0, true)`) **twice per scene event** (once
  per traversal iteration) — the sort uses `phxViewMatrix`, which for the first iteration is still
  the previous frame's matrix. **[VERIFIED — a real ordering quirk]**.
* The scene renders with `cubeResolution = 0` always (the cube-map branch is commented out).

### 5.8 Camera shake and camera override

**Shake** (`apEx/Phoenix/Timeline.cpp:272-313`) **[VERIFIED]**:

```cpp
#ifdef PHX_EVENT_CAMSHAKE
static long aholdrand = 1L;

int __cdecl arand()
{
  return( ( ( aholdrand = aholdrand * 214013L + 2531011L ) >> 16 ) & 0x7fff );
}

void CphxEvent_CameraShake::Render( float t, float prevt, float aspect, bool subroutine )
{
  float dist = 1 / (float)ShakesPerSec;
  float currTime = CurrentFrame / (float)TimelineFramerate;
  float d1 = fmod( currTime, dist );
  float d = d1 / dist;
  int t1 = (int)( ( currTime - d1 )*TimelineFramerate );
  int t2 = (int)( ( currTime - d1 + dist )*TimelineFramerate );

  aholdrand = t1;

  D3DXVECTOR3 eo1, eo2, to1, to2;

  for ( int x = 0; x < 3; x++ )
  {
    eo1[ x ] = arand() / (float)RAND_MAX - 0.5f;
    to1[ x ] = arand() / (float)RAND_MAX - 0.5f;
  }

  aholdrand = t2;

  for ( int x = 0; x < 3; x++ )
  {
    eo2[ x ] = arand() / (float)RAND_MAX - 0.5f;
    to2[ x ] = arand() / (float)RAND_MAX - 0.5f;
  }

  EyeIntensity->CalculateValue( t );
  TargetIntensity->CalculateValue( t );

  EyeOffset = ( ( eo2 - eo1 )*d + eo1 )*EyeIntensity->Value[ 0 ];
  TargetOffset = ( ( to2 - to1 )*d + to1 )*TargetIntensity->Value[ 0 ];
}
#endif
```

* Shake is a **global side effect** consumed by the *next* `EVENT_RENDERSCENE` in the same frame; it
  is reset to zero at the top of `CphxTimeline::Render`. Since events are executed in sorted order,
  a shake event placed after a render-scene event affects it only on the following frame — actually
  it does not affect it at all in that frame. Order matters. **[VERIFIED]**
* The LCG `arand()` is deterministic and re-seeded from the **integer frame numbers** `t1`, `t2` —
  which means the shake is a pure function of the absolute frame number and `ShakesPerSec`. A port
  reproduces it exactly as long as `CurrentFrame` matches.
* `EyeOffset` is *added to the camera's world position*, `TargetOffset` to the look-at point
  (§5.7).

**Override** (`apEx/Phoenix/Timeline.cpp:142-149`) **[VERIFIED]**:

```cpp
void CphxEvent_CameraOverride::Render( float t, float prevt, float aspect, bool subroutine )
{
  if ( !Scene || ( !Camera ) ) return;

  Scene->UpdateSceneGraph( Clip, t );
  cameraOverride = Camera;
}
```

* `cameraOverride` is a **global** file-scope pointer (`Timeline.cpp:11`) cleared once per frame and
  consumed by every later `EVENT_RENDERSCENE`, **regardless of which scene those events render**.
  A camera object belongs to a scene, so overriding across scenes uses another scene's object —
  the engine does not validate this. Clean Slate uses one type-6 event at Pass 0 spanning frames
  0..33620, so the override is always set before any render-scene event. **[VERIFIED]**
* It also runs a full `UpdateSceneGraph` at `t` for its own clip (extra traversal, extra render
  instances — these are added to the layer lists and then **discarded** by the next
  `UpdateSceneGraph` call in the following scene event; but if the override's scene is the same
  scene, the render instances of that scene's layers persist only until the next traversal).
  **[VERIFIED]** — i.e. `EVENT_CAMERAOVERRIDE` performs one wasteful full scene evaluation.

### 5.9 Particle emitters and affectors

`CphxObject_ParticleEmitter_CPU` (`Scene.h:251-305`) is a CPU integrator; there is no GPU particle
path that the runtime uses (the D3D11-compute emitter is fully commented out).

**Fixed-step integrator** — `UpdateParticles` (`Scene.cpp:463-606`) **[VERIFIED, abridged]**:

```cpp
void CphxObject_ParticleEmitter_CPU::UpdateParticles( float elapsedtime, bool updatebuffer )
{
  int particlecount = 1 << BufferSize;
  Ticks += elapsedtime * PARTICLEENGINE_FRAMERATE;

  int objectcount = 1;
  int affectorcount = 0;
  ...
  matrices[ 0 ] = currMatrix;// GetWorldMatrix();
  oldmatrices[ 0 ] = prevMatrix;// GetWorldMatrix();
  ...
  while ( Ticks >= 1 )
  {
    //age and move particles
    for ( int y = 0; y < particlecount; y++ )
    {
      if ( Aging ) Particles[ y ].LifeLeft -= 1;
      if ( Particles[ y ].LifeLeft > 0 )
      {
        for ( int x = 0; x < affectorcount; x++ )
          if ( affectors[ x ]->ParticleInside( Particles[ y ].Position ) )
            Particles[ y ].Velocity += affectors[ x ]->GetForce( &Particles[ y ] );

        Particles[ y ].Position += Particles[ y ].Velocity;
        Particles[ y ].Rotation += Particles[ y ].RotationSpeed;
      }
    }

    //spawn new particles
    if ( SplineResults[ Spline_Particle_EmissionPerSecond ] > 0 )
    {
      int cnt = 1 + (int)( ( 1 - fmod( EmissionFraction, 1.0f ) ) / ( PARTICLEENGINE_FRAMERATE / ( SplineResults[ Spline_Particle_EmissionPerSecond ] * objectcount ) ) );
      int idx = 0;
      while ( EmissionFraction < 1 )
      {
        int id = ( objIdxMod++ ) % objectcount;
        SpawnParticle( EmissionFraction - (int)EmissionFraction, matrices[ id ], oldmatrices[ id ], idx / (float)cnt );
        EmissionFraction += PARTICLEENGINE_FRAMERATE / ( SplineResults[ Spline_Particle_EmissionPerSecond ] * objectcount );
        idx++;
      }
    }

    EmissionFraction--;
    Ticks--;
  }
  ...
  //create display buffer: interpolate by the leftover Ticks fraction
  D3DXVECTOR3 v = Particles[ idx ].Position + Particles[ idx ].Velocity*Ticks;
  Data[ 0..7 ] = { v.x, v.y, v.z, 1, life, rotation + rotSpeed*Ticks, chaos, 0 };
}
```

* `PARTICLEENGINE_FRAMERATE = 25.0f` (`Scene.h:15`) — particles advance at a **fixed 25 Hz
  simulation rate**, decoupled from the demo frame rate. `elapsedtime` is in seconds; the number of
  sim steps is `floor(Ticks)`.
* Render interpolation uses the fractional leftover `Ticks` — so a port must carry the emitter's
  `Ticks`/`EmissionFraction`/`Particles[]` state across frames.
* Two distinct drivers:
  * `EVENT_PARTICLECALC` → `UpdateParticles( (tme - lasttime)/1000, false )` where
    `tme = ((EndFrame-StartFrame)*t + StartFrame)*1000/TimelineFramerate`
    (`Timeline.cpp:236-269`) — **absolute time is reconstructed from the event-local `t`**, so
    particle calc is deterministic w.r.t. the frame number as long as the first active frame is
    consistent (`OnScreenLastFrame` initialises `lastt`/`lasttime`).
  * `EVENT_RENDERSCENE` → `UpdateParticles(0, true)` purely to sort the display buffer.
* `SpawnParticle` uses the C `rand()` (`Scene.cpp:401-452`) seeded by `srand(RandSeed)` in
  `ResetParticles` (`Scene.cpp:785`). ⚠ `rand()` is called from many places; **any** extra/missing
  `rand()` call changes the whole particle look. Reproducing Clean Slate exactly requires an
  MSVC-compatible `rand()` and the identical call sequence.
* Affectors: `Drag` = `-power * v`; `Gravity` = `power/100 * normalize(pos)` (directional) or
  `power/(100) * (pos-p)/|pos-p|³`; `Vortex` = `cross(worldY, normalize(pos-p)) * power`;
  `Turbulence` = 3-octave trilinear-sampled 32³ random kernel controlled by `RandSeed` and
  `TurbulenceFrequency` (`Scene.cpp:851-957`), so turbulence also depends on `rand()`.
  `ParticleInside` transforms by the **inverse world matrix** and tests a unit cube
  (`AreaType != 0`).

### 5.10 Render-data creation per object type

| object | function | effect |
|---|---|---|
| Model | `CphxObject_Model::CreateRenderDataInstances` (`Scene.cpp:331-335`) | delegates to `CphxModel::CreateRenderDataInstances(Clips[Clip], m, RootScene, …)` |
| SubScene | `Scene.cpp:338-366` | nested traversals (see §4.11) |
| ParticleEmitterCPU | `Scene.cpp:608-724` | animated material params, spline texture upload, then either one instanced draw of the whole particle buffer or per-particle mesh/subscene emission |
| Light / CamEye / Dummy / Logic / affectors | base `CphxObject::CreateRenderDataInstances` (`Scene.h:184`) | no draw |

`CphxModel::CreateRenderDataInstances` → `CphxModelObject_Mesh::CreateRenderDataInstances`
(`Model.cpp:25-46`) **[VERIFIED]**:

```cpp
void CphxModelObject_Mesh::CreateRenderDataInstances( CphxObjectClip *Clip, const D3DXMATRIX &m, CphxScene *RootScene, void *CloneData )
{
  if ( !Material ) return;

  D3DXMatrixMultiply( &phxWorldMatrix, &GetMatrix(), &m );
  D3DXMatrixInverse( &phxITWorldMatrix, NULL, &phxWorldMatrix );
  D3DXMatrixTranspose( &phxITWorldMatrix, &phxITWorldMatrix );

  // load animated data into object material
  Clip->MaterialSplines->ApplyToParameters( this );

  //update dynamic material variables

  int passid = 0;

  for ( int x = 0; x < Material->TechCount; x++ )
    for ( int y = 0; y < Material->Techniques[ x ]->PassCount; y++ )
      Material->Techniques[ x ]->CollectAnimatedData( MaterialState[ passid++ ], y );

  //create render data instance here
  Material->CreateRenderDataInstances( this, RootScene, CloneData ? CloneData : ToolObject );
}
```

* The mesh's own baked matrix (`GetMatrix()`, 12 halves) multiplies the **accumulated scene
  matrix** `m` to give `phxWorldMatrix`; `phxITWorldMatrix` is its inverse-transpose.
  `Matrices[0/1]` of every render instance created for this mesh are these two matrices
  (`Material.cpp:157-158`).
* Material splines are applied **per scene object** (grouped by `GroupingData == this`), then the
  animated constants are collected per pass.
* Meshes are cached/instanced per model object; clones recurse with their own matrices
  (`Model.cpp:48-55`).


---

## 6. MATERIALS

### 6.1 Types and the parameter union

`apEx/Phoenix/Material.h:5-147` **[VERIFIED]**:

```cpp
enum MATERIALPARAMSCOPE
{
  PARAM_CONSTANT = 0,
  PARAM_VARIABLE = 1,
  PARAM_ANIMATED = 2,
};

enum COMPARISONFUNCTION
{
  COMPARE_LESSEQUAL = 0,
  COMPARE_NEVER = 1,
  COMPARE_LESS = 2,
  COMPARE_EQUAL = 3,
  COMPARE_GREATER = 4,
  COMPARE_NOTEQUAL = 5,
  COMPARE_GREATEREQUAL = 6,
  COMPARE_ALWAYS = 7,
};

enum MATERIALPARAMTYPE
{
  //data types added here need to be named in the MaterialTypeNames[] array
  PARAM_FLOAT = 0,
  PARAM_COLOR,
  PARAM_ZMODE,
  PARAM_ZFUNCTION,
  PARAM_FILLMODE,
  PARAM_CULLMODE,
  PARAM_RENDERPRIORITY,
  PARAM_TEXTURE0,
  PARAM_TEXTURE1,
  PARAM_TEXTURE2,
  PARAM_TEXTURE3,
  PARAM_TEXTURE4,
  PARAM_TEXTURE5,
  PARAM_TEXTURE6,
  PARAM_TEXTURE7,
  PARAM_BLENDMODE0,
  PARAM_BLENDMODE1,
  PARAM_BLENDMODE2,
  PARAM_BLENDMODE3,
  PARAM_BLENDMODE4,
  PARAM_BLENDMODE5,
  PARAM_BLENDMODE6,
  PARAM_BLENDMODE7,
  PARAM_RENDERTARGET, //only for postproc
  PARAM_PARTICLELIFEFLOAT,
  PARAM_DEPTHTEXTURE7,
  PARAM_3DTEXTURE6,
  PARAM_MESHDATA0,
  PARAM_MESHDATA1,
  PARAM_MESHDATA2,
  PARAM_MESHDATA3,
  PARAM_MESHDATA4,
  PARAM_MESHDATA5,
  PARAM_MESHDATA6,
  PARAM_MESHDATA7,
  PARAM_PARTICLELIFE,
  PARAM_LTC1,
  PARAM_LTC2,

  PARAM_COUNT, //leave this at the end!
};
```

Numeric values are the XML `<Type>` values. From the real project: `Type 0` float, `5` cull mode,
`6` render priority, `7` texture0, `8` texture1, `15` blendmode0, `23` rendertarget, `25`
depthtexture7.

```cpp
union MATERIALVALUE
{
  float Float;
  float Color[ 4 ];
  unsigned char ZMode;
  unsigned char BlendMode;
  D3D11_COMPARISON_FUNC ZFunction;
  bool Wireframe;
  D3D11_CULL_MODE CullMode;
  int RenderPriority;
  ID3D11ShaderResourceView *TextureView;
  CphxRenderTarget *RenderTarget;
};
```

`sizeof(MATERIALVALUE) == 16` **[VERIFIED]** — this is exactly why the XML `valuebase64` payloads
decode to 16 bytes. The union is type-punned: the same 16 bytes are interpreted according to
`Type`.

```cpp
struct CphxMaterialParameter
{
  MATERIALPARAMTYPE Type;
  MATERIALPARAMSCOPE Scope;
  MATERIALVALUE Value;
};

struct CphxMaterialParameterBatch //stores parameter data for a tech or a pass
{
  int ParamCount;
  CphxMaterialParameter **Parameters;
  float CollectedData[ MATERIALDATASIZE / 4 ];
  int CollectAnimatedData();
};
```

**Scope semantics** **[VERIFIED]**:

| scope | who writes `Value` | where it ends up |
|---|---|---|
| `PARAM_CONSTANT` (0) | the project loader (`ImportMaterialParamValue` for texture/float/colour/zmode) or the tool | compiled into per-pass GPU state (blend/depth/raster/shader) **and** appended to the static constant data |
| `PARAM_VARIABLE` (1) | the per-instance override (`materialparamdata` in the scene for objects, `TechData` for shadertoy events) | appended to the static constant data (or bound as a texture) |
| `PARAM_ANIMATED` (2) | **nobody at load** — driven every frame by `CphxMaterialSplineBatch` + `CollectAnimatedData` | appended to the *animated* constant data each frame |

### 6.2 Technique / pass / material objects

```cpp
struct CphxMaterialRenderPass
{
  CphxMaterialParameterBatch Parameters;

  ID3D11VertexShader *VS; //WARNING! DO NOT CHANGE THE ORDER OF THESE DUE TO A MEMCPY IN CreateRenderDataInstances()
  ID3D11PixelShader *PS;
  ID3D11GeometryShader *GS;
  ID3D11HullShader *HS;
  ID3D11DomainShader *DS;

#ifdef _DEBUG
  char* shaderText;
#endif
};

enum TECHNIQUETYPE
{
  TECH_MATERIAL = 0,
  TECH_POSTPROCESS = 1,
  TECH_SHADERTOY = 2,
  TECH_PARTICLE = 3,
};

struct CphxMaterialTechnique
{
  CphxMaterialParameterBatch Parameters;

  TECHNIQUETYPE Type;
  CphxRenderLayerDescriptor *TargetLayer;

  int PassCount;
  CphxMaterialRenderPass **RenderPasses;

  void CreateRenderDataInstances( CphxMaterialPassConstantState **MaterialState, int &passid, CphxScene *RootScene, ID3D11Buffer *VertexBuffer, ID3D11Buffer *IndexBuffer, ID3D11Buffer *WireBuffer, int VertexCount, int IndexCount, void *ToolData, bool Indexed );
  void CollectAnimatedData( CphxMaterialPassConstantState *State, int Pass );
};

struct CphxMaterial
{
  int TechCount;
  CphxMaterialTechnique **Techniques;

  int PassCount; //calculated summary pass count used by the minimalengine

  void CreateRenderDataInstances( CphxModelObject_Mesh *Model, CphxScene *RootScene, void *ToolData );
  void CreateRenderDataInstances( CphxMaterialPassConstantState **State, CphxScene *RootScene, ID3D11Buffer *VertexBuffer, int VertexCount );
};
```

`apEx/Phoenix/Model.h:42-60` — the per-instance GPU state machine **[VERIFIED]**:

```cpp
class CphxMaterialPassConstantState
{
public:

  ID3D11BlendState *BlendState; //WARNING! DO NOT CHANGE THE ORDER OF THESE DUE TO A MEMCPY IN CreateRenderDataInstances() in Material.cpp!
  ID3D11RasterizerState *RasterizerState;
  ID3D11DepthStencilState *DepthStencilState;
  ID3D11ShaderResourceView *Textures[ 8 ];

  bool Wireframe;
  unsigned char RenderPriority;
  CphxRenderTarget *RenderTarget;
  float ConstantData[ MATERIALDATASIZE / 4 ];
  float AnimatedData[ MATERIALDATASIZE / 4 ];
  int ConstantDataSize;
  int AnimatedDataSize;

  ID3D11ShaderResourceView *TextureBackup[ 8 ]; //used by the minimalengine to store original texture references when the internal temp buffer is copied to
};
```

Note `RenderPriority` is an `unsigned char` here but an `int` in `CphxRenderDataInstance` — values
>255 wrap. `ConstantDataSize`/`AnimatedDataSize` are **byte sizes** (see the `*= 4` below).

### 6.3 How a pass binds shaders, constants, textures and samplers

**Shaders and states** come from the per-instance `CphxRenderDataInstance` (filled by `memcpy` from
`CphxMaterialPassConstantState` at instance creation, `Material.cpp:117-166`) and are bound in
`CphxRenderDataInstance::Render` (§3.2):

```
IASetInputLayout      (mesh vs particle layout)
IASetVertexBuffers(0, VertexBuffer, stride=84)
IASetPrimitiveTopology(Wireframe ? LINELIST : TRIANGLELIST)
VSSetShader / GSSetShader / HSSetShader / DSSetShader / PSSetShader
RSSetState(RasterizerState) / OMSetBlendState(BlendState, 0xffffffff) / OMSetDepthStencilState(DepthStencilState, 0)
Map(ObjectMatrixBuffer) -> memcpy(Matrices[2]) + memcpy(MaterialData, MATERIALDATASIZE)
VSSetShaderResources(0,8,Textures) / GSSetShaderResources / PSSetShaderResources
IASetIndexBuffer(Wireframe ? WireBuffer : IndexBuffer, R32_UINT)
DrawIndexed(...)  or  Draw(...) for non-indexed particles
```

Constant buffers are bound once per layer (scene constants) and per draw (object constants)
(`Scene.cpp:184-190`):

```cpp
    for ( int y = 0; y < 2; y++ )
    {
      ID3D11Buffer *Buffer = y ? ObjectMatrixBuffer : SceneDataBuffer;
      phxContext->VSSetConstantBuffers( y, 1, &Buffer );
      phxContext->GSSetConstantBuffers( y, 1, &Buffer );
      phxContext->PSSetConstantBuffers( y, 1, &Buffer );
    }
```

⇒ **`b0` = scene constants, `b1` = per-object constants**, in VS/GS/PS alike. Layouts in §2.4.

**Samplers** are global and bound only twice: at init for VS
(`phxEngine.cpp:212-217`) and per scene layer / per texgen pass / per shadertoy pass for PS
(`Texgen.cpp:92-97`):

```cpp
static D3D11_SAMPLER_DESC smpData = { D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISON_NEVER, 0, 0, 0, 0, -D3D11_FLOAT32_MAX, D3D11_FLOAT32_MAX };
...
  phxContext->CreateSamplerState( &smpData, &TexgenSampler );
  smpData.AddressU = smpData.AddressV = smpData.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  phxDev->CreateSamplerState( &smpData, &TexgenSampler_NoWrap );
  smpData.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
  smpData.ComparisonFunc = D3D11_COMPARISON_LESS;
  phxDev->CreateSamplerState( &smpData, &TexgenSampler_ShadowCompare );
```

| slot | filter | address | comparison |
|---|---|---|---|
| s0 | anisotropic, maxAniso 16 | wrap | never |
| s1 | anisotropic, maxAniso 16 | clamp | never |
| s2 | comparison min/mag/mip linear | clamp | less |

A Vulkan port needs exactly three immutable samplers; the per-pass shaders index them by register.

`PARAM_LTC1` / `PARAM_LTC2` bind two 16×16 LUTs (`ltc_1.h`/`ltc_2.h` raw arrays) to texture slots
4 and 5 (`Project.cpp:1924-1933`, upload in `phxEngine.cpp:298-332`), and
`PARAM_3DTEXTURE6` binds the voxel 3D texture to slot 6.

### 6.4 Filling `CphxMaterialPassConstantState`

**(a) static data + state, once at load** — `BuildPassState` / `CollectRenderState`
(`Project.cpp:1839-1968`) **[VERIFIED]**:

```cpp
void BuildPassState( CphxMaterialTechnique *Tech, CphxMaterialRenderPass *Pass, CphxMaterialPassConstantState* p )
{
  D3D11_RASTERIZER_DESC RasterDesc;
  D3D11_DEPTH_STENCIL_DESC DepthStencilDesc;
  D3D11_BLEND_DESC BlendDesc;
  memset( &RasterDesc, 0, sizeof( RasterDesc ) );
  memset( &DepthStencilDesc, 0, sizeof( DepthStencilDesc ) );
  memset( &BlendDesc, 0, sizeof( BlendDesc ) );
  RasterDesc.FillMode = D3D11_FILL_SOLID;
  RasterDesc.CullMode = D3D11_CULL_BACK;
  RasterDesc.DepthClipEnable = true;
  RasterDesc.AntialiasedLineEnable = true;

  if ( Tech->TargetLayer->VoxelizerLayer )
  {
    RasterDesc.DepthClipEnable = false;
  }

  DepthStencilDesc.DepthEnable = true;
  DepthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  DepthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

  for ( int z = 0; z < 8; z++ ) BlendDesc.RenderTarget[ z ] = blenddesc;
  p->RenderPriority = 127;

  CollectRenderState( Tech->Parameters, RasterDesc, DepthStencilDesc, BlendDesc, p );
  CollectRenderState( Pass->Parameters, RasterDesc, DepthStencilDesc, BlendDesc, p );

  phxDev->CreateBlendState( &BlendDesc, &p->BlendState );
  phxDev->CreateRasterizerState( &RasterDesc, &p->RasterizerState );
  phxDev->CreateDepthStencilState( &DepthStencilDesc, &p->DepthStencilState );
  p->ConstantDataSize *= 4; //hack to convert float count to data size
}
```

**Technique-level parameters are applied before pass-level ones**, so a pass parameter overrides a
technique parameter of the same state type. Constant floats accumulate in that same order
(tech first, then pass) — which must match the shader's `cbuffer b1` field order.

**(b) animated data, every frame** — `CphxMaterialTechnique::CollectAnimatedData`
(`Material.cpp:168-179`) **[VERIFIED]**:

```cpp
void CphxMaterialTechnique::CollectAnimatedData( CphxMaterialPassConstantState *State, int Pass )
{
  CphxMaterialParameterBatch &techbatch = Parameters;
  int datasize = techbatch.CollectAnimatedData();

  CphxMaterialParameterBatch &passbatch = RenderPasses[ Pass ]->Parameters;
  int passdatasize = passbatch.CollectAnimatedData();

  memcpy( State->AnimatedData, techbatch.CollectedData, datasize * sizeof( float ) );
  memcpy( State->AnimatedData + datasize, passbatch.CollectedData, passdatasize * sizeof( float ) );
  State->AnimatedDataSize = ( datasize + passdatasize ) * sizeof( float );
}
```

with (`Material.cpp:23-49`) **[VERIFIED]**:

```cpp
int CphxMaterialParameterBatch::CollectAnimatedData()
{
  int DataPos = 0;
  for ( int x = 0; x < ParamCount; x++ )
  {
    CphxMaterialParameter *p = Parameters[ x ];
    if ( p->Scope != PARAM_ANIMATED ) continue;

    switch ( p->Type )
    {
    case PARAM_FLOAT:
      CollectedData[ DataPos ] = p->Value.Float;
      break;
    case PARAM_COLOR:
      for ( int y = 0; y < 4; y++ )
        CollectedData[ DataPos + y ] = p->Value.Color[ y ];
      break;
    }

    switch ( Parameters[ x ]->Type )
    {
    case PARAM_FLOAT: DataPos += 1; break;
    case PARAM_COLOR: DataPos += 4; break;
    }
  }
  return DataPos;
}
```

* **Only `PARAM_FLOAT` and `PARAM_COLOR` can be animated.** Any other parameter with
  `Scope == PARAM_ANIMATED` is skipped without consuming constant slots (and its state fields —
  blend/z/cull — are never re-collected per frame).
* `AnimatedData` is appended **after** the static constants inside the same `b1` buffer
  (`CphxRenderDataInstance::Render` memcpy's `Matrices` then the whole `MaterialData[64]`;
  `CphxMaterialTechnique::CreateRenderDataInstances` copies `ConstantData` then `AnimatedData` at
  float offset `constdatasize/4`, `Material.cpp:151-155`).
* `MATERIALDATASIZE = 64*sizeof(float) = 256` bytes (`phxEngine.h:50`) is a **hard cap**: more than
  64 floats of constants (static + animated) would run past the 300-byte `ObjectMatrixBuffer`.

**(c) animated value source** — `CphxMaterialSplineBatch` (`Material.cpp:84-95`,
`Material.cpp:211-223`) **[VERIFIED]**:

```cpp
void CphxMaterialSplineBatch::ApplyToParameters( void *GroupingData )
{
  for ( int x = 0; x < SplineCount; x++ )
    if ( Splines[ x ]->GroupingData == GroupingData || GroupingData == NULL )
      Splines[ x ]->Target->Value = Splines[ x ]->GetValue();
}

void CphxMaterialSplineBatch::CalculateValues( float t )
{
  for ( int x = 0; x < SplineCount; x++ )
    Splines[ x ]->CalculateValue( t );
}
```
```cpp
MATERIALVALUE CphxMaterialSpline::GetValue()
{
  MATERIALVALUE v;
  for ( int x = 0; x < 4; x++ )
    v.Color[ x ] = Splines[ x ]->Value[ 0 ];
  return v;
}

void CphxMaterialSpline::CalculateValue( float t )
{
  for ( int x = 0; x < 4; x++ )
    Splines[ x ]->CalculateValue( t );
}
```

`GroupingData` is the **scene/model object** that owns the instance (set at export time via
`<GroupingGUID>` / the tool's `GroupingObject`), which is how one shared material can animate
differently per object. `ApplyToParameters(NULL)` applies all splines (used by shadertoy events).

**(d) variable (per-instance) data** — `ImportTechniqueData` (`Project.cpp:1556-1571`) **[VERIFIED]**:

```cpp
void ImportTechniqueData( CphxProject* proj, CphxMaterialTechnique* t, int& passid, CphxMaterialPassConstantState** state )
{
  for ( int y = 0; y < t->Parameters.ParamCount; y++ )
    if ( t->Parameters.Parameters[ y ]->Scope == PARAM_VARIABLE )
      ImportMaterialParamValue( proj, t->Parameters.Parameters[ y ] );

  for ( int y = 0; y < t->PassCount; y++ )
  {
    CphxMaterialRenderPass* p = t->RenderPasses[ y ];
    for ( int z = 0; z < p->Parameters.ParamCount; z++ )
      if ( p->Parameters.Parameters[ z ]->Scope == PARAM_VARIABLE )
        ImportMaterialParamValue( proj, p->Parameters.Parameters[ z ] );

    BuildPassState( t, p, state[ passid++ ] );
  }
}
```

and `ImportMaterialParamValue` (`Project.cpp:1501-1554`) reads the actual value from
`Stream_MaterialParam` / `Stream_TextureOperatorReference`:

```cpp
  case PARAM_FLOAT:
    Param->Value.Float = ( readbyte( Stream_MaterialParam ) ) / 255.0f;
    break;
  case PARAM_COLOR:
    for ( int x = 0; x < 4; x++ )
      Param->Value.Color[ x ] = ( readbyte( Stream_MaterialParam ) ) / 255.0f;
    break;
  ...
  case PARAM_TEXTURE0: … case PARAM_TEXTURE7:
  {
    int textureop = readshort( Stream_TextureOperatorReference );

    Param->Value.TextureView = NULL; //can't optimize this out as the param is a reused resource
    if ( textureop == 0xffff ) break;

    if ( textureop & 0x8000 )
      Param->Value.TextureView = proj->Timeline.RenderTargets[ textureop & 0x7fff ]->View;
    else
      Param->Value.TextureView = proj->TextureOperators[ textureop ].CachedResult->View;

    break;
  }
  case PARAM_RENDERTARGET:
  {
    int textureop = readshort( Stream_TextureOperatorReference );
    Param->Value.RenderTarget = NULL;
    if ( textureop == 0xffff ) break;
    if ( textureop & 0x8000 )
      Param->Value.RenderTarget = proj->Timeline.RenderTargets[ textureop & 0x7fff ];
    break;
  }
  default:
    Param->Value.ZMode = readbyte( Stream_MaterialParam );
  case PARAM_DEPTHTEXTURE7:
  case PARAM_LTC1:
  case PARAM_LTC2:
    break;
```

Texture reference encoding: 16-bit, `0x8000` set ⇒ **render target index** (`& 0x7fff`), clear ⇒
**texture-operator index**; `0xffff` ⇒ no texture. The referenced texture operator's pre-generated
`CachedResult->View` is used directly.

`PARAM_DEPTHTEXTURE7` needs no data — it is resolved to `phxDepthBufferShaderView` at state
collection time (§7).

**(e) Wire encoding of material parameter values — quantisation matters**

The exporter writes **one byte per scalar** (`apEx/apEx/MinimalExport.cpp:619-654`) **[VERIFIED]**:

```cpp
void ExportMaterialParameterValue( MATERIALPARAMTYPE& type, MATERIALVALUE& value, CphxGUID& textureGUID )
{
  switch ( type )
  {
  case PARAM_FLOAT: //stored on one byte
    WriteByte( Stream_MaterialParam, (TS32)( max( 0, min( 1, value.Float ) ) * 255 ), MaterialParameterDataSize );
    break;
  case PARAM_COLOR:
    for ( TS32 x = 0; x < 4; x++ )
      WriteByte( Stream_MaterialParam, (TS32)( max( 0, min( 1, value.Color[ x ] ) ) * 255 ), MaterialParameterDataSize );
    break;
  case PARAM_ZMODE:
    WriteByte( Stream_MaterialParam, value.ZMode, MaterialParameterDataSize );
    break;
  case PARAM_ZFUNCTION:
    WriteByte( Stream_MaterialParam, value.ZFunction, MaterialParameterDataSize );
    break;
  case PARAM_FILLMODE:
    WriteByte( Stream_MaterialParam, value.Wireframe, MaterialParameterDataSize );
    break;
  case PARAM_CULLMODE:
    WriteByte( Stream_MaterialParam, value.CullMode, MaterialParameterDataSize );
    break;
  case PARAM_RENDERPRIORITY:
    WriteByte( Stream_MaterialParam, value.RenderPriority, MaterialParameterDataSize );
    break;
  case PARAM_BLENDMODE0:
  ...
  case PARAM_BLENDMODE7:
    WriteByte( Stream_MaterialParam, value.BlendMode + 0x11, MaterialParameterDataSize );
    break;
```

Consequences that a reimplementation **must** reproduce:

* **`PARAM_FLOAT` and `PARAM_COLOR` are quantised to 8 bits and clamped to [0,1] on export** and
  divided by 255 on load. A float material parameter can therefore never be negative or > 1 in a
  release build, and has only 256 distinct values. (The XML `.apx` path stores the full 16-byte
  union as base64 and is *not* quantised — the tool and the release differ here.)
* **`PARAM_RENDERPRIORITY` is one byte** in the stream but lands in an `int`
  (`CphxRenderDataInstance::RenderPriority`) and in an `unsigned char`
  (`CphxMaterialPassConstantState::RenderPriority`) — priorities are effectively 0..255 with the
  default 127.
* **`PARAM_BLENDMODE*` is biased by `+0x11` on export** so that the byte can be cast straight to
  `D3D11_BLEND` at runtime. The authoring encoding in `Material.h:69-99` (`phxSrcBlend_*` = 0..14,
  `phxDstBlend_*` = 0x00,0x10,…,0xe0) is 0-based; adding `0x11` maps
  `phxSrcBlend_ZERO(0)→D3D11_BLEND_ZERO(1)`, `phxSrcBlend_ONE(1)→ONE(2)`, …,
  `phxSrcBlend_INVBLENDFACTOR(14)→INV_BLEND_FACTOR(15)`, and the same for the destination nibble.
  `phxSrcBlend_BOTHSRCALPHA(11)`/`BOTHINVSRCALPHA(12)` map to 12/13 which are **not valid**
  `D3D11_BLEND` values — a port must special-case or ignore them. **[VERIFIED arithmetic]**
* **`PARAM_ZMODE`, `PARAM_ZFUNCTION`, `PARAM_FILLMODE`, `PARAM_CULLMODE` are written as one byte
  into a 16-byte union**, but `ImportMaterialParamValue` writes only that one byte
  (`Param->Value.ZMode = readbyte(Stream_MaterialParam)`), and the parameter object comes from
  `new CphxMaterialParameter` with no zeroing (`Project.cpp:1485-1499`):
  ```cpp
  void ImportMaterialParams( CphxProject *proj, CphxMaterialParameterBatch &Params )
  {
    Params.ParamCount = readbyte( Stream_Main );
    Params.Parameters = new CphxMaterialParameter*[ Params.ParamCount ];

    for ( int x = 0; x < Params.ParamCount; x++ )
    {
      CphxMaterialParameter *p = new CphxMaterialParameter;
      Params.Parameters[ x ] = p;
      p->Scope = (MATERIALPARAMSCOPE)( readbyte( Stream_Main ) );
      p->Type = (MATERIALPARAMTYPE)( readbyte( Stream_Main ) );
      if ( p->Scope == PARAM_CONSTANT ) 
        ImportMaterialParamValue( proj, p );
    }
  }
  ```
  Multi-byte union members (`Wireframe` bool, `CullMode`, `ZFunction`, `RenderPriority` int) are
  therefore read with **indeterminate high bytes**. In practice this works, which implies the
  allocator hands back zeroed memory — but a reimplementation should zero the union and then apply
  the single byte. **[VERIFIED code, UNCERTAIN runtime behaviour]**

### 6.5 Per-frame GPU upload, in order

For each mesh pass, per frame:

1. `Clip->MaterialSplines->ApplyToParameters(this)` — animated parameters of *this instance* are
   written into `CphxMaterialParameter::Value` (`Model.cpp:34`).
2. `Material->Techniques[x]->CollectAnimatedData(MaterialState[passid++], y)` — parameters are
   packed into `AnimatedData` floats (`Model.cpp:40-42`).
3. `Material->CreateRenderDataInstances(...)` → `CphxMaterialTechnique::CreateRenderDataInstances`
   creates one `CphxRenderDataInstance` per pass, `memcpy`s shaders (5 pointers), states (3
   pointers + 8 textures = 11 pointers), constant data, animated data, and the two world matrices
   (`Material.cpp:117-166`).
4. `CphxRenderDataInstance::Render()` uploads `Matrices[2] + MaterialData[64]` into
   `ObjectMatrixBuffer` and draws (§3.2).

So the constant data is effectively `[static floats][animated floats]` and the port should build one
per-draw constant block of `128 + 256` bytes (or push constants / a mapped ring).

`CphxMaterialPassConstantState::TextureBackup[8]` is used **only** by the shadertoy path to
temporarily swap a read-write target for `phxInternalRenderTarget` (§2.7).

### 6.6 Particle materials

`CphxObject_ParticleEmitter_CPU::CreateRenderDataInstances` (`Scene.cpp:608-645`) applies the
emitter clip's material splines grouped by the emitter, collects animated data per pass, forces
`Textures[0] = SplineTextureView` on every pass (the life/size curves are sampled from a
`2048 × n` `R32_FLOAT` texture built by `UpdateSplineTexture`, `Scene.cpp:729-774`), then calls
`Material->CreateRenderDataInstances(MaterialState, RootScene, VertexBuffer, LiveCount)` which
creates **non-indexed** draws (`Indexed=false` ⇒ `POINTLIST` unless a mesh/subscene emission mode is
active).

---

## 7. RENDER STATE DEFAULTS

### 7.1 Defaults and the `PARAM_*` → state mapping

Defaults are set in `BuildPassState` (§6.4) **[VERIFIED]**:

| state | default |
|---|---|
| `RasterizerDesc.FillMode` | `D3D11_FILL_SOLID` |
| `RasterizerDesc.CullMode` | `D3D11_CULL_BACK` |
| `RasterizerDesc.DepthClipEnable` | `true` (→ `false` if the technique's layer is a voxelizer layer) |
| `RasterizerDesc.AntialiasedLineEnable` | `true` |
| everything else in `RasterizerDesc` | zero from `memset` (`FrontCounterClockwise=false`, `DepthBias=0`, `ScissorEnable=false`, `MultisampleEnable=false`) |
| `DepthStencilDesc.DepthEnable` | `true` |
| `DepthStencilDesc.DepthWriteMask` | `D3D11_DEPTH_WRITE_MASK_ALL` |
| `DepthStencilDesc.DepthFunc` | `D3D11_COMPARISON_LESS` |
| `DepthStencilDesc.StencilEnable` | `false` (memset) |
| `BlendDesc.RenderTarget[0..7]` | the static `blenddesc` in `Project.cpp:97`: `BlendEnable=true, SrcBlend=ONE, DestBlend=ZERO, BlendOp=ADD, SrcBlendAlpha=ONE, DestBlendAlpha=ZERO, BlendOpAlpha=ADD, RenderTargetWriteMask=ALL` |
| `AlphaToCoverageEnable`, `IndependentBlendEnable` | false (memset) |
| `CphxMaterialPassConstantState::RenderPriority` | `127` |
| `CphxMaterialPassConstantState::Wireframe` | false (caller-allocated, not set here) |

So the engine-wide default is "opaque, z-test + z-write with LESS, back-face culling, no blending" —
note `BlendEnable = true` with `ONE/ZERO`, which is equivalent to opaque.

### 7.2 Per-parameter effect

`apEx/Phoenix/Project.cpp:1873-1968` **[VERIFIED]** — `CollectRenderState`, reproduced because it
is the exact contract for every `PARAM_*`:

```cpp
void CollectRenderState( CphxMaterialParameterBatch &Params, D3D11_RASTERIZER_DESC &RasterDesc, D3D11_DEPTH_STENCIL_DESC &DepthStencilDesc, D3D11_BLEND_DESC &BlendDesc, CphxMaterialPassConstantState *pass )
{
  for ( int x = 0; x < Params.ParamCount; x++ )
  {
    MATERIALVALUE &v = Params.Parameters[ x ]->Value;
    switch ( Params.Parameters[ x ]->Type )
    {
    case PARAM_FLOAT:
      if ( Params.Parameters[ x ]->Scope != PARAM_ANIMATED )
        pass->ConstantData[ pass->ConstantDataSize++ ] = v.Float;
      break;
    case PARAM_COLOR:
      if ( Params.Parameters[ x ]->Scope != PARAM_ANIMATED )
        for ( int y = 0; y < 4; y++ )
          pass->ConstantData[ pass->ConstantDataSize++ ] = v.Color[ y ];
      break;
    case PARAM_ZMODE:
      DepthStencilDesc.DepthEnable = !( v.ZMode & 1 ); //default zero means z enabled
      DepthStencilDesc.DepthWriteMask = !( ( v.ZMode >> 1 ) & 1 ) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO; //default zero means z write enabled
      break;
    case PARAM_ZFUNCTION:
      DepthStencilDesc.DepthFunc = v.ZFunction;
      break;
    case PARAM_TEXTURE0:
    case PARAM_TEXTURE1:
    ...
    case PARAM_TEXTURE7:
      pass->Textures[ Params.Parameters[ x ]->Type - PARAM_TEXTURE0 ] = v.TextureView;
      break;
    case PARAM_RENDERTARGET:
      pass->RenderTarget = v.RenderTarget;
      break;
    case PARAM_DEPTHTEXTURE7:
      pass->Textures[ 7 ] = phxDepthBufferShaderView;
      break;
#ifdef LTC1
    case PARAM_LTC1:
      pass->Textures[ 4 ] = ltc1view;
      break;
#endif
#ifdef LTC2
    case PARAM_LTC2:
      pass->Textures[ 5 ] = ltc2view;
      break;
#endif
#ifdef PHX_VOLUMETRIC_RENDERTARGETS
    case PARAM_3DTEXTURE6:
      pass->Textures[ 6 ] = phxTexture3DResourceView;
      break;
#endif
    case PARAM_BLENDMODE0:
    ...
    case PARAM_BLENDMODE7:
    {
      int z = Params.Parameters[ x ]->Type - PARAM_BLENDMODE0;
      BlendDesc.RenderTarget[ z ].BlendEnable = true;
      BlendDesc.RenderTarget[ z ].SrcBlend = (D3D11_BLEND)( ( v.BlendMode & 15 ) );
      BlendDesc.RenderTarget[ z ].DestBlend = (D3D11_BLEND)( ( v.BlendMode >> 4 ) );
    }
    break;
    case PARAM_CULLMODE:
      RasterDesc.CullMode = v.CullMode;
      break;
    case PARAM_FILLMODE:
      pass->Wireframe = v.Wireframe;
      break;
    case PARAM_RENDERPRIORITY:
      pass->RenderPriority = v.RenderPriority;
      break;

    default:
      break;
    }
  }
}
```

Important consequences:

* **`PARAM_FLOAT`/`PARAM_COLOR` with scope `ANIMATED` do not consume a static constant slot** — they
  are appended to the animated block instead (§6.4b). Their relative position inside the animated
  block is the order of `PARAM_ANIMATED` float/color parameters in the technique+pass parameter
  lists.
* **`PARAM_BLENDMODE*` implicitly enables blending** (`BlendEnable = true`) and only sets
  `SrcBlend`/`DestBlend`; the blend **op** stays `ADD` and the alpha channel keeps the default
  `ONE/ZERO`. The nibble encoding is defined in `Material.h:69-99`:
  `SrcBlend` constants are `0x0..0x0e` (ZERO, ONE, SRCCOLOR, INVSRCCOLOR, SRCALPHA, INVSRCALPHA,
  DESTALPHA, INVDESTALPHA, DESTCOLOR, INVDESTCOLOR, SRCALPHASAT, BOTHSRCALPHA, BOTHINVSRCALPHA,
  BLENDFACTOR, INVBLENDFACTOR) and `DestBlend` constants are the same values `<< 4`. The exporter
  adds `+0x11` to that authoring byte (`§6.4e`), which turns the 0-based `phx` indices into the
  1-based `D3D11_BLEND` values, so the stream byte can be fed straight into
  `SrcBlend = v.BlendMode & 15` / `DestBlend = v.BlendMode >> 4`. `phxSrcBlend_BOTHSRCALPHA` /
  `BOTHINVSRCALPHA` map to invalid D3D values 12/13.
* **`PARAM_ZMODE`** bit 0 = disable depth test, bit 1 = disable depth write. Note
  `Material.h` declares `ZMode` as a single `unsigned char` in the union; the tool exports it via
  the `default:` branch of `ImportMaterialParamValue` (one byte from `Stream_MaterialParam`).
* **`PARAM_FILLMODE`** stores a **bool** (`Wireframe`) not a `D3D11_FILL_MODE`; it only affects
  vertex/index buffer choice and primitive topology, **not** `RasterizerDesc.FillMode`
  (which stays SOLID — a real D3D state/behaviour mismatch; the wireframe look comes from the
  line index buffer). **[VERIFIED]**
* **`PARAM_ZFUNCTION`** carries a `D3D11_COMPARISON_FUNC` — the `COMPARISONFUNCTION` enum in
  `Material.h:12-22` uses a *different* ordering (LESSEQUAL=0, NEVER=1, LESS=2, …) so it is
  **not** wire-compatible with `D3D11_COMPARISON_FUNC`; the tool writes the D3D value
  (`Parameter.Value.ZFunction` is the union member). Any importer must use the D3D ordering.
  **[VERIFIED — the enum appears unused; the union field is the D3D enum]**
* **`PARAM_MESHDATA0..7`**, `PARAM_PARTICLELIFE(FLOAT)`, `PARAM_MESH*` have **no case** in
  `CollectRenderState` → they contribute nothing to the runtime state on this path. Mesh data
  buffers are bound by the tool's editor path only (`CphxParameterDataCollector`, not used by the
  release loader). **[VERIFIED]**
* **`PARAM_RENDERTARGET`** is meaningful only for shadertoy events (§2.7); for a normal mesh pass it
  merely stores a pointer that is never read.
* `pass->ConstantDataSize` is a **float count** in `CollectRenderState`; `BuildPassState` multiplies
  by 4 at the end. `CollectAnimatedData` returns a float count which is multiplied by
  `sizeof(float)` in `CphxMaterialTechnique::CollectAnimatedData`. So `ConstantDataSize` and
  `AnimatedDataSize` are both **bytes** downstream. **[VERIFIED]**

### 7.3 Tool-side state creation (for parity)

The tool builds the same struct through a different code path
(`CphxParameterDataCollector::Apply`, `Material_Tool.cpp:1275-1291`):

```cpp
void CphxParameterDataCollector::Apply( CphxMaterialPassConstantState *p )
{
  p->RenderPriority = RenderPriority;
  p->Wireframe = Wireframe != 0;
  if ( p->BlendState ) p->BlendState->Release();
  ...
  phxDev->CreateBlendState( &BlendDesc, &p->BlendState );
  phxDev->CreateRasterizerState( &RasterDesc, &p->RasterizerState );
  phxDev->CreateDepthStencilState( &DepthStencilDesc, &p->DepthStencilState );
  ApplyTextures( p );
  for ( TS32 x = 0; x < StaticFloatData.NumItems() && x < MATERIALDATASIZE / sizeof( float ); x++ )
    p->ConstantData[ x ] = StaticFloatData[ x ];
  p->ConstantDataSize = min( StaticFloatData.NumItems() * sizeof( float ), MATERIALDATASIZE / sizeof( float ) );
  p->RenderTarget = RenderTarget;
}
```

and the tool's default rasterizer/depth/blend come from `InitDefaultStates`
(`apEx/apEx/apExBase.cpp`/`DefaultStates` — the tool-level `RenderRasterState`,
`RenderDepthState_Normal`, `DisableBlend` globals applied around every tool draw). These are
**editor** defaults (e.g. `DisableBlend` for presentation), not engine defaults; a player only needs
the engine defaults in §7.1.


---

## 8. MinimalPlayer.cpp — what a minimal release player actually does

Read in full: `apEx/MinimalPlayer/MinimalPlayer.cpp` (315 lines). Supporting files:
`MinimalPlayer.h`, `WindowHandler.cpp/.h`, `MusicPlayer.cpp/.h`, `SetupDialog.cpp/.h`,
`release.asm` / `asm.bat` (in `Data/`).

### 8.1 Startup sequence (`MinimalPlayer.cpp:85-267`) **[VERIFIED]**

```cpp
INT WINAPI WinMain( __in HINSTANCE hInstance, __in_opt HINSTANCE hPrevInstance, __in LPSTR lpCmdLine, __in INT nCmdShow )
{
  OPENDEBUGFILE();
  ...
  for ( int x = 0; x < 7; x++ )
  {
    DEBUGLOG( "Loading Library %s", Libraries[ x ] );

    if ( !LoadLibrary( Libraries[ x ] ) )
    {
      ...
      MessageBoxA( NULL, (LPCSTR)meh, NULL, MB_ICONERROR );
      return 0;
    }
    ...
  }

  unsigned char *DemoData = (unsigned char*)demo;
  ...
  char *Strings[ 7 ];

  static CphxProject Precalc;
  static CphxProject Demo;

  DEBUGLOG( "Parsing Demo Data" );
  for ( int x = 0; x < 7; x++ )
  {
    Strings[ x ] = (char*)DemoData;
    while ( *( DemoData++ ) );
  }
  ...
          DEBUGLOG( "Opening Setup Window" );
          if ( OpenSetupDialog( Strings ) )
          { ... }
  ...
          DEBUGLOG( "Initializing Window: Fullscreen (%d) Width (%d) Height (%d) Frequency (%d)", setupcfg.fullscreen, setupcfg.mode.dmPelsWidth, setupcfg.mode.dmPelsHeight, setupcfg.mode.dmDisplayFrequency );

          InitWindow( hInstance, setupcfg.mode.dmPelsWidth, setupcfg.mode.dmPelsHeight, setupcfg.mode.dmDisplayFrequency, setupcfg.fullscreen != 0, LoadIcon( hInstance, MAKEINTRESOURCE( IDI_ICON1 ) ), 0 );

          DEBUGLOG( "Initializing Phoenix" );

          InitializePhoenix();

          DEBUGLOG( "--------------------------------- LOADING PRECALC PROJECT" );
          Precalc.LoadProject( (unsigned char*)precalc + 7, NULL, setupcfg );
          DEBUGLOG( "--------------------------------- LOADING DEMO PROJECT" );
          Demo.LoadProject( DemoData, &Precalc, setupcfg );
```

Libraries loaded explicitly (`MinimalPlayer.cpp:73-82`): `d3d11.dll`, `d3dx10_43.dll`,
`d3dx11_43.dll`, `dsound.dll`, `msacm32.dll`, `glu32.dll`, `d3dcompiler_47.dll`. Failure of any of
them produces a message box and process exit — a useful enumeration of the runtime's real
dependencies.

**Order matters**: window + D3D device first (`InitWindow` creates the swap chain and therefore
`phxDev`/`phxContext`), then `InitializePhoenix()` (constant buffers, samplers, inline shaders,
texgen pool, LTC LUTs), then the precalc project (shader/texture precomputation), then the demo
project.

**The 7 strings** at the head of the demo blob are read out of `release.64k` and are, in order
(verified against `_deleteme_test.64k`: `CONSPIRACY`, `Darkness lay your eyes upon me`,
`http://conspiracy.hu`, pouet, facebook, youtube, share):

| index | content | consumer |
|---|---|---|
| 0 | Group | `setupcfg.GroupName` |
| 1 | Title | `setupcfg.ReleaseName` |
| 2 | www url | `setupcfg.urls[1]` |
| 3 | pouet url | `setupcfg.urls[3]` |
| 4 | facebook url | `setupcfg.urls[2]` |
| 5 | youtube url | `setupcfg.urls[4]` |
| 6 | share url | `setupcfg.urls[0]` |

from `SetupDialog.cpp:182-192` and `MinimalExport.cpp:2193-2199` (which writes Group first, then
Title, then `Urls[0..4]`).

### 8.2 Window, swap chain and presentation

`apEx/MinimalPlayer/WindowHandler.cpp:43-106` **[VERIFIED]**:

```cpp
#ifdef PHOENIX_ENGINE_LINEAR_RENDER
static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, 0, 0, DXGI_SWAP_EFFECT_DISCARD, 0 };
#else
static DXGI_SWAP_CHAIN_DESC scd = { 0, 0, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED, 1, 0, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, 0, 0, DXGI_SWAP_EFFECT_DISCARD, 0 };
#endif
...
  DWORD dwStyle = !FullScreen ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_MINIMIZEBOX : WS_POPUP | WS_OVERLAPPED;
  AdjustWindowRect( &WindowRect, dwStyle, FALSE );

  hWnd = CreateWindowA( clss, nullptr, dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, WindowRect.right - WindowRect.left, WindowRect.bottom - WindowRect.top, NULL, NULL, h, NULL );

  scd.BufferDesc.RefreshRate.Numerator = refreshrate;
  scd.OutputWindow = (HWND)hWnd;
  scd.Windowed = !FullScreen;
  scd.BufferDesc.Width = XRes;
  scd.BufferDesc.Height = YRes;
  ...
    D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &scd, &SwapChain, &phxDev, NULL, &phxContext );

  ID3D11Texture2D *bb;
  SwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), (LPVOID*)&bb );
  phxDev->CreateRenderTargetView( bb, NULL, &phxBackBufferView );
```

* **One back buffer**, `DXGI_SWAP_EFFECT_DISCARD`, `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` when
  `PHOENIX_ENGINE_LINEAR_RENDER` is defined (`phxEngine.h:14` defines it unconditionally!), i.e.
  the demo renders **linear** colours into an RT and lets the swap chain do sRGB encode.
  ⇒ In Vulkan use `VK_FORMAT_R8G8B8A8_SRGB` (or `B8G8R8A8_SRGB`) for the surface and a non-sRGB
  intermediate, exactly as here.
* `Present(vsync ? 1 : 0, 0)` — `MinimalPlayer.cpp:290`. No tearing handling, no frame pacing.
* `WM_KEYDOWN`/`VK_ESCAPE` and `WM_CLOSE` set `phxDone = true`, which breaks the main loop
  (`WindowHandler.cpp:28-38`).
* No DPI awareness, no resize handling, no device-lost handling (`DeviceOK()` does not exist in the
  minimal player). A port must add these.

### 8.3 Resolution / scaling behaviour

Three mutually exclusive paths in `MinimalPlayer.cpp` **[VERIFIED]**, selected by preprocessor
defines:

1. **Default release** (`#ifndef HIGHFRAMERATECOMPO`, `#ifndef PHX_FAKE_FARBRAUSCH_INTRO_BUILD`,
   `#ifndef _DEBUG`): show the engine's own setup dialog (`OpenSetupDialog`), which lets the user
   pick fullscreen/windowed, resolution and vsync into the global `SETUPCFG setupcfg`
   (`SetupDialog.h`):
   ```cpp
   typedef struct
   {
     char *ReleaseName;
     char *GroupName;
   #ifdef SETUPBOX_HAS_SOCIAL
     char *urls[ 5 ];
   #endif
     int fullscreen;
     DEVMODE mode;
     int vsync;
     float HardwareAspectRatio;
   } SETUPCFG;
   ```
2. **`PHX_FAKE_FARBRAUSCH_INTRO_BUILD`**: hard-coded 1920×1080 fullscreen, aspect 16/9, vsync on.
3. **`HIGHFRAMERATECOMPO`**: command-line driven
   (`nowarnings`, `720p` → 1280×720, `current` → current desktop mode, `novsync`), with explicit
   DXGI mode enumeration that **prefers a 120 Hz refresh rate** and warns if the requested
   resolution is unsupported or the refresh rate is below 120:
   ```cpp
             if ( displayModeList[ i ].Width == (unsigned int)xres )
             {
               if ( displayModeList[ i ].Height == (unsigned int)yres )
               {
                 float displayfreq = displayModeList[ i ].RefreshRate.Numerator / (float)displayModeList[ i ].RefreshRate.Denominator;
                 if ( abs( displayfreq - 120 ) < 1 || displayfreq >= selected.dmDisplayFrequency )
                 {
                   selected.dmPelsWidth = xres;
                   selected.dmPelsHeight = yres;
                   selected.dmDisplayFrequency = displayModeList[ i ].RefreshRate.Numerator;
                   SetDenominator( displayModeList[ i ].RefreshRate.Denominator );
                   if ( abs( displayfreq - 120 ) < 1 ) break;
                 }
               }
             }
   ...
           if ( selected.dmDisplayFrequency != 120 )
             if ( warningdialogs ) MessageBoxA( NULL, "120hz not supported by display device.\nRunning on lower frequency.", NULL, MB_ICONWARNING );

           setupcfg.vsync = vsync;
           setupcfg.mode = selected;
           setupcfg.HardwareAspectRatio = current.dmPelsWidth / (float)current.dmPelsHeight;
           setupcfg.fullscreen = true;
   ```
   `HIGHFRAMERATECOMPO` is **not defined** in the checked-in code (it is commented out at
   `MinimalPlayer.cpp:141`), but it is the only codepath that reveals the intended 120 Hz +
   120 fps assumption.
4. **`_DEBUG`**: 800×450 windowed, 16/9, vsync.

The actual demo-resolution computation then happens in `CphxProject::LoadProject` (§2.1) using
`setupcfg.mode.dmPelsWidth/Height`, `setupcfg.fullscreen` and `setupcfg.HardwareAspectRatio`.
Nothing in the release player rescales afterwards: the backbuffer is the mode size and the engine
blits the demo target 1:1 at a computed offset (§2.5). **There is no upscaling, no
`DXGI_MODE_SCALING` handling, no render-scale option.** If the display mode cannot be exactly the
demo resolution, the demo resolution is re-derived to the same aspect ratio and the swap chain
stays at the mode size — letterbox/pillarbox bars appear.

Because the frame number comes from music position, **a slower display simply skips frames**
(`floor(ms*120/1000)` jumps), it does not slow the demo down.

### 8.4 The `release.*` blobs and `.64k` files

The player references three linker symbols declared by (`MinimalPlayer.cpp:44-50`) **[VERIFIED]**:

```cpp
#define BINARY_DECLARE(x) \
	extern "C" char x[];\
	extern "C" int x##_size;

BINARY_DECLARE( music );
BINARY_DECLARE( demo );
BINARY_DECLARE( precalc );
```

They are produced by NASM (`apEx/MinimalPlayer/Data/release.asm`) **[VERIFIED]**:

```asm
;----------------------------------------
%macro include_binary 2
global %1
%1 incbin %2
%1end:
global %1_size
%1_size dd %1end - %1
%endmacro 
;----------------------------------------

section .data

include_binary _music, "release.mus"
include_binary _demo, "release.64k"
include_binary _precalc, "precalc.64k"
```

built with `asm.bat` → `nasmw.exe -f win32 release.asm -o release.obj`, and `Data/release.obj` is
linked into the player (`MinimalPlayer.vcxproj`). The three input files are staged by the tool:

* `release.64k` — `ExportMinimal(<src>/MinimalPlayer/Data/release.64k, zip, false)`
  (`apExRoot.cpp:1763`), i.e. the **demo project**, uncompressed.
* `precalc.64k` — copied from one of
  `precalcphoenix_shader.64k` / `precalcminimal.64k` / `precalcfabrausch.64k` /
  `precalcapex.64k` depending on the precalc mode (`apExRoot.cpp:1768-1781`).
* `release.mus` — a copy of `Project.SongFile` (`apExRoot.cpp:1793`).

#### 8.4.1 `release.64k` (demo) layout

Written by `ExportMinimal` (`apEx/apEx/MinimalExport.cpp:2063-3040`) and consumed by
`CphxProject::LoadProject` (`Project.cpp:262-278`) **[VERIFIED]**:

```
offset 0                    7 NUL-terminated ASCII strings (Group, Title, url0..url4)
                             (may be empty strings, each still 1 byte)
then                        StreamCount (=19) little-endian uint32 stream offsets
then                        stream payloads, concatenated
```

* `StreamCount` comes from `PHXDATASTREAMS` (`Project.h:11-38`) and is **19**:
  `Main, ASCIIZ, Shaders, SplineDescriptors, SplineKeyCounts, SplineKeyTimes, SplineKeyValues,
  TextureOperatorReference, MaterialParam, ModelPrimitives, ModelTransformation, ModelParameters,
  ModelReferences, MaterialID, ModelFilterCount, ModelFilterType, ModelFilterData, EventType,
  EventPosition`.
* The offset of stream 0 is **76** = `StreamCount*4` — the size of the offset table itself; every
  other offset is relative to the first byte of the offset table (equivalently: relative to the
  pointer the loader receives). The writer forces this
  (`MinimalExport.cpp:2984-2999`):
  ```cpp
  for ( TS32 x = 0; x < StreamCount; x++ )
  {
    ( (TS32*)( MinimalStreams[ Stream_Main ].GetData() + StreamStartArrayPosition ) )[ x ] = Position;
    if ( x != Stream_Main )
    {
      Position += MinimalStreams[ x ].GetLength();
    }
    else
    {
      ( (TS32*)( MinimalStreams[ Stream_Main ].GetData() + StreamStartArrayPosition ) )[ x ] = StreamCount * 4; // add the size of the stream position array so it's not needed in the minimalplayer
      Position += MinimalStreams[ x ].GetLength() - StreamStartArrayPosition;
    }
  }
  ```
  ⚠ `MinimalPlayer/Data/_deleteme_test.64k` (a stale dev artifact in the repo) has offset[0] == 0,
  which cannot be parsed correctly by `LoadProject` (Stream_Main would begin with the offset table
  itself, yielding `AspectX=0, AspectY=0, FrameRate=0, RenderTargetCount=0`). Treat it as stale.
  **[VERIFIED observation, UNCERTAIN provenance]**
* The loader (`Project.cpp:271-274`):
  ```cpp
    // stream setup
    datastreams = (unsigned char**)DATA;
    for ( int x = 0; x < StreamCount; x++ )
      datastreams[ x ] += (int)DATA;
  ```
  i.e. `datastream[ i ] = DATA + offsets[ i ]`, where `DATA` is the position right after the 7
  strings (because `MinimalPlayer.cpp` advances `DemoData` past them).
* Stream 0 starts with the packed header (§2.1):
  `AspectX|AspectY<<8|FrameRate<<16|RenderTargetCount<<24`, then per render target
  `resolutionDescriptor` + `pixelFormat`, then the render-layer table, then texture filters,
  texture operators, techniques (with layer indices), materials, models, scenes, events. The
  complete read order is the body of `CphxProject::LoadProject`
  (`Project.cpp:262-1478`) — see the staged `DoPrecalc()` calls which mark the phases
  (filters, texture pages, texture operators).
* A sidecar `<target>.sym` is written alongside for debugging
  (`MinimalExport.cpp:3029-3039`): magic `"PHXP"`, `MinimalDataSize`, symbol count, then symbol
  names (NUL-separated) and the symbol stream. Not needed at runtime.
* If `zip == true` (used by the "k-krunchy" 64k build path) the file instead begins with
  `{int compressedLength; int uncompressedLength;}` followed by a miniz-deflate blob
  (`MinimalExport.cpp:3006-3026`). `MinimalPlayer.cpp` has the matching
  `#ifdef MUSIC_IS_OGG` header read (`MinimalPlayer.cpp:118-127`) with the actual
  `uncompress()` call commented out — so in the current code `zip` is always `false` for the
  paths that feed the player. **[VERIFIED]**

#### 8.4.2 `precalc.64k` layout

Identical stream-table format, but the precalc project has **empty Group/Title/urls**, which are
written as 7 NUL bytes, and the player skips them with a hard-coded `+ 7`
(`MinimalPlayer.cpp:264`):

```cpp
          Precalc.LoadProject( (unsigned char*)precalc + 7, NULL, setupcfg );
```

Byte-level verification on the checked-in `Data/precalc.64k` (which is the **Clean Slate** precalc:
`10 09 78 01` at the first stream payload) **[VERIFIED]**:

```
0000  00 00 00 00 00 00 00        <- 7 empty ProdInfo strings
0007  4C 00 00 00                 <- offsets[0] = 76 (table size)
000B  69 00 00 00                 <- offsets[1] = 105
...
0053  10 09 78 01                 <- Stream_Main payload: AspectX=16, AspectY=9, FrameRate=120, RTCount=1
```

so `AspectX=16, AspectY=9, FrameRate=120, RenderTargetCount=1` for Clean Slate. This is a
convenient sanity check for any importer.

`precalc` is used at load only for `DoPrecalc(Pos)`: it is re-rendered into the swap chain as a
progress bar during loading and to warm up shader compilation (`Project.cpp:167-194`):

```cpp
static bool DoPrecalc( float Pos )
{
  if ( !Precalc ) return false;
  int tme = timeGetTime();
  if ( tme - LastPrecalcTime < 40 ) return false;
  LastPrecalcTime = tme;

  Precalc->Timeline.Render( Pos * 1000, false, false );

#ifdef DEBUGINTOFILE
  HRESULT res =
#endif
    SwapChain->Present( 0, 0 );
  ...
  return phxDone;
}
```

⚠ `Precalc->Timeline.Render(Pos * 1000, ...)` passes **milliseconds as a frame number** here (no
`/1000*FrameRate`), unlike `CphxProject::Render`. It is a progress bar, so it does not matter, but
it means the precalc project's events must be authored to span 0..1000 "frames". **[VERIFIED]**

#### 8.4.3 `release.mus`

The raw song file (`.mvm` for Clean Slate: `step010padding.mvm`), linked verbatim and handed to
`MUSIC_INIT(hWnd, music, music_size)`. `MusicPlayer.h` selects one of three backends at compile
time:

```cpp
#else // MVX is the default, of course
#define MUSIC_INIT(a,b,c) mvxSystem_Init(a,b,c)
#define MUSIC_PLAY mvxSystem_Play
#define MUSIC_GETSYNC mvxSystem_GetSync
#define MUSIC_STOP mvxSystem_Stop
#define MUSIC_DEINIT mvxSystem_DeInit
#endif
```

`Release 64k` builds use MVX; `Release Demo` builds define `MUSIC_IS_OGG`
(`MinimalPlayer.vcxproj:203`). MVX is a **prebuilt x86-32 static library**
(`apEx/MVX/MVXLibrary.lib`, header `apEx/MVX/mvx.h`) with no source in this repository —
the single hardest dependency for a cross-platform port. See `docs/spec/music-mvx.md`.

### 8.5 Exact per-frame behaviour of the release player

```
loop:
  pump all Win32 messages (no TranslateMessage)      # MinimalPlayer.cpp:280-284
  frame = MUSIC_GETSYNC()                            # song position in ms
  Demo.Render(frame)                                 # = Timeline.Render(frame/1000*fps, tool=false, subroutine=false)
  SwapChain->Present(vsync ? 1 : 0, 0)
```

`Demo.Render` → `CphxTimeline::Render` → per §1.3 and §2.2. There is **nothing else**: no
frame limiter, no timer, no accumulator, no interpolation between frames, no delta time passed to
the engine except through `prevt = t - 1 frame`.

### 8.6 Shutdown

`MinimalPlayer.cpp:294-304` **[VERIFIED]**:

```cpp
            DEBUGLOG( "Stopping Music" );
            MUSIC_STOP();
            DEBUGLOG( "Sound Deinit" );
            MUSIC_DEINIT();
          }

          if ( setupcfg.fullscreen )
          {
            DEBUGLOG( "Switching fullscreen mode back" );
            SwapChain->SetFullscreenState( FALSE, NULL ); //at least return from fullscreen mode properly
          }
```

No resource release, no `DestroyWindow` — the 64k ethos. `phxDone` is set by `EVENT_ENDDEMO`
(or by ESC/close).


---

## 9. Porting notes for a headless-capable Vulkan player

This section is *derived*, not quoted. It collects the D3D11 assumptions that have no direct
Vulkan equivalent and the minimum work to be frame-accurate.

### 9.1 Structural mapping

| D3D11 concept | engine use | Vulkan equivalent |
|---|---|---|
| `CphxRenderLayerDescriptor` | colour attachments + optional depth + clear policy | one `VkRenderPass`/dynamic rendering instance; **attachments must share dimensions** — see §3.1 |
| `CphxRenderDataInstance` | full pipeline state + shaders + 8 SRVs + 2 matrices + 64 constant floats | pre-built `VkPipeline` cache (key = shader set + blend + depth + raster + topology + vertex layout) + descriptor set + a slot in a per-frame constant ring |
| `ObjectMatrixBuffer` (`b1`) | 128 B matrices + 256 B material | uniform/storage buffer slice or push constants (128+256 = 384 B; within the 128 B push-constant minimum? **no** — must be a UBO) |
| `SceneDataBuffer` (`b0`) | 1200 B scene constants, re-uploaded **per layer** | UBO updated per layer; note it is bound to VS+GS+PS |
| 3 global samplers (s0/s1/s2) | fixed filter/address/compare | immutable samplers in a shared descriptor set layout |
| `GenerateMips` | after every layer/pass | explicit blit chain (the D3D implementation is a driver box filter; exact pixel parity is *not* guaranteed across vendors — for byte-exact output prefer implementing your own and validating against a reference capture) |
| `ClearRenderTargetView` / `ClearDepthStencilView` | per frame (all targets) + per layer (predicated) | `vkCmdClearColorImage`/render-pass `loadOp = CLEAR`, or attachment load ops |
| `D3D11_PRIMITIVE_TOPOLOGY_LINELIST` | wireframe via a separate line index buffer | `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` + a distinct pipeline (rasterization mode does not need to change; the engine never sets `FillMode = WIREFRAME`) |
| `D3DXMatrixLookAtRH` / `PerspectiveOffCenterRH` | camera | same maths, but **flip Y** and remap clip-space Z from [-1,1] to [0,1], or use negative viewport height + `VK_KHR_maintenance1` |
| `DXGI_FORMAT_R16G16B16A16_FLOAT` / `R32_FLOAT` RTs | all colour targets | `VK_FORMAT_R16G16B16A16_SFLOAT` / `VK_FORMAT_R32_SFLOAT` |
| `R24G8_TYPELESS` depth, sampled as `R24_UNORM_X8` | depth texture 7 for shadows/SSAO | `VK_FORMAT_D24_UNORM_S8_UINT` (or `D32_SFLOAT`), sampled as a comparison or as `R24_UNORM` (not available as a sampled colour format on all drivers → sample the depth aspect directly) |
| swap chain `R8G8B8A8_UNORM_SRGB` | final present | `VK_FORMAT_B8G8R8A8_SRGB` / `R8G8B8A8_SRGB` surface + non-sRGB internal targets |
| HLSL `cbuffer b : register(b0)` / `c : register(b1)`, `Texture2D : register(t0..t7)`, `SamplerState : register(s0..s2)` | shaders shipped as **HLSL source strings** (`<Code>`), compiled at load with `D3DCompile(entry, ps_5_0/vs_5_0/gs_5_0)` | compile HLSL → SPIR-V offline (DXC with `-fvk-*-shift` / `--shift` to remap register spaces) or at load; entry points are the literal names `"v"`, `"p"`, `"g"` (`Project.cpp:98-99`, `entrypoints[]`/`versions[]`) |
| `D3DCompileCall` loaded from `d3dcompiler_47.dll` | runtime shader compilation | not required if shaders are pre-compiled; otherwise needs a DXC equivalent |

### 9.2 Headless rendering checklist

1. **Load** `release.64k` (or the XML `.apx` for authoring parity). Resolve the stream table,
   then follow `CphxProject::LoadProject`'s read order.
2. **Create** one `CapexRenderTargetCollection`-equivalent: the internal RT, the depth buffer and
   every project render target, all sized by `GetRenderTargetResolution` against the demo base
   resolution computed in §2.1. Generate mip chains for all of them.
3. **Generate texgen textures once** (§2.3) before the first frame, in operator index order, with
   `srand(RandSeed)` per filter pass.
4. **Frame advance**: `frame = floor(song_ms * FrameRate / 1000)` with `song_ms` from an offline
   MVX render (marker table) or from a real clock. For deterministic dumps use the video dumper's
   mapping (§1.8).
5. **Per frame**: clear, run events in sorted order, apply particle/affector integration with the
   fixed 25 Hz accumulator, and finally copy `Target` into the surface with the centred viewport
   from §2.5.
6. **Determinism hazards to control**: `rand()` call sequence (particles, turbulence, noise
   splines, texgen lookup tables), `GenerateMips` filter specifics, HLSL compiler differences, and
   `float` vs `double` in `D3DXMatrixInverse`/`D3DXQuaternionSquad`.
7. **Audio**: the demo cannot advance without a millisecond clock; MVX has no source in the repo,
   so either keep a Windows helper process for audio/markers or reproduce the tick table from the
   `.mvm` (see `docs/spec/music-mvx.md`).

### 9.3 Constants worth hard-coding as validation targets

| constant | value | source |
|---|---|---|
| `StreamCount` | 19 | `Project.h:11-38` |
| `MAX_LIGHT_COUNT` | 8 | `phxEngine.h:13` |
| `MATERIALDATASIZE` | 256 bytes (64 floats) | `phxEngine.h:50` |
| `SCENEDATASIZE` | 1200 bytes | `phxEngine.h:49` |
| `PHXVERTEXFORMATSIZE` | 84 bytes | `phxEngine.h:53` |
| `PARTICLESIZE` | `4*(4+4)` = 32 bytes | `Scene.h:232` |
| `PARTICLEENGINE_FRAMERATE` | 25.0 | `Scene.h:15` |
| `WAVEFORMSPLINENOISEBUFFERSIZE` | 8192 | `phxSpline.h:79` |
| `TEXGEN_MAX_PARENTS` / `TEXGEN_MAX_PARAMS` | 3 / 16 | `Texgen.h:11-12` |
| `SHADERDATALENGTH` | `(4+16)*4` = 80 bytes | `Texgen.h:13` |
| `Spline_Count` | 57 | `Scene.h:17-92` |
| `sizeof(MATERIALVALUE)` | 16 | union of `float[4]` + 2 pointers |
| `sizeof(LIGHTDATA)` | 96 | 6 × float4 |
| Clean Slate | 16:9, **120 fps**, 1 render target, 213 events, 4 render layers, 13 render targets | `cleanslate.apx` / `precalc.64k` |

---

## 10. UNCERTAIN / unverified items

Collected so they can be resolved before implementation depends on them.

1. **`PHX_HAS_SCENE_OBJECT_HIERARCHIES`** — whether the shipped Clean Slate build defines it. It
   changes `CphxObject::GetWorldMatrix()` (parent multiplication) and which matrices
   `UpdateParticles`/`ResetParticles` use for emitters. `PhoenixConfig.h` in the repo is the
   *minimal* config; the real per-release config is generated by the tool
   (`CapexRoot::CreateMinimalConfig`, `apExRoot.cpp:~1900`). **Must be confirmed for the release
   binary being targeted.**
2. **`PHX_VOLUMETRIC_RENDERTARGETS`** and the voxelizer: the exported render-layer byte has no
   voxelizer bit and the loader never sets `CphxRenderLayerDescriptor::VoxelizerLayer`
   (`Project.cpp:326-335`). If the macro is on, that field is read uninitialised. Whether Clean
   Slate uses voxel cone tracing is unresolved.
3. **`PHX_HAS_NON_DEMO_RELATIVE_RENDERTARGETS`** — enables absolute-power-of-two RT descriptors
   (bit 7 clear). All descriptors in `cleanslate.apx` have bit 7 set, so the macro's state is
   unobservable from data.
4. **`PHX_HAS_STANDARD_PARTICLES` / `PHX_HAS_MESH_PARTICLES` / `PHX_HAS_SUBSCENE_PARTICLES` /
   `PHX_HAS_PARTICLE_SORTING`** — all change the particle code paths in `Scene.cpp`
   (indexed vs point-list draws, extra texture, sorting). Not resolvable from the repo config.
5. **`_deleteme_test.64k`** in `MinimalPlayer/Data` has `offsets[0] == 0` where the writer forces
   `StreamCount*4 = 76`, and cannot be parsed correctly by `LoadProject`. Assumed to be a stale artifact from an older
   exporter. **Do not use it as a format reference.**
6. **Event end-frame semantics differ** between runtime (`<`) and the editor preview (`<=`).
   The runtime is assumed normative.
7. **`EVENT_RENDERDEMO` / timeline nesting** is an empty stub; `subroutine` rendering has no caller
   in the release player. The `<event>` type-1 element carries no data, so nothing is lost.
8. **`CphxMaterialPassConstantState::RenderPriority` is `unsigned char`** while
   `CphxRenderDataInstance::RenderPriority` is `int`; the tool's `CphxParameterDataCollector` also
   stores `TS32`. Values > 255 silently wrap. No project data seems to exceed it.
9. **`PARAM_ZFUNCTION` encoding** — the code assigns the union member straight to
   `D3D11_COMPARISON_FUNC`, but `Material.h` also declares an incompatible `COMPARISONFUNCTION`
   enum. Assumed D3D ordering (the union member's type). The XML writes the raw union bytes, so a
   real project can settle it.
10. **Blend `SrcBlendAlpha`/`DestBlendAlpha`/`BlendOp`** are not settable from material parameters:
    they always stay at the default `ONE/ZERO/ADD`. Confirmed by code; noting it because a
    "correct-looking" Vulkan port might assume otherwise.
11. **`PARAM_MESHDATA*`** — declared and exportable but with no runtime effect on the release path
    (`CollectRenderState` has no case). Assumed dead for the player; the tool's editor path uses
    them.
12. **Mixed-size colour attachments in one render layer** — permitted by D3D11 viewport semantics
    but not by Vulkan. Not present in `cleanslate.apx`; unknown for other projects.
13. **Cube-map render targets** — `CubeMap` flag exists and `pFormat & 0xf0` sets
    `CphxRenderTarget::cubeMap`, but no runtime code uses it (the cube-face loop in
    `CphxEvent_RenderScene::Render` is commented out). Assumed unused.
14. **`ZResolution`** on render targets is read but never used. Assumed legacy.
15. **`EVENT_CAMERAOVERRIDE` runs a full `UpdateSceneGraph`** whose render instances are wasted.
    This is a real per-frame cost; a port that skips it would diverge in *timing* but not in
    output. **[VERIFIED as code, UNCERTAIN as intent]** — the extra traversal does have one
    observable side effect: it advances `currMatrix`/`prevMatrix` on that scene's objects an extra
    time per frame, which changes particle interpolation for emitters in that scene.
16. **`CphxTimeline::Render` clears *all* render targets every frame**, even ones a later layer
    clears and draws. A port must keep this (it also resets mips? no — `ClearRenderTargetView`
    only clears mip 0). **[VERIFIED]**
17. **Mip contents after `ClearRenderTargetView`** — D3D clears only the top mip; lower mips keep
    stale content until `GenerateMips` runs. Behaviour on first frame vs later frames therefore
    differs. A port must reproduce or explicitly zero the whole chain.
18. **`GenerateMips` vendor differences** — byte-exact output across GPUs is not guaranteed by
    D3D either.
19. **Uninitialised `MATERIALVALUE` union bytes** for one-byte parameter encodings (`ZMODE`,
    `ZFUNCTION`, `FILLMODE`, `CULLMODE`, `RENDERPRIORITY`) — see §6.4e. The engine reads them as
    multi-byte members. Works in practice; assume zeroed bytes when reimplementing, and verify
    against a reference render if a value ever comes out absurd (e.g. an invalid `CullMode`).
20. **XML vs release value fidelity** — the `.apx` authoring file stores full 16-byte
    `MATERIALVALUE` base64, while `release.64k` stores quantised bytes (§6.4e). A player built on
    the XML path will *not* be numerically identical to the release player for float/colour
    material parameters.

---

## Appendix A — symbol index (function → file:line)

| symbol | location |
|---|---|
| `CphxProject::LoadProject` | `apEx/Phoenix/Project.cpp:262` |
| `CphxProject::Render` | `apEx/Phoenix/Project.cpp:1480` |
| `DoPrecalc` | `apEx/Phoenix/Project.cpp:170` |
| `CreateRenderTarget` / `CreateDepthBuffer` | `apEx/Phoenix/Project.cpp:196` / `:219` |
| `ImportMaterialParams` / `ImportMaterialParamValue` | `apEx/Phoenix/Project.cpp:1485` / `:1501` |
| `ImportTechniqueData` | `apEx/Phoenix/Project.cpp:1556` |
| `ImportClipSpline` / `ImportSpline` | `apEx/Phoenix/Project.cpp:1585` / `:1635` |
| `BuildPassState` / `CollectRenderState` | `apEx/Phoenix/Project.cpp:1839` / `:1873` |
| `CphxTimeline::Render` | `apEx/Phoenix/Timeline.cpp:318` |
| `CphxEvent_RenderScene::Render` | `apEx/Phoenix/Timeline.cpp:152` |
| `CphxEvent_Shadertoy::Render` | `apEx/Phoenix/Timeline.cpp:33` |
| `CphxEvent_ParticleCalc::Render` | `apEx/Phoenix/Timeline.cpp:236` |
| `CphxEvent_CameraShake::Render` / `arand` | `apEx/Phoenix/Timeline.cpp:280` / `:275` |
| `CphxEvent_CameraOverride::Render` | `apEx/Phoenix/Timeline.cpp:142` |
| `CphxScene::UpdateSceneGraph` (2 overloads) | `apEx/Phoenix/Scene.cpp:51` / `:92` |
| `CphxScene::CollectLights` | `apEx/Phoenix/Scene.cpp:101` |
| `CphxScene::Render` | `apEx/Phoenix/Scene.cpp:136` |
| `CphxScene::AddRenderDataInstance` | `apEx/Phoenix/Scene.cpp:205` |
| `CphxObject::TraverseSceneGraph` / `CalculateAnimation` | `apEx/Phoenix/Scene.cpp:229` / `:274` |
| `CphxObject_SubScene::CreateRenderDataInstances` | `apEx/Phoenix/Scene.cpp:338` |
| `CphxObject_ParticleEmitter_CPU::UpdateParticles` | `apEx/Phoenix/Scene.cpp:463` |
| `CphxObject_ParticleEmitter_CPU::SpawnParticle` | `apEx/Phoenix/Scene.cpp:374` |
| `Pivot` / `SortRenderLayer` | `apEx/Phoenix/Scene.cpp:16` / `:41` |
| `CphxRenderDataInstance::Render` | `apEx/Phoenix/RenderLayer.cpp:27` |
| `CphxRenderLayerDescriptor::SetEnvironment` / `GenMipmaps` | `apEx/Phoenix/RenderLayer.cpp:107` / `:151` |
| `GetRenderTargetResolution` | `apEx/Phoenix/RenderLayer.cpp:3` |
| `CphxMaterialParameterBatch::CollectAnimatedData` | `apEx/Phoenix/Material.cpp:23` |
| `CphxMaterialTechnique::CreateRenderDataInstances` | `apEx/Phoenix/Material.cpp:117` |
| `CphxMaterialTechnique::CollectAnimatedData` | `apEx/Phoenix/Material.cpp:168` |
| `CphxMaterial::CreateRenderDataInstances` (2) | `apEx/Phoenix/Material.cpp:197` / `:204` |
| `CphxModelObject_Mesh::CreateRenderDataInstances` | `apEx/Phoenix/Model.cpp:25` |
| `CphxSpline::CalculateValue` | `apEx/Phoenix/phxSpline.cpp:33` |
| `CphxSpline_float16::PostProcess` | `apEx/Phoenix/phxSpline.cpp:143` |
| `PHXTEXTUREFILTER::Render` | `apEx/Phoenix/Texgen.cpp:120` |
| `PHXTEXTUREOPERATOR::Generate` | `apEx/Phoenix/Texgen.cpp:464` |
| `Prepare2dRender` / `SetSamplers` | `apEx/Phoenix/Texgen.cpp:99` / `:92` |
| `InitializePhoenix` | `apEx/Phoenix/phxEngine.cpp:168` |
| `CapexProject::ImportMain` / `ExportToString` | `apEx/Phoenix_Tool/apxProject.cpp:420` / `:721` |
| `CapexProject::ApplyRenderTargets` | `apEx/Phoenix_Tool/apxProject.cpp:1144` |
| `CapexProject::GetFrameToRender` | `apEx/Phoenix_Tool/apxProject.cpp:1291` |
| `CapexProject::UpdateCurrentFrameFromPlayback` | `apEx/Phoenix_Tool/apxProject.cpp:1377` |
| `CapexProject::AcquireTickData` | `apEx/Phoenix_Tool/apxProject.cpp:1418` |
| `CapexRenderTargetCollection::UpdateCollection` | `apEx/Phoenix_Tool/apxProject.cpp:1727` |
| `CphxTimeline_Tool::Render` / `Sort` | `apEx/Phoenix_Tool/Timeline_tool.cpp:207` / `:285` |
| `CphxEvent_Tool::ExportData` / `ImportData` | `apEx/Phoenix_Tool/Timeline_tool.cpp:37` / `:52` |
| `CphxScene_Tool::ExportData` / `ImportData` | `apEx/Phoenix_Tool/Scene_tool.cpp:168` / `:187` |
| `CphxObject_Tool::ExportData` | `apEx/Phoenix_Tool/Scene_tool.cpp:1267` |
| `CphxObjectClip_Tool::ExportData` | `apEx/Phoenix_Tool/Scene_tool.cpp:2055` |
| `CphxClipSpline_Tool::ExportData` | `apEx/Phoenix_Tool/Scene_tool.cpp:2252` |
| `CphxRenderLayerDescriptor_Tool::ExportData` | `apEx/Phoenix_Tool/RenderLayer.cpp:22` |
| `CphxRenderTarget_Tool::ExportData` / `CreateResolutionDescription` | `apEx/Phoenix_Tool/RenderTargets.cpp:88` / `:121` |
| `ExportMinimal` | `apEx/apEx/MinimalExport.cpp:2063` |
| `ExportEvent` | `apEx/apEx/MinimalExport.cpp:1660` |
| `CapexVideoDumper::OnDraw` / `PrepareFrame` | `apEx/apEx/VideoDumper.cpp:258` / `:127` |
| `CWBDemoPreview::OnDraw` | `apEx/apEx/TimelinePreview.cpp:26` |
| `CWBSceneDisplay::OnDraw` | `apEx/apEx/SceneView.cpp:841` |
| `MinimalPlayer` `WinMain` | `apEx/MinimalPlayer/MinimalPlayer.cpp:85` |
| `InitWindow` | `apEx/MinimalPlayer/WindowHandler.cpp:54` |
| `OpenSetupDialog` | `apEx/MinimalPlayer/SetupDialog.cpp:182` |

## Appendix B — real project quick reference (Clean Slate)

| item | value |
|---|---|
| project file | `Projects/Clean Slate/cleanslate.apx` (13.4 MB XML) |
| precalc | `Projects/Clean Slate/Precalc.apx`, packaged as `MinimalPlayer/Data/precalc.64k` |
| song | `Projects/Clean Slate/step010padding.mvm` (985 KB) |
| aspect / fps | 16:9 / **120** |
| render targets | 13 (RT0 = *Backbuffer*) |
| render layers | 4, order: Shadow, Solid, Lighting, Transparent |
| events | 213 (types 0, 2, 3, 4, 6); frame range 0..34125 |
| precalc header decodes to | AspectX=16, AspectY=9, FrameRate=120, RenderTargetCount=1 |
