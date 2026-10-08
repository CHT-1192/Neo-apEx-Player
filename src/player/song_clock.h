// Song clock.
//
// The apEx release player derives the frame number from the music position:
//   frame = floor(song_milliseconds / 1000 * framerate)
// (see docs/spec/engine-pipeline.md §1 — MUSIC_GETSYNC() returns song ms).
//
// The MVX synthesiser is closed-source 32-bit Windows code with no portable
// source (docs/spec/music-mvx.md §4), so this player cannot render the music
// itself.  It can, however, reproduce the *clock* exactly: the .mvm header
// fully describes the tick timeline, and the demo is authored against it.
// We therefore drive playback from a monotonic wall clock (which is
// indistinguishable from the original's audio play cursor) and use the .mvm
// tick table to know the song's total length so playback can stop on time.
//
// If a pre-rendered PCM track is available (the original tool writes
// SoundCache/<djb2>.apxsndcache, see the music spec §6) it can drive the clock
// instead — SongClock::SetAudioClock() exists for that.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace phxplayer
{

// Parsed .mvm header.  Field layout verified against step010.mvm and
// step010padding.mvm; see docs/spec/music-mvx.md §1-2.
struct MvmInfo
{
  bool valid = false;
  uint8_t initialBpm = 0;
  uint8_t initialTpb = 0;
  uint16_t numMachines = 0;
  uint16_t numMasterTicks = 0;
  std::vector<uint8_t> masterSeq; // 0xFF = unchanged
  std::vector<uint8_t> tpbSeq;
  std::vector<uint8_t> bpmSeq;
  std::vector<uint32_t> tickStartSamples; // sample index at the START of each tick
  uint32_t sampleRate = 44100;
  uint64_t totalSamples = 0;
  double totalMs = 0.0;
  std::string error;
};

// Reads only the header + sequence tables; the pattern payload is not decoded.
MvmInfo ParseMvmHeader(const char *path);
MvmInfo ParseMvmHeaderFromMemory(const uint8_t *data, size_t size);

class SongClock
{
public:
  // `fps` is the project framerate (Clean Slate: 120).
  void Configure(const MvmInfo &song, double fps);
  void SetAudioClock(bool enabled) { audioClock = enabled; }
  // External audio position in milliseconds (from an SDL audio callback or a
  // decoded PCM track).  Only used when SetAudioClock(true).
  void SetAudioPositionMs(double ms) { audioMs = ms; }

  void Start();  // resets the wall clock / song position
  void Pause(bool p) { paused = p; }
  bool IsPaused() const { return paused; }

  // Advances using the wall clock (no-op when paused or in audio-clock mode).
  void Update();

  // Position within the demo, in milliseconds.
  double PositionMs() const;
  // floor(ms / 1000 * fps) — identical to the original player.
  int64_t Frame() const;
  double FrameExact() const;
  bool Finished() const;

  void SeekMs(double ms);
  void SetFrame(int64_t frame);

  // Fixed-step mode: Frame() advances by `step` on every AdvanceFixedStep().
  // Used for deterministic offline rendering (--frames N).
  void SetFixedStep(int64_t step) { fixedStep = step; }
  void AdvanceFixedStep() { fixedFrame += fixedStep; }

  double FrameRate() const { return framerate; }
  const MvmInfo &Song() const { return songInfo; }

private:
  MvmInfo songInfo;
  double framerate = 120.0;
  double startWallMs = 0.0;
  double manualOffsetMs = 0.0;
  double audioMs = 0.0;
  bool audioClock = false;
  bool paused = false;
  bool started = false;
  int64_t fixedFrame = 0;
  int64_t fixedStep = 0;
};

// Monotonic millisecond clock.
double NowMs();

} // namespace phxplayer
