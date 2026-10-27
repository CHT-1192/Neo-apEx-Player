#pragma once
#include <thread>   // [port] MSVC resolved <Thread> case-insensitively to this

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