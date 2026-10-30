---
name: wolang-decisions
description: Look up or add numbered design decisions (D1-D220+) in docs/decisions.md without loading the 160 KB file. Use when code cites (D123), when asking why something is the way it is, or when a change needs a new decision.
allowed-tools: Bash(.claude/scripts/decision:*)
---

# Decisions

`docs/decisions.md` is ~40k tokens. Never read it whole (a hook blocks it).

- One or more: `.claude/scripts/decision D73 D184`
- Which decision covers X: `.claude/scripts/decision -s 'soa|layout'` (titles of matching decisions)
- All titles: `.claude/scripts/decision -l` (~200 lines; prefer `-s`)
- Highest number in use: `.claude/scripts/decision -l | tail -1`

Adding one: append under the matching `##` section as
`**D<next>. <One-sentence claim.>** <Why, what was rejected, what it costs.>`
in the project's plain voice (see any neighbour). Cite it in code comments as `(D<n>)` and in
STATUS.md's current "Done since" list. Decisions are revisited only when something new is known.
