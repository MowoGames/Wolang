// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/bind/reflection.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wolang/lsp/json.h"
#include "wolang/syntax/token_kind.h"

namespace wolang {
namespace bind {

using lsp::Json;

// How one C++ type crosses: what a script sees, what the glue's C
// signature takes, and the engine's own spelling for the casts.
struct Shape {
  enum class Kind { kUnsupported, kVoid, kScalar, kEnum, kObject, kVector, kStruct, kText };
  Kind kind = Kind::kUnsupported;
  std::string wolang;
  std::string c;
  std::string cxx;
  // An object's type as a field holds it, and whether that is a smart
  // pointer whose raw pointer is `.Get()` (D262).
  std::string stored;
  bool smart = false;
};

// A value type that crosses by pointer to a struct of the same fields on
// both sides. The engine's vectors are built by a constructor, so they
// can go both ways; a reflected struct is only read back (D262).
struct ValueField {
  std::string name;
  std::string member;
  std::string wolang;
  std::string c;
};

struct ValueType {
  std::string name;
  std::string cxx;
  std::vector<ValueField> fields;
  bool constructible = true;
};

static ValueType Floats(const char* name, const char* cxx, const std::vector<std::pair<const char*, const char*>>& fields) {
  ValueType type;
  type.name = name;
  type.cxx = cxx;
  for (const auto& field : fields) {
    ValueField value;
    value.name = field.first;
    value.member = field.second;
    value.wolang = "f32";
    value.c = "float";
    type.fields.push_back(std::move(value));
  }
  return type;
}

// The engine's small value types, laid out as floats in this order. The
// engine builds a quaternion `x, y, z, w` (D262), and a rotator is
// `Pitch, Yaw, Roll`.
static const std::vector<ValueType>& Builtins() {
  static const std::vector<ValueType> sm_Builtins = {
      Floats("Vec2", "Vec2", {{"x", "x"}, {"y", "y"}}),
      Floats("Vec3", "Vec3", {{"x", "x"}, {"y", "y"}, {"z", "z"}}),
      Floats("Vec4", "Vec4", {{"x", "x"}, {"y", "y"}, {"z", "z"}, {"w", "w"}}),
      Floats("Quat", "Quat", {{"x", "x"}, {"y", "y"}, {"z", "z"}, {"w", "w"}}),
      Floats("Rotator", "FRotator", {{"pitch", "Pitch"}, {"yaw", "Yaw"}, {"roll", "Roll"}}),
  };
  return sm_Builtins;
}

static std::string TrimText(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(" \t");
  return std::string(text.substr(begin, end - begin + 1));
}

static bool HasFlag(const Json& owner, const char* flag) {
  const Json& flags = owner["flags"];
  for (size_t f = 0; f < flags.size(); ++f) {
    if (flags.At(f).AsString() == flag) {
      return true;
    }
  }
  return false;
}

struct ClassInfo {
  std::string name;
  std::string qualified;
  std::string base;
  std::string include;
  const Json* type = nullptr;
};

class Generator {
 public:
  Generator(const ReflectionOptions& options, ReflectionResult* result) : m_Options(options), m_Result(result) {}

  void Collect(const Json& document) {
    const Json& headers = document["headers"];
    for (size_t h = 0; h < headers.size(); ++h) {
      const Json& header = headers.At(h);
      const Json& enums = header["enums"];
      for (size_t e = 0; e < enums.size(); ++e) {
        m_Enums[enums.At(e)["name"].AsString()] = enums.At(e)["qualifiedName"].AsString();
      }
      const Json& types = header["types"];
      for (size_t t = 0; t < types.size(); ++t) {
        const Json& type = types.At(t);
        const std::string kind = type["kind"].AsString();
        if (kind != "class") {
          if (kind == "component") {
            m_Result->skipped.push_back(type["name"].AsString() + ": a component, which belongs on the ECS side");
          }
          else if (kind == "struct") {
            ClassInfo info;
            info.name = type["name"].AsString();
            info.qualified = type["qualifiedName"].AsString();
            info.include = header["include"].AsString();
            info.type = &type;
            m_StructInfos.push_back(std::move(info));
          }
          continue;
        }
        ClassInfo info;
        info.name = type["name"].AsString();
        info.qualified = type["qualifiedName"].AsString();
        info.base = type["super"].AsString();
        info.include = header["include"].AsString();
        info.type = &type;
        m_Classes.push_back(std::move(info));
      }
    }
  }

