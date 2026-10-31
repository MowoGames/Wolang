// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/fuzz/generator.h"

#include <cstdio>
#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/abi/layout.h"
#include "wolang/codegen/compiler.h"
#include "wolang/codegen/foreign.h"
#include "wolang/compiler/database.h"
#include "wolang/interp/interpreter.h"

namespace wolang {
namespace {

// What one program did, as a string: a value, or the trap that stopped it.
std::string Describe(const interp::Outcome& outcome) {
  if (outcome.ok()) {
    return outcome.value.Describe();
  }
  // The kind of trap and its words, without where: a pass that turns one
  // instruction into several moves the trap onto a different one without
  // changing which operation failed (D231).
  return "trap: " + std::string(ir::TrapKindName(outcome.trap)) + " (" +
         outcome.message + ")";
}

// Runs one generated program through every tier and reports the first
// disagreement, or an empty string when they all agree.
std::string CompareTiers(uint64_t seed, bool traps) {
  fuzz::Options options;
  options.seed = seed;
  options.traps = traps;
  const std::string source = fuzz::Generate(options);

  Database db;
  const FileId file = db.AddFile("<fuzz>", source);
  const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file);
  if (diagnostics != nullptr) {
    for (const Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity == Severity::kError) {
        return "seed " + std::to_string(seed) + ": the generator wrote a " +
               "program the checker rejected: " + diagnostic.code + " " +
               diagnostic.message + "\n" + source;
      }
    }
  }

  const LoweredFile* lowered = db.Lowered(file);
  const CheckedFile* checked = db.Checked(file);
  if (lowered == nullptr || checked == nullptr) {
    return "seed " + std::to_string(seed) + ": not lowered\n" + source;
  }
  if (!lowered->problems.empty()) {
    return "seed " + std::to_string(seed) + ": " + lowered->problems.front() +
           "\n" + source;
  }

  const Symbol probe = db.interner().Intern("Probe");
  abi::LayoutStore layouts(db.types(), checked->resolution);
  const std::unique_ptr<interp::ForeignCaller> foreign =
      codegen::MakeForeignCaller();

  const interp::Outcome reference =
      interp::RunNamed(db.interner(), db.types(), lowered->unoptimized, probe,
                       {}, {}, nullptr, foreign.get(), &layouts);
  // A program the reference tier could not finish says nothing about the
  // other two, and the compiled tier has no step limit to stop it with.
  if (!reference.ok() && reference.trap == ir::TrapKind::kStepLimit) {
    return {};
  }
  const interp::Outcome optimized =
      interp::RunNamed(db.interner(), db.types(), lowered->module, probe, {},
                       {}, nullptr, foreign.get(), &layouts);
  if (Describe(reference) != Describe(optimized)) {
    return "seed " + std::to_string(seed) + ": the passes changed the " +
           "meaning: " + Describe(reference) + " then " +
           Describe(optimized) + "\n" + source;
  }

  if (!codegen::IsSupportedTarget()) {
    return {};
  }
  std::unique_ptr<codegen::CompiledModule> compiled = codegen::Compile(
      db.interner(), db.types(), checked->resolution, lowered->module);
  if (compiled == nullptr || !compiled->Contains(probe)) {
    return {};
  }
  const interp::Outcome machine = compiled->Run(probe, {});
  if (Describe(machine) != Describe(reference)) {
    return "seed " + std::to_string(seed) + ": the code generator " +
           "disagrees: interpreter said " + Describe(reference) +
           ", machine code said " + Describe(machine) + "\n" + source;
  }
  return {};
}

