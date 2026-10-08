#pragma once
// The handful of Win32-sized integer types the platform layer needs without
// pulling in compat/Windows.h (which is a build-wide shim and must not be a
// dependency of standalone tools).
#include <cstdint>

typedef uint8_t TU8;
typedef uint16_t TU16;
typedef uint32_t TU32;
typedef uint64_t TU64;
typedef int8_t TS8;
typedef int16_t TS16;
typedef int32_t TS32;
typedef int64_t TS64;
typedef float TF32;
typedef double TF64;
typedef unsigned char TBOOL;
