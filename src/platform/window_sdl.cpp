// SDL3 implementation of Bedrock/CoRE2's CCoreWindowHandler interface.
//
// The vendored Bedrock/CoRE2/WindowHandler.cpp is Win32-only (RegisterClass /
// CreateWindow / WndProc / PeekMessage / LoadCursor).  It is excluded from the
// build (see CMakeLists.txt) and replaced by this file, which implements the
// same class with the same method-by-method semantics on top of SDL3.  Read the
// two side by side: every method below mirrors its Win32 counterpart, and where
// the platform could not express something the deviation is called out in a
// comment and in the summary at the end of this header block.
//
// HWND convention
// ---------------
// GetHandle() hands out the window's SDL_Window* as the engine's TU32 "HWND"
// (compat/Windows.h defines HWND as void*, so the D3D11 shim and the vendored
// CCoreDX11Device see exactly the value the engine was given).  TU32 is 32-bit
// on this host while pointers are 64-bit, so the value is truncated in transit;
// consumers must resolve it with PhxWindowFromEngineHandle() - see
// src/platform/window_sdl.h for the full explanation.
//
// Messages
// --------
// CCoreWindowHandlerWin's extension point is WindowProc(UINT, WPARAM, LPARAM)
// and the vendored GUI layer (Bedrock/Whiteboard/Application.cpp) overrides it
// with a full Win32 message switch.  SDL events are therefore translated back
// into WM_* messages here rather than inventing a second, incompatible event
// API: WM_CLOSE, WM_ACTIVATE/WM_SETFOCUS/WM_KILLFOCUS, WM_SIZE
// (SIZE_MINIMIZED/SIZE_MAXIMIZED/SIZE_RESTORED), WM_PAINT (expose), WM_KEYDOWN/
// WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP + WM_CHAR, the six mouse button messages
// (including *DBLCLK), WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_DISPLAYCHANGE and
// WM_SYSCOMMAND (screensaver/monitor power/system menu are swallowed like the
// original did).  Note that SDL has no DefWindowProc equivalent and none is
// called: SDL itself owns the OS-level default behaviour.
//
// Rendering
// ---------
// The message pump never draws and never presents: resize work is handed to the
// device through HandleResize() -> CCoreDevice::Resize(), which is what the
// original did through WM_EXITSIZEMOVE/WM_SIZE.  WM_PAINT is still synthesised
// from SDL_EVENT_WINDOW_EXPOSED so a subclass that renders from inside the pump
// (the apEx pattern) keeps working.  The pump never blocks: SDL_PollEvent
// returns immediately, exactly like the original PeekMessage loop.
//
// Deliberate deviations from the Win32 original
// --------------------------------------------
//  * CCoreWindowParameters' (hinst, ...) constructor no longer allocates
//    `new CCore()` (an implicit, leaked DX11 device).  The window layer does not
//    own device creation; assign CCoreWindowParameters::Device before
//    Initialize().
//  * Window style overrides (OverrideWindowStyle/OverrideWindowStyleEx) and
//    HICON window icons have no portable equivalent; they are ignored with a
//    warning.
//  * There is no WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE phase on this platform: every
//    resize event is applied immediately (the alternative would be to guess when
//    the user stopped dragging).
//  * XRes/YRes/ClientRect are tracked in *device pixels*
//    (SDL_GetWindowSizeInPixels), because that is the size the swapchain must be
//    created at.  Mouse coordinates are converted from SDL's window points into
//    the same pixel space so GUI hit-testing stays consistent on HiDPI screens.
//  * The frame limiter clock is std::chrono::steady_clock (monotonic); the
//    original used timeGetTime(), and the engine's globalTimer is only advanced
//    by the application's own frame loop.
#include "window_sdl.h"

// Defined next to PhxWindowSetEventHook() further down; the message pump needs
// it before then.
static PhxSdlEventHook s_sdlEventHook = nullptr;

#include <SDL3/SDL.h>

#include <chrono>

//////////////////////////////////////////////////////////////////////////////
// file-local state and helpers

namespace
{

// Monotonic millisecond clock.  Only differences are used (frame limiter), so
// the arbitrary epoch is irrelevant.
TS64 PhxNowMs()
{
  using namespace std::chrono;
  return (TS64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The window created by the most recent successful Initialize().  It is the
// fallback for PhxWindowGetPrimaryWindow() and the sanity net for
// PhxWindowFromEngineHandle() when the 32-bit engine handle cannot be matched.
SDL_Window *g_primaryWindow = nullptr;  // mirrored into window_handle.cpp

// SDL window property that stores the handler owning a window.  This is the
// portable stand-in for the Win32 GWLP_USERDATA slot the original used to find
// its CCoreWindowHandlerWin from the HWND in WndProcProxy.
const char *const kHandlerProperty = "phx.core.window.handler";

// Win32-compatible key state: bit 15 = key currently down, bit 0 = toggled
// (caps/num/scroll lock).  Win32 key state is per-thread/global rather than
// per-window, and so is this.
TU16 g_keyState[256];

inline SDL_Window *PhxWindowFromHandle(HWND handle)
{
  return reinterpret_cast<SDL_Window *>(handle);
}

inline HWND PhxHandleFromWindow(SDL_Window *window)
{
  return reinterpret_cast<HWND>(window);
}

CCoreWindowHandlerWin *PhxHandlerFromWindow(SDL_Window *window)
{
  if (!window) return nullptr;
  return static_cast<CCoreWindowHandlerWin *>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), kHandlerProperty, nullptr));
}

