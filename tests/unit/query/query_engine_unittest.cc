// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/query/query_engine.h"

#include <string>

#include "harness/test.h"

namespace wolang {
namespace {

// The engine is generic, so the tests drive it with stand-in queries:
// kSourceText and kModuleFiles are the two inputs, and the derived queries
// stand for "cheap derivation" and "derivation whose result often repeats".
constexpr QueryKey kTextA{QueryKind::kSourceText, 1};
constexpr QueryKey kTextB{QueryKind::kSourceText, 2};

QueryKey Tokens(uint32_t file) {
  return QueryKey{QueryKind::kTokens, file};
}
QueryKey Items(uint32_t file) {
  return QueryKey{QueryKind::kItemTree, file};
}
QueryKey Scope(uint32_t module) {
  return QueryKey{QueryKind::kModuleScope, module};
}

void SetText(QueryEngine* engine, QueryKey key, std::string text) {
  engine->SetInput(key, MakeResult(std::move(text)));
}

WOLANG_TEST(QueryKindTest, NamesAndCategories) {
  WOLANG_EXPECT_EQ(QueryKindName(QueryKind::kSourceText), "SourceText");
  WOLANG_EXPECT_EQ(QueryKindName(QueryKind::kItemTree), "ItemTree");
  WOLANG_EXPECT(IsInputQuery(QueryKind::kSourceText));
  WOLANG_EXPECT(!IsInputQuery(QueryKind::kItemTree));
  WOLANG_EXPECT(!QueryKindDescription(QueryKind::kTokens).empty());

  for (uint16_t i = 0; i < static_cast<uint16_t>(QueryKind::kCount); ++i) {
    const auto kind = static_cast<QueryKind>(i);
    WOLANG_ASSERT(!QueryKindName(kind).empty());
    WOLANG_ASSERT(!QueryKindDescription(kind).empty());
  }
}

WOLANG_TEST(QueryKeyTest, PacksAndCompares) {
  WOLANG_EXPECT_EQ(kTextA, (QueryKey{QueryKind::kSourceText, 1}));
  WOLANG_EXPECT_NE(kTextA, kTextB);
  WOLANG_EXPECT_NE(kTextA.Pack(), Tokens(1).Pack());
}

WOLANG_TEST(QueryEngine, UnsetInputHasNoValue) {
  QueryEngine engine;
  WOLANG_EXPECT(engine.Get(kTextA) == nullptr);
  WOLANG_EXPECT(!engine.HasValue(kTextA));
}

WOLANG_TEST(QueryEngine, UnregisteredQueryHasNoValue) {
  QueryEngine engine;
  WOLANG_EXPECT(engine.Get(Tokens(1)) == nullptr);
}

WOLANG_TEST(QueryEngine, InputRoundTripsAndBumpsTheRevision) {
  QueryEngine engine;
  const Revision before = engine.revision();
  SetText(&engine, kTextA, "fn f() {}");

  WOLANG_EXPECT_GT(engine.revision(), before);
  const std::string* text = engine.GetAs<std::string>(kTextA);
  WOLANG_ASSERT_NE(text, nullptr);
  WOLANG_EXPECT_EQ(*text, "fn f() {}");
}

WOLANG_TEST(QueryEngine, DerivedQueryRunsOnceAndThenHits) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<size_t>(text ? text->size() : 0);
                  });

  SetText(&engine, kTextA, "abcd");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(1)), size_t{4});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{1});

  // Asking again in the same revision does not recompute.
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(1)), size_t{4});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{1});
  WOLANG_EXPECT_GT(engine.total_hits(), uint64_t{0});
}

WOLANG_TEST(QueryEngine, ChangingAnInputRecomputesDependents) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<size_t>(text ? text->size() : 0);
                  });

  SetText(&engine, kTextA, "abcd");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(1)), size_t{4});

  SetText(&engine, kTextA, "abcdef");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(1)), size_t{6});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{2});
}

WOLANG_TEST(QueryEngine, UnrelatedInputDoesNotRecompute) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<size_t>(text ? text->size() : 0);
                  });

  SetText(&engine, kTextA, "abcd");
  SetText(&engine, kTextB, "xy");
  engine.Get(Tokens(1));
  engine.ResetCounters();

  // Editing file 2 must not disturb the derived value of file 1.
  SetText(&engine, kTextB, "xyz");
  engine.Get(Tokens(1));
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{0});
  WOLANG_EXPECT_EQ(engine.total_verifications(), uint64_t{1});
}

