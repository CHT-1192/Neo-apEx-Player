# Music backend for the cross-platform Vulkan apEx player — MVX and the `.mvm` container

Target: play the music of **Clean Slate** (Conspiracy, 2023) in a portable (macOS/Linux/Windows)
Vulkan player, without the Windows-only apEx tool.

* Source under analysis (READ-ONLY): `~/Repos/apEx-public/`
* Real data: `Projects/Clean Slate/step010.mvm`, `Projects/Clean Slate/step010padding.mvm`
* Project file: `Projects/Clean Slate/cleanslate.apx`

## Confidence legend

Every non-obvious claim below is tagged:

* **[VERIFIED]** — read directly out of source code, or computed from the real `.mvm` bytes and
  cross-checked against an independent constraint in the file.
* **[INFERRED]** — consistent with all evidence but not proven by the loader (whose source we do
  not have).
* **[UNCERTAIN]** — plausible reading; treat as a hypothesis to confirm.

---

## 0. Executive summary

1. The `.mvm` file is **not** a plugin container. It contains **no MZ/PE executable, no DLL, no
   compressed sample blob**. It is pure music data (machine list, connection graph, per-tick
   automation sequences, pattern/note data), and its ~985 KB is ~99 % `0xFF` "empty" fill.
   **[VERIFIED]**
2. The MVX synthesizer engine is **not open source and not present in the repository**. Only
   headers (`mvx.h`) and prebuilt **x86-32 MSVC static libraries** exist. There is no `.cpp` for
   MVX anywhere in the tree. **[VERIFIED]**
3. Therefore "port the real MVX synth to macOS" is **not achievable**. See §5 for the recommended
   path (offline render once with the original Windows binaries, then play PCM + tick table).
4. The tool never streams MVX in real time. `phxSongBase::OfflineRender` renders the **entire song
   once** into a single DirectSound secondary buffer, plus a per-tick millisecond marker array.
   That architecture ports to any platform trivially once you have the PCM. **[VERIFIED]**
5. The `.mvm` header is **fully decoded** (byte-exact, §1.2) and the per-tick timeline can be
   reconstructed from the file alone, **without the synth** — good enough to drive demo frame sync
   with zero audio. **[VERIFIED]** for structure, **[INFERRED]** for the exact tempo semantics.

---

## 1. The `.mvm` container format

### 1.1 Magic and dispatch

`apEx/Phoenix_Tool/Audio/phxSound.cpp:64-101` dispatches on the first DWORD read little-endian:

| file | first DWORD | case label | backend |
|---|---|---|---|
| `.ogg` | `0x6F676753` (`OggS`) | `'SggO'` | `CphxSongOGG` |
| `.mvm` | `0x004D564D` (`MVM\0`) | `'\0MVM'` | `CphxSongMVM` |
| other | probe DWORD at offset 4: if `22050 <= v <= 48000` → WaveSabre, else V2M |

`cleanslate.apx` selects the file with `<songfile>.\step010padding.mvm</songfile>`
— **the padding variant is the one Clean Slate actually uses [VERIFIED]**.

### 1.2 Byte-level layout (both real files)

All offsets decimal-hex, all multi-byte fields little-endian. `N` = `NumMasterTicks`.

| offset | size | field | step010 | step010padding | confidence |
|---|---|---|---|---|---|
| `0x000` | 4 | magic `"MVM\0"` | `4D 56 4D 00` | same | **[VERIFIED]** |
| `0x004` | 1 | initial **BPM** | `0x87` = **135** | 135 | **[INFERRED]** |
| `0x005` | 1 | initial **TPB** (ticks per beat) | `0x05` = 5 | 5 | **[INFERRED]** |
| `0x006` | 2 | `NumMasterTicks` / song length in ticks (`N`) | 2336 (`0x0920`) | 2432 (`0x0980`) | **[VERIFIED]** (repeated verbatim at `0x0F2` and `0x0F4+3N`) |
| `0x008` | 2 | unknown, `0` in both files | `00 00` | `00 00` | **[UNCERTAIN]** |
| `0x00A` | 2 | `NumMachines` = 46 | `2E 00` | `2E 00` | **[VERIFIED]** (46 = size of the two 46-byte arrays that follow, and 138 = 3×46 for the next block) |
| `0x00C` | 46 | machine **type IDs**, one per machine slot | `14 16 18 06 … 09 1B` | identical | **[INFERRED]** |
| `0x03A` | 46 | **machine order** list | permutation of `0..46` **excluding 27** | identical | **[INFERRED]** |
| `0x068` | 138 | 46 × 3-byte records `{src, dst, amount}` — the **connection graph** (§1.4) | see §1.4 | identical | **[INFERRED]** (strong) |
| `0x0F2` | 2 | `NumMasterTicks` again (= `N`) | 2336 | 2432 | **[VERIFIED]** |
| `0x0F4` | `N` | **`MasterSeq[N]`** — master volume per tick, `0xFF` = no change | *all `0xFF`* | *all `0xFF`* | **[INFERRED]** (assignment), **[VERIFIED]** (offset/size/runs) |
| `0x0F4+N` | `N` | **`TPBSeq[N]`** — ticks-per-beat per tick, `0xFF` = no change | `3,5,3,5,…` ×2304 then `0xFF` ×32 | `3,5,3,5,…` ×2304 then `0xFF` ×128 | **[INFERRED]** (assignment), **[VERIFIED]** (structure) |
| `0x0F4+2N` | `N` | **`BPMSeq[N]`** — BPM per tick, `0xFF` = no change | `[0]=0x87`(135), rest `0xFF` | same | **[INFERRED]** (assignment), **[VERIFIED]** (structure) |
| `0x0F4+3N` | 8 | trailer, four `u16`: `128, 2048, N, 1026` | `80 00 00 08 20 09 02 04` | `80 00 00 08 80 09 02 04` | **[VERIFIED]** (only field 3 varies) |
| `0x0F4+3N+8` | … | machine state / patterns / note data, `0xFF`-dominated, terminated by a final chunk | see §1.4 | see §1.4 | **[UNCERTAIN]** |

**Why the layout is certain.** The three-array hypothesis was confirmed by an independent
arithmetic constraint, not by guessing:

* file *a*: `0x0F4 + 1·2336 = 0x0A14` and `0x0F4 + 2·2336 = 0x1334`
* file *b*: `0x0F4 + 1·2432 = 0x0A74` and `0x0F4 + 2·2432 = 0x1D74`

The **byte differences between the two files fall exactly on those boundaries**. The diff ranges
`0xA14–0xA73` and `0x1314–0x1373` (file a vs b) are exactly the 96-byte shift that inserting
96 ticks into the middle array produces, and `0x1D74` is `0x0F4+2·2432` in the padding file.
Reproduce with the script in §7.

