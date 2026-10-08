// Portable replacement for Bedrock/BaseLib/StackTracker.cpp.
//
// The Windows original walks the stack with DbgHelp (StackWalk64 /
// SymGetLineFromAddr).  The player only needs allocation provenance in debug
// builds, so we use execinfo/backtrace + dladdr and print raw addresses.
// The vendored StackTracker.cpp is excluded from the build.
#include "../../third_party/apEx/Bedrock/BaseLib/BaseLib.h"

#ifdef ENABLE_STACKTRACKER_CLASS

#include <dlfcn.h>
#include <execinfo.h>

TBOOL CStackTracker::DbgInitialized = false;

CStackTracker::CStackTracker(TS8 Offset)
{
  memset(Stack, 0, sizeof(void *) * STACK_TRACE_DEPTH);

  void *frames[STACK_TRACE_DEPTH + 8];
  int want = STACK_TRACE_DEPTH + Offset + 1;
  if (want > (int)(sizeof(frames) / sizeof(frames[0]))) want = (int)(sizeof(frames) / sizeof(frames[0]));
  int n = backtrace(frames, want);

  int src = 1 + Offset;
  for (int i = 0; i < STACK_TRACE_DEPTH; i++)
  {
    int idx = src + i;
    Stack[i] = (idx < n) ? frames[idx] : nullptr;
  }
}

CStackTracker::CStackTracker(void *Context, TS8 Offset)
{
  // There is no portable equivalent of a CONTEXT record; record the caller's
  // frame instead so the object is still usable.
  (void)Context;
  memset(Stack, 0, sizeof(void *) * STACK_TRACE_DEPTH);
  void *frames[STACK_TRACE_DEPTH + 8];
  int want = STACK_TRACE_DEPTH + Offset + 1;
  if (want > (int)(sizeof(frames) / sizeof(frames[0]))) want = (int)(sizeof(frames) / sizeof(frames[0]));
  int n = backtrace(frames, want);
  int src = 2 + Offset;
  for (int i = 0; i < STACK_TRACE_DEPTH; i++)
  {
    int idx = src + i;
    Stack[i] = (idx < n) ? frames[idx] : nullptr;
  }
}

void CStackTracker::InitializeSym()
{
  DbgInitialized = true;
}

namespace
{
CString DescribeFrame(void *addr)
{
  Dl_info info;
  if (addr && dladdr(addr, &info) && info.dli_sname)
  {
    uintptr_t base = (uintptr_t)info.dli_fbase;
    return CString::Format(_T("%s + 0x%llx (%s)"), info.dli_sname,
                           (unsigned long long)((uintptr_t)addr - base),
                           info.dli_fname ? info.dli_fname : "?");
  }
  return CString::Format(_T("Unresolved address: %p"), addr);
}
} // namespace

void CStackTracker::DumpToLog(LOGVERBOSITY v)
{
  InitializeSym();
  for (TS32 x = 0; x < STACK_TRACE_DEPTH; x++)
    if (Stack[x]) LOGSIMPLE(v, _T("\t\t%s"), DescribeFrame(Stack[x]).GetPointer());
  LOGSIMPLE(v, _T(""));
}

void CStackTracker::DumpToDebugOutput()
{
  InitializeSym();
  for (TS32 x = 0; x < STACK_TRACE_DEPTH; x++)
    if (Stack[x]) OutputDebugString((CString("\t\t") + DescribeFrame(Stack[x]) + "\n").GetPointer());
  OutputDebugString("\n");
}

TS8 *CStackTracker::DumpToString()
{
  InitializeSym();
  CString s;
  for (TS32 x = 0; x < STACK_TRACE_DEPTH; x++)
    if (Stack[x]) s += (CString("\t\t") + DescribeFrame(Stack[x]) + "\n");
  s += _T("\n");

  TS8 *copy = new TS8[s.Length() + 1];
  memset(copy, 0, s.Length() + 1);
  s.WriteAsMultiByte(copy, s.Length() + 1);
  return copy;
}

#endif
