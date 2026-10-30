// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The embedding API, exercised the way a host does: load something, call
// it, hand a function back in.

#include "wolang/embed/wolang.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "harness/test.h"
#include "wolang/lsp/json.h"
#include "wolang/runtime/sockets.h"

namespace {

// What a script calls back into. The signature is the contract the
// script's `extern "C"` declaration states.
int32_t test_host_double(int32_t value) {
  return value * 2;
}

std::string TextOf(const WolangValue& value) {
  if (value.kind != kWolangText || value.text == nullptr) {
    return std::string();
  }
  return std::string(value.text, static_cast<size_t>(value.length));
}

}  // namespace

extern "C" uint32_t wolang_abi_c_check_size(void);

static_assert(sizeof(WolangValue) == 32, "WolangValue is 32 bytes (D241)");
static_assert(offsetof(WolangValue, integer) == 8, "payload at 8 (D241)");
static_assert(sizeof(WolangValue::kind) == 4, "kind is 32 bits (D241)");

WOLANG_TEST(EmbedTest, CallsAFunctionInLoadedSource) {
  WolangVm* vm = wolang_vm_create();
  const int32_t loaded = wolang_vm_load_source(vm, "script.wo", R"(
offer fun Multiply(a: int, b: int) -> int => a * b

offer fun Flag(on: bool) -> bool => !on

offer fun Half(value: float) -> float => value / 2.0

fun Hidden() -> int => 7
)");
  WOLANG_EXPECT_EQ(loaded, 0);
  WOLANG_EXPECT(wolang_vm_ok(vm) == 1);

  // Three offered functions, and the one that was not offered is not
  // part of what the script offers a host either (D128).
  WOLANG_EXPECT_EQ(wolang_vm_function_count(vm), 3);
  WOLANG_EXPECT(wolang_vm_has(vm, "Multiply") == 1);
  WOLANG_EXPECT(wolang_vm_has(vm, "Hidden") == 0);

  WolangValue arguments[2] = {wolang_int(6), wolang_int(7)};
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Multiply", arguments, 2, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangInt);
  WOLANG_EXPECT_EQ(answer.integer, 42);

  arguments[0] = wolang_bool(1);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Flag", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangBool);
  WOLANG_EXPECT_EQ(answer.boolean, 0);

  arguments[0] = wolang_float(5.0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Half", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.kind, kWolangFloat);
  WOLANG_EXPECT(answer.number > 2.4 && answer.number < 2.6);

  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, PassesTextBothWays) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "text.wo", R"(
offer fun Longer(left: text, right: text) -> text {
  if left.len >= right.len {
    return left
  }
  return right
}

offer fun Size(value: text) -> int => value.len
)"), 0);

  const char* left = "wolang";
  const char* right = "hi";
  WolangValue arguments[2] = {
      wolang_text(left, std::strlen(left)),
      wolang_text(right, std::strlen(right)),
  };
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Longer", arguments, 2, &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), std::string("wolang"));

  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Size", arguments, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 6);
  wolang_vm_destroy(vm);
}

struct FakeActor {
  int32_t Health = 100;
  int32_t References = 0;
  bool Alive = true;
};

static void FakeRetain(void* object, void* user) {
  static_cast<FakeActor*>(object)->References += 1;
  *static_cast<int32_t*>(user) += 1;
}

static void FakeRelease(void* object, void* user) {
  static_cast<FakeActor*>(object)->References -= 1;
  *static_cast<int32_t*>(user) += 1;
}

static int32_t FakeAlive(void* object, void* user) {
  return static_cast<FakeActor*>(object)->Alive ? 1 : 0;
}

static int32_t Actor_Health(void* actor) {
  return static_cast<FakeActor*>(actor)->Health;
}

static void Actor_Hurt(void* actor, int32_t amount) {
  static_cast<FakeActor*>(actor)->Health -= amount;
}

