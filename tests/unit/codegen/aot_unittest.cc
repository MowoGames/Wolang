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

WOLANG_TEST(AotTest, AnEscapingClosureOwnsWhatItCaptured) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

var drops: int = 0

class Cell {
  var n: int = 0
  fun Bump() -> int {
    n = n + 1
    return n
  }
  deinit {
    drops = drops + 1
  }
}

fun Make() -> fun() -> int {
  const cell = Cell(0)
  return || cell.Bump()
}

fun main() uses unsafe {
  var total = 0
  {
    const counter = Make()
    total = counter() * 10 + counter()
  }
  exit((total * 10 + drops) as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 121");
}

WOLANG_TEST(AotTest, PayloadErrorsAgree) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

error Plain, Bad(line: int, why: text)

fun Make(n: int) -> int!(Plain, Bad) {
  if n == 0 {
    return Plain
  }
  if n == 1 {
    return Bad(40, "late")
  }
  return n
}

fun Pass(n: int) -> int!(Plain, Bad) {
  const v = Make(n)?
  return v + 1
}

fun Use(n: int) -> int {
  return match Pass(n) {
    Ok(v) => v
    Plain => 1
    Bad(line, why) => line + why.len
  }
}

fun main() uses unsafe {
  exit((Use(0) + Use(1) + Use(5)) as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 51");
}

WOLANG_TEST(AotTest, AClassWithAnOwnedTextFieldGivesItBack) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)

class Holder {
  var label: Text
}

fun FromBorrowed(word: Text) -> int {
  const h = Holder("{word}")
  return h.label.len
}

fun FromNumber(n: int) -> int {
  const h = Holder("n={n}")
  return h.label.len
}

fun Replaced(n: int) -> int {
  const h = Holder("a{n}")
  h.label = "bb{n}"
  return h.label.len
}

fun main() uses unsafe {
  var total = 0
  for i in 0..20 {
    const word = "w{i}"
    total = total + FromBorrowed(word) + FromNumber(i) + Replaced(i)
  }
  exit(total as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 190");
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

WOLANG_TEST(AotTest, AnOperatorOnAStructIsACall) {
  if (!Links()) {
    return;
  }
  // D239: `a + b` on a struct is `a.Plus(b)`, and the compiled binary
  // agrees with the interpreter.
  Binary program(R"(
extern "C" fun exit(code: s32)

@Operator
ability Add {
  fun Plus(other: Self) -> Self
}

struct V {
  var x: int
  var y: int
}

extend V as Add {
  fun Plus(other: Self) -> Self => V(x + other.x, y + other.y)
}

fun main() uses unsafe {
  var a = V(1, 2) + V(10, 20)
  a += V(3, 4)
  exit((a.x + a.y) as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 40");
}

static std::string ObjectBytes(std::string_view code, std::string_view tag) {
  Database database;
  const FileId file = database.AddFile("<test>", std::string(code));
  const LoweredFile* lowered = database.Lowered(file);
  const CheckedFile* checked = database.Checked(file);
  if (lowered == nullptr || checked == nullptr) {
    return "<not lowered>";
  }

  const FileCode files[1] = {FileCode{&checked->resolution, &lowered->module}};
  AotOptions options;
  options.object_path = (std::filesystem::temp_directory_path() / ("wolang-repro-" + std::string(tag) + ".o")).string();
  options.optimization = 2;
  const AotResult result = EmitObject(database.interner(), database.types(), files, options);
  if (!result.error.empty()) {
    return "<backend: " + result.error + ">";
  }

  std::string bytes;
  if (FILE* held = std::fopen(options.object_path.c_str(), "rb"); held != nullptr) {
    char chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), held)) > 0) {
      bytes.append(chunk, got);
    }
    std::fclose(held);
  }
  std::remove(options.object_path.c_str());
  return bytes;
}

WOLANG_TEST(AotTest, TheSameSourceMakesTheSameObjectBytes) {
  const std::string_view code = R"(
class Cell { var n: int }

fun Bump(c: Cell, by: int) -> int {
  c.n = c.n + by
  return c.n
}

fun main() uses unsafe {
  const c = Cell(1)
  var total = 0
  for i in 0..10 {
    total += Bump(c, i)
  }
  exit(total as s32)
}
)";
  const std::string first = ObjectBytes(code, "a");
  const std::string second = ObjectBytes(code, "b");
  WOLANG_EXPECT(first.size() > 0u && first[0] != '<');
  WOLANG_EXPECT(first == second);
}

WOLANG_TEST(AotTest, TheBundledLinkNeedsNoCCompiler) {
  const std::string command = BundledLinkCommand("program.o", "program", "/opt/wolang/sysroot", "/opt/wolang/sysroot/bin/ld.lld");
  WOLANG_EXPECT(command.find("c++") == std::string::npos || command.find("-lc++") != std::string::npos);
  WOLANG_EXPECT(command.find("--build-id=none") != std::string::npos);
  WOLANG_EXPECT(command.find("-static") != std::string::npos);
  WOLANG_EXPECT(command.find("'/opt/wolang/sysroot/bin/ld.lld'") == 0u);
  WOLANG_EXPECT(command.find("crt1.o") < command.find("program.o"));
  WOLANG_EXPECT(command.find("program.o") < command.find("libwolang_runtime.a"));
  WOLANG_EXPECT(command.find("libwolang_runtime.a") < command.find("--start-group"));
  WOLANG_EXPECT(command.find("--end-group") < command.find("crtn.o"));
}

WOLANG_TEST(AotTest, TheLinkLineCarriesTheSanitizersTheRuntimeWasBuiltWith) {
  const std::string command = LinkCommand("program.o", "program");
  const std::string flag = "-fsanitize=" + std::string(RuntimeSanitizers());
  if (RuntimeSanitizers().empty()) {
    WOLANG_EXPECT(command.find("-fsanitize=") == std::string::npos);
  } else {
    WOLANG_EXPECT(command.find(flag) != std::string::npos);
  }

WOLANG_TEST(AotTest, SavesAndLoadsAnObjectGraph) {
  if (!Links()) {
    return;
  }
  Binary program(R"(
extern "C" fun exit(code: s32)
extern "C" fun wolang_save_encode(schema: *u8, schema_length: u64, offsets: u64, root: u64, out_length: *mut u64) -> *mut u8
extern "C" fun wolang_save_decode(schema: *u8, schema_length: u64, offsets: u64, data: *u8, data_length: u64, status: *mut s32) -> u64

struct Shape<T> {
  var schema: text
  var offsets: u64
}

fun ShapeOf<T>() -> Shape<T>
fun HandleOf<T>(value: T) -> u64
fun FromHandle<T>(word: u64) -> T

class Node {
  @Save
  var n: int = 0
  @Save
  var name: text = ""
  @Save
  var next: Node? = null
  var scratch: int = 0
}

fun main() uses unsafe {
  const a = Node()
  const b = Node()
  a.n = 1
  b.n = 2
  a.name = "ab"
  a.scratch = 50
  a.next = b
  b.next = a

  const first = ShapeOf<Node>()
  var length: u64 = 0
  const bytes = wolang_save_encode(first.schema.data, first.schema.length, first.offsets, HandleOf(a), &mut length)

  const second = ShapeOf<Node>()
  var status: s32 = 0
  const word = wolang_save_decode(second.schema.data, second.schema.length, second.offsets, bytes, length, &mut status)
  const back = FromHandle<Node>(word)

  var code = back.n + (back.name.length as int) * 100 + back.scratch
  if const hop = back.next {
    code = code + hop.n * 10
    if const around = hop.next {
      around.n = 3
      code = code + back.n * 1000
    }
  }
  exit((code % 256) as s32)
}
)");
  WOLANG_EXPECT_EQ(program.Output(), "exit 149");
}

}  // namespace
}  // namespace codegen
}  // namespace wolang
