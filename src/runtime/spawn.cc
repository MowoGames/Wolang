// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Starting another program and reading what it said.
//
// `system()` is what `std.process.Run` had: it answers an exit code and
// throws the output away, which is enough for `wolang build` and nothing
// else. A tool needs the output, the error stream and the code, so this
// is `fork` and `exec` with pipes -- and no shell, because a command
// built out of a string is where the oldest hole in computing lives
// (D153).

#include "wolang/runtime/spawn.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wolang {
namespace runtime {
namespace {

// The words of a command, split on blanks, with quoted runs kept whole.
//
// Not a shell: no expansion, no globs, no pipelines. What it is for is
// `wolang run` handing `git -C /some/path rev-parse HEAD` to the system
// without a shell in between.
std::vector<std::string> Words(const char* command, uint64_t length) {
  std::vector<std::string> words;
  std::string current;
  bool quoted = false;
  char quote = 0;
  for (uint64_t i = 0; i < length; ++i) {
    const char c = command[i];
    if (quoted) {
      if (c == quote) {
        quoted = false;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quoted = true;
      quote = c;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    words.push_back(current);
  }
  return words;
}

// Everything a descriptor has to give, until it closes.
std::string Drain(int fd) {
  std::string held;
  char buffer[4096];
  for (;;) {
    const ssize_t received = ::read(fd, buffer, sizeof(buffer));
    if (received <= 0) {
      break;
    }
    held.append(buffer, static_cast<size_t>(received));
  }
  return held;
}

}  // namespace

extern "C" int32_t wolang_spawn(const uint8_t* command,
                                uint64_t length,
                                const uint8_t* input,
                                uint64_t input_length,
                                uint8_t* out,
                                uint64_t out_room,
                                uint64_t* out_written,
                                uint8_t* errors,
                                uint64_t errors_room,
                                uint64_t* errors_written) {
  if (command == nullptr || length == 0) {
    return -1;
  }
  const std::vector<std::string> words =
      Words(reinterpret_cast<const char*>(command), length);
  if (words.empty()) {
    return -1;
  }
  std::vector<char*> argv;
  argv.reserve(words.size() + 1);
  for (const std::string& word : words) {
    argv.push_back(const_cast<char*>(word.c_str()));
  }
  argv.push_back(nullptr);

  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  int from_errors[2] = {-1, -1};
  if (::pipe(to_child) != 0) {
    return -1;
  }
  if (::pipe(from_child) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    return -1;
  }
  if (::pipe(from_errors) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return -1;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    // The child: its ends of the pipes become its streams, and then it
    // is the other program.
    ::dup2(to_child[0], 0);
    ::dup2(from_child[1], 1);
    ::dup2(from_errors[1], 2);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    ::close(from_errors[0]);
    ::close(from_errors[1]);
    ::execvp(argv[0], argv.data());
    // Only reached when the program is not there, and the parent reads
    // the code rather than the message.
    ::_exit(127);
  }

  ::close(to_child[0]);
  ::close(from_child[1]);
  ::close(from_errors[1]);
  if (input != nullptr && input_length != 0) {
    uint64_t at = 0;
    while (at < input_length) {
      const ssize_t written =
          ::write(to_child[1], input + at,
                  static_cast<size_t>(input_length - at));
      if (written <= 0) {
        break;
      }
      at += static_cast<uint64_t>(written);
    }
  }
  // Closed before reading, or a child waiting for more input and a
  // parent waiting for output wait for each other.
  ::close(to_child[1]);

  const std::string said = Drain(from_child[0]);
  const std::string complained = Drain(from_errors[0]);
  ::close(from_child[0]);
  ::close(from_errors[0]);

  int status = 0;
  ::waitpid(child, &status, 0);

  const auto copy = [](const std::string& from, uint8_t* into,
                       uint64_t room, uint64_t* written) {
    if (into == nullptr || written == nullptr) {
      return;
    }
    const uint64_t size =
        from.size() < room ? static_cast<uint64_t>(from.size()) : room;
    std::memcpy(into, from.data(), static_cast<size_t>(size));
    *written = size;
  };
  copy(said, out, out_room, out_written);
  copy(complained, errors, errors_room, errors_written);

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    // A program the system stopped: negative, so that a caller can tell
    // it from an exit code.
    return -WTERMSIG(status);
  }
  return -1;
}

}  // namespace runtime
}  // namespace wolang
