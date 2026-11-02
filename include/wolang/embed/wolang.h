// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Wolang, embedded: one header, a C ABI, and no C++ in sight.
//
// What this is for: a program written in something else -- an engine, a
// server, an editor -- that wants Wolang as the language its users
// write. Load a package, call a function, hand some of your own
// functions back in. The compiler is a library; this is the part of it
// that does not need a C++ compiler to call.
//
// What it is not: a sandbox. A Wolang program can declare `extern "C"`
// and reach anything the process can, the same way the standard library
// does. Capability checks are the language's own answer to that and they
// are not complete yet, so a host that runs code it does not trust
// should run it in a process it can kill.

#ifndef WOLANG_EMBED_WOLANG_H_
#define WOLANG_EMBED_WOLANG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------- version ---

// What this header was written against: major in the high half, minor in
// the low. A host compares it with `wolang_abi_version()` at load, since
// the library it links may not be the one it compiled against (D241). A
// different major is a different ABI; a larger minor only adds.
#define WOLANG_ABI_VERSION ((1u << 16) | 7u)

uint32_t wolang_abi_version(void);

// How a call went. Zero is success; the codes are only ever added to, so a
// host treats one it does not know as a failure.
typedef enum WolangStatus {
  kWolangOk = 0,
  // The call could not be made or the load had errors;
  // `wolang_vm_errors` says which.
  kWolangError = 1,
  // The program was running and trapped.
  kWolangTrap = 2,
} WolangStatus;

// ---------------------------------------------------------------- values ---

// What kind of thing a value is. A host passes and receives these;
// anything richer -- a class, a list, an enum with a payload -- stays
// inside the language, and a function at the boundary takes and answers
// what C can carry.
typedef enum WolangKind {
  kWolangVoid = 0,
  kWolangInt = 1,
  kWolangFloat = 2,
  kWolangBool = 3,
  // Bytes and a length, not a C string: a Wolang `text` is a view with a
  // length, and a host that has one of those should not have to add a
  // zero to pass it.
  kWolangText = 4,
  // An object of the host's, of a type the script declared as
  // `extern class` (D213).
  kWolangHandle = 5,
  // One of the program's own objects: a class instance a function handed
  // back. `handle` is the object, `text` is its type's name. The host
  // holds a reference it lets go of with `wolang_vm_object_release` (D213).
  kWolangObject = 6,
} WolangKind;

// A kind, a reserved word, and one of the scalar or handle beside a text
// and its length: 32 bytes, the same on every compiler (D241). The member
// names are the ones the first header used, so host source compiles
// unchanged. `kind` is a `uint32_t` holding a `WolangKind`, because the
// size of an enum is the C compiler's choice. Kinds are only ever added:
// a host must accept one it does not know. `reserved` is zero except in a
// `kWolangObject`, where it is the program's id for the object's type.
typedef struct WolangValue {
  uint32_t kind;
  uint32_t reserved;
  union {
    int64_t integer;
    double number;
    int32_t boolean;
    void* handle;
  };
  const char* text;
  uint64_t length;
} WolangValue;

// The five constructors, so a caller writes `wolang_int(7)` rather than a
// designated initializer with five fields.
WolangValue wolang_void(void);
WolangValue wolang_int(int64_t value);
WolangValue wolang_float(double value);
WolangValue wolang_bool(int32_t value);
WolangValue wolang_text(const char* bytes, uint64_t length);
WolangValue wolang_handle(void* object);

// ------------------------------------------------------------------- vm ---

// One loaded program, with everything it needs to run: the sources, the
// types, the lowered code, and the machine code where there is any.
typedef struct WolangVm WolangVm;

WolangVm* wolang_vm_create(void);

// Options for `wolang_vm_create_with`. A struct that will grow starts with
// its own size, so a library can tell which fields a host knew about
// (D241): set |size| to `sizeof(WolangVmOptions)`. Null when |size| is
// not one this library understands.
typedef struct WolangVmOptions {
  uint32_t size;
} WolangVmOptions;

