// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/sema/resolution.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "wolang/compiler/item_tree.h"

namespace wolang {
namespace {

constexpr std::string_view kUnknownType = "E0110";
constexpr std::string_view kNotAType = "E0111";
constexpr std::string_view kDuplicateName = "E0113";
constexpr std::string_view kUnknownDimension = "E0114";
constexpr std::string_view kBadUnitFactor = "E0115";
constexpr std::string_view kBadDimensionProduct = "E0116";
constexpr std::string_view kCyclicDimension = "E0118";

// The names the language provides without any declaration. Spelled out here
// rather than injected as a synthetic module, because they have no
// declaration to point at and no module to belong to.
struct BuiltinType {
  std::string_view name;
  TypeKind kind;
  IntWidth int_width;
  bool is_signed;
  FloatWidth float_width;
};

constexpr BuiltinType kBuiltins[] = {
    {"bool", TypeKind::kBool, IntWidth::k32, true, FloatWidth::k32},
    {"char", TypeKind::kChar, IntWidth::k32, true, FloatWidth::k32},
    // `text` is a view of bytes and `Text` owns them. Lowercase for what
    // the machine has built in, PascalCase for a type that owns something,
    // which is the same rule every declared type follows.
    {"Text", TypeKind::kString, IntWidth::k32, true, FloatWidth::k32},
    {"text", TypeKind::kStr, IntWidth::k32, true, FloatWidth::k32},
    {"never", TypeKind::kNever, IntWidth::k32, true, FloatWidth::k32},
    // `int` and `float` are the widths gameplay code should reach for when it
    // does not care, so they are aliases rather than separate types.
    {"int", TypeKind::kInt, IntWidth::k32, true, FloatWidth::k32},
    // `s` for signed, not `i` for integer: both halves of the table are
    // integers, and only one of them is signed.
    {"s8", TypeKind::kInt, IntWidth::k8, true, FloatWidth::k32},
    {"s16", TypeKind::kInt, IntWidth::k16, true, FloatWidth::k32},
    {"s32", TypeKind::kInt, IntWidth::k32, true, FloatWidth::k32},
    {"s64", TypeKind::kInt, IntWidth::k64, true, FloatWidth::k32},
    {"u8", TypeKind::kInt, IntWidth::k8, false, FloatWidth::k32},
    {"u16", TypeKind::kInt, IntWidth::k16, false, FloatWidth::k32},
    {"u32", TypeKind::kInt, IntWidth::k32, false, FloatWidth::k32},
    {"u64", TypeKind::kInt, IntWidth::k64, false, FloatWidth::k32},
    {"float", TypeKind::kFloat, IntWidth::k32, true, FloatWidth::k32},
    {"f32", TypeKind::kFloat, IntWidth::k32, true, FloatWidth::k32},
    {"f64", TypeKind::kFloat, IntWidth::k32, true, FloatWidth::k64},
};

// Declarations that introduce a type name.
bool DeclaresAType(ItemKind kind) {
  switch (kind) {
    case ItemKind::kClass:
    case ItemKind::kStruct:
    case ItemKind::kEnum:
    case ItemKind::kAbility:
    case ItemKind::kComponent:
    case ItemKind::kTag:
    case ItemKind::kTypeAlias:
    // A machine is a type whose value is which state it is in. Everything
    // else about it -- the events, the states, the blocks that run on the
    // way in and out -- is members of that type.
    case ItemKind::kMachine:
      return true;
    default:
      return false;
  }
}

BindingKind BindingKindOf(ItemKind kind) {
  switch (kind) {
    case ItemKind::kFunction:
      return BindingKind::kFunction;
    case ItemKind::kBinding:
      return BindingKind::kValue;
    case ItemKind::kDimension:
      return BindingKind::kDimension;
    case ItemKind::kUnit:
      return BindingKind::kUnit;
    case ItemKind::kEffect:
      return BindingKind::kEffect;
    case ItemKind::kSignal:
      return BindingKind::kSignal;
    case ItemKind::kSystem:
      return BindingKind::kSystem;
    case ItemKind::kMachine:
      // A machine names a type: a value of it is which state it is in.
      // The kind exists for the places that care about the difference,
      // and nothing stops it being a type where a type is wanted.
      return BindingKind::kType;
    default:
      return BindingKind::kType;
  }
}

}  // namespace

Dimensions FileResolution::ProductOf(DimensionId dimension) const {
  for (const auto& [id, product] : dimension_products) {
    if (id == dimension) {
      return product;
    }
  }
  return {};
}

bool FileResolution::IsValueType(TypeId type) const {
  for (const TypeMembers& table : type_members) {
    if (table.type == type) {
      return table.is_value;
    }
  }
  return false;
}

std::span<const TypeId> FileResolution::ParametersOf(TypeId type) const {
  for (const TypeMembers& table : type_members) {
    if (table.type == type) {
      return table.parameters;
    }
  }
  return {};
}

std::span<const Member> FileResolution::MembersOf(TypeId type) const {
  for (const TypeMembers& table : type_members) {
    if (table.type == type) {
      return table.members;
    }
  }

  // `Box<int>` keeps its members on `Box`, with the instance's arguments
  // standing in for the declaration's parameters. Built here rather than
  // at every reader, because there are a dozen of them -- a layout, a
  // lowering, an interpreter -- and each one getting it right separately
  // is a dozen chances to get it wrong.
  if (types == nullptr || types->KindOf(type) != TypeKind::kInstance) {
    return {};
  }
  for (const TypeMembers& table : instances) {
    if (table.type == type) {
      return table.members;
    }
  }
  const TypeId base = types->Get(type).base;
  const TypeMembers* declared = nullptr;
  for (const TypeMembers& table : type_members) {
    if (table.type == base) {
      declared = &table;
      break;
    }
  }
  if (declared == nullptr) {
    return {};
  }
  const std::vector<TypeId> arguments = types->ArgumentsOf(type);
  TypeStore::Substitution substitution;
  for (size_t i = 0; i < declared->parameters.size() && i < arguments.size();
       ++i) {
    substitution.emplace_back(declared->parameters[i], arguments[i]);
  }

  TypeMembers built;
  built.type = type;
  built.is_value = declared->is_value;
  built.is_ability = declared->is_ability;
  built.parameters = declared->parameters;
  built.members = declared->members;
  for (Member& member : built.members) {
    member.type = types->Substitute(member.type, substitution);
  }
  instances.push_back(std::move(built));
  return instances.back().members;
}

const Member* FileResolution::FindMember(TypeId type, Symbol name) const {
  for (const Member& member : MembersOf(type)) {
    if (member.name == name) {
      return &member;
    }
  }
  return nullptr;
}

const EffectRow* FileResolution::DeclaredRow(uint32_t file,
                                             uint32_t item) const {
  for (const DeclaredEffects& entry : declared_effects) {
    if (entry.file == file && entry.item == item) {
      return &entry.row;
    }
  }
  return nullptr;
}

EffectId FileResolution::FindEffect(Symbol name,
                                    const StringInterner& interner) const {
  // A declared effect wins, so a project can shadow nothing by accident:
  // `effect io` in one's own file is a different effect than the built-in.
  for (const auto& entry : effect_names) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  const BuiltinEffect builtin = LookupBuiltinEffect(interner.Text(name));
  if (builtin != BuiltinEffect::kCount) {
    return EffectId{static_cast<uint32_t>(builtin)};
  }
  return EffectId{};
}

std::span<const TypeId> FileResolution::AbilitiesOf(TypeId type) const {
  for (const TypeRelations& entry : relations) {
    if (entry.type == type) {
      return entry.abilities;
    }
  }
  return {};
}

bool FileResolution::IsAbility(TypeId type) const {
  for (const TypeMembers& table : type_members) {
    if (table.type == type) {
      return table.is_ability;
    }
  }
  return false;
}

TypeId FileResolution::BaseOf(TypeId type) const {
  for (const TypeRelations& relation : relations) {
    if (relation.type == type) {
      return relation.base;
    }
  }
  return TypeId{};
}

const Member* FileResolution::FindInheritedMember(TypeId type,
                                                  Symbol name) const {
  // Bounded by the number of relations, so a cycle in the base chain -- which
  // is a declaration error, not a crash -- terminates here too.
  TypeId current = BaseOf(type);
  for (size_t depth = 0; depth <= relations.size() && current.is_valid();
       ++depth) {
    if (const Member* member = FindMember(current, name); member != nullptr) {
      return member;
    }
    current = BaseOf(current);
  }
  return nullptr;
}

bool FileResolution::Satisfies(TypeId type, TypeId bound) const {
  if (!type.is_valid() || !bound.is_valid()) {
    return true;
  }

  // `ListWalk<int>` satisfies what `extend ListWalk<T> as ...` declared.
  if (types != nullptr && types->KindOf(type) == TypeKind::kInstance && Satisfies(types->Get(type).base, bound)) {
    return true;
  }

  TypeId current = type;
  for (size_t depth = 0; depth <= relations.size() && current.is_valid();
       ++depth) {
    if (current == bound) {
      return true;
    }
    for (const TypeRelations& relation : relations) {
      if (relation.type != current) {
        continue;
      }
      for (TypeId ability : relation.abilities) {
        if (ability == bound) {
          return true;
        }
      }
      break;
    }
    current = BaseOf(current);
  }
  return false;
}

const Binding* FileResolution::Find(Symbol name) const {
  for (const Binding& binding : scope) {
    if (binding.name == name) {
      return &binding;
    }
  }
  // A qualified spelling made up by the resolver (D246).
  for (const Qualified& entry : qualified) {
    if (entry.binding.name == name) {
      return &entry.binding;
    }
  }
  return nullptr;
}

const Binding* FileResolution::FindQualified(Symbol qualifier,
                                             Symbol member) const {
  for (const Qualified& entry : qualified) {
    if (entry.qualifier == qualifier && entry.binding.real_name == member) {
      return &entry.binding;
    }
  }
  return nullptr;
}

bool FileResolution::IsQualifier(Symbol name) const {
  return std::find(qualifiers.begin(), qualifiers.end(), name) !=
         qualifiers.end();
}

bool FileResolution::IsAmbiguous(Symbol name) const {
  return std::find(ambiguous.begin(), ambiguous.end(), name) !=
         ambiguous.end();
}

namespace {

// The members a `machine` has.
//
// `state` is where it is; `Is<State>()` asks; `<Event>()` tries the
// transition the current state declares for that event and says whether
// one fired. The states themselves are members too, so that a diagnostic
// can name them and so that nothing else has to parse the machine again.
void MachineMembers(const Ast& ast,
                    const ItemTree& /*tree*/,
                    StringInterner* interner,
                    TypeStore* types,
                    uint32_t machine,
                    uint32_t file_index,
                    std::vector<Member>* members) {
  const ItemId id = ResolveItem(ast, machine);
  if (!ast.Contains(id)) {
    return;
  }

  Member current;
  current.name = Symbol{};
  current.kind = MemberKind::kMachineState;
  current.type = types->Int(IntWidth::k32, /*is_signed=*/true);
  current.file = file_index;
  current.item = machine;
  current.is_mutable = true;
  current.is_public = false;
  members->push_back(std::move(current));

  // Every event mentioned by any state, once: the method is the event, and
  // which state is in is what decides what it does.
  std::vector<Symbol> events;
  for (StateId state_id : ast.Get(ast.Get(id).states)) {
    if (!ast.Contains(state_id)) {
      continue;
    }
    const State& state = ast.Get(state_id);
    for (TransitionId transition_id : ast.Get(state.transitions)) {
      if (!ast.Contains(transition_id)) {
        continue;
      }
      const Transition& transition = ast.Get(transition_id);
      if (!transition.event.is_valid()) {
        continue;  // `always`, which fires on the way in.
      }
      if (std::find(events.begin(), events.end(), transition.event) ==
          events.end()) {
        events.push_back(transition.event);
      }
    }
  }

  // Each generated member gets a slot of its own: the machine is one
  // declaration and these are many functions, so the item alone cannot
  // tell them apart.
  uint32_t slot = 1;
  for (Symbol event : events) {
    Member method;
    method.name = event;
    method.kind = MemberKind::kMethod;
    method.type = types->Function({}, types->bool_type());
    method.file = file_index;
    method.item = machine;
    method.slot = slot++;
    method.is_public = true;
    members->push_back(std::move(method));
  }

  for (StateId state_id : ast.Get(ast.Get(id).states)) {
    if (!ast.Contains(state_id)) {
      continue;
    }
    const State& state = ast.Get(state_id);
    if (!state.name.is_valid()) {
      continue;
    }
    Member marker;
    marker.name = state.name;
    marker.kind = MemberKind::kState;
    marker.type = types->Int(IntWidth::k32, /*is_signed=*/true);
    marker.file = file_index;
    marker.item = machine;
    marker.is_public = true;
    members->push_back(std::move(marker));

    Member predicate;
    predicate.name = interner->Intern(
        "Is" + std::string(interner->Text(state.name)));
    predicate.kind = MemberKind::kMethod;
    predicate.type = types->Function({}, types->bool_type());
    predicate.file = file_index;
    predicate.item = machine;
    predicate.slot = slot++;
    predicate.is_public = true;
    members->push_back(std::move(predicate));
  }
}

// Which of |members| the expression of the derived field declared at
// |item| reads, as indices into that list.
//
// A bare name or `self.name` that matches a field counts; a call counts as
// reading everything, because what it reads is not something this walk can
// see. Conservative in the direction that recomputes too often rather than
// too rarely -- a stale value is a wrong answer and an extra computation
// is not.
std::vector<uint32_t> DerivedDependencies(const Ast& ast,
                                          const ItemTree& tree,
                                          const std::vector<Member>& members,
                                          uint32_t item) {
  std::vector<uint32_t> depends;
  const ItemId id = ResolveItem(ast, item);
  if (!ast.Contains(id)) {
    return depends;
  }
  const ExprId root = ast.Get(id).init;
  if (!ast.Contains(root)) {
    return depends;
  }

  const auto add = [&members, &depends](Symbol name) {
    for (uint32_t i = 0; i < members.size(); ++i) {
      if (members[i].name != name) {
        continue;
      }
      if (members[i].kind != MemberKind::kField &&
          members[i].kind != MemberKind::kDerived) {
        continue;
      }
      if (std::find(depends.begin(), depends.end(), i) == depends.end()) {
        depends.push_back(i);
      }
      return;
    }
  };
  const auto everything = [&members, &depends]() {
    depends.clear();
    for (uint32_t i = 0; i < members.size(); ++i) {
      if (members[i].kind == MemberKind::kField ||
          members[i].kind == MemberKind::kDerived) {
        depends.push_back(i);
      }
    }
  };

  std::vector<ExprId> work{root};
  bool opaque = false;
  while (!work.empty() && !opaque) {
    const ExprId current = work.back();
    work.pop_back();
    if (!ast.Contains(current)) {
      continue;
    }
    const Expr& expr = ast.Get(current);
    switch (expr.kind) {
      case ExprKind::kPath:
        add(expr.name);
        break;
      case ExprKind::kField:
      case ExprKind::kOptionalField:
        add(expr.name);
        break;
      case ExprKind::kCall:
      case ExprKind::kAwait:
      case ExprKind::kSpawn:
        // A call may read anything of its own.
        opaque = true;
        break;
      default:
        break;
    }
    for (ExprId child : {expr.lhs, expr.rhs, expr.extra}) {
      if (child.is_valid()) {
        work.push_back(child);
      }
    }
    for (ExprId argument : ast.Get(expr.args)) {
      work.push_back(argument);
    }
  }
  if (opaque) {
    everything();
  }
  (void)tree;
  return depends;
}

bool EvaluateDimensionProduct(const Ast& ast,
                              const FileResolution& resolution,
                              const StringInterner& interner,
                              ExprId id,
                              DiagnosticEngine* diagnostics,
                              Dimensions* out) {
  if (!ast.Contains(id)) {
    return false;
  }
  const Expr& expr = ast.Get(id);

  switch (expr.kind) {
    case ExprKind::kPath: {
      const Binding* binding = resolution.Find(expr.name);
      if (binding == nullptr ||
          binding->kind != BindingKind::kDimension) {
        std::string message = "no dimension named `";
        message.append(interner.Text(expr.name));
        message.append("`");
        diagnostics->Error(kUnknownDimension, std::move(message))
            .Primary(expr.span);
        return false;
      }
      *out = resolution.ProductOf(binding->dimension);
      return true;
    }

    case ExprKind::kIntLiteral:
      // A bare `1` is the dimensionless product, so `1/Time` is writable.
      if (ast.number(expr.aux).integer_value == 1) {
        *out = Dimensions();
        return true;
      }
      break;

    case ExprKind::kBinary: {
      Dimensions left;
      Dimensions right;
      if (expr.op == TokenKind::kStar || expr.op == TokenKind::kSlash) {
        if (!EvaluateDimensionProduct(ast, resolution, interner, expr.lhs,
                                      diagnostics, &left) ||
            !EvaluateDimensionProduct(ast, resolution, interner, expr.rhs,
                                      diagnostics, &right)) {
          return false;
        }
        *out = expr.op == TokenKind::kStar ? left.Multiply(right)
                                           : left.Divide(right);
        return true;
      }
      if (expr.op == TokenKind::kCaret) {
        if (!EvaluateDimensionProduct(ast, resolution, interner, expr.lhs,
                                      diagnostics, &left)) {
          return false;
        }
        if (!ast.Contains(expr.rhs) ||
            ast.Get(expr.rhs).kind != ExprKind::kIntLiteral) {
          diagnostics->Error(kBadDimensionProduct,
                             "an exponent must be an integer literal")
              .Primary(expr.span);
          return false;
        }
        const uint64_t exponent =
            ast.number(ast.Get(expr.rhs).aux).integer_value;
        *out = left.Power(static_cast<int32_t>(exponent));
        return true;
      }
      break;
    }

    default:
      break;
  }

  diagnostics->Error(kBadDimensionProduct,
                     "a dimension is a product of dimension names")
      .Primary(expr.span)
      .Note("write `dimension Velocity = Length / Time`");
  return false;
}

}  // namespace

// ------------------------------------------------------------- resolver -----

TypeResolver::TypeResolver(TypeStore* types,
                           const StringInterner* interner,
                           const FileResolution* resolution,
                           DiagnosticEngine* diagnostics)
    : types_(types),
      interner_(interner),
      resolution_(resolution),
      diagnostics_(diagnostics) {
  // Builtin names are looked up by symbol, so they are interned once here
  // rather than compared as strings on every reference.
  for (const BuiltinType& builtin : kBuiltins) {
    const Symbol name = interner_->Lookup(builtin.name);
    if (!name.is_valid()) {
      // The name never appears in this compilation, so nothing can refer to
      // it and there is nothing to register.
      continue;
    }
    TypeId type;
    switch (builtin.kind) {
      case TypeKind::kBool:
        type = types_->bool_type();
        break;
      case TypeKind::kChar:
        type = types_->char_type();
        break;
      case TypeKind::kString:
        type = types_->string_type();
        break;
      case TypeKind::kStr:
        type = types_->str();
        break;
      case TypeKind::kNever:
        type = types_->never();
        break;
      case TypeKind::kInt:
        type = types_->Int(builtin.int_width, builtin.is_signed);
        break;
      case TypeKind::kFloat:
        type = types_->Float(builtin.float_width);
        break;
      default:
        continue;
    }
    builtins_.emplace(name.index, type);
  }
}

void TypeResolver::PushTypeParameter(Symbol name, TypeId type) {
  type_parameters_.emplace_back(name, type);
}

void TypeResolver::PopTypeParameters(size_t count) {
  const size_t keep =
      count >= type_parameters_.size() ? 0 : type_parameters_.size() - count;
  type_parameters_.resize(keep);
}

TypeId TypeResolver::LookupBuiltin(Symbol name) const {
  const auto it = builtins_.find(name.index);
  return it == builtins_.end() ? TypeId{} : it->second;
}

TypeId TypeResolver::ResolvePath(const Ast& ast, const TypeRef& type) {
  const std::span<const Symbol> path = ast.Get(type.path);
  if (path.empty()) {
    return types_->error();
  }
  // `C.Walker.Item` where `C` is a type parameter: each segment after the
  // first is an associated type of what came before (D212).
  if (path.size() > 1) {
    for (auto it = type_parameters_.rbegin(); it != type_parameters_.rend(); ++it) {
      if (it->first != path.front()) {
        continue;
      }
      TypeId projected = it->second;
      for (size_t i = 1; i < path.size(); ++i) {
        projected = types_->Projection(projected, path[i]);
      }
      return projected;
    }
  }

  // `json.Value` names the type the module `json` offers, even when another
  // module offers a `Value` too (D246).
  if (path.size() == 2 && resolution_ != nullptr) {
    if (const Binding* qualified =
            resolution_->FindQualified(path.front(), path.back());
        qualified != nullptr && qualified->kind == BindingKind::kType) {
      if (used_ != nullptr) {
        used_->push_back(path.back());
      }
      return qualified->type;
    }
  }

  // Only the last segment names the type; earlier segments are the module or
  // the enclosing type, which module resolution already checked.
  const Symbol name = path.back();

  // Type parameters shadow everything, since they are the innermost scope.
  for (auto it = type_parameters_.rbegin(); it != type_parameters_.rend();
       ++it) {
    if (it->first == name) {
      return it->second;
    }
  }

  if (const TypeId builtin = LookupBuiltin(name); builtin.is_valid()) {
    return builtin;
  }

  // `Query<mut Velocity, with Falling>` names what a system asks the world
  // for. Built in rather than declared, because the compiler is what reads
  // it: the access graph comes from the arguments, so there is nothing for
  // a library to put in the type.
  if (interner_->Text(name) == "Query") {
    return types_->query();
  }

  if (used_ != nullptr) {
    used_->push_back(name);
  }

  const Binding* binding =
      resolution_ == nullptr ? nullptr : resolution_->Find(name);
  if (binding == nullptr) {
    std::string message = "no type named `";
    message.append(interner_->Text(name));
    message.append("` is in scope");
    diagnostics_->Error(kUnknownType, std::move(message))
        .Primary(type.span);
    return types_->error();
  }
  // A dimension name is a type: the type of quantities carrying it. That is
  // what makes `distance: Length` and `speed: Velocity` writable, with the
  // derived dimension declared once rather than spelled out at every use.
  if (binding->kind == BindingKind::kDimension) {
    return types_->Quantity(types_->default_float(),
                            resolution_->ProductOf(binding->dimension));
  }

  if (binding->kind != BindingKind::kType &&
      binding->kind != BindingKind::kTypeParam) {
    std::string message = "`";
    message.append(interner_->Text(name));
    message.append("` is not a type");
    diagnostics_->Error(kNotAType, std::move(message)).Primary(type.span);
    return types_->error();
  }
  return binding->type;
}

TypeId TypeResolver::Resolve(const Ast& ast, TypeRefId id) {
  if (!ast.Contains(id)) {
    return types_->error();
  }
  const TypeRef& type = ast.Get(id);
  const bool is_soa = HasFlag(type.flags, TypeRefFlags::kSoa);

  switch (type.kind) {
    case TypeRefKind::kError:
      return types_->error();

    case TypeRefKind::kPath: {
      const TypeId base = ResolvePath(ast, type);
      if (type.args.empty()) {
        return base;
      }
      std::vector<TypeId> args;
      // Whether the component the loop binds is written. The modifiers
      // themselves stay in the syntax, where the access graph reads them;
      // this one travels in the type because `foreach v in q` has to know
      // whether `v` may be assigned to.
      bool writes = false;
      bool first = true;
      for (TypeRefId arg : ast.Get(type.args)) {
        if (first && ast.Contains(arg)) {
          writes = ast.Get(arg).kind == TypeRefKind::kMut;
          first = false;
        }
        args.push_back(Resolve(ast, arg));
      }
      if (base == types_->query()) {
        return types_->Query(args, writes);
      }
      return types_->Instance(base, args);
    }

    case TypeRefKind::kOptional:
      return types_->Optional(Resolve(ast, type.element));

    case TypeRefKind::kFallible:
      // The error set is inferred from the body, so the declared type starts
      // with an open set and type checking fills it in.
      return types_->Fallible(Resolve(ast, type.element),
                              types_->ErrorSet({}));

    case TypeRefKind::kSlice:
      return types_->Slice(Resolve(ast, type.element), is_soa);

    case TypeRefKind::kArray: {
      const TypeId element = Resolve(ast, type.element);
      uint64_t length = 0;
      // Only a literal length is understood for now; a `comptime` expression
      // becomes possible in wave 7.
      if (ast.Contains(type.length)) {
        const Expr& expr = ast.Get(type.length);
        if (expr.kind == ExprKind::kIntLiteral) {
          length = ast.number(expr.aux).integer_value;
        }
      }
      return types_->Array(element, length, is_soa);
    }

    case TypeRefKind::kTuple: {
      std::vector<TypeId> elements;
      for (TypeRefId element : ast.Get(type.args)) {
        elements.push_back(Resolve(ast, element));
      }
      return types_->Tuple(elements);
    }

    case TypeRefKind::kFunction: {
      std::vector<TypeId> params;
      for (TypeRefId param : ast.Get(type.args)) {
        params.push_back(Resolve(ast, param));
      }
      const TypeId result = type.element.is_valid()
                                ? Resolve(ast, type.element)
                                : types_->void_type();
      return types_->Function(params, result);
    }

    case TypeRefKind::kPointer:
      return types_->Pointer(Resolve(ast, type.element),
                             HasFlag(type.flags, TypeRefFlags::kMutable));

    case TypeRefKind::kReference:
      return types_->Reference(Resolve(ast, type.element),
                               HasFlag(type.flags, TypeRefFlags::kMutable));

    case TypeRefKind::kMut:
      // `mut T` in a query is a write, and what the loop over it binds is
      // something assignable -- so the argument resolves to a mutable
      // reference to the component. The access graph still reads the
      // syntax, because `with` and `without` are reads of a *fact* rather
      // than of the component (D101).
      return types_->Reference(Resolve(ast, type.element),
                               /*is_mutable=*/true);

    case TypeRefKind::kWith:
    case TypeRefKind::kWithout:
      // A filter: whether an entity has the component, which is reading
      // the fact rather than the value.
      return Resolve(ast, type.element);
  }
  return types_->error();
}

// ------------------------------------------------------------ resolution ----

FileResolution ResolveFile(TypeStore* types,
                           StringInterner* interner,
                           const Ast& ast,
                           uint32_t file_index,
                           DiagnosticEngine* diagnostics) {
  FileResolution resolution;
  resolution.types = types;
  const ItemTree tree = BuildItemTree(*interner, ast);
  resolution.item_types.assign(tree.entries.size(), TypeId{});

  // First pass: give every type declaration its type, so that a declaration
  // can refer to one that appears later in the file.
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    const ItemTreeEntry& entry = tree.entries[index];
    if (entry.parent != ItemTreeEntry::kNoParent || !entry.name.is_valid()) {
      continue;
    }
    if (DeclaresAType(entry.kind)) {
      resolution.item_types[index] = types->Named(
          entry.name, file_index, index,
          // A machine is a handle for the same reason a class is: an
          // event changes which state it is in, and a method that changes
          // its object has to be given the object rather than a copy.
          /*is_handle=*/entry.kind == ItemKind::kClass ||
              entry.kind == ItemKind::kMachine,
          /*is_foreign=*/entry.kind == ItemKind::kClass && HasFlag(entry.flags, ItemFlags::kExtern));
    }
  }

