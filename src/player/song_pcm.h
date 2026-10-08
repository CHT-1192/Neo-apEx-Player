// Pre-rendered song PCM.
//
// The MVX synthesiser that turns a .mvm into audio exists only as closed-source
// 32-bit Windows object code (docs/spec/music-mvx.md §2), so this player cannot
// synthesise the music.  It does not have to: the original Windows tool renders
// the whole song to memory before playback and caches it, and that cache is a
// fully specified, portable container (docs/spec/music-mvx.md §1.6):
//
//   u32  '!CSA'                 0x41534321
//   u32  metadataSize           == strlen(headerContents) + 1
//   char headerContents[]       ASCIIZ synth configuration string
//   u32  numMarkers
//   u32  markers[numMarkers]    per-tick positions in MILLISECONDS
//   u32  sampleDataSize         bytes
//   s16  sampleData[]           interleaved stereo, 44100 Hz
//
// All little-endian.  Reading it (or a plain WAV next to the .mvm) makes real
// audio playback possible with no synth and no reverse engineering, and the
// marker table is the authoritative tick timeline.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace phxplayer
{

struct SongPcm
{
  bool valid = false;
  std::string source;      // path it was read from
  std::string format;      // "apxsndcache" | "wav"
  std::string synthConfig; // headerContents, empty for WAV
  std::vector<uint32_t> markers;  // tick start positions, milliseconds
  std::vector<int16_t> samples;   // interleaved
  uint32_t sampleRate = 44100;
  int channels = 2;
  std::string error;

  double DurationMs() const
  {
    if (sampleRate == 0 || channels == 0) return 0.0;
    const uint64_t frames = (uint64_t)samples.size() / (uint64_t)channels;
    return (double)frames * 1000.0 / (double)sampleRate;
  }
};

// Dispatches on the magic bytes, so either container can be passed in.
SongPcm LoadSongPcm(const char *path);

// Looks for a playable rendering of `mvmPath`:
//   1. <mvmPath>.apxsndcache          (exactly what the Windows tool writes)
//   2. the same name with .apxsndcache / .wav / .ogg / .mp3 substituted
//   3. SoundCache/<djb2-of-the-mvm>.apxsndcache next to it, which is where the
//      Windows tool puts it (phxSongBase::OfflineRender)
// Returns an invalid SongPcm with an empty `source` when there is nothing to
// play, which is not an error - the player then runs on the wall clock.
SongPcm FindSongPcm(const char *mvmPath, std::string *searched);

// The djb2 variant the original uses to name its cache files: hash starts at
// 5381 and is `hash * 33 + byte` over the whole .mvm, printed as %I64x.
uint64_t SongCacheHash(const uint8_t *data, size_t size);
uint64_t SongCacheHashFile(const char *path);

} // namespace phxplayer
