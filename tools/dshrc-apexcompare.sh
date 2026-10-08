# --- apEx player vs. original video: side-by-side frame comparison -----------
#
#   apexcompare <frame> [--video FILE] [--apx FILE] [--out FILE] [--scale WxH]
#
# The apEx player renders at the project's <Framerate>, which for Clean Slate is
# 120, and the upload is 284.501 s against the demo's 34132 frames = 284.43 s, so
# demo frame N sits at exactly N/120 seconds in the video.  This dumps that frame
# from both and stacks them: original on top, this port below.
apexcompare() {
  local frame="" video="$HOME/Movies/Clean Slate [O3T1-nadehU].webm"
  local apx="$HOME/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx"
  local player="$HOME/Projects/Neo-apEx-Player/build/neo_apex_player"
  local fps=120 scale="960:540" out=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --video) video="$2"; shift 2 ;;
      --apx)   apx="$2";   shift 2 ;;
      --out)   out="$2";   shift 2 ;;
      --scale) scale="$2"; shift 2 ;;
      --fps)   fps="$2";   shift 2 ;;
      -h|--help) sed -n '2,9p' "$HOME/.dshrc"; return 0 ;;
      *) frame="$1"; shift ;;
    esac
  done
  if [ -z "$frame" ]; then
    echo "usage: apexcompare <frame> [--video FILE] [--apx FILE] [--out FILE] [--scale WxH]" >&2
    return 2
  fi
  [ -f "$video" ]  || { echo "apexcompare: no video at $video" >&2; return 1; }
  [ -f "$apx" ]    || { echo "apexcompare: no project at $apx" >&2; return 1; }
  [ -x "$player" ] || { echo "apexcompare: build the player first ($player)" >&2; return 1; }

  local t ref port
  t=$(awk -v f="$frame" -v r="$fps" 'BEGIN{printf "%.4f", f/r}')
  [ -n "$out" ] || out="/tmp/apexcompare-${frame}.png"
  ref="/tmp/apexcompare-ref-${frame}.png"
  port="/tmp/apexcompare-port-${frame}.ppm"

  echo "apexcompare: frame $frame = ${t}s"

  # -ss before -i makes ffmpeg seek rather than decode from the start of a 4K
  # source; for a still that is exact enough and enormously faster.
  ffmpeg -v error -ss "$t" -i "$video" -frames:v 1 -vf "scale=$scale" -y "$ref" || return 1

  # --start-frame puts the clock straight at the requested demo frame and
  # --frames 1 renders exactly one present, which the dump matches on.
  # --no-ui: the setup dialog is modal and belongs to a human.
  "$player" "$apx" --no-ui --no-audio --no-vsync --start-frame "$frame" --frames 1 \
      --dump-frame 0="$port" >/dev/null 2>&1
  [ -f "$port" ] || { echo "apexcompare: the player produced no frame" >&2; return 1; }

  # Label both halves, then stack original-over-port.
  ffmpeg -v error -i "$ref" -i "$port" -filter_complex \
    "[0:v]scale=$scale,drawtext=text='ORIGINAL  video @ ${t}s':x=8:y=8:fontsize=20:fontcolor=white:box=1:boxcolor=black@0.6[a];\
     [1:v]scale=$scale,drawtext=text='PORT  demo frame ${frame}':x=8:y=8:fontsize=20:fontcolor=white:box=1:boxcolor=black@0.6[b];\
     [a][b]vstack=inputs=2" -frames:v 1 -y "$out" || return 1

  rm -f "$ref" "$port"
  echo "apexcompare: wrote $out"
  echo "$out"
}
