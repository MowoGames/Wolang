// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/compiler/item_tree.h"

#include <string_view>

#include "wolang/syntax/ast_printer.h"

namespace wolang {
namespace {

// FNV-1a. The hash only has to be stable within one build of the compiler and
// well distributed; it is never written to disk as an identity.
uint64_t HashBytes(std::string_view text, uint64_t seed = 0xcbf29ce484222325) {
  uint64_t hash = seed;
  for (char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3;
  }
  return hash;
}

class SignatureWriter {
 public:
  SignatureWriter(const StringInterner& interner, const Ast& ast)
      : interner_(interner), ast_(ast) {}

  std::string Build(ItemId id) {
    out_.clear();
    WriteItem(id);
    return std::move(out_);
  }

 private:
  void Put(std::string_view text) { out_.append(text); }

  void PutName(Symbol symbol) {
    out_.append(symbol.is_valid() ? interner_.Text(symbol) : "?");
  }

  void PutPath(AstRange<Symbol> path) {
    bool first = true;
    for (Symbol segment : ast_.Get(path)) {
      if (!first) {
        out_.push_back('.');
      }
      first = false;
      PutName(segment);
    }
  }

  void PutFlags(ItemFlags flags) {
    static constexpr struct {
      ItemFlags flag;
      std::string_view name;
    } kItemNames[] = {
        {ItemFlags::kOffer, "offer"},         {ItemFlags::kPriv, "priv"},
        {ItemFlags::kAsync, "task"},     {ItemFlags::kVirtual, "virtual"},
        {ItemFlags::kOverride, "override"}, {ItemFlags::kComptime, "comptime"},
        {ItemFlags::kDerived, "derived"}, {ItemFlags::kMutable, "mut"},
        {ItemFlags::kExtern, "extern"},   {ItemFlags::kAbilityImpl, "for"},
    };
    for (const auto& entry : kItemNames) {
      if (HasFlag(flags, entry.flag)) {
        out_.push_back(' ');
        Put(entry.name);
      }
    }
  }

  // An expression inside a signature is rendered through the tree printer, so
  // the text depends on the expression's shape and never on its position.
  void PutExpr(ExprId id) {
    if (!id.is_valid()) {
      return;
    }
    AstPrintOptions options;
    options.indent_width = 0;
    Put(PrintExpr(nullptr, interner_, ast_, id, options));
  }

  void PutType(TypeRefId id) {
    if (!ast_.Contains(id)) {
      Put("?");
      return;
    }
    const TypeRef& type = ast_.Get(id);
    if (HasFlag(type.flags, TypeRefFlags::kSoa)) {
      Put("soa ");
    } else if (HasFlag(type.flags, TypeRefFlags::kAos)) {
      Put("aos ");
    }
    switch (type.kind) {
      case TypeRefKind::kError:
        Put("<error>");
        return;
      case TypeRefKind::kPath:
        PutPath(type.path);
        if (!type.args.empty()) {
          out_.push_back('<');
          bool first = true;
          for (TypeRefId arg : ast_.Get(type.args)) {
            if (!first) {
              out_.push_back(',');
            }
            first = false;
            PutType(arg);
          }
          out_.push_back('>');
        }
        return;
      case TypeRefKind::kOptional:
        PutType(type.element);
        out_.push_back('?');
        return;
      case TypeRefKind::kFallible:
        PutType(type.element);
        out_.push_back('!');
        return;
      case TypeRefKind::kSlice:
        out_.push_back('[');
        PutType(type.element);
        out_.push_back(']');
        return;
      case TypeRefKind::kArray:
        out_.push_back('[');
        PutType(type.element);
        Put("; ");
        PutExpr(type.length);
        out_.push_back(']');
        return;
      case TypeRefKind::kFunction: {
        Put("fun(");
        bool first = true;
        for (TypeRefId arg : ast_.Get(type.args)) {
          if (!first) {
            out_.push_back(',');
          }
          first = false;
          PutType(arg);
        }
        Put(")");
        if (type.element.is_valid()) {
          Put("->");
          PutType(type.element);
        }
        return;
      }
      case TypeRefKind::kPointer:
        Put(HasFlag(type.flags, TypeRefFlags::kMutable) ? "*mut " : "*");
        PutType(type.element);
        return;
      case TypeRefKind::kReference:
        Put(HasFlag(type.flags, TypeRefFlags::kMutable) ? "&mut " : "&");
        PutType(type.element);
        return;
      case TypeRefKind::kTuple: {
        out_.push_back('(');
        bool first = true;
        for (TypeRefId arg : ast_.Get(type.args)) {
          if (!first) {
            out_.push_back(',');
          }
          first = false;
          PutType(arg);
        }
        out_.push_back(')');
        return;
      }
      case TypeRefKind::kMut:
        Put("mut ");
        PutType(type.element);
        return;
      case TypeRefKind::kWith:
        Put("with ");
        PutType(type.element);
        return;
      case TypeRefKind::kWithout:
        Put("without ");
        PutType(type.element);
        return;
    }
  }

  void PutAttributes(AstRange<AttributeId> attributes) {
    for (AttributeId id : ast_.Get(attributes)) {
      const Attribute& attribute = ast_.Get(id);
      Put(" @");
      PutName(attribute.name);
      if (attribute.args.empty()) {
        continue;
      }
      out_.push_back('(');
      bool first = true;
      for (ExprId arg : ast_.Get(attribute.args)) {
        if (!first) {
          out_.push_back(',');
        }
        first = false;
        PutExpr(arg);
      }
      out_.push_back(')');
    }
  }

