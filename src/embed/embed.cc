// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Wolang as a library, behind a C ABI.
//
// Everything here is the same machinery `wolang run` uses -- the query
// database, the lowering, the dev tier, the interpreter -- with the
// pieces a host cares about brought out through a handful of functions
// and no C++ in the header (D128).

#include "wolang/embed/wolang.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "wolang/abi/layout.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/diagnostic_renderer.h"
#include "wolang/codegen/compiler.h"
#include "wolang/codegen/foreign.h"
#include "wolang/compiler/database.h"
#include "wolang/compiler/workspace.h"
#include "wolang/debug/dap.h"
#include "wolang/interp/interpreter.h"
#include "wolang/ir/ir.h"
#include "wolang/syntax/ast.h"
#include "wolang/interp/marshal.h"
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/env.h"
#include "wolang/runtime/globals.h"
#include "wolang/runtime/host.h"
#include "wolang/runtime/scheduler.h"
#include "wolang/runtime/sockets.h"
#include "wolang/runtime/system.h"
#include "wolang/runtime/text.h"

namespace {

using wolang::CheckedFile;
using wolang::Database;
using wolang::Diagnostic;
using wolang::DiagnosticEngine;
using wolang::FileId;
using wolang::LoweredFile;
using wolang::Module;
using wolang::RenderOptions;
using wolang::Severity;
using wolang::Symbol;
using wolang::TypeId;
using wolang::TypeKind;

// One function a host may call, and where it lives.
struct Callable {
  std::string name;
  std::string owner;
  std::string doc;
  Symbol symbol;
  const wolang::ir::Module* module = nullptr;
  const wolang::ir::Function* function = nullptr;
  WolangVm* vm = nullptr;
  // A method or a `$drop`: found by its name, because a `$drop` has no
  // declaration of its own to be found by.
  bool is_member = false;
};

}  // namespace

// The vm is a C type in the header and this in reality.
// What a reload leaves behind: the program as it was, kept until the vm
// goes away. Code built from it is never run again, but values the new
// program carried over may still point into it -- at a string literal, at
// an object the old code made (D215).
struct Retired {
  std::unique_ptr<Database> database;
  std::unique_ptr<wolang::interp::ForeignCaller> foreign;
  std::unique_ptr<wolang::codegen::CompiledModule> compiled;
  std::shared_ptr<wolang::runtime::GlobalSpace> globals;
};

// An editor's way in: the socket it connects to, the connection it is on,
// and the sessions it has had -- every one kept, because a call that was
// stopped in one may still be on its way out of it (D216).
struct DebugLink {
  int64_t listener = -1;
  std::atomic<int64_t> client{-1};
  std::atomic<bool> closing{false};
  std::thread thread;
  std::mutex lock;
  std::shared_ptr<wolang::debug::DapSession> session;
  std::vector<std::shared_ptr<wolang::debug::DapSession>> sessions;

  ~DebugLink() {
    closing = true;
    if (client >= 0) {
      wolang::runtime::wolang_socket_shutdown(client);
    }
    // Shutting a listener down is what wakes `accept` on one system and
    // closing it is what does on the other.
    wolang::runtime::wolang_socket_shutdown(listener);
    wolang::runtime::wolang_socket_close(listener);
    if (thread.joinable()) {
      thread.join();
    }
  }
};

// What `wolang_vm_function` hands out: a name, so that it means the same
// function after a reload as before (D215).
// How one word of a prepared call is read or written: a parameter in a
// stub's slot, or the answer in its result.
enum class WordKind : uint8_t { kVoid, kInt, kFloat32, kFloat64, kBool, kHandle };

struct Word {
  WordKind kind = WordKind::kVoid;
  uint8_t bytes = 8;
  bool is_signed = true;
  TypeId type;
};

struct FunctionHandle {
  WolangVm* vm = nullptr;
  std::string name;
  // What a call needs to skip the lookup and the boxing, worked out for
  // the program as it was at |generation| and again after a reload. Zero
  // |prepared| when some parameter is not one word (D219).
  uint64_t generation = 0;
  uintptr_t prepared = 0;
  std::vector<Word> words;
  Word result;
};

struct WolangVm {
  // Its own allocation, so that a reload can retire it with the code that
  // was built from it.
  std::unique_ptr<Database> owned_database = std::make_unique<Database>();
  Database& database = *owned_database;
  // What was loaded, so a reload can do it again and a host can watch
  // the files for changes. A path, or a name and the source the host
  // gave under it.
  struct Loaded {
    std::string path;
    std::string source;
    bool from_source = false;
  };
  std::vector<Loaded> loaded;
  std::vector<FileId> files;
  // The files of the package the host asked for, as opposed to its
  // dependencies: what `wolang_vm_function_count` lists, because a host
  // wants its script's functions and not the standard library's (D128).
  std::vector<FileId> own;
  std::vector<Callable> callable;
  // Methods and `$drop`s, which a host reaches through an object rather
  // than by name (D213).
  std::vector<Callable> members;
  std::unique_ptr<wolang::codegen::CompiledModule> compiled;
  std::unique_ptr<wolang::interp::ForeignCaller> foreign;
  std::vector<const wolang::ir::Module*> linked;
  std::string errors;
  bool ok = false;
  // Which files have had their module-level `var`s started. A second
  // load must not start them again: a session that adds a file per line
  // would see every variable it has set go back to its first value
  // (D140).
  std::vector<FileId> initialized;
  // How much a call may do, and whether the machine-code tier may run
  // it: counting steps is something only the interpreter can do (D137).
  uint64_t steps = 0;
  bool allow_compiled = true;
  // Whether a load starts the module-level `var`s it declares (D140).
  bool initialize = true;
  // Kept alive for as long as the vm is: a text the program answered
  // points into one of these, so the host can read it after the call.
  std::deque<std::string> answers;
  // One call into a vm at a time, from whichever thread makes it; the
  // same thread may come back in through a host function (D214). Shared,
  // because a reload builds the vm again around the lock it is holding.
  std::shared_ptr<std::recursive_mutex> lock = std::make_shared<std::recursive_mutex>();
  // The program's module-level `var`s, which belong to this vm and to no
  // other (D214).
  std::shared_ptr<wolang::runtime::GlobalSpace> globals{wolang::runtime::GlobalSpaceCreate(), &wolang::runtime::GlobalSpaceDestroy};
  // These outlive a reload, which builds the vm again around them.
  std::shared_ptr<std::vector<Retired>> retired = std::make_shared<std::vector<Retired>>();
  std::shared_ptr<std::deque<FunctionHandle>> handles = std::make_shared<std::deque<FunctionHandle>>();
  std::shared_ptr<DebugLink> debug;
  // What the last reload could not carry over, one line each.
  std::string notes;
  // Which program this is: a new number every time one is compiled, which
  // is how a function handle knows what it prepared is stale.
  uint64_t generation = 0;
  // The packages loaded as mods, and what each was granted (D217).
  std::vector<std::pair<std::string, std::vector<std::string>>> sandboxes;
  // Where this vm's output goes when its host asked for it (D257).
  WolangWriter writer = nullptr;
  void* writer_user = nullptr;
  WolangWriter reporter = nullptr;
  void* reporter_user = nullptr;
  // The program's objects the host took a reference to, by address: what a
  // reload must not leave pointing at a class whose layout changed (D255).
  struct Held {
    int32_t Count = 0;
    std::string Type;
    uint32_t Index = 0;
  };
  std::map<void*, Held> held;
};

namespace {

// The `///` lines in front of a declaration, with the slashes taken
// off: what a host shows beside a script's function in a console, and
// the same text `wolang doc` writes (D137).
std::string DocumentationFor(WolangVm* vm, FileId file, Symbol name);

// Hands |text| to the host's writer, or to standard output when there is
// none (D140).
void Written(WolangVm* vm, const std::string& text);

// Everything the program has to say about itself, rendered the way the
// command-line compiler renders it.
std::string Render(Database* database,
                   const std::vector<Diagnostic>& diagnostics) {
  if (diagnostics.empty()) {
    return std::string();
  }
  RenderOptions options;
  options.color = false;
  std::string text;
  for (const Diagnostic& diagnostic : diagnostics) {
    text.append(wolang::RenderDiagnostic(database->sources(), diagnostic,
                                         options));
    text.push_back('\n');
  }
  return text;
}

std::string DocumentationFor(WolangVm* vm, FileId file, Symbol name) {
  const wolang::ParsedFile* parsed = vm->database.Parse(file);
  if (parsed == nullptr) {
    return std::string();
  }
  for (const wolang::ItemId id : parsed->ast.Get(parsed->ast.root())) {
    const wolang::Item& item = parsed->ast.Get(id);
    if (item.name != name || !item.doc.is_valid()) {
      continue;
    }
    std::string text;
    const std::string_view whole =
        vm->database.sources().SpanText(item.doc);
    size_t at = 0;
    while (at <= whole.size()) {
      const size_t stop = whole.find('\n', at);
      std::string_view line = whole.substr(
          at, stop == std::string_view::npos ? stop : stop - at);
      at = stop == std::string_view::npos ? whole.size() + 1 : stop + 1;
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
      }
      if (line.rfind("///", 0) == 0) {
        line.remove_prefix(3);
      }
      if (!line.empty() && line.front() == ' ') {
        line.remove_prefix(1);
      }
      text.append(line);
      text.push_back('\n');
    }
    return text;
  }
  return std::string();
}

// Builds what a call needs: the code, the machine code where there is
// any, and the list of functions a host may name.
void Prepare(WolangVm* vm) {
  std::vector<Diagnostic> all;
  vm->callable.clear();
  vm->members.clear();
  vm->linked.clear();
  std::vector<wolang::codegen::FileCode> code;

  for (FileId file : vm->files) {
    if (const std::vector<Diagnostic>* diagnostics =
            vm->database.Diagnostics(file)) {
      all.insert(all.end(), diagnostics->begin(), diagnostics->end());
    }
    const LoweredFile* lowered = vm->database.Lowered(file);
    const CheckedFile* checked = vm->database.Checked(file);
    if (lowered == nullptr || checked == nullptr) {
      continue;
    }
    vm->linked.push_back(&lowered->module);
    code.push_back(
        wolang::codegen::FileCode{&checked->resolution, &lowered->module});

    for (const wolang::ir::Function& function : lowered->module.functions) {
      // A function with a body and a name a host can say: the compiler's
      // own helpers are named with characters an identifier cannot hold,
      // which is what keeps them out of this list.
      if (function.blocks.empty() || !function.name.is_valid()) {
        continue;
      }
      if (std::find(vm->own.begin(), vm->own.end(), file) ==
          vm->own.end()) {
        continue;
      }
      const std::string_view spelled = vm->database.interner().Text(function.name);
      if (spelled.find('.') != std::string_view::npos || spelled.ends_with("$drop")) {
        Callable member;
        member.name = std::string(spelled);
        member.symbol = function.name;
        member.module = &lowered->module;
        member.function = &function;
        member.vm = vm;
        member.is_member = true;
        vm->members.push_back(std::move(member));
        continue;
      }
      // Only what the file offered: a helper a script did not export is
      // not part of what it offers a host, the same way it is not part
      // of what it offers another module.
      const wolang::Binding* binding =
          checked->resolution.Find(function.name);
      if (binding == nullptr || !binding->is_public) {
        continue;
      }
      const std::string_view name = vm->database.interner().Text(function.name);
      if (name.find_first_of(".$<") != std::string_view::npos) {
        continue;
      }
      Callable entry;
      entry.name = std::string(name);
      if (const wolang::Module* home = vm->database.GetModule(vm->database.ModuleOf(file)); home != nullptr) {
        entry.owner = home->name;
      }
      entry.doc = DocumentationFor(vm, file, function.name);
      entry.symbol = function.name;
      entry.module = &lowered->module;
      entry.function = &function;
      entry.vm = vm;
      vm->callable.push_back(std::move(entry));
    }
  }

  bool failed = false;
  for (const Diagnostic& diagnostic : all) {
    failed |= diagnostic.severity == Severity::kError;
  }
  vm->errors = Render(&vm->database, all);
  vm->ok = !failed && !vm->files.empty();
  if (!vm->ok) {
    vm->compiled.reset();
    return;
  }

  if (vm->foreign == nullptr) {
    vm->foreign = wolang::codegen::MakeForeignCaller();
  }
  vm->compiled = wolang::codegen::Compile(vm->database.interner(),
                                          vm->database.types(), code);
  static std::atomic<uint64_t> sm_Generations{0};
  vm->generation = ++sm_Generations;

  // A module-level `var` starts as what its initializer says, and
  // something has to run those: the driver does it before `main`, and a
  // host has no `main` -- so loading is where it happens (D137).
  for (FileId file : vm->files) {
    const LoweredFile* lowered = vm->database.Lowered(file);
    if (lowered == nullptr ||
        lowered->module.initializer >= lowered->module.functions.size()) {
      continue;
    }
    if (std::find(vm->initialized.begin(), vm->initialized.end(), file) !=
        vm->initialized.end()) {
      continue;
    }
    vm->initialized.push_back(file);
    if (!vm->initialize) {
      continue;
    }
    const wolang::ir::Function& init =
        lowered->module.functions[lowered->module.initializer];
    const CheckedFile* checked = vm->database.Checked(file);
    static const wolang::FileResolution kNothing;
    wolang::abi::LayoutStore layouts(
        vm->database.types(),
        checked != nullptr ? checked->resolution : kNothing);
    vm->database.ShareMembers(&layouts);
    // One module's initializer at a time, leaving the ones already run
    // alone: a reset here would undo the file loaded before this one.
    wolang::interp::Limits once;
    once.keep_globals = true;
    if (vm->compiled != nullptr && vm->compiled->Contains(init.name)) {
      vm->compiled->Run(init.name, {}, nullptr, /*keep_globals=*/true);
      continue;
    }
    wolang::interp::RunNamed(vm->database.interner(), vm->database.types(),
                             lowered->module, init.name, {}, once, nullptr,
                             vm->foreign.get(), &layouts, vm->linked);
  }
}

