// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Hundreds of actors ticked every frame, the way an engine ticks them: a
// script's `Tick` per actor, against the same logic in C++ behind virtual
// calls (D219).
//
//   wolang_actor_bench [benchmarks/actors/actors.wo]

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "wolang/embed/wolang.h"

#if defined(WOLANG_ACTORS_AOT)
extern "C" void actors_start(void);
extern "C" float actors_Tick(void* target, float delta);
#endif

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

class Actor {
 public:
  virtual ~Actor() = default;

  virtual Vec3 GetLocation() const { return m_Location; }
  virtual void SetLocation(const Vec3& location) { m_Location = location; }
  virtual Vec3 GetVelocity() const { return m_Velocity; }
  virtual void SetVelocity(const Vec3& velocity) { m_Velocity = velocity; }
  virtual float GetHealth() const { return m_Health; }
  virtual void SetHealth(float health) { m_Health = health; }

 private:
  Vec3 m_Location;
  Vec3 m_Velocity;
  float m_Health = 100.0f;
};

static float NativeTick(Actor* target, float delta) {
  Vec3 spot = target->GetLocation();
  Vec3 speed = target->GetVelocity();

  spot.x += speed.x * delta;
  spot.y += speed.y * delta;
  spot.z += speed.z * delta;
  if (spot.x > 100.0f || spot.x < -100.0f) {
    speed.x = -speed.x;
  }
  if (spot.y > 100.0f || spot.y < -100.0f) {
    speed.y = -speed.y;
  }
  if (spot.z > 100.0f || spot.z < 0.0f) {
    speed.z = -speed.z;
  }

  float health = target->GetHealth() - delta * 0.5f;
  if (health < 0.0f) {
    health = 100.0f;
  }
  target->SetHealth(health);
  target->SetLocation(spot);
  target->SetVelocity(speed);
  return health;
}

extern "C" void Actor_RawGetLocation(void* self, Vec3* out) {
  *out = static_cast<Actor*>(self)->GetLocation();
}

extern "C" void Actor_RawSetLocation(void* self, const Vec3* location) {
  static_cast<Actor*>(self)->SetLocation(*location);
}

extern "C" void Actor_RawGetVelocity(void* self, Vec3* out) {
  *out = static_cast<Actor*>(self)->GetVelocity();
}

extern "C" void Actor_RawSetVelocity(void* self, const Vec3* velocity) {
  static_cast<Actor*>(self)->SetVelocity(*velocity);
}

extern "C" float Actor_GetHealth(void* self) {
  return static_cast<Actor*>(self)->GetHealth();
}

extern "C" void Actor_SetHealth(void* self, float health) {
  static_cast<Actor*>(self)->SetHealth(health);
}

static std::vector<std::unique_ptr<Actor>> Spawn(int32_t count) {
  std::vector<std::unique_ptr<Actor>> actors;
  for (int32_t i = 0; i < count; ++i) {
    auto actor = std::make_unique<Actor>();
    const float f = static_cast<float>(i);
    Vec3 location;
    location.x = std::fmod(f * 7.0f, 200.0f) - 100.0f;
    location.y = std::fmod(f * 13.0f, 200.0f) - 100.0f;
    location.z = std::fmod(f * 3.0f, 100.0f);
    Vec3 velocity;
    velocity.x = 1.0f + std::fmod(f, 5.0f);
    velocity.y = 2.0f - std::fmod(f, 3.0f);
    velocity.z = 0.5f;
    actor->SetLocation(location);
    actor->SetVelocity(velocity);
    actors.push_back(std::move(actor));
  }
  return actors;
}

static WolangVm* Load(const std::string& script, bool compiled) {
  WolangVm* vm = wolang_vm_create();
  wolang_vm_set_compiled(vm, compiled ? 1 : 0);
  wolang_vm_register_class(vm, "Actor", nullptr, nullptr, nullptr, nullptr);
  wolang_vm_bind(vm, "Actor_RawGetLocation", reinterpret_cast<void*>(&Actor_RawGetLocation));
  wolang_vm_bind(vm, "Actor_RawSetLocation", reinterpret_cast<void*>(&Actor_RawSetLocation));
  wolang_vm_bind(vm, "Actor_RawGetVelocity", reinterpret_cast<void*>(&Actor_RawGetVelocity));
  wolang_vm_bind(vm, "Actor_RawSetVelocity", reinterpret_cast<void*>(&Actor_RawSetVelocity));
  wolang_vm_bind(vm, "Actor_GetHealth", reinterpret_cast<void*>(&Actor_GetHealth));
  wolang_vm_bind(vm, "Actor_SetHealth", reinterpret_cast<void*>(&Actor_SetHealth));
  if (wolang_vm_load(vm, script.c_str()) != 0) {
    std::fprintf(stderr, "%s", wolang_vm_errors(vm));
    wolang_vm_destroy(vm);
    return nullptr;
  }
  return vm;
}

