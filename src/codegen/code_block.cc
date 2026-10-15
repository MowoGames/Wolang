// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/code_block.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#include "wolang/base/check.h"

namespace wolang {
namespace codegen {
namespace {

size_t PageSize() {
#if defined(_WIN32)
  static const size_t size = [] {
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return static_cast<size_t>(info.dwPageSize);
  }();
#else
  static const size_t size = static_cast<size_t>(::getpagesize());
#endif
  return size;
}

// Pages that can be written now and executed later.
//
// Two steps rather than one, on every system: memory that is writable
// *and* executable at the same time is what every exploit of the last
// thirty years wanted, and a tier that compiles and then runs has no
// need of it (D159).
void* Writable(size_t rounded) {
#if defined(_WIN32)
  void* memory = ::VirtualAlloc(nullptr, rounded,
                                MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
  return memory;
#else
  void* memory = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  return memory == MAP_FAILED ? nullptr : memory;
#endif
}

bool MakeExecutable(void* base, size_t size) {
#if defined(_WIN32)
  DWORD was = 0;
  return ::VirtualProtect(base, size, PAGE_EXECUTE_READ, &was) != 0;
#else
  return ::mprotect(base, size, PROT_READ | PROT_EXEC) == 0;
#endif
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
#if defined(_WIN32)
    ::VirtualFree(base_, 0, MEM_RELEASE);
#else
    ::munmap(base_, size_);
#endif
    base_ = nullptr;
    size_ = 0;
  }
}

const void* CodeBlock::Reserve(size_t size) {
  Release();
  if (size == 0) {
    return nullptr;
  }
  const size_t rounded = RoundUpToPage(size);
  void* memory = Writable(rounded);
  if (memory == nullptr) {
    return nullptr;
  }
  base_ = memory;
  size_ = rounded;
  return base_;
}

bool CodeBlock::Commit(const std::vector<uint8_t>& code) {
  if (base_ == nullptr || code.size() > size_) {
    return false;
  }
  std::memcpy(base_, code.data(), code.size());
  if (!MakeExecutable(base_, size_)) {
    Release();
    return false;
  }
  SynchronizeCaches(base_, code.size());
  return true;
}

bool CodeBlock::Install(const std::vector<uint8_t>& code) {
  Release();
  if (code.empty()) {
    return false;
  }
  const size_t bytes = code.size();
  const size_t rounded = RoundUpToPage(bytes);

  void* memory = Writable(rounded);
  if (memory == nullptr) {
    return false;
  }
  std::memcpy(memory, code.data(), bytes);

  if (!MakeExecutable(memory, rounded)) {
#if defined(_WIN32)
    ::VirtualFree(memory, 0, MEM_RELEASE);
#else
    ::munmap(memory, rounded);
#endif
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
