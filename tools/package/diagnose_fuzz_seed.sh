#!/bin/sh
# diagnose_fuzz_seed.sh <build dir>: where generating fuzz seed 8 aborts on
# a system nobody here can run: the generator is run under the debugger
# Windows ships, which stops at `abort` and prints the stack. It never
# fails the job.
B="$1"
cdb=""
for d in "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64" \
         "/c/Program Files/Windows Kits/10/Debuggers/x64"; do
  [ -x "$d/cdb.exe" ] && cdb="$d/cdb.exe" && break
done
if [ -z "$cdb" ]; then
  echo "no cdb.exe on this machine"
  find /c/Program\ Files* -maxdepth 6 -iname cdb.exe 2>/dev/null | head -3
  exit 0
fi
echo "== $cdb"
timeout -k 5 300 "$cdb" -lines -c \
  ".symopt+0x40; bu ucrtbased!abort; bu ucrtbase!abort; g; .echo STACK; kn 40; q" \
  "$B/tools/wolang/wolang.exe" fuzz --seed 8 --count 1 2>&1 |
  sed -n '/STACK/,$p' | head -60
exit 0