//////////////////////////////////////////////////////////////////////////////
// keyboard translation

// SDL virtual key -> Win32 virtual-key code.  Returns 0 for keys the engine has
// no VK code for (media keys, international dead keys, ...); those still update
// nothing but are reported in the log at debug level.
TS32 PhxVKFromKeycode(SDL_Keycode key)
{
  // Printable keys: SDL uses the character's own code point, Win32 uses the
  // upper-case ASCII code for letters.  These ranges are contiguous in both.
  if (key >= 'a' && key <= 'z') return VK_A + (TS32)(key - 'a');
  if (key >= '0' && key <= '9') return VK_0 + (TS32)(key - '0');
  // SDL's F-keys are split into two ranges: F1..F12 are 0x4000003A..45 and
  // F13..F24 are 0x40000068..73.  The 0x46..0x67 span in between holds other
  // keys (PrintScreen, arrows, keypad, ...), so it must not be treated as one
  // range.
  if (key >= SDLK_F1 && key <= SDLK_F12) return VK_F1 + (TS32)(key - SDLK_F1);
  if (key >= SDLK_F13 && key <= SDLK_F24) return VK_F13 + (TS32)(key - SDLK_F13);
  if (key >= SDLK_KP_1 && key <= SDLK_KP_9) return VK_NUMPAD1 + (TS32)(key - SDLK_KP_1);

  switch (key)
  {
    case SDLK_RETURN: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_CLEAR: return VK_CLEAR;
    case SDLK_SELECT: return VK_SELECT;
    case SDLK_EXECUTE: return VK_EXECUTE;
    case SDLK_HELP: return VK_HELP;
    case SDLK_MENU:
    case SDLK_APPLICATION: return VK_APPS;
    case SDLK_PAUSE: return VK_PAUSE;
    case SDLK_PRINTSCREEN: return VK_SNAPSHOT;
    case SDLK_SCROLLLOCK: return VK_SCROLL;
    case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_SLEEP: return VK_SLEEP;
    case SDLK_LCTRL: return VK_LCONTROL;
    case SDLK_RCTRL: return VK_RCONTROL;
    case SDLK_LSHIFT: return VK_LSHIFT;
    case SDLK_RSHIFT: return VK_RSHIFT;
    // Win32 reports the Alt keys as VK_LMENU/VK_RMENU; CCoreWindowHandlerWin's
    // WM_SYSKEYDOWN handling accepts all three spellings (see WindowProc).
    case SDLK_LALT: return VK_LMENU;
    case SDLK_RALT: return VK_RMENU;
    case SDLK_LGUI: return VK_LWIN;
    case SDLK_RGUI: return VK_RWIN;
    case SDLK_KP_0: return VK_NUMPAD0;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_ENTER: return VK_RETURN;
    // US-layout OEM keys; the engine's GUI only cares that they are distinct.
    case SDLK_MINUS: return VK_OEM_MINUS;
    case SDLK_EQUALS: return VK_OEM_PLUS;
    case SDLK_LEFTBRACKET: return VK_OEM_4;
    case SDLK_RIGHTBRACKET: return VK_OEM_6;
    case SDLK_BACKSLASH: return VK_OEM_5;
    case SDLK_SEMICOLON: return VK_OEM_1;
    case SDLK_APOSTROPHE: return VK_OEM_7;
    case SDLK_GRAVE: return VK_OEM_3;
    case SDLK_COMMA: return VK_OEM_COMMA;
    case SDLK_PERIOD: return VK_OEM_PERIOD;
    case SDLK_SLASH: return VK_OEM_2;
    default: return 0;
  }
}

void PhxSetKeyState(TS32 vk, bool down)
{
  if (vk <= 0 || vk > 0xFF) return;

  if (down)
  {
    // Latching keys toggle their low bit on the press transition, like Win32.
    if (!(g_keyState[vk] & 0x8000))
    {
      switch (vk)
      {
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL:
          g_keyState[vk] ^= 0x0001;
          break;
        default:
          break;
      }
    }
    g_keyState[vk] |= 0x8000;
  }
  else
  {
    g_keyState[vk] = (TU16)(g_keyState[vk] & ~0x8000);
  }
}

// Keep the generic modifier VKs in sync with SDL's modifier mask.  Windows only
// sets VK_SHIFT/VK_CONTROL/VK_MENU while either side is held, which is what
// CWBApplication::UpdateControlKeyStates() polls through GetKeyState().
void PhxSyncModifierKeys(SDL_Keymod mod)
{
  PhxSetKeyState(VK_SHIFT, (mod & SDL_KMOD_SHIFT) != 0);
  PhxSetKeyState(VK_CONTROL, (mod & SDL_KMOD_CTRL) != 0);
  PhxSetKeyState(VK_MENU, (mod & SDL_KMOD_ALT) != 0);
}

// Win32 mouse-message wParam (MK_*).  SDL's button mask plus the modifier keys.
WPARAM PhxMouseKeyState(SDL_MouseButtonFlags state)
{
  WPARAM result = 0;
  if (state & SDL_BUTTON_LMASK) result |= MK_LBUTTON;
  if (state & SDL_BUTTON_RMASK) result |= MK_RBUTTON;
  if (state & SDL_BUTTON_MMASK) result |= MK_MBUTTON;
  if (state & SDL_BUTTON_X1MASK) result |= MK_XBUTTON1;
  if (state & SDL_BUTTON_X2MASK) result |= MK_XBUTTON2;
  if (PhxWindowIsKeyDown(VK_SHIFT)) result |= MK_SHIFT;
  if (PhxWindowIsKeyDown(VK_CONTROL)) result |= MK_CONTROL;
  return result;
}

