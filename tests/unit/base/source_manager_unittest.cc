// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/source_manager.h"

#include <string>

#include "harness/test.h"

namespace wolang {
namespace {

WOLANG_TEST(SourceManager, RegistersFilesWithStableIds) {
  SourceManager sources;
  const FileId first = sources.AddVirtualFile("a.wo", "const a = 1\n");
  const FileId second = sources.AddVirtualFile("b.wo", "const b = 2\n");

  WOLANG_EXPECT(first.is_valid());
  WOLANG_EXPECT_NE(first, second);
  WOLANG_EXPECT_EQ(sources.file_count(), size_t{2});
  WOLANG_EXPECT_EQ(sources.Path(first), "a.wo");
  WOLANG_EXPECT_EQ(sources.Text(second), "const b = 2\n");
}

WOLANG_TEST(SourceManager, InvalidFileQueriesAreEmpty) {
  SourceManager sources;
  const FileId invalid;
  WOLANG_EXPECT(!invalid.is_valid());
  WOLANG_EXPECT(sources.Text(invalid).empty());
  WOLANG_EXPECT(sources.Path(invalid).empty());
  WOLANG_EXPECT_EQ(sources.LineCount(invalid), 0u);
}

WOLANG_TEST(SourceManager, PositionMapsOffsetsToLineAndColumn) {
  SourceManager sources;
  //                                   0123 4567 89
  const FileId file = sources.AddVirtualFile("x.wo", "abc\ndef\ngh");

  WOLANG_EXPECT_EQ(sources.Position(file, 0).line, 1u);
  WOLANG_EXPECT_EQ(sources.Position(file, 0).column, 1u);
  WOLANG_EXPECT_EQ(sources.Position(file, 2).column, 3u);
  // The newline itself belongs to the line it terminates.
  WOLANG_EXPECT_EQ(sources.Position(file, 3).line, 1u);
  WOLANG_EXPECT_EQ(sources.Position(file, 4).line, 2u);
  WOLANG_EXPECT_EQ(sources.Position(file, 4).column, 1u);
  WOLANG_EXPECT_EQ(sources.Position(file, 9).line, 3u);
  WOLANG_EXPECT_EQ(sources.Position(file, 9).column, 2u);
}

WOLANG_TEST(SourceManager, PositionClampsPastEndOfFile) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("x.wo", "ab\n");
  const LineColumn position = sources.Position(file, 1000);
  WOLANG_EXPECT_EQ(position.line, 2u);
  WOLANG_EXPECT_EQ(position.column, 1u);
}

WOLANG_TEST(SourceManager, LineCountCountsTrailingPartialLine) {
  SourceManager sources;
  WOLANG_EXPECT_EQ(sources.LineCount(sources.AddVirtualFile("a", "")), 1u);
  WOLANG_EXPECT_EQ(sources.LineCount(sources.AddVirtualFile("b", "x")), 1u);
  WOLANG_EXPECT_EQ(sources.LineCount(sources.AddVirtualFile("c", "x\n")), 2u);
  WOLANG_EXPECT_EQ(sources.LineCount(sources.AddVirtualFile("d", "x\ny")), 2u);
}

WOLANG_TEST(SourceManager, LineTextStripsTerminators) {
  SourceManager sources;
  const FileId file =
      sources.AddVirtualFile("x.wo", "first\r\nsecond\nthird");

  WOLANG_EXPECT_EQ(sources.LineText(file, 1), "first");
  WOLANG_EXPECT_EQ(sources.LineText(file, 2), "second");
  WOLANG_EXPECT_EQ(sources.LineText(file, 3), "third");
  WOLANG_EXPECT(sources.LineText(file, 0).empty());
  WOLANG_EXPECT(sources.LineText(file, 4).empty());
}

WOLANG_TEST(SourceManager, LineStartOffsetMatchesPosition) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("x.wo", "ab\ncd\nef\n");
  WOLANG_EXPECT_EQ(sources.LineStartOffset(file, 1), 0u);
  WOLANG_EXPECT_EQ(sources.LineStartOffset(file, 2), 3u);
  WOLANG_EXPECT_EQ(sources.LineStartOffset(file, 3), 6u);
}

WOLANG_TEST(SourceManager, SpanTextClampsToFile) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("x.wo", "hello world");
  WOLANG_EXPECT_EQ(sources.SpanText(SourceSpan{file, 6, 11}), "world");
  WOLANG_EXPECT_EQ(sources.SpanText(SourceSpan{file, 6, 999}), "world");
  WOLANG_EXPECT(sources.SpanText(SourceSpan{file, 5, 5}).empty());
}

WOLANG_TEST(SourceManager, FormatPositionIsHumanReadable) {
  SourceManager sources;
  const FileId file = sources.AddVirtualFile("combat/turret.wo", "a\nbc\n");
  WOLANG_EXPECT_EQ(sources.FormatPosition(file, 3), "combat/turret.wo:2:2");
}

WOLANG_TEST(SourceSpanTest, MergeCoversBothOperands) {
  const FileId file{0};
  const SourceSpan left{file, 10, 20};
  const SourceSpan right{file, 15, 30};
  const SourceSpan merged = left.Merge(right);
  WOLANG_EXPECT_EQ(merged.begin, 10u);
  WOLANG_EXPECT_EQ(merged.end, 30u);
}

WOLANG_TEST(SourceSpanTest, MergeIgnoresOtherFiles) {
  const SourceSpan left{FileId{0}, 10, 20};
  const SourceSpan right{FileId{1}, 0, 5};
  WOLANG_EXPECT_EQ(left.Merge(right), left);
}

WOLANG_TEST(SourceSpanTest, PointHasZeroLength) {
  const SourceSpan point = SourceSpan::Point(FileId{3}, 42);
  WOLANG_EXPECT_EQ(point.length(), 0u);
  WOLANG_EXPECT(point.is_valid());
}

WOLANG_TEST(SourceManager, LoadFileReportsMissingPath) {
  SourceManager sources;
  std::string error;
  const FileId file =
      sources.LoadFile("definitely/missing/file.wo", &error);
  WOLANG_EXPECT(!file.is_valid());
  WOLANG_EXPECT(!error.empty());
}

}  // namespace
}  // namespace wolang
