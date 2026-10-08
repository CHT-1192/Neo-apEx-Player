// MSVC-compatible rand()/srand().
//
// The apEx engine uses the C runtime's rand() to drive procedural geometry,
// particle spawns and noise (see BaseLib/Random.h which defines
// RAND_MAX == 0x7fff).  macOS libc's rand() returns 31-bit values with a
// different generator, which would (a) push every `rand()/(float)RAND_MAX`
// far out of the intended 0..1 range and (b) produce different geometry than
// the original tool.  Defining these two symbols in the executable overrides
// libSystem for code in this process only, and reproduces the Windows CRT
// generator bit-for-bit.
#include <cstdlib>

namespace
{
unsigned int g_state = 1;
} // namespace

extern "C" int rand(void)
{
  g_state = g_state * 214013u + 2531011u;
  return (int)((g_state >> 16) & 0x7fff);
}

extern "C" void srand(unsigned int seed)
{
  g_state = seed;
}
