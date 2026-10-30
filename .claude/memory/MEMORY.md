# Project memory (index)

Committed, shared, loaded every session through CLAUDE.md. Keep this file under ~40 lines;
details live in the topic files. Add with the `remember` skill.

- [gotchas.md](gotchas.md) - build, tooling and test traps already paid for once.
- [codebase.md](codebase.md) - non-obvious invariants and where things really live.
- [workflow.md](workflow.md) - how the owner wants work done.

Always true:
- Build/test via `.claude/scripts/wl` (compact). Decisions via `.claude/scripts/decision`.
- A lone `.wo` file has no imports; run snippets as a package in `build/scratch/` (see gotchas).
- STATUS.md is the human resume point; this memory is only for agent-facing lessons.
