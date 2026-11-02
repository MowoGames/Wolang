// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/save.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "wolang/runtime/scheduler.h"
#include "wolang/runtime/text.h"

namespace wolang {
namespace runtime {

// What one field holds, spelled the way the schema does: a leaf, an optional
// around another shape, a reference to a class or a struct stored inline.
struct SaveShape {
  // i, u, f, b, c, e, s (leaves); o (optional); r (class); v (struct).
  char Kind = 0;
  uint32_t Size = 0;
  uint64_t PayloadOffset = 0;
  std::string Name;
  std::unique_ptr<SaveShape> Inner;
};

struct SaveField {
  std::string Name;
  uint64_t Offset = 0;
  std::string Wire;
  SaveShape Shape;
};

struct SaveType {
  std::string Name;
  bool IsClass = false;
  uint64_t Size = 0;
  std::vector<SaveField> Fields;
};

struct SaveSchemaText {
  std::vector<SaveType> Types;
  std::unordered_map<std::string, uint32_t> ByName;
  std::string Root;

  const SaveType* Find(std::string_view name) const {
    const auto found = ByName.find(std::string(name));

    return found == ByName.end() ? nullptr : &Types[found->second];
  }
};

static constexpr uint32_t kSaveVersion = 1;
static constexpr size_t kMaxName = 0xFFFF;
static constexpr int kMaxDepth = 16;

static std::vector<std::string_view> SplitWords(std::string_view line) {
  std::vector<std::string_view> words;
  size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
      ++at;
    }

    const size_t start = at;
    while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') {
      ++at;
    }
    if (at > start) {
      words.push_back(line.substr(start, at - start));
    }
  }

  return words;
}

static bool ParseNumber(std::string_view word, const uint64_t* offsets, uint64_t* out) {
  if (word.empty()) {
    return false;
  }
  if (word[0] == '@') {
    uint64_t index = 0;
    const auto parsed = std::from_chars(word.data() + 1, word.data() + word.size(), index);
    if (offsets == nullptr || parsed.ec != std::errc() || parsed.ptr != word.data() + word.size()) {
      return false;
    }
    *out = offsets[index];

    return true;
  }

  const auto parsed = std::from_chars(word.data(), word.data() + word.size(), *out);

  return parsed.ec == std::errc() && parsed.ptr == word.data() + word.size();
}

static bool ParseShape(const std::vector<std::string_view>& words, size_t* at, const uint64_t* offsets, SaveShape* shape, int depth) {
  if (*at >= words.size() || depth > kMaxDepth) {
    return false;
  }

  const std::string_view word = words[(*at)++];
  if (word == "i1" || word == "u1") {
    shape->Kind = word[0];
    shape->Size = 1;
  } else if (word == "i2" || word == "u2") {
    shape->Kind = word[0];
    shape->Size = 2;
  } else if (word == "i4" || word == "u4") {
    shape->Kind = word[0];
    shape->Size = 4;
  } else if (word == "i8" || word == "u8") {
    shape->Kind = word[0];
    shape->Size = 8;
  } else if (word == "f4") {
    shape->Kind = 'f';
    shape->Size = 4;
  } else if (word == "f8") {
    shape->Kind = 'f';
    shape->Size = 8;
  } else if (word == "b") {
    shape->Kind = 'b';
    shape->Size = 1;
  } else if (word == "c") {
    shape->Kind = 'c';
    shape->Size = 4;
  } else if (word == "e") {
    shape->Kind = 'e';
    shape->Size = 4;
  } else if (word == "s") {
    shape->Kind = 's';
    shape->Size = 16;
  } else if (word == "o") {
    shape->Kind = 'o';
    if (*at >= words.size() || !ParseNumber(words[(*at)++], offsets, &shape->PayloadOffset)) {
      return false;
    }
    shape->Inner = std::make_unique<SaveShape>();

    return ParseShape(words, at, offsets, shape->Inner.get(), depth + 1);
  } else if (word == "r" || word == "v") {
    shape->Kind = word[0];
    if (*at >= words.size()) {
      return false;
    }
    shape->Name = std::string(words[(*at)++]);
  } else {
    return false;
  }

  return true;
}