WOLANG_TEST(EmbedTest, ScriptsUseTheHostsObjects) {
  WolangVm* vm = wolang_vm_create();
  int32_t calls = 0;
  WOLANG_EXPECT_EQ(wolang_vm_register_class(vm, "Actor", &FakeRetain, &FakeRelease, &FakeAlive, &calls), 0);
  WOLANG_EXPECT_EQ(wolang_vm_bind(vm, "Actor_Health", reinterpret_cast<void*>(&Actor_Health)), 0);
  WOLANG_EXPECT_EQ(wolang_vm_bind(vm, "Actor_Hurt", reinterpret_cast<void*>(&Actor_Hurt)), 0);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "actors.wo", R"(
extern class Actor {
  fun Health() -> s32
  fun Hurt(amount: s32)
}

class Tracker {
  var held: Actor? = null
  weak var seen: Actor? = null
}

var tracker: Tracker = Tracker(null, null)

offer fun Tick(pawn: Actor) -> int uses unsafe {
  pawn.Hurt(5)
  return pawn.Health() as int
}

offer fun Keep(pawn: Actor) uses unsafe {
  tracker.held = pawn
  tracker.seen = pawn
}

offer fun Drop() {
  tracker.held = null
}

offer fun Sees() -> bool {
  return tracker.seen != null
}

offer fun Back() -> Actor? {
  return tracker.held
}
)"), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");

  FakeActor actor;
  WolangValue argument = wolang_handle(&actor);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Tick", &argument, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 95);
  WOLANG_EXPECT_EQ(actor.References, 0);

  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Keep", &argument, 1, &answer), 0);
  WOLANG_EXPECT_EQ(actor.References, 1);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Sees", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.boolean, 1);

  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Drop", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(actor.References, 0);
  actor.Alive = false;
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Sees", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.boolean, 0);
  WOLANG_EXPECT(calls >= 2);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, HostsHoldTheProgramsObjects) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "enemies.wo", R"(
var gone: int = 0

offer class Enemy {
  var health: int
  var name: text

  fun Hurt(amount: int) -> int {
    health = health - amount
    return health
  }

  deinit {
    gone = gone + 1
  }
}

offer fun Spawn(health: int) -> Enemy {
  return Enemy(health, "imp")
}

