// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_SEMA_RESOLUTION_H_
#define WOLANG_SEMA_RESOLUTION_H_

#include <cstdint>
#include <span>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "wolang/base/diagnostic.h"
#include "wolang/base/string_interner.h"
#include "wolang/sema/dimension.h"
#include "wolang/sema/type.h"
#include "wolang/syntax/ast.h"

namespace wolang {

// What a name refers to.
enum class BindingKind : uint8_t {
  kType,       // struct, class, enum, trait, component, tag, type alias
  kFunction,
  kValue,      // a module-level `let` or `var`
  kLocal,      // a local binding, a parameter, or a pattern binding
  kDimension,
  kUnit,
  kEffect,
  kErrorTag,   // one member of an `error` declaration
  kSignal,
  kSystem,
  kMachine,
  kTypeParam,
};

// One name in scope.
struct Binding {
  Symbol name;
  BindingKind kind = BindingKind::kValue;
  TypeId type;
  // The declaration this name came from, as an index into its file's item
  // tree, or kNoItem for a local.
  static constexpr uint32_t kNoItem = 0xFFFFFFFFu;
  uint32_t item = kNoItem;
  uint32_t file = 0;
  // Units: the unit's dimension and its factor relative to the base unit, so
  // `0.5s` and `500ms` compare equal after conversion.
  DimensionId dimension;
  double factor = 1.0;
  // Locals: whether the binding may be reassigned.
  bool is_mutable = false;
  // Whether other modules may refer to this name.
  bool is_public = false;
  // Whether the declaration is `extern`: implemented outside the language,
  // reached through the C ABI. Carried here so a call site knows without
  // reading the declaring file -- calling one is the `unsafe` effect.
  bool is_foreign = false;
  // Whether the declaration is a `task`: it may suspend, so a call to it
  // has to be under a `wait` or a `spawn`. Carried here for the same reason
  // `is_foreign` is -- the call site must not have to read the declaring
  // file.
  bool is_task = false;
  // Whether the declaration has a body. A `task` without one is a
  // suspension primitive: it suspends once and then finishes, and the
  // compiler writes that machine itself.
  bool has_body = false;

  friend bool operator==(const Binding& lhs, const Binding& rhs) {
    return lhs.name == rhs.name && lhs.kind == rhs.kind &&
           lhs.type == rhs.type && lhs.item == rhs.item &&
           lhs.file == rhs.file && lhs.dimension == rhs.dimension &&
           lhs.factor == rhs.factor && lhs.is_mutable == rhs.is_mutable &&
           lhs.is_public == rhs.is_public &&
           lhs.is_foreign == rhs.is_foreign &&
           lhs.is_task == rhs.is_task && lhs.has_body == rhs.has_body;
  }
};

// The names one file can see, and the types its declarations introduce.
//
// This is the output of name resolution and the input to type checking. It
// holds no source positions: a diagnostic resolves its span against the
// current syntax tree, so a cached resolution never carries a stale offset.
// The members of one named type, in declaration order.
struct TypeMembers {
  TypeId type;
  std::vector<Member> members;
  // True for a struct, component, tag or enum: its fields are stored inline,
  // so one of them being the type itself would have no size. A class holds a
  // reference, so it may contain itself.
  bool is_value = false;
  // A trait: a type nothing is an instance of directly. A value of one is
  // a pair -- what it points at and which class that is -- so that a call
  // on it can find the body (D86).
  bool is_trait = false;
  // The declaration's own type parameters, in order. Needed to work out what
  // a field of `Holder<int>` is: zip these with the instance's arguments.
  std::vector<TypeId> parameters;

  friend bool operator==(const TypeMembers& lhs, const TypeMembers& rhs) {
    return lhs.type == rhs.type && lhs.members == rhs.members &&
           lhs.is_value == rhs.is_value && lhs.is_trait == rhs.is_trait &&
           lhs.parameters == rhs.parameters;
  }
};

// What a named type is declared to be: the type it derives from, and the
// traits it implements. Conformance is declared rather than inferred from
// the methods present, because `extend T as Trait` is the declaration.
struct TypeRelations {
  TypeId type;
  // From `class Turret : Actor`.
  TypeId base;
  // From every `extend T as Trait` anywhere in the program.
  std::vector<TypeId> traits;

  friend bool operator==(const TypeRelations& lhs, const TypeRelations& rhs) {
    return lhs.type == rhs.type && lhs.base == rhs.base &&
           lhs.traits == rhs.traits;
  }
};

// The `uses` row of one declaration, identified the way everything that
// crosses a file boundary has to be.
struct DeclaredEffects {
  uint32_t file = 0;
  uint32_t item = 0;
  EffectRow row;

  friend bool operator==(const DeclaredEffects& lhs,
                         const DeclaredEffects& rhs) {
    return lhs.file == rhs.file && lhs.item == rhs.item &&
           lhs.row == rhs.row;
  }
};

struct FileResolution {
  // Every name visible at file scope: this file's declarations, the other
  // files of its module, and whatever its imports selected.
  std::vector<Binding> scope;
  // The type each declaration of this file introduces, indexed by item tree
  // index. Invalid for declarations that are not types.
  std::vector<TypeId> item_types;
  // What each dimension this file can see stands for: a base dimension is
  // itself, and a derived one is the product it was declared as. Keyed by
  // dimension id, which is global, so a product survives being imported.
  std::vector<std::pair<DimensionId, Dimensions>> dimension_products;
  // The members of every named type this file can see: the ones it declares,
  // plus those its module siblings and imports brought in. Kept here rather
  // than in the type store so that changing a field's type changes this
  // value, which is what invalidates the files that read the field.
  std::vector<TypeMembers> type_members;
  // The base type and implemented traits of every named type this file can
  // see, merged the same way member tables are.
  std::vector<TypeRelations> relations;
  // The effect row written after `uses` on a declaration. A declaration with
  // no `uses` has no entry, which is not the same as an empty row: one says
  // "not stated", the other says "nothing observable".
  //
  // Keyed by file as well as item, because an item index only means anything
  // within the file it came from.
  std::vector<DeclaredEffects> declared_effects;
  // Aliases whose target never settled, because the chain comes back to the
  // alias itself. Recorded rather than reported here: the signature pass is
  // silent by design, and the checker is the one reporter.
  std::vector<uint32_t> cyclic_aliases;
  // Effects declared in this file, so that `uses Telemetry` resolves.
  std::vector<std::pair<Symbol, EffectId>> effect_names;

