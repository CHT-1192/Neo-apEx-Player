// [port] "Monitor Aspect": the shape of the display the demo is being shown on.
//
// This is a real control in the release player's setup dialog (the "Monitor
// Aspect" combo, whose first entry is "<w>:<h> (auto)" for the real screen) and
// the engine's aspect correction reads it, so the port has to carry it across:
//
//   Phoenix_Tool/apxProject.cpp, CapexRenderTargetCollection::UpdateCollection()
//
//       aspect = ( timelineAspectX * monitorX * screenY )
//              / ( timelineAspectY * monitorY * screenX )
//
//   `screen` is GetSystemMetrics( SM_CXSCREEN / SM_CYSCREEN ) - the real
//   display - and `monitor` is "the monitor I am viewing this on".
//
// Upstream took `monitor` from the editor's own config file
// (unvendored ../apEx/Config.h, `Config::MonitorAspectX/Y`), which the tool's
// settings dialog wrote.  The release player asks the same question in its
// setup dialog.  Those are the same value, so the player records its answer
// here and the engine reads it, instead of the port hard-coding the screen.
//
// 0,0 means "auto" == the real screen.  That is the release player's default
// (the combo opens on the "(auto)" entry) and the only setting that reproduces
// the reference release on a 16:9 display.
//
// Declared with `int` rather than TS32 so that this header has no dependency on
// the vendored tree: TS32 is `typedef int` (Bedrock/BaseLib/Types.h) and the
// engine-side extern in apxProject.cpp says TS32, which is the same type.
#pragma once

// Record the setup dialog's choice.  x == 0 or y == 0 means "auto".
void PhxSetMonitorAspect(int x, int y);

// Resolve the recorded choice into *x / *y, falling back to the real screen for
// "auto".  Never leaves a non-positive value in either output.  Called by the
// engine's aspect correction, which is why it resolves instead of exposing the
// raw globals.
void PhxMonitorAspect(int *x, int *y);