// The function called |name|, preferring the one loaded last.
//
// A host's own source wins over a library's: a session that declares
// `Read` means its own, and two files of a program may both have a
// function by that name. |arity| narrows it further when the caller
// knows how many arguments it is passing -- which is what stops a call
// reaching a function of the same name and a different shape (D140).
bool Names(const Callable& entry, std::string_view spelled) {
  if (entry.name == spelled) {
    return true;
  }
  return !entry.owner.empty() && spelled.size() == entry.owner.size() + 1 + entry.name.size() && spelled.starts_with(entry.owner) && spelled[entry.owner.size()] == '.' && spelled.substr(entry.owner.size() + 1) == entry.name;
}

bool Ambiguous(const WolangVm* vm, const char* name) {
  if (vm == nullptr || name == nullptr) {
    return false;
  }
  const Callable* first = nullptr;
  for (const Callable& entry : vm->callable) {
    if (entry.name != name) {
      continue;
    }
    if (first != nullptr && first->owner != entry.owner) {
      return true;
    }
    first = &entry;
  }
  return false;
}

const Callable* Find(const WolangVm* vm, const char* name,
                     int32_t arity = -1) {
  if (vm == nullptr || name == nullptr) {
    return nullptr;
  }
  const Callable* fallback = nullptr;
  for (auto it = vm->callable.rbegin(); it != vm->callable.rend(); ++it) {
    if (!Names(*it, name)) {
      continue;
    }
    if (arity < 0) {
      return &*it;
    }
    const size_t takes =
        it->function == nullptr
            ? 0
            : vm->database.types().ArgumentsOf(it->function->type).size();
    if (static_cast<int32_t>(takes) == arity) {
      return &*it;
    }
    if (fallback == nullptr) {
      fallback = &*it;
    }
  }
  return fallback;
}

// A host's value as the interpreter holds one, at the type the function
// declared: an `int` parameter is 32 bits wide and a host that passes a
// 64-bit number should get the same answer either tier runs it.
wolang::interp::Value Boxed(const WolangVm* vm,
                            const WolangValue& value,
                            TypeId wanted) {
  const wolang::TypeStore& types = vm->database.types();
  const TypeKind kind =
      wanted.is_valid() ? types.KindOf(wanted) : TypeKind::kError;
  switch (value.kind) {
    case kWolangInt:
      if (kind == TypeKind::kFloat) {
        return wolang::interp::Value::Float(
            static_cast<double>(value.integer),
            types.Get(wanted).float_width);
      }
      if (kind == TypeKind::kInt) {
        return wolang::interp::Value::Int(
            static_cast<uint64_t>(value.integer),
            types.Get(wanted).int_width, types.Get(wanted).is_signed);
      }
      return wolang::interp::Value::Int(
          static_cast<uint64_t>(value.integer), wolang::IntWidth::k64,
          /*is_signed=*/true);
    case kWolangFloat:
      if (kind == TypeKind::kInt) {
        return wolang::interp::Value::Int(
            static_cast<uint64_t>(static_cast<int64_t>(value.number)),
            types.Get(wanted).int_width, types.Get(wanted).is_signed);
      }
      return wolang::interp::Value::Float(
          value.number, kind == TypeKind::kFloat
                            ? types.Get(wanted).float_width
                            : wolang::FloatWidth::k64);
    case kWolangBool:
      return wolang::interp::Value::Bool(value.boolean != 0);
    case kWolangText: {
      // The pair every tier holds a string as: where the bytes are and
      // how many. The host's bytes, borrowed for the length of the call
      // -- which is why the header says so (D128).
      std::vector<wolang::interp::Value> pair;
      pair.push_back(wolang::interp::Value::Int(
          reinterpret_cast<uint64_t>(value.text), wolang::IntWidth::k64,
          /*is_signed=*/false));
      pair.push_back(wolang::interp::Value::Int(
          value.length, wolang::IntWidth::k64, /*is_signed=*/false));
      return wolang::interp::Value::Aggregate(std::move(pair));
    }
    case kWolangHandle:
    case kWolangObject: {
      const wolang::interp::Value word = wolang::interp::Value::Int(reinterpret_cast<uint64_t>(value.handle), wolang::IntWidth::k64, /*is_signed=*/false);
      if (kind == TypeKind::kOptional) {
        return value.handle == nullptr ? wolang::interp::Value::Absent() : wolang::interp::Value::Present(word);
      }
      return word;
    }
    case kWolangVoid:
    default:
      return wolang::interp::Value::Void();
  }
}

WolangValue Unboxed(WolangVm* vm, const wolang::interp::Value& value) {
  WolangValue result = wolang_void();
  switch (value.kind()) {
    case wolang::interp::Value::Kind::kInt:
      result.kind = kWolangInt;
      result.integer = value.signed_integer();
      return result;
    case wolang::interp::Value::Kind::kFloat:
      result.kind = kWolangFloat;
      result.number = value.floating();
      return result;
    case wolang::interp::Value::Kind::kBool:
      result.kind = kWolangBool;
      result.boolean = value.boolean() ? 1 : 0;
      return result;
    case wolang::interp::Value::Kind::kString:
      // Kept by the vm, because the host holds a pointer to it.
      vm->answers.push_back(value.text());
      result.kind = kWolangText;
      result.text = vm->answers.back().c_str();
      result.length = vm->answers.back().size();
      return result;
    case wolang::interp::Value::Kind::kAggregate: {
      // A string as both tiers hold one: a pointer and a length. Copied
      // out, because the program's own copy may live in an arena that is
      // about to be given back.
      const std::span<const wolang::interp::Value> parts = value.elements();
      if (parts.size() != 2 ||
          parts[0].kind() != wolang::interp::Value::Kind::kInt ||
          parts[1].kind() != wolang::interp::Value::Kind::kInt) {
        return result;
      }
      const auto* bytes =
          reinterpret_cast<const char*>(parts[0].integer());
      const auto length = static_cast<size_t>(parts[1].integer());
      if (bytes == nullptr) {
        vm->answers.emplace_back();
      } else {
        vm->answers.emplace_back(bytes, length);
      }
      result.kind = kWolangText;
      result.text = vm->answers.back().c_str();
      result.length = vm->answers.back().size();
      return result;
    }
    default:
      return result;
  }
}

}  // namespace