// Microseconds a frame takes, averaged over |frames|.
template <typename Body>
static double Measure(int32_t frames, Body body) {
  body();

  const auto start = std::chrono::steady_clock::now();
  for (int32_t frame = 0; frame < frames; ++frame) {
    body();
  }
  const auto took = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::micro>(took).count() / frames;
}

static double ScriptFrame(WolangFunction* tick, std::vector<std::unique_ptr<Actor>>& actors, int32_t frames, bool* failed) {
  return Measure(frames, [&]() {
    WolangValue arguments[2] = {wolang_void(), wolang_float(1.0 / 60.0)};
    WolangValue answer = wolang_void();
    for (const std::unique_ptr<Actor>& actor : actors) {
      arguments[0] = wolang_handle(actor.get());
      if (wolang_call(tick, arguments, 2, &answer) != 0) {
        *failed = true;
      }
    }
  });
}

static bool Agree(const std::vector<std::unique_ptr<Actor>>& left, const std::vector<std::unique_ptr<Actor>>& right) {
  for (size_t i = 0; i < left.size(); ++i) {
    const Vec3 a = left[i]->GetLocation();
    const Vec3 b = right[i]->GetLocation();
    if (std::fabs(a.x - b.x) > 1e-2f || std::fabs(a.y - b.y) > 1e-2f || std::fabs(a.z - b.z) > 1e-2f || std::fabs(left[i]->GetHealth() - right[i]->GetHealth()) > 1e-2f) {
      return false;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  const std::string script = argc > 1 ? argv[1] : "benchmarks/actors/actors.wo";
  WolangVm* compiled = Load(script, true);
  WolangVm* interpreted = Load(script, false);
  if (compiled == nullptr || interpreted == nullptr) {
    return 1;
  }
  WolangFunction* fast = wolang_vm_function(compiled, "Tick");
  WolangFunction* slow = wolang_vm_function(interpreted, "Tick");

#if defined(WOLANG_ACTORS_AOT)
  actors_start();
#endif
  std::printf("%8s %10s %10s %7s %10s %7s %12s %8s %6s\n", "actors", "c++ us", "aot us", "ratio", "jit us", "ratio", "interp us", "ratio", "agree");
  bool failed = false;
  for (const int32_t count : {100, 500, 1000, 5000}) {
    std::vector<std::unique_ptr<Actor>> native = Spawn(count);
    std::vector<std::unique_ptr<Actor>> jit = Spawn(count);
    std::vector<std::unique_ptr<Actor>> interp = Spawn(count);
    std::vector<std::unique_ptr<Actor>> aot = Spawn(count);
    const int32_t frames = 200;

    const double native_us = Measure(frames, [&]() {
      for (const std::unique_ptr<Actor>& actor : native) {
        NativeTick(actor.get(), 1.0f / 60.0f);
      }
    });
    double aot_us = 0.0;
#if defined(WOLANG_ACTORS_AOT)
    aot_us = Measure(frames, [&]() {
      for (const std::unique_ptr<Actor>& actor : aot) {
        actors_Tick(actor.get(), 1.0f / 60.0f);
      }
    });
#endif
    const double jit_us = ScriptFrame(fast, jit, frames, &failed);
    const double interp_us = ScriptFrame(slow, interp, count >= 1000 ? 4 : 20, &failed);
    const bool agree = Agree(native, jit) && (aot_us == 0.0 || Agree(native, aot));

    std::printf("%8d %10.1f %10.1f %6.1fx %10.1f %6.1fx %12.1f %7.1fx %6s\n", count, native_us, aot_us, aot_us / native_us, jit_us, jit_us / native_us, interp_us, interp_us / native_us, agree ? "yes" : "NO");
  }

  wolang_vm_destroy(compiled);
  wolang_vm_destroy(interpreted);
  if (failed) {
    std::fprintf(stderr, "a script call failed\n");
    return 1;
  }
  return 0;
}