// Win32 keyboard lParam.  SDL has no scan-code-to-lParam conversion either, so
// the SDL physical scancode is placed where the Win32 scan code would be; the
// bits the engine's GUI layer looks at (context code for SYSKEY messages,
// transition/previous state) are set faithfully.
LPARAM PhxKeyLParam(const SDL_KeyboardEvent &key, bool down)
{
  const LPARAM repeat = 1;
  const LPARAM scan = (LPARAM)(key.scancode & 0xFF);
  const LPARAM context = (key.mod & SDL_KMOD_ALT) ? (LPARAM)(1 << 29) : 0;
  const LPARAM previous = down ? 0 : (LPARAM)(1 << 30);
  const LPARAM transition = down ? 0 : (LPARAM)(1 << 31);
  return repeat | (scan << 16) | context | previous | transition;
}

// Window points -> device pixels, so that mouse coordinates live in the same
// space as XRes/YRes/ClientRect on HiDPI displays.
void PhxWindowPixelScale(SDL_Window *window, float *scaleX, float *scaleY)
{
  int pixelW = 0, pixelH = 0, pointW = 0, pointH = 0;
  SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
  SDL_GetWindowSize(window, &pointW, &pointH);
  *scaleX = (pointW > 0 && pixelW > 0) ? (float)pixelW / (float)pointW : 1.0f;
  *scaleY = (pointH > 0 && pixelH > 0) ? (float)pixelH / (float)pointH : 1.0f;
}

CPoint PhxMouseToPixels(SDL_Window *window, float x, float y)
{
  float scaleX = 1.0f, scaleY = 1.0f;
  PhxWindowPixelScale(window, &scaleX, &scaleY);
  return CPoint((TS32)(x * scaleX + 0.5f), (TS32)(y * scaleY + 0.5f));
}

// The SW_* command code that describes a window's current state; stored in
// WINDOWPLACEMENT::showCmd when the window goes fullscreen.
UINT PhxShowCmdFromWindow(SDL_Window *window)
{
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  if (flags & SDL_WINDOW_MINIMIZED) return SW_SHOWMINIMIZED;
  if (flags & SDL_WINDOW_MAXIMIZED) return SW_SHOWMAXIMIZED;
  return SW_SHOWNORMAL;
}