extern "C" {

// A call into a vm: its lock held, and its globals the ones this thread's
// code reads and writes.
class VmTurn {
 public:
  explicit VmTurn(const WolangVm* vm) : m_Lock(vm != nullptr ? std::unique_lock<std::recursive_mutex>(*vm->lock) : std::unique_lock<std::recursive_mutex>()), m_Space(vm != nullptr ? vm->globals.get() : wolang::runtime::GlobalSpaceCurrent()) {}

 private:
  std::unique_lock<std::recursive_mutex> m_Lock;
  wolang::runtime::GlobalSpaceScope m_Space;
};

WolangValue wolang_void(void) {
  WolangValue value;
  std::memset(&value, 0, sizeof(value));
  value.kind = kWolangVoid;
  return value;
}

WolangValue wolang_int(int64_t number) {
  WolangValue value = wolang_void();
  value.kind = kWolangInt;
  value.integer = number;
  return value;
}

WolangValue wolang_float(double number) {
  WolangValue value = wolang_void();
  value.kind = kWolangFloat;
  value.number = number;
  return value;
}

WolangValue wolang_bool(int32_t flag) {
  WolangValue value = wolang_void();
  value.kind = kWolangBool;
  value.boolean = flag != 0 ? 1 : 0;
  return value;
}

WolangValue wolang_text(const char* bytes, uint64_t length) {
  WolangValue value = wolang_void();
  value.kind = kWolangText;
  value.text = bytes;
  value.length = length;
  return value;
}

WolangValue wolang_handle(void* object) {
  WolangValue value = wolang_void();
  value.kind = kWolangHandle;
  value.handle = object;
  return value;
}

WolangStatus wolang_vm_register_class(WolangVm* vm, const char* name, WolangHostRetain retain, WolangHostRelease release, WolangHostAlive alive, void* user) {
  if (vm == nullptr || name == nullptr) {
    return kWolangError;
  }
  const VmTurn turn(vm);
  wolang::runtime::SpaceClass entry;
  entry.Tag = wolang::runtime::HostTag(name);
  entry.Retain = retain;
  entry.Release = release;
  entry.Alive = alive;
  entry.User = user;
  wolang::runtime::SpaceRegisterClass(vm->globals.get(), entry);
  return kWolangOk;
}

WolangStatus wolang_vm_register_class_ex(WolangVm* vm, const WolangHostClass* host_class) {
  if (vm == nullptr || host_class == nullptr || host_class->name == nullptr || host_class->size < offsetof(WolangHostClass, user) + sizeof(void*)) {
    return kWolangError;
  }
  const VmTurn turn(vm);
  wolang::runtime::SpaceClass entry;
  entry.Tag = wolang::runtime::HostTag(host_class->name);
  entry.Retain = host_class->retain;
  entry.Release = host_class->release;
  entry.Alive = host_class->alive;
  entry.Token = host_class->token;
  entry.User = host_class->user;
  wolang::runtime::SpaceRegisterClass(vm->globals.get(), entry);
  return kWolangOk;
}

WolangVm* wolang_vm_create(void) {
  return new WolangVm();
}

// |options| says how much of it the host knows (D241): a struct from a
// newer header than this library is refused rather than half read.
WolangVm* wolang_vm_create_with(const WolangVmOptions* options) {
  if (options == nullptr || options->size < sizeof(uint32_t) ||
      options->size > sizeof(WolangVmOptions)) {
    return nullptr;
  }
  return new WolangVm();
}

uint32_t wolang_abi_version(void) {
  return WOLANG_ABI_VERSION;
}

static void ReleaseGlobals(WolangVm* vm);

void wolang_vm_destroy(WolangVm* vm) {
  if (vm != nullptr) {
    const VmTurn turn(vm);
    ReleaseGlobals(vm);
  }
  delete vm;
}

// The package |directory| holds, among those loaded.
static wolang::PackageId PackageAt(WolangVm* vm, const std::string& directory) {
  std::error_code failed;
  const std::filesystem::path wanted = std::filesystem::weakly_canonical(directory, failed);
  for (size_t i = 0; i < vm->database.package_count(); ++i) {
    const wolang::PackageId id{static_cast<uint32_t>(i)};
    const wolang::Package* package = vm->database.GetPackage(id);
    if (package != nullptr && std::filesystem::weakly_canonical(package->directory, failed) == wanted) {
      return id;
    }
  }
  return wolang::PackageId{};
}

WolangStatus wolang_vm_load_mod(WolangVm* vm, const char* path, const char* capabilities) {
  const VmTurn turn(vm);
  if (vm == nullptr || path == nullptr || !wolang::IsPackageDirectory(path)) {
    if (vm != nullptr) {
      vm->errors = "wolang: a mod is a package directory\n";
      vm->ok = false;
    }
    return kWolangError;
  }
  std::vector<std::string> granted;
  std::string list = capabilities != nullptr ? capabilities : "";
  size_t start = 0;
  while (start <= list.size()) {
    const size_t comma = std::min(list.find(',', start), list.size());
    std::string name = list.substr(start, comma - start);
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    if (!name.empty()) {
      granted.push_back(name);
    }
    start = comma + 1;
  }
  vm->sandboxes.emplace_back(path, granted);
  return wolang_vm_load(vm, path);
}

static void AttachToHost(WolangVm* vm, FileId file);

// The standard library for a lone file, found the way the driver finds it:
// WOLANG_STD, then a tree the file sits inside. A package loaded by path
// brings its own dependencies; a single file has none to bring (D259).
static void LoadStandardFor(WolangVm* vm, const char* path) {
  for (size_t i = 0; i < vm->database.package_count(); ++i) {
    const wolang::Package* held = vm->database.GetPackage(wolang::PackageId{static_cast<uint32_t>(i)});
    if (held != nullptr && held->name == "std") {
      return;
    }
  }

  std::error_code failed;
  std::string standard;
  if (const char* said = std::getenv("WOLANG_STD")) {
    standard = said;
  } else {
    for (std::filesystem::path up = std::filesystem::absolute(path, failed).parent_path(); !up.empty() && up != up.root_path(); up = up.parent_path()) {
      if (std::filesystem::exists(up / "lib" / "std" / "wolang.toml", failed)) {
        standard = (up / "lib" / "std").string();
        break;
      }
    }
  }
  if (standard.empty() || !wolang::IsPackageDirectory(standard.c_str())) {
    return;
  }

  DiagnosticEngine loading;
  if (!wolang::LoadPackage(&vm->database, standard.c_str(), &loading)) {
    return;
  }
  for (const Module& module : vm->database.modules()) {
    for (FileId file : module.files) {
      if (std::find(vm->files.begin(), vm->files.end(), file) == vm->files.end()) {
        vm->files.push_back(file);
      }
    }
  }
}

WolangStatus wolang_vm_load(WolangVm* vm, const char* path) {
  const VmTurn turn(vm);
  if (vm == nullptr || path == nullptr) {
    return kWolangError;
  }
  DiagnosticEngine loading;
  if (wolang::IsPackageDirectory(path)) {
    if (!wolang::LoadPackage(&vm->database, path, &loading)) {
      vm->errors = Render(&vm->database, loading.diagnostics());
      if (vm->errors.empty()) {
        vm->errors = "wolang: cannot load the package\n";
      }
      vm->ok = false;
      return kWolangError;
    }
    // A mod's grant is the host's, and it is applied before anything is
    // checked, every time the package is loaded -- a reload included.
    for (const std::pair<std::string, std::vector<std::string>>& sandbox : vm->sandboxes) {
      vm->database.Restrict(PackageAt(vm, sandbox.first), sandbox.second);
    }
    for (const Module& module : vm->database.modules()) {
      for (FileId file : module.files) {
        if (std::find(vm->files.begin(), vm->files.end(), file) ==
            vm->files.end()) {
          vm->files.push_back(file);
        }
      }
    }
    // Which of them is the package the host named, as opposed to a
    // dependency: the files under the directory that was asked for.
    // Matched on the resolved path, because the host may name it
    // relatively and the database keeps it absolute.
    std::error_code failed;
    const std::string wanted =
        std::filesystem::weakly_canonical(path, failed).string();
    for (FileId file : vm->files) {
      const std::string where(vm->database.sources().Path(file));
      if (!wanted.empty() && where.rfind(wanted, 0) == 0 &&
          std::find(vm->own.begin(), vm->own.end(), file) ==
              vm->own.end()) {
        vm->own.push_back(file);
      }
    }
  } else {
    std::string error;
    const FileId file = vm->database.LoadFile(path, &error);
    if (!file.is_valid()) {
      vm->errors = error + "\n";
      vm->ok = false;
      return kWolangError;
    }
    LoadStandardFor(vm, path);
    AttachToHost(vm, file);
    vm->files.push_back(file);
    vm->own.push_back(file);
  }
  vm->loaded.push_back(WolangVm::Loaded{path, std::string(), false});
  Prepare(vm);
  return vm->ok ? kWolangOk : kWolangError;
}

// Source a host handed over belongs to a package of its own, which depends
// on every package already loaded: a script that cannot say `need std.io` is
// not a script anyone wants to write (D140). A single file loaded by path
// gets the same package, so it resolves `need` the same way (D259).
static void AttachToHost(WolangVm* vm, FileId file) {
  // One package for everything a host hands over, made once: a second
  // one would put the second file in another module, and then two
  // lines of a session could not see each other's declarations (D140).
  wolang::ModuleId module = vm->database.FindModule("host");
  if (!module.is_valid()) {
    wolang::Manifest manifest;
    manifest.name = "host";
    manifest.version = "0.0.0";
    for (size_t i = 0; i < vm->database.package_count(); ++i) {
      const wolang::Package* held =
          vm->database.GetPackage(
              wolang::PackageId{static_cast<uint32_t>(i)});
      if (held == nullptr || held->name == "host") {
        continue;
      }
      wolang::ManifestDependency dependency;
      dependency.name = held->name;
      dependency.path = held->directory;
      manifest.dependencies.push_back(std::move(dependency));
    }
    const wolang::PackageId package = vm->database.AddPackage(
        "host", std::filesystem::current_path().string(),
        std::move(manifest));
    module = vm->database.AddModule(
        "host", std::filesystem::current_path().string(), package);
  }
  std::vector<FileId> files;
  if (const wolang::Module* existing = vm->database.GetModule(module);
      existing != nullptr) {
    files = existing->files;
  }
  files.push_back(file);
  vm->database.SetModuleFiles(module, std::move(files));
}

WolangStatus wolang_vm_load_source(WolangVm* vm,
                              const char* name,
                              const char* source) {
  const VmTurn turn(vm);
  if (vm == nullptr || source == nullptr) {
    return kWolangError;
  }
  const FileId file = vm->database.AddFile(
      name == nullptr ? "<host>" : name, source);
  if (file.is_valid()) {
    LoadStandardFor(vm, name == nullptr ? "<host>" : name);
    AttachToHost(vm, file);
  }
  if (!file.is_valid()) {
    vm->errors = "wolang: cannot add the source\n";
    vm->ok = false;
    return kWolangError;
  }
  vm->files.push_back(file);
  vm->own.push_back(file);
  vm->loaded.push_back(WolangVm::Loaded{
      name == nullptr ? "<host>" : name, source, true});
  Prepare(vm);
  return vm->ok ? kWolangOk : kWolangError;
}

const char* wolang_vm_errors(const WolangVm* vm) {
  const VmTurn turn(vm);
  return vm == nullptr ? "" : vm->errors.c_str();
}

int32_t wolang_vm_ok(const WolangVm* vm) {
  return vm != nullptr && vm->ok ? 1 : 0;
}

int32_t wolang_vm_has(const WolangVm* vm, const char* name) {
  return Find(vm, name) != nullptr ? 1 : 0;
}

int32_t wolang_vm_function_count(const WolangVm* vm) {
  return vm == nullptr ? 0 : static_cast<int32_t>(vm->callable.size());
}

const char* wolang_vm_function_name(const WolangVm* vm, int32_t index) {
  if (vm == nullptr || index < 0 ||
      static_cast<size_t>(index) >= vm->callable.size()) {
    return "";
  }
  return vm->callable[static_cast<size_t>(index)].name.c_str();
}

const char* wolang_vm_function_module(const WolangVm* vm, int32_t index) {
  if (vm == nullptr || index < 0 ||
      static_cast<size_t>(index) >= vm->callable.size()) {
    return "";
  }
  return vm->callable[static_cast<size_t>(index)].owner.c_str();
}

WolangStatus wolang_vm_bind(WolangVm* vm, const char* symbol, void* function) {
  const VmTurn turn(vm);
  if (vm == nullptr || symbol == nullptr || function == nullptr) {
    return kWolangError;
  }
  wolang::runtime::SpaceBindSymbol(vm->globals.get(), symbol, function);
  return kWolangOk;
}

void wolang_vm_set_args(WolangVm* vm,
                        int32_t count,
                        const char* const* args) {
  const VmTurn turn(vm);
  (void)vm;
  wolang::runtime::wolang_args_set(count < 0 ? 0 : count, args);
}

// A host's object or one of the program's, as what a value of |type|
// answers: the address, and what kind of thing is behind it (D213).
static WolangValue ObjectOf(WolangVm* vm, uint64_t address, TypeId type) {
  const wolang::TypeStore& types = vm->database.types();
  if (types.IsForeignClass(type)) {
    return wolang_handle(reinterpret_cast<void*>(address));
  }
  WolangValue value = wolang_void();
  if (address == 0) {
    return value;
  }
  value.kind = kWolangObject;
  value.handle = reinterpret_cast<void*>(address);
  value.reserved = types.StripWrappers(type).index;
  vm->answers.push_back(types.Describe(vm->database.interner(), types.StripWrappers(type)));
  value.text = vm->answers.back().c_str();
  value.length = vm->answers.back().size();
  return value;
}

// An object a call just handed to the host: the host owns that reference
// until it releases it, so a reload has to keep the object as it is (D255).
static void Handed(WolangVm* vm, const WolangValue& value) {
  if (value.kind != kWolangObject || value.handle == nullptr) {
    return;
  }
  WolangVm::Held& entry = vm->held[value.handle];
  entry.Count += 1;
  entry.Type.assign(value.text != nullptr ? value.text : "", static_cast<size_t>(value.length));
  entry.Index = value.reserved;
}

// Runs |entry| with |boxed|, in machine code when there is some, and
// answers what it returned.
// The editor attached to |vm| right now, if one is.
static std::shared_ptr<wolang::debug::DapSession> Attached(WolangVm* vm) {
  if (vm->debug == nullptr) {
    return nullptr;
  }
  const std::lock_guard<std::mutex> held(vm->debug->lock);
  const std::shared_ptr<wolang::debug::DapSession>& session = vm->debug->session;
  return session != nullptr && session->attached() && !session->finished() ? session : nullptr;
}

// Runs |entry| on the interpreter as the source wrote it -- the IR before
// any pass, where every local is where the source put it -- with |session|
// told where it is.
static wolang::interp::Outcome Debugged(WolangVm* vm, const Callable* entry, const std::vector<wolang::interp::Value>& boxed, wolang::debug::DapSession* session) {
  std::vector<const wolang::ir::Module*> modules;
  for (FileId file : vm->files) {
    if (const LoweredFile* lowered = vm->database.Lowered(file)) {
      modules.push_back(&lowered->unoptimized);
    }
  }
  static const wolang::FileResolution kNothing;
  const FileId home{entry->function->source_file};
  const LoweredFile* lowered = vm->database.Lowered(home);
  const CheckedFile* checked = vm->database.Checked(home);
  wolang::abi::LayoutStore layouts(vm->database.types(), checked != nullptr ? checked->resolution : kNothing);
  vm->database.ShareMembers(&layouts);

  wolang::interp::Limits limits;
  limits.keep_globals = true;
  limits.steps = UINT64_MAX;
  limits.debugger = session;
  session->SetSources(&vm->database.sources());
  wolang::interp::Output printed;
  const wolang::interp::Outcome outcome = wolang::interp::RunNamed(vm->database.interner(), vm->database.types(), lowered != nullptr ? lowered->unoptimized : *entry->module, entry->symbol, boxed, limits, &printed, vm->foreign.get(), &layouts, modules);
  if (!printed.text.empty()) {
    Written(vm, printed.text);
  }
  return outcome;
}

static WolangStatus Invoke(WolangVm* vm, const Callable* entry, const std::vector<wolang::interp::Value>& boxed, WolangValue* out) {
  wolang::interp::Outcome outcome;
  const std::shared_ptr<wolang::debug::DapSession> debugging = Attached(vm);
  // The compiled form where there is one, the interpreter otherwise:
  // the same choice `wolang run` makes, and the reason a host gets
  // machine-code speed without asking. A step limit keeps the call off
  // the compiled tier, because counting steps is something only the
  // interpreter does (D137).
  if (debugging != nullptr) {
    outcome = Debugged(vm, entry, boxed, debugging.get());
  }
  else if (vm->allow_compiled && vm->steps == 0 && vm->compiled != nullptr && entry->is_member && vm->compiled->Contains(entry->symbol)) {
    outcome = vm->compiled->Run(entry->symbol, boxed, nullptr, /*keep_globals=*/true);
  }
  else if (vm->allow_compiled && vm->steps == 0 && vm->compiled != nullptr &&
      vm->compiled->ContainsSource(entry->function->source_file,
                                   entry->function->source_item)) {
    outcome = vm->compiled->RunSource(entry->function->source_file,
                                      entry->function->source_item, boxed,
                                      nullptr, /*keep_globals=*/true);
  }
  else {
    static const wolang::FileResolution kNothing;
    const CheckedFile* checked =
        vm->database.Checked(FileId{entry->function->source_file});
    wolang::abi::LayoutStore layouts(
        vm->database.types(),
        checked != nullptr ? checked->resolution : kNothing);
    vm->database.ShareMembers(&layouts);
    wolang::interp::Limits limits;
    limits.keep_globals = true;
    if (vm->steps != 0) {
      limits.steps = vm->steps;
    }
    wolang::interp::Output printed;
    outcome = wolang::interp::RunNamed(
        vm->database.interner(), vm->database.types(), *entry->module,
        entry->symbol, boxed, limits, &printed, vm->foreign.get(),
        &layouts, vm->linked);
    if (!printed.text.empty()) {
      Written(vm, printed.text);
    }
  }

  if (!outcome.ok()) {
    vm->errors = std::string("wolang: ") +
                 std::string(wolang::interp::TrapKindName(outcome.trap)) +
                 " at " + outcome.where + "\n";
    return kWolangTrap;
  }
  if (out != nullptr) {
    *out = Unboxed(vm, outcome.value);
    const wolang::TypeStore& types = vm->database.types();
    const TypeId returned = types.ElementOf(entry->function->type);
    if (types.IsHandle(returned) && !types.IsOptional(returned) && outcome.value.kind() == wolang::interp::Value::Kind::kInt) {
      *out = ObjectOf(vm, outcome.value.integer(), returned);
      Handed(vm, *out);
    }
  }
  return kWolangOk;
}

// The members an object or a value of |type| stores, in the order its
// layout counts them -- the root class first -- and whether a class
// descriptor comes before all of them.
static std::vector<const wolang::Member*> StoredMembers(const wolang::FileResolution& resolution, TypeId type, bool* header) {
  std::vector<TypeId> chain;
  for (TypeId current = type; current.is_valid(); current = resolution.BaseOf(current)) {
    chain.push_back(current);
  }
  std::reverse(chain.begin(), chain.end());

  *header = false;
  std::vector<const wolang::Member*> stored;
  for (TypeId current : chain) {
    for (const wolang::Member& member : resolution.MembersOf(current)) {
      *header = *header || (member.kind == wolang::MemberKind::kMethod && (member.is_virtual || member.is_override));
      if (wolang::HasStorage(member.kind)) {
        stored.push_back(&member);
      }
    }
  }
  return stored;
}

// The class called |name| in |database|, by the spelling an object answers
// for its type; invalid when the program has no such class.
static TypeId FindClassNamed(Database& database, const std::string& name) {
  const wolang::TypeStore& types = database.types();
  for (size_t i = 0; i < types.size(); ++i) {
    const TypeId id{static_cast<uint32_t>(i)};
    const TypeKind kind = types.KindOf(id);
    if ((kind == TypeKind::kNamed || kind == TypeKind::kInstance) && types.IsHandle(id) && !types.IsForeignClass(id) && types.Describe(database.interner(), id) == name) {
      return id;
    }
  }
  return TypeId{};
}

static const Callable* FindMember(const WolangVm* vm, const std::string& name) {
  for (auto it = vm->members.rbegin(); it != vm->members.rend(); ++it) {
    if (it->name == name) {
      return &*it;
    }
  }
  return nullptr;
}

// An object whose last reference just went: its class's `$drop` runs the
// `deinit` and gives the storage back.
static int32_t Destroyed(WolangVm* vm, void* object, const std::string& type) {
  if (const Callable* drop = FindMember(vm, type + "$drop")) {
    std::vector<wolang::interp::Value> boxed;
    boxed.push_back(wolang::interp::Value::Int(reinterpret_cast<uint64_t>(object), wolang::IntWidth::k64, /*is_signed=*/false));
    return Invoke(vm, drop, boxed, nullptr);
  }
  wolang::runtime::wolang_object_free(object);
  return 0;
}

WolangStatus wolang_vm_object_retain(WolangVm* vm, WolangValue object) {
  const VmTurn turn(vm);
  if (vm == nullptr || object.kind != kWolangObject || object.handle == nullptr) {
    return kWolangError;
  }
  wolang::runtime::wolang_object_retain(object.handle);
  WolangVm::Held& entry = vm->held[object.handle];
  entry.Count += 1;
  entry.Type.assign(object.text != nullptr ? object.text : "", static_cast<size_t>(object.length));
  entry.Index = object.reserved;
  return kWolangOk;
}

WolangStatus wolang_vm_object_release(WolangVm* vm, WolangValue object) {
  const VmTurn turn(vm);
  if (vm == nullptr || object.kind != kWolangObject || object.handle == nullptr) {
    return kWolangError;
  }
  if (const auto found = vm->held.find(object.handle); found != vm->held.end() && --found->second.Count <= 0) {
    vm->held.erase(found);
  }
  if (wolang::runtime::wolang_object_release(object.handle) == 0) {
    return kWolangOk;
  }

  return Destroyed(vm, object.handle, std::string(object.text != nullptr ? object.text : "", static_cast<size_t>(object.length))) != 0 ? kWolangError : kWolangOk;
}

WolangStatus wolang_vm_object_call(WolangVm* vm, WolangValue object, const char* method, const WolangValue* arguments, int32_t count, WolangValue* out) {
  const VmTurn turn(vm);
  if (vm == nullptr || !vm->ok || object.kind != kWolangObject || object.handle == nullptr || method == nullptr) {
    return kWolangError;
  }
  const std::string type(object.text != nullptr ? object.text : "", static_cast<size_t>(object.length));
  const Callable* entry = FindMember(vm, type + "." + method);
  if (entry == nullptr) {
    vm->errors = "wolang: `" + type + "` has no method called `" + method + "`\n";
    return kWolangError;
  }

  const std::vector<TypeId> params = vm->database.types().ArgumentsOf(entry->function->type);
  if (params.size() != static_cast<size_t>(count < 0 ? 0 : count) + 1) {
    vm->errors = "wolang: `" + entry->name + "` takes " + std::to_string(params.size() - 1) + " arguments\n";
    return kWolangError;
  }
  std::vector<wolang::interp::Value> boxed;
  boxed.push_back(wolang::interp::Value::Int(reinterpret_cast<uint64_t>(object.handle), wolang::IntWidth::k64, /*is_signed=*/false));
  for (int32_t i = 0; i < count && arguments != nullptr; ++i) {
    boxed.push_back(Boxed(vm, arguments[i], params[static_cast<size_t>(i) + 1]));
  }
  return Invoke(vm, entry, boxed, out);
}

WolangStatus wolang_vm_object_get(WolangVm* vm, WolangValue object, const char* field, WolangValue* out) {
  const VmTurn turn(vm);
  if (vm == nullptr || object.kind != kWolangObject || object.handle == nullptr || field == nullptr || out == nullptr) {
    return kWolangError;
  }
  wolang::TypeStore& types = vm->database.types();
  TypeId type = FindClassNamed(vm->database, std::string(object.text != nullptr ? object.text : "", static_cast<size_t>(object.length)));
  if (!type.is_valid()) {
    type = TypeId{object.reserved};
  }
  const uint32_t file = types.Get(type).declaring_file;
  const CheckedFile* checked = vm->database.Checked(FileId{file});
  if (checked == nullptr) {
    return kWolangError;
  }
  const wolang::FileResolution& resolution = checked->resolution;

  bool header = false;
  const std::vector<const wolang::Member*> stored = StoredMembers(resolution, type, &header);
  const wolang::Symbol name = vm->database.interner().Lookup(field);
  for (size_t i = 0; i < stored.size(); ++i) {
    if (stored[i]->name != name) {
      continue;
    }
    wolang::abi::LayoutStore layouts(types, resolution);
    vm->database.ShareMembers(&layouts);
    const wolang::abi::Layout& layout = layouts.ObjectOf(type);
    const size_t index = i + (header ? 1 : 0);
    if (index >= layout.offsets.size()) {
      return kWolangError;
    }
    const auto* bytes = static_cast<const uint8_t*>(object.handle) + layout.offsets[index];
    const wolang::interp::Value value = wolang::interp::Decode(types, &layouts, bytes, stored[i]->type);
    *out = Unboxed(vm, value);
    if (types.IsHandle(stored[i]->type) && !types.IsOptional(stored[i]->type) && value.kind() == wolang::interp::Value::Kind::kInt) {
      *out = ObjectOf(vm, value.integer(), stored[i]->type);
    }
    return kWolangOk;
  }
  vm->errors = std::string("wolang: no field called `") + field + "`\n";
  return kWolangError;
}

WolangStatus wolang_vm_call(WolangVm* vm,
                       const char* name,
                       const WolangValue* arguments,
                       int32_t count,
                       WolangValue* out) {
  const VmTurn turn(vm);
  if (vm == nullptr) {
    return kWolangError;
  }
  if (!vm->ok) {
    return kWolangError;
  }
  if (Ambiguous(vm, name)) {
    vm->errors = std::string("wolang: `") + name + "` is offered by two modules; write `module." + name + "`\n";
    return kWolangError;
  }
  const Callable* entry = Find(vm, name, count < 0 ? 0 : count);
  if (entry == nullptr) {
    vm->errors = std::string("wolang: no function called `") +
                 (name == nullptr ? "" : name) + "`\n";
    return kWolangError;
  }

  const std::vector<TypeId> params =
      vm->database.types().ArgumentsOf(entry->function->type);
  std::vector<wolang::interp::Value> boxed;
  for (int32_t i = 0; i < count && arguments != nullptr; ++i) {
    const TypeId wanted = static_cast<size_t>(i) < params.size()
                              ? params[static_cast<size_t>(i)]
                              : TypeId{};
    boxed.push_back(Boxed(vm, arguments[i], wanted));
  }
  if (boxed.size() != params.size()) {
    vm->errors = std::string("wolang: `") + entry->name + "` takes " +
                 std::to_string(params.size()) + " arguments\n";
    return kWolangError;
  }

  return Invoke(vm, entry, boxed, out);
}

// ------------------------------------------------------------- handles ---

namespace {

// How many bytes an integer of this width takes.
size_t BytesOfWidth(wolang::IntWidth width) {
  switch (width) {
    case wolang::IntWidth::k8:
      return 1;
    case wolang::IntWidth::k16:
      return 2;
    case wolang::IntWidth::k32:
      return 4;
    case wolang::IntWidth::k64:
      return 8;
  }
  return 8;
}

// What kind of thing a Wolang type is, as the C side sees it.
WolangKind KindOf(const wolang::TypeStore& types, TypeId type) {
  if (!type.is_valid()) {
    return kWolangVoid;
  }
  switch (types.KindOf(type)) {
    case TypeKind::kBool:
      return kWolangBool;
    case TypeKind::kInt:
    case TypeKind::kChar:
      return kWolangInt;
    case TypeKind::kFloat:
      return kWolangFloat;
    case TypeKind::kStr:
    case TypeKind::kString:
      return kWolangText;
    default:
      break;
  }

  const TypeId held = types.IsOptional(type) ? types.ElementOf(type) : type;
  if (types.IsForeignClass(held)) {
    return kWolangHandle;
  }
  if (types.IsHandle(held)) {
    return kWolangObject;
  }
  return kWolangVoid;
}

}  // namespace

// ------------------------------------------------------------- classes ---

// A class the host's own files declare, and where to read its attributes.
struct ClassInfo {
  std::string Name;
  const wolang::ParsedFile* Parsed = nullptr;
  const wolang::Item* Declared = nullptr;
};

static std::vector<ClassInfo> ClassesOf(WolangVm* vm) {
  std::vector<ClassInfo> classes;
  for (FileId file : vm->own) {
    const wolang::ParsedFile* parsed = vm->database.Parse(file);
    if (parsed == nullptr) {
      continue;
    }
    for (const wolang::ItemId id : parsed->ast.Get(parsed->ast.root())) {
      const wolang::Item& item = parsed->ast.Get(id);
      if (item.kind != wolang::ItemKind::kClass) {
        continue;
      }
      ClassInfo info;
      info.Name = std::string(vm->database.interner().Text(item.name));
      info.Parsed = parsed;
      info.Declared = &item;
      classes.push_back(std::move(info));
    }
  }
  return classes;
}

static const ClassInfo* ClassNamed(const std::vector<ClassInfo>& classes, const char* name) {
  for (const ClassInfo& info : classes) {
    if (name != nullptr && info.Name == name) {
      return &info;
    }
  }
  return nullptr;
}

static const char* Kept(WolangVm* vm, const std::string& text) {
  vm->answers.push_back(text);
  return vm->answers.back().c_str();
}

// The stored fields of the class called |name|, or none when the program
// has no such class.
static std::vector<const wolang::Member*> FieldsOfClass(WolangVm* vm, const std::string& name, TypeId* type, bool* header) {
  *header = false;
  *type = FindClassNamed(vm->database, name);
  if (!type->is_valid()) {
    return {};
  }
  const CheckedFile* checked = vm->database.Checked(FileId{vm->database.types().Get(*type).declaring_file});
  if (checked == nullptr) {
    return {};
  }
  return StoredMembers(checked->resolution, *type, header);
}

// A field the host can fill from a number or a flag, and that reads as a
// sensible value while it is zero (D258).
static bool Settable(const wolang::TypeStore& types, TypeId type) {
  const TypeKind kind = types.KindOf(type);
  return kind == TypeKind::kBool || kind == TypeKind::kInt || kind == TypeKind::kChar || kind == TypeKind::kFloat;
}

WolangStatus wolang_vm_object_new(WolangVm* vm, const char* class_name, WolangValue* out) {
  const VmTurn turn(vm);
  if (vm == nullptr || !vm->ok || class_name == nullptr || out == nullptr) {
    return kWolangError;
  }
  TypeId type;
  bool header = false;
  const std::vector<const wolang::Member*> stored = FieldsOfClass(vm, class_name, &type, &header);
  if (!type.is_valid()) {
    vm->errors = std::string("wolang: no class called `") + class_name + "`\n";
    return kWolangError;
  }
  const wolang::TypeStore& types = vm->database.types();
  if (header) {
    vm->errors = std::string("wolang: `") + class_name + "` has virtual methods; make it in a script function\n";
    return kWolangError;
  }
  for (const wolang::Member* member : stored) {
    if (!Settable(types, member->type) && !types.IsOptional(member->type)) {
      vm->errors = std::string("wolang: `") + class_name + "` holds fields only a script function can fill\n";
      return kWolangError;
    }
  }

  const CheckedFile* checked = vm->database.Checked(FileId{types.Get(type).declaring_file});
  wolang::abi::LayoutStore layouts(vm->database.types(), checked->resolution);
  vm->database.ShareMembers(&layouts);
  void* object = wolang::runtime::wolang_object_alloc(std::max(layouts.ObjectOf(type).size, 1u));
  if (object == nullptr) {
    return kWolangError;
  }
  *out = ObjectOf(vm, reinterpret_cast<uint64_t>(object), type);
  Handed(vm, *out);
  return kWolangOk;
}

WolangStatus wolang_vm_object_set(WolangVm* vm, WolangValue object, const char* field, WolangValue value) {
  const VmTurn turn(vm);
  if (vm == nullptr || !vm->ok || object.kind != kWolangObject || object.handle == nullptr || field == nullptr) {
    return kWolangError;
  }
  TypeId type;
  bool header = false;
  const std::vector<const wolang::Member*> stored = FieldsOfClass(vm, std::string(object.text != nullptr ? object.text : "", static_cast<size_t>(object.length)), &type, &header);
  if (!type.is_valid()) {
    return kWolangError;
  }
  wolang::TypeStore& types = vm->database.types();
  const wolang::Symbol name = vm->database.interner().Lookup(field);
  for (size_t i = 0; i < stored.size(); ++i) {
    if (stored[i]->name != name) {
      continue;
    }
    if (!Settable(types, stored[i]->type) || value.kind == kWolangText || value.kind == kWolangObject || value.kind == kWolangHandle) {
      vm->errors = std::string("wolang: `") + field + "` takes a number or a flag from the host\n";
      return kWolangError;
    }
    const CheckedFile* checked = vm->database.Checked(FileId{types.Get(type).declaring_file});
    wolang::abi::LayoutStore layouts(types, checked->resolution);
    vm->database.ShareMembers(&layouts);
    const wolang::abi::Layout& layout = layouts.ObjectOf(type);
    const size_t index = i + (header ? 1 : 0);
    if (index >= layout.offsets.size()) {
      return kWolangError;
    }
    wolang::interp::Encode(types, &layouts, Boxed(vm, value, stored[i]->type), stored[i]->type, static_cast<uint8_t*>(object.handle) + layout.offsets[index]);
    return kWolangOk;
  }
  vm->errors = std::string("wolang: no field called `") + field + "`\n";
  return kWolangError;
}

int32_t wolang_vm_class_count(const WolangVm* vm) {
  const VmTurn turn(vm);
  return vm == nullptr ? 0 : static_cast<int32_t>(ClassesOf(const_cast<WolangVm*>(vm)).size());
}

const char* wolang_vm_class_name(const WolangVm* vm, int32_t index) {
  const VmTurn turn(vm);
  if (vm == nullptr) {
    return "";
  }
  WolangVm* mutable_vm = const_cast<WolangVm*>(vm);
  const std::vector<ClassInfo> classes = ClassesOf(mutable_vm);
  return index < 0 || static_cast<size_t>(index) >= classes.size() ? "" : Kept(mutable_vm, classes[static_cast<size_t>(index)].Name);
}

int32_t wolang_vm_class_field_count(const WolangVm* vm, const char* class_name) {
  const VmTurn turn(vm);
  if (vm == nullptr || class_name == nullptr) {
    return 0;
  }
  TypeId type;
  bool header = false;
  return static_cast<int32_t>(FieldsOfClass(const_cast<WolangVm*>(vm), class_name, &type, &header).size());
}

const char* wolang_vm_class_field_name(const WolangVm* vm, const char* class_name, int32_t index) {
  const VmTurn turn(vm);
  if (vm == nullptr || class_name == nullptr) {
    return "";
  }
  WolangVm* mutable_vm = const_cast<WolangVm*>(vm);
  TypeId type;
  bool header = false;
  const std::vector<const wolang::Member*> stored = FieldsOfClass(mutable_vm, class_name, &type, &header);
  return index < 0 || static_cast<size_t>(index) >= stored.size() ? "" : Kept(mutable_vm, std::string(mutable_vm->database.interner().Text(stored[static_cast<size_t>(index)]->name)));
}

WolangKind wolang_vm_class_field_kind(const WolangVm* vm, const char* class_name, int32_t index) {
  const VmTurn turn(vm);
  if (vm == nullptr || class_name == nullptr) {
    return kWolangVoid;
  }
  WolangVm* mutable_vm = const_cast<WolangVm*>(vm);
  TypeId type;
  bool header = false;
  const std::vector<const wolang::Member*> stored = FieldsOfClass(mutable_vm, class_name, &type, &header);
  return index < 0 || static_cast<size_t>(index) >= stored.size() ? kWolangVoid : KindOf(mutable_vm->database.types(), stored[static_cast<size_t>(index)]->type);
}

int32_t wolang_vm_class_attribute_count(const WolangVm* vm, const char* class_name) {
  const VmTurn turn(vm);
  if (vm == nullptr) {
    return 0;
  }
  const std::vector<ClassInfo> classes = ClassesOf(const_cast<WolangVm*>(vm));
  const ClassInfo* info = ClassNamed(classes, class_name);
  return info == nullptr ? 0 : static_cast<int32_t>(info->Parsed->ast.Get(info->Declared->attributes).size());
}

const char* wolang_vm_class_attribute_name(const WolangVm* vm, const char* class_name, int32_t index) {
  const VmTurn turn(vm);
  if (vm == nullptr) {
    return "";
  }
  WolangVm* mutable_vm = const_cast<WolangVm*>(vm);
  const std::vector<ClassInfo> classes = ClassesOf(mutable_vm);
  const ClassInfo* info = ClassNamed(classes, class_name);
  if (info == nullptr) {
    return "";
  }
  const auto attributes = info->Parsed->ast.Get(info->Declared->attributes);
  if (index < 0 || static_cast<size_t>(index) >= attributes.size()) {
    return "";
  }
  return Kept(mutable_vm, std::string(mutable_vm->database.interner().Text(info->Parsed->ast.Get(attributes[static_cast<size_t>(index)]).name)));
}

WolangFunction* wolang_vm_function(WolangVm* vm, const char* name) {
  const VmTurn turn(vm);
  if (Find(vm, name) == nullptr) {
    return nullptr;
  }
  for (FunctionHandle& handle : *vm->handles) {
    if (handle.name == name) {
      return reinterpret_cast<WolangFunction*>(&handle);
    }
  }
  FunctionHandle handle;
  handle.vm = vm;
  handle.name = name;
  vm->handles->push_back(std::move(handle));
  return reinterpret_cast<WolangFunction*>(&vm->handles->back());
}

static const FunctionHandle* HandleOf(const WolangFunction* function) {
  return reinterpret_cast<const FunctionHandle*>(function);
}

// The function a handle names in the program as it is now.
static const Callable* Resolved(const WolangFunction* function) {
  if (function == nullptr || HandleOf(function)->vm == nullptr) {
    return nullptr;
  }
  const Callable* entry = Find(HandleOf(function)->vm, HandleOf(function)->name.c_str());
  return entry != nullptr && entry->function != nullptr ? entry : nullptr;
}

// One word of a prepared call, for a value of |type|; `kVoid` when it is
// not one word.
static Word WordOf(const wolang::TypeStore& types, TypeId type) {
  Word word;
  word.type = type;
  if (!type.is_valid()) {
    return word;
  }
  switch (types.KindOf(type)) {
    case TypeKind::kBool:
      word.kind = WordKind::kBool;
      return word;
    case TypeKind::kInt:
      word.kind = WordKind::kInt;
      word.bytes = static_cast<uint8_t>(BytesOfWidth(types.Get(type).int_width));
      word.is_signed = types.Get(type).is_signed;
      return word;
    case TypeKind::kFloat:
      word.kind = types.Get(type).float_width == wolang::FloatWidth::k32 ? WordKind::kFloat32 : WordKind::kFloat64;
      return word;
    default:
      break;
  }
  if (types.IsHandle(type) && !types.IsOptional(type)) {
    word.kind = WordKind::kHandle;
  }
  return word;
}

static void PrepareHandle(WolangVm* vm, FunctionHandle* handle) {
  handle->generation = vm->generation;
  handle->prepared = 0;
  handle->words.clear();
  const Callable* entry = Find(vm, handle->name.c_str());
  if (entry == nullptr || entry->function == nullptr || vm->compiled == nullptr) {
    return;
  }

  const wolang::TypeStore& types = vm->database.types();
  for (TypeId param : types.ArgumentsOf(entry->function->type)) {
    const Word word = WordOf(types, param);
    if (word.kind == WordKind::kVoid || handle->words.size() == 16) {
      handle->words.clear();
      return;
    }
    handle->words.push_back(word);
  }
  const TypeId returned = types.ElementOf(entry->function->type);
  handle->result = WordOf(types, returned);
  if (handle->result.kind == WordKind::kVoid && returned.is_valid() && types.KindOf(returned) != TypeKind::kVoid) {
    handle->words.clear();
    return;
  }
  handle->prepared = vm->compiled->Prepare(entry->function->source_file, entry->function->source_item);
}

// A host's value as the word a stub takes.
static uint64_t SlotOf(const Word& word, const WolangValue& value) {
  uint64_t slot = 0;
  switch (word.kind) {
    case WordKind::kInt:
      slot = static_cast<uint64_t>(value.kind == kWolangFloat ? static_cast<int64_t>(value.number) : value.integer);
      return slot;
    case WordKind::kFloat32: {
      const auto number = static_cast<float>(value.kind == kWolangInt ? static_cast<double>(value.integer) : value.number);
      std::memcpy(&slot, &number, sizeof(number));
      return slot;
    }
    case WordKind::kFloat64: {
      const double number = value.kind == kWolangInt ? static_cast<double>(value.integer) : value.number;
      std::memcpy(&slot, &number, sizeof(number));
      return slot;
    }
    case WordKind::kBool:
      return (value.kind == kWolangInt ? value.integer != 0 : value.boolean != 0) ? 1 : 0;
    case WordKind::kHandle:
      return reinterpret_cast<uint64_t>(value.handle);
    default:
      return 0;
  }
}

// A stub's answer as the host's value.
static WolangValue AnswerOf(WolangVm* vm, const Word& word, const uint8_t* bytes) {
  switch (word.kind) {
    case WordKind::kInt: {
      uint64_t raw = 0;
      std::memcpy(&raw, bytes, word.bytes);
      if (word.is_signed && word.bytes < 8) {
        const uint64_t sign = uint64_t{1} << (word.bytes * 8 - 1);
        raw = (raw ^ sign) - sign;
      }
      return wolang_int(static_cast<int64_t>(raw));
    }
    case WordKind::kFloat32: {
      float number = 0.0f;
      std::memcpy(&number, bytes, sizeof(number));
      return wolang_float(number);
    }
    case WordKind::kFloat64: {
      double number = 0.0;
      std::memcpy(&number, bytes, sizeof(number));
      return wolang_float(number);
    }
    case WordKind::kBool:
      return wolang_bool((bytes[0] & 1) != 0);
    case WordKind::kHandle: {
      uint64_t address = 0;
      std::memcpy(&address, bytes, sizeof(address));
      return ObjectOf(vm, address, word.type);
    }
    default:
      return wolang_void();
  }
}

WolangStatus wolang_call(WolangFunction* function,
                    const WolangValue* arguments,
                    int32_t count,
                    WolangValue* out) {
  if (function == nullptr || HandleOf(function)->vm == nullptr) {
    return kWolangError;
  }
  auto* handle = reinterpret_cast<FunctionHandle*>(function);
  WolangVm* vm = handle->vm;
  const VmTurn turn(vm);

  // A frame's call into a script: the words straight into the compiled
  // stub. Anything that needs more -- a string, a step limit, an editor
  // attached -- goes the long way (D219).
  if (vm->ok && vm->allow_compiled && vm->steps == 0 && vm->compiled != nullptr && Attached(vm) == nullptr) {
    if (handle->generation != vm->generation) {
      PrepareHandle(vm, handle);
    }
    if (handle->prepared != 0 && count == static_cast<int32_t>(handle->words.size()) && (count == 0 || arguments != nullptr)) {
      uint64_t slots[16] = {};
      for (int32_t i = 0; i < count; ++i) {
        slots[i] = SlotOf(handle->words[static_cast<size_t>(i)], arguments[i]);
      }
      alignas(16) uint8_t result[16] = {};

      wolang::interp::Outcome trapped;
      if (!vm->compiled->CallPrepared(handle->prepared, slots, result, &trapped)) {
        vm->errors = std::string("wolang: ") + std::string(wolang::interp::TrapKindName(trapped.trap)) + " at " + trapped.where + "\n";
        return kWolangTrap;
      }
      if (out != nullptr) {
        *out = AnswerOf(vm, handle->result, result);
      }
      return kWolangOk;
    }
  }
  return wolang_vm_call(vm, handle->name.c_str(), arguments, count, out);
}


int32_t wolang_function_arity(const WolangFunction* function) {
  const VmTurn turn(function != nullptr ? HandleOf(function)->vm : nullptr);
  const Callable* entry = Resolved(function);
  if (entry == nullptr) {
    return -1;
  }
  return static_cast<int32_t>(entry->vm->database.types().ArgumentsOf(entry->function->type).size());
}

WolangKind wolang_function_result(const WolangFunction* function) {
  const VmTurn turn(function != nullptr ? HandleOf(function)->vm : nullptr);
  const Callable* entry = Resolved(function);
  if (entry == nullptr) {
    return kWolangVoid;
  }
  const wolang::TypeStore& types = entry->vm->database.types();
  return KindOf(types, types.ElementOf(entry->function->type));
}

WolangKind wolang_function_parameter(const WolangFunction* function,
                                     int32_t index) {
  const VmTurn turn(function != nullptr ? HandleOf(function)->vm : nullptr);
  const Callable* entry = Resolved(function);
  if (entry == nullptr || index < 0) {
    return kWolangVoid;
  }
  const wolang::TypeStore& types = entry->vm->database.types();
  const std::vector<TypeId> params = types.ArgumentsOf(entry->function->type);
  if (static_cast<size_t>(index) >= params.size()) {
    return kWolangVoid;
  }
  return KindOf(types, params[static_cast<size_t>(index)]);
}

const char* wolang_function_doc(const WolangFunction* function) {
  const VmTurn turn(function != nullptr ? HandleOf(function)->vm : nullptr);
  const Callable* entry = Resolved(function);
  return entry != nullptr ? entry->doc.c_str() : "";
}

// ------------------------------------------------------------- globals ---

namespace {

// The storage of a module-level `var` of the program, or null.
//
// The runtime owns a global's storage and hands out its address by the
// (file, item) pair the declaration came from -- the same token every
// tier uses, which is what makes a global written by compiled code the
// one the interpreter reads (D88).
void* GlobalStorage(WolangVm* vm, const char* name, TypeId* type) {
  if (vm == nullptr || name == nullptr) {
    return nullptr;
  }
  const Symbol symbol = vm->database.interner().Lookup(name);
  if (!symbol.is_valid()) {
    return nullptr;
  }
  for (FileId file : vm->own) {
    const CheckedFile* checked = vm->database.Checked(file);
    if (checked == nullptr) {
      continue;
    }
    const wolang::Binding* binding = checked->resolution.Find(symbol);
    if (binding == nullptr || !binding->is_mutable ||
        !binding->type.is_valid()) {
      continue;
    }
    static const wolang::FileResolution kNothing;
    wolang::abi::LayoutStore layouts(vm->database.types(),
                                     checked->resolution);
    vm->database.ShareMembers(&layouts);
    const uint32_t size =
        std::max(layouts.SizeOf(binding->type), 1u);
    const uint64_t token =
        (static_cast<uint64_t>(binding->file) << 32) | binding->item;
    if (type != nullptr) {
      *type = binding->type;
    }
    return wolang::runtime::wolang_global_address(token, size);
  }
  return nullptr;
}

}  // namespace

WolangStatus wolang_vm_get_global(WolangVm* vm,
                             const char* name,
                             WolangValue* out) {
  const VmTurn turn(vm);
  TypeId type;
  void* storage = GlobalStorage(vm, name, &type);
  if (storage == nullptr || out == nullptr) {
    return kWolangError;
  }
  const wolang::TypeStore& types = vm->database.types();
  switch (KindOf(types, type)) {
    case kWolangInt: {
      int64_t held = 0;
      std::memcpy(&held, storage,
                  std::min<size_t>(sizeof(held),
                                   BytesOfWidth(types.Get(type).int_width)));
      *out = wolang_int(held);
      return kWolangOk;
    }
    case kWolangFloat: {
      if (types.Get(type).float_width == wolang::FloatWidth::k32) {
        float held = 0;
        std::memcpy(&held, storage, sizeof(held));
        *out = wolang_float(held);
        return kWolangOk;
      }
      double held = 0;
      std::memcpy(&held, storage, sizeof(held));
      *out = wolang_float(held);
      return kWolangOk;
    }
    case kWolangBool: {
      uint8_t held = 0;
      std::memcpy(&held, storage, sizeof(held));
      *out = wolang_bool(held != 0);
      return kWolangOk;
    }
    case kWolangText: {
      // A string is where its bytes are and how many: copied out, for
      // the same reason an answered one is (D128).
      const uint8_t* bytes = nullptr;
      uint64_t length = 0;
      std::memcpy(&bytes, storage, sizeof(bytes));
      std::memcpy(&length, static_cast<uint8_t*>(storage) + 8,
                  sizeof(length));
      vm->answers.emplace_back(
          bytes == nullptr ? std::string()
                           : std::string(reinterpret_cast<const char*>(bytes),
                                         static_cast<size_t>(length)));
      *out = wolang_text(vm->answers.back().c_str(),
                         vm->answers.back().size());
      return kWolangOk;
    }
    default:
      return kWolangError;
  }
}

WolangStatus wolang_vm_set_global(WolangVm* vm,
                             const char* name,
                             WolangValue value) {
  const VmTurn turn(vm);
  TypeId type;
  void* storage = GlobalStorage(vm, name, &type);
  if (storage == nullptr) {
    return kWolangError;
  }
  const wolang::TypeStore& types = vm->database.types();
  switch (KindOf(types, type)) {
    case kWolangInt: {
      const int64_t held = value.kind == kWolangFloat
                               ? static_cast<int64_t>(value.number)
                               : value.integer;
      std::memcpy(storage, &held,
                  std::min<size_t>(sizeof(held),
                                   BytesOfWidth(types.Get(type).int_width)));
      return kWolangOk;
    }
    case kWolangFloat: {
      const double held = value.kind == kWolangInt
                              ? static_cast<double>(value.integer)
                              : value.number;
      if (types.Get(type).float_width == wolang::FloatWidth::k32) {
        const auto narrow = static_cast<float>(held);
        std::memcpy(storage, &narrow, sizeof(narrow));
        return kWolangOk;
      }
      std::memcpy(storage, &held, sizeof(held));
      return kWolangOk;
    }
    case kWolangBool: {
      const uint8_t held = value.kind == kWolangInt
                               ? (value.integer != 0 ? 1 : 0)
                               : (value.boolean != 0 ? 1 : 0);
      std::memcpy(storage, &held, sizeof(held));
      return kWolangOk;
    }
    case kWolangText: {
      // The host's bytes, borrowed: a global pointing at memory the host
      // frees is the host's mistake to avoid, and copying would need an
      // allocator whose storage nothing frees.
      const auto* bytes = reinterpret_cast<const uint8_t*>(value.text);
      const uint64_t length = value.length;
      std::memcpy(storage, &bytes, sizeof(bytes));
      std::memcpy(static_cast<uint8_t*>(storage) + 8, &length,
                  sizeof(length));
      return kWolangOk;
    }
    default:
      return kWolangError;
  }
}

// -------------------------------------------------------------- output ---

namespace {

// What `std.io` calls, through the symbol `write` and through
// `wolang_fd_write`. Both reach the writer of the vm whose turn it is,
// found in the space that turn entered (D257).
int64_t FdWriteThroughHost(void* user, int32_t fd, const uint8_t* bytes, uint64_t count) {
  WolangVm* vm = static_cast<WolangVm*>(user);
  if (fd == 1 && vm->writer != nullptr) {
    vm->writer(vm->writer_user, reinterpret_cast<const char*>(bytes), count);
    return static_cast<int64_t>(count);
  }
  if (fd == 2 && vm->reporter != nullptr) {
    vm->reporter(vm->reporter_user, reinterpret_cast<const char*>(bytes), count);
    return static_cast<int64_t>(count);
  }
  return -1;
}

extern "C" int64_t WriteThroughHost(int32_t fd, const char* bytes, uint64_t count) {
  int64_t answered = 0;
  if (wolang::runtime::SpaceWrite(fd, reinterpret_cast<const uint8_t*>(bytes), count, &answered)) {
    return answered;
  }
  return static_cast<int64_t>(::fwrite(bytes, 1, static_cast<size_t>(count), fd == 2 ? stderr : stdout));
}

}  // namespace

// Inside the `extern "C"` block, and declared above with C++ linkage:
// gcc gives an unnamed namespace here C linkage unless told otherwise.
extern "C++" {
namespace {

void Written(WolangVm* vm, const std::string& text) {
  if (vm != nullptr && vm->writer != nullptr) {
    vm->writer(vm->writer_user, text.data(), text.size());
    return;
  }
  std::fwrite(text.data(), 1, text.size(), stdout);
}

}  // namespace
}  // extern "C++"

static void InstallWriter(WolangVm* vm) {
  wolang::runtime::SpaceBindSymbol(vm->globals.get(), "write", reinterpret_cast<void*>(&WriteThroughHost));
  wolang::runtime::SpaceSetWriteHook(vm->globals.get(), &FdWriteThroughHost, vm);
}

void wolang_vm_set_writer(WolangVm* vm, WolangWriter writer, void* user) {
  if (vm == nullptr) {
    return;
  }
  const VmTurn turn(vm);
  vm->writer = writer;
  vm->writer_user = user;
  InstallWriter(vm);
}

void wolang_vm_set_reporter(WolangVm* vm, WolangWriter reporter, void* user) {
  if (vm == nullptr) {
    return;
  }
  const VmTurn turn(vm);
  vm->reporter = reporter;
  vm->reporter_user = user;
  InstallWriter(vm);
}

// -------------------------------------------------------------- limits ---

void wolang_vm_set_step_limit(WolangVm* vm, uint64_t steps) {
  const VmTurn turn(vm);
  if (vm != nullptr) {
    vm->steps = steps;
  }
}

void wolang_vm_set_compiled(WolangVm* vm, int32_t allowed) {
  const VmTurn turn(vm);
  if (vm != nullptr) {
    vm->allow_compiled = allowed != 0;
  }
}

void wolang_vm_set_initialize(WolangVm* vm, int32_t initialize) {
  const VmTurn turn(vm);
  if (vm != nullptr) {
    vm->initialize = initialize != 0;
  }
}

// ------------------------------------------------------------- reloads ---

// A module-level `var` of the program, and where its value is.
struct GlobalSlot {
  std::string Name;
  TypeId Type;
  void* Storage = nullptr;
  const CheckedFile* Checked = nullptr;
  bool Weak = false;
};

// Every module-level `var` the host's own files declare, with storage in
// the globals the calling thread has entered.
static std::vector<GlobalSlot> GlobalsOf(WolangVm* vm, bool every_file = false) {
  std::vector<GlobalSlot> slots;
  for (FileId file : every_file ? vm->files : vm->own) {
    const CheckedFile* checked = vm->database.Checked(file);
    if (checked == nullptr) {
      continue;
    }
    for (const wolang::Binding& binding : checked->resolution.scope) {
      if (binding.kind != wolang::BindingKind::kValue || !binding.is_mutable || !binding.type.is_valid() || binding.item == wolang::Binding::kNoItem || binding.file != file.index) {
        continue;
      }
      wolang::abi::LayoutStore layouts(vm->database.types(), checked->resolution);
      vm->database.ShareMembers(&layouts);
      const uint32_t size = std::max(layouts.SizeOf(binding.type), 1u);
      const uint64_t token = (static_cast<uint64_t>(binding.file) << 32) | binding.item;
      GlobalSlot slot;
      slot.Name = std::string(vm->database.interner().Text(binding.name));
      slot.Type = binding.type;
      slot.Storage = wolang::runtime::wolang_global_address(token, size);
      slot.Checked = checked;
      slot.Weak = binding.is_weak;
      slots.push_back(std::move(slot));
    }
  }
  return slots;
}

// What a value of |type| is made of, spelled so that two programs answer
// the same text exactly when the bytes of one mean the same to the other:
// the type's name, and for a declared type every field it stores, by name
// and by shape, all the way down. A class with a descriptor has none,
// because which descriptor is a question about the code that made it.
static std::string ShapeOf(Database& database, TypeId type, std::vector<TypeId>* seen) {
  const wolang::TypeStore& types = database.types();
  std::string shape = types.Describe(database.interner(), type);
  const TypeKind kind = types.KindOf(type);
  if (kind == TypeKind::kOptional || kind == TypeKind::kFallible || kind == TypeKind::kArray || kind == TypeKind::kSlice) {
    return shape + "(" + ShapeOf(database, types.ElementOf(type), seen) + ")";
  }
  if ((kind != TypeKind::kNamed && kind != TypeKind::kInstance) || types.IsForeignClass(type)) {
    return shape;
  }
  if (std::find(seen->begin(), seen->end(), type) != seen->end()) {
    return shape;
  }
  seen->push_back(type);

  const CheckedFile* checked = database.Checked(FileId{types.Get(types.StripWrappers(type)).declaring_file});
  if (checked == nullptr) {
    return shape + "{?}";
  }
  bool header = false;
  const std::vector<const wolang::Member*> stored = StoredMembers(checked->resolution, type, &header);
  if (header) {
    return shape + "{virtual}";
  }
  shape += "{";
  for (const wolang::Member* member : stored) {
    shape += std::string(database.interner().Text(member->name)) + ":" + ShapeOf(database, member->type, seen) + ",";
  }
  for (const wolang::Member& member : checked->resolution.MembersOf(type)) {
    if (member.kind == wolang::MemberKind::kVariant) {
      shape += "|" + std::string(database.interner().Text(member.name)) + ":" + ShapeOf(database, member.type, seen);
    }
  }
  return shape + "}";
}

static std::string Shape(Database& database, TypeId type) {
  std::vector<TypeId> seen;
  return ShapeOf(database, type, &seen);
}

// A value the new program's initializer made, given back before what the
// old program had takes its place. What this does not know how to give
// back -- a struct holding objects -- is left, which costs memory and
// nothing else.
static void LetGo(WolangVm* vm, TypeId type, void* storage, bool weak = false) {
  const wolang::TypeStore& types = vm->database.types();
  if (types.KindOf(type) == TypeKind::kString) {
    const uint8_t* data = nullptr;
    std::memcpy(&data, storage, sizeof(data));
    wolang::runtime::wolang_text_free(data);
    return;
  }
  const TypeId held = types.IsOptional(type) ? types.ElementOf(type) : type;
  if (!types.IsHandle(held)) {
    return;
  }
  const uint8_t* at = static_cast<const uint8_t*>(storage);
  if (types.IsOptional(type)) {
    static const wolang::FileResolution kNothing;
    wolang::abi::LayoutStore layouts(types, kNothing);
    const wolang::abi::Layout& layout = layouts.Of(type);
    if (*at == 0 || layout.offsets.size() < 2) {
      return;
    }
    at += layout.offsets[1];
  }
  void* object = nullptr;
  std::memcpy(&object, at, sizeof(object));
  if (object == nullptr) {
    return;
  }
  if (types.IsForeignClass(held)) {
    const uint64_t tag = wolang::runtime::HostTag(types.Describe(vm->database.interner(), held));
    if (weak) {
      wolang::runtime::wolang_host_weak_free(tag, reinterpret_cast<uint64_t>(object));
    }
    else {
      wolang::runtime::wolang_host_release(tag, object);
    }
    return;
  }
  if (weak) {
    wolang::runtime::wolang_object_weak_release(object);
    return;
  }
  if (wolang::runtime::wolang_object_release(object) == 1) {
    Destroyed(vm, object, types.Describe(vm->database.interner(), types.StripWrappers(held)));
  }
}

// What a vm still holds when it goes away: every module-level `var` of
// every file it loaded, so a host object a global kept gets its release
// (D259).
static void ReleaseGlobals(WolangVm* vm) {
  for (const GlobalSlot& slot : GlobalsOf(vm, true)) {
    LetGo(vm, slot.Type, slot.Storage, slot.Weak);
  }
}

// An object whose class changed: the fields the new class still has, with
// the same shape, take the old object's values; the rest keep what the
// new initializer gave them.
static void CarryFields(WolangVm* vm, Database& before, const GlobalSlot& old, const GlobalSlot& now) {
  void* old_object = nullptr;
  void* new_object = nullptr;
  std::memcpy(&old_object, old.Storage, sizeof(old_object));
  std::memcpy(&new_object, now.Storage, sizeof(new_object));
  if (old_object == nullptr || new_object == nullptr) {
    vm->notes += "`" + now.Name + "` starts again: its class changed\n";
    return;
  }

  const wolang::TypeStore& old_types = before.types();
  const wolang::TypeStore& new_types = vm->database.types();
  const CheckedFile* old_checked = before.Checked(FileId{old_types.Get(old.Type).declaring_file});
  const CheckedFile* new_checked = vm->database.Checked(FileId{new_types.Get(now.Type).declaring_file});
  if (old_checked == nullptr || new_checked == nullptr) {
    return;
  }
  bool old_header = false;
  bool new_header = false;
  const std::vector<const wolang::Member*> old_fields = StoredMembers(old_checked->resolution, old.Type, &old_header);
  const std::vector<const wolang::Member*> new_fields = StoredMembers(new_checked->resolution, now.Type, &new_header);
  wolang::abi::LayoutStore old_layouts(old_types, old_checked->resolution);
  before.ShareMembers(&old_layouts);
  wolang::abi::LayoutStore new_layouts(new_types, new_checked->resolution);
  vm->database.ShareMembers(&new_layouts);
  const wolang::abi::Layout& old_layout = old_layouts.ObjectOf(old.Type);
  const wolang::abi::Layout& new_layout = new_layouts.ObjectOf(now.Type);

  for (size_t i = 0; i < new_fields.size(); ++i) {
    const std::string name(vm->database.interner().Text(new_fields[i]->name));
    size_t found = old_fields.size();
    for (size_t j = 0; j < old_fields.size(); ++j) {
      if (before.interner().Text(old_fields[j]->name) == name) {
        found = j;
      }
    }
    if (found == old_fields.size()) {
      continue;
    }
    const size_t new_index = i + (new_header ? 1 : 0);
    const size_t old_index = found + (old_header ? 1 : 0);
    if (new_index >= new_layout.offsets.size() || old_index >= old_layout.offsets.size()) {
      continue;
    }
    if (Shape(before, old_fields[found]->type) != Shape(vm->database, new_fields[i]->type)) {
      vm->notes += "`" + now.Name + "." + name + "` starts again: its type changed\n";
      continue;
    }
    auto* into = static_cast<uint8_t*>(new_object) + new_layout.offsets[new_index];
    LetGo(vm, new_fields[i]->type, into);
    std::memcpy(into, static_cast<uint8_t*>(old_object) + old_layout.offsets[old_index], std::max(new_layouts.SizeOf(new_fields[i]->type), 1u));
  }
}

// The old program's values, where the new program still has a place of
// the same name and shape for them.
//
// A value is moved rather than copied: the old program's globals are
// never read or released again, so what they held belongs to the new
// program from here on, references and all.
static void Carry(WolangVm* vm, Database& before, const std::vector<GlobalSlot>& old) {
  for (const GlobalSlot& now : GlobalsOf(vm)) {
    const GlobalSlot* match = nullptr;
    for (const GlobalSlot& slot : old) {
      if (slot.Name == now.Name) {
        match = &slot;
      }
    }
    if (match == nullptr) {
      continue;
    }
    if (Shape(before, match->Type) == Shape(vm->database, now.Type)) {
      wolang::abi::LayoutStore layouts(vm->database.types(), now.Checked->resolution);
      vm->database.ShareMembers(&layouts);
      LetGo(vm, now.Type, now.Storage, now.Weak);
      std::memcpy(now.Storage, match->Storage, std::max(layouts.SizeOf(now.Type), 1u));
      continue;
    }
    const wolang::TypeStore& old_types = before.types();
    const wolang::TypeStore& new_types = vm->database.types();
    const bool classes = old_types.IsHandle(match->Type) && new_types.IsHandle(now.Type) && !old_types.IsOptional(match->Type) && !new_types.IsOptional(now.Type) && !new_types.IsForeignClass(now.Type) && old_types.Describe(before.interner(), match->Type) == new_types.Describe(vm->database.interner(), now.Type);
    if (classes) {
      CarryFields(vm, before, *match, now);
      continue;
    }
    vm->notes += "`" + now.Name + "` starts again: its type changed\n";
  }
}

// What an object of |type| is made of, by field, the way two programs must
// agree for the same bytes to mean the same thing. Unlike `Shape` it looks
// inside a class with a descriptor, because an object the host holds is not
// rebuilt and so cannot be allowed to change under it (D255).
static std::string LayoutKey(Database& database, TypeId type) {
  const CheckedFile* checked = database.Checked(FileId{database.types().Get(type).declaring_file});
  if (checked == nullptr) {
    return "?";
  }
  bool header = false;
  std::string key;
  for (const wolang::Member* member : StoredMembers(checked->resolution, type, &header)) {
    key += std::string(database.interner().Text(member->name)) + ":" + Shape(database, member->type) + ",";
  }
  return (header ? "virtual;" : "plain;") + key;
}

// Why a reload would hurt an object the host holds, or nothing when it
// would not: |staged| is the new program, loaded and not yet running.
static std::string HeldObjectsBlock(WolangVm* vm, WolangVm* staged) {
  std::string blocked;
  for (const auto& [address, held] : vm->held) {
    (void)address;
    const TypeId before = FindClassNamed(vm->database, held.Type);
    const TypeId after = FindClassNamed(staged->database, held.Type);
    if (!before.is_valid() || !after.is_valid()) {
      blocked += "the host holds a `" + held.Type + "` and the new program has no such class\n";
      continue;
    }
    if (LayoutKey(vm->database, before) != LayoutKey(staged->database, after)) {
      blocked += "the host holds a `" + held.Type + "` and its fields changed\n";
    }
  }
  return blocked;
}

WolangStatus wolang_vm_reload(WolangVm* vm) {
  if (vm == nullptr) {
    return kWolangError;
  }
  const VmTurn turn(vm);
  const std::vector<WolangVm::Loaded> again = vm->loaded;

  // The new program is loaded beside the old one first, without starting
  // anything. Only a program that compiles, and that leaves every object
  // the host holds as it was, replaces the one running; otherwise the old
  // one keeps running and the vm says why (D255).
  {
    std::unique_ptr<WolangVm> staged = std::make_unique<WolangVm>();
    staged->initialize = false;
    staged->allow_compiled = false;
    staged->sandboxes = vm->sandboxes;
    wolang::runtime::SpaceCopyBindings(vm->globals.get(), staged->globals.get());
    int32_t staged_status = 0;
    for (const WolangVm::Loaded& file : again) {
      staged_status |= file.from_source ? wolang_vm_load_source(staged.get(), file.path.c_str(), file.source.c_str()) : wolang_vm_load(staged.get(), file.path.c_str());
    }
    if (staged_status != 0 || !staged->ok) {
      vm->errors = staged->errors;
      vm->notes = "the reload was refused; the program that was running keeps running\n";
      return kWolangError;
    }
    const std::string blocked = HeldObjectsBlock(vm, staged.get());
    if (!blocked.empty()) {
      vm->errors = "wolang: the reload was refused: " + blocked;
      vm->notes = blocked;
      return kWolangError;
    }
  }

  const std::vector<GlobalSlot> old = GlobalsOf(vm);

  // The program as it was goes into retirement rather than away: a value
  // carried over may point into it (D215).
  Retired retired;
  retired.database = std::move(vm->owned_database);
  retired.foreign = std::move(vm->foreign);
  retired.compiled = std::move(vm->compiled);
  retired.globals = vm->globals;
  Database& before = *retired.database;
  const std::shared_ptr<wolang::runtime::GlobalSpace> graveyard_space = vm->globals;
  const std::shared_ptr<std::recursive_mutex> lock = vm->lock;
  const std::shared_ptr<std::vector<Retired>> graveyard = vm->retired;
  const std::shared_ptr<std::deque<FunctionHandle>> handles = vm->handles;
  const std::shared_ptr<DebugLink> debug = vm->debug;
  const uint64_t steps = vm->steps;
  const bool allow_compiled = vm->allow_compiled;
  const bool initialize = vm->initialize;
  const std::vector<std::pair<std::string, std::vector<std::string>>> sandboxes = vm->sandboxes;
  const WolangWriter writer = vm->writer;
  void* const writer_user = vm->writer_user;
  const WolangWriter reporter = vm->reporter;
  void* const reporter_user = vm->reporter_user;
  std::map<void*, WolangVm::Held> held = std::move(vm->held);
  std::deque<std::string> answers = std::move(vm->answers);

  // A fresh database, because a query cache remembers what the files
  // said: reloading is loading again, not re-asking.
  vm->~WolangVm();
  new (vm) WolangVm();
  vm->lock = lock;
  vm->retired = graveyard;
  vm->handles = handles;
  vm->debug = debug;
  vm->steps = steps;
  vm->allow_compiled = allow_compiled;
  vm->initialize = initialize;
  vm->sandboxes = sandboxes;
  vm->writer = writer;
  vm->writer_user = writer_user;
  vm->reporter = reporter;
  vm->reporter_user = reporter_user;
  vm->held = std::move(held);
  vm->answers = std::move(answers);
  wolang::runtime::SpaceCopyBindings(graveyard_space.get(), vm->globals.get());
  if (writer != nullptr || reporter != nullptr) {
    InstallWriter(vm);
  }
  graveyard->push_back(std::move(retired));

  const wolang::runtime::GlobalSpaceScope entered(vm->globals.get());
  int32_t status = 0;
  for (const WolangVm::Loaded& file : again) {
    status |= file.from_source
                  ? wolang_vm_load_source(vm, file.path.c_str(),
                                          file.source.c_str())
                  : wolang_vm_load(vm, file.path.c_str());
  }
  if (status == 0 && vm->ok) {
    Carry(vm, before, old);
    for (auto& [address, entry] : vm->held) {
      (void)address;
      const TypeId now = FindClassNamed(vm->database, entry.Type);
      if (now.is_valid()) {
        entry.Index = now.index;
      }
    }
  }
  return status != 0 ? kWolangError : kWolangOk;
}

// One message from the editor's connection, framed the way DAP frames
// them; |pending| keeps what arrived past the end of it.
static std::optional<std::string> ReceiveFramed(int64_t client, std::string* pending) {
  size_t length = 0;
  for (;;) {
    const size_t header = pending->find("\r\n\r\n");
    if (header != std::string::npos) {
      const size_t at = pending->find("Content-Length:");
      if (at == std::string::npos || at > header) {
        return std::nullopt;
      }
      length = static_cast<size_t>(std::strtoull(pending->c_str() + at + 15, nullptr, 10));
      if (pending->size() >= header + 4 + length) {
        std::string body = pending->substr(header + 4, length);
        pending->erase(0, header + 4 + length);
        return body;
      }
    }
    uint8_t chunk[4096];
    const int64_t got = wolang::runtime::wolang_socket_receive(client, chunk, sizeof(chunk));
    if (got <= 0) {
      return std::nullopt;
    }
    pending->append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(got));
  }
}

