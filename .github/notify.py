#!/usr/bin/env python3
# Copyright 2025 Mowo Games. All Rights Reserved.
"""A CI/CD card for the project's Discord channel and Telegram topic. Called by notify.sh, which
is what the workflows source; see there for the environment it reads.

  notify.py <colour> <title> <description>

The colour is a meaning, not a hue: red is a failure and only a failure, green is something that
worked, amber is in progress or needs a look, blue is information or something waiting for a
person. With GITHUB_TOKEN (actions: read) the card also says who started the run and how long it
has taken, lists every finished job with its result, and for a failure names the failed step and
quotes the end of its log — so the card itself usually says what broke.

NOTIFY_ONLY_IF_FIXED=1: post only when the previous completed run of this workflow on this branch
failed — the «green again» card, so a red card is always followed by an answer.

Python standard library only; a message that cannot be sent is printed, never raised.
"""
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

COLOURS = {"red": 0xED4245, "green": 0x57F287, "amber": 0xFEE75C, "blue": 0x5865F2}
ICONS = {"success": "✅", "failure": "❌", "cancelled": "⛔", "skipped": "⏭️", "timed_out": "⏱️"}
NAMES = {"durak-online": "UltimateDurak", "mowoengine": "Mowo Engine", "mowoengine.com": "mowoengine.com",
         "mowogames-website": "mowogames.com", "wolang": "Wolang", "mowoprotection": "MowoProtection"}
E = os.environ.get
REPO = E("GITHUB_REPOSITORY", "")
SERVER = E("GITHUB_SERVER_URL", "https://github.com")
API = E("GITHUB_API_URL", "https://api.github.com")


def api(path, raw=False):
    """GitHub, or None. The token is not sent on to wherever a log download redirects."""
    if not E("GITHUB_TOKEN"):
        return None
    req = urllib.request.Request(f"{API}{path}", headers={"Accept": "application/vnd.github+json",
                                                         "User-Agent": "mowo-notify"})
    req.add_unredirected_header("Authorization", f"Bearer {E('GITHUB_TOKEN')}")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read()
            return data.decode("utf-8", "replace") if raw else json.loads(data)
    except (urllib.error.URLError, TimeoutError, ValueError) as err:
        print(f"notify: {path}: {err}")
        return None


def duration(start):
    s = int((datetime.now(timezone.utc) - datetime.fromisoformat(start.replace("Z", "+00:00"))).total_seconds())
    return f"{s // 3600}h {s % 3600 // 60}m" if s >= 3600 else f"{s // 60}m {s % 60}s" if s >= 60 else f"{s}s"


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
STAMP = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+Z ?")


def excerpt(log, limit=900):
    """The lines that say why a job failed: up to a dozen before its first real ##[error] (the
    last one, «Process completed with exit code N», says nothing by itself)."""
    lines = [ANSI.sub("", STAMP.sub("", ln)).rstrip() for ln in log.splitlines()]
    errs = [i for i, ln in enumerate(lines) if ln.startswith("##[error]")]
    if not errs:
        return ""
    real = [i for i in errs if "Process completed with exit code" not in lines[i]]
    end = real[0] if real else errs[-1]
    keep = [ln.removeprefix("##[error]") for ln in lines[max(0, end - 12): end + 1]
            if ln.strip() and not ln.startswith(("##[group]", "##[endgroup]", "shell: ", "env:"))
            and not re.match(r"  [A-Za-z_][A-Za-z0-9_]*: ", ln)]  # the step's env, as the runner prints it
    if real:
        keep = [ln for ln in keep if "Process completed with exit code" not in ln]
    text = "\n".join(keep)
    return ("…" + text[-(limit - 1):]) if len(text) > limit else text


def previous_failed(run):
    """Whether the last result before this one, of the same workflow on the same branch, was a
    failure. «Re-run failed jobs» keeps the run and adds an attempt, so a re-run that passed
    looks at its own previous attempt. A cancelled run is no result — a newer push cancels one
    still waiting — so it is looked past, or a red, cancelled, green sequence would say nothing."""
    attempt = run.get("run_attempt") or 1
    if attempt > 1:
        prev = api(f"/repos/{REPO}/actions/runs/{run['id']}/attempts/{attempt - 1}") or {}
        if prev.get("conclusion") == "failure":
            return True
    runs = api(f"/repos/{REPO}/actions/workflows/{run['workflow_id']}/runs?branch={run['head_branch']}"
               f"&status=completed&per_page=30") or {}
    before = [r for r in runs.get("workflow_runs", [])
              if r["id"] != run["id"] and r["created_at"] <= run["created_at"]
              and r.get("conclusion") not in ("cancelled", "skipped", None)]
    if before:
        print(f"notify: the run before this one ({before[0]['html_url']}) concluded {before[0]['conclusion']}")
    return bool(before) and before[0]["conclusion"] == "failure"


