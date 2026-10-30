# Copyright 2025 Mowo Games. All Rights Reserved.
# What the team needs to know about CI/CD, as a card in the project's Discord channel and its
# Telegram topic: a build is waiting for someone, a deployment went live or did not, a pipeline
# broke — or is green again after it broke. Green after green says nothing — that is noise.
#
# Sourced by the workflows:   . .github/notify.sh
#   notify <colour> <title> <description>     the card
#   notify_fixed <title> <description>        a green card, only if the previous run of this
#                                             workflow on this branch failed
# The colour is a meaning: $NOTIFY_RED a failure (and nothing else), $NOTIFY_GREEN something that
# worked, $NOTIFY_AMBER in progress or worth a look, $NOTIFY_BLUE information or waiting for a
# person. The description may use **bold** and a last line starting "> " (a quote).
#
# notify.py beside this file draws the card: who started the run, how long it took, every job
# with its result and link, and for a failure the failed step and the end of its log. Where
# there is no python3 (a bare self-hosted host), a plain card is sent from here instead.
#
# The same files are in every repository that posts (durak-online, MowoEngine, mowoengine.com,
# mowogames-website, Wolang, MowoProtection); a fix goes into every copy.
#
# Where it posts — none of it set, nothing is sent, and a message that cannot be sent never fails
# the job that sends it:
#   DISCORD_WEBHOOK_URL   secret: the project's CI channel webhook(s), one per line
#   TELEGRAM_BOT_TOKEN    secret: the bot that posts
#   TELEGRAM_CHAT_ID      the supergroup
#   TELEGRAM_THREAD_ID    the project's CI topic (empty or 1: the General topic)
#   GITHUB_TOKEN          optional, with `actions: read`: the details above; without it the card
#                         has the commit, branch and run only, and notify_fixed says nothing

NOTIFY_RED=red
NOTIFY_GREEN=green
NOTIFY_AMBER=amber
NOTIFY_BLUE=blue
_notify_py="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)/notify.py"

notify() {
  if command -v python3 >/dev/null 2>&1 && [ -f "$_notify_py" ]; then
    python3 "$_notify_py" "$1" "$2" "$3" || echo "notify: the message was not sent"
    return 0
  fi
  # Without python3 the previous run cannot be looked up: say nothing rather than guess
  [ "${NOTIFY_ONLY_IF_FIXED:-}" = 1 ] && return 0
  command -v curl >/dev/null 2>&1 || { echo "notify: no curl here"; return 0; }
  case "$1" in
    red) _colour=15548997 ;; green) _colour=5763719 ;; amber) _colour=16705372 ;; *) _colour=5793266 ;;
  esac
  _repo="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}"
  _run="$_repo/actions/runs/${GITHUB_RUN_ID}"
  _commit="$_repo/commit/${GITHUB_SHA:-}"
  _who="${GITHUB_ACTOR:-—}"
  _sha="$(printf '%s' "${GITHUB_SHA:-}" | cut -c1-8)"
  _desc="$(printf '%s\n' "$3" | sed '/^> *$/d')"

  if [ -n "${DISCORD_WEBHOOK_URL:-}" ]; then
    _j() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | awk 'NR > 1 { printf "\\n" } { printf "%s", $0 }'; }
    _payload=$(printf '{"username":"%s CI/CD","allowed_mentions":{"parse":[]},"embeds":[{"title":"%s","url":"%s","description":"%s","color":%s,"fields":[{"name":"Branch","value":"`%s`","inline":true},{"name":"Commit","value":"[`%s`](%s)","inline":true},{"name":"Repository","value":"[%s](%s)","inline":true}],"author":{"name":"%s"},"footer":{"text":"%s · %s"},"timestamp":"%s"}]}' \
      "$(_j "${GITHUB_REPOSITORY#*/}")" "$(_j "$2")" "$_run" "$(_j "$_desc")" "$_colour" \
      "$(_j "${GITHUB_REF_NAME:-}")" "$_sha" "$_commit" "$(_j "$GITHUB_REPOSITORY")" "$_repo" \
      "$(_j "$_who")" "$(_j "${GITHUB_WORKFLOW:-}")" "$(_j "${GITHUB_JOB:-}")" "$(date -u +%Y-%m-%dT%H:%M:%SZ)")
    for _hook in $DISCORD_WEBHOOK_URL; do
      curl -fsS -m 10 -H 'Content-Type: application/json' -d "$_payload" "$_hook" >/dev/null \
        || echo "notify: the Discord message was not sent"
    done
  fi

  if [ -n "${TELEGRAM_BOT_TOKEN:-}" ] && [ -n "${TELEGRAM_CHAT_ID:-}" ]; then
    _h() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }
    _text="<b><a href=\"$_run\">$(_h "$2")</a></b>
$(_h "$_desc" | sed -E -e 's/\*\*([^*]+)\*\*/<b>\1<\/b>/g' -e 's/^&gt; (.*)$/<blockquote>\1<\/blockquote>/')

<code>$(_h "${GITHUB_REF_NAME:-}")</code> · <a href=\"$_commit\">$_sha</a> · $(_h "$_who") · <a href=\"$_repo\">$(_h "$GITHUB_REPOSITORY")</a>
<i><a href=\"$_run\">$(_h "${GITHUB_WORKFLOW:-}") run</a></i>"
    set -- -fsS -m 10 -o /dev/null \
      --data-urlencode "chat_id=$TELEGRAM_CHAT_ID" \
      --data-urlencode "parse_mode=HTML" \
      --data-urlencode 'link_preview_options={"is_disabled":true}' \
      --data-urlencode "text=$_text"
    # The forum's General topic takes no thread id; it refuses 1
    if [ -n "${TELEGRAM_THREAD_ID:-}" ] && [ "$TELEGRAM_THREAD_ID" != 1 ]; then
      set -- "$@" --data-urlencode "message_thread_id=$TELEGRAM_THREAD_ID"
    fi
    curl "$@" "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
      || echo "notify: the Telegram message was not sent"
  fi
  return 0
}

notify_fixed() {
  NOTIFY_ONLY_IF_FIXED=1 notify "$NOTIFY_GREEN" "$1" "$2"
}
