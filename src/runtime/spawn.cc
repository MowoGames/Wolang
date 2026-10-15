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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstddef>
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

#if !defined(_WIN32)
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
#endif  // !_WIN32

// What the two implementations below both end with: the output and the
// error stream copied into the caller's buffers.
void Copy(const std::string& from, uint8_t* into, uint64_t room,
          uint64_t* written) {
  if (into == nullptr || written == nullptr) {
    return;
  }
  const uint64_t size =
      from.size() < room ? static_cast<uint64_t>(from.size()) : room;
  std::memcpy(into, from.data(), static_cast<size_t>(size));
  *written = size;
}

}  // namespace

#if defined(_WIN32)

// Starting another program here is one call that takes a command line
// rather than a list of arguments, and three pipes made by hand. The
// words are joined back together with the quoting this system's own
// parser expects -- which is why they were split in the first place:
// what a caller wrote is the same on both systems (D159).
namespace {

std::string Joined(const std::vector<std::string>& words) {
  std::string line;
  for (const std::string& word : words) {
    if (!line.empty()) {
      line.push_back(' ');
    }
    const bool needs_quotes =
        word.empty() || word.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes) {
      line.append(word);
      continue;
    }
    line.push_back('"');
    for (size_t i = 0; i < word.size(); ++i) {
      if (word[i] == '"') {
        line.push_back('\\');
      }
      line.push_back(word[i]);
    }
    line.push_back('"');
  }
  return line;
}

std::string DrainHandle(HANDLE handle) {
  std::string held;
  char buffer[4096];
  DWORD received = 0;
  while (::ReadFile(handle, buffer, sizeof(buffer), &received, nullptr) &&
         received > 0) {
    held.append(buffer, received);
  }
  return held;
}

}  // namespace

extern "C" int32_t wolang_spawn_in(const uint8_t* command,
                                   uint64_t length,
                                   const uint8_t* input,
                                   uint64_t input_length,
                                   const uint8_t* directory,
                                   uint64_t directory_length,
                                   const uint8_t* environment,
                                   uint64_t environment_length,
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
  // This system takes the whole environment or none of it, so the
  // additions are merged with this process's own and handed over as one
  // block (D179).
  std::string block;
  if (environment != nullptr && environment_length != 0) {
    std::string held(reinterpret_cast<const char*>(environment),
                     static_cast<size_t>(environment_length));
    for (char& c : held) {
      if (c == '\n') {
        c = 0;
      }
    }
    block.assign(held);
    block.push_back(0);
    if (const char* existing = ::GetEnvironmentStringsA();
        existing != nullptr) {
      const char* walk = existing;
      while (*walk != 0) {
        const size_t size = std::strlen(walk);
        block.append(walk, size);
        block.push_back(0);
        walk += size + 1;
      }
      ::FreeEnvironmentStringsA(const_cast<char*>(existing));
    }
    block.push_back(0);
  }
  const std::string where =
      directory != nullptr && directory_length != 0
          ? std::string(reinterpret_cast<const char*>(directory),
                        static_cast<size_t>(directory_length))
          : std::string();
  std::string line = Joined(words);

  SECURITY_ATTRIBUTES inherit = {};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;

  HANDLE to_child_read = nullptr;
  HANDLE to_child_write = nullptr;
  HANDLE from_child_read = nullptr;
  HANDLE from_child_write = nullptr;
  HANDLE from_errors_read = nullptr;
  HANDLE from_errors_write = nullptr;
  if (!::CreatePipe(&to_child_read, &to_child_write, &inherit, 0) ||
      !::CreatePipe(&from_child_read, &from_child_write, &inherit, 0) ||
      !::CreatePipe(&from_errors_read, &from_errors_write, &inherit,
                    0)) {
    return -1;
  }
  // The ends this process keeps must not travel to the child, or the
  // read below never sees the pipe close.
  ::SetHandleInformation(to_child_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(from_child_read, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(from_errors_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA start = {};
  start.cb = sizeof(start);
  start.dwFlags = STARTF_USESTDHANDLES;
  start.hStdInput = to_child_read;
  start.hStdOutput = from_child_write;
  start.hStdError = from_errors_write;
  PROCESS_INFORMATION made = {};
  const BOOL started = ::CreateProcessA(
      nullptr, line.data(), nullptr, nullptr, TRUE, 0,
      block.empty() ? nullptr : block.data(),
      where.empty() ? nullptr : where.c_str(), &start, &made);
  ::CloseHandle(to_child_read);
  ::CloseHandle(from_child_write);
  ::CloseHandle(from_errors_write);
  if (!started) {
    ::CloseHandle(to_child_write);
    ::CloseHandle(from_child_read);
    ::CloseHandle(from_errors_read);
    // The same number this family of calls answers everywhere when the
    // program is not there.
    return 127;
  }

  if (input != nullptr && input_length != 0) {
    DWORD written = 0;
    ::WriteFile(to_child_write, input,
                static_cast<DWORD>(input_length), &written, nullptr);
  }
  ::CloseHandle(to_child_write);

  const std::string said = DrainHandle(from_child_read);
  const std::string complained = DrainHandle(from_errors_read);
  ::CloseHandle(from_child_read);
  ::CloseHandle(from_errors_read);

  ::WaitForSingleObject(made.hProcess, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(made.hProcess, &code);
  ::CloseHandle(made.hProcess);
  ::CloseHandle(made.hThread);

  Copy(said, out, out_room, out_written);
  Copy(complained, errors, errors_room, errors_written);
  return static_cast<int32_t>(code);
}

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
  return wolang_spawn_in(command, length, input, input_length, nullptr,
                         0, nullptr, 0, out, out_room, out_written,
                         errors, errors_room, errors_written);
}

#else

extern "C" int32_t wolang_spawn_in(const uint8_t* command,
                                   uint64_t length,
                                   const uint8_t* input,
                                   uint64_t input_length,
                                   const uint8_t* directory,
                                   uint64_t directory_length,
                                   const uint8_t* environment,
                                   uint64_t environment_length,
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
  // `NAME=value` lines, which is what the child's environment is made
  // of: added to what this process has rather than replacing it, because
  // a child with no `PATH` cannot find anything (D179).
  std::vector<std::string> added;
  if (environment != nullptr && environment_length != 0) {
    std::string held(reinterpret_cast<const char*>(environment),
                     static_cast<size_t>(environment_length));
    size_t start = 0;
    while (start <= held.size()) {
      const size_t end = held.find('\n', start);
      const std::string line = held.substr(
          start, end == std::string::npos ? std::string::npos
                                          : end - start);
      if (!line.empty()) {
        added.push_back(line);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }
  const std::string where =
      directory != nullptr && directory_length != 0
          ? std::string(reinterpret_cast<const char*>(directory),
                        static_cast<size_t>(directory_length))
          : std::string();
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
    if (!where.empty()) {
      if (::chdir(where.c_str()) != 0) {
        ::_exit(127);
      }
    }
    for (const std::string& line : added) {
      const size_t equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      ::setenv(line.substr(0, equals).c_str(),
               line.substr(equals + 1).c_str(), 1);
    }
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

  Copy(said, out, out_room, out_written);
  Copy(complained, errors, errors_room, errors_written);

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
  return wolang_spawn_in(command, length, input, input_length, nullptr,
                         0, nullptr, 0, out, out_room, out_written,
                         errors, errors_room, errors_written);
}

#endif  // the system

}  // namespace runtime
}  // namespace wolang