WolangVm* wolang_vm_create_with(const WolangVmOptions* options);
void wolang_vm_destroy(WolangVm* vm);

// Generated bindings call these first, with the ABI they were generated for
// and the layout of every struct they pass by pointer: the struct's size,
// its alignment, then the offset of each field, as the host's C++ compiler
// lays it out. A binding the library cannot speak, or a struct the compiler
// lays out differently, fails the load with a message naming both, rather
// than reading garbage (D271). Added in ABI 1.4.
WolangStatus wolang_vm_expect_abi(WolangVm* vm, uint32_t version);
WolangStatus wolang_vm_expect_struct(WolangVm* vm, const char* name, const uint32_t* layout, int32_t count);

// Loads a package directory -- one with a `wolang.toml` in it -- or a
// single `.wo` file. Zero on success; anything else means the program has
// errors, which `wolang_vm_errors` spells out.
//
// A second call adds to what is loaded rather than replacing it, which is
// how a host loads its own standard library and then a user's script.
WolangStatus wolang_vm_load(WolangVm* vm, const char* path);

// Loads source text under a name of the host's choosing.
//
// The name is what diagnostics and backtraces will say, so it should look
// like a path even when there is no file.
WolangStatus wolang_vm_load_source(WolangVm* vm,
                              const char* name,
                              const char* source);

// Loads a package the host holds as data, under the name its scripts `need`
// (`mowo`, `std`, `game`). |paths| and |sources| are |count| files; a path is
// below the package root, `src/ui/button.wo`, and belongs to the module
// `<package_name>.ui` as it would on disk. |manifest_toml| is a manifest, or
// null for a package that depends on everything loaded so far, as a source
// handed to `wolang_vm_load_source` does. A manifest is trusted like one on
// disk; its dependencies are found by name among the packages loaded before,
// so load `std` first and write `std = { path = "." }` (the path is a
// placeholder). A reload loads the package again from the same data.
// Refused: the name `host`, a name already loaded, a manifest naming another
// package, a dependency not loaded. Added in ABI 1.7 (D293).
WolangStatus wolang_vm_load_package_source(WolangVm* vm,
                                           const char* package_name,
                                           const char* manifest_toml,
                                           const char* const* paths,
                                           const char* const* sources,
                                           int32_t count);

// Every diagnostic from the last load, rendered the way the command-line
// compiler renders them. Owned by the vm; valid until the next call.
const char* wolang_vm_errors(const WolangVm* vm);

// Whether the program as loaded has no errors.
int32_t wolang_vm_ok(const WolangVm* vm);

// ----------------------------------------------------------------- calls ---

// Calls the function called |name| with |count| arguments.
//
// Zero on success, with the answer in |out| when it is not null. A
// non-zero answer means the call could not be made or the program
// trapped, and `wolang_vm_errors` says which.
//
// The compiled form is used where there is one and the interpreter
// otherwise -- the same arrangement `wolang run` has, and the reason a
// host gets machine-code speed without asking for it.
WolangStatus wolang_vm_call(WolangVm* vm,
                       const char* name,
                       const WolangValue* arguments,
                       int32_t count,
                       WolangValue* out);

// Whether the program has a function called |name| that can be called
// from here.
int32_t wolang_vm_has(const WolangVm* vm, const char* name);

// How many functions the program offers, and their names: what an editor
// or a console in a host would list.
int32_t wolang_vm_function_count(const WolangVm* vm);
const char* wolang_vm_function_name(const WolangVm* vm, int32_t index);

// The module that offers function |index|, such as `sample.mover`. A name
// can be given to `wolang_vm_call` and `wolang_vm_has` bare (`Tick`) or with
// its module (`sample.mover.Tick`); a bare name two of the host's modules
// both offer is refused (D259). Added in ABI 1.1.
const char* wolang_vm_function_module(const WolangVm* vm, int32_t index);