offer fun Gone() -> int {
  return gone
}
)"), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");

  WolangValue argument = wolang_int(40);
  WolangValue enemy = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Spawn", &argument, 1, &enemy), 0);
  WOLANG_EXPECT_EQ(static_cast<int32_t>(enemy.kind), static_cast<int32_t>(kWolangObject));
  WOLANG_EXPECT_EQ(std::string(enemy.text, static_cast<size_t>(enemy.length)), "Enemy");

  WolangValue amount = wolang_int(15);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_object_call(vm, enemy, "Hurt", &amount, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 25);
  WOLANG_EXPECT_EQ(wolang_vm_object_get(vm, enemy, "health", &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 25);
  WOLANG_EXPECT_EQ(wolang_vm_object_get(vm, enemy, "name", &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), "imp");
  WOLANG_EXPECT(wolang_vm_object_get(vm, enemy, "armor", &answer) != 0);

  WOLANG_EXPECT_EQ(wolang_vm_object_retain(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_object_release(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Gone", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 0);
  WOLANG_EXPECT_EQ(wolang_vm_object_release(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Gone", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 1);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, HostsHoldTheProgramsObjectsInterpreted) {
  WolangVm* vm = wolang_vm_create();
  wolang_vm_set_compiled(vm, 0);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "enemies.wo", R"(
var gone: int = 0

offer class Enemy {
  var health: int
  var name: text

  fun Hurt(amount: int) -> int {
    health = health - amount
    return health
  }

  deinit {
    gone = gone + 1
  }
}

offer fun Spawn(health: int) -> Enemy {
  return Enemy(health, "imp")
}

offer fun Gone() -> int {
  return gone
}
)"), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");

  WolangValue argument = wolang_int(40);
  WolangValue enemy = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Spawn", &argument, 1, &enemy), 0);
  WOLANG_EXPECT_EQ(static_cast<int32_t>(enemy.kind), static_cast<int32_t>(kWolangObject));
  WOLANG_EXPECT_EQ(std::string(enemy.text, static_cast<size_t>(enemy.length)), "Enemy");

  WolangValue amount = wolang_int(15);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_object_call(vm, enemy, "Hurt", &amount, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 25);
  WOLANG_EXPECT_EQ(wolang_vm_object_get(vm, enemy, "health", &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 25);
  WOLANG_EXPECT_EQ(wolang_vm_object_get(vm, enemy, "name", &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), "imp");
  WOLANG_EXPECT(wolang_vm_object_get(vm, enemy, "armor", &answer) != 0);

  WOLANG_EXPECT_EQ(wolang_vm_object_retain(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_object_release(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Gone", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 0);
  WOLANG_EXPECT_EQ(wolang_vm_object_release(vm, enemy), 0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Gone", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 1);
  wolang_vm_destroy(vm);
}

static const char* const kWorkScript = R"(
class Cell {
  var value: int
}

var total: int = 0

offer fun Work(seed: int) -> int {
  var sum = 0
  for i in 0..100 {
    const cell = Cell(seed + i)
    sum = sum + cell.value
  }
  total = total + 1
  return sum
}

offer fun Total() -> int {
  return total
}
)";

static int32_t RunWork(WolangVm* vm, int32_t seed, int32_t times) {
  int32_t wrong = 0;
  for (int32_t i = 0; i < times; ++i) {
    WolangValue argument = wolang_int(seed);
    WolangValue answer = wolang_void();
    if (wolang_vm_call(vm, "Work", &argument, 1, &answer) != 0 || answer.integer != seed * 100 + 4950) {
      ++wrong;
    }
  }
  return wrong;
}

WOLANG_TEST(EmbedTest, EachThreadRunsItsOwnVm) {
  std::vector<int32_t> wrong(4, 0);
  std::vector<std::thread> threads;
  for (int32_t t = 0; t < 4; ++t) {
    threads.emplace_back([t, &wrong] {
      WolangVm* vm = wolang_vm_create();
      if (wolang_vm_load_source(vm, "work.wo", kWorkScript) != 0) {
        std::fprintf(stderr, "%s\n", wolang_vm_errors(vm));
        wrong[t] = -1;
      }
      else {
        wrong[t] = RunWork(vm, t, 200);
      }
      wolang_vm_destroy(vm);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (int32_t count : wrong) {
    WOLANG_EXPECT_EQ(count, 0);
  }
}

WOLANG_TEST(EmbedTest, ThreadsShareOneVmOneCallAtATime) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "work.wo", kWorkScript), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");
  std::vector<int32_t> wrong(4, 0);
  std::vector<std::thread> threads;
  for (int32_t t = 0; t < 4; ++t) {
    threads.emplace_back([t, vm, &wrong] {
      wrong[t] = RunWork(vm, t, 200);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (int32_t count : wrong) {
    WOLANG_EXPECT_EQ(count, 0);
  }

  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Total", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 800);
  wolang_vm_destroy(vm);
}

static void WriteFile(const std::filesystem::path& path, const char* text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

WOLANG_TEST(EmbedTest, AReloadKeepsWhatTheProgramHad) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "wolang_reload_test.wo";
  WriteFile(path, R"(
class Player {
  var hp: int
}

var score: int = 0
var player: Player = Player(10)
var label: text = "start"
var level: int = 1

offer fun Hit() {
  score = score + 1
  player.hp = player.hp - 1
  label = "hit"
  level = level + 1
}

offer fun Score() -> int {
  return score
}
)");

  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load(vm, path.string().c_str()), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");
  WolangFunction* score = wolang_vm_function(vm, "Score");
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Hit", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Hit", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(wolang_call(score, nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 2);

  WriteFile(path, R"(
class Player {
  var hp: int
  var mana: int
}

var score: int = 0
var player: Player = Player(10, 50)
var label: text = "start"
var level: text = "one"
var bonus: f64 = 1.5

offer fun Score() -> int {
  return score * 100
}

offer fun Hp() -> int {
  return player.hp
}

offer fun Mana() -> int {
  return player.mana
}

offer fun Label() -> text {
  return label
}

offer fun Level() -> text {
  return level
}
)");
  WOLANG_EXPECT_EQ(wolang_vm_reload(vm), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(vm)), "");
  WOLANG_EXPECT_EQ(wolang_call(score, nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 200);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Hp", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 8);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Mana", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 50);
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Label", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), "hit");
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Level", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(TextOf(answer), "one");
  WOLANG_EXPECT_EQ(std::string(wolang_vm_notes(vm)), "`level` starts again: its type changed\n");
  wolang_vm_destroy(vm);
  std::filesystem::remove(path);
}

WOLANG_TEST(EmbedTest, EachVmHasItsOwnGlobals) {
  WolangVm* first = wolang_vm_create();
  WolangVm* second = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(first, "work.wo", kWorkScript), 0);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(second, "work.wo", kWorkScript), 0);
  WOLANG_EXPECT_EQ(RunWork(first, 1, 3), 0);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(first, "Total", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 3);
  WOLANG_EXPECT_EQ(wolang_vm_call(second, "Total", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 0);
  wolang_vm_destroy(first);
  wolang_vm_destroy(second);
}

// An editor at the other end of a vm's debugging port.
class EditorConnection {
 public:
  explicit EditorConnection(int32_t port) : m_Socket(wolang::runtime::wolang_socket_open()) {
    wolang::runtime::wolang_socket_connect(m_Socket, 0x7F000001u, static_cast<uint32_t>(port));
  }

  ~EditorConnection() { wolang::runtime::wolang_socket_close(m_Socket); }

  void Send(const std::string& command, const std::string& arguments) {
    const std::string body = "{\"seq\":" + std::to_string(++m_Sequence) + ",\"type\":\"request\",\"command\":\"" + command + "\",\"arguments\":" + arguments + "}";
    const std::string message = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    wolang::runtime::wolang_socket_send(m_Socket, reinterpret_cast<const uint8_t*>(message.data()), static_cast<int64_t>(message.size()));
  }

  // The next message called |name|, whatever came before it.
  wolang::lsp::Json WaitFor(const std::string& name) {
    for (;;) {
      const size_t header = m_Pending.find("\r\n\r\n");
      if (header != std::string::npos) {
        const size_t length = static_cast<size_t>(std::strtoull(m_Pending.c_str() + m_Pending.find("Content-Length:") + 15, nullptr, 10));
        if (m_Pending.size() >= header + 4 + length) {
          wolang::lsp::Json message = wolang::lsp::Json::Parse(m_Pending.substr(header + 4, length));
          m_Pending.erase(0, header + 4 + length);
          if (message["event"].AsString() == name || message["command"].AsString() == name) {
            return message;
          }
          continue;
        }
      }
      uint8_t chunk[4096];
      const int64_t got = wolang::runtime::wolang_socket_receive(m_Socket, chunk, sizeof(chunk));
      if (got <= 0) {
        return wolang::lsp::Json();
      }
      m_Pending.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(got));
    }
  }

 private:
  int64_t m_Socket;
  int64_t m_Sequence = 0;
  std::string m_Pending;
};

WOLANG_TEST(EmbedTest, AnEditorAttachesAndStopsTheHostsCall) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "attached.wo", R"(
var ticks: int = 0

offer fun Tick(delta: int) -> int {
  const next = ticks + delta
  ticks = next
  return ticks
}
)"), 0);
  const int32_t port = wolang_vm_debug_listen(vm, 0);
  WOLANG_EXPECT(port > 0);

  std::string reason;
  std::string function;
  int64_t line = 0;
  std::string delta;
  std::atomic<bool> stopped_once{false};
  std::thread editor([&] {
    EditorConnection connection(port);
    connection.Send("initialize", "{}");
    connection.WaitFor("initialized");
    connection.Send("attach", "{}");
    connection.WaitFor("attach");
    connection.Send("setBreakpoints", "{\"source\":{\"path\":\"attached.wo\"},\"breakpoints\":[{\"line\":6}]}");
    connection.WaitFor("setBreakpoints");
    connection.Send("configurationDone", "{}");
    connection.WaitFor("configurationDone");

    const wolang::lsp::Json stopped = connection.WaitFor("stopped");
    reason = stopped["body"]["reason"].AsString();
    stopped_once = true;
    connection.Send("stackTrace", "{\"threadId\":1}");
    const wolang::lsp::Json trace = connection.WaitFor("stackTrace");
    function = trace["body"]["stackFrames"].At(0)["name"].AsString();
    line = trace["body"]["stackFrames"].At(0)["line"].AsInt();
    connection.Send("variables", "{\"variablesReference\":1}");
    const wolang::lsp::Json variables = connection.WaitFor("variables");
    delta = variables["body"]["variables"].At(0)["value"].AsString();
    connection.Send("disconnect", "{}");
    connection.WaitFor("disconnect");
  });

  // The host's frame, which waits at the breakpoint until the editor lets
  // it go. The editor may not have set its breakpoint yet, so the host
  // keeps ticking until it has been stopped once.
  WolangValue argument = wolang_int(3);
  WolangValue answer = wolang_void();
  int32_t calls = 0;
  while (!stopped_once && calls < 2000) {
    WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Tick", &argument, 1, &answer), 0);
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  editor.join();
  WOLANG_EXPECT_EQ(reason, "breakpoint");
  WOLANG_EXPECT_EQ(function, "Tick");
  WOLANG_EXPECT_EQ(line, 6);
  WOLANG_EXPECT_EQ(delta, "3");

  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Tick", &argument, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, static_cast<int64_t>(calls + 1) * 3);
  wolang_vm_destroy(vm);
}

// A package on disk: a manifest and one source file.
static std::filesystem::path WritePackage(const std::string& name, const char* manifest_extra, const char* source) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / ("wolang_" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "src");
  const std::string manifest = "[package]\nname = \"" + name + "\"\nsource = \"src\"\n" + manifest_extra;
  WriteFile(root / "wolang.toml", manifest.c_str());
  WriteFile(root / "src" / "main.wo", source);
  return root;
}

WOLANG_TEST(EmbedTest, AModGetsWhatTheHostGrantsAndNothingMore) {
  const std::string std_path = std::string(WOLANG_PROJECT_DIR) + "/lib/std";
  const std::string dependency = "\n[dependencies]\nstd = { path = \"" + std_path + "\" }\n";

  // Reading a file is `io` all the way down now, through `std.fs`.
  const std::filesystem::path reader = WritePackage("modreader", dependency.c_str(), R"(
need std.fs

offer fun Peek() -> int uses io {
  return fs.SizeOf("wolang.toml") as int
}
)");
  WolangVm* granted = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_mod(granted, reader.string().c_str(), "io"), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(granted)), "");
  wolang_vm_destroy(granted);

  WolangVm* denied = wolang_vm_create();
  WOLANG_EXPECT(wolang_vm_load_mod(denied, reader.string().c_str(), "") != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(denied)).find("E0703") != std::string::npos);
  wolang_vm_destroy(denied);

  // A manifest that claims more than the host grants gets the host's.
  const std::string claims = "\n[build]\ncapabilities = \"io, net\"\n" + dependency;
  const std::filesystem::path greedy = WritePackage("modgreedy", claims.c_str(), R"(
need std.fs

offer fun Peek() -> int uses io {
  return fs.SizeOf("wolang.toml") as int
}
)");
  WolangVm* narrowed = wolang_vm_create();
  WOLANG_EXPECT(wolang_vm_load_mod(narrowed, greedy.string().c_str(), "time") != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(narrowed)).find("E0703") != std::string::npos);
  wolang_vm_destroy(narrowed);

  // No door of its own: an `extern`, even one never called, and `unsafe`.
  const std::filesystem::path door = WritePackage("moddoor", "", R"(
extern class wolang {
  fun system(command: *u8) -> s32
}

offer fun Nothing() -> int {
  return 0
}
)");
  WolangVm* sandboxed = wolang_vm_create();
  WOLANG_EXPECT(wolang_vm_load_mod(sandboxed, door.string().c_str(), "io") != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(sandboxed)).find("E0705") != std::string::npos);
  wolang_vm_destroy(sandboxed);

  const std::filesystem::path raw = WritePackage("modraw", "", R"(
offer fun Poke(address: u64) -> u8 uses unsafe {
  return *(address as *u8)
}
)");
  WolangVm* poking = wolang_vm_create();
  WOLANG_EXPECT(wolang_vm_load_mod(poking, raw.string().c_str(), "io") != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(poking)).find("unsafe") != std::string::npos);
  wolang_vm_destroy(poking);

  for (const std::filesystem::path& root : {reader, greedy, door, raw}) {
    std::filesystem::remove_all(root);
  }

  // A bodiless `uses unsafe` primitive hands its obligation to the caller,
  // so a mod cannot reach it by leaving `unsafe` out (D221).
  const std::filesystem::path prim = WritePackage("modprim", "", R"(
need std.mem.{ArrayNew}

offer fun Grab() -> int {
  const room = ArrayNew<u8>(8)
  return room.len
}
)");
  WolangVm* grabbing = wolang_vm_create();
  WOLANG_EXPECT(wolang_vm_load_mod(grabbing, prim.string().c_str(), "io") != 0);
  wolang_vm_destroy(grabbing);
  std::filesystem::remove_all(prim);
}

WOLANG_TEST(EmbedTest, ASandboxedModCannotHideAnExternInsideAType) {
  // The door check walked only top-level items, so an `extern` in a struct
  // body or an `extend` got through (D217).
  const char* const nested[] = {R"(
struct Holder {
  var n: int

  extern "C" fun system(command: *u8) -> s32
}

offer fun Nothing() -> int {
  return 0
}
)", R"(
struct Holder {
  var n: int
}

extend Holder {
  extern "C" fun system(command: *u8) -> s32
}

offer fun Nothing() -> int {
  return 0
}
)"};
  int which = 0;
  for (const char* source : nested) {
    const std::filesystem::path root =
        WritePackage("modnested" + std::to_string(which++), "", source);
    WolangVm* vm = wolang_vm_create();
    WOLANG_EXPECT(wolang_vm_load_mod(vm, root.string().c_str(), "io") != 0);
    WOLANG_EXPECT(std::string(wolang_vm_errors(vm)).find("E0705") !=
                  std::string::npos);
    wolang_vm_destroy(vm);
    std::filesystem::remove_all(root);
  }

  // The twin: a plain method in the same places is fine.
  const std::filesystem::path plain = WritePackage("modplain", "", R"(
struct Holder {
  var n: int

  fun Get() -> int => n
}

extend Holder {
  fun Twice() -> int => n * 2
}

offer fun Nothing() -> int {
  return 0
}
)");
  WolangVm* ok = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_mod(ok, plain.string().c_str(), "io"), 0);
  WOLANG_EXPECT_EQ(std::string(wolang_vm_errors(ok)), "");
  wolang_vm_destroy(ok);
  std::filesystem::remove_all(plain);
}

