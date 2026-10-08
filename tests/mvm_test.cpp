#include "player/song_clock.h"
#include <cstdio>
int main(int argc, char**argv){
  int fail=0;
  for(int i=1;i<argc;i++){
    auto m = phxplayer::ParseMvmHeader(argv[i]);
    if(!m.valid){ printf("FAIL %s: %s\n", argv[i], m.error.c_str()); fail=1; continue; }
    printf("%-30s bpm=%u tpb=%u machines=%u ticks=%u samples=%llu ms=%.1f (%.2f s) = %lld frames @120\n",
      argv[i], m.initialBpm, m.initialTpb, m.numMachines, m.numMasterTicks,
      (unsigned long long)m.totalSamples, m.totalMs, m.totalMs/1000.0,
      (long long)(m.totalMs/1000.0*120));
    unsigned changes=0; for(size_t k=0;k<m.tpbSeq.size();k++) if(m.tpbSeq[k]!=0xFF) changes++;
    printf("    tpb changes=%u, tickStartSamples[1..4]=%u,%u,%u,%u\n", changes,
      m.tickStartSamples.size()>1?m.tickStartSamples[1]:0,
      m.tickStartSamples.size()>2?m.tickStartSamples[2]:0,
      m.tickStartSamples.size()>3?m.tickStartSamples[3]:0,
      m.tickStartSamples.size()>4?m.tickStartSamples[4]:0);
  }
  return fail;
}
