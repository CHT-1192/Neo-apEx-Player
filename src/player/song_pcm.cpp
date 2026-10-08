#include "song_pcm.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

namespace phxplayer
{

namespace
{

bool ReadWholeFile(const char *path, std::vector<uint8_t> *out)
{
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0)
  {
    fclose(f);
    return false;
  }
  out->resize((size_t)size);
  const size_t got = fread(out->data(), 1, (size_t)size, f);
  fclose(f);
  return got == (size_t)size;
}

uint32_t ReadU32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t ReadU16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

bool FileExists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Everything before the extension.
std::string StripExtension(const std::string &p)
{
  const size_t slash = p.find_last_of("/\\");
  const size_t dot = p.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return p;
  return p.substr(0, dot);
}

std::string DirectoryOf(const std::string &p)
{
  const size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

//////////////////////////////////////////////////////////////////////////////
// the SoundCache container

SongPcm ParseSndCache(const uint8_t *data, size_t size, const char *path)
{
  SongPcm pcm;
  pcm.source = path;
  pcm.format = "apxsndcache";

  size_t off = 0;
  auto need = [&](size_t n) { return off + n <= size; };

  if (!need(8)) { pcm.error = "truncated header"; return pcm; }
  // '!CSA' little-endian: the bytes are '!', 'C', 'S', 'A'.
  if (memcmp(data, "!CSA", 4) != 0) { pcm.error = "bad magic (expected !CSA)"; return pcm; }
  off += 4;

  const uint32_t metadataSize = ReadU32(data + off);
  off += 4;
  if (!need(metadataSize)) { pcm.error = "truncated metadata"; return pcm; }
  pcm.synthConfig.assign((const char *)data + off, metadataSize ? metadataSize - 1 : 0);
  off += metadataSize;

  if (!need(4)) { pcm.error = "truncated marker count"; return pcm; }
  const uint32_t numMarkers = ReadU32(data + off);
  off += 4;
  if (numMarkers > (size - off) / 4) { pcm.error = "marker count exceeds the file"; return pcm; }
  pcm.markers.reserve(numMarkers);
  for (uint32_t i = 0; i < numMarkers; i++, off += 4) pcm.markers.push_back(ReadU32(data + off));

  if (!need(4)) { pcm.error = "truncated sample size"; return pcm; }
  const uint32_t sampleDataSize = ReadU32(data + off);
  off += 4;

  // The header states a byte count; trust whichever is smaller so a truncated
  // file degrades into a shorter song instead of reading past the end.
  size_t bytes = sampleDataSize;
  if (bytes > size - off)
  {
    bytes = size - off;
    pcm.error = "sample data is shorter than the header claims (using what is there)";
  }
  bytes &= ~(size_t)1; // whole s16 samples only

  pcm.samples.resize(bytes / 2);
  memcpy(pcm.samples.data(), data + off, bytes);

  pcm.sampleRate = 44100;
  pcm.channels = 2;
  pcm.valid = !pcm.samples.empty();
  if (!pcm.valid && pcm.error.empty()) pcm.error = "no sample data";
  return pcm;
}

//////////////////////////////////////////////////////////////////////////////
// WAV, so a hand-rendered track can drop in next to the .mvm

SongPcm ParseWav(const uint8_t *data, size_t size, const char *path)
{
  SongPcm pcm;
  pcm.source = path;
  pcm.format = "wav";

  if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
  {
    pcm.error = "not a RIFF/WAVE file";
    return pcm;
  }

  uint16_t formatTag = 0, channels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;
  const uint8_t *payload = nullptr;
  size_t payloadSize = 0;

  size_t off = 12;
  while (off + 8 <= size)
  {
    const uint32_t chunkSize = ReadU32(data + off + 4);
    const uint8_t *body = data + off + 8;
    if (off + 8 + (size_t)chunkSize > size) break;

    if (memcmp(data + off, "fmt ", 4) == 0 && chunkSize >= 16)
    {
      formatTag = ReadU16(body);
      channels = ReadU16(body + 2);
      sampleRate = ReadU32(body + 4);
      bitsPerSample = ReadU16(body + 14);
    }
    else if (memcmp(data + off, "data", 4) == 0)
    {
      payload = body;
      payloadSize = chunkSize;
    }
    off += 8 + chunkSize + (chunkSize & 1); // chunks are word-aligned
  }

  if (!payload || !payloadSize) { pcm.error = "no data chunk"; return pcm; }
  if (formatTag != 1) { pcm.error = "only uncompressed PCM WAV is supported (format tag != 1)"; return pcm; }
  if (bitsPerSample != 16) { pcm.error = "only 16-bit WAV is supported"; return pcm; }
  if (channels < 1 || channels > 2) { pcm.error = "only mono or stereo WAV is supported"; return pcm; }

  // Normalise to the engine's interleaved stereo s16.
  const size_t frames = payloadSize / (2u * channels);
  pcm.samples.resize(frames * 2);
  const int16_t *src = (const int16_t *)payload;
  if (channels == 2)
    memcpy(pcm.samples.data(), src, frames * 2 * sizeof(int16_t));
  else
    for (size_t i = 0; i < frames; i++)
    {
      pcm.samples[i * 2 + 0] = src[i];
      pcm.samples[i * 2 + 1] = src[i];
    }

  pcm.sampleRate = sampleRate ? sampleRate : 44100;
  pcm.channels = 2;
  pcm.valid = frames > 0;
  if (!pcm.valid) pcm.error = "no frames";
  return pcm;
}

} // namespace

uint64_t SongCacheHash(const uint8_t *data, size_t size)
{
  // Exactly HashBuffer() in Audio/phxSongBase.cpp.  It accumulates into an
  // unsigned 64-bit value but reads into a `char` buffer, and MSVC's char is
  // signed, so bytes >= 0x80 subtract from the hash rather than adding.
  uint64_t hash = 5381;
  for (size_t i = 0; i < size; i++)
    hash = (hash << 5) + hash + (uint64_t)(int64_t)(int8_t)data[i];
  return hash;
}

uint64_t SongCacheHashFile(const char *path)
{
  std::vector<uint8_t> data;
  if (!ReadWholeFile(path, &data)) return 0;
  return SongCacheHash(data.data(), data.size());
}

SongPcm LoadSongPcm(const char *path)
{
  SongPcm pcm;
  if (!path)
  {
    pcm.error = "no path";
    return pcm;
  }

  std::vector<uint8_t> data;
  if (!ReadWholeFile(path, &data))
  {
    pcm.error = std::string("cannot read ") + path;
    return pcm;
  }
  if (data.size() >= 4 && memcmp(data.data(), "!CSA", 4) == 0)
    return ParseSndCache(data.data(), data.size(), path);
  if (data.size() >= 12 && memcmp(data.data(), "RIFF", 4) == 0)
    return ParseWav(data.data(), data.size(), path);

  pcm.error = std::string(path) + ": unknown container (expected !CSA or RIFF/WAVE)";
  return pcm;
}

SongPcm FindSongPcm(const char *mvmPath, std::string *searched)
{
  std::vector<std::string> candidates;
  const std::string mvm = mvmPath ? mvmPath : "";
  const std::string base = StripExtension(mvm);
  const std::string dir = DirectoryOf(mvm);

  if (!mvm.empty())
  {
    candidates.push_back(mvm + ".apxsndcache"); // the literal name the tool writes
    candidates.push_back(base + ".apxsndcache");
    candidates.push_back(base + ".wav");
    candidates.push_back(base + ".ogg");
    candidates.push_back(base + ".mp3");
  }

  // SoundCache/<djb2>.apxsndcache, the tool's own location.  It is relative to
  // the working directory there, but a file next to the song is the more useful
  // guess for a player, so try both.
  if (!mvm.empty())
  {
    const uint64_t hash = SongCacheHashFile(mvm.c_str());
    if (hash)
    {
      char name[64];
      // NOT "%I64x": that is an MSVC-only conversion and glibc/BSD printf print
      // it literally, producing "I64x.apxsndcache" and never finding the file.
      // The tool writes lowercase hex without padding.
      snprintf(name, sizeof(name), "%llx.apxsndcache", (unsigned long long)hash);
      candidates.push_back(dir + "SoundCache/" + name);
      candidates.push_back(std::string("SoundCache/") + name);
      candidates.push_back(dir + name);
    }
  }

  for (const std::string &c : candidates)
  {
    if (searched) *searched += c + " ";
    if (!FileExists(c.c_str())) continue;
    SongPcm pcm = LoadSongPcm(c.c_str());
    if (pcm.valid) return pcm;
    // A file that exists but does not parse is worth reporting rather than
    // silently skipping, because it is almost always the intended track.
    if (searched) *searched += "(" + pcm.error + ") ";
  }

  return SongPcm{};
}

} // namespace phxplayer
