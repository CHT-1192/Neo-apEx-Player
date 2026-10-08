// Portable Win32 compatibility header for the apEx Vulkan player.
//
// The apEx demotool was written for Windows/MSVC.  Rather than rewriting the
// engine, we shadow <Windows.h> with this header (the 'compat' directory is
// placed first on the include path) and implement the small subset of the
// Win32 API the reusable code actually calls on top of POSIX.
//
// Design rules:
//   * TCHAR == char, _T(x) == x  -> all wide-char code paths are dead code.
//   * Calling convention keywords compile away on x86-64.
//   * Anything not implemented here is a link error, never a silent stub,
//     unless it is explicitly listed in src/platform/win32_compat.cpp.
#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <climits>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

//////////////////////////////////////////////////////////////////////////////
// calling conventions / declspec

#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef __thiscall
#define __thiscall
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef __declspec
#define __declspec(x)
#endif
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#ifndef _cdecl
#define _cdecl
#endif

//////////////////////////////////////////////////////////////////////////////
// fundamental types

// NOTE: fixed-width Windows types.  DWORD/LONG/ULONG must stay 32-bit even on
// LP64 hosts, otherwise struct layouts and file offsets silently diverge from
// the Windows behaviour the engine code expects.
typedef int                 BOOL;
typedef uint8_t             BYTE;
typedef uint16_t            WORD;
typedef uint32_t            DWORD;
typedef uint64_t            DWORDLONG;
typedef uint32_t            UINT;
typedef int32_t             INT;
typedef int32_t             LONG;
typedef uint32_t            ULONG;
typedef int64_t             LONGLONG;
typedef uint64_t            ULONGLONG;
typedef char                CHAR;
typedef wchar_t             WCHAR;
typedef CHAR                TCHAR;
typedef float               FLOAT;
typedef double              DOUBLE;
typedef void                VOID;

typedef BYTE                *PBYTE, *LPBYTE;
typedef WORD                *PWORD, *LPWORD;
typedef DWORD               *PDWORD, *LPDWORD;
typedef UINT                *PUINT;
typedef CHAR                *PCHAR, *LPSTR, *PSTR;
typedef const CHAR          *LPCSTR, *PCSTR;
typedef WCHAR               *LPWSTR, *PWSTR;
typedef const WCHAR         *LPCWSTR, *PCWSTR;
typedef TCHAR               *LPTSTR, *PTSTR;
typedef const TCHAR         *LPCTSTR, *PCTSTR;
typedef void                *LPVOID, *PVOID;
typedef const void          *LPCVOID, *PCVOID;
typedef size_t              SIZE_T;
typedef ptrdiff_t           SSIZE_T;

typedef intptr_t            INT_PTR;
typedef uintptr_t           UINT_PTR;
typedef intptr_t            LONG_PTR;
typedef uintptr_t           ULONG_PTR;
typedef UINT_PTR            WPARAM;
typedef LONG_PTR            LPARAM;
typedef LONG_PTR            LRESULT;
typedef DWORD               COLORREF;
typedef DWORD               ATOM;
typedef WORD                LANGID;

//////////////////////////////////////////////////////////////////////////////
// handles

typedef void                *HANDLE;
typedef void                *HWND;
typedef void                *HINSTANCE;
typedef void                *HMODULE;
typedef void                *HDC;
typedef void                *HGDIOBJ;
typedef void                *HICON;
typedef void                *HCURSOR;
typedef void                *HMENU;
typedef void                *HBRUSH;
typedef void                *HPEN;
typedef void                *HFONT;
typedef void                *HBITMAP;
typedef void                *HRGN;
typedef void                *HKEY;
typedef void                *HGLRC;
typedef void                *HGLOBAL;
typedef void                *HLOCAL;
typedef void                *HACCEL;
typedef void                *HDROP;
typedef void                *HMONITOR;
typedef void                *HPALETTE;
typedef void                *HKL;
typedef void                *HHOOK;
typedef void                *HRAWINPUT;
typedef void                *HWINSTA;
typedef void                *HDESK;
typedef void                *SOCKET;
// FARPROC is deliberately a plain void*: GetProcAddress() forwards to dlsym(),
// and the engine assigns its result straight to void* before casting it to the
// real function-pointer type (see CoRE2/DX11Shader.cpp:87).  A function-pointer
// typedef would make those assignments ill-formed in C++.
typedef void                *FARPROC;

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define HWND_DESKTOP         ((HWND)0)
#define HWND_TOPMOST         ((HWND)-1)
#define HWND_NOTOPMOST       ((HWND)-2)
#define HWND_TOP             ((HWND)0)
#define HWND_BOTTOM          ((HWND)1)