WOLANG_TEST(EmbedTest, CallsBackIntoTheHost) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(
      wolang_vm_bind(vm, "test_host_double",
                     reinterpret_cast<void*>(&test_host_double)),
      0);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "callback.wo", R"(
extern "C" fun test_host_double(value: s32) -> s32

offer fun Quadruple(value: int) -> int uses unsafe {
  return test_host_double(test_host_double(value as s32)) as int
}
)"), 0);

  WolangValue argument = wolang_int(5);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Quadruple", &argument, 1, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 20);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, SaysWhatIsWrongWithAProgram) {
  WolangVm* vm = wolang_vm_create();
  const int32_t loaded =
      wolang_vm_load_source(vm, "broken.wo", "offer fun F() -> int => \"x\"");
  WOLANG_EXPECT(loaded != 0);
  WOLANG_EXPECT(wolang_vm_ok(vm) == 0);
  // The same diagnostic the command line gives, rendered without colour.
  const std::string errors(wolang_vm_errors(vm));
  WOLANG_EXPECT(errors.find("expected `s32`") != std::string::npos);
  WOLANG_EXPECT(errors.find("broken.wo") != std::string::npos);

  // And a call into a program with errors is refused rather than run.
  WOLANG_EXPECT(wolang_vm_call(vm, "F", nullptr, 0, nullptr) != 0);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, RefusesTheWrongNumberOfArguments) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(
      wolang_vm_load_source(vm, "arity.wo", "offer fun Add(a: int, b: int) -> int => a + b"),
      0);
  WolangValue one = wolang_int(1);
  WOLANG_EXPECT(wolang_vm_call(vm, "Add", &one, 1, nullptr) != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(vm)).find("takes 2") !=
                     std::string::npos);
  WOLANG_EXPECT(wolang_vm_call(vm, "Missing", nullptr, 0, nullptr) != 0);
  wolang_vm_destroy(vm);
}

