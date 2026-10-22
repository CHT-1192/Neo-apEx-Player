#!/bin/sh
# Records a demo with the player's own raw frame pipe -- no screen capture.
#
#   record.sh <project.apx> <out.mp4> [WIDTHxHEIGHT] [frames]
#
# The frame size and rate are taken from the player itself (the `#rawvideo`
# header it prints on stderr) rather than guessed, because ffmpeg is unforgiving
# about a -s that does not match: it does not fail, it shears every frame.
set -eu

prog=$1
out=$2
size=${3:-1920x1080}
frames=${4:-}

player=./build/neo_apex_player
if [ ! -x "$player" ]; then
  player=./build/neo_apex_player
  echo "record.sh: no built player at $player -- run cmake --build build first" >&2
  exit 1
fi

w=${size%x*}
h=${size#*x}

# One frame, to learn what size and rate the renderer will actually produce.
hdr=$("$player" "$prog" --headless --no-ui --no-audio --no-vsync --fixed-fps \
        --width "$w" --height "$h" --frames 1 --raw-video 2>&1 >/dev/null \
      | grep -m1 '^#rawvideo' || true)
if [ -z "$hdr" ]; then
  echo "record.sh: the player did not announce a raw video header" >&2
  exit 1
fi

# "#rawvideo rgb24 1920x1080 fps 120.000000 frames 60"
set -- $hdr
size=$3
rate=$5
echo "record.sh: $size at $rate fps -> $out" >&2

limit=""
if [ -n "$frames" ]; then limit="--frames $frames"; fi

# --yield keeps a long render from monopolising the machine; 50 halves the load
# and doubles the wall time.  Drop it if wall time matters more than the machine.
"$player" "$prog" --headless --no-ui --no-audio --no-vsync --fixed-fps \
  --width "$w" --height "$h" --yield 50 $limit --raw-video 2>/dev/null \
| ffmpeg -v error -f rawvideo -pix_fmt rgb24 -s "$size" -r "$rate" -i - \
    -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p -y "$out"

echo "record.sh: wrote $out" >&2
