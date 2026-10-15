# Debugging Wolang

A Wolang binary carries DWARF, so every debugger already knows where
every line is: `wolang build` writes the line information by default, and
`--no-debug` leaves it out.

```sh
wolang debug .              # build with line information, start a debugger
```

That is the whole of it: `wolang debug` builds the package with `-O0`,
finds lldb or gdb, and hands the binary over. From there it is an
ordinary native debugging session:

```
(lldb) b main.wo:12         # a breakpoint on a line of Wolang
(lldb) run
(lldb) next                 # one line at a time
(lldb) frame variable       # the locals of the frame
(lldb) bt                   # who called whom
```

Names are mangled as `wo$<file>$<name>`, so `b Steps` finds
`wo$0$Steps`; `b main.wo:12` is usually what you want anyway.

## In an editor

`editors/vscode/templates/` holds a `launch.json` and a `tasks.json`.
Copy them into `.vscode/`: the launch configurations use either CodeLLDB
or the C/C++ extension, and the build task is `wolang build -O0` with a
problem matcher that turns `wolang check`'s output into clickable
diagnostics.

There is no Wolang-specific debug adapter, and this is why: a debug
adapter's job is to put a debugger behind a protocol an editor speaks,
and the debuggers that already speak it understand DWARF. A Wolang
adapter would be a worse lldb.

## What a trap says

A program that traps names the file and the line, in both tiers:

```
wolang: out of bounds at app.wo:34:11 (@Update:%27): index is outside a
length of 16
```

The `@Update:%27` part is the instruction, which is what to grep the
output of `wolang ir` for when the line is not enough.

## Where the time goes

```sh
wolang run --profile .            # a thousand samples a second
wolang run --profile --hertz 4000 .
```

A sampling profiler, attributed to compiled functions by name --
including generic instantiations and closures, so `@Partition<s32>` and
`@closure$2` are what a profile of `Sort(xs, |a, b| a < b)` shows.
Samples that land outside the program's own code are reported as one
line, because "most of the time is in the allocator" is an answer.

## Making a failure happen again

```sh
wolang run --record run.journal .      # on the machine where it fails
wolang run --replay run.journal .      # anywhere
```

The clock, standard input and the process id are written down and handed
back in order, so a run that depended on any of them -- including a
random number seeded from the clock -- happens the same way again.
Threads are not part of it: they interleave as the machine decides.
