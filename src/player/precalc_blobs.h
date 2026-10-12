// src/player/precalc_blobs.h
//
// The loading-screen projects, embedded exactly as MinimalPlayer embeds them.

#pragma once

struct PrecalcBlob
{
  const char *name;
  const unsigned char *data;
  int size;
};

extern const PrecalcBlob kPrecalcBlobs[];
extern const int kPrecalcBlobCount;