// -------------------------------------------------------------- bindings ---

// Makes |function| available to the program under |symbol|.
//
// A Wolang program reaches it by declaring it: `extern "C" fun
// host_spawn(x: f32, y: f32) -> s32`. The signature is the host's
// responsibility -- this is the C ABI, and a mismatch is the same mistake
// as a wrong prototype in C.
//
// Must be called before the program is loaded, or at least before the
// first call into it: the machine-code tier resolves symbols once.
WolangStatus wolang_vm_bind(WolangVm* vm, const char* symbol, void* function);

// The host's side of an `extern class`: what to do when a script takes a
// reference to one of its objects, lets go of one, and whether an object
// a `weak var` points at is still there. Any of the three may be null.
//
// A script never makes or frees such an object; its methods are bound
// with `wolang_vm_bind` as `Class_Method`, taking the object first. An
// object a script hands back holds a reference the host lets go of, and
// one passed in is lent for the length of the call (D213).
// A program's object in the host's hands: taken and let go of, read, and
// asked to do something. A method is called by its name, `Hurt` rather
// than `Enemy.Hurt`, with the object as `self`.
WolangStatus wolang_vm_object_retain(WolangVm* vm, WolangValue object);
WolangStatus wolang_vm_object_release(WolangVm* vm, WolangValue object);
WolangStatus wolang_vm_object_call(WolangVm* vm,
                           WolangValue object,
                           const char* method,
                           const WolangValue* arguments,
                           int32_t count,
                           WolangValue* out);
WolangStatus wolang_vm_object_get(WolangVm* vm,
                          WolangValue object,
                          const char* field,
                          WolangValue* out);

// Makes an object of the program's class |class_name| with every field zero,
// and sets one of its numeric or flag fields. The host owns the reference it
// is given and lets go of it with `wolang_vm_object_release`. A class with
// virtual methods, or with a field only a script can fill (text, a class, an
// array), is refused: make it in a script function and call that instead
// (D258). Added in ABI 1.2.
WolangStatus wolang_vm_object_new(WolangVm* vm, const char* class_name, WolangValue* out);
WolangStatus wolang_vm_object_set(WolangVm* vm, WolangValue object, const char* field, WolangValue value);

// The classes the host's own files declare, their stored fields in
// declaration order with the kind of each, and the attributes written on a
// class (`Replicated` for `@Replicated`). The names stay valid for as long as
// the vm does. Added in ABI 1.2.
int32_t wolang_vm_class_count(const WolangVm* vm);
const char* wolang_vm_class_name(const WolangVm* vm, int32_t index);
int32_t wolang_vm_class_field_count(const WolangVm* vm, const char* class_name);
const char* wolang_vm_class_field_name(const WolangVm* vm, const char* class_name, int32_t index);
WolangKind wolang_vm_class_field_kind(const WolangVm* vm, const char* class_name, int32_t index);
int32_t wolang_vm_class_attribute_count(const WolangVm* vm, const char* class_name);
const char* wolang_vm_class_attribute_name(const WolangVm* vm, const char* class_name, int32_t index);

// What the program says about replication (D268). Wolang does no networking:
// `@Replicated` on a field of a script class, and `@Server`, `@Client` and
// `@Multicast` on its methods, are facts the checker validated and these report,
// and the host maps them to its own policy. The flags of a field or a method
// are `WOLANG_REPLICATION_OWNER` for `(owner)` and
// `WOLANG_REPLICATION_UNRELIABLE` for `(unreliable)`. A field's index counts
// its stored fields as `wolang_vm_class_field_name` does. The methods are the
// class's own and then those of the classes it derives from, in declaration
// order; an RPC returns nothing and its parameters are the kinds a host can read.
// Added in ABI 1.5.
typedef enum WolangReplication {
  kWolangNotReplicated = 0,
  kWolangReplicated = 1,
  kWolangServer = 2,
  kWolangClient = 3,
  kWolangMulticast = 4,
} WolangReplication;
#define WOLANG_REPLICATION_OWNER 1
#define WOLANG_REPLICATION_UNRELIABLE 2

