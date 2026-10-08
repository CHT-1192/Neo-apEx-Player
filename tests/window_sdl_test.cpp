// Standalone smoke test for the SDL3 window layer (src/platform/window_sdl.cpp).
//
// It creates a real CCoreWindowHandlerWin through CCoreWindowParameters, opens
// a Vulkan-capable window sized from those parameters, pumps the message loop
// for about a second while printing the resolution, every keyboard/mouse message
// the handler produces, and the modifier-key state, and then shuts down.  Exit
// code 0 means the whole path worked (window created, device initialised from the
// engine handle, messages translated, clean destruction).
//
// This file contains the test itself plus PhxTestDevice, a CCoreDevice mock that
// records what the window layer asks it to do.  The engine symbols the test
// would otherwise have to link (CCoreDevice's out-of-line members, CString's
// destructor, the engine logger) are doubled in
// tests/window_sdl_test_link_doubles.cpp, which explains each of them.
#include "window_sdl.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

//////////////////////////////////////////////////////////////////////////////
// instrumentation

namespace
{

class PhxTestWindowHandler : public CCoreWindowHandlerWin
{
public:
  TS32 ResizeCalls = 0;
  TS32 MessageCount = 0;
  TS32 CloseMessages = 0;
  TS32 KeyDownMessages = 0;
  TS32 KeyUpMessages = 0;
  TS32 CharMessages = 0;
  TS32 MouseMoveMessages = 0;
  TS32 MouseButtonMessages = 0;
  TS32 WheelMessages = 0;
  TS32 PaintMessages = 0;

  // Last translated values, so the test can assert on the translation tables.
  TS32 LastKeyDownVK = 0;
  TS32 LastCharCodePoint = 0;

protected:
  virtual void HandleResize()
  {
    ++ResizeCalls;
    CCoreWindowHandlerWin::HandleResize();
    printf("[handler] HandleResize() -> %dx%d (client %dx%d)\n", (int)XRes, (int)YRes,
           (int)(ClientRect.x2 - ClientRect.x1), (int)(ClientRect.y2 - ClientRect.y1));
    fflush(stdout);
  }

  virtual LRESULT WindowProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
  {
    ++MessageCount;

    switch (uMsg)
    {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        ++KeyDownMessages;
        LastKeyDownVK = (TS32)wParam;
        printf("[event] %s vk=0x%02X scan=0x%02X mods=%d\n", uMsg == WM_SYSKEYDOWN ? "WM_SYSKEYDOWN" : "WM_KEYDOWN",
               (unsigned)wParam, (unsigned)((lParam >> 16) & 0xFF),
               (int)((lParam >> 29) & 1));
        break;

      case WM_KEYUP:
      case WM_SYSKEYUP:
        ++KeyUpMessages;
        printf("[event] %s vk=0x%02X\n", uMsg == WM_SYSKEYUP ? "WM_SYSKEYUP" : "WM_KEYUP", (unsigned)wParam);
        break;

      case WM_CHAR:
        ++CharMessages;
        LastCharCodePoint = (TS32)wParam;
        printf("[event] WM_CHAR U+%04X ('%c')\n", (unsigned)wParam,
               (wParam >= 32 && wParam < 127) ? (char)wParam : '?');
        break;

      case WM_MOUSEMOVE:
        ++MouseMoveMessages;
        if (MouseMoveMessages <= 5 || (MouseMoveMessages % 50) == 0)
          printf("[event] WM_MOUSEMOVE #%d at (%d,%d)\n", (int)MouseMoveMessages, (int)(short)LOWORD(lParam),
                 (int)(short)HIWORD(lParam));
        break;

      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_RBUTTONDBLCLK:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MBUTTONDBLCLK:
        ++MouseButtonMessages;
        printf("[event] mouse message 0x%03X at (%d,%d)\n", (unsigned)uMsg, (int)(short)LOWORD(lParam),
               (int)(short)HIWORD(lParam));
        break;

      case WM_MOUSEWHEEL:
        ++WheelMessages;
        printf("[event] WM_MOUSEWHEEL delta=%d\n", (int)GET_WHEEL_DELTA_WPARAM(wParam));
        break;

      case WM_PAINT:
        ++PaintMessages; // subclasses that render from inside the pump would draw here
        break;

      case WM_CLOSE:
        ++CloseMessages;
        printf("[event] WM_CLOSE\n");
        break;

      default:
        break;
    }

    fflush(stdout);
    return CCoreWindowHandlerWin::WindowProc(uMsg, wParam, lParam);
  }
};

// A device that records the window layer's calls.  Everything outside the
// window contract is a no-op returning failure - this is a test double, not a
// renderer.
class PhxTestDevice : public CCoreDevice
{
public:
  CCoreWindowHandler *Handler = nullptr;
  TU32 SeenHandle = 0;
  TS32 SeenXRes = 0;
  TS32 SeenYRes = 0;
  TBOOL SeenFullScreen = false;
  TS32 ResizeCalls = 0;
  TS32 FullScreenModeCalls = 0;
  TBOOL Windowed = true;

