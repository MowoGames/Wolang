// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

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
  WOLANG_EXPECT_EQ(store.Int(IntWidth::k64, true), store.default_int());
  WOLANG_EXPECT_NE(store.Int(IntWidth::k32, true), store.default_int());

  const TypeId a = store.Optional(store.default_int());
  const TypeId b = store.Optional(store.default_int());
  WOLANG_EXPECT_EQ(a, b);
}

WOLANG_TEST(TypeStoreTest, StructuralTypesAreInternedByContents) {
  TypeStore store;
  const TypeId wide = store.default_int();
  const TypeId f32 = store.default_float();

  WOLANG_EXPECT_EQ(store.Tuple({wide, f32}), store.Tuple({wide, f32}));
  WOLANG_EXPECT_NE(store.Tuple({wide, f32}), store.Tuple({f32, wide}));
  WOLANG_EXPECT_EQ(store.Function({wide}, f32), store.Function({wide}, f32));
  WOLANG_EXPECT_NE(store.Function({wide}, f32), store.Function({f32}, f32));
  WOLANG_EXPECT_NE(store.Slice(wide, false), store.Slice(wide, true));
  WOLANG_EXPECT_NE(store.Array(wide, 3, false), store.Array(wide, 4, false));
  WOLANG_EXPECT_NE(store.Pointer(wide, false), store.Pointer(wide, true));
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
  const TypeId wide = store.default_int();
  const TypeId f32 = store.default_float();

  const TypeId tuple = store.Tuple({wide, f32, wide});
  const std::vector<TypeId> elements = store.ArgumentsOf(tuple);
  WOLANG_ASSERT_EQ(elements.size(), size_t{3});
  WOLANG_EXPECT_EQ(elements[0], wide);
  WOLANG_EXPECT_EQ(elements[1], f32);

  WOLANG_EXPECT_EQ(store.ElementOf(store.Optional(wide)), wide);
  WOLANG_EXPECT_EQ(store.ElementOf(store.Slice(f32, false)), f32);
  WOLANG_EXPECT_EQ(store.ElementOf(store.Function({}, f32)), f32);
  WOLANG_EXPECT(!store.ElementOf(wide).is_valid());
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
  const TypeId wide = store.default_int();
  const TypeId errors = store.ErrorSet({});
  const TypeId wrapped = store.Fallible(store.Optional(wide), errors);
  WOLANG_EXPECT_EQ(store.StripWrappers(wrapped), wide);
  WOLANG_EXPECT_EQ(store.StripWrappers(wide), wide);
}

WOLANG_TEST(TypeStoreTest, DescribesTypesTheWayTheyAreWritten) {
  TypeStore store;
  StringInterner interner;
  const TypeId wide = store.default_int();
  const TypeId f32 = store.default_float();
  const TypeId pawn = store.Named(interner.Intern("Pawn"), 0, 0);
  const TypeId array = store.Named(interner.Intern("Array"), 0, 1);

  WOLANG_EXPECT_EQ(store.Describe(interner, wide), "s64");
  WOLANG_EXPECT_EQ(store.Describe(interner, f32), "f32");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Int(IntWidth::k8, false)),
                   "u8");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Optional(pawn)), "Pawn?");
  WOLANG_EXPECT_EQ(
      store.Describe(interner, store.Fallible(pawn, store.ErrorSet({}))),
      "Pawn!");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Slice(f32, false)),
                   "[]f32");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Slice(pawn, true)),
                   "soa []Pawn");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Array(f32, 3, false)),
                   "[3]f32");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Instance(array, {wide})),
                   "Array<s64>");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Tuple({wide, f32})),
                   "(s64, f32)");
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Function({wide}, f32)),
                   "fun(s64) -> f32");
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

static ErrorMember MakeMember(StringInterner* interner, std::string_view name, TypeId detail) {
  ErrorMember member;
  member.name = interner->Intern(name);
  member.tag = ErrorTag(name);
  member.detail = detail;
  return member;
}

WOLANG_TEST(TypeStoreTest, ErrorTagsAreStableAndNeverZero) {
  WOLANG_EXPECT_NE(ErrorTag("std.json.Malformed"), ErrorTag("std.yaml.Malformed"));
  WOLANG_EXPECT_EQ(ErrorTag("a.B"), ErrorTag("a.B"));
  WOLANG_EXPECT_NE(ErrorTag("a.B"), 0u);
  WOLANG_EXPECT_NE(ErrorTag(""), 0u);
}

