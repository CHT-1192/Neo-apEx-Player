// CphxSound: honest "music unavailable" implementation for this port.
//
// The original CphxSound (Phoenix_Tool/Audio/phxSound.cpp) renders the whole
// song offline through one of four synth back-ends and plays it from a
// DirectSound buffer.  None of that is portable:
//
//   * MVX, the synth Clean Slate uses, ships only as closed-source 32-bit
//     Windows object code (.lib) with SSE and DirectSound inside - there is no
//     source to port (see docs/spec/music-mvx.md §4);
//   * the V2 / WaveSabre / Ogg back-ends are Windows/DirectSound too.
//
// Every Audio/*.cpp is therefore excluded from the build, but
// CapexProject::LoadSong() and friends still reference CphxSound, so the
// symbols have to exist.  This file provides them, and Load() deliberately
// fails with a clear message instead of pretending to succeed: the player then
// runs on the wall clock, which reproduces the demo's timing exactly because
// the release player's frame number is derived from song milliseconds
// (docs/spec/engine-pipeline.md §1) and the .mvm tick timeline is fully
// recoverable from its header (src/player/song_clock.cpp).
#include "Audio/phxSound.h"

#include "rhi/vk_core.h"

//////////////////////////////////////////////////////////////////////////////
// statics (declared private in the class; DirectSound is never touched)

LPDIRECTSOUND CphxSound::pDirectSound = nullptr;
LPDIRECTSOUNDBUFFER CphxSound::pPrimaryBuffer = nullptr;
WAVEFORMATEX CphxSound::wfxFormat = {};
DSBUFFERDESC CphxSound::pBufferDescPrimary = {};

//////////////////////////////////////////////////////////////////////////////

CphxSound::CphxSound() : pSong(nullptr)
{
}

CphxSound::~CphxSound()
{
  // pSong is always null here: Load() never creates one.
  pSong = nullptr;
}

TBOOL CphxSound::Init(HWND)
{
  phxvk::LogWarn("[mvx] audio output is not available in this port (DirectSound back-ends only)");
  return false;
}

TBOOL CphxSound::Deinit()
{
  return true;
}

TBOOL CphxSound::Load(CStreamReader &stream, SOUND_LOADCALLBACK pLoad, void *pUserData)
{
  (void)stream;
  (void)pLoad;
  (void)pUserData;

  // This is the engine's own in-process synth, which only the Windows tool calls
  // (it drives MVX and hands the result to DirectSound).  The player never uses
  // this path: it plays a pre-rendered track through SDL and takes its clock from
  // the device's play cursor, so the music works even though this returns false.
  // The message used to claim playback fell back to the wall clock, which stopped
  // being true once the player grew an audio path - and it printed even while the
  // music was playing, which was actively misleading.
  phxvk::LogWarn("[mvx] the in-engine synthesiser is unavailable (MVX is closed-source 32-bit "
                 "Windows code). This is not the player's audio path: it plays a pre-rendered "
                 "track if one is present, otherwise it runs on the wall clock, which reproduces "
                 "the original frame timing exactly. See README.md for how to produce the track.");
  return false;
}

TBOOL CphxSound::Play(TU32)
{
  return false;
}

TBOOL CphxSound::Stop()
{
  return true;
}

TU32 CphxSound::GetSongLength()
{
  return 0;
}

TBOOL CphxSound::GetMarkers(CArray<TU32> &aMarkers)
{
  (void)aMarkers;
  return false;
}

TBOOL CphxSound::IsLoaded()
{
  return false;
}

TU32 CphxSound::GetPlayPosition()
{
  return 0;
}

CString CphxSound::GetFileTypeMacro()
{
  return CString(_T("NONE"));
}

CString CphxSound::GetSynthConfig()
{
  return CString();
}