namespace {

std::string captured;

void Capture(void* user, const char* bytes, uint64_t length) {
  (void)user;
  captured.append(bytes, static_cast<size_t>(length));
}

}  // namespace

WOLANG_TEST(EmbedTest, CallsThroughAHandle) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "frame.wo", R"(
/// Moves one step.
offer fun Update(delta: f32) -> f32 => delta * 2.0
)"),
                   0);

  // What an engine does once rather than per frame (D137).
  WolangFunction* update = wolang_vm_function(vm, "Update");
  WOLANG_EXPECT(update != nullptr);
  WOLANG_EXPECT_EQ(wolang_function_arity(update), 1);
  WOLANG_EXPECT_EQ(wolang_function_parameter(update, 0), kWolangFloat);
  WOLANG_EXPECT_EQ(wolang_function_result(update), kWolangFloat);

  WolangValue delta = wolang_float(0.25);
  WolangValue answer = wolang_void();
  for (int frame = 0; frame < 3; ++frame) {
    WOLANG_EXPECT_EQ(wolang_call(update, &delta, 1, &answer), 0);
    WOLANG_EXPECT(answer.number > 0.49 && answer.number < 0.51);
  }
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, ReadsAndWritesGlobals) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "config.wo", R"(
offer var gravity: f32 = -9.8
offer var level: int = 1
offer var paused: bool = false

