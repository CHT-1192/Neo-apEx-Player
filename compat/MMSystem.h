#pragma once
// Minimal <MMSystem.h>: the only multimedia timer entry points the engine uses.
#include "Windows.h"


typedef struct tWAVEFORMATEX
{
  WORD wFormatTag;
  WORD nChannels;
  DWORD nSamplesPerSec;
  DWORD nAvgBytesPerSec;
  WORD nBlockAlign;
  WORD wBitsPerSample;
  WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

#ifdef __cplusplus
extern "C" {
#endif

typedef UINT MMRESULT;
typedef UINT TIMECAPS;
#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97

DWORD timeGetTime(void);
MMRESULT timeGetDevCaps(TIMECAPS *caps, UINT size);
MMRESULT timeBeginPeriod(UINT period);
MMRESULT timeEndPeriod(UINT period);

#ifdef __cplusplus
}
#endif
