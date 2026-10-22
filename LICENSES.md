# Licence map

This repository is **not one licence**. Which terms apply depends on the path,
and it matters: one of the two licences below forbids commercial use.

| Path | Licence | Where the text is |
| --- | --- | --- |
| `src/`, `tools/`, `tests/`, `docs/`, `ci/`, `cmake/`, `.github/`, `.claude/`, `.dshrc`, `CMakeLists.txt`, `README.md`, `NOTICE` | **Apache License 2.0** | [`LICENSE`](LICENSE) |
| `third_party/apEx/` | **CC BY-NC 4.0** | [`third_party/apEx/LICENSE.txt`](third_party/apEx/LICENSE.txt) |
| `third_party/imgui/` | **MIT** | [`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt) |
| `demo/cleanslate.apx`, `demo/music/*.mvm` | **CC BY-NC 4.0** (Conspiracy's project and song data: the songs are the apEx release's, one per bundled project) | as above |
| `src/player/precalc_blobs.cpp` | **CC BY-NC 4.0** | as above |

`src/player/precalc_blobs.cpp` is an exception inside the Apache directory and
is called out deliberately: it embeds the release's four loading-screen projects
(`apEx/MinimalPlayer/Data/precalc*.64k`) byte for byte, because the loading
screen is player data rather than project data.  Those bytes are Conspiracy's.

Shaders quoted verbatim in `tests/` and `tools/` for comparison purposes remain
the licenced material; the quoting code around them is Apache 2.0.

## What that means in one paragraph

The engine sources and the demo are **NonCommercial**, so **the repository as a
whole cannot be used for commercial purposes**.  The Apache 2.0 grant above does
not change that, and does not extend to anything in the table marked CC BY-NC.
Because CC BY-NC 4.0 is not an OSI-approved licence, **this repository is not an
open-source work as a whole** — the paths marked Apache 2.0 are, and the rest is
non-commercial source-available material that its own licence permits
redistributing with attribution.

CC BY-NC 4.0 carries no ShareAlike clause, which is why this project's own code
can be Apache 2.0 at all: nothing forces the port's sources to inherit the
NonCommercial term.  They simply cannot be used to make the engine or the demo
commercial.

## Attribution, and the modification notice

The vendored engine and the demo are **Conspiracy's** apEx and Clean Slate.
`NOTICE` carries the copyright line and the full attribution list.

CC BY-NC 4.0 clause 3(a)(1)(B) requires modifications to be indicated, so:

`third_party/apEx` here is **modified, and is a subset of the release**: it
carries `Bedrock/`, `Phoenix/`, `Phoenix_Tool/`, `LibCTiny/`, `Libraries/`,
`MVX/` and `MinimalPlayer/`, not the release's top level (`Projects/` lives in
`demo/` here, and the release's `README.md` is not vendored).  Every change to
the files it does carry is a single documented edit in
[`tools/apply_patches.py`](tools/apply_patches.py), applied by running that
script, and marked `[port]` in the source it touches.

Nothing in `third_party/apEx` is relicensed under Apache 2.0: the port's own
files elsewhere are, and the modified engine files keep their original terms.
That distinction is the whole point of this file.

## Not distributed here

The rendered audio — the song as a WAV, and the `.apxsndcache` the player uses
for sync — is **not** in this repository, and is not distributed with it.  It is
produced locally or by CI from the project's own song data, and the composition
remains Conspiracy's and its composer's.
