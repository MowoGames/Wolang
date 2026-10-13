// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/compiler/database.h"

#include <string>

#include "harness/test.h"
#include "wolang/compiler/item_tree.h"

namespace wolang {
namespace {

std::string Signature(Database* db, FileId file, uint32_t entry) {
  const ParsedFile* parsed = db->Parse(file);
  if (parsed == nullptr) {
    return "<none>";
  }
  return SignatureText(db->interner(), parsed->ast,
                       ResolveItem(parsed->ast, entry));
}

WOLANG_TEST(Database, LexesAndParsesOnDemand) {
  Database db;
  const FileId file = db.AddFile("a.wo", "fn f() -> int { return 1 }\n");

  const TokenizedFile* tokens = db.Tokens(file);
  WOLANG_ASSERT_NE(tokens, nullptr);
  WOLANG_EXPECT(tokens->diagnostics.empty());
  WOLANG_EXPECT_GT(tokens->tokens.size(), size_t{5});

  const ParsedFile* parsed = db.Parse(file);
  WOLANG_ASSERT_NE(parsed, nullptr);
  WOLANG_EXPECT(parsed->diagnostics.empty());
  WOLANG_EXPECT_EQ(parsed->ast.Get(parsed->ast.root()).size(), size_t{1});
}

WOLANG_TEST(Database, RepeatedQuestionsDoNotRecompute) {
  Database db;
  const FileId file = db.AddFile("a.wo", "fn f() {}\n");
  db.Items(file);
  db.engine().ResetCounters();

  db.Items(file);
  db.Parse(file);
  db.Tokens(file);
  WOLANG_EXPECT_EQ(db.engine().total_computations(), uint64_t{0});
}

WOLANG_TEST(Database, CollectsDiagnosticsPerFile) {
  Database db;
  const FileId good = db.AddFile("good.wo", "fn f() {}\n");
  const FileId bad = db.AddFile("bad.wo", "fn f() { let a = * }\n");

  WOLANG_EXPECT(db.Diagnostics(good)->empty());
  WOLANG_ASSERT_EQ(db.Diagnostics(bad)->size(), size_t{1});
  WOLANG_EXPECT_EQ(db.Diagnostics(bad)->front().code, "E0024");
  WOLANG_EXPECT_EQ(db.AllDiagnostics().size(), size_t{1});
}

WOLANG_TEST(Database, DiagnosticsFollowAnEdit) {
  Database db;
  const FileId file = db.AddFile("a.wo", "fn f() { let a = * }\n");
  WOLANG_EXPECT_EQ(db.Diagnostics(file)->size(), size_t{1});

  db.SetFileText(file, "fn f() { let a = 1 }\n");
  WOLANG_EXPECT(db.Diagnostics(file)->empty());
}

// The exit criterion of this wave.
WOLANG_TEST(Database, EditingABodyLeavesTheItemTreeEqual) {
  Database db;
  const FileId file = db.AddFile(
      "a.wo",
      "fn Damage(base: float, mult: float) -> float {\n"
      "  return base * mult\n"
      "}\n");

  const ItemTree* before = db.Items(file);
  WOLANG_ASSERT_NE(before, nullptr);
  WOLANG_ASSERT_EQ(before->entries.size(), size_t{1});
  const ItemTree snapshot = *before;

  const Revision item_tree_changed_at =
      db.engine().ChangedAt(QueryKey{QueryKind::kItemTree, file.index});
  db.engine().ResetCounters();

  // Change the body only.
  db.SetFileText(file,
                 "fn Damage(base: float, mult: float) -> float {\n"
                 "  return base * mult * 2.0\n"
                 "}\n");

  const ItemTree* after = db.Items(file);
  WOLANG_ASSERT_NE(after, nullptr);
  WOLANG_EXPECT(*after == snapshot);

  // The file was relexed and reparsed, and the item tree was rebuilt, but it
  // came out equal -- so nothing downstream of it is stale.
  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kTokens),
                   uint64_t{1});
  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kItemTree),
                   uint64_t{1});
  WOLANG_EXPECT_EQ(
      db.engine().ChangedAt(QueryKey{QueryKind::kItemTree, file.index}),
      item_tree_changed_at);
}