  void Write() {
    for (const ClassInfo& info : m_Classes) {
      m_Known[info.name] = &info;
    }
    ResolveStructs();
    CountStatics();
    for (const ClassInfo& info : m_Classes) {
      WriteClass(info);
    }

    std::string& source = m_Result->source;
    source = "// Generated by `wolang bind --reflection` from the engine's reflection.\n\n";
    if (m_UsesText) {
      source += "need std.text.{TextFromRaw}\n\n";
    }
    if (!m_Options.effect.empty()) {
      source += "offer effect " + m_Options.effect + "\n\n";
    }
    const std::vector<const ValueType*> used = UsedValues();
    for (const ValueType* type : used) {
      source += "offer struct " + type->name + " {\n";
      for (const ValueField& field : type->fields) {
        source += "  var " + field.name + ": " + field.wolang + "\n";
      }
      source += "}\n\n";
    }
    source += m_Declarations;
    source += m_Extensions;
    source += m_Free;

    std::string& glue = m_Result->glue;
    glue = "// Generated by `wolang bind --reflection` from the engine's reflection.\n\n";
    std::set<std::string> includes;
    for (const ClassInfo& info : m_Classes) {
      includes.insert(info.include);
    }
    for (const ClassInfo& info : m_StructInfos) {
      includes.insert(info.include);
    }
    for (const std::string& include : includes) {
      glue += "#include \"" + include + "\"\n";
    }
    glue += "\n#include <cstdint>\n#include <string>\n\n#include \"wolang/embed/wolang.h\"\n\n";
    for (const ValueType* type : used) {
      glue += "struct Wolang" + type->name + " {\n";
      for (const ValueField& field : type->fields) {
        glue += "\t" + field.c + " " + field.name + ";\n";
      }
      glue += "};\n\n";
    }
    glue += "extern \"C\" uint32_t MowoWolangPropertyOffset(const char* className, const char* key);\n";
    glue += "extern \"C\" void MowoWolangRetain(void* object, void* user);\n";
    glue += "extern \"C\" void MowoWolangRelease(void* object, void* user);\n";
    glue += "extern \"C\" int32_t MowoWolangAlive(void* object, void* user);\n\n";
    glue += "namespace Mowo {\n\n" + m_Glue + "}\n\n";
    glue += "extern \"C\" void MowoWolang_Bind_" + m_Options.module + "(WolangVm* vm) {\n" + m_Registrations + "}\n";
  }

 private:
  // A name a script can use: a reserved word gets a suffix.
  static std::string Identifier(const std::string& name, size_t index) {
    if (name.empty()) {
      return "argument" + std::to_string(index);
    }
    if (LookupKeyword(name) != TokenKind::kIdentifier || IsReservedWord(name) || LookupContextualKeyword(name) != ContextualKeyword::kNone) {
      return name + "Value";
    }
    return name;
  }

  const ValueType* ValueNamed(const std::string& spelled) const {
    for (const ValueType& type : Builtins()) {
      if (spelled == type.name || spelled == type.cxx) {
        return &type;
      }
    }
    for (const ValueType& type : m_Structs) {
      if (spelled == type.name || spelled == type.cxx) {
        return &type;
      }
    }
    return nullptr;
  }

  std::vector<const ValueType*> UsedValues() const {
    std::vector<const ValueType*> used;
    for (const ValueType& type : Builtins()) {
      if (m_UsedValues.count(type.name) != 0) {
        used.push_back(&type);
      }
    }
    for (const ValueType& type : m_Structs) {
      if (m_UsedValues.count(type.name) != 0) {
        used.push_back(&type);
      }
    }
    return used;
  }

