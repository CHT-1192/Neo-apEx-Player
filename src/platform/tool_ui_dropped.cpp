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