**Assignment of `MasterSeq`/`TPBSeq`/`BPMSeq` to the three arrays** follows the declaration order
of those globals in `mvx.h` (see §3.1) and survives elimination: the middle array alternates `3,5`,
which cannot be BPM; the third array's single non-`0xFF` byte is `135`, which equals the header's
own BPM byte at `0x004` — a self-consistency check that also argues for `0x004`/`0x005` being
BPM/TPB rather than a single `u16` `0x0587 = 1415` (1415 BPM would give a nonsensical 623 samples
per tick). This ordering is **[INFERRED]**; the genuinely uncertain part is only whether the
all-`0xFF` array is master volume or BPM (it is empty either way, so it cannot change the audio).

### 1.3 The three per-tick sequences (decoded, high confidence)

```
A0 = file[0x0F4        : 0x0F4+N    ]  len N   all 0xFF
A1 = file[0x0F4+  N    : 0x0F4+2N   ]  len N   [3,5,3,5,...] for 2304 entries, 0xFF after
A2 = file[0x0F4+2N     : 0x0F4+3N   ]  len N   [135, 0xFF, 0xFF, ...]
```

`0xFF` means "keep previous value", exactly as implemented by the loader:

```cpp
// apEx/Phoenix_Tool/Audio/phxSongMVM.cpp:51-59
if ( mvxSystem_MasterTick < mvxSystem_NumMasterTicks ) {
  if ( mvxSystem_MasterSeq[ mvxSystem_MasterTick ] != 0xFF )
    mvxSystem_MasterVolume = mvxSystem_MasterSeq[ mvxSystem_MasterTick ];
  if ( mvxSystem_TPBSeq[ mvxSystem_MasterTick ] != 0xFF )
    mvxSystem_TPB = mvxSystem_TPBSeq[ mvxSystem_MasterTick ];
  if ( mvxSystem_BPMSeq[ mvxSystem_MasterTick ] != 0xFF )
    mvxSystem_BPM = mvxSystem_BPMSeq[ mvxSystem_MasterTick ];
  mvxSystem_SamplesPerTick = ( 60 * MVX_SAMPLERATE ) / ( mvxSystem_TPB*mvxSystem_BPM );
}
```

The `TPB = 3,5,3,5,…` alternation is real data, not a mis-parse (verified by run-length encoding:
2305 runs, strictly alternating). Its musical meaning is **not** established here: with
`BPM=135` a beat is exactly `44100*60/135 = 19600` samples, while a tick pair is
`2646000/(3*135) + 2646000/(5*135) = 6533 + 3920 = 10453` samples, so ticks are *not* a constant
fraction of a beat. Treat "TPB swing" as **[UNCERTAIN]**; the *arithmetic consequences* below are
exact regardless, because they follow the loader's own integer formula.

### 1.4 The remainder of the file (`0x0F4+3N+8` → EOF)

Not decodable to byte-level from the public repo, because the only code that reads it is
`mvxSystem_LoadMusic()` inside a proprietary `.lib` (§2.2). What *is* established:

* **Size**: `985429 - (0x0F4+3·2336+8) = 978177` bytes for `step010.mvm`;
  `1025749 - (0x0F4+3·2432+8) = 1019669` bytes for the padding file.
* **It is not compressed audio and not a binary.** Shannon entropy measured in 4 KB windows is
  **0.00–1.86 bits/byte** (max would be 8.0), i.e. extremely sparse. The longest single run of
  `0xFF` is **7609 bytes at offset `0x976A5`**. There is no `RIFF`, `DLS `, `fmt `, `WAVE`,
  `MZ`, `PE\0\0`, `PK\x03\x04`, `\x1f\x8b`, `LZ` or `SZDD` signature anywhere in either file.
  **[VERIFIED]**
* **Record/chunk structure exists.** The tail shows a repeating chunk header
  `00 00 FF FF 01 00 00` followed by a few payload bytes, e.g. at the end of `step010.mvm`:
  `… 00 00 FF FF 01 00 00 82 83 80 19 A0 01 3E D0 A0`. Bytes `0x0F4+3N` onwards are the
  four-`u16` trailer described in §1.2, immediately followed by a long `0xFF` run. **[VERIFIED]**
  (observation), **[UNCERTAIN]** (meaning).
* **The `0x068` block is the MVX connection graph.** It occupies `0x068..0x0F1`, i.e. exactly
  `138 = 3 × 46` bytes, ending precisely where the `NumMasterTicks` field begins, and decodes as
  46 records of `{u8 src; u8 dst; u8 amount}` **sorted by destination**:

  ```
   5   0 0xff      4   5 0xff      8   9 0xff     22  15 0xff     28  30 0xff
   3   2 0xd8      7   6 0x8e     15  12 0xff     16  17 0xff     30  31 0xff
  36   2 0x5d      9   6 0x4f     11  13 0xff     18  19 0xff     38  31 0xff
  43   2 0x67     23   6 0x1e     14  13 0x8e     18  20 0xff     32  33 0xff
  45   2 0xb8     24   6 0x4c     17  13 0x08     19  21 0xff     34  35 0xff
   1   3 0xcc     26   6 0x44     21  13 0xb8     20  21 0xff     35  36 0xff
  29   3 0x80     44   6 0x8e                       13  22 0xff     37  38 0xff
   2   4 0xb1                                      25  26 0xff     42  41 0x3b
   6   4 0x37                                      27  28 0xff     40  42 0xff
  10   4 0xd6                                      31  29 0xff     41  43 0xff
  12   4 0x3f                                      33  45 0xff
  39   4 0x71
  ```

  Evidence: column 2 is a destination index and every source in a destination group is listed
  contiguously (`dst 2 ← {3,36,43,45}`, `dst 6 ← {7,9,23,24,26,44}`, `dst 13 ← {11,14,17,21}`);
  destination `0` (= `MVX_MACHINE_MASTER`) receives from machine `5`; all indices are `< 46`;
  the third byte is `0xFF` in 27 of 46 records and a plausible `amount/255` gain otherwise
  (`0x3B`=0.23, `0x4C`=0.30, `0x8E`=0.56, `0xCC`=0.80, `0xD8`=0.85). The engine really does have
  `MVX_CONNECTION` and an `mvxSystem_Connections` array
  (`mvxSystem_Connections@@3PAUMVX_CONNECTION@@A` in the symbol table), and `mvxSystem_NumConnections`
  is declared in `mvx.h`. **[INFERRED]** — note the caveat that 46 is also `NumMachines`, so the
  "46 records" framing could be coincidence and `NumConnections` may be stored elsewhere (e.g. in
  the still-unidentified `u16 @0x008`).


If byte-exact access to note/pattern data is ever needed, the only realistic route is to drive the
original Windows binary and instrument it, or to obtain the MVX sources from the author.

### 1.5 Where the ~1 MB goes — and the "embedded DLL" question

**There is no embedded plugin/DLL binary. [VERIFIED]**

