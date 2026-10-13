// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_BASE_SOURCE_MANAGER_H_
#define WOLANG_BASE_SOURCE_MANAGER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wolang {

// Identifies a file registered with a SourceManager.
struct FileId {
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

  uint32_t index = kInvalidIndex;

  bool is_valid() const { return index != kInvalidIndex; }

  friend bool operator==(FileId lhs, FileId rhs) {
    return lhs.index == rhs.index;
  }
  friend bool operator!=(FileId lhs, FileId rhs) { return !(lhs == rhs); }
};

// A half-open byte range [begin, end) inside one file. Line and column numbers
// are derived on demand, so spans stay 12 bytes and cheap to copy.
struct SourceSpan {
  FileId file;
  uint32_t begin = 0;
  uint32_t end = 0;

  static SourceSpan Point(FileId file, uint32_t offset) {
    return SourceSpan{file, offset, offset};
  }

  bool is_valid() const { return file.is_valid() && begin <= end; }
  uint32_t length() const { return end - begin; }

  // Returns the smallest span covering both operands. Spans from different
  // files are not merged; |*this| wins in that case.
  SourceSpan Merge(SourceSpan other) const;

  friend bool operator==(SourceSpan lhs, SourceSpan rhs) {
    return lhs.file == rhs.file && lhs.begin == rhs.begin &&
           lhs.end == rhs.end;
  }
};

// A one-based position. |column| counts bytes inside the line, which keeps it
// exact for ASCII and stable for UTF-8; presentation layers that need
// character or UTF-16 columns convert using the line text.
struct LineColumn {
  uint32_t line = 1;
  uint32_t column = 1;
};

// Owns the text of every source file in a compilation and answers position
// queries about them. Files are immutable once added, and their contents and
// line tables are stable, so lexers and diagnostics can hold views into them.
class SourceManager {
 public:
  SourceManager();
  ~SourceManager();

  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;

  // Registers |text| under |path| and returns its identifier.
  FileId AddFile(std::string path, std::string text);

  // Registers an in-memory buffer. |name| is shown in diagnostics and is
  // conventionally bracketed, for example "<repl>".
  FileId AddVirtualFile(std::string name, std::string text);

  // Reads |path| from disk and registers it. On failure returns an invalid
  // FileId and, when |error| is not null, stores a human readable reason.
  FileId LoadFile(std::string_view path, std::string* error);

  // Replaces the contents of an already registered file, which is what an
  // editor does on every keystroke.
  //
  // Every `std::string_view` previously handed out for this file dangles
  // afterwards, so views must not be held across an edit. Offsets stay
  // meaningful: cached token streams and syntax trees store offsets, not
  // views, which is what lets them survive until something asks for them
  // again.
  void ReplaceText(FileId file, std::string text);

  size_t file_count() const { return files_.size(); }

  std::string_view Text(FileId file) const;
  std::string_view Path(FileId file) const;

  // Converts a byte offset into a one-based line and column. Offsets past the
  // end of the file clamp to the last position.
  LineColumn Position(FileId file, uint32_t offset) const;

  LineColumn Position(SourceSpan span) const {
    return Position(span.file, span.begin);
  }

  // Number of lines in the file; a file without a trailing newline still
  // counts its last partial line.
  uint32_t LineCount(FileId file) const;

  // Returns the text of a one-based line without its line terminator.
  std::string_view LineText(FileId file, uint32_t line) const;

  // Byte offset where a one-based line starts.
  uint32_t LineStartOffset(FileId file, uint32_t line) const;

  std::string_view SpanText(SourceSpan span) const;

  // Formats "path:line:column" for log output and error prefixes.
  std::string FormatPosition(FileId file, uint32_t offset) const;

 private:
  struct Entry {
    std::string path;
    std::string text;
    // Byte offset of the first character of each line. Always starts with 0.
    std::vector<uint32_t> line_starts;
  };

  const Entry* Get(FileId file) const;
  FileId Register(std::string path, std::string text);

  std::vector<std::unique_ptr<Entry>> files_;
};

}  // namespace wolang

#endif  // WOLANG_BASE_SOURCE_MANAGER_H_