  void PutGenerics(AstRange<GenericParamId> generics) {
    if (generics.empty()) {
      return;
    }
    out_.push_back('<');
    bool first = true;
    for (GenericParamId id : ast_.Get(generics)) {
      if (!first) {
        out_.push_back(',');
      }
      first = false;
      const GenericParam& param = ast_.Get(id);
      PutName(param.name);
      for (TypeRefId bound : ast_.Get(param.bounds)) {
        out_.push_back(':');
        PutType(bound);
      }
    }
    out_.push_back('>');
  }

  void PutParams(AstRange<ParamId> params) {
    out_.push_back('(');
    bool first = true;
    for (ParamId id : ast_.Get(params)) {
      if (!first) {
        out_.push_back(',');
      }
      first = false;
      const Param& param = ast_.Get(id);
      if (param.is_mutable) {
        Put("mut ");
      }
      PutName(param.name);
      out_.push_back(':');
      PutType(param.type);
      // Whether a parameter has a default is part of the signature; which
      // value it defaults to is not, so changing the default does not
      // invalidate callers.
      if (param.default_value.is_valid()) {
        Put("=_");
      }
    }
    out_.push_back(')');
  }

  void WriteItem(ItemId id) {
    if (!ast_.Contains(id)) {
      Put("<invalid>");
      return;
    }
    const Item& item = ast_.Get(id);

    Put(ItemKindName(item.kind));
    out_.push_back(' ');
    PutName(item.name);
    PutFlags(item.flags);
    PutAttributes(item.attributes);
    PutGenerics(item.generics);

    if (!item.params.empty() || item.kind == ItemKind::kFunction ||
        item.kind == ItemKind::kSignal || item.kind == ItemKind::kSystem ||
        item.kind == ItemKind::kOn) {
      PutParams(item.params);
    }

    if (item.type.is_valid()) {
      Put("->");
      PutType(item.type);
    }
    if (item.super_type.is_valid()) {
      Put(":");
      PutType(item.super_type);
    }
    if (item.target_type.is_valid()) {
      Put(" impl ");
      PutType(item.target_type);
    }

    if (!item.effects.empty()) {
      Put(" uses ");
      bool first = true;
      for (TypeRefId effect : ast_.Get(item.effects)) {
        if (!first) {
          out_.push_back(',');
        }
        first = false;
        PutType(effect);
      }
    }

    if (!item.orderings.empty()) {
      for (OrderingId id_ordering : ast_.Get(item.orderings)) {
        const Ordering& ordering = ast_.Get(id_ordering);
        Put(ordering.is_after ? " after " : " before ");
        PutPath(ordering.path);
      }
    }

    if (!item.path.empty()) {
      out_.push_back(' ');
      PutPath(item.path);
    }
    if (!item.names.empty()) {
      Put(" {");
      bool first = true;
      for (Symbol name : ast_.Get(item.names)) {
        if (!first) {
          out_.push_back(',');
        }
        first = false;
        PutName(name);
      }
      out_.push_back('}');
    }

    if (item.link_name.is_valid()) {
      Put(" = ");
      PutName(item.link_name);
    }
    if (item.abi.is_valid()) {
      Put(" abi ");
      PutName(item.abi);
    }

    // A unit's conversion factor is part of its meaning, so it belongs to the
    // signature. Every other initializer is a body and does not.
    if (item.kind == ItemKind::kUnit && item.init.is_valid()) {
      Put(" = ");
      PutExpr(item.init);
    }

    // The transitions of a machine are its interface.
    for (StateId state_id : ast_.Get(item.states)) {
      const State& state = ast_.Get(state_id);
      Put(" state ");
      PutName(state.name);
      for (TransitionId transition_id : ast_.Get(state.transitions)) {
        const Transition& transition = ast_.Get(transition_id);
        out_.push_back(' ');
        if (transition.is_always) {
          Put("always");
        } else {
          PutName(transition.event);
        }
        Put("->");
        PutName(transition.target);
      }
    }
  }

  const StringInterner& interner_;
  const Ast& ast_;
  std::string out_;
};

void Walk(const Ast& ast,
          AstRange<ItemId> items,
          uint32_t parent,
          SignatureWriter* writer,
          ItemTree* tree) {
  for (ItemId id : ast.Get(items)) {
    if (!ast.Contains(id)) {
      continue;
    }
    const Item& item = ast.Get(id);

    ItemTreeEntry entry;
    entry.kind = item.kind;
    entry.flags = item.flags;
    entry.name = item.name;
    entry.parent = parent;
    entry.signature = HashBytes(writer->Build(id));

    const auto index = static_cast<uint32_t>(tree->entries.size());
    tree->entries.push_back(entry);
    Walk(ast, item.members, index, writer, tree);
  }
}

bool WalkToIndex(const Ast& ast,
                 AstRange<ItemId> items,
                 uint32_t* remaining,
                 ItemId* out) {
  for (ItemId id : ast.Get(items)) {
    if (!ast.Contains(id)) {
      continue;
    }
    if (*remaining == 0) {
      *out = id;
      return true;
    }
    --*remaining;
    if (WalkToIndex(ast, ast.Get(id).members, remaining, out)) {
      return true;
    }
  }
  return false;
}

}  // namespace

ItemTree BuildItemTree(const StringInterner& interner, const Ast& ast) {
  SignatureWriter writer(interner, ast);
  ItemTree tree;
  Walk(ast, ast.root(), ItemTreeEntry::kNoParent, &writer, &tree);
  return tree;
}

ItemId ResolveItem(const Ast& ast, uint32_t index) {
  uint32_t remaining = index;
  ItemId found;
  WalkToIndex(ast, ast.root(), &remaining, &found);
  return found;
}

std::string SignatureText(const StringInterner& interner,
                          const Ast& ast,
                          ItemId item) {
  SignatureWriter writer(interner, ast);
  return writer.Build(item);
}

}  // namespace wolang
