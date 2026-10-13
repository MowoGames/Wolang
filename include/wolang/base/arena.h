// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_BASE_ARENA_H_
#define WOLANG_BASE_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wolang {

// A bump allocator used for compiler data that lives as long as the compilation
// unit that produced it: tokens, AST nodes, interned strings, IR bodies.
//
// Destructors are never run, so only trivially destructible types may be
// allocated. Memory is released in one step by destroying or resetting the
// arena, which is what makes per-file and per-function lifetimes cheap.
class Arena {
 public:
  static constexpr size_t kDefaultChunkSize = 64 * 1024;

  explicit Arena(size_t chunk_size = kDefaultChunkSize);
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&& other) noexcept;
  Arena& operator=(Arena&& other) noexcept;

  // Returns uninitialized storage of at least |size| bytes aligned to |align|.
  // |align| must be a power of two. Never returns null.
  void* Allocate(size_t size, size_t align);

  template <typename T, typename... Args>
  T* New(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena never runs destructors");
    void* storage = Allocate(sizeof(T), alignof(T));
    return new (storage) T(std::forward<Args>(args)...);
  }

  // Returns |count| default-initialized elements.
  template <typename T>
  std::span<T> NewArray(size_t count) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena never runs destructors");
    if (count == 0) {
      return {};
    }
    void* storage = Allocate(sizeof(T) * count, alignof(T));
    T* elements = new (storage) T[count]();
    return std::span<T>(elements, count);
  }

  // Copies |text| into the arena and returns a view of the copy. The copy is
  // NUL terminated so it can be handed to C APIs.
  std::string_view CopyString(std::string_view text);

  // Drops every allocation, keeping the first chunk for reuse.
  void Reset();

  // Bytes handed out to callers, excluding alignment padding and unused chunk
  // tails.
  size_t bytes_used() const { return bytes_used_; }

  // Bytes currently owned by the arena.
  size_t bytes_reserved() const { return bytes_reserved_; }

  size_t chunk_count() const { return chunks_.size(); }

 private:
  struct Chunk {
    std::unique_ptr<std::byte[]> data;
    size_t size = 0;
  };

  void AddChunk(size_t minimum_size);

  std::vector<Chunk> chunks_;
  std::byte* cursor_ = nullptr;
  std::byte* chunk_end_ = nullptr;
  size_t chunk_size_ = kDefaultChunkSize;
  size_t bytes_used_ = 0;
  size_t bytes_reserved_ = 0;
};

}  // namespace wolang

#endif  // WOLANG_BASE_ARENA_H_
