// Neo-apEx-Player: a Vulkan player for apEx demotool projects (.apx).
//
// Frame model (see docs/DESIGN-player-app.md and docs/spec/engine-pipeline.md §1):
// the original release player derives the frame number from the music position,
//     frame = floor(song_milliseconds / 1000 * framerate)
// and that is the only clock the demo has.  The MVX synthesiser is closed-source
// 32-bit Windows object code with no portable source, so this player cannot
// synthesise the music - but the song's tick timeline is fully recoverable from
// the .mvm header, so the wall clock (src/player/song_clock.cpp) reproduces the
// original timing exactly.
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform/window_sdl.h" // pulls WindowHandler.h, Enums.h, Device.h + SDL
#include "DX11Device.h"
#include "D3Dcompiler.h"
#include "phxEngine.h"
#include "apxPhoenix.h"
#include "apxProject.h"
#include "phxResource.h"

// Declared in Phoenix_Tool/phxResource.cpp; only used for the load-time
// diagnostic below.
extern CArray<CphxResource *> UpdateQueue;
#include "Texgen_tool.h"
#include "Timeline_tool.h"

#include "song_clock.h"
#include "frame_dump.h"
#include "audio_out.h"
#include "song_pcm.h"
#include "RenderTarget.h"  // CphxRenderTarget
#include "rhi/vk_core.h"

// Defined in third_party/apEx/Phoenix/phxEngine.cpp and created by
// D3D11CreateDeviceAndSwapChain (our shim).  The original MinimalPlayer
// presents through this global as well.
extern IDXGISwapChain *SwapChain;
// Defined in Bedrock/CoRE2/DX11Shader.cpp with a file-local typedef; the
// original resolves it by LoadLibraryA("d3dcompiler_47.dll"), which does not
// exist on this platform.  We mirror apxPhoenix.cpp's declaration exactly
// (same trick the original tool uses across those two translation units).
typedef HRESULT(__stdcall d3d_compile_func)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                            const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                            LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                            ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);
extern d3d_compile_func *D3DCompileFunc;

//////////////////////////////////////////////////////////////////////////////
// command line

struct Options
{
  std::string project;
  std::string song;         // explicit .mvm; default: <songfile> from the .apx
  int width = 0;            // 0 = derive from the project's aspect ratio
  int height = 0;
  bool fullscreen = false;
  bool vsync = true;
  bool validate = false;
  long long frames = -1;    // -1 = run until the song ends / window closes
  long long startFrame = 0;
  bool fixedFps = false;    // advance exactly one frame per present
  double fpsOverride = 0;   // 0 = take <Framerate> from the project
  std::vector<phxplayer::FrameDumpRequest> dumps; // --dump-frame N=path.ppm
  std::string music;   // --music FILE: explicit pre-rendered track
  bool noAudio = false; // --no-audio: never open an output device
};

static void Usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s <project.apx> [options]\n"
          "\n"
          "  --song FILE          .mvm to take the timeline from (default: the project's <songfile>)\n"
          "  --width N --height N window size (default: derived from <AspectX>/<AspectY>)\n"
          "  --fullscreen         start fullscreen\n"
          "  --frames N           render N frames and exit (deterministic)\n"
          "  --start-frame N      begin at frame N\n"
          "  --fixed-fps          ignore the clock; advance one frame per present\n"
          "  --fps N              override the project framerate\n"
          "  --no-vsync           present without vsync\n"
          "  --music FILE         play a pre-rendered track (WAV, or the Windows\n"
          "                       tool's .apxsndcache) and drive the clock from it\n"
          "  --no-audio           never open an audio device\n"
          "  --validate           enable Vulkan validation layers\n"
          "  --dump-frame N=FILE  write frame N to FILE as binary PPM (repeatable)\n",
          argv0);
}

