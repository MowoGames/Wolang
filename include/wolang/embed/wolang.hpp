// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The embedding API for C++, which is the same API with the ceremony
// removed: a vm that closes itself, calls that take their arguments as
// arguments, and answers that convert to what was asked for.
//
// Header only, and over the C ABI rather than over the compiler's own
// headers -- so a host built with a different standard library, or a
// different compiler, still links (D137).

#ifndef WOLANG_EMBED_WOLANG_HPP_
#define WOLANG_EMBED_WOLANG_HPP_

#include "wolang/embed/wolang.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wolang {

// One value crossing the boundary. Converts from what a host has and to
// what a host wants, and nothing else.
class Value {
 public:
  Value() : held_(wolang_void()) {}
  Value(const WolangValue& value) : held_(value) {}  // NOLINT
  Value(int value) : held_(wolang_int(value)) {}  // NOLINT
  Value(long value) : held_(wolang_int(value)) {}  // NOLINT
  Value(long long value) : held_(wolang_int(value)) {}  // NOLINT
  Value(unsigned value) : held_(wolang_int(value)) {}  // NOLINT
  Value(double value) : held_(wolang_float(value)) {}  // NOLINT
  Value(float value) : held_(wolang_float(value)) {}  // NOLINT
  Value(bool value) : held_(wolang_bool(value ? 1 : 0)) {}  // NOLINT
  Value(std::string_view value)  // NOLINT
      : held_(wolang_text(value.data(), value.size())) {}
  Value(const char* value)  // NOLINT
      : held_(wolang_text(value, value == nullptr ? 0
                                                 : std::string_view(value)
                                                       .size())) {}

  WolangKind kind() const { return static_cast<WolangKind>(held_.kind); }
  const WolangValue& raw() const { return held_; }

  long long integer() const {
    return held_.kind == kWolangFloat
               ? static_cast<long long>(held_.number)
               : held_.integer;
  }
  double number() const {
    return held_.kind == kWolangInt ? static_cast<double>(held_.integer)
                                    : held_.number;
  }
  bool boolean() const {
    return held_.kind == kWolangInt ? held_.integer != 0
                                    : held_.boolean != 0;
  }
  std::string_view text() const {
    if (held_.kind != kWolangText || held_.text == nullptr) {
      return {};
    }
    return std::string_view(held_.text, static_cast<size_t>(held_.length));
  }

 private:
  WolangValue held_;
};

// A function looked up once and called many times, which is what a host
// with a frame loop wants.
class Function {
 public:
  Function() = default;
  explicit Function(WolangFunction* held) : held_(held) {}

  bool is_valid() const { return held_ != nullptr; }
  int arity() const { return wolang_function_arity(held_); }
  WolangKind result() const { return wolang_function_result(held_); }
  std::string_view documentation() const {
    return wolang_function_doc(held_);
  }

  template <typename... Arguments>
  std::optional<Value> operator()(Arguments&&... arguments) const {
    const WolangValue boxed[] = {
        Value(std::forward<Arguments>(arguments)).raw()...};
    WolangValue answer = wolang_void();
    const int32_t count = static_cast<int32_t>(sizeof...(Arguments));
    if (wolang_call(held_, count == 0 ? nullptr : boxed, count, &answer) !=
        0) {
      return std::nullopt;
    }
    return Value(answer);
  }

 private:
  WolangFunction* held_ = nullptr;
};

// A loaded program. Closes itself; cannot be copied, because what it
// holds is one program.
class Vm {
 public:
  Vm() : held_(wolang_vm_create()) {}
  ~Vm() { wolang_vm_destroy(held_); }
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;
  Vm(Vm&& other) noexcept : held_(other.held_) { other.held_ = nullptr; }
  Vm& operator=(Vm&& other) noexcept {
    if (this != &other) {
      wolang_vm_destroy(held_);
      held_ = other.held_;
      other.held_ = nullptr;
    }
    return *this;
  }