  // Second pass: bind every module-level name, and collect dimensions.
  std::vector<uint32_t> derived_dimensions;
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    const ItemTreeEntry& entry = tree.entries[index];
    if (entry.parent != ItemTreeEntry::kNoParent) {
      continue;
    }

    const ItemId item_id = ResolveItem(ast, index);
    const Item* item = ast.Contains(item_id) ? &ast.Get(item_id) : nullptr;

    // A `dimension` or `error` declaration binds several names at once.
    if (entry.kind == ItemKind::kDimension && item != nullptr) {
      for (Symbol name : ast.Get(item->names)) {
        // Interned by declaration, so `Length` is one dimension however many
        // files name it.
        const DimensionId dimension = types->Dimension(name, file_index, index);
        // Replaced below for a derived dimension; a base one stands for
        // itself.
        resolution.dimension_products.emplace_back(dimension,
                                                   Dimensions::Base(dimension));

        Binding binding;
        binding.name = name;
        binding.kind = BindingKind::kDimension;
        binding.item = index;
        binding.file = file_index;
        binding.dimension = dimension;
        binding.is_public = HasFlag(entry.flags, ItemFlags::kOffer);
        resolution.scope.push_back(binding);
      }
      if (item->init.is_valid()) {
        derived_dimensions.push_back(index);
      }
      continue;
    }
    if (entry.kind == ItemKind::kEffect && item != nullptr) {
      // Interned by declaration, so one `effect` is one effect however many
      // files name it -- and so a project's own `effect io` is not the
      // built-in one.
      resolution.effect_names.emplace_back(
          entry.name, types->Effect(entry.name, file_index, index));
      // Falls through: an effect also binds its name like any declaration.
    }
    if (entry.kind == ItemKind::kErrorSet && item != nullptr) {
      for (Symbol name : ast.Get(item->names)) {
        Binding binding;
        binding.name = name;
        binding.kind = BindingKind::kErrorTag;
        binding.item = index;
        binding.file = file_index;
        binding.is_public = HasFlag(entry.flags, ItemFlags::kOffer);
        resolution.scope.push_back(binding);
      }
      continue;
    }

