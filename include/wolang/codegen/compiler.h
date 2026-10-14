// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_COMPILER_H_
#define WOLANG_CODEGEN_COMPILER_H_

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/codegen/emitter.h"
#include "wolang/interp/interpreter.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/resolution.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace codegen {

// One file's IR, with the resolution its types need.
struct FileCode {
  const FileResolution* resolution = nullptr;
  const ir::Module* module = nullptr;
};

// A module's functions as machine code.
//
// The dev tier compiles what it can and says what it could not: the
// interpreter is the reference semantics and is always there, so a function
// holding a string or calling something unsupported is left to it rather
// than guessed at.
class CompiledModule {
 public:
  CompiledModule();
  ~CompiledModule();

  CompiledModule(const CompiledModule&) = delete;
  CompiledModule& operator=(const CompiledModule&) = delete;

  bool Contains(Symbol name) const;
  // Why |name| was left to the interpreter, or empty when it was compiled.
  std::string_view SkipReason(Symbol name) const;

  // Runs the compiled |name|.
  //
  // Arguments and the result are boxed the way the interpreter holds them,
  // so that the two tiers' outcomes compare directly -- which is the whole
  // reason this returns an interpreter outcome rather than a type of its
  // own.
  //
  // |backtrace| receives the compiled callers at the moment of a trap,
  // innermost first, when one is asked for.
  interp::Outcome Run(Symbol name,
                      const std::vector<interp::Value>& arguments,
                      std::vector<std::string>* backtrace = nullptr) const;

  // Which function an offset into the code belongs to, as `@Name+0x1c`.
  std::string Symbolize(uint32_t offset) const;

  // Every compiled function and how many bytes it took, in the order they
  // were emitted.
  std::vector<std::pair<std::string, uint32_t>> Listing() const;

  // The machine words of one function, for a dump or a disassembly test.
  std::span<const uint32_t> WordsOf(Symbol name) const;

  // Bytes of code installed.
  size_t size() const;
  // How many functions were compiled, and how many were left behind.
  uint32_t compiled_count() const;
  uint32_t skipped_count() const;

  class Impl;

 private:
  friend std::unique_ptr<CompiledModule> Compile(
      const StringInterner&,
      const TypeStore&,
      std::span<const FileCode>);

  std::unique_ptr<Impl> impl_;
};

// Compiles every function the dev tier can take, across all the files given
// at once.
//
// All of them together, because lowering is per file and a call into
// another file is a declaration with no body: compiling one file at a time
// would leave every cross-module call to the interpreter, which in a real
// package is most of them. A declaration is linked to its definition by the
// declaration it came from -- a file and an item index -- rather than by
// name, because two files may each define a private `Helper`.
//
// Never fails: a module where nothing could be compiled is an empty one,
// and the interpreter runs it all.
std::unique_ptr<CompiledModule> Compile(const StringInterner& interner,
                                        const TypeStore& types,
                                        std::span<const FileCode> files);

// One file on its own, which is what a test and a single-file program want.
std::unique_ptr<CompiledModule> Compile(const StringInterner& interner,
                                        const TypeStore& types,
                                        const FileResolution& resolution,
                                        const ir::Module& module);

// True on a target this tier knows how to emit for.
bool IsSupportedTarget();

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_COMPILER_H_
