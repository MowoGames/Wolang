// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/reader.h"

#include <string>
#include <string_view>
#include <vector>

#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/ir/printer.h"
#include "wolang/ir/verifier.h"

namespace wolang {

static std::string JoinLines(const std::vector<std::string>& lines) {
  std::string result;
  for (const std::string& line : lines) {
    if (!result.empty()) {
      result.push_back('\n');
    }
    result.append(line);
  }
  return result;
}

static void CollectNamed(const TypeStore& types, const StringInterner& interner, TypeId type, ir::ReadOptions* options) {
  if (!type.is_valid()) {
    return;
  }

  const Type& described = types.Get(type);
  switch (described.kind) {
    case TypeKind::kVoid:
    case TypeKind::kNever:
    case TypeKind::kBool:
    case TypeKind::kChar:
    case TypeKind::kString:
    case TypeKind::kStr:
    case TypeKind::kInt:
    case TypeKind::kFloat:
      return;
    case TypeKind::kOptional:
    case TypeKind::kPointer:
    case TypeKind::kReference:
    case TypeKind::kSlice:
    case TypeKind::kArray:
      CollectNamed(types, interner, described.element, options);
      return;
    case TypeKind::kTuple:
      for (TypeId element : types.ArgumentsOf(type)) {
        CollectNamed(types, interner, element, options);
      }
      return;
    case TypeKind::kFunction:
      for (TypeId param : types.ArgumentsOf(type)) {
        CollectNamed(types, interner, param, options);
      }
      CollectNamed(types, interner, described.element, options);
      return;
    default:
      options->named[types.Describe(interner, type)] = type;
      return;
  }
}

static ir::ReadOptions NamedTypesOf(const Database& db, const ir::Module& module) {
  ir::ReadOptions options;
  for (const ir::Function& function : module.functions) {
    CollectNamed(db.types(), db.interner(), function.type, &options);
    CollectNamed(db.types(), db.interner(), function.self_type, &options);
    for (const ir::Inst& inst : function.instructions) {
      CollectNamed(db.types(), db.interner(), inst.type, &options);
    }
  }
  for (const ir::ClassInfo& info : module.classes) {
    CollectNamed(db.types(), db.interner(), info.type, &options);
  }
  return options;
}

static void ExpectRoundTrip(std::string_view code) {
  Database db;
  const FileId file = db.AddFile("<test>", std::string(code));
  const LoweredFile* lowered = db.Lowered(file);
  WOLANG_ASSERT(lowered != nullptr);
  WOLANG_EXPECT_EQ(JoinLines(lowered->problems), "");

  const std::string first = ir::PrintModuleTyped(db.interner(), db.types(), lowered->unoptimized);
  const ir::ReadOptions options = NamedTypesOf(db, lowered->unoptimized);
  const ir::ReadResult read = ir::ReadModule(&db.interner(), &db.types(), first, options);
  WOLANG_EXPECT_EQ(JoinLines(read.errors), "");

  const std::string second = ir::PrintModuleTyped(db.interner(), db.types(), read.module);
  WOLANG_EXPECT_EQ(second, first);

  const ir::VerificationResult verified = ir::Verify(db.interner(), db.types(), read.module);
  WOLANG_EXPECT_EQ(JoinLines(verified.problems), "");
}

static std::string ReadErrors(std::string_view text, const ir::ReadOptions& options) {
  StringInterner interner;
  TypeStore types;
  const ir::ReadResult read = ir::ReadModule(&interner, &types, text, options);
  return JoinLines(read.errors);
}

static const char kHeader[] = "module initializer=4294967295 runaway=4294967295\n";

WOLANG_TEST(IrReaderTest, RoundTripsArithmetic) {
  ExpectRoundTrip("fun Add(a: int, b: int) -> int {\n  return a * b + a - b\n}\n");
}

WOLANG_TEST(IrReaderTest, RoundTripsBranchesAndLoops) {
  ExpectRoundTrip(R"(
fun Sum(n: int) -> int {
  var total = 0
  var i = 0
  while i < n {
    if i % 2 == 0 {
      total = total + i
    }
    i = i + 1
  }
  return total
}
)");
}

WOLANG_TEST(IrReaderTest, RoundTripsAMatchAndFloats) {
  ExpectRoundTrip(R"(
fun Classify(n: int) -> float {
  return match n {
    0 => 1.5
    1 => 2.25
    _ => 0.1
  }
}
)");
}

WOLANG_TEST(IrReaderTest, RoundTripsTextAndOptionals) {
  ExpectRoundTrip(R"(
fun Name(flag: bool) -> text {
  if flag {
    return "wolf"
  }
  return "fox\n"
}

fun Pick(p: int?) -> int {
  if const q = p {
    return q
  }
  return -1
}
)");
}

WOLANG_TEST(IrReaderTest, RoundTripsStructsAndClasses) {
  ExpectRoundTrip(R"(
struct Pawn {
  var health: int
  var armour: int
}

class Counter {
  var count: int = 0

  fun Bump() -> int {
    count = count + 1
    return count
  }
}

fun Use(p: Pawn) -> int {
  var c = Counter()
  c.Bump()
  return p.health + p.armour + c.Bump()
}
)");
}

WOLANG_TEST(IrReaderTest, ReadsAMinimalFunction) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> s64 {\n"
                           "^0:  // entry\n"
                           "  %0 = const.int 7 : s64\n"
                           "  ret %0\n"
                           "}\n";
  WOLANG_EXPECT_EQ(ReadErrors(text, ir::ReadOptions()), "");
}

WOLANG_TEST(IrReaderTest, RefusesAnUnknownInstruction) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> void {\n"
                           "^0:  // entry\n"
                           "  frobnicate\n"
                           "}\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("`frobnicate` is not an instruction") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, RefusesAValueWithoutAName) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> void {\n"
                           "^0:  // entry\n"
                           "  const.int 7 : s64\n"
                           "  ret\n"
                           "}\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("produces a value and needs a name") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, RefusesAValueDefinedTwice) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> void {\n"
                           "^0:  // entry\n"
                           "  %0 = const.int 1 : s64\n"
                           "  %0 = const.int 2 : s64\n"
                           "  ret\n"
                           "}\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("%0 is defined twice") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, RefusesAFunctionWithoutItsClosingBrace) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> void {\n"
                           "^0:  // entry\n"
                           "  ret\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("missing its closing }") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, RefusesATypeItWasNotGiven) {
  const std::string text = std::string(kHeader) + "fun @\"F\" : fn(`Pawn`) -> void\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("`Pawn` is not one the reader was given") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, ReadsATypeItWasGiven) {
  StringInterner interner;
  TypeStore types;
  ir::ReadOptions options;
  options.named["Pawn"] = types.Named(interner.Intern("Pawn"), 0, 0, false, false);

