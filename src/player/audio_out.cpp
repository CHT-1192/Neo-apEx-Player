#include "audio_out.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

namespace phxplayer
{

AudioOut::~AudioOut()
{
  Close();
}

bool AudioOut::Open(int sampleRate, int channels, std::string *err)
{
  Close();

  if (sampleRate <= 0) sampleRate = 44100;
  if (channels <= 0) channels = 2;
  rate = sampleRate;
  sourceChannels = channels;

  if (!SDL_WasInit(SDL_INIT_AUDIO))
  {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
      error = std::string("SDL_InitSubSystem(SDL_INIT_AUDIO): ") + SDL_GetError();
      if (err) *err = error;
      return false;
    }
  }

  // The engine's songs are stereo; a mono source is duplicated on the way in.
  SDL_AudioSpec want;
  SDL_zero(want);
  want.format = SDL_AUDIO_S16;
  want.channels = 2;
  want.freq = rate;

  stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
  if (!stream)
  {
    // Most often "no audio device" (a headless or remote session).  That is not
    // fatal - the caller falls back to the wall clock - so report precisely.
    error = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
    if (err) *err = error;
    return false;
  }

  error.clear();
  return true;
}

void AudioOut::Close()
{
  if (stream)
  {
    SDL_DestroyAudioStream(stream);
    stream = nullptr;
  }
  samples = nullptr;
  sampleCount = 0;
  sourceFrame = 0;
  totalSourceFrames = 0;
  consumedFrames = 0;
  paused = false;
}

bool AudioOut::SetSource(const int16_t *src, size_t count)
{
  if (!stream || !src || count == 0) return false;
  if (sourceChannels <= 0) sourceChannels = 2;

  samples = src;
  sampleCount = count;
  sourceFrame = 0;
  totalSourceFrames = (uint64_t)count / (uint64_t)sourceChannels;
  consumedFrames = 0;

  // Put a little in immediately so playback starts without a gap, then let
  // Pump() keep it topped up.
  Pump();
  return true;
}

// Appends `frames` source frames to the device stream, duplicating mono.
void AudioOut::FeedFrames(uint64_t frames)
{
  if (!stream || !samples) return;
  if (sourceFrame + frames > totalSourceFrames) frames = totalSourceFrames - sourceFrame;
  if (frames == 0) return;

  const int16_t *from = samples + (size_t)sourceFrame * (size_t)sourceChannels;
  std::vector<int16_t> duplicated;
  const int16_t *data = from;
  size_t outSamples = (size_t)frames * 2; // always stereo to the device

  if (sourceChannels == 1)
  {
    duplicated.resize(outSamples);
    for (uint64_t i = 0; i < frames; i++)
    {
      duplicated[(size_t)i * 2 + 0] = from[i];
      duplicated[(size_t)i * 2 + 1] = from[i];
    }
    data = duplicated.data();
  }

  SDL_PutAudioStreamData(stream, data, (int)(outSamples * sizeof(int16_t)));
  sourceFrame += frames;
}

void AudioOut::Pump()
{
  if (!stream || !samples) return;

  // Keep at most ~250 ms ahead of the play cursor.  The device drains the queue
  // as it plays, so this both starts playback and keeps it fed without ever
  // holding the whole track.
  const uint64_t want = (uint64_t)rate / 4;
  const int queuedBytes = SDL_GetAudioStreamQueued(stream);
  const uint64_t queuedFrames = queuedBytes > 0 ? (uint64_t)queuedBytes / (2u * sizeof(int16_t)) : 0;

  if (queuedFrames < want)
    FeedFrames(want - queuedFrames);
}

bool AudioOut::SourceExhausted() const
{
  return samples != nullptr && sourceFrame >= totalSourceFrames;
}

void AudioOut::RecomputeConsumed() const
{
  if (!stream) return;
  const int queuedBytes = SDL_GetAudioStreamQueued(stream);
  if (queuedBytes < 0) return;
  // Frames handed to the device minus frames it has not played yet.  Both are
  // in source frames (the device is always stereo).
  const uint64_t queuedFrames = (uint64_t)queuedBytes / (2u * sizeof(int16_t));
  consumedFrames = sourceFrame > queuedFrames ? sourceFrame - queuedFrames : 0;
}

double AudioOut::PositionMs() const
{
  if (!stream) return 0.0;
  RecomputeConsumed();
  return (double)consumedFrames * 1000.0 / (double)rate;
}

uint64_t AudioOut::FramesConsumed() const
{
  RecomputeConsumed();
  return consumedFrames;
}

void AudioOut::Resume()
{
  if (stream) SDL_ResumeAudioStreamDevice(stream);
}

void AudioOut::Pause()
{
  if (stream) SDL_PauseAudioStreamDevice(stream);
}

void AudioOut::SetPaused(bool p)
{
  paused = p;
  if (p) Pause(); else Resume();
}

void AudioOut::SeekMs(double ms)
{
  if (!stream || !samples || sampleCount == 0) return;

  if (ms < 0) ms = 0;
  uint64_t frame = (uint64_t)(ms * (double)rate / 1000.0);
  if (frame >= totalSourceFrames) frame = totalSourceFrames ? totalSourceFrames - 1 : 0;

  SDL_ClearAudioStream(stream);
  sourceFrame = frame;
  consumedFrames = 0;
  Pump();
}

} // namespace phxplayer
