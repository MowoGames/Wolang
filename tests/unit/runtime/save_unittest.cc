// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/runtime/save.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/runtime/scheduler.h"

namespace wolang {
namespace runtime {

struct SaveTestCell {
  int64_t Steps;
  const uint8_t* Data;
  uint64_t Length;
  SaveTestCell* Next;
};

static SaveTestCell* NewCell(int64_t steps, const char* name) {
  auto* cell = static_cast<SaveTestCell*>(wolang_object_alloc(sizeof(SaveTestCell)));
  std::memset(cell, 0, sizeof(SaveTestCell));
  cell->Steps = steps;
  cell->Data = reinterpret_cast<const uint8_t*>(name);
  cell->Length = name == nullptr ? 0 : std::strlen(name);

  return cell;
}

static std::string CellSchema(const char* steps_code, bool with_name) {
  std::string schema = "WSCHEMA 1\nROOT Cell\n";
  schema += "TYPE C Cell " + std::to_string(sizeof(SaveTestCell)) + " 3\n";
  schema += std::string("FIELD steps ") + std::to_string(offsetof(SaveTestCell, Steps)) + " " + steps_code + " " + steps_code + "\n";
  if (with_name) {
    schema += "FIELD name " + std::to_string(offsetof(SaveTestCell, Data)) + " s s\n";
  }
  schema += "FIELD next " + std::to_string(offsetof(SaveTestCell, Next)) + " r:Cell r Cell\n";

  return schema;
}

static std::vector<uint8_t> Encode(const std::string& schema, SaveTestCell* root) {
  SaveRoot start;
  start.Object = root;
  start.Class = "Cell";

  std::vector<uint8_t> bytes;
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveEncode(schema, nullptr, std::span<const SaveRoot>(&start, 1), &bytes)), 0);

  return bytes;
}

static void FreeAll(const std::vector<SaveTestCell*>& cells) {
  for (SaveTestCell* cell : cells) {
    wolang_object_free(cell);
  }
}

WOLANG_TEST(RuntimeSave, RoundTripsAGraphWithASharedObjectAndACycle) {
  const std::string schema = CellSchema("i8", true);
  SaveTestCell* first = NewCell(1, "first");
  SaveTestCell* second = NewCell(2, "second");
  first->Next = second;
  second->Next = first;

  const std::vector<uint8_t> bytes = Encode(schema, first);
  std::vector<void*> roots;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveDecode(schema, nullptr, bytes, &roots)), 0);
  WOLANG_ASSERT_EQ(roots.size(), 1u);

  auto* back = static_cast<SaveTestCell*>(roots[0]);
  WOLANG_EXPECT_EQ(back->Steps, 1);
  WOLANG_EXPECT_EQ(std::string(reinterpret_cast<const char*>(back->Data), back->Length), "first");
  WOLANG_ASSERT(back->Next != nullptr);
  WOLANG_EXPECT_EQ(back->Next->Steps, 2);
  WOLANG_EXPECT_EQ(std::string(reinterpret_cast<const char*>(back->Next->Data), back->Next->Length), "second");
  WOLANG_EXPECT(back->Next->Next == back);
  WOLANG_EXPECT(back != first);

  FreeAll({first, second, back, back->Next});
}

WOLANG_TEST(RuntimeSave, WritesAnObjectOnceHoweverManyPointToIt) {
  const std::string schema = CellSchema("i8", false);
  SaveTestCell* shared = NewCell(9, nullptr);
  SaveTestCell* a = NewCell(1, nullptr);
  SaveTestCell* b = NewCell(2, nullptr);
  a->Next = shared;
  b->Next = shared;

  SaveRoot roots[2];
  roots[0].Object = a;
  roots[0].Class = "Cell";
  roots[1].Object = b;
  roots[1].Class = "Cell";

  std::vector<uint8_t> bytes;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveEncode(schema, nullptr, std::span<const SaveRoot>(roots, 2), &bytes)), 0);

  std::vector<void*> back;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveDecode(schema, nullptr, bytes, &back)), 0);
  WOLANG_ASSERT_EQ(back.size(), 2u);

  auto* first = static_cast<SaveTestCell*>(back[0]);
  auto* second = static_cast<SaveTestCell*>(back[1]);
  WOLANG_EXPECT(first->Next != nullptr);
  WOLANG_EXPECT(first->Next == second->Next);
  WOLANG_EXPECT_EQ(first->Next->Steps, 9);

  FreeAll({shared, a, b, first, second, first->Next});
}

