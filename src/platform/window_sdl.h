// SDL3 window/platform layer for the apEx Vulkan port - private header.
//
// Everything that needs CCoreWindowHandlerWin (or the SDL window that backs it)
// includes this header instead of Bedrock/CoRE2/WindowHandler.h directly, for
// two reasons:
//
//   1. It pulls in src/platform/win32_window_gap.h first.  compat/Windows.h
//      still has no Win32 message/virtual-key identifiers, and WindowHandler's
//      WindowProc() hook speaks in WM_*/VK_* values.
//   2. It declares the two entry points that make the "HWND is an SDL_Window*"
//      convention usable in both directions (see below).
//
// The implementation lives in src/platform/window_sdl.cpp; the vendored Win32
// Bedrock/CoRE2/WindowHandler.cpp is excluded from the build (CMakeLists.txt).
#pragma once

#include "win32_window_gap.h"

// the interface this layer implements (also pulls Bedrock/BaseLib)
#include "WindowHandler.h"

// COREDEVICEAPI, used by CCoreWindowHandlerWin::HandleAltEnter
#include "Enums.h"

// CCoreDevice: CCoreWindowHandlerWin drives the device
// (Initialize/DeviceOk/Resize/IsWindowed/SetFullScreenMode/GetAPIType)
#include "Device.h"

#include <SDL3/SDL.h>

//////////////////////////////////////////////////////////////////////////////
// The engine handle <-> SDL_Window* convention
//
// CCoreWindowHandlerWin::GetHandle() returns the SDL_Window* of the created
// window, cast to the engine's TU32 ("HWND").  compat/Windows.h defines HWND as
// void*, so the D3D11 shim and the vendored CCoreDX11Device see the same value
// the engine was handed.
//
// IMPORTANT, measured on the target (macOS/arm64): TU32 is `unsigned int`
// (Bedrock/BaseLib/Types.h:7), i.e. 32 bits, while an SDL_Window* is 64 bits and
// heap addresses routinely exceed 4 GiB.  GetHandle() therefore *truncates* the
// pointer, exactly like the Win32 original did on Win64 - and unlike on Win32
// the truncated value cannot be cast back.  Any consumer that receives the
// engine handle (scd.OutputWindow in D3D11CreateDeviceAndSwapChain) must go
// through PhxWindowFromEngineHandle() instead of casting:
//
//   SDL_Window *w = PhxWindowFromEngineHandle((TU32)(uintptr_t)scd.OutputWindow);
//
// which resolves the value against the live SDL window list (and is exact for
// the engine's single-window case, which is the only configuration this port
// supports).  PhxWindowGetPrimaryWindow() is the same thing without a handle,
// for shims that are called without one.

SDL_Window *PhxWindowFromEngineHandle(TU32 handle);
SDL_Window *PhxWindowGetPrimaryWindow();

// Called by the window layer when the primary window is created/destroyed.
// Declared here but defined in window_handle.cpp, which deliberately has no
// BaseLib/Logger dependency so small tools can link the resolver alone.
extern "C" void PhxWindowSetPrimaryWindow(void *window);

//////////////////////////////////////////////////////////////////////////////
// Keyboard state
//
// CCoreWindowHandler's interface declares no key state at all: the Win32
// original tracked none either and the GUI layer instead polls GetKeyState().
// This layer keeps the equivalent table (bit 15 = key down, bit 0 = toggled)
// filled from SDL_EVENT_KEY_DOWN/UP, so a compat GetKeyState() can forward to
// it.  VK codes are the Win32 ones declared in win32_window_gap.h.

// Optional UI event hook.
//
// The window layer translates SDL events into WM_* messages for the engine, and
// nothing outside it sees them.  Dear ImGui needs to, so it can drive a setup
// screen and know whether the pointer is over its own widgets.  The hook is
// called for every event BEFORE that translation and returns true when the UI
// consumed it, in which case the engine does not see the event at all.
//
// A free function rather than a virtual on CCoreWindowHandlerWin: that class is
// vendored, and this is the whole of the interface the UI needs.
using PhxSdlEventHook = bool (*)(const SDL_Event *event);
void PhxWindowSetEventHook(PhxSdlEventHook hook);

TS16 PhxWindowGetKeyState(TS32 virtualKey);
TBOOL PhxWindowIsKeyDown(TS32 virtualKey);