// UTF-8 -> code points, for WM_CHAR.  Returns how many code points were
// produced (invalid sequences are skipped).
TS32 PhxDecodeUTF8(const char *text, TU32 *out, TS32 maxOut)
{
  if (!text) return 0;

  TS32 count = 0;
  const unsigned char *s = (const unsigned char *)text;
  while (*s && count < maxOut)
  {
    TU32 codePoint = 0;
    int continuation = 0;
    const unsigned char lead = *s++;

    if (lead < 0x80)
    {
      codePoint = lead;
    }
    else if ((lead & 0xE0) == 0xC0)
    {
      codePoint = lead & 0x1F;
      continuation = 1;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
      codePoint = lead & 0x0F;
      continuation = 2;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
      codePoint = lead & 0x07;
      continuation = 3;
    }
    else
    {
      continue; // stray continuation byte / invalid lead byte
    }

    for (int i = 0; i < continuation; ++i)
    {
      if ((*s & 0xC0) != 0x80)
      {
        codePoint = 0;
        break;
      }
      codePoint = (codePoint << 6) | (TU32)(*s++ & 0x3F);
    }

    if (codePoint) out[count++] = codePoint;
  }
  return count;
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// window init parameter structure
// (CCoreWindowParameters::Initialize is the genuine implementation of
// WindowHandler.cpp:27 - only the two constructors deviate.)

CCoreWindowParameters::CCoreWindowParameters()
{
  Device = nullptr;
  hInstance = 0;
  FullScreen = false;
  XRes = 800;
  YRes = 600;
  WindowTitle = nullptr;
  Icon = 0;
  Maximized = false;
  ResizeDisabled = false;
}

CCoreWindowParameters::CCoreWindowParameters(HINSTANCE hinst, TBOOL fs, TS32 x, TS32 y, TCHAR *title,
                                             HICON icon, TBOOL maximize, TBOOL noresize)
{
  // The Win32 original called Initialize(new CCore(), ...) here, i.e. it
  // allocated (and leaked) a CCoreDX11Device as a side effect of building a
  // parameter struct.  A platform-parameter struct must not create a graphics
  // device implicitly, so Device stays NULL: the player assigns its device
  // before calling CCoreWindowHandler::Initialize().
  Initialize(nullptr, hinst, fs, x, y, title, icon, maximize, noresize);
}

void CCoreWindowParameters::Initialize(CCoreDevice *device, HINSTANCE hinst, TBOOL fs, TS32 x, TS32 y,
                                      TCHAR *title, HICON icon, TBOOL maximize, TBOOL noresize)
{
  Device = device;
  hInstance = hinst;
  FullScreen = fs;
  XRes = x;
  YRes = y;
  WindowTitle = title;
  Icon = icon;
  Maximized = maximize;
  ResizeDisabled = noresize;
}

//////////////////////////////////////////////////////////////////////////////
// windowhandler baseclass

CCoreWindowHandler::CCoreWindowHandler()
{
  Device = nullptr;
  Done = false;
  XRes = YRes = 0;
  CurrentMouseCursor = CM_ARROW;

  InactiveFrameLimiter = true;
  LimitedFPS = 20;
  // Win32 original: globalTimer.GetTime().  The engine's globalTimer is only
  // advanced by the application's frame loop, so it cannot serve as the clock
  // inside the pump; the monotonic clock below has the same millisecond unit.
  LastRenderedFrame = (TS32)PhxNowMs();
}

CCoreWindowHandler::~CCoreWindowHandler()
{
  Destroy();
  // The device is BORROWED, not owned.
  //
  // The Win32 original's CCoreWindowParameters::Initialize() side-effected a
  // `new CCore()` and this destructor deleted it.  This port deliberately
  // removed that (the player owns exactly one device and hands over a pointer,
  // see CCoreWindowParameters::Device), so deleting it here would free an
  // object the caller owns - for the player that is a stack object in main(),
  // which libmalloc reports as "pointer being freed was not allocated".
  // The owner releases it; we only drop the reference.
  Device = nullptr;
}

void CCoreWindowHandler::Destroy()
{
  Done = true;
}

TS32 CCoreWindowHandler::GetXRes()
{
  return XRes;
}

TS32 CCoreWindowHandler::GetYRes()
{
  return YRes;
}

CCoreWindowParameters &CCoreWindowHandler::GetInitParameters()
{
  return InitParameters;
}

void CCoreWindowHandler::SelectMouseCursor(COREMOUSECURSOR m)
{
  CurrentMouseCursor = m;
}

CPoint CCoreWindowHandler::GetMousePos()
{
  return MousePos;
}

CPoint CCoreWindowHandler::GetLeftDownPos()
{
  return LeftDownPos;
}

CPoint CCoreWindowHandler::GetRightDownPos()
{
  return RightDownPos;
}

CPoint CCoreWindowHandler::GetMidDownPos()
{
  return MidDownPos;
}

void CCoreWindowHandler::SetInactiveFrameLimiter(TBOOL set)
{
  InactiveFrameLimiter = set;
}

//////////////////////////////////////////////////////////////////////////////
// SDL3 windowhandler

CCoreWindowHandlerWin::CCoreWindowHandlerWin() : CCoreWindowHandler()
{
  hWnd = nullptr;
  memset(&WindowPlacement, 0, sizeof(WindowPlacement));
  WindowPlacement.length = sizeof(WINDOWPLACEMENT);
  dwStyle = 0;
  FullScreenX = FullScreenY = 0;
}

CCoreWindowHandlerWin::~CCoreWindowHandlerWin()
{
  // ~CCoreWindowHandler() calls Destroy(), but by then the dynamic type is
  // already CCoreWindowHandler and the override below would not run (the Win32
  // original leaked its HWND for exactly this reason).  Destroy() is
  // idempotent, so calling it here as well is safe.
  Destroy();
}

TBOOL CCoreWindowHandlerWin::Initialize(const CCoreWindowParameters &WindowParams)
{
  const CCoreWindowParameters &wp = WindowParams;

  XRes = wp.XRes;
  YRes = wp.YRes;
  InitParameters = wp;

  if (wp.OverrideWindowStyle || wp.OverrideWindowStyleEx)
  {
    LOG_WARN("[core] window: Win32 style overrides (0x%x/0x%x) have no SDL3 equivalent and were ignored",
             (unsigned)wp.OverrideWindowStyle, (unsigned)wp.OverrideWindowStyleEx);
  }
  if (wp.Icon)
  {
    LOG_WARN("[core] window: HICON window icons are not portable; use SDL_SetWindowIcon() with an SDL_Surface instead");
  }
  if (wp.hInstance)
  {
    // Nothing else in this port consumes HINSTANCE; keep the value in
    // InitParameters (a caller may read it back) but do not act on it.
    LOG_DBG("[core] window: HINSTANCE ignored by the SDL3 backend");
  }

  if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
  {
    LOG_ERR("[core] window: SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s", SDL_GetError());
    return false;
  }

  SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
  if (!wp.ResizeDisabled) flags |= SDL_WINDOW_RESIZABLE;
  if (wp.FullScreen) flags |= SDL_WINDOW_FULLSCREEN;
  if (wp.Maximized) flags |= SDL_WINDOW_MAXIMIZED;

  SDL_Window *window = SDL_CreateWindow(wp.WindowTitle ? wp.WindowTitle : "", XRes, YRes, flags);
  if (!window)
  {
    LOG_ERR("[core] window: SDL_CreateWindow(%dx%d) failed: %s", (int)XRes, (int)YRes, SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
  }

  hWnd = PhxHandleFromWindow(window);
  g_primaryWindow = window;
  PhxWindowSetPrimaryWindow(window);
  SDL_SetPointerProperty(SDL_GetWindowProperties(window), kHandlerProperty, this);

  // FullScreenX/Y mirror the Win32 original: the desktop resolution while
  // windowed, the requested resolution while fullscreen.
  if (wp.FullScreen)
  {
    FullScreenX = XRes;
    FullScreenY = YRes;
  }
  else
  {
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
    FullScreenX = mode ? mode->w : XRes;
    FullScreenY = mode ? mode->h : YRes;
  }

  Device = wp.Device;

  if (!Device)
  {
    LOG_ERR("[init] Device object is NULL during init.");
    Destroy();
    return false;
  }

  // Create the graphics device/swapchain for the window we just made.  This is
  // where the engine's CCoreDX11Device::Initialize() -> InitAPI(GetHandle(), ...)
  // picking up InitParameters and GetXRes/GetYRes happens.
  if (!Device->Initialize(this))
  {
    LOG_ERR("[core] window: device initialization failed");
    SAFEDELETE(Device);
    Destroy();
    return false;
  }

  // Mouse cursors, indexed by COREMOUSECURSOR (CM_ARROW ... CM_WAIT).
  static const SDL_SystemCursor cursorIDs[] = {
      SDL_SYSTEM_CURSOR_DEFAULT,    // CM_ARROW
      SDL_SYSTEM_CURSOR_CROSSHAIR,  // CM_CROSS
      SDL_SYSTEM_CURSOR_EW_RESIZE,  // CM_SIZEWE
      SDL_SYSTEM_CURSOR_NS_RESIZE,  // CM_SIZENS
      SDL_SYSTEM_CURSOR_NESW_RESIZE,// CM_SIZENESW
      SDL_SYSTEM_CURSOR_NWSE_RESIZE,// CM_SIZENWSE
      SDL_SYSTEM_CURSOR_TEXT,       // CM_TEXT
      SDL_SYSTEM_CURSOR_WAIT,       // CM_WAIT
  };
  for (SDL_SystemCursor id : cursorIDs)
  {
    SDL_Cursor *cursor = SDL_CreateSystemCursor(id);
    if (!cursor)
    {
      LOG_WARN("[core] window: SDL_CreateSystemCursor(%d) failed: %s", (int)id, SDL_GetError());
      continue;
    }
    MouseCursors += reinterpret_cast<HCURSOR>(cursor);
  }

  SDL_StartTextInput(window);

  Maximized = wp.Maximized;
  Minimized = false;
  Active = true;

  // Client size is tracked in pixels (see the file header).  Read it directly
  // rather than through HandleResize(): the device was just created with
  // XRes/YRes, so it must not be told to resize to the same size again.
  int pixelW = 0, pixelH = 0;
  if (SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH) && pixelW > 0 && pixelH > 0)
  {
    XRes = (TS32)pixelW;
    YRes = (TS32)pixelH;
  }
  ClientRect.x1 = 0;
  ClientRect.y1 = 0;
  ClientRect.x2 = XRes;
  ClientRect.y2 = YRes;

  // The Win32 original showed the window after the device was created (the
  // swapchain is built from a not-yet-visible window there too).  SDL maps the
  // window in SDL_CreateWindow unless SDL_WINDOW_HIDDEN was requested, so these
  // calls only raise/focus it, mirroring SetForegroundWindow/SetFocus.
  SDL_ShowWindow(window);
  SDL_RaiseWindow(window);
  if (wp.Maximized) SDL_MaximizeWindow(window);

  // Show/maximize are asynchronous on some platforms: this picks up the size
  // the window manager settled on, and notifies the device if it changed.
  HandleResize();

  return true;
}

TBOOL CCoreWindowHandlerWin::HandleMessages()
{
  return HandleOSMessages();
}

// ---------------------------------------------------------------------------
// PeekMessage / DispatchMessage
//
// The engine's CphxProject::DoPrecalc() (Phoenix/Project.cpp:170) pumps Win32
// messages while it presents a loading frame, so the symbols are referenced as
// soon as anything calls CphxProject::LoadProject().  The port's own loop owns
// event dispatch -- CCoreWindowHandlerWin::HandleOSMessages() below is the one
// consumer, and it routes events to the engine *and* to the UI hook -- so these
// only pull pending events into SDL's queue and report that there is nothing
// for the engine to dispatch.  Handing out a second copy of the same events
// would deliver every key press twice.
BOOL PeekMessageA( LPMSG, HWND, UINT, UINT, UINT )
{
  SDL_PumpEvents();
  return FALSE;
}

LRESULT DispatchMessageA( const MSG * )
{
  return 0;
}

TBOOL CCoreWindowHandlerWin::HandleOSMessages()
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (!window) return !Done;

  const SDL_WindowID windowID = SDL_GetWindowID(window);
  SDL_Event event;

  // Non-blocking, like the original PeekMessage(PM_REMOVE) loop: the render
  // thread is never parked here and nothing is drawn from the pump.
  while (SDL_PollEvent(&event))
  {
    // Give the UI first refusal; if it takes the event the engine must not also
    // act on it (a click on a button should not also reach the demo).
    if (s_sdlEventHook && s_sdlEventHook(&event)) continue;

    switch (event.type)
    {
      ////////////////////////////////////////////////////////////////////////
      // application shutdown

      case SDL_EVENT_QUIT:
        // No window attached; the engine treats it as WM_CLOSE.
        WindowProc(WM_CLOSE, 0, 0);
        break;

      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == windowID) WindowProc(WM_CLOSE, 0, 0);
        break;

      ////////////////////////////////////////////////////////////////////////
      // activation / focus

      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        WindowProc(WM_SETFOCUS, 0, 0);
        break;

      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
        WindowProc(WM_KILLFOCUS, 0, 0);
        break;

      case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (event.window.windowID != windowID) break;
        // Mouse focus is not activation: Active is only changed by
        // WM_ACTIVATE (keyboard focus) and WM_SIZE, as in the original.
        FinalizeMouseCursor();
        break;

      case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        // Win32 sent WM_MOUSELEAVE here; nothing in CCoreWindowHandlerWin acts
        // on it.  The cursor is reset when the pointer comes back.
        break;

      ////////////////////////////////////////////////////////////////////////
      // size / state changes

      case SDL_EVENT_WINDOW_MINIMIZED:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_SIZE, SIZE_MINIMIZED, 0);
        break;

      case SDL_EVENT_WINDOW_MAXIMIZED:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_SIZE, SIZE_MAXIMIZED,
                   MAKELPARAM((TS32)event.window.data1, (TS32)event.window.data2));
        break;

      case SDL_EVENT_WINDOW_RESTORED:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_SIZE, SIZE_RESTORED,
                   MAKELPARAM((TS32)event.window.data1, (TS32)event.window.data2));
        // The base class only resizes on a min/max transition; a plain restore
        // must still pick up the new client size.
        HandleResize();
        break;

      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event.window.windowID != windowID) break;
        // SDL has no size-move phase, so every size change is applied here.
        // HandleResize() is a no-op unless the pixel size really changed, and
        // it is the only place that talks to the device.
        if (!Minimized) HandleResize();
        break;

      case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
      case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        // Either the engine (ToggleFullScreen) or the window manager (the macOS
        // zoom button) can change this; re-read the client size either way.
        if (event.window.windowID != windowID) break;
        HandleResize();
        break;

      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        if (event.window.windowID != windowID) break;
        WindowProc(WM_DISPLAYCHANGE, 0, 0);
        HandleResize();
        break;

      case SDL_EVENT_WINDOW_EXPOSED:
        // The apEx pattern is to render from inside the message pump; keep the
        // WM_PAINT notification so a subclass that does so still runs.
        if (event.window.windowID != windowID) break;
        WindowProc(WM_PAINT, 0, 0);
        break;

      ////////////////////////////////////////////////////////////////////////
      // keyboard

      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
      {
        if (event.key.windowID != windowID) break;

        const bool down = event.key.down;
        const TS32 vk = PhxVKFromKeycode(event.key.key);
        PhxSyncModifierKeys(event.key.mod);

        if (!vk)
        {
          LOG_DBG("[core] window: unmapped SDL key 0x%x (%s)", (unsigned)event.key.key,
                  SDL_GetKeyName(event.key.key));
          break;
        }

        PhxSetKeyState(vk, down);

        // Alt+key is a system key on Windows, which is what makes Alt+Enter
        // reach WM_SYSKEYDOWN -> HandleAltEnter().
        const UINT message = (event.key.mod & SDL_KMOD_ALT)
                                 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                 : (down ? WM_KEYDOWN : WM_KEYUP);
        WindowProc(message, (WPARAM)vk, PhxKeyLParam(event.key, down));
        break;
      }

      case SDL_EVENT_TEXT_INPUT:
      {
        if (event.text.windowID != windowID) break;

        TU32 codePoints[32];
        const TS32 count = PhxDecodeUTF8(event.text.text, codePoints, 32);
        for (TS32 i = 0; i < count; ++i)
        {
          // Win32 sent one WM_CHAR per character; the engine's GUI treats wParam
          // as the character code.
          WindowProc(WM_CHAR, (WPARAM)codePoints[i], 1);
        }
        break;
      }

      ////////////////////////////////////////////////////////////////////////
      // mouse

      case SDL_EVENT_MOUSE_MOTION:
      {
        if (event.motion.windowID != windowID) break;

        const CPoint position = PhxMouseToPixels(window, event.motion.x, event.motion.y);
        MousePos.x = position.x;
        MousePos.y = position.y;
        WindowProc(WM_MOUSEMOVE, PhxMouseKeyState(event.motion.state),
                   MAKELPARAM(position.x, position.y));
        break;
      }

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        if (event.button.windowID != windowID) break;

        const bool down = event.button.down;
        const CPoint position = PhxMouseToPixels(window, event.button.x, event.button.y);
        MousePos.x = position.x;
        MousePos.y = position.y;

        UINT message = 0;
        switch (event.button.button)
        {
          case SDL_BUTTON_LEFT:
            if (down) LeftDownPos = position;
            message = !down ? WM_LBUTTONUP : (event.button.clicks == 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
            break;
          case SDL_BUTTON_RIGHT:
            if (down) RightDownPos = position;
            message = !down ? WM_RBUTTONUP : (event.button.clicks == 2 ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN);
            break;
          case SDL_BUTTON_MIDDLE:
            if (down) MidDownPos = position;
            message = !down ? WM_MBUTTONUP : (event.button.clicks == 2 ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN);
            break;
          default:
            // X1/X2 have no CCoreWindowHandlerWin behaviour; report them with
            // the Win32 message IDs (the XBUTTON index that Win32 packs into the
            // high word of wParam is not encoded - nothing in apEx reads it).
            message = !down ? WM_XBUTTONUP : WM_XBUTTONDOWN;
            break;
        }

        // SDL_MouseButtonEvent carries no button-state field; the device state
        // was already updated by the time the event is queued.
        WindowProc(message, PhxMouseKeyState(SDL_GetMouseState(nullptr, nullptr)),
                   MAKELPARAM(position.x, position.y));
        break;
      }

      case SDL_EVENT_MOUSE_WHEEL:
      {
        if (event.wheel.windowID != windowID) break;

        // Win32 puts *screen* coordinates in the wheel message's lParam (unlike
        // the other mouse messages); SDL's global mouse state is in screen
        // points, which is the closest analogue.
        float screenX = 0.0f, screenY = 0.0f;
        const SDL_MouseButtonFlags state = SDL_GetGlobalMouseState(&screenX, &screenY);
        const int delta = (int)(event.wheel.y * (float)WHEEL_DELTA);
        WindowProc(WM_MOUSEWHEEL,
                   MAKEWPARAM(PhxMouseKeyState(state), (WORD)(TS16)delta),
                   MAKELPARAM((TS32)screenX, (TS32)screenY));
        break;
      }

      default:
        // Everything else (joysticks, sensors, clipboard, text editing, ...) is
        // not part of the CCoreWindowHandler contract.
        break;
    }
  }

  return !Done;
}