WOLANG_TEST(FuzzTest, TheTiersAgreeOnGeneratedPrograms) {
  // A fixed range of seeds rather than a random one: a test that fails
  // only sometimes is a test nobody trusts, and the seeds are cheap
  // enough to run every build. `wolang fuzz` is the one that goes wide.
  // Eighty seeds rather than eight hundred: this runs on every build, and
  // `wolang fuzz --count 10000` is what goes wide when something is
  // suspected. The seeds here are the regression net.
  for (uint64_t seed = 1; seed <= 80; ++seed) {
    // Named before it runs, so one that never finishes says which it was.
    if (test::Verbose()) {
      std::fprintf(stdout, "  seed %llu\n",
                   static_cast<unsigned long long>(seed));
      std::fflush(stdout);
    }
    const std::string disagreement = CompareTiers(seed, /*traps=*/true);
    WOLANG_EXPECT_EQ(disagreement, "");
  }
}

WOLANG_TEST(FuzzTest, TheTiersAgreeWithoutTraps) {
  // The same again with nothing that can trap, so that a disagreement
  // about a *value* is not hidden behind one about a trap.
  for (uint64_t seed = 1; seed <= 80; ++seed) {
    if (test::Verbose()) {
      std::fprintf(stdout, "  seed %llu\n",
                   static_cast<unsigned long long>(seed + 1000));
      std::fflush(stdout);
    }
    const std::string disagreement = CompareTiers(seed + 1000, false);
    WOLANG_EXPECT_EQ(disagreement, "");
  }
}

// Whether the checker and lowering accept |source|; the empty string when
// they do. A generator that writes a program the compiler rejects compares
// nothing, so this is the self-check that every program is valid (D231).
std::string Rejection(uint64_t seed, const fuzz::Options& options) {
  const std::string source = fuzz::Generate(options);
  Database db;
  const FileId file = db.AddFile("<fuzz>", source);
  if (const std::vector<Diagnostic>* diagnostics = db.Diagnostics(file)) {
    for (const Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity == Severity::kError) {
        return "seed " + std::to_string(seed) + ": " + diagnostic.code +
               " " + diagnostic.message + "\n" + source;
      }
    }
  }
  const LoweredFile* lowered = db.Lowered(file);
  if (lowered == nullptr || !lowered->problems.empty()) {
    return "seed " + std::to_string(seed) + ": did not lower\n" + source;
  }
  return {};
}

WOLANG_TEST(FuzzTest, EveryGeneratedProgramChecksClean) {
  for (uint64_t seed = 5000; seed < 5300; ++seed) {
    fuzz::Options options;
    options.seed = seed;
    WOLANG_EXPECT_EQ(Rejection(seed, options), "");
  }
  // And with the optional features off, which write different programs.
  for (uint64_t seed = 6000; seed < 6060; ++seed) {
    fuzz::Options options;
    options.seed = seed;
    options.floats = false;
    options.traps = false;
    WOLANG_EXPECT_EQ(Rejection(seed, options), "");
  }
}

WOLANG_TEST(FuzzTest, GeneratedProgramsUseTheWholeLanguageSurface) {
  // The point of the extension (D231): over a range of seeds, each
  // construct the generator is meant to reach is actually written.
  std::string all;
  for (uint64_t seed = 1; seed <= 60; ++seed) {
    fuzz::Options options;
    options.seed = seed;
    all += fuzz::Generate(options);
  }
  for (const char* needle :
       {"as u8", "as s64", " << ", " >> ", "~", "break L", "continue L",
        "loop {", "match ", "Shape.Box", "?? ", "Check_", " % ", "u64",
        "Cell(", ".len", "wolang_objects_live", "..="}) {
    WOLANG_EXPECT(all.find(needle) != std::string::npos);
  }
}

WOLANG_TEST(FuzzTest, ASeedNamesOneProgram) {
  // The whole point of the seed: a failure can be looked at again.
  fuzz::Options options;
  options.seed = 42;
  const std::string first = fuzz::Generate(options);
  WOLANG_EXPECT_EQ(fuzz::Generate(options), first);
  options.seed = 43;
  WOLANG_EXPECT(fuzz::Generate(options) != first);
}

}  // namespace
}  // namespace wolang