  // The store the types in this resolution came from, so that the members
  // of `Box<int>` can be worked out from those of `Box`. Not owned, not
  // compared: it is the one store the whole compilation shares.
  TypeStore* types = nullptr;
  // Member tables built for instances of generic types, cached because a
  // span has to point at something that outlives the call.
  mutable std::vector<TypeMembers> instances;

  const Binding* Find(Symbol name) const;

  // True when |type| stores its members inline. False for a class, a type
  // from a file this one cannot see, or anything that is not a named type.
  bool IsValueType(TypeId type) const;

  // True when |type| is a `trait`.
  bool IsTrait(TypeId type) const;

  // The traits |type| implements, from every `extend T as Trait` the
  // merge saw.
  std::span<const TypeId> TraitsOf(TypeId type) const;

  // The members of |type|, or an empty span when none are known -- which is
  // the case for a type declared in a file this one cannot see.
  std::span<const Member> MembersOf(TypeId type) const;

  // The member of |type| named |name|, or null.
  const Member* FindMember(TypeId type, Symbol name) const;

  // The type parameters of |type|'s declaration, in order.
  std::span<const TypeId> ParametersOf(TypeId type) const;

  // The type |type| derives from, or an invalid id.
  TypeId BaseOf(TypeId type) const;

  // The member of |type|'s base chain named |name|, or null. Does not look
  // at |type|'s own members.
  const Member* FindInheritedMember(TypeId type, Symbol name) const;

  // The row written after `uses` on a declaration, or null when none was.
  const EffectRow* DeclaredRow(uint32_t file, uint32_t item) const;

  // The effect named |name|, built-in or declared in a file this one can
  // see, or an invalid id.
  EffectId FindEffect(Symbol name, const StringInterner& interner) const;

  // True when |type| may be used where |bound| is required: it is that type,
  // derives from it, or implements it as a trait.
  bool Satisfies(TypeId type, TypeId bound) const;

  // The product a dimension stands for, or the empty product.
  Dimensions ProductOf(DimensionId dimension) const;

  friend bool operator==(const FileResolution& lhs,
                         const FileResolution& rhs) {
    return lhs.scope == rhs.scope && lhs.item_types == rhs.item_types &&
           lhs.dimension_products == rhs.dimension_products &&
           lhs.type_members == rhs.type_members &&
           lhs.relations == rhs.relations &&
           lhs.declared_effects == rhs.declared_effects &&
           lhs.effect_names == rhs.effect_names &&
           lhs.cyclic_aliases == rhs.cyclic_aliases;
  }
};

// Resolves a type expression written in source to a type.
//
// Names come from |scope|; anything unresolved is reported through
// |diagnostics| and becomes the error type, which then propagates silently so
// one unknown name does not produce a dozen follow-up messages.
class TypeResolver {
 public:
  TypeResolver(TypeStore* types,
               const StringInterner* interner,
               const FileResolution* resolution,
               DiagnosticEngine* diagnostics);

  // Resolves one `TypeRef` node of |ast|.
  TypeId Resolve(const Ast& ast, TypeRefId id);

  // Type parameters of the declaration being resolved, pushed and popped
  // around a generic declaration's signature and body.
  void PushTypeParameter(Symbol name, TypeId type);
  void PopTypeParameters(size_t count);

  // Collects every file-scope name a type annotation referred to, so that an
  // import nothing names can be reported. Optional: nothing is recorded
  // until a sink is installed.
  void CollectUsedNames(std::vector<Symbol>* sink) { used_ = sink; }

 private:
  TypeId ResolvePath(const Ast& ast, const TypeRef& type);
  TypeId LookupBuiltin(Symbol name) const;

  TypeStore* types_;
  const StringInterner* interner_;
  const FileResolution* resolution_;
  DiagnosticEngine* diagnostics_;
  std::vector<std::pair<Symbol, TypeId>> type_parameters_;
  std::vector<Symbol>* used_ = nullptr;
  std::unordered_map<uint32_t, TypeId> builtins_;
};

// Builds the scope and the declaration types of one file.
// Works out the type of every signature, field and member of |ast|, and the
// member table of every type it declares. Separate from ResolveFile because a
// field's type may name an imported type, so this needs the file's whole
// scope -- while ResolveFile must not, or two files of one module would each
// wait on the other.
//
// Reports nothing: an annotation that does not resolve becomes an error type
// here and is reported once, by the type checker.
void ResolveSignatures(TypeStore* types,
                       StringInterner* interner,
                       const Ast& ast,
                       uint32_t file_index,
                       FileResolution* resolution,
                       DiagnosticEngine* diagnostics);

FileResolution ResolveFile(TypeStore* types,
                           StringInterner* interner,
                           const Ast& ast,
                           uint32_t file_index,
                           DiagnosticEngine* diagnostics);

}  // namespace wolang

#endif  // WOLANG_SEMA_RESOLUTION_H_
