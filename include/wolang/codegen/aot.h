// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_CODEGEN_AOT_H_
#define WOLANG_CODEGEN_AOT_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/abi/layout.h"
#include "wolang/base/source_manager.h"
#include "wolang/base/string_interner.h"
#include "wolang/codegen/compiler.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace codegen {

// What `<library>_abi_version()` answers: the embedding ABI the library
// was built to match, kept equal to WOLANG_ABI_VERSION in wolang.h by a
// test (D241).
inline constexpr uint32_t kLibraryAbiVersion = (1u << 16) | 3u;

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
  // Not empty for a library a host links rather than a program: no
  // `main`, a `<library>_start` the host calls once before anything else,
  // and every function the |exported| files offer under the C name
  // `<library>_<Name>` (D219), and a `<library>_abi_version` (D241).
  std::string library;
  std::vector<uint32_t> exported;
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

// The command that links |object| and the runtime into |binary| with the
// system's own linker: `c++` where there is one, `clang-cl` on Windows.
// What a package adds goes after it, in that linker's own spelling (D208).
std::string LinkCommand(std::string_view object, std::string_view binary);

// What the linker on this system calls a library search path and a
// library, for the `[link]` table of a manifest.
std::string LinkPathFlag(std::string_view path);
std::string LinkLibraryFlag(std::string_view library);

// Where a binary named |stem| ends up: `.exe` on Windows.
std::string BinaryName(std::string_view stem);

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