  static const std::map<std::string, std::pair<std::string, std::string>>& Scalars() {
    static const std::map<std::string, std::pair<std::string, std::string>> sm_Scalars = {
        {"float", {"f32", "float"}},
        {"double", {"f64", "double"}},
        {"bool", {"bool", "bool"}},
        {"int", {"s32", "int32_t"}},
        {"int32_t", {"s32", "int32_t"}},
        {"uint32_t", {"u32", "uint32_t"}},
        {"int64_t", {"s64", "int64_t"}},
        {"uint64_t", {"u64", "uint64_t"}},
        {"int16_t", {"s16", "int16_t"}},
        {"uint16_t", {"u16", "uint16_t"}},
        {"int8_t", {"s8", "int8_t"}},
        {"uint8_t", {"u8", "uint8_t"}},
    };
    return sm_Scalars;
  }

  // A reflected struct crosses when every field is a scalar: it is the
  // out-pointer a vector already uses, with its own fields (D262).
  void ResolveStructs() {
    for (const ClassInfo& info : m_StructInfos) {
      ValueType type;
      type.name = info.name;
      type.cxx = info.qualified.empty() ? info.name : info.qualified;
      type.constructible = false;
      std::string problem;
      const Json& properties = (*info.type)["properties"];
      for (size_t i = 0; i < properties.size(); ++i) {
        const Json& property = properties.At(i);
        const std::string member = property["name"].AsString();
        const auto scalar = Scalars().find(TrimText(property["type"].AsString()));
        if (member.empty() || scalar == Scalars().end()) {
          problem = "`" + property["type"].AsString() + "` field has no binding";
          break;
        }
        std::string spelled = member.rfind("m_", 0) == 0 ? member.substr(2) : member;
        if (spelled.empty()) {
          problem = "a field with no name";
          break;
        }
        spelled[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(spelled[0])));
        ValueField field;
        field.name = Identifier(spelled, i);
        field.member = member;
        field.wolang = scalar->second.first;
        field.c = scalar->second.second;
        type.fields.push_back(std::move(field));
      }
      if (problem.empty() && type.fields.empty()) {
        problem = "a struct with no fields";
      }
      if (!problem.empty()) {
        m_Result->skipped.push_back(info.name + ": a struct that cannot be returned: " + problem);
        continue;
      }
      m_Structs.push_back(std::move(type));
    }
  }

  void CountStatics() {
    for (const ClassInfo& info : m_Classes) {
      const Json& functions = (*info.type)["functions"];
      for (size_t i = 0; i < functions.size(); ++i) {
        if (HasFlag(functions.At(i), "Static")) {
          ++m_StaticCount[functions.At(i)["name"].AsString()];
        }
      }
    }
  }

  // A class name without the engine's one-letter prefix: `MStringStatics`
  // is `StringStatics`, `Camera` stays as it is (D262).
  static std::string Stem(const std::string& name) {
    if (name.size() > 1 && std::isupper(static_cast<unsigned char>(name[0])) != 0 && std::isupper(static_cast<unsigned char>(name[1])) != 0) {
      return name.substr(1);
    }
    return name;
  }

  Shape ObjectShape(const std::string& pointee_spelled, bool smart, const std::string& spelled) const {
    Shape shape;
    const std::string pointee = TrimText(pointee_spelled);
    const auto known = m_Known.find(pointee.rfind("const ", 0) == 0 ? TrimText(pointee.substr(6)) : pointee);
    if (known == m_Known.end()) {
      return shape;
    }
    shape.kind = Shape::Kind::kObject;
    shape.wolang = known->second->name;
    shape.c = "void*";
    shape.cxx = known->second->qualified;
    shape.smart = smart;
    shape.stored = smart ? spelled : shape.cxx + "*";
    return shape;
  }

