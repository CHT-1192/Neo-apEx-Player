// POSIX implementation of the Win32 subset declared in compat/Windows.h.
//
// Everything the reused apEx code touches is implemented here for real (no
// silent stubs): files, threads, synchronisation, timers, directory scanning
// and string conversion.  Anything that only exists on Windows (COM, sockets,
// minidumps, structured exception handling) is deliberately absent and its
// owning source file is excluded from the build.
#include "Windows.h"
#include "MMSystem.h"   // timeGetTime/timeGetDevCaps, defined below
#include "shlwapi.h"     // PathCombineA etc., defined below
#include "wingdi.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>

//////////////////////////////////////////////////////////////////////////////
// internal helpers

namespace
{

enum : unsigned
{
  kMagicFile = 0x46494c45u, // 'FILE'
  kMagicFind = 0x46494e44u, // 'FIND'
};

struct WinFileHandle
{
  unsigned magic = kMagicFile;
  int fd;
};

struct WinFindHandle
{
  unsigned magic = kMagicFind;
  DIR *dir;
  std::string pattern;
  std::string dirPath;
};

inline WinFileHandle *AsFile(HANDLE h) { return reinterpret_cast<WinFileHandle *>(h); }
inline WinFindHandle *AsFind(HANDLE h) { return reinterpret_cast<WinFindHandle *>(h); }

DWORD g_lastError = 0;

const char *BaseName(const char *p)
{
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// Convert a Win32 wildcard pattern into a fnmatch pattern (case-insensitive on
// Windows; we keep it case-insensitive for parity with the original code).
bool WildcardMatch(const char *pattern, const char *name)
{
  std::string p, n;
  for (const char *c = pattern; *c; ++c) p.push_back((char)tolower((unsigned char)*c));
  for (const char *c = name; *c; ++c) n.push_back((char)tolower((unsigned char)*c));
  return fnmatch(p.c_str(), n.c_str(), 0) == 0;
}

time_t FileTimeToUnix(const struct stat &st) { return st.st_mtime; }

void StatToFindData(const struct stat &st, const char *name, LPWIN32_FIND_DATAA out)
{
  memset(out, 0, sizeof(WIN32_FIND_DATAA));
  out->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  if (!(st.st_mode & S_IWUSR)) out->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
  out->nFileSizeLow = (DWORD)(st.st_size & 0xffffffffu);
  out->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
  strncpy(out->cFileName, name, MAX_PATH - 1);
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// time

extern "C" DWORD GetTickCount(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (DWORD)((unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

extern "C" unsigned long long GetTickCount64(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

extern "C" void Sleep(DWORD ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

extern "C" BOOL QueryPerformanceCounter(LARGE_INTEGER *v)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  v->QuadPart = (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
  return TRUE;
}

extern "C" BOOL QueryPerformanceFrequency(LARGE_INTEGER *v)
{
  v->QuadPart = 1000000000ll;
  return TRUE;
}

extern "C" BOOL GetLocalTime(LPSYSTEMTIME st)
{
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  st->wYear = (WORD)(tmv.tm_year + 1900);
  st->wMonth = (WORD)(tmv.tm_mon + 1);
  st->wDayOfWeek = (WORD)tmv.tm_wday;
  st->wDay = (WORD)tmv.tm_mday;
  st->wHour = (WORD)tmv.tm_hour;
  st->wMinute = (WORD)tmv.tm_min;
  st->wSecond = (WORD)tmv.tm_sec;
  st->wMilliseconds = 0;
  return TRUE;
}

extern "C" BOOL GetSystemTime(LPSYSTEMTIME st)
{
  time_t t = time(nullptr);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  st->wYear = (WORD)(tmv.tm_year + 1900);
  st->wMonth = (WORD)(tmv.tm_mon + 1);
  st->wDayOfWeek = (WORD)tmv.tm_wday;
  st->wDay = (WORD)tmv.tm_mday;
  st->wHour = (WORD)tmv.tm_hour;
  st->wMinute = (WORD)tmv.tm_min;
  st->wSecond = (WORD)tmv.tm_sec;
  st->wMilliseconds = 0;
  return TRUE;
}

extern "C" void GetSystemTimeAsFileTime(LPFILETIME ft)
{
  // 100ns intervals since 1601-01-01
  const unsigned long long epoch = 116444736000000000ull;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  unsigned long long v = epoch + (unsigned long long)ts.tv_sec * 10000000ull + ts.tv_nsec / 100ull;
  ft->dwLowDateTime = (DWORD)(v & 0xffffffffu);
  ft->dwHighDateTime = (DWORD)(v >> 32);
}

//////////////////////////////////////////////////////////////////////////////
// debug output / errors

extern "C" void OutputDebugStringA(const char *s)
{
  fputs(s, stderr);
  fflush(stderr);
}

extern "C" void OutputDebugStringW(const wchar_t *s)
{
  fputws(s, stderr);
  fflush(stderr);
}

extern "C" DWORD GetLastError(void) { return g_lastError; }
extern "C" void SetLastError(DWORD e) { g_lastError = e; }
extern "C" void DebugBreak(void) { __builtin_trap(); }
extern "C" BOOL IsDebuggerPresent(void) { return 0; }
extern "C" int MessageBoxA(HWND, const char *text, const char *caption, UINT)
{
  fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
  return IDOK;
}

//////////////////////////////////////////////////////////////////////////////
// threads

extern "C" DWORD GetCurrentThreadId(void)
{
#if defined(__APPLE__)
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return (DWORD)id;
#else
  return (DWORD)(uintptr_t)pthread_self();
#endif
}

extern "C" DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
extern "C" HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
extern "C" HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

namespace
{
struct ThreadStart
{
  DWORD(__stdcall *fn)(LPVOID);
  LPVOID param;
};
} // namespace

extern "C" HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, DWORD(__stdcall *start)(LPVOID), LPVOID param, DWORD flags, DWORD *threadId)
{
  (void)flags;
  pthread_t *t = new pthread_t();
  ThreadStart *ts = new ThreadStart{start, param};
  if (pthread_create(t, nullptr, [](void *arg) -> void * {
        ThreadStart *ts = (ThreadStart *)arg;
        DWORD(__stdcall * fn)(LPVOID) = ts->fn;
        LPVOID p = ts->param;
        delete ts;
        fn(p);
        return nullptr;
      },
        ts) != 0)
  {
    delete t;
    delete ts;
    return nullptr;
  }
  if (threadId) *threadId = GetCurrentThreadId();
  return (HANDLE)t;
}

extern "C" DWORD ResumeThread(HANDLE) { return 0; }
extern "C" BOOL TerminateThread(HANDLE h, DWORD)
{
  if (h)
  {
    pthread_cancel(*(pthread_t *)h);
    pthread_join(*(pthread_t *)h, nullptr);
    delete (pthread_t *)h;
  }
  return TRUE;
}
extern "C" DWORD GetExitCodeThread(HANDLE, DWORD *code)
{
  if (code) *code = 0;
  return 0;
}
extern "C" void ExitThread(DWORD) { pthread_exit(nullptr); }
extern "C" BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
extern "C" int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
extern "C" HANDLE OpenProcess(DWORD, BOOL, DWORD) { return nullptr; }
extern "C" BOOL TerminateProcess(HANDLE, UINT code) { exit((int)code); }
extern "C" void ExitProcess(UINT code) { exit((int)code); }
extern "C" DWORD WaitForInputIdle(HANDLE, DWORD) { return 0; }

//////////////////////////////////////////////////////////////////////////////
// synchronisation

struct WinEvent
{
  pthread_mutex_t m;
  pthread_cond_t c;
  bool manual;
  bool signaled;
};

extern "C" HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manualReset, BOOL initialState, const char *)
{
  WinEvent *e = new WinEvent();
  pthread_mutex_init(&e->m, nullptr);
  pthread_cond_init(&e->c, nullptr);
  e->manual = manualReset != 0;
  e->signaled = initialState != 0;
  return (HANDLE)e;
}

extern "C" BOOL SetEvent(HANDLE h)
{
  WinEvent *e = (WinEvent *)h;
  if (!e) return FALSE;
  pthread_mutex_lock(&e->m);
  e->signaled = true;
  if (e->manual)
    pthread_cond_broadcast(&e->c);
  else
    pthread_cond_signal(&e->c);
  pthread_mutex_unlock(&e->m);
  return TRUE;
}

extern "C" BOOL ResetEvent(HANDLE h)
{
  WinEvent *e = (WinEvent *)h;
  if (!e) return FALSE;
  pthread_mutex_lock(&e->m);
  e->signaled = false;
  pthread_mutex_unlock(&e->m);
  return TRUE;
}

extern "C" DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
  WinEvent *e = (WinEvent *)h;
  if (!e) return WAIT_FAILED;
  pthread_mutex_lock(&e->m);
  if (!e->signaled)
  {
    if (ms == INFINITE)
    {
      while (!e->signaled) pthread_cond_wait(&e->c, &e->m);
    }
    else
    {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += ms / 1000;
      ts.tv_nsec += (long)(ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L)
      {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
      }
      while (!e->signaled)
      {
        if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT)
        {
          pthread_mutex_unlock(&e->m);
          return WAIT_TIMEOUT;
        }
      }
    }
  }
  if (!e->manual) e->signaled = false;
  pthread_mutex_unlock(&e->m);
  return WAIT_OBJECT_0;
}

extern "C" DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
  DWORD start = GetTickCount();
  for (;;)
  {
    DWORD satisfied = 0;
    for (DWORD i = 0; i < count; i++)
    {
      WinEvent *e = (WinEvent *)handles[i];
      pthread_mutex_lock(&e->m);
      bool s = e->signaled;
      pthread_mutex_unlock(&e->m);
      if (s)
      {
        if (!waitAll) return i;
        satisfied++;
      }
    }
    if (waitAll && satisfied == count) return WAIT_OBJECT_0;
    if (ms != INFINITE && GetTickCount() - start >= ms) return WAIT_TIMEOUT;
    Sleep(1);
  }
}

extern "C" HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG, const char *)
{
  sem_t *s = new sem_t();
  sem_init(s, 0, (unsigned)initial);
  return (HANDLE)s;
}

extern "C" BOOL ReleaseSemaphore(HANDLE h, LONG count, LONG *prev)
{
  sem_t *s = (sem_t *)h;
  if (!s) return FALSE;
  int v = 0;
  sem_getvalue(s, &v);
  if (prev) *prev = v;
  for (LONG i = 0; i < count; i++) sem_post(s);
  return TRUE;
}

struct WinMutex
{
  pthread_mutex_t m;
};

extern "C" HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initialOwner, const char *)
{
  WinMutex *m = new WinMutex();
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m->m, &a);
  pthread_mutexattr_destroy(&a);
  if (initialOwner) pthread_mutex_lock(&m->m);
  return (HANDLE)m;
}