static bool ParseSchema(std::string_view text, const uint64_t* offsets, SaveSchemaText* schema) {
  size_t line_start = 0;
  bool header = false;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }

    const std::vector<std::string_view> words = SplitWords(text.substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (words.empty()) {
      continue;
    }

    if (!header) {
      if (words.size() != 2 || words[0] != "WSCHEMA" || words[1] != "1") {
        return false;
      }
      header = true;
      continue;
    }

    if (words[0] == "ROOT") {
      if (words.size() != 2) {
        return false;
      }
      schema->Root = std::string(words[1]);
    } else if (words[0] == "TYPE") {
      if (words.size() != 5 || (words[1] != "C" && words[1] != "S")) {
        return false;
      }

      SaveType type;
      type.IsClass = words[1] == "C";
      type.Name = std::string(words[2]);
      if (type.Name.size() > kMaxName || !ParseNumber(words[3], offsets, &type.Size) || schema->ByName.count(type.Name) != 0) {
        return false;
      }
      schema->ByName[type.Name] = static_cast<uint32_t>(schema->Types.size());
      schema->Types.push_back(std::move(type));
    } else if (words[0] == "FIELD") {
      if (schema->Types.empty() || words.size() < 5) {
        return false;
      }

      SaveField field;
      field.Name = std::string(words[1]);
      field.Wire = std::string(words[3]);
      if (field.Name.size() > kMaxName || field.Wire.size() > kMaxName || !ParseNumber(words[2], offsets, &field.Offset)) {
        return false;
      }

      size_t at = 4;
      if (!ParseShape(words, &at, offsets, &field.Shape, 0) || at != words.size()) {
        return false;
      }
      schema->Types.back().Fields.push_back(std::move(field));
    } else {
      return false;
    }
  }

  return header;
}

static bool ShapeIsClosed(const SaveSchemaText& schema, const SaveShape& shape, int depth) {
  if (depth > kMaxDepth) {
    return false;
  }
  if (shape.Kind == 'o') {
    return ShapeIsClosed(schema, *shape.Inner, depth + 1);
  }
  if (shape.Kind == 'r' || shape.Kind == 'v') {
    const SaveType* type = schema.Find(shape.Name);

    return type != nullptr && type->IsClass == (shape.Kind == 'r');
  }

  return true;
}

static bool LoadSchema(std::string_view text, const uint64_t* offsets, SaveSchemaText* schema) {
  if (!ParseSchema(text, offsets, schema)) {
    return false;
  }

  for (const SaveType& type : schema->Types) {
    for (const SaveField& field : type.Fields) {
      if (!ShapeIsClosed(*schema, field.Shape, 0)) {
        return false;
      }
    }
  }

  return true;
}

static uint64_t HashText(std::string_view text) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char letter : text) {
    hash ^= static_cast<uint8_t>(letter);
    hash *= 0x100000001b3ull;
  }

  return hash;
}

static void PutU8(std::vector<uint8_t>* out, uint8_t value) {
  out->push_back(value);
}

static void PutU16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value));
  out->push_back(static_cast<uint8_t>(value >> 8));
}

static void PutU32(std::vector<uint8_t>* out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<uint8_t>(value >> shift));
  }
}

static void PutU64(std::vector<uint8_t>* out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<uint8_t>(value >> shift));
  }
}

static void PutName(std::vector<uint8_t>* out, const std::string& name) {
  PutU16(out, static_cast<uint16_t>(name.size()));
  out->insert(out->end(), name.begin(), name.end());
}

class SaveEncoder {
 public:
  explicit SaveEncoder(const SaveSchemaText& schema) : m_Schema(schema) {}

