// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/sema/type.h"

#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/base/string_interner.h"
#include "wolang/sema/dimension.h"

namespace wolang {
namespace {

WOLANG_TEST(TypeStoreTest, BuiltinsAreDistinctAndStable) {
  TypeStore store;
  const std::vector<TypeId> builtins = {
      store.error(),       store.void_type(),    store.never(),
      store.bool_type(),   store.char_type(),    store.string_type(),
      store.str(),         store.int_literal(),  store.float_literal(),
  };
  for (size_t i = 0; i < builtins.size(); ++i) {
    for (size_t j = i + 1; j < builtins.size(); ++j) {
      WOLANG_ASSERT_NE(builtins[i], builtins[j]);
    }
  }
  WOLANG_EXPECT_EQ(store.KindOf(store.bool_type()), TypeKind::kBool);
  WOLANG_EXPECT_EQ(store.KindOf(store.never()), TypeKind::kNever);
}

WOLANG_TEST(TypeStoreTest, EqualTypesGetTheSameId) {
  TypeStore store;
  // Equality is the whole point of interning: one integer comparison.
  WOLANG_EXPECT_EQ(store.Int(IntWidth::k32, true),
                   store.Int(IntWidth::k32, true));
  WOLANG_EXPECT_NE(store.Int(IntWidth::k32, true),
                   store.Int(IntWidth::k32, false));
  WOLANG_EXPECT_NE(store.Int(IntWidth::k32, true),
                   store.Int(IntWidth::k64, true));
  WOLANG_EXPECT_EQ(store.Float(FloatWidth::k32), store.default_float());
  WOLANG_EXPECT_EQ(store.Int(IntWidth::k32, true), store.default_int());

  const TypeId a = store.Optional(store.default_int());
  const TypeId b = store.Optional(store.default_int());
  WOLANG_EXPECT_EQ(a, b);
}

WOLANG_TEST(TypeStoreTest, StructuralTypesAreInternedByContents) {
  TypeStore store;
  const TypeId i32 = store.default_int();
  const TypeId f32 = store.default_float();

  WOLANG_EXPECT_EQ(store.Tuple({i32, f32}), store.Tuple({i32, f32}));
  WOLANG_EXPECT_NE(store.Tuple({i32, f32}), store.Tuple({f32, i32}));
  WOLANG_EXPECT_EQ(store.Function({i32}, f32), store.Function({i32}, f32));
  WOLANG_EXPECT_NE(store.Function({i32}, f32), store.Function({f32}, f32));
  WOLANG_EXPECT_NE(store.Slice(i32, false), store.Slice(i32, true));
  WOLANG_EXPECT_NE(store.Array(i32, 3, false), store.Array(i32, 4, false));
  WOLANG_EXPECT_NE(store.Pointer(i32, false), store.Pointer(i32, true));
}

WOLANG_TEST(TypeStoreTest, NamedTypesAreIdentifiedByTheirDeclaration) {
  TypeStore store;
  StringInterner interner;
  const Symbol name = interner.Intern("Turret");

  // Two declarations with the same spelling in different files are different
  // types, which is what module privacy relies on.
  WOLANG_EXPECT_EQ(store.Named(name, 0, 1), store.Named(name, 0, 1));
  WOLANG_EXPECT_NE(store.Named(name, 0, 1), store.Named(name, 1, 1));
  WOLANG_EXPECT_NE(store.Named(name, 0, 1), store.Named(name, 0, 2));
}

WOLANG_TEST(TypeStoreTest, DoubleOptionalCollapses) {
  TypeStore store;
  const TypeId once = store.Optional(store.default_int());
  // There is one kind of absence, so `T??` says nothing `T?` does not.
  WOLANG_EXPECT_EQ(store.Optional(once), once);
}

WOLANG_TEST(TypeStoreTest, GenericInstanceWithoutArgumentsIsItsBase) {
  TypeStore store;
  StringInterner interner;
  const TypeId base = store.Named(interner.Intern("Array"), 0, 0);
  WOLANG_EXPECT_EQ(store.Instance(base, {}), base);
  WOLANG_EXPECT_NE(store.Instance(base, {store.default_int()}), base);
}

WOLANG_TEST(TypeStoreTest, AccessorsReturnWhatWasBuilt) {
  TypeStore store;
  const TypeId i32 = store.default_int();
  const TypeId f32 = store.default_float();

  const TypeId tuple = store.Tuple({i32, f32, i32});
  const std::vector<TypeId> elements = store.ArgumentsOf(tuple);
  WOLANG_ASSERT_EQ(elements.size(), size_t{3});
  WOLANG_EXPECT_EQ(elements[0], i32);
  WOLANG_EXPECT_EQ(elements[1], f32);

  WOLANG_EXPECT_EQ(store.ElementOf(store.Optional(i32)), i32);
  WOLANG_EXPECT_EQ(store.ElementOf(store.Slice(f32, false)), f32);
  WOLANG_EXPECT_EQ(store.ElementOf(store.Function({}, f32)), f32);
  WOLANG_EXPECT(!store.ElementOf(i32).is_valid());
}

WOLANG_TEST(TypeStoreTest, ClassifiesNumbers) {
  TypeStore store;
  WOLANG_EXPECT(store.IsInteger(store.default_int()));
  WOLANG_EXPECT(store.IsInteger(store.int_literal()));
  WOLANG_EXPECT(store.IsFloatingPoint(store.default_float()));
  WOLANG_EXPECT(store.IsFloatingPoint(store.float_literal()));
  WOLANG_EXPECT(store.IsNumeric(store.default_int()));
  WOLANG_EXPECT(!store.IsNumeric(store.bool_type()));
  WOLANG_EXPECT(!store.IsNumeric(store.str()));

  WOLANG_EXPECT(store.IsLiteral(store.int_literal()));
  WOLANG_EXPECT(!store.IsLiteral(store.default_int()));

  WOLANG_EXPECT_EQ(store.BitWidth(store.Int(IntWidth::k8, true)), 8u);
  WOLANG_EXPECT_EQ(store.BitWidth(store.Float(FloatWidth::k64)), 64u);
  WOLANG_EXPECT_EQ(store.BitWidth(store.bool_type()), 0u);
}

WOLANG_TEST(TypeStoreTest, StripsOptionalAndFallibleLayers) {
  TypeStore store;
  const TypeId i32 = store.default_int();
  const TypeId errors = store.ErrorSet({});
  const TypeId wrapped = store.Fallible(store.Optional(i32), errors);
  WOLANG_EXPECT_EQ(store.StripWrappers(wrapped), i32);
  WOLANG_EXPECT_EQ(store.StripWrappers(i32), i32);
}

WOLANG_TEST(TypeStoreTest, DescribesTypesTheWayTheyAreWritten) {
  TypeStore store;
  StringInterner interner;
  const TypeId i32 = store.default_int();
  const TypeId f32 = store.default_float();
  const TypeId pawn = store.Named(interner.Intern("Pawn"), 0, 0);
  const TypeId array = store.Named(interner.Intern("Array"), 0, 1);

  WOLANG_EXPECT_EQ(store.Describe(interner, i32), "i32");
  WOLANG_EXPECT_EQ(store.Describe(interner, f32), "f32");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Int(IntWidth::k8, false)),
                   "u8");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Optional(pawn)), "Pawn?");
  WOLANG_EXPECT_EQ(
      store.Describe(interner, store.Fallible(pawn, store.ErrorSet({}))),
      "Pawn!");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Slice(f32, false)),
                   "[f32]");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Slice(pawn, true)),
                   "soa [Pawn]");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Array(f32, 3, false)),
                   "[f32; 3]");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Instance(array, {i32})),
                   "Array<i32>");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Tuple({i32, f32})),
                   "(i32, f32)");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Function({i32}, f32)),
                   "fun(i32) -> f32");
  WOLANG_EXPECT_EQ(
      store.Describe(interner, store.Function({}, store.void_type())),
      "fun()");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Pointer(f32, true)),
                   "*mut f32");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Reference(pawn, false)),
                   "&Pawn");

  // A fallible optional reads left to right, as it is written.
  WOLANG_EXPECT_EQ(
      store.Describe(interner,
                     store.Fallible(store.Optional(pawn),
                                    store.ErrorSet({}))),
      "Pawn?!");
}