//////////////////////////////////////////////////////////////////////////////
// scalar constants

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#undef NULL
#ifndef NULL
#define NULL 0
#endif

#define MAX_PATH 260
#define MAX_PATHNAME 260
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFF
#define STILL_ACTIVE 259
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define CREATE_SUSPENDED 0x00000004
#define CP_ACP 0
#define CP_UTF8 65001
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONINFORMATION 0x40
#define MB_ICONWARNING 0x30
#define MB_YESNO 0x04
#define IDYES 6
#define IDNO 7
#define IDOK 1
#define IDCANCEL 2
#define GWLP_USERDATA (-21)
#define GWL_USERDATA (-21)
#define GWL_STYLE (-16)
#define SW_SHOW 5
#define SW_HIDE 0
#define SW_MAXIMIZE 3
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_READONLY 0x01
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define MAXIMUM_WAIT_OBJECTS 64

#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_COPY_ALLOWED 0x2

//////////////////////////////////////////////////////////////////////////////
// macros

#define MAKEWORD(a, b) ((WORD)(((BYTE)((a) & 0xff)) | ((WORD)((BYTE)((b) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)((a) & 0xffff)) | ((DWORD)((WORD)((b) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)((l) & 0xffff))
#define HIWORD(l) ((WORD)(((l) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)((w) & 0xff))
#define HIBYTE(w) ((BYTE)(((w) >> 8) & 0xff))
#define MAKELPARAM(a, b) ((LPARAM)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | (((WORD)((BYTE)(g))) << 8) | (((DWORD)((BYTE)(b))) << 16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb) >> 16))

#define _T(x) x
#define _TEXT(x) x
#define TEXT(x) x
#define __T(x) x
#define _tcslen strlen
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcscat strcat
#define _tcscmp strcmp
#define _tcsncmp strncmp
#define _tcsicmp strcasecmp
#define _tcsstr strstr
#define _tcsrchr strrchr
#define _tcsncpy_s strncpy_s
#define _stprintf sprintf
#define _sntprintf snprintf
#define _tfopen fopen
#define _ttoi atoi
#define _tstof atof
#define _tfopen_s fopen_s
#define _vsntprintf vsnprintf

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ZeroMemory
#define ZeroMemory(dst, len) memset((dst), 0, (len))
#endif
#ifndef CopyMemory
#define CopyMemory(dst, src, len) memcpy((dst), (src), (len))
#endif
#ifndef FillMemory
#define FillMemory(dst, len, val) memset((dst), (val), (len))
#endif
#ifndef MoveMemory
#define MoveMemory(dst, src, len) memmove((dst), (src), (len))
#endif
#define RtlZeroMemory(dst, len) memset((dst), 0, (len))
#define RtlCopyMemory(dst, src, len) memcpy((dst), (src), (len))

#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define MAX(a, b) max(a, b)
#define MIN(a, b) min(a, b)

//////////////////////////////////////////////////////////////////////////////
// secure CRT replacements abused by the MSVC code base

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