  Shape ShapeOf(const std::string& spelled) const {
    std::string text = TrimText(spelled);
    if (text.rfind("const ", 0) == 0) {
      text = TrimText(text.substr(6));
    }
    if (!text.empty() && text.back() == '&') {
      text = TrimText(text.substr(0, text.size() - 1));
    }

    Shape shape;
    if (text == "void") {
      shape.kind = Shape::Kind::kVoid;
      return shape;
    }
    if (!text.empty() && text.back() == '*') {
      return ObjectShape(text.substr(0, text.size() - 1), false, text);
    }
    static const std::string kSmart = "TObjectPtr<";
    if (text.rfind(kSmart, 0) == 0 && text.back() == '>') {
      return ObjectShape(text.substr(kSmart.size(), text.size() - kSmart.size() - 1), true, text);
    }

    const auto scalar = Scalars().find(text);
    if (scalar != Scalars().end()) {
      shape.kind = Shape::Kind::kScalar;
      shape.wolang = scalar->second.first;
      shape.c = scalar->second.second;
      shape.cxx = text;
      return shape;
    }
    if (text == "std::string") {
      shape.kind = Shape::Kind::kText;
      shape.wolang = "text";
      shape.cxx = "std::string";
      return shape;
    }
    if (const ValueType* value = ValueNamed(text)) {
      shape.kind = value->constructible ? Shape::Kind::kVector : Shape::Kind::kStruct;
      shape.wolang = value->name;
      shape.c = "Wolang" + value->name;
      shape.cxx = value->cxx;
      return shape;
    }
    const auto known_enum = m_Enums.find(text);
    if (known_enum != m_Enums.end()) {
      shape.kind = Shape::Kind::kEnum;
      shape.wolang = "s32";
      shape.c = "int32_t";
      shape.cxx = known_enum->second.empty() ? text : known_enum->second;
    }
    return shape;
  }

  // What every engine call says it does, and what a wrapper adds.
  std::string Uses(bool wrapper) const {
    if (m_Options.effect.empty()) {
      return wrapper ? " uses unsafe" : "";
    }
    return wrapper ? " uses " + m_Options.effect + ", unsafe" : " uses " + m_Options.effect;
  }

  // What the glue checks before it touches an object: a script may hold
  // an object the engine has already destroyed (D262).
  std::string Guard() const {
    if (m_Options.check_alive) {
      return "self == nullptr || MowoWolangAlive(self, nullptr) == 0";
    }
    return "self == nullptr";
  }

  // Whether |name| is declared by a class |info| derives from: an override,
  // which the base's binding already reaches.
  bool Inherited(const ClassInfo& info, const std::string& name) const {
    for (auto base = m_Known.find(info.base); base != m_Known.end(); base = m_Known.find(base->second->base)) {
      if (m_Declared.count(base->second->name + "." + name) != 0) {
        return true;
      }
      const Json& functions = (*base->second->type)["functions"];
      for (size_t i = 0; i < functions.size(); ++i) {
        if (functions.At(i)["name"].AsString() == name) {
          return true;
        }
      }
    }
    return false;
  }

  struct Parameter {
    std::string name;
    Shape shape;
  };

  static void Add(std::string* list, const std::string& item) {
    if (!list->empty()) {
      list->append(", ");
    }
    list->append(item);
  }

  static std::string Indented(const std::string& block, const std::string& indent) {
    std::string out;
    size_t begin = 0;
    while (begin < block.size()) {
      size_t end = block.find('\n', begin);
      if (end == std::string::npos) {
        end = block.size();
      }
      if (end > begin) {
        out += indent + block.substr(begin, end - begin);
      }
      out += "\n";
      begin = end + 1;
    }
    return out;
  }