TBOOL CCoreWindowHandlerWin::DeviceOK()
{
  if (!Active)
  {
    // Inactive (unfocused or being resized): throttle with the monotonic clock
    // instead of rendering every frame.
    if (!InactiveFrameLimiter) return Device && Device->DeviceOk();

    const TS32 time = (TS32)PhxNowMs();
    if (time - LastRenderedFrame >= 1000 / LimitedFPS)
    {
      LastRenderedFrame = time;
      return true;
    }
    return false;
  }

  if (!Device) return false;
  return Device->DeviceOk();
}

void CCoreWindowHandlerWin::Destroy()
{
  Done = true;

  SDL_Window *window = PhxWindowFromHandle(hWnd);

  // Drop the active cursor before destroying the cursor objects that back it.
  if (window && SDL_WasInit(SDL_INIT_VIDEO)) SDL_SetCursor(nullptr);

  while (MouseCursors.NumItems())
  {
    SDL_Cursor *cursor = reinterpret_cast<SDL_Cursor *>(MouseCursors[MouseCursors.NumItems() - 1]);
    if (cursor) SDL_DestroyCursor(cursor);
    MouseCursors.DeleteByIndex(MouseCursors.NumItems() - 1);
  }

  if (window)
  {
    if (g_primaryWindow == window)
    {
      g_primaryWindow = nullptr;
      PhxWindowSetPrimaryWindow(nullptr);
    }
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_StopTextInput(window);
    SDL_DestroyWindow(window);
    hWnd = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
}

void CCoreWindowHandlerWin::ToggleFullScreen()
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (!Device || !window) return;

  if (Device->IsWindowed())
  {
    // windowed -> fullscreen: remember the windowed placement and style.
    TS32 x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &w, &h);

    WindowPlacement.length = sizeof(WINDOWPLACEMENT);
    WindowPlacement.flags = 0;
    WindowPlacement.showCmd = PhxShowCmdFromWindow(window);
    WindowPlacement.ptMinPosition.x = 0;
    WindowPlacement.ptMinPosition.y = 0;
    WindowPlacement.ptMaxPosition.x = 0;
    WindowPlacement.ptMaxPosition.y = 0;
    WindowPlacement.rcNormalPosition.left = x;
    WindowPlacement.rcNormalPosition.top = y;
    WindowPlacement.rcNormalPosition.right = x + w;
    WindowPlacement.rcNormalPosition.bottom = y + h;
    // Only the low bits are needed to restore the windowed state (resizable /
    // borderless); SDL_WindowFlags values above bit 31 do not affect the window
    // decoration and are re-derived by SDL when leaving fullscreen.
    dwStyle = (TS32)(SDL_GetWindowFlags(window) & 0xFFFFFFFFu);

    SDL_SetWindowFullscreen(window, true);
    Device->SetFullScreenMode(true, FullScreenX, FullScreenY);
    HandleResize();
  }
  else
  {
    // fullscreen -> windowed.
    SDL_SetWindowFullscreen(window, false);

    if (WindowPlacement.rcNormalPosition.right > WindowPlacement.rcNormalPosition.left &&
        WindowPlacement.rcNormalPosition.bottom > WindowPlacement.rcNormalPosition.top)
    {
      SDL_SetWindowPosition(window, WindowPlacement.rcNormalPosition.left, WindowPlacement.rcNormalPosition.top);
      SDL_SetWindowSize(window, WindowPlacement.rcNormalPosition.right - WindowPlacement.rcNormalPosition.left,
                        WindowPlacement.rcNormalPosition.bottom - WindowPlacement.rcNormalPosition.top);
    }
    else
    {
      LOG_WARN("[core] window: no saved windowed placement; leaving the window at its current size");
    }

    SDL_SetWindowResizable(window, (dwStyle & (TS32)SDL_WINDOW_RESIZABLE) != 0);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    Device->SetFullScreenMode(false, 0, 0);
    HandleResize();
  }
}