def gather():
    """Who, how long, the jobs and — for a failure — where and why. Empty without a token."""
    info = {"jobs": [], "failed": [], "excerpt": ""}
    run = api(f"/repos/{REPO}/actions/runs/{E('GITHUB_RUN_ID')}")
    if not run:
        return info, None
    info["actor"] = run.get("triggering_actor") or run.get("actor") or {}
    if run.get("run_started_at"):
        info["duration"] = duration(run["run_started_at"])
    jobs = (api(f"/repos/{REPO}/actions/runs/{run['id']}/attempts/{run.get('run_attempt', 1)}/jobs?per_page=100") or {}).get("jobs", [])
    for j in jobs:
        if not j.get("conclusion") or j["conclusion"] == "skipped":
            continue  # this job, still running; a skipped one says nothing
        info["jobs"].append((ICONS.get(j["conclusion"], "⚠️"), j["name"], j["html_url"]))
        if j["conclusion"] in ("failure", "timed_out"):
            steps = [s["name"] for s in j.get("steps", []) if s.get("conclusion") == "failure"]
            info["failed"].append((j["name"], steps[0] if steps else "", j["html_url"]))
            if not info["excerpt"]:
                info["excerpt"] = excerpt(api(f"/repos/{REPO}/actions/jobs/{j['id']}/logs", raw=True) or "")
    return info, run


def md(s):
    for ch in "\\*_~`|[]":
        s = s.replace(ch, "\\" + ch)
    return s


def discord_text(s):
    """The description's own markup is **bold** and a "> " quote, which Discord already reads."""
    return s


def telegram_text(s):
    out = []
    for ln in html.escape(s, quote=False).split("\n"):
        ln = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", ln)
        out.append(f"<blockquote>{ln[5:]}</blockquote>" if ln.startswith("&gt; ") else ln)
    return "\n".join(out)