  void WriteClass(const ClassInfo& info) {
    ++m_Result->classes;
    const bool derived = m_Known.count(info.base) != 0;
    m_Declarations += "offer extern class " + info.name + (derived ? ": " + info.base : "") + " {\n";
    std::string extension;
    m_Registrations += "\twolang_vm_register_class(vm, \"" + info.name + "\", &MowoWolangRetain, &MowoWolangRelease, &MowoWolangAlive, nullptr);\n";

    std::set<std::string> names;
    const Json& functions = (*info.type)["functions"];
    for (size_t i = 0; i < functions.size(); ++i) {
      const Json& function = functions.At(i);
      const std::string name = function["name"].AsString();
      const std::string where = info.name + "." + name;
      const bool is_static = HasFlag(function, "Static");
      if (names.count(name) != 0) {
        m_Result->skipped.push_back(where + ": overloaded");
        continue;
      }
      if (!is_static && Inherited(info, name)) {
        continue;
      }

      std::vector<Parameter> parameters;
      std::string problem;
      const Json& params = function["params"];
      for (size_t p = 0; p < params.size(); ++p) {
        Parameter parameter;
        parameter.name = Identifier(params.At(p)["name"].AsString(), p);
        parameter.shape = ShapeOf(params.At(p)["type"].AsString());
        if (parameter.shape.kind == Shape::Kind::kUnsupported || parameter.shape.kind == Shape::Kind::kVoid) {
          problem = "`" + params.At(p)["type"].AsString() + "` has no binding";
        }
        if (parameter.shape.kind == Shape::Kind::kStruct) {
          problem = "`" + params.At(p)["type"].AsString() + "` is a struct, which is only answered, not taken";
        }
        parameters.push_back(std::move(parameter));
      }
      const Shape result = ShapeOf(function["returnType"].AsString());
      if (result.kind == Shape::Kind::kUnsupported) {
        problem = "answers `" + function["returnType"].AsString() + "`, which has no binding";
      }
      if (!problem.empty()) {
        m_Result->skipped.push_back(where + ": " + problem);
        continue;
      }

      names.insert(name);
      m_Declared.insert(where);
      ++m_Result->functions;
      std::string exposed = name;
      if (is_static && m_StaticCount[name] > 1) {
        exposed = Stem(info.name) + name;
      }
      WriteFunction(info, name, exposed, is_static, parameters, result, &extension);
    }

    const Json& properties = (*info.type)["properties"];
    for (size_t i = 0; i < properties.size(); ++i) {
      const Json& property = properties.At(i);
      std::string base = property["name"].AsString();
      if (base.rfind("m_", 0) == 0) {
        base = base.substr(2);
      }
      if (base.empty()) {
        continue;
      }
      base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
      const std::string where = info.name + "." + property["name"].AsString();
      const Shape shape = ShapeOf(property["type"].AsString());
      if (shape.kind == Shape::Kind::kUnsupported || shape.kind == Shape::Kind::kStruct) {
        m_Result->skipped.push_back(where + ": a `" + property["type"].AsString() + "` property has no binding");
        continue;
      }
      if (names.count("Get" + base) != 0 || names.count("Set" + base) != 0 || Inherited(info, "Get" + base)) {
        m_Result->skipped.push_back(where + ": a function already has its accessor's name");
        continue;
      }
      bool writable = true;
      const Json& flags = property["flags"];
      for (size_t f = 0; f < flags.size(); ++f) {
        writable = writable && flags.At(f).AsString() != "ReadOnly" && flags.At(f).AsString() != "VisibleAnywhere";
      }
      names.insert("Get" + base);
      ++m_Result->properties;
      WriteProperty(info, base, property["key"].AsString(), shape, writable, &extension);
    }

    m_Declarations += "}\n\n";
    if (!extension.empty()) {
      m_Extensions += "extend " + info.name + " {\n" + extension + "}\n\n";
    }
  }