offer fun Describe() -> int => level * 10
)"),
                   0);

  // What a host configures a script with.
  WolangValue held = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_get_global(vm, "gravity", &held), 0);
  WOLANG_EXPECT(held.number < -9.0 && held.number > -10.0);

  WOLANG_EXPECT_EQ(wolang_vm_set_global(vm, "level", wolang_int(7)), 0);
  WOLANG_EXPECT_EQ(wolang_vm_get_global(vm, "level", &held), 0);
  WOLANG_EXPECT_EQ(held.integer, 7);

  // And the program sees what the host wrote.
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Describe", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 70);

  WOLANG_EXPECT_EQ(wolang_vm_set_global(vm, "paused", wolang_bool(1)), 0);
  WOLANG_EXPECT_EQ(wolang_vm_get_global(vm, "paused", &held), 0);
  WOLANG_EXPECT_EQ(held.boolean, 1);

  WOLANG_EXPECT(wolang_vm_get_global(vm, "missing", &held) != 0);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, SendsOutputToTheHost) {
  captured.clear();
  WolangVm* vm = wolang_vm_create();
  wolang_vm_set_writer(vm, &Capture, nullptr);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "talk.wo", R"(
extern "C" fun write(fd: s32, buffer: *u8, count: u64) -> s64

offer fun Say() -> int uses unsafe {
  const message = "from the script\n"
  const _written = write(1, message.data, message.length)
  return 1
}
)"),
                   0);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Say", nullptr, 0, &answer), 0);
  // The host's writer saw it instead of standard output (D137).
  WOLANG_EXPECT_EQ(captured, std::string("from the script\n"));
  wolang_vm_set_writer(vm, nullptr, nullptr);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, StopsAScriptThatNeverEnds) {
  WolangVm* vm = wolang_vm_create();
  wolang_vm_set_step_limit(vm, 100000);
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "forever.wo", R"(
offer fun Spin() -> int {
  var i = 0
  loop {
    i = i + 1
  }
}
)"),
                   0);
  // A host running code it did not write gets an error rather than a
  // hung process (D137).
  WOLANG_EXPECT(wolang_vm_call(vm, "Spin", nullptr, 0, nullptr) != 0);
  WOLANG_EXPECT(std::string(wolang_vm_errors(vm)).find("step limit") !=
                std::string::npos);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, ReloadsWhatItLoaded) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(vm, "mod.wo",
                                         "offer fun N() -> int => 1"),
                   0);
  WOLANG_EXPECT_EQ(wolang_vm_file_count(vm), 1);
  // Reloading source given by the host loads the same source again: the
  // point of the call is a *file* that changed, and this at least says
  // the machinery works.
  WOLANG_EXPECT_EQ(wolang_vm_reload(vm), 0);
  WolangValue answer = wolang_void();
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "N", nullptr, 0, &answer), 0);
  WOLANG_EXPECT_EQ(answer.integer, 1);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, ReportsItsAbiVersion) {
  WOLANG_EXPECT_EQ(wolang_abi_version(), WOLANG_ABI_VERSION);
  WOLANG_EXPECT_EQ(wolang_abi_version() >> 16, 1u);
}