WOLANG_TEST(RuntimeSave, DropsWhatTheReaderNoLongerHasOrHoldsUnderAnotherType) {
  SaveTestCell* cell = NewCell(7, "gone");
  const std::vector<uint8_t> bytes = Encode(CellSchema("i8", true), cell);

  std::vector<void*> without_name;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveDecode(CellSchema("i8", false), nullptr, bytes, &without_name)), 0);
  auto* kept = static_cast<SaveTestCell*>(without_name[0]);
  WOLANG_EXPECT_EQ(kept->Steps, 7);
  WOLANG_EXPECT(kept->Data == nullptr);
  WOLANG_EXPECT_EQ(kept->Length, 0u);

  std::vector<void*> retyped;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveDecode(CellSchema("i4", true), nullptr, bytes, &retyped)), 0);
  auto* zeroed = static_cast<SaveTestCell*>(retyped[0]);
  WOLANG_EXPECT_EQ(zeroed->Steps, 0);
  WOLANG_EXPECT_EQ(zeroed->Length, 4u);

  FreeAll({cell, kept, zeroed});
}

WOLANG_TEST(RuntimeSave, RejectsBytesThatEndEarlyAndAllocatesNothing) {
  const std::string schema = CellSchema("i8", true);
  SaveTestCell* first = NewCell(1, "first");
  SaveTestCell* second = NewCell(2, "second");
  first->Next = second;

  const std::vector<uint8_t> bytes = Encode(schema, first);
  const int64_t live = wolang_objects_live();
  for (size_t keep = 0; keep < bytes.size(); ++keep) {
    std::vector<void*> roots;
    const SaveStatus status = SaveDecode(schema, nullptr, std::span<const uint8_t>(bytes.data(), keep), &roots);
    WOLANG_EXPECT(status != SaveStatus::kOk);
    WOLANG_EXPECT(roots.empty());
  }
  WOLANG_EXPECT_EQ(wolang_objects_live(), live);

  std::vector<void*> whole;
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveDecode(schema, nullptr, bytes, &whole)), 0);

  FreeAll({first, second, static_cast<SaveTestCell*>(whole[0]), static_cast<SaveTestCell*>(whole[0])->Next});
}

WOLANG_TEST(RuntimeSave, RejectsAClassTheReaderDoesNotKnow) {
  SaveTestCell* cell = NewCell(1, nullptr);
  const std::vector<uint8_t> bytes = Encode(CellSchema("i8", false), cell);

  std::string other = "WSCHEMA 1\nROOT Other\nTYPE C Other 8 0\n";
  std::vector<void*> roots;
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveDecode(other, nullptr, bytes, &roots)), static_cast<int32_t>(SaveStatus::kUnknownClass));
  WOLANG_EXPECT(roots.empty());

  FreeAll({cell});
}

WOLANG_TEST(RuntimeSave, RejectsASchemaThatIsNotOne) {
  std::vector<uint8_t> bytes;
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveEncode("not a schema", nullptr, std::span<const SaveRoot>(), &bytes)), static_cast<int32_t>(SaveStatus::kBadSchema));
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveEncode("WSCHEMA 1\nROOT Cell\nTYPE C Cell 8 1\nFIELD a 0 r:Nope r Nope\n", nullptr, std::span<const SaveRoot>(), &bytes)), static_cast<int32_t>(SaveStatus::kBadSchema));
  WOLANG_EXPECT_EQ(static_cast<int32_t>(SaveEncode("WSCHEMA 1\nROOT Cell\nTYPE C Cell @0 1\n", nullptr, std::span<const SaveRoot>(), &bytes)), static_cast<int32_t>(SaveStatus::kBadSchema));
}

WOLANG_TEST(RuntimeSave, TakesItsNumbersFromTheTableWhenTheSchemaAsksForThem) {
  SaveTestCell* cell = NewCell(3, nullptr);
  const std::string spelled = CellSchema("i8", false);

  std::string asked = "WSCHEMA 1\nROOT Cell\nTYPE C Cell @0 2\nFIELD steps @1 i8 i8\nFIELD next @2 r:Cell r Cell\n";
  auto* table = static_cast<uint64_t*>(wolang_object_alloc(3 * sizeof(uint64_t)));
  table[0] = sizeof(SaveTestCell);
  table[1] = offsetof(SaveTestCell, Steps);
  table[2] = offsetof(SaveTestCell, Next);

  uint64_t length = 0;
  uint8_t* bytes = wolang_save_encode(reinterpret_cast<const uint8_t*>(asked.data()), asked.size(), table, reinterpret_cast<uint64_t>(cell), &length);
  WOLANG_ASSERT(bytes != nullptr);
  WOLANG_EXPECT(length > 0);

  std::vector<void*> roots;
  WOLANG_ASSERT_EQ(static_cast<int32_t>(SaveDecode(spelled, nullptr, std::span<const uint8_t>(bytes, length), &roots)), 0);
  WOLANG_EXPECT_EQ(static_cast<SaveTestCell*>(roots[0])->Steps, 3);

  wolang_object_free(bytes);
  FreeAll({cell, static_cast<SaveTestCell*>(roots[0])});
}

}  // namespace runtime
}  // namespace wolang
