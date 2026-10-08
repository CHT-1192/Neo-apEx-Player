#pragma once
#include "../../../Bedrock/BaseLib/BaseLib.h"
#include <mmsystem.h>
#include <dsound.h>

#include "phxSongBase.h"
// The concrete synth hosts (MVX, Ogg Vorbis, V2, WaveSabre) are NOT part of
// this port: MVX ships as closed-source 32-bit Windows object code and the
// other backends are Windows/DirectSound only (docs/spec/music-mvx.md).
// Their translation units are excluded from the build, so only the abstract
// base is needed here - and dropping them also removes the last dependency on
// the Windows multimedia headers.
//#include "phxSongMVM.h"
//#include "phxSongOGG.h"
//#include "phxSongV2M.h"
//#include "phxSongWaveSabre.h"

class CphxSound
{
  friend class CphxSongBase;
public:
  static TBOOL Init( HWND hWnd );
  static TBOOL Deinit();

  CphxSound();
  ~CphxSound();

  TBOOL Load( CStreamReader &stream, SOUND_LOADCALLBACK pLoad = NULL, void * pUserData = NULL );
  TBOOL Play( TU32 nMilliSec );
  TBOOL Stop();
  TU32 GetSongLength(); // millisec
  TBOOL GetMarkers( CArray<TU32> & aMarkers );
  TBOOL IsLoaded();

  TU32 GetPlayPosition();
  CString GetFileTypeMacro();
  CString GetSynthConfig();

private:
  static LPDIRECTSOUND pDirectSound;
  static LPDIRECTSOUNDBUFFER pPrimaryBuffer;
  static WAVEFORMATEX wfxFormat;
  static DSBUFFERDESC pBufferDescPrimary;
  CphxSongBase * pSong;
};