  WolangVm* raw() const { return held_; }

  bool Load(const std::string& path) {
    return wolang_vm_load(held_, path.c_str()) == 0;
  }
  bool LoadSource(const std::string& name, const std::string& source) {
    return wolang_vm_load_source(held_, name.c_str(), source.c_str()) == 0;
  }
  bool Reload() { return wolang_vm_reload(held_) == 0; }

  bool ok() const { return wolang_vm_ok(held_) != 0; }
  std::string errors() const { return wolang_vm_errors(held_); }

  bool Has(const std::string& name) const {
    return wolang_vm_has(held_, name.c_str()) != 0;
  }

  // What the program offers, which is what a host lists in a console.
  std::vector<std::string> Functions() const {
    std::vector<std::string> names;
    const int32_t count = wolang_vm_function_count(held_);
    names.reserve(static_cast<size_t>(count < 0 ? 0 : count));
    for (int32_t i = 0; i < count; ++i) {
      names.emplace_back(wolang_vm_function_name(held_, i));
    }
    return names;
  }

  Function Lookup(const std::string& name) const {
    return Function(wolang_vm_function(held_, name.c_str()));
  }

  template <typename... Arguments>
  std::optional<Value> Call(const std::string& name,
                            Arguments&&... arguments) const {
    const WolangValue boxed[] = {
        Value(std::forward<Arguments>(arguments)).raw()...};
    WolangValue answer = wolang_void();
    const int32_t count = static_cast<int32_t>(sizeof...(Arguments));
    if (wolang_vm_call(held_, name.c_str(), count == 0 ? nullptr : boxed,
                       count, &answer) != 0) {
      return std::nullopt;
    }
    return Value(answer);
  }

  // A host function the program may call, by the name it declares with
  // `extern "C" fun`. The signature is the contract; a mismatch is the
  // same mistake as a wrong prototype in C.
  template <typename Function>
  bool Bind(const std::string& symbol, Function* function) {
    return wolang_vm_bind(held_, symbol.c_str(),
                          reinterpret_cast<void*>(function)) == 0;
  }

  std::optional<Value> Global(const std::string& name) const {
    WolangValue held = wolang_void();
    if (wolang_vm_get_global(held_, name.c_str(), &held) != 0) {
      return std::nullopt;
    }
    return Value(held);
  }
  bool SetGlobal(const std::string& name, const Value& value) {
    return wolang_vm_set_global(held_, name.c_str(), value.raw()) == 0;
  }

  void OnOutput(WolangWriter writer, void* user) {
    wolang_vm_set_writer(held_, writer, user);
  }
  void OnDiagnostic(WolangWriter writer, void* user) {
    wolang_vm_set_reporter(held_, writer, user);
  }

  // At most |steps| instructions per call, for a host running code it
  // did not write. Keeps calls on the interpreter, which is the trade.
  void LimitSteps(unsigned long long steps) {
    wolang_vm_set_step_limit(held_, steps);
  }
  void AllowCompiled(bool allowed) {
    wolang_vm_set_compiled(held_, allowed ? 1 : 0);
  }

  // Whether a load starts the module-level `var`s it declares. A session
  // replaying what it has already run turns it off (D140).
  void InitializeGlobals(bool initialize) {
    wolang_vm_set_initialize(held_, initialize ? 1 : 0);
  }

  // The files the program was built from, for a host that watches them.
  std::vector<std::string> Files() const {
    std::vector<std::string> paths;
    const int32_t count = wolang_vm_file_count(held_);
    for (int32_t i = 0; i < count; ++i) {
      paths.emplace_back(wolang_vm_file_name(held_, i));
    }
    return paths;
  }

 private:
  WolangVm* held_ = nullptr;
};

inline std::string Version() {
  return wolang_version();
}

}  // namespace wolang

#endif  // WOLANG_EMBED_WOLANG_HPP_