extern "C" void InitializeCriticalSection(LPVOID cs)
{
  CRITICAL_SECTION *c = (CRITICAL_SECTION *)cs;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&c->m, &a);
  pthread_mutexattr_destroy(&a);
}

extern "C" void DeleteCriticalSection(LPVOID cs)
{
  pthread_mutex_destroy(&((CRITICAL_SECTION *)cs)->m);
}
extern "C" void EnterCriticalSection(LPVOID cs) { pthread_mutex_lock(&((CRITICAL_SECTION *)cs)->m); }
extern "C" void LeaveCriticalSection(LPVOID cs) { pthread_mutex_unlock(&((CRITICAL_SECTION *)cs)->m); }
extern "C" BOOL TryEnterCriticalSection(LPVOID cs) { return pthread_mutex_trylock(&((CRITICAL_SECTION *)cs)->m) == 0; }

extern "C" LONG InterlockedIncrement(volatile LONG *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedDecrement(volatile LONG *v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedExchange(volatile LONG *v, LONG val) { return __atomic_exchange_n(v, val, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedCompareExchange(volatile LONG *v, LONG exchange, LONG comparand)
{
  __atomic_compare_exchange_n(v, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return comparand;
}
extern "C" LONG InterlockedExchangeAdd(volatile LONG *v, LONG add) { return __atomic_fetch_add(v, add, __ATOMIC_SEQ_CST); }
extern "C" void *InterlockedExchangePointer(void *volatile *target, void *value)
{
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

extern "C" BOOL CloseHandle(HANDLE h)
{
  if (!h) return FALSE;
  if (h == (HANDLE)(intptr_t)-1 || h == (HANDLE)(intptr_t)-2) return TRUE;
  unsigned magic = *(const unsigned *)h;
  if (magic == kMagicFile)
  {
    WinFileHandle *f = AsFile(h);
    if (f->fd >= 0) close(f->fd);
    delete f;
    return TRUE;
  }
  if (magic == kMagicFind) return FindClose(h);
  // Events, mutexes and semaphores are owned by their factory types.
  return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
// modules / dynamic loading

extern "C" HMODULE GetModuleHandleA(const char *) { return (HMODULE)(intptr_t)-1; }
extern "C" HMODULE GetModuleHandleW(const wchar_t *) { return (HMODULE)(intptr_t)-1; }

extern "C" HMODULE LoadLibraryA(const char *name)
{
  return (HMODULE)dlopen(name, RTLD_LAZY | RTLD_LOCAL);
}

extern "C" FARPROC GetProcAddress(HMODULE m, const char *name)
{
  if (!m) return nullptr;
  return (FARPROC)dlsym((void *)m, name);
}

extern "C" BOOL FreeLibrary(HMODULE m)
{
  if (!m) return FALSE;
  return dlclose((void *)m) == 0;
}

extern "C" DWORD GetModuleFileNameA(HMODULE, char *buf, DWORD n)
{
  if (!buf || !n) return 0;
  extern char **environ;
  // Prefer _NSGetExecutablePath on macOS; fall back to /proc on Linux.
#if defined(__APPLE__)
  extern int _NSGetExecutablePath(char *, unsigned int *);
  unsigned int size = n;
  if (_NSGetExecutablePath(buf, &size) == 0) return (DWORD)strlen(buf);
#endif
  ssize_t r = readlink("/proc/self/exe", buf, n - 1);
  if (r <= 0) return 0;
  buf[r] = 0;
  return (DWORD)r;
}

//////////////////////////////////////////////////////////////////////////////
// string conversion

extern "C" int MultiByteToWideChar(UINT cp, DWORD, const char *in, int inLen, wchar_t *out, int outLen)
{
  (void)cp;
  std::wstring w;
  if (inLen < 0)
  {
    for (const char *p = in; *p; ++p) w.push_back((wchar_t)(unsigned char)*p);
  }
  else
  {
    for (int i = 0; i < inLen; i++) w.push_back((wchar_t)(unsigned char)in[i]);
  }
  if (outLen == 0) return (int)w.size() + 1;
  int n = (int)w.size();
  if (n > outLen - 1) n = outLen - 1;
  for (int i = 0; i < n; i++) out[i] = w[i];
  out[n] = 0;
  return n;
}

extern "C" int WideCharToMultiByte(UINT cp, DWORD, const wchar_t *in, int inLen, char *out, int outLen, const char *, BOOL *)
{
  (void)cp;
  std::string s;
  if (inLen < 0)
  {
    for (const wchar_t *p = in; *p; ++p) s.push_back((char)(*p & 0xff));
  }
  else
  {
    for (int i = 0; i < inLen; i++) s.push_back((char)(in[i] & 0xff));
  }
  if (outLen == 0) return (int)s.size() + 1;
  int n = (int)s.size();
  if (n > outLen - 1) n = outLen - 1;
  memcpy(out, s.data(), n);
  out[n] = 0;
  return n;
}

//////////////////////////////////////////////////////////////////////////////
// files

static bool IsSeekable(int fd)
{
  struct stat st;
  if (fstat(fd, &st) != 0) return false;
  return !S_ISDIR(st.st_mode) && !S_ISFIFO(st.st_mode);
}

extern "C" HANDLE CreateFileA(const char *path, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
{
  int flags = 0;
  bool read = (access & GENERIC_READ) != 0;
  bool write = (access & GENERIC_WRITE) != 0;

  switch (disposition)
  {
  case CREATE_NEW: flags = O_CREAT | O_EXCL | (read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY)); break;
  case CREATE_ALWAYS: flags = O_CREAT | O_TRUNC | (read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY)); break;
  case OPEN_EXISTING: flags = read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY); break;
  case OPEN_ALWAYS: flags = O_CREAT | (read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY)); break;
  case TRUNCATE_EXISTING: flags = O_TRUNC | (read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY)); break;
  default: flags = O_RDONLY; break;
  }

  int fd = open(path, flags, 0644);
  if (fd < 0)
  {
    g_lastError = (DWORD)errno;
    return INVALID_HANDLE_VALUE;
  }
  WinFileHandle *h = new WinFileHandle{kMagicFile, fd};
  return (HANDLE)h;
}

extern "C" BOOL ReadFile(HANDLE f, LPVOID buf, DWORD toRead, DWORD *read, LPOVERLAPPED)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return FALSE;
  ssize_t r = ::read(h->fd, buf, toRead);
  if (r < 0)
  {
    g_lastError = (DWORD)errno;
    return FALSE;
  }
  if (read) *read = (DWORD)r;
  return TRUE;
}

extern "C" BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD toWrite, DWORD *written, LPOVERLAPPED)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return FALSE;
  size_t done = 0;
  const char *p = (const char *)buf;
  while (done < toWrite)
  {
    ssize_t r = ::write(h->fd, p + done, toWrite - done);
    if (r <= 0)
    {
      g_lastError = (DWORD)errno;
      return FALSE;
    }
    done += (size_t)r;
  }
  if (written) *written = (DWORD)done;
  return TRUE;
}

