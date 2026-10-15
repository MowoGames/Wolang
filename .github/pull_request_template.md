## What this changes

<!-- One paragraph. What was wrong or missing, and what this does about it. -->

## How it was checked

- [ ] `ctest --preset debug`
- [ ] `wolang test lib/std` (and the other packages this touches)
- [ ] `wolang fuzz --count 200` — only needed when lowering, a back end
      or the runtime changed
- [ ] `wolang fmt --check lib examples`

## The test that would have caught it

<!-- Which test fails without this change. A fix without one needs a reason. -->

## Decisions

<!--
If this changes how something works rather than fixing it, add a numbered
entry to docs/decisions.md and point at it from the code.
-->
