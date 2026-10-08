#include "song_clock.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace phxplayer
{

double NowMs()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

namespace
{
uint16_t ReadU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The original renders the whole song offline, one tick at a time:
//   SamplesPerTick = (60 * MVX_SAMPLERATE) / (TPB * BPM)      // integer division
//   after rendering tick k:  marker[k] = k_total_samples * 1000 / SAMPLERATE
// (Phoenix_Tool/Audio/phxSongMVM.cpp:51-71)
const uint32_t kSampleRate = 44100;
} // namespace

MvmInfo ParseMvmHeaderFromMemory(const uint8_t *data, size_t size)
{
  MvmInfo info;
  auto fail = [&](const char *why) {
    info.valid = false;
    info.error = why;
    return info;
  };

  if (!data || size < 0x100) return fail("file too small");
  if (memcmp(data, "MVM\0", 4) != 0) return fail("bad magic (expected 'MVM\\0')");

  info.initialBpm = data[0x004];
  info.initialTpb = data[0x005];
  info.numMasterTicks = ReadU16(data + 0x006);
  info.numMachines = ReadU16(data + 0x00A);
  info.sampleRate = kSampleRate;

  const uint32_t n = info.numMasterTicks;
  const size_t seqBase = 0x0F4;
  const size_t needed = seqBase + (size_t)n * 3 + 8;
  if (n == 0) return fail("zero master ticks");
  if (size < needed) return fail("truncated sequence tables");

  info.masterSeq.assign(data + seqBase, data + seqBase + n);
  info.tpbSeq.assign(data + seqBase + n, data + seqBase + 2 * n);
  info.bpmSeq.assign(data + seqBase + 2 * n, data + seqBase + 3 * n);

  info.tickStartSamples.reserve(n + 1);
  uint32_t tpb = info.initialTpb ? info.initialTpb : 5;
  uint32_t bpm = info.initialBpm ? info.initialBpm : 135;
  uint64_t samples = 0;
  for (uint32_t t = 0; t < n; t++)
  {
    info.tickStartSamples.push_back((uint32_t)samples);
    if (info.tpbSeq[t] != 0xFF) tpb = info.tpbSeq[t];
    if (info.bpmSeq[t] != 0xFF) bpm = info.bpmSeq[t];
    if (tpb == 0 || bpm == 0) return fail("degenerate tempo (tpb or bpm is zero)");
    samples += (uint64_t)((60 * kSampleRate) / (tpb * bpm));
  }
  info.tickStartSamples.push_back((uint32_t)samples);
  info.totalSamples = samples;
  info.totalMs = samples * 1000.0 / kSampleRate;
  info.valid = true;
  return info;
}

MvmInfo ParseMvmHeader(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    MvmInfo info;
    info.error = std::string("cannot open ") + path;
    return info;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0)
  {
    fclose(f);
    MvmInfo info;
    info.error = "empty file";
    return info;
  }
  std::vector<uint8_t> buf((size_t)n);
  size_t got = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  if (got != buf.size())
  {
    MvmInfo info;
    info.error = "short read";
    return info;
  }
  return ParseMvmHeaderFromMemory(buf.data(), buf.size());
}

void SongClock::Configure(const MvmInfo &song, double fps)
{
  songInfo = song;
  framerate = fps > 0 ? fps : 120.0;
}

void SongClock::Start()
{
  startWallMs = NowMs();
  manualOffsetMs = 0.0;
  started = true;
  paused = false;
  fixedFrame = 0;
}

void SongClock::Update()
{
  if (!started) Start();
}

double SongClock::PositionMs() const
{
  if (fixedStep != 0) return fixedFrame * 1000.0 / framerate;
  if (audioClock) return audioMs;
  if (!started) return manualOffsetMs;
  if (paused) return manualOffsetMs;
  return manualOffsetMs + (NowMs() - startWallMs);
}

int64_t SongClock::Frame() const
{
  if (fixedStep != 0) return fixedFrame;
  double ms = PositionMs();
  if (ms < 0) ms = 0;
  return (int64_t)floor(ms / 1000.0 * framerate);
}

double SongClock::FrameExact() const
{
  double ms = PositionMs();
  return ms < 0 ? 0 : ms / 1000.0 * framerate;
}

bool SongClock::Finished() const
{
  if (!songInfo.valid || songInfo.totalMs <= 0) return false;
  // A little slack so the last frame still renders.
  return PositionMs() > songInfo.totalMs + 1000.0 / framerate;
}

void SongClock::SeekMs(double ms)
{
  manualOffsetMs = ms;
  startWallMs = NowMs();
  started = true;
  audioMs = ms;
}

void SongClock::SetFrame(int64_t frame)
{
  fixedFrame = frame;
  SeekMs(frame * 1000.0 / framerate);
}

} // namespace phxplayer