WOLANG_TEST(Database, EditingASignatureChangesTheItemTree) {
  Database db;
  const FileId file = db.AddFile(
      "a.wo", "fn Damage(base: float) -> float {\n  return base\n}\n");

  const ItemTree snapshot = *db.Items(file);
  const Revision before =
      db.engine().ChangedAt(QueryKey{QueryKind::kItemTree, file.index});

  db.SetFileText(file,
                 "fn Damage(base: f64) -> float {\n  return base\n}\n");

  WOLANG_EXPECT(!(*db.Items(file) == snapshot));
  WOLANG_EXPECT_GT(
      db.engine().ChangedAt(QueryKey{QueryKind::kItemTree, file.index}),
      before);
}

WOLANG_TEST(Database, EditingOneFileDoesNotTouchAnother) {
  Database db;
  const FileId first = db.AddFile("a.wo", "fn a() {}\n");
  const FileId second = db.AddFile("b.wo", "fn b() {}\n");
  db.Items(first);
  db.Items(second);
  db.engine().ResetCounters();

  db.SetFileText(first, "fn a() { Work() }\n");
  db.Items(first);
  db.Items(second);

  WOLANG_EXPECT_EQ(db.engine().ComputationCount(QueryKind::kItemTree),
                   uint64_t{1});
}

WOLANG_TEST(Database, AddingADeclarationGrowsTheItemTree) {
  Database db;
  const FileId file = db.AddFile("a.wo", "fn a() {}\n");
  WOLANG_EXPECT_EQ(db.Items(file)->entries.size(), size_t{1});

  db.SetFileText(file, "fn a() {}\nfn b() {}\n");
  WOLANG_EXPECT_EQ(db.Items(file)->entries.size(), size_t{2});
}

WOLANG_TEST(Database, ItemTreeIncludesMembersInPreOrder) {
  Database db;
  const FileId file = db.AddFile(
      "a.wo",
      "class Turret : Actor {\n"
      "  var health: int = 100\n"
      "  fn Fire() {}\n"
      "}\n"
      "fn free() {}\n");

  const ItemTree* tree = db.Items(file);
  WOLANG_ASSERT_EQ(tree->entries.size(), size_t{4});
  WOLANG_EXPECT_EQ(tree->entries[0].kind, ItemKind::kClass);
  WOLANG_EXPECT_EQ(tree->entries[0].parent, ItemTreeEntry::kNoParent);
  WOLANG_EXPECT_EQ(tree->entries[1].kind, ItemKind::kBinding);
  WOLANG_EXPECT_EQ(tree->entries[1].parent, 0u);
  WOLANG_EXPECT_EQ(tree->entries[2].kind, ItemKind::kFunction);
  WOLANG_EXPECT_EQ(tree->entries[2].parent, 0u);
  WOLANG_EXPECT_EQ(tree->entries[3].kind, ItemKind::kFunction);
  WOLANG_EXPECT_EQ(tree->entries[3].parent, ItemTreeEntry::kNoParent);

  WOLANG_EXPECT_EQ(db.interner().Text(tree->entries[2].name), "Fire");
}

WOLANG_TEST(Database, ResolveItemFindsTheDeclarationOfAnEntry) {
  Database db;
  const FileId file = db.AddFile(
      "a.wo", "class C {\n  fn Method(x: int) {}\n}\n");

  WOLANG_EXPECT_EQ(Signature(&db, file, 0), "class C");
  WOLANG_EXPECT_EQ(Signature(&db, file, 1), "fn Method(x:int)");
  WOLANG_EXPECT_EQ(Signature(&db, file, 2), "<invalid>");
}

WOLANG_TEST(Database, NestedItemsInABodyStayOutOfTheItemTree) {
  Database db;
  const FileId file = db.AddFile(
      "a.wo", "fn outer() {\n  fn inner() {}\n  inner()\n}\n");
  // A declaration inside a body is a statement, not a member: it cannot be
  // referred to from outside, so it is not part of the file's signature.
  WOLANG_EXPECT_EQ(db.Items(file)->entries.size(), size_t{1});

  const ItemTree snapshot = *db.Items(file);
  db.SetFileText(file,
                 "fn outer() {\n  fn inner() -> int => 2\n  inner()\n}\n");
  WOLANG_EXPECT(*db.Items(file) == snapshot);
}

}  // namespace
}  // namespace wolang
