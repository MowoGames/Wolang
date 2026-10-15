// A C program with Wolang inside it.
//
// Build it against `libwolang_embed`, hand it a package, call a function,
// and let the script call back into the host. Which is the whole of what
// "embeddable" has to mean.
//
//   cc -I include examples/embed/host.c \
//      -L build/debug/src -lwolang_embed -lc++ -o host
//   ./host examples/embed/script

#include <stdio.h>
#include <string.h>

#include "wolang/embed/wolang.h"

// Something for the script to call. The script declares it as
//   extern "C" fun host_log(message: *u8, length: u64)
// which is the same contract a C prototype is.
static void host_log(const char* message, unsigned long long length) {
  printf("host: %.*s\n", (int)length, message);
}

static int host_add(int a, int b) {
  return a + b;
}

int main(int argc, char** argv) {
  const char* package = argc > 1 ? argv[1] : "examples/embed/script";

  WolangVm* vm = wolang_vm_create();

  // Bound before the program is loaded, because the machine-code tier
  // resolves a symbol once -- when it compiles the call.
  wolang_vm_bind(vm, "host_log", (void*)&host_log);
  wolang_vm_bind(vm, "host_add", (void*)&host_add);

  if (wolang_vm_load(vm, package) != 0) {
    fprintf(stderr, "%s", wolang_vm_errors(vm));
    wolang_vm_destroy(vm);
    return 1;
  }

  printf("loaded %d functions:", wolang_vm_function_count(vm));
  for (int i = 0; i < wolang_vm_function_count(vm); ++i) {
    printf(" %s", wolang_vm_function_name(vm, i));
  }
  printf("\n");

  // An integer in, an integer out.
  WolangValue arguments[2];
  arguments[0] = wolang_int(6);
  arguments[1] = wolang_int(7);
  WolangValue answer;
  if (wolang_vm_call(vm, "Multiply", arguments, 2, &answer) != 0) {
    fprintf(stderr, "%s", wolang_vm_errors(vm));
    wolang_vm_destroy(vm);
    return 1;
  }
  printf("Multiply(6, 7) = %lld\n", (long long)answer.integer);

  // A string in and a string out, which is the shape most host calls
  // actually have.
  const char* name = "mowo";
  arguments[0] = wolang_text(name, strlen(name));
  if (wolang_vm_call(vm, "Greet", arguments, 1, &answer) == 0) {
    printf("Greet(\"%s\") = %.*s\n", name, (int)answer.length, answer.text);
  } else {
    fprintf(stderr, "%s", wolang_vm_errors(vm));
  }

  // And a script that calls back into the host.
  if (wolang_vm_call(vm, "Report", 0, 0, &answer) != 0) {
    fprintf(stderr, "%s", wolang_vm_errors(vm));
  }

  wolang_vm_destroy(vm);
  return 0;
}
