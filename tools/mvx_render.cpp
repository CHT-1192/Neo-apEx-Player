// Offline MVX renderer.
//
// MVX is the softsynth that turns a .mvm into Clean Slate's music.  It is closed
// source - "REALTIME BUZZ-BASED SYNTHESIZER, Written by Gargaj/Conspiracy" - and
// the apEx release ships only mvx.h plus x86-32 MSVC static libraries.  It
// therefore cannot be ported to the player's own platforms, but it does not have
// to be: it runs natively on 32-bit Windows, and GitHub's windows-latest runners
// execute x86-32 through WoW64, which makes them the one place the real synth can
// be run as part of the build.
//
// This is not a reimplementation and not a guess.  The call sequence below is
// copied from the tool's own offline renderer
// (apEx/Phoenix_Tool/Audio/phxSongMVM.cpp:OfflineRenderSong), including the two
// things that are easy to miss:
//
//   * the synth is DRIVEN BY HAND: every machine's Tick() must be called once per
//     tick before the mixer renders it.  mvxRenderOffline() looks like the right
//     entry point and is exported, but it does not set that up and crashes.
//   * the x87 control word must be switched to single precision (0x0200).  The
//     DSP was written for 24-bit-mantissa arithmetic; leaving the FPU in extended
//     precision changes the output.
//
// Output is both a WAV (to listen to / inspect) and the original tool's
// SoundCache container (!CSA), which the player reads and auto-discovers.
//
//   mvx_render <song.mvm> <out.wav> [out.apxsndcache]
//
// Build (32-bit is required - the libraries are x86):
//   cl /nologo /EHsc /O2 /I third_party/apEx/Libraries/MVX tools/mvx_render.cpp \
//      third_party/apEx/Libraries/MVX/mvxPlayerLib_vs2019_releasestatic.lib \
//      /link /NODEFAULTLIB:libucrt.lib /NODEFAULTLIB:libvcruntime.lib \
//      msvcrt.lib legacy_stdio_definitions.lib oldnames.lib dsound.lib winmm.lib

#include "mvx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// Declared locally, exactly as the tool does: mvx.h does not expose the machine
// array.  The vtable order matters - Tick() first, then Render().
class mvxMachine
{
public:
  mvxMachine(){};
  virtual void Tick() = NULL;
  virtual int Render(float *buffer, int numsamp, float preamp) = NULL;
};

extern mvxMachine **mvxSystem_Machines;

// Re-declared for the same reason: mvx.h exposes the counters but not the
// sequence pointers the loader fills in.
extern BYTE *mvxSystem_MasterSeq;
extern BYTE *mvxSystem_TPBSeq;
extern BYTE *mvxSystem_BPMSeq;
extern int mvxSystem_SamplesPerTick;

static void PutU32(FILE *f, unsigned v)
{
  unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                        (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
  fwrite(b, 1, 4, f);
}

static void PutU16(FILE *f, unsigned v)
{
  unsigned char b[2] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff)};
  fwrite(b, 1, 2, f);
}

// Canonical RIFF/WAVE, 16-bit stereo at the engine's 44100 Hz.
static void WriteWavHeader(FILE *f, unsigned dataBytes)
{
  const unsigned channels = 2, bits = 16, rate = MVX_SAMPLERATE;
  const unsigned blockAlign = channels * bits / 8, byteRate = rate * blockAlign;

  fwrite("RIFF", 1, 4, f);
  PutU32(f, 36 + dataBytes);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f);
  PutU32(f, 16);
  PutU16(f, 1);
  PutU16(f, channels);
  PutU32(f, rate);
  PutU32(f, byteRate);
  PutU16(f, blockAlign);
  PutU16(f, bits);
  fwrite("data", 1, 4, f);
  PutU32(f, dataBytes);
}