inline int sprintf_s(char *buf, size_t n, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, N, fmt, ap);
  va_end(ap);
  return r;
}
inline int _snprintf_s(char *buf, size_t n, size_t count, const char *fmt, ...)
{
  (void)count;
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
inline int vsprintf_s(char *buf, size_t n, const char *fmt, va_list ap) { return vsnprintf(buf, n, fmt, ap); }
inline int strcpy_s(char *dst, size_t n, const char *src)
{
  if (!n) return -1;
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
  return 0;
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char *src)
{
  return strcpy_s(dst, N, src);
}
inline int strcat_s(char *dst, size_t n, const char *src)
{
  size_t l = strlen(dst);
  if (l + 1 >= n) return -1;
  strncat(dst, src, n - l - 1);
  return 0;
}
template <size_t N>
inline int strcat_s(char (&dst)[N], const char *src)
{
  return strcat_s(dst, N, src);
}
inline int strncpy_s(char *dst, size_t n, const char *src, size_t count)
{
  if (!n) return -1;
  size_t c = (count == (size_t)-1) ? strlen(src) : count;
  if (c >= n) c = n - 1;
  memcpy(dst, src, c);
  dst[c] = 0;
  return 0;
}
inline int memcpy_s(void *dst, size_t n, const void *src, size_t count)
{
  if (count > n) return -1;
  memcpy(dst, src, count);
  return 0;
}
inline int fopen_s(FILE **f, const char *name, const char *mode)
{
  *f = fopen(name, mode);
  return *f ? 0 : -1;
}
inline int localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : -1; }
inline int gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : -1; }
inline char *strlwr(char *s)
{
  for (char *p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
  return s;
}
inline char *strupr(char *s)
{
  for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
  return s;
}
inline int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
inline int strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
inline int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
inline char *itoa(int v, char *buf, int radix)
{
  if (radix == 10)
    snprintf(buf, 32, "%d", v);
  else if (radix == 16)
    snprintf(buf, 32, "%x", v);
  else
    snprintf(buf, 32, "%o", v);
  return buf;
}
inline long long _atoi64(const char *s) { return atoll(s); }


// _vsctprintf: number of characters vsnprintf would emit (excluding the NUL).
inline int _vsctprintf(const char *fmt, va_list ap)
{
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  return n;
}
inline int _vscprintf(const char *fmt, va_list ap) { return _vsctprintf(fmt, ap); }
#define _vsctprintf _vsctprintf

//////////////////////////////////////////////////////////////////////////////
// HRESULT

typedef int32_t HRESULT;  // MSVC HRESULT is 32-bit; LP64 'long' is not
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_POINTER ((HRESULT)0x80004003L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | (unsigned long)(code)))
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x))

//////////////////////////////////////////////////////////////////////////////
// misc structs

#pragma pack(push, 8)
typedef struct _LARGE_INTEGER
{
  union
  {
    struct
    {
      DWORD LowPart;
      LONG HighPart;
    };
    LONGLONG QuadPart;
  };
} LARGE_INTEGER, *PLARGE_INTEGER;
#pragma pack(pop)

