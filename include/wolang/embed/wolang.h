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
} WolangKind;

typedef struct WolangValue {
  WolangKind kind;
  int64_t integer;
  double number;
  int32_t boolean;
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

// ------------------------------------------------------------------- vm ---

// One loaded program, with everything it needs to run: the sources, the
// types, the lowered code, and the machine code where there is any.
typedef struct WolangVm WolangVm;

WolangVm* wolang_vm_create(void);
void wolang_vm_destroy(WolangVm* vm);

// Loads a package directory -- one with a `wolang.toml` in it -- or a
// single `.wo` file. Zero on success; anything else means the program has
// errors, which `wolang_vm_errors` spells out.
//
// A second call adds to what is loaded rather than replacing it, which is
// how a host loads its own standard library and then a user's script.
int32_t wolang_vm_load(WolangVm* vm, const char* path);

// Loads source text under a name of the host's choosing.
//
// The name is what diagnostics and backtraces will say, so it should look
// like a path even when there is no file.
int32_t wolang_vm_load_source(WolangVm* vm,
                              const char* name,
                              const char* source);

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
int32_t wolang_vm_call(WolangVm* vm,
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
int32_t wolang_vm_bind(WolangVm* vm, const char* symbol, void* function);

// The arguments the program sees in `std.env.Args`.
void wolang_vm_set_args(WolangVm* vm, int32_t count, const char* const* args);

// --------------------------------------------------------------- handles ---

// A function looked up once and called many times.
//
// What an engine wants: a script's `Update` is called every frame, and a
// name lookup per frame is a string comparison per frame for nothing. A
// handle is valid until the program is reloaded.
typedef struct WolangFunction WolangFunction;

WolangFunction* wolang_vm_function(WolangVm* vm, const char* name);

// Calls |function|. The same answers as `wolang_vm_call`.
int32_t wolang_call(WolangFunction* function,
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
int32_t wolang_vm_get_global(WolangVm* vm,
                             const char* name,
                             WolangValue* out);
int32_t wolang_vm_set_global(WolangVm* vm,
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

// --------------------------------------------------------------- reloads ---

// Forgets everything and loads |path| again.
//
// What hot reloading is: a script changed on disk, and the next frame
// should run the new one. Handles from before are invalid afterwards.
int32_t wolang_vm_reload(WolangVm* vm);

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
