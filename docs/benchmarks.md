# Benchmarks

What a script call costs when an engine makes it every frame, in each tier,
next to the same logic written in C++. The numbers are here so that an engine
can budget scripts per actor before it writes them (D273).

## What is measured

A host ticks a crowd of actors once a frame, the way a game does. The script's
`Tick` moves an actor, keeps it in bounds and wears its health down, through
the same calls that `wolang bind --reflection` writes for an engine's actor
class: two reads, two writes and a health round trip, each crossing from the
script into C++. The C++ column is the same logic behind virtual calls.

Four tiers run it:

| Tier | What it is |
|---|---|
| c++ | The logic written directly, behind virtual calls. The yardstick. |
| aot | The script compiled ahead of time and linked into the host: what a shipped game runs. |
| jit | The script compiled when the program loads: what an editor runs. |
| interp | The reference interpreter. It exists to be correct, not fast. |

Each number is the mean of 200 frames (4 or 20 for the interpreter, which is
slow), after one frame to warm up. The host checks that every tier ends with
the same actor positions and health, and the column `agree` says so.

## The numbers

An optimized build (`windows-release`, RelWithDebInfo, clang-cl 23.1.3), on a
12-core AMD Ryzen 9 7900X with Windows 11. Microseconds a frame takes, and how
many times slower than C++ that is:

| Actors | c++ | aot | jit | interp |
|---:|---:|---:|---:|---:|
| 100 | 0.7 | 1.0 (1.5x) | 6.9 (10.5x) | 1022 (1560x) |
| 500 | 3.3 | 5.0 (1.5x) | 34.7 (10.4x) | 4959 (1482x) |
| 1000 | 6.6 | 9.7 (1.5x) | 68.8 (10.4x) | 9941 (1503x) |
| 5000 | 33.2 | 49.1 (1.5x) | 350.1 (10.6x) | 50678 (1527x) |

Per call, at 1000 actors a frame:

| Tier | Calls per millisecond | Objects allocated per call |
|---|---:|---:|
| c++ | 151229 | 0 |
| aot | 102738 | 0 |
| jit | 14527 | 0 |
| interp | 101 | 0 |

A frame at 60 frames a second is 16.7 milliseconds. A game that gives its
scripts a tenth of that, 1.7 milliseconds, can make about 170 thousand calls
like this one from ahead-of-time code, 24 thousand from the JIT, and 170 from
the interpreter.

`Tick` allocates no objects: it works on structs held on the stack. A call that
builds a `class` instance, an interpolated string or a closure allocates, and
`@Hot` refuses those (D237). The counter that makes "objects per call" a
measurement is `wolang_objects_allocated`, which counts every object ever made,
freed or not.

## Reading them

- The call count is dominated by the crossing, not the arithmetic. Every
  `target.GetHealth()` in the script is a call into the host. A binding that
  hands over a struct in one call is cheaper than three that hand over a float.
- The JIT tier is about ten times slower than the C++ it is compared with, and
  AOT one and a half. If a script is hot, compile it ahead of time.
- The interpreter is a reference. Use it to check the other tiers, not to run
  a game.

## Running it

```sh
cmake --preset release
cmake --build --preset release
build/release/wolang_actor_bench benchmarks/actors/actors.wo
```

or `.claude/scripts/wl bench [preset]` from a checkout. On Windows use the
`windows-release` preset. Time an optimized build: a debug build measures the
assertions.

The numbers move with the machine. These were taken on one, once warm, with
other work running, so read the ratios and not the last digit. The table is
re-measured when the runtime or a back end changes in a way that should move it.

The gameplay microbenchmarks in `benchmarks/gameplay.wo` against C++ are a
separate set: `benchmarks/run.sh` prints them.
