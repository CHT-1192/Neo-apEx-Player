# Platform Port Spec — apEx → macOS / clang++ / libc++ (Vulkan player)

Status: analysis-only. **Nothing in this document was produced by compiling the apEx tree.**
Where a claim was verified empirically it was done with tiny standalone `clang++` snippets, and the
snippet + result is recorded in [Appendix A](#appendix-a--empirical-verification-log).

Source (READ-ONLY): `~/Repos/apEx-public/`
Target: `Bedrock/BaseLib`, `Bedrock/UtilLib`, `apEx/Phoenix`, `apEx/Phoenix_Tool`
Excluded: `Bedrock/Whiteboard`, the `apEx/apEx` tool UI, `Bedrock/CoRE2` (unless noted as a dependency)
This project already owns: `compat/*.h` (Win32 header shadows) and `tools/apply_patches.py`.

Line references are relative to the **source root**, e.g. `Bedrock/BaseLib/String.cpp:701`.
References to `compat/…` and `third_party/…` are relative to the project root, and are given by
symbol name as well as line number because `compat/` is being actively edited.

**Vendored layout (verified):** `third_party/apEx/` holds `Bedrock/{BaseLib,CoRE2,UtilLib,Whiteboard}`,
`Phoenix/`, `Phoenix_Tool/`, `LibCTiny/`, `MVX/`, `apEx/MVX/`. So source `apEx/Phoenix/x.cpp` →
`third_party/apEx/Phoenix/x.cpp`. Beware that the source tree has **two** MVX directories
(`apEx/MVX/` for `Phoenix/phxEngine.h:11`, and `apEx/Phoenix/MVX/` for
`Phoenix_Tool/Audio/phxSongMVM.cpp:3`) and they are vendored to *different* places — see §12.1 D1.
`Bedrock/CoRE2` *is* vendored even though it is out of scope, because `phxResource.h:2`,
`RenderTargets.h:2`, `Texgen_tool.h:2` and `apxProject.cpp:3` need it.

> **State at time of writing.** Several items below were already fixed while this analysis ran.
> §12 tracks the delta against the live `compat/` + `third_party/` trees so this document does not
> send anyone chasing closed issues.

---

## 0. Sizes and headline numbers

| directory | files (.cpp/.h/.c/.hpp, excl. ExternalLibraries) | lines | Win32-touching files |
|---|---|---|---|
| `Bedrock/BaseLib` | 73 | 9 957 | 24 |
| `Bedrock/UtilLib` | 18 (72 OpenSSL headers additionally) | 21 660 | 6 |
| `apEx/Phoenix` | 38 | 14 287 | 10 |
| `apEx/Phoenix_Tool` | 43 | 16 754 | 18 |

Four facts dominate everything else in this document:

1. **`apEx/Phoenix` (the runtime) has zero `TCHAR`, zero `_T()`, zero `CString`.** Verified: `grep -rn 'TCHAR\|LPCTSTR\|LPTSTR' apEx/Phoenix/` → no hits; `grep -rn '_T(' apEx/Phoenix/` → no hits.
   The runtime is already ANSI/`char`. All 1 205 `_T()` uses and all `CString` traffic live in `Phoenix_Tool`.
2. **`BaseLib`/`UtilLib`/`Phoenix_Tool` are officially built `MultiByte`**, i.e. `TCHAR == char`,
   `_T(x) == x`. `CharacterSet=MultiByte` at `apEx/Phoenix/Phoenix.vcxproj:23,30`,
   `apEx/Phoenix_Tool/Phoenix_Tool.vcxproj:257,264`, and for `BaseLib`/`UtilLib` the
   `Debug_MultiByte|Release_MultiByte|*64` configurations (`Bedrock/BaseLib/BaseLib.vcxproj:153,192`;
   `Bedrock/UtilLib/UtilLib.vcxproj:97,136`). `BaseLib` *also* offers `Unicode` configs
   (`BaseLib.vcxproj:141,178`) — **pick MultiByte and never look back.** The wide-char overloads of
   `CString` remain reachable but are dead code.
3. **The single highest-leverage change is the MSXML/COM XML layer.** `Bedrock/UtilLib/XMLNode.h:5`
   includes a checked-in 3 037-line generated type-library header `msxml3.tlh`; `XMLDocument`/`XMLNode`
   are raw `MSXML2::IXMLDOMDocument2` COM. Via `apEx/Phoenix_Tool/phxResource.h:3` and
   `apEx/Phoenix_Tool/phxSplineExt.h:11` this contaminates **every Phoenix_Tool header**
   (`Material_Tool.h`, `Model_Tool.h`, `Scene_tool.h`, `Texgen_tool.h`, `RenderTargets.h`,
   `RenderLayer.h`, `TreeSpecies.h`) and therefore `apxProject.h`. RapidXML is already vendored,
   unused, and portable → reimplement `CXMLDocument`/`CXMLNode` on RapidXML with the same API.
4. **`apEx/Phoenix` is not "portable engine + backend seam"; it is a D3D11 renderer.** It calls
   `ID3D11*`/`IDXGI*`/`D3D11_*` directly in 25 files (243 hits in `Mesh.cpp`, 135 in `Scene.cpp`,
   73 in `phxEngine.cpp`) and uses ~35 `D3DX10Math` free functions plus `D3DXVECTOR3` (344 uses),
   `D3DXMATRIX` (215), `D3DXFLOAT16` (108), `D3DXQUATERNION` (52). There is **no** vtable seam to
   implement behind. §9 sizes this honestly.

**Two blocking defects existed in the `third_party/` vendoring** and were fixed while this document was
being written (details in §12.1): `third_party/apEx/MVX/` was missing while `Phoenix/phxEngine.h:11`
includes it (D1), and `LibCTiny/libcminimal.h` — a 20-line header included by four runtime files — was
never vendored (D2). **§12.3 tracks which of the patches below are still open.**

---

## 1. Inventory: files that include `<windows.h>`/winsock or use Win32 types

### 1.1 Direct `#include` of a Windows-only header

| file:line | include | avoidable? |
|---|---|---|
| `Bedrock/BaseLib/BaseLib.h:10` | `<WinSock2.h>` | **Yes** — only `Socket.h` needs it; `Socket.cpp`/`SocketSSL.cpp` are the sole consumers. Drop the include and the `Socket.h` include (already done in `apply_patches.py`). |
| `Bedrock/BaseLib/BaseLib.h:11` | `<Windows.h>` | No — replaced by `compat/Windows.h`. |
| `Bedrock/BaseLib/BaseLib.h:12` | `<comdef.h>` | **Yes** — only `ImageDecompressor.cpp` uses `_com_error`. |
| `Bedrock/BaseLib/BaseLib.h:14` | `<tchar.h>` | No — replaced by `compat/tchar.h`. |
| `Bedrock/BaseLib/Memory.cpp:7,9` | `<windows.h>`, `<tchar.h>` | Only under `#ifdef MEMORY_TRACKING` (i.e. `_DEBUG`). **Avoidable by not defining `_DEBUG`.** |
| `Bedrock/BaseLib/CriticalSection.cpp:2` | `<Windows.h>` | No — needs `InterlockedCompareExchange`, `GetCurrentThreadId`, `Sleep`. |
| `Bedrock/BaseLib/Timer.cpp:2` | `<MMSystem.h>` + `#pragma comment(lib,"winmm.lib")` (`:3`) | No — `timeGetTime` is the engine's clock. Shim or swap to `mach_absolute_time`. |
| `Bedrock/BaseLib/Profile.cpp:2,3` | `<MMSystem.h>`, winmm | Same. |
| `Bedrock/BaseLib/Socket.cpp:2,3` | `BaseLib.h`, `<MMSystem.h>` | **Yes** — drop `Socket.cpp` from the build (nothing in the runtime uses `CSocket`). |
| `apEx/Phoenix/MVX/mvx.h:21,22,25` | `<windows.h>`, `<mmsystem.h>`, `<dsound.h>` | **Yes, but only by replacing MVX** — see §9.4. `phxEngine.h:11` includes `mvx.h`, so it is pulled into the runtime. |
| `apEx/Phoenix/phxEngine.h:9,10` | `<d3d11.h>`, `<D3DX11.h>` | No — core renderer. |
| `apEx/Phoenix_Tool/phxSplineExt.h:6` | `<d3d10_1.h>` | Yes — used only for a D3D10 type in a tool-only path. |
| `Bedrock/UtilLib/XMLNode.h:5` | `"msxml3.tlh"` | No — must be replaced (§1.3). |
| `apEx/Phoenix_Tool/Audio/phxSound.h:3,4` | `<mmsystem.h>`, `<dsound.h>` | See §9.4. |
| `apEx/Phoenix/{Mesh.cpp:5, Texgen.cpp:3, Project.cpp:13, Scene.cpp:2}` | `"…/LibCTiny/libcminimal.h"` | ✅ handled — the 20-line header was vendored to `third_party/apEx/LibCTiny/` and the two backslash includes were rewritten (was a hard clang error; see §12.1 D2). |
| `apEx/Phoenix/Mesh.cpp:1790,1791` | `<gl\gl.h>`, `<gl\glu.h>` | No — but they are **hard clang errors** (backslashes) and the GLU tessellator has no Apple-supplied replacement; see §9.3. |
| `apEx/Phoenix/phxMath.h:4`, `Mesh.h:7` | `<D3DX10Math.h>` | No — must be replaced by a portable shim; the bundled header declares `WINAPI` functions that live in `d3dx10.dll`. See §9.1. |
| `apEx/Phoenix/phxEngine.h:11` | `"../MVX/mvx.h"` | **Yes** — currently a missing header; see §12.1 D1. |
| `apEx/Phoenix_Tool/Audio/*.h` (5 files) | `"..\..\..\Bedrock\BaseLib\BaseLib.h"` | **Yes** — backslashes → hard clang errors; the whole Audio directory is droppable. |
| `apEx/Phoenix_Tool/{Material_Tool.cpp:1, apxProject.cpp:1, Timeline_tool.cpp:1, phxSplineExt.h:2}` | `#define _WINSOCKAPI_` | **Yes** — only exists to reorder `<winsock2.h>` before `<windows.h>`; dead once sockets are gone (P17). |

### 1.2 Per-file Win32 usage census

Column meanings: `TCHAR` = `TCHAR|_T(|LPCTSTR|LPTSTR|_tcs*`; `IO` = `HANDLE/CreateFile/ReadFile/WriteFile/FindFirstFile/WIN32_FIND_DATA`; `GDI/COM` = GDI, OLE, `IPicture`, `GlobalAlloc`; `types` = the scalar/typedef soup (`DWORD/BOOL/HRESULT/LPVOID/ZeroMemory/OutputDebugString/TRUE/FALSE/...`).
Counts are non-comment occurrences.

| file | TCHAR | HWND/HINST | IO | GDI/COM | net | thread | types | timer |
|---|---|---|---|---|---|---|---|---|
| `Bedrock/BaseLib/String.cpp` | 91 | | | | | | 2 | |
| `Bedrock/BaseLib/String.h` | 21 | | | | | | | |
| `Bedrock/BaseLib/Logger.h` | 12 | | | | | | | |
| `Bedrock/BaseLib/Logger.cpp` | 18 | | | | | | 2 | |
| `Bedrock/BaseLib/StreamReader.cpp` | 2 | | 19 | | | | 6 | |
| `Bedrock/BaseLib/StreamReader.h` | 2 | | 1 | | | | | |
| `Bedrock/BaseLib/StreamWriter.cpp` | 6 | | 12 | | | | 2 | |
| `Bedrock/BaseLib/StreamWriter.h` | 3 | | 1 | | | | | |
| `Bedrock/BaseLib/Archive.cpp` | 13 | | 3 | | | | | |
| `Bedrock/BaseLib/Archive.h` | | | 1 | | | | | |
| `Bedrock/BaseLib/FileList.cpp` | | | 10 | | | | 4 | |
| `Bedrock/BaseLib/CrashTracker.cpp` | 10 | | 10 | | | 2 | 8 | |
| `Bedrock/BaseLib/CrashTracker.h` | | | | | | | 4 | |
| `Bedrock/BaseLib/StackTracker.cpp` | 6 | | | | | | 11 | |
| `Bedrock/BaseLib/ImageDecompressor.cpp` | 3 | | | **15** | | | 6 | |
| `Bedrock/BaseLib/Socket.cpp` | 6 | | | | **12** | | | 6 |
| `Bedrock/BaseLib/Socket.h` | | | | | 2 | | | |
| `Bedrock/BaseLib/CriticalSection.cpp` | | | | | | **3** | 3 | |
| `Bedrock/BaseLib/Assert.h` | | | | | | | 4 | |
| `Bedrock/BaseLib/Memory.cpp` | 2 | | | | | | 6 | |
| `Bedrock/BaseLib/Timer.cpp` | | | | | | | | 2 |
| `Bedrock/BaseLib/Profile.cpp` | 1 | | | | | | | 2 |
| `Bedrock/BaseLib/EnumHelpers.{h,cpp}` | 3+3 | | | | | | | |
| `Bedrock/BaseLib/Thread.h` | 1 (bad include) | | | | | | | |
| `Bedrock/UtilLib/XMLNode.cpp` | 42 | | | COM | | | 12 | |
| `Bedrock/UtilLib/XMLNode.h` | 17 | | | COM | | | | |
| `Bedrock/UtilLib/XMLDocument.cpp` | 2 | | 6 | COM | | | 13 | |
| `Bedrock/UtilLib/XMLDocument.h` | 2 | | | COM | | | 2 | |
| `Bedrock/UtilLib/SocketSSL.cpp` | 1 | | | | | | | |
| `Bedrock/UtilLib/stb_vorbis.c` | | | | | | | 82 (types) | |
| `apEx/Phoenix/Texgen.cpp` | | | | **17** | | | 8 | |
| `apEx/Phoenix/Mesh.cpp` | | | | 1 (+GDI font, GLU) | | | 9 | |
| `apEx/Phoenix/Project.cpp` | | | | | | | 4 | 2 |
| `apEx/Phoenix/phxEngine.{h,cpp}` | | 1 | | | | | 9 | 1 |
| `apEx/Phoenix/Project.h` | | | | | | | | |
| `apEx/Phoenix/MVX/mvx.h` | | 3 | | | | | 8 | |
| `apEx/Phoenix_Tool/Texgen_tool.cpp` | **226** | | | | | | 1 | |
| `apEx/Phoenix_Tool/Model_Tool.cpp` | **185** | | | | | | | |
| `apEx/Phoenix_Tool/Scene_tool.cpp` | **167** | | | | | | 1 | |
| `apEx/Phoenix_Tool/apxProject.cpp` | **157** | 4 | 6 | | | | 7 | |
| `apEx/Phoenix_Tool/TreeSpecies.cpp` | **132** | | | | | | | |
| `apEx/Phoenix_Tool/Timeline_tool.cpp` | **112** | | | | | | | |
| `apEx/Phoenix_Tool/Material_Tool.cpp` | **75** | | | | | | | |
| `apEx/Phoenix_Tool/phxSplineExt.cpp` | 54 | | | | | | 7 | |
| `apEx/Phoenix_Tool/TexgenPage.cpp` | 31 | | | | | | | |
| `apEx/Phoenix_Tool/RenderTargets.cpp` | 22 | | | | | | 1 | |
| `apEx/Phoenix_Tool/RenderLayer.cpp` | 22 | | | | | | | |
| `apEx/Phoenix_Tool/apxProject.h` | | 5 (HWND) | | | | | | |
| `apEx/Phoenix_Tool/GUID.cpp` | | | | | | | | 1 |
| `apEx/Phoenix_Tool/Audio/phxSound.{h,cpp}` | | 1 | | DSound | | | 2 | |
| `apEx/Phoenix_Tool/Audio/phxSongBase.cpp` | 2 | | 1 | | | 1 | 6 | |
| `apEx/Phoenix_Tool/Audio/phxSongBase.h` | | | | DSound | | | | |

**Avoidable files (drop from the build, no patching):**
`BaseLib/Socket.cpp`, `BaseLib/Socket.h`, `BaseLib/ImageDecompressor.cpp` (GDI+/OLE),
`BaseLib/CrashTracker.cpp/.h` (SEH + TEB), `BaseLib/StackTracker.cpp` (DbgHelp),
`BaseLib/FileList.cpp` (only `FindFirstFile`; nothing in the runtime calls `CFileList`),
`UtilLib/SocketSSL.cpp` + `Bedrock/UtilLib/ExternalLibraries/openssl/` (72 files of OpenSSL 0.9.8-era
headers + two MSVC `.lib` binaries, `libeay32.lib`/`ssleay32.lib`, and `SSLv3_method()` which modern
OpenSSL removed) — nothing in Bedrock/apEx uses it,
`UtilLib/XMLNode.cpp` + `XMLDocument.cpp` + `msxml3.tlh` (replaced),
`UtilLib/jsonxx.cpp` (verified unused: `grep -rn jsonxx Bedrock apEx` outside `UtilLib/jsonxx*` → no hits),
`Phoenix_Tool/Audio/*` (DirectSound + three closed-source MSVC static libs).

**Unavoidable:** the BaseLib string/IO/logger/timer/container core, `Phoenix` runtime, and
`Phoenix_Tool` data-model files.

---

## 2. `Bedrock/BaseLib/BaseLib.h` — what it really pulls in, and the minimal replacement

### 2.1 What `BaseLib.h` is

`BaseLib.h` is 64 lines: a type header + a platform header + an aggregate include of 40 BaseLib headers
(`BaseLib.h:2..62`). It defines exactly one macro of its own, `NoEmptyFile()` (`:64`).

Order matters, and it is the reason half the tree compiles at all under MSVC:

```
1  #pragma once
2  #include "Types.h"          // TU8..TF64, TBOOL
4  #include "BaseConfig.h"     // ENABLE_TRACKING / ENABLE_STACKTRACKER_CLASS / MEMORY_TRACKING
5  #include "StackTracker.h"
6  #include "Memory.h"         // #define new new(__FILE__,__LINE__)  (only if MEMORY_TRACKING)
8  #define WIN32_LEAN_AND_MEAN
10 #include <WinSock2.h>
11 #include <Windows.h>        // <- supplies DWORD, HANDLE, BOOL, ZeroMemory, max/min, OutputDebugString,
12 #include <comdef.h>         //    DebugBreak, memset/memcpy/strlen/va_list/qsort/abs, _com_error
14 #include <tchar.h>          // <- supplies TCHAR, _T(), _tcs*, _tfopen_s, _vsctprintf, ...
15 #include <float.h>
16 #include <stdio.h>          // <stdio.h>/<stdlib.h>/<math.h> are genuinely included
17 #include <stdlib.h>
18 #include <math.h>
...
59 #include "Thread.h"         // <- #include <Thread>   (case!)
62 #include "CrashTracker.h"
```

Consequences for the port:

* **`<Windows.h>` is load-bearing as a *CRT* header.** `Bedrock/BaseLib/Dictionary.h:56,65,118,136`
  calls `memset` with no `<string.h>`; `Bedrock/BaseLib/String.h:146,149,218,219` declares `va_list`
  parameters with no `<stdarg.h>`; `Bedrock/BaseLib/Archive.h:53` and `Logger.h:52` use `FILE*` with no
  `<stdio.h>`; `Bedrock/BaseLib/Array.h:203` calls `qsort` with no `<stdlib.h>`;
  `apEx/Phoenix/phxMath.h:20` and `apEx/Phoenix/phxarray.h:216` call `qsort`/use `size_t` with no
  `<stdlib.h>`/`<stddef.h>`; `apEx/Phoenix/Mesh.cpp` uses `sin/cos/fabs/max/min` with no `<math.h>`.
  All of these are satisfied today only because `Windows.h` transitively includes those CRT headers.
  `compat/Windows.h` already includes `<cstring> <cstdio> <cstdlib> <cmath> <ctime> <cstdarg> <climits>`
  (lines 15–24) and defines `min`/`max` — **this is the correct mechanism; keep it, and additionally
  force-include the shim (`-include compat/Windows.h`) so no TU can dodge it.**
* **`max`/`min` are macros, not `std::max`.** 53 `max(` call sites + 34 `min(` call sites across the
  four dirs (`Bedrock/BaseLib/Timer.cpp:36`, `apEx/Phoenix/Mesh.cpp:173,174,408,1097,...`). Function-like
  macros (as in `compat/Windows.h`) reproduce Windows semantics bit-for-bit. Do **not** replace them with
  `std::max` — overload resolution differs (`max(0, Speed)` with `Speed` a `TF32`, `max(uint32, int)`).

### 2.2 Typedefs defined by BaseLib

`Bedrock/BaseLib/Types.h:5–15` — the whole type vocabulary of the codebase:

| typedef | definition | portable replacement |
|---|---|---|
| `TU8`/`TU16`/`TU32`/`TU64` | `unsigned char/short/int/__int64` | `uint8_t/uint16_t/uint32_t/uint64_t` |
| `TS8`/`TS16`/`TS32`/`TS64` | `char/short/int/__int64` | `int8_t/int16_t/int32_t/int64_t` |
| `TF32`/`TF64` | `float`/`double` | unchanged |
| `TBOOL` | `unsigned char` | unchanged (note: **not** `BOOL`/`int`) |

`__int64` (`Types.h:8,12`) is an MSVC keyword; clang accepts it as an alias for `long long`, but a
`-std=c++17` build with `-Wpedantic` will warn. Replace with `<cstdint>` types.

`TCHAR`, `LPCTSTR`, `LPTSTR`, `BOOL`, `BYTE`, `WORD`, `DWORD`, `UINT`, `LONG`, `HRESULT`, `WCHAR`,
`FLOAT`, `SIZE_T`, `HANDLE`/`HWND`/`HDC`/`HMODULE`/… all come from `<Windows.h>`/`<tchar.h>`, not from
BaseLib. `_T()` comes from `<tchar.h>`. BaseLib defines **no** exceptions, no RTTI helpers, no
`std::hash` specialisations (verified: `grep -rn 'std::hash\|unordered_' Bedrock apEx` → no hits),
no `<functional>` use outside `Bedrock/BaseLib/String.h:3` (`std::function` for `UTF8CHARCALLBACK`).

### 2.3 MSVC-specific things in `BaseLib.h` and its immediate dependencies

| file:line | thing | clang behaviour | action |
|---|---|---|---|
| `BaseLib.h:8` | `#define WIN32_LEAN_AND_MEAN` | harmless macro | ignore |
| `BaseLib.h:10–12` | winsock/COM includes | unavailable | drop (already patched) |
| `BaseLib/Thread.h:2` | `#include <Thread>` | resolves on case-insensitive APFS with `-Wnonportable-include-path`; **fails on a case-sensitive volume** (verified §A.3) | patch to `<thread>` |
| `BaseLib/Memory.h:16` | `#define new new(__FILE__,__LINE__)` | poisons `<new>`, placement new and any STL header pulled in afterwards | only active under `MEMORY_TRACKING`; build without `_DEBUG` |
| `BaseLib/Memory.cpp:17` | `#pragma warning(disable:4074)` | `-Wunknown-pragmas` warning (verified §A.4) | harmless |
| `BaseLib/Memory.cpp:18` | `#pragma init_seg(compiler)` | `-Wunknown-pragmas` warning, **and semantics lost**: MSVC forced `memTracker` (Memory.cpp:19) to construct first; without it the `operator new` override (`Memory.cpp:125`) can run before `memTracker` exists (static-init-order fiasco) | build without `_DEBUG` |
| `BaseLib/Memory.h:8–11`, `Memory.cpp:125–163` | `__cdecl` on `operator new/delete` overloads | fine once `__cdecl` is `#define`d away | covered by `compat/Windows.h` |
| `BaseLib/Array.h:23,203,327`, `Dictionary.h:111,413` | `__cdecl` in function-pointer typedefs | same | covered |
| `BaseLib/Array.h:203,327` | `_cdecl` (single underscore) | MSVC legacy synonym; clang does **not** know `_cdecl` | `compat/Windows.h` defines `_cdecl` — good. Also `apEx/Phoenix/phxMath.h:20`, `apEx/Phoenix/phxarray.h:216`, `apEx/Phoenix/Mesh.cpp:143`, `apEx/Phoenix/Scene.cpp:455` |
| `BaseLib/Assert.h:10` | `#include <crtdbg.h>` | unavailable | only when `!_DEBUG`+`MEMORY_TRACKING`; drop |
| `BaseLib/Assert.h:14,21` | `_CrtDbgReport(_CRT_ASSERT,...)` | unavailable | same |
| `BaseLib/Assert.h:15,22,39,45` | `DebugBreak()` | Win32 intrinsic | `compat/Windows.h` declares it |
| `BaseLib/StackTracker.h:5` | `enum LOGVERBOSITY;` — forward declaration of an unscoped enum with no fixed underlying type | **hard error**: `ISO C++ forbids forward references to 'enum' types` (verified §A.5) | already patched by `apply_patches.py` (`: int`) |
| `BaseLib/Thread.h:9` | `virtual void Run(void*) = 0;` inheriting bare `CThread` | fine | — |

### 2.4 The minimal portable replacement header

`compat/Windows.h` (~800 lines) already *is* this header, and it is well-targeted. The minimum content
it must supply, derived from the actual symbol census (§A.6), is:

1. **Calling-convention/`declspec` neutralisers:** `__cdecl __stdcall __fastcall __thiscall _cdecl
   CALLBACK WINAPI APIENTRY __declspec(x) __forceinline`.
2. **Fixed-width Win32 scalars, deliberately 32-bit on LP64:** `BOOL BYTE WORD DWORD UINT LONG ULONG
   LONGLONG ULONGLONG CHAR WCHAR TCHAR FLOAT VOID SIZE_T`, plus the `LP*` aliases and opaque `H*`
   handles. **`DWORD`/`LONG`/`ULONG` must stay 32-bit** or every binary structure in
   `Texgen.h`, `Archive.h` and the precalc blob diverges.
   ✅ `HRESULT` — was `typedef long HRESULT;` (64-bit on macOS vs 32-bit on MSVC, so any struct or
   file field holding one changed size). Now `typedef int32_t HRESULT;`.
3. **String/char helpers:** `TCHAR == char`, `_T(x) == x`, `_tcs*` and `_t*` macro families.
   ✅ `_tcsncpy_s` — **was** `#define _tcsncpy_s strncpy` in both `compat/Windows.h` and
   `compat/tchar.h`. `_tcsncpy_s` takes **4** arguments `(dst, dstsize, src, count)`; `strncpy` takes 3.
   Every call site (`Bedrock/BaseLib/String.cpp:73,86`, `Bedrock/UtilLib/XMLNode.cpp:237,281`) passes 4 →
   `error: no matching function for call to 'strncpy'` (verified §A.7). Now fixed to `strncpy_s` in
   `compat/Windows.h`; **the duplicate macro is still defined in both headers** — keep one.
   ✅ `_vsctprintf` (`compat/Windows.h`) and `_tstoi`/`_tstoi64` (`compat/tchar.h`) are now present;
   `String.cpp:701` and `String.cpp:1202` are covered.
   ⚠️ `Bedrock/BaseLib/Random.h:3` does `#define RAND_MAX 0x7fff` over libc++'s `RAND_MAX` — see §4.9;
   this is a **silent behaviour break**, not a compile error.
4. **CRT polyfills** (MSVC secure-CRT): `sprintf_s strcpy_s strcat_s strncpy_s memcpy_s vsprintf_s
   _snprintf_s fopen_s fread_s localtime_s gmtime_s _vsntprintf_s _vsctprintf _tstoi`.
5. **Kernel32 shims**: `CreateFile/ReadFile/WriteFile/CloseHandle/SetFilePointer/GetFileSize/
   FindFirstFile/FindNextFile/FindClose/GetLastError/FormatMessage/GetTickCount/GetTickCount64/
   Sleep/GetCurrentThreadId/OutputDebugString/DebugBreak/IsDebuggerPresent/LoadLibrary/GetProcAddress/
   InterlockedCompareExchange/MultiByteToWideChar/WideCharToMultiByte`.
6. **`min`/`max` function-like macros, `ZeroMemory`, `TRUE`/`FALSE`, `INVALID_HANDLE_VALUE`.**

**Note on `INVALID_HANDLE_VALUE`-style handles:** once `HANDLE` is `void*` and `CreateFile` is a POSIX
wrapper, sentinel comparisons like `CrashTracker.cpp:23`, `FileList.cpp:42`, `StreamReader.cpp:184`
must keep working. `compat`'s `INVALID_HANDLE_VALUE` (`((HANDLE)(intptr_t)-1)`) is correct.

---

## 3. Platform services → backing library

Everything the four directories need, with the recommended implementation.

| service | call sites (evidence) | recommended backing | notes |
|---|---|---|---|
| **Threading (create)** | `apEx/Phoenix_Tool/Audio/phxSongBase.cpp:49` (`CreateThread`), `:26` (`DWORD __stdcall phxSongBase_Thread`). `Bedrock/BaseLib/Thread.h` is a 20-line stub; `JobHandler.cpp` is 3 lines (`#include "BaseLib.h"` only) | **`std::thread`** (or drop — see notes) | There is **no `Thread.cpp` and no `Threading.cpp`** anywhere in the repo (verified: `find . -iname '*thread*'` → only `Thread.h`, plus WaveSabre's DirectSound thread). The only real use is in the DirectSound song loader, which we are removing. The player should use `std::thread` in its own code. |
| **Threading (sync)** | `Bedrock/BaseLib/CriticalSection.cpp:13,33,43,45` — `GetCurrentThreadId`, `InterlockedCompareExchange`, `Sleep(1)`; state at `CriticalSection.h:3–8` | **`std::atomic<int32_t>` + `std::this_thread::yield()`**, or `pthread_mutex` | This is a hand-rolled spinlock (`EnterLightweightCS`). `compat/Windows.h` declares `InterlockedCompareExchange`/`GetCurrentThreadId`/`Sleep`; a `pthread_mutex`-backed `CRITICAL_SECTION` also exists (`compat/Windows.h` (`_RTL_CRITICAL_SECTION`)) but is **not what this code uses**. Implement the three `Interlocked*`/`GetCurrentThreadId` shims in `src/platform/win32_compat.cpp` and leave `CriticalSection.cpp` untouched. Note the algorithm depends on `LONG`/`TS32` being 32-bit. |
| **Atomics** | only `InterlockedCompareExchange` above. No `std::atomic`, no `std::hash`, no lock-free containers anywhere in scope | **none needed** | Do not add a dependency. |
| **Timers** | `timeGetTime`: `BaseLib/Timer.cpp:9,22`, `BaseLib/Profile.cpp:8,14`, `apEx/Phoenix/Project.cpp:173,267`, `apEx/Phoenix/phxEngine.cpp:116` (`fTimerFunction TimerFunction = timeGetTime;`), `Phoenix_Tool/phxResource.cpp:376,383`, `Phoenix_Tool/GUID.cpp:6`. `GetTickCount64`: `BaseLib/Socket.cpp:8,14,26,58,187,219`. `QueryPerformanceCounter` only in a comment (`Timer.cpp:60–76`). `_controlfp_s` in `BaseLib/FloatControl.cpp` | **`std::chrono::steady_clock`** behind a `timeGetTime()` returning `DWORD` (ms since process start/first call). Keep `MMSystem.h` shim. `QueryPerformanceCounter/Frequency` → `mach_absolute_time`/`mach_timebase_info` or `steady_clock` | ⚠️ `timeGetTime` is `DWORD` (32-bit, wraps at ~49.7 days). `CTimer` (`Timer.h:11` `unsigned long Time`) relies on 32-bit wraparound arithmetic (`Timer.cpp:22,30`). If you widen `DWORD` you break it — `DWORD` stays 32-bit. ⚠️ `compat/Windows.h` (`GetTickCount64`) declares `DWORD GetTickCount64(void)` — real Windows returns `ULONGLONG`. `Socket.cpp` stores it in a `TS64` (`Socket.h:6`), so 32-bit truncation silently changes `TimeSinceLastActivity`. Only matters if you keep sockets. |
| **File IO (FILE\*)** | `_tfopen_s`/`fopen_s`: `Archive.cpp:149,221`, `Logger.cpp:69,71,77,79`, `FileList.cpp:14`, `CrashTracker.cpp:61`; `fread_s`: `Archive.cpp:111,153,330,419` | **`fopen`/`fread` polyfills** (already in `compat/Windows.h` (`fopen_s`, `fread_s`)) | `fopen_s(&f, name, mode)` returns 0 on success; the polyfill matches. No change needed to the sources. |
| **File IO (Win32 handles)** | `CreateFile/ReadFile/WriteFile/CloseHandle/SetFilePointer/GetFileSize`: `StreamReader.cpp:183,186,190,204,243,249,257,259,266,271,278,285`; `StreamWriter.cpp:224,230,238,240,241,243,247,260`; `CrashTracker.cpp:22,34,40,132,146,157`; `XMLDocument.cpp:146` | **POSIX `open`/`pread`/`pwrite`/`lseek`/`fstat`/`close`** in `src/platform/win32_compat.cpp` | `CStreamReaderFile`/`CStreamWriterFile` are used by the runtime. Keep `HANDLE` opaque (`void*` holding the fd, or a `struct` wrapper). ⚠️ `SetFilePointer(HANDLE, LONG, LONG*, DWORD)` must support the 64-bit `LARGE_INTEGER` path at `StreamReader.cpp:276–285`. |
| **Directory enumeration** | `FindFirstFile/FindNextFile/FindClose` + `WIN32_FIND_DATA`: `FileList.cpp:33,34,40,61,64,69,70,81,84` | **`<dirent.h>` + `fnmatch`** | Only `CFileList` uses it, and nothing in the runtime does. Drop `FileList.cpp` (the header can stay for source compatibility). If kept: `WIN32_FIND_DATA.dwFileAttributes`/`cFileName` must be populated identically (offsets matter if anything serialises it — nothing does). |
| **Dynamic library loading** | `LoadLibraryA` + `GetProcAddress`: `apEx/Phoenix/phxEngine.cpp:172,173` (loads `d3dcompiler_47.dll` for `D3DCompile`). Also in CoRE2 (`DX11Shader.cpp:84,87,132,135`) | **not needed for the player** — replaced by the SPIR-V pipeline (`work/shader-extract/` already exists). If needed generally: `dlopen`/`dlsym` (`-ldl`, implicit on macOS) | This is the shader compiler loader; in the Vulkan player, shaders are pre-compiled SPIR-V and this whole path disappears. `compat/Windows.h` (`LoadLibraryA`/`GetProcAddress`/`FreeLibrary`) already declares the trio. |
| **String conversion** | `MultiByteToWideChar`: `String.cpp:88,174,184`; `WideCharToMultiByte`: `String.cpp:71,241,251`. `CP_UTF8` at `:71,174,241` | **hand-written UTF-8 ⇄ UTF-16/32 conversion** | ⚠️ **`wchar_t` is 2 bytes on Windows and 4 bytes on macOS.** `CString::WriteAsWideChar` (`String.cpp:80–92`) and `CString::Append(const wchar_t*)` (`String.cpp:234`) use `MultiByteToWideChar`/`WideCharToMultiByte` against `wchar_t*`. If the shim's `wchar_t` is treated as UTF-16, macOS `wchar_t` buffers overflow. **Recommendation: implement the shim in terms of explicit `char16_t`, and treat macOS `wchar_t` (UTF-32) as a third case — or simply make these two functions convert via UTF-32.** These are dead in ANSI mode except for the `wchar_t` overloads, which are only reachable from source that explicitly passes a wide literal. Grep shows no `L"..."` literals in the four dirs. **UNCERTAIN** whether any runtime path reaches them; safest is a correct implementation. |
| **Console / debug output** | `OutputDebugString`: `Memory.cpp:36,39,53,55,59,96`, `StackTracker.cpp:110,114,118`, `Logger.cpp:22,23`(; `fprintf` for the file/stdout sinks at `Logger.cpp:59,201`) | **`fputs` to `stderr`** (already `compat/Windows.h` (`OutputDebugStringA`)) | `LOGGER_BASE_OUTPUT_VERBOSITY`/`LOG_TO_DEBUGOUTPUT` at `Logger.h:8,11`; the player should register `CLoggerOutput_StdOut` or `CLoggerOutput_File` instead of the debug sink. Note `CLoggerOutput_File::OpenLogFile` (`Logger.cpp:65–82`) uses the MSVC `"wt, ccs= UTF-8 "` mode string — **that mode is not accepted by `fopen` on macOS**; the ANSI branch (`:69,71`) is taken in MultiByte mode, so this is dead, but do not enable `UNICODE`. |
| **Memory** | `VirtualAlloc/VirtualFree`: **no uses** in the four dirs (only declared in `compat/Windows.h`). `GlobalAlloc/GlobalLock/GlobalFree`: `ImageDecompressor.cpp:14,19,22,28,32,52,57` (drop the file). `malloc/free`: `Memory.cpp:127,134,144,150,156,162` only. Everything else is `new`/`delete` | **`malloc`/`free`** | `Memory.h`'s `SAFEDELETE`/`SAFEDELETEA`/`SAFEFREE` (`Memory.h:70–72`) are the real allocator API. Do not provide `VirtualAlloc` unless something links against it. |
| **Structured exception handling** | `__try/__except` + `EXCEPTION_EXECUTE_HANDLER` in `CrashTracker.cpp:9,24,114,119,134,166`; `GetExceptionInformation`, `CONTEXT`, `EXCEPTION_POINTERS` | **drop** (`CrashTracker.cpp/.h`) | Replace with a `signal(SIGSEGV)/std::set_terminate` backtrace writer in the player. |
| **Stack walking** | `RtlCaptureStackBackTrace` `StackTracker.cpp:14,16`, `StackWalk` `:47`, `SymInitialize` `:158`, `SymGetLineFromAddr` `:82,108,136`, `#pragma comment(lib,"dbghelp.lib")` `:8` | **drop** — or `backtrace()`/`backtrace_symbols` + `dladdr` | Controlled by `ENABLE_STACKTRACKER_CLASS` (`BaseConfig.h:15`). **One-line patch to `BaseConfig.h` removes the entire DbgHelp dependency.** |
| **Sockets** | `Socket.cpp` (whole file), `SocketSSL.cpp` (whole file) | **drop both** | Nothing in `apEx/Phoenix` or `apEx/Phoenix_Tool` constructs a `CSocket` (verified by grep). Only `UtilLib/SocketSSL` subclasses it. |
| **GDI / OLE** | `ImageDecompressor.cpp:2,14,19,37,45,67–108`; `apEx/Phoenix/Texgen.cpp:187,257–298,382–406`; `apEx/Phoenix/Mesh.cpp:1874–2005` | **replace with portable implementations** — see §9.3 | `ImageDecompressor` is droppable; the `Phoenix` uses are **runtime-critical**. |
| **DirectSound** | `Phoenix_Tool/Audio/phxSound.cpp:4,5,7,17,28–36`; `phxSound.h:3,4` | **CoreAudio (AudioToolbox) or SDL2 audio** — see §9.4 | |

---

## 4. MSVC-isms that clang will reject or warn about

Legend: **[E]** = compile error, **[W]** = warning, **[B]** = compiles but behaviour/semantics differ.

### 4.1 `#pragma comment(lib, ...)` — [W] `-Wunknown-pragmas` (all silently ignored)

`Bedrock/BaseLib/Timer.cpp:3` `winmm.lib` · `Profile.cpp:3` `winmm.lib` · `StackTracker.cpp:8` `dbghelp.lib` ·
`apEx/Phoenix/phxEngine.cpp:9` `d3dx10.lib` · `:10` `d3dx11.lib` · `apEx/Phoenix_Tool/GUID.cpp:4` `winmm.lib` ·
`apEx/Phoenix_Tool/Audio/phxSongMVM.cpp:7,9` (MVX) · `phxSongWaveSabre.cpp:8,9,11,12` (WaveSabre) ·
`phxSongV2M.cpp:7` (libv2). Commented out: `apEx/Phoenix/phxSpline.cpp:8`, `apEx/Phoenix/float16.cpp:55`.
Harmless as warnings, but they are a **complete list of MSVC static/shared libs the build needed** — every
one must be supplied by another route or removed.

### 4.2 Calling conventions and compiler extensions

| file:line | token | clang | action |
|---|---|---|---|
| `Bedrock/BaseLib/Types.h:8,12` | `__int64` | accepted as `long long`, `-Wpedantic` warns | replace with `int64_t` |
| `Bedrock/BaseLib/Memory.h:8–11`, `Memory.cpp:125,132,141,147,153,159` | `__cdecl` | needs `#define __cdecl` | covered by `compat/Windows.h` (`__cdecl`) |
| `Bedrock/BaseLib/Array.h:23,203,327`, `Dictionary.h:111,413` | `__cdecl`, `_cdecl` | same | covered |
| `apEx/Phoenix/phxMath.h:20`, `apEx/Phoenix/phxarray.h:216`, `apEx/Phoenix/Mesh.cpp:143`, `apEx/Phoenix/Scene.cpp:455` | `_cdecl` | clang has no `_cdecl` | covered by `compat/Windows.h` (`_cdecl`) |
| `apEx/Phoenix/RenderLayer.cpp:3` and `RenderLayer.h:83` | `void __fastcall GetRenderTargetResolution(...)` | `#define __fastcall` | covered |
| `apEx/Phoenix/phxEngine.cpp:168` and `phxEngine.h:61` | `void __fastcall InitializePhoenix()` | same | covered |
| `apEx/Phoenix/qsort2.cpp:3,47,165` | `__fastcall` on function pointer type | same | covered |
| `apEx/Phoenix/MVX/mvx.h:58` | `#define MVXCALL __fastcall` | same | covered (but MVX is dropped, §9.4) |
| `apEx/Phoenix/phxEngine.h:55`, `apEx/Phoenix_Tool/apxPhoenix.cpp:5` | `HRESULT(__stdcall d3dcompilefunc)(...)` | `#define __stdcall` | covered |
| `apEx/Phoenix/phxEngine.h:58` | `typedef DWORD(__stdcall *fTimerFunction)()` | same | covered |
| `apEx/Phoenix_Tool/Audio/phxSongBase.cpp:26` | `DWORD __stdcall phxSongBase_Thread(LPVOID)` | same | dropped with DirectSound |
| `apEx/Phoenix_Tool/Audio/phxSongBase.h:12` | `typedef TBOOL(__fastcall *SOUND_LOADCALLBACK)(...)` | same | dropped |
| `apEx/Phoenix_Tool/apxProject.cpp:1305` | `TBOOL __fastcall MvxProgress(...)` | same | dropped |
| `apEx/Phoenix_Tool/Scene_tool.cpp:1052` | `void __fastcall SortRenderLayer(...)` | same | covered |
| `apEx/Phoenix/Arbaro.h:257,312,321,322,324`, `Arbaro.cpp:330,486,677,715,795` | `__forceinline` | `#define __forceinline inline __attribute__((always_inline))` | covered |
| `apEx/Phoenix/Mesh.h:168` | `__forceinline int GetOtherPoly(int)` | same | covered |
| `Bedrock/UtilLib/stb_image.h:369`, `stb_vorbis.c:555`, `miniz.c:964` | `__forceinline` | **guarded by `#ifndef _MSC_VER`** | safe |
| `apEx/Phoenix_Tool/Audio/phxSongMVM.cpp:34` | inline `__asm { ... }` | **MSVC x86 only**; clang rejects `__asm` blocks | dropped with MVX |

### 4.3 `#pragma` directives

| file:line | pragma | clang |
|---|---|---|
| `Bedrock/BaseLib/Memory.cpp:17` | `#pragma warning(disable:4074)` | **[W]** unknown pragma (verified §A.4) |
| `Bedrock/BaseLib/Memory.cpp:18` | `#pragma init_seg(compiler)` | **[W]** + **[B]** static-init ordering lost |
| `Bedrock/UtilLib/stb_image.h:195,196,4542`, `jsonxx.h:69,70,76`, `miniz.c:2795,2796,2838` | `#pragma warning(...)` | guarded by `_MSC_VER` → inert |
| `Bedrock/BaseLib/Archive.h:5,35` | `#pragma pack(push,1)` / `(pop)` | supported silently |
| `apEx/Phoenix/Texgen.h:9,156` | `#pragma pack(push,1)` / `(pop)` | supported silently |
| `apEx/Phoenix/phxMath.h:2,5`, `phxSpline.cpp:9,12`, `phxSpline.h:2,5`, `Mesh.h:5,8`, `Model.h:6`, `compat/Windows.h` | `#pragma push_macro("new")` / `pop_macro` | supported |

**⚠️ Packed-class hazard [B]:** `Bedrock/BaseLib/Archive.h:5–35` wraps `class CArchiveEntry` — a class
with **virtual functions** (`:28,29`) and a **`CString` member** (`:26`) — in `#pragma pack(1)`. On
x86-64 this happens to work; on arm64 a misaligned access inside the packed object can fault or be
emoji-slow under the alignment checker. `ARCHIVEHEADER` (`:7–14`) is the only struct whose *layout on
disk* matters. **Patch: move `class CArchiveEntry` out of the packed region** and keep only
`ARCHIVEHEADER` packed (or apply `__attribute__((packed))` to `ARCHIVEHEADER` alone).
`apEx/Phoenix/Texgen.h` packed structs are all `float`/`unsigned char`/`unsigned short` arrays that map
to GPU buffers; verify against the SPIR-V side but they are alignment-safe as-is.

### 4.4 Secure-CRT / MSVC-only library functions

| file:line | call | action |
|---|---|---|
| `Bedrock/BaseLib/String.cpp:701` | `_vsctprintf(format, argList)` | **missing from `compat/`** → add |
| `Bedrock/BaseLib/String.cpp:705` | `_vsntprintf_s(mText, n+1, n, format, argList)` | present (`compat/Windows.h` (`_vsntprintf_s`)) |
| `Bedrock/BaseLib/String.cpp:73,86` | `_tcsncpy_s(Copy, SizeInChars, String, len)` — **4 args** | ✅ fixed in `compat/` (was `strncpy` → [E], verified §A.7); the macro is still duplicated in `Windows.h` + `tchar.h` |
| `Bedrock/BaseLib/String.cpp:1202` | `_tstoi(str)` | **missing** → add |
| `Bedrock/BaseLib/String.cpp:15,740,909,937,1187,1192,1195,1200` | `_tcslen/_tcsstr/_tcsicmp/_tcscmp/_tcsncmp/_tstof` | covered by the `_tcs*` macros |
| `Bedrock/BaseLib/String.cpp:704` | `ZeroMemory` | covered |
| `Bedrock/BaseLib/CrashTracker.cpp:52,56,61` | `localtime_s`, `sprintf_s`, `fopen_s` | covered (file dropped) |
| `Bedrock/BaseLib/Logger.cpp:69,71,77,79,144` | `fopen_s`, `localtime_s` | covered |
| `Bedrock/BaseLib/FileList.cpp:14` | `fopen_s` | covered |
| `Bedrock/BaseLib/Archive.cpp:111,149,153,221,330,419` | `fread_s`, `_tfopen_s` | covered |
| `Bedrock/UtilLib/XMLNode.cpp:237,281` | `_tcsncpy_s(..., _TRUNCATE)` | replaced with RapidXML; `_TRUNCATE` is MSVC-only |
| `Bedrock/UtilLib/XMLNode.cpp:434,524` | `_stscanf_s`, `_sntprintf_s` | replaced |
| `Bedrock/UtilLib/stb_vorbis.c:5021` | `fopen_s(&f, filename, "rb")` **unguarded** | covered by the `fopen_s` shim (⚠️ the source line itself is also logically inverted — `if (!fopen_s(...)) return NULL;` returns on *success*; `stb_vorbis_open_filename` is broken in this vendored copy). Prefer never calling `stb_vorbis_open_filename` (the code uses `_open_memory`). |
| `Bedrock/UtilLib/stb_image_write.h:159,500` | `fopen_s(&f, filename, "wb")` **unguarded** | covered by the shim; `:500` `stbi_write_png` **is** used (via `ExportPNG`) |
| `Bedrock/UtilLib/miniz.c:2855,3045` | `fopen_s`, `localtime_s` | guarded by `_MSC_VER` → inert |
| `apEx/Phoenix_Tool/apxProject.cpp:1115` | `localtime_s` | covered |
| `Bedrock/UtilLib/stb_vorbis.c:1087,1243` | `__cdecl` **unguarded** | clang accepts `__cdecl` on x86-64 / ignored; **UNCERTAIN on arm64** — add `#define __cdecl` (already done) |

**No hits** for `sscanf_s`, `strcpy_s`, `strcat_s`, `std::string`, `std::wstring`, `std::hash`,
`for each`, `_stricmp`/`stricmp`, `_snprintf`, `swprintf`, `fopen_s` outside the above,
`#pragma warning` outside the above, `__declspec(property)`, visible anonymous structs/unions with
layout significance (only `GUID.cpp:52` and `jsonxx.h:341`, both named members). Verified:
`grep -rnE '\bfor[[:space:]]+each\b'` → only prose in comments; `grep -rnE '\b(_)?stricmp\b'` → no hits.

**Anonymous/unnamed members:** `compat/Windows.h` itself uses an anonymous `union` in `LARGE_INTEGER`
and `SYSTEM_INFO` — clang accepts those (unnamed members are standard).

### 4.5 Case- and path-sensitive `#include`s

* `Bedrock/BaseLib/Thread.h:2` — `#include <Thread>`. On case-insensitive APFS this resolves with
  `-Wnonportable-include-path`; on a case-sensitive volume it is a **hard error**. Verified §A.3.
* `apEx/Phoenix_Tool/Scene_tool.cpp:1116` — `#include "../apEx/apexRoot.h"`; the file on disk is
  `apEx/apEx/apExRoot.h` (`Timeline_tool.cpp:7` and `apxProject.cpp:7` get it right). Same
  case-insensitivity caveat.
* **Backslash include paths — [E] `fatal error: 'gl\gl.h' file not found`** (verified §A.8):
  * `apEx/Phoenix/Scene.cpp:2` — `#include "..\LibCTiny\libcminimal.h"`
  * `apEx/Phoenix/Project.cpp:13` — `#include "..\LibCTiny\libcminimal.h"`
  * `apEx/Phoenix_Tool/Audio/phxSongMVM.h:3`, `phxSongBase.h:5`, `phxSongWaveSabre.h:3`,
    `phxSongOGG.h:3`, `phxSongV2M.h:3` — `#include "..\..\..\Bedrock\BaseLib\BaseLib.h"`
* `apEx/Phoenix/Mesh.cpp:1790,1791` — `#include <gl\gl.h>`, `#include <gl\glu.h>` → **both are [E]**;
  see §9.3.
* Includes that rely on a project-level `-I` rather than the includer's directory:
  `apEx/Phoenix_Tool/Audio/phxSound.cpp:1`, `phxSongBase.cpp:1`, `phxSongMVM.cpp:1`,
  `phxSongOGG.cpp:1`, `phxSongV2M.cpp:1`, `phxSongWaveSabre.cpp:1` all `#include "BasePCH.h"` while the
  file lives at `apEx/Phoenix_Tool/BasePCH.h`. Provide `-I apEx/Phoenix_Tool` or drop the Audio files.
* `apEx/Phoenix_Tool/Audio/phxSongV2M.cpp:5` — `#include "../../libraries/v2/v2mPlayer.cpp"`
  (lowercase `libraries`; the directory is `apEx/Libraries/`). Only survives on a case-insensitive
  volume. Dropped with the audio stack.

### 4.6 Missing includes (MSVC tolerated them via the PCH / `<Windows.h>`)

These are genuine "works only because Windows.h was included first" cases. Do **not** patch each file:
force-include the compat header, which already pulls in the right CRT headers.

| file:line | needs |
|---|---|
| `Bedrock/BaseLib/Dictionary.h:56,65,118,136` | `<string.h>` (`memset`) |
| `Bedrock/BaseLib/String.h:146,149,218,219` | `<stdarg.h>` (`va_list`) |
| `Bedrock/BaseLib/Archive.h:53` | `<stdio.h>` (`FILE`) |
| `Bedrock/BaseLib/Logger.h:52` | `<stdio.h>` (`FILE`) |
| `Bedrock/BaseLib/Array.h:203,327` | `<stdlib.h>` (`qsort`) |
| `Bedrock/BaseLib/Memory.h:8,9` | `<stddef.h>` (`size_t`) |
| `Bedrock/BaseLib/Array.h`, `Dictionary.h` | `<new>` (`new`/`delete` in templates) |
| `apEx/Phoenix/phxMath.h:20`, `apEx/Phoenix/qsort2.cpp:3,4,47,61,165,181` | `<stddef.h>`/`<stdlib.h>` |
| `apEx/Phoenix/phxarray.h:216` | `<stdlib.h>` (`qsort`) |
| `apEx/Phoenix/Mesh.cpp:174,...,2014`, `Scene.cpp:571`, `Arbaro.cpp:631`, `phxSpline.cpp:198` | `<algorithm>`-equivalent `max`/`min` macros, `<math.h>` (`sin/cos/fabs/floor`), `<stdlib.h>` (`qsort`, `abs`) |
| `Bedrock/UtilLib/XMLNode.cpp:71,144,189,258,315,544`, `XMLDocument.cpp:146` | `<string.h>` (`strlen`) |
| `Bedrock/UtilLib/PNGDecompressor.cpp:22,57` | `<string.h>` (`memcpy`) |

### 4.7 Enums, `enum` forward declarations, and other MSVC relaxations

* `Bedrock/BaseLib/StackTracker.h:5` — `enum LOGVERBOSITY;` → **[E]** verified §A.5. Already patched.
* `Bedrock/BaseLib/StreamReader.h:15,34,35,36,37` — `= NULL` used as a **pure-specifier**
  (`virtual TS32 ReadStream(...) = NULL;`). MSVC's `NULL` is `0` so it parses; libc++'s `NULL` is
  `__null` → **`error: initializer on function does not look like a pure-specifier`** (verified §A.9).
  `Bedrock/BaseLib/Socket.h` inherits the problem via `CStreamReader`. **Patch `= NULL` → `= 0` in
  `StreamReader.h`** (5 sites).
* Non-const lvalue reference bound to a temporary — MSVC `permissive` extension, **[E] in clang**:
  * `Bedrock/BaseLib/Matrix.cpp:441,442` and `:468,469` — `CVector3 &X = Up%Z;` (already patched)
  * `Bedrock/BaseLib/Quaternion.cpp` — `CQuaternion &invq1 = q1.Inverted();` (already patched)
  * `apEx/Phoenix/Arbaro.cpp:244` — `D3DXVECTOR3 &vx = transf.apply(...)` where
    `Transformation::apply` returns by value (`Arbaro.h:116`) **and** takes `D3DXVECTOR3&` (so the
    temporary argument is a second error)
  * `apEx/Phoenix/Arbaro.cpp:817` — `Transformation &trf = substemDirection(...).translate(...)` where
    `translate` returns by value (`Arbaro.h:122`)
  * Class of problems: `Transformation::apply(D3DXVECTOR3&)` (`Arbaro.h:116`) and
    `Transformation::translate(D3DXVECTOR3&)` (`Arbaro.h:122`) take non-const references but are called
    with prvalues at `Arbaro.cpp:239,240,244,284,335,342,659,817,831,1095` (see the call-site list in
    §A.10). **Best fix: change both parameters to `const D3DXVECTOR3&`** (2-line patch) rather than
    10 call-site patches.
  * Pattern to keep hunting: `T& x = <expression containing `+ - * % /` or a constructor call>;`
* `enum` as an implicit `int` in switch/bitwise arithmetic — none found.
* `/Zc:forScope`, `/Zc:wchar_t`, `/Zc:__cplusplus`, narrowing-in-braces — no hits.

### 4.8 Two-phase lookup (templates)

* **`Bedrock/BaseLib/Dictionary.h:24,30,97,168`** call `DictionaryHash(KeyType)` unqualified from inside
  the `CDictionary`/`KDPair` templates, but the declarations that make it work for non-class keys live at
  the **bottom of the same header** — `Dictionary.h:830` (`const TS32&`) and `:831` (`const void*`) — and
  `Dictionary.cpp:3,8` only define them. For `KeyType == CString` this survives by ADL
  (`String.h:217` declares it in the global namespace, which is CString's associated namespace). For
  `void*` and `TS32` there is no associated namespace → **`error: call to function 'DictionaryHash' that
  is neither visible in the template definition nor found by argument-dependent lookup`** (verified §A.11).
  Actual instantiations that hit it: `CDictionary<void*, CAllocationInfo>` at `Memory.cpp:29,62` and
  `Memory.h:42` (only under `MEMORY_TRACKING`, i.e. `_DEBUG`). `CDictionary<CphxGUID, ...>`
  (`Material_Tool.h`, 11 sites) and `DictionaryHash(CphxGUID)` (`Material_Tool.h:40`) are fine by ADL.
  **Fix (one patch): move the two declarations from `Dictionary.h:830–831` to the top of the file.**
* `Bedrock/BaseLib/Dictionary.h` `CDictionaryEnumerable` referring to the base's nested `KDPair`
  unqualified — already patched by `apply_patches.py` (`typedef typename ... KDPair KDPair;`).
* `Bedrock/BaseLib/Array.h:9–120`, `Array.h:327+` (`CArrayThreadSafe`) — the template uses only its own
  members; the `ThreadSafe` wrapper *forwards* to a member named `Array` (not a dependent base), so no
  `this->` needed. **No further two-phase issues found in the four dirs.**
* `Bedrock/BaseLib/Dictionary.h` and `Array.h` do not include `<cstring>`/`<new>`; that is the §4.6 issue,
  not a lookup issue.
* `CDictionary::KDPair::~KDPair` (`Dictionary.h:32–36`) `delete`s `Next` recursively — a latent stack
  overflow on long chains, unrelated to the port but worth noting since we are in this file.

### 4.9 Silent behaviour differences (no diagnostic at all)

These are the most dangerous items in the report because they will not show up as build failures.

| # | issue | file:line | effect |
|---|---|---|---|
| **B1** | `#define RAND_MAX 0x7fff` overriding libc++'s `0x7fffffff` | `Bedrock/BaseLib/Random.h:3` | MSVC `rand()` returns `0..32767`; macOS `rand()` returns `0..2147483647`. Runtime code computes `rand() / (float)RAND_MAX` in ~20 places: `apEx/Phoenix/Scene.cpp:401,405,408,436,440,441,443,451`, `apEx/Phoenix/Mesh.cpp:1517,1534,1583,1617,1645,1649,1656`, `apEx/Phoenix/phxSpline.cpp:197`. With `RAND_MAX` forced to `0x7fff` but libc's `rand()` returning 31-bit values, every scatter/particle/waveform-noise value is up to **65 538×** out of range instead of `0..1`. **This changes procedural geometry, particle placement and noise output — the demo would not look the same.** Fix: implement `rand()`/`srand()` with MSVC's LCG (`Random.cpp` should not delegate to libc), or better, use `CRandom` (`Random.h:5`) everywhere and give it a fixed 15-bit generator. Then `RAND_MAX` must be **`#undef`ed before redefining** to avoid the macro-redefinition warning. |
| **B2** | `wchar_t` is 4 bytes on macOS, 2 on Windows | `String.cpp:80–92,234–265` | see §3 "String conversion". |
| **B3** | `long` is 8 bytes on macOS (LP64), 4 on Windows (LLP64) | `Bedrock/BaseLib/Timer.h:11,20,23`, `Timer.cpp:44,54` (`unsigned long Time`, `GetTime()`, `SkipTime`); `Bedrock/BaseLib/Socket.cpp:151,208`; `Bedrock/UtilLib/stb_image.h:4517` (`long pos = ftell(f)`) | `CTimer`'s `Time` silently widens. Probably benign, but it also changes `CString::operator+(long)` overload resolution (macOS `long` and `int` are distinct widths; `CString + <int64>` can now be ambiguous or pick the wrong overload — `String.h:81,85,89,93`). Audit `CString` arithmetic at the call sites the compiler flags. |
| **B4** | `#pragma init_seg(compiler)` ignored | `Memory.cpp:18` | static-init-order fiasco for `memTracker` if `MEMORY_TRACKING` is on. Stay in release. |
| **B5** | `#pragma pack(1)` on a polymorphic class | `Archive.h:5–35` | arm64 misalignment. See §4.3. |
| **B6** | `D3DXFLOAT16` is a `WORD`-sized storage type used 108× | `apEx/ThirdParty/D3D/Include/D3DX10math.h:88–96` | the portable shim **must** keep `D3DXFLOAT16` exactly 2 bytes and `D3DXVECTOR3` exactly 12 bytes, or every serialized `.apx`/precalc field moves. |
| **B7** | `char` signedness | throughout | macOS `char` is signed like MSVC x86, so this is the *safe* direction. Just don't add `-funsigned-char`. |

### 4.10 `override` / `virtual` audit — clean

`grep -rn '\boverride\b'` in the four dirs finds `Bedrock/BaseLib/Socket.h:39`
(`CString ReadLine() override` — matches `CStreamReader::ReadLine` at `StreamReader.h:31`),
`apEx/Phoenix_Tool/Scene_tool.h:251,317` (`MarkSpecialRequired()` — matches the
`virtual void MarkSpecialRequired() {}` at `phxResource.h:123`), and
`apEx/Phoenix_Tool/Audio/phxSongWaveSabre.h:14` (`GenerateHeaderContents()` — matches
`phxSongBase.h:33`). **All correct.** No virtual-with-default-argument or covariant-return problems found.

### 4.11 `class`/`struct` forward-declaration mismatches — none

A scripted pass over all four directories matched every `class X;`/`struct X;` forward declaration
against every `class X :`/`struct X {` definition (including the next-line-brace form) and found
**zero kind mismatches**. The `class CphxObjectClip;` (`Model.h:12`) vs any `struct CphxObjectClip`
case does not exist. Item closed.

---

## 5. `Bedrock/UtilLib` inventory

18 source files / 21 660 lines, plus 72 files of vendored OpenSSL headers and two MSVC `.lib` binaries.

Compiled by `Bedrock/UtilLib/UtilLib.vcxproj`: `PNGDecompressor.cpp`, `SocketSSL.cpp`,
`XMLDocument.cpp`, `XMLNode.cpp`, `jsonxx.cpp`, `miniz.c`, `stb_vorbis.c` (+ the `.h` entries, which
MSVC lists but does not compile). **`RapidXML/` is not compiled and nothing includes it** (verified:
`grep -rn rapidxml Bedrock apEx` excluding `UtilLib/RapidXML` → no hits).

| file | lines | purpose | player needs? | Win32 / non-portable | verdict |
|---|---|---|---|---|---|
| `RapidXML/rapidxml.hpp` | 2 596 | header-only XML DOM/parser, **v1.13** | **Yes** | none — portable, no `_MSC_VER` branches of consequence | **use as the XML backend** |
| `RapidXML/rapidxml_iterators.hpp` | — | iterators | optional | portable | use |
| `RapidXML/rapidxml_print.hpp` | — | serialiser (needed: the tool writes `.apx`) | **Yes** (export path) | portable | use |
| `RapidXML/rapidxml_utils.hpp` | — | `file<>` convenience loader | optional | portable | use |
| `XMLNode.h` / `XMLNode.cpp` | 528+ / ~600 | COM DOM wrapper (`CXMLNode`) | **Yes, API only** | `XMLNode.h:5` `#include "msxml3.tlh"`; `:9` `using namespace MSXML2`; `MSXML2::IXMLDOMNode*`; `SysAllocString`/`SysFreeString` (18), `VARIANT`/`BSTR`, `_bstr_t`, `_com_error`, `_tcsncpy_s(..., _TRUNCATE)` at `:237,281`, `WriteFile` at `:146` of the companion | **replace with a RapidXML-backed implementation of the same class** |
| `XMLDocument.h` / `XMLDocument.cpp` | ~30 / ~180 | COM document (`CXMLDocument`) | **Yes, API only** | `XMLDocument.h:7` `MSXML2::IXMLDOMDocument2*`; `XMLDocument.cpp:5,17` `CoInitializeEx`/`CoUninitialize`; `:27` `VARIANT_BOOL`; `:38` `FAILED`; `:44` `long nLine`; `:146` `WriteFile`; `HRESULT`-as-`long` exception (`:67,76,99`) | same |
| `msxml3.tlh` | 3 037 | MSVC `#import "msxml3.dll"` output, checked in because the `#import` is commented out at `XMLNode.h:4` | **No** | 100 % Windows COM declarations | **delete from the build** |
| `PNGDecompressor.h` / `.cpp` | 6 / ~90 | `DecompressPNG`, `ExportPNG/Tga/Bmp`, `ARGBtoABGR`, `ClearZeroAlpha` over stb_image | **Yes** | portable; needs `<string.h>` for the `memcpy` at `:22,57` | **keep** |
| `stb_image.h` | 4 655 | **v1.41** JPEG/PNG reader | **Yes** (via PNGDecompressor) | `_MSC_VER`-guarded (`:193,362,373,389,395,1384,4541`); `__declspec(align(16))` at `:1384` is inside `#ifdef STBI_SIMD` | keep; **v1.41 is from 2011 — consider upgrading to a current stb_image** (bug + security fixes) |
| `stb_image_write.h` | 520 | **v0.94** PNG/BMP/TGA writer | **Yes** (`ExportPNG`) | ⚠️ **unguarded `fopen_s` at `:159` and `:500`** (needs the shim, but the shim covers it). Old version; `stbi_write_png` at `:500` is the live path | keep; **upgrade strongly advisable** (v0.94 predates most fixes) |
| `stb_vorbis.c` | 5 449 | **v1.01** Ogg Vorbis decoder | **Yes for OGG audio** (`Phoenix_Tool/Audio/phxSongOGG.cpp:6` `#include "../../../Bedrock/UtilLib/stb_vorbis.c"`, used at `:21,23,28,37`) | `<malloc.h>` at `:547` is correctly excluded for `__APPLE__`; `alloca` at `:860` works on clang; `__cdecl` at `:1087,1243` unguarded (minor); `fopen_s` at `:5021` unguarded (shim covers; the function is also logically broken) | keep; upstream v1.01 is from 2013 — upgrade if OGG stays |
| `miniz.c` | 4 916 | **v1.15** deflate/inflate + ZIP read/write | **Yes for the 64k/CNSPack path** (`apEx/apEx/CNSPack.cpp`, `apEx/MinimalPlayer/MinimalPlayer.cpp`) | all MSVC paths `_MSC_VER`-guarded (`:963,2794,2851,3042`,…); `MZ_FORCEINLINE` has a `__GNUC__` branch at `:965` | keep (also duplicated at `apEx/MinimalPlayer/miniz.c`, 225 896 bytes — do not compile both) |
| `jsonxx.h` / `jsonxx.cpp` | 528 / ~900 | JSON reader/writer | **No** — verified unused anywhere outside `UtilLib/jsonxx*` | `_MSC_VER`-guarded pragmas; portable otherwise | **drop** |
| `SocketSSL.h` / `.cpp` | 18 / ~65 | TLS socket over BaseLib `CSocket` | **No** | `SocketSSL.cpp:2,3` include vendored OpenSSL 0.9.8 headers; `:4,5` `#pragma comment(lib,"…/libeay32.lib"/"…/ssleay32.lib")`; uses `SSLv3_method()` — **removed from modern OpenSSL**; `SSL_library_init`/`SSL_load_error_strings` (deprecated in 1.1.0) | **drop** |
| `ExternalLibraries/openssl/` | 72 files | OpenSSL 0.9.8-era headers + `libeay32.lib`, `ssleay32.lib` (MSVC binaries) | **No** | `openssl/rand.h:67` `#include <windows.h>`; `applink.c:29` `#include <io.h>` | **drop**; if TLS is ever needed, link a system OpenSSL/LibreSSL |

**Player-relevant summary:** from UtilLib the player needs `PNGDecompressor` (+ stb_image,
stb_image_write), `stb_vorbis.c` if OGG audio survives, `miniz.c` if the compressed-blob path survives,
and **RapidXML as the replacement XML backend**. Everything else is droppable.

---

## 6. `CString` / `CArray` / `CDictionary` / `CList` / `CRingBuffer`

### 6.1 `CList` does not exist

Verified: `grep -rn 'class CList\|CList<' Bedrock apEx` → **no hits**. BaseLib's container set is
`CArray` (`Array.h`), `CArrayThreadSafe` (`Array.h:327+`), `CDictionary`/`CDictionaryEnumerable`
(`Dictionary.h`), `CRingBuffer` (`RingBuffer.h`), plus `phxArray` in the runtime
(`apEx/Phoenix/phxarray.h`) and `CStringArray`/`CStringArrayMarkers` (`String.h:205,210`).
**Treat "CList" in the task brief as not present** rather than as a porting item.

### 6.2 `CString` (`String.h` / `String.cpp`, 219 + 1 338 lines)

* **Representation:** `TCHAR *String` (`String.h:20`) + cached `TU32 LengthCached` (`:21`),
  optional `TU32 Hash`/`LowercaseHash` (`:190,191`, guarded by `HASHED_STRINGS` which `BaseConfig.h:38`
  defines on). Heap-allocated with `new TCHAR[]`/`delete[]`; **no SSO**. `virtual ~CString()`
  (`String.h:29`) makes every `CString` 8–16 bytes larger than it needs to be but that mirrors the
  original ABI.
* **Dual `char*`/`wchar_t*` API:** both are first-class (`String.h:33,37,56,59,73,106,124,128`).
  In ANSI mode `TS8 == TCHAR == char`, so `CString(const TS8*)` **is** `CString(const char*)`;
  the `wchar_t*` overloads become the only wide entry point and are the only consumers of
  `MultiByteToWideChar`/`WideCharToMultiByte` (`String.cpp:71,88,174,184,241,251`).
* **Win32 dependencies inside `CString`:** `_tcslen` (`:15`), `_tcsncpy_s` (`:73,86`),
  `MultiByteToWideChar` (`:88,174,184`), `WideCharToMultiByte` (`:71,241,251`), `_tcscmp`
  (`:378,385,402,428,443,458,1187`), `_tcsncat`-family via `memcpy` (fine), `_tcsstr` (`:740`),
  `_tcsicmp` (`:909`), `_tcsncmp` (`:1192`), `_tstof` (`:1195`), `_tstoi` (`:1202`),
  `_vsctprintf` (`:701`), `_vsntprintf_s` (`:705`), `ZeroMemory` (`:704`), `_T()` (`:11,12,928,1314,1316`),
  `vsscanf`/`vswscanf` (`:1170`-ish), `CP_UTF8` (`:71,174,241`), `wcslen` (`:270`).
  **No GDI, no COM, no sockets, no threading.** So `CString` is portable once the CRT/`_tcs*` shims exist.
* Perf note: `CString::operator+` allocates a fresh buffer per concatenation (`String.cpp:106–134`);
  `String.cpp:36,55` and `Memory.cpp:36–59` build log/leak strings that way. Not a porting problem.
* `EstimateStringFormatLength` is declared (`String.h:218,219`) — verify it is defined (UNCERTAIN; not
  found in `String.cpp` by the greps performed). If it is only declared, it is harmless unless called.

### 6.3 `CArray` (`Array.h`, 501 lines) — portable

* Template; members `TS32 Capacity/ItemCount`, `ItemType *Array` (`Array.h:13–15`).
* Includes only `CriticalSection.h` and `Assert.h` (`Array.h:2,3`). The only non-standard bits are
  `__cdecl` in the sort-callback typedefs (`:23`, `:327`) and `_cdecl` in the `qsort` cast (`:203`),
  all neutralised by `compat/Windows.h`.
* Growth policy `EXPANSIONRATIO 0.2f` (`:7`) → `Expand((TS32)(Capacity*0.2f + 1))` (`:73,88`) — the
  `+1` avoids the zero-growth trap. Fine.
* `CArrayThreadSafe` (`:327+`) embeds a `LIGHTWEIGHT_CRITICALSECTION` (`:329`) and takes the spinlock in
  **every** accessor including `operator[]` (`:410`) — returns a reference *after* releasing the lock, so
  it is not actually safe. Not a porting blocker but a real bug; consider `std::mutex` + documented
  semantics or banning the class in new code.
* **Win32 dependencies: none** beyond `__cdecl`/`_cdecl` and (transitively) `<string.h>`/`<new>`.

### 6.4 `CDictionary` (`Dictionary.h`, 831 lines) — portable, with one lookup patch

* Chained hash table; `KDPair{ Key, Hash, Data, Next }` (`Dictionary.h:11–38`), `TableSize`/`ItemCount`/
  `KDPair** HashTable` (`:43–45`), `_dummy` for out-of-range `operator[]` (`:47`).
  Load factor 0.7 / collapse 0.2 (`:5,6`).
* Hashes via the free function `DictionaryHash` overloads: `const TS32&` → identity (`Dictionary.cpp:3`),
  `const void*` → djb2 over the pointer bytes (`Dictionary.cpp:8`), `const CString&` →
  `String.cpp:630`, `CphxGUID` → `Material_Tool.h:40`/`Material_Tool.cpp:1020`.
* **Win32 dependencies: none** other than `__cdecl` (`:111,413`) and the missing `<string.h>` for
  `memset` (`:56,65,118,136`).
* **Two-phase-lookup defect:** see §4.8 — patch `Dictionary.h` to declare `DictionaryHash(const TS32&)`
  and `DictionaryHash(const void*)` *before* the template. (`apply_patches.py` does not yet do this;
  it only fixes the `KDPair` name.)
* Real bug worth flagging: `KDPair::~KDPair` recursively `delete Next`, and `Delete()` (`:163+`) unlinks
  correctly but `ResizeTable` can call `Insert` on nodes whose `Next` is stale — the file's own comment
  at `Dictionary.h:187–192` admits a past crash from exactly this. Treat as "works, don't touch".

### 6.5 `CRingBuffer` (`RingBuffer.h`) — portable

Template, `ItemType *Array`, `TU32 Size` default 500 (`RingBuffer.h:11`); uses `SAFEDELETEA`
(`:20`) from `Memory.h`. No Win32.

---

## 7. Recommended layering

The project already implements the right idea. This section states it precisely, reconciles it with what
exists, and lists exactly what to shim vs patch.

### 7.1 Layer diagram

```
  Layer 0  third_party/apEx/**                 vendored MSVC sources (patched only by tools/apply_patches.py)
  Layer 1  compat/                             header shadows, FIRST on the include path (-Icompat -include compat/Windows.h)
             Windows.h  tchar.h  MMSystem.h  winsock2.h  comdef.h  olectl.h  dbghelp.h
             intrin.h  psapi.h  rpc.h  objbase.h
           aim: make every `#include <Windows.h>`/`<TCHAR.H>` in the vendored tree resolve to a
           portable header.  NO vendored file is edited for this.
  Layer 2  src/platform/win32_compat.cpp        the non-inline Win32 implementations (POSIX/mach/pthread)
             CreateFile/ReadFile/... , timeGetTime, GetTickCount, OutputDebugString, Interlocked*,
             MultiByteToWideChar, FormatMessage, _controlfp_s
  Layer 3  src/compat/rapidxml_xml.{h,cpp}      drop-in replacement of Bedrock/UtilLib's CXMLDocument/CXMLNode
             (same API, RapidXML inside) -- the ONE structural replacement
  Layer 4  src/platform/{fs,clock,audio,image,text}.{h,cpp}   player-side portable services
  Layer 5  src/vk/**                            Vulkan backend replacing the D3D11 calls inside apEx/Phoenix
  Layer 6  src/app/**                           the player: window (SDL3/GLFW), swapchain swap, main loop
```

Design rules that make this work (all already honoured by `compat/Windows.h`):

* `-I<compat>` **before** the SDK, and `-include compat/Windows.h` so no TU can dodge the CRT headers
  that `Windows.h` used to supply (§2.1, §4.6).
* `-DNDEBUG` and **never** `_DEBUG`: this single flag disables `MEMORY_TRACKING` (`BaseConfig.h:20`),
  which disables the `#define new new(__FILE__,__LINE__)` (`Memory.h:16`), the `CMemTracker` global and
  its static-init-order problem (`Memory.cpp:18,19`), and the *use* of `CStackTracker`'s DbgHelp path
  (`Memory.cpp:98,100`). It does not disable `ENABLE_STACKTRACKER_CLASS` (`BaseConfig.h:15`) — for that,
  one tiny patch to `BaseConfig.h` (§7.3) removes the whole DbgHelp/StackWalk dependency.
* `-D__APPLE__` is implicit; several vendored third-party files key off it (`stb_vorbis.c:546`).
* `-Wno-unknown-pragmas -Wno-nonportable-include-path` are acceptable, but prefer to patch the two
  real case/backslash includes (§4.5) rather than mute them.

### 7.2 Shim (do **not** edit the vendored source)

| what | where | covers |
|---|---|---|
| All Win32 types, handles, macros, constants | `compat/Windows.h` (exists) | 56 files |
| `TCHAR`/`_T`/`_tcs*`/`_t*` families | `compat/tchar.h` + `Windows.h` (exists) | 174 `TCHAR`, 1 205 `_T()`, 72 `_T()` in BaseLib |
| `timeGetTime`/`timeGetDevCaps`/`timeBeginPeriod`/`timeEndPeriod` | `compat/MMSystem.h` (exists) + `src/platform` | 10 call sites |
| `CreateFile/ReadFile/WriteFile/CloseHandle/SetFilePointer/GetFileSize/_tfopen_s/fread_s` | `compat/Windows.h` decls + `src/platform/win32_compat.cpp` | StreamReader/StreamWriter/Archive/Logger |
| `InterlockedCompareExchange/GetCurrentThreadId/Sleep` | `compat/Windows.h` decls + `src/platform` | `CriticalSection.cpp` only |
| `OutputDebugString`/`DebugBreak`/`IsDebuggerPresent` | `compat/Windows.h` decls + `src/platform` | Memory.cpp, Logger.cpp, Assert.h |
| `MultiByteToWideChar`/`WideCharToMultiByte` | `compat/Windows.h` decls + `src/platform` (UTF-8 ⇄ UTF-16/32) | `String.cpp` |
| `min`/`max`/`ZeroMemory`/`TRUE`/`FALSE`/`INVALID_HANDLE_VALUE` | `compat/Windows.h` (exists) | everywhere |
| All `sprintf_s`-family + `_vsctprintf` + `_tstoi` + 4-arg `_tcsncpy_s` | `compat/Windows.h` (**gaps to fix**) | String.cpp, Archive.cpp, Logger.cpp, stb_* |
| `#include <gl\gl.h>`, `<gl\glu.h>` | ✅ handled by an `#ifdef __APPLE__` branch in the vendored `Mesh.cpp:1789` → `<OpenGL/gl.h>`/`<OpenGL/glu.h>`; needs `-framework OpenGL -DGL_SILENCE_DEPRECATION`. A `compat/` shadow cannot work here because backslashes are a hard error before any include path is consulted. | `Mesh.cpp:1790,1791` |
| `"…/LibCTiny/libcminimal.h"` | ✅ `third_party/apEx/LibCTiny/libcminimal.h` now exists (20 lines, P8b). A `compat/` shadow could not work here because the include is resolved relative to the *including file* (`third_party/apEx/Phoenix/../LibCTiny/…`) | `Mesh.cpp:5`, `Texgen.cpp:3`, `Project.cpp:13`, `Scene.cpp:2` |
| `enum LOGVERBOSITY` forward-declaration | *patched* (see §7.3) | StackTracker.h |

### 7.3 Patch (edit the vendored source, via `tools/apply_patches.py`)

Everything below is small, mechanical, and idempotent. `apply_patches.py` already contains 9 of them;
the rest are the additions this analysis found. **Several were applied to the live tree while this
document was being written — §12.3 carries the re-verified status of each, so start there.**

**Already in `apply_patches.py`:** `StackTracker.h` enum, `Logger.h` enum, `Dictionary.h` `KDPair`,
`BaseLib.h` winsock/`Socket.h`/`CrashTracker.h` removal, `Matrix.cpp` ×2 ref-to-temporary,
`Quaternion.cpp` ref-to-temporary.

**New patches needed (verified errors, in priority order).** ✅ = already fixed in the live tree;
the table is kept complete because a re-vendor resets these.

| # | file | change | why |
|---|---|---|---|
| P1 | ✅ `compat/Windows.h` | was `#define _tcsncpy_s strncpy` → now `strncpy_s`. ⚠️ **still define it in exactly one place** — the macro currently lives in *both* `compat/Windows.h` and `compat/tchar.h` | was a **4-arg vs 3-arg [E]** at `String.cpp:73,86`, `XMLNode.cpp:237,281` |
| P2 | ✅ `compat/Windows.h`, `compat/tchar.h` | `_vsctprintf`, `_tstoi`/`_tstoi64` | `String.cpp:701`, `String.cpp:1202` |
| P3 | ✅ `compat/Windows.h` | was `typedef long HRESULT` → now `int32_t` | LP64 layout divergence (§4.9-B3 family) |
| P4 | `Bedrock/BaseLib/StreamReader.h:7,29,30,33,34` | `= NULL` → `= 0` | **pure-specifier [E]**, verified §A.9 |
| P5 | `Bedrock/BaseLib/Dictionary.h` | move the `DictionaryHash(const TS32&)` / `(const void*)` declarations from `:832,833` to above the template | **two-phase lookup [E]**, verified §A.11 |
| P6 | `Bedrock/BaseLib/BaseConfig.h:15` | drop `#define ENABLE_STACKTRACKER_CLASS` | removes DbgHelp/`RtlCaptureStackBackTrace`/`StackWalk`/`SymInitialize` entirely |
| P7 | `Bedrock/BaseLib/Thread.h:2` | `<Thread>` → `<thread>` (or delete the include) | **`[E]` on a case-sensitive volume**, verified §A.3 |
| P8 | ✅ `apEx/Phoenix/Scene.cpp:2`, `apEx/Phoenix/Project.cpp:13` | backslashes → `../LibCTiny/libcminimal.h` | was an **`[E]` fatal error**, verified §A.8 |
| P8b | ✅ `third_party/apEx/LibCTiny/libcminimal.h` | created (was missing). `apEx/LibCTiny/libcminimal.h` is 20 lines: `void WinMainCRTStartup();` + no-op `OPENDEBUGFILE()`/`DEBUGLOG()` (all `DEBUGINTOFILE` code is `#ifdef`-off). Needed by `Phoenix/Mesh.cpp:5`, `Phoenix/Texgen.cpp:3`, `Phoenix/Project.cpp:13`, `Phoenix/Scene.cpp:2` | 4 runtime files would fail to compile without it |
| P9 | `apEx/Phoenix_Tool/Scene_tool.cpp:1116` | `apexRoot.h` → `apExRoot.h` | **`[E]` on a case-sensitive volume** |
| P10 | `apEx/Phoenix/Arbaro.h:116,122` | `apply(D3DXVECTOR3&)` / `translate(D3DXVECTOR3&)` → `const D3DXVECTOR3&` | fixes ~10 **ref-to-temporary `[E]`s** at `Arbaro.cpp:239,240,244,284,335,342,659,817,831,1095` |
| P11 | ✅ `apEx/Phoenix/Mesh.cpp:1790,1791` | backslash GL includes replaced by an `#ifdef __APPLE__` branch including `<OpenGL/gl.h>`/`<OpenGL/glu.h>` | was an **`[E]` fatal error**, verified §A.8. Requires `-framework OpenGL -DGL_SILENCE_DEPRECATION`; GLU is deprecated since 10.9 (verified §A.19) |
| P12 | `Bedrock/BaseLib/Random.h:3` + `Random.cpp` | `#undef RAND_MAX` then redefine, **and** implement a 15-bit MSVC-compatible `rand()` | **silent demo-altering bug B1**, §4.9 |
| P13 | `Bedrock/BaseLib/Archive.h:5–35` | move `class CArchiveEntry` (`:16`) out of `#pragma pack(1)` | arm64 misalignment, §4.3 |
| P14 | `apEx/Phoenix/Project.h:9,104` | replace `#include "../MinimalPlayer/SetupDialog.h"` + `SETUPCFG&` with a player-owned struct | drags `<windows.h>`+`DEVMODE`+resource.h into the runtime (§9.5) |
| P15 | `Bedrock/BaseLib/Logger.cpp:65–82` | only if `UNICODE` is ever enabled: the `"wt, ccs= UTF-8 "` mode strings are not valid `fopen` modes on macOS | dead in ANSI mode; the `#error` in `compat/tchar.h` already guards it |
| P16 | **open** — `apEx/Phoenix/phxEngine.h:11` | remove `#include "../MVX/mvx.h"` | the MVX header now resolves, but MVX itself is a closed-source MSVC `.lib` requiring `<dsound.h>` and must be replaced — see §12.1 D1, §9.4 |
| P17 | **open** — `apEx/Phoenix_Tool/{Material_Tool,apxProject,Timeline_tool}.cpp:1` + `phxSplineExt.h:2` | strip the leading `#define _WINSOCKAPI_` | exists only to reorder `<winsock2.h>` before `<windows.h>`; dead weight once sockets are gone |

**Structural replacement (counts as a shim, not a patch):**

| # | what | why |
|---|---|---|
| S1 | New `src/compat/rapidxml_xml.cpp` implementing `CXMLDocument`/`CXMLNode` (`XMLDocument.h`, `XMLNode.h`) on RapidXML, with `-I` ordering preferring it over `Bedrock/UtilLib/XMLDocument.h`, or by patching the two include lines in `phxResource.h:3` and `phxSplineExt.h:11` | removes MSXML/COM from ~20 Phoenix_Tool files (§0-3) |
| S2 | ~~`compat/gl/gl.h` + `compat/gl/glu.h`~~ **not needed** — `<OpenGL/glu.h>` exists in the macOS SDK; the vendored `Mesh.cpp:1789` already has the `#ifdef __APPLE__` branch (verified §A.19) | GLU is deprecated, not absent; see §9.3 |

### 7.4 Files that must be **excluded from the build** (no patching at all)

`Bedrock/BaseLib/Socket.cpp` · `Socket.h` · `CrashTracker.cpp` · `CrashTracker.h` ·
`StackTracker.cpp` · `ImageDecompressor.cpp` · `ImageDecompressor.h` · `FileList.cpp` ·
`Bedrock/UtilLib/SocketSSL.cpp` · `SocketSSL.h` · `XMLDocument.cpp` (superseded by S1) ·
`XMLNode.cpp` (superseded by S1) · `jsonxx.cpp` · `msxml3.tlh` (delete from the tree and from
`UtilLib.vcxproj`'s `ClCompile` list) · `ExternalLibraries/openssl/**` ·
`apEx/Phoenix_Tool/Audio/**` · `apEx/Phoenix/float16.cpp` + `apxFloat16.cpp` (both are 100 % commented
out — zero-line TUs) · `apEx/Phoenix/qsort2.cpp` (only if you use `std::sort`; note `phxMath.h:20`
declares a `qsort` override that shadows libc's, so check which one the call sites get).

Note: `Bedrock/UtilLib/UtilLib.vcxproj` lists *headers* in its `ClCompile` group
(`PNGDecompressor.h`, `stb_image.h`, …) — MSVC ignores them, but a naive CMake/meson glob translation
will try to compile them. Exclude `.h` from the source glob.

### 7.5 Exact patch-vs-shim answer for the top offenders

| file | approach |
|---|---|
| `BaseLib/BaseLib.h` | **patch** (drop winsock/comdef/`Socket.h`) + **shim** (`Windows.h`, `tchar.h`) |
| `BaseLib/Types.h` | **patch** `__int64` → `int64_t` |
| `BaseLib/String.cpp/.h` | **shim only** (`_tcs*`, `_vsctprintf`, `_tstoi`, MB↔WC) — no source edits |
| `BaseLib/Array.h`, `Dictionary.h`, `RingBuffer.h`, `Vector/Matrix/Quaternion/…` | **shim only** (`__cdecl`, `<string.h>`, `<new>` force-include) + **patch** `Dictionary.h` lookup order |
| `BaseLib/CriticalSection.cpp/.h` | **shim only** (`Interlocked*`, `GetCurrentThreadId`, `Sleep`) |
| `BaseLib/Memory.cpp/.h` | **shim/none** — build with `NDEBUG` |
| `BaseLib/Timer.cpp`, `Profile.cpp` | **shim only** (`MMSystem.h`, `timeGetTime`) |
| `BaseLib/StreamReader.cpp/.h`, `StreamWriter.cpp/.h`, `Archive.cpp/.h` | **shim** (file IO) + **patch** `StreamReader.h` `= NULL` and `Archive.h` packing |
| `BaseLib/Logger.cpp/.h`, `Assert.h`, `EnumHelpers.*`, `FileList.*` | **shim only** (or exclude `FileList`) |
| `BaseLib/Thread.h`, `JobHandler.*`, `StackTracker.*` | **patch** (case) / **patch** (`BaseConfig.h`) / **exclude** |
| `UtilLib/XMLNode.*`, `XMLDocument.*` | **replace** (S1) |
| `UtilLib/PNGDecompressor.*`, `stb_*`, `miniz.c` | **shim only** (`fopen_s` already covered) |
| `UtilLib/SocketSSL.*`, `jsonxx.*`, `openssl/**` | **exclude** |
| `Phoenix/**` | **shim** Win32/GDI (`hdc`, `HFONT`, `GetGlyphOutline`, `TextOut`, `GetDIBits`) + **patch** P8, P10, P11, P14 + **replace** the D3D11 calls (§9.1) |
| `Phoenix_Tool/**` | **replace** XML (S1) + **exclude** `Audio/**` + **patch** P9 + **shim** `TCHAR`/`_T()`/`HWND` (which for `_T()` needs no patch at all: `_T(x) == x`) |

---

## 8. What "run on macOS" additionally requires (beyond compiling)

Compiling is roughly a third of the work. The four directories assume Windows services *at runtime*:

1. **A window/message loop.** `apEx/Phoenix/Project.cpp:189–190` pumps `PeekMessage`/`DispatchMessage`
   inside `DoPrecalc`. `Project.cpp` also uses `IDXGISwapChain::Present` (`:184`). The player owns the
   window (SDL3/GLFW) and the loop.
2. **A swapchain and present path.** `Project.cpp:184`, `phxEngine.h:34` (`phxBackBufferView`).
3. **Text/vector rasterisation** (§9.3) — `Mesh.cpp` (text meshes) and `Texgen.cpp` (text + metafile
   texture filters) both call GDI.
4. **Audio** (§9.4).
5. **A `SETUPCFG`** `Project.h:6` / `Project.cpp:262` — currently the MinimalPlayer's Windows dialog
   struct.

---

## 9. Blocking subsystems (beyond Win32) — sized honestly

### 9.1 D3D11 → Vulkan inside `apEx/Phoenix`

`ID3D11*`/`IDXGI*`/`D3D11_*`/`D3DX*` occurrence counts per file:

| file | count | file | count |
|---|---|---|---|
| `apEx/Phoenix/Mesh.cpp` | 243 | `apEx/Phoenix/Mesh.h` | 35 |
| `apEx/Phoenix/Scene.cpp` | 135 | `apEx/Phoenix_Tool/phxSplineExt.cpp` | 32 |
| `apEx/Phoenix_Tool/Model_Tool.cpp` | 83 | `apEx/Phoenix/phxEngine.h` | 25 |
| `apEx/Phoenix_Tool/Scene_tool.cpp` | 75 | `apEx/Phoenix/Arbaro.h` | 25 |
| `apEx/Phoenix/phxEngine.cpp` | 73 | `apEx/Phoenix/TreeGen.h` | 24 |
| `apEx/Phoenix/Arbaro.cpp` | 52 | `apEx/Phoenix/Material.cpp` | 22 |
| `apEx/Phoenix/Project.cpp` | 51 | `apEx/Phoenix/Timeline.cpp` | 20 |
| `apEx/Phoenix/Scene.h` | 49 | `apEx/Phoenix_Tool/Material_Tool.cpp` | 19 |
| `apEx/Phoenix/TreeGen.cpp` | 41 | … | |

Representative direct calls: `ID3D11Buffer*` parameters in the public API
(`Mesh.h:243` `BuildMesh(ID3D11Buffer*&, ID3D11Buffer*&, ID3D11Buffer*&, int&, int&, int&)`),
`D3D11_BUFFER_DESC`/`D3D11_USAGE_DEFAULT`/`D3D11_BIND_VERTEX_BUFFER` at `Mesh.cpp:464,485`,
`ID3D11Texture2D*` at `Mesh.h:288` / `Mesh.cpp:1121`, structured-buffer SRVs at
`Mesh.cpp:1735–1755`, and the whole global device state in `phxEngine.h:16–44`
(`phxDev`, `phxContext`, six `ID3D11InputLayout*`/`ID3D11SamplerState*`/`ID3D11Buffer*` globals,
`phxDepthBufferView`, `phxBackBufferView`, …).

**Shader path (good news):** shaders are HLSL strings carried in the project
(`apEx/Phoenix_Tool/apxProject.cpp:56ff` builds `pass->ShaderCode = _T(...)`) and compiled at runtime by
`D3DCompile` loaded from `d3dcompiler_47.dll` (`phxEngine.cpp:168–173`). This project has **already**
extracted them and compiled them to SPIR-V (`work/shader-extract/*.hlsl`, `work/shader-extract/spv/*.spv`,
`work/vktest/*.spv`) — so the Vulkan path is: load the pre-built SPIR-V, **do not** implement an HLSL
front-end. That removes `phxEngine.cpp:168–173` and both `LoadLibraryA` uses in Phoenix.

**D3DX10Math is a hard dependency, not a nuisance.** The free functions
`D3DXVec3Normalize` (42 uses), `D3DXMatrixMultiply` (34), `D3DXMatrixIdentity` (23),
`D3DXVec3Transform` (21), `D3DXMatrixInverse` (14), `D3DXVec3Cross` (13),
`D3DXVec3Transform3` (12), `D3DXVec3Length` (12), `D3DXMatrixTransformation` (12),
`D3DXVec3Dot` (9), `D3DXMatrixTranspose` (7), `D3DXVec4Transform` (6),
`D3DXQuaternionMultiply` (6), `D3DXQuaternionIdentity` (6), `D3DXMatrixRotationAxis` (5),
`D3DXVec3Unproject` (4), `D3DXQuaternionRotationAxis` (4), `D3DXMatrixRotationX` (4),
`D3DXVec3TransformNormal` (3), `D3DXMatrixRotationZ` (3), `D3DXMatrixRotationY` (2),
`D3DXIntersectTri` (2), and 1 each of `D3DXVec4Lerp`, `D3DXVec3LengthSq`,
`D3DXQuaternionToAxisAngle`, `D3DXQuaternionSquadSetup`, `D3DXQuaternionSquad`,
`D3DXQuaternionSlerp`, `D3DXQuaternionRotationMatrix`, `D3DXQuaternionInverse`,
`D3DXMatrixTranslation`, `D3DXMatrixScaling`, `D3DXMatrixRotationQuaternion`,
`D3DXMatrixPerspectiveOffCenterRH`, `D3DXMatrixPerspectiveFovRH`, `D3DXMatrixLookAtRH`,
`D3DXMatrixIsIdentity`, `D3DXMatrixDecompose`, `D3DXFloat16To32Array` — plus three
`D3DX11*` texture helpers (`D3DX11SaveTextureToMemory`, `D3DX11SaveTextureToFile`,
`D3DX11CreateTextureFromMemory`) — are **declared `WINAPI` in the bundled header**
(`apEx/ThirdParty/D3D/Include/D3DX10math.h:610–970`) and implemented in `d3dx10.dll`.
Only the class methods are inline (`D3DX10math.inl` uses `D3DX10INLINE`, defined at `D3DX10.h:29–37`,
and `D3DXVec3Normalize`/`D3DXMatrixMultiply` are *not* among the 217 `D3DX10INLINE` definitions —
`D3DXMatrixMultiply` appears in the `.inl` only as a call from an inline method at `:830,898`).
**So a portable `d3dx10math.h` shim must implement ~40 free functions** over the project's own
`CVector3`/`CMatrix4x4`/`CQuaternion` (`Bedrock/BaseLib/Vector.h`, `Matrix.h`, `Quaternion.h`) or over a
fresh header, **keeping `D3DXFLOAT16` at 2 bytes, `D3DXVECTOR2/3/4` at 8/12/16 bytes, `D3DXMATRIX` at
64 bytes** (serialized blobs, §4.9-B6). Note `apEx/Phoenix/phxMath.h:4` is the only place that includes
`<D3DX10Math.h>`; `Mesh.h:7` uses `#include <D3DX10Math.h>` too — both resolve to the shim.
Sites needing reimplementation, not typedef-ing: `D3DXVec3Unproject` (4),
`D3DXIntersectTri` (2), the quaternion Squad machinery (3), `D3DXMatrixDecompose` (1),
`D3DXFloat16To32Array` (1).

### 9.2 `D3D11_*` enum leakage into the project format

`apEx/Phoenix_Tool/apxProject.cpp:56` writes `p->Parameter.Value.CullMode = D3D11_CULL_NONE;`, and
`Project.cpp:161–165` uses `DXGI_FORMAT_R16G16B16A16_FLOAT`/`DXGI_FORMAT_R32_FLOAT` as project pixel
formats. `RenderTarget.h` exposes
`CphxRenderTarget::PixelFormat::RGBA16Float`. **The`.apx` file itself only stores integers**
(verified: an `.apx` is plain XML — `<apExProject build="0296_11786r"><AspectX>64</AspectX>…`), so the
*file* is portable; only the in-memory C++ enums are D3D11 names. Introduce a neutral
`PHXFORMAT`/`PHXCULL` enum and map at the Vulkan boundary — or keep the `D3D11_*` names as
player-local constants. Do **not** try to keep `<d3d11.h>`.

### 9.3 GDI text/image rasterisation — runtime-critical

* `apEx/Phoenix/Mesh.cpp:1874` `HFONT hFont = CreateFontA(32,...,EngineFontList[Font])`, `:1877`
  `SelectObject`, `:1897,1900` `GetGlyphOutline(hdc, *Text, GGO_NATIVE, &gm, ...)`, `:1911,1926,1987`
  walk `TTPOLYGONHEADER`/`TTPOLYCURVE`/`POINTFX`, `:2004` `DeleteObject`. The glyph outlines are then
  **tessellated with legacy GLU**: `Mesh.cpp:1884` `gluNewTess()`, `:1886–1890`
  `gluTessCallback`, `:1903–1918` `gluTessBeginPolygon`/`gluTessVertex`/`gluTessEndPolygon`. Headers:
  `Mesh.cpp:1790,1791` `#include <gl\gl.h>`, `<gl\glu.h>` (**hard error**).
  Also `Mesh.cpp:1874` `extern HDC hdc;` — the GDI device context is created in `Texgen.cpp:187`
  (`HDC hdc = GetDC(NULL);`) using **Windows font family names** (`Arial`, `Wingdings`, `Consolas` —
  `apEx/Phoenix_Tool/apxPhoenix.cpp:20–42`).
  **The tessellator is not a problem** (verified §A.19): modern macOS SDKs still ship
  `<OpenGL/glu.h>` and `gluNewTess` compiles, links (`-framework OpenGL`) and runs — it is marked
  deprecated since 10.9, so add `-DGL_SILENCE_DEPRECATION`. The vendored `Mesh.cpp:1789` already has an
  `#ifdef __APPLE__` branch including `<OpenGL/gl.h>`/`<OpenGL/glu.h>`. **GLU is a stopgap, not a
  destination** (Apple's OpenGL is deprecated and will eventually go), but it removes one work item.
  **Remaining port work:** FreeType (or CoreText) for glyph outlines to replace `CreateFontA`/
  `GetGlyphOutline`/`TTPOLYGONHEADER` walking, and replace `EngineFontList` with bundled font files.
* `apEx/Phoenix/Texgen.cpp:187` `GetDC(NULL)`; `:257–298` `CreateCompatibleDC`/`CreateCompatibleBitmap`/
  `SelectObject`/`TextOut`/`GetDIBits`/`DeleteDC`/`DeleteObject` for the **text texture filter**;
  `:352–406` `PHXTEXTUREFILTER::LoadMetafile` uses `SHCreateMemStream` + `OleLoadPictureEx` +
  `IPicture::Render` + double-buffered `GetDIBits` for anti-aliased metafile/vector images
  (`#include <olectl.h>`, `<Shlwapi.h>` at `:350,351`).
  **Port:** text via FreeType, raster images via the already-vendored stb_image, and for the vector
  metafile path either drop the filter or plug in a small SVG/EMF rasteriser.
  This is `PHX_TEXGEN_IMAGE` (`PhoenixConfig.h:53`), i.e. **the runtime texgen path, not a tool feature.**

### 9.4 Audio — replace entirely

`apEx/Phoenix_Tool/Audio/` is a DirectSound front-end over **four mutually exclusive closed-source
MSVC static libraries**:

* `phxSound.h:3,4` `<mmsystem.h>`, `<dsound.h>`; `phxSound.cpp:4,5` `LPDIRECTSOUND`,
  `LPDIRECTSOUNDBUFFER`; `:7` `WAVEFORMATEX`; `:17` `DSBUFFERDESC`; `:28–36`
  `DirectSoundCreate`/`SetCooperativeLevel`/`CreateSoundBuffer`/`SetFormat`.
* `phxSongMVM.cpp:3` `#include "../../MVX/mvx.h"`, `:7,9` `#pragma comment(lib,"…mvxplayerlib_…lib")`,
  `:34` inline `__asm`. `MVX/mvx.h` needs `<dsound.h>` and `HWND`, and its `mvxSystem_Init(HWND, ...)`
  is `__fastcall`. Binaries: `apEx/Libraries/MVX/mvxPlayerLib_{vs2010,vs2015,vs2019}_*.lib`
  (and `apEx/MinimalPlayer/mvxplayerlib_minimal*.lib`). A pre-existing spec already covers this:
  `docs/spec/music-mvx.md`. ⚠️ `apEx/Phoenix/phxEngine.h:11` also includes `"../MVX/mvx.h"` from a
  *different* source directory (`apEx/Phoenix/MVX/`), which is why the vendoring is currently broken —
  see §12.1 D1.
* `phxSongV2M.cpp:5` `#include "../../libraries/v2/v2mPlayer.cpp"` + `:7` `libv2.lib`
  (`apEx/Libraries/V2/` — **source is present**, `v2mplayer.cpp`, so this one is portable-modulo-Win32).
* `phxSongWaveSabre.cpp:4` `WaveSabrePlayerLib.h` + `:8–12` four `.lib`s. Sources **are** present
  under `apEx/Libraries/WaveSabre/{WaveSabreCore,WaveSabrePlayerLib}/`, but the player lib contains
  `DirectSoundRenderThread.cpp`.
* `phxSongOGG.cpp:6` `#include "../../../Bedrock/UtilLib/stb_vorbis.c"` — **portable**, and it is the
  only one of the four with no Windows dependency.

Plus `phxSongBase.cpp:49` `CreateThread`, `:26` `DWORD __stdcall` thread proc, and
`apxProject.h:21` `HWND AppHandle` / `:103,108,109` `Import(..., HWND hwnd)` — i.e. the *project model*
carries an `HWND`.

**Recommendation:** decode `.mvm` (Buzz machine) with a portable player or pre-render, decode OGG with
the already-vendored `stb_vorbis.c`, and output through CoreAudio (AudioToolbox `AudioQueue`/
`AudioUnit`) or SDL2 audio. Split `CphxSongBase` from `CphxSound`: the `OfflineRenderSong` API
(`phxSongBase.h`) is what the tool uses to *bake* audio into the project and has no Windows dependency
except the thread/callback typedefs.

### 9.5 `SETUPCFG` / `SetupDialog`

`apEx/Phoenix/Project.h:6` `#include "../MinimalPlayer/SetupDialog.h"`; that header (in
`apEx/MinimalPlayer`, **outside** the four directories) does `#include <windows.h>`, `#include
"resource.h"` and declares `typedef struct { char *ReleaseName; char *GroupName; … int fullscreen;
DEVMODE mode; int vsync; float HardwareAspectRatio; } SETUPCFG;` plus `OpenSetupDialog(char*[7])`.
`CphxProject::LoadProject(unsigned char *Data, CphxProject *Precalc, SETUPCFG &setup)`
(`Project.h:104`, `Project.cpp:262`) is on the runtime path (called from `MinimalPlayer.cpp:264,266`).
**Patch P14:** give the player its own `SETUPCFG` (drop `DEVMODE`) and point the include at it.

### 9.6 What the runtime actually loads — precalc, not `.apx`

Important architectural fact for the player's design: `CphxProject::LoadProject` takes a **binary
`unsigned char*` precalc blob** (`Project.h:104`, `Project.cpp:262`), not XML. The `.apx` XML is the
*tool's* project format, read by `CapexProject::ImportMain` (`apxProject.cpp:420,425`) via
`CXMLDocument`, and baked into the blob by `apEx/apEx/MinimalExport.cpp` (which is in the excluded tool
UI, and itself includes `../Phoenix_Tool/apxProject.h` at `:3`). The `Stream_*` enum at `Project.h:5–28`
is the blob's section table. `MinimalPlayer.cpp:264,266` loads a `precalc` blob and then a `demo` blob.

So the player has a **fork in the road**:
* **(a) Blob path** — require a pre-baked precalc blob. Smallest runtime (no XML, no tool data model);
  but you cannot open an `.apx` directly, so the player needs an offline/exporter step.
  `docs/spec/_scratch/parse_apx.py` suggests the parent has already gone this way.
* **(b) `.apx` path** — port `CapexProject` + `MinimalExport` so the player can consume an `.apx`
  directly. This requires S1 (RapidXML) **and** untangling `apxProject.cpp` from `apExRoot.h` /
  `TimelinePreview.h` / `Config.h` (`:7,9,12`) and `Scene_tool.cpp` from `apexRoot.h` / `ModelView.h` /
  `SceneView.h` / `WorkBench.h` (`:1116–1118,2539`). **Only 3 `.cpp` files and 1 header carry the UI
  coupling** (verified by counting `apEx/`-includes and UI identifiers per file): `apxProject.cpp`
  (3 UI includes, 8 UI symbols), `Scene_tool.cpp` (4, 9), `Timeline_tool.cpp` (1, 1), `apxProject.h`
  (2). Everything else in `Phoenix_Tool` is UI-free.

---

## 10. Suggested work packages

| WP | scope | exit criterion |
|---|---|---|
| **WP0** | Fix the compat gaps **P1–P3** and force-include the shim | `String.cpp`, `Archive.cpp`, `Logger.cpp` compile standalone |
| **WP1** | BaseLib math/containers/color/math + `CString` compile clean | `libbase_math.a` builds with `-Wall -Werror=return-type` |
| **WP2** | BaseLib IO/log/timer: shim file IO + `timeGetTime`; patches **P4, P6, P12, P13** | `StreamReader/StreamWriter/Archive/Logger/Timer` link and pass a round-trip test |
| **WP3** | `src/compat/rapidxml_xml.cpp` (**S1**): reimplement `CXMLDocument`/`CXMLNode` on RapidXML | round-trip an existing `.apx` byte-compatibly enough for `ImportMain` |
| **WP4** | D3DX10Math shim (~40 free functions) with byte-exact layouts | `Mesh.cpp`, `Scene.cpp`, `Arbaro.cpp`, `Timeline.cpp` compile |
| **WP5** | Vulkan backend replacing `ID3D11*` in `Phoenix/{phxEngine,RenderTarget,RenderLayer,Material,Mesh,Scene,Texgen,Timeline,Model}` + load the SPIR-V from `work/shader-extract/spv/` | one frame of a project renders |
| **WP6** | Text/image rasterisation (**§9.3**): FreeType + tessellator; patches **P11** | a text mesh and a text texture filter produce the same output as the tool screenshots |
| **WP7** | Audio (**§9.4**): CoreAudio/SDL2 + stb_vorbis; drop `Phoenix_Tool/Audio` | music plays |
| **WP8** | Player shell: window, main loop replacing `Project.cpp:184–190`, `SETUPCFG` (**P14**) | end-to-end playback |
| **WP9** | Patches **P7–P10** (case/backslash/ref-to-temporary) | build works on a case-sensitive APFS volume / CI |

Ordering note: **WP4 must precede WP5** (the runtime will not compile without D3DX types, and the
types' sizes are a serialization contract). **WP3 is independent** and unblocks `Phoenix_Tool`
compilation in parallel.

---

## 11. Risk register / uncertain items

| id | item | confidence |
|---|---|---|
| U1 | Whether any live path reaches `CString`'s `wchar_t` overloads (`String.cpp:234–265`) or `WriteAsWideChar` (`:80–92`). `grep -rn 'L"'` finds no wide literals in the four dirs, so probably dead — but implement `MultiByteToWideChar`/`WideCharToMultiByte` correctly for `wchar_t` = UTF-32 rather than assuming UTF-16. | **UNCERTAIN** |
| U2 | ~~`EstimateStringFormatLength` declared but not defined~~ **RESOLVED**: declared at `String.h:218,219`, defined nowhere, **and has no callers** (`grep -rn 'EstimateStringFormatLength'` outside `String.h` → no hits). Harmless dead declaration. | resolved |
| U3 | `__cdecl` at `stb_vorbis.c:1087,1243` is unguarded. clang ignores/accepts it on x86-64; behaviour on **arm64** is unverified here. `#define __cdecl` covers it either way. | **UNCERTAIN (arm64)** |
| U4 | arm64 in general: `#pragma pack(1)` on the polymorphic `CArchiveEntry` (`Archive.h:5–35`) and the x86-oriented `__fastcall`/`__asm` remnants. `apply_patches.py` and this spec assume the primary target is **x86-64** (macOS/Clang, as stated); if arm64 is also a target, add **P13** and re-audit `Arbaro.cpp`/`qsort2.cpp` for pointer-truncating casts (`Bedrock/BaseLib/StackTracker.cpp:82,108,136` `(DWORD)Stack[x]` is the only one found, and that file is dropped). | **UNCERTAIN** |
| U5 | `D3DXVec3Unproject`, `D3DXIntersectTri`, `D3DXQuaternionSquad*`, `D3DXMatrixDecompose`, `D3DXFloat16To32Array` have no obvious portable equivalent in `Bedrock/BaseLib`; they need real implementations, not typedefs. Effort not sized. | **UNCERTAIN** |
| U6 | Whether `apEx/Phoenix/Texgen.cpp`'s `OleLoadPictureEx` metafile path (`:352–406`) is exercised by the shipped demos. If not, WP6 shrinks a lot. | **UNCERTAIN** |
| U7 | `BaseLib` has *both* `Unicode` and `MultiByte` MSVC configurations; the sources contain `#ifdef UNICODE` branches (`String.cpp:70,85,173,183,240,250`; `Logger.cpp:60–79`). This port assumes `MultiByte` **everywhere**. A build-time guard already exists (`compat/tchar.h` `#ifdef UNICODE / #error`). | high confidence |
| U8 | ~~`apply_patches.py` `Dictionary.h` anchor may not match~~ **RESOLVED**: the anchor (`KEYSORTCALLBACK`/`VALUESORTCALLBACK` block, `Dictionary.h:571,572`) matches the source exactly, and the vendored copy already contains the inserted `typedef typename … KDPair KDPair;` (its `SortByKey` is at `:649` vs `:647` in the source), so the patch has been applied successfully. | resolved |
| U9 | Whether the shipped demos can be driven from a precalc blob alone (§9.6 path (a)) without re-running `MinimalExport`. `docs/spec/_scratch/parse_apx.py` suggests that is the chosen route; unverified here. | **UNCERTAIN** |

---

## 12. Delta against the live `compat/` + `third_party/` trees

Verified by reading the live files. This is the "what to do next" list, stripped of everything already
done. If the tree is re-vendored, re-run this check.

### 12.1 Blocking defects found in the current vendoring (fix first)

| # | defect | evidence | fix |
|---|---|---|---|
| **D1** | ~~`third_party/apEx/MVX/` is missing~~ **FIXED during this analysis** — `third_party/apEx/MVX/mvx.h` now exists, so `Phoenix/phxEngine.h:11` `#include "../MVX/mvx.h"` resolves. (The source tree has two MVX dirs — `apEx/MVX/{mvx.h,MVXLibrary.lib}` and `apEx/Phoenix/MVX/mvx.h`; `phxEngine.h:11` needs the former at `third_party/apEx/MVX/`, while `Phoenix_Tool/Audio/phxSongMVM.cpp:3` `"../../MVX/mvx.h"` needs the latter at `third_party/apEx/apEx/MVX/`.) | `ls third_party/apEx/MVX` → `mvx.h` | none. MVX itself is still a closed-source MSVC `.lib` needing `<dsound.h>` and should be removed (**P16**, §9.4). |
| **D2** | ~~`LibCTiny` is not vendored~~ **FIXED during this analysis** — `third_party/apEx/LibCTiny/libcminimal.h` now exists and the two backslash includes (`Project.cpp:13`, `Scene.cpp:2`) were rewritten to `"../LibCTiny/libcminimal.h"`. | `ls third_party/apEx/LibCTiny` → `libcminimal.h`; all four include sites now use forward slashes | none |
| **D3** | ~~`compat/Windows.h` `_tcsncpy_s` was `strncpy`~~ **FIXED during this analysis** — both headers now say `strncpy_s`. Residual nit: the macro is defined in **both** `compat/Windows.h` and `compat/tchar.h` (a redefinition warning waiting to happen; keep one). | re-read `compat/Windows.h` / `compat/tchar.h` | delete one of the two `#define _tcsncpy_s strncpy_s` |
| **D4** | `HRESULT` is `long` (64-bit here). | `compat/Windows.h` `typedef long HRESULT;` | apply **P3** |
| **D5** | `Bedrock/UtilLib/XMLNode.cpp` / `XMLDocument.cpp` and `msxml3.tlh` are still in the vendored tree and will be compiled unless the build glob excludes them. | `utilLib` `ClCompile` list includes both | either exclude them from the build (`S1` provides the replacement) or delete `msxml3.tlh` and the two TUs from the build file list (see §7.4) |

### 12.2 Confirmed already handled by `apply_patches.py` (do not redo)

`StackTracker.h` enum fwd-decl; `Logger.h` matching enum definition; `Dictionary.h` `KDPair` typename;
`BaseLib.h` winsock/`comdef`/`Socket.h`/`CrashTracker.h` removal; `Matrix.cpp` ×2 and `Quaternion.cpp`
non-const-ref-to-temporary.

### 12.3 Patch status (re-grep-verified against the live vendored tree)

| patch | grep evidence in `third_party/apEx` (re-verified after the fixes above) |
|---|---|
| **P1** ✅ | `compat/Windows.h` and `compat/tchar.h` both now `#define _tcsncpy_s strncpy_s` — **residual nit: the macro is defined twice** |
| **P3** ✅ | `compat/Windows.h` now `typedef int32_t HRESULT;` |
| **P4** ⛔ open | `Bedrock/BaseLib/StreamReader.h` still has **5** `= NULL` pure-specifiers |
| **P5** ⛔ open | `Bedrock/BaseLib/Dictionary.h:832,833` still hold the `DictionaryHash` declarations, after the template |
| **P6** ⛔ open | `Bedrock/BaseLib/BaseConfig.h:15` still `#define ENABLE_STACKTRACKER_CLASS` |
| **P7** ⛔ open | `Bedrock/BaseLib/Thread.h:2` still `#include <Thread>` |
| **P8 / P8b** ✅ | `third_party/apEx/LibCTiny/libcminimal.h` exists; `Project.cpp:13` and `Scene.cpp:2` now use forward slashes |
| **P9** ⛔ open | `Phoenix_Tool/Scene_tool.cpp:1116` still `#include "../apEx/apexRoot.h"` (should be `apExRoot.h`) |
| **P10** ⛔ open | `Phoenix/Arbaro.h:116,122` still take non-const `D3DXVECTOR3&` |
| **P11** ✅ | `Phoenix/Mesh.cpp:1790` now has the `#ifdef __APPLE__` → `<OpenGL/gl.h>`/`<OpenGL/glu.h>` branch; GLU calls at `:1884–1918` intact (14 refs) |
| **P12** ⛔ open | `Bedrock/BaseLib/Random.h:3` still `#define RAND_MAX 0x7fff` — **highest-priority silent bug in the port** |
| **P13** ⛔ open | `Bedrock/BaseLib/Archive.h:16` `class CArchiveEntry` still inside the `:5–35` pack region |
| **P14** ⛔ open | `Phoenix/Project.h:9` still includes `../MinimalPlayer/SetupDialog.h`; `:104` still takes `SETUPCFG&` |
| **P16** ⛔ open | `Phoenix/phxEngine.h:11` still `#include "../MVX/mvx.h"` (the header now resolves, but MVX must still go) |
| **P17** ⛔ open | `Phoenix_Tool/{Material_Tool,apxProject,Timeline_tool}.cpp:1` and `phxSplineExt.h:2` still `#define _WINSOCKAPI_` |

`Phoenix/` files modified versus the source at the time of writing: `Mesh.cpp`, `Project.cpp`,
`Scene.cpp` (the three fixes above). No other Phoenix file has been touched.

### 12.4 Not yet done at all

* **S1** — the RapidXML-backed `CXMLDocument`/`CXMLNode` replacement. This is the largest single
  unblocker: it is what lets ~20 `Phoenix_Tool` files compile.
* **WP4** — the ~40-function `D3DX10Math` portable shim (§9.1). Nothing in the tree provides it yet;
  `apEx/Phoenix/phxMath.h:4` and `apEx/Phoenix/Mesh.h:7` both `#include <D3DX10Math.h>`.
* **WP6** — a portable text/image rasteriser (§9.3). `Phoenix/Texgen.cpp` still contains 6 GDI sites
  (`GetDIBits`/`CreateCompatibleDC`/`OleLoadPicture`), and `Phoenix/Mesh.cpp:1874–2005` still requires a
  GDI `HDC` created at `Texgen.cpp:187`.
* **WP8** — the `Project.cpp:184–190` present/message-pump replacement.

---

## Appendix A — empirical verification log

Each item below was tested with a standalone `clang++ -std=c++17` snippet on this machine
(Apple clang, macOS, case-insensitive APFS), not by compiling apEx.

| # | test | result |
|---|---|---|
| A.1 | filesystem case sensitivity | `diskutil info /` → `File System Personality: APFS`; `#include <Thread>` in a scratch file resolved **with** `warning: non-portable path to file '<thread>'; specified path differs in case from file name on disk [-Wnonportable-include-path]`. ⇒ a case mismatch is a warning here and an **error** on a case-sensitive volume or with `-Werror`. |
| A.2 | `#include <gl\gl.h>` | `fatal error: 'gl\gl.h' file not found` — backslash includes are hard errors regardless of filesystem. |
| A.3 | `#include <Thread>` | see A.1 (same test). |
| A.4 | `#pragma warning(disable:4074)` / `#pragma init_seg(compiler)` / `#pragma comment(lib,"winmm.lib")` | three × `warning: unknown pragma ignored [-Wunknown-pragmas]`, exit 0. |
| A.5 | `enum E;` | `error: ISO C++ forbids forward references to 'enum' types`. |
| A.6 | symbol census | scripted extraction of 190 Win32 identifiers over the four dirs; top: `NULL` 804, `TCHAR` 174, `FALSE` 51, `TRUE` 40, `DWORD` 20, `HRESULT` 16, `HWND` 14, `CP_UTF8` 12, `SOCKET_ERROR` 10, `timeGetTime` 10, `HANDLE` 9, `MultiByteToWideChar` 9, `CreateFile` 8. Full table in §1.2. |
| A.7 | `#define _tcsncpy_s strncpy` then `_tcsncpy_s(a,8,"x",1)` | `error: no matching function for call to 'strncpy'`. This reproduced the `_tcsncpy_s`/`strncpy` mismatch that `compat/Windows.h` had; **fixed in the tree during this analysis** — it now defines `_tcsncpy_s strncpy_s` (see §12.1 D3). |
| A.8 | backslash-include grep | 7 sites (listed in §4.5). |
| A.9 | `class A { virtual void f() = NULL; };` with libc++ | `error: initializer on function does not look like a pure-specifier`. |
| A.10 | `Arbaro` call sites | `Transformation::apply(D3DXVECTOR3&)` (`Arbaro.h:116`, returns by value) called with prvalues at `Arbaro.cpp:239,240,244,284`; `Transformation::translate(D3DXVECTOR3&)` (`Arbaro.h:122`, returns by value) called with prvalues at `Arbaro.cpp:335,342,421,659,817,831,1095`; plus two ref-to-prvalue *bindings* at `:244` and `:817`. |
| A.11 | two-phase lookup, `DictionaryHash` declared after the template | `error: call to function 'DictionaryHash' that is neither visible in the template definition nor found by argument-dependent lookup`, with the note `'DictionaryHash' should be declared prior to the call site`. Reproduces `Dictionary.h:24,30,97,168` + `Dictionary.h:830,831`. |
| A.12 | `<malloc.h>` on macOS | `fatal error: 'malloc.h' file not found` — but `stb_vorbis.c:546–548` correctly guards it with `!(defined(__APPLE__) \|\| …)`. |
| A.13 | `alloca(16)` after only `<stdlib.h>` | compiles — clang provides `alloca` as a builtin, so `stb_vorbis.c:860` is fine. |
| A.14 | third-party `_MSC_VER` audit | `stb_image.h` (`:193,362,373,389,395,1384,4541`), `stb_vorbis.c:553`, `miniz.c` (`:963,2794,2851,3042,…`), `jsonxx.h` (`:30,36,68,75`) all guard their MSVC paths. **Unguarded exceptions:** `stb_vorbis.c:1087,1243` (`__cdecl`), `stb_vorbis.c:5021` (`fopen_s`), `stb_image_write.h:159,500` (`fopen_s`). |
| A.15 | vendored-layout resolution check | `ls third_party/apEx/MVX` → *No such file or directory*; `ls third_party/apEx/apEx/MVX` → `MVXLibrary.lib mvx.h`. Confirms **D1**. |
| A.16 | `LibCTiny` presence | `find . -name 'libcminimal*'` → no hits, while `Phoenix/{Mesh.cpp:5,Texgen.cpp:3,Project.cpp:13,Scene.cpp:2}` include it. Confirms **D2**. |
| A.17 | `EstimateStringFormatLength` | `grep -rn 'EstimateStringFormatLength' <source>` → only `String.h:218,219`. Confirms **U2** resolved (dead declaration). |
| A.18 | `apply_patches.py` `Dictionary.h` anchor | source `Dictionary.h:571,572` matches the anchor verbatim; the vendored file's `SortByKey` sits at `:649` vs `:647` in the source, i.e. the two inserted lines are present. Confirms **U8** resolved. |
| A.19 | macOS GLU availability | `clang++ -std=c++17 -DGL_SILENCE_DEPRECATION /tmp/tgl.cpp -framework OpenGL -o tgl` where `tgl.cpp` is `#include <OpenGL/glu.h>` + `gluNewTess()` → **compile, link and run all succeed** (one `-Wdeprecated-declarations` warning without `GL_SILENCE_DEPRECATION`). `<OpenGL/glu.h>` is present in the Xcode SDK (`…/MacOSX.sdk/System/Library/Frameworks/OpenGL.framework/Headers/glu.h`) but **not** in `/System/Library/Frameworks/OpenGL.framework/Headers/`. ⇒ the `Mesh.cpp` GLU tessellator is usable on macOS today; it is deprecated, not absent. |
| A.20 | post-fix re-verification of P1/P3/P8/P8b/P11/D1/D2 | all confirmed fixed in the live tree (see §12.3). **Still open at time of writing: P4, P5, P6, P7, P9, P10, P12, P13, P14, P16, P17.** |

## Appendix B — commands used to produce the §1/§2/§5/§6 tables

```
# per-file Win32 census
grep -rn --include=*.cpp --include=*.h --include=*.c -wE \
  'HWND|HINSTANCE|HANDLE|HDC|HMODULE|DWORD|LPCTSTR|LPTSTR|TCHAR|WPARAM|LRESULT|HBRUSH|...' \
  Bedrock/BaseLib Bedrock/UtilLib apEx/Phoenix apEx/Phoenix_Tool
# TCHAR / _T() distribution
grep -rc 'TCHAR' <dir>/*.h <dir>/*.cpp | grep -v ':0'
grep -rho '_T(' <dir> | wc -l
# missing-include heuristic (usage vs. included headers per file)
python3 - <<'EOF'  # see §4.6 for the output
EOF
# class/struct forward-declaration kind mismatch sweep
python3 - <<'EOF'  # 0 mismatches over the four dirs
EOF
# MSVC-identifier gap analysis against compat/ and src/
python3 - <<'EOF'  # produced the _vsctprintf / _tstoi / _tcsncpy_s findings
EOF
```