static void SendFramed(int64_t client, const std::string& body) {
  const std::string message = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < message.size()) {
    const int64_t wrote = wolang::runtime::wolang_socket_send(client, reinterpret_cast<const uint8_t*>(message.data()) + sent, static_cast<int64_t>(message.size() - sent));
    if (wrote <= 0) {
      return;
    }
    sent += static_cast<size_t>(wrote);
  }
}

// One editor at a time, for as long as the vm lives.
static void Serve(DebugLink* link) {
  while (!link->closing) {
    const int64_t client = wolang::runtime::wolang_socket_accept(link->listener);
    if (client < 0 || link->closing) {
      if (client >= 0) {
        wolang::runtime::wolang_socket_close(client);
      }
      return;
    }
    link->client = client;
    auto session = std::make_shared<wolang::debug::DapSession>([client](const std::string& body) { SendFramed(client, body); }, [](const wolang::lsp::Json&, wolang::debug::DapSession*, std::string* error) {
      *error = "wolang: this program is a host's and already running; attach to it instead\n";
      return -1;
    });
    {
      const std::lock_guard<std::mutex> held(link->lock);
      link->session = session;
      link->sessions.push_back(session);
    }

    std::string pending;
    while (!session->finished()) {
      const std::optional<std::string> body = ReceiveFramed(client, &pending);
      if (!body.has_value()) {
        break;
      }
      session->Handle(*body);
    }
    if (!session->finished() && session->attached()) {
      session->Handle("{\"seq\":0,\"type\":\"request\",\"command\":\"disconnect\"}");
    }
    {
      const std::lock_guard<std::mutex> held(link->lock);
      link->session = nullptr;
    }
    link->client = -1;
    wolang::runtime::wolang_socket_close(client);
  }
}

