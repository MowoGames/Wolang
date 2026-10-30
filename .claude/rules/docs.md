---
paths:
  - "docs/**"
  - "STATUS.md"
  - "README.md"
---

# Documentation

- Every fenced `wolang` block that has `fun main` is compiled by `tools/package/check_docs.py` as a
  package (std, http, db available). Run `wl docs` after editing one.
- The project voice is plain, concrete, first-principles English; short sentences, reasons over claims.
- docs/decisions.md: use `.claude/scripts/decision`; append new decisions, never renumber.
- STATUS.md is the resume point for humans: date line, "Done since", "Not done yet". Edit sections in place.
- docs/waves/ are closed historical reports; do not edit them.
