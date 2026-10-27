// Force-included before anything else in every translation unit that sees the
// compat headers (see apex_compat_flags in CMakeLists.txt).
//
// Why: compat/Windows.h defines min and max as function-like macros, exactly as
// the real <windows.h> does, because the vendored engine calls them bare in 334
// places.  libc++ tolerates that; libstdc++ does not -- it uses `min`/`max`
// unqualified inside its own headers (<chrono>, <algorithm>, ...), and once the
// macros exist those lines expand into nonsense:
//
//   bits/algorithmfwd.h:449: error: too few arguments provided to function-like
//   macro invocation
//   compat/Windows.h:267: note: macro 'min' defined here
//
// The macros cannot be removed, and hiding them file by file means finding every
// translation unit that reaches a standard header.  So the standard headers are
// pulled in FIRST, before any Windows header has had a chance to define the
// macros; their include guards then make every later inclusion a no-op.
//
// Keep this list to what the engine actually reaches -- it is compiled into every
// source file, so it is paid for on each one.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
