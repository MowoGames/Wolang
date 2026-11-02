// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/ir/save_schema.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "wolang/ir/ir.h"

namespace wolang {
namespace ir {

struct SaveSchemaField {
  uint32_t Index = 0;
  TypeId Type;
  Symbol Name;
  bool Save = false;
};

class SaveSchemaWriter {
 public:
  explicit SaveSchemaWriter(const SaveSchemaHost& host) : host_(host) {}

  bool HasVirtualMethods(TypeId type) {
    const FileResolution* const where = host_.resolution(type);
    TypeId named = type;
    if (host_.types->KindOf(named) == TypeKind::kInstance) {
      named = host_.types->Get(type).base;
    }
    for (TypeId current = named; current.is_valid(); current = where->BaseOf(current)) {
      for (const Member& member : where->MembersOf(current)) {
        if (member.kind == MemberKind::kMethod && (member.is_virtual || member.is_override)) {
          return true;
        }
      }
    }

    return false;
  }

  std::string Build(TypeId root) {
    std::vector<TypeId> queue;
    std::vector<TypeId> done;
    TypeStore& types = *host_.types;

    if (!types.IsHandle(root) || types.IsForeignClass(root) || HasVirtualMethods(root)) {
      return std::string();
    }

    const std::string root_name = NameOf(root);
    std::string body;
    queue.push_back(root);
    for (size_t at = 0; at < queue.size(); ++at) {
      const TypeId type = queue[at];
      if (std::find(done.begin(), done.end(), type) != done.end()) {
        continue;
      }
      done.push_back(type);

      // A struct is a value, and half of one is not worth saving: unless it
      // marks fields of its own, all of it is written (D266).
      const bool is_class = types.IsHandle(type);
      const std::vector<SaveSchemaField> every = StoredFields(type);
      bool marked = is_class;
      for (const SaveSchemaField& field : every) {
        marked = marked || field.Save;
      }

      std::vector<SaveSchemaField> saved;
      for (const SaveSchemaField& field : every) {
        if (!marked || field.Save) {
          saved.push_back(field);
        }
      }

      body.append("TYPE ");
      body.append(is_class ? "C " : "S ");
      body.append(NameOf(type));
      body.append(" ");
      body.append(is_class ? host_.offset(type, kObjectSizeField) : std::string("0"));
      body.append(" ");
      body.append(std::to_string(saved.size()));
      body.append("\n");
      for (const SaveSchemaField& field : saved) {
        std::string shape;
        std::string wire;
        if (!ShapeOf(host_.erase(field.Type), &queue, &shape, &wire, 0)) {
          return std::string();
        }
        body.append("FIELD ");
        body.append(host_.interner->Text(field.Name));
        body.append(" ");
        body.append(host_.offset(type, field.Index + 1));
        body.append(" ");
        body.append(wire);
        body.append(" ");
        body.append(shape);
        body.append("\n");
      }
    }

    return "WSCHEMA 1\nROOT " + root_name + "\n" + body;
  }

 private:
  std::vector<SaveSchemaField> StoredFields(TypeId type) {
    TypeStore& types = *host_.types;
    TypeId named = type;
    TypeStore::Substitution substitution;
    const FileResolution* const where = host_.resolution(type);
    if (types.KindOf(named) == TypeKind::kInstance) {
      named = types.Get(type).base;
      const std::span<const TypeId> parameters = where->ParametersOf(named);
      const std::vector<TypeId> arguments = types.ArgumentsOf(type);
      for (size_t i = 0; i < parameters.size() && i < arguments.size(); ++i) {
        substitution.emplace_back(parameters[i], arguments[i]);
      }
    }

    std::vector<TypeId> chain;
    for (TypeId current = named; current.is_valid(); current = where->BaseOf(current)) {
      chain.push_back(current);
    }
    std::reverse(chain.begin(), chain.end());

    bool header = false;
    if (types.IsHandle(named)) {
      for (TypeId current : chain) {
        for (const Member& member : where->MembersOf(current)) {
          header = header || (member.kind == MemberKind::kMethod && (member.is_virtual || member.is_override));
        }
      }
    }

    std::vector<SaveSchemaField> fields;
    uint32_t index = header ? 1u : 0u;
    for (TypeId current : chain) {
      for (const Member& member : where->MembersOf(current)) {
        if (!HasStorage(member.kind)) {
          continue;
        }
        SaveSchemaField field;
        field.Index = index;
        field.Type = substitution.empty() ? member.type : types.Substitute(member.type, substitution);
        field.Name = member.name;
        field.Save = member.is_save;
        fields.push_back(field);
        ++index;
      }
    }

    return fields;
  }

