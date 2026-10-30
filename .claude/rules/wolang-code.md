---
paths:
  - "**/*.wo"
  - "lib/**/wolang.toml"
---

# Wolang sources

- Load the `wolang-lang` skill before writing Wolang if it is not already loaded.
- Edited `.wo` files are formatted by a hook (`wolang fmt --write`); a parse error comes back at once.
- `examples/errors.wo` and `examples/recovery.wo` are invalid on purpose - do not "fix" them.
- Library code keeps its `@Test`s in the same file; run `wl lib <pkg>`.
- Public library API changes -> `docs/stdlib.md` and the generated docs (`wl wo doc lib/<pkg> -o ...`).
