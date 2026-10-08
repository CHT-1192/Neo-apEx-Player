// --dump-frame N=path.ppm support.
//
// Written so that a run can be *checked* rather than merely observed: the
// player copies the back buffer into a staging texture through the ordinary
// D3D11 API (the same one the engine uses), maps it and writes a binary PPM.
// PPM needs no dependency and is trivially convertible (`sips -s format png`).
#pragma once

#include <string>
#include <vector>

#include "d3d11.h"

namespace phxplayer
{

// Dumps the current back buffer.  `path` should end in .ppm; any other suffix
// still produces a PPM and logs the actual format.
// Returns true on success.
bool DumpBackBufferToPPM(const char *path, std::string *error);

// Same, for any texture (used to inspect an intermediate render target, which
// separates "the events drew nothing" from "the final blit is broken").
bool DumpTextureToPPM(ID3D11Texture2D *texture, const char *path, std::string *error);

// "N=path" specs parsed from --dump-frame.
struct FrameDumpRequest
{
  long long frame = 0;
  std::string path;
};

} // namespace phxplayer
