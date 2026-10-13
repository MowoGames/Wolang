// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/cache.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "harness/test.h"

namespace wolang {
namespace {

// Renders an item tree as text. Symbols are indices into one compilation's
// interner, so two Databases number the same name differently; comparing the
// spelled-out form is what "the same tree" means across processes.
std::string Describe(const Database& db, const ItemTree& tree) {
  std::string out;
  for (const ItemTreeEntry& entry : tree.entries) {
    out.append(ItemKindName(entry.kind));
    out.push_back(' ');
    out.append(db.interner().Text(entry.name));
    out.append(" parent=");
    out.append(std::to_string(static_cast<int64_t>(entry.parent)));
    out.append(" flags=");
    out.append(std::to_string(static_cast<uint32_t>(entry.flags)));
    out.append(" sig=");
    out.append(std::to_string(entry.signature));
    out.push_back('\n');
  }
  return out;
}

namespace fs = std::filesystem;

int ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

// A scratch directory holding one cache file and the sources it describes.
class TempDir {
 public:
  explicit TempDir(std::string_view name) {
    std::error_code error;
    root_ = fs::temp_directory_path(error) /
            ("wolang-cache-" + std::to_string(ProcessId()) + "-" +
             std::string(name));
    fs::remove_all(root_, error);
    fs::create_directories(root_, error);
  }

  ~TempDir() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string Write(std::string_view relative, std::string_view text) {
    const fs::path path = root_ / relative;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return path.string();
  }

  std::string path(std::string_view relative) const {
    return (root_ / relative).string();
  }

 private:
  fs::path root_;
};

WOLANG_TEST(Cache, RoundTripsItemTrees) {
  TempDir dir("roundtrip");
  const std::string source = dir.Write(
      "a.wo",
      "offer fun Fire(target: Pawn) -> bool {\n  return true\n}\n"
      "class Turret {\n  var health: int = 100\n}\n");
  const std::string cache_path = dir.path("cache/items.wdb");

  std::string original;
  {
    Database db;
    std::string error;
    const FileId file = db.LoadFile(source, &error);
    WOLANG_ASSERT(file.is_valid());
    original = Describe(db, *db.Items(file));
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  // A fresh process: the file is registered, but nothing has been parsed.
  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);
  WOLANG_ASSERT(file.is_valid());

  CacheStats stats;
  WOLANG_ASSERT(Cache::Load(&db, cache_path, &stats));
  WOLANG_EXPECT_EQ(stats.reused, 1u);
  WOLANG_EXPECT_EQ(stats.stale, 0u);

  // The item tree is available without the file being lexed or parsed.
  const ItemTree* loaded = db.Items(file);
  WOLANG_ASSERT_NE(loaded, nullptr);
  WOLANG_EXPECT_EQ(Describe(db, *loaded), original);
  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kTokens),
                   uint64_t{0});
  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kParsedFile),
                   uint64_t{0});
  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kItemTree),
                   uint64_t{0});
}