  SaveStatus Run(std::string_view schema_text, std::span<const SaveRoot> roots, std::vector<uint8_t>* out) {
    std::vector<uint32_t> root_ids;
    for (const SaveRoot& root : roots) {
      const SaveType* type = m_Schema.Find(root.Class);
      if (type == nullptr || !type->IsClass) {
        return SaveStatus::kBadSchema;
      }
      root_ids.push_back(Intern(static_cast<const uint8_t*>(root.Object), type));
    }

    std::vector<uint8_t> body;
    for (size_t i = 0; i < m_Objects.size(); ++i) {
      const Pending pending = m_Objects[i];
      PutName(&body, pending.Type->Name);
      if (!EncodeFields(*pending.Type, pending.Object, &body)) {
        return SaveStatus::kCorrupt;
      }
    }

    out->clear();
    const char magic[4] = {'W', 'S', 'A', 'V'};
    out->insert(out->end(), magic, magic + 4);
    PutU32(out, kSaveVersion);
    PutU64(out, HashText(schema_text));
    PutU32(out, static_cast<uint32_t>(root_ids.size()));
    for (uint32_t id : root_ids) {
      PutU32(out, id);
    }
    PutU32(out, static_cast<uint32_t>(m_Objects.size()));
    out->insert(out->end(), body.begin(), body.end());

    return SaveStatus::kOk;
  }

 private:
  struct Pending {
    const uint8_t* Object = nullptr;
    const SaveType* Type = nullptr;
  };

  uint32_t Intern(const uint8_t* object, const SaveType* type) {
    if (object == nullptr) {
      return 0;
    }

    const auto found = m_Ids.find(object);
    if (found != m_Ids.end()) {
      return found->second;
    }

    Pending pending;
    pending.Object = object;
    pending.Type = type;
    m_Objects.push_back(pending);
    const uint32_t id = static_cast<uint32_t>(m_Objects.size());
    m_Ids[object] = id;

    return id;
  }

  bool EncodeFields(const SaveType& type, const uint8_t* base, std::vector<uint8_t>* out) {
    PutU32(out, static_cast<uint32_t>(type.Fields.size()));
    for (const SaveField& field : type.Fields) {
      PutName(out, field.Name);
      PutName(out, field.Wire);

      const size_t length_at = out->size();
      PutU32(out, 0);
      if (!EncodeValue(field.Shape, base + field.Offset, out, 0)) {
        return false;
      }

      const size_t length = out->size() - length_at - 4;
      if (length > 0xFFFFFFFFull) {
        return false;
      }
      for (int shift = 0; shift < 32; shift += 8) {
        (*out)[length_at + shift / 8] = static_cast<uint8_t>(length >> shift);
      }
    }

    return true;
  }

  bool EncodeValue(const SaveShape& shape, const uint8_t* at, std::vector<uint8_t>* out, int depth) {
    if (depth > kMaxDepth) {
      return false;
    }

    switch (shape.Kind) {
      case 'i':
      case 'u':
      case 'f':
      case 'b':
      case 'c':
      case 'e':
        out->insert(out->end(), at, at + shape.Size);
        return true;
      case 's': {
        const uint8_t* data = nullptr;
        uint64_t length = 0;
        std::memcpy(&data, at, sizeof(data));
        std::memcpy(&length, at + 8, sizeof(length));
        if (length > 0xFFFFFFFFull || (length != 0 && data == nullptr)) {
          return false;
        }
        PutU32(out, static_cast<uint32_t>(length));
        if (length != 0) {
          out->insert(out->end(), data, data + length);
        }

        return true;
      }
      case 'o': {
        const bool present = at[0] != 0;
        PutU8(out, present ? 1 : 0);

        return !present || EncodeValue(*shape.Inner, at + shape.PayloadOffset, out, depth + 1);
      }
      case 'r': {
        const uint8_t* target = nullptr;
        std::memcpy(&target, at, sizeof(target));
        PutU32(out, Intern(target, m_Schema.Find(shape.Name)));

        return true;
      }
      case 'v': {
        const SaveType* type = m_Schema.Find(shape.Name);

        return type != nullptr && EncodeFields(*type, at, out);
      }
      default:
        return false;
    }
  }

  const SaveSchemaText& m_Schema;
  std::vector<Pending> m_Objects;
  std::unordered_map<const uint8_t*, uint32_t> m_Ids;
};