static unsigned char *ReadWholeFile(const char *path, long *sizeOut)
{
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return NULL; }
  unsigned char *data = (unsigned char *)malloc((size_t)size);
  if (!data) { fclose(f); return NULL; }
  if (fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return NULL; }
  fclose(f);
  *sizeOut = size;
  return data;
}

// Samples per tick is (60 * rate) / (TPB * BPM) and TPB/BPM both change during
// the song, so the per-tick buffer must be sized for the *largest* tick, not the
// first one.  The original allocates once from the initial value; that is only
// safe when the first tick happens to be the longest, so compute it properly.
static int WorstCaseSamplesPerTick()
{
  int worst = 0, tpb = mvxSystem_TPB, bpm = mvxSystem_BPM;
  for (int i = 0; i < mvxSystem_NumMasterTicks; i++)
  {
    if (mvxSystem_TPBSeq && mvxSystem_TPBSeq[i] != 0xFF) tpb = mvxSystem_TPBSeq[i];
    if (mvxSystem_BPMSeq && mvxSystem_BPMSeq[i] != 0xFF) bpm = mvxSystem_BPMSeq[i];
    if (tpb <= 0 || bpm <= 0) continue;
    const int n = (60 * MVX_SAMPLERATE) / (tpb * bpm);
    if (n > worst) worst = n;
  }
  return worst > 0 ? worst : mvxSystem_SamplesPerTick;
}

int main(int argc, char **argv)
{
  // Unbuffered: if the synth faults part way through, every line it managed to
  // print still reaches the log.  (The first attempt produced a 0-byte file and
  // no output at all, which said nothing about why.)
  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc < 3)
  {
    fprintf(stderr, "usage: %s <song.mvm> <out.wav> [out.apxsndcache]\n", argv[0]);
    return 2;
  }

  long musicSize = 0;
  unsigned char *music = ReadWholeFile(argv[1], &musicSize);
  if (!music)
  {
    fprintf(stderr, "mvx_render: cannot read %s\n", argv[1]);
    return 1;
  }
  printf("mvx_render: %s, %ld bytes\n", argv[1], musicSize);

  if (!mvxSystem_LoadMusic((char *)music, (int)musicSize))
  {
    fprintf(stderr, "mvx_render: mvxSystem_LoadMusic failed (not a valid .mvm?)\n");
    free(music);
    return 1;
  }

  printf("mvx_render: %d machines, %d ticks, %d bpm / %d tpb, %d connections\n",
         mvxSystem_NumMachines, mvxSystem_SongLength, mvxSystem_BPM, mvxSystem_TPB,
         mvxSystem_NumConnections);

  mvxMixer_Init();

#ifdef _M_IX86
  // Single precision, as the tool does.  MVX's DSP assumes a 24-bit mantissa;
  // leaving the x87 unit in extended precision would change the mix.
  {
    unsigned short cw = 0;
    __asm { fstcw cw }
    cw |= 0x0200;
    __asm { fldcw cw }
  }