  void WriteFunction(const ClassInfo& info, const std::string& name, const std::string& exposed, bool is_static, const std::vector<Parameter>& parameters, const Shape& result, std::string* extension) {
    const bool value_result = result.kind == Shape::Kind::kVector || result.kind == Shape::Kind::kStruct;
    bool wrapped = is_static || value_result || result.kind == Shape::Kind::kText;
    for (const Parameter& parameter : parameters) {
      wrapped = wrapped || parameter.shape.kind == Shape::Kind::kVector || parameter.shape.kind == Shape::Kind::kText;
    }
    const std::string raw = wrapped && !is_static ? "Raw" + name : name;
    const std::string symbol = info.name + "_" + raw;

    std::string declared;
    std::string glue_parameters = is_static ? "" : "void* self";
    std::string call;
    std::string forwarded;
    std::string prologue;
    std::string wrapper_parameters;
    for (const Parameter& parameter : parameters) {
      const Shape& shape = parameter.shape;
      const std::string& n = parameter.name;
      Add(&wrapper_parameters, n + ": " + shape.wolang);
      switch (shape.kind) {
        case Shape::Kind::kVector:
          Add(&declared, n + ": *mut " + shape.wolang);
          Add(&glue_parameters, "const " + shape.c + "* " + n);
          Add(&call, VectorValue(*ValueNamed(shape.wolang), n + "->"));
          prologue += "var " + n + "Copy = " + n + "\n";
          Add(&forwarded, "&mut " + n + "Copy");
          m_UsedValues.insert(shape.wolang);
          break;
        case Shape::Kind::kText:
          Add(&declared, n + ": *u8, " + n + "Length: u64");
          Add(&glue_parameters, "const char* " + n + ", uint64_t " + n + "Length");
          Add(&call, "std::string(" + n + ", static_cast<size_t>(" + n + "Length))");
          Add(&forwarded, n + ".data, " + n + ".length");
          break;
        case Shape::Kind::kEnum:
          Add(&declared, n + ": s32");
          Add(&glue_parameters, "int32_t " + n);
          Add(&call, "static_cast<" + shape.cxx + ">(" + n + ")");
          Add(&forwarded, n);
          break;
        case Shape::Kind::kObject:
          Add(&declared, n + ": " + shape.wolang);
          Add(&glue_parameters, "void* " + n);
          Add(&call, "static_cast<" + shape.cxx + "*>(" + n + ")");
          Add(&forwarded, n);
          break;
        default:
          Add(&declared, n + ": " + shape.wolang);
          Add(&glue_parameters, shape.c + " " + n);
          Add(&call, n);
          Add(&forwarded, n);
          break;
      }
    }

    std::string answer;
    std::string wrapper_answer;
    std::string glue_result = "void";
    const std::string invoked = is_static ? info.qualified + "::" + name + "(" + call + ")" : "static_cast<" + info.qualified + "*>(self)->" + name + "(" + call + ")";
    std::string body;
    switch (result.kind) {
      case Shape::Kind::kVector:
      case Shape::Kind::kStruct:
        Add(&declared, "out: *mut " + result.wolang);
        Add(&glue_parameters, result.c + "* out");
        body = "\tconst auto value = " + invoked + ";\n" + ValueOut(*ValueNamed(result.wolang));
        wrapper_answer = " -> " + result.wolang;
        m_UsedValues.insert(result.wolang);
        break;
      case Shape::Kind::kText:
        Add(&declared, "resultLength: *mut u64");
        Add(&glue_parameters, "uint64_t* resultLength");
        answer = " -> *u8";
        wrapper_answer = " -> text";
        glue_result = "const char*";
        body = "\tstatic thread_local std::string held;\n\theld = " + invoked + ";\n\t*resultLength = held.size();\n\treturn held.c_str();\n";
        m_UsesText = true;
        break;
      case Shape::Kind::kVoid:
        body = "\t" + invoked + ";\n";
        break;
      case Shape::Kind::kEnum:
        answer = " -> s32";
        wrapper_answer = answer;
        glue_result = "int32_t";
        body = "\treturn static_cast<int32_t>(" + invoked + ");\n";
        break;
      case Shape::Kind::kObject:
        answer = " -> " + result.wolang;
        wrapper_answer = answer;
        glue_result = "void*";
        body = "\treturn const_cast<void*>(static_cast<const void*>(" + invoked + (result.smart ? ".Get()" : "") + "));\n";
        break;
      default:
        answer = " -> " + result.wolang;
        wrapper_answer = answer;
        glue_result = result.c;
        body = "\treturn " + invoked + ";\n";
        break;
    }

    if (is_static) {
      m_Free += "extern \"C\" fun " + symbol + "(" + declared + ")" + answer + Uses(false) + "\n";
    }
    else {
      m_Declarations += "  fun " + raw + "(" + declared + ")" + answer + Uses(false) + "\n";
    }
    std::string guard;
    if (!is_static) {
      guard = "\tif (" + Guard() + ") {\n\t\treturn" + (glue_result == "void" ? "" : " {}") + ";\n\t}\n\n";
    }
    m_Glue += "extern \"C\" " + glue_result + " " + symbol + "(" + glue_parameters + ") {\n" + guard + body + "}\n\n";
    m_Registrations += "\twolang_vm_bind(vm, \"" + symbol + "\", reinterpret_cast<void*>(&Mowo::" + symbol + "));\n";

    if (!wrapped) {
      return;
    }
    const std::string target = is_static ? symbol : "self." + raw;
    auto call_with = [&](const std::string& extra) {
      std::string list = forwarded;
      if (!extra.empty()) {
        Add(&list, extra);
      }
      return target + "(" + list + ")";
    };

    std::string lines = prologue;
    if (value_result) {
      lines += "var out = " + ZeroOf(*ValueNamed(result.wolang)) + "\n" + call_with("&mut out") + "\nreturn out\n";
    }
    else if (result.kind == Shape::Kind::kText) {
      lines += "var resultLength: u64 = 0\nconst resultData = " + call_with("&mut resultLength") + "\nreturn TextFromRaw(resultData, resultLength as int) + \"\"\n";
    }
    else if (result.kind == Shape::Kind::kVoid) {
      lines += call_with("") + "\n";
    }
    else {
      lines += "return " + call_with("") + "\n";
    }

    const std::string indent = is_static ? "" : "  ";
    std::string wrapper = indent + "offer fun " + exposed + "(" + wrapper_parameters + ")" + wrapper_answer + Uses(true) + " {\n" + Indented(lines, indent + "  ") + indent + "}\n";
    if (is_static) {
      m_Free += wrapper + "\n";
    }
    else {
      *extension += wrapper;
    }
  }

