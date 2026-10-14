// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/fuzz/generator.h"

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
  // The kind of trap and where, without the message: a pass that turns one
  // instruction into several moves the trap onto a different one without
  // changing which operation failed.
  return "trap: " + std::string(ir::TrapKindName(outcome.trap));
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
    const std::string disagreement = CompareTiers(seed, /*traps=*/true);
    WOLANG_EXPECT_EQ(disagreement, "");
  }
}

WOLANG_TEST(FuzzTest, TheTiersAgreeWithoutTraps) {
  // The same again with nothing that can trap, so that a disagreement
  // about a *value* is not hidden behind one about a trap.
  for (uint64_t seed = 1; seed <= 80; ++seed) {
    const std::string disagreement = CompareTiers(seed + 1000, false);
    WOLANG_EXPECT_EQ(disagreement, "");
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
