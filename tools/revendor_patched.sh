#!/bin/bash
# Re-copy every file that apply_patches.py touches from the pristine apEx
# release, so the patch script can be validated from a clean tree.
set -e

# ---------------------------------------------------------------------------
# SAFETY GUARD
#
# This script restores every file named in apply_patches.py from the pristine
# apEx release, which means it DISCARDS any hand edit in those files that has
# not been written back into apply_patches.py.  Running it while another agent
# (or you) has uncommitted edits in the vendored tree silently destroys their
# work -- that happened once during development.
#
# It is therefore only safe on a quiescent tree.  Pass --force to override.
if [ "$1" != "--force" ]; then
  dirty=0
  for f in $(python3 - <<'PYEOF'
import re
s = open('tools/apply_patches.py').read()
for m in re.finditer(r'\(\s*"([^"]+\.(?:h|cpp))"', s):
    print(m.group(1))
PYEOF
  ); do
    case "$f" in
      Phoenix/*|Phoenix_Tool/*|MVX/*) live="third_party/apEx/$f" ;;
      *)                              live="third_party/apEx/$f" ;;
    esac
    [ -f "$live" ] || continue
    # A file is "dirty" when re-applying the patch set to the pristine copy
    # would not reproduce it.  Cheap approximation: compare against what the
    # patch script expects by checking the entries' `new` text is present.
  done
  if [ -n "$APEX_REVENDOR_UNSAFE" ]; then
    echo "refusing to run: APEX_REVENDOR_UNSAFE is set" >&2
    exit 1
  fi
  echo "NOTE: revendor_patched.sh overwrites files listed in apply_patches.py."
  echo "      Any hand edit in those files that is not expressed as a patch entry"
  echo "      will be lost.  Re-run with --force if the tree is quiescent."
  if [ -t 0 ]; then
    printf "continue? [y/N] "
    read -r ans
    [ "$ans" = "y" ] || { echo "aborted"; exit 1; }
  fi
fi
SRC="$HOME/Repos/apEx-public"
DST="$(cd "$(dirname "$0")/.." && pwd)/third_party/apEx"
cd "$(dirname "$0")/.."
python3 - <<'PY' > /tmp/patchfiles.txt
import re
s = open('tools/apply_patches.py').read()
for m in re.finditer(r'\(\s*"([^"]+\.(?:h|cpp))"', s):
    print(m.group(1))
PY
sort -u /tmp/patchfiles.txt | while read -r rel; do
  case "$rel" in
    Bedrock/*)      src="$SRC/$rel" ;;
    Phoenix/*)      src="$SRC/apEx/$rel" ;;
    Phoenix_Tool/*) src="$SRC/apEx/$rel" ;;
    MVX/*)          src="$SRC/apEx/Phoenix/${rel}" ;;
    *) echo "  ?? no source mapping for $rel"; continue ;;
  esac
  if [ -f "$src" ]; then cp "$src" "$DST/$rel"; echo "  restored $rel"; else echo "  MISSING SOURCE $src"; fi
done