void CCoreWindowHandlerWin::HandleAltEnter()
{
  if (!Device) return;

  switch (Device->GetAPIType())
  {
    case COREAPI_DX9:
      ToggleFullScreen();
      break;
    case COREAPI_DX11:
      // "handled by dxgi <3" in the original: the swapchain owns the
      // fullscreen transition, which in this port is the Vulkan/D3D11 shim.
      break;
    case COREAPI_OPENGL:
      break;
    default:
      break;
  }
}

LRESULT CALLBACK CCoreWindowHandlerWin::WndProcProxy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  // There is no window-class dispatch under SDL: HandleOSMessages() already
  // routes each event to the handler that owns the window.  This entry point
  // exists because the declared interface has it, and it resolves the handler
  // from the window handle the same way the original resolved GWLP_USERDATA.
  CCoreWindowHandlerWin *handler = PhxHandlerFromWindow(PhxWindowFromHandle(hWnd));
  if (!handler) return 0;
  return handler->WindowProc(uMsg, wParam, lParam);
}

LRESULT CCoreWindowHandlerWin::WindowProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (!window) return 0;

  (void)lParam;

  switch (uMsg)
  {
    case WM_CLOSE:
      Done = true;
      return 0;

    case WM_ACTIVATE:
      Active = (LOWORD(wParam) != WA_INACTIVE);
      break;

    case WM_ENTERSIZEMOVE:
      Active = false;
      break;

    case WM_EXITSIZEMOVE:
      Active = true;
      HandleResize();
      break;

    case WM_SIZE:
    {
      if (Device && Device->IsWindowed() && window)
        dwStyle = (TS32)(SDL_GetWindowFlags(window) & 0xFFFFFFFFu);

      if (wParam == SIZE_MINIMIZED)
      {
        Active = false;
        Minimized = true;
        Maximized = false;
      }

      if (wParam == SIZE_MAXIMIZED)
      {
        Active = true;
        Minimized = false;
        Maximized = true;
        HandleResize();
      }

      if (wParam == SIZE_RESTORED)
      {
        if (Maximized)
        {
          Maximized = false;
          HandleResize();
        }
        else if (Minimized)
        {
          Active = true;
          Minimized = false;
          HandleResize();
        }
        // Neither maximized nor minimized: on Win32 this is the live-resize
        // case, where the original waited for WM_EXITSIZEMOVE.  Here the pump
        // calls HandleResize() directly (see HandleOSMessages).
      }
      break;
    }

    case WM_SYSKEYDOWN:
    {
      // Alt is reported as VK_MENU/VK_LMENU/VK_RMENU depending on the source;
      // all three must be swallowed so the system menu does not open.
      if (wParam == VK_F10) return 0;
      if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) return 0;
      if (wParam == VK_RETURN)
      {
        HandleAltEnter();
        break;
      }
      break;
    }

    case WM_SYSCOMMAND:
    {
      switch (wParam)
      {
        case SC_SCREENSAVE:
        case SC_MONITORPOWER:
        case SC_KEYMENU: // don't ding on alt+enter
          return 0;
        default:
          break;
      }
      break;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
      // SetCapture() analogue.  SDL keeps delivering button events to the
      // captured window until the last button is released.
      SDL_CaptureMouse(true);
      break;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
      // ReleaseCapture() analogue.
      SDL_CaptureMouse(false);
      break;

    default:
      // No DefWindowProc(): SDL owns the OS-level default behaviour, so there
      // is nothing left to forward the message to.
      break;
  }

  return 0;
}