int32_t wolang_vm_class_field_replication(const WolangVm* vm, const char* class_name, int32_t index, int32_t* flags);
int32_t wolang_vm_class_method_count(const WolangVm* vm, const char* class_name);
const char* wolang_vm_class_method_name(const WolangVm* vm, const char* class_name, int32_t index);
int32_t wolang_vm_class_method_replication(const WolangVm* vm, const char* class_name, int32_t index, int32_t* flags);
int32_t wolang_vm_class_method_param_count(const WolangVm* vm, const char* class_name, int32_t index);
WolangKind wolang_vm_class_method_param_kind(const WolangVm* vm, const char* class_name, int32_t index, int32_t param);

typedef void (*WolangHostRetain)(void* object, void* user);
typedef void (*WolangHostRelease)(void* object, void* user);
typedef int32_t (*WolangHostAlive)(void* object, void* user);
WolangStatus wolang_vm_register_class(WolangVm* vm,
                                 const char* name,
                                 WolangHostRetain retain,
                                 WolangHostRelease release,
                                 WolangHostAlive alive,
                                 void* user);

// A host class with a token, for a weak reference that survives the address
// being reused (D254). |size| is `sizeof(WolangHostClass)`, so a struct that
// grows later is still read correctly. |token| may be null, and then a weak
// reference behaves as `wolang_vm_register_class` makes it.
//
// |token| answers a number that tells two objects apart that lived at the
// same address one after the other: an index and a serial, say. It is asked
// only of an object |alive| says is there. Register the class before the
// program is loaded: what a weak slot holds depends on it. Added in ABI 1.3.
typedef uint64_t (*WolangHostToken)(void* object, void* user);
typedef struct WolangHostClass {
  uint32_t size;
  const char* name;
  WolangHostRetain retain;
  WolangHostRelease release;
  WolangHostAlive alive;
  WolangHostToken token;
  void* user;
} WolangHostClass;
WolangStatus wolang_vm_register_class_ex(WolangVm* vm, const WolangHostClass* host_class);

// The arguments the program sees in `std.env.Args`.
void wolang_vm_set_args(WolangVm* vm, int32_t count, const char* const* args);

// --------------------------------------------------------------- handles ---

// A function looked up once and called many times.
//
// What an engine wants: a script's `Update` is called every frame, and a
// name lookup per frame is a string comparison per frame for nothing. A
// handle names the function, so it survives a reload and calls the new
// code afterwards (D215).
typedef struct WolangFunction WolangFunction;

WolangFunction* wolang_vm_function(WolangVm* vm, const char* name);

// Calls |function|. The same answers as `wolang_vm_call`.
WolangStatus wolang_call(WolangFunction* function,
                    const WolangValue* arguments,
                    int32_t count,
                    WolangValue* out);

// How many arguments it takes, and what it answers: what a host checks
// once rather than guessing per call. The kinds are `WolangKind` values,
// and `-1` says the function is not callable from here.
int32_t wolang_function_arity(const WolangFunction* function);
WolangKind wolang_function_result(const WolangFunction* function);
WolangKind wolang_function_parameter(const WolangFunction* function,
                                     int32_t index);
const char* wolang_function_doc(const WolangFunction* function);

// --------------------------------------------------------------- globals ---

// A module-level `var` of the loaded program, read and written.
//
// What a host configures a script with: `var gravity: f32 = -9.8` in the
// script, and the engine's own value written into it before the first
// call. Zero on success.
WolangStatus wolang_vm_get_global(WolangVm* vm,
                             const char* name,
                             WolangValue* out);