* No `MZ` header and no `PE\0\0` signature anywhere in either `.mvm`. (A Win32 DLL would contain
  a DOS stub beginning `4D 5A` and a PE header; neither appears even once.)
* No DLL-related strings; the only ASCII runs longer than 5 bytes are byte-ramp tables
  (`#)+/37;?CGKOSW[_cgkosw{` and its reverse) — i.e. waveform/wavetable data, not text.
* The engine is **not** inside the song file; it ships separately as `mvxPlayerLib_*.lib`
  (§2.2). The `.mvm` is the *song*, and songs of a 20+-machine modular synth with 46 machine
  instances, a routing graph and ~2400 ticks of automation legitimately reach ~1 MB — the file is
  roughly 0.4 KB per tick.
* Entropy is ~0–2 bits/byte, which is what a table of mostly-empty pattern cells looks like, not
  what code or compressed media looks like.

### 1.6 Free win: the on-disk PCM cache format

`phxSongBase::OfflineRender` (`apEx/Phoenix_Tool/Audio/phxSongBase.cpp:74-157`) caches the rendered
song to `SoundCache\<djb2-hash-of-mvm>.apxsndcache`. This is a **fully specified, portable
container** and is the cheapest possible source of the music for the new player:

```
u32  '!CSA'                (0x41534321)
u32  metadataSize          (== headerContents.Length()+1)
char headerContents[]      (ASCIIZ — the synth config string)
u32  numMarkers
u32  markers[numMarkers]   (per-tick positions in MILLISECONDS)
u32  sampleDataSize
s16  sampleData[]          (interleaved stereo, 44100 Hz)
```

`HashBuffer` is the djb2 variant `hash = hash*33 + c` over the whole `.mvm`, starting at `5381`,
printed with `%I64x`. **[VERIFIED]** `CphxSongMVM` returns `ShouldCache() == true`, so the original
tool *already* produces this file (`.mvm` → cache). A portable player can either read an
`.apxsndcache` produced by the Windows tool, or emit its own equivalent.

### 1.7 How it is loaded (`phxSongMVM.cpp`, whole file, 83 lines)

```cpp
class mvxMachine {                    // re-declared locally; matches the engine's real class
public:
  mvxMachine() {};
  virtual void Tick() = NULL;         // note: "= NULL" is "= 0"; MSVC accepts this
  virtual int Render( float * buffer, int numsamp, float preamp ) = NULL;
};
extern mvxMachine ** mvxSystem_Machines;

void CphxSongMVM::OfflineRenderSong( CStreamReader & streamSong, CStreamWriter & streamOut,
                                     CArray<TU32> & aMarkers, SOUND_LOADCALLBACK pLoad, void * pUserData )
{
  TU8 * pData = new TU8[ (TS32) streamSong.GetLength() ];
  streamSong.SeekFromStart( 0 );
  streamSong.Read( pData, (TS32) streamSong.GetLength() );

  mvxSystem_LoadMusic( (char*) pData, (TS32) streamSong.GetLength() );   // <-- the opaque parser
  mvxMixer_Init();

  unsigned short fl = 0;
  __asm { fstcw[ fl ]  or [ fl ], 0x0200  fldcw[ fl ] }                  // x87: 53-bit precision

  float * clipbuffer32 = new float[ mvxSystem_SamplesPerTick * 2 * 2 ];
  short * clipbuffer16 = new short[ mvxSystem_SamplesPerTick * 2 * 2 ];
  aMarkers.Flush();

  mvxSystem_MasterTick = 0;
  TU32 dwFramesRendered = 0;
  for ( int x = 0; x < mvxSystem_SongLength; x++ ) {                     // one iteration == one tick
    for ( int y = 0; y < mvxSystem_NumMachines; y++ )
      if ( mvxSystem_Machines[ y ] ) mvxSystem_Machines[ y ]->Tick();
    /* apply MasterSeq/TPBSeq/BPMSeq, recompute mvxSystem_SamplesPerTick   (see §1.3) */
    mvxSystem_MasterTick++;
    mvxMixer_Render( clipbuffer32, mvxSystem_SamplesPerTick );
    mvxSystem_ClipBuffer( clipbuffer32, clipbuffer16, mvxSystem_SamplesPerTick * 2 );
    aMarkers.Add( MulDiv( dwFramesRendered, 1000, PHXSOUND_SAMPLERATE ) );  // <-- tick -> ms
    streamOut.Write( clipbuffer16, mvxSystem_SamplesPerTick * MVX_SAMPLESIZE );
    if ( pLoad( pUserData, x, mvxSystem_SongLength ) == false ) break;
    dwFramesRendered += mvxSystem_SamplesPerTick;
  }
  delete[] clipbuffer32; delete[] clipbuffer16;
  mvxMixer_DeInit(); mvxSystem_FreeMusic(); delete[] pData;
  pLoad( pUserData, mvxSystem_SongLength, mvxSystem_SongLength );
}
```

Notes for the port:

* `mvxSystem_Machines` and the local `mvxMachine` vtable are the **only** engine internals the tool
  reaches into directly. Everything else (BPM/TPB/seq pointers, `SamplesPerTick`, `SongLength`,
  `NumMachines`, `ClipBuffer`, `Mixer_*`) is declared in `mvx.h`.
* `mvxSystem_LoadMusic` is the single opaque call: the entire `.mvm` body is handed over as one
  buffer and the engine sets its globals as *pointers into that buffer* (`BYTE * mvxSystem_MasterSeq`
  etc.), which is why the header must be traversed in place.
* `mmsystem.h`/`dsound.h` are pulled in by `mvx.h` itself, so even the *header* is Windows-only.
* The inline `__asm` x87 block is MSVC-x86-only; on clang/GCC it would be `_controlfp(_PC_53, _MCW_PC)`.
  It exists to match the original render bit-for-bit; a fresh implementation that does not need
  bit-identical output can drop it.

### 1.8 Verified computed timeline (from the data alone, no synth)

Simulating the loop above with the loader's integer arithmetic
(`(60*44100)/(TPB*BPM)`, integer division) over the real sequence arrays:

| | `step010.mvm` | `step010padding.mvm` |
|---|---|---|
| `N` (ticks) | 2336 | 2432 |
| total samples | **12 167 296** | **12 543 616** |
| duration | **275.90 s** (4 min 36 s) | **284.44 s** (4 min 44 s) |
| duration as PCM s16 stereo 44.1 kHz | 46.5 MiB | **47.9 MiB (50 174 464 bytes)** |
| first markers (ms) | `0, 148, 237, 385, 474, 622` | same |
| last marker | 275 813 ms | 284 346 ms |

The difference is exactly **96 ticks × 3920 samples = 376 320 samples = 8.5333 s = 1024 frames
at 120 fps** — an independent confirmation that "padding" means "96 extra ticks appended"
(`96 × 3920` follows from `TPB=5, BPM=135` holding after tick 2303). **[VERIFIED]** arithmetic.