extern "C" DWORD SetFilePointer(HANDLE f, LONG dist, LONG *high, DWORD method)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return INVALID_SET_FILE_POINTER;
  long long d = dist;
  if (high) d |= ((long long)*high) << 32;
  int whence = method == FILE_BEGIN ? SEEK_SET : (method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
  off_t r = lseek(h->fd, (off_t)d, whence);
  if (r < 0) return INVALID_SET_FILE_POINTER;
  if (high) *high = (LONG)((unsigned long long)r >> 32);
  return (DWORD)(r & 0xffffffffu);
}

extern "C" BOOL SetEndOfFile(HANDLE f)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return FALSE;
  off_t p = lseek(h->fd, 0, SEEK_CUR);
  return ftruncate(h->fd, p) == 0;
}

extern "C" BOOL FlushFileBuffers(HANDLE f)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return FALSE;
  return fsync(h->fd) == 0;
}

extern "C" DWORD GetFileSize(HANDLE f, DWORD *high)
{
  WinFileHandle *h = AsFile(f);
  if (!h) return INVALID_FILE_SIZE;
  struct stat st;
  if (fstat(h->fd, &st) != 0) return INVALID_FILE_SIZE;
  if (high) *high = (DWORD)((unsigned long long)st.st_size >> 32);
  return (DWORD)(st.st_size & 0xffffffffu);
}