static bool ParseArgs(int argc, char **argv, Options &o)
{
  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    auto next = [&](const char *what) -> const char * {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "%s needs a value\n", what);
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h")
    {
      Usage(argv[0]);
      exit(0);
    }
    else if (a == "--song") o.song = next("--song");
    else if (a == "--width") o.width = atoi(next("--width"));
    else if (a == "--height") o.height = atoi(next("--height"));
    else if (a == "--fullscreen") o.fullscreen = true;
    else if (a == "--vsync") o.vsync = true;
    else if (a == "--no-vsync") o.vsync = false;
    else if (a == "--music") o.music = next("--music");
    else if (a == "--no-audio") o.noAudio = true;
    else if (a == "--validate") o.validate = true;
    else if (a == "--frames") o.frames = atoll(next("--frames"));
    else if (a == "--start-frame") o.startFrame = atoll(next("--start-frame"));
    else if (a == "--fixed-fps") o.fixedFps = true;
    else if (a == "--fps") o.fpsOverride = atof(next("--fps"));
    else if (a == "--dump-frame")
    {
      const char *spec = next("--dump-frame");
      const char *eq = strchr(spec, '=');
      if (!eq)
      {
        fprintf(stderr, "--dump-frame expects N=FILE\n");
        return false;
      }
      phxplayer::FrameDumpRequest r;
      r.frame = atoll(std::string(spec, eq - spec).c_str());
      r.path = eq + 1;
      o.dumps.push_back(r);
    }
    else if (!a.empty() && a[0] == '-')
    {
      fprintf(stderr, "unknown option '%s'\n", a.c_str());
      Usage(argv[0]);
      return false;
    }
    else if (o.project.empty()) o.project = a;
    else
    {
      fprintf(stderr, "unexpected extra argument '%s'\n", a.c_str());
      return false;
    }
  }
  if (o.project.empty())
  {
    Usage(argv[0]);
    return false;
  }
  return true;
}

// <songfile> in the .apx is a Windows-style path relative to the project
// (".\\step010padding.mvm"), so normalise and resolve it next to the .apx.
static std::string ResolveSongPath(const std::string &apxPath, const std::string &declared)
{
  std::string s = declared;
  for (char &c : s)
    if (c == '\\') c = '/';
  if (!s.empty() && s[0] == '/') return s;
  if (s.size() > 1 && s[1] == ':') return s; // absolute Windows path: leave it
  if (s.rfind("./", 0) == 0) s = s.substr(2);
  size_t slash = apxPath.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? std::string() : apxPath.substr(0, slash + 1);
  return dir + s;
}

