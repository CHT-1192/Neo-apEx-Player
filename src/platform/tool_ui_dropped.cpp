// Definitions for the few globals that the tool's UI used to own.
//
// Decoupling Phoenix_Tool from the unvendored editor (third_party/apEx/apEx/)
// left a handful of `extern` declarations whose definitions lived in UI
// translation units.  They are listed here with what they were used for, so the
// gap is explicit rather than a silent stub.
#include "../../third_party/apEx/Bedrock/BaseLib/BaseLib.h"

// apxProject.cpp:720 `extern CString apexBuild;` — written into the
// <apExProject build="..."> attribute by CapexProject::ExportToString().  The
// player never exports, so this only has to exist; it reports the player
// instead of pretending to be the original tool build.
CString apexBuild(_T("Neo-apEx-Player (Vulkan port)"));

// Model_Tool.cpp:1333 `extern bool pickingEnabled;` — the editor flips this
// while dragging a gizmo.  CphxModelObject_Tool_Mesh::Pick() returns false
// early when it is off.  Nothing in the player calls Pick(), and "enabled" is
// the original default, so this preserves the upstream behaviour.
bool pickingEnabled = true;

// apxProject.cpp (CapexRenderTargetCollection::UpdateCollection) —
// `Config::MonitorAspectX/Y`, which the editor's settings dialog wrote to its
// config file.  The release player asks the same question in its setup dialog,
// so the answer is a runtime value here rather than a compile-time constant.
// See src/platform/phx_monitor_aspect.h for the full derivation; 0,0 is "auto"
// and resolves to the real screen, which is what the port used to hard-code.
namespace
{
int g_monitorAspectX = 0;
int g_monitorAspectY = 0;
} // namespace

void PhxSetMonitorAspect(int x, int y)
{
  g_monitorAspectX = x > 0 ? x : 0;
  g_monitorAspectY = y > 0 ? y : 0;
}

void PhxMonitorAspect(int *x, int *y)
{
  int sx = GetSystemMetrics(SM_CXSCREEN);
  int sy = GetSystemMetrics(SM_CYSCREEN);
  // GetSystemMetrics already falls back to 1920x1080 when CoreGraphics reports
  // no pixels, but do not hand a zero to a division even if it stops doing so.
  if (sx <= 0) sx = 1920;
  if (sy <= 0) sy = 1080;

  if (x) *x = g_monitorAspectX > 0 ? g_monitorAspectX : sx;
  if (y) *y = g_monitorAspectY > 0 ? g_monitorAspectY : sy;
}
