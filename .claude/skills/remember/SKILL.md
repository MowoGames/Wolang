---
name: remember
description: Save a durable, non-obvious lesson about this repo to the committed project memory in .claude/memory/. Use when the user says "remember", or after discovering a trap a future session would hit again.
argument-hint: <the lesson>
---

# Remember

Lesson: $ARGUMENTS

Project memory is committed (cloud containers are ephemeral, so `~/.claude` memory does not survive).
`.claude/memory/MEMORY.md` is the index and is imported into every session by CLAUDE.md - keep it
under ~40 lines. Details go in topic files.

1. Pick the topic file: `gotchas.md` (build/tooling/test traps), `codebase.md` (non-obvious invariants,
   where things really live), `workflow.md` (how the owner wants work done). Create a new topic only
   if none fits, and add it to the index.
2. `rg -n -i '<keyword>' .claude/memory/` - if a similar entry exists, update it instead of adding.
3. Append one bullet: `- <fact>. <why it matters / what to do>. (YYYY-MM-DD)` - one or two lines.
   Facts, not narratives. No secrets, no copies of what STATUS.md, docs or git log already hold.
4. If the lesson changes how every session should behave, add a one-line pointer in MEMORY.md.
5. Remove entries the code has made false. Memory that is wrong is worse than none.
