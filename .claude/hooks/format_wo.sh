#!/usr/bin/env bash
# PostToolUse on Edit|Write: format a .wo file the moment it changes, and
# hand a parse error straight back so it is fixed in the same turn rather
# than found by CI. A no-op until the driver has been built, and for the
# two examples that do not parse on purpose.
set -uo pipefail
file="$(python3 -c 'import json,sys; print((json.load(sys.stdin).get("tool_input") or {}).get("file_path",""))' 2>/dev/null)"
case "$file" in
  *.wo) ;;
  *) exit 0 ;;
esac
case "$file" in
  */examples/errors.wo|*/examples/recovery.wo) exit 0 ;;
esac
root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
w="$root/build/${WL_PRESET:-debug}/tools/wolang/wolang"
[ -x "$w" ] && [ -f "$file" ] || exit 0
if ! out="$("$w" fmt --write --no-color "$file" 2>&1)"; then
  echo "wolang fmt could not format $file:" >&2
  echo "$out" | head -20 >&2
  exit 2
fi
exit 0
