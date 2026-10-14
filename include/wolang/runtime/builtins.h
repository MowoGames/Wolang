// Copyright 2026 The Wolang Authors. All rights reserved.

#ifndef WOLANG_RUNTIME_BUILTINS_H_
#define WOLANG_RUNTIME_BUILTINS_H_

#include <cstdint>
#include <string_view>

namespace wolang {
namespace runtime {

// The functions the language's own machinery calls: allocating the frame a
// `task` keeps its state in, and releasing it.
//
// Looked up by name through a table in the compiler rather than through the
// host's loader. Two reasons, and the second is the one that matters: a
// static library's object file is dropped when nothing references it, so
// exporting these for `dlsym` would mean asking every executable to force
// the file in -- and a target without a dynamic loader at all would have no
// way to find them.
extern "C" {

// Allocates |size| bytes for a task frame, zeroed: a frame's state has to
// start at zero and a slot has to start as nothing.
void* wolang_frame_alloc(uint64_t size);
void wolang_frame_free(void* frame);

}  // extern "C"

// The address of the runtime function named |name|, or null. Consulted
// before the host's loader, so the language's own symbols cannot be shadowed
// by something that happens to share a name.
void* LookupSymbol(std::string_view name);

}  // namespace runtime
}  // namespace wolang

#endif  // WOLANG_RUNTIME_BUILTINS_H_
