// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_CODEGEN_AOT_H_
#define WOLANG_CODEGEN_AOT_H_

#include <string>
#include <vector>

#include "wolang/abi/layout.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/codegen/compiler.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace codegen {

// Ahead-of-time compilation: the same IR, through LLVM, into an object
// file.
//
// Why a third tier at all. The interpreter is the reference and the dev
// tier is what a developer waits for -- neither is what a program ships
// as. A shipping build should need no JIT, should be optimized by
// something that has had twenty years of attention, and should run where
// writing machine code by hand is not an option (D98).
//
// The translation is deliberately literal: one LLVM instruction per IR
// instruction wherever that is possible, aggregates in `alloca`s at the
// offsets the layout gives, and block parameters as phi nodes -- which is
// what the IR's block-parameter form was chosen to make easy. What LLVM
// is for here is the back end, not the middle: the passes in `ir/passes.h`
// have already run.
struct AotOptions {
  // Empty for this machine.
  std::string triple;
  // Where the object file goes.
  std::string object_path;
  // The optimization level LLVM runs at, 0 to 3.
  int optimization = 2;
  // Where the source is, for the line table a debugger reads. Without it
  // the binary has no debug information, which is a fine thing for a
  // release build to ask for (D118).
  const SourceManager* sources = nullptr;
};

struct AotResult {
  // Empty when the object was written.
  std::string error;
  // Functions this backend could not translate, with the reason. A
  // shipping build with any of these is incomplete, so the driver says
  // so rather than quietly producing a binary that traps.
  std::vector<std::string> skipped;
};

// True when this build has the backend at all.
bool HasAotBackend();

// What this machine is, as an LLVM triple. Empty when there is no
// backend to ask. A `--target` that matches it is not a cross build, so
// the driver links it the usual way (D160).
std::string HostTriple();

// Compiles every file into one object file, with a C `main` that starts
// the program: the class tables registered, then the module initializers,
// then Wolang's own `main`.
//
// All the files at once for the same reason the dev tier takes them that
// way: a call that crosses a file is a declaration here and a definition
// there, and linking the two is what makes this compile a program rather
// than a file.
AotResult EmitObject(const StringInterner& interner,
                     const TypeStore& types,
                     std::span<const FileCode> files,
                     const AotOptions& options);

// The runtime archive this build links an ahead-of-time binary against,
// or empty when the build did not record one.
std::string_view RuntimeArchive();

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_AOT_H_