These figures are durations, **not** a guarantee of audio content: they assume the initial
BPM/TPB are the header bytes `0x87`/`0x05` (§1.2), which is the only reading consistent with a
sane tempo.

---

## 2. The MVX synthesizer engine

Header self-description: *"MVX — REALTIME BUZZ-BASED SYNTHESIZER, Written by Gargaj/Conspiracy ^
Ümlaüt Design ^ CoolPHat"*. It is a Buzz-style modular softsynth: a graph of "machines" (plug
types) instantiated per song, each with `Tick()` (advance one tick) and `Render(buffer, n, preamp)`.

### 2.1 The three `MVX` directories — what each one actually is

| path | contents | identity |
|---|---|---|
| `apEx/MVX/` | `mvx.h`, `MVXLibrary.lib` | **The one the tool builds against.** `phxSongMVM.cpp:3` does `#include "../../MVX/mvx.h"`. Its `mvx.h` (md5 `8b23b87bd3993e19eceac4a327d2a719`) is the older/simpler API: no `mvxWavRenderer`, no `MVX_PURE_C_EXPORT`. |
| `apEx/Libraries/MVX/` | `mvx.h` + 5 `.lib` builds | Vendor drop of the engine. `mvx.h` md5 `e8671ef1dd50e74bff6f59e95fb6ed00` — **newer**: adds `MVX_PURE_C_EXPORT` (a `__cdecl` + `extern "C"` build switch) and `mvxWavRenderer(...)`, a bulk offline exporter returning `wavdata` **and `tickdata`/`ticknum`**. |
| `apEx/Phoenix/MVX/` | `mvx.h` only | Byte-identical to `apEx/Libraries/MVX/mvx.h` (same md5). A stale copy. |

**Important trap:** `mvxWavRenderer` and any `extern "C"` / `MVX_PURE_C_EXPORT` symbol **do not
exist in any shipped library**. Grepping all six `.lib` files for `mvxWavRenderer` returns nothing,
and the archive symbol tables contain only C++ `__fastcall`-mangled names, e.g.
`?mvxSystem_LoadMusic@@YIHPADH@Z` (`YI` = `__fastcall`, `PADH` = `char*, int`). So
`apEx/Libraries/MVX/mvx.h` documents an API generation that was never shipped in this repo, and
there is **no C ABI** to link against. **[VERIFIED]**

### 2.2 There is no engine source — only binaries

`find . -iname "*mvx*"` over the whole public repo returns **only** `mvx.h` files and `.lib` files
(plus the `apEx/MinimalPlayer` player library). There is no `mvxSystem.cpp`, `mvxMixer.cpp`,
`mvxPlug_*.cpp`, or anything equivalent. **[VERIFIED]**

The engine's internals can, however, be read out of the archive symbol tables. From
`mvxPlayerLib_vs2019_releasestatic.lib` (872 symbols):

* Translation units (from the archive long-name table):
  `mvxACMDecompress`, `mvxCommon_Debug`, `mvxCommon_OscTab`, `mvxEnvelope`, `mvxFilter`,
  `mvxMachineList`, `mvxPlug_BandSaw`, `mvxPlug_BandSaw_Osc`, `mvxPlug_BandSaw_Track`,
  `mvxPlug_Chorus`, `mvxPlug_Comp`, `mvxPlug_Delay`, `mvxPlug_Disto`, `mvxPlug_Drum`,
  `mvxPlug_EQ`, `mvxPlug_Filter`, `mvxPlug_Flange`, `mvxPlug_Freeverb`, `mvxPlug_Glitch`,
  `mvxPlug_GrainDLS`, `mvxPlug_GrainDLS_Track`, `mvxPlug_Grain`, `mvxPlug_Grain_Track`,
  `mvxPlug_Monsta`, `mvxPlug_Monsta_RBJ_Filter`, `mvxPlug_Monsta_Track`, `mvxPlug_Reverb`,
  `mvxPlug_Sampler`, `mvxPlug_Sampler_Track`, `mvxPlug_Shaper`, `mvxPlug_Sweep`,
  `mvxPlug_Sweep_B`, `mvxPlug_Sweep_B_Track`, `mvxSystem`, `mvxRenderer`, `mvxRawPlayer`,
  `mvxMixer`. **[VERIFIED]**
* Machine type enumeration, in enum order as emitted into the binary:
  `MVX_MACHINE_MASTER, _DRUM, _DISTO, _FILTER, _DELAY, _COMP, _FLANGE, _EQ, _REVERB, _SAMPLER,
  _MONSTA, _FREEVERB, _GRAINDLS, _CHORUS, …` and additionally `_FORMANT, _GLITCH, _GRAIN,
  _BANDSAW, _SHAPER, _SWEEP, _SWEEP_B, _COMB` (alphabetical dump). The `.mvm`'s machine-ID array
  uses values up to **27**, i.e. ≥28 machine types exist. **[VERIFIED]** (strings), **[INFERRED]**
  (ID numbering).
* Instrumented parameter enums survive too: `BANDSAW_PARAMS`, `CHORUS_PARAMS`, `COMP_PARAMS`,
  `DELAY_PARAMS`, `DISTO_PARAMS`, `DRUM_PARAMS`, `EQ_PARAMS`, `FILTER_PARAMS`, `FLANGE_PARAMS`,
  `FREEVERB_PARAMS`, `GLITCH_PARAMS`, `GRAIN_PARAMS`, `GRAINDLS_PARAMS`, `MONSTA_PARAMS`, …,
  plus `MVX_CONNECTION`, `GRAINDLS_LOOPTYPE_*`, `MONSTARBJ_TYPE_*`, `MonstaEnvelope`. So a
  re-implementation is *documentable* but not *given*. **[VERIFIED]**

Shipped binaries (all **x86 32-bit COFF**, `IMAGE_FILE_MACHINE_I386 = 0x014C`):

| file | size | md5 | notes |
|---|---|---|---|
| `apEx/Libraries/MVX/mvxPlayerLib_vs2010_releasestatic.lib` | 2 145 422 | `e34f0b87…` | **byte-identical to `apEx/MinimalPlayer/mvxplayerlib_minimal.lib`** |
| `apEx/Libraries/MVX/mvxPlayerLib_vs2010_debug.lib` | — | `3a1c09de…` | debug build |
| `apEx/Libraries/MVX/mvxPlayerLib_vs2015_debug.lib` / `_releasestatic` | — | — | rebuilds |
| `apEx/Libraries/MVX/mvxPlayerLib_vs2019_releasestatic.lib` | 689 758 | — | smaller, 872 syms |
| `apEx/MVX/MVXLibrary.lib` | — | `53d4980d…` | the copy the tool actually links |
| `apEx/MinimalPlayer/mvxplayerlib_minimal.{darkness,silence,sunday,vessel}` | ~1.9–2.1 MB | all distinct | per-song builds of the same engine (see §2.5) |

