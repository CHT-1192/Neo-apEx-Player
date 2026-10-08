// Engine window-handle resolution, deliberately free of any BaseLib/Logger
// dependency so that the D3D11 shim can link it without dragging the whole
// engine into small tools and tests.
//
// The engine carries its window handle in a TU32
// (CCoreWindowHandler::GetHandle returns TU32; Bedrock/BaseLib/Types.h), while
// an SDL_Window* is 64-bit.  The value is therefore truncated in transit, so a
// raw cast back to SDL_Window* produces an invalid pointer.  Everything that
// needs the window must resolve the handle here instead.
//
// See src/platform/window_sdl.h for the interface documentation.
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>

// compat/Windows.h has no BaseLib/Logger dependency (it is declarations
// only), so including it here does not defeat the point of this module.
#include "Windows.h"
#include "win32_compat_types.h"

namespace
{
SDL_Window *g_primary = nullptr;
} // namespace

extern "C" void PhxWindowSetPrimaryWindow(void *window)
{
  g_primary = (SDL_Window *)window;
}

SDL_Window *PhxWindowGetPrimaryWindow()
{
  return g_primary;
}

SDL_Window *PhxWindowFromEngineHandle(uint32_t handle)
{
  if (handle == 0) return nullptr;

  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);

  SDL_Window *match = nullptr;
  int matches = 0;
  for (int i = 0; i < count; ++i)
  {
    if ((uint32_t)(uintptr_t)windows[i] == handle)
    {
      match = windows[i];
      ++matches;
    }
  }

  if (matches == 1) return match;

  if (matches > 1)
  {
    fprintf(stderr, "[core] window: engine handle 0x%x matches %d live windows; refusing to guess\n", (unsigned)handle, matches);
    return nullptr;
  }

  // The handle did not match anything.  The engine only ever has one window,
  // so fall back to it - but say so, because a mismatch means somebody passed
  // a value that GetHandle() never produced.
  if (g_primary)
  {
    fprintf(stderr, "[core] window: engine handle 0x%x does not match the primary window (%p); using the primary window\n",
            (unsigned)handle, (void *)g_primary);
    return g_primary;
  }

  return nullptr;
}

//////////////////////////////////////////////////////////////////////////////
// The three window-management calls the engine makes on its own handle.
//
// compat/Windows.h declares these (Bedrock/CoRE2/DX11Device.cpp:238-243 calls
// them right after device init, mirroring the Win32 original's
// SetForegroundWindow/SetFocus).  They live here rather than in
// win32_compat.cpp because they need SDL, and the handle must go through
// PhxWindowFromEngineHandle() for the same reason the swapchain does - the
// engine's TU32 handle is a truncated pointer.

BOOL ShowWindow(HWND hWnd, int nCmdShow)
{
  SDL_Window *w = PhxWindowFromEngineHandle((uint32_t)(uintptr_t)hWnd);
  if (!w) w = PhxWindowGetPrimaryWindow();
  if (!w) return 0;

  switch (nCmdShow)
  {
  case 0: /* SW_HIDE */ SDL_HideWindow(w); break;
  case 3: /* SW_MAXIMIZE */ SDL_MaximizeWindow(w); SDL_ShowWindow(w); break;
  case 6: /* SW_MINIMIZE */ SDL_MinimizeWindow(w); break;
  case 9: /* SW_RESTORE */ SDL_RestoreWindow(w); SDL_ShowWindow(w); break;
  default: SDL_ShowWindow(w); break; // SW_SHOW / SW_SHOWNORMAL
  }
  return 1;
}

HWND SetFocus(HWND hWnd)
{
  SDL_Window *w = PhxWindowFromEngineHandle((uint32_t)(uintptr_t)hWnd);
  if (!w) w = PhxWindowGetPrimaryWindow();
  if (w)
  {
    SDL_RaiseWindow(w);
    SDL_SetWindowKeyboardGrab(w, true);
  }
  return (HWND)hWnd;
}

BOOL SetForegroundWindow(HWND hWnd)
{
  SDL_Window *w = PhxWindowFromEngineHandle((uint32_t)(uintptr_t)hWnd);
  if (!w) w = PhxWindowGetPrimaryWindow();
  if (!w) return 0;
  SDL_ShowWindow(w);
  SDL_RaiseWindow(w);
  return 1;
}
