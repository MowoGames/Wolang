#!/bin/sh
# diagnose_fuzz_seed.sh <wolang>: where fuzz seed 8 hangs. The program is
# checked in (generated on Linux), so nothing has to run to get it; each
# step has a hard time limit and says how it ended. `asm` only compiles,
# so it tells a hang in the compiler from one in the code it made.
# A diagnostic for CI on a system nobody here can run; it never fails.
W="$1"
here="$(dirname "$0")"
work="$(mktemp -d)"
limit() { timeout -k 5 "$@"; }
echo "== fuzz, stage by stage"
WOLANG_FUZZ_TRACE=1 limit 120 "$W" fuzz "$here/diag/seed8.wo"
echo "fuzz rc $?"
echo "== asm (compile only)"
limit 120 "$W" asm --raw "$here/diag/seed8.wo" > "$work/asm.txt" 2>&1
echo "asm rc $?"
awk '/^@(f0|f1|f2|Body|Probe):/{p=1} /^@/&&!/^@(f0|f1|f2|Body|Probe):/{p=0} p' "$work/asm.txt"
for entry in "f1()" "f2(3)" "Body()" "Probe()"; do
  name="$(echo "$entry" | sed 's/(.*//')"
  { cat "$here/diag/seed8.wo"
    printf '\nfun main() uses unsafe {\n  const _r = %s\n}\n' "$entry"
  } > "$work/$name.wo"
  limit 60 "$W" run --interpret "$work/$name.wo" > /dev/null 2>&1
  i=$?
  limit 60 "$W" run "$work/$name.wo" > /dev/null 2>&1
  j=$?
  echo "$entry: interpreter rc $i, compiled rc $j"
done
exit 0
