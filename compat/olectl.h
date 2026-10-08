#pragma once
// compat/olectl.h
//
// Portable stand-in for the Windows SDK <olectl.h> / <ocidl.h> picture subset.
//
// The original apEx tool decodes embedded pictures (bitmaps and metafiles) with
// OLE: OleLoadPicture(Ex) hands back an IPicture and Phoenix/Texgen.cpp renders
// it into a GDI DC.  That code is compiled in this port - PhoenixConfig_Full.h
// defines PHX_TEXGEN_IMAGE, so Texgen.cpp's LoadMetafile is reachable whenever
// D3DX11CreateTextureFromMemory cannot decode a payload (i.e. anything that is
// not DDS) - so the interfaces below are real declarations, not placeholders.
//
//   * IStream / IPicture keep the genuine Windows COM vtable order, so code
//     written against the real headers would interoperate.
//   * Implemented in src/platform/gdi_posix.cpp on ImageIO/CoreGraphics.
//   * Bedrock/BaseLib/ImageDecompressor.cpp is excluded from the build; it would
//     additionally need GlobalAlloc/CreateStreamOnHGlobal, which this port does
//     not provide (see cmake/engine_sources.cmake).
//
// The COM foundation (IUnknown, REFIID, STDMETHOD*, HRESULT) comes from
// compat/D3Dcommon.h, which is where compat/Windows.h keeps it.

#include "Windows.h"
#include "D3Dcommon.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- OLE scalar types ------------------------------------------------------
typedef UINT OLE_HANDLE;
typedef LONG OLE_XPOS_HIMETRIC;
typedef LONG OLE_YPOS_HIMETRIC;
typedef LONG OLE_XSIZE_HIMETRIC;
typedef LONG OLE_YSIZE_HIMETRIC;

// LoadPictureConstants
#define LP_DEFAULT     0x0000
#define LP_MONOCHROME  0x0001
#define LP_VGACOLOR    0x0002
#define LP_COLOR       0x0004

// PICTYPE
#define PICTYPE_UNINITIALIZED (-1)
#define PICTYPE_NONE          0
#define PICTYPE_BITMAP        1
#define PICTYPE_METAFILE      2
#define PICTYPE_ICON          3
#define PICTYPE_ENHMETAFILE   4

typedef WCHAR *LPOLESTR;
typedef const WCHAR *LPCOLESTR;

// winuser.h type used by IPicture::Render; compat/Windows.h only has LPRECT.
#ifndef _DSH_LPCRECT_DEFINED
#define _DSH_LPCRECT_DEFINED
typedef const RECT *LPCRECT;
#endif

typedef struct _ULARGE_INTEGER
{
  union
  {
    struct
    {
      DWORD LowPart;
      DWORD HighPart;
    };
    ULONGLONG QuadPart;
  };
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct tagSTATSTG
{
  LPOLESTR pwcsName;
  DWORD    type;
  ULARGE_INTEGER cbSize;
  FILETIME mtime;
  FILETIME ctime;
  FILETIME atime;
  DWORD    grfMode;
  DWORD    grfLocksSupported;
  GUID     clsid;
  DWORD    grfStateBits;
  DWORD    reserved;
} STATSTG;

enum STGTY
{
  STGTY_STORAGE   = 1,
  STGTY_STREAM    = 2,
  STGTY_LOCKBYTES = 3,
  STGTY_PROPERTY  = 4
};

enum STREAM_SEEK
{
  STREAM_SEEK_SET = 0,
  STREAM_SEEK_CUR = 1,
  STREAM_SEEK_END = 2
};

#define STGC_DEFAULT        0
#define STGC_OVERWRITE      1
#define STGC_ONLYIFCURRENT  2
#define STGC_DANGEROUSLYCOMMITMERELYTODISKCACHE 4
#define STGC_CONSOLIDATE    8

#ifdef __cplusplus
} // extern "C"
#endif

//////////////////////////////////////////////////////////////////////////////
// IStream / IPicture
//
// Method order is the IDL declaration order from objidl.h / ocidl.h.  Only the
// methods the engine reaches have a real implementation; the rest return
// E_NOTIMPL (and log once, see gdi_posix.cpp).
//////////////////////////////////////////////////////////////////////////////

struct ISequentialStream : public IUnknown
{
  virtual HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) = 0;
  virtual HRESULT STDMETHODCALLTYPE Write(const void *pv, ULONG cb, ULONG *pcbWritten) = 0;
};

struct IStream : public ISequentialStream
{
  virtual HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) = 0;
  virtual HRESULT STDMETHODCALLTYPE CopyTo(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) = 0;
  virtual HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) = 0;
  virtual HRESULT STDMETHODCALLTYPE Revert(void) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) = 0;
  virtual HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD grfStatFlag) = 0;
  virtual HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) = 0;
};

typedef IStream *LPSTREAM;

struct IPicture : public IUnknown
{
  virtual HRESULT STDMETHODCALLTYPE get_Handle(OLE_HANDLE *phandle) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_hPal(OLE_HANDLE *phpal) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Type(SHORT *ptype) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Width(OLE_XSIZE_HIMETRIC *pwidth) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Height(OLE_YSIZE_HIMETRIC *pheight) = 0;
  virtual HRESULT STDMETHODCALLTYPE Render(HDC hdc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC xSrc, OLE_YPOS_HIMETRIC ySrc, OLE_XSIZE_HIMETRIC cxSrc,
                                           OLE_YSIZE_HIMETRIC cySrc, LPCRECT pRcWBounds) = 0;
  virtual HRESULT STDMETHODCALLTYPE set_hPal(OLE_HANDLE hpal) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_CurDC(HDC *phdc) = 0;
  virtual HRESULT STDMETHODCALLTYPE SelectPicture(HDC hdcIn, HDC *phdcOut, OLE_HANDLE *phbmpOut) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_KeepOriginalFormat(BOOL *pfkeep) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_KeepOriginalFormat(BOOL fkeep) = 0;
  virtual HRESULT STDMETHODCALLTYPE PictureChanged(void) = 0;
  virtual HRESULT STDMETHODCALLTYPE SaveAsFile(LPSTREAM pstm, BOOL fSaveMemCopy, LONG *pcbSize) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Attributes(DWORD *pdwAttr) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Attributes(DWORD dwAttr) = 0;
};

typedef IPicture *LPPICTURE;

#ifdef __cplusplus
extern "C" {
#endif

// The genuine OLE IPicture IID {7BF80980-BF32-101A-8BBB-00AA00300CAB}; defined
// in src/platform/gdi_posix.cpp.
extern const GUID IID_IPicture;

HRESULT WINAPI OleLoadPictureEx(LPSTREAM lpstream, LONG lSize, BOOL fRunmode, REFIID riid, DWORD xSizeDesired, DWORD ySizeDesired, DWORD dwFlags, LPVOID *lplpvObj);
HRESULT WINAPI OleLoadPicture(LPSTREAM lpstream, LONG lSize, BOOL fRunmode, REFIID riid, LPVOID *lplpvObj);

// Declared by <shlwapi.h> on Windows; a duplicate-but-identical declaration is
// harmless (compat/shlwapi.h belongs to the shell-path work).
IStream *WINAPI SHCreateMemStream(const BYTE *pInit, UINT cbInit);

#ifdef __cplusplus
}
#endif
