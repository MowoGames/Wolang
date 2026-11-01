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
| **Debug** | `wolang_vm_debug_listen` opens a port an editor attaches to with the Debug Adapter Protocol; breakpoints stop the host's call where it is. See [debugging](debugging.md). |
| **Reload** | `wolang_vm_reload` loads everything again and keeps the program's state; `wolang_vm_file_count`/`_name` say which files to watch. |

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

## The ABI keeps its shape

`wolang.h` defines `WOLANG_ABI_VERSION` (major in the high 16 bits, minor in
the low) and the library answers `wolang_abi_version()`. A host compares the
two once at load, because the library it links is not always the one it was
compiled against: a different major is a different ABI, a larger minor only
adds (D241).

```c
if ((wolang_abi_version() >> 16) != (WOLANG_ABI_VERSION >> 16)) {
  return 1;  /* built against another ABI */
}
WolangVmOptions options = {sizeof(WolangVmOptions)};
WolangVm* vm = wolang_vm_create_with(&options);
```

- A `WolangValue` is 32 bytes: `kind` (a `uint32_t`), a reserved word, one
  union of `integer`, `number`, `boolean` or `handle`, then `text` and
  `length`. The member names are unchanged, so existing host code compiles.
  The size does not depend on the C compiler's choice of enum size.
- A library built with `wolang build --library actors` exports
  `actors_abi_version()` the same way, answering the same number.
- Kinds are only ever added; a host accepts a kind it does not know.
- A struct that will grow starts with its size. `WolangVmOptions` is the
  first; `wolang_vm_create_with` answers null for a size it does not know.
  `wolang_vm_create()` still works and means the defaults.
- Fallible calls answer a `WolangStatus`: `kWolangOk` (0), `kWolangError`
  (the call could not be made; `wolang_vm_errors` says why) or
  `kWolangTrap` (the program trapped). Code is only ever added, so treat an
  unknown one as failure. `!= 0` still means "failed".

## What a call costs

`benchmarks/actors` is a host ticking actors the way an engine does: a
`Tick(actor, delta)` per actor per frame that reads the actor's position
and velocity through its bindings, moves it, keeps it in bounds and
writes it back. The same logic in C++ goes through virtual calls. A
release build on Windows x64, a thousand actors:

| | a frame | against C++ |
| --- | --- | --- |
| C++ | 5.8 µs | 1× |
| Wolang, compiled ahead of time | 9.6 µs | 1.7× |
| Wolang, the dev tier | 63 µs | 11× |
| Wolang, the interpreter | 8.1 ms | 1400× |

Every column ends in the same state as the C++ one. Ahead of time is what
a shipped game runs: `wolang build --library actors` writes an object with
no `main`, an `actors_start()` to call once, and `actors_Tick` to call
like any C function. The dev tier is what an editor runs, with hot
reloading; a call through a `WolangFunction` handle goes straight into
compiled code when every argument is a number, a `bool` or an object. The
interpreter is what runs under a debugger. `wolang_actor_bench` is built
with everything else, and prints the table (D219).

## Bindings from an engine's reflection

