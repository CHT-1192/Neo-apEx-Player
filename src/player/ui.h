// The release player's setup dialog.
//
// apEx/MinimalPlayer puts this in front of everything else:
//
//   MinimalPlayer.cpp:147   if ( OpenSetupDialog( Strings ) ) { ...
//   SetupDialog.cpp:195     DialogBox( 0, MAKEINTRESOURCE( IDD_SETUP ), 0, DlgFunc )
//
// and only afterwards does `InitWindow( hInstance, setupcfg.mode.dmPelsWidth,
// setupcfg.mode.dmPelsHeight, ..., setupcfg.fullscreen )` run.  So it is a
// parentless, borderless, screen-centred popup that owns the whole event loop
// until the user presses GO! or Quit; nothing of the demo exists while it is
// up, and it runs on no graphics device at all.
//
// This is that dialog rebuilt on SDL3 + Dear ImGui (the cross-platform choice
// for this port): its own SDL window, its own SDL_Renderer, its own ImGui
// context.  It deliberately does NOT touch the D3D11/Vulkan path - no swap
// chain, no render pass, no image layout - which is what makes it behave like
// the original instead of fighting the engine for the window.
//
// The controls are the ones in the resource (Setup_Dialog_Without_Social.h,
// a 209x146-DLU template with twelve items):
//
//   "Resolution"     combo,  the display's fullscreen modes, current one first
//   "Monitor Aspect" combo,  "<w>:<h> (auto)" then nine presets
//   "Fullscreen"     checkbox, ticked by default
//   "V-sync"         checkbox, ticked by default
//   "GO!"            default button   -> play
//   "Quit"           button           -> do not play
//
// plus the release's group and title in the upper frame.
#pragma once

#include <string>

namespace phxplayer
{

// What the dialog shows above the controls.  The release player passes the
// compiled-in <Group> and <Title> strings (MinimalPlayer.cpp:129-139); here
// they come from the .apx, which carries the same two values.
struct SetupInfo
{
  std::string group;
  std::string title;
};

// The answer, and the initial state of the controls.  Seeding the members from
// the command line is how `--width/--height/--fullscreen/--vsync` stay usable
// when the dialog is shown.
struct SetupChoice
{
  // false when the user pressed Quit (or closed the window).  The caller must
  // then exit without creating anything, exactly as the original falls through
  // its `if ( OpenSetupDialog( Strings ) )` to `return 0`.
  bool go = false;

  // Which mode the Resolution combo starts on.  width == 0 means "no
  // preference", i.e. open on whatever the desktop is running now - which is
  // what the original pre-selects (SetupDialog.cpp:94-99) and what a caller
  // that was not given --width/--height wants.
  int width = 0;
  int height = 0;
  bool fullscreen = true;
  bool vsync = true;

  // The "Monitor Aspect" combo.  0,0 is its "(auto)" entry, i.e. the real
  // display; anything else is an aspect preset.  See
  // src/platform/phx_monitor_aspect.h for what the engine does with it.
  int monitorAspectX = 0;
  int monitorAspectY = 0;
};

// Show the dialog and block until the user answers.
//
// Returns true when a choice was made, in which case `choice->go` says whether
// to play.  Returns false when the dialog could not be created at all (no
// display, no SDL video, no renderer), leaving `choice` untouched: the caller
// should carry on with the defaults it already has.  This is the same
// degradation as the original's `_DEBUG` build, which skips the dialog and
// fills setupcfg in by hand.
bool RunSetupDialog(const SetupInfo &info, SetupChoice *choice);

} // namespace phxplayer
