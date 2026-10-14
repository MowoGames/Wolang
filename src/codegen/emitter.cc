// Copyright 2026 The Wolang Authors. All rights reserved.

#include "wolang/codegen/emitter.h"

#include <string>

namespace wolang {
namespace codegen {

CompiledFunction EmitFunction(Target target,
                              const StringInterner& interner,
                              const TypeStore& types,
                              abi::LayoutStore* layouts,
                              const ir::Module& module,
                              const ir::Function& function,
                              const std::vector<bool>& compilable,
                              const std::vector<uint64_t>& foreign,
                              const std::vector<std::pair<uint64_t,
                                                          uint64_t>>& strings,
                              uint32_t trap_site_base) {
  switch (target) {
    case Target::kArm64:
      return arm64::EmitFunction(interner, types, layouts, module, function,
                                 compilable, foreign, strings,
                                 trap_site_base);
    case Target::kX64:
    case Target::kUnknown:
      break;
  }
  CompiledFunction result;
  result.reason = "has no code generator for ";
  result.reason.append(TargetName(target));
  return result;
}

}  // namespace codegen
}  // namespace wolang
