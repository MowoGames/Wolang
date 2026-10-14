// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/runtime/arena.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

// One chunk of an arena. Chunks are a list rather than one block that
// grows, because a pointer handed out earlier must not move.
struct Chunk {
  uint8_t* bytes = nullptr;
  uint64_t size = 0;
  uint64_t used = 0;
};

struct Arena {
  std::vector<Chunk> chunks;
  uint64_t used = 0;
};

// The stack of open blocks. One per thread when there are threads; there
// is one thread for now and this is where that assumption lives.
std::vector<Arena>& Stack() {
  static std::vector<Arena> stack;
  return stack;
}

constexpr uint64_t kFirstChunk = 64 * 1024;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

void wolang_arena_push() {
  Stack().emplace_back();
}

void wolang_arena_pop() {
  if (Stack().empty()) {
    return;
  }
  Arena& arena = Stack().back();
  for (Chunk& chunk : arena.chunks) {
    std::free(chunk.bytes);
  }
  Stack().pop_back();
}

void* wolang_arena_alloc(uint64_t size) {
  const uint64_t wanted = AlignUp(size != 0 ? size : 1, 16);
  if (Stack().empty()) {
    // No block around this: the general heap, so that a program that
    // never writes `arena` behaves exactly as it did.
    return std::calloc(wanted != 0 ? static_cast<size_t>(wanted) : 1, 1);
  }

  Arena& arena = Stack().back();
  if (!arena.chunks.empty()) {
    Chunk& chunk = arena.chunks.back();
    if (chunk.size - chunk.used >= wanted) {
      uint8_t* result = chunk.bytes + chunk.used;
      chunk.used += wanted;
      arena.used += wanted;
      return result;
    }
  }

  // A new chunk, at least as big as what was asked for and twice the last
  // one: a handler that allocates a lot should stop asking the system for
  // memory after the first few times.
  uint64_t chunk_size = arena.chunks.empty()
                            ? kFirstChunk
                            : arena.chunks.back().size * 2;
  if (chunk_size < wanted) {
    chunk_size = AlignUp(wanted, kFirstChunk);
  }
  auto* bytes = static_cast<uint8_t*>(
      std::calloc(static_cast<size_t>(chunk_size), 1));
  if (bytes == nullptr) {
    return nullptr;
  }
  arena.chunks.push_back(Chunk{bytes, chunk_size, wanted});
  arena.used += wanted;
  return bytes;
}

uint64_t wolang_arena_depth() {
  return Stack().size();
}

uint64_t wolang_arena_used() {
  return Stack().empty() ? 0 : Stack().back().used;
}

}  // namespace runtime
}  // namespace wolang
