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
  # A run started by hand has no commit message: its empty quote line goes, not a blank quote
  _desc="$(printf '%s\n' "$3" | sed '/^> *$/d')"

  if [ -n "${DISCORD_WEBHOOK_URL:-}" ]; then
    _j() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | awk 'NR > 1 { printf "\\n" } { printf "%s", $0 }'; }
    _payload=$(printf '{"username":"%s CI/CD","allowed_mentions":{"parse":[]},"embeds":[{"title":"%s","url":"%s","description":"%s","color":%s,"fields":[{"name":"Branch","value":"`%s`","inline":true},{"name":"Commit","value":"[`%s`](%s)","inline":true},{"name":"Author","value":"%s","inline":true}],"footer":{"text":"%s · %s"},"timestamp":"%s"}]}' \
      "$(_j "$_name")" "$(_j "$2")" "$_run" "$(_j "$_desc")" "$1" \
      "$(_j "${GITHUB_REF_NAME:-}")" "$_sha" "$_commit" \
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

<code>$(_h "${GITHUB_REF_NAME:-}")</code> · <a href=\"$_commit\">$_sha</a> · $(_h "$_who")
<i>$(_h "$_name") · $(_h "${GITHUB_WORKFLOW:-}") · $(_h "${GITHUB_JOB:-}")</i>"
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
