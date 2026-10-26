// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// The embedding API, exercised the way a host does: load something, call
// it, hand a function back in.

#include "wolang/embed/wolang.h"

#include <cstring>
#include <string>

#include "harness/test.h"

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