WolangStatus wolang_vm_set_global(WolangVm* vm,
                             const char* name,
                             WolangValue value);

// ---------------------------------------------------------------- output ---

// Where the program's `Print` goes.
//
// Without one it goes to standard output, like any program's. A host with
// a console of its own installs this and gets the bytes instead.
typedef void (*WolangWriter)(void* user, const char* bytes, uint64_t length);

void wolang_vm_set_writer(WolangVm* vm, WolangWriter writer, void* user);

// Where a diagnostic goes as it happens, rather than being collected.
// Same shape, same reason.
void wolang_vm_set_reporter(WolangVm* vm, WolangWriter reporter, void* user);

// ---------------------------------------------------------------- limits ---

// How much a call may do before it is stopped.
//
// For a host running scripts it did not write: a loop that never ends is
// stopped after |steps| instructions rather than hanging the process.
// Zero means no limit, which is the default. Only the interpreter can
// count steps, so setting this keeps calls off the compiled tier --
// which is the trade the caller is making knowingly.
void wolang_vm_set_step_limit(WolangVm* vm, uint64_t steps);

// A budget for each call, in ticks: one for each entry into a script function
// and each jump back to an earlier block, which is where a runaway script
// spends its time. A call that uses it up stops with `kWolangTrap`, and
// `wolang_vm_errors` says `step limit` and where; the vm stays usable, and the
// next call starts with a full budget. Unlike the step limit this works in the
// compiled tier too, at a cost of a few instructions at each of those points,
// paid only when a budget is set. Zero, the default, is no limit. The same
// budget starts again for a script function a host function calls back into.
// arm64 compiled code does not count yet, so a vm with a budget runs the
// interpreter there (D265). Added in ABI 1.4.
void wolang_vm_set_budget(WolangVm* vm, uint64_t ticks);

// Whether to use the machine-code tier at all. On by default; a host that
// wants every call counted turns it off.
void wolang_vm_set_compiled(WolangVm* vm, int32_t allowed);

// Whether loading a file starts its module-level `var`s from what the
// source wrote. On by default.
//
// A session that reloads what it has already run turns it off: the
// values are in the runtime's table already, and starting them again
// would undo everything the session did (D140).
void wolang_vm_set_initialize(WolangVm* vm, int32_t initialize);

// Loads a package someone else wrote -- a mod -- and grants it
// |capabilities| and nothing more, spelled the way a `uses` clause is:
// `"io"`, `"World, Audio"`, or `""` for nothing at all. Its own manifest
// can narrow that and cannot widen it. It may not use `unsafe`, declare
// `extern` (`E0705`) or link anything; it reaches the engine through the
// packages loaded before it, which the host trusts (D217).
WolangStatus wolang_vm_load_mod(WolangVm* vm, const char* path, const char* capabilities);

// ------------------------------------------------------------- debugging ---

// Lets an editor attach to this vm with the Debug Adapter Protocol, over
// TCP on 127.0.0.1:|port| -- 0 for whichever port is free. Answers the
// port, or -1.
//
// While an editor is attached, every call runs on the interpreter so that
// it can stop at a breakpoint, and stops on the thread that made it: the
// engine's frame waits while someone looks at the script (D216). When the
// editor goes, calls are compiled again.
int32_t wolang_vm_debug_listen(WolangVm* vm, int32_t port);

// --------------------------------------------------------------- reloads ---

// Loads everything again and keeps what the program had.
//
// What hot reloading is: a script changed on disk, and the next frame
// should run the new one with the state the last frame left. A
// module-level `var` the new program still declares, with the same type,
// keeps its value -- objects and all. One whose class gained or lost
// fields keeps the fields that are still there; a `var` or a field whose
// type changed starts from its new initializer, and `wolang_vm_notes`
// says which. Function handles stay valid (D215).
WolangStatus wolang_vm_reload(WolangVm* vm);
// What the last reload could not carry over: a `var` whose type changed,
// a field whose type changed. One line each, empty when everything came
// across.
const char* wolang_vm_notes(const WolangVm* vm);

