// An engine-shaped host: C++, a script with state, and a frame loop.
//
// The same API as `host.c` with the ceremony removed -- a vm that closes
// itself, calls that take their arguments as arguments, and a function
// looked up once rather than per frame (D137).
//
//   cmake --install build/debug --prefix /tmp/wolang
//   find_package(Wolang CONFIG REQUIRED) ; Wolang::embed
//
#include <cstdio>
#include <string>

#include "wolang/embed/wolang.hpp"

static float host_noise(float x) { return x * 0.5f; }

int main() {
  wolang::Vm vm;
  vm.Bind("host_noise", &host_noise);
  if (!vm.LoadSource("script.wo", R"(
extern "C" fun host_noise(x: f32) -> f32

offer var speed: f32 = 1.0

/// One frame.
offer fun Update(delta: f32) -> f32 uses unsafe {
  return host_noise(delta) * speed
}

offer fun Name() -> text => "the script"
)")) {
    std::fputs(vm.errors().c_str(), stderr);
    return 1;
  }

  std::printf("version %s, functions:", wolang::Version().c_str());
  for (const std::string& name : vm.Functions()) {
    std::printf(" %s", name.c_str());
  }
  std::printf("\n");

  vm.SetGlobal("speed", 3.0);
  const wolang::Function update = vm.Lookup("Update");
  std::printf("arity %d, doc %s\n", update.arity(),
              std::string(update.documentation()).c_str());
  for (int frame = 0; frame < 3; ++frame) {
    if (const auto answer = update(0.5f)) {
      std::printf("frame %d -> %.3f\n", frame, answer->number());
    }
  }
  if (const auto name = vm.Call("Name")) {
    std::printf("name: %s\n", std::string(name->text()).c_str());
  }
  return 0;
}
