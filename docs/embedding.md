# Embedding Wolang

Wolang is a library before it is a tool. One header, a C ABI, and a
`libwolang_embed` to link: a program written in something else — an
engine, a server, an editor — gets Wolang as the language its users
write.

```c
#include "wolang/embed/wolang.h"

WolangVm* vm = wolang_vm_create();
wolang_vm_bind(vm, "host_log", (void*)&host_log);   // before loading
wolang_vm_load(vm, "scripts/enemy");               // a package or a file

WolangValue delta = wolang_float(1.0 / 60.0);
WolangValue out;
wolang_vm_call(vm, "Update", &delta, 1, &out);
```

Everything below is the same API with the ceremony removed, for C++:

```cpp
#include "wolang/embed/wolang.hpp"

wolang::Vm vm;
vm.Bind("host_log", &host_log);
if (!vm.LoadSource("enemy.wo", source)) {
  std::fputs(vm.errors().c_str(), stderr);
}
vm.SetGlobal("speed", 3.0);
const wolang::Function update = vm.Lookup("Update");
for (;;) {
  if (const auto answer = update(1.0f / 60.0f)) {
    Apply(answer->number());
  }
}
```

## Getting it

```sh
cmake --preset release
cmake --build --preset release -j
cmake --install build/release --prefix /usr/local
```

That lays out exactly what a host needs:

```
<prefix>/include/wolang/embed/wolang.h     the C API
<prefix>/include/wolang/embed/wolang.hpp   the C++ one, header only
<prefix>/lib/libwolang_embed.a             static
<prefix>/lib/libwolang_embed.dylib|so      shared
<prefix>/lib/std, http, game, core         the standard library's sources
<prefix>/lib/cmake/Wolang/                 find_package(Wolang CONFIG)
<prefix>/lib/pkgconfig/wolang-embed.pc     for everything else
<prefix>/bin/wolang                        the compiler, for `wolang new`
```

With CMake:

```cmake
find_package(Wolang CONFIG REQUIRED)
target_link_libraries(my_engine PRIVATE Wolang::embed)
# ${WOLANG_STD_DIR} is where the standard library's sources are.
```

Without CMake:

```sh
cc host.c $(pkg-config --cflags --libs wolang-embed) -o host
```

A release archive holds the same layout, so unpacking it beside a project
and pointing `CMAKE_PREFIX_PATH` at it is enough.

## What a host can do

| | |
| --- | --- |
| **Load** | `wolang_vm_load` takes a package directory or a `.wo` file; `wolang_vm_load_source` takes text under a name of your choosing. A second call adds to what is loaded. |
| **Call** | `wolang_vm_call` by name, or `wolang_vm_function` once and `wolang_call` per frame. The compiled tier runs what it can and the interpreter runs the rest, which is why calls are fast without asking. |
| **Ask** | `wolang_vm_function_count`/`_name` list what the package *offered*; `wolang_function_arity`, `_parameter`, `_result` and `_doc` say what one takes, answers and is for. |
| **Configure** | `wolang_vm_get_global`/`_set_global` read and write the program's module-level `var`s. A script's `var gravity: f32 = -9.8` is a setting the host owns. |
| **Extend** | `wolang_vm_bind` makes a host function reachable: the script declares `extern "C" fun host_log(message: *u8, length: u64)` and calls it. |
| **Capture** | `wolang_vm_set_writer` sends the program's output to the host's console instead of standard output; `wolang_vm_set_reporter` does the same for diagnostics. |
| **Contain** | `wolang_vm_set_step_limit` stops a call after so many instructions, for scripts the host did not write. It keeps calls on the interpreter, which is the trade. |
| **Reload** | `wolang_vm_reload` loads everything again; `wolang_vm_file_count`/`_name` say which files to watch. Handles do not survive a reload. |

## What crosses the boundary

Integers, floats, booleans and text. A string is a pointer and a length
in both directions — the host's bytes are borrowed for the length of a
call, and a string the program answered is copied out, because the
program's own copy may live in an arena that is about to be given back.

