// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/source_manager.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

#include "wolang/base/check.h"

namespace wolang {
namespace {

std::vector<uint32_t> ComputeLineStarts(std::string_view text) {
  std::vector<uint32_t> starts;
  starts.push_back(0);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      starts.push_back(static_cast<uint32_t>(i + 1));
    }
  }
  return starts;
}

}  // namespace

SourceSpan SourceSpan::Merge(SourceSpan other) const {
  if (!other.is_valid() || other.file != file) {
    return *this;
  }
  return SourceSpan{file, std::min(begin, other.begin),
                    std::max(end, other.end)};
}

SourceManager::SourceManager() = default;
SourceManager::~SourceManager() = default;

FileId SourceManager::Register(std::string path, std::string text) {
  auto entry = std::make_unique<Entry>();
  entry->path = std::move(path);
  entry->text = std::move(text);
  entry->line_starts = ComputeLineStarts(entry->text);

  const auto index = static_cast<uint32_t>(files_.size());
  files_.push_back(std::move(entry));
  return FileId{index};
}

FileId SourceManager::AddFile(std::string path, std::string text) {
  return Register(std::move(path), std::move(text));
}

FileId SourceManager::AddVirtualFile(std::string name, std::string text) {
  return Register(std::move(name), std::move(text));
}

FileId SourceManager::LoadFile(std::string_view path, std::string* error) {
  const std::string path_string(path);
  std::ifstream stream(path_string, std::ios::binary);
  if (!stream.is_open()) {
    if (error) {
      *error = "cannot open '" + path_string + "'";
    }
    return FileId{};
  }

  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  if (stream.bad()) {
    if (error) {
      *error = "cannot read '" + path_string + "'";
    }
    return FileId{};
  }

  return Register(path_string, std::move(text));
}

void SourceManager::ReplaceText(FileId file, std::string text) {
  if (!file.is_valid() || file.index >= files_.size()) {
    return;
  }
  Entry& entry = *files_[file.index];
  entry.text = std::move(text);
  entry.line_starts = ComputeLineStarts(entry.text);
}

const SourceManager::Entry* SourceManager::Get(FileId file) const {
  if (!file.is_valid() || file.index >= files_.size()) {
    return nullptr;
  }
  return files_[file.index].get();
}

std::string_view SourceManager::Text(FileId file) const {
  const Entry* entry = Get(file);
  return entry ? std::string_view(entry->text) : std::string_view();
}

std::string_view SourceManager::Path(FileId file) const {
  const Entry* entry = Get(file);
  return entry ? std::string_view(entry->path) : std::string_view();
}

LineColumn SourceManager::Position(FileId file, uint32_t offset) const {
  const Entry* entry = Get(file);
  if (!entry) {
    return LineColumn{};
  }

  const uint32_t clamped =
      std::min<uint32_t>(offset, static_cast<uint32_t>(entry->text.size()));
  const auto& starts = entry->line_starts;
  // The first line start greater than |clamped| bounds the line containing it.
  auto it = std::upper_bound(starts.begin(), starts.end(), clamped);
  const auto line_index = static_cast<uint32_t>(it - starts.begin() - 1);
  return LineColumn{line_index + 1, clamped - starts[line_index] + 1};
}

uint32_t SourceManager::LineCount(FileId file) const {
  const Entry* entry = Get(file);
  if (!entry) {
    return 0;
  }
  return static_cast<uint32_t>(entry->line_starts.size());
}

uint32_t SourceManager::LineStartOffset(FileId file, uint32_t line) const {
  const Entry* entry = Get(file);
  if (!entry || line == 0 || line > entry->line_starts.size()) {
    return 0;
  }
  return entry->line_starts[line - 1];
}

std::string_view SourceManager::LineText(FileId file, uint32_t line) const {
  const Entry* entry = Get(file);
  if (!entry || line == 0 || line > entry->line_starts.size()) {
    return {};
  }

  const uint32_t begin = entry->line_starts[line - 1];
  uint32_t end = static_cast<uint32_t>(entry->text.size());
  if (line < entry->line_starts.size()) {
    end = entry->line_starts[line];
    // Drop the terminator, handling both "\n" and "\r\n".
    if (end > begin && entry->text[end - 1] == '\n') {
      --end;
    }
    if (end > begin && entry->text[end - 1] == '\r') {
      --end;
    }
  }
  return std::string_view(entry->text).substr(begin, end - begin);
}

std::string_view SourceManager::SpanText(SourceSpan span) const {
  const Entry* entry = Get(span.file);
  if (!entry || span.begin > span.end) {
    return {};
  }
  const auto size = static_cast<uint32_t>(entry->text.size());
  const uint32_t begin = std::min(span.begin, size);
  const uint32_t end = std::min(span.end, size);
  return std::string_view(entry->text).substr(begin, end - begin);
}

std::string SourceManager::FormatPosition(FileId file, uint32_t offset) const {
  const LineColumn position = Position(file, offset);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), ":%u:%u", position.line,
                position.column);
  return std::string(Path(file)) + buffer;
}

}  // namespace wolang
