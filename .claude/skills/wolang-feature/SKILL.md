---
name: wolang-feature
description: The model-tiered pipeline for a non-trivial change - architect (Opus) plans, coder (Sonnet) implements, verifier (Haiku) checks, reviewer (Sonnet) reviews, then docs/STATUS and memory. Use for any feature or fix spanning more than one file.
argument-hint: <what to build or fix>
---

# Feature pipeline

Task: $ARGUMENTS

Keep the main conversation as the coordinator: it holds plans and summaries, never file dumps or logs.

1. **Research** (skip if you already know): `researcher` agent - where it lives, governing decisions.
2. **Design**: `architect` agent with the task and the research summary. For small single-file
   fixes skip this and plan inline. Check the plan against `.claude/memory/` gotchas.
3. **Implement**: one `coder` agent per plan step (sequential if steps share files; parallel only for
   disjoint files). Give each the step text, the files, and the verification command.
4. **Verify**: `verifier` agent with "run wl ci" (or the narrower set the plan names).
   On failure send the failure summary back to a `coder`, not to yourself.
5. **Review**: `reviewer` agent on the diff. Fix real findings via `coder`.
6. **Close** with the `wolang-wrapup` skill: docs, STATUS.md, memory, commit.

Never let a subagent read docs/decisions.md or a >60 KB source whole; pass them D-numbers and
function names instead.