WOLANG_TEST(Cache, PreservesNamesFlagsAndNesting) {
  TempDir dir("fields");
  const std::string source = dir.Write(
      "a.wo", "offer class C {\n  priv fun Hidden() {}\n}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(source, &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);
  WOLANG_ASSERT(Cache::Load(&db, cache_path));

  const ItemTree* tree = db.Items(file);
  WOLANG_ASSERT_EQ(tree->entries.size(), size_t{2});
  WOLANG_EXPECT_EQ(tree->entries[0].kind, ItemKind::kClass);
  WOLANG_EXPECT(HasFlag(tree->entries[0].flags, ItemFlags::kOffer));
  WOLANG_EXPECT_EQ(db.interner().Text(tree->entries[0].name), "C");
  WOLANG_EXPECT_EQ(tree->entries[1].parent, 0u);
  WOLANG_EXPECT(HasFlag(tree->entries[1].flags, ItemFlags::kPriv));
  WOLANG_EXPECT_EQ(db.interner().Text(tree->entries[1].name), "Hidden");
  WOLANG_EXPECT_NE(tree->entries[0].signature, tree->entries[1].signature);
}

WOLANG_TEST(Cache, DiscardsAnEntryWhoseFileChanged) {
  TempDir dir("changed");
  const std::string source = dir.Write("a.wo", "fun One() {}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(source, &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  // The file now says something else, so the cached tree is wrong.
  dir.Write("a.wo", "fun Two(x: int) {}\n");

  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);

  CacheStats stats;
  // Nothing was reusable, which is reported as "no cache", not as an error.
  WOLANG_EXPECT(!Cache::Load(&db, cache_path, &stats));
  WOLANG_EXPECT_EQ(stats.reused, 0u);
  WOLANG_EXPECT_EQ(stats.stale, 1u);

  // The current text is what gets used.
  const ItemTree* tree = db.Items(file);
  WOLANG_ASSERT_EQ(tree->entries.size(), size_t{1});
  WOLANG_EXPECT_EQ(db.interner().Text(tree->entries[0].name), "Two");
}

WOLANG_TEST(Cache, DiscardsAnEntryForAFileThatIsNoLongerRegistered) {
  TempDir dir("gone");
  const std::string first = dir.Write("a.wo", "fun A() {}\n");
  dir.Write("b.wo", "fun B() {}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(first, &error);
    db.LoadFile(dir.path("b.wo"), &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  // This build only knows about one of the two files.
  Database db;
  std::string error;
  db.LoadFile(first, &error);

  CacheStats stats;
  WOLANG_EXPECT(Cache::Load(&db, cache_path, &stats));
  WOLANG_EXPECT_EQ(stats.reused, 1u);
  WOLANG_EXPECT_EQ(stats.stale, 1u);
}

WOLANG_TEST(Cache, AnEditAfterLoadingStillInvalidates) {
  TempDir dir("edit");
  const std::string source = dir.Write("a.wo", "fun One() {}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(source, &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);
  WOLANG_ASSERT(Cache::Load(&db, cache_path));
  WOLANG_EXPECT_EQ(db.interner().Text(db.Items(file)->entries[0].name), "One");

  // A seeded value is not special: editing the file invalidates it like any
  // other computed value.
  db.SetFileText(file, "fun Two() {}\nfun Three() {}\n");
  const ItemTree* tree = db.Items(file);
  WOLANG_ASSERT_EQ(tree->entries.size(), size_t{2});
  WOLANG_EXPECT_EQ(db.interner().Text(tree->entries[0].name), "Two");
}

WOLANG_TEST(Cache, RejectsAMissingFile) {
  TempDir dir("missing");
  Database db;
  CacheStats stats;
  WOLANG_EXPECT(!Cache::Load(&db, dir.path("nothing.wdb"), &stats));
  WOLANG_EXPECT_EQ(stats.reused, 0u);
}

WOLANG_TEST(Cache, RejectsAFileWithTheWrongMagic) {
  TempDir dir("magic");
  const std::string path = dir.Write("items.wdb", "not a wolang cache at all");
  Database db;
  WOLANG_EXPECT(!Cache::Load(&db, path));
}

WOLANG_TEST(Cache, RejectsAnotherFormatVersion) {
  TempDir dir("version");
  std::string bytes = "WOLANGDB";
  // A version field that is not kVersion.
  const uint32_t other = Cache::kVersion + 1;
  for (int i = 0; i < 4; ++i) {
    bytes.push_back(static_cast<char>((other >> (i * 8)) & 0xFF));
  }
  bytes.append(4, '\0');
  const std::string path = dir.Write("items.wdb", bytes);

  Database db;
  WOLANG_EXPECT(!Cache::Load(&db, path));
}

WOLANG_TEST(Cache, RejectsATruncatedFile) {
  TempDir dir("truncated");
  const std::string source = dir.Write("a.wo", "fun A(x: int) -> bool {}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(source, &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  // Keep the header but cut the body, as a crash mid-write would.
  std::string bytes;
  {
    std::ifstream stream(cache_path, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(stream)),
                 std::istreambuf_iterator<char>());
  }
  dir.Write("items.wdb", std::string_view(bytes).substr(0, 16));

  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);
  Cache::Load(&db, cache_path);

  // Whatever the cache said, the database still answers correctly.
  const ItemTree* tree = db.Items(file);
  WOLANG_ASSERT_NE(tree, nullptr);
  WOLANG_EXPECT_EQ(tree->entries.size(), size_t{1});
}

WOLANG_TEST(Cache, DoesNotOverwriteFresherKnowledge) {
  TempDir dir("fresh");
  const std::string source = dir.Write("a.wo", "fun One() {}\n");
  const std::string cache_path = dir.path("items.wdb");

  {
    Database db;
    std::string error;
    db.LoadFile(source, &error);
    WOLANG_ASSERT(Cache::Save(&db, cache_path));
  }

  Database db;
  std::string error;
  const FileId file = db.LoadFile(source, &error);
  // Compute it first, so the cache has nothing to add.
  const ItemTree computed = *db.Items(file);
  Cache::Load(&db, cache_path);
  WOLANG_EXPECT(*db.Items(file) == computed);
}

WOLANG_TEST(Cache, SkipsManifestsAndCreatesItsDirectory) {
  TempDir dir("manifest");
  const std::string source = dir.Write("a.wo", "fun A() {}\n");
  const std::string manifest =
      dir.Write("wolang.toml", "[package]\nname = \"p\"\n");
  const std::string cache_path = dir.path("deep/nested/items.wdb");

  Database db;
  std::string error;
  db.LoadFile(source, &error);
  db.LoadFile(manifest, &error, FileRole::kAuxiliary);
  WOLANG_ASSERT(Cache::Save(&db, cache_path));

  Database other;
  other.LoadFile(source, &error);
  other.LoadFile(manifest, &error, FileRole::kAuxiliary);
  CacheStats stats;
  WOLANG_ASSERT(Cache::Load(&other, cache_path, &stats));
  // Only the source file was stored; the manifest is not Wolang source.
  WOLANG_EXPECT_EQ(stats.reused, 1u);
  WOLANG_EXPECT_EQ(stats.stale, 0u);
}

}  // namespace
}  // namespace wolang
