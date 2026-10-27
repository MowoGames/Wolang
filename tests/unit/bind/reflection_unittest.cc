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
            {"name": "GetActorName", "returnType": "std::string", "params": []},
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
  WOLANG_EXPECT(!Holds(out, "GetActorName"));
  WOLANG_EXPECT(Holds(out, "  fun Speed() -> f32 uses World\n"));

  bool said_name = false;
  bool said_component = false;
  bool said_world = false;
  for (const std::string& skipped : result.skipped) {
    said_name = said_name || Holds(skipped, "AActor.GetActorName");
    said_component = said_component || Holds(skipped, "FHealthComponent");
    said_world = said_world || Holds(skipped, "AActor.m_world");
  }
  WOLANG_EXPECT(said_name);
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

  wolang::Database database;
  const wolang::FileId file = database.AddFile("mowo.wo", result.source);
  std::string errors;
  if (const std::vector<wolang::Diagnostic>* diagnostics = database.Diagnostics(file)) {
    for (const wolang::Diagnostic& diagnostic : *diagnostics) {
      if (diagnostic.severity == wolang::Severity::kError) {
        errors += diagnostic.code + " " + diagnostic.message + "\n";
      }
    }
  }
  WOLANG_EXPECT_EQ(errors, "");
}
