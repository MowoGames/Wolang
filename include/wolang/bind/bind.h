// Copyright 2026 The Wolang Authors. All rights reserved.
//
// Declarations for a C library, written as Wolang.
//
// A header is C, and C is a language with a preprocessor: the honest way
// to read one is to let the system's own preprocessor expand it first and
// then read *declarations*, which is what this does. What comes out is a
// file of `extern "C" fun` declarations and constants -- a starting point
// a person edits, not a binding that pretends to be complete (D130).

#ifndef WOLANG_BIND_BIND_H_
#define WOLANG_BIND_BIND_H_

#include <string>
#include <vector>

namespace wolang {
namespace bind {

struct Options {
  // What the generated module is called in a comment at the top.
  std::string header;
  // Declarations whose name starts with this, and nothing else. For a
  // library whose header pulls in half the system.
  std::string prefix;
  // Emit `offer` on everything, so another module can use the bindings.
  bool offer = true;
};

struct Result {
  std::string source;
  // What was skipped and why: a struct passed by value, a variadic, a
  // type nothing could be made of. Reported rather than dropped --
  // silence is what makes a generated binding untrustworthy.
  std::vector<std::string> skipped;
  int functions = 0;
  int constants = 0;
  int aliases = 0;
};

// Reads C declarations out of |text| -- preprocessed or not -- and writes
// Wolang for them.
Result Generate(const std::string& text, const Options& options);

// Runs the system's C preprocessor over |path| and answers what it said.
// Empty when there is no preprocessor or it failed, in which case the
// caller reads the file as it is.
std::string Preprocess(const std::string& path,
                       const std::vector<std::string>& include_paths);

}  // namespace bind
}  // namespace wolang

#endif  // WOLANG_BIND_BIND_H_