#endif

  const int maxSamples = WorstCaseSamplesPerTick();
  printf("mvx_render: buffer for %d samples/tick (worst case)\n", maxSamples);

  float *mix32 = new float[(size_t)maxSamples * 4];
  short *mix16 = new short[(size_t)maxSamples * 4];

  FILE *out = fopen(argv[2], "wb");
  if (!out)
  {
    fprintf(stderr, "mvx_render: cannot write %s\n", argv[2]);
    return 1;
  }
  WriteWavHeader(out, 0); // patched once the real length is known

  std::vector<unsigned> markers;
  markers.reserve((size_t)mvxSystem_SongLength);

  printf("mvx_render: rendering %d ticks...\n", mvxSystem_SongLength);

  mvxSystem_MasterTick = 0;
  unsigned long long framesRendered = 0;

  for (int tick = 0; tick < mvxSystem_SongLength; tick++)
  {
    // 1. let every machine advance its sequencer for this tick
    for (int m = 0; m < mvxSystem_NumMachines; m++)
      if (mvxSystem_Machines[m]) mvxSystem_Machines[m]->Tick();

    // 2. apply the master sequences (0xFF means "keep the previous value")
    if (mvxSystem_MasterTick < mvxSystem_NumMasterTicks)
    {
      if (mvxSystem_MasterSeq[mvxSystem_MasterTick] != 0xFF)
        mvxSystem_MasterVolume = mvxSystem_MasterSeq[mvxSystem_MasterTick];
      if (mvxSystem_TPBSeq[mvxSystem_MasterTick] != 0xFF)
        mvxSystem_TPB = mvxSystem_TPBSeq[mvxSystem_MasterTick];
      if (mvxSystem_BPMSeq[mvxSystem_MasterTick] != 0xFF)
        mvxSystem_BPM = mvxSystem_BPMSeq[mvxSystem_MasterTick];
      mvxSystem_SamplesPerTick = (60 * MVX_SAMPLERATE) / (mvxSystem_TPB * mvxSystem_BPM);
    }
    mvxSystem_MasterTick++;

    const int n = mvxSystem_SamplesPerTick;
    if ((size_t)n * 4 > (size_t)maxSamples * 4)
    {
      fprintf(stderr, "mvx_render: tick %d wants %d samples, buffer holds %d\n", tick, n, maxSamples);
      break;
    }

    // 3. render and convert
    mvxMixer_Render(mix32, n);
    mvxSystem_ClipBuffer(mix32, mix16, n * 2);

    markers.push_back((unsigned)((framesRendered * 1000) / MVX_SAMPLERATE)); // marker in ms
    if (fwrite(mix16, 1, (size_t)n * MVX_SAMPLESIZE, out) != (size_t)n * MVX_SAMPLESIZE)
    {
      fprintf(stderr, "mvx_render: short write at tick %d\n", tick);
      break;
    }
    framesRendered += n;

    if ((tick % 250) == 0)
      printf("  tick %d/%d (%.1f s)\n", tick, mvxSystem_SongLength,
             (double)framesRendered / MVX_SAMPLERATE);
  }

  printf("mvx_render: %llu frames (%.2f s)\n", framesRendered,
         (double)framesRendered / MVX_SAMPLERATE);

  const unsigned dataBytes = (unsigned)(framesRendered * MVX_SAMPLESIZE);
  fseek(out, 0, SEEK_SET);
  WriteWavHeader(out, dataBytes);
  fclose(out);

  mvxMixer_DeInit();
  mvxSystem_FreeMusic();
  delete[] mix32;
  delete[] mix16;

  // The player reads the tool's own cache format, so emit that too and it gets
  // picked up with no flags at all.  Layout per docs/spec/music-mvx.md 1.6.
  if (argc > 3)
  {
    FILE *cache = fopen(argv[3], "wb");
    if (!cache)
    {
      fprintf(stderr, "mvx_render: cannot write %s\n", argv[3]);
    }
    else
    {
      const char *config = "rendered by mvx_render (original MVX library)";
      fwrite("!CSA", 1, 4, cache);
      PutU32(cache, (unsigned)strlen(config) + 1);
      fwrite(config, 1, strlen(config) + 1, cache);
      PutU32(cache, (unsigned)markers.size());
      for (size_t i = 0; i < markers.size(); i++) PutU32(cache, markers[i]);
      PutU32(cache, dataBytes);
      // Stream the payload back out of the WAV rather than holding the whole
      // song in memory a second time.
      FILE *wav = fopen(argv[2], "rb");
      if (wav)
      {
        fseek(wav, 44, SEEK_SET);
        char buf[64 * 1024];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), wav)) > 0) fwrite(buf, 1, got, cache);
        fclose(wav);
      }
      fclose(cache);
      printf("mvx_render: wrote %s (%d markers)\n", argv[3], (int)markers.size());
    }
  }

  free(music);
  printf("mvx_render: wrote %s (%u data bytes)\n", argv[2], dataBytes);
  return framesRendered > 0 ? 0 : 1;
}