WOLANG_TEST(TypeStoreTest, ErrorSetsAreInternedByMembers) {
  TypeStore store;
  StringInterner interner;
  const Symbol not_found = interner.Intern("NotFound");
  const Symbol corrupt = interner.Intern("Corrupt");

  WOLANG_EXPECT_EQ(store.ErrorSet({not_found, corrupt}),
                   store.ErrorSet({not_found, corrupt}));
  WOLANG_EXPECT_NE(store.ErrorSet({not_found}),
                   store.ErrorSet({not_found, corrupt}));

  const TypeId set = store.ErrorSet({not_found, corrupt});
  const std::vector<Symbol> members = store.ErrorMembersOf(set);
  WOLANG_ASSERT_EQ(members.size(), size_t{2});
  WOLANG_EXPECT_EQ(interner.Text(members[0]), "NotFound");
  WOLANG_EXPECT_EQ(store.Describe(interner, set), "NotFound | Corrupt");
}

// --------------------------------------------------------------- quantities --

WOLANG_TEST(DimensionsTest, MultipliesAndDividesExponents) {
  const DimensionId length{0};
  const DimensionId time{1};

  const Dimensions scalar;
  WOLANG_EXPECT(scalar.is_dimensionless());

  const Dimensions l = Dimensions::Base(length);
  const Dimensions t = Dimensions::Base(time);
  WOLANG_EXPECT_EQ(l.ExponentOf(length), 1);
  WOLANG_EXPECT_EQ(l.ExponentOf(time), 0);

  const Dimensions velocity = l.Divide(t);
  WOLANG_EXPECT_EQ(velocity.ExponentOf(length), 1);
  WOLANG_EXPECT_EQ(velocity.ExponentOf(time), -1);

  const Dimensions acceleration = velocity.Divide(t);
  WOLANG_EXPECT_EQ(acceleration.ExponentOf(time), -2);

  const Dimensions area = l.Multiply(l);
  WOLANG_EXPECT_EQ(area.ExponentOf(length), 2);
  WOLANG_EXPECT_EQ(l.Power(3).ExponentOf(length), 3);
}

