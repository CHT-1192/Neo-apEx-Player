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

// Reads the back buffer as tightly packed RGB24 (height * width * 3 bytes), the
// form a raw video pipe wants: `--raw-video | ffmpeg -f rawvideo -pix_fmt rgb24`.
bool ReadBackBufferRGB(std::vector<uint8_t> &out, UINT *width, UINT *height, std::string *error);
bool ReadTextureRGB24(ID3D11Texture2D *texture, std::vector<uint8_t> &out, UINT *width, UINT *height,
                      std::string *error);

// Same, for any texture (used to inspect an intermediate render target, which
// separates "the events drew nothing" from "the final blit is broken").
bool DumpTextureToPPM(ID3D11Texture2D *texture, const char *path, std::string *error);

// "N=path" specs parsed from --dump-frame, plus "loading=path" for the loading
// screen.  The loading screen is presented from inside the resource-generation
// loop rather than from the demo's frame loop, so it cannot be reached by frame
// number -- and it is a thing this port has repeatedly got wrong, so it has to
// be capturable.  The frame written is the first one at or past the middle of
// the load: the early loading frames are presented before the generator has
// produced anything, and the last one is past the end of the loading project's
// timeline, so neither of those says anything.
struct FrameDumpRequest
{
  long long frame = 0;
  bool loading = false; // "loading=path"
  bool done = false;
  std::string path;
};

} // namespace phxplayer