An engine with reflection already knows its classes, so nobody should
write `extern class` declarations by hand. `wolang bind --reflection`
reads the JSON a header tool writes (MowoEngine's `*.reflection.json`)
and writes both halves:

```sh
wolang bind --reflection --effect World     Intermediate/Build/Generated/Debug/MowoEngine.*/Reflection/*.json     -o scripts/mowo/src/mowo.wo --glue Source/Scripting/MowoWolangBindings.cpp
```

- **`mowo.wo`** — an `offer extern class` per reflected class, with its
  base, its methods, and a `Get`/`Set` pair per property. A method that
  takes or answers a `Vec3`, a `Quat`, a `Rotator` or a string has a raw
  form the host answers and a method in an `extend` block that scripts
  call: `actor.GetActorLocation()` answers a `Vec3`. A string comes back as
  an owned copy of what the engine held, and a reflected struct whose
  fields are all numbers is answered the way a vector is. A property of
  an object type (`X*` or `TObjectPtr<X>`) or of `std::string` has its
  `Get`/`Set` pair like any other.
- **Statics** — an engine `static` function is a free `offer fun` in
  the same package, named after the function; two classes that both have
  a `Count` get `ActorCount` and `WorldCount`, the class name without its
  one-letter engine prefix first (D262).
- **The glue** — an `extern "C"` function per method and accessor, and
  `MowoWolang_Bind_mowo(vm)`, which registers every class and binds every
  function. A null object, or one `MowoWolangAlive` says the engine has
  destroyed, answers zero rather than crashing; `--no-alive-check` leaves
  the second test out (D262). A property
  is reached by its offset, asked for once from the engine's own
  reflection through `MowoWolangPropertyOffset(class, key)`.

The engine supplies four functions the glue declares: that offset, and
`MowoWolangRetain`/`Release`/`Alive` for its objects' lifetimes. With
`--effect World` every engine call carries `uses World`, so a mod has to
be granted `World` to touch the world at all.

What is left out is printed, one line each: components (which belong on
the ECS side), properties and functions of types with no binding, structs
with a field that is not a number, a struct taken as a parameter, and
overloads (D218, D262).

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

## Mods

A script the host wrote is trusted: it can declare `extern "C"` and reach
anything the process can, the same way the standard library does. A mod
is loaded differently:

```c
wolang_vm_load(vm, "scripts/engine_api");          // the host's own, trusted
wolang_vm_load_mod(vm, "mods/better_turrets", "World, Audio");
```

The mod is granted those capabilities and nothing else, whatever its own
manifest says. It may not use `unsafe` or declare `extern` (`E0705`), so
the only doors out are the ones the trusted packages offer — and each of
those states the capability it needs, down to the standard library's
`std.fs.Open`, which is `io`. A mod that reaches past its grant does not
load: `E0703` names the function and the capability (D217).

The grant bounds *reach*. `wolang_vm_set_step_limit` bounds *time*, for a
mod that loops forever; together they are what a mod sandbox is. Memory a
mod allocates is not bounded, and a host that runs mods from strangers
should still give them a vm of their own.

## Threads

Any thread may call into a `WolangVm`, and a vm takes one call at a time:
a second thread waits for the first to come back. A call that reaches a
host function which calls into the same vm again is the same thread and
goes straight through. That is the whole contract (D214), and it gives
an engine two ways to run scripts from a job system:

- **One vm per worker.** Each worker loads the program into its own vm,
  and calls run in parallel. A module-level `var` belongs to its vm, so
  state the workers share lives on the host's side — in the engine's
  components, which is where it was going to be anyway.
- **One vm, many workers.** Every job calls the same vm and the calls
  take turns. Simple, and right for scripts that are cheap next to what
  the engine does around them.

The counts on the program's objects are atomic, so an object one vm
handed to the host can be retained and released from any thread. The
callbacks an `extern class` registers are called on whichever thread is
running the script at the time; an engine whose own counting is not
thread-safe should keep those calls on the thread it counts on.

## Hot reloading

```c
if (ChangedOnDisk(vm)) {
  wolang_vm_reload(vm);              // `update` still works, and runs the new code
  Log(wolang_vm_notes(vm));          // what had to start again, if anything
}
```

A reload keeps what the program had. Every module-level `var` the new
program still declares with the same type keeps its value, and that
includes objects: a `var player: Player` is the same player, with the
health it had. When `Player` gained a field, the player keeps the fields
that are still there, and the new one starts from what the initializer
gave it. A `var` or a field whose type changed starts again from its
initializer, and `wolang_vm_notes` has a line saying so.

Function handles from `wolang_vm_function` survive a reload and call the
new code. The program as it was is kept until the vm is destroyed —
values carried over may still point into it — so a session that reloads
a thousand times uses a thousand programs' worth of memory, which is the
trade for never having a dangling string.

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