class SaveReader {
 public:
  SaveReader(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

  bool Ok() const { return m_Ok; }
  size_t Left() const { return m_Size - m_Position; }
  size_t Position() const { return m_Position; }
  const uint8_t* Here() const { return m_Data + m_Position; }

  uint8_t U8() {
    if (!Need(1)) {
      return 0;
    }

    return m_Data[m_Position++];
  }

  uint16_t U16() {
    if (!Need(2)) {
      return 0;
    }
    const uint16_t value = static_cast<uint16_t>(m_Data[m_Position] | (m_Data[m_Position + 1] << 8));
    m_Position += 2;

    return value;
  }

  uint32_t U32() {
    if (!Need(4)) {
      return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<uint32_t>(m_Data[m_Position + i]) << (8 * i);
    }
    m_Position += 4;

    return value;
  }

  std::string Name() {
    const uint16_t length = U16();
    if (!Need(length)) {
      return std::string();
    }
    std::string name(reinterpret_cast<const char*>(m_Data + m_Position), length);
    m_Position += length;

    return name;
  }

  bool Skip(size_t count) {
    if (!Need(count)) {
      return false;
    }
    m_Position += count;

    return true;
  }

  void Fail() { m_Ok = false; }

 private:
  bool Need(size_t count) {
    if (!m_Ok || count > m_Size - m_Position) {
      m_Ok = false;

      return false;
    }

    return true;
  }

  const uint8_t* m_Data;
  size_t m_Size;
  size_t m_Position = 0;
  bool m_Ok = true;
};

class SaveDecoder {
 public:
  SaveDecoder(const SaveSchemaText& schema, std::span<const uint8_t> data) : m_Schema(schema), m_Data(data) {}

  SaveStatus Run(std::vector<void*>* roots) {
    SaveReader header(m_Data.data(), m_Data.size());
    const uint8_t* magic = header.Here();
    if (!header.Skip(4) || std::memcmp(magic, "WSAV", 4) != 0 || header.U32() != kSaveVersion) {
      return SaveStatus::kCorrupt;
    }
    header.Skip(8);

    const uint32_t root_count = header.U32();
    if (!header.Ok() || static_cast<uint64_t>(root_count) * 4 > header.Left()) {
      return SaveStatus::kCorrupt;
    }
    for (uint32_t i = 0; i < root_count; ++i) {
      m_RootIds.push_back(header.U32());
    }

    const uint32_t object_count = header.U32();
    if (!header.Ok() || static_cast<uint64_t>(object_count) * 6 > header.Left()) {
      return SaveStatus::kCorrupt;
    }
    for (uint32_t id : m_RootIds) {
      if (id > object_count) {
        return SaveStatus::kCorrupt;
      }
    }

    m_Records.resize(object_count);
    for (uint32_t i = 0; i < object_count; ++i) {
      const std::string name = header.Name();
      if (!header.Ok()) {
        return SaveStatus::kCorrupt;
      }

      const SaveType* type = m_Schema.Find(name);
      if (type == nullptr || !type->IsClass) {
        return SaveStatus::kUnknownClass;
      }
      m_Records[i].Type = type;
      m_Records[i].FieldsAt = header.Position();
      if (!SkipFields(&header)) {
        return SaveStatus::kCorrupt;
      }
    }
    if (header.Left() != 0) {
      return SaveStatus::kCorrupt;
    }

    std::vector<uint32_t> work;
    for (uint32_t id : m_RootIds) {
      Reach(id, &work);
    }
    while (!work.empty()) {
      const uint32_t id = work.back();
      work.pop_back();
      SaveReader reader(m_Data.data(), m_Data.size());
      reader.Skip(m_Records[id - 1].FieldsAt);
      if (!ApplyFields(*m_Records[id - 1].Type, &reader, nullptr, &work) || !reader.Ok()) {
        return SaveStatus::kCorrupt;
      }
    }

    for (uint32_t i = 0; i < object_count; ++i) {
      if (!m_Records[i].Reached) {
        continue;
      }

      const uint64_t size = m_Records[i].Type->Size == 0 ? 1 : m_Records[i].Type->Size;
      void* object = wolang_object_alloc(size);
      if (object == nullptr) {
        Release(i);

        return SaveStatus::kCorrupt;
      }
      std::memset(object, 0, size);
      m_Records[i].Object = static_cast<uint8_t*>(object);
    }
    for (uint32_t i = 0; i < object_count; ++i) {
      if (!m_Records[i].Reached) {
        continue;
      }

      SaveReader reader(m_Data.data(), m_Data.size());
      reader.Skip(m_Records[i].FieldsAt);
      ApplyFields(*m_Records[i].Type, &reader, m_Records[i].Object, nullptr);
    }

    std::vector<bool> is_root(object_count, false);
    for (uint32_t id : m_RootIds) {
      if (id == 0) {
        roots->push_back(nullptr);
        continue;
      }
      if (is_root[id - 1]) {
        wolang_object_retain(m_Records[id - 1].Object);
      }
      is_root[id - 1] = true;
      roots->push_back(m_Records[id - 1].Object);
    }
    for (uint32_t i = 0; i < object_count; ++i) {
      if (m_Records[i].Reached && !is_root[i]) {
        wolang_object_release(m_Records[i].Object);
      }
    }

    return SaveStatus::kOk;
  }