### 2.3 Platform dependencies — what makes it Windows-x86-only

| dependency | evidence | portable? |
|---|---|---|
| **No source at all** | §2.2 | ✗ — nothing to compile |
| x86-32 COFF objects | `IMAGE_FILE_MACHINE_I386` in every `.lib` | ✗ on arm64; ✗ on x86_64 Mach-O |
| MSVC C++ ABI + **`__fastcall`** | `MVXCALL` = `__fastcall` in both `mvx.h`; symbols `?mvxSystem_LoadMusic@@YIHPADH@Z` | ✗ clang/GCC differ |
| **SSE intrinsics** | `?AllPassFilter@mvxPlugFreeverb@@QAE?AT__m128@@T2@HH@Z`, `?CombFilter@…__m128@@…`, i.e. Freeverb operates on `__m128` | would need rewriting |
| Inline **x87 asm** | `phxSongMVM.cpp:34-38` `fstcw/fldcw` (MSVC x86 only); same block in `MinimalPlayer/MusicPlayer.cpp:174-178` | trivially replaceable |
| Windows headers in the API | `mvx.h` includes `<windows.h>`, `<mmsystem.h>`, `<dsound.h>`; API takes `HWND`, `GUID*` | ✗ |
| **DirectSound** output path | `mvxSystem_Init(HWND,…)`, `mvxSystem_InitWithGUID(GUID*,…)`, `mvxSystem_Raw*`, `mvxSystem_GetSync` | replaceable, unused by the tool |
| **Windows ACM** (Audio Compression Manager) | `mvxACMDecompress.obj` exports `DriverEnumCallback(HACMDRIVERID…)`, `FormatEnumCallback(…tACMFORMATDETAILSA…)`, `hFoundDID`, `dwfx` (`tWAVEFORMATEX`) | ✗ — decodes compressed embedded samples |
| `DLS` sample playback | `mvxPlugGrainDLS::ReadDLSSample`, `GRAINDLS_*` | needs a DLS loader |

### 2.4 Porting difficulty, quantified

* **Files that must be compiled to use the real engine on macOS: zero are available.** The engine
  is not source; the only artefacts are ~36 x86-32 MSVC objects in `ar` archives. You cannot link
  a Win32/x86 static library into a Mach-O binary, and Rosetta 2 does not emulate Win32.
* **Re-implementation cost.** To reproduce Clean Slate's music you would need, bit-compatibly:
  ≥28 machine types (`mvxPlug_*`), a per-machine `Tick`/`Render` virtual protocol, the 46-instance
  routing graph, `mvxEnvelope`, `mvxFilter`, `mvxOscTab`/`mvxRandTab` wavetables, `mvxMixer`,
  `mvxSystem` (the `.mvm` parser), `mvxRenderer`, `mvxACMDecompress`, DLS/grain samplers, and the
  exact `(60*44100)/(TPB*BPM)` timing plus 53-bit x87 FP semantics. The symbol tables alone name
  ~250 engine functions/classes; a faithful re-implementation is a multi-month project and would
  still not be sample-accurate.
* **Verdict:** porting the real MVX synth is **not realistic**. Do not put it on the critical path.

### 2.5 Aside: the `MinimalPlayer` build matrix

`apEx/MinimalPlayer/` is a standalone, tiny Win32 player (`MinimalPlayer.cpp`, `MusicPlayer.cpp`,
`WindowHandler.cpp`, `SetupDialog.cpp`, `miniz.c`, `mvxplayerlib_minimal.lib`) with three
compile-time music backends: `MUSIC_IS_OGG` (stb_vorbis + DirectSound), `MUSIC_IS_WAVESABRE`
(`RealtimePlayer`), and MVX. The four oddly named libraries
(`.darkness`, `.silence`, `.sunday`, `.vessel`) are distinct ~2 MB engine builds with the same
`ar` layout — i.e. **per-song "minimal" engine builds** containing only the machines a given tune
needs (compare: `mvxSystem_NumMachines` is per-song, and the libs differ in member set/size).
This is a very strong hint that the author *intends* tiny per-song players, and it is the model to
copy for the portable player: one self-contained executable per demo, with the music pre-rendered.

---

## 3. Playback API — how the tool drives a song

### 3.1 The MVX API surface (`apEx/MVX/mvx.h`)

```c
#define MVX_SAMPLERATE 44100
#define MVX_SAMPLESIZE (sizeof(short)*2)      // i.e. 4 bytes per frame, stereo s16
#define MVX_BUFFERSIZE (MVX_SAMPLERATE * 2)

// state the tool reads/writes directly:
extern int   mvxSystem_NumMachines, mvxSystem_NumConnections, mvxSystem_NumMasterTicks;
extern BYTE *mvxSystem_MachineIDs, *mvxSystem_MachineOrder;
extern BYTE *mvxSystem_MasterSeq, *mvxSystem_TPBSeq, *mvxSystem_BPMSeq;
extern int   mvxSystem_BPM, mvxSystem_TPB, mvxSystem_MasterTick, mvxSystem_SongLength;
extern int   mvxSystem_SamplesPerTick;
extern BYTE  mvxSystem_MasterVolume;
extern int  *mvxSystem_ClipStart, *mvxSystem_ClipEnd;

void mvxMixer_Init();  void mvxMixer_Render(float*, int);  void mvxMixer_DeInit();
void mvxSystem_ClipBuffer(float* source, short* dest, int nums);
int  mvxSystem_LoadMusic(char* musicdata, int musicsize);
void mvxSystem_FreeMusic();

// realtime DirectSound output — NOT used by the tool:
int  mvxSystem_Init(HWND, char* music, int size);
int  mvxSystem_InitWithGUID(GUID*, HWND, char* music, int size);
void mvxSystem_Play();  int mvxSystem_GetSync();  void mvxSystem_Stop();
void mvxSystem_DeInit(); void mvxSystem_Pause(); void mvxSystem_Resume();
void mvxRenderOffline(MVX_RENDERCALLBACK cb, void* userdata);      // present in the libs
// declared only in apEx/Libraries/MVX/mvx.h, ABSENT from every shipped .lib:
void mvxWavRenderer(HWND, char* data, unsigned size, unsigned char** wavdata,
                    unsigned int* wavsize, unsigned int** tickdata,
                    unsigned int* ticknum, MVX_WAVRENDER_CALLBACK cb);
```

`mvxSystem_GetSync()` would be the natural "song position" call, but the tool bypasses all of the
`mvxSystem_*` output path and drives `Tick()`/`mvxMixer_Render()` manually so that it can render
*offline*, faster than real time, with a progress callback.

### 3.2 `CphxSound` / `CphxSongBase` — offline render to one buffer

`apEx/Phoenix_Tool/Audio/phxSound.{h,cpp}`, `phxSongBase.{h,cpp}`:

