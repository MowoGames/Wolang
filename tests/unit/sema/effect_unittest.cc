// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/effect.h"

#include "harness/test.h"
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

}  // namespace
}  // namespace wolang
