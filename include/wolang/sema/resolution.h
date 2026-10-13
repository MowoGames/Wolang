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

  friend bool operator==(const Binding& lhs, const Binding& rhs) {
    return lhs.name == rhs.name && lhs.kind == rhs.kind &&
           lhs.type == rhs.type && lhs.item == rhs.item &&
           lhs.file == rhs.file && lhs.dimension == rhs.dimension &&
           lhs.factor == rhs.factor && lhs.is_mutable == rhs.is_mutable &&
           lhs.is_public == rhs.is_public;
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

  friend bool operator==(const TypeMembers& lhs, const TypeMembers& rhs) {
    return lhs.type == rhs.type && lhs.members == rhs.members;
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

  const Binding* Find(Symbol name) const;

  // The members of |type|, or an empty span when none are known -- which is
  // the case for a type declared in a file this one cannot see.
  std::span<const Member> MembersOf(TypeId type) const;

  // The member of |type| named |name|, or null.
  const Member* FindMember(TypeId type, Symbol name) const;

  // The product a dimension stands for, or the empty product.
  Dimensions ProductOf(DimensionId dimension) const;

  friend bool operator==(const FileResolution& lhs,
                         const FileResolution& rhs) {
    return lhs.scope == rhs.scope && lhs.item_types == rhs.item_types &&
           lhs.dimension_products == rhs.dimension_products &&
           lhs.type_members == rhs.type_members;
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

 private:
  TypeId ResolvePath(const Ast& ast, const TypeRef& type);
  TypeId LookupBuiltin(Symbol name) const;

  TypeStore* types_;
  const StringInterner* interner_;
  const FileResolution* resolution_;
  DiagnosticEngine* diagnostics_;
  std::vector<std::pair<Symbol, TypeId>> type_parameters_;
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
                       const StringInterner* interner,
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
