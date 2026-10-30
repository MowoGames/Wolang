# Codebase notes

- The interpreter (src/interp) is the reference semantics; the JIT (src/codegen x64/arm64) and LLVM AOT
  must match it. `wolang fuzz` compares them. (2026-10-30)
- `tools/wolang/main.cc` holds every driver command; grep `options.command == "<cmd>"`. (2026-10-30)
- Diagnostic code ranges per layer are in the table at the top of docs/diagnostics.md. (2026-10-30)
- Commit titles reach public Discord/Telegram channels on push to `alpha` (.github/changelog.py). (2026-10-30)
