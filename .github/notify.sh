# Copyright 2025 Mowo Games. All Rights Reserved.
# What the team needs to know about CI/CD, as a card in the project's Discord channel and its
# Telegram topic: a build is waiting for someone, a deployment went live or did not, a pipeline
# broke. Green checks say nothing — that is noise.
#
# Sourced by the workflows:   . .github/notify.sh; notify <colour> <title> <description>
# The description may use **bold** and a last line starting "> " (a quote); both are drawn in
# Discord and in Telegram.
#
# The same file is in every repository that posts (durak-online, MowoEngine, mowoengine.com,
# mowogames-website, Wolang, MowoProtection); a fix goes into every copy. It needs only sh, sed,
# awk and curl, because it also runs on self-hosted deploy runners.
#
# Where it posts — none of it set, nothing is sent, and a message that cannot be sent never fails
# the job that sends it:
#   DISCORD_WEBHOOK_URL   secret: the project's CI channel webhook(s), one per line
#   TELEGRAM_BOT_TOKEN    secret: the bot that posts
#   TELEGRAM_CHAT_ID      variable: the supergroup
#   TELEGRAM_THREAD_ID    variable: the project's CI topic (empty or 1: the General topic)
#   GITHUB_TOKEN          optional, with `actions: read`: the card lists every job of the run with
#                         its result and a link to it; needs python3 to read GitHub's answer

NOTIFY_GREEN=5763719
NOTIFY_LIME=13168714
NOTIFY_RED=15548997
NOTIFY_BLUE=5793266
NOTIFY_AMBER=16705372

notify() {
  command -v curl >/dev/null 2>&1 || { echo "notify: no curl here"; return 0; }
  case "${GITHUB_REPOSITORY:-}" in
    */durak-online) _name="UltimateDurak" ;;
    */MowoEngine) _name="Mowo Engine" ;;
    */mowoengine.com) _name="mowoengine.com" ;;
    */mowogames-website) _name="mowogames.com" ;;
    */Wolang) _name="Wolang" ;;
    */MowoProtection) _name="MowoProtection" ;;
    *) _name="${GITHUB_REPOSITORY#*/}" ;;
  esac
  _run="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}"
  _commit="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}/commit/${GITHUB_SHA:-}"
  _who="${COMMIT_AUTHOR:-${GITHUB_ACTOR:-—}}"
  _sha="$(printf '%s' "${GITHUB_SHA:-}" | cut -c1-8)"
  _repo="${GITHUB_SERVER_URL}/${GITHUB_REPOSITORY}"
  # A run started by hand has no commit message: its empty quote line goes, not a blank quote
  _desc="$(printf '%s\n' "$3" | sed '/^> *$/d')"

  # The run's jobs that have finished, as "icon<TAB>name<TAB>url" — the job posting this card is
  # still running and is left out. Nothing (and no list) when the token or python3 is missing.
  _jobs=""
  if [ -n "${GITHUB_TOKEN:-}" ] && command -v python3 >/dev/null 2>&1; then
    _jobs="$(curl -fsS -m 10 -H "Authorization: Bearer $GITHUB_TOKEN" -H 'Accept: application/vnd.github+json' \
      "${GITHUB_API_URL:-https://api.github.com}/repos/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}/attempts/${GITHUB_RUN_ATTEMPT:-1}/jobs?per_page=100" 2>/dev/null \
      | python3 -c '
import json, sys
icon = {"success": "✅", "failure": "❌", "cancelled": "⛔", "skipped": "⏭️", "timed_out": "⏱️"}
for j in json.load(sys.stdin).get("jobs", []):
    if j.get("conclusion"):
        print(icon.get(j["conclusion"], "⚠️"), j["name"].replace("\t", " "), j["html_url"], sep="\t")
' 2>/dev/null)" || _jobs=""
  fi

  if [ -n "${DISCORD_WEBHOOK_URL:-}" ]; then
    _djobs=""
    if [ -n "$_jobs" ]; then
      # a field holds 1024 characters: the list stops before it would not fit
      _djobs=$(printf '%s\n' "$_jobs" | awk -F '\t' '{ gsub(/[][\\"]/, "", $2); l = $1 " [" $2 "](" $3 ")"; if (n + length(l) > 1000) exit; out = out (n ? "\\n" : "") l; n += length(l) + 1 } END { printf "%s", out }')
      _djobs=",{\"name\":\"Jobs\",\"value\":\"$_djobs\",\"inline\":false}"
    fi
    _j() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | awk 'NR > 1 { printf "\\n" } { printf "%s", $0 }'; }
    _payload=$(printf '{"username":"%s CI/CD","allowed_mentions":{"parse":[]},"embeds":[{"title":"%s","url":"%s","description":"%s","color":%s,"fields":[{"name":"Branch","value":"`%s`","inline":true},{"name":"Commit","value":"[`%s`](%s)","inline":true},{"name":"Author","value":"%s","inline":true},{"name":"Repository","value":"[%s](%s)","inline":true}%s],"footer":{"text":"%s · %s"},"timestamp":"%s"}]}' \
      "$(_j "$_name")" "$(_j "$2")" "$_run" "$(_j "$_desc")" "$1" \
      "$(_j "${GITHUB_REF_NAME:-}")" "$_sha" "$_commit" \
      "$(_j "$_who")" "$(_j "$GITHUB_REPOSITORY")" "$_repo" "$_djobs" "$(_j "${GITHUB_WORKFLOW:-}")" "$(_j "${GITHUB_JOB:-}")" "$(date -u +%Y-%m-%dT%H:%M:%SZ)")
    for _hook in $DISCORD_WEBHOOK_URL; do
      curl -fsS -m 10 -H 'Content-Type: application/json' -d "$_payload" "$_hook" >/dev/null \
        || echo "notify: the Discord message was not sent"
    done
  fi

  if [ -n "${TELEGRAM_BOT_TOKEN:-}" ] && [ -n "${TELEGRAM_CHAT_ID:-}" ]; then
    _h() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }
    _tjobs=$(printf '%s\n' "$_jobs" | awk -F '\t' 'NF == 3 { gsub(/&/, "\\&amp;", $2); gsub(/</, "\\&lt;", $2); gsub(/>/, "\\&gt;", $2); print $1 " <a href=\"" $3 "\">" $2 "</a>" }')
    [ -z "$_tjobs" ] || _tjobs="

$_tjobs"
    _text="<b><a href=\"$_run\">$(_h "$2")</a></b>
$(_h "$_desc" | sed -E -e 's/\*\*([^*]+)\*\*/<b>\1<\/b>/g' -e 's/^&gt; (.*)$/<blockquote>\1<\/blockquote>/')$_tjobs

<code>$(_h "${GITHUB_REF_NAME:-}")</code> · <a href=\"$_commit\">$_sha</a> · $(_h "$_who") · <a href=\"$_repo\">$(_h "$GITHUB_REPOSITORY")</a>
<i>$(_h "$_name") · <a href=\"$_run\">$(_h "${GITHUB_WORKFLOW:-}") run</a></i>"
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
