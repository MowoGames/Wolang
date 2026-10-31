// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.

#include "wolang/codegen/aot.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "harness/test.h"
#include "wolang/compiler/database.h"

namespace wolang {
namespace codegen {
namespace {

// Compiles a program ahead of time, links it, runs it, and reads what it
// printed.
//
// The whole way through on purpose. A unit test that stopped at the object
// file would be checking that LLVM accepted the module, which is not the
// question: the question is whether the binary a user ships prints what
// the reference tier says it should (D98). So this one shells out to the
// linker and runs the result, and the answer it compares is the program's
// own output.
class Binary {
 public:
  explicit Binary(std::string_view code) : code_(code) {}

  ~Binary() {
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }

  // What the program printed, or a `<...>` saying what went wrong instead.
  std::string Output() {
    Database database;
    const FileId file = database.AddFile("<test>", code_);
    const LoweredFile* lowered = database.Lowered(file);
    const CheckedFile* checked = database.Checked(file);
    if (lowered == nullptr || checked == nullptr) {
      return "<not lowered>";
    }
    if (const std::vector<Diagnostic>* diagnostics =
            database.Diagnostics(file);
        diagnostics != nullptr) {
      for (const Diagnostic& diagnostic : *diagnostics) {
        if (diagnostic.severity == Severity::kError) {
          return "<error: " + std::string(diagnostic.code) + ">";
        }
      }
    }

    // A name of this process's own, so that two tests running at once do
    // not write each other's object file.
#if defined(_WIN32)
    const int process = _getpid();
#else
    const int process = static_cast<int>(::getpid());
#endif
    const std::filesystem::path stem = std::filesystem::temp_directory_path() / ("wolang-aot-" + std::to_string(process) + "-" + std::to_string(++serial_));
    path_ = BinaryName(stem.string());
    const FileCode files[1] = {
        FileCode{&checked->resolution, &lowered->module}};
    AotOptions options;
    options.object_path = stem.string() + ".o";
    options.optimization = 2;
    const AotResult result =
        EmitObject(database.interner(), database.types(), files, options);
    if (!result.error.empty()) {
      return "<backend: " + result.error + ">";
    }
    if (!result.skipped.empty()) {
      return "<left out: " + result.skipped.front() + ">";
    }

#if defined(_WIN32)
    const std::string link = LinkCommand(options.object_path, path_) + " > NUL 2>&1";
#else
    const std::string link = LinkCommand(options.object_path, path_) + " 2>/dev/null";
#endif
    const int linked = std::system(link.c_str());
    std::remove(options.object_path.c_str());
    if (linked != 0) {
      return "<linker failed>";
    }

    std::string said;
#if defined(_WIN32)
    FILE* running = ::_popen(("\"" + path_ + "\" 2>&1").c_str(), "r");
#else
    FILE* running = ::popen((path_ + " 2>&1").c_str(), "r");
#endif
    if (running == nullptr) {
      return "<could not run>";
    }
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), running) != nullptr) {
      said.append(buffer);
    }
#if defined(_WIN32)
    const int code = ::_pclose(running);
#else
    const int status = ::pclose(running);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) {
      said.pop_back();
    }
    // How it ended, as part of the answer: a program says what it
    // computed by the code it exits with, because a test here cannot
    // import `std.io` -- a single file is not a package, and the point of
    // these tests is the back end rather than the library.
    said.append(said.empty() ? "exit " : " exit ");
    said.append(std::to_string(code));
    return said;
  }

 private:
  std::string code_;
  std::string path_;
  static int serial_;
};

int Binary::serial_ = 0;

static bool Links() {
  return HasAotBackend();
}

WOLANG_TEST(AotTest, RunsArithmeticAndControlFlow) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

fun Fib(n: int) -> int {
  if n < 2 {
    return n
  }
  return Fib(n - 1) + Fib(n - 2)
}

fun main() uses unsafe {
  var total = 0
  for i in 0..10 {
    total = total + Fib(i)
  }
  exit(total as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 88");
}

WOLANG_TEST(AotTest, RunsClassesAndVirtualCalls) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

class Shape {
  var size: int = 1
  virtual fun Area() -> int {
    return size
  }
}

class Square: Shape {
  override fun Area() -> int {
    return size * size
  }
}

fun main() uses unsafe {
  const shapes = [Shape(5), Square(5)]
  var total = 0
  for shape in shapes {
    total = total + shape.Area()
  }
  exit(total as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 30");
}

WOLANG_TEST(AotTest, RunsStructsOptionalsAndEnums) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

struct Point {
  var x: int
  var y: int
}

enum Step {
  Wait,
  Move(x: int, y: int),
}

fun Walk(step: Step) -> int {
  return match step {
    Step.Wait => 0,
    Step.Move(x, y) => x + y,
  }
}

fun Maybe(flag: bool) -> int? {
  if flag {
    return 7
  }
  return null
}

fun main() uses unsafe {
  const point = Point(3, 4)
  var total = point.x * point.y
  total = total + Walk(Step.Move(2, 3)) + Walk(Step.Wait)
  total = total + (Maybe(true) ?? 0) + (Maybe(false) ?? 1)
  exit(total as s32)
}
)");
  // Twelve from the struct, five from the enum, seven and one from the
  // optionals.
  WOLANG_EXPECT_EQ(program.Output(), "exit 25");
}

WOLANG_TEST(AotTest, ATrapSaysWhatHappenedAndStops) {
  if (!Links()) {
    return;
  }
  // A binary has no interpreter under it to report a trap to, so a trap
  // is a message and an exit rather than undefined behaviour.
  Binary program(R"(
extern "C" fun exit(code: s32)

fun Pick(xs: [4]int, at: int) -> int {
  return xs[at]
}

fun main() uses unsafe {
  const xs = [1, 2, 3, 4]
  exit(Pick(xs, 9) as s32)
}
)");
  const std::string said = program.Output();
  WOLANG_EXPECT(said.find("out of bounds") != std::string::npos);
  WOLANG_EXPECT(said.find("exit 70") != std::string::npos);
}

WOLANG_TEST(AotTest, IntIsSixtyFourBits) {
  if (!Links()) {
    return;
  }
  // 2^31 is a value of `int` (D238); 2^31 / 2^30 is 2.
  Binary program(R"(
extern "C" fun exit(code: s32)

fun main() uses unsafe {
  var a = 2147483647
  a += 1
  exit((a / 1073741824) as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 2");
}

WOLANG_TEST(AotTest, ASignedThirtyTwoBitSumStillTraps) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

fun main() uses unsafe {
  var a: s32 = 2147483647
  a += 1
  exit(a)
}
)");
  WOLANG_EXPECT(program.Output().find("overflow") != std::string::npos);
}

}  // namespace
}  // namespace codegen
}  // namespace wolang