  // A name the schema can carry on one line, one per type.
  std::string NameOf(TypeId type) {
    for (const auto& [seen, name] : names_) {
      if (seen == type) {
        return name;
      }
    }

    std::string spelled = host_.types->Describe(*host_.interner, type);
    for (char& letter : spelled) {
      if (letter == ' ' || letter == '\t' || letter == '\n') {
        letter = '_';
      }
    }
    for (const auto& [seen, name] : names_) {
      if (name == spelled) {
        spelled.append("#");
        spelled.append(std::to_string(type.index));
        break;
      }
    }
    names_.emplace_back(type, spelled);

    return spelled;
  }

  // The shape and the wire code of a field type `std.save` can write, and
  // false for one it cannot. Classes and structs it meets are queued so
  // that their own fields get described too (D266).
  bool ShapeOf(TypeId type, std::vector<TypeId>* queue, std::string* shape, std::string* wire, int depth) {
    TypeStore& types = *host_.types;
    if (depth > 8) {
      return false;
    }

    switch (types.KindOf(type)) {
      case TypeKind::kBool:
        *shape = "b";
        *wire = "b";
        return true;
      case TypeKind::kChar:
        *shape = "c";
        *wire = "c";
        return true;
      case TypeKind::kInt: {
        const Type& info = types.Get(type);
        const uint32_t bytes = 1u << static_cast<uint32_t>(info.int_width);
        *shape = std::string(info.is_signed ? "i" : "u") + std::to_string(bytes);
        *wire = *shape;
        return true;
      }
      case TypeKind::kFloat:
        *shape = types.Get(type).float_width == FloatWidth::k32 ? "f4" : "f8";
        *wire = *shape;
        return true;
      case TypeKind::kStr:
        *shape = "s";
        *wire = "s";
        return true;
      case TypeKind::kOptional: {
        std::string inner_shape;
        std::string inner_wire;
        if (!ShapeOf(types.ElementOf(type), queue, &inner_shape, &inner_wire, depth + 1)) {
          return false;
        }
        *shape = "o " + host_.offset(type, 2) + " " + inner_shape;
        *wire = "o(" + inner_wire + ")";
        return true;
      }
      case TypeKind::kNamed:
      case TypeKind::kInstance: {
        const FileResolution* const where = host_.resolution(type);
        if (where->IsAbility(type) || types.IsForeignClass(type)) {
          return false;
        }

        const std::string name = NameOf(type);
        if (types.IsHandle(type)) {
          if (HasVirtualMethods(type)) {
            return false;
          }
          *shape = "r " + name;
          *wire = "r:" + name;
          queue->push_back(type);
          return true;
        }

        bool variants = false;
        for (const Member& member : where->MembersOf(type)) {
          if (member.kind == MemberKind::kVariant) {
            if (types.KindOf(member.type) == TypeKind::kFunction) {
              return false;
            }
            variants = true;
          }
        }
        if (variants) {
          *shape = "e";
          *wire = "e:" + name;
          return true;
        }

        *shape = "v " + name;
        *wire = "v:" + name;
        queue->push_back(type);
        return true;
      }
      default:
        return false;
    }
  }

  const SaveSchemaHost& host_;
  std::vector<std::pair<TypeId, std::string>> names_;
};

std::string BuildSaveSchema(const SaveSchemaHost& host, TypeId root) {
  SaveSchemaWriter writer(host);

  return writer.Build(root);
}

bool SaveHasVirtualMethods(const SaveSchemaHost& host, TypeId type) {
  SaveSchemaWriter writer(host);

  return writer.HasVirtualMethods(type);
}

}  // namespace ir
}  // namespace wolang