  virtual COREDEVICEAPI GetAPIType() { return COREAPI_DX11; }

  virtual TBOOL Initialize(CCoreWindowHandler *window, const TS32 AALevel = 0)
  {
    (void)AALevel;
    Handler = window;
    SeenHandle = window->GetHandle();
    SeenXRes = window->GetXRes();
    SeenYRes = window->GetYRes();
    SeenFullScreen = window->GetInitParameters().FullScreen;

    printf("[device] Initialize(): handle=0x%08X res=%dx%d fullscreen=%d\n", (unsigned)SeenHandle, (int)SeenXRes,
           (int)SeenYRes, (int)SeenFullScreen);
    fflush(stdout);
    return true;
  }

  virtual TBOOL DeviceOk() { return true; }
  virtual TBOOL IsWindowed() { return Windowed; }

  virtual void Resize(const TS32 xr, const TS32 yr)
  {
    ++ResizeCalls;
    printf("[device] Resize(%d, %d)\n", (int)xr, (int)yr);
    fflush(stdout);
  }

  virtual void SetFullScreenMode(const TBOOL fullScreen, const TS32 xr, const TS32 yr)
  {
    ++FullScreenModeCalls;
    Windowed = !fullScreen;
    printf("[device] SetFullScreenMode(%d, %d, %d)\n", (int)fullScreen, (int)xr, (int)yr);
    fflush(stdout);
  }

