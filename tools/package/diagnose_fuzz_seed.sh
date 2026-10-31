#!/bin/sh
# diagnose_fuzz_seed.sh <build dir>: how generating fuzz seed 8 ends on a
# system nobody here can run. The exit code says which kind of end it is
# (3 an abort, 0xC0000005 an access violation, 0xC0000409 a fast fail,
# 0xC00000FD a stack overflow). It never fails the job.
B="$1"
limit() { timeout -k 5 "$@"; }
limit 120 "$B/tests/wolang_tests.exe" --filter FuzzTest.ASeedWrites --verbose
code=$?
printf 'wolang_tests exit %d (0x%X)\n' "$code" "$code"
limit 120 "$B/tools/wolang/wolang.exe" fuzz --seed 8 --count 1
code=$?
printf 'wolang fuzz --seed 8 exit %d (0x%X)\n' "$code" "$code"
exit 0