int32_t wolang_vm_debug_listen(WolangVm* vm, int32_t port) {
  const VmTurn turn(vm);
  if (vm == nullptr || vm->debug != nullptr || port < 0 || port > 65535) {
    return -1;
  }
  const int64_t listener = wolang::runtime::wolang_socket_open();
  if (listener < 0) {
    return -1;
  }
  wolang::runtime::wolang_socket_reuse(listener, 1);
  if (wolang::runtime::wolang_socket_listen(listener, 0x7F000001u, static_cast<uint32_t>(port), 1) != 0) {
    wolang::runtime::wolang_socket_close(listener);
    return -1;
  }

  auto link = std::make_shared<DebugLink>();
  link->listener = listener;
  link->thread = std::thread(&Serve, link.get());
  vm->debug = link;
  return wolang::runtime::wolang_socket_port(listener);
}

const char* wolang_vm_notes(const WolangVm* vm) {
  return vm == nullptr ? "" : vm->notes.c_str();
}

int32_t wolang_vm_file_count(const WolangVm* vm) {
  return vm == nullptr ? 0 : static_cast<int32_t>(vm->files.size());
}

const char* wolang_vm_file_name(const WolangVm* vm, int32_t index) {
  if (vm == nullptr || index < 0 ||
      static_cast<size_t>(index) >= vm->files.size()) {
    return "";
  }
  // The database owns the path, and it outlives the vm's answer.
  return vm->database.sources()
      .Path(vm->files[static_cast<size_t>(index)])
      .data();
}

const char* wolang_version(void) {
  return WOLANG_VERSION_STRING;
}

}  // extern "C"