Objects cross too, in both directions — see below. Anything else richer
— a list, a struct, an enum with a payload — stays inside the language. A
function at the boundary takes and answers what C can carry, which is a
constraint worth designing the script's API around rather than working
past.

## The host's objects in a script

An engine's actor is the engine's: it makes it, owns it, and frees it.
A script names the type with `extern class` and gets methods and nothing
else:

```wolang
extern class Actor {
  fun Position() -> Vec3
  fun Hurt(amount: s32)
}

offer fun Tick(self_actor: Actor) uses unsafe {
  self_actor.Hurt(5)
}
```

The host binds each method as `Class_Method`, taking the object first,
and says how its objects are counted:

```c
wolang_vm_register_class(vm, "Actor", ActorRetain, ActorRelease, ActorAlive, engine);
wolang_vm_bind(vm, "Actor_Hurt", (void*)&Actor_Hurt);   // void Actor_Hurt(void* actor, int32_t amount)

WolangValue actor = wolang_handle(engine_actor);
wolang_vm_call(vm, "Tick", &actor, 1, NULL);
```

A script that keeps an `Actor` in a field calls `retain`, and `release`
when it lets go — the host decides what that means, a reference count of
its own or nothing at all. A `weak var` costs nothing and asks `alive`
when it is read, which is how a script holds an actor the level may
destroy: it reads `null` afterwards instead of a dangling pointer. An
object passed into a call is lent for the length of it. A script cannot
construct an `extern class` or give it fields (`E0139`).

## The program's objects in the host

A function that answers a class instance hands the host a
`kWolangObject`: `handle` is the object, `text` is its type. The host
holds a reference, and lets go of it with `wolang_vm_object_release` —
the last one runs the class's `deinit` and frees it, the same as inside
the language.

```c
WolangValue enemy;
wolang_vm_call(vm, "Spawn", &health, 1, &enemy);
wolang_vm_object_call(vm, enemy, "Hurt", &amount, 1, &left);
wolang_vm_object_get(vm, enemy, "health", &left);
wolang_vm_object_release(vm, enemy);
```

`wolang_vm_object_retain` takes another reference, for a host that keeps
the object in two places.

## What this is not

A sandbox. A Wolang program can declare `extern "C"` and reach anything
the process can, the same way the standard library does: `std.fs` is
`open` and `read`, and nothing stops a script from declaring them
itself. The step limit bounds *time*, not reach.

A host that runs code it does not trust should run it in a process it can
kill. Capability checks — `capabilities = ""` in a package's manifest —
are the language's own answer and they are not complete yet.

## Threads

A `WolangVm` is not thread-safe, and one per thread is the way to use
several: the program's module-level `var`s are process-wide, so two vms
holding the same program share them. A host that calls into scripts from
a job system should either serialize the calls or give each thread its
own vm and keep the shared state on the host's side.

## Hot reloading

```c
if (ChangedOnDisk(vm)) {
  wolang_vm_reload(vm);              // handles from before are invalid
  update = wolang_vm_function(vm, "Update");
}
```

`wolang_vm_file_count`/`_name` give the files the program was built from,
including the standard library's, so watching them is a host's own
business and needs nothing from here.

## Two files, no build system

A host that wants none of CMake, LLVM or a package manager can take the
whole compiler as one source file:

```sh
python3 tools/package/make_amalgamation.py       # build/amalgamation
```

```
wolang.h                  the C interface
wolang.hpp                the C++ one, over the same calls
wolang_amalgamation.cc    everything else, about two megabytes
```

```sh
clang++ -std=c++20 -fno-exceptions -fno-rtti -O2 \
    -c wolang_amalgamation.cc -o wolang.o
clang++ host.cc wolang.o -o host
```

That is the interpreter and the just-in-time tier — everything embedding
uses. The ahead-of-time backend is not in it, because it needs LLVM and
embedding does not.

What it cost to make this possible: half a dozen file-local names in the
compiler were too generic to share one translation unit — four different
`Entry` structs, two `State`s, two `Checker`s — and they now say which
file they are in. CI builds the amalgamation and runs a host against it
on every commit, so it cannot quietly stop working (D186).
