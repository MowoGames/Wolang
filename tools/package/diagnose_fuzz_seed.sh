#!/bin/sh
# diagnose_fuzz_seed.sh <wolang> <seed>: which tier and which function of
# a generated program hang. Each entry point is run on its own, in the
# interpreter and then in compiled code, under a time limit, and the
# machine code of the generated functions is printed for a disassembler.
# A diagnostic for CI on a system nobody here can run; it never fails.
W="$1"
SEED="$2"
work="$(mktemp -d)"
"$W" fuzz --seed "$SEED" --count 1 --source |
  sed '1d;/program.* checked/d;/^\[ /d' > "$work/p.wo"
echo "== fuzz --seed $SEED"
timeout 120 "$W" fuzz --seed "$SEED" --count 1; echo "rc $?"
for entry in "Body()" "f1()" "f2(3)" "f0(7, Cell(1, 2, 0.5))" "Probe()"; do
  name="$(echo "$entry" | sed 's/(.*//')"
  { cat "$work/p.wo"
    printf '\nfun main() uses unsafe {\n  const _r = %s\n}\n' "$entry"
  } > "$work/$name.wo"
  timeout 60 "$W" run --interpret "$work/$name.wo" > /dev/null 2>&1
  i=$?
  timeout 60 "$W" run "$work/$name.wo" > /dev/null 2>&1
  j=$?
  echo "$entry: interpreter rc $i, compiled rc $j"
done
echo "== machine code"
"$W" asm --raw "$work/Body.wo" 2>/dev/null |
  awk '/^@(f0|f1|f2|Body|Probe):/{p=1} /^@/&&!/^@(f0|f1|f2|Body|Probe):/{p=0} p'
exit 0
