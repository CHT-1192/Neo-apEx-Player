#pragma once

// Starts a Metal (and therefore MoltenVK) frame capture writing a .gputrace to
// `path`; stop it once the frames of interest have been submitted.  Apple only --
// the implementation is Objective-C++ in metal_capture.mm.
bool phxplayer_start_metal_capture(const char *path);
void phxplayer_stop_metal_capture(void);