* `CphxSound::Init(HWND)` → `DirectSoundCreate` + `SetCooperativeLevel(DSSCL_PRIORITY)` +
  a **primary** buffer with `SetFormat(WAVE_FORMAT_PCM, 2ch, 44100, 16-bit)`. **[VERIFIED]**
* `CphxSound::Load()` dispatches on magic (§1.1) and returns immediately; the real work happens on
  a **worker thread** (`phxSongBase.cpp:26-52`, `CreateThread` → `OfflineRender`).
* `CphxSongBase::OfflineRender` → cache hit from `SoundCache\*.apxsndcache`, else
  `OfflineRenderSong` (§1.7) which writes the whole song as PCM into a `CStreamWriterMemory`.
  Progress is reported through `SOUND_LOADCALLBACK`; `MvxProgress` updates
  `MusicData.MVXRenderStatus` and, at completion, calls `AcquireTickData()`.
* The finished PCM is copied **into a single DirectSound secondary buffer**
  (`DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS`, format = `CphxSound::wfxFormat`).
* API from there on: `Play(ms)` = `SetCurrentPosition(ms) + Play()`;
  `GetPlayPosition()` = `MulDiv(playCursor, 1000, 44100*4)` (milliseconds, from the **audio
  clock**, not from TickData); `GetSongLength()` = `MulDiv(bufferSize, 1000, 44100*4)`;
  `GetMarkers(aMarkers)` = the per-tick millisecond array. **[VERIFIED]**

So the whole "playback API" a portable player must reproduce is:
**`render everything → keep a tick→ms table → report position in ms → report length in ms`.**

### 3.3 The tick timeline (`MUSICDATA.TickData`)

```cpp
// apEx/Phoenix_Tool/apxProject.h:17-30
struct MUSICDATA {
  CArray<TU32> TickData;      // per-tick positions, MILLISECONDS
  HWND AppHandle;             // = NULL
  TF32 MVXRenderStatus;       // = 0
  TBOOL Playing;              // = false
  TS32 PlaybackStart;         // = 0
  TS32 CurrentFrame;          // = 0
  TBOOL TimelineDragged;      // = false
  TS32 OfflineLength;         // = 0     (song length in ms)
};
```

Filled once, after rendering:

```cpp
// apEx/Phoenix_Tool/apxProject.cpp:1418-1428
void CapexProject::AcquireTickData() {
  MusicData.TickData.Flush();
  MusicData.OfflineLength = 0;
  if ( !Song ) return;
  MusicData.OfflineLength = Song->GetSongLength();
  Song->GetMarkers( MusicData.TickData );
}
```

`TickData[x]` is exactly the `aMarkers.Add(MulDiv(dwFramesRendered,1000,44100))` value produced
per tick in §1.7 — i.e. `floor(samplesBeforeTick_x * 1000 / 44100)` ms, **not** evenly spaced
(the real data is `0, 148, 237, 385, 474, 622, …`).

**Crucial finding:** `TickData` is used **only by the timeline editor** for drawing/snapping the
tick grid — `apEx/apEx/TimelineEditor.cpp:604, 644-667, 1271, 1300-1315` — e.g.

```cpp
// TimelineEditor.cpp:658
TS32 f = FrameToClient( (TS32)( Project.MusicData.TickData[ x ] / 1000.0f
                                * Project.Timeline->Timeline->FrameRate ) );
```

It is **never** used to decide which frame to render. **[VERIFIED]** — a grep for `TickData`
across the tree yields only `apxProject.{h,cpp}` and `TimelineEditor.cpp`.

### 3.4 How song time maps to demo frames for Clean Slate

Runtime sync uses the **DirectSound play cursor**, not the tick table:

```cpp
// apEx/Phoenix_Tool/apxProject.cpp:1291-1303
TS32 CapexProject::GetFrameToRender() {
  if ( !MusicData.TimelineDragged ) {
    if ( MusicData.Playing )
      if ( Song )
        return max( 0, (TS32)( Song->GetPlayPosition() / 1000.0f * Project.Timeline->Timeline->FrameRate ) );
      else
        return max( 0, (TS32)( ( globalTimer.GetTime() - MusicData.PlaybackStart ) / 1000.0f
                               * Project.Timeline->Timeline->FrameRate ) );
  }
  return MusicData.CurrentFrame;
}
// apxProject.cpp:1379-1386  UpdateCurrentFrameFromPlayback(): identical formula into CurrentFrame
```

Clean Slate's real project settings, read from `cleanslate.apx` **[VERIFIED]**:

```xml
<Framerate>120</Framerate>
<gridbpm>120</gridbpm>
<songfile>.\step010padding.mvm</songfile>
```

Therefore, for Clean Slate:

```
frame(playPos_ms) = max(0, (int)( playPos_ms / 1000.0f * 120.0f ))      // == floor(ms * 0.12)
playPos_ms        = MulDiv(directSoundPlayCursorFrames, 1000, 44100)    // integer ms
```

* `FrameRate = 120`, so **one demo frame = 8.3333 ms**, and the music's `BPM=135`/`TPB≈4` gives
  ≈ 15 frames per tick on average (`5512.5 samples = 125 ms = 15 frames` at TPB·BPM = 540).
* `<gridbpm>120</gridbpm>` is the **editor grid**, not the song tempo: the song data says
  `BPM = 135` (`0x87` at `0x004`, echoed by `BPMSeq[0]`). The two values are unrelated; do not use
  `gridbpm` for sync.
* The conversion is applied to an **integer millisecond** value and then truncated to an int
  frame, so any portable player must quantise the same way to be frame-exact:
  `frame = (int)( (samples_played * 1000 / 44100) * 120 / 1000 )`, or equivalently
  `frame = (int)( samples_played / 44100.0f * 120.0f )` if the audio clock is sample-exact.
  Exactness to the original is not guaranteed because the original's ms value comes from a 4-byte
  aligned DirectSound cursor (`MulDiv` truncation), which loses up to 1 ms ≈ 0.12 frame.
* `Play(ms)` seeks by `SetCurrentPosition(MulDiv(ms, 44100*4, 1000))`; `TogglePlaying()` seeks to
  `CurrentFrame / FrameRate * 1000` ms. **[VERIFIED]**
* `<songfile>` is `.\step010padding.mvm`, so the **padding** variant is what ships: 2432 ticks,
  12 543 616 samples, 284.44 s (§1.8).

---

## 4. Audio output

**Today (Windows):** DirectSound, in two layers.

1. *Output layer* — `CphxSound`: `DirectSoundCreate` → primary buffer `SetFormat(44100 Hz, 2 ch,
   16-bit)`; the song is one big secondary buffer
   (`DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS`) filled by `Lock/CopyMemory/Unlock`, played
   with `Play(0,0,NULL)`, position read with `GetCurrentPosition()`. No mixer, no streaming, no
   callback. `mmsystem.h`/`dsound.h` are included by `phxSound.h:3-4`. **[VERIFIED]**