WOLANG_TEST(TypeStoreTest, StatedErrorSetsAreOrderIndependent) {
  TypeStore store;
  StringInterner interner;
  const ErrorMember a = MakeMember(&interner, "m.A", TypeId{});
  const ErrorMember b = MakeMember(&interner, "m.B", TypeId{});
  WOLANG_EXPECT_EQ(store.StatedErrorSet({a, b}), store.StatedErrorSet({b, a}));
  WOLANG_EXPECT_EQ(store.StatedErrorSet({a, a}), store.StatedErrorSet({a}));
  WOLANG_EXPECT_NE(store.StatedErrorSet({a}), store.StatedErrorSet({a, b}));
  WOLANG_EXPECT_EQ(store.StatedErrorsOf(store.StatedErrorSet({a, b})).size(), size_t{2});
  WOLANG_EXPECT(store.IsOpenErrorSet(store.OpenErrorSet()));
  WOLANG_EXPECT(!store.IsOpenErrorSet(store.StatedErrorSet({a})));
}

WOLANG_TEST(TypeStoreTest, ErrorSetFitsFollowsTheSubsetRule) {
  TypeStore store;
  StringInterner interner;
  const TypeId box = store.Named(interner.Intern("$err.m.P"), 0, 0);
  const ErrorMember a = MakeMember(&interner, "m.A", TypeId{});
  const ErrorMember b = MakeMember(&interner, "m.B", TypeId{});
  const ErrorMember p = MakeMember(&interner, "m.P", box);
  const TypeId open = store.OpenErrorSet();
  const TypeId only_a = store.StatedErrorSet({a});
  const TypeId a_and_b = store.StatedErrorSet({a, b});
  const TypeId only_p = store.StatedErrorSet({p});

  std::vector<Symbol> missing;
  WOLANG_EXPECT(store.ErrorSetFits(a_and_b, only_a, &missing) == ErrorFit::kFits);
  WOLANG_EXPECT(missing.empty());
  WOLANG_EXPECT(store.ErrorSetFits(only_a, a_and_b, &missing) == ErrorFit::kMissing);
  WOLANG_EXPECT_EQ(missing.size(), size_t{1});
  WOLANG_EXPECT(store.ErrorSetFits(only_a, open, nullptr) == ErrorFit::kMissing);
  WOLANG_EXPECT(store.ErrorSetFits(open, a_and_b, nullptr) == ErrorFit::kFits);
  WOLANG_EXPECT(store.ErrorSetFits(open, only_p, nullptr) == ErrorFit::kPayloadIntoOpen);
  WOLANG_EXPECT(store.ErrorSetFits(only_p, only_p, nullptr) == ErrorFit::kFits);
}

WOLANG_TEST(TypeStoreTest, DescribesAStatedSetOfFallible) {
  TypeStore store;
  StringInterner interner;
  const ErrorMember a = MakeMember(&interner, "m.A", TypeId{});
  const ErrorMember b = MakeMember(&interner, "m.B", TypeId{});
  const TypeId wide = store.default_int();
  WOLANG_EXPECT_EQ(store.Describe(interner, store.Fallible(wide, store.StatedErrorSet({a}))), "s64!A");
  const std::string two = store.Describe(interner, store.Fallible(wide, store.StatedErrorSet({a, b})));
  WOLANG_EXPECT(two == "s64!(A, B)" || two == "s64!(B, A)");
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
  const std::vector<Symbol> table = {interner.Intern("Length"),
                                     interner.Intern("Time")};
  const auto names = [&table](DimensionId d) {
    return d.index < table.size() ? table[d.index] : Symbol{};
  };
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
  // Dimensions are interned by their declaration, so the same one named in
  // two files is one dimension.
  const DimensionId length = store.Dimension(interner.Intern("Length"), 0, 0);
  const DimensionId time = store.Dimension(interner.Intern("Time"), 0, 1);
  WOLANG_EXPECT_EQ(store.Dimension(interner.Intern("Length"), 0, 0), length);
  WOLANG_EXPECT_NE(store.Dimension(interner.Intern("Length"), 1, 0), length);
  const TypeId f32 = store.default_float();

  const TypeId metres = store.Quantity(f32, Dimensions::Base(length));
  const TypeId seconds = store.Quantity(f32, Dimensions::Base(time));
  WOLANG_EXPECT_NE(metres, seconds);
  WOLANG_EXPECT_EQ(metres, store.Quantity(f32, Dimensions::Base(length)));
  WOLANG_EXPECT(store.IsNumeric(metres));

  const TypeId velocity = store.Quantity(
      f32, Dimensions::Base(length).Divide(Dimensions::Base(time)));
  WOLANG_EXPECT_EQ(store.Describe(interner, velocity), "Length/Time");
  WOLANG_EXPECT_EQ(store.DimensionsOf(velocity).ExponentOf(time), -1);

  // A dimensionless quantity is just its carrier, so `10m / 2m` works
  // anywhere a plain number does.
  WOLANG_EXPECT_EQ(store.Quantity(f32, Dimensions()), f32);
}


