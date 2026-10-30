---
name: wolang-tiers
description: Debug a disagreement between the interpreter, the JIT (dev tier) and the LLVM AOT tier, or a fuzz failure. Use when `wl fuzz` or `wolang test` reports the tiers disagree.
---

# When the tiers disagree

The interpreter (`src/interp`) is the specification. A disagreement is a bug in the other tier
unless the interpreter contradicts `docs/spec.md`.

1. Reproduce: `wl fuzz <count> <seed>` with the reported seed; add `--source` via
   `wl wo fuzz --seed <s> --count 1 --source` to print the program.
2. Minimise into a scratch package (`wl wo new build/scratch/t`; a lone file has no imports), then compare:
   `wl wo run --interpret build/scratch/t` vs `wl wo run build/scratch/t` (JIT) vs
   `wl wo build build/scratch/t -o build/scratch/t.bin && build/scratch/t.bin` (AOT, needs LLVM).
3. Look at what each tier was given: `wl wo ir <path>` and `wl wo ir --unoptimized <path>`
   (difference => a pass in `src/ir/passes.cc`), `wl wo asm <path>` for the dev tier.
4. Locate: lowering `src/ir/lower.cc` (grep the construct, never read whole), passes `src/ir/passes.cc`,
   dev tier `src/codegen/{x64,arm64}/emitter.cc`, layout `src/abi/layout.cc`, AOT `src/codegen/compiler.cc`.
5. Fix, add the minimal program as a `WOLANG_TEST` (codegen/interp suite) that fails before the fix,
   then `wl test` and `wl fuzz 200` with two or three seeds.
