// Copyright 2026 The Wolang Authors. All rights reserved.
//
// Wolang as a library, behind a C ABI.
//
// Everything here is the same machinery `wolang run` uses -- the query
// database, the lowering, the dev tier, the interpreter -- with the
// pieces a host cares about brought out through a handful of functions
// and no C++ in the header (D128).

#include "wolang/embed/wolang.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "wolang/abi/layout.h"
#include "wolang/base/diagnostic.h"
#include "wolang/base/diagnostic_renderer.h"
#include "wolang/codegen/compiler.h"
#include "wolang/codegen/foreign.h"
#include "wolang/compiler/database.h"
#include "wolang/compiler/workspace.h"
#include "wolang/interp/interpreter.h"
#include "wolang/ir/ir.h"
#include "wolang/syntax/ast.h"
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/env.h"
#include "wolang/runtime/globals.h"
#include "wolang/runtime/scheduler.h"

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
  std::string doc;
  Symbol symbol;
  const wolang::ir::Module* module = nullptr;
  const wolang::ir::Function* function = nullptr;
  WolangVm* vm = nullptr;
};

}  // namespace

// The vm is a C type in the header and this in reality.
struct WolangVm {
  Database database;
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
  std::vector<std::string> answers;
};

namespace {

// The `///` lines in front of a declaration, with the slashes taken
// off: what a host shows beside a script's function in a console, and
// the same text `wolang doc` writes (D137).
std::string DocumentationFor(WolangVm* vm, FileId file, Symbol name);

// Hands |text| to the host's writer, or to standard output when there is
// none (D140).
void Written(const std::string& text);

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
const Callable* Find(const WolangVm* vm, const char* name,
                     int32_t arity = -1) {
  if (vm == nullptr || name == nullptr) {
    return nullptr;
  }
  const Callable* fallback = nullptr;
  for (auto it = vm->callable.rbegin(); it != vm->callable.rend(); ++it) {
    if (it->name != name) {
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

WolangVm* wolang_vm_create(void) {
  return new WolangVm();
}

void wolang_vm_destroy(WolangVm* vm) {
  delete vm;
}

int32_t wolang_vm_load(WolangVm* vm, const char* path) {
  if (vm == nullptr || path == nullptr) {
    return 1;
  }
  DiagnosticEngine loading;
  if (wolang::IsPackageDirectory(path)) {
    if (!wolang::LoadPackage(&vm->database, path, &loading)) {
      vm->errors = Render(&vm->database, loading.diagnostics());
      if (vm->errors.empty()) {
        vm->errors = "wolang: cannot load the package\n";
      }
      vm->ok = false;
      return 1;
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
      return 1;
    }
    vm->files.push_back(file);
    vm->own.push_back(file);
  }
  vm->loaded.push_back(WolangVm::Loaded{path, std::string(), false});
  Prepare(vm);
  return vm->ok ? 0 : 1;
}

int32_t wolang_vm_load_source(WolangVm* vm,
                              const char* name,
                              const char* source) {
  if (vm == nullptr || source == nullptr) {
    return 1;
  }
  const FileId file = vm->database.AddFile(
      name == nullptr ? "<host>" : name, source);
  // Source a host handed over belongs to a package of its own, which
  // depends on every package already loaded: a script that cannot say
  // `need std.io` is not a script anyone wants to write (D140).
  if (file.is_valid()) {
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
        manifest.dependencies.push_back(
            wolang::ManifestDependency{held->name, held->directory});
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
  if (!file.is_valid()) {
    vm->errors = "wolang: cannot add the source\n";
    vm->ok = false;
    return 1;
  }
  vm->files.push_back(file);
  vm->own.push_back(file);
  vm->loaded.push_back(WolangVm::Loaded{
      name == nullptr ? "<host>" : name, source, true});
  Prepare(vm);
  return vm->ok ? 0 : 1;
}

const char* wolang_vm_errors(const WolangVm* vm) {
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

int32_t wolang_vm_bind(WolangVm* vm, const char* symbol, void* function) {
  (void)vm;
  if (symbol == nullptr || function == nullptr) {
    return 1;
  }
  wolang::runtime::RegisterSymbol(symbol, function);
  return 0;
}

void wolang_vm_set_args(WolangVm* vm,
                        int32_t count,
                        const char* const* args) {
  (void)vm;
  wolang::runtime::wolang_args_set(count < 0 ? 0 : count, args);
}

int32_t wolang_vm_call(WolangVm* vm,
                       const char* name,
                       const WolangValue* arguments,
                       int32_t count,
                       WolangValue* out) {
  if (vm == nullptr) {
    return 1;
  }
  if (!vm->ok) {
    return 1;
  }
  const Callable* entry = Find(vm, name, count < 0 ? 0 : count);
  if (entry == nullptr) {
    vm->errors = std::string("wolang: no function called `") +
                 (name == nullptr ? "" : name) + "`\n";
    return 1;
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
    return 1;
  }

  // The compiled form where there is one, the interpreter otherwise:
  // the same choice `wolang run` makes, and the reason a host gets
  // machine-code speed without asking. A step limit keeps the call off
  // the compiled tier, because counting steps is something only the
  // interpreter does (D137).
  wolang::interp::Outcome outcome;
  // By the declaration rather than by the name: a program may have two
  // functions called `Peek`, and a host calling one of them has to reach
  // the one it looked up (D140).
  if (vm->allow_compiled && vm->steps == 0 && vm->compiled != nullptr &&
      vm->compiled->ContainsSource(entry->function->source_file,
                                   entry->function->source_item)) {
    outcome = vm->compiled->RunSource(entry->function->source_file,
                                      entry->function->source_item, boxed,
                                      nullptr, /*keep_globals=*/true);
  } else {
    // The resolution of the file the function came from, with every
    // other file's member tables shared in: a type crosses files, and a
    // layout is a question about the type (D125).
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
    // The interpreter collects what a program printed rather than
    // writing it: a single file that has not linked `std.io` reaches
    // `Print` as an intrinsic, and a host should see that output like
    // any other (D140).
    wolang::interp::Output printed;
    outcome = wolang::interp::RunNamed(
        vm->database.interner(), vm->database.types(), *entry->module,
        entry->symbol, boxed, limits, &printed, vm->foreign.get(),
        &layouts, vm->linked);
    if (!printed.text.empty()) {
      Written(printed.text);
    }
  }

  if (!outcome.ok()) {
    vm->errors = std::string("wolang: ") +
                 std::string(wolang::interp::TrapKindName(outcome.trap)) +
                 " at " + outcome.where + "\n";
    return 1;
  }
  if (out != nullptr) {
    *out = Unboxed(vm, outcome.value);
  }
  return 0;
}

// ------------------------------------------------------------- handles ---

WolangFunction* wolang_vm_function(WolangVm* vm, const char* name) {
  // A `Callable` *is* the handle: the list does not move while a program
  // is loaded, and a reload clears it -- which is why the header says a
  // handle does not survive one.
  return reinterpret_cast<WolangFunction*>(
      const_cast<Callable*>(Find(vm, name)));
}

int32_t wolang_call(WolangFunction* function,
                    const WolangValue* arguments,
                    int32_t count,
                    WolangValue* out) {
  if (function == nullptr) {
    return 1;
  }
  const auto* entry = reinterpret_cast<const Callable*>(function);
  if (entry->vm == nullptr) {
    return 1;
  }
  return wolang_vm_call(entry->vm, entry->name.c_str(), arguments, count,
                        out);
}

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
      return kWolangVoid;
  }
}

}  // namespace

int32_t wolang_function_arity(const WolangFunction* function) {
  if (function == nullptr) {
    return -1;
  }
  const auto* entry = reinterpret_cast<const Callable*>(function);
  if (entry->vm == nullptr || entry->function == nullptr) {
    return -1;
  }
  return static_cast<int32_t>(
      entry->vm->database.types().ArgumentsOf(entry->function->type).size());
}

WolangKind wolang_function_result(const WolangFunction* function) {
  if (function == nullptr) {
    return kWolangVoid;
  }
  const auto* entry = reinterpret_cast<const Callable*>(function);
  if (entry->vm == nullptr || entry->function == nullptr) {
    return kWolangVoid;
  }
  const wolang::TypeStore& types = entry->vm->database.types();
  return KindOf(types, types.ElementOf(entry->function->type));
}

WolangKind wolang_function_parameter(const WolangFunction* function,
                                     int32_t index) {
  if (function == nullptr || index < 0) {
    return kWolangVoid;
  }
  const auto* entry = reinterpret_cast<const Callable*>(function);
  if (entry->vm == nullptr || entry->function == nullptr) {
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
  if (function == nullptr) {
    return "";
  }
  return reinterpret_cast<const Callable*>(function)->doc.c_str();
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

int32_t wolang_vm_get_global(WolangVm* vm,
                             const char* name,
                             WolangValue* out) {
  TypeId type;
  void* storage = GlobalStorage(vm, name, &type);
  if (storage == nullptr || out == nullptr) {
    return 1;
  }
  const wolang::TypeStore& types = vm->database.types();
  switch (KindOf(types, type)) {
    case kWolangInt: {
      int64_t held = 0;
      std::memcpy(&held, storage,
                  std::min<size_t>(sizeof(held),
                                   BytesOfWidth(types.Get(type).int_width)));
      *out = wolang_int(held);
      return 0;
    }
    case kWolangFloat: {
      if (types.Get(type).float_width == wolang::FloatWidth::k32) {
        float held = 0;
        std::memcpy(&held, storage, sizeof(held));
        *out = wolang_float(held);
        return 0;
      }
      double held = 0;
      std::memcpy(&held, storage, sizeof(held));
      *out = wolang_float(held);
      return 0;
    }
    case kWolangBool: {
      uint8_t held = 0;
      std::memcpy(&held, storage, sizeof(held));
      *out = wolang_bool(held != 0);
      return 0;
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
      return 0;
    }
    default:
      return 1;
  }
}

int32_t wolang_vm_set_global(WolangVm* vm,
                             const char* name,
                             WolangValue value) {
  TypeId type;
  void* storage = GlobalStorage(vm, name, &type);
  if (storage == nullptr) {
    return 1;
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
      return 0;
    }
    case kWolangFloat: {
      const double held = value.kind == kWolangInt
                              ? static_cast<double>(value.integer)
                              : value.number;
      if (types.Get(type).float_width == wolang::FloatWidth::k32) {
        const auto narrow = static_cast<float>(held);
        std::memcpy(storage, &narrow, sizeof(narrow));
        return 0;
      }
      std::memcpy(storage, &held, sizeof(held));
      return 0;
    }
    case kWolangBool: {
      const uint8_t held = value.kind == kWolangInt
                               ? (value.integer != 0 ? 1 : 0)
                               : (value.boolean != 0 ? 1 : 0);
      std::memcpy(storage, &held, sizeof(held));
      return 0;
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
      return 0;
    }
    default:
      return 1;
  }
}

// -------------------------------------------------------------- output ---

namespace {

// Where a program's output goes when a host asked for it.
WolangWriter host_writer = nullptr;
void* host_writer_user = nullptr;
WolangWriter host_reporter = nullptr;
void* host_reporter_user = nullptr;

// What `std.io` calls. Registered under `write`, which the runtime's
// symbol table answers before the loader is asked -- so a program that
// prints reaches the host without knowing it (D137).
extern "C" int64_t WriteThroughHost(int32_t fd,
                                    const char* bytes,
                                    uint64_t count) {
  if (fd == 1 && host_writer != nullptr) {
    host_writer(host_writer_user, bytes, count);
    return static_cast<int64_t>(count);
  }
  if (fd == 2 && host_reporter != nullptr) {
    host_reporter(host_reporter_user, bytes, count);
    return static_cast<int64_t>(count);
  }
  return static_cast<int64_t>(
      ::fwrite(bytes, 1, static_cast<size_t>(count),
               fd == 2 ? stderr : stdout));
}

}  // namespace

namespace {

void Written(const std::string& text) {
  if (host_writer != nullptr) {
    host_writer(host_writer_user, text.data(), text.size());
    return;
  }
  std::fwrite(text.data(), 1, text.size(), stdout);
}

}  // namespace

void wolang_vm_set_writer(WolangVm* vm, WolangWriter writer, void* user) {
  (void)vm;
  host_writer = writer;
  host_writer_user = user;
  wolang::runtime::RegisterSymbol(
      "write", reinterpret_cast<void*>(&WriteThroughHost));
}

void wolang_vm_set_reporter(WolangVm* vm, WolangWriter reporter,
                            void* user) {
  (void)vm;
  host_reporter = reporter;
  host_reporter_user = user;
  wolang::runtime::RegisterSymbol(
      "write", reinterpret_cast<void*>(&WriteThroughHost));
}

// -------------------------------------------------------------- limits ---

void wolang_vm_set_step_limit(WolangVm* vm, uint64_t steps) {
  if (vm != nullptr) {
    vm->steps = steps;
  }
}

void wolang_vm_set_compiled(WolangVm* vm, int32_t allowed) {
  if (vm != nullptr) {
    vm->allow_compiled = allowed != 0;
  }
}

void wolang_vm_set_initialize(WolangVm* vm, int32_t initialize) {
  if (vm != nullptr) {
    vm->initialize = initialize != 0;
  }
}

// ------------------------------------------------------------- reloads ---

int32_t wolang_vm_reload(WolangVm* vm) {
  if (vm == nullptr) {
    return 1;
  }
  const std::vector<WolangVm::Loaded> again = vm->loaded;
  // A fresh database, because a query cache remembers what the files
  // said: reloading is loading again, not re-asking.
  vm->~WolangVm();
  new (vm) WolangVm();
  int32_t status = 0;
  for (const WolangVm::Loaded& held : again) {
    status |= held.from_source
                  ? wolang_vm_load_source(vm, held.path.c_str(),
                                          held.source.c_str())
                  : wolang_vm_load(vm, held.path.c_str());
  }
  return status;
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
  return "0.1.0";
}

}  // extern "C"
