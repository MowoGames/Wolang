// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_CODEGEN_CODE_BLOCK_H_
#define WOLANG_CODEGEN_CODE_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wolang {
namespace codegen {

// Memory that can be executed.
//
// Writable while it is being filled and executable afterwards, never both:
// a page that is writable and executable at the same time is the thing an
// attacker is looking for, and the two-step protection costs one system
// call per compilation rather than per instruction.
class CodeBlock {
 public:
  CodeBlock() = default;
  ~CodeBlock();

  CodeBlock(const CodeBlock&) = delete;
  CodeBlock& operator=(const CodeBlock&) = delete;
  CodeBlock(CodeBlock&& other) noexcept;
  CodeBlock& operator=(CodeBlock&& other) noexcept;

  // Copies |words| into fresh pages and makes them executable. Returns false
  // when the system refused, which a caller has to handle: the dev tier
  // falls back to the interpreter rather than failing the build.
  bool Install(const std::vector<uint32_t>& words);

  bool is_installed() const { return base_ != nullptr; }
  size_t size() const { return size_; }

  // The address |offset| bytes into the block, as something callable.
  const void* AddressOf(uint32_t offset) const;

 private:
  void Release();

  void* base_ = nullptr;
  size_t size_ = 0;
};

}  // namespace codegen
}  // namespace wolang

#endif  // WOLANG_CODEGEN_CODE_BLOCK_H_
