// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/code_block.h"

#include <cstring>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {
namespace {

size_t PageSize() {
  static const size_t size = static_cast<size_t>(::getpagesize());
  return size;
}

size_t RoundUpToPage(size_t bytes) {
  const size_t page = PageSize();
  return (bytes + page - 1) / page * page;
}

// Tells the processor that the bytes it may have read as data are now
// instructions. On a machine with separate caches, skipping this executes
// whatever used to be at that address -- a bug that reproduces only under
// load, which is the worst kind to leave in.
void SynchronizeCaches(void* base, size_t size) {
#if defined(__APPLE__)
  ::sys_icache_invalidate(base, size);
#elif defined(__aarch64__)
  __builtin___clear_cache(static_cast<char*>(base),
                          static_cast<char*>(base) + size);
#else
  (void)base;
  (void)size;
#endif
}

}  // namespace

CodeBlock::~CodeBlock() {
  Release();
}

CodeBlock::CodeBlock(CodeBlock&& other) noexcept
    : base_(other.base_), size_(other.size_) {
  other.base_ = nullptr;
  other.size_ = 0;
}

CodeBlock& CodeBlock::operator=(CodeBlock&& other) noexcept {
  if (this != &other) {
    Release();
    base_ = other.base_;
    size_ = other.size_;
    other.base_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void CodeBlock::Release() {
  if (base_ != nullptr) {
    ::munmap(base_, size_);
    base_ = nullptr;
    size_ = 0;
  }
}

bool CodeBlock::Install(const std::vector<uint32_t>& words) {
  Release();
  if (words.empty()) {
    return false;
  }
  const size_t bytes = words.size() * sizeof(uint32_t);
  const size_t rounded = RoundUpToPage(bytes);

  void* memory = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  if (memory == MAP_FAILED) {
    return false;
  }
  std::memcpy(memory, words.data(), bytes);

  if (::mprotect(memory, rounded, PROT_READ | PROT_EXEC) != 0) {
    ::munmap(memory, rounded);
    return false;
  }
  SynchronizeCaches(memory, bytes);

  base_ = memory;
  size_ = rounded;
  return true;
}

const void* CodeBlock::AddressOf(uint32_t offset) const {
  WOLANG_CHECK(base_ != nullptr && offset < size_);
  return static_cast<const uint8_t*>(base_) + offset;
}

}  // namespace codegen
}  // namespace wolang
