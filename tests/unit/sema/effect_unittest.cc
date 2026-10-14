// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/effect.h"

#include "harness/temp_tree.h"
#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/compiler/workspace.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace {

WOLANG_TEST(EffectRowTest, BuiltinsAreInternedFirst) {
  // Their ids are the enumerator values, so testing for one is an index
  // comparison rather than a lookup.
  TypeStore store;
  WOLANG_EXPECT_EQ(store.BuiltinEffectId(BuiltinEffect::kAlloc).index,
                   uint32_t{0});
  WOLANG_EXPECT(store.IsBuiltinEffect(
      store.BuiltinEffectId(BuiltinEffect::kPanic)));

  StringInterner interner;
  const EffectId declared = store.Effect(interner.Intern("Telemetry"), 0, 3);
  WOLANG_EXPECT(!store.IsBuiltinEffect(declared));
  WOLANG_EXPECT_EQ(store.Effect(interner.Intern("Telemetry"), 0, 3), declared);
  // Interned by declaration, so the same spelling in another file is another
  // effect.
  WOLANG_EXPECT_NE(store.Effect(interner.Intern("Telemetry"), 1, 3), declared);
}

WOLANG_TEST(EffectRowTest, LooksUpBuiltinsByName) {
  WOLANG_EXPECT_EQ(LookupBuiltinEffect("io"), BuiltinEffect::kIo);
  WOLANG_EXPECT_EQ(LookupBuiltinEffect("alloc"), BuiltinEffect::kAlloc);
  WOLANG_EXPECT_EQ(LookupBuiltinEffect("Telemetry"), BuiltinEffect::kCount);
  WOLANG_EXPECT_EQ(BuiltinEffectName(BuiltinEffect::kRand), "rand");
}

WOLANG_TEST(EffectRowTest, StaysSortedAndDeduplicated) {
  const EffectId io{static_cast<uint32_t>(BuiltinEffect::kIo)};
  const EffectId alloc{static_cast<uint32_t>(BuiltinEffect::kAlloc)};

  EffectRow row;
  WOLANG_EXPECT(row.empty());
  WOLANG_EXPECT(row.Add(io));
  // Adding the same effect again does not grow the row, which is what a
  // fixpoint iteration watches.
  WOLANG_EXPECT(!row.Add(io));
  WOLANG_EXPECT(row.Add(alloc));
  WOLANG_EXPECT(row.Contains(BuiltinEffect::kIo));
  WOLANG_EXPECT(!row.Contains(BuiltinEffect::kRand));

  // Sorted, so two rows built in different orders are equal.
  EffectRow other;
  other.Add(alloc);
  other.Add(io);
  WOLANG_EXPECT_EQ(row, other);
}

WOLANG_TEST(EffectRowTest, MergeReportsWhetherItGrew) {
  EffectRow row = EffectRow::Single(
      EffectId{static_cast<uint32_t>(BuiltinEffect::kIo)});
  EffectRow addition = EffectRow::Single(
      EffectId{static_cast<uint32_t>(BuiltinEffect::kRand)});

  WOLANG_EXPECT(row.Merge(addition));
  WOLANG_EXPECT(!row.Merge(addition));
}

WOLANG_TEST(EffectRowTest, MissingNamesWhatIsNotCovered) {
  const EffectId io{static_cast<uint32_t>(BuiltinEffect::kIo)};
  const EffectId rand{static_cast<uint32_t>(BuiltinEffect::kRand)};

  EffectRow declared = EffectRow::Single(io);
  EffectRow actual;
  actual.Add(io);
  actual.Add(rand);

  const EffectRow missing = declared.Missing(actual);
  WOLANG_EXPECT_EQ(missing.effects().size(), size_t{1});
  WOLANG_EXPECT(missing.Contains(BuiltinEffect::kRand));
  // An empty row is a positive claim, not "unknown": everything is missing
  // from it.
  WOLANG_EXPECT_EQ(EffectRow().Missing(actual).effects().size(), size_t{2});
}

