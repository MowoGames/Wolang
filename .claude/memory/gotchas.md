# Gotchas

- `cmake --preset debug` refuses gcc; needs `CC=clang CXX=clang++` (D184). `wl` sets them. (2026-10-30)
- A single `.wo` file resolves no imports: `need std.io.{Print}` -> E0055/E0103. Use
  `wl wo new build/scratch/<name>` (inside the repo, so std is found) and run the directory. (2026-10-30)
- `wolang fmt` prints to stdout by default; `--write` rewrites in place, `--check` for CI. (2026-10-30)
- C++ sources are not clang-format clean (main.cc differs by ~500 lines); formatting a whole file
  makes a noisy diff. Format only lines you wrote, or match by hand. (2026-10-30)
- Test harness: per-check failures go to stderr, `[   failed ]` lines to stdout (buffered), so
  in a combined log they are not adjacent. `wl test` lists them separately. (2026-10-30)
- Clean debug build ~40 s on 4 cores; full unit suite (716 tests) a few seconds. (2026-10-30)