    // An `extend` block is named after the type it extends; it declares no
    // new name, so binding it would collide with the declaration itself.
    // A `need` is named after its module so that `module.Name` can be read,
    // but it declares no value: binding it made `json` a type with no type,
    // and `json.Parse` an error value nothing reported (D246).
    if (!entry.name.is_valid() || entry.kind == ItemKind::kExtend ||
        entry.kind == ItemKind::kNeed) {
      continue;
    }

    Binding binding;
    binding.name = entry.name;
    binding.kind = BindingKindOf(entry.kind);
    binding.type = resolution.item_types[index];
    binding.item = index;
    binding.file = file_index;
    binding.is_mutable = HasFlag(entry.flags, ItemFlags::kMutable);
    binding.is_public = HasFlag(entry.flags, ItemFlags::kOffer);
    binding.is_foreign = HasFlag(entry.flags, ItemFlags::kExtern);
    binding.is_task = HasFlag(entry.flags, ItemFlags::kAsync);
    binding.has_body = HasFlag(entry.flags, ItemFlags::kHasBody);
    if (item != nullptr && entry.kind == ItemKind::kFunction) {
      for (AttributeId attribute_id : ast.Get(item->attributes)) {
        const Attribute& attribute = ast.Get(attribute_id);
        if (interner->Text(attribute.name) == "Exposes") {
          binding.exposes = true;
          continue;
        }
        if (interner->Text(attribute.name) != "Thread") {
          continue;
        }
        binding.thread_args = 1;
        for (ExprId argument : ast.Get(attribute.args)) {
          if (ast.Contains(argument) &&
              interner->Text(ast.Get(argument).name) == "captures") {
            binding.thread_args = 2;
          }
        }
      }
    }
    resolution.scope.push_back(binding);
  }

  // Third pass: work out what each derived dimension stands for, now that
  // every dimension name is bound.
  for (uint32_t index : derived_dimensions) {
    const ItemId item_id = ResolveItem(ast, index);
    if (!ast.Contains(item_id)) {
      continue;
    }
    const Item& item = ast.Get(item_id);
    const Binding* binding = resolution.Find(item.name);
    if (binding == nullptr || !binding->dimension.is_valid()) {
      continue;
    }

    Dimensions product;
    if (!EvaluateDimensionProduct(ast, resolution, *interner, item.init,
                                  diagnostics, &product)) {
      continue;
    }
    // `dimension D = D`, or a pair that defines each other. Evaluation runs
    // in declaration order, so a cycle would quietly produce whichever
    // answer the order happened to give.
    if (product.ExponentOf(binding->dimension) != 0) {
      std::string message = "`";
      message.append(interner->Text(item.name));
      message.append("` is defined in terms of itself");
      diagnostics->Error(kCyclicDimension, std::move(message))
          .Primary(item.name_span);
      continue;
    }
    for (auto& [id, stored] : resolution.dimension_products) {
      if (id == binding->dimension) {
        stored = product;
        break;
      }
    }
  }

  // Fourth pass: resolve units now that their dimensions exist.
  TypeResolver resolver(types, interner, &resolution, diagnostics);
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    const ItemTreeEntry& entry = tree.entries[index];
    if (entry.kind != ItemKind::kUnit ||
        entry.parent != ItemTreeEntry::kNoParent) {
      continue;
    }
    const ItemId item_id = ResolveItem(ast, index);
    if (!ast.Contains(item_id)) {
      continue;
    }
    const Item& item = ast.Get(item_id);

    Binding* binding = nullptr;
    for (Binding& candidate : resolution.scope) {
      if (candidate.name == entry.name &&
          candidate.kind == BindingKind::kUnit) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      continue;
    }

    if (!item.path.empty()) {
      // `unit m: Length` introduces a base unit of a dimension.
      const std::span<const Symbol> path = ast.Get(item.path);
      const Symbol dimension_name = path.back();
      const Binding* dimension = resolution.Find(dimension_name);
      if (dimension == nullptr ||
          dimension->kind != BindingKind::kDimension) {
        std::string message = "no dimension named `";
        message.append(interner->Text(dimension_name));
        message.append("`");
        diagnostics->Error(kUnknownDimension, std::move(message))
            .Primary(item.name_span);
        continue;
      }
      binding->dimension = dimension->dimension;
      binding->factor = 1.0;
      continue;
    }

    // `unit cm = 0.01m` derives a unit from another one, so it inherits the
    // dimension and multiplies the factor.
    if (!ast.Contains(item.init)) {
      continue;
    }
    const Expr& init = ast.Get(item.init);
    if (init.kind != ExprKind::kFloatLiteral &&
        init.kind != ExprKind::kIntLiteral) {
      diagnostics->Error(kBadUnitFactor,
                         "a unit must convert through a numeric literal")
          .Primary(item.name_span)
          .Note("write `unit cm = 0.01m`");
      continue;
    }
    const NumberLiteral& number = ast.number(init.aux);
    if (!number.has_unit()) {
      diagnostics->Error(kBadUnitFactor,
                         "a unit conversion needs a unit on its literal")
          .Primary(item.name_span)
          .Note("write `unit cm = 0.01m`");
      continue;
    }
    const Binding* base = resolution.Find(number.unit);
    if (base == nullptr || base->kind != BindingKind::kUnit) {
      std::string message = "no unit named `";
      message.append(interner->Text(number.unit));
      message.append("`");
      diagnostics->Error(kUnknownDimension, std::move(message))
          .Primary(item.name_span);
      continue;
    }
    binding->dimension = base->dimension;
    binding->factor =
        base->factor *
        (init.kind == ExprKind::kFloatLiteral
             ? number.float_value
             : static_cast<double>(number.integer_value));
  }

  // A name bound twice at file scope is reported once, on the later
  // declaration. Module-wide duplicates are a separate check in wave 3.
  for (size_t i = 0; i < resolution.scope.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (resolution.scope[i].name != resolution.scope[j].name) {
        continue;
      }
      const ItemId item_id = ResolveItem(ast, resolution.scope[i].item);
      if (!ast.Contains(item_id)) {
        break;
      }
      std::string message = "`";
      message.append(interner->Text(resolution.scope[i].name));
      message.append("` is already declared in this file");
      diagnostics->Error(kDuplicateName, std::move(message))
          .Primary(ast.Get(item_id).name_span);
      break;
    }
  }

  return resolution;
}