WOLANG_TEST(TypeStoreTest, UnifiesAndSubstitutesTypeParameters) {
  TypeStore store;
  StringInterner interner;
  const TypeId t = store.Parameter(interner.Intern("T"), 0, 0);
  const TypeId u = store.Parameter(interner.Intern("U"), 0, 1);
  const TypeId wide = store.default_int();
  const TypeId f32 = store.default_float();

  WOLANG_EXPECT(store.MentionsParameter(store.Slice(t, false)));
  WOLANG_EXPECT(!store.MentionsParameter(store.Slice(wide, false)));

  TypeStore::Substitution substitution;
  WOLANG_EXPECT(store.Unify(store.Slice(t, false), store.Slice(wide, false),
                            &substitution));
  WOLANG_EXPECT_EQ(store.Substitute(t, substitution), wide);
  WOLANG_EXPECT_EQ(store.Substitute(store.Optional(t), substitution),
                   store.Optional(wide));

  // A parameter already inferred cannot be inferred differently.
  WOLANG_EXPECT(!store.Unify(t, f32, &substitution));

  // A second parameter is independent.
  WOLANG_EXPECT(store.Unify(u, f32, &substitution));
  WOLANG_EXPECT_EQ(store.Substitute(store.Tuple({t, u}), substitution),
                   store.Tuple({wide, f32}));

  // Walking a type never follows a field its kind does not use: a tuple has
  // no element type, and reading one would be a zeroed id pointing at a real
  // type.
  WOLANG_EXPECT(!store.MentionsParameter(store.Tuple({wide, f32})));
  WOLANG_EXPECT(store.MentionsParameter(store.Tuple({wide, t})));
}

WOLANG_TEST(TypeStoreTest, KeptAndBorrowedFunctionsAreDistinct) {
  TypeStore store;
  const TypeId i32 = store.Int(IntWidth::k32, true);
  const TypeId kept = store.Function({i32}, i32, true);
  const TypeId borrowed = store.Function({i32}, i32, false);
  WOLANG_EXPECT_NE(kept, borrowed);
  WOLANG_EXPECT_EQ(store.Kept(borrowed), kept);
  WOLANG_EXPECT_EQ(store.Borrowed(kept), borrowed);
  WOLANG_EXPECT(store.IsKept(kept));
  WOLANG_EXPECT(!store.IsKept(borrowed));
  WOLANG_EXPECT_EQ(store.Borrowed(store.Optional(kept)), store.Optional(borrowed));
  WOLANG_EXPECT_EQ(store.Kept(i32), i32);
  WOLANG_EXPECT(store.IsKept(i32));
  WOLANG_EXPECT(!store.IsFunctionLike(i32));
  WOLANG_EXPECT(store.IsFunctionLike(store.Optional(borrowed)));
}

WOLANG_TEST(TypeStoreTest, UnifyIgnoresKept) {
  TypeStore store;
  const TypeId i32 = store.Int(IntWidth::k32, true);
  TypeStore::Substitution substitution;
  WOLANG_EXPECT(store.Unify(store.Function({i32}, i32, true), store.Function({i32}, i32, false), &substitution));
}

}  // namespace
}  // namespace wolang
