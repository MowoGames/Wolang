// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#ifndef WOLANG_IR_READER_H_
#define WOLANG_IR_READER_H_

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "wolang/base/string_interner.h"
#include "wolang/ir/ir.h"
#include "wolang/sema/type.h"

namespace wolang {
namespace ir {

struct ReadOptions {
  std::map<std::string, TypeId> named;
};

struct ReadResult {
  Module module;
  std::vector<std::string> errors;

  bool ok() const { return errors.empty(); }
};

ReadResult ReadModule(StringInterner* interner, TypeStore* types, std::string_view text, const ReadOptions& options);

}  // namespace ir
}  // namespace wolang

#endif  // WOLANG_IR_READER_H_
