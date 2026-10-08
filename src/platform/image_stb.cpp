// Portable replacement for Bedrock/BaseLib/ImageDecompressor.cpp.
//
// The Windows original decodes through OLE/GDI+.  We use stb_image, which
// covers the formats apEx projects actually reference (PNG, JPG, TGA, BMP,
// DDS via the engine's own loader, PSD, GIF, HDR).
#include "../../third_party/apEx/Bedrock/BaseLib/BaseLib.h"

// NOTE: the *implementation* lives in the vendored
// Bedrock/UtilLib/PNGDecompressor.cpp, which also defines
// STB_IMAGE_IMPLEMENTATION.  Defining it here as well would duplicate every
// stbi_* symbol at link time; this file only uses the API.
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

TU8 *DecompressImage(const TU8 *ImageData, TS32 ImageDataSize, TS32 &XSize, TS32 &YSize)
{
  int x = 0, y = 0, comp = 0;
  stbi_uc *pixels = stbi_load_from_memory(ImageData, ImageDataSize, &x, &y, &comp, 4);
  if (!pixels)
  {
    XSize = 0;
    YSize = 0;
    return nullptr;
  }

  // stb hands back a buffer from its own allocator; copy into the engine's
  // allocator so the caller can `delete[]` it (which is what DX11Texture does).
  const size_t bytes = (size_t)x * (size_t)y * 4;
  TU8 *out = new TU8[bytes];
  memcpy(out, pixels, bytes);
  stbi_image_free(pixels);

  XSize = x;
  YSize = y;
  return out;
}
