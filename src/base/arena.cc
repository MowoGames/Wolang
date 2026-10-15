// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/base/arena.h"

#include <algorithm>
#include <cstring>

#include "wolang/base/check.h"

namespace wolang {
namespace {

// Only referenced by a debug assertion, which release builds compile away.
[[maybe_unused]] bool IsPowerOfTwo(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

size_t AlignUp(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

}  // namespace

Arena::Arena(size_t chunk_size)
    : chunk_size_(std::max<size_t>(chunk_size, 1024)) {}

Arena::~Arena() = default;

Arena::Arena(Arena&& other) noexcept
    : chunks_(std::move(other.chunks_)),
      cursor_(other.cursor_),
      chunk_end_(other.chunk_end_),
      chunk_size_(other.chunk_size_),
      bytes_used_(other.bytes_used_),
      bytes_reserved_(other.bytes_reserved_) {
  other.cursor_ = nullptr;
  other.chunk_end_ = nullptr;
  other.bytes_used_ = 0;
  other.bytes_reserved_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  chunks_ = std::move(other.chunks_);
  cursor_ = other.cursor_;
  chunk_end_ = other.chunk_end_;
  chunk_size_ = other.chunk_size_;
  bytes_used_ = other.bytes_used_;
  bytes_reserved_ = other.bytes_reserved_;
  other.cursor_ = nullptr;
  other.chunk_end_ = nullptr;
  other.bytes_used_ = 0;
  other.bytes_reserved_ = 0;
  return *this;
}

void* Arena::Allocate(size_t size, size_t align) {
  WOLANG_DCHECK(IsPowerOfTwo(align));
  if (size == 0) {
    size = 1;
  }

  auto cursor_value = reinterpret_cast<uintptr_t>(cursor_);
  auto aligned = static_cast<uintptr_t>(AlignUp(cursor_value, align));
  if (cursor_ == nullptr ||
      aligned + size > reinterpret_cast<uintptr_t>(chunk_end_)) {
    AddChunk(size + align);
    cursor_value = reinterpret_cast<uintptr_t>(cursor_);
    aligned = static_cast<uintptr_t>(AlignUp(cursor_value, align));
  }

  auto* result = reinterpret_cast<std::byte*>(aligned);
  cursor_ = result + size;
  bytes_used_ += size;
  return result;
}

void Arena::AddChunk(size_t minimum_size) {
  const size_t size = std::max(chunk_size_, minimum_size);
  Chunk chunk;
  chunk.data = std::make_unique<std::byte[]>(size);
  chunk.size = size;
  cursor_ = chunk.data.get();
  chunk_end_ = cursor_ + size;
  bytes_reserved_ += size;
  chunks_.push_back(std::move(chunk));

  // Grow geometrically so that long-lived arenas stop hitting the allocator,
  // but keep a ceiling so a single chunk never dominates memory use.
  chunk_size_ = std::min<size_t>(chunk_size_ * 2, 4 * 1024 * 1024);
}

std::string_view Arena::CopyString(std::string_view text) {
  auto* storage = static_cast<char*>(Allocate(text.size() + 1, alignof(char)));
  if (!text.empty()) {
    std::memcpy(storage, text.data(), text.size());
  }
  storage[text.size()] = '\0';
  return std::string_view(storage, text.size());
}

void Arena::Reset() {
  bytes_used_ = 0;
  if (chunks_.empty()) {
    cursor_ = nullptr;
    chunk_end_ = nullptr;
    return;
  }

  chunks_.resize(1);
  cursor_ = chunks_.front().data.get();
  chunk_end_ = cursor_ + chunks_.front().size;
  bytes_reserved_ = chunks_.front().size;
}

}  // namespace wolang
