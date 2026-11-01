# What Wolang still needs to be good inside MowoEngine

Source: engine integration milestones 1 and 2 (branch `lane70-wolang`, guide
`Engine/Documentation/Internal/programming/wolang-integration.mdx`, section
"Waiting on Wolang"). Items 1-25 were each reproduced by the engine team; the
"Suggested additions" section is the engine side's judgment of what a game
language needs and has NOT been checked against the Wolang repo. This copy was
pasted in on 2026-10-31; [1.2.0-status.md](1.2.0-status.md) sorts it into
releases.

## A. Host interop (blocks scripts from doing real work)

1. A body-less `extern class` method cannot be called from another package (`offer extern class AActor { fun SetActorHealth(value: f32) uses World }` then `a.SetActorHealth(5.0)` fails with "no body").
4. `wolang_function_parameter` / `_result` return `kWolangVoid` for `class` and `extern class` parameters (no `kWolangObject` / `kWolangHandle` in `KindOf`).
17. A program object as a `wolang_call` argument: "undefined value" in the interpreter, a crash in compiled mode when a text argument is next to it.
18. The host cannot construct a script object, has no `object_set`, and has no reflection of classes and attributes. The engine scans `.wo` sources to find them.
25. Bound functions cannot return structs (`MPerformanceStatics.GetSnapshot` is skipped by the generator).
15. The binding generator skips 52 functions that return `std::string` (actor, scene and save names, input, all of `MStringStatics`, string properties). Text returns are needed.
14. Statics are unreachable: engine "static" APIs are `MFUNCTION`s on `*Statics` classes called through the class default object, which a script cannot obtain.
13. The generator builds `Quat` as `Quat(w, x, y, z)` (`w_first`). The engine constructor is `Quat(x, y, z, w)`, so Mowo needs `w_first = false`.
16. No binding for `FRotator`, `Quat` properties and `TObjectPtr<...>`.

## B. Object lifetime and safety

2. A module-level `weak var` behaves as a strong reference: assignment retains, `alive` is never consulted on read, and after `alive = false` a `!= null` check is still true.
9. `alive` is not protected against address reuse (ABA). Proposal: a capture callback on `weak var` assignment (the host returns an index + serial token), or a user word in the handle.
10. The glue checks only `self == null`: an object the script holds strongly that the host destroyed is dereferenced. Proposal: an optional `MowoWolangAlive(self)` hook before every call.
3. `wolang_vm_destroy` releases nothing: module-level `var`s holding host objects never get a release (`refs after destroy 2`).
19. A subclass of an `extern class` compiles but its instance is not a host object, and an upcast to the extern type also compiles. Unsafe.

## C. Hot reload (a daily workflow in an editor)

6. An object the host holds does not migrate on reload: after a class gains a field, `Steps(mover)` returns 0 instead of 5 and the new code reads the object with the new layout.
7. A failed reload leaves the VM with no program: every call fails until the next successful edit. Keep running the old program instead.

## D. VM and embedding API

5. `wolang_vm_set_writer` does not intercept `std.io.Print` (it writes to stdout through `wolang_fd_write`), so script output never reaches the engine log.
8. `wolang_vm_bind`, the writer/reporter and `register_class` are process-wide: the `vm` parameter is ignored, so two VMs cannot have different bindings or callbacks.
11. A single `.wo` in `wolang_vm_load` does not resolve `need`, although the docs promise "package or file".
12. `wolang_vm_function` is one flat namespace: module names are ignored, so two packages with `Tick` conflict (`sample.mover.Tick`).

## E. Language features

20. A type alias from another package (`MeshAsset = text`) is not equal to `text`.
21. `extend` of a foreign class does not see its fields.
22. `if const t = x` does not fire for `AActor?`, and `?.` with a method call does not compile.
23. `after` in `machine` is not implemented, and there is no host hook for the game clock (pause, slow motion).
24. A false "unused import" warning for a base class.

## F. Environment (not a Wolang bug)

On Linux, AOT calls `cc`. In a container without `cc` four `AotTest`s fail with `<linker failed>`; with `cc` linked to `clang-22` they pass.

## Suggested additions for a game language (engine-side judgment, unverified)

These are what the first game (2-5 player co-op, server-authoritative, listen-server) will need beyond fixing the bugs above:

- **Replication attributes.** `replicated` fields, `server`/`client`/`multicast` functions and owner-only conditions that map to the engine's `MNET_CONDITION` and RPC policy. Without them, scripts cannot be used for networked gameplay.
- **Debugging.** A debug protocol (breakpoints, step, locals, call stack) that the engine's existing breakpoint UI can drive, and runtime errors with source spans and script stack traces delivered to the editor Message Log.
- **Execution budget.** A per-frame instruction or time budget with an infinite-loop guard, and a way to report a runaway script instead of hanging the game thread (the engine has a hang watchdog, but a script should fail cleanly first).
- **Suspending code.** Coroutines or `await` for "wait N seconds", "wait for event" and "wait for next frame", tied to the game clock (item 23), so gameplay logic is not written as state machines.
- **Serialization.** A supported way to save and restore script object state for save games and for hot-reload migration (item 6).
- **Stable binding ABI.** Versioned generated bindings and a clear compatibility rule, so a new Wolang version does not silently change struct layouts the engine relies on (items 13, 16).
- **Tooling.** Language-server basics: diagnostics, completion, go-to-definition and rename, and a formatter, so the Visual Studio/Rider/VS Code story in the engine roadmap covers Wolang too.
- **Mod sandboxing.** A capability model (the `uses World` clause is a good start) that can deny file, network and process access to script mods.
- **Shipping.** AOT on all three desktop platforms with no dependence on a system `cc`, and a deterministic build for reproducible packages.
- **Performance evidence.** A published benchmark of interpreter and compiled modes for per-frame script calls (calls per millisecond, allocation per call), so the engine can budget scripts per actor.