// Where to hear that a reload moved an object the host holds (D266).
//
// A reload that changes a class the host holds an object of used to be
// refused. With a function registered here it is not: the object, and every
// object it reaches through the fields it stores, is written down under the
// old layout and read back under the new one -- a field that is still there
// keeps its value by name and shape, a new field is zero, a removed one is
// dropped -- and then |remap| is called once per object the host held with
// the old object and the new one. From then on the host uses the new one; the
// vm has moved the host's references over, so each `wolang_vm_object_release`
// of the old object becomes one of the new. The old object is left to the
// retired program and is not to be used again. A module-level `var` that
// pointed at the object points at the new one. A class the new program no
// longer has, or a field a reload cannot carry (an array, a closure, a weak
// reference, a host object), still refuses the reload and says which. Pass a
// null |remap| to go back to refusing. Added in ABI 1.4.
typedef void (*WolangRemap)(WolangVm* vm, WolangValue old_object, WolangValue new_object, void* user);
void wolang_vm_set_remap(WolangVm* vm, WolangRemap remap, void* user);

// Saves and loads an object graph from the host (D292), the same bytes
// `std.save` writes from a script (D266). The fields marked `@Save` of
// |object|, and of every object they reach, are written by name; an object
// reached twice is written once, so sharing and cycles survive. |bytes| and
// |length| get the save, which the vm owns: it stays valid until the next
// save, a reload or `wolang_vm_destroy`. A class with a virtual method, or
// with a marked field only a script can describe, is refused and the line is
// in `wolang_vm_errors`. Added in ABI 1.6.
WolangStatus wolang_vm_save(WolangVm* vm, WolangValue object, const uint8_t** bytes, uint64_t* length);

// Rebuilds the graph |bytes| holds as objects of the class |class_name|
// (qualify it, `module.Class`, when two classes share a name). |out| is the
// root, with one reference the host lets go of with `wolang_vm_object_release`.
// Bytes that are not a save of the class give `kWolangError`, and then nothing
// was allocated. A field the bytes have and the class lacks is dropped, and one
// the class has and the bytes lack is zero. Added in ABI 1.6.
WolangStatus wolang_vm_load_save(WolangVm* vm, const char* class_name, const uint8_t* bytes, uint64_t length, WolangValue* out);

// ----------------------------------------------------------------- tasks ---

// Routines on the host's clock (D267). A `task` the program starts with
// `spawn` -- from any function the host calls -- is advanced by this and by
// nothing else: it gives every task that was ready one step, with the vm's
// clock moved on by |dt| seconds first. A task that is not finished comes back
// on the next tick, and one spawned during a tick starts on the next. The
// clock is the host's: pausing is not calling this, and slow motion is a
// smaller |dt|. `wait Wait(2.0)` in a task finishes when two seconds of |dt|
// have gone by; `wait Yield()` finishes on the next tick.
//
// |steps|, when not null, gets how many steps were taken. Each vm has its own
// tasks and its own clock. The tasks must all be declared in one of the host's
// files; a program whose tasks are in several is refused, and says so in
// `wolang_vm_errors`. Added in ABI 1.5.
WolangStatus wolang_vm_tick(WolangVm* vm, double dt, int32_t* steps);
// How many tasks the vm's program has started that have not finished.
int32_t wolang_vm_tasks_pending(WolangVm* vm);

// What was loaded, so a host can watch it for changes: the files of the
// program, in load order.
int32_t wolang_vm_file_count(const WolangVm* vm);
const char* wolang_vm_file_name(const WolangVm* vm, int32_t index);

// ------------------------------------------------------------------ misc ---

// The version of the compiler this was built from.
const char* wolang_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // WOLANG_EMBED_WOLANG_H_