void ResolveSignatures(TypeStore* types,
                       StringInterner* interner,
                       const Ast& ast,
                       uint32_t file_index,
                       FileResolution* resolution,
                       DiagnosticEngine* diagnostics) {
  const ItemTree tree = BuildItemTree(*interner, ast);
  if (resolution->item_types.size() < tree.entries.size()) {
    resolution->item_types.resize(tree.entries.size(), TypeId{});
  }
  TypeResolver resolver(types, interner, resolution, diagnostics);

  // Aliases first, and repeatedly: `type A = B` where `B` is itself an alias
  // needs B settled before A can be. An alias is transparent -- it is
  // another spelling of its target, not a new type -- so leaving it as the
  // opaque name pass one gave it would make `let c: Count = 5` an error.
  size_t alias_count = 0;
  for (const ItemTreeEntry& entry : tree.entries) {
    alias_count += entry.kind == ItemKind::kTypeAlias ? 1 : 0;
  }
  for (size_t round = 0; round <= alias_count; ++round) {
    bool changed = false;
    for (uint32_t index = 0; index < tree.entries.size(); ++index) {
      if (tree.entries[index].kind != ItemKind::kTypeAlias || tree.entries[index].parent != ItemTreeEntry::kNoParent) {
        continue;
      }
      const ItemId id = ResolveItem(ast, index);
      if (!ast.Contains(id) || !ast.Get(id).type.is_valid()) {
        continue;
      }
      const TypeId target = resolver.Resolve(ast, ast.Get(id).type);
      if (target.is_valid() && target != resolution->item_types[index]) {
        resolution->item_types[index] = target;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
    // Every alias binding has to see the new target before the next round.
    for (Binding& binding : resolution->scope) {
      if (binding.kind != BindingKind::kType ||
          binding.file != file_index ||
          binding.item == Binding::kNoItem ||
          binding.item >= tree.entries.size() ||
          tree.entries[binding.item].kind != ItemKind::kTypeAlias) {
        continue;
      }
      binding.type = resolution->item_types[binding.item];
    }
  }

  const auto signature_of = [&](const Item& item) -> TypeId {
    std::vector<TypeId> params;
    params.reserve(item.params.size());
    for (ParamId param_id : ast.Get(item.params)) {
      const Param& param = ast.Get(param_id);
      params.push_back(param.type.is_valid() ? resolver.Resolve(ast, param.type)
                                             : types->error());
    }
    const TypeId result = item.type.is_valid()
                              ? resolver.Resolve(ast, item.type)
                              : types->void_type();
    return types->Function(params, result);
  };

  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    const ItemTreeEntry& entry = tree.entries[index];
    const ItemId item_id = ResolveItem(ast, index);
    if (!ast.Contains(item_id)) {
      continue;
    }
    const Item& item = ast.Get(item_id);

    // A generic declaration's parameters have to be in scope while its own
    // signature is resolved.
    // A generic declaration's parameters have to be in scope while its own
    // signature is resolved, and so do an enclosing declaration's: the `T`
    // of `struct Holder<T>` is what the type of its `value` field names.
    // Pushed outermost first, so an inner parameter shadows an outer one of
    // the same name.
    std::vector<uint32_t> owners;
    for (uint32_t owner = index; owner != ItemTreeEntry::kNoParent;
         owner = tree.entries[owner].parent) {
      owners.push_back(owner);
      if (owners.size() > tree.entries.size()) {
        break;
      }
    }
    std::reverse(owners.begin(), owners.end());

    size_t pushed = 0;
    for (uint32_t owner : owners) {
      const ItemId owner_id = ResolveItem(ast, owner);
      if (!ast.Contains(owner_id)) {
        continue;
      }
      uint32_t position = 0;
      for (GenericParamId generic_id : ast.Get(ast.Get(owner_id).generics)) {
        const GenericParam& generic = ast.Get(generic_id);
        std::vector<TypeId> bounds;
        for (TypeRefId bound : ast.Get(generic.bounds)) {
          bounds.push_back(resolver.Resolve(ast, bound));
        }
        resolver.PushTypeParameter(
            generic.name,
            types->Parameter(generic.name, owner, position, bounds));
        ++position;
        ++pushed;
      }
      // Inside an ability, `Item` is the associated type of whatever
      // implements it: a projection of the ability itself, which a use
      // replaces with the type it is used on (D212).
      if (tree.entries[owner].kind == ItemKind::kAbility && resolution->item_types[owner].is_valid()) {
        for (uint32_t child = owner + 1; child < tree.entries.size(); ++child) {
          if (tree.entries[child].parent == owner && tree.entries[child].kind == ItemKind::kTypeAlias) {
            resolver.PushTypeParameter(tree.entries[child].name, types->Projection(resolution->item_types[owner], tree.entries[child].name));
            ++pushed;
          }
        }
      }
    }

    switch (entry.kind) {
      case ItemKind::kFunction:
      case ItemKind::kSystem:
      // A destructor takes nothing and gives nothing back; it is a
      // function type so that everything which lowers a method can lower
      // it too, without a second shape to handle.
      case ItemKind::kDeinit:
        resolution->item_types[index] = signature_of(item);
        break;
      case ItemKind::kSignal:
      case ItemKind::kOn:
        // A signal's type is the type of what listens to it: `signal
        // Died(by: int)` is connected to by a `fun(int)`. One type for
        // the two sides, so `Connect` and `Emit` check against the same
        // thing and a handler of the wrong shape is reported where it is
        // written.
        resolution->item_types[index] = signature_of(item);
        break;
      case ItemKind::kBinding:
      case ItemKind::kTypeAlias:
        if (item.type.is_valid()) {
          resolution->item_types[index] = resolver.Resolve(ast, item.type);
        }
        // `type Walker: Iterator` in an ability: what the member records
        // is the bound an implementation's choice has to satisfy.
        else if (entry.kind == ItemKind::kTypeAlias && item.super_type.is_valid()) {
          resolution->item_types[index] = resolver.Resolve(ast, item.super_type);
        }
        break;
      case ItemKind::kVariant: {
        // A variant with fields is constructed like a function returning
        // its enum; one without is a value of that enum already.
        const TypeId owner = entry.parent != ItemTreeEntry::kNoParent
                                 ? resolution->item_types[entry.parent]
                                 : types->error();
        std::vector<TypeId> fields;
        for (ParamId param : ast.Get(item.params)) {
          if (!ast.Contains(param)) {
            continue;
          }
          const TypeRefId annotation = ast.Get(param).type;
          fields.push_back(annotation.is_valid()
                               ? resolver.Resolve(ast, annotation)
                               : types->error());
        }
        resolution->item_types[index] =
            fields.empty() ? owner : types->Function(fields, owner);
        break;
      }
      default:
        break;
    }

    // `uses io, Telemetry`. Resolved here because an effect name may be
    // imported, exactly like a type name.
    //
    // `task` counts as part of the clause: the keyword is how a signature
    // says "this may suspend", so a declared row has to carry `async` or a
    // caller would not see it. A `task fun` with no clause at all keeps no
    // row -- an absent row is not an empty one (D38), and inference will
    // find `async` in the body.
    if (!item.effects.empty()) {
      EffectRow row;
      if (HasFlag(item.flags, ItemFlags::kAsync)) {
        row.Add(EffectId{static_cast<uint32_t>(BuiltinEffect::kAsync)});
      }
      for (TypeRefId effect_id : ast.Get(item.effects)) {
        if (!ast.Contains(effect_id)) {
          continue;
        }
        const TypeRef& effect = ast.Get(effect_id);
        const std::span<const Symbol> path = ast.Get(effect.path);
        if (path.empty()) {
          continue;
        }
        row.Add(resolution->FindEffect(path.back(), *interner));
      }
      resolution->declared_effects.push_back(
          DeclaredEffects{file_index, index, std::move(row)});
    }

    resolver.PopTypeParameters(pushed);
  }

  // An alias whose target is still an alias's own opaque name never
  // settled, which means the chain comes back to itself.
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    if (tree.entries[index].kind != ItemKind::kTypeAlias) {
      continue;
    }
    const TypeId target = resolution->item_types[index];
    if (types->KindOf(target) != TypeKind::kNamed) {
      continue;
    }
    const Type& named = types->Get(target);
    if (named.declaring_file != file_index ||
        named.declaring_item >= tree.entries.size() ||
        tree.entries[named.declaring_item].kind != ItemKind::kTypeAlias) {
      continue;
    }
    resolution->cyclic_aliases.push_back(index);
    resolution->item_types[index] = types->error();
  }

  // Now that every declaration has a type, collect the members of each named
  // type from its children. An `extend` block contributes to the table of the
  // type it extends, which is what makes `extend` additive.
  const auto members_of = [&](uint32_t index) {
    std::vector<Member> members;
    for (uint32_t child = 0; child < tree.entries.size(); ++child) {
      const ItemTreeEntry& member_entry = tree.entries[child];
      if (member_entry.parent != index || !member_entry.name.is_valid()) {
        continue;
      }

      Member member;
      member.name = member_entry.name;
      member.type = resolution->item_types[child];
      member.file = file_index;
      member.item = child;
      member.is_mutable = HasFlag(member_entry.flags, ItemFlags::kMutable);
      member.is_public = !HasFlag(member_entry.flags, ItemFlags::kPriv);
      member.is_virtual = HasFlag(member_entry.flags, ItemFlags::kVirtual);
      member.is_override = HasFlag(member_entry.flags, ItemFlags::kOverride);
      switch (member_entry.kind) {
        case ItemKind::kFunction:
          member.kind = MemberKind::kMethod;
          break;
        case ItemKind::kVariant:
          member.kind = MemberKind::kVariant;
          break;
        case ItemKind::kSignal:
        case ItemKind::kOn:
          member.kind = MemberKind::kSignal;
          break;
        case ItemKind::kDeinit:
          member.kind = MemberKind::kDeinit;
          break;
        case ItemKind::kTypeAlias:
          member.kind = MemberKind::kAssociatedType;
          break;
        default:
          member.kind =
              HasFlag(member_entry.flags, ItemFlags::kDerived)
                  ? MemberKind::kDerived
                  : MemberKind::kField;
          member.is_weak = HasFlag(member_entry.flags, ItemFlags::kWeak);
          break;
      }
      members.push_back(member);
    }

    // What each derived field reads, and the bit that says whether it has
    // been computed.
    //
    // The dependencies are worked out here rather than at the use, because
    // the code that writes a field may be in another module and a member
    // table is what crosses files. The bit is a member for the same
    // reason: it is part of what the type holds, and every tier has to
    // agree about that.
    // What a machine is: one field saying which state it is in, a
    // predicate per state, and a method per event.
    //
    // Synthesized here because the member table is what the checker reads
    // -- so `door.Interact()` and `door.IsOpen()` resolve with no special
    // case anywhere above this, and a machine from another module works
    // for the same reason a class does.
    if (tree.entries[index].kind == ItemKind::kMachine) {
      MachineMembers(ast, tree, interner, types, index, file_index,
                     &members);
    }

    // What a signal holds: how many handlers, how many it has room for,
    // and where they are. A separate member because the signal's own type
    // is the handler's -- and because a member table is what every tier
    // reads to find out what a type holds.
    std::vector<Member> storage;
    for (const Member& member : members) {
      if (member.kind != MemberKind::kSignal) {
        continue;
      }
      Member list;
      list.kind = MemberKind::kSignalList;
      list.type = types->Tuple(
          {types->Int(IntWidth::k64, /*is_signed=*/false),
           types->Int(IntWidth::k64, /*is_signed=*/false),
           types->Pointer(types->Int(IntWidth::k8, /*is_signed=*/false),
                          /*is_mutable=*/true)});
      list.file = member.file;
      list.item = member.item;
      list.is_mutable = true;
      list.is_public = false;
      storage.push_back(std::move(list));
    }
    for (Member& list : storage) {
      members.push_back(std::move(list));
    }

    std::vector<Member> ready;
    for (uint32_t i = 0; i < members.size(); ++i) {
      if (members[i].kind != MemberKind::kDerived) {
        continue;
      }
      members[i].depends_on =
          DerivedDependencies(ast, tree, members, members[i].item);
      Member bit;
      // No name: nothing in the source can refer to it, and leaving it
      // nameless is what guarantees that. It is found by the declaration
      // it belongs to, which is also what makes it work across files.
      bit.kind = MemberKind::kReadyBit;
      bit.type = types->bool_type();
      bit.file = file_index;
      bit.item = members[i].item;
      bit.is_mutable = true;
      bit.is_public = false;
      ready.push_back(std::move(bit));
    }
    for (Member& bit : ready) {
      members.push_back(std::move(bit));
    }
    return members;
  };

  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    const ItemTreeEntry& entry = tree.entries[index];
    if (!DeclaresAType(entry.kind) || !resolution->item_types[index].is_valid()) {
      continue;
    }
    // A `type` inside an ability or an extend is an associated type, a
    // member of its owner rather than a type of its own.
    if (entry.kind == ItemKind::kTypeAlias && entry.parent != ItemTreeEntry::kNoParent) {
      continue;
    }

    TypeMembers table;
    table.type = resolution->item_types[index];
    table.members = members_of(index);
    // A class is a reference to a heap object; everything else here stores
    // its members inline.
    table.is_value = entry.kind != ItemKind::kClass &&
                     entry.kind != ItemKind::kAbility;
    table.is_ability = entry.kind == ItemKind::kAbility;
    // The declaration's own parameters, interned against it, so that a field
    // of `Holder<int>` can be worked out by substitution.
    const ItemId owner_id = ResolveItem(ast, index);
    if (ast.Contains(owner_id)) {
      uint32_t position = 0;
      for (GenericParamId generic_id : ast.Get(ast.Get(owner_id).generics)) {
        table.parameters.push_back(types->Parameter(
            ast.Get(generic_id).name, index, position));
        ++position;
      }
    }

    resolution->type_members.push_back(std::move(table));

    // `class Turret : Actor` is what makes a bound of `Actor` satisfiable
    // and what gives `override` a method to override.
    TypeRelations relation;
    relation.type = resolution->item_types[index];
    const ItemId declaration = ResolveItem(ast, index);
    if (ast.Contains(declaration)) {
      const Item& item = ast.Get(declaration);
      if (item.super_type.is_valid()) {
        relation.base = resolver.Resolve(ast, item.super_type);
      }
    }
    resolution->relations.push_back(std::move(relation));
  }

  // `extend T { ... }` adds to T's table rather than declaring its own.
  for (uint32_t index = 0; index < tree.entries.size(); ++index) {
    if (tree.entries[index].kind != ItemKind::kExtend) {
      continue;
    }
    const ItemId item_id = ResolveItem(ast, index);
    if (!ast.Contains(item_id)) {
      continue;
    }
    const Item& item = ast.Get(item_id);
    // `extend<T> Box<T>`: the parameters it declares, the same ones its
    // members' signatures were written in.
    size_t declared_here = 0;
    uint32_t position = 0;
    for (GenericParamId generic_id : ast.Get(item.generics)) {
      const GenericParam& generic = ast.Get(generic_id);
      std::vector<TypeId> bounds;
      for (TypeRefId bound : ast.Get(generic.bounds)) {
        bounds.push_back(resolver.Resolve(ast, bound));
      }
      resolver.PushTypeParameter(generic.name, types->Parameter(generic.name, index, position, bounds));
      ++position;
      ++declared_here;
    }
    TypeId target = item.target_type.is_valid()
                        ? resolver.Resolve(ast, item.target_type)
                        : TypeId{};
    if (!target.is_valid()) {
      resolver.PopTypeParameters(declared_here);
      continue;
    }

    std::vector<Member> added = members_of(index);

    // `extend ListWalk<T>` for every `T`: the members belong to `ListWalk`
    // itself, written in its own parameters, so that `ListWalk<int>` finds
    // them the way it finds the ones declared inside it (D212).
    if (types->KindOf(target) == TypeKind::kInstance) {
      const TypeId base = types->Get(target).base;
      const std::vector<TypeId> given = types->ArgumentsOf(target);
      const std::span<const TypeId> declared = resolution->ParametersOf(base);
      bool every = given.size() == declared.size() && !given.empty();
      for (size_t i = 0; every && i < given.size(); ++i) {
        every = types->KindOf(given[i]) == TypeKind::kParameter && std::count(given.begin(), given.end(), given[i]) == 1;
      }
      if (every) {
        TypeStore::Substitution renamed;
        for (size_t i = 0; i < given.size(); ++i) {
          renamed.emplace_back(given[i], declared[i]);
        }
        for (Member& member : added) {
          member.type = types->Substitute(member.type, renamed);
        }
        target = base;
      }
    }
    bool merged = false;
    for (TypeMembers& table : resolution->type_members) {
      if (table.type != target) {
        continue;
      }
      table.members.insert(table.members.end(), added.begin(), added.end());
      merged = true;
      break;
    }
    if (!merged) {
      // The type is declared in another file, which keeps its own table; the
      // extension still has to be visible here.
      TypeMembers extension;
      extension.type = target;
      extension.members = std::move(added);
      resolution->type_members.push_back(std::move(extension));
    }

    // `extend T as Ability` is the declaration of conformance.
    if (!item.super_type.is_valid()) {
      resolver.PopTypeParameters(declared_here);
      continue;
    }
    const TypeId ability = resolver.Resolve(ast, item.super_type);
    resolver.PopTypeParameters(declared_here);
    if (!ability.is_valid()) {
      continue;
    }
    TypeRelations* relation = nullptr;
    for (TypeRelations& candidate : resolution->relations) {
      if (candidate.type == target) {
        relation = &candidate;
        break;
      }
    }
    if (relation == nullptr) {
      resolution->relations.push_back(TypeRelations{target, TypeId{}, {}});
      relation = &resolution->relations.back();
    }
    relation->abilities.push_back(ability);
  }

  // Patch this file's own bindings, whose type only now exists. An item
  // index only means anything within the file it came from, so a binding
  // brought in from elsewhere is left for that file's own pass.
  for (Binding& binding : resolution->scope) {
    if (binding.file != file_index || binding.type.is_valid() ||
        binding.item == Binding::kNoItem ||
        binding.item >= resolution->item_types.size()) {
      continue;
    }
    binding.type = resolution->item_types[binding.item];
  }

}

}  // namespace wolang
