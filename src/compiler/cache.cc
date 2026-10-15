// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/compiler/cache.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace wolang {
namespace {

constexpr char kMagic[8] = {'W', 'O', 'L', 'A', 'N', 'G', 'D', 'B'};

// A little-endian byte writer. Hand-rolled rather than memcpy of structs: the
// cache has to be readable by a compiler built with different padding or
// struct layout decisions, and a byte-exact format is also what makes the
// version check meaningful.
class Writer {
 public:
  void U8(uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

  void U32(uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      U8(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
  }

  void U64(uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      U8(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
  }

  void Text(std::string_view text) {
    U32(static_cast<uint32_t>(text.size()));
    bytes_.append(text);
  }

  void Raw(const char* data, size_t size) { bytes_.append(data, size); }

  const std::string& bytes() const { return bytes_; }

 private:
  std::string bytes_;
};

class Reader {
 public:
  explicit Reader(std::string bytes) : bytes_(std::move(bytes)) {}

  bool ok() const { return ok_; }

  bool Raw(char* out, size_t size) {
    if (!ok_ || position_ + size > bytes_.size()) {
      ok_ = false;
      return false;
    }
    std::memcpy(out, bytes_.data() + position_, size);
    position_ += size;
    return true;
  }

  uint8_t U8() {
    if (!ok_ || position_ >= bytes_.size()) {
      ok_ = false;
      return 0;
    }
    return static_cast<uint8_t>(bytes_[position_++]);
  }

  uint32_t U32() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<uint32_t>(U8()) << (i * 8);
    }
    return value;
  }

  uint64_t U64() {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<uint64_t>(U8()) << (i * 8);
    }
    return value;
  }

  std::string Text() {
    const uint32_t size = U32();
    if (!ok_ || position_ + size > bytes_.size()) {
      ok_ = false;
      return std::string();
    }
    std::string result(bytes_.data() + position_, size);
    position_ += size;
    return result;
  }

  bool at_end() const { return position_ >= bytes_.size(); }

 private:
  std::string bytes_;
  size_t position_ = 0;
  bool ok_ = true;
};

uint64_t HashCacheText(std::string_view text) {
  uint64_t hash = 0xcbf29ce484222325;
  for (char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3;
  }
  return hash;
}

}  // namespace

bool Cache::Save(Database* database, std::string_view path) {
  Writer writer;
  writer.Raw(kMagic, sizeof(kMagic));
  writer.U32(kVersion);

  // Collect first so the entry count is known before anything is written.
  struct Entry {
    std::string path;
    uint64_t hash = 0;
    uint32_t size = 0;
    const ItemTree* tree = nullptr;
  };
  std::vector<Entry> entries;

  for (uint32_t i = 0; i < database->file_count(); ++i) {
    const FileId file{i};
    if (database->RoleOf(file) != FileRole::kSource) {
      continue;
    }
    const ItemTree* tree = database->Items(file);
    if (tree == nullptr) {
      continue;
    }
    const std::string_view text = database->sources().Text(file);
    entries.push_back(Entry{std::string(database->sources().Path(file)),
                            HashCacheText(text),
                            static_cast<uint32_t>(text.size()), tree});
  }

  writer.U32(static_cast<uint32_t>(entries.size()));
  for (const Entry& entry : entries) {
    writer.Text(entry.path);
    writer.U64(entry.hash);
    writer.U32(entry.size);

    writer.U32(static_cast<uint32_t>(entry.tree->entries.size()));
    for (const ItemTreeEntry& item : entry.tree->entries) {
      writer.U8(static_cast<uint8_t>(item.kind));
      writer.U32(static_cast<uint32_t>(item.flags));
      // Symbols are indices into a per-compilation interner, so the text is
      // what is stored and the symbol is re-interned on load.
      writer.Text(database->interner().Text(item.name));
      writer.U32(item.parent);
      writer.U64(item.signature);
    }
  }

  std::error_code error;
  const std::filesystem::path file_path(path);
  if (file_path.has_parent_path()) {
    std::filesystem::create_directories(file_path.parent_path(), error);
  }

  std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  const std::string& bytes = writer.bytes();
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return stream.good();
}

bool Cache::Load(Database* database,
                 std::string_view path,
                 CacheStats* stats) {
  std::ifstream stream(std::filesystem::path(path), std::ios::binary);
  if (!stream.is_open()) {
    return false;
  }
  std::string bytes((std::istreambuf_iterator<char>(stream)),
                    std::istreambuf_iterator<char>());
  Reader reader(std::move(bytes));

  char magic[sizeof(kMagic)] = {};
  if (!reader.Raw(magic, sizeof(magic)) ||
      std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  if (reader.U32() != kVersion) {
    // A cache from another compiler build is discarded, not misread.
    return false;
  }

  const uint32_t count = reader.U32();
  if (!reader.ok()) {
    return false;
  }

  CacheStats local;
  for (uint32_t i = 0; i < count && reader.ok(); ++i) {
    const std::string file_path = reader.Text();
    const uint64_t hash = reader.U64();
    const uint32_t size = reader.U32();

    ItemTree tree;
    const uint32_t entry_count = reader.U32();
    if (!reader.ok()) {
      break;
    }
    tree.entries.reserve(entry_count);
    for (uint32_t j = 0; j < entry_count && reader.ok(); ++j) {
      ItemTreeEntry entry;
      entry.kind = static_cast<ItemKind>(reader.U8());
      entry.flags = static_cast<ItemFlags>(reader.U32());
      const std::string name = reader.Text();
      entry.name = name.empty() ? Symbol{}
                                : database->interner().Intern(name);
      entry.parent = reader.U32();
      entry.signature = reader.U64();
      tree.entries.push_back(entry);
    }
    if (!reader.ok()) {
      break;
    }

    // The entry is only usable if the file is still exactly what it was
    // computed from.
    const FileId file = database->FindFile(file_path);
    if (!file.is_valid()) {
      ++local.stale;
      continue;
    }
    const std::string_view text = database->sources().Text(file);
    if (text.size() != size || HashCacheText(text) != hash) {
      ++local.stale;
      continue;
    }

    database->SeedItemTree(file, std::move(tree));
    ++local.reused;
  }

  if (stats != nullptr) {
    *stats = local;
  }
  return reader.ok() && local.reused > 0;
}

}  // namespace wolang
