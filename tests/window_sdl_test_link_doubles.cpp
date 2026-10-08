// Link-level doubles for tests/window_sdl_test.cpp.
//
// The window layer (src/platform/window_sdl.cpp) is deliberately independent of
// the rest of the engine, but two things it *declares* against cannot be linked
// into a small test yet:
//
//   1. Bedrock/CoRE2/Device.cpp - CCoreDevice's out-of-line members.  Linking
//      it drags in the whole CoRE2 resource stack (RenderState, Material,
//      CRenderLayer, the XML layer, ...).  The window test only needs the class
//      to be *instantiable*, so the members below are provided as doubles.
//      Note that ~CCoreDevice is CCoreDevice's key function, so defining it here
//      also emits the class vtable, which in turn references the non-pure
//      virtual Destroy*() helpers - hence those four as well.
//
//   2. Bedrock/BaseLib/Logger.cpp - the engine logger.  It emits
//      CLoggerOutput_DebugOutput::Process(), which calls OutputDebugString() from
//      src/platform/win32_compat.cpp; that file does not compile on this machine
//      yet (GetTickCount64 declaration conflict and a missing _SC_AVPHYS_PAGES),
//      so it cannot be part of this target.  The production window layer logs
//      through the engine's LOG macros, so the test provides CLogger and prints
//      its output to stderr.
//
// CString::~CString() is only referenced because CCoreDevice::TakeScreenShot()
// takes a CString by value; no CString is ever constructed by this test.
//
// Everything here is test-only: the shipping build links the real Device.cpp,
// Logger.cpp and String.cpp.
#include "Device.h"

#include <cstdarg>
#include <cstdio>

//////////////////////////////////////////////////////////////////////////////
// CCoreDevice

CCoreDevice::CCoreDevice()
{
  // Mirrors the member initialisation in Bedrock/CoRE2/Device.cpp:26.  The
  // members Device.cpp leaves alone (Resources, RenderLayers, CurrentRenderState,
  // ...) are default-constructed by their own constructors.
  Window = nullptr;
  CurrentVertexBuffer = RequestedVertexBuffer = nullptr;
  CurrentVertexBufferOffset = RequestedVertexBufferOffset = 0;
  CurrentVertexFormatSize = 0;
  DefaultBlendState = nullptr;
  DefaultDepthStencilState = nullptr;
  DefaultRasterizerState = nullptr;
}

CCoreDevice::~CCoreDevice()
{
  // The real destructor deletes the default render states and reports leaked
  // resources; this test never creates any.
}

TBOOL CCoreDevice::DestroyTexture(CCoreTexture *Texture) const
{
  (void)Texture;
  return false;
}

TBOOL CCoreDevice::DestroyVertexBuffer(CCoreVertexBuffer *VertexBuffer) const
{
  (void)VertexBuffer;
  return false;
}

TBOOL CCoreDevice::DestroyVertexFormat(CCoreVertexFormat *VertexFormat) const
{
  (void)VertexFormat;
  return false;
}

TBOOL CCoreDevice::DestroyIndexBuffer(CCoreIndexBuffer *IndexBuffer) const
{
  (void)IndexBuffer;
  return false;
}

//////////////////////////////////////////////////////////////////////////////
// CString

CString::~CString()
{
  // Never reached: the test creates no CString.  It exists so that the linker
  // can resolve the by-value parameter destructor of
  // CCoreDevice::TakeScreenShot() in the mock device.
}

//////////////////////////////////////////////////////////////////////////////
// CLogger (engine logging)

CLogger Logger;

CLogger::CLogger()
{
}

CLogger::~CLogger()
{
}

void CLogger::Log(LOGVERBOSITY v, TBOOL Prefix, TBOOL TimeStamp, TCHAR *String, ...)
{
  (void)Prefix;
  (void)TimeStamp;

  const char *level = "error";
  if (v >= LOG_DEBUG)
    level = "debug";
  else if (v >= LOG_INFO)
    level = "info";
  else if (v >= LOG_WARNING)
    level = "warn";

  va_list args;
  va_start(args, String);
  fprintf(stderr, "[%s] ", level);
  vfprintf(stderr, String, args);
  fputc('\n', stderr);
  va_end(args);
  fflush(stderr);
}
