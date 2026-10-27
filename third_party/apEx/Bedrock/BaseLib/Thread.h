#pragma once
// [port] MSVC resolved <Thread> case-insensitively to the standard <thread>,
// and on that platform the min/max macros from <windows.h> are tolerated by the
// standard library.  libstdc++ is not so forgiving: <thread> pulls in <chrono>,
// which calls std::min/std::max, and the macros turn those into syntax errors.
// Hide them for the duration of the include only -- 334 places in the vendored
// engine still want them as macros.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include <thread>
#pragma pop_macro("max")
#pragma pop_macro("min")

class CThread
{
public:

  virtual void Start() {}
  virtual void Run( void *This ) = 0;

  CThread()
  {
    //std::thread 
  }

  virtual ~CThread()
  {

  }
};