# Claude Code setup for Wolang

This directory configures Claude Code for this repository. Everything here is
committed and works the same locally and in cloud sessions. It needs nothing
beyond what the project already uses: bash, py and clang.

## The model split

| Role | Model | Where it is set |
|---|---|---|
| Architecture and planning | Opus | `"model": "opusplan"` in `settings.json` (plan mode runs Opus, everything else Sonnet), plus the `architect` agent |
| Coding | Sonnet | main thread outside plan mode, the `coder` and `reviewer` agents, and any subagent with no model of its own (`CLAUDE_CODE_SUBAGENT_MODEL=sonnet`) |
| Research and running checks | Haiku | the `researcher` and `verifier` agents (the built-in `Explore` agent is Haiku too) |

To use Fable for architecture instead, change `model: opus` to `model: fable`
in `agents/architect.md`. Plan mode still runs Opus through `opusplan`. For
one session you can type `/model fable`.

## What is here

| Path | What it does |
|---|---|
| `../CLAUDE.md` | Loaded every turn, so it is kept short. It imports `memory/MEMORY.md`. |
| `settings.json` | Model, subagent default, output caps, permissions for build and read commands, hooks. |
| `scripts/wl` | Build, test, fuzz and check. Prints a one-line verdict or only the failing lines, and keeps full logs in `build/logs/`. |
| `scripts/decision` | Reads one numbered decision from `docs/decisions.md` (~40k tokens) instead of the whole file. |
| `hooks/guard_big_reads.py` | PreToolUse hook. Refuses whole-file reads of anything over 60 KB and says what to do instead. |
| `hooks/format_wo.sh` | PostToolUse hook. Runs `wolang fmt --write` on each `.wo` file that is edited and returns parse errors straight away. |
| `hooks/session_start.sh` | Prints three lines of orientation. In cloud sessions it starts the debug build in the background. |
| `agents/` | `architect`, `coder`, `researcher`, `verifier`, `reviewer`. |
| `skills/` | `wolang-lang` (writing Wolang), `wolang-build`, `wolang-decisions`, `wolang-diagnostic`, `wolang-tiers` (tier disagreements), `wolang-feature` (the full pipeline), `wolang-wrapup` (docs, STATUS and commit), `remember`. |
| `rules/` | Conventions for C++, `.wo` files and docs. Each loads only when a matching file is touched. |
| `memory/` | Project memory: an index plus topic files. It is committed because cloud containers do not keep `~/.claude`. |

Typical use: `/wolang-feature add strftime patterns to std.time`, or simply
describe the task and let the agents and skills be picked up.

## Where the savings come from

1. **Build and test output.** `wl test` turns about 720 lines into one. A
   failing build shows the first errors, not hundreds of lines of cascade.
2. **Giant files.** `lower.cc` alone is about 72k tokens. The read guard makes
   every agent grep first and then read a range.
3. **Decisions.** `decision D184` costs about 300 tokens. The full file costs
   about 40k.
4. **Delegation.** Logs and file exploration stay in Haiku and Sonnet
   subagents, and the main thread only sees their summaries.
5. **Lazy context.** Rules and skills load only when relevant. The always-on
   part, CLAUDE.md plus the memory index, is about 1k tokens.

## The tools that were evaluated

| Tool | Verdict | Why |
|---|---|---|
| [rtk-ai/rtk](https://github.com/rtk-ai/rtk) | **Recommended, opt in, per machine** | A single Rust binary with no daemon. It compresses the output of git, ls, grep and similar shell commands through a PreToolUse hook (it claims 60-90% on Bash output). `wl` already covers build and test output, so RTK adds the rest. It writes its hook into your user settings, which is why it is not committed here. |
| [claude-code-setup](https://claude.com/marketplace/plugins/claude-code-setup) (official) | Run once, optional | A read-only audit that recommends automations. This directory is the result of that kind of audit. |
| [thedotmack/claude-mem](https://github.com/thedotmack/claude-mem) | Not adopted | It needs a worker daemon, SQLite and Chroma, and an observer model that spends tokens summarising. By default it signs you up for a hosted sync. Its state disappears with a cloud container. Committed `memory/` gives the durable part at no cost. Use it locally if you want automatic capture. |
| [affaan-m/ECC](https://github.com/affaan-m/ECC) | Ideas taken, not installed | It has 71 agents and 314 skills, a large context surface that works against the goal. The agent roles, memory files and lean hooks here follow its patterns. If you want it anyway, use `--profile minimal`. |
| [headroomlabs-ai/headroom](https://github.com/headroomlabs-ai/headroom) | Optional, local | A compression proxy. Telemetry is on by default (`HEADROOM_TELEMETRY=off`) and it needs a running proxy, so it is not suited to cloud sessions. |
| [vagkaratzas/token-saviour](https://github.com/vagkaratzas/token-saviour) | Not needed | A skill that routes work to rtk, serena and similar tools. With RTK plus `wl` it adds little. |
| [rebelytics/one-skill-to-rule-them-all](https://github.com/rebelytics/one-skill-to-rule-them-all) | Not adopted | A meta-skill that watches other skills. It adds context on every task and only pays off with a large skill library. |
| [musistudio/claude-code-router](https://github.com/musistudio/claude-code-router), [OmniRoute](https://github.com/cheaper-inference/OmniRoute), [freellmapi](https://github.com/tashfeenahmed/freellmapi) | Not adopted | All three send requests to non-Claude or free-tier models through a local server. The native per-agent model setting already gives the Opus/Sonnet/Haiku split with no daemon. Free tiers are weak at compiler work, and your code would go to third parties. |
| agenticskills.io find-skills | Not evaluated | The site was not reachable from the session that wrote this. |

### Installing RTK (optional)

On your machine:

```sh
brew install rtk        # or: curl -fsSL https://raw.githubusercontent.com/rtk-ai/rtk/refs/heads/master/install.sh | sh
rtk init -g --dry-run   # look at what it will write first
rtk init -g
rtk gain                # what it has saved
```

For cloud sessions, add the install and `rtk init -g` to the environment's
setup script rather than to this repository. That way nothing in the repo
downloads and runs a binary on its own.

## Keeping it healthy

- After a session that discovered something, run `/remember <lesson>`.
  Delete memory entries that the code has made false.
- If a file grows past 60 KB, the guard starts covering it automatically.
- Keep CLAUDE.md under about 60 lines. Detail belongs in rules, skills or memory.