WOLANG_TEST(DimensionsTest, CancelledFactorsDisappear) {
  const DimensionId length{0};
  const Dimensions l = Dimensions::Base(length);
  // `10m / 2m` is a plain number, so the product must come out empty rather
  // than carrying a zero exponent.
  const Dimensions ratio = l.Divide(l);
  WOLANG_EXPECT(ratio.is_dimensionless());
  WOLANG_EXPECT_EQ(ratio, Dimensions());
  WOLANG_EXPECT(l.Power(0).is_dimensionless());
}

WOLANG_TEST(DimensionsTest, EqualityIgnoresConstructionOrder) {
  const DimensionId length{0};
  const DimensionId time{1};
  const Dimensions a =
      Dimensions::Base(length).Divide(Dimensions::Base(time));
  const Dimensions b = Dimensions()
                           .Divide(Dimensions::Base(time))
                           .Multiply(Dimensions::Base(length));
  WOLANG_EXPECT_EQ(a, b);
}

WOLANG_TEST(DimensionsTest, RootsOnlyWorkWhenExponentsDivide) {
  const DimensionId length{0};
  Dimensions result;
  // The square root of an area is a length.
  WOLANG_EXPECT(Dimensions::Base(length).Power(2).TryRoot(2, &result));
  WOLANG_EXPECT_EQ(result, Dimensions::Base(length));
  // The square root of a length is not expressible, which is the right
  // answer rather than a silently wrong one.
  WOLANG_EXPECT(!Dimensions::Base(length).TryRoot(2, &result));
}

WOLANG_TEST(DimensionsTest, DescribesProductsReadably) {
  StringInterner interner;
  const std::vector<Symbol> names = {interner.Intern("Length"),
                                     interner.Intern("Time")};
  const DimensionId length{0};
  const DimensionId time{1};

  WOLANG_EXPECT_EQ(Dimensions().Describe(interner, names), "1");
  WOLANG_EXPECT_EQ(Dimensions::Base(length).Describe(interner, names),
                   "Length");
  WOLANG_EXPECT_EQ(
      Dimensions::Base(length).Divide(Dimensions::Base(time))
          .Describe(interner, names),
      "Length/Time");
  WOLANG_EXPECT_EQ(
      Dimensions::Base(length)
          .Divide(Dimensions::Base(time).Power(2))
          .Describe(interner, names),
      "Length/Time^2");
  WOLANG_EXPECT_EQ(
      Dimensions::Base(length).Power(2).Describe(interner, names),
      "Length^2");
  WOLANG_EXPECT_EQ(
      Dimensions().Divide(Dimensions::Base(time)).Describe(interner, names),
      "1/Time");
}

WOLANG_TEST(TypeStoreTest, QuantitiesCarryTheirDimension) {
  TypeStore store;
  StringInterner interner;
  const std::vector<Symbol> names = {interner.Intern("Length"),
                                     interner.Intern("Time")};
  const DimensionId length{0};
  const DimensionId time{1};
  const TypeId f32 = store.default_float();

  const TypeId metres = store.Quantity(f32, Dimensions::Base(length));
  const TypeId seconds = store.Quantity(f32, Dimensions::Base(time));
  WOLANG_EXPECT_NE(metres, seconds);
  WOLANG_EXPECT_EQ(metres, store.Quantity(f32, Dimensions::Base(length)));
  WOLANG_EXPECT(store.IsNumeric(metres));

  const TypeId velocity = store.Quantity(
      f32, Dimensions::Base(length).Divide(Dimensions::Base(time)));
  WOLANG_EXPECT_EQ(store.Describe(interner, velocity, names), "Length/Time");
  WOLANG_EXPECT_EQ(store.DimensionsOf(velocity).ExponentOf(time), -1);

  // A dimensionless quantity is just its carrier, so `10m / 2m` works
  // anywhere a plain number does.
  WOLANG_EXPECT_EQ(store.Quantity(f32, Dimensions()), f32);
}

}  // namespace
}  // namespace wolang