  // ---- not exercised by a window test -------------------------------------
protected:
  virtual void ResetPrivateResources() {}
  virtual TBOOL InitAPI(const TU32, const TBOOL, const TS32, const TS32, const TS32, const TS32) { return false; }
  virtual TBOOL ApplyRenderState(const CORESAMPLER, const CORERENDERSTATE, const CORERENDERSTATEVALUE) { return false; }
  virtual TBOOL SetNoVertexBuffer() { return false; }
  virtual TBOOL CommitRenderStates() { return false; }

public:
  virtual CCoreTexture2D *CreateTexture2D(const TS32, const TS32, const TU8 *, const TS8, const COREFORMAT, const TBOOL) { return nullptr; }
  virtual CCoreTexture2D *CreateTexture2D(const TU8 *, const TS32) { return nullptr; }
  virtual CCoreTexture2D *CopyTexture(CCoreTexture2D *) { return nullptr; }
  virtual CCoreVertexBuffer *CreateVertexBuffer(const TU8 *, const TS32) { return nullptr; }
  virtual CCoreVertexBuffer *CreateVertexBufferDynamic(const TS32) { return nullptr; }
  virtual CCoreIndexBuffer *CreateIndexBuffer(const TS32, const TS32) { return nullptr; }
  virtual CCoreVertexFormat *CreateVertexFormat(const CArray<COREVERTEXATTRIBUTE> &, CCoreVertexShader *) { return nullptr; }
  virtual TBOOL SetRenderTarget(CCoreTexture2D *) { return false; }
  virtual TBOOL SetViewport(CRect) { return false; }
  virtual CCoreVertexShader *CreateVertexShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCorePixelShader *CreatePixelShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCoreVertexShader *CreateVertexShaderFromBlob(TU8 *, TS32) { return nullptr; }
  virtual CCorePixelShader *CreatePixelShaderFromBlob(TU8 *, TS32) { return nullptr; }
  virtual CCoreGeometryShader *CreateGeometryShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCoreDomainShader *CreateDomainShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCoreHullShader *CreateHullShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCoreComputeShader *CreateComputeShader(LPCSTR, TS32, LPCSTR, LPCSTR, CString *) { return nullptr; }
  virtual CCoreVertexShader *CreateVertexShader() { return nullptr; }
  virtual CCorePixelShader *CreatePixelShader() { return nullptr; }
  virtual CCoreGeometryShader *CreateGeometryShader() { return nullptr; }
  virtual CCoreDomainShader *CreateDomainShader() { return nullptr; }
  virtual CCoreHullShader *CreateHullShader() { return nullptr; }
  virtual CCoreComputeShader *CreateComputeShader() { return nullptr; }
  virtual void SetShaderConstants(TS32, TS32, CCoreConstantBuffer **) {}
  virtual CCoreConstantBuffer *CreateConstantBuffer() { return nullptr; }
  virtual CCoreBlendState *CreateBlendState() { return nullptr; }
  virtual CCoreDepthStencilState *CreateDepthStencilState() { return nullptr; }
  virtual CCoreRasterizerState *CreateRasterizerState() { return nullptr; }
  virtual CCoreSamplerState *CreateSamplerState() { return nullptr; }
  virtual TBOOL BeginScene() { return false; }
  virtual TBOOL EndScene() { return false; }
  virtual TBOOL Clear(const TBOOL, const TBOOL, const CColor &, const TF32, const TS32) { return false; }
  virtual TBOOL Flip(TBOOL) { return false; }
  virtual TBOOL DrawIndexedTriangles(TS32, TS32) { return false; }
  virtual TBOOL DrawIndexedLines(TS32, TS32) { return false; }
  virtual TBOOL DrawTriangles(TS32) { return false; }
  virtual TBOOL DrawLines(TS32) { return false; }
  virtual void ForceStateReset() {}
  virtual void TakeScreenShot(CString) {}
  virtual void InitializeDebugAPI() {}
  virtual void CaptureCurrentFrame() {}
  virtual void BeginOcclusionQuery() {}
  virtual TBOOL EndOcclusionQuery() { return false; }
};

TS64 NowMs()
{
  using namespace std::chrono;
  return (TS64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// test doubles for the engine's non-window link dependencies
//
// CCoreDevice's out-of-line members, CString's destructor and the engine logger
// live in tests/window_sdl_test_link_doubles.cpp, with the reasoning for each.
// PhxTestDevice above is the device *mock* (the object the window layer talks
// to); the link doubles only make the classes instantiable without pulling the
// CoRE2 resource stack into this target.

//////////////////////////////////////////////////////////////////////////////

int main()
{
  const int sdlVersion = SDL_GetVersion();

  printf("== phx_window_test: SDL3 window/platform layer ==\n");
  printf("SDL %d.%d.%d\n", SDL_VERSIONNUM_MAJOR(sdlVersion), SDL_VERSIONNUM_MINOR(sdlVersion),
         SDL_VERSIONNUM_MICRO(sdlVersion));
  fflush(stdout);

  PhxTestDevice *device = new PhxTestDevice();

  CCoreWindowParameters parameters;
  parameters.Device = device;
  parameters.FullScreen = false;
  parameters.XRes = 960;
  parameters.YRes = 540;
  parameters.WindowTitle = (TCHAR *)"phx_window_test";
  parameters.Maximized = false;
  parameters.ResizeDisabled = false;

  PhxTestWindowHandler handler;
  if (!handler.Initialize(parameters))
  {
    printf("FAIL: CCoreWindowHandlerWin::Initialize() returned false (SDL error: %s)\n", SDL_GetError());
    return 1;
  }

  const TU32 handle = handler.GetHandle();
  SDL_Window *primary = PhxWindowGetPrimaryWindow();
  SDL_Window *fromHandle = PhxWindowFromEngineHandle(handle);
  const uintptr_t rawPointer = (uintptr_t)primary;
  const SDL_WindowID windowID = SDL_GetWindowID(primary);

  printf("requested resolution : %dx%d\n", 960, 540);
  printf("handler resolution   : %dx%d\n", (int)handler.GetXRes(), (int)handler.GetYRes());
  printf("GetHandle()          : 0x%08X\n", (unsigned)handle);
  printf("SDL_Window*          : %p (%s 32 bits)\n", (void *)rawPointer,
         rawPointer <= 0xFFFFFFFFu ? "fits in" : "does NOT fit in");

  int pointW = 0, pointH = 0;
  SDL_GetWindowSize(primary, &pointW, &pointH);
  printf("window size (points) : %dx%d (device init saw %dx%d)\n", pointW, pointH, (int)device->SeenXRes,
         (int)device->SeenYRes);
  fflush(stdout);

  bool handleRoundTrip = (fromHandle == primary);
  printf("handle round-trip    : %s\n", handleRoundTrip ? "ok" : "MISMATCH");
  printf("device saw handle    : 0x%08X\n", (unsigned)device->SeenHandle);
  fflush(stdout);

  // ---- synthetic input ----------------------------------------------------
  //
  // Nobody is guaranteed to be at the keyboard during the one-second window, so
  // the translation is exercised with events pushed through SDL itself.  They
  // travel the exact same path as real input (SDL_PollEvent -> WM_* -> the
  // handler's WindowProc) and the key-state table is checked in between.
  bool keyStateDownOk = false;
  bool keyStateUpOk = false;
  bool utf8Ok = false;
  bool resizeSeen = false;

  const auto pushKey = [windowID](SDL_Keycode key, SDL_Scancode scan, SDL_Keymod mod, bool down) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = windowID;
    event.key.key = key;
    event.key.scancode = scan;
    event.key.mod = mod;
    event.key.down = down;
    SDL_PushEvent(&event);
  };

  const auto pushText = [windowID](const char *text) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = windowID;
    event.text.text = text;
    SDL_PushEvent(&event);
  };

  const auto pushMouseButton = [windowID](Uint8 button, bool down, float x, float y) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = windowID;
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    SDL_PushEvent(&event);
  };