 private:
  struct Record {
    const SaveType* Type = nullptr;
    size_t FieldsAt = 0;
    bool Reached = false;
    uint8_t* Object = nullptr;
  };

  void Release(uint32_t upto) {
    for (uint32_t i = 0; i < upto; ++i) {
      if (m_Records[i].Object != nullptr) {
        wolang_object_free(m_Records[i].Object);
        m_Records[i].Object = nullptr;
      }
    }
  }

  void Reach(uint32_t id, std::vector<uint32_t>* work) {
    if (id == 0 || m_Records[id - 1].Reached) {
      return;
    }
    m_Records[id - 1].Reached = true;
    work->push_back(id);
  }

  bool SkipFields(SaveReader* reader) {
    const uint32_t count = reader->U32();
    for (uint32_t i = 0; i < count && reader->Ok(); ++i) {
      reader->Name();
      reader->Name();
      const uint32_t length = reader->U32();
      reader->Skip(length);
    }

    return reader->Ok();
  }

  const SaveField* FindField(const SaveType& type, const std::string& name, const std::string& wire) const {
    for (const SaveField& field : type.Fields) {
      if (field.Name == name && field.Wire == wire) {
        return &field;
      }
    }

    return nullptr;
  }

  bool ApplyFields(const SaveType& type, SaveReader* reader, uint8_t* base, std::vector<uint32_t>* work) {
    const uint32_t count = reader->U32();
    for (uint32_t i = 0; i < count; ++i) {
      const std::string name = reader->Name();
      const std::string wire = reader->Name();
      const uint32_t length = reader->U32();
      if (!reader->Ok() || length > reader->Left()) {
        return false;
      }

      const SaveField* field = FindField(type, name, wire);
      if (field == nullptr) {
        reader->Skip(length);
        continue;
      }

      SaveReader value(reader->Here(), length);
      if (!ApplyValue(field->Shape, &value, base == nullptr ? nullptr : base + field->Offset, work, 0) || !value.Ok() || value.Left() != 0) {
        return false;
      }
      reader->Skip(length);
    }

    return reader->Ok();
  }

  bool ApplyValue(const SaveShape& shape, SaveReader* reader, uint8_t* at, std::vector<uint32_t>* work, int depth) {
    if (depth > kMaxDepth) {
      return false;
    }

    switch (shape.Kind) {
      case 'i':
      case 'u':
      case 'f':
      case 'b':
      case 'c':
      case 'e': {
        const uint8_t* bytes = reader->Here();
        if (!reader->Skip(shape.Size)) {
          return false;
        }
        if (at != nullptr) {
          std::memcpy(at, bytes, shape.Size);
        }

        return true;
      }
      case 's': {
        const uint32_t length = reader->U32();
        const uint8_t* bytes = reader->Here();
        if (!reader->Skip(length)) {
          return false;
        }
        if (at != nullptr && length != 0) {
          const Text* made = wolang_text_concat(bytes, length, nullptr, 0);
          if (made != nullptr) {
            std::memcpy(at, &made->data, sizeof(made->data));
            std::memcpy(at + 8, &made->length, sizeof(made->length));
          }
        }

        return true;
      }
      case 'o': {
        const uint8_t present = reader->U8();
        if (!reader->Ok() || present > 1) {
          return false;
        }
        if (at != nullptr) {
          at[0] = present;
        }

        return present == 0 || ApplyValue(*shape.Inner, reader, at == nullptr ? nullptr : at + shape.PayloadOffset, work, depth + 1);
      }
      case 'r': {
        const uint32_t id = reader->U32();
        if (!reader->Ok() || id > m_Records.size()) {
          return false;
        }
        if (id == 0) {
          return true;
        }
        if (at == nullptr) {
          Reach(id, work);

          return true;
        }

        uint8_t* target = m_Records[id - 1].Object;
        std::memcpy(at, &target, sizeof(target));
        wolang_object_retain(target);

        return true;
      }
      case 'v': {
        const SaveType* type = m_Schema.Find(shape.Name);

        return type != nullptr && ApplyFields(*type, reader, at, work);
      }
      default:
        return false;
    }
  }