// The core incremental property: a derived value that comes out equal stops
// invalidation from spreading, even though it had to be recomputed itself.
WOLANG_TEST(QueryEngine, EqualRecomputationKeepsDependentsGreen) {
  QueryEngine engine;

  // "Signature" of a file: its first line only. Comparable, so an edit below
  // the first line produces an equal value.
  engine.Register(QueryKind::kItemTree,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    std::string signature;
                    if (text != nullptr) {
                      signature = text->substr(0, text->find('\n'));
                    }
                    return MakeComparableResult(std::move(signature));
                  });

  // "Scope" of a module: derived from the signature, never from the body.
  engine.Register(QueryKind::kModuleScope,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* signature =
                        db.GetAs<std::string>(Items(arg));
                    return MakeResult<size_t>(signature ? signature->size()
                                                        : 0);
                  });

  SetText(&engine, kTextA, "fn f() -> int\n  return 1\n");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Scope(1)), size_t{13});
  engine.ResetCounters();

  // Edit the body only. The signature query must rerun, because its input
  // changed, but its result is equal, so the scope stays valid.
  SetText(&engine, kTextA, "fn f() -> int\n  return 2\n");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Scope(1)), size_t{13});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kItemTree),
                   uint64_t{1});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kModuleScope),
                   uint64_t{0});

  // Editing the signature does propagate.
  engine.ResetCounters();
  SetText(&engine, kTextA, "fn f() -> bool\n  return 2\n");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Scope(1)), size_t{14});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kItemTree),
                   uint64_t{1});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kModuleScope),
                   uint64_t{1});
}

WOLANG_TEST(QueryEngine, ResultsWithoutComparisonAlwaysPropagate) {
  QueryEngine engine;
  engine.Register(QueryKind::kItemTree,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    db.Get(QueryKey{QueryKind::kSourceText, arg});
                    // No comparison: every recomputation counts as a change.
                    return MakeResult<int>(7);
                  });
  engine.Register(QueryKind::kModuleScope,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const int* value = db.GetAs<int>(Items(arg));
                    return MakeResult<int>(value ? *value : 0);
                  });

  SetText(&engine, kTextA, "one");
  engine.Get(Scope(1));
  engine.ResetCounters();

  SetText(&engine, kTextA, "two");
  engine.Get(Scope(1));
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kModuleScope),
                   uint64_t{1});
}

WOLANG_TEST(QueryEngine, DiamondDependenciesComputeEachQueryOnce) {
  QueryEngine engine;
  // Tokens depends on the text; ItemTree and ModuleScope both depend on
  // Tokens; Imports depends on both of them.
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<size_t>(text ? text->size() : 0);
                  });
  engine.Register(QueryKind::kItemTree,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    return MakeResult<size_t>(*db.GetAs<size_t>(Tokens(arg)) +
                                              1);
                  });
  engine.Register(QueryKind::kModuleScope,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    return MakeResult<size_t>(*db.GetAs<size_t>(Tokens(arg)) +
                                              2);
                  });
  engine.Register(QueryKind::kImports,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    return MakeResult<size_t>(*db.GetAs<size_t>(Items(arg)) +
                                              *db.GetAs<size_t>(Scope(arg)));
                  });

  SetText(&engine, kTextA, "abcd");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(QueryKey{QueryKind::kImports, 1}),
                   size_t{11});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{1});
  WOLANG_EXPECT_EQ(engine.total_computations(), uint64_t{4});
}

WOLANG_TEST(QueryEngine, ConstantQueryIsNeverRecomputed) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine&, uint32_t) -> QueryResult {
                    return MakeResult<int>(42);
                  });

  engine.Get(Tokens(1));
  SetText(&engine, kTextA, "unrelated");
  SetText(&engine, kTextB, "also unrelated");
  engine.Get(Tokens(1));
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{1});
}

WOLANG_TEST(QueryEngine, DetectsCyclesInsteadOfRecursingForever) {
  QueryEngine engine;
  engine.Register(QueryKind::kItemTree,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    db.Get(Scope(arg));
                    return MakeResult<int>(1);
                  });
  engine.Register(QueryKind::kModuleScope,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    db.Get(Items(arg));
                    return MakeResult<int>(2);
                  });

  WOLANG_EXPECT(engine.Get(Items(1)) != nullptr);
  WOLANG_EXPECT(engine.cycle_detected());
  engine.clear_cycle_detected();
  WOLANG_EXPECT(!engine.cycle_detected());
}

WOLANG_TEST(QueryEngine, TracksDistinctArgumentsSeparately) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<size_t>(text ? text->size() : 0);
                  });

  SetText(&engine, kTextA, "abcd");
  SetText(&engine, kTextB, "xy");
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(1)), size_t{4});
  WOLANG_EXPECT_EQ(*engine.GetAs<size_t>(Tokens(2)), size_t{2});
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{2});
  WOLANG_EXPECT_EQ(engine.node_count(), size_t{4});
}

WOLANG_TEST(QueryEngine, MissingDependencyIsRecordedAndRetriedLater) {
  QueryEngine engine;
  engine.Register(QueryKind::kTokens,
                  [](QueryEngine& db, uint32_t arg) -> QueryResult {
                    const std::string* text = db.GetAs<std::string>(
                        QueryKey{QueryKind::kSourceText, arg});
                    return MakeResult<std::string>(text ? *text : "<missing>");
                  });

  // The input does not exist yet, but the dependency is still recorded.
  WOLANG_EXPECT_EQ(*engine.GetAs<std::string>(Tokens(1)), "<missing>");

  SetText(&engine, kTextA, "now here");
  WOLANG_EXPECT_EQ(*engine.GetAs<std::string>(Tokens(1)), "now here");
  WOLANG_EXPECT_EQ(engine.ComputationCount(QueryKind::kTokens), uint64_t{2});
}

}  // namespace
}  // namespace wolang