def send(url, payload, what):
    body = json.dumps(payload).encode()
    for _ in range(4):
        req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json", "User-Agent": "mowo-notify"})
        try:
            urllib.request.urlopen(req, timeout=15).close()
            print(f"notify: sent to {what}")
            return
        except urllib.error.HTTPError as err:
            raw = err.read()
            if err.code == 429:
                try:
                    j = json.loads(raw)
                    wait = j.get("retry_after") or (j.get("parameters") or {}).get("retry_after") or 2
                except ValueError:
                    wait = 2
                time.sleep(min(float(wait), 30) + 0.5)
                continue
            print(f"notify: the {what} message was not sent: HTTP {err.code} {raw[:300]!r}")
            return
        except (urllib.error.URLError, TimeoutError) as err:
            print(f"notify: the {what} message was not sent: {err}")
            return
    print(f"notify: the {what} message was not sent: rate limited")


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: notify.py <colour> <title> <description>")
    colour, title, desc = sys.argv[1:]
    colour = COLOURS.get(colour, int(colour) if colour.isdigit() else COLOURS["blue"])
    # A run started by hand has no commit message: its empty quote line goes, not a blank quote
    desc = "\n".join(ln for ln in desc.split("\n") if not re.fullmatch(r"> *", ln)).strip()

    hooks = E("DISCORD_WEBHOOK_URL", "").split()
    tok, chat, thread = E("TELEGRAM_BOT_TOKEN", ""), E("TELEGRAM_CHAT_ID", ""), E("TELEGRAM_THREAD_ID", "")
    dry = E("NOTIFY_DRY_RUN") == "1"
    if not (hooks or (tok and chat) or dry):
        print("notify: no Discord webhook and no Telegram bot configured; nothing sent")
        return
    # One of the two set and the other not is a missing secret, not a choice: say so in the run
    if hooks and not (tok and chat):
        print("::warning::notify: Telegram is not sent: "
              + ("the secret TELEGRAM_BOT_TOKEN is not set for this repository" if not tok else "TELEGRAM_CHAT_ID is empty"))
    if (tok and chat) and not hooks:
        print("::warning::notify: Discord is not sent: the secret DISCORD_WEBHOOK_URL is not set for this repository")

    info, run = gather()
    if E("NOTIFY_ONLY_IF_FIXED") == "1" and not (run and previous_failed(run)):
        print("notify: the previous run did not fail; nothing to say")
        return

    repo_url = f"{SERVER}/{REPO}"
    run_url = f"{repo_url}/actions/runs/{E('GITHUB_RUN_ID', '')}"
    sha = E("GITHUB_SHA") or (run or {}).get("head_sha", "")
    commit_url = f"{repo_url}/commit/{sha}"
    branch = E("GITHUB_REF_NAME", "")
    actor = info.get("actor") or {}
    who = actor.get("login") or E("GITHUB_ACTOR", "—")
    who_url = actor.get("html_url") or f"{SERVER}/{who}"
    name = NAMES.get(REPO.split("/")[-1].lower(), REPO.split("/")[-1])
    took = info.get("duration")

    # ── Discord ──
    fields = [{"name": "Branch", "value": f"`{branch}`", "inline": True},
              {"name": "Commit", "value": f"[`{sha[:8]}`]({commit_url})", "inline": True},
              {"name": "Repository", "value": f"[{md(REPO)}]({repo_url})", "inline": True}]
    if took:
        fields.append({"name": "Duration", "value": took, "inline": True})
    if info["failed"]:
        fields.append({"name": "Failed at", "inline": False, "value": "\n".join(
            f"[{md(j)}]({u})" + (f" › {md(s)}" if s else "") for j, s, u in info["failed"])[:1024]})
    if info["excerpt"]:
        fields.append({"name": "Log", "inline": False, "value": "```\n" + info["excerpt"].replace("```", "ʼʼʼ")[:1000] + "\n```"})
    if info["jobs"]:
        lines, size = [], 0
        for icon, j, u in info["jobs"]:
            line = f"{icon} [{md(j)}]({u})"
            if size + len(line) > 1000:
                lines.append("…")
                break
            lines.append(line)
            size += len(line) + 1
        fields.append({"name": "Jobs", "value": "\n".join(lines), "inline": False})
    embed = {"title": title[:256], "url": run_url, "description": discord_text(desc)[:4000] or "—", "color": colour,
             "fields": fields, "footer": {"text": f"{E('GITHUB_WORKFLOW', '')} · {E('GITHUB_JOB', '')}"},
             "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
             "author": {"name": who, "url": who_url, **({"icon_url": actor["avatar_url"]} if actor.get("avatar_url") else {})}}
    discord = {"username": f"{name} CI/CD", "allowed_mentions": {"parse": []}, "embeds": [embed]}

    # ── Telegram ──
    e = lambda s: html.escape(s, quote=False)
    a = lambda u, t: f'<a href="{html.escape(u)}">{e(t)}</a>'
    parts = [f"<b>{a(run_url, title)}</b>", telegram_text(desc)]
    if info["failed"]:
        parts.append("\n".join("Failed at " + a(u, j) + (f" › {e(s)}" if s else "") for j, s, u in info["failed"]))
    if info["excerpt"]:
        parts.append(f"<pre>{e(info['excerpt'])}</pre>")
    if info["jobs"]:
        parts.append("\n".join(f"{icon} {a(u, j)}" for icon, j, u in info["jobs"]))
    parts.append(f"<code>{e(branch)}</code> · {a(commit_url, sha[:8])} · {a(who_url, who)} · {a(repo_url, REPO)}"
                 + (f" · {e(took)}" if took else "") + f"\n<i>{e(name)} · {a(run_url, E('GITHUB_WORKFLOW', '') + ' run')}</i>")
    text = "\n\n".join(p for p in parts if p)
    if len(text) > 4000:  # Telegram's limit is 4096: the log goes first, then the job list
        parts = [p for p in parts if not p.startswith("<pre>")]
        text = "\n\n".join(p for p in parts if p)[:4000]

    if dry:
        print(json.dumps(discord, ensure_ascii=False, indent=2))
        print(text)
        return
    for hook in hooks:
        send(hook, discord, "Discord")
    if tok and chat:
        msg = {"chat_id": chat, "text": text, "parse_mode": "HTML", "link_preview_options": {"is_disabled": True}}
        # The forum's General topic takes no thread id; it refuses 1
        if thread and thread != "1":
            msg["message_thread_id"] = int(thread)
        send(f"https://api.telegram.org/bot{tok}/sendMessage", msg, "Telegram")


if __name__ == "__main__":
    main()
