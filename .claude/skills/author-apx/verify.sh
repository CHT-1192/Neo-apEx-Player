#!/bin/sh
# Answers one question about a .apx: does it load and render anything?
#
#   verify.sh <project.apx> [frames]
#
# Prints three facts and a verdict:
#   * the exit code (a crash means the importer met something unexpected)
#   * how many [import] errors it logged
#   * the brightest byte of the last rendered frame, because "rendered nothing"
#     and "rendered a black frame" look the same to the eye but not to the
#     loader, and a project that loads and draws nothing is otherwise
#     indistinguishable from success.
#
# Mean brightness is the wrong test here: this demo opens on a fade from black,
# so a perfectly working project measures 0.36/255 on its first frames.  A
# wholly black frame is the real failure signal.
set -u

prog=${1:?usage: verify.sh <project.apx> [frames]}
frames=${2:-30}
last=$((frames - 1))
dump=/tmp/verify-$$.ppm
log=/tmp/verify-$$.log

if [ ! -x ./build/neo_apex_player ]; then
  echo "verify.sh: no player at ./build/neo_apex_player -- build it first" >&2
  exit 2
fi

./build/neo_apex_player "$prog" --headless --no-ui --no-audio --no-vsync \
  --fixed-fps --width 320 --height 180 --frames "$frames" \
  --dump-frame "$last=$dump" >"$log" 2>&1
code=$?

imports=$(grep -c '\[import\]' "$log" 2>/dev/null || true)
[ -z "$imports" ] && imports=0

if [ -f "$dump" ] && [ "$(head -c 2 "$dump")" = "P6" ]; then
  read -r brightness maxbyte <<EOF
$(python3 -c 'import sys;d=open(sys.argv[1],"rb").read().split(b"\n",3);px=d[3] if len(d)>3 else b"";print("%.2f %d"%(sum(px)/len(px) if px else 0.0,max(px) if px else 0))' "$dump")
EOF
else
  brightness=no-dump
  maxbyte=no-dump
fi

echo "project:     $prog"
echo "exit code:   $code"
echo "imports:     $imports '[import]' line(s) -- Clean Slate itself reports 35"
echo "             (shadertoy events naming parameters the material does not carry);"
echo "             they are tolerated, so compare this number, do not expect zero"
echo "frame $last:   mean brightness $brightness, brightest byte $maxbyte (0 = wholly black)"

verdict=fail
if [ "$code" -eq 0 ]; then
  case "$maxbyte" in
    ''|*[!0-9]*) : ;;
    *) [ "$maxbyte" -gt 0 ] && verdict=ok ;;
  esac
fi
echo "verdict:     $verdict"

if [ "$verdict" != ok ]; then
  echo "--- last 12 lines ---"
  tail -12 "$log"
fi

rm -f "$dump" "$dump.target.ppm" "$log"
[ "$verdict" = ok ]
