#pragma once
// DirectSound declarations for the apEx *tool* sound layer.
//
// This port does not implement DirectSound.  The MVX synthesiser that the
// original player uses to render the music is closed-source 32-bit Windows
// object code with no portable source (see docs/spec/music-mvx.md), and every
// translation unit that would call into DirectSound
// (Phoenix_Tool/Audio/*.cpp) is excluded from the build.
//
// This header exists because the *shared declarations* still have to parse:
// apExProject.h includes Audio/phxSound.h, which names LPDIRECTSOUND,
// LPDIRECTSOUNDBUFFER and DSBUFFERDESC as members.  Only the types are needed;
// no function here is ever called.
#include "Windows.h"

struct IDirectSound;
struct IDirectSound8;
struct IDirectSoundBuffer;
struct IDirectSoundBuffer8;
struct IDirectSoundNotify;

typedef IDirectSound *LPDIRECTSOUND;
typedef IDirectSound8 *LPDIRECTSOUND8;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER;
typedef IDirectSoundBuffer8 *LPDIRECTSOUNDBUFFER8;

typedef struct _DSCAPS
{
  DWORD dwSize, dwFlags, dwMinSecondarySampleRate, dwMaxSecondarySampleRate;
  DWORD dwPrimaryBuffers, dwMaxHwMixingAllBuffers, dwMaxHwMixingStaticBuffers;
  DWORD dwMaxHwMixingStreamingBuffers, dwFreeHwMixingAllBuffers;
  DWORD dwFreeHwMixingStaticBuffers, dwFreeHwMixingStreamingBuffers;
  DWORD dwMaxHw3DAllBuffers, dwMaxHw3DStaticBuffers, dwMaxHw3DStreamingBuffers;
  DWORD dwFreeHw3DAllBuffers, dwFreeHw3DStaticBuffers, dwFreeHw3DStreamingBuffers;
  DWORD dwTotalHwMemBytes, dwFreeHwMemBytes, dwMaxContigFreeHwMemBytes;
  DWORD dwUnlockTransferRateHwBuffers, dwPlayCpuOverheadSwBuffers, dwReserved1, dwReserved2;
} DSCAPS, *LPDSCAPS;

typedef struct _DSBCAPS
{
  DWORD dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead;
} DSBCAPS, *LPDSBCAPS;

typedef struct _DSBUFFERDESC
{
  DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
  void *lpwfxFormat;
  DWORD dw3DAlgorithm;
} DSBUFFERDESC, *LPDSBUFFERDESC;

typedef struct _DSBPOSITIONNOTIFY
{
  DWORD dwOffset;
  HANDLE hEventNotify;
} DSBPOSITIONNOTIFY, *LPDSBPOSITIONNOTIFY;

#define DSCAPS_PRIMARYMONO 0x00000001
#define DSCAPS_PRIMARYSTEREO 0x00000002
#define DSCAPS_PRIMARY8BIT 0x00000004
#define DSCAPS_PRIMARY16BIT 0x00000008
#define DSCAPS_CONTINUOUSRATE 0x00000010
#define DSCAPS_EMULDRIVER 0x00000020
#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_STATIC 0x00000002
#define DSBCAPS_LOCHARDWARE 0x00000004
#define DSBCAPS_LOCSOFTWARE 0x00000008
#define DSBCAPS_CTRL3D 0x00000010
#define DSBCAPS_CTRLFREQUENCY 0x00000020
#define DSBCAPS_CTRLPAN 0x00000040
#define DSBCAPS_CTRLVOLUME 0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100
#define DSBCAPS_CTRLFX 0x00000200
#define DSBCAPS_STICKYFOCUS 0x00004000
#define DSBCAPS_GLOBALFOCUS 0x00008000
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBCAPS_TRUEPLAYPOSITION 0x00080000
#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_BUFFERLOST 0x00000002
#define DSBSTATUS_LOOPING 0x00000004
#define DSSCL_NORMAL 0x00000001
#define DSSCL_PRIORITY 0x00000002
#define DSSCL_EXCLUSIVE 0x00000003
#define DSSCL_WRITEPRIMARY 0x00000004
#define DSERR_BUFFERLOST 0x88780096L
#define DS_OK 0L
