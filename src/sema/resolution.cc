// Copyright 2026 The Wolang Authors. All rights reserved.

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
    {"String", TypeKind::kString, IntWidth::k32, true, FloatWidth::k32},
    {"str", TypeKind::kStr, IntWidth::k32, true, FloatWidth::k32},
    {"never", TypeKind::kNever, IntWidth::k32, true, FloatWidth::k32},
    // `int` and `float` are the widths gameplay code should reach for when it
    // does not care, so they are aliases rather than separate types.
    {"int", TypeKind::kInt, IntWidth::k32, true, FloatWidth::k32},
    {"i8", TypeKind::kInt, IntWidth::k8, true, FloatWidth::k32},
    {"i16", TypeKind::kInt, IntWidth::k16, true, FloatWidth::k32},
    {"i32", TypeKind::kInt, IntWidth::k32, true, FloatWidth::k32},
    {"i64", TypeKind::kInt, IntWidth::k64, true, FloatWidth::k32},
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
    case ItemKind::kTrait:
    case ItemKind::kComponent:
    case ItemKind::kTag:
    case ItemKind::kTypeAlias:
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
      return BindingKind::kMachine;
    default:
      return BindingKind::kType;
  }
}

}  // namespace

Dimensions FileResolution::ProductOf(DimensionId dimension) const {
  if (!dimension.is_valid() ||
      dimension.index >= dimension_products.size()) {
    return Dimensions();
  }
  return dimension_products[dimension.index];
}

const Binding* FileResolution::Find(Symbol name) const {
  for (const Binding& binding : scope) {
    if (binding.name == name) {
      return &binding;
    }
  }
  return nullptr;
}

namespace {

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
      for (TypeRefId arg : ast.Get(type.args)) {
        args.push_back(Resolve(ast, arg));
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
    case TypeRefKind::kWith:
    case TypeRefKind::kWithout:
      // Query modifiers describe access, not the type itself; the access
      // graph in wave 11 reads them from the syntax.
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
      resolution.item_types[index] =
          types->Named(entry.name, file_index, index);
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
        const auto dimension_index =
            static_cast<uint32_t>(resolution.dimension_names.size());
        resolution.dimension_names.push_back(name);
        // Filled in below for a derived dimension; a base one stands for
        // itself.
        resolution.dimension_products.push_back(
            Dimensions::Base(DimensionId{dimension_index}));

        Binding binding;
        binding.name = name;
        binding.kind = BindingKind::kDimension;
        binding.item = index;
        binding.file = file_index;
        binding.dimension = DimensionId{dimension_index};
        binding.is_public = HasFlag(entry.flags, ItemFlags::kOffer);
        resolution.scope.push_back(binding);
      }
      if (item->init.is_valid()) {
        derived_dimensions.push_back(index);
      }
      continue;
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

    if (!entry.name.is_valid()) {
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
    if (EvaluateDimensionProduct(ast, resolution, *interner, item.init,
                                 diagnostics, &product)) {
      resolution.dimension_products[binding->dimension.index] = product;
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

}  // namespace wolang
