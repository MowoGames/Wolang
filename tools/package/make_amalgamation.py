#!/usr/bin/env python3
"""Builds the whole compiler into one `.cc` and one header.

What this is for: a host that wants to embed Wolang and does not want
CMake, LLVM or a package manager. Two files go into their build, their
own compiler builds them, and `wolang.h` is the whole interface (D186).

    python3 tools/package/make_amalgamation.py            # build/amalgamation
    python3 tools/package/make_amalgamation.py --out dist

What comes out:

    wolang.h                  the C interface
    wolang.hpp                the C++ one, over the same calls
    wolang_amalgamation.cc    everything else

And how to use it:

    clang++ -std=c++20 -fno-exceptions -fno-rtti -O2 \\
        -c wolang_amalgamation.cc -o wolang.o
    clang++ host.cc wolang.o -o host

The ahead-of-time backend is left out: it needs LLVM, and embedding is
the interpreter and the just-in-time tier, neither of which does.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "include"
SRC = ROOT / "src"

# What the ahead-of-time backend needs and nothing else does.
SKIP = ("codegen/aot/",)

PROJECT = re.compile(r'^\s*#include\s+"([^"]+)"\s*$')
SYSTEM = re.compile(r"^\s*#include\s+<([^>]+)>\s*$")
PRAGMA_ONCE = re.compile(r"^\s*#pragma once\s*$")


def sources():
    held = []
    for path in sorted(SRC.rglob("*.cc")):
        relative = str(path.relative_to(SRC))
        if any(relative.startswith(skip) for skip in SKIP):
            continue
        held.append(path)
    return held


class Gatherer:
    """Pulls the project's headers in, each once, in the order they are
    first needed. A header guard is what stops a second copy; this keeps
    its own set as well, so the output has no guards left in it."""

    def __init__(self):
        self.seen = set()
        self.system = set()
        self.lines = []

    def header(self, name):
        # An `.inc` is an X-macro table: it is included several times on
        # purpose, with different macros defined each time, and taking
        # the second copy away leaves an empty table and a static
        # assertion that fails a long way from the cause (D186).
        once = not name.endswith(".inc")
        if once:
            if name in self.seen:
                return
            self.seen.add(name)
        path = INCLUDE / name
        if not path.exists():
            return
        self.lines.append("// ---- {} ----".format(name))
        self.file(path)

    @staticmethod
    def unguarded(text):
        """The file without its include guard.

        A guard is an `#ifndef` around the whole file, and leaving it in
        would make every include inside the header look conditional --
        which is how the first version of this hoisted nothing and left
        every include where the compiler could not find it."""
        lines = text.splitlines()
        first = None
        for index, line in enumerate(lines):
            if line.strip().startswith("#"):
                first = index
                break
            if line.strip() and not line.strip().startswith("//"):
                return lines
        if first is None:
            return lines
        guard = re.match(r"\s*#ifndef\s+(\w+)\s*$", lines[first])
        if guard is None:
            return lines
        if first + 1 >= len(lines):
            return lines
        defined = re.match(r"\s*#define\s+" + guard.group(1) + r"\s*$",
                           lines[first + 1])
        if defined is None:
            return lines
        held = lines[:first] + lines[first + 2:]
        for index in range(len(held) - 1, -1, -1):
            if held[index].strip().startswith("#endif"):
                del held[index]
                break
        return held

    def file(self, path):
        # How deep in `#if` this line is. An include inside one belongs
        # where it is -- `<windows.h>` hoisted to the top of a file
        # being built on macOS is a file that does not build.
        depth = 0
        for line in self.unguarded(path.read_text(encoding="utf-8")):
            stripped = line.strip()
            if stripped.startswith("#if"):
                depth += 1
            elif stripped.startswith("#endif"):
                depth = max(0, depth - 1)

            project = PROJECT.match(line)
            if project is not None:
                if depth > 0:
                    # Conditional: left as it is, and the header's guard
                    # is what stops a second copy.
                    self.lines.append(line)
                else:
                    self.header(project.group(1))
                continue
            system = SYSTEM.match(line)
            if system is not None:
                if depth > 0:
                    self.lines.append(line)
                else:
                    self.system.add(system.group(1))
                continue
            if PRAGMA_ONCE.match(line):
                continue
            self.lines.append(line)


def compiler_version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"VERSION\s+(\d+\.\d+\.\d+)", text).group(1)


def build(out):
    out.mkdir(parents=True, exist_ok=True)
    gatherer = Gatherer()

    # The public headers first, so that what a host includes is what the
    # amalgamation defines.
    gatherer.header("wolang/embed/wolang.h")
    for path in sources():
        gatherer.lines.append("")
        gatherer.lines.append("// ======== {} ========".format(
            path.relative_to(ROOT)))
        gatherer.file(path)

    head = [
        "// Wolang, as one file.",
        "//",
        "// Generated by tools/package/make_amalgamation.py -- do not",
        "// edit. What it is for and how to build it is in that file and",
        "// in docs/embedding.md (D186).",
        "",
        "#define WOLANG_AMALGAMATION 1",
        # The CMake build passes this on the command line (D248).
        "#ifndef WOLANG_VERSION_STRING",
        '#define WOLANG_VERSION_STRING "{}"'.format(compiler_version()),
        "#endif",
        "#ifndef WOLANG_WITH_LLVM",
        "#define WOLANG_WITH_LLVM 0",
        "#endif",
        "",
    ]
    head += ["#include <{}>".format(name)
             for name in sorted(gatherer.system)]
    head.append("")

    body = "\n".join(head + gatherer.lines) + "\n"
    (out / "wolang_amalgamation.cc").write_text(body, encoding="utf-8")

    for name in ("wolang.h", "wolang.hpp"):
        source = INCLUDE / "wolang" / "embed" / name
        if source.exists():
            (out / name).write_text(source.read_text(encoding="utf-8"),
                                    encoding="utf-8")

    (out / "README.md").write_text("""# Wolang, as two files

```
wolang.h                  the C interface
wolang.hpp                the C++ one, over the same calls
wolang_amalgamation.cc    everything else
```

```sh
clang++ -std=c++20 -fno-exceptions -fno-rtti -O2 \\\\
    -c wolang_amalgamation.cc -o wolang.o
clang++ host.cc wolang.o -o host
```

```c
#include <stdio.h>

#include "wolang.h"

int main(void) {
  WolangVm* vm = wolang_vm_create();
  // Zero is success, as in every other C interface. A function a host
  // calls has to be `offer`ed, like one another package calls.
  if (wolang_vm_load_source(
          vm, "host.wo",
          "offer fun Add(a: int, b: int) -> int => a + b\n") != 0) {
    fprintf(stderr, "%s", wolang_vm_errors(vm));
    return 1;
  }
  WolangValue args[2] = {wolang_int(2), wolang_int(40)};
  WolangValue out;
  wolang_vm_call(vm, "Add", args, 2, &out);
  printf("%lld\n", (long long)out.integer);   // 42
  wolang_vm_destroy(vm);
  return 0;
}
```

Built from the sources at the commit this was generated from. The
ahead-of-time backend is not in here -- it needs LLVM, and embedding
runs on the interpreter and the just-in-time tier.
""", encoding="utf-8")

    return out / "wolang_amalgamation.cc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "build" /
                                             "amalgamation"))
    arguments = parser.parse_args()
    target = build(Path(arguments.out))
    size = target.stat().st_size
    print("{} ({:.1f} MB)".format(target, size / (1024 * 1024)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