int main(int argc, char **argv)
{
  Options opt;
  if (!ParseArgs(argc, argv, opt)) return 2;

  phxvk::LogInfo("Neo-apEx-Player: %s", opt.project.c_str());
  // The vendored engine only requests validation under _DEBUG; forward our flag.
  phxvk::vkRequestValidation(opt.validate);

  // ---------------------------------------------------------------------
  // window + device
  //
  // CCoreWindowParameters deliberately does NOT create the device (the Win32
  // original leaked an implicit one); the player owns it.
  const int defaultW = 1280;
  const int defaultH = 720;
  const int w = opt.width > 0 ? opt.width : defaultW;
  const int h = opt.height > 0 ? opt.height : defaultH;

  // Order matters, and it is not the obvious one:
  //   * CCoreWindowHandlerWin::Initialize() copies params.Device into the
  //     handler and *fails* when it is NULL ("[init] Device object is NULL
  //     during init."), but the device's own Initialize() needs the window's
  //     handle to create the swap chain.
  //   * So the device object has to exist (uninitialized) before the window is
  //     created.  In the Win32 original CCoreWindowParameters::Initialize
  //     side-effected `new CCore()`, which leaked one device per window; here
  //     the player owns the single device and just hands over the pointer.
  CCoreDX11Device device;

  // The Win32 player's only control, reproduced exactly: ESC ends the demo
  // (apEx/MinimalPlayer/WindowHandler.cpp:28-30, `WM_KEYDOWN` + `VK_ESCAPE` ->
  // `phxDone = true`).  There is no pause or seek in the original either - it is
  // a linear music video - so this is the whole control set, and anything more
  // would be invention rather than a port.
  class PlayerWindow : public CCoreWindowHandlerWin
  {
  public:
    bool quitRequested = false;

  protected:
    LRESULT WindowProc(UINT uMsg, WPARAM wParam, LPARAM lParam) override
    {
      if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE) quitRequested = true;
      return CCoreWindowHandlerWin::WindowProc(uMsg, wParam, lParam);
    }
  };

  PlayerWindow window;
  CCoreWindowParameters params((HINSTANCE)0, opt.fullscreen ? 1 : 0, w, h,
                               (TCHAR *)"Neo-apEx-Player", (HICON)0, false, false);
  params.Device = &device;

  if (!window.Initialize(params))
  {
    phxvk::LogError("window initialization failed");
    return 1;
  }
  if (!device.Initialize(&window, 0))
  {
    phxvk::LogError("device initialization failed");
    return 1;
  }
  phxvk::LogInfo("device: %dx%d", window.GetXRes(), window.GetYRes());

  // The engine keeps its own global `SwapChain` (Phoenix/phxEngine.cpp:84) and
  // presents through it (Phoenix/Project.cpp DoPrecalc, the MinimalPlayer main
  // loop).  CCoreDX11Device has always owned a same-named private member, so
  // publish ours into the global the engine actually reads.
  SwapChain = device.GetSwapChain();
  if (!SwapChain)
  {
    phxvk::LogError("the device created no swap chain");
    return 1;
  }


  // ---------------------------------------------------------------------
  // Phoenix bring-up
  //
  // The vendored DX11Shader.cpp looks D3DCompile up with
  // LoadLibraryA("d3dcompiler_47.dll") / GetProcAddress.  There is no such DLL
  // on this platform: our D3D11 shim *is* the compiler and exports D3DCompile
  // directly, so point the engine's function pointer at it.  PhoenixInitTool
  // copies this into D3DCompileCall, which phxEngine and Project.cpp use.
  D3DCompileFunc = (d3d_compile_func *)&D3DCompile;
  PhoenixInitTool(device.GetDevice(), device.GetDeviceContext());

  // The back buffer the timeline composites the finished frame into.
  //
  // CphxTimeline::Render() finishes with
  //     OMSetRenderTargets(1, &phxBackBufferView, NULL); PSSetShaderResources(0, 1, &Target->View); Draw(6, 0);
  // (Phoenix/Timeline.cpp:371-375).  `phxBackBufferView` is a Phoenix global
  // that the release player owns - apEx/MinimalPlayer/WindowHandler.cpp creates
  // it from swap-chain buffer 0 - and nothing in the engine sets it, so leaving
  // it null makes the final blit target nothing and every frame is black.
  // ScreenX/ScreenY (the viewport it blits into) come from the same place.
  {
    ID3D11Texture2D *backBuffer = nullptr;
    HRESULT hr = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer);
    if (FAILED(hr) || !backBuffer)
    {
      phxvk::LogError("SwapChain::GetBuffer(0) failed; nothing can be presented");
      return 1;
    }
    // Create the view with an _SRGB format over the UNORM swap-chain image.
    //
    // The engine works in linear light: the timeline composites into an
    // R16G16B16A16_FLOAT target and the final blit passes those values straight
    // through.  Writing linear numbers into a UNORM back buffer and presenting
    // them makes the whole demo look far too dark - measured against the
    // original video, the background came out at RGB(0,2,4) instead of
    // RGB(0,22,32), and applying the sRGB transfer function to the captured
    // frame reproduces the reference to within 1-3 levels out of 255.  An sRGB
    // render-target view makes the hardware do that conversion on write, which
    // is what the D3D11 build had.
    D3D11_RENDER_TARGET_VIEW_DESC srgbView;
    memset(&srgbView, 0, sizeof(srgbView));
    srgbView.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    srgbView.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = phxDev->CreateRenderTargetView(backBuffer, &srgbView, &phxBackBufferView);
    if (FAILED(hr) || !phxBackBufferView)
    {
      // Not all drivers allow an sRGB view over the swap-chain image; fall back
      // to the plain format and say so, because the picture will be dark.
      phxvk::LogWarn("the back buffer does not accept an sRGB view; presenting linear values "
                     "will look darker than the original");
      hr = phxDev->CreateRenderTargetView(backBuffer, nullptr, &phxBackBufferView);
    }
    backBuffer->Release();
    if (FAILED(hr) || !phxBackBufferView)
    {
      phxvk::LogError("could not create the back-buffer render target view");
      return 1;
    }
    phxvk::LogInfo("back buffer view: %p", (void *)phxBackBufferView);
  }

  // ---------------------------------------------------------------------
  // load the project

  CString apxPath(opt.project.c_str());
  Project.Import(apxPath, (HWND)0);
  phxvk::LogInfo("project imported: %s", opt.project.c_str());

  // ---------------------------------------------------------------------
  // Create the demo's render targets and point the timeline at them.
  //
  // This is the piece the tool UI used to do.  CphxTimeline::Render() blits to
  // the back buffer only when an event sets `Target`
  // (Phoenix/Timeline.cpp:355), and the events' Target pointers are assigned by
  // CapexProject::ApplyRenderTargets() from the *tool-side* render target list,
  // backed by a CapexRenderTargetCollection.  Without this the timeline renders
  // into nothing and every frame stays black.
  CapexRenderTargetCollection *rtCollections =
      Project.SpawnRenderTargetCollection(&device, window.GetXRes(), window.GetYRes(), true);
  Project.ApplyRenderTargets(rtCollections);

  // CphxTimeline::EventCount starts at 0 (its constructor) and is normally set
  // per frame by CphxTimeline_Tool::Render from Project.LastPassToDraw.  With it
  // left at 0 the timeline iterates over no events at all, which is the other
  // half of why every frame came out black.
  Project.Timeline->Timeline->EventCount = (unsigned short)Project.Timeline->Events.NumItems();
  phxvk::LogInfo("timeline: %d event(s)", (int)Project.Timeline->Timeline->EventCount);

  // The viewport the timeline blits the finished frame into, normally taken
  // from the release setup data (Phoenix/Project.cpp:286-287).
  Project.Timeline->Timeline->ScreenX = window.GetXRes();
  Project.Timeline->Timeline->ScreenY = window.GetYRes();

  {
    // ApplyRenderTargets() only gives an event a Target when its <TargetRT> GUID
    // resolves to a render target; without one the timeline has nothing to
    // composite and the frame stays black.
    int withTarget = 0, withRt = 0;
    for (TS32 i = 0; i < Project.Timeline->Events.NumItems(); i++)
    {
      if (Project.Timeline->Events[i]->Event->Target) withTarget++;
      if (Project.Timeline->Events[i]->TargetID.GetString()[0] &&
          strcmp(Project.Timeline->Events[i]->TargetID.GetString(), "NONENONENONENONENONENONENONENONE") != 0)
        withRt++;
    }
    phxvk::LogInfo("events: %d have a resolved Target of %d total (%d declare a TargetRT)",
                   withTarget, Project.Timeline->Events.NumItems(), withRt);
    // The demo's timeline is driven entirely by these ranges; an ENDDEMO event
    // (type 0) ending the run early means they were read wrong.
    for (TS32 i = 0; i < Project.Timeline->Events.NumItems() && i < 6; i++)
      phxvk::LogInfo("  event %d: type %d frames [%d, %d) pass %d", (int)i,
                     (int)Project.Timeline->Events[i]->Type,
                     (int)Project.Timeline->Events[i]->Event->StartFrame,
                     (int)Project.Timeline->Events[i]->Event->EndFrame,
                     (int)Project.Timeline->Events[i]->Pass);
  }

  if (rtCollections && rtCollections->RenderTargets.NumItems() > 0)
    phxvk::LogInfo("render targets: %d collection target(s), timeline has %d",
                   rtCollections->RenderTargets.NumItems(), (int)Project.Timeline->Timeline->RenderTargetCount);
  else
    phxvk::LogError("no render targets were created; the timeline will not composite anything");

  // ---------------------------------------------------------------------
  // The scratch render target the shadertoy events copy into.
  //
  // CphxEvent_Shadertoy::Render (Phoenix/Timeline.cpp:52-71) breaks the
  // "sample the target you are rendering into" hazard by copying the target to
  // a scratch one and sampling that instead.  CphxProject::LoadProject()
  // creates `phxInternalRenderTarget` on the binary/precalc path; the .apx path
  // (CapexProject) never did, so it stayed null and Timeline.cpp dereferenced
  // it.  The player owns the .apx path, so it creates it here, with the same
  // format and flags the original used (R16G16B16A16_FLOAT, mip chain
  // generated, bound as both render target and shader resource).
  {
    extern CphxRenderTarget *phxInternalRenderTarget;

    int rtW = window.GetXRes() > 0 ? window.GetXRes() : w;
    int rtH = window.GetYRes() > 0 ? window.GetYRes() : h;
    // Must match the timeline target it is copied from (CopyResource requires
    // identical dimensions).
    if (Project.Timeline && Project.Timeline->Timeline && Project.Timeline->Timeline->RenderTargetCount > 0 &&
        Project.Timeline->Timeline->RenderTargets[0] &&
        Project.Timeline->Timeline->RenderTargets[0]->XRes > 0)
    {
      rtW = Project.Timeline->Timeline->RenderTargets[0]->XRes;
      rtH = Project.Timeline->Timeline->RenderTargets[0]->YRes;
    }

    phxInternalRenderTarget = new CphxRenderTarget;
    phxInternalRenderTarget->cubeMap = false;
    phxInternalRenderTarget->XRes = rtW;
    phxInternalRenderTarget->YRes = rtH;

    D3D11_TEXTURE2D_DESC tex;
    memset(&tex, 0, sizeof(tex));
    tex.Width = rtW;
    tex.Height = rtH;
    tex.MipLevels = 0;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    tex.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    D3D11_RENDER_TARGET_VIEW_DESC rtv;
    memset(&rtv, 0, sizeof(rtv));
    rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;

    phxDev->CreateTexture2D(&tex, nullptr, &phxInternalRenderTarget->Texture);
    phxDev->CreateShaderResourceView(phxInternalRenderTarget->Texture, nullptr, &phxInternalRenderTarget->View);
    phxDev->CreateRenderTargetView(phxInternalRenderTarget->Texture, &rtv, &phxInternalRenderTarget->RTView);

    if (!phxInternalRenderTarget->Texture || !phxInternalRenderTarget->View || !phxInternalRenderTarget->RTView)
      phxvk::LogError("could not create the internal render target (%dx%d)", rtW, rtH);
    else
      phxvk::LogInfo("internal render target: %dx%d", rtW, rtH);
  }

  // ---------------------------------------------------------------------
  // song clock

  CphxTimeline *timeline = Project.Timeline ? Project.Timeline->Timeline : nullptr;
  if (!timeline)
  {
    phxvk::LogError("project has no timeline");
    return 1;
  }

  double fps = opt.fpsOverride > 0 ? opt.fpsOverride : (double)timeline->FrameRate;
  if (fps <= 0) fps = 120.0;

  std::string songPath = opt.song;
  if (songPath.empty())
  {
    // CapexProject keeps the parsed <songfile>; the accessor name differs from
    // the member, so go through the public one.
    songPath = ResolveSongPath(opt.project, Project.SongFile.GetPointer());
  }

  phxplayer::MvmInfo song;
  if (!songPath.empty())
  {
    song = phxplayer::ParseMvmHeader(songPath.c_str());
    if (song.valid)
      phxvk::LogInfo("song: %s (%.2f s, %u ticks, %u bpm / %u tpb)", songPath.c_str(),
                     song.totalMs / 1000.0, song.numMasterTicks, song.initialBpm, song.initialTpb);
    else
      phxvk::LogWarn("song '%s' unusable (%s); running on the wall clock only", songPath.c_str(), song.error.c_str());
  }
  else
  {
    phxvk::LogWarn("the project declares no <songfile>; running on the wall clock only");
  }

  phxplayer::SongClock clock;
  clock.Configure(song, fps);

  // ---------------------------------------------------------------------
  // Music.
  //
  // The MVX synthesiser cannot be ported (closed-source 32-bit Windows object
  // code), but the original tool renders the whole song to memory up front and
  // caches it in a fully specified container (docs/spec/music-mvx.md §1.6), so
  // a pre-rendered track is all that is needed for real playback: load it, hand
  // it to SDL, and take the clock from the device's own play cursor - which is
  // exactly what the DirectSound path did via GetCurrentPosition().
  phxplayer::AudioOut audio;
  phxplayer::SongPcm pcm;
  bool audioHandedOver = false;
  if (!opt.noAudio)
  {
    std::string searched;
    if (!opt.music.empty())
      pcm = phxplayer::LoadSongPcm(opt.music.c_str());
    else if (!songPath.empty())
      pcm = phxplayer::FindSongPcm(songPath.c_str(), &searched);

    if (pcm.valid)
    {
      std::string err;
      if (audio.Open(pcm.sampleRate, pcm.channels, &err))
      {
        if (audio.SetSource(pcm.samples.data(), pcm.samples.size()))
        {
          audio.Resume();
          clock.SetAudioClock(true);
          phxvk::LogInfo("music: %s (%s, %.2f s, %u Hz, %d ch, %zu tick marker(s))",
                         pcm.source.c_str(), pcm.format.c_str(), pcm.DurationMs() / 1000.0,
                         pcm.sampleRate, pcm.channels, pcm.markers.size());
          if (!pcm.error.empty()) phxvk::LogWarn("music: %s", pcm.error.c_str());
        }
        else
        {
          phxvk::LogError("music: %s", audio.Error().c_str());
        }
      }
      else
      {
        phxvk::LogWarn("music: %s; continuing on the wall clock", err.c_str());
      }
    }
    else if (!opt.music.empty())
    {
      phxvk::LogWarn("music: %s", pcm.error.c_str());
    }
    else
    {
      phxvk::LogWarn("no pre-rendered music next to '%s'.  The MVX synthesiser exists only as "
                     "closed-source 32-bit Windows code, so this player cannot render the song "
                     "itself; run the Windows tool once to produce SoundCache/<hash>.apxsndcache, "
                     "or pass --music FILE.wav.  Playback continues on the wall clock, which "
                     "reproduces the original frame timing exactly.",
                     songPath.empty() ? "(no song)" : songPath.c_str());
      if (!searched.empty()) phxvk::LogInfo("music: looked for %s", searched.c_str());
    }
  }
  if (opt.fixedFps)
  {
    clock.SetFixedStep(1);
    clock.SetFrame(opt.startFrame);
  }
  // NOTE: the clock is deliberately NOT started here.  Loading the project and
  // generating its GPU resources takes seconds, and the original player's clock
  // is the music position - which does not advance until playback begins.  A
  // clock started before generation would make the demo begin thousands of
  // frames in (the first run of this player skipped straight to frame ~1200).

  phxvk::LogInfo("framerate: %.3f fps%s", fps, song.valid ? "" : " (no song)");

  // ---------------------------------------------------------------------
  // build the GPU resources the timeline needs
  //
  // CphxTimeline_Tool::Render refuses to draw a frame whose events are not
  // ContentReady() - it only re-arms the generator.  So drain the queue to
  // completion up front, exactly as apEx/apEx/apEx.cpp:250-256 drains it per
  // frame in the tool.
  {
    // Drive on the queue's own length, not on ProcessGeneratorQueue()'s return
    // value: that counts the steps taken (so it is > 0 even on the call that
    // drains the queue), which made this loop spin.
    int rounds = 0;
    while (UpdateQueue.NumItems() > 0)
    {
      ProcessGeneratorQueue(100, &device);
      if (rounds < 2)
        phxvk::LogInfo("generator round %d: %d item(s) queued, head type %d", rounds + 1, UpdateQueue.NumItems(),
                       UpdateQueue.NumItems() ? (int)UpdateQueue[0]->GetType() : -1);
      if (++rounds > 100000)
      {
        phxvk::LogError("resource generation did not converge; %d item(s) still queued, head type %d",
                        UpdateQueue.NumItems(), UpdateQueue.NumItems() ? (int)UpdateQueue[0]->GetType() : -1);
        break;
      }
    }
    phxvk::LogInfo("resource generation: %d rounds, %d item(s) left", rounds, UpdateQueue.NumItems());
  }

  // ---------------------------------------------------------------------
  // main loop

  if (!opt.fixedFps)
  {
    // Start() resets the song position, so the seek has to come after it -
    // calling SetFrame first made --start-frame a silent no-op.
    clock.Start();
    if (opt.startFrame > 0) clock.SetFrame(opt.startFrame);
  }

  // Report any event that ends the demo, so an early stop is explained rather
  // than mysterious.
  {
    int ended = 0;
    for (TS32 i = 0; i < Project.Timeline->Events.NumItems(); i++)
      if (Project.Timeline->Events[i]->Type == EVENT_ENDDEMO)
      {
        phxvk::LogInfo("  ENDDEMO event %d: frames [%d, %d)", (int)i,
                       (int)Project.Timeline->Events[i]->Event->StartFrame,
                       (int)Project.Timeline->Events[i]->Event->EndFrame);
        ended++;
      }
    phxvk::LogInfo("end-demo events: %d", ended);
  }

  long long rendered = 0;
  int exitCode = 0;
  int fixedDiagLeft = 3;
  long long lastLoggedFrame = -1;
  while (!phxDone)
  {
    (void)0;
    if (!window.HandleMessages())
    {
      phxvk::LogInfo("window requested close at frame %lld", (long long)clock.Frame());
      break;
    }
    if (window.quitRequested)
    {
      phxvk::LogInfo("ESC at frame %lld", (long long)clock.Frame());
      break;
    }
    // DeviceOK() is false while the window is inactive and the engine's own
    // frame limiter is throttling - that is normal Win32 behaviour (it means
    // "skip this frame"), not a lost device.  Treating it as fatal stopped the
    // player after a minute or two of unattended running.
    if (!window.DeviceOK()) continue;

    // Audio-clock mode: the device's play cursor is the song position, exactly
    // as GetCurrentPosition() was in the DirectSound path.
    if (audio.IsOpen())
    {
      audio.Pump(); // keep the device fed; it only ever holds ~250 ms
      const double audioMs = audio.PositionMs();
      clock.SetAudioPositionMs(audioMs);
      // A track shorter than the demo (a hand-rendered excerpt, say) would
      // otherwise freeze the timeline when it ends.  Hand over to the wall clock
      // at that point instead of stalling.
      if (!audioHandedOver && audio.SourceExhausted() && audioMs >= pcm.DurationMs() - 1.0)
      {
        audioHandedOver = true;
        clock.SetAudioClock(false);
        clock.SeekMs(audioMs);
        phxvk::LogWarn("music ended after %.2f s; continuing on the wall clock", audioMs / 1000.0);
      }
    }

    float frame = (float)clock.Frame();
    if (song.valid && clock.Finished()) break;

    // Call the timeline directly with tool=false.
    //
    // CphxTimeline_Tool::Render() passes tool=true, and the engine uses that
    // flag to skip BOTH the back-buffer clear and the final composite blit
    // (Timeline.cpp:337 and :355) - the editor draws the target into its own
    // preview panel instead.  A player wants the engine's own present path, so
    // it drives CphxTimeline::Render(frame, tool=false, subroutine=false); the
    // per-frame work the tool wrapper did (re-arming the generator queue) is
    // done explicitly below.
    // The per-frame work CphxTimeline_Tool::Render() does before it draws
    // (Timeline_tool.cpp:212-247).  It is not optional: ForceUpdateContent() is
    // what builds a scene's render-data instances, and without it the scene
    // events draw nothing at all and the target stays zero.
    {
      CphxTimeline_Tool *tt = Project.Timeline;
      for (TS32 i = 0; i < tt->Events.NumItems(); i++)
      {
        CphxEvent_Tool *e = tt->Events[i];
        if (!e->Event) continue;
        if (e->Event->StartFrame <= frame && frame < e->Event->EndFrame)
        {
          if (!e->ContentReady()) e->RequestContent();

          if (e->Type == EVENT_RENDERSCENE)
          {
            CphxEvent_RenderScene_Tool *rs = (CphxEvent_RenderScene_Tool *)e;
            if (rs->Scene) rs->Scene->ForceUpdateContent();
          }
          else if (e->Type == EVENT_SHADERTOY)
          {
            ((CphxEvent_Shadertoy_Tool *)e)->UpdateMaterialState();
          }
        }
      }
    }

    Project.Timeline->Timeline->EventCount = (unsigned short)Project.Timeline->Events.NumItems();
    Project.Timeline->Timeline->Render(frame, false, false);
    if (fixedDiagLeft > 0)
    {
      fixedDiagLeft--;
      phxvk::LogInfo("frame %lld: timeline target %p", (long long)frame, (void *)Project.Timeline->Timeline->Target);
    }

    // Events can activate resources that were not needed at load time; the
    // timeline re-arms them, so service the queue every frame.  It is cheap
    // when empty.
    ProcessGeneratorQueue(20, &device);

    // Dump before Present: the back buffer still holds what the timeline blitted
    // into it.
    // Match on the iteration index, not on the clock-derived frame number: with
    // a real-time clock a given frame can be skipped entirely between two
    // presents, which made --dump-frame silently do nothing.  Use --fixed-fps
    // for a run where the frame number and the iteration index are the same.
    for (auto &d : opt.dumps)
    {
      if (d.frame == rendered)
      {
        std::string err;
        // Dump the timeline's composite target *and* the back buffer: if the
        // target has content but the back buffer does not, the engine's final
        // blit is the problem rather than the scene rendering.
        if (Project.Timeline->Timeline->Target && Project.Timeline->Timeline->Target->Texture)
        {
          std::string rtPath = d.path + ".target.ppm";
          if (!phxplayer::DumpTextureToPPM(Project.Timeline->Timeline->Target->Texture, rtPath.c_str(), &err))
            phxvk::LogError("--dump-frame %lld (target) failed: %s", d.frame, err.c_str());
        }
        else
        {
          phxvk::LogWarn("--dump-frame %lld: the timeline has no target texture", d.frame);
        }
        if (!phxplayer::DumpBackBufferToPPM(d.path.c_str(), &err))
          phxvk::LogError("--dump-frame %lld failed: %s", d.frame, err.c_str());
      }
    }

    device.ForceStateReset();
    if (SwapChain) SwapChain->Present(opt.vsync ? 1 : 0, 0);

    if (opt.fixedFps) clock.AdvanceFixedStep();
    else clock.Update();

    rendered++;
    if (opt.frames >= 0 && rendered >= opt.frames) break;
    if (phxDone) phxvk::LogInfo("the timeline ended the demo at frame %lld", (long long)clock.Frame());
    if (song.valid && clock.Finished())
    {
      phxvk::LogInfo("song finished at frame %lld", (long long)clock.Frame());
      break;
    }
  }

  if (audio.IsOpen())
    phxvk::LogInfo("audio: played %.3f s of %.3f s (clock is the device play cursor)",
                   audio.PositionMs() / 1000.0, pcm.DurationMs() / 1000.0);

  phxvk::LogInfo("rendered %lld frames, exiting", rendered);

  // Release what the player itself created before the engine's device runs its
  // leak report, so a normal exit is quiet.
  if (rtCollections) Project.DeleteRenderTargetCollection(rtCollections);

  // Exit without unwinding the engine's object graph.
  //
  // The apEx engine was written for an application that ends by falling off the
  // end of main / ExitProcess, and its teardown order is not sound for a normal
  // C++ unwind: after the device has released its resources, destroying the
  // still-registered project objects walks freed memory inside the engine
  // (CphxMaterialRenderPass_Tool's destructor) and crashes *after* the last
  // frame has already been rendered and presented.  Rendering, frame dumping
  // and the file/log flush below are all complete by this point, and the
  // original release player likewise terminates the process rather than
  // unwinding, so this is deliberate rather than a workaround for a bug in the
  // port.  Use --frames with a debugger if you want to inspect teardown.
  fflush(nullptr);
  std::_Exit(exitCode);
}
