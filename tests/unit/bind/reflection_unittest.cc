// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// What `wolang bind --reflection` makes of an engine's reflection.

#include "wolang/bind/reflection.h"

#include <string>
#include <vector>

#include "harness/test.h"
#include "wolang/base/diagnostic.h"
#include "wolang/compiler/database.h"

namespace {

const char* const kReflection = R"({
  "module": "MowoEngine.Game",
  "formatVersion": 1,
  "headers": [
    {
      "include": "Game/AActor.h",
      "types": [
        {
          "name": "AActor", "qualifiedName": "Mowo::AActor", "kind": "class", "super": "MObject",
          "properties": [
            {"name": "tickInterval", "type": "float", "key": "tickInterval", "flags": ["EditAnywhere"]},
            {"name": "m_health", "type": "int32_t", "key": "health", "flags": ["VisibleAnywhere"]},
            {"name": "m_world", "type": "TObjectPtr<MWorld>", "key": "world"}
          ],
          "functions": [
            {"name": "GetActorLocation", "returnType": "Vec3", "params": [], "flags": ["Const"]},
            {"name": "SetActorLocation", "returnType": "void", "params": [{"name": "location", "type": "const Vec3 &"}]},
            {"name": "FindTarget", "returnType": "AActor *", "params": [{"name": "type", "type": "ETeam"}, {"name": "tag", "type": "const std::string &"}]},
            {"name": "IsHidden", "returnType": "bool", "params": []}
          ]
        },
        {
          "name": "APawn", "qualifiedName": "Mowo::APawn", "kind": "class", "super": "AActor",
          "properties": [],
          "functions": [
            {"name": "IsHidden", "returnType": "bool", "params": []},
            {"name": "Speed", "returnType": "float", "params": []}
          ]
        },
        {"name": "FHealthComponent", "qualifiedName": "Mowo::FHealthComponent", "kind": "component", "properties": [], "functions": []}
      ],
      "enums": [{"name": "ETeam", "qualifiedName": "Mowo::ETeam", "scoped": true, "entries": []}],
      "delegates": []
    }
  ]
})";

const char* const kValues = R"({
  "module": "MowoEngine.Game",
  "formatVersion": 1,
  "headers": [
    {
      "include": "Game/AActor.h",
      "types": [
        {
          "name": "AActor", "qualifiedName": "Mowo::AActor", "kind": "class", "super": "",
          "properties": [
            {"name": "m_world", "type": "TObjectPtr<MWorld>", "key": "world"},
            {"name": "m_spin", "type": "Quat", "key": "spin"},
            {"name": "m_facing", "type": "FRotator", "key": "facing"}
          ],
          "functions": [
            {"name": "GetSnapshot", "returnType": "FSnapshot", "params": []},
            {"name": "Make", "returnType": "AActor *", "params": [{"name": "seed", "type": "int32_t"}], "flags": ["Static"]},
            {"name": "Count", "returnType": "int32_t", "params": [], "flags": ["Static"]}
          ]
        },
        {
          "name": "MWorld", "qualifiedName": "Mowo::MWorld", "kind": "class", "super": "",
          "properties": [],
          "functions": [
            {"name": "Count", "returnType": "int32_t", "params": [], "flags": ["Static"]}
          ]
        },
        {
          "name": "FSnapshot", "qualifiedName": "Mowo::FSnapshot", "kind": "struct",
          "properties": [
            {"name": "frame", "type": "uint64_t", "key": "frame"},
            {"name": "m_fps", "type": "float", "key": "fps"}
          ],
          "functions": []
        },
        {
          "name": "FPair", "qualifiedName": "Mowo::FPair", "kind": "struct",
          "properties": [{"name": "name", "type": "std::string", "key": "name"}],
          "functions": []
        }
      ],
      "enums": [],
      "delegates": []
    }
  ]
})";

const char* const kText = R"({
  "module": "MowoEngine.Game",
  "formatVersion": 1,
  "headers": [
    {
      "include": "Game/AActor.h",
      "types": [
        {
          "name": "AActor", "qualifiedName": "Mowo::AActor", "kind": "class", "super": "",
          "properties": [{"name": "m_label", "type": "std::string", "key": "label"}],
          "functions": [
            {"name": "GetActorName", "returnType": "std::string", "params": []},
            {"name": "Find", "returnType": "AActor *", "params": [{"name": "tag", "type": "const std::string &"}], "flags": ["Static"]}
          ]
        }
      ],
      "enums": [],
      "delegates": []
    }
  ]
})";

std::string Errors(const std::string& source) {
  wolang::Database database;
  const wolang::FileId file = database.AddFile("mowo.wo", source);
  std::string errors;
  if (const std::vector<wolang::Diagnostic>* diagnostics = database.Diagnostics(file)) {
    for (const wolang::Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity == wolang::Severity::kError) {
        errors += diagnostic.code + " " + diagnostic.message + "\n";
      }
    }
  }
  return errors;
}

