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
#include "wolang/runtime/builtins.h"
#include "wolang/runtime/env.h"
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
  Symbol symbol;
  const wolang::ir::Module* module = nullptr;
  const wolang::ir::Function* function = nullptr;
};

}  // namespace

// The vm is a C type in the header and this in reality.
struct WolangVm {
  Database database;
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
  // Kept alive for as long as the vm is: a text the program answered
  // points into one of these, so the host can read it after the call.
  std::vector<std::string> answers;
};

namespace {

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
      entry.symbol = function.name;
      entry.module = &lowered->module;
      entry.function = &function;
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
}

const Callable* Find(const WolangVm* vm, const char* name) {
  if (vm == nullptr || name == nullptr) {
    return nullptr;
  }
  for (const Callable& entry : vm->callable) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
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
  if (!file.is_valid()) {
    vm->errors = "wolang: cannot add the source\n";
    vm->ok = false;
    return 1;
  }
  vm->files.push_back(file);
  vm->own.push_back(file);
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
  const Callable* entry = Find(vm, name);
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
  // machine-code speed without asking.
  wolang::interp::Outcome outcome;
  if (vm->compiled != nullptr && vm->compiled->Contains(entry->symbol)) {
    outcome = vm->compiled->Run(entry->symbol, boxed);
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
    outcome = wolang::interp::RunNamed(
        vm->database.interner(), vm->database.types(), *entry->module,
        entry->symbol, boxed, {}, nullptr, vm->foreign.get(), &layouts,
        vm->linked);
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

const char* wolang_version(void) {
  return "0.1.0";
}

}  // extern "C"
