// SDL3 audio output for the pre-rendered song.
//
// The apEx tool renders the entire song to memory and then plays it from one
// big DirectSound buffer, reading the play cursor for the sync position
// (docs/spec/music-mvx.md §4).  SDL3's streaming device is the direct
// equivalent: queue the whole track once and derive the playhead from
// "bytes queued minus bytes still buffered", which is the same quantity
// DirectSound's GetCurrentPosition() reports.
#pragma once

#include <cstdint>
#include <string>

struct SDL_AudioStream;

namespace phxplayer
{

class AudioOut
{
public:
  AudioOut() = default;
  ~AudioOut();
  AudioOut(const AudioOut &) = delete;
  AudioOut &operator=(const AudioOut &) = delete;

  // `channels` is the *source* channel count; the device always runs stereo.
  bool Open(int sampleRate, int channels, std::string *error);
  void Close();
  bool IsOpen() const { return stream != nullptr; }

  // Hands over the whole track.  Nothing is queued yet: Pump() feeds the device
  // a little ahead of the play cursor, because pushing 50 MB of PCM in one
  // SDL_PutAudioStreamData() call overruns SDL's staging buffer (it memmoves off
  // the end and faults).
  bool SetSource(const int16_t *samples, size_t count);

  // Top the device up.  Call once per frame; keeps ~250 ms queued.
  void Pump();

  // Song position in milliseconds, from the device's own play cursor.
  double PositionMs() const;

  void Resume();
  void Pause();
  void SetPaused(bool paused);
  bool IsPaused() const { return paused; }

  // Restarts playback from `ms` (clears what is buffered, then re-queues from
  // that offset).
  void SeekMs(double ms);

  const std::string &Error() const { return error; }

  // Test hooks: the playback position the queue bookkeeping believes in, so a
  // headless test can verify the playhead without a sound card.
  uint64_t FramesConsumed() const;
  bool SourceExhausted() const;

private:
  void RecomputeConsumed() const;
  void FeedFrames(uint64_t frames);

  SDL_AudioStream *stream = nullptr;
  int rate = 44100;
  int sourceChannels = 2;
  bool paused = false;
  std::string error;

  // Source samples retained so SeekMs() can re-queue from an offset.
  const int16_t *samples = nullptr;
  size_t sampleCount = 0;

  // Source read cursor, in source frames.
  uint64_t sourceFrame = 0;
  uint64_t totalSourceFrames = 0;
  mutable uint64_t consumedFrames = 0;
};

} // namespace phxplayer
