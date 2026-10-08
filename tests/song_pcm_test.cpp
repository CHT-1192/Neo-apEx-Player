// Tests for the pre-rendered song reader (src/player/song_pcm.cpp).
//
// This is the piece that makes music playback possible without the closed-source
// MVX synthesiser: the Windows tool renders the whole song and writes a fully
// specified container (docs/spec/music-mvx.md §1.6), and a portable player only
// has to read it.  The reader is therefore worth pinning down precisely - a
// silently wrong sample count or marker table would desynchronise the demo.
//
//   ./build/phx_song_pcm_test [path-to-a-real.apxsndcache]

#include "song_pcm.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <vector>

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const char *what, const char *detail = "")
{
  checks++;
  if (ok)
  {
    printf("  ok   %s\n", what);
  }
  else
  {
    failures++;
    printf("  FAIL %s%s%s\n", what, detail[0] ? " -- " : "", detail);
  }
}

static void CheckEq(long long got, long long want, const char *what)
{
  char detail[160];
  snprintf(detail, sizeof(detail), "got %lld, want %lld", got, want);
  Check(got == want, what, detail);
}

namespace
{

void PutU32(std::vector<uint8_t> &v, uint32_t x)
{
  v.push_back((uint8_t)(x & 0xff));
  v.push_back((uint8_t)((x >> 8) & 0xff));
  v.push_back((uint8_t)((x >> 16) & 0xff));
  v.push_back((uint8_t)((x >> 24) & 0xff));
}

void PutS16(std::vector<uint8_t> &v, int16_t x)
{
  v.push_back((uint8_t)((uint16_t)x & 0xff));
  v.push_back((uint8_t)(((uint16_t)x >> 8) & 0xff));
}

bool WriteFile(const char *path, const std::vector<uint8_t> &data)
{
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

// Builds a container exactly as phxSongBase::OfflineRender writes it.
std::vector<uint8_t> MakeSndCache(const std::string &config,
                                  const std::vector<uint32_t> &markers,
                                  const std::vector<int16_t> &samples)
{
  std::vector<uint8_t> v;
  const char magic[4] = {'!', 'C', 'S', 'A'};
  v.insert(v.end(), magic, magic + 4);
  PutU32(v, (uint32_t)config.size() + 1); // metadataSize includes the NUL
  v.insert(v.end(), config.begin(), config.end());
  v.push_back(0);
  PutU32(v, (uint32_t)markers.size());
  for (uint32_t m : markers) PutU32(v, m);
  PutU32(v, (uint32_t)(samples.size() * 2));
  for (int16_t s : samples) PutS16(v, s);
  return v;
}

std::vector<uint8_t> MakeWav(const std::vector<int16_t> &samples, uint16_t channels, uint32_t rate)
{
  const uint32_t dataBytes = (uint32_t)(samples.size() * 2);
  std::vector<uint8_t> v;
  const char *riff = "RIFF";
  v.insert(v.end(), riff, riff + 4);
  PutU32(v, 36 + dataBytes);
  const char *wave = "WAVE";
  v.insert(v.end(), wave, wave + 4);
  const char *fmt = "fmt ";
  v.insert(v.end(), fmt, fmt + 4);
  PutU32(v, 16);
  v.push_back(1); v.push_back(0);                       // PCM
  v.push_back((uint8_t)(channels & 0xff)); v.push_back(0);
  PutU32(v, rate);
  PutU32(v, rate * channels * 2);                       // byte rate
  v.push_back((uint8_t)(channels * 2)); v.push_back(0); // block align
  v.push_back(16); v.push_back(0);                      // bits
  const char *data = "data";
  v.insert(v.end(), data, data + 4);
  PutU32(v, dataBytes);
  for (int16_t s : samples) PutS16(v, s);
  return v;
}

} // namespace

int main(int argc, char **argv)
{
  printf("== song_pcm ==\n");

  // -- the djb2 variant is what names the cache file, so it must match --------
  {
    // Hand-computed: 5381*33 + 'a' = 177573 + 97 = 177670; *33 + 'b' = 5863110+98.
    const uint8_t data[2] = {'a', 'b'};
    CheckEq((long long)phxplayer::SongCacheHash(data, 2), 5381LL * 33 * 33 + 97 * 33 + 98, "djb2 hash of \"ab\"");

    // MSVC's char is signed, so a byte >= 0x80 subtracts.  Getting this wrong
    // would look up the wrong cache file, so pin it.
    const uint8_t hi[1] = {0x80};
    CheckEq((long long)phxplayer::SongCacheHash(hi, 1), (long long)(5381ULL * 33ULL - 128LL), "djb2 hash treats char as signed (MSVC)");
    Check(phxplayer::SongCacheHash(data, 0) == 5381, "empty input hashes to the seed");
  }

  // -- the container round-trips exactly -------------------------------------
  const char *cachePath = "/tmp/phx_song_pcm_test.apxsndcache";
  {
    std::vector<int16_t> samples;
    for (int i = 0; i < 1000; i++) { samples.push_back((int16_t)(i * 7)); samples.push_back((int16_t)(-i * 3)); }
    const std::vector<uint32_t> markers = {0, 37, 74, 105};
    const std::vector<uint8_t> bytes = MakeSndCache("test synth config", markers, samples);
    Check(WriteFile(cachePath, bytes), "wrote a synthetic .apxsndcache");

    phxplayer::SongPcm pcm = phxplayer::LoadSongPcm(cachePath);
    Check(pcm.valid, "the synthetic cache is valid", pcm.error.c_str());
    Check(pcm.format == "apxsndcache", "format is reported as apxsndcache");
    Check(pcm.synthConfig == "test synth config", "synth config string round-trips");
    CheckEq((long long)pcm.markers.size(), 4, "all markers are read");
    CheckEq(pcm.markers[2], 74, "marker value round-trips");
    CheckEq((long long)pcm.samples.size(), (long long)samples.size(), "every sample is read");
    CheckEq(pcm.samples[3], samples[3], "sample values round-trip");
    CheckEq(pcm.sampleRate, 44100, "sample rate is the documented 44100");
    CheckEq(pcm.channels, 2, "channels is 2");
    // 1000 stereo frames at 44100 Hz.
    Check(fabs(pcm.DurationMs() - 1000.0 * 1000.0 / 44100.0) < 0.001, "duration derives from frames, not samples");
  }

  // -- a truncated payload degrades instead of reading past the end ----------
  {
    std::vector<int16_t> samples(100, 5);
    std::vector<uint8_t> bytes = MakeSndCache("x", {1, 2}, samples);
    bytes.resize(bytes.size() - 40); // cut the last 20 samples
    const char *path = "/tmp/phx_song_pcm_test_trunc.apxsndcache";
    Check(WriteFile(path, bytes), "wrote a truncated cache");
    phxplayer::SongPcm pcm = phxplayer::LoadSongPcm(path);
    Check(pcm.valid, "a truncated cache still loads what is there");
    Check(!pcm.error.empty(), "and explains the truncation");
    CheckEq((long long)pcm.samples.size(), 80, "only the surviving samples are exposed");
    remove(path);
  }

  // -- badly formed input is rejected, not misread ---------------------------
  {
    const char *path = "/tmp/phx_song_pcm_test_bad.bin";
    std::vector<uint8_t> junk = {'N', 'O', 'P', 'E', 1, 2, 3, 4, 5, 6, 7, 8};
    Check(WriteFile(path, junk), "wrote a file with an unknown magic");
    phxplayer::SongPcm pcm = phxplayer::LoadSongPcm(path);
    Check(!pcm.valid, "an unknown container is rejected");
    Check(pcm.error.find("unknown container") != std::string::npos, "and says so");
    remove(path);
  }

  // -- WAV, so a hand-rendered track can be dropped in ------------------------
  {
    const char *path = "/tmp/phx_song_pcm_test.wav";
    std::vector<int16_t> stereo;
    for (int i = 0; i < 500; i++) { stereo.push_back((int16_t)i); stereo.push_back((int16_t)(-i)); }
    Check(WriteFile(path, MakeWav(stereo, 2, 48000)), "wrote a stereo 48 kHz WAV");

    phxplayer::SongPcm pcm = phxplayer::LoadSongPcm(path);
    Check(pcm.valid, "the WAV loads", pcm.error.c_str());
    CheckEq(pcm.sampleRate, 48000, "the WAV sample rate is honoured");
    CheckEq((long long)pcm.samples.size(), 1000, "all WAV frames are read");
    CheckEq(pcm.samples[2], stereo[2], "WAV sample values round-trip");

    // Mono is duplicated to the stereo device rather than dropped.
    std::vector<int16_t> mono;
    for (int i = 0; i < 300; i++) mono.push_back((int16_t)(i * 11));
    const char *monoPath = "/tmp/phx_song_pcm_test_mono.wav";
    Check(WriteFile(monoPath, MakeWav(mono, 1, 22050)), "wrote a mono WAV");
    phxplayer::SongPcm m = phxplayer::LoadSongPcm(monoPath);
    Check(m.valid, "the mono WAV loads", m.error.c_str());
    CheckEq((long long)m.samples.size(), 600, "mono is widened to stereo");
    Check(m.samples[0] == mono[0] && m.samples[1] == mono[0], "both channels carry the mono sample");
    remove(monoPath);
    remove(path);
  }

  // -- discovery -------------------------------------------------------------
  {
    // Nothing next to a fictitious song: not an error, just nothing to play.
    std::string searched;
    phxplayer::SongPcm pcm = phxplayer::FindSongPcm("/tmp/phx_does_not_exist.mvm", &searched);
    Check(!pcm.valid, "no music next to a song that does not exist");
    Check(searched.find("apxsndcache") != std::string::npos, "the search covers the cache name");
  }

  // -- discovery finds a cache sitting next to the song ----------------------
  {
    const char *mvm = "/tmp/phx_song_pcm_test.mvm";
    std::vector<uint8_t> mvmBytes(256, 0x5a); // contents only matter for the hash
    Check(WriteFile(mvm, mvmBytes), "wrote a stand-in .mvm");

    // 1. <song>.apxsndcache, the direct form.
    std::vector<int16_t> samples(400, 1234);
    const char *direct = "/tmp/phx_song_pcm_test.mvm.apxsndcache";
    Check(WriteFile(direct, MakeSndCache("c", {1, 2, 3}, samples)), "wrote <song>.apxsndcache");
    std::string searched;
    phxplayer::SongPcm pcm = phxplayer::FindSongPcm(mvm, &searched);
    Check(pcm.valid, "discovery finds the cache next to the song", pcm.error.c_str());
    CheckEq((long long)pcm.samples.size(), 400, "and reads it correctly");
    remove(direct);

    // 2. SoundCache/<djb2>.apxsndcache, exactly where the Windows tool writes it.
    //    This also pins the %llx formatting: with MSVC's %I64x the name would be
    //    the literal "I64x.apxsndcache" and never match.
    char name[64];
    snprintf(name, sizeof(name), "/tmp/SoundCache/%llx.apxsndcache",
             (unsigned long long)phxplayer::SongCacheHash(mvmBytes.data(), mvmBytes.size()));
    std::string dir = name;
    dir = dir.substr(0, dir.find_last_of('/'));
    CheckEq(mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST, 1, "created the SoundCache directory");
    Check(WriteFile(name, MakeSndCache("c", {9}, samples)), "wrote SoundCache/<hash>.apxsndcache");

    // The player looks for the cache relative to the song's directory, so copy
    // the song there too and look it up by its bare name.
    const char *songInTmp = "/tmp/phx_song_pcm_test_dir.mvm";
    Check(WriteFile(songInTmp, mvmBytes), "wrote a second stand-in .mvm");
    char cacheInTmp[64];
    snprintf(cacheInTmp, sizeof(cacheInTmp), "/tmp/SoundCache/%llx.apxsndcache",
             (unsigned long long)phxplayer::SongCacheHash(mvmBytes.data(), mvmBytes.size()));
    searched.clear();
    phxplayer::SongPcm byHash = phxplayer::FindSongPcm(songInTmp, &searched);
    Check(byHash.valid, "discovery finds SoundCache/<hash>.apxsndcache", searched.c_str());
    remove(songInTmp);
    remove(name);

    remove(mvm);
  }

  // -- optionally, a real container produced by the Windows tool -------------
  if (argc > 1)
  {
    printf("-- real file: %s --\n", argv[1]);
    phxplayer::SongPcm pcm = phxplayer::LoadSongPcm(argv[1]);
    Check(pcm.valid, "the real cache loads", pcm.error.c_str());
    if (pcm.valid)
    {
      printf("     %zu samples, %u Hz, %d ch, %zu markers, %.2f s\n",
             pcm.samples.size(), pcm.sampleRate, pcm.channels, pcm.markers.size(), pcm.DurationMs() / 1000.0);
      Check(pcm.markers.size() > 0, "it carries a tick marker table");
      Check(pcm.DurationMs() > 1000.0, "it is more than a second long");
    }
  }

  remove(cachePath);
  printf("\n== %s (%d failure%s, %d checks) ==\n", failures ? "FAILED" : "PASSED",
         failures, failures == 1 ? "" : "s", checks);
  return failures ? 1 : 0;
}
