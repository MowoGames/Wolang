// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Bindings for an engine's reflected classes, from the JSON its header
// tool writes: Wolang declarations for scripts, and the C++ the engine
// compiles to answer them (D218).

#ifndef WOLANG_BIND_REFLECTION_H_
#define WOLANG_BIND_REFLECTION_H_

#include <string>
#include <vector>

namespace wolang {
namespace bind {

struct ReflectionOptions {
  // The Wolang package the declarations are for, which is also what the
  // registration function is called after: `MowoWolang_Bind_mowo`.
  std::string module = "mowo";
  // An effect every engine call carries, so that a mod has to be granted
  // it: `World`. Empty for none.
  std::string effect;
};

struct ReflectionResult {
  std::string source;
  std::string glue;
  // What was left out and why: a parameter of a type with no binding, a
  // `std::string` answer. Reported rather than dropped.
  std::vector<std::string> skipped;
  int classes = 0;
  int functions = 0;
  int properties = 0;
};

// |documents| are `*.reflection.json` files, every module's together: a
// class in one names a class in another.
ReflectionResult GenerateFromReflection(const std::vector<std::string>& documents, const ReflectionOptions& options);

}  // namespace bind
}  // namespace wolang

#endif  // WOLANG_BIND_REFLECTION_H_