extern "C" BOOL GetFileSizeEx(HANDLE f, LARGE_INTEGER *size)
{
  WinFileHandle *h = AsFile(f);
  if (!h || !size) return FALSE;
  struct stat st;
  if (fstat(h->fd, &st) != 0) return FALSE;
  size->QuadPart = st.st_size;
  return TRUE;
}

extern "C" BOOL DeleteFileA(const char *path) { return unlink(path) == 0; }
extern "C" BOOL CopyFileA(const char *src, const char *dst, BOOL failIfExists)
{
  if (failIfExists && access(dst, F_OK) == 0)
  {
    g_lastError = EEXIST;
    return FALSE;
  }
  FILE *in = fopen(src, "rb");
  if (!in) return FALSE;
  FILE *out = fopen(dst, "wb");
  if (!out)
  {
    fclose(in);
    return FALSE;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return TRUE;
}
extern "C" BOOL MoveFileA(const char *src, const char *dst) { return rename(src, dst) == 0; }
extern "C" BOOL MoveFileExA(const char *src, const char *dst, DWORD) { return rename(src, dst) == 0; }
extern "C" BOOL CreateDirectoryA(const char *path, LPSECURITY_ATTRIBUTES) { return mkdir(path, 0755) == 0 || errno == EEXIST; }
extern "C" BOOL RemoveDirectoryA(const char *path) { return rmdir(path) == 0; }

extern "C" DWORD GetFileAttributesA(const char *path)
{
  struct stat st;
  if (stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
  DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
  return a;
}

extern "C" BOOL SetFileAttributesA(const char *, DWORD) { return TRUE; }

extern "C" DWORD GetFullPathNameA(const char *name, DWORD len, char *buf, char **filePart)
{
  char resolved[MAX_PATH * 4];
  if (name[0] == '/')
    strncpy(resolved, name, sizeof(resolved) - 1);
  else
  {
    char cwd[MAX_PATH * 4];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    snprintf(resolved, sizeof(resolved), "%s/%s", cwd, name);
  }
  resolved[sizeof(resolved) - 1] = 0;
  size_t n = strlen(resolved);
  if (n + 1 > len) return (DWORD)(n + 1);
  strcpy(buf, resolved);
  if (filePart) *filePart = buf + n;
  return (DWORD)n;
}

extern "C" DWORD GetCurrentDirectoryA(DWORD len, char *buf)
{
  if (!getcwd(buf, len)) return 0;
  return (DWORD)strlen(buf);
}

extern "C" BOOL SetCurrentDirectoryA(const char *path) { return chdir(path) == 0; }

extern "C" DWORD GetTempPathA(DWORD len, char *buf)
{
  const char *t = getenv("TMPDIR");
  if (!t) t = "/tmp/";
  if (strlen(t) + 1 > len) return 0;
  strcpy(buf, t);
  return (DWORD)strlen(buf);
}

extern "C" UINT GetTempFileNameA(const char *path, const char *prefix, UINT, char *buf)
{
  snprintf(buf, MAX_PATH, "%s/%s%u.tmp", path, prefix, (unsigned)getpid());
  return 1;
}

//////////////////////////////////////////////////////////////////////////////
// directory enumeration

extern "C" HANDLE FindFirstFileA(const char *spec, LPWIN32_FIND_DATAA data)
{
  std::string s(spec);
  size_t slash = s.find_last_of('/');
  std::string dirPath = (slash == std::string::npos) ? "." : s.substr(0, slash);
  std::string pattern = (slash == std::string::npos) ? s : s.substr(slash + 1);

  DIR *d = opendir(dirPath.c_str());
  if (!d)
  {
    g_lastError = (DWORD)errno;
    return INVALID_HANDLE_VALUE;
  }
  WinFindHandle *h = new WinFindHandle{kMagicFind, d, pattern, dirPath};
  struct dirent *e;
  while ((e = readdir(d)) != nullptr)
  {
    if (!WildcardMatch(pattern.c_str(), e->d_name)) continue;
    struct stat st;
    std::string full = dirPath + "/" + e->d_name;
    if (stat(full.c_str(), &st) != 0) continue;
    StatToFindData(st, e->d_name, data);
    return (HANDLE)h;
  }
  closedir(d);
  delete h;
  g_lastError = ENOENT;
  return INVALID_HANDLE_VALUE;
}

extern "C" BOOL FindNextFileA(HANDLE hh, LPWIN32_FIND_DATAA data)
{
  WinFindHandle *h = AsFind(hh);
  if (!h) return FALSE;
  struct dirent *e;
  while ((e = readdir(h->dir)) != nullptr)
  {
    if (!WildcardMatch(h->pattern.c_str(), e->d_name)) continue;
    struct stat st;
    std::string full = h->dirPath + "/" + e->d_name;
    if (stat(full.c_str(), &st) != 0) continue;
    StatToFindData(st, e->d_name, data);
    return TRUE;
  }
  return FALSE;
}

extern "C" BOOL FindClose(HANDLE hh)
{
  WinFindHandle *h = AsFind(hh);
  if (!h) return FALSE;
  closedir(h->dir);
  delete h;
  return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
// system info

extern "C" void GetSystemInfo(LPVOID info)
{
  SYSTEM_INFO *si = (SYSTEM_INFO *)info;
  memset(si, 0, sizeof(SYSTEM_INFO));
  si->wProcessorArchitecture = 9; // PROCESSOR_ARCHITECTURE_AMD64
  si->dwPageSize = 4096;
  si->dwAllocationGranularity = 65536;
  si->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
}

extern "C" void GetNativeSystemInfo(LPVOID info) { GetSystemInfo(info); }

extern "C" BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX m)
{
  memset(m, 0, sizeof(MEMORYSTATUSEX));
  m->dwLength = sizeof(MEMORYSTATUSEX);
  uint64_t mem = 0;
#if defined(__APPLE__)
  size_t len = sizeof(mem);
  bool haveSysctl = sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0;
#else
  bool haveSysctl = false;
#endif
  if (haveSysctl)
  {
    m->ullTotalPhys = mem;
    m->ullAvailPhys = mem / 2;
  }
  else
  {
    long pages = sysconf(_SC_PHYS_PAGES);
    long ps = sysconf(_SC_PAGESIZE);
    m->ullTotalPhys = (uint64_t)pages * ps;
#if defined(_SC_AVPHYS_PAGES)
    long avail = sysconf(_SC_AVPHYS_PAGES);
    m->ullAvailPhys = (uint64_t)avail * ps;
#else
    // macOS has no _SC_AVPHYS_PAGES; report the total rather than lying with 0.
    m->ullAvailPhys = m->ullTotalPhys;
#endif
  }
  m->ullTotalVirtual = m->ullTotalPhys;
  m->ullAvailVirtual = m->ullAvailPhys;
  m->ullTotalPageFile = m->ullTotalPhys;
  m->ullAvailPageFile = m->ullAvailPhys;
  return TRUE;
}

extern "C" void GlobalMemoryStatus(LPMEMORYSTATUS m)
{
  MEMORYSTATUSEX e;
  GlobalMemoryStatusEx(&e);
  m->dwTotalPhys = e.ullTotalPhys;
  m->dwAvailPhys = e.ullAvailPhys;
  m->dwTotalVirtual = e.ullTotalVirtual;
  m->dwAvailVirtual = e.ullAvailVirtual;
  m->dwMemoryLoad = e.dwMemoryLoad;
}

extern "C" BOOL SystemParametersInfoA(UINT, UINT, void *, UINT) { return TRUE; }

//////////////////////////////////////////////////////////////////////////////
// virtual memory

extern "C" LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
  int prot = PROT_READ | PROT_WRITE;
  if (protect == PAGE_EXECUTE_READWRITE) prot |= PROT_EXEC;
  if (type & MEM_RESERVE && !(type & MEM_COMMIT))
    return mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

extern "C" BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD)
{
  return munmap(addr, size) == 0;
}

extern "C" DWORD VirtualQuery(LPCVOID addr, LPVOID info, SIZE_T len)
{
  if (len < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
  MEMORY_BASIC_INFORMATION *m = (MEMORY_BASIC_INFORMATION *)info;
  memset(m, 0, sizeof(*m));
  m->BaseAddress = (PVOID)addr;
  m->RegionSize = 4096;
  m->State = MEM_COMMIT;
  m->Protect = PAGE_READWRITE;
  m->Type = MEM_PRIVATE;
  return sizeof(MEMORY_BASIC_INFORMATION);
}

//////////////////////////////////////////////////////////////////////////////
// multimedia timers (compat/MMSystem.h)

extern "C" DWORD timeGetTime(void) { return GetTickCount(); }
extern "C" UINT timeGetDevCaps(TIMECAPS *caps, UINT size)
{
  if (caps && size >= sizeof(TIMECAPS)) *caps = 1;
  return TIMERR_NOERROR;
}
extern "C" UINT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
extern "C" UINT timeEndPeriod(UINT) { return TIMERR_NOERROR; }

//////////////////////////////////////////////////////////////////////////////
// FormatMessageA: only the "format from system / from string" paths are used
// for building error strings, and the code tolerates failure.

extern "C" DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD messageId, DWORD, LPSTR buffer, DWORD size, va_list *)
{
  if (!buffer || !size) return 0;
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
  {
    char *heap = (char *)malloc(64);
    snprintf(heap, 64, "error %u", (unsigned)messageId);
    *(LPSTR *)buffer = heap;
    return (DWORD)strlen(heap);
  }
  snprintf(buffer, size, "error %u", (unsigned)messageId);
  return (DWORD)strlen(buffer);
}

//////////////////////////////////////////////////////////////////////////////
// floating point control word: the engine uses this to switch x87/SSE to
// 64-bit mantissa precision, which is the default on x86-64 anyway.

extern "C" unsigned int _controlfp_s(unsigned int *current, unsigned int newValue, unsigned int mask)
{
  unsigned int old = 0x0008001Fu; // default control word (PC_64, round near)
  if (current) *current = old;
  (void)newValue;
  (void)mask;
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
// global memory (only used for FormatMessage-style scratch buffers)

extern "C" HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes)
{
  (void)flags;
  return (HGLOBAL)malloc(bytes ? bytes : 1);
}
extern "C" HGLOBAL GlobalReAlloc(HGLOBAL h, SIZE_T bytes, UINT)
{
  return (HGLOBAL)realloc(h, bytes ? bytes : 1);
}
extern "C" LPVOID GlobalLock(HGLOBAL h) { return (LPVOID)h; }
extern "C" BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
extern "C" HGLOBAL GlobalFree(HGLOBAL h)
{
  free(h);
  return nullptr;
}
extern "C" SIZE_T GlobalSize(HGLOBAL)
{
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
// shlwapi path helpers (compat/shlwapi.h)
//
// The project loader uses these to resolve <songfile> (a Windows-style path
// such as ".\step010padding.mvm") against the directory of the .apx.

namespace
{
void NormaliseSlashes(char *p)
{
  for (; *p; ++p)
    if (*p == '\\') *p = '/';
}
} // namespace

extern "C" BOOL PathRemoveFileSpecA(char *path)
{
  if (!path) return FALSE;
  NormaliseSlashes(path);
  char *slash = strrchr(path, '/');
  if (!slash) return FALSE;
  *slash = 0;
  return TRUE;
}

extern "C" BOOL PathCombineA(char *dest, const char *dir, const char *file)
{
  if (!dest || !dir || !file) return FALSE;
  if (!*dir)
  {
    strcpy(dest, file);
  }
  else
  {
    size_t n = strlen(dir);
    bool needsSep = (dir[n - 1] != '/' && dir[n - 1] != '\\');
    snprintf(dest, MAX_PATH, "%s%s%s", dir, needsSep ? "/" : "", file);
  }
  // ".\x" and ".\\x" collapse to "x", matching what the loader expects.
  if (dest[0] == '.' && (dest[1] == '/' || dest[1] == '\\')) memmove(dest, dest + 2, strlen(dest + 2) + 1);
  NormaliseSlashes(dest);
  return TRUE;
}

extern "C" BOOL PathAppendA(char *path, const char *more)
{
  if (!path || !more) return FALSE;
  return PathCombineA(path, path, more);
}

extern "C" BOOL PathFileExistsA(const char *path)
{
  struct stat st;
  return path && stat(path, &st) == 0;
}

extern "C" BOOL PathIsRelativeA(const char *path)
{
  if (!path || !*path) return TRUE;
  if (path[0] == '/' || path[0] == '\\') return FALSE;
  if (isalpha((unsigned char)path[0]) && path[1] == ':') return FALSE;
  return TRUE;
}

extern "C" char *PathFindFileNameA(const char *path)
{
  if (!path) return nullptr;
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *best = slash > bslash ? slash : bslash;
  return (char *)(best ? best + 1 : path);
}

extern "C" void PathStripPathA(char *path)
{
  char *name = PathFindFileNameA(path);
  if (name && name != path) memmove(path, name, strlen(name) + 1);
}

extern "C" void PathRemoveExtensionA(char *path)
{
  if (!path) return;
  char *dot = strrchr(path, '.');
  char *slash = strrchr(path, '/');
  if (dot && (!slash || dot > slash)) *dot = 0;
}

extern "C" char *PathFindExtensionA(const char *path)
{
  if (!path) return nullptr;
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  if (!dot || (slash && dot < slash)) return (char *)(path + strlen(path));
  return (char *)dot;
}