WOLANG_TEST(EmbedTest, ValueLayoutAgreesBetweenCAndCxx) {
  WOLANG_EXPECT_EQ(wolang_abi_c_check_size(),
                   static_cast<uint32_t>(sizeof(WolangValue)));
}

WOLANG_TEST(EmbedTest, CreatesWithOptionsOfAKnownSize) {
  WolangVmOptions options;
  std::memset(&options, 0, sizeof(options));
  options.size = sizeof(options);
  WolangVm* vm = wolang_vm_create_with(&options);
  WOLANG_ASSERT(vm != nullptr);
  wolang_vm_destroy(vm);
}

WOLANG_TEST(EmbedTest, RefusesOptionsOfAnUnknownSize) {
  WolangVmOptions options;
  std::memset(&options, 0, sizeof(options));
  options.size = 0;
  WOLANG_EXPECT(wolang_vm_create_with(&options) == nullptr);
  options.size = sizeof(options) + 8;
  WOLANG_EXPECT(wolang_vm_create_with(&options) == nullptr);
  WOLANG_EXPECT(wolang_vm_create_with(nullptr) == nullptr);
}

WOLANG_TEST(EmbedTest, CallsAnswerAStatus) {
  WolangVm* vm = wolang_vm_create();
  WOLANG_EXPECT_EQ(wolang_vm_load_source(
                       vm, "s.wo",
                       "offer fun Id(a: int) -> int => a\n"),
                   kWolangOk);
  WolangValue out = wolang_void();
  WolangValue args[1] = {wolang_int(5)};
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Id", args, 1, &out), kWolangOk);
  WOLANG_EXPECT_EQ(out.kind, static_cast<uint32_t>(kWolangInt));
  WOLANG_EXPECT_EQ(out.integer, int64_t{5});
  WOLANG_EXPECT_EQ(wolang_vm_call(vm, "Nope", nullptr, 0, &out), kWolangError);
  wolang_vm_destroy(vm);
}