typedef struct _FILETIME
{
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME
{
  WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct tagPOINT
{
  LONG x, y;
} POINT, *PPOINT, *LPPOINT;

typedef struct tagSIZE
{
  LONG cx, cy;
} SIZE, *PSIZE, *LPSIZE;

typedef struct tagRECT
{
  LONG left, top, right, bottom;
} RECT, *PRECT, *LPRECT;

typedef struct _MEMORYSTATUS
{
  DWORD dwLength, dwMemoryLoad;
  SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile;
  SIZE_T dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _MEMORYSTATUSEX
{
  DWORD dwLength, dwMemoryLoad;
  DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
  DWORDLONG ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _SECURITY_ATTRIBUTES
{
  DWORD nLength;
  LPVOID lpSecurityDescriptor;
  BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED
{
  ULONG_PTR Internal, InternalHigh;
  DWORD Offset, OffsetHigh;
  HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _WIN32_FIND_DATAA
{
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
  DWORD dwReserved0, dwReserved1;
  CHAR cFileName[MAX_PATH];
  CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA, *PWIN32_FIND_DATA, *LPWIN32_FIND_DATA;

typedef struct _STARTUPINFOA
{
  DWORD cb;
  LPSTR lpReserved, lpDesktop, lpTitle;
  DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
  WORD wShowWindow, cbReserved2;
  LPBYTE lpReserved2;
  HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef STARTUPINFOA STARTUPINFO, *LPSTARTUPINFO;

typedef struct _PROCESS_INFORMATION
{
  HANDLE hProcess, hThread;
  DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct _GUID
{
  DWORD Data1;
  WORD Data2;
  WORD Data3;
  BYTE Data4[8];
} GUID;
typedef GUID IID;
typedef GUID CLSID;
typedef GUID UUID;

//////////////////////////////////////////////////////////////////////////////
// string conversion helpers (implemented in src/platform/win32_compat.cpp)

#ifdef __cplusplus
extern "C" {
#endif

DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void Sleep(DWORD ms);
void OutputDebugStringA(const char *s);
void OutputDebugStringW(const wchar_t *s);
DWORD GetLastError(void);
void SetLastError(DWORD e);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
void DebugBreak(void);
DWORD GetModuleFileNameA(HMODULE m, char *buf, DWORD n);
HMODULE GetModuleHandleA(const char *name);
HMODULE LoadLibraryA(const char *name);
FARPROC GetProcAddress(HMODULE m, const char *name);
BOOL FreeLibrary(HMODULE m);
int MultiByteToWideChar(UINT cp, DWORD flags, const char *in, int inLen, wchar_t *out, int outLen);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *in, int inLen, char *out, int outLen, const char *def, BOOL *used);
DWORD GetFullPathNameA(const char *name, DWORD len, char *buf, char **filePart);
DWORD GetCurrentDirectoryA(DWORD len, char *buf);
BOOL SetCurrentDirectoryA(const char *path);
BOOL CreateDirectoryA(const char *path, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryA(const char *path);
BOOL DeleteFileA(const char *path);
BOOL CopyFileA(const char *src, const char *dst, BOOL failIfExists);
BOOL MoveFileA(const char *src, const char *dst);
BOOL MoveFileExA(const char *src, const char *dst, DWORD flags);
DWORD GetFileAttributesA(const char *path);
BOOL SetFileAttributesA(const char *path, DWORD attr);
DWORD GetFileSize(HANDLE f, DWORD *high);
BOOL GetFileSizeEx(HANDLE f, LARGE_INTEGER *size);
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE f, LPVOID buf, DWORD toRead, DWORD *read, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD toWrite, DWORD *written, LPOVERLAPPED ov);
BOOL CloseHandle(HANDLE h);
DWORD SetFilePointer(HANDLE f, LONG dist, LONG *high, DWORD method);
BOOL SetEndOfFile(HANDLE f);
BOOL FlushFileBuffers(HANDLE f);
HANDLE FindFirstFileA(const char *spec, LPWIN32_FIND_DATAA data);
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
BOOL FindClose(HANDLE h);
BOOL GetLocalTime(LPSYSTEMTIME st);
BOOL GetSystemTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL QueryPerformanceCounter(LARGE_INTEGER *v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *v);
void GetSystemInfo(LPVOID info);
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX m);
void GlobalMemoryStatus(LPMEMORYSTATUS m);
void InitializeCriticalSection(LPVOID cs);
void DeleteCriticalSection(LPVOID cs);
void EnterCriticalSection(LPVOID cs);
void LeaveCriticalSection(LPVOID cs);
BOOL TryEnterCriticalSection(LPVOID cs);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, const char *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, const char *name);
BOOL ReleaseSemaphore(HANDLE h, LONG count, LONG *prev);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, const char *name);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize, DWORD(__stdcall *start)(LPVOID), LPVOID param, DWORD flags, DWORD *threadId);
DWORD ResumeThread(HANDLE h);
BOOL TerminateThread(HANDLE h, DWORD code);
DWORD GetExitCodeThread(HANDLE h, DWORD *code);
void ExitThread(DWORD code);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL TerminateProcess(HANDLE h, UINT code);
void ExitProcess(UINT code);
DWORD WaitForInputIdle(HANDLE h, DWORD ms);
LONG InterlockedIncrement(volatile LONG *v);
LONG InterlockedDecrement(volatile LONG *v);
LONG InterlockedExchange(volatile LONG *v, LONG val);
LONG InterlockedCompareExchange(volatile LONG *v, LONG exchange, LONG comparand);
LONG InterlockedExchangeAdd(volatile LONG *v, LONG add);
void *InterlockedExchangePointer(void *volatile *target, void *value);
int MessageBoxA(HWND h, const char *text, const char *caption, UINT type);
HMODULE GetModuleHandleW(const wchar_t *name);
DWORD GetTempPathA(DWORD len, char *buf);
UINT GetTempFileNameA(const char *path, const char *prefix, UINT unique, char *buf);
BOOL IsDebuggerPresent(void);
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
DWORD VirtualQuery(LPCVOID addr, LPVOID info, SIZE_T len);
void GetNativeSystemInfo(LPVOID info);
BOOL SystemParametersInfoA(UINT action, UINT param, void *data, UINT winIni);

#ifdef __cplusplus
}
#endif

#define VIRTUAL_ALLOC_RESERVE 0x3000
#define VIRTUAL_ALLOC_COMMIT 0x1000
#define VIRTUAL_ALLOC_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_DECOMMIT 0x4000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_IMAGE 0x1000000


//////////////////////////////////////////////////////////////////////////////
// CRITICAL_SECTION (pthread backed, see src/platform/win32_compat.cpp)

typedef struct _RTL_CRITICAL_SECTION
{
  pthread_mutex_t m;
} CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

//////////////////////////////////////////////////////////////////////////////
// floating point control (FloatControl.cpp)

extern "C" unsigned int _controlfp_s(unsigned int *current, unsigned int newValue, unsigned int mask);
#define _MCW_PC 0x00030000u
#define _PC_24 0x00020000u
#define _PC_53 0x00010000u
#define _PC_64 0x00000000u
#define _RC_NEAR 0x00000000u
#define _RC_CHOP 0x00000300u
#define _RC_UP 0x00000200u
#define _RC_DOWN 0x00000100u
#define _MCW_RC 0x00000300u
#define _DN_SAVE 0x00000000u

//////////////////////////////////////////////////////////////////////////////
// format-message flags (Logger/StreamWriter error text)

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE 0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_ARGUMENT_ARRAY 0x00002000
#define LANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01

#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
extern "C" DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD messageId, DWORD langId, LPSTR buffer, DWORD size, va_list *args);
#define FormatMessage FormatMessageA
#define LocalFree(h) free(h)


#define MAKELANGID(p, s) ((WORD)(((WORD)((s)) << 10) | (WORD)((p))))
#define NO_ERROR 0L
#define ERROR_SUCCESS 0L
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_APPEND_DATA 0x0004
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
extern "C" HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes);
extern "C" HGLOBAL GlobalReAlloc(HGLOBAL h, SIZE_T bytes, UINT flags);
extern "C" LPVOID GlobalLock(HGLOBAL h);
extern "C" BOOL GlobalUnlock(HGLOBAL h);
extern "C" HGLOBAL GlobalFree(HGLOBAL h);
extern "C" SIZE_T GlobalSize(HGLOBAL h);

//////////////////////////////////////////////////////////////////////////////
// ANSI aliases: the engine is built without UNICODE, exactly like the original
// MSVC tool project.

#define CreateFile CreateFileA
#define CreateDirectory CreateDirectoryA
#define DeleteFile DeleteFileA
#define CopyFile CopyFileA
#define MoveFile MoveFileA
#define MoveFileEx MoveFileExA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA
#define GetFileAttributes GetFileAttributesA
#define SetFileAttributes SetFileAttributesA
#define GetFullPathName GetFullPathNameA
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetModuleFileName GetModuleFileNameA
#define GetModuleHandle GetModuleHandleA
#define LoadLibrary LoadLibraryA
#define GetTempPath GetTempPathA
#define GetTempFileName GetTempFileNameA
#define MessageBox MessageBoxA
#define OutputDebugString OutputDebugStringA
#define CreateEvent CreateEventA
#define CreateSemaphore CreateSemaphoreA
#define CreateMutex CreateMutexA
#define SystemParametersInfo SystemParametersInfoA
#define WIN32_FIND_DATA WIN32_FIND_DATAA
#define LPWIN32_FIND_DATA LPWIN32_FIND_DATAA

//////////////////////////////////////////////////////////////////////////////
// secure CRT additions used by StreamReader/Archive

inline size_t fread_s(void *buf, size_t bufSize, size_t elemSize, size_t count, FILE *f)
{
  size_t maxElems = elemSize ? bufSize / elemSize : 0;
  if (count > maxElems) count = maxElems;
  return fread(buf, elemSize, count, f);
}
inline int _vsntprintf_s(char *buf, size_t n, size_t count, const char *fmt, va_list ap)
{
  (void)count;
  return vsnprintf(buf, n, fmt, ap);
}

// GDI declarations live in their own header (see src/platform/gdi_posix.cpp);
// it is created by the platform work and included automatically once present.
#if defined(__has_include)
#if __has_include("wingdi.h")
#include "wingdi.h"
#endif
#endif

#pragma push_macro("new")
#undef new

typedef struct _MEMORY_BASIC_INFORMATION
{
  PVOID BaseAddress, AllocationBase;
  DWORD AllocationProtect;
  SIZE_T RegionSize;
  DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO
{
  union
  {
    DWORD dwOemId;
    struct
    {
      WORD wProcessorArchitecture;
      WORD wReserved;
    };
  };
  DWORD dwPageSize;
  LPVOID lpMinimumApplicationAddress;
  LPVOID lpMaximumApplicationAddress;
  ULONG_PTR dwActiveProcessorMask;
  DWORD dwNumberOfProcessors;
  DWORD dwProcessorType;
  DWORD dwAllocationGranularity;
  WORD wProcessorLevel;
  WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

#pragma pop_macro("new")

//////////////////////////////////////////////////////////////////////////////
// Additions required by Bedrock/CoRE2 that were missing from the first pass of
// this header.  Kept together at the end of the file so that concurrent edits
// to the sections above do not collide.
//////////////////////////////////////////////////////////////////////////////

// rpcndr.h/minwindef.h spelling used by the D3D/DDS helpers
// (CoRE2/DDSTextureLoader.h takes `const byte*`).
#ifndef _DSH_BYTE_DEFINED
#define _DSH_BYTE_DEFINED
typedef unsigned char byte;
#endif

// ShowWindow()/SetWindowPlacement() command values.  SW_MAXIMIZE already exists
// as an alias of SW_SHOWMAXIMIZED in the Win32 headers, so mirror that here.
#ifndef SW_SHOWNORMAL
#define SW_SHOWNORMAL 1
#endif
#ifndef SW_SHOWMINIMIZED
#define SW_SHOWMINIMIZED 2
#endif
#ifndef SW_SHOWMAXIMIZED
#define SW_SHOWMAXIMIZED 3
#endif
#ifndef SW_SHOWMINNOACTIVE
#define SW_SHOWMINNOACTIVE 7
#endif
#ifndef SW_SHOWNA
#define SW_SHOWNA 8
#endif
#ifndef SW_SHOWDEFAULT
#define SW_SHOWDEFAULT 10
#endif
#ifndef SW_FORCEMINIMIZE
#define SW_FORCEMINIMIZE 11
#endif

// WINDOWPLACEMENT flags
#ifndef WPF_SETMINPOSITION
#define WPF_SETMINPOSITION 0x0001
#endif
#ifndef WPF_RESTORETOMAXIMIZED
#define WPF_RESTORETOMAXIMIZED 0x0002
#endif
#ifndef WPF_ASYNCWINDOWPLACEMENT
#define WPF_ASYNCWINDOWPLACEMENT 0x0004
#endif

// CCoreWindowHandlerWin stores one of these by value (WindowHandler.h), so the
// real field order/layout matters.
typedef struct tagWINDOWPLACEMENT
{
  UINT  length;
  UINT  flags;
  UINT  showCmd;
  POINT ptMinPosition;
  POINT ptMaxPosition;
  RECT  rcNormalPosition;
} WINDOWPLACEMENT, *PWINDOWPLACEMENT, *LPWINDOWPLACEMENT;

//////////////////////////////////////////////////////////////////////////////
// Message loop (PeekMessage/TranslateMessage/DispatchMessage)
//
// The vendored Phoenix/Project.cpp and CoRE2/WindowHandler.cpp run the classic
// Win32 pump.  The definitions come from the SDL-backed window layer
// (src/platform/window_sdl.cpp).
//////////////////////////////////////////////////////////////////////////////

#ifndef PM_NOREMOVE
#define PM_NOREMOVE 0x0000
#endif
#ifndef PM_REMOVE
#define PM_REMOVE 0x0001
#endif
#ifndef PM_NOYIELD
#define PM_NOYIELD 0x0002
#endif

// Used by Phoenix/Project.cpp to size the default window.  SM_CXSCREEN /
// SM_CYSCREEN are already defined above.
int GetSystemMetrics(int nIndex);

typedef struct tagMSG
{
  HWND   hwnd;
  UINT   message;
  WPARAM wParam;
  LPARAM lParam;
  DWORD  time;
  POINT  pt;
} MSG, *PMSG, *LPMSG;

BOOL PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL TranslateMessage(const MSG *lpMsg);
LRESULT DispatchMessageA(const MSG *lpMsg);

#ifndef PeekMessage
#define PeekMessage PeekMessageA
#endif
#ifndef DispatchMessage
#define DispatchMessage DispatchMessageA
#endif

//////////////////////////////////////////////////////////////////////////////
// DEVMODE / POINTL
//
// third_party/apEx/MinimalPlayer/SetupDialog.h (reached from Phoenix/Project.h)
// stores a DEVMODE to describe the chosen display mode, and uses the
// DM_* / ENUM_CURRENT_SETTINGS constants with ChangeDisplaySettingsA().  Print
// and ICM members are kept so the struct matches the Win32 layout; the engine
// only ever touches dmSize/dmFields/dmBitsPerPel/dmPelsWidth/dmPelsHeight/
// dmDisplayFrequency.
//////////////////////////////////////////////////////////////////////////////

#ifndef CCHDEVICENAME
#define CCHDEVICENAME 32
#endif
#ifndef CCHFORMNAME
#define CCHFORMNAME 32
#endif

typedef struct _POINTL
{
  LONG x;
  LONG y;
} POINTL, *PPOINTL;

#define DM_ORIENTATION        0x00000001L
#define DM_PAPERSIZE          0x00000002L
#define DM_PAPERLENGTH        0x00000004L
#define DM_PAPERWIDTH         0x00000008L
#define DM_SCALE              0x00000010L
#define DM_COPIES             0x00000100L
#define DM_DEFAULTSOURCE      0x00000200L
#define DM_PRINTQUALITY       0x00000400L
#define DM_COLOR              0x00000800L
#define DM_DUPLEX             0x00001000L
#define DM_YRESOLUTION        0x00002000L
#define DM_TTOPTION           0x00004000L
#define DM_COLLATE            0x00008000L
#define DM_FORMNAME           0x00010000L
#define DM_LOGPIXELS          0x00020000L
#define DM_BITSPERPEL         0x00040000L
#define DM_PELSWIDTH          0x00080000L
#define DM_PELSHEIGHT         0x00100000L
#define DM_DISPLAYFLAGS       0x00200000L
#define DM_DISPLAYFREQUENCY   0x00400000L
#define DM_ICMMETHOD          0x00800000L
#define DM_ICMINTENT          0x01000000L
#define DM_MEDIATYPE          0x02000000L
#define DM_DITHERTYPE         0x04000000L
#define DM_PANNINGWIDTH       0x08000000L
#define DM_PANNINGHEIGHT      0x10000000L

#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)

typedef struct _devicemodeA
{
  BYTE  dmDeviceName[CCHDEVICENAME];
  WORD  dmSpecVersion;
  WORD  dmDriverVersion;
  WORD  dmSize;
  WORD  dmDriverExtra;
  DWORD dmFields;
  union
  {
    struct
    {
      short dmOrientation;
      short dmPaperSize;
      short dmPaperLength;
      short dmPaperWidth;
      short dmScale;
      short dmCopies;
      short dmDefaultSource;
      short dmPrintQuality;
    };
    struct
    {
      POINTL dmPosition;
      DWORD  dmDisplayOrientation;
      DWORD  dmDisplayFixedOutput;
    };
  };
  short dmColor;
  short dmDuplex;
  short dmYResolution;
  short dmTTOption;
  short dmCollate;
  BYTE  dmFormName[CCHFORMNAME];
  WORD  dmLogPixels;
  DWORD dmBitsPerPel;
  DWORD dmPelsWidth;
  DWORD dmPelsHeight;
  union
  {
    DWORD dmDisplayFlags;
    DWORD dmNup;
  };
  DWORD dmDisplayFrequency;
  DWORD dmICMMethod;
  DWORD dmICMIntent;
  DWORD dmMediaType;
  DWORD dmDitherType;
  DWORD dmReserved1;
  DWORD dmReserved2;
  DWORD dmPanningWidth;
  DWORD dmPanningHeight;
} DEVMODEA, *PDEVMODEA, *LPDEVMODEA;

typedef DEVMODEA DEVMODE, *PDEVMODE, *LPDEVMODE;

// Window-management entry points used by CCoreDX11Device::Initialize() and by
// CCoreWindowHandlerWin.  Declared here (implemented in
// src/platform/win32_compat.cpp, per this header's "unimplemented means a link
// error" rule).
BOOL ShowWindow(HWND hWnd, int nCmdShow);
HWND SetFocus(HWND hWnd);
BOOL SetForegroundWindow(HWND hWnd);
BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT *lpwndpl);
BOOL SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT *lpwndpl);

// The engine uses _com_error (from <comdef.h>) in the CoRE2 DX11/DX9 backends
// without including comdef.h itself - on MSVC it leaked in through the OLE
// headers that BaseLib pulls in.  There is no OLE here, so close the loop
// explicitly.  comdef.h includes this file first, but "#pragma once" makes that
// recursive include a no-op, so both orders work.
#include "comdef.h"

#endif // !_WIN32
