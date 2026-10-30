#!/usr/bin/env python3
# Copyright 2025 Mowo Games. All Rights Reserved.
"""Change Log: what of a Mowo project landed or went live, as a card in its Discord channel and
its Telegram topic.

The same file is in every repository that posts one (durak-online, MowoEngine, mowoengine.com,
mowogames-website, Wolang, MowoProtection); only PROJECTS below tells them apart. A fix goes into
every copy.

  changelog.py release   after a deployment (durak-online): a new release/X.Y went live → its card
  changelog.py digest    every morning (durak-online): everything else deployed in the 24 hours
                         before — hotfixes, rollbacks — as one card; nothing deployed, nothing posted
  changelog.py deploy    after a deployment (the websites): what this deployment put live, compared
                         with the previous successful one
  changelog.py push      on a push to the working branch: the commits it brought
                         (--before / --after, from the push event)
  changelog.py tag       on a release tag: the commits since the previous tag (--from)
  … --dry-run            print instead of posting

The text is the commit titles (merges dropped), so they are written for people.

Env: GITHUB_TOKEN (contents read; deployments and actions read for release/digest/deploy),
GITHUB_REPOSITORY. Where to post — none of it set, nothing is posted, and a post that fails is a
warning, never a failed job:
  CHANGELOG_WEBHOOK_URL         Discord webhook(s) of the changelog channel, one per line
  TELEGRAM_BOT_TOKEN            the bot that posts
  TELEGRAM_CHAT_ID              the supergroup
  TELEGRAM_THREAD_ID            the project's changelog topic
  TELEGRAM_GLOBAL_THREAD_ID     optional: release cards are posted there as well
Python standard library only.
"""
import argparse
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

GITHUB_API = "https://api.github.com"
TZ = ZoneInfo(os.environ.get("CHANGELOG_TZ", "Europe/Riga"))
MAX_LINES = 15  # commit lines in one digest or push card; a release card shows 25

# By repository. `site`: where a card's title links. `releases`: deployments come from release/X.Y
# branches, versioned vX.Y.<commits on the branch> like Durak's CI does, and a new X.Y is a
# release; `baseline` is what was live before the first deployment GitHub knows of (the last
# GitLab one), so that one is not taken for a new release. `drop`: title prefixes that are not
# news (bot commits). `collapse`: title prefix → one line said once instead; a deployment that
# brought nothing but collapsed lines is not posted (`quiet`), so the docs bot moving a submodule
# on every engine push does not fill the channel.
PROJECTS = {
    "mowogames/durak-online": {
        "name": "UltimateDurak", "emoji": "🃏", "site": "https://ultimatedurak.com",
        "releases": True,
        "baseline": {"ref": "release/1.6", "sha": "bd9373fe3b643fb498baf0d280368a7d632d29ed",
                     "version": "v1.6.150", "user": "igbi", "at": "2026-10-26T00:02:33Z"},
        "collapse": {"chore(docs): engine docs from MowoEngine": "Engine docs updated"},
    },
    "mowogames/mowoengine": {
        "name": "Mowo Engine", "emoji": "🎮", "site": "https://mowoengine.com",
        "drop": ["Regenerate reference data from the code"],
    },
    "mowogames/mowoengine.com": {
        "name": "mowoengine.com", "emoji": "🕸️", "site": "https://mowoengine.com",
        "collapse": {"chore(docs): engine docs from MowoEngine": "Engine docs updated",
                     "chore(docs): Wolang docs from Wolang": "Wolang docs updated"},
        "quiet": True,
    },
    "mowogames/mowogames-website": {
        "name": "mowogames.com", "emoji": "🕸️", "site": "https://mowogames.com",
    },
    "mowogames/wolang": {
        "name": "Wolang", "emoji": "🐺", "site": "https://github.com/MowoGames/Wolang",
    },
    "mowogames/mowoprotection": {
        "name": "MowoProtection", "emoji": "🛡️", "site": "https://github.com/MowoGames/MowoProtection",
    },
}