  const SaveSchemaText& m_Schema;
  std::span<const uint8_t> m_Data;
  std::vector<uint32_t> m_RootIds;
  std::vector<Record> m_Records;
};

SaveStatus SaveEncode(std::string_view schema, const uint64_t* offsets, std::span<const SaveRoot> roots, std::vector<uint8_t>* out) {
  SaveSchemaText parsed;
  if (!LoadSchema(schema, offsets, &parsed)) {
    return SaveStatus::kBadSchema;
  }

  SaveEncoder encoder(parsed);

  return encoder.Run(schema, roots, out);
}

SaveStatus SaveDecode(std::string_view schema, const uint64_t* offsets, std::span<const uint8_t> data, std::vector<void*>* roots) {
  SaveSchemaText parsed;
  if (!LoadSchema(schema, offsets, &parsed)) {
    return SaveStatus::kBadSchema;
  }

  SaveDecoder decoder(parsed, data);

  return decoder.Run(roots);
}

// The table `ShapeOf` built is the call's to give back: nothing else holds it
// once the schema has been read, and a caller that had to free it would have
// to say how (D266).
static void FreeOffsets(const uint64_t* offsets) {
  if (offsets != nullptr) {
    wolang_object_free(const_cast<uint64_t*>(offsets));
  }
}

extern "C" {

uint8_t* wolang_save_encode(const uint8_t* schema, uint64_t schema_length, const uint64_t* offsets, uint64_t root, uint64_t* out_length) {
  *out_length = 0;
  const std::string_view text(reinterpret_cast<const char*>(schema), schema_length);

  SaveSchemaText parsed;
  const bool read = LoadSchema(text, offsets, &parsed);
  FreeOffsets(offsets);
  if (!read) {
    return nullptr;
  }

  SaveRoot start;
  start.Object = reinterpret_cast<void*>(root);
  start.Class = parsed.Root;

  std::vector<uint8_t> bytes;
  SaveEncoder encoder(parsed);
  if (encoder.Run(text, std::span<const SaveRoot>(&start, 1), &bytes) != SaveStatus::kOk) {
    return nullptr;
  }

  auto* made = static_cast<uint8_t*>(wolang_object_alloc(bytes.size()));
  if (made == nullptr) {
    return nullptr;
  }
  std::memcpy(made, bytes.data(), bytes.size());
  *out_length = bytes.size();

  return made;
}

uint64_t wolang_save_decode(const uint8_t* schema, uint64_t schema_length, const uint64_t* offsets, const uint8_t* data, uint64_t data_length, int32_t* status) {
  const std::string_view text(reinterpret_cast<const char*>(schema), schema_length);

  std::vector<void*> roots;
  const SaveStatus made = SaveDecode(text, offsets, std::span<const uint8_t>(data, data_length), &roots);
  FreeOffsets(offsets);
  *status = static_cast<int32_t>(made);
  if (made != SaveStatus::kOk) {
    return 0;
  }
  if (roots.size() != 1 || roots[0] == nullptr) {
    *status = static_cast<int32_t>(SaveStatus::kCorrupt);

    return 0;
  }

  return reinterpret_cast<uint64_t>(roots[0]);
}

}  // extern "C"

}  // namespace runtime
}  // namespace wolang