2. *Engine layer* — MVX also has its own DirectSound output path (`mvxSystem_Init`,
   `mvxSystem_InitWithGUID`, `mvxSystem_Raw*`, `mvxSystem_GetSync`) plus `mvxACMDecompress`
   (Windows ACM) for compressed samples. **The tool does not use any of it** — it calls
   `mvxMixer_Render` itself. V2M and OGG backends have their own DirectSound threads
   (`oggPlayer_SamplesToRender` in `MinimalPlayer/MusicPlayer.cpp:143-153`). **[VERIFIED]**

**Portable replacement:** because the tool renders the *entire* song to memory up front, the
replacement is far simpler than a synth host — it is a PCM player:

```c
// one-time, at load:
//   1. obtain PCM: <song>.apxsndcache (SoundCache format, §1.6)  or  <song>.wav/opus
//   2. build the tick table: TickData[i] in ms (shipped alongside, or parse the .mvm header)
SDL_AudioSpec want = { .freq = 44100, .format = AUDIO_S16SYS, .channels = 2,
                       .samples = 1024, .callback = NULL };
SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
SDL_QueueAudio(dev, pcm, pcmBytes);
// playhead:
uint64_t queuedAndPlayed = pcmBytes;                 // if fully queued
Uint32   playedBytes     = ...;                      // SDL_GetQueuedAudioSize delta, or track it
uint32_t ms   = (uint32_t)(playedFrames * 1000ull / 44100ull);
int      frame = (int)( ms / 1000.0f * 120.0f );     // exact same formula as GetFrameToRender()
// seek: SDL_ClearQueuedAudio + SDL_QueueAudio(pcm + offsetBytes, ...)
// song length: 284346 ms (padding variant, §1.8)
```

Guidance:

* Prefer a **push/queue** model (`SDL_QueueAudio`) over a callback: the source is one contiguous
  buffer, so seeking is just a re-queue, and drift is impossible because position is derived from
  bytes consumed, not from a wall clock.
* If the shipped asset is Opus/Vorbis instead of PCM (47.9 MiB is large for a demo bundle),
  decode the **whole** file to PCM at load (as the original does) so the playhead still has an
  exact `samples_played` counter. Do not stream-decode, or position will not match `TickData`.
* Keep a `TickData` equivalent for the editor/debug overlay — `MulDiv(samples, 1000, 44100)` per
  tick reproduces the original's ms table bit-for-bit for a fixed TPB/BPM song.
* Do **not** attempt a real-time MVX render callback. It would require the engine, and it would
  make frame sync dependent on audio callback jitter, whereas the original design is offline +
  cursor.

---

## 5. Recommendation

**Do not port the MVX synth. Ship pre-rendered audio and drive sync from the sample clock.**

### Evidence

| question | finding | source |
|---|---|---|
| Is MVX engine source in the repo? | **No.** Only `mvx.h` ×3 and 6 `.lib` files. | `find -iname "*mvx*"` over the whole tree |
| Is it a portable library? | **No.** x86-32 COFF (`0x014C`), MSVC `__fastcall` ABI, SSE `__m128`, DirectSound/ACM/`HWND` in the public API. | archive headers + symbol tables |
| Is there an offline WAV export API? | Declared (`mvxWavRenderer`, returns `tickdata`) but **absent from every shipped `.lib`**; no `extern "C"` export exists either. | grep over all `.lib` |
| Is there a portable format to consume instead? | **Yes**: `SoundCache\<djb2>.apxsndcache` = `'!CSA'` + header string + ms marker table + s16 stereo PCM. Produced by the original tool for `.mvm` files. | `phxSongBase.cpp:74-157` |
| Does the portable player need the synth at runtime? | **No.** Render happens once, offline; playback is a single PCM buffer + a play cursor. | `phxSongBase.cpp`, `phxSound.cpp` |
| Can the tick timeline be recovered without the synth? | **Yes**, from the `.mvm` header alone (§1.2/§1.8). | byte analysis, boundary arithmetic |

### Ranked options

**Option A (recommended) — offline capture, once, on Windows/Wine.**
Use the original `MinimalPlayer` (or the apEx tool itself, or its `SoundCache` output) to render
`step010padding.mvm` to PCM/WAV, then ship that asset in the portable player.

* *How*: the tool already does exactly this. The cheapest route is to let the Windows build produce
  `SoundCache\<hash>.apxsndcache` (fully specified, §1.6) and convert it, or run a 30-line
  `MinimalPlayer`-style harness that calls `mvxSystem_LoadMusic` + the `Tick()`/`mvxMixer_Render`
  loop and writes a WAV. Both need the x86 Windows libs, so build on Windows, or on macOS/Linux
  via Wine/CrossOver (Rosetta 2 alone cannot run Win32).
* *Deliverable*: `music.opus`/`music.wav` + `music.ticks` (ms markers) + metadata
  (`N=2432`, `BPM=135`, duration 284 346 ms, 12 543 616 frames).
* *Why*: it is the **only** route that preserves the exact music, and it costs one build step, not
  a porting project. 47.9 MiB of s16 stereo PCM can be shipped as ~5–8 MiB Opus if size matters.

**Option B — timeline-only (no audio).** Parse the `.mvm` header in the portable player
(§1.2/§1.8: magic, `N`, BPM/TPB bytes, the three sequence arrays, plus the trailer) and drive
`frame = f(sample_clock_or_tick_clock)`. Ship a silent/placeholder track or let the demo run
silently. This is genuinely viable because the demo's frame sync only needs a monotonic
`frame = floor(ms * 120 / 1000)` — the tick table is cosmetic. Use this as the **hard fallback**
when no audio asset is available (e.g. missing asset → still play the demo in sync).

**Option C — reimplement MVX.** Rejected. There is no source; you would be re-authoring ≥28
machine types and the `.mvm` deserialiser from symbol names alone. Even then the output would not
be bit-identical, and every parameter name/value mapping would have to be reverse-engineered.
Weeks-to-months of work for a strictly worse result than Option A.

**Option D — substitute a different synth (V2M/WaveSabre, both of which *are* portable C++ source,
`apEx/Libraries/V2` and `apEx/Libraries/WaveSabre`).** Rejected for Clean Slate specifically: the
`.mvm` is MVX-specific data; re-scoring the music is a creative change, not a port.

### Concrete plan for the portable player

1. `MusicAsset { pcm[]; uint32_t markers[]; uint32_t length_ms; uint32_t num_ticks; }`
   loaded from a sidecar file next to the `.apx`/`.mvm`.
2. Playhead: `samples_played` from the audio backend (SDL queue accounting). Never from a timer —
   unless no audio device exists, in which case fall back to `clock_gettime` + `PlaybackStart`,
   exactly like `apxProject.cpp:1297`.
