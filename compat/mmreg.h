#pragma once
// <mmreg.h> is the multimedia "registry" header: the WAVEFORMAT* family and
// the WAVE_FORMAT_* tags.  The port only needs it so that the WaveSabre
// headers (vendored, not compiled - the audio path is excluded) parse.
#include "MMSystem.h"

#define WAVE_FORMAT_1M08 0x00000001
#define WAVE_FORMAT_1S08 0x00000002
#define WAVE_FORMAT_1M16 0x00000004
#define WAVE_FORMAT_1S16 0x00000008
#define WAVE_FORMAT_2M08 0x00000010
#define WAVE_FORMAT_2S08 0x00000020
#define WAVE_FORMAT_2M16 0x00000040
#define WAVE_FORMAT_2S16 0x00000080
#define WAVE_FORMAT_4M08 0x00000100
#define WAVE_FORMAT_4S08 0x00000200
#define WAVE_FORMAT_4M16 0x00000400
#define WAVE_FORMAT_4S16 0x00000800

typedef struct tWAVEFORMATEXTENSIBLE
{
  WAVEFORMATEX Format;
  union
  {
    WORD wValidBitsPerSample;
    WORD wSamplesPerBlock;
    WORD wReserved;
  } Samples;
  DWORD dwChannelMask;
  GUID SubFormat;
} WAVEFORMATEXTENSIBLE, *PWAVEFORMATEXTENSIBLE;
