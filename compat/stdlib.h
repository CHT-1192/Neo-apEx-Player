// Portable <stdlib.h> for the apEx port: pins RAND_MAX to the Windows value.
//
// The engine writes `rand() / (float)RAND_MAX` in 36 places - particle spawns,
// procedural geometry, spline noise, mesh erosion - and every one of them was
// written against the MSVC CRT, where RAND_MAX is 0x7fff.  macOS's libc says
// 0x7fffffff, 65537x larger, so `rand() / RAND_MAX` lands in 0..1.5e-5 instead
// of 0..1.
//
// That is not a cosmetic difference.  CphxObject_ParticleEmitter_CPU::
// SpawnParticle (Phoenix/Scene.cpp:403-406) rejection-samples a point in
// [-0.5, 0.5]^3 until it falls inside the sphere for a spherical emitter:
//
//     do { p = (rand()/(float)RAND_MAX - 0.5f, ...); }
//     while ( !( EmitterType != 1 || D3DXVec3LengthSq( &p ) < 0.25 ) );
//
// With every sample pinned at the cube's corner, |p|^2 = 0.75 and the condition
// is never true, so the loop never ends.  The demo hung for 90+ seconds on the
// first frame that spawned a particle from a sphere emitter (frame 5999), with
// a stack sample showing nothing but this loop and rand().
//
// src/platform/msvc_rand.cpp already reproduces the Windows generator.  This
// pins the constant that goes with it.  Bedrock/BaseLib/Random.h re-declares
// 0x7fff for the same reason, but only translation units that include BaseLib.h
// ever see it, and it is overridden again the moment <stdlib.h> is read
// afterwards (compat/Windows.h pulls in <cstdlib>).  Scene.cpp, for one, never
// includes BaseLib.h at all - the only RAND_MAX in its translation unit was the
// system one.
//
// Doing it here covers every translation unit, which is what the Windows CRT
// did.  `#include_next` reaches the real header: this directory is first on the
// include path.
#pragma once

#include_next <stdlib.h>

// The same value as Random.h's, so including that afterwards is a benign
// redefinition rather than a conflicting one.
#ifdef RAND_MAX
#undef RAND_MAX
#endif
#define RAND_MAX 0x7fff
