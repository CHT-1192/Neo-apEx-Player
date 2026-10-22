---
name: record-demo
description: Record Neo-apEx-Player demo playback to a video file using the player's own raw frame pipe, and compare rendered frames against the release video. Use when asked to record, capture, export or re-encode a demo (.apx project or .mvm music), to produce a video of the port, or to check a rendered frame against the reference.
---

# Recording a demo

The player renders to a pipe; ffmpeg encodes it. No screen capture, no window,
no display scaling, no screen-recording permission, and the frames are exactly
the frames the renderer produced.

## The one command

```sh
./build/neo_apex_player demo/cleanslate.apx --headless --no-ui \
  --raw-video --width 1920 --height 1080 \
  | ffmpeg -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 120 -i - \
      -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p out.mp4
```

Or let the bundled script build it from the header the player prints:

```sh
.claude/skills/record-demo/record.sh demo/cleanslate.apx out.mp4 1920x1080 7200
```

Its fourth argument caps the frame count; omit it to record the whole timeline.

`--raw-video` writes **only** pixels to stdout. Everything the player has to say
-- including the `#rawvideo rgb24 WxH fps N` line that gives the size and rate --
goes to stderr.

## Five things that go wrong silently

1. **`-s` must match the announced size exactly.** If it does not, ffmpeg reads
   each row at the wrong stride and every frame is sheared. It does not warn.
   Take the size from the `#rawvideo` line, never from the flag you passed.

2. **Always pass `--width` and `--height`.** Without them the frame size is the
   window's, and a hidden window still gets resized by the window server: a
   320x180 run once reported 1470x779 from frame ~3600 onward, because the swap
   chain had been recreated underneath it. The player now locks the size when
   you give one and stops with an error if it changes anyway, so a stream that
   starts is a stream of one size -- but only if you ask for a size.

3. **`-r` must be the timeline's rate, which is 120 for Clean Slate.** Encoding
   it at 60 does not halve the file size, it plays the demo at half speed. Read
   the rate from the header too.

4. **Bandwidth is the ceiling and the player cannot lift it.** RGB24 is 3 bytes
   per pixel: 1080p120 is 746 MB/s, 4K120 is 3 GB/s. 1080p120 pipes fine; 4K120
   does not keep up and the pipe back-pressures into dropped frames. For a 4K
   deliverable, record 1080p and scale in ffmpeg, or record 60 fps.

5. **The audio is a separate file.** The picture is silent. Pass `--music FILE`
   (a 16-bit PCM WAV, or an `.apxsndcache`) and mux it afterwards; without it the
   clock falls back to wall-clock timing instead of the song's.

For a long render, `--yield 50` sleeps half of each frame's own cost: it halves
the machine load and doubles the wall time. Use it for anything over a minute
that nobody is watching.

## Verifying a recording without watching it

* A raw stream is exactly `W * H * 3 * frames` bytes. Anything else means the
  pipe was corrupted (a size change, or diagnostics leaking into stdout):

  ```sh
  ... --raw-video 2>/dev/null | wc -c        # expect W*H*3*frames
  ```

* `--dump-frame N=FILE` writes a single frame as PPM. **N is the iteration
  index, not the clock frame**: after `--start-frame 3600` the frame you want is
  `0=FILE`, and asking for `3600=FILE` writes nothing and says nothing.

* The regression baseline is `work/baseline/frame-3724.ppm` (frame 3724, 0:31,
  native 1280x720). A rendering change that moves that frame has changed
  something:

  ```sh
  ./build/neo_apex_player demo/cleanslate.apx --headless --no-ui --no-audio \
    --no-vsync --fixed-fps --start-frame 3694 --frames 31 --dump-frame 30=/tmp/f.ppm
  cmp -s /tmp/f.ppm work/baseline/frame-3724.ppm && echo identical
  ```

## Comparing against the release video

`~/Movies/Clean Slate [O3T1-nadehU].webm` is **60.008 fps at 3840x2160**,
against a demo that runs at 120 fps. It resolves 16.7 ms, so:

* sampling it at demo-frame granularity returns the same video frame twice in a
  row about half the time -- successive samples come back bit-identical, which
  looks like evidence about the port and is an artefact of the sampling;
* a claimed offset smaller than ~17 ms is not measurable from it at all, and an
  offset like "0.25 s early" is 15 video frames and needs a window alignment,
  not a frame pair.

Prefer `--raw-video` for anything that needs frame-exact truth: pipe a range,
encode it, and compare encoded frames to encoded frames.

## Related

Making or fixing the project itself is the other skill:
[`../author-apx/SKILL.md`](../author-apx/SKILL.md).

## Reference points

* Timeline rate: 120 fps. Clean Slate is 284.44 s, so the whole demo is 34,133
  frames and a full playthrough is minutes, not seconds -- bound it with
  `--frames` and use `--start-frame` to reach a section (which still replays
  from the beginning, because particle state has history).
* 0:31 is frame 3724; 3:00 is frame 21600.
* `--fixed-fps` makes the clock advance exactly one frame per rendered frame.
  That is what makes recordings deterministic; without it the clock follows the
  audio play cursor.
