---
name: wolang-wrapup
description: Finish a piece of work - make docs, STATUS.md and project memory agree with the code, then commit. Use before every commit of a feature or fix.
---

# Wrap up

Wave discipline (docs/style.md): code, tests, `docs/`, and `STATUS.md` must agree at every commit.

1. User-visible language change -> `docs/spec.md` section, `docs/tour.md`/`docs/cookbook.md` example if
   it helps, `docs/diagnostics.md` for new codes, `docs/stdlib.md` for std API. Then `wl docs`.
2. New design decision -> `wolang-decisions` skill.
3. `STATUS.md`: update the date line, add the item to the current "Done since" list (with D-number),
   remove it from "Not done yet" if it was there. Edit only those lines; do not reread the whole file -
   `grep -n '^## ' STATUS.md` and read the section you change.
4. Memory: if you learned something a future session would otherwise rediscover (a trap, a slow path,
   a non-obvious invariant), use the `remember` skill. Do not record what git log or STATUS.md already say.
5. Commit: one line, Conventional Commits, imperative, lowercase, scope from
   `lexer parser sema ir codegen interp runtime std http gfx db tls lsp editors docs ci build tools tests`.
   Titles are posted publicly to Discord/Telegram on push to `alpha` - write them for people.