  // ---- pump for ~1 second -------------------------------------------------
  const TS64 start = NowMs();
  TS32 frames = 0;
  TS64 elapsed = 0;

  while ((elapsed = NowMs() - start) < 1000)
  {
    if (!handler.HandleMessages())
    {
      printf("[test] the handler reported Done (quit requested) after %d frames\n", (int)frames);
      break;
    }

    ++frames;

    // A, then Shift+A as text, then release: covers WM_KEYDOWN/WM_CHAR/WM_KEYUP
    // and the VK_A/VK_SHIFT/VK_MENU key state.
    if (elapsed >= 100 && handler.KeyDownMessages == 0)
    {
      printf("[test] pushing synthetic SDL_EVENT_KEY_DOWN for 'a' with Shift\n");
      pushKey(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LSHIFT, true);
      handler.HandleMessages();
      keyStateDownOk = PhxWindowIsKeyDown(VK_A) && PhxWindowIsKeyDown(VK_SHIFT) && !PhxWindowIsKeyDown(VK_MENU);
      printf("[test] key state after press: VK_A=%d VK_SHIFT=%d VK_MENU=%d -> %s\n", (int)PhxWindowIsKeyDown(VK_A),
             (int)PhxWindowIsKeyDown(VK_SHIFT), (int)PhxWindowIsKeyDown(VK_MENU), keyStateDownOk ? "ok" : "WRONG");
      pushText("a");
      pushText("\xC3\xA9"); // U+00E9: exercises the UTF-8 -> WM_CHAR path
      handler.HandleMessages();
      utf8Ok = handler.LastCharCodePoint == 0xE9;
      printf("[test] last WM_CHAR code point: U+%04X (expected U+00E9) -> %s\n", (unsigned)handler.LastCharCodePoint,
             utf8Ok ? "ok" : "WRONG");
    }
    else if (elapsed >= 400 && handler.KeyUpMessages == 0)
    {
      printf("[test] pushing synthetic SDL_EVENT_KEY_UP for 'a'\n");
      pushKey(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_NONE, false);
      handler.HandleMessages();
      keyStateUpOk = !PhxWindowIsKeyDown(VK_A) && !PhxWindowIsKeyDown(VK_SHIFT);
      printf("[test] key state after release: VK_A=%d VK_SHIFT=%d -> %s\n", (int)PhxWindowIsKeyDown(VK_A),
             (int)PhxWindowIsKeyDown(VK_SHIFT), keyStateUpOk ? "ok" : "WRONG");
    }
    else if (elapsed >= 550 && handler.MouseButtonMessages == 0)
    {
      printf("[test] pushing synthetic left button down/up\n");
      pushMouseButton(SDL_BUTTON_LEFT, true, 120.0f, 80.0f);
      pushMouseButton(SDL_BUTTON_LEFT, false, 121.0f, 80.0f);
    }
    else if (elapsed >= 600 && handler.WheelMessages == 0)
    {
      SDL_Event wheel;
      SDL_zero(wheel);
      wheel.type = SDL_EVENT_MOUSE_WHEEL;
      wheel.wheel.windowID = windowID;
      wheel.wheel.y = 1.0f;
      SDL_PushEvent(&wheel);
    }
    else if (elapsed >= 650 && !resizeSeen)
    {
      // The resize path: SDL_EVENT_WINDOW_*_SIZE_CHANGED -> HandleResize() ->
      // CCoreDevice::Resize().  Applied by the window manager asynchronously.
      printf("[test] asking SDL to resize the window to 800x450\n");
      SDL_SetWindowSize(primary, 800, 450);
      resizeSeen = true;
    }

    if ((frames % 20) == 0)
    {
      printf("[test] t=%4dms res=%dx%d shift=%d ctrl=%d alt=%d\n", (int)elapsed, (int)handler.GetXRes(),
             (int)handler.GetYRes(), (int)PhxWindowIsKeyDown(VK_SHIFT), (int)PhxWindowIsKeyDown(VK_CONTROL),
             (int)PhxWindowIsKeyDown(VK_MENU));
      fflush(stdout);
    }

    SDL_Delay(5); // keep the CPU cool; the pump itself never blocks
  }

