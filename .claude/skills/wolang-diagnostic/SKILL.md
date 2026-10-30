---
name: wolang-diagnostic
description: Add or change a compiler diagnostic (E/W/P/H code) end to end. Use when a change emits a new error or warning.
---

# A new diagnostic

1. Pick the code from the owner's range in `docs/diagnostics.md` ("Code ranges" table; `grep -n 'E01' docs/diagnostics.md`).
   Codes never change meaning and are never reused.
2. Emit it where the layer reports: `diags.Error("E0xxx", "message")` (see `include/wolang/base/diagnostic.h`).
   Messages are lowercase, say what is wrong in the user's terms; add a quick fix if the fix is mechanical.
3. Two tests in `tests/unit/<layer>/`: one program where it fires, one similar correct program where it
   does not. Assert by **code**, never message text.
4. Document it in `docs/diagnostics.md` in the right table, with an example.
5. `wl test <Suite>` then `wl test`; `wl docs` if you added code blocks.