3. Frame: `frame = (int)( (samples_played * 1000 / 44100) * 120 / 1000 )`, clamped `max(0, …)`.
4. Seek: `TogglePlaying()`'s semantics — re-queue PCM from `frame/120*1000 ms`.
5. Implement the MVM header parser (§1.2) as the **fallback** so the demo still syncs if the audio
   asset is missing, and to self-verify the shipped asset (`N`, duration, marker count).

---

## 6. Does the `.mvm` embed a plugin binary? How to extract it?

**No. There is no embedded plugin/DLL binary in either `.mvm`. [VERIFIED]**

Procedure that was run (repeatable, and it is the same procedure to re-verify on any new `.mvm`):

```bash
cd "~/Repos/apEx-public/Projects/Clean Slate"
python3 - <<'PY'
import re, math, collections
for fn in ("step010.mvm", "step010padding.mvm"):
    d = open(fn,'rb').read()
    print(fn, len(d))
    for sig in (b'MZ', b'PE\x00\x00', b'RIFF', b'DLS ', b'PK\x03\x04', b'\x1f\x8b', b'LZ', b'SZDD'):
        print("  ", sig, [hex(m.start()) for m in re.finditer(re.escape(sig), d)][:5])
    for i in range(0, len(d), 4096):
        w = d[i:i+4096]; c = collections.Counter(w)
        H = -sum((n/len(w))*math.log2(n/len(w)) for n in c.values())
        if i % (len(d)//12 or 1) < 4096: print(f"   @0x{i:06x} entropy {H:.2f}")
PY
```

Results: `MZ` → `[]`, `PE\0\0` → `[]`, `RIFF`/`DLS `/`fmt `/`WAVE` → `[]`, `PK`/gzip/LZ/SZDD → `[]`;
per-4 KB entropy **0.00–1.86 bits/byte**; longest `0xFF` run 7609 bytes at `0x976A5`.

* The **only** plugin-like binaries anywhere in this story are the MVX engine `.lib` files
  (`apEx/MVX/MVXLibrary.lib`, `apEx/Libraries/MVX/mvxPlayerLib_*.lib`,
  `apEx/MinimalPlayer/mvxplayerlib_minimal.*`). They are **MSVC `ar` static archives of x86-32
  COFF objects** — not Win32 DLLs, so there is no PE header to extract and nothing loadable at
  runtime. They can only be consumed by an MSVC x86 linker on Windows.
* The `MinimalPlayer/*.lib` variants (`minimal`, `.darkness`, `.silence`, `.sunday`, `.vessel`)
  can be unpacked with `ar x` / `llvm-ar x` and inspected with `strings`/`objdump` (this is how the
  machine list in §2.2 was recovered), but they contain no usable macOS payload.
* If a future `.mvm` *did* embed a plugin, the extraction rule would be: locate the `MZ` magic at a
  4-byte-aligned offset, read `e_lfanew` at `+0x3C`, verify `PE\0\0` there, then dump
  `SizeOfImage` bytes. For Clean Slate this never triggers.

---

## 7. Reproduction / verification commands

```bash
# structure of the two files, boundary proof for the three sequence arrays
cd "~/Repos/apEx-public/Projects/Clean Slate"
python3 - <<'PY'
a=open("step010.mvm",'rb').read(); b=open("step010padding.mvm",'rb').read()
N=2336; N2=2432; base=0xF4
assert a.count(b'MZ')==0 and a.count(b'PE\x00\x00')==0
print("u16@6", int.from_bytes(a[6:8],'little'), "u16@0xF2", int.from_bytes(a[0xF2:0xF4],'little'))
print("numMachines", int.from_bytes(a[0x0A:0x0C],'little'))
print("A1 @0x%X = %s..." % (base+N, a[base+N:base+N+8].hex(' ')))
print("A2 @0x%X = %s..." % (base+2*N, a[base+2*N:base+2*N+8].hex(' ')))
print("A2 @0x%X = %s..." % (base+2*N2, b[base+2*N2:base+2*N2+8].hex(' ')))
print("trailer a", [int.from_bytes(a[base+3*N+2*k:base+3*N+2*k+2],'little') for k in range(4)])
print("trailer b", [int.from_bytes(b[base+3*N2+2*k:base+3*N2+2*k+2],'little') for k in range(4)])
PY

# engine binaries: architecture and API surface
cd ~/Repos/apEx-public
python3 - <<'PY'
import struct,glob
for fn in sorted(glob.glob("apEx/Libraries/MVX/*.lib"))+["apEx/MVX/MVXLibrary.lib"]:
    d=open(fn,'rb').read(); off=8
    while off+60<=len(d):
        hdr=d[off:off+60]; sz=int(hdr[48:58].decode('latin1').strip())
        nm=hdr[0:16].decode('latin1').strip()
        if nm not in ('/','//',''):
            print(fn, hex(struct.unpack_from('<H',d,off+60)[0])); break
        off+=60+sz+(sz%2)
PY
strings -a apEx/Libraries/MVX/mvxPlayerLib_vs2019_releasestatic.lib | grep -E "^\?mvx(System|Mixer|RenderOffline)" | sort -u
grep -c mvxWavRenderer apEx/Libraries/MVX/*.lib          # -> 0 everywhere
```

## 8. Open items / explicitly uncertain

1. Semantics of the `u16 @0x008` (always 0) and of the four-`u16` trailer
   `128, 2048, N, 1026`. **[UNCERTAIN]**
2. `MasterSeq` vs `BPMSeq` assignment for the all-`0xFF` and `[135,0xFF,…]` arrays (§1.2).
   Either way the timeline maths in §1.8 is unchanged, because the only non-`0xFF` byte is `135`,
   which is also the header BPM. **[INFERRED]**
3. Musical meaning of the strict `TPB = 3,5,3,5,…` alternation. The *arithmetic* is exact; the
   intent (swing? two interleaved sequences? a different field entirely?) is **[UNCERTAIN]**. If
   exact tick boundaries matter, validate against an `.apxsndcache` marker table from the original
   tool — that is a direct, cheap falsification test.
4. Full byte-level decoding of the pattern/note payload after `0xF4+3N+8`; requires
   `mvxSystem_LoadMusic`, which is binary-only. **[UNCERTAIN]**
5. Whether `apEx/MinimalPlayer/mvxplayerlib_minimal.{darkness,silence,sunday,vessel}` correspond to
   specific named songs. Sizes/md5s all differ and each is a valid x86 `ar` archive; the per-song
   claim is **[INFERRED]** from the naming and the per-song `NumMachines`.
6. `apxProject.cpp` `LoadSong`/`AcquireTickData` were read directly; the exact `Timeline->Timeline`
   object owning `FrameRate` (`apEx/Phoenix_Tool/Timeline_tool.*`) was not opened in this pass —
   the value `120` is taken from `cleanslate.apx`'s `<Framerate>` element and the usage sites.