WOLANG_TEST(EffectRowTest, DescribesARowAsItIsWritten) {
  TypeStore store;
  StringInterner interner;
  EffectRow row;
  row.Add(store.BuiltinEffectId(BuiltinEffect::kIo));
  row.Add(store.Effect(interner.Intern("Telemetry"), 0, 1));

  WOLANG_EXPECT_EQ(store.Describe(interner, row), "io, Telemetry");
  WOLANG_EXPECT_EQ(store.Describe(interner, EffectRow()), "no effects");
}

// The codes a package reports, in order.
std::string PackageCodes(Database* db) {
  std::string result;
  for (const Diagnostic& diagnostic : db->AllDiagnostics()) {
    if (!result.empty()) {
      result.push_back(',');
    }
    result.append(diagnostic.code);
  }
  return result;
}

WOLANG_TEST(EffectRowTest, CallingOutOfTheLanguageIsUnsafe) {
  // The whole FFI rule, and there is no second rule to keep in step with
  // it: a foreign call carries `unsafe`, so everything already decided
  // about `unsafe` applies -- an offered function has to say so, and a
  // package that forbids it cannot reach C at all.
  testing::TempTree tree("ffi-unsafe");
  tree.Write("wolang.toml", "[package]\nname = \"ffi\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", R"(
extern "C" fun abs(n: s32) -> s32

offer fun Quiet(n: s32) -> s32 {
  return abs(n)
}
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT_EQ(PackageCodes(&db), "E0704");
}

WOLANG_TEST(EffectRowTest, APackageThatForbidsUnsafeCannotReachC) {
  testing::TempTree tree("ffi-denied");
  tree.Write("wolang.toml",
             "[package]\nname = \"ffi\"\nversion = \"1\"\n\n"
             "[build]\nunsafe = \"deny\"\n");
  tree.Write("src/main.wo", R"(
extern "C" fun abs(n: s32) -> s32

fun Magnitude(n: s32) -> s32 uses unsafe {
  return abs(n)
}
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT_EQ(PackageCodes(&db), "E0801");
}

WOLANG_TEST(EffectRowTest, SuspensionIsAnEffectAHotPathForbids) {
  // The payoff of putting `async` in the row: a hot path that suspends is
  // caught by the same check that catches an allocation, and says so in the
  // same words.
  testing::TempTree tree("async-hot");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", R"(
offer task fun Sleep(ms: int) uses io

@Hot
task fun Frame(n: int) -> int uses io {
  wait Sleep(1)
  return n
}
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT_EQ(PackageCodes(&db), "E0702");
}

WOLANG_TEST(EffectRowTest, SpawningIsNotSuspending) {
  // Starting work is being responsible for what it does -- so `io` carries
  // over -- but the spawner does not wait, so `async` does not.
  testing::TempTree tree("async-spawn");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", R"(
offer task fun Sleep(ms: int) uses io

offer fun Starter() uses io {
  const _handle = spawn Sleep(1)
}
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  // `uses io` is enough: no `async` is missing from the row.
  WOLANG_EXPECT_EQ(PackageCodes(&db), "");
}

WOLANG_TEST(EffectRowTest, TaskStatesTheEffectWithoutAUsesClause) {
  // `task` is part of the signature, so a caller sees `async` whether or
  // not there is a clause to put it in -- and the author does not have to
  // write the same thing twice.
  testing::TempTree tree("async-stated");
  tree.Write("wolang.toml", "[package]\nname = \"game\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", R"(
offer task fun Yield() {
  wait Nothing()
}

task fun Nothing() {}
)");
  Database db;
  DiagnosticEngine diagnostics;
  WOLANG_ASSERT(LoadPackage(&db, tree.path(), &diagnostics));
  WOLANG_EXPECT_EQ(PackageCodes(&db), "");
}

}  // namespace
}  // namespace wolang