  void WriteProperty(const ClassInfo& info, const std::string& base, const std::string& key, const Shape& shape, bool writable, std::string* extension) {
    const std::string offset = "\tstatic const uint32_t offset = MowoWolangPropertyOffset(\"" + info.name + "\", \"" + key + "\");\n\tif (" + Guard() + " || offset == UINT32_MAX) {\n\t\treturn";
    const std::string at = "static_cast<char*>(self) + offset";
    const bool raw = shape.kind == Shape::Kind::kVector || shape.kind == Shape::Kind::kText;
    const std::string getter = info.name + "_" + (raw ? "RawGet" : "Get") + base;
    const std::string setter = info.name + "_" + (raw ? "RawSet" : "Set") + base;
    std::string stored = shape.cxx;
    if (shape.kind == Shape::Kind::kObject) {
      stored = shape.stored;
    }

    if (shape.kind == Shape::Kind::kVector) {
      const ValueType& type = *ValueNamed(shape.wolang);
      m_UsedValues.insert(shape.wolang);
      m_Declarations += "  fun RawGet" + base + "(out: *mut " + shape.wolang + ")" + Uses(false) + "\n";
      m_Glue += "extern \"C\" void " + getter + "(void* self, " + shape.c + "* out) {\n" + offset + ";\n\t}\n\n\tconst auto& value = *reinterpret_cast<const " + stored + "*>(" + at + ");\n" + ValueOut(type) + "}\n\n";
      *extension += "  offer fun Get" + base + "() -> " + shape.wolang + Uses(true) + " {\n    var out = " + ZeroOf(type) + "\n    self.RawGet" + base + "(&mut out)\n    return out\n  }\n";
      if (writable) {
        m_Declarations += "  fun RawSet" + base + "(value: *mut " + shape.wolang + ")" + Uses(false) + "\n";
        m_Glue += "extern \"C\" void " + setter + "(void* self, const " + shape.c + "* value) {\n" + offset + ";\n\t}\n\n\t*reinterpret_cast<" + stored + "*>(" + at + ") = " + VectorValue(type, "value->") + ";\n}\n\n";
        *extension += "  offer fun Set" + base + "(value: " + shape.wolang + ")" + Uses(true) + " {\n    var copy = value\n    self.RawSet" + base + "(&mut copy)\n  }\n";
      }
    }
    else if (shape.kind == Shape::Kind::kText) {
      m_UsesText = true;
      m_Declarations += "  fun RawGet" + base + "(resultLength: *mut u64) -> *u8" + Uses(false) + "\n";
      m_Glue += "extern \"C\" const char* " + getter + "(void* self, uint64_t* resultLength) {\n" + offset + " nullptr;\n\t}\n\n\tstatic thread_local std::string held;\n\theld = *reinterpret_cast<const std::string*>(" + at + ");\n\t*resultLength = held.size();\n\treturn held.c_str();\n}\n\n";
      *extension += "  offer fun Get" + base + "() -> text" + Uses(true) + " {\n    var resultLength: u64 = 0\n    const resultData = self.RawGet" + base + "(&mut resultLength)\n    return TextFromRaw(resultData, resultLength as int) + \"\"\n  }\n";
      if (writable) {
        m_Declarations += "  fun RawSet" + base + "(value: *u8, valueLength: u64)" + Uses(false) + "\n";
        m_Glue += "extern \"C\" void " + setter + "(void* self, const char* value, uint64_t valueLength) {\n" + offset + ";\n\t}\n\n\t*reinterpret_cast<std::string*>(" + at + ") = std::string(value, static_cast<size_t>(valueLength));\n}\n\n";
        *extension += "  offer fun Set" + base + "(value: text)" + Uses(true) + " {\n    self.RawSet" + base + "(value.data, value.length)\n  }\n";
      }
    }
    else {
      const std::string c = shape.kind == Shape::Kind::kEnum ? "int32_t" : shape.c;
      std::string read = "static_cast<" + c + ">(*reinterpret_cast<const " + stored + "*>(" + at + "))";
      std::string write = "static_cast<" + stored + ">(value)";
      if (shape.kind == Shape::Kind::kObject) {
        const std::string held = "reinterpret_cast<const " + stored + "*>(" + at + ")";
        read = "const_cast<void*>(static_cast<const void*>(" + (shape.smart ? held + "->Get()" : "*" + held) + "))";
        write = "static_cast<" + shape.cxx + "*>(value)";
      }
      m_Declarations += "  fun Get" + base + "() -> " + shape.wolang + Uses(false) + "\n";
      m_Glue += "extern \"C\" " + c + " " + getter + "(void* self) {\n" + offset + " {};\n\t}\n\n\treturn " + read + ";\n}\n\n";
      if (writable) {
        m_Declarations += "  fun Set" + base + "(value: " + shape.wolang + ")" + Uses(false) + "\n";
        m_Glue += "extern \"C\" void " + setter + "(void* self, " + c + " value) {\n" + offset + ";\n\t}\n\n\t*reinterpret_cast<" + stored + "*>(" + at + ") = " + write + ";\n}\n\n";
      }
    }
    m_Registrations += "\twolang_vm_bind(vm, \"" + getter + "\", reinterpret_cast<void*>(&Mowo::" + getter + "));\n";
    if (writable) {
      m_Registrations += "\twolang_vm_bind(vm, \"" + setter + "\", reinterpret_cast<void*>(&Mowo::" + setter + "));\n";
    }
  }