# A card's colour is what happened, the same in every project: green went live or was released,
# blue is information (a push), amber needs a look (a digest with a rollback). Red is for
# failures, which are the CI channel's (notify.py), so a changelog card is never red.
GREEN, BLUE, AMBER = 0x57F287, 0x5865F2, 0xFEE75C

MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
          "October", "November", "December"]


def github(path, method="GET", body=None, **params):
    url = path if path.startswith("https://") else f"{GITHUB_API}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    tok = os.environ.get("GITHUB_TOKEN")
    if not tok:
        raise SystemExit("GITHUB_TOKEN is not set")
    req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body else None,
                                 headers={"Authorization": f"Bearer {tok}", "Accept": "application/vnd.github+json",
                                          "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "mowo-changelog"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def project():
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    p = PROJECTS.get(repo.lower())
    if not p:
        raise SystemExit(f"{repo or 'GITHUB_REPOSITORY'} is not in PROJECTS")
    return {"path": repo, **p}


_history = {}


def commit_count(path, sha):
    """`git rev-list --count <sha>` — the N of vX.Y.N."""
    if sha not in _history:
        owner, name = path.split("/")
        q = {"query": "query($o:String!,$n:String!,$s:GitObjectID!){repository(owner:$o,name:$n){object(oid:$s){... on Commit{history(first:0){totalCount}}}}}",
             "variables": {"o": owner, "n": name, "s": sha}}
        _history[sha] = github("/graphql", "POST", q)["data"]["repository"]["object"]["history"]["totalCount"]
    return _history[sha]


def all_deployments(p):
    """Successful production deployments, oldest first, as {sha, ref, at, user, rollback}.

    `at` is when the deployment succeeded (its success status). With `releases`, a rollback is a
    run of the workflow `deploy` with a version: its run title ends «(откат)» or «(rollback)»."""
    out = []
    for d in github(f"/repos/{p['path']}/deployments", environment="production", per_page=50):
        st = github(f"/repos/{p['path']}/deployments/{d['id']}/statuses", per_page=1)
        if not st or st[0]["state"] != "success":
            continue
        rollback = None
        run = (st[0].get("log_url") or st[0].get("target_url") or "")
        if p.get("releases") and "/actions/runs/" in run:
            run_id = run.split("/actions/runs/")[1].split("/")[0]
            title = github(f"/repos/{p['path']}/actions/runs/{run_id}").get("display_title", "")
            if title.endswith(("(откат)", "(rollback)")):
                rollback = title.split()[1]
        out.append({"sha": d["sha"], "ref": d["ref"], "at": st[0]["created_at"],
                    "user": (d.get("creator") or {}).get("login", "—"), "rollback": rollback})
    return sorted(out, key=lambda d: d["at"])


def changes(p, frm, to):
    """Commits between two commits as (title, sha), oldest first, without merges and bot noise;
    a collapsed line has no sha. Second value: whether any line is more than a collapsed one."""
    if not to or frm == to:
        return [], False
    if frm:
        try:
            commits = github(f"/repos/{p['path']}/compare/{frm}...{to}", per_page=250).get("commits", [])
        except urllib.error.HTTPError as err:
            # A force-push leaves `before` unknown or unrelated: say what the new head is
            print(f"compare {frm[:8]}...{to[:8]}: HTTP {err.code}; only the head commit is shown")
            commits = [github(f"/repos/{p['path']}/commits/{to}")]
    else:
        commits = [github(f"/repos/{p['path']}/commits/{to}")]
    # The branch's own history is its first parents. A pull request merged with a merge commit is
    # one line — its title, from the merge message — rather than every commit made on the way;
    # any other merge (a «Merge branch 'dev'» pull) shows the commits it brought in.
    by = {c["sha"]: c for c in commits}
    mainline, s = [], to
    while s in by:
        mainline.append(s)
        s = (by[s].get("parents") or [{}])[0].get("sha")
    on_main, entries = set(mainline), []
    for sha in reversed(mainline):
        c = by[sha]
        message = c["commit"]["message"].strip()
        title = message.split("\n")[0].strip()
        if len(c.get("parents", [])) < 2:
            entries.append((title, sha, None))
            continue
        pr = re.match(r"Merge pull request #(\d+) from ", title)
        if pr:
            body = [ln.strip() for ln in message.split("\n")[1:] if ln.strip()]
            entries.append((body[0] if body else title, sha, pr.group(1)))
            continue
        side, stack = [], [x["sha"] for x in c["parents"][1:]]
        while stack:
            x = stack.pop()
            if x in by and x not in on_main and x not in side:
                side.append(x)
                stack += [q["sha"] for q in by[x].get("parents", [])]
        for x in [k for k in by if k in side]:  # compare lists them oldest first
            if len(by[x].get("parents", [])) < 2:
                entries.append((by[x]["commit"]["message"].strip().split("\n")[0].strip(), x, None))

    lines, seen, collapsed = [], set(), []
    for title, sha, pr in entries:
        if title.startswith("Merge "):
            continue
        if any(title.startswith(d) for d in p.get("drop", [])):
            continue
        line = next((v for k, v in p.get("collapse", {}).items() if title.startswith(k)), None)
        if line:
            if (line, None, None) not in collapsed:
                collapsed.append((line, None, None))
            continue
        # a squash merge says «Title (#12)»: the number becomes the link
        squash = re.fullmatch(r"(.*\S)\s+\(#(\d+)\)", title)
        if squash:
            title, pr = squash.group(1), pr or squash.group(2)
        if title not in seen:
            seen.add(title)
            lines.append((title, sha, pr))
    return lines + collapsed, bool(lines)


def parse(ts):
    return datetime.fromisoformat(ts.replace("Z", "+00:00"))


def when(dt):
    d = dt.astimezone(TZ)
    return f"{d.day} {MONTHS[d.month - 1]}, {d:%H:%M}"


def version(p, dep):
    if dep.get("version"):
        return dep["version"]
    if dep.get("rollback"):
        return dep["rollback"]
    ref = dep.get("ref") or ""
    if p.get("releases") and ref.startswith("release/"):
        return f"v{ref.removeprefix('release/')}.{commit_count(p['path'], dep['sha'])}"
    return dep["sha"][:8]


# ── Cards ─────────────────────────────────────────────────────────────────────────────────────
# A card is {title, url, color, intro, heading, lines, more, empty, links, footer, timestamp}:
# plain text, escaped for each place it goes. `intro` may carry **bold**, the one markup both
# understand. A line is (title, sha) and links to its commit; `links` is the row under the lines
# — the repository, what changed, the run that posted it.

def short(t, n=150):
    return t if len(t) <= n else t[: n - 1].rstrip() + "…"


def repo_url(p):
    return f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/{p['path']}"


def card(p, title, intro="", lines=(), limit=MAX_LINES, heading="", empty="", footer="", at=None, links=(), color=GREEN,
         who=""):
    lines = list(lines)
    row = [("Repository", repo_url(p)), *links]
    if os.environ.get("GITHUB_RUN_ID"):
        row.append(("Run", f"{repo_url(p)}/actions/runs/{os.environ['GITHUB_RUN_ID']}"))
    return {"title": title, "url": p["site"], "color": color, "intro": intro, "heading": heading if lines else "",
            "lines": [(short(t), sha and f"{repo_url(p)}/commit/{sha}", pr and f"{repo_url(p)}/pull/{pr}")
                      for t, sha, pr in lines[:limit]],
            "more": max(0, len(lines) - limit), "empty": "" if lines else empty, "links": row,
            "footer": footer, "timestamp": at, "who": who if who and who != "—" else ""}


def compare_link(p, frm, to):
    return [("Changes", f"{repo_url(p)}/compare/{frm}...{to}")] if frm and to and frm != to else []


def discord_md(s):
    for ch in "\\*_~`|>[]":
        s = s.replace(ch, "\\" + ch)
    return s


def bold_parts(s):
    """'a **b** c' → [('a ', False), ('b', True), (' c', False)]."""
    parts = s.split("**")
    return [(t, i % 2 == 1) for i, t in enumerate(parts) if t]


def to_discord(c):
    intro = "".join(f"**{discord_md(t)}**" if b else discord_md(t) for t, b in bold_parts(c["intro"]))
    links = " · ".join(f"[{label}]({url})" for label, url in c["links"])
    body, size, more = [], len(intro) + len(links), c["more"]
    for i, (t, url, pr) in enumerate(c["lines"]):
        line = (f"• {discord_md(t)}" + (f" · [#{pr.rsplit('/', 1)[1]}]({pr})" if pr else "")
                + (f" · [`{url.rsplit('/', 1)[1][:7]}`]({url})" if url else ""))
        # an embed's description holds 4096 characters; a line is never cut through its link
        if size + len(line) > 3600:
            more += len(c["lines"]) - i
            break
        body.append(line)
        size += len(line) + 1
    if more:
        body.append(f"…and {more} more")
    text = "\n".join(body)
    desc = "\n\n".join(x for x in [intro, (f"**{c['heading']}**\n" if c["heading"] else "") + text, c["empty"], links] if x)
    em = {"title": c["title"][:256], "url": c["url"], "description": desc or "—", "color": c["color"]}
    if c["footer"]:
        em["footer"] = {"text": c["footer"][:2048]}
    if c["timestamp"]:
        em["timestamp"] = c["timestamp"]
    if c.get("who"):
        server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
        em["author"] = {"name": c["who"], "url": f"{server}/{c['who']}", "icon_url": f"{server}/{c['who']}.png?size=64"}
    return em


def to_telegram(c):
    e = lambda s: html.escape(s, quote=False)
    out = [f'<b><a href="{html.escape(c["url"])}">{e(c["title"])}</a></b>']
    if c["intro"]:
        out.append("".join(f"<b>{e(t)}</b>" if b else e(t) for t, b in bold_parts(c["intro"])))
    if c["heading"]:
        out.append(f"\n<b>{e(c['heading'])}</b>")
    body, size = [], sum(len(x) for x in out)
    for t, url, pr in c["lines"]:
        line = (f"• {e(t)}" + (f' · <a href="{html.escape(pr)}">#{pr.rsplit("/", 1)[1]}</a>' if pr else "")
                + (f' · <a href="{html.escape(url)}">{url.rsplit("/", 1)[1][:7]}</a>' if url else ""))
        if size + len(line) > 3500:
            c = {**c, "more": c["more"] + len(c["lines"]) - len(body)}
            break
        body.append(line)
        size += len(line) + 1
    if not c["heading"] and body:
        out.append("")
    out += body
    if c["more"]:
        out.append(f"…and {c['more']} more")
    if c["empty"]:
        out.append(f"\n{e(c['empty'])}")
    out.append("\n" + " · ".join(f'<a href="{html.escape(url)}">{e(label)}</a>' for label, url in c["links"]))
    if c["footer"]:
        out.append(f"<i>{e(c['footer'])}</i>")
    return "\n".join(out)


def _send(url, payload, what):
    body = json.dumps(payload).encode()
    for _ in range(5):
        req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json", "User-Agent": "mowo-changelog"})
        try:
            urllib.request.urlopen(req, timeout=30).close()
            return True
        except urllib.error.HTTPError as err:
            raw = err.read()
            if err.code == 429:
                try:
                    j = json.loads(raw)
                    wait = j.get("retry_after") or (j.get("parameters") or {}).get("retry_after") or 2
                except ValueError:
                    wait = 2
                time.sleep(float(wait) + 0.5)
                continue
            print(f"::warning::{what}: HTTP {err.code} {raw[:300]!r}")
            return False
        except (urllib.error.URLError, TimeoutError) as err:
            print(f"::warning::{what}: {err}")
            return False
    print(f"::warning::{what}: rate limited")
    return False


def post(cards, dry, globally=False):
    """Discord: up to 10 embeds and 6000 characters of embed text per message. Telegram: one
    message per card. `globally`: the Telegram copy goes to the global topic too."""
    if not cards:
        return
    hooks = os.environ.get("CHANGELOG_WEBHOOK_URL", "").split()
    tok, chat = os.environ.get("TELEGRAM_BOT_TOKEN", ""), os.environ.get("TELEGRAM_CHAT_ID", "")
    threads = [os.environ.get("TELEGRAM_THREAD_ID", "")]
    if globally and os.environ.get("TELEGRAM_GLOBAL_THREAD_ID"):
        threads.append(os.environ["TELEGRAM_GLOBAL_THREAD_ID"])

    messages, batch, size = [], [], 0
    for em in map(to_discord, cards):
        n = len(em["title"]) + len(em["description"]) + len(em.get("footer", {}).get("text", ""))
        if batch and (size + n > 5500 or len(batch) == 10):
            messages.append(batch)
            batch, size = [], 0
        batch.append(em)
        size += n
    messages.append(batch)

    if dry or not (hooks or (tok and chat)):
        for m in messages:
            print("── Discord ──\n" + json.dumps({"embeds": m}, ensure_ascii=False, indent=2))
        for c in cards:
            print(f"── Telegram (topics {', '.join(t or 'none' for t in threads)}) ──\n{to_telegram(c)}")
        return
    for hook in hooks:
        for m in messages:
            _send(hook, {"username": "Change Log", "allowed_mentions": {"parse": []}, "embeds": m}, "discord")
    if tok and chat:
        for c in cards:
            for thread in threads:
                msg = {"chat_id": chat, "text": to_telegram(c), "parse_mode": "HTML",
                       "link_preview_options": {"is_disabled": True}}
                # The forum's General topic takes no thread id; it refuses 1
                if thread and thread != "1":
                    msg["message_thread_id"] = int(thread)
                _send(f"https://api.telegram.org/bot{tok}/sendMessage", msg, "telegram")


# ── Modes ─────────────────────────────────────────────────────────────────────────────────────

def collect(p, dep, prev):
    """One deployment as an entry: release, rollback or update."""
    ref = dep.get("ref") or ""
    prev_ref = (prev or {}).get("ref") or ""
    prev_sha = (prev or {}).get("sha")
    kind = "update"
    if dep.get("rollback"):
        kind = "rollback"
    elif p.get("releases") and ref.startswith("release/") and ref != prev_ref:
        kind = "release"
    lines, news = ([], False) if kind == "rollback" or not prev_sha else changes(p, prev_sha, dep["sha"])
    return {"kind": kind, "sha": dep["sha"], "prev_sha": prev_sha, "version": version(p, dep),
            "prev_version": version(p, prev) if prev else None,
            "release": ref.removeprefix("release/") if ref.startswith("release/") else None,
            "user": dep.get("user", "—"), "at": dep["at"], "changes": lines, "news": news}


def release_card(p, e):
    lines = e["changes"]
    return card(p, f"🎉 {p['name']} {e['release']} is live", f"Version **{e['version']}** · {when(parse(e['at']))}",
                lines, 25, "What's new", "No new commits.",
                f"deployed by {e['user']} · changes: {len(lines)}" + (f" · was {e['prev_version']}" if e["prev_version"] else ""),
                e["at"], compare_link(p, e["prev_sha"], e["sha"]), who=e["user"])


def digest_card(p, es, day):
    lines, notes, seen = [], [], set()
    for e in es:
        if e["kind"] == "rollback":
            notes.append(f"↩️ Rolled back to {e['version']} ({when(parse(e['at']))}, {e['user']})")
        for t in e["changes"]:
            if t[0] not in seen:
                seen.add(t[0])
                lines.append(t)
    first, last = es[0], es[-1]
    title = f"{p['emoji']} {p['name']}"
    if first["prev_version"] and first["prev_version"] != last["version"]:
        title += f" · {first['prev_version']} → {last['version']}"
    else:
        title += f" · {last['version']}"
    users = sorted({e["user"] for e in es})
    return card(p, title, "\n".join(notes), lines, empty="" if notes else "Redeployed with no new commits.",
                color=AMBER if notes else GREEN,
                footer=f"{day.day} {MONTHS[day.month - 1]} · last 24 hours · deployments: {len(es)} · {', '.join(users)}",
                links=compare_link(p, first["prev_sha"], last["sha"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["release", "digest", "deploy", "push", "tag"])
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only-at-hour", type=int, help="digest: do nothing unless it is this hour in CHANGELOG_TZ "
                    "(the schedule is in UTC and runs at both 04:00 and 05:00, for summer and winter time)")
    ap.add_argument("--before", default="", help="push: the commit the branch was at")
    ap.add_argument("--after", default="", help="push: the commit it is at now")
    ap.add_argument("--from", dest="frm", default="", help="tag: the previous tag")
    ap.add_argument("--actor", default=os.environ.get("GITHUB_ACTOR", "—"))
    args = ap.parse_args()
    if args.only_at_hour is not None and datetime.now(TZ).hour != args.only_at_hour:
        print(f"not {args.only_at_hour}:00 in {TZ.key}; nothing to do")
        return
    p = project()
    now = datetime.now(timezone.utc)

    if args.mode == "push":
        before = "" if not args.before or set(args.before) == {"0"} else args.before
        lines, _ = changes(p, before, args.after)
        if not lines:
            print("nothing to post: no commits worth a line")
            return
        branch = os.environ.get("GITHUB_REF_NAME", "")
        n = len(lines)
        post([card(p, f"{p['emoji']} {p['name']} · {branch}", f"{n} change{'s' if n != 1 else ''} · pushed by **{args.actor}**",
                   lines, footer=f"{branch} · {(before or '—')[:8]} → {args.after[:8]}", at=now.isoformat(),
                   links=compare_link(p, before, args.after), color=BLUE, who=args.actor)], args.dry_run)
        return

    if args.mode == "tag":
        tag = os.environ.get("GITHUB_REF_NAME", "")
        sha = os.environ.get("GITHUB_SHA", "")
        lines, _ = changes(p, args.frm, sha) if args.frm else ([], False)
        post([card(p, f"🎉 {p['name']} {tag} released", f"Tagged by **{args.actor}** · {when(now)}", lines, 25,
                   f"Since {args.frm}" if args.frm else "", "First release." if not args.frm else "No new commits.",
                   f"changes: {len(lines)}", now.isoformat(),
                   [("Release", f"{repo_url(p)}/releases/tag/{tag}"), *compare_link(p, args.frm, tag)], who=args.actor)],
             args.dry_run, globally=True)
        return

    deps = all_deployments(p)

    if args.mode == "deploy":
        if not deps:
            return
        dep, prev = deps[-1], (deps[-2] if len(deps) > 1 else None)
        e = collect(p, dep, prev)
        if p.get("quiet") and not e["news"]:
            print(f"{e['version']}: {', '.join(t[0] for t in e['changes']) or 'no new commits'}; not posted")
            return
        post([card(p, f"{p['emoji']} {p['name']} is live", f"**{e['version']}** · deployed {when(parse(e['at']))}",
                   e["changes"], 25, "What's new", "Redeployed with no new commits.",
                   f"deployed by {e['user']}" + (f" · was {e['prev_version']}" if e["prev_version"] else ""), e["at"],
                   compare_link(p, e["prev_sha"], e["sha"]), who=e["user"])],
             args.dry_run)
        return

    if args.mode == "release":
        if not deps:
            return
        e = collect(p, deps[-1], deps[-2] if len(deps) > 1 else p.get("baseline"))
        if e["kind"] == "release":
            post([release_card(p, e)], args.dry_run, globally=True)
        else:
            print(f"{e['version']}: {e['kind']}, goes to the morning digest")
        return

    since = now - timedelta(hours=24)
    entries = []
    for i, dep in enumerate(deps):
        if parse(dep["at"]) <= since:
            continue
        e = collect(p, dep, deps[i - 1] if i > 0 else p.get("baseline"))
        if e["kind"] != "release":
            entries.append(e)
    if entries:
        post([digest_card(p, entries, now.astimezone(TZ).date())], args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
