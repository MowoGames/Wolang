// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_COMPILER_CACHE_H_
#define WOLANG_COMPILER_CACHE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/compiler/database.h"

namespace wolang {

// The on-disk form of what one compilation learned, so that reopening a
// project does not reparse files that did not change.
//
// Only item trees are stored. They are the expensive-to-reach, cheap-to-store
// summaries: once an item tree is known to be unchanged, every query that
// depends on it stays valid without the file being lexed or parsed at all.
// Token streams and syntax trees are deliberately left out -- they are large,
// and anything that needs them is about to look at the file anyway.
//
// The cache is a correctness-neutral optimization: a stale, corrupt or
// unreadable cache is discarded, never trusted. Every entry carries the hash
// and size of the text it was computed from, and the whole file carries a
// format version, so nothing survives a change to the compiler's data layout.
struct CacheStats {
  // Entries read from disk whose file still has the same text.
  uint32_t reused = 0;
  // Entries dropped because the file changed, vanished, or did not match.
  uint32_t stale = 0;
};

class Cache {
 public:
  // The format version. Bump this whenever a serialized structure changes;
  // an older file is then discarded rather than misread.
  static constexpr uint32_t kVersion = 6;

  // Writes the item trees of every source file in |database| to |path|.
  // Returns false when the file could not be written.
  static bool Save(Database* database, std::string_view path);

  // Loads |path| and hands every still-valid entry to |database|. Returns
  // false when there was nothing usable to load, which is not an error: the
  // first build of a project has no cache.
  static bool Load(Database* database,
                   std::string_view path,
                   CacheStats* stats = nullptr);
};

}  // namespace wolang

#endif  // WOLANG_COMPILER_CACHE_H_
