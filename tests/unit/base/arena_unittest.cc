// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/arena.h"

#include <cstdint>
#include <cstring>

#include "harness/test.h"

namespace wolang {
namespace {

bool IsAligned(const void* pointer, size_t alignment) {
  return reinterpret_cast<uintptr_t>(pointer) % alignment == 0;
}

WOLANG_TEST(Arena, AllocatesWithRequestedAlignment) {
  Arena arena;
  for (size_t alignment : {size_t{1}, size_t{2}, size_t{4}, size_t{8},
                           size_t{16}, size_t{32}, size_t{64}}) {
    // Interleave a one byte allocation so the cursor is rarely aligned.
    arena.Allocate(1, 1);
    void* pointer = arena.Allocate(alignment * 3, alignment);
    WOLANG_ASSERT_NE(pointer, nullptr);
    WOLANG_EXPECT(IsAligned(pointer, alignment));
  }
}

WOLANG_TEST(Arena, ZeroSizedAllocationReturnsUsableStorage) {
  Arena arena;
  void* first = arena.Allocate(0, 1);
  void* second = arena.Allocate(0, 1);
  WOLANG_ASSERT_NE(first, nullptr);
  WOLANG_EXPECT_NE(first, second);
}

WOLANG_TEST(Arena, NewConstructsObject) {
  struct Point {
    int x;
    int y;
  };

  Arena arena;
  Point* point = arena.New<Point>(Point{3, 4});
  WOLANG_ASSERT_NE(point, nullptr);
  WOLANG_EXPECT_EQ(point->x, 3);
  WOLANG_EXPECT_EQ(point->y, 4);
}

WOLANG_TEST(Arena, NewArrayIsZeroInitialized) {
  Arena arena;
  std::span<uint32_t> values = arena.NewArray<uint32_t>(16);
  WOLANG_ASSERT_EQ(values.size(), size_t{16});
  for (uint32_t value : values) {
    WOLANG_EXPECT_EQ(value, 0u);
  }
}

WOLANG_TEST(Arena, NewArrayOfZeroElementsIsEmpty) {
  Arena arena;
  std::span<uint32_t> values = arena.NewArray<uint32_t>(0);
  WOLANG_EXPECT(values.empty());
}

WOLANG_TEST(Arena, CopyStringIsNulTerminatedAndIndependent) {
  Arena arena;
  std::string source = "gameplay";
  std::string_view copy = arena.CopyString(source);
  source[0] = 'G';

  WOLANG_EXPECT_EQ(copy, "gameplay");
  WOLANG_EXPECT_EQ(copy.size(), size_t{8});
  WOLANG_EXPECT_EQ(copy.data()[copy.size()], '\0');
}

WOLANG_TEST(Arena, CopyStringHandlesEmptyInput) {
  Arena arena;
  std::string_view copy = arena.CopyString("");
  WOLANG_EXPECT(copy.empty());
  WOLANG_EXPECT_EQ(copy.data()[0], '\0');
}

WOLANG_TEST(Arena, GrowsPastTheFirstChunk) {
  Arena arena(1024);
  for (int i = 0; i < 100; ++i) {
    WOLANG_ASSERT_NE(arena.Allocate(64, 8), nullptr);
  }
  WOLANG_EXPECT_GT(arena.chunk_count(), size_t{1});
  WOLANG_EXPECT_GE(arena.bytes_used(), size_t{6400});
}

WOLANG_TEST(Arena, ServesAllocationsLargerThanTheChunkSize) {
  Arena arena(1024);
  void* pointer = arena.Allocate(64 * 1024, 64);
  WOLANG_ASSERT_NE(pointer, nullptr);
  WOLANG_EXPECT(IsAligned(pointer, 64));
  std::memset(pointer, 0xAB, 64 * 1024);
}

WOLANG_TEST(Arena, ResetKeepsOneChunkAndClearsUsage) {
  Arena arena(1024);
  for (int i = 0; i < 100; ++i) {
    arena.Allocate(64, 8);
  }
  arena.Reset();
  WOLANG_EXPECT_EQ(arena.bytes_used(), size_t{0});
  WOLANG_EXPECT_EQ(arena.chunk_count(), size_t{1});
  WOLANG_EXPECT_NE(arena.Allocate(16, 8), nullptr);
}

WOLANG_TEST(Arena, MoveTransfersOwnership) {
  Arena source(1024);
  std::string_view text = source.CopyString("transferred");
  const size_t used = source.bytes_used();

  Arena target(std::move(source));
  WOLANG_EXPECT_EQ(target.bytes_used(), used);
  WOLANG_EXPECT_EQ(text, "transferred");
  WOLANG_EXPECT_EQ(source.bytes_used(), size_t{0});
}

}  // namespace
}  // namespace wolang