  const std::string text = std::string(kHeader) + "fun @\"F\" : fn(`Pawn`) -> void\n";
  const ir::ReadResult read = ir::ReadModule(&interner, &types, text, options);
  WOLANG_EXPECT_EQ(JoinLines(read.errors), "");
  WOLANG_ASSERT(read.module.functions.size() == 1u);
  WOLANG_EXPECT(read.module.functions[0].is_declaration());
}

WOLANG_TEST(IrReaderTest, RefusesAParamInABody) {
  const std::string text = std::string(kHeader) +
                           "fun @\"F\" : fn() -> void {\n"
                           "^0:  // entry\n"
                           "  %0 = param : s64\n"
                           "  ret\n"
                           "}\n";
  WOLANG_EXPECT(ReadErrors(text, ir::ReadOptions()).find("a param belongs in a block header") != std::string::npos);
}

WOLANG_TEST(IrReaderTest, ReadsEveryPrimitiveTypeAFunctionCanTake) {
  const std::string text = std::string(kHeader) + "fun @\"F\" : fn(s8, u16, f32, bool, char, text, Text, opt(ptrmut(u8)), slice(s64), array(4, f64), tuple(s32, bool), fnkept(s64) -> s64) -> void\n";
  WOLANG_EXPECT_EQ(ReadErrors(text, ir::ReadOptions()), "");
}

}  // namespace wolang