  // The engine's value built from the glue's: `Vec3(v->x, v->y, v->z)`.
  static std::string VectorValue(const ValueType& type, const std::string& from) {
    std::string text = type.cxx + "(";
    for (size_t i = 0; i < type.fields.size(); ++i) {
      text += (i == 0 ? "" : ", ") + from + type.fields[i].name;
    }
    return text + ")";
  }

  static std::string ValueOut(const ValueType& type) {
    std::string text;
    for (const ValueField& field : type.fields) {
      text += "\tout->" + field.name + " = value." + field.member + ";\n";
    }
    return text;
  }

  static std::string ZeroOf(const ValueType& type) {
    std::string text = type.name + "(";
    for (size_t i = 0; i < type.fields.size(); ++i) {
      const std::string& wolang = type.fields[i].wolang;
      const char* zero = "0";
      if (wolang == "f32" || wolang == "f64") {
        zero = "0.0";
      }
      else if (wolang == "bool") {
        zero = "false";
      }
      text += (i == 0 ? "" : ", ") + std::string(zero);
    }
    return text + ")";
  }

  const ReflectionOptions& m_Options;
  ReflectionResult* m_Result;
  std::vector<ClassInfo> m_Classes;
  std::vector<ClassInfo> m_StructInfos;
  std::vector<ValueType> m_Structs;
  std::map<std::string, const ClassInfo*> m_Known;
  std::map<std::string, std::string> m_Enums;
  std::map<std::string, int> m_StaticCount;
  std::set<std::string> m_UsedValues;
  std::set<std::string> m_Declared;
  bool m_UsesText = false;
  std::string m_Declarations;
  std::string m_Extensions;
  std::string m_Free;
  std::string m_Glue;
  std::string m_Registrations;
};

ReflectionResult GenerateFromReflection(const std::vector<std::string>& documents, const ReflectionOptions& options) {
  ReflectionResult result;
  std::vector<Json> parsed;
  parsed.reserve(documents.size());
  for (const std::string& document : documents) {
    parsed.push_back(Json::Parse(document));
  }

  Generator generator(options, &result);
  for (const Json& document : parsed) {
    generator.Collect(document);
  }
  generator.Write();
  return result;
}

}  // namespace bind
}  // namespace wolang