  // ---- SDL key code -> Win32 virtual key mapping --------------------------
  //
  // The SDL key code space has traps (F13..F24 live far away from F1..F12, and
  // the range in between holds PrintScreen/arrows/keypad), so a handful of
  // representative codes are translated and compared against the Win32 values
  // the engine's GUI layer expects.
  const auto expectVK = [&](SDL_Keycode key, SDL_Scancode scan, TS32 expectedVK, const char *name) {
    pushKey(key, scan, SDL_KMOD_NONE, true);
    handler.HandleMessages();
    const TS32 actual = handler.LastKeyDownVK;
    pushKey(key, scan, SDL_KMOD_NONE, false);
    handler.HandleMessages();

    const bool match = (actual == expectedVK);
    printf("[test] VK mapping %-12s -> 0x%02X (expected 0x%02X) %s\n", name, (unsigned)actual, (unsigned)expectedVK,
           match ? "ok" : "WRONG");
    fflush(stdout);
    return match;
  };

  bool mappingOk = true;
  mappingOk &= expectVK(SDLK_A, SDL_SCANCODE_A, VK_A, "A");
  mappingOk &= expectVK(SDLK_PRINTSCREEN, SDL_SCANCODE_PRINTSCREEN, VK_SNAPSHOT, "PrintScreen");
  mappingOk &= expectVK(SDLK_F1, SDL_SCANCODE_F1, VK_F1, "F1");
  mappingOk &= expectVK(SDLK_F13, SDL_SCANCODE_F13, VK_F13, "F13");
  mappingOk &= expectVK(SDLK_LEFT, SDL_SCANCODE_LEFT, VK_LEFT, "Left");
  mappingOk &= expectVK(SDLK_KP_1, SDL_SCANCODE_KP_1, VK_NUMPAD1, "KP_1");
  mappingOk &= expectVK(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, VK_ESCAPE, "Escape");
  mappingOk &= expectVK(SDLK_SEMICOLON, SDL_SCANCODE_SEMICOLON, VK_OEM_1, "Semicolon");

  const bool keyboardOk = handler.KeyDownMessages >= 1 && handler.KeyUpMessages >= 1 && handler.CharMessages >= 2 &&
                          keyStateDownOk && keyStateUpOk && utf8Ok && mappingOk;
  const bool mouseOk = handler.MouseMoveMessages >= 1 && handler.MouseButtonMessages >= 2 && handler.WheelMessages >= 1;

  printf("---- summary ----\n");
  printf("frames pumped        : %d\n", (int)frames);
  printf("messages translated  : %d\n", (int)handler.MessageCount);
  printf("  key down/up        : %d/%d (chars %d) -> %s\n", (int)handler.KeyDownMessages, (int)handler.KeyUpMessages,
         (int)handler.CharMessages, keyboardOk ? "ok" : "FAILED");
  printf("  key state + VK map : %s\n",
         (keyStateDownOk && keyStateUpOk && utf8Ok && mappingOk) ? "ok" : "FAILED");
  printf("  mouse move/button  : %d/%d (wheel %d) -> %s\n", (int)handler.MouseMoveMessages,
         (int)handler.MouseButtonMessages, (int)handler.WheelMessages, mouseOk ? "ok" : "FAILED");
  printf("  expose/WM_PAINT    : %d\n", (int)handler.PaintMessages);
  printf("  WM_CLOSE           : %d\n", (int)handler.CloseMessages);
  printf("handler resizes      : %d\n", (int)handler.ResizeCalls);
  printf("device resizes       : %d (last %dx%d)\n", (int)device->ResizeCalls, (int)handler.GetXRes(),
         (int)handler.GetYRes());
  printf("device fullscreen    : %d\n", (int)device->FullScreenModeCalls);
  printf("resize applied       : %s\n", handler.GetXRes() == 800 && handler.GetYRes() == 450 ? "ok" : "not observed");

  handler.Destroy();
  const bool destroyed = PhxWindowGetPrimaryWindow() == nullptr;
  printf("destroyed cleanly    : %s\n", destroyed ? "yes" : "no");
  fflush(stdout);

  const bool ok = handleRoundTrip && keyboardOk && mouseOk && destroyed;
  printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 3;
}
