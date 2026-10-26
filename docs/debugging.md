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

The VS Code extension debugs Wolang itself: set a breakpoint in a `.wo`
file and press F5. Behind it is `wolang dap`, the Debug Adapter Protocol
over standard input and output, so any editor that speaks DAP can use it.

```jsonc
{
  "type": "wolang",
  "request": "launch",
  "name": "Wolang: run this package",
  "program": "${workspaceFolder}",
  "stopOnEntry": false
}
```

It runs the program on the interpreter, from the IR as lowering left it:
every local is where the source put it, and nothing the optimizer moved
or folded is missing. Breakpoints by line, step over, into and out,
pause, the call stack, and every local in scope with the fields of an
object or a struct a level at a time. The interpreter is the reference
tier and the compiled ones are checked against it (D88), so what the
program does under the debugger is what it does compiled — only slower.

### A script inside an engine

A host opens a port, and the editor attaches to it:

```c
wolang_vm_debug_listen(vm, 4711);    // 0 picks a free port and answers it
```

```jsonc
{ "type": "wolang", "request": "attach", "name": "Wolang: attach to the engine", "port": 4711 }
```

While an editor is attached every call into the vm runs on the
interpreter, and a breakpoint stops the thread that made the call — the
engine's frame waits while the script is looked at. When the editor
disconnects, breakpoints go with it and calls are compiled again (D216).

### Native debuggers

A compiled binary is still debugged the native way, for when what you
are after is the machine code. `editors/vscode/templates/` holds a
`launch.json` and a `tasks.json` for CodeLLDB or the C/C++ extension, and
the build task is `wolang build -O0` with a problem matcher that turns
`wolang check`'s output into clickable diagnostics.

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