bool Holds(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

WOLANG_TEST(ReflectionBindTest, DeclaresTheEngineClassesAScriptCanCall) {
  wolang::bind::ReflectionOptions options;
  options.effect = "World";
  const wolang::bind::ReflectionResult result = wolang::bind::GenerateFromReflection({kReflection}, options);
  const std::string& out = result.source;

  WOLANG_EXPECT_EQ(result.classes, 2);
  WOLANG_EXPECT(Holds(out, "offer effect World"));
  WOLANG_EXPECT(Holds(out, "offer extern class AActor {"));
  WOLANG_EXPECT(Holds(out, "offer extern class APawn: AActor {"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetActorLocation(out: *mut Vec3) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  offer fun GetActorLocation() -> Vec3 uses World, unsafe {"));
  WOLANG_EXPECT(Holds(out, "  fun RawFindTarget(typeValue: s32, tagValue: *u8, tagValueLength: u64) -> AActor uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun GetTickInterval() -> f32 uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun SetTickInterval(value: f32) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun GetHealth() -> s32 uses World\n"));
  WOLANG_EXPECT(!Holds(out, "SetHealth"));
  WOLANG_EXPECT(Holds(out, "  fun Speed() -> f32 uses World\n"));

  bool said_component = false;
  bool said_world = false;
  for (const std::string& skipped : result.skipped) {
    said_component = said_component || Holds(skipped, "FHealthComponent");
    said_world = said_world || Holds(skipped, "AActor.m_world");
  }
  WOLANG_EXPECT(said_component);
  WOLANG_EXPECT(said_world);

  const std::string& glue = result.glue;
  WOLANG_EXPECT(Holds(glue, "#include \"Game/AActor.h\""));
  WOLANG_EXPECT(Holds(glue, "extern \"C\" void AActor_RawGetActorLocation(void* self, WolangVec3* out) {"));
  WOLANG_EXPECT(Holds(glue, "static_cast<Mowo::ETeam>(typeValue)"));
  WOLANG_EXPECT(Holds(glue, "MowoWolangPropertyOffset(\"AActor\", \"tickInterval\")"));
  WOLANG_EXPECT(Holds(glue, "extern \"C\" void MowoWolang_Bind_mowo(WolangVm* vm) {"));
  WOLANG_EXPECT(Holds(glue, "wolang_vm_register_class(vm, \"APawn\""));
}

WOLANG_TEST(ReflectionBindTest, WhatItWritesIsWolangThatChecks) {
  wolang::bind::ReflectionOptions options;
  options.effect = "World";
  const wolang::bind::ReflectionResult result = wolang::bind::GenerateFromReflection({kReflection}, options);

  WOLANG_EXPECT_EQ(Errors(result.source), "");
}

WOLANG_TEST(ReflectionBindTest, ValuesObjectsAndStaticsCross) {
  wolang::bind::ReflectionOptions options;
  options.effect = "World";
  const wolang::bind::ReflectionResult result = wolang::bind::GenerateFromReflection({kValues}, options);
  const std::string& out = result.source;
  const std::string& glue = result.glue;

  WOLANG_EXPECT(Holds(out, "offer struct FSnapshot {\n  var frame: u64\n  var fps: f32\n}"));
  WOLANG_EXPECT(Holds(out, "offer struct Rotator {\n  var pitch: f32\n  var yaw: f32\n  var roll: f32\n}"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetSnapshot(out: *mut FSnapshot) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  offer fun GetSnapshot() -> FSnapshot uses World, unsafe {"));
  WOLANG_EXPECT(Holds(out, "    var out = FSnapshot(0, 0.0)\n"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetSpin(out: *mut Quat) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetFacing(out: *mut Rotator) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun GetWorld() -> MWorld uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun SetWorld(value: MWorld) uses World\n"));
  WOLANG_EXPECT(Holds(out, "extern \"C\" fun AActor_Make(seed: s32) -> AActor uses World\n"));
  WOLANG_EXPECT(Holds(out, "offer fun Make(seed: s32) -> AActor uses World, unsafe {\n  return AActor_Make(seed)\n}\n"));
  WOLANG_EXPECT(Holds(out, "offer fun ActorCount() -> s32 uses World, unsafe {"));
  WOLANG_EXPECT(Holds(out, "offer fun WorldCount() -> s32 uses World, unsafe {"));
  WOLANG_EXPECT(!Holds(out, "offer fun Count("));

  WOLANG_EXPECT(Holds(glue, "#include \"Game/AActor.h\""));
  WOLANG_EXPECT(Holds(glue, "struct WolangFSnapshot {\n\tuint64_t frame;\n\tfloat fps;\n};"));
  WOLANG_EXPECT(Holds(glue, "extern \"C\" void AActor_RawGetSnapshot(void* self, WolangFSnapshot* out) {"));
  WOLANG_EXPECT(Holds(glue, "\tout->fps = value.m_fps;\n"));
  WOLANG_EXPECT(Holds(glue, "Quat(value->x, value->y, value->z, value->w)"));
  WOLANG_EXPECT(Holds(glue, "FRotator(value->pitch, value->yaw, value->roll)"));
  WOLANG_EXPECT(Holds(glue, "\tout->pitch = value.Pitch;\n"));
  WOLANG_EXPECT(Holds(glue, "reinterpret_cast<const TObjectPtr<MWorld>*>(static_cast<char*>(self) + offset)->Get()"));
  WOLANG_EXPECT(Holds(glue, "extern \"C\" void* AActor_Make(int32_t seed) {"));
  WOLANG_EXPECT(Holds(glue, "Mowo::AActor::Make(seed)"));
  WOLANG_EXPECT(Holds(glue, "extern \"C\" int32_t AActor_Count() {"));
  WOLANG_EXPECT(!Holds(glue, "AActor_Count(void* self"));

  bool said_pair = false;
  for (const std::string& skipped : result.skipped) {
    said_pair = said_pair || Holds(skipped, "FPair");
  }
  WOLANG_EXPECT(said_pair);
  WOLANG_EXPECT(!Holds(out, "FPair"));
}

WOLANG_TEST(ReflectionBindTest, ValuesObjectsAndStaticsCheck) {
  wolang::bind::ReflectionOptions options;
  options.effect = "World";
  const wolang::bind::ReflectionResult result = wolang::bind::GenerateFromReflection({kValues}, options);
  WOLANG_EXPECT_EQ(Errors(result.source), "");
}

WOLANG_TEST(ReflectionBindTest, TextCrossesAsACopy) {
  wolang::bind::ReflectionOptions options;
  options.effect = "World";
  const wolang::bind::ReflectionResult result = wolang::bind::GenerateFromReflection({kText}, options);
  const std::string& out = result.source;
  const std::string& glue = result.glue;

  WOLANG_EXPECT(Holds(out, "need std.text.{TextFromRaw}"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetActorName(resultLength: *mut u64) -> *u8 uses World\n"));
  WOLANG_EXPECT(Holds(out, "  offer fun GetActorName() -> text uses World, unsafe {\n    var resultLength: u64 = 0\n"));
  WOLANG_EXPECT(Holds(out, "return TextFromRaw(resultData, resultLength as int) + \"\"\n"));
  WOLANG_EXPECT(Holds(out, "  fun RawGetLabel(resultLength: *mut u64) -> *u8 uses World\n"));
  WOLANG_EXPECT(Holds(out, "  fun RawSetLabel(value: *u8, valueLength: u64) uses World\n"));
  WOLANG_EXPECT(Holds(out, "  offer fun SetLabel(value: text) uses World, unsafe {"));
  WOLANG_EXPECT(Holds(out, "offer fun Find(tagValue: text) -> AActor uses World, unsafe {"));

  WOLANG_EXPECT(Holds(glue, "extern \"C\" const char* AActor_RawGetActorName(void* self, uint64_t* resultLength) {"));
  WOLANG_EXPECT(Holds(glue, "static thread_local std::string held;\n\theld = static_cast<Mowo::AActor*>(self)->GetActorName();"));
  WOLANG_EXPECT(Holds(glue, "\t*resultLength = held.size();\n\treturn held.c_str();\n"));
  WOLANG_EXPECT(Holds(glue, "held = *reinterpret_cast<const std::string*>(static_cast<char*>(self) + offset);"));
  WOLANG_EXPECT(Holds(glue, "Mowo::AActor::Find(std::string(tagValue, static_cast<size_t>(tagValueLength)))"));

  bool said_text = false;
  for (const std::string& skipped : result.skipped) {
    said_text = said_text || Holds(skipped, "GetActorName") || Holds(skipped, "m_label");
  }
  WOLANG_EXPECT(!said_text);
}

WOLANG_TEST(ReflectionBindTest, TheGlueAsksIfTheObjectIsAlive) {
  wolang::bind::ReflectionOptions options;
  const wolang::bind::ReflectionResult checked = wolang::bind::GenerateFromReflection({kReflection}, options);
  WOLANG_EXPECT(Holds(checked.glue, "if (self == nullptr || MowoWolangAlive(self, nullptr) == 0) {"));
  WOLANG_EXPECT(Holds(checked.glue, "if (self == nullptr || MowoWolangAlive(self, nullptr) == 0 || offset == UINT32_MAX) {"));

  options.check_alive = false;
  const wolang::bind::ReflectionResult bare = wolang::bind::GenerateFromReflection({kReflection}, options);
  WOLANG_EXPECT(Holds(bare.glue, "if (self == nullptr) {"));
  WOLANG_EXPECT(Holds(bare.glue, "if (self == nullptr || offset == UINT32_MAX) {"));
  WOLANG_EXPECT(!Holds(bare.glue, "MowoWolangAlive(self"));
}
