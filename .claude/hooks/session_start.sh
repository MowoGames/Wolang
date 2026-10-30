#!/usr/bin/env bash
# SessionStart: a few lines of orientation, never a document. What the
# project remembers is imported by CLAUDE.md; this only says where work
# stands right now. In a cloud session it also starts the debug build in
# the background, so the first `wl test` does not wait 40 seconds.
set -uo pipefail
root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$root" || exit 0

branch="$(git branch --show-current 2>/dev/null)"
dirty="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
status_date="$(grep -m1 -oE '\*\*Date:\*\* [0-9-]+' STATUS.md 2>/dev/null | sed 's/\*\*Date:\*\* //')"
last="$(git log -1 --format='%h %s' 2>/dev/null)"

echo "Wolang session: branch ${branch:-?}, ${dirty} uncommitted file(s); last commit: ${last}"
echo "STATUS.md dated ${status_date:-?} -- read its 'In short' and 'Not done yet' only when the task needs them."

if [ -x build/debug/tools/wolang/wolang ]; then
  echo "Driver built: build/debug/tools/wolang/wolang (use .claude/scripts/wl)."
elif [ "${CLAUDE_CODE_REMOTE:-}" = "true" ] && command -v clang >/dev/null; then
  mkdir -p build/logs
  nohup "$root/.claude/scripts/wl" build >build/logs/session-build.log 2>&1 &
  echo "Debug build started in the background (build/logs/session-build.log)."
else
  echo "Not built yet: .claude/scripts/wl build"
fi
exit 0
