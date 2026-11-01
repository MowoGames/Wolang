// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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

// Writes a one-file package and returns the codes it reports (D221, D222).
std::string CodesOf(const char* name, const char* source) {
  testing::TempTree tree(name);
  tree.Write("wolang.toml", "[package]\nname = \"t\"\nversion = \"1\"\n");
  tree.Write("src/main.wo", source);
  Database db;
  DiagnosticEngine diagnostics;
  if (!LoadPackage(&db, tree.path(), &diagnostics)) {
    return "load failed";
  }
  return PackageCodes(&db);
}

WOLANG_TEST(EffectRowTest, BodilessUnsafePrimitiveReExportsUnsafe) {
  // A primitive has no body vouching for it, so its `uses unsafe` is the
  // caller's to state, like `extern` (D221).
  WOLANG_EXPECT_EQ(CodesOf("prim-fires", R"(
unsafe fun Free(items: []u8)

offer fun Drop(items: []u8) {
  Free(items)
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("prim-twin", R"(
unsafe fun Free(items: []u8)

offer fun Drop(items: []u8) uses unsafe {
  Free(items)
}
)"), "");
}

WOLANG_TEST(EffectRowTest, WrapperWithABodyStillAbsorbsUnsafe) {
  WOLANG_EXPECT_EQ(CodesOf("wrap-absorbs", R"(
unsafe fun Free(items: []u8)

fun Release(items: []u8) uses unsafe {
  Free(items)
}

fun Safe(items: []u8) {
  Release(items)
}
)"), "");
}

WOLANG_TEST(EffectRowTest, AnUnsafeFunChargesItsCaller) {
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-fires", R"(
unsafe fun Poke() {}

offer fun Use() {
  Poke()
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-twin", R"(
unsafe fun Poke() {}

offer fun Use() uses unsafe {
  Poke()
}
)"), "");
}

WOLANG_TEST(EffectRowTest, AnUnsafeFunBodyStatesNothing) {
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-body", R"(
extern "C" fun abs(n: s32) -> s32

offer unsafe fun Abs(n: s32) -> s32 {
  return abs(n)
}
)"), "");
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-named", R"(
unsafe fun Poke() {}

offer fun Pick() uses io {
  const _f: fun() = Poke
}
)"), "E0701");
}

WOLANG_TEST(EffectRowTest, AnUnsafeFunStillStatesItsOtherEffects) {
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-io", R"(
offer unsafe fun Poke() {
  Out()
}

fun Out() uses io
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("unsafe-fun-io-twin", R"(
offer unsafe fun Poke() uses io {
  Out()
}

fun Out() uses io
)"), "");
}

WOLANG_TEST(EffectRowTest, NamingAFunctionAsAValueChargesItsEffects) {
  // Binding a function to a local and calling that must cost what calling
  // it directly does (D222).
  WOLANG_EXPECT_EQ(CodesOf("value-fires", R"(
fun Out() uses io

offer fun Sneaky() {
  const w: fun() = Out
  w()
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("value-twin", R"(
fun Out() uses io

offer fun Sneaky() uses io {
  const w: fun() = Out
  w()
}
)"), "");
  WOLANG_EXPECT_EQ(CodesOf("value-unsafe", R"(
unsafe fun Free(items: []u8)

offer fun Sneaky() {
  const _w: fun([]u8) = Free
}
)"), "E0704");
}

WOLANG_TEST(EffectRowTest, ComptimeCannotNameAnIoFunctionAsAValue) {
  WOLANG_EXPECT_EQ(CodesOf("value-comptime", R"(
fun Out() uses io

comptime fun Pick() -> int {
  const _w: fun() = Out
  return 1
}
)"), "E0702");
  WOLANG_EXPECT_EQ(CodesOf("value-comptime-twin", R"(
fun Pure() {}

comptime fun Pick() -> int {
  const _w: fun() = Pure
  return 1
}
)"), "");
}

// D232: what a `@Thread` parameter is given runs on another thread.
WOLANG_TEST(EffectRowTest, ThreadBodyCannotReachAMutableGlobal) {
  WOLANG_EXPECT_EQ(CodesOf("thread-global", R"(
var shared: int = 0

@Thread
fun Launch(body: fun()) {
  body()
}

fun Work() {
  shared = shared + 1
}

fun Go() {
  Launch(Work)
}
)"), "E0147");
  // Through a call, not only directly.
  WOLANG_EXPECT_EQ(CodesOf("thread-global-indirect", R"(
var shared: int = 0

@Thread
fun Launch(body: fun()) {
  body()
}

fun Bump() {
  shared = 1
}

fun Work() {
  Bump()
}

fun Go() {
  Launch(Work)
}
)"), "E0147");
  // A closure body is judged too.
  WOLANG_EXPECT_EQ(CodesOf("thread-global-closure", R"(
var shared: int = 0

@Thread
fun Launch(body: fun()) {
  body()
}

fun Go() {
  Launch(|| { shared = 2 })
}
)"), "E0147");
}

WOLANG_TEST(EffectRowTest, ThreadBodyThatTouchesNothingIsFine) {
  WOLANG_EXPECT_EQ(CodesOf("thread-clean", R"(
const limit: int = 3
var shared: int = 0

@Thread
fun Launch(body: fun()) {
  body()
}

fun Work() {
  const _x = limit
}

fun Poke() {
  shared = 1
}

fun Go() {
  Launch(Work)
  Poke()
}
)"), "");
  // The caller takes responsibility.
  WOLANG_EXPECT_EQ(CodesOf("thread-unsafe", R"(
var shared: int = 0

@Thread
fun Launch(body: fun()) {
  body()
}

fun Work() {
  shared = shared + 1
}

fun Go() uses unsafe {
  Launch(Work)
}
)"), "");
}

WOLANG_TEST(EffectRowTest, ThreadClosureCannotCapture) {
  WOLANG_EXPECT_EQ(CodesOf("thread-capture", R"(
@Thread
fun Launch(body: fun()) {
  body()
}

fun Go() {
  const n = 5
  Launch(|| { const _m = n })
}
)"), "E0148");
  WOLANG_EXPECT_EQ(CodesOf("thread-capture-twin", R"(
@Thread
fun Launch(body: fun()) {
  body()
}

fun Go() {
  Launch(|| { const _m = 5 })
}
)"), "");
  // Not even `uses unsafe` supplies the environment a capture needs.
  WOLANG_EXPECT_EQ(CodesOf("thread-capture-unsafe", R"(
@Thread
fun Launch(body: fun()) {
  body()
}

fun Go() uses unsafe {
  const n = 5
  Launch(|| { const _m = n })
}
)"), "E0148");
  // `@Thread(captures)`: the runtime copies the environment per thread.
  WOLANG_EXPECT_EQ(CodesOf("thread-capture-allowed", R"(
@Thread(captures)
fun Launch(body: fun()) {
  body()
}

fun Go() {
  const n = 5
  Launch(|| { const _m = n })
}
)"), "");
}

WOLANG_TEST(EffectRowTest, ANamedMethodChargesItsEffects) {
  // A method called or named as a value reaches its effects as surely as a
  // function does; neither was charged before (D222).
  WOLANG_EXPECT_EQ(CodesOf("method-call-fires", R"(
struct Box {
  var n: int

  fun Out() uses io {
  }
}

offer fun Sneaky(b: Box) {
  b.Out()
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("method-value-fires", R"(
struct Box {
  var n: int

  fun Out() uses io {
  }
}

offer fun Sneaky(b: Box) {
  const _w = b.Out
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("method-type-value-fires", R"(
struct Box {
  var n: int

  fun Out() uses io {
  }
}

offer fun Sneaky() {
  const _w = Box.Out
}
)"), "E0704");
  WOLANG_EXPECT_EQ(CodesOf("method-twin", R"(
struct Box {
  var n: int

  fun Out() uses io {
  }

  fun Quiet() {
  }
}

offer fun Declared(b: Box) uses io {
  const _w = b.Out
  b.Out()
}

offer fun Pure(b: Box) {
  b.Quiet()
  const _w = b.Quiet
}
)"), "");
}

}  // namespace
}  // namespace wolang
