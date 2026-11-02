// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/abi/layout.h"

#include <string>

#include "harness/test.h"
#include "wolang/compiler/database.h"
#include "wolang/sema/resolution.h"

namespace wolang {
namespace {

static uint32_t ObjectSize(const char* source, const char* name) {
  Database db;
  const FileId file = db.AddFile("<test>", std::string(source));
  const CheckedFile* checked = db.Checked(file);
  if (checked == nullptr) {
    return 0xFFFFFFFFu;
  }

  const Binding* binding = checked->resolution.Find(db.interner().Intern(name));
  if (binding == nullptr) {
    return 0xFFFFFFFEu;
  }

  abi::LayoutStore layouts(db.types(), checked->resolution);
  return layouts.ObjectOf(binding->type).size;
}

WOLANG_TEST(LayoutTest, AnOwnedTextFieldOfAClassTakesTwoWords) {
  WOLANG_EXPECT_EQ(ObjectSize("class Holder {\n  var label: Text\n}\n", "Holder"), 16u);
}

WOLANG_TEST(LayoutTest, ATextFieldBesideANumberKeepsBothInTheObject) {
  WOLANG_EXPECT_EQ(ObjectSize("class Holder {\n  var id: int\n  var label: Text\n}\n", "Holder"), 24u);
}

WOLANG_TEST(LayoutTest, ABorrowedTextFieldOfAClassTakesTwoWords) {
  WOLANG_EXPECT_EQ(ObjectSize("class Holder {\n  var label: text\n}\n", "Holder"), 16u);
}

}  // namespace
}  // namespace wolang