void CCoreWindowHandlerWin::HandleResize()
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (!window) return;

  int pixelW = 0, pixelH = 0;
  if (!SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH)) return;

  // A minimized (or not yet mapped) window reports a 0-sized drawable; keep the
  // last valid resolution instead of telling the device to resize to 0x0.
  if (pixelW <= 0 || pixelH <= 0) return;

  const TS32 oldWidth = ClientRect.x2 - ClientRect.x1;
  const TS32 oldHeight = ClientRect.y2 - ClientRect.y1;

  ClientRect.x1 = 0;
  ClientRect.y1 = 0;
  ClientRect.x2 = (TS32)pixelW;
  ClientRect.y2 = (TS32)pixelH;
  XRes = (TS32)pixelW;
  YRes = (TS32)pixelH;

  if (Device && (oldWidth != XRes || oldHeight != YRes))
    Device->Resize(XRes, YRes);
}

TU32 CCoreWindowHandlerWin::GetHandle()
{
  // The agreed convention: the engine's HWND *is* the SDL_Window*.  TU32 is
  // 32-bit on this host, so this truncates a 64-bit pointer; use
  // PhxWindowFromEngineHandle() to get the window back (window_sdl.h).
  return (TU32)(uintptr_t)hWnd;
}

void CCoreWindowHandlerWin::FinalizeMouseCursor()
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (!window) return;

  CPoint mouse;
  float globalX = 0.0f, globalY = 0.0f;
  SDL_GetGlobalMouseState(&globalX, &globalY);

  TS32 windowX = 0, windowY = 0, windowW = 0, windowH = 0;
  SDL_GetWindowPosition(window, &windowX, &windowY);
  SDL_GetWindowSize(window, &windowW, &windowH);

  // Screen-coordinate space (points), which is what SDL_GetWindowPosition/
  // GetWindowSize report - deliberately not the pixel-space ClientRect.
  mouse.x = (TS32)globalX - windowX;
  mouse.y = (TS32)globalY - windowY;

  const TS32 cursorIndex = (TS32)CurrentMouseCursor;
  if (cursorIndex < 0 || cursorIndex >= MouseCursors.NumItems()) return;

  if (mouse.x >= 0 && mouse.y >= 0 && mouse.x < windowW && mouse.y < windowH)
    SDL_SetCursor(reinterpret_cast<SDL_Cursor *>(MouseCursors[cursorIndex]));
}

void CCoreWindowHandlerWin::SetWindowTitle(CString &Title)
{
  SDL_Window *window = PhxWindowFromHandle(hWnd);
  if (window) SDL_SetWindowTitle(window, Title.GetPointer());
}

//////////////////////////////////////////////////////////////////////////////
// handle / keyboard helpers declared in window_sdl.h


void PhxWindowSetEventHook(PhxSdlEventHook hook) { s_sdlEventHook = hook; }

TS16 PhxWindowGetKeyState(TS32 virtualKey)
{
  if (virtualKey <= 0 || virtualKey > 0xFF) return 0;
  return (TS16)g_keyState[virtualKey];
}

TBOOL PhxWindowIsKeyDown(TS32 virtualKey)
{
  return (PhxWindowGetKeyState(virtualKey) & (TS16)0x8000) ? true : false;
}